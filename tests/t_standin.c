/*
 * Riptide - IPv4 programs to IPv6-only hosts (RFC 6535 "Bump-in-the-Host", src/core/standin.c)
 * over lo0 with the host stack thread running: the IPv4-only lookup (rt_resolve_name4, what
 * gethostbyname() uses on the Amiga) gives a stand-in from 198.18.0.0/15 for a name with only an
 * IPv6 address, the IPv6-aware ones the real address; an AF_INET socket reaches an IPv6 listener
 * through the stand-in (TCP), UDP both ways, connected and not, with getpeername, getsockname,
 * recvfrom and accept's peer as the program expects; raw sockets refused; reverse lookups;
 * pool exhaustion and reuse; switched off. A small DNS server on 127.0.0.1:53 inside the stack
 * answers dns6.test with an AAAA record only.
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

static struct rt_sockctx c;

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

static struct rt_sockaddr_in sa4(u32 net_addr, u16 port)
{
	struct rt_sockaddr_in s;

	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = net_addr;
	s.sin_port = rt_htons(port);
	return s;
}

static void set_timeout(int fd, u32 ms)
{
	struct rt_timeval tv;

	tv.tv_secs = ms / 1000;
	tv.tv_micro = (ms % 1000) * 1000;
	rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv));
}

static int in_pool(u32 v4)
{
	return (rt_ntohl(v4) & 0xFFFE0000u) == 0xC6120000u;
}

static int held(void)
{
	int t, h;
	rt_lock();
	rt_standin_count(&t, &h);
	rt_unlock();
	return h;
}

static int quiet;

static int conf(const char *line)
{
	char err[160];
	int r;
	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0 && !quiet)
		fprintf(stderr, "conf '%s': %s\n", line, err);
	return r;
}

static u32 standin;             /* v6only.test's */

/* ---- a DNS server on 127.0.0.1:53: dns6.test has only an AAAA record (::1), dns4.test only
   an A one (127.0.0.1), everything else does not exist ---- */
static struct rt_sockctx dc;
static int dns_fd = -1, dns_a_queries, dns_aaaa_queries;

static void *dns_server(void *arg)
{
	u8 q[512], r[600];
	struct rt_sockaddr_in from;
	u32 fl;
	(void)arg;
	for (;;) {
		int n, qe, ql;
		u16 qtype;
		char name[128];
		u32 k = 0;
		fl = sizeof(from);
		n = rt_api_recvfrom(&dc, dns_fd, q, sizeof(q), 0, &from, &fl);
		if (n < 0)
			return NULL;
		if (n < 17)
			continue;
		/* the question's name, as text */
		for (qe = 12; qe < n && q[qe] && k + q[qe] + 2 < sizeof(name); qe += q[qe] + 1) {
			if (k)
				name[k++] = '.';
			memcpy(name + k, q + qe + 1, q[qe]);
			k += q[qe];
		}
		name[k] = 0;
		qe++;
		qtype = rt_get_be16(q + qe);
		ql = qe + 4;
		memcpy(r, q, (size_t)ql);
		r[2] = (u8)(0x84 | (q[2] & 1));         /* QR, AA, RD as asked */
		r[3] = 0x80;                            /* RA, NOERROR */
		r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;
		if (qtype == 1)
			dns_a_queries++;
		if (qtype == 28)
			dns_aaaa_queries++;
		if (!strcmp(name, "dns6.test") || !strcmp(name, "dns4.test")) {
			int six = name[3] == '6';
			if ((six && qtype == 28) || (!six && qtype == 1)) {
				r[7] = 1;
				r[ql++] = 0xc0;
				r[ql++] = 12;
				rt_put_be16(r + ql, qtype);
				rt_put_be16(r + ql + 2, 1);
				rt_put_be32(r + ql + 4, 60);
				rt_put_be16(r + ql + 8, six ? 16 : 4);
				ql += 10;
				memset(r + ql, 0, six ? 16 : 4);
				if (six) {
					r[ql + 15] = 1;         /* ::1 */
				} else {
					r[ql] = 127;
					r[ql + 3] = 1;
				}
				ql += six ? 16 : 4;
			}
		} else {
			r[3] = 0x83;                    /* NXDOMAIN */
		}
		rt_api_sendto(&dc, dns_fd, r, (u32)ql, 0, &from, fl);
	}
}

static void test_dns(void)
{
	rt_in6_addr a6[2];
	u32 a4[2], again;
	char name[64];
	int a0;

	a0 = dns_a_queries;
	CHECK(rt_resolve_name4("dns6.test", a4, 2, name, sizeof(name)) == 1 && in_pool(a4[0]));
	CHECK(dns_a_queries > a0 && dns_aaaa_queries > 0);          /* A first, then AAAA */
	CHECK(rt_resolve_name4("dns6.test", &again, 1, NULL, 0) == 1 && again == a4[0]);
	CHECK(rt_resolve_addr(a4[0], name, sizeof(name)) == 0 && !strcmp(name, "dns6.test"));
	CHECK(rt_resolve_name6("dns6.test", a6, 2, NULL, 0) == 1 && rt_in6_is_loopback(&a6[0]));
	CHECK(rt_resolve_name46("dns6.test", a6, 2, RT_AF_UNSPEC, NULL, 0) == 1 && rt_in6_is_loopback(&a6[0]));
	/* a name with an A record: no AAAA asked for, the real address */
	a0 = dns_aaaa_queries;
	CHECK(rt_resolve_name4("dns4.test", a4, 2, NULL, 0) == 1 && a4[0] == rt_htonl(0x7f000001u));
	CHECK(dns_aaaa_queries == a0);
	CHECK(rt_resolve_name4("nowhere.test", a4, 2, NULL, 0) == -RT_HOST_NOT_FOUND);
}

static void test_resolver(void)
{
	rt_in6_addr a6[4], ll;
	u32 a4[4], again;
	char name[128];
	int n;

	rt_lock();
	CHECK(rt_inet6_pton("::1", &a6[0]) == 0 && rt_hosts_add6("v6only.test", &a6[0]) == 0);
	CHECK(rt_inet6_pton("2001:db8::5", &a6[0]) == 0 && rt_hosts_add6("both.test", &a6[0]) == 0);
	CHECK(rt_hosts_add("both.test", rt_htonl(0x7f000001u)) == 0);
	CHECK(rt_inet6_pton("fe80::1", &ll) == 0 && rt_hosts_add6("ll.test", &ll) == 0);
	rt_unlock();

	/* IPv4-only lookup: a stand-in; the same one again */
	n = rt_resolve_name4("v6only.test", a4, 4, name, sizeof(name));
	CHECK(n == 1 && in_pool(a4[0]));
	standin = a4[0];
	CHECK(rt_resolve_name4("v6only.test", &again, 1, NULL, 0) == 1 && again == standin);
	/* the plain IPv4 lookup and the IPv6-aware ones are unchanged */
	CHECK(rt_resolve_name("v6only.test", a4, 4, NULL, 0) <= 0);
	CHECK(rt_resolve_name6("v6only.test", a6, 4, NULL, 0) == 1 && rt_in6_is_loopback(&a6[0]));
	n = rt_resolve_name46("v6only.test", a6, 4, RT_AF_UNSPEC, NULL, 0);
	CHECK(n == 1 && rt_in6_is_loopback(&a6[0]));
	/* a name with an IPv4 address: that one, no stand-in (RFC 6535 3.2) */
	CHECK(rt_resolve_name4("both.test", a4, 4, NULL, 0) == 1 && a4[0] == rt_htonl(0x7f000001u));
	/* literals stay as they are */
	CHECK(rt_resolve_name4("10.1.2.3", a4, 4, NULL, 0) == 1 && a4[0] == rt_htonl(0x0a010203u));
	/* link-local only: nothing (the scope would be lost) */
	CHECK(rt_resolve_name4("ll.test", a4, 4, NULL, 0) <= 0);
	CHECK(rt_resolve_name4("nothing.test", a4, 4, NULL, 0) <= 0);
	/* reverse: the name it was made for */
	CHECK(rt_resolve_addr(standin, name, sizeof(name)) == 0 && !strcmp(name, "v6only.test"));
	/* prefer: a stand-in for a name with both */
	CHECK(conf("standin prefer") == 0);
	CHECK(rt_resolve_name4("both.test", a4, 4, NULL, 0) == 1 && in_pool(a4[0]) && a4[0] != standin);
	CHECK(rt_resolve_name4("v6only.test", a4, 4, NULL, 0) == 1 && a4[0] == standin);
	CHECK(conf("standin on") == 0 && !rt_standin_prefer6);
	CHECK(rt_resolve_name4("both.test", a4, 4, NULL, 0) == 1 && a4[0] == rt_htonl(0x7f000001u));
	quiet = 1;
	CHECK(conf("standin 300.1.2.3") < 0 && conf("standin maybe") < 0 && conf("standin 10.0.0.0/30") < 0);
	quiet = 0;
}

static void test_tcp(void)
{
	struct rt_sockaddr_in6 a6;
	struct rt_sockaddr_in a4;
	u32 len;
	char buf[64];
	int l, s, ch, r, one = 1, tos = 0x20;

	l = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	CHECK(l >= 0);
	rt_api_setsockopt(&c, l, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one));
	a6 = sa6("::1", 7101);
	CHECK(rt_api_bind(&c, l, &a6, sizeof(a6)) == 0 && rt_api_listen(&c, l, 4) == 0);
	set_timeout(l, 3000);

	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(s >= 0);
	set_timeout(s, 3000);
	/* IP-level options on such a socket do no harm */
	CHECK(rt_api_setsockopt(&c, s, RT_IPPROTO_IP, RT_IP_TOS, &tos, sizeof(tos)) == 0);
	a4 = sa4(standin, 7101);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) == 0);
	CHECK(held() == 1);
	len = sizeof(a6);
	ch = rt_api_accept(&c, l, &a6, &len);
	CHECK(ch >= 0 && a6.sin6_family == RT_AF_INET6 && rt_in6_is_loopback(&a6.sin6_addr));
	set_timeout(ch, 3000);
	/* the program sees the stand-in as its peer and 127.0.0.1 (its IPv4 address) as itself */
	len = sizeof(a4);
	memset(&a4, 0, sizeof(a4));
	CHECK(rt_api_getpeername(&c, s, &a4, &len) == 0 && len == sizeof(a4) && a4.sin_family == RT_AF_INET &&
	      a4.sin_addr == standin && a4.sin_port == rt_htons(7101));
	len = sizeof(a4);
	memset(&a4, 0, sizeof(a4));
	CHECK(rt_api_getsockname(&c, s, &a4, &len) == 0 && a4.sin_family == RT_AF_INET &&
	      a4.sin_addr == rt_htonl(0x7f000001u) && a4.sin_port != 0);
	CHECK(rt_api_send(&c, s, "GET /", 5, 0) == 5);
	r = rt_api_recv(&c, ch, buf, sizeof(buf), 0);
	CHECK(r == 5 && !memcmp(buf, "GET /", 5));
	CHECK(rt_api_send(&c, ch, "It works", 8, 0) == 8);
	r = rt_api_recv(&c, s, buf, sizeof(buf), 0);
	CHECK(r == 8 && !memcmp(buf, "It works", 8));
	/* a second connection to the same stand-in */
	{
		int s2 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0), ch2;
		set_timeout(s2, 3000);
		a4 = sa4(standin, 7101);
		CHECK(rt_api_connect(&c, s2, &a4, sizeof(a4)) == 0);
		ch2 = rt_api_accept(&c, l, NULL, NULL);
		CHECK(ch2 >= 0);
		rt_api_close(&c, ch2);
		rt_api_close(&c, s2);
	}
	rt_api_close(&c, ch);
	rt_api_close(&c, s);
	CHECK(held() == 0);
	/* nothing listening: refused, as for IPv4 */
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	set_timeout(s, 3000);
	a4 = sa4(standin, 7102);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) == -RT_ECONNREFUSED);
	rt_api_close(&c, s);
	/* an AF_INET socket bound to an IPv4 address keeps it: it cannot go over IPv6 */
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	a4 = sa4(rt_htonl(0x7f000001u), 0);
	CHECK(rt_api_bind(&c, s, &a4, sizeof(a4)) == 0);
	a4 = sa4(standin, 7101);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) < 0);
	rt_api_close(&c, s);
	CHECK(held() == 0);
	rt_api_close(&c, l);
}

static void test_udp(void)
{
	struct rt_sockaddr_in6 a6, from6;
	struct rt_sockaddr_in a4, from;
	u32 fl;
	char buf[64];
	int srv, srv4, s, r, one = 1;

	srv = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	rt_api_setsockopt(&c, srv, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one));
	a6 = sa6("::1", 7201);
	CHECK(rt_api_bind(&c, srv, &a6, sizeof(a6)) == 0);
	set_timeout(srv, 2000);
	srv4 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	a4 = sa4(rt_htonl(0x7f000001u), 7202);
	CHECK(rt_api_bind(&c, srv4, &a4, sizeof(a4)) == 0);
	set_timeout(srv4, 2000);

	/* unconnected: sendto the stand-in, the answer comes from the stand-in */
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	set_timeout(s, 2000);
	a4 = sa4(standin, 7201);
	CHECK(rt_api_sendto(&c, s, "ping", 4, 0, &a4, sizeof(a4)) == 4);
	CHECK(held() == 1);
	fl = sizeof(from6);
	r = rt_api_recvfrom(&c, srv, buf, sizeof(buf), 0, &from6, &fl);
	CHECK(r == 4 && !memcmp(buf, "ping", 4) && rt_in6_is_loopback(&from6.sin6_addr));
	CHECK(rt_api_sendto(&c, srv, "pong", 4, 0, &from6, fl) == 4);
	fl = sizeof(from);
	memset(&from, 0, sizeof(from));
	r = rt_api_recvfrom(&c, s, buf, sizeof(buf), 0, &from, &fl);
	CHECK(r == 4 && !memcmp(buf, "pong", 4) && fl == sizeof(from) && from.sin_family == RT_AF_INET &&
	      from.sin_addr == standin && from.sin_port == rt_htons(7201));
	/* the same socket still talks IPv4 */
	a4 = sa4(rt_htonl(0x7f000001u), 7202);
	CHECK(rt_api_sendto(&c, s, "v4", 2, 0, &a4, sizeof(a4)) == 2);
	fl = sizeof(from);
	r = rt_api_recvfrom(&c, srv4, buf, sizeof(buf), 0, &from, &fl);
	CHECK(r == 2 && from.sin_addr == rt_htonl(0x7f000001u));
	CHECK(rt_api_sendto(&c, srv4, "v4back", 6, 0, &from, fl) == 6);
	fl = sizeof(from);
	r = rt_api_recvfrom(&c, s, buf, sizeof(buf), 0, &from, &fl);
	CHECK(r == 6 && from.sin_addr == rt_htonl(0x7f000001u) && from.sin_port == rt_htons(7202));
	/* recvmsg reports the stand-in too */
	a4 = sa4(standin, 7201);
	CHECK(rt_api_sendto(&c, s, "m", 1, 0, &a4, sizeof(a4)) == 1);
	fl = sizeof(from6);
	CHECK(rt_api_recvfrom(&c, srv, buf, sizeof(buf), 0, &from6, &fl) == 1);
	CHECK(rt_api_sendto(&c, srv, "msg", 3, 0, &from6, fl) == 3);
	{
		struct rt_iov v = { buf, sizeof(buf) };
		int mf = 0;
		fl = sizeof(from);
		memset(&from, 0, sizeof(from));
		r = rt_api_recvmsg(&c, s, &v, 1, 0, &from, &fl, &mf);
		CHECK(r == 3 && from.sin_addr == standin);
	}
	rt_api_close(&c, s);
	CHECK(held() == 0);

	/* connected: send/recv, getpeername */
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	set_timeout(s, 2000);
	a4 = sa4(standin, 7201);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) == 0);
	CHECK(rt_api_send(&c, s, "conn", 4, 0) == 4);
	fl = sizeof(from6);
	r = rt_api_recvfrom(&c, srv, buf, sizeof(buf), 0, &from6, &fl);
	CHECK(r == 4 && rt_in6_is_loopback(&from6.sin6_addr));
	CHECK(rt_api_sendto(&c, srv, "back", 4, 0, &from6, fl) == 4);
	CHECK(rt_api_recv(&c, s, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "back", 4));
	fl = sizeof(a4);
	memset(&a4, 0, sizeof(a4));
	CHECK(rt_api_getpeername(&c, s, &a4, &fl) == 0 && a4.sin_addr == standin && a4.sin_port == rt_htons(7201));
	fl = sizeof(a4);
	CHECK(rt_api_getsockname(&c, s, &a4, &fl) == 0 && a4.sin_addr == rt_htonl(0x7f000001u));
	rt_api_close(&c, s);
	rt_api_close(&c, srv);
	rt_api_close(&c, srv4);
	CHECK(held() == 0);
}

static void test_raw(void)
{
	struct rt_sockaddr_in a4;
	u8 echo[8] = { 8, 0, 0xf7, 0xff, 0, 0, 0, 0 };
	int s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);

	CHECK(s >= 0);
	a4 = sa4(standin, 0);
	/* ICMP is not translated: ping to a stand-in says the network is unreachable */
	CHECK(rt_api_sendto(&c, s, echo, sizeof(echo), 0, &a4, sizeof(a4)) == -RT_ENETUNREACH);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) == -RT_ENETUNREACH);
	rt_api_close(&c, s);
	CHECK(held() == 0);
}

static void test_pool(void)
{
	rt_in6_addr a;
	u32 v[RT_STANDIN_SLOTS], extra, first_again;
	char name[64];
	int i, distinct = 1, l, s, ch;
	struct rt_sockaddr_in6 a6;
	struct rt_sockaddr_in a4;

	/* another range: the table starts empty */
	CHECK(conf("standin 10.64.0.0/16") == 0);
	CHECK(rt_resolve_name4("v6only.test", &standin, 1, NULL, 0) == 1 &&
	      (rt_ntohl(standin) & 0xFFFF0000u) == 0x0A400000u);
	/* a connection holds its stand-in while the table fills up */
	l = rt_api_socket(&c, RT_AF_INET6, RT_SOCK_STREAM, 0);
	a6 = sa6("::1", 7301);
	CHECK(rt_api_bind(&c, l, &a6, sizeof(a6)) == 0 && rt_api_listen(&c, l, 2) == 0);
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	set_timeout(s, 3000);
	a4 = sa4(standin, 7301);
	CHECK(rt_api_connect(&c, s, &a4, sizeof(a4)) == 0);
	ch = rt_api_accept(&c, l, NULL, NULL);
	CHECK(ch >= 0);
	rt_lock();
	rt_inet6_pton("2001:db8:1::", &a);
	for (i = 0; i < RT_STANDIN_SLOTS - 1; i++) {
		a.b[15] = (u8)(i + 1);
		v[i] = rt_standin_for(&a, NULL);
		if (!v[i] || v[i] == standin || (i && v[i] == v[i - 1]))
			distinct = 0;
	}
	CHECK(distinct);
	/* full: the idle one used longest ago goes, never the held one */
	for (i = 1; i < RT_STANDIN_SLOTS - 1; i++)
		rt_standin_hold(v[i], 1);
	a.b[15] = 200;
	extra = rt_standin_for(&a, "new.test");
	CHECK(extra == v[0]);
	CHECK(rt_standin_lookup(standin, &a, name, sizeof(name)) && rt_in6_is_loopback(&a) &&
	      !strcmp(name, "v6only.test"));
	/* every slot held: none */
	rt_standin_hold(extra, 1);
	a.b[15] = 201;
	CHECK(rt_standin_for(&a, NULL) == 0);
	for (i = 0; i < RT_STANDIN_SLOTS - 1; i++)
		rt_standin_hold(v[i], 0);
	a.b[15] = 201;
	first_again = rt_standin_for(&a, NULL);
	CHECK(first_again != 0 && first_again != standin);
	rt_unlock();
	/* the connection still works */
	CHECK(rt_api_send(&c, s, "still", 5, 0) == 5);
	{
		char buf[8];
		set_timeout(ch, 3000);
		CHECK(rt_api_recv(&c, ch, buf, sizeof(buf), 0) == 5);
	}
	rt_api_close(&c, ch);
	rt_api_close(&c, s);
	rt_api_close(&c, l);
	CHECK(held() == 0);
	CHECK(conf("standin 198.18.0.0/15") == 0);
}

static void test_off(void)
{
	struct rt_sockaddr_in a4;
	char out[160];
	rt_in6_addr v6;
	u32 a;
	int s;

	CHECK(rt_resolve_name4("v6only.test", &standin, 1, NULL, 0) == 1 && in_pool(standin));
	CHECK(rt_sysctl_set_text("net.inet.standin.enable", "0") == 0);
	CHECK(rt_resolve_name4("v6only.test", &a, 1, NULL, 0) <= 0);
	CHECK(rt_resolve_addr(standin, out, sizeof(out)) < 0 || strcmp(out, "v6only.test"));
	/* an AF_INET socket to the old stand-in goes as IPv4 (nowhere, here) */
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	a4 = sa4(standin, 7201);
	CHECK(rt_api_sendto(&c, s, "x", 1, 0, &a4, sizeof(a4)) < 0);
	rt_api_close(&c, s);
	CHECK(held() == 0);
	CHECK(conf("standin on") == 0 && rt_standin_enable);
	rt_lock();
	CHECK(rt_config_line("standin status", out, sizeof(out), NULL) == 0 && strstr(out, "198.18.0.0/15"));
	rt_unlock();
	CHECK(conf("standin off") == 0 && !rt_standin_enable);
	CHECK(rt_resolve_name4("v6only.test", &a, 1, NULL, 0) <= 0);
	CHECK(conf("standin on") == 0);
	/* a restart of the configuration: on again, the default range */
	CHECK(conf("standin 10.64.0.0/16") == 0 && conf("standin off") == 0);
	rt_lock();
	rt_config_reset();
	CHECK(rt_inet6_pton("::1", &v6) == 0 && rt_hosts_add6("v6only.test", &v6) == 0);   /* hosts went too */
	rt_unlock();
	CHECK(rt_standin_enable && rt_resolve_name4("v6only.test", &a, 1, NULL, 0) == 1 && in_pool(a));
}

int main(void)
{
	CHECK(rt_host_start() == 0);
	CHECK(rt_ctx_init(&c, 64, NULL) == 0);
	CHECK(rt_ctx_init(&dc, 8, NULL) == 0);
	{
		struct rt_sockaddr_in a = sa4(rt_htonl(0x7f000001u), 53);
		pthread_t th;
		dns_fd = rt_api_socket(&dc, RT_AF_INET, RT_SOCK_DGRAM, 0);
		CHECK(dns_fd >= 0 && rt_api_bind(&dc, dns_fd, &a, sizeof(a)) == 0);
		pthread_create(&th, NULL, dns_server, NULL);
		pthread_detach(th);
		rt_lock();
		CHECK(rt_dns_add_server(rt_htonl(0x7f000001u)) == 0);
		rt_unlock();
	}
	test_resolver();
	test_dns();
	test_tcp();
	test_udp();
	test_raw();
	test_pool();
	test_off();
	rt_ctx_destroy(&c);
	rt_host_stop();
	printf("t_standin: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
