/*
 * Riptide - IPv4 input, forwarding and output (RFC 791, RFC 1122, RFC 1812).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"

s32 rt_ip_forwarding = 0;
s32 rt_ip_defttl = 64;
s32 rt_ip_sendredirects = 1;
s32 rt_ip_pathmtudisc = 1;
s32 rt_ip_mtu_expires = 600;
s32 rt_ip_portrange_first = 49152;
s32 rt_ip_portrange_last = 65535;

static const struct rt_sysctl ip_sysctl[] = {
	{ "net.inet.ip.forwarding", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 0, 1 },
	  &rt_ip_forwarding, 0, 1, NULL, "Forward IP datagrams between interfaces" },
	{ "net.inet.ip.redirect", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 0, 2 },
	  &rt_ip_sendredirects, 0, 1, NULL, "Send ICMP redirects when forwarding" },
	{ "net.inet.ip.ttl", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 0, 3 },
	  &rt_ip_defttl, 1, 255, NULL, "Default TTL of outgoing datagrams" },
	{ "net.inet.ip.pathmtudisc", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip_pathmtudisc, 0, 1, NULL, "Use path MTU discovery (RFC 1191)" },
	{ "net.inet.ip.mtu_expires", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip_mtu_expires, 0, 86400, NULL, "Lifetime of learned path MTUs and redirects (s, 0 = forever)" },
	{ "net.inet.ip.portrange.first", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip_portrange_first, 1024, 65535, NULL, "First ephemeral port" },
	{ "net.inet.ip.portrange.last", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip_portrange_last, 1024, 65535, NULL, "Last ephemeral port" },
};

/* ---- IP identification ----
 * RFC 6864 / Linux ip_idents: per destination bucket counters starting at
 * random values, so the ID sequence seen by one host reveals nothing about
 * traffic to others (no idle scans) while staying unique per destination. */
#define ID_BUCKETS 64
static u16 id_bucket[ID_BUCKETS];
static u32 id_secret;
static u16 id_global;

u16 rt_ip_select_id(u32 daddr)
{
	u32 h = (daddr ^ id_secret) * 2654435761u;
	return id_bucket[h >> 26]++;
}

u16 rt_ip_next_id(void)
{
	return id_global++;
}

static int ip_hdr_aligned(const struct rt_buf *b)
{
	return ((uintptr_t)b->data & 1) == 0;
}

/* Copy b into a fresh buffer whose data starts on an even address with
 * full headroom (68000 cannot access words at odd addresses). Frees b. */
static struct rt_buf *ip_realign(struct rt_buf *b)
{
	struct rt_buf *c = rt_buf_alloc(RT_BUF_HEADROOM, b->len);

	if (c) {
		rt_copy_buf(rt_buf_put(c, b->len), b->data, b->len);
		c->netif = b->netif;
		c->flags = b->flags;
		c->csum = b->csum;
		c->l3_proto = b->l3_proto;
		memcpy(&c->cb, &b->cb, sizeof(c->cb));
	}
	rt_buf_free(b);
	return c;
}

/* ---- output ---- */

int rt_ip_send_routed(struct rt_buf *b, const struct rt_nexthop *nh)
{
	const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;

	if (b->len > nh->mtu) {
		if (ip->frag_off & rt_htons(RT_IP_DF)) {
			rt_ipstat.FragFails++;
			rt_buf_free(b);
			return -RT_EMSGSIZE;
		}
		return rt_ipfrag_output(b, nh->netif, nh->nexthop, nh->mtu);
	}
	return rt_netif_output_ip(nh->netif, b, nh->nexthop);
}

/* SO_DONTROUTE: the destination must be on a directly attached network. */
static int route_direct(u32 dst, struct rt_nexthop *nh)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next) {
		if (!(n->flags & RT_IFF_UP) || !n->addr)
			continue;
		if ((n->type == RT_IFT_PTP && n->dstaddr == dst) ||
		    (n->netmask && (dst & n->netmask) == (n->addr & n->netmask))) {
			memset(nh, 0, sizeof(*nh));
			nh->netif = n;
			nh->nexthop = dst;
			nh->src = n->addr;
			nh->mtu = n->mtu;
			nh->flags = RT_RTF_UP;
			return 0;
		}
	}
	return -RT_ENETUNREACH;
}

static int ip_route_out(u32 daddr, struct rt_netif *oif, int dontroute, struct rt_nexthop *nh)
{
	int r;

	if (oif && (dontroute || daddr == RT_INADDR_BROADCAST || rt_ip_is_multicast(daddr) ||
	            rt_ip_is_broadcast(daddr, oif))) {
		if (!(oif->flags & RT_IFF_UP))
			return -RT_ENETDOWN;
		memset(nh, 0, sizeof(*nh));
		nh->netif = oif;
		nh->nexthop = daddr;
		nh->src = oif->addr;
		nh->mtu = oif->mtu;
		nh->flags = RT_RTF_UP;
		return 0;
	}
	if (dontroute)
		return route_direct(daddr, nh);
	r = rt_ip_route(daddr, nh);
	if (r == 0 && rt_ip_is_multicast(daddr))
		nh->nexthop = daddr;     /* multicast never goes to a gateway */
	return r;
}

int rt_ip_output(struct rt_buf *b, const struct rt_ip_out *o)
{
	struct rt_nexthop nh;
	struct rt_iphdr *ip;
	u32 saddr;
	int r;

	rt_ipstat.OutRequests++;
	if (o->daddr == RT_INADDR_ANY) {
		r = -RT_EADDRNOTAVAIL;
		goto fail;
	}
	if (o->daddr == RT_INADDR_BROADCAST && !o->allow_broadcast) {
		r = -RT_EACCES;          /* before routing: no route is no excuse */
		goto fail;
	}
	r = ip_route_out(o->daddr, o->oif, o->dontroute, &nh);
	if (r < 0) {
		rt_ipstat.OutNoRoutes++;
		goto fail;
	}
	if (rt_ip_is_broadcast(o->daddr, nh.netif)) {
		if (!o->allow_broadcast) {
			r = -RT_EACCES;
			goto fail;
		}
		nh.nexthop = o->daddr;
		/* BSD: broadcasts are never fragmented */
		if (b->len + 20 > nh.mtu) {
			r = -RT_EMSGSIZE;
			goto fail;
		}
	}
	if (b->len > 65535 - 20) {
		r = -RT_EMSGSIZE;
		goto fail;
	}
	saddr = o->saddr ? o->saddr : nh.src;
	if (rt_buf_headroom(b) < 20 || !ip_hdr_aligned(b)) {
		b = ip_realign(b);
		if (!b) {
			r = -RT_ENOBUFS;
			rt_ipstat.OutDiscards++;
			return r;
		}
	}
	ip = (struct rt_iphdr *)rt_buf_push(b, 20);
	ip->ver_ihl = 0x45;
	ip->tos = o->tos;
	ip->tot_len = rt_htons((u16)b->len);
	ip->id = rt_htons(rt_ip_select_id(o->daddr));
	ip->frag_off = o->df ? rt_htons(RT_IP_DF) : 0;
	ip->ttl = o->ttl ? o->ttl : rt_ip_is_multicast(o->daddr) ? 1 : (u8)rt_ip_defttl;
	ip->protocol = o->proto;
	ip->check = 0;
	ip->saddr = saddr;
	ip->daddr = o->daddr;
	ip->check = rt_csum_fold(rt_csum_hdr(ip, 20, 0));
	return rt_ip_send_routed(b, &nh);
fail:
	rt_ipstat.OutDiscards++;
	rt_buf_free(b);
	return r;
}

int rt_ip_output_raw(struct rt_buf *b, int dontroute)
{
	struct rt_nexthop nh;
	struct rt_iphdr *ip;
	u32 hlen;
	int r;

	rt_ipstat.OutRequests++;
	if (b->len < 20 || b->len > 65535 || !ip_hdr_aligned(b)) {
		r = -RT_EINVAL;
		goto fail;
	}
	ip = (struct rt_iphdr *)b->data;
	hlen = rt_ip_hlen(ip);
	if ((ip->ver_ihl >> 4) != 4 || hlen < 20 || hlen > b->len) {
		r = -RT_EINVAL;
		goto fail;
	}
	if (ip->daddr == RT_INADDR_ANY) {
		r = -RT_EADDRNOTAVAIL;
		goto fail;
	}
	r = ip_route_out(ip->daddr, NULL, dontroute, &nh);
	if (r < 0) {
		rt_ipstat.OutNoRoutes++;
		goto fail;
	}
	if (rt_ip_is_broadcast(ip->daddr, nh.netif))
		nh.nexthop = ip->daddr;
	/* Like Linux raw_send_hdrinc(): the stack owns length and checksum,
	 * and fills in id and source when left zero. */
	ip->tot_len = rt_htons((u16)b->len);
	if (!ip->id)
		ip->id = rt_htons(rt_ip_select_id(ip->daddr));
	if (!ip->saddr)
		ip->saddr = nh.src;
	ip->check = 0;
	ip->check = rt_csum_fold(rt_csum_hdr(ip, hlen, 0));
	return rt_ip_send_routed(b, &nh);
fail:
	rt_ipstat.OutDiscards++;
	rt_buf_free(b);
	return r;
}

/* ---- forwarding (RFC 1812 5.2) ---- */

static void ip_forward(struct rt_netif *inif, struct rt_buf *b)
{
	struct rt_iphdr *ip = (struct rt_iphdr *)b->data;
	struct rt_nexthop nh;
	u32 hlen = rt_ip_hlen(ip);

	if (ip->ttl <= 1) {
		rt_icmp_send_error(b, RT_ICMP_TIMXCEED, 0, 0);
		goto drop;
	}
	if (rt_ip_is_zeronet(ip->daddr) || rt_ip_is_loopback_net(ip->daddr) ||
	    rt_ip_is_zeronet(ip->saddr))
		goto drop;
	if (rt_ip_route(ip->daddr, &nh) < 0) {
		rt_icmp_send_error(b, RT_ICMP_UNREACH, RT_ICMP_UNREACH_NET, 0);
		goto drop;
	}
	if (nh.netif == rt_loopback_if || rt_ip_is_broadcast(ip->daddr, nh.netif))
		goto drop;               /* no directed broadcast forwarding (RFC 2644) */

	/* RFC 1812 5.2.7.2: redirect hosts on the incoming subnet that sent
	 * to us although the next hop is on the same link. */
	if (nh.netif == inif && rt_ip_sendredirects && inif->netmask &&
	    (ip->saddr & inif->netmask) == (inif->addr & inif->netmask) &&
	    (nh.nexthop & inif->netmask) == (inif->addr & inif->netmask))
		rt_icmp_send_error(b, RT_ICMP_REDIRECT, 1 /* host */, nh.nexthop);

	if (b->len > nh.mtu && (ip->frag_off & rt_htons(RT_IP_DF))) {
		rt_icmp_send_error(b, RT_ICMP_UNREACH, RT_ICMP_UNREACH_NEEDFRAG, nh.mtu);
		rt_ipstat.FragFails++;
		goto drop;
	}
	ip->ttl--;
	ip->check = 0;
	ip->check = rt_csum_fold(rt_csum_hdr(ip, hlen, 0));
	b->flags = 0;
	b->netif = NULL;
	rt_ipstat.ForwDatagrams++;
	rt_ip_send_routed(b, &nh);
	return;
drop:
	rt_buf_free(b);
}

/* ---- input ---- */

static void ip_deliver(struct rt_buf *b)
{
	const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;
	u32 hlen = rt_ip_hlen(ip);
	int nraw;

	rt_buf_pull(b, hlen);
	nraw = rt_raw_input(b, ip);
	switch (ip->protocol) {
	case RT_IPPROTO_TCP:
		rt_ipstat.InDelivers++;
		rt_tcp_input(b, ip);
		return;
	case RT_IPPROTO_UDP:
		rt_ipstat.InDelivers++;
		rt_udp_input(b, ip);
		return;
	case RT_IPPROTO_ICMP:
		rt_ipstat.InDelivers++;
		rt_icmp_input(b, ip);
		return;
	}
	if (nraw) {
		rt_ipstat.InDelivers++;
	} else {
		rt_ipstat.InUnknownProtos++;
		rt_buf_push(b, hlen);
		rt_icmp_send_error(b, RT_ICMP_UNREACH, RT_ICMP_UNREACH_PROTOCOL, 0);
	}
	rt_buf_free(b);
}

static void ip_input_body(struct rt_netif *nif, struct rt_buf *b)
{
	struct rt_iphdr *ip;
	u32 hlen, tot;
	u32 daddr, saddr;

	rt_ipstat.InReceives++;
	if (!ip_hdr_aligned(b)) {
		b = ip_realign(b);
		if (!b) {
			rt_ipstat.InDiscards++;
			return;
		}
	}
	b->netif = nif;
	if (b->len < 20)
		goto hdrerr;
	ip = (struct rt_iphdr *)b->data;
	hlen = rt_ip_hlen(ip);
	if ((ip->ver_ihl >> 4) != 4 || hlen < 20 || hlen > b->len)
		goto hdrerr;
	tot = rt_ntohs(ip->tot_len);
	if (tot < hlen || tot > b->len)
		goto hdrerr;
	if (!(b->flags & RT_BUFF_LOOP) && rt_csum_fold(rt_csum_hdr(ip, hlen, 0)) != 0)
		goto hdrerr;
	/* strip link padding; a valid header sums to zero, so a driver's
	 * RT_BUFF_CSUM_PART sum stays the transport's when it is pulled */
	rt_buf_trim(b, tot);

	saddr = ip->saddr;
	daddr = ip->daddr;
	/* Martian sources (RFC 1812 5.3.7) */
	if (rt_ip_is_multicast(saddr) || saddr == RT_INADDR_BROADCAST ||
	    (rt_ip_is_loopback_net(saddr) && !(nif->flags & RT_IFF_LOOPBACK)))
		goto addrerr;
	if (rt_ip_is_loopback_net(daddr) && !(nif->flags & RT_IFF_LOOPBACK))
		goto addrerr;

	if (rt_ip_is_multicast(daddr)) {
		/* No group membership support: only all-hosts is ours. */
		if (daddr != rt_htonl(0xe0000001u))
			goto discard;
		b->flags |= RT_BUFF_MCAST;
	} else if (!rt_ip_is_local(daddr)) {
		if (rt_ip_forwarding && !(b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))) {
			ip_forward(nif, b);
			return;
		}
		goto addrerr;
	}
	if (rt_ip_is_broadcast(daddr, NULL)) {
		rt_ipstat.InBcast++;
		b->flags |= RT_BUFF_BCAST;
	}

	if (ip->frag_off & rt_htons(RT_IP_MF | RT_IP_OFFMASK)) {
		b->flags &= (u16)~RT_BUFF_CSUM_PART;
		b = rt_ipfrag_reass(b);
		if (!b)
			return;
	}
	ip_deliver(b);
	return;

hdrerr:
	rt_ipstat.InHdrErrors++;
	RT_DBG(RT_DBG_IP, "in: header error, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
	return;
addrerr:
	rt_ipstat.InAddrErrors++;
	RT_DBG(RT_DBG_IP, "in: not for us, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
	return;
discard:
	rt_ipstat.InDiscards++;
	RT_DBG(RT_DBG_IP, "in: discarded, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
}

void rt_ip_input(struct rt_netif *nif, struct rt_buf *b)
{
	RT_PROF_ENTER(RT_PROF_IP_IN);
	ip_input_body(nif, b);
	RT_PROF_EXIT(RT_PROF_IP_IN);
}

void rt_ip_init(void)
{
	unsigned i;

	id_secret = rt_random32();
	for (i = 0; i < ID_BUCKETS; i++)
		id_bucket[i] = (u16)rt_random32();
	id_global = (u16)rt_random32();
	rt_ipfrag_init();
	rt_sysctl_register(ip_sysctl, (int)RT_ARRAY_SIZE(ip_sysctl));
}
