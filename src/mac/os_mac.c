/* src/mac/os_mac.c - Riptide on the classic Mac OS: the OS layer (include/rt/rt_os.h).
 *
 * The Mac has one processor and no threads. Every call into the protocol core happens in one
 * context, the stack's deferred task (Deferred Task Manager: interrupt level 0, interrupts on,
 * after the interrupt handlers), on the stack it interrupted. The Time Manager task, the
 * .ENET protocol handler and the .IPP driver's Control routine only queue work and ask for that
 * task (mac_schedule). So the core's lock is a nesting counter, the wait queues never block (the
 * MacTCP interface uses non-blocking calls and completes parameter blocks later), and memory
 * comes from a pool allocated at Open, because the Memory Manager may not be called at
 * interrupt time.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdarg.h>
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_ctl.h"
#include "rt_diag.h"
#include "riptide.h"
#include "mac.h"

/* ---- the lock: one context, so only the nesting is kept ---------------------------------- */
static int lock_depth;
void rt_lock(void) { lock_depth++; }
void rt_unlock(void) { if (lock_depth > 0) lock_depth--; }
int  rt_lock_held(void) { return lock_depth > 0; }
int  rt_lock_wanted(void) { return 0; }
int  rt_lock_yield(void) { return 0; }

/* ---- time ----------------------------------------------------------------------------------- */
static int have_us;
u64 rt_now_us(void)
{
	if (have_us)
		return mac_microseconds();
	return (u64)mac_ticks() * 16667u;               /* System 6: the 60.15 Hz tick */
}
u32 rt_now_ms(void) { return (u32)(rt_now_us() / 1000u); }
#define MAC_EPOCH_TO_UNIX 2082844800u                 /* 1904 to 1970 */
u32 rt_wallclock(void)
{
	u32 t = *(volatile u32 *)0x20C;                /* Time: local seconds since 1904 */
	return t > MAC_EPOCH_TO_UNIX ? t - MAC_EPOCH_TO_UNIX : 0;
}
void rt_wallclock_tv(u32 *sec, u32 *usec) { *sec = rt_wallclock(); *usec = 0; }

/* ---- memory: a first-fit pool with boundary coalescing -------------------------------------- */
struct blk { u32 size; struct blk *next; };           /* size includes this header; free list by address */
#define HDR 8u
static u8 *pool; static u32 pool_size, pool_free;
static struct blk *free_list;
void *rt_malloc(size_t n)
{
	u32 want = ((u32)n + HDR + 7u) & ~7u;
	u16 sr = mac_irq_off();
	struct blk **pp = &free_list, *b;
	for (b = free_list; b; pp = &b->next, b = b->next) {
		if (b->size < want)
			continue;
		if (b->size - want >= 32) {               /* split: the tail stays free */
			struct blk *r = (struct blk *)((u8 *)b + want);
			r->size = b->size - want; r->next = b->next;
			*pp = r;
			b->size = want;
		} else
			*pp = b->next;
		pool_free -= b->size;
		b->next = (struct blk *)0x52495054u;      /* 'RIPT': in use */
		mac_irq_restore(sr);
		return (u8 *)b + HDR;
	}
	mac_irq_restore(sr);
	return NULL;
}
void *rt_zalloc(size_t n) { void *p = rt_malloc(n); if (p) memset(p, 0, n); return p; }
void rt_free(void *p)
{
	if (!p)
		return;
	struct blk *b = (struct blk *)((u8 *)p - HDR), **pp = &free_list, *n;
	if ((u32)b->next != 0x52495054u)
		return;                                  /* not ours, or freed twice */
	u16 sr = mac_irq_off();
	pool_free += b->size;
	while ((n = *pp) && n < b)
		pp = &n->next;
	b->next = n;
	*pp = b;
	if (n && (u8 *)b + b->size == (u8 *)n) { b->size += n->size; b->next = n->next; }
	if (pp != &free_list) {
		struct blk *prev = (struct blk *)((u8 *)pp - (u32)&((struct blk *)0)->next);
		if ((u8 *)prev + prev->size == (u8 *)b) { prev->size += b->size; prev->next = b->next; }
	}
	mac_irq_restore(sr);
}
u32 rt_os_mem_avail(int *chip_only) { if (chip_only) *chip_only = 0; return pool_free; }
void rt_os_program_name(char *buf, u32 len) { if (len) buf[0] = 0; }

/* ---- random numbers ------------------------------------------------------------------------ */
static u32 jitter_count;
u32 rt_os_jitter(void) { return (u32)mac_microseconds() ^ mac_ticks() ^ ++jitter_count; }
u32 rt_random32(void) { u32 x; rt_random_bytes(&x, sizeof x); return x; }
void rt_random_bytes(void *buf, size_t n) { u16 sr = mac_irq_off(); rt_rng_bytes(buf, (u32)n); mac_irq_restore(sr); }

/* ---- files: none (packet captures are not offered on the Mac) ------------------------------ */
void *rt_file_create(const char *path) { (void)path; return NULL; }
int   rt_file_write(void *f, const void *p, u32 n) { (void)f; (void)p; (void)n; return -1; }
int   rt_file_close(void *f) { (void)f; return -1; }

/* ---- the log: a ring in memory (the Riptide control panel shows it) ------------------------ */
int rt_log_level = RT_LOG_INFO;
static char logbuf[8192]; static u32 log_pos, log_len;
#ifdef AURORA68_LOG
/* development on Aurora68 (a Raspberry Pi in the 68000 socket): every log line also goes to the
   kernel's log through the slot-9 mailbox's INFO command. Not for other Macs: the address is
   Aurora68's card, which a real Mac does not have. */
static void a68_log(const char *s)
{
	volatile u8 *c = (volatile u8 *)((*(volatile u8 *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
	u32 n = (u32)strlen(s), seq;
	*(volatile u16 *)(c + 8) = 2;
	*(const char *volatile *)(c + 28) = s;
	*(volatile u32 *)(c + 32) = n;
	seq = *(volatile u32 *)c + 1;
	*(volatile u32 *)c = seq;
	for (u32 i = 0; i < 200000u && *(volatile u32 *)(c + 4) != seq; i++) ;
}
#endif
void mac_log_line(const char *s)
{
#ifdef AURORA68_LOG
	{ char t[220]; rt_snprintf(t, sizeof t, "Riptide: %s", s); a68_log(t); }
#endif
	u16 sr = mac_irq_off();
	for (; *s; s++) { logbuf[log_pos] = *s; log_pos = (log_pos + 1) % sizeof logbuf; if (log_len < sizeof logbuf) log_len++; }
	logbuf[log_pos] = '\r'; log_pos = (log_pos + 1) % sizeof logbuf; if (log_len < sizeof logbuf) log_len++;
	mac_irq_restore(sr);
}
const char *mac_log_ring(u32 *len, u32 *pos) { *len = log_len; *pos = log_pos; return logbuf; }
void rt_log(int level, const char *fmt, ...)
{
	char line[200];
	va_list ap;
	if (level > rt_log_level)
		return;
	va_start(ap, fmt);
	rt_vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	mac_log_line(line);
}

/* ---- profiling and the CPU meter: the tick is the only clock everywhere -------------------- */
u32 rt_prof_ticks(void) { return (u32)rt_now_us(); }
u32 rt_prof_hz(void) { return 1000000u; }
static u32 busy_us, idle_us;
void rt_cpu_counts(u32 *now, u32 *idle, u32 *hz) { *now = busy_us + idle_us; *idle = idle_us; *hz = 1000000u; }

/* ---- waiting: never (the Mac layer only uses non-blocking calls) --------------------------- */
struct rt_sleeper { int woken; };
static struct rt_sleeper the_sleeper;
volatile int mac_wakeups;                          /* ipp.c rescans its pending calls when this moves */
struct rt_sleeper *rt_sleeper_self(void) { return &the_sleeper; }
void rt_waitq_add(struct rt_waitq *wq, struct rt_wait_entry *e, struct rt_sleeper *s)
{
	e->sleeper = s; e->prev = NULL; e->next = wq->head;
	if (wq->head) wq->head->prev = e;
	wq->head = e;
}
void rt_waitq_remove(struct rt_waitq *wq, struct rt_wait_entry *e)
{
	if (e->prev) e->prev->next = e->next; else if (wq->head == e) wq->head = e->next;
	if (e->next) e->next->prev = e->prev;
	e->next = e->prev = NULL;
}
void rt_waitq_wake(struct rt_waitq *wq)
{
	for (struct rt_wait_entry *e = wq->head; e; e = e->next) if (e->sleeper) e->sleeper->woken = 1;
	mac_wakeups++;
}
int rt_sleeper_wait(struct rt_sleeper *s, s32 timeout_ms, u32 *extra_signals)
{
	(void)timeout_ms;
	if (extra_signals) *extra_signals = 0;
	if (s->woken) { s->woken = 0; return 0; }
	return -RT_ETIMEDOUT;                        /* a blocking call here would hang the Mac: never wait */
}

/* ---- the stack's context ------------------------------------------------------------------- */
/* Deferred Task Manager record (Inside Macintosh: Processes): qLink, qType 7, dtFlags, dtAddr,
   dtParam, dtReserved. Time Manager extended task: qLink, qType, tmAddr, tmCount, tmWakeUp,
   tmReserved. */
struct dtask { void *qlink; s16 qtype; s16 flags; void *addr; void *param; u32 reserved; };
struct tmtask { void *qlink; s16 qtype; void *addr; s32 count; u32 wakeup; u32 reserved; };
static struct dtask dt;
static struct tmtask tm;
static volatile u8 dt_queued, in_stack, again;
u32 mac_stack_top;
void mac_schedule(void)
{
	u16 sr = mac_irq_off();
	if (in_stack)
		again = 1;
	else if (!dt_queued) {
		dt_queued = 1;
		mac_dtinstall(&dt);
	}
	mac_irq_restore(sr);
}
void rt_kick(void) { mac_schedule(); }
void mac_stack_run(void)
{
	u16 sr = mac_irq_off();
	dt_queued = 0; in_stack = 1;
	mac_irq_restore(sr);
	u64 t0 = rt_now_us();
	u32 ms;
	for (int rounds = 0; ; rounds++) {
		again = 0;
		net_mac_poll();
		ipp_poll();
		rt_lock();
		ms = rt_stack_poll();
		rt_unlock();
		if (!again || rounds > 8)
			break;
	}
	busy_us += (u32)(rt_now_us() - t0);
	if (ms < 5) ms = 5;
	if (ms > 100) ms = 100;
	mac_primetime(&tm, (s32)ms);                 /* the next timer, at the latest */
	sr = mac_irq_off();
	in_stack = 0;
	int more = again;
	mac_irq_restore(sr);
	if (more)
		mac_schedule();
}
int mac_os_init(u32 heap_bytes)
{
	have_us = *(volatile u16 *)0x15A >= 0x0700;  /* SysVersion: Microseconds is System 7's */
	pool = mac_newptr_sys(heap_bytes);
	if (!pool)
		return -RT_ENOMEM;
	pool_size = pool_free = heap_bytes & ~7u;
	free_list = (struct blk *)pool;
	free_list->size = pool_size; free_list->next = NULL;
	/* no stack of its own: the stack sniffer (a VBL task) compares SP with the application heap,
	   and a stack in the system heap, below it, reads as "stack collision with heap" (System
	   Error 28, the first Mac boot). The core keeps its frames small (docs/DESIGN.md), so the
	   deferred task runs on the stack it interrupted. mac_stack_top stays 0. */
	{ u32 seed[4] = { rt_os_jitter(), mac_ticks(), *(volatile u32 *)0x20C, (u32)pool }; rt_rng_stir(seed, sizeof seed); }
	dt.qtype = 7; dt.addr = (void *)mac_dt_entry; dt.param = NULL;
	tm.qtype = 0; tm.addr = (void *)mac_tm_entry;
	mac_insxtime(&tm);
	return 0;
}
