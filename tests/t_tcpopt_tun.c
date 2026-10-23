/*
 * Riptide - TCP connection behaviour against the Linux kernel over TUN
 * (needs root, /dev/net/tun, iproute2 and iptables; exits 77 otherwise):
 *
 *   Riptide 10.91.1.2 <-- tun --> Linux 10.91.1.1 in netns "rtfoN"
 *   (net.ipv4.tcp_fastopen=3 there; iptables rules play middleboxes)
 *
 * Covers: TCP Fast Open (cookie request, data in the SYN with
 * TCP_FASTOPEN_CONNECT and sendto(MSG_FASTOPEN), a deferred connect that
 * reads or waits first, a server without TFO, a middlebox dropping SYNs
 * with data and the global switch-off), the connect timeout and SYN
 * backoff, ICMP unreachable while connecting, TCP_KEEPINIT and
 * SO_SNDTIMEO on connect(), TIME_WAIT reuse with a small port range and
 * a connection churn, keepalive probes and the dead peer, keepalive_all,
 * and Nagle / TCP_NODELAY / TCP_CORK / TCP_NOPUSH / MSG_MORE segment
 * boundaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include "t_tcp_common.h"

#define OUR_IP   0x0a5b0102u   /* 10.91.1.2 */
#define LINUX_IP 0x0a5b0101u   /* 10.91.1.1 */

static struct rt_sockctx ctx;
static char tun_name[16], ns_name[24];
static struct rt_netif *tun;

static int sh(const char *fmt, ...)
{
	char cmd[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

/* run a command in the Linux namespace */
#define NS(...) sh_ns(__VA_ARGS__)
static int sh_ns(const char *fmt, ...)
{
	char cmd[640];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return sh("ip netns exec %s sh -c '%s'", ns_name, cmd);
}

/* the calling thread joins the Linux namespace */
static void enter_ns(void)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/var/run/netns/%s", ns_name);
	fd = open(path, O_RDONLY);
	if (fd < 0 || setns(fd, CLONE_NEWNET) < 0) {
		perror("setns");
		exit(2);
	}
	close(fd);
}

static int srv_any;      /* listen on every address (the 10.92.0.x servers) */

static int lx_listen(u16 port, int tfo)
{
	struct sockaddr_in a;
	int one = 1, q = 16, fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = srv_any ? 0 : htonl(LINUX_IP);
	a.sin_port = htons(port);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (tfo)
		setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &q, sizeof(q));
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 64) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static double elapsed(double t0) { return t_now() - t0; }

/* ---- Linux servers (threads inside the namespace) ---- */
struct srv {
	u16 port;
	int tfo;            /* TCP_FASTOPEN on the listener */
	int mode;           /* 0 request/response, 1 banner first, 2 sink until EOF, 3 idle until EOF,
	                     * 4 idle 2.5 s, then close */
	int conns;          /* connections to serve (0 = until stop) */
	volatile int stop;
	volatile int ready;
	int syn_data[64];   /* per connection: Linux saw data in the SYN */
	int served;
	u64 got;
	pthread_t th;
};

static void *srv_thread(void *p)
{
	struct srv *s = p;
	int lfd, n = 0;

	enter_ns();
	lfd = lx_listen(s->port, s->tfo);
	s->ready = lfd >= 0 ? 1 : -1;
	if (lfd < 0)
		return NULL;
	while (!s->stop && (!s->conns || n < s->conns)) {
		struct timeval tv = { 0, 200000 };
		fd_set r;
		int fd;
		FD_ZERO(&r);
		FD_SET(lfd, &r);
		if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0)
			continue;
		fd = accept(lfd, NULL, NULL);
		if (fd < 0)
			continue;
		if (s->mode == 0) {
			char buf[256];
			struct tcp_info ti;
			socklen_t l = sizeof(ti);
			ssize_t k, got = 0;
			while (got < (ssize_t)sizeof(buf) - 1 && (k = recv(fd, buf + got, sizeof(buf) - 1 - (size_t)got, 0)) > 0) {
				got += k;
				if (memchr(buf, '\n', (size_t)got))
					break;
			}
			memset(&ti, 0, sizeof(ti));
			getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &l);
			if (n < 64)
				s->syn_data[n] = (ti.tcpi_options & TCPI_OPT_SYN_DATA) != 0;
			if (got > 0) {
				char rep[300];
				int rl = snprintf(rep, sizeof(rep), "OK %.*s", (int)got, buf);
				send(fd, rep, (size_t)rl, 0);
			}
			shutdown(fd, SHUT_WR);
			while (recv(fd, buf, sizeof(buf), 0) > 0)
				;
		} else if (s->mode == 4) {
			usleep(2500000);
		} else if (s->mode == 1) {
			char buf[64];
			send(fd, "220 hello\n", 10, 0);
			while (recv(fd, buf, sizeof(buf), 0) > 0)
				;
		} else {
			char buf[4096];
			ssize_t k;
			struct timeval rt = { 0, 300000 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
			for (;;) {
				k = recv(fd, buf, sizeof(buf), 0);
				if (k > 0)
					s->got += (u64)k;
				else if (k == 0 || s->stop || (errno != EAGAIN && errno != EWOULDBLOCK))
					break;
			}
		}
		close(fd);
		n++;
		s->served = n;
	}
	close(lfd);
	return NULL;
}

static void srv_start(struct srv *s, u16 port, int tfo, int mode, int conns)
{
	memset(s, 0, sizeof(*s));
	s->port = port;
	s->tfo = tfo;
	s->mode = mode;
	s->conns = conns;
	pthread_create(&s->th, NULL, srv_thread, s);
	while (!s->ready)
		usleep(1000);
}

static void srv_stop(struct srv *s)
{
	s->stop = 1;
	pthread_join(s->th, NULL);
}

static int rt_sock(void)
{
	return rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
}

static int rt_conn_ip(int fd, u32 ip, u16 port)
{
	struct rt_sockaddr_in d = t_sa(ip, port);
	return rt_api_connect(&ctx, fd, &d, sizeof(d));
}

static int rt_conn(int fd, u16 port)
{
	return rt_conn_ip(fd, LINUX_IP, port);
}

/* read until EOF into buf; returns bytes */
static int rt_read_all(int fd, char *buf, int len)
{
	int n = 0, r;
	while (n < len - 1 && (r = rt_api_recv(&ctx, fd, buf + n, (u32)(len - 1 - n), 0)) > 0)
		n += r;
	buf[n] = 0;
	return n;
}

static void cache_text(char *buf, u32 len)
{
	u32 l = len;
	buf[0] = 0;
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.fastopen_cache", buf, &l, NULL, 0);
	rt_unlock();
}

static void cache_clear(void)
{
	rt_lock();
	rt_sysctl_byname("net.inet.tcp.fastopen_cache", NULL, NULL, "clear", 5);
	rt_unlock();
}

/* one request/response with TCP_FASTOPEN_CONNECT; returns the bytes of
 * the reply, *deferred = connect() returned before any SYN went out */
static int tfo_request_ip(u32 ip, u16 port, const char *req, char *rep, int replen, int *deferred)
{
	int fd = rt_sock(), r, n;
	u32 opens = rt_tcpstat.ActiveOpens;

	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_FASTOPEN_CONNECT, 1) == 0);
	r = rt_conn_ip(fd, ip, port);
	CHECKV(r == 0, "connect %d", r);
	*deferred = rt_tcpstat.ActiveOpens == opens;
	n = rt_api_send(&ctx, fd, req, (u32)strlen(req), 0);
	CHECKV(n == (int)strlen(req), "send %d", n);
	/* at once, as nc does: possibly before the handshake is complete */
	CHECK(rt_api_shutdown(&ctx, fd, 1) == 0);
	n = rt_read_all(fd, rep, replen);
	{
		struct rt_tcp_info ti;
		t_get_info(&ctx, fd, &ti);
		if (ti.tcpi_options & 32)
			*deferred |= 2;          /* our TCP_INFO: SYN data acknowledged */
	}
	rt_api_close(&ctx, fd);
	return n;
}

static int tfo_request(u16 port, const char *req, char *rep, int replen, int *deferred)
{
	return tfo_request_ip(LINUX_IP, port, req, rep, replen, deferred);
}

static void test_fastopen(void)
{
	struct srv s;
	char rep[512], cache[2048];
	struct rt_tcp_stats b = rt_tcpstat;
	int d = 0, n;

	cache_clear();
	srv_start(&s, 8300, 1, 0, 0);
	/* 1: no cookie yet: a normal handshake with a cookie request */
	n = tfo_request(8300, "GET 1\n", rep, sizeof(rep), &d);
	CHECKV(n > 0 && !strcmp(rep, "OK GET 1\n"), "reply '%s'", rep);
	CHECKV(!(d & 1), "deferred %d", d);
	CHECK(rt_tcpstat.TCPFastOpenCookieReqd == b.TCPFastOpenCookieReqd + 1);
	cache_text(cache, sizeof(cache));
	printf("TFO cache after the first connection: %s", cache);
	CHECKV(strstr(cache, "10.91.1.1 cookie ") != NULL, "cache '%s'", cache);
	/* 2: the cookie: connect() returns at once, the data goes in the SYN */
	n = tfo_request(8300, "GET 2\n", rep, sizeof(rep), &d);
	CHECKV(n > 0 && !strcmp(rep, "OK GET 2\n"), "reply '%s'", rep);
	CHECKV((d & 1) && (d & 2), "deferred/syn-data %d", d);
	CHECK(rt_tcpstat.TCPFastOpenActive == b.TCPFastOpenActive + 1);
	/* 3: sendto(MSG_FASTOPEN) on an unconnected socket */
	{
		struct rt_sockaddr_in a = t_sa(LINUX_IP, 8300);
		int fd = rt_sock();
		n = rt_api_sendto(&ctx, fd, "GET 3\n", 6, RT_MSG_FASTOPEN, &a, sizeof(a));
		CHECKV(n == 6, "sendto %d", n);
		n = rt_read_all(fd, rep, sizeof(rep));
		CHECKV(!strcmp(rep, "OK GET 3\n"), "reply '%s'", rep);
		rt_api_close(&ctx, fd);
		CHECK(rt_tcpstat.TCPFastOpenActive == b.TCPFastOpenActive + 2);
	}
	/* the FIN of a shutdown() during the handshake crosses the server's
	 * (20 ms on our side: the SYN-ACK comes after the shutdown): a
	 * simultaneous close while the program still holds the socket */
	{
		struct rt_host_impair im;
		int i;
		memset(&im, 0, sizeof(im));
		im.delay_ms = 20;
		rt_host_impair(tun, &im);
		for (i = 0; i < 4; i++) {
			n = tfo_request(8300, "X\n", rep, sizeof(rep), &d);
			CHECKV(n > 0 && !strcmp(rep, "OK X\n"), "crossing FINs %d: '%s'", i, rep);
		}
		memset(&im, 0, sizeof(im));
		rt_host_impair(tun, &im);
	}
	srv_stop(&s);
	CHECKV(s.served == 7 && !s.syn_data[0] && s.syn_data[1] && s.syn_data[2],
	       "linux saw SYN data %d %d %d (served %d)", s.syn_data[0], s.syn_data[1], s.syn_data[2], s.served);

	/* 4: deferred connect, the server speaks first: recv() sends the SYN */
	srv_start(&s, 8301, 1, 1, 0);
	{
		int fd = rt_sock(), r;
		char buf[64];
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_FASTOPEN_CONNECT, 1);
		CHECK(rt_conn(fd, 8301) == 0);
		r = rt_api_recv(&ctx, fd, buf, sizeof(buf) - 1, 0);
		CHECKV(r == 10 && !memcmp(buf, "220 hello\n", 10), "banner r=%d", r);
		rt_api_close(&ctx, fd);
		/* the same with WaitSelect for reading */
		fd = rt_sock();
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_FASTOPEN_CONNECT, 1);
		CHECK(rt_conn(fd, 8301) == 0);
		{
			u32 rd[1] = { 1u << fd };
			r = rt_api_select(&ctx, fd + 1, rd, NULL, NULL, 3000, NULL);
			CHECKV(r == 1, "select %d", r);
		}
		r = rt_api_recv(&ctx, fd, buf, sizeof(buf) - 1, 0);
		CHECKV(r == 10, "banner after select r=%d", r);
		rt_api_close(&ctx, fd);
	}
	srv_stop(&s);

	/* 5: a middlebox drops SYNs with data: the plain SYN retransmission
	 * gets through and the data follows; the second loss in a row stops
	 * TFO to that server for a while */
	NS("iptables -A INPUT -p tcp --dport 8302 --syn -m length --length 120:2000 -j DROP");
	srv_start(&s, 8302, 1, 0, 0);
	{
		int i, used[3];
		for (i = 0; i < 3; i++) {
			double t0 = t_now();
			char req[96];
			snprintf(req, sizeof(req), "BH %d %060d\n", i, 0);
			n = tfo_request(8302, req, rep, sizeof(rep), &d);
			CHECKV(n > 0 && !strncmp(rep, "OK BH", 5), "reply '%s'", rep);
			used[i] = d & 1;
			printf("TFO through a SYN-data dropping middlebox: %.2f s, data in SYN %d\n", elapsed(t0), d & 1);
		}
		CHECKV(used[0] && used[1] && !used[2], "TFO used %d %d %d", used[0], used[1], used[2]);
		CHECKV(rt_tcpstat.TCPFastOpenSynLost == b.TCPFastOpenSynLost + 2, "lost %u",
		       rt_tcpstat.TCPFastOpenSynLost - b.TCPFastOpenSynLost);
		cache_text(cache, sizeof(cache));
		printf("TFO cache: %s", cache);
		CHECKV(strstr(cache, "syn-lost") != NULL, "%s", cache);
	}
	srv_stop(&s);
	NS("iptables -F INPUT");

	/* the same at three servers in a row: TFO goes off everywhere */
	cache_clear();
	{
		int i;
		u32 bh = rt_tcpstat.TCPFastOpenBlackhole;
		char req[96];
		NS("ip addr add 10.92.0.1/32 dev lo; ip addr add 10.92.0.2/32 dev lo; "
		   "ip addr add 10.92.0.3/32 dev lo; ip addr add 10.92.0.4/32 dev lo");
		rt_lock();
		rt_route_add(rt_htonl(0x0a5c0000u), rt_htonl(0xffffff00u), rt_htonl(LINUX_IP), tun,
		             RT_RTF_UP | RT_RTF_GATEWAY | RT_RTF_STATIC, 0);
		rt_unlock();
		srv_any = 1;
		srv_start(&s, 8304, 1, 0, 0);
		snprintf(req, sizeof(req), "BIG %060d\n", 0);
		for (i = 1; i <= 3; i++)
			tfo_request_ip(0x0a5c0000u + (u32)i, 8304, "C\n", rep, sizeof(rep), &d);
		NS("iptables -A INPUT -p tcp --syn -m length --length 120:2000 -j DROP");
		for (i = 1; i <= 3; i++)
			tfo_request_ip(0x0a5c0000u + (u32)i, 8304, req, rep, sizeof(rep), &d);
		cache_text(cache, sizeof(cache));
		printf("three lost TFO SYNs at three servers: switch-offs %u, cache: %s",
		       rt_tcpstat.TCPFastOpenBlackhole - bh, cache);
		CHECK(rt_tcpstat.TCPFastOpenBlackhole == bh + 1);
		CHECKV(!strncmp(cache, "disabled for 36", 15), "%s", cache);
		{
			u32 req0 = rt_tcpstat.TCPFastOpenCookieReqd;
			tfo_request_ip(0x0a5c0004u, 8304, "D\n", rep, sizeof(rep), &d);
			CHECKV(!(d & 1) && rt_tcpstat.TCPFastOpenCookieReqd == req0, "d %d", d);
		}
		NS("iptables -F INPUT");
		srv_stop(&s);
		srv_any = 0;
	}
	cache_clear();

	/* 6: a server without TFO ignores the SYN data: sent again at once */
	srv_start(&s, 8300, 1, 0, 0);
	tfo_request(8300, "C\n", rep, sizeof(rep), &d);       /* cookie for 10.91.1.1 */
	srv_stop(&s);
	srv_start(&s, 8303, 0, 0, 0);
	{
		double t0 = t_now();
		u32 fail = rt_tcpstat.TCPFastOpenActiveFail;
		n = tfo_request(8303, "PLAIN\n", rep, sizeof(rep), &d);
		CHECKV(!strcmp(rep, "OK PLAIN\n"), "reply '%s'", rep);
		CHECKV((d & 1) && !(d & 2), "d %d", d);
		CHECK(rt_tcpstat.TCPFastOpenActiveFail == fail + 1);
		CHECKV(elapsed(t0) < 0.5, "%.2f s", elapsed(t0));
		printf("TFO to a server without TFO: %.3f s\n", elapsed(t0));
	}
	srv_stop(&s);
	cache_text(cache, sizeof(cache));
	CHECKV(strstr(cache, "no cookie") != NULL, "cache after the ignored cookie: %s", cache);

	/* TFO off: no option at all */
	t_sysctl("net.inet.tcp.fastopen", 0);
	{
		u32 req = rt_tcpstat.TCPFastOpenCookieReqd;
		srv_start(&s, 8300, 1, 0, 0);
		tfo_request(8300, "OFF\n", rep, sizeof(rep), &d);
		srv_stop(&s);
		CHECK(rt_tcpstat.TCPFastOpenCookieReqd == req && !(d & 1));
	}
	t_sysctl("net.inet.tcp.fastopen", 1);
	cache_clear();
}

/* ---- connecting: timeouts, backoff, ICMP ---- */
static int timed_connect(u16 port, double *secs, int keepinit, int sndtimeo_ms)
{
	int fd = rt_sock(), r;
	double t0;

	if (keepinit)
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPINIT, keepinit);
	if (sndtimeo_ms) {
		struct rt_timeval tv;
		tv.tv_secs = (u32)(sndtimeo_ms / 1000);
		tv.tv_micro = (u32)(sndtimeo_ms % 1000) * 1000u;
		rt_api_setsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv));
	}
	t0 = t_now();
	r = rt_conn(fd, port);
	*secs = elapsed(t0);
	rt_api_close(&ctx, fd);
	return r;
}

static void test_connect(void)
{
	double t;
	int r;
	u32 syns;

	NS("iptables -A INPUT -p tcp --dport 9999 -j DROP");
	/* defaults: 1 s doubling to 4 s, 20 s in all */
	t_sysctl("net.inet.tcp.connect_timeout", 6);
	syns = rt_tcpstat.RetransSegs;
	r = timed_connect(9999, &t, 0, 0);
	printf("connect to a dead port, connect_timeout 6: %d after %.2f s, %u SYN retransmissions\n", r, t,
	       rt_tcpstat.RetransSegs - syns);
	CHECKV(r == -RT_ETIMEDOUT && t > 5.5 && t < 7.0, "r %d t %.2f", r, t);
	/* SYNs at 0, 1, 3 (rto 2); the next (rto 4) would be after the 6 s */
	CHECKV(rt_tcpstat.RetransSegs - syns == 2, "%u", rt_tcpstat.RetransSegs - syns);

	/* linear 500 ms steps, at most 1 s */
	t_sysctl("net.inet.tcp.syn_rto_initial_ms", 500);
	t_sysctl("net.inet.tcp.syn_rto_max_ms", 1000);
	t_sysctl("net.inet.tcp.syn_backoff", 0);
	t_sysctl("net.inet.tcp.connect_timeout", 4);
	syns = rt_tcpstat.RetransSegs;
	r = timed_connect(9999, &t, 0, 0);
	printf("linear 0.5 s backoff capped at 1 s, 4 s timeout: %d after %.2f s, %u SYN retransmissions\n", r, t,
	       rt_tcpstat.RetransSegs - syns);
	/* SYNs at 0, 0.5, 1.5, 2.5, 3.5 */
	CHECKV(r == -RT_ETIMEDOUT && t > 3.7 && t < 4.6, "r %d t %.2f", r, t);
	CHECKV(rt_tcpstat.RetransSegs - syns == 4, "%u", rt_tcpstat.RetransSegs - syns);
	t_sysctl("net.inet.tcp.syn_rto_initial_ms", 1000);
	t_sysctl("net.inet.tcp.syn_rto_max_ms", 4000);
	t_sysctl("net.inet.tcp.syn_backoff", 1);
	t_sysctl("net.inet.tcp.connect_timeout", 20);

	/* per socket: TCP_KEEPINIT 2 s; SO_SNDTIMEO 1 s (the attempt goes on) */
	r = timed_connect(9999, &t, 2, 0);
	CHECKV(r == -RT_ETIMEDOUT && t > 1.8 && t < 2.6, "keepinit r %d t %.2f", r, t);
	r = timed_connect(9999, &t, 0, 1000);
	CHECKV(r == -RT_ETIMEDOUT && t > 0.9 && t < 1.4, "sndtimeo r %d t %.2f", r, t);
	NS("iptables -F INPUT");

	/* ICMP host unreachable for the SYN */
	NS("iptables -A INPUT -p tcp --dport 9998 -j REJECT --reject-with icmp-host-unreachable");
	r = timed_connect(9998, &t, 0, 0);
	printf("host unreachable, syn_icmp_abort 1: %d after %.2f s\n", r, t);
	CHECKV(r == -RT_EHOSTUNREACH && t > 0.8 && t < 1.6, "r %d t %.2f", r, t);
	t_sysctl("net.inet.tcp.syn_icmp_abort", 2);
	r = timed_connect(9998, &t, 0, 0);
	CHECKV(r == -RT_EHOSTUNREACH && t < 0.5, "abort 2: r %d t %.2f", r, t);
	t_sysctl("net.inet.tcp.syn_icmp_abort", 0);
	t_sysctl("net.inet.tcp.connect_timeout", 3);
	r = timed_connect(9998, &t, 0, 0);
	CHECKV(r == -RT_EHOSTUNREACH && t > 2.8 && t < 3.6, "abort 0: r %d t %.2f", r, t);
	t_sysctl("net.inet.tcp.syn_icmp_abort", 1);
	t_sysctl("net.inet.tcp.connect_timeout", 20);
	/* port unreachable is a hard error: at once, whatever the setting */
	NS("iptables -F INPUT; iptables -A INPUT -p tcp --dport 9997 -j REJECT --reject-with icmp-port-unreachable");
	r = timed_connect(9997, &t, 0, 0);
	CHECKV(r == -RT_ECONNREFUSED && t < 0.5, "port unreachable r %d t %.2f", r, t);
	NS("iptables -F INPUT");
}

/* ---- TIME_WAIT reuse and port churn ---- */
static int short_conn(u16 port)
{
	int fd = rt_sock(), r;
	r = rt_conn(fd, port);
	if (r == 0) {
		rt_api_send(&ctx, fd, "x", 1, 0);
		rt_api_shutdown(&ctx, fd, 1);   /* we close first: TIME_WAIT is ours */
		{
			char b[8];
			while (rt_api_recv(&ctx, fd, b, sizeof(b), 0) > 0)
				;
		}
	}
	rt_api_close(&ctx, fd);
	return r;
}

static void test_time_wait(void)
{
	struct srv s;
	int i, r = 0, ok = 0;
	u32 rec = rt_tcpstat.TimeWaitRecycled;
	double t0;

	srv_start(&s, 8400, 0, 2, 0);
	t_sysctl("net.inet.ip.portrange.first", 61000);
	t_sysctl("net.inet.ip.portrange.last", 61019);
	t_sysctl("net.inet.tcp.tw_reuse", 0);
	for (i = 0; i < 20; i++)
		ok += short_conn(8400) == 0;
	r = short_conn(8400);
	printf("20 ports in TIME_WAIT, tw_reuse 0: %d of 20, the 21st: %d\n", ok, r);
	CHECKV(ok == 20 && r == -RT_EADDRNOTAVAIL, "ok %d r %d", ok, r);
	t_sysctl("net.inet.tcp.tw_reuse", 1);
	usleep(1100000);                 /* PAWS: at least a second */
	for (ok = 0, i = 0; i < 20; i++)
		ok += short_conn(8400) == 0;
	printf("tw_reuse 1 after 1.1 s: %d of 20 connections, %u TIME_WAIT records reused\n", ok,
	       rt_tcpstat.TimeWaitRecycled - rec);
	CHECKV(ok == 20 && rt_tcpstat.TimeWaitRecycled - rec >= 20, "ok %d rec %u", ok,
	       rt_tcpstat.TimeWaitRecycled - rec);
	t_sysctl("net.inet.ip.portrange.first", 49152);
	t_sysctl("net.inet.ip.portrange.last", 65535);
	/* churn: short connections to one server port */
	t0 = t_now();
	for (ok = 0, i = 0; i < 3000; i++)
		ok += short_conn(8400) == 0;
	printf("churn: %d of 3000 short connections in %.2f s (%.0f/s), TIME_WAIT reused %u\n", ok, elapsed(t0),
	       3000 / elapsed(t0), rt_tcpstat.TimeWaitRecycled - rec);
	CHECK(ok == 3000);
	srv_stop(&s);
}

/* ---- keepalive ---- */
static void test_keepalive(void)
{
	struct srv s;
	int fd, r, v;
	u32 len, ka;
	char b[8];
	double t0;

	srv_start(&s, 8500, 0, 3, 0);
	fd = rt_sock();
	CHECK(rt_conn(fd, 8500) == 0);
	CHECK(t_set_int(&ctx, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, 1) == 0);
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, 1) == 0);
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPINTVL, 1) == 0);
	CHECK(t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPCNT, 3) == 0);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, &v, &len) == 0 && v == 1);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPCNT, &v, &len) == 0 && v == 3);
	ka = rt_tcpstat.TCPKeepAlive;
	usleep(4500000);
	printf("keepalive 1/1/3 to a live peer: %u probes in 4.5 s\n", rt_tcpstat.TCPKeepAlive - ka);
	CHECKV(rt_tcpstat.TCPKeepAlive - ka >= 3, "%u", rt_tcpstat.TCPKeepAlive - ka);
	CHECK(t_set_nbio(&ctx, fd, 1) == 0);
	CHECK(rt_api_recv(&ctx, fd, b, 1, 0) == -RT_EWOULDBLOCK);
	CHECK(t_set_nbio(&ctx, fd, 0) == 0);
	/* the peer goes away silently */
	NS("iptables -A INPUT -s 10.91.1.2 -j DROP");
	t0 = t_now();
	r = rt_api_recv(&ctx, fd, b, 1, 0);
	printf("dead peer: recv %d after %.2f s\n", r, elapsed(t0));
	CHECKV(r == -RT_ETIMEDOUT && elapsed(t0) < 6.0, "r %d t %.2f", r, elapsed(t0));
	rt_api_close(&ctx, fd);
	NS("iptables -F INPUT");

	/* as nc -K 1 -d -w 5: probes answered, then we close first (TIME_WAIT
	 * with the keepalive timer having run); the stack must go on */
	fd = rt_sock();
	CHECK(rt_conn(fd, 8500) == 0);
	t_set_int(&ctx, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, 1);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, 1);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPINTVL, 1);
	{
		u32 rd[2] = { 0, 0 };
		rd[fd / 32] |= 1u << (fd % 32);
		t0 = t_now();
		r = rt_api_select(&ctx, fd + 1, rd, NULL, NULL, 3500, NULL);
		CHECKV(r == 0 && elapsed(t0) > 3.0, "select r %d after %.2f s", r, elapsed(t0));
	}
	t0 = t_now();
	CHECK(rt_api_close(&ctx, fd) == 0);
	CHECKV(elapsed(t0) < 0.5, "close took %.2f s", elapsed(t0));
	usleep(300000);
	{
		struct srv s2;
		char rep[64];
		int d;
		srv_start(&s2, 8501, 0, 0, 0);
		t0 = t_now();
		r = tfo_request(8501, "AFTER\n", rep, sizeof(rep), &d);
		CHECKV(r > 0 && !strcmp(rep, "OK AFTER\n") && elapsed(t0) < 1.0,
		       "a connection after the keepalive close: '%s' in %.2f s", rep, elapsed(t0));
		srv_stop(&s2);
	}
	/* the peer closes an idle keepalive connection: recv() returns 0 */
	{
		struct srv s3;
		srv_start(&s3, 8502, 0, 4, 0);
		fd = rt_sock();
		CHECK(rt_conn(fd, 8502) == 0);
		t_set_int(&ctx, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, 1);
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, 1);
		t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_KEEPINTVL, 1);
		t0 = t_now();
		r = rt_api_recv(&ctx, fd, b, 1, 0);
		CHECKV(r == 0 && elapsed(t0) > 2.0 && elapsed(t0) < 4.0, "peer close: recv %d after %.2f s", r,
		       elapsed(t0));
		CHECK(rt_api_close(&ctx, fd) == 0);
		srv_stop(&s3);
	}

	/* keepalive_all: a program that never asked */
	t_sysctl("net.inet.tcp.keepalive_all", 1);
	t_sysctl("net.inet.tcp.keepintvl", 1);
	fd = rt_sock();
	CHECK(rt_conn(fd, 8500) == 0);
	ka = rt_tcpstat.TCPKeepAlive;
	usleep(2500000);
	printf("keepalive_all 1: %u probes in 2.5 s\n", rt_tcpstat.TCPKeepAlive - ka);
	CHECK(rt_tcpstat.TCPKeepAlive - ka >= 1);
	/* an explicit SO_KEEPALIVE 0 wins */
	CHECK(t_set_int(&ctx, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, 0) == 0);
	usleep(300000);
	ka = rt_tcpstat.TCPKeepAlive;
	usleep(2500000);
	CHECKV(rt_tcpstat.TCPKeepAlive == ka, "%u probes after SO_KEEPALIVE 0", rt_tcpstat.TCPKeepAlive - ka);
	rt_api_close(&ctx, fd);
	t_sysctl("net.inet.tcp.keepalive_all", 0);
	t_sysctl("net.inet.tcp.keepintvl", 75);
	srv_stop(&s);
}

/* ---- Nagle, TCP_NODELAY, TCP_CORK, TCP_NOPUSH, MSG_MORE ---- */
static u32 data_segs(int fd)
{
	struct rt_tcp_info ti;
	t_get_info(&ctx, fd, &ti);
	return ti.tcpi_data_segs_out;
}

static void sends(int fd, int n, u32 len, int flags)
{
	static char buf[1024];
	int i;
	memset(buf, 'a', sizeof(buf));
	for (i = 0; i < n; i++)
		CHECK(rt_api_send(&ctx, fd, buf, len, flags) == (int)len);
}

static void test_nagle_cork(void)
{
	struct srv s;
	struct rt_host_impair im;
	int fd;
	u32 d0;

	/* 50 ms on our segments: ACKs come late, so Nagle's effect shows */
	memset(&im, 0, sizeof(im));
	im.delay_ms = 50;
	rt_host_impair(tun, &im);
	srv_start(&s, 8600, 0, 2, 0);
	fd = rt_sock();
	CHECK(rt_conn(fd, 8600) == 0);
	usleep(200000);

	/* Nagle: one small segment in flight, the rest waits */
	d0 = data_segs(fd);
	sends(fd, 10, 10, 0);
	CHECKV(data_segs(fd) - d0 == 1, "nagle %u", data_segs(fd) - d0);
	/* TCP_NODELAY pushes what Nagle held at once */
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_NODELAY, 1);
	CHECKV(data_segs(fd) - d0 == 2, "nodelay push %u", data_segs(fd) - d0);
	usleep(300000);
	d0 = data_segs(fd);
	sends(fd, 10, 10, 0);
	CHECKV(data_segs(fd) - d0 == 10, "nodelay %u", data_segs(fd) - d0);
	usleep(300000);

	/* TCP_CORK: held, then sent as one segment by the 200 ms ceiling */
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CORK, 1);
	d0 = data_segs(fd);
	sends(fd, 10, 100, 0);
	CHECKV(data_segs(fd) == d0, "cork %u", data_segs(fd) - d0);
	usleep(350000);
	CHECKV(data_segs(fd) - d0 == 1, "cork ceiling %u", data_segs(fd) - d0);
	/* uncork sends at once */
	d0 = data_segs(fd);
	sends(fd, 3, 100, 0);
	CHECK(data_segs(fd) == d0);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CORK, 0);
	CHECKV(data_segs(fd) - d0 == 1, "uncork %u", data_segs(fd) - d0);
	usleep(300000);

	/* TCP_NOPUSH: held; with nothing in flight the window probe timer
	 * sends it after an RTO, as Linux does for TCP_CORK */
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_NOPUSH, 1);
	d0 = data_segs(fd);
	sends(fd, 3, 100, 0);
	CHECKV(data_segs(fd) == d0, "nopush %u", data_segs(fd) - d0);
	usleep(800000);
	CHECKV(data_segs(fd) - d0 == 1, "nopush after an RTO %u", data_segs(fd) - d0);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_NOPUSH, 0);
	usleep(300000);

	/* MSG_MORE: one segment for four sends */
	d0 = data_segs(fd);
	sends(fd, 3, 100, RT_MSG_MORE);
	CHECK(data_segs(fd) == d0);
	sends(fd, 1, 100, 0);
	CHECKV(data_segs(fd) - d0 == 1, "msg_more %u", data_segs(fd) - d0);
	/* full segments are never held */
	d0 = data_segs(fd);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CORK, 1);
	{
		static char big[4000];
		memset(big, 'b', sizeof(big));
		CHECK(rt_api_send(&ctx, fd, big, sizeof(big), 0) == (int)sizeof(big));
	}
	CHECKV(data_segs(fd) - d0 == 2, "cork full segments %u", data_segs(fd) - d0);
	t_set_int(&ctx, fd, RT_IPPROTO_TCP, RT_TCP_CORK, 0);
	rt_api_shutdown(&ctx, fd, 1);
	usleep(300000);
	rt_api_close(&ctx, fd);
	srv_stop(&s);
	memset(&im, 0, sizeof(im));
	rt_host_impair(tun, &im);
	printf("Nagle/NODELAY/CORK/NOPUSH/MSG_MORE: %llu bytes reached Linux, %u cork ceiling flushes\n",
	       (unsigned long long)s.got, rt_tcpstat.TCPCorkFlush);
	CHECKV(s.got == 100 + 100 + 1000 + 300 + 300 + 400 + 4000, "linux got %llu", (unsigned long long)s.got);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0 || system("iptables -V >/dev/null 2>&1") != 0) {
		printf("t_tcpopt_tun: SKIP (needs root, /dev/net/tun, iproute2 and iptables)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(600);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tun_name, sizeof(tun_name), "rtfo%d", (int)(getpid() % 100000));
	snprintf(ns_name, sizeof(ns_name), "rtfons%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(tun_name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffc));
	if (!tun) {
		printf("t_tcpopt_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	if (sh("ip netns add %s", ns_name) || sh("ip link set %s netns %s", tun_name, ns_name) ||
	    NS("ip addr add 10.91.1.1/30 dev %s && ip link set %s up && ip link set lo up && "
	       "sysctl -qw net.ipv4.tcp_fastopen=3", tun_name, tun_name)) {
		printf("t_tcpopt_tun: SKIP (cannot set up the namespace)\n");
		sh("ip netns del %s 2>/dev/null", ns_name);
		return 77;
	}
	if (NS("iptables -A INPUT -m length --length 1:2000 -j ACCEPT && iptables -F INPUT") ) {
		printf("t_tcpopt_tun: SKIP (iptables does not work in a namespace)\n");
		sh("ip netns del %s 2>/dev/null", ns_name);
		return 77;
	}
	rt_ctx_init(&ctx, 64, NULL);

	test_fastopen();
	test_connect();
	test_time_wait();
	test_keepalive();
	test_nagle_cork();

	printf("tcp stats: TFO ok %u fail %u cookie requests %u lost SYNs %u blackhole %u, connect timeouts %u, "
	       "unreachable %u, cork flushes %u, TIME_WAIT reused %u, keepalive probes %u\n",
	       rt_tcpstat.TCPFastOpenActive, rt_tcpstat.TCPFastOpenActiveFail, rt_tcpstat.TCPFastOpenCookieReqd,
	       rt_tcpstat.TCPFastOpenSynLost, rt_tcpstat.TCPFastOpenBlackhole, rt_tcpstat.TCPConnectTimeouts,
	       rt_tcpstat.TCPConnectUnreach, rt_tcpstat.TCPCorkFlush, rt_tcpstat.TimeWaitRecycled,
	       rt_tcpstat.TCPKeepAlive);
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	sh("ip netns del %s 2>/dev/null", ns_name);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
