/*
 * Riptide - background transfers against the Linux kernel over TUN
 * (needs root, /dev/net/tun and iproute2; exits 77 = skip otherwise).
 *
 *   Linux 10.83.1.1/30 <-- tun --> Riptide 10.83.1.2
 *
 * Downloads (Linux sends, Riptide receives): Riptide reads the tun
 * device at 8 Mbit/s (rt_host_tun_rx_rate), so the kernel's tun queue,
 * 300 packets (about 450 ms), is the bottleneck queue. (A tbf qdisc on
 * the tun device does not work: TCP Small Queues keep a Linux sender
 * from filling a qdisc on its own host; the tun queue holds orphaned
 * packets.) Riptide
 * delays what it sends by 10 ms (the base RTT). Both receivers offer a
 * 600 KB window (SO_RCVBUF). A normal download fills the queue; a
 * background one (rLEDBAT, chosen by a "background lport=" rule) keeps
 * the RTT near the base plus the 60 ms target and still uses most of the
 * link; both together: the normal one gets most of it.
 *
 * Uploads (Riptide sends): Riptide's own transmit rate limit is the
 * bottleneck (8 Mbit/s, 300 packets of queue, 10 ms delay); LEDBAT
 * (TCP_CONGESTION "ledbat") against CUBIC.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <arpa/inet.h>
#include "t_tcp_common.h"

#define OUR_IP   0x0a530102u   /* 10.83.1.2 */
#define LINUX_IP 0x0a530101u   /* 10.83.1.1 */
#define RATE_KBIT 8000

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

/* one flow; counters read by the main thread */
struct flow {
	const char *name;
	u16 port;
	int riptide_sends;           /* upload */
	const char *cc;              /* Riptide's TCP_CONGESTION (upload) */
	double secs;
	volatile u64 bytes;          /* received so far */
	int lfd, rfd;                /* Linux listener (upload) / Riptide listener (download) */
	pthread_t th[2];
	struct tcp_info lx;          /* Linux's view at the end */
	struct rt_tcp_info rt;
	int bg;                      /* TCP_BACKGROUND of the Riptide socket */
	struct rt_ctl_tcpinfo rti;   /* net.riptide.tcpinfo at the end */
	int have_rti;
};

static void lx_info(int fd, struct tcp_info *ti)
{
	socklen_t l = sizeof(*ti);
	memset(ti, 0, sizeof(*ti));
	getsockopt(fd, IPPROTO_TCP, TCP_INFO, ti, &l);
}

static void rt_info_of(int fd, struct flow *f)
{
	struct rt_sockaddr_in me;
	static struct rt_ctl_tcpinfo recs[32];
	u32 l = sizeof(me);
	int n, i, v = -1;
	u32 vl = sizeof(v);

	t_get_info(&ctx, fd, &f->rt);
	rt_api_getsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_BACKGROUND, &v, &vl);
	f->bg = v;
	rt_api_getsockname(&ctx, fd, &me, &l);
	rt_lock();
	n = rt_tcp_info_list(recs, 32);
	rt_unlock();
	for (i = 0; i < n && i < 32; i++)
		if (recs[i].lport == rt_ntohs(me.sin_port) && recs[i].state == RT_TCPS_ESTABLISHED) {
			f->rti = recs[i];
			f->have_rti = 1;
		}
}

/* Linux sends for f->secs (download) */
static void *lx_sender(void *p)
{
	struct flow *f = p;
	static __thread u8 buf[65536];
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	double end;

	/* a loss-based server (this kernel may default to BBR, which keeps
	 * no standing queue: then there is nothing to yield to) */
	setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "cubic", 5);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(OUR_IP);
	a.sin_port = htons(f->port);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("connect");
		close(fd);
		return NULL;
	}
	memset(buf, 0x5a, sizeof(buf));
	end = t_now() + f->secs;
	while (t_now() < end)
		if (send(fd, buf, sizeof(buf), MSG_NOSIGNAL) <= 0)
			break;
	lx_info(fd, &f->lx);
	shutdown(fd, SHUT_WR);
	{
		char x;
		recv(fd, &x, 1, 0);
	}
	close(fd);
	return NULL;
}

/* Riptide receives (download) */
static void *rt_receiver(void *p)
{
	struct flow *f = p;
	static __thread u8 buf[32768];
	int fd = rt_api_accept(&ctx, f->rfd, NULL, NULL);
	int r, info = 0;
	double half = t_now() + f->secs * 0.8;

	if (fd < 0)
		return NULL;
	while ((r = rt_api_recv(&ctx, fd, buf, sizeof(buf), 0)) > 0) {
		f->bytes += (u64)r;
		if (!info && t_now() > half) {
			rt_info_of(fd, f);
			info = 1;
		}
	}
	rt_api_close(&ctx, fd);
	return NULL;
}

/* Riptide sends for f->secs (upload) */
static void *rt_sender(void *p)
{
	struct flow *f = p;
	static __thread u8 buf[32768];
	struct rt_sockaddr_in d = t_sa(LINUX_IP, f->port);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0), info = 0;
	double end, half;

	if (f->cc)
		rt_api_setsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CONGESTION, f->cc, (u32)strlen(f->cc));
	if (rt_api_connect(&ctx, fd, &d, sizeof(d)) != 0) {
		rt_api_close(&ctx, fd);
		return NULL;
	}
	memset(buf, 0xa5, sizeof(buf));
	end = t_now() + f->secs;
	half = t_now() + f->secs * 0.8;
	while (t_now() < end) {
		if (rt_api_send(&ctx, fd, buf, sizeof(buf), 0) <= 0)
			break;
		if (!info && t_now() > half) {
			rt_info_of(fd, f);
			info = 1;
		}
	}
	rt_api_shutdown(&ctx, fd, 1);
	{
		char x;
		rt_api_recv(&ctx, fd, &x, 1, 0);
	}
	rt_api_close(&ctx, fd);
	return NULL;
}

/* Linux receives (upload) */
static void *lx_receiver(void *p)
{
	struct flow *f = p;
	static __thread u8 buf[65536];
	int fd = accept(f->lfd, NULL, NULL);
	ssize_t r;

	if (fd < 0)
		return NULL;
	while ((r = recv(fd, buf, sizeof(buf), 0)) > 0)
		f->bytes += (u64)r;
	close(fd);
	return NULL;
}

static void flow_start(struct flow *f)
{
	f->bytes = 0;
	f->have_rti = 0;
	if (f->riptide_sends) {
		struct sockaddr_in a;
		int one = 1;
		f->lfd = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(f->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(LINUX_IP);
		a.sin_port = htons(f->port);
		CHECK(bind(f->lfd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(f->lfd, 4) == 0);
		pthread_create(&f->th[0], NULL, lx_receiver, f);
		pthread_create(&f->th[1], NULL, rt_sender, f);
	} else {
		struct rt_sockaddr_in a = t_sa(0, f->port);
		f->rfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		t_set_int(&ctx, f->rfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, 1);
		/* a window large enough to fill the 400 ms queue */
		t_set_int(&ctx, f->rfd, RT_SOL_SOCKET, RT_SO_RCVBUF, 600000);
		CHECK(rt_api_bind(&ctx, f->rfd, &a, sizeof(a)) == 0 && rt_api_listen(&ctx, f->rfd, 4) == 0);
		pthread_create(&f->th[0], NULL, rt_receiver, f);
		pthread_create(&f->th[1], NULL, lx_sender, f);
	}
}

static void flow_join(struct flow *f)
{
	pthread_join(f->th[0], NULL);
	pthread_join(f->th[1], NULL);
	if (f->riptide_sends)
		close(f->lfd);
	else
		rt_api_close(&ctx, f->rfd);
}

/* RTT as the sender sees it, ms */
static double sender_rtt_ms(const struct flow *f)
{
	return f->riptide_sends ? f->rt.tcpi_rtt_us / 1000.0 : f->lx.tcpi_rtt / 1000.0;
}

static void report(const char *tag, const struct flow *f, double rate_kbs)
{
	printf("  %-28s %7.1f KB/s (%3.0f%% of the link)  sender RTT %6.1f ms  cc %s bg %d",
	       tag, rate_kbs, rate_kbs * 100.0 / (RATE_KBIT / 8.0 * 1000.0 / 1024.0), sender_rtt_ms(f),
	       f->rt.tcpi_ca_name, f->bg);
	if (getenv("T_DEBUG"))
		printf("  [linux: cwnd %u ssthresh %u retrans %u ca %u, busy %llu rwnd-limited %llu sndbuf-limited "
		       "%llu ms] [riptide rcv_wnd %u rcvbuf %u]",
		       f->lx.tcpi_snd_cwnd, f->lx.tcpi_snd_ssthresh, f->lx.tcpi_total_retrans, f->lx.tcpi_ca_state,
		       (unsigned long long)f->lx.tcpi_busy_time / 1000,
		       (unsigned long long)f->lx.tcpi_rwnd_limited / 1000,
		       (unsigned long long)f->lx.tcpi_sndbuf_limited / 1000, f->rt.tcpi_rcv_wnd, f->rt.tcpi_rcvbuf);
	if (f->have_rti && f->rti.ledbat_base_us)
		printf("  LEDBAT delay %.1f ms over %.1f ms%s", f->rti.ledbat_qdelay_us / 1000.0,
		       f->rti.ledbat_base_us / 1000.0, f->rti.bg & RT_CTL_TCPBG_RL_ON ? ", rLEDBAT on" : "");
	printf("\n");
}

/* run flows for secs; the rate of each over the last 'measure' seconds */
static void run(struct flow *fl, int n, double secs, double measure, double *kbs)
{
	u64 b0[4];
	double t0;
	int i;

	for (i = 0; i < n; i++) {
		fl[i].secs = secs;
		flow_start(&fl[i]);
	}
	usleep((useconds_t)((secs - measure - 0.5) * 1e6));
	for (i = 0; i < n; i++)
		b0[i] = fl[i].bytes;
	t0 = t_now();
	usleep((useconds_t)(measure * 1e6));
	for (i = 0; i < n; i++)
		kbs[i] = (fl[i].bytes - b0[i]) / (t_now() - t0) / 1024.0;
	for (i = 0; i < n; i++)
		flow_join(&fl[i]);
}

static void test_download(void)
{
	struct flow f[2];
	double kbs[2], cap = RATE_KBIT / 8.0 * 1000.0 / 1024.0;
	char err[160];

	memset(f, 0, sizeof(f));
	f[0].name = "normal";
	f[0].port = 9401;
	f[1].name = "background";
	f[1].port = 9402;
	rt_lock();
	CHECK(rt_config_line("background lport=9402", err, sizeof(err), NULL) == 0);
	rt_unlock();

	printf("download, %d kbit/s with 300 packets of queue, 10 ms base RTT:\n", RATE_KBIT);
	run(&f[0], 1, 8, 4, kbs);
	report("normal alone", &f[0], kbs[0]);
	CHECKV(sender_rtt_ms(&f[0]) > 150, "normal RTT %.1f", sender_rtt_ms(&f[0]));
	CHECK(kbs[0] > 0.8 * cap);

	run(&f[1], 1, 10, 4, kbs);
	report("background alone", &f[1], kbs[0]);
	CHECK(f[1].bg == 3 && (f[1].rti.bg & RT_CTL_TCPBG_RL_ON));
	CHECKV(sender_rtt_ms(&f[1]) < 10 + 60 * 1.6, "background RTT %.1f", sender_rtt_ms(&f[1]));
	CHECKV(kbs[0] > 0.6 * cap, "background alone %.1f KB/s", kbs[0]);

	run(f, 2, 12, 6, kbs);
	report("normal, with background", &f[0], kbs[0]);
	report("background, with normal", &f[1], kbs[1]);
	CHECKV(kbs[0] > 3 * kbs[1], "normal %.1f background %.1f", kbs[0], kbs[1]);
	CHECK(kbs[0] + kbs[1] > 0.8 * cap);

	rt_lock();
	rt_config_line("background clear", err, sizeof(err), NULL);
	rt_unlock();
}

static void test_upload(void)
{
	struct flow f[2];
	struct rt_host_impair im;
	double kbs[2], cap = RATE_KBIT / 8.0 * 1000.0 / 1024.0;

	memset(&im, 0, sizeof(im));
	im.delay_ms = 10;
	im.rate_kbit = RATE_KBIT;
	im.queue_limit = 300;
	im.seed = 4242;
	rt_host_impair(tun, &im);
	memset(f, 0, sizeof(f));
	f[0].name = "cubic";
	f[0].port = 9411;
	f[0].riptide_sends = 1;
	f[0].cc = "cubic";
	f[1].name = "ledbat";
	f[1].port = 9412;
	f[1].riptide_sends = 1;
	f[1].cc = "ledbat";

	printf("upload, Riptide's %d kbit/s link with 300 packets of queue, 10 ms delay:\n", RATE_KBIT);
	run(&f[0], 1, 8, 4, kbs);
	report("cubic alone", &f[0], kbs[0]);
	CHECKV(sender_rtt_ms(&f[0]) > 150, "cubic RTT %.1f", sender_rtt_ms(&f[0]));
	run(&f[1], 1, 10, 4, kbs);
	report("ledbat alone", &f[1], kbs[0]);
	CHECKV(sender_rtt_ms(&f[1]) < 10 + 60 * 1.6, "ledbat RTT %.1f", sender_rtt_ms(&f[1]));
	CHECKV(kbs[0] > 0.6 * cap, "ledbat alone %.1f KB/s", kbs[0]);
	run(f, 2, 12, 6, kbs);
	report("cubic, with ledbat", &f[0], kbs[0]);
	report("ledbat, with cubic", &f[1], kbs[1]);
	CHECKV(kbs[0] > 3 * kbs[1], "cubic %.1f ledbat %.1f", kbs[0], kbs[1]);
	rt_host_impair(tun, NULL);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_ledbat_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(240);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tun_name, sizeof(tun_name), "rtl%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(tun_name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffc));
	if (!tun) {
		printf("t_ledbat_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	if (sh("ip addr add 10.83.1.1/30 dev %s && ip link set %s up", tun_name, tun_name)) {
		printf("t_ledbat_tun: SKIP (cannot configure %s)\n", tun_name);
		return 77;
	}
	sh("ip link set dev %s txqueuelen 300", tun_name);
	rt_ctx_init(&ctx, 32, NULL);
	{
		struct rt_host_impair im;
		memset(&im, 0, sizeof(im));
		im.delay_ms = 10;
		im.seed = 77;
		rt_host_impair(tun, &im);
	}
	rt_host_tun_rx_rate(tun, RATE_KBIT);
	test_download();
	rt_host_tun_rx_rate(tun, 0);
	rt_host_impair(tun, NULL);
	test_upload();
	rt_ctx_destroy(&ctx);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
