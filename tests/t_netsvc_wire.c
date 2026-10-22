/*
 * Riptide - SNTP client and mDNS on the simulated Ethernet wire: two stack
 * instances (two processes), no privileges needed.
 *
 * B ("beta", 192.168.77.2) runs the mDNS responder and a small SNTP server
 * whose clock is 100 s ahead (its first answer is a kiss-o'-death).
 * A ("alpha", 192.168.77.1):
 *   1. resolves beta.local (legacy unicast mDNS query) and its own name
 *   2. asks for 2.77.168.192.in-addr.arpa PTR by hand: beta.local
 *   3. "ntp 192.168.77.2": the KoD fails the first sync, "ntp sync" then
 *      steps the clock (through rt_set_wallclock_hook) by about +100 s
 *   4. renames itself to "beta": the probe meets B's answer and A ends up
 *      as beta-2.local
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define A_IP 0xc0a84d01u     /* 192.168.77.1 */
#define B_IP 0xc0a84d02u     /* 192.168.77.2 */
#define CTL_PORT 9001
#define AHEAD 100            /* B's clock, seconds ahead */
#define NTP_EPOCH 2208988800u

static const u8 mac_a[6] = { 0x02, 0x52, 0x54, 0, 1, 0x0a };
static const u8 mac_b[6] = { 0x02, 0x52, 0x54, 0, 1, 0x0b };

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

static struct rt_netif *bring_up(const char *me, const char *peer, const u8 *mac, u32 ip, const char *host)
{
	struct rt_netif *n;

	rt_strlcpy(rt_hostname, host, sizeof(rt_hostname));
	if (rt_host_start() != 0)
		return NULL;
	n = rt_host_wire_open(me, peer, mac);
	if (!n)
		return NULL;
	rt_lock();
	rt_netif_set_addr(n, rt_htonl(ip), rt_htonl(0xffffff00u), 0);
	rt_netif_up(n);
	{
		char e[64];
		rt_config_line("mdns on", e, sizeof(e), NULL);
	}
	rt_unlock();
	return n;
}

static void ntp_stamp(u8 *p, const struct timeval *t)
{
	rt_put_be32(p, (u32)t->tv_sec + AHEAD + NTP_EPOCH);
	rt_put_be32(p + 4, (u32)(((unsigned long long)t->tv_usec << 32) / 1000000u));
}

/* Process B: SNTP server on 123 until "quit" arrives on CTL_PORT. */
static int run_b(const char *pa, const char *pb)
{
	struct rt_sockctx c;
	struct rt_sockaddr_in a = sa(0, 123), ctl = sa(0, CTL_PORT), from;
	u8 buf[512];
	int fd, cfd, n = 0;

	alarm(80);
	if (!bring_up(pb, pa, mac_b, B_IP, "beta"))
		return 2;
	rt_ctx_init(&c, 8, NULL);
	fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	cfd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (rt_api_bind(&c, fd, &a, sizeof(a)) != 0 || rt_api_bind(&c, cfd, &ctl, sizeof(ctl)) != 0)
		return 3;
	for (;;) {
		u32 set = (1u << fd) | (1u << cfd), fl = sizeof(from);
		int r = rt_api_select(&c, (fd > cfd ? fd : cfd) + 1, &set, NULL, NULL, -1, NULL);
		struct timeval now;
		if (r <= 0)
			continue;
		if (set & (1u << cfd))
			break;
		r = rt_api_recvfrom(&c, fd, buf, sizeof(buf), 0, &from, &fl);
		gettimeofday(&now, NULL);
		if (r < 48 || (buf[0] & 7) != 3)
			continue;
		memcpy(buf + 24, buf + 40, 8);          /* originate = their transmit */
		buf[0] = 0x24;                          /* version 4, server */
		buf[2] = 6;
		buf[3] = 0xec;
		memset(buf + 4, 0, 8);
		if (n++ == 0) {                         /* kiss-o'-death RATE */
			buf[1] = 0;
			memcpy(buf + 12, "RATE", 4);
		} else {
			buf[1] = 2;
			memcpy(buf + 12, "TEST", 4);
		}
		ntp_stamp(buf + 16, &now);
		ntp_stamp(buf + 32, &now);
		ntp_stamp(buf + 40, &now);
		rt_api_sendto(&c, fd, buf, 48, 0, &from, fl);
	}
	rt_host_stop();
	return 0;
}

static volatile u32 hook_sec, hook_usec, hook_calls;
static int test_hook(u32 sec, u32 usec)
{
	hook_sec = sec;
	hook_usec = usec;
	hook_calls++;
	return 0;
}

static int cmd(const char *line, char *out, u32 len)
{
	int r;

	rt_lock();
	r = rt_config_line(line, out, len, NULL);
	rt_unlock();
	return r;
}

static int wait_for(int (*cond)(void), u32 ms)
{
	u32 end = rt_now_ms() + ms;

	while ((s32)(rt_now_ms() - end) < 0) {
		if (cond())
			return 1;
		usleep(50000);
	}
	return cond();
}

static const char *mname(void)
{
	const char *h;

	rt_lock();
	h = rt_mdns_hostname();
	rt_unlock();
	return h;
}
static int responding(void) { return mname()[0] != 0; }
static int renamed(void) { return !strcmp(mname(), "beta-2"); }
static int hooked(void) { return hook_calls != 0; }

/* a hand made legacy query for name/type; returns the answer length */
static int raw_query(struct rt_sockctx *c, const char *name, int type, u8 *ans, int anslen)
{
	struct rt_sockaddr_in g = sa(0xe00000fbu, 5353), from;
	struct rt_timeval tv = { 0, 300000 };
	u8 q[300];
	int fd, ql, r = -1, i;

	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, 0);
	rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv));
	ql = rt_res_mkquery(0, name, 1, type, q, sizeof(q));
	for (i = 0; i < 5 && r < 0 && ql > 0; i++) {
		u32 fl = sizeof(from);
		rt_api_sendto(c, fd, q, (u32)ql, 0, &g, sizeof(g));
		r = rt_api_recvfrom(c, fd, ans, (u32)anslen, 0, &from, &fl);
		if (r >= 12 && rt_get_be16(ans) != rt_get_be16(q))
			r = -1;
	}
	rt_api_close(c, fd);
	return r;
}

int main(void)
{
	char pa[64], pb[64], out[256];
	struct rt_sockctx c;
	struct rt_netif *nif;
	u32 addrs[4];
	u8 ans[512];
	pid_t child;
	int i, n, status = 0;

	snprintf(pa, sizeof(pa), "/tmp/rt_nsvc_%d_a", (int)getpid());
	snprintf(pb, sizeof(pb), "/tmp/rt_nsvc_%d_b", (int)getpid());
	child = fork();
	if (child < 0)
		return 1;
	if (child == 0)
		_exit(run_b(pa, pb));

	alarm(90);
	nif = bring_up(pa, pb, mac_a, A_IP, "alpha");
	CHECK(nif != NULL);
	if (!nif)
		goto out;
	rt_ctx_init(&c, 8, NULL);

	/* 1. mDNS names */
	CHECK(wait_for(responding, 6000));
	CHECK(!strcmp(mname(), "alpha"));
	for (i = 0, n = 0; i < 5 && n <= 0; i++)
		n = rt_resolve_name("beta.local", addrs, 4, NULL, 0);
	CHECK(n == 1 && addrs[0] == rt_htonl(B_IP));
	if (n != 1)
		fprintf(stderr, "beta.local: %d\n", n);
	n = rt_resolve_name("Alpha.local.", addrs, 4, NULL, 0);
	CHECK(n == 1 && addrs[0] == rt_htonl(A_IP));
	n = rt_resolve_name("nobody.local", addrs, 4, NULL, 0);
	CHECK(n < 0);

	/* 2. reverse */
	n = raw_query(&c, "2.77.168.192.in-addr.arpa", 12, ans, sizeof(ans));
	CHECK(n > 12 && rt_get_be16(ans + 6) >= 1);
	CHECK(n > 12 && memmem(ans, (size_t)n, "\004beta\005local", 11) != NULL);

	/* 3. SNTP: KoD first, then a step of +AHEAD */
	rt_set_wallclock_hook = test_hook;
	CHECK(cmd("ntp 192.168.77.2", out, sizeof(out)) == 0);
	for (i = 0; i < 80; i++) {
		cmd("ntp status", out, sizeof(out));
		if (strstr(out, "no valid answer"))
			break;
		usleep(100000);
	}
	CHECK(strstr(out, "no valid answer") != NULL && hook_calls == 0);
	if (!strstr(out, "no valid answer"))
		fprintf(stderr, "after KoD: %s\n", out);
	CHECK(cmd("ntp sync", out, sizeof(out)) == 0);
	CHECK(wait_for(hooked, 5000));
	{
		long d = (long)hook_sec - (long)time(NULL);
		CHECK(d >= AHEAD - 2 && d <= AHEAD + 1);
		fprintf(stderr, "clock step to now%+ld s\n", d);
	}
	cmd("ntp status", out, sizeof(out));
	printf("%s\n", out);
	CHECK(strstr(out, "stratum 2") && strstr(out, "(clock set)") && strstr(out, "offset +"));

	/* 4. name conflict */
	rt_lock();
	rt_strlcpy(rt_hostname, "beta", sizeof(rt_hostname));
	rt_unlock();
	CHECK(wait_for(renamed, 8000));
	cmd("mdns status", out, sizeof(out));
	printf("%s\n", out);
	CHECK(strstr(out, "beta-2.local") && strstr(out, "1 conflicts"));

out:
	{
		struct rt_sockaddr_in b = sa(B_IP, CTL_PORT);
		int fd = rt_api_socket(&c, RT_AF_INET, RT_SOCK_DGRAM, 0);
		rt_api_sendto(&c, fd, "quit", 4, 0, &b, sizeof(b));
	}
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	rt_host_stop();
	unlink(pa);
	unlink(pb);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
