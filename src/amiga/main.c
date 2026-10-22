/*
 * Riptide - CLI entry point.
 *
 *   Riptide [CONFIG=file] [DEBUG=level] [QUIT] [STATUS] [SELFTEST] [BENCH]
 *           [CAPTURE start|stop|status|save FILE|clear ...]
 *   Riptide PROFILE START|STOP|RESET|SHOW
 *   Riptide DEBUG CATEGORIES|NONE|SHOW [N]|CLEAR|DUMP FILE|STATUS [SIZE=KB]
 *   Riptide NTP [SYNC|STATUS|...] | MDNS [ON|OFF|STATUS] | TIMEZONE [+HH:MM|AUTO]
 *   Riptide BACKGROUND [CLEAR] [program=NAME,...] [port=P,...] [lport=P,...] [mode=...]
 *
 * Without QUIT/STATUS/CAPTURE/PROFILE/DEBUG it starts the stack and stays
 * in this process (start it with "Run >NIL: Riptide"); a numeric DEBUG is
 * then the log level. The others talk to a running instance through the
 * public message port "RIPTIDE" (CAPTURE: packet capture, the rest of the
 * line is the "capture" command). BENCH times the checksum and copy
 * routines (bench.c) and exits.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

/* CAPTURE/F: the rest of the line as it is (ARGS/M would split "iface=x") */
#define TEMPLATE "CONFIG/K,DEBUG/K,QUIT/S,STATUS/S,SELFTEST/S,BENCH/S,PROFILE/K,CAPTURE/K/F,ARGS/M"
enum { ARG_CONFIG, ARG_DEBUG, ARG_QUIT, ARG_STATUS, ARG_SELFTEST, ARG_BENCH, ARG_PROFILE, ARG_CAPTURE,
       ARG_ARGS, ARG_COUNT };

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
		strncpy((char *)m->text, line, sizeof(m->text) - 1);
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

static int is_number(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

/* "Riptide PROFILE SHOW", "Riptide DEBUG DUMP RAM:x": one line of the
 * configuration language for the running instance */
static int send_verb(const char *verb, const char *first, char **more)
{
	char line[256];
	u32 o;

	o = (u32)rt_snprintf(line, sizeof(line), "%s %s", verb, first);
	/* words with spaces (file names) quoted again */
	for (; more && *more && o < sizeof(line); more++)
		o += (u32)rt_snprintf(line + o, sizeof(line) - o, strchr(*more, ' ') ? " \"%s\"" : " %s", *more);
	return send_command(RTCMD_COMMAND, line);
}

static int riptide_main(void)
{
	LONG args[ARG_COUNT];
	struct RDArgs *rda;
	const char *config = NULL;
	char **more;
	int rc = 0;

	memset(args, 0, sizeof(args));
	rda = (struct RDArgs *)ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (CONST_STRPTR)"Riptide");
		return 10;
	}
	more = (char **)args[ARG_ARGS];
	/* Riptide NTP SYNC|STATUS|..., MDNS ON|OFF|STATUS, TIMEZONE +HH:MM
	 * (ReadArgs takes a STATUS word for the STATUS switch) */
	if (more && (!rt_strcasecmp(more[0], "ntp") || !rt_strcasecmp(more[0], "mdns") ||
	             !rt_strcasecmp(more[0], "timezone") || !rt_strcasecmp(more[0], "background"))) {
		char verb[12];
		u32 i;
		for (i = 0; more[0][i] && i + 1 < sizeof(verb); i++)
			verb[i] = (char)(more[0][i] | 0x20);
		verb[i] = 0;
		rc = send_verb(verb, more[1] ? more[1] : args[ARG_STATUS] ? "status" : "",
		               more[1] ? more + 2 : NULL);
		FreeArgs(rda);
		return rc;
	}
	if (args[ARG_QUIT] || args[ARG_STATUS]) {
		rc = send_command(args[ARG_QUIT] ? RTCMD_QUIT : RTCMD_STATUS, NULL);
		FreeArgs(rda);
		return rc;
	}
	if (args[ARG_CAPTURE]) {
		rc = send_verb("capture", (char *)args[ARG_CAPTURE], NULL);
		FreeArgs(rda);
		return rc;
	}
	if (args[ARG_PROFILE] || (args[ARG_DEBUG] && (more || !is_number((char *)args[ARG_DEBUG])))) {
		rc = send_verb(args[ARG_PROFILE] ? "profile" : "debug",
		               (char *)(args[ARG_PROFILE] ? args[ARG_PROFILE] : args[ARG_DEBUG]), more);
		FreeArgs(rda);
		return rc;
	}
	if (more) {
		put("Riptide: unexpected argument (see Riptide ?)\n");
		FreeArgs(rda);
		return 10;
	}
	if (args[ARG_DEBUG])
		rt_log_level = (int)rt_strtoul((char *)args[ARG_DEBUG], NULL, 10);
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
