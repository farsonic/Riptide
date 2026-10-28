/*
 * Riptide - TCP interop with the Linux kernel TCP over TUN (needs root,
 * /dev/net/tun and iproute2; exits 77 = skip otherwise).
 *
 *   Linux 10.78.1.1/30 <-- tun --> Riptide 10.78.1.2
 *   Linux routes 10.79.0.0/24 into netns "rtpmtuN" over a veth with
 *   MTU 1000 (PMTU discovery test: Linux sends ICMP frag-needed).
 *
 * Covers: handshakes both directions, option negotiation checked from
 * both stacks' TCP_INFO (timestamps, SACK, window scaling, ECN), 20 MB
 * transfers both ways with byte verification, half close, RST/refused,
 * MSS clamping with a small link MTU, transfers under netem loss/delay,
 * and PMTU discovery through a Linux router.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "t_tcp_common.h"

#define OUR_IP   0x0a4e0102u   /* 10.78.1.2 */
#define LINUX_IP 0x0a4e0101u   /* 10.78.1.1 */
#define NS_IP    0x0a4f0002u   /* 10.79.0.2 (netns behind a 1000 MTU link) */

static struct rt_sockctx ctx;
static char tun_name[16];
static struct rt_netif *tun;

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

/* ---- Linux side helpers ---- */
static int lx_listen(u32 ip, u16 port)
{
	struct sockaddr_in a;
	int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(ip);
	a.sin_port = htons(port);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int lx_connect(u32 ip, u16 port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(ip);
	a.sin_port = htons(port);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	return fd;
}

static u64 lx_send_pattern(int fd, u64 total, u32 seed)
{
	static u8 buf[65536];
	u64 off = 0;

	while (off < total) {
		u32 n = (u32)((total - off) < sizeof(buf) ? (total - off) : sizeof(buf));
		ssize_t r;
		t_fill(buf, n, off, seed);
		r = send(fd, buf, n, MSG_NOSIGNAL);
		if (r <= 0)
			break;
		off += (u64)r;
		if ((u32)r < n)
			continue;
	}
	return off;
}

static u64 lx_recv_pattern(int fd, u32 seed, u64 *bad)
{
	static u8 buf[65536];
	u64 off = 0;

	*bad = 0;
	for (;;) {
		ssize_t r = recv(fd, buf, sizeof(buf), 0), i;
		if (r <= 0)
			break;
		for (i = 0; i < r; i++)
			if (buf[i] != t_pat(off + (u64)i, seed))
				(*bad)++;
		off += (u64)r;
	}
	return off;
}

static void lx_info(int fd, struct tcp_info *ti)
{
	socklen_t l = sizeof(*ti);
	memset(ti, 0, sizeof(*ti));
	getsockopt(fd, IPPROTO_TCP, TCP_INFO, ti, &l);
}

/* ---- Linux peer thread ---- */
struct lx_job {
	int mode;          /* 1 accept+sink, 2 accept+source, 3 connect+sink, 4 connect+source */
	int lfd;
	u32 ip;
	u16 port;
	u64 total;
	u32 seed;
	u64 got, bad;
	struct tcp_info ti;
	int err;
};

static void *lx_thread(void *p)
{
	struct lx_job *j = p;
	int fd;

	if (j->mode <= 2)
		fd = accept(j->lfd, NULL, NULL);
	else
		fd = lx_connect(j->ip, j->port);
	if (fd < 0) {
		j->err = fd;
		return NULL;
	}
	if (j->mode == 1 || j->mode == 3) {
		j->got = lx_recv_pattern(fd, j->seed, &j->bad);
		lx_info(fd, &j->ti);
	} else {
		j->got = lx_send_pattern(fd, j->total, j->seed);
		lx_info(fd, &j->ti);
		shutdown(fd, SHUT_WR);
		{
			char x;
			recv(fd, &x, 1, 0);
		}
	}
	close(fd);
	return NULL;
}

static void print_lx_info(const char *tag, const struct tcp_info *ti)
{
	printf("%s: linux sees opts 0x%x snd_wscale %u rcv_wscale %u snd_mss %u rcv_mss %u "
	       "rtt %uus retrans %u cwnd %u\n", tag, ti->tcpi_options, ti->tcpi_snd_wscale,
	       ti->tcpi_rcv_wscale, ti->tcpi_snd_mss, ti->tcpi_rcv_mss, ti->tcpi_rtt,
	       ti->tcpi_total_retrans, ti->tcpi_snd_cwnd);
}

/* One transfer: dir 0 = Riptide sends, 1 = Linux sends; who = 0 Riptide
 * connects, 1 Linux connects. */
static void transfer(const char *name, int who, int dir, u64 total, u32 dst_ip, u16 port,
                     u32 expect_opts)
{
	struct lx_job j;
	pthread_t th;
	struct rt_tcp_info ti;
	struct rt_tcp_stats before = rt_tcpstat;
	int fd = -1, rfd = -1;
	double t0;
	u64 got = 0, bad = 0;

	memset(&j, 0, sizeof(j));
	j.total = total;
	j.seed = 0x7700 + port;
	j.port = port;
	j.ip = OUR_IP;
	if (who == 0) {
		j.lfd = lx_listen(dst_ip == NS_IP ? 0 : LINUX_IP, port);
		CHECK(j.lfd >= 0);
		j.mode = dir == 0 ? 1 : 2;
	} else {
		struct rt_sockaddr_in a = t_sa(0, port);
		rfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		t_set_int(&ctx, rfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
		CHECK(rt_api_bind(&ctx, rfd, &a, sizeof(a)) == 0);
		CHECK(rt_api_listen(&ctx, rfd, 4) == 0);
		j.mode = dir == 0 ? 3 : 4;
	}
	t0 = t_now();
	pthread_create(&th, NULL, lx_thread, &j);
	if (who == 0) {
		struct rt_sockaddr_in d = t_sa(dst_ip, port);
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == 0);
	} else {
		fd = rt_api_accept(&ctx, rfd, NULL, NULL);
		CHECK(fd >= 0);
	}
	if (dir == 0) {
		got = t_send_pattern(&ctx, fd, total, j.seed, 32768);
		t_get_info(&ctx, fd, &ti);
		rt_api_shutdown(&ctx, fd, 1);
		pthread_join(th, NULL);
		CHECKV(got == total && j.got == total && j.bad == 0, "%s: sent %llu linux got %llu bad %llu",
		       name, (unsigned long long)got, (unsigned long long)j.got, (unsigned long long)j.bad);
	} else {
		got = t_recv_pattern(&ctx, fd, j.seed, &bad, 65536);
		t_get_info(&ctx, fd, &ti);
		rt_api_close(&ctx, fd);
		fd = -1;
		pthread_join(th, NULL);
		CHECKV(got == total && bad == 0, "%s: got %llu bad %llu", name,
		       (unsigned long long)got, (unsigned long long)bad);
	}
	{
		double dt = t_now() - t0;
		printf("TUN %-34s %s %5.1f MB %6.2fs %8.1f Mbit/s riptide retrans %u (fast %u rto %u tlp %u) linux retrans %u\n",
		       name, dir ? "linux->riptide" : "riptide->linux", total / 1048576.0, dt,
		       total * 8 / dt / 1e6, rt_tcpstat.RetransSegs - before.RetransSegs,
		       rt_tcpstat.TCPFastRetrans - before.TCPFastRetrans,
		       rt_tcpstat.TCPTimeouts - before.TCPTimeouts,
		       rt_tcpstat.TCPLossProbes - before.TCPLossProbes, j.ti.tcpi_total_retrans);
	}
	t_print_info("    riptide", &ti);
	print_lx_info("    ", &j.ti);
	if (expect_opts) {
		CHECKV((ti.tcpi_options & expect_opts) == expect_opts, "riptide opts %x", ti.tcpi_options);
		/* qemu-user does not convert the host's struct tcp_info for
		 * a big-endian guest: only check it on native builds */
		if (!RT_BIG_ENDIAN)
		CHECKV((j.ti.tcpi_options & (TCPI_OPT_TIMESTAMPS | TCPI_OPT_SACK | TCPI_OPT_WSCALE)) ==
		       (TCPI_OPT_TIMESTAMPS | TCPI_OPT_SACK | TCPI_OPT_WSCALE), "linux opts %x",
		       j.ti.tcpi_options);
		if (!RT_BIG_ENDIAN)
			CHECK(j.ti.tcpi_snd_wscale == ti.tcpi_rcv_wscale &&
			      j.ti.tcpi_rcv_wscale == ti.tcpi_snd_wscale);
	}
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	if (rfd >= 0)
		rt_api_close(&ctx, rfd);
	if (j.lfd > 0)
		close(j.lfd);
}

static void test_refused_and_reset(void)
{
	struct rt_sockaddr_in d = t_sa(LINUX_IP, 9);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0), r, lfd, afd;
	char buf[8];

	r = rt_api_connect(&ctx, fd, &d, sizeof(d));
	CHECKV(r == -RT_ECONNREFUSED, "r=%d", r);
	rt_api_close(&ctx, fd);
	/* Linux connecting to a Riptide port nobody listens on */
	r = lx_connect(OUR_IP, 9);
	CHECKV(r == -ECONNREFUSED, "linux connect r=%d", r);

	/* Linux closes with unread data -> Riptide sees ECONNRESET */
	lfd = lx_listen(LINUX_IP, 8100);
	d = t_sa(LINUX_IP, 8100);
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == 0);
	afd = accept(lfd, NULL, NULL);
	CHECK(rt_api_send(&ctx, fd, "data", 4, 0) == 4);
	usleep(100000);
	close(afd);
	r = rt_api_recv(&ctx, fd, buf, sizeof(buf), 0);
	CHECKV(r == -RT_ECONNRESET, "r=%d", r);
	rt_api_close(&ctx, fd);

	/* half close with Linux: Riptide shuts down writes, Linux answers */
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == 0);
	afd = accept(lfd, NULL, NULL);
	CHECK(rt_api_send(&ctx, fd, "ping", 4, 0) == 4);
	rt_api_shutdown(&ctx, fd, 1);
	{
		char lb[16];
		ssize_t n = 0, k;
		while ((k = recv(afd, lb + n, sizeof(lb) - (size_t)n, 0)) > 0)
			n += k;
		CHECK(n == 4 && k == 0);
		CHECK(send(afd, "pong", 4, 0) == 4);
		close(afd);
	}
	CHECK(rt_api_recv(&ctx, fd, buf, 4, RT_MSG_WAITALL) == 4 && memcmp(buf, "pong", 4) == 0);
	CHECK(rt_api_recv(&ctx, fd, buf, 4, 0) == 0);
	rt_api_close(&ctx, fd);
	close(lfd);
}

/* PMTU: Linux forwards into a netns over a 1000 byte MTU veth link and
 * answers our DF segments with ICMP fragmentation-needed. */
static int ns_server_pid;
static char ns_name[32];

static int setup_netns(void)
{
	snprintf(ns_name, sizeof(ns_name), "rtpmtu%d", (int)(getpid() % 100000));
	if (sh("ip netns add %s 2>/dev/null", ns_name))
		return -1;
	if (sh("ip link add %sa type veth peer name %sb 2>/dev/null", ns_name, ns_name) ||
	    sh("ip link set %sb netns %s", ns_name, ns_name) ||
	    sh("ip addr add 10.79.0.1/24 dev %sa && ip link set %sa mtu 1000 up", ns_name, ns_name) ||
	    sh("ip netns exec %s sh -c 'ip addr add 10.79.0.2/24 dev %sb && ip link set %sb mtu 1500 up "
	       "&& ip link set lo up && ip route add default via 10.79.0.1'", ns_name, ns_name, ns_name) ||
	    sh("sysctl -qw net.ipv4.ip_forward=1") ||
	    sh("sysctl -qw net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.%s.rp_filter=0", tun_name))
		return -1;
	return 0;
}

static void teardown_netns(void)
{
	sh("ip netns del %s 2>/dev/null", ns_name);
	sh("ip link del %sa 2>/dev/null", ns_name);
}

/* child: enter the netns and run a sink server for one connection */
static int ns_sink(u16 port, u32 seed, u64 total)
{
	char path[64];
	int nfd, lfd, fd;
	u64 bad, got;
	u8 rep[16];

	snprintf(path, sizeof(path), "/var/run/netns/%s", ns_name);
	nfd = open(path, O_RDONLY);
	if (nfd < 0 || setns(nfd, CLONE_NEWNET) < 0)
		return 2;
	lfd = lx_listen(0, port);
	if (lfd < 0)
		return 3;
	fd = accept(lfd, NULL, NULL);
	got = lx_recv_pattern(fd, seed, &bad);
	rt_put_be32(rep, (u32)got);
	rt_put_be32(rep + 4, (u32)bad);
	send(fd, rep, 8, 0);
	close(fd);
	return got == total && bad == 0 ? 0 : 4;
}

static void test_pmtu(void)
{
	struct rt_sockaddr_in d = t_sa(NS_IP, 8200);
	struct rt_tcp_info ti;
	u64 total = 4u << 20, sent;
	u32 icmp0 = rt_icmpstat.InDestUnreachs;
	int fd, st = 0, i;
	pid_t pid;
	u8 rep[8];

	if (setup_netns()) {
		printf("PMTU test skipped (netns/veth setup failed)\n");
		teardown_netns();
		return;
	}
	rt_lock();
	rt_route_add(rt_htonl(0x0a4f0000u), rt_htonl(0xffffff00u), rt_htonl(LINUX_IP), tun,
	             RT_RTF_UP | RT_RTF_GATEWAY | RT_RTF_STATIC, 0);
	rt_unlock();
	pid = fork();
	if (pid == 0)
		_exit(ns_sink(8200, 0x5150, total));
	ns_server_pid = pid;
	fd = -1;
	for (i = 0; i < 50; i++) {
		int r;
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		r = rt_api_connect(&ctx, fd, &d, sizeof(d));
		if (r == 0)
			break;
		rt_api_close(&ctx, fd);
		fd = -1;
		usleep(100000);
	}
	CHECK(fd >= 0);
	if (fd >= 0) {
		t_get_info(&ctx, fd, &ti);
		printf("PMTU: initial snd_mss %u (path MTU 1000 not yet known)\n", ti.tcpi_snd_mss);
		sent = t_send_pattern(&ctx, fd, total, 0x5150, 32768);
		t_get_info(&ctx, fd, &ti);
		rt_api_shutdown(&ctx, fd, 1);
		CHECK(rt_api_recv(&ctx, fd, rep, 8, RT_MSG_WAITALL) == 8);
		CHECK(sent == total && rt_get_be32(rep) == (u32)total && rt_get_be32(rep + 4) == 0);
		printf("PMTU: after transfer snd_mss %u, ICMP unreach received %u, retrans %u\n",
		       ti.tcpi_snd_mss, rt_icmpstat.InDestUnreachs - icmp0, ti.tcpi_total_retrans);
		CHECKV(ti.tcpi_snd_mss <= 1000 - 40 - 12, "mss %u", ti.tcpi_snd_mss);
		CHECK(rt_icmpstat.InDestUnreachs > icmp0);
		rt_api_close(&ctx, fd);
	}
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	teardown_netns();
}

int main(int argc, char **argv)
{
	u64 big = 20u << 20;
	int have_tc;

	if (argc > 1)
		big = strtoull(argv[1], NULL, 0);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_tcp_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(900);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tun_name, sizeof(tun_name), "rtt%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(tun_name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffc));
	if (!tun) {
		printf("t_tcp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	if (sh("ip addr add 10.78.1.1/30 dev %s && ip link set %s up", tun_name, tun_name)) {
		printf("t_tcp_tun: SKIP (cannot configure %s)\n", tun_name);
		return 77;
	}
	rt_ctx_init(&ctx, 32, NULL);

	test_refused_and_reset();
	transfer("riptide connects", 0, 0, big, LINUX_IP, 8001, 7);
	transfer("riptide connects", 0, 1, big, LINUX_IP, 8002, 7);
	transfer("linux connects", 1, 0, big, LINUX_IP, 8003, 7);
	transfer("linux connects", 1, 1, big, LINUX_IP, 8004, 7);
	/* ECN requested by default; Linux accepts it */
	transfer("ecn requested", 0, 0, big / 4, LINUX_IP, 8005, 0xf);

	/* small link MTU on the Linux side: Linux advertises MSS 536 */
	sh("ip link set dev %s mtu 576", tun_name);
	transfer("linux mtu 576", 0, 0, big / 8, LINUX_IP, 8006, 7);
	sh("ip link set dev %s mtu 1500", tun_name);

	have_tc = sh("tc qdisc add dev %s root netem loss 2%% delay 10ms 2>/dev/null", tun_name) == 0;
	if (!have_tc)
		printf("netem not available: impairing the Riptide side only\n");
	{
		struct rt_host_impair im;
		memset(&im, 0, sizeof(im));
		im.loss = 200;
		im.delay_ms = 10;
		im.seed = 31337;
		rt_host_impair(tun, &im);
		transfer("2% loss 10ms (riptide egress)", 0, 0, big / 4, LINUX_IP, 8007, 7);
		transfer("2% loss 10ms (riptide egress)", 0, 1, big / 4, LINUX_IP, 8008, 7);
		im.loss = 100;
		im.reorder = 300;
		im.delay_ms = 2;
		rt_host_impair(tun, &im);
		transfer("1% loss 3% reorder", 1, 0, big / 4, LINUX_IP, 8009, 7);
		memset(&im, 0, sizeof(im));
		rt_host_impair(tun, &im);
		if (have_tc)
			sh("tc qdisc del dev %s root 2>/dev/null", tun_name);
	}
	test_pmtu();
	t_print_stats("riptide");
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

