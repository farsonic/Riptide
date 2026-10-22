/*
 * Riptide - track H (Phase 3) interop with the Linux kernel over TAP
 * (needs root, /dev/net/tun and iproute2; exits 77 = skip otherwise):
 *
 *   Linux 10.81.1.1/24, 10.81.2.1/24 <-- tap -->
 *   Riptide 10.81.1.2/24 + 10.81.2.2/24 (second address), fd00:81::2/64
 *
 * - the second address answers ARP and ping, accepts TCP, and is the
 *   source towards its own subnet; removing it, and "ipv4 off", stop that
 * - DSCP: SO_PRIORITY and "qos rule" marks arrive at Linux (IP_RECVTOS),
 *   also on a TCP SYN; IP_TOS wins
 * - Happy Eyeballs: a name with an IPv6 address nobody answers connects
 *   over IPv4 one attempt delay later; the AAAA filter skips IPv6 without
 *   a route; RFC 6724 order (IPv4 before a ULA) without a preference
 * - parallel A/AAAA lookups against a DNS server on Linux whose AAAA
 *   answer never comes (A alone, 50 ms later) or comes late (waited for)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
/* Riptide headers first: <net/if.h> defines ifr_* macros */
#include "rt/riptide.h"
#include "rt/rt_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKV(c, ...) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s (", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); \
	fprintf(stderr, ")\n"); } } while (0)

static struct rt_sockctx ctx;
static char tap_name[16];
static struct rt_netif *tap;
static const u8 mac[6] = { 0x02, 0x52, 0x54, 0x00, 0x81, 0x02 };
static int psock = -1;            /* Linux: IP packets on the tap */

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static u32 A(const char *s)
{
	struct in_addr a;
	inet_pton(AF_INET, s, &a);
	return a.s_addr;
}

static long ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* a riptide.conf line; "%s" is the tap's name */
static int config(const char *fmt, ...)
{
	char line[256], err[256];
	va_list ap;
	int r;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0)
		fprintf(stderr, "config \"%s\": %s\n", line, err);
	return r;
}

/* ---- Linux side ---- */

static int lx_udp(u16 port, int recvtos)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
	struct sockaddr_in sin;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (recvtos)
		setsockopt(s, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

/* one datagram within ms: its source address and TOS (-1 none) */
static int lx_recv(int s, int ms, u32 *src, int *tos)
{
	struct pollfd pf = { s, POLLIN, 0 };
	char buf[512], cbuf[256];
	struct sockaddr_in from;
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh;
	struct cmsghdr *cm;
	ssize_t n;

	*src = 0;
	*tos = -1;
	if (poll(&pf, 1, ms) <= 0)
		return -1;
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = sizeof(from);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cbuf;
	mh.msg_controllen = sizeof(cbuf);
	n = recvmsg(s, &mh, 0);
	if (n < 0)
		return -1;
	*src = from.sin_addr.s_addr;
	for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
		if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TOS)
			*tos = *(u8 *)CMSG_DATA(cm);
	return (int)n;
}

static int lx_listen(int family, const char *addr, u16 port)
{
	int s = socket(family, SOCK_STREAM, 0), one = 1, zero = 0;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (family == AF_INET6) {
		struct sockaddr_in6 s6;
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
		memset(&s6, 0, sizeof(s6));
		s6.sin6_family = AF_INET6;
		s6.sin6_port = htons(port);
		inet_pton(AF_INET6, addr, &s6.sin6_addr);
		if (bind(s, (struct sockaddr *)&s6, sizeof(s6)) < 0) {
			close(s);
			return -1;
		}
	} else {
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);
		sin.sin_addr.s_addr = A(addr);
		if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			close(s);
			return -1;
		}
	}
	listen(s, 8);
	return s;
}

/* the next TCP SYN from Riptide seen on the tap within ms: its TOS */
static int syn_tos(int ms)
{
	long end = ms_now() + ms;
	u8 p[2048];

	for (;;) {
		struct pollfd pf = { psock, POLLIN, 0 };
		struct sockaddr_ll ll;
		socklen_t sl = sizeof(ll);
		long left = end - ms_now();
		ssize_t n;
		if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
			return -1;
		n = recvfrom(psock, p, sizeof(p), 0, (struct sockaddr *)&ll, &sl);
		if (n < 40 || ll.sll_pkttype == PACKET_OUTGOING || (p[0] >> 4) != 4 || p[9] != 6)
			continue;
		if (memcmp(p + 12, &(u32){ A("10.81.1.2") }, 4))
			continue;
		if ((p[(p[0] & 15) * 4 + 13] & 0x12) == 0x02)     /* SYN, not SYN-ACK */
			return p[1];
	}
}

/* ---- a DNS server on Linux 10.81.1.1:53 ---- */

static int dns_sock = -1;
static volatile int dns_stop;
static volatile int aaaa_delay_ms;        /* -1: AAAA never answered */
static volatile int a_delay_ms;

struct pending {
	long at;
	u8 msg[512];
	int len;
	struct sockaddr_in to;
};

static int dns_answer(const u8 *q, int ql, u8 *r, int *qtype)
{
	int off = 12, rl;
	u16 type;
	char name[256];
	int nl = 0;

	while (off < ql && q[off]) {
		int l = q[off];
		if (nl)
			name[nl++] = '.';
		memcpy(name + nl, q + off + 1, (size_t)l);
		nl += l;
		off += 1 + l;
	}
	name[nl] = 0;
	off++;
	if (off + 4 > ql)
		return -1;
	type = (u16)(q[off] << 8 | q[off + 1]);
	*qtype = type;
	off += 4;
	memcpy(r, q, (size_t)off);
	r[2] = 0x81;
	r[3] = 0x80;
	r[6] = r[7] = 0;
	r[8] = r[9] = r[10] = r[11] = 0;
	rl = off;
	if (strstr(name, ".test") == NULL) {
		r[3] = 0x83;                         /* NXDOMAIN */
		return rl;
	}
	if (type == 1 || type == 28) {
		static const u8 a4[4] = { 10, 81, 1, 1 };
		static const u8 a6[16] = { 0xfd, 0, 0, 0x81, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
		r[7] = 1;
		r[rl++] = 0xc0;
		r[rl++] = 12;
		r[rl++] = 0;
		r[rl++] = (u8)type;
		r[rl++] = 0;
		r[rl++] = 1;
		r[rl++] = 0;
		r[rl++] = 0;
		r[rl++] = 0;
		r[rl++] = 60;
		r[rl++] = 0;
		r[rl++] = type == 1 ? 4 : 16;
		memcpy(r + rl, type == 1 ? a4 : a6, type == 1 ? 4u : 16u);
		rl += type == 1 ? 4 : 16;
	}
	return rl;
}

static void *dns_thread(void *arg)
{
	struct pending pend[16];
	int np = 0, i;

	(void)arg;
	while (!dns_stop) {
		struct pollfd pf = { dns_sock, POLLIN, 0 };
		long now = ms_now(), wait = 50;
		for (i = 0; i < np; i++)
			if (pend[i].at - now < wait)
				wait = pend[i].at - now < 0 ? 0 : pend[i].at - now;
		if (poll(&pf, 1, (int)wait) > 0) {
			u8 q[512];
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			int n = (int)recvfrom(dns_sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl), qt = 0, d;
			if (n > 12 && np < 16) {
				pend[np].len = dns_answer(q, n, pend[np].msg, &qt);
				d = qt == 28 ? aaaa_delay_ms : qt == 1 ? a_delay_ms : 0;
				if (pend[np].len > 0 && d >= 0) {
					pend[np].at = ms_now() + d;
					pend[np].to = from;
					np++;
				}
			}
		}
		now = ms_now();
		for (i = 0; i < np; i++)
			if (pend[i].at <= now) {
				sendto(dns_sock, pend[i].msg, (size_t)pend[i].len, 0, (struct sockaddr *)&pend[i].to,
				       sizeof(pend[i].to));
				pend[i--] = pend[--np];
			}
	}
	return NULL;
}

/* ---- Riptide side ---- */

static int rt_udp_to(const char *dst, u16 port, int prio, int tos)
{
	struct rt_sockaddr_in sin;
	int s = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0), r;
	s32 v;

	if (s < 0)
		return s;
	if (prio >= 0) {
		v = prio;
		rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_PRIORITY, &v, sizeof(v));
	}
	if (tos >= 0) {
		v = tos;
		rt_api_setsockopt(&ctx, s, RT_IPPROTO_IP, RT_IP_TOS, &v, sizeof(v));
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(port);
	sin.sin_addr = A(dst);
	r = rt_api_sendto(&ctx, s, "hello", 5, 0, &sin, sizeof(sin));
	rt_api_close(&ctx, s);
	return r;
}

static void test_addresses(void)
{
	struct rt_sockaddr_in sin;
	int lx, s, a, c, tos, r;
	u32 src;

	/* the second address answers ARP and ping */
	CHECK(sh("ping -c 1 -W 2 10.81.1.2 >/dev/null 2>&1") == 0);
	CHECK(sh("ping -c 1 -W 2 10.81.2.2 >/dev/null 2>&1") == 0);
	/* and accepts TCP */
	s = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(6000);
	CHECK(rt_api_bind(&ctx, s, &sin, sizeof(sin)) == 0 && rt_api_listen(&ctx, s, 4) == 0);
	{
		struct sockaddr_in to;
		c = socket(AF_INET, SOCK_STREAM, 0);
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = htons(6000);
		to.sin_addr.s_addr = A("10.81.2.2");
		CHECK(connect(c, (struct sockaddr *)&to, sizeof(to)) == 0);
		a = rt_api_accept(&ctx, s, NULL, NULL);
		CHECK(a >= 0);
		if (a >= 0) {
			struct rt_sockaddr_in me;
			u32 ml = sizeof(me);
			CHECK(rt_api_getsockname(&ctx, a, &me, &ml) == 0 && me.sin_addr == A("10.81.2.2"));
			rt_api_close(&ctx, a);
		}
		close(c);
	}
	rt_api_close(&ctx, s);
	/* source selection */
	lx = lx_udp(7000, 0);
	CHECK(lx >= 0);
	CHECK(rt_udp_to("10.81.2.1", 7000, -1, -1) > 0);
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && src == A("10.81.2.2"), "r %d src %08x", r, src);
	CHECK(rt_udp_to("10.81.1.1", 7000, -1, -1) > 0);
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && src == A("10.81.1.2"), "r %d src %08x", r, src);
	close(lx);
	/* removed: no answer; back again */
	CHECK(config("ifconfig %s 10.81.2.2 -alias", tap_name) == 0);
	CHECK(sh("ping -c 1 -W 1 10.81.2.2 >/dev/null 2>&1") != 0);
	CHECK(config("ifconfig %s alias 10.81.2.2/24", tap_name) == 0);
	CHECK(sh("ping -c 1 -W 2 10.81.2.2 >/dev/null 2>&1") == 0);
	/* ipv4 off: nothing answers; on again with the addresses back */
	CHECK(config("ipv4 %s off", tap_name) == 0);
	CHECK(sh("ping -c 1 -W 1 10.81.1.2 >/dev/null 2>&1") != 0);
	CHECK(config("ipv4 %s on", tap_name) == 0);
	CHECK(config("ifconfig %s 10.81.1.2/24", tap_name) == 0);
	CHECK(config("ifconfig %s alias 10.81.2.2/24", tap_name) == 0);
	CHECK(sh("ping -c 1 -W 2 10.81.1.2 >/dev/null 2>&1") == 0);
}

static void test_dscp(void)
{
	int lx = lx_udp(7001, 1), tos, r, ls, s;
	u32 src;
	struct rt_sockaddr_in sin;
	s32 v = 6;

	CHECK(lx >= 0);
	CHECK(rt_udp_to("10.81.1.1", 7001, 6, -1) > 0);           /* priority 6: EF */
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && tos == 0xb8, "r %d tos %#x", r, tos);
	CHECK(rt_udp_to("10.81.1.1", 7001, 6, 0x10) > 0);         /* IP_TOS wins */
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && tos == 0x10, "r %d tos %#x", r, tos);
	CHECK(rt_udp_to("10.81.1.1", 7001, -1, -1) > 0);          /* nothing: 0 */
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && tos == 0, "r %d tos %#x", r, tos);
	CHECK(config("qos rule rport=7001 dscp=CS1") == 0);       /* a rule */
	CHECK(rt_udp_to("10.81.1.1", 7001, -1, -1) > 0);
	r = lx_recv(lx, 2000, &src, &tos);
	CHECKV(r > 0 && tos == 0x20, "r %d tos %#x", r, tos);
	CHECK(config("qos rules flush") == 0);
	close(lx);
	/* a TCP SYN carries the mark */
	ls = lx_listen(AF_INET, "10.81.1.1", 7003);
	CHECK(ls >= 0);
	s = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_PRIORITY, &v, sizeof(v));
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(7003);
	sin.sin_addr = A("10.81.1.1");
	CHECK(rt_api_connect(&ctx, s, &sin, sizeof(sin)) == 0);
	tos = syn_tos(2000);
	CHECKV((tos & 0xfc) == 0xb8, "tos %#x", tos);
	rt_api_close(&ctx, s);
	close(ls);
}

static void test_happy_eyeballs(void)
{
	struct rt_he_result res;
	int l4 = lx_listen(AF_INET, "10.81.1.1", 7100), fd;
	long t0;
	rt_in6_addr dead;

	CHECK(l4 >= 0);
	inet_pton(AF_INET6, "fd00:81::99", dead.b);     /* nobody answers its NS */
	rt_lock();
	CHECK(rt_hosts_add6("he1.test", &dead) == 0 && rt_hosts_add("he1.test", A("10.81.1.1")) == 0);
	rt_unlock();

	/* IPv6 first, but it never answers: IPv4 an attempt delay later */
	rt_he_aaaa_filter = 0;
	rt_he_prefer = 6;
	t0 = ms_now();
	fd = rt_he_connect(&ctx, "he1.test", 7100, RT_AF_UNSPEC, 0, 5000, &res);
	CHECKV(fd >= 0, "fd %d", fd);
	CHECKV(rt_in6_is_v4mapped(&res.addr) && res.index == 1 && res.attempts == 2 && res.naddrs == 2,
	       "index %u attempts %u naddrs %u", res.index, res.attempts, res.naddrs);
	CHECKV(res.ms >= 200 && res.ms < 1500 && ms_now() - t0 < 2000, "%lu ms", (unsigned long)res.ms);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	/* the AAAA filter: no IPv6 route to the Internet, no IPv6 attempt */
	rt_he_aaaa_filter = 1;
	rt_lock();
	CHECK(!rt_he_v6_usable());
	rt_unlock();
	fd = rt_he_connect(&ctx, "he1.test", 7100, RT_AF_UNSPEC, 0, 5000, &res);
	CHECKV(fd >= 0 && res.attempts == 1 && res.naddrs == 1 && res.ms < 200, "fd %d attempts %u ms %lu", fd,
	       res.attempts, (unsigned long)res.ms);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	rt_he_aaaa_filter = 0;
	/* no preference: RFC 6724 puts IPv4 before a ULA, the first try works */
	CHECK(config("happy_eyeballs prefer auto") == 0 && rt_he_prefer == 0);
	fd = rt_he_connect(&ctx, "he1.test", 7100, RT_AF_UNSPEC, 0, 5000, &res);
	CHECKV(fd >= 0 && rt_in6_is_v4mapped(&res.addr) && res.index == 0 && res.attempts == 1 && res.ms < 200,
	       "fd %d index %u attempts %u %lu ms", fd, res.index, res.attempts, (unsigned long)res.ms);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	/* Happy Eyeballs off: one address after the other, IPv6 first */
	CHECK(config("happy_eyeballs prefer ipv6") == 0 && rt_he_prefer == 6);
	CHECK(config("happy_eyeballs off") == 0 && !rt_he_enable);
	t0 = ms_now();
	fd = rt_he_connect(&ctx, "he1.test", 7100, RT_AF_UNSPEC, 0, 4000, &res);
	CHECKV(fd >= 0 && res.index == 1 && ms_now() - t0 >= 1900, "fd %d index %u %ld ms", fd, res.index,
	       ms_now() - t0);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	CHECK(config("happy_eyeballs on") == 0 && rt_he_enable);
	/* literals and errors */
	fd = rt_he_connect(&ctx, "10.81.1.1", 7100, RT_AF_UNSPEC, 0, 5000, &res);
	CHECK(fd >= 0 && res.naddrs == 1);
	if (fd >= 0)
		rt_api_close(&ctx, fd);
	CHECK(rt_he_connect(&ctx, "10.81.1.1", 7199, RT_AF_UNSPEC, 0, 5000, NULL) == -RT_ECONNREFUSED);
	CHECK(rt_he_connect(&ctx, "10.81.1.1", 7100, RT_AF_INET6, 0, 5000, NULL) == -RT_EAFNOSUPPORT);
	CHECK(config("happy_eyeballs prefer auto") == 0 && config("happy_eyeballs delay 5") < 0);
	rt_he_prefer = 0;
	close(l4);
}

static void test_parallel_dns(void)
{
	rt_in6_addr a[8];
	char canon[64];
	long t0;
	int n;

	/* the AAAA answer never comes: A alone, about 50 ms after it */
	aaaa_delay_ms = -1;
	a_delay_ms = 0;
	t0 = ms_now();
	n = rt_resolve_name46("slow6.test", a, 8, RT_AF_UNSPEC, canon, sizeof(canon));
	CHECKV(n == 1 && rt_in6_is_v4mapped(&a[0]) && ms_now() - t0 < 600, "n %d %ld ms", n, ms_now() - t0);
	/* the A answer is 300 ms late: waited for, both */
	aaaa_delay_ms = 0;
	a_delay_ms = 300;
	t0 = ms_now();
	n = rt_resolve_name46("slow4.test", a, 8, RT_AF_UNSPEC, canon, sizeof(canon));
	CHECKV(n == 2 && !rt_in6_is_v4mapped(&a[0]) && rt_in6_is_v4mapped(&a[1]) && ms_now() - t0 >= 250,
	       "n %d %ld ms", n, ms_now() - t0);
	CHECK(!strcmp(canon, "slow4.test"));
	/* the cache: no waiting */
	t0 = ms_now();
	n = rt_resolve_name46("slow4.test", a, 8, RT_AF_UNSPEC, NULL, 0);
	CHECKV(n == 2 && ms_now() - t0 < 50, "n %d %ld ms", n, ms_now() - t0);
	/* one family */
	a_delay_ms = 0;
	n = rt_resolve_name46("only4.test", a, 8, RT_AF_INET, NULL, 0);
	CHECK(n == 1 && rt_in6_is_v4mapped(&a[0]));
	n = rt_resolve_name46("nosuch.example", a, 8, RT_AF_UNSPEC, NULL, 0);
	CHECKV(n == -RT_HOST_NOT_FOUND, "n %d", n);
}

int main(void)
{
	struct sockaddr_ll ll;
	struct sockaddr_in sin;
	pthread_t dt;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0 || system("ping -V >/dev/null 2>&1") != 0) {
		printf("t_ip_tap: SKIP (needs root, /dev/net/tun, iproute2 and ping)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(180);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tap_name, sizeof(tap_name), "rth%d", (int)(getpid() % 100000));
	tap = rt_host_tap_open(tap_name, mac);
	if (!tap) {
		printf("t_ip_tap: SKIP (cannot create TAP device)\n");
		return 77;
	}
	/* Linux needs no IPv6 (the container kernel may have none): Riptide's
	 * IPv6 attempts to fd00:81::99 go unanswered either way */
	if (sh("ip addr add 10.81.1.1/24 dev %s && ip addr add 10.81.2.1/24 dev %s && ip link set %s up",
	       tap_name, tap_name, tap_name)) {
		printf("t_ip_tap: SKIP (cannot configure %s)\n", tap_name);
		return 77;
	}
	psock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (psock < 0) {
		printf("t_ip_tap: SKIP (no packet socket)\n");
		return 77;
	}
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(ETH_P_IP);
	ll.sll_ifindex = (int)if_nametoindex(tap_name);
	CHECK(bind(psock, (struct sockaddr *)&ll, sizeof(ll)) == 0);
	dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(53);
	sin.sin_addr.s_addr = A("10.81.1.1");
	if (bind(dns_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		printf("t_ip_tap: SKIP (cannot bind 10.81.1.1:53)\n");
		return 77;
	}
	pthread_create(&dt, NULL, dns_thread, NULL);
	rt_ctx_init(&ctx, 32, NULL);
	CHECK(config("ifconfig %s 10.81.1.2/24 up", tap_name) == 0);
	CHECK(config("ifconfig %s alias 10.81.2.2/24", tap_name) == 0);
	CHECK(config("ifconfig %s inet6 fd00:81::2/64 nodad", tap_name) == 0);
	CHECK(config("nameserver 10.81.1.1") == 0);
	usleep(300000);

	test_addresses();
	test_dscp();
	test_happy_eyeballs();
	test_parallel_dns();

	dns_stop = 1;
	pthread_join(dt, NULL);
	close(dns_sock);
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	close(psock);
	printf("t_ip_tap: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
