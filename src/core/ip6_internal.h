/*
 * Riptide - internal interfaces of the IPv6 modules (ip6.c, ip6_frag.c,
 * ip6_if.c, nd6.c, route6.c, icmp6.c, dhcp6.c). Not a contract header.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_IP6_INTERNAL_H
#define RT_IP6_INTERNAL_H

#include "infra.h"
#include "rt/rt_ip6.h"

/* ---- tunables (net.inet6.*) ---- */
extern s32 rt_ip6_forwarding;       /* net.inet6.ip6.forwarding */
extern s32 rt_ip6_hlim;             /* net.inet6.ip6.hlim */
extern s32 rt_ip6_accept_rtadv;     /* net.inet6.ip6.accept_rtadv */
extern s32 rt_ip6_use_tempaddr;     /* net.inet6.ip6.use_tempaddr */
extern s32 rt_ip6_auto_enable;      /* net.inet6.ip6.enable: IPv6 on new interfaces */
extern s32 rt_ip6_dad_count;        /* net.inet6.ip6.dad_count */
extern s32 rt_ip6_addr_gen_mode;    /* 0 EUI-64, 1 RFC 7217 stable privacy */
extern s32 rt_ip6_mtu_expires;      /* PMTU lifetime (s) */
extern char rt_ip6_stable_secret[40];

/* ---- per interface state (ip6_if.c), nif->ip6 ---- */
struct in6_ifaddr {
	struct in6_ifaddr *next;
	struct ip6_if *ifp;
	rt_in6_addr addr;
	u32 valid_until;            /* rt_ip6_seconds() deadline, 0 = forever */
	u32 pref_until;             /* idem */
	u32 created_s;
	struct rt_timer dad_timer;
	u16 flags;                  /* RT_IN6_IFF_* */
	u8  plen;
	u8  dad_left;               /* DAD probes still to send */
	u8  dad_counter;            /* RFC 7217 DAD_Counter */
	u8  prefix_route;           /* we installed plen's on-link route */
	u8  nonce[6];               /* RFC 7527 nonce of our DAD probes */
};

struct in6_multi {
	struct in6_multi *next;
	rt_in6_addr grp;
	u16 refcnt;
	u8  unsolicited;            /* unsolicited reports still to send */
	u8  query_pending;          /* answer the last query at mld_resp_s */
};

struct ip6_if {
	struct rt_netif *nif;
	struct in6_ifaddr *addrs;
	struct in6_multi *mcast;
	struct rt_timer timer;      /* router solicitation and MLD reports */
	u32 mtu;                    /* RA MTU option, 0 = interface MTU */
	u32 base_reachable_ms, reachable_ms, retrans_ms;
	u32 reachable_set_ms;       /* when reachable_ms was last randomised */
	u32 rs_next_ms, mld_next_ms, mld_resp_ms;
	u32 mldv1_until_s;          /* MLDv1 querier present (RFC 3810 8.2.1) */
	u8  cur_hlim;               /* RA Cur Hop Limit, 0 = net.inet6.ip6.hlim */
	u8  rs_left;                /* solicitations still to send */
	u8  mld_pending;            /* state change report retransmissions */
	u8  ra_managed, ra_other;   /* M / O flags of the last RA */
	u8  dhcp6_started;
	u8  nd;                     /* uses neighbour discovery (Ethernet) */
	u8  iid_eui64[8];           /* modified EUI-64 interface identifier */
};

static inline struct ip6_if *ip6_ifp(const struct rt_netif *nif)
{
	return nif ? (struct ip6_if *)nif->ip6 : NULL;
}

/* ip6_if.c */
void ip6_if_init(void);
struct in6_ifaddr *in6_ifa_find(const struct rt_netif *nif, const rt_in6_addr *a);
/* Any address (usable, not tentative) of scope link on nif: NULL if none. */
struct in6_ifaddr *in6_ifa_linklocal(const struct rt_netif *nif, int usable);
/* DAD saw a duplicate (NS from :: or NA for a tentative address). */
void in6_dad_duplicate(struct in6_ifaddr *ia);
/* Our NS nonce? (RFC 7527: a looped back DAD probe) */
int  in6_dad_own_nonce(const struct rt_netif *nif, const u8 *nonce);
/* Router advertisement prefix information (RFC 4862 5.5.3). */
void in6_slaac_prefix(struct rt_netif *nif, const rt_in6_addr *prefix, int plen,
                      u32 valid_s, u32 pref_s);
/* MLD input (query / reports of others). */
void mld6_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len);
/* Randomised ReachableTime (RFC 4861 6.3.2). */
u32  ip6_reachable_ms(struct ip6_if *ifp);
/* Effective hop limit for unicast. */
u8   ip6_default_hlim(const struct rt_netif *nif);
/* 1 s housekeeping: lifetimes of addresses, routes, DNS servers. */
void ip6_if_slowtimo(u32 now_s);

/* nd6.c */
void nd6_init(void);
void nd6_input(struct rt_netif *nif, struct rt_buf *b, const struct rt_ip6hdr *ip6,
               const u8 *m, u32 len, int hlim_ok);
int  nd6_resolve_output(struct rt_netif *nif, struct rt_buf *b, const rt_in6_addr *nexthop);
/* Send a neighbour solicitation for target. src NULL: DAD (from ::). */
void nd6_send_ns(struct rt_netif *nif, const rt_in6_addr *target, const rt_in6_addr *src,
                 const u8 *unicast_hw, const u8 *nonce);
void nd6_send_na(struct rt_netif *nif, const rt_in6_addr *dst, const rt_in6_addr *target,
                 int solicited, int override);
void nd6_send_rs(struct rt_netif *nif);
/* Is there a neighbour entry for a router at addr that is not known to be
 * unreachable? (default router selection, RFC 4861 6.3.6) */
int  nd6_router_reachable(const struct rt_netif *nif, const rt_in6_addr *addr);
/* A neighbour sent a router advertisement or said it is no router. */
void nd6_router_flag(struct rt_netif *nif, const rt_in6_addr *addr, int is_router);

/* route6.c */
void route6_init(void);
void route6_slowtimo(u32 now_s);
/* add or refresh a learned route (RA): expires after lifetime_s, 0 deletes */
void route6_ra_update(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                      struct rt_netif *nif, u32 lifetime_s, s8 pref);
struct rt_route6 *route6_find(const rt_in6_addr *dst, int plen, const rt_in6_addr *gw,
                              const struct rt_netif *nif);
void route6_redirect(const rt_in6_addr *dst, const rt_in6_addr *target, struct rt_netif *nif,
                     const rt_in6_addr *from);
/* Is dst on a link of ours (connected prefix)? Returns the interface. */
struct rt_netif *route6_onlink(const rt_in6_addr *dst);

/* ip6.c */
int  ip6_send_link(struct rt_netif *nif, struct rt_buf *b, const rt_in6_addr *nexthop);
/* Build and send an IPv6 packet from link code (ND, MLD): no routing. */
int  ip6_output_direct(struct rt_buf *b, struct rt_netif *nif, const rt_in6_addr *src,
                       const rt_in6_addr *dst, u8 nxt, u8 hlim, int router_alert);
/* Copy b into a 4 byte aligned buffer with full headroom. Frees b. */
struct rt_buf *ip6_realign(struct rt_buf *b);
/* Walk the extension headers of a (quoted) packet. Returns the offset of
 * the upper layer header and its protocol in *nxt, or -1. Fragments other
 * than the first stop the walk (-1). */
int  ip6_skip_exthdr(const u8 *pkt, u32 len, u8 *nxt);

/* ip6_frag.c */
void ip6frag_init(void);
/* Fragment header at offset 'fhoff' of b (data at IPv6 header). Returns
 * the reassembled packet without fragment header (data at the IPv6
 * header, nxt fixed up) or NULL. *nxt_off is set to the offset of the
 * next header byte that described the fragment header. */
struct rt_buf *ip6frag_reass(struct rt_buf *b, u32 fhoff, u32 prev_nxt_off);
int  ip6frag_output(struct rt_buf *b, u32 unfrag_len, struct rt_netif *nif,
                    const rt_in6_addr *nexthop, u32 mtu);

/* icmp6.c */
void icmp6_init(void);
void icmp6_input(struct rt_buf *b, const struct rt_ip6hdr *ip6);
/* Build and send an ICMPv6 message (checksum filled) from src (may be
 * unspecified: selected) to dst. 'oif' for link-local/multicast. */
int  icmp6_send(struct rt_netif *oif, const rt_in6_addr *src, const rt_in6_addr *dst, u8 hlim,
                const u8 *hdr, u32 hlen, const void *data, u32 len);

/* dhcp6.c: stateless DHCPv6 (information request) */
void dhcp6_init(void);
void dhcp6_start_info(struct rt_netif *nif);
void dhcp6_stop(struct rt_netif *nif);

/* SipHash-2-4 (ip6_if.c): stable interface identifiers, flow labels */
u64  rt_siphash(const void *data, u32 len, const u8 key[16]);

#endif
