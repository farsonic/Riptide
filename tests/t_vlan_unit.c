/*
 * Riptide - unit tests of 802.1Q VLAN interfaces (src/core/vlan.c): the
 * "interface NAME vlan=ID parent=IF" line and its errors, the tag on
 * transmit (VID, PCP, PCP from the socket priority, inner ethertype), the
 * dispatch on receive (by VID, priority-tagged frames to the parent,
 * unknown VIDs and stacked tags dropped), ARP answered through a VLAN,
 * MTUs, the parent's vlan_rx hook, the reference counted multicast
 * filter, net.riptide.ifaces, and removal with the parent.
 * Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "rt/rt_ctl.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the fake Ethernet driver: keeps the last frames it was given */
#define NREC 16
static struct { u16 et; u32 len; u8 dst[6]; int bcast; u8 data[1600]; } rec[NREC];
static int nrec;
static int vlan_rx_on, vlan_rx_calls;
static struct rt_mc_ref drv_mc[8];
static int drv_nmc, drv_joins, drv_leaves;

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	(void)nif;
	if (nrec < NREC) {
		rec[nrec].et = et;
		rec[nrec].len = b->len;
		rec[nrec].bcast = !dsthw;
		if (dsthw)
			memcpy(rec[nrec].dst, dsthw, 6);
		memcpy(rec[nrec].data, b->data, b->len < 1600 ? b->len : 1600);
		nrec++;
	}
	rt_buf_free(b);
	return 0;
}

static int fake_vlan_rx(struct rt_netif *nif, int on)
{
	(void)nif;
	vlan_rx_on = on;
	vlan_rx_calls++;
	return 0;
}

static int fake_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	(void)nif;
	if (rt_mc_ref(drv_mc, 8, &drv_nmc, hw, join)) {
		if (join)
			drv_joins++;
		else
			drv_leaves++;
	}
	return 0;
}

static struct rt_netif eth0;
static const u8 mac0[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 peer[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };

static u32 A(int a, int b, int c, int d)
{
	return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d);
}

static int cfg(const char *line)
{
	char err[200];
	int r = rt_config_line(line, err, sizeof(err), NULL);
	if (r < 0 && getenv("T_VERBOSE"))
		fprintf(stderr, "%s -> %d %s\n", line, r, err);
	return r;
}

/* an ARP request for 'tip' from 'sip', optionally tagged with tci */
static struct rt_buf *arp_req(u32 sip, u32 tip, int tagged, u16 tci)
{
	struct rt_buf *b = rt_buf_alloc(64, 4 + 28);
	u8 *p;

	if (tagged) {
		p = rt_buf_put(b, 4);
		rt_put_be16(p, tci);
		rt_put_be16(p + 2, RT_ETH_P_ARP);
	}
	p = rt_buf_put(b, 28);
	rt_put_be16(p, 1);               /* Ethernet */
	rt_put_be16(p + 2, RT_ETH_P_IP);
	p[4] = 6;
	p[5] = 4;
	rt_put_be16(p + 6, 1);           /* request */
	memcpy(p + 8, peer, 6);
	memcpy(p + 14, &sip, 4);
	memset(p + 18, 0, 6);
	memcpy(p + 24, &tip, 4);
	return b;
}

static int find_rec(u16 et, u16 tci_or_minus1, u16 inner)
{
	int i;

	for (i = 0; i < nrec; i++) {
		if (rec[i].et != et)
			continue;
		if (et != RT_ETH_P_8021Q)
			return i;
		if (rt_get_be16(rec[i].data) == tci_or_minus1 && rt_get_be16(rec[i].data + 2) == inner)
			return i;
	}
	return -1;
}

static void test_config(void)
{
	struct rt_netif *v10, *v20;

	CHECK(cfg("interface eth0.10 vlan=10 parent=eth0") == 0);
	v10 = rt_netif_find("eth0.10");
	CHECK(v10 && v10->vlan_parent == &eth0 && v10->vlan_id == 10);
	CHECK(v10 && v10->mtu == 1496);             /* parent's less the tag */
	CHECK(v10 && v10->type == RT_IFT_ETHER && !memcmp(v10->hwaddr, mac0, 6));
	CHECK(v10 && !(v10->flags & RT_IFF_UP));    /* comes up with ifconfig/dhcp, as other interfaces */
	CHECK(eth0.nvlan == 1 && vlan_rx_on == 1 && vlan_rx_calls == 1);
	CHECK(cfg("interface eth0.20 vlan=20 parent=eth0 pcp=5 mtu=1500") == 0);
	v20 = rt_netif_find("eth0.20");
	CHECK(v20 && v20->mtu == 1500 && v20->vlan_pcp == 5 && !v20->vlan_pcp_prio);
	CHECK(eth0.nvlan == 2 && vlan_rx_calls == 1);
	CHECK(cfg("interface eth0.30 vlan=30 link=eth0 pcp=priority ipv6=off") == 0);
	CHECK(rt_netif_find("eth0.30") && rt_netif_find("eth0.30")->vlan_pcp_prio);
	/* errors */
	CHECK(cfg("interface x vlan=0 parent=eth0") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=4095 parent=eth0") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=10 parent=eth0") == -RT_EEXIST);      /* id in use */
	CHECK(cfg("interface eth0.20 vlan=21 parent=eth0") == -RT_EEXIST); /* name in use */
	CHECK(cfg("interface x vlan=11 parent=nope") == -RT_ENXIO);
	CHECK(cfg("interface x vlan=11") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=11 parent=eth0 mtu=1501") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=11 parent=eth0 pcp=8") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=11 parent=eth0 bogus=1") == -RT_EINVAL);
	CHECK(cfg("interface x vlan=11 parent=eth0.10") == -RT_EOPNOTSUPP); /* no stacked tags */
	CHECK(cfg("interface x vlan=11 parent=lo0") == -RT_EOPNOTSUPP);
	CHECK(!rt_netif_find("x"));
	/* not a VLAN line: left to the OS glue (no handler here) */
	CHECK(cfg("interface eth9 device=foo.device") == -RT_EINVAL);
}

static void test_transmit(void)
{
	struct rt_netif *v10 = rt_netif_find("eth0.10"), *v20 = rt_netif_find("eth0.20");
	struct rt_netif *v30 = rt_netif_find("eth0.30");
	struct rt_buf *b;
	u32 op;
	int i;

	CHECK(cfg("ifconfig eth0.10 10.10.0.1 netmask 255.255.255.0 up") == 0);
	CHECK(eth0.flags & RT_IFF_UP);              /* the parent came up with it */
	CHECK(v10->flags & RT_IFF_RUNNING);
	/* the gratuitous ARP of the new address went out tagged 10 */
	i = find_rec(RT_ETH_P_8021Q, 10, RT_ETH_P_ARP);
	CHECK(i >= 0 && rec[i].bcast && rec[i].len == 4 + 28);
	/* a broadcast IPv4 packet through VLAN 20 (PCP 5) */
	CHECK(cfg("ifconfig eth0.20 10.20.0.1 netmask 255.255.255.0 up") == 0);
	nrec = 0;
	op = eth0.stats.opackets;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 28);
	memset(rt_buf_put(b, 28), 0x45, 28);
	CHECK(rt_netif_output_ip(v20, b, A(10, 20, 0, 255)) == 0);
	i = find_rec(RT_ETH_P_8021Q, (u16)((5u << 13) | 20u), RT_ETH_P_IP);
	CHECK(i >= 0 && rec[i].len == 4 + 28 && rec[i].data[4] == 0x45);
	CHECK(eth0.stats.opackets == op + 1);
	/* PCP from the socket priority (b->prio = priority + 1) */
	CHECK(cfg("ifconfig eth0.30 10.30.0.1 netmask 255.255.255.0 up") == 0);
	nrec = 0;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 28);
	memset(rt_buf_put(b, 28), 0x45, 28);
	b->prio = 4;
	CHECK(rt_netif_output_ip(v30, b, A(10, 30, 0, 255)) == 0);
	CHECK(find_rec(RT_ETH_P_8021Q, (u16)((3u << 13) | 30u), RT_ETH_P_IP) >= 0);
	/* little headroom: copied, still tagged */
	nrec = 0;
	b = rt_buf_alloc(0, 28);
	memset(rt_buf_put(b, 28), 0x45, 28);
	CHECK(rt_netif_output_ip(v10, b, A(10, 10, 0, 255)) == 0);
	CHECK(find_rec(RT_ETH_P_8021Q, 10, RT_ETH_P_IP) >= 0);
	/* a parent that is down: nothing goes out */
	rt_netif_down(&eth0);
	CHECK(!(v10->flags & RT_IFF_RUNNING));
	nrec = 0;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 28);
	memset(rt_buf_put(b, 28), 0x45, 28);
	CHECK(rt_netif_output_ip(v10, b, A(10, 10, 0, 255)) == -RT_ENETDOWN);
	CHECK(nrec == 0);
	rt_netif_up(&eth0);
	CHECK(v10->flags & RT_IFF_RUNNING);
}

static void test_receive(void)
{
	struct rt_netif *v10 = rt_netif_find("eth0.10");
	u32 ip0, np0;
	int i;

	CHECK(cfg("ifconfig eth0 192.168.1.1 netmask 255.255.255.0 up") == 0);
	/* ARP for the VLAN 10 address, tagged 10: answered tagged 10 */
	nrec = 0;
	ip0 = v10->stats.ipackets;
	rt_netif_input(&eth0, arp_req(A(10, 10, 0, 2), A(10, 10, 0, 1), 1, 10), RT_ETH_P_8021Q, peer,
	               RT_BUFF_BCAST);
	CHECK(v10->stats.ipackets == ip0 + 1);
	i = find_rec(RT_ETH_P_8021Q, 10, RT_ETH_P_ARP);
	CHECK(i >= 0 && !rec[i].bcast && !memcmp(rec[i].dst, peer, 6));
	CHECK(i >= 0 && rt_get_be16(rec[i].data + 4 + 6) == 2);    /* a reply */
	/* the same request on VLAN 20 is not ours there: no answer */
	nrec = 0;
	rt_netif_input(&eth0, arp_req(A(10, 10, 0, 2), A(10, 10, 0, 1), 1, 20), RT_ETH_P_8021Q, peer,
	               RT_BUFF_BCAST);
	CHECK(nrec == 0);
	/* priority tagged (VID 0, PCP 6): the parent's */
	nrec = 0;
	rt_netif_input(&eth0, arp_req(A(192, 168, 1, 2), A(192, 168, 1, 1), 1, 6u << 13), RT_ETH_P_8021Q,
	               peer, RT_BUFF_BCAST);
	i = find_rec(RT_ETH_P_ARP, 0, 0);
	CHECK(i >= 0 && rt_get_be16(rec[i].data + 6) == 2);
	/* an id without an interface, a stacked tag, a runt: dropped */
	np0 = eth0.stats.noproto;
	nrec = 0;
	rt_netif_input(&eth0, arp_req(A(10, 10, 0, 2), A(10, 10, 0, 1), 1, 99), RT_ETH_P_8021Q, peer, 0);
	{
		struct rt_buf *b = arp_req(A(10, 10, 0, 2), A(10, 10, 0, 1), 1, 10);
		rt_put_be16(b->data + 2, RT_ETH_P_8021Q);
		rt_netif_input(&eth0, b, RT_ETH_P_8021Q, peer, 0);
	}
	{
		struct rt_buf *b = rt_buf_alloc(64, 2);
		rt_buf_put(b, 2);
		rt_netif_input(&eth0, b, RT_ETH_P_8021Q, peer, 0);
	}
	CHECK(eth0.stats.noproto == np0 + 3);
	CHECK(nrec == 0);
}

static void test_mtu_mcast_ctl(void)
{
	struct rt_netif *v20 = rt_netif_find("eth0.20"), *v10 = rt_netif_find("eth0.10");
	static u8 buf[8192];
	u32 len = sizeof(buf), k;
	const struct rt_ctl_hdr *h = (const struct rt_ctl_hdr *)buf;
	int found = 0;
	u8 g[6] = { 0x33, 0x33, 0, 0, 0, 0x42 };
	struct rt_mc_ref t[2];
	int n = 0;

	/* a smaller parent MTU clamps the VLANs */
	eth0.mtu = 1400;
	rt_vlan_parent_changed(&eth0);
	CHECK(v20->mtu == 1400 && v10->mtu == 1400);
	eth0.mtu = 1500;
	/* the reference counts */
	CHECK(rt_mc_ref(t, 2, &n, g, 1) == 1 && rt_mc_ref(t, 2, &n, g, 1) == 0 && n == 1);
	CHECK(rt_mc_ref(t, 2, &n, g, 0) == 0 && rt_mc_ref(t, 2, &n, g, 0) == 1 && n == 0);
	CHECK(rt_mc_ref(t, 2, &n, g, 0) == 0);              /* unknown leave */
	/* parent and VLAN join the same group: the driver sees it once */
	drv_joins = drv_leaves = 0;
	CHECK(eth0.mcast(&eth0, g, 1) == 0 && v10->mcast(v10, g, 1) == 0);
	CHECK(drv_joins == 1);
	CHECK(v10->mcast(v10, g, 0) == 0 && drv_leaves == 0);
	CHECK(eth0.mcast(&eth0, g, 0) == 0 && drv_leaves == 1);
	/* net.riptide.ifaces */
	CHECK(rt_sysctl_byname("net.riptide.ifaces", buf, &len, NULL, 0) == 0);
	CHECK(h->recsize >= sizeof(struct rt_ctl_iface));
	for (k = 0; k < h->count; k++) {
		const struct rt_ctl_iface *f = (const void *)(buf + sizeof(*h) + k * h->recsize);
		if (!strcmp(f->name, "eth0.20")) {
			CHECK(f->vlan_parent == eth0.index && f->vlan_id == 20 && f->vlan_pcp == 5);
			found++;
		} else if (!strcmp(f->name, "eth0.30")) {
			CHECK(f->vlan_pcp == RT_CTL_VLAN_PCP_PRIO);
			found++;
		} else if (!strcmp(f->name, "eth0")) {
			CHECK(f->vlan_parent == 0 && f->nvlan == 3);
			found++;
		}
	}
	CHECK(found == 3);
	/* the transmit priority bands look past the tag: ARP is interactive */
	{
		struct rt_buf *b = arp_req(1, 2, 1, 10);
		CHECK(rt_qos_band(b, RT_ETH_P_8021Q) == RT_QOS_BAND_INTERACTIVE);
		rt_buf_free(b);
	}
}

static void test_delete(void)
{
	CHECK(cfg("interface eth0.10 delete") == 0);
	CHECK(!rt_netif_find("eth0.10") && eth0.nvlan == 2 && vlan_rx_on == 1);
	CHECK(cfg("interface eth0.20 delete") == 0);
	CHECK(eth0.nvlan == 1 && vlan_rx_on == 1);
	/* the address on the VLAN line itself (as RiptidePrefs writes it) */
	CHECK(cfg("interface eth0.50 vlan=50 parent=eth0 address=10.50.0.1/24") == 0);
	CHECK(rt_netif_find("eth0.50") && rt_netif_find("eth0.50")->addr == A(10, 50, 0, 1) &&
	      rt_netif_find("eth0.50")->netmask == A(255, 255, 255, 0) && (rt_netif_find("eth0.50")->flags & RT_IFF_UP));
	CHECK(cfg("interface eth0.60 vlan=60 parent=eth0 up") == 0);
	CHECK(rt_netif_find("eth0.60") && (rt_netif_find("eth0.60")->flags & RT_IFF_UP) && !rt_netif_find("eth0.60")->addr);
	CHECK(cfg("interface eth0.70 vlan=70 parent=eth0 address=300.1.1.1/24") < 0);
	CHECK(!rt_netif_find("eth0.70"));          /* all or nothing */
	/* the parent goes: its last VLAN goes first, and tagged reads stop */
	CHECK(cfg("interface eth0.40 vlan=40 parent=eth0") == 0);
	rt_netif_remove(&eth0);
	CHECK(!rt_netif_find("eth0.30") && !rt_netif_find("eth0.40") && !rt_netif_find("eth0.50"));
	CHECK(eth0.nvlan == 0 && vlan_rx_on == 0);
}

int main(void)
{
	CHECK(rt_init() == 0);
	rt_log_level = 0;
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, mac0, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	eth0.vlan_rx = fake_vlan_rx;
	eth0.mcast = fake_mcast;
	CHECK(rt_netif_add(&eth0) == 0);
	test_config();
	test_transmit();
	test_receive();
	test_mtu_mcast_ctl();
	test_delete();
	printf("t_vlan_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
