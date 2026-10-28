/*
 * Riptide - TCP path MTU black holes against the Linux kernel over TUN
 * (needs root, /dev/net/tun, iproute2, iptables and ip6tables; exits 77
 * otherwise):
 *
 *   Riptide 10.91.4.2 --tun-- router 10.91.4.1 | 10.91.5.1 --veth mtu 1400-- 10.91.5.2 Linux
 *           fd00:91:4::2            ns "rtmpr"   fd00:91:5::1                fd00:91:5::2 ns "rtmps"
 *
 * The router forwards our 1500 byte packets into a 1400 byte link and
 * answers with ICMP "fragmentation needed" / "packet too big"; iptables
 * in the router drops that ICMP (a PMTU black hole) or, for the length
 * variant, every forwarded packet over 1400 bytes. Uploads to and (with a
 * Linux client) downloads from a Riptide listener must complete with
 * net.inet.tcp.mtu_probing 1 and stall with 0, over IPv4 and IPv6.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include "t_tcp_common.h"

#define OUR_IP    0x0a5b0402u   /* 10.91.4.2 */
#define ROUTER_IP 0x0a5b0401u   /* 10.91.4.1 */
#define SRV_IP    0x0a5b0502u   /* 10.91.5.2 */
#define OUR_IP6   "fd00:91:4::2"
#define ROUTER_IP6 "fd00:91:4::1"
#define SRV_IP6   "fd00:91:5::2"
#define SINK_PORT 9001
#define SRC_PORT  9002
#define TOTAL     (1u << 20)

static struct rt_sockctx ctx;
static char tun_name[16], nsr[24], nss[24], veth_r[16], veth_s[16];
static struct rt_netif *tun;
static int have_v6;

static int sh(const char *fmt, ...)
{
	char cmd[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static void enter_ns(const char *ns)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/var/run/netns/%s", ns);
	fd = open(path, O_RDONLY);
	if (fd < 0 || setns(fd, CLONE_NEWNET) < 0) {
		perror("setns");
		exit(2);
	}
	close(fd);
}

/* ---- Linux sink (in the server namespace): counts bytes until EOF ---- */
static volatile int sink_ready, sink_stop;
static volatile u64 sink_last;

static void *sink_thread(void *p)
{
	struct sockaddr_in6 a6;
	struct sockaddr_in a4;
	int one = 1, zero = 0, lfd, rc;

	(void)p;
	enter_ns(nss);
	lfd = socket(have_v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (have_v6) {      /* both families */
		memset(&a6, 0, sizeof(a6));
		a6.sin6_family = AF_INET6;
		a6.sin6_port = htons(SINK_PORT);
		setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
		rc = bind(lfd, (struct sockaddr *)&a6, sizeof(a6));
	} else {
		memset(&a4, 0, sizeof(a4));
		a4.sin_family = AF_INET;
		a4.sin_port = htons(SINK_PORT);
		rc = bind(lfd, (struct sockaddr *)&a4, sizeof(a4));
	}
	if (rc < 0 || listen(lfd, 8) < 0) {
		sink_ready = -1;
		return NULL;
	}
	sink_ready = 1;
	while (!sink_stop) {
		struct timeval tv = { 0, 200000 };
		fd_set r;
		char buf[65536];
		u64 got = 0;
		ssize_t k;
		int fd;
		FD_ZERO(&r);
		FD_SET(lfd, &r);
		if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0)
			continue;
		fd = accept(lfd, NULL, NULL);
		if (fd < 0)
			continue;
		tv.tv_sec = 12;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		while ((k = recv(fd, buf, sizeof(buf), 0)) > 0)
			got += (u64)k;
		sink_last = got;
		if (k == 0)
			send(fd, "k", 1, 0);
		close(fd);
	}
	close(lfd);
	return NULL;
}

/* ---- Linux client (server namespace) downloading from a Riptide listener ---- */
struct dl {
	int v6;
	u64 got;
	double secs;
};

static void *dl_thread(void *p)
{
	struct dl *d = p;
	struct sockaddr_storage ss;
	socklen_t sl;
	char buf[65536];
	struct timeval tv = { 30, 0 };
	ssize_t k;
	double t0;
	int fd;

	enter_ns(nss);
	memset(&ss, 0, sizeof(ss));
	if (d->v6) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(SRC_PORT);
		inet_pton(AF_INET6, OUR_IP6, &a->sin6_addr);
		sl = sizeof(*a);
	} else {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;
		a->sin_family = AF_INET;
		a->sin_port = htons(SRC_PORT);
		a->sin_addr.s_addr = htonl(OUR_IP);
		sl = sizeof(*a);
	}
	fd = socket(d->v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	t0 = t_now();
	if (connect(fd, (struct sockaddr *)&ss, sl) == 0)
		while ((k = recv(fd, buf, sizeof(buf), 0)) > 0)
			d->got += (u64)k;
	d->secs = t_now() - t0;
	close(fd);
	return NULL;
}

/* ---- the cases ---- */
static int rt_connect_srv(int v6)
{
	int fd, r;

	if (v6) {
		struct rt_sockaddr_in6 s;
		memset(&s, 0, sizeof(s));
		s.sin6_len = sizeof(s);
		s.sin6_family = RT_AF_INET6;
		s.sin6_port = rt_htons(SINK_PORT);
		rt_inet6_pton(SRV_IP6, &s.sin6_addr);
		fd = rt_api_socket(&ctx, RT_AF_INET6, RT_SOCK_STREAM, 0);
		r = rt_api_connect(&ctx, fd, &s, sizeof(s));
	} else {
		struct rt_sockaddr_in s = t_sa(SRV_IP, SINK_PORT);
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		r = rt_api_connect(&ctx, fd, &s, sizeof(s));
	}
	if (r) {
		rt_api_close(&ctx, fd);
		return -1;
	}
	return fd;
}

/* forget black holes (TCP) and IPv4 path MTUs learnt from ICMP */
static void flush_state(u32 probing)
{
	t_sysctl("net.inet.tcp.mtu_probing", (s32)probing);
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.mtu_cache", NULL, NULL, "clear", 5);
	rt_route_flush_dynamic_if(tun);     /* IPv4 path MTUs learnt from ICMP */
	rt_unlock();
}

/* upload TOTAL bytes to the Linux sink; returns seconds or -1 on a stall */
static double upload(const char *name, int v6, u32 user_to, u32 *mss_out)
{
	struct rt_tcp_info ti;
	u32 bh0 = rt_tcpstat.TCPMTUPBlackhole, ok0 = rt_tcpstat.TCPMTUPSuccess,
	    fail0 = rt_tcpstat.TCPMTUPFail, rto0 = rt_tcpstat.TCPTimeouts;
	double t0 = t_now(), dt;
	u64 sent;
	char x;
	int fd = rt_connect_srv(v6), ok = 0;

	CHECK(fd >= 0);
	if (fd < 0)
		return -1;
	if (user_to)
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_USER_TIMEOUT, (int)user_to);
	sink_last = ~0ull;
	sent = t_send_pattern(&ctx, fd, TOTAL, 1, 32768);
	t_get_info(&ctx, fd, &ti);
	if (sent == TOTAL) {
		rt_api_shutdown(&ctx, fd, 1);
		ok = rt_api_recv(&ctx, fd, &x, 1, 0) == 1;
	}
	dt = t_now() - t0;
	rt_api_close(&ctx, fd);
	for (fd = 0; fd < 150 && sink_last == ~0ull; fd++)
		usleep(100000);
	printf("RUN %-36s %s %.2fs %s linux got %llu, mss %u, blackhole %u probes ok %u lost %u rto %u\n",
	       name, v6 ? "v6" : "v4", dt, ok ? "done" : "STALLED", (unsigned long long)sink_last,
	       ti.tcpi_snd_mss, rt_tcpstat.TCPMTUPBlackhole - bh0, rt_tcpstat.TCPMTUPSuccess - ok0,
	       rt_tcpstat.TCPMTUPFail - fail0, rt_tcpstat.TCPTimeouts - rto0);
	if (mss_out)
		*mss_out = ti.tcpi_snd_mss;
	return ok && sink_last == TOTAL ? dt : -1;
}

/* a Linux client downloads TOTAL bytes from a Riptide listener */
static double download(const char *name, int v6, u32 *mss_out)
{
	struct dl d;
	pthread_t th;
	struct rt_tcp_info ti;
	u32 bh0 = rt_tcpstat.TCPMTUPBlackhole;
	int lfd, fd;

	memset(&d, 0, sizeof(d));
	d.v6 = v6;
	if (v6) {
		struct rt_sockaddr_in6 s;
		memset(&s, 0, sizeof(s));
		s.sin6_len = sizeof(s);
		s.sin6_family = RT_AF_INET6;
		s.sin6_port = rt_htons(SRC_PORT);
		lfd = rt_api_socket(&ctx, RT_AF_INET6, RT_SOCK_STREAM, 0);
		t_set_int(&ctx, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
		CHECK(rt_api_bind(&ctx, lfd, &s, sizeof(s)) == 0);
	} else {
		struct rt_sockaddr_in s = t_sa(0, SRC_PORT);
		lfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		t_set_int(&ctx, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
		CHECK(rt_api_bind(&ctx, lfd, &s, sizeof(s)) == 0);
	}
	CHECK(rt_api_listen(&ctx, lfd, 2) == 0);
	pthread_create(&th, NULL, dl_thread, &d);
	fd = rt_api_accept(&ctx, lfd, NULL, NULL);
	CHECK(fd >= 0);
	if (fd >= 0) {
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_USER_TIMEOUT, 20000);
		t_send_pattern(&ctx, fd, TOTAL, 2, 32768);
		t_get_info(&ctx, fd, &ti);
		rt_api_shutdown(&ctx, fd, 1);
	}
	pthread_join(th, NULL);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	rt_api_close(&ctx, lfd);
	printf("RUN %-36s %s %.2fs linux got %llu, mss %u, blackhole %u\n", name, v6 ? "v6" : "v4", d.secs,
	       (unsigned long long)d.got, ti.tcpi_snd_mss, rt_tcpstat.TCPMTUPBlackhole - bh0);
	if (mss_out)
		*mss_out = ti.tcpi_snd_mss;
	return d.got == TOTAL ? d.secs : -1;
}

static int setup(void)
{
	rt_in6_addr a, gw, any;
	int i, ready = 0;

	snprintf(tun_name, sizeof(tun_name), "rtmp%d", (int)(getpid() % 100000));
	snprintf(nsr, sizeof(nsr), "rtmpr%d", (int)(getpid() % 100000));
	snprintf(nss, sizeof(nss), "rtmps%d", (int)(getpid() % 100000));
	snprintf(veth_r, sizeof(veth_r), "rtmvr%d", (int)(getpid() % 100000));
	snprintf(veth_s, sizeof(veth_s), "rtmvs%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(tun_name, rt_htonl(OUR_IP), rt_htonl(ROUTER_IP), rt_htonl(0xfffffffc));
	if (!tun)
		return -1;
	if (sh("ip netns add %s && ip netns add %s", nsr, nss) ||
	    sh("ip link set %s netns %s", tun_name, nsr) ||
	    sh("ip link add %s type veth peer name %s netns %s", veth_r, veth_s, nss) ||
	    sh("ip link set %s netns %s", veth_r, nsr) ||
	    sh("ip netns exec %s sh -c 'ip link set lo up && ip link set %s up && ip link set %s up mtu 1400 && "
	       "ip addr add 10.91.4.1/30 dev %s && ip addr add 10.91.5.1/24 dev %s && "
	       "sysctl -qw net.ipv4.ip_forward=1 net.ipv4.icmp_ratelimit=0'",
	       nsr, tun_name, veth_r, tun_name, veth_r) ||
	    sh("ip netns exec %s sh -c 'ip link set lo up && ip link set %s up && "
	       "ip addr add 10.91.5.2/24 dev %s && ip route add default via 10.91.5.1'",
	       nss, veth_s, veth_s))
		return -1;
	if (sh("ip netns exec %s iptables -L -n >/dev/null", nsr))
		return -2;
	rt_lock();
	rt_route_add(0, 0, rt_htonl(ROUTER_IP), tun, RT_RTF_STATIC, 0);
	rt_unlock();
	/* IPv6 only where the host kernel has it */
	have_v6 = access("/proc/sys/net/ipv6", F_OK) == 0 &&
	          !sh("ip netns exec %s sh -c 'ip -6 addr add fd00:91:4::1/64 dev %s nodad && "
	              "ip -6 addr add fd00:91:5::1/64 dev %s nodad && "
	              "sysctl -qw net.ipv6.conf.all.forwarding=1 net.ipv6.icmp.ratelimit=0 && "
	              "ip6tables -L -n >/dev/null'", nsr, tun_name, veth_r) &&
	          !sh("ip netns exec %s sh -c 'ip -6 addr add fd00:91:5::2/64 dev %s nodad && "
	              "ip -6 route add default via fd00:91:5::1'", nss, veth_s);
	if (!have_v6)
		return 0;
	rt_inet6_pton(OUR_IP6, &a);
	rt_inet6_pton(ROUTER_IP6, &gw);
	memset(&any, 0, sizeof(any));
	rt_lock();
	rt_in6_addr_add(tun, &a, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE);
	rt_route6_add(&any, 0, &gw, tun, 0, 0);
	rt_unlock();
	for (i = 0; i < 50 && !ready; i++) {
		struct rt_in6_ifaddr_info ai[8];
		int n, k;
		usleep(100000);
		rt_lock();
		n = rt_in6_addr_list(tun, ai, 8);
		for (k = 0; k < n && k < 8; k++)
			if (rt_in6_eq(&ai[k].addr, &a) && !(ai[k].flags & RT_IN6_IFF_TENTATIVE))
				ready = 1;
		rt_unlock();
	}
	return ready ? 0 : -1;
}

/* a command in the router namespace */
static int router(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return sh("ip netns exec %s sh -c '%s' >/dev/null 2>&1", nsr, cmd);
}

int main(int argc, char **argv)
{
	pthread_t sink;
	double base, t;
	u32 mss;
	int v6, r;

	(void)argc;
	(void)argv;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0 || system("iptables -V >/dev/null 2>&1") != 0) {
		printf("t_mtup_tun: SKIP (needs root, /dev/net/tun, iproute2 and iptables)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(600);
	if (rt_host_start() != 0)
		return 1;
	r = setup();
	if (r) {
		printf("t_mtup_tun: SKIP (cannot set up the namespaces%s)\n", r == -2 ? ": iptables" : "");
		sh("ip netns del %s 2>/dev/null; ip netns del %s 2>/dev/null", nsr, nss);
		return 77;
	}
	rt_ctx_init(&ctx, 64, NULL);
	pthread_create(&sink, NULL, sink_thread, NULL);
	while (!sink_ready)
		usleep(10000);
	CHECK(sink_ready == 1);
	if (!have_v6)
		printf("(no IPv6 in this kernel: IPv4 only; t_mtup_wire covers IPv6)\n");
	for (v6 = 0; v6 <= have_v6; v6++) {
		u32 fit = 1400 - (v6 ? 40 : 20) - 32, bh0;
		double hole;

		/* the router's ICMP is dropped (first: nothing learnt via ICMP yet) */
		CHECK(router(v6 ? "ip6tables -A OUTPUT -p icmpv6 --icmpv6-type packet-too-big -j DROP" :
		             "iptables -A OUTPUT -p icmp --icmp-type fragmentation-needed -j DROP") == 0);
		flush_state(1);
		hole = upload("ICMP dropped, mtu_probing 1", v6, 0, &mss);
		CHECK(hole > 0 && mss <= fit);
		flush_state(1);
		t = download("ICMP dropped, Linux downloads", v6, &mss);
		CHECK(t > 0 && mss <= fit);
		flush_state(0);
		t = upload("ICMP dropped, mtu_probing 0", v6, 6000, NULL);
		CHECK(t < 0);
		CHECK(router(v6 ? "ip6tables -F OUTPUT" : "iptables -F OUTPUT") == 0);

		/* a plain length filter (no smaller link MTU involved) */
		CHECK(router("ip link set %s mtu 1500 && %s -A FORWARD -m length --length 1401:65535 -j DROP",
		             veth_r, v6 ? "ip6tables" : "iptables") == 0);
		flush_state(1);
		t = upload("length > 1400 dropped, mtu_probing 1", v6, 0, &mss);
		CHECK(t > 0 && mss <= fit);
		CHECK(router("%s -F FORWARD && ip link set %s mtu 1400", v6 ? "ip6tables" : "iptables", veth_r) == 0);

		/* ICMP gets through: classic PMTUD, no black hole (last: the
		 * path MTU learnt stays cached) */
		flush_state(1);
		bh0 = rt_tcpstat.TCPMTUPBlackhole;
		base = upload("router ICMP passes", v6, 0, &mss);
		CHECK(base > 0 && mss <= fit && rt_tcpstat.TCPMTUPBlackhole == bh0);
		if (hole > 0 && base > 0)
			printf("    black hole recovery: %.2fs over the transfer with ICMP\n", hole - base);
	}
	printf("netstat -s: %u MTU probes acknowledged, %u lost, %u black hole reductions\n",
	       rt_tcpstat.TCPMTUPSuccess, rt_tcpstat.TCPMTUPFail, rt_tcpstat.TCPMTUPBlackhole);
	CHECK(rt_tcpstat.TCPMTUPSuccess > 0 && rt_tcpstat.TCPMTUPBlackhole >= 3u * (have_v6 + 1));

	flush_state(1);
	sink_stop = 1;
	pthread_join(sink, NULL);
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	sh("ip netns del %s 2>/dev/null; ip netns del %s 2>/dev/null", nsr, nss);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
