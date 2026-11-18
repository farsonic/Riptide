/* tests/mac/mactcp_test.c - Riptide on the classic Mac OS: a MacTCP program that tests Riptide
 * (stages 2 and 3 of docs/MAC.md) the way MacTCP programs use it: the .IPP driver's UDP and TCP
 * calls, synchronous, and the resolver through the 'dnrp' code of the Riptide DNR file, loaded as
 * Apple's DNR glue (dnr.c) loads it. Against the Aurora68 testbed peer (tools/pinet.py: echo on
 * UDP and TCP port 7, DNS for *.testbed): each step's result goes to the window and, built with
 * AURORA68_LOG, to the Pi's log as "[MAC] MacTCP test: ...". Startup Items is a good place for it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "toolbox.h"

typedef unsigned long u32;
int rt_snprintf(char *buf, unsigned long size, const char *fmt, ...);

#define PEER 0xC0A84401UL               /* 192.168.68.1 */
static short ipp;
static void *win;
static int line_y = 20, passed, failed;

static void log_line(const char *s)
{
	unsigned char p[256];
#ifdef AURORA68_LOG
	char t[220];
	volatile unsigned char *c = (volatile unsigned char *)((*(volatile unsigned char *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
	u32 n, seq, i;
	rt_snprintf(t, sizeof t, "MacTCP test: %s", s);
	n = (u32)strlen(t);
	*(volatile unsigned short *)(c + 8) = 2;
	*(const char *volatile *)(c + 28) = t;
	*(volatile u32 *)(c + 32) = n;
	seq = *(volatile u32 *)c + 1;
	*(volatile u32 *)c = seq;
	for (i = 0; i < 200000u && *(volatile u32 *)(c + 4) != seq; i++)
		;
#endif
	SetPort(win);
	MoveTo(8, line_y);
	DrawString(mac_pstr(p, s));
	line_y += 13;
}

static void check(int ok, const char *what, long r)
{
	char t[160];
	rt_snprintf(t, sizeof t, "%s %s (%ld)", ok ? "PASS" : "FAIL", what, r);
	log_line(t);
	if (ok)
		passed++;
	else
		failed++;
}

/* a MacTCP call, synchronous: the parameter block's header, csCode, stream, then its own fields */
static short ctl(unsigned char *pb, short cs, u32 stream)
{
	*(short *)(pb + 24) = ipp;
	*(short *)(pb + 26) = cs;
	*(u32 *)(pb + 28) = stream;
	return mac_pb(pb, TRAP_CONTROL);
}

static void test_udp(void)
{
	static char rbuf[4096];
	unsigned char pb[100];
	static const char msg[] = "riptide udp";
	unsigned char wds[12];
	const char *mp = msg;
	u32 s;
	short r;
	memset(pb, 0, sizeof pb);
	*(char **)(pb + 32) = rbuf;
	*(u32 *)(pb + 36) = sizeof rbuf;
	r = ctl(pb, 20, 0);                                     /* UDPCreate */
	s = *(u32 *)(pb + 28);
	check(r == 0 && s, "UDPCreate", r);
	if (r)
		return;
	*(unsigned short *)wds = sizeof msg - 1;
	memcpy(wds + 2, &mp, 4);
	*(unsigned short *)(wds + 6) = 0;
	memset(pb + 32, 0, 60);
	*(u32 *)(pb + 34) = PEER;
	*(unsigned short *)(pb + 38) = 7;
	*(unsigned char **)(pb + 40) = wds;
	r = ctl(pb, 23, s);                                     /* UDPWrite */
	check(r == 0, "UDPWrite to 192.168.68.1:7", r);
	memset(pb + 32, 0, 60);
	*(unsigned short *)(pb + 32) = 5;                       /* timeOut, seconds */
	r = ctl(pb, 21, s);                                     /* UDPRead */
	check(r == 0 && *(unsigned short *)(pb + 44) == sizeof msg - 1 &&
	      !memcmp(*(char **)(pb + 40), msg, sizeof msg - 1) && *(u32 *)(pb + 34) == PEER, "UDPRead: the echo", r);
	if (r == 0)
		ctl(pb, 22, s);                                 /* UDPBfrReturn (rcvBuff still at 40) */
	memset(pb + 32, 0, 60);
	r = ctl(pb, 24, s);                                     /* UDPRelease */
	check(r == 0 && *(char **)(pb + 32) == rbuf, "UDPRelease", r);
}

static void test_tcp(void)
{
	static char rbuf[8192], in[64];
	unsigned char pb[110];
	static const char msg[] = "riptide tcp";
	unsigned char wds[12];
	const char *mp = msg;
	u32 s;
	short r;
	memset(pb, 0, sizeof pb);
	*(char **)(pb + 32) = rbuf;
	*(u32 *)(pb + 36) = sizeof rbuf;
	r = ctl(pb, 30, 0);                                     /* TCPCreate */
	s = *(u32 *)(pb + 28);
	check(r == 0 && s, "TCPCreate", r);
	if (r)
		return;
	memset(pb + 32, 0, 70);
	pb[35] = 10;                                            /* commandTimeoutValue */
	*(u32 *)(pb + 36) = PEER;
	*(unsigned short *)(pb + 40) = 7;
	r = ctl(pb, 32, s);                                     /* TCPActiveOpen */
	check(r == 0 && *(unsigned short *)(pb + 46) != 0, "TCPActiveOpen 192.168.68.1:7", r);
	if (r == 0) {
		*(unsigned short *)wds = sizeof msg - 1;
		memcpy(wds + 2, &mp, 4);
		*(unsigned short *)(wds + 6) = 0;
		memset(pb + 32, 0, 70);
		pb[35] = 1;                                     /* push */
		*(unsigned char **)(pb + 38) = wds;
		r = ctl(pb, 34, s);                             /* TCPSend */
		check(r == 0, "TCPSend", r);
		memset(pb + 32, 0, 70);
		pb[32] = 5;                                     /* commandTimeoutValue */
		*(char **)(pb + 36) = in;
		*(unsigned short *)(pb + 40) = sizeof in;
		r = ctl(pb, 37, s);                             /* TCPRcv */
		check(r == 0 && *(unsigned short *)(pb + 40) == sizeof msg - 1 && !memcmp(in, msg, sizeof msg - 1),
		      "TCPRcv: the echo", r);
		memset(pb + 32, 0, 70);
		r = ctl(pb, 40, s);                             /* TCPStatus */
		check(r == 0 && pb[50] == 8, "TCPStatus: established", r);
		memset(pb + 32, 0, 70);
		r = ctl(pb, 38, s);                             /* TCPClose */
		check(r == 0, "TCPClose", r);
		memset(pb + 32, 0, 70);
		pb[32] = 5;
		*(char **)(pb + 36) = in;
		*(unsigned short *)(pb + 40) = sizeof in;
		r = ctl(pb, 37, s);                             /* TCPRcv after the other end's FIN */
		check(r == -23005, "TCPRcv: connectionClosing after the FIN", r);
	}
	memset(pb + 32, 0, 70);
	r = ctl(pb, 42, s);                                     /* TCPRelease */
	check(r == 0 && *(char **)(pb + 32) == rbuf, "TCPRelease", r);
}

/* the resolver: 'dnrp' 0 of the Riptide DNR file in the System Folder, called as dnr.c calls it */
struct hostInfo { long rtnCode; char cname[255]; char pad; u32 addr[4]; };
typedef long (*dnr_fn)(long sel, ...);
static volatile long done;
void pas_result(void);
__asm__(".globl pas_result\npas_result:\n\tmove.l 4(%sp),%a0\n\tmove.l #1,(%a0)\n\tmove.l (%sp)+,%a1\n\taddq.l #8,%sp\n\tjmp (%a1)\n");

static long wait_done(void)
{
	u32 t0 = LM_TICKS;
	while (!done && LM_TICKS - t0 < 600)
		;
	return done;
}

static void test_dnr(void)
{
	short vref = 0, ref;
	long dir = 0, r;
	unsigned char name[32];
	void **h;
	dnr_fn dnr;
	static struct hostInfo hi;
	char t[200];
	if (mac_findfolder(-32768, MAC_TYPE('m', 'a', 'c', 's'), 0, &vref, &dir) != 0) {
		check(0, "the System Folder", 0);
		return;
	}
	ref = HOpenResFile(vref, dir, mac_pstr(name, "Riptide DNR"), 1);
	check(ref > 0, "the Riptide DNR file", ref);
	if (ref <= 0)
		return;
	h = GetResource(MAC_TYPE('d', 'n', 'r', 'p'), 0);
	check(h != NULL, "'dnrp' 0", 0);
	if (!h)
		return;
	mac_hlock(h);
	DetachResource(h);
	CloseResFile(ref);
	dnr = (dnr_fn)*h;
	r = dnr(1L, (char *)0);                                 /* OPENRESOLVER */
	check(r == 0, "OpenResolver", r);
	r = dnr(3L, "192.168.68.9", &hi, (void *)pas_result, (char *)&done);
	check(r == 0 && hi.addr[0] == 0xC0A84409UL, "StrToAddr of a dotted address: at once", r);
	done = 0;
	r = dnr(3L, "peer.testbed", &hi, (void *)pas_result, (char *)&done);
	check(r == -23042, "StrToAddr peer.testbed: cacheFault, then the result procedure", r);
	if (r == -23042) {
		wait_done();
		rt_snprintf(t, sizeof t, "StrToAddr peer.testbed: %s = %lu.%lu.%lu.%lu", hi.cname, hi.addr[0] >> 24,
		            (hi.addr[0] >> 16) & 255, (hi.addr[0] >> 8) & 255, hi.addr[0] & 255);
		check(done && hi.rtnCode == 0 && hi.addr[0] == PEER, t, hi.rtnCode);
	}
	done = 0;
	r = dnr(6L, PEER, &hi, (void *)pas_result, (char *)&done);
	if (r == -23042)
		wait_done();
	rt_snprintf(t, sizeof t, "AddrToName 192.168.68.1: %s", hi.cname);
	check(done && hi.rtnCode == 0 && !strcmp(hi.cname, "peer.testbed"), t, hi.rtnCode);
	done = 0;
	r = dnr(3L, "nowhere.invalid", &hi, (void *)pas_result, (char *)&done);
	if (r == -23042)
		wait_done();
	check(done && hi.rtnCode == -23045, "StrToAddr nowhere.invalid: authNameErr", hi.rtnCode);
	dnr(2L);                                                /* CLOSERESOLVER */
}

int main(void)
{
	Rect b = { 44, 8, 330, 504 };
	unsigned char pb[80], p[64];
	char t[80];
	win = NewWindow(0, &b, mac_pstr(p, "MacTCP test"), 1, documentProc, (void *)-1, 0, 0);
	SetPort(win);
	TextFont(4);
	TextSize(9);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, ".IPP");
	check(mac_pb(pb, TRAP_OPEN) == 0, "open .IPP", 0);
	ipp = *(short *)(pb + 24);
	test_udp();
	test_tcp();
	test_dnr();
	rt_snprintf(t, sizeof t, "done: %d passed, %d failed", passed, failed);
	log_line(t);
	{
		u32 t0 = LM_TICKS;
		while (LM_TICKS - t0 < 600 && !Button())
			;
	}
	return 0;
}
