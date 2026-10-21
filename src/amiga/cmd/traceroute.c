/*
 * Riptide - traceroute: print the route packets take to a host.
 *
 *   traceroute [-4|-6] [-n] [-I] [-m max_ttl] [-q nqueries] [-w waittime]
 *              [-p port] [-f first_ttl] HOST [packetlen]
 *
 * UDP probes (default) go to port 33434 and up with IP_TTL set on a UDP
 * socket; -I sends ICMP echo requests instead. Answers (TIME_EXCEEDED,
 * DEST_UNREACH quoting our probe, ECHOREPLY) arrive on a raw ICMP socket.
 * IPv6 (-6, an IPv6 literal, or the program called traceroute6) does the
 * same with IPV6_UNICAST_HOPS and a raw ICMPv6 socket.
 * Uses only the standard bsdsocket.library API.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define RBUF 1280

#ifndef TR_NAME
#define TR_NAME "traceroute"
#define TR_FAMILY 0
#endif

struct tv { ULONG secs, micro; };

static int opt_n, opt_icmp, v6;
static u32 max_ttl = 30, nq = 3, wait_ms = 5000, first_ttl = 1, base_port = 33434, plen = 0;
static u16 ident, sport;
static u8 *rbuf, *pkt;
static struct cl_addr dst;

static void usage(void)
{
	cl_puts("Usage: " TR_NAME " [ -46nI ] [ -f first_ttl ] [ -m max_ttl ] [ -p port ] [ -q nqueries ]\n"
	        "                  [ -w waittime ] host [ packetlen ]\n");
}

static u16 cksum(const u8 *p, u32 n)
{
	u32 s = 0;

	for (; n > 1; p += 2, n -= 2)
		s += ((u32)p[0] << 8) | p[1];
	if (n)
		s += (u32)p[0] << 8;
	s = (s & 0xffff) + (s >> 16);
	s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}

/* Is the ICMP(v6) message at ic (n bytes) an answer to our probe 'key'?
 * Returns the type or -1. */
static int match4(const u8 *ic, u32 n, u16 key)
{
	const u8 *in;
	u32 ihl, d = rt_in6_v4(&dst.a);

	if (ic[0] == RT_ICMP_ECHOREPLY)
		return opt_icmp && rt_get_be16(ic + 4) == ident && rt_get_be16(ic + 6) == key ? ic[0] : -1;
	if (ic[0] != RT_ICMP_TIMXCEED && ic[0] != RT_ICMP_UNREACH)
		return -1;
	if (n < 8 + 20 + 8)
		return -1;
	in = ic + 8;
	ihl = (u32)(in[0] & 15) * 4;
	if (n < 8 + ihl + 8 || memcmp(in + 16, &d, 4))
		return -1;
	if (opt_icmp)
		return in[9] == RT_IPPROTO_ICMP && rt_get_be16(in + ihl + 4) == ident &&
		       rt_get_be16(in + ihl + 6) == key ? ic[0] : -1;
	return in[9] == RT_IPPROTO_UDP && rt_get_be16(in + ihl) == sport &&
	       rt_get_be16(in + ihl + 2) == key ? ic[0] : -1;
}

static int match6(const u8 *ic, u32 n, u16 key)
{
	const u8 *in = ic + 8, *iq = in + 40;

	if (ic[0] == RT_ICMP6_ECHO_REPLY)
		return opt_icmp && rt_get_be16(ic + 4) == ident && rt_get_be16(ic + 6) == key ? ic[0] : -1;
	if (ic[0] != RT_ICMP6_TIME_EXCEEDED && ic[0] != RT_ICMP6_DST_UNREACH)
		return -1;
	if (n < 8 + 40 + 8 || memcmp(in + 24, dst.a.b, 16))
		return -1;
	if (opt_icmp)
		return in[6] == RT_IPPROTO_ICMPV6 && iq[0] == RT_ICMP6_ECHO_REQUEST && rt_get_be16(iq + 4) == ident &&
		       rt_get_be16(iq + 6) == key ? ic[0] : -1;
	return in[6] == RT_IPPROTO_UDP && rt_get_be16(iq) == sport && rt_get_be16(iq + 2) == key ? ic[0] : -1;
}

/* Wait for the answer to the probe (UDP dport / ICMP seq = key).
 * Returns ICMP type (code in *code) or -1 on timeout, -2 on Ctrl-C. */
static int wait_reply(int rs, u16 key, u32 t0, struct cl_addr *from, u32 *rtt, int *code)
{
	for (;;) {
		u32 now = cl_now_us(), el = now - t0, hl;
		ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
		struct tv tmo;
		struct rt_sockaddr_in6 f;
		LONG fl = sizeof(f), n;
		int type;

		if (el >= wait_ms * 1000UL)
			return -1;
		el = wait_ms * 1000UL - el;
		tmo.secs = el / 1000000UL;
		tmo.micro = el % 1000000UL;
		rd[rs / 32] |= 1UL << (rs % 32);
		n = WaitSelect(rs + 1, rd, NULL, NULL, &tmo, &sigs);
		if (sigs & SIGBREAKF_CTRL_C)
			return -2;
		if (n < 0)
			return (Errno() == RT_EINTR || cl_break()) ? -2 : -1;
		if (n == 0)
			continue;
		n = recvfrom(rs, rbuf, RBUF, 0, &f, &fl);
		if (v6) {
			/* raw IPv6 sockets deliver the ICMPv6 message only */
			if (n < 8)
				continue;
			hl = 0;
			type = match6(rbuf, (u32)n, key);
		} else {
			if (n < 28)
				continue;
			hl = (u32)(rbuf[0] & 15) * 4;
			if ((u32)n < hl + 8)
				continue;
			type = match4(rbuf + hl, (u32)n - hl, key);
		}
		if (type < 0)
			continue;
		*rtt = cl_now_us() - t0;
		cl_from_sockaddr(&f, from, NULL);
		*code = rbuf[hl + 1];
		return type;
	}
}

/* "!N" etc. for an unreachable; sets *done or counts *unreach */
static void unreach_mark(int code, const struct cl_addr *from, u32 ttl, int *done, int *unreach)
{
	if (v6) {
		switch (code) {
		case 4:
			if (!rt_in6_eq(&from->a, &dst.a) && ttl <= 1)
				cl_puts(" !");
			*done = 1;
			return;
		case 0: cl_puts(" !N"); break;
		case 1: cl_puts(" !X"); break;
		case 2: cl_puts(" !S"); break;
		case 3: cl_puts(" !H"); break;
		default: cl_printf(" !<%d>", code); break;
		}
		(*unreach)++;
		return;
	}
	switch (code) {
	case RT_ICMP_UNREACH_PORT:
		if (!rt_in6_eq(&from->a, &dst.a) && ttl <= 1)
			cl_puts(" !");
		*done = 1;
		return;
	case RT_ICMP_UNREACH_PROTOCOL: cl_puts(" !P"); *done = 1; return;
	case RT_ICMP_UNREACH_NET: cl_puts(" !N"); break;
	case RT_ICMP_UNREACH_HOST: cl_puts(" !H"); break;
	case RT_ICMP_UNREACH_NEEDFRAG: cl_printf(" !F-%u", 0U); break;
	case RT_ICMP_UNREACH_SRCFAIL: cl_puts(" !S"); break;
	case RT_ICMP_UNREACH_ADMIN: cl_puts(" !X"); break;
	default: cl_printf(" !<%d>", code); break;
	}
	(*unreach)++;
}

static int tr_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in6 to;
	struct cl_addr prev;
	const char *host;
	u32 ttl, iphl, tolen;
	int c, rs = -1, us = -1, rc = CL_FAIL, done = 0, stop = 0, family = TR_FAMILY, have_prev;
	u16 seq = 0;
	char a[CL_ADDRSTRLEN], name[128];

	memset(&g, 0, sizeof(g));
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage();
		return CL_OK;
	}
	while ((c = cl_getopt(argc, argv, "nIm:q:w:p:f:N:z:46", &g)) != -1) {
		switch (c) {
		case 'n': opt_n = 1; break;
		case 'I': opt_icmp = 1; break;
		case 'm': if (cl_parse_uint(g.arg, 255, &max_ttl) < 0 || !max_ttl) goto bad; break;
		case 'q': if (cl_parse_uint(g.arg, 10, &nq) < 0 || !nq) goto bad; break;
		case 'w': if (cl_parse_ms(g.arg, &wait_ms) < 0) goto bad; break;
		case 'p': if (cl_parse_uint(g.arg, 65535, &base_port) < 0) goto bad; break;
		case 'f': if (cl_parse_uint(g.arg, 255, &first_ttl) < 0 || !first_ttl) goto bad; break;
		case '4': family = 4; break;
		case '6': family = 6; break;
		case 'N': case 'z': break;
		default: usage(); return CL_ERROR;
		}
	}
	if (g.ind >= argc) {
		usage();
		return CL_ERROR;
	}
	host = argv[g.ind];
	if (g.ind + 1 < argc) {
		if (cl_parse_uint(argv[g.ind + 1], 32768, &plen) < 0)
			goto bad;
	}
	if (first_ttl > max_ttl) {
		cl_puts("first hop out of range\n");
		return CL_ERROR;
	}
	if (cl_need_timer() < 0 || cl_need_socket() < 0)
		return CL_FAIL;
	if (family == 0 && cl_parse_addr(host, &dst) == 0)
		family = dst.family == RT_AF_INET6 ? 6 : 4;
	if (cl_resolve_af(host, family == 6 ? 6 : 4, &dst) < 0)
		return CL_ERROR;
	v6 = dst.family == RT_AF_INET6;
	iphl = v6 ? 40 : 20;
	if (!plen)
		plen = v6 ? 80 : 60;
	if (plen < iphl + 8 + 8)
		plen = iphl + 8 + 8;
	rbuf = AllocVec(RBUF, MEMF_ANY);
	pkt = AllocVec(plen, MEMF_ANY | MEMF_CLEAR);
	if (!rbuf || !pkt) {
		cl_error("out of memory");
		goto out;
	}
	rs = v6 ? socket(RT_AF_INET6, RT_SOCK_RAW, RT_IPPROTO_ICMPV6) : socket(RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	if (rs < 0) {
		cl_error("cannot create a raw %s socket (errno %ld)", v6 ? "ICMPv6" : "ICMP", (long)Errno());
		goto out;
	}
	ident = (u16)(((u32)FindTask(NULL) >> 4) | 0x8000);
	if (!opt_icmp) {
		struct rt_sockaddr_in6 me;
		struct cl_addr any;
		LONG l = sizeof(me);
		u16 p = 0;
		memset(&any, 0, sizeof(any));
		any.family = dst.family;
		if (!v6)
			rt_in6_set_v4mapped(&any.a, 0);
		us = socket(v6 ? RT_AF_INET6 : RT_AF_INET, RT_SOCK_DGRAM, 0);
		if (us < 0 || bind(us, &me, (LONG)cl_sockaddr(&any, 0, &me)) < 0 || getsockname(us, &me, &l) < 0) {
			cl_error("cannot create a UDP socket");
			goto out;
		}
		cl_from_sockaddr(&me, &any, &p);
		sport = p;
	}
	cl_printf("%s to %s (%s), %lu hops max, %lu byte packets\n", TR_NAME, host, cl_addr_str(&dst, a),
	          (unsigned long)max_ttl, (unsigned long)plen);
	cl_flush();
	rc = CL_OK;
	for (ttl = first_ttl; ttl <= max_ttl && !done && !stop; ttl++) {
		LONG t = (LONG)ttl;
		u32 q;
		int unreach = 0, sr;

		cl_printf("%2lu ", (unsigned long)ttl);
		have_prev = 0;
		if (v6)
			sr = setsockopt(opt_icmp ? rs : us, RT_IPPROTO_IPV6, RT_IPV6_UNICAST_HOPS, &t, sizeof(t));
		else
			sr = setsockopt(opt_icmp ? rs : us, RT_IPPROTO_IP, RT_IP_TTL, &t, sizeof(t));
		if (sr < 0) {
			cl_error("setsockopt %s: errno %ld", v6 ? "IPV6_UNICAST_HOPS" : "IP_TTL", (long)Errno());
			rc = CL_FAIL;
			break;
		}
		for (q = 0; q < nq; q++) {
			struct cl_addr from;
			u32 rtt = 0, t0;
			int type, code = 0;
			u16 key;
			LONG r;

			seq++;
			if (opt_icmp) {
				u32 n = plen - iphl;
				key = seq;
				memset(pkt, 0, n);
				pkt[0] = v6 ? RT_ICMP6_ECHO_REQUEST : RT_ICMP_ECHO;
				rt_put_be16(pkt + 4, ident);
				rt_put_be16(pkt + 6, key);
				if (!v6)        /* the stack computes the ICMPv6 checksum */
					rt_put_be16(pkt + 2, cksum(pkt, n));
				tolen = cl_sockaddr(&dst, 0, &to);
				t0 = cl_now_us();
				r = sendto(rs, pkt, (LONG)n, 0, &to, (LONG)tolen);
			} else {
				key = (u16)(base_port + seq - 1);
				tolen = cl_sockaddr(&dst, key, &to);
				t0 = cl_now_us();
				r = sendto(us, pkt, (LONG)(plen - iphl - 8), 0, &to, (LONG)tolen);
			}
			if (r < 0) {
				LONG e = Errno();
				cl_printf(" send failed: %s", cl_strerror((int)e));
				if (e == RT_ENETUNREACH || e == RT_EHOSTUNREACH) {
					done = 1;
					break;
				}
				continue;
			}
			type = wait_reply(rs, key, t0, &from, &rtt, &code);
			if (type == -2) {
				stop = 1;
				break;
			}
			if (type < 0) {
				cl_puts(" *");
				cl_flush();
				continue;
			}
			if (!have_prev || !rt_in6_eq(&from.a, &prev.a)) {
				if (opt_n)
					cl_printf(" %s", cl_addr_str(&from, a));
				else
					cl_printf(" %s (%s)", cl_addr_name(&from, 0, name, sizeof(name)), cl_addr_str(&from, a));
				prev = from;
				have_prev = 1;
			}
			cl_printf("  %lu.%03lu ms", (unsigned long)(rtt / 1000), (unsigned long)(rtt % 1000));
			if (type == RT_ICMP_ECHOREPLY || type == RT_ICMP6_ECHO_REPLY)
				done = 1;
			else if (v6 ? type == RT_ICMP6_DST_UNREACH : type == RT_ICMP_UNREACH)
				unreach_mark(code, &from, ttl, &done, &unreach);
			cl_flush();
		}
		cl_puts("\n");
		cl_flush();
		if (unreach && (u32)unreach >= nq - 1)
			done = 1;
	}
	if (stop) {
		cl_flush();
		PutStr((CONST_STRPTR)"***Break\n");
		rc = CL_WARN;
	}
out:
	if (us >= 0)
		CloseSocket(us);
	if (rs >= 0)
		CloseSocket(rs);
	if (rbuf)
		FreeVec(rbuf);
	if (pkt)
		FreeVec(pkt);
	return rc;
bad:
	cl_printf("%s: bad value '%s'\n", TR_NAME, g.arg ? g.arg : argv[g.ind + 1]);
	usage();
	return CL_ERROR;
}

CL_MAIN(tr_main, TR_NAME)
