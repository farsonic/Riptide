/*
 * Riptide - IPv4 multicast interop with the Linux kernel over TAP (needs
 * root, /dev/net/tun and iproute2; exits 77 = skip otherwise).
 *
 *   Linux 10.79.7.1/24  <-- tap -->  Riptide 10.79.7.2/24
 *
 * Riptide joins a group with IP_ADD_MEMBERSHIP: its IGMPv3 report is seen
 * on a packet socket and Linux's datagrams to the group arrive (those to
 * another group are filtered). Linux joins a group and receives Riptide's
 * datagrams (source, TTL). Queries crafted on a Linux raw socket get the
 * right answers: v3 general query -> IS_EX report, v2 query -> v2 report
 * (fallback), drop -> v2 leave to 224.0.0.2.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
/* Riptide headers first: <net/if.h> defines ifr_* macros */
#include "rt/riptide.h"
#include "rt/rt_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
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
static const u8 mac[6] = { 0x02, 0x52, 0x54, 0x00, 0x79, 0x02 };
static int psock = -1;            /* Linux: IP packets on the tap */

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

/* Next IGMP message that Riptide sent (seen by Linux) within ms;
 * returns its length, copies the IP datagram to pkt. */
static int igmp_from_riptide(u8 *pkt, int max, int ms)
{
	struct timeval end, now;

	gettimeofday(&end, NULL);
	end.tv_sec += ms / 1000;
	end.tv_usec += (ms % 1000) * 1000;
	if (end.tv_usec >= 1000000) {
		end.tv_sec++;
		end.tv_usec -= 1000000;
	}
	for (;;) {
		struct pollfd pf = { psock, POLLIN, 0 };
		struct sockaddr_ll ll;
		socklen_t sl = sizeof(ll);
		long left;
		ssize_t n;

		gettimeofday(&now, NULL);
		left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_usec - now.tv_usec) / 1000;
		if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
			return 0;
		n = recvfrom(psock, pkt, (size_t)max, 0, (struct sockaddr *)&ll, &sl);
		if (n < 24 || ll.sll_pkttype == PACKET_OUTGOING)
			continue;
		if ((pkt[0] >> 4) != 4 || pkt[9] != 2 || memcmp(pkt + 12, &(u32){ A("10.79.7.2") }, 4))
			continue;
		return (int)n;
	}
}

static const u8 *igmp_msg(const u8 *pkt) { return pkt + (pkt[0] & 15) * 4; }

static int v3_has(const u8 *pkt, u8 type, u32 grp)
{
	const u8 *m = igmp_msg(pkt), *r = m + 8;
	int i, n = (m[6] << 8) | m[7];

	if (m[0] != 0x22)
		return 0;
	for (i = 0; i < n; i++) {
		if (r[0] == type && !memcmp(r + 4, &grp, 4))
			return 1;
		r += 8 + 4 * ((r[2] << 8) | r[3]) + 4 * r[1];
	}
	return 0;
}

/* Linux: send an IGMP message from 10.79.7.1 (raw socket, TTL 1) */
static void lx_igmp(const u8 *m, int len, const char *dst)
{
	int s = socket(AF_INET, SOCK_RAW, IPPROTO_IGMP), ttl = 1, loop = 0;
	struct in_addr ifa = { A("10.79.7.1") };
	struct sockaddr_in to;
	u8 buf[64];
	u32 sum = 0;
	int i;

	memcpy(buf, m, (size_t)len);
	buf[2] = buf[3] = 0;
	for (i = 0; i < len; i += 2)
		sum += (u32)(buf[i] << 8 | (i + 1 < len ? buf[i + 1] : 0));
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	sum = ~sum & 0xffff;
	buf[2] = (u8)(sum >> 8);
	buf[3] = (u8)sum;
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = A(dst);
	CHECK(sendto(s, buf, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to)) == len);
	close(s);
}

static void lx_send_udp(const char *grp, u16 port, const char *text)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct in_addr ifa = { A("10.79.7.1") };
	struct sockaddr_in to;

	setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = A(grp);
	CHECK(sendto(s, text, strlen(text), 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)strlen(text));
	close(s);
}

static int rt_udp_socket(u16 port)
{
	struct rt_sockaddr_in a;
	struct rt_timeval tv = { 2, 0 };
	int s = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0), one = 1;

	rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(port);
	CHECK(rt_api_bind(&ctx, s, &a, sizeof(a)) == 0);
	return s;
}

static int rt_mship(int s, int add, const char *grp)
{
	struct rt_ip_mreq mr;

	mr.imr_multiaddr = A(grp);
	mr.imr_interface = A("10.79.7.2");
	return rt_api_setsockopt(&ctx, s, RT_IPPROTO_IP, add ? RT_IP_ADD_MEMBERSHIP : RT_IP_DROP_MEMBERSHIP,
	                         &mr, sizeof(mr));
}

static void test_riptide_receives(void)
{
	u8 pkt[2048];
	char buf[64];
	int s = rt_udp_socket(7001), n, got = 0;

	CHECK(rt_mship(s, 1, "239.77.0.1") == 0);
	/* the unsolicited IGMPv3 report: TO_EX {} for the group, router alert */
	n = igmp_from_riptide(pkt, sizeof(pkt), 2000);
	CHECK(n > 0 && v3_has(pkt, 4, A("239.77.0.1")));
	CHECK(n > 0 && pkt[0] == 0x46 && pkt[20] == 0x94 && pkt[8] == 1 &&
	      !memcmp(pkt + 16, &(u32){ A("224.0.0.22") }, 4));
	printf("t_mcast_tap: Riptide's IGMPv3 report seen by Linux\n");
	/* the retransmission (robustness 2) within a second */
	n = igmp_from_riptide(pkt, sizeof(pkt), 2000);
	CHECK(n > 0 && v3_has(pkt, 4, A("239.77.0.1")));

	lx_send_udp("239.77.0.2", 7001, "other");      /* not joined: filtered */
	lx_send_udp("239.77.0.1", 7001, "hello group");
	while ((n = rt_api_recv(&ctx, s, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = 0;
		if (!strcmp(buf, "hello group"))
			got++;
		else
			got += 100;
		break;
	}
	CHECK(got == 1);
	CHECK(rt_api_recv(&ctx, s, buf, sizeof(buf), RT_MSG_DONTWAIT) == -RT_EWOULDBLOCK);
	printf("t_mcast_tap: Linux -> 239.77.0.1 received by Riptide: %s\n", got == 1 ? "ok" : "FAILED");

	/* queries from Linux: v3 general query -> IS_EX record */
	{
		u8 q[12] = { 0x11, 10, 0, 0, 0, 0, 0, 0, 2, 125, 0, 0 };
		lx_igmp(q, 12, "224.0.0.1");
		n = igmp_from_riptide(pkt, sizeof(pkt), 3000);
		CHECK(n > 0 && v3_has(pkt, 2, A("239.77.0.1")));
		printf("t_mcast_tap: v3 general query answered: %s\n", n > 0 ? "ok" : "FAILED");
	}
	/* v2 query: Riptide falls back, answers with a v2 report to the group */
	{
		u8 q[8] = { 0x11, 10, 0, 0, 0, 0, 0, 0 };
		lx_igmp(q, 8, "224.0.0.1");
		n = igmp_from_riptide(pkt, sizeof(pkt), 3000);
		CHECK(n > 0 && igmp_msg(pkt)[0] == 0x16 && !memcmp(pkt + 16, &(u32){ A("239.77.0.1") }, 4) &&
		      !memcmp(igmp_msg(pkt) + 4, &(u32){ A("239.77.0.1") }, 4));
		printf("t_mcast_tap: v2 query answered with a v2 report: %s\n", n > 0 ? "ok" : "FAILED");
	}
	/* drop: v2 leave to 224.0.0.2 */
	CHECK(rt_mship(s, 0, "239.77.0.1") == 0);
	n = igmp_from_riptide(pkt, sizeof(pkt), 2000);
	CHECK(n > 0 && igmp_msg(pkt)[0] == 0x17 && !memcmp(pkt + 16, &(u32){ A("224.0.0.2") }, 4));
	lx_send_udp("239.77.0.1", 7001, "after leave");
	{
		struct rt_timeval tv = { 0, 500000 };
		rt_api_setsockopt(&ctx, s, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	}
	CHECK(rt_api_recv(&ctx, s, buf, sizeof(buf), 0) < 0);
	rt_api_close(&ctx, s);
}

static void test_linux_receives(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1, r, ttl = -1;
	struct sockaddr_in a, from;
	struct ip_mreq mr;
	socklen_t fl = sizeof(from);
	struct timeval tv = { 3, 0 };
	char buf[256], cbuf[256];
	struct rt_sockaddr_in to;
	struct iovec iov = { buf, sizeof(buf) - 1 };
	struct msghdr mh;
	struct cmsghdr *cm;
	int rs;
	u32 ifa = A("10.79.7.2");

	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, IPPROTO_IP, IP_RECVTTL, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(7003);
	a.sin_addr.s_addr = A("239.77.0.3");
	CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
	mr.imr_multiaddr.s_addr = A("239.77.0.3");
	mr.imr_interface.s_addr = A("10.79.7.1");
	CHECK(setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) == 0);

	rs = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(rt_api_setsockopt(&ctx, rs, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &ifa, 4) == 0);
	memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = RT_AF_INET;
	to.sin_port = rt_htons(7003);
	to.sin_addr = A("239.77.0.3");
	CHECK(rt_api_sendto(&ctx, rs, "from riptide", 12, 0, &to, sizeof(to)) == 12);
	memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = fl;
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cbuf;
	mh.msg_controllen = sizeof(cbuf);
	r = (int)recvmsg(s, &mh, 0);
	for (cm = r > 0 ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm))
		if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TTL)
			memcpy(&ttl, CMSG_DATA(cm), sizeof(ttl));
	CHECK(r == 12 && !memcmp(buf, "from riptide", 12));
	CHECK(r == 12 && from.sin_addr.s_addr == A("10.79.7.2"));
	CHECK(ttl == 1);
	printf("t_mcast_tap: Riptide -> 239.77.0.3 received by Linux: %s (TTL %d)\n",
	       r == 12 ? "ok" : "FAILED", ttl);
	/* IP_MULTICAST_TTL 9 */
	{
		u8 t = 9;
		CHECK(rt_api_setsockopt(&ctx, rs, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &t, 1) == 0);
		CHECK(rt_api_sendto(&ctx, rs, "ttl9", 4, 0, &to, sizeof(to)) == 4);
		mh.msg_controllen = sizeof(cbuf);
		ttl = -1;
		r = (int)recvmsg(s, &mh, 0);
		for (cm = r > 0 ? CMSG_FIRSTHDR(&mh) : NULL; cm; cm = CMSG_NXTHDR(&mh, cm))
			if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TTL)
				memcpy(&ttl, CMSG_DATA(cm), sizeof(ttl));
		CHECK(r == 4 && ttl == 9);
	}
	rt_api_close(&ctx, rs);
	close(s);
}

int main(void)
{
	struct sockaddr_ll ll;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_mcast_tap: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	alarm(120);
	if (rt_host_start() != 0)
		return 1;
	snprintf(tap_name, sizeof(tap_name), "rtmc%d", (int)(getpid() % 100000));
	tap = rt_host_tap_open(tap_name, mac);
	if (!tap) {
		printf("t_mcast_tap: SKIP (cannot create TAP device)\n");
		return 77;
	}
	if (sh("ip addr add 10.79.7.1/24 dev %s && ip link set %s up", tap_name, tap_name)) {
		printf("t_mcast_tap: SKIP (cannot configure %s)\n", tap_name);
		return 77;
	}
	psock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (psock < 0) {
		printf("t_mcast_tap: SKIP (no packet socket)\n");
		return 77;
	}
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(ETH_P_IP);
	ll.sll_ifindex = (int)if_nametoindex(tap_name);
	CHECK(bind(psock, (struct sockaddr *)&ll, sizeof(ll)) == 0);
	rt_ctx_init(&ctx, 32, NULL);
	rt_lock();
	CHECK(rt_netif_set_addr(tap, A("10.79.7.2"), A("255.255.255.0"), 0) == 0);
	rt_netif_up(tap);
	rt_unlock();
	usleep(200000);

	test_riptide_receives();
	test_linux_receives();

	rt_ctx_destroy(&ctx);
	rt_host_stop();
	close(psock);
	printf("t_mcast_tap: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
