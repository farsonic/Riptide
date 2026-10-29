/*
 * stackprobe - main program: arguments, which stack runs, the report on
 * the console and in a JSON or CSV file, and the helpers the tests share
 * (clock, loopback connections, waiting with a timeout, the watchdog that
 * breaks a call that blocks for ever).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/execbase.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "stackprobe.h"

struct Library *SocketBase, *MiamiBase, *UserGroupBase;
struct Device *TimerBase;
int sp_verbose, sp_quick, sp_perf;
const char *sp_peer;
ULONG sp_peer_addr;

const char sp_verstag[] = "$VER: stackprobe " SP_VERSION " (29.10.2026)";

#define SP_MAXRES 400
static struct sp_result *results;
static int nres;
static char cur_area[16];
static const char *only;
static int counts[5];

static struct timerequest *treq;
static struct MsgPort *tport;
static ULONG eclock_hz;

/* ---- clock ---- */
void sp_clock_init(void)
{
	struct EClockVal ev;

	tport = CreateMsgPort();
	if (tport)
		treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq));
	if (treq && !OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0)) {
		TimerBase = treq->tr_node.io_Device;
		eclock_hz = ReadEClock(&ev);
	}
}

static void clock_free(void)
{
	if (TimerBase)
		CloseDevice((struct IORequest *)treq);
	if (treq)
		DeleteIORequest((struct IORequest *)treq);
	if (tport)
		DeleteMsgPort(tport);
}

ULONG sp_now_us(void)
{
	struct EClockVal ev;
	unsigned long long t;

	if (!TimerBase || !eclock_hz) {
		struct DateStamp ds;
		DateStamp(&ds);
		return (ULONG)((ds.ds_Minute * 60UL * 50UL + ds.ds_Tick) * 20000UL);
	}
	ReadEClock(&ev);
	t = ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
	return (ULONG)(t * 1000000ULL / eclock_hz);
}

ULONG sp_now_ms(void)
{
	struct EClockVal ev;
	unsigned long long t;

	if (!TimerBase || !eclock_hz)
		return sp_now_us() / 1000;
	ReadEClock(&ev);
	t = ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
	return (ULONG)(t * 1000ULL / eclock_hz);
}

/* ---- the watchdog: a process that sends us Ctrl-C when a call that may
 * block for ever (on a stack that ignores a timeout option) takes too
 * long; the stack's break mask turns that into EINTR ---- */
static struct Task *wd_parent;
static volatile LONG wd_ticks;    /* ticks left, 0: disarmed */
static volatile int wd_quit, wd_running, wd_fired;

static SP_ENTRY void wd_entry(void)
{
	wd_running = 1;
	while (!wd_quit) {
		Delay(2);
		if (wd_ticks > 0) {
			wd_ticks -= 2;
			if (wd_ticks <= 0) {
				wd_ticks = 0;
				wd_fired = 1;
				Signal(wd_parent, SIGBREAKF_CTRL_C);
			}
		}
	}
	Forbid();
	wd_running = 0;
}

static void wd_start(void)
{
	wd_parent = FindTask(NULL);
	if (CreateNewProcTags(NP_Entry, (ULONG)wd_entry, NP_Name, (ULONG)"stackprobe watchdog", NP_StackSize, 4096,
	                      NP_Priority, 1, TAG_DONE))
		while (!wd_running)
			Delay(1);
}

static void wd_stop(void)
{
	wd_ticks = 0;
	wd_quit = 1;
	while (wd_running)
		Delay(1);
}

/* every test runs under the watchdog: a call that blocks for ever on some
 * stack ends with EINTR after SP_SAFETY_MS instead of hanging the run */
#define SP_SAFETY_MS 20000

/* arm the watchdog for ms milliseconds; 0: back to the safety time and
 * clear our Ctrl-C */
void sp_watchdog(LONG ms)
{
	wd_fired = 0;
	if (!ms) {
		SetSignal(0, SIGBREAKF_CTRL_C);
		ms = SP_SAFETY_MS;
	}
	wd_ticks = (ms + 19) / 20;
}

int sp_watchdog_fired(void)
{
	return wd_fired;
}

/* ---- report ---- */
static const char *const resname[5] = { "PASS", "FAIL", "UNSUPPORTED", "SKIPPED", "INFO" };
static const char *const resshort[5] = { "PASS", "FAIL", "--  ", "skip", "info" };

void sp_area(const char *area)
{
	strncpy(cur_area, area, sizeof(cur_area) - 1);
	printf("\n== %s\n", area);
}

static void add(const char *id, const char *desc, int res, LONG err, LONG ms, const char *fmt, va_list ap)
{
	struct sp_result *r;
	char val[64];

	val[0] = 0;
	if (fmt)
		vsnprintf(val, sizeof(val), fmt, ap);
	if (wd_fired && !val[0] && res != R_PASS)
		strcpy(val, "blocked: ended by Ctrl-C");
	sp_watchdog(0);
	if (res == R_PASS || res == R_INFO)
		err = 0;
	printf("  %-4s  %-28s %-38s", resshort[res], id, desc);
	if (val[0])
		printf(" %s", val);
	if (err)
		printf(" (%s)", sp_errname(err));
	if (ms >= 0 && sp_verbose)
		printf(" [%ld ms]", (long)ms);
	printf("\n");
	counts[res]++;
	if (!results || nres >= SP_MAXRES)
		return;
	r = &results[nres++];
	strncpy(r->id, id, sizeof(r->id) - 1);
	strncpy(r->desc, desc, sizeof(r->desc) - 1);
	strcpy(r->value, val);
	r->res = (UBYTE)res;
	r->err = err;
	r->ms = ms;
}

void sp_result(const char *id, const char *desc, int res, LONG err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	add(id, desc, res, err, -1, fmt, ap);
	va_end(ap);
}

void sp_result_ms(const char *id, const char *desc, int res, LONG err, LONG ms, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	add(id, desc, res, err, ms, fmt, ap);
	va_end(ap);
}

void sp_unsup_call(const char *id, const char *desc)
{
	sp_result(id, desc, R_UNSUP, 0, "no such call");
}

void sp_note(const char *fmt, ...)
{
	va_list ap;
	if (!sp_verbose)
		return;
	va_start(ap, fmt);
	printf("        ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

/* ---- helpers ---- */
/* strncmp() ignoring ASCII case */
int sp_strnicmp(const char *a, const char *b, unsigned long n)
{
	for (; n; n--, a++, b++) {
		int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
		if (x != y)
			return x - y;
		if (!x)
			break;
	}
	return 0;
}

LONG sp_errno(void)
{
	return SP_HAVE_BSD(Errno) ? sp_Errno() : -1;
}

const char *sp_errname(LONG e)
{
	static char buf[16];
	static const struct { LONG n; const char *s; } names[] = {
		{ EINTR, "EINTR" }, { EBADF, "EBADF" }, { EACCES, "EACCES" }, { EINVAL, "EINVAL" },
		{ EMFILE, "EMFILE" }, { EPIPE, "EPIPE" }, { EWOULDBLOCK, "EWOULDBLOCK" },
		{ EINPROGRESS, "EINPROGRESS" }, { EALREADY, "EALREADY" }, { ENOTSOCK, "ENOTSOCK" },
		{ EMSGSIZE, "EMSGSIZE" }, { EPROTOTYPE, "EPROTOTYPE" }, { ENOPROTOOPT, "ENOPROTOOPT" },
		{ EPROTONOSUPPORT, "EPROTONOSUPPORT" }, { ESOCKTNOSUPPORT, "ESOCKTNOSUPPORT" },
		{ EOPNOTSUPP, "EOPNOTSUPP" }, { EAFNOSUPPORT, "EAFNOSUPPORT" }, { EADDRINUSE, "EADDRINUSE" },
		{ EADDRNOTAVAIL, "EADDRNOTAVAIL" }, { ENETUNREACH, "ENETUNREACH" },
		{ ECONNABORTED, "ECONNABORTED" }, { ECONNRESET, "ECONNRESET" }, { ENOBUFS, "ENOBUFS" },
		{ EISCONN, "EISCONN" }, { ENOTCONN, "ENOTCONN" }, { ETIMEDOUT, "ETIMEDOUT" },
		{ ECONNREFUSED, "ECONNREFUSED" }, { EHOSTUNREACH, "EHOSTUNREACH" }, { ENOSYS, "ENOSYS" },
	};
	unsigned i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].n == e)
			return names[i].s;
	sprintf(buf, "errno %ld", (long)e);
	return buf;
}

void sp_sin(struct sp_sockaddr_in *sa, ULONG addr, UWORD port)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_len = sizeof(*sa);
	sa->sin_family = AF_INET;
	sa->sin_port = port;
	sa->sin_addr.s_addr = addr;
}

LONG sp_nonblock(LONG s, LONG on)
{
	return sp_IoctlSocket(s, FIONBIO, (APTR)&on);
}

/* 1: s is readable (writable), 0: not within ms, -1: error */
LONG sp_wait(LONG s, int write, LONG ms)
{
	sp_fdset fs;
	struct sp_timeval tv;
	LONG r;

	SP_FD_ZERO(&fs);
	SP_FD_SET(s, &fs);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	r = sp_WaitSelect(s + 1, write ? NULL : (APTR)&fs, write ? (APTR)&fs : NULL, NULL, (APTR)&tv, NULL);
	return r < 0 ? -1 : r > 0 && SP_FD_ISSET(s, &fs) ? 1 : 0;
}

void sp_close(LONG *s)
{
	if (*s >= 0)
		sp_CloseSocket(*s);
	*s = -1;
}

/* a listening TCP socket on 127.0.0.1; *port gets its port */
LONG sp_tcp_listener(UWORD *port)
{
	struct sp_sockaddr_in sa;
	LONG s = sp_socket(AF_INET, SOCK_STREAM, 0), one = 1, len = sizeof(sa);

	if (s < 0)
		return -1;
	sp_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof(one));
	sp_sin(&sa, INADDR_LOOPBACK, 0);
	if (sp_bind(s, (APTR)&sa, sizeof(sa)) < 0 || sp_listen(s, 5) < 0 ||
	    sp_getsockname(s, (APTR)&sa, &len) < 0 || !sa.sin_port) {
		sp_CloseSocket(s);
		return -1;
	}
	*port = sa.sin_port;
	return s;
}

/* a connected loopback pair through listener lst: client and accepted
 * socket, both blocking; 0 or -1 (errno in the stack) */
LONG sp_tcp_pair(LONG *client, LONG *server, LONG lst, UWORD port)
{
	struct sp_sockaddr_in sa;
	LONG c, a, len = sizeof(sa);

	*client = *server = -1;
	c = sp_socket(AF_INET, SOCK_STREAM, 0);
	if (c < 0)
		return -1;
	sp_nonblock(c, 1);
	sp_sin(&sa, INADDR_LOOPBACK, port);
	if (sp_connect(c, (APTR)&sa, sizeof(sa)) < 0 && sp_errno() != EINPROGRESS) {
		sp_CloseSocket(c);
		return -1;
	}
	if (sp_wait(lst, 0, 3000) != 1) {
		sp_CloseSocket(c);
		return -1;
	}
	a = sp_accept(lst, (APTR)&sa, &len);
	if (a < 0 || sp_wait(c, 1, 3000) != 1) {
		if (a >= 0)
			sp_CloseSocket(a);
		sp_CloseSocket(c);
		return -1;
	}
	sp_nonblock(c, 0);
	sp_nonblock(a, 0);
	*client = c;
	*server = a;
	return 0;
}

/* receive exactly len bytes, waiting up to ms for each piece; bytes got */
LONG sp_recv_all(LONG s, char *buf, LONG len, LONG ms)
{
	LONG got = 0, r;

	while (got < len) {
		if (sp_wait(s, 0, ms) != 1)
			break;
		r = sp_recv(s, buf + got, len - got, 0);
		if (r <= 0)
			break;
		got += r;
	}
	return got;
}

/* ---- which stack, which machine ---- */
static char stack_name[40], stack_version[48], release[128], idstring[96], cpu[16], os[32], romid[64], wbver[16];

static int contains(const char *s, const char *w)
{
	size_t n = strlen(w);
	for (; *s; s++)
		if (!sp_strnicmp(s, w, n))
			return 1;
	return 0;
}

static void copy_clean(char *d, const char *s, size_t size)
{
	size_t i;
	for (i = 0; s && s[i] && i < size - 1; i++)
		d[i] = s[i] == '\r' || s[i] == '\n' || s[i] == '"' || s[i] == '\\' ? ' ' : s[i];
	d[i] = 0;
	while (i && d[i - 1] == ' ')
		d[--i] = 0;
}

static void detect(const char *label)
{
	struct ExecBase *eb = SysBase;
	UWORD a = eb->AttnFlags;
	struct Library *l;
	ULONG tags[3];
	const char *v = NULL;

	strcpy(cpu, a & AFF_68060 ? "68060" : a & AFF_68040 ? "68040" : a & AFF_68030 ? "68030" :
	            a & AFF_68020 ? "68020" : a & AFF_68010 ? "68010" : "68000");
	if (a & (AFF_68881 | AFF_68882 | AFF_FPU40))
		strcat(cpu, "+FPU");
	sprintf(os, "Kickstart %u.%u", (unsigned)eb->LibNode.lib_Version, (unsigned)eb->SoftVer);
	if (eb->LibNode.lib_IdString)
		copy_clean(romid, (const char *)eb->LibNode.lib_IdString, sizeof(romid));
	if ((l = OpenLibrary((STRPTR)"version.library", 0)) != NULL) {
		sprintf(wbver, "%u.%u", (unsigned)l->lib_Version, (unsigned)l->lib_Revision);
		CloseLibrary(l);
	}
	if (SocketBase->lib_IdString)
		copy_clean(idstring, (const char *)SocketBase->lib_IdString, sizeof(idstring));
	tags[0] = SBTM_GETVAL(SBTC_RELEASESTRPTR);
	tags[1] = 0;
	tags[2] = TAG_DONE;
	if (SP_HAVE_BSD(SocketBaseTagList) && !sp_SocketBaseTagList((APTR)tags) && tags[1])
		copy_clean(release, (const char *)tags[1], sizeof(release));
	if ((l = OpenLibrary((STRPTR)"riptide.library", 0)) != NULL) {
		strcpy(stack_name, "Riptide");
		CloseLibrary(l);
	} else if (contains(release, "Roadshow") || contains(idstring, "Roadshow")) {
		strcpy(stack_name, "Roadshow");
	} else if (contains(release, "Genesis") || contains(idstring, "Genesis")) {
		strcpy(stack_name, "Genesis");
	} else if (contains(release, "Miami") || contains(idstring, "Miami") || MiamiBase) {
		strcpy(stack_name, "Miami");
	} else if (contains(release, "AmiTCP") || contains(idstring, "AmiTCP")) {
		strcpy(stack_name, "AmiTCP");
	} else if (contains(release, "MorphOS") || contains(idstring, "MorphOS")) {
		strcpy(stack_name, "MorphOS netstack");
	} else {
		copy_clean(stack_name, release[0] ? release : idstring[0] ? idstring : "unknown", 20);
		if (strchr(stack_name, ' '))
			*strchr(stack_name, ' ') = 0;
	}
	/* the version: the first word with a digit in the release string */
	for (v = release; *v; v++)
		if (*v >= '0' && *v <= '9' && (v == release || v[-1] == ' ' || v[-1] == 'v' || v[-1] == 'V'))
			break;
	if (*v) {
		size_t n = strcspn(v, " ()");
		if (n >= sizeof(stack_version))
			n = sizeof(stack_version) - 1;
		memcpy(stack_version, v, n);
		stack_version[n] = 0;
	} else {
		sprintf(stack_version, "%u.%u", SocketBase->lib_Version, SocketBase->lib_Revision);
	}
	if (label && *label)
		copy_clean(stack_name, label, sizeof(stack_name));
}

/* ---- the report file ---- */
static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		if ((unsigned char)*s >= 32)
			fputc(*s, f);
	}
	fputc('"', f);
}

static void write_json(const char *path)
{
	FILE *f = fopen(path, "w");
	int i;

	if (!f) {
		printf("stackprobe: cannot write %s\n", path);
		return;
	}
	fprintf(f, "{\n \"probe\": \"stackprobe " SP_VERSION "\",\n \"stack\": {\"name\": ");
	json_str(f, stack_name);
	fprintf(f, ", \"version\": ");
	json_str(f, stack_version);
	fprintf(f, ", \"release\": ");
	json_str(f, release);
	fprintf(f, ", \"idstring\": ");
	json_str(f, idstring);
	fprintf(f, ", \"bsdsocket\": \"%u.%u\"},\n", (unsigned)(SocketBase ? SocketBase->lib_Version : 0),
	        (unsigned)(SocketBase ? SocketBase->lib_Revision : 0));
	fprintf(f, " \"system\": {\"cpu\": \"%s\", \"os\": \"%s\", \"exec\": ", cpu, os);
	json_str(f, romid);
	fprintf(f, ", \"workbench\": \"%s\"},\n", wbver);
	fprintf(f, " \"options\": {\"quick\": %d, \"perf\": %d, \"peer\": %s},\n", sp_quick, sp_perf, sp_peer ? "true" : "false");
	fprintf(f, " \"tests\": [\n");
	for (i = 0; i < nres; i++) {
		struct sp_result *r = &results[i];
		fprintf(f, "  {\"id\": \"%s\", \"desc\": ", r->id);
		json_str(f, r->desc);
		fprintf(f, ", \"result\": \"%s\", \"value\": ", resname[r->res]);
		json_str(f, r->value);
		fprintf(f, ", \"errno\": %ld, \"ms\": %ld}%s\n", (long)r->err, (long)r->ms, i + 1 < nres ? "," : "");
	}
	fprintf(f, " ]\n}\n");
	fclose(f);
}

static void write_csv(const char *path)
{
	FILE *f = fopen(path, "w");
	int i;

	if (!f) {
		printf("stackprobe: cannot write %s\n", path);
		return;
	}
	fprintf(f, "stack,version,id,result,value,errno,ms\n");
	for (i = 0; i < nres; i++) {
		struct sp_result *r = &results[i];
		char *p;
		for (p = r->value; *p; p++)
			if (*p == ',')
				*p = ';';
		fprintf(f, "%s,%s,%s,%s,%s,%ld,%ld\n", stack_name, stack_version, r->id, resname[r->res], r->value,
		        (long)r->err, (long)r->ms);
	}
	fclose(f);
}

/* ---- main ---- */
static int want(const char *area)
{
	const char *p;
	size_t n = strlen(area);

	if (!only)
		return strcmp(area, "perf") || sp_perf;
	for (p = only; *p;) {
		if (!sp_strnicmp(p, area, n) && (p[n] == 0 || p[n] == ','))
			return 1;
		p = strchr(p, ',');
		if (!p)
			break;
		p++;
	}
	return 0;
}

#define TEMPLATE "QUICK/S,PERF/S,PEER/K,ONLY/K,JSON/K,CSV/K,NAME/K,VERBOSE/S"

int main(void)
{
	LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda;
	ULONG t0;

	rda = ReadArgs((STRPTR)TEMPLATE, args, NULL);
	if (!rda) {
		PrintFault(IoErr(), (STRPTR)"stackprobe");
		printf("Usage: stackprobe [QUICK] [PERF] [PEER=host] [ONLY=area,...] [JSON=file] [CSV=file]\n"
		       "                  [NAME=label] [VERBOSE]\n"
		       "Areas: libs api tags netdb tcp udp options select misc ipv6 ext perf peer\n");
		return 10;
	}
	sp_quick = args[0] != 0;
	sp_perf = args[1] != 0;
	sp_peer = (const char *)args[2];
	only = (const char *)args[3];
	sp_verbose = args[7] != 0;
	results = calloc(SP_MAXRES, sizeof(*results));
	sp_clock_init();

	printf("stackprobe " SP_VERSION ": TCP/IP stack features through the public Amiga APIs\n");
	sp_area("libs");
	sp_test_libraries();
	if (!SocketBase) {
		printf("\nNo bsdsocket.library: no TCP/IP stack is running.\n");
		strcpy(stack_name, "none");
		if (args[4])
			write_json((const char *)args[4]);
		clock_free();
		FreeArgs(rda);
		return 5;
	}
	detect((const char *)args[6]);
	printf("\nStack: %s %s (bsdsocket.library %u.%u)\n  release: %s\n  id: %s\n  CPU %s, %s (%s), Workbench %s\n",
	       stack_name, stack_version, (unsigned)SocketBase->lib_Version, (unsigned)SocketBase->lib_Revision,
	       release[0] ? release : "-", idstring[0] ? idstring : "-", cpu, os, romid, wbver[0] ? wbver : "-");
	if (sp_peer) {
		ULONG a = sp_inet_addr((STRPTR)sp_peer);
		if (a == 0xffffffffUL) {
			struct sp_hostent *h = (struct sp_hostent *)sp_gethostbyname((STRPTR)sp_peer);
			if (h && h->h_addr_list && h->h_addr_list[0])
				memcpy(&a, h->h_addr_list[0], 4);
		}
		sp_peer_addr = a;
	}
	wd_start();
	sp_watchdog(0);
	t0 = sp_now_ms();
	if (want("api")) { sp_area("api"); sp_test_api(); }
	if (want("tags")) { sp_area("tags"); sp_test_tags(); }
	if (want("netdb")) { sp_area("netdb"); sp_test_netdb(); }
	if (want("tcp")) { sp_area("tcp"); sp_test_tcp(); }
	if (want("udp")) { sp_area("udp"); sp_test_udp(); }
	if (want("options")) { sp_area("options"); sp_test_options(); }
	if (want("select")) { sp_area("select"); sp_test_select(); }
	if (want("misc")) { sp_area("misc"); sp_test_misc(); }
	if (want("ipv6")) { sp_area("ipv6"); sp_test_ipv6(); }
	if (want("ext")) { sp_area("ext"); sp_test_ext(); }
	if (want("perf")) { sp_area("perf"); sp_test_perf(); }
	if (want("peer")) { sp_area("peer"); sp_test_peer(); }
	wd_stop();
	SetSignal(0, SIGBREAKF_CTRL_C);

	printf("\n%s %s: %d passed, %d failed, %d unsupported, %d skipped (%lu s)\n", stack_name, stack_version,
	       counts[R_PASS], counts[R_FAIL], counts[R_UNSUP], counts[R_SKIP], (unsigned long)((sp_now_ms() - t0) / 1000));
	if (args[4])
		write_json((const char *)args[4]);
	if (args[5])
		write_csv((const char *)args[5]);
	if (UserGroupBase)
		CloseLibrary(UserGroupBase);
	if (MiamiBase)
		CloseLibrary(MiamiBase);
	CloseLibrary(SocketBase);
	clock_free();
	FreeArgs(rda);
	return 0;
}
