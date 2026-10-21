/*
 * Riptide - IPv6 interop with an independent stack: gVisor's netstack
 * (tests/interop/gvpeer, built with "make gvpeer") on the simulated
 * Ethernet. gvpeer is the link's router (it sends the router
 * advertisements) and runs echo/discard/source services.
 *
 * Checked: SLAAC from its RA (prefix, default route, RDNSS, MTU option),
 * neighbour discovery both ways, ICMPv6 echo both ways (global and
 * link-local with a scope id), TCP bulk both ways with data integrity,
 * UDP echo including fragmented datagrams in both directions, ICMPv6
 * port unreachable and TCP RST from the peer, and the MSS after an RA
 * lowers the link MTU to 1280. Prints the TCP throughput.
 *
 * Skips (77) when the gvpeer binary is missing: build it with
 * "make gvpeer" (needs Go; downloads gvisor.dev/gvisor).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct rt_sockctx C;
static struct rt_netif *eth;
static FILE *peer_in, *peer_out;
static pid_t peer_pid;
static rt_in6_addr my_global;

static rt_in6_addr A(const char *s)
{
	rt_in6_addr a;
	rt_inet6_pton(s, &a);
	return a;
}

static struct rt_sockaddr_in6 sa6(const char *addr, u16 port, u32 scope)
{
	struct rt_sockaddr_in6 s;

	memset(&s, 0, sizeof(s));
	s.sin6_len = sizeof(s);
	s.sin6_family = RT_AF_INET6;
	s.sin6_port = rt_htons(port);
	s.sin6_addr = A(addr);
	s.sin6_scope_id = scope;
	return s;
}

static void set_timeout(int fd, u32 ms)
{
	struct rt_timeval tv;

	tv.tv_secs = ms / 1000;
	tv.tv_micro = (ms % 1000) * 1000;
	rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv));
}

static double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* send a command to gvpeer and return its answer line */
static char *peer_cmd(const char *fmt, ...)
{
	static char line[256];
	va_list ap;

	va_start(ap, fmt);
	vfprintf(peer_in, fmt, ap);
	va_end(ap);
	fputc('\n', peer_in);
	fflush(peer_in);
	if (!fgets(line, sizeof(line), peer_out))
		return "fail eof";
	line[strcspn(line, "\n")] = 0;
	return line;
}

static int start_peer(const char *bin, const char *me, const char *peer)
{
	int in[2], out[2];
	char line[128];

	if (pipe(in) < 0 || pipe(out) < 0)
		return -1;
	peer_pid = fork();
	if (peer_pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		close(in[1]);
		close(out[0]);
		execl(bin, bin, "-sock", peer, "-peer", me, "-mac", "02:00:00:00:00:01", (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	peer_in = fdopen(in[1], "w");
	peer_out = fdopen(out[0], "r");
	if (!fgets(line, sizeof(line), peer_out) || strncmp(line, "ready", 5))
		return -1;
	return 0;
}

static int wait_slaac(int ms)
{
	struct rt_in6_ifaddr_info info[8];
	int i, n, t;

	for (t = 0; t < ms; t += 50) {
		rt_lock();
		n = rt_in6_addr_list(eth, info, 8);
		rt_unlock();
		for (i = 0; i < n && i < 8; i++)
			if (info[i].scope == RT_IN6_SCOPE_GLOBAL && !(info[i].flags & RT_IN6_IFF_TENTATIVE) &&
			    rt_in6_prefix_eq(&info[i].addr, &(rt_in6_addr){ .b = { 0x20, 1, 0xd, 0xb8, 0, 1 } }, 64)) {
				my_global = info[i].addr;
				return 0;
			}
		usleep(50000);
	}
	return -1;
}

static void test_ping(const char *addr, u32 scope)
{
	struct rt_sockaddr_in6 a = sa6(addr, 0, scope), from;
	u8 req[40], buf[256];
	u32 fl;
	int s = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_RAW, RT_IPPROTO_ICMPV6), r, i, got = 0;

	set_timeout(s, 3000);
	memset(req, 0, sizeof(req));
	req[0] = RT_ICMP6_ECHO_REQUEST;
	rt_put_be16(req + 4, 0xbeef);
	for (i = 8; i < (int)sizeof(req); i++)
		req[i] = (u8)i;
	CHECK(rt_api_sendto(&C, s, req, sizeof(req), 0, &a, sizeof(a)) == (int)sizeof(req));
	for (i = 0; i < 5 && !got; i++) {
		fl = sizeof(from);
		r = rt_api_recvfrom(&C, s, buf, sizeof(buf), 0, &from, &fl);
		if (r <= 0)
			break;
		if (buf[0] == RT_ICMP6_ECHO_REPLY && rt_get_be16(buf + 4) == 0xbeef &&
		    rt_in6_eq(&from.sin6_addr, &a.sin6_addr) && !memcmp(buf + 8, req + 8, sizeof(req) - 8))
			got = 1;
	}
	CHECK(got);
	if (!got)
		fprintf(stderr, "no echo reply from %s\n", addr);
	rt_api_close(&C, s);
}

static u8 pattern(u32 i) { return (u8)(i * 7 + (i >> 9)); }

struct rx {
	int fd;
	u32 total;
	u32 got;
	int ok;
};

static void *rx_thread(void *p)
{
	struct rx *x = p;
	static u8 buf[16384];
	int r;

	x->ok = 1;
	while (x->got < x->total) {
		u32 k;
		r = rt_api_recv(&C, x->fd, buf, sizeof(buf), 0);
		if (r <= 0)
			break;
		for (k = 0; k < (u32)r; k++)
			if (buf[k] != pattern(x->got + k))
				x->ok = 0;
		x->got += (u32)r;
	}
	return NULL;
}

static int tx_all(int fd, u32 total)
{
	static u8 buf[16384];
	u32 sent = 0;

	while (sent < total) {
		u32 n = total - sent > sizeof(buf) ? sizeof(buf) : total - sent, k;
		int r;
		for (k = 0; k < n; k++)
			buf[k] = pattern(sent + k);
		r = rt_api_send(&C, fd, buf, n, 0);
		if (r <= 0)
			return -1;
		sent += (u32)r;
	}
	return 0;
}

static int tcp_connect(u16 port)
{
	struct rt_sockaddr_in6 a = sa6("2001:db8:1::1", port, 0);
	int fd = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_STREAM, 0);

	set_timeout(fd, 20000);
	if (rt_api_connect(&C, fd, &a, sizeof(a)) < 0) {
		rt_api_close(&C, fd);
		return -1;
	}
	return fd;
}

static void test_tcp(u32 total, const char *what)
{
	struct rx x;
	pthread_t th;
	double t0, t1;
	int fd;
	u8 hdr[4];

	/* echo: integrity both ways at once */
	fd = tcp_connect(7);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	x.fd = fd;
	x.total = total;
	x.got = 0;
	pthread_create(&th, NULL, rx_thread, &x);
	CHECK(tx_all(fd, total) == 0);
	pthread_join(th, NULL);
	CHECK(x.ok && x.got == total);
	rt_api_close(&C, fd);
	/* source: peer -> us */
	fd = tcp_connect(19);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	rt_put_be32(hdr, total);
	t0 = now_s();
	CHECK(rt_api_send(&C, fd, hdr, 4, 0) == 4);
	x.fd = fd;
	x.got = 0;
	rx_thread(&x);
	t1 = now_s();
	CHECK(x.ok && x.got == total);
	printf("%s: TCP over IPv6 gvisor->riptide %u bytes in %.2f s: %.1f Mbit/s\n", what, total, t1 - t0,
	       total * 8 / (t1 - t0) / 1e6);
	rt_api_close(&C, fd);
	/* discard: us -> peer */
	fd = tcp_connect(9);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	t0 = now_s();
	CHECK(tx_all(fd, total) == 0);
	rt_api_shutdown(&C, fd, 1);
	{
		char b;
		CHECK(rt_api_recv(&C, fd, &b, 1, 0) == 0);    /* their FIN after all data */
	}
	t1 = now_s();
	printf("%s: TCP over IPv6 riptide->gvisor %u bytes in %.2f s: %.1f Mbit/s\n", what, total, t1 - t0,
	       total * 8 / (t1 - t0) / 1e6);
	rt_api_close(&C, fd);
}

static void test_udp(void)
{
	struct rt_sockaddr_in6 a = sa6("2001:db8:1::1", 7, 0), from;
	static u8 out[4000], in[5000];
	u32 fl, sizes[] = { 1, 1000, 1452, 3000, 3999 }, i, k;
	int s = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_DGRAM, 0), r;

	set_timeout(s, 3000);
	for (i = 0; i < sizeof(out); i++)
		out[i] = pattern(i ^ 0x55);
	for (k = 0; k < RT_ARRAY_SIZE(sizes); k++) {
		CHECK(rt_api_sendto(&C, s, out, sizes[k], 0, &a, sizeof(a)) == (int)sizes[k]);
		fl = sizeof(from);
		r = rt_api_recvfrom(&C, s, in, sizeof(in), 0, &from, &fl);
		CHECK(r == (int)sizes[k] && !memcmp(in, out, sizes[k]) &&
		      rt_in6_eq(&from.sin6_addr, &a.sin6_addr) && from.sin6_port == rt_htons(7));
		if (r != (int)sizes[k])
			fprintf(stderr, "udp echo of %u bytes: %d\n", sizes[k], r);
	}
	rt_api_close(&C, s);
	/* connected socket to a closed port: ICMPv6 port unreachable */
	s = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	set_timeout(s, 3000);
	a = sa6("2001:db8:1::1", 4444, 0);
	CHECK(rt_api_connect(&C, s, &a, sizeof(a)) == 0);
	CHECK(rt_api_send(&C, s, "x", 1, 0) == 1);
	r = rt_api_recv(&C, s, in, sizeof(in), 0);
	CHECK(r == -RT_ECONNREFUSED);
	rt_api_close(&C, s);
	/* TCP to a closed port: RST */
	CHECK(tcp_connect(4444) < 0);
}

/* our services for gvpeer's client commands */
static int lfd, ufd;

static void *echo_server(void *p)
{
	static u8 buf[16384];
	int afd;

	(void)p;
	afd = rt_api_accept(&C, lfd, NULL, NULL);
	if (afd < 0)
		return NULL;
	set_timeout(afd, 20000);
	for (;;) {
		int r = rt_api_recv(&C, afd, buf, sizeof(buf), 0), o = 0;
		if (r <= 0)
			break;
		while (o < r) {
			int w = rt_api_send(&C, afd, buf + o, (u32)(r - o), 0);
			if (w <= 0)
				goto out;
			o += w;
		}
	}
out:
	rt_api_close(&C, afd);
	return NULL;
}

static void *udp_server(void *p)
{
	static u8 buf[8192];
	struct rt_sockaddr_in6 from;
	u32 fl = sizeof(from);
	int r;

	(void)p;
	r = rt_api_recvfrom(&C, ufd, buf, sizeof(buf), 0, &from, &fl);
	if (r > 0)
		rt_api_sendto(&C, ufd, buf, (u32)r, 0, &from, fl);
	return NULL;
}

static void test_peer_client(void)
{
	struct rt_sockaddr_in6 a = sa6("::", 8000, 0);
	char addr[RT_INET6_ADDRSTRLEN], ll[RT_INET6_ADDRSTRLEN];
	pthread_t th;
	char *ans;
	int one = 1;
	struct rt_in6_ifaddr_info info[8];
	int i, n;

	rt_inet6_ntop(&my_global, addr, sizeof(addr));
	lfd = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_STREAM, 0);
	rt_api_setsockopt(&C, lfd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
	CHECK(rt_api_bind(&C, lfd, &a, sizeof(a)) == 0 && rt_api_listen(&C, lfd, 4) == 0);
	set_timeout(lfd, 20000);
	pthread_create(&th, NULL, echo_server, NULL);
	ans = peer_cmd("connect %s 8000 3000000", addr);
	CHECK(!strcmp(ans, "ok 3000000"));
	if (strcmp(ans, "ok 3000000"))
		fprintf(stderr, "gvpeer connect: %s\n", ans);
	pthread_join(th, NULL);
	rt_api_close(&C, lfd);

	ufd = rt_api_socket(&C, RT_AF_INET6, RT_SOCK_DGRAM, 0);
	a = sa6("::", 8001, 0);
	CHECK(rt_api_bind(&C, ufd, &a, sizeof(a)) == 0);
	set_timeout(ufd, 5000);
	pthread_create(&th, NULL, udp_server, NULL);
	ans = peer_cmd("udp %s 8001 2500", addr);
	CHECK(!strcmp(ans, "ok 2500"));
	pthread_join(th, NULL);
	rt_api_close(&C, ufd);

	ans = peer_cmd("ping %s", addr);
	CHECK(!strcmp(ans, "ok"));
	rt_lock();
	n = rt_in6_addr_list(eth, info, 8);
	rt_unlock();
	ll[0] = 0;
	for (i = 0; i < n && i < 8; i++)
		if (info[i].scope == RT_IN6_SCOPE_LINK)
			rt_inet6_ntop(&info[i].addr, ll, sizeof(ll));
	ans = peer_cmd("ping %s", ll);
	CHECK(!strcmp(ans, "ok"));
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/rt_gv_XXXXXX", me[128], peer[128], bin[512];
	static const u8 mac[6] = { 0x02, 0, 0, 0, 0, 0x22 };
	const char *gv = getenv("RT_GVPEER");
	struct rt_nexthop6 nh;
	rt_in6_addr dns[8];
	char *ans, *slash;
	int ndns, i, found;

	(void)argc;
	if (!gv) {
		rt_strlcpy(bin, argv[0], sizeof(bin));
		slash = strrchr(bin, '/');
		if (slash)
			*slash = 0;
		else
			strcpy(bin, ".");
		strncat(bin, "/../gvpeer", sizeof(bin) - strlen(bin) - 1);
		gv = bin;
	}
	if (access(gv, X_OK) != 0) {
		printf("skip: no gvpeer binary (make gvpeer; needs Go)\n");
		return 77;
	}
	signal(SIGPIPE, SIG_IGN);
	if (!mkdtemp(dir))
		return 1;
	snprintf(me, sizeof(me), "%s/rt", dir);
	snprintf(peer, sizeof(peer), "%s/gv", dir);
	CHECK(rt_host_start() == 0);
	CHECK(rt_ctx_init(&C, 64, NULL) == 0);
	rt_lock();
	CHECK(rt_sysctl_set_text("net.inet6.icmp6.ratelimit", "0") == 0);
	rt_unlock();
	eth = rt_host_wire_open(me, peer, mac);
	CHECK(eth != NULL);
	if (!eth)
		return 1;
	rt_lock();
	rt_netif_up(eth);
	rt_unlock();
	if (start_peer(gv, me, peer) < 0) {
		fprintf(stderr, "gvpeer did not start\n");
		return 1;
	}
	ans = peer_cmd("ra");
	CHECK(!strcmp(ans, "ok ra"));
	CHECK(wait_slaac(8000) == 0);
	rt_lock();
	CHECK(rt_ip6_route(&(rt_in6_addr){ .b = { 0x20, 1, 0xd, 0xb8, 0xff } }, NULL, &nh) == 0 &&
	      nh.netif == eth && (nh.flags & RT_RTF_GATEWAY));
	ndns = rt_dns_get_servers6(dns, NULL, NULL, 8);
	for (i = 0, found = 0; i < ndns && i < 8; i++)
		found |= rt_in6_eq(&dns[i], &(rt_in6_addr){ .b = { 0x20, 1, 0xd, 0xb8, 0, 1, [15] = 0x53 } });
	CHECK(found);
	rt_unlock();

	test_ping("2001:db8:1::1", 0);
	test_ping("fe80::1", eth->index);
	{
		struct rt_nd6_info ni[8];
		int k, n, ok = 0;
		rt_lock();
		n = rt_nd6_list(ni, 8);
		rt_unlock();
		for (k = 0; k < n && k < 8; k++)
			if (ni[k].hw[5] == 0x01 && ni[k].state != RT_NUD_INCOMPLETE)
				ok = 1;
		CHECK(ok);
	}
	test_tcp(4u << 20, "MTU 1500");
	test_udp();
	test_peer_client();

	/* RA lowers the link MTU: new connections use MSS 1280 - 60 - 12 */
	ans = peer_cmd("ra mtu 1280");
	CHECK(!strcmp(ans, "ok ra"));
	usleep(200000);
	CHECK(rt_ip6_link_mtu(eth) == 1280);
	{
		int fd = tcp_connect(7), mss = 0;
		u32 l = sizeof(mss);
		CHECK(fd >= 0 && rt_api_getsockopt(&C, fd, RT_IPPROTO_TCP, RT_TCP_MAXSEG, &mss, &l) == 0 &&
		      mss == 1280 - 60 - 12);
		rt_api_close(&C, fd);
	}
	test_tcp(1u << 20, "MTU 1280");

	peer_cmd("quit");
	waitpid(peer_pid, NULL, 0);
	rt_ctx_destroy(&C);
	rt_host_stop();
	unlink(me);
	unlink(peer);
	rmdir(dir);
	printf("t_ip6_gvisor: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
