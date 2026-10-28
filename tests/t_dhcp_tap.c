/*
 * Riptide - DHCP options 121/119 and IPv4 link-local addresses against
 * Linux over TAP (needs root, /dev/net/tun, iproute2, ping and python3;
 * exits 77 = skip otherwise):
 *
 *   Linux 10.82.1.1/24, 10.99.0.1/32, 169.254.1.1/16 <-- tap --> Riptide
 *
 * 1. tests/interop/dhcp_server.py hands out 10.82.1.50 with classless
 *    routes 10.99.0.0/16 and 0/0 via 10.82.1.1 (option 3 says .254 and is
 *    ignored) and a compressed search list: the routes are used (a
 *    datagram to 10.99.0.1 reaches Linux), Linux pings the lease
 * 2. no DHCP server: a 169.254 address after the wait, Linux pings it, no
 *    default route
 * 3. Linux claims that address (gratuitous ARP, as "arping -U"; arping
 *    itself when installed): defended once, given up after the second, a
 *    new one that Linux pings
 * 4. the server comes back: the lease replaces the link-local address
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include "rt/riptide.h"
#include "rt/rt_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rt_sockctx ctx;
static char tap_name[16];
static struct rt_netif *tap;
static const u8 mac[6] = { 0x02, 0x52, 0x54, 0x00, 0x82, 0x02 };
static pid_t srv_pid = -1;

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

static u32 A(const char *s)
{
	struct in_addr a;
	inet_pton(AF_INET, s, &a);
	return a.s_addr;
}

static const char *S(u32 a, char *buf)
{
	return inet_ntop(AF_INET, &a, buf, 16);
}

static int config(const char *fmt, ...)
{
	char line[256], err[256];
	va_list ap;
	int r;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	rt_lock();
	r = rt_config_line(line, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0)
		fprintf(stderr, "config \"%s\": %s\n", line, err);
	return r;
}

static void server_start(void)
{
	srv_pid = fork();
	if (srv_pid == 0) {
		execlp("python3", "python3", "tests/interop/dhcp_server.py", "--iface", tap_name,
		       "--server", "10.82.1.1", "--addr", "10.82.1.50",
		       "--classless", "10.99.0.0/16,10.82.1.1;0.0.0.0/0,10.82.1.1",
		       "--router", "10.82.1.254", "--domain", "dhcp.example",
		       "--search", "lan.example,corp.example,eng.corp.example", (char *)NULL);
		_exit(127);
	}
	usleep(700000);            /* python starts */
}

static void server_stop(void)
{
	if (srv_pid > 0) {
		kill(srv_pid, SIGTERM);
		waitpid(srv_pid, NULL, 0);
	}
	srv_pid = -1;
}

static struct rt_route *route(const char *dst, u32 bits, const char *gw)
{
	u32 mask = bits ? rt_htonl(0xffffffffu << (32 - bits)) : 0;
	struct rt_route *r;

	for (r = rt_route_list(); r; r = r->next)
		if (r->dst == A(dst) && r->mask == mask && r->gateway == A(gw))
			return r;
	return NULL;
}

/* wait for nif's address to satisfy: 1 = the lease, 2 = link-local and not 'not' */
static u32 wait_addr(int what, u32 not, int ms)
{
	long t = 0;
	u32 a;

	for (;;) {
		rt_lock();
		a = tap->addr;
		rt_unlock();
		if (what == 1 ? a == A("10.82.1.50") : (a && rt_ipv4ll_is_ll(a) && a != not))
			return a;
		if (t >= ms)
			return 0;
		usleep(50000);
		t += 50;
	}
}

static int lx_udp(u16 port)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
	struct sockaddr_in sin;

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	bind(s, (struct sockaddr *)&sin, sizeof(sin));
	return s;
}

/* Riptide -> dst:port, Linux receives it: the source address, 0 = nothing */
static u32 udp_check(int lx, const char *dst, u16 port)
{
	struct rt_sockaddr_in sin;
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	struct pollfd pf = { lx, POLLIN, 0 };
	char buf[64];
	int s = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(port);
	sin.sin_addr = A(dst);
	rt_api_sendto(&ctx, s, "hello", 5, 0, &sin, sizeof(sin));
	rt_api_close(&ctx, s);
	if (poll(&pf, 1, 2000) <= 0 || recvfrom(lx, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) < 0)
		return 0;
	return from.sin_addr.s_addr;
}

/* Linux claims addr: a gratuitous ARP from the tap's MAC address */
static void lx_claim(u32 addr)
{
	char a[16];
	int s;
	u8 f[28], lmac[6];
	struct sockaddr_ll ll;
	FILE *fp;
	char path[64];
	unsigned m[6];

	if (sh("command -v arping >/dev/null 2>&1") == 0 &&
	    sh("arping -q -U -c 1 -I %s -s %s %s >/dev/null 2>&1", tap_name, S(addr, a), a) == 0)
		return;
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", tap_name);
	fp = fopen(path, "r");
	if (!fp || fscanf(fp, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
		if (fp)
			fclose(fp);
		return;
	}
	fclose(fp);
	for (s = 0; s < 6; s++)
		lmac[s] = (u8)m[s];
	s = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ARP));
	if (s < 0)
		return;
	rt_put_be16(f, 1);
	rt_put_be16(f + 2, 0x0800);
	f[4] = 6;
	f[5] = 4;
	rt_put_be16(f + 6, 1);
	memcpy(f + 8, lmac, 6);
	memcpy(f + 14, &addr, 4);
	memset(f + 18, 0, 6);
	memcpy(f + 24, &addr, 4);
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(ETH_P_ARP);
	ll.sll_ifindex = (int)if_nametoindex(tap_name);
	ll.sll_halen = 6;
	memset(ll.sll_addr, 0xff, 6);
	sendto(s, f, sizeof(f), 0, (struct sockaddr *)&ll, sizeof(ll));
	close(s);
}

int main(void)
{
	struct rt_dhcp_info di;
	struct rt_route *r;
	char a[16];
	u32 ll1, ll2;
	int lx, i;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0 || system("ping -V >/dev/null 2>&1") != 0 ||
	    system("python3 -c '' >/dev/null 2>&1") != 0 ||
	    access("tests/interop/dhcp_server.py", R_OK) != 0) {
		printf("t_dhcp_tap: SKIP (needs root, /dev/net/tun, iproute2, ping, python3)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(150);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tap_name, sizeof(tap_name), "rtd%d", (int)(getpid() % 100000));
	tap = rt_host_tap_open(tap_name, mac);
	if (!tap) {
		printf("t_dhcp_tap: SKIP (cannot create TAP device)\n");
		return 77;
	}
	if (sh("ip addr add 10.82.1.1/24 dev %s && ip addr add 10.99.0.1/32 dev %s && "
	       "ip addr add 169.254.1.1/16 dev %s && ip link set %s up",
	       tap_name, tap_name, tap_name, tap_name)) {
		printf("t_dhcp_tap: SKIP (cannot configure %s)\n", tap_name);
		return 77;
	}
	rt_ctx_init(&ctx, 16, NULL);
	lx = lx_udp(5599);

	/* 1: classless routes and the search list */
	server_start();
	CHECK(config("dhcp %s", tap_name) == 0);
	CHECK(wait_addr(1, 0, 15000) != 0);
	rt_lock();
	r = route("10.99.0.0", 16, "10.82.1.1");
	CHECK(r && (r->flags & RT_RTF_DHCP));
	CHECK(route("0.0.0.0", 0, "10.82.1.1") && !route("0.0.0.0", 0, "10.82.1.254"));
	CHECK(!strcmp(rt_dns_search_list(), "lan.example corp.example eng.corp.example"));
	CHECK(rt_dhcp_get_info(tap, &di) == RT_DHCPS_BOUND && di.nroutes == 2);
	rt_unlock();
	CHECK(sh("ping -c 1 -W 2 10.82.1.50 >/dev/null 2>&1") == 0);
	CHECK(udp_check(lx, "10.99.0.1", 5599) == A("10.82.1.50"));

	/* 2: no server: link-local */
	server_stop();
	CHECK(config("dhcp %s stop", tap_name) == 0);
	CHECK(config("dhcp %s linklocal on wait 1", tap_name) == 0);
	ll1 = wait_addr(2, 0, 15000);
	CHECK(ll1 != 0);
	printf("link-local %s\n", S(ll1, a));
	rt_lock();
	CHECK(tap->netmask == A("255.255.0.0") && route("169.254.0.0", 16, "0.0.0.0"));
	for (r = rt_route_list(), i = 0; r; r = r->next)
		if (r->mask == 0)
			i++;
	CHECK(i == 0);
	rt_unlock();
	CHECK(sh("ping -c 1 -W 2 %s >/dev/null 2>&1", S(ll1, a)) == 0);
	CHECK(udp_check(lx, "169.254.1.1", 5599) == ll1);

	/* 3: Linux claims it twice: a new address */
	usleep(4000000);           /* announcing done */
	lx_claim(ll1);
	usleep(300000);
	CHECK(tap->addr == ll1);   /* defended */
	lx_claim(ll1);
	ll2 = wait_addr(2, ll1, 15000);
	CHECK(ll2 != 0 && ll2 != ll1);
	printf("after the conflict %s\n", S(ll2, a));
	rt_lock();
	rt_dhcp_get_info(tap, &di);
	CHECK(di.ll_addr == ll2 && di.ll_conflicts >= 1);
	rt_unlock();
	CHECK(sh("ping -c 1 -W 2 %s >/dev/null 2>&1", S(ll2, a)) == 0);

	/* 4: a lease replaces it */
	server_start();
	CHECK(config("dhcp %s renew", tap_name) == 0);
	CHECK(wait_addr(1, 0, 15000) != 0);
	rt_lock();
	CHECK(!route("169.254.0.0", 16, "0.0.0.0"));
	rt_dhcp_get_info(tap, &di);
	CHECK(di.ll_addr == 0 && di.ll_state == RT_LL_OFF);
	rt_unlock();
	CHECK(sh("ping -c 1 -W 2 10.82.1.50 >/dev/null 2>&1") == 0);

	CHECK(config("dhcp %s stop", tap_name) == 0);
	server_stop();
	close(lx);
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	printf("t_dhcp_tap: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
