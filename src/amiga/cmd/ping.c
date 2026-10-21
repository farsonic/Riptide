/*
 * Riptide - ping: ICMP echo client for AmigaOS. Uses only the standard
 * bsdsocket.library API (a raw ICMP socket), so it works with any
 * AmiTCP-compatible stack. Accepts both the Amiga keywords and Linux
 * options:
 *
 *   ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=ms] [TIMEOUT=ms] [QUIET]
 *   ping [-4|-6] [-c count] [-s size] [-i seconds] [-W seconds] [-t ttl] [-q] [-n] HOST
 *
 * IPv6 (ICMPv6 echo over a raw AF_INET6 socket) with -6, for IPv6 literals
 * ("fe80::1%eth0" too) and when the program is called ping6. Host names
 * resolve to IPv4 unless -6 is given. The hop limit of IPv6 replies is not
 * shown (no IPV6_RECVHOPLIMIT ancillary data).
 *
 * COUNT 0 (-c 0) pings until Ctrl-C; the default is 4. Ctrl-C stops early
 * and prints the summary. Exit code 0 if any reply arrived, 5 if none,
 * 20 on errors.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define MAX_PAYLOAD  1472           /* fits a 1500 byte MTU unfragmented */
#define HDR          8              /* ICMP echo header */
#define RBUF         (60 + HDR + MAX_PAYLOAD + 64)
#define SEQ_BITS     1024

struct tv { ULONG secs, micro; };

static u32 bitmap[SEQ_BITS / 32];     /* replies seen, per sequence number */
static u32 sendtime[SEQ_BITS];        /* cl_now_us() at send, per sequence number */
static u32 corrupt;                   /* replies whose data did not match a probe */
static int shift_noted;
static u32 recvd, dups, errors, sent, min_us = 0xffffffffUL, max_us;
static u64 sum_us, sum2_us;
static int quiet;
static u16 ident;
static u8 *rbuf;
static int v6;                        /* ICMPv6 */
static struct cl_addr dst6;
#ifndef PING_NAME
#define PING_NAME "ping"
#define PING_FAMILY 0
#endif

static void usage(void)
{
	cl_puts("Usage: " PING_NAME " [-4|-6] [-c count] [-s size] [-i interval] [-W timeout] [-t ttl] [-qn] HOST\n"
	        "       ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=ms] [TIMEOUT=ms] [QUIET]\n");
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

static void fmt_ms(char *b, u32 us)
{
	rt_snprintf(b, 20, "%lu.%03lu", (unsigned long)(us / 1000), (unsigned long)(us % 1000));
}

static u32 isqrt64(u64 v)
{
	u64 r = 0, bit = (u64)1 << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (u32)r;
}

static const char *unreach_text(int type, int code)
{
	if (type == RT_ICMP_TIMXCEED)
		return code ? "Frag reassembly time exceeded" : "Time to live exceeded";
	if (type == RT_ICMP_PARAMPROB)
		return "Parameter problem";
	if (type == RT_ICMP_SOURCEQUENCH)
		return "Source Quench";
	if (type == RT_ICMP_REDIRECT)
		return "Redirect";
	switch (code) {
	case 0: return "Destination Net Unreachable";
	case 1: return "Destination Host Unreachable";
	case 2: return "Destination Protocol Unreachable";
	case 3: return "Destination Port Unreachable";
	case 4: return "Frag needed and DF set";
	case 5: return "Source Route Failed";
	case 13: return "Packet filtered";
	}
	return "Dest Unreachable, Bad Code";
}

/* Handle one received datagram. */
static void handle(LONG n, u32 from, u32 dst)
{
	u32 hl = (u32)(rbuf[0] & 0x0f) * 4, seq, t0, rt;
	u8 *ic = rbuf + hl;
	char a[16], b[20];

	if (n < (LONG)(hl + HDR))
		return;
	if (ic[0] == RT_ICMP_ECHOREPLY) {
		int dup;
		u32 off, avail;
		u8 *e = NULL;

		if (rt_get_be16(ic + 4) != ident || from != dst)
			return;
		/* Find our echo header and trust a reply only if its sequence number
		 * was sent and its data carries that probe's send time. RTT comes
		 * from our own send-time table, never from the reply. Offset 20
		 * copes with emulators (old slirp on macOS) that hand back the
		 * host's IP header in front of the ICMP message. */
		for (off = 0; off <= 20; off += 20) {
			avail = (u32)n - hl;
			if (avail < off + HDR + 4 || ic[off] != RT_ICMP_ECHOREPLY)
				continue;
			seq = rt_get_be16(ic + off + 6);
			if (seq == 0 || seq > sent || sent - seq >= SEQ_BITS)
				continue;
			memcpy(&t0, ic + off + HDR, 4);
			if (t0 != sendtime[seq % SEQ_BITS])
				continue;
			e = ic + off;
			break;
		}
		if (!e) {
			corrupt++;
			if (!quiet && corrupt == 1)
				cl_printf("Reply from %s with data that matches no probe (ignored)\n",
				          cl_ip(from, a));
			return;
		}
		if (off && !shift_noted && !quiet) {
			shift_noted = 1;
			cl_printf("(note: replies arrive with an extra IP header - emulator NAT quirk; compensated)\n");
		}
		rt = cl_now_us() - t0;
		dup = (bitmap[(seq % SEQ_BITS) / 32] >> (seq % 32)) & 1;
		bitmap[(seq % SEQ_BITS) / 32] |= 1UL << (seq % 32);
		if (dup) {
			dups++;
		} else {
			recvd++;
			sum_us += rt;
			sum2_us += (u64)rt * rt;
			if (rt < min_us)
				min_us = rt;
			if (rt > max_us)
				max_us = rt;
		}
		if (!quiet) {
			fmt_ms(b, rt);
			cl_printf("%ld bytes from %s: icmp_seq=%lu ttl=%u time=%s ms%s\n", (long)(n - hl - off),
			          cl_ip(from, a), (unsigned long)seq, (unsigned)rbuf[8], b, dup ? " (DUP!)" : "");
		}
		return;
	}
	if (ic[0] == RT_ICMP_UNREACH || ic[0] == RT_ICMP_TIMXCEED || ic[0] == RT_ICMP_PARAMPROB ||
	    ic[0] == RT_ICMP_SOURCEQUENCH || ic[0] == RT_ICMP_REDIRECT) {
		/* error about one of our echo requests? */
		u8 *in = ic + 8, *iq;
		u32 ihl;
		if (n < (LONG)(hl + 8 + 20 + 8))
			return;
		ihl = (u32)(in[0] & 15) * 4;
		iq = in + ihl;
		if (n < (LONG)(hl + 8 + ihl + 8) || in[9] != RT_IPPROTO_ICMP || iq[0] != RT_ICMP_ECHO ||
		    rt_get_be16(iq + 4) != ident || memcmp(in + 16, &dst, 4))
			return;
		if (ic[0] != RT_ICMP_REDIRECT)
			errors++;
		if (!quiet) {
			cl_printf("From %s icmp_seq=%lu %s", cl_ip(from, a), (unsigned long)rt_get_be16(iq + 6),
			          unreach_text(ic[0], ic[1]));
			if (ic[0] == RT_ICMP_UNREACH && ic[1] == 4)
				cl_printf(" (mtu = %lu)", (unsigned long)rt_get_be16(ic + 6));
			cl_puts("\n");
		}
	}
}

static const char *unreach6_text(int type, int code)
{
	if (type == RT_ICMP6_TIME_EXCEEDED)
		return code ? "Time exceeded: Frag reassembly time exceeded" : "Time exceeded: Hop limit";
	if (type == RT_ICMP6_PARAM_PROB)
		return "Parameter problem";
	if (type == RT_ICMP6_PACKET_TOO_BIG)
		return "Packet too big";
	switch (code) {
	case 0: return "Destination unreachable: No route";
	case 1: return "Destination unreachable: Administratively prohibited";
	case 2: return "Destination unreachable: Beyond scope of source address";
	case 3: return "Destination unreachable: Address unreachable";
	case 4: return "Destination unreachable: Port unreachable";
	case 5: return "Destination unreachable: Source address failed ingress/egress policy";
	case 6: return "Destination unreachable: Reject route to destination";
	}
	return "Destination unreachable: Unknown code";
}

/* One ICMPv6 message (raw IPv6 sockets deliver no IP header). */
static void handle6(LONG n, const struct cl_addr *from)
{
	u8 *ic = rbuf;
	char a[CL_ADDRSTRLEN], b[20];
	u32 seq, t0, rt;

	if (n < HDR)
		return;
	if (ic[0] == RT_ICMP6_ECHO_REPLY) {
		int dup;
		if (rt_get_be16(ic + 4) != ident || n < HDR + 4)
			return;
		if (!rt_in6_is_mcast(&dst6.a) && !rt_in6_eq(&from->a, &dst6.a))
			return;
		seq = rt_get_be16(ic + 6);
		memcpy(&t0, ic + HDR, 4);
		if (seq == 0 || seq > sent || sent - seq >= SEQ_BITS || t0 != sendtime[seq % SEQ_BITS]) {
			corrupt++;
			return;
		}
		rt = cl_now_us() - t0;
		dup = (bitmap[(seq % SEQ_BITS) / 32] >> (seq % 32)) & 1;
		bitmap[(seq % SEQ_BITS) / 32] |= 1UL << (seq % 32);
		if (dup) {
			dups++;
		} else {
			recvd++;
			sum_us += rt;
			sum2_us += (u64)rt * rt;
			if (rt < min_us)
				min_us = rt;
			if (rt > max_us)
				max_us = rt;
		}
		if (!quiet) {
			fmt_ms(b, rt);
			cl_printf("%ld bytes from %s: icmp_seq=%lu time=%s ms%s\n", (long)n, cl_addr_str(from, a),
			          (unsigned long)seq, b, dup ? " (DUP!)" : "");
		}
		return;
	}
	if (ic[0] >= RT_ICMP6_DST_UNREACH && ic[0] <= RT_ICMP6_PARAM_PROB) {
		/* the invoking packet: IPv6 header (no extension headers) + our echo request */
		u8 *in = ic + 8, *iq = in + 40;
		if (n < 8 + 40 + 8 || in[6] != RT_IPPROTO_ICMPV6 || iq[0] != RT_ICMP6_ECHO_REQUEST ||
		    rt_get_be16(iq + 4) != ident || memcmp(in + 24, dst6.a.b, 16))
			return;
		errors++;
		if (!quiet) {
			cl_printf("From %s icmp_seq=%lu %s", cl_addr_str(from, a), (unsigned long)rt_get_be16(iq + 6),
			          unreach6_text(ic[0], ic[1]));
			if (ic[0] == RT_ICMP6_PACKET_TOO_BIG)
				cl_printf(" (mtu = %lu)", (unsigned long)rt_get_be32(ic + 4));
			cl_puts("\n");
		}
	}
}

/* Receive until 'until' (cl_now_us). Returns 1 on Ctrl-C. */
static int receive_until(int s, u32 until, u32 dst, u32 stop_after)
{
	for (;;) {
		u32 now = cl_now_us(), left;
		ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
		struct tv tmo;
		struct rt_sockaddr_in6 from;
		LONG fl = sizeof(from), n;

		if (stop_after && recvd + errors >= stop_after)
			return 0;
		if ((s32)(until - now) <= 0)
			return 0;
		left = until - now;
		tmo.secs = left / 1000000UL;
		tmo.micro = left % 1000000UL;
		rd[s / 32] |= 1UL << (s % 32);
		n = WaitSelect(s + 1, rd, NULL, NULL, &tmo, &sigs);
		if (sigs & SIGBREAKF_CTRL_C)
			return 1;
		if (n < 0) {
			if (Errno() == RT_EINTR || cl_break())
				return 1;
			return 0;            /* select failed: give up this wait */
		}
		if (n == 0)
			continue;
		n = recvfrom(s, rbuf, RBUF, 0, &from, &fl);
		if (v6) {
			struct cl_addr fa;
			cl_from_sockaddr(&from, &fa, NULL);
			if (n >= HDR)
				handle6(n, &fa);
		} else if (n >= 20) {
			handle(n, ((struct rt_sockaddr_in *)&from)->sin_addr, dst);
		}
		cl_flush();
	}
}

/* keyword=value (Amiga template style), also "KEY value" */
static const char *kw(int argc, char **argv, int *i, const char *full, const char *alias)
{
	const char *a = argv[*i], *eq = strchr(a, '=');
	u32 n = eq ? (u32)(eq - a) : (u32)strlen(a);

	if (!((n == strlen(full) && !rt_strncasecmp(a, full, n)) ||
	      (alias && n == strlen(alias) && !rt_strncasecmp(a, alias, n))))
		return NULL;
	if (eq)
		return eq + 1;
	if (*i + 1 < argc)
		return argv[++(*i)];
	return NULL;
}

static int ping_main(int argc, char **argv)
{
	const char *host = NULL, *v;
	u32 count = 4, size = 56, interval_ms = 1000, timeout_ms = 2000, ttl = 0, dst = 0, x, tolen;
	int family = PING_FAMILY;
	struct rt_sockaddr_in6 to;
	u8 *pkt = NULL;
	int s = -1, i, rc = CL_FAIL, stop = 0;
	u32 start;
	char a[CL_ADDRSTRLEN], b[20], c[20], d[20], e[20];

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (arg[0] == '-' && arg[1]) {
			const char *o = arg + 1;
			if (!strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "-?")) {
				usage();
				return CL_OK;
			}
			for (; *o; o++) {
				const char *val = NULL;
				if (strchr("csiWtwl", *o)) {
					val = o[1] ? o + 1 : i + 1 < argc ? argv[++i] : NULL;
					if (!val) {
						cl_printf("ping: option requires an argument -- '%c'\n", *o);
						return CL_ERROR;
					}
				}
				switch (*o) {
				case 'c': if (cl_parse_uint(val, 0x7fffffff, &count) < 0) goto bad; break;
				case 's': if (cl_parse_uint(val, 65507, &size) < 0) goto bad; break;
				case 'i': if (cl_parse_ms(val, &interval_ms) < 0) goto bad; break;
				case 'W': if (cl_parse_ms(val, &timeout_ms) < 0) goto bad; break;
				case 't': if (cl_parse_uint(val, 255, &ttl) < 0 || !ttl) goto bad; break;
				case 'w': case 'l': break;
				case 'q': quiet = 1; break;
				case 'n': break;
				case '4': family = 4; break;
				case '6': family = 6; break;
				default:
					cl_printf("ping: invalid option -- '%c'\n", *o);
					usage();
					return CL_ERROR;
				}
				if (val)
					break;
			}
			continue;
		}
		if ((v = kw(argc, argv, &i, "COUNT", "C")) != NULL) {
			if (cl_parse_uint(v, 0x7fffffff, &count) < 0) goto bad;
		} else if ((v = kw(argc, argv, &i, "SIZE", "S")) != NULL) {
			if (cl_parse_uint(v, 65507, &size) < 0) goto bad;
		} else if ((v = kw(argc, argv, &i, "INTERVAL", "I")) != NULL) {
			if (cl_parse_uint(v, 3600000, &interval_ms) < 0) goto bad;
		} else if ((v = kw(argc, argv, &i, "TIMEOUT", "W")) != NULL) {
			if (cl_parse_uint(v, 3600000, &timeout_ms) < 0) goto bad;
		} else if (!rt_strcasecmp(arg, "QUIET") || !rt_strcasecmp(arg, "Q")) {
			quiet = 1;
		} else if (!rt_strcasecmp(arg, "HOST") && i + 1 < argc) {
			host = argv[++i];
		} else if (!host) {
			host = arg;
		} else {
			cl_printf("ping: unexpected argument '%s'\n", arg);
			usage();
			return CL_ERROR;
		}
	}
	if (!host) {
		usage();
		return CL_ERROR;
	}
	if (size < 8)
		size = 8;          /* room for the send timestamp */
	if (size > MAX_PAYLOAD)
		size = MAX_PAYLOAD;
	if (interval_ms < 10)
		interval_ms = 10;
	if (cl_need_timer() < 0 || cl_need_socket() < 0)
		return CL_FAIL;
	if (family == 6 || (family == 0 && cl_parse_addr(host, &dst6) == 0 && dst6.family == RT_AF_INET6)) {
		if (cl_resolve_af(host, 6, &dst6) < 0)
			return CL_FAIL;
		v6 = 1;
		if (size > MAX_PAYLOAD - 20)
			size = MAX_PAYLOAD - 20;
	} else if (cl_resolve(host, &dst) < 0) {
		return CL_FAIL;
	}
	s = v6 ? socket(RT_AF_INET6, RT_SOCK_RAW, RT_IPPROTO_ICMPV6) : socket(RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	if (s < 0) {
		cl_error("cannot create raw %s socket (errno %ld)", v6 ? "ICMPv6" : "ICMP", (long)Errno());
		return CL_FAIL;
	}
	if (ttl) {
		LONG t = (LONG)ttl;
		if (v6)
			setsockopt(s, RT_IPPROTO_IPV6, RT_IPV6_UNICAST_HOPS, &t, sizeof(t));
		else
			setsockopt(s, RT_IPPROTO_IP, RT_IP_TTL, &t, sizeof(t));
	}
	pkt = AllocVec(HDR + MAX_PAYLOAD, MEMF_ANY | MEMF_CLEAR);
	rbuf = AllocVec(RBUF, MEMF_ANY);
	if (!pkt || !rbuf) {
		cl_error("out of memory");
		goto done;
	}
	if (v6) {
		tolen = cl_sockaddr(&dst6, 0, &to);
	} else {
		struct cl_addr d4;
		d4.family = RT_AF_INET;
		d4.scope = 0;
		rt_in6_set_v4mapped(&d4.a, dst);
		tolen = cl_sockaddr(&d4, 0, &to);
	}
	ident = (u16)((u32)FindTask(NULL) >> 4);
	if (v6)
		cl_printf("PING %s(%s) %lu data bytes\n", host, cl_addr_str(&dst6, a), (unsigned long)size);
	else
		cl_printf("PING %s (%s) %lu(%lu) bytes of data.\n", host, cl_ip(dst, a), (unsigned long)size,
		          (unsigned long)(size + 28));
	cl_flush();
	start = cl_now_us();
	while (!stop && (count == 0 || sent < count)) {
		u32 t0, seq = (sent + 1) & 0xffff, k;

		pkt[0] = v6 ? RT_ICMP6_ECHO_REQUEST : RT_ICMP_ECHO;
		pkt[1] = 0;
		pkt[2] = pkt[3] = 0;
		rt_put_be16(pkt + 4, ident);
		rt_put_be16(pkt + 6, (u16)seq);
		t0 = cl_now_us();
		memcpy(pkt + HDR, &t0, 4);
		sendtime[seq % SEQ_BITS] = t0;
		for (k = 4; k < size; k++)
			pkt[HDR + k] = (u8)k;
		if (!v6)                /* the stack computes the ICMPv6 checksum */
			rt_put_be16(pkt + 2, cksum(pkt, HDR + size));
		bitmap[(seq % SEQ_BITS) / 32] &= ~(1UL << (seq % 32));
		sent++;
		if (sendto(s, pkt, (LONG)(HDR + size), 0, &to, (LONG)tolen) < 0) {
			LONG err = Errno();
			cl_printf("ping: sendmsg: %s\n", cl_strerror((int)err));
			errors++;
		}
		cl_flush();
		if (count && sent >= count)
			break;
		stop = receive_until(s, t0 + interval_ms * 1000UL, dst, 0);
	}
	/* wait for the outstanding replies (all answered: stop at once) */
	if (!stop)
		stop = receive_until(s, cl_now_us() + timeout_ms * 1000UL, dst, sent);
	x = (cl_now_us() - start) / 1000;
	cl_printf("\n--- %s ping statistics ---\n", host);
	cl_printf("%lu packets transmitted, %lu received", (unsigned long)sent, (unsigned long)recvd);
	if (dups)
		cl_printf(", +%lu duplicates", (unsigned long)dups);
	if (errors)
		cl_printf(", +%lu errors", (unsigned long)errors);
	if (corrupt)
		cl_printf(", +%lu unmatched", (unsigned long)corrupt);
	cl_printf(", %lu%% packet loss, time %lums\n",
	          (unsigned long)(sent ? (sent - (recvd > sent ? sent : recvd)) * 100 / sent : 0),
	          (unsigned long)x);
	if (recvd) {
		u64 avg = sum_us / recvd, sq = sum2_us / recvd;
		u64 var = sq > avg * avg ? sq - avg * avg : 0;
		fmt_ms(b, min_us);
		fmt_ms(c, (u32)avg);
		fmt_ms(d, max_us);
		fmt_ms(e, isqrt64(var));
		cl_printf("rtt min/avg/max/mdev = %s/%s/%s/%s ms\n", b, c, d, e);
	}
	rc = recvd ? CL_OK : CL_WARN;
done:
	if (s >= 0)
		CloseSocket(s);
	if (pkt)
		FreeVec(pkt);
	if (rbuf)
		FreeVec(rbuf);
	return rc;
bad:
	cl_printf("ping: invalid argument: '%s'\n", argv[i]);
	return CL_ERROR;
}

CL_MAIN(ping_main, PING_NAME)
