/*
 * Riptide - internal interfaces of the AmigaOS glue (src/amiga).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_PRIV_H
#define RT_AMIGA_PRIV_H

#include "amiga_os.h"
#include "amiga_net.h"
#include "rt/riptide.h"

#include "rt/riptide_port.h"
#define RIPTIDE_DATE     "20.10.2026"

/* ---- os_amiga.c ---------------------------------------------------- */

struct rt_sleeper {
	struct MinNode node;
	struct Task   *task;
	ULONG          sigmask;      /* wakeup signal */
	BYTE           sigbit;
	BYTE           tsigbit;      /* timer reply port signal */
	UBYTE          timer_open;
	volatile UBYTE woken;        /* sticky wakeup, protected by the stack lock */
	const ULONG   *breakmask;    /* ctx->sigintr_mask, or NULL */
	struct MsgPort tport;
	struct timerequest treq;
};

/* Initialise a sleeper for the CALLING task (allocates two signals and
 * opens timer.device). Returns 0 or -errno. */
int  amiga_sleeper_init(struct rt_sleeper *s, const ULONG *breakmask);
/* Must be called by the owning task. */
void amiga_sleeper_fini(struct rt_sleeper *s);

/* Called once by the stack process before anything else. */
int  amiga_os_init(void);
void amiga_os_fini(void);
extern struct Task *amiga_stack_task;     /* the Riptide process */
extern ULONG amiga_kick_mask;             /* rt_kick() signal */
/* Write buffered log lines to Output(). Only the stack process does DOS I/O. */
void amiga_log_flush(void);
extern int amiga_log_to_stdout;
/* Virtual clock for the vamos self test (vamos has no working timer.device). */
extern int amiga_virtual_clock;
extern u32 amiga_virtual_ms;
/* Format with the core's rt_vsnprintf and log. */
void amiga_out(const char *fmt, ...);     /* stack process only: Write(Output()) */

/* ---- libs.c: library construction ---------------------------------- */

/* Per-task context shared by bsdsocket.library, riptide.library and its
 * compat alias (the historical stack does the same, docs/RE-NOTES.md 2.2). */
struct TaskCtx {
	struct MinNode node;
	struct Task   *task;
	struct rt_sockctx sc;
	struct rt_sleeper sl;
	int    nbases;               /* per-task bases using this context */
	LONG   errno_;
	LONG   herrno;
	APTR   errnoptr;
	LONG   errnosize;
	LONG  *herrnoptr;
	/* syslog */
	LONG   logstat, logfacility, logmask;
	STRPTR logtag;
	/* fd callback as installed by the application (register d0/d1) */
	APTR   fdcb;
	LONG   fdcb_cpu;
	ULONG  online_sigmask;
	/* per-opener result buffers of the BSD netdb calls */
	char   ntoa[20];
	struct {
		struct hostent he;
		char  *aliases[2];
		char  *addrs[9];
		ULONG  addr[8 * 4];      /* 8 IPv4 or 8 IPv6 addresses */
		char   name[256];
	} host;
	struct {
		struct servent se;
		char  *aliases[5];
		char   name[32];
		char   proto[8];
	} serv;
	struct {
		struct protoent pe;
		char  *aliases[5];
	} proto;
	struct {
		struct netent ne;
		char  *aliases[1];
	} net;
	char   ifname[RT_IFNAMSIZ];
};

/* Base of a per-task library (bsdsocket, riptide, its alias, usergroup) */
struct RtBase {
	struct Library lib;
	struct TaskCtx *ctx;         /* at +0x22 like the historical stack */
	struct RtLibDef *def;
	struct MinNode link;         /* on def->bases */
	struct Task *task;
	APTR   priv;                 /* library specific (usergroup state) */
};

struct RtLibDef {
	const char *name;
	const char *idstring;
	UWORD version, revision;
	const ULONG *vectors;
	ULONG possize;               /* >= sizeof(struct RtBase) */
	int   needs_ctx;             /* bsdsocket/riptide: attach a TaskCtx */
	int   (*init)(struct RtBase *b);
	void  (*fini)(struct RtBase *b);
	struct Library *master;
	struct MinList bases;
};

extern struct RtLibDef rtlib_bsdsocket, rtlib_riptide, rtlib_compat, rtlib_usergroup;
extern volatile int amiga_libs_closing;   /* refuse new opens */
/* "compat aliases on|off": publish the compat alias library (default on) */
extern int amiga_compat_aliases;
/* Create and AddLibrary() the masters (the alias only when
 * amiga_compat_aliases is set). 0 or -1. */
int  amiga_libs_init(void);
/* Add or remove the compat alias while the libraries exist (lock held).
 * 0, or -RT_EBUSY if programs still have the alias open. */
int  amiga_libs_set_compat(int on, char *err, u32 errlen);
/* One line naming the published libraries and the alias state. */
void amiga_libs_summary(char *buf, u32 len);
/* Number of open per-task bases of all libraries. */
int  amiga_libs_users(void);
/* Remove the masters from the system list if nobody has them open.
 * Returns 0 on success, -1 if users remain (nothing changed). */
int  amiga_libs_remove(void);
/* Find (or not) the TaskCtx of the calling task. */
struct TaskCtx *amiga_ctx_find(struct Task *t);
int  amiga_ctx_valid(struct TaskCtx *c);    /* under Forbid() */
/* The per-task generic open/close used by the Open/Close vectors. */
struct Library *rtlib_open(struct RtLibDef *def, struct Library *master);
ULONG rtlib_close(struct RtBase *b);
static inline struct TaskCtx *base_ctx(struct Library *base)
{
	return ((struct RtBase *)base)->ctx;
}

/* bsdsocket.c */
int  bsd_ctx_init(struct TaskCtx *c);    /* default errno state etc. */
void bsd_set_errno(struct TaskCtx *c, LONG e);
void amiga_async_hook(struct rt_socket *so, u32 events);
const char *amiga_herror_str(LONG h);
const char *amiga_s2err_str(LONG e);
const char *amiga_s2werr_str(LONG e);
const char *amiga_ioerr_str(LONG e);
/* Fill the per-task hostent from a name lookup result. */
struct hostent *bsd_make_hostent(struct TaskCtx *c, const char *name, const u32 *addrs, int n);
struct hostent *bsd_make_hostent6(struct TaskCtx *c, const char *name, const rt_in6_addr *addrs, int n);

/* Call an application callback taking d0/d1 (fd callback). */
LONG amiga_call_d0d1(APTR fn, LONG d0, LONG d1);
int  amiga_inet_aton(const char *cp, u32 *addr);
STRPTR amiga_ntoa(u32 in_net, char *buf);
void amiga_set_fdcb(struct TaskCtx *c, APTR fn, LONG cpu);
/* libs.c */
int  amiga_libs_present(void);          /* any TCP/IP stack library in the system? */
void amiga_libs_describe(char *buf, unsigned len); /* which ones, with versions */
void amiga_libs_signal_users(ULONG sigs);
/* os_amiga.c: append a line to the log regardless of rt_log_level */
void amiga_log_line(const char *line);
/* usergroup.c: the configured user */
extern char amiga_user_name[32], amiga_user_home[64], amiga_user_realname[64];
extern LONG amiga_user_uid, amiga_user_gid;

/* ---- interface drivers (sana2.c, mni.c) ---------------------------- */
struct amiga_if {
	struct amiga_if *next;
	struct rt_netif nif;
	ULONG  sigmask;              /* stack process signals the driver uses */
	/* lock held: process completed I/O */
	void (*poll)(struct amiga_if *ai);
	/* lock held: online/offline request (RiptideOnOffline) */
	void (*set_online)(struct amiga_if *ai, int online);
	/* lock NOT held: stop and free everything */
	void (*close)(struct amiga_if *ai);
	int  (*status)(struct amiga_if *ai, char *buf, u32 len);
	int  online;
	void *drv;
};
extern struct amiga_if *amiga_ifs;
/* Config keywords handled by the Amiga glue ('unknown' callback of
 * rt_config_line): "interface NAME device=... | mni=..." and "user ...". */
int  amiga_config_unknown(int argc, char **argv, char *err, u32 errlen);
#define AMIGA_STACK_PRI 2   /* default exec priority of the Riptide process */
int  sana2_open_if(const char *name, const char *device, ULONG unit, int mtu, int raw, int reads,
                   char *err, u32 errlen);
int  mni_open_if(const char *name, const char *driver, ULONG unit, const char *options,
                 int mtu, char *err, u32 errlen);
struct amiga_if *amiga_if_find(const char *name);
/* file part of an AmigaDOS path ("DEVS:Networks/a2065.device" -> "a2065.device") */
static inline const char *amiga_basename(const char *p)
{
	const char *b = p;

	for (; *p; p++)
		if (*p == '/' || *p == ':')
			b = p + 1;
	return b;
}
/* "interface NAME delete" (lock held) */
int  amiga_if_delete(const char *name, char *err, u32 errlen);

/* ---- stackproc.c --------------------------------------------------- */
int  amiga_stack_start(const char *config, int selftest);
void amiga_stack_loop(void);
void amiga_stack_stop(void);
/* Public port protocol: include/rt/riptide_port.h */
int  amiga_status_text(char *buf, u32 len);

/* ---- selftest.c ---------------------------------------------------- */
int  amiga_selftest(void);
int  amiga_bench(void);                 /* "Riptide BENCH" (bench.c) */

#endif
