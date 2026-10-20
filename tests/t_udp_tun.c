/*
 * Riptide - UDP interop with Linux sockets over TUN (needs root and
 * /dev/net/tun; exits 77 = skip otherwise).
 *   Linux 10.77.1.1/30  <-- tun -->  Riptide 10.77.1.2
 * A Riptide echo server (rt_api_* sockets) answers a Linux UDP client,
 * with small and fragmented datagrams, and Riptide sends to a Linux
 * server; source addresses and ports are checked on both sides.
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

#define OUR_IP   0x0a4d0102u
#define LINUX_IP 0x0a4d0101u
#define ECHO_PORT 7777

static struct rt_sockctx ctx;
static int echo_fd;
static volatile int echo_count;

/* Riptide echo server thread: blocking recvfrom/sendto via the API. */
static void *echo_main(void *arg)
{
	static char buf[9216];
	(void)arg;
	for (;;) {
		struct rt_sockaddr_in from;
		u32 flen = sizeof(from);
		int r = rt_api_recvfrom(&ctx, echo_fd, buf, sizeof(buf), 0, &from, &flen);
		if (r < 0) {
			if (r == -RT_EWOULDBLOCK || r == -RT_EINTR)
				continue;
			break;           /* socket closed */
		}
		if (r == 4 && memcmp(buf, "quit", 4) == 0)
			break;
		if (rt_api_sendto(&ctx, echo_fd, buf, (u32)r, 0, &from, flen) == r)
			echo_count++;
	}
	return NULL;
}

static int linux_udp(u16 port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = rt_htonl(LINUX_IP);
	a.sin_port = rt_htons(port);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		return -1;
	return fd;
}

static ssize_t recv_wait(int fd, void *buf, size_t len, struct sockaddr_in *from)
{
	struct pollfd p = { fd, POLLIN, 0 };
	socklen_t fl = sizeof(*from);

	if (poll(&p, 1, 3000) <= 0)
		return -1;
	return recvfrom(fd, buf, len, 0, (struct sockaddr *)from, &fl);
}

int main(void)
{
	static char out[8000], in[9000];
	struct rt_netif *tun;
	struct rt_sockaddr_in a;
	struct sockaddr_in to, from;
	pthread_t th;
	char name[16], cmd[256];
	int lfd, i, n, ok;

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_udp_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rtu%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffc));
	if (!tun) {
		printf("t_udp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(cmd, sizeof(cmd), "ip addr add 10.77.1.1/30 dev %s && ip link set %s up", name, name);
	if (system(cmd) != 0) {
		printf("t_udp_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}

	CHECK(rt_ctx_init(&ctx, 16, NULL) == 0);
	echo_fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(ECHO_PORT);
	CHECK(rt_api_bind(&ctx, echo_fd, &a, sizeof(a)) == 0);
	pthread_create(&th, NULL, echo_main, NULL);

	/* Linux client -> Riptide echo server */
	lfd = linux_udp(40000);
	CHECK(lfd >= 0);
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = rt_htonl(OUR_IP);
	to.sin_port = rt_htons(ECHO_PORT);
	ok = 0;
	for (i = 0; i < 20; i++) {
		int len = i < 15 ? 1 + i * 37 : 1400 + (i - 15) * 1500;   /* last ones fragment */
		int j;
		for (j = 0; j < len; j++)
			out[j] = (char)(j * 13 + i);
		sendto(lfd, out, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to));
		n = (int)recv_wait(lfd, in, sizeof(in), &from);
		if (n == len && memcmp(in, out, (size_t)len) == 0 &&
		    from.sin_addr.s_addr == rt_htonl(OUR_IP) && from.sin_port == rt_htons(ECHO_PORT))
			ok++;
	}
	CHECK(ok == 20);
	CHECK(rt_udpstat.InDatagrams >= 20 && rt_udpstat.InCsumErrors == 0);

	/* Riptide client -> Linux server, unconnected and connected */
	{
		int lsrv = linux_udp(40001), cs;
		struct rt_sockaddr_in d;
		memset(&d, 0, sizeof(d));
		d.sin_len = sizeof(d);
		d.sin_family = RT_AF_INET;
		d.sin_addr = rt_htonl(LINUX_IP);
		d.sin_port = rt_htons(40001);
		cs = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
		CHECK(rt_api_sendto(&ctx, cs, "from riptide", 12, 0, &d, sizeof(d)) == 12);
		n = (int)recv_wait(lsrv, in, sizeof(in), &from);
		CHECK(n == 12 && memcmp(in, "from riptide", 12) == 0 && from.sin_addr.s_addr == rt_htonl(OUR_IP) &&
		      rt_ntohs(from.sin_port) >= 49152);
		/* 6000 bytes: Riptide fragments, Linux reassembles */
		CHECK(rt_api_sendto(&ctx, cs, out, 6000, 0, &d, sizeof(d)) == 6000);
		n = (int)recv_wait(lsrv, in, sizeof(in), &from);
		CHECK(n == 6000 && memcmp(in, out, 6000) == 0);
		/* Linux answers to the ephemeral port */
		sendto(lsrv, "pong", 4, 0, (struct sockaddr *)&from, sizeof(from));
		{
			struct rt_timeval tv = { 3, 0 };
			rt_api_setsockopt(&ctx, cs, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		}
		CHECK(rt_api_recv(&ctx, cs, in, sizeof(in), 0) == 4 && memcmp(in, "pong", 4) == 0);
		rt_api_close(&ctx, cs);
		close(lsrv);
	}

	/* stop the echo thread */
	sendto(lfd, "quit", 4, 0, (struct sockaddr *)&to, sizeof(to));
	pthread_join(th, NULL);
	close(lfd);
	rt_api_close(&ctx, echo_fd);
	rt_ctx_destroy(&ctx);
	rt_host_stop();
	printf("t_udp_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
