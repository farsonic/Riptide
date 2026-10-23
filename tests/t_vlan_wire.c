/*
 * Riptide - 802.1Q VLAN interfaces between two stack instances (two
 * processes) on a simulated Ethernet wire. Both have the parent untagged
 * (192.168.77.0/24), VLAN 10 (10.10.0.0/24) and VLAN 20 (10.20.0.0/24),
 * made with config lines. Checks: UDP echo untagged and on VLAN 10, a
 * 1 MB TCP transfer each way on VLAN 20 with every byte verified, VLANs
 * kept apart (an address of VLAN 10 is not reached through VLAN 20, a VLAN
 * the other side lacks gets no answer), and a VLAN deleted at run time.
 * No privileges needed.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <signal.h>
#include <sys/wait.h>

#include "t_tcp_common.h"
#include "t_infra_stubs.h"

#define A_IP 0xc0a84d01u     /* 192.168.77.1 */
#define B_IP 0xc0a84d02u
#define A10  0x0a0a0001u     /* 10.10.0.1 */
#define B10  0x0a0a0002u
#define A20  0x0a140001u     /* 10.20.0.1 */
#define B20  0x0a140002u
#define A30  0x0a1e0001u     /* 10.30.0.1: only A has VLAN 30 */
#define B30  0x0a1e0002u
#define UPORT 9000
#define TPORT 9001
#define TOTAL (1024u * 1024u)

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 0, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 0, 0x0b };
static volatile int b_quit;

static int line(const char *fmt, const char *a)
{
	char l[200], err[200];
	int r;

	snprintf(l, sizeof(l), fmt, a, a, a);
	rt_lock();
	r = rt_config_line(l, err, sizeof(err), NULL);
	rt_unlock();
	if (r < 0)
		fprintf(stderr, "%s: %s\n", l, err);
	return r;
}

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, int b)
{
	struct rt_netif *n;
	char ip[64];

	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	snprintf(ip, sizeof(ip), "ifconfig %%s 192.168.77.%d netmask 255.255.255.0 up", b ? 2 : 1);
	if (line(ip, n->name) < 0 ||
	    line("interface %s.10 vlan=10 parent=%s", n->name) < 0 ||
	    line("interface %s.20 vlan=20 parent=%s pcp=3", n->name) < 0)
		return NULL;
	snprintf(ip, sizeof(ip), "ifconfig %%s.10 10.10.0.%d netmask 255.255.255.0 up", b ? 2 : 1);
	if (line(ip, n->name) < 0)
		return NULL;
	snprintf(ip, sizeof(ip), "ifconfig %%s.20 10.20.0.%d netmask 255.255.255.0 up", b ? 2 : 1);
	if (line(ip, n->name) < 0)
		return NULL;
	if (!b && (line("interface %s.30 vlan=30 parent=%s", n->name) < 0 ||
	           line("ifconfig %s.30 10.30.0.1 netmask 255.255.255.0 up", n->name) < 0))
		return NULL;
	return n;
}

/* B: UDP echo on every address; TCP on VLAN 20: verify what A sends,
 * answer with the count, then send TOTAL bytes back. */
static void *b_udp(void *arg)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = t_sa(0, UPORT), from;
	static char buf[2048];
	int fd;

	(void)arg;
	rt_ctx_init(&c, 4, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	rt_api_bind(&c, fd, &a, sizeof(a));
	for (;;) {
		u32 fl = sizeof(from);
		int r = rt_api_recvfrom(&c, fd, buf, sizeof(buf), 0, &from, &fl);
		if (r == 4 && !memcmp(buf, "quit", 4))
			b_quit = 1;
		else if (r >= 0)
			rt_api_sendto(&c, fd, buf, (u32)r, 0, &from, fl);
	}
	return NULL;
}

static int run_b(const char *pa, const char *pb)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = t_sa(B20, TPORT);
	pthread_t th;
	int lfd, fd;
	u64 got, bad;
	u8 res[16];

	alarm(120);
	if (!bring_up(pb, pa, mac_b, 1))
		return 2;
	pthread_create(&th, NULL, b_udp, NULL);
	rt_ctx_init(&c, 8, NULL);
	lfd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	if (rt_api_bind(&c, lfd, &a, sizeof(a)) != 0 || rt_api_listen(&c, lfd, 2) != 0)
		return 3;
	fd = rt_api_accept(&c, lfd, NULL, NULL);
	if (fd < 0)
		return 4;
	got = t_recv_pattern(&c, fd, 11, &bad, 8192);
	rt_put_be32(res, (u32)got);
	rt_put_be32(res + 4, (u32)bad);
	rt_api_send(&c, fd, res, 8, 0);
	t_send_pattern(&c, fd, TOTAL, 22, 8192);
	rt_api_shutdown(&c, fd, 1);
	/* until A closes */
	while (rt_api_recv(&c, fd, res, sizeof(res), 0) > 0)
		;
	rt_api_close(&c, fd);
	while (!b_quit)
		usleep(20000);
	rt_host_stop();
	return 0;
}

/* one UDP exchange to ip: 1 if echoed within ms */
static int udp_echo(struct rt_sockctx *c, int fd, u32 ip, u32 len, u32 ms)
{
	static char buf[2048], in[2048];
	struct rt_sockaddr_in d = t_sa(ip, UPORT);
	struct rt_timeval tv = { 0, 100000 };
	double end = t_now() + ms / 1000.0;

	memset(buf, (int)(ip & 0xff), sizeof(buf));
	rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	rt_api_sendto(c, fd, buf, len, 0, &d, sizeof(d));
	while (t_now() < end) {
		struct rt_sockaddr_in from;
		u32 fl = sizeof(from);
		int r = rt_api_recvfrom(c, fd, in, sizeof(in), 0, &from, &fl);
		if (r == (int)len && from.sin_addr == d.sin_addr && !memcmp(in, buf, len))
			return 1;
	}
	return 0;
}

int main(void)
{
	char pa[64], pb[64], name[24];
	struct rt_netif *nif, *v10, *v20;
	struct rt_sockctx c;
	struct rt_sockaddr_in d;
	pid_t child;
	int fd, tfd, i, ok, status = 0;
	u64 got, bad;
	u8 res[8];
	double t0;

	snprintf(pa, sizeof(pa), "/tmp/rt_vlan_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_vlan_%d_b", (int)getpid());
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(run_b(pa, pb));

	alarm(120);
	nif = bring_up(pa, pb, mac_a, 0);
	CHECK(nif != NULL);
	if (!nif)
		goto out;
	snprintf(name, sizeof(name), "%s.10", nif->name);
	v10 = rt_netif_find(name);
	snprintf(name, sizeof(name), "%s.20", nif->name);
	v20 = rt_netif_find(name);
	CHECK(v10 && v20 && v10->mtu == 1496);
	rt_ctx_init(&c, 8, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);

	/* untagged and VLAN 10 (B may still be starting: retry) */
	for (i = 0, ok = 0; i < 30 && !ok; i++)
		ok = udp_echo(&c, fd, B_IP, 32, 300);
	CHECK(ok);
	for (i = 0, ok = 0; i < 10 && !ok; i++)
		ok = udp_echo(&c, fd, B10, 32, 300);
	CHECK(ok);
	CHECK(v10 && v10->stats.ipackets > 0 && v10->stats.opackets > 0);
	/* a datagram that fills the VLAN MTU, and one that is fragmented */
	CHECK(udp_echo(&c, fd, B10, 1496 - 28, 1000));
	CHECK(udp_echo(&c, fd, B10, 2000, 1000));
	/* B has no VLAN 30: no answer (B drops the frames as noproto) */
	CHECK(!udp_echo(&c, fd, B30, 32, 600));

	/* TCP on VLAN 20, both ways, every byte checked */
	tfd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_STREAM, 0);
	d = t_sa(B20, TPORT);
	CHECK(rt_api_connect(&c, tfd, &d, sizeof(d)) == 0);
	t0 = t_now();
	CHECK(t_send_pattern(&c, tfd, TOTAL, 11, 8192) == TOTAL);
	rt_api_shutdown(&c, tfd, 1);
	{
		u32 n = 0;
		while (n < 8) {
			int r = rt_api_recv(&c, tfd, res + n, 8 - n, 0);
			if (r <= 0)
				break;
			n += (u32)r;
		}
		CHECK(n == 8 && rt_get_be32(res) == TOTAL && rt_get_be32(res + 4) == 0);
	}
	got = t_recv_pattern(&c, tfd, 22, &bad, 8192);
	CHECK(got == TOTAL && bad == 0);
	printf("VLAN 20: 2 x %u bytes in %.2f s\n", TOTAL, t_now() - t0);
	rt_api_close(&c, tfd);
	CHECK(v20 && v20->stats.ipackets > 500 && nif->stats.ipackets >= v20->stats.ipackets);

	/* kept apart: the VLAN 10 address is not on VLAN 20's wire */
	{
		char l[96], err[96];
		snprintf(l, sizeof(l), "route add 10.10.0.2 netmask 255.255.255.255 dev %s", v20->name);
		rt_lock();
		rt_config_line(l, err, sizeof(err), NULL);
		rt_unlock();
		/* the host route sends it out tagged 20; B's VLAN 20 has no 10.10.0.2 */
		CHECK(!udp_echo(&c, fd, B10, 32, 600));
		snprintf(l, sizeof(l), "route del 10.10.0.2 netmask 255.255.255.255 dev %s", v20->name);
		rt_lock();
		rt_config_line(l, err, sizeof(err), NULL);
		rt_unlock();
	}

	/* delete VLAN 10 at run time: 10.10.0.2 is gone, the rest stays */
	CHECK(line("interface %s.10 delete", nif->name) == 0);
	CHECK(!udp_echo(&c, fd, B10, 32, 400));
	CHECK(udp_echo(&c, fd, B_IP, 32, 1000));
	CHECK(udp_echo(&c, fd, B20, 32, 1000));
	for (i = 0; i < 5; i++) {
		struct rt_sockaddr_in q = t_sa(B_IP, UPORT);
		rt_api_sendto(&c, fd, "quit", 4, 0, &q, sizeof(q));
	}
	rt_ctx_destroy(&c);
out:
	if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "B exited with status %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		failures++;
	}
	rt_host_stop();
	unlink(pa);
	unlink(pb);
	printf("t_vlan_wire: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
