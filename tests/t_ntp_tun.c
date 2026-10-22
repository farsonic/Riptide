/*
 * Riptide - SNTP client against an SNTP server and a DNS server on the
 * Linux side of a TUN device (needs root and /dev/net/tun; exits 77
 * otherwise). The server's clock runs an hour behind; "ntp time.test"
 * makes the client look the name up itself (asynchronous A query from the
 * stack thread), sync, and step the clock by about -3600 s.
 *   Linux 10.79.1.1/29  <-- tun -->  Riptide 10.79.1.2
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/time.h>
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
#define BEHIND   3600

static int nfd, dfd;
static volatile int stop, ntp_queries, dns_queries;

static void ntp_stamp(u8 *p, const struct timeval *t)
{
	rt_put_be32(p, (u32)t->tv_sec - BEHIND + 2208988800u);
	rt_put_be32(p + 4, (u32)(((unsigned long long)t->tv_usec << 32) / 1000000u));
}

static void ntp_reply(void)
{
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	struct timeval now;
	u8 b[512];
	ssize_t n = recvfrom(nfd, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);

	gettimeofday(&now, NULL);
	if (n < 48 || (b[0] & 7) != 3)
		return;
	ntp_queries++;
	memcpy(b + 24, b + 40, 8);
	b[0] = 0x1c;              /* version 3 answer: accepted too */
	b[1] = 1;
	b[2] = 6;
	b[3] = 0xe9;
	memset(b + 4, 0, 8);
	memcpy(b + 12, "GPS\0", 4);
	ntp_stamp(b + 16, &now);
	ntp_stamp(b + 32, &now);
	usleep(3000);             /* 3 ms of "processing" */
	gettimeofday(&now, NULL);
	ntp_stamp(b + 40, &now);
	sendto(nfd, b, 48, 0, (struct sockaddr *)&from, fl);
}

/* time.test A 10.79.1.1, everything else NXDOMAIN */
static void dns_reply(void)
{
	struct sockaddr_in from;
	socklen_t fl = sizeof(from);
	u8 q[512];
	ssize_t n = recvfrom(dfd, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
	u32 p = 12;

	if (n < 17)
		return;
	dns_queries++;
	while (p < (u32)n && q[p])
		p += 1u + q[p];
	p += 5;
	if (p > (u32)n)
		return;
	q[2] = 0x81;
	q[3] = 0x80;
	rt_put_be16(q + 6, 0);
	rt_put_be16(q + 8, 0);
	rt_put_be16(q + 10, 0);
	if (!memcmp(q + 12, "\004time\004test\000\000\001", 13)) {
		rt_put_be16(q + 6, 1);
		rt_put_be16(q + p, 0xc00c);
		rt_put_be16(q + p + 2, 1);
		rt_put_be16(q + p + 4, 1);
		rt_put_be32(q + p + 6, 60);
		rt_put_be16(q + p + 10, 4);
		rt_put_be32(q + p + 12, LINUX_IP);
		p += 16;
	} else if (rt_get_be16(q + p - 4) != 28) {
		q[3] |= 3;
	}
	sendto(dfd, q, p, 0, (struct sockaddr *)&from, fl);
}

static void *server_main(void *arg)
{
	while (!stop) {
		struct pollfd pf[2] = { { nfd, POLLIN, 0 }, { dfd, POLLIN, 0 } };
		if (poll(pf, 2, 100) <= 0)
			continue;
		if (pf[0].revents & POLLIN)
			ntp_reply();
		if (pf[1].revents & POLLIN)
			dns_reply();
	}
	return NULL;
}

static volatile u32 hook_sec, hook_calls;
static int test_hook(u32 sec, u32 usec)
{
	hook_sec = sec;
	hook_calls++;
	return 0;
}

static int udp_bind(u16 port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = rt_htonl(LINUX_IP);
	sa.sin_port = rt_htons(port);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		return -1;
	return fd;
}

static int cmd(const char *line, char *out, u32 len)
{
	int r;

	rt_lock();
	r = rt_config_line(line, out, len, NULL);
	rt_unlock();
	return r;
}

int main(void)
{
	struct rt_netif *tun;
	pthread_t th;
	char name[16], sh[256], out[256];
	int i;

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_ntp_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rtn%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), 0, rt_htonl(0xfffffff8u));
	if (!tun) {
		printf("t_ntp_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(sh, sizeof(sh), "ip addr add 10.79.1.1/29 dev %s && ip link set %s up", name, name);
	if (system(sh) != 0) {
		printf("t_ntp_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}
	nfd = udp_bind(123);
	dfd = udp_bind(53);
	if (nfd < 0 || dfd < 0) {
		printf("t_ntp_tun: SKIP (cannot bind ports 123 and 53)\n");
		return 77;
	}
	pthread_create(&th, NULL, server_main, NULL);
	alarm(60);

	rt_set_wallclock_hook = test_hook;
	CHECK(cmd("nameserver 10.79.1.1", out, sizeof(out)) == 0);
	CHECK(cmd("ntp time.test", out, sizeof(out)) == 0);
	CHECK(cmd("ntp sync", out, sizeof(out)) == 0);
	for (i = 0; i < 100 && !hook_calls; i++)
		usleep(100000);
	CHECK(hook_calls == 1);
	{
		long d = (long)hook_sec - (long)time(NULL);
		CHECK(d >= -BEHIND - 2 && d <= -BEHIND + 1);
		printf("clock step to now%+ld s after %d DNS and %d NTP queries\n", d, dns_queries, ntp_queries);
	}
	cmd("ntp status", out, sizeof(out));
	printf("%s\n", out);
	CHECK(strstr(out, "NTP: time.test, stratum 1, offset -3599.") || strstr(out, "NTP: time.test, stratum 1, offset -3600."));
	CHECK(dns_queries >= 1 && ntp_queries == 1);

	/* below the step threshold: measured, not set */
	CHECK(cmd("ntp step 4000000", out, sizeof(out)) == 0);
	CHECK(cmd("ntp sync", out, sizeof(out)) == 0);
	for (i = 0; i < 50 && ntp_queries < 2; i++)
		usleep(100000);
	usleep(200000);
	CHECK(ntp_queries == 2 && hook_calls == 1);
	cmd("ntp status", out, sizeof(out));
	CHECK(strstr(out, "stratum 1") && !strstr(out, "clock set"));

	/* a server that does not answer: the status says so */
	CHECK(cmd("ntp 10.79.1.3", out, sizeof(out)) == 0);
	CHECK(cmd("ntp sync", out, sizeof(out)) == 0);
	for (i = 0; i < 60; i++) {
		cmd("ntp status", out, sizeof(out));
		if (strstr(out, "last try"))
			break;
		usleep(100000);
	}
	printf("%s\n", out);
	CHECK(strstr(out, "last try: no valid answer") != NULL);
	cmd("ntp off", out, sizeof(out));

	stop = 1;
	pthread_join(th, NULL);
	rt_host_stop();
	printf("t_ntp_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
