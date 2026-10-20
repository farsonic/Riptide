/*
 * Riptide - BSD routing, ARP and interface alias ioctls (SIOCADDRT,
 * SIOCDELRT, SIOCSARP, SIOCGARP, SIOCDARP, SIOCAIFADDR), so AmiTCP-era
 * route/arp/ifconfig programs work. Called by rt_api_ioctl() with the
 * stack lock held. No privilege checks (single user AmigaOS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "sock_internal.h"
#include "infra.h"

static u32 sa_addr(const struct rt_sockaddr *sa)
{
	struct rt_sockaddr_in sin;

	memcpy(&sin, sa, sizeof(sin));
	return sin.sin_addr;
}

static int sa_family_ok(const struct rt_sockaddr *sa)
{
	return sa->sa_family == RT_AF_INET || sa->sa_family == RT_AF_UNSPEC;
}

/* Classful mask of a network number, widened until it covers every set
 * bit (as route(8)'s inet_makenetandmask() does). */
static u32 net_mask_for(u32 dst)
{
	u32 h = rt_ntohl(dst), m;

	if (h == 0)
		return 0;
	if (!(h & 0x80000000u))
		m = 0xff000000u;
	else if ((h & 0xc0000000u) == 0x80000000u)
		m = 0xffff0000u;
	else
		m = 0xffffff00u;
	while (h & ~m)
		m = (m >> 1) | 0x80000000u;
	return rt_htonl(m);
}

static int route_ioctl(u32 req, struct rt_ortentry *rt)
{
	struct rt_netif *nif = NULL;
	u32 dst, mask, gw = 0, flags;
	u16 f;

	if (!sa_family_ok(&rt->rt_dst))
		return -RT_EAFNOSUPPORT;
	f = (u16)rt->rt_flags;
	dst = sa_addr(&rt->rt_dst);
	if (f & RT_RTF_HOST)
		mask = 0xffffffffu;
	else if (f & RT_RTF_MASK)
		mask = rt->rt_hash;
	else
		mask = net_mask_for(dst);
	if (rt->rt_ifp) {
		if (rt->rt_ifp > 255 || !(nif = rt_netif_by_index(rt->rt_ifp)))
			return -RT_ENXIO;
	}
	if (f & RT_RTF_GATEWAY) {
		gw = sa_addr(&rt->rt_gateway);
		if (!gw)
			return -RT_EINVAL;
	} else if (!nif && !(f & RT_RTF_REJECT)) {
		/* BSD: the "gateway" of an interface route is the interface's
		 * own address (deleting needs no interface) */
		nif = rt_netif_by_addr(sa_addr(&rt->rt_gateway));
		if (!nif && req == RT_SIOCADDRT)
			return -RT_ENETUNREACH;
	}
	if (req == RT_SIOCDELRT) {
		int r = rt_route_delete_match(dst, mask, gw, gw ? NULL : nif, 0);
		return r < 0 ? r : 0;
	}
	flags = RT_RTF_UP | RT_RTF_STATIC | (f & RT_RTF_REJECT);
	return rt_route_add(dst, mask, gw, nif, flags, 0);
}

/* AmiTCP (38 bytes) or 4.4BSD (36 bytes) struct arpreq */
static int arp_ioctl(u32 req, void *arg)
{
	struct rt_arpreq *ar = arg;
	int bsd = req == RT_SIOCSARP_BSD || req == RT_SIOCGARP_BSD || req == RT_SIOCDARP_BSD;
	u32 flags_off = bsd ? 32 : 34;
	struct rt_arp_entry_info ai;
	struct rt_nexthop nh;
	u32 ip;
	s32 fl;
	int r;

	if (!sa_family_ok(&ar->arp_pa))
		return -RT_EAFNOSUPPORT;
	ip = sa_addr(&ar->arp_pa);
	memcpy(&fl, (u8 *)arg + flags_off, sizeof(fl));
	switch (req) {
	case RT_SIOCSARP:
	case RT_SIOCSARP_BSD:
		if (fl & RT_ATF_PUBL)
			return -RT_EOPNOTSUPP;      /* no proxy ARP */
		/* like Linux arp -s: the address must be on a connected
		 * Ethernet network */
		r = rt_ip_route(ip, &nh);
		if (r < 0)
			return r;
		if ((nh.flags & RT_RTF_GATEWAY) || nh.netif->type != RT_IFT_ETHER)
			return -RT_ENETUNREACH;
		return rt_arp_add_static(nh.netif, ip, ar->arp_ha.sa_data);
	case RT_SIOCGARP:
	case RT_SIOCGARP_BSD:
		if (rt_arp_get(NULL, ip, &ai) < 0)
			return -RT_ENXIO;
		memset(&ar->arp_ha, 0, bsd ? 16 : sizeof(ar->arp_ha));
		ar->arp_ha.sa_len = bsd ? 16 : 18;
		ar->arp_ha.sa_family = RT_AF_UNSPEC;
		memcpy(ar->arp_ha.sa_data, ai.hw, 6);
		fl = RT_ATF_INUSE;
		if (ai.state != 0)
			fl |= RT_ATF_COM;
		if (ai.state == 3)
			fl |= RT_ATF_PERM;
		memcpy((u8 *)arg + flags_off, &fl, sizeof(fl));
		return 0;
	default:
		return rt_arp_delete(ip) < 0 ? -RT_ENXIO : 0;
	}
}

static int aifaddr(struct rt_ifaliasreq *ifra)
{
	char name[RT_IFNAMSIZ + 1];
	struct rt_netif *nif;
	u32 a, m, b;
	int r;

	memcpy(name, ifra->ifra_name, RT_IFNAMSIZ);
	name[RT_IFNAMSIZ] = 0;
	nif = rt_netif_find(name);
	if (!nif)
		return -RT_ENXIO;
	if (!sa_family_ok(&ifra->ifra_addr))
		return -RT_EAFNOSUPPORT;
	a = sa_addr(&ifra->ifra_addr);
	m = ifra->ifra_mask.sa_len ? sa_addr(&ifra->ifra_mask) :
	    nif->netmask ? nif->netmask : net_mask_for(a);
	b = ifra->ifra_broadaddr.sa_len ? sa_addr(&ifra->ifra_broadaddr) : 0;
	r = rt_netif_set_addr(nif, a, m, b);
	if (r == 0 && a && !(nif->flags & RT_IFF_UP))
		rt_netif_up(nif);
	return r;
}

/* Returns 1 if req is not one of ours. */
int rt_ioctl_net(u32 req, void *arg)
{
	switch (req) {
	case RT_SIOCADDRT:
	case RT_SIOCDELRT:
		return route_ioctl(req, arg);
	case RT_SIOCSARP: case RT_SIOCGARP: case RT_SIOCDARP:
	case RT_SIOCSARP_BSD: case RT_SIOCGARP_BSD: case RT_SIOCDARP_BSD:
		return arp_ioctl(req, arg);
	case RT_SIOCAIFADDR:
		return aifaddr(arg);
	}
	return 1;
}
