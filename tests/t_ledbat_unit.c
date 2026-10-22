/*
 * Riptide - background transfers and socket memory limits:
 *   - the LEDBAT++ controller in a fluid model of a bottleneck (a link of
 *     C segments per ms, a base RTT, a drop-tail queue): alone it keeps
 *     the queueing delay near the target and the link busy; against a
 *     Reno flow it yields; periodic slowdowns; loss handling;
 *   - rLEDBAT on a bare tcp_sock: the window grows with low delay,
 *     shrinks with high delay, and __tcp_select_window() follows it;
 *   - the "background" keyword and sysctls; rules by program name and
 *     local port on loopback connections; TCP_BACKGROUND; a bulk
 *     transfer over a background connection;
 *   - net.inet.tcp.mem_*: many connections that nobody reads stay under
 *     the limit, the data is intact once read, the state recovers.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"
#include "../src/core/tcp.h"

/* ---------------------------------------------------------------- model */
struct flow {
	int ledbat;
	struct ledbat lb;
	double w, ssthresh;          /* Reno, segments */
	double acc;                  /* fractional acked, segments * 256 */
	double delivered;            /* segments in the measured interval */
	u32 last_loss;
	int slowdowns;
	u8 last_phase;
};

struct link {
	double cap;                  /* segments per ms */
	double base;                 /* ms */
	double qmax;                 /* segments */
	double q;
	double qsum;                 /* queue integral over the measured interval */
	u32 qn;
};

static double fw(const struct flow *f)
{
	return f->ledbat ? f->lb.wnd / 256.0 : f->w;
}

/* run nf flows for ms milliseconds starting at time t0; measure from 'from' */
static u32 sim(struct link *L, struct flow *f, int nf, u32 t0, u32 ms, u32 from)
{
	u32 t;
	int i;

	for (t = t0; t < t0 + ms; t++) {
		double rtt = L->base + L->q / L->cap, rate[4], total = 0, delivered;
		int loss = 0;
		for (i = 0; i < nf; i++) {
			rate[i] = fw(&f[i]) / rtt;
			total += rate[i];
		}
		L->q += total - L->cap;
		if (L->q < 0) {
			delivered = total;
			L->q = 0;
		} else {
			delivered = L->cap;
		}
		if (L->q > L->qmax) {
			L->q = L->qmax;
			loss = 1;
		}
		if (t >= from) {
			L->qsum += L->q / L->cap;
			L->qn++;
		}
		for (i = 0; i < nf; i++) {
			struct flow *x = &f[i];
			double d = total > 0 ? rate[i] * delivered / total : 0;
			if (t >= from)
				x->delivered += d;
			if (x->ledbat) {
				u32 n;
				ledbat_rtt(&x->lb, (u32)(rtt * 1000.0), t);
				x->acc += d * 256.0;
				n = (u32)x->acc;
				x->acc -= n;
				if (loss && t - x->last_loss > (u32)rtt) {
					ledbat_loss(&x->lb, t);
					x->last_loss = t;
				}
				ledbat_acked(&x->lb, n, t, 1);
				if (x->lb.phase == LEDBAT_SLOW && x->last_phase != LEDBAT_SLOW)
					x->slowdowns++;
				x->last_phase = x->lb.phase;
			} else {
				if (loss && t - x->last_loss > (u32)rtt) {
					x->w = x->w / 2 > 2 ? x->w / 2 : 2;
					x->ssthresh = x->w;
					x->last_loss = t;
				} else if (x->w < x->ssthresh) {
					x->w += d;
				} else {
					x->w += d / x->w;
				}
			}
		}
	}
	return t;
}

static void flow_init(struct flow *f, int ledbat)
{
	memset(f, 0, sizeof(*f));
	f->ledbat = ledbat;
	f->w = 10;
	f->ssthresh = 1e9;
	ledbat_init(&f->lb, 10, 0);
}

static void test_controller(void)
{
	struct link L;
	struct flow f[2];
	u32 t;
	double util, qavg;

	/* LEDBAT alone: 12 Mbit/s (1 segment per ms), 20 ms base RTT, 500 ms of buffer */
	memset(&L, 0, sizeof(L));
	L.cap = 1.0;
	L.base = 20;
	L.qmax = 500;
	flow_init(&f[0], 1);
	sim(&L, f, 1, 1, 60000, 20000);
	util = f[0].delivered / (40000 * L.cap);
	qavg = L.qsum / L.qn;
	printf("ledbat alone: utilisation %.0f%%, queue delay %.1f ms (target %ld), base %u us, %d slowdowns\n",
	       util * 100, qavg, (long)sysctl_tcp_ledbat_target, ledbat_base(&f[0].lb), f[0].slowdowns);
	CHECKV(util > 0.80, "utilisation %.2f", util);
	CHECKV(qavg > 5 && qavg < 1.3 * sysctl_tcp_ledbat_target, "queue %.1f ms", qavg);
	CHECKV(f[0].slowdowns >= 2, "%d slowdowns", f[0].slowdowns);
	CHECKV(ledbat_base(&f[0].lb) >= 19000 && ledbat_base(&f[0].lb) <= 22000, "base %u",
	       ledbat_base(&f[0].lb));

	/* Reno alone fills the buffer (the bufferbloat LEDBAT avoids) */
	memset(&L, 0, sizeof(L));
	L.cap = 1.0;
	L.base = 20;
	L.qmax = 500;
	flow_init(&f[0], 0);
	sim(&L, f, 1, 1, 60000, 20000);
	qavg = L.qsum / L.qn;
	printf("reno alone: queue delay %.1f ms\n", qavg);
	CHECKV(qavg > 150, "queue %.1f ms", qavg);

	/* LEDBAT against Reno: Reno gets nearly everything */
	memset(&L, 0, sizeof(L));
	L.cap = 1.0;
	L.base = 20;
	L.qmax = 500;
	flow_init(&f[0], 1);
	flow_init(&f[1], 0);
	sim(&L, f, 2, 1, 60000, 20000);
	printf("ledbat vs reno: ledbat %.1f%%, reno %.1f%% of the link\n", f[0].delivered / 400.0,
	       f[1].delivered / 400.0);
	CHECKV(f[1].delivered > 6 * f[0].delivered, "reno %.0f ledbat %.0f", f[1].delivered, f[0].delivered);

	/* LEDBAT first, Reno joins later: LEDBAT backs off within seconds */
	memset(&L, 0, sizeof(L));
	L.cap = 1.0;
	L.base = 20;
	L.qmax = 500;
	flow_init(&f[0], 1);
	t = sim(&L, f, 1, 1, 30000, 1);
	flow_init(&f[1], 0);
	f[0].delivered = 0;
	L.qsum = 0;
	L.qn = 0;
	sim(&L, f, 2, t, 30000, t + 10000);
	printf("reno joins: ledbat %.1f%%, reno %.1f%%\n", f[0].delivered / 200.0, f[1].delivered / 200.0);
	CHECKV(f[1].delivered > 5 * f[0].delivered, "reno %.0f ledbat %.0f", f[1].delivered, f[0].delivered);

	/* without slowdowns there are none */
	sysctl_tcp_ledbat_slowdown = 0;
	memset(&L, 0, sizeof(L));
	L.cap = 1.0;
	L.base = 20;
	L.qmax = 500;
	flow_init(&f[0], 1);
	sim(&L, f, 1, 1, 30000, 1);
	CHECK(f[0].slowdowns == 0);
	sysctl_tcp_ledbat_slowdown = 1;

	/* a loss halves the window once per round */
	{
		struct ledbat lb;
		u32 w;
		ledbat_init(&lb, 10, 1000);
		ledbat_rtt(&lb, 50000, 1000);
		ledbat_acked(&lb, 256, 1000, 1);
		lb.wnd = 100 * 256;
		w = lb.wnd;
		ledbat_loss(&lb, 1001);
		CHECK(lb.wnd == w / 2);
		ledbat_loss(&lb, 1002);
		CHECK(lb.wnd == w / 2);
		CHECK(lb.phase == LEDBAT_CA);
		/* the next round may halve again */
		ledbat_acked(&lb, 256, 1100, 1);
		ledbat_loss(&lb, 1101);
		CHECKV(lb.wnd <= w / 4 + 256, "wnd %u", lb.wnd);
		/* never below two segments */
		lb.wnd = 3 * 256;
		lb.dec_done = 0;
		ledbat_loss(&lb, 1300);
		CHECK(lb.wnd == 2 * 256);
	}
}

/* ---------------------------------------------------------------- rLEDBAT */
static void test_rledbat_window(void)
{
	struct tcp_sock *tp = calloc(1, sizeof(*tp));
	u32 i, w0, w1, w2, now;

	rt_lock();
	tp->advmss = 1448;
	tp->rcvbuf = 262144;
	tp->rx_opt.tstamp_ok = 1;
	tp->state = RT_TCPS_ESTABLISHED;
	tcp_rledbat_start(tp);
	CHECK(tp->rl != NULL);
	w0 = tcp_rledbat_window(tp);
	CHECKV(w0 == 10 * 1448, "w0 %u", w0);
	/* low delay (20 ms, no queue): the window opens */
	for (i = 0; i < 400; i++) {
		tcp_rledbat_rtt(tp, 20000);
		tcp_rledbat_data(tp, 1448);
	}
	w1 = tcp_rledbat_window(tp);
	printf("rLEDBAT: %u -> %u bytes with 20 ms RTT\n", w0, w1);
	CHECK(w1 > 2 * w0);
	/* a queue builds: 200 ms over the base (the decrease is once per
	 * round, so let time pass between the samples) */
	now = rt_now_ms();
	for (i = 0; i < 20; i++) {
		tcp_rledbat_rtt(tp, 220000);
		tcp_rledbat_data(tp, 1448);
		rt_host_advance_clock(250);
	}
	w2 = tcp_rledbat_window(tp);
	printf("rLEDBAT: %u bytes after 20 rounds at 220 ms (%u ms)\n", w2, rt_now_ms() - now);
	CHECK(w2 <= 4 * 1448);
	/* __tcp_select_window() stays within it */
	tp->rcv_ssthresh = 262144;
	tp->window_clamp = 262144;
	tp->ack.rcv_mss = 1448;
	tp->rcv_wnd = 2 * 1448;
	CHECKV(__tcp_select_window(tp) <= RT_MAX(w2, 2u * 1448), "window %u", __tcp_select_window(tp));
	tcp_rledbat_stop(tp);
	CHECK(tp->rl == NULL);
	rt_unlock();
	free(tp);
}

/* ---------------------------------------------------------------- policy */
static struct rt_sockctx ctx;

static int conf(const char *line, char *err, u32 len)
{
	int r;
	rt_lock();
	r = rt_config_line(line, err, len, NULL);
	rt_unlock();
	return r;
}

static void sysctl_text(const char *name, char *buf, u32 len)
{
	const struct rt_sysctl *e;
	rt_lock();
	e = rt_sysctl_find(name);
	buf[0] = 0;
	if (e)
		rt_sysctl_get_text(e, buf, len);
	rt_unlock();
}

static int get_int(int fd, int level, int name)
{
	int v = -1;
	u32 len = sizeof(v);
	rt_api_getsockopt(&ctx, fd, level, name, &v, &len);
	return v;
}

static void get_cc(int fd, char *buf)
{
	u32 len = 16;
	memset(buf, 0, 16);
	rt_api_getsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, buf, &len);
}

/* the net.riptide.tcpinfo record of the connection with local port lport */
static int tcpinfo(u16 lport, struct rt_ctl_tcpinfo *out)
{
	static struct rt_ctl_tcpinfo recs[64];
	int n, i;
	rt_lock();
	n = rt_tcp_info_list(recs, 64);
	rt_unlock();
	for (i = 0; i < n && i < 64; i++)
		if (recs[i].lport == lport && recs[i].state == RT_TCPS_ESTABLISHED) {
			*out = recs[i];
			return 1;
		}
	return 0;
}

static int listener(u16 port)
{
	struct rt_sockaddr_in a = t_sa(0x7f000001u, port);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	t_set_int(&ctx, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
	CHECK(rt_api_bind(&ctx, fd, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&ctx, fd, 8) == 0);
	return fd;
}

/* a connection from a program called prog; returns the client fd, *afd the accepted one */
static int connect_as(const char *prog, u16 port, int lfd, int *afd, const char *cc)
{
	struct rt_sockaddr_in d = t_sa(0x7f000001u, port);
	int fd;

	rt_host_set_program(prog);
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	rt_host_set_program("");
	if (cc)
		rt_api_setsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, cc, (u32)strlen(cc));
	CHECK(rt_api_connect(&ctx, fd, &d, sizeof(d)) == 0);
	*afd = rt_api_accept(&ctx, lfd, NULL, NULL);
	CHECK(*afd >= 0);
	return fd;
}

struct sender {
	int fd;
	u64 total;
	u32 seed;
	u64 sent;
	int rfd;                 /* the receiving end (reader threads) */
	u64 got, bad;
};

static void *send_thread(void *p)
{
	struct sender *s = p;
	s->sent = t_send_pattern(&ctx, s->fd, s->total, s->seed, 16384);
	rt_api_shutdown(&ctx, s->fd, 1);
	return NULL;
}

static void *recv_thread(void *p)
{
	struct sender *s = p;
	s->got = t_recv_pattern(&ctx, s->rfd, s->seed, &s->bad, 16384);
	return NULL;
}

static void test_policy(void)
{
	char err[200], buf[160], cc[16];
	struct rt_ctl_tcpinfo ti;
	int lfd, lfd2, fd, afd, r;

	/* the keyword and the sysctls */
	r = conf("background program=aget,wget* port=20,21,6881-6889 lport=9300", err, sizeof(err));
	CHECKV(r == 0, "%s", err);
	sysctl_text("net.inet.tcp.bg_programs", buf, sizeof(buf));
	CHECKV(!strcmp(buf, "aget,wget*"), "%s", buf);
	sysctl_text("net.inet.tcp.bg_ports", buf, sizeof(buf));
	CHECKV(!strcmp(buf, "20,21,6881-6889"), "%s", buf);
	CHECK(conf("background port=70000", err, sizeof(err)) == -RT_EINVAL);
	CHECK(conf("background port=9-5", err, sizeof(err)) == -RT_EINVAL);
	CHECK(conf("background colour=red", err, sizeof(err)) == -RT_EINVAL);
	CHECK(conf("background mode=sometimes", err, sizeof(err)) == -RT_EINVAL);
	CHECK(conf("sysctl net.inet.tcp.bg_lports=1-2-3", err, sizeof(err)) < 0);
	sysctl_text("net.inet.tcp.bg_ports", buf, sizeof(buf));
	CHECKV(!strcmp(buf, "20,21,6881-6889"), "unchanged after errors: %s", buf);
	CHECK(conf("background", err, sizeof(err)) == 0 && strstr(err, "mode both") && strstr(err, "aget"));
	/* the forms AmigaDOS ReadArgs makes of "key=value" */
	CHECK(conf("background program xx port = 80 lport=", err, sizeof(err)) == 0);
	sysctl_text("net.inet.tcp.bg_programs", buf, sizeof(buf));
	CHECKV(!strcmp(buf, "xx"), "%s", buf);
	sysctl_text("net.inet.tcp.bg_ports", buf, sizeof(buf));
	CHECKV(!strcmp(buf, "80"), "%s", buf);
	sysctl_text("net.inet.tcp.bg_lports", buf, sizeof(buf));
	CHECKV(!buf[0], "%s", buf);
	CHECK(conf("background program", err, sizeof(err)) == -RT_EINVAL);
	CHECK(conf("background program=aget,wget* port=20,21,6881-6889 lport=9300", err, sizeof(err)) == 0);
	CHECK(conf("sysctl net.inet.tcp.cc=ledbat", err, sizeof(err)) == 0);
	CHECK(conf("sysctl net.inet.tcp.cc=cubic", err, sizeof(err)) == 0);

	lfd = listener(9301);
	/* a background program, case-insensitive */
	fd = connect_as("AGET", 9301, lfd, &afd, NULL);
	CHECKV(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 3, "%d",
	       get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND));
	get_cc(fd, cc);
	CHECKV(!strcmp(cc, "ledbat"), "%s", cc);
	CHECK(get_int(afd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 0);
	{
		struct rt_sockaddr_in me;
		u32 l = sizeof(me);
		rt_api_getsockname(&ctx, fd, &me, &l);
		CHECK(tcpinfo(rt_ntohs(me.sin_port), &ti) && (ti.bg & RT_CTL_TCPBG_RL_ON) &&
		      (ti.bg & RT_CTL_TCPBG_RULE) && ti.rledbat_wnd > 0);
	}
	rt_api_close(&ctx, fd);
	rt_api_close(&ctx, afd);

	/* a prefix pattern; the program's own TCP_CONGESTION wins for sending */
	fd = connect_as("wgetX", 9301, lfd, &afd, "cubic");
	CHECK(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 3);
	get_cc(fd, cc);
	CHECKV(!strcmp(cc, "cubic"), "%s", cc);
	rt_api_close(&ctx, fd);
	rt_api_close(&ctx, afd);

	/* another program: normal */
	fd = connect_as("ftp", 9301, lfd, &afd, NULL);
	CHECK(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 0);
	get_cc(fd, cc);
	CHECKV(!strcmp(cc, "cubic"), "%s", cc);
	/* TCP_BACKGROUND on a live connection */
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND, 1) == 0);
	CHECK(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 1);
	get_cc(fd, cc);
	CHECK(!strcmp(cc, "cubic"));
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND, 3) == 0);
	get_cc(fd, cc);
	CHECK(!strcmp(cc, "ledbat"));
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND, 0) == 0);
	get_cc(fd, cc);
	CHECK(!strcmp(cc, "cubic"));
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND, 4) == -RT_EINVAL);
	rt_api_close(&ctx, fd);
	rt_api_close(&ctx, afd);
	rt_api_close(&ctx, lfd);

	/* the passive side by local port; a bulk transfer over it */
	lfd2 = listener(9300);
	fd = connect_as("", 9300, lfd2, &afd, NULL);
	CHECK(get_int(afd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 3);
	CHECK(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 0);
	{
		struct sender s = { fd, 4u << 20, 0x1edba7, 0, -1, 0, 0 };
		pthread_t th;
		u64 bad, got;
		double t0 = t_now();
		pthread_create(&th, NULL, send_thread, &s);
		got = t_recv_pattern(&ctx, afd, s.seed, &bad, 8192);
		pthread_join(th, NULL);
		CHECKV(got == s.total && bad == 0, "got %llu bad %llu", (unsigned long long)got,
		       (unsigned long long)bad);
		printf("background loopback transfer: 4 MB in %.2f s\n", t_now() - t0);
	}
	rt_api_close(&ctx, fd);
	rt_api_close(&ctx, afd);
	rt_api_close(&ctx, lfd2);

	/* mode=off: the rules are kept but not applied */
	CHECK(conf("background mode=off", err, sizeof(err)) == 0);
	lfd = listener(9301);
	fd = connect_as("aget", 9301, lfd, &afd, NULL);
	CHECK(get_int(fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND) == 0);
	rt_api_close(&ctx, fd);
	rt_api_close(&ctx, afd);
	rt_api_close(&ctx, lfd);
	CHECK(conf("background clear", err, sizeof(err)) == 0);
	CHECK(conf("background mode=both", err, sizeof(err)) == 0);
	sysctl_text("net.inet.tcp.bg_programs", buf, sizeof(buf));
	CHECK(buf[0] == 0);
}

/* ---------------------------------------------------------------- memory */
#define NCONN 12

static s32 sysctl_int(const char *name)
{
	s32 v = -1;
	u32 l = sizeof(v);
	rt_lock();
	rt_sysctl_byname(name, &v, &l, NULL, 0);
	rt_unlock();
	return v;
}

static u32 fill_and_measure(int limit, u32 *drops)
{
	struct sender s[NCONN];
	pthread_t th[NCONN], rth[NCONN];
	int fds[NCONN], afds[NCONN], lfd, i;
	u32 peak = 0, d0 = rt_tcpstat.TCPMemLimitDrops;
	double t0;

	lfd = listener(9310);
	for (i = 0; i < NCONN; i++) {
		fds[i] = connect_as("", 9310, lfd, &afds[i], NULL);
		s[i].fd = fds[i];
		s[i].rfd = afds[i];
		s[i].total = 600u << 10;
		s[i].seed = 0x3e3 + (u32)i;
		pthread_create(&th[i], NULL, send_thread, &s[i]);
	}
	/* nobody reads for a while: the queues fill */
	t0 = t_now();
	while (t_now() - t0 < 3.0) {
		u32 kb = (u32)sysctl_int("net.inet.tcp.mem_used");
		if (kb > peak) {
			static struct rt_ctl_tcpinfo recs[64];
			int n, k;
			u32 sq = 0, rq = 0, sb = 0, rb = 0;
			peak = kb;
			rt_lock();
			n = rt_tcp_info_list(recs, 64);
			rt_unlock();
			for (k = 0; k < n && k < 64; k++) {
				sq += recs[k].sendq;
				rq += recs[k].recvq;
				sb += recs[k].sndbuf;
				rb += recs[k].rcvbuf;
			}
			if (getenv("T_DEBUG"))
				printf("  peak %u KB: sendq %u recvq %u sndbuf %u rcvbuf %u\n", kb, sq >> 10, rq >> 10,
				       sb >> 10, rb >> 10);
		}
		usleep(20000);
	}
	printf("%s: %d connections nobody reads: up to %u KB in socket buffers (state %d)\n",
	       limit ? "with limits" : "no limits", NCONN, peak, (int)sysctl_int("net.inet.tcp.mem_state"));
	t0 = t_now();
	/* now everybody reads at once */
	for (i = 0; i < NCONN; i++)
		pthread_create(&rth[i], NULL, recv_thread, &s[i]);
	for (i = 0; i < NCONN; i++) {
		pthread_join(rth[i], NULL);
		CHECKV(s[i].got == s[i].total && s[i].bad == 0, "conn %d: got %llu bad %llu", i,
		       (unsigned long long)s[i].got, (unsigned long long)s[i].bad);
		pthread_join(th[i], NULL);
		rt_api_close(&ctx, fds[i]);
		rt_api_close(&ctx, afds[i]);
	}
	rt_api_close(&ctx, lfd);
	printf("  read in %.1f s\n", t_now() - t0);
	*drops = rt_tcpstat.TCPMemLimitDrops - d0;
	return peak;
}

static void test_memory(void)
{
	char err[160];
	u32 peak, drops, p0 = rt_tcpstat.TCPMemoryPressures;

	/* defaults follow the free memory */
	rt_host_set_mem_avail(8u << 20, 0);
	rt_lock();
	tcp_mem_init();
	rt_unlock();
	printf("8 MB free: low %ld pressure %ld high %ld KB\n", (long)sysctl_tcp_mem_low,
	       (long)sysctl_tcp_mem_pressure, (long)sysctl_tcp_mem_high);
	CHECK(sysctl_tcp_mem_high == 2048 && sysctl_tcp_mem_pressure == 1365 && sysctl_tcp_mem_low >= 1000 && sysctl_tcp_mem_low < 1365);
	rt_host_set_mem_avail(1500u << 10, 1);
	rt_lock();
	tcp_mem_init();
	rt_unlock();
	printf("1.5 MB Chip RAM: low %ld pressure %ld high %ld KB\n", (long)sysctl_tcp_mem_low,
	       (long)sysctl_tcp_mem_pressure, (long)sysctl_tcp_mem_high);
	CHECK(sysctl_tcp_mem_high == 311 && sysctl_tcp_mem_pressure < sysctl_tcp_mem_high &&
	      sysctl_tcp_mem_low < sysctl_tcp_mem_pressure);
	rt_host_set_mem_avail(0, 0);

	/* Ethernet-sized segments (loopback has 16 KB ones) */
	CHECK(conf("ifconfig lo0 mtu 1500", err, sizeof(err)) == 0);
	/* without limits (the host defaults are 16/24/32 MB) */
	conf("sysctl net.inet.tcp.mem_low=16384", err, sizeof(err));
	conf("sysctl net.inet.tcp.mem_pressure=24576", err, sizeof(err));
	conf("sysctl net.inet.tcp.mem_high=32768", err, sizeof(err));
	peak = fill_and_measure(0, &drops);
	CHECKV(peak > 900, "control: %u KB", peak);
	/* 1 MB: the use stays near it (a connection below its 8 KB minimum
	 * per direction may still fill that, plus a segment) */
	CHECK(conf("sysctl net.inet.tcp.mem_low=512", err, sizeof(err)) == 0);
	CHECK(conf("sysctl net.inet.tcp.mem_pressure=768", err, sizeof(err)) == 0);
	CHECK(conf("sysctl net.inet.tcp.mem_high=1024", err, sizeof(err)) == 0);
	peak = fill_and_measure(1, &drops);
	CHECKV(peak <= 1024 + NCONN * 2 * (8 + 2), "%u KB", peak);
	CHECK(rt_tcpstat.TCPMemoryPressures > p0);
	printf("memory limit drops: %u, pressure periods %u, %u ms\n", drops,
	       rt_tcpstat.TCPMemoryPressures - p0, rt_tcpstat.TCPMemoryPressuresMs);
	/* all gone: back to normal */
	usleep(50000);
	CHECKV(sysctl_int("net.inet.tcp.mem_state") == 0, "state %d", (int)sysctl_int("net.inet.tcp.mem_state"));
	CHECK(sysctl_int("net.inet.tcp.mem_used") < 64);
	CHECK(conf("sysctl net.inet.tcp.mem_used=5", err, sizeof(err)) < 0);
	conf("sysctl net.inet.tcp.mem_low=16384", err, sizeof(err));
	conf("sysctl net.inet.tcp.mem_pressure=24576", err, sizeof(err));
	conf("sysctl net.inet.tcp.mem_high=32768", err, sizeof(err));
	conf("ifconfig lo0 mtu 16384", err, sizeof(err));
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(300);
	if (rt_host_start() != 0)
		return 1;
	test_controller();
	test_rledbat_window();
	rt_ctx_init(&ctx, 64, NULL);
	test_policy();
	test_memory();
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
