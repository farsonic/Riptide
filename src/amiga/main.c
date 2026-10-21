/*
 * Riptide - CLI entry point.
 *
 *   Riptide [CONFIG=file] [DEBUG=level] [QUIT] [STATUS] [SELFTEST] [BENCH]
 *           [CAPTURE start|stop|status|save FILE|clear ...]
 *
 * Without QUIT/STATUS it starts the stack and stays in this process (start
 * it with "Run >NIL: Riptide"). QUIT and STATUS talk to a running instance
 * through the public message port "RIPTIDE", as does CAPTURE (packet
 * capture, the rest of the line is the "capture" command). BENCH times the checksum and
 * copy routines (bench.c) and exits.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

#define TEMPLATE "CONFIG/K,DEBUG/N,QUIT/S,STATUS/S,SELFTEST/S,BENCH/S,CAPTURE/K/F"
enum { ARG_CONFIG, ARG_DEBUG, ARG_QUIT, ARG_STATUS, ARG_SELFTEST, ARG_BENCH, ARG_CAPTURE, ARG_COUNT };

const char riptide_version_tag[] = "$VER: Riptide " RIPTIDE_VERSION_STRING " (" RIPTIDE_DATE ")";

static void put(const char *s)
{
	PutStr((CONST_STRPTR)s);
}

/* line: the command line of RTCMD_COMMAND, else NULL */
static int send_command(ULONG cmd, const char *line)
{
	struct MsgPort *reply, *port;
	struct RiptideMsg *m;
	char *out = NULL;
	int rc = 0;

	reply = CreateMsgPort();
	m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
	out = AllocVec(8192, MEMF_PUBLIC | MEMF_CLEAR);
	if (!reply || !m || !out) {
		put("Riptide: out of memory\n");
		rc = 20;
		goto out;
	}
	m->msg.mn_Node.ln_Type = NT_MESSAGE;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = cmd;
	if (line)
		rt_snprintf(m->text, sizeof(m->text), "capture %s", line);
	m->reply = (STRPTR)out;
	m->replysize = 8192;
	Forbid();
	port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
	if (port)
		PutMsg(port, &m->msg);
	Permit();
	if (!port) {
		put("Riptide is not running.\n");
		rc = 5;
		goto out;
	}
	WaitPort(reply);
	GetMsg(reply);
	out[8191] = 0;
	/* a Riptide 0.9 server answers in text */
	if (!out[0]) {
		m->text[sizeof(m->text) - 1] = 0;
		put(m->text);
	} else {
		/* PutStr() output is fine for any length */
		put(out);
	}
	rc = m->result ? 5 : 0;
out:
	if (out)
		FreeVec(out);
	if (m)
		FreeVec(m);
	if (reply)
		DeleteMsgPort(reply);
	return rc;
}

static int riptide_main(void)
{
	LONG args[ARG_COUNT];
	struct RDArgs *rda;
	const char *config = NULL;
	int rc = 0;

	memset(args, 0, sizeof(args));
	rda = (struct RDArgs *)ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"Riptide");
		return 10;
	}
	if (args[ARG_CAPTURE]) {
		rc = send_command(RTCMD_COMMAND, (const char *)args[ARG_CAPTURE]);
		FreeArgs(rda);
		return rc;
	}
	if (args[ARG_QUIT] || args[ARG_STATUS]) {
		rc = send_command(args[ARG_QUIT] ? RTCMD_QUIT : RTCMD_STATUS, NULL);
		FreeArgs(rda);
		return rc;
	}
	if (args[ARG_DEBUG])
		rt_log_level = (int)*(LONG *)args[ARG_DEBUG];
	if (args[ARG_CONFIG])
		config = (const char *)args[ARG_CONFIG];
	if (args[ARG_BENCH]) {
		if (amiga_os_init() < 0) {
			put("Riptide: cannot initialise (timer.device or signals)\n");
			rc = 20;
		} else {
			rc = amiga_bench();
			amiga_os_fini();
		}
		FreeArgs(rda);
		return rc;
	}
	if (amiga_libs_present()) {
		char who[256];

		put("A TCP/IP stack is already running:\n");
		amiga_libs_describe(who, sizeof(who));
		put(who);
		put("Quit it first (for Riptide: Riptide QUIT). Disable any emulator\n"
		    "bsdsocket emulation, and remove other stacks' LIBS:bsdsocket.library,\n"
		    "which loads automatically when a program opens it.\n");
		FreeArgs(rda);
		return 20;
	}
	if (args[ARG_SELFTEST]) {
		amiga_virtual_clock = 1;
		if (amiga_os_init() < 0) {
			put("Riptide: cannot initialise\n");
			rc = 20;
		} else {
			rc = amiga_selftest();
			amiga_os_fini();
		}
		FreeArgs(rda);
		return rc;
	}
	if (amiga_os_init() < 0) {
		put("Riptide: cannot initialise (timer.device or signals)\n");
		FreeArgs(rda);
		return 20;
	}
	if (amiga_stack_start(config, 0) < 0) {
		amiga_log_flush();
		amiga_libs_remove();
		amiga_stack_stop();
		amiga_os_fini();
		FreeArgs(rda);
		return 20;
	}
	amiga_stack_loop();
	amiga_stack_stop();
	amiga_os_fini();
	FreeArgs(rda);
	return rc;
}

#if defined(__VBCC__)
/* vbcc's startup code opens dos.library and handles Workbench starts. */
int main(void)
{
	return riptide_main();
}
#else
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

long amiga_main(char *argstr, long arglen)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	int rc;

	(void)argstr;
	(void)arglen;
	if (!me->pr_CLI) {
		/* Workbench start: reply the startup message and leave */
		struct Message *wbmsg;
		WaitPort(&me->pr_MsgPort);
		wbmsg = GetMsg(&me->pr_MsgPort);
		Forbid();
		ReplyMsg(wbmsg);
		return 20;
	}
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	if (!DOSBase)
		return 20;
	rc = riptide_main();
	CloseLibrary((struct Library *)DOSBase);
	return rc;
}
#endif
