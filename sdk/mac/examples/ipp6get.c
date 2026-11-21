/*
 * ipp6get.c - a small .IPP6 program (docs/IPP6.md): look .IPP6 up, resolve a name to its IPv6 and
 * IPv4 addresses, connect to them in order (IPv6 first, falling back to IPv4 when that fails or
 * when the host has no IPv6 address), and fetch a page over HTTP/1.0. On a Mac without .IPP6 it
 * falls back to MacTCP's .IPP, with an IPv4 address (names would need Apple's DNR glue there).
 *
 * The calls are synchronous for brevity: a real program makes them asynchronously and goes on
 * taking events (docs/IPP6.md, section 13, has an interrupt-time receive chain).
 *
 * Builds with MPW C, THINK C or CodeWarrior and Universal Interfaces (a console program: SIOW or
 * THINK's console), and with Riptide's own GCC build (make -f Makefile.mac examples, which
 * defines IPP6_EXAMPLE_RIPTIDE: a window of results, and on Aurora68 the Pi's log too).
 *
 * Public domain (CC0): copy what helps.
 */
#include <string.h>
#include "IPP6.h"

/* the pages it fetches: Aurora68's testbed peer (tools/pinet.py) answers peer.testbed with both
   kinds of address, www.testbed with IPv4 only, v6only.testbed with IPv6 only */
static const char *const hosts[] = { "peer.testbed", "v6only.testbed", "www.testbed" };
#define PATH        "/"
#define PORT        80
#define FALLBACK_V4 0xC0A84401UL        /* 192.168.68.1: what the .IPP fallback connects to */

/* ==== the platform: five calls ================================================================ */
#ifdef IPP6_EXAMPLE_RIPTIDE
/* Riptide's GCC build: src/mac/app's crt0 and Toolbox glue */
#include "toolbox.h"
int rt_snprintf(char *buf, unsigned long size, const char *fmt, ...);
#define sprintf_n rt_snprintf
static short control(void *pb)          /* PBControlSync */
{
	short r;
	__asm__ volatile("move.l %1,%%a0\n\t.short 0xA004\n\tmove.w %%d0,%0" : "=d"(r) : "g"(pb) : "d0", "d1", "a0", "a1", "d2", "cc", "memory");
	return r;
}
static short open_driver(const char *name, short *ref)
{
	unsigned char pb[80], pname[16];
	short r;
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(pname, name);
	r = mac_pb(pb, TRAP_OPEN);
	*ref = *(short *)(pb + 24);
	return r;
}
static long gestalt(unsigned long sel, long *resp) { return mac_gestalt((long)sel, resp); }
static void *win;
static int line_y = 14;
static void say(const char *s)
{
	unsigned char p[256];
#ifdef AURORA68_LOG
	{   /* the Aurora68 card's mailbox: the line in the Pi's log too */
		char t[220];
		volatile unsigned char *c = (volatile unsigned char *)((*(volatile unsigned char *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
		unsigned long n, seq, i;
		rt_snprintf(t, sizeof t, "ipp6get: %s", s);
		n = strlen(t);
		*(volatile unsigned short *)(c + 8) = 2;
		*(const char *volatile *)(c + 28) = t;
		*(volatile unsigned long *)(c + 32) = n;
		seq = *(volatile unsigned long *)c + 1;
		*(volatile unsigned long *)c = seq;
		for (i = 0; i < 200000u && *(volatile unsigned long *)(c + 4) != seq; i++)
			;
	}
#endif
	SetPort(win);
	MoveTo(6, line_y);
	DrawString(mac_pstr(p, s));
	line_y += 11;
}
static void *new_ptr(long n) { return mac_newptr(n); }
#else
/* MPW, THINK C, CodeWarrior with Universal Interfaces */
#include <stdio.h>
#include <Devices.h>
#include <Gestalt.h>
#include <Memory.h>
#include <stdarg.h>
static int sprintf_n(char *buf, unsigned long size, const char *fmt, ...)   /* C89: no snprintf */
{
	va_list ap;
	int n;
	(void)size;                     /* the lines here are short: 200 bytes hold any of them */
	va_start(ap, fmt);
	n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}
static short control(void *pb) { return PBControlSync((ParmBlkPtr)pb); }
static short open_driver(const char *name, short *ref)
{
	Str255 p;
	p[0] = (unsigned char)strlen(name);
	memcpy(p + 1, name, p[0]);
	return OpenDriver(p, ref);
}
static long gestalt(unsigned long sel, long *resp) { return Gestalt((OSType)sel, resp); }
static void say(const char *s) { printf("%s\n", s); }
static void *new_ptr(long n) { return NewPtr(n); }
#endif

/* ==== .IPP6 ==================================================================================== */
static short ipp6;                      /* the reference number */

static short call6(void *pb, short cs)
{
	((IP6iopb *)pb)->ioCompletion = 0;
	((IP6iopb *)pb)->ioCRefNum = ipp6;
	((IP6iopb *)pb)->csCode = cs;
	return control(pb);
}

static void addr_text(const IP6Address *a, char *out, unsigned short len)
{
	IP6iopb pb;
	memset(&pb, 0, sizeof pb);
	pb.csParam.resolve.addr = *a;
	pb.csParam.resolve.hostName = out;
	pb.csParam.resolve.hostNameLen = len;
	if (call6(&pb, ip6AddrToStr) != 0 && len)
		out[0] = 0;
}

/* the reply: status line and size; returns the bytes received */
static long receive_all(unsigned long stream, char *first, unsigned short firstlen)
{
	static char buf[4096];
	TCP6iopb pb;
	long total = 0;
	short r;
	first[0] = 0;
	for (;;) {
		memset(&pb, 0, sizeof pb);
		pb.tcpStream = stream;
		pb.csParam.receive.commandTimeoutValue = 20;     /* seconds without data */
		pb.csParam.receive.rcvBuff = buf;
		pb.csParam.receive.rcvBuffLen = sizeof buf;
		r = call6(&pb, TCPRcv);
		if (r != 0)
			break;                                   /* connectionClosing: all of it is here */
		if (!total) {
			unsigned short n = pb.csParam.receive.rcvBuffLen, k;
			for (k = 0; k < n && k < firstlen - 1 && buf[k] != '\r' && buf[k] != '\n'; k++)
				first[k] = buf[k];
			first[k] = 0;
		}
		total += pb.csParam.receive.rcvBuffLen;
	}
	return total;
}

static void fetch6(const char *host)
{
	static IP6Address addrs[8];
	IP6iopb look;
	TCP6iopb pb;
	unsigned long stream;
	char line[200], text[64], status[80];
	char req[160];
	unsigned char wds[2 * 6 + 2];
	char *p;
	short r, i, n;

	/* 1. the name: AAAA and A, every IPv6 address first (kIP6OrderDefault would be RFC 6724's
	   order, which puts IPv4 before a unique local IPv6 address) */
	memset(&look, 0, sizeof look);
	look.csParam.resolve.hostName = (char *)host;
	look.csParam.resolve.flags = kIP6OrderIPv6First;
	look.csParam.resolve.results = addrs;
	look.csParam.resolve.maxResults = 8;
	look.csParam.resolve.port = PORT;
	r = call6(&look, ip6Resolve);
	n = look.csParam.resolve.count;
	sprintf_n(line, sizeof line, "%s: %d address%s (result %d)", host, n, n == 1 ? "" : "es", r);
	say(line);
	if (r != 0)
		return;

	/* 2. a stream, and the addresses in order until one connects */
	memset(&pb, 0, sizeof pb);
	pb.csParam.create.rcvBuff = new_ptr(16384);
	pb.csParam.create.rcvBuffLen = 16384;
	if ((r = call6(&pb, TCPCreate)) != 0) {
		sprintf_n(line, sizeof line, "TCPCreate: %d", r);
		say(line);
		return;
	}
	stream = pb.tcpStream;
	for (i = 0; i < n; i++) {
		addr_text(&addrs[i], text, sizeof text);
		memset(&pb.csParam, 0, sizeof pb.csParam);
		pb.tcpStream = stream;
		pb.csParam.open.ulpTimeoutValue = 15;           /* seconds for each address ... */
		pb.csParam.open.validityFlags = timeoutValue;   /* ... valid */
		pb.csParam.open.remoteAddr = addrs[i];
		r = call6(&pb, TCPActiveOpen);
		sprintf_n(line, sizeof line, "  %s [%s]:%u: %s (%d)", IP6_IS_V4(&addrs[i]) ? "IPv4" : "IPv6", text,
		          (unsigned)addrs[i].port, r == 0 ? "connected" : "failed, next", r);
		say(line);
		if (r == 0)
			break;
	}
	if (i == n) {
		say("  no address answered");
	} else {
		/* 3. the request, in one WDS of one piece */
		sprintf_n(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ipp6get\r\n\r\n", PATH, host);
		memset(wds, 0, sizeof wds);
		*(unsigned short *)wds = (unsigned short)strlen(req);
		p = req;
		memcpy(wds + 2, &p, 4);
		memset(&pb.csParam, 0, sizeof pb.csParam);
		pb.tcpStream = stream;
		pb.csParam.send.pushFlag = 1;
		pb.csParam.send.wdsPtr = (char *)wds;
		r = call6(&pb, TCPSend);
		if (r == 0) {
			long got = receive_all(stream, status, sizeof status);
			sprintf_n(line, sizeof line, "  \"%s\", %ld bytes", status, got);
			say(line);
		}
		memset(&pb.csParam, 0, sizeof pb.csParam);
		pb.tcpStream = stream;
		call6(&pb, TCPClose);
	}
	memset(&pb.csParam, 0, sizeof pb.csParam);
	pb.tcpStream = stream;
	call6(&pb, TCPRelease);                                 /* the buffer is ours again */
}

/* ==== the fallback: MacTCP's .IPP, IPv4 (MacTCP.h's TCPiopb, by offset) ======================== */
static void fetch_mactcp(short ipp, unsigned long host)
{
	static unsigned char pb[104];
	static char rbuf[8192], buf[2048];
	static const char req[] = "GET " PATH " HTTP/1.0\r\n\r\n";
	unsigned char wds[8];
	const char *p = req;
	unsigned long stream;
	long total = 0;
	char line[120];
	short r;
#define MT(off, type) (*(type *)(pb + (off)))
	memset(pb, 0, sizeof pb);
	MT(24, short) = ipp;
	MT(26, short) = TCPCreate;
	MT(32, char *) = rbuf;                                  /* rcvBuff, rcvBuffLen */
	MT(36, unsigned long) = sizeof rbuf;
	if ((r = control(pb)) != 0) {
		sprintf_n(line, sizeof line, "MacTCP TCPCreate: %d", r);
		say(line);
		return;
	}
	stream = MT(28, unsigned long);
	memset(pb + 28, 0, sizeof pb - 28);
	MT(26, short) = TCPActiveOpen;
	MT(28, unsigned long) = stream;
	MT(36, unsigned long) = host;                           /* remoteHost, remotePort */
	MT(40, unsigned short) = PORT;
	r = control(pb);
	sprintf_n(line, sizeof line, "MacTCP: open %lu.%lu.%lu.%lu: %d", host >> 24, (host >> 16) & 255,
	          (host >> 8) & 255, host & 255, r);
	say(line);
	if (r == 0) {
		memset(wds, 0, sizeof wds);
		*(unsigned short *)wds = sizeof req - 1;
		memcpy(wds + 2, &p, 4);
		memset(pb + 32, 0, sizeof pb - 32);
		MT(26, short) = TCPSend;
		MT(38, unsigned char *) = wds;                  /* wdsPtr */
		if (control(pb) == 0)
			for (;;) {
				memset(pb + 32, 0, sizeof pb - 32);
				MT(26, short) = TCPRcv;
				MT(32, unsigned char) = 20;             /* commandTimeoutValue */
				MT(36, char *) = buf;
				MT(40, unsigned short) = sizeof buf;
				if (control(pb) != 0)
					break;
				total += MT(40, unsigned short);
			}
		sprintf_n(line, sizeof line, "MacTCP: %ld bytes", total);
		say(line);
	}
	memset(pb + 32, 0, sizeof pb - 32);
	MT(26, short) = TCPRelease;
	control(pb);
#undef MT
}

/* ==== main ==================================================================================== */
static void run(void)
{
	long version = 0;
	short ipp, i;
	char line[160];
	/* 1. .IPP6 if it is there (Gestalt first: an OpenDriver of a name not installed searches the
	   resource files), else MacTCP's .IPP */
	if (gestalt(gestaltIPP6, &version) == 0 && version >= 1 && open_driver(IPP6_DRIVER_NAME_C, &ipp6) == 0) {
		IP6iopb pb;
		static IP6AddrInfo mine[8];
		memset(&pb, 0, sizeof pb);
		call6(&pb, ip6GetInfo);
		sprintf_n(line, sizeof line, ".IPP6 version %lu: %s, features %lx", pb.csParam.info.version,
		          pb.csParam.info.implementation, pb.csParam.info.features);
		say(line);
		memset(&pb, 0, sizeof pb);
		pb.csParam.addrs.buffer = mine;
		pb.csParam.addrs.bufferLen = sizeof mine;
		if (call6(&pb, ip6GetAddrs) == 0)
			for (i = 0; i < pb.csParam.addrs.count; i++) {
				char text[64];
				addr_text(&mine[i].addr, text, sizeof text);
				sprintf_n(line, sizeof line, "  %s %s/%u", mine[i].ifName, text, (unsigned)mine[i].prefixLen);
				say(line);
			}
		for (i = 0; i < (short)(sizeof hosts / sizeof hosts[0]); i++)
			fetch6(hosts[i]);
	} else if (open_driver(".IPP", &ipp) == 0) {
		say("no .IPP6 here: MacTCP's .IPP, IPv4 only");
		fetch_mactcp(ipp, FALLBACK_V4);
	} else {
		say("no TCP/IP on this Mac");
	}
	say("done");
}

#ifdef IPP6_EXAMPLE_RIPTIDE
int main(void)
{
	Rect b = { 40, 10, 330, 500 };
	unsigned char p[32];
	EventRecord ev;
	unsigned long t0;
	win = NewWindow(0, &b, mac_pstr(p, "ipp6get"), 1, documentProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(monaco);
	TextSize(9);
	run();
	/* the results stay until a click or a key, or for two minutes */
	t0 = TickCount();
	while (TickCount() - t0 < 120 * 60) {
		if (WaitNextEvent(everyEvent, &ev, 30, NULL) && (ev.what == mouseDown || ev.what == keyDown))
			break;
	}
	return 0;
}
#else
int main(void)
{
	run();
	return 0;
}
#endif
