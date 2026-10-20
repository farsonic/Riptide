/*
 * Riptide - stub resolver against a tiny DNS server on the Linux side of a
 * TUN device (needs root and /dev/net/tun; exits 77 otherwise).
 *   Linux 10.78.1.1/29  <-- tun -->  Riptide 10.78.1.2
 * 10.78.1.5 is a "dead" server (Linux drops it): tests server failover.
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

#define OUR_IP   0x0a4e0102u
#define LINUX_IP 0x0a4e0101u
#define DEAD_IP  0x0a4e0105u

static int sfd;
static volatile int queries, stop;
static volatile u16 ports[64];

/* ---- responder ---- */
static int qname(const u8 *q, u32 len, char *out)
{
	u32 p = 12, o = 0;
	while (p < len && q[p]) {
		u32 l = q[p];
		if (p + 1 + l > len || o + l + 2 > 255)
			return -1;
		if (o)
			out[o++] = '.';
		memcpy(out + o, q + p + 1, l);
		o += l;
		p += 1 + l;
	}
	out[o] = 0;
	return (int)p + 1;
}

static u8 *put_a(u8 *p, u16 owner, u32 ttl, u32 ip)
{
	rt_put_be16(p, (u16)(0xc000 | owner)); rt_put_be16(p + 2, 1); rt_put_be16(p + 4, 1);
	rt_put_be32(p + 6, ttl); rt_put_be16(p + 10, 4); rt_put_be32(p + 12, ip);
	return p + 16;
}

static void respond(const u8 *q, u32 len, struct sockaddr_in *from)
{
	u8 r[512], *p;
	char name[256];
	int qe = qname(q, len, name);
	u16 type, an = 0, flags = 0x8180;

	if (qe < 0 || (u32)qe + 4 > len)
		return;
	if (queries < 64)
		ports[queries] = from->sin_port;
	queries++;
	type = rt_get_be16(q + qe);
	memcpy(r, q, (u32)qe + 4);
	p = r + qe + 4;
	if (!strcmp(name, "www.test.lan") && type == 1) {
		p = put_a(p, 12, 60, 0x0a010101u);
		p = put_a(p, 12, 60, 0x0a010102u);
		an = 2;
	} else if (!strcmp(name, "alias.test.lan") && type == 1) {
		/* alias CNAME www.test.lan (written in full), then its A */
		u8 *tgt;
		rt_put_be16(p, 0xc00c); rt_put_be16(p + 2, 5); rt_put_be16(p + 4, 1);
		rt_put_be32(p + 6, 300); rt_put_be16(p + 10, 14);
		tgt = p + 12;
		memcpy(tgt, "\003www\004test\003lan\000", 14);
		p = tgt + 14;
		p = put_a(p, (u16)(tgt - r), 30, 0x0a010101u);
		an = 2;
	} else if (!strcmp(name, "short.test.lan") && type == 1) {
		p = put_a(p, 12, 60, 0x0a010103u);
		an = 1;
	} else if (!strcmp(name, "trunc.test.lan") && type == 1) {
		p = put_a(p, 12, 60, 0x0a010104u);
		an = 1;
		flags |= 0x0200;
	} else if (!strcmp(name, "badid.test.lan") && type == 1) {
		u8 bad[512];
		memcpy(bad, r, (u32)(p - r));
		bad[0] ^= 0x55;
		rt_put_be16(bad + 2, flags);
		rt_put_be16(bad + 4, 1);
		rt_put_be16(bad + 6, 0);
		sendto(sfd, bad, (size_t)(p - r), 0, (struct sockaddr *)from, sizeof(*from));
		p = put_a(p, 12, 60, 0x0a010105u);
		an = 1;
	} else if (!strcmp(name, "1.1.1.10.in-addr.arpa") && type == 12) {
		rt_put_be16(p, 0xc00c); rt_put_be16(p + 2, 12); rt_put_be16(p + 4, 1);
		rt_put_be32(p + 6, 300); rt_put_be16(p + 10, 14);
		memcpy(p + 12, "\003www\004test\003lan\000", 14);
		p += 26;
		an = 1;
	} else if (!strcmp(name, "noa.test.lan") || !strcmp(name, "www.test.lan")) {
		an = 0;
	} else {
		flags |= 3;    /* NXDOMAIN */
	}
	rt_put_be16(r + 2, flags);
	rt_put_be16(r + 4, 1);
	rt_put_be16(r + 6, an);
	rt_put_be16(r + 8, 0);
	rt_put_be16(r + 10, 0);
	sendto(sfd, r, (size_t)(p - r), 0, (struct sockaddr *)from, sizeof(*from));
}

static void *server_main(void *arg)
{
	u8 q[512];
	while (!stop) {
		struct pollfd pf = { sfd, POLLIN, 0 };
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n;
		if (poll(&pf, 1, 100) <= 0)
			continue;
		n = recvfrom(sfd, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
		if (n >= 12)
			respond(q, (u32)n, &from);
	}
	return NULL;
}

int main(void)
{
	struct rt_netif *tun;
	struct sockaddr_in sa;
	pthread_t th;
	char name[16], cmd[256], canon[128], host[128];
	u32 addrs[8], t0;
	u8 ans[512];
	int n, q0, i, distinct;

	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 ||
	    system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_dns_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rtd%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), 0, rt_htonl(0xfffffff8u));
	if (!tun) {
		printf("t_dns_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(cmd, sizeof(cmd), "ip addr add 10.78.1.1/29 dev %s && ip link set %s up", name, name);
	if (system(cmd) != 0) {
		printf("t_dns_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}
	sfd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = rt_htonl(LINUX_IP);
	sa.sin_port = rt_htons(53);
	if (sfd < 0 || bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("t_dns_tun: SKIP (cannot bind port 53)\n");
		return 77;
	}
	pthread_create(&th, NULL, server_main, NULL);

	rt_lock();
	rt_dns_clear_servers();
	rt_hosts_clear();
	CHECK(rt_config_line("nameserver 10.78.1.1", NULL, 0, NULL) == 0);
	CHECK(rt_config_line("domain test.lan", NULL, 0, NULL) == 0);
	rt_unlock();

	n = rt_resolve_name("www.test.lan", addrs, 8, canon, sizeof(canon));
	CHECK(n == 2 && addrs[0] == rt_htonl(0x0a010101u) && addrs[1] == rt_htonl(0x0a010102u));
	CHECK(!strcmp(canon, "www.test.lan"));
	q0 = queries;
	CHECK(rt_resolve_name("WWW.test.lan", addrs, 8, NULL, 0) == 2);
	CHECK(queries == q0);                                   /* cache hit */
	CHECK(rt_resolve_name("alias.test.lan", addrs, 8, canon, sizeof(canon)) == 1 &&
	      addrs[0] == rt_htonl(0x0a010101u) && !strcmp(canon, "www.test.lan"));
	CHECK(rt_resolve_name("nx.test.lan", addrs, 8, NULL, 0) == -RT_HOST_NOT_FOUND);
	CHECK(rt_resolve_name("short", addrs, 8, canon, sizeof(canon)) == 1 &&
	      addrs[0] == rt_htonl(0x0a010103u) && !strcmp(canon, "short.test.lan"));
	CHECK(rt_resolve_name("noa.test.lan", addrs, 8, NULL, 0) == -RT_NO_DATA);
	CHECK(rt_resolve_name("trunc.test.lan", addrs, 8, NULL, 0) == 1);
	CHECK(rt_resolve_name("badid.test.lan", addrs, 8, NULL, 0) == 1 && addrs[0] == rt_htonl(0x0a010105u));
	CHECK(rt_resolve_addr(rt_htonl(0x0a010101u), host, sizeof(host)) == 0 && !strcmp(host, "www.test.lan"));
	CHECK(rt_resolve_addr(rt_htonl(0x0a010109u), host, sizeof(host)) == -RT_HOST_NOT_FOUND);

	/* raw interface */
	n = rt_res_query("www.test.lan", 1, 1, ans, sizeof(ans));
	CHECK(n > 12 && rt_get_be16(ans + 6) == 2);
	if (n > 12) {
		char exp[256];
		CHECK(rt_dn_expand(ans, ans + n, ans + 12, exp, sizeof(exp)) == 14 && !strcmp(exp, "www.test.lan"));
	}
	CHECK(rt_res_query("www.test.lan", 1, 15, ans, sizeof(ans)) == -RT_NO_DATA);
	CHECK(rt_res_query("nx.test.lan", 1, 1, ans, sizeof(ans)) == -RT_HOST_NOT_FOUND);
	CHECK(rt_res_search("short", 1, 1, ans, sizeof(ans)) > 12);

	/* random source ports */
	for (i = 1, distinct = 0; i < queries && i < 64; i++)
		distinct += ports[i] != ports[i - 1];
	CHECK(distinct >= queries / 2);

	/* dead first server: 2 s timeout, then the next server */
	rt_lock();
	rt_dns_clear_servers();
	rt_dns_add_server(rt_htonl(DEAD_IP));
	rt_dns_add_server(rt_htonl(LINUX_IP));
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	t0 = rt_now_ms();
	CHECK(rt_resolve_name("www.test.lan.", addrs, 8, NULL, 0) == 2);
	n = (int)(rt_now_ms() - t0);
	CHECK(n >= 1900 && n < 4000);

	/* disallowed: only the hosts table */
	rt_lock();
	rt_dns_disallowed = 1;
	rt_unlock();
	CHECK(rt_resolve_name("alias.test.lan", addrs, 8, NULL, 0) == -RT_HOST_NOT_FOUND);
	rt_lock();
	rt_dns_disallowed = 0;
	rt_dns_clear_servers();
	rt_unlock();

	stop = 1;
	pthread_join(th, NULL);
	rt_host_stop();
	printf("t_dns_tun: %d checks, %d failures (%d queries)\n", checks, failures, queries);
	return failures ? 1 : 0;
}
