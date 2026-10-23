/*
 * Riptide - netbench: TCP throughput client for the speed lab
 * (tests/amiga/run_fsuae_netbench.sh). Talks to tests/amiga/netbench_host.py.
 *
 *   netbench -d|-u [-n BYTES] [-b BUFSIZE] [-w RCVBUF] [-l LABEL] [-I] A.B.C.D PORT
 *   netbench -W SECONDS      wait until a stack's bsdsocket.library opens
 *   netbench -s SECONDS      sleep (the AROS ROM has no Wait command)
 *   netbench -A NAME=PATH    assign NAME: to PATH (-a: add to NAME:), for
 *                            boot disks without the Assign command (vbcc)
 *   netbench -R              add a do-nothing rexxsyslib.library (AmiTCP 3.0
 *                            refuses to start without one; the AROS ROM has
 *                            none; vbcc)
 *
 * -d downloads BYTES from the host and throws them away, -u uploads BYTES
 * of a pattern and waits until the host confirms it has them all. Prints
 *
 *   netbench: LABEL download 4194304 bytes 12.345678 s 331.79 KB/s
 *
 * With -I (vbcc build) it also prints how much CPU time was left over
 * ("cpu idle 12.3 %"): a task at priority -127 counts in a loop, first
 * for half a second before the connect (all of the CPU), then during the
 * transfer. The profiler of a stack cannot tell that apart from time its
 * own task sleeps while others run.
 *
 * timed from the first byte of the request to the last byte (download) or
 * the host's confirmation (upload); the connect is not timed. Uses only
 * bsdsocket.library version 3 calls (socket, connect, send, recv,
 * shutdown, setsockopt, CloseSocket), so it runs on any Amiga TCP/IP stack
 * (AmiTCP 3.0 and later, Miami, Roadshow, Riptide); nothing touches a disk.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "gen/bsdsocket_calls.h"

static char *buf;

#ifdef __VBCC__
/* -I: the idle counter task */
static volatile u32 idle_count, idle_stop, idle_gone;
static struct Task *idle_task;
static u8 *idle_stack;
#define IDLE_STACK 2048

static void idle_loop(void)
{
	while (!idle_stop)
		idle_count++;
	Forbid();               /* broken only by RemTask: gone means gone */
	idle_gone = 1;
}

static int idle_start(void)
{
	idle_task = AllocMem(sizeof(*idle_task), MEMF_PUBLIC | MEMF_CLEAR);
	idle_stack = AllocMem(IDLE_STACK, MEMF_PUBLIC | MEMF_CLEAR);
	if (!idle_task || !idle_stack)
		return -1;
	idle_task->tc_Node.ln_Type = NT_TASK;
	idle_task->tc_Node.ln_Pri = -127;
	idle_task->tc_Node.ln_Name = (char *)"netbench idle";
	idle_task->tc_SPLower = idle_stack;
	idle_task->tc_SPUpper = idle_stack + IDLE_STACK;
	idle_task->tc_SPReg = idle_stack + IDLE_STACK;
	AddTask(idle_task, (APTR)idle_loop, NULL);
	return 0;
}

static void idle_end(void)
{
	if (!idle_task)
		return;
	idle_stop = 1;
	while (!idle_gone)
		Delay(1);
	FreeMem(idle_stack, IDLE_STACK);
	FreeMem(idle_task, sizeof(*idle_task));
	idle_task = NULL;
}

/* counts per second of an otherwise idle CPU */
static u32 idle_calibrate(void)
{
	u32 c0 = idle_count, t0 = cl_now_us(), us;

	Delay(25);
	us = cl_now_us() - t0;
	return us / 1000u ? (u32)(((u64)(idle_count - c0) * 1000u) / (us / 1000u)) : 0;
}
#endif

static void usage(void)
{
	cl_puts("Usage: netbench -d|-u [-n BYTES] [-b BUFSIZE] [-w RCVBUF] [-l LABEL] [-I] A.B.C.D PORT\n"
	        "       netbench -W SECONDS | -s SECONDS | -A NAME=PATH | -a NAME=PATH | -R\n");
}

/* -A / -a: Assign NAME: PATH [ADD] without the Assign command */
static int do_assign(const char *spec, int add)
{
#ifdef __VBCC__
	char name[32];
	const char *eq = strchr(spec, '=');
	u32 l = eq ? (u32)(eq - spec) : 0;
	BPTR lock;
	LONG ok;

	if (!eq || !l || l >= sizeof(name)) {
		usage();
		return CL_ERROR;
	}
	memcpy(name, spec, l);
	name[l] = 0;
	lock = Lock((CONST_STRPTR)(eq + 1), ACCESS_READ);
	if (!lock) {
		cl_error("cannot lock %s", eq + 1);
		return CL_ERROR;
	}
	ok = add ? AssignAdd((CONST_STRPTR)name, lock) : AssignLock((CONST_STRPTR)name, lock);
	if (!ok) {
		UnLock(lock);
		cl_error("cannot assign %s:", name);
		return CL_ERROR;
	}
	return CL_OK;
#else
	(void)spec;
	(void)add;
	cl_error("-A/-a need the vbcc build");
	return CL_ERROR;
#endif
}

/*
 * -R: a stub rexxsyslib.library. The code has to outlive this command, so
 * it is copied into its own memory: Open (addq.w #1,lib_OpenCnt(a6);
 * move.l a6,d0; rts), Close (subq.w #1,lib_OpenCnt(a6); moveq #0,d0; rts)
 * and every other vector (moveq #0,d0; rts). Every Rexx call fails, which
 * only matters to a stack that gets an ARexx message.
 */
static int add_rexx_stub(void)
{
#ifdef __VBCC__
	static const u16 code[] = { 0x526e, 0x0020, 0x200e, 0x4e75,
	                            0x536e, 0x0020, 0x7000, 0x4e75,
	                            0x7000, 0x4e75 };
	static APTR vec[4 + 86 + 1];
	struct Library *lib;
	u8 *mem;
	int i;

	Forbid();
	lib = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)"rexxsyslib.library");
	Permit();
	if (lib)
		return CL_OK;
	mem = AllocMem(sizeof(code) + 24, MEMF_PUBLIC);
	if (!mem)
		return CL_ERROR;
	memcpy(mem, code, sizeof(code));
	strcpy((char *)mem + sizeof(code), "rexxsyslib.library");
	CacheClearU();
	vec[0] = mem;
	vec[1] = mem + 8;
	for (i = 2; i < 4 + 86; i++)
		vec[i] = mem + 16;
	vec[i] = (APTR)-1;
	lib = (struct Library *)MakeLibrary((CONST_APTR)vec, NULL, NULL, sizeof(struct Library) + 64, 0);
	if (!lib) {
		FreeMem(mem, sizeof(code) + 24);
		return CL_ERROR;
	}
	lib->lib_Node.ln_Type = NT_LIBRARY;
	lib->lib_Node.ln_Name = (char *)mem + sizeof(code);
	lib->lib_Version = 36;
	AddLibrary(lib);
	return CL_OK;
#else
	cl_error("-R needs the vbcc build");
	return CL_ERROR;
#endif
}

static int netbench_main(int argc, char **argv)
{
	struct cl_getopt g = { 1, 0, NULL };
	u32 total = 4u << 20, bufsize = 16384, rcvbuf = 0, addr, port, t0, t1, done = 0, us, wait = 0, pause = 0;
	const char *label = "-";
	int c, dir = 0, s, n;
	char req[48];
	struct rt_sockaddr_in sa;

	int idle = 0;
#ifdef __VBCC__
	u32 idle_rate = 0, idle_c0 = 0;
#endif

	while ((c = cl_getopt(argc, argv, "dun:b:w:l:s:W:A:a:RI", &g)) != -1) {
		switch (c) {
		case 'I': idle = 1; break;
		case 'R': return add_rexx_stub();
		case 'A': return do_assign(g.arg, 0);
		case 'a': return do_assign(g.arg, 1);
		case 's': if (cl_parse_uint(g.arg, 3600, &pause) < 0) { usage(); return CL_ERROR; } break;
		case 'W': if (cl_parse_uint(g.arg, 3600, &wait) < 0) { usage(); return CL_ERROR; } break;
		case 'd': dir = 'd'; break;
		case 'u': dir = 'u'; break;
		case 'n': if (cl_parse_uint(g.arg, 0x7fffffff, &total) < 0) { usage(); return CL_ERROR; } break;
		case 'b': if (cl_parse_uint(g.arg, 1u << 20, &bufsize) < 0 || !bufsize) { usage(); return CL_ERROR; } break;
		case 'w': if (cl_parse_uint(g.arg, 1u << 24, &rcvbuf) < 0) { usage(); return CL_ERROR; } break;
		case 'l': label = g.arg; break;
		default: usage(); return CL_ERROR;
		}
	}
	if (pause) {
		Delay(pause * 50);
		return CL_OK;
	}
	if (wait) {
		/* any version: stacks publish bsdsocket.library 3 (AmiTCP 3) or 4 */
		for (us = 0; us < wait * 10u; us++) {
			SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
			if (SocketBase)
				break;
			Delay(5);
		}
		if (!SocketBase) {
			cl_error("no bsdsocket.library after %lu s", (unsigned long)wait);
			return CL_FAIL;
		}
		cl_printf("netbench: stack ready: %s\n", SocketBase->lib_IdString ?
		          (const char *)SocketBase->lib_IdString : "bsdsocket.library");
		CloseLibrary(SocketBase);
		SocketBase = NULL;
		return CL_OK;
	}
	if (!dir || argc - g.ind != 2 || cl_parse_ip(argv[g.ind], &addr) < 0 ||
	    cl_parse_uint(argv[g.ind + 1], 65535, &port) < 0) {
		usage();
		return CL_ERROR;
	}
	/* version 3: AmiTCP 3.0's bsdsocket.library is 3.28 */
	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 3);
	if (!SocketBase) {
		cl_error("cannot open bsdsocket.library");
		return CL_FAIL;
	}
	if (cl_need_timer() < 0)
		return CL_FAIL;
	buf = AllocVec(bufsize, MEMF_ANY);
	if (!buf) {
		cl_error("no memory for a %lu byte buffer", (unsigned long)bufsize);
		return CL_FAIL;
	}
	for (n = 0; n < (int)bufsize; n++)
		buf[n] = (char)('a' + n % 26);
	s = socket(RT_AF_INET, RT_SOCK_STREAM, 0);
	if (s < 0) {
		cl_error("socket() failed");
		return CL_FAIL;
	}
	if (rcvbuf) {
		LONG v = (LONG)rcvbuf;
		setsockopt(s, RT_SOL_SOCKET, RT_SO_RCVBUF, &v, sizeof(v));
	}
#ifdef __VBCC__
	if (idle && idle_start() == 0)
		idle_rate = idle_calibrate();
#else
	(void)idle;
#endif
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons((u16)port);
	sa.sin_addr = addr;
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		cl_error("connect() failed");
		CloseSocket(s);
#ifdef __VBCC__
		idle_end();
#endif
		return CL_FAIL;
	}
	n = rt_snprintf(req, sizeof(req), "%c %lu\n", dir == 'd' ? 'D' : 'U', (unsigned long)total);
#ifdef __VBCC__
	idle_c0 = idle_count;
#endif
	t0 = cl_now_us();
	if (send(s, req, n, 0) != n) {
		cl_error("send() failed");
		CloseSocket(s);
#ifdef __VBCC__
		idle_end();
#endif
		return CL_FAIL;
	}
	if (dir == 'd') {
		while (done < total) {
			n = recv(s, buf, (LONG)bufsize, 0);
			if (n <= 0)
				break;
			done += (u32)n;
		}
	} else {
		while (done < total) {
			u32 k = total - done < bufsize ? total - done : bufsize;
			n = send(s, buf, (LONG)k, 0);
			if (n <= 0)
				break;
			done += (u32)n;
		}
		shutdown(s, 1);
		/* the host answers "OK <bytes>" once it has read them all */
		while ((n = recv(s, req, sizeof(req) - 1, 0)) > 0)
			;
	}
	t1 = cl_now_us();
#ifdef __VBCC__
	if (idle_rate) {
		u32 ms = (t1 - t0) / 1000u, c = idle_count - idle_c0;
		u32 p10 = ms ? (u32)(((u64)c * 100000u) / ((u64)idle_rate * ms)) : 0;   /* tenths of % */
		cl_printf("netbench: %s cpu idle %lu.%lu %%\n", label, (unsigned long)(p10 / 10u),
		          (unsigned long)(p10 % 10u));
	}
	idle_end();
#endif
	CloseSocket(s);
	us = t1 - t0;
	if (!us)
		us = 1;
	/* KB/s with two decimals in 32-bit arithmetic */
	{
		u32 kbs100 = (u32)(((done / 1024u) * 100000u) / (us / 1000u ? us / 1000u : 1u));
		cl_printf("netbench: %s %s %lu bytes %lu.%06lu s %lu.%02lu KB/s\n", label,
		          dir == 'd' ? "download" : "upload", (unsigned long)done,
		          (unsigned long)(us / 1000000u), (unsigned long)(us % 1000000u),
		          (unsigned long)(kbs100 / 100u), (unsigned long)(kbs100 % 100u));
	}
	FreeVec(buf);
	CloseLibrary(SocketBase);
	SocketBase = NULL;
	return done == total ? CL_OK : CL_WARN;
}

CL_MAIN(netbench_main, "netbench")
