/*
 * Riptide - ICMP interop with the Linux kernel over TUN (needs root and
 * /dev/net/tun; exits 77 = skip otherwise).
 *   Linux 10.77.0.1/30  <-- tun -->  Riptide 10.77.0.2
 * Linux pings us (small and fragmented echo requests), and we ping Linux
 * through a raw ICMP socket, like Amiga ping tools do.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int sh(const char *fmt, const char *arg)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), fmt, arg, arg, arg);
	return system(cmd);
}

static int have_ping;

/* Linux side pinger: the ping(8) tool when installed, else an equivalent
 * using a Linux raw ICMP socket. Returns 0 if every echo was answered. */
static int linux_ping(const char *dst, int count, int size)
{
	static u8 pkt[4000], buf[4200];
	struct sockaddr_in a;
	int fd, seq, ok = 0;

	if (have_ping) {
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "ping -n -q -c %d -i 0.2 -W 2 -s %d %s >/dev/null", count, size, dst);
		return system(cmd);
	}
	fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = rt_htonl(0x0a4d0002);
	for (seq = 1; seq <= count; seq++) {
		u32 len = 8 + (u32)size;
		u16 ck;
		int t;

		memset(pkt, 0, len);
		pkt[0] = 8;
		rt_put_be16(pkt + 4, 0x4c58);
		rt_put_be16(pkt + 6, (u16)seq);
		memset(pkt + 8, seq, (size_t)size);
		ck = rt_inet_csum(pkt, len);
		memcpy(pkt + 2, &ck, 2);
		sendto(fd, pkt, len, 0, (struct sockaddr *)&a, sizeof(a));
		for (t = 0; t < 20; t++) {
			struct pollfd p = { fd, POLLIN, 0 };
			ssize_t n;
			if (poll(&p, 1, 2000) <= 0)
				break;
			n = recv(fd, buf, sizeof(buf), 0);
			if (n >= 28 && buf[20] == 0 && rt_get_be16(buf + 24) == 0x4c58 &&
			    rt_get_be16(buf + 26) == seq && (u32)n == 20 + len) {
				ok++;
				break;
			}
		}
	}
	close(fd);
	return ok == count ? 0 : 1;
}

/* One raw-socket ping from the stack; returns 1 when the reply came. */
static int stack_ping(struct rt_sockctx *c, int rs, u32 dst, u16 seq, u32 size)
{
	static u8 pkt[4000], buf[4100];
	struct rt_sockaddr_in a;
	u16 ck;
	int i, r;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = RT_ICMP_ECHO;
	rt_put_be16(pkt + 4, 0x5254);
	rt_put_be16(pkt + 6, seq);
	for (i = 8; i < (int)size; i++)
		pkt[i] = (u8)(i ^ seq);
	ck = rt_inet_csum(pkt, size);
	memcpy(pkt + 2, &ck, 2);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_addr = dst;
	if (rt_api_sendto(c, rs, pkt, size, 0, &a, sizeof(a)) != (int)size)
		return 0;
	for (i = 0; i < 10; i++) {
		r = rt_api_recv(c, rs, buf, sizeof(buf), 0);
		if (r < 0)
			return 0;
		if (r < 28 || (buf[0] >> 4) != 4)
			continue;
		{
			u32 hl = (u32)(buf[0] & 15) * 4;
			u8 *ic = buf + hl;
			if ((u32)r == hl + size && ic[0] == RT_ICMP_ECHOREPLY && rt_get_be16(ic + 6) == seq &&
			    memcmp(ic + 8, pkt + 8, size - 8) == 0 && rt_inet_csum(ic, size) == 0)
				return 1;
		}
	}
	return 0;
}

/* ---- traceroute: UDP probes with IP_TTL, ICMP echo with IP_TTL ----
 *   Riptide 10.77.0.2 --tun-- 10.77.0.1 Linux (forwarding) 10.77.5.1
 *        --veth-- 10.77.5.2 netns "rttrN"
 * Hop 1 must answer TIME_EXCEEDED, hop 2 PORT_UNREACH (UDP) or
 * ECHOREPLY (ICMP), and the raw ICMP socket must see the errors with the
 * probe quoted. */
static char tr_ns[32];

static int tr_setup(const char *tun_name)
{
	snprintf(tr_ns, sizeof(tr_ns), "rttr%d", (int)(getpid() % 100000));
	if (sh("ip netns add %s 2>/dev/null", tr_ns))
		return -1;
	if (sh("ip link add %sa type veth peer name %sb 2>/dev/null", tr_ns) ||
	    sh("ip link set %sb netns %s", tr_ns) ||
	    sh("ip addr add 10.77.5.1/24 dev %sa && ip link set %sa up", tr_ns) ||
	    sh("ip netns exec %s sh -c 'ip addr add 10.77.5.2/24 dev %sb && ip link set %sb up "
	       "&& ip link set lo up && ip route add default via 10.77.5.1'", tr_ns) ||
	    sh("sysctl -qw net.ipv4.ip_forward=1", tr_ns))
		return -1;
	sh("sysctl -qw net.ipv4.conf.all.rp_filter=0", tr_ns);
	{
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "sysctl -qw net.ipv4.conf.%s.rp_filter=0", tun_name);
		if (system(cmd)) {}
	}
	return 0;
}

static void tr_teardown(void)
{
	sh("ip netns del %s 2>/dev/null", tr_ns);
	sh("ip link del %sa 2>/dev/null", tr_ns);
}

/* Wait for the ICMP answer to the probe to dst (UDP dport, or ICMP echo
 * seq). Returns the ICMP type (or -1) and the sender. */
static int tr_wait(struct rt_sockctx *c, int rs, u32 dst, int udp, u16 key, u32 *from)
{
	u8 buf[600];
	u32 t0 = rt_now_ms();

	while ((s32)(rt_now_ms() - t0) < 3000) {
		struct rt_sockaddr_in f;
		u32 fl = sizeof(f), hl, ihl;
		const u8 *ic, *in;
		int n = rt_api_recvfrom(c, rs, buf, sizeof(buf), 0, &f, &fl);
		if (n < 0)
			return -1;
		hl = (u32)(buf[0] & 15) * 4;
		if ((u32)n < hl + 8)
			continue;
		ic = buf + hl;
		if (ic[0] == RT_ICMP_ECHOREPLY && !udp) {
			if (rt_get_be16(ic + 4) == 0x7472 && rt_get_be16(ic + 6) == key) {
				*from = f.sin_addr;
				return ic[0];
			}
			continue;
		}
		if (ic[0] != RT_ICMP_TIMXCEED && ic[0] != RT_ICMP_UNREACH)
			continue;
		if ((u32)n < hl + 8 + 20 + 8)
			continue;
		in = ic + 8;
		ihl = (u32)(in[0] & 15) * 4;
		if (memcmp(in + 16, &dst, 4) != 0 || (u32)n < hl + 8 + ihl + 8)
			continue;
		if (udp ? (in[9] != RT_IPPROTO_UDP || rt_get_be16(in + ihl + 2) != key)
		        : (in[9] != RT_IPPROTO_ICMP || rt_get_be16(in + ihl + 6) != key))
			continue;
		*from = f.sin_addr;
		return ic[0];
	}
	return -1;
}

static void test_traceroute(struct rt_sockctx *c, struct rt_netif *tun, const char *tun_name)
{
	u32 linux_ip = rt_htonl(0x0a4d0001), far = rt_htonl(0x0a4d0502), from;
	struct rt_timeval tv = { 1, 0 };
	struct rt_sockaddr_in a;
	int rs, us, ttl, t;
	u8 pkt[16];
	char out[200];

	if (tr_setup(tun_name)) {
		printf("traceroute test skipped (netns/veth setup failed)\n");
		tr_teardown();
		return;
	}
	rt_lock();
	CHECK(rt_ctl_command("route add 10.77.5.0/24 via 10.77.0.1", out, sizeof(out), NULL) == 0);
	rt_unlock();
	rs = rt_api_socket(c, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	us = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	rt_api_setsockopt(c, rs, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_addr = far;
	/* UDP probes */
	for (ttl = 1; ttl <= 2; ttl++) {
		s32 v = ttl, got = 0;
		u32 gl = sizeof(got);
		CHECK(rt_api_setsockopt(c, us, RT_IPPROTO_IP, RT_IP_TTL, &v, sizeof(v)) == 0);
		CHECK(rt_api_getsockopt(c, us, RT_IPPROTO_IP, RT_IP_TTL, &got, &gl) == 0 && got == ttl);
		a.sin_port = rt_htons((u16)(33434 + ttl));
		CHECK(rt_api_sendto(c, us, "probe", 5, 0, &a, sizeof(a)) == 5);
		t = tr_wait(c, rs, far, 1, (u16)(33434 + ttl), &from);
		if (ttl == 1)
			CHECK(t == RT_ICMP_TIMXCEED && from == linux_ip);
		else
			CHECK(t == RT_ICMP_UNREACH && from == far);
	}
	/* ICMP echo probes with IP_TTL on the raw socket */
	for (ttl = 1; ttl <= 2; ttl++) {
		s32 v = ttl;
		u16 ck;
		CHECK(rt_api_setsockopt(c, rs, RT_IPPROTO_IP, RT_IP_TTL, &v, sizeof(v)) == 0);
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = RT_ICMP_ECHO;
		rt_put_be16(pkt + 4, 0x7472);
		rt_put_be16(pkt + 6, (u16)ttl);
		ck = rt_inet_csum(pkt, sizeof(pkt));
		memcpy(pkt + 2, &ck, 2);
		a.sin_port = 0;
		CHECK(rt_api_sendto(c, rs, pkt, sizeof(pkt), 0, &a, sizeof(a)) == (int)sizeof(pkt));
		t = tr_wait(c, rs, far, 0, (u16)ttl, &from);
		if (ttl == 1)
			CHECK(t == RT_ICMP_TIMXCEED && from == linux_ip);
		else
			CHECK(t == RT_ICMP_ECHOREPLY && from == far);
	}
	printf("traceroute probes done (last answer type %d)\n", t);
	rt_api_close(c, us);
	rt_api_close(c, rs);
	tr_teardown();
	(void)tun;
}

int main(void)
{
	struct rt_netif *tun;
	struct rt_sockctx c;
	char name[16];
	int rs, r;
	struct rt_timeval tv = { 2, 0 };
	u32 linux_ip = rt_htonl(0x0a4d0001), our_ip = rt_htonl(0x0a4d0002);

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0) {
		printf("t_icmp_tun: SKIP (needs root and /dev/net/tun)\n");
		return 77;
	}
	if (system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_icmp_tun: SKIP (needs iproute2)\n");
		return 77;
	}
	have_ping = system("ping -V >/dev/null 2>&1") == 0 && !getenv("RT_NO_PING");
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rti%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, our_ip, linux_ip, rt_htonl(0xfffffffc));
	if (!tun) {
		printf("t_icmp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	if (sh("ip addr add 10.77.0.1/30 dev %s && ip link set %s up", name) != 0) {
		printf("t_icmp_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}

	/* Linux -> Riptide echo: plain, then 3000 bytes (fragmented both ways) */
	CHECK(linux_ping("10.77.0.2", 3, 56) == 0);
	CHECK(linux_ping("10.77.0.2", 2, 3000) == 0);
	CHECK(linux_ping("10.77.0.2", 1, 1472) == 0);   /* exactly fills the MTU */
	CHECK(rt_ipstat.ReasmOKs >= 2);
	CHECK(rt_icmpstat.InEchos >= 6 && rt_icmpstat.OutEchoReps >= 6);

	/* Riptide -> Linux via raw ICMP socket */
	CHECK(rt_ctx_init(&c, 16, NULL) == 0);
	rs = rt_api_socket(&c, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	CHECK(rs >= 0);
	rt_api_setsockopt(&c, rs, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	CHECK(stack_ping(&c, rs, linux_ip, 1, 64));
	CHECK(stack_ping(&c, rs, linux_ip, 2, 3500));      /* we fragment, Linux reassembles */
	/* ICMP error path: UDP to a closed Linux port makes Linux answer with
	 * port unreachable, which our raw socket also sees */
	{
		int us = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
		struct rt_sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_len = sizeof(a);
		a.sin_family = RT_AF_INET;
		a.sin_addr = linux_ip;
		a.sin_port = rt_htons(9);
		rt_api_setsockopt(&c, us, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		CHECK(rt_api_connect(&c, us, &a, sizeof(a)) == 0);
		CHECK(rt_api_send(&c, us, "knock", 5, 0) == 5);
		r = rt_api_recv(&c, us, name, sizeof(name), 0);
		CHECK(r == -RT_ECONNREFUSED);
		rt_api_close(&c, us);
	}
	rt_api_close(&c, rs);
	test_traceroute(&c, tun, name);
	rt_ctx_destroy(&c);
	rt_host_stop();
	printf("t_icmp_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
