/*
 * Riptide - IPv6 neighbour discovery for hosts (RFC 4861): neighbour
 * cache with neighbour unreachability detection, neighbour and router
 * solicitations/advertisements, redirects, and the router advertisement
 * options for autoconfiguration (prefix, MTU, RDNSS/DNSSL of RFC 8106,
 * route information of RFC 4191).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Neighbour states (RFC 4861 7.3.2):
 *   INCOMPLETE  multicast NS sent, up to ND_UNRES_QLEN packets queued;
 *               MAX_MULTICAST_SOLICIT tries RetransTimer apart, then the
 *               queued packets get ICMPv6 address unreachable.
 *   REACHABLE   confirmed by a solicited NA; STALE after ReachableTime.
 *   STALE       usable; the first use moves it to DELAY.
 *   DELAY       waits DELAY_FIRST_PROBE_TIME for confirmation, then PROBE.
 *   PROBE       unicast NS every RetransTimer, MAX_UNICAST_SOLICIT times.
 *   PERMANENT   static entry.
 * There are no upper layer reachability hints (as in arp.c): DELAY always
 * ends in a probe. Failed router entries are kept a little while
 * (FAILED) so default router selection can avoid them.
 */
#include "ip6_internal.h"

#define ND_UNRES_QLEN          3
#define MAX_MULTICAST_SOLICIT  3
#define MAX_UNICAST_SOLICIT    3
#define DELAY_FIRST_PROBE_MS   5000u
#define ND_GC_STALE_MS         600000u
#define ND_FAILED_KEEP_MS      20000u
#define ND_MAX_ENTRIES         256
#define ND_HASH                64

#define ST_INCOMPLETE RT_NUD_INCOMPLETE
#define ST_REACHABLE  RT_NUD_REACHABLE
#define ST_STALE      RT_NUD_STALE
#define ST_PERMANENT  RT_NUD_PERMANENT
#define ST_DELAY      RT_NUD_DELAY
#define ST_PROBE      RT_NUD_PROBE
#define ST_FAILED     RT_NUD_FAILED

/* option types */
#define ND_OPT_SLLA     1
#define ND_OPT_TLLA     2
#define ND_OPT_PREFIX   3
#define ND_OPT_REDIR    4
#define ND_OPT_MTU      5
#define ND_OPT_NONCE    14
#define ND_OPT_ROUTE    24
#define ND_OPT_RDNSS    25
#define ND_OPT_DNSSL    31

struct nd6_entry {
	struct nd6_entry *next;
	struct rt_netif *nif;
	rt_in6_addr addr;
	u8  hw[6];
	u8  state;
	u8  probes;
	u8  router;
	u32 updated_ms;
	u32 used_ms;
	struct rt_bufq pending;
	struct rt_timer timer;
};

static struct nd6_entry *nd_tab[ND_HASH];
static int nd_count;

static unsigned nd_hash(const rt_in6_addr *a)
{
	u32 h = a->w[2] ^ a->w[3] ^ (a->w[0] >> 7);
	h ^= h >> 16;
	return (h ^ (h >> 8)) & (ND_HASH - 1);
}

static struct nd6_entry *nd_lookup(const struct rt_netif *nif, const rt_in6_addr *a)
{
	struct nd6_entry *e;

	for (e = nd_tab[nd_hash(a)]; e; e = e->next)
		if (e->nif == nif && rt_in6_eq(&e->addr, a))
			return e;
	return NULL;
}

static void nd_timer_fn(void *arg);

static void nd_destroy(struct nd6_entry *e)
{
	struct nd6_entry **pp;

	for (pp = &nd_tab[nd_hash(&e->addr)]; *pp; pp = &(*pp)->next) {
		if (*pp == e) {
			*pp = e->next;
			break;
		}
	}
	rt_timer_del(&e->timer);
	rt_bufq_purge(&e->pending);
	rt_free(e);
	nd_count--;
}

static int nd_forced_gc(void)
{
	struct nd6_entry *e, *victim = NULL;
	u32 now = rt_now_ms();
	int i;

	for (i = 0; i < ND_HASH; i++)
		for (e = nd_tab[i]; e; e = e->next)
			if (e->state != ST_PERMANENT &&
			    (!victim || (s32)((now - e->used_ms) - (now - victim->used_ms)) > 0))
				victim = e;
	if (!victim)
		return -1;
	nd_destroy(victim);
	return 0;
}

static struct nd6_entry *nd_create(struct rt_netif *nif, const rt_in6_addr *a)
{
	struct nd6_entry *e;
	unsigned h;

	if (nd_count >= ND_MAX_ENTRIES && nd_forced_gc() < 0)
		return NULL;
	e = rt_zalloc(sizeof(*e));
	if (!e)
		return NULL;
	e->nif = nif;
	e->addr = *a;
	e->state = ST_INCOMPLETE;
	e->updated_ms = e->used_ms = rt_now_ms();
	rt_bufq_init(&e->pending);
	rt_timer_init(&e->timer, nd_timer_fn, e);
	h = nd_hash(a);
	e->next = nd_tab[h];
	nd_tab[h] = e;
	nd_count++;
	return e;
}

static void nd_set_state(struct nd6_entry *e, u8 st, u32 timeout_ms)
{
	e->state = st;
	e->probes = 0;
	e->updated_ms = rt_now_ms();
	if (timeout_ms)
		rt_timer_after(&e->timer, timeout_ms);
	else
		rt_timer_del(&e->timer);
}

static u32 retrans_ms(const struct rt_netif *nif)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	return ifp && ifp->retrans_ms ? ifp->retrans_ms : 1000u;
}

static u32 reachable_ms(struct rt_netif *nif)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	return ifp ? ip6_reachable_ms(ifp) : 30000u;
}

/* ---- transmission of ND messages ---- */

/* Fixed header + send at link level to hw (NULL: the multicast mapping of
 * dst). Hop limit 255 (RFC 4861: receivers check it). */
static void nd_xmit(struct rt_netif *nif, struct rt_buf *b, const rt_in6_addr *src,
                    const rt_in6_addr *dst, const u8 *hw)
{
	struct rt_ip6hdr *ip6;
	u8 mac[6];
	u16 ck;
	u8 *m = b->data;

	ck = rt_csum_fold(rt_csum_partial(m, b->len, rt_csum_pseudo6(src, dst, RT_IPPROTO_ICMPV6, b->len)));
	memcpy(m + 2, &ck, 2);
	ip6 = (struct rt_ip6hdr *)rt_buf_push(b, RT_IP6_HLEN);
	ip6->vtcfl = rt_htonl(0x60000000u);
	ip6->plen = rt_htons((u16)(b->len - RT_IP6_HLEN));
	ip6->nxt = RT_IPPROTO_ICMPV6;
	ip6->hlim = 255;
	ip6->src = *src;
	ip6->dst = *dst;
	rt_ip6stat.OutRequests++;
	rt_icmp6stat.OutMsgs++;
	if (!(nif->flags & RT_IFF_UP)) {
		rt_buf_free(b);
		return;
	}
	if (!hw) {
		rt_in6_mc_mac(mac, dst);
		hw = mac;
		rt_ip6stat.OutMcastPkts++;
		nif->stats.omcasts++;
	}
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	if (nif->link_output(nif, b, nif->type == RT_IFT_ETHER ? hw : NULL, RT_ETH_P_IPV6) < 0)
		nif->stats.oerrors++;
}

static u8 *put_lla_opt(u8 *p, u8 type, const struct rt_netif *nif)
{
	p[0] = type;
	p[1] = 1;
	memcpy(p + 2, nif->hwaddr, 6);
	return p + 8;
}

/* Source for an NS that resolves 'target' on behalf of packet b (RFC 4861
 * 7.2.2): b's source if it is ours on nif, else a usable address of nif. */
static int ns_source(struct rt_netif *nif, const struct rt_buf *b, const rt_in6_addr *target,
                     rt_in6_addr *src)
{
	if (b && b->len >= RT_IP6_HLEN) {
		const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
		struct in6_ifaddr *ia = in6_ifa_find(nif, &ip6->src);
		if (ia && !(ia->flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED))) {
			*src = ip6->src;
			return 0;
		}
	}
	return rt_ip6_select_source(target, nif, src);
}

void nd6_send_ns(struct rt_netif *nif, const rt_in6_addr *target, const rt_in6_addr *src,
                 const u8 *unicast_hw, const u8 *nonce)
{
	struct rt_buf *b;
	rt_in6_addr dst, s;
	u32 len = 24 + (src ? 8 : 0) + (nonce ? 8 : 0);
	u8 *p;

	if (nif->type != RT_IFT_ETHER)
		return;
	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b)
		return;
	p = rt_buf_put(b, len);
	memset(p, 0, len);
	p[0] = RT_ND_NEIGHBOR_SOLICIT;
	memcpy(p + 8, target->b, 16);
	p += 24;
	if (src) {
		s = *src;
		p = put_lla_opt(p, ND_OPT_SLLA, nif);
	} else {
		memset(&s, 0, sizeof(s));
	}
	if (nonce) {
		p[0] = ND_OPT_NONCE;
		p[1] = 1;
		memcpy(p + 2, nonce, 6);
	}
	if (unicast_hw)
		dst = *target;
	else
		rt_in6_solnode(&dst, target);
	rt_icmp6stat.OutNeighborSolicits++;
	nd_xmit(nif, b, &s, &dst, unicast_hw);
}

void nd6_send_na(struct rt_netif *nif, const rt_in6_addr *dst, const rt_in6_addr *target,
                 int solicited, int override)
{
	struct rt_buf *b;
	struct nd6_entry *e;
	rt_in6_addr src = *target;
	u8 *p;

	b = rt_buf_alloc(RT_BUF_HEADROOM, 32);
	if (!b)
		return;
	p = rt_buf_put(b, 32);
	memset(p, 0, 32);
	p[0] = RT_ND_NEIGHBOR_ADVERT;
	p[4] = (u8)((rt_ip6_forwarding ? 0x80 : 0) | (solicited ? 0x40 : 0) | (override ? 0x20 : 0));
	memcpy(p + 8, target->b, 16);
	put_lla_opt(p + 24, ND_OPT_TLLA, nif);
	rt_icmp6stat.OutNeighborAdvertisements++;
	if (rt_in6_is_mcast(dst)) {
		nd_xmit(nif, b, &src, dst, NULL);
		return;
	}
	e = nd_lookup(nif, dst);
	if (e && e->state != ST_INCOMPLETE && e->state != ST_FAILED) {
		nd_xmit(nif, b, &src, dst, e->hw);
		return;
	}
	/* no link address yet: the normal output path resolves it */
	{
		u16 ck;
		u8 *m = b->data;
		ck = rt_csum_fold(rt_csum_partial(m, b->len, rt_csum_pseudo6(&src, dst, RT_IPPROTO_ICMPV6, b->len)));
		memcpy(m + 2, &ck, 2);
		rt_icmp6stat.OutMsgs++;
		ip6_output_direct(b, nif, &src, dst, RT_IPPROTO_ICMPV6, 255, 0);
	}
}

void nd6_send_rs(struct rt_netif *nif)
{
	static const rt_in6_addr all_routers = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } };
	struct in6_ifaddr *ll = in6_ifa_linklocal(nif, 1);
	struct rt_buf *b;
	rt_in6_addr src;
	u32 len = ll && nif->type == RT_IFT_ETHER ? 16 : 8;
	u8 *p;

	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b)
		return;
	p = rt_buf_put(b, len);
	memset(p, 0, len);
	p[0] = RT_ND_ROUTER_SOLICIT;
	if (ll) {
		src = ll->addr;
		if (len == 16)
			put_lla_opt(p + 8, ND_OPT_SLLA, nif);
	} else {
		memset(&src, 0, sizeof(src));
	}
	rt_icmp6stat.OutRouterSolicits++;
	nd_xmit(nif, b, &src, &all_routers, NULL);
}

/* ---- the state machine ---- */

static void nd_xmit_data(struct nd6_entry *e, struct rt_buf *b)
{
	struct rt_netif *nif = e->nif;

	e->used_ms = rt_now_ms();
	if (nif->link_output(nif, b, e->hw, RT_ETH_P_IPV6) < 0)
		nif->stats.oerrors++;
}

static void nd_solicit(struct nd6_entry *e, const struct rt_buf *trigger, int unicast)
{
	rt_in6_addr src;

	e->probes++;
	if (ns_source(e->nif, trigger, &e->addr, &src) < 0) {
		struct in6_ifaddr *ll = in6_ifa_linklocal(e->nif, 1);
		if (!ll)
			return;
		src = ll->addr;
	}
	nd6_send_ns(e->nif, &e->addr, &src, unicast ? e->hw : NULL, NULL);
}

static void nd_timer_fn(void *arg)
{
	struct nd6_entry *e = arg;
	u32 now = rt_now_ms();

	switch (e->state) {
	case ST_INCOMPLETE:
		if (e->probes < MAX_MULTICAST_SOLICIT) {
			nd_solicit(e, e->pending.head, 0);
			rt_timer_after(&e->timer, retrans_ms(e->nif));
			return;
		}
		/* RFC 4861 7.2.2: address unreachable to the senders */
		{
			struct rt_bufq q = e->pending;
			struct rt_buf *b;
			rt_bufq_init(&e->pending);
			if (e->router) {
				nd_set_state(e, ST_FAILED, ND_FAILED_KEEP_MS);
			} else {
				nd_destroy(e);
			}
			while ((b = rt_bufq_dequeue(&q)) != NULL) {
				rt_icmp6_send_error(b, RT_ICMP6_DST_UNREACH, RT_ICMP6_DST_UNREACH_ADDR, 0);
				rt_buf_free(b);
			}
		}
		return;
	case ST_REACHABLE:
		nd_set_state(e, ST_STALE, ND_GC_STALE_MS);
		return;
	case ST_STALE:
		if ((s32)(now - e->used_ms) >= (s32)ND_GC_STALE_MS) {
			nd_destroy(e);
			return;
		}
		rt_timer_mod(&e->timer, e->used_ms + ND_GC_STALE_MS);
		return;
	case ST_DELAY:
		nd_set_state(e, ST_PROBE, retrans_ms(e->nif));
		nd_solicit(e, NULL, 1);
		return;
	case ST_PROBE:
		if (e->probes < MAX_UNICAST_SOLICIT) {
			nd_solicit(e, NULL, 1);
			rt_timer_after(&e->timer, retrans_ms(e->nif));
			return;
		}
		if (e->router) {
			nd_set_state(e, ST_FAILED, ND_FAILED_KEEP_MS);
			return;
		}
		nd_destroy(e);
		return;
	case ST_FAILED:
		nd_destroy(e);
		return;
	}
}

int nd6_resolve_output(struct rt_netif *nif, struct rt_buf *b, const rt_in6_addr *nexthop)
{
	struct nd6_entry *e = nd_lookup(nif, nexthop);

	if (!e) {
		e = nd_create(nif, nexthop);
		if (!e) {
			nif->stats.oerrors++;
			rt_buf_free(b);
			return -RT_ENOBUFS;
		}
	}
	switch (e->state) {
	case ST_FAILED:
		e->state = ST_INCOMPLETE;
		e->probes = 0;
		/* fall through */
	case ST_INCOMPLETE:
		if (e->pending.qlen >= ND_UNRES_QLEN) {
			nif->stats.oerrors++;
			rt_buf_free(rt_bufq_dequeue(&e->pending));
		}
		rt_bufq_tail(&e->pending, b);
		if (!rt_timer_pending(&e->timer)) {
			nd_solicit(e, b, 0);
			rt_timer_after(&e->timer, retrans_ms(nif));
		}
		return 0;
	case ST_STALE:
		nd_set_state(e, ST_DELAY, DELAY_FIRST_PROBE_MS);
		break;
	default:
		break;
	}
	nd_xmit_data(e, b);
	return 0;
}

/* Learn addr -> hw from an unsolicited source (NS, RS, RA, redirect):
 * RFC 4861 7.2.3 / 6.3.4: new or changed entries become STALE. */
static struct nd6_entry *nd_learn(struct rt_netif *nif, const rt_in6_addr *a, const u8 *hw)
{
	struct nd6_entry *e = nd_lookup(nif, a);
	struct rt_buf *b;

	if (!e) {
		e = nd_create(nif, a);
		if (!e)
			return NULL;
	} else if (e->state == ST_PERMANENT) {
		return e;
	} else if (e->state != ST_INCOMPLETE && e->state != ST_FAILED && !memcmp(e->hw, hw, 6)) {
		return e;
	}
	memcpy(e->hw, hw, 6);
	nd_set_state(e, ST_STALE, ND_GC_STALE_MS);
	while ((b = rt_bufq_dequeue(&e->pending)) != NULL)
		nd_xmit_data(e, b);
	return e;
}

int nd6_router_reachable(const struct rt_netif *nif, const rt_in6_addr *addr)
{
	struct nd6_entry *e = nd_lookup(nif, addr);

	if (!nif || nif->type != RT_IFT_ETHER)
		return 1;
	if (!e)
		return 1;                 /* unknown: probably reachable */
	if (e->state == ST_FAILED)
		return 0;
	if (e->state == ST_INCOMPLETE && e->probes >= MAX_MULTICAST_SOLICIT)
		return 0;
	return 1;
}

void nd6_router_flag(struct rt_netif *nif, const rt_in6_addr *addr, int is_router)
{
	struct nd6_entry *e = nd_lookup(nif, addr);

	if (e)
		e->router = (u8)(is_router != 0);
	if (!is_router) {
		/* RFC 4861 7.2.5: no longer a router: drop it as default router */
		rt_in6_addr any;
		memset(&any, 0, sizeof(any));
		rt_route6_delete(&any, 0, addr, nif, RT_RTF_RA);
	}
}

/* ---- option parsing ---- */

struct nd_opts {
	const u8 *slla, *tlla, *mtu, *nonce;
	u32 total;                    /* bytes of options */
};

/* Walk the options of message m (opts at offset off). Returns 0, or -1 on
 * a zero length option (RFC 4861 4.6: discard the message). */
static int nd_parse_opts(const u8 *m, u32 len, u32 off, struct nd_opts *o)
{
	memset(o, 0, sizeof(*o));
	while (off + 2 <= len) {
		u32 ol = (u32)m[off + 1] * 8;
		if (ol == 0 || off + ol > len)
			return -1;
		switch (m[off]) {
		case ND_OPT_SLLA:
			if (!o->slla && ol >= 8)
				o->slla = m + off + 2;
			break;
		case ND_OPT_TLLA:
			if (!o->tlla && ol >= 8)
				o->tlla = m + off + 2;
			break;
		case ND_OPT_MTU:
			if (ol == 8)
				o->mtu = m + off;
			break;
		case ND_OPT_NONCE:
			if (ol >= 8)
				o->nonce = m + off + 2;
			break;
		}
		off += ol;
	}
	return off == len ? 0 : -1;
}

/* ---- input ---- */

static void ns_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len)
{
	struct nd_opts o;
	struct in6_ifaddr *ia;
	rt_in6_addr target;
	int dad = rt_in6_is_unspec(&ip6->src);

	rt_icmp6stat.InNeighborSolicits++;
	if (len < 24 || nd_parse_opts(m, len, 24, &o) < 0)
		goto bad;
	memcpy(target.b, m + 8, 16);
	if (rt_in6_is_mcast(&target))
		goto bad;
	if (dad) {
		rt_in6_addr sol;
		rt_in6_solnode(&sol, &target);
		if (!rt_in6_eq(&ip6->dst, &sol) || o.slla)
			goto bad;
	}
	ia = in6_ifa_find(nif, &target);
	if (!ia)
		return;                   /* not ours (no proxy ND) */
	if (ia->flags & RT_IN6_IFF_TENTATIVE) {
		/* RFC 4862 5.4.3; RFC 7527: our own looped back probe */
		if (dad && !(o.nonce && in6_dad_own_nonce(nif, o.nonce)))
			in6_dad_duplicate(ia);
		return;
	}
	if (ia->flags & RT_IN6_IFF_DUPLICATED)
		return;
	if (dad) {
		static const rt_in6_addr allnodes = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
		nd6_send_na(nif, &allnodes, &target, 0, 1);
		return;
	}
	if (o.slla)
		nd_learn(nif, &ip6->src, o.slla);
	nd6_send_na(nif, &ip6->src, &target, 1, 1);
	return;
bad:
	rt_icmp6stat.InErrors++;
}

static void na_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len)
{
	struct nd_opts o;
	struct nd6_entry *e;
	struct in6_ifaddr *ia;
	struct rt_buf *b;
	rt_in6_addr target;
	int r_flag, s_flag, o_flag, changed;

	rt_icmp6stat.InNeighborAdvertisements++;
	if (len < 24 || nd_parse_opts(m, len, 24, &o) < 0)
		goto bad;
	r_flag = (m[4] & 0x80) != 0;
	s_flag = (m[4] & 0x40) != 0;
	o_flag = (m[4] & 0x20) != 0;
	memcpy(target.b, m + 8, 16);
	if (rt_in6_is_mcast(&target) || (s_flag && rt_in6_is_mcast(&ip6->dst)))
		goto bad;
	ia = in6_ifa_find(nif, &target);
	if (ia) {
		if (ia->flags & RT_IN6_IFF_TENTATIVE) {
			in6_dad_duplicate(ia);
		} else if (o.tlla && memcmp(o.tlla, nif->hwaddr, 6)) {
			char buf[RT_INET6_ADDRSTRLEN];
			rt_log(RT_LOG_WARN, "ip6: %s: another node claims %s", nif->name,
			       rt_inet6_ntop(&target, buf, sizeof(buf)));
		}
		return;
	}
	e = nd_lookup(nif, &target);
	if (!e || e->state == ST_PERMANENT)
		return;
	if (e->state == ST_INCOMPLETE || e->state == ST_FAILED) {
		if (!o.tlla)
			return;
		memcpy(e->hw, o.tlla, 6);
		if (s_flag)
			nd_set_state(e, ST_REACHABLE, reachable_ms(nif));
		else
			nd_set_state(e, ST_STALE, ND_GC_STALE_MS);
		e->router = (u8)r_flag;
		while ((b = rt_bufq_dequeue(&e->pending)) != NULL)
			nd_xmit_data(e, b);
		return;
	}
	changed = o.tlla && memcmp(e->hw, o.tlla, 6) != 0;
	if (!o_flag && changed) {
		if (e->state == ST_REACHABLE)
			nd_set_state(e, ST_STALE, ND_GC_STALE_MS);
		return;
	}
	if (o.tlla)
		memcpy(e->hw, o.tlla, 6);
	if (s_flag)
		nd_set_state(e, ST_REACHABLE, reachable_ms(nif));
	else if (changed)
		nd_set_state(e, ST_STALE, ND_GC_STALE_MS);
	if (e->router && !r_flag)
		nd6_router_flag(nif, &target, 0);
	else
		e->router = (u8)r_flag;
	return;
bad:
	rt_icmp6stat.InErrors++;
}

/* RFC 8106 5.2 DNSSL: first domain becomes the search domain unless one
 * is configured. */
void rt_dns_set_search_auto(const char *domain);

static void dnssl_option(const u8 *opt, u32 ol)
{
	char name[64];
	u32 off = 8, n = 0;

	if (ol < 16 || rt_get_be32(opt + 4) == 0)
		return;
	while (off < ol && opt[off]) {
		u32 l = opt[off];
		if (l > 63 || off + 1 + l > ol || n + l + 2 > sizeof(name))
			return;
		if (n)
			name[n++] = '.';
		memcpy(name + n, opt + off + 1, l);
		n += l;
		off += 1 + l;
	}
	if (!n)
		return;
	name[n] = 0;
	rt_dns_set_search_auto(name);
}

static void ra_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct nd_opts o;
	struct nd6_entry *e;
	rt_in6_addr any;
	u32 off, lifetime, v;
	s8 pref;

	rt_icmp6stat.InRouterAdvertisements++;
	if (len < 16 || !rt_in6_is_linklocal(&ip6->src) || nd_parse_opts(m, len, 16, &o) < 0)
		goto bad;
	if (!ifp || !rt_ip6_accept_rtadv || rt_ip6_forwarding || nif->type == RT_IFT_LOOP)
		return;
	if (in6_ifa_find(nif, &ip6->src))
		return;                   /* our own */
	/* RFC 4191 2.2: 10 is reserved and treated as 00 (medium) */
	switch ((m[5] >> 3) & 3) {
	case 1: pref = 1; break;
	case 3: pref = -1; break;
	default: pref = 0; break;
	}
	if (m[4])
		ifp->cur_hlim = m[4];
	v = rt_get_be32(m + 8);
	if (v && v <= 3600000u && v != ifp->base_reachable_ms) {
		ifp->base_reachable_ms = v;
		ifp->reachable_ms = 0;
	}
	v = rt_get_be32(m + 12);
	if (v && v <= 60000u)
		ifp->retrans_ms = v;
	ifp->ra_managed = (m[5] & 0x80) != 0;
	ifp->ra_other = (m[5] & 0x40) != 0;
	if (o.slla && nif->type == RT_IFT_ETHER) {
		e = nd_learn(nif, &ip6->src, o.slla);
		if (e)
			e->router = 1;
	}
	if (o.mtu) {
		v = rt_get_be32(o.mtu + 4);
		if (v >= RT_IP6_MIN_MTU && v <= nif->mtu)
			ifp->mtu = v;
	}
	lifetime = rt_get_be16(m + 6);
	memset(&any, 0, sizeof(any));
	route6_ra_update(&any, 0, &ip6->src, nif, lifetime, pref);

	/* prefix, route information, RDNSS, DNSSL */
	for (off = 16; off + 8 <= len; off += (u32)m[off + 1] * 8) {
		const u8 *opt = m + off;
		u32 ol = (u32)opt[1] * 8;
		if (opt[0] == ND_OPT_PREFIX && ol == 32) {
			int plen = opt[2];
			u32 valid = rt_get_be32(opt + 4), prefl = rt_get_be32(opt + 8);
			rt_in6_addr pfx;
			if (plen > 128)
				continue;
			memcpy(pfx.b, opt + 16, 16);
			rt_in6_mask(&pfx, &pfx, plen);
			if (rt_in6_is_linklocal(&pfx) || rt_in6_is_mcast(&pfx))
				continue;
			if (opt[3] & 0x80)
				route6_ra_update(&pfx, plen, NULL, nif, valid, 0);
			if (opt[3] & 0x40)
				in6_slaac_prefix(nif, &pfx, plen, valid, prefl);
		} else if (opt[0] == ND_OPT_ROUTE && ol >= 8 && ol <= 24) {
			int plen = opt[2];
			s8 rp;
			rt_in6_addr pfx;
			if (plen > 128 || (plen > 64 && ol < 24) || (plen > 0 && ol < 16))
				continue;
			memset(&pfx, 0, sizeof(pfx));
			memcpy(pfx.b, opt + 8, ol - 8);
			rt_in6_mask(&pfx, &pfx, plen);
			switch ((opt[3] >> 3) & 3) {
			case 1: rp = 1; break;
			case 2: continue;       /* reserved: ignore the option */
			case 3: rp = -1; break;
			default: rp = 0; break;
			}
			route6_ra_update(&pfx, plen, &ip6->src, nif, rt_get_be32(opt + 4), rp);
		} else if (opt[0] == ND_OPT_RDNSS && ol >= 24 && ((ol / 8) & 1)) {
			u32 lt = rt_get_be32(opt + 4), k;
			for (k = 8; k + 16 <= ol; k += 16) {
				rt_in6_addr ns;
				memcpy(ns.b, opt + k, 16);
				if (lt)
					rt_dns_add_server6(&ns, nif->index, lt, RT_DNS_SRC_RA);
				else
					rt_dns_remove_server6(&ns);
			}
		} else if (opt[0] == ND_OPT_DNSSL) {
			dnssl_option(opt, ol);
		}
	}
	if ((ifp->ra_other || ifp->ra_managed) && !ifp->dhcp6_started) {
		ifp->dhcp6_started = 1;
		dhcp6_start_info(nif);
	}
	return;
bad:
	rt_icmp6stat.InErrors++;
}

static void redirect_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len)
{
	struct nd_opts o;
	rt_in6_addr target, dst;

	rt_icmp6stat.InRedirects++;
	if (len < 40 || !rt_in6_is_linklocal(&ip6->src) || nd_parse_opts(m, len, 40, &o) < 0)
		goto bad;
	memcpy(target.b, m + 8, 16);
	memcpy(dst.b, m + 24, 16);
	if (rt_in6_is_mcast(&dst) || (!rt_in6_is_linklocal(&target) && !rt_in6_eq(&target, &dst)))
		goto bad;
	route6_redirect(&dst, &target, nif, &ip6->src);
	if (o.tlla && nif->type == RT_IFT_ETHER)
		nd_learn(nif, &target, o.tlla);
	return;
bad:
	rt_icmp6stat.InErrors++;
}

void nd6_input(struct rt_netif *nif, struct rt_buf *b, const struct rt_ip6hdr *ip6,
               const u8 *m, u32 len, int hlim_ok)
{
	/* RFC 4861: hop limit 255 (not forwarded) and code 0 */
	if (!hlim_ok || m[1] != 0 || !nif->ip6) {
		rt_icmp6stat.InErrors++;
		return;
	}
	switch (m[0]) {
	case RT_ND_NEIGHBOR_SOLICIT:
		if (nif->type == RT_IFT_ETHER)
			ns_input(nif, ip6, m, len);
		break;
	case RT_ND_NEIGHBOR_ADVERT:
		if (nif->type == RT_IFT_ETHER)
			na_input(nif, ip6, m, len);
		break;
	case RT_ND_ROUTER_SOLICIT:
		rt_icmp6stat.InRouterSolicits++;
		break;              /* hosts ignore them */
	case RT_ND_ROUTER_ADVERT:
		ra_input(nif, ip6, m, len);
		break;
	case RT_ND_REDIRECT:
		redirect_input(nif, ip6, m, len);
		break;
	}
}

/* ---- administration ---- */

void rt_nd6_flush(struct rt_netif *nif, int keep_static)
{
	int i;

	for (i = 0; i < ND_HASH; i++) {
		struct nd6_entry *e = nd_tab[i], *n;
		for (; e; e = n) {
			n = e->next;
			if ((!nif || e->nif == nif) && !(keep_static && e->state == ST_PERMANENT))
				nd_destroy(e);
		}
	}
}

int rt_nd6_add_static(struct rt_netif *nif, const rt_in6_addr *a, const u8 *hw)
{
	struct nd6_entry *e;
	struct rt_buf *b;

	if (!nif || nif->type != RT_IFT_ETHER || rt_in6_is_mcast(a) || rt_in6_is_unspec(a) || (hw[0] & 1))
		return -RT_EINVAL;
	e = nd_lookup(nif, a);
	if (!e && !(e = nd_create(nif, a)))
		return -RT_ENOMEM;
	memcpy(e->hw, hw, 6);
	nd_set_state(e, ST_PERMANENT, 0);
	while ((b = rt_bufq_dequeue(&e->pending)) != NULL)
		nd_xmit_data(e, b);
	return 0;
}

int rt_nd6_delete(struct rt_netif *nif, const rt_in6_addr *a)
{
	int i, n = 0;

	for (i = 0; i < ND_HASH; i++) {
		struct nd6_entry *e = nd_tab[i], *nx;
		for (; e; e = nx) {
			nx = e->next;
			if (rt_in6_eq(&e->addr, a) && (!nif || e->nif == nif)) {
				nd_destroy(e);
				n++;
			}
		}
	}
	return n ? 0 : -RT_ENOENT;
}

int rt_nd6_list(struct rt_nd6_info *out, int max)
{
	u32 now = rt_now_ms();
	int i, n = 0;

	for (i = 0; i < ND_HASH; i++) {
		struct nd6_entry *e;
		for (e = nd_tab[i]; e; e = e->next, n++) {
			if (!out || n >= max)
				continue;
			out[n].addr = e->addr;
			memcpy(out[n].hw, e->hw, 6);
			out[n].state = e->state;
			out[n].router = e->router;
			out[n].age_s = (now - e->updated_ms) / 1000;
			out[n].ifindex = e->nif->index;
		}
	}
	return n;
}

void nd6_init(void)
{
	memset(nd_tab, 0, sizeof(nd_tab));
	nd_count = 0;
}
