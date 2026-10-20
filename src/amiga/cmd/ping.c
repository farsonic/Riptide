/*
 * Riptide - ping: ICMP echo client for AmigaOS. Uses only the standard
 * bsdsocket.library API (a raw ICMP socket), so it works with any
 * AmiTCP-compatible stack. Accepts both the Amiga keywords and Linux
 * options:
 *
 *   ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=ms] [TIMEOUT=ms] [QUIET]
 *   ping [-c count] [-s size] [-i seconds] [-W seconds] [-t ttl] [-q] [-n] HOST
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

static u32 bitmap[SEQ_BITS / 32];
static u32 recvd, dups, errors, sent, min_us = 0xffffffffUL, max_us;
static u64 sum_us, sum2_us;
static int quiet;
static u16 ident;
static u8 *rbuf;

static void usage(void)
{
	cl_puts("Usage: ping [-c count] [-s size] [-i interval] [-W timeout] [-t ttl] [-qn] HOST\n"
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
		if (rt_get_be16(ic + 4) != ident || from != dst)
			return;
		seq = rt_get_be16(ic + 6);
		memcpy(&t0, ic + HDR, 4);
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
			cl_printf("%ld bytes from %s: icmp_seq=%lu ttl=%u time=%s ms%s\n", (long)(n - hl),
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

/* Receive until 'until' (cl_now_us). Returns 1 on Ctrl-C. */
static int receive_until(int s, u32 until, u32 dst, u32 stop_after)
{
	for (;;) {
		u32 now = cl_now_us(), left;
		ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
		struct tv tmo;
		struct rt_sockaddr_in from;
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
		if (n <= 0)
			continue;
		n = recvfrom(s, rbuf, RBUF, 0, &from, &fl);
		if (n >= 20)
			handle(n, from.sin_addr, dst);
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
	u32 count = 4, size = 56, interval_ms = 1000, timeout_ms = 2000, ttl = 0, dst, x;
	struct rt_sockaddr_in to;
	u8 *pkt = NULL;
	int s = -1, i, rc = CL_FAIL, stop = 0;
	u32 start;
	char a[16], b[20], c[20], d[20], e[20];

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
				case 'n': case '4': break;
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
	if (cl_resolve(host, &dst) < 0)
		return CL_FAIL;
	s = socket(RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
	if (s < 0) {
		cl_error("cannot create raw ICMP socket (errno %ld)", (long)Errno());
		return CL_FAIL;
	}
	if (ttl) {
		LONG t = (LONG)ttl;
		setsockopt(s, RT_IPPROTO_IP, RT_IP_TTL, &t, sizeof(t));
	}
	pkt = AllocVec(HDR + MAX_PAYLOAD, MEMF_ANY | MEMF_CLEAR);
	rbuf = AllocVec(RBUF, MEMF_ANY);
	if (!pkt || !rbuf) {
		cl_error("out of memory");
		goto done;
	}
	memset(&to, 0, sizeof(to));
	to.sin_len = sizeof(to);
	to.sin_family = RT_AF_INET;
	to.sin_addr = dst;
	ident = (u16)((u32)FindTask(NULL) >> 4);
	cl_printf("PING %s (%s) %lu(%lu) bytes of data.\n", host, cl_ip(dst, a), (unsigned long)size,
	          (unsigned long)(size + 28));
	cl_flush();
	start = cl_now_us();
	while (!stop && (count == 0 || sent < count)) {
		u32 t0, seq = (sent + 1) & 0xffff, k;

		pkt[0] = RT_ICMP_ECHO;
		pkt[1] = 0;
		pkt[2] = pkt[3] = 0;
		rt_put_be16(pkt + 4, ident);
		rt_put_be16(pkt + 6, (u16)seq);
		t0 = cl_now_us();
		memcpy(pkt + HDR, &t0, 4);
		for (k = 4; k < size; k++)
			pkt[HDR + k] = (u8)k;
		rt_put_be16(pkt + 2, cksum(pkt, HDR + size));
		bitmap[(seq % SEQ_BITS) / 32] &= ~(1UL << (seq % 32));
		sent++;
		if (sendto(s, pkt, (LONG)(HDR + size), 0, &to, sizeof(to)) < 0) {
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
	cl_printf(", %lu%% packet loss, time %lums\n",
	          (unsigned long)(sent ? (sent - (recvd > sent ? sent : recvd)) * 100 / sent : 0),
	          (unsigned long)x);
	if (recvd) {
		u64 avg = sum_us / recvd, var = sum2_us / recvd - avg * avg;
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

CL_MAIN(ping_main, "ping")
