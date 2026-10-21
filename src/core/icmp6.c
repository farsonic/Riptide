/*
 * Riptide - ICMPv6 (RFC 4443): echo, error generation with rate limiting,
 * error input (path MTU, transports) and dispatch of neighbour discovery
 * and MLD messages.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Rate limiting follows Linux: a per destination token bucket (one token
 * per net.inet6.icmp6.ratelimit ms, burst 6) and a global one
 * (net.inet6.icmp6.errppslimit per second). Packet too big is never
 * limited (Linux ratemask 0-1,3-127): path MTU discovery depends on it.
 */
#include "ip6_internal.h"

static s32 icmp6_ratelimit = 1000;
static s32 icmp6_errppslimit = 100;
static s32 icmp6_echo_ignore_mcast = 0;

struct rt_icmp6_stats rt_icmp6stat;

static const struct rt_sysctl icmp6_sysctl[] = {
	{ "net.inet6.icmp6.ratelimit", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp6_ratelimit, 0, 600000, NULL, "Min ms between ICMPv6 errors to one host (burst 6), 0 = off" },
	{ "net.inet6.icmp6.errppslimit", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 28, 58, 18 },
	  &icmp6_errppslimit, 0, 100000, NULL, "Global ICMPv6 error rate (per second), 0 = off" },
	{ "net.inet6.icmp6.echo_ignore_multicast", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp6_echo_ignore_mcast, 0, 1, NULL, "Do not answer echo requests sent to multicast groups" },
};

#define XRLIM_BURST 6
#define PEER_SLOTS  32

struct icmp6_peer {
	rt_in6_addr addr;
	u32 last, tokens;
};
static struct icmp6_peer peers[PEER_SLOTS];
static u32 glob_tokens, glob_last;

static int glob_allow(u32 now)
{
	u32 delta, credit;

	if (icmp6_errppslimit <= 0)
		return 1;
	delta = now - glob_last;
	if (delta > 1000)
		delta = 1000;
	credit = delta * (u32)icmp6_errppslimit / 1000;
	if (credit) {
		glob_tokens += credit;
		if (glob_tokens > (u32)icmp6_errppslimit)
			glob_tokens = (u32)icmp6_errppslimit;
		glob_last = now;
	}
	if (!glob_tokens)
		return 0;
	glob_tokens--;
	return 1;
}

static int peer_allow(const rt_in6_addr *dst, u32 now)
{
	u32 h = ((dst->w[3] ^ dst->w[2] ^ dst->w[0]) * 2654435761u) >> 27;
	struct icmp6_peer *p = &peers[h];
	u32 max, tok;

	if (icmp6_ratelimit <= 0)
		return 1;
	max = (u32)icmp6_ratelimit * XRLIM_BURST;
	if (!rt_in6_eq(&p->addr, dst)) {
		p->addr = *dst;
		p->tokens = max;
		p->last = now;
	}
	tok = p->tokens + (now - p->last);
	if (tok > max)
		tok = max;
	p->last = now;
	if (tok < (u32)icmp6_ratelimit) {
		p->tokens = tok;
		return 0;
	}
	p->tokens = tok - (u32)icmp6_ratelimit;
	return 1;
}

int icmp6_send(struct rt_netif *oif, const rt_in6_addr *src, const rt_in6_addr *dst, u8 hlim,
               const u8 *hdr, u32 hlen, const void *data, u32 len)
{
	struct rt_ip6_out o;
	struct rt_buf *b;
	rt_in6_addr s = *src;
	u8 *p;
	u16 ck;

	if (rt_in6_is_unspec(&s)) {
		struct rt_nexthop6 nh;
		if (rt_ip6_route(dst, oif, &nh) < 0 || rt_ip6_select_source(dst, nh.netif, &s) < 0) {
			rt_icmp6stat.OutErrors++;
			return -RT_EADDRNOTAVAIL;
		}
	}
	b = rt_buf_alloc(RT_BUF_HEADROOM, hlen + len);
	if (!b) {
		rt_icmp6stat.OutErrors++;
		return -RT_ENOBUFS;
	}
	p = rt_buf_put(b, hlen + len);
	memcpy(p, hdr, hlen);
	if (len)
		rt_copy_buf(p + hlen, data, len);
	p[2] = p[3] = 0;
	ck = rt_csum_fold(rt_csum_partial(p, hlen + len, rt_csum_pseudo6(&s, dst, RT_IPPROTO_ICMPV6, hlen + len)));
	memcpy(p + 2, &ck, 2);
	memset(&o, 0, sizeof(o));
	o.src = s;
	o.dst = *dst;
	o.nxt = RT_IPPROTO_ICMPV6;
	o.hlim = hlim ? (s16)hlim : -1;
	o.oif = oif;
	rt_icmp6stat.OutMsgs++;
	return rt_ip6_output(b, &o);
}

static int icmp6_is_error_type(u8 t)
{
	return t < 128;
}

void rt_icmp6_send_error(const struct rt_buf *orig, u8 type, u8 code, u32 info)
{
	const struct rt_ip6hdr *ip6;
	struct rt_netif *zone = NULL;
	struct in6_ifaddr *ia;
	rt_in6_addr src;
	u32 qlen;
	u8 hdr[8], nxt;
	int off, mc_ok;

	if (orig->len < RT_IP6_HLEN || ((uintptr_t)orig->data & 3))
		return;
	ip6 = (const struct rt_ip6hdr *)orig->data;
	/* RFC 4443 2.4 (e) */
	mc_ok = type == RT_ICMP6_PACKET_TOO_BIG ||
	        (type == RT_ICMP6_PARAM_PROB && code == RT_ICMP6_PARAMPROB_OPTION);
	if (!mc_ok && (rt_in6_is_mcast(&ip6->dst) || (orig->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))))
		return;
	if (rt_in6_is_unspec(&ip6->src) || rt_in6_is_mcast(&ip6->src) || rt_in6_is_v4mapped(&ip6->src))
		return;
	off = ip6_skip_exthdr(orig->data, orig->len, &nxt);
	if (off >= 0 && nxt == RT_IPPROTO_ICMPV6) {
		if ((u32)off + 1 > orig->len || icmp6_is_error_type(orig->data[off]) ||
		    orig->data[off] == RT_ND_REDIRECT)
			return;                 /* never about errors (and redirects) */
	}
	if (type != RT_ICMP6_PACKET_TOO_BIG && !rt_ip6_is_local(&ip6->src)) {
		u32 now = rt_now_ms();
		if (!glob_allow(now) || !peer_allow(&ip6->src, now)) {
			rt_icmp6stat.OutRateLimited++;
			return;
		}
	}
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = type;
	hdr[1] = code;
	if (type == RT_ICMP6_PACKET_TOO_BIG || type == RT_ICMP6_PARAM_PROB)
		rt_put_be32(hdr + 4, info);
	/* as much of the packet as fits in the minimum MTU */
	qlen = orig->len;
	if (qlen > RT_IP6_MIN_MTU - RT_IP6_HLEN - 8)
		qlen = RT_IP6_MIN_MTU - RT_IP6_HLEN - 8;
	/* source: the destination if it is ours (unicast), else selected */
	memset(&src, 0, sizeof(src));
	if (!rt_in6_is_mcast(&ip6->dst)) {
		struct rt_netif *own = rt_in6_addr_owner(&ip6->dst, 0);
		if (own && (ia = in6_ifa_find(own, &ip6->dst)) != NULL && !(ia->flags & RT_IN6_IFF_TENTATIVE))
			src = ip6->dst;
		else if (rt_in6_is_loopback(&ip6->dst))
			src = ip6->dst;
	}
	if (rt_in6_is_linklocal(&ip6->src) && orig->netif && orig->netif != rt_loopback_if)
		zone = orig->netif;
	switch (type) {
	case RT_ICMP6_DST_UNREACH: rt_icmp6stat.OutDestUnreachs++; break;
	case RT_ICMP6_PACKET_TOO_BIG: rt_icmp6stat.OutPktTooBigs++; break;
	case RT_ICMP6_TIME_EXCEEDED: rt_icmp6stat.OutTimeExcds++; break;
	case RT_ICMP6_PARAM_PROB: rt_icmp6stat.OutParmProblems++; break;
	}
	icmp6_send(zone, &src, &ip6->src, 0, hdr, 8, orig->data, qlen);
}

/* ---- input ---- */

static void icmp6_error_input(const u8 *m, u32 len)
{
	const struct rt_ip6hdr *inner;
	const u8 *pkt = m + 8;
	u32 plen = len - 8, info = rt_get_be32(m + 4);
	u8 nxt;
	int off;

	if (len < 8 + RT_IP6_HLEN)
		goto bad;
	if (((uintptr_t)pkt & 3) != 0)
		goto bad;                   /* cannot happen: the header is 4 aligned */
	inner = (const struct rt_ip6hdr *)(const void *)pkt;
	if ((pkt[0] >> 4) != 6)
		goto bad;
	/* we must have sent the quoted packet */
	if (!rt_in6_addr_owner(&inner->src, 1) && !rt_in6_is_loopback(&inner->src))
		goto bad;
	switch (m[0]) {
	case RT_ICMP6_DST_UNREACH:
		rt_icmp6stat.InDestUnreachs++;
		break;
	case RT_ICMP6_PACKET_TOO_BIG:
		rt_icmp6stat.InPktTooBigs++;
		rt_ip6_pmtu_update(&inner->dst, info);
		break;
	case RT_ICMP6_TIME_EXCEEDED:
		rt_icmp6stat.InTimeExcds++;
		break;
	case RT_ICMP6_PARAM_PROB:
		rt_icmp6stat.InParmProblems++;
		break;
	default:
		return;
	}
	off = ip6_skip_exthdr(pkt, plen, &nxt);
	if (off < 0 || (u32)off + 8 > plen)
		return;
	if (nxt == RT_IPPROTO_TCP)
		rt_tcp_icmp6_error(m[0], m[1], info, inner, pkt + off, plen - (u32)off);
	else if (nxt == RT_IPPROTO_UDP)
		rt_udp_icmp6_error(m[0], m[1], info, inner, pkt + off, plen - (u32)off);
	return;
bad:
	rt_icmp6stat.InErrors++;
}

void icmp6_input(struct rt_buf *b, const struct rt_ip6hdr *ip6)
{
	u8 *m = b->data;
	u32 len = b->len;
	struct rt_netif *nif = b->netif;

	rt_icmp6stat.InMsgs++;
	if (len < 4)
		goto bad;
	if (!(b->flags & (RT_BUFF_CSUM_OK | RT_BUFF_LOOP)) &&
	    rt_csum_fold(rt_csum_partial(m, len, rt_csum_pseudo6(&ip6->src, &ip6->dst, RT_IPPROTO_ICMPV6, len))) != 0)
		goto bad;
	switch (m[0]) {
	case RT_ICMP6_ECHO_REQUEST: {
		rt_in6_addr src;
		u8 hdr[8];
		rt_icmp6stat.InEchos++;
		if (len < 8)
			goto bad;
		if (rt_in6_is_mcast(&ip6->dst) && icmp6_echo_ignore_mcast)
			break;
		if (rt_in6_is_unspec(&ip6->src))
			break;
		memcpy(hdr, m, 8);
		hdr[0] = RT_ICMP6_ECHO_REPLY;
		if (rt_in6_is_mcast(&ip6->dst))
			memset(&src, 0, sizeof(src));
		else
			src = ip6->dst;
		rt_icmp6stat.OutEchoReplies++;
		icmp6_send(rt_in6_is_linklocal(&ip6->src) || rt_in6_is_mcast(&ip6->dst) ? nif : NULL,
		           &src, &ip6->src, 0, hdr, 8, m + 8, len - 8);
		break;
	}
	case RT_ICMP6_ECHO_REPLY:
		rt_icmp6stat.InEchoReplies++;
		break;                      /* raw sockets (ping6) have their copy */
	case RT_ICMP6_DST_UNREACH:
	case RT_ICMP6_PACKET_TOO_BIG:
	case RT_ICMP6_TIME_EXCEEDED:
	case RT_ICMP6_PARAM_PROB:
		icmp6_error_input(m, len);
		break;
	case RT_ICMP6_MLD_QUERY:
	case RT_ICMP6_MLD_REPORT:
	case RT_ICMP6_MLD_DONE:
	case RT_ICMP6_MLDV2_REPORT:
		if (nif)
			mld6_input(nif, ip6, m, len);
		break;
	case RT_ND_ROUTER_SOLICIT:
	case RT_ND_ROUTER_ADVERT:
	case RT_ND_NEIGHBOR_SOLICIT:
	case RT_ND_NEIGHBOR_ADVERT:
	case RT_ND_REDIRECT:
		if (nif && len >= 8)
			nd6_input(nif, b, ip6, m, len, ip6->hlim == 255);
		break;
	default:
		break;
	}
	rt_buf_free(b);
	return;
bad:
	rt_icmp6stat.InErrors++;
	rt_buf_free(b);
}

void icmp6_init(void)
{
	memset(peers, 0, sizeof(peers));
	memset(&rt_icmp6stat, 0, sizeof(rt_icmp6stat));
	glob_tokens = (u32)icmp6_errppslimit;
	glob_last = rt_now_ms();
	rt_sysctl_register(icmp6_sysctl, (int)RT_ARRAY_SIZE(icmp6_sysctl));
}
