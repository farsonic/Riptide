/*
 * Riptide - network interface list, addressing and L3 -> link dispatch.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"

static struct rt_netif *netif_list;
static u32 netif_next_index = 1;

void rt_netif_init(void)
{
	netif_list = NULL;
	netif_next_index = 1;
}

struct rt_netif *rt_netif_list(void)
{
	return netif_list;
}

struct rt_netif *rt_netif_find(const char *name)
{
	struct rt_netif *n;

	if (!name)
		return NULL;
	for (n = netif_list; n; n = n->next)
		if (strncmp(n->name, name, RT_IFNAMSIZ) == 0)
			return n;
	return NULL;
}

struct rt_netif *rt_netif_by_index(u32 idx)
{
	struct rt_netif *n;

	for (n = netif_list; n; n = n->next)
		if (n->index == idx)
			return n;
	return NULL;
}

struct rt_netif *rt_netif_by_addr(u32 addr)
{
	struct rt_netif *n;

	if (addr == 0)
		return NULL;
	for (n = netif_list; n; n = n->next)
		if (rt_netif_has_addr(n, addr))
			return n;
	return NULL;
}

int rt_netif_has_addr(const struct rt_netif *nif, u32 addr)
{
	int i;

	if (!addr)
		return 0;
	if (nif->addr == addr)
		return 1;
	for (i = 0; i < nif->nalias; i++)
		if (nif->alias[i].addr == addr)
			return 1;
	return 0;
}

static int on_subnet(u32 a, u32 ifa, u32 mask)
{
	return ifa && mask && ((a ^ ifa) & mask) == 0;
}

int rt_netif_on_link(const struct rt_netif *nif, u32 dst)
{
	int i;

	if (on_subnet(dst, nif->addr, nif->netmask))
		return 1;
	for (i = 0; i < nif->nalias; i++)
		if (on_subnet(dst, nif->alias[i].addr, nif->alias[i].netmask))
			return 1;
	return 0;
}

u32 rt_netif_src(const struct rt_netif *nif, u32 dst, u32 nexthop)
{
	int i, pass;

	if (!nif->nalias)
		return nif->addr;
	if (!nexthop)
		nexthop = dst;
	/* the address on the next hop's subnet, then on the destination's */
	for (pass = 0; pass < 2; pass++) {
		u32 a = pass ? dst : nexthop;
		if (on_subnet(a, nif->addr, nif->netmask))
			return nif->addr;
		for (i = 0; i < nif->nalias; i++)
			if (on_subnet(a, nif->alias[i].addr, nif->alias[i].netmask))
				return nif->alias[i].addr;
	}
	return nif->addr ? nif->addr : nif->alias[0].addr;
}

int rt_netif_addrs(const struct rt_netif *nif, struct rt_ifaddr4 *out, int max)
{
	int i, n = 0;

	if (nif->addr) {
		if (n < max) {
			out[n].addr = nif->addr;
			out[n].netmask = nif->netmask;
			out[n].broadaddr = nif->broadaddr;
		}
		n++;
	}
	for (i = 0; i < nif->nalias; i++, n++)
		if (n < max)
			out[n] = nif->alias[i];
	return n;
}

int rt_netif_add(struct rt_netif *nif)
{
	struct rt_netif **pp;

	if (!nif || !nif->name[0] || !nif->link_output)
		return -RT_EINVAL;
	nif->name[RT_IFNAMSIZ - 1] = '\0';
	if (rt_netif_find(nif->name))
		return -RT_EEXIST;
	if (nif->mtu == 0)
		nif->mtu = 1500;
	if (nif->mtu < 68)
		return -RT_EINVAL;
	nif->index = netif_next_index++;
	if (!rt_ip4_auto_enable && nif->type != RT_IFT_LOOP)
		nif->ip4_disabled = 1;
	nif->flags &= (u16)~(RT_IFF_UP | RT_IFF_RUNNING);
	nif->next = NULL;
	for (pp = &netif_list; *pp; pp = &(*pp)->next)
		;
	*pp = nif;
	rt_route_changed();
	return 0;
}

void rt_netif_remove(struct rt_netif *nif)
{
	struct rt_netif **pp;

	rt_route_changed();        /* cached routes may point at nif */
	if (nif->nvlan)
		rt_vlan_parent_removed(nif);   /* its VLAN interfaces go first */
	rt_dhcp_stop(nif);
	rt_netif_down(nif);
	rt_qos_enable(nif, 0);
	rt_ip6_ifenable(nif, 0);
	rt_mdns_if_gone(nif);
	rt_ip_mc_flush(nif);
	rt_route_flush_if(nif);
	rt_arp_flush(nif, 0);
	for (pp = &netif_list; *pp; pp = &(*pp)->next) {
		if (*pp == nif) {
			*pp = nif->next;
			break;
		}
	}
	nif->next = NULL;
}

/* Is addr the broadcast address of the subnet of address 'ifa'? */
static int bcast_of(u32 addr, u32 ifa, u32 mask, u32 broadaddr)
{
	if (broadaddr && addr == broadaddr)
		return 1;
	/* /31 and /32 have no broadcast address (RFC 3021) */
	if (mask == 0xffffffffu || mask == rt_htonl(0xfffffffeu))
		return 0;
	return (addr & mask) == (ifa & mask) && ((addr & ~mask) == ~mask || (addr & ~mask) == 0);
}

/* BSD in_broadcast(): limited broadcast, the old all-zeros broadcast, and
 * the directed broadcast (all ones or all zeros host part) of any
 * broadcast capable interface - restricted to 'nif' when given. */
int rt_ip_is_broadcast(u32 addr, const struct rt_netif *nif)
{
	const struct rt_netif *n;

	if (addr == RT_INADDR_BROADCAST || addr == RT_INADDR_ANY)
		return 1;
	for (n = nif ? nif : netif_list; n; n = nif ? NULL : n->next) {
		int i;
		if (!(n->flags & RT_IFF_BROADCAST))
			continue;
		if (n->addr && bcast_of(addr, n->addr, n->netmask, n->broadaddr))
			return 1;
		for (i = 0; i < n->nalias; i++)
			if (bcast_of(addr, n->alias[i].addr, n->alias[i].netmask, n->alias[i].broadaddr))
				return 1;
	}
	return 0;
}

/* Weak host model (RFC 1122 3.3.4.2, as BSD and Linux): an address of any
 * interface is ours whatever interface the packet came in on. */
int rt_ip_is_local(u32 addr)
{
	const struct rt_netif *n;

	if (addr == 0)
		return 0;
	if (rt_ip_is_loopback_net(addr))
		return rt_loopback_if && (rt_loopback_if->flags & RT_IFF_UP);
	for (n = netif_list; n; n = n->next)
		if ((n->flags & RT_IFF_UP) && rt_netif_has_addr(n, addr))
			return 1;
	return rt_ip_is_broadcast(addr, NULL);
}

static int mask_is_contiguous(u32 mask_n)
{
	u32 inv = ~rt_ntohl(mask_n);    /* must look like 0..01..1 */
	return (inv & (inv + 1)) == 0;
}

/* the connected routes of the primary and the additional addresses (one
 * per prefix: rt_route_add() refuses duplicates) */
static void install_connected(struct rt_netif *nif)
{
	int i;

	if (nif->addr) {
		if (nif->type == RT_IFT_PTP && nif->dstaddr)
			rt_route_add(nif->dstaddr, 0xffffffffu, 0, nif,
			             RT_RTF_HOST | RT_RTF_CONNECTED, nif->metric);
		else
			rt_route_add(nif->addr & nif->netmask, nif->netmask, 0, nif,
			             RT_RTF_CONNECTED, nif->metric);
	}
	for (i = 0; i < nif->nalias; i++)
		rt_route_add(nif->alias[i].addr & nif->alias[i].netmask, nif->alias[i].netmask, 0, nif,
		             RT_RTF_CONNECTED, nif->metric);
}

static void reinstall_connected(struct rt_netif *nif)
{
	rt_route_del_connected(nif);
	install_connected(nif);
}

static u32 default_bcast(const struct rt_netif *nif, u32 addr, u32 netmask, u32 bcast)
{
	if (!(nif->flags & RT_IFF_BROADCAST))
		return 0;
	if (netmask == 0xffffffffu || netmask == rt_htonl(0xfffffffeu))
		return bcast;
	return bcast ? bcast : (addr | ~netmask);
}

static int alias_find(const struct rt_netif *nif, u32 addr)
{
	int i;

	for (i = 0; i < nif->nalias; i++)
		if (nif->alias[i].addr == addr)
			return i;
	return -1;
}

static void alias_remove(struct rt_netif *nif, int i)
{
	memmove(&nif->alias[i], &nif->alias[i + 1], (u32)(nif->nalias - i - 1) * sizeof(nif->alias[0]));
	nif->nalias--;
}

int rt_netif_addr_add(struct rt_netif *nif, u32 addr, u32 netmask, u32 bcast)
{
	int i;

	if (!addr || !mask_is_contiguous(netmask) || rt_ip_is_multicast(addr) ||
	    addr == RT_INADDR_BROADCAST)
		return -RT_EINVAL;
	if (nif->ip4_disabled)
		return -RT_EAFNOSUPPORT;
	if (!nif->addr || nif->addr == addr)
		return rt_netif_set_addr(nif, addr, netmask, bcast);
	if (nif->type == RT_IFT_PTP)
		return -RT_EOPNOTSUPP;     /* point to point: one address and its peer */
	i = alias_find(nif, addr);
	if (i < 0) {
		if (nif->nalias >= RT_IF_MAXADDR4)
			return -RT_ENOSPC;
		i = nif->nalias++;
	}
	nif->alias[i].addr = addr;
	nif->alias[i].netmask = netmask;
	nif->alias[i].broadaddr = default_bcast(nif, addr, netmask, bcast);
	rt_route_changed();
	reinstall_connected(nif);
	if ((nif->flags & RT_IFF_UP) && nif->type == RT_IFT_ETHER)
		rt_arp_announce(nif, addr);
	return 0;
}

int rt_netif_addr_del(struct rt_netif *nif, u32 addr)
{
	int i;

	if (addr && addr == nif->addr) {
		struct rt_ifaddr4 a;
		if (!nif->nalias)
			return rt_netif_set_addr(nif, 0, 0, 0);
		/* promote the first additional address */
		a = nif->alias[0];
		alias_remove(nif, 0);
		return rt_netif_set_addr(nif, a.addr, a.netmask, a.broadaddr);
	}
	i = alias_find(nif, addr);
	if (i < 0)
		return -RT_EADDRNOTAVAIL;
	alias_remove(nif, i);
	rt_route_changed();
	rt_arp_flush(nif, 1);
	reinstall_connected(nif);
	return 0;
}

int rt_netif_set_addr(struct rt_netif *nif, u32 addr, u32 netmask, u32 peer)
{
	u32 old = nif->addr;

	rt_route_changed();
	if (!mask_is_contiguous(netmask))
		return -RT_EINVAL;
	if (addr && (rt_ip_is_multicast(addr) || addr == RT_INADDR_BROADCAST))
		return -RT_EINVAL;
	if (addr && nif->ip4_disabled)
		return -RT_EAFNOSUPPORT;
	rt_route_del_connected(nif);
	rt_arp_flush(nif, 1);
	/* an additional address that becomes the primary one */
	if (addr && alias_find(nif, addr) >= 0)
		alias_remove(nif, alias_find(nif, addr));
	nif->addr = addr;
	nif->netmask = addr ? netmask : 0;
	nif->broadaddr = 0;
	nif->dstaddr = 0;
	if (!addr) {
		install_connected(nif);    /* the additional addresses' */
		return 0;
	}
	if (nif->type == RT_IFT_PTP) {
		nif->dstaddr = peer;
	} else if (nif->flags & RT_IFF_BROADCAST) {
		if (netmask == 0xffffffffu || netmask == rt_htonl(0xfffffffeu))
			nif->broadaddr = peer;     /* none unless given */
		else
			nif->broadaddr = peer ? peer : (addr | ~netmask);
	}
	install_connected(nif);
	if ((nif->flags & RT_IFF_UP) && nif->type == RT_IFT_ETHER)
		rt_arp_announce(nif, addr);
	/* reports sent before had source 0.0.0.0 (or the old address) */
	if ((nif->flags & RT_IFF_UP) && addr != old)
		rt_igmp_if_up(nif);
	return 0;
}

void rt_netif_up(struct rt_netif *nif)
{
	u16 nf = nif->flags | RT_IFF_UP | RT_IFF_RUNNING;

	/* a VLAN interface receives through its parent: that comes up too,
	 * and the VLAN runs only while the parent does */
	if (nif->vlan_parent) {
		if (!(nif->vlan_parent->flags & RT_IFF_UP))
			rt_netif_up(nif->vlan_parent);
		if (!(nif->vlan_parent->flags & RT_IFF_RUNNING))
			nf &= (u16)~RT_IFF_RUNNING;
	}
	if (nif->ioctl_flags && nf != nif->flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
	rt_route_changed();
	if (nif->type == RT_IFT_ETHER) {
		int i;
		if (nif->addr)
			rt_arp_announce(nif, nif->addr);
		for (i = 0; i < nif->nalias; i++)
			rt_arp_announce(nif, nif->alias[i].addr);
	}
	rt_igmp_if_up(nif);
	rt_ip6_if_up(nif);
	rt_vlan_parent_changed(nif);
	rt_kick();
}

void rt_netif_down(struct rt_netif *nif)
{
	u16 nf = nif->flags & (u16)~(RT_IFF_UP | RT_IFF_RUNNING);

	if (nif->ioctl_flags && nf != nif->flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
	rt_route_changed();
	rt_arp_flush(nif, 1);
	/* Configured routes stay (unused while down, shown as linkdown);
	 * learned ones go, as their information may be stale when it is
	 * back up. */
	rt_route_flush_dynamic_if(nif);
	rt_igmp_if_down(nif);
	rt_ip6_if_down(nif);
	rt_vlan_parent_changed(nif);
	/* UDP senders waiting for room find out (the send then fails) */
	if (nif->tx_waiters) {
		nif->tx_waiters = 0;
		rt_udp_tx_wake(nif);
	}
}

void rt_netif_input(struct rt_netif *nif, struct rt_buf *b, u16 ethertype,
                    const u8 *srchw, u16 flags)
{
	RT_PROF_EV(RT_EV_DRV_RX);
	nif->stats.ipackets++;
	nif->stats.ibytes += b->len;
	if (flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))
		nif->stats.imcasts++;
	if (rt_capture_on)
		rt_capture_frame(nif, b, RT_CAP_IN, ethertype, srchw, flags);
	if (!(nif->flags & RT_IFF_UP)) {
		nif->stats.iqdrops++;
		rt_buf_free(b);
		return;
	}
	rt_netif_deliver(nif, b, ethertype, srchw, flags);
}

void rt_netif_deliver(struct rt_netif *nif, struct rt_buf *b, u16 ethertype, const u8 *srchw, u16 flags)
{
	b->netif = nif;
	b->flags = (u16)((b->flags & (RT_BUFF_LOOP | RT_BUFF_CSUM_OK | RT_BUFF_CSUM_PART)) |
	                 (flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)));
	b->l3_proto = ethertype;
	switch (ethertype) {
	case RT_ETH_P_IP:
		if (nif->ip4_disabled)
			break;
		rt_ip_input(nif, b);
		return;
	case RT_ETH_P_ARP:
		if (nif->type == RT_IFT_ETHER && !nif->ip4_disabled) {
			rt_arp_input(nif, b);
			return;
		}
		break;
	case RT_ETH_P_IPV6:
		if (nif->ip6) {
			rt_ip6_input(nif, b);
			return;
		}
		break;
	case RT_ETH_P_8021Q:
		if (nif->type == RT_IFT_ETHER) {
			rt_vlan_input(nif, b, srchw, flags);
			return;
		}
		break;
	}
	nif->stats.noproto++;
	rt_buf_free(b);
}

u32 rt_netif_tx_last;

/* one timer for all interfaces: it wakes every UDP sender that waits
 * for room, and they look again (a stalled driver now has room) */
static struct rt_timer tx_stall_timer;

static void tx_stall_fire(void *arg)
{
	struct rt_netif *n;

	for (n = netif_list; n; n = n->next) {
		if (n->tx_waiters) {
			n->tx_waiters = 0;
			rt_udp_tx_wake(n);
		}
	}
}

void rt_netif_tx_wait(struct rt_netif *nif)
{
	const struct rt_drv_stats *d = nif->drv;
	u32 left = RT_NETIF_TX_STALL_MS, due;

	nif->tx_waiters = 1;
	if (d && d->tx_inflight) {
		s32 el = (s32)(rt_now_ms() - d->tx_progress_ms);
		left = el >= RT_NETIF_TX_STALL_MS ? 1 : (u32)(RT_NETIF_TX_STALL_MS - el);
	}
	if (!tx_stall_timer.fn)
		rt_timer_init(&tx_stall_timer, tx_stall_fire, NULL);
	/* a completion meanwhile at most makes it fire early: the waiters
	 * look again and wait again */
	due = rt_now_ms() + left;
	if (!rt_timer_pending(&tx_stall_timer) || (s32)(tx_stall_timer.expires - due) > 0)
		rt_timer_mod(&tx_stall_timer, due);
}

int rt_netif_xmit(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	RT_PROF_EV(RT_EV_DRV_TX);
	rt_netif_tx_last = nif->index;
	if (nif->qos)
		return rt_qos_xmit(nif, b, dsthw, ethertype);
	if (rt_capture_on)
		rt_capture_frame(nif, b, RT_CAP_OUT, ethertype, NULL, 0);
	return nif->link_output(nif, b, dsthw, ethertype);
}

int rt_ip4_auto_enable = 1;

int rt_netif_ip4_enable(struct rt_netif *nif, int on)
{
	rt_route_changed();
	if (nif->type == RT_IFT_LOOP)
		return on ? 0 : -RT_EINVAL;
	if (on) {
		nif->ip4_disabled = 0;
		return 0;
	}
	if (nif->ip4_disabled)
		return 0;
	rt_dhcp_stop(nif);
	nif->nalias = 0;
	rt_netif_set_addr(nif, 0, 0, 0);
	rt_route_flush_if(nif);
	rt_arp_flush(nif, 0);
	rt_ip_mc_flush(nif);
	nif->ip4_disabled = 1;
	return 0;
}

static int netif_output_ip(struct rt_netif *nif, struct rt_buf *b, u32 nexthop, int unicast)
{
	u8 mc[6];
	int r;

	if (!(nif->flags & RT_IFF_UP) || nif->ip4_disabled) {
		nif->stats.oerrors++;
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	switch (nif->type) {
	case RT_IFT_ETHER:
		if (unicast) {
			return rt_arp_resolve_output(nif, b, nexthop);
		} else if (rt_ip_is_broadcast(nexthop, nif)) {
			nif->stats.omcasts++;
			r = rt_netif_xmit(nif, b, NULL, RT_ETH_P_IP);
		} else if (rt_ip_is_multicast(nexthop)) {
			/* RFC 1112 6.4: 01:00:5e + low 23 bits of the group */
			u32 h = rt_ntohl(nexthop);
			mc[0] = 0x01; mc[1] = 0x00; mc[2] = 0x5e;
			mc[3] = (u8)((h >> 16) & 0x7f); mc[4] = (u8)(h >> 8); mc[5] = (u8)h;
			nif->stats.omcasts++;
			r = rt_netif_xmit(nif, b, mc, RT_ETH_P_IP);
		} else {
			return rt_arp_resolve_output(nif, b, nexthop);
		}
		break;
	case RT_IFT_PTP:
	case RT_IFT_LOOP:
	default:
		/* loopback's link_output is the deferred queue of loopback.c */
		r = rt_netif_xmit(nif, b, NULL, RT_ETH_P_IP);
		break;
	}
	if (r < 0)
		nif->stats.oerrors++;
	return r;
}

int rt_netif_output_ip(struct rt_netif *nif, struct rt_buf *b, u32 nexthop)
{
	return netif_output_ip(nif, b, nexthop, 0);
}

int rt_netif_output_ip_unicast(struct rt_netif *nif, struct rt_buf *b, u32 nexthop)
{
	return netif_output_ip(nif, b, nexthop, 1);
}
