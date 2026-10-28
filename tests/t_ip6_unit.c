/*
 * Riptide - unit tests of the IPv6 modules, single threaded with fake
 * links whose transmissions are captured (no stack thread; the clock is
 * moved forward with rt_host_advance_clock()):
 *   - address text forms (RFC 4291 / RFC 5952) and helpers
 *   - interface bring-up: link-local address, DAD (nonce), MLDv2, RS
 *   - router advertisements: default route, prefix/SLAAC, MTU, hop limit,
 *     RDNSS/DNSSL; SLAAC lifetimes and the two-hour rule; temporaries
 *   - the neighbour cache state machine and NS/NA handling
 *   - DAD conflicts and RFC 7217 regeneration
 *   - extension headers, parameter problems, malformed packet fuzzing
 *   - reassembly (overlap, duplicates, timeout, RFC 7112), fragmentation
 *     and PMTU
 *   - source address selection and routing
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/ip6_internal.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static rt_in6_addr A(const char *s)
{
	rt_in6_addr a;

	memset(&a, 0, sizeof(a));
	if (rt_inet6_pton(s, &a) < 0)
		fprintf(stderr, "bad test address %s\n", s);
	return a;
}

static const rt_in6_addr *P(const char *s)
{
	static rt_in6_addr ring[16];
	static int i;
	rt_in6_addr *r = &ring[i++ & 15];

	*r = A(s);
	return r;
}

/* ------------------------------------------------------------------ */
static void ntop_is(const char *in, const char *want)
{
	rt_in6_addr a;
	char buf[RT_INET6_ADDRSTRLEN];

	checks++;
	if (rt_inet6_pton(in, &a) < 0 || !rt_inet6_ntop(&a, buf, sizeof(buf)) || strcmp(buf, want)) {
		failures++;
		fprintf(stderr, "FAIL ntop(%s): got '%s', want '%s'\n", in,
		        rt_inet6_pton(in, &a) < 0 ? "(parse error)" : buf, want);
	}
}

static void test_text(void)
{
	static const char *const bad[] = {
		"", ":", ":::", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7",
		"::1.2.3", "1.2.3.4", "::01.2.3.4", "g::1", "1:2:3:4:5:6:7:8::", ":1::", "1::2:",
		"::1:2:3:4:5:6:7:8", "1:2:3:4:5:6:1.2.3.4:5", "::256.1.1.1", "1:::2", " ::1", "::1 ",
		"1:2:3:4:5:6:7:8:", "fe80::1%", NULL
	};
	rt_in6_addr a, b;
	char buf[RT_INET6_ADDRSTRLEN], zone[16];
	int i, plen;

	ntop_is("2001:db8::1", "2001:db8::1");
	ntop_is("2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1");
	ntop_is("2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1");     /* first of equal runs */
	ntop_is("2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1");   /* no :: for one group */
	ntop_is("1:0:0:2:0:0:0:3", "1:0:0:2::3");                  /* longest run */
	ntop_is("2001:DB8::", "2001:db8::");
	ntop_is("::", "::");
	ntop_is("::1", "::1");
	ntop_is("1::", "1::");
	ntop_is("1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0");
	ntop_is("::2:3:4:5:6:7:8", "0:2:3:4:5:6:7:8");
	ntop_is("::ffff:192.0.2.1", "::ffff:192.0.2.1");
	ntop_is("::ffff:c000:0201", "::ffff:192.0.2.1");
	ntop_is("::192.0.2.1", "::192.0.2.1");
	ntop_is("fe80::abcd:ef01:2345:6789", "fe80::abcd:ef01:2345:6789");
	ntop_is("ff02::1:ff00:1", "ff02::1:ff00:1");
	ntop_is("0:0:0:1::", "0:0:0:1::");
	for (i = 0; bad[i]; i++) {
		checks++;
		if (rt_inet6_pton(bad[i], &a) == 0) {
			failures++;
			fprintf(stderr, "FAIL pton accepted '%s'\n", bad[i]);
		}
	}
	CHECK(rt_inet6_ntop(&rt_in6addr_loopback, buf, 3) == NULL);
	CHECK(rt_inet6_parse_prefix("fe80::1%eth0/64", &a, &plen, zone, sizeof(zone)) == 0 &&
	      plen == 64 && !strcmp(zone, "eth0") && rt_in6_is_linklocal(&a));
	CHECK(rt_inet6_parse_prefix("2001:db8::/129", &a, &plen, NULL, 0) < 0);
	CHECK(rt_inet6_parse_prefix("2001:db8::", &a, &plen, NULL, 0) == 0 && plen == -1);
	CHECK(rt_inet6_parse_prefix("fe80::1%averyveryverylongzonename", &a, &plen, zone, sizeof(zone)) < 0);
	/* random round trips, with runs of zero groups */
	srand(7);
	for (i = 0; i < 20000; i++) {
		int k;
		for (k = 0; k < 8; k++)
			a.h[k] = (rand() % 3) ? 0 : (u16)rand();
		if (!rt_inet6_ntop(&a, buf, sizeof(buf)) || rt_inet6_pton(buf, &b) < 0 || !rt_in6_eq(&a, &b)) {
			failures++;
			fprintf(stderr, "FAIL round trip %s\n", buf);
			break;
		}
	}
	checks++;
	/* helpers */
	a = A("2001:db8:ffff::1");
	b = A("2001:db8:fff0::1");
	CHECK(rt_in6_common_prefix(&a, &b) == 44 && rt_in6_prefix_eq(&a, &b, 44) && !rt_in6_prefix_eq(&a, &b, 45));
	rt_in6_mask(&b, &a, 36);
	CHECK(rt_in6_eq(&b, &(rt_in6_addr){ .b = { 0x20, 1, 0xd, 0xb8, 0xf0 } }));
	rt_in6_solnode(&b, P("2001:db8::12:3456"));
	CHECK(rt_in6_eq(&b, &(rt_in6_addr){ .b = { 0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0x12, 0x34, 0x56 } }));
	{
		u8 mac[6];
		rt_in6_mc_mac(mac, &b);
		CHECK(mac[0] == 0x33 && mac[1] == 0x33 && mac[2] == 0xff && mac[5] == 0x56);
	}
	CHECK(rt_in6_scope(P("fe80::1")) == RT_IN6_SCOPE_LINK && rt_in6_scope(P("ff05::2")) == 5 &&
	      rt_in6_scope(P("2001:db8::1")) == RT_IN6_SCOPE_GLOBAL &&
	      rt_in6_scope(P("::ffff:127.0.0.1")) == RT_IN6_SCOPE_LINK &&
	      rt_in6_scope(P("::ffff:10.0.0.1")) == RT_IN6_SCOPE_GLOBAL);
}

/* ---- fake Ethernet links with captured output ---- */
struct cap {
	struct rt_bufq q;
	u8 mcast_joined[16][6];
	int nmcast;
};
struct capcb {
	u8 dst[6];
	u8 has_dst;
	u16 ethertype;
};

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	struct cap *c = nif->driver;
	struct capcb *cb = (struct capcb *)(void *)b->cb.b;

	memset(cb, 0, sizeof(*cb));
	if (dsthw) {
		memcpy(cb->dst, dsthw, 6);
		cb->has_dst = 1;
	}
	cb->ethertype = et;
	rt_bufq_tail(&c->q, b);
	return 0;
}

static int fake_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct cap *c = nif->driver;
	int i;

	for (i = 0; i < c->nmcast; i++)
		if (!memcmp(c->mcast_joined[i], hw, 6))
			break;
	if (join && i == c->nmcast && c->nmcast < 16)
		memcpy(c->mcast_joined[c->nmcast++], hw, 6);
	else if (!join && i < c->nmcast)
		memcpy(c->mcast_joined[i], c->mcast_joined[--c->nmcast], 6);
	return 0;
}

static struct rt_netif eth0, eth1, tun0;
static struct cap eth0_cap, eth1_cap, tun0_cap;
static const u8 eth0_mac[6] = { 0x02, 0, 0, 0, 0, 0x10 };
static const u8 eth1_mac[6] = { 0x02, 0, 0, 0, 0, 0x11 };
static const u8 peer_mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const u8 rtr_mac[6] = { 0x02, 0, 0, 0, 0, 0xfe };

static void make_if(struct rt_netif *n, struct cap *c, const char *name, const u8 *mac, int type)
{
	memset(n, 0, sizeof(*n));
	rt_strlcpy(n->name, name, sizeof(n->name));
	n->type = (u8)type;
	if (mac) {
		n->hwlen = 6;
		memcpy(n->hwaddr, mac, 6);
		n->flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST;
	} else {
		n->flags = RT_IFF_POINTOPOINT;
	}
	n->mtu = 1500;
	n->link_output = fake_output;
	n->mcast = mac ? fake_mcast : NULL;
	n->driver = c;
	memset(c, 0, sizeof(*c));
	rt_bufq_init(&c->q);
	CHECK(rt_netif_add(n) == 0);
}

static void flush(struct cap *c)
{
	rt_bufq_purge(&c->q);
}

/* advance the clock in small steps, running timers and loopback */
static void advance(u32 ms)
{
	while (ms) {
		u32 step = ms > 50 ? 50 : ms;
		rt_host_advance_clock(step);
		rt_lock();
		rt_stack_poll();
		rt_unlock();
		ms -= step;
	}
	rt_lock();
	rt_stack_poll();
	rt_unlock();
}

/* first captured ICMPv6 message of type t (any when t == 0); returns the
 * buffer (caller frees) and sets *m to the message */
static struct rt_buf *take(struct cap *c, u8 t, const u8 **m, u32 *mlen)
{
	struct rt_buf *b;

	for (b = c->q.head; b; b = b->next) {
		u32 off;
		u8 nxt;
		int o;
		if (b->len < 40 || (b->data[0] >> 4) != 6)
			continue;
		o = ip6_skip_exthdr(b->data, b->len, &nxt);
		if (o < 0 || nxt != RT_IPPROTO_ICMPV6)
			continue;
		off = (u32)o;
		if (t && b->data[off] != t)
			continue;
		rt_bufq_unlink(&c->q, b);
		if (m)
			*m = b->data + off;
		if (mlen)
			*mlen = b->len - off;
		return b;
	}
	return NULL;
}

static int count_type(struct cap *c, u8 t)
{
	struct rt_buf *b;
	int n = 0;

	for (b = c->q.head; b; b = b->next) {
		u8 nxt;
		int o = b->len >= 40 ? ip6_skip_exthdr(b->data, b->len, &nxt) : -1;
		if (o >= 0 && nxt == RT_IPPROTO_ICMPV6 && b->data[o] == t)
			n++;
	}
	return n;
}

static struct rt_buf *mkpkt(const rt_in6_addr *src, const rt_in6_addr *dst, u8 nxt, u8 hlim,
                            const void *payload, u32 len)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 40 + len);
	struct rt_ip6hdr *ip6;

	ip6 = (struct rt_ip6hdr *)rt_buf_put(b, 40);
	ip6->vtcfl = rt_htonl(0x60000000u);
	ip6->plen = rt_htons((u16)len);
	ip6->nxt = nxt;
	ip6->hlim = hlim;
	ip6->src = *src;
	ip6->dst = *dst;
	memcpy(rt_buf_put(b, len), payload, len);
	return b;
}

/* ICMPv6 message with checksum, optionally behind extension headers */
static struct rt_buf *mkicmp(const rt_in6_addr *src, const rt_in6_addr *dst, u8 hlim, const u8 *msg, u32 len)
{
	u8 tmp[1500];
	u16 ck;

	memcpy(tmp, msg, len);
	tmp[2] = tmp[3] = 0;
	ck = rt_csum_fold(rt_csum_partial(tmp, len, rt_csum_pseudo6(src, dst, RT_IPPROTO_ICMPV6, len)));
	memcpy(tmp + 2, &ck, 2);
	return mkpkt(src, dst, RT_IPPROTO_ICMPV6, hlim, tmp, len);
}

static void deliver(struct rt_netif *n, struct rt_buf *b, const u8 *src_mac)
{
	const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
	u16 fl = rt_in6_is_mcast(&ip6->dst) ? RT_BUFF_MCAST : 0;

	rt_lock();
	rt_netif_input(n, b, RT_ETH_P_IPV6, src_mac, fl);
	rt_stack_poll();
	rt_unlock();
}

static struct in6_ifaddr *ifa_of(struct rt_netif *n, int linklocal, int autoconf)
{
	struct ip6_if *ifp = ip6_ifp(n);
	struct in6_ifaddr *ia;

	for (ia = ifp ? ifp->addrs : NULL; ia; ia = ia->next) {
		if (rt_in6_is_linklocal(&ia->addr) != linklocal)
			continue;
		if (autoconf >= 0 && !!(ia->flags & RT_IN6_IFF_AUTOCONF) != autoconf)
			continue;
		if (ia->flags & RT_IN6_IFF_TEMPORARY)
			continue;
		return ia;
	}
	return NULL;
}

static int nd_state(struct rt_netif *n, const rt_in6_addr *a, u8 *hw)
{
	struct rt_nd6_info info[32];
	int cnt, i;

	rt_lock();
	cnt = rt_nd6_list(info, 32);
	rt_unlock();
	for (i = 0; i < cnt && i < 32; i++)
		if (info[i].ifindex == n->index && rt_in6_eq(&info[i].addr, a)) {
			if (hw)
				memcpy(hw, info[i].hw, 6);
			return info[i].state;
		}
	return -1;
}

/* ------------------------------------------------------------------ */
static rt_in6_addr ll0;        /* eth0's link-local */
static const rt_in6_addr peer_ll = { { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
static const rt_in6_addr rtr_ll = { { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfe } };

static void test_bringup(void)
{
	struct in6_ifaddr *ia;
	struct rt_buf *b;
	const u8 *m;
	u32 len;
	rt_in6_addr sol, first;
	u8 mac[6];
	int i, found;

	make_if(&eth0, &eth0_cap, "eth0", eth0_mac, RT_IFT_ETHER);
	rt_lock();
	rt_netif_up(&eth0);
	rt_unlock();
	CHECK(rt_ip6_ifenabled(&eth0));
	ia = ifa_of(&eth0, 1, -1);
	CHECK(ia && (ia->flags & RT_IN6_IFF_TENTATIVE) && ia->plen == 64);
	if (!ia)
		return;
	ll0 = ia->addr;
	first = ll0;
	/* RFC 7217 identifiers do not embed the MAC (EUI-64 would be ...ff:fe...) */
	CHECK(!(ll0.b[11] == 0xff && ll0.b[12] == 0xfe));
	/* joined all-nodes and our solicited-node group in the link filter */
	rt_in6_solnode(&sol, &ll0);
	rt_in6_mc_mac(mac, &sol);
	for (i = 0, found = 0; i < eth0_cap.nmcast; i++)
		found |= !memcmp(eth0_cap.mcast_joined[i], mac, 6);
	CHECK(found);
	CHECK(rt_ip6_mc_joined(&eth0, P("ff02::1")) && rt_ip6_mc_joined(&eth0, &sol));
	/* DAD probe within a second: from ::, to the solicited-node group, nonce */
	advance(1100);
	b = take(&eth0_cap, RT_ND_NEIGHBOR_SOLICIT, &m, &len);
	CHECK(b != NULL);
	if (b) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		CHECK(rt_in6_is_unspec(&ip6->src) && rt_in6_eq(&ip6->dst, &sol) && ip6->hlim == 255);
		CHECK(len >= 32 && !memcmp(m + 8, ll0.b, 16) && m[24] == 14 /* nonce */);
		CHECK(((struct capcb *)(void *)b->cb.b)->dst[0] == 0x33);
		rt_buf_free(b);
	}
	/* MLDv2 report with router alert */
	b = take(&eth0_cap, RT_ICMP6_MLDV2_REPORT, &m, &len);
	CHECK(b != NULL);
	if (b) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		CHECK(ip6->nxt == RT_IPPROTO_HOPOPTS && b->data[42] == 5 && ip6->hlim == 1 &&
		      rt_in6_eq(&ip6->dst, P("ff02::16")));
		CHECK(rt_get_be16(m + 6) >= 1 && m[8] == 4 && !memcmp(m + 12, sol.b, 16));
		rt_buf_free(b);
	}
	advance(1500);
	CHECK(!(ia->flags & RT_IN6_IFF_TENTATIVE));
	/* router solicitation after a random delay of up to a second */
	advance(1200);
	b = take(&eth0_cap, RT_ND_ROUTER_SOLICIT, &m, &len);
	CHECK(b != NULL);
	if (b) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		CHECK(rt_in6_eq(&ip6->src, &ll0) && rt_in6_eq(&ip6->dst, P("ff02::2")) &&
		      len == 16 && m[8] == 1 && !memcmp(m + 10, eth0_mac, 6));
		rt_buf_free(b);
	}
	/* three solicitations, 4 s apart, then silence */
	advance(9000);
	CHECK(count_type(&eth0_cap, RT_ND_ROUTER_SOLICIT) == 2);
	flush(&eth0_cap);
	advance(10000);
	CHECK(count_type(&eth0_cap, RT_ND_ROUTER_SOLICIT) == 0);
	flush(&eth0_cap);
	/* the identifier is stable across down/up */
	rt_lock();
	rt_ip6_ifenable(&eth0, 0);
	CHECK(!rt_ip6_ifenabled(&eth0) && eth0.ip6_disabled);
	rt_ip6_ifenable(&eth0, 1);
	rt_unlock();
	ia = ifa_of(&eth0, 1, -1);
	CHECK(ia && rt_in6_eq(&ia->addr, &first));
	advance(3000);
	CHECK(ia && !(ia->flags & RT_IN6_IFF_TENTATIVE));
	flush(&eth0_cap);
	/* EUI-64 mode */
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.ip6.addr_gen_mode", "0") == 0);
	make_if(&eth1, &eth1_cap, "eth1", eth1_mac, RT_IFT_ETHER);
	rt_netif_up(&eth1);
	rt_unlock();
	ia = ifa_of(&eth1, 1, -1);
	CHECK(ia && rt_in6_eq(&ia->addr, P("fe80::ff:fe00:11")));
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.ip6.addr_gen_mode", "1") == 0);
	rt_unlock();
	advance(3000);
	flush(&eth1_cap);
}

static u32 build_ra(u8 *m, u16 lifetime, u8 hlim, u8 flags, const char *prefix, u32 valid, u32 pref,
                    u32 mtu, int rdnss)
{
	u32 n = 16;

	memset(m, 0, 200);
	m[0] = RT_ND_ROUTER_ADVERT;
	m[4] = hlim;
	m[5] = flags;
	rt_put_be16(m + 6, lifetime);
	/* SLLA */
	m[n] = 1; m[n + 1] = 1; memcpy(m + n + 2, rtr_mac, 6); n += 8;
	if (prefix) {
		rt_in6_addr p = A(prefix);
		m[n] = 3; m[n + 1] = 4; m[n + 2] = 64; m[n + 3] = 0xc0;   /* L, A */
		rt_put_be32(m + n + 4, valid);
		rt_put_be32(m + n + 8, pref);
		memcpy(m + n + 16, p.b, 16);
		n += 32;
	}
	if (mtu) {
		m[n] = 5; m[n + 1] = 1;
		rt_put_be32(m + n + 4, mtu);
		n += 8;
	}
	if (rdnss) {
		rt_in6_addr d = A("2001:db8::53");
		m[n] = 25; m[n + 1] = 3;
		rt_put_be32(m + n + 4, 60);
		memcpy(m + n + 8, d.b, 16);
		n += 24;
		/* DNSSL "example.org" */
		m[n] = 31; m[n + 1] = 3;
		rt_put_be32(m + n + 4, 60);
		memcpy(m + n + 8, "\007example\003org", 13);
		n += 24;
	}
	return n;
}

static void test_ra_slaac(void)
{
	u8 m[200];
	u32 n;
	struct rt_nexthop6 nh;
	struct in6_ifaddr *ia;
	rt_in6_addr servers[8], expect;
	int cnt, i, found;

	n = build_ra(m, 1800, 50, 0, "2001:db8:1::", 30, 20, 1400, 1);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	rt_lock();
	CHECK(rt_ip6_route(P("2001:db8:77::1"), NULL, &nh) == 0 && nh.netif == &eth0 &&
	      rt_in6_eq(&nh.nexthop, &rtr_ll) && (nh.flags & RT_RTF_GATEWAY) && nh.mtu == 1400);
	CHECK(rt_ip6_route(P("2001:db8:1::99"), NULL, &nh) == 0 && rt_in6_eq(&nh.nexthop, P("2001:db8:1::99")));
	CHECK(ip6_default_hlim(&eth0) == 50 && rt_ip6_link_mtu(&eth0) == 1400);
	cnt = rt_dns_get_servers6(servers, NULL, NULL, 8);
	expect = A("2001:db8::53");
	for (i = 0, found = 0; i < cnt; i++)
		found |= rt_in6_eq(&servers[i], &expect);
	CHECK(found);
	rt_unlock();
	CHECK(nd_state(&eth0, &rtr_ll, NULL) == RT_NUD_STALE);
	ia = ifa_of(&eth0, 0, 1);
	CHECK(ia && rt_in6_prefix_eq(&ia->addr, P("2001:db8:1::"), 64) && (ia->flags & RT_IN6_IFF_TENTATIVE));
	advance(2500);
	CHECK(ia && !(ia->flags & RT_IN6_IFF_TENTATIVE));
	/* hop limit 255 only: the same RA forwarded is ignored */
	n = build_ra(m, 1800, 9, 0, "2001:db8:9::", 30, 20, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 254, m, n), rtr_mac);
	CHECK(ip6_default_hlim(&eth0) == 50 && !route6_onlink(P("2001:db8:9::1")));
	/* lifetimes: deprecated after 20 s, gone after 30 s */
	advance(21000);
	CHECK(ia->flags & RT_IN6_IFF_DEPRECATED);
	advance(10000);
	CHECK(ifa_of(&eth0, 0, 1) == NULL);
	CHECK(!route6_onlink(P("2001:db8:1::99")));
	/* RFC 4862 5.5.3 e): a short valid lifetime cannot cut a long one below 2 h */
	n = build_ra(m, 1800, 0, 0, "2001:db8:2::", 100000, 90000, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	ia = ifa_of(&eth0, 0, 1);
	CHECK(ia != NULL);
	n = build_ra(m, 1800, 0, 0, "2001:db8:2::", 60, 30, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	{
		struct rt_in6_ifaddr_info info[16];
		int k, cnt2;
		rt_lock();
		cnt2 = rt_in6_addr_list(&eth0, info, 16);
		rt_unlock();
		for (k = 0, found = 0; k < cnt2 && k < 16; k++)
			if (rt_in6_prefix_eq(&info[k].addr, P("2001:db8:2::"), 64))
				found = info[k].valid_s >= 7199 && info[k].valid_s <= 7200 && info[k].pref_s == 30;
		CHECK(found);
	}
	/* temporary addresses (use_tempaddr=2: also preferred as source) */
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.ip6.use_tempaddr", "2") == 0);
	rt_unlock();
	n = build_ra(m, 1800, 0, 0, "2001:db8:2::", 100000, 90000, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	advance(2500);
	{
		struct ip6_if *ifp = ip6_ifp(&eth0);
		struct in6_ifaddr *t;
		rt_in6_addr src;
		for (t = ifp->addrs; t; t = t->next)
			if (t->flags & RT_IN6_IFF_TEMPORARY)
				break;
		CHECK(t && rt_in6_prefix_eq(&t->addr, P("2001:db8:2::"), 64) && !(t->flags & RT_IN6_IFF_TENTATIVE));
		rt_lock();
		CHECK(rt_ip6_select_source(P("2001:db8:99::1"), &eth0, &src) == 0 && t && rt_in6_eq(&src, &t->addr));
		CHECK(rt_sysctl_set_text("net.inet6.ip6.use_tempaddr", "0") == 0);
		CHECK(rt_ip6_select_source(P("2001:db8:99::1"), &eth0, &src) == 0 && rt_in6_eq(&src, &ia->addr));
		rt_unlock();
	}
	/* router lifetime 0 removes the default route */
	n = build_ra(m, 0, 0, 0, NULL, 0, 0, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	rt_lock();
	CHECK(rt_ip6_route(P("2001:db8:77::1"), NULL, &nh) == -RT_ENETUNREACH);
	rt_unlock();
	n = build_ra(m, 1800, 0, 0, NULL, 0, 0, 0, 0);
	deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	flush(&eth0_cap);
}

static void test_neighbours(void)
{
	static const u8 payload[8] = { 0x80, 0, 0, 0, 0, 1, 0, 1 };
	struct rt_ip6_out o;
	struct rt_buf *b;
	const u8 *m;
	u32 len;
	u8 na[32], hw[6];
	rt_in6_addr dst = A("2001:db8:2::99"), sol;
	struct in6_ifaddr *ia = ifa_of(&eth0, 0, 1);

	CHECK(ia != NULL);
	if (!ia)
		return;
	memset(&o, 0, sizeof(o));
	o.dst = dst;
	o.nxt = RT_IPPROTO_NONE;
	o.hlim = -1;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 8);
	memcpy(rt_buf_put(b, 8), payload, 8);
	rt_lock();
	CHECK(rt_ip6_output(b, &o) == 0);
	rt_unlock();
	CHECK(nd_state(&eth0, &dst, NULL) == RT_NUD_INCOMPLETE);
	b = take(&eth0_cap, RT_ND_NEIGHBOR_SOLICIT, &m, &len);
	CHECK(b != NULL);
	if (b) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		rt_in6_solnode(&sol, &dst);
		/* the source of the waiting packet is the NS's source (RFC 4861 7.2.2) */
		CHECK(rt_in6_eq(&ip6->dst, &sol) && rt_in6_eq(&ip6->src, &ia->addr) && !memcmp(m + 8, dst.b, 16) &&
		      m[24] == 1 && !memcmp(m + 26, eth0_mac, 6));
		rt_buf_free(b);
	}
	CHECK(eth0_cap.q.qlen == 0);           /* the packet waits */
	/* solicited NA: the queued packet goes out, entry REACHABLE */
	memset(na, 0, sizeof(na));
	na[0] = RT_ND_NEIGHBOR_ADVERT;
	na[4] = 0x60;                          /* S, O */
	memcpy(na + 8, dst.b, 16);
	na[24] = 2; na[25] = 1; memcpy(na + 26, peer_mac, 6);
	deliver(&eth0, mkicmp(&dst, &ia->addr, 255, na, 32), peer_mac);
	CHECK(nd_state(&eth0, &dst, hw) == RT_NUD_REACHABLE && !memcmp(hw, peer_mac, 6));
	CHECK(eth0_cap.q.qlen == 1 && eth0_cap.q.head &&
	      !memcmp(((struct capcb *)(void *)eth0_cap.q.head->cb.b)->dst, peer_mac, 6) &&
	      eth0_cap.q.head->data[6] == RT_IPPROTO_NONE);
	flush(&eth0_cap);
	/* REACHABLE -> STALE after ReachableTime (15..45 s) */
	advance(46000);
	CHECK(nd_state(&eth0, &dst, NULL) == RT_NUD_STALE);
	flush(&eth0_cap);
	/* use: DELAY, then PROBE with unicast solicitations */
	b = rt_buf_alloc(RT_BUF_HEADROOM, 8);
	memcpy(rt_buf_put(b, 8), payload, 8);
	rt_lock();
	CHECK(rt_ip6_output(b, &o) == 0);
	rt_unlock();
	CHECK(nd_state(&eth0, &dst, NULL) == RT_NUD_DELAY && eth0_cap.q.qlen == 1);
	flush(&eth0_cap);
	advance(5100);
	CHECK(nd_state(&eth0, &dst, NULL) == RT_NUD_PROBE);
	b = take(&eth0_cap, RT_ND_NEIGHBOR_SOLICIT, &m, &len);
	CHECK(b && !memcmp(((struct capcb *)(void *)b->cb.b)->dst, peer_mac, 6) &&
	      rt_in6_eq(&((const struct rt_ip6hdr *)b->data)->dst, &dst));
	rt_buf_free(b);
	/* an unsolicited NA (S=0, same address) does not confirm */
	na[4] = 0x20;
	deliver(&eth0, mkicmp(&dst, P("ff02::1"), 255, na, 32), peer_mac);
	CHECK(nd_state(&eth0, &dst, NULL) == RT_NUD_PROBE);
	/* no answer: gone after three probes */
	advance(3500);
	CHECK(nd_state(&eth0, &dst, NULL) == -1);
	flush(&eth0_cap);

	/* NS from a neighbour for our address: learned STALE, answered unicast */
	{
		u8 ns[32];
		const rt_in6_addr nb = A("2001:db8:2::42");
		memset(ns, 0, sizeof(ns));
		ns[0] = RT_ND_NEIGHBOR_SOLICIT;
		memcpy(ns + 8, ia->addr.b, 16);
		ns[24] = 1; ns[25] = 1; memcpy(ns + 26, peer_mac, 6);
		rt_in6_solnode(&sol, &ia->addr);
		deliver(&eth0, mkicmp(&nb, &sol, 255, ns, 32), peer_mac);
		CHECK(nd_state(&eth0, &nb, NULL) == RT_NUD_STALE);
		b = take(&eth0_cap, RT_ND_NEIGHBOR_ADVERT, &m, &len);
		CHECK(b && (m[4] & 0x60) == 0x60 && !memcmp(m + 8, ia->addr.b, 16) && m[24] == 2 &&
		      !memcmp(m + 26, eth0_mac, 6) && rt_in6_eq(&((const struct rt_ip6hdr *)b->data)->dst, &nb) &&
		      !memcmp(((struct capcb *)(void *)b->cb.b)->dst, peer_mac, 6));
		rt_buf_free(b);
		/* an NS with a zero length option is discarded */
		ns[25] = 0;
		deliver(&eth0, mkicmp(&nb, &sol, 255, ns, 32), peer_mac);
		CHECK(count_type(&eth0_cap, RT_ND_NEIGHBOR_ADVERT) == 0);
	}
	/* a router's NA without the R flag drops its default route */
	{
		struct rt_nexthop6 nh;
		memset(na, 0, sizeof(na));
		na[0] = RT_ND_NEIGHBOR_ADVERT;
		na[4] = 0x20;
		memcpy(na + 8, rtr_ll.b, 16);
		na[24] = 2; na[25] = 1; memcpy(na + 26, rtr_mac, 6);
		rt_lock();
		CHECK(rt_ip6_route(P("2001:db8:77::1"), NULL, &nh) == 0);
		rt_unlock();
		deliver(&eth0, mkicmp(&rtr_ll, P("ff02::1"), 255, na, 32), rtr_mac);
		rt_lock();
		CHECK(rt_ip6_route(P("2001:db8:77::1"), NULL, &nh) == -RT_ENETUNREACH);
		rt_unlock();
	}
	flush(&eth0_cap);
}

static void test_dad_conflict(void)
{
	struct in6_ifaddr *ia;
	u8 na[32], m[200];
	u32 n;
	rt_in6_addr manual = A("2001:db8:5::5"), old;

	/* a manual address and an NA claiming it during DAD */
	rt_lock();
	CHECK(rt_in6_addr_add(&eth1, &manual, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	CHECK(rt_in6_addr_add(&eth1, &manual, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == -RT_EEXIST);
	rt_unlock();
	ia = in6_ifa_find(&eth1, &manual);
	CHECK(ia && (ia->flags & RT_IN6_IFF_TENTATIVE));
	memset(na, 0, sizeof(na));
	na[0] = RT_ND_NEIGHBOR_ADVERT;
	na[4] = 0x20;
	memcpy(na + 8, manual.b, 16);
	na[24] = 2; na[25] = 1; memcpy(na + 26, peer_mac, 6);
	deliver(&eth1, mkicmp(&peer_ll, P("ff02::1"), 255, na, 32), peer_mac);
	CHECK(ia && (ia->flags & RT_IN6_IFF_DUPLICATED));
	/* a duplicated address is not used */
	{
		rt_in6_addr src;
		rt_lock();
		CHECK(rt_ip6_select_source(P("2001:db8:5::1"), &eth1, &src) == 0 && !rt_in6_eq(&src, &manual));
		CHECK(rt_in6_addr_del(&eth1, &manual) == 0 && rt_in6_addr_del(&eth1, &manual) == -RT_EADDRNOTAVAIL);
		rt_unlock();
	}
	/* SLAAC address: another node's DAD probe (from ::) during our DAD
	 * makes us pick the next RFC 7217 identifier */
	n = build_ra(m, 1800, 0, 0, "2001:db8:6::", 3600, 3600, 0, 0);
	deliver(&eth1, mkicmp(&rtr_ll, P("ff02::1"), 255, m, n), rtr_mac);
	ia = ifa_of(&eth1, 0, 1);
	CHECK(ia && (ia->flags & RT_IN6_IFF_TENTATIVE));
	if (ia) {
		u8 ns[32];
		rt_in6_addr sol;
		old = ia->addr;
		memset(ns, 0, sizeof(ns));
		ns[0] = RT_ND_NEIGHBOR_SOLICIT;
		memcpy(ns + 8, old.b, 16);
		rt_in6_solnode(&sol, &old);
		deliver(&eth1, mkicmp(&rt_in6addr_any, &sol, 255, ns, 24), peer_mac);
		CHECK(!rt_in6_eq(&ia->addr, &old) && (ia->flags & RT_IN6_IFF_TENTATIVE) && ia->dad_counter == 1 &&
		      rt_in6_prefix_eq(&ia->addr, &old, 64));
		/* our own probe looped back (same nonce) is not a conflict */
		advance(1100);
		{
			struct rt_buf *b;
			const u8 *msg;
			u32 len;
			b = take(&eth1_cap, RT_ND_NEIGHBOR_SOLICIT, &msg, &len);
			CHECK(b && len >= 32);
			if (b) {
				rt_lock();
				rt_netif_input(&eth1, b, RT_ETH_P_IPV6, eth1_mac, RT_BUFF_MCAST);
				rt_unlock();
			}
		}
		CHECK(!(ia->flags & RT_IN6_IFF_DUPLICATED));
		advance(1500);
		CHECK(!(ia->flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED)));
	}
	flush(&eth1_cap);
}

/* ---- RFC 9131 gratuitous neighbour advertisements ---- */
/* Takes every captured NA; counts those for target sent to ff02::2 with
 * R/S/O clear and our link-layer address as the TLLA option. Returns -1
 * if one is malformed or an announcement names a link-local target. */
static int take_unas(struct cap *c, const struct rt_netif *n, const rt_in6_addr *target)
{
	struct rt_buf *b;
	const u8 *m;
	u32 len;
	int cnt = 0;

	while ((b = take(c, RT_ND_NEIGHBOR_ADVERT, &m, &len)) != NULL) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		struct capcb *cb = (struct capcb *)(void *)b->cb.b;
		rt_in6_addr t;
		memcpy(t.b, m + 8, 16);
		if (rt_in6_eq(&ip6->dst, P("ff02::2"))) {
			static const u8 mc2[6] = { 0x33, 0x33, 0, 0, 0, 2 };
			if (len < 32 || m[4] != 0 || m[24] != 2 || m[25] != 1 || memcmp(m + 26, n->hwaddr, 6) ||
			    ip6->hlim != 255 || !rt_in6_eq(&ip6->src, &t) || rt_in6_is_linklocal(&t) ||
			    !cb->has_dst || memcmp(cb->dst, mc2, 6))
				cnt = -1000;
			else if (rt_in6_eq(&t, target))
				cnt++;
		}
		rt_buf_free(b);
	}
	return cnt < 0 ? -1 : cnt;
}

static void test_gratuitous_na(void)
{
	rt_in6_addr a = A("2001:db8:7::7"), d = A("2001:db8:7::8"), q = A("2001:db8:7::9");
	struct in6_ifaddr *ia;
	u32 before;

	advance(3000);                  /* earlier tests' announcements end */
	flush(&eth1_cap);
	before = rt_icmp6stat.OutUnsolicitedNAs;
	rt_lock();
	CHECK(rt_in6_addr_add(&eth1, &a, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	rt_unlock();
	ia = in6_ifa_find(&eth1, &a);
	CHECK(ia && (ia->flags & RT_IN6_IFF_TENTATIVE));
	/* nothing while tentative (DAD probe within 1 s, then RetransTimer) */
	advance(900);
	CHECK(ia && (ia->flags & RT_IN6_IFF_TENTATIVE));
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 0);
	/* preferred: the first at once, three in all, a second apart */
	advance(1200);
	CHECK(ia && !(ia->flags & RT_IN6_IFF_TENTATIVE));
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 1);
	advance(1000);
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 1);
	advance(1000);
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 1);
	advance(5000);
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 0);
	CHECK(rt_icmp6stat.OutUnsolicitedNAs - before == 3);
	/* a deprecated address is not announced */
	rt_lock();
	CHECK(rt_in6_addr_add(&eth1, &d, 64, 0, 600, 0) == 0);
	rt_unlock();
	advance(5000);
	CHECK(take_unas(&eth1_cap, &eth1, &d) == 0);
	/* the sysctl turns it off */
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.ip6.gratuitous_na", "0") == 0);
	CHECK(rt_in6_addr_add(&eth1, &q, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	rt_unlock();
	advance(5000);
	CHECK(take_unas(&eth1_cap, &eth1, &q) == 0);
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.ip6.gratuitous_na", "1") == 0);
	/* removing an address mid-announcement stops it */
	CHECK(rt_in6_addr_del(&eth1, &q) == 0);
	CHECK(rt_in6_addr_add(&eth1, &q, 64, RT_IN6_IFF_NODAD, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	CHECK(rt_in6_addr_del(&eth1, &q) == 0);
	CHECK(rt_in6_addr_del(&eth1, &a) == 0 && rt_in6_addr_del(&eth1, &d) == 0);
	rt_unlock();
	advance(3000);
	CHECK(take_unas(&eth1_cap, &eth1, &q) == 1);
	/* link down and up: global addresses are announced again after DAD,
	 * the link-local one never (take_unas fails on a link-local target) */
	rt_lock();
	CHECK(rt_in6_addr_add(&eth1, &a, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	rt_unlock();
	advance(5000);
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 3);
	rt_lock();
	rt_netif_down(&eth1);
	rt_netif_up(&eth1);
	rt_unlock();
	advance(6000);
	CHECK(take_unas(&eth1_cap, &eth1, &a) == 3);
	rt_lock();
	CHECK(rt_in6_addr_del(&eth1, &a) == 0);
	rt_unlock();
	flush(&eth1_cap);
}

/* ---- extension headers ---- */
static rt_in6_addr gl0;      /* eth0's global SLAAC address */

static struct rt_buf *take_pp(u8 code, u32 *ptr)
{
	const u8 *m;
	u32 len;
	struct rt_buf *b = take(&eth0_cap, RT_ICMP6_PARAM_PROB, &m, &len);

	if (!b)
		return NULL;
	if (m[1] != code) {
		rt_buf_free(b);
		return NULL;
	}
	*ptr = rt_get_be32(m + 4);
	return b;
}

static void test_exthdr(void)
{
	u8 p[256];
	struct rt_buf *b;
	u32 ptr = 0;
	const u8 *m;
	u32 len;
	static const u8 echo[16] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1, 'p', 'i', 'n', 'g', 6, 6, 6, 6 };
	u16 ck;

	gl0 = ifa_of(&eth0, 0, 1)->addr;
	/* the peer is a permanent neighbour so our answers leave at once */
	rt_lock();
	CHECK(rt_nd6_add_static(&eth0, &peer_ll, peer_mac) == 0);
	rt_unlock();
	/* echo request with a checksum over the pseudo header */
	memcpy(p, echo, 16);
	p[2] = p[3] = 0;
	ck = rt_csum_fold(rt_csum_partial(p, 16, rt_csum_pseudo6(&peer_ll, &ll0, RT_IPPROTO_ICMPV6, 16)));
	memcpy(p + 2, &ck, 2);

	/* plain echo works */
	deliver(&eth0, mkicmp(&peer_ll, &ll0, 64, echo, 16), peer_mac);
	b = take(&eth0_cap, RT_ICMP6_ECHO_REPLY, &m, &len);
	CHECK(b && len == 16 && !memcmp(m + 4, echo + 4, 12) && rt_in6_eq(&((struct rt_ip6hdr *)b->data)->src, &ll0));
	rt_buf_free(b);

	/* hop-by-hop with an unknown option, action 10: parameter problem code 2 */
	{
		u8 q[8 + 16];
		q[0] = RT_IPPROTO_ICMPV6; q[1] = 0;
		q[2] = 0x80; q[3] = 1; q[4] = 0;          /* unknown, action 10 */
		q[5] = 1; q[6] = 0;                        /* PadN */
		q[7] = 0;                                  /* Pad1 */
		memcpy(q + 8, p, 16);
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		b = take_pp(RT_ICMP6_PARAMPROB_OPTION, &ptr);
		CHECK(b && ptr == 42);
		rt_buf_free(b);
		CHECK(count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 0);
		/* action 01: silently discarded */
		q[2] = 0x40;
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		CHECK(eth0_cap.q.qlen == 0);
		/* action 00: skipped, the echo is answered */
		q[2] = 0x1e;
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		CHECK(count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 1);
		flush(&eth0_cap);
		/* action 11 to a multicast destination: no error */
		q[2] = 0xc0;
		deliver(&eth0, mkpkt(&peer_ll, P("ff02::1"), RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		CHECK(eth0_cap.q.qlen == 0);
		/* option running past the header */
		q[2] = 0x1e; q[3] = 9;
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		b = take_pp(RT_ICMP6_PARAMPROB_HEADER, &ptr);
		CHECK(b != NULL);
		rt_buf_free(b);
		flush(&eth0_cap);
	}
	/* hop-by-hop after a destination options header: code 1 at the nxt byte */
	{
		u8 q[16 + 16];
		memset(q, 0, sizeof(q));
		q[0] = RT_IPPROTO_HOPOPTS; q[1] = 0; q[2] = 1; q[3] = 4;
		q[8] = RT_IPPROTO_ICMPV6; q[9] = 0; q[10] = 1; q[11] = 4;
		memcpy(q + 16, p, 16);
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_DSTOPTS, 64, q, sizeof(q)), peer_mac);
		b = take_pp(RT_ICMP6_PARAMPROB_NEXTHEADER, &ptr);
		CHECK(b && ptr == 40);
		rt_buf_free(b);
	}
	/* routing header type 0 with segments left: code 0 at the type field */
	{
		u8 q[24 + 16];
		memset(q, 0, sizeof(q));
		q[0] = RT_IPPROTO_ICMPV6; q[1] = 2; q[2] = 0; q[3] = 1;
		memcpy(q + 24, p, 16);
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_ROUTING, 64, q, sizeof(q)), peer_mac);
		b = take_pp(RT_ICMP6_PARAMPROB_HEADER, &ptr);
		CHECK(b && ptr == 42);
		rt_buf_free(b);
		CHECK(count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 0);
		/* segments left 0: ignored, processing continues */
		q[3] = 0;
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_ROUTING, 64, q, sizeof(q)), peer_mac);
		CHECK(count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 1);
		flush(&eth0_cap);
	}
	/* unknown upper layer: code 1 pointing at the fixed header's next header */
	deliver(&eth0, mkpkt(&peer_ll, &ll0, 253, 64, p, 16), peer_mac);
	b = take_pp(RT_ICMP6_PARAMPROB_NEXTHEADER, &ptr);
	CHECK(b && ptr == 6);
	rt_buf_free(b);
	/* no next header: dropped quietly */
	deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_NONE, 64, p, 16), peer_mac);
	CHECK(eth0_cap.q.qlen == 0);
	/* never an error about an ICMPv6 error (an unknown option in front of
	 * a destination unreachable message) */
	{
		u8 q[8 + 8];
		memset(q, 0, sizeof(q));
		q[0] = RT_IPPROTO_ICMPV6; q[2] = 0x80; q[3] = 4;
		q[8] = RT_ICMP6_DST_UNREACH;
		deliver(&eth0, mkpkt(&peer_ll, &ll0, RT_IPPROTO_HOPOPTS, 64, q, sizeof(q)), peer_mac);
		CHECK(eth0_cap.q.qlen == 0);
		flush(&eth0_cap);
	}
	/* UDP to a closed port: port unreachable quoting the packet */
	{
		u8 u[16];
		u16 c;
		memset(u, 0, sizeof(u));
		rt_put_be16(u, 5555);
		rt_put_be16(u + 2, 9);
		rt_put_be16(u + 4, 16);
		c = rt_csum_fold(rt_csum_partial(u, 16, rt_csum_pseudo6(&peer_ll, &gl0, RT_IPPROTO_UDP, 16)));
		memcpy(u + 6, &c, 2);
		deliver(&eth0, mkpkt(&peer_ll, &gl0, RT_IPPROTO_UDP, 64, u, 16), peer_mac);
		b = take(&eth0_cap, RT_ICMP6_DST_UNREACH, &m, &len);
		CHECK(b && m[1] == RT_ICMP6_DST_UNREACH_NOPORT && len == 8 + 40 + 16 &&
		      rt_in6_eq(&((struct rt_ip6hdr *)b->data)->src, &gl0));
		rt_buf_free(b);
		/* zero UDP checksum over IPv6: dropped, no error */
		u[6] = u[7] = 0;
		deliver(&eth0, mkpkt(&peer_ll, &gl0, RT_IPPROTO_UDP, 64, u, 16), peer_mac);
		CHECK(eth0_cap.q.qlen == 0);
	}
	/* martians: multicast or loopback sources, mapped destinations */
	deliver(&eth0, mkicmp(P("ff02::1"), &ll0, 64, echo, 16), peer_mac);
	deliver(&eth0, mkicmp(&rt_in6addr_loopback, &ll0, 64, echo, 16), peer_mac);
	deliver(&eth0, mkicmp(&peer_ll, P("::ffff:10.0.0.1"), 64, echo, 16), peer_mac);
	deliver(&eth0, mkicmp(&peer_ll, P("2001:db8:dead::1"), 64, echo, 16), peer_mac);  /* not ours */
	CHECK(eth0_cap.q.qlen == 0);
	flush(&eth0_cap);
}

/* RFC 4443 2.4 (f): per destination token bucket, burst 6 */
static void test_ratelimit(void)
{
	u8 u[16];
	u16 c;
	int i;

	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.icmp6.ratelimit", "1000") == 0);
	rt_unlock();
	memset(u, 0, sizeof(u));
	rt_put_be16(u, 5555);
	rt_put_be16(u + 2, 9);
	rt_put_be16(u + 4, 16);
	c = rt_csum_fold(rt_csum_partial(u, 16, rt_csum_pseudo6(&peer_ll, &gl0, RT_IPPROTO_UDP, 16)));
	memcpy(u + 6, &c, 2);
	flush(&eth0_cap);
	for (i = 0; i < 20; i++)
		deliver(&eth0, mkpkt(&peer_ll, &gl0, RT_IPPROTO_UDP, 64, u, 16), peer_mac);
	CHECK(count_type(&eth0_cap, RT_ICMP6_DST_UNREACH) == 6);
	flush(&eth0_cap);
	advance(2100);
	for (i = 0; i < 20; i++)
		deliver(&eth0, mkpkt(&peer_ll, &gl0, RT_IPPROTO_UDP, 64, u, 16), peer_mac);
	CHECK(count_type(&eth0_cap, RT_ICMP6_DST_UNREACH) == 2);
	flush(&eth0_cap);
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.icmp6.ratelimit", "0") == 0);
	rt_unlock();
}

static void test_fuzz(void)
{
	static const u8 echo[16] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1, 'f', 'u', 'z', 'z', 1, 2, 3, 4 };
	u8 base[200];
	int i;
	struct rt_buf_stats st0, st1;

	rt_buf_get_stats(&st0);
	srand(42);
	for (i = 0; i < 30000; i++) {
		struct rt_buf *b;
		u32 len = 40, k;
		int kind = rand() % 6;
		struct rt_ip6hdr *ip6 = (struct rt_ip6hdr *)(void *)base;
		memset(base, 0, sizeof(base));
		ip6->vtcfl = rt_htonl(0x60000000u);
		ip6->hlim = 64;
		ip6->src = peer_ll;
		ip6->dst = (rand() & 1) ? ll0 : gl0;
		/* a chain of random extension headers, then ICMPv6 / UDP / TCP / frag */
		{
			static const u8 hdrs[] = { 0, 43, 44, 60, 58, 17, 6, 59, 51 };
			u8 *nxtp = &ip6->nxt;
			int nh = rand() % 4;
			while (nh-- > 0 && len + 16 < sizeof(base)) {
				u8 t = hdrs[rand() % RT_ARRAY_SIZE(hdrs)];
				*nxtp = t;
				if (t == 58 || t == 17 || t == 6 || t == 59)
					break;
				base[len + 1] = (u8)(rand() % 2);
				nxtp = &base[len];
				for (k = 2; k < 16; k++)
					base[len + k] = (u8)rand();
				len += (u32)(base[len + 1] + 1) * 8;
				if (len > sizeof(base) - 24)
					len = sizeof(base) - 24;
			}
			if (*nxtp == 0 && nxtp != &ip6->nxt)
				*nxtp = 58;
		}
		memcpy(base + len, echo, 16);
		len += 16;
		/* random corruption */
		for (k = 0; k < (u32)(rand() % 6); k++)
			base[rand() % len] = (u8)rand();
		if (kind == 0)
			len = (u32)(rand() % (int)len);       /* truncated */
		rt_put_be16(&base[4], (u16)(kind == 1 ? rand() : (int)(len > 40 ? len - 40 : 0)));
		b = rt_buf_alloc(RT_BUF_HEADROOM, len ? len : 1);
		memcpy(rt_buf_put(b, len), base, len);
		if (kind == 2 && len > 1) {
			/* misaligned start */
			struct rt_buf *c = rt_buf_alloc(RT_BUF_HEADROOM + 1, len);
			c->data++;
			memcpy(rt_buf_put(c, len), base, len);
			rt_buf_free(b);
			b = c;
		}
		rt_lock();
		rt_netif_input(&eth0, b, RT_ETH_P_IPV6, peer_mac, 0);
		rt_unlock();
		if (i % 64 == 0) {
			advance(10);
			flush(&eth0_cap);
			flush(&eth1_cap);
		}
	}
	advance(61000);                 /* reassembly queues time out */
	flush(&eth0_cap);
	flush(&eth1_cap);
	rt_buf_get_stats(&st1);
	CHECK(st1.allocated <= st0.allocated + 2);
	if (st1.allocated > st0.allocated + 2) {
		struct rt_buf *x;
		int nn = 0;
		for (x = eth1_cap.q.head; x && nn < 8; x = x->next, nn++) {
			u8 t; int o = ip6_skip_exthdr(x->data, x->len, &t);
			fprintf(stderr, "eth1 frame et %x len %u nxt %u type %u\n", ((struct capcb *)(void *)x->cb.b)->ethertype, x->len, t, o >= 0 ? x->data[o] : 0);
		}
		u32 q, bytes;
		void rt_ip6frag_stats(u32 *queues, u32 *bytes);
		rt_ip6frag_stats(&q, &bytes);
		fprintf(stderr, "fuzz: %u buffers before, %u after; %d neighbours, %u frag queues, lo %u\n",
		        st0.allocated, st1.allocated, rt_nd6_list(NULL, 0), q, eth0_cap.q.qlen + eth1_cap.q.qlen * 1000 + tun0_cap.q.qlen * 100000);
	}
}

/* ---- fragments ---- */
static struct rt_buf *frag(u32 id, u32 off, int more, const u8 *data, u32 len, const rt_in6_addr *dst)
{
	u8 q[8 + 1500];

	q[0] = RT_IPPROTO_ICMPV6;
	q[1] = 0;
	rt_put_be16(q + 2, (u16)(off | (more ? 1 : 0)));
	rt_put_be32(q + 4, id);
	memcpy(q + 8, data, len);
	return mkpkt(&peer_ll, dst, RT_IPPROTO_FRAGMENT, 64, q, 8 + len);
}

static void test_reassembly(void)
{
	u8 msg[1000];
	struct rt_buf *b;
	const u8 *m;
	u32 len, ptr, i;
	u16 ck;

	memset(msg, 0, sizeof(msg));
	msg[0] = RT_ICMP6_ECHO_REQUEST;
	rt_put_be16(msg + 4, 0x4242);
	for (i = 8; i < sizeof(msg); i++)
		msg[i] = (u8)(i * 3);
	ck = rt_csum_fold(rt_csum_partial(msg, sizeof(msg), rt_csum_pseudo6(&peer_ll, &gl0, RT_IPPROTO_ICMPV6, sizeof(msg))));
	memcpy(msg + 2, &ck, 2);
	/* out of order, with a duplicate */
	deliver(&eth0, frag(1, 800, 0, msg + 800, 200, &gl0), peer_mac);
	deliver(&eth0, frag(1, 400, 1, msg + 400, 400, &gl0), peer_mac);
	deliver(&eth0, frag(1, 400, 1, msg + 400, 400, &gl0), peer_mac);
	CHECK(eth0_cap.q.qlen == 0);
	deliver(&eth0, frag(1, 0, 1, msg, 400, &gl0), peer_mac);
	b = take(&eth0_cap, RT_ICMP6_ECHO_REPLY, &m, &len);
	CHECK(b && len == sizeof(msg) && !memcmp(m + 8, msg + 8, sizeof(msg) - 8));
	rt_buf_free(b);
	/* overlap (RFC 5722): the whole datagram goes */
	{
		u32 fails = rt_ip6stat.ReasmFails;
		deliver(&eth0, frag(2, 0, 1, msg, 400, &gl0), peer_mac);
		deliver(&eth0, frag(2, 392, 1, msg + 392, 408, &gl0), peer_mac);
		deliver(&eth0, frag(2, 800, 0, msg + 800, 200, &gl0), peer_mac);
		CHECK(rt_ip6stat.ReasmFails > fails && count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 0);
	}
	/* M set with a length not a multiple of 8: code 0 pointing at the payload length */
	deliver(&eth0, frag(3, 0, 1, msg, 401, &gl0), peer_mac);
	b = take_pp(RT_ICMP6_PARAMPROB_HEADER, &ptr);
	CHECK(b && ptr == 4);
	rt_buf_free(b);
	/* atomic fragment (offset 0, M clear): processed at once (RFC 6946) */
	deliver(&eth0, frag(4, 0, 0, msg, sizeof(msg), &gl0), peer_mac);
	CHECK(count_type(&eth0_cap, RT_ICMP6_ECHO_REPLY) == 1);
	flush(&eth0_cap);
	/* first fragment without the upper layer header (RFC 7112): code 3 */
	{
		u8 q[8 + 8];
		memset(q, 0, sizeof(q));
		q[0] = RT_IPPROTO_DSTOPTS;
		rt_put_be16(q + 2, 1);
		rt_put_be32(q + 4, 5);
		q[8] = RT_IPPROTO_ICMPV6;
		q[9] = 3;                          /* 32 bytes, beyond the fragment */
		deliver(&eth0, mkpkt(&peer_ll, &gl0, RT_IPPROTO_FRAGMENT, 64, q, sizeof(q)), peer_mac);
		b = take_pp(3, &ptr);
		CHECK(b != NULL);
		rt_buf_free(b);
	}
	/* timeout with fragment zero: time exceeded, code 1 */
	deliver(&eth0, frag(6, 0, 1, msg, 400, &gl0), peer_mac);
	advance(61000);
	b = take(&eth0_cap, RT_ICMP6_TIME_EXCEEDED, &m, &len);
	CHECK(b && m[1] == RT_ICMP6_TIME_EXCEED_REASSEMBLY);
	rt_buf_free(b);
	flush(&eth0_cap);
	{
		u32 q, bytes;
		void rt_ip6frag_stats(u32 *queues, u32 *bytes);
		rt_ip6frag_stats(&q, &bytes);
		CHECK(q == 0 && bytes == 0);
	}
}

static void test_fragmentation_pmtu(void)
{
	struct rt_ip6_out o;
	struct rt_buf *b, *f;
	u8 data[3000];
	u32 i, total = 0, id = 0;
	int nfrag = 0, ok = 1;
	rt_in6_addr dst = A("2001:db8:2::77");

	rt_lock();
	CHECK(rt_nd6_add_static(&eth0, &dst, peer_mac) == 0);
	rt_unlock();
	for (i = 0; i < sizeof(data); i++)
		data[i] = (u8)i;
	memset(&o, 0, sizeof(o));
	o.dst = dst;
	o.nxt = RT_IPPROTO_NONE;
	o.hlim = -1;
	b = rt_buf_alloc(RT_BUF_HEADROOM, sizeof(data));
	memcpy(rt_buf_put(b, sizeof(data)), data, sizeof(data));
	rt_lock();
	CHECK(rt_ip6_output(b, &o) == 0);
	rt_unlock();
	while ((f = rt_bufq_dequeue(&eth0_cap.q)) != NULL) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)f->data;
		u16 fo;
		if (f->len > 1500 || ip6->nxt != RT_IPPROTO_FRAGMENT || f->data[40] != RT_IPPROTO_NONE)
			ok = 0;
		fo = rt_get_be16(f->data + 42);
		if (nfrag == 0)
			id = rt_get_be32(f->data + 44);
		else if (rt_get_be32(f->data + 44) != id)
			ok = 0;
		if ((fo & 0xfff8) != total || memcmp(f->data + 48, data + total, f->len - 48))
			ok = 0;
		if (!(fo & 1) != (total + f->len - 48 == sizeof(data)))
			ok = 0;
		if (f->len != 48 + ((rt_ip6_link_mtu(&eth0) - 48) & ~7u) && total + f->len - 48 != sizeof(data))
			ok = 0;
		total += f->len - 48;
		nfrag++;
		rt_buf_free(f);
	}
	CHECK(ok && nfrag == (int)((sizeof(data) + ((rt_ip6_link_mtu(&eth0) - 48) & ~7u) - 1) / ((rt_ip6_link_mtu(&eth0) - 48) & ~7u)) && total == sizeof(data));
	/* packet too big from a router: the path MTU drops (not below 1280) */
	{
		u8 ptb[8 + 1232];
		struct rt_nexthop6 nh;
		struct rt_ip6hdr inner;
		memset(ptb, 0, sizeof(ptb));
		ptb[0] = RT_ICMP6_PACKET_TOO_BIG;
		rt_put_be32(ptb + 4, 1300);
		memset(&inner, 0, sizeof(inner));
		inner.vtcfl = rt_htonl(0x60000000u);
		inner.nxt = RT_IPPROTO_NONE;
		inner.hlim = 64;
		inner.src = gl0;
		inner.dst = dst;
		memcpy(ptb + 8, &inner, 40);
		deliver(&eth0, mkicmp(&rtr_ll, &gl0, 64, ptb, sizeof(ptb)), rtr_mac);
		rt_lock();
		CHECK(rt_ip6_route(&dst, NULL, &nh) == 0 && nh.mtu == 1300);
		rt_unlock();
		rt_put_be32(ptb + 4, 600);
		deliver(&eth0, mkicmp(&rtr_ll, &gl0, 64, ptb, sizeof(ptb)), rtr_mac);
		rt_lock();
		CHECK(rt_ip6_route(&dst, NULL, &nh) == 0 && nh.mtu == 1280);
		rt_unlock();
		/* and a PTB claiming to be about someone else's packet is ignored */
		inner.src = A("2001:db8:2::1234");
		memcpy(ptb + 8, &inner, 40);
		rt_put_be32(ptb + 4, 1280);
		deliver(&eth0, mkicmp(&rtr_ll, &gl0, 64, ptb, sizeof(ptb)), rtr_mac);
	}
	b = rt_buf_alloc(RT_BUF_HEADROOM, 1300);
	memcpy(rt_buf_put(b, 1300), data, 1300);
	o.dontfrag = 1;
	rt_lock();
	CHECK(rt_ip6_output(b, &o) == -RT_EMSGSIZE);
	rt_unlock();
	flush(&eth0_cap);
}

static void test_select_route(void)
{
	rt_in6_addr src, ula = A("fd00:1::1"), dep = A("2001:db8:2::dead");
	struct rt_nexthop6 nh;
	struct in6_ifaddr *ia;

	rt_lock();
	CHECK(rt_in6_addr_add(&eth0, &ula, 64, RT_IN6_IFF_NODAD, RT_IN6_INFINITE, RT_IN6_INFINITE) == 0);
	CHECK(rt_in6_addr_add(&eth0, &dep, 64, RT_IN6_IFF_NODAD, 600, 0) == 0);
	rt_unlock();
	advance(100);
	ia = in6_ifa_find(&eth0, &dep);
	CHECK(ia && (ia->flags & RT_IN6_IFF_DEPRECATED) && !(ia->flags & RT_IN6_IFF_TENTATIVE));
	rt_lock();
	/* rule 2: scope; link-local destination -> link-local source */
	CHECK(rt_ip6_select_source(P("fe80::99"), &eth0, &src) == 0 && rt_in6_eq(&src, &ll0));
	/* global destination -> the preferred global (rule 3 avoids the deprecated one) */
	CHECK(rt_ip6_select_source(P("2001:db8:2::5"), &eth0, &src) == 0 && rt_in6_eq(&src, &gl0));
	/* rule 6 / 8: the ULA for a ULA destination */
	CHECK(rt_ip6_select_source(P("fd00:1::99"), &eth0, &src) == 0 && rt_in6_eq(&src, &ula));
	/* rule 1: the destination itself */
	CHECK(rt_ip6_select_source(&dep, &eth0, &src) == 0 && rt_in6_eq(&src, &dep));
	CHECK(rt_ip6_select_source(&rt_in6addr_loopback, NULL, &src) == 0 && rt_in6_is_loopback(&src));
	/* routing: longest prefix, gateways must be on-link, reject routes */
	CHECK(rt_route6_add(P("2001:db9::"), 32, P("fe80::2"), &eth0, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route6_add(P("2001:db9:5::"), 48, P("fe80::3"), &eth0, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route6_add(P("2001:dba::"), 32, P("2001:dead::1"), NULL, RT_RTF_STATIC, 0) == -RT_ENETUNREACH);
	CHECK(rt_route6_add(P("2001:dba::"), 32, P("fe80::1"), NULL, RT_RTF_STATIC, 0) == -RT_EINVAL);
	CHECK(rt_route6_add(P("2001:dbb::"), 32, NULL, NULL, RT_RTF_STATIC | RT_RTF_REJECT, 0) == 0);
	CHECK(rt_ip6_route(P("2001:db9:5::1"), NULL, &nh) == 0 && rt_in6_eq(&nh.nexthop, P("fe80::3")));
	CHECK(rt_ip6_route(P("2001:db9:6::1"), NULL, &nh) == 0 && rt_in6_eq(&nh.nexthop, P("fe80::2")));
	CHECK(rt_ip6_route(P("2001:dbb::1"), NULL, &nh) == -RT_EHOSTUNREACH);
	CHECK(rt_ip6_route(P("fe80::5"), &eth1, &nh) == 0 && nh.netif == &eth1);
	CHECK(rt_ip6_route(&gl0, NULL, &nh) == 0 && nh.netif == rt_loopback_if);
	CHECK(rt_route6_delete(P("2001:db9::"), 32, NULL, NULL, 0) == 1);
	CHECK(rt_route6_delete(P("2001:db9::"), 32, NULL, NULL, 0) == -RT_ESRCH);
	rt_in6_addr_del(&eth0, &ula);
	rt_in6_addr_del(&eth0, &dep);
	rt_unlock();
}

/* getaddrinfo() ordering (RFC 6724 6) */
static void test_dest_sort(void)
{
	rt_in6_addr l[3];

	rt_lock();
	CHECK(rt_inet_have_addr(RT_AF_INET6));
	/* rule 1: no IPv4 route, the IPv6 destination first */
	l[0] = A("::ffff:198.51.100.1");
	l[1] = A("2001:db8:7::5");
	rt_inet_sort_dest(l, 2);
	CHECK(rt_in6_eq(&l[0], P("2001:db8:7::5")) && rt_in6_eq(&l[1], P("::ffff:198.51.100.1")));
	/* rule 5: a ULA destination with a global source loses to a global one */
	l[0] = A("fd00:9::1");
	l[1] = A("2001:db8:7::5");
	rt_inet_sort_dest(l, 2);
	CHECK(rt_in6_eq(&l[0], P("2001:db8:7::5")));
	/* rule 6: ::1 has the highest precedence; rule 10 keeps equal ones */
	l[0] = A("2001:db8:7::5");
	l[1] = A("2001:db8:7::6");
	l[2] = A("::1");
	rt_inet_sort_dest(l, 3);
	CHECK(rt_in6_is_loopback(&l[0]) && rt_in6_eq(&l[1], P("2001:db8:7::5")) && rt_in6_eq(&l[2], P("2001:db8:7::6")));
	rt_unlock();
}

static void test_ptp(void)
{
	struct in6_ifaddr *ia;

	make_if(&tun0, &tun0_cap, "tun0", NULL, RT_IFT_PTP);
	rt_lock();
	rt_netif_up(&tun0);
	rt_unlock();
	ia = ifa_of(&tun0, 1, -1);
	/* no ND on point-to-point links: usable at once, nothing sent */
	CHECK(ia && !(ia->flags & RT_IN6_IFF_TENTATIVE));
	advance(5000);
	CHECK(count_type(&tun0_cap, RT_ND_NEIGHBOR_SOLICIT) == 0 && count_type(&tun0_cap, RT_ICMP6_MLDV2_REPORT) == 0);
	flush(&tun0_cap);
}

int main(void)
{
	rt_buf_drain_cache();
	test_text();
	CHECK(rt_init() == 0);
	/* the error tests count every ICMPv6 error (test_ratelimit turns it on) */
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.icmp6.ratelimit", "0") == 0);
	CHECK(rt_sysctl_set_text("net.inet6.icmp6.errppslimit", "0") == 0);
	rt_unlock();
	test_bringup();
	test_ra_slaac();
	test_neighbours();
	test_dad_conflict();
	test_gratuitous_na();
	test_exthdr();
	test_reassembly();
	test_fragmentation_pmtu();
	test_select_route();
	test_dest_sort();
	test_ptp();
	test_ratelimit();
	test_fuzz();
	rt_lock();
	rt_netif_remove(&eth0);
	rt_netif_remove(&eth1);
	rt_netif_remove(&tun0);
	rt_unlock();
	flush(&eth0_cap);
	flush(&eth1_cap);
	flush(&tun0_cap);
	{
		struct rt_buf_stats st;
		rt_buf_get_stats(&st);
		CHECK(st.allocated == 0);
		if (st.allocated)
			fprintf(stderr, "leaked %u buffers\n", st.allocated);
	}
	printf("t_ip6_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
