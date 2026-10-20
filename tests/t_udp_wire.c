/*
 * Riptide - two stack instances (two processes) on a simulated Ethernet
 * wire: ARP resolution, UDP echo, and the impairment layer (loss,
 * duplication, reordering, delay, rate limit) with a fixed seed.
 * No privileges needed.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define A_IP 0xc0a84d01u     /* 192.168.77.1 */
#define B_IP 0xc0a84d02u     /* 192.168.77.2 */
#define PORT 9000

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 0, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 0, 0x0b };

static struct rt_sockaddr_in sa(u32 ip, u16 port)
{
	struct rt_sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_addr = rt_htonl(ip);
	s.sin_port = rt_htons(port);
	return s;
}

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, u32 ip)
{
	struct rt_netif *n;

	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	rt_lock();
	rt_netif_set_addr(n, rt_htonl(ip), rt_htonl(0xffffff00u), 0);
	rt_netif_up(n);
	rt_unlock();
	return n;
}

/* Process B: UDP echo server until "quit". */
static int run_b(const char *pa, const char *pb)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = sa(0, PORT), from;
	static char buf[4096];
	int fd;

	alarm(60);
	if (!bring_up(pb, pa, mac_b, B_IP))
		return 2;
	rt_ctx_init(&c, 8, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	{
		int big = 512 * 1024;
		rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_RCVBUF, &big, sizeof(big));
	}
	if (rt_api_bind(&c, fd, &a, sizeof(a)) != 0)
		return 3;
	for (;;) {
		u32 fl = sizeof(from);
		int r = rt_api_recvfrom(&c, fd, buf, sizeof(buf), 0, &from, &fl);
		if (r == 4 && memcmp(buf, "quit", 4) == 0)
			break;
		if (r >= 0)
			rt_api_sendto(&c, fd, buf, (u32)r, 0, &from, fl);
	}
	rt_host_stop();
	return 0;
}

/* Send n datagrams of 'len' bytes (seq in the first 4 bytes), collect
 * echoes until 'wait_ms' after the last send; returns unique replies. */
static int burst(struct rt_sockctx *c, int fd, int n, u32 len, u32 wait_ms, u32 *dups, u32 *ooo)
{
	static char buf[4096];
	static u8 seen[2048];
	struct rt_sockaddr_in b = sa(B_IP, PORT);
	struct rt_timeval tv = { 0, 100000 };
	u32 last_seq = 0, end;
	int i, got = 0;

	memset(seen, 0, sizeof(seen));
	memset(buf, 0x5a, sizeof(buf));
	*dups = *ooo = 0;
	rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	for (i = 0; i < n; i++) {
		rt_put_be32(buf, (u32)i + 1);
		rt_api_sendto(c, fd, buf, len, 0, &b, sizeof(b));
	}
	end = rt_now_ms() + wait_ms;
	while ((s32)(rt_now_ms() - end) < 0) {
		int r = rt_api_recv(c, fd, buf, sizeof(buf), 0);
		u32 s;
		if (r < 4)
			continue;
		s = rt_get_be32(buf);
		if (s == 0 || s > (u32)n || s >= sizeof(seen))
			continue;
		if (seen[s]) {
			(*dups)++;
			continue;
		}
		if (s < last_seq)
			(*ooo)++;
		last_seq = s;
		seen[s] = 1;
		got++;
		if (got == n)
			break;
	}
	return got;
}

int main(void)
{
	char pa[64], pb[64];
	struct rt_netif *nif;
	struct rt_sockctx c;
	struct rt_host_impair im;
	struct rt_host_impair_stats st;
	struct rt_arp_entry_info arp[8];
	u32 dups, ooo, t0;
	pid_t child;
	int fd, got, i, n, status = 0;

	snprintf(pa, sizeof(pa), "/tmp/rt_wire_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_wire_%d_b", (int)getpid());
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(run_b(pa, pb));

	alarm(90);
	nif = bring_up(pa, pb, mac_a, A_IP);
	CHECK(nif != NULL);
	if (!nif)
		goto out;
	rt_ctx_init(&c, 8, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	{
		int big = 512 * 1024;
		rt_api_setsockopt(&c, fd, RT_SOL_SOCKET, RT_SO_RCVBUF, &big, sizeof(big));
	}

	/* clean wire: everything echoed; B may still be starting, so retry
	 * the first exchange (it also drives ARP resolution) */
	for (i = 0, got = 0; i < 20 && got == 0; i++)
		got = burst(&c, fd, 1, 32, 300, &dups, &ooo);
	CHECK(got == 1);
	got = burst(&c, fd, 200, 200, 2000, &dups, &ooo);
	CHECK(got == 200 && dups == 0);
	if (got != 200)
		fprintf(stderr, "clean burst: %d/200; udp in %u rcvbuf %u noport %u; if ipk %u oerr %u iqdrop %u; ip in %u disc %u\n",
		        got, rt_udpstat.InDatagrams, rt_udpstat.RcvbufErrors, rt_udpstat.NoPorts,
		        nif->stats.ipackets, nif->stats.oerrors, nif->stats.iqdrops, rt_ipstat.InReceives,
		        rt_ipstat.InDiscards);
	rt_lock();
	n = rt_arp_list(arp, 8);
	rt_unlock();
	CHECK(n >= 1);
	for (i = 0; i < n && i < 8; i++)
		if (arp[i].ip == rt_htonl(B_IP))
			CHECK(arp[i].state == 1 && memcmp(arp[i].hw, mac_b, 6) == 0);

	/* impaired: 10% loss, 5% duplicates, 10% reordered, 5+-5 ms delay */
	memset(&im, 0, sizeof(im));
	im.loss = 1000;
	im.duplicate = 500;
	im.reorder = 1000;
	im.reorder_ms = 15;
	im.delay_ms = 5;
	im.jitter_ms = 5;
	im.seed = 4242;
	rt_host_impair(nif, &im);
	got = burst(&c, fd, 400, 100, 2000, &dups, &ooo);
	rt_host_impair_get_stats(nif, &st);
	printf("impaired: %d/400 echoed, %u dups, %u out of order; dropped %u dup %u reord %u\n",
	       got, dups, ooo, st.dropped, st.duplicated, st.reordered);
	CHECK(got >= 300 && got <= 390);
	CHECK(st.dropped >= 20 && st.duplicated >= 5 && st.reordered >= 15);
	CHECK(dups >= 1 && ooo >= 1);

	/* rate limit: 40 x 1250 bytes at 1000 kbit/s takes >= ~400 ms */
	memset(&im, 0, sizeof(im));
	im.rate_kbit = 1000;
	im.seed = 1;
	rt_host_impair(nif, &im);
	t0 = rt_now_ms();
	got = burst(&c, fd, 40, 1250 - 28 - 14, 3000, &dups, &ooo);
	t0 = rt_now_ms() - t0;
	printf("rate limited: %d/40 echoed in %u ms\n", got, t0);
	CHECK(got == 40 && t0 >= 350 && t0 < 2000);
	rt_host_impair(nif, NULL);

	{
		struct rt_sockaddr_in b = sa(B_IP, PORT);
		rt_api_sendto(&c, fd, "quit", 4, 0, &b, sizeof(b));
	}
	rt_api_close(&c, fd);
	rt_ctx_destroy(&c);
	rt_host_stop();
out:
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(pa);
	unlink(pb);
	printf("t_udp_wire: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
