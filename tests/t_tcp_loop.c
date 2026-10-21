/*
 * Riptide - TCP functional tests over the loopback interface (one stack,
 * client and server threads): handshake, data integrity, half close,
 * RST cases, TIME_WAIT and port reuse, urgent data, zero window, listen
 * backlog with SYN cookies, many connections, simultaneous open.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"

#define LO 0x7f000001u

static struct rt_sockctx cs, ss;    /* client / server contexts */

static int listen_on(struct rt_sockctx *c, u16 port, int backlog)
{
	struct rt_sockaddr_in a = t_sa(0, port);
	int fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, 0);

	t_set_int(c, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	if (rt_api_bind(c, fd, &a, sizeof(a)) != 0 || rt_api_listen(c, fd, backlog) != 0) {
		rt_api_close(c, fd);
		return -1;
	}
	return fd;
}

static int connect_to(struct rt_sockctx *c, u16 port)
{
	struct rt_sockaddr_in a = t_sa(LO, port);
	int fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, 0);
	int r = rt_api_connect(c, fd, &a, sizeof(a));

	if (r != 0) {
		fprintf(stderr, "connect: %d\n", r);
		rt_api_close(c, fd);
		return r;
	}
	return fd;
}

/* ---- bulk transfer with a server thread ---- */
struct bulk_arg {
	int lfd;
	u64 total;
	u32 seed;
	u64 got, bad;
	int reverse;          /* server sends instead */
};

static void *bulk_server(void *p)
{
	struct bulk_arg *a = p;
	int fd = rt_api_accept(&ss, a->lfd, NULL, NULL);

	if (fd < 0) {
		fprintf(stderr, "accept %d\n", fd);
		return NULL;
	}
	if (a->reverse) {
		a->got = t_send_pattern(&ss, fd, a->total, a->seed, 16384);
		rt_api_shutdown(&ss, fd, 1);
		{
			char x;
			rt_api_recv(&ss, fd, &x, 1, 0);
		}
	} else {
		a->got = t_recv_pattern(&ss, fd, a->seed, &a->bad, 32768);
	}
	rt_api_close(&ss, fd);
	return NULL;
}

static void test_bulk(u16 port, u64 total, int reverse)
{
	struct bulk_arg a;
	pthread_t th;
	struct rt_tcp_info ti;
	double t0, dt;
	int fd;

	memset(&a, 0, sizeof(a));
	a.lfd = listen_on(&ss, port, 5);
	a.total = total;
	a.seed = 0x1234 + port;
	a.reverse = reverse;
	CHECK(a.lfd >= 0);
	pthread_create(&th, NULL, bulk_server, &a);
	fd = connect_to(&cs, port);
	CHECK(fd >= 0);
	if (fd < 0) {
		rt_api_close(&ss, a.lfd);
		pthread_join(th, NULL);
		return;
	}
	t0 = t_now();
	if (reverse) {
		u64 bad, got = t_recv_pattern(&cs, fd, a.seed, &bad, 32768);
		CHECKV(got == total && bad == 0, "got %llu bad %llu", (unsigned long long)got,
		       (unsigned long long)bad);
		t_get_info(&cs, fd, &ti);
		rt_api_close(&cs, fd);
		fd = -1;
	} else {
		u64 sent = t_send_pattern(&cs, fd, total, a.seed, 16384);
		CHECK(sent == total);
		t_get_info(&cs, fd, &ti);
		rt_api_shutdown(&cs, fd, 1);
	}
	pthread_join(th, NULL);
	dt = t_now() - t0;
	if (!reverse)
		CHECKV(a.got == total && a.bad == 0, "got %llu bad %llu", (unsigned long long)a.got,
		       (unsigned long long)a.bad);
	t_print_info(reverse ? "loop bulk receiver" : "loop bulk sender", &ti);
	printf("loopback bulk %s %llu bytes in %.2fs: %.1f Mbit/s\n", reverse ? "s->c" : "c->s",
	       (unsigned long long)total, dt, total * 8 / dt / 1e6);
	if (fd >= 0)
		rt_api_close(&cs, fd);
	rt_api_close(&ss, a.lfd);
}

/* ---- connect to a closed port ---- */
static void test_refused(void)
{
	struct rt_sockaddr_in a = t_sa(LO, 1);
	int fd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	int r = rt_api_connect(&cs, fd, &a, sizeof(a));

	CHECKV(r == -RT_ECONNREFUSED, "r=%d", r);
	rt_api_close(&cs, fd);
}


/* accept helper with timeout (non-blocking listener) */
static int accept_wait(struct rt_sockctx *c, int lfd, int ms)
{
	int i, fd = -1;

	t_set_nbio(c, lfd, 1);
	for (i = 0; i < ms; i++) {
		fd = rt_api_accept(c, lfd, NULL, NULL);
		if (fd >= 0 || fd != -RT_EWOULDBLOCK)
			break;
		usleep(1000);
	}
	t_set_nbio(c, lfd, 0);
	return fd;
}

static int pair(u16 port, int *lfd, int *cfd, int *afd)
{
	*lfd = listen_on(&ss, port, 5);
	*cfd = connect_to(&cs, port);
	*afd = *lfd >= 0 && *cfd >= 0 ? accept_wait(&ss, *lfd, 2000) : -1;
	return *lfd >= 0 && *cfd >= 0 && *afd >= 0;
}

/* half close: client shuts down writing, server keeps sending */
static void test_half_close(void)
{
	int lfd, cfd, afd, r;
	char buf[64];
	struct rt_tcp_info ti;

	CHECK(pair(5010, &lfd, &cfd, &afd));
	CHECK(rt_api_send(&cs, cfd, "abc", 3, 0) == 3);
	CHECK(rt_api_shutdown(&cs, cfd, 1) == 0);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), RT_MSG_WAITALL);
	CHECKV(r == 3, "r=%d", r);
	CHECK(rt_api_recv(&ss, afd, buf, sizeof(buf), 0) == 0);      /* EOF */
	t_get_info(&ss, afd, &ti);
	CHECK(ti.tcpi_state == RT_TCPS_CLOSE_WAIT);
	for (r = 0; r < 100; r++) {          /* the FIN's ACK may be delayed */
		t_get_info(&cs, cfd, &ti);
		if (ti.tcpi_state == RT_TCPS_FIN_WAIT_2)
			break;
		usleep(10000);
	}
	CHECK(ti.tcpi_state == RT_TCPS_FIN_WAIT_2);
	CHECK(rt_api_send(&cs, cfd, "x", 1, 0) == -RT_EPIPE);
	CHECK(rt_api_send(&ss, afd, "reply", 5, 0) == 5);
	CHECK(rt_api_recv(&cs, cfd, buf, 5, RT_MSG_WAITALL) == 5 && memcmp(buf, "reply", 5) == 0);
	rt_api_close(&ss, afd);       /* LAST_ACK -> CLOSED */
	CHECK(rt_api_recv(&cs, cfd, buf, sizeof(buf), 0) == 0);
	usleep(20000);
	t_get_info(&cs, cfd, &ti);
	CHECKV(ti.tcpi_state == RT_TCPS_CLOSED, "state %u", ti.tcpi_state);  /* now in TIME_WAIT record */
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, lfd);
}

static int count_state(u16 lport, int state)
{
	static struct rt_pcb_info p[512];
	int n, i, c = 0;

	rt_lock();
	n = rt_tcp_list(p, 512);
	rt_unlock();
	for (i = 0; i < n && i < 512; i++)
		if ((p[i].lport == lport || p[i].fport == lport) && p[i].state == state)
			c++;
	return c;
}

/* RST: close with unread data; SO_LINGER 0; RST on a listener-less port */
static void test_resets(void)
{
	int lfd, cfd, afd, r;
	char buf[16];
	struct rt_linger lg;
	u32 rst0;

	CHECK(pair(5011, &lfd, &cfd, &afd));
	CHECK(rt_api_send(&cs, cfd, "unread", 6, 0) == 6);
	usleep(20000);
	rst0 = rt_tcpstat.OutRsts;
	rt_api_close(&ss, afd);             /* unread data: RST (RFC 2525) */
	r = rt_api_recv(&cs, cfd, buf, sizeof(buf), 0);
	CHECKV(r == -RT_ECONNRESET, "r=%d", r);
	CHECK(rt_tcpstat.OutRsts > rst0);
	CHECK(rt_tcpstat.TCPAbortOnClose >= 1);
	rt_api_close(&cs, cfd);

	cfd = connect_to(&cs, 5011);
	afd = accept_wait(&ss, lfd, 2000);
	CHECK(cfd >= 0 && afd >= 0);
	lg.l_onoff = 1;
	lg.l_linger = 0;
	rt_api_setsockopt(&cs, cfd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof(lg));
	rt_api_close(&cs, cfd);             /* abortive close */
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == -RT_ECONNRESET, "r=%d", r);
	CHECK(count_state(5011, RT_TCPS_TIME_WAIT) == 0);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* TIME_WAIT: active closer keeps a record; port reuse rules */
static void test_time_wait(void)
{
	int lfd, cfd, afd, fd;
	char buf[4];
	struct rt_sockaddr_in a = t_sa(0, 5012);

	CHECK(pair(5012, &lfd, &cfd, &afd));
	rt_api_close(&ss, lfd);
	rt_api_close(&ss, afd);                    /* server closes first */
	CHECK(rt_api_recv(&cs, cfd, buf, 4, 0) == 0);
	rt_api_close(&cs, cfd);
	usleep(20000);
	CHECK(count_state(5012, RT_TCPS_TIME_WAIT) == 1);
	/* bind without SO_REUSEADDR conflicts with the TIME_WAIT owner */
	fd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_api_bind(&ss, fd, &a, sizeof(a)) == -RT_EADDRINUSE);
	rt_api_close(&ss, fd);
	fd = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&ss, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&ss, fd, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&ss, fd, 5) == 0);
	/* a new SYN to the TIME_WAIT 4-tuple is accepted (RFC 6191) by
	 * reconnecting from the same client port */
	rt_api_close(&ss, fd);
}

/* urgent data: MSG_OOB byte and SIOCATMARK */
static void test_oob(void)
{
	int lfd, cfd, afd, r;
	char buf[16];
	s32 mark = -1;

	CHECK(pair(5013, &lfd, &cfd, &afd));
	CHECK(rt_api_send(&cs, cfd, "ab", 2, 0) == 2);
	CHECK(rt_api_send(&cs, cfd, "!", 1, RT_MSG_OOB) == 1);
	CHECK(rt_api_send(&cs, cfd, "cd", 2, 0) == 2);
	usleep(30000);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == 2 && memcmp(buf, "ab", 2) == 0, "r=%d", r);   /* stops at the mark */
	rt_api_ioctl(&ss, afd, RT_SIOCATMARK, &mark);
	CHECK(mark == 1);
	r = rt_api_recv(&ss, afd, buf, 1, RT_MSG_OOB);
	CHECKV(r == 1 && buf[0] == '!', "r=%d", r);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == 2 && memcmp(buf, "cd", 2) == 0, "r=%d", r);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* zero window: small receive buffer and a reader that pauses */
static void test_zero_window(void)
{
	int lfd, cfd, afd, r, total = 0;
	static u8 buf[8192];
	u32 probes0 = rt_tcpstat.TCPWinProbe;
	u64 got = 0, bad = 0;
	struct rt_tcp_info ti;

	lfd = listen_on(&ss, 5014, 5);
	t_set_int(&ss, lfd, RT_SOL_SOCKET, RT_SO_RCVBUF, 4096);
	cfd = connect_to(&cs, 5014);
	afd = accept_wait(&ss, lfd, 2000);
	CHECK(lfd >= 0 && cfd >= 0 && afd >= 0);
	t_set_nbio(&cs, cfd, 1);
	for (;;) {
		t_fill(buf, sizeof(buf), (u64)total, 77);
		r = rt_api_send(&cs, cfd, buf, sizeof(buf), 0);
		if (r <= 0)
			break;
		total += r;
	}
	CHECK(r == -RT_EWOULDBLOCK);
	sleep(2);                                  /* persist timer runs */
	t_get_info(&cs, cfd, &ti);
	CHECKV(ti.tcpi_snd_wnd == 0, "snd_wnd %u", ti.tcpi_snd_wnd);
	CHECKV(rt_tcpstat.TCPWinProbe > probes0, "probes %u", rt_tcpstat.TCPWinProbe - probes0);
	/* diagnostics: the sender saw a zero window and was held back by it,
	 * the receiver advertised one and its reader left data unread */
	CHECKV(ti.tcpi_zwin_in >= 1, "zwin_in %u", ti.tcpi_zwin_in);
	/* sndbuf-limited takes precedence over rwnd-limited (as in Linux) */
	CHECKV(ti.tcpi_rwnd_limited_ms + ti.tcpi_sndbuf_limited_ms >= 1500, "rwnd %u sndbuf %u ms",
	       ti.tcpi_rwnd_limited_ms, ti.tcpi_sndbuf_limited_ms);
	CHECKV(ti.tcpi_busy_ms >= ti.tcpi_rwnd_limited_ms + ti.tcpi_sndbuf_limited_ms, "busy %u",
	       ti.tcpi_busy_ms);
	t_get_info(&ss, afd, &ti);
	CHECKV(ti.tcpi_zwin_out >= 1, "zwin_out %u", ti.tcpi_zwin_out);
	CHECKV(ti.tcpi_zwin_ms >= 1500, "zwin %u ms", ti.tcpi_zwin_ms);
	CHECKV(ti.tcpi_unread_ms >= 1500, "unread %u ms", ti.tcpi_unread_ms);
	rt_api_shutdown(&cs, cfd, 1);
	got = t_recv_pattern(&ss, afd, 77, &bad, 1000);
	CHECKV(got == (u64)total && bad == 0, "got %llu of %d bad %llu", (unsigned long long)got,
	       total, (unsigned long long)bad);
	printf("zero window: %d bytes queued before EWOULDBLOCK, %u window probes\n", total,
	       rt_tcpstat.TCPWinProbe - probes0);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* ---- diagnostics: net.riptide.tcpinfo and TCP_INFO ---- */
static int find_tcpinfo(u16 lport, u16 fport, struct rt_ctl_tcpinfo *out)
{
	u8 *buf;
	u32 len = 0, k;
	struct rt_ctl_hdr h;
	int found = 0;

	rt_lock();
	rt_sysctl_byname("net.riptide.tcpinfo", NULL, &len, NULL, 0);
	len += 4096;
	buf = malloc(len);
	if (rt_sysctl_byname("net.riptide.tcpinfo", buf, &len, NULL, 0) != 0) {
		rt_unlock();
		free(buf);
		return 0;
	}
	rt_unlock();
	memcpy(&h, buf, sizeof(h));
	CHECKV(h.recsize == sizeof(struct rt_ctl_tcpinfo), "recsize %u", h.recsize);
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_tcpinfo *t = (void *)(buf + sizeof(h) + k * h.recsize);
		if (t->lport == lport && t->fport == fport) {
			*out = *t;
			found = 1;
		}
	}
	free(buf);
	return found;
}

/* sender thread: 'total' bytes of pattern on the server side, then FIN */
struct snd_arg {
	int fd;
	u64 total, sent;
	u32 seed;
};

static void *snd_thread(void *p)
{
	struct snd_arg *a = p;

	a->sent = t_send_pattern(&ss, a->fd, a->total, a->seed, 16384);
	rt_api_shutdown(&ss, a->fd, 1);
	return NULL;
}

static u16 local_port(struct rt_sockctx *c, int fd)
{
	struct rt_sockaddr_in a;
	u32 l = sizeof(a);

	memset(&a, 0, sizeof(a));
	rt_api_getsockname(c, fd, &a, &l);
	return rt_ntohs(a.sin_port);
}

/* server -> client bulk on a fresh pair; *ti = the client's TCP_INFO */
static u64 pull(u16 port, u64 total, struct rt_tcp_info *ti, int *cfd_out, int *afd_out, int *lfd_out)
{
	int lfd, cfd, afd;
	u64 got = 0, bad = 0;
	struct snd_arg sa;
	pthread_t th;

	*cfd_out = *afd_out = *lfd_out = -1;
	if (!pair(port, &lfd, &cfd, &afd)) {
		CHECK(0);
		return 0;
	}
	t_set_nbio(&ss, afd, 0);
	memset(&sa, 0, sizeof(sa));
	sa.fd = afd;
	sa.total = total;
	sa.seed = port;
	pthread_create(&th, NULL, snd_thread, &sa);
	got = t_recv_pattern(&cs, cfd, port, &bad, 32768);
	pthread_join(th, NULL);
	CHECKV(got == total && bad == 0, "got %llu bad %llu", (unsigned long long)got, (unsigned long long)bad);
	t_get_info(&cs, cfd, ti);
	*cfd_out = cfd;
	*afd_out = afd;
	*lfd_out = lfd;
	return got;
}

static void test_tcpinfo(void)
{
	int lfd, cfd, afd;
	u64 total = 2u << 20, got, bad = 0;
	struct rt_tcp_info ci, si;
	struct rt_ctl_tcpinfo t;

	pull(5060, total, &ci, &cfd, &afd, &lfd);
	/* the other way: data sits unread for a while */
	CHECK(t_send_pattern(&cs, cfd, 65536, 5, 16384) == 65536);
	usleep(300000);
	rt_api_shutdown(&cs, cfd, 1);
	got = t_recv_pattern(&ss, afd, 5, &bad, 32768);
	CHECK(got == 65536 && bad == 0);
	usleep(100000);
	t_get_info(&cs, cfd, &ci);
	t_get_info(&ss, afd, &si);
	CHECKV(ci.tcpi_data_segs_in > 0 && ci.tcpi_data_segs_in <= ci.tcpi_segs_in, "data_segs_in %u segs_in %u",
	       ci.tcpi_data_segs_in, ci.tcpi_segs_in);
	CHECKV(ci.tcpi_acks_out > 0 && ci.tcpi_acks_out <= ci.tcpi_data_segs_in + 8, "acks_out %u data_segs_in %u",
	       ci.tcpi_acks_out, ci.tcpi_data_segs_in);
	CHECK(ci.tcpi_delayed_acks_out <= ci.tcpi_acks_out);
	CHECKV(si.tcpi_data_segs_out + 1 >= ci.tcpi_data_segs_in, "out %u in %u", si.tcpi_data_segs_out,
	       ci.tcpi_data_segs_in);
	CHECKV(si.tcpi_unread_ms >= 250, "server unread %u ms", si.tcpi_unread_ms);
	CHECKV(ci.tcpi_rcvbuf > 0 && ci.tcpi_sndbuf > 0, "rcvbuf %u sndbuf %u", ci.tcpi_rcvbuf, ci.tcpi_sndbuf);
	CHECK(ci.tcpi_rto_timeouts == 0 && ci.tcpi_fast_recoveries == 0);
	/* the table carries the same counters, 64-bit byte counts included */
	CHECK(find_tcpinfo(local_port(&cs, cfd), 5060, &t));
	CHECKV(t.bytes_received_lo == total && t.bytes_received_hi == 0, "rx %u", t.bytes_received_lo);
	CHECKV(t.bytes_acked_lo == 65536, "acked %u", t.bytes_acked_lo);
	CHECK(t.family == RT_AF_INET && t.faddr[10] == 0xff && t.faddr[12] == 127);
	CHECK(t.acks_out >= ci.tcpi_acks_out && t.data_segs_in == ci.tcpi_data_segs_in);
	CHECKV(t.age_ms >= 300 && t.age_ms < 60000, "age %u", t.age_ms);
	CHECKV(!strcmp(t.cc, "cubic") || !strcmp(t.cc, "reno"), "cc %s", t.cc);
	CHECK(find_tcpinfo(5060, 0, &t) && t.state == RT_TCPS_LISTEN);
	printf("tcpinfo: client rx %u data segs, %u ACKs (%u delayed), server unread %u ms\n",
	       ci.tcpi_data_segs_in, ci.tcpi_acks_out, ci.tcpi_delayed_acks_out, si.tcpi_unread_ms);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* net.inet.tcp.delack_segs / quickack: ACKs per data segment received.
 * The sender paces full segments 1 ms apart and a reader thread empties
 * the queue in between, as a receiving application that keeps up does. */
static void *drain_thread(void *p)
{
	static u8 buf[65536];
	int fd = *(int *)p;

	while (rt_api_recv(&cs, fd, buf, sizeof(buf), 0) > 0)
		;
	return NULL;
}

static double ack_ratio(u16 port, const char *segs, const char *quick)
{
	static u8 buf[1460];
	int lfd, cfd, afd, k, sent = 0;
	struct rt_tcp_info t0, t1;
	u32 segs_in, acks;
	pthread_t th;
	double ratio;

	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet.tcp.delack_segs", segs) == 0);
	CHECK(rt_sysctl_set_text("net.inet.tcp.quickack", quick) == 0);
	/* no SACK, no tail loss probes: on loopback a probe beats the
	 * delayed ACK timer, and the duplicate it causes means quick ACKs */
	rt_sysctl_set_text("net.inet.tcp.sack", "0");
	rt_unlock();
	lfd = listen_on(&ss, port, 5);
	cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_MAXSEG, 1460);     /* Ethernet-sized */
	{
		struct rt_sockaddr_in a = t_sa(LO, port);
		CHECK(rt_api_connect(&cs, cfd, &a, sizeof(a)) == 0);
	}
	afd = accept_wait(&ss, lfd, 2000);
	CHECK(lfd >= 0 && cfd >= 0 && afd >= 0);
	t_set_nbio(&ss, afd, 0);
	t_set_int(&ss, afd, RT_IPPROTO_TCP, RT_TCP_NODELAY, 1);
	t_get_info(&ss, afd, &t0);
	pthread_create(&th, NULL, drain_thread, &cfd);
	memset(buf, 0x5a, sizeof(buf));
	for (k = 0; k < 400; k++) {
		if (k == 40) {
			usleep(300000);                   /* delayed ACK timer, then */
			t_get_info(&cs, cfd, &t0);        /* past the initial quick ACKs */
		}
		sent += rt_api_send(&ss, afd, buf, t0.tcpi_snd_mss ? t0.tcpi_snd_mss : 1448, 0) > 0;
		usleep(1000);
	}
	CHECKV(sent == 400, "sent %d", sent);
	usleep(300000);
	t_get_info(&cs, cfd, &t1);
	rt_api_shutdown(&ss, afd, 1);
	pthread_join(th, NULL);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
	segs_in = t1.tcpi_data_segs_in - t0.tcpi_data_segs_in;
	acks = t1.tcpi_acks_out - t0.tcpi_acks_out;
	ratio = segs_in ? (double)acks / segs_in : 0;
	printf("delack_segs %s quickack %s: %u data segs, %u ACKs (%u delayed): %.2f ACKs/seg\n", segs, quick,
	       segs_in, acks, t1.tcpi_delayed_acks_out - t0.tcpi_delayed_acks_out, ratio);
	return ratio;
}

static void test_delack(void)
{
	double r1, r2, r4, rq;

	r2 = ack_ratio(5061, "2", "0");
	r1 = ack_ratio(5062, "1", "0");
	r4 = ack_ratio(5063, "4", "0");
	rq = ack_ratio(5064, "4", "1");
	rt_lock();
	rt_sysctl_set_text("net.inet.tcp.delack_segs", "2");
	rt_sysctl_set_text("net.inet.tcp.quickack", "0");
	rt_sysctl_set_text("net.inet.tcp.sack", "1");
	CHECK(rt_sysctl_set_text("net.inet.tcp.delack_segs", "0") != 0);
	CHECK(rt_sysctl_set_text("net.inet.tcp.delack_ms", "100") == 0);
	CHECK(rt_sysctl_set_text("net.inet.tcp.delack_ms", "200") == 0);
	rt_unlock();
	CHECKV(r1 >= 0.8, "delack_segs 1: %.2f", r1);
	CHECKV(rq >= 0.8, "quickack: %.2f", rq);
	CHECKV(r2 >= 0.4 && r2 <= 0.75, "delack_segs 2: %.2f", r2);
	CHECKV(r4 >= 0.18 && r4 <= 0.45, "delack_segs 4: %.2f", r4);
}

/* "ifconfig lo0 tcpwindow N": connections over lo0 offer at most N bytes
 * (both the active and the passive side), auto-tuning stays below it, and
 * connections opened after "tcpwindow 0" are not limited */
static int cfg(const char *line)
{
	char err[256];
	int r;

	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	return r;
}

static void test_tcpwindow(void)
{
	struct rt_tcp_info ti;
	struct rt_ctl_tcpinfo c, a;
	int lfd, cfd, afd;
	u64 total = 300000;
	double t0;

	CHECK(cfg("ifconfig lo0 tcpwindow 100") < 0);
	CHECK(cfg("ifconfig lo0 tcpwindow 1440") == 0);
	CHECK(rt_netif_find("lo0")->tcp_window == 1440);
	t0 = t_now();
	CHECK(pull(5071, total, &ti, &cfd, &afd, &lfd) == total);
	CHECK(find_tcpinfo(local_port(&cs, cfd), 5071, &c));
	CHECK(find_tcpinfo(5071, local_port(&cs, cfd), &a));
	CHECKV(c.rcv_wnd > 0 && c.rcv_wnd <= 1440 && c.rcv_ssthresh <= 1440, "client rcv_wnd %u ssthresh %u",
	       c.rcv_wnd, c.rcv_ssthresh);
	CHECKV(a.rcv_wnd > 0 && a.rcv_wnd <= 1440, "server rcv_wnd %u", a.rcv_wnd);
	printf("tcpwindow 1440: %llu bytes in %.2fs, client rcv_wnd %u rcvbuf %u\n", (unsigned long long)total,
	       t_now() - t0, c.rcv_wnd, c.rcvbuf);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);

	CHECK(cfg("ifconfig lo0 tcpwindow 0") == 0);
	CHECK(pull(5072, total, &ti, &cfd, &afd, &lfd) == total);
	CHECK(find_tcpinfo(local_port(&cs, cfd), 5072, &c));
	CHECKV(c.rcv_wnd > 1440, "unlimited rcv_wnd %u", c.rcv_wnd);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* listen backlog overflow: SYN cookies, nothing lost */
static void test_backlog(void)
{
	enum { N = 12 };
	int lfd, c[N], a[N], i, ok = 0;
	u32 cookies0 = rt_tcpstat.SyncookiesSent, recv0 = rt_tcpstat.SyncookiesRecv;
	struct rt_sockaddr_in d = t_sa(LO, 5015);
	char buf[8];

	lfd = listen_on(&ss, 5015, 1);
	for (i = 0; i < N; i++) {
		c[i] = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
		t_set_nbio(&cs, c[i], 1);
		rt_api_connect(&cs, c[i], &d, sizeof(d));
	}
	{
		int sent[N], got[N], na = 0;
		double t0 = t_now();
		memset(sent, 0, sizeof(sent));
		memset(got, 0, sizeof(got));
		for (i = 0; i < N; i++)
			a[i] = -1;
		t_set_nbio(&ss, lfd, 1);
		while (ok < N && t_now() - t0 < 20) {
			for (i = 0; i < N; i++) {
				char m[4];
				if (!sent[i]) {
					snprintf(m, sizeof(m), "%03d", i);
					if (rt_api_send(&cs, c[i], m, 3, 0) == 3)
						sent[i] = 1;
				}
			}
			while (na < N && (a[na] = rt_api_accept(&ss, lfd, NULL, NULL)) >= 0) {
				t_set_nbio(&ss, a[na], 1);
				na++;
			}
			for (i = 0; i < na; i++)
				if (!got[i] && rt_api_recv(&ss, a[i], buf, 3, 0) == 3) {
					got[i] = 1;
					ok++;
				}
			usleep(5000);
		}
		printf("backlog: all done after %.1fs\n", t_now() - t0);
	}
	printf("backlog 1, %d connects: %d accepted with data, cookies sent %u recv %u, "
	       "overflows %u\n", N, ok, rt_tcpstat.SyncookiesSent - cookies0,
	       rt_tcpstat.SyncookiesRecv - recv0, rt_tcpstat.ListenOverflows);
	CHECK(ok == N);
	CHECK(rt_tcpstat.SyncookiesSent > cookies0);
	for (i = 0; i < N; i++) {
		rt_api_close(&cs, c[i]);
		if (a[i] >= 0)
			rt_api_close(&ss, a[i]);
	}
	rt_api_close(&ss, lfd);
}

/* many concurrent connections */
static void test_many(void)
{
	enum { N = 200 };
	static int c[N], a[N];
	int lfd, i, ok = 0;
	struct rt_sockctx mc, ms;

	rt_ctx_init(&mc, N + 8, NULL);
	rt_ctx_init(&ms, N + 8, NULL);
	lfd = listen_on(&ms, 5016, 128);
	for (i = 0; i < N; i++) {
		struct rt_sockaddr_in d = t_sa(LO, 5016);
		c[i] = rt_api_socket(&mc, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_connect(&mc, c[i], &d, sizeof(d)) == 0);
		a[i] = accept_wait(&ms, lfd, 2000);
	}
	for (i = 0; i < N; i++) {
		char m[8];
		snprintf(m, sizeof(m), "%05d", i);
		rt_api_send(&mc, c[i], m, 5, 0);
	}
	for (i = 0; i < N; i++) {
		char r[8];
		if (a[i] >= 0 && rt_api_recv(&ms, a[i], r, 5, RT_MSG_WAITALL) == 5)
			ok++;
	}
	CHECKV(ok == N, "ok %d", ok);
	printf("many: %d connections, CurrEstab %u\n", ok, rt_tcpstat.CurrEstab);
	for (i = 0; i < N; i++) {
		rt_api_close(&mc, c[i]);
		rt_api_close(&ms, a[i]);
	}
	rt_api_close(&ms, lfd);
	rt_ctx_destroy(&mc);
	rt_ctx_destroy(&ms);
}

/* simultaneous open (RFC 9293 3.5 figure 7) */
static void test_simultaneous_open(void)
{
	int f1 = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	int f2 = rt_api_socket(&ss, RT_AF_INET, RT_SOCK_STREAM, 0);
	struct rt_sockaddr_in a1 = t_sa(LO, 5020), a2 = t_sa(LO, 5021);
	struct rt_tcp_info t1, t2;
	char buf[4];
	int i;
	u32 chal0 = rt_tcpstat.TCPChallengeACK;

	CHECK(rt_api_bind(&cs, f1, &a1, sizeof(a1)) == 0);
	CHECK(rt_api_bind(&ss, f2, &a2, sizeof(a2)) == 0);
	t_set_nbio(&cs, f1, 1);
	t_set_nbio(&ss, f2, 1);
	rt_lock();          /* both SYNs leave before either is delivered */
	rt_unlock();
	rt_api_connect(&cs, f1, &a2, sizeof(a2));
	rt_api_connect(&ss, f2, &a1, sizeof(a1));
	for (i = 0; i < 200; i++) {
		t_get_info(&cs, f1, &t1);
		t_get_info(&ss, f2, &t2);
		if (t1.tcpi_state == RT_TCPS_ESTABLISHED && t2.tcpi_state == RT_TCPS_ESTABLISHED)
			break;
		usleep(10000);
	}
	CHECKV(t1.tcpi_state == RT_TCPS_ESTABLISHED && t2.tcpi_state == RT_TCPS_ESTABLISHED,
	       "%u %u", t1.tcpi_state, t2.tcpi_state);
	CHECKV(rt_tcpstat.TCPChallengeACK == chal0, "challenge acks %u", rt_tcpstat.TCPChallengeACK - chal0);
	t_set_nbio(&cs, f1, 0);
	t_set_nbio(&ss, f2, 0);
	CHECK(rt_api_send(&cs, f1, "si", 2, 0) == 2);
	CHECK(rt_api_recv(&ss, f2, buf, 2, RT_MSG_WAITALL) == 2);
	/* simultaneous close */
	rt_api_shutdown(&cs, f1, 1);
	rt_api_shutdown(&ss, f2, 1);
	CHECK(rt_api_recv(&cs, f1, buf, 2, 0) == 0);
	CHECK(rt_api_recv(&ss, f2, buf, 2, 0) == 0);
	rt_api_close(&cs, f1);
	rt_api_close(&ss, f2);
}

/* options: TCP_INFO negotiation, sysctl-controlled option sets */
static void test_negotiation(void)
{
	int lfd, cfd, afd;
	struct rt_tcp_info ti;
	char name[16];
	u32 len = sizeof(name);

	CHECK(pair(5030, &lfd, &cfd, &afd));
	t_get_info(&cs, cfd, &ti);
	CHECKV(ti.tcpi_options == 7, "opts %x", ti.tcpi_options);   /* TS SACK WS */
	CHECK(rt_api_getsockopt(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, name, &len) == 0 &&
	      strcmp(name, "cubic") == 0);
	CHECK(rt_api_setsockopt(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, "reno", 4) == 0);
	len = sizeof(name);
	rt_api_getsockopt(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, name, &len);
	CHECK(strcmp(name, "reno") == 0);
	CHECK(rt_api_setsockopt(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, "bogus", 5) < 0);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);

	t_sysctl("net.inet.tcp.rfc1323", 0);
	CHECK(pair(5031, &lfd, &cfd, &afd));
	t_get_info(&cs, cfd, &ti);
	CHECKV(ti.tcpi_options == 2, "opts %x", ti.tcpi_options);   /* SACK only */
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
	t_sysctl("net.inet.tcp.rfc1323", 1);

	t_sysctl("net.inet.tcp.ecn", 1);
	CHECK(pair(5032, &lfd, &cfd, &afd));
	t_get_info(&cs, cfd, &ti);
	CHECKV(ti.tcpi_options == 0xf, "opts %x", ti.tcpi_options);  /* + ECN */
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
	t_sysctl("net.inet.tcp.ecn", 2);
}


/* orphaned FIN_WAIT_2 becomes a compact record, then TIME_WAIT */
static void test_orphan_fin_wait2(void)
{
	int lfd, cfd, afd;
	char buf[4];

	CHECK(pair(5040, &lfd, &cfd, &afd));
	rt_api_close(&cs, cfd);                    /* client: FIN_WAIT_1 -> 2, orphan */
	usleep(300000);
	CHECKV(count_state(5040, RT_TCPS_FIN_WAIT_2) == 1, "fw2 %d", count_state(5040, RT_TCPS_FIN_WAIT_2));
	CHECK(rt_api_recv(&ss, afd, buf, 4, 0) == 0);
	rt_api_close(&ss, afd);                    /* FIN reaches the record */
	usleep(100000);
	CHECK(count_state(5040, RT_TCPS_FIN_WAIT_2) == 0);
	CHECK(count_state(5040, RT_TCPS_TIME_WAIT) == 1);
	rt_api_close(&ss, lfd);

	/* orphan whose peer never closes: fin_timeout reaps it */
	t_sysctl("net.inet.tcp.fin_timeout", 1);
	CHECK(pair(5041, &lfd, &cfd, &afd));
	rt_api_close(&cs, cfd);
	usleep(300000);
	CHECK(count_state(5041, RT_TCPS_FIN_WAIT_2) == 1);
	sleep(2);
	CHECK(count_state(5041, RT_TCPS_FIN_WAIT_2) == 0);
	t_sysctl("net.inet.tcp.fin_timeout", 60);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* RFC 6191: a SYN with a newer timestamp reopens a TIME_WAIT 4-tuple;
 * tw_reuse lets connect() reuse its own TIME_WAIT 4-tuple. */
static void test_tw_reopen(void)
{
	struct rt_sockaddr_in me = t_sa(LO, 5051), d = t_sa(LO, 5050);
	int lfd = listen_on(&ss, 5050, 5), cfd, afd, r;
	char buf[4];
	double t0;

	/* server closes first: TIME_WAIT on the server side */
	cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&cs, cfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&cs, cfd, &me, sizeof(me)) == 0);
	CHECK(rt_api_connect(&cs, cfd, &d, sizeof(d)) == 0);
	afd = accept_wait(&ss, lfd, 2000);
	rt_api_close(&ss, afd);
	CHECK(rt_api_recv(&cs, cfd, buf, 4, 0) == 0);
	rt_api_close(&cs, cfd);
	usleep(50000);
	CHECK(count_state(5050, RT_TCPS_TIME_WAIT) == 1);
	cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&cs, cfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&cs, cfd, &me, sizeof(me)) == 0);
	t0 = t_now();
	r = rt_api_connect(&cs, cfd, &d, sizeof(d));
	CHECKV(r == 0 && t_now() - t0 < 0.5, "r=%d t=%.2f", r, t_now() - t0);
	afd = accept_wait(&ss, lfd, 2000);
	CHECK(afd >= 0);
	printf("SYN to TIME_WAIT accepted in %.3fs\n", t_now() - t0);

	/* now the client closes first and reconnects from the same port */
	rt_api_close(&cs, cfd);
	CHECK(rt_api_recv(&ss, afd, buf, 4, 0) == 0);
	rt_api_close(&ss, afd);
	usleep(50000);
	sleep(1);           /* tw_reuse needs the TIME_WAIT to be >= 1 s old */
	cfd = rt_api_socket(&cs, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&cs, cfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&cs, cfd, &me, sizeof(me)) == 0);
	r = rt_api_connect(&cs, cfd, &d, sizeof(d));
	CHECKV(r == 0, "tw_reuse connect r=%d", r);
	afd = accept_wait(&ss, lfd, 2000);
	CHECK(afd >= 0);
	CHECK(rt_tcpstat.TimeWaitRecycled >= 1);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

/* TCP_NOPUSH (cork) holds partial segments; TCP_NODELAY pushes */
static void test_cork(void)
{
	int lfd, cfd, afd, r;
	char buf[64];

	CHECK(pair(5060, &lfd, &cfd, &afd));
	t_set_int(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_NOPUSH, 1);
	CHECK(rt_api_send(&cs, cfd, "corked", 6, 0) == 6);
	t_set_nbio(&ss, afd, 1);
	usleep(50000);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == -RT_EWOULDBLOCK, "r=%d", r);
	t_set_int(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_NOPUSH, 0);
	usleep(50000);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == 6, "r=%d", r);
	t_set_int(&cs, cfd, RT_IPPROTO_TCP, RT_TCP_NODELAY, 1);
	for (r = 0; r < 10; r++)
		rt_api_send(&cs, cfd, "x", 1, 0);
	usleep(50000);
	r = rt_api_recv(&ss, afd, buf, sizeof(buf), 0);
	CHECKV(r == 10, "r=%d", r);
	rt_api_close(&cs, cfd);
	rt_api_close(&ss, afd);
	rt_api_close(&ss, lfd);
}

int main(int argc, char **argv)
{
	u64 big = 20u << 20;

	if (argc > 1)
		big = strtoull(argv[1], NULL, 0);
	alarm(600);
	if (rt_host_start() != 0) {
		printf("cannot start stack\n");
		return 1;
	}
	rt_ctx_init(&cs, 64, NULL);
	rt_ctx_init(&ss, 64, NULL);

	test_refused();
	test_bulk(5001, 100000, 0);
	test_bulk(5002, big, 0);
	test_bulk(5003, big / 4, 1);
	test_negotiation();
	test_half_close();
	test_resets();
	test_time_wait();
	test_oob();
	test_zero_window();
	test_tcpinfo();
	test_delack();
	test_tcpwindow();
	test_backlog();
	test_many();
	test_simultaneous_open();
	test_orphan_fin_wait2();
	test_tw_reopen();
	test_cork();
	{
		struct rt_buf_stats bs;
		usleep(500000);
		rt_lock();
		rt_buf_get_stats(&bs);
		rt_unlock();
		printf("buffers still allocated: %u (peak %u)\n", bs.allocated, bs.peak);
		CHECK(bs.allocated < 8);
	}
	t_print_stats("loop");

	rt_ctx_destroy(&cs);
	rt_ctx_destroy(&ss);
	rt_host_stop();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
