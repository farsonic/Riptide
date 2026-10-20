/*
 * Riptide - DHCP client against a scripted DHCP server on the simulated
 * Ethernet wire (no privileges needed; TUN is L3 only, so the wire is the
 * practical way to test broadcasts before the interface has an address).
 *
 * The server thread owns the far end of the wire (an AF_UNIX datagram
 * socket) and speaks raw Ethernet/ARP/IP/UDP/BOOTP. Scenario:
 *   1. first DISCOVER ignored -> retransmission after 4 +- 1 s
 *   2. OFFER / REQUEST / ACK -> BOUND: address, mask, default route,
 *      DNS server, domain, MTU; option 12 carries rt_hostname
 *   3. T1 (2 s): unicast REQUEST (needs ARP) -> ACK -> RENEWED
 *   4. next T1: NAK -> LOST, address removed, rediscovery -> BOUND
 *   5. server silent: lease (8 s) expires -> LOST
 *   6. rebind and rt_dhcp_stop() sends RELEASE
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
#define DNS_IP   0xc0a85835u
#define LEASE    8
static const u8 cli_mac[6] = { 0x02, 0, 0, 0, 0x11, 0x22 };
static const u8 srv_mac[6] = { 0x02, 0, 0, 0, 0x33, 0x44 };

static char path_a[108], path_b[108];
static int wfd;
static struct rt_netif *nif;

/* server behaviour, set by the main thread */
static volatile int mode;            /* 0 normal, 1 NAK requests, 2 silent */
static volatile int ignore_discovers = 1;
static volatile int n_discover, n_request, n_request_unicast, n_release, n_arp;
static volatile u32 t_discover[4];
static volatile int saw_hostname, saw_bcast_flag;
static volatile int stop_server;

/* events from the DHCP callback (stack thread) */
static volatile int ev_bound, ev_renewed, ev_lost;
static void dhcp_cb(struct rt_netif *n, int ev)
{
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
	u32 len, v;
	u16 ck;
	static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	memset(pkt, 0, sizeof(pkt));
	m[0] = 2; m[1] = 1; m[2] = 6;
	rt_put_be32(m + 4, xid);
	rt_put_be32(m + 16, yiaddr);
	rt_put_be32(m + 20, SRV_IP);
	memcpy(m + 28, chaddr, 6);
	rt_put_be32(m + 236, 0x63825363u);
	o = m + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	*o++ = 54; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
	if (type != 6) {
		*o++ = 1; *o++ = 4; rt_put_be32(o, 0xffffff00u); o += 4;
		*o++ = 3; *o++ = 8; rt_put_be32(o, SRV_IP); rt_put_be32(o + 4, 0xc0a858feu); o += 8;
		*o++ = 6; *o++ = 4; rt_put_be32(o, DNS_IP); o += 4;
		*o++ = 15; *o++ = 8; memcpy(o, "dhcp.lan", 8); o += 8;
		*o++ = 26; *o++ = 2; rt_put_be16(o, 1400); o += 2;
		*o++ = 51; *o++ = 4; rt_put_be32(o, LEASE); o += 4;
		*o++ = 58; *o++ = 4; rt_put_be32(o, 2); o += 4;
		*o++ = 59; *o++ = 4; rt_put_be32(o, 5); o += 4;
		*o++ = 0; *o++ = 0;                       /* pads are skipped */
	}
	*o++ = 255;
	len = (u32)(o - m);
	if (len < 300)
		len = 300;
	/* UDP 67 -> 68, checksum 0 (none) */
	rt_put_be16(udp, 67); rt_put_be16(udp + 2, 68); rt_put_be16(udp + 4, (u16)(8 + len));
	ip[0] = 0x45; rt_put_be16(ip + 2, (u16)(28 + len)); ip[8] = 64; ip[9] = 17;
	rt_put_be32(ip + 12, SRV_IP);
	v = 0xffffffffu;
	memcpy(ip + 16, &v, 4);
	ck = rt_inet_csum(ip, 20);
	memcpy(ip + 10, &ck, 2);
	wire_send(bc, 0x0800, pkt, 28 + len);
}

static void handle_arp(const u8 *p, u32 len)
{
	u8 r[28];
	if (len < 28 || rt_get_be16(p + 6) != 1 || rt_get_be32(p + 24) != SRV_IP)
		return;
	n_arp++;
	memcpy(r, p, 8);
	rt_put_be16(r + 6, 2);
	memcpy(r + 8, srv_mac, 6);
	rt_put_be32(r + 14, SRV_IP);
	memcpy(r + 18, p + 8, 10);
	wire_send(p + 8, 0x0806, r, 28);
}

static void handle_dhcp(const u8 *ip, u32 len)
{
	const u8 *m, *o, *end;
	u8 type = 0;
	u32 xid, ciaddr, hl = (u32)(ip[0] & 15) * 4;
	int unicast = rt_get_be32(ip + 16) == SRV_IP;

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
		if (o[0] == 12 && o[1] == 5 && !memcmp(o + 2, "amiga", 5))
			saw_hostname = 1;
		o += 2 + o[1];
	}
	if (type == 1) {
		if (n_discover < 4)
			t_discover[n_discover] = rt_now_ms();
		n_discover++;
		if (rt_get_be16(m + 10) & 0x8000)
			saw_bcast_flag = 1;
		if (ignore_discovers > 0) {
			ignore_discovers--;
			return;
		}
		if (mode != 2)
			send_reply(2, xid, m + 28, CLI_IP);
	} else if (type == 3) {
		n_request++;
		if (unicast)
			n_request_unicast++;
		if (mode == 2)
			return;
		if (mode == 1) {
			send_reply(6, xid, m + 28, 0);
			mode = 0;
		} else {
			send_reply(5, xid, m + 28, ciaddr ? ciaddr : CLI_IP);
		}
	} else if (type == 7) {
		n_release++;
	}
}

static void *server_main(void *arg)
{
	u8 f[2048];
	while (!stop_server) {
		struct pollfd p = { wfd, POLLIN, 0 };
		ssize_t n;
		if (poll(&p, 1, 100) <= 0)
			continue;
		n = recv(wfd, f, sizeof(f), 0);
		if (n < 14)
			continue;
		if (rt_get_be16(f + 12) == 0x0806)
			handle_arp(f + 14, (u32)n - 14);
		else if (rt_get_be16(f + 12) == 0x0800)
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

static int has_default_route(u32 gw)
{
	struct rt_route *r;
	int found = 0;
	rt_lock();
	for (r = rt_route_list(); r; r = r->next)
		if (r->dst == 0 && r->mask == 0 && r->gateway == rt_htonl(gw))
			found = 1;
	rt_unlock();
	return found;
}

int main(void)
{
	struct sockaddr_un a;
	pthread_t th;
	u32 dns[4], t0;
	int n;

	snprintf(path_a, sizeof(path_a), "/tmp/rt_dhcp_a.%d", (int)getpid());
	snprintf(path_b, sizeof(path_b), "/tmp/rt_dhcp_b.%d", (int)getpid());
	unlink(path_a);
	unlink(path_b);
	wfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path_b);
	if (wfd < 0 || bind(wfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("t_dhcp_wire: SKIP (no AF_UNIX sockets)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	nif = rt_host_wire_open(path_a, path_b, cli_mac);
	CHECK(nif != NULL);
	if (!nif)
		return 1;
	pthread_create(&th, NULL, server_main, NULL);

	rt_lock();
	rt_strlcpy(rt_hostname, "amiga.home", sizeof(rt_hostname));
	rt_domainname[0] = 0;
	rt_dns_clear_servers();
	CHECK(!rt_dhcp_bound(nif));
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);          /* idempotent */
	rt_unlock();

	/* 1+2: retransmitted DISCOVER, then bound */
	CHECK(wait_for(&ev_bound, 1, 9000));
	CHECK(n_discover >= 2);
	n = (int)(t_discover[1] - t_discover[0]);
	CHECK(n >= 2900 && n <= 5200);
	CHECK(saw_hostname && saw_bcast_flag);
	rt_lock();
	CHECK(rt_dhcp_bound(nif));
	CHECK(nif->addr == rt_htonl(CLI_IP) && nif->netmask == rt_htonl(0xffffff00u));
	CHECK(nif->broadaddr == rt_htonl(0xc0a858ffu));
	CHECK(nif->mtu == 1400);
	CHECK(rt_dns_get_servers(dns, 4) == 1 && dns[0] == rt_htonl(DNS_IP));
	CHECK(!strcmp(rt_domainname, "dhcp.lan"));
	rt_unlock();
	CHECK(has_default_route(SRV_IP));

	/* 3: renew at T1 via unicast */
	t0 = rt_now_ms();
	CHECK(wait_for(&ev_renewed, 1, 5000));
	CHECK(rt_now_ms() - t0 >= 1000);
	CHECK(n_request_unicast >= 1 && n_arp >= 1);
	CHECK(nif->addr == rt_htonl(CLI_IP));

	/* 4: NAK at the next renewal -> lost, then bound again */
	mode = 1;
	CHECK(wait_for(&ev_lost, 1, 6000));
	CHECK(wait_for(&ev_bound, 2, 8000));
	CHECK(nif->addr == rt_htonl(CLI_IP) && has_default_route(SRV_IP));

	/* 5: silent server: the lease runs out (renew + rebind unanswered) */
	mode = 2;
	t0 = rt_now_ms();
	CHECK(wait_for(&ev_lost, 2, 12000));
	n = (int)(rt_now_ms() - t0);
	CHECK(n >= 4000);
	rt_lock();
	CHECK(!rt_dhcp_bound(nif) && nif->addr == 0);
	CHECK(rt_dns_get_servers(dns, 4) == 0);
	rt_unlock();
	CHECK(!has_default_route(SRV_IP));

	/* 6: server back; restart (instead of waiting for the 64 s backoff);
	 * stop releases */
	mode = 0;
	rt_lock();
	rt_dhcp_stop(nif);
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	rt_unlock();
	CHECK(wait_for(&ev_bound, 3, 5000));
	rt_lock();
	rt_dhcp_stop(nif);
	CHECK(!rt_dhcp_bound(nif) && nif->addr == 0);
	rt_unlock();
	CHECK(wait_for(&n_release, 1, 2000));

	stop_server = 1;
	pthread_join(th, NULL);
	rt_host_stop();
	unlink(path_a);
	unlink(path_b);
	printf("t_dhcp_wire: %d checks, %d failures (discover %d request %d)\n",
	       checks, failures, n_discover, n_request);
	return failures ? 1 : 0;
}
