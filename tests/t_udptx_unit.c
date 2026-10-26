/*
 * Riptide - unit tests of the UDP sender features (0.17): pacing (a UDP
 * socket is writable only while the interface its datagrams leave by has
 * transmit room; EWOULDBLOCK instead of a dropped datagram; a blocking
 * sender sleeps until the driver finishes a frame), SO_NO_CHECK, and
 * rt_api_sendmmsg()/rt_api_recvmmsg(). The fake driver keeps the frames
 * it gets, like a SANA-II device with all its write requests busy, and
 * may never complete them (a stalled driver).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define HELD 64
#define SLOTS 4
static struct rt_buf *held[HELD];
static int nheld;
static struct rt_netif eth0;
static struct rt_drv_stats d0;

static int hold_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	(void)dsthw;
	(void)et;
	rt_drv_tx_start(nif, b->len);
	if (nheld < HELD)
		held[nheld++] = b;
	else
		rt_buf_free(b);
	return 0;
}

/* the driver finishes its oldest frame (lock held) */
static void complete_one(void)
{
	struct rt_buf *b;

	if (!nheld)
		return;
	b = held[0];
	memmove(&held[0], &held[1], (size_t)(nheld - 1) * sizeof(held[0]));
	nheld--;
	rt_buf_free(b);
	rt_drv_tx_done(&eth0, 0);
}

static void complete_all(void)
{
	rt_lock();
	while (nheld)
		complete_one();
	rt_unlock();
}

static u32 A(int a, int b, int c, int d)
{
	return rt_htonl(((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d);
}

static struct rt_sockaddr_in bcast(u16 port)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(port);
	sin.sin_addr = A(10, 9, 0, 255);
	return sin;
}

static int writable(struct rt_sockctx *c, int fd)
{
	u32 wr[8];

	memset(wr, 0, sizeof(wr));
	wr[fd / 32] |= 1u << (fd % 32);
	return rt_api_select(c, fd + 1, NULL, wr, NULL, 0, NULL) == 1;
}

static void setup(void)
{
	rt_lock();
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	eth0.mtu = 1500;
	eth0.flags = RT_IFF_BROADCAST;
	eth0.link_output = hold_output;
	CHECK(rt_netif_add(&eth0) >= 0);
	rt_drv_attach(&eth0, &d0);
	d0.tx_slots = SLOTS;
	rt_ip6_ifenable(&eth0, 0);
	rt_netif_set_addr(&eth0, A(10, 9, 0, 2), A(255, 255, 255, 0), 0);
	rt_netif_up(&eth0);
	rt_unlock();
	complete_all();          /* gratuitous ARP and the like */
}

static int udp_socket(struct rt_sockctx *c, int nbio)
{
	s32 on = 1;
	int fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, 0);

	CHECK(fd >= 0);
	CHECK(rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_BROADCAST, &on, sizeof(on)) == 0);
	if (nbio)
		CHECK(rt_api_ioctl(c, fd, RT_FIONBIO, &on) == 0);
	return fd;
}

static void test_pacing(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9000);
	char data[100];
	u32 noroom0 = rt_udpstat.TxNoRoom;
	int fd = udp_socket(c, 1), i;

	memset(data, 'x', sizeof(data));
	CHECK(writable(c, fd));
	for (i = 0; i < SLOTS; i++)
		CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(nheld == SLOTS);
	/* the driver is full: not writable, and a send would block (nothing
	 * is dropped) */
	CHECK(!writable(c, fd));
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == -RT_EWOULDBLOCK);
	CHECK(nheld == SLOTS && rt_udpstat.TxNoRoom > noroom0);
	CHECK(eth0.tx_waiters == 1);
	/* one frame done: writable again, the waiter flag cleared */
	rt_lock();
	complete_one();
	rt_unlock();
	CHECK(eth0.tx_waiters == 0);
	CHECK(writable(c, fd));
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(nheld == SLOTS);
	complete_all();
	rt_api_close(c, fd);
}

/* a blocking sender sleeps until the driver finishes a frame */
static void *finisher(void *arg)
{
	(void)arg;
	usleep(150000);
	rt_lock();
	complete_one();
	rt_unlock();
	return NULL;
}

static void test_blocking(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9001);
	char data[100];
	pthread_t th;
	double t0, t1;
	struct timespec ts;
	int fd = udp_socket(c, 0), i;

	memset(data, 'y', sizeof(data));
	for (i = 0; i < SLOTS; i++)
		CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	pthread_create(&th, NULL, finisher, NULL);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t0 = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t1 = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
	pthread_join(th, NULL);
	CHECK(t1 - t0 >= 0.1);           /* it waited for the finisher */
	CHECK(nheld == SLOTS);
	complete_all();
	rt_api_close(c, fd);
}

static u16 udp_csum_of(const struct rt_buf *b)
{
	u32 ihl = (u32)(b->data[0] & 15) * 4;
	return rt_get_be16(b->data + ihl + 6);
}

static void test_nocheck(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9002);
	char data[64];
	s32 v = 1;
	u32 len = sizeof(v);
	int fd = udp_socket(c, 1), tcp;

	memset(data, 'z', sizeof(data));
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(nheld == 1 && udp_csum_of(held[0]) != 0);
	complete_all();
	CHECK(rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_NO_CHECK, &v, sizeof(v)) == 0);
	v = 0;
	CHECK(rt_api_getsockopt(c, fd, RT_SOL_SOCKET, RT_SO_NO_CHECK_LINUX, &v, &len) == 0 && v == 1);
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(nheld == 1 && udp_csum_of(held[0]) == 0);
	complete_all();
	v = 0;
	CHECK(rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_NO_CHECK_LINUX, &v, sizeof(v)) == 0);
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(nheld == 1 && udp_csum_of(held[0]) != 0);
	complete_all();
	rt_api_close(c, fd);
	/* datagram sockets only */
	tcp = rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, 0);
	v = 1;
	CHECK(rt_api_setsockopt(c, tcp, RT_SOL_SOCKET, RT_SO_NO_CHECK, &v, sizeof(v)) == -RT_ENOPROTOOPT);
	rt_api_close(c, tcp);
}

static void test_mmsg(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9003), lo, from[4];
	struct rt_iov iov[4];
	struct rt_mmsg m[4];
	char data[4][32], in[4][32];
	u32 mm0 = rt_udpstat.MMsgDatagrams;
	int fd = udp_socket(c, 1), rx, i;

	for (i = 0; i < 4; i++) {
		memset(data[i], 'a' + i, sizeof(data[i]));
		iov[i].base = data[i];
		iov[i].len = (u32)(10 + i);
		memset(&m[i], 0, sizeof(m[i]));
		m[i].iov = &iov[i];
		m[i].iovcnt = 1;
		m[i].name = &to;
		m[i].namelen = sizeof(to);
	}
	/* three of four slots */
	CHECK(rt_api_sendmmsg(c, fd, m, 3, 0) == 3);
	CHECK(m[0].len == 10 && m[2].len == 12 && nheld == 3);
	CHECK(rt_udpstat.MMsgDatagrams == mm0 + 3);
	/* one slot left: a non-blocking socket sends one, then stops */
	CHECK(rt_api_sendmmsg(c, fd, m, 4, 0) == 1 && nheld == SLOTS);
	CHECK(rt_api_sendmmsg(c, fd, m, 4, 0) == -RT_EWOULDBLOCK);
	CHECK(rt_api_sendmmsg(c, fd, m, 0, 0) == 0);
	complete_all();
	rt_api_close(c, fd);

	/* recvmmsg over loopback: three queued, a fourth would block */
	rx = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	memset(&lo, 0, sizeof(lo));
	lo.sin_len = sizeof(lo);
	lo.sin_family = RT_AF_INET;
	lo.sin_port = rt_htons(9004);
	lo.sin_addr = A(127, 0, 0, 1);
	CHECK(rt_api_bind(c, rx, &lo, sizeof(lo)) == 0);
	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	for (i = 0; i < 4; i++) {
		m[i].name = &lo;
		m[i].namelen = sizeof(lo);
	}
	CHECK(rt_api_sendmmsg(c, fd, m, 3, 0) == 3);
	rt_lock();
	rt_stack_poll();         /* the loopback queue */
	rt_unlock();
	for (i = 0; i < 4; i++) {
		iov[i].base = in[i];
		iov[i].len = sizeof(in[i]);
		m[i].name = &from[i];
		m[i].namelen = sizeof(from[i]);
		m[i].len = 0;
	}
	CHECK(rt_api_recvmmsg(c, rx, m, 4, RT_MSG_DONTWAIT) == 3);
	CHECK(m[0].len == 10 && m[1].len == 11 && m[2].len == 12 && in[1][0] == 'b');
	CHECK(m[0].namelen == sizeof(struct rt_sockaddr_in) && from[0].sin_addr == A(127, 0, 0, 1));
	CHECK(rt_api_recvmmsg(c, rx, m, 4, RT_MSG_DONTWAIT) == -RT_EWOULDBLOCK);
	rt_api_close(c, fd);
	rt_api_close(c, rx);
}

/* the interface goes down under a blocked sender: it wakes up */
static void *downer(void *arg)
{
	(void)arg;
	usleep(150000);
	rt_lock();
	rt_netif_down(&eth0);
	rt_unlock();
	return NULL;
}

static void test_down(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9005);
	char data[100];
	pthread_t th;
	int fd = udp_socket(c, 0), i, r;

	memset(data, 'w', sizeof(data));
	for (i = 0; i < SLOTS; i++)
		CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	pthread_create(&th, NULL, downer, NULL);
	r = rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to));
	pthread_join(th, NULL);
	CHECK(r < 0);                    /* returned (not sent: the interface is down) */
	CHECK(nheld == SLOTS && eth0.tx_waiters == 0);
	complete_all();
	rt_lock();
	rt_netif_up(&eth0);
	rt_unlock();
	complete_all();          /* what coming up sent */
	rt_api_close(c, fd);
}

/* The driver stops completing frames (as uaenet.device did in the tools
 * suite: 32 writes outstanding for good). A blocking sender must not
 * sleep forever: after RT_NETIF_TX_STALL_MS the stall timer wakes it and
 * the interface counts as having room again. */
static volatile int poller_stop;

static void *poller(void *arg)
{
	(void)arg;
	while (!poller_stop) {
		rt_lock();
		rt_stack_poll();
		rt_unlock();
		usleep(10000);
	}
	return NULL;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_stall(struct rt_sockctx *c)
{
	struct rt_sockaddr_in to = bcast(9006);
	char data[100];
	pthread_t th;
	double t0, t1;
	int fd = udp_socket(c, 0), nb = udp_socket(c, 1), i, r;

	memset(data, 's', sizeof(data));
	for (i = 0; i < SLOTS; i++)
		CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	/* not stalled yet: a non-blocking sender still waits for room */
	CHECK(rt_api_sendto(c, nb, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(rt_api_sendto(c, nb, data, sizeof(data), 0, &to, sizeof(to)) == -RT_EWOULDBLOCK);
	CHECK(!rt_netif_tx_stalled(&eth0));
	poller_stop = 0;
	pthread_create(&th, NULL, poller, NULL);
	t0 = now_s();
	r = rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to));
	t1 = now_s();
	CHECK(r == (int)sizeof(data));   /* sent (the fake driver takes it) */
	CHECK(t1 - t0 >= 0.8 && t1 - t0 < 5.0);
	CHECK(rt_netif_tx_stalled(&eth0));
	/* stalled: no more waiting, writable at once */
	CHECK(writable(c, nb));
	t0 = now_s();
	CHECK(rt_api_sendto(c, fd, data, sizeof(data), 0, &to, sizeof(to)) == (int)sizeof(data));
	CHECK(now_s() - t0 < 0.5);
	poller_stop = 1;
	pthread_join(th, NULL);
	/* the driver recovers: pacing again */
	complete_all();
	CHECK(!rt_netif_tx_stalled(&eth0));
	rt_api_close(c, fd);
	rt_api_close(c, nb);
}

int main(void)
{
	struct rt_sockctx c;

	CHECK(rt_init() == 0);
	rt_log_level = 0;
	setup();
	rt_ctx_init(&c, 16, NULL);
	test_pacing(&c);
	test_blocking(&c);
	test_nocheck(&c);
	test_mmsg(&c);
	test_down(&c);
	test_stall(&c);
	printf("t_udptx_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
