/*
 * Riptide - unit tests of track H (Phase 3): transmit priority bands
 * (qos.c: classification, strict order behind a driver that holds frames,
 * tail drops, the failsafe timer, "qos" commands and sysctls), socket
 * priority and DSCP (SO_PRIORITY, "qos rule" by port and program), several
 * IPv4 addresses per interface (add/delete/promote, routes, source
 * selection, broadcast, ARP, config lines, net.riptide.addrs4) and
 * "ipv4 on|off". Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the fake driver keeps what it gets, like a device with a busy queue */
#define HELD 64
static struct rt_buf *held[HELD];
static u16 held_et[HELD];
static int nheld;

static int hold_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	rt_drv_tx_start(nif, b->len);
	if (nheld < HELD) {
		held_et[nheld] = et;
		held[nheld++] = b;
	} else {
		rt_buf_free(b);
	}
	return 0;
}

/* the driver finishes its oldest frame */
static void complete_one(struct rt_netif *nif)
{
	struct rt_buf *b;

	if (!nheld)
		return;
	b = held[0];
	memmove(&held[0], &held[1], (size_t)(nheld - 1) * sizeof(held[0]));
	memmove(&held_et[0], &held_et[1], (size_t)(nheld - 1) * sizeof(held_et[0]));
	nheld--;
	rt_buf_free(b);
	rt_drv_tx_done(nif, 0);
}

static void drop_held(void)
{
	while (nheld)
		rt_buf_free(held[--nheld]);
}

static struct rt_netif eth0, eth1;
static struct rt_drv_stats d0;

static u32 A(int a, int b, int c, int d)
{
	return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d);
}

/* an IPv4 packet: proto, DSCP, payload length (TCP: 20 byte header) */
static struct rt_buf *pkt(u8 proto, u8 dscp, u32 payload, u16 dport, u8 prio)
{
	struct rt_buf *b = rt_buf_alloc(64, 20 + 20 + payload);
	u8 *p;

	p = rt_buf_put(b, 20 + (proto == RT_IPPROTO_UDP ? 8 : proto == RT_IPPROTO_TCP ? 20 : 8) + payload);
	memset(p, 0, b->len);
	p[0] = 0x45;
	p[1] = (u8)(dscp << 2);
	rt_put_be16(p + 2, (u16)b->len);
	p[8] = 64;
	p[9] = proto;
	if (proto == RT_IPPROTO_TCP)
		p[32] = 5 << 4;
	if (proto == RT_IPPROTO_TCP || proto == RT_IPPROTO_UDP) {
		rt_put_be16(p + 20, 40000);
		rt_put_be16(p + 22, dport);
	}
	b->prio = prio;
	return b;
}

static void test_bands(void)
{
	struct rt_buf *b;

	b = pkt(RT_IPPROTO_TCP, 0, 0, 80, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);     /* ACK */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 0, 1400, 80, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_DEFAULT);
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 0, 3, 23, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);     /* keystroke */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 8, 1400, 80, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_BULK);            /* CS1 */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 46, 1400, 80, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);     /* EF */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 0, 1400, 80, 1 + 1);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_BULK);            /* SO_PRIORITY 1 */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_TCP, 0, 1400, 80, 6 + 1);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);     /* SO_PRIORITY 6 */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_UDP, 0, 30, 53, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);     /* DNS */
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_UDP, 0, 1000, 9999, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_DEFAULT);
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_ICMP, 0, 56, 0, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_IP) == RT_QOS_BAND_INTERACTIVE);
	rt_buf_free(b);
	b = pkt(RT_IPPROTO_UDP, 0, 30, 0, 0);
	CHECK(rt_qos_band(b, RT_ETH_P_ARP) == RT_QOS_BAND_INTERACTIVE);
	rt_buf_free(b);
	CHECK(rt_qos_prio2dscp(6) == 46 && rt_qos_prio2dscp(1) == 8 && rt_qos_prio2dscp(0) == 0);
}

static void test_queue(void)
{
	struct rt_ctl_qos q;
	struct rt_buf *b;
	char out[512];
	int i;

	rt_lock();
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	eth0.mtu = 1500;
	eth0.flags = RT_IFF_BROADCAST;
	eth0.link_output = hold_output;
	CHECK(rt_netif_add(&eth0) >= 0);
	rt_drv_attach(&eth0, &d0);
	CHECK(eth0.qos != NULL);               /* qos is on by default */
	rt_ip6_ifenable(&eth0, 0);              /* no RS, MLD, DAD in the counts */
	rt_netif_up(&eth0);
	drop_held();                           /* gratuitous ARP and the like */
	d0.tx_inflight = 0;
	rt_qos_inflight = 2;
	/* two frames go straight to the driver, the rest wait */
	for (i = 0; i < 4; i++)
		CHECK(rt_netif_xmit(&eth0, pkt(RT_IPPROTO_TCP, 8, 1400, 80, 0), NULL, RT_ETH_P_IP) == 0);
	CHECK(nheld == 2 && d0.tx_inflight == 2);
	b = pkt(RT_IPPROTO_TCP, 0, 1400, 80, 0);
	CHECK(rt_netif_xmit(&eth0, b, NULL, RT_ETH_P_IP) == 0);              /* default */
	CHECK(rt_netif_xmit(&eth0, pkt(RT_IPPROTO_TCP, 0, 0, 80, 0), NULL, RT_ETH_P_IP) == 0);  /* ACK */
	CHECK(nheld == 2);
	rt_qos_fill(&eth0, &q);
	CHECK(q.queued[0] == 1 && q.queued[1] == 1 && q.queued[2] == 2 && q.bypass == 2);
	/* the driver finishes one: the ACK goes next, then the default one */
	complete_one(&eth0);
	CHECK(nheld == 2 && held[1]->len == 40);
	complete_one(&eth0);
	CHECK(nheld == 2 && held[1] == b);
	complete_one(&eth0);
	complete_one(&eth0);
	complete_one(&eth0);
	complete_one(&eth0);
	rt_qos_fill(&eth0, &q);
	CHECK(q.queued[0] == 0 && q.queued[1] == 0 && q.queued[2] == 0);
	CHECK(q.packets[0] == 1 && q.packets[1] == 1 && q.packets[2] == 2);
	CHECK(nheld == 0 && d0.tx_inflight == 0);

	/* a full band drops at its tail */
	rt_qos_limit = 8;
	for (i = 0; i < 2 + 8 + 3; i++)
		rt_netif_xmit(&eth0, pkt(RT_IPPROTO_TCP, 8, 1000, 80, 0), NULL, RT_ETH_P_IP);
	rt_qos_fill(&eth0, &q);
	CHECK(q.queued[2] == 8 && q.drops[2] == 3 && q.maxq[2] == 8);
	/* a driver that never completes: the failsafe sends one frame per 250 ms */
	rt_unlock();
	rt_host_advance_clock(300);
	rt_lock();
	rt_stack_poll();
	CHECK(nheld == 3);
	rt_qos_fill(&eth0, &q);
	CHECK(q.queued[2] == 7);
	/* qos off: what waits goes to the driver */
	CHECK(rt_config_line("qos off eth0", out, sizeof(out), NULL) == 0 && eth0.qos == NULL);
	CHECK(nheld == 10);
	drop_held();
	d0.tx_inflight = 0;
	CHECK(rt_config_line("qos on eth0", out, sizeof(out), NULL) == 0 && eth0.qos != NULL);
	CHECK(rt_config_line("qos inflight 6", out, sizeof(out), NULL) == 0 && rt_qos_inflight == 6);
	CHECK(rt_config_line("qos inflight 99", out, sizeof(out), NULL) < 0);
	CHECK(rt_config_line("qos limit 64", out, sizeof(out), NULL) == 0 && rt_qos_limit == 64);
	CHECK(rt_config_line("qos priodscp off", out, sizeof(out), NULL) == 0 && !rt_qos_prio_dscp);
	CHECK(rt_config_line("qos priodscp on", out, sizeof(out), NULL) == 0 && rt_qos_prio_dscp);
	CHECK(rt_config_line("qos status", out, sizeof(out), NULL) == 0 && strstr(out, "eth0: on"));
	/* the loopback has no completions to pace by */
	CHECK(rt_config_line("qos on lo0", out, sizeof(out), NULL) < 0 && strstr(out, "completions"));
	rt_unlock();
}

static void prog_hook(char *buf, u32 len)
{
	rt_strlcpy(buf, "Work:Net/AmiFTP", len);
}

static void test_priority_rules(void)
{
	struct rt_sockctx c;
	struct rt_socket *so;
	struct rt_sockaddr_in sin;
	char out[1024];
	s32 v;
	u32 len;
	int fd, fd2;

	rt_ctx_init(&c, 16, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	so = c.fds[fd];
	CHECK(rt_qos_sock_prio(so) == 0 && rt_qos_sock_tos(so, 0) == 0);
	v = 6;
	CHECK(rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_PRIORITY, &v, sizeof(v)) == 0);
	v = 0;
	len = sizeof(v);
	CHECK(rt_api_getsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_PRIORITY_LINUX, &v, &len) == 0 && v == 6);
	CHECK(rt_qos_sock_prio(so) == 7);
	CHECK(rt_qos_sock_tos(so, 0) == (46 << 2));          /* EF */
	CHECK(rt_qos_sock_tos(so, 2) == ((46 << 2) | 2));    /* ECN bits kept */
	CHECK(rt_qos_sock_tos(so, 0x10) == 0x10);            /* IP_TOS wins */
	v = 8;
	CHECK(rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_PRIORITY, &v, sizeof(v)) == -RT_EINVAL);
	rt_api_close(&c, fd);

	/* rules: by program, by port; SO_PRIORITY of the program wins */
	rt_qos_program_hook = prog_hook;
	rt_lock();
	CHECK(rt_config_line("qos rule program=AmiFTP priority=bulk", out, sizeof(out), NULL) == 0);
	CHECK(rt_config_line("qos rule proto=tcp port=22,23 priority=interactive dscp=CS4", out, sizeof(out),
	                     NULL) == 0);
	CHECK(rt_config_line("qos rule port=0 priority=bulk", out, sizeof(out), NULL) < 0);
	CHECK(rt_config_line("qos rule priority=bulk", out, sizeof(out), NULL) < 0);
	CHECK(rt_config_line("qos status", out, sizeof(out), NULL) == 0 &&
	      strstr(out, "qos rule program=AmiFTP priority=bulk") &&
	      strstr(out, "qos rule proto=tcp port=22,23 priority=interactive dscp=CS4"));
	rt_unlock();
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	so = c.fds[fd];
	CHECK(rt_qos_sock_prio(so) == 2 && rt_qos_sock_tos(so, 0) == (8 << 2));    /* bulk: CS1 */
	rt_qos_program_hook = NULL;
	fd2 = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(rt_qos_sock_prio(c.fds[fd2]) == 0);
	/* a local port of 23: interactive with the rule's DSCP */
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(23);
	CHECK(rt_api_bind(&c, fd2, &sin, sizeof(sin)) == 0);
	CHECK(rt_qos_sock_prio(c.fds[fd2]) == 7 && rt_qos_sock_tos(c.fds[fd2], 0) == (32 << 2));
	rt_api_close(&c, fd);
	rt_api_close(&c, fd2);
	rt_lock();
	CHECK(rt_config_line("qos rules flush", out, sizeof(out), NULL) == 0 && rt_qos_nrules() == 0);
	rt_unlock();
	rt_ctx_destroy(&c);
}

static int has_route(u32 dst, u32 mask, struct rt_netif *nif)
{
	struct rt_nexthop nh;

	(void)mask;
	return rt_ip_route(dst, &nh) == 0 && nh.netif == nif;
}

static void test_addresses(void)
{
	struct rt_ifaddr4 a[1 + RT_IF_MAXADDR4];
	struct rt_nexthop nh;
	char out[512];
	u8 buf[1024];
	u32 len;
	int i;

	rt_lock();
	rt_strlcpy(eth1.name, "eth1", sizeof(eth1.name));
	eth1.type = RT_IFT_ETHER;
	eth1.hwlen = 6;
	eth1.mtu = 1500;
	eth1.flags = RT_IFF_BROADCAST;
	eth1.link_output = hold_output;
	CHECK(rt_netif_add(&eth1) >= 0);
	rt_ip6_ifenable(&eth1, 0);
	rt_netif_up(&eth1);
	/* the first one is the primary, the next ones are additional */
	CHECK(rt_netif_addr_add(&eth1, A(10, 1, 0, 2), A(255, 255, 255, 0), 0) == 0);
	CHECK(eth1.addr == A(10, 1, 0, 2) && eth1.nalias == 0);
	CHECK(rt_config_line("ifconfig eth1 alias 10.2.0.2/16", out, sizeof(out), NULL) == 0);
	CHECK(rt_config_line("ifconfig eth1 alias 10.3.0.2 netmask 255.255.255.0 broadcast 10.3.0.127",
	                     out, sizeof(out), NULL) == 0);
	CHECK(eth1.nalias == 2 && eth1.alias[0].addr == A(10, 2, 0, 2) && eth1.alias[0].netmask == A(255, 255, 0, 0));
	CHECK(eth1.alias[1].broadaddr == A(10, 3, 0, 127));
	CHECK(rt_netif_addrs(&eth1, a, 1 + RT_IF_MAXADDR4) == 3 && a[0].addr == A(10, 1, 0, 2));
	/* routes, locality, broadcast, source selection */
	CHECK(has_route(A(10, 2, 9, 9), 0, &eth1) && has_route(A(10, 3, 0, 9), 0, &eth1));
	CHECK(rt_ip_is_local(A(10, 3, 0, 2)) && rt_netif_by_addr(A(10, 2, 0, 2)) == &eth1);
	CHECK(rt_ip_is_broadcast(A(10, 2, 255, 255), &eth1) && rt_ip_is_broadcast(A(10, 3, 0, 127), NULL));
	CHECK(rt_ip_route(A(10, 2, 7, 7), &nh) == 0 && nh.src == A(10, 2, 0, 2));
	CHECK(rt_ip_route(A(10, 1, 0, 7), &nh) == 0 && nh.src == A(10, 1, 0, 2));
	/* a default route via a gateway on the additional subnet: its source */
	CHECK(rt_route_add(0, 0, A(10, 3, 0, 1), &eth1, 0, 0) == 0);
	CHECK(rt_ip_route(A(8, 8, 8, 8), &nh) == 0 && nh.src == A(10, 3, 0, 2));
	CHECK(rt_netif_src(&eth1, A(8, 8, 8, 8), A(10, 1, 0, 1)) == A(10, 1, 0, 2));
	CHECK(rt_config_line("route del default", out, sizeof(out), NULL) == 0);
	/* the address table */
	len = sizeof(buf);
	CHECK(rt_sysctl_byname("net.riptide.addrs4", buf, &len, NULL, 0) == 0);
	{
		struct rt_ctl_hdr h;
		struct rt_ctl_addr4 *r = (struct rt_ctl_addr4 *)(void *)(buf + sizeof(h));
		int n2 = 0;
		memcpy(&h, buf, sizeof(h));
		CHECK(h.recsize == sizeof(*r));
		for (i = 0; i < (int)h.count; i++)
			if (r[i].ifindex == eth1.index) {
				n2++;
				if (r[i].addr == A(10, 1, 0, 2))
					CHECK(r[i].flags == RT_CTL_A4_PRIMARY && r[i].prefixlen == 24);
				if (r[i].addr == A(10, 2, 0, 2))
					CHECK(r[i].flags == 0 && r[i].prefixlen == 16);
			}
		CHECK(n2 == 3);
	}
	/* duplicates update, limits */
	CHECK(rt_netif_addr_add(&eth1, A(10, 2, 0, 2), A(255, 255, 255, 0), 0) == 0 && eth1.nalias == 2 &&
	      eth1.alias[0].netmask == A(255, 255, 255, 0));
	for (i = 0; i < RT_IF_MAXADDR4 - 2; i++)
		CHECK(rt_netif_addr_add(&eth1, A(10, 4, 0, (int)(10 + i)), A(255, 255, 255, 0), 0) == 0);
	CHECK(rt_netif_addr_add(&eth1, A(10, 5, 0, 1), A(255, 255, 255, 0), 0) == -RT_ENOSPC);
	for (i = 0; i < RT_IF_MAXADDR4 - 2; i++)
		CHECK(rt_netif_addr_del(&eth1, A(10, 4, 0, (int)(10 + i))) == 0);
	CHECK(eth1.nalias == 2 && has_route(A(10, 2, 0, 9), 0, &eth1));
	/* deleting one keeps the others' routes */
	CHECK(rt_config_line("ifconfig eth1 10.3.0.2 -alias", out, sizeof(out), NULL) == 0);
	CHECK(eth1.nalias == 1 && !rt_ip_is_local(A(10, 3, 0, 2)) && has_route(A(10, 2, 0, 9), 0, &eth1));
	CHECK(rt_config_line("ifconfig eth1 10.3.0.2 -alias", out, sizeof(out), NULL) < 0);
	/* deleting the primary promotes the next one */
	CHECK(rt_netif_addr_del(&eth1, A(10, 1, 0, 2)) == 0);
	CHECK(eth1.addr == A(10, 2, 0, 2) && eth1.nalias == 0 && has_route(A(10, 2, 0, 9), 0, &eth1));
	CHECK(rt_config_line("ifconfig eth1 alias 10.1.0.2/24", out, sizeof(out), NULL) == 0 && eth1.nalias == 1);
	/* "ifconfig IF delete": all of them */
	CHECK(rt_config_line("ifconfig eth1 delete", out, sizeof(out), NULL) == 0);
	CHECK(eth1.addr == 0 && eth1.nalias == 0 && !has_route(A(10, 2, 0, 9), 0, &eth1));
	/* only an additional address: still the source */
	CHECK(rt_netif_addr_add(&eth1, A(10, 6, 0, 2), A(255, 255, 255, 0), 0) == 0 && eth1.addr == A(10, 6, 0, 2));

	/* ipv4 off: addresses go, no new ones, no input */
	CHECK(rt_config_line("ifconfig eth1 alias 10.7.0.2/24", out, sizeof(out), NULL) == 0);
	CHECK(rt_config_line("ipv4 eth1 off", out, sizeof(out), NULL) == 0);
	CHECK(eth1.ip4_disabled && eth1.addr == 0 && eth1.nalias == 0 && !has_route(A(10, 6, 0, 9), 0, &eth1));
	CHECK(rt_netif_addr_add(&eth1, A(10, 6, 0, 2), A(255, 255, 255, 0), 0) == -RT_EAFNOSUPPORT);
	CHECK(rt_dhcp_start(&eth1, NULL) == -RT_EAFNOSUPPORT);
	CHECK(rt_config_line("ipv4 eth1 on", out, sizeof(out), NULL) == 0 && !eth1.ip4_disabled);
	CHECK(rt_netif_addr_add(&eth1, A(10, 6, 0, 2), A(255, 255, 255, 0), 0) == 0);
	CHECK(rt_config_line("ipv4 bogus", out, sizeof(out), NULL) < 0);
	CHECK(rt_config_line("ipv4 nope0 off", out, sizeof(out), NULL) < 0);
	drop_held();
	rt_unlock();
}

int main(void)
{
	CHECK(rt_init() == 0);
	rt_log_level = 0;
	test_bands();
	test_queue();
	test_priority_rules();
	test_addresses();
	printf("t_qos_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
