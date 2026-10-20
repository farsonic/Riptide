/*
 * Riptide - traceroute: print the route packets take to a host.
 *
 *   traceroute [-n] [-I] [-m max_ttl] [-q nqueries] [-w waittime]
 *              [-p port] [-f first_ttl] HOST [packetlen]
 *
 * UDP probes (default) go to port 33434 and up with IP_TTL set on a UDP
 * socket; -I sends ICMP echo requests instead. Answers (TIME_EXCEEDED,
 * DEST_UNREACH quoting our probe, ECHOREPLY) arrive on a raw ICMP socket.
 * Uses only the standard bsdsocket.library API.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define RBUF 1024

struct tv { ULONG secs, micro; };

static int opt_n, opt_icmp;
static u32 max_ttl = 30, nq = 3, wait_ms = 5000, first_ttl = 1, base_port = 33434, plen = 60;
static u16 ident, sport;
static u8 *rbuf, *pkt;

static void usage(void)
{
	cl_puts("Usage: traceroute [ -nI ] [ -f first_ttl ] [ -m max_ttl ] [ -p port ] [ -q nqueries ]\n"
	        "                  [ -w waittime ] host [ packetlen ]\n");
}

static void sin_set(struct rt_sockaddr_in *s, u32 a, u16 port)
{
	memset(s, 0, sizeof(*s));
	s->sin_len = sizeof(*s);
	s->sin_family = RT_AF_INET;
	s->sin_port = rt_htons(port);
	s->sin_addr = a;
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

/* Wait for the answer to the probe (UDP dport / ICMP seq = key).
 * Returns ICMP type (code in *code) or -1 on timeout, -2 on Ctrl-C. */
static int wait_reply(int rs, u32 dst, u16 key, u32 t0, u32 *from, u32 *rtt, int *code)
{
	for (;;) {
		u32 now = cl_now_us(), el = now - t0, hl, ihl;
		ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
		struct tv tmo;
		struct rt_sockaddr_in f;
		LONG fl = sizeof(f), n;
		const u8 *ic, *in;

		if (el >= wait_ms * 1000UL)
			return -1;
		el = wait_ms * 1000UL - el;
		tmo.secs = el / 1000000UL;
		tmo.micro = el % 1000000UL;
		rd[rs / 32] |= 1UL << (rs % 32);
		n = WaitSelect(rs + 1, rd, NULL, NULL, &tmo, &sigs);
		if (sigs & SIGBREAKF_CTRL_C)
			return -2;
		if (n <= 0)
			continue;
		n = recvfrom(rs, rbuf, RBUF, 0, &f, &fl);
		if (n < 28)
			continue;
		hl = (u32)(rbuf[0] & 15) * 4;
		if ((u32)n < hl + 8)
			continue;
		ic = rbuf + hl;
		if (ic[0] == RT_ICMP_ECHOREPLY) {
			if (!opt_icmp || rt_get_be16(ic + 4) != ident || rt_get_be16(ic + 6) != key)
				continue;
		} else if (ic[0] == RT_ICMP_TIMXCEED || ic[0] == RT_ICMP_UNREACH) {
			if ((u32)n < hl + 8 + 20 + 8)
				continue;
			in = ic + 8;
			ihl = (u32)(in[0] & 15) * 4;
			if ((u32)n < hl + 8 + ihl + 8 || memcmp(in + 16, &dst, 4))
				continue;
			if (opt_icmp) {
				if (in[9] != RT_IPPROTO_ICMP || rt_get_be16(in + ihl + 4) != ident ||
				    rt_get_be16(in + ihl + 6) != key)
					continue;
			} else if (in[9] != RT_IPPROTO_UDP || rt_get_be16(in + ihl) != sport ||
			           rt_get_be16(in + ihl + 2) != key) {
				continue;
			}
		} else {
			continue;
		}
		*rtt = cl_now_us() - t0;
		*from = f.sin_addr;
		*code = ic[1];
		return ic[0];
	}
}

static int tr_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in to;
	const char *host;
	u32 dst, ttl, prev = 0;
	int c, rs = -1, us = -1, rc = CL_FAIL, done = 0, stop = 0;
	u16 seq = 0;
	char a[16], name[128];

	memset(&g, 0, sizeof(g));
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage();
		return CL_OK;
	}
	while ((c = cl_getopt(argc, argv, "nIm:q:w:p:f:N:z:4", &g)) != -1) {
		switch (c) {
		case 'n': opt_n = 1; break;
		case 'I': opt_icmp = 1; break;
		case 'm': if (cl_parse_uint(g.arg, 255, &max_ttl) < 0 || !max_ttl) goto bad; break;
		case 'q': if (cl_parse_uint(g.arg, 10, &nq) < 0 || !nq) goto bad; break;
		case 'w': if (cl_parse_ms(g.arg, &wait_ms) < 0) goto bad; break;
		case 'p': if (cl_parse_uint(g.arg, 65535, &base_port) < 0) goto bad; break;
		case 'f': if (cl_parse_uint(g.arg, 255, &first_ttl) < 0 || !first_ttl) goto bad; break;
		case 'N': case 'z': case '4': break;
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
	if (plen < 28 + 8)
		plen = 28 + 8;
	if (first_ttl > max_ttl) {
		cl_puts("first hop out of range\n");
		return CL_ERROR;
	}
	if (cl_need_timer() < 0 || cl_need_socket() < 0)
		return CL_FAIL;
	if (cl_resolve(host, &dst) < 0)
		return CL_ERROR;
	rbuf = AllocVec(RBUF, MEMF_ANY);
	pkt = AllocVec(plen, MEMF_ANY | MEMF_CLEAR);
	if (!rbuf || !pkt) {
		cl_error("out of memory");
		goto out;
	}
	rs = socket(RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	if (rs < 0) {
		cl_error("cannot create a raw ICMP socket (errno %ld)", (long)Errno());
		goto out;
	}
	ident = (u16)(((u32)FindTask(NULL) >> 4) | 0x8000);
	if (!opt_icmp) {
		struct rt_sockaddr_in me;
		LONG l = sizeof(me);
		us = socket(RT_AF_INET, RT_SOCK_DGRAM, 0);
		sin_set(&me, 0, 0);
		if (us < 0 || bind(us, &me, sizeof(me)) < 0 || getsockname(us, &me, &l) < 0) {
			cl_error("cannot create a UDP socket");
			goto out;
		}
		sport = rt_ntohs(me.sin_port);
	}
	cl_printf("traceroute to %s (%s), %lu hops max, %lu byte packets\n", host, cl_ip(dst, a),
	          (unsigned long)max_ttl, (unsigned long)plen);
	cl_flush();
	rc = CL_OK;
	for (ttl = first_ttl; ttl <= max_ttl && !done && !stop; ttl++) {
		LONG t = (LONG)ttl;
		u32 q;
		int unreach = 0;

		cl_printf("%2lu ", (unsigned long)ttl);
		prev = 0;
		if (setsockopt(opt_icmp ? rs : us, RT_IPPROTO_IP, RT_IP_TTL, &t, sizeof(t)) < 0) {
			cl_error("setsockopt IP_TTL: errno %ld", (long)Errno());
			rc = CL_FAIL;
			break;
		}
		for (q = 0; q < nq; q++) {
			u32 from = 0, rtt = 0, t0;
			int type, code = 0;
			u16 key;
			LONG r;

			seq++;
			if (opt_icmp) {
				key = seq;
				memset(pkt, 0, plen - 20);
				pkt[0] = RT_ICMP_ECHO;
				rt_put_be16(pkt + 4, ident);
				rt_put_be16(pkt + 6, key);
				rt_put_be16(pkt + 2, cksum(pkt, plen - 20));
				sin_set(&to, dst, 0);
				t0 = cl_now_us();
				r = sendto(rs, pkt, (LONG)(plen - 20), 0, &to, sizeof(to));
			} else {
				key = (u16)(base_port + seq - 1);
				sin_set(&to, dst, key);
				t0 = cl_now_us();
				r = sendto(us, pkt, (LONG)(plen - 28), 0, &to, sizeof(to));
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
			type = wait_reply(rs, dst, key, t0, &from, &rtt, &code);
			if (type == -2) {
				stop = 1;
				break;
			}
			if (type < 0) {
				cl_puts(" *");
				cl_flush();
				continue;
			}
			if (from != prev) {
				if (opt_n)
					cl_printf(" %s", cl_ip(from, a));
				else
					cl_printf(" %s (%s)", cl_hostname(from, 0, name, sizeof(name)), cl_ip(from, a));
				prev = from;
			}
			cl_printf("  %lu.%03lu ms", (unsigned long)(rtt / 1000), (unsigned long)(rtt % 1000));
			if (type == RT_ICMP_ECHOREPLY) {
				done = 1;
			} else if (type == RT_ICMP_UNREACH) {
				switch (code) {
				case RT_ICMP_UNREACH_PORT:
					if (from != dst && ttl <= 1)
						cl_puts(" !");
					done = 1;
					break;
				case RT_ICMP_UNREACH_NET: cl_puts(" !N"); unreach++; break;
				case RT_ICMP_UNREACH_HOST: cl_puts(" !H"); unreach++; break;
				case RT_ICMP_UNREACH_PROTOCOL: cl_puts(" !P"); done = 1; break;
				case RT_ICMP_UNREACH_NEEDFRAG: cl_printf(" !F-%u", 0U); unreach++; break;
				case RT_ICMP_UNREACH_SRCFAIL: cl_puts(" !S"); unreach++; break;
				case RT_ICMP_UNREACH_ADMIN: cl_puts(" !X"); unreach++; break;
				default: cl_printf(" !<%d>", code); unreach++; break;
				}
			}
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
	cl_printf("traceroute: bad value '%s'\n", g.arg ? g.arg : argv[g.ind + 1]);
	usage();
	return CL_ERROR;
}

CL_MAIN(tr_main, "traceroute")
