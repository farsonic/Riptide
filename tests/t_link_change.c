/*
 * Riptide - link state changes (rt_netif_link_change) on the simulated
 * Ethernet wire, with a scripted DHCP server:
 *   1. DHCP binds on eth A; a second interface B has a static default
 *      route of a worse metric
 *   2. A's link goes down: not RUNNING, the default route through B wins,
 *      rt_netif_up() keeps it not RUNNING
 *   3. A's link comes back: RUNNING, a broadcast DHCPREQUEST from 0.0.0.0
 *      with option 50 (INIT-REBOOT) confirms the lease at once
 *   4. down and up again, the server NAKs: the lease is lost and
 *      discovery binds again
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SRV_IP   0xc0a85801u   /* 192.168.88.1 */
#define CLI_IP   0xc0a85864u   /* 192.168.88.100 */
#define B_IP     0x0a000002u   /* 10.0.0.2 on interface B */
#define B_GW     0x0a000001u
static const u8 cli_mac[6] = { 0x02, 0, 0, 0, 0x11, 0x22 };
static const u8 srv_mac[6] = { 0x02, 0, 0, 0, 0x33, 0x44 };
static const u8 b_mac[6] = { 0x02, 0, 0, 0, 0x55, 0x66 };

static char path_a[108], path_b[108], path_c[108], path_d[108];
static int wfd;

static volatile int nak_next, stop_server;
static volatile int n_reboot_req;          /* REQUEST, ciaddr 0, option 50, no option 54 */
static volatile u32 t_reboot_req;
static volatile int ev_bound, ev_renewed, ev_lost;

static void dhcp_cb(struct rt_netif *n, int ev)
{
	(void)n;
	if (ev == RT_DHCP_EV_BOUND) ev_bound++;
	if (ev == RT_DHCP_EV_RENEWED) ev_renewed++;
	if (ev == RT_DHCP_EV_LOST) ev_lost++;
}

static void wire_send(const u8 *dst, u16 type, const u8 *payload, u32 len)
{
	u8 f[1600];
	struct sockaddr_un a;

	memcpy(f, dst, 6);
	memcpy(f + 6, srv_mac, 6);
	rt_put_be16(f + 12, type);
	memcpy(f + 14, payload, len);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path_a);
	sendto(wfd, f, 14 + len, 0, (struct sockaddr *)&a, sizeof(a));
}

static void send_reply(u8 type, u32 xid, const u8 *chaddr, u32 yiaddr)
{
	u8 pkt[600], *ip = pkt, *udp = pkt + 20, *m = pkt + 28, *o;
	static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u32 len, v;
	u16 ck;

	memset(pkt, 0, sizeof(pkt));
	m[0] = 2; m[1] = 1; m[2] = 6;
	rt_put_be32(m + 4, xid);
	rt_put_be32(m + 16, yiaddr);
	memcpy(m + 28, chaddr, 6);
	rt_put_be32(m + 236, 0x63825363u);
	o = m + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	*o++ = 54; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
	if (type != 6) {
		*o++ = 1; *o++ = 4; rt_put_be32(o, 0xffffff00u); o += 4;
		*o++ = 3; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
		*o++ = 51; *o++ = 4; rt_put_be32(o, 600); o += 4;
	}
	*o++ = 255;
	len = (u32)(o - m);
	if (len < 300)
		len = 300;
	rt_put_be16(udp, 67); rt_put_be16(udp + 2, 68); rt_put_be16(udp + 4, (u16)(8 + len));
	ip[0] = 0x45; rt_put_be16(ip + 2, (u16)(28 + len)); ip[8] = 64; ip[9] = 17;
	rt_put_be32(ip + 12, SRV_IP);
	v = 0xffffffffu;
	memcpy(ip + 16, &v, 4);
	ck = rt_inet_csum(ip, 20);
	memcpy(ip + 10, &ck, 2);
	wire_send(bc, 0x0800, pkt, 28 + len);
}

static void handle_dhcp(const u8 *ip, u32 len)
{
	const u8 *m, *o, *end;
	u8 type = 0;
	u32 xid, ciaddr, reqip = 0, hl = (u32)(ip[0] & 15) * 4;
	int has_sid = 0;

	if (len < hl + 8 + 240 || ip[9] != 17 || rt_get_be16(ip + hl + 2) != 67)
		return;
	m = ip + hl + 8;
	end = ip + len;
	if (m[0] != 1 || rt_get_be32(m + 236) != 0x63825363u || memcmp(m + 28, cli_mac, 6))
		return;
	xid = rt_get_be32(m + 4);
	ciaddr = rt_get_be32(m + 12);
	for (o = m + 240; o + 2 <= end && *o != 255;) {
		if (*o == 0) { o++; continue; }
		if (o[0] == 53)
			type = o[2];
		if (o[0] == 50 && o[1] == 4)
			reqip = rt_get_be32(o + 2);
		if (o[0] == 54)
			has_sid = 1;
		o += 2 + o[1];
	}
	if (type == 1) {
		send_reply(2, xid, m + 28, CLI_IP);
	} else if (type == 3) {
		if (!ciaddr && reqip == CLI_IP && !has_sid && rt_get_be32(ip + 12) == 0 &&
		    rt_get_be32(ip + 16) == 0xffffffffu) {
			n_reboot_req++;
			t_reboot_req = rt_now_ms();
		}
		if (nak_next && !has_sid) {
			nak_next = 0;
			send_reply(6, xid, m + 28, 0);
			return;
		}
		send_reply(5, xid, m + 28, CLI_IP);
	}
}

static void *server_main(void *arg)
{
	u8 f[2048];

	(void)arg;
	while (!stop_server) {
		struct pollfd p = { wfd, POLLIN, 0 };
		ssize_t n;
		if (poll(&p, 1, 50) <= 0)
			continue;
		n = recv(wfd, f, sizeof(f), 0);
		if (n >= 14 && rt_get_be16(f + 12) == 0x0800)
			handle_dhcp(f + 14, (u32)n - 14);
	}
	return NULL;
}

static int wait_for(volatile int *v, int val, int ms)
{
	u32 t0 = rt_now_ms();
	while (*v < val && (int)(rt_now_ms() - t0) < ms)
		usleep(20000);
	return *v >= val;
}

static struct rt_netif *route_if(u32 dst)
{
	struct rt_nexthop nh;

	if (rt_ip_route(rt_htonl(dst), &nh) < 0)
		return NULL;
	return nh.netif;
}

int main(void)
{
	struct sockaddr_un a;
	struct rt_netif *na, *nb;
	pthread_t th;
	char out[256];
	u32 t0;
	int n;

	snprintf(path_a, sizeof(path_a), "/tmp/rt_link_a.%d", (int)getpid());
	snprintf(path_b, sizeof(path_b), "/tmp/rt_link_b.%d", (int)getpid());
	snprintf(path_c, sizeof(path_c), "/tmp/rt_link_c.%d", (int)getpid());
	snprintf(path_d, sizeof(path_d), "/tmp/rt_link_d.%d", (int)getpid());
	unlink(path_a);
	unlink(path_b);
	unlink(path_c);
	wfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path_b);
	if (wfd < 0 || bind(wfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("t_link_change: SKIP (no AF_UNIX sockets)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	na = rt_host_wire_open(path_a, path_b, cli_mac);
	nb = rt_host_wire_open(path_c, path_d, b_mac);     /* nobody on its wire */
	CHECK(na && nb);
	if (!na || !nb)
		return 1;
	pthread_create(&th, NULL, server_main, NULL);

	/* 1 */
	rt_lock();
	rt_netif_set_addr(nb, rt_htonl(B_IP), rt_htonl(0xffffff00u), 0);
	rt_netif_up(nb);
	CHECK(rt_ctl_command("route add default via 10.0.0.1 metric 50", out, sizeof(out), NULL) == 0);
	CHECK(rt_dhcp_start(na, dhcp_cb) == 0);
	rt_unlock();
	CHECK(wait_for(&ev_bound, 1, 6000));
	rt_lock();
	CHECK(na->addr == rt_htonl(CLI_IP));
	CHECK(route_if(0x08080808u) == na);              /* DHCP route, metric 0 */
	rt_unlock();

	/* 2 */
	rt_lock();
	rt_netif_link_change(na, 0);
	CHECK((na->flags & RT_IFF_UP) && !(na->flags & RT_IFF_RUNNING) && na->link_down);
	CHECK(route_if(0x08080808u) == nb);
	CHECK(route_if(SRV_IP) == nb);                   /* even its own subnet */
	rt_netif_up(na);
	CHECK(!(na->flags & RT_IFF_RUNNING));
	rt_netif_link_change(na, 0);                     /* no change: nothing happens */
	/* B down too: A's routes are better than none */
	rt_netif_link_change(nb, 0);
	CHECK(route_if(0x08080808u) == na);
	rt_netif_link_change(nb, 1);
	CHECK(route_if(0x08080808u) == nb);
	rt_unlock();

	/* 3 */
	n = ev_renewed;
	t0 = rt_now_ms();
	rt_lock();
	rt_netif_link_change(na, 1);
	CHECK((na->flags & RT_IFF_RUNNING) && !na->link_down);
	CHECK(route_if(0x08080808u) == na);
	rt_unlock();
	CHECK(wait_for(&ev_renewed, n + 1, 3000));
	CHECK(n_reboot_req == 1 && t_reboot_req - t0 < 1000);
	rt_lock();
	CHECK(rt_dhcp_bound(na) && na->addr == rt_htonl(CLI_IP));
	rt_unlock();

	/* 4 */
	nak_next = 1;
	n = ev_bound;
	rt_lock();
	rt_netif_link_change(na, 0);
	rt_netif_link_change(na, 1);
	rt_unlock();
	CHECK(wait_for(&ev_lost, 1, 3000));
	CHECK(wait_for(&ev_bound, n + 1, 6000));
	CHECK(n_reboot_req == 2);
	rt_lock();
	CHECK(rt_dhcp_bound(na) && na->addr == rt_htonl(CLI_IP));
	rt_dhcp_stop(na);
	rt_unlock();

	stop_server = 1;
	pthread_join(th, NULL);
	rt_host_stop();
	unlink(path_a);
	unlink(path_b);
	unlink(path_c);
	printf("t_link_change: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
