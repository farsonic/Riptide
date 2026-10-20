/*
 * Riptide - IPv4 routing table.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A single list sorted by prefix length (longest first), then metric, so
 * the first usable match is the longest-prefix match. Home and small
 * office machines have a handful of routes; a list beats a radix tree in
 * both code size and speed at that scale.
 *
 * Two kinds of dynamic host entries live in the table:
 *  - redirect routes (RT_RTF_DYNAMIC): a gateway learned from an ICMP
 *    redirect, used like any other route;
 *  - PMTU cache entries (RT_RTF_PMTU): carry only the discovered path MTU
 *    for one destination. They are never selected as the route itself,
 *    only consulted for the MTU, so routing changes cannot leave stale
 *    gateways behind (Linux keeps these as "nexthop exceptions").
 * Both expire after net.inet.ip.mtu_expires seconds.
 */
#include "infra.h"

#define ROUTE_GC_MS     60000u
#define ROUTE_MAX_DYN   128      /* dynamic entries (PMTU + redirects) */
#define PMTU_MIN        552      /* Linux ip_rt_min_pmtu: 512 + 20 + 20 */

static struct rt_route *route_head;
static struct rt_timer route_gc_timer;
static int route_ndyn;

struct rt_route *rt_route_list(void)
{
	return route_head;
}

static int prefix_len(u32 mask_n)
{
	u32 m = rt_ntohl(mask_n);
	int n = 0;

	while (m & 0x80000000u) {
		n++;
		m <<= 1;
	}
	return n;
}

static int mask_ok(u32 mask_n)
{
	u32 inv = ~rt_ntohl(mask_n);
	return (inv & (inv + 1)) == 0;
}

static void route_free(struct rt_route *r)
{
	if (r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC))
		route_ndyn--;
	rt_free(r);
}

static void route_insert(struct rt_route *r)
{
	struct rt_route **pp;
	int plen = prefix_len(r->mask);

	for (pp = &route_head; *pp; pp = &(*pp)->next) {
		int ol = prefix_len((*pp)->mask);
		if (ol < plen || (ol == plen && (*pp)->metric > r->metric))
			break;
	}
	r->next = *pp;
	*pp = r;
	if (r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC))
		route_ndyn++;
}

static int route_expired(const struct rt_route *r, u32 now)
{
	return r->expire_ms && (s32)(now - r->expire_ms) >= 0;
}

/* Drop expired dynamic entries; with 'force', also the oldest dynamic
 * entry when the table is at its limit. */
static void route_gc(int force)
{
	struct rt_route **pp, *r, **oldest = NULL;
	u32 now = rt_now_ms();

	for (pp = &route_head; (r = *pp) != NULL;) {
		if (route_expired(r, now)) {
			*pp = r->next;
			route_free(r);
			continue;
		}
		if ((r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) &&
		    (!oldest || (s32)(r->expire_ms - (*oldest)->expire_ms) < 0))
			oldest = pp;
		pp = &r->next;
	}
	if (force && route_ndyn >= ROUTE_MAX_DYN && oldest) {
		r = *oldest;
		*oldest = r->next;
		route_free(r);
	}
}

static void route_gc_fn(void *arg)
{
	route_gc(0);
	rt_timer_after(&route_gc_timer, ROUTE_GC_MS);
}

/* Interface route covering 'addr' (directly reachable), or NULL. */
static struct rt_route *route_connected_for(u32 addr)
{
	struct rt_route *r;

	for (r = route_head; r; r = r->next) {
		if (r->flags & (RT_RTF_PMTU | RT_RTF_GATEWAY | RT_RTF_REJECT))
			continue;
		if (!r->netif || !(r->netif->flags & RT_IFF_UP))
			continue;
		if ((addr & r->mask) == r->dst)
			return r;
	}
	return NULL;
}

int rt_route_add(u32 dst, u32 mask, u32 gateway, struct rt_netif *nif, u32 flags, u32 metric)
{
	struct rt_route *r;

	if (!mask_ok(mask))
		return -RT_EINVAL;
	dst &= mask;
	flags &= ~(u32)RT_RTF_GATEWAY;
	if (gateway) {
		if (rt_ip_is_multicast(gateway) || gateway == RT_INADDR_BROADCAST)
			return -RT_EINVAL;
		if (!nif) {
			struct rt_route *c = route_connected_for(gateway);
			if (!c)
				return -RT_ENETUNREACH;
			nif = c->netif;
		}
		flags |= RT_RTF_GATEWAY;
	} else if (!nif && !(flags & RT_RTF_REJECT)) {
		return -RT_EINVAL;
	}
	if (mask == 0xffffffffu)
		flags |= RT_RTF_HOST;
	for (r = route_head; r; r = r->next) {
		if (r->dst == dst && r->mask == mask && r->gateway == gateway &&
		    !((r->flags ^ flags) & RT_RTF_PMTU) && (gateway || r->netif == nif))
			return -RT_EEXIST;
	}
	if ((flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) && route_ndyn >= ROUTE_MAX_DYN)
		route_gc(1);
	r = rt_zalloc(sizeof(*r));
	if (!r)
		return -RT_ENOMEM;
	r->dst = dst;
	r->mask = mask;
	r->gateway = gateway;
	r->netif = nif;
	r->flags = flags | RT_RTF_UP;
	r->metric = metric;
	route_insert(r);
	return 0;
}

int rt_route_delete(u32 dst, u32 mask, u32 gateway)
{
	struct rt_route **pp, *r;

	dst &= mask;
	for (pp = &route_head; (r = *pp) != NULL; pp = &r->next) {
		if (r->dst == dst && r->mask == mask && !(r->flags & RT_RTF_PMTU) &&
		    (!gateway || r->gateway == gateway)) {
			*pp = r->next;
			route_free(r);
			return 0;
		}
	}
	return -RT_ESRCH;
}

static void route_flush_match(struct rt_netif *nif, u32 need_flags)
{
	struct rt_route **pp, *r;

	for (pp = &route_head; (r = *pp) != NULL;) {
		if (r->netif == nif && (r->flags & need_flags) == need_flags) {
			*pp = r->next;
			route_free(r);
		} else {
			pp = &r->next;
		}
	}
}

void rt_route_flush_if(struct rt_netif *nif)
{
	route_flush_match(nif, 0);
}

void rt_route_del_connected(struct rt_netif *nif)
{
	route_flush_match(nif, RT_RTF_CONNECTED);
}

/* Expiry time of a dynamic entry; 0 (never) if mtu_expires is 0. */
static u32 dyn_expiry(u32 now)
{
	u32 t;

	if (rt_ip_mtu_expires <= 0)
		return 0;
	t = now + (u32)rt_ip_mtu_expires * 1000u;
	return t ? t : 1;
}

static struct rt_route *pmtu_find(u32 dst, u32 now)
{
	struct rt_route *r;

	for (r = route_head; r; r = r->next) {
		if (r->mask != 0xffffffffu)
			break;           /* host routes come first */
		if ((r->flags & RT_RTF_PMTU) && r->dst == dst && !route_expired(r, now))
			return r;
	}
	return NULL;
}

int rt_ip_route(u32 dst, struct rt_nexthop *nh)
{
	struct rt_route *r, *best = NULL, *pm;
	struct rt_netif *nif;
	u32 now = rt_now_ms();

	memset(nh, 0, sizeof(*nh));

	/* Our own addresses route over the loopback interface. */
	if (rt_loopback_if && (rt_loopback_if->flags & RT_IFF_UP) &&
	    (rt_ip_is_loopback_net(dst) || rt_netif_by_addr(dst))) {
		nif = rt_netif_by_addr(dst);
		if (nif && !(nif->flags & RT_IFF_UP) && !rt_ip_is_loopback_net(dst))
			goto lookup;
		nh->netif = rt_loopback_if;
		nh->nexthop = dst;
		nh->src = rt_ip_is_loopback_net(dst) ? RT_INADDR_LOOPBACK : dst;
		nh->mtu = rt_loopback_if->mtu;
		nh->flags = RT_RTF_UP | RT_RTF_HOST;
		return 0;
	}
lookup:
	for (r = route_head; r; r = r->next) {
		if (r->flags & RT_RTF_PMTU)
			continue;
		if ((dst & r->mask) != r->dst)
			continue;
		if (route_expired(r, now))
			continue;            /* the GC timer reclaims it */
		if (r->flags & RT_RTF_REJECT) {
			best = r;
			break;
		}
		if (!r->netif || !(r->netif->flags & RT_IFF_UP))
			continue;
		best = r;
		break;
	}
	if (!best) {
		if (dst == RT_INADDR_BROADCAST) {
			/* Limited broadcast without any route: first broadcast
			 * capable interface that is up. */
			for (nif = rt_netif_list(); nif; nif = nif->next)
				if ((nif->flags & (RT_IFF_UP | RT_IFF_BROADCAST)) ==
				    (RT_IFF_UP | RT_IFF_BROADCAST))
					break;
			if (nif) {
				nh->netif = nif;
				nh->nexthop = dst;
				nh->src = nif->addr;
				nh->mtu = nif->mtu;
				nh->flags = RT_RTF_UP;
				return 0;
			}
		}
		return -RT_ENETUNREACH;
	}
	if (best->flags & RT_RTF_REJECT)
		return -RT_EHOSTUNREACH;
	best->use++;
	nif = best->netif;
	nh->netif = nif;
	nh->flags = best->flags;
	/* Limited broadcast is sent on the route's interface, never to a
	 * gateway. */
	nh->nexthop = (best->flags & RT_RTF_GATEWAY) && dst != RT_INADDR_BROADCAST ?
	              best->gateway : dst;
	nh->src = nif->addr;
	nh->mtu = nif->mtu;
	if (best->mtu && best->mtu < nh->mtu)
		nh->mtu = best->mtu;
	pm = pmtu_find(dst, now);
	if (pm && pm->mtu < nh->mtu)
		nh->mtu = pm->mtu;
	return 0;
}

u32 rt_ip_select_source(u32 dst)
{
	struct rt_nexthop nh;

	if (rt_ip_route(dst, &nh) < 0)
		return 0;
	return nh.src;
}

/* RFC 1191: remember a smaller path MTU for dst. */
void rt_ip_pmtu_update(u32 dst, u32 mtu)
{
	struct rt_route *pm;
	struct rt_nexthop nh;
	u32 now = rt_now_ms();

	if (!rt_ip_pathmtudisc || rt_ip_route(dst, &nh) < 0)
		return;
	if (mtu < PMTU_MIN)
		mtu = PMTU_MIN;
	if (mtu >= nh.mtu)
		return;                  /* only ever lower it */
	pm = pmtu_find(dst, now);
	if (!pm) {
		if (rt_route_add(dst, 0xffffffffu, 0, nh.netif, RT_RTF_PMTU | RT_RTF_HOST, 0) < 0)
			return;
		pm = pmtu_find(dst, now);
		if (!pm)
			return;
	}
	pm->mtu = mtu;
	pm->expire_ms = dyn_expiry(now);
}

/* ICMP redirect (RFC 1122 3.2.2.2): accepted only from the gateway we
 * currently use for dst, and only towards a directly connected router. */
void rt_route_redirect(u32 dst, u32 gw, u32 from)
{
	struct rt_nexthop nh;
	struct rt_route *c, *r;

	if (rt_ip_forwarding || rt_ip_route(dst, &nh) < 0)
		return;
	if (!(nh.flags & RT_RTF_GATEWAY) || nh.nexthop != from || gw == from)
		return;
	if (!gw || rt_ip_is_local(gw) || rt_ip_is_multicast(gw) || rt_ip_is_broadcast(gw, NULL))
		return;
	c = route_connected_for(gw);
	if (!c || c->netif != nh.netif)
		return;
	/* replace an older redirect for the same host */
	for (r = route_head; r; r = r->next) {
		if (r->dst == dst && r->mask == 0xffffffffu && (r->flags & RT_RTF_DYNAMIC)) {
			rt_route_delete(dst, 0xffffffffu, r->gateway);
			break;
		}
	}
	if (rt_route_add(dst, 0xffffffffu, gw, nh.netif, RT_RTF_DYNAMIC | RT_RTF_HOST, 0) < 0)
		return;
	for (r = route_head; r; r = r->next) {
		if (r->dst == dst && r->gateway == gw && (r->flags & RT_RTF_DYNAMIC)) {
			r->expire_ms = dyn_expiry(rt_now_ms());
			break;
		}
	}
}

void rt_route_init(void)
{
	while (route_head) {
		struct rt_route *r = route_head;
		route_head = r->next;
		route_free(r);
	}
	route_ndyn = 0;
	rt_timer_init(&route_gc_timer, route_gc_fn, NULL);
	rt_timer_after(&route_gc_timer, ROUTE_GC_MS);
}
