/*
 * Riptide - IPv6 input, output and (optional) forwarding (RFC 8200).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Input walks the extension header chain iteratively: hop-by-hop options
 * (only right after the fixed header), destination options, routing
 * headers (any routing header with segments left is refused with a
 * parameter problem, which covers RFC 5095's ban of type 0), fragments
 * (reassembled in ip6_frag.c; atomic fragments are processed in place,
 * RFC 6946) and "no next header". Unknown options follow the action bits
 * of RFC 8200 4.2; unknown upper layers get a parameter problem unless a
 * raw socket took the packet.
 *
 * Output fragments only at the source, using the path MTU from route6.c
 * (never below 1280) and random identifications (RFC 7739).
 */
#include "ip6_internal.h"

s32 rt_ip6_forwarding = 0;
s32 rt_ip6_hlim = 64;
s32 rt_ip6_accept_rtadv = 1;
s32 rt_ip6_use_tempaddr = 0;
s32 rt_ip6_auto_enable = 1;
s32 rt_ip6_dad_count = 1;
s32 rt_ip6_gratuitous_na = 1;
s32 rt_ip6_addr_gen_mode = 1;
s32 rt_ip6_mtu_expires = 600;
char rt_ip6_stable_secret[40];

struct rt_ip6_stats rt_ip6stat;

static const struct rt_sysctl ip6_sysctl[] = {
	{ "net.inet6.ip6.forwarding", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 41, 1 },
	  &rt_ip6_forwarding, 0, 1, NULL, "Forward IPv6 packets between interfaces" },
	{ "net.inet6.ip6.hlim", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 41, 3 },
	  &rt_ip6_hlim, 1, 255, NULL, "Default hop limit (a router advertisement may override it)" },
	{ "net.inet6.ip6.accept_rtadv", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 41, 12 },
	  &rt_ip6_accept_rtadv, 0, 1, NULL, "Accept router advertisements (SLAAC, default routes)" },
	{ "net.inet6.ip6.use_tempaddr", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 41, 32 },
	  &rt_ip6_use_tempaddr, 0, 2, NULL, "RFC 8981 temporary addresses: 1 create, 2 also prefer" },
	{ "net.inet6.ip6.enable", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip6_auto_enable, 0, 1, NULL, "Enable IPv6 on interfaces when they come up" },
	{ "net.inet6.ip6.dad_count", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip6_dad_count, 0, 10, NULL, "Duplicate address detection probes (0 = off)" },
	{ "net.inet6.ip6.gratuitous_na", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip6_gratuitous_na, 0, 1, NULL, "Announce new global addresses to routers (RFC 9131)" },
	{ "net.inet6.ip6.addr_gen_mode", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip6_addr_gen_mode, 0, 1, NULL, "Interface identifiers: 0 EUI-64, 1 RFC 7217 stable privacy" },
	{ "net.inet6.ip6.stable_secret", RT_SYSCTL_STRING, RT_SYSCTL_RW, 0, { 0 },
	  rt_ip6_stable_secret, 0, sizeof(rt_ip6_stable_secret), NULL, "Secret for RFC 7217 identifiers" },
	{ "net.inet6.ip6.mtu_expires", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ip6_mtu_expires, 0, 86400, NULL, "Lifetime of learned path MTUs and redirects (s, 0 = forever)" },
};

/* ---- seconds clock for lifetimes ---- */
static u32 secs_last_ms, secs_acc_ms, secs_now = 1;

u32 rt_ip6_seconds(void)
{
	u32 now = rt_now_ms();
	u32 d = now - secs_last_ms;

	secs_last_ms = now;
	secs_acc_ms += d;
	if (secs_acc_ms >= 1000) {
		secs_now += secs_acc_ms / 1000;
		secs_acc_ms %= 1000;
	}
	return secs_now;
}

static struct rt_timer slow_timer;

static void slowtimo(void *arg)
{
	u32 now = rt_ip6_seconds();

	ip6_if_slowtimo(now);
	route6_slowtimo(now);
	rt_timer_after(&slow_timer, 1000);
}

u32 rt_csum_pseudo6(const rt_in6_addr *src, const rt_in6_addr *dst, u8 nxt, u32 len)
{
	u32 s = rt_csum_partial(src->b, 16, 0);
	u8 tail[8];

	s = rt_csum_partial(dst->b, 16, s);
	rt_put_be32(tail, len);
	tail[4] = tail[5] = tail[6] = 0;
	tail[7] = nxt;
	return rt_csum_partial(tail, 8, s);
}

struct rt_buf *ip6_realign(struct rt_buf *b)
{
	struct rt_buf *c = rt_buf_alloc(RT_BUF_HEADROOM, b->len);

	if (c) {
		rt_copy_buf(rt_buf_put(c, b->len), b->data, b->len);
		c->netif = b->netif;
		c->flags = b->flags;
		c->l3_proto = b->l3_proto;
		memcpy(&c->cb, &b->cb, sizeof(c->cb));
	}
	rt_buf_free(b);
	return c;
}

/* ---- link output ---- */

int ip6_send_link(struct rt_netif *nif, struct rt_buf *b, const rt_in6_addr *nexthop)
{
	u8 mac[6];
	int r;

	if (!(nif->flags & RT_IFF_UP)) {
		nif->stats.oerrors++;
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	if (nif->type == RT_IFT_ETHER) {
		if (!rt_in6_is_mcast(nexthop))
			return nd6_resolve_output(nif, b, nexthop);
		rt_in6_mc_mac(mac, nexthop);
		nif->stats.omcasts++;
		rt_ip6stat.OutMcastPkts++;
		r = rt_netif_xmit(nif, b, mac, RT_ETH_P_IPV6);
	} else {
		r = rt_netif_xmit(nif, b, NULL, RT_ETH_P_IPV6);
	}
	if (r < 0)
		nif->stats.oerrors++;
	return r;
}

/* Fixed header (+ hop-by-hop Router Alert) in front of b->data. */
static struct rt_buf *ip6_push_hdr(struct rt_buf *b, const rt_in6_addr *src, const rt_in6_addr *dst,
                                   u8 nxt, u8 hlim, u8 tclass, u32 flow, int ra)
{
	struct rt_ip6hdr *ip6;
	u32 need = RT_IP6_HLEN + (ra ? 8 : 0);

	if (rt_buf_headroom(b) < need + 16 || ((uintptr_t)(b->data - need) & 3)) {
		b = ip6_realign(b);
		if (!b)
			return NULL;
	}
	if (ra) {
		u8 *h = rt_buf_push(b, 8);
		h[0] = nxt;
		h[1] = 0;
		h[2] = 5;                  /* Router Alert (RFC 2711), MLD */
		h[3] = 2;
		h[4] = 0;
		h[5] = 0;
		h[6] = 1;                  /* PadN */
		h[7] = 0;
		nxt = RT_IPPROTO_HOPOPTS;
	}
	ip6 = (struct rt_ip6hdr *)rt_buf_push(b, RT_IP6_HLEN);
	ip6->vtcfl = rt_htonl(0x60000000u | ((u32)tclass << 20) | (flow & 0xfffffu));
	ip6->plen = rt_htons((u16)(b->len - RT_IP6_HLEN));
	ip6->nxt = nxt;
	ip6->hlim = hlim;
	ip6->src = *src;
	ip6->dst = *dst;
	return b;
}

int ip6_output_direct(struct rt_buf *b, struct rt_netif *nif, const rt_in6_addr *src,
                      const rt_in6_addr *dst, u8 nxt, u8 hlim, int router_alert)
{
	rt_ip6stat.OutRequests++;
	b = ip6_push_hdr(b, src, dst, nxt, hlim, 0, 0, router_alert);
	if (!b) {
		rt_ip6stat.OutDiscards++;
		return -RT_ENOBUFS;
	}
	return ip6_send_link(nif, b, dst);
}

int rt_ip6_output(struct rt_buf *b, const struct rt_ip6_out *o)
{
	struct rt_nexthop6 nh;
	rt_in6_addr src;
	u32 unfrag;
	u8 hlim;
	int r;

	rt_ip6stat.OutRequests++;
	if (rt_in6_is_unspec(&o->dst) || rt_in6_is_v4mapped(&o->dst)) {
		r = -RT_EADDRNOTAVAIL;
		goto fail;
	}
	if (o->dontroute) {
		struct rt_netif *nif = o->oif;
		if (!nif && !rt_in6_is_linklocal(&o->dst) && !rt_in6_is_mcast(&o->dst))
			nif = route6_onlink(&o->dst);
		if (!nif) {
			if (rt_ip6_route(&o->dst, NULL, &nh) < 0 || (nh.flags & RT_RTF_GATEWAY)) {
				r = -RT_ENETUNREACH;
				goto noroute;
			}
		} else {
			memset(&nh, 0, sizeof(nh));
			nh.netif = nif;
			nh.nexthop = o->dst;
			nh.mtu = rt_ip6_link_mtu(nif);
		}
	} else {
		r = rt_ip6_route(&o->dst, o->oif, &nh);
		if (r < 0)
			goto noroute;
	}
	src = o->src;
	if (rt_in6_is_unspec(&src) && rt_ip6_select_source(&o->dst, nh.netif, &src) < 0) {
		r = -RT_EADDRNOTAVAIL;
		goto fail;
	}
	if (b->len > 65535 - (o->router_alert ? 8 : 0)) {
		r = -RT_EMSGSIZE;
		goto fail;
	}
	if (o->hlim >= 0)
		hlim = (u8)o->hlim;
	else
		hlim = rt_in6_is_mcast(&o->dst) ? 1 : ip6_default_hlim(nh.netif);
	b = ip6_push_hdr(b, &src, &o->dst, o->nxt, hlim, o->tclass, o->flowlabel, o->router_alert);
	if (!b) {
		rt_ip6stat.OutDiscards++;
		return -RT_ENOBUFS;
	}
	if (b->len > nh.mtu) {
		if (o->dontfrag) {
			rt_ip6stat.FragFails++;
			rt_buf_free(b);
			return -RT_EMSGSIZE;
		}
		unfrag = RT_IP6_HLEN + (o->router_alert ? 8 : 0);
		return ip6frag_output(b, unfrag, nh.netif, &nh.nexthop, nh.mtu);
	}
	return ip6_send_link(nh.netif, b, &nh.nexthop);
noroute:
	rt_ip6stat.OutNoRoutes++;
fail:
	rt_ip6stat.OutDiscards++;
	rt_buf_free(b);
	return r;
}

/* ---- extension headers ---- */

int ip6_skip_exthdr(const u8 *pkt, u32 len, u8 *nxt)
{
	u32 off = RT_IP6_HLEN;
	u8 n;
	int hops = 0;

	if (len < RT_IP6_HLEN)
		return -1;
	n = pkt[6];
	for (;;) {
		u32 hl;
		switch (n) {
		case RT_IPPROTO_HOPOPTS:
		case RT_IPPROTO_ROUTING:
		case RT_IPPROTO_DSTOPTS:
			if (off + 8 > len)
				return -1;
			hl = ((u32)pkt[off + 1] + 1) * 8;
			break;
		case RT_IPPROTO_FRAGMENT:
			if (off + 8 > len)
				return -1;
			if (rt_get_be16(pkt + off + 2) & 0xfff8)
				return -1;      /* not the first fragment */
			hl = 8;
			break;
		case 51:                    /* AH (RFC 4302) */
			if (off + 8 > len)
				return -1;
			hl = ((u32)pkt[off + 1] + 2) * 4;
			break;
		default:
			*nxt = n;
			return (int)off;
		}
		if (++hops > 16)
			return -1;
		n = pkt[off];
		off += hl;
		if (off > len)
			return -1;
	}
}

/* Hop-by-hop / destination options (RFC 8200 4.2). 0 continue, -1 the
 * packet was dropped (and an error sent where the option says so). */
static int ip6_opts(struct rt_buf *b, u32 start, u32 end)
{
	const u8 *d = b->data;
	const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)d;
	u32 i = start;

	while (i < end) {
		u8 t = d[i];
		u32 l;

		if (t == 0) {               /* Pad1 */
			i++;
			continue;
		}
		if (i + 2 > end || i + 2 + d[i + 1] > end) {
			rt_ip6stat.InHdrErrors++;
			rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_HEADER, i);
			return -1;
		}
		l = d[i + 1];
		switch (t) {
		case 1:                     /* PadN */
		case 5:                     /* Router Alert */
			break;
		default:
			switch (t >> 6) {
			case 0:
				break;
			case 1:
				return -1;
			case 3:
				if (rt_in6_is_mcast(&ip6->dst))
					return -1;
				/* fall through */
			case 2:
				rt_ip6stat.InHdrErrors++;
				rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_OPTION, i);
				return -1;
			}
		}
		i += 2 + l;
	}
	return 0;
}

static void ip6_forward(struct rt_netif *inif, struct rt_buf *b)
{
	struct rt_ip6hdr *ip6 = (struct rt_ip6hdr *)b->data;
	struct rt_nexthop6 nh;

	if (rt_in6_is_linklocal(&ip6->src) || rt_in6_is_linklocal(&ip6->dst) ||
	    rt_in6_is_mcast(&ip6->dst) || (b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)))
		goto drop;
	if (ip6->hlim <= 1) {
		rt_icmp6_send_error(b, RT_ICMP6_TIME_EXCEEDED, RT_ICMP6_TIME_EXCEED_TRANSIT, 0);
		goto drop;
	}
	if (rt_ip6_route(&ip6->dst, NULL, &nh) < 0) {
		rt_ip6stat.InNoRoutes++;
		rt_icmp6_send_error(b, RT_ICMP6_DST_UNREACH, RT_ICMP6_DST_UNREACH_NOROUTE, 0);
		goto drop;
	}
	if (nh.netif == rt_loopback_if)
		goto drop;
	if (b->len > nh.mtu) {
		rt_ip6stat.InTooBigErrors++;
		rt_icmp6_send_error(b, RT_ICMP6_PACKET_TOO_BIG, 0, nh.mtu);
		goto drop;
	}
	ip6->hlim--;
	b->flags = 0;
	b->netif = NULL;
	rt_ip6stat.OutForwDatagrams++;
	ip6_send_link(nh.netif, b, &nh.nexthop);
	return;
drop:
	rt_buf_free(b);
}

static void ip6_deliver(struct rt_buf *b, int tentative)
{
	const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
	u32 off = RT_IP6_HLEN, prev = 6;
	u8 nxt = ip6->nxt;
	int hops = 0, nraw;

	for (;;) {
		const u8 *p = b->data + off;
		u32 hl;

		if (++hops > 32)
			goto hdrerr;
		switch (nxt) {
		case RT_IPPROTO_HOPOPTS:
		case RT_IPPROTO_DSTOPTS:
			if (nxt == RT_IPPROTO_HOPOPTS && off != RT_IP6_HLEN) {
				rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_NEXTHEADER, prev);
				goto hdrerr;
			}
			if (off + 8 > b->len)
				goto truncated;
			hl = ((u32)p[1] + 1) * 8;
			if (off + hl > b->len)
				goto truncated;
			if (ip6_opts(b, off + 2, off + hl) < 0)
				goto drop;
			break;
		case RT_IPPROTO_ROUTING:
			if (off + 8 > b->len)
				goto truncated;
			hl = ((u32)p[1] + 1) * 8;
			if (off + hl > b->len)
				goto truncated;
			if (p[3] != 0) {
				/* a host: no routing header type is processed
				 * (RFC 8200 4.4, RFC 5095 for type 0) */
				rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_HEADER, off + 2);
				goto hdrerr;
			}
			break;
		case RT_IPPROTO_FRAGMENT:
			if (off + 8 > b->len)
				goto truncated;
			if ((rt_get_be16(p + 2) & 0xfff9) == 0) {
				hl = 8;             /* atomic fragment (RFC 6946) */
				break;
			}
			b->flags &= (u16)~RT_BUFF_CSUM_PART;
			b = ip6frag_reass(b, off, prev);
			if (!b)
				return;
			ip6 = (const struct rt_ip6hdr *)b->data;
			nxt = b->data[prev];
			continue;               /* the fragment header is gone */
		case RT_IPPROTO_NONE:
			goto drop;
		default:
			goto upper;
		}
		nxt = p[0];
		prev = off;
		off += hl;
	}
upper:
	if (tentative) {
		/* RFC 4862 5.4: only ND for a tentative address */
		if (nxt != RT_IPPROTO_ICMPV6 || off + 1 > b->len ||
		    (b->data[off] != RT_ND_NEIGHBOR_SOLICIT && b->data[off] != RT_ND_NEIGHBOR_ADVERT))
			goto drop;
	}
	if (b->flags & RT_BUFF_CSUM_PART)    /* subtract the headers' sum */
		b->csum = rt_csum_add(b->csum, rt_csum_fold(rt_csum_partial(b->data, off, 0)));
	rt_buf_pull(b, off);
	nraw = rt_raw6_input(b, ip6, nxt);
	switch (nxt) {
	case RT_IPPROTO_TCP:
		rt_ip6stat.InDelivers++;
		rt_tcp_input6(b, ip6);
		return;
	case RT_IPPROTO_UDP:
		rt_ip6stat.InDelivers++;
		rt_udp_input6(b, ip6);
		return;
	case RT_IPPROTO_ICMPV6:
		rt_ip6stat.InDelivers++;
		icmp6_input(b, ip6);
		return;
	}
	if (nraw) {
		rt_ip6stat.InDelivers++;
	} else {
		rt_ip6stat.InUnknownProtos++;
		rt_buf_push(b, off);
		rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_NEXTHEADER, prev);
	}
	rt_buf_free(b);
	return;
truncated:
	rt_ip6stat.InTruncatedPkts++;
	rt_buf_free(b);
	return;
hdrerr:
	rt_ip6stat.InHdrErrors++;
drop:
	rt_buf_free(b);
}

static void ip6_input_body(struct rt_netif *nif, struct rt_buf *b)
{
	struct rt_ip6hdr *ip6;
	struct rt_netif *owner;
	u32 plen;
	int tentative = 0;

	rt_ip6stat.InReceives++;
	if (!nif->ip6) {
		rt_ip6stat.InDiscards++;
		nif->stats.noproto++;
		rt_buf_free(b);
		return;
	}
	if ((uintptr_t)b->data & 3) {
		b = ip6_realign(b);
		if (!b) {
			rt_ip6stat.InDiscards++;
			return;
		}
	}
	b->netif = nif;
	if (b->len < RT_IP6_HLEN)
		goto hdrerr;
	ip6 = (struct rt_ip6hdr *)b->data;
	if ((b->data[0] >> 4) != 6)
		goto hdrerr;
	plen = rt_ntohs(ip6->plen);
	if (plen == 0 && ip6->nxt == RT_IPPROTO_HOPOPTS)
		goto hdrerr;                /* jumbograms are not supported */
	if (plen + RT_IP6_HLEN > b->len) {
		rt_ip6stat.InTruncatedPkts++;
		rt_buf_free(b);
		return;
	}
	rt_buf_trim(b, plen + RT_IP6_HLEN);

	/* addresses that never appear on a link (RFC 4291 2.5.2, 2.5.3, 2.7) */
	if (rt_in6_is_mcast(&ip6->src) || rt_in6_is_v4mapped(&ip6->src) || rt_in6_is_v4mapped(&ip6->dst) ||
	    rt_in6_is_unspec(&ip6->dst))
		goto addrerr;
	if (!(nif->flags & RT_IFF_LOOPBACK) &&
	    (rt_in6_is_loopback(&ip6->src) || rt_in6_is_loopback(&ip6->dst)))
		goto addrerr;

	if (rt_in6_is_mcast(&ip6->dst)) {
		int sc = rt_in6_mc_scope(&ip6->dst);
		if (sc == 0 || (sc == RT_IN6_SCOPE_IFACE && !(nif->flags & RT_IFF_LOOPBACK)))
			goto addrerr;
		if (!rt_ip6_mc_joined(nif, &ip6->dst))
			goto discard;
		rt_ip6stat.InMcastPkts++;
		b->flags |= RT_BUFF_MCAST;
	} else {
		owner = rt_in6_addr_owner(&ip6->dst, 1);
		if (!owner && !rt_in6_is_loopback(&ip6->dst)) {
			if (rt_ip6_forwarding && !(b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))) {
				ip6_forward(nif, b);
				return;
			}
			goto addrerr;
		}
		if (owner) {
			struct in6_ifaddr *ia = in6_ifa_find(owner, &ip6->dst);
			if (ia && (ia->flags & RT_IN6_IFF_DUPLICATED))
				goto addrerr;
			if (ia && (ia->flags & RT_IN6_IFF_TENTATIVE))
				tentative = 1;
			/* link-local addresses belong to their link */
			if (rt_in6_is_linklocal(&ip6->dst) && owner != nif && !(nif->flags & RT_IFF_LOOPBACK))
				goto addrerr;
		}
	}
	ip6_deliver(b, tentative);
	return;
hdrerr:
	rt_ip6stat.InHdrErrors++;
	RT_DBG(RT_DBG_IP6, "in: header error, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
	return;
addrerr:
	rt_ip6stat.InAddrErrors++;
	RT_DBG(RT_DBG_IP6, "in: not for us, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
	return;
discard:
	rt_ip6stat.InDiscards++;
	RT_DBG(RT_DBG_IP6, "in: discarded, %lu bytes from %s", (unsigned long)b->len, nif->name);
	rt_buf_free(b);
}

void rt_ip6_input(struct rt_netif *nif, struct rt_buf *b)
{
	RT_PROF_ENTER(RT_PROF_IP6_IN);
	ip6_input_body(nif, b);
	RT_PROF_EXIT(RT_PROF_IP6_IN);
}

void rt_ip6_init(void)
{
	memset(&rt_ip6stat, 0, sizeof(rt_ip6stat));
	secs_last_ms = rt_now_ms();
	secs_acc_ms = 0;
	ip6_if_init();
	nd6_init();
	route6_init();
	ip6frag_init();
	icmp6_init();
	dhcp6_init();
	rt_sysctl_register(ip6_sysctl, (int)RT_ARRAY_SIZE(ip6_sysctl));
	rt_timer_init(&slow_timer, slowtimo, NULL);
	rt_timer_after(&slow_timer, 1000);
}
