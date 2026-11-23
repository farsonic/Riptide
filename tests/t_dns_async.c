/*
 * Riptide - the asynchronous lookups (rt_dns_lookup_*) and the asynchronous
 * Happy Eyeballs (rt_he_conn_*) against a small DNS server of the test's own
 * on the core's loopback (127.0.0.1:53, UDP and TCP), polled the way the
 * Mac's deferred task polls them: never waiting, a poll every few ms.
 * No root needed. The server answers:
 *   both.test      A 192.0.2.1, AAAA 2001:db8::1     cname.test   CNAME both.test
 *   v4.test        A, and NODATA (with an SOA) for AAAA
 *   nx.test        NXDOMAIN with an SOA (negative TTL 30)
 *   big.test, big2.test   100 A records: truncated over UDP (TC, no answers), whole over TCP
 *   slowa.test, slowb.test   A at once, AAAA 300 ms later
 *   silent.test    nothing           formerr.test  FORMERR to a query with EDNS
 *   short.test     A (for the search list)        lo.test  A 127.0.0.1, AAAA ::1
 *   1.2.0.192.in-addr.arpa  PTR both.test
 * and NXDOMAIN with an SOA for everything else. Answers echo an OPT record
 * when the query had one, as EDNS servers do.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the server -------------------------------------------------------------------------------- */
static struct rt_sockctx sctx;
static int sfd, lfd;
static volatile int stop;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int nq_udp, nq_tcp, last_opt, last_opt_size;
static char last_name[256];
static struct { char name[64]; int type, n; } counts[64];
static int ncounts;
struct later { u32 due; struct rt_sockaddr_in to; int len; u8 m[600]; };
static struct later laters[8];

static u32 ms_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (u32)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

static void count(const char *name, int type)
{
	int i;
	pthread_mutex_lock(&mu);
	for (i = 0; i < ncounts; i++)
		if (!strcmp(counts[i].name, name) && counts[i].type == type)
			break;
	if (i == ncounts && ncounts < 64) {
		snprintf(counts[i].name, sizeof counts[i].name, "%.63s", name);
		counts[i].type = type;
		counts[i].n = 0;
		ncounts++;
	}
	if (i < 64)
		counts[i].n++;
	pthread_mutex_unlock(&mu);
}

static int asked(const char *name, int type)
{
	int i, n = 0;
	pthread_mutex_lock(&mu);
	for (i = 0; i < ncounts; i++)
		if (!strcmp(counts[i].name, name) && counts[i].type == type)
			n = counts[i].n;
	pthread_mutex_unlock(&mu);
	return n;
}

static int qname(const u8 *q, u32 len, char *out, u32 *end)
{
	u32 p = 12, o = 0;
	while (p < len && q[p]) {
		u32 l = q[p];
		if (p + 1 + l > len || o + l + 2 > 255)
			return -1;
		if (o)
			out[o++] = '.';
		memcpy(out + o, q + p + 1, l);
		o += l;
		p += 1 + l;
	}
	out[o] = 0;
	*end = p + 1;
	return 0;
}

static u8 *rr(u8 *p, u16 owner, u16 type, u32 ttl, const void *rd, u16 rdlen)
{
	rt_put_be16(p, (u16)(0xc000 | owner));
	rt_put_be16(p + 2, type);
	rt_put_be16(p + 4, 1);
	rt_put_be32(p + 6, ttl);
	rt_put_be16(p + 10, rdlen);
	memcpy(p + 12, rd, rdlen);
	return p + 12 + rdlen;
}

static u8 *soa(u8 *p, u32 min)
{
	u8 rd[28];
	memcpy(rd, "\002ns\000\002hm\000", 8);
	memset(rd + 8, 0, 20);
	rt_put_be32(rd + 24, min);
	return rr(p, 12, 6, 300, rd, sizeof rd);
}

/* the answer to the query q (len bytes) into a (room cap); its length, 0 = no answer, -1 = later */
static int answer(const u8 *q, u32 len, u8 *a, u32 cap, int tcp, int *delay)
{
	char name[256];
	u32 end, i;
	u16 type, an = 0, ns = 0, ar = 0, rcode = 0, udpmax = 512;
	u8 *p, rd[16];
	int opt = 0;

	*delay = 0;
	if (len < 17 || qname(q, len, name, &end) < 0 || end + 4 > len)
		return 0;
	type = rt_get_be16(q + end);
	/* an OPT record after the question: EDNS, and its size */
	if (rt_get_be16(q + 10) && end + 4 + 11 <= len && q[end + 4] == 0 && rt_get_be16(q + end + 5) == 41) {
		opt = 1;
		udpmax = rt_get_be16(q + end + 7);
	}
	pthread_mutex_lock(&mu);
	if (tcp) nq_tcp++; else nq_udp++;
	last_opt = opt;
	last_opt_size = opt ? udpmax : 0;
	snprintf(last_name, sizeof last_name, "%s", name);
	pthread_mutex_unlock(&mu);
	count(name, type);
	if (!strcmp(name, "silent.test"))
		return 0;
	memcpy(a, q, end + 4);
	p = a + end + 4;
	if (!strcmp(name, "formerr.test") && opt) {
		rcode = 1;
		opt = 0;
	} else if (!strcmp(name, "both.test") || !strcmp(name, "cname.test") || !strcmp(name, "slowa.test") ||
	           !strcmp(name, "slowb.test") || !strcmp(name, "formerr.test") || !strcmp(name, "short.test") ||
	           !strcmp(name, "lo.test")) {
		u16 owner = 12;
		if (!strcmp(name, "cname.test")) {
			static const u8 target[] = "\004both\004test";
			p = rr(p, 12, 5, 60, target, sizeof target);
			owner = (u16)(end + 4 + 12);       /* the target name, in the CNAME's data */
			an++;
		}
		if (type == 1) {
			rt_put_be32(rd, !strcmp(name, "lo.test") ? 0x7f000001u : 0xc0000201u);
			p = rr(p, owner, 1, 60, rd, 4);
			an++;
		} else if (type == 28 && strcmp(name, "short.test") && strcmp(name, "formerr.test")) {
			memset(rd, 0, 16);
			if (strcmp(name, "lo.test")) {
				rd[0] = 0x20; rd[1] = 0x01; rd[2] = 0x0d; rd[3] = 0xb8;
			}
			rd[15] = 1;
			p = rr(p, owner, 28, 60, rd, 16);
			an++;
			if (!strncmp(name, "slow", 4))
				*delay = 300;
		} else {
			p = soa(p, 30);
			ns++;
		}
	} else if (!strcmp(name, "v4.test")) {
		if (type == 1) {
			rt_put_be32(rd, 0xc0000202u);
			p = rr(p, 12, 1, 60, rd, 4);
			an++;
		} else {
			p = soa(p, 30);
			ns++;
		}
	} else if (!strcmp(name, "big.test") || !strcmp(name, "big2.test")) {
		if (type == 1) {
			for (i = 0; i < 100; i++) {
				rt_put_be32(rd, 0xc6336400u + i);
				p = rr(p, 12, 1, 60, rd, 4);
				an++;
			}
		} else {
			p = soa(p, 30);
			ns++;
		}
	} else if (!strcmp(name, "1.2.0.192.in-addr.arpa") && type == 12) {
		static const u8 target[] = "\004both\004test";
		p = rr(p, 12, 12, 60, target, sizeof target);
		an++;
	} else {
		rcode = 3;
		p = soa(p, 30);
		ns++;
	}
	if (opt) {
		p[0] = 0;
		rt_put_be16(p + 1, 41);
		rt_put_be16(p + 3, 1232);
		memset(p + 5, 0, 6);
		p += 11;
		ar = 1;
	}
	rt_put_be16(a + 2, (u16)(0x8180 | rcode));
	rt_put_be16(a + 4, 1);
	rt_put_be16(a + 6, an);
	rt_put_be16(a + 8, ns);
	rt_put_be16(a + 10, ar);
	if (!tcp && (u32)(p - a) > udpmax) {
		/* truncated: the question (and OPT) only, TC set */
		p = a + end + 4;
		rt_put_be16(a + 2, (u16)(0x8380 | rcode));
		rt_put_be16(a + 6, 0);
		rt_put_be16(a + 8, 0);
		if (opt) {
			p[0] = 0;
			rt_put_be16(p + 1, 41);
			rt_put_be16(p + 3, 1232);
			memset(p + 5, 0, 6);
			p += 11;
		}
	}
	(void)cap;
	return (int)(p - a);
}

static void *server(void *arg)
{
	static u8 q[1500], a[2048];
	(void)arg;
	while (!stop) {
		struct rt_sockaddr_in from;
		u32 fl = sizeof from, set[2] = { 0, 0 }, i;
		int n, delay, m, cfd;
		set[0] = (1u << sfd) | (1u << lfd);
		if (rt_api_select(&sctx, (sfd > lfd ? sfd : lfd) + 1, set, NULL, NULL, 5, NULL) > 0) {
			if (set[0] & (1u << sfd)) {
				n = rt_api_recvfrom(&sctx, sfd, q, sizeof q, RT_MSG_DONTWAIT, &from, &fl);
				if (n > 0 && (m = answer(q, (u32)n, a, sizeof a, 0, &delay)) > 0) {
					if (delay) {
						for (i = 0; i < 8 && laters[i].len; i++)
							;
						if (i < 8) {
							laters[i].due = ms_now() + (u32)delay;
							laters[i].to = from;
							memcpy(laters[i].m, a, (u32)m);
							laters[i].len = m;
						}
					} else {
						rt_api_sendto(&sctx, sfd, a, (u32)m, 0, &from, sizeof from);
					}
				}
			}
			if ((set[0] & (1u << lfd)) && (cfd = rt_api_accept(&sctx, lfd, NULL, NULL)) >= 0) {
				/* one query per connection: the length, the query, the whole answer */
				u8 hdr[2];
				struct rt_timeval tv = { 2, 0 };
				rt_api_setsockopt(&sctx, cfd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof tv);
				if (rt_api_recv(&sctx, cfd, hdr, 2, RT_MSG_WAITALL) == 2) {
					u32 ql = rt_get_be16(hdr);
					if (ql <= sizeof q && rt_api_recv(&sctx, cfd, q, ql, RT_MSG_WAITALL) == (int)ql &&
					    (m = answer(q, ql, a + 2, sizeof a - 2, 1, &delay)) > 0) {
						rt_put_be16(a, (u16)m);
						rt_api_send(&sctx, cfd, a, (u32)m + 2, 0);
					}
				}
				rt_api_close(&sctx, cfd);
			}
		}
		for (i = 0; i < 8; i++)
			if (laters[i].len && (s32)(ms_now() - laters[i].due) >= 0) {
				rt_api_sendto(&sctx, sfd, laters[i].m, (u32)laters[i].len, 0, &laters[i].to,
				              sizeof laters[i].to);
				laters[i].len = 0;
			}
	}
	return NULL;
}

/* ---- the client, polled as the Mac's deferred task polls it ------------------------------------ */
static struct rt_sockctx cctx;

static int run(struct rt_dns_lookup *l, u32 *ms)
{
	u32 t0 = ms_now();
	int r, guard = 0;
	while ((r = rt_dns_lookup_poll(l)) == 0 && guard++ < 10000)
		usleep(2000);
	if (ms)
		*ms = ms_now() - t0;
	return r;
}

static int lookup(const char *name, int family, int flags, u32 timeout, u32 *a4, int *n4, rt_in6_addr *a6,
                  int *n6, char *canon, u32 *ms)
{
	int err = 0, r;
	struct rt_dns_lookup *l = rt_dns_lookup_start(&cctx, name, family, flags, timeout, &err);
	if (!l)
		return err ? err - 1000 : -9999;
	r = run(l, ms);
	rt_dns_lookup_get(l, a4, n4, a6, n6, canon, 256);
	rt_dns_lookup_free(l);
	return r;
}

static void cmd(const char *a, const char *b, const char *c)
{
	char *argv[3] = { (char *)a, (char *)b, (char *)c }, out[256];
	rt_lock();
	rt_dns_command(c ? 3 : 2, argv, out, sizeof out);
	rt_unlock();
}

int main(void)
{
	struct rt_sockaddr_in sa;
	pthread_t th;
	u32 a4[16], ms;
	rt_in6_addr a6[16];
	char canon[256];
	int n4, n6, r, one = 1, q0;

	if (rt_host_start() != 0) {
		printf("t_dns_async: SKIP (no host stack)\n");
		return 77;
	}
	CHECK(rt_ctx_init(&sctx, 32, NULL) == 0);
	CHECK(rt_ctx_init(&cctx, 64, NULL) == 0);
	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons(53);
	sa.sin_addr = rt_htonl(0x7f000001);
	sfd = rt_api_socket(&sctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	lfd = rt_api_socket(&sctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	rt_api_setsockopt(&sctx, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof one);
	CHECK(sfd >= 0 && sfd < 32 && lfd >= 0 && lfd < 32);
	CHECK(rt_api_bind(&sctx, sfd, &sa, sizeof sa) == 0);
	CHECK(rt_api_bind(&sctx, lfd, &sa, sizeof sa) == 0);
	CHECK(rt_api_listen(&sctx, lfd, 4) == 0);
	pthread_create(&th, NULL, server, NULL);

	/* no name server: -ENXIO at once */
	r = lookup("both.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_ENXIO - 1000);
	rt_lock();
	rt_dns_add_server(rt_htonl(0x7f000001));
	rt_unlock();
	/* not a name DNS can carry */
	r = lookup("a.0123456789012345678901234567890123456789012345678901234567890123.test", RT_AF_UNSPEC, 0, 0,
	           a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_EINVAL - 1000);
	CHECK(lookup("a..test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_EINVAL - 1000);

	/* A and AAAA at once, EDNS with dns.edns_size, the answers into the cache */
	n4 = n6 = 16;
	r = lookup("both.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 2 && n4 == 1 && n6 == 1 && a4[0] == rt_htonl(0xc0000201u) && a6[0].b[0] == 0x20 && a6[0].b[15] == 1);
	CHECK(!strcmp(canon, "both.test"));
	CHECK(asked("both.test", 1) == 1 && asked("both.test", 28) == 1);
	CHECK(last_opt && last_opt_size == 1232);
	{
		u32 c4[4];
		rt_in6_addr c6[4];
		CHECK(rt_resolve_local46("both.test", 0, c4, NULL, 4) == 1 && c4[0] == rt_htonl(0xc0000201u));
		CHECK(rt_resolve_local46("both.test", 1, NULL, c6, 4) == 1);
	}
	n4 = n6 = 16;
	r = lookup("both.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 2 && asked("both.test", 1) == 1 && asked("both.test", 28) == 1);   /* from the cache */

	/* one family only; a CNAME; IPv4 only names (NODATA for AAAA) */
	n4 = n6 = 16;
	CHECK(lookup("cname.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms) == 1 && n4 == 1 && n6 == 0);
	CHECK(!strcmp(canon, "both.test") && asked("cname.test", 28) == 0);
	n4 = n6 = 16;
	CHECK(lookup("v4.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms) == 1 && n4 == 1 && n6 == 0);
	n4 = n6 = 16;
	CHECK(lookup("v4.test", RT_AF_INET6, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_NO_DATA);
	CHECK(asked("v4.test", 28) == 1);                    /* NODATA came from the negative cache */

	/* NXDOMAIN, then from the negative cache; with dns.cache.neg_maxttl 0 asked again */
	r = lookup("nx.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_HOST_NOT_FOUND);
	q0 = asked("nx.test", 1) + asked("nx.test", 28);
	CHECK(q0 == 2);
	CHECK(lookup("nx.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND);
	CHECK(asked("nx.test", 1) + asked("nx.test", 28) == 2);
	CHECK(lookup("nx.test", RT_AF_INET6, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND);
	CHECK(asked("nx.test", 28) == 1);
	cmd("dns", "flush", NULL);
	CHECK(rt_sysctl_set_text("dns.cache.neg_maxttl", "0") == 0);
	CHECK(lookup("nx.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND);
	CHECK(lookup("nx.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND);
	CHECK(asked("nx.test", 1) + asked("nx.test", 28) == 6);
	CHECK(rt_sysctl_set_text("dns.cache.neg_maxttl", "3600") == 0);

	/* a large answer: truncated over UDP, whole over TCP; with dns.tcp_fallback 0 no answer */
	cmd("dns", "tcp", "off");
	n4 = n6 = 16;
	r = lookup("big2.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_TRY_AGAIN && nq_tcp == 0);
	cmd("dns", "tcp", "on");
	n4 = n6 = 16;
	r = lookup("big.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 8 && n4 == 8 && a4[0] == rt_htonl(0xc6336400u) && a4[7] == rt_htonl(0xc6336407u));
	CHECK(nq_tcp == 1);
	{
		u32 c4[8];
		CHECK(rt_resolve_local46("big.test", 0, c4, NULL, 8) == 8);
	}

	/* EDNS off: no OPT record */
	CHECK(rt_sysctl_set_text("dns.edns_size", "0") == 0);
	n4 = n6 = 16;
	CHECK(lookup("v4.test", RT_AF_INET, RT_DNSL_NOSEARCH, 0, a4, &n4, a6, &n6, canon, &ms) == 1);
	CHECK(rt_sysctl_set_text("dns.edns_size", "1232") == 0);
	/* (from the cache: ask a new name) */
	n4 = n6 = 16;
	cmd("dns", "flush", NULL);
	CHECK(rt_sysctl_set_text("dns.edns_size", "0") == 0);
	CHECK(lookup("v4.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms) == 1);
	CHECK(!last_opt);
	CHECK(rt_sysctl_set_text("dns.edns_size", "1232") == 0);
	/* FORMERR to EDNS: asked again without, and the server remembered as one without */
	n4 = n6 = 16;
	CHECK(lookup("formerr.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms) == 1);
	CHECK(asked("formerr.test", 1) == 2 && !last_opt);
	n4 = n6 = 16;
	CHECK(lookup("short.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms) == 1 && !last_opt);
	cmd("dns", "reset", NULL);

	/* the resolution delay: the A answer waits rt_he_resolution_delay (50 ms) for a late AAAA; with
	   RT_DNSL_BOTH both are waited for */
	n4 = n6 = 16;
	r = lookup("slowa.test", RT_AF_UNSPEC, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 1 && n4 == 1 && n6 == 0 && ms < 250);
	printf("slowa.test: A only after %u ms\n", (unsigned)ms);
	n4 = n6 = 16;
	r = lookup("slowb.test", RT_AF_UNSPEC, RT_DNSL_BOTH, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 2 && n4 == 1 && n6 == 1 && ms >= 290);
	printf("slowb.test: A and AAAA after %u ms\n", (unsigned)ms);

	/* the search list: a name without a dot gets the domains first; RT_DNSL_NOSEARCH as it is */
	rt_lock();
	rt_dns_set_search("example test");
	rt_unlock();
	n4 = n6 = 16;
	r = lookup("short", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == 1 && !strcmp(canon, "short.test") && asked("short.example", 1) == 1);
	cmd("dns", "flush", NULL);                              /* "short" is in the cache now */
	CHECK(lookup("short", RT_AF_INET, RT_DNSL_NOSEARCH, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND);
	CHECK(asked("short", 1) == 1);
	/* MacTCP's rule: the name first, then the domains; a name with a dot as it is */
	CHECK(lookup("short", RT_AF_INET, RT_DNSL_SHORTSEARCH, 0, a4, &n4, a6, &n6, canon, &ms) == 1);
	CHECK(asked("short", 1) == 1 && asked("short.example", 1) == 2 && !strcmp(canon, "short.test"));
	CHECK(lookup("nx.test", RT_AF_INET, RT_DNSL_SHORTSEARCH, 0, a4, &n4, a6, &n6, canon, &ms) ==
	      -RT_HOST_NOT_FOUND && asked("nx.test.test", 1) == 0 && asked("nx.test.example", 1) == 0);
	CHECK(lookup("nx.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms) == -RT_HOST_NOT_FOUND &&
	      asked("nx.test.example", 1) == 1);
	rt_lock();
	rt_dns_set_search("");
	rt_unlock();

	/* a server that never answers: two rounds of 2 s; the caller's timeout cuts it short */
	r = lookup("silent.test", RT_AF_INET, 0, 600, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_TRY_AGAIN && ms >= 550 && ms < 1500);
	printf("silent.test with a 600 ms timeout: %d after %u ms\n", r, (unsigned)ms);
	cmd("dns", "flush", NULL);
	r = lookup("silent.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	CHECK(r == -RT_TRY_AGAIN && ms >= 3900 && ms < 6000 && asked("silent.test", 1) == 3);
	printf("silent.test: %d after %u ms, %d queries\n", r, (unsigned)ms, asked("silent.test", 1));
	/* ... and the failure is remembered (RFC 9520): not asked again at once */
	r = lookup("silent.test", RT_AF_INET, 0, 0, a4, &n4, a6, &n6, canon, &ms);
	printf("silent.test again: %d after %u ms, %d queries\n", r, (unsigned)ms, asked("silent.test", 1));
	CHECK(r == -RT_TRY_AGAIN && asked("silent.test", 1) == 3 && ms < 100);

	/* the hosts table: no query at all */
	rt_lock();
	rt_hosts_add("hosted.test", rt_htonl(0xc0000209u));
	rt_unlock();
	{
		int err;
		struct rt_dns_lookup *l = rt_dns_lookup_start(&cctx, "hosted.test", RT_AF_INET, 0, 0, &err);
		CHECK(l && rt_dns_lookup_poll(l) == 1);
		n4 = 4;
		rt_dns_lookup_get(l, a4, &n4, NULL, NULL, NULL, 0);
		CHECK(n4 == 1 && a4[0] == rt_htonl(0xc0000209u) && asked("hosted.test", 1) == 0);
		rt_dns_lookup_free(l);
	}

	/* reverse lookups */
	{
		rt_in6_addr a;
		int err;
		struct rt_dns_lookup *l;
		rt_in6_set_v4mapped(&a, rt_htonl(0xc0000201u));
		l = rt_dns_lookup_start_addr(&cctx, &a, 0, &err);
		CHECK(l && run(l, NULL) == 1);
		rt_dns_lookup_get(l, NULL, NULL, NULL, NULL, canon, sizeof canon);
		CHECK(!strcmp(canon, "both.test"));
		rt_dns_lookup_free(l);
		rt_in6_set_v4mapped(&a, rt_htonl(0xc0000299u));
		l = rt_dns_lookup_start_addr(&cctx, &a, 0, &err);
		CHECK(l && run(l, NULL) == -RT_HOST_NOT_FOUND);
		rt_dns_lookup_free(l);
		rt_in6_set_v4mapped(&a, rt_htonl(0x7f000001u));
		l = rt_dns_lookup_start_addr(&cctx, &a, 0, &err);
		CHECK(l && rt_dns_lookup_poll(l) == 1);
		rt_dns_lookup_get(l, NULL, NULL, NULL, NULL, canon, sizeof canon);
		CHECK(!strcmp(canon, "localhost"));
		rt_dns_lookup_free(l);
	}

	/* a lookup ended before its answer: its sockets go */
	{
		int err;
		struct rt_dns_lookup *l = rt_dns_lookup_start(&cctx, "silent2.test", RT_AF_UNSPEC, 0, 0, &err);
		CHECK(l && rt_dns_lookup_poll(l) == 0);
		rt_dns_lookup_free(l);
	}

	/* ---- Happy Eyeballs, asynchronously: lo.test is ::1 and 127.0.0.1 ---- */
	{
		struct rt_sockctx hctx;
		struct rt_he_conn *h;
		struct rt_he_result res;
		struct rt_sockaddr_in6 s6;
		int l4, l6, err, fd, guard;
		rt_he_aaaa_filter = 0;
		CHECK(rt_ctx_init(&hctx, 64, NULL) == 0);
		l4 = rt_api_socket(&hctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		sa.sin_port = rt_htons(8080);
		CHECK(rt_api_bind(&hctx, l4, &sa, sizeof sa) == 0 && rt_api_listen(&hctx, l4, 4) == 0);
		/* IPv6 first (RFC 6724: ::1 before 127.0.0.1), refused there, then IPv4 at once */
		h = rt_he_conn_start(&cctx, "lo.test", 8080, RT_AF_UNSPEC, RT_HE_V6SOCK, -1, 5000, &err);
		CHECK(h != NULL);
		for (guard = 0; (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		CHECK(r == 0);
		fd = rt_he_conn_take(h, &res);
		CHECK(fd >= 0 && res.naddrs == 2 && res.attempts == 2 && res.index == 1 && rt_in6_is_v4mapped(&res.addr));
		printf("lo.test:8080 (IPv4 only): %u attempts, connected to the %u. in %u ms\n", res.attempts,
		       res.index + 1, (unsigned)res.ms);
		rt_he_conn_free(h);
		if (fd >= 0)
			rt_api_close(&cctx, fd);
		/* a listener on both families: the first, IPv6, wins */
		l6 = rt_api_socket(&hctx, RT_AF_INET6, RT_SOCK_STREAM, 0);
		memset(&s6, 0, sizeof s6);
		s6.sin6_len = sizeof s6;
		s6.sin6_family = RT_AF_INET6;
		s6.sin6_port = rt_htons(8081);
		CHECK(rt_api_bind(&hctx, l6, &s6, sizeof s6) == 0 && rt_api_listen(&hctx, l6, 4) == 0);
		h = rt_he_conn_start(&cctx, "lo.test", 8081, RT_AF_UNSPEC, 0, -1, 5000, &err);
		for (guard = 0; h && (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		CHECK(h && r == 0);
		fd = rt_he_conn_take(h, &res);
		CHECK(fd >= 0 && res.attempts == 1 && !rt_in6_is_v4mapped(&res.addr));
		rt_he_conn_free(h);
		if (fd >= 0)
			rt_api_close(&cctx, fd);
		/* prefer IPv4 */
		h = rt_he_conn_start(&cctx, "lo.test", 8081, RT_AF_UNSPEC, 0, 4, 5000, &err);
		for (guard = 0; h && (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		fd = h ? rt_he_conn_take(h, &res) : -1;
		CHECK(r == 0 && fd >= 0 && res.attempts == 1 && rt_in6_is_v4mapped(&res.addr));
		rt_he_conn_free(h);
		if (fd >= 0)
			rt_api_close(&cctx, fd);
		/* a literal; a name that does not exist; nothing listening */
		h = rt_he_conn_start(&cctx, "127.0.0.1", 8080, RT_AF_UNSPEC, 0, -1, 5000, &err);
		for (guard = 0; h && (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		CHECK(h && r == 0);
		rt_he_conn_free(h);
		CHECK(rt_he_conn_start(&cctx, "127.0.0.1", 8080, RT_AF_INET6, 0, -1, 5000, &err) == NULL &&
		      err == -RT_EAFNOSUPPORT);
		h = rt_he_conn_start(&cctx, "nx.test", 8080, RT_AF_UNSPEC, 0, -1, 5000, &err);
		for (guard = 0; h && (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		CHECK(h && r == RT_HE_LOOKUP_FAILED && rt_he_conn_dns_error(h) == -RT_HOST_NOT_FOUND);
		rt_he_conn_free(h);
		h = rt_he_conn_start(&cctx, "lo.test", 8099, RT_AF_UNSPEC, 0, -1, 5000, &err);
		for (guard = 0; h && (r = rt_he_conn_poll(h)) == 1 && guard < 2000; guard++)
			usleep(2000);
		CHECK(h && r == -RT_ECONNREFUSED);
		rt_he_conn_info(h, &res);
		CHECK(res.attempts == 2);
		rt_he_conn_free(h);
		rt_api_close(&hctx, l4);
		rt_api_close(&hctx, l6);
		rt_ctx_destroy(&hctx);
	}

	stop = 1;
	pthread_join(th, NULL);
	printf("t_dns_async: %d checks, %d failures (%d UDP and %d TCP queries)\n", checks, failures, nq_udp, nq_tcp);
	return failures ? 1 : 0;
}
