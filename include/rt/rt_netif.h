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

/* An additional IPv4 address of an interface (network byte order). */
#define RT_IF_MAXADDR4 8         /* additional addresses per interface */
struct rt_ifaddr4 {
	u32 addr;
	u32 netmask;
	u32 broadaddr;           /* broadcast (ETHER), 0 = none */
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
	u8    ip6_disabled;      /* 1 "ipv6 off": not enabled automatically; 2 "ipv6=on": always */
	/* optional driver counters (net.riptide.drivers), owned by the driver */
	struct rt_drv_stats *drv;
	/* "ifconfig IF tcpwindow N": the largest receive window TCP offers on
	 * connections routed over this interface (bytes, 0 = no limit; Linux
	 * "ip route ... window N" per interface) */
	u32   tcp_window;
	/* additional IPv4 addresses ("ip addr add", "ifconfig IF alias");
	 * addr/netmask/broadaddr above are the primary one */
	struct rt_ifaddr4 alias[RT_IF_MAXADDR4];
	u8    nalias;
	/* "ipv4 off" / "interface ... ipv4=off": no IPv4 (or ARP) here */
	u8    ip4_disabled;
	/* transmit priority queue (src/core/qos.c), NULL while not in use */
	void *qos;
};

/* ---- driver counters (src/core/drvstats.c) ----
 * Kept by a driver under the stack lock with the rt_drv_* helpers and
 * published as net.riptide.drivers (struct rt_ctl_driver). */
#define RT_DRV_RX_IP  0          /* receive queues */
#define RT_DRV_RX_ARP 1
#define RT_DRV_RX_IP6 2
#define RT_DRV_NRXQ   3
#define RT_DRV_NBATCH 6          /* 1, 2-3, 4-7, 8-15, 16-31, 32+ */
#define RT_DRV_NERR   8

struct rt_ctl_driver;
struct rt_drv_err {
	s32 code;
	u32 count;
};

struct rt_drv_stats {
	struct {
		u32 cfg, queued, low, empty;
	} rxq[RT_DRV_NRXQ];
	u32 rx_frames, rx_bytes, tx_frames, tx_bytes;
	u32 batch[RT_DRV_NBATCH], batch_max;
	u32 rx_nobuf, rx_errors, tx_errors;
	struct rt_drv_err rxerr[RT_DRV_NERR], txerr[RT_DRV_NERR];
	u32 tx_slots, tx_inflight, tx_inflight_max, tx_full;
	u32 reset_ms;            /* rt_now_ms() at the last reset */
	u32 warn_ms;             /* last "reads ran out" warning */
	u32 warn_empty;          /* empty count at that warning */
	u8  warned;
	u8  noqueue;             /* driver has no receive queue (RT_CTL_DRVF_NOQ) */
	/* optional: fill the driver specific fields of a record (kind,
	 * driver, unit, flags, dq_/gs_/special). Lock held; must not block. */
	void (*info)(struct rt_netif *nif, struct rt_ctl_driver *r);
	/* optional: called by rt_drv_reset() for the driver's own counters */
	void (*reset)(struct rt_netif *nif);
};

/* Prepare 'd' (zeroed by the caller) and attach it to nif. */
void rt_drv_attach(struct rt_netif *nif, struct rt_drv_stats *d);
/* A read for queue q was handed to the driver / came back (len bytes, or
 * err != 0: the driver's error code, counted in rxerr[]). */
void rt_drv_rx_queued(struct rt_netif *nif, int q);
void rt_drv_rx_done(struct rt_netif *nif, int q, u32 len, s32 err);
/* At a wakeup the driver still held 'avail' reads of queue q: updates the
 * low-water mark; avail 0 counts an empty queue and logs a warning at
 * most once a minute. */
void rt_drv_rx_avail(struct rt_netif *nif, int q, u32 avail);
/* 'n' frames were received in one wakeup (n > 0). */
void rt_drv_batch(struct rt_netif *nif, u32 n);
int  rt_drv_batch_bucket(u32 n);
void rt_drv_rx_nobuf(struct rt_netif *nif);
void rt_drv_tx_start(struct rt_netif *nif, u32 len);
void rt_drv_tx_done(struct rt_netif *nif, s32 err);
void rt_drv_tx_full(struct rt_netif *nif);
/* Count 'code' in an error table (fixed size, last slot for the rest). */
void rt_drv_count_err(struct rt_drv_err *tab, s32 code);
/* Zero the interface and driver counters (keeps cfg/queued/in flight). */
void rt_netif_reset_stats(struct rt_netif *nif);
/* Fill a net.riptide.drivers record; 0, or -RT_ENOENT without counters. */
int  rt_drv_fill(struct rt_netif *nif, struct rt_ctl_driver *r);

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

/* Additional IPv4 addresses. rt_netif_addr_add() makes 'addr' the primary
 * address when the interface has none, updates the mask/broadcast of an
 * address it has, and otherwise adds it (at most RT_IF_MAXADDR4) with its
 * connected route. rt_netif_addr_del() removes one; deleting the primary
 * promotes the first additional address (Linux promote_secondaries). Both
 * -errno or 0; lock held. */
int  rt_netif_addr_add(struct rt_netif *nif, u32 addr, u32 netmask, u32 bcast);
int  rt_netif_addr_del(struct rt_netif *nif, u32 addr);
/* Is addr the primary or an additional address of nif? */
int  rt_netif_has_addr(const struct rt_netif *nif, u32 addr);
/* Source address on nif for a packet to 'dst' (via 'nexthop', 0 = dst):
 * the address whose prefix holds the next hop or the destination, else
 * the primary, else the first additional address; 0 if none. */
u32  rt_netif_src(const struct rt_netif *nif, u32 dst, u32 nexthop);
/* Is dst on the subnet of one of nif's addresses? */
int  rt_netif_on_link(const struct rt_netif *nif, u32 dst);
/* Every IPv4 address of nif, primary first; returns the count. */
int  rt_netif_addrs(const struct rt_netif *nif, struct rt_ifaddr4 *out, int max);
/* ipv4 on|off for one interface: off removes its addresses, stops DHCP
 * and drops IPv4 and ARP input. */
int  rt_netif_ip4_enable(struct rt_netif *nif, int on);
/* "ipv4 off" without an interface: default for interfaces added later */
extern int rt_ip4_auto_enable;

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
/* A receive batch (lock held, may nest): frames that arrived together are
 * input between begin and end. TCP holds back the in-order ACKs the frames
 * call for and wakes each reader once, then end sends one ACK per
 * connection (if still due) and does the wakeups. Dup ACKs, ACKs asked
 * for explicitly and everything else go out at once as usual. */
void rt_rx_batch_begin(void);
void rt_rx_batch_end(void);

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
