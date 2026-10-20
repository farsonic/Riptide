/*
 * Riptide - ICMP (RFC 792, RFC 1122 3.2.2, RFC 1812 4.3).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Rate limiting follows Linux: a global token bucket (icmp_msgs_per_sec,
 * burst icmp_msgs_burst) plus a per-destination bucket refilling one
 * token every icmp_ratelimit ms with a burst of 6 (inet_peer_xrlim_allow).
 * Only the types of Linux's default icmp_ratemask are limited
 * (unreachable, source quench, time exceeded, parameter problem), and never
 * "fragmentation needed", which PMTU discovery depends on.
 */
#include "infra.h"

static s32 icmp_bmcastecho = 0;
static s32 icmp_maskrepl = 0;
static s32 icmp_ratelimit = 1000;      /* ms per token, per destination */
static s32 icmp_msgs_per_sec = 1000;
static s32 icmp_msgs_burst = 50;
static s32 icmp_drop_redirect = 0;

static const struct rt_sysctl icmp_sysctl[] = {
	{ "net.inet.icmp.maskrepl", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 1, 1 },
	  &icmp_maskrepl, 0, 1, NULL, "Answer ICMP address mask requests" },
	{ "net.inet.icmp.bmcastecho", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp_bmcastecho, 0, 1, NULL, "Answer broadcast/multicast echo requests" },
	{ "net.inet.icmp.ratelimit", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp_ratelimit, 0, 600000, NULL, "Min ms between ICMP errors to one host (burst 6), 0 = off" },
	{ "net.inet.icmp.msgs_per_sec", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp_msgs_per_sec, 0, 100000, NULL, "Global ICMP error rate limit, 0 = off" },
	{ "net.inet.icmp.msgs_burst", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp_msgs_burst, 1, 100000, NULL, "Global ICMP error burst" },
	{ "net.inet.icmp.drop_redirect", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &icmp_drop_redirect, 0, 1, NULL, "Ignore ICMP redirects" },
};

/* ---- rate limiting ---- */

#define XRLIM_BURST 6
#define PEER_SLOTS  32

struct icmp_peer {
	u32 addr;
	u32 last;          /* ms */
	u32 tokens;        /* in ms of credit */
};
static struct icmp_peer icmp_peers[PEER_SLOTS];
static u32 glob_tokens, glob_last;

static int icmp_global_allow(u32 now)
{
	u32 delta, credit;

	if (icmp_msgs_per_sec <= 0)
		return 1;
	delta = now - glob_last;
	if (delta > 1000)
		delta = 1000;
	credit = delta * (u32)icmp_msgs_per_sec / 1000;
	if (credit) {
		glob_tokens += credit;
		if (glob_tokens > (u32)icmp_msgs_burst)
			glob_tokens = (u32)icmp_msgs_burst;
		glob_last = now;
	}
	if (!glob_tokens)
		return 0;
	glob_tokens--;
	return 1;
}

static int icmp_peer_allow(u32 dst, u32 now)
{
	u32 h = (dst * 2654435761u) >> 27;    /* 32 slots */
	struct icmp_peer *p = &icmp_peers[h];
	u32 max, tok;

	if (icmp_ratelimit <= 0)
		return 1;
	max = (u32)icmp_ratelimit * XRLIM_BURST;
	if (p->addr != dst) {
		/* new (or evicted) peer starts with a full bucket */
		p->addr = dst;
		p->tokens = max;
		p->last = now;
	}
	tok = p->tokens + (now - p->last);
	if (tok > max)
		tok = max;
	p->last = now;
	if (tok < (u32)icmp_ratelimit) {
		p->tokens = tok;
		return 0;
	}
	p->tokens = tok - (u32)icmp_ratelimit;
	return 1;
}

static int icmp_ratelimited(u8 type, u8 code, u32 dst)
{
	u32 now = rt_now_ms();

	if (!(type == RT_ICMP_UNREACH || type == RT_ICMP_SOURCEQUENCH ||
	      type == RT_ICMP_TIMXCEED || type == RT_ICMP_PARAMPROB))
		return 0;
	if (type == RT_ICMP_UNREACH && code == RT_ICMP_UNREACH_NEEDFRAG)
		return 0;
	if (rt_ip_is_local(dst))
		return 0;               /* no limit towards ourselves (Linux) */
	return !icmp_global_allow(now) || !icmp_peer_allow(dst, now);
}

static int icmp_is_error(u8 type)
{
	return type == RT_ICMP_UNREACH || type == RT_ICMP_SOURCEQUENCH ||
	       type == RT_ICMP_REDIRECT || type == RT_ICMP_TIMXCEED ||
	       type == RT_ICMP_PARAMPROB;
}

/* ---- output ---- */

/* Send an ICMP message: 'hdr' (8 bytes: type, code, 0, rest) followed by
 * 'len' bytes of 'data'. */
static void icmp_send(u32 saddr, u32 daddr, u8 tos, const u8 *hdr, const void *data, u32 len)
{
	struct rt_buf *b;
	struct rt_ip_out o;
	u8 *p;

	b = rt_buf_alloc(RT_BUF_HEADROOM, 8 + len);
	if (!b) {
		rt_icmpstat.OutErrors++;
		return;
	}
	p = rt_buf_put(b, 8 + len);
	memcpy(p, hdr, 8);
	memcpy(p + 8, data, len);
	p[2] = p[3] = 0;
	{
		u16 ck = rt_inet_csum(p, 8 + len);
		memcpy(p + 2, &ck, 2);
	}
	memset(&o, 0, sizeof(o));
	o.saddr = saddr;
	o.daddr = daddr;
	o.proto = RT_IPPROTO_ICMP;
	o.tos = tos;
	rt_icmpstat.OutMsgs++;
	if (hdr[0] == RT_ICMP_UNREACH)
		rt_icmpstat.OutDestUnreachs++;
	else if (hdr[0] == RT_ICMP_ECHOREPLY)
		rt_icmpstat.OutEchoReps++;
	if (rt_ip_output(b, &o) < 0)
		rt_icmpstat.OutErrors++;
}

void rt_icmp_send_error(const struct rt_buf *orig, u8 type, u8 code, u32 info)
{
	const struct rt_iphdr *ip;
	u32 hlen, tot, qlen, saddr;
	u8 hdr[8];

	if (orig->len < 20 || ((uintptr_t)orig->data & 1))
		return;
	ip = (const struct rt_iphdr *)orig->data;
	hlen = rt_ip_hlen(ip);
	tot = rt_ntohs(ip->tot_len);
	if (hlen < 20 || hlen > orig->len)
		return;
	if (tot > orig->len)
		tot = orig->len;
	if (tot < hlen)
		return;

	/* RFC 1122 3.2.2 / RFC 1812 4.3.2.7: never about ... */
	if (orig->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))
		return;                                  /* link broadcasts */
	if (rt_ip_is_multicast(ip->daddr) || ip->daddr == RT_INADDR_BROADCAST ||
	    (orig->netif && rt_ip_is_broadcast(ip->daddr, orig->netif)))
		return;                                  /* IP broadcasts */
	if (ip->frag_off & rt_htons(RT_IP_OFFMASK))
		return;                                  /* non-initial fragments */
	if (ip->saddr == 0 || rt_ip_is_multicast(ip->saddr) || rt_ip_is_broadcast(ip->saddr, NULL) ||
	    rt_ip_is_zeronet(ip->saddr))
		return;                                  /* no unique source */
	if (ip->protocol == RT_IPPROTO_ICMP) {
		const u8 *ic = orig->data + hlen;
		if (tot < hlen + 1 || icmp_is_error(ic[0]) || ic[0] > RT_ICMP_MASKREPLY)
			return;                          /* ICMP errors / unknown */
	}
	if (icmp_ratelimited(type, code, ip->saddr)) {
		rt_icmpstat.OutRateLimited++;
		return;
	}

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = type;
	hdr[1] = code;
	if (type == RT_ICMP_UNREACH && code == RT_ICMP_UNREACH_NEEDFRAG)
		rt_put_be16(hdr + 6, (u16)info);         /* RFC 1191 next-hop MTU */
	else if (type == RT_ICMP_PARAMPROB)
		hdr[4] = (u8)info;                       /* pointer */
	else if (type == RT_ICMP_REDIRECT)
		memcpy(hdr + 4, &info, 4);               /* gateway, network order */

	/* Quote as much as fits in 576 bytes (RFC 1812 4.3.2.3). */
	qlen = tot;
	if (qlen > 576 - 20 - 8)
		qlen = 576 - 20 - 8;
	/* Source: the address the offending datagram was sent to if it is
	 * ours, else the outgoing interface's (route chosen). */
	saddr = rt_netif_by_addr(ip->daddr) || rt_ip_is_loopback_net(ip->daddr) ? ip->daddr : 0;
	icmp_send(saddr, ip->saddr, (u8)((ip->tos & 0x1e) | 0xc0), hdr, orig->data, qlen);
}

/* ---- input ---- */

/* RFC 1191 7.1: when a router sends no MTU, guess the next plateau below
 * the size of the datagram that did not fit. */
static u32 mtu_plateau(u32 len)
{
	static const u16 plateaus[] = { 32000, 17914, 8166, 4352, 2002, 1492, 1006, 508, 296, 68 };
	unsigned i;

	for (i = 0; i < RT_ARRAY_SIZE(plateaus); i++)
		if (plateaus[i] < len)
			return plateaus[i];
	return 68;
}

static void icmp_error_input(const u8 *ic, u32 len, u32 from)
{
	const struct rt_iphdr *inner;
	u8 type = ic[0], code = ic[1];
	u32 ihlen, info = 0;
	u32 inner_len = len - 8;

	/* quoted header plus 8 bytes of transport header, properly formed */
	if (len < 8 + 20 + 8)
		goto bad;
	inner = (const struct rt_iphdr *)(ic + 8);
	ihlen = rt_ip_hlen(inner);
	if ((inner->ver_ihl >> 4) != 4 || ihlen < 20 || inner_len < ihlen + 8)
		goto bad;
	/* We must have sent the quoted datagram (cheap spoofing filter). */
	if (!rt_netif_by_addr(inner->saddr) && !rt_ip_is_loopback_net(inner->saddr))
		goto bad;

	switch (type) {
	case RT_ICMP_UNREACH:
		rt_icmpstat.InDestUnreachs++;
		if (code == RT_ICMP_UNREACH_NEEDFRAG) {
			info = rt_get_be16(ic + 6);
			if (info < 68)
				info = mtu_plateau(rt_ntohs(inner->tot_len));
			rt_ip_pmtu_update(inner->daddr, info);
		}
		break;
	case RT_ICMP_TIMXCEED:
		rt_icmpstat.InTimeExcds++;
		break;
	case RT_ICMP_PARAMPROB:
		info = ic[4];
		break;
	case RT_ICMP_SOURCEQUENCH:
		break;   /* RFC 6633: ignored, but pass on for statistics */
	case RT_ICMP_REDIRECT:
		rt_icmpstat.InRedirects++;
		if (!icmp_drop_redirect && code <= 3) {
			u32 gw;
			memcpy(&gw, ic + 4, 4);
			rt_route_redirect(inner->daddr, gw, from);
		}
		return;
	}
	if (inner->protocol == RT_IPPROTO_TCP)
		rt_tcp_icmp_error(type, code, info, inner, inner_len);
	else if (inner->protocol == RT_IPPROTO_UDP)
		rt_udp_icmp_error(type, code, info, inner, inner_len);
	return;
bad:
	rt_icmpstat.InErrors++;
}

void rt_icmp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	u8 *ic = b->data;
	u32 len = b->len;
	u8 hdr[8];

	rt_icmpstat.InMsgs++;
	if (len < 8 || rt_inet_csum(ic, len) != 0) {
		rt_icmpstat.InErrors++;
		goto done;
	}
	switch (ic[0]) {
	case RT_ICMP_ECHO:
		rt_icmpstat.InEchos++;
		if ((b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)) && !icmp_bmcastecho)
			break;
		memcpy(hdr, ic, 8);
		hdr[0] = RT_ICMP_ECHOREPLY;
		icmp_send((b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)) ? 0 : ip->daddr,
		          ip->saddr, ip->tos, hdr, ic + 8, len - 8);
		break;
	case RT_ICMP_ECHOREPLY:
		rt_icmpstat.InEchoReps++;
		break;   /* raw sockets (ping) already have their copy */
	case RT_ICMP_MASKREQ:
		if (!icmp_maskrepl || len < 12 || !b->netif || !b->netif->addr || !ip->saddr)
			break;
		memcpy(hdr, ic, 8);
		hdr[0] = RT_ICMP_MASKREPLY;
		icmp_send(b->netif->addr, ip->saddr, 0, hdr, &b->netif->netmask, 4);
		break;
	case RT_ICMP_UNREACH:
	case RT_ICMP_SOURCEQUENCH:
	case RT_ICMP_REDIRECT:
	case RT_ICMP_TIMXCEED:
	case RT_ICMP_PARAMPROB:
		icmp_error_input(ic, len, ip->saddr);
		break;
	default:
		break;
	}
done:
	rt_buf_free(b);
}

void rt_icmp_init(void)
{
	memset(icmp_peers, 0, sizeof(icmp_peers));
	glob_tokens = (u32)icmp_msgs_burst;
	glob_last = rt_now_ms();
	rt_sysctl_register(icmp_sysctl, (int)RT_ARRAY_SIZE(icmp_sysctl));
}
