/*
 * Riptide - network interfaces.
 *
 * Drivers (SANA-II on AmigaOS, TUN / simulated wire / loopback on the host)
 * hand received packets to rt_netif_input() with the link header already
 * removed, and transmit through netif->link_output() which receives an L3
 * packet plus the link destination and ethertype. The driver adds whatever
 * framing it needs (SANA-II "cooked" mode does it in the device driver).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_NETIF_H
#define RT_NETIF_H

#include "rt_types.h"
#include "rt_buf.h"

#define RT_IFNAMSIZ 16
#define RT_MAX_HWADDR 16

/* interface types */
#define RT_IFT_ETHER   1   /* broadcast, 6 byte addresses, ARP */
#define RT_IFT_PTP     2   /* point to point, no link addresses (PPP, SLIP, TUN) */
#define RT_IFT_LOOP    3   /* loopback */

/* flags (BSD IFF_* values, so SIOCGIFFLAGS can return them unchanged) */
#define RT_IFF_UP          0x0001
#define RT_IFF_BROADCAST   0x0002
#define RT_IFF_DEBUG       0x0004
#define RT_IFF_LOOPBACK    0x0008
#define RT_IFF_POINTOPOINT 0x0010
#define RT_IFF_NOTRAILERS  0x0020
#define RT_IFF_RUNNING     0x0040
#define RT_IFF_NOARP       0x0080
#define RT_IFF_PROMISC     0x0100
#define RT_IFF_ALLMULTI    0x0200
#define RT_IFF_SIMPLEX     0x0800
#define RT_IFF_MULTICAST   0x8000

/* ethertypes */
#define RT_ETH_P_IP   0x0800
#define RT_ETH_P_ARP  0x0806
#define RT_ETH_P_IPV6 0x86dd

struct rt_netif;

/* Transmit an L3 packet. dsthw == NULL means link broadcast (or "the peer"
 * on point to point links). Ownership of b passes to the driver, which must
 * rt_buf_free() it eventually. Return 0 or -errno (the buffer is consumed
 * either way). */
typedef int (*rt_link_output_fn)(struct rt_netif *nif, struct rt_buf *b,
                                 const u8 *dsthw, u16 ethertype);

struct rt_netif_stats {
	u32 ipackets, ierrors, opackets, oerrors, collisions;
	u32 ibytes, obytes, imcasts, omcasts, iqdrops, noproto;
};

struct rt_netif {
	struct rt_netif *next;
	char  name[RT_IFNAMSIZ];
	u32   index;             /* 1.. */
	u8    type;              /* RT_IFT_* */
	u8    hwlen;
	u16   flags;             /* RT_IFF_* */
	u16   mtu;               /* L3 MTU */
	u8    hwaddr[RT_MAX_HWADDR];
	/* IPv4 configuration, network byte order; 0 = unset */
	u32   addr;
	u32   netmask;
	u32   broadaddr;         /* broadcast (ETHER) */
	u32   dstaddr;           /* peer (PTP) */
	u32   metric;
	rt_link_output_fn link_output;
	/* optional: called when flags change (e.g. promisc, up/down) */
	void (*ioctl_flags)(struct rt_netif *nif, u16 newflags);
	void *driver;            /* driver private */
	struct rt_netif_stats stats;
	/* bookkeeping for the link layer (ARP) */
	void *arp_priv;
	/* driver description for tools, e.g. "uaenet.device unit 0" */
	char  descr[48];
	/* optional: add (join = 1) or remove a link layer multicast address
	 * in the receive filter (IPv6: 33:33:xx:xx:xx:xx). NULL if the driver
	 * receives every multicast frame. Called with the lock held; return
	 * 0 or -errno (failures only cost the reception of that group). */
	int (*mcast)(struct rt_netif *nif, const u8 *hwaddr, int join);
	/* IPv6 state (src/core/ip6_if.c), NULL while IPv6 is off */
	void *ip6;
	u8    ip6_disabled;      /* "ipv6 off": not enabled automatically */
};

/* Register an interface (assigns index, name must be set). The interface
 * starts down; set address with rt_netif_set_addr() then rt_netif_up(). */
int  rt_netif_add(struct rt_netif *nif);
void rt_netif_remove(struct rt_netif *nif);  /* purges routes, ARP entries */
struct rt_netif *rt_netif_list(void);        /* head of list */
struct rt_netif *rt_netif_find(const char *name);
struct rt_netif *rt_netif_by_index(u32 idx);
/* Interface owning local address 'addr' (network order), or NULL. */
struct rt_netif *rt_netif_by_addr(u32 addr);
/* Is 'addr' one of our addresses or a broadcast address of an interface? */
int  rt_ip_is_local(u32 addr);
int  rt_ip_is_broadcast(u32 addr, const struct rt_netif *nif);

/* Sets addr/mask/broadcast-or-peer and installs the connected route.
 * For PTP interfaces 'peer' is the remote address; otherwise the
 * broadcast address is derived when peer == 0. */
int  rt_netif_set_addr(struct rt_netif *nif, u32 addr, u32 netmask, u32 peer);
void rt_netif_up(struct rt_netif *nif);
void rt_netif_down(struct rt_netif *nif);

/* Driver -> stack. b->data points at the L3 header. srchw may be NULL.
 * flags: RT_BUFF_BCAST/MCAST. Must be called with the stack lock held. */
void rt_netif_input(struct rt_netif *nif, struct rt_buf *b, u16 ethertype,
                    const u8 *srchw, u16 flags);

/* Stack -> link: send an IP packet to next hop 'nexthop' (network order)
 * on 'nif'. Resolves ARP on ethernet-like links (queueing while
 * unresolved). Consumes b. */
int  rt_netif_output_ip(struct rt_netif *nif, struct rt_buf *b, u32 nexthop);

/* Every transmission goes through here (packet capture), never through
 * nif->link_output() directly. Consumes b. */
int  rt_netif_xmit(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype);

/* The loopback interface "lo0" (127.0.0.1/8), created by rt_init(). */
extern struct rt_netif *rt_loopback_if;

/* ---- ARP (src/core/arp.c) ---- */
struct rt_arp_entry_info {
	u32 ip;                  /* network order */
	u8  hw[6];
	u8  state;               /* 0 incomplete 1 reachable 2 stale 3 static */
	u32 age_s;
	u32 ifindex;
};
void rt_arp_input(struct rt_netif *nif, struct rt_buf *b);
int  rt_arp_resolve_output(struct rt_netif *nif, struct rt_buf *b, u32 nexthop);
void rt_arp_flush_if(struct rt_netif *nif);
int  rt_arp_add_static(struct rt_netif *nif, u32 ip, const u8 *hw);
int  rt_arp_delete(u32 ip);
/* Look up the entry for ip (on nif, or any interface if NULL). 0 or -RT_ENOENT. */
int  rt_arp_get(const struct rt_netif *nif, u32 ip, struct rt_arp_entry_info *out);
/* Fill up to max entries; returns total number of entries. */
int  rt_arp_list(struct rt_arp_entry_info *out, int max);
/* Gratuitous ARP / probe (used by DHCP for conflict detection). */
void rt_arp_announce(struct rt_netif *nif, u32 ip);

#endif
