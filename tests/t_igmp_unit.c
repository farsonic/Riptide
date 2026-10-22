/*
 * Riptide - unit tests of IPv4 multicast (src/core/igmp.c and the socket
 * API on top), single threaded with a fake Ethernet link whose
 * transmissions are captured; the clock is moved with
 * rt_host_advance_clock():
 *   - membership refcounts and the link layer filter (shared 01:00:5e)
 *   - IGMPv3 state change reports (robustness, record types, router
 *     alert, TTL, TOS) and current state reports for general,
 *     group and group-and-source queries
 *   - fallback to v2 (report suppression, leave) and v1, timeouts,
 *     net.inet.igmp.version
 *   - malformed messages
 *   - IP input filtering, IP_ADD/DROP_MEMBERSHIP, per socket delivery,
 *     IP_MULTICAST_IF/TTL/LOOP, the local copy and TTL 0
 *   - net.riptide.mcast records
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/infra.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define IP(a, b, c, d) rt_htonl(((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

/* ---- fake Ethernet link with captured output ---- */
struct cap {
	struct rt_bufq q;
	u8 mc[32][6];
	int mcref[32];
	int nmc;
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

/* counts calls per address: the stack must call once per address */
static int fake_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct cap *c = nif->driver;
	int i;

	for (i = 0; i < c->nmc; i++)
		if (!memcmp(c->mc[i], hw, 6))
			break;
	if (join) {
		if (i == c->nmc) {
			memcpy(c->mc[c->nmc], hw, 6);
			c->mcref[c->nmc++] = 0;
		}
		c->mcref[i]++;
	} else if (i < c->nmc && --c->mcref[i] == 0) {
		c->nmc--;
		memcpy(c->mc[i], c->mc[c->nmc], 6);
		c->mcref[i] = c->mcref[c->nmc];
	}
	return 0;
}

static int mc_refs(const struct cap *c, u32 grp)
{
	u32 h = rt_ntohl(grp);
	u8 mac[6] = { 0x01, 0x00, 0x5e, (u8)((h >> 16) & 0x7f), (u8)(h >> 8), (u8)h };
	int i;

	for (i = 0; i < c->nmc; i++)
		if (!memcmp(c->mc[i], mac, 6))
			return c->mcref[i];
	return 0;
}

static struct rt_netif eth0;
static struct cap cap0;
static const u8 eth0_mac[6] = { 0x02, 0, 0, 0, 0, 0x10 };
static const u8 peer_mac[6] = { 0x02, 0, 0, 0, 0, 0x01 };

static void flush(void)
{
	rt_bufq_purge(&cap0.q);
}

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

/* ---- captured IGMP messages ---- */
struct msg {
	struct rt_buf *b;
	const u8 *m;          /* IGMP message */
	u32 len;
	u32 dst;
};

static int is_igmp(const struct rt_buf *b)
{
	return b->len >= 20 && (b->data[0] >> 4) == 4 && b->data[9] == RT_IPPROTO_IGMP;
}

/* first captured IGMP message (unlinked; free with msg_free) */
static int take(struct msg *g)
{
	struct rt_buf *b;

	for (b = cap0.q.head; b; b = b->next) {
		u32 hl;
		if (!is_igmp(b))
			continue;
		rt_bufq_unlink(&cap0.q, b);
		hl = (u32)(b->data[0] & 15) * 4;
		g->b = b;
		g->m = b->data + hl;
		g->len = b->len - hl;
		memcpy(&g->dst, b->data + 16, 4);
		return 1;
	}
	g->b = NULL;
	return 0;
}

static void msg_free(struct msg *g)
{
	rt_buf_free(g->b);
	g->b = NULL;
}

static int count_igmp(void)
{
	struct rt_buf *b;
	int n = 0;

	for (b = cap0.q.head; b; b = b->next)
		n += is_igmp(b);
	return n;
}

/* v3 record i: type, group, sources */
static const u8 *v3_rec(const struct msg *g, int i)
{
	const u8 *p = g->m + 8;

	while (i-- > 0)
		p += 8 + 4u * rt_get_be16(p + 2) + 4u * p[1];
	return p;
}

static int v3_has(const struct msg *g, u8 type, u32 grp)
{
	int i, n = rt_get_be16(g->m + 6);

	for (i = 0; i < n; i++) {
		const u8 *r = v3_rec(g, i);
		if (r[0] == type && !memcmp(r + 4, &grp, 4))
			return 1;
	}
	return 0;
}

/* ---- injecting packets ---- */
static struct rt_buf *mk_ip(u32 src, u32 dst, u8 proto, u8 ttl, const void *payload, u32 len)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20 + len);
	struct rt_iphdr *ip = (struct rt_iphdr *)rt_buf_put(b, 20);

	memset(ip, 0, 20);
	ip->ver_ihl = 0x45;
	ip->tot_len = rt_htons((u16)(20 + len));
	ip->ttl = ttl;
	ip->protocol = proto;
	ip->saddr = src;
	ip->daddr = dst;
	ip->check = rt_inet_csum(ip, 20);
	memcpy(rt_buf_put(b, len), payload, len);
	return b;
}

static void deliver(struct rt_buf *b)
{
	rt_lock();
	rt_netif_input(&eth0, b, RT_ETH_P_IP, peer_mac, RT_BUFF_MCAST);
	rt_stack_poll();
	rt_unlock();
}

static void send_igmp(u32 src, u32 dst, const u8 *m, u32 len, int good_csum)
{
	u8 tmp[256];
	u16 ck;

	memcpy(tmp, m, len);
	tmp[2] = tmp[3] = 0;
	ck = rt_inet_csum(tmp, len);
	if (!good_csum)
		ck ^= 0x1234;
	memcpy(tmp + 2, &ck, 2);
	deliver(mk_ip(src, dst, RT_IPPROTO_IGMP, 1, tmp, len));
}

static void query_v3(u32 grp, u8 code, const u32 *src, int nsrc)
{
	u8 q[12 + 4 * 8];
	int i;

	memset(q, 0, sizeof(q));
	q[0] = 0x11;
	q[1] = code;
	memcpy(q + 4, &grp, 4);
	q[8] = 2;           /* QRV */
	q[9] = 125;         /* QQIC */
	rt_put_be16(q + 10, (u16)nsrc);
	for (i = 0; i < nsrc; i++)
		memcpy(q + 12 + 4 * i, &src[i], 4);
	send_igmp(IP(10, 0, 0, 1), grp ? grp : IP(224, 0, 0, 1), q, 12 + 4u * nsrc, 1);
}

static void query_v12(u32 grp, u8 code)
{
	u8 q[8];

	memset(q, 0, sizeof(q));
	q[0] = 0x11;
	q[1] = code;
	memcpy(q + 4, &grp, 4);
	send_igmp(IP(10, 0, 0, 1), grp ? grp : IP(224, 0, 0, 1), q, 8, 1);
}

static int join(u32 g)
{
	int r;

	rt_lock();
	r = rt_ip_mc_join(&eth0, g);
	rt_unlock();
	return r;
}

static int leave(u32 g)
{
	int r;

	rt_lock();
	r = rt_ip_mc_leave(&eth0, g);
	rt_unlock();
	return r;
}

static int joined(u32 g)
{
	int r;

	rt_lock();
	r = rt_ip_mc_joined(&eth0, g);
	rt_unlock();
	return r;
}

static int set_version(const char *v)
{
	int r;

	rt_lock();
	r = rt_sysctl_set_text("net.inet.igmp.version", v);
	rt_unlock();
	return r;
}

/* ------------------------------------------------------------------ */
static void test_bringup(void)
{
	memset(&eth0, 0, sizeof(eth0));
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, eth0_mac, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	eth0.mcast = fake_mcast;
	eth0.driver = &cap0;
	rt_bufq_init(&cap0.q);
	rt_lock();
	CHECK(rt_netif_add(&eth0) == 0);
	eth0.ip6_disabled = 1;          /* keep MLD out of the capture */
	CHECK(rt_netif_set_addr(&eth0, IP(10, 0, 0, 2), IP(255, 255, 255, 0), 0) == 0);
	rt_netif_up(&eth0);
	rt_unlock();
	advance(2000);
	/* 224.0.0.1: joined, in the filter, never reported */
	CHECK(joined(IP(224, 0, 0, 1)));
	CHECK(mc_refs(&cap0, IP(224, 0, 0, 1)) == 1);
	CHECK(count_igmp() == 0);
	flush();
}

static void test_v3_state_changes(void)
{
	struct msg g;
	const u8 *ip;
	u32 g1 = IP(239, 1, 2, 3), g2 = IP(224, 1, 2, 3);   /* same 01:00:5e:01:02:03 */

	CHECK(join(IP(10, 0, 0, 9)) == -RT_EINVAL);
	CHECK(join(g1) == 0);
	CHECK(mc_refs(&cap0, g1) == 1);
	advance(0);
	CHECK(take(&g));
	if (g.b) {
		struct capcb *cb = (struct capcb *)(void *)g.b->cb.b;
		ip = g.b->data;
		CHECK(ip[0] == 0x46 && ip[1] == 0xc0 && ip[8] == 1);              /* RA, TOS, TTL */
		CHECK(ip[20] == 0x94 && ip[21] == 4 && ip[22] == 0 && ip[23] == 0);
		CHECK(rt_inet_csum(ip, 24) == 0);
		CHECK(!memcmp(ip + 12, &eth0.addr, 4));
		CHECK(g.dst == IP(224, 0, 0, 22));
		CHECK(cb->has_dst && cb->dst[0] == 1 && cb->dst[2] == 0x5e && cb->dst[5] == 0x16);
		CHECK(g.m[0] == 0x22 && rt_inet_csum(g.m, g.len) == 0);
		CHECK(rt_get_be16(g.m + 6) == 1 && v3_has(&g, 4, g1));   /* CHANGE_TO_EXCLUDE {} */
		CHECK(g.len == 16);
		msg_free(&g);
	}
	CHECK(count_igmp() == 0);
	/* robustness 2: one retransmission within 1 s */
	advance(1000);
	CHECK(take(&g) && v3_has(&g, 4, g1));
	msg_free(&g);
	advance(5000);
	CHECK(count_igmp() == 0);

	/* refcounts: another user, no report; the filter entry is shared
	 * with a second group on the same MAC */
	CHECK(join(g1) == 0);
	CHECK(join(g2) == 0);
	CHECK(mc_refs(&cap0, g1) == 1);
	advance(3000);
	CHECK(take(&g) && v3_has(&g, 4, g2) && !v3_has(&g, 4, g1));
	msg_free(&g);
	flush();
	CHECK(leave(g1) == 0);
	advance(3000);
	CHECK(count_igmp() == 0);
	CHECK(joined(g1));
	CHECK(leave(g1) == 0);
	CHECK(!joined(g1));
	CHECK(leave(g1) == -RT_EADDRNOTAVAIL);
	CHECK(mc_refs(&cap0, g1) == 1);         /* still needed by g2 */
	advance(0);
	CHECK(take(&g) && v3_has(&g, 3, g1));   /* CHANGE_TO_INCLUDE {} */
	msg_free(&g);
	advance(1000);
	CHECK(take(&g) && v3_has(&g, 3, g1));
	msg_free(&g);
	advance(3000);
	CHECK(count_igmp() == 0);
	CHECK(leave(g2) == 0);
	CHECK(mc_refs(&cap0, g1) == 0);
	advance(3000);
	CHECK(count_igmp() == 2);
	flush();
	/* leave and join again before the leave went out: one TO_EX */
	CHECK(join(g1) == 0);
	advance(3000);
	flush();
	CHECK(leave(g1) == 0);
	CHECK(join(g1) == 0);
	advance(0);
	CHECK(take(&g) && v3_has(&g, 4, g1) && !v3_has(&g, 3, g1));
	msg_free(&g);
	advance(3000);
	flush();
}

static void test_v3_queries(void)
{
	struct msg g;
	u32 g1 = IP(239, 1, 2, 3), g3 = IP(239, 3, 3, 3);
	u32 srcs[2] = { IP(10, 0, 0, 77), IP(10, 0, 0, 78) };
	struct rt_igmp_stats before = rt_igmpstat;

	CHECK(join(g3) == 0);
	advance(3000);
	flush();
	/* general query, Max Resp Time 2 s: one current state report with
	 * MODE_IS_EXCLUDE for each group, never 224.0.0.1 */
	query_v3(0, 20, NULL, 0);
	CHECK(rt_igmpstat.InQueriesV3 == before.InQueriesV3 + 1);
	advance(2100);
	CHECK(count_igmp() == 1);
	CHECK(take(&g));
	if (g.b) {
		CHECK(rt_get_be16(g.m + 6) == 2 && v3_has(&g, 2, g1) && v3_has(&g, 2, g3));
		CHECK(!v3_has(&g, 2, IP(224, 0, 0, 1)));
		msg_free(&g);
	}
	/* group query */
	query_v3(g3, 10, NULL, 0);
	advance(1100);
	CHECK(take(&g) && rt_get_be16(g.m + 6) == 1 && v3_has(&g, 2, g3));
	msg_free(&g);
	/* group query for a group we are not in: nothing */
	query_v3(IP(239, 8, 8, 8), 10, NULL, 0);
	advance(1100);
	CHECK(count_igmp() == 0);
	/* group-and-source query: IS_IN of the queried sources */
	query_v3(g3, 10, srcs, 2);
	advance(1100);
	CHECK(take(&g));
	if (g.b) {
		const u8 *r = v3_rec(&g, 0);
		CHECK(r[0] == 1 && rt_get_be16(r + 2) == 2 && !memcmp(r + 4, &g3, 4));
		CHECK(!memcmp(r + 8, &srcs[0], 4) && !memcmp(r + 12, &srcs[1], 4));
		CHECK(g.len == 8 + 8 + 8);
		msg_free(&g);
	}
	/* exponential Max Resp Code: 0x8f = (0xf | 0x10) << 3 = 248 (24.8 s) */
	query_v3(0, 0x8f, NULL, 0);
	advance(24900);
	CHECK(count_igmp() == 1);
	flush();
	/* malformed: short v3 query, bad checksum, query for a unicast group */
	{
		u8 q[12];
		struct rt_igmp_stats s0 = rt_igmpstat;
		memset(q, 0, sizeof(q));
		q[0] = 0x11;
		q[1] = 10;
		send_igmp(IP(10, 0, 0, 1), IP(224, 0, 0, 1), q, 10, 1);
		CHECK(rt_igmpstat.InBadQueries == s0.InBadQueries + 1);
		rt_put_be16(q + 10, 5);              /* 5 sources announced, none there */
		send_igmp(IP(10, 0, 0, 1), IP(224, 0, 0, 1), q, 12, 1);
		CHECK(rt_igmpstat.InBadQueries == s0.InBadQueries + 2);
		send_igmp(IP(10, 0, 0, 1), IP(224, 0, 0, 1), q, 8, 0);
		CHECK(rt_igmpstat.InErrors == s0.InErrors + 1);
		send_igmp(IP(10, 0, 0, 1), IP(224, 0, 0, 1), q, 4, 1);
		CHECK(rt_igmpstat.InErrors == s0.InErrors + 2);
		q[1] = 0;
		rt_put_be16(q + 10, 0);
		memcpy(q + 4, &eth0.addr, 4);
		send_igmp(IP(10, 0, 0, 1), IP(224, 0, 0, 1), q, 12, 1);
		CHECK(rt_igmpstat.InBadQueries == s0.InBadQueries + 3);
		advance(11000);
		CHECK(count_igmp() == 0);
	}
	CHECK(leave(g3) == 0);
	advance(3000);
	flush();
}

static int mcast_rec(u32 grp, struct rt_ctl_mcast *out)
{
	struct rt_ctl_mcast recs[16];
	int n, i;

	rt_lock();
	n = rt_mcast_list(recs, 16);
	rt_unlock();
	for (i = 0; i < n && i < 16; i++)
		if (recs[i].family == RT_AF_INET && !memcmp(recs[i].group + 12, &grp, 4) &&
		    recs[i].ifindex == eth0.index) {
			if (out)
				*out = recs[i];
			return 1;
		}
	return 0;
}

static void test_v2_v1(void)
{
	struct msg g;
	struct rt_ctl_mcast r;
	u32 g1 = IP(239, 1, 2, 3), g4 = IP(239, 4, 4, 4);
	u8 rep[8];

	/* v2 querier: compatibility mode 2, report to the group */
	query_v12(0, 50);                  /* 5 s */
	CHECK(mcast_rec(g1, &r) && r.igmp == 2);
	advance(5100);
	CHECK(take(&g));
	if (g.b) {
		CHECK(g.m[0] == 0x16 && g.len == 8 && !memcmp(g.m + 4, &g1, 4) && g.dst == g1);
		CHECK(g.b->data[0] == 0x46 && g.b->data[8] == 1);
		msg_free(&g);
	}
	CHECK(count_igmp() == 0);
	CHECK(mcast_rec(g1, &r) && r.reporter);
	/* suppression: another member reports first */
	query_v12(g1, 50);
	memset(rep, 0, sizeof(rep));
	rep[0] = 0x16;
	memcpy(rep + 4, &g1, 4);
	{
		u32 ours = rt_igmpstat.InOurReports;
		send_igmp(IP(10, 0, 0, 33), g1, rep, 8, 1);
		CHECK(rt_igmpstat.InOurReports == ours + 1);
	}
	advance(5100);
	CHECK(count_igmp() == 0);
	CHECK(mcast_rec(g1, &r) && !r.reporter);
	/* v3 query while in v2 mode: answered as v2 */
	query_v3(0, 10, NULL, 0);
	advance(1100);
	CHECK(take(&g) && g.m[0] == 0x16);
	msg_free(&g);
	/* join in v2 mode: report at once, repeated within 10 s */
	CHECK(join(g4) == 0);
	advance(0);
	CHECK(take(&g) && g.m[0] == 0x16 && !memcmp(g.m + 4, &g4, 4));
	msg_free(&g);
	advance(10000);
	CHECK(take(&g) && g.m[0] == 0x16 && !memcmp(g.m + 4, &g4, 4));
	msg_free(&g);
	advance(10000);
	CHECK(count_igmp() == 0);
	/* leave: we were the last reporter -> leave to 224.0.0.2 */
	CHECK(leave(g4) == 0);
	CHECK(take(&g) && g.m[0] == 0x17 && g.dst == IP(224, 0, 0, 2) && !memcmp(g.m + 4, &g4, 4));
	msg_free(&g);
	advance(3000);
	CHECK(count_igmp() == 0);

	/* v1 querier: v1 reports (Max Resp Time 10 s), no leaves */
	query_v12(0, 0);
	CHECK(mcast_rec(g1, &r) && r.igmp == 1);
	advance(10100);
	CHECK(take(&g) && g.m[0] == 0x12 && g.dst == g1);
	msg_free(&g);
	CHECK(join(g4) == 0);
	advance(20000);
	CHECK(take(&g) && g.m[0] == 0x12);
	msg_free(&g);
	flush();
	CHECK(leave(g4) == 0);
	advance(1000);
	CHECK(count_igmp() == 0);

	/* the v1 querier goes away: after 400 s back to v3 (the v2 querier
	 * present timer, 260 s, ran out before) */
	advance(400000);
	flush();
	CHECK(mcast_rec(g1, &r) && r.igmp == 3);
	CHECK(join(g4) == 0);
	advance(0);
	CHECK(take(&g) && g.m[0] == 0x22 && v3_has(&g, 4, g4));
	msg_free(&g);
	advance(3000);
	flush();

	/* net.inet.igmp.version 2 forces v2 */
	CHECK(set_version("2") == 0);
	CHECK(leave(g4) == 0);
	CHECK(count_igmp() == 0);          /* no v2 leave: never sent a v2 report for it */
	CHECK(join(g4) == 0);
	advance(0);
	CHECK(take(&g) && g.m[0] == 0x16);
	msg_free(&g);
	advance(12000);
	flush();
	CHECK(leave(g4) == 0);
	flush();
	CHECK(set_version("0") == 0);
	CHECK(set_version("4") < 0);
	advance(3000);
	flush();
}

/* ---- sockets ---- */
static struct rt_sockaddr_in sa(u32 addr, u16 port)
{
	struct rt_sockaddr_in s;

	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = addr;
	s.sin_port = rt_htons(port);
	return s;
}

static int udp_pkt(u32 dst, u16 port, const char *text)
{
	u8 p[64];
	u32 n = (u32)strlen(text);
	u32 before = rt_udpstat.InDatagrams;

	memset(p, 0, 8);
	rt_put_be16(p, 4000);
	rt_put_be16(p + 2, port);
	rt_put_be16(p + 4, (u16)(8 + n));
	memcpy(p + 8, text, n);          /* checksum 0: none */
	deliver(mk_ip(IP(10, 0, 0, 1), dst, RT_IPPROTO_UDP, 1, p, 8 + n));
	return (int)(rt_udpstat.InDatagrams - before);
}

static int rd(struct rt_sockctx *c, int fd, char *buf)
{
	int n = rt_api_recv(c, fd, buf, 63, RT_MSG_DONTWAIT);

	if (n >= 0)
		buf[n] = 0;
	return n;
}

static struct rt_buf *take_udp(void)
{
	struct rt_buf *b;

	for (b = cap0.q.head; b; b = b->next)
		if (b->len >= 28 && (b->data[0] >> 4) == 4 && b->data[9] == RT_IPPROTO_UDP) {
			rt_bufq_unlink(&cap0.q, b);
			return b;
		}
	return NULL;
}

static void test_sockets(void)
{
	struct rt_sockctx c;
	struct rt_ip_mreq mr;
	struct rt_ip_mreqn mrn;
	struct rt_sockaddr_in a;
	struct rt_buf *b;
	u32 g5 = IP(239, 5, 5, 5), g6 = IP(239, 6, 6, 6), len, v;
	int s1, s2, s3, one = 1, i;
	u8 ttl;
	char buf[64];
	struct rt_ip_stats ip0;

	CHECK(rt_ctx_init(&c, 16, NULL) == 0);
	s1 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	s2 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	s3 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(s1 >= 0 && s2 >= 0 && s3 >= 0);
	CHECK(rt_api_setsockopt(&c, s1, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one)) == 0);
	CHECK(rt_api_setsockopt(&c, s2, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one)) == 0);
	a = sa(0, 5000);
	CHECK(rt_api_bind(&c, s1, &a, sizeof(a)) == 0);
	a = sa(g5, 5000);                 /* bound to the group itself */
	CHECK(rt_api_bind(&c, s2, &a, sizeof(a)) == 0);

	/* datagrams for groups nobody joined are dropped by IP */
	ip0 = rt_ipstat;
	CHECK(udp_pkt(g5, 5000, "x") == 0);
	CHECK(rt_ipstat.InDiscards == ip0.InDiscards + 1);

	/* options */
	mr.imr_multiaddr = IP(10, 1, 1, 1);
	mr.imr_interface = 0;
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == -RT_EINVAL);
	mr.imr_multiaddr = g5;
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, 4) == -RT_EINVAL);
	mr.imr_interface = IP(10, 9, 9, 9);
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) ==
	      -RT_EADDRNOTAVAIL);
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_DROP_MEMBERSHIP, &mr, sizeof(mr)) ==
	      -RT_EADDRNOTAVAIL);
	mr.imr_interface = eth0.addr;
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0);
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) ==
	      -RT_EADDRINUSE);
	CHECK(joined(g5) && mc_refs(&cap0, g5) == 1);
	/* struct ip_mreqn by index, INADDR_ANY = default interface */
	memset(&mrn, 0, sizeof(mrn));
	mrn.imr_multiaddr = g5;
	mrn.imr_ifindex = (s32)eth0.index;
	CHECK(rt_api_setsockopt(&c, s2, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mrn, sizeof(mrn)) == 0);
	mr.imr_multiaddr = g6;
	mr.imr_interface = 0;
	CHECK(rt_api_setsockopt(&c, s2, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0);
	CHECK(joined(g6));
	{
		struct rt_ctl_mcast r;
		CHECK(mcast_rec(g5, &r) && r.refcnt == 2 && r.hwlen == 6 && r.hwaddr[0] == 1 && r.hwaddr[5] == 5);
	}
	advance(3000);
	flush();

	/* delivery: s1 and s2 are members of g5, s3 (same port) is not */
	CHECK(rt_api_setsockopt(&c, s3, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one)) == 0);
	a = sa(0, 5000);
	CHECK(rt_api_bind(&c, s3, &a, sizeof(a)) == 0);
	CHECK(udp_pkt(g5, 5000, "hello") == 2);
	CHECK(rd(&c, s1, buf) == 5 && !strcmp(buf, "hello"));
	CHECK(rd(&c, s2, buf) == 5);
	CHECK(rd(&c, s3, buf) == -RT_EWOULDBLOCK);
	/* g6: only s2 is a member, and s2 is bound to g5 */
	CHECK(udp_pkt(g6, 5000, "six") == 0);
	CHECK(rd(&c, s1, buf) < 0 && rd(&c, s2, buf) < 0 && rd(&c, s3, buf) < 0);
	/* 224.0.0.1 reaches every socket on the port */
	CHECK(udp_pkt(IP(224, 0, 0, 1), 5000, "all") == 2);
	CHECK(rd(&c, s1, buf) == 3 && rd(&c, s3, buf) == 3);
	/* drop: s1 no longer gets g5 */
	mr.imr_multiaddr = g5;
	mr.imr_interface = 0;
	CHECK(rt_api_setsockopt(&c, s1, RT_IPPROTO_IP, RT_IP_DROP_MEMBERSHIP, &mr, sizeof(mr)) == 0);
	CHECK(udp_pkt(g5, 5000, "again") == 1);
	CHECK(rd(&c, s1, buf) < 0 && rd(&c, s2, buf) == 5);

	/* sending: defaults TTL 1 and loop on */
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &v, &len) == 0 && v == 1);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_LOOP, &v, &len) == 0 && v == 1);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, &len) == 0 && v == 0);
	a = sa(g5, 5000);
	CHECK(rt_api_sendto(&c, s3, "loop", 4, 0, &a, sizeof(a)) == 4);
	b = take_udp();
	CHECK(b != NULL);
	if (b) {
		struct capcb *cb = (struct capcb *)(void *)b->cb.b;
		CHECK(b->data[8] == 1 && !memcmp(b->data + 12, &eth0.addr, 4) && !memcmp(b->data + 16, &g5, 4));
		CHECK(cb->has_dst && cb->dst[0] == 1 && cb->dst[1] == 0 && cb->dst[2] == 0x5e &&
		      cb->dst[3] == 5 && cb->dst[4] == 5 && cb->dst[5] == 5);
		rt_buf_free(b);
	}
	advance(0);
	CHECK(rd(&c, s2, buf) == 4 && !strcmp(buf, "loop"));   /* the local copy */
	CHECK(rd(&c, s3, buf) < 0);                           /* not a member */
	/* TTL 7 (one byte option value), loop off */
	ttl = 7;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &ttl, 1) == 0);
	ttl = 0;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_LOOP, &ttl, 1) == 0);
	CHECK(rt_api_sendto(&c, s3, "noloop", 6, 0, &a, sizeof(a)) == 6);
	b = take_udp();
	CHECK(b && b->data[8] == 7);
	rt_buf_free(b);
	advance(0);
	CHECK(rd(&c, s2, buf) < 0);
	/* TTL 0: the local copy only */
	ttl = 1;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_LOOP, &ttl, 1) == 0);
	ttl = 0;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &ttl, 1) == 0);
	CHECK(rt_api_sendto(&c, s3, "local", 5, 0, &a, sizeof(a)) == 5);
	CHECK(take_udp() == NULL);
	advance(0);
	CHECK(rd(&c, s2, buf) == 5 && !strcmp(buf, "local"));
	/* IP_MULTICAST_IF: by address, read back; unknown address refused */
	v = eth0.addr;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, 4) == 0);
	v = 0;
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, &len) == 0 && v == eth0.addr);
	v = IP(10, 9, 9, 9);
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, 4) == -RT_EADDRNOTAVAIL);
	/* struct ip_mreqn: the index wins; struct ip_mreq: the second field */
	memset(&mrn, 0, sizeof(mrn));
	mrn.imr_ifindex = 1;                 /* lo0 */
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &mrn, sizeof(mrn)) == 0);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, &len) == 0 &&
	      v == rt_htonl(0x7f000001));
	mr.imr_multiaddr = 0;
	mr.imr_interface = eth0.addr;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &mr, sizeof(mr)) == 0);
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, &len) == 0 && v == eth0.addr);
	ttl = 3;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &ttl, 1) == 0);
	/* via lo0: the interface must be named; loopback delivers through lo0 */
	v = rt_htonl(0x7f000001);
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, 4) == 0);
	CHECK(rt_api_sendto(&c, s3, "lo", 2, 0, &a, sizeof(a)) == 2);
	CHECK(take_udp() == NULL);
	advance(0);
	CHECK(rd(&c, s2, buf) < 0);       /* s2 joined on eth0, not on lo0 */
	v = eth0.addr;
	CHECK(rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &v, 4) == 0);
	/* connect() to a group works without a route to it */
	CHECK(rt_api_connect(&c, s3, &a, sizeof(a)) == 0);
	CHECK(rt_api_send(&c, s3, "conn", 4, 0) == 4);
	b = take_udp();
	CHECK(b && b->data[8] == 3);
	rt_buf_free(b);
	advance(0);
	CHECK(rd(&c, s2, buf) == 4);
	flush();

	/* the per socket limit */
	for (i = 0; i < RT_IP_MAX_MEMBERSHIPS + 1; i++) {
		int r;
		mr.imr_multiaddr = IP(239, 200, 0, i);
		r = rt_api_setsockopt(&c, s3, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
		CHECK(i < RT_IP_MAX_MEMBERSHIPS ? r == 0 : r == -RT_ETOOMANYREFS);
	}
	/* close leaves the groups */
	CHECK(rt_api_close(&c, s3) == 0);
	CHECK(!joined(IP(239, 200, 0, 0)));
	CHECK(rt_api_close(&c, s1) == 0);
	CHECK(joined(g5) && joined(g6));
	CHECK(rt_api_close(&c, s2) == 0);
	CHECK(!joined(g5) && !joined(g6));
	CHECK(mc_refs(&cap0, g5) == 0 && mc_refs(&cap0, g6) == 0);
	rt_ctx_destroy(&c);
	advance(5000);
	flush();
}

static void test_raw(void)
{
	struct rt_sockctx c;
	struct rt_ip_mreq mr;
	struct rt_sockaddr_in a;
	struct rt_buf *b;
	u32 g7 = IP(239, 7, 7, 7);
	int s;
	u8 ttl = 4;

	CHECK(rt_ctx_init(&c, 4, NULL) == 0);
	s = rt_api_socket(&c, RT_AF_INET, RT_SOCK_RAW, 253);
	CHECK(s >= 0);
	mr.imr_multiaddr = g7;
	mr.imr_interface = 0;
	CHECK(rt_api_setsockopt(&c, s, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0);
	CHECK(joined(g7));
	CHECK(rt_api_setsockopt(&c, s, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &ttl, 1) == 0);
	a = sa(g7, 0);
	CHECK(rt_api_sendto(&c, s, "raw", 3, 0, &a, sizeof(a)) == 3);
	for (b = cap0.q.head; b; b = b->next)
		if (b->data[9] == 253)
			break;
	CHECK(b && b->data[8] == 4 && !memcmp(b->data + 16, &g7, 4));
	advance(0);
	{
		char buf[64];
		/* the local copy, with the IP header */
		CHECK(rt_api_recv(&c, s, buf, sizeof(buf), RT_MSG_DONTWAIT) == 23);
	}
	CHECK(rt_api_close(&c, s) == 0);
	CHECK(!joined(g7));
	rt_ctx_destroy(&c);
	advance(5000);
	flush();
}

static void test_down_remove(void)
{
	struct msg g;
	u32 g8 = IP(239, 8, 8, 8);

	CHECK(join(g8) == 0);
	advance(3000);
	flush();
	/* down: nothing sent; up again: the groups are announced again */
	rt_lock();
	rt_netif_down(&eth0);
	rt_unlock();
	query_v3(0, 10, NULL, 0);
	advance(3000);
	CHECK(count_igmp() == 0);
	rt_lock();
	rt_netif_up(&eth0);
	rt_unlock();
	advance(0);
	CHECK(take(&g) && v3_has(&g, 4, g8));
	msg_free(&g);
	advance(3000);
	flush();
	/* a new address re-announces too (reports with the right source) */
	rt_lock();
	rt_netif_set_addr(&eth0, IP(10, 0, 0, 3), IP(255, 255, 255, 0), 0);
	rt_unlock();
	advance(0);
	CHECK(take(&g) && v3_has(&g, 4, g8) && !memcmp(g.b->data + 12, &eth0.addr, 4));
	msg_free(&g);
	advance(3000);
	flush();
	rt_lock();
	rt_netif_remove(&eth0);
	rt_unlock();
	CHECK(cap0.nmc == 0);
	CHECK(!mcast_rec(g8, NULL));
	flush();
}

int main(void)
{
	rt_buf_drain_cache();
	CHECK(rt_init() == 0);
	test_bringup();
	test_v3_state_changes();
	test_v3_queries();
	test_v2_v1();
	test_sockets();
	test_raw();
	test_down_remove();
	{
		struct rt_buf_stats st;
		rt_buf_get_stats(&st);
		CHECK(st.allocated == 0);
		if (st.allocated)
			fprintf(stderr, "leaked %u buffers\n", st.allocated);
	}
	printf("t_igmp_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
