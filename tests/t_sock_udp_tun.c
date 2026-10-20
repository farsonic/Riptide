/*
 * Riptide - socket API with real UDP over TUN against Linux sockets (needs
 * root and /dev/net/tun; exits 77 otherwise):
 *   Linux 10.79.1.1/30  <-- tun -->  Riptide 10.79.1.2
 * select() over several sockets with timeouts and wakeups from Linux
 * traffic, echo, MSG_PEEK/MSG_TRUNC/FIONREAD, SO_RCVTIMEO, non-blocking,
 * connected UDP + ICMP port unreachable, interface ioctls.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define OUR_IP   0x0a4f0102u
#define LINUX_IP 0x0a4f0101u

static struct rt_sockctx ctx;

static struct rt_sockaddr_in rsa(u32 ip, u16 port)
{
	struct rt_sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = rt_htonl(ip);
	s.sin_port = rt_htons(port);
	return s;
}

static int rt_udp(u16 port)
{
	struct rt_sockaddr_in s = rsa(0, port);
	int fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (fd >= 0 && rt_api_bind(&ctx, fd, &s, sizeof(s)) < 0)
		return -1;
	return fd;
}

static int lx_fd;
static void lx_send(u16 port, const char *msg, u32 len)
{
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = rt_htonl(OUR_IP);
	to.sin_port = rt_htons(port);
	sendto(lx_fd, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void *later_send(void *arg)
{
	usleep(60000);
	lx_send(5002, "later", 5);
	return NULL;
}

int main(void)
{
	struct rt_netif *tun;
	struct sockaddr_in la;
	struct rt_sockaddr_in from, dst;
	pthread_t th;
	char name[16], cmd[256], buf[2048];
	int s1, s2, s3, n, r, mf;
	u32 rd[2], fl, t0;
	s32 v;

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_sock_udp_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rts%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffcu));
	if (!tun) {
		printf("t_sock_udp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(cmd, sizeof(cmd), "ip addr add 10.79.1.1/30 dev %s && ip link set %s up", name, name);
	if (system(cmd) != 0) {
		printf("t_sock_udp_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}
	lx_fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&la, 0, sizeof(la));
	la.sin_family = AF_INET;
	la.sin_addr.s_addr = rt_htonl(LINUX_IP);
	la.sin_port = rt_htons(6000);
	CHECK(bind(lx_fd, (struct sockaddr *)&la, sizeof(la)) == 0);

	CHECK(rt_ctx_init(&ctx, 32, NULL) == 0);
	s1 = rt_udp(5001);
	s2 = rt_udp(5002);
	s3 = rt_udp(5003);
	CHECK(s1 >= 0 && s2 >= 0 && s3 >= 0);
	CHECK(rt_udp(5001) == -1);    /* EADDRINUSE */

	/* select timeout with nothing to read */
	rd[0] = (1u << s1) | (1u << s2) | (1u << s3);
	t0 = rt_now_ms();
	CHECK(rt_api_select(&ctx, 32, rd, NULL, NULL, 80, NULL) == 0 && rd[0] == 0);
	CHECK(rt_now_ms() - t0 >= 75);
	/* Linux traffic wakes a sleeping select */
	pthread_create(&th, NULL, later_send, NULL);
	rd[0] = (1u << s1) | (1u << s2) | (1u << s3);
	r = rt_api_select(&ctx, 32, rd, NULL, NULL, 3000, NULL);
	CHECK(r == 1 && rd[0] == (1u << s2));
	pthread_join(th, NULL);
	/* FIONREAD, PEEK, then the real read with source address */
	v = 0;
	CHECK(rt_api_ioctl(&ctx, s2, RT_FIONREAD, &v) == 0 && v == 5);
	CHECK(rt_api_recv(&ctx, s2, buf, sizeof(buf), RT_MSG_PEEK) == 5);
	fl = sizeof(from);
	CHECK(rt_api_recvfrom(&ctx, s2, buf, sizeof(buf), 0, &from, &fl) == 5 && !memcmp(buf, "later", 5));
	CHECK(fl == 16 && from.sin_addr == rt_htonl(LINUX_IP) && from.sin_port == rt_htons(6000) &&
	      from.sin_family == RT_AF_INET);
	/* two sockets ready at once */
	lx_send(5001, "a", 1);
	lx_send(5003, "c", 1);
	usleep(50000);
	rd[0] = (1u << s1) | (1u << s2) | (1u << s3);
	CHECK(rt_api_select(&ctx, 32, rd, NULL, NULL, 1000, NULL) == 2 && rd[0] == ((1u << s1) | (1u << s3)));
	CHECK(rt_api_recv(&ctx, s1, buf, 1, 0) == 1);
	CHECK(rt_api_recv(&ctx, s3, buf, 1, 0) == 1);
	/* writable */
	rd[0] = 1u << s1;
	CHECK(rt_api_select(&ctx, 32, NULL, rd, NULL, 0, NULL) == 1);
	/* MSG_TRUNC */
	lx_send(5001, "0123456789", 10);
	{
		struct rt_iov iov = { buf, 4 };
		usleep(30000);
		mf = 0;
		CHECK(rt_api_recvmsg(&ctx, s1, &iov, 1, 0, NULL, NULL, &mf) == 4 && (mf & RT_MSG_TRUNC));
	}
	/* SO_RCVTIMEO and non-blocking */
	{
		struct rt_timeval tv = { 0, 100000 };
		int on = 1;
		CHECK(rt_api_setsockopt(&ctx, s1, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
		t0 = rt_now_ms();
		CHECK(rt_api_recv(&ctx, s1, buf, 10, 0) == -RT_EWOULDBLOCK && rt_now_ms() - t0 >= 95);
		CHECK(rt_api_ioctl(&ctx, s3, RT_FIONBIO, &on) == 0);
		CHECK(rt_api_recv(&ctx, s3, buf, 10, 0) == -RT_EWOULDBLOCK);
	}
	/* echo to Linux */
	dst = rsa(LINUX_IP, 6000);
	CHECK(rt_api_sendto(&ctx, s3, "hello", 5, 0, &dst, sizeof(dst)) == 5);
	{
		struct pollfd p = { lx_fd, POLLIN, 0 };
		struct sockaddr_in f;
		socklen_t sl = sizeof(f);
		CHECK(poll(&p, 1, 2000) == 1);
		n = (int)recvfrom(lx_fd, buf, sizeof(buf), 0, (struct sockaddr *)&f, &sl);
		CHECK(n == 5 && f.sin_port == rt_htons(5003) && f.sin_addr.s_addr == rt_htonl(OUR_IP));
	}
	/* connected UDP to a closed Linux port -> ECONNREFUSED */
	{
		int c = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
		struct rt_sockaddr_in closed = rsa(LINUX_IP, 6999), me;
		struct rt_timeval tv = { 2, 0 };
		u32 ml = sizeof(me);
		CHECK(rt_api_connect(&ctx, c, &closed, sizeof(closed)) == 0);
		CHECK(rt_api_getsockname(&ctx, c, &me, &ml) == 0 && me.sin_addr == rt_htonl(OUR_IP) && me.sin_port);
		CHECK(rt_api_getpeername(&ctx, c, &me, &ml) == 0 && me.sin_port == rt_htons(6999));
		rt_api_setsockopt(&ctx, c, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		CHECK(rt_api_send(&ctx, c, "x", 1, 0) == 1);
		r = rt_api_recv(&ctx, c, buf, 10, 0);
		CHECK(r == -RT_ECONNREFUSED);
		/* dissolve the association with AF_UNSPEC */
		memset(&closed, 0, sizeof(closed));
		CHECK(rt_api_connect(&ctx, c, &closed, sizeof(closed)) == 0);
		CHECK(rt_api_getpeername(&ctx, c, &me, &ml) == -RT_ENOTCONN);
		rt_api_close(&ctx, c);
	}

	/* interface ioctls */
	{
		struct rt_ifreq ifrs[8], ifr;
		struct rt_ifconf ifc;
		struct rt_sockaddr_in *sin;
		int i, found = 0, lo = 0;
		ifc.ifc_len = sizeof(ifrs);
		ifc.ifc_buf = ifrs;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFCONF, &ifc) == 0);
		CHECK(ifc.ifc_len > 0 && ifc.ifc_len % (int)sizeof(struct rt_ifreq) == 0);
		for (i = 0; i < ifc.ifc_len / (int)sizeof(struct rt_ifreq); i++) {
			sin = (struct rt_sockaddr_in *)&ifrs[i].ifr_ifru.ifru_addr;
			if (!strcmp(ifrs[i].ifr_name, name) && sin->sin_addr == rt_htonl(OUR_IP) &&
			    sin->sin_family == RT_AF_INET)
				found = 1;
			if (sin->sin_addr == RT_INADDR_LOOPBACK)
				lo = 1;
		}
		CHECK(found && lo);
		ifc.ifc_len = sizeof(struct rt_ifreq) + 4;      /* room for one */
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFCONF, &ifc) == 0 && ifc.ifc_len == sizeof(struct rt_ifreq));

		memset(&ifr, 0, sizeof(ifr));
		strcpy(ifr.ifr_name, name);
		sin = (struct rt_sockaddr_in *)&ifr.ifr_ifru.ifru_addr;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFADDR, &ifr) == 0 && sin->sin_addr == rt_htonl(OUR_IP));
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFNETMASK, &ifr) == 0 && sin->sin_addr == rt_htonl(0xfffffffcu));
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFDSTADDR, &ifr) == 0 && sin->sin_addr == rt_htonl(LINUX_IP));
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFBRDADDR, &ifr) == -RT_EINVAL);
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFFLAGS, &ifr) == 0 &&
		      (ifr.ifr_ifru.ifru_flags & (RT_IFF_UP | RT_IFF_POINTOPOINT)) == (RT_IFF_UP | RT_IFF_POINTOPOINT));
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFMTU, &ifr) == 0 && ifr.ifr_ifru.ifru_mtu == tun->mtu);
		ifr.ifr_ifru.ifru_mtu = 1280;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFMTU, &ifr) == 0 && tun->mtu == 1280);
		ifr.ifr_ifru.ifru_mtu = 20;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFMTU, &ifr) == -RT_EINVAL);
		ifr.ifr_ifru.ifru_metric = 3;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFMETRIC, &ifr) == 0);
		ifr.ifr_ifru.ifru_metric = 0;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFMETRIC, &ifr) == 0 && ifr.ifr_ifru.ifru_metric == 3);
		strcpy(ifr.ifr_name, "nosuch9");
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFADDR, &ifr) == -RT_ENXIO);

		/* lo0: delete and re-add the address; netmask */
		memset(&ifr, 0, sizeof(ifr));
		strcpy(ifr.ifr_name, rt_loopback_if->name);
		sin->sin_len = 16;
		sin->sin_family = RT_AF_INET;
		sin->sin_addr = RT_INADDR_LOOPBACK;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCDIFADDR, &ifr) == 0 && rt_loopback_if->addr == 0);
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFADDR, &ifr) == -RT_EADDRNOTAVAIL);
		sin->sin_addr = rt_htonl(0xff000000u);
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFNETMASK, &ifr) == 0);
		sin->sin_addr = RT_INADDR_LOOPBACK;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFADDR, &ifr) == 0 && rt_loopback_if->addr == RT_INADDR_LOOPBACK &&
		      rt_loopback_if->netmask == rt_htonl(0xff000000u));
		/* flags: down and up again */
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCGIFFLAGS, &ifr) == 0);
		ifr.ifr_ifru.ifru_flags &= ~RT_IFF_UP;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFFLAGS, &ifr) == 0 && !(rt_loopback_if->flags & RT_IFF_UP));
		ifr.ifr_ifru.ifru_flags |= RT_IFF_UP;
		CHECK(rt_api_ioctl(&ctx, s1, RT_SIOCSIFFLAGS, &ifr) == 0 && (rt_loopback_if->flags & RT_IFF_UP) &&
		      (rt_loopback_if->flags & RT_IFF_LOOPBACK));
	}
	/* loopback still works after the dance */
	{
		struct rt_sockaddr_in lo = rsa(0x7f000001u, 5001);
		CHECK(rt_api_sendto(&ctx, s3, "lo", 2, 0, &lo, sizeof(lo)) == 2);
		CHECK(rt_api_recv(&ctx, s1, buf, 10, 0) == 2);
	}

	rt_ctx_destroy(&ctx);
	close(lx_fd);
	rt_host_stop();
	printf("t_sock_udp_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
