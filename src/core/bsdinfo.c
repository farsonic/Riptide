/*
 * Riptide - 4.4BSD binary tables for the Roadshow API (see rt_bsdinfo.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"
#include "rt/rt_bsdinfo.h"
#include "rt/rt_stats.h"

/* Copy src (n bytes) to buf at off, as far as len allows. */
static void out(u8 *buf, u32 len, u32 off, const void *src, u32 n)
{
	if (!buf || off >= len)
		return;
	memcpy(buf + off, src, RT_MIN(n, len - off));
}

static void out32(u8 *buf, u32 len, u32 off, u32 v)
{
	out(buf, len, off, &v, 4);
}

static void out16(u8 *buf, u32 len, u32 off, u16 v)
{
	out(buf, len, off, &v, 2);
}

/* ---- routes ---- */

static u32 put_sin(u8 *buf, u32 len, u32 off, u32 addr)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = addr;
	out(buf, len, off, &sin, sizeof(sin));
	return sizeof(sin);
}

u32 rt_bsd_route_dump(u32 flags, u8 *buf, u32 len)
{
	struct rt_route *rt;
	u32 off = 0, now = rt_now_ms();
	u8 zero[RT_RTM_HDRLEN];

	for (rt = rt_route_list(); rt; rt = rt->next) {
		u32 f = (rt->flags & 0xffffu) | RT_RTF_UP, addrs = RT_RTA_DST, start = off, ifa;
		if (rt->flags & RT_RTF_PMTU)
			continue;               /* a path MTU cache entry, not a route */
		if (rt->mask == 0xffffffffu)
			f |= RT_RTF_HOST;
		if (rt->gateway)
			f |= RT_RTF_GATEWAY;
		if ((f & flags) != flags)
			continue;
		ifa = rt->netif ? rt_netif_src(rt->netif, rt->dst, rt->gateway) : 0;
		if (rt->gateway)
			addrs |= RT_RTA_GATEWAY;
		if (!(f & RT_RTF_HOST))
			addrs |= RT_RTA_NETMASK;
		if (ifa)
			addrs |= RT_RTA_IFA;
		off += RT_RTM_HDRLEN;
		off += put_sin(buf, len, off, rt->dst);
		if (addrs & RT_RTA_GATEWAY)
			off += put_sin(buf, len, off, rt->gateway);
		if (addrs & RT_RTA_NETMASK)
			off += put_sin(buf, len, off, rt->mask);
		if (addrs & RT_RTA_IFA)
			off += put_sin(buf, len, off, ifa);
		if (buf && start < len) {
			u8 h[RT_RTM_HDRLEN];
			u16 ml = (u16)(off - start), ix = (u16)(rt->netif ? rt->netif->index : 0);
			memset(h, 0, sizeof(h));
			memcpy(h + RT_RTM_OFS_MSGLEN, &ml, 2);
			h[RT_RTM_OFS_VERSION] = RT_RTM_VERSION;
			h[RT_RTM_OFS_TYPE] = RT_RTM_GET;
			memcpy(h + RT_RTM_OFS_INDEX, &ix, 2);
			memcpy(h + RT_RTM_OFS_FLAGS, &f, 4);
			memcpy(h + RT_RTM_OFS_ADDRS, &addrs, 4);
			memcpy(h + RT_RTM_OFS_USE, &rt->use, 4);
			memcpy(h + RT_RTM_OFS_RMX + 4 * RT_RMX_MTU, &rt->mtu, 4);
			memcpy(h + RT_RTM_OFS_RMX + 4 * RT_RMX_HOPCOUNT, &rt->metric, 4);
			if (rt->expire_ms) {
				u32 e = (s32)(rt->expire_ms - now) > 0 ? (rt->expire_ms - now + 999) / 1000 : 0;
				memcpy(h + RT_RTM_OFS_RMX + 4 * RT_RMX_EXPIRE, &e, 4);
			}
			memcpy(h + RT_RTM_OFS_RMX + 4 * RT_RMX_PKSENT, &rt->use, 4);
			out(buf, len, start, h, sizeof(h));
		}
	}
	memset(zero, 0, sizeof(zero));
	out(buf, len, off, zero, sizeof(zero));
	return off + RT_RTM_HDRLEN;
}

/* ---- statistics ---- */

static u32 sub0(u32 a, u32 b)
{
	return a > b ? a - b : 0;
}

/* struct icmpstat: error, oldshort, oldicmp, outhist[19], badcode,
 * tooshort, checksum, badlen, reflect, inhist[19] */
static void icmpstat(u32 *v)
{
	const struct rt_icmp_stats *s = &rt_icmpstat;
	u32 *outh = v + 3, *inh = v + 3 + 19 + 5;

	v[0] = sub0(s->OutMsgs, s->OutEchoReps);
	outh[0] = s->OutEchoReps;
	outh[3] = s->OutDestUnreachs;
	v[3 + 19 + 2] = s->InErrors;              /* checksum */
	v[3 + 19 + 4] = s->OutEchoReps;           /* reflect */
	inh[0] = s->InEchoReps;
	inh[3] = s->InDestUnreachs;
	inh[5] = s->InRedirects;
	inh[8] = s->InEchos;
	inh[11] = s->InTimeExcds;
}

static void igmpstat(u32 *v)
{
	const struct rt_igmp_stats *s = &rt_igmpstat;

	v[0] = s->InMsgs;
	v[2] = s->InErrors;
	v[3] = s->InQueriesV1 + s->InQueriesV2 + s->InQueriesV3;
	v[4] = s->InBadQueries;
	v[5] = s->InReports;
	v[7] = s->InOurReports;
	v[8] = s->OutReportsV1 + s->OutReportsV2 + s->OutReportsV3;
}

static void ipstat(u32 *v)
{
	const struct rt_ip_stats *s = &rt_ipstat;

	v[0] = s->InReceives;
	v[1] = s->InHdrErrors;        /* badsum: header errors, checksums among them */
	v[6] = s->ReasmReqds;         /* fragments */
	v[7] = s->ReasmFails;         /* fragdropped */
	v[8] = s->ReasmTimeout;       /* fragtimeout */
	v[9] = s->ForwDatagrams;      /* forward */
	v[10] = s->InAddrErrors;      /* cantforward */
	v[12] = s->InUnknownProtos;   /* noproto */
	v[13] = s->InDelivers;        /* delivered */
	v[14] = s->OutRequests;       /* localout */
	v[15] = s->OutDiscards;       /* odropped */
	v[16] = s->ReasmOKs;          /* reassembled */
	v[17] = s->FragOKs;           /* fragmented */
	v[18] = s->FragCreates;       /* ofragments */
	v[19] = s->FragFails;         /* cantfrag */
	v[21] = s->OutNoRoutes;       /* noroute */
}

static void tcpstat(u32 *v)
{
	const struct rt_tcp_stats *s = &rt_tcpstat;

	v[0] = s->ActiveOpens;                                 /* connattempt */
	v[1] = s->PassiveOpens;                                /* accepts */
	v[2] = sub0(s->ActiveOpens + s->PassiveOpens, s->AttemptFails); /* connects */
	v[3] = s->EstabResets;                                 /* drops */
	v[4] = s->AttemptFails;                                /* conndrops */
	v[8] = s->DelayedACKs;                                 /* delack */
	v[9] = s->TCPAbortOnTimeout;                           /* timeoutdrop */
	v[10] = s->TCPTimeouts;                                /* rexmttimeo */
	v[13] = s->TCPKeepAlive;                               /* keepprobe */
	v[15] = s->OutSegs;                                    /* sndtotal */
	v[18] = s->RetransSegs;                                /* sndrexmitpack */
	v[21] = s->TCPWinProbe;                                /* sndprobe */
	v[24] = s->OutRsts;                                    /* sndctrl (resets only) */
	v[25] = s->InSegs;                                     /* rcvtotal */
	v[28] = s->InCsumErrors;                               /* rcvbadsum */
	v[30] = sub0(s->InErrs, s->InCsumErrors);              /* rcvshort */
	v[35] = s->TCPOFOQueue;                                /* rcvoopack */
	v[43] = s->TCPPureAcks;                                /* rcvackpack */
	v[46] = s->PAWSEstab;                                  /* pawsdrop */
	v[47] = s->TCPHPAcks;                                  /* predack */
	v[48] = s->TCPHPHits;                                  /* preddat */
}

static void udpstat(u32 *v)
{
	const struct rt_udp_stats *s = &rt_udpstat;

	v[0] = s->InDatagrams + s->NoPorts + s->InErrors;      /* ipackets */
	v[1] = sub0(s->InErrors, s->InCsumErrors + s->RcvbufErrors); /* hdrops */
	v[2] = s->InCsumErrors;                                /* badsum */
	v[4] = s->NoPorts;                                     /* noport */
	v[6] = s->RcvbufErrors;                                /* fullsock */
	v[8] = s->OutDatagrams;                                /* opackets */
}

/* struct protocol_connection_data: foreign address (0), foreign port (4),
 * local address (6), local port (10), receive queue (12), send queue
 * (16), TCP state (20) */
static int sockets(int tcp, u8 *buf, u32 len)
{
	struct rt_pcb_info *pi;
	int n = tcp ? rt_tcp_list(NULL, 0) : rt_udp_list(NULL, 0), i;
	u32 off = 0;

	if (n <= 0)
		return 0;
	pi = rt_malloc((u32)n * sizeof(*pi));
	if (!pi)
		return -RT_ENOMEM;
	n = RT_MIN(n, tcp ? rt_tcp_list(pi, n) : rt_udp_list(pi, n));
	for (i = 0; i < n; i++) {
		s32 st = tcp ? (s32)pi[i].state : -1;
		if (pi[i].v6)
			continue;               /* the structure holds IPv4 addresses */
		out(buf, len, off, &pi[i].faddr, 4);
		out16(buf, len, off + 4, rt_htons(pi[i].fport));
		out(buf, len, off + 6, &pi[i].laddr, 4);
		out16(buf, len, off + 10, rt_htons(pi[i].lport));
		out32(buf, len, off + 12, pi[i].recvq);
		out32(buf, len, off + 16, pi[i].sendq);
		out32(buf, len, off + 20, (u32)st);
		off += RT_BSD_PCD_SIZE;
	}
	rt_free(pi);
	return (int)off;
}

int rt_bsd_netstat(int type, u8 *buf, u32 len)
{
	u32 v[RT_BSD_TCPSTAT_SIZE / 4], size;

	memset(v, 0, sizeof(v));
	switch (type) {
	case RT_NETSTATUS_ICMP: icmpstat(v); size = RT_BSD_ICMPSTAT_SIZE; break;
	case RT_NETSTATUS_IGMP: igmpstat(v); size = RT_BSD_IGMPSTAT_SIZE; break;
	case RT_NETSTATUS_IP:   ipstat(v);   size = RT_BSD_IPSTAT_SIZE;   break;
	case RT_NETSTATUS_MB:   size = RT_BSD_MBSTAT_SIZE;  break;   /* no mbufs */
	case RT_NETSTATUS_MRT:  size = RT_BSD_MRTSTAT_SIZE; break;   /* no multicast routing */
	case RT_NETSTATUS_RT: {
		s16 w[5];
		memset(w, 0, sizeof(w));
		w[3] = (s16)RT_MIN(rt_ipstat.OutNoRoutes, 0x7fffu);   /* rts_unreach */
		out(buf, len, 0, w, sizeof(w));
		return RT_BSD_RTSTAT_SIZE;
	}
	case RT_NETSTATUS_TCP:  tcpstat(v);  size = RT_BSD_TCPSTAT_SIZE;  break;
	case RT_NETSTATUS_UDP:  udpstat(v);  size = RT_BSD_UDPSTAT_SIZE;  break;
	case RT_NETSTATUS_TCP_SOCKETS:
		return sockets(1, buf, len);
	case RT_NETSTATUS_UDP_SOCKETS:
		return sockets(0, buf, len);
	default:
		return -RT_EINVAL;
	}
	out(buf, len, 0, v, size);
	return (int)size;
}
