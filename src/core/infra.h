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
};
int  rt_inet_setopt(struct rt_inet_opts *o, int name, const void *val, u32 len, int allow_hdrincl);
int  rt_inet_getopt(const struct rt_inet_opts *o, int name, void *val, u32 *len, int allow_hdrincl);
/* recv() for datagram sockets (rcvq of rt_dgram_cb tagged buffers). */
int  rt_dgram_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                   int flags, struct rt_sockaddr_in *from, int *msgflags);
u32  rt_dgram_poll(struct rt_socket *so);
int  rt_dgram_ioctl(struct rt_socket *so, u32 req, void *arg);
/* Queue b (data = what the application will read) on so with ancillary
 * info, waking readers. Consumes b. Returns 0 or -RT_ENOBUFS. */
int  rt_dgram_deliver(struct rt_socket *so, struct rt_buf *b, u32 saddr, u16 sport,
                      u32 daddr, const struct rt_netif *nif);
/* Validate a user supplied sockaddr_in (family AF_INET or 0, len). */
int  rt_sin_check(const struct rt_sockaddr_in *sin);

#endif
