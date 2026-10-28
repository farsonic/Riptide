/*
 * Riptide - unit tests of the resolver message code (malformed and fuzzed
 * responses), the local resolver paths (numeric, hosts, localhost,
 * disallow), the BIND style helpers, configuration parsing and netdb.
 * No network: runs without TUN.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "../src/core/sock_internal.h"
#include "../src/core/resolv_internal.h"

__attribute__((weak)) struct rt_tcp_stats rt_tcpstat;
__attribute__((weak)) const struct rt_proto_ops rt_tcp_ops;
__attribute__((weak)) void rt_tcp_init(void) {}
__attribute__((weak)) void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip) { rt_buf_free(b); }
__attribute__((weak)) void rt_tcp_icmp_error(u8 t, u8 c, u32 i, const struct rt_iphdr *in, u32 l) {}
__attribute__((weak)) int rt_tcp_list(struct rt_pcb_info *o, int m) { return 0; }

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 ip4(int a, int b, int c, int d) { return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d); }

/* ---- response builder ---- */
struct msg { u8 b[4096]; u32 len; };

static void put8(struct msg *m, u8 v) { m->b[m->len++] = v; }
static void put16(struct msg *m, u16 v) { rt_put_be16(m->b + m->len, v); m->len += 2; }
static void put32(struct msg *m, u32 v) { rt_put_be32(m->b + m->len, v); m->len += 4; }
static void putname(struct msg *m, const char *n)
{
	while (*n) {
		const char *d = strchr(n, '.');
		u32 l = d ? (u32)(d - n) : (u32)strlen(n);
		put8(m, (u8)l);
		memcpy(m->b + m->len, n, l);
		m->len += l;
		n += l;
		if (*n == '.')
			n++;
	}
	put8(m, 0);
}
static void putptr(struct msg *m, u16 off) { put16(m, (u16)(0xc000 | off)); }

static void hdr(struct msg *m, u16 id, u16 flags, u16 an)
{
	m->len = 0;
	put16(m, id); put16(m, flags); put16(m, 1); put16(m, an); put16(m, 0); put16(m, 0);
}
static void question(struct msg *m, const char *n, u16 type)
{
	putname(m, n); put16(m, type); put16(m, 1);
}
/* RR with owner given as pointer (off != 0) or name */
static void rr_a(struct msg *m, u16 owner_ptr, u32 ttl, u32 addr)
{
	putptr(m, owner_ptr); put16(m, DNS_TYPE_A); put16(m, 1); put32(m, ttl); put16(m, 4);
	memcpy(m->b + m->len, &addr, 4); m->len += 4;
}

static struct dns_result res;
static u32 addrs[16];

static int parse(struct msg *m, u16 id, const char *q, u16 type)
{
	memset(&res, 0, sizeof(res));
	res.addrs = addrs;
	res.max = 8;
	return dns_parse_response(m->b, m->len, id, q, type, &res);
}

static void test_build(void)
{
	u8 q[300];
	int n;
	char big[300];

	n = dns_build_query(q, sizeof(q), 0x1234, "www.example.com", DNS_TYPE_A);
	CHECK(n == 12 + 17 + 4);
	CHECK(q[0] == 0x12 && q[1] == 0x34 && q[2] == 1 && q[5] == 1);
	CHECK(q[12] == 3 && !memcmp(q + 13, "www", 3) && q[16] == 7 && q[28] == 0);
	CHECK(dns_build_query(q, sizeof(q), 1, "www.example.com.", DNS_TYPE_A) == n);
	CHECK(dns_build_query(q, sizeof(q), 1, "a..b", DNS_TYPE_A) < 0);
	CHECK(dns_build_query(q, sizeof(q), 1, ".a", DNS_TYPE_A) < 0);
	CHECK(dns_build_query(q, sizeof(q), 1, "", DNS_TYPE_A) < 0);
	CHECK(dns_build_query(q, sizeof(q), 1, ".", DNS_TYPE_A) == 12 + 1 + 4);
	memset(big, 'a', 64); big[64] = 0;
	CHECK(dns_build_query(q, sizeof(q), 1, big, DNS_TYPE_A) < 0);       /* label > 63 */
	for (n = 0; n < 299; n++) big[n] = (n % 2) ? '.' : 'a';
	big[299] = 0;
	CHECK(dns_build_query(q, sizeof(q), 1, big, DNS_TYPE_A) < 0);       /* name > 255 */
	CHECK(dns_build_query(q, 20, 1, "www.example.com", DNS_TYPE_A) < 0); /* buffer */
	CHECK(dns_name_eq("WWW.Example.COM.", "www.example.com"));
	CHECK(!dns_name_eq("www.example.co", "www.example.com"));
}

static void test_parse(void)
{
	struct msg m;
	u32 qend;

	/* simple answer with two A records, compression to the question */
	hdr(&m, 7, 0x8180, 2);
	question(&m, "host.example.com", DNS_TYPE_A);
	rr_a(&m, 12, 300, ip4(10, 0, 0, 1));
	rr_a(&m, 12, 100, ip4(10, 0, 0, 2));
	CHECK(parse(&m, 7, "host.example.com", DNS_TYPE_A) == 2);
	CHECK(addrs[0] == ip4(10, 0, 0, 1) && addrs[1] == ip4(10, 0, 0, 2) && res.ttl == 100);
	CHECK(!strcmp(res.canon, "host.example.com"));
	CHECK(parse(&m, 7, "HOST.EXAMPLE.COM.", DNS_TYPE_A) == 2);
	/* wrong id, question, type, not a response */
	CHECK(parse(&m, 8, "host.example.com", DNS_TYPE_A) == DNS_IGNORE);
	CHECK(parse(&m, 7, "other.example.com", DNS_TYPE_A) == DNS_IGNORE);
	CHECK(parse(&m, 7, "host.example.com", DNS_TYPE_PTR) == DNS_IGNORE);
	m.b[2] &= 0x7f;
	CHECK(parse(&m, 7, "host.example.com", DNS_TYPE_A) == DNS_IGNORE);
	m.b[2] |= 0x80;
	/* duplicate addresses collapse, max respected */
	hdr(&m, 7, 0x8180, 3);
	question(&m, "h", DNS_TYPE_A);
	rr_a(&m, 12, 300, ip4(1, 1, 1, 1));
	rr_a(&m, 12, 300, ip4(1, 1, 1, 1));
	rr_a(&m, 12, 300, ip4(1, 1, 1, 2));
	CHECK(parse(&m, 7, "h", DNS_TYPE_A) == 2);
	memset(&res, 0, sizeof(res));
	res.addrs = addrs;
	res.max = 1;
	CHECK(dns_parse_response(m.b, m.len, 7, "h", DNS_TYPE_A, &res) == 1);

	/* rcodes */
	hdr(&m, 9, 0x8183, 0);
	question(&m, "nx.example.com", DNS_TYPE_A);
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == -RT_HOST_NOT_FOUND);
	m.b[3] = 0x82;
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == -RT_TRY_AGAIN);
	m.b[3] = 0x85;
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == -RT_TRY_AGAIN);
	m.b[3] = 0x81;
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == -RT_NO_RECOVERY);
	m.b[3] = 0x80;
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == 0);         /* NODATA */
	m.b[2] |= 0x02;                                                    /* TC */
	CHECK(parse(&m, 9, "nx.example.com", DNS_TYPE_A) == -RT_TRY_AGAIN);

	/* CNAME chain, records out of order: A for target first */
	hdr(&m, 3, 0x8180, 3);
	question(&m, "www.a.org", DNS_TYPE_A);
	qend = m.len;
	/* A record owned by "real.b.net" (written in full) */
	putname(&m, "real.b.net"); put16(&m, 1); put16(&m, 1); put32(&m, 50); put16(&m, 4);
	put32(&m, 0xc0000201u);
	/* CNAME www.a.org -> mid.a.org ; mid.a.org -> real.b.net */
	putptr(&m, 12); put16(&m, 5); put16(&m, 1); put32(&m, 500); put16(&m, 6);
	put8(&m, 3); memcpy(m.b + m.len, "mid", 3); m.len += 3; putptr(&m, 16);
	{
		u32 mid = m.len - 6;
		putptr(&m, (u16)mid); put16(&m, 5); put16(&m, 1); put32(&m, 40); put16(&m, 2);
		putptr(&m, (u16)qend);
	}
	CHECK(parse(&m, 3, "www.a.org", DNS_TYPE_A) == 1);
	CHECK(addrs[0] == rt_htonl(0xc0000201u) && !strcmp(res.canon, "real.b.net") && res.ttl == 40);

	/* CNAME loop a -> b -> a */
	hdr(&m, 4, 0x8180, 2);
	question(&m, "a", DNS_TYPE_A);
	putptr(&m, 12); put16(&m, 5); put16(&m, 1); put32(&m, 5); put16(&m, 3);
	put8(&m, 1); put8(&m, 'b'); put8(&m, 0);
	{
		u32 bname = m.len - 3;
		putptr(&m, (u16)bname); put16(&m, 5); put16(&m, 1); put32(&m, 5); put16(&m, 2);
		putptr(&m, 12);
	}
	CHECK(parse(&m, 4, "a", DNS_TYPE_A) == -RT_NO_RECOVERY);

	/* compression loops: pointer to itself, forward pointer, mutual */
	hdr(&m, 5, 0x8180, 1);
	question(&m, "x", DNS_TYPE_A);
	putptr(&m, (u16)m.len); put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 4); put32(&m, 1);
	CHECK(parse(&m, 5, "x", DNS_TYPE_A) == 0);
	{
		char out[256];
		u32 next;
		u8 loop[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 14, 0xc0, 12 };
		CHECK(dns_expand(loop, sizeof(loop), 12, out, sizeof(out), &next) < 0);
		CHECK(dns_expand(loop, sizeof(loop), 14, out, sizeof(out), &next) < 0);
		u8 lab[] = { 1, 'a', 0xc0, 0 };   /* pointer back to itself region */
		CHECK(dns_expand(lab, sizeof(lab), 0, out, sizeof(out), &next) < 0);
		u8 bad[] = { 0x40, 'a', 0 };      /* reserved label type */
		CHECK(dns_expand(bad, sizeof(bad), 0, out, sizeof(out), &next) < 0);
		u8 dot[] = { 3, 'a', '.', 'b', 0 };
		CHECK(dns_expand(dot, sizeof(dot), 0, out, sizeof(out), &next) < 0);
		CHECK(rt_dn_expand(dot, dot + sizeof(dot), dot, out, sizeof(out)) == 5 &&
		      !strcmp(out, "a\\.b"));
		u8 bin[] = { 2, 'a', 1, 0 };
		CHECK(rt_dn_expand(bin, bin + sizeof(bin), bin, out, sizeof(out)) == 4 &&
		      !strcmp(out, "a\\001"));
		CHECK(rt_dn_expand(bin, bin + sizeof(bin), bin, out, 3) < 0);
		u8 ok[] = { 1, 'x', 0, 1, 'y', 0xc0, 0 };
		CHECK(dns_expand(ok, sizeof(ok), 3, out, sizeof(out), &next) == 0 &&
		      !strcmp(out, "y.x") && next == 7);
		CHECK(rt_dn_skipname(ok + 3, ok + sizeof(ok)) == 4);
		CHECK(rt_dn_skipname(ok, ok + sizeof(ok)) == 3);
		CHECK(rt_dn_skipname(ok, ok + 2) < 0);
		/* output buffer too small */
		CHECK(dns_expand(ok, sizeof(ok), 3, out, 3, &next) < 0);
		/* root */
		u8 root[] = { 0 };
		CHECK(dns_expand(root, 1, 0, out, sizeof(out), &next) == 0 && out[0] == 0 && next == 1);
	}

	/* PTR */
	hdr(&m, 6, 0x8580, 1);
	question(&m, "4.3.2.1.in-addr.arpa", DNS_TYPE_PTR);
	putptr(&m, 12); put16(&m, 12); put16(&m, 1); put32(&m, 3600); put16(&m, 9);
	putname(&m, "box.lan");
	CHECK(parse(&m, 6, "4.3.2.1.in-addr.arpa", DNS_TYPE_PTR) == 1 && !strcmp(res.name, "box.lan"));

	/* bad rdlength for A is skipped; truncated RR stops parsing */
	hdr(&m, 7, 0x8180, 2);
	question(&m, "h", DNS_TYPE_A);
	putptr(&m, 12); put16(&m, 1); put16(&m, 1); put32(&m, 1); put16(&m, 3); put8(&m, 1); put8(&m, 2); put8(&m, 3);
	rr_a(&m, 12, 1, ip4(9, 9, 9, 9));
	CHECK(parse(&m, 7, "h", DNS_TYPE_A) == 1 && addrs[0] == ip4(9, 9, 9, 9));
	m.len -= 2;
	CHECK(parse(&m, 7, "h", DNS_TYPE_A) == 0);
}

/* EDNS0 OPT record, TC, answers beyond 512 bytes, many records, CNAME
 * chains up to the limit */
/* SOA in the authority section (ns records after an answers) */
static void soa(struct msg *m, u32 ttl, u32 minimum)
{
	putptr(m, 12); put16(m, DNS_TYPE_SOA); put16(m, 1); put32(m, ttl); put16(m, 32);
	putname(m, "ns.x"); putname(m, "hm.x");
	put32(m, 1); put32(m, 2); put32(m, 3); put32(m, 4); put32(m, minimum);
}

/* RFC 2308: how long NXDOMAIN / NODATA may be cached */
static void test_negttl(void)
{
	struct msg m;

	/* NXDOMAIN: min(SOA TTL, MINIMUM) */
	hdr(&m, 9, 0x8183, 0);
	rt_put_be16(m.b + 8, 1);
	question(&m, "nx.example", DNS_TYPE_A);
	soa(&m, 3600, 900);
	CHECK(parse(&m, 9, "nx.example", DNS_TYPE_A) == -RT_HOST_NOT_FOUND && res.negttl == 900);
	hdr(&m, 9, 0x8183, 0);
	rt_put_be16(m.b + 8, 1);
	question(&m, "nx.example", DNS_TYPE_A);
	soa(&m, 60, 900);
	CHECK(parse(&m, 9, "nx.example", DNS_TYPE_A) == -RT_HOST_NOT_FOUND && res.negttl == 60);
	/* without SOA: 0, not cached */
	hdr(&m, 9, 0x8183, 0);
	question(&m, "nx.example", DNS_TYPE_A);
	CHECK(parse(&m, 9, "nx.example", DNS_TYPE_A) == -RT_HOST_NOT_FOUND && res.negttl == 0);
	/* NODATA through a CNAME: also bounded by the CNAME's TTL */
	hdr(&m, 9, 0x8180, 1);
	rt_put_be16(m.b + 8, 1);
	question(&m, "a.example", DNS_TYPE_AAAA);
	putptr(&m, 12); put16(&m, DNS_TYPE_CNAME); put16(&m, 1); put32(&m, 30); put16(&m, 5);
	putname(&m, "b.x");
	soa(&m, 300, 300);
	CHECK(parse(&m, 9, "a.example", DNS_TYPE_AAAA) == 0 && res.negttl == 30);
	/* a broken SOA: not cached */
	hdr(&m, 9, 0x8180, 0);
	rt_put_be16(m.b + 8, 1);
	question(&m, "a.example", DNS_TYPE_AAAA);
	soa(&m, 300, 300);
	rt_put_be16(m.b + m.len - 34, 31);   /* RDLENGTH one short */
	CHECK(parse(&m, 9, "a.example", DNS_TYPE_AAAA) == 0 && res.negttl == 0);
	/* a positive answer has none */
	hdr(&m, 9, 0x8180, 1);
	question(&m, "a.example", DNS_TYPE_A);
	rr_a(&m, 12, 300, ip4(10, 0, 0, 1));
	CHECK(parse(&m, 9, "a.example", DNS_TYPE_A) == 1 && res.negttl == 0);
}

static void test_edns(void)
{
	static struct msg m;
	static rt_in6_addr a6[16];
	u8 q[300];
	u16 size = 0;
	int n, i, l;

	/* OPT appended to a query */
	n = dns_build_query(q, sizeof(q), 0x4242, "www.example.com", DNS_TYPE_AAAA);
	l = dns_add_opt(q, (u32)n, sizeof(q), 1232);
	CHECK(l == n + 11 && rt_get_be16(q + 10) == 1);
	CHECK(q[n] == 0 && rt_get_be16(q + n + 1) == DNS_TYPE_OPT && rt_get_be16(q + n + 3) == 1232 &&
	      rt_get_be32(q + n + 5) == 0 && rt_get_be16(q + n + 9) == 0);
	CHECK(dns_find_opt(q, (u32)l, &size) == 1 && size == 1232);
	CHECK(dns_find_opt(q, (u32)n, &size) == -1);          /* ARCOUNT 1, record missing */
	rt_put_be16(q + 10, 0);
	CHECK(dns_find_opt(q, (u32)n, &size) == 0);
	CHECK(dns_add_opt(q, (u32)n, (u32)n + 10, 1232) < 0);  /* no room */
	CHECK(dns_add_opt(q, 8, sizeof(q), 1232) < 0);

	/* answer echoing OPT after an A record; OPT type in the answer
	 * section does not count; malformed rdlength */
	hdr(&m, 1, 0x8180, 1);
	question(&m, "e.test", DNS_TYPE_A);
	rr_a(&m, 12, 60, ip4(10, 0, 0, 1));
	CHECK(dns_find_opt(m.b, m.len, NULL) == 0);
	rt_put_be16(m.b + 10, 1);
	put8(&m, 0); put16(&m, DNS_TYPE_OPT); put16(&m, 4096); put32(&m, 0); put16(&m, 0);
	CHECK(dns_find_opt(m.b, m.len, &size) == 1 && size == 4096);
	CHECK(parse(&m, 1, "e.test", DNS_TYPE_A) == 1);        /* OPT ignored by the parser */
	m.b[m.len - 1] = 5;
	CHECK(dns_find_opt(m.b, m.len, &size) == -1);
	hdr(&m, 1, 0x8180, 1);
	question(&m, "e.test", DNS_TYPE_A);
	put8(&m, 0); put16(&m, DNS_TYPE_OPT); put16(&m, 4096); put32(&m, 0); put16(&m, 0);
	CHECK(dns_find_opt(m.b, m.len, &size) == 0);
	CHECK(dns_find_opt(m.b, 5, &size) == -1);

	/* TC with data: what fits is used; FORMERR (EDNS refused) */
	hdr(&m, 2, 0x8380, 2);
	question(&m, "tc.test", DNS_TYPE_A);
	rr_a(&m, 12, 60, ip4(10, 0, 0, 7));
	CHECK(parse(&m, 2, "tc.test", DNS_TYPE_A) == 1 && addrs[0] == ip4(10, 0, 0, 7));
	hdr(&m, 2, 0x8181, 0);
	question(&m, "tc.test", DNS_TYPE_A);
	CHECK(parse(&m, 2, "tc.test", DNS_TYPE_A) == -RT_NO_RECOVERY);

	/* 40 A records: about 700 bytes (only fits with EDNS or TCP); 16 kept */
	hdr(&m, 3, 0x8180, 40);
	question(&m, "cdn.test", DNS_TYPE_A);
	for (i = 0; i < 40; i++)
		rr_a(&m, 12, 30, ip4(10, 1, 0, i + 1));
	CHECK(m.len > 512);
	memset(&res, 0, sizeof(res));
	res.addrs = addrs;
	res.max = 16;
	CHECK(dns_parse_response(m.b, m.len, 3, "cdn.test", DNS_TYPE_A, &res) == 16);
	CHECK(addrs[0] == ip4(10, 1, 0, 1) && addrs[15] == ip4(10, 1, 0, 16));

	/* 24 AAAA records */
	hdr(&m, 4, 0x8180, 24);
	question(&m, "cdn6.test", DNS_TYPE_AAAA);
	for (i = 0; i < 24; i++) {
		putptr(&m, 12); put16(&m, DNS_TYPE_AAAA); put16(&m, 1); put32(&m, 30); put16(&m, 16);
		memset(m.b + m.len, 0, 16);
		m.b[m.len] = 0x20; m.b[m.len + 1] = 0x01; m.b[m.len + 15] = (u8)(i + 1);
		m.len += 16;
	}
	CHECK(m.len > 512);
	memset(&res, 0, sizeof(res));
	res.addrs6 = a6;
	res.max = 16;
	CHECK(dns_parse_response(m.b, m.len, 4, "cdn6.test", DNS_TYPE_AAAA, &res) == 16);
	CHECK(a6[0].b[0] == 0x20 && a6[0].b[15] == 1 && a6[15].b[15] == 16);

	/* CNAME chains: c0 -> c1 -> ... -> cN, then A for cN (names in full) */
	for (l = 1; l <= 10; l++) {
		char nm[16], nx[16];
		hdr(&m, 5, 0x8180, (u16)(l + 1));
		question(&m, "c0.chain.test", DNS_TYPE_A);
		for (i = 0; i < l; i++) {
			u32 at;
			snprintf(nm, sizeof(nm), "c%d.chain.test", i);
			snprintf(nx, sizeof(nx), "c%d.chain.test", i + 1);
			putname(&m, nm); put16(&m, DNS_TYPE_CNAME); put16(&m, 1); put32(&m, 100); put16(&m, 0);
			at = m.len;
			putname(&m, nx);
			rt_put_be16(m.b + at - 2, (u16)(m.len - at));
		}
		putname(&m, nx); put16(&m, DNS_TYPE_A); put16(&m, 1); put32(&m, 100); put16(&m, 4);
		put32(&m, 0x0a636301u);
		n = parse(&m, 5, "c0.chain.test", DNS_TYPE_A);
		if (l < DNS_MAX_CNAME)
			CHECK(n == 1 && addrs[0] == rt_htonl(0x0a636301u) && !strcmp(res.canon, nx));
		else
			CHECK(n <= 0);                             /* longer than DNS_MAX_CNAME */
	}
	CHECK(m.len > 200);
}

/* Every truncation and many random mutations of valid responses must be
 * handled without crashing (run under ASan with make SAN=1). */
static void test_fuzz(void)
{
	struct msg m, f;
	u32 i, k, seed = 12345;
	int r, sane = 1;

	hdr(&m, 0x4242, 0x8180, 3);
	question(&m, "www.fuzz.example", DNS_TYPE_A);
	putptr(&m, 12); put16(&m, 5); put16(&m, 1); put32(&m, 10); put16(&m, 7);
	put8(&m, 4); memcpy(m.b + m.len, "real", 4); m.len += 4; putptr(&m, 16);
	rr_a(&m, (u16)(m.len - 7), 300, ip4(10, 1, 2, 3));
	rr_a(&m, 12, 300, ip4(10, 1, 2, 4));
	r = parse(&m, 0x4242, "www.fuzz.example", DNS_TYPE_A);
	CHECK(r == 1 && addrs[0] == ip4(10, 1, 2, 3));

	for (i = 0; i <= m.len; i++) {
		f = m;
		f.len = i;
		r = parse(&f, 0x4242, "www.fuzz.example", DNS_TYPE_A);
		if (r > 8 || res.naddr > 8)
			sane = 0;
	}
	for (i = 0; i < 200000; i++) {
		u32 nmut;
		f = m;
		seed = seed * 1103515245u + 12345u;
		nmut = 1 + (seed >> 16) % 6;
		for (k = 0; k < nmut; k++) {
			seed = seed * 1103515245u + 12345u;
			f.b[12 + (seed >> 8) % (m.len - 12)] = (u8)(seed >> 24);
		}
		seed = seed * 1103515245u + 12345u;
		if ((seed >> 20) % 4 == 0)
			f.len = 12 + (seed >> 8) % (m.len - 11);
		r = parse(&f, 0x4242, "www.fuzz.example", DNS_TYPE_A);
		if (r > 8 || res.naddr > 8 || strlen(res.canon) > 255)
			sane = 0;
		{
			char out[64];
			rt_dn_expand(f.b, f.b + f.len, f.b + 12 + (seed % (f.len > 12 ? f.len - 12 : 1)), out, sizeof(out));
			rt_dn_skipname(f.b + 12, f.b + f.len);
		}
	}
	/* fully random garbage with a valid header/question prefix */
	for (i = 0; i < 50000; i++) {
		f = m;
		f.len = 12 + 22 + (i % 200);
		for (k = 34; k < f.len; k++) {
			seed = seed * 1103515245u + 12345u;
			f.b[k] = (u8)(seed >> 24);
		}
		r = parse(&f, 0x4242, "www.fuzz.example", DNS_TYPE_A);
		if (r > 8)
			sane = 0;
	}
	CHECK(sane);
}

static void test_local_resolve(void)
{
	u32 a[4];
	char canon[64], name[64];

	rt_lock();
	rt_hosts_clear();
	rt_dns_clear_servers();
	CHECK(rt_hosts_add("fileserver.lan", ip4(192, 168, 1, 5)) == 0);
	CHECK(rt_hosts_add("multi", ip4(192, 168, 1, 6)) == 0);
	CHECK(rt_hosts_add("multi", ip4(192, 168, 1, 7)) == 0);
	CHECK(rt_hosts_add("", 1) == -RT_EINVAL);
	rt_dns_set_search("lan.");
	rt_unlock();

	CHECK(rt_resolve_name("10.20.30.40", a, 4, canon, sizeof(canon)) == 1 &&
	      a[0] == ip4(10, 20, 30, 40) && !strcmp(canon, "10.20.30.40"));
	CHECK(rt_resolve_name("localhost", a, 4, NULL, 0) == 1 && a[0] == RT_INADDR_LOOPBACK);
	CHECK(rt_resolve_name("FileServer.lan", a, 4, canon, sizeof(canon)) == 1 && a[0] == ip4(192, 168, 1, 5));
	CHECK(rt_resolve_name("fileserver", a, 4, NULL, 0) == 1 && a[0] == ip4(192, 168, 1, 5));
	CHECK(rt_resolve_name("fileserver.lan.", a, 4, NULL, 0) == 1);
	CHECK(rt_resolve_name("multi", a, 4, NULL, 0) == 2 && a[1] == ip4(192, 168, 1, 7));
	CHECK(rt_resolve_name("multi", a, 1, NULL, 0) == 1);
	/* no servers: TRY_AGAIN without waiting */
	CHECK(rt_resolve_name("unknown.example", a, 4, NULL, 0) == -RT_TRY_AGAIN);
	CHECK(rt_resolve_name("", a, 4, NULL, 0) == -RT_HOST_NOT_FOUND);
	CHECK(rt_resolve_name(NULL, a, 4, NULL, 0) < 0);
	rt_lock();
	rt_dns_disallowed = 1;
	rt_unlock();
	CHECK(rt_resolve_name("unknown.example", a, 4, NULL, 0) == -RT_HOST_NOT_FOUND);
	CHECK(rt_resolve_name("multi", a, 4, NULL, 0) == 2);
	CHECK(rt_resolve_addr(ip4(8, 8, 8, 8), name, sizeof(name)) == -RT_HOST_NOT_FOUND);
	rt_lock();
	rt_dns_disallowed = 0;
	rt_unlock();
	CHECK(rt_resolve_addr(ip4(192, 168, 1, 5), name, sizeof(name)) == 0 && !strcmp(name, "fileserver.lan"));
	CHECK(rt_resolve_addr(RT_INADDR_LOOPBACK, name, sizeof(name)) == 0 && !strcmp(name, "localhost"));

	rt_lock();
	{
		u32 s[4];
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 1)) == 0);
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 1)) == 0);        /* duplicate ignored */
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 2)) == 0);
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 3)) == 0);
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 4)) == 0);
		CHECK(rt_dns_add_server(ip4(1, 1, 1, 5)) == -RT_ENOSPC);
		CHECK(rt_dns_add_server(0) == -RT_EINVAL);
		CHECK(rt_dns_get_servers(s, 4) == 4 && s[3] == ip4(1, 1, 1, 4));
		rt_dns_remove_server(ip4(1, 1, 1, 2));
		CHECK(rt_dns_get_servers(s, 4) == 3 && s[1] == ip4(1, 1, 1, 3));
		rt_dns_clear_servers();
		CHECK(rt_dns_get_servers(s, 4) == 0);
	}
	rt_hosts_clear();
	rt_unlock();
	{
		u8 q[300];
		CHECK(rt_res_mkquery(0, "a.b", 1, 16, q, sizeof(q)) == 12 + 5 + 4 && q[12 + 5 + 1] == 16);
		CHECK(rt_res_mkquery(1, "a.b", 1, 1, q, sizeof(q)) == -1);
		CHECK(rt_res_query("x.example", 1, 1, q, sizeof(q)) == -RT_TRY_AGAIN);  /* no servers */
	}
}

static int unknown_calls;
static int unknown_kw(int argc, char **argv, char *err, u32 errlen)
{
	unknown_calls++;
	if (!strcmp(argv[0], "interface") && argc == 3)
		return 0;
	rt_snprintf(err, errlen, "bad %s", argv[0]);
	return -RT_EINVAL;
}

static void test_config(void)
{
	char line[128], err[128], *argv[8];
	char buf[16];
	u32 a;
	int argc;

	CHECK(rt_parse_ipv4("192.168.0.1", &a) == 0 && a == ip4(192, 168, 0, 1));
	CHECK(rt_parse_ipv4("0.0.0.0", &a) == 0 && a == 0);
	CHECK(rt_parse_ipv4("255.255.255.255", &a) == 0 && a == 0xffffffffu);
	CHECK(rt_parse_ipv4("256.1.1.1", &a) < 0);
	CHECK(rt_parse_ipv4("1.2.3", &a) < 0);
	CHECK(rt_parse_ipv4("1.2.3.4.5", &a) < 0);
	CHECK(rt_parse_ipv4("1.2.3.4 ", &a) < 0);
	CHECK(rt_parse_ipv4("1..3.4", &a) < 0);
	CHECK(rt_parse_ipv4("0001.2.3.4", &a) < 0);
	CHECK(rt_parse_ipv4("", &a) < 0);
	CHECK(!strcmp(rt_format_ipv4(ip4(10, 0, 200, 1), buf), "10.0.200.1"));
	CHECK(!strcmp(rt_format_ipv4(0xffffffffu, buf), "255.255.255.255"));

	strcpy(line, "  route add  \"quoted word\" x\\y # comment");
	argc = rt_config_split(line, argv, 8);
	CHECK(argc == 4 && !strcmp(argv[0], "route") && !strcmp(argv[2], "quoted word") &&
	      !strcmp(argv[3], "x\\y"));
	strcpy(line, "a b c d e");
	CHECK(rt_config_split(line, argv, 3) == 3);
	strcpy(line, "\"esc \\\" q\"");
	CHECK(rt_config_split(line, argv, 3) == 1 && !strcmp(argv[0], "esc \" q"));
	strcpy(line, "# only comment");
	CHECK(rt_config_split(line, argv, 3) == 0);

	rt_lock();
	CHECK(rt_config_line("", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("   # x", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("hostname amiga1200", err, sizeof(err), NULL) == 0 && !strcmp(rt_hostname, "amiga1200"));
	CHECK(rt_config_line("hostname", err, sizeof(err), NULL) < 0 && strstr(err, "usage"));
	CHECK(rt_config_line("domain home.lan", err, sizeof(err), NULL) == 0 && !strcmp(rt_domainname, "home.lan"));
	CHECK(rt_config_line("nameserver 9.9.9.9", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("nameserver 9.9.9", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "9.9.9"));
	{
		u32 s[4];
		CHECK(rt_dns_get_servers(s, 4) == 1 && s[0] == ip4(9, 9, 9, 9));
	}
	CHECK(rt_config_line("host nas 10.0.0.9", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("host nas", err, sizeof(err), NULL) < 0);
	CHECK(rt_config_line("sysctl dns.cache.enabled=0", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("sysctl dns.cache.enabled 1", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("sysctl dns.cache.enabled=7", err, sizeof(err), NULL) == -RT_EINVAL &&
	      strstr(err, "bad value"));
	CHECK(rt_config_line("sysctl no.such=1", err, sizeof(err), NULL) == -RT_ENOENT && strstr(err, "unknown"));
	/* resolver: EDNS0 and TCP fallback */
	CHECK(rt_dns_edns_size == 1232 && rt_dns_tcp == 1);
	CHECK(rt_config_line("dns edns_size 4096", err, sizeof(err), NULL) == 0 && rt_dns_edns_size == 4096);
	CHECK(rt_config_line("dns edns_size 0", err, sizeof(err), NULL) == 0 && rt_dns_edns_size == 0);
	CHECK(rt_config_line("dns edns on", err, sizeof(err), NULL) == 0 && rt_dns_edns_size == 1232);
	CHECK(rt_config_line("dns edns_size 100", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "512"));
	CHECK(rt_config_line("dns edns_size 5000", err, sizeof(err), NULL) == -RT_EINVAL);
	CHECK(rt_config_line("dns tcp off", err, sizeof(err), NULL) == 0 && rt_dns_tcp == 0);
	CHECK(rt_config_line("dns", err, sizeof(err), NULL) == 0 && strstr(err, "EDNS0 1232 bytes") &&
	      strstr(err, "TCP fallback off"));
	CHECK(rt_config_line("dns tcp on", err, sizeof(err), NULL) == 0 && rt_dns_tcp == 1);
	CHECK(rt_config_line("dns negttl 600", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("dns negttl 86401", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "86400"));
	CHECK(rt_config_line("dns failttl x", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "300"));
	CHECK(rt_config_line("sysctl dns.cache.neg_maxttl=3600", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("sysctl dns.cache.fail_max=301", err, sizeof(err), NULL) == -RT_EINVAL);
	CHECK(rt_config_line("dns flush", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("dns reset", err, sizeof(err), NULL) == 0);
	CHECK(rt_config_line("dns frob", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "usage"));
	CHECK(rt_config_line("sysctl dns.edns_size=512", err, sizeof(err), NULL) == 0 && rt_dns_edns_size == 512);
	CHECK(rt_config_line("sysctl dns.edns_size=4097", err, sizeof(err), NULL) == -RT_EINVAL);
	CHECK(rt_config_line("sysctl dns.tcp_fallback=0", err, sizeof(err), NULL) == 0 && rt_dns_tcp == 0);
	CHECK(rt_config_line("sysctl dns.stats.tcp=1", err, sizeof(err), NULL) == -RT_EPERM);
	rt_dns_edns_size = 1232;
	rt_dns_tcp = 1;
	CHECK(rt_config_line("interface eth0 foo.device", err, sizeof(err), unknown_kw) == 0 && unknown_calls == 1);
	CHECK(rt_config_line("frobnicate", err, sizeof(err), NULL) == -RT_EINVAL && strstr(err, "frobnicate"));
	CHECK(rt_config_line("frob", err, sizeof(err), unknown_kw) == -RT_EINVAL && !strcmp(err, "bad frob"));
	CHECK(rt_config_line("route add default 10.99.0.1", err, sizeof(err), NULL) == -RT_ENETUNREACH &&
	      strstr(err, "not on a connected"));
	CHECK(rt_config_line("route add 10.0.0.0/33 1.2.3.4", err, sizeof(err), NULL) == -RT_EINVAL);
	CHECK(rt_config_line("route frob x y", err, sizeof(err), NULL) == -RT_EINVAL);
	CHECK(rt_config_line("route del 10.5.0.0/16", err, sizeof(err), NULL) < 0 && strstr(err, "no such"));
	CHECK(rt_config_line("ifconfig nosuch0 1.2.3.4", err, sizeof(err), NULL) == -RT_ENXIO);
	CHECK(rt_config_line("dhcp nosuch0", err, sizeof(err), NULL) == -RT_ENXIO);
	rt_dns_clear_servers();
	rt_hosts_clear();
	rt_unlock();
}

static void test_netdb(void)
{
	const struct rt_servent_info *s;
	const struct rt_protoent_info *p;

	s = rt_getservbyname("http", "tcp");
	CHECK(s && s->port == 80);
	s = rt_getservbyname("www", NULL);
	CHECK(s && s->port == 80);
	s = rt_getservbyname("domain", "udp");
	CHECK(s && s->port == 53 && !strcmp(s->proto, "udp"));
	CHECK(rt_getservbyname("bootpc", "tcp") == NULL);
	CHECK(rt_getservbyname("nosuch", NULL) == NULL);
	s = rt_getservbyport(22, "tcp");
	CHECK(s && !strcmp(s->name, "ssh"));
	s = rt_getservbyport(68, NULL);
	CHECK(s && !strcmp(s->name, "bootpc"));
	{
		static const char *const need[] = { "echo", "discard", "daytime", "ftp-data", "ftp", "ssh",
			"telnet", "smtp", "time", "domain", "bootps", "bootpc", "tftp", "gopher", "finger",
			"http", "pop3", "sunrpc", "auth", "nntp", "ntp", "imap", "snmp", "irc", "https",
			"imaps", "pop3s", "submission", "socks" };
		unsigned i;
		for (i = 0; i < sizeof(need) / sizeof(need[0]); i++)
			CHECK(rt_getservbyname(need[i], NULL) != NULL);
	}
	p = rt_getprotobyname("udp");
	CHECK(p && p->proto == 17);
	p = rt_getprotobyname("ICMP");
	CHECK(p && p->proto == 1);
	p = rt_getprotobynumber(6);
	CHECK(p && !strcmp(p->name, "tcp"));
	CHECK(rt_getprotobynumber(255) && rt_getprotobynumber(2) && rt_getprotobynumber(0));
	CHECK(rt_getprotobynumber(200) == NULL);
	CHECK(!strcmp(rt_strerror(RT_EWOULDBLOCK), "Resource temporarily unavailable"));
	CHECK(!strcmp(rt_strerror(RT_ECONNREFUSED), "Connection refused"));
	CHECK(!strcmp(rt_strerror(RT_ENOSYS), "Function not implemented"));
	CHECK(!strcmp(rt_strerror(-RT_EPIPE), "Broken pipe"));
	CHECK(!strcmp(rt_strerror(9999), "Unknown error"));
}

int main(void)
{
	rt_lock();
	rt_sysctl_init();
	rt_resolv_init();
	rt_unlock();
	test_build();
	test_parse();
	test_negttl();
	test_edns();
	test_fuzz();
	test_local_resolve();
	test_config();
	test_netdb();
	printf("t_dns_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
