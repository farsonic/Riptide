/*
 * Riptide - family independent glue between the transports (TCP, UDP,
 * raw) and IPv4/IPv6. Transports keep every address as rt_in6_addr,
 * IPv4 as IPv4-mapped (RFC 4291 2.5.5.2, as dual-stack BSD and Linux
 * sockets do), and use these helpers to route, checksum and send.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ip6_internal.h"

/* RFC 3493 5.3 default for IPV6_V6ONLY is off (Linux too; FreeBSD turns
 * it on): an AF_INET6 server socket also accepts IPv4 clients. */
s32 rt_ip6_v6only = 0;

static const struct rt_sysctl inet_sysctl[] = {
	{ "net.inet6.ip6.v6only", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 41, 24 },
	  &rt_ip6_v6only, 0, 1, NULL, "IPV6_V6ONLY default of new AF_INET6 sockets" },
};

/* ---- destination address selection (RFC 6724 6) ---- */

struct dest_key {
	rt_in6_addr d, s;
	int usable;
	int ds, ss;                   /* scopes */
	int dl, sl;                   /* labels */
	int prec;
};

static void dest_key(const rt_in6_addr *d, struct dest_key *k)
{
	struct rt_inet_route r;

	k->d = *d;
	k->usable = rt_inet_route(d, 0, &r) == 0 && !rt_inaddr_is_any(&r.src);
	k->s = k->usable ? r.src : rt_in6addr_any;
	k->ds = rt_in6_scope(d);
	k->ss = k->usable ? rt_in6_scope(&k->s) : -1;
	k->dl = rt_in6_label(d);
	k->sl = k->usable ? rt_in6_label(&k->s) : -1;
	k->prec = rt_in6_precedence(d);
}

/* < 0: a before b, > 0: b before a, 0: keep the order (rule 10) */
static int dest_cmp(const struct dest_key *a, const struct dest_key *b)
{
	int ca, cb;

	if (a->usable != b->usable)                              /* rule 1 */
		return a->usable ? -1 : 1;
	if ((a->ds == a->ss) != (b->ds == b->ss))                /* rule 2 */
		return a->ds == a->ss ? -1 : 1;
	if ((a->dl == a->sl) != (b->dl == b->sl))                /* rule 5 */
		return a->dl == a->sl ? -1 : 1;
	if (a->prec != b->prec)                                  /* rule 6 */
		return a->prec > b->prec ? -1 : 1;
	if (a->ds != b->ds)                                      /* rule 8 */
		return a->ds < b->ds ? -1 : 1;
	if (a->usable && !rt_in6_is_v4mapped(&a->d) && !rt_in6_is_v4mapped(&b->d)) {
		ca = rt_in6_common_prefix(&a->d, &a->s);             /* rule 9 */
		cb = rt_in6_common_prefix(&b->d, &b->s);
		if (ca > 64)
			ca = 64;                                         /* RFC 6724 2.2: up to the prefix */
		if (cb > 64)
			cb = 64;
		if (ca != cb)
			return ca > cb ? -1 : 1;
	}
	return 0;
}

void rt_inet_sort_dest(rt_in6_addr *a, int n)
{
	struct dest_key k[16], t;
	int i, j;

	if (n > 16)
		n = 16;
	for (i = 0; i < n; i++)
		dest_key(&a[i], &k[i]);
	for (i = 1; i < n; i++) {                                /* insertion sort: stable */
		t = k[i];
		for (j = i; j > 0 && dest_cmp(&t, &k[j - 1]) < 0; j--)
			k[j] = k[j - 1];
		k[j] = t;
	}
	for (i = 0; i < n; i++)
		a[i] = k[i].d;
}

int rt_inet_have_addr(int family)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next) {
		if (n->type == RT_IFT_LOOP || !(n->flags & RT_IFF_UP))
			continue;
		if (family == RT_AF_INET && n->addr)
			return 1;
		if (family == RT_AF_INET6) {
			struct rt_in6_ifaddr_info ai[8];
			int c = rt_in6_addr_list(n, ai, 8), i;
			for (i = 0; i < c && i < 8; i++)
				if (!rt_in6_is_linklocal(&ai[i].addr) && !(ai[i].flags & RT_IN6_IFF_TENTATIVE))
					return 1;
		}
	}
	return 0;
}

void rt_inet_init(void);
void rt_inet_init(void)
{
	rt_sysctl_register(inet_sysctl, (int)RT_ARRAY_SIZE(inet_sysctl));
}

int rt_inet_route(const rt_in6_addr *dst, u32 scope, struct rt_inet_route *r)
{
	memset(r, 0, sizeof(*r));
	if (rt_in6_is_v4mapped(dst)) {
		struct rt_nexthop nh;
		u32 d = rt_in6_v4(dst);
		int e = rt_ip_route(d, &nh);
		if (e < 0)
			return e;
		r->netif = nh.netif;
		r->mtu = nh.mtu;
		r->flags = nh.flags;
		r->bcast = (u8)(d == RT_INADDR_BROADCAST || rt_ip_is_broadcast(d, nh.netif));
		rt_in6_set_v4mapped(&r->src, nh.src ? nh.src : rt_ip_select_source(d));
		return 0;
	} else {
		struct rt_nexthop6 nh;
		struct rt_netif *oif = scope ? rt_netif_by_index(scope) : NULL;
		int e;
		if (scope && !oif)
			return -RT_ENXIO;
		e = rt_ip6_route(dst, oif, &nh);
		if (e < 0)
			return e;
		r->netif = nh.netif;
		r->mtu = nh.mtu;
		r->flags = nh.flags;
		if (rt_ip6_select_source(dst, nh.netif, &r->src) < 0)
			memset(&r->src, 0, sizeof(r->src));
		return 0;
	}
}

u32 rt_inet_pseudo(const rt_in6_addr *src, const rt_in6_addr *dst, u8 proto, u32 len)
{
	if (rt_in6_is_v4mapped(dst))
		return rt_csum_pseudo(rt_in6_v4(src), rt_in6_v4(dst), proto, (u16)len);
	return rt_csum_pseudo6(src, dst, proto, len);
}

int rt_inet_output(struct rt_buf *b, const struct rt_inet_out *o)
{
	if (rt_in6_is_v4mapped(&o->dst)) {
		struct rt_ip_out io;
		memset(&io, 0, sizeof(io));
		io.saddr = rt_in6_is_v4mapped(&o->src) ? rt_in6_v4(&o->src) : 0;
		io.daddr = rt_in6_v4(&o->dst);
		io.proto = o->proto;
		io.ttl = o->ttl > 0 ? (u8)o->ttl : 0;
		io.tos = o->tos;
		io.df = o->df;
		io.dontroute = o->dontroute;
		io.allow_broadcast = o->allow_broadcast;
		io.mcast_noloop = o->mcast_noloop;
		io.mcast_ttl0 = o->mcast_ttl0;
		/* scope: the interface of a multicast send (IP_MULTICAST_IF) */
		if (o->scope && rt_ip_is_multicast(io.daddr)) {
			io.oif = rt_netif_by_index(o->scope);
			if (!io.oif) {
				rt_buf_free(b);
				return -RT_ENXIO;
			}
		}
		return rt_ip_output(b, &io);
	} else {
		struct rt_ip6_out o6;
		memset(&o6, 0, sizeof(o6));
		o6.src = o->src;
		o6.dst = o->dst;
		o6.nxt = o->proto;
		o6.hlim = o->ttl > 0 ? o->ttl : -1;
		if (o6.hlim > 255)
			o6.hlim = 255;
		o6.tclass = o->tos;
		o6.dontfrag = o->df;
		o6.dontroute = o->dontroute;
		o6.mcast_noloop = o->mcast_noloop;
		o6.oif = o->scope ? rt_netif_by_index(o->scope) : NULL;
		if (o->scope && !o6.oif) {
			rt_buf_free(b);
			return -RT_ENXIO;
		}
		return rt_ip6_output(b, &o6);
	}
}

void rt_inet_pmtu_update(const rt_in6_addr *dst, u32 mtu)
{
	if (rt_in6_is_v4mapped(dst))
		rt_ip_pmtu_update(rt_in6_v4(dst), mtu);
	else
		rt_ip6_pmtu_update(dst, mtu);
}

int rt_inet_is_local(const rt_in6_addr *a)
{
	if (rt_in6_is_v4mapped(a))
		return rt_ip_is_local(rt_in6_v4(a));
	return rt_ip6_is_local(a) || rt_in6_addr_owner(a, 1) != NULL;
}

static int is_v4any(const rt_in6_addr *a)
{
	return rt_in6_is_v4mapped(a) && a->w[3] == 0;
}

int rt_inet_bind_overlap(const rt_in6_addr *a, int a_v6only, const rt_in6_addr *b, int b_v6only)
{
	if (rt_in6_is_unspec(a) || rt_in6_is_unspec(b)) {
		const rt_in6_addr *o = rt_in6_is_unspec(a) ? b : a;
		int wild_v6only = rt_in6_is_unspec(a) ? a_v6only : b_v6only;
		if (rt_in6_is_unspec(o))
			return 1;
		if (rt_in6_is_v4mapped(o))
			return !wild_v6only;
		return 1;
	}
	if (is_v4any(a) || is_v4any(b))
		return rt_in6_is_v4mapped(a) && rt_in6_is_v4mapped(b);
	return rt_in6_eq(a, b);
}

int rt_inet_laddr_match(const rt_in6_addr *la, int v6only, const rt_in6_addr *dst)
{
	if (rt_in6_eq(la, dst))
		return 2;
	if (is_v4any(la))
		return rt_in6_is_v4mapped(dst) ? 1 : 0;
	if (rt_in6_is_unspec(la))
		return rt_in6_is_v4mapped(dst) ? !v6only : 1;
	return 0;
}
