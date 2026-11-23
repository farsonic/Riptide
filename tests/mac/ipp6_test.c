/* tests/mac/ipp6_test.c - Riptide on the classic Mac OS: the .IPP6 test ("TCP, UDP and the resolver"
 * over the IPv6 interface, docs/IPP6.md), a Mac program that runs against the Aurora68 testbed's peer
 * (tools/pinet.py at 192.168.68.1 and fd68::1: HTTP on port 80 over IPv4 and IPv6, / a page, /big
 * 256 KB, /reset a reset after the headers; UDP echo on port 7 over both; DNS for *.testbed, where
 * peer.testbed has an A and an AAAA record, v6only.testbed (fd68::80) only AAAA, the other names
 * only A, silent.testbed is never answered; nothing answers fd68::99).
 *
 * Everything is asynchronous, with a completion routine that checks the driver's conventions
 * (ioResult set before it runs, the block in A0 and on the stack), and the events go on meanwhile.
 * The receives of one stream are chained from the completion routine itself, at interrupt time, the
 * way an audio player (Aurora68 Play) reads a stream. Each stream has a notification routine that
 * checks A5, D0 and A2 and that it is never re-entered.
 *
 * What is tested: detection (Gestalt 'ipp6', OpenDriver, ip6GetInfo), Close refused; ip6GetAddrs
 * (IPv4, unique local and link-local addresses, a buffer too small); ip6StrToAddr and ip6AddrToStr;
 * ip6Resolve of a name with both kinds of address in each order and family, of IPv6-only and
 * IPv4-only names, a literal, riptide.conf host lines, a name that does not exist, a server that
 * never answers (timeout), ip6ResolveCancel; ip6ResolveAddr; TCP over IPv6 (a page with
 * TCPNoCopyRcv and TCPRcvBfrReturn, 256 KB with TCPRcv chained from its completion routine,
 * TCPStatus with IPv6 ends, TCPClosing on the server's close, TCPClose and TCPTerminate with
 * TCPULPClose, a synchronous TCPStatus inside the notification routine), the same over IPv4 through
 * an IPv4-mapped address, a reset (TCPTerminate, TCPRemoteAbort), an open refused, falling back from
 * an IPv6 address that does not answer to the IPv4 one; immediate synchronous calls from a completion
 * routine; UDP echo over IPv6 and IPv4 on one stream, UDPDataArrival, UDPMaxMTUSize; TCPPassiveOpen
 * timing out; TCPGlobalInfo; the error results; TCPRelease and UDPRelease. Version 2: ip6ConnectName
 * (a connection by name with the core's Happy Eyeballs: each order, an IPv6-only name, a literal,
 * many.testbed's nine silent IPv6 addresses before IPv4, a refused port, the errors).
 *
 * Results go to the window and, built with AURORA68_LOG, to the Pi's log as "[MAC] IPP6 test: ...".
 * The riptide.conf of the run has `host h6.lan fd68::80` and `host h6.lan 192.168.68.1`.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "toolbox.h"
#include "IPP6.h"

typedef unsigned long u32;
int rt_snprintf(char *buf, unsigned long size, const char *fmt, ...);

#define NOT_DONE 1                      /* a call still in progress when the wait ended */
static const unsigned char PEER6[16] = { 0xfd, 0x68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const unsigned char V6ONLY[16] = { 0xfd, 0x68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80 };
static const unsigned char DEAD6[16] = { 0xfd, 0x68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x99 };
#define PEER4 0xC0A84401UL              /* 192.168.68.1 */
#define MAC4  0xC0A84402UL              /* 192.168.68.2: what the peer's DHCP gives */

static int find_page(const char *b);
static void *win;
static int passed, failed, quit_asked;
void *app_a5;
static short ipp6;

/* ---- the results window: the last lines that fit --------------------------------------------- */
#define MAXL 60
#define LINEW 96
static char lines[MAXL][LINEW];
static int nlines;

static void draw_lines(void)
{
	Rect r = *PORT_RECT(win);
	int vis = (r.bottom - r.top - 4) / 11, first = nlines > vis ? nlines - vis : 0, i;
	unsigned char p[LINEW + 1];
	SetPort(win);
	EraseRect(&r);
	for (i = first; i < nlines; i++) {
		MoveTo(4, 12 + (i - first) * 11);
		DrawString(mac_pstr(p, lines[i]));
	}
}

static void out(const char *s)
{
#ifdef AURORA68_LOG
	char t[220];
	volatile unsigned char *c = (volatile unsigned char *)((*(volatile unsigned char *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
	u32 n, seq, i;
	rt_snprintf(t, sizeof t, "IPP6 test: %s", s);
	n = (u32)strlen(t);
	*(volatile unsigned short *)(c + 8) = 2;
	*(const char *volatile *)(c + 28) = t;
	*(volatile u32 *)(c + 32) = n;
	seq = *(volatile u32 *)c + 1;
	*(volatile u32 *)c = seq;
	for (i = 0; i < 200000u && *(volatile u32 *)(c + 4) != seq; i++)
		;
#endif
	if (nlines == MAXL) {
		memmove(lines[0], lines[1], (MAXL - 1) * LINEW);
		nlines--;
	}
	strncpy(lines[nlines], s, LINEW - 1);
	lines[nlines][LINEW - 1] = 0;
	nlines++;
	draw_lines();
}

static void check(int ok, const char *what, long r)
{
	char t[200];
	rt_snprintf(t, sizeof t, "%s %s (%ld)", ok ? "PASS" : "FAIL", what, r);
	out(t);
	if (ok)
		passed++;
	else
		failed++;
}

static void pump(long sleep)
{
	EventRecord ev;
	if (WaitNextEvent(everyEvent, &ev, sleep, NULL)) {
		if (ev.what == keyDown && (ev.modifiers & cmdKey) && (ev.message & charCodeMask) == 'q')
			quit_asked = 1;
		else if (ev.what == updateEvt && (void *)ev.message == win) {
			BeginUpdate(win);
			draw_lines();
			EndUpdate(win);
		}
	}
}

/* ---- calls ------------------------------------------------------------------------------------ */
/* a block (any of the three) and our bookkeeping after it. The completion routine comp6 sets the
   result, whether ioResult was already the result when it ran (order_ok), whether the block was
   both in A0 and on the stack (stack_ok); then, if 'then' is set, calls it with A5 set to the
   program's (a completion routine runs at interrupt time with A5 not ours), and 'done' unless
   'then' answered 1 (it made the call again: a receive chained from its own completion). */
struct call {
	union { TCP6iopb t; UDP6iopb u; IP6iopb i; unsigned char raw[132]; } pb;   /* 0 */
	volatile short done;            /* 132 */
	volatile short result;          /* 134 */
	volatile short order_ok;        /* 136 */
	volatile short stack_ok;        /* 138 */
	int (*then)(struct call *c);    /* 140 */
	long n;                         /* the chain's bytes, the 'then' functions' own */
	int count;
};
void comp6(void);
__asm__(".globl comp6\ncomp6:\n"
        "\tmove.l %a0,-(%sp)\n"
        "\tmove.w %d0,134(%a0)\n"
        "\tclr.w 136(%a0)\n"
        "\tclr.w 138(%a0)\n"
        "\tcmp.l 8(%sp),%a0\n"                       /* the argument, past the A0 just pushed */
        "\tbne.s 2f\n"
        "\tmove.w #1,138(%a0)\n"
        "2:\tcmp.w 16(%a0),%d0\n"
        "\tbne.s 1f\n"
        "\tcmp.w #1,%d0\n"
        "\tbeq.s 1f\n"
        "\tmove.w #1,136(%a0)\n"
        "1:\tmove.l 140(%a0),%d1\n"
        "\tbeq.s 3f\n"
        "\tmovem.l %d2-%d7/%a2-%a6,-(%sp)\n"
        "\tmove.l app_a5,%a5\n"
        "\tmove.l %a0,-(%sp)\n"
        "\tmove.l %d1,%a1\n"
        "\tjsr (%a1)\n"
        "\taddq.l #4,%sp\n"
        "\tmovem.l (%sp)+,%d2-%d7/%a2-%a6\n"
        "\tmove.l (%sp),%a0\n"
        "\ttst.l %d0\n"
        "\tbne.s 4f\n"
        "3:\tmove.w #1,132(%a0)\n"
        "4:\taddq.l #4,%sp\n"
        "\trts\n");
short pb_async(void *pb);
short pb_sync(void *pb);
short pb_close(void *pb);
__asm__(".globl pb_async\npb_async:\n\tmove.l 4(%sp),%a0\n\t.short 0xA404\n\text.l %d0\n\trts\n"
        ".globl pb_sync\npb_sync:\n\tmove.l 4(%sp),%a0\n\t.short 0xA004\n\text.l %d0\n\trts\n"
        ".globl pb_close\npb_close:\n\tmove.l 4(%sp),%a0\n\t.short 0xA001\n\text.l %d0\n\trts\n");
static int completions, order_bad, stack_bad;

static void start(struct call *c, short cs)
{
	c->pb.i.ioCRefNum = ipp6;
	c->pb.i.csCode = cs;
	c->pb.i.ioCompletion = (IP6IOCompletionUPP)comp6;
	c->done = 0;
	pb_async(&c->pb);
}
static short wait_call(struct call *c, u32 ticks)
{
	u32 t0 = TickCount();
	while (!c->done && TickCount() - t0 < ticks)
		pump(1);
	if (!c->done)
		return NOT_DONE;
	completions++;
	if (!c->order_ok)
		order_bad++;
	if (!c->stack_ok)
		stack_bad++;
	return c->result;
}
/* a call started and waited for (20 seconds unless said) */
static short run(struct call *c, short cs) { start(c, cs); return wait_call(c, 20 * 60); }
static void clear(struct call *c) { memset(c, 0, sizeof *c); }

/* ---- notification routines --------------------------------------------------------------------- */
struct sstate {                         /* a stream's events: the routine's user data */
	volatile int closing, terminate, reason, data, other, udp_data;
	volatile int a5_bad, regs_bad;
	int status_in_asr;              /* make a synchronous TCPStatus inside the routine */
	volatile int status_r, status_unread, status_state;
};
static volatile int in_asr, asr_reentered;
static TCP6iopb asr_pb;
void asr_c(u32 stream, u32 event, struct sstate *st, u32 reason, void *a5, u32 d0, void *a2, int tcp);
void asr_c(u32 stream, u32 event, struct sstate *st, u32 reason, void *a5, u32 d0, void *a2, int tcp)
{
	if (in_asr)
		asr_reentered = 1;
	in_asr = 1;
	if (a5 != app_a5)
		st->a5_bad = 1;
	if ((d0 & 0xFFFF) != event || a2 != (void *)st)
		st->regs_bad = 1;
	if (!tcp) {
		if (event == UDPDataArrival)
			st->udp_data++;
		else
			st->other++;
	} else if (event == TCPClosing) {
		st->closing++;
	} else if (event == TCPTerminate) {
		st->terminate++;
		st->reason = (int)reason;
	} else if (event == TCPDataArrival) {
		st->data++;
		if (st->status_in_asr == 1) {
			/* ask at once how much has come, synchronously: an immediate call */
			memset(&asr_pb, 0, sizeof asr_pb);
			asr_pb.ioCRefNum = ipp6;
			asr_pb.csCode = TCPStatus;
			asr_pb.tcpStream = stream;
			st->status_r = pb_sync(&asr_pb);
			st->status_unread = asr_pb.csParam.status.amtUnreadData;
			st->status_state = asr_pb.csParam.status.connectionState;
			st->status_in_asr = 2;
		}
	} else {
		st->other++;
	}
	in_asr = 0;
}
/* pascal TCP6NotifyProc(stream, eventCode, userDataPtr, terminReason, icmpMsg): 16 bytes of arguments */
void pas_asr(void);
__asm__(".globl pas_asr\npas_asr:\n"
        "\tmovem.l %d0-%d2/%a0-%a2,-(%sp)\n"          /* 24 bytes: the return address at 24(sp) */
        "\tpea 1.w\n"                                 /* tcp */
        "\tmove.l %a2,-(%sp)\n"
        "\tmove.l %d0,-(%sp)\n"
        "\tmove.l %a5,-(%sp)\n"                       /* 16 pushed: the arguments from 44(sp) */
        "\tmoveq #0,%d1\n\tmove.w 48(%sp),%d1\n\tmove.l %d1,-(%sp)\n"     /* reason */
        "\tmove.l 54(%sp),-(%sp)\n"                   /* udata */
        "\tmoveq #0,%d1\n\tmove.w 62(%sp),%d1\n\tmove.l %d1,-(%sp)\n"     /* event */
        "\tmove.l 68(%sp),-(%sp)\n"                   /* stream */
        "\tjsr asr_c\n"
        "\tlea 32(%sp),%sp\n"
        "\tmovem.l (%sp)+,%d0-%d2/%a0-%a2\n"
        "\tmove.l (%sp)+,%a0\n"
        "\tlea 16(%sp),%sp\n"
        "\tjmp (%a0)\n");
/* pascal UDP6NotifyProc(stream, eventCode, userDataPtr, icmpMsg): 14 bytes */
void pas_uasr(void);
__asm__(".globl pas_uasr\npas_uasr:\n"
        "\tmovem.l %d0-%d2/%a0-%a2,-(%sp)\n"
        "\tclr.l -(%sp)\n"                            /* tcp = 0 */
        "\tmove.l %a2,-(%sp)\n"
        "\tmove.l %d0,-(%sp)\n"
        "\tmove.l %a5,-(%sp)\n"                       /* the arguments from 44(sp): icmp 44, udata 48, event 52, stream 54 */
        "\tclr.l -(%sp)\n"                            /* reason */
        "\tmove.l 52(%sp),-(%sp)\n"                   /* udata (48 + 4) */
        "\tmoveq #0,%d1\n\tmove.w 60(%sp),%d1\n\tmove.l %d1,-(%sp)\n"     /* event (52 + 8) */
        "\tmove.l 66(%sp),-(%sp)\n"                   /* stream (54 + 12) */
        "\tjsr asr_c\n"
        "\tlea 32(%sp),%sp\n"
        "\tmovem.l (%sp)+,%d0-%d2/%a0-%a2\n"
        "\tmove.l (%sp)+,%a0\n"
        "\tlea 14(%sp),%sp\n"
        "\tjmp (%a0)\n");

/* ---- helpers ---------------------------------------------------------------------------------- */
static void a6(IP6Address *a, const unsigned char *b, unsigned short port)
{
	memset(a, 0, sizeof *a);
	memcpy(a->addr, b, 16);
	a->port = port;
}
static void a4(IP6Address *a, u32 v4, unsigned short port)
{
	memset(a, 0, sizeof *a);
	IP6_SET_V4(a, v4);
	a->port = port;
}
static const char *text(const IP6Address *a)
{
	static char t[4][64];
	static int k;
	IP6iopb pb;
	char *o = t[k++ & 3];
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = ipp6;
	pb.csCode = ip6AddrToStr;
	pb.csParam.resolve.addr = *a;
	pb.csParam.resolve.hostName = o;
	pb.csParam.resolve.hostNameLen = 64;
	if (pb_sync(&pb) != 0)
		strcpy(o, "?");
	return o;
}

/* ---- 1. detection --------------------------------------------------------------------------- */
static void test_detect(void)
{
	long v = 0;
	unsigned char pb[80], p[16];
	struct call c;
	short r, ipp;
	char t[160];
	r = (short)mac_gestalt(gestaltIPP6, &v);
	check(r == 0 && v == IPP6_VERSION, "Gestalt 'ipp6' answers the interface version 2", v);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, ".IPP6");
	r = mac_pb(pb, TRAP_OPEN);
	ipp6 = *(short *)(pb + 24);
	check(r == 0 && ipp6 < 0, "OpenDriver(\".IPP6\") gives a reference number", ipp6);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, ".IPP");
	r = mac_pb(pb, TRAP_OPEN);
	ipp = *(short *)(pb + 24);
	check(r == 0 && ipp != ipp6, ".IPP is there too, another driver", ipp);
	clear(&c);
	r = run(&c, ip6GetInfo);
	rt_snprintf(t, sizeof t, "ip6GetInfo: version 2, 32 streams, 8 lookups (%s)",
	            c.pb.i.csParam.info.implementation ? c.pb.i.csParam.info.implementation : "?");
	check(r == 0 && c.pb.i.csParam.info.version == 2 && c.pb.i.csParam.info.maxTCPStreams == 32 &&
	      c.pb.i.csParam.info.maxLookups == 8, t, r);
	/* the addresses come with DHCP and the router advertisement: wait for both */
	{
		u32 t0 = TickCount();
		IP6iopb q;
		do {
			memset(&q, 0, sizeof q);
			q.ioCRefNum = ipp6;
			q.csCode = ip6GetInfo;
			pb_sync(&q);                    /* answered in the Control call */
			if ((q.csParam.info.features & (kIP6HasIPv4 | kIP6HasIPv6)) == (kIP6HasIPv4 | kIP6HasIPv6))
				break;
			pump(30);
		} while (TickCount() - t0 < 60 * 60);
		rt_snprintf(t, sizeof t, "features: IPv4, IPv6, link-local, a name server (%lx)", q.csParam.info.features);
		check((q.csParam.info.features & (kIP6HasIPv4 | kIP6HasIPv6 | kIP6HasLinkLocal | kIP6HasNameServer)) ==
		      (kIP6HasIPv4 | kIP6HasIPv6 | kIP6HasLinkLocal | kIP6HasNameServer), t, (long)q.csParam.info.features);
	}
	/* a Close, as programs make on quitting, is refused and the driver stays */
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = ipp6;
	r = pb_close(pb);
	clear(&c);
	check(r == -24 && run(&c, ip6GetInfo) == 0, "Close refused (closErr), .IPP6 still answers", r);
}

/* ---- 2. addresses and text -------------------------------------------------------------------- */
static short en0;                       /* the interface's index (the scope of its link-local addresses) */
static IP6Address my6;                  /* our fd68:: address */
static void test_addrs(void)
{
	static IP6AddrInfo info[12];
	struct call c;
	short r, i, v4 = 0, ula = 0, ll = 0;
	char t[160];
	clear(&c);
	c.pb.i.csParam.addrs.buffer = info;
	c.pb.i.csParam.addrs.bufferLen = sizeof info;
	r = run(&c, ip6GetAddrs);
	for (i = 0; i < c.pb.i.csParam.addrs.count; i++) {
		IP6AddrInfo *e = &info[i];
		rt_snprintf(t, sizeof t, "  %s %s/%u flags %02x scope %u", e->ifName, text(&e->addr), e->prefixLen, e->flags,
		            e->addr.scope);
		out(t);
		if ((e->flags & kIP6AddrIPv4) && IP6_GET_V4(&e->addr) == MAC4 && (e->flags & kIP6AddrPrimary) && e->prefixLen == 24)
			v4 = 1;
		if (!(e->flags & kIP6AddrIPv4) && e->addr.addr[0] == 0xfd && e->addr.addr[1] == 0x68 && e->prefixLen == 64 &&
		    !(e->flags & kIP6AddrTentative)) {
			ula = 1;
			my6 = e->addr;
		}
		if ((e->flags & kIP6AddrLinkLocal) && IP6_IS_LINKLOCAL(&e->addr) && e->addr.scope == e->ifIndex) {
			ll = 1;
			en0 = (short)e->ifIndex;
		}
	}
	check(r == 0 && v4, "ip6GetAddrs: 192.168.68.2/24 (IPv4-mapped, the primary interface)", r);
	check(ula, "... an address of fd68::/64 from the router advertisement", ula);
	check(ll, "... a link-local address whose scope is its interface", en0);
	clear(&c);
	c.pb.i.csParam.addrs.buffer = info;
	c.pb.i.csParam.addrs.bufferLen = sizeof info[0];
	r = run(&c, ip6GetAddrs);
	check(r == 0 && c.pb.i.csParam.addrs.count == 1 && c.pb.i.csParam.addrs.total >= 3,
	      "... a buffer of one entry: count 1, total all of them", c.pb.i.csParam.addrs.total);
}

static short str2(const char *s, IP6Address *a)
{
	IP6iopb pb;
	short r;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = ipp6;
	pb.csCode = ip6StrToAddr;
	pb.csParam.resolve.hostName = (char *)s;
	pb.csParam.resolve.port = 8080;
	r = pb_sync(&pb);
	*a = pb.csParam.resolve.addr;
	return r;
}
static void test_text(void)
{
	IP6Address a;
	IP6iopb pb;
	char small[8];
	short r;
	r = str2("fd68::1", &a);
	check(r == 0 && !memcmp(a.addr, PEER6, 16) && a.port == 8080, "ip6StrToAddr fd68::1 (and the port)", r);
	r = str2("192.168.68.1", &a);
	check(r == 0 && IP6_IS_V4(&a) && IP6_GET_V4(&a) == PEER4, "ip6StrToAddr 192.168.68.1: ::ffff:192.168.68.1", r);
	r = str2("[fe80::1%en0]", &a);
	check(r == 0 && IP6_IS_LINKLOCAL(&a) && a.scope == en0 && en0, "ip6StrToAddr [fe80::1%en0]: the scope is en0's", a.scope);
	r = str2("fe80::1%en7", &a);
	check(r == ip6ScopeErr, "ip6StrToAddr fe80::1%en7: ip6ScopeErr", r);
	r = str2("peer.testbed", &a);
	check(r == nameSyntaxErr, "ip6StrToAddr of a name: nameSyntaxErr (names are ip6Resolve's)", r);
	a6(&a, PEER6, 0);
	check(!strcmp(text(&a), "fd68::1"), "ip6AddrToStr fd68::1", 0);
	a4(&a, PEER4, 0);
	check(!strcmp(text(&a), "192.168.68.1"), "ip6AddrToStr ::ffff:192.168.68.1: 192.168.68.1", 0);
	if (str2("fe80::1%en0", &a) == 0)
		check(!strcmp(text(&a), "fe80::1%en0"), "ip6AddrToStr fe80::1%en0", a.scope);
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = ipp6;
	pb.csCode = ip6AddrToStr;
	a6(&pb.csParam.resolve.addr, PEER6, 0);
	pb.csParam.resolve.hostName = small;
	pb.csParam.resolve.hostNameLen = 5;
	r = pb_sync(&pb);
	check(r == ip6TruncatedErr && !strcmp(small, "fd68"), "ip6AddrToStr into 5 bytes: ip6TruncatedErr, \"fd68\"", r);
}

/* ---- 3. names -------------------------------------------------------------------------------- */
static IP6Address res[8];
static char canon[64];
static short resolve(const char *name, unsigned short flags, unsigned short timeout, struct call *c)
{
	clear(c);
	memset(res, 0, sizeof res);
	canon[0] = 0;
	c->pb.i.csParam.resolve.hostName = (char *)name;
	c->pb.i.csParam.resolve.flags = flags;
	c->pb.i.csParam.resolve.results = res;
	c->pb.i.csParam.resolve.maxResults = 8;
	c->pb.i.csParam.resolve.port = 80;
	c->pb.i.csParam.resolve.timeout = timeout;
	c->pb.i.csParam.resolve.canonName = canon;
	c->pb.i.csParam.resolve.canonNameLen = sizeof canon;
	start(c, ip6Resolve);
	return wait_call(c, 30 * 60);
}
static int is6(const IP6Address *a, const unsigned char *b) { return !IP6_IS_V4(a) && !memcmp(a->addr, b, 16); }
static int is4(const IP6Address *a, u32 v4) { return IP6_IS_V4(a) && IP6_GET_V4(a) == v4; }
static void show(const char *name, struct call *c, short r)
{
	char t[200];
	int n = c->pb.i.csParam.resolve.count, i, o;
	o = rt_snprintf(t, sizeof t, "  %s: %d, count %d:", name, r, n);
	for (i = 0; i < n && i < 4 && o < (int)sizeof t - 50; i++)
		o += rt_snprintf(t + o, sizeof t - o, " %s", text(&res[i]));
	out(t);
}
static void test_resolve(void)
{
	struct call c, x;
	short r;
	u32 t0;
	int n;
	r = resolve("peer.testbed", kIP6OrderIPv6First, 0, &c);
	show("peer.testbed, IPv6 first", &c, r);
	n = c.pb.i.csParam.resolve.count;
	check(r == 0 && n == 2 && is6(&res[0], PEER6) && is4(&res[1], PEER4) && res[0].port == 80 && res[1].port == 80,
	      "ip6Resolve peer.testbed (AAAA and A): fd68::1, then 192.168.68.1, port 80 in both", r);
	check(!strcmp(canon, "peer.testbed"), "... the canonical name", 0);
	r = resolve("peer.testbed", kIP6OrderIPv4First, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 2 && is4(&res[0], PEER4) && is6(&res[1], PEER6),
	      "... IPv4 first: 192.168.68.1, then fd68::1", r);
	r = resolve("peer.testbed", kIP6OrderRFC6724, 0, &c);
	show("peer.testbed, RFC 6724", &c, r);
	/* RFC 6724's policy table ranks unique local IPv6 (fc00::/7, precedence 3) below IPv4 (35) */
	check(r == 0 && c.pb.i.csParam.resolve.count == 2 && is4(&res[0], PEER4) && is6(&res[1], PEER6),
	      "... RFC 6724: IPv4 before a unique local IPv6 address (its policy table)", r);
	r = resolve("peer.testbed", kIP6OnlyIPv6, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is6(&res[0], PEER6), "... IPv6 only: fd68::1", r);
	r = resolve("peer.testbed", kIP6OnlyIPv4, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is4(&res[0], PEER4), "... IPv4 only: 192.168.68.1", r);
	r = resolve("v6only.testbed", 0, 0, &c);
	show("v6only.testbed", &c, r);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is6(&res[0], V6ONLY), "ip6Resolve v6only.testbed: fd68::80 only", r);
	r = resolve("www.testbed", 0, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is4(&res[0], PEER4), "ip6Resolve www.testbed: 192.168.68.1 only", r);
	r = resolve("fd68::80", 0, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is6(&res[0], V6ONLY) && res[0].port == 80,
	      "ip6Resolve of a literal: at once, one result", r);
	r = resolve("h6.lan", kIP6OrderIPv6First, 0, &c);
	show("h6.lan", &c, r);
	check(r == 0 && c.pb.i.csParam.resolve.count == 2 && is6(&res[0], V6ONLY) && is4(&res[1], PEER4),
	      "ip6Resolve h6.lan: riptide.conf's two host lines, IPv6 and IPv4", r);
	r = resolve("nosuch.example", 0, 0, &c);
	check(r == authNameErr, "ip6Resolve of a name that does not exist: authNameErr", r);
	t0 = TickCount();
	r = resolve("silent.testbed", 0, 3, &c);
	check(r == noAnsErr && TickCount() - t0 >= 150 && TickCount() - t0 < 8 * 60,
	      "ip6Resolve with a server that never answers, timeout 3 s: noAnsErr in about 3 s", (long)(TickCount() - t0));
	/* withdrawn: its completion comes first, with ip6CancelledErr */
	clear(&c);
	c.pb.i.csParam.resolve.hostName = "silent.testbed";
	c.pb.i.csParam.resolve.results = res;
	c.pb.i.csParam.resolve.maxResults = 8;
	start(&c, ip6Resolve);
	pump(10);
	clear(&x);
	x.pb.i.csParam.cancel.target = &c.pb.i;
	r = run(&x, ip6ResolveCancel);
	check(r == 0 && c.done && c.result == ip6CancelledErr, "ip6ResolveCancel: the lookup ends in ip6CancelledErr first", c.result);
	clear(&x);
	x.pb.i.csParam.cancel.target = &c.pb.i;
	check(run(&x, ip6ResolveCancel) == ip6NotPendingErr, "... a second time: ip6NotPendingErr", x.result);
	/* errors of the call itself */
	clear(&c);
	c.pb.i.csParam.resolve.hostName = "peer.testbed";
	c.pb.i.csParam.resolve.results = res;
	check(run(&c, ip6Resolve) == invalidLength, "ip6Resolve with maxResults 0: invalidLength", c.result);
	/* reverse */
	clear(&c);
	a4(&c.pb.i.csParam.resolve.addr, PEER4, 0);
	c.pb.i.csParam.resolve.hostName = canon;
	c.pb.i.csParam.resolve.hostNameLen = sizeof canon;
	r = run(&c, ip6ResolveAddr);
	check(r == 0 && !strcmp(canon, "peer.testbed"), "ip6ResolveAddr ::ffff:192.168.68.1: peer.testbed (in-addr.arpa)", r);
	clear(&c);
	a4(&c.pb.i.csParam.resolve.addr, PEER4, 0);
	c.pb.i.csParam.resolve.hostName = canon;
	c.pb.i.csParam.resolve.hostNameLen = 5;
	r = run(&c, ip6ResolveAddr);
	check(r == ip6TruncatedErr && !strcmp(canon, "peer"), "... into 5 bytes: ip6TruncatedErr, \"peer\"", r);
	clear(&c);
	a6(&c.pb.i.csParam.resolve.addr, PEER6, 0);
	c.pb.i.csParam.resolve.hostName = canon;
	c.pb.i.csParam.resolve.hostNameLen = sizeof canon;
	r = run(&c, ip6ResolveAddr);
	check(r == authNameErr, "ip6ResolveAddr fd68::1: authNameErr (the peer knows no ip6.arpa names)", r);
}

/* ---- 4. TCP ----------------------------------------------------------------------------------- */
struct conn {
	struct call c;                  /* the call in progress */
	u32 stream;
	char rbuf[16384];
	struct sstate st;
	unsigned char rds[6 * 6 + 2];
	char body[1024];
	long got;
	int status_ok;
};
static struct conn conns[4];
static char chain_buf[2][4096];
static volatile int chain_done[2], chain_r[2], chain_bad[2];
static volatile long chain_got[2];
static volatile short immed_r = 1, immed_count;

static short tcp_create(struct conn *k, int asr)
{
	short r;
	clear(&k->c);
	k->c.pb.t.csParam.create.rcvBuff = k->rbuf;
	k->c.pb.t.csParam.create.rcvBuffLen = sizeof k->rbuf;
	if (asr) {
		k->c.pb.t.csParam.create.notifyProc = (TCP6NotifyUPP)pas_asr;
		k->c.pb.t.csParam.create.userDataPtr = (char *)&k->st;
	}
	r = run(&k->c, TCPCreate);
	k->stream = r == 0 ? k->c.pb.t.tcpStream : 0;
	return r;
}
static void tcp_open(struct conn *k, const IP6Address *to, unsigned char ulp)
{
	int keep = k->st.status_in_asr;
	clear(&k->c);
	k->c.pb.t.tcpStream = k->stream;
	k->c.pb.t.csParam.open.ulpTimeoutValue = ulp;
	k->c.pb.t.csParam.open.validityFlags = timeoutValue;
	k->c.pb.t.csParam.open.remoteAddr = *to;
	memset(&k->st, 0, sizeof k->st);
	k->st.status_in_asr = keep;
	start(&k->c, TCPActiveOpen);
}
static short tcp_simple(struct conn *k, short cs)
{
	clear(&k->c);
	k->c.pb.t.tcpStream = k->stream;
	return run(&k->c, cs);
}
static short send_get(struct conn *k, const char *path, const char *host)
{
	static char req[4][128];
	static unsigned char wds[4][2 * 6 + 2];
	int i = (int)(k - conns);
	const char *p = req[i];
	rt_snprintf(req[i], sizeof req[i], "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Riptide IPP6 test\r\n\r\n", path, host);
	memset(wds[i], 0, sizeof wds[i]);
	*(unsigned short *)wds[i] = (unsigned short)strlen(req[i]);
	memcpy(wds[i] + 2, &p, 4);
	clear(&k->c);
	k->c.pb.t.tcpStream = k->stream;
	k->c.pb.t.csParam.send.pushFlag = 1;
	k->c.pb.t.csParam.send.wdsPtr = (char *)wds[i];
	return run(&k->c, TCPSend);
}
/* TCPNoCopyRcv with a six-entry RDS and TCPRcvBfrReturn, to connectionClosing */
static short read_nocopy(struct conn *k)
{
	short r;
	k->got = 0;
	for (;;) {
		clear(&k->c);
		k->c.pb.t.tcpStream = k->stream;
		k->c.pb.t.csParam.receive.commandTimeoutValue = 15;
		k->c.pb.t.csParam.receive.rdsPtr = (char *)k->rds;
		k->c.pb.t.csParam.receive.rdsLength = 6;
		r = run(&k->c, TCPNoCopyRcv);
		if (r != 0)
			return r;
		{
			unsigned char *e = k->rds;
			for (; *(unsigned short *)e; e += 6) {
				unsigned short n = *(unsigned short *)e;
				char *d;
				memcpy(&d, e + 2, 4);
				if (k->got < (long)sizeof k->body - 1) {
					long m = n < (long)sizeof k->body - 1 - k->got ? n : (long)sizeof k->body - 1 - k->got;
					memcpy(k->body + k->got, d, m);
					k->body[k->got + m] = 0;
				}
				k->got += n;
			}
		}
		clear(&k->c);
		k->c.pb.t.tcpStream = k->stream;
		k->c.pb.t.csParam.receive.rdsPtr = (char *)k->rds;
		if ((r = run(&k->c, TCPRcvBfrReturn)) != 0)
			return r;
	}
}
/* TCPRcv chained from its own completion routine (interrupt time, A5 set by comp6) */
static int chain_then(struct call *c)
{
	int i = c->count;
	if (!c->order_ok || !c->stack_ok)
		chain_bad[i]++;
	if (c->result == 0) {
		chain_got[i] += c->pb.t.csParam.receive.rcvBuffLen;
		if (c->n++ == 3) {
			/* an immediate call made synchronously from a completion routine: answered on the spot */
			static IP6AddrInfo e[4];
			static IP6iopb q;
			memset(&q, 0, sizeof q);
			q.ioCRefNum = ipp6;
			q.csCode = ip6GetAddrs;
			q.csParam.addrs.buffer = e;
			q.csParam.addrs.bufferLen = sizeof e;
			immed_r = pb_sync(&q);
			immed_count = (short)q.csParam.addrs.count;
		}
		c->pb.t.csParam.receive.rcvBuff = chain_buf[i];
		c->pb.t.csParam.receive.rcvBuffLen = sizeof chain_buf[i];
		c->pb.t.csParam.receive.commandTimeoutValue = 0;
		pb_async(&c->pb);                       /* the next receive, from the completion routine */
		return 1;
	}
	chain_r[i] = c->result;
	chain_done[i] = 1;
	return 0;
}
static void chain_start(struct conn *k, int i)
{
	chain_done[i] = 0;
	chain_got[i] = 0;
	chain_bad[i] = 0;
	clear(&k->c);
	k->c.then = chain_then;
	k->c.count = i;
	k->c.pb.t.tcpStream = k->stream;
	k->c.pb.t.csParam.receive.rcvBuff = chain_buf[i];
	k->c.pb.t.csParam.receive.rcvBuffLen = sizeof chain_buf[i];
	start(&k->c, TCPRcv);
}
static int wait_flag(volatile int *f, u32 ticks)
{
	u32 t0 = TickCount();
	while (!*f && TickCount() - t0 < ticks)
		pump(1);
	return *f;
}
static void close_and_terminate(struct conn *k, const char *what)
{
	char t[160];
	short r = tcp_simple(k, TCPClose);
	wait_flag(&k->st.terminate, 10 * 60);
	rt_snprintf(t, sizeof t, "%s: TCPClose, then TCPTerminate with TCPULPClose", what);
	check(r == 0 && k->st.terminate == 1 && k->st.reason == TCPULPClose, t, k->st.reason);
}

static void test_tcp(void)
{
	struct conn *A = &conns[0], *B = &conns[1], *C = &conns[2];
	IP6Address to;
	short r;
	char t[200];
	int i;
	for (i = 0; i < 3; i++)
		check(tcp_create(&conns[i], 1) == 0, "TCPCreate", conns[i].stream);
	/* three opens at once: two over IPv6, one over IPv4 */
	a6(&to, PEER6, 80);
	tcp_open(A, &to, 20);
	a6(&to, V6ONLY, 80);
	tcp_open(B, &to, 20);
	a4(&to, PEER4, 80);
	tcp_open(C, &to, 20);
	r = wait_call(&A->c, 25 * 60);
	rt_snprintf(t, sizeof t, "TCPActiveOpen [fd68::1]:80, ends %s / %s", text(&A->c.pb.t.csParam.open.remoteAddr),
	            text(&A->c.pb.t.csParam.open.localAddr));
	check(r == 0 && is6(&A->c.pb.t.csParam.open.remoteAddr, PEER6) && A->c.pb.t.csParam.open.remoteAddr.port == 80 &&
	      A->c.pb.t.csParam.open.localAddr.addr[0] == 0xfd && A->c.pb.t.csParam.open.localAddr.port, t, r);
	r = wait_call(&B->c, 25 * 60);
	check(r == 0 && is6(&B->c.pb.t.csParam.open.remoteAddr, V6ONLY), "TCPActiveOpen [fd68::80]:80 (an IPv6-only host)", r);
	r = wait_call(&C->c, 25 * 60);
	rt_snprintf(t, sizeof t, "TCPActiveOpen ::ffff:192.168.68.1:80 goes over IPv4, local %s",
	            text(&C->c.pb.t.csParam.open.localAddr));
	check(r == 0 && is4(&C->c.pb.t.csParam.open.remoteAddr, PEER4) && is4(&C->c.pb.t.csParam.open.localAddr, MAC4), t, r);
	/* TCPStatus: the IPv6 ends and the state */
	r = tcp_simple(A, TCPStatus);
	rt_snprintf(t, sizeof t, "TCPStatus: [%s]:%u, local [%s]:%u, state %u", text(&A->c.pb.t.csParam.status.remoteAddr),
	            A->c.pb.t.csParam.status.remoteAddr.port, text(&A->c.pb.t.csParam.status.localAddr),
	            A->c.pb.t.csParam.status.localAddr.port, A->c.pb.t.csParam.status.connectionState);
	check(r == 0 && is6(&A->c.pb.t.csParam.status.remoteAddr, PEER6) && A->c.pb.t.csParam.status.remoteAddr.port == 80 &&
	      !memcmp(A->c.pb.t.csParam.status.localAddr.addr, my6.addr, 16) &&
	      A->c.pb.t.csParam.status.connectionState == kIP6StateEstablished, t, r);
	/* the requests, then three ways of reading */
	B->st.status_in_asr = 1;
	check(send_get(A, "/big", "peer.testbed") == 0, "TCPSend GET /big over IPv6", A->c.result);
	check(send_get(B, "/", "v6only.testbed") == 0, "TCPSend GET / to the IPv6-only host", B->c.result);
	check(send_get(C, "/", "www.testbed") == 0, "TCPSend GET / over IPv4", C->c.result);
	chain_start(A, 0);
	wait_flag(&B->st.data, 5 * 60);                 /* no receive waits yet: TCPDataArrival */
	r = read_nocopy(B);
	rt_snprintf(t, sizeof t, "TCPNoCopyRcv and TCPRcvBfrReturn over IPv6: %ld bytes, the page", B->got);
	check(r == connectionClosing && B->got > 100 && find_page(B->body), t, r);
	check(B->st.data >= 1 && B->st.status_in_asr == 2 && B->st.status_r == 0,
	      "TCPDataArrival, and a synchronous TCPStatus inside the notification routine", B->st.status_r);
	wait_flag(&B->st.closing, 5 * 60);
	check(B->st.closing == 1, "TCPClosing: the server closed (an ASR on the remote close)", B->st.closing);
	close_and_terminate(B, "IPv6");
	r = read_nocopy(C);
	check(r == connectionClosing && find_page(C->body), "TCPNoCopyRcv over IPv4 (an IPv4-mapped stream)", C->got);
	wait_flag(&C->st.closing, 5 * 60);
	close_and_terminate(C, "IPv4");
	wait_flag(&chain_done[0], 60 * 60);
	rt_snprintf(t, sizeof t, "TCPRcv chained from its completion routine: %ld bytes of /big (256 KB and the headers)",
	            chain_got[0]);
	check(chain_done[0] && chain_r[0] == connectionClosing && chain_got[0] > 262144 && chain_got[0] < 262144 + 400 &&
	      !chain_bad[0], t, chain_r[0]);
	check(immed_r == 0 && immed_count >= 3, "a synchronous ip6GetAddrs from inside a completion routine: answered", immed_r);
	close_and_terminate(A, "/big");
	/* the stream opened again: a reset after the headers */
	a6(&to, PEER6, 80);
	tcp_open(B, &to, 20);
	r = wait_call(&B->c, 25 * 60);
	check(r == 0, "the stream opened again (TCPActiveOpen after TCPTerminate)", r);
	send_get(B, "/reset", "peer.testbed");
	chain_start(B, 1);
	wait_flag(&chain_done[1], 15 * 60);
	wait_flag(&B->st.terminate, 5 * 60);
	/* the reset may come before the chained receive is made (connectionDoesntExist), as in the
	   Browser test */
	check(B->st.terminate == 1 && B->st.reason == TCPRemoteAbort && chain_done[1] &&
	      (chain_r[1] == connectionTerminated || chain_r[1] == connectionClosing ||
	       chain_r[1] == connectionDoesntExist),
	      "/reset over IPv6: TCPTerminate with TCPRemoteAbort, the receive ends", B->st.reason);
	/* refused */
	a6(&to, PEER6, 81);
	tcp_open(B, &to, 10);
	r = wait_call(&B->c, 15 * 60);
	check(r == openFailed, "TCPActiveOpen [fd68::1]:81 (a reset): openFailed", r);
	check(tcp_simple(B, TCPStatus) == connectionDoesntExist && B->c.pb.t.csParam.status.connectionState == kIP6StateClosed,
	      "TCPStatus with no connection: connectionDoesntExist, state closed", B->c.result);
}

static int find_page(const char *b)
{
	const char *h;
	for (h = b; *h; h++)
		if (!strncmp(h, "It works", 8))
			return !strncmp(b, "HTTP/1.0 200", 12);
	return 0;
}

/* the example's way: the addresses in order until one connects */
static void test_fallback(void)
{
	struct conn *D = &conns[3];
	struct call c;
	IP6Address list[2];
	short r, i;
	char t[160];
	u32 t0;
	check(tcp_create(D, 1) == 0, "TCPCreate", D->stream);
	r = resolve("www.testbed", 0, 0, &c);
	check(r == 0 && c.pb.i.csParam.resolve.count == 1 && is4(&res[0], PEER4), "an IPv4-only host: one IPv4-mapped address", r);
	tcp_open(D, &res[0], 15);
	r = wait_call(&D->c, 20 * 60);
	if (r == 0) {
		send_get(D, "/", "www.testbed");
		r = read_nocopy(D);
		check(r == connectionClosing && find_page(D->body), "... connected over IPv4 and fetched the page", D->got);
		close_and_terminate(D, "www.testbed");
	} else {
		check(0, "... TCPActiveOpen", r);
	}
	/* an IPv6 address that nothing answers, then the IPv4 one */
	a6(&list[0], DEAD6, 80);
	a4(&list[1], PEER4, 80);
	t0 = TickCount();
	for (i = 0; i < 2; i++) {
		tcp_open(D, &list[i], 4);
		r = wait_call(&D->c, 15 * 60);
		rt_snprintf(t, sizeof t, "  %s: %d after %lu ticks", text(&list[i]), r, (unsigned long)(TickCount() - t0));
		out(t);
		if (r == 0)
			break;
	}
	check(i == 1 && r == 0, "fallback: [fd68::99] does not answer (openFailed or ipDestDeadErr), then 192.168.68.1 connects", r);
	if (r == 0) {
		send_get(D, "/", "peer.testbed");
		r = read_nocopy(D);
		check(r == connectionClosing && find_page(D->body), "... and the page comes over IPv4", D->got);
		tcp_simple(D, TCPAbort);
	}
}

/* ---- 4b. ip6ConnectName (version 2): a connection by name, with the core's Happy Eyeballs ----- */
static void connect_name(struct conn *k, const char *name, unsigned short port, unsigned short flags,
                         unsigned char ulp)
{
	int keep = k->st.status_in_asr;
	clear(&k->c);
	k->c.pb.t.tcpStream = k->stream;
	k->c.pb.t.csParam.connectName.ulpTimeoutValue = ulp;
	k->c.pb.t.csParam.connectName.validityFlags = timeoutValue;
	k->c.pb.t.csParam.connectName.remoteAddr.port = port;
	k->c.pb.t.csParam.connectName.hostName = (char *)name;
	k->c.pb.t.csParam.connectName.flags = flags;
	memset(&k->st, 0, sizeof k->st);
	k->st.status_in_asr = keep;
	start(&k->c, ip6ConnectName);
}
static short connect_wait(struct conn *k, const char *what)
{
	char t[200];
	TCP6ConnectNamePB *p = &k->c.pb.t.csParam.connectName;
	short r = wait_call(&k->c, 30 * 60);
	rt_snprintf(t, sizeof t, "  %s: %d, %s port %u, %u addresses, %u attempts, %lu ms", what, r, text(&p->remoteAddr),
	            (unsigned)p->remoteAddr.port, (unsigned)p->addrCount, (unsigned)p->attempts, (unsigned long)p->elapsed);
	out(t);
	return r;
}
static void test_connect_name(void)
{
	struct conn *D = &conns[3];
	TCP6ConnectNamePB *p = &D->c.pb.t.csParam.connectName;
	struct call c;
	short r;
	/* D was made by test_fallback, and is not connected */
	connect_name(D, "peer.testbed", 80, kIP6OrderIPv6First, 20);
	r = connect_wait(D, "peer.testbed:80, IPv6 first");
	check(r == 0 && is6(&p->remoteAddr, PEER6) && p->remoteAddr.port == 80 && p->addrCount == 2 && p->attempts == 1,
	      "ip6ConnectName peer.testbed (AAAA and A), IPv6 first: [fd68::1]:80 at the first attempt", r);
	if (r == 0) {
		check(!IP6_IS_V4(&p->localAddr) && p->localAddr.port, "... the local end is IPv6, with a port", p->localAddr.port);
		clear(&c);
		c.pb.t.tcpStream = D->stream;
		c.pb.t.csParam.connectName.remoteAddr.port = 80;
		c.pb.t.csParam.connectName.hostName = "peer.testbed";
		r = run(&c, ip6ConnectName);
		check(r == connectionExists, "... ip6ConnectName on a stream with a connection: connectionExists", r);
		send_get(D, "/", "peer.testbed");
		r = read_nocopy(D);
		check(r == connectionClosing && find_page(D->body), "... and the page comes over IPv6", D->got);
		close_and_terminate(D, "peer.testbed by name");
	}
	connect_name(D, "peer.testbed", 80, 0, 20);
	r = connect_wait(D, "peer.testbed:80, the default order");
	check(r == 0 && is4(&p->remoteAddr, PEER4) && p->attempts == 1,
	      "... in the default order (RFC 6724: IPv4 before a unique local address): 192.168.68.1", r);
	if (r == 0)
		tcp_simple(D, TCPAbort);
	connect_name(D, "v6only.testbed", 80, 0, 20);
	r = connect_wait(D, "v6only.testbed:80");
	check(r == 0 && is6(&p->remoteAddr, V6ONLY) && p->addrCount == 1, "ip6ConnectName v6only.testbed: [fd68::80]:80", r);
	if (r == 0) {
		send_get(D, "/", "v6only.testbed");
		r = read_nocopy(D);
		check(r == connectionClosing && find_page(D->body), "... and its page", D->got);
		tcp_simple(D, TCPAbort);
	}
	/* Happy Eyeballs: nine IPv6 addresses that nothing answers and one IPv4 address; IPv6 first, the
	   IPv4 attempt starts the attempt delay (250 ms) after the first, not when IPv6 has given up */
	connect_name(D, "many.testbed", 80, kIP6OrderIPv6First, 20);
	r = connect_wait(D, "many.testbed:80, IPv6 first");
	check(r == 0 && is4(&p->remoteAddr, PEER4) && p->addrCount == 10 && p->attempts == 2 && p->elapsed >= 200 &&
	      p->elapsed < 2500, "ip6ConnectName many.testbed: IPv4 connects at the second attempt, about 250 ms on", r);
	if (r == 0)
		tcp_simple(D, TCPAbort);
	/* the resolution delay: slow6.testbed's AAAA answer comes a second after the A one (tests/mac/dns_peer.py);
	   the connection does not wait for it (RFC 8305 3), ip6Resolve does (it gives both families) */
	connect_name(D, "slow6.testbed", 80, kIP6OrderIPv6First, 20);
	r = connect_wait(D, "slow6.testbed:80, IPv6 first");
	check(r == 0 && is4(&p->remoteAddr, PEER4) && p->elapsed < 800,
	      "ip6ConnectName slow6.testbed (AAAA a second late): IPv4 at once, the resolution delay (dns_peer.py)", r);
	if (r == 0)
		tcp_simple(D, TCPAbort);
	{
		u32 t0 = TickCount();
		r = resolve("slow6.testbed", kIP6OrderIPv6First, 0, &c);
		show("slow6.testbed", &c, r);
		check(r == 0 && c.pb.i.csParam.resolve.count == 2 && is6(&res[0], PEER6) && TickCount() - t0 >= 50,
		      "... ip6Resolve waits for the late AAAA answer: fd68::1, then 192.168.68.1", (long)(TickCount() - t0));
	}
	/* an answer too large for UDP: truncated, then over TCP (dns.tcp_fallback; tests/mac/dns_peer.py) */
	r = resolve("big.testbed", kIP6OnlyIPv4, 0, &c);
	show("big.testbed", &c, r);
	check(r == 0 && c.pb.i.csParam.resolve.count == 8 && is4(&res[0], PEER4) && is4(&res[7], 0xC6336407UL),
	      "ip6Resolve big.testbed (100 A records, 1,612 bytes): 8 of them, over TCP (dns_peer.py)", r);
	connect_name(D, "fd68::1", 80, 0, 20);
	r = connect_wait(D, "fd68::1:80");
	check(r == 0 && is6(&p->remoteAddr, PEER6), "ip6ConnectName of a literal: [fd68::1]:80", r);
	if (r == 0)
		tcp_simple(D, TCPAbort);
	connect_name(D, "peer.testbed", 81, 0, 20);
	r = connect_wait(D, "peer.testbed:81");
	check(r == openFailed && p->attempts == 2, "ip6ConnectName to a port nobody listens on: openFailed, both addresses tried", r);
	connect_name(D, "nosuch.example", 80, 0, 20);
	r = connect_wait(D, "nosuch.example:80");
	check(r == authNameErr, "ip6ConnectName to a name that does not exist: authNameErr", r);
	connect_name(D, "peer.testbed", 0, 0, 20);
	check(wait_call(&D->c, 5 * 60) == invalidLength, "ip6ConnectName with port 0: invalidLength", D->c.result);
	connect_name(D, NULL, 80, 0, 20);
	check(wait_call(&D->c, 5 * 60) == invalidBufPtr, "ip6ConnectName without a name: invalidBufPtr", D->c.result);
}

/* ---- 5. UDP ----------------------------------------------------------------------------------- */
static void test_udp(void)
{
	static char ubuf[4096], msg6[] = "echo over IPv6", msg4[] = "echo over IPv4";
	static unsigned char wds[2 * 6 + 2];
	struct sstate st;
	struct call c, rd;
	u32 stream;
	short r;
	char t[200];
	char *p;
	memset(&st, 0, sizeof st);
	clear(&c);
	c.pb.u.csParam.create.rcvBuff = ubuf;
	c.pb.u.csParam.create.rcvBuffLen = sizeof ubuf;
	c.pb.u.csParam.create.notifyProc = (UDP6NotifyUPP)pas_uasr;
	c.pb.u.csParam.create.userDataPtr = (char *)&st;
	r = run(&c, UDPCreate);
	stream = c.pb.u.udpStream;
	check(r == 0 && c.pb.u.csParam.create.localPort, "UDPCreate: a port of the stack's choice", c.pb.u.csParam.create.localPort);
	/* a read waiting, then the datagram */
	clear(&rd);
	rd.pb.u.udpStream = stream;
	rd.pb.u.csParam.receive.timeOut = 10;
	start(&rd, UDPRead);
	p = msg6;
	*(unsigned short *)wds = sizeof msg6 - 1;
	memcpy(wds + 2, &p, 4);
	*(unsigned short *)(wds + 6) = 0;
	clear(&c);
	c.pb.u.udpStream = stream;
	a6(&c.pb.u.csParam.send.remoteAddr, PEER6, 7);
	c.pb.u.csParam.send.wdsPtr = (char *)wds;
	check(run(&c, UDPWrite) == 0, "UDPWrite to [fd68::1]:7", c.result);
	r = wait_call(&rd, 12 * 60);
	rt_snprintf(t, sizeof t, "UDPRead: %u bytes from [%s]:%u to [%s]:%u", rd.pb.u.csParam.receive.rcvBuffLen,
	            text(&rd.pb.u.csParam.receive.remoteAddr), rd.pb.u.csParam.receive.remoteAddr.port,
	            text(&rd.pb.u.csParam.receive.destAddr), rd.pb.u.csParam.receive.destAddr.port);
	check(r == 0 && rd.pb.u.csParam.receive.rcvBuffLen == sizeof msg6 - 1 &&
	      !memcmp(rd.pb.u.csParam.receive.rcvBuff, msg6, sizeof msg6 - 1) &&
	      is6(&rd.pb.u.csParam.receive.remoteAddr, PEER6) && rd.pb.u.csParam.receive.remoteAddr.port == 7 &&
	      !memcmp(rd.pb.u.csParam.receive.destAddr.addr, my6.addr, 16), t, r);
	c = rd;
	check(run(&c, UDPBfrReturn) == 0, "UDPBfrReturn", c.result);
	/* no read waiting: UDPDataArrival, then the read; the same stream over IPv4 */
	p = msg4;
	*(unsigned short *)wds = sizeof msg4 - 1;
	memcpy(wds + 2, &p, 4);
	clear(&c);
	c.pb.u.udpStream = stream;
	a4(&c.pb.u.csParam.send.remoteAddr, PEER4, 7);
	c.pb.u.csParam.send.wdsPtr = (char *)wds;
	check(run(&c, UDPWrite) == 0, "UDPWrite to ::ffff:192.168.68.1:7 on the same stream", c.result);
	wait_flag(&st.udp_data, 5 * 60);
	check(st.udp_data == 1, "UDPDataArrival", st.udp_data);
	clear(&rd);
	rd.pb.u.udpStream = stream;
	rd.pb.u.csParam.receive.timeOut = 5;
	r = run(&rd, UDPRead);
	check(r == 0 && !memcmp(rd.pb.u.csParam.receive.rcvBuff, msg4, sizeof msg4 - 1) &&
	      is4(&rd.pb.u.csParam.receive.remoteAddr, PEER4) && is4(&rd.pb.u.csParam.receive.destAddr, MAC4),
	      "UDPRead: the echo from 192.168.68.1 to 192.168.68.2", r);
	c = rd;
	run(&c, UDPBfrReturn);
	clear(&rd);
	rd.pb.u.udpStream = stream;
	rd.pb.u.csParam.receive.timeOut = 2;
	check(run(&rd, UDPRead) == commandTimeout, "UDPRead with nothing coming: commandTimeout", rd.result);
	clear(&c);
	a4(&c.pb.u.csParam.mtu.remoteAddr, PEER4, 0);
	r = run(&c, UDPMaxMTUSize);
	check(r == 0 && c.pb.u.csParam.mtu.mtuSize == 1472, "UDPMaxMTUSize IPv4: 1472", c.pb.u.csParam.mtu.mtuSize);
	clear(&c);
	a6(&c.pb.u.csParam.mtu.remoteAddr, PEER6, 0);
	r = run(&c, UDPMaxMTUSize);
	check(r == 0 && c.pb.u.csParam.mtu.mtuSize == 1452, "UDPMaxMTUSize IPv6: 1452", c.pb.u.csParam.mtu.mtuSize);
	clear(&c);
	c.pb.u.udpStream = stream;
	a6(&c.pb.u.csParam.send.remoteAddr, PEER6, 7);
	memset(c.pb.u.csParam.send.remoteAddr.addr, 0, 16);
	c.pb.u.csParam.send.wdsPtr = (char *)wds;
	check(run(&c, UDPWrite) == ip6BadAddrErr, "UDPWrite to ::: ip6BadAddrErr", c.result);
	clear(&c);
	c.pb.u.udpStream = stream;
	r = run(&c, UDPRelease);
	check(r == 0 && c.pb.u.csParam.create.rcvBuff == ubuf && c.pb.u.csParam.create.rcvBuffLen == sizeof ubuf,
	      "UDPRelease gives the buffer back", r);
	check(!st.a5_bad && !st.regs_bad && !st.other, "the UDP notification routine: A5, D0, A2 as the guide says", st.a5_bad);
}

/* ---- 6. the rest: passive open, global info, errors, release ------------------------------------ */
static void test_misc(void)
{
	struct conn *A = &conns[0];
	struct call c;
	IP6Address to;
	short r;
	u32 t0;
	clear(&A->c);
	A->c.pb.t.tcpStream = A->stream;
	A->c.pb.t.csParam.open.commandTimeoutValue = 2;
	A->c.pb.t.csParam.open.localAddr.port = 8086;
	start(&A->c, TCPPassiveOpen);
	pump(20);
	{
		struct call s;
		clear(&s);
		s.pb.t.tcpStream = A->stream;
		r = run(&s, TCPStatus);
		check(r == 0 && s.pb.t.csParam.status.connectionState == kIP6StateListen, "TCPPassiveOpen: TCPStatus says listen", r);
	}
	t0 = TickCount();
	r = wait_call(&A->c, 10 * 60);
	check(r == commandTimeout && TickCount() - t0 < 5 * 60, "... and its command timeout of 2 s: commandTimeout", r);
	clear(&c);
	c.pb.t.tcpStream = A->stream;
	r = run(&c, TCPGlobalInfo);
	{
		u32 *tab = (u32 *)c.pb.t.csParam.globalInfo.tcpCDBTable;
		int i, found = 0;
		for (i = 0; tab && i < c.pb.t.csParam.globalInfo.maxTCPConnections; i++)
			found |= tab[i] == A->stream;
		check(r == 0 && c.pb.t.csParam.globalInfo.maxTCPConnections == 32 && found, "TCPGlobalInfo: 32, our stream in the table", r);
	}
	/* errors */
	memset(&to, 0, sizeof to);
	to.port = 80;
	tcp_open(A, &to, 5);
	check(wait_call(&A->c, 5 * 60) == ip6BadAddrErr, "TCPActiveOpen to ::: ip6BadAddrErr", A->c.result);
	a6(&to, PEER6, 80);
	to.addr[0] = 0xff;
	to.addr[1] = 0x02;
	tcp_open(A, &to, 5);
	check(wait_call(&A->c, 5 * 60) == ip6BadAddrErr, "TCPActiveOpen to ff02::1: ip6BadAddrErr", A->c.result);
	str2("fe80::1", &to);
	to.port = 80;
	to.scope = 77;
	tcp_open(A, &to, 5);
	check(wait_call(&A->c, 5 * 60) == ip6ScopeErr, "TCPActiveOpen to fe80::1 with scope 77: ip6ScopeErr", A->c.result);
	a6(&to, PEER6, 0);
	tcp_open(A, &to, 5);
	check(wait_call(&A->c, 5 * 60) == invalidLength, "TCPActiveOpen to port 0: invalidLength", A->c.result);
	clear(&c);
	c.pb.t.tcpStream = 0x12345678;
	check(run(&c, TCPSend) == invalidStreamPtr, "a stream that is not one: invalidStreamPtr", c.result);
	clear(&c);
	check(run(&c, 27) == -17, "UDPMultiCreate (not in version 1): controlErr", c.result);
	clear(&c);
	check(run(&c, 99) == -17, "csCode 99: controlErr", c.result);
	/* release everything */
	{
		int i, ok = 1;
		for (i = 0; i < 4; i++)
			if (conns[i].stream) {
				r = tcp_simple(&conns[i], TCPRelease);
				ok &= r == 0 && conns[i].c.pb.t.csParam.create.rcvBuff == conns[i].rbuf &&
				      conns[i].c.pb.t.csParam.create.rcvBuffLen == sizeof conns[i].rbuf;
				conns[i].stream = 0;
			}
		check(ok, "TCPRelease of every stream gives its buffer back", r);
	}
}

/* A call the Device Manager queued while the driver was taking another (23 November 2026: Aurora68 Play's lock-up on
   the owner's SE). The Device Manager sets drvrActive before it calls the driver, with interrupts on; a call made
   meanwhile (from a completion routine of the stack's deferred task, which may run at the end of any interrupt) is
   only queued. The driver went idle again (_Dequeue, drvrActive cleared) without taking it, so it stayed in the queue
   until the next call, which then waited in its place: a synchronous one for ever. Made here by hand: drvrActive set
   with interrupts off, a call (queued only), drvrActive cleared, then a second call, which the Device Manager hands
   the driver as the head of the queue, the first. Both must complete; with the old driver the second did not (a third
   call pushes it through, so that nothing is left on the queue) */
static struct call q1, q2, q3;
static void test_queue(void)
{
	void ***ut = *(void ****)0x11C;                    /* UTableBase */
	unsigned char *dce = ut && ut[~ipp6] ? (unsigned char *)*ut[~ipp6] : 0;
	short sr, r1, r2;
	if (!dce) {
		check(0, "the .IPP6 device control entry", 0);
		return;
	}
	clear(&q1);
	clear(&q2);
	clear(&q3);
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc", "memory");
	dce[5] |= 0x80;                                    /* drvrActive: as while it takes another call */
	start(&q1, ip6GetInfo);                            /* queued, not handed to the driver */
	dce[5] &= 0x7F;
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc", "memory");
	start(&q2, ip6GetInfo);                            /* the driver gets q1, the head; q2 waits behind it */
	r1 = wait_call(&q1, 3 * 60);
	r2 = wait_call(&q2, 3 * 60);
	check(r1 == 0, "a call queued while the driver was busy completes", r1);
	check(r2 == 0, "... and so does the call behind it, with no other call to push it (the Device Manager's queue)", r2);
	if (r2 == NOT_DONE) {
		start(&q3, ip6GetInfo);
		wait_call(&q2, 3 * 60);
		wait_call(&q3, 3 * 60);
	}
}

static void run_tests(void)
{
	int k, a5bad = 0, regsbad = 0, other = 0;
	char t[160];
	out("---- .IPP6 test ----");
	test_detect();
	if (!ipp6)
		return;
	test_queue();
	test_addrs();
	test_text();
	test_resolve();
	test_tcp();
	test_fallback();
	test_connect_name();
	test_udp();
	test_misc();
	for (k = 0; k < 4; k++) {
		a5bad |= conns[k].st.a5_bad;
		regsbad |= conns[k].st.regs_bad;
		other += conns[k].st.other;
	}
	check(!a5bad, "the TCP notification routines ran with the program's A5", a5bad);
	check(!regsbad, "... with D0 = the event and A2 = the user data", regsbad);
	check(!asr_reentered && !other, "... never re-entered, no unexpected events", other);
	rt_snprintf(t, sizeof t, "every completion routine saw ioResult set first (%d completions)", completions);
	check(!order_bad && completions > 0, t, order_bad);
	check(!stack_bad, "... and the parameter block both in A0 and on the stack", stack_bad);
	rt_snprintf(t, sizeof t, "done: %d passed, %d failed", passed, failed);
	out(t);
}

int main(void)
{
	Rect b = { 40, 4, 470, 600 };
	unsigned char p[64];
	__asm__ volatile("move.l %%a5,%0" : "=g"(app_a5));
	win = NewWindow(0, &b, mac_pstr(p, "IPP6 test"), 1, documentProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(monaco);
	TextSize(9);
	run_tests();
	while (!quit_asked)
		pump(30);
	return 0;
}
