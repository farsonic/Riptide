/*
 * Riptide - ping: ICMP echo client for AmigaOS. Uses only the standard
 * bsdsocket.library API (a raw ICMP socket), so it works with any
 * AmiTCP-compatible stack.
 *
 *   ping HOST [COUNT=n] [SIZE=bytes] [INTERVAL=ms] [TIMEOUT=ms] [QUIET]
 *
 * Ctrl-C stops early and prints the summary. Exit code 0 if any reply
 * arrived, 5 if none, 20 on errors.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_os.h"
#include "amiga_net.h"
#include "rt/rt_types.h"

struct Library *SocketBase;
struct Device *TimerBase;

#include "gen/bsdsocket_calls.h"

#define TEMPLATE "HOST/A,COUNT=C/K/N,SIZE=S/K/N,INTERVAL=I/K/N,TIMEOUT=W/K/N,QUIET=Q/S"
enum { A_HOST, A_COUNT, A_SIZE, A_INTERVAL, A_TIMEOUT, A_QUIET, A_NUM };

#define AF_INET      2
#define SOCK_RAW     3
#define IPPROTO_ICMP 1
#define ICMP_ECHOREPLY 0
#define ICMP_ECHO      8
#define MAX_PAYLOAD  1472           /* fits a 1500 byte MTU unfragmented */
#define HDR          8              /* ICMP echo header */

struct sin {
	UBYTE sin_len, sin_family;
	UWORD sin_port;
	ULONG sin_addr;
	UBYTE sin_zero[8];
};
struct tv {
	ULONG secs, micro;
};

static void out(const char *fmt, ...)
{
	char b[200];
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	PutStr((CONST_STRPTR)b);
}

static ULONG now_us_lo(struct timeval *t)
{
	GetSysTime(t);
	return (ULONG)t->tv_secs * 1000000UL + t->tv_micro;
}

static UWORD cksum(const UBYTE *p, LONG n)
{
	ULONG s = 0;

	for (; n > 1; p += 2, n -= 2)
		s += ((ULONG)p[0] << 8) | p[1];
	if (n)
		s += (ULONG)p[0] << 8;
	s = (s & 0xffff) + (s >> 16);
	s = (s & 0xffff) + (s >> 16);
	return (UWORD)~s;
}

/* microseconds -> "12.345" ms */
static void fmt_ms(char *b, ULONG us)
{
	rt_snprintf(b, 16, "%lu.%03lu", (unsigned long)(us / 1000), (unsigned long)(us % 1000));
}

static int ping_main(void)
{
	LONG args[A_NUM] = { 0 };
	struct RDArgs *rda;
	struct timerequest *tr = NULL;
	struct MsgPort *tport = NULL;
	struct timeval t;
	struct sin dst, from;
	UBYTE *pkt = NULL, *rbuf = NULL;
	LONG s = -1, count, size, interval, timeout;
	ULONG target, id, sent = 0, recvd = 0, min = 0xffffffffUL, max = 0, sum = 0;
	int quiet, rc = 20, stop = 0;
	char a[16], b[16], c[16];

	rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"ping");
		return 20;
	}
	count = args[A_COUNT] ? *(LONG *)args[A_COUNT] : 4;
	size = args[A_SIZE] ? *(LONG *)args[A_SIZE] : 56;
	interval = args[A_INTERVAL] ? *(LONG *)args[A_INTERVAL] : 1000;
	timeout = args[A_TIMEOUT] ? *(LONG *)args[A_TIMEOUT] : 2000;
	quiet = args[A_QUIET] != 0;
	if (size < 8)
		size = 8;          /* room for the send timestamp */
	if (size > MAX_PAYLOAD)
		size = MAX_PAYLOAD;
	if (interval < 10)
		interval = 10;

	tport = CreateMsgPort();
	tr = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*tr)) : NULL;
	if (!tr || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)tr, 0)) {
		PutStr((CONST_STRPTR)"ping: cannot open timer.device\n");
		if (tr)
			DeleteIORequest((struct IORequest *)tr);
		tr = NULL;
		goto done;
	}
	TimerBase = tr->tr_node.io_Device;

	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) {
		PutStr((CONST_STRPTR)"ping: no TCP/IP stack running (cannot open bsdsocket.library 4)\n");
		goto done;
	}

	target = inet_addr((CONST_STRPTR)args[A_HOST]);
	if (target == 0xffffffffUL) {
		struct hostent *h = (struct hostent *)gethostbyname((CONST_STRPTR)args[A_HOST]);
		if (!h || !h->h_addr_list || !h->h_addr_list[0]) {
			out("ping: unknown host %s\n", (char *)args[A_HOST]);
			goto done;
		}
		CopyMem(h->h_addr_list[0], &target, 4);
	}

	s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (s < 0) {
		out("ping: cannot create raw ICMP socket (errno %ld)\n", (long)Errno());
		goto done;
	}
	pkt = AllocVec(HDR + MAX_PAYLOAD, MEMF_ANY | MEMF_CLEAR);
	rbuf = AllocVec(60 + HDR + MAX_PAYLOAD + 64, MEMF_ANY);
	if (!pkt || !rbuf) {
		PutStr((CONST_STRPTR)"ping: out of memory\n");
		goto done;
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin_len = sizeof(dst);
	dst.sin_family = AF_INET;
	dst.sin_addr = target;
	id = ((ULONG)FindTask(NULL) >> 4) & 0xffff;
	rt_strlcpy(a, (const char *)Inet_NtoA(target), sizeof(a));
	out("PING %s (%s): %ld data bytes\n", (char *)args[A_HOST], a, (long)size);

	while (!stop && (count <= 0 || (LONG)sent < count)) {
		ULONG t0, deadline, seq = sent & 0xffff;
		LONG i, got = 0;

		/* echo request: type, code, checksum, id, seq, payload (timestamp first) */
		pkt[0] = ICMP_ECHO;
		pkt[1] = 0;
		pkt[2] = pkt[3] = 0;
		pkt[4] = (UBYTE)(id >> 8);
		pkt[5] = (UBYTE)id;
		pkt[6] = (UBYTE)(seq >> 8);
		pkt[7] = (UBYTE)seq;
		t0 = now_us_lo(&t);
		CopyMem(&t0, pkt + HDR, 4);
		for (i = 4; i < size; i++)
			pkt[HDR + i] = (UBYTE)i;
		i = cksum(pkt, HDR + size);
		pkt[2] = (UBYTE)(i >> 8);
		pkt[3] = (UBYTE)i;
		sent++;
		if (sendto(s, pkt, HDR + size, 0, &dst, sizeof(dst)) < 0) {
			LONG e = Errno();
			out("ping: icmp_seq %lu: %s\n", (unsigned long)seq,
			    e == 51 ? "network is unreachable" : e == 65 ? "no route to host" :
			    e == 64 ? "host is down" : e == 55 ? "no buffer space" : "send failed");
			got = -1;
		}

		/* wait for the reply until the timeout, then pad to the interval */
		deadline = t0 + (ULONG)timeout * 1000UL;
		while (got == 0) {
			ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C, nowt = now_us_lo(&t), left;
			struct tv tmo;
			LONG n, fl = sizeof(from);

			if ((LONG)(deadline - nowt) <= 0)
				break;
			left = deadline - nowt;
			tmo.secs = left / 1000000UL;
			tmo.micro = left % 1000000UL;
			rd[s / 32] |= 1UL << (s % 32);
			n = WaitSelect(s + 1, rd, NULL, NULL, &tmo, &sigs);
			if (sigs & SIGBREAKF_CTRL_C) {
				stop = 1;
				break;
			}
			if (n <= 0)
				break;
			n = recvfrom(s, rbuf, 60 + HDR + MAX_PAYLOAD + 64, 0, &from, &fl);
			if (n >= 20) {
				/* raw sockets deliver the IP header too */
				LONG hl = (rbuf[0] & 0x0f) * 4;
				UBYTE *ic = rbuf + hl;
				ULONG rt, ttl = rbuf[8];

				if (n < hl + HDR || ic[0] != ICMP_ECHOREPLY)
					continue;
				if ((((ULONG)ic[4] << 8) | ic[5]) != id)
					continue;      /* someone else's ping */
				if ((((ULONG)ic[6] << 8) | ic[7]) != seq)
					continue;      /* late reply to an earlier probe */
				rt = now_us_lo(&t) - t0;
				recvd++;
				sum += rt;
				if (rt < min)
					min = rt;
				if (rt > max)
					max = rt;
				if (!quiet) {
					fmt_ms(b, rt);
					rt_strlcpy(c, (const char *)Inet_NtoA(from.sin_addr), sizeof(c));
					out("%ld bytes from %s: icmp_seq=%lu ttl=%lu time=%s ms\n",
					    (long)(n - hl), c, (unsigned long)seq, (unsigned long)ttl, b);
				}
				got = 1;
				break;
			}
		}
		if (got == 0 && !stop && !quiet)
			out("Request timeout for icmp_seq %lu\n", (unsigned long)seq);
		if (stop || (count > 0 && (LONG)sent >= count))
			break;
		/* sleep the rest of the interval, still honouring Ctrl-C */
		{
			ULONG el = now_us_lo(&t) - t0, wait = (ULONG)interval * 1000UL;
			if (el < wait) {
				struct tv tmo;
				ULONG sigs = SIGBREAKF_CTRL_C;

				tmo.secs = (wait - el) / 1000000UL;
				tmo.micro = (wait - el) % 1000000UL;
				WaitSelect(0, NULL, NULL, NULL, &tmo, &sigs);
				if (sigs & SIGBREAKF_CTRL_C)
					stop = 1;
			}
		}
	}

	out("--- %s ping statistics ---\n", a);
	out("%lu packets transmitted, %lu packets received, %lu%% packet loss\n",
	    (unsigned long)sent, (unsigned long)recvd,
	    (unsigned long)(sent ? (sent - recvd) * 100 / sent : 0));
	if (recvd) {
		fmt_ms(a, min);
		fmt_ms(b, sum / recvd);
		fmt_ms(c, max);
		out("round-trip min/avg/max = %s/%s/%s ms\n", a, b, c);
	}
	rc = recvd ? 0 : 5;
done:
	if (s >= 0)
		CloseSocket(s);
	if (SocketBase)
		CloseLibrary(SocketBase);
	if (pkt)
		FreeVec(pkt);
	if (rbuf)
		FreeVec(rbuf);
	if (tr) {
		CloseDevice((struct IORequest *)tr);
		DeleteIORequest((struct IORequest *)tr);
	}
	if (tport)
		DeleteMsgPort(tport);
	FreeArgs(rda);
	return rc;
}

#if defined(__VBCC__)
int main(void)
{
	return ping_main();
}
#else
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

long amiga_main(char *argstr, long arglen)
{
	int rc;

	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	if (!DOSBase)
		return 20;
	rc = ping_main();
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
#endif
