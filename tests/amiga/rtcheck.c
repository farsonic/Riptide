/*
 * Riptide - rtcheck: a bsdsocket/riptide/usergroup.library client for a
 * running Riptide on real AmigaOS (or an emulator). It opens the
 * libraries with exec OpenLibrary() and runs the checks in rtcheck_body.h
 * through the real library vectors, using blocking calls where possible.
 * Exit code 0 when every check passed, 10 otherwise, 20 if bsdsocket.library
 * cannot be opened.
 *
 * USERSTARTUP=FILE [BLOCK=FILE] is for the installer and opens no library:
 * it takes every ";BEGIN Riptide" ... ";END Riptide" block out of FILE
 * (S:User-Startup) and, with BLOCK, puts a new one with BLOCK's lines at
 * the end, so Riptide starts after everything else. Exit code 0 or 20;
 * DONE=FILE is deleted when it worked (the installer checks for it).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_os.h"
#include "amiga_net.h"
#include "rt/rt_types.h"
#include "rt/riptide_port.h"

struct Library *SocketBase, *RiptideBase, *CompatBase, *UserGroupBase;

#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"
#include "gen/usergroup_calls.h"

static void rtc_logf(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(b, sizeof(b) - 1, fmt, ap);
	va_end(ap);
	PutStr((CONST_STRPTR)b);
	PutStr((CONST_STRPTR)"\n");
}

/* Send one configuration line to the RIPTIDE port; 0 or an errno. */
static LONG rtc_command(const char *line)
{
	struct MsgPort *reply = CreateMsgPort(), *port;
	struct RiptideMsg *m;
	LONG r = 5;          /* EIO: no reply port / no stack */
	int i;

	m = reply ? AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR) : NULL;
	if (m) {
		m->msg.mn_ReplyPort = reply;
		m->msg.mn_Length = sizeof(*m);
		m->cmd = RTCMD_COMMAND;
		for (i = 0; line[i] && i < RIPTIDE_TEXTLEN - 1; i++)
			m->text[i] = line[i];
		Forbid();
		port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
		if (port)
			PutMsg(port, &m->msg);
		Permit();
		if (port) {
			WaitPort(reply);
			GetMsg(reply);
			r = m->result;
			if (r)
				rtc_logf("rtcheck: '%s': %s", line, m->text);
		}
		FreeVec(m);
	}
	if (reply)
		DeleteMsgPort(reply);
	return r;
}

#define RTC_LOG(...) rtc_logf(__VA_ARGS__)
#define RTC_COMMAND(line) rtc_command(line)
#define RTC_CLOSELIB(b) CloseLibrary(b)
#define RTC_PUMP() Delay(1)
#define RTC_BLOCKING 1
#include "rtcheck_body.h"

/* Run a command synchronously (output to ours) or detached. */
static LONG run(const char *cmd, int async)
{
	struct TagItem t[4];
	BPTR in = 0, out = 0;

	if (async) {
		in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
		out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	}
	t[0].ti_Tag = SYS_Asynch;
	t[0].ti_Data = async;
	t[1].ti_Tag = async ? SYS_Input : TAG_IGNORE;
	t[1].ti_Data = (ULONG)in;
	t[2].ti_Tag = async ? SYS_Output : TAG_IGNORE;
	t[2].ti_Data = (ULONG)out;
	t[3].ti_Tag = TAG_DONE;
	t[3].ti_Data = 0;
	return SystemTagList((CONST_STRPTR)cmd, t);
}

#define TEMPLATE "START/K,STOP/K,CONNECT/K,NOCHECK/S,VERBOSE=-V/S,USERSTARTUP/K,BLOCK/K,DONE/K"

/* a whole file into memory (NUL-terminated); NULL when it cannot be read,
 * an empty buffer when it is not there and missing_ok */
static char *rtc_slurp(const char *path, LONG *len, int missing_ok)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	char *buf;
	LONG size, got = 0, r;

	*len = 0;
	if (!fh) {
		if (!missing_ok)
			return NULL;
		buf = AllocVec(1, MEMF_CLEAR);
		return buf;
	}
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	buf = size >= 0 ? AllocVec((ULONG)size + 1, MEMF_CLEAR) : NULL;
	while (buf && got < size && (r = Read(fh, buf + got, size - got)) > 0)
		got += r;
	Close(fh);
	if (buf && got != size) {
		FreeVec(buf);
		return NULL;
	}
	*len = got;
	return buf;
}

/* is line [l, e) the marker (case and surrounding blanks do not matter)? */
static int rtc_marker(const char *l, const char *e, const char *mark)
{
	while (l < e && (*l == ' ' || *l == '\t'))
		l++;
	while (e > l && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		e--;
	for (; l < e && *mark; l++, mark++) {
		char a = *l, b = *mark;
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (b >= 'A' && b <= 'Z')
			b += 32;
		if (a != b)
			return 0;
	}
	return l == e && !*mark;
}

static int rtc_write(BPTR fh, const char *p, LONG n)
{
	return n <= 0 || Write(fh, (APTR)p, n) == n;
}

static int rtc_userstartup(const char *path, const char *blockpath)
{
	char tmp[256], *us, *blk = NULL;
	const char *p, *e, *end;
	LONG uslen, blen = 0, blank = 0;
	BPTR fh;
	int in = 0, ok = 1, i;

	us = rtc_slurp(path, &uslen, 1);
	if (blockpath)
		blk = rtc_slurp(blockpath, &blen, 0);
	if (!us || (blockpath && !blk)) {
		rtc_logf("rtcheck: cannot read %s", !us ? path : blockpath);
		if (us)
			FreeVec(us);
		return 20;
	}
	for (i = 0; path[i] && i < (int)sizeof(tmp) - 14; i++)
		tmp[i] = path[i];
	for (p = ".riptide-new"; *p; p++)
		tmp[i++] = *p;
	tmp[i] = 0;
	fh = Open((CONST_STRPTR)tmp, MODE_NEWFILE);
	if (!fh) {
		rtc_logf("rtcheck: cannot write %s", tmp);
		FreeVec(us);
		if (blk)
			FreeVec(blk);
		return 20;
	}
	/* the file without its Riptide blocks; blank lines are written only
	 * when another line follows, so none pile up at the end */
	end = us + uslen;
	for (p = us; p < end; p = e < end ? e + 1 : end) {
		const char *q;
		for (e = p; e < end && *e != '\n'; e++)
			;
		if (!in && rtc_marker(p, e, ";BEGIN Riptide")) {
			in = 1;
			blank = 0;        /* the blank lines before it go with it */
			continue;
		}
		if (in) {
			if (rtc_marker(p, e, ";END Riptide"))
				in = 0;
			continue;
		}
		for (q = p; q < e && (*q == ' ' || *q == '\t' || *q == '\r'); q++)
			;
		if (q == e) {
			blank++;
			continue;
		}
		for (; blank > 0; blank--)
			ok &= rtc_write(fh, "\n", 1);
		ok &= rtc_write(fh, p, (LONG)(e - p)) && rtc_write(fh, "\n", 1);
	}
	if (blk) {
		/* a blank line, then ours at the end */
		ok &= rtc_write(fh, "\n;BEGIN Riptide\n", 16) && rtc_write(fh, blk, blen);
		if (blen && blk[blen - 1] != '\n')
			ok &= rtc_write(fh, "\n", 1);
		ok &= rtc_write(fh, ";END Riptide\n", 13);
	}
	Close(fh);
	FreeVec(us);
	if (blk)
		FreeVec(blk);
	if (!ok) {
		DeleteFile((CONST_STRPTR)tmp);
		rtc_logf("rtcheck: cannot write %s", tmp);
		return 20;
	}
	DeleteFile((CONST_STRPTR)path);
	if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)path)) {
		rtc_logf("rtcheck: cannot rename %s to %s", tmp, path);
		return 20;
	}
	return 0;
}

/* CONNECT=a.b.c.d:port - TCP echo test through a real interface: send a
 * line and expect it back (the host side runs an echo server). */
static void rtc_connect(const char *spec)
{
	struct rtc_sin sa;
	char host[32], buf[64];
	const char *msg = "hello from riptide\n";
	ULONG port = 0;
	LONG s, r, got = 0, i;

	for (i = 0; spec[i] && spec[i] != ':' && i < 31; i++)
		host[i] = spec[i];
	host[i] = 0;
	if (spec[i] == ':')
		for (i++; spec[i] >= '0' && spec[i] <= '9'; i++)
			port = port * 10 + (ULONG)(spec[i] - '0');
	rtc_sin(&sa, inet_addr(host), (UWORD)port);
	s = socket(RTC_AF_INET, RTC_SOCK_STREAM, 0);
	r = connect(s, &sa, sizeof(sa));
	CHECK(r == 0, "connect to the echo server over the network interface");
	if (r == 0) {
		CHECK(send(s, (APTR)msg, 19, 0) == 19, "send to echo server");
		while (got < 19) {
			r = recv(s, buf + got, sizeof(buf) - 1 - got, 0);
			if (r <= 0)
				break;
			got += r;
		}
		buf[got] = 0;
		CHECK(got == 19 && rtc_streq(buf, msg), "echo received");
	}
	CloseSocket(s);
}

static int rtcheck_main(void)
{
	LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	APTR rda;
	int fails, i;

	rda = (APTR)ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"rtcheck");
		return 20;
	}
	if (args[5]) {
		i = rtc_userstartup((const char *)args[5], (const char *)args[6]);
		if (i == 0 && args[7])
			DeleteFile((CONST_STRPTR)args[7]);
		FreeArgs(rda);
		return i;
	}
	rtc_verbose = args[4] != 0;
	if (args[0]) {
		/* start the stack ourselves (full-system tests boot straight into
		 * rtcheck from S:Startup-Sequence) */
		rtc_logf("rtcheck: starting '%s'", (const char *)args[0]);
		if (run((const char *)args[0], 1) != 0)
			rtc_logf("rtcheck: cannot start the stack");
		for (i = 0; i < 100 && !SocketBase; i++) {
			Delay(10);
			SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
		}
	} else {
		SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	}
	if (!SocketBase) {
		FreeArgs(rda);
		PutStr((CONST_STRPTR)"rtcheck: cannot open bsdsocket.library 4\n");
		return 20;
	}
	if (args[3]) {
		/* NOCHECK: only start the stack and wait until it is ready */
		rtc_logf("rtcheck: stack ready: %s", (const char *)SocketBase->lib_IdString);
		rtc_logf("rtcheck: %s %slisted, compat alias %s %slisted", RIPTIDE_LIB_NAME,
		         rtc_lib_listed(RIPTIDE_LIB_NAME) ? "" : "not ", RT_COMPAT_LIBNAME,
		         rtc_lib_listed(RT_COMPAT_LIBNAME) ? "" : "not ");
		CloseLibrary(SocketBase);
		FreeArgs(rda);
		return 0;
	}
	RiptideBase = OpenLibrary((CONST_STRPTR)RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
	CompatBase = OpenLibrary((CONST_STRPTR)RT_COMPAT_LIBNAME, RT_COMPAT_LIBVERSION);
	RTC_SECTION("libraries:");
	CHECK(RiptideBase != NULL, "open " RIPTIDE_LIB_NAME);
	CHECK(CompatBase != NULL, "open the compat alias " RT_COMPAT_LIBNAME);
	UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 4);
	rtc_logf("rtcheck: %s", (const char *)SocketBase->lib_IdString);
	fails = rtcheck_run();
	if (args[2]) {
		RTC_SECTION("TCP echo through a network interface:");
		rtc_connect((const char *)args[2]);
		fails = rtc_fail;
	}
	RTC_SECTION("compat alias on/off switch:");
	rtc_compat_switch();         /* closes CompatBase */
	fails = rtc_fail;
	if (args[1]) {
		/* must be refused: we still have the libraries open */
		rtc_logf("rtcheck: '%s' while the libraries are open:", (const char *)args[1]);
		CHECK(run((const char *)args[1], 0) != 0, "QUIT refused while libraries are open");
	}
	if (UserGroupBase)
		CloseLibrary(UserGroupBase);
	if (RiptideBase)
		CloseLibrary(RiptideBase);
	CloseLibrary(SocketBase);
	if (args[1]) {
		rtc_logf("rtcheck: '%s' after closing them:", (const char *)args[1]);
		CHECK(run((const char *)args[1], 0) == 0, "QUIT accepted after closing");
		Delay(25);
		Forbid();
		i = FindName(exec_liblist(SysBase), (CONST_STRPTR)"bsdsocket.library") != NULL;
		Permit();
		CHECK(!i, "bsdsocket.library removed after QUIT");
		fails = rtc_fail;
	}
	rtc_logf("rtcheck: %s (%d passed, %d failed)", fails ? "FAILED" : "PASSED", rtc_pass, rtc_fail);
	FreeArgs(rda);
	return fails ? 10 : 0;
}

#if defined(__VBCC__)
int main(void)
{
	return rtcheck_main();
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
	rc = rtcheck_main();
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
#endif
