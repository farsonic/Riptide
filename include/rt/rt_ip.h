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
 * Works on any alignment. */
u32 rt_csum_partial(const void *data, u32 len, u32 sum);
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
static inline u16 rt_inet_csum(const void *data, u32 len) { return rt_csum_fold(rt_csum_partial(data, len, 0)); }

/* ---- routing ---- */
#define RT_RTF_UP       0x0001
#define RT_RTF_GATEWAY  0x0002
#define RT_RTF_HOST     0x0004
#define RT_RTF_REJECT   0x0008
#define RT_RTF_DYNAMIC  0x0010   /* created by redirect */
#define RT_RTF_STATIC   0x0800
#define RT_RTF_CONNECTED 0x10000 /* interface route (not a BSD flag) */
#define RT_RTF_PMTU     0x20000 /* PMTU cache entry: only mtu/expire_ms valid (not a BSD flag) */

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
void rt_tcp_init(void);
void rt_raw_init(void);
void rt_arp_init(void);

#endif
