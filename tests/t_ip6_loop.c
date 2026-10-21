/*
 * Riptide - IPv6 sockets over lo0 (::1) through the descriptor API with
 * the host stack thread running (no root needed): UDP, TCP bulk with data
 * integrity, dual-stack listeners (IPv4-mapped peers, IPV6_V6ONLY), bind
 * conflicts across families, raw ICMPv6 echo, ICMPv6 port unreachable,
 * sockaddr_in6 lengths (28 and the 24 byte RFC 2133 form) and the
 * resolver's AAAA/PTR shortcuts.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rt_sockctx c, c2;

static struct rt_sockaddr_in6 sa6(const char *addr, u16 port)
{
	struct rt_sockaddr_in6 s;

	memset(&s, 0, sizeof(s));
	s.sin6_len = sizeof(s);
	s.sin6_family = RT_AF_INET6;
	s.sin6_port = rt_htons(port);
	if (rt_inet6_pton(addr, &s.sin6_addr) < 0)
		fprintf(stderr, "bad address %s\n", addr);
	return s;
}

static struct rt_sockaddr_in sa4(u32 host_order_addr, u16 port)
{
	struct rt_sockaddr_in s;

	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = rt_htonl(host_order_addr);
	s.sin_port = rt_htons(port);
	return s;
}

static void set_timeout(struct rt_sockctx *x, int fd, u32 ms)
{
	struct rt_timeval tv;

	tv.tv_secs = ms / 1000;
	tv.tv_micro = (ms % 1000) * 1000;
	rt_api_setsockopt(x, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	rt_api_setsockopt(x, fd, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv));
}

static int addr_is(const struct rt_sockaddr_in6 *s, const char *a)
{
	rt_in6_addr x;

	return rt_inet6_pton(a, &x) == 0 && rt_in6_eq(&x, &s->sin6_addr);
}

static void test_basics(void)
{
	rt_in6_addr a;
	char buf[RT_INET6_ADDRSTRLEN];

	rt_lock();
	CHECK(rt_ip6_ifenabled(rt_loopback_if));
	CHECK(rt_ip6_is_local(&rt_in6addr_loopback));
	CHECK(rt_ip6_select_source(&rt_in6addr_loopback, NULL, &a) == 0 && rt_in6_is_loopback(&a));
	rt_unlock();
	CHECK(rt_resolve_name6("localhost", &a, 1, buf, sizeof(buf)) == 1 && rt_in6_is_loopback(&a));
	CHECK(rt_resolve_name6("2001:db8::7", &a, 1, NULL, 0) == 1 && a.b[15] == 7);
	CHECK(rt_resolve_name6("10.1.2.3", &a, 1, NULL, 0) == -RT_NO_DATA);
	CHECK(rt_resolve_addr6(&rt_in6addr_loopback, buf, sizeof(buf)) == 0 && !strcmp(buf, "localhost"));
	rt_lock();
	CHECK(rt_inet6_pton("2001:db8::99", &a) == 0 && rt_hosts_add6("six.lan", &a) == 0);
	rt_unlock();
	CHECK(rt_resolve_name6("six.lan", &a, 1, NULL, 0) == 1 && a.b[15] == 0x99);
	CHECK(rt_resolve_addr6(&a, buf, sizeof(buf)) == 0 && !strcmp(buf, "six.lan"));
	{
		u32 v4;
		CHECK(rt_resolve_name("six.lan", &v4, 1, NULL, 0) <= 0);   /* no A record */
	}
}

static void test_udp(void)
{
	struct rt_sockaddr_in6 a, from;
	struct rt_sockaddr_in a4;
	u32 flen, alen;
	char buf[256];
	int s1, s2, s4, r, one = 1;

	s1 = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	s2 = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	CHECK(s1 >= 0 && s2 >= 0);
	set_timeout(&c, s1, 2000);
	set_timeout(&c, s2, 2000);
	a = sa6("::1", 6001);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	CHECK(rt_api_bind(&c, s2, &a, sizeof(a)) == -RT_EADDRINUSE);
	a = sa6("2001:db8::1", 6002);
	CHECK(rt_api_bind(&c, s2, &a, sizeof(a)) == -RT_EADDRNOTAVAIL);
	/* a sockaddr_in on an AF_INET6 socket is refused */
	a4 = sa4(0x7f000001, 6001);
	CHECK(rt_api_sendto(&c, s2, "x", 1, 0, &a4, sizeof(a4)) == -RT_EINVAL ||
	      rt_api_sendto(&c, s2, "x", 1, 0, &a4, sizeof(a4)) == -RT_EAFNOSUPPORT);
	a = sa6("::1", 6001);
	CHECK(rt_api_sendto(&c, s2, "hello6", 6, 0, &a, sizeof(a)) == 6);
	flen = sizeof(from);
	memset(&from, 0, sizeof(from));
	r = rt_api_recvfrom(&c, s1, buf, sizeof(buf), 0, &from, &flen);
	CHECK(r == 6 && !memcmp(buf, "hello6", 6));
	CHECK(flen == 28 && from.sin6_len == 28 && from.sin6_family == RT_AF_INET6 &&
	      addr_is(&from, "::1") && from.sin6_port != 0);
	/* RFC 2133 sized buffer: 24 bytes */
	CHECK(rt_api_sendto(&c, s2, "again", 5, 0, &a, 24) == 5);
	flen = 24;
	memset(&from, 0xee, sizeof(from));
	r = rt_api_recvfrom(&c, s1, buf, sizeof(buf), 0, &from, &flen);
	CHECK(r == 5 && flen == 24 && from.sin6_len == 24 && addr_is(&from, "::1") &&
	      from.sin6_scope_id == 0xeeeeeeeeu);
	alen = sizeof(a);
	CHECK(rt_api_getsockname(&c, s2, &a, &alen) == 0 && alen == 28 && a.sin6_family == RT_AF_INET6);

	/* connected socket, port unreachable from ICMPv6 */
	a = sa6("::1", 6009);
	CHECK(rt_api_connect(&c, s2, &a, sizeof(a)) == 0);
	CHECK(rt_api_send(&c, s2, "x", 1, 0) == 1);
	r = rt_api_recv(&c, s2, buf, sizeof(buf), 0);
	CHECK(r == -RT_ECONNREFUSED);
	alen = sizeof(a);
	CHECK(rt_api_getpeername(&c, s2, &a, &alen) == 0 && addr_is(&a, "::1") && a.sin6_port == rt_htons(6009));

	/* dual stack: an AF_INET6 socket on :: receives IPv4, the peer is mapped */
	s4 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	set_timeout(&c, s4, 2000);
	rt_api_close(&c, s1);
	s1 = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	set_timeout(&c, s1, 2000);
	a = sa6("::", 6003);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	a4 = sa4(0x7f000001, 6003);
	CHECK(rt_api_sendto(&c, s4, "four", 4, 0, &a4, sizeof(a4)) == 4);
	flen = sizeof(from);
	r = rt_api_recvfrom(&c, s1, buf, sizeof(buf), 0, &from, &flen);
	CHECK(r == 4 && addr_is(&from, "::ffff:127.0.0.1"));
	/* and answers it through the mapped address */
	CHECK(rt_api_sendto(&c, s1, "back", 4, 0, &from, flen) == 4);
	{
		struct rt_sockaddr_in f4;
		u32 l4 = sizeof(f4);
		r = rt_api_recvfrom(&c, s4, buf, sizeof(buf), 0, &f4, &l4);
		CHECK(r == 4 && l4 == 16 && f4.sin_family == RT_AF_INET &&
		      f4.sin_addr == rt_htonl(0x7f000001) && f4.sin_port == rt_htons(6003));
	}
	/* IPV6_V6ONLY: only before bind; then no IPv4 */
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one)) == -RT_EINVAL);
	rt_api_close(&c, s1);
	s1 = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one)) == 0);
	a = sa6("::", 6003);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	a = sa6("::ffff:127.0.0.1", 6003);
	CHECK(rt_api_sendto(&c, s1, "x", 1, 0, &a, sizeof(a)) == -RT_EAFNOSUPPORT);
	/* an AF_INET socket may take the IPv4 side of the port now */
	rt_api_close(&c, s4);
	s4 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	a4 = sa4(0, 6003);
	CHECK(rt_api_bind(&c, s4, &a4, sizeof(a4)) == 0);
	rt_api_close(&c, s4);
	rt_api_close(&c, s1);
	/* but not next to a dual-stack one */
	s1 = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	a = sa6("::", 6004);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	s4 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	a4 = sa4(0, 6004);
	CHECK(rt_api_bind(&c, s4, &a4, sizeof(a4)) == -RT_EADDRINUSE);
	rt_api_close(&c, s4);
	rt_api_close(&c, s1);
	rt_api_close(&c, s2);
}

static void test_raw_ping(void)
{
	struct rt_sockaddr_in6 a, from;
	u8 pkt[64], buf[256];
	u32 flen;
	int s, r, i;

	s = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_RAW, RT_IPPROTO_ICMPV6);
	CHECK(s >= 0);
	set_timeout(&c, s, 2000);
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = RT_ICMP6_ECHO_REQUEST;
	rt_put_be16(pkt + 4, 0x1234);
	rt_put_be16(pkt + 6, 1);
	for (i = 8; i < (int)sizeof(pkt); i++)
		pkt[i] = (u8)i;
	a = sa6("::1", 0);
	CHECK(rt_api_sendto(&c, s, pkt, sizeof(pkt), 0, &a, sizeof(a)) == (int)sizeof(pkt));
	/* we see our own request too (raw sockets get every ICMPv6 message) */
	for (i = 0; i < 3; i++) {
		flen = sizeof(from);
		r = rt_api_recvfrom(&c, s, buf, sizeof(buf), 0, &from, &flen);
		if (r <= 0 || buf[0] == RT_ICMP6_ECHO_REPLY)
			break;
	}
	CHECK(r == (int)sizeof(pkt) && buf[0] == RT_ICMP6_ECHO_REPLY && rt_get_be16(buf + 4) == 0x1234 &&
	      !memcmp(buf + 8, pkt + 8, sizeof(pkt) - 8) && addr_is(&from, "::1"));
	/* ICMP6_FILTER: block echo requests, pass replies */
	{
		u32 filt[8];
		memset(filt, 0, sizeof(filt));
		filt[RT_ICMP6_ECHO_REPLY >> 5] |= 1u << (RT_ICMP6_ECHO_REPLY & 31);
		CHECK(rt_api_setsockopt(&c, s, RT_IPPROTO_ICMPV6, RT_ICMP6_FILTER, filt, sizeof(filt)) == 0);
		CHECK(rt_api_sendto(&c, s, pkt, sizeof(pkt), 0, &a, sizeof(a)) == (int)sizeof(pkt));
		flen = sizeof(from);
		r = rt_api_recvfrom(&c, s, buf, sizeof(buf), 0, &from, &flen);
		CHECK(r == (int)sizeof(pkt) && buf[0] == RT_ICMP6_ECHO_REPLY);
	}
	rt_api_close(&c, s);
}

/* ---- TCP ---- */

struct bulk {
	struct rt_sockctx *ctx;
	int fd;
	u32 total;
	u32 sum;
	int ok;
};

static u8 pattern(u32 i) { return (u8)(i * 7 + (i >> 9)); }

static void *bulk_rx(void *p)
{
	struct bulk *b = p;
	static u8 buf[8192];
	u32 got = 0;
	int r;

	b->ok = 1;
	while (got < b->total) {
		u32 k;
		r = rt_api_recv(b->ctx, b->fd, buf, sizeof(buf), 0);
		if (r <= 0) {
			b->ok = 0;
			break;
		}
		for (k = 0; k < (u32)r; k++)
			if (buf[k] != pattern(got + k))
				b->ok = 0;
		got += (u32)r;
	}
	b->sum = got;
	return NULL;
}

static int tx_all(struct rt_sockctx *x, int fd, u32 total)
{
	static u8 buf[16384];
	u32 sent = 0;

	while (sent < total) {
		u32 n = total - sent > sizeof(buf) ? sizeof(buf) : total - sent, k;
		int r;
		for (k = 0; k < n; k++)
			buf[k] = pattern(sent + k);
		r = rt_api_send(x, fd, buf, n, 0);
		if (r <= 0)
			return -1;
		sent += (u32)r;
	}
	return 0;
}

static int accept_wait(struct rt_sockctx *x, int lfd, void *a, u32 *alen)
{
	set_timeout(x, lfd, 3000);
	return rt_api_accept(x, lfd, a, alen);
}

static void test_tcp(void)
{
	struct rt_sockaddr_in6 a, peer, me;
	struct rt_sockaddr_in a4;
	u32 alen;
	int lfd, cfd, afd, one = 1, mss;
	struct bulk bk;
	pthread_t th;

	lfd = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	a = sa6("::", 6100);
	CHECK(rt_api_setsockopt(&c, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one)) == 0);
	CHECK(rt_api_bind(&c, lfd, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&c, lfd, 8) == 0);

	/* IPv6 client */
	cfd = rt_api_socket(&c2, RT_AF_INET6, RT_SOCK_STREAM, 0);
	set_timeout(&c2, cfd, 5000);
	a = sa6("::1", 6100);
	CHECK(rt_api_connect(&c2, cfd, &a, sizeof(a)) == 0);
	alen = sizeof(peer);
	afd = accept_wait(&c, lfd, &peer, &alen);
	CHECK(afd >= 0 && alen == 28 && peer.sin6_family == RT_AF_INET6 && addr_is(&peer, "::1"));
	alen = sizeof(me);
	CHECK(rt_api_getsockname(&c2, cfd, &me, &alen) == 0 && addr_is(&me, "::1") &&
	      peer.sin6_port == me.sin6_port);
	{
		u32 l = sizeof(mss);
		CHECK(rt_api_getsockopt(&c2, cfd, RT_IPPROTO_TCP, RT_TCP_MAXSEG, &mss, &l) == 0);
		/* lo0 MTU 16384 - 40 (IPv6) - 20 (TCP) - 12 (timestamps) */
		CHECK(mss == 16384 - 60 - 12);
	}
	set_timeout(&c, afd, 5000);
	/* 1 MB each way, checked byte by byte */
	bk.ctx = &c;
	bk.fd = afd;
	bk.total = 1u << 20;
	pthread_create(&th, NULL, bulk_rx, &bk);
	CHECK(tx_all(&c2, cfd, bk.total) == 0);
	pthread_join(th, NULL);
	CHECK(bk.ok && bk.sum == bk.total);
	bk.ctx = &c2;
	bk.fd = cfd;
	pthread_create(&th, NULL, bulk_rx, &bk);
	CHECK(tx_all(&c, afd, bk.total) == 0);
	pthread_join(th, NULL);
	CHECK(bk.ok && bk.sum == bk.total);
	rt_api_close(&c2, cfd);
	rt_api_close(&c, afd);

	/* IPv4 client of the same dual-stack listener */
	cfd = rt_api_socket(&c2, RT_AF_INET, RT_SOCK_STREAM, 0);
	set_timeout(&c2, cfd, 5000);
	a4 = sa4(0x7f000001, 6100);
	CHECK(rt_api_connect(&c2, cfd, &a4, sizeof(a4)) == 0);
	alen = sizeof(peer);
	afd = accept_wait(&c, lfd, &peer, &alen);
	CHECK(afd >= 0 && alen == 28 && addr_is(&peer, "::ffff:127.0.0.1"));
	alen = sizeof(me);
	CHECK(rt_api_getsockname(&c, afd, &me, &alen) == 0 && addr_is(&me, "::ffff:127.0.0.1"));
	CHECK(rt_api_send(&c2, cfd, "v4!", 3, 0) == 3);
	{
		char b3[8];
		set_timeout(&c, afd, 3000);
		CHECK(rt_api_recv(&c, afd, b3, sizeof(b3), 0) == 3 && !memcmp(b3, "v4!", 3));
		/* MSS over IPv4 from a v6 socket: 16384 - 40 - 12 */
		{
			u32 l = sizeof(mss);
			CHECK(rt_api_getsockopt(&c, afd, RT_IPPROTO_TCP, RT_TCP_MAXSEG, &mss, &l) == 0 &&
			      mss == 16384 - 40 - 12);
		}
	}
	rt_api_close(&c2, cfd);
	rt_api_close(&c, afd);

	/* an IPv6 client to a mapped address from an AF_INET6 socket */
	cfd = rt_api_socket(&c2, RT_AF_INET6, RT_SOCK_STREAM, 0);
	set_timeout(&c2, cfd, 5000);
	a = sa6("::ffff:127.0.0.1", 6100);
	CHECK(rt_api_connect(&c2, cfd, &a, sizeof(a)) == 0);
	afd = accept_wait(&c, lfd, NULL, NULL);
	CHECK(afd >= 0);
	alen = sizeof(me);
	CHECK(rt_api_getsockname(&c2, cfd, &me, &alen) == 0 && addr_is(&me, "::ffff:127.0.0.1"));
	rt_api_close(&c2, cfd);
	rt_api_close(&c, afd);
	rt_api_close(&c, lfd);

	/* V6ONLY listener refuses IPv4 */
	lfd = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	CHECK(rt_api_setsockopt(&c, lfd, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one)) == 0);
	a = sa6("::", 6101);
	CHECK(rt_api_bind(&c, lfd, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&c, lfd, 8) == 0);
	cfd = rt_api_socket(&c2, RT_AF_INET, RT_SOCK_STREAM, 0);
	a4 = sa4(0x7f000001, 6101);
	CHECK(rt_api_connect(&c2, cfd, &a4, sizeof(a4)) == -RT_ECONNREFUSED);
	rt_api_close(&c2, cfd);
	/* and its port is still free for IPv4 */
	cfd = rt_api_socket(&c2, RT_AF_INET, RT_SOCK_STREAM, 0);
	a4 = sa4(0, 6101);
	CHECK(rt_api_bind(&c2, cfd, &a4, sizeof(a4)) == 0);
	rt_api_close(&c2, cfd);
	cfd = rt_api_socket(&c2, RT_AF_INET6, RT_SOCK_STREAM, 0);
	a = sa6("::1", 6101);
	CHECK(rt_api_connect(&c2, cfd, &a, sizeof(a)) == 0);
	rt_api_close(&c2, cfd);
	rt_api_close(&c, lfd);

	/* nobody listening: RST -> ECONNREFUSED */
	cfd = rt_api_socket(&c2, RT_AF_INET6, RT_SOCK_STREAM, 0);
	a = sa6("::1", 6199);
	CHECK(rt_api_connect(&c2, cfd, &a, sizeof(a)) == -RT_ECONNREFUSED);
	rt_api_close(&c2, cfd);
}

static void test_netstat(void)
{
	struct rt_pcb_info p[64];
	int n, i, found = 0, lfd;
	struct rt_sockaddr_in6 a = sa6("::", 6300);

	lfd = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	CHECK(rt_api_bind(&c, lfd, &a, sizeof(a)) == 0 && rt_api_listen(&c, lfd, 1) == 0);
	rt_lock();
	n = rt_tcp_list(p, 64);
	rt_unlock();
	for (i = 0; i < n && i < 64; i++)
		if (p[i].v6 && p[i].lport == 6300 && p[i].state == RT_TCPS_LISTEN && rt_in6_is_unspec(&p[i].laddr6))
			found = 1;
	CHECK(found);
	rt_api_close(&c, lfd);
}

int main(void)
{
	CHECK(rt_host_start() == 0);
	CHECK(rt_ctx_init(&c, 64, NULL) == 0);
	CHECK(rt_ctx_init(&c2, 64, NULL) == 0);
	test_basics();
	test_udp();
	test_raw_ping();
	test_tcp();
	test_netstat();
	rt_ctx_destroy(&c);
	rt_ctx_destroy(&c2);
	rt_host_stop();
	printf("t_ip6_loop: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
