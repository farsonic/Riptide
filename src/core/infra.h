/*
 * Riptide - internal interfaces between the IPv4/link "infra" modules
 * (netif, arp, loopback, route, ip, ip_frag, icmp, udp, raw, init).
 * Not a contract header: other teams use include/rt/ only.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_INFRA_H
#define RT_INFRA_H

#include "rt/riptide.h"

/* ---- tunables (s32 so they can be registered with rt_sysctl) ---- */
extern s32 rt_ip_forwarding;        /* net.inet.ip.forwarding */
extern s32 rt_ip_defttl;            /* net.inet.ip.ttl */
extern s32 rt_ip_sendredirects;     /* net.inet.ip.redirect */
extern s32 rt_ip_pathmtudisc;       /* net.inet.ip.pathmtudisc */
extern s32 rt_ip_mtu_expires;       /* net.inet.ip.mtu_expires (s) */
extern s32 rt_ip_portrange_first;   /* net.inet.ip.portrange.first */
extern s32 rt_ip_portrange_last;    /* net.inet.ip.portrange.last */
extern s32 rt_udp_checksum;         /* net.inet.udp.checksum */
extern s32 rt_udp_recvspace;        /* net.inet.udp.recvspace */
extern s32 rt_udp_maxdgram;         /* net.inet.udp.maxdgram */

/* Address classification (network order arguments). */
static inline int rt_ip_is_multicast(u32 a) { return (rt_ntohl(a) & 0xf0000000u) == 0xe0000000u; }
static inline int rt_ip_is_loopback_net(u32 a) { return (rt_ntohl(a) >> 24) == 127; }
static inline int rt_ip_is_zeronet(u32 a) { return (rt_ntohl(a) >> 24) == 0; }

/* ---- buf.c ---- */
/* Release the allocator's free lists (leak checks in tests). */
void rt_buf_drain_cache(void);

/* ---- netif.c ---- */
void rt_netif_init(void);

/* ---- ip6_if.c: link state changes (netif.c) ---- */
void rt_ip6_if_up(struct rt_netif *nif);
void rt_ip6_if_down(struct rt_netif *nif);
void rt_route6_flush_learned(struct rt_netif *nif);
/* ---- inet.c ---- */
void rt_inet_init(void);

/* ---- loopback.c ---- */
int  rt_loopback_init(void);
/* Deliver the deferred loopback queue; returns 1 if work remains. */
int  rt_loopback_poll(void);

/* ---- arp.c ---- */
/* Forget dynamic entries of nif (keep_static: keep static ones). */
void rt_arp_flush(struct rt_netif *nif, int keep_static);

/* ---- route.c ---- */
void rt_route_init(void);
/* Connected route maintenance used by rt_netif_set_addr(). */
void rt_route_del_connected(struct rt_netif *nif);
/* Accept an ICMP redirect for 'dst' to 'gw' (sent by 'from'). */
void rt_route_redirect(u32 dst, u32 gw, u32 from);

/* ---- ip.c ---- */
/* Transmit an IP datagram whose header is complete (data at IP header,
 * checksum valid) towards nh: fragments if needed and permitted. */
int  rt_ip_send_routed(struct rt_buf *b, const struct rt_nexthop *nh);
/* Per destination randomised IP id (RFC 6864 / Linux ip_idents). */
u16  rt_ip_select_id(u32 daddr);

/* ---- ip_frag.c ---- */
void rt_ipfrag_init(void);
/* Takes a fragment (data at IP header, tot_len trimmed). Returns the
 * reassembled datagram (data at IP header) or NULL (kept or dropped). */
struct rt_buf *rt_ipfrag_reass(struct rt_buf *b);
/* Split b (data at IP header) into fragments of at most 'mtu' bytes and
 * send each via rt_netif_output_ip(nif, .., nexthop). Consumes b. */
int  rt_ipfrag_output(struct rt_buf *b, struct rt_netif *nif, u32 nexthop, u32 mtu);
/* Number of datagrams being reassembled / bytes held (tests, netstat). */
void rt_ipfrag_stats(u32 *queues, u32 *bytes);

/* ---- udp.c: helpers shared with raw.c ---- */
/* Per socket IP level options of datagram sockets. */
struct rt_inet_opts {
	u8 ttl;             /* 0 = default */
	u8 tos;
	u8 recvdstaddr;
	u8 hdrincl;
	u8 mcast_ttl;
	u8 mcast_loop;
	/* IPPROTO_IPV6 level (AF_INET6 sockets) */
	s16 hlim;           /* IPV6_UNICAST_HOPS, -1 = default */
	s16 mcast_hops;     /* IPV6_MULTICAST_HOPS, -1 = default (1) */
	u32 mcast_if;       /* IPV6_MULTICAST_IF index, 0 = route */
	u8  tclass;         /* IPV6_TCLASS */
	u8  v6only;         /* IPV6_V6ONLY */
	u8  dontfrag;       /* IPV6_DONTFRAG */
	u8  mcast_loop6;
};
void rt_inet_opts_init(struct rt_inet_opts *o, int domain);
int  rt_inet_setopt(struct rt_inet_opts *o, int name, const void *val, u32 len, int allow_hdrincl);
int  rt_inet_getopt(const struct rt_inet_opts *o, int name, void *val, u32 *len, int allow_hdrincl);
/* IPPROTO_IPV6 level; 'so' for V6ONLY checks (bound sockets) */
int  rt_inet6_setopt(struct rt_socket *so, struct rt_inet_opts *o, int name, const void *val, u32 len);
int  rt_inet6_getopt(const struct rt_inet_opts *o, int name, void *val, u32 *len);
/* recv() for datagram sockets (rcvq of rt_dgram_cb tagged buffers). */
int  rt_dgram_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                   int flags, struct rt_sockaddr_in6 *from, int *msgflags);
u32  rt_dgram_poll(struct rt_socket *so);
int  rt_dgram_ioctl(struct rt_socket *so, u32 req, void *arg);
/* Queue b (data = what the application will read) on so with ancillary
 * info, waking readers. Addresses IPv4-mapped for IPv4. Consumes b.
 * Returns 0 or -RT_ENOBUFS. */
int  rt_dgram_deliver(struct rt_socket *so, struct rt_buf *b, const rt_in6_addr *saddr, u16 sport,
                      const rt_in6_addr *daddr, const struct rt_netif *nif);
/* Validate a normalised address from the socket layer for so (family
 * AF_INET/AF_UNSPEC on AF_INET sockets, address IPv4-mapped; AF_INET6 on
 * AF_INET6 sockets, IPv4-mapped only without V6ONLY). */
int  rt_sin_check(const struct rt_socket *so, const struct rt_sockaddr_in6 *sin, int v6only);
/* Fill a normalised address for so. */
void rt_sin_fill(const struct rt_socket *so, struct rt_sockaddr_in6 *sin, const rt_in6_addr *a,
                 u16 port, u32 scope);

/* ---- inet.c: family independent helpers for the transports ----
 * Addresses are rt_in6_addr, IPv4 as IPv4-mapped. */
static inline int rt_inaddr_is_any(const rt_in6_addr *a)
{
	return rt_in6_is_unspec(a) || (rt_in6_is_v4mapped(a) && a->w[3] == 0);
}
static inline void rt_inaddr_v4any(rt_in6_addr *a) { rt_in6_set_v4mapped(a, 0); }
struct rt_inet_route {
	struct rt_netif *netif;
	rt_in6_addr src;              /* preferred source */
	u32 mtu;
	u32 flags;                    /* RT_RTF_* */
	u8  bcast;                    /* IPv4 broadcast on that interface */
};
/* Route to dst (scope: interface index for link-local/multicast, 0 = any). */
int  rt_inet_route(const rt_in6_addr *dst, u32 scope, struct rt_inet_route *r);
u32  rt_inet_pseudo(const rt_in6_addr *src, const rt_in6_addr *dst, u8 proto, u32 len);
struct rt_inet_out {
	rt_in6_addr src, dst;         /* both v4-mapped or both IPv6 */
	u8  proto;
	u8  tos;                      /* TOS / traffic class */
	s16 ttl;                      /* TTL / hop limit, <= 0: default */
	u8  df;                       /* IPv4 DF / IPv6 DONTFRAG */
	u8  dontroute;
	u8  allow_broadcast;
	u8  mcast_noloop;
	u32 scope;                    /* outgoing interface index (IPv6 zone / multicast), 0 */
};
int  rt_inet_output(struct rt_buf *b, const struct rt_inet_out *o);
void rt_inet_pmtu_update(const rt_in6_addr *dst, u32 mtu);
/* One of our addresses (or loopback, or for IPv4 a broadcast address)? */
int  rt_inet_is_local(const rt_in6_addr *a);
/* Could a socket bound to la (v6only flag) receive packets sent to lb, or
 * do two bindings overlap? (dual-stack rules: :: covers IPv4 unless
 * V6ONLY, ::ffff:0.0.0.0 covers all IPv4) */
int  rt_inet_bind_overlap(const rt_in6_addr *a, int a_v6only, const rt_in6_addr *b, int b_v6only);
/* Does local binding la (v6only) accept packets for dst? 0 no, 1 wildcard, 2 exact */
int  rt_inet_laddr_match(const rt_in6_addr *la, int v6only, const rt_in6_addr *dst);
extern s32 rt_ip6_v6only;           /* net.inet6.ip6.v6only: IPV6_V6ONLY default */
extern s32 rt_ip6_hlim;             /* net.inet6.ip6.hlim */
/* SipHash-2-4 (ip6_if.c) */
u64  rt_siphash(const void *data, u32 len, const u8 key[16]);

#endif
