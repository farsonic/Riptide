/*
 * Riptide - unit tests of the tool interfaces: the net.riptide.* table
 * dumps (by name and MIB, size query, short buffers), the BSD routing/ARP/
 * alias ioctls, rt_ctl_command() (sysctl queries and the extended route,
 * ifconfig, arp, nameserver and dhcp verbs) and route bookkeeping
 * (static beats DHCP, replace, linkdown, cleanup on interface removal).
 * Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 ip4(int a, int b, int c, int d) { return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d); }

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	rt_buf_free(b);
	return 0;
}

static struct rt_netif eth0, eth1;
static u8 dumpbuf[16384];

/* fetch a dump by name; returns record count or -1 */
static int fetch(const char *name, u16 recsize)
{
	struct rt_ctl_hdr h;
	u32 len = 0;
	int r;

	rt_lock();
	r = rt_sysctl_byname(name, NULL, &len, NULL, 0);
	if (r == 0) {
		CHECK(len >= sizeof(h) && len <= sizeof(dumpbuf));
		len = sizeof(dumpbuf);
		r = rt_sysctl_byname(name, dumpbuf, &len, NULL, 0);
	}
	rt_unlock();
	if (r < 0)
		return -1;
	memcpy(&h, dumpbuf, sizeof(h));
	CHECK(h.version == RT_CTL_VERSION && h.recsize == recsize);
	CHECK(len == sizeof(h) + h.count * h.recsize);
	return (int)h.count;
}

static void *rec(int i, u16 recsize)
{
	return dumpbuf + sizeof(struct rt_ctl_hdr) + (u32)i * recsize;
}

static const struct rt_ctl_route *find_route(int n, u32 dst, u32 mask, u32 gw)
{
	int i;

	for (i = 0; i < n; i++) {
		const struct rt_ctl_route *r = rec(i, sizeof(*r));
		if (r->dst == dst && r->mask == mask && (!gw || r->gateway == gw) &&
		    !(r->flags & RT_RTF_PMTU))
			return r;
	}
	return NULL;
}

static int count_routes(u32 dst, u32 mask)
{
	struct rt_route *r;
	int n = 0;

	rt_lock();
	for (r = rt_route_list(); r; r = r->next)
		if (r->dst == dst && r->mask == mask && !(r->flags & RT_RTF_PMTU))
			n++;
	rt_unlock();
	return n;
}

static int cmd(const char *line, char *out, u32 len)
{
	int r;

	rt_lock();
	r = rt_ctl_command(line, out, len, NULL);
	rt_unlock();
	return r;
}

static void sin_set(struct rt_sockaddr *sa, u32 addr)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = addr;
	memcpy(sa, &sin, sizeof(sin));
}

static void setup(void)
{
	static const u8 mac0[6] = { 0x02, 0, 0, 0, 0, 0x10 }, mac1[6] = { 0x02, 0, 0, 0, 0, 0x11 };

	memset(&eth0, 0, sizeof(eth0));
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	rt_strlcpy(eth0.descr, "fake.device unit 0", sizeof(eth0.descr));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, mac0, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	eth1 = eth0;
	rt_strlcpy(eth1.name, "eth1", sizeof(eth1.name));
	memcpy(eth1.hwaddr, mac1, 6);
	rt_lock();
	CHECK(rt_netif_add(&eth0) == 0);
	CHECK(rt_netif_add(&eth1) == 0);
	CHECK(rt_netif_set_addr(&eth0, ip4(192, 168, 1, 10), ip4(255, 255, 255, 0), 0) == 0);
	rt_netif_up(&eth0);
	CHECK(rt_netif_set_addr(&eth1, ip4(10, 0, 2, 15), ip4(255, 255, 255, 0), 0) == 0);
	rt_netif_up(&eth1);
	rt_unlock();
}

static void test_dumps(void)
{
	const struct rt_ctl_iface *ifr = NULL;
	const struct rt_ctl_route *rr;
	struct rt_ctl_hdr h;
	struct rt_ctl_dns dns;
	s32 mib[3] = { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, RT_CTL_IFACES };
	u32 len, need;
	int n, i;

	n = fetch("net.riptide.ifaces", sizeof(struct rt_ctl_iface));
	CHECK(n == 3);
	for (i = 0; i < n; i++) {
		const struct rt_ctl_iface *x = rec(i, sizeof(*x));
		if (!strcmp(x->name, "eth0"))
			ifr = x;
		if (!strcmp(x->name, "lo0"))
			CHECK(x->type == RT_IFT_LOOP && x->addr == RT_INADDR_LOOPBACK &&
			      (x->flags & RT_IFF_LOOPBACK) && x->dhcp_state == RT_DHCPS_NONE);
	}
	CHECK(ifr != NULL);
	if (ifr) {
		CHECK(ifr->index == eth0.index && ifr->type == RT_IFT_ETHER && ifr->hwlen == 6);
		CHECK(ifr->hwaddr[5] == 0x10 && ifr->mtu == 1500 && (ifr->flags & RT_IFF_UP));
		CHECK(ifr->addr == ip4(192, 168, 1, 10) && ifr->netmask == ip4(255, 255, 255, 0));
		CHECK(ifr->broadaddr == ip4(192, 168, 1, 255));
		CHECK(!strcmp(ifr->descr, "fake.device unit 0"));
		CHECK(ifr->opackets >= 1);    /* gratuitous ARP */
	}

	/* by MIB, as RiptideSysCtl() does */
	rt_lock();
	len = 0;
	CHECK(rt_sysctl_bymib(mib, 3, NULL, &len, NULL, 0) == 0);
	need = len;
	CHECK(need == sizeof(h) + 3 * sizeof(struct rt_ctl_iface));
	/* short buffer: whole records only, count adjusted, size needed back */
	len = sizeof(h) + sizeof(struct rt_ctl_iface) + 10;
	CHECK(rt_sysctl_bymib(mib, 3, dumpbuf, &len, NULL, 0) == -RT_ENOMEM);
	memcpy(&h, dumpbuf, sizeof(h));
	CHECK(h.count == 1 && len == need);
	len = 4;
	CHECK(rt_sysctl_bymib(mib, 3, dumpbuf, &len, NULL, 0) == -RT_ENOMEM && len == need);
	/* read only */
	CHECK(rt_sysctl_bymib(mib, 3, NULL, NULL, dumpbuf, 8) == -RT_EPERM);
	mib[2] = 99;
	CHECK(rt_sysctl_bymib(mib, 3, NULL, &len, NULL, 0) == -RT_ENOENT);
	rt_unlock();

	n = fetch("net.riptide.routes", sizeof(struct rt_ctl_route));
	rr = find_route(n, ip4(192, 168, 1, 0), ip4(255, 255, 255, 0), 0);
	CHECK(rr && rr->proto == RT_PROTO_KERNEL && rr->ifindex == eth0.index &&
	      !strcmp(rr->ifname, "eth0") && !rr->linkdown && rr->gateway == 0);

	CHECK(fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh)) == 0);

	rt_lock();
	rt_dns_clear_servers();
	rt_dns_add_server(ip4(10, 0, 2, 3));
	rt_strlcpy(rt_hostname, "amiga", sizeof(rt_hostname));
	rt_unlock();
	CHECK(fetch("net.riptide.dns", sizeof(struct rt_ctl_dns)) == 1);
	memcpy(&dns, rec(0, sizeof(dns)), sizeof(dns));
	CHECK(dns.nservers == 1 && dns.servers[0] == ip4(10, 0, 2, 3) && !strcmp(dns.hostname, "amiga"));

	CHECK(fetch("net.riptide.ipstat", sizeof(struct rt_ip_stats)) == 1);
	CHECK(fetch("net.riptide.icmpstat", sizeof(struct rt_icmp_stats)) == 1);
	CHECK(fetch("net.riptide.udpstat", sizeof(struct rt_udp_stats)) == 1);
	CHECK(fetch("net.riptide.tcpstat", sizeof(struct rt_tcp_stats)) == 1);
}

static void test_ioctls(struct rt_sockctx *ctx, int fd)
{
	struct rt_ortentry ort;
	struct rt_arpreq ar;
	u8 bsdar[36];
	struct rt_ifaliasreq ifra;
	struct rt_nexthop nh;
	s32 fl;
	int n;
	const struct rt_ctl_route *rr;

	CHECK(RT_SIOCADDRT == 0x8030720aUL && RT_SIOCDELRT == 0x8030720bUL);
	CHECK(RT_SIOCSARP == 0x8026491eUL && RT_SIOCGARP == 0xc0264926UL);
	CHECK(sizeof(struct rt_ortentry) == 48 && sizeof(struct rt_arpreq) == 38 &&
	      sizeof(struct rt_ifaliasreq) == 64);

	/* default route via a gateway on eth0 */
	memset(&ort, 0, sizeof(ort));
	sin_set(&ort.rt_dst, 0);
	sin_set(&ort.rt_gateway, ip4(192, 168, 1, 1));
	ort.rt_flags = RT_RTF_UP | RT_RTF_GATEWAY;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == -RT_EEXIST);
	rt_lock();
	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 1) &&
	      nh.netif == &eth0);
	rt_unlock();
	/* gateway not on a connected network */
	sin_set(&ort.rt_gateway, ip4(172, 16, 0, 1));
	sin_set(&ort.rt_dst, ip4(172, 17, 0, 0));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == -RT_ENETUNREACH);

	/* network route with an explicit mask (RTF_MASK extension) */
	sin_set(&ort.rt_dst, ip4(10, 20, 0, 0));
	sin_set(&ort.rt_gateway, ip4(10, 0, 2, 2));
	ort.rt_hash = ip4(255, 255, 0, 0);
	ort.rt_flags = RT_RTF_UP | RT_RTF_GATEWAY | RT_RTF_MASK;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	/* classful (widened) mask without it: 10.30.0.0 -> /15 */
	sin_set(&ort.rt_dst, ip4(10, 30, 0, 0));
	ort.rt_hash = 0;
	ort.rt_flags = RT_RTF_UP | RT_RTF_GATEWAY;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	/* host route */
	sin_set(&ort.rt_dst, ip4(1, 2, 3, 4));
	ort.rt_flags = RT_RTF_UP | RT_RTF_GATEWAY | RT_RTF_HOST;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	/* interface route: gateway = local interface address (BSD) */
	sin_set(&ort.rt_dst, ip4(172, 20, 0, 0));
	sin_set(&ort.rt_gateway, ip4(10, 0, 2, 15));
	ort.rt_flags = RT_RTF_UP;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	/* interface route by index (rt_ifp extension) */
	sin_set(&ort.rt_dst, ip4(172, 21, 0, 0));
	sin_set(&ort.rt_gateway, 0);
	ort.rt_ifp = eth0.index;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == 0);
	ort.rt_ifp = 200;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCADDRT, &ort) == -RT_ENXIO);
	ort.rt_ifp = 0;

	n = fetch("net.riptide.routes", sizeof(struct rt_ctl_route));
	rr = find_route(n, 0, 0, ip4(192, 168, 1, 1));
	CHECK(rr && rr->proto == RT_PROTO_STATIC && (rr->flags & RT_RTF_GATEWAY) && rr->ifindex == eth0.index);
	rr = find_route(n, ip4(10, 20, 0, 0), ip4(255, 255, 0, 0), ip4(10, 0, 2, 2));
	CHECK(rr && !strcmp(rr->ifname, "eth1"));
	CHECK(find_route(n, ip4(10, 30, 0, 0), ip4(255, 254, 0, 0), 0) != NULL);
	rr = find_route(n, ip4(1, 2, 3, 4), 0xffffffffu, 0);
	CHECK(rr && (rr->flags & RT_RTF_HOST));
	rr = find_route(n, ip4(172, 20, 0, 0), ip4(255, 255, 0, 0), 0);
	CHECK(rr && rr->ifindex == eth1.index && rr->gateway == 0);
	rr = find_route(n, ip4(172, 21, 0, 0), ip4(255, 255, 0, 0), 0);
	CHECK(rr && rr->ifindex == eth0.index);

	/* delete */
	sin_set(&ort.rt_dst, ip4(10, 20, 0, 0));
	sin_set(&ort.rt_gateway, ip4(10, 0, 2, 2));
	ort.rt_hash = ip4(255, 255, 0, 0);
	ort.rt_flags = RT_RTF_GATEWAY | RT_RTF_MASK;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDELRT, &ort) == 0);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDELRT, &ort) == -RT_ESRCH);
	sin_set(&ort.rt_dst, ip4(1, 2, 3, 4));
	ort.rt_flags = RT_RTF_HOST;
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDELRT, &ort) == 0);
	CHECK(count_routes(ip4(1, 2, 3, 4), 0xffffffffu) == 0);

	/* ARP, AmiTCP layout */
	memset(&ar, 0, sizeof(ar));
	sin_set(&ar.arp_pa, ip4(192, 168, 1, 50));
	ar.arp_ha.sa_family = RT_AF_UNSPEC;
	memcpy(ar.arp_ha.sa_data, "\x52\x55\x0a\x00\x02\x02", 6);
	fl = RT_ATF_PERM;
	memcpy(ar.arp_flags, &fl, 4);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCSARP, &ar) == 0);
	memset(&ar.arp_ha, 0, sizeof(ar.arp_ha));
	memset(ar.arp_flags, 0, 4);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCGARP, &ar) == 0);
	memcpy(&fl, ar.arp_flags, 4);
	CHECK(!memcmp(ar.arp_ha.sa_data, "\x52\x55\x0a\x00\x02\x02", 6));
	CHECK(fl == (RT_ATF_INUSE | RT_ATF_COM | RT_ATF_PERM));
	/* not on a connected network / proxy ARP */
	sin_set(&ar.arp_pa, ip4(8, 8, 8, 8));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCSARP, &ar) == -RT_ENETUNREACH);
	sin_set(&ar.arp_pa, ip4(192, 168, 1, 51));
	fl = RT_ATF_PUBL;
	memcpy(ar.arp_flags, &fl, 4);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCSARP, &ar) == -RT_EOPNOTSUPP);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCGARP, &ar) == -RT_ENXIO);
	/* 4.4BSD layout: flags at offset 32 */
	memset(bsdar, 0, sizeof(bsdar));
	sin_set((struct rt_sockaddr *)bsdar, ip4(10, 0, 2, 2));
	memcpy(bsdar + 18, "\x52\x55\x0a\x00\x02\x03", 6);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCSARP_BSD, bsdar) == 0);
	memset(bsdar + 16, 0, 20);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCGARP_BSD, bsdar) == 0);
	memcpy(&fl, bsdar + 32, 4);
	CHECK(bsdar[23] == 0x03 && (fl & RT_ATF_PERM));

	n = fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh));
	CHECK(n == 2);
	if (n >= 1) {
		const struct rt_ctl_neigh *ne = rec(0, sizeof(*ne));
		const struct rt_ctl_neigh *ne2 = rec(1, sizeof(*ne2));
		if (ne->addr != ip4(192, 168, 1, 50))
			ne = ne2;
		CHECK(ne->addr == ip4(192, 168, 1, 50) && ne->state == RT_NUD_PERMANENT &&
		      ne->ifindex == eth0.index && !strcmp(ne->ifname, "eth0") && ne->hwaddr[5] == 2);
	}
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDARP_BSD, bsdar) == 0);
	sin_set(&ar.arp_pa, ip4(192, 168, 1, 50));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDARP, &ar) == 0);
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCDARP, &ar) == -RT_ENXIO);
	CHECK(fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh)) == 0);

	/* SIOCAIFADDR */
	memset(&ifra, 0, sizeof(ifra));
	rt_strlcpy(ifra.ifra_name, "eth1", sizeof(ifra.ifra_name));
	sin_set(&ifra.ifra_addr, ip4(10, 0, 3, 15));
	sin_set(&ifra.ifra_mask, ip4(255, 255, 0, 0));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCAIFADDR, &ifra) == 0);
	CHECK(eth1.addr == ip4(10, 0, 3, 15) && eth1.netmask == ip4(255, 255, 0, 0) &&
	      eth1.broadaddr == ip4(10, 0, 255, 255));
	sin_set(&ifra.ifra_addr, ip4(10, 0, 2, 15));
	sin_set(&ifra.ifra_mask, ip4(255, 255, 255, 0));
	sin_set(&ifra.ifra_broadaddr, ip4(10, 0, 2, 255));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCAIFADDR, &ifra) == 0);
	CHECK(eth1.addr == ip4(10, 0, 2, 15) && eth1.broadaddr == ip4(10, 0, 2, 255));
	rt_strlcpy(ifra.ifra_name, "nope0", sizeof(ifra.ifra_name));
	CHECK(rt_api_ioctl(ctx, fd, RT_SIOCAIFADDR, &ifra) == -RT_ENXIO);
}

static void test_commands(void)
{
	char out[4096];
	struct rt_nexthop nh;
	const struct rt_ctl_route *rr;
	const struct rt_ctl_iface *ifr;
	struct rt_route *r;
	int n, i;

	/* sysctl queries */
	CHECK(cmd("sysctl net.inet.ip.ttl", out, sizeof(out)) == 0 && !strcmp(out, "net.inet.ip.ttl = 64\n"));
	CHECK(cmd("sysctl -w net.inet.ip.ttl=32", out, sizeof(out)) == 0 &&
	      !strcmp(out, "net.inet.ip.ttl = 32\n") && rt_ip_defttl == 32);
	CHECK(cmd("sysctl net.inet.ip.ttl=64", out, sizeof(out)) == 0 && rt_ip_defttl == 64);
	CHECK(cmd("sysctl -n net.inet.ip.ttl", out, sizeof(out)) == 0 && !strcmp(out, "64\n"));
	CHECK(cmd("sysctl -a", out, sizeof(out)) == 0 && strstr(out, "kern.hostname = amiga\n") &&
	      strstr(out, "net.inet.tcp.") && !strstr(out, "net.riptide."));
	CHECK(cmd("sysctl net.inet.ip", out, sizeof(out)) == 0 && strstr(out, "net.inet.ip.ttl = 64") &&
	      !strstr(out, "tcp"));
	CHECK(cmd("sysctl no.such.thing", out, sizeof(out)) == -RT_ENOENT && strstr(out, "cannot stat"));
	CHECK(cmd("sysctl net.inet.ip.ttl=1000", out, sizeof(out)) == -RT_EINVAL);
	CHECK(cmd("sysctl net.riptide.ifaces", out, sizeof(out)) == 0 && strstr(out, "opaque"));
	/* truncation is safe */
	CHECK(cmd("sysctl -a", out, 40) == 0 && strlen(out) == 39);

	/* errors come back as text */
	CHECK(cmd("bogus", out, sizeof(out)) < 0 && strstr(out, "unknown keyword") &&
	      out[strlen(out) - 1] == '\n');
	CHECK(cmd("route add default via 99.0.0.1", out, sizeof(out)) == -RT_ENETUNREACH &&
	      strstr(out, "99.0.0.1"));

	/* a DHCP default route never beats a static one */
	rt_lock();
	CHECK(rt_route_add(0, 0, ip4(10, 0, 2, 2), &eth1, RT_RTF_UP | RT_RTF_DHCP, 0) == 0);
	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 1));
	rt_unlock();
	n = fetch("net.riptide.routes", sizeof(struct rt_ctl_route));
	rr = find_route(n, 0, 0, ip4(10, 0, 2, 2));
	CHECK(rr && rr->proto == RT_PROTO_DHCP && !strcmp(rr->ifname, "eth1"));
	/* ... even when the static one is added later */
	CHECK(cmd("route del default via 192.168.1.1", out, sizeof(out)) == 0);
	rt_lock();
	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(10, 0, 2, 2));
	rt_unlock();
	CHECK(cmd("route add default via 192.168.1.1 dev eth0", out, sizeof(out)) == 0);
	rt_lock();
	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 1));
	rt_unlock();
	CHECK(count_routes(0, 0) == 2);
	/* replace: one route left, the new gateway */
	CHECK(cmd("route replace default via 192.168.1.254", out, sizeof(out)) == 0);
	CHECK(count_routes(0, 0) == 1);
	rt_lock();
	CHECK(rt_ip_route(ip4(8, 8, 8, 8), &nh) == 0 && nh.nexthop == ip4(192, 168, 1, 254));
	rt_unlock();
	CHECK(cmd("route change 10.99.0.0/16 via 10.0.2.2", out, sizeof(out)) == -RT_ESRCH);
	/* classic syntax still works */
	CHECK(cmd("route add -net 10.40.0.0 netmask 255.255.0.0 10.0.2.2", out, sizeof(out)) == 0);
	CHECK(count_routes(ip4(10, 40, 0, 0), ip4(255, 255, 0, 0)) == 1);
	CHECK(cmd("route add -host 10.41.0.1 gw 10.0.2.2", out, sizeof(out)) == 0);
	CHECK(count_routes(ip4(10, 41, 0, 1), 0xffffffffu) == 1);
	/* metric and mtu */
	CHECK(cmd("route add 10.9.0.0/16 via 10.0.2.2 dev eth1 metric 5 mtu 1400", out, sizeof(out)) == 0);
	n = fetch("net.riptide.routes", sizeof(struct rt_ctl_route));
	rr = find_route(n, ip4(10, 9, 0, 0), ip4(255, 255, 0, 0), 0);
	CHECK(rr && rr->metric == 5 && rr->mtu == 1400 && rr->proto == RT_PROTO_STATIC);
	rt_lock();
	CHECK(rt_ip_route(ip4(10, 9, 1, 1), &nh) == 0 && nh.mtu == 1400);
	rt_unlock();
	CHECK(cmd("route change 10.9.0.0/16 via 10.0.2.3 metric 5", out, sizeof(out)) == 0);
	CHECK(count_routes(ip4(10, 9, 0, 0), ip4(255, 255, 0, 0)) == 1);
	CHECK(cmd("route del 10.9.0.0/16", out, sizeof(out)) == 0);
	CHECK(cmd("route del 10.9.0.0/16", out, sizeof(out)) == -RT_ESRCH);
	/* reject route */
	CHECK(cmd("route add 10.66.0.0/16 unreachable", out, sizeof(out)) == 0);
	rt_lock();
	CHECK(rt_ip_route(ip4(10, 66, 0, 1), &nh) == -RT_EHOSTUNREACH);
	rt_unlock();
	/* flush dev: static routes of eth1 go, the connected one stays */
	CHECK(cmd("route flush dev eth1", out, sizeof(out)) == 0);
	CHECK(count_routes(ip4(10, 40, 0, 0), ip4(255, 255, 0, 0)) == 0);
	CHECK(count_routes(ip4(10, 0, 2, 0), ip4(255, 255, 255, 0)) == 1);
	CHECK(count_routes(0, 0) == 1);
	/* PMTU cache */
	rt_lock();
	rt_ip_pmtu_update(ip4(8, 8, 4, 4), 1280);
	CHECK(rt_ip_route(ip4(8, 8, 4, 4), &nh) == 0 && nh.mtu == 1280);
	rt_unlock();
	CHECK(cmd("route flush cache", out, sizeof(out)) == 0);
	rt_lock();
	CHECK(rt_ip_route(ip4(8, 8, 4, 4), &nh) == 0 && nh.mtu == 1500);
	rt_unlock();

	/* link down: configured routes stay but are flagged linkdown */
	CHECK(cmd("ifconfig eth0 down", out, sizeof(out)) == 0 && !(eth0.flags & RT_IFF_UP));
	n = fetch("net.riptide.routes", sizeof(struct rt_ctl_route));
	rr = find_route(n, 0, 0, 0);
	CHECK(rr && rr->linkdown);
	CHECK(cmd("ifconfig eth0 up", out, sizeof(out)) == 0 && (eth0.flags & RT_IFF_UP));

	/* ifconfig extras */
	CHECK(cmd("ifconfig eth0 promisc mtu 1400 metric 3", out, sizeof(out)) == 0);
	CHECK((eth0.flags & RT_IFF_PROMISC) && eth0.mtu == 1400 && eth0.metric == 3);
	CHECK(cmd("ifconfig eth0 -promisc -arp mtu 1500 metric 0", out, sizeof(out)) == 0);
	CHECK(!(eth0.flags & RT_IFF_PROMISC) && (eth0.flags & RT_IFF_NOARP));
	CHECK(cmd("ifconfig eth0 arp broadcast 192.168.1.127", out, sizeof(out)) == 0);
	CHECK(!(eth0.flags & RT_IFF_NOARP) && eth0.broadaddr == ip4(192, 168, 1, 127));
	CHECK(cmd("ifconfig eth0 192.168.1.10 netmask 255.255.255.0 broadcast 192.168.1.255", out,
	          sizeof(out)) == 0 && eth0.broadaddr == ip4(192, 168, 1, 255));
	CHECK(cmd("ifconfig eth1 10.0.2.99 delete", out, sizeof(out)) == -RT_EADDRNOTAVAIL);
	n = fetch("net.riptide.ifaces", sizeof(struct rt_ctl_iface));
	for (i = 0, ifr = NULL; i < n; i++)
		if (!strcmp(((const struct rt_ctl_iface *)rec(i, sizeof(*ifr)))->name, "eth0"))
			ifr = rec(i, sizeof(*ifr));
	CHECK(ifr && ifr->mtu == 1500 && ifr->metric == 0);

	/* neighbours */
	CHECK(cmd("arp add 192.168.1.60 52:55:0a:00:02:aa", out, sizeof(out)) == 0);
	CHECK(cmd("arp add 192.168.1.61 lladdr 2:0:0:0:0:1 dev eth0 nud permanent", out, sizeof(out)) == 0);
	CHECK(cmd("arp add 8.8.8.8 52:55:0a:00:02:aa", out, sizeof(out)) == -RT_ENETUNREACH);
	CHECK(cmd("arp add 192.168.1.62 52:55:zz", out, sizeof(out)) < 0);
	CHECK(fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh)) == 2);
	CHECK(cmd("arp del 192.168.1.60", out, sizeof(out)) == 0);
	CHECK(cmd("arp del 192.168.1.60", out, sizeof(out)) == -RT_ENXIO);
	CHECK(cmd("arp flush dev eth0", out, sizeof(out)) == 0);
	CHECK(fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh)) == 0);

	/* resolver */
	CHECK(cmd("nameserver add 1.1.1.1", out, sizeof(out)) == 0);
	CHECK(cmd("nameserver 9.9.9.9", out, sizeof(out)) == 0);
	CHECK(fetch("net.riptide.dns", sizeof(struct rt_ctl_dns)) == 1 &&
	      ((struct rt_ctl_dns *)rec(0, sizeof(struct rt_ctl_dns)))->nservers == 3);
	CHECK(cmd("nameserver del 1.1.1.1", out, sizeof(out)) == 0);
	CHECK(cmd("nameserver del 1.1.1.1", out, sizeof(out)) == -RT_ESRCH);
	CHECK(cmd("nameserver flush", out, sizeof(out)) == 0 && rt_dns_get_servers(NULL, 0) == 0);
	CHECK(cmd("domain example.net", out, sizeof(out)) == 0);
	CHECK(fetch("net.riptide.dns", sizeof(struct rt_ctl_dns)) == 1 &&
	      !strcmp(((struct rt_ctl_dns *)rec(0, sizeof(struct rt_ctl_dns)))->domain, "example.net"));

	/* dhcp verbs without a client */
	CHECK(cmd("dhcp eth1 stop", out, sizeof(out)) == -RT_ESRCH);
	CHECK(cmd("dhcp eth1 renew", out, sizeof(out)) == -RT_ESRCH);
	CHECK(cmd("dhcp eth1 frobnicate", out, sizeof(out)) == -RT_EINVAL);
	CHECK(cmd("dhcp nope0", out, sizeof(out)) == -RT_ENXIO);
	CHECK(cmd("dhcp eth1 start", out, sizeof(out)) == 0);
	n = fetch("net.riptide.ifaces", sizeof(struct rt_ctl_iface));
	for (i = 0, ifr = NULL; i < n; i++)
		if (!strcmp(((const struct rt_ctl_iface *)rec(i, sizeof(*ifr)))->name, "eth1"))
			ifr = rec(i, sizeof(*ifr));
	CHECK(ifr && ifr->dhcp_state == RT_DHCPS_INIT);
	CHECK(cmd("dhcp eth1 stop", out, sizeof(out)) == 0);

	/* removing an interface removes its routes and neighbours */
	CHECK(cmd("route add 10.50.0.0/16 via 10.0.2.2", out, sizeof(out)) == 0);
	CHECK(cmd("arp add 10.0.2.2 52:55:0a:00:02:02", out, sizeof(out)) == 0);
	rt_lock();
	rt_netif_remove(&eth1);
	for (r = rt_route_list(); r; r = r->next)
		CHECK(r->netif != &eth1);
	rt_unlock();
	CHECK(fetch("net.riptide.neigh", sizeof(struct rt_ctl_neigh)) == 0);
	CHECK(fetch("net.riptide.ifaces", sizeof(struct rt_ctl_iface)) == 2);
}

static void test_sockets(struct rt_sockctx *ctx)
{
	struct rt_sockaddr_in sin;
	int fd = rt_api_socket(ctx, RT_AF_INET, RT_SOCK_DGRAM, 0), raw, n, i, found = 0;

	raw = rt_api_socket(ctx, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(5555);
	CHECK(rt_api_bind(ctx, fd, &sin, sizeof(sin)) == 0);
	n = fetch("net.riptide.sockets", sizeof(struct rt_ctl_sock));
	for (i = 0; i < n; i++) {
		const struct rt_ctl_sock *s = rec(i, sizeof(*s));
		if (s->kind == RT_CTL_SOCK_UDP && s->lport == 5555 && s->proto == RT_IPPROTO_UDP)
			found |= 1;
		if (s->kind == RT_CTL_SOCK_RAW && s->proto == RT_IPPROTO_ICMP)
			found |= 2;
	}
	CHECK(found == 3);
	rt_api_close(ctx, fd);
	rt_api_close(ctx, raw);
}

/* IPv6 configuration verbs and the IPv6 tables */
static int find_addr6(const char *a, u8 plen)
{
	rt_in6_addr x;
	int n = fetch("net.riptide.addrs6", sizeof(struct rt_ctl_addr6)), i;

	rt_inet6_pton(a, &x);
	for (i = 0; i < n; i++) {
		const struct rt_ctl_addr6 *r = rec(i, sizeof(*r));
		if (!memcmp(r->addr, x.b, 16) && r->plen == plen)
			return i;
	}
	return -1;
}

static const struct rt_ctl_route6 *find_route6(const char *dst, u8 plen, const char *gw)
{
	rt_in6_addr d, g;
	int n = fetch("net.riptide.routes6", sizeof(struct rt_ctl_route6)), i;

	rt_inet6_pton(dst, &d);
	if (gw)
		rt_inet6_pton(gw, &g);
	for (i = 0; i < n; i++) {
		const struct rt_ctl_route6 *r = rec(i, sizeof(*r));
		if (!memcmp(r->dst, d.b, 16) && r->plen == plen && (!gw || !memcmp(r->gateway, g.b, 16)) &&
		    !(r->flags & RT_RTF_PMTU))
			return r;
	}
	return NULL;
}

static void test_ipv6(struct rt_sockctx *ctx)
{
	char out[512];
	int i, n, found;

	/* interfaces came up with IPv6: link-local addresses, ::1 on lo0 */
	CHECK(find_addr6("::1", 128) >= 0);
	n = fetch("net.riptide.ifaces6", sizeof(struct rt_ctl_iface6));
	for (i = 0, found = 0; i < n; i++) {
		const struct rt_ctl_iface6 *f = rec(i, sizeof(*f));
		if (!strcmp(f->name, "eth0") && f->enabled && f->nd && f->mtu == 1500 && f->cur_hlim == 64)
			found = 1;
	}
	CHECK(found);
	CHECK(cmd("ifconfig eth0 inet6 2001:db8:a::10/64", out, sizeof(out)) == 0);
	CHECK(cmd("ifconfig eth0 inet6 2001:db8:a::11 prefixlen 80 nodad", out, sizeof(out)) == 0);
	CHECK(find_addr6("2001:db8:a::10", 64) >= 0 && find_addr6("2001:db8:a::11", 80) >= 0);
	i = find_addr6("2001:db8:a::10", 64);
	{
		const struct rt_ctl_addr6 *r = rec(i, sizeof(*r));
		CHECK(!strcmp(r->ifname, "eth0") && r->scope == 14 && (r->flags & RT_CTL_IN6_TENTATIVE) &&
		      r->valid_s == 0xffffffffu);
	}
	CHECK(find_route6("2001:db8:a::", 64, NULL) != NULL);
	CHECK(cmd("ifconfig eth0 inet6 2001:db8:zz::1/64", out, sizeof(out)) < 0 && strstr(out, "bad IPv6"));
	/* routes: -inet6, iproute2 style, link-local gateways need a device */
	CHECK(cmd("route add -inet6 default 2001:db8:a::1", out, sizeof(out)) == 0);
	CHECK(find_route6("::", 0, "2001:db8:a::1") != NULL);
	CHECK(cmd("route add 2001:db8:b::/48 via fe80::1", out, sizeof(out)) < 0 && strstr(out, "dev IF"));
	CHECK(cmd("route add 2001:db8:b::/48 via fe80::1 dev eth0 metric 5", out, sizeof(out)) == 0);
	{
		const struct rt_ctl_route6 *r = find_route6("2001:db8:b::", 48, "fe80::1");
		CHECK(r && !strcmp(r->ifname, "eth0") && r->metric == 5 && r->proto == RT_PROTO_STATIC &&
		      (r->flags & RT_RTF_GATEWAY));
	}
	CHECK(cmd("route add 2001:db8:c::/48 via fe80::2%eth0", out, sizeof(out)) == 0);
	CHECK(cmd("route add 2001:db8:d::/48 via 2001:db8:99::1", out, sizeof(out)) < 0);
	CHECK(cmd("route del -inet6 2001:db8:b::/48", out, sizeof(out)) == 0);
	CHECK(find_route6("2001:db8:b::", 48, NULL) == NULL);
	CHECK(cmd("route del -inet6 2001:db8:b::/48", out, sizeof(out)) < 0);
	CHECK(cmd("route flush -inet6 dev eth0", out, sizeof(out)) == 0 && find_route6("2001:db8:c::", 48, NULL) == NULL);
	/* the IPv4 table is untouched */
	CHECK(fetch("net.riptide.routes", sizeof(struct rt_ctl_route)) > 0);
	/* resolver: IPv6 nameservers, zone required for link-local */
	CHECK(cmd("nameserver 2001:4860:4860::8888", out, sizeof(out)) == 0);
	CHECK(cmd("nameserver fe80::53", out, sizeof(out)) < 0);
	CHECK(cmd("nameserver fe80::53%eth0", out, sizeof(out)) == 0);
	CHECK(fetch("net.riptide.dns6", sizeof(struct rt_ctl_dns6)) == 1);
	{
		const struct rt_ctl_dns6 *d = rec(0, sizeof(*d));
		int k;
		for (k = 0, found = 0; k < (int)d->nservers; k++)
			if (d->servers[k].addr[15] == 0x53 && d->servers[k].ifindex == eth0.index)
				found = 1;
		CHECK(found);
	}
	CHECK(cmd("nameserver del fe80::53", out, sizeof(out)) == 0);
	CHECK(cmd("nameserver del 2001:4860:4860::8888", out, sizeof(out)) == 0);
	CHECK(cmd("host six.example 2001:db8:a::99", out, sizeof(out)) == 0);
	/* neighbours */
	CHECK(cmd("arp add 2001:db8:a::42 02:00:00:00:00:42 dev eth0", out, sizeof(out)) == 0);
	n = fetch("net.riptide.neigh6", sizeof(struct rt_ctl_neigh6));
	for (i = 0, found = 0; i < n; i++) {
		const struct rt_ctl_neigh6 *e = rec(i, sizeof(*e));
		if (e->addr[15] == 0x42 && e->state == RT_NUD_PERMANENT && e->hwaddr[5] == 0x42 && !strcmp(e->ifname, "eth0"))
			found = 1;
	}
	CHECK(found);
	CHECK(cmd("arp del 2001:db8:a::42 dev eth0", out, sizeof(out)) == 0);
	CHECK(cmd("arp del 2001:db8:a::42 dev eth0", out, sizeof(out)) < 0);
	/* sysctls */
	CHECK(cmd("sysctl net.inet6.ip6.hlim=32", out, sizeof(out)) == 0 && strstr(out, "= 32"));
	CHECK(cmd("sysctl net.inet6.ip6.forwarding net.inet6.ip6.accept_rtadv net.inet6.ip6.use_tempaddr",
	          out, sizeof(out)) == 0 && strstr(out, "forwarding = 0") && strstr(out, "accept_rtadv = 1") &&
	      strstr(out, "use_tempaddr = 0"));
	CHECK(cmd("sysctl net.inet6.ip6.hlim=64", out, sizeof(out)) == 0);
	{
		s32 mib[4] = { RT_CTL_NET, 28, 41, 3 }, v = 0;
		u32 l = sizeof(v);
		rt_lock();
		CHECK(rt_sysctl_bymib(mib, 4, &v, &l, NULL, 0) == 0 && v == 64);
		rt_unlock();
	}
	/* sockets: AF_INET6 ones in sockets6 only */
	{
		struct rt_sockaddr_in6 s6;
		int fd = rt_api_socket(ctx, RT_AF_INET6, RT_SOCK_DGRAM, 0);
		memset(&s6, 0, sizeof(s6));
		s6.sin6_len = sizeof(s6);
		s6.sin6_family = RT_AF_INET6;
		s6.sin6_port = rt_htons(6666);
		CHECK(rt_api_bind(ctx, fd, &s6, sizeof(s6)) == 0);
		n = fetch("net.riptide.sockets6", sizeof(struct rt_ctl_sock6));
		for (i = 0, found = 0; i < n; i++) {
			const struct rt_ctl_sock6 *s = rec(i, sizeof(*s));
			if (s->kind == RT_CTL_SOCK_UDP && s->lport == 6666)
				found = 1;
		}
		CHECK(found);
		n = fetch("net.riptide.sockets", sizeof(struct rt_ctl_sock));
		for (i = 0, found = 0; i < n; i++) {
			const struct rt_ctl_sock *s = rec(i, sizeof(*s));
			if (s->lport == 6666)
				found = 1;
		}
		CHECK(!found);
		rt_api_close(ctx, fd);
	}
	CHECK(fetch("net.riptide.ip6stat", sizeof(struct rt_ip6_stats)) == 1);
	CHECK(fetch("net.riptide.icmp6stat", sizeof(struct rt_icmp6_stats)) == 1);
	CHECK(cmd("ifconfig eth0 inet6 2001:db8:a::10 delete", out, sizeof(out)) == 0 &&
	      find_addr6("2001:db8:a::10", 64) < 0);
	/* ipv6 off / on per interface: off drops every IPv6 address */
	CHECK(cmd("ipv6 eth0 off", out, sizeof(out)) == 0 && !rt_ip6_ifenabled(&eth0) &&
	      find_addr6("2001:db8:a::11", 80) < 0);
	CHECK(cmd("ipv6 eth0 on", out, sizeof(out)) == 0 && rt_ip6_ifenabled(&eth0));
	CHECK(cmd("ipv6 nosuch on", out, sizeof(out)) < 0);
}

int main(void)
{
	struct rt_sockctx ctx;
	int fd;

	CHECK(rt_init() == 0);
	setup();
	test_dumps();
	CHECK(rt_ctx_init(&ctx, 16, NULL) == 0);
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	test_ioctls(&ctx, fd);
	test_commands();
	test_sockets(&ctx);
	test_ipv6(&ctx);
	rt_api_close(&ctx, fd);
	rt_ctx_destroy(&ctx);
	printf("t_ctl_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
