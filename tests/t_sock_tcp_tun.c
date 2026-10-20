/*
 * Riptide - socket layer semantics with real TCP against Linux over TUN
 * (needs root, /dev/net/tun and the TCP module; exits 77 otherwise):
 *   Linux 10.80.1.1/30  <-- tun -->  Riptide 10.80.1.2
 * blocking / non-blocking connect and accept, refused connections and
 * SO_ERROR, select on connect completion and accept, large blocking
 * sends (partial send loop), MSG_WAITALL, shutdown/EOF, SO_LINGER 0 reset.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
/* no weak TCP stubs: this test needs the real TCP objects */

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define OUR_IP   0x0a500102u
#define LINUX_IP 0x0a500101u
#define BIG      (256 * 1024)

static struct rt_sockctx ctx;
static u8 *bigbuf, *rxbuf;

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

static int lx_listen(u16 port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = rt_htonl(LINUX_IP);
	a.sin_port = rt_htons(port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0)
		return -1;
	return fd;
}

static int lx_accept(int l)
{
	struct pollfd p = { l, POLLIN, 0 };
	if (poll(&p, 1, 5000) != 1)
		return -1;
	return accept(l, NULL, NULL);
}

static int lx_connect(u16 port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = rt_htonl(OUR_IP);
	a.sin_port = rt_htons(port);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int lx_fd_reader;
static volatile long lx_read_total;
static void *lx_reader(void *arg)
{
	long got = 0;
	ssize_t n;
	while ((n = read(lx_fd_reader, rxbuf + (got % BIG), (size_t)(BIG - got % BIG))) > 0)
		got += n;
	lx_read_total = got;
	return NULL;
}

static void *lx_writer(void *arg)
{
	int fd = *(int *)arg;
	long sent = 0;
	while (sent < BIG) {
		ssize_t n = write(fd, bigbuf + sent, (size_t)RT_MIN(4096, BIG - sent));
		if (n <= 0)
			break;
		sent += n;
		usleep(500);
	}
	return NULL;
}

int main(void)
{
	struct rt_netif *tun;
	pthread_t th;
	char name[16], cmd[256], buf[256];
	int lfd, fd, lx, r, i, on = 1;
	u32 len, wr[1], rd[1];
	s32 err;

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_sock_tcp_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	if (!rt_tcp_ops.connect || !rt_tcp_ops.attach) {
		printf("t_sock_tcp_tun: SKIP (TCP module not linked)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rtt%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffcu));
	if (!tun) {
		printf("t_sock_tcp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(cmd, sizeof(cmd), "ip addr add 10.80.1.1/30 dev %s && ip link set %s up", name, name);
	if (system(cmd) != 0) {
		printf("t_sock_tcp_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}
	bigbuf = malloc(BIG);
	rxbuf = malloc(BIG);
	for (i = 0; i < BIG; i++)
		bigbuf[i] = (u8)(i * 7 + (i >> 9));
	CHECK(rt_ctx_init(&ctx, 32, NULL) == 0);

	/* 1. blocking connect to Linux, data both ways, big blocking send */
	lfd = lx_listen(7000);
	CHECK(lfd >= 0);
	{
		struct rt_sockaddr_in to = rsa(LINUX_IP, 7000), pa;
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == 0);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == -RT_EISCONN);
		len = sizeof(pa);
		CHECK(rt_api_getpeername(&ctx, fd, &pa, &len) == 0 && pa.sin_port == rt_htons(7000));
		lx = lx_accept(lfd);
		CHECK(lx >= 0);
		CHECK(rt_api_send(&ctx, fd, "ping", 4, 0) == 4);
		CHECK(read(lx, buf, sizeof(buf)) == 4);
		CHECK(write(lx, "pong", 4) == 4);
		CHECK(rt_api_recv(&ctx, fd, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "pong", 4));
		/* Riptide -> Linux 256 KB in one blocking send */
		lx_fd_reader = lx;
		pthread_create(&th, NULL, lx_reader, NULL);
		r = rt_api_send(&ctx, fd, bigbuf, BIG, 0);
		CHECK(r == BIG);
		CHECK(rt_api_shutdown(&ctx, fd, 1) == 0);
		CHECK(rt_api_send(&ctx, fd, "x", 1, 0) == -RT_EPIPE);
		pthread_join(th, NULL);
		CHECK(lx_read_total == BIG && !memcmp(rxbuf, bigbuf, BIG));
		/* Linux -> Riptide with MSG_WAITALL */
		pthread_create(&th, NULL, lx_writer, &lx);
		r = rt_api_recv(&ctx, fd, rxbuf, BIG, RT_MSG_WAITALL);
		CHECK(r == BIG && !memcmp(rxbuf, bigbuf, BIG));
		pthread_join(th, NULL);
		close(lx);
		CHECK(rt_api_recv(&ctx, fd, buf, sizeof(buf), 0) == 0);       /* EOF */
		rt_api_close(&ctx, fd);
	}

	/* 2. non-blocking connect, select for completion */
	{
		struct rt_sockaddr_in to = rsa(LINUX_IP, 7000);
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_ioctl(&ctx, fd, RT_FIONBIO, &on) == 0);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == -RT_EINPROGRESS);
		r = rt_api_connect(&ctx, fd, &to, sizeof(to));
		CHECK(r == -RT_EALREADY || r == -RT_EISCONN);
		wr[0] = 1u << fd;
		CHECK(rt_api_select(&ctx, fd + 1, NULL, wr, NULL, 3000, NULL) == 1);
		len = 4;
		CHECK(rt_api_getsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &len) == 0 && err == 0);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == -RT_EISCONN);
		lx = lx_accept(lfd);
		CHECK(lx >= 0);
		CHECK(rt_api_recv(&ctx, fd, buf, 4, 0) == -RT_EWOULDBLOCK);
		/* SO_LINGER {1,0}: close sends RST */
		{
			struct rt_linger lg = { 1, 0 };
			CHECK(rt_api_setsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof(lg)) == 0);
		}
		rt_api_close(&ctx, fd);
		{
			struct pollfd p = { lx, POLLIN, 0 };
			poll(&p, 1, 3000);
			errno = 0;
			r = (int)read(lx, buf, sizeof(buf));
			CHECK(r < 0 && errno == ECONNRESET);
		}
		close(lx);
	}
	close(lfd);

	/* 3. refused: blocking and non-blocking */
	{
		struct rt_sockaddr_in to = rsa(LINUX_IP, 7999);
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == -RT_ECONNREFUSED);
		rt_api_close(&ctx, fd);
		fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		rt_api_ioctl(&ctx, fd, RT_FIONBIO, &on);
		CHECK(rt_api_connect(&ctx, fd, &to, sizeof(to)) == -RT_EINPROGRESS);
		wr[0] = 1u << fd;
		CHECK(rt_api_select(&ctx, fd + 1, NULL, wr, NULL, 3000, NULL) == 1);
		len = 4;
		CHECK(rt_api_getsockopt(&ctx, fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &len) == 0 &&
		      err == RT_ECONNREFUSED);
		rt_api_close(&ctx, fd);
	}

	/* 4. Riptide listener: non-blocking and blocking accept */
	{
		struct rt_sockaddr_in me = rsa(0, 7001), pa;
		int c1, c2, a1, a2;
		lfd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
		CHECK(rt_api_setsockopt(&ctx, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &on, 4) == 0);
		CHECK(rt_api_bind(&ctx, lfd, &me, sizeof(me)) == 0);
		CHECK(rt_api_listen(&ctx, lfd, 4) == 0);
		rt_api_ioctl(&ctx, lfd, RT_FIONBIO, &on);
		CHECK(rt_api_accept(&ctx, lfd, NULL, NULL) == -RT_EWOULDBLOCK);
		c1 = lx_connect(7001);
		CHECK(c1 >= 0);
		rd[0] = 1u << lfd;
		CHECK(rt_api_select(&ctx, lfd + 1, rd, NULL, NULL, 3000, NULL) == 1);
		len = sizeof(pa);
		a1 = rt_api_accept(&ctx, lfd, &pa, &len);
		CHECK(a1 >= 0 && pa.sin_addr == rt_htonl(LINUX_IP) && len == 16);
		{
			int off = 0;
			rt_api_ioctl(&ctx, lfd, RT_FIONBIO, &off);
		}
		c2 = lx_connect(7001);
		a2 = rt_api_accept(&ctx, lfd, NULL, NULL);            /* blocking */
		CHECK(c2 >= 0 && a2 >= 0);
		CHECK(write(c2, "hey", 3) == 3);
		{
			int off = 0;
			rt_api_ioctl(&ctx, a2, RT_FIONBIO, &off);
		}
		CHECK(rt_api_recv(&ctx, a2, buf, sizeof(buf), 0) == 3);
		/* FIONREAD on TCP */
		CHECK(write(c1, "12345", 5) == 5);
		usleep(100000);
		err = 0;
		CHECK(rt_api_ioctl(&ctx, a1, RT_FIONREAD, &err) == 0 && err == 5);
		/* peer closes: recv 0, later sends fail */
		close(c1);
		{
			int off = 0;
			struct rt_timeval tv = { 2, 0 };
			rt_api_ioctl(&ctx, a1, RT_FIONBIO, &off);
			rt_api_setsockopt(&ctx, a1, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
		}
		CHECK(rt_api_recv(&ctx, a1, buf, sizeof(buf), 0) == 5);
		CHECK(rt_api_recv(&ctx, a1, buf, sizeof(buf), 0) == 0);
		rt_api_send(&ctx, a1, "a", 1, 0);
		usleep(100000);
		r = rt_api_send(&ctx, a1, "b", 1, 0);
		CHECK(r == -RT_EPIPE || r == -RT_ECONNRESET);
		close(c2);
		rt_api_close(&ctx, a1);
		rt_api_close(&ctx, a2);
		rt_api_close(&ctx, lfd);
	}

	rt_ctx_destroy(&ctx);
	rt_host_stop();
	free(bigbuf);
	free(rxbuf);
	printf("t_sock_tcp_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
