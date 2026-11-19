/* tests/mac/http_test.c - Riptide on the classic Mac OS: the Browser test, a MacTCP program that uses
 * Riptide the way a web browser of 1997 uses MacTCP, against the Aurora68 testbed peer
 * (tools/pinet.py: HTTP on port 80 with / a page, /big 256 KB, /slow the page in pieces a second
 * apart, /reset a reset after the headers; echo on port 7; nothing on port 81; DNS for *.testbed,
 * 192.168.68.1 named peer.testbed, 192.168.68.66 never answered).
 *
 * A well-behaved application (Macintosh Human Interface Guidelines, 1992): the tests run when it
 * starts and again with File > Run Tests (Command-R); while they run, the event loop goes on
 * (every call is asynchronous and its completion is looked for between events), the results
 * window scrolls and redraws, and File > Stop (Command-period), the close box, File > Close and
 * File > Quit stop the tests: TCPAbort and TCPRelease on every stream, CloseResolver, and nothing
 * is left with the driver that points into the program. File > Quit Leaving Streams Open
 * (Command-K) quits without releasing, on purpose: the driver must notice the program is gone and
 * release its streams itself (its log says so).
 *
 * What is tested:
 *   - MacTCP looked for as programs look for it: Gestalt 'mtcp'; the resolver found as Apple's DNR
 *     glue finds it (1991 and 1988 versions) with MacTCP's "MacTCP DNR" left in the System Folder;
 *     StrToAddr with a dotted address (answered at once: noErr, rtnCode noErr, the address, the
 *     other three zero, with or without a result procedure) and with names; AddrToName with and without a name, and with a name server that never answers (the
 *     result procedure is still called, with MacTCP's error); AddrToStr, HInfo, MXInfo, EnumCache.
 *   - .IPP opened by name; a Close (a program quitting) leaves it open for the others.
 *   - Three streams at once, every call asynchronous with a completion routine, each stream with
 *     a notification routine (ASR): TCPActiveOpen; TCPSend with a WDS in three pieces;
 *     TCPNoCopyRcv with a six-entry RDS and TCPRcvBfrReturn (the page); TCPRcv into a locked
 *     handle (the large file, every line checked); a TCPRcv that times out, then receives issued
 *     again from inside the completion routine with a timeout of 0 (the slow page); reading to
 *     connectionClosing; TCPStatus; TCPClose and TCPTerminate (TCPULPClose).
 *   - A fourth way: TCPStatus polled for amtUnreadData, then TCPRcv of exactly that, and a
 *     synchronous TCPStatus made inside the ASR (which must not hang).
 *   - The completion routines check that ioResult is set before they run, that A0 and the
 *     parameter on the stack (MacTCP.h's C completion routines) are the block; the ASRs that A5 is
 *     the program's, D0 the event and A2 the user data, and that they are never re-entered.
 *   - A stream opened again; a reset (TCPRemoteAbort); TCPAbort; an open refused (openFailed);
 *     TCPGlobalInfo; TCPRelease.
 *   - Connections the server closes after 30 idle seconds while the program sits in
 *     WaitNextEvent, once in front and once with the Finder in front: TCPClosing with the
 *     program's A5, then TCPClose and TCPTerminate.
 * Each result goes to the window and, built with AURORA68_LOG, to the Pi's log as
 * "[MAC] Browser test: ...".
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "toolbox.h"

typedef unsigned long u32;
int rt_snprintf(char *buf, unsigned long size, const char *fmt, ...);
int mac_alert(int icon, const char *text, const char *const *btn, int n, int def, int cancel);

#define PEER   0xC0A84401UL             /* 192.168.68.1 */
#define MAC_IP 0xC0A84402UL             /* 192.168.68.2: what the peer's DHCP gives */
/* csCodes and results (MacTCP Programmer's Guide) */
enum { TCPCreate = 30, TCPActiveOpen = 32, TCPSend = 34, TCPNoCopyRcv, TCPRcvBfrReturn, TCPRcv, TCPClose,
       TCPAbort, TCPStatus, TCPRelease = 42, TCPGlobalInfo };
#define connectionClosing     -23005
#define connectionDoesntExist -23008
#define connectionTerminated  -23012
#define openFailed            -23015
#define commandTimeout        -23016
#define cacheFault            -23042
#define authNameErr           -23045
#define noAnsErr              -23046
#define TCPClosing 1
#define TCPTerminate 3
#define TCPDataArrival 4
#define TCPRemoteAbort 2
#define TCPULPAbort 6
#define TCPULPClose 7
#define NOT_DONE 1                      /* a call still in progress (stopped, or out of time) */

#define APPLE_ID 128
#define FILE_ID  129
#define EDIT_ID  130
#define SBW      16
enum { F_RUN = 1, F_STOP, F_SEP1, F_CLOSE, F_QUIT, F_SEP2, F_LEAVE };

static void *win, *sbar, *menus[3];
static int running, stop_asked, quit_asked, leave_open, in_cleanup, passed, failed, follow = 1;
static void *app_a5;
static short ipp;

/* ---- the results window ---------------------------------------------------------------------- */
#define MAXL 400
#define LINEW 100
static char lines[MAXL][LINEW];
static int nlines;
static int line_h = 11;

static void *find_str(const char *h, const char *n)
{
	unsigned long k = strlen(n);
	for (; *h; h++)
		if (!strncmp(h, n, k))
			return (void *)h;
	return 0;
}

static Rect text_rect(void)
{
	Rect r = *PORT_RECT(win);
	r.right -= SBW;
	r.bottom -= SBW;
	return r;
}
static int visible(void) { Rect r = text_rect(); return (r.bottom - r.top - 4) / line_h; }

static void draw_lines(void)
{
	Rect r = text_rect();
	int top = GetCtlValue(sbar), i;
	unsigned char p[LINEW + 1];
	SetPort(win);
	EraseRect(&r);
	for (i = 0; i < visible() && top + i < nlines; i++) {
		MoveTo(r.left + 4, r.top + 2 + (i + 1) * line_h - 2);
		DrawString(mac_pstr(p, lines[top + i]));
	}
}

static void sync_bar(void)
{
	int max = nlines - visible();
	SetCtlMax(sbar, max < 0 ? 0 : max);
	if (follow)
		SetCtlValue(sbar, max < 0 ? 0 : max);
}

void c_ctl_action(void *ctl, int part)
{
	int v = GetCtlValue(ctl), page = visible() - 1;
	if (page < 1)
		page = 1;
	switch (part) {
	case 20: v--; break;
	case 21: v++; break;
	case 22: v -= page; break;
	case 23: v += page; break;
	}
	SetCtlValue(ctl, v);
	follow = GetCtlValue(ctl) >= GetCtlMax(ctl);
	draw_lines();
}

static void out(const char *s)
{
#ifdef AURORA68_LOG
	char t[220];
	volatile unsigned char *c = (volatile unsigned char *)((*(volatile unsigned char *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
	u32 n, seq, i;
	rt_snprintf(t, sizeof t, "Browser test: %s", s);
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
	sync_bar();
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

/* ---- menus and events ------------------------------------------------------------------------ */
static void dim_menus(void)
{
	if (running) {
		DisableItem(menus[1], F_RUN);
		EnableItem(menus[1], F_STOP);
	} else {
		EnableItem(menus[1], F_RUN);
		DisableItem(menus[1], F_STOP);
	}
}

static void ask_stop(void)
{
	if (running && !stop_asked) {
		stop_asked = 1;
		out("Stopping...");
	}
}

static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	if (id == APPLE_ID) {
		if (item == 1) {
			static const char *const ok[] = { "OK" };
			mac_alert(1, "Browser test\n\nRiptide's MacTCP interface used as a web browser of 1997 uses it, "
			             "against the Aurora68 testbed's HTTP server (192.168.68.1).", ok, 1, 0, -1);
		} else {
			unsigned char name[64];
			GetItem(menus[0], item, name);
			OpenDeskAcc(name);
		}
	} else if (id == FILE_ID) {
		if (item == F_RUN && !running)
			running = 2;                    /* the main loop starts the run */
		else if (item == F_STOP)
			ask_stop();
		else if (item == F_CLOSE || item == F_QUIT) {
			quit_asked = 1;
			ask_stop();
		} else if (item == F_LEAVE) {
			leave_open = 1;
			quit_asked = 1;
		}
	} else if (id == EDIT_ID) {
		SystemEdit(item - 1);
	}
	HiliteMenu(0);
}

static void event(EventRecord *ev)
{
	void *w;
	int part;
	switch (ev->what) {
	case mouseDown:
		part = FindWindow(pt_long(ev->where), &w);
		if (part == inMenuBar) {
			dim_menus();
			do_menu(MenuSelect(pt_long(ev->where)));
		} else if (part == inSysWindow) {
			SystemClick(ev, w);
		} else if (part == inDrag) {
			Rect lim = *RGN_BBOX(LM_GRAYRGN);
			DragWindow(w, pt_long(ev->where), &lim);
		} else if (part == inGoAway) {
			if (TrackGoAway(w, pt_long(ev->where))) {
				quit_asked = 1;
				ask_stop();
			}
		} else if (part == inContent) {
			if (w != FrontWindow()) {
				SelectWindow(w);
			} else {
				Point p = ev->where;
				void *ctl;
				int cp;
				SetPort(win);
				GlobalToLocal(&p);
				cp = FindControl(pt_long(p), win, &ctl);
				if (cp == 129) {
					TrackControl(ctl, pt_long(p), NULL);
					follow = GetCtlValue(sbar) >= GetCtlMax(sbar);
					draw_lines();
				} else if (cp) {
					TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
				}
			}
		}
		break;
	case keyDown:
	case autoKey:
		if (ev->modifiers & cmdKey) {
			int ch = (int)(ev->message & charCodeMask);
			if (ch == '.')
				ask_stop();
			else {
				dim_menus();
				do_menu(MenuKey(ch));
			}
		}
		break;
	case updateEvt:
		if ((void *)ev->message == win) {
			SetPort(win);
			BeginUpdate(win);
			draw_lines();
			DrawControls(win);
			DrawGrowIcon(win);
			EndUpdate(win);
		}
		break;
	case activateEvt:
		if ((void *)ev->message == win) {
			HiliteControl(sbar, (ev->modifiers & activeFlag) ? 0 : 255);
			DrawGrowIcon(win);
		}
		break;
	}
}

/* one round of events: the waits of the tests come here, so the program never stops answering */
static void pump(long sleep)
{
	EventRecord ev;
	if (WaitNextEvent(everyEvent, &ev, sleep, NULL))
		event(&ev);
}

static int stopped(void) { return (stop_asked || leave_open) && !in_cleanup; }

/* ---- calls ----------------------------------------------------------------------------------- */
/* a TCPiopb with our bookkeeping after it: the completion routine (comp_routine) sets done, the
   result, whether ioResult was already the result when it ran, and whether the block was both in
   A0 (the Device Manager's convention) and on the stack (MacTCP.h's C completion routines) */
struct call {
	unsigned char pb[104];
	volatile short done;            /* 104 */
	volatile short result;          /* 106: D0 */
	volatile short order_ok;        /* 108: ioResult == D0 and not 1 */
	volatile short stack_ok;        /* 110: 4(sp) == A0 */
};
void comp_routine(void);
__asm__(".globl comp_routine\ncomp_routine:\n"
        "\tmove.w %d0,106(%a0)\n"
        "\tclr.w 108(%a0)\n"
        "\tclr.w 110(%a0)\n"
        "\tcmp.l 4(%sp),%a0\n"
        "\tbne.s 2f\n"
        "\tmove.w #1,110(%a0)\n"
        "2:\tcmp.w 16(%a0),%d0\n"
        "\tbne.s 1f\n"
        "\tcmp.w #1,%d0\n"
        "\tbeq.s 1f\n"
        "\tmove.w #1,108(%a0)\n"
        "1:\tmove.w #1,104(%a0)\n"
        "\trts\n");
/* _Control without the self-modifying mac_pb (a notification routine may call it at any time) */
short pb_control_async(void *pb);
short pb_control_sync(void *pb);
__asm__(".globl pb_control_async\npb_control_async:\n\tmove.l 4(%sp),%a0\n\t.short 0xA404\n\text.l %d0\n\trts\n"
        ".globl pb_control_sync\npb_control_sync:\n\tmove.l 4(%sp),%a0\n\t.short 0xA004\n\text.l %d0\n\trts\n");
static int completions, order_bad, stack_bad;

static void start(struct call *c, short cs, u32 stream)
{
	*(short *)(c->pb + 24) = ipp;
	*(short *)(c->pb + 26) = cs;
	*(u32 *)(c->pb + 28) = stream;
	*(void **)(c->pb + 12) = (void *)comp_routine;
	c->done = 0;
	pb_control_async(c->pb);
}

/* the result of a call started with start(), events going on meanwhile; NOT_DONE when it did not
   finish in time or a stop was asked for (it stays with the driver until its stream is released) */
static short wait_call(struct call *c, u32 ticks)
{
	u32 t0 = TickCount();
	while (!c->done && TickCount() - t0 < ticks && !stopped())
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

/* a call of its own, waited for: the block in pb (copied in and back) */
static struct call one;
static short ctl(unsigned char *pb, short cs, u32 stream)
{
	short r;
	if (!one.done && *(short *)(one.pb + 26))
		return NOT_DONE;                /* the last one never finished: not reused */
	memcpy(one.pb, pb, 104);
	start(&one, cs, stream);
	r = wait_call(&one, 20 * 60);
	if (r != NOT_DONE)
		memcpy(pb, one.pb, 104);
	return r;
}

/* ---- the notification routine ---------------------------------------------------------------- */
struct sstate {                         /* a stream's events, the ASR's user data */
	volatile int closing, terminate, reason, data, other;
	volatile int a5_bad, regs_bad;
	int status_in_asr;              /* make a synchronous TCPStatus inside the ASR */
	volatile int status_done, status_r, status_unread;
	u32 stream;
};
static volatile int in_asr, asr_reentered;
static unsigned char asr_pb[104];
void asr_c(u32 stream, u32 event, struct sstate *st, u32 reason, void *a5, u32 d0, void *a2);
void asr_c(u32 stream, u32 event, struct sstate *st, u32 reason, void *a5, u32 d0, void *a2)
{
	if (in_asr)
		asr_reentered = 1;
	in_asr = 1;
	if (a5 != app_a5)
		st->a5_bad = 1;
	if ((d0 & 0xFFFF) != event || a2 != (void *)st)
		st->regs_bad = 1;
	if (event == TCPClosing)
		st->closing++;
	else if (event == TCPTerminate) {
		st->terminate++;
		st->reason = (int)reason;
	} else if (event == TCPDataArrival) {
		st->data++;
		if (st->status_in_asr && !st->status_done) {
			/* what some programs do: ask at once how much has come, synchronously */
			memset(asr_pb, 0, sizeof asr_pb);
			*(short *)(asr_pb + 24) = ipp;
			*(short *)(asr_pb + 26) = TCPStatus;
			*(u32 *)(asr_pb + 28) = stream;
			st->status_r = pb_control_sync(asr_pb);
			st->status_unread = *(unsigned short *)(asr_pb + 60);
			st->status_done = 1;
		}
	} else
		st->other++;
	in_asr = 0;
}
/* pascal TCPNotifyProc(stream, eventCode, userDataPtr, terminReason, icmpMsg): 16 bytes of
   arguments; asr_c(stream, event, udata, reason, A5, D0, A2) */
void pas_asr(void);
__asm__(".globl pas_asr\npas_asr:\n"
        "\tmovem.l %d0-%d2/%a0-%a2,-(%sp)\n"          /* 24 bytes */
        "\tmove.l %a2,-(%sp)\n"
        "\tmove.l %d0,-(%sp)\n"
        "\tmove.l %a5,-(%sp)\n"                       /* the return address is at 36(sp) now */
        "\tmoveq #0,%d1\n\tmove.w 44(%sp),%d1\n\tmove.l %d1,-(%sp)\n"     /* reason */
        "\tmove.l 50(%sp),-(%sp)\n"                   /* udata (46 + 4) */
        "\tmoveq #0,%d1\n\tmove.w 58(%sp),%d1\n\tmove.l %d1,-(%sp)\n"     /* event (50 + 8) */
        "\tmove.l 64(%sp),-(%sp)\n"                   /* stream (52 + 12) */
        "\tjsr asr_c\n"
        "\tlea 28(%sp),%sp\n"
        "\tmovem.l (%sp)+,%d0-%d2/%a0-%a2\n"
        "\tmove.l (%sp)+,%a0\n"
        "\tlea 16(%sp),%sp\n"
        "\tjmp (%a0)\n");

/* ---- streams --------------------------------------------------------------------------------- */
struct conn {
	struct call c;                  /* the call in progress (first: the completion's A0 is the conn) */
	const char *name, *path;
	u32 stream;
	char rbuf[16384];               /* the stream's receive buffer (TCPCreate) */
	struct sstate st;
	unsigned char rds[6 * 6 + 2];   /* TCPNoCopyRcv's RDS: six entries and the terminating zero */
	char *in;                       /* TCPRcv's buffer (a locked handle for the large file) */
	unsigned short inlen;
	char body[2048];                /* the start of what came */
	u32 got, bodylen;
	int nocopy, chain, eof, failed, expect_error, lines_bad;
	u32 big_pos;
	char big_line[64];
	int big_have, in_body;
	char hdr_tail[4];
	volatile int chain_done, chain_result;
	volatile u32 chain_got;
};
static struct conn conns[5];
static char small_in[4][4096];

static short tcp_create(struct conn *k)
{
	unsigned char pb[104];
	short r;
	memset(pb, 0, sizeof pb);
	*(char **)(pb + 32) = k->rbuf;
	*(u32 *)(pb + 36) = sizeof k->rbuf;
	*(void **)(pb + 40) = (void *)pas_asr;
	*(void **)(pb + 44) = (void *)&k->st;
	r = ctl(pb, TCPCreate, 0);
	k->stream = r == 0 ? *(u32 *)(pb + 28) : 0;
	k->st.stream = k->stream;
	return r;
}

static void open_to(struct conn *k, u32 host, unsigned short port)
{
	int keep = k->st.status_in_asr;
	memset(k->c.pb + 32, 0, 72);
	*(u32 *)(k->c.pb + 36) = host;
	*(unsigned short *)(k->c.pb + 40) = port;
	k->c.pb[32] = 20;                                       /* ULP timeout 20 s ... */
	k->c.pb[34] = 0x80;                                     /* ... valid */
	memset(&k->st, 0, sizeof k->st);
	k->st.status_in_asr = keep;
	k->st.stream = k->stream;
	start(&k->c, TCPActiveOpen, k->stream);
}
static void open_start(struct conn *k, unsigned short port) { open_to(k, PEER, port); }

/* GET path HTTP/1.0, in three pieces of one WDS */
static short send_get(struct conn *k)
{
	static char l1[80];
	static const char l2[] = "Host: peer.testbed\r\nUser-Agent: Riptide browser test\r\n", l3[] = "\r\n";
	static unsigned char wds[4 * 6];
	unsigned char pb[104];
	const char *p;
	rt_snprintf(l1, sizeof l1, "GET %s HTTP/1.0\r\n", k->path);
	*(unsigned short *)wds = (unsigned short)strlen(l1);
	p = l1; memcpy(wds + 2, &p, 4);
	*(unsigned short *)(wds + 6) = sizeof l2 - 1;
	p = l2; memcpy(wds + 8, &p, 4);
	*(unsigned short *)(wds + 12) = sizeof l3 - 1;
	p = l3; memcpy(wds + 14, &p, 4);
	*(unsigned short *)(wds + 18) = 0;
	memset(pb, 0, sizeof pb);
	pb[35] = 1;                                             /* push */
	*(unsigned char **)(pb + 38) = wds;
	return ctl(pb, TCPSend, k->stream);
}

/* the completion of a receive issued again from inside the completion routine (the slow page) */
void comp_chain(void);
void c_comp_chain(struct conn *k);
__asm__(".globl comp_chain\ncomp_chain:\n\tmovem.l %d0-%d2/%a0-%a1,-(%sp)\n\tmove.l %a0,-(%sp)\n"
        "\tjsr c_comp_chain\n\taddq.l #4,%sp\n\tmovem.l (%sp)+,%d0-%d2/%a0-%a1\n\trts\n");
static void take(struct conn *k, const char *p, u32 n);
void c_comp_chain(struct conn *k)
{
	short r = *(volatile short *)(k->c.pb + 16);
	if (r == 0 && !in_cleanup) {
		u32 n = *(unsigned short *)(k->c.pb + 40);
		k->chain_got += n;
		take(k, k->in, n);
		/* the next receive, from here (interrupt time), with no timeout */
		memset(k->c.pb + 32, 0, 72);
		*(char **)(k->c.pb + 36) = k->in;
		*(unsigned short *)(k->c.pb + 40) = k->inlen;
		pb_control_async(k->c.pb);
		return;
	}
	k->chain_result = r;
	k->chain_done = 1;
}

static void rcv_start(struct conn *k, int secs)
{
	memset(k->c.pb + 32, 0, 72);
	k->c.pb[32] = (unsigned char)secs;                      /* commandTimeoutValue, 0: none */
	if (k->nocopy) {
		*(unsigned char **)(k->c.pb + 42) = k->rds;
		*(unsigned short *)(k->c.pb + 46) = 6;
		start(&k->c, TCPNoCopyRcv, k->stream);
	} else if (k->chain) {
		/* the completion routine issues the next receive itself */
		*(char **)(k->c.pb + 36) = k->in;
		*(unsigned short *)(k->c.pb + 40) = k->inlen;
		*(short *)(k->c.pb + 24) = ipp;
		*(short *)(k->c.pb + 26) = TCPRcv;
		*(u32 *)(k->c.pb + 28) = k->stream;
		*(void **)(k->c.pb + 12) = (void *)comp_chain;
		k->c.done = 0;
		pb_control_async(k->c.pb);
	} else {
		*(char **)(k->c.pb + 36) = k->in;
		*(unsigned short *)(k->c.pb + 40) = k->inlen;
		start(&k->c, TCPRcv, k->stream);
	}
}

/* /big: line k is "%06d Riptide on the Mac: line of the large file, 64 bytes long." cut to 62 bytes,
   then CR LF: every line checked */
static void big_byte(struct conn *k, char ch)
{
	k->big_line[k->big_have++] = ch;
	if (k->big_have == 64) {
		char want[80];
		rt_snprintf(want, sizeof want, "%06lu Riptide on the Mac: line of the large file, 64 bytes long.",
		            (unsigned long)(k->big_pos / 64));
		if (memcmp(k->big_line, want, 62) || k->big_line[62] != '\r' || k->big_line[63] != '\n')
			k->lines_bad++;
		k->big_have = 0;
	}
	k->big_pos++;
}

static void take(struct conn *k, const char *p, u32 n)
{
	u32 i;
	k->got += n;
	for (i = 0; i < n; i++) {
		char ch = p[i];
		if (!k->in_body) {
			k->hdr_tail[0] = k->hdr_tail[1];
			k->hdr_tail[1] = k->hdr_tail[2];
			k->hdr_tail[2] = k->hdr_tail[3];
			k->hdr_tail[3] = ch;
			if (!memcmp(k->hdr_tail, "\r\n\r\n", 4))
				k->in_body = 1;
			continue;
		}
		if (k->bodylen < sizeof k->body - 1)
			k->body[k->bodylen] = ch;
		k->bodylen++;
		if (!strcmp(k->path, "/big"))
			big_byte(k, ch);
	}
	k->body[k->bodylen < sizeof k->body - 1 ? k->bodylen : sizeof k->body - 1] = 0;
}

static void reset_conn(struct conn *k)
{
	k->got = k->bodylen = k->big_pos = 0;
	k->eof = k->failed = k->in_body = k->big_have = k->lines_bad = 0;
	k->chain_done = k->chain_result = 0;
	k->chain_got = 0;
	memset(k->hdr_tail, 0, sizeof k->hdr_tail);
	k->body[0] = 0;
}

/* a receive finished: the data, the buffers given back, the next receive */
static void rcv_done(struct conn *k, short r)
{
	char t[120];
	if (r == 0) {
		if (k->nocopy) {
			unsigned char pb[104];
			unsigned char *e;
			short rr;
			for (e = k->rds; *(unsigned short *)e; e += 6) {
				char *p;
				memcpy(&p, e + 2, 4);
				take(k, p, *(unsigned short *)e);
			}
			memset(pb, 0, sizeof pb);
			*(unsigned char **)(pb + 42) = k->rds;
			rr = ctl(pb, TCPRcvBfrReturn, k->stream);
			if (rr) {
				rt_snprintf(t, sizeof t, "%s: TCPRcvBfrReturn", k->name);
				check(0, t, rr);
			}
		} else {
			take(k, k->in, *(unsigned short *)(k->c.pb + 40));
		}
		rcv_start(k, 10);
	} else if (r == connectionClosing) {
		k->eof = 1;
	} else {
		if (!k->expect_error) {
			rt_snprintf(t, sizeof t, "%s: receive", k->name);
			check(0, t, r);
		}
		k->failed = 1;
	}
}

/* the receives of conns[0..n-1] until each has read to connectionClosing, events going on */
static void read_all(int n, u32 ticks)
{
	u32 t0 = TickCount();
	int k, busy = 1;
	while (busy && TickCount() - t0 < ticks && !stopped()) {
		busy = 0;
		for (k = 0; k < n; k++) {
			struct conn *c = &conns[k];
			if (c->eof || c->failed)
				continue;
			busy = 1;
			if (c->chain) {
				if (c->chain_done) {
					if (c->chain_result == connectionClosing)
						c->eof = 1;
					else {
						c->chain = 0;
						rcv_done(c, (short)c->chain_result);
					}
				}
				continue;
			}
			if (!c->c.done)
				continue;
			completions++;
			if (!c->c.order_ok)
				order_bad++;
			if (!c->c.stack_ok)
				stack_bad++;
			rcv_done(c, c->c.result);
		}
		if (busy)
			pump(1);
	}
}

static short status(struct conn *k, unsigned char *pb)
{
	memset(pb, 0, 104);
	return ctl(pb, TCPStatus, k->stream);
}

/* ---- the resolver, found as Apple's DNR glue finds it ---------------------------------------- */
struct hostInfo { long rtnCode; char cname[255]; char pad; u32 addr[4]; };
typedef long (*dnr_fn)(long sel, ...);
static volatile long done;
static char dnr_from[64];
static dnr_fn dnr;
void pas_result(void);
__asm__(".globl pas_result\npas_result:\n\tmove.l 4(%sp),%a0\n\tmove.l #1,(%a0)\n\tmove.l (%sp)+,%a1\n\taddq.l #8,%sp\n\tjmp (%a1)\n");

/* the glue's SearchFolderForDNRP: one folder's files by index, of the type and creator, the first
   holding a 'dnrp' */
static short search_folder(long type, long creator, short vref, long dir)
{
	unsigned char pb[80], name[64];
	short i;
	for (i = 1; i < 500; i++) {
		short ref;
		memset(pb, 0, sizeof pb);
		*(unsigned char **)(pb + 18) = name;
		*(short *)(pb + 22) = vref;
		*(short *)(pb + 28) = i;
		*(long *)(pb + 48) = dir;
		if (mac_pb(pb, TRAP_HGETFINFO) != 0)
			return -1;
		if (*(long *)(pb + 32) != type || *(long *)(pb + 36) != creator)
			continue;
		ref = HOpenResFile(vref, dir, name, 1);
		if (!GetIndResource(MAC_TYPE('d', 'n', 'r', 'p'), 1)) {
			if (ref > 0)
				CloseResFile(ref);
			continue;
		}
		memcpy(dnr_from, name + 1, name[0]);
		dnr_from[name[0]] = 0;
		return ref;
	}
	return -1;
}

static dnr_fn open_resolver(long *err)
{
	short vref, ref = -1;
	long dir;
	void **h;
	if (mac_findfolder(-32768, MAC_TYPE('c', 't', 'r', 'l'), 0, &vref, &dir) == 0)
		ref = search_folder(MAC_TYPE('c', 'd', 'e', 'v'), MAC_TYPE('z', 't', 'c', 'p'), vref, dir);
	if (ref == -1 && mac_findfolder(-32768, MAC_TYPE('m', 'a', 'c', 's'), 0, &vref, &dir) == 0)
		ref = search_folder(MAC_TYPE('c', 'd', 'e', 'v'), MAC_TYPE('m', 't', 'c', 'p'), vref, dir);
	if (ref == -1 && mac_findfolder(-32768, MAC_TYPE('c', 't', 'r', 'l'), 0, &vref, &dir) == 0)
		ref = search_folder(MAC_TYPE('c', 'd', 'e', 'v'), MAC_TYPE('m', 't', 'c', 'p'), vref, dir);
	h = GetIndResource(MAC_TYPE('d', 'n', 'r', 'p'), 1);
	if (!h) {
		*err = ResError();
		return 0;
	}
	DetachResource(h);
	if (ref != -1)
		CloseResFile(ref);
	mac_hlock(h);
	*err = ((dnr_fn)*h)(1L, (char *)0);                     /* OPENRESOLVER */
	return *err ? 0 : (dnr_fn)*h;
}

/* the 1988 glue: the System Folder's first 'cdev' 'mtcp', whatever it holds */
static int first_mtcp(char *outname)
{
	unsigned char pb[80], name[64];
	short vref, i;
	long dir;
	if (mac_findfolder(-32768, MAC_TYPE('m', 'a', 'c', 's'), 0, &vref, &dir) != 0)
		return 0;
	for (i = 1; i < 500; i++) {
		memset(pb, 0, sizeof pb);
		*(unsigned char **)(pb + 18) = name;
		*(short *)(pb + 22) = vref;
		*(short *)(pb + 28) = i;
		*(long *)(pb + 48) = dir;
		if (mac_pb(pb, TRAP_HGETFINFO) != 0)
			return 0;
		if (*(long *)(pb + 32) == MAC_TYPE('c', 'd', 'e', 'v') && *(long *)(pb + 36) == MAC_TYPE('m', 't', 'c', 'p')) {
			memcpy(outname, name + 1, name[0]);
			outname[name[0]] = 0;
			return 1;
		}
	}
	return 0;
}

/* a file in the System Folder's "Disabled (Riptide)" folder */
static int moved_aside(const char *file)
{
	unsigned char pb[80], name[64];
	char path[80];
	short vref;
	long dir;
	if (mac_findfolder(-32768, MAC_TYPE('m', 'a', 'c', 's'), 0, &vref, &dir) != 0)
		return 0;
	rt_snprintf(path, sizeof path, ":Disabled (Riptide):%s", file);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(name, path);
	*(short *)(pb + 22) = vref;
	*(long *)(pb + 48) = dir;
	return mac_pb(pb, TRAP_HGETFINFO) == 0;
}

/* a lookup through the result procedure: what the call returned in *first, then the rtnCode the
   result procedure brought (or NOT_DONE); *ticks how long it took */
static long lookup(long sel, long arg, struct hostInfo *hi, u32 *ticks, long *first)
{
	u32 t0 = TickCount();
	long r;
	done = 0;
	memset(hi, 0, sizeof *hi);
	r = dnr(sel, arg, hi, (void *)pas_result, (char *)&done);
	*first = r;
	if (r == cacheFault) {
		while (!done && TickCount() - t0 < 40 * 60 && !stopped())
			pump(1);
		r = done ? hi->rtnCode : NOT_DONE;
	}
	*ticks = TickCount() - t0;
	return r;
}

static void test_resolver(void)
{
	static struct hostInfo hi;
	long r, first_r, resp = 0;
	char t[200], s[32], first[64] = "";
	u32 ticks;
	int any;
	r = mac_gestalt(MAC_TYPE('m', 't', 'c', 'p'), &resp);
	check(r == 0 && resp == 3, "Gestalt 'mtcp' answers 3 (MacTCP 2.0)", r ? r : resp);
	/* the image keeps the "MacTCP DNR" MacTCP left in the System Folder ('cdev' 'mtcp', MacTCP's own
	   'dnrp'), which comes before Riptide DNR: Riptide moves it aside at startup */
	any = first_mtcp(first);
	rt_snprintf(t, sizeof t, "the 1988 glue's choice, the System Folder's first 'cdev' 'mtcp': \"%s\"", any ? first : "none");
	check(any && !strcmp(first, "Riptide DNR"), t, 0);
	check(moved_aside("MacTCP DNR"), "MacTCP DNR moved into System Folder:Disabled (Riptide)", 0);
	dnr = open_resolver(&r);
	rt_snprintf(t, sizeof t, "OpenResolver as the 1991 glue does it: 'dnrp' from \"%s\"", dnr_from);
	check(dnr != 0, t, r);
	if (!dnr)
		return;
	/* a URL by number: "returned immediately" (the guide), noErr, without the result procedure */
	memset(&hi, 0xAA, sizeof hi);
	done = 0;
	r = dnr(3L, "192.168.68.1", &hi, (void *)pas_result, (char *)&done);
	check(r == 0 && hi.rtnCode == 0 && hi.addr[0] == PEER && !hi.addr[1] && !hi.addr[2] && !hi.addr[3] &&
	      !strcmp(hi.cname, "192.168.68.1") && !done,
	      "StrToAddr 192.168.68.1: at once, noErr, rtnCode noErr, the other addresses 0", r);
	memset(&hi, 0xAA, sizeof hi);
	r = dnr(3L, "192.168.68.1", &hi, (void *)0, (char *)0);
	check(r == 0 && hi.rtnCode == 0 && hi.addr[0] == PEER && !hi.addr[1],
	      "StrToAddr 192.168.68.1 without a result procedure: noErr at once", r);
	r = lookup(3L, (long)"peer.testbed", &hi, &ticks, &first_r);
	check(r == 0 && hi.addr[0] == PEER, "StrToAddr peer.testbed, through the result procedure", r);
	r = lookup(3L, (long)"nowhere.testbad", &hi, &ticks, &first_r);
	check(r == authNameErr, "StrToAddr of a name that does not exist: authNameErr", r);
	r = lookup(6L, (long)PEER, &hi, &ticks, &first_r);
	rt_snprintf(t, sizeof t, "AddrToName 192.168.68.1: \"%s\"", hi.cname);
	check(r == 0 && !strcmp(hi.cname, "peer.testbed"), t, r);
	r = lookup(6L, (long)0xC0A84409UL, &hi, &ticks, &first_r);
	rt_snprintf(t, sizeof t, "AddrToName 192.168.68.9 (no name): authNameErr in %lu ticks", (unsigned long)ticks);
	check(r == authNameErr && ticks < 5 * 60, t, r);
	r = lookup(6L, (long)0xC0A84442UL, &hi, &ticks, &first_r);
	rt_snprintf(t, sizeof t, "AddrToName 192.168.68.66 (the name server never answers): noAnsErr in %lu ticks",
	            (unsigned long)ticks);
	check(r == noAnsErr && ticks < 15 * 60, t, r);
	if (stopped())
		return;
	memset(s, 0, sizeof s);
	r = dnr(4L, (long)PEER, s);
	check(r == 0 && !strcmp(s, "192.168.68.1"), "AddrToStr", r);
	r = dnr(7L, "peer.testbed", &hi, (void *)pas_result, (char *)&done);
	check(r == noAnsErr, "HInfo: noAnsErr (not supported)", r);
	r = dnr(8L, "peer.testbed", &hi, (void *)pas_result, (char *)&done);
	check(r == noAnsErr, "MXInfo: noAnsErr (not supported)", r);
	r = dnr(5L, (void *)pas_result, (char *)&done);
	check(r == noAnsErr, "EnumCache: noAnsErr (no cache)", r);
}

/* ---- .IPP: open by name, and a Close must not close it for the others ------------------------ */
static int dce_open(void)
{
	void ***utable = *(void ****)0x11C;
	unsigned char **dce = (unsigned char **)utable[~ipp];
	return dce && *dce && ((*dce)[5] & 0x20);               /* dCtlFlags: dOpened */
}

static void test_driver(void)
{
	unsigned char pb[104], p[16];
	short r, first;
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, ".IPP");
	r = mac_pb(pb, TRAP_OPEN);
	ipp = r == 0 ? *(short *)(pb + 24) : 0;
	check(r == 0 && ipp < 0, "open .IPP", r ? r : ipp);
	if (!ipp)
		return;
	check(dce_open(), "the device control entry says open (dOpened)", 0);
	memset(pb, 0, sizeof pb);
	r = ctl(pb, 15, 0);                                     /* ipctlGetAddr */
	check(r == 0 && *(u32 *)(pb + 28) == MAC_IP, "ipctlGetAddr: 192.168.68.2", r);
	/* a program quitting closes the driver: MacTCP refuses, and so must Riptide */
	first = ipp;
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = ipp;
	r = mac_pb(pb, TRAP_CLOSE);
	check(r == -24, "Close .IPP is refused (closErr)", r);
	check(dce_open(), "still open after the Close", 0);
	memset(pb, 0, sizeof pb);
	r = ctl(pb, 15, 0);
	check(r == 0, "the old reference number still works (not notOpenErr, -28)", r);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, ".IPP");
	r = mac_pb(pb, TRAP_OPEN);
	check(r == 0 && *(short *)(pb + 24) == first, "open .IPP again: the same reference number", r);
}

/* ---- the browser ----------------------------------------------------------------------------- */
static void **big_h;

static void test_three(void)
{
	static const char *const names[3] = { "page", "big", "slow" }, *const paths[3] = { "/", "/big", "/slow" };
	unsigned char pb[104];
	char t[160];
	int k;
	short r;
	for (k = 0; k < 3; k++) {
		struct conn *c = &conns[k];
		c->name = names[k];
		c->path = paths[k];
		c->nocopy = k == 0;
		c->chain = 0;
		c->in = small_in[k];
		c->inlen = sizeof small_in[k];
		if (k == 1 && big_h) {
			c->in = *big_h;                         /* a receive buffer in a locked handle */
			c->inlen = 4096;
		}
		reset_conn(c);
		r = tcp_create(c);
		rt_snprintf(t, sizeof t, "%s: TCPCreate", c->name);
		check(r == 0 && c->stream, t, r);
		if (r)
			return;
	}
	for (k = 0; k < 3; k++)
		open_start(&conns[k], 80);                      /* three opens at once */
	for (k = 0; k < 3; k++) {
		r = wait_call(&conns[k].c, 10 * 60);
		rt_snprintf(t, sizeof t, "%s: TCPActiveOpen 192.168.68.1:80, asynchronous", conns[k].name);
		check(r == 0 && *(unsigned short *)(conns[k].c.pb + 46) != 0, t, r);
		if (r)
			return;
	}
	r = status(&conns[0], pb);
	check(r == 0 && pb[52] == 8 && *(u32 *)(pb + 38) == PEER && *(unsigned short *)(pb + 42) == 80 &&
	      *(u32 *)(pb + 44) == MAC_IP, "TCPStatus: established (state at 52), the two ends", r ? r : pb[52]);
	/* the slow one: a receive before the request times out; then receives issued again from the
	   completion routine, with no timeout */
	rcv_start(&conns[2], 2);
	r = wait_call(&conns[2].c, 10 * 60);
	check(r == commandTimeout, "slow: TCPRcv with a 2 s timeout and no data: commandTimeout", r);
	if (stopped())
		return;
	for (k = 0; k < 3; k++) {
		r = send_get(&conns[k]);
		rt_snprintf(t, sizeof t, "%s: TCPSend GET %s, a WDS in three pieces", conns[k].name, conns[k].path);
		check(r == 0, t, r);
	}
	conns[2].chain = 1;
	for (k = 0; k < 3; k++)
		rcv_start(&conns[k], k == 2 ? 0 : 10);
	read_all(3, 60 * 60);
	if (stopped())
		return;
	for (k = 0; k < 3; k++) {
		struct conn *c = &conns[k];
		rt_snprintf(t, sizeof t, "%s: read to connectionClosing, %lu bytes (%s)", c->name, (unsigned long)c->got,
		            k == 0 ? "TCPNoCopyRcv" : k == 2 ? "TCPRcv from the completion routine" : "TCPRcv, locked handle");
		check(c->eof && !c->failed, t, c->eof ? 0 : 1);
	}
	check(find_str(conns[0].body, "<h1>It works</h1>") != 0, "page: the page came", (long)conns[0].bodylen);
	check(conns[1].bodylen == 262144 && conns[1].big_pos == 262144 && !conns[1].lines_bad,
	      "big: 262144 bytes, every line as sent", conns[1].lines_bad);
	check(!strcmp(conns[2].body, conns[0].body), "slow: the page, in pieces", (long)conns[2].bodylen);
	r = status(&conns[0], pb);
	check(r == 0 && pb[52] == 14, "TCPStatus after the server's FIN: Close Wait", r ? r : pb[52]);
	for (k = 0; k < 3; k++) {
		rt_snprintf(t, sizeof t, "%s: TCPClosing notification", conns[k].name);
		check(conns[k].st.closing == 1, t, conns[k].st.closing);
	}
	for (k = 0; k < 3; k++) {
		conns[k].chain = 0;
		memset(conns[k].c.pb + 32, 0, 72);
		start(&conns[k].c, TCPClose, conns[k].stream);
	}
	for (k = 0; k < 3; k++) {
		u32 t0 = TickCount();
		r = wait_call(&conns[k].c, 600);
		while (!conns[k].st.terminate && TickCount() - t0 < 300 && !stopped())
			pump(1);
		rt_snprintf(t, sizeof t, "%s: TCPClose, then TCPTerminate (TCPULPClose) once", conns[k].name);
		check(r == 0 && conns[k].st.terminate == 1 && conns[k].st.reason == TCPULPClose, t,
		      r ? r : conns[k].st.reason);
	}
	r = status(&conns[0], pb);
	check(pb[52] == 0, "TCPStatus after the connection ended: Closed", r ? r : pb[52]);
}

/* what some programs do instead of waiting: TCPStatus polled for amtUnreadData, then TCPRcv of
   exactly that much; and a synchronous TCPStatus inside the ASR */
static void test_poll(void)
{
	struct conn *c = &conns[4];
	unsigned char pb[104];
	char t[160];
	short r;
	u32 t0;
	int polls = 0;
	c->name = "poll";
	c->path = "/";
	c->nocopy = c->chain = 0;
	c->in = small_in[3];
	c->inlen = sizeof small_in[3];
	reset_conn(c);
	c->st.status_in_asr = 1;
	r = tcp_create(c);
	check(r == 0, "poll: TCPCreate", r);
	if (r)
		return;
	open_start(c, 80);
	r = wait_call(&c->c, 600);
	check(r == 0, "poll: TCPActiveOpen", r);
	if (r)
		return;
	send_get(c);
	t0 = TickCount();
	while (!c->eof && TickCount() - t0 < 20 * 60 && !stopped()) {
		u32 unread;
		r = status(c, pb);
		polls++;
		unread = *(unsigned short *)(pb + 60);
		if (unread) {
			memset(pb, 0, sizeof pb);
			*(char **)(pb + 36) = c->in;
			*(unsigned short *)(pb + 40) = (unsigned short)(unread < c->inlen ? unread : c->inlen);
			r = ctl(pb, TCPRcv, c->stream);
			if (r == 0)
				take(c, c->in, *(unsigned short *)(pb + 40));
		} else if (pb[52] == 14 || pb[52] == 0) {
			c->eof = 1;                     /* Close Wait and nothing unread: all has come */
		} else {
			pump(2);
		}
	}
	rt_snprintf(t, sizeof t, "poll: TCPStatus %d times, TCPRcv of amtUnreadData, the page", polls);
	check(c->eof && find_str(c->body, "It works") != 0, t, (long)c->got);
	check(c->st.status_done && c->st.status_r == 0, "poll: a synchronous TCPStatus inside the ASR came back",
	      c->st.status_done ? c->st.status_r : NOT_DONE);
	memset(pb, 0, sizeof pb);
	ctl(pb, TCPClose, c->stream);
}

/* a stream opened again after its connection ended (browsers keep their streams) */
static void test_reuse(void)
{
	struct conn *c = &conns[0];
	short r;
	reset_conn(c);
	c->nocopy = 0;
	open_start(c, 80);
	r = wait_call(&c->c, 600);
	check(r == 0, "page: the same stream opened again", r);
	if (r)
		return;
	r = send_get(c);
	rcv_start(c, 10);
	read_all(1, 20 * 60);
	check(r == 0 && c->eof && find_str(c->body, "It works") != 0, "page: fetched again on it", (long)c->got);
	memset(c->c.pb + 32, 0, 72);
	start(&c->c, TCPClose, c->stream);
	wait_call(&c->c, 600);
}

static void test_reset_abort(void)
{
	struct conn *c = &conns[3];
	unsigned char pb[104];
	short r;
	u32 t0;
	c->name = "reset";
	c->path = "/reset";
	c->expect_error = 1;
	c->nocopy = c->chain = 0;
	c->in = small_in[3];
	c->inlen = sizeof small_in[3];
	reset_conn(c);
	r = tcp_create(c);
	check(r == 0, "reset: TCPCreate", r);
	if (r)
		return;
	open_start(c, 80);
	r = wait_call(&c->c, 600);
	if (r == 0) {
		send_get(c);
		rcv_start(c, 10);
		read_all(4, 20 * 60);
		t0 = TickCount();
		while (!c->st.terminate && TickCount() - t0 < 300 && !stopped())
			pump(1);
		/* the reset may come before the receive is made (connectionDoesntExist) or while it waits
		   (connectionTerminated); either way TCPTerminate tells why */
		check(c->failed && (c->c.result == connectionTerminated || c->c.result == connectionDoesntExist) &&
		      c->st.terminate == 1 && c->st.reason == TCPRemoteAbort,
		      "reset: the receive ends, TCPTerminate (TCPRemoteAbort)", c->c.result);
	} else {
		check(0, "reset: TCPActiveOpen", r);
	}
	if (stopped())
		return;
	c->name = "abort";
	c->failed = 0;
	open_start(c, 7);
	r = wait_call(&c->c, 600);
	check(r == 0, "abort: TCPActiveOpen 192.168.68.1:7 on the same stream", r);
	memset(pb, 0, sizeof pb);
	r = ctl(pb, TCPAbort, c->stream);
	check(r == 0 && c->st.terminate == 1 && c->st.reason == TCPULPAbort, "abort: TCPAbort, TCPTerminate (TCPULPAbort)",
	      r ? r : c->st.reason);
	memset(pb, 0, sizeof pb);
	*(char **)(pb + 36) = c->in;
	*(unsigned short *)(pb + 40) = 16;
	r = ctl(pb, TCPRcv, c->stream);
	check(r == connectionDoesntExist, "abort: TCPRcv afterwards: connectionDoesntExist", r);
	open_start(c, 81);                                      /* nothing listens there */
	r = wait_call(&c->c, 600);
	check(r == openFailed, "TCPActiveOpen to a closed port: openFailed", r);
}

static void test_global(void)
{
	unsigned char pb[104];
	short r;
	memset(pb, 0, sizeof pb);
	r = ctl(pb, TCPGlobalInfo, 0);
	check(r == 0 && *(void **)(pb + 32) && *(void **)(pb + 40) && *(unsigned short *)(pb + 48) > 0,
	      "TCPGlobalInfo: parameters, statistics, the stream table", r);
}

/* ---- connections the server closes while the program idles, in front and behind ------------- */
/* the Process Manager (_OSDispatch): to put the Finder in front while a connection idles, so that
   its close is noticed while another program is current */
short pm_current(void *psn);
short pm_next(void *psn);
short pm_info(const void *psn, void *info);
short pm_front(const void *psn);
__asm__(".globl pm_current\npm_current:\n\tmove.l %d2,-(%sp)\n\tmove.l 8(%sp),%a0\n\tclr.w -(%sp)\n\tmove.l %a0,-(%sp)\n"
        "\tmove.w #0x37,-(%sp)\n\t.short 0xA88F\n\tmove.w (%sp)+,%d0\n\text.l %d0\n\tmove.l (%sp)+,%d2\n\trts\n"
        ".globl pm_next\npm_next:\n\tmove.l %d2,-(%sp)\n\tmove.l 8(%sp),%a0\n\tclr.w -(%sp)\n\tmove.l %a0,-(%sp)\n"
        "\tmove.w #0x38,-(%sp)\n\t.short 0xA88F\n\tmove.w (%sp)+,%d0\n\text.l %d0\n\tmove.l (%sp)+,%d2\n\trts\n"
        ".globl pm_info\npm_info:\n\tmove.l %d2,-(%sp)\n\tmove.l 8(%sp),%a0\n\tmove.l 12(%sp),%a1\n\tclr.w -(%sp)\n"
        "\tmove.l %a0,-(%sp)\n\tmove.l %a1,-(%sp)\n\tmove.w #0x3A,-(%sp)\n\t.short 0xA88F\n\tmove.w (%sp)+,%d0\n"
        "\text.l %d0\n\tmove.l (%sp)+,%d2\n\trts\n"
        ".globl pm_front\npm_front:\n\tmove.l %d2,-(%sp)\n\tmove.l 8(%sp),%a0\n\tclr.w -(%sp)\n\tmove.l %a0,-(%sp)\n"
        "\tmove.w #0x3B,-(%sp)\n\t.short 0xA88F\n\tmove.w (%sp)+,%d0\n\text.l %d0\n\tmove.l (%sp)+,%d2\n\trts\n");

static int finder_psn(unsigned char *psn)
{
	unsigned char info[60];
	memset(psn, 0, 8);
	while (pm_next(psn) == 0) {
		memset(info, 0, sizeof info);
		*(u32 *)info = sizeof info;
		if (pm_info(psn, info) == 0 && *(u32 *)(info + 20) == MAC_TYPE('M', 'A', 'C', 'S'))
			return 1;
	}
	return 0;
}

/* GET /idle (the page, kept alive), read the page, then idle in WaitNextEvent until the server
   closes after 30 s: the TCPClosing notification must come, with this program's A5, also when the
   Finder is in front; then TCPClose and TCPTerminate */
static void idle_one(struct conn *c, int behind)
{
	unsigned char me[8], finder[8], pb[104];
	char t[160];
	short r;
	u32 t0;
	int front_back = 0;
	reset_conn(c);
	c->path = "/idle";
	c->nocopy = c->chain = 0;
	c->expect_error = 0;
	open_start(c, 80);
	r = wait_call(&c->c, 600);
	if (r) {
		check(0, "idle: TCPActiveOpen", r);
		return;
	}
	send_get(c);
	t0 = TickCount();
	while (!find_str(c->body, "</html>") && TickCount() - t0 < 10 * 60 && !stopped()) {
		rcv_start(c, 5);
		r = wait_call(&c->c, 6 * 60);
		if (r)
			break;
		take(c, c->in, *(unsigned short *)(c->c.pb + 40));
	}
	if (behind && pm_current(me) == 0 && finder_psn(finder) && pm_front(finder) == 0)
		front_back = 1;
	t0 = TickCount();
	while (!c->st.closing && TickCount() - t0 < 45 * 60 && !stopped())
		pump(30);                               /* idle: nothing asked of the stream */
	if (front_back)
		pm_front(me);
	rt_snprintf(t, sizeof t, "idle: the server's close after 30 s, noticed %s: TCPClosing, with our A5",
	            behind ? (front_back ? "with the Finder in front" : "(the Finder could not be put in front)") : "in front");
	check(find_str(c->body, "It works") && c->st.closing == 1 && !c->st.a5_bad, t, c->st.closing);
	memset(pb, 0, sizeof pb);
	*(char **)(pb + 36) = c->in;
	*(unsigned short *)(pb + 40) = 16;
	r = ctl(pb, TCPRcv, c->stream);
	check(r == connectionClosing, "idle: TCPRcv afterwards: connectionClosing", r);
	memset(pb, 0, sizeof pb);
	r = ctl(pb, TCPClose, c->stream);
	t0 = TickCount();
	while (!c->st.terminate && TickCount() - t0 < 300 && !stopped())
		pump(1);
	check(r == 0 && c->st.terminate == 1 && c->st.reason == TCPULPClose, "idle: TCPClose, TCPTerminate (TCPULPClose)",
	      r ? r : c->st.reason);
}

static void test_idle(void)
{
	struct conn *c = &conns[3];
	c->name = "idle";
	c->in = small_in[3];
	c->inlen = sizeof small_in[3];
	idle_one(c, 0);
	if (!stopped())
		idle_one(c, 1);
}

/* every stream aborted and released, every call of ours finished, the resolver closed: nothing is
   left with the driver that points into this program */
static void cleanup(int report)
{
	unsigned char pb[104];
	char t[120];
	int k, left = 0;
	u32 t0;
	in_cleanup = 1;
	for (k = 0; k < 5; k++) {
		struct conn *c = &conns[k];
		short r;
		if (!c->stream)
			continue;
		memset(pb, 0, sizeof pb);
		ctl(pb, TCPAbort, c->stream);
		memset(pb, 0, sizeof pb);
		r = ctl(pb, TCPRelease, c->stream);
		if (report) {
			rt_snprintf(t, sizeof t, "%s: TCPRelease gives the buffer back", c->name);
			check(r == 0 && *(char **)(pb + 32) == c->rbuf, t, r);
		}
		c->stream = 0;
	}
	/* the released streams' calls come back with connectionTerminated; wait for each */
	t0 = TickCount();
	for (;;) {
		left = 0;
		for (k = 0; k < 5; k++) {
			struct conn *c = &conns[k];
			/* a call started and not back (a chained receive reports through chain_done) */
			if (*(short *)(c->c.pb + 26) && !c->c.done && !c->chain_done)
				left++;
		}
		if (*(short *)(one.pb + 26) && !one.done)
			left++;
		if (!left || TickCount() - t0 > 5 * 60)
			break;
		pump(1);
	}
	if (dnr) {
		dnr(2L);                                        /* CLOSERESOLVER: its lookups are withdrawn */
		dnr = 0;
	}
	if (left) {
		rt_snprintf(t, sizeof t, "%d calls never came back", left);
		check(0, t, left);
	}
	in_cleanup = 0;
}

static void run_tests(void)
{
	int k, a5bad = 0, regsbad = 0, other = 0, data = 0;
	char t[160];
	running = 1;
	stop_asked = 0;
	passed = failed = completions = order_bad = stack_bad = 0;
	asr_reentered = 0;
	for (k = 0; k < 5; k++) {
		memset(&conns[k], 0, sizeof conns[k]);
		conns[k].name = "stream";
	}
	memset(&one, 0, sizeof one);
	dim_menus();
	out("---- Browser test ----");
	test_driver();
	if (ipp && !stopped())
		test_resolver();
	if (ipp && !stopped())
		test_three();
	if (ipp && !stopped())
		test_poll();
	if (ipp && !stopped())
		test_reuse();
	if (ipp && !stopped())
		test_reset_abort();
	if (ipp && !stopped())
		test_global();
	if (ipp && !stopped())
		test_idle();
	if (leave_open) {
		running = 0;
		return;                                 /* on purpose: the driver must clean up after us */
	}
	if (ipp)
		cleanup(!stop_asked);
	if (stop_asked) {
		out("Stopped: every stream aborted and released, the resolver closed.");
	} else {
		for (k = 0; k < 5; k++) {
			a5bad |= conns[k].st.a5_bad;
			regsbad |= conns[k].st.regs_bad;
			other += conns[k].st.other;
			data += conns[k].st.data;
		}
		check(!a5bad, "the notification routines ran with the program's A5", a5bad);
		check(!regsbad, "... with D0 = the event and A2 = the user data", regsbad);
		check(!asr_reentered && !other, "... never re-entered, no unexpected events", other);
		rt_snprintf(t, sizeof t, "every completion routine saw ioResult set first (%d completions)", completions);
		check(!order_bad && completions > 0, t, order_bad);
		check(!stack_bad, "... and the parameter block both in A0 and on the stack", stack_bad);
		rt_snprintf(t, sizeof t, "done: %d passed, %d failed (%d data arrival notifications)", passed, failed, data);
		out(t);
	}
	running = 0;
	stop_asked = 0;
	dim_menus();
}

/* ---- start ---- */
static void menus_init(void)
{
	unsigned char pb[120];
	menus[0] = NewMenu(APPLE_ID, mac_pstr(pb, "\x14"));
	AppendMenu(menus[0], mac_pstr(pb, "About Browser Test...;(-"));
	AddResMenu(menus[0], MAC_TYPE('D', 'R', 'V', 'R'));
	InsertMenu(menus[0], 0);
	menus[1] = NewMenu(FILE_ID, mac_pstr(pb, "File"));
	AppendMenu(menus[1], mac_pstr(pb, "Run Tests/R;Stop/.;(-;Close/W;Quit/Q;(-;Quit Leaving Streams Open/K"));
	InsertMenu(menus[1], 0);
	menus[2] = NewMenu(EDIT_ID, mac_pstr(pb, "Edit"));
	AppendMenu(menus[2], mac_pstr(pb, "(Undo/Z;(-;(Cut/X;(Copy/C;(Paste/V;(Clear"));
	InsertMenu(menus[2], 0);
	DrawMenuBar();
}

int main(void)
{
	Rect b = { 40, 4, 440, 564 }, s;
	unsigned char p[64];
	EventRecord ev;
	__asm__ volatile("move.l %%a5,%0" : "=g"(app_a5));
	menus_init();
	win = NewWindow(0, &b, mac_pstr(p, "Browser test"), 1, documentProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(monaco);
	TextSize(9);
	s = *PORT_RECT(win);
	s.left = s.right - SBW + 1;
	s.top -= 1;
	s.bottom = s.bottom - SBW + 2;
	s.right += 1;
	sbar = NewControl(win, &s, mac_pstr(p, ""), 1, 0, 0, 0, 16, 0);   /* scrollBarProc */
	big_h = mac_newhandle(4096);
	if (big_h)
		mac_hlock(big_h);
	running = 2;                                    /* the tests run at the start */
	while (!quit_asked) {
		if (running == 2)
			run_tests();
		if (quit_asked)
			break;
		dim_menus();
		if (WaitNextEvent(everyEvent, &ev, 20, NULL))
			event(&ev);
	}
	if (leave_open)
		out("Quitting with the streams left open: Riptide must release them.");
	else if (ipp)
		cleanup(0);                             /* (a quit between runs: nothing is open) */
	return 0;
}
