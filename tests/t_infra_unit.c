/*
 * Riptide - unit tests of the infra modules: buffers, timers, checksums,
 * printf, routing, ARP, IP fragmentation/reassembly, ICMP, UDP input and
 * robustness against malformed packets. Single threaded, no stack thread:
 * the test drives rt_stack_poll()/rt_timers_run() itself.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 ip4(int a, int b, int c, int d) { return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d); }

/* ------------------------------------------------------------------ */
static void test_printf(void)
{
	char b[64];
	const char *end;

	rt_snprintf(b, sizeof(b), "%d %i %u %x %X %o %c %s %%", -42, 7, 3000000000u, 0xbeef, 0xbeef, 8, 'z', "str");
	CHECK(strcmp(b, "-42 7 3000000000 beef BEEF 10 z str %") == 0);
	rt_snprintf(b, sizeof(b), "[%5d|%-5d|%05d|%-3s|%3s|%.2s]", 42, 42, -42, "a", "b", "xyz");
	CHECK(strcmp(b, "[   42|42   |-0042|a  |  b|xy]") == 0);
	rt_snprintf(b, sizeof(b), "%lld %llu %lx %zu %hd %hhu", -9000000000LL, 18446744073709551615ULL,
	            0x12345678UL, (size_t)99, (short)-3, (unsigned char)255);
	CHECK(strcmp(b, "-9000000000 18446744073709551615 12345678 99 -3 255") == 0);
	CHECK(rt_snprintf(b, 5, "hello world") == 11 && strcmp(b, "hell") == 0);
	{
		const char *volatile np = NULL;
		CHECK(rt_snprintf(b, sizeof(b), "%s", np) == 6);
	}
	rt_snprintf(b, sizeof(b), "%*d|%-*d", 4, 1, 3, 2);
	CHECK(strcmp(b, "   1|2  ") == 0);
	rt_snprintf(b, sizeof(b), "%p", (void *)0x1234);
	CHECK(strcmp(b, "0x1234") == 0);
	CHECK(rt_snprintf(NULL, 0, "abc%d", 12) == 5);

	CHECK(rt_strtoul("  0x1fZ", &end, 0) == 0x1f && *end == 'Z');
	CHECK(rt_strtoul("0755", NULL, 0) == 0755);
	CHECK(rt_strtoul("123", NULL, 10) == 123);
	CHECK(rt_strtoul("zz", &end, 10) == 0 && strcmp(end, "zz") == 0);
	CHECK(rt_strtoul("99999999999999999999999", NULL, 10) == ~0UL);
	CHECK(rt_strcasecmp("HeLLo", "hello") == 0 && rt_strcasecmp("a", "B") < 0);
	CHECK(rt_strncasecmp("abcX", "ABCY", 3) == 0);
	CHECK(rt_strlcpy(b, "truncate me", 5) == 11 && strcmp(b, "trun") == 0);
}

/* ------------------------------------------------------------------ */
static u16 ref_csum(const u8 *p, u32 len)
{
	u32 s = 0, i;
	u16 r;
	u8 out[2];

	for (i = 0; i + 1 < len; i += 2)
		s += (u32)(p[i] << 8 | p[i + 1]);
	if (len & 1)
		s += (u32)p[len - 1] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	r = (u16)~s;
	out[0] = (u8)(r >> 8);
	out[1] = (u8)r;
	memcpy(&r, out, 2);       /* network order bytes as stored */
	return r;
}

static void test_checksum(void)
{
	/* RFC 1071 4.1 example: sum 0xddf2 -> checksum 0x220d */
	static const u8 v[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
	/* A real IPv4 header including its checksum (b861): sums to zero */
	static const u8 iph[] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
	                          0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
	u8 buf[3000 + 2];
	u16 ck;
	u8 *b;
	int i;

	ck = rt_inet_csum(v, sizeof(v));
	b = (u8 *)&ck;
	CHECK(b[0] == 0x22 && b[1] == 0x0d);
	CHECK(rt_inet_csum(iph, sizeof(iph)) == 0);

	srand(1);
	for (i = 0; i < 3000; i++)
		buf[i] = (u8)rand();
	for (i = 0; i < 400; i++) {
		u32 off = (u32)(rand() % 7), len = (u32)(rand() % 2990);
		CHECK(rt_inet_csum(buf + off, len) == ref_csum(buf + off, len));
	}
	/* chaining with an even first part */
	CHECK(rt_csum_fold(rt_csum_partial(buf + 100, 501, rt_csum_partial(buf, 100, 0))) ==
	      ref_csum(buf, 601));
	/* copy + checksum in one pass; odd-offset pieces of an iov (UDP send) */
	{
		u8 d[700];
		struct rt_iov iov[3];
		u32 sum;
		CHECK(rt_csum_fold(rt_csum_copy(d + 1, buf + 3, 601, 0x1234)) ==
		      rt_csum_fold(rt_csum_partial(buf + 3, 601, 0x1234)) && memcmp(d + 1, buf + 3, 601) == 0);
		iov[0].base = buf + 1;   iov[0].len = 7;
		iov[1].base = buf + 50;  iov[1].len = 300;
		iov[2].base = buf + 400; iov[2].len = 101;
		CHECK(rt_iov_to_buf_csum(iov, 3, 0, d, 408, &sum) == 408);
		CHECK(rt_csum_fold(sum) == ref_csum(d, 408));
	}
	/* pseudo header: UDP datagram from RFC-ish fixed vector computed by reference */
	{
		u8 ph[12 + 8];
		u32 s = ip4(10, 0, 0, 1), d = ip4(10, 0, 0, 2);
		memcpy(ph, &s, 4);
		memcpy(ph + 4, &d, 4);
		ph[8] = 0; ph[9] = 17; ph[10] = 0; ph[11] = 8;
		ph[12] = 0x12; ph[13] = 0x34; ph[14] = 0x00; ph[15] = 0x35;
		ph[16] = 0x00; ph[17] = 0x08; ph[18] = 0; ph[19] = 0;
		CHECK(rt_csum_fold(rt_csum_partial(ph + 12, 8, rt_csum_pseudo(s, d, 17, 8))) ==
		      ref_csum(ph, 20));
	}
}

/* ------------------------------------------------------------------ */
static void test_buf(void)
{
	struct rt_buf_stats st0, st;
	struct rt_buf *a, *b, *c;
	struct rt_bufq q;

	rt_buf_get_stats(&st0);
	a = rt_buf_alloc(RT_BUF_HEADROOM, 100);
	CHECK(a && a->len == 0 && rt_buf_headroom(a) == RT_BUF_HEADROOM && rt_buf_tailroom(a) >= 100);
	memset(rt_buf_put(a, 100), 0xab, 100);
	CHECK(a->len == 100);
	memset(rt_buf_push(a, 20), 0xcd, 20);
	CHECK(a->len == 120 && a->data[0] == 0xcd && a->data[20] == 0xab);
	rt_buf_pull(a, 20);
	CHECK(a->len == 100 && a->data[0] == 0xab);
	a->cb.w[3] = 0x11223344;
	b = rt_buf_copy(a);
	CHECK(b && b->len == 100 && b->data[99] == 0xab && b->cb.w[3] == 0x11223344 &&
	      rt_buf_headroom(b) >= RT_BUF_HEADROOM);
	c = rt_buf_alloc(0, 100000);
	CHECK(c && rt_buf_tailroom(c) >= 100000);
	CHECK(rt_buf_alloc(0, 0x7fffffff) == NULL);

	rt_bufq_init(&q);
	rt_bufq_tail(&q, a);
	rt_bufq_headq(&q, b);
	rt_bufq_insert_after(&q, b, c);
	CHECK(q.qlen == 3 && q.head == b && b->next == c && c->next == a && q.tail == a);
	rt_bufq_unlink(&q, c);
	CHECK(q.qlen == 2 && b->next == a && a->prev == b);
	CHECK(rt_bufq_dequeue(&q) == b && q.qlen == 1);
	rt_buf_free(b);
	rt_buf_free(c);
	rt_bufq_purge(&q);
	CHECK(rt_bufq_empty(&q) && q.qlen == 0 && q.bytes == 0);

	rt_buf_set_limit(st0.bytes + 4000);
	a = rt_buf_alloc(0, 1500);
	b = rt_buf_alloc(0, 1500);
	c = rt_buf_alloc(0, 1500);
	CHECK(a && b && !c);
	rt_buf_free(a);
	rt_buf_free(b);
	rt_buf_set_limit(0);
	rt_buf_get_stats(&st);
	CHECK(st.allocated == st0.allocated && st.bytes == st0.bytes && st.failures > st0.failures);
}

/* ------------------------------------------------------------------ */
static int tlog[16], tn;
static struct rt_timer tt[4];
static void tfn(void *arg)
{
	int id = (int)(intptr_t)arg;
	if (tn < 16)
		tlog[tn++] = id;
	if (id == 2 && tn < 3)
		rt_timer_mod(&tt[2], tt[2].expires);   /* re-arm for "now": next run */
	if (id == 0)
		rt_timer_del(&tt[3]);                  /* cancel a co-expiring timer */
}

static void test_timer(void)
{
	u32 base = rt_now_ms() + 10000000u;
	int i;

	for (i = 0; i < 4; i++)
		rt_timer_init(&tt[i], tfn, (void *)(intptr_t)i);
	rt_timer_mod(&tt[1], base + 30);
	rt_timer_mod(&tt[0], base + 10);
	rt_timer_mod(&tt[2], base + 20);
	rt_timer_mod(&tt[3], base + 10);
	CHECK(rt_timers_next(base) == 10);
	CHECK(rt_timers_run(base + 5) == 5 && tn == 0);
	rt_timers_run(base + 25);
	CHECK(tn == 2 && tlog[0] == 0 && tlog[1] == 2);
	CHECK(!rt_timer_pending(&tt[3]) && rt_timer_pending(&tt[2]));
	rt_timers_run(base + 25);
	CHECK(tn == 3 && tlog[2] == 2);
	rt_timer_del(&tt[1]);
	CHECK(rt_timers_next(base) == RT_TIMER_NONE || rt_timers_next(base) > 30);
	/* wrap-around of the 32-bit clock */
	rt_timer_mod(&tt[0], 0xfffffff0u + 0x20u);
	CHECK(rt_timers_next(0xfffffff0u) == 0x20);
	rt_timer_del(&tt[0]);
}

/* ------------------------------------------------------------------ */
/* A fake link that records what the stack transmits. */
struct cap {
	struct rt_bufq q;
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

static struct rt_netif eth0, ptp0;
static struct cap eth0_cap, ptp0_cap;
static const u8 eth0_mac[6] = { 0x02, 0, 0, 0, 0, 0x10 };
static const u8 peer_mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };

static void setup_ifs(void)
{
	memset(&eth0, 0, sizeof(eth0));
	rt_strlcpy(eth0.name, "fk0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, eth0_mac, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	eth0.driver = &eth0_cap;
	rt_bufq_init(&eth0_cap.q);
	CHECK(rt_netif_add(&eth0) == 0);
	CHECK(rt_netif_add(&eth0) == -RT_EEXIST);
	CHECK(rt_netif_set_addr(&eth0, ip4(192, 168, 1, 10), ip4(255, 255, 255, 0), 0) == 0);
	CHECK(eth0.broadaddr == ip4(192, 168, 1, 255));
	CHECK(rt_netif_set_addr(&eth0, ip4(192, 168, 1, 10), ip4(255, 0, 255, 0), 0) == -RT_EINVAL);
	rt_netif_up(&eth0);

	memset(&ptp0, 0, sizeof(ptp0));
	rt_strlcpy(ptp0.name, "pp0", sizeof(ptp0.name));
	ptp0.type = RT_IFT_PTP;
	ptp0.flags = RT_IFF_POINTOPOINT;
	ptp0.mtu = 576;
	ptp0.link_output = fake_output;
	ptp0.driver = &ptp0_cap;
	rt_bufq_init(&ptp0_cap.q);
	CHECK(rt_netif_add(&ptp0) == 0);
	CHECK(rt_netif_set_addr(&ptp0, ip4(10, 9, 0, 1), ip4(255, 255, 255, 255), ip4(10, 9, 0, 2)) == 0);
	rt_netif_up(&ptp0);
	/* gratuitous ARP when eth0 came up */
	CHECK(eth0_cap.q.qlen == 1);
	rt_bufq_purge(&eth0_cap.q);
}

static void flush_caps(void)
{
	rt_bufq_purge(&eth0_cap.q);
	rt_bufq_purge(&ptp0_cap.q);
}

static void poll_stack(void)
{
	int i;
	for (i = 0; i < 10; i++)
		rt_stack_poll();
}

/* ------------------------------------------------------------------ */
static void test_route(void)
{
	struct rt_nexthop nh;
	struct rt_route *r;

	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == -RT_ENETUNREACH);
	CHECK(rt_route_add(0, 0, ip4(192, 168, 1, 1), NULL, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route_add(ip4(10, 0, 0, 0), ip4(255, 0, 0, 0), ip4(192, 168, 1, 2), NULL, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route_add(ip4(10, 1, 0, 0), ip4(255, 255, 0, 0), ip4(192, 168, 1, 3), NULL, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route_add(ip4(10, 1, 2, 3), ip4(255, 255, 255, 255), ip4(192, 168, 1, 4), NULL, RT_RTF_STATIC, 0) == 0);
	CHECK(rt_route_add(ip4(10, 1, 0, 0), ip4(255, 255, 0, 0), ip4(192, 168, 1, 3), NULL, 0, 0) == -RT_EEXIST);
	CHECK(rt_route_add(ip4(1, 0, 0, 0), ip4(255, 0, 0, 0), ip4(172, 16, 0, 1), NULL, 0, 0) == -RT_ENETUNREACH);
	CHECK(rt_route_add(ip4(1, 0, 0, 0), ip4(255, 0, 255, 0), ip4(192, 168, 1, 1), NULL, 0, 0) == -RT_EINVAL);
	CHECK(rt_route_add(ip4(172, 30, 0, 0), ip4(255, 255, 0, 0), 0, NULL, RT_RTF_REJECT, 0) == 0);

	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 1) &&
	      nh.netif == &eth0 && nh.src == ip4(192, 168, 1, 10) && (nh.flags & RT_RTF_GATEWAY));
	CHECK(rt_ip_route(ip4(10, 2, 0, 1), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 2));
	CHECK(rt_ip_route(ip4(10, 1, 9, 9), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 3));
	CHECK(rt_ip_route(ip4(10, 1, 2, 3), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 4));
	CHECK(rt_ip_route(ip4(192, 168, 1, 77), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 77) &&
	      !(nh.flags & RT_RTF_GATEWAY));
	CHECK(rt_ip_route(ip4(10, 9, 0, 2), &nh) == 0 && nh.netif == &ptp0 && nh.mtu == 576 &&
	      nh.src == ip4(10, 9, 0, 1));
	CHECK(rt_ip_route(ip4(172, 30, 1, 1), &nh) == -RT_EHOSTUNREACH);
	CHECK(rt_ip_route(ip4(192, 168, 1, 10), &nh) == 0 && nh.netif == rt_loopback_if);
	CHECK(rt_ip_route(ip4(127, 0, 0, 1), &nh) == 0 && nh.netif == rt_loopback_if);
	CHECK(rt_ip_route(RT_INADDR_BROADCAST, &nh) == 0 && nh.netif == &eth0 &&
	      nh.nexthop == RT_INADDR_BROADCAST);
	CHECK(rt_ip_select_source(ip4(1, 2, 3, 4)) == ip4(192, 168, 1, 10));

	/* PMTU: lowered, floor at 552, never raised, expires */
	rt_ip_pmtu_update(ip4(10, 1, 9, 9), 1400);
	CHECK(rt_ip_route(ip4(10, 1, 9, 9), &nh) == 0 && nh.mtu == 1400 && nh.nexthop == ip4(192, 168, 1, 3));
	rt_ip_pmtu_update(ip4(10, 1, 9, 9), 1450);
	CHECK(rt_ip_route(ip4(10, 1, 9, 9), &nh) == 0 && nh.mtu == 1400);
	rt_ip_pmtu_update(ip4(10, 1, 9, 9), 100);
	CHECK(rt_ip_route(ip4(10, 1, 9, 9), &nh) == 0 && nh.mtu == 552);
	CHECK(rt_ip_route(ip4(10, 1, 9, 8), &nh) == 0 && nh.mtu == 1500);
	for (r = rt_route_list(); r; r = r->next)
		if (r->flags & RT_RTF_PMTU)
			r->expire_ms = rt_now_ms() - 1;
	CHECK(rt_ip_route(ip4(10, 1, 9, 9), &nh) == 0 && nh.mtu == 1500);

	/* interface down: its routes are skipped */
	rt_netif_down(&ptp0);
	CHECK(rt_ip_route(ip4(10, 9, 0, 2), &nh) == 0 && nh.netif == &eth0);
	rt_netif_up(&ptp0);

	CHECK(rt_route_delete(ip4(10, 1, 2, 3), ip4(255, 255, 255, 255), 0) == 0);
	CHECK(rt_route_delete(ip4(10, 1, 2, 3), ip4(255, 255, 255, 255), 0) == -RT_ESRCH);
	CHECK(rt_ip_route(ip4(10, 1, 2, 3), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 3));
	CHECK(rt_route_delete(ip4(172, 30, 0, 0), ip4(255, 255, 0, 0), 0) == 0);
}

/* ------------------------------------------------------------------ */
static struct rt_buf *make_ip(u32 src, u32 dst, u8 proto, const void *payload, u32 plen)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20 + plen);
	struct rt_iphdr *ip;

	ip = (struct rt_iphdr *)rt_buf_put(b, 20 + plen);
	memset(ip, 0, 20);
	ip->ver_ihl = 0x45;
	ip->tot_len = rt_htons((u16)(20 + plen));
	ip->id = rt_htons(0x1234);
	ip->ttl = 64;
	ip->protocol = proto;
	ip->saddr = src;
	ip->daddr = dst;
	ip->check = rt_inet_csum(ip, 20);
	if (plen)
		memcpy((u8 *)ip + 20, payload, plen);
	return b;
}

static struct rt_buf *make_arp(u16 op, const u8 *sha, u32 sip, const u8 *tha, u32 tip)
{
	struct rt_buf *b = rt_buf_alloc(64, 28);
	u8 *p = rt_buf_put(b, 28);

	rt_put_be16(p, 1);
	rt_put_be16(p + 2, 0x0800);
	p[4] = 6; p[5] = 4;
	rt_put_be16(p + 6, op);
	memcpy(p + 8, sha, 6);
	memcpy(p + 14, &sip, 4);
	memcpy(p + 18, tha, 6);
	memcpy(p + 24, &tip, 4);
	return b;
}

static int find_arp(u32 ip, struct rt_arp_entry_info *out)
{
	struct rt_arp_entry_info tab[32];
	int n = rt_arp_list(tab, 32), i;

	for (i = 0; i < n && i < 32; i++)
		if (tab[i].ip == ip) {
			if (out)
				*out = tab[i];
			return 1;
		}
	return 0;
}

static void test_arp(void)
{
	static const u8 zero[6];
	struct rt_arp_entry_info e;
	struct rt_buf *b;
	struct capcb *cb;
	u32 gw = ip4(192, 168, 1, 1), me = ip4(192, 168, 1, 10);
	u8 pl[4] = { 1, 2, 3, 4 };
	int i;

	flush_caps();
	/* 4 packets to an unresolved neighbour: one broadcast request, 3 queued */
	for (i = 0; i < 4; i++) {
		b = make_ip(me, ip4(8, 8, 8, 8), 253, pl, 4);
		rt_buf_pull(b, 0);
		CHECK(rt_netif_output_ip(&eth0, b, gw) == 0);
	}
	CHECK(eth0_cap.q.qlen == 1);
	b = rt_bufq_dequeue(&eth0_cap.q);
	cb = (struct capcb *)(void *)b->cb.b;
	CHECK(b->len == 28 && cb->ethertype == RT_ETH_P_ARP && !cb->has_dst);
	CHECK(rt_get_be16(b->data + 6) == 1 && memcmp(b->data + 8, eth0_mac, 6) == 0 &&
	      memcmp(b->data + 14, &me, 4) == 0 && memcmp(b->data + 24, &gw, 4) == 0);
	rt_buf_free(b);
	CHECK(find_arp(gw, &e) && e.state == 0);

	/* reply resolves it and flushes the 3 queued packets to the peer MAC */
	rt_netif_input(&eth0, make_arp(2, peer_mac, gw, eth0_mac, me), RT_ETH_P_ARP, peer_mac, 0);
	CHECK(eth0_cap.q.qlen == 3);
	while ((b = rt_bufq_dequeue(&eth0_cap.q)) != NULL) {
		cb = (struct capcb *)(void *)b->cb.b;
		CHECK(cb->has_dst && memcmp(cb->dst, peer_mac, 6) == 0 && cb->ethertype == RT_ETH_P_IP);
		rt_buf_free(b);
	}
	CHECK(find_arp(gw, &e) && e.state == 1 && memcmp(e.hw, peer_mac, 6) == 0 && e.ifindex == eth0.index);

	/* request for our address: we answer, and learn the requester */
	rt_netif_input(&eth0, make_arp(1, peer_mac, ip4(192, 168, 1, 5), zero, me), RT_ETH_P_ARP, peer_mac, RT_BUFF_BCAST);
	CHECK(eth0_cap.q.qlen == 1);
	b = rt_bufq_dequeue(&eth0_cap.q);
	cb = (struct capcb *)(void *)b->cb.b;
	CHECK(b && rt_get_be16(b->data + 6) == 2 && cb->has_dst && memcmp(cb->dst, peer_mac, 6) == 0 &&
	      memcmp(b->data + 18, peer_mac, 6) == 0);
	rt_buf_free(b);
	CHECK(find_arp(ip4(192, 168, 1, 5), &e) && e.state == 2);

	/* request for someone else: ignored, not learned */
	rt_netif_input(&eth0, make_arp(1, peer_mac, ip4(192, 168, 1, 6), zero, ip4(192, 168, 1, 7)), RT_ETH_P_ARP, peer_mac, 0);
	CHECK(eth0_cap.q.qlen == 0 && !find_arp(ip4(192, 168, 1, 6), NULL));

	/* malformed ARP (short, bad hlen) is dropped quietly */
	b = make_arp(1, peer_mac, ip4(192, 168, 1, 6), zero, me);
	b->data[4] = 8;
	rt_netif_input(&eth0, b, RT_ETH_P_ARP, NULL, 0);
	b = make_arp(1, peer_mac, ip4(192, 168, 1, 6), zero, me);
	b->len = 27;
	rt_netif_input(&eth0, b, RT_ETH_P_ARP, NULL, 0);
	CHECK(eth0_cap.q.qlen == 0);

	/* reachable -> stale after the reachable time, then a use probes */
	rt_timers_run(rt_now_ms() + 95000);
	CHECK(find_arp(gw, &e) && e.state == 2);
	b = make_ip(me, ip4(8, 8, 8, 8), 253, pl, 4);
	CHECK(rt_netif_output_ip(&eth0, b, gw) == 0);
	CHECK(eth0_cap.q.qlen == 2);         /* unicast probe + the packet */
	b = rt_bufq_dequeue(&eth0_cap.q);
	cb = (struct capcb *)(void *)b->cb.b;
	CHECK(cb->ethertype == RT_ETH_P_ARP && cb->has_dst && memcmp(cb->dst, peer_mac, 6) == 0);
	rt_buf_free(b);
	flush_caps();
	/* unanswered probes delete the entry */
	for (i = 0; i < 5; i++)
		rt_timers_run(rt_now_ms() + 96000 + 1000u * (u32)i);
	CHECK(!find_arp(gw, NULL));
	flush_caps();

	/* static entries: used directly, survive rt_arp_flush(keep_static) */
	CHECK(rt_arp_add_static(&eth0, ip4(192, 168, 1, 99), peer_mac) == 0);
	CHECK(find_arp(ip4(192, 168, 1, 99), &e) && e.state == 3);
	b = make_ip(me, ip4(192, 168, 1, 99), 253, pl, 4);
	CHECK(rt_netif_output_ip(&eth0, b, ip4(192, 168, 1, 99)) == 0 && eth0_cap.q.qlen == 1);
	flush_caps();
	rt_netif_input(&eth0, make_arp(2, eth0_mac + 0, ip4(192, 168, 1, 99), eth0_mac, me), RT_ETH_P_ARP, NULL, 0);
	rt_arp_flush(&eth0, 1);
	CHECK(find_arp(ip4(192, 168, 1, 99), &e) && memcmp(e.hw, peer_mac, 6) == 0);
	CHECK(rt_arp_delete(ip4(192, 168, 1, 99)) == 0 && rt_arp_delete(ip4(192, 168, 1, 99)) == -RT_ENOENT);

	/* failed resolution of a locally sourced packet: ICMP host
	 * unreachable comes back to us through lo0 */
	{
		u32 before = rt_icmpstat.InDestUnreachs;
		b = make_ip(me, ip4(192, 168, 1, 200), 17, "\x12\x34\x00\x35\x00\x0c\x00\x00" "abcd", 12);
		CHECK(rt_netif_output_ip(&eth0, b, ip4(192, 168, 1, 200)) == 0);
		for (i = 0; i < 4; i++)
			rt_timers_run(rt_now_ms() + 200000 + 1100u * (u32)i);
		poll_stack();
		CHECK(rt_icmpstat.InDestUnreachs == before + 1);
		CHECK(!find_arp(ip4(192, 168, 1, 200), NULL));
	}
	flush_caps();
}

/* ------------------------------------------------------------------ */
static void test_frag(void)
{
	static u8 payload[3000];
	struct rt_buf *frags[16], *b, *r;
	struct rt_ip_out o;
	int n = 0, i;
	u32 total = 0;
	u32 q, bytes;

	for (i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (u8)(i * 7);
	flush_caps();
	/* 3000 bytes over a 576 MTU link -> 6 fragments of <= 552 data */
	b = rt_buf_alloc(RT_BUF_HEADROOM, sizeof(payload));
	memcpy(rt_buf_put(b, sizeof(payload)), payload, sizeof(payload));
	memset(&o, 0, sizeof(o));
	o.daddr = ip4(10, 9, 0, 2);
	o.proto = 253;
	CHECK(rt_ip_output(b, &o) == 0);
	CHECK(ptp0_cap.q.qlen == 6);
	while ((b = rt_bufq_dequeue(&ptp0_cap.q)) != NULL && n < 16) {
		const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;
		u16 fo = rt_ntohs(ip->frag_off);
		CHECK(b->len <= 576 && rt_inet_csum(ip, 20) == 0 && rt_ntohs(ip->tot_len) == b->len);
		CHECK(((fo & RT_IP_OFFMASK) << 3) == total);
		CHECK(((fo & RT_IP_MF) != 0) == (total + b->len - 20 < sizeof(payload)));
		if (fo & RT_IP_MF)
			CHECK(((b->len - 20) & 7) == 0);
		total += b->len - 20;
		frags[n++] = b;
	}
	CHECK(total == sizeof(payload));

	/* reassemble in a scrambled order with a duplicate */
	r = rt_ipfrag_reass(rt_buf_copy(frags[5]));
	CHECK(!r);
	r = rt_ipfrag_reass(rt_buf_copy(frags[2]));
	CHECK(!r);
	r = rt_ipfrag_reass(rt_buf_copy(frags[2]));   /* exact duplicate */
	CHECK(!r);
	for (i = 0; i < 5 && !r; i++)
		if (i != 2)
			r = rt_ipfrag_reass(rt_buf_copy(frags[i]));
	CHECK(r && r->len == 20 + sizeof(payload) && memcmp(r->data + 20, payload, sizeof(payload)) == 0);
	if (r) {
		const struct rt_iphdr *ip = (const struct rt_iphdr *)r->data;
		CHECK(rt_inet_csum(ip, 20) == 0 && ip->frag_off == 0 && rt_ntohs(ip->tot_len) == r->len);
		rt_buf_free(r);
	}
	rt_ipfrag_stats(&q, &bytes);
	CHECK(q == 0 && bytes == 0);

	/* overlap (not a duplicate) kills the datagram */
	{
		u32 fails = rt_ipstat.ReasmFails;
		struct rt_buf *ov = rt_buf_copy(frags[1]);
		struct rt_iphdr *ip = (struct rt_iphdr *)ov->data;
		u16 fo = rt_ntohs(ip->frag_off);
		ip->frag_off = rt_htons((u16)((fo & ~RT_IP_OFFMASK) | ((fo & RT_IP_OFFMASK) + 1)));
		CHECK(!rt_ipfrag_reass(rt_buf_copy(frags[0])));
		CHECK(!rt_ipfrag_reass(rt_buf_copy(frags[1])));
		CHECK(!rt_ipfrag_reass(ov));
		CHECK(rt_ipstat.ReasmFails == fails + 1);
		rt_ipfrag_stats(&q, NULL);
		CHECK(q == 0);
		for (i = 2; i < 6; i++)
			CHECK(!rt_ipfrag_reass(rt_buf_copy(frags[i])));
	}
	/* those leftovers time out with ICMP time exceeded? Not: frag 0 is
	 * missing, so no ICMP is sent (RFC 792) */
	flush_caps();
	rt_timers_run(rt_now_ms() + 31000);
	rt_ipfrag_stats(&q, NULL);
	CHECK(q == 0 && ptp0_cap.q.qlen == 0);
	/* incomplete datagram with fragment 0 -> ICMP time exceeded code 1
	 * goes back to the sender (10.9.0.2 via pp0) */
	{
		struct rt_buf *f0 = rt_buf_copy(frags[0]);
		struct rt_iphdr *ip = (struct rt_iphdr *)f0->data;
		ip->saddr = ip4(10, 9, 0, 2);
		ip->daddr = ip4(10, 9, 0, 1);
		ip->check = 0;
		ip->check = rt_inet_csum(ip, 20);
		rt_netif_input(&ptp0, f0, RT_ETH_P_IP, NULL, 0);
		rt_timers_run(rt_now_ms() + 62000);
		CHECK(ptp0_cap.q.qlen == 1);
		b = rt_bufq_dequeue(&ptp0_cap.q);
		if (b) {
			CHECK(b->data[20] == RT_ICMP_TIMXCEED && b->data[21] == 1);
			rt_buf_free(b);
		}
	}
	/* DF too big -> EMSGSIZE */
	b = rt_buf_alloc(RT_BUF_HEADROOM, 1000);
	rt_buf_put(b, 1000);
	o.df = 1;
	CHECK(rt_ip_output(b, &o) == -RT_EMSGSIZE);
	/* ping of death: offset + length beyond 65535 */
	{
		struct rt_buf *f = rt_buf_copy(frags[5]);
		struct rt_iphdr *ip = (struct rt_iphdr *)f->data;
		ip->frag_off = rt_htons(RT_IP_OFFMASK);
		CHECK(!rt_ipfrag_reass(f));
		rt_ipfrag_stats(&q, NULL);
		CHECK(q == 0);
	}
	for (i = 0; i < n; i++)
		rt_buf_free(frags[i]);
	flush_caps();
}

/* ------------------------------------------------------------------ */
static void test_icmp_udp_input(void)
{
	u8 echo[8 + 56];
	struct rt_buf *b;
	u16 ck;
	int i;
	u32 peer = ip4(10, 9, 0, 2), me = ip4(10, 9, 0, 1);

	flush_caps();
	memset(echo, 0, sizeof(echo));
	echo[0] = RT_ICMP_ECHO;
	echo[4] = 0x12; echo[5] = 0x34; echo[7] = 1;
	for (i = 8; i < (int)sizeof(echo); i++)
		echo[i] = (u8)i;
	ck = rt_inet_csum(echo, sizeof(echo));
	memcpy(echo + 2, &ck, 2);
	rt_netif_input(&ptp0, make_ip(peer, me, 1, echo, sizeof(echo)), RT_ETH_P_IP, NULL, 0);
	CHECK(ptp0_cap.q.qlen == 1);
	b = rt_bufq_dequeue(&ptp0_cap.q);
	if (b) {
		const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;
		CHECK(ip->saddr == me && ip->daddr == peer && ip->protocol == 1 && ip->ttl == 64);
		CHECK(rt_inet_csum(ip, 20) == 0 && rt_inet_csum(b->data + 20, b->len - 20) == 0);
		CHECK(b->data[20] == 0 && memcmp(b->data + 24, echo + 4, sizeof(echo) - 4) == 0);
		rt_buf_free(b);
	}
	/* bad ICMP checksum: no reply */
	echo[10] ^= 1;
	rt_netif_input(&ptp0, make_ip(peer, me, 1, echo, sizeof(echo)), RT_ETH_P_IP, NULL, 0);
	CHECK(ptp0_cap.q.qlen == 0);
	echo[10] ^= 1;
	/* broadcast echo: ignored by default (bmcastecho=0) */
	rt_netif_input(&eth0, make_ip(ip4(192, 168, 1, 5), ip4(192, 168, 1, 255), 1, echo, sizeof(echo)),
	               RT_ETH_P_IP, NULL, RT_BUFF_BCAST);
	CHECK(eth0_cap.q.qlen == 0);

	/* UDP to a closed port -> ICMP port unreachable quoting the datagram */
	{
		u8 udp[8 + 4] = { 0x30, 0x39, 0x00, 0x07, 0x00, 12, 0, 0, 'p', 'i', 'n', 'g' };
		u32 noports = rt_udpstat.NoPorts;
		rt_netif_input(&ptp0, make_ip(peer, me, 17, udp, sizeof(udp)), RT_ETH_P_IP, NULL, 0);
		CHECK(rt_udpstat.NoPorts == noports + 1);
		CHECK(ptp0_cap.q.qlen == 1);
		b = rt_bufq_dequeue(&ptp0_cap.q);
		if (b) {
			CHECK(b->len == 20 + 8 + 20 + 12);
			CHECK(b->data[20] == RT_ICMP_UNREACH && b->data[21] == RT_ICMP_UNREACH_PORT);
			CHECK(memcmp(b->data + 48, udp, 12) == 0);
			rt_buf_free(b);
		}
		/* broadcast UDP to a closed port: silently dropped */
		rt_netif_input(&eth0, make_ip(ip4(192, 168, 1, 5), RT_INADDR_BROADCAST, 17, udp, sizeof(udp)),
		               RT_ETH_P_IP, NULL, RT_BUFF_BCAST);
		CHECK(eth0_cap.q.qlen == 0);
	}
	/* unknown protocol -> protocol unreachable */
	rt_netif_input(&ptp0, make_ip(peer, me, 200, "x", 1), RT_ETH_P_IP, NULL, 0);
	b = rt_bufq_dequeue(&ptp0_cap.q);
	CHECK(b && b->data[20] == RT_ICMP_UNREACH && b->data[21] == RT_ICMP_UNREACH_PROTOCOL);
	rt_buf_free(b);
	/* not for us, forwarding off: dropped silently */
	rt_netif_input(&ptp0, make_ip(peer, ip4(10, 1, 1, 1), 200, "x", 1), RT_ETH_P_IP, NULL, 0);
	CHECK(ptp0_cap.q.qlen == 0 && eth0_cap.q.qlen == 0);
	/* forwarding on: TTL decremented, sent via eth0 (ARP for 192.168.1.3) */
	rt_ip_forwarding = 1;
	b = make_ip(peer, ip4(10, 1, 1, 1), 200, "x", 1);
	rt_netif_input(&ptp0, b, RT_ETH_P_IP, NULL, 0);
	CHECK(eth0_cap.q.qlen == 1);            /* ARP request; packet queued */
	/* TTL 1 -> time exceeded back to the sender */
	b = make_ip(peer, ip4(10, 1, 1, 1), 200, "x", 1);
	((struct rt_iphdr *)b->data)->ttl = 1;
	((struct rt_iphdr *)b->data)->check = 0;
	((struct rt_iphdr *)b->data)->check = rt_inet_csum(b->data, 20);
	rt_netif_input(&ptp0, b, RT_ETH_P_IP, NULL, 0);
	b = rt_bufq_dequeue(&ptp0_cap.q);
	CHECK(b && b->data[20] == RT_ICMP_TIMXCEED && b->data[21] == 0);
	rt_buf_free(b);
	rt_ip_forwarding = 0;
	rt_arp_flush(&eth0, 0);
	flush_caps();

	/* ICMP needfrag about a datagram we sent lowers the path MTU */
	{
		u8 msg[8 + 20 + 8];
		struct rt_nexthop nh;
		struct rt_iphdr *in = (struct rt_iphdr *)(msg + 8);

		memset(msg, 0, sizeof(msg));
		msg[0] = RT_ICMP_UNREACH;
		msg[1] = RT_ICMP_UNREACH_NEEDFRAG;
		rt_put_be16(msg + 6, 1280);
		in->ver_ihl = 0x45;
		in->tot_len = rt_htons(1500);
		in->protocol = 17;
		in->saddr = ip4(192, 168, 1, 10);
		in->daddr = ip4(10, 50, 0, 1);
		ck = rt_inet_csum(msg, sizeof(msg));
		memcpy(msg + 2, &ck, 2);
		rt_netif_input(&eth0, make_ip(ip4(192, 168, 1, 2), ip4(192, 168, 1, 10), 1, msg, sizeof(msg)),
		               RT_ETH_P_IP, NULL, 0);
		CHECK(rt_ip_route(ip4(10, 50, 0, 1), &nh) == 0 && nh.mtu == 1280);
	}
}

/* ------------------------------------------------------------------ */
/* Malformed and random packets must never crash or read out of bounds
 * (run under ASan/UBSan with make SAN=1). */
static void test_fuzz(void)
{
	static const u8 icmp_echo[8] = { 8, 0, 0xf7, 0xff, 0, 0, 0, 0 };
	u32 seed = 12345;
	int i, j;

	for (i = 0; i < 20000; i++) {
		u32 len, proto;
		struct rt_buf *b;
		u8 *p;

		seed = seed * 1103515245u + 12345u;
		proto = (seed >> 8) % 5;
		len = (seed >> 16) % 120;
		b = make_ip(ip4(10, 9, 0, 2), ip4(10, 9, 0, 1),
		            proto == 0 ? 1 : proto == 1 ? 17 : proto == 2 ? 6 : 200, icmp_echo, 8);
		p = b->data;
		/* random mutations, keeping the header checksum valid half the time */
		for (j = 0; j < 4; j++) {
			seed = seed * 1103515245u + 12345u;
			p[(seed >> 16) % 28] = (u8)(seed >> 8);
		}
		if (seed & 0x10000) {
			((struct rt_iphdr *)p)->check = 0;
			((struct rt_iphdr *)p)->check = rt_inet_csum(p, 20);
		}
		b->len = len < b->len ? len : b->len;
		rt_netif_input(&ptp0, b, RT_ETH_P_IP, NULL, (seed & 0x20000) ? RT_BUFF_BCAST : 0);
		if ((i & 255) == 0)
			flush_caps();
	}
	for (i = 0; i < 5000; i++) {
		struct rt_buf *b = rt_buf_alloc(16, 40);
		u8 *p = rt_buf_put(b, 40);
		for (j = 0; j < 40; j++) {
			seed = seed * 1103515245u + 12345u;
			p[j] = (u8)(seed >> 16);
		}
		rt_put_be16(p, 1);
		rt_put_be16(p + 2, 0x0800);
		p[4] = 6; p[5] = 4;
		b->len = (seed >> 8) % 41;
		rt_netif_input(&eth0, b, RT_ETH_P_ARP, NULL, 0);
	}
	poll_stack();
	/* fuzzed fragments wait in reassembly queues until they expire */
	rt_timers_run(rt_now_ms() + 31000);
	poll_stack();
	flush_caps();
	rt_arp_flush(NULL, 0);
}

int main(void)
{
	struct rt_buf_stats st;

	test_printf();
	test_checksum();
	test_buf();
	test_timer();

	CHECK(rt_init() == 0);
	CHECK(rt_loopback_if && (rt_loopback_if->flags & RT_IFF_UP));
	CHECK(rt_ip_is_local(RT_INADDR_LOOPBACK));
	setup_ifs();
	CHECK(rt_ip_is_local(ip4(192, 168, 1, 10)) && rt_ip_is_local(ip4(192, 168, 1, 255)) &&
	      !rt_ip_is_local(ip4(192, 168, 1, 11)));
	CHECK(rt_ip_is_broadcast(ip4(192, 168, 1, 255), &eth0) && !rt_ip_is_broadcast(ip4(10, 9, 0, 2), NULL));
	test_route();
	test_arp();
	test_frag();
	test_icmp_udp_input();
	test_fuzz();

	poll_stack();
	flush_caps();
	rt_buf_get_stats(&st);
	CHECK(st.allocated == 0);
	if (st.allocated)
		fprintf(stderr, "leaked %u buffers\n", st.allocated);

	printf("t_infra_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
