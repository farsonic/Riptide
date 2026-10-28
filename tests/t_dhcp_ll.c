/*
 * Riptide - DHCP options 121/249 (classless static routes, RFC 3442) and
 * 119 (domain search, RFC 3397), and IPv4 link-local addresses (RFC 3927).
 *
 *   1. option decoding, including malformed input
 *   2. a scripted server on the simulated wire (as t_dhcp_wire.c): 121
 *      split over two instances (RFC 3396) with option 3 ignored, 119 with
 *      compression; a renewal with other routes; option 3 again; stop
 *   3. no server: probing, conflicts while probing (an ARP reply, another
 *      host probing), announcements, defence and a second conflict,
 *      "dhcp IF linklocal off|on", a lease replacing the address
 *   4. static addresses: no link-local probing next to a routable one,
 *      "ifconfig IF 169.254.x.y/16" by hand
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
#define GW2      0xc0a85802u   /* 192.168.88.2 */
#define GW254    0xc0a858feu
static const u8 cli_mac[6] = { 0x02, 0, 0, 0, 0x11, 0x23 };
static const u8 srv_mac[6] = { 0x02, 0, 0, 0, 0x33, 0x45 };
static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static char path_a[108], path_b[108];
static int wfd;
static struct rt_netif *nif;

static volatile int silent = 1;       /* DHCP server off */
static volatile int optset = 1;       /* 1: 121+119, 2: other 121, 3: option 3 only */
static volatile int stop_server;
static volatile int n_probe, n_announce;
static volatile u32 last_probe, last_announce;
static volatile u32 probe_ms[64];
/* link-local candidates in the order they were first probed, and what
 * the "other host" does about each: 0 nothing, 1 answers (uses it),
 * 2 probes for it too */
static volatile u32 cand[16];
static volatile int ncand;
static volatile int cand_action[16];

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

/* ARP from the "other host" (srv_mac); addresses host order */
static void send_arp(u16 op, u32 sip, u32 tip, const u8 *dst)
{
	u8 r[28];

	rt_put_be16(r, 1);
	rt_put_be16(r + 2, 0x0800);
	r[4] = 6;
	r[5] = 4;
	rt_put_be16(r + 6, op);
	memcpy(r + 8, srv_mac, 6);
	rt_put_be32(r + 14, sip);
	memset(r + 18, 0, 6);
	if (op == 2)
		memcpy(r + 18, dst, 6);
	rt_put_be32(r + 24, tip);
	wire_send(dst, 0x0806, r, 28);
}

static u8 *put_opts(u8 *o)
{
	if (optset == 1) {
		/* 10/8 via .2, 172.16.5/24 on-link, default via .254 (option
		 * 3 says .1: ignored), 198.51.100.7/32 via .3; split in two
		 * instances in the middle of an entry */
		static const u8 cls[] = { 8, 10, 192, 168, 88, 2,
		                          24, 172, 16, 5, 0, 0, 0, 0,
		                          0, 192, 168, 88, 254,
		                          32, 198, 51, 100, 7, 192, 168, 88, 3 };
		/* lan.example, corp.example (-> "example"), lan.example again
		 * (one pointer), eng.corp.example (pointer to a pointer) */
		static const u8 srch[] = { 3, 'l', 'a', 'n', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0,
		                           4, 'c', 'o', 'r', 'p', 0xc0, 4,
		                           0xc0, 0,
		                           3, 'e', 'n', 'g', 0xc0, 13 };
		*o++ = 121; *o++ = 9; memcpy(o, cls, 9); o += 9;
		*o++ = 3; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
		*o++ = 121; *o++ = sizeof(cls) - 9; memcpy(o, cls + 9, sizeof(cls) - 9); o += sizeof(cls) - 9;
		*o++ = 119; *o++ = sizeof(srch); memcpy(o, srch, sizeof(srch)); o += sizeof(srch);
	} else if (optset == 2) {
		/* 10/8 and the host route gone, 192.0.2/24 via .2 new; Microsoft's
		 * 249 is ignored next to 121 */
		static const u8 cls[] = { 24, 172, 16, 5, 0, 0, 0, 0,
		                          0, 192, 168, 88, 254,
		                          24, 192, 0, 2, 192, 168, 88, 2 };
		static const u8 ms[] = { 8, 11, 192, 168, 88, 2 };
		*o++ = 249; *o++ = sizeof(ms); memcpy(o, ms, sizeof(ms)); o += sizeof(ms);
		*o++ = 121; *o++ = sizeof(cls); memcpy(o, cls, sizeof(cls)); o += sizeof(cls);
		*o++ = 3; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
	} else {
		*o++ = 3; *o++ = 4; rt_put_be32(o, SRV_IP); o += 4;
	}
	*o++ = 15; *o++ = 8; memcpy(o, "dhcp.lan", 8); o += 8;
	return o;
}

static void send_reply(u8 type, u32 xid, const u8 *chaddr, u32 yiaddr)
{
	u8 pkt[600], *ip = pkt, *udp = pkt + 20, *m = pkt + 28, *o;
	u32 len, v;
	u16 ck;

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
	*o++ = 1; *o++ = 4; rt_put_be32(o, 0xffffff00u); o += 4;
	*o++ = 51; *o++ = 4; rt_put_be32(o, 3600); o += 4;
	o = put_opts(o);
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

static void handle_arp(const u8 *p, u32 len)
{
	u32 sip, tip;
	int k;

	if (len < 28 || rt_get_be16(p + 6) != 1)
		return;
	sip = rt_get_be32(p + 14);
	tip = rt_get_be32(p + 24);
	if (tip == SRV_IP) {
		send_arp(2, SRV_IP, sip, p + 8);
		return;
	}
	if (memcmp(p + 8, cli_mac, 6))
		return;
	if (sip == 0) {
		if (n_probe < 64)
			probe_ms[n_probe] = rt_now_ms();
		last_probe = tip;
		n_probe++;
		for (k = 0; k < ncand; k++)
			if (cand[k] == tip)
				break;
		if (k == ncand && ncand < 16) {
			cand[ncand++] = tip;
			if (cand_action[k] == 1)
				send_arp(2, tip, 0, p + 8);         /* "it's mine" */
			else if (cand_action[k] == 2)
				send_arp(1, 0, tip, bc);            /* probing too */
		}
	} else if (sip == tip) {
		last_announce = sip;
		n_announce++;
	}
}

static void handle_dhcp(const u8 *ip, u32 len)
{
	const u8 *m, *o, *end;
	u8 type = 0;
	u32 xid, ciaddr, hl = (u32)(ip[0] & 15) * 4;

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
		o += 2 + o[1];
	}
	if (silent)
		return;
	if (type == 1)
		send_reply(2, xid, m + 28, CLI_IP);
	else if (type == 3)
		send_reply(5, xid, m + 28, ciaddr ? ciaddr : CLI_IP);
}

static void *server_main(void *arg)
{
	u8 f[2048];

	(void)arg;
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

static int wait_addr(u32 pred_ll, int ms)
{
	u32 t0 = rt_now_ms(), a;
	for (;;) {
		rt_lock();
		a = nif->addr;
		rt_unlock();
		if (pred_ll ? (a && rt_ipv4ll_is_ll(a) && a != pred_ll) : a == 0)
			return 1;
		if ((int)(rt_now_ms() - t0) >= ms)
			return 0;
		usleep(20000);
	}
}

/* route lookups with the lock held; host order arguments */
static struct rt_route *route(u32 dst, u32 bits, u32 gw)
{
	u32 mask = bits ? 0xffffffffu << (32 - bits) : 0;
	struct rt_route *r;

	for (r = rt_route_list(); r; r = r->next)
		if (r->dst == rt_htonl(dst) && r->mask == rt_htonl(mask) && r->gateway == rt_htonl(gw))
			return r;
	return NULL;
}

static int n_default_routes(void)
{
	struct rt_route *r;
	int n = 0;

	for (r = rt_route_list(); r; r = r->next)
		if (r->mask == 0)
			n++;
	return n;
}

/* ---------------------------------------------------------------- 1 */

static void test_parse(void)
{
	struct rt_dhcp_route r[RT_DHCP_MAX_ROUTES];
	char s[RT_DHCP_SEARCH_LEN];
	u8 big[200];
	int i;

	{
		static const u8 a[] = { 24, 192, 168, 1, 10, 0, 0, 1,  0, 10, 0, 0, 254,
		                        32, 1, 2, 3, 4, 5, 6, 7, 8,  12, 10, 0xff, 0, 0, 0, 0 };
		CHECK(rt_dhcp_parse_classless(a, sizeof(a), r, 16) == 4);
		CHECK(r[0].dst == rt_htonl(0xc0a80100u) && r[0].mask == rt_htonl(0xffffff00u) &&
		      r[0].gw == rt_htonl(0x0a000001u));
		CHECK(r[1].dst == 0 && r[1].mask == 0 && r[1].gw == rt_htonl(0x0a0000feu));
		CHECK(r[2].dst == rt_htonl(0x01020304u) && r[2].mask == 0xffffffffu &&
		      r[2].gw == rt_htonl(0x05060708u));
		/* host bits beyond the width are dropped */
		CHECK(r[3].dst == rt_htonl(0x0af00000u) && r[3].mask == rt_htonl(0xfff00000u) && r[3].gw == 0);
	}
	{
		static const u8 w33[] = { 33, 1, 2, 3, 4, 5, 1, 2, 3, 4 };
		static const u8 shortd[] = { 24, 192, 168, 1, 10, 0, 0 };
		static const u8 tail[] = { 0, 10, 0, 0, 1, 8 };
		static const u8 tiny[] = { 0, 1, 2, 3 };
		CHECK(rt_dhcp_parse_classless(w33, sizeof(w33), r, 16) == -1);
		CHECK(rt_dhcp_parse_classless(shortd, sizeof(shortd), r, 16) == -1);
		CHECK(rt_dhcp_parse_classless(tail, sizeof(tail), r, 16) == -1);
		CHECK(rt_dhcp_parse_classless(tiny, sizeof(tiny), r, 16) == -1);
		CHECK(rt_dhcp_parse_classless(tiny, 0, r, 16) == -1);
	}
	for (i = 0; i < 20; i++) {            /* 20 routes: 16 kept */
		u8 *p = big + i * 7;
		p[0] = 16; p[1] = 10; p[2] = (u8)i; p[3] = 10; p[4] = 0; p[5] = 0; p[6] = 1;
	}
	CHECK(rt_dhcp_parse_classless(big, 140, r, RT_DHCP_MAX_ROUTES) == RT_DHCP_MAX_ROUTES);
	CHECK(r[15].dst == rt_htonl(0x0a0f0000u));

	{
		static const u8 a[] = { 3, 'l', 'a', 'n', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0,
		                        4, 'c', 'o', 'r', 'p', 0xc0, 4,  3, 'e', 'n', 'g', 0xc0, 13 };
		CHECK(rt_dhcp_parse_search(a, sizeof(a), s, sizeof(s)) == 3);
		CHECK(!strcmp(s, "lan.example corp.example eng.corp.example"));
		CHECK(rt_dhcp_parse_search(a, 13, s, sizeof(s)) == 1 && !strcmp(s, "lan.example"));
		/* missing terminator, label past the end, pointer cut off */
		CHECK(rt_dhcp_parse_search(a, 12, s, sizeof(s)) == -1 && !s[0]);
		CHECK(rt_dhcp_parse_search(a, 8, s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(a, 19, s, sizeof(s)) == -1);
	}
	{
		static const u8 self[] = { 3, 'a', 'b', 'c', 0xc0, 0 };       /* loops to itself */
		static const u8 fwd[] = { 0xc0, 2, 1, 'x', 0 };              /* forward pointer */
		static const u8 out[] = { 1, 'x', 0, 0xc0, 50 };             /* outside the option */
		static const u8 ext[] = { 0x41, 'x', 0 };                    /* extended label type */
		static const u8 sp[] = { 3, 'a', ' ', 'b', 0 };              /* no spaces */
		static const u8 cm[] = { 3, 'a', ',', 'b', 0 };
		static const u8 ring[] = { 1, 'a', 0, 1, 'b', 0xc0, 3 };     /* b -> b */
		CHECK(rt_dhcp_parse_search(self, sizeof(self), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(fwd, sizeof(fwd), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(out, sizeof(out), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(ext, sizeof(ext), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(sp, sizeof(sp), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(cm, sizeof(cm), s, sizeof(s)) == -1);
		CHECK(rt_dhcp_parse_search(ring, sizeof(ring), s, sizeof(s)) == -1);
	}
	{
		/* eight names: six kept; a 70-character name skipped; the root
		 * name ignored */
		u8 b[300], *p = b;
		int k;
		*p++ = 0;
		for (k = 0; k < 8; k++) {
			*p++ = 2; *p++ = 'd'; *p++ = (u8)('0' + k); *p++ = 0;
		}
		CHECK(rt_dhcp_parse_search(b, (u32)(p - b), s, sizeof(s)) == 6);
		CHECK(!strcmp(s, "d0 d1 d2 d3 d4 d5"));
		p = b;
		*p++ = 63; memset(p, 'x', 63); p += 63; *p++ = 6; memset(p, 'y', 6); p += 6; *p++ = 0;
		*p++ = 2; *p++ = 'o'; *p++ = 'k'; *p++ = 0;
		CHECK(rt_dhcp_parse_search(b, (u32)(p - b), s, sizeof(s)) == 1 && !strcmp(s, "ok"));
		CHECK(rt_dhcp_parse_search(b, 0, s, sizeof(s)) == 0 && !s[0]);
	}
}

/* ---------------------------------------------------------------- 2 */

static void test_options(void)
{
	struct rt_route *r;
	struct rt_dhcp_info di;
	int renewed;

	silent = 0;
	optset = 1;
	rt_lock();
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	rt_unlock();
	CHECK(wait_for(&ev_bound, 1, 9000));
	rt_lock();
	CHECK(nif->addr == rt_htonl(CLI_IP));
	r = route(0x0a000000u, 8, GW2);
	CHECK(r && (r->flags & RT_RTF_DHCP) && r->netif == nif);
	r = route(0xac100500u, 24, 0);
	CHECK(r && (r->flags & RT_RTF_DHCP) && !(r->flags & RT_RTF_GATEWAY));
	CHECK(route(0xc6336407u, 32, 0xc0a85803u) != NULL);
	r = route(0, 0, GW254);
	CHECK(r && (r->flags & RT_RTF_DHCP));
	CHECK(!route(0, 0, SRV_IP) && n_default_routes() == 1);        /* option 3 ignored */
	CHECK(rt_dhcp_get_info(nif, &di) == RT_DHCPS_BOUND && di.router == rt_htonl(GW254) &&
	      di.nroutes == 4 && di.ll_addr == 0 && di.ll_state == RT_LL_OFF);
	CHECK(!strcmp(rt_dns_search_list(), "lan.example corp.example eng.corp.example"));
	CHECK(!strcmp(rt_domainname, "dhcp.lan"));
	renewed = ev_renewed;
	optset = 2;
	CHECK(rt_dhcp_renew(nif) == 0);
	rt_unlock();

	/* renewal: changed routes replaced, the rest kept; no 119: option 15 */
	CHECK(wait_for(&ev_renewed, renewed + 1, 3000));
	rt_lock();
	CHECK(!route(0x0a000000u, 8, GW2) && !route(0xc6336407u, 32, 0xc0a85803u));
	CHECK(route(0xc0000200u, 24, GW2) && route(0xac100500u, 24, 0));
	CHECK(!route(0x0b000000u, 8, GW2));                              /* 249 ignored */
	CHECK(route(0, 0, GW254) && n_default_routes() == 1);
	CHECK(rt_dhcp_get_info(nif, &di) >= RT_DHCPS_BOUND && di.nroutes == 3);
	CHECK(!strcmp(rt_dns_search_list(), "dhcp.lan"));
	renewed = ev_renewed + ev_bound;
	optset = 3;
	CHECK(rt_dhcp_renew(nif) == 0);
	rt_unlock();

	/* no 121 any more: option 3, classless routes gone */
	{
		u32 t0 = rt_now_ms();
		while (ev_renewed + ev_bound <= renewed && rt_now_ms() - t0 < 3000)
			usleep(20000);
	}
	rt_lock();
	CHECK(route(0, 0, SRV_IP) && !route(0, 0, GW254) && n_default_routes() == 1);
	CHECK(!route(0xc0000200u, 24, GW2) && !route(0xac100500u, 24, 0));
	CHECK(rt_dhcp_get_info(nif, &di) >= RT_DHCPS_BOUND && di.nroutes == 0);
	optset = 1;
	renewed = ev_renewed + ev_bound;
	CHECK(rt_dhcp_renew(nif) == 0);
	rt_unlock();
	{
		u32 t0 = rt_now_ms();
		while (ev_renewed + ev_bound <= renewed && rt_now_ms() - t0 < 3000)
			usleep(20000);
	}

	/* a static search list wins over DHCP; stop removes everything */
	rt_lock();
	CHECK(route(0x0a000000u, 8, GW2) != NULL && route(0, 0, GW254) && !route(0, 0, SRV_IP));
	rt_dhcp_stop(nif);
	CHECK(nif->addr == 0 && n_default_routes() == 0);
	CHECK(!route(0x0a000000u, 8, GW2) && !route(0xac100500u, 24, 0) &&
	      !route(0xc6336407u, 32, 0xc0a85803u));
	CHECK(!rt_dns_search_list()[0] && !rt_domainname[0]);
	rt_dns_set_search("mine.example");
	rt_unlock();
	ev_bound = 0;
	rt_lock();
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	rt_unlock();
	CHECK(wait_for(&ev_bound, 1, 9000));
	rt_lock();
	CHECK(!strcmp(rt_dns_search_list(), "mine.example"));
	rt_dhcp_stop(nif);
	CHECK(!strcmp(rt_dns_search_list(), "mine.example"));
	rt_dns_set_search("");
	rt_unlock();
}

/* ---------------------------------------------------------------- 3 */

static u32 mac_candidate(void)
{
	u32 n = 2166136261u;
	int i;

	for (i = 0; i < 6; i++)
		n = (n ^ cli_mac[i]) * 16777619u;
	return 0xa9fe0000u + 0x100u + n % 0xfe00u;
}

static void test_linklocal(void)
{
	char out[256], line[80];
	struct rt_dhcp_info di;
	u32 a3, a4, t0;
	int k, probes, ann;

	silent = 1;
	n_announce = 0;
	cand_action[0] = 1;            /* the first candidate is in use */
	cand_action[1] = 2;            /* another host probes for the second */
	rt_lock();
	CHECK(rt_dhcp_start(nif, dhcp_cb) == 0);
	CHECK(rt_dhcp_set_linklocal(nif, 1, 1) == 0);
	CHECK(rt_dhcp_get_linklocal(nif, &k) == 1 && k == 1);
	rt_unlock();

	CHECK(wait_addr(1, 20000));
	rt_lock();
	a3 = nif->addr;
	CHECK(ncand == 3 && cand[2] == rt_ntohl(a3));
	CHECK(cand[0] == mac_candidate());                     /* from the MAC */
	for (k = 0; k < ncand; k++)
		CHECK(cand[k] >= 0xa9fe0100u && cand[k] <= 0xa9fefeffu);
	CHECK(nif->netmask == rt_htonl(0xffff0000u));
	CHECK(route(0xa9fe0000u, 16, 0) != NULL && n_default_routes() == 0);
	rt_dhcp_get_info(nif, &di);
	CHECK(di.ll_addr == a3 && di.ll_conflicts == 2 &&
	      (di.ll_state == RT_LL_ANNOUNCE || di.ll_state == RT_LL_BOUND));
	CHECK(di.state == RT_DHCPS_SELECTING);
	rt_unlock();
	/* three probes for the address it took, 1-2 s apart */
	CHECK(n_probe >= 5);
	if (n_probe >= 5 && n_probe <= 64) {
		int d1 = (int)(probe_ms[n_probe - 2] - probe_ms[n_probe - 3]);
		int d2 = (int)(probe_ms[n_probe - 1] - probe_ms[n_probe - 2]);
		CHECK(d1 >= 950 && d1 <= 2200 && d2 >= 950 && d2 <= 2200);
	}
	/* two announcements, two seconds apart */
	CHECK(wait_for(&n_announce, 2, 4000));
	CHECK(last_announce == rt_ntohl(a3));
	rt_lock();
	rt_dhcp_get_info(nif, &di);
	CHECK(di.ll_state == RT_LL_BOUND);
	rt_unlock();

	/* someone else claims it: defended once, address kept */
	ann = n_announce;
	probes = n_probe;
	send_arp(1, rt_ntohl(a3), rt_ntohl(a3), bc);
	CHECK(wait_for(&n_announce, ann + 1, 1000));
	usleep(200000);
	CHECK(nif->addr == a3);
	/* again within ten seconds: given up, a new one */
	send_arp(2, rt_ntohl(a3), rt_ntohl(a3), cli_mac);
	CHECK(wait_for(&n_probe, probes + 1, 2000));
	CHECK(wait_addr(a3, 12000));
	rt_lock();
	a4 = nif->addr;
	CHECK(a4 != a3 && rt_ipv4ll_is_ll(a4));
	rt_dhcp_get_info(nif, &di);
	CHECK(di.ll_addr == a4 && di.ll_conflicts == 3);

	/* "dhcp IF linklocal off" drops it at once; on again: the MAC
	 * candidate comes back after the wait (nobody answers now) */
	snprintf(line, sizeof(line), "dhcp %s linklocal off", nif->name);
	CHECK(rt_ctl_command(line, out, sizeof(out), NULL) == 0);
	CHECK(nif->addr == 0 && !route(0xa9fe0000u, 16, 0));
	rt_dhcp_get_info(nif, &di);
	CHECK(di.ll_addr == 0 && di.ll_state == RT_LL_OFF && di.state == RT_DHCPS_SELECTING);
	CHECK(rt_dhcp_get_linklocal(nif, NULL) == 0);
	snprintf(line, sizeof(line), "dhcp %s linklocal maybe", nif->name);
	CHECK(rt_ctl_command(line, out, sizeof(out), NULL) < 0);
	snprintf(line, sizeof(line), "dhcp %s linklocal on wait 1", nif->name);
	CHECK(rt_ctl_command(line, out, sizeof(out), NULL) == 0);
	rt_unlock();
	t0 = rt_now_ms();
	CHECK(wait_addr(a4, 12000));
	CHECK(rt_now_ms() - t0 >= 2500);
	CHECK(nif->addr == rt_htonl(mac_candidate()));

	/* a lease replaces it */
	silent = 0;
	ev_bound = 0;
	rt_lock();
	CHECK(rt_dhcp_renew(nif) == 0);
	rt_unlock();
	CHECK(wait_for(&ev_bound, 1, 6000));
	rt_lock();
	CHECK(nif->addr == rt_htonl(CLI_IP));
	CHECK(!route(0xa9fe0000u, 16, 0) && route(0, 0, GW254));
	rt_dhcp_get_info(nif, &di);
	CHECK(di.ll_addr == 0 && di.ll_state == RT_LL_OFF);
	rt_dhcp_stop(nif);
	CHECK(nif->addr == 0);
	rt_unlock();
}

/* ---------------------------------------------------------------- 4 */

static void test_static(void)
{
	char out[256], line[80];
	int probes = n_probe;
	u8 st;

	rt_lock();
	CHECK(rt_netif_set_addr(nif, rt_htonl(0x0a090909u), rt_htonl(0xffffff00u), 0) == 0);
	CHECK(rt_ipv4ll_start(nif) == 0);
	rt_unlock();
	usleep(2500000);
	rt_lock();
	CHECK(n_probe == probes && nif->addr == rt_htonl(0x0a090909u));
	CHECK(rt_ipv4ll_info(nif, &st, NULL) == 0 && st == RT_LL_PROBE);
	rt_ipv4ll_stop(nif);
	CHECK(nif->addr == rt_htonl(0x0a090909u));
	snprintf(line, sizeof(line), "ifconfig %s 169.254.7.7/16 up", nif->name);
	CHECK(rt_ctl_command(line, out, sizeof(out), NULL) == 0);
	CHECK(nif->addr == rt_htonl(0xa9fe0707u) && route(0xa9fe0000u, 16, 0));
	CHECK(rt_ipv4ll_info(nif, &st, NULL) == 0 && st == RT_LL_OFF);
	rt_unlock();
}

/* the resolver's search list (resolv.c), as option 119 fills it */
static void test_search(void)
{
	char out[256];
	u32 a[4];

	rt_lock();
	rt_dns_set_search(" .x.example., y.example,X.EXAMPLE\tz1 z2 z3 z4 z5");
	CHECK(!strcmp(rt_dns_search_list(), "x.example y.example z1 z2 z3 z4"));
	CHECK(rt_ctl_command("search", out, sizeof(out), NULL) == 0 && !rt_dns_search_list()[0]);
	CHECK(rt_ctl_command("search a.example b.example", out, sizeof(out), NULL) == 0);
	CHECK(!strcmp(rt_dns_search_list(), "a.example b.example"));
	rt_hosts_clear();
	CHECK(rt_hosts_add("nas.b.example", 0x0a000005u) == 0);
	rt_dns_clear_servers();
	rt_unlock();
	/* a single label: every domain of the list */
	CHECK(rt_resolve_name("nas", a, 4, NULL, 0) == 1 && a[0] == 0x0a000005u);
	CHECK(rt_resolve_name("nas.b", a, 4, NULL, 0) < 0);
	rt_lock();
	rt_hosts_clear();
	rt_dns_set_search("");
	rt_unlock();
}

int main(void)
{
	struct sockaddr_un a;
	pthread_t th;

	test_parse();

	snprintf(path_a, sizeof(path_a), "/tmp/rt_dhll_a.%d", (int)getpid());
	snprintf(path_b, sizeof(path_b), "/tmp/rt_dhll_b.%d", (int)getpid());
	unlink(path_a);
	unlink(path_b);
	wfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strcpy(a.sun_path, path_b);
	if (wfd < 0 || bind(wfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		printf("t_dhcp_ll: SKIP (no AF_UNIX sockets)\n");
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
	rt_domainname[0] = 0;
	rt_dns_set_search("");
	rt_dns_clear_servers();
	rt_unlock();

	test_search();
	test_options();
	test_linklocal();
	test_static();

	stop_server = 1;
	pthread_join(th, NULL);
	rt_host_stop();
	unlink(path_a);
	unlink(path_b);
	printf("t_dhcp_ll: %d checks, %d failures (probes %d announcements %d)\n",
	       checks, failures, n_probe, n_announce);
	return failures ? 1 : 0;
}
