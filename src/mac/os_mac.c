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
#include "rt_netsvc.h"
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
#define MAC_TIME (*(volatile u32 *)0x20C)             /* Time: local seconds since 1904 */

/* The wall clock. The Mac keeps local time in whole seconds (Time, moved on by the clock chip's
   one-second interrupt); the core wants UTC with microseconds (NTP's offsets). UTC is local time
   less rt_tz_offset: the Map control panel's time zone (mac_tz_read) unless riptide.conf has
   "timezone". The fraction is measured: the stack's task looks at Time at least every 100 ms
   (mac_clock_sample) and notes when it moved on by one; the time since then is the fraction, to
   within half the gap between two looks. Until a tick has been seen it is taken as 0.5 s. */
static volatile u32 sub_time, sub_prev_us, sub_us;
static volatile u8 sub_ok;
static int clock_phase(u32 now, u32 *frac)
{
	u32 k = MAC_TIME - sub_time, d;
	if (!sub_ok || k > 1)
		return 0;
	d = now - sub_us - k * 1000000u;                /* k = 1: Time moved on since the last look */
	*frac = (s32)d < 0 ? 0 : d > 999999u ? 999999u : d;
	return 1;
}
void rt_wallclock_tv(u32 *sec, u32 *usec)
{
	u32 t = MAC_TIME, us = 500000u;
	clock_phase((u32)rt_now_us(), &us);
	*sec = t > MAC_EPOCH_TO_UNIX ? t - MAC_EPOCH_TO_UNIX - (u32)rt_tz_offset : 0;
	*usec = us;
}
u32 rt_wallclock(void) { u32 s, u; rt_wallclock_tv(&s, &u); return s; }

/* Setting the clock (the core's rt_set_wallclock_hook, from NTP in the stack's task). SetDateTime
   talks to the clock chip, which is not done at interrupt or deferred task time: the hook only
   notes the time, and the driver's accRun (a program's context, every sixth of a second: ipp.c)
   calls mac_clock_run, which sets it just after the chip's next tick (so that the new seconds
   start where the chip's seconds start: within 0.2 s), and in whole seconds, rounded. The result
   is logged from the stack's task. The core steps only beyond "ntp step" (1 s on the Mac). */
static volatile u8 set_pending, set_done, tz_news;
static u32 set_sec, set_usec, set_at_us, set_local;
static s32 set_diff;
static int mac_set_clock(u32 sec, u32 usec)
{
	u16 sr = mac_irq_off();
	set_sec = sec;
	set_usec = usec;
	set_at_us = (u32)rt_now_us();
	set_pending = 1;
	mac_irq_restore(sr);
	return 0;
}

/* The Map control panel's MachineLocation (12 bytes of extended parameter RAM at $E4, what
   ReadLocation reads): latitude, longitude, then gmtDelta, the seconds east of UTC in 24 bits
   (with daylight saving time in them when its box is ticked), under dlsDelta, whose top bit says
   daylight saving time is on. All zero: Map never set; local time is then taken as UTC. */
static s32 tz_delta;
static u8 tz_dst, tz_known, tz_seen;
void mac_tz_read(void)
{
	u8 loc[12];
	u32 g;
	s32 d;
	int i, any = 0;
	memset(loc, 0, sizeof loc);
	if (mac_readxpram(loc, 0x000C00E4u) != 0)
		return;
	for (i = 0; i < 12; i++)
		any |= loc[i];
	g = (u32)loc[9] << 16 | (u32)loc[10] << 8 | loc[11];
	d = g & 0x800000u ? (s32)(g | 0xFF000000u) : (s32)g;
	if (tz_seen && d == tz_delta && tz_known == (any != 0) && tz_dst == (loc[8] >> 7))
		return;
	tz_delta = d;
	tz_known = any != 0;
	tz_dst = loc[8] >> 7;
	tz_seen = 1;
	rt_tz_system = d;
	if (!rt_tz_configured)
		rt_tz_offset = d;
	tz_news = 1;
}

/* accRun (ipp.c): a program's context, where the clock chip may be used */
void mac_clock_run(void)
{
	static u32 tz_ticks;
	u32 now, age, frac = 0, secs, local;
	s32 us;
	if (!tz_ticks || mac_ticks() - tz_ticks >= 3600u) {        /* Map's settings, once a minute */
		mac_tz_read();
		tz_ticks = mac_ticks() | 1;
	}
	if (!set_pending)
		return;
	now = (u32)rt_now_us();
	age = now - set_at_us;
	if (age > 600000000u) {                     /* ten minutes without accRun: too old to use */
		set_pending = 0;
		return;
	}
	if (clock_phase(now, &frac)) {
		if (frac > 200000u && age < 3000000u)
			return;                             /* wait for just after the chip's tick */
	} else if (age < 3000000u) {
		return;                                 /* no tick seen yet: wait for one, 3 s at most */
	} else {
		frac = 0;
	}
	/* the time now, less the part of this second the chip has counted already, rounded */
	us = (s32)set_usec + (s32)age - (s32)frac + 1500000;
	secs = set_sec - 1u + (u32)us / 1000000u;
	local = secs + (u32)rt_tz_offset + MAC_EPOCH_TO_UNIX;
	set_diff = (s32)(local - MAC_TIME);
	set_pending = 0;
	if (mac_setdatetime(local) == 0) {
		u16 sr = mac_irq_off();
		sub_ok = 0;                             /* a jump, not a tick: the phase again */
		sub_time = MAC_TIME;
		sub_prev_us = (u32)rt_now_us();
		mac_irq_restore(sr);
		set_local = local;
		set_done = 1;
	}
}

/* local seconds since 1904 as "2026-11-23 14:05:09" */
static void fmt_local(char *b, u32 len, u32 t)
{
	u32 days = t / 86400u, s = t % 86400u, y = 1904, m = 0;
	static const u8 mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	for (;;) {
		u32 yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
		if (days < yd)
			break;
		days -= yd;
		y++;
	}
	for (; m < 12; m++) {
		u32 md = mdays[m] + (m == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		if (days < md)
			break;
		days -= md;
	}
	rt_snprintf(b, len, "%lu-%02lu-%02lu %02lu:%02lu:%02lu", (unsigned long)y, (unsigned long)m + 1,
	            (unsigned long)days + 1, (unsigned long)(s / 3600u), (unsigned long)(s / 60u % 60u),
	            (unsigned long)(s % 60u));
}

/* the stack's task, every pass (at least every 100 ms): the clock's phase, and what accRun did */
static void mac_clock_sample(void)
{
	u32 t = MAC_TIME, now = (u32)rt_now_us();
	u16 sr = mac_irq_off();
	if (t != sub_time) {
		u32 gap = now - sub_prev_us;
		if (t == sub_time + 1 && gap < 250000u) {
			sub_us = now - gap / 2;             /* the tick was between the two looks */
			sub_ok = 1;
		} else if (t == sub_time + 1 && sub_ok) {
			sub_us += 1000000u;                 /* looked late: the phase carries on */
		} else {
			sub_ok = 0;                         /* the first look, or the clock was set */
		}
		sub_time = t;
	}
	sub_prev_us = now;
	mac_irq_restore(sr);
	if (tz_news) {
		s32 o = tz_delta < 0 ? -tz_delta : tz_delta;
		tz_news = 0;
		if (!tz_known)
			rt_log(RT_LOG_INFO, "time zone: none set in the Map control panel, local time taken as UTC "
			       "(set it there, or \"timezone +HH:MM\" in riptide.conf)");
		else
			rt_log(RT_LOG_INFO, "time zone: UTC%c%02ld:%02ld%s (the Map control panel)%s",
			       tz_delta < 0 ? '-' : '+', (long)(o / 3600), (long)(o / 60 % 60),
			       tz_dst ? " with daylight saving time" : "",
			       rt_tz_configured ? "; riptide.conf's \"timezone\" is used instead" : "");
	}
	if (set_done) {
		char b[32];
		set_done = 0;
		fmt_local(b, sizeof b, set_local);
		rt_log(RT_LOG_INFO, "ntp: the Mac's clock set to %s local time (%s%ld s)", b, set_diff < 0 ? "-" : "+",
		       (long)(set_diff < 0 ? -set_diff : set_diff));
	}
}

void mac_clock_init(void)
{
	mac_tz_read();
	sub_time = MAC_TIME;
	rt_set_wallclock_hook = mac_set_clock;
}

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
/* the MacTCP or .IPP6 program a socket is made for (ipp.h): RiptidePrefs' Background programs and
   QoS rules match it, as they match the Amiga's task names */
const char *mac_prog_name;
void rt_os_program_name(char *buf, u32 len) { if (len) rt_strlcpy(buf, mac_prog_name ? mac_prog_name : "", len); }

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
/* Riptide runs only in its deferred task (mac_stack_run counts the time there): the rest of the
   clock's time it was idle, which the Status page's meter shows as Riptide's share of the Mac */
void rt_cpu_counts(u32 *now, u32 *idle, u32 *hz)
{
	u32 t = (u32)rt_now_us();
	idle_us = t - busy_us;
	*now = t;
	*idle = idle_us;
	*hz = 1000000u;
}

/* ---- waiting -------------------------------------------------------------------------------
   The deferred task never waits (the MacTCP interface uses non-blocking calls). A program calling
   the core in its own context (api_mac.c) does: its client's sleeper gives the core back and
   calls the program's yield until a waker sets 'woken', the time is up or the user stops it. */
static struct rt_sleeper the_sleeper;
volatile int mac_wakeups;                          /* ipp.c rescans its pending calls when this moves */
static struct mac_client *core_client;            /* the program the core is lent to, or NULL */
static volatile u8 core_lent, stack_put_off;
void mac_core_enter(struct mac_client *c)
{
	u16 sr = mac_irq_off();
	core_lent = 1;
	core_client = c;
	mac_irq_restore(sr);
}
void mac_core_leave(void)
{
	u16 sr = mac_irq_off();
	int run = stack_put_off;
	core_lent = 0;
	core_client = NULL;
	stack_put_off = 0;
	mac_irq_restore(sr);
	if (run)
		mac_schedule();
}
struct rt_sleeper *rt_sleeper_self(void) { return core_client ? &core_client->sl : &the_sleeper; }
#define BREAK_SIGNAL (1u << 12)                    /* SIGBREAKF_CTRL_C, as WaitSelect's mask has it */
static int client_wait(struct mac_client *c, s32 timeout_ms, u32 *extra_signals)
{
	u32 want = extra_signals ? *extra_signals : 0, t0 = rt_now_ms();
	if (extra_signals)
		*extra_signals = 0;
	for (;;) {
		int depth;
		if (c->sl.woken) {
			c->sl.woken = 0;
			return 0;
		}
		if (c->brk && c->brk(c->arg)) {
			if (want & BREAK_SIGNAL) {
				*extra_signals = BREAK_SIGNAL;
				return 0;
			}
			return -RT_EINTR;
		}
		if (timeout_ms == 0 || (timeout_ms > 0 && rt_now_ms() - t0 >= (u32)timeout_ms))
			return -RT_ETIMEDOUT;
		/* give the core back while the program takes its events */
		depth = lock_depth;
		lock_depth = 0;
		mac_core_leave();
		if (c->yield)
			c->yield(c->arg);
		mac_core_enter(c);
		lock_depth = depth;
	}
}
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
/* A program waiting in the core sits in WaitNextEvent with a sleep of a tick or more; setting
   'woken' alone left it there until the tick (16.7 ms of every ping on the Mac's side, measured on
   Aurora68 on 19 November). WakeUpProcess ends that sleep now. */
void rt_waitq_wake(struct rt_waitq *wq)
{
	for (struct rt_wait_entry *e = wq->head; e; e = e->next)
		if (e->sleeper) {
			e->sleeper->woken = 1;
			if (e->sleeper->has_psn)
				mac_wakeup_process(e->sleeper->psn);
		}
	mac_wakeups++;
}
int rt_sleeper_wait(struct rt_sleeper *s, s32 timeout_ms, u32 *extra_signals)
{
	if (core_client && s == &core_client->sl)
		return client_wait(core_client, timeout_ms, extra_signals);
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
	dt_queued = 0;
	if (core_lent) {
		/* a program is in the core (api_mac.c): its leaving asks for this again */
		stack_put_off = 1;
		mac_irq_restore(sr);
		return;
	}
	in_stack = 1;
	mac_irq_restore(sr);
	u64 t0 = rt_now_us();
	u32 ms;
	for (int rounds = 0; ; rounds++) {
		again = 0;
		mac_clock_sample();
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
#ifdef AURORA68_LOG
	/* where the driver's image is: a PC in the kernel's heartbeat, less this, is an address in
	   build/mac/drvr.map */
	{ char t[64]; rt_snprintf(t, sizeof t, "image at %p (mac_os_init)", (void *)mac_os_init); mac_log_line(t); }
#endif
	dt.qtype = 7; dt.addr = (void *)mac_dt_entry; dt.param = NULL;
	tm.qtype = 0; tm.addr = (void *)mac_tm_entry;
	mac_insxtime(&tm);
	return 0;
}
