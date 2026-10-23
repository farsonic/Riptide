/*
 * Riptide - IPv4, routing, checksums, ICMP and transport dispatch.
 * All addresses are in network byte order unless stated otherwise.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_IP_H
#define RT_IP_H

#include "rt_types.h"
#include "rt_buf.h"
#include "rt_netif.h"
#include "rt_arch.h"
#include "rt_diag.h"

#define RT_IPPROTO_IP    0
#define RT_IPPROTO_ICMP  1
#define RT_IPPROTO_IGMP  2
#define RT_IPPROTO_TCP   6
#define RT_IPPROTO_UDP   17
#define RT_IPPROTO_RAW   255

#define RT_INADDR_ANY       0x00000000u
#define RT_INADDR_BROADCAST 0xffffffffu
#define RT_INADDR_LOOPBACK  rt_htonl(0x7f000001u)

/* Wire format, all fields network order. Accessed only on 2 byte aligned
 * addresses (the IP header always starts on an even address). */
struct rt_iphdr {
	u8  ver_ihl;      /* version << 4 | header length in words */
	u8  tos;
	u16 tot_len;
	u16 id;
	u16 frag_off;     /* flags(3) | offset(13) */
	u8  ttl;
	u8  protocol;
	u16 check;
	u32 saddr;
	u32 daddr;
};
#define RT_IP_DF     0x4000
#define RT_IP_MF     0x2000
#define RT_IP_OFFMASK 0x1fff
static inline u32 rt_ip_hlen(const struct rt_iphdr *ip) { return (u32)(ip->ver_ihl & 0x0f) << 2; }

/* ---- checksums ---- */
/* 32-bit one's complement partial sum of 'len' bytes added to 'sum'.
 * Works on any alignment. Dispatches to the CPU specific routine (see
 * rt_arch.h for the exact contract; compare sums only after folding). */
#ifndef RT_TOOL_BUILD
u32 rt_csum_partial_prof(const void *data, u32 len, u32 sum);   /* checksum.c */
static inline u32 rt_csum_partial(const void *data, u32 len, u32 sum)
{
#ifndef RT_NO_DIAG
	if (rt_prof_on)
		return rt_csum_partial_prof(data, len, sum);
#endif
	return rt_arch.csum(data, len, sum);
}
#endif
static inline u16 rt_csum_fold(u32 sum)
{
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}
/* Pseudo header sum for TCP/UDP: saddr, daddr (network order), proto,
 * transport length (host order). Result is a partial sum. */
u32 rt_csum_pseudo(u32 saddr, u32 daddr, u8 proto, u16 len);
/* Full Internet checksum of a buffer (returns value to store, net order
 * compatible: store with memcpy / as computed over network order data). */
#ifndef RT_TOOL_BUILD  /* tools (src/amiga/cmd) link without the core */
static inline u16 rt_inet_csum(const void *data, u32 len) { return rt_csum_fold(rt_csum_partial(data, len, 0)); }
/* Partial sum of a header (at most 60 bytes, even address and length):
 * 16-bit adds inline, without the call and set-up of the bulk routine.
 * Same contract as rt_csum_partial(). */
#if defined(__GNUC__)
typedef u16 __attribute__((may_alias)) rt_u16_alias;
#else
typedef u16 rt_u16_alias;
#endif
static inline u32 rt_csum_hdr(const void *data, u32 len, u32 sum)
{
	const rt_u16_alias *w = (const rt_u16_alias *)data;
	u32 acc = 0;

	if (((uintptr_t)data & 1) || (len & 1) || len > 60)
		return rt_csum_partial(data, len, sum);
	for (; len; len -= 2)
		acc += *w++;
	return rt_csum_add(sum, acc);
}
/* Partial sum of the whole of b's data added to 'sum': the one the driver
 * computed while copying the frame in (RT_BUFF_CSUM_PART), or a pass now. */
static inline u32 rt_buf_csum(const struct rt_buf *b, u32 sum)
{
	if (b->flags & RT_BUFF_CSUM_PART)
		return rt_csum_add(sum, b->csum);
	return rt_csum_partial(b->data, b->len, sum);
}
#endif

/* ---- routing ---- */
#define RT_RTF_UP       0x0001
#define RT_RTF_GATEWAY  0x0002
#define RT_RTF_HOST     0x0004
#define RT_RTF_REJECT   0x0008
#define RT_RTF_DYNAMIC  0x0010   /* created by redirect */
#define RT_RTF_MASK     0x0080   /* SIOCADDRT: netmask in rt_hash (see rt_bsd.h) */
#define RT_RTF_STATIC   0x0800
#define RT_RTF_CONNECTED 0x10000 /* interface route (not a BSD flag) */
#define RT_RTF_PMTU     0x20000 /* PMTU cache entry: only mtu/expire_ms valid (not a BSD flag) */
#define RT_RTF_DHCP     0x40000 /* installed by the DHCP client (not a BSD flag) */
/* rt_nexthop only: the next hop is a unicast address (a cached route) */
#define RT_NHF_UNICAST  0x80000000u

struct rt_route {
	struct rt_route *next;
	u32 dst;
	u32 mask;
	u32 gateway;          /* 0 for directly connected */
	struct rt_netif *netif;
	u32 flags;
	u32 metric;
	u32 mtu;              /* 0 = interface MTU; PMTU cache uses host routes */
	u32 expire_ms;        /* for dynamic/PMTU entries, 0 = never */
	u32 use;              /* usage counter */
};

int  rt_route_add(u32 dst, u32 mask, u32 gateway, struct rt_netif *nif, u32 flags, u32 metric);
int  rt_route_delete(u32 dst, u32 mask, u32 gateway);
/* Delete every route (not PMTU entries) for dst/mask whose gateway
 * matches (0 = any), whose interface matches (NULL = any) and which has
 * all of need_flags. Returns the number deleted or -RT_ESRCH. */
int  rt_route_delete_match(u32 dst, u32 mask, u32 gateway, struct rt_netif *nif, u32 need_flags);
/* Find a route by dst/mask/gateway (gateway 0 = first match), or NULL. */
struct rt_route *rt_route_find(u32 dst, u32 mask, u32 gateway);
/* Drop the dynamic entries (redirects, PMTU) that use nif. */
void rt_route_flush_dynamic_if(struct rt_netif *nif);
void rt_route_flush_if(struct rt_netif *nif);
struct rt_route *rt_route_list(void);  /* head (for netstat -r / sysctl) */

/* Result of a route lookup for an outgoing packet. */
struct rt_nexthop {
	struct rt_netif *netif;
	u32 nexthop;          /* gateway or destination */
	u32 src;              /* preferred source address */
	u32 mtu;              /* path MTU known for this destination */
	u32 flags;            /* route flags */
};
/* Longest-prefix lookup. Returns 0 or -RT_ENETUNREACH / -RT_EHOSTUNREACH. */
int  rt_ip_route(u32 dst, struct rt_nexthop *nh);
/* A connection's cached route (Linux's sk_dst_cache, BSD's inp_route):
 * rt_ip_output() fills it on a unicast send and uses it while no route,
 * interface or address changed (rt_route_gen) and for at most
 * RT_IP_DST_MS, which bounds what a change made around the functions that
 * bump the generation (expiry of a dynamic route) takes to apply. */
#define RT_IP_DST_MS 1000u
struct rt_ip_dst {
	u32 gen;              /* rt_route_gen when filled; 0 = empty */
	u32 daddr;
	u32 stamp;            /* rt_now_ms() when filled */
	struct rt_nexthop nh; /* nh.nexthop is a unicast address */
};
extern u32 rt_route_gen;
/* Routes, interfaces or their addresses changed: drop cached routes. */
void rt_route_changed(void);
/* PMTU discovery (RFC 1191): record a smaller MTU for dst (expires after
 * net.inet.ip.mtu_expires). */
void rt_ip_pmtu_update(u32 dst, u32 mtu);

/* ---- IP output ---- */
struct rt_ip_out {
	u32 saddr;            /* 0 = choose from route */
	u32 daddr;
	u8  proto;
	u8  ttl;              /* 0 = default */
	u8  tos;
	u8  df;               /* set DF */
	u8  dontroute;        /* SO_DONTROUTE / MSG_DONTROUTE */
	u8  allow_broadcast;  /* SO_BROADCAST */
	struct rt_netif *oif; /* force interface (multicast/broadcast), or NULL */
	/* multicast only: no copy for local members (IP_MULTICAST_LOOP 0),
	 * and TTL 0 (only the local copy, nothing on the wire) */
	u8  mcast_noloop;
	u8  mcast_ttl0;
	struct rt_ip_dst *dst;  /* the connection's route cache, or NULL */
};
/* b->data points at the transport header (payload of IP). IP pushes its
 * header, routes, fragments if needed (unless df) and transmits. Consumes b
 * always. Returns 0 or -errno (-RT_EMSGSIZE if df and too big,
 * -RT_EHOSTUNREACH, -RT_ENETUNREACH, -RT_ENOBUFS, -RT_EACCES for
 * broadcast without allow_broadcast). */
int  rt_ip_output(struct rt_buf *b, const struct rt_ip_out *o);
/* Send a fully formed IP datagram (IP_HDRINCL raw sockets / forwarding).
 * Fills id and checksum if zero. */
int  rt_ip_output_raw(struct rt_buf *b, int dontroute);
u16  rt_ip_next_id(void);
/* Choose source address for talking to dst (route based). 0 if none. */
u32  rt_ip_select_source(u32 dst);

/* ---- IP input ---- */
void rt_ip_input(struct rt_netif *nif, struct rt_buf *b);

/* ---- IPv4 multicast group membership (src/core/igmp.c) ----
 * Per interface and reference counted: every socket membership
 * (IP_ADD_MEMBERSHIP) and every internal user (e.g. an mDNS responder)
 * holds one. The first join programs the link layer filter (01:00:5e +
 * low 23 bits) and reports the group with IGMP (v3, v2 or v1, whatever
 * the network's querier speaks); the last leave reports the leave. IP
 * input accepts datagrams for joined groups and 224.0.0.1; UDP delivers
 * them to sockets that are members (BSD semantics). Lock held; groups in
 * network order. */
int  rt_ip_mc_join(struct rt_netif *nif, u32 group);
int  rt_ip_mc_leave(struct rt_netif *nif, u32 group);   /* -RT_EADDRNOTAVAIL: not joined */
int  rt_ip_mc_joined(const struct rt_netif *nif, u32 group);
/* Does IP input accept 'group' on nif (joined, or 224.0.0.1)? */
int  rt_ip_mc_member(const struct rt_netif *nif, u32 group);
/* The interface is going away: forget its groups (rt_netif_remove). */
void rt_ip_mc_flush(struct rt_netif *nif);
/* The interface for sending to / joining 'group' when the user names
 * none: the route to the group (default route), else the first up
 * multicast capable interface other than loopback. NULL if none. */
struct rt_netif *rt_ip_mc_default_if(u32 group);
#define RT_INADDR_ALLHOSTS_GROUP  0xe0000001u   /* 224.0.0.1 (host order) */
#define RT_INADDR_ALLRTRS_GROUP   0xe0000002u   /* 224.0.0.2 */
#define RT_INADDR_IGMPV3_GROUP    0xe0000016u   /* 224.0.0.22 */

/* ---- transport protocol entry points (called by IP input) ----
 * 'b->data' points at the transport header and b->len is the transport
 * length (IP padding removed). 'ip' points at the IP header (still inside
 * b's headroom, valid until the buffer is freed or modified). The callee
 * owns b. */
void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip);
void rt_udp_input(struct rt_buf *b, const struct rt_iphdr *ip);
void rt_icmp_input(struct rt_buf *b, const struct rt_iphdr *ip);
/* Raw sockets get a copy of every datagram for their protocol; b is NOT
 * consumed. Returns number of sockets that took a copy. */
int  rt_raw_input(const struct rt_buf *b, const struct rt_iphdr *ip);

/* ICMP errors delivered to transports. 'inner' points at the IP header
 * quoted in the ICMP message, 'inner_len' bytes are available (at least
 * header + 8). 'info' carries the next-hop MTU for
 * fragmentation-needed (type 3 code 4), otherwise 0. */
void rt_tcp_icmp_error(u8 type, u8 code, u32 info, const struct rt_iphdr *inner, u32 inner_len);
void rt_udp_icmp_error(u8 type, u8 code, u32 info, const struct rt_iphdr *inner, u32 inner_len);

/* ---- ICMP ---- */
#define RT_ICMP_ECHOREPLY     0
#define RT_ICMP_UNREACH       3
#define   RT_ICMP_UNREACH_NET       0
#define   RT_ICMP_UNREACH_HOST      1
#define   RT_ICMP_UNREACH_PROTOCOL  2
#define   RT_ICMP_UNREACH_PORT      3
#define   RT_ICMP_UNREACH_NEEDFRAG  4
#define   RT_ICMP_UNREACH_SRCFAIL   5
#define   RT_ICMP_UNREACH_ADMIN     13
#define RT_ICMP_SOURCEQUENCH  4
#define RT_ICMP_REDIRECT      5
#define RT_ICMP_ECHO          8
#define RT_ICMP_TIMXCEED      11
#define RT_ICMP_PARAMPROB     12
#define RT_ICMP_TSTAMP        13
#define RT_ICMP_TSTAMPREPLY   14
#define RT_ICMP_MASKREQ       17
#define RT_ICMP_MASKREPLY     18

/* Send an ICMP error about the datagram in 'orig' (whose data points at
 * its IP header). Applies RFC 1812 rules (never about ICMP errors,
 * broadcasts, fragments other than the first) and rate limiting.
 * 'orig' is not consumed. */
void rt_icmp_send_error(const struct rt_buf *orig, u8 type, u8 code, u32 info);

/* ---- init hooks (called by rt_init) ---- */
void rt_ip_init(void);
void rt_icmp_init(void);
void rt_udp_init(void);
void rt_igmp_init(void);
void rt_tcp_init(void);
void rt_raw_init(void);
void rt_arp_init(void);

#endif
