/*
 * Riptide - rtdiag: collect everything needed to diagnose a slow transfer
 * into one drawer.
 *
 *   rtdiag [-o DIR] [-t SECONDS] [-i IF] [-s KB] [-v] [COMMAND ...]
 *
 * Resets the interface and driver counters, starts the profiler, the
 * debug log (log messages and a tcpstat line per connection every 500 ms;
 * -v adds every TCP decision and received frame, which slows the stack
 * down) and a packet capture, then runs COMMAND (e.g. "ftp -o NIL:
 * ftp://host/file") or, without one, waits SECONDS (default 30) or for
 * Ctrl-C while you run the transfer in another Shell. Afterwards it
 * writes DIR (default RAM:rtdiag):
 *   report.txt    the run, the profile, counters, tables and settings
 *   debug.txt     the debug log
 *   capture.pcap  the packet capture (Wireshark, tcpdump)
 * and switches the diagnostics off again.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"

#define CMDLEN 512

static BPTR rep;                 /* report.txt */
static char path[256];

static void usage(void)
{
	cl_puts("Usage: rtdiag [-o DIR] [-t SECONDS] [-i IF] [-s KB] [-v] [COMMAND ...]\n"
	        "  -o DIR      where to write report.txt, debug.txt, capture.pcap (RAM:rtdiag)\n"
	        "  -t SECONDS  without COMMAND: how long to record (30; Ctrl-C stops)\n"
	        "  -i IF       capture on this interface only (default: all)\n"
	        "  -s KB       capture ring size (512)\n"
	        "  -v          debug log of every TCP decision and frame (slower)\n"
	        "Runs COMMAND (or waits) with the profiler, debug log and packet\n"
	        "capture on, then saves everything for a bug report.\n");
}

static void rput(const char *s)
{
	if (rep)
		Write(rep, (APTR)s, (LONG)strlen(s));
}

static void rprintf(const char *fmt, ...) RT_PRINTF(1, 2);
static void rprintf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	rput(buf);
}

static const char *in_dir(const char *dir, const char *file)
{
	u32 l = (u32)strlen(dir);
	char c = l ? dir[l - 1] : ':';

	rt_snprintf(path, sizeof(path), "%s%s%s", dir, (c == ':' || c == '/') ? "" : "/", file);
	return path;
}

/* a RIPTIDE port command; its answer goes to the report (and the console
 * when it failed) */
static int port_cmd(const char *title, const char *line)
{
	int r = cl_command(line, 0);

	if (title) {
		rprintf("==== %s\n", title);
		rput(cl_reply());
		rput("\n");
	}
	if (r)
		cl_printf("rtdiag: %s: %s", line, cl_reply()[0] ? cl_reply() : "failed\n");
	return r;
}

/* run a tool with its output appended to the report */
static void tool(const char *line)
{
	struct TagItem tags[3];
	BPTR nil = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);

	rprintf("==== %s\n", line);
	tags[0].ti_Tag = SYS_Input;
	tags[0].ti_Data = (ULONG)nil;
	tags[1].ti_Tag = SYS_Output;
	tags[1].ti_Data = (ULONG)rep;
	tags[2].ti_Tag = TAG_END;
	tags[2].ti_Data = 0;
	if (SystemTagList((CONST_STRPTR)line, tags) == -1)
		rput("(could not run it)\n");
	rput("\n");
	if (nil)
		Close(nil);
}

/* 1/50 s since t0 (cl_now_us64(): steady when the clock is set, e.g. by NTP) */
static u32 ticks_since(u64 t0)
{
	return (u32)((cl_now_us64() - t0) / 20000u);
}

static const char *cpu_name(void)
{
	UWORD af = RT_AMIGA_ATTNFLAGS(SysBase);

	return (af & (1u << 7)) ? "68060" : (af & (1u << 3)) ? "68040" : (af & (1u << 2)) ? "68030" :
	       (af & (1u << 1)) ? "68020" : (af & (1u << 0)) ? "68010" : "68000";
}

static int rtdiag_main(int argc, char **argv)
{
	struct cl_getopt g = { 1, 0, NULL };
	const char *dir = "RAM:rtdiag", *iface = NULL;
	static char cmd[CMDLEN], absdir[256];
	u64 t0;
	u32 secs = 30, kb = 512, ticks, o = 0;
	LONG rc = 0;
	BPTR lock;
	int c, i, verbose = 0;

	while ((c = cl_getopt(argc, argv, "o:t:i:s:vh", &g)) != -1) {
		switch (c) {
		case 'v': verbose = 1; break;
		case 'o': dir = g.arg; break;
		case 'i': iface = g.arg; break;
		case 't':
			if (cl_parse_uint(g.arg, 86400, &secs) < 0 || !secs) {
				cl_error("bad time '%s'", g.arg);
				return CL_ERROR;
			}
			break;
		case 's':
			if (cl_parse_uint(g.arg, 16384, &kb) < 0 || kb < 16) {
				cl_error("bad capture size '%s' (16..16384 KB)", g.arg);
				return CL_ERROR;
			}
			break;
		case 'h': usage(); return CL_OK;
		default: usage(); return CL_ERROR;
		}
	}
	cmd[0] = 0;
	for (i = g.ind; i < argc && o < sizeof(cmd) - 1; i++)
		o += (u32)rt_snprintf(cmd + o, sizeof(cmd) - o, strchr(argv[i], ' ') ? "%s\"%s\"" : "%s%s",
		                      o ? " " : "", argv[i]);
	if (o >= sizeof(cmd) - 1) {
		cl_error("command too long");
		return CL_ERROR;
	}

	if (cl_need_timer() < 0)
		return CL_ERROR;
	/* is Riptide running, and does it have the diagnostics? */
	c = cl_command("debug status", 0);
	if (c < 0)
		return CL_FAIL;
	if (c) {
		cl_printf("rtdiag: this Riptide has no debug log: %s", cl_reply());
		return CL_FAIL;
	}
	lock = Lock((CONST_STRPTR)dir, ACCESS_READ);
	if (!lock)
		lock = CreateDir((CONST_STRPTR)dir);
	if (!lock) {
		cl_error("cannot create %s", dir);
		return CL_ERROR;
	}
	/* Riptide writes the capture and the log: give it the full path */
	if (NameFromLock(lock, (STRPTR)absdir, sizeof(absdir)))
		dir = absdir;
	UnLock(lock);
	rep = Open((CONST_STRPTR)in_dir(dir, "report.txt"), MODE_NEWFILE);
	if (!rep) {
		cl_error("cannot write %s", path);
		return CL_ERROR;
	}

	rput("rtdiag report\n");
	rprintf("CPU %s, Kickstart %u, free memory: chip %lu KB, fast %lu KB\n", cpu_name(),
	        (unsigned)((struct Library *)SysBase)->lib_Version, (unsigned long)(AvailMem(MEMF_CHIP) >> 10),
	        (unsigned long)(AvailMem(MEMF_FAST) >> 10));
	rprintf("command: %s\n\n", cmd[0] ? cmd : "(none: recorded while waiting)");
	if (cl_request(RTCMD_STATUS, NULL, 0) == 0) {
		rput("==== Riptide STATUS (before)\n");
		rput(cl_reply());
		rput("\n");
	}
	tool("sysctl -a");

	/* start everything */
	port_cmd(NULL, "interface all resetstats");
	port_cmd(NULL, "profile reset");
	port_cmd(NULL, "debug clear");
	port_cmd(NULL, verbose ? "debug tcp,driver,log,tcpstat interval=500 size=256" :
	                         "debug log,tcpstat interval=500 size=64");
	port_cmd(NULL, "capture stop");
	port_cmd(NULL, "capture clear");
	if (port_cmd(NULL, cl_line("capture start snaplen=128 size=%lu%s%s", (unsigned long)kb,
	                           iface ? " iface=" : "", iface ? iface : "")))
		cl_puts("rtdiag: continuing without a packet capture\n");
	port_cmd(NULL, "profile start");

	t0 = cl_now_us64();
	if (cmd[0]) {
		struct TagItem tags[3];
		cl_printf("rtdiag: running \"%s\"\n", cmd);
		cl_flush();
		tags[0].ti_Tag = SYS_Input;
		tags[0].ti_Data = (ULONG)Input();
		tags[1].ti_Tag = SYS_Output;
		tags[1].ti_Data = (ULONG)Output();
		tags[2].ti_Tag = TAG_END;
		tags[2].ti_Data = 0;
		rc = SystemTagList((CONST_STRPTR)cmd, tags);
	} else {
		cl_printf("rtdiag: recording for %lu seconds; start the transfer now (Ctrl-C stops)\n",
		          (unsigned long)secs);
		cl_flush();
		while (ticks_since(t0) < secs * 50u && !cl_break())
			Delay(10);
	}
	ticks = ticks_since(t0);

	/* stop and collect */
	port_cmd(NULL, "profile stop");
	port_cmd(NULL, "capture stop");
	port_cmd(NULL, "debug -tcpstat");
	rprintf("==== run\n%lu.%02lu seconds", (unsigned long)(ticks / 50u), (unsigned long)(ticks % 50u * 2u));
	if (cmd[0])
		rprintf(", command returned %ld", (long)rc);
	rput("\n\n");
	port_cmd("profile show", "profile show");
	port_cmd("capture status", "capture status");
	port_cmd("debug status", "debug status");
	tool("ip -s -s link");
	tool("ifconfig -v");
	tool("ip addr");
	tool("ip route");
	tool("ip -6 route");
	tool("netstat -s");
	tool("netstat -T -a -n");
	if (cl_request(RTCMD_STATUS, NULL, 0) == 0) {
		rput("==== Riptide STATUS (after)\n");
		rput(cl_reply());
		rput("\n");
	}

	if (port_cmd(NULL, cl_line("capture save \"%s\"", in_dir(dir, "capture.pcap"))) == 0)
		cl_printf("rtdiag: %s\n", path);
	if (port_cmd(NULL, cl_line("debug dump \"%s\"", in_dir(dir, "debug.txt"))) == 0)
		cl_printf("rtdiag: %s\n", path);
	port_cmd(NULL, "debug none");
	port_cmd(NULL, "capture clear");
	Close(rep);
	rep = 0;
	cl_printf("rtdiag: %s\n", in_dir(dir, "report.txt"));
	cl_printf("rtdiag: %lu.%02lu seconds. Send these files with your report.\n",
	          (unsigned long)(ticks / 50u), (unsigned long)(ticks % 50u * 2u));
	return CL_OK;
}

CL_MAIN(rtdiag_main, "rtdiag")
