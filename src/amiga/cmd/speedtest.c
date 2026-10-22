/*
 * Riptide - speedtest: bandwidth test against speedtest.net servers (or
 * any server speaking the legacy speedtest HTTP protocol).
 *
 *   speedtest [--simple|--json|--csv] [--list] [--server ID] ...
 *   speedtest --server-url http://HOST:PORT/speedtest/upload.php
 *
 * The protocol, HTTP, XML and measurement code is portable
 * (src/apps/speedtest); this file is its AmigaOS shim: bsdsocket.library
 * sockets (non-blocking, multiplexed with WaitSelect, Ctrl-C in the
 * signal mask), timer.device for the clock, AllocVec for the buffers.
 * Uses only the standard bsdsocket.library API.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"
#include "../../apps/speedtest/speedtest.h"

struct st_tv { ULONG secs, micro; };

static int a_resolve(void *ctx, const char *host, u32 *addr)
{
	struct hostent *h;

	if (cl_parse_ip(host, addr) == 0)
		return 0;
	h = (struct hostent *)gethostbyname((CONST_STRPTR)host);
	if (!h || !h->h_addr_list || !h->h_addr_list[0])
		return -1;
	memcpy(addr, h->h_addr_list[0], 4);
	return 0;
}

static int a_connect(void *ctx, u32 addr, u16 port)
{
	struct rt_sockaddr_in sa;
	LONG fd, on = 1, e;

	fd = socket(RT_AF_INET, RT_SOCK_STREAM, 0);
	if (fd < 0)
		return -(int)Errno();
	IoctlSocket(fd, RT_FIONBIO, &on);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons(port);
	sa.sin_addr = addr;
	if (connect(fd, &sa, sizeof(sa)) < 0) {
		e = Errno();
		if (e != RT_EINPROGRESS && e != RT_EALREADY) {
			CloseSocket(fd);
			return -(int)e;
		}
	}
	return (int)fd;
}

static int a_conn_error(void *ctx, int fd)
{
	LONG e = 0, len = sizeof(e);

	if (getsockopt(fd, RT_SOL_SOCKET, RT_SO_ERROR, &e, &len) < 0)
		e = Errno();
	return (int)e;
}

static int a_send(void *ctx, int fd, const void *p, u32 n)
{
	LONG k = send(fd, (APTR)p, (LONG)n, 0);

	return k < 0 ? -(int)Errno() : (int)k;
}

static int a_recv(void *ctx, int fd, void *p, u32 n)
{
	LONG k = recv(fd, p, (LONG)n, 0);

	return k < 0 ? -(int)Errno() : (int)k;
}

static void a_close(void *ctx, int fd)
{
	CloseSocket(fd);
}

static int a_wait(void *ctx, int nfds, u32 *rd, u32 *wr, u32 ms)
{
	ULONG sigs = SIGBREAKF_CTRL_C;
	struct st_tv tmo;
	LONG n;

	tmo.secs = ms / 1000;
	tmo.micro = (ms % 1000) * 1000;
	n = WaitSelect(nfds, rd, wr, NULL, &tmo, &sigs);
	if (sigs & SIGBREAKF_CTRL_C)
		return -1;
	if (n < 0)
		return Errno() == RT_EINTR ? -1 : -2;
	return (int)n;
}

static u32 a_now_us(void *ctx)
{
	return TimerBase ? cl_now_us() : 0;
}

static u32 a_unix_time(void *ctx)
{
	struct DateStamp ds;

	DateStamp(&ds);
	/* AmigaDOS counts from 1978-01-01 (2922 days after 1970-01-01), in
	 * local time: the system clock has no time zone */
	return ((u32)ds.ds_Days + 2922UL) * 86400UL + (u32)ds.ds_Minute * 60UL + (u32)ds.ds_Tick / 50UL;
}

static int a_brk(void *ctx)
{
	/* WaitSelect() need not report a Ctrl-C while data keeps arriving */
	return (CheckSignal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

static void a_raise_brk(void *ctx)
{
	Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
}

/* The CLI has one output stream: "stderr" text (errors, --debug) goes
 * there too, flushed at once so that it shows up in order. */
static void a_out(void *ctx, int to_stderr, const char *s, u32 n)
{
	static char b[257];

	while (n) {
		u32 k = n > 256 ? 256 : n;
		memcpy(b, s, k);
		b[k] = 0;
		cl_puts(b);
		s += k;
		n -= k;
	}
	if (to_stderr)
		cl_flush();
}

static void a_flush(void *ctx)
{
	cl_flush();
}

static void *a_alloc(void *ctx, u32 n)
{
	return AllocVec(n, MEMF_ANY | MEMF_CLEAR);
}

static void a_free(void *ctx, void *p)
{
	FreeVec(p);
}

static const char *a_strerror(void *ctx, int e)
{
	return st_strerror(e);
}

/* FIONWRITE (Riptide 0.15, FreeBSD): unacknowledged bytes; -1 on other stacks */
static int a_unsent(void *ctx, int fd)
{
	LONG v = 0;

	if (IoctlSocket(fd, RT_FIONWRITE, &v) < 0)
		return -1;
	return (int)v;
}

static const struct st_sys amiga_sys = {
	NULL, a_resolve, a_connect, a_conn_error, a_send, a_recv, a_close, a_wait,
	a_now_us, a_unix_time, a_brk, a_raise_brk, a_out, a_flush, a_alloc, a_free,
	a_strerror, 256, "AmigaOS; m68k", a_unsent
};

/* An interface with a TCP window limit (tcpwindow=, the workaround for
 * FS-UAE's frame drops on unpatched uaenet.device/slirp) caps downloads:
 * say so, or the result looks like a slow line. */
static void note_tcpwindow(int argc, char **argv)
{
	struct rt_ctl_hdr h;
	LONG mib[3], len = 0;
	u8 *d;
	u32 i;

	for (i = 1; i < (u32)argc; i++)
		if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "--csv") || !strcmp(argv[i], "--simple") ||
		    !strcmp(argv[i], "--list"))
			return;
	/* quietly: another stack has no riptide.library */
	if (!RiptideBase)
		RiptideBase = OpenLibrary((CONST_STRPTR)RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
	if (!RiptideBase)
		return;
	mib[0] = 4;             /* CTL_NET */
	mib[1] = RT_CTL_MIB_RIPTIDE;
	mib[2] = RT_CTL_IFACES;
	if (RiptideSysCtl(mib, 3, NULL, &len, NULL, 0) != 0)
		return;
	len += 1024;
	d = AllocVec((ULONG)len, MEMF_ANY | MEMF_CLEAR);
	if (!d)
		return;
	if (RiptideSysCtl(mib, 3, d, &len, NULL, 0) != 0 || (memcpy(&h, d, sizeof(h)), h.version != RT_CTL_VERSION)) {
		FreeVec(d);
		return;
	}
	for (i = 0; i < h.count; i++) {
		const struct rt_ctl_iface *f = cl_rec(d, &h, i);
		if ((f->flags & RT_IFF_UP) && !(f->flags & RT_IFF_LOOPBACK) && f->tcp_window)
			cl_printf("Note: %s has tcpwindow=%lu (the FS-UAE slirp workaround): downloads over it\n"
			          "      are limited to about one window per round trip. Remove it on a\n"
			          "      patched FS-UAE or another emulator.\n",
			          f->name, (unsigned long)f->tcp_window);
	}
	FreeVec(d);
}

static int speedtest_main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		/* these need no TCP/IP stack */
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "--csv-header") || !strcmp(argv[i], "-h") ||
		    !strcmp(argv[i], "--help"))
			return st_main(argc, argv, &amiga_sys);
	}
	if (cl_need_socket() < 0 || cl_need_timer() < 0)
		return CL_FAIL;
	note_tcpwindow(argc, argv);
	return st_main(argc, argv, &amiga_sys);
}

CL_MAIN(speedtest_main, "speedtest")
