/* tests/mac/restart_test.c - Riptide on the classic Mac OS: connections held open while Riptide's
 * network restarts (RiptidePrefs' Save or Apply of a card, address, MTU or IPv6 change; docs/MAC.md,
 * "Restarting the network while the Mac runs"). Against the Aurora68 testbed's peer: a MacTCP
 * stream (.IPP) to the TCP echo server at 192.168.68.1 port 7, and a .IPP6 stream to [fd68::1] port
 * 7007 (pinet's PINET_FORWARD to an echo server on the host). Every two seconds each sends a line
 * and reads its echo; what each stream sees is written to the window and, built with AURORA68_LOG,
 * to the Pi's log as "[MAC] Restart test: ...": its local address after it connects, every tenth
 * echo, every echo that failed, every notification (TCPTerminate with its reason), and the results
 * of the calls that were waiting. A stream whose connection ended is opened again three seconds
 * later, so the log also shows when a new connection works again. Every call is asynchronous with
 * no completion routine (ioResult polled from the event loop), so the program keeps taking events
 * and RiptidePrefs runs meanwhile. Command-Q aborts and releases both streams and quits.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "toolbox.h"
#include "IPP6.h"

typedef unsigned long u32;
int rt_snprintf(char *buf, unsigned long size, const char *fmt, ...);

#define PEER4 0xC0A84401UL              /* 192.168.68.1 */
static const unsigned char PEER6[16] = { 0xfd, 0x68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static void *win;
static int quit_asked;

/* ---- the window: the last lines that fit ------------------------------------------------------ */
#define MAXL 40
#define LINEW 90
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
	rt_snprintf(t, sizeof t, "Restart test: %s", s);
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

short pb_async(void *pb);
short pb_sync(void *pb);
__asm__(".globl pb_async\npb_async:\n\tmove.l 4(%sp),%a0\n\t.short 0xA404\n\text.l %d0\n\trts\n"
        ".globl pb_sync\npb_sync:\n\tmove.l 4(%sp),%a0\n\t.short 0xA004\n\text.l %d0\n\trts\n");

/* ---- a stream ----------------------------------------------------------------------------------
   The notification routine (pascal: stream, eventCode, userDataPtr, terminReason, icmpMsg, 16 bytes
   of arguments) only notes the event in the stream's record, which userDataPtr points to: a word
   counting the notifications, the last event and the last terminate reason. */
struct note { volatile short count, event, reason, terminated; };
void pas_note(void);
__asm__(".globl pas_note\npas_note:\n"
        "\tmove.l 10(%sp),%a0\n"                  /* userDataPtr: the record */
        "\taddq.w #1,(%a0)\n"
        "\tmove.w 14(%sp),2(%a0)\n"               /* eventCode */
        "\tcmp.w #3,14(%sp)\n"                    /* TCPTerminate */
        "\tbne.s 1f\n"
        "\tmove.w 8(%sp),4(%a0)\n"                /* terminReason */
        "\tmove.w #1,6(%a0)\n"
        "1:\tmove.l (%sp)+,%a1\n"
        "\tlea 16(%sp),%sp\n"
        "\tjmp (%a1)\n");

enum { S_IDLE, S_OPENING, S_OPEN, S_SENDING, S_RECEIVING };
struct strm {
	const char *name;
	int six;                        /* .IPP6 */
	short ref;
	u32 stream;
	int state;
	u32 next_tick;
	long sent, echoed, failed, opens;
	struct note note;
	short seen_notes;
	unsigned char pb[132];
	unsigned char wds[8];
	char msg[24], in[64];
	char rbuf[8192];
};
static struct strm s4 = { "MacTCP 192.168.68.1:7", 0 }, s6 = { ".IPP6 [fd68::1]:7007", 1 };

static const char *reason_name(int r)
{
	switch (r) {
	case 2: return "TCPRemoteAbort";
	case 3: return "TCPNetworkFailure";
	case 4: return "TCPSecPrecMismatch";
	case 5: return "TCPULPTimeoutTerminate";
	case 6: return "TCPULPAbort";
	case 7: return "TCPULPClose";
	case 8: return "TCPServiceError";
	}
	return "?";
}

static void say(struct strm *s, const char *what)
{
	char t[160];
	rt_snprintf(t, sizeof t, "%s: %s", s->name, what);
	out(t);
}

static short call(struct strm *s, short cs, int async)
{
	*(void **)(s->pb + 12) = NULL;                              /* no completion routine */
	*(short *)(s->pb + 24) = s->ref;
	*(short *)(s->pb + 26) = cs;
	*(u32 *)(s->pb + 28) = s->stream;
	return async ? pb_async(s->pb) : pb_sync(s->pb);
}
#define RESULT(s) (*(volatile short *)((s)->pb + 16))

static int create(struct strm *s)
{
	short r;
	memset(s->pb, 0, sizeof s->pb);
	*(char **)(s->pb + 32) = s->rbuf;
	*(u32 *)(s->pb + 36) = sizeof s->rbuf;
	*(void **)(s->pb + 40) = (void *)pas_note;
	*(void **)(s->pb + 44) = &s->note;
	s->stream = 0;
	r = call(s, 30, 0);                                         /* TCPCreate */
	s->stream = *(u32 *)(s->pb + 28);
	if (r) {
		char t[64];
		rt_snprintf(t, sizeof t, "TCPCreate: %d", r);
		say(s, t);
		return -1;
	}
	return 0;
}

static void open_start(struct strm *s)
{
	memset(s->pb + 32, 0, sizeof s->pb - 32);
	s->pb[35] = 10;                                             /* commandTimeoutValue, seconds */
	if (s->six) {
		memcpy(s->pb + 36, PEER6, 16);                          /* remoteAddr */
		*(unsigned short *)(s->pb + 52) = 7007;
	} else {
		*(u32 *)(s->pb + 36) = PEER4;                           /* remoteHost */
		*(unsigned short *)(s->pb + 40) = 7;
	}
	s->note.terminated = 0;
	s->opens++;
	call(s, 32, 1);                                             /* TCPActiveOpen */
	s->state = S_OPENING;
}

static void show_local(struct strm *s)
{
	unsigned char pb[132];
	char t[120];
	memcpy(pb, s->pb, 32);
	memset(pb + 32, 0, sizeof pb - 32);
	*(void **)(pb + 12) = NULL;
	*(short *)(pb + 26) = 40;                                   /* TCPStatus */
	if (pb_sync(pb) != 0)
		return;
	if (s->six) {
		const unsigned char *a = pb + 58;
		if (a[10] == 0xff && a[11] == 0xff && !a[0])
			rt_snprintf(t, sizeof t, "connected (open %ld), local %u.%u.%u.%u port %u", s->opens, a[12], a[13], a[14],
			            a[15], *(unsigned short *)(pb + 74));
		else
			rt_snprintf(t, sizeof t, "connected (open %ld), local %x:%x:%x:%x:%x:%x:%x:%x port %u", s->opens,
			            a[0] << 8 | a[1], a[2] << 8 | a[3], a[4] << 8 | a[5], a[6] << 8 | a[7], a[8] << 8 | a[9],
			            a[10] << 8 | a[11], a[12] << 8 | a[13], a[14] << 8 | a[15], *(unsigned short *)(pb + 74));
	} else {
		u32 l = *(u32 *)(pb + 44);
		rt_snprintf(t, sizeof t, "connected (open %ld), local %lu.%lu.%lu.%lu port %u", s->opens, l >> 24, (l >> 16) & 255,
		            (l >> 8) & 255, l & 255, *(unsigned short *)(pb + 48));
	}
	say(s, t);
}

/* a notification or a call that ended the connection: the stream is as TCPCreate left it */
static void ended(struct strm *s, const char *how, short r)
{
	char t[160];
	rt_snprintf(t, sizeof t, "%s (%d); %ld echoes so far, %ld failed; opening again in 3 s", how, r, s->echoed,
	            s->failed);
	say(s, t);
	s->state = S_IDLE;
	s->next_tick = TickCount() + 180;
}

static void step(struct strm *s)
{
	char t[160];
	short r;
	if (!s->ref)
		return;
	if (s->note.count != s->seen_notes) {
		s->seen_notes = s->note.count;
		if (s->note.event == 3) {
			rt_snprintf(t, sizeof t, "TCPTerminate, reason %d (%s)", s->note.reason, reason_name(s->note.reason));
			say(s, t);
		} else if (s->note.event != 4) {                        /* TCPDataArrival is routine */
			rt_snprintf(t, sizeof t, "notification %d", s->note.event);
			say(s, t);
		}
	}
	switch (s->state) {
	case S_IDLE:
		if ((long)(TickCount() - s->next_tick) >= 0)
			open_start(s);
		break;
	case S_OPENING:
		if ((r = RESULT(s)) > 0)
			break;
		if (r == 0) {
			show_local(s);
			s->state = S_OPEN;
			s->next_tick = TickCount();
		} else {
			rt_snprintf(t, sizeof t, "TCPActiveOpen failed (%d); again in 3 s", r);
			say(s, t);
			s->state = S_IDLE;
			s->next_tick = TickCount() + 180;
		}
		break;
	case S_OPEN:
		if ((long)(TickCount() - s->next_tick) < 0)
			break;
		s->next_tick = TickCount() + 120;
		rt_snprintf(s->msg, sizeof s->msg, "echo %ld\r\n", ++s->sent);
		{
			const char *mp = s->msg;
			*(unsigned short *)s->wds = (unsigned short)strlen(s->msg);
			memcpy(s->wds + 2, &mp, 4);
			*(unsigned short *)(s->wds + 6) = 0;
		}
		memset(s->pb + 32, 0, sizeof s->pb - 32);
		s->pb[35] = 1;                                          /* push */
		*(unsigned char **)(s->pb + 38) = s->wds;
		call(s, 34, 1);                                         /* TCPSend */
		s->state = S_SENDING;
		break;
	case S_SENDING:
		if ((r = RESULT(s)) > 0)
			break;
		if (r) {
			s->failed++;
			ended(s, "TCPSend ended", r);
			break;
		}
		memset(s->pb + 32, 0, sizeof s->pb - 32);
		s->pb[32] = 20;                                         /* commandTimeoutValue, seconds */
		*(char **)(s->pb + 36) = s->in;
		*(unsigned short *)(s->pb + 40) = sizeof s->in;
		call(s, 37, 1);                                         /* TCPRcv */
		s->state = S_RECEIVING;
		break;
	case S_RECEIVING:
		if ((r = RESULT(s)) > 0)
			break;
		if (r == 0) {
			unsigned short n = *(unsigned short *)(s->pb + 40);
			if (n == strlen(s->msg) && !memcmp(s->in, s->msg, n)) {
				if (++s->echoed % 10 == 0) {
					rt_snprintf(t, sizeof t, "%ld echoes", s->echoed);
					say(s, t);
				}
			} else {
				s->failed++;
				rt_snprintf(t, sizeof t, "echo %ld wrong (%u bytes)", s->sent, n);
				say(s, t);
			}
			s->state = S_OPEN;
		} else if (r == -23015) {                               /* commandTimeout: the echo is late */
			s->failed++;
			rt_snprintf(t, sizeof t, "echo %ld: no answer in 20 s", s->sent);
			say(s, t);
			s->state = S_OPEN;
		} else {
			s->failed++;
			ended(s, "TCPRcv ended", r);
		}
		break;
	}
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

static short open_driver(const char *name)
{
	unsigned char pb[80], p[16];
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = mac_pstr(p, name);
	if (mac_pb(pb, TRAP_OPEN) != 0)
		return 0;
	return *(short *)(pb + 24);
}

static void finish(struct strm *s)
{
	if (!s->ref || !s->stream)
		return;
	memset(s->pb + 32, 0, sizeof s->pb - 32);
	call(s, 39, 0);                                             /* TCPAbort */
	memset(s->pb + 32, 0, sizeof s->pb - 32);
	call(s, 42, 0);                                             /* TCPRelease */
}

int main(void)
{
	Rect b = { 40, 4, 300, 500 };
	unsigned char p[64];
	char t[100];
	win = NewWindow(0, &b, mac_pstr(p, "Restart test"), 1, documentProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(monaco);
	TextSize(9);
	s4.ref = open_driver(".IPP");
	s6.ref = open_driver(".IPP6");
	rt_snprintf(t, sizeof t, "start: .IPP %d, .IPP6 %d", s4.ref, s6.ref);
	out(t);
	if (s4.ref && create(&s4) < 0)
		s4.ref = 0;
	if (s6.ref && create(&s6) < 0)
		s6.ref = 0;
	while (!quit_asked) {
		step(&s4);
		step(&s6);
		pump(6);
	}
	finish(&s4);
	finish(&s6);
	out("quit: streams aborted and released");
	return 0;
}
