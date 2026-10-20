/*
 * Riptide - TCP between two stack instances (two processes) over the
 * simulated Ethernet wire with impairment: bulk transfers in both
 * directions with byte-exact verification under loss, reordering,
 * duplication, delay and rate limits; NewReno / non-RACK variants;
 * keepalive and TCP_USER_TIMEOUT expiry. No privileges needed.
 *
 * Usage: t_tcp_wire [scale]   (scale divides the transfer sizes; the
 * qemu-m68k run uses a larger scale)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <signal.h>
#include <sys/wait.h>
#include "t_tcp_common.h"

#define A_IP 0xc0a85801u     /* 192.168.88.1 */
#define B_IP 0xc0a85802u     /* 192.168.88.2 */
#define PORT 7000

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 1, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 1, 0x0b };

/* command header A -> B (all big-endian u32) */
enum { CMD_QUIT = 0, CMD_SINK = 1, CMD_SOURCE = 2, CMD_HOLD = 3, CMD_CONFIG = 4, CMD_STATS = 5 };
struct cmd {
	u32 mode, total_lo, total_hi, seed;
	u32 loss, reorder, dup, delay, rate, impair_seed;
	u32 sack, cc_reno, recovery;
};
#define CMD_WORDS 13

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, u32 ip)
{
	struct rt_netif *n;

	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	rt_lock();
	rt_netif_set_addr(n, rt_htonl(ip), rt_htonl(0xffffff00u), 0);
	rt_netif_up(n);
	rt_unlock();
	return n;
}

static void apply_impair(struct rt_netif *n, const struct cmd *c)
{
	struct rt_host_impair im;

	memset(&im, 0, sizeof(im));
	im.loss = c->loss;
	im.reorder = c->reorder;
	im.reorder_ms = 10;
	im.duplicate = c->dup;
	im.delay_ms = c->delay;
	im.rate_kbit = c->rate;
	im.queue_limit = c->rate ? 200 : 2000;
	im.seed = c->impair_seed;
	rt_host_impair(n, &im);
}

static void apply_tunables(const struct cmd *c)
{
	char cc[16];

	t_sysctl("net.inet.tcp.sack", (s32)c->sack);
	t_sysctl("net.inet.tcp.recovery", (s32)c->recovery);
	strcpy(cc, c->cc_reno ? "reno" : "cubic");
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.cc", NULL, NULL, cc, (u32)strlen(cc));
	rt_unlock();
}

static int recv_all(struct rt_sockctx *c, int fd, void *buf, u32 len)
{
	u32 got = 0;
	while (got < len) {
		int r = rt_api_recv(c, fd, (u8 *)buf + got, len - got, 0);
		if (r <= 0)
			return -1;
		got += (u32)r;
	}
	return 0;
}

/* ---- process B: command server ---- */
static int run_b(const char *pa, const char *pb)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = t_sa(0, PORT);
	struct rt_netif *nif;
	int lfd;

	alarm(1800);
	nif = bring_up(pb, pa, mac_b, B_IP);
	if (!nif)
		return 2;
	rt_ctx_init(&c, 16, NULL);
	lfd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&c, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	if (rt_api_bind(&c, lfd, &a, sizeof(a)) || rt_api_listen(&c, lfd, 4))
		return 3;
	for (;;) {
		u32 w[CMD_WORDS], i;
		struct cmd cm;
		u8 reply[16];
		int fd = rt_api_accept(&c, lfd, NULL, NULL);

		if (fd < 0)
			continue;
		if (recv_all(&c, fd, w, sizeof(w))) {
			rt_api_close(&c, fd);
			continue;
		}
		for (i = 0; i < CMD_WORDS; i++)
			w[i] = rt_ntohl(w[i]);
		memcpy(&cm, w, sizeof(cm));
		if (cm.mode == CMD_QUIT) {
			rt_api_close(&c, fd);
			break;
		}
		if (cm.mode == CMD_CONFIG) {
			/* sysctls for the next connection's handshake */
			apply_tunables(&cm);
			rt_api_close(&c, fd);
			continue;
		}
		if (cm.mode == CMD_STATS) {
			rt_api_send(&c, fd, &rt_tcpstat, sizeof(rt_tcpstat), 0);
			rt_api_close(&c, fd);
			continue;
		}
		apply_impair(nif, &cm);
		if (cm.mode == CMD_SINK) {
			u64 bad, got = t_recv_pattern(&c, fd, cm.seed, &bad, 65536);
			rt_put_be32(reply, (u32)(got >> 32));
			rt_put_be32(reply + 4, (u32)got);
			rt_put_be32(reply + 8, (u32)bad);
			rt_put_be32(reply + 12, rt_tcpstat.RetransSegs);
			rt_api_send(&c, fd, reply, 16, 0);
		} else if (cm.mode == CMD_SOURCE) {
			u64 total = ((u64)cm.total_hi << 32) | cm.total_lo;
			t_send_pattern(&c, fd, total, cm.seed, 32768);
			rt_api_shutdown(&c, fd, 1);
			rt_api_recv(&c, fd, reply, 1, 0);
		} else if (cm.mode == CMD_HOLD) {
			/* keep the connection open, never read */
			rt_api_recv(&c, fd, reply, 0, 0);
			sleep(30);
		}
		rt_api_close(&c, fd);
		memset(&cm, 0, sizeof(cm));
		apply_impair(nif, &cm);
	}
	rt_api_close(&c, lfd);
	rt_host_stop();
	return 0;
}

/* ---- process A ---- */
static struct rt_sockctx ca;
static struct rt_netif *anif;

static int open_cmd(const struct cmd *cm)
{
	struct rt_sockaddr_in b = t_sa(B_IP, PORT);
	u32 w[CMD_WORDS], i;
	int fd = -1, r = -1, tries;

	for (tries = 0; tries < 50; tries++) {
		fd = rt_api_socket(&ca, RT_AF_INET, RT_SOCK_STREAM, 0);
		r = rt_api_connect(&ca, fd, &b, sizeof(b));
		if (r == 0)
			break;
		rt_api_close(&ca, fd);
		usleep(100000);
	}
	if (r != 0)
		return -1;
	memcpy(w, cm, sizeof(w));
	for (i = 0; i < CMD_WORDS; i++)
		w[i] = rt_htonl(w[i]);
	rt_api_send(&ca, fd, w, sizeof(w), 0);
	return fd;
}

static void fetch_b_stats(struct rt_tcp_stats *st)
{
	struct cmd q;
	int fd;

	memset(st, 0, sizeof(*st));
	memset(&q, 0, sizeof(q));
	q.mode = CMD_STATS;
	q.sack = 1;
	q.recovery = 1;
	fd = open_cmd(&q);
	if (fd < 0)
		return;
	recv_all(&ca, fd, st, sizeof(*st));
	rt_api_close(&ca, fd);
}

struct run {
	const char *name;
	int dir;            /* 0: A sends to B, 1: B sends to A */
	u64 total;
	u32 loss, reorder, dup, delay, rate;
	u32 sack, reno, recovery;
};

static void do_run(const struct run *rn, u32 scale)
{
	struct cmd cm;
	struct rt_tcp_info ti;
	struct rt_tcp_stats before = rt_tcpstat, bbefore, bafter;
	u64 total = rn->total / scale;
	double t0, dt;
	int fd;
	u32 retrans_b0 = 0;

	memset(&cm, 0, sizeof(cm));
	cm.mode = rn->dir ? CMD_SOURCE : CMD_SINK;
	cm.total_lo = (u32)total;
	cm.total_hi = (u32)(total >> 32);
	cm.seed = 0xabc0 + rn->loss;
	cm.loss = rn->loss;
	cm.reorder = rn->reorder;
	cm.dup = rn->dup;
	cm.delay = rn->delay;
	cm.rate = rn->rate;
	cm.impair_seed = 777;
	cm.sack = rn->sack;
	cm.cc_reno = rn->reno;
	cm.recovery = rn->recovery;

	/* tunables on both sides before the data connection's handshake */
	apply_tunables(&cm);
	{
		struct cmd cf = cm;
		cf.mode = CMD_CONFIG;
		fd = open_cmd(&cf);
		if (fd >= 0) {
			char x;
			rt_api_recv(&ca, fd, &x, 1, 0);
			rt_api_close(&ca, fd);
		}
	}
	fetch_b_stats(&bbefore);
	fd = open_cmd(&cm);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	{
		struct cmd ai = cm;
		ai.impair_seed = 999;
		apply_impair(anif, &ai);
	}
	t0 = t_now();
	if (rn->dir == 0) {
		u8 reply[16];
		u64 sent = t_send_pattern(&ca, fd, total, cm.seed, 32768), got;
		u32 bad;
		t_get_info(&ca, fd, &ti);
		rt_api_shutdown(&ca, fd, 1);
		CHECK(recv_all(&ca, fd, reply, 16) == 0);
		dt = t_now() - t0;
		got = ((u64)rt_get_be32(reply) << 32) | rt_get_be32(reply + 4);
		bad = rt_get_be32(reply + 8);
		retrans_b0 = rt_get_be32(reply + 12);
		CHECKV(sent == total && got == total && bad == 0, "%s: sent %llu got %llu bad %u",
		       rn->name, (unsigned long long)sent, (unsigned long long)got, bad);
	} else {
		u64 bad, got = t_recv_pattern(&ca, fd, cm.seed, &bad, 65536);
		dt = t_now() - t0;
		t_get_info(&ca, fd, &ti);
		CHECKV(got == total && bad == 0, "%s: got %llu bad %llu", rn->name,
		       (unsigned long long)got, (unsigned long long)bad);
	}
	rt_api_close(&ca, fd);
	memset(&cm, 0, sizeof(cm));
	apply_impair(anif, &cm);
	printf("RUN %-28s %s %6.2f MB %6.2fs %8.2f Mbit/s  A: retrans %u fastretr %u rto %u "
	       "tlp %u rackloss %u sackrec %u renorec %u undo %u spurRTO %u dsack %u\n",
	       rn->name, rn->dir ? "B->A" : "A->B", total / 1048576.0, dt, total * 8 / dt / 1e6,
	       rt_tcpstat.RetransSegs - before.RetransSegs,
	       rt_tcpstat.TCPFastRetrans - before.TCPFastRetrans,
	       rt_tcpstat.TCPTimeouts - before.TCPTimeouts,
	       rt_tcpstat.TCPLossProbes - before.TCPLossProbes,
	       rt_tcpstat.TCPRackLoss - before.TCPRackLoss,
	       rt_tcpstat.TCPSackRecovery - before.TCPSackRecovery,
	       rt_tcpstat.TCPRenoRecovery - before.TCPRenoRecovery,
	       rt_tcpstat.TCPFullUndo - before.TCPFullUndo,
	       rt_tcpstat.TCPSpuriousRTOs - before.TCPSpuriousRTOs,
	       rt_tcpstat.TCPDSACKRecv - before.TCPDSACKRecv);
	fetch_b_stats(&bafter);
	printf("    B: retrans %u fastretr %u rto %u tlp %u rackloss %u sackrec %u renorec %u "
	       "undo %u dsack-sent %u\n",
	       bafter.RetransSegs - bbefore.RetransSegs, bafter.TCPFastRetrans - bbefore.TCPFastRetrans,
	       bafter.TCPTimeouts - bbefore.TCPTimeouts, bafter.TCPLossProbes - bbefore.TCPLossProbes,
	       bafter.TCPRackLoss - bbefore.TCPRackLoss, bafter.TCPSackRecovery - bbefore.TCPSackRecovery,
	       bafter.TCPRenoRecovery - bbefore.TCPRenoRecovery, bafter.TCPFullUndo - bbefore.TCPFullUndo,
	       (bafter.TCPDSACKOldSent + bafter.TCPDSACKOfoSent) -
	       (bbefore.TCPDSACKOldSent + bbefore.TCPDSACKOfoSent));
	(void)retrans_b0;
	t_print_info("    A", &ti);
	fflush(stdout);
}

/* keepalive: idle 1 s, interval 1 s, 3 probes; then cut the wire */
static void test_keepalive(void)
{
	struct cmd cm;
	struct rt_host_impair im;
	struct rt_tcp_info ti;
	double t0;
	char x;
	int fd, r;
	u32 ka0 = rt_tcpstat.TCPKeepAlive;

	memset(&cm, 0, sizeof(cm));
	cm.mode = CMD_HOLD;
	cm.sack = 1;
	cm.recovery = 1;
	fd = open_cmd(&cm);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	t_set_int(&ca, fd, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, 1);
	t_set_int(&ca, fd, RT_IPPROTO_TCP, RT_TCP_KEEPINTVL, 1);
	t_set_int(&ca, fd, RT_IPPROTO_TCP, RT_TCP_KEEPCNT, 3);
	t_set_int(&ca, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, 1);
	sleep(3);
	/* probes answered so far: still alive */
	t_get_info(&ca, fd, &ti);
	CHECK(ti.tcpi_state == RT_TCPS_ESTABLISHED);
	CHECKV(rt_tcpstat.TCPKeepAlive - ka0 >= 1, "keepalives %u", rt_tcpstat.TCPKeepAlive - ka0);
	memset(&im, 0, sizeof(im));
	im.loss = 10000;
	rt_host_impair(anif, &im);
	t0 = t_now();
	r = rt_api_recv(&ca, fd, &x, 1, 0);
	printf("keepalive: recv -> %d after %.1fs (probes %u)\n", r, t_now() - t0,
	       rt_tcpstat.TCPKeepAlive - ka0);
	CHECKV(r == -RT_ETIMEDOUT, "r=%d", r);
	CHECK(t_now() - t0 < 8);
	rt_api_close(&ca, fd);
	memset(&im, 0, sizeof(im));
	rt_host_impair(anif, &im);
}

/* TCP_USER_TIMEOUT: unacknowledged data for longer than 2 s aborts */
static void test_user_timeout(void)
{
	struct cmd cm;
	struct rt_host_impair im;
	static u8 buf[4000];
	double t0;
	int fd, r;

	memset(&cm, 0, sizeof(cm));
	cm.mode = CMD_HOLD;
	cm.sack = 1;
	cm.recovery = 1;
	fd = open_cmd(&cm);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	t_set_int(&ca, fd, RT_IPPROTO_TCP, RT_TCP_USER_TIMEOUT, 2000);
	usleep(200000);
	memset(&im, 0, sizeof(im));
	im.loss = 10000;
	rt_host_impair(anif, &im);
	t0 = t_now();
	rt_api_send(&ca, fd, buf, sizeof(buf), 0);
	for (;;) {
		r = rt_api_send(&ca, fd, buf, 1, 0);
		if (r < 0)
			break;
		usleep(100000);
		if (t_now() - t0 > 20)
			break;
	}
	printf("user timeout: send -> %d after %.1fs\n", r, t_now() - t0);
	CHECKV(r == -RT_ETIMEDOUT || r == -RT_EPIPE, "r=%d", r);
	CHECK(t_now() - t0 < 6);
	rt_api_close(&ca, fd);
	memset(&im, 0, sizeof(im));
	rt_host_impair(anif, &im);
}

int main(int argc, char **argv)
{
	static const struct run runs[] = {
		/* name                     dir total       loss reord dup delay rate  sack reno rack */
		{ "clean",                   0, 20u << 20,    0,    0,  0,  0,     0,   1,  0,  1 },
		{ "clean",                   1, 20u << 20,    0,    0,  0,  0,     0,   1,  0,  1 },
		{ "loss 1%",                 0, 20u << 20,  100,    0,  0,  0,     0,   1,  0,  1 },
		{ "loss 1%",                 1, 20u << 20,  100,    0,  0,  0,     0,   1,  0,  1 },
		{ "loss 5%",                 0, 20u << 20,  500,    0,  0,  0,     0,   1,  0,  1 },
		{ "reorder 5% + dup 1%",     0, 20u << 20,    0,  500,100,  0,     0,   1,  0,  1 },
		{ "delay 50ms",              0,  8u << 20,    0,    0,  0, 50,     0,   1,  0,  1 },
		{ "delay 50ms loss 1%",      1,  4u << 20,  100,    0,  0, 50,     0,   1,  0,  1 },
		{ "delay 50ms loss 5%",      0,  2u << 20,  500,    0,  0, 50,     0,   1,  0,  1 },
		{ "rate 20M delay 10ms",     0, 20u << 20,    0,    0,  0, 10, 20000,   1,  0,  1 },
		{ "newreno no-sack loss 1%", 0, 10u << 20,  100,    0,  0,  0,     0,   0,  1,  1 },
		{ "newreno no-sack loss 5%", 1,  5u << 20,  500,    0,  0,  5,     0,   0,  1,  1 },
		{ "sack no-rack loss 2%",    0, 10u << 20,  200,    0,  0,  5,     0,   1,  0,  0 },
		{ "loss 5% reord 5% 20ms",   1,  4u << 20,  500,  500,  0, 20,     0,   1,  0,  1 },
	};
	char pa[64], pb[64];
	pid_t child;
	u32 scale = 1, i;
	int status = 0;

	if (argc > 1)
		scale = (u32)strtoul(argv[1], NULL, 0);
	if (!scale)
		scale = 1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(pa, sizeof(pa), "/tmp/rt_tcpw_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_tcpw_%d_b", (int)getpid());
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(run_b(pa, pb));
	alarm(1800);
	anif = bring_up(pa, pb, mac_a, A_IP);
	CHECK(anif != NULL);
	if (!anif)
		goto out;
	rt_ctx_init(&ca, 16, NULL);
	for (i = 0; i < RT_ARRAY_SIZE(runs); i++)
		do_run(&runs[i], scale);
	{
		struct cmd d;
		memset(&d, 0, sizeof(d));
		d.sack = 1;
		d.recovery = 1;
		apply_tunables(&d);
		d.mode = CMD_CONFIG;
		i = (u32)open_cmd(&d);
		if ((int)i >= 0)
			rt_api_close(&ca, (int)i);
	}
	test_keepalive();
	test_user_timeout();
	t_print_stats("A");
	{
		struct cmd q;
		int fd;
		memset(&q, 0, sizeof(q));
		q.mode = CMD_QUIT;
		fd = open_cmd(&q);
		if (fd >= 0)
			rt_api_close(&ca, fd);
	}
out:
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(pa);
	unlink(pb);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
