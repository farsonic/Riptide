/*
 * Riptide - the Riptide Command Terminal on the classic Mac OS: the AmigaOS calls the network tools make
 * (src/amiga/cmd, unchanged), beyond those RiptidePrefs already has (src/mac/mui/mm_os.c: memory,
 * lists, ports, files by path). Output() and Input() are the terminal's console; the timer.device
 * the tools read the EClock from is the Mac's Microseconds; Ctrl-C is Command-period;
 * bsdsocket.library and riptide.library answer when Riptide's .IPP driver gives its calls for
 * programs (term_bsd.c).
 *
 * mm_os.c is compiled for the terminal with its console-free versions renamed (Makefile.mac:
 * Read and Write become mm_file_Read and mm_file_Write, and so on): the calls here use them for
 * real files.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "term.h"

LONG mm_file_Read(BPTR fh, APTR buf, LONG n);
LONG mm_file_Write(BPTR fh, const APTR buf, LONG n);
struct Library *mm_OpenLibrary(CONST_STRPTR name, ULONG version);

/* what mm_os.c expects from RiptidePrefs' other files */
struct Screen mm_screen;
ULONG mm_tag(const struct TagItem *t, ULONG tag, ULONG def)
{
	for (; t && t->ti_Tag != TAG_DONE; t++)
		if (t->ti_Tag == tag)
			return t->ti_Data;
	return def;
}

/* ---- the console ---- */
BPTR Output(void) { return TERM_OUT; }
BPTR Input(void) { return TERM_IN; }

LONG Write(BPTR fh, const APTR buf, LONG n)
{
	if (fh == TERM_OUT || fh == TERM_IN) {
		term_out(buf, n);
		return n;
	}
	if (!fh)
		return n;                               /* NIL: */
	return mm_file_Write(fh, buf, n);
}

LONG Read(BPTR fh, APTR buf, LONG n)
{
	if (fh == TERM_IN || fh == TERM_OUT)
		return term_in(buf, n);
	if (!fh)
		return 0;
	return mm_file_Read(fh, buf, n);
}

LONG PutStr(CONST_STRPTR s)
{
	term_out((const char *)s, (LONG)strlen((const char *)s));
	return 0;
}

STRPTR GetArgStr(void) { return (STRPTR)term_args; }
LONG IsInteractive(BPTR fh) { return fh == TERM_IN || fh == TERM_OUT ? DOSTRUE : 0; }
LONG SetMode(BPTR fh, LONG mode) { (void)fh; (void)mode; return DOSTRUE; }

/* timeout in microseconds */
LONG WaitForChar(BPTR fh, LONG timeout)
{
	ULONG t0 = TickCount(), ticks = (ULONG)timeout / 16667u;
	if (fh != TERM_IN)
		return DOSTRUE;
	for (;;) {
		if (term_in_ready())
			return DOSTRUE;
		if (TickCount() - t0 >= ticks || term_break_pending(NULL))
			return 0;
		term_yield(NULL);
	}
}

/* ---- signals: Ctrl-C is Command-period ---- */
ULONG CheckSignal(ULONG mask)
{
	term_poll();
	if ((mask & SIGBREAKF_CTRL_C) && term_break_pending(NULL)) {
		term_break_take();
		return SIGBREAKF_CTRL_C;
	}
	return 0;
}

ULONG SetSignal(ULONG newsigs, ULONG mask)
{
	ULONG old = term_break_pending(NULL) ? SIGBREAKF_CTRL_C : 0;
	if ((mask & SIGBREAKF_CTRL_C) && !(newsigs & SIGBREAKF_CTRL_C))
		term_break_take();
	return old;
}

void Signal(struct Task *task, ULONG sigs)
{
	(void)task;
	(void)sigs;
}

/* 50ths of a second; the window stays alive meanwhile */
void Delay(LONG ticks)
{
	ULONG t0 = TickCount(), n = (ULONG)ticks * 60u / 50u;
	while (TickCount() - t0 < n)
		term_yield(NULL);
}

ULONG AvailMem(ULONG req)
{
	(void)req;
	return (ULONG)(*(volatile long *)0x130 - *(volatile long *)0x114);   /* ApplLimit - HeapEnd */
}

/* ---- dos, the parts the tools touch that have no Mac counterpart here ---- */
BPTR CurrentDir(BPTR lock) { (void)lock; return 0; }
LONG NameFromLock(BPTR lock, STRPTR buf, LONG len) { (void)lock; rt_strlcpy((char *)buf, ":", (u32)len); return DOSTRUE; }
LONG MatchFirst(CONST_STRPTR pat, struct AnchorPath *ap) { (void)pat; (void)ap; return 232; }   /* ERROR_NO_MORE_ENTRIES */
LONG MatchNext(struct AnchorPath *ap) { (void)ap; return 232; }
void MatchEnd(struct AnchorPath *ap) { (void)ap; }
LONG GetVar(CONST_STRPTR name, STRPTR buf, LONG size, ULONG flags) { (void)name; (void)buf; (void)size; (void)flags; return -1; }
LONG SystemTagList(CONST_STRPTR cmd, const struct TagItem *tags) { (void)cmd; (void)tags; return -1; }
LONG Fault(LONG code, CONST_STRPTR header, STRPTR buf, LONG len)
{
	rt_snprintf((char *)buf, (u32)len, "%s%serror %ld", header ? (const char *)header : "", header ? ": " : "", (long)code);
	return (LONG)strlen((char *)buf);
}

/* ---- libraries and timer.device ---- */
static struct Library socketlib, riptidelib;
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
	if (!strcmp((const char *)name, "bsdsocket.library") || !strcmp((const char *)name, "riptide.library")) {
		struct Library *l = name[0] == 'b' ? &socketlib : &riptidelib;
		if (!term_api_ok() && term_api_open() < 0)
			return NULL;
		/* riptide.library 1: the tools resolve and connect themselves (no RiptideConnectName) */
		l->lib_Version = name[0] == 'b' ? 4 : 1;
		return version <= l->lib_Version ? l : NULL;
	}
	return mm_OpenLibrary(name, version);
}

static struct Device timer_dev;
BYTE OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *io, ULONG flags)
{
	(void)unit;
	(void)flags;
	if (!io || strcmp((const char *)name, "timer.device"))
		return -1;
	io->io_Device = &timer_dev;
	return 0;
}

void CloseDevice(struct IORequest *io) { (void)io; }

APTR CreateIORequest(const struct MsgPort *port, ULONG size)
{
	struct IORequest *io = AllocVec(size, MEMF_ANY | MEMF_CLEAR);
	if (io) {
		io->io_Message.mn_ReplyPort = (struct MsgPort *)port;
		io->io_Message.mn_Length = (UWORD)size;
	}
	return io;
}

void DeleteIORequest(APTR io) { FreeVec(io); }

ULONG ReadEClock(struct EClockVal *ev)
{
	unsigned long long t = mac_us();
	ev->ev_hi = (ULONG)(t >> 32);
	ev->ev_lo = (ULONG)t;
	return 1000000u;
}

/* ---- the RIPTIDE port: cl_command()'s messages as RTMAC_PORT Control calls (as RiptidePrefs) ---- */
static struct MsgPort rt_port;
static short ipp_ref;
static int ipp_state;                     /* 0 not looked, 1 Riptide, -1 not there */

struct MsgPort *mm_riptide_port(void)
{
	if (!ipp_state) {
		UBYTE pb[80];
		unsigned char name[6] = { 4, '.', 'I', 'P', 'P', 0 };
		ipp_state = -1;
		memset(pb, 0, sizeof pb);
		*(unsigned char **)(pb + 18) = name;
		if (mac_pb(pb, TRAP_OPEN) == 0) {
			ipp_ref = *(short *)(pb + 24);
			memset(pb, 0, sizeof pb);
			*(short *)(pb + 24) = ipp_ref;
			*(short *)(pb + 26) = RTMAC_PING;
			if (mac_pb(pb, TRAP_CONTROL) == 0 && *(ULONG *)(pb + 28) == RTMAC_MAGIC)
				ipp_state = 1;
		}
	}
	return ipp_state > 0 ? &rt_port : NULL;
}

void mm_riptide_put(struct Message *msg)
{
	struct RiptideMsg *m = (struct RiptideMsg *)msg;
	struct rtmac_port_pb p;
	memset(&p, 0, sizeof p);
	p.ioCRefNum = ipp_ref;
	p.csCode = RTMAC_PORT;
	p.cmd = m->cmd;
	p.text = m->text;
	p.textlen = RIPTIDE_TEXTLEN;
	if (msg->mn_Length >= sizeof(*m) && m->reply && m->replysize > 1) {
		p.reply = (char *)m->reply;
		p.replysize = m->replysize;
	} else {
		p.reply = m->text;
		p.replysize = RIPTIDE_TEXTLEN;
	}
	if (mac_pb(&p, TRAP_CONTROL) != 0) {
		m->result = RT_EINVAL;
		rt_strlcpy(p.reply, "unknown command\n", p.replysize);
		p.replylen = (ULONG)strlen(p.reply);
	} else {
		m->result = (LONG)p.result;
	}
	if (msg->mn_Length >= sizeof(*m)) {
		m->replylen = p.replylen;
		m->flags = p.flags;
	}
	ReplyMsg(msg);
}
