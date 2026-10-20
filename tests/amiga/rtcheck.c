/*
 * Riptide - rtcheck: a bsdsocket/riptide/usergroup.library client for a
 * running Riptide on real AmigaOS (or an emulator). It opens the
 * libraries with exec OpenLibrary() and runs the checks in rtcheck_body.h
 * through the real library vectors, using blocking calls where possible.
 * Exit code 0 when every check passed, 10 otherwise, 20 if bsdsocket.library
 * cannot be opened.
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

#define TEMPLATE "START/K,STOP/K,CONNECT/K,NOCHECK/S"

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
	LONG args[4] = { 0, 0, 0, 0 };
	APTR rda;
	int fails, i;

	rda = (APTR)ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"rtcheck");
		return 20;
	}
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
	CHECK(RiptideBase != NULL, "open " RIPTIDE_LIB_NAME);
	CHECK(CompatBase != NULL, "open the compat alias " RT_COMPAT_LIBNAME);
	UserGroupBase = OpenLibrary((CONST_STRPTR)"usergroup.library", 4);
	rtc_logf("rtcheck: %s", (const char *)SocketBase->lib_IdString);
	fails = rtcheck_run();
	if (args[2]) {
		rtc_connect((const char *)args[2]);
		fails = rtc_fail;
	}
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
