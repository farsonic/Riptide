/*
 * Riptide - TCP path MTU black hole detection and packetization layer
 * PMTU discovery (RFC 4821, net.inet.tcp.mtu_probing) between two stack
 * instances over the simulated wire. The impairment layer silently drops
 * packets over 1400 bytes (no ICMP): with mtu_probing 1 bulk transfers in
 * both directions, over IPv4 and IPv6, complete after the black hole
 * reaction; with 0 they stall until TCP_USER_TIMEOUT; a second connection
 * starts at the remembered size; mtu_probing 2 probes up to the path MTU.
 * No privileges needed.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <signal.h>
#include <sys/wait.h>
#include "t_tcp_common.h"

#define A_IP  0xc0a85901u    /* 192.168.89.1 */
#define B_IP  0xc0a85902u    /* 192.168.89.2 */
#define A_IP6 "fd00:89::1"
#define B_IP6 "fd00:89::2"
#define PORT  7100           /* commands (IPv4) and IPv4 data */
#define PORT6 7101           /* IPv6 data */
#define HOLE  1400           /* the path's real MTU */

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 2, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 2, 0x0b };

enum { CMD_QUIT = 0, CMD_SINK = 1, CMD_SOURCE = 2, CMD_CONFIG = 3, CMD_STATS = 4 };
/* command header A -> B (big-endian u32) */
struct cmd {
	u32 mode, total, seed;
	u32 max_len;         /* B's transmit size limit */
	u32 probing;         /* B's net.inet.tcp.mtu_probing (CMD_CONFIG) */
};
#define CMD_WORDS 5

static int addr_ready(struct rt_netif *n, const char *addr)
{
	struct rt_in6_ifaddr_info ai[8];
	rt_in6_addr a;
	int i, k, ok = 0;

	rt_inet6_pton(addr, &a);
	for (i = 0; i < 50 && !ok; i++) {
		usleep(100000);
		rt_lock();
		k = rt_in6_addr_list(n, ai, 8);
		while (--k >= 0)
			if (k < 8 && rt_in6_eq(&ai[k].addr, &a) && !(ai[k].flags & RT_IN6_IFF_TENTATIVE))
				ok = 1;
		rt_unlock();
	}
	return ok;
}

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, u32 ip,
                                 const char *ip6)
{
	struct rt_netif *n;
	rt_in6_addr a;

	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	rt_inet6_pton(ip6, &a);
	rt_lock();
	rt_netif_set_addr(n, rt_htonl(ip), rt_htonl(0xffffff00u), 0);
	rt_netif_up(n);
	rt_in6_addr_add(n, &a, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE);
	rt_unlock();
	return addr_ready(n, ip6) ? n : NULL;
}

static void set_hole(struct rt_netif *n, u32 max_len)
{
	struct rt_host_impair im;

	memset(&im, 0, sizeof(im));
	im.max_len = max_len;
	rt_host_impair(n, max_len ? &im : NULL);
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

static struct rt_sockaddr_in6 sa6(const char *addr, u16 port)
{
	struct rt_sockaddr_in6 s;

	memset(&s, 0, sizeof(s));
	s.sin6_len = sizeof(s);
	s.sin6_family = RT_AF_INET6;
	s.sin6_port = rt_htons(port);
	rt_inet6_pton(addr, &s.sin6_addr);
	return s;
}

/* ---- process B: command server on IPv4 and IPv6 ---- */
static int run_b(const char *pa, const char *pb)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = t_sa(0, PORT);
	struct rt_sockaddr_in6 a6 = sa6("::", PORT6);
	struct rt_netif *nif;
	int lfd[2], k = 0;

	alarm(900);
	nif = bring_up(pb, pa, mac_b, B_IP, B_IP6);
	if (!nif)
		return 2;
	rt_ctx_init(&c, 16, NULL);
	lfd[0] = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	lfd[1] = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	t_set_int(&c, lfd[0], RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	t_set_int(&c, lfd[1], RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	if (rt_api_bind(&c, lfd[0], &a, sizeof(a)) || rt_api_listen(&c, lfd[0], 4) ||
	    rt_api_bind(&c, lfd[1], &a6, sizeof(a6)) || rt_api_listen(&c, lfd[1], 4))
		return 3;
	t_set_nbio(&c, lfd[0], 1);
	t_set_nbio(&c, lfd[1], 1);
	for (;;) {
		u32 w[CMD_WORDS], i;
		struct cmd cm;
		u8 reply[12];
		int fd = rt_api_accept(&c, lfd[k], NULL, NULL);

		k ^= 1;
		if (fd < 0) {
			usleep(2000);
			continue;
		}
		t_set_nbio(&c, fd, 0);
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
			t_sysctl("net.inet.tcp.mtu_probing", (s32)cm.probing);
			rt_lock();
			rt_sysctl_byname("net.inet.tcp.mtu_cache", NULL, NULL, "clear", 5);
			rt_unlock();
		} else if (cm.mode == CMD_STATS) {
			rt_api_send(&c, fd, &rt_tcpstat, sizeof(rt_tcpstat), 0);
		} else if (cm.mode == CMD_SINK) {
			struct rt_timeval tv = { 10, 0 };   /* the stall test's sender gives up */
			u64 bad, got;
			rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
			got = t_recv_pattern(&c, fd, cm.seed, &bad, 65536);
			rt_put_be32(reply, (u32)got);
			rt_put_be32(reply + 4, (u32)bad);
			rt_api_send(&c, fd, reply, 8, 0);
		} else if (cm.mode == CMD_SOURCE) {
			struct rt_tcp_info ti;
			set_hole(nif, cm.max_len);
			t_send_pattern(&c, fd, cm.total, cm.seed, 32768);
			rt_api_shutdown(&c, fd, 1);
			rt_api_recv(&c, fd, reply, 1, 0);
			t_get_info(&c, fd, &ti);
			printf("    B: final mss %u\n", ti.tcpi_snd_mss);
			set_hole(nif, 0);
		}
		rt_api_close(&c, fd);
	}
	rt_api_close(&c, lfd[0]);
	rt_api_close(&c, lfd[1]);
	rt_host_stop();
	return 0;
}

/* ---- process A ---- */
static struct rt_sockctx ca;
static struct rt_netif *anif;

static int open_cmd(const struct cmd *cm, int v6)
{
	struct rt_sockaddr_in b = t_sa(B_IP, PORT);
	struct rt_sockaddr_in6 b6 = sa6(B_IP6, PORT6);
	u32 w[CMD_WORDS], i;
	int fd = -1, r = -1, tries;

	for (tries = 0; tries < 50; tries++) {
		fd = rt_api_socket(&ca, v6 ? RT_AF_INET6 : RT_AF_INET, RT_SOCK_STREAM, 0);
		r = v6 ? rt_api_connect(&ca, fd, &b6, sizeof(b6)) : rt_api_connect(&ca, fd, &b, sizeof(b));
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

/* a command without data: wait for B to close */
static void simple_cmd(u32 mode, u32 probing, void *out, u32 outlen)
{
	struct cmd q;
	char x;
	int fd;

	memset(&q, 0, sizeof(q));
	q.mode = mode;
	q.probing = probing;
	fd = open_cmd(&q, 0);
	if (fd < 0)
		return;
	if (out)
		recv_all(&ca, fd, out, outlen);
	else
		rt_api_recv(&ca, fd, &x, 1, 0);
	rt_api_close(&ca, fd);
}

static void configure(u32 probing)
{
	t_sysctl("net.inet.tcp.mtu_probing", (s32)probing);
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.mtu_cache", NULL, NULL, "clear", 5);
	rt_unlock();
	simple_cmd(CMD_CONFIG, probing, NULL, 0);
}

struct res {
	double secs;
	u32 got, mss;
	int err;
	struct rt_tcp_stats a, b;      /* counter deltas */
};

static void stat_delta(struct rt_tcp_stats *d, const struct rt_tcp_stats *x, const struct rt_tcp_stats *y)
{
	d->TCPMTUPBlackhole = y->TCPMTUPBlackhole - x->TCPMTUPBlackhole;
	d->TCPMTUPSuccess = y->TCPMTUPSuccess - x->TCPMTUPSuccess;
	d->TCPMTUPFail = y->TCPMTUPFail - x->TCPMTUPFail;
	d->TCPTimeouts = y->TCPTimeouts - x->TCPTimeouts;
	d->RetransSegs = y->RetransSegs - x->RetransSegs;
}

/* one transfer of 'total' bytes; dir 0: A sends, 1: B sends. hole: the
 * sender's size limit. user_to: A's TCP_USER_TIMEOUT (stall test). */
static struct res transfer(const char *name, int v6, int dir, u32 total, u32 hole, u32 user_to)
{
	struct cmd cm;
	struct res r;
	struct rt_tcp_info ti;
	struct rt_tcp_stats a0 = rt_tcpstat, b0, b1;
	double t0;
	int fd;

	memset(&r, 0, sizeof(r));
	simple_cmd(CMD_STATS, 0, &b0, sizeof(b0));
	memset(&cm, 0, sizeof(cm));
	cm.mode = dir ? CMD_SOURCE : CMD_SINK;
	cm.total = total;
	cm.seed = 0x5a00 + total;
	cm.max_len = dir ? hole : 0;
	fd = open_cmd(&cm, v6);
	CHECK(fd >= 0);
	if (fd < 0)
		return r;
	if (user_to)
		t_set_int(&ca, fd, RT_IPPROTO_TCP, RT_TCP_USER_TIMEOUT, (int)user_to);
	if (!dir)
		set_hole(anif, hole);
	t0 = t_now();
	if (!dir) {
		u8 reply[8];
		u64 sent = t_send_pattern(&ca, fd, total, cm.seed, 32768);
		t_get_info(&ca, fd, &ti);
		r.err = sent == total ? 0 : -1;
		if (sent == total) {
			rt_api_shutdown(&ca, fd, 1);
			if (recv_all(&ca, fd, reply, 8) == 0 && rt_get_be32(reply + 4) == 0)
				r.got = rt_get_be32(reply);
			else
				r.err = -1;
		}
	} else {
		u64 bad, got = t_recv_pattern(&ca, fd, cm.seed, &bad, 65536);
		t_get_info(&ca, fd, &ti);
		r.got = bad ? 0 : (u32)got;
	}
	r.secs = t_now() - t0;
	r.mss = ti.tcpi_snd_mss;
	rt_api_close(&ca, fd);
	set_hole(anif, 0);
	simple_cmd(CMD_STATS, 0, &b1, sizeof(b1));
	stat_delta(&r.a, &a0, &rt_tcpstat);
	stat_delta(&r.b, &b0, &b1);
	printf("RUN %-34s %s %s %5.2f MB %6.2fs got %u err %d A: mss %u blackhole %u probes ok %u "
	       "lost %u rto %u retrans %u | B: blackhole %u ok %u lost %u rto %u\n",
	       name, v6 ? "v6" : "v4", dir ? "B->A" : "A->B", total / 1048576.0, r.secs, r.got, r.err,
	       r.mss, r.a.TCPMTUPBlackhole, r.a.TCPMTUPSuccess, r.a.TCPMTUPFail, r.a.TCPTimeouts,
	       r.a.RetransSegs, r.b.TCPMTUPBlackhole, r.b.TCPMTUPSuccess, r.b.TCPMTUPFail, r.b.TCPTimeouts);
	fflush(stdout);
	return r;
}

/* the mss that fits a 1400 byte path with timestamps (IPv4 / IPv6) */
#define FIT4 (HOLE - 20 - 32)
#define FIT6 (HOLE - 40 - 32)

int main(int argc, char **argv)
{
	char pa[64], pb[64], cache[256];
	pid_t child;
	int status = 0;
	u32 mb = 1u << 20, len;
	struct res base, r;

	(void)argc;
	(void)argv;
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(pa, sizeof(pa), "/tmp/rt_mtup_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_mtup_%d_b", (int)getpid());
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(run_b(pa, pb));
	alarm(900);
	anif = bring_up(pa, pb, mac_a, A_IP, A_IP6);
	CHECK(anif != NULL);
	if (!anif)
		goto out;
	rt_ctx_init(&ca, 16, NULL);

	/* the default is 1 (Linux) */
	{
		s32 v = -1;
		u32 l = sizeof(v);
		rt_lock();
		rt_sysctl_byname("net.inet.tcp.mtu_probing", &v, &l, NULL, 0);
		rt_unlock();
		CHECK(v == 1);
	}

	configure(1);
	base = transfer("clean path", 0, 0, mb, 0, 0);
	CHECK(base.got == mb && base.mss == 1448 && base.a.TCPMTUPBlackhole == 0);

	/* upload into the black hole: detected after tcp_retries1 timeouts */
	r = transfer("black hole, mtu_probing 1", 0, 0, mb, HOLE, 0);
	CHECKV(r.got == mb, "got %u", r.got);
	CHECK(r.a.TCPMTUPBlackhole >= 1 && r.mss <= FIT4);
	CHECKV(r.secs < 15, "%.1fs", r.secs);
	printf("    recovery: %.2fs over the clean transfer\n", r.secs - base.secs);

	/* the destination is remembered: the next connection starts small */
	len = sizeof(cache);
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.mtu_cache", cache, &len, NULL, 0);
	rt_unlock();
	printf("    mtu_cache: %s", cache);
	CHECK(strstr(cache, "192.168.89.2 mtu ") != NULL);
	r = transfer("black hole, remembered", 0, 0, mb, HOLE, 0);
	CHECK(r.got == mb && r.a.TCPMTUPBlackhole == 0 && r.a.TCPTimeouts == 0 && r.mss <= FIT4);
	CHECKV(r.secs < 2, "%.1fs", r.secs);

	/* download: B is the sender */
	configure(1);
	r = transfer("black hole, mtu_probing 1", 0, 1, mb, HOLE, 0);
	CHECKV(r.got == mb, "got %u", r.got);
	CHECK(r.b.TCPMTUPBlackhole >= 1);

	/* IPv6 both ways */
	configure(1);
	r = transfer("black hole, mtu_probing 1", 1, 0, mb, HOLE, 0);
	CHECKV(r.got == mb, "got %u", r.got);
	CHECK(r.a.TCPMTUPBlackhole >= 1 && r.mss <= FIT6);
	r = transfer("black hole, mtu_probing 1", 1, 1, mb, HOLE, 0);
	CHECKV(r.got == mb, "got %u", r.got);

	/* off: the transfer stalls until TCP_USER_TIMEOUT */
	configure(0);
	r = transfer("black hole, mtu_probing 0", 0, 0, mb, HOLE, 6000);
	CHECKV(r.got < mb && r.err && r.secs > 5.5 && r.secs < 9, "got %u err %d", r.got, r.err);
	CHECK(r.a.TCPMTUPBlackhole == 0 && r.mss == 1448);

	/* always on: start at base_mss, probe up to the path MTU */
	configure(2);
	r = transfer("clean path, mtu_probing 2", 0, 0, 8 * mb, 0, 0);
	CHECK(r.got == 8 * mb && r.a.TCPMTUPSuccess >= 3);
	CHECKV(r.mss >= 1448 - 8 && r.mss <= 1448, "mss %u", r.mss);
	r = transfer("black hole, mtu_probing 2", 0, 0, 8 * mb, HOLE, 0);
	CHECK(r.got == 8 * mb && r.a.TCPMTUPSuccess >= 1 && r.a.TCPMTUPFail >= 1);
	CHECK(r.a.TCPMTUPBlackhole == 0);
	CHECKV(r.mss >= FIT4 - 8 && r.mss <= FIT4, "mss %u", r.mss);
	r = transfer("black hole, mtu_probing 2", 1, 1, 8 * mb, HOLE, 0);
	CHECK(r.got == 8 * mb && r.b.TCPMTUPFail >= 1 && r.b.TCPMTUPBlackhole == 0);

	configure(1);
	simple_cmd(CMD_QUIT, 0, NULL, 0);
out:
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(pa);
	unlink(pb);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
