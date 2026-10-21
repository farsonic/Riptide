/*
 * Riptide - IPv6 routing table (RFC 4861 5.2 conceptual model: prefix
 * list and default router list are routes with lifetimes here).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * One list sorted by prefix length (longest first). Kinds of entries:
 *  - connected: fe80::/64 of every IPv6 interface and the prefixes of
 *    manual addresses (RT_RTF_CONNECTED);
 *  - learned from router advertisements (RT_RTF_RA): on-link prefixes and
 *    default routes, both with lifetimes; "router preference" (RFC 4191)
 *    is kept for default router selection;
 *  - static routes (RT_RTF_STATIC) from the configuration;
 *  - redirects (RT_RTF_DYNAMIC host routes) and PMTU cache entries
 *    (RT_RTF_PMTU host routes that only carry a path MTU, as in route.c).
 */
#include "ip6_internal.h"

#define ROUTE6_MAX_DYN  128
#define RA_METRIC       1024     /* Linux: RA routes get metric 1024 */

static struct rt_route6 *r6_head;
static int r6_ndyn;

struct rt_route6 *rt_route6_list(void)
{
	return r6_head;
}

static int r6_is_dyn(const struct rt_route6 *r)
{
	return (r->flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) != 0;
}

static void r6_free(struct rt_route6 *r)
{
	if (r6_is_dyn(r))
		r6_ndyn--;
	rt_free(r);
}

static void r6_insert(struct rt_route6 *r)
{
	struct rt_route6 **pp;

	for (pp = &r6_head; *pp; pp = &(*pp)->next) {
		if ((*pp)->plen < r->plen || ((*pp)->plen == r->plen && (*pp)->metric > r->metric))
			break;
	}
	r->next = *pp;
	*pp = r;
	if (r6_is_dyn(r))
		r6_ndyn++;
}

static int r6_expired(const struct rt_route6 *r, u32 now_s)
{
	return r->expire_s && (s32)(now_s - r->expire_s) >= 0;
}

static int nif_usable(const struct rt_netif *n)
{
	return n && (n->flags & RT_IFF_UP) && n->ip6;
}

struct rt_netif *route6_onlink(const rt_in6_addr *dst)
{
	struct rt_route6 *r;
	u32 now = rt_ip6_seconds();

	for (r = r6_head; r; r = r->next) {
		if (r->flags & (RT_RTF_PMTU | RT_RTF_GATEWAY | RT_RTF_REJECT))
			continue;
		if (!nif_usable(r->netif) || r6_expired(r, now))
			continue;
		if (rt_in6_prefix_eq(dst, &r->dst, r->plen))
			return r->netif;
	}
	return NULL;
}

struct rt_route6 *route6_find(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                              const struct rt_netif *nif)
{
	struct rt_route6 *r;
	rt_in6_addr d;

	rt_in6_mask(&d, dst, plen);
	for (r = r6_head; r; r = r->next) {
		if (r->plen != plen || !rt_in6_eq(&r->dst, &d) || (r->flags & RT_RTF_PMTU))
			continue;
		if (gw && !rt_in6_is_unspec(gw) && !rt_in6_eq(&r->gateway, gw))
			continue;
		if (nif && r->netif != nif)
			continue;
		return r;
	}
	return NULL;
}

static void r6_gc(int force)
{
	struct rt_route6 **pp, *r, **oldest = NULL;
	u32 now = rt_ip6_seconds();

	for (pp = &r6_head; (r = *pp) != NULL;) {
		if (r6_expired(r, now)) {
			*pp = r->next;
			r6_free(r);
			continue;
		}
		if (r6_is_dyn(r) && (!oldest || (s32)(r->expire_s - (*oldest)->expire_s) < 0))
			oldest = pp;
		pp = &r->next;
	}
	if (force && r6_ndyn >= ROUTE6_MAX_DYN && oldest) {
		r = *oldest;
		*oldest = r->next;
		r6_free(r);
	}
}

void route6_slowtimo(u32 now_s)
{
	r6_gc(0);
}

int rt_route6_add(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw, struct rt_netif *nif,
                  u32 flags, u32 metric)
{
	struct rt_route6 *r;
	rt_in6_addr d, g;

	if (plen < 0 || plen > 128)
		return -RT_EINVAL;
	rt_in6_mask(&d, dst, plen);
	memset(&g, 0, sizeof(g));
	flags &= ~(u32)RT_RTF_GATEWAY;
	if (gw && !rt_in6_is_unspec(gw)) {
		if (rt_in6_is_mcast(gw) || rt_in6_is_v4mapped(gw))
			return -RT_EINVAL;
		g = *gw;
		if (!nif) {
			if (rt_in6_is_linklocal(gw))
				return -RT_EINVAL;         /* needs "dev IF" */
			nif = route6_onlink(gw);
			if (!nif)
				return -RT_ENETUNREACH;
		}
		flags |= RT_RTF_GATEWAY;
	} else if (!nif && !(flags & RT_RTF_REJECT)) {
		return -RT_EINVAL;
	}
	if (plen == 128)
		flags |= RT_RTF_HOST;
	for (r = r6_head; r; r = r->next)
		if (r->plen == plen && rt_in6_eq(&r->dst, &d) && rt_in6_eq(&r->gateway, &g) &&
		    r->netif == nif && !((r->flags ^ flags) & RT_RTF_PMTU))
			return -RT_EEXIST;
	if ((flags & (RT_RTF_PMTU | RT_RTF_DYNAMIC)) && r6_ndyn >= ROUTE6_MAX_DYN)
		r6_gc(1);
	r = rt_zalloc(sizeof(*r));
	if (!r)
		return -RT_ENOMEM;
	r->dst = d;
	r->plen = (u8)plen;
	r->gateway = g;
	r->netif = nif;
	r->flags = flags | RT_RTF_UP;
	r->metric = metric;
	r6_insert(r);
	return 0;
}

int rt_route6_delete(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                     struct rt_netif *nif, u32 need_flags)
{
	struct rt_route6 **pp, *r;
	rt_in6_addr d;
	int n = 0;

	rt_in6_mask(&d, dst, plen);
	for (pp = &r6_head; (r = *pp) != NULL;) {
		if (r->plen == plen && rt_in6_eq(&r->dst, &d) && !(r->flags & RT_RTF_PMTU) &&
		    (!gw || rt_in6_is_unspec(gw) || rt_in6_eq(&r->gateway, gw)) &&
		    (!nif || r->netif == nif) && (r->flags & need_flags) == need_flags) {
			*pp = r->next;
			r6_free(r);
			n++;
		} else {
			pp = &r->next;
		}
	}
	return n ? n : -RT_ESRCH;
}

void rt_route6_flush_if(struct rt_netif *nif)
{
	struct rt_route6 **pp, *r;

	for (pp = &r6_head; (r = *pp) != NULL;) {
		if (r->netif == nif) {
			*pp = r->next;
			r6_free(r);
		} else {
			pp = &r->next;
		}
	}
}

/* Learned routes go when the interface goes down. */
void rt_route6_flush_learned(struct rt_netif *nif);
void rt_route6_flush_learned(struct rt_netif *nif)
{
	struct rt_route6 **pp, *r;

	for (pp = &r6_head; (r = *pp) != NULL;) {
		if (r->netif == nif && (r->flags & (RT_RTF_RA | RT_RTF_PMTU | RT_RTF_DYNAMIC))) {
			*pp = r->next;
			r6_free(r);
		} else {
			pp = &r->next;
		}
	}
}

void route6_ra_update(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                      struct rt_netif *nif, u32 lifetime_s, s8 pref)
{
	struct rt_route6 *r = route6_find(dst, plen, gw, nif);
	u32 exp;

	if (r && !(r->flags & RT_RTF_RA))
		return;                  /* a configured route wins */
	if (lifetime_s == 0) {
		if (r)
			rt_route6_delete(dst, plen, gw, nif, RT_RTF_RA);
		return;
	}
	exp = lifetime_s == RT_IN6_INFINITE ? 0 : rt_ip6_seconds() + lifetime_s;
	if (exp == 0 && lifetime_s != RT_IN6_INFINITE)
		exp = 1;
	if (!r) {
		if (rt_route6_add(dst, plen, gw, nif, RT_RTF_RA, RA_METRIC) < 0)
			return;
		r = route6_find(dst, plen, gw, nif);
		if (!r)
			return;
	}
	r->expire_s = exp;
	r->pref = pref;
}

static struct rt_route6 *pmtu6_find(const rt_in6_addr *dst, u32 now)
{
	struct rt_route6 *r;

	for (r = r6_head; r && r->plen == 128; r = r->next)
		if ((r->flags & RT_RTF_PMTU) && rt_in6_eq(&r->dst, dst) && !r6_expired(r, now))
			return r;
	return NULL;
}

/* The first interface with IPv6 that is up, not loopback (zone default). */
static struct rt_netif *default_zone_if(void)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next)
		if (nif_usable(n) && n->type != RT_IFT_LOOP)
			return n;
	return NULL;
}

/* RFC 4861 6.3.6 / RFC 4191: compare two candidate default routes. */
static int better_default(const struct rt_route6 *a, const struct rt_route6 *b)
{
	int ra = !(a->flags & RT_RTF_GATEWAY) || nd6_router_reachable(a->netif, &a->gateway);
	int rb = !(b->flags & RT_RTF_GATEWAY) || nd6_router_reachable(b->netif, &b->gateway);

	if (a->metric != b->metric)
		return a->metric < b->metric;
	if (ra != rb)
		return ra;
	return a->pref > b->pref;
}

int rt_ip6_route(const rt_in6_addr *dst, const struct rt_netif *oif, struct rt_nexthop6 *nh)
{
	struct rt_route6 *r, *best = NULL, *pm;
	struct rt_netif *nif;
	u32 now = rt_ip6_seconds();

	memset(nh, 0, sizeof(*nh));
	if (rt_in6_is_v4mapped(dst) || rt_in6_is_unspec(dst))
		return -RT_ENETUNREACH;
	/* our own addresses (and ::1) go over loopback */
	if (rt_loopback_if && nif_usable(rt_loopback_if) &&
	    (rt_in6_is_loopback(dst) || (!rt_in6_is_mcast(dst) && rt_in6_addr_owner(dst, 1)))) {
		nh->netif = rt_loopback_if;
		nh->nexthop = *dst;
		nh->mtu = rt_ip6_link_mtu(rt_loopback_if);
		nh->flags = RT_RTF_UP | RT_RTF_HOST;
		return 0;
	}
	if (rt_in6_is_mcast(dst) || rt_in6_is_linklocal(dst)) {
		if (rt_in6_is_mcast(dst) && rt_in6_mc_scope(dst) == RT_IN6_SCOPE_IFACE)
			nif = rt_loopback_if;
		else
			nif = oif ? (struct rt_netif *)oif : default_zone_if();
		if (!nif_usable(nif))
			return oif ? -RT_ENETDOWN : -RT_ENETUNREACH;
		nh->netif = nif;
		nh->nexthop = *dst;
		nh->mtu = rt_ip6_link_mtu(nif);
		nh->flags = RT_RTF_UP;
		return 0;
	}
	for (r = r6_head; r; r = r->next) {
		if (best && r->plen < best->plen)
			break;
		if (r->flags & RT_RTF_PMTU)
			continue;
		if (!rt_in6_prefix_eq(dst, &r->dst, r->plen) || r6_expired(r, now))
			continue;
		if (r->flags & RT_RTF_REJECT) {
			if (!best)
				best = r;
			continue;
		}
		if (!nif_usable(r->netif) || (oif && r->netif != oif))
			continue;
		if (!best || (best->flags & RT_RTF_REJECT) || better_default(r, best))
			best = r;
	}
	if (!best)
		return -RT_ENETUNREACH;
	if (best->flags & RT_RTF_REJECT)
		return -RT_EHOSTUNREACH;
	best->use++;
	nh->netif = best->netif;
	nh->flags = best->flags;
	nh->nexthop = (best->flags & RT_RTF_GATEWAY) ? best->gateway : *dst;
	nh->mtu = rt_ip6_link_mtu(best->netif);
	if (best->mtu && best->mtu < nh->mtu)
		nh->mtu = best->mtu;
	pm = pmtu6_find(dst, now);
	if (pm && pm->mtu < nh->mtu)
		nh->mtu = pm->mtu;
	return 0;
}

void rt_ip6_pmtu_update(const rt_in6_addr *dst, u32 mtu)
{
	struct rt_nexthop6 nh;
	struct rt_route6 *pm;
	u32 now = rt_ip6_seconds();

	if (rt_ip6_route(dst, NULL, &nh) < 0)
		return;
	if (mtu < RT_IP6_MIN_MTU)
		mtu = RT_IP6_MIN_MTU;    /* RFC 8201 4: never below 1280 */
	if (mtu >= nh.mtu)
		return;
	pm = pmtu6_find(dst, now);
	if (!pm) {
		if (rt_route6_add(dst, 128, NULL, nh.netif, RT_RTF_PMTU | RT_RTF_HOST, 0) < 0)
			return;
		pm = pmtu6_find(dst, now);
		if (!pm)
			return;
	}
	pm->mtu = mtu;
	pm->expire_s = rt_ip6_mtu_expires > 0 ? now + (u32)rt_ip6_mtu_expires : 0;
}

/* RFC 4861 8.3: accept a redirect from our current first hop for dst. */
void route6_redirect(const rt_in6_addr *dst, const rt_in6_addr *target, struct rt_netif *nif,
                     const rt_in6_addr *from)
{
	struct rt_nexthop6 nh;
	struct rt_route6 *r;

	if (rt_ip6_forwarding || rt_ip6_route(dst, nif, &nh) < 0 || nh.netif != nif)
		return;
	if (!rt_in6_eq(&nh.nexthop, from))
		return;
	if (rt_in6_eq(target, dst)) {
		/* the destination is on-link: host route without gateway */
		rt_route6_delete(dst, 128, NULL, nif, RT_RTF_DYNAMIC);
		if (rt_route6_add(dst, 128, NULL, nif, RT_RTF_DYNAMIC | RT_RTF_HOST, 0) < 0)
			return;
	} else {
		if (!rt_in6_is_linklocal(target))
			return;
		rt_route6_delete(dst, 128, NULL, nif, RT_RTF_DYNAMIC);
		if (rt_route6_add(dst, 128, target, nif, RT_RTF_DYNAMIC | RT_RTF_HOST, 0) < 0)
			return;
	}
	r = route6_find(dst, 128, NULL, nif);
	if (r && (r->flags & RT_RTF_DYNAMIC))
		r->expire_s = rt_ip6_seconds() + (u32)(rt_ip6_mtu_expires > 0 ? rt_ip6_mtu_expires : 600);
}

void route6_init(void)
{
	while (r6_head) {
		struct rt_route6 *r = r6_head;
		r6_head = r->next;
		r6_free(r);
	}
	r6_ndyn = 0;
}
