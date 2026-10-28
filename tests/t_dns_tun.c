/*
 * Riptide - stub resolver against a tiny DNS server on the Linux side of a
 * TUN device (needs root and /dev/net/tun; exits 77 otherwise).
 *   Linux 10.78.1.1/29  <-- tun -->  Riptide 10.78.1.2
 * 10.78.1.5 is a "dead" server (Linux drops it): tests server failover.
 * The server truncates UDP answers at 512 bytes, or at the EDNS0 size the
 * query advertises, and also answers over TCP: EDNS0, FORMERR fallback and
 * the TCP fallback for A, AAAA, PTR and res_query.
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

static int sfd, tfd;
static volatile int queries, stop, tcp_queries, last_edns, last_edns_size;
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

static u8 *put_aaaa(u8 *p, u16 owner, u32 ttl, int i)
{
	rt_put_be16(p, (u16)(0xc000 | owner)); rt_put_be16(p + 2, 28); rt_put_be16(p + 4, 1);
	rt_put_be32(p + 6, ttl); rt_put_be16(p + 10, 16);
	memset(p + 12, 0, 16);
	p[12] = 0xfd; p[13] = 0x0d; p[26] = (u8)(i >> 8); p[27] = (u8)i;
	return p + 28;
}

static u8 *put_opt(u8 *p)
{
	p[0] = 0;
	rt_put_be16(p + 1, 41); rt_put_be16(p + 3, 1232);
	rt_put_be32(p + 5, 0); rt_put_be16(p + 9, 0);
	return p + 11;
}

/* The answer to query q (len bytes) in r (4096 bytes), as over TCP; its
 * length, 0 for no answer. *limit: the largest UDP answer the query
 * allows (512, or its EDNS0 size). */
static u32 answer(const u8 *q, u32 len, u8 *r, u32 *limit, int tcp)
{
	char name[256];
	int qe = qname(q, len, name), i, edns = 0, opt = 1;
	u16 type, an = 0, flags = 0x8180, size = 0;
	u8 *p;

	if (qe < 0 || (u32)qe + 4 > len)
		return 0;
	type = rt_get_be16(q + qe);
	/* OPT record right after the question */
	if (rt_get_be16(q + 10) == 1 && (u32)qe + 4 + 11 <= len && q[qe + 4] == 0 &&
	    rt_get_be16(q + qe + 5) == 41) {
		edns = 1;
		size = rt_get_be16(q + qe + 7);
	}
	if (!tcp) {
		last_edns = edns;
		last_edns_size = size;
	}
	*limit = edns ? (size < 512 ? 512 : size) : 512;
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
		/* over UDP always TC with one record, over TCP three */
		p = put_a(p, 12, 60, 0x0a010104u);
		an = 1;
		if (tcp) {
			p = put_a(p, 12, 60, 0x0a010106u);
			p = put_a(p, 12, 60, 0x0a010107u);
			an = 3;
		} else {
			flags |= 0x0200;
		}
	} else if (!strcmp(name, "badid.test.lan") && type == 1) {
		p = put_a(p, 12, 60, 0x0a010105u);
		an = 1;
	} else if ((!strcmp(name, "big.test.lan") || !strcmp(name, "noopt.test.lan")) && type == 1) {
		/* 40 A records: about 690 bytes, over UDP only with EDNS */
		for (i = 0; i < 40; i++)
			p = put_a(p, 12, 60, 0x0a020000u + (u32)i + 1);
		an = 40;
		opt = strcmp(name, "noopt.test.lan") != 0;
	} else if (!strcmp(name, "huge.test.lan") && type == 1) {
		/* 100 A records: about 1650 bytes, more than 1232: TCP */
		for (i = 0; i < 100; i++)
			p = put_a(p, 12, 60, 0x0a030000u + (u32)i + 1);
		an = 100;
	} else if (!strcmp(name, "huge.test.lan") && type == 28) {
		for (i = 0; i < 60; i++)
			p = put_aaaa(p, 12, 60, i + 1);
		an = 60;
	} else if (!strcmp(name, "formerr.test.lan")) {
		if (edns) {
			flags |= 1;     /* FORMERR, no OPT: an old server */
			opt = 0;
		} else if (type == 1) {
			p = put_a(p, 12, 60, 0x0a010108u);
			an = 1;
		}
	} else if (!strcmp(name, "1.1.1.10.in-addr.arpa") && type == 12) {
		rt_put_be16(p, 0xc00c); rt_put_be16(p + 2, 12); rt_put_be16(p + 4, 1);
		rt_put_be32(p + 6, 300); rt_put_be16(p + 10, 14);
		memcpy(p + 12, "\003www\004test\003lan\000", 14);
		p += 26;
		an = 1;
	} else if (!strcmp(name, "2.1.1.10.in-addr.arpa") && type == 12) {
		/* PTR only over TCP */
		if (tcp) {
			rt_put_be16(p, 0xc00c); rt_put_be16(p + 2, 12); rt_put_be16(p + 4, 1);
			rt_put_be32(p + 6, 300); rt_put_be16(p + 10, 14);
			memcpy(p + 12, "\003tcp\004test\003lan\000", 14);
			p += 26;
			an = 1;
		} else {
			flags |= 0x0200;
		}
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
	if (edns && opt) {
		p = put_opt(p);
		rt_put_be16(r + 10, 1);
	}
	return (u32)(p - r);
}

static void respond(const u8 *q, u32 len, struct sockaddr_in *from)
{
	static u8 r[4096];
	char name[256];
	int qe = qname(q, len, name);
	u32 n, limit;

	if (qe < 0)
		return;
	if (queries < 64)
		ports[queries] = from->sin_port;
	queries++;
	n = answer(q, len, r, &limit, 0);
	if (!n)
		return;
	if (!strcmp(name, "badid.test.lan")) {
		u8 bad[512];
		memcpy(bad, r, (u32)qe + 4);
		bad[0] ^= 0x55;
		rt_put_be16(bad + 6, 0);
		rt_put_be16(bad + 10, 0);
		sendto(sfd, bad, (size_t)qe + 4, 0, (struct sockaddr *)from, sizeof(*from));
	}
	if (n > limit) {
		/* too large: the question alone with TC (and OPT when asked) */
		u8 *p = r + qe + 4;
		r[2] |= 0x02;
		rt_put_be16(r + 6, 0);
		if (rt_get_be16(r + 10))
			p = put_opt(p);
		n = (u32)(p - r);
	}
	sendto(sfd, r, n, 0, (struct sockaddr *)from, sizeof(*from));
}

static int readn(int fd, u8 *b, u32 n)
{
	u32 got = 0;
	while (got < n) {
		ssize_t r = read(fd, b + got, n - got);
		if (r <= 0)
			return -1;
		got += (u32)r;
	}
	return 0;
}

/* one query per connection (RFC 7766 allows more; we need not) */
static void tcp_client(int c)
{
	static u8 q[600], r[4098];
	u8 hdr[2];
	u32 len, n, limit;

	if (readn(c, hdr, 2) == 0 && (len = rt_get_be16(hdr)) >= 12 && len <= sizeof(q) &&
	    readn(c, q, len) == 0) {
		tcp_queries++;
		n = answer(q, len, r + 2, &limit, 1);
		if (n) {
			rt_put_be16(r, (u16)n);
			if (write(c, r, 2 + n) != (ssize_t)(2 + n))
				fprintf(stderr, "t_dns_tun: short TCP write\n");
		}
	}
	close(c);
}

static void *server_main(void *arg)
{
	u8 q[600];
	while (!stop) {
		struct pollfd pf[2] = { { sfd, POLLIN, 0 }, { tfd, POLLIN, 0 } };
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n;
		if (poll(pf, 2, 100) <= 0)
			continue;
		if (pf[1].revents & POLLIN) {
			int c = accept(tfd, NULL, NULL);
			if (c >= 0)
				tcp_client(c);
		}
		if (!(pf[0].revents & POLLIN))
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
	u32 addrs[16], t0;
	u8 ans[512];
	static u8 big[4096];
	static rt_in6_addr a6[16];
	int t;
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
	tfd = socket(AF_INET, SOCK_STREAM, 0);
	i = 1;
	setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
	if (tfd < 0 || bind(tfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(tfd, 4) < 0) {
		printf("t_dns_tun: SKIP (cannot listen on TCP port 53)\n");
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
	CHECK(last_edns && last_edns_size == 1232);             /* EDNS0 by default */
	t = tcp_queries;
	CHECK(rt_resolve_name("trunc.test.lan", addrs, 8, NULL, 0) == 3);   /* again over TCP */
	CHECK(tcp_queries == t + 1 && addrs[2] == rt_htonl(0x0a010107u));
	CHECK(rt_resolve_name("badid.test.lan", addrs, 8, NULL, 0) == 1 && addrs[0] == rt_htonl(0x0a010105u));
	CHECK(rt_resolve_addr(rt_htonl(0x0a010101u), host, sizeof(host)) == 0 && !strcmp(host, "www.test.lan"));
	CHECK(rt_resolve_addr(rt_htonl(0x0a010109u), host, sizeof(host)) == -RT_HOST_NOT_FOUND);

	/* larger than 512 bytes: over UDP with EDNS0, TCP beyond 1232 */
	t = tcp_queries;
	CHECK(rt_resolve_name("big.test.lan", addrs, 16, NULL, 0) == 16 && addrs[15] == rt_htonl(0x0a020010u));
	CHECK(tcp_queries == t);
	CHECK(rt_resolve_name("huge.test.lan", addrs, 16, NULL, 0) == 16 && addrs[0] == rt_htonl(0x0a030001u));
	CHECK(tcp_queries == t + 1);
	/* the cache keeps 8 (gethostbyname's limit) */
	CHECK(rt_resolve_name("big.test.lan", addrs, 16, NULL, 0) == 8 && tcp_queries == t + 1);
	/* A and AAAA together (getaddrinfo, Happy Eyeballs): both over TCP */
	rt_lock();
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	n = rt_resolve_name46("huge.test.lan", a6, 16, RT_AF_UNSPEC, NULL, 0);
	CHECK(n == 16 && a6[0].b[0] == 0xfd && tcp_queries == t + 3);
	rt_lock();
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	CHECK(rt_resolve_name6("huge.test.lan", a6, 16, NULL, 0) == 16 && a6[15].b[15] == 16 &&
	      tcp_queries == t + 4);
	/* PTR over TCP */
	CHECK(rt_resolve_addr(rt_htonl(0x0a010102u), host, sizeof(host)) == 0 && !strcmp(host, "tcp.test.lan"));
	CHECK(tcp_queries == t + 5);
	/* without EDNS: big needs TCP too; without TCP the truncated answer */
	rt_lock();
	CHECK(rt_config_line("dns edns_size 0", NULL, 0, NULL) == 0);
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	CHECK(rt_resolve_name("big.test.lan", addrs, 16, NULL, 0) == 16 && !last_edns && tcp_queries == t + 6);
	rt_lock();
	CHECK(rt_config_line("dns tcp off", NULL, 0, NULL) == 0);
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	CHECK(rt_resolve_name("big.test.lan", addrs, 16, NULL, 0) == -RT_TRY_AGAIN && tcp_queries == t + 6);
	CHECK(rt_resolve_name("trunc.test.lan", addrs, 8, NULL, 0) == 1);   /* what fits */
	rt_lock();
	CHECK(rt_config_line("dns tcp on", NULL, 0, NULL) == 0);
	CHECK(rt_config_line("dns edns_size 1232", NULL, 0, NULL) == 0);
	rt_unlock();

	/* FORMERR to EDNS: once more without, then no EDNS to that server */
	CHECK(rt_resolve_name("formerr.test.lan", addrs, 8, NULL, 0) == 1 && addrs[0] == rt_htonl(0x0a010108u));
	CHECK(rt_resolve_name("short.test.lan", addrs, 8, NULL, 0) == 1 && !last_edns);
	rt_lock();
	CHECK(rt_config_line("dns", (char *)big, 200, NULL) == 0 && strstr((char *)big, "1 server(s) without EDNS"));
	CHECK(rt_config_line("dns reset", NULL, 0, NULL) == 0);
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();
	CHECK(rt_resolve_name("short.test.lan", addrs, 8, NULL, 0) == 1 && last_edns);
	/* an answer without OPT: no EDNS from then on */
	CHECK(rt_resolve_name("noopt.test.lan", addrs, 8, NULL, 0) == 8);
	CHECK(rt_resolve_name("www.test.lan", addrs, 8, NULL, 0) == 2 && !last_edns);
	rt_lock();
	CHECK(rt_config_line("dns reset", NULL, 0, NULL) == 0);
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();

	/* the same for A and AAAA together */
	n = rt_resolve_name46("formerr.test.lan", a6, 16, RT_AF_UNSPEC, NULL, 0);
	CHECK(n == 1 && rt_in6_is_v4mapped(&a6[0]) && rt_in6_v4(&a6[0]) == rt_htonl(0x0a010108u) && !last_edns);
	rt_lock();
	CHECK(rt_config_line("dns reset", NULL, 0, NULL) == 0);
	CHECK(rt_sysctl_set_text("dns.cache.flush", "1") == 0);
	rt_unlock();

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
	/* res_query: no EDNS (as BIND), TCP for a truncated answer; a copy
	 * cut at the caller's buffer has TC set */
	t = tcp_queries;
	n = rt_res_query("huge.test.lan", 1, 1, big, sizeof(big));
	CHECK(n > 1600 && rt_get_be16(big + 6) == 100 && !(big[2] & 0x02) && !last_edns && tcp_queries == t + 1);
	n = rt_res_query("huge.test.lan", 1, 1, ans, sizeof(ans));
	CHECK(n == (int)sizeof(ans) && (ans[2] & 0x02) && tcp_queries == t + 2);
	n = rt_res_query("big.test.lan", 1, 1, big, sizeof(big));
	CHECK(n > 512 && rt_get_be16(big + 6) == 40 && tcp_queries == t + 3);

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
	printf("t_dns_tun: %d checks, %d failures (%d queries, %d over TCP)\n", checks, failures, queries,
	       tcp_queries);
	return failures ? 1 : 0;
}
