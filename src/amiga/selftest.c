/*
 * Riptide - "Riptide SELFTEST": single-task test of the library layer,
 * made for the vamos emulator (which has no multitasking, Wait(), Signal()
 * or working timer.device). It creates the three libraries with
 * MakeLibrary()/AddLibrary(), opens them through their Open vectors (exec's
 * OpenLibrary() when it can find them, else LVO -6 directly) and runs the
 * shared checks in tests/amiga/rtcheck_body.h through the real jump tables
 * with non-blocking sockets, running rt_stack_poll() in between on a
 * virtual clock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

struct Library *SocketBase, *MiamiBase, *UserGroupBase;

#include "gen/bsdsocket_calls.h"
#include "gen/miami_calls.h"
#include "gen/usergroup_calls.h"

static void pump(void)
{
	rt_lock();
	rt_stack_poll();
	rt_unlock();
	amiga_virtual_ms += 10;
	amiga_log_flush();
}

#define RTC_LOG(...) do { amiga_out(__VA_ARGS__); amiga_out("\n"); } while (0)
#define RTC_PUMP() pump()
#define RTC_BLOCKING 0
#include "../../tests/amiga/rtcheck_body.h"

/* Call a library's Open (-6) / Close (-12) vector like exec does. */
#if defined(__VBCC__)
struct Library *__lib_open(__reg("a6") void *, __reg("d0") ULONG) = "\tjsr\t-6(a6)";
ULONG __lib_close(__reg("a6") void *) = "\tjsr\t-12(a6)";
#define lib_open(b, v) __lib_open((b), (v))
#define lib_close(b) __lib_close((b))
#else
static inline struct Library *lib_open(struct Library *b, ULONG v)
{
	register unsigned long d0 __asm("d0") = v;
	register unsigned long d1 __asm("d1"), a0 __asm("a0"), a1 __asm("a1");
	__asm volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -6(%%a6)\n\tmove.l (%%sp)+,%%a6"
	                : "+r"(d0), "=r"(d1), "=r"(a0), "=r"(a1) : [b] "a"(b) : "cc", "memory");
	return (struct Library *)d0;
}
static inline ULONG lib_close(struct Library *b)
{
	register unsigned long d0 __asm("d0"), d1 __asm("d1"), a0 __asm("a0"), a1 __asm("a1");
	__asm volatile ("move.l %%a6,-(%%sp)\n\tmove.l %[b],%%a6\n\tjsr -12(%%a6)\n\tmove.l (%%sp)+,%%a6"
	                : "=r"(d0), "=r"(d1), "=r"(a0), "=r"(a1) : [b] "a"(b) : "cc", "memory");
	return d0;
}
#endif

static int used_exec_open;

static struct Library *open_lib(const char *name, ULONG version)
{
	struct Library *b, *m;

	b = OpenLibrary((CONST_STRPTR)name, version);
	if (b && b->lib_Node.ln_Name && !strcmp(b->lib_Node.ln_Name, name)) {
		used_exec_open = 1;
		return b;
	}
	Forbid();
	m = (struct Library *)FindName(exec_liblist(SysBase), (CONST_STRPTR)name);
	Permit();
	if (!m) {
		amiga_out("FAIL %s is not in the exec library list\n", name);
		return NULL;
	}
	if (m->lib_Version < version)
		return NULL;
	return lib_open(m, version);
}

static void close_lib(struct Library *b)
{
	if (!b)
		return;
	if (used_exec_open)
		CloseLibrary(b);
	else
		lib_close(b);
}

int amiga_selftest(void)
{
	static const char *const conf[] = {
		"hostname selftest",
		"host localhost 127.0.0.1",
		"user user uid=0 gid=0 home=SYS:",
		"sysctl net.inet.tcp.sack=1",
	};
	char err[128];
	unsigned i;
	int fails = 0, r;
	struct Library *again;

	amiga_out("Riptide self test (single task, virtual clock)\n");
	rt_sock_async_hook = amiga_async_hook;
	if (rt_init() < 0) {
		amiga_out("FAIL rt_init\n");
		return 20;
	}
	for (i = 0; i < RT_ARRAY_SIZE(conf); i++) {
		rt_lock();
		r = rt_config_line(conf[i], err, sizeof(err), amiga_config_unknown);
		rt_unlock();
		if (r < 0) {
			amiga_out("note: config '%s': %s\n", conf[i], err);
			if (i < 3)
				fails++;
		}
	}
	if (amiga_libs_init() < 0) {
		amiga_out("FAIL amiga_libs_init (MakeLibrary/AddLibrary)\n");
		return 20;
	}
	if (!amiga_libs_present()) {
		amiga_out("FAIL libraries not found with FindName()\n");
		fails++;
	}
	SocketBase = open_lib("bsdsocket.library", 4);
	MiamiBase = open_lib("miami.library", 21);
	UserGroupBase = open_lib("usergroup.library", 4);
	amiga_out("opened through %s: bsdsocket %s, miami %s, usergroup %s\n",
	          used_exec_open ? "exec OpenLibrary()" : "the LVO -6 Open vector",
	          SocketBase ? "ok" : "FAILED", MiamiBase ? "ok" : "FAILED", UserGroupBase ? "ok" : "FAILED");
	if (!SocketBase)
		return 20;
	if (!MiamiBase || !UserGroupBase)
		fails++;
	again = open_lib("bsdsocket.library", 4);
	if (again != SocketBase || SocketBase->lib_OpenCnt != 2) {
		amiga_out("FAIL second open from the same task must return the same base\n");
		fails++;
	}
	close_lib(again);
	if (SocketBase->lib_Version != 4 || amiga_libs_users() != 3) {
		amiga_out("FAIL version/open count (%d users)\n", amiga_libs_users());
		fails++;
	}
	fails += rtcheck_run();
	if (amiga_libs_remove() == 0) {
		amiga_out("FAIL libraries removed while open\n");
		fails++;
	}
	close_lib(UserGroupBase);
	close_lib(MiamiBase);
	close_lib(SocketBase);
	if (amiga_libs_users() != 0) {
		amiga_out("FAIL %d users after closing\n", amiga_libs_users());
		fails++;
	}
	if (amiga_libs_remove() != 0 || amiga_libs_present()) {
		amiga_out("FAIL libraries not removed\n");
		fails++;
	}
	amiga_log_flush();
	amiga_out("SELFTEST %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
	return fails ? 10 : 0;
}
