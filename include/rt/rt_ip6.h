/*
 * Riptide - IPv6 (RFC 8200), ICMPv6 (RFC 4443), neighbour discovery
 * (RFC 4861), stateless autoconfiguration (RFC 4862), MLDv2 (RFC 3810).
 *
 * Addresses are 16 byte arrays in network order (rt_in6_addr). Transports
 * keep IPv4 endpoints as IPv4-mapped addresses (::ffff:a.b.c.d, RFC 4291
 * 2.5.5.2), so one address type serves both families above the IP layer.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_IP6_H
#define RT_IP6_H

#include "rt_types.h"
#include "rt_buf.h"
#include "rt_netif.h"


/* protocol numbers (IANA) */
#define RT_IPPROTO_HOPOPTS  0
#define RT_IPPROTO_IPV6     41      /* also the setsockopt level */
#define RT_IPPROTO_ROUTING  43
#define RT_IPPROTO_FRAGMENT 44
#define RT_IPPROTO_ICMPV6   58
#define RT_IPPROTO_NONE     59
#define RT_IPPROTO_DSTOPTS  60

#define RT_IP6_HLEN       40
#define RT_IP6_MIN_MTU    1280      /* RFC 8200 5 */
#define RT_INET6_ADDRSTRLEN 46

/* Fixed header (RFC 8200 3). Input buffers are realigned to 4 bytes. */
struct rt_ip6hdr {
	u32 vtcfl;               /* version(4) traffic class(8) flow label(20), network order */
	u16 plen;                /* payload length, network order */
	u8  nxt;
	u8  hlim;
	rt_in6_addr src;
	rt_in6_addr dst;
};

extern const rt_in6_addr rt_in6addr_any;
extern const rt_in6_addr rt_in6addr_loopback;

/* ---- address helpers ---- */
static inline int rt_in6_eq(const rt_in6_addr *a, const rt_in6_addr *b)
{
	return a->w[0] == b->w[0] && a->w[1] == b->w[1] && a->w[2] == b->w[2] && a->w[3] == b->w[3];
}
static inline int rt_in6_is_unspec(const rt_in6_addr *a)
{
	return (a->w[0] | a->w[1] | a->w[2] | a->w[3]) == 0;
}
static inline int rt_in6_is_loopback(const rt_in6_addr *a)
{
	return (a->w[0] | a->w[1] | a->w[2]) == 0 && a->w[3] == rt_htonl(1);
}
static inline int rt_in6_is_mcast(const rt_in6_addr *a) { return a->b[0] == 0xff; }
static inline int rt_in6_is_linklocal(const rt_in6_addr *a)
{
	return a->b[0] == 0xfe && (a->b[1] & 0xc0) == 0x80;
}
static inline int rt_in6_is_sitelocal(const rt_in6_addr *a)
{
	return a->b[0] == 0xfe && (a->b[1] & 0xc0) == 0xc0;
}
static inline int rt_in6_is_ula(const rt_in6_addr *a) { return (a->b[0] & 0xfe) == 0xfc; }
static inline int rt_in6_is_v4mapped(const rt_in6_addr *a)
{
	return (a->w[0] | a->w[1]) == 0 && a->w[2] == rt_htonl(0xffffu);
}
/* IPv4 address (network order) of a mapped address. */
static inline u32 rt_in6_v4(const rt_in6_addr *a) { return a->w[3]; }
static inline void rt_in6_set_v4mapped(rt_in6_addr *a, u32 v4)
{
	a->w[0] = 0;
	a->w[1] = 0;
	a->w[2] = rt_htonl(0xffffu);
	a->w[3] = v4;
}
/* Multicast scope (RFC 4291 2.7): 1 interface, 2 link, 5 site, 8 org, e global. */
static inline int rt_in6_mc_scope(const rt_in6_addr *a) { return a->b[1] & 0x0f; }
static inline int rt_in6_is_mc_linklocal(const rt_in6_addr *a)
{
	return a->b[0] == 0xff && (a->b[1] & 0x0f) <= 2;
}
/* Address scope for source selection and zone handling (RFC 4007 /
 * RFC 6724 3.1): mapped IPv4 addresses get the scope of their class. */
#define RT_IN6_SCOPE_IFACE  0x1
#define RT_IN6_SCOPE_LINK   0x2
#define RT_IN6_SCOPE_SITE   0x5
#define RT_IN6_SCOPE_GLOBAL 0xe
int  rt_in6_scope(const rt_in6_addr *a);
/* Do the first plen bits of a and b agree? */
int  rt_in6_prefix_eq(const rt_in6_addr *a, const rt_in6_addr *b, int plen);
/* Number of leading bits a and b have in common (0..128). */
int  rt_in6_common_prefix(const rt_in6_addr *a, const rt_in6_addr *b);
/* out = a with every bit after plen cleared. */
void rt_in6_mask(rt_in6_addr *out, const rt_in6_addr *a, int plen);
/* Solicited-node multicast group ff02::1:ffXX:XXXX of a (RFC 4291 2.7.1). */
void rt_in6_solnode(rt_in6_addr *out, const rt_in6_addr *a);
/* Link layer multicast address 33:33:xx:xx:xx:xx of group g (RFC 2464 7). */
void rt_in6_mc_mac(u8 *mac, const rt_in6_addr *g);

/* ---- text form (src/core/inet6_text.c, also linked by the tools) ----
 * rt_inet6_pton: full RFC 4291 2.2 syntax including "::" and a trailing
 * dotted quad. Returns 0 or -1. rt_inet6_ntop: RFC 5952 canonical form
 * (lower case, longest run of >= 2 zero groups as "::", first on ties,
 * mapped/compatible IPv4 in dotted quad). buf >= RT_INET6_ADDRSTRLEN. */
int   rt_inet6_pton(const char *s, rt_in6_addr *out);
char *rt_inet6_ntop(const rt_in6_addr *a, char *buf, u32 len);
/* "ADDR[%zone][/PLEN]" without modifying s; zone (may be NULL) gets the
 * text after '%' (empty if none, max zonelen-1 chars), *plen = -1 when no
 * prefix is given. 0 or -1. */
int   rt_inet6_parse_prefix(const char *s, rt_in6_addr *out, int *plen, char *zone, u32 zonelen);

/* ---- interface addresses ---- */
#define RT_IN6_IFF_TENTATIVE  0x0001  /* DAD in progress: not usable yet */
#define RT_IN6_IFF_DEPRECATED 0x0002  /* preferred lifetime over */
#define RT_IN6_IFF_AUTOCONF   0x0004  /* SLAAC (RFC 4862) */
#define RT_IN6_IFF_DUPLICATED 0x0008  /* DAD failed */
#define RT_IN6_IFF_NODAD      0x0010  /* no DAD (loopback, PTP, "nodad") */
#define RT_IN6_IFF_PERMANENT  0x0020  /* configured, no lifetime */
#define RT_IN6_IFF_TEMPORARY  0x0040  /* RFC 8981 temporary address */
#define RT_IN6_IFF_DHCP       0x0080  /* reserved (stateful DHCPv6 not implemented) */

#define RT_IN6_INFINITE 0xffffffffu  /* lifetime "forever" (seconds) */

struct rt_in6_ifaddr_info {
	rt_in6_addr addr;
	u32 ifindex;
	u32 valid_s, pref_s;     /* seconds left, RT_IN6_INFINITE = forever */
	u16 flags;               /* RT_IN6_IFF_* */
	u8  plen;
	u8  scope;               /* RT_IN6_SCOPE_* */
};

/* Enable (on = 1) or disable IPv6 on nif. Enabling creates the link-local
 * address (and ::1 on loopback), joins all-nodes and, on Ethernet, starts
 * DAD, router solicitation and MLD. Lock held. */
int  rt_ip6_ifenable(struct rt_netif *nif, int on);
int  rt_ip6_ifenabled(const struct rt_netif *nif);
/* Add a manual address. valid/pref in seconds (RT_IN6_INFINITE: permanent).
 * flags: RT_IN6_IFF_NODAD may be given. Installs the on-link prefix route
 * for plen < 128 (as Linux). -RT_EEXIST if present (lifetimes updated). */
int  rt_in6_addr_add(struct rt_netif *nif, const rt_in6_addr *a, int plen, u32 flags,
                     u32 valid_s, u32 pref_s);
int  rt_in6_addr_del(struct rt_netif *nif, const rt_in6_addr *a);
/* Fill up to max entries (all interfaces when nif is NULL); returns the total. */
int  rt_in6_addr_list(const struct rt_netif *nif, struct rt_in6_ifaddr_info *out, int max);
/* Interface owning a's address (also tentative ones when any != 0), or NULL. */
struct rt_netif *rt_in6_addr_owner(const rt_in6_addr *a, int any);
/* Is a one of our usable unicast addresses (or a group we joined on nif,
 * nif may be NULL for unicast checks)? */
int  rt_ip6_is_local(const rt_in6_addr *a);
int  rt_ip6_mc_joined(const struct rt_netif *nif, const rt_in6_addr *g);
/* Group membership (refcounted, sends MLD reports). Lock held. */
int  rt_ip6_mc_join(struct rt_netif *nif, const rt_in6_addr *g);
int  rt_ip6_mc_leave(struct rt_netif *nif, const rt_in6_addr *g);
/* Source address selection (RFC 6724 5, without home addresses and with a
 * fixed policy table). oif may be NULL. 0 or -RT_EADDRNOTAVAIL. */
int  rt_ip6_select_source(const rt_in6_addr *dst, const struct rt_netif *oif, rt_in6_addr *src);
/* IPv6 MTU of the link (RA MTU option or the interface MTU). */
u32  rt_ip6_link_mtu(const struct rt_netif *nif);

/* ---- routing ---- */
#define RT_RTF_RA       0x80000  /* learned from router advertisements (not a BSD flag) */

struct rt_route6 {
	struct rt_route6 *next;
	rt_in6_addr dst;
	rt_in6_addr gateway;     /* unspecified for on-link */
	struct rt_netif *netif;
	u32 flags;               /* RT_RTF_* */
	u32 metric;
	u32 mtu;                 /* 0 = link MTU; PMTU entries carry the path MTU */
	u32 expire_s;            /* rt_ip6_seconds() deadline, 0 = never */
	u32 use;
	u8  plen;
	s8  pref;                /* RA default router preference (RFC 4191): -1, 0, 1 */
};

struct rt_nexthop6 {
	struct rt_netif *netif;
	rt_in6_addr nexthop;     /* gateway or destination */
	u32 mtu;                 /* path MTU */
	u32 flags;
};

int  rt_route6_add(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw, struct rt_netif *nif,
                   u32 flags, u32 metric);
/* Delete every route matching dst/plen (gw NULL or unspecified = any,
 * nif NULL = any) with all of need_flags. Count or -RT_ESRCH. */
int  rt_route6_delete(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                      struct rt_netif *nif, u32 need_flags);
struct rt_route6 *rt_route6_list(void);
void rt_route6_flush_if(struct rt_netif *nif);
/* Longest prefix lookup. Link-local and multicast destinations need the
 * interface: oif (or the first interface with IPv6 when NULL). */
int  rt_ip6_route(const rt_in6_addr *dst, const struct rt_netif *oif, struct rt_nexthop6 *nh);
/* RFC 8201: record a smaller path MTU (never below 1280). */
void rt_ip6_pmtu_update(const rt_in6_addr *dst, u32 mtu);
/* Monotonic seconds used for lifetimes (starts at 1). */
u32  rt_ip6_seconds(void);

/* ---- neighbour cache (src/core/nd6.c) ---- */
/* states for tools (rt_ctl.h RT_NUD_* plus these) */
struct rt_nd6_info {
	rt_in6_addr addr;
	u8  hw[6];
	u8  state;               /* RT_NUD_* (rt_ctl.h) */
	u8  router;
	u32 age_s;
	u32 ifindex;
};
int  rt_nd6_list(struct rt_nd6_info *out, int max);
int  rt_nd6_add_static(struct rt_netif *nif, const rt_in6_addr *a, const u8 *hw);
int  rt_nd6_delete(struct rt_netif *nif, const rt_in6_addr *a);
void rt_nd6_flush(struct rt_netif *nif, int keep_static);

/* ---- output ---- */
struct rt_ip6_out {
	rt_in6_addr src;         /* unspecified: select */
	rt_in6_addr dst;
	u8  nxt;                 /* upper layer protocol */
	s16 hlim;                /* -1: default (unicast) / 1 (multicast) */
	u8  tclass;
	u8  dontfrag;            /* IPV6_DONTFRAG: EMSGSIZE instead of fragmenting */
	u8  dontroute;
	u8  mcast_noloop;        /* IPV6_MULTICAST_LOOP 0 */
	u8  router_alert;        /* hop-by-hop Router Alert (MLD) */
	u32 flowlabel;           /* low 20 bits */
	struct rt_netif *oif;    /* interface for link-local/multicast, or NULL */
};
/* b->data at the upper layer header (its checksum must already be
 * filled in using o->src; with o->src unspecified the caller first calls
 * rt_ip6_select_source()). Consumes b. 0 or -errno. */
int  rt_ip6_output(struct rt_buf *b, const struct rt_ip6_out *o);
/* Pseudo header sum (RFC 8200 8.1); partial sum like rt_csum_pseudo(). */
u32  rt_csum_pseudo6(const rt_in6_addr *src, const rt_in6_addr *dst, u8 nxt, u32 len);

/* ---- input / transports ----
 * Transports get b->data at their header and b->len their length; 'ip6'
 * is the fixed header (valid while b is). */
void rt_ip6_input(struct rt_netif *nif, struct rt_buf *b);
void rt_tcp_input6(struct rt_buf *b, const struct rt_ip6hdr *ip6);
void rt_udp_input6(struct rt_buf *b, const struct rt_ip6hdr *ip6);
/* raw IPv6 sockets of protocol nxt get a copy; b not consumed */
int  rt_raw6_input(const struct rt_buf *b, const struct rt_ip6hdr *ip6, u8 nxt);
/* ICMPv6 errors for transports: inner is the quoted IPv6 header,
 * 'tp' the quoted upper layer header (inner_len bytes from inner on),
 * proto its protocol after the quoted extension headers. */
void rt_tcp_icmp6_error(u8 type, u8 code, u32 info, const struct rt_ip6hdr *inner,
                        const u8 *th, u32 thlen);
void rt_udp_icmp6_error(u8 type, u8 code, u32 info, const struct rt_ip6hdr *inner,
                        const u8 *uh, u32 uhlen);

/* ---- ICMPv6 (RFC 4443) ---- */
#define RT_ICMP6_DST_UNREACH       1
#define   RT_ICMP6_DST_UNREACH_NOROUTE   0
#define   RT_ICMP6_DST_UNREACH_ADMIN     1
#define   RT_ICMP6_DST_UNREACH_BEYONDSCOPE 2
#define   RT_ICMP6_DST_UNREACH_ADDR      3
#define   RT_ICMP6_DST_UNREACH_NOPORT    4
#define RT_ICMP6_PACKET_TOO_BIG    2
#define RT_ICMP6_TIME_EXCEEDED     3
#define   RT_ICMP6_TIME_EXCEED_TRANSIT   0
#define   RT_ICMP6_TIME_EXCEED_REASSEMBLY 1
#define RT_ICMP6_PARAM_PROB        4
#define   RT_ICMP6_PARAMPROB_HEADER      0
#define   RT_ICMP6_PARAMPROB_NEXTHEADER  1
#define   RT_ICMP6_PARAMPROB_OPTION      2
#define RT_ICMP6_ECHO_REQUEST      128
#define RT_ICMP6_ECHO_REPLY        129
#define RT_ICMP6_MLD_QUERY         130
#define RT_ICMP6_MLD_REPORT        131
#define RT_ICMP6_MLD_DONE          132
#define RT_ND_ROUTER_SOLICIT       133
#define RT_ND_ROUTER_ADVERT        134
#define RT_ND_NEIGHBOR_SOLICIT     135
#define RT_ND_NEIGHBOR_ADVERT      136
#define RT_ND_REDIRECT             137
#define RT_ICMP6_MLDV2_REPORT      143

/* Send an ICMPv6 error about 'orig' (data at its IPv6 header). Applies
 * RFC 4443 2.4 (e) and (f) and the rate limit. Not consumed. */
void rt_icmp6_send_error(const struct rt_buf *orig, u8 type, u8 code, u32 info);

/* ---- statistics (RFC 4293 / Linux Ip6 / Icmp6 names) ---- */
struct rt_ip6_stats {
	u32 InReceives, InHdrErrors, InTooBigErrors, InNoRoutes, InAddrErrors,
	    InUnknownProtos, InTruncatedPkts, InDiscards, InDelivers, OutForwDatagrams,
	    OutRequests, OutDiscards, OutNoRoutes, ReasmTimeout, ReasmReqds, ReasmOKs,
	    ReasmFails, FragOKs, FragFails, FragCreates, InMcastPkts, OutMcastPkts;
};
struct rt_icmp6_stats {
	u32 InMsgs, InErrors, InDestUnreachs, InPktTooBigs, InTimeExcds, InParmProblems,
	    InEchos, InEchoReplies, InRouterSolicits, InRouterAdvertisements,
	    InNeighborSolicits, InNeighborAdvertisements, InRedirects, InMLDQueries,
	    OutMsgs, OutErrors, OutDestUnreachs, OutPktTooBigs, OutTimeExcds, OutParmProblems,
	    OutEchoReplies, OutRouterSolicits, OutNeighborSolicits, OutNeighborAdvertisements,
	    OutMLDv2Reports, OutRateLimited, DadDuplicates;
};
extern struct rt_ip6_stats   rt_ip6stat;
extern struct rt_icmp6_stats rt_icmp6stat;

/* ---- resolver hooks: RDNSS/DNSSL from router advertisements (RFC 8106)
 * and DHCPv6 information (RFC 8415). Lock held. ---- */
int  rt_dns_add_server6(const rt_in6_addr *a, u32 ifindex, u32 lifetime_s, int source);
void rt_dns_remove_server6(const rt_in6_addr *a);
#define RT_DNS_SRC_STATIC 0
#define RT_DNS_SRC_RA     1
#define RT_DNS_SRC_DHCP6  2

/* ---- init (rt_init) ---- */
void rt_ip6_init(void);

#endif
