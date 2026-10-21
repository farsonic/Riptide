/*
 * Riptide - Ethernet interop with the Linux kernel over TAP (needs root,
 * /dev/net/tun and iproute2; exits 77 = skip otherwise).
 *
 *   Linux 10.78.7.1/24, 2001:db8:78::1/64  <-- tap -->  Riptide 10.78.7.2, 2001:db8:78::2
 *
 * IPv4: ARP both ways and TCP in both directions with data checks. IPv6
 * (only when the kernel has IPv6, /proc/net/if_inet6): the kernel's own
 * NDP resolves Riptide and vice versa, DAD runs against a real neighbour,
 * TCP in both directions, UDP, and ICMPv6 echo to the kernel's link-local
 * address. Without kernel IPv6 that part is reported as skipped.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rt_sockctx ctx;
static char tap_name[16];
static struct rt_netif *tap;
static const u8 mac[6] = { 0x02, 0x52, 0x54, 0x00, 0x78, 0x02 };

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static u8 pat(u32 i) { return (u8)(i * 13 + (i >> 8)); }

/* ---- Linux side: a one-shot echo server thread ---- */
struct lx_srv { int lfd; int ok; };

static void *lx_echo(void *p)
{
	struct lx_srv *s = p;
	struct pollfd pf = { s->lfd, POLLIN, 0 };
	char buf[8192];
	int fd;
	ssize_t n;

	if (poll(&pf, 1, 10000) <= 0)
		return NULL;
	fd = accept(s->lfd, NULL, NULL);
	if (fd < 0)
		return NULL;
	while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
		if (send(fd, buf, (size_t)n, 0) != n)
			break;
	s->ok = n == 0;
	close(fd);
	return NULL;
}

static int lx_listen(int af, const char *addr, u16 port)
{
	struct sockaddr_storage ss;
	socklen_t sl;
	int one = 1, fd = socket(af, SOCK_STREAM, 0);

	memset(&ss, 0, sizeof(ss));
	if (af == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;
		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		inet_pton(AF_INET, addr, &a->sin_addr);
		sl = sizeof(*a);
	} else {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		inet_pton(AF_INET6, addr, &a->sin6_addr);
		sl = sizeof(*a);
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&ss, sl) < 0 || listen(fd, 4) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Riptide connects to Linux's echo server, sends n bytes, reads them back */
static void riptide_to_linux(int af, const char *laddr, u16 port, u32 n)
{
	struct lx_srv s = { -1, 0 };
	pthread_t th;
	struct rt_sockaddr_in6 a6;
	struct rt_sockaddr_in a4;
	static u8 buf[16384];
	u32 sent = 0, got = 0;
	int fd, ok = 1, r;

	s.lfd = lx_listen(af, laddr, port);
	CHECK(s.lfd >= 0);
	if (s.lfd < 0)
		return;
	pthread_create(&th, NULL, lx_echo, &s);
	fd = rt_api_socket(&ctx, af == AF_INET ? RT_AF_INET : RT_AF_INET6, RT_SOCK_STREAM, 0);
	{
		struct rt_timeval tv = { 10, 0 };
		rt_api_setsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	}
	if (af == AF_INET) {
		memset(&a4, 0, sizeof(a4));
		a4.sin_len = sizeof(a4);
		a4.sin_family = RT_AF_INET;
		a4.sin_port = rt_htons(port);
		rt_parse_ipv4(laddr, &a4.sin_addr);
		r = rt_api_connect(&ctx, fd, &a4, sizeof(a4));
	} else {
		memset(&a6, 0, sizeof(a6));
		a6.sin6_len = sizeof(a6);
		a6.sin6_family = RT_AF_INET6;
		a6.sin6_port = rt_htons(port);
		rt_inet6_pton(laddr, &a6.sin6_addr);
		r = rt_api_connect(&ctx, fd, &a6, sizeof(a6));
	}
	CHECK(r == 0);
	while (r == 0 && got < n) {
		if (sent < n) {
			u32 k, m = n - sent > 4096 ? 4096 : n - sent;
			for (k = 0; k < m; k++)
				buf[k] = pat(sent + k);
			r = rt_api_send(&ctx, fd, buf, m, 0);
			if (r <= 0)
				break;
			sent += (u32)r;
			if (sent == n)
				rt_api_shutdown(&ctx, fd, 1);
			r = 0;
		}
		{
			int k = rt_api_recv(&ctx, fd, buf, sizeof(buf), sent < n ? RT_MSG_DONTWAIT : 0);
			if (k == -RT_EWOULDBLOCK || k == -RT_EAGAIN)
				continue;
			if (k <= 0)
				break;
			for (r = 0; r < k; r++)
				if (buf[r] != pat(got + (u32)r))
					ok = 0;
			got += (u32)k;
			r = 0;
		}
	}
	CHECK(got == n && ok);
	rt_api_close(&ctx, fd);
	pthread_join(th, NULL);
	CHECK(s.ok);
	close(s.lfd);
}

/* Linux connects to a Riptide listener and sends n bytes */
struct lx_cli { int af; const char *addr; u16 port; u32 n; int ok; };

static void *lx_client(void *p)
{
	struct lx_cli *c = p;
	struct sockaddr_storage ss;
	socklen_t sl;
	static char buf[4096];
	u32 sent = 0;
	int fd = socket(c->af, SOCK_STREAM, 0), i;

	memset(&ss, 0, sizeof(ss));
	if (c->af == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;
		a->sin_family = AF_INET;
		a->sin_port = htons(c->port);
		inet_pton(AF_INET, c->addr, &a->sin_addr);
		sl = sizeof(*a);
	} else {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(c->port);
		inet_pton(AF_INET6, c->addr, &a->sin6_addr);
		sl = sizeof(*a);
	}
	if (connect(fd, (struct sockaddr *)&ss, sl) < 0) {
		close(fd);
		return NULL;
	}
	while (sent < c->n) {
		u32 m = c->n - sent > sizeof(buf) ? sizeof(buf) : c->n - sent;
		for (i = 0; i < (int)m; i++)
			buf[i] = (char)pat(sent + (u32)i);
		if (send(fd, buf, m, 0) != (ssize_t)m)
			break;
		sent += m;
	}
	c->ok = sent == c->n;
	close(fd);
	return NULL;
}

static void linux_to_riptide(int af, const char *laddr, const char *raddr, u16 port, u32 n, int v6listener)
{
	struct lx_cli c = { af, raddr, port, n, 0 };
	pthread_t th;
	struct rt_sockaddr_in6 a;
	static u8 buf[16384];
	u32 got = 0, len = sizeof(a);
	int lfd, fd, ok = 1, k;

	(void)laddr;
	memset(&a, 0, sizeof(a));
	if (v6listener) {
		a.sin6_len = sizeof(a);
		a.sin6_family = RT_AF_INET6;
		a.sin6_port = rt_htons(port);     /* [::]: dual-stack */
		lfd = rt_api_socket(&ctx, RT_AF_INET6, RT_SOCK_STREAM, 0);
	} else {
		struct rt_sockaddr_in *a4 = (struct rt_sockaddr_in *)&a;
		a4->sin_len = sizeof(*a4);
		a4->sin_family = RT_AF_INET;
		a4->sin_port = rt_htons(port);
		lfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	}
	CHECK(rt_api_bind(&ctx, lfd, &a, v6listener ? sizeof(a) : sizeof(struct rt_sockaddr_in)) == 0);
	CHECK(rt_api_listen(&ctx, lfd, 4) == 0);
	pthread_create(&th, NULL, lx_client, &c);
	fd = rt_api_accept(&ctx, lfd, &a, &len);
	CHECK(fd >= 0);
	if (fd >= 0) {
		struct rt_timeval tv = { 10, 0 };
		rt_api_setsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		if (v6listener && af == AF_INET)
			CHECK(a.sin6_family == RT_AF_INET6 && rt_in6_is_v4mapped(&a.sin6_addr));
		while ((k = rt_api_recv(&ctx, fd, buf, sizeof(buf), 0)) > 0) {
			int i;
			for (i = 0; i < k; i++)
				if (buf[i] != pat(got + (u32)i))
					ok = 0;
			got += (u32)k;
		}
		rt_api_close(&ctx, fd);
	}
	pthread_join(th, NULL);
	CHECK(c.ok && got == n && ok);
	rt_api_close(&ctx, lfd);
}

static int nd_has(const char *addr, int want_router)
{
	struct rt_nd6_info ni[16];
	rt_in6_addr a;
	int n, i, r = 0;

	rt_inet6_pton(addr, &a);
	rt_lock();
	n = rt_nd6_list(ni, 16);
	for (i = 0; i < n && i < 16; i++)
		if (rt_in6_eq(&ni[i].addr, &a) && ni[i].state != RT_NUD_INCOMPLETE && ni[i].state != RT_NUD_FAILED &&
		    (!want_router || ni[i].router))
			r = 1;
	rt_unlock();
	return r;
}

static void test_ipv6(void)
{
	rt_in6_addr me;
	char lx_ll[64] = "";
	FILE *f;
	int i;

	rt_inet6_pton("2001:db8:78::2", &me);
	rt_lock();
	CHECK(rt_in6_addr_add(tap, &me, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	rt_unlock();
	CHECK(sh("ip -6 addr add 2001:db8:78::1/64 dev %s nodad", tap_name) == 0);
	/* our DAD (against a real neighbour) then the kernel's NDP */
	for (i = 0; i < 40; i++) {
		struct rt_in6_ifaddr_info ai[8];
		int n, k, ready = 0;
		usleep(100000);
		rt_lock();
		n = rt_in6_addr_list(tap, ai, 8);
		for (k = 0; k < n && k < 8; k++)
			if (rt_in6_eq(&ai[k].addr, &me) && !(ai[k].flags & RT_IN6_IFF_TENTATIVE))
				ready = 1;
		rt_unlock();
		if (ready)
			break;
	}
	CHECK(i < 40);
	riptide_to_linux(AF_INET6, "2001:db8:78::1", 9101, 1u << 20);
	CHECK(nd_has("2001:db8:78::1", 0));
	linux_to_riptide(AF_INET6, NULL, "2001:db8:78::2", 9102, 1u << 20, 1);
	/* the kernel resolved us with NDP */
	CHECK(sh("ip -6 neigh show dev %s | grep -q '2001:db8:78::2 .*lladdr 02:52:54:00:78:02'", tap_name) == 0);
	/* ICMPv6 echo to the kernel's link-local address (found via ip -6 addr) */
	{
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "ip -6 addr show dev %s scope link | sed -n 's/.*inet6 \\(fe80[^/]*\\).*/\\1/p'",
		         tap_name);
		f = popen(cmd, "r");
		if (f) {
			if (!fgets(lx_ll, sizeof(lx_ll), f))
				lx_ll[0] = 0;
			pclose(f);
		}
		lx_ll[strcspn(lx_ll, "\n")] = 0;
	}
	CHECK(lx_ll[0] != 0);
	if (lx_ll[0]) {
		struct rt_sockaddr_in6 to;
		u8 pkt[64], rb[256];
		int s = rt_api_socket(&ctx, RT_AF_INET6, RT_SOCK_RAW, RT_IPPROTO_ICMPV6), r, got = 0;
		struct rt_timeval tv = { 3, 0 };
		rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		memset(&to, 0, sizeof(to));
		to.sin6_len = sizeof(to);
		to.sin6_family = RT_AF_INET6;
		rt_inet6_pton(lx_ll, &to.sin6_addr);
		to.sin6_scope_id = tap->index;
		memset(pkt, 0xa5, sizeof(pkt));
		pkt[0] = RT_ICMP6_ECHO_REQUEST;
		pkt[1] = 0;
		rt_put_be16(pkt + 4, 0x7878);
		rt_put_be16(pkt + 6, 1);
		CHECK(rt_api_sendto(&ctx, s, pkt, sizeof(pkt), 0, &to, sizeof(to)) == (int)sizeof(pkt));
		while ((r = rt_api_recv(&ctx, s, rb, sizeof(rb), 0)) > 0)
			if (r == (int)sizeof(pkt) && rb[0] == RT_ICMP6_ECHO_REPLY && rt_get_be16(rb + 4) == 0x7878) {
				got = 1;
				break;
			}
		CHECK(got);
		rt_api_close(&ctx, s);
		printf("t_tap: ICMPv6 echo to %s%%%s %s\n", lx_ll, tap_name, got ? "answered" : "FAILED");
	}
}

/* net.riptide.drivers after 2.25 MB went both ways */
static void check_drvstats(void)
{
	static u8 buf[4096];
	struct rt_ctl_hdr h;
	const struct rt_ctl_driver *r = NULL;
	u32 len = sizeof(buf), i, j;

	u32 opackets;

	rt_lock();
	CHECK(rt_sysctl_byname("net.riptide.drivers", buf, &len, NULL, 0) == 0);
	opackets = tap->stats.opackets;
	rt_unlock();
	memcpy(&h, buf, sizeof(h));
	for (i = 0; i < h.count; i++) {
		const struct rt_ctl_driver *x = (const void *)(buf + sizeof(h) + i * h.recsize);
		if (!strcmp(x->name, tap->name))
			r = x;
	}
	CHECK(r != NULL);
	if (!r)
		return;
	CHECK(!strcmp(r->kind, "tap") && strstr(r->driver, tap_name) != NULL);
	CHECK(r->flags & RT_CTL_DRVF_NOQ);
	CHECK(r->rx_frames > 1000 && r->rx_bytes > (2u << 20));
	CHECK(r->tx_frames > 700 && r->tx_bytes > (1u << 20));   /* 1 MB of data; ACK count varies */
	CHECK(r->tx_inflight == 0 && r->tx_errors == 0);
	CHECK(r->tx_frames == opackets);   /* the interface counts every frame once */
	for (i = j = 0; i < RT_CTL_DRV_BATCH; i++)
		j += r->batch[i];
	CHECK(j > 0 && r->batch_max >= 1);
	printf("t_tap: driver counters: rx %lu frames, tx %lu frames, %lu wakeups, at most %lu frames per wakeup\n",
	       (unsigned long)r->rx_frames, (unsigned long)r->tx_frames, (unsigned long)j,
	       (unsigned long)r->batch_max);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_tap: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(300);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tap_name, sizeof(tap_name), "rtap%d", (int)(getpid() % 100000));
	tap = rt_host_tap_open(tap_name, mac);
	if (!tap) {
		printf("t_tap: SKIP (cannot create TAP device)\n");
		return 77;
	}
	if (sh("ip addr add 10.78.7.1/24 dev %s && ip link set %s up", tap_name, tap_name)) {
		printf("t_tap: SKIP (cannot configure %s)\n", tap_name);
		return 77;
	}
	rt_ctx_init(&ctx, 32, NULL);
	rt_lock();
	CHECK(rt_netif_set_addr(tap, rt_htonl(0x0a4e0702u), rt_htonl(0xffffff00u), 0) == 0);
	rt_netif_up(tap);
	rt_unlock();

	/* IPv4 over Ethernet: ARP both ways, TCP both directions */
	riptide_to_linux(AF_INET, "10.78.7.1", 9001, 1u << 20);
	linux_to_riptide(AF_INET, NULL, "10.78.7.2", 9002, 1u << 20, 0);
	CHECK(sh("ip neigh show dev %s | grep -q '10.78.7.2 .*lladdr 02:52:54:00:78:02'", tap_name) == 0);
	/* an AF_INET6 listener on [::] takes the IPv4 client (dual-stack) */
	linux_to_riptide(AF_INET, NULL, "10.78.7.2", 9003, 256u << 10, 1);
	check_drvstats();

	if (access("/proc/net/if_inet6", R_OK) == 0)
		test_ipv6();
	else
		printf("t_tap: IPv6 part skipped (this kernel has no IPv6)\n");

	rt_ctx_destroy(&ctx);
	rt_host_stop();
	printf("t_tap: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
