/*
 * Riptide - IEEE 802.1Q VLAN interfaces.
 *
 *   interface eth0.10 vlan=10 parent=eth0 [pcp=N|priority] [mtu=N]
 *             [dhcp | address=A.B.C.D/BITS | up] ...
 *
 * A VLAN interface is an Ethernet interface of its own (addresses, DHCP,
 * IPv6, ARP, multicast, routes) on the wire of its parent. Transmit pushes
 * the tag control field and the inner ethertype in front of the packet and
 * hands it to the parent as ethertype 0x8100, so the parent's driver builds
 * a tagged frame with whatever framing it uses: SANA-II cooked (the driver
 * writes type 0x8100, the payload starts with the tag), SANA-II raw, MNI and
 * the host's TAP and wire alike. Receive: the parent gets ethertype 0x8100
 * (SANA-II needs reads for that type: nif->vlan_rx), rt_vlan_input() strips
 * the tag and passes the frame to the VLAN interface with that id.
 * Priority-tagged frames (id 0) stay on the parent; frames for an id with
 * no interface are dropped (counted as noproto on the parent).
 *
 * The VLAN interface uses the parent's MAC address; link multicast joins
 * go to the parent's filter (drivers count references: rt_mc_ref). Its MTU
 * defaults to the parent's less 4, so a tagged frame is no longer than an
 * untagged full-size one (many SANA-II drivers refuse frames over 1514
 * bytes); mtu= may raise it up to the parent's MTU where the driver and
 * the network take 1518 byte frames. Not supported: stacked tags (QinQ),
 * VLANs on point-to-point interfaces.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"

#define VLAN_HLEN 4
/* room a driver may need in front of the tag (raw Ethernet header) */
#define VLAN_TX_HEADROOM (VLAN_HLEN + 32)

struct rt_netif *rt_vlan_find(const struct rt_netif *parent, u16 vid)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next)
		if (n->vlan_parent == parent && n->vlan_id == vid)
			return n;
	return NULL;
}

static int vlan_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct rt_netif *parent = nif->vlan_parent;
	u16 tci;
	u8 *p;

	if (!parent || !(parent->flags & RT_IFF_UP)) {
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	if (rt_buf_headroom(b) < VLAN_TX_HEADROOM) {
		struct rt_buf *c = rt_buf_copy_expand(b, RT_BUF_HEADROOM);
		rt_buf_free(b);
		if (!c)
			return -RT_ENOBUFS;
		b = c;
	}
	tci = (u16)(nif->vlan_id & 0x0fffu);
	if (nif->vlan_pcp_prio && b->prio)
		tci |= (u16)(((b->prio - 1u) & 7u) << 13);
	else
		tci |= (u16)((nif->vlan_pcp & 7u) << 13);
	/* the transport checksum (if the driver had computed one) covers
	 * only the payload; the tag is outside it */
	b->flags &= (u16)~RT_BUFF_CSUM_PART;
	p = rt_buf_push(b, VLAN_HLEN);
	rt_put_be16(p, tci);
	rt_put_be16(p + 2, ethertype);
	parent->stats.opackets++;
	parent->stats.obytes += b->len;
	if (!dsthw || (dsthw[0] & 1))
		parent->stats.omcasts++;
	return rt_netif_xmit(parent, b, dsthw, RT_ETH_P_8021Q);
}

static int vlan_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct rt_netif *parent = nif->vlan_parent;

	if (!parent || !parent->mcast)
		return 0;
	return parent->mcast(parent, hw, join);
}

void rt_vlan_input(struct rt_netif *parent, struct rt_buf *b, const u8 *srchw, u16 flags)
{
	struct rt_netif *child;
	u16 tci, inner, vid;

	if (b->len < VLAN_HLEN || parent->vlan_parent) {
		/* runt, or a stacked tag (QinQ): not supported */
		parent->stats.noproto++;
		rt_buf_free(b);
		return;
	}
	tci = rt_get_be16(b->data);
	inner = rt_get_be16(b->data + 2);
	vid = (u16)(tci & 0x0fffu);
	/* a driver's sum over the frame covered the tag: take it out */
	if (b->flags & RT_BUFF_CSUM_PART)
		b->csum = rt_csum_add(b->csum, rt_csum_fold(rt_csum_partial(b->data, VLAN_HLEN, 0)));
	rt_buf_pull(b, VLAN_HLEN);
	if (vid == 0) {
		/* priority tagged (802.1p only): belongs to the parent */
		rt_netif_deliver(parent, b, inner, srchw, flags);
		return;
	}
	child = rt_vlan_find(parent, vid);
	if (!child || inner == RT_ETH_P_8021Q || inner == 0x88a8) {
		parent->stats.noproto++;
		rt_buf_free(b);
		return;
	}
	rt_netif_input(child, b, inner, srchw, flags);
}

/* RUNNING follows the parent's; the MTU stays within the parent's */
void rt_vlan_parent_changed(struct rt_netif *parent)
{
	struct rt_netif *n;

	if (!parent->nvlan)
		return;
	for (n = rt_netif_list(); n; n = n->next) {
		if (n->vlan_parent != parent)
			continue;
		if (n->mtu > parent->mtu)
			n->mtu = parent->mtu;
		if ((parent->flags & RT_IFF_RUNNING) && (n->flags & RT_IFF_UP))
			n->flags |= RT_IFF_RUNNING;
		else
			n->flags &= (u16)~RT_IFF_RUNNING;
	}
}

int rt_vlan_add(struct rt_netif *parent, const char *name, u16 vid, int pcp, u16 mtu,
                struct rt_netif **out)
{
	struct rt_netif *nif;
	int r;

	if (!parent || parent->type != RT_IFT_ETHER || parent->hwlen != 6 || parent->vlan_parent)
		return -RT_EOPNOTSUPP;
	if (vid < 1 || vid > 4094 || pcp < -1 || pcp > 7)
		return -RT_EINVAL;
	if (!name || !name[0] || strlen(name) >= RT_IFNAMSIZ)
		return -RT_EINVAL;
	if (rt_netif_find(name) || rt_vlan_find(parent, vid))
		return -RT_EEXIST;
	if (!mtu)
		mtu = (u16)(parent->mtu > 68 + VLAN_HLEN ? parent->mtu - VLAN_HLEN : 68);
	if (mtu < 68 || mtu > parent->mtu)
		return -RT_EINVAL;
	nif = rt_zalloc(sizeof(*nif));
	if (!nif)
		return -RT_ENOMEM;
	rt_strlcpy(nif->name, name, sizeof(nif->name));
	nif->type = RT_IFT_ETHER;
	nif->hwlen = 6;
	memcpy(nif->hwaddr, parent->hwaddr, 6);
	nif->flags = (u16)(parent->flags & (RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX));
	nif->mtu = mtu;
	nif->metric = parent->metric;
	nif->link_output = vlan_output;
	nif->mcast = vlan_mcast;
	nif->vlan_parent = parent;
	nif->vlan_id = vid;
	nif->vlan_pcp = (u8)(pcp < 0 ? 0 : pcp);
	nif->vlan_pcp_prio = pcp < 0;
	rt_snprintf(nif->descr, sizeof(nif->descr), "VLAN %u on %s", (unsigned)vid, parent->name);
	r = rt_netif_add(nif);
	if (r < 0) {
		rt_free(nif);
		return r;
	}
	if (parent->nvlan++ == 0 && parent->vlan_rx && (r = parent->vlan_rx(parent, 1)) < 0)
		rt_log(RT_LOG_WARN, "%s: cannot receive VLAN frames (%s)", parent->name, rt_strerror(-r));
	if (out)
		*out = nif;
	return 0;
}

int rt_vlan_del(struct rt_netif *nif)
{
	struct rt_netif *parent;

	if (!nif || !nif->vlan_parent)
		return -RT_EINVAL;
	parent = nif->vlan_parent;
	rt_netif_remove(nif);
	nif->vlan_parent = NULL;
	if (parent->nvlan && --parent->nvlan == 0 && parent->vlan_rx)
		parent->vlan_rx(parent, 0);
	rt_free(nif);
	return 0;
}

void rt_vlan_parent_removed(struct rt_netif *parent)
{
	struct rt_netif *n, *next;

	for (n = rt_netif_list(); n && parent->nvlan; n = next) {
		next = n->next;
		if (n->vlan_parent == parent) {
			rt_vlan_del(n);
			next = rt_netif_list();      /* the list changed */
		}
	}
}

int rt_mc_ref(struct rt_mc_ref *tab, int max, int *n, const u8 *hw, int join)
{
	int i;

	for (i = 0; i < *n; i++)
		if (!memcmp(tab[i].hw, hw, 6))
			break;
	if (join) {
		if (i < *n) {
			if (tab[i].refs < 0xffffu)
				tab[i].refs++;
			return 0;
		}
		if (*n >= max)
			return 1;           /* untracked: the driver joins */
		memcpy(tab[*n].hw, hw, 6);
		tab[*n].refs = 1;
		(*n)++;
		return 1;
	}
	if (i >= *n)
		return 0;
	if (--tab[i].refs)
		return 0;
	(*n)--;
	memmove(&tab[i], &tab[i + 1], (u32)(*n - i) * sizeof(tab[0]));
	return 1;
}

/* ---- configuration ---- */

static const char *kv(const char *arg, const char *key)
{
	u32 l = (u32)strlen(key);

	if (!rt_strncasecmp(arg, key, l) && arg[l] == '=')
		return arg + l + 1;
	return NULL;
}

static int num(const char *v, u32 lo, u32 hi, u32 *out)
{
	const char *e;
	u32 x;

	if (!v || !*v)
		return -1;
	x = (u32)rt_strtoul(v, &e, 0);
	if (*e || x < lo || x > hi)
		return -1;
	*out = x;
	return 0;
}

#define VLAN_USAGE "usage: interface NAME vlan=ID parent=IF [pcp=0-7|priority] [mtu=N] " \
                   "[dhcp|address=A.B.C.D/BITS|up] [tcpwindow=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off]"

int rt_vlan_config(int argc, char **argv, char *err, u32 errlen)
{
	struct rt_netif *parent = NULL, *nif;
	const char *v, *address = NULL;
	int dhcp = 0, up = 0;
	u32 vid = 0, mtu = 0, x;
	int i, r, pcp = 0, ip4 = -1, ip6 = -1, qos = -1, have_vid = 0;
	long tcpwin = -1;

	if (argc < 2 || rt_strcasecmp(argv[0], "interface"))
		return 1;
	if (argc == 3 && (!rt_strcasecmp(argv[2], "delete") || !rt_strcasecmp(argv[2], "del") ||
	                  !rt_strcasecmp(argv[2], "remove"))) {
		nif = rt_netif_find(argv[1]);
		if (!nif || !nif->vlan_parent)
			return 1;
		return rt_vlan_del(nif);
	}
	for (i = 2; i < argc; i++)
		if (kv(argv[i], "vlan"))
			have_vid = 1;
	if (!have_vid)
		return 1;
	for (i = 2; i < argc; i++) {
		if ((v = kv(argv[i], "vlan")) != NULL) {
			if (num(v, 1, 4094, &vid) < 0) {
				rt_snprintf(err, errlen, "interface: vlan= is 1..4094");
				return -RT_EINVAL;
			}
		} else if ((v = kv(argv[i], "parent")) != NULL || (v = kv(argv[i], "link")) != NULL) {
			parent = rt_netif_find(v);
			if (!parent) {
				rt_snprintf(err, errlen, "interface: no interface '%s'", v);
				return -RT_ENXIO;
			}
		} else if ((v = kv(argv[i], "pcp")) != NULL) {
			if (!rt_strcasecmp(v, "priority") || !rt_strcasecmp(v, "prio"))
				pcp = -1;
			else if (num(v, 0, 7, &x) < 0) {
				rt_snprintf(err, errlen, "interface: pcp= is 0..7 or priority");
				return -RT_EINVAL;
			} else
				pcp = (int)x;
		} else if ((v = kv(argv[i], "mtu")) != NULL) {
			if (num(v, 68, 65535, &mtu) < 0) {
				rt_snprintf(err, errlen, "interface: mtu= is 68 up to the parent's MTU");
				return -RT_EINVAL;
			}
		} else if ((v = kv(argv[i], "tcpwindow")) != NULL) {
			if (num(v, 0, 16u * 1024 * 1024, &x) < 0 || (x && x < 536)) {
				rt_snprintf(err, errlen, "interface: tcpwindow is 0 or 536..16777216 bytes");
				return -RT_EINVAL;
			}
			tcpwin = (long)x;
		} else if (!rt_strcasecmp(argv[i], "dhcp")) {
			dhcp = 1;
		} else if (!rt_strcasecmp(argv[i], "up")) {
			up = 1;
		} else if ((v = kv(argv[i], "address")) != NULL || (v = kv(argv[i], "addr")) != NULL) {
			address = v;
		} else if ((v = kv(argv[i], "ipv4")) != NULL || (v = kv(argv[i], "ipv6")) != NULL ||
		           (v = kv(argv[i], "qos")) != NULL) {
			int on = !rt_strcasecmp(v, "on") || !strcmp(v, "1") ? 1 :
			         !rt_strcasecmp(v, "off") || !strcmp(v, "0") ? 0 : -1;
			if (on < 0) {
				rt_snprintf(err, errlen, "interface: %.4s= is on or off", argv[i]);
				return -RT_EINVAL;
			}
			if (!rt_strncasecmp(argv[i], "ipv4", 4))
				ip4 = on;
			else if (!rt_strncasecmp(argv[i], "ipv6", 4))
				ip6 = on;
			else
				qos = on;
		} else {
			rt_snprintf(err, errlen, "interface: unknown option '%s' for a VLAN (%s)", argv[i],
			            VLAN_USAGE);
			return -RT_EINVAL;
		}
	}
	if (!parent) {
		rt_snprintf(err, errlen, "%s", VLAN_USAGE);
		return -RT_EINVAL;
	}
	if (mtu > parent->mtu) {
		rt_snprintf(err, errlen, "interface: mtu= is at most %u, the MTU of %s", (unsigned)parent->mtu,
		            parent->name);
		return -RT_EINVAL;
	}
	r = rt_vlan_add(parent, argv[1], (u16)vid, pcp, (u16)mtu, &nif);
	if (r == -RT_EEXIST) {
		rt_snprintf(err, errlen, rt_netif_find(argv[1]) ? "interface %s exists already" :
		            "%s has a VLAN %lu already", rt_netif_find(argv[1]) ? argv[1] : parent->name,
		            (unsigned long)vid);
		return r;
	}
	if (r == -RT_EOPNOTSUPP) {
		rt_snprintf(err, errlen, "interface: VLANs need an Ethernet parent (%s is not one)", parent->name);
		return r;
	}
	if (r < 0) {
		rt_snprintf(err, errlen, "interface: %s", rt_strerror(-r));
		return r;
	}
	if (tcpwin >= 0)
		nif->tcp_window = (u32)tcpwin;
	if (ip4 >= 0)
		rt_netif_ip4_enable(nif, ip4);
	if (ip6 == 0)
		rt_ip6_ifenable(nif, 0);
	else if (ip6 == 1)
		nif->ip6_disabled = 2;
	if (qos >= 0)
		rt_qos_enable(nif, qos);
	/* the address on the same line (one line per VLAN, as RiptidePrefs
	 * writes them): as "ifconfig NAME ADDR up" / "dhcp NAME" would */
	if (address) {
		char l[80];
		rt_snprintf(l, sizeof(l), "ifconfig %s %s up", nif->name, address);
		r = rt_config_line(l, err, errlen, NULL);
		if (r < 0) {
			rt_vlan_del(nif);         /* all or nothing */
			return r;
		}
	} else if (up || dhcp) {
		rt_netif_up(nif);
	}
	if (dhcp) {
		r = rt_dhcp_start(nif, NULL);
		if (r < 0) {
			rt_snprintf(err, errlen, "dhcp %s: %s", nif->name, rt_strerror(-r));
			rt_vlan_del(nif);
			return r;
		}
	}
	return 0;
}
