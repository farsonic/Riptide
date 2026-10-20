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
		if (n->addr == addr)
			return n;
	return NULL;
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
	nif->flags &= (u16)~(RT_IFF_UP | RT_IFF_RUNNING);
	nif->next = NULL;
	for (pp = &netif_list; *pp; pp = &(*pp)->next)
		;
	*pp = nif;
	return 0;
}

void rt_netif_remove(struct rt_netif *nif)
{
	struct rt_netif **pp;

	rt_dhcp_stop(nif);
	rt_netif_down(nif);
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

/* BSD in_broadcast(): limited broadcast, the old all-zeros broadcast, and
 * the directed broadcast (all ones or all zeros host part) of any
 * broadcast capable interface - restricted to 'nif' when given. */
int rt_ip_is_broadcast(u32 addr, const struct rt_netif *nif)
{
	const struct rt_netif *n;

	if (addr == RT_INADDR_BROADCAST || addr == RT_INADDR_ANY)
		return 1;
	for (n = nif ? nif : netif_list; n; n = nif ? NULL : n->next) {
		u32 mask = n->netmask;
		if (!(n->flags & RT_IFF_BROADCAST) || !n->addr)
			continue;
		if (n->broadaddr && addr == n->broadaddr)
			return 1;
		/* /31 and /32 have no broadcast address (RFC 3021) */
		if (mask == 0xffffffffu || mask == rt_htonl(0xfffffffeu))
			continue;
		if ((addr & mask) == (n->addr & mask) &&
		    ((addr & ~mask) == ~mask || (addr & ~mask) == 0))
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
		if (n->addr == addr && (n->flags & RT_IFF_UP))
			return 1;
	return rt_ip_is_broadcast(addr, NULL);
}

static int mask_is_contiguous(u32 mask_n)
{
	u32 inv = ~rt_ntohl(mask_n);    /* must look like 0..01..1 */
	return (inv & (inv + 1)) == 0;
}

static void install_connected(struct rt_netif *nif)
{
	if (!nif->addr)
		return;
	if (nif->type == RT_IFT_PTP && nif->dstaddr)
		rt_route_add(nif->dstaddr, 0xffffffffu, 0, nif,
		             RT_RTF_HOST | RT_RTF_CONNECTED, nif->metric);
	else
		rt_route_add(nif->addr & nif->netmask, nif->netmask, 0, nif,
		             RT_RTF_CONNECTED, nif->metric);
}

int rt_netif_set_addr(struct rt_netif *nif, u32 addr, u32 netmask, u32 peer)
{
	if (!mask_is_contiguous(netmask))
		return -RT_EINVAL;
	if (addr && (rt_ip_is_multicast(addr) || addr == RT_INADDR_BROADCAST))
		return -RT_EINVAL;
	rt_route_del_connected(nif);
	rt_arp_flush(nif, 1);
	nif->addr = addr;
	nif->netmask = addr ? netmask : 0;
	nif->broadaddr = 0;
	nif->dstaddr = 0;
	if (!addr)
		return 0;
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
	return 0;
}

void rt_netif_up(struct rt_netif *nif)
{
	u16 nf = nif->flags | RT_IFF_UP | RT_IFF_RUNNING;

	if (nif->ioctl_flags && nf != nif->flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
	if (nif->type == RT_IFT_ETHER && nif->addr)
		rt_arp_announce(nif, nif->addr);
	rt_kick();
}

void rt_netif_down(struct rt_netif *nif)
{
	u16 nf = nif->flags & (u16)~(RT_IFF_UP | RT_IFF_RUNNING);

	if (nif->ioctl_flags && nf != nif->flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
	rt_arp_flush(nif, 1);
	/* Configured routes stay (unused while down, shown as linkdown);
	 * learned ones go, as their information may be stale when it is
	 * back up. */
	rt_route_flush_dynamic_if(nif);
}

void rt_netif_input(struct rt_netif *nif, struct rt_buf *b, u16 ethertype,
                    const u8 *srchw, u16 flags)
{
	nif->stats.ipackets++;
	nif->stats.ibytes += b->len;
	if (flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))
		nif->stats.imcasts++;
	if (!(nif->flags & RT_IFF_UP)) {
		nif->stats.iqdrops++;
		rt_buf_free(b);
		return;
	}
	b->netif = nif;
	b->flags = (u16)((b->flags & (RT_BUFF_LOOP | RT_BUFF_CSUM_OK)) |
	                 (flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)));
	b->l3_proto = ethertype;
	switch (ethertype) {
	case RT_ETH_P_IP:
		rt_ip_input(nif, b);
		return;
	case RT_ETH_P_ARP:
		if (nif->type == RT_IFT_ETHER) {
			rt_arp_input(nif, b);
			return;
		}
		break;
	}
	nif->stats.noproto++;
	rt_buf_free(b);
}

int rt_netif_output_ip(struct rt_netif *nif, struct rt_buf *b, u32 nexthop)
{
	u8 mc[6];
	int r;

	if (!(nif->flags & RT_IFF_UP)) {
		nif->stats.oerrors++;
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	switch (nif->type) {
	case RT_IFT_ETHER:
		if (rt_ip_is_broadcast(nexthop, nif)) {
			nif->stats.omcasts++;
			r = nif->link_output(nif, b, NULL, RT_ETH_P_IP);
		} else if (rt_ip_is_multicast(nexthop)) {
			/* RFC 1112 6.4: 01:00:5e + low 23 bits of the group */
			u32 h = rt_ntohl(nexthop);
			mc[0] = 0x01; mc[1] = 0x00; mc[2] = 0x5e;
			mc[3] = (u8)((h >> 16) & 0x7f); mc[4] = (u8)(h >> 8); mc[5] = (u8)h;
			nif->stats.omcasts++;
			r = nif->link_output(nif, b, mc, RT_ETH_P_IP);
		} else {
			return rt_arp_resolve_output(nif, b, nexthop);
		}
		break;
	case RT_IFT_PTP:
	case RT_IFT_LOOP:
	default:
		/* loopback's link_output is the deferred queue of loopback.c */
		r = nif->link_output(nif, b, NULL, RT_ETH_P_IP);
		break;
	}
	if (r < 0)
		nif->stats.oerrors++;
	return r;
}
