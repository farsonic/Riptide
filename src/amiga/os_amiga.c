/*
 * Riptide - rt_os.h for AmigaOS (exec.library, timer.device).
 *
 * Stack lock: one exec SignalSemaphore. Sleepers: one per client task,
 * embedded in the per-task context (libs.c); a sleeper owns a wakeup
 * signal and a private timer request for timeouts. Clocks come from
 * ReadEClock(). Log lines go to a ring buffer that only the Riptide
 * process writes out with dos.library (client tasks must not do DOS I/O).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

struct Device *TimerBase;
struct LocaleBase *LocaleBase;
struct Library *BattClockBase;

static struct SignalSemaphore stack_sem;
static struct timerequest *clock_req;    /* keeps timer.device open */
static struct MsgPort *clock_port;
static ULONG eclock_freq;

struct Task *amiga_stack_task;
ULONG amiga_kick_mask;
int amiga_virtual_clock;
u32 amiga_virtual_ms;
int amiga_log_to_stdout = 1;

/* ---- lock ---- */

void rt_lock(void)
{
	u32 t0;

	if (!rt_prof_on) {
		ObtainSemaphore(&stack_sem);
		return;
	}
	/* profiling: time the waits (an uncontended lock costs no clock read) */
	if (AttemptSemaphore(&stack_sem))
		return;
	t0 = rt_prof_ticks();
	ObtainSemaphore(&stack_sem);
	rt_prof_add(FindTask(NULL) == amiga_stack_task ? RT_PROF_LOCK_STACK : RT_PROF_LOCK_APP,
	            rt_prof_ticks() - t0);
}

static u32 clk_ok;    /* see eclock_read() */

void rt_unlock(void)
{
	if (stack_sem.ss_NestCount == 1) {
		clk_ok = 0;
		/* no profiled section spans a lock release */
		if (rt_prof_depth)
			rt_prof_unwind();
	}
	ReleaseSemaphore(&stack_sem);
}

int rt_lock_held(void)
{
	return stack_sem.ss_Owner == FindTask(NULL) && stack_sem.ss_NestCount > 0;
}

/* ---- clocks ---- */

/*
 * The clocks run for every packet, so they use 32-bit multiplies and
 * divides only: the compilers' 64-bit helpers use the 64-bit mulu.l and
 * divu.l forms, which the 68060 traps to 68060.library. ev_hi * 2^32
 * divided by the EClock frequency changes only when ev_hi does (every 100
 * minutes or so); it is cached, and the sequence check keeps concurrent
 * callers (any task may read the clock) from mixing two cache entries.
 *
 * A packet reads the clock about nine times (TCP's clocks, timers, the
 * route and neighbour caches), each a ReadEClock() and divisions: around
 * a millisecond per packet on a 68020. So the holder of the stack lock
 * reads it once per hold: the time is that of the lock's first clock read
 * until its outermost rt_unlock(), like a jiffies counter that ticks
 * between the stack's polls (holds take milliseconds). Other tasks, and
 * code outside the lock, read the EClock every time.
 */
static volatile u32 ec_hi = 0xffffffffu, ec_hq, ec_hr;
static u32 clk_q, clk_r;

static void eclock_read(u32 *q, u32 *r)
{
	struct EClockVal ev;
	u32 h, hq, hr, lq, lr;
	int owner = stack_sem.ss_Owner == RT_AMIGA_THISTASK(SysBase);

	if (owner && clk_ok) {
		*q = clk_q;
		*r = clk_r;
		return;
	}
	ReadEClock(&ev);
	h = ec_hi;
	hq = ec_hq;
	hr = ec_hr;
	if (h != ev.ev_hi || ec_hi != h) {
		u64 e = (u64)ev.ev_hi << 32;
		hq = (u32)(e / eclock_freq);
		hr = (u32)(e % eclock_freq);
		ec_hi = 0xffffffffu;
		ec_hq = hq;
		ec_hr = hr;
		ec_hi = ev.ev_hi;
	}
	lq = ev.ev_lo / eclock_freq;
	lr = ev.ev_lo % eclock_freq + hr;    /* < 2 * eclock_freq */
	if (lr >= eclock_freq) {
		lr -= eclock_freq;
		lq++;
	}
	*q = hq + lq;
	*r = lr;
	if (owner) {
		clk_q = *q;
		clk_r = lr;
		clk_ok = 1;
	}
}

u32 rt_now_ms(void)
{
	u32 q, r;

	if (amiga_virtual_clock || !TimerBase)
		return amiga_virtual_ms;
	eclock_read(&q, &r);
	if (eclock_freq < 4294967u)          /* r * 1000 fits 32 bits */
		return q * 1000u + r * 1000u / eclock_freq;
	return q * 1000u + (u32)(((u64)r * 1000u) / eclock_freq);
}

u64 rt_now_us(void)
{
	u32 q, r, m;

	if (amiga_virtual_clock || !TimerBase)
		return rt_mul32x32(amiga_virtual_ms, 1000u);
	eclock_read(&q, &r);
	if (eclock_freq >= 4294967u)
		return (u64)q * 1000000u + ((u64)r * 1000000u) / eclock_freq;
	/* r * 10^6 / f = 1000 * (r * 1000 / f) + (r * 1000 % f) * 1000 / f */
	m = r * 1000u;
	return rt_mul32x32(q, 1000000u) + (m / eclock_freq) * 1000u + (m % eclock_freq) * 1000u / eclock_freq;
}

/* profiling clock: the EClock's low 32 bits (wraps after 100 minutes) */
u32 rt_prof_ticks(void)
{
	struct EClockVal ev;

	if (amiga_virtual_clock || !TimerBase)
		return amiga_virtual_ms;
	ReadEClock(&ev);
	return ev.ev_lo;
}

u32 rt_prof_hz(void)
{
	return amiga_virtual_clock || !TimerBase || !eclock_freq ? 1000u : eclock_freq;
}

/* AmigaOS keeps local time since 1.1.1978: 2922 days after 1970 */
#define AMIGA_EPOCH 252460800u

void rt_wallclock_tv(u32 *sec, u32 *usec)
{
	struct timeval tv;

	if (amiga_virtual_clock || !TimerBase) {
		*sec = *usec = 0;
		return;
	}
	GetSysTime(&tv);
	*sec = tv.tv_secs + AMIGA_EPOCH - (u32)rt_tz_offset;
	*usec = tv.tv_micro;
}

u32 rt_wallclock(void)
{
	u32 s, u;

	rt_wallclock_tv(&s, &u);
	return s;
}

/* NTP steps the clock (the stack process; a private request, as clock_req
 * belongs to the task that ran amiga_os_init()) */
static int amiga_set_clock(u32 sec, u32 usec)
{
	struct MsgPort *port;
	struct timerequest *tr;
	u32 local = sec - AMIGA_EPOCH + (u32)rt_tz_offset;
	int r = -RT_EIO;

	if (amiga_virtual_clock || (s32)local < 0)
		return -RT_EINVAL;
	port = CreateMsgPort();
	if (!port)
		return -RT_ENOMEM;
	tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr));
	if (tr && OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &tr->tr_node, 0) == 0) {
		tr->tr_node.io_Command = TR_SETSYSTIME;
		tr->tr_time.tv_secs = local;
		tr->tr_time.tv_micro = usec;
		if (DoIO(&tr->tr_node) == 0)
			r = 0;
		CloseDevice(&tr->tr_node);
	}
	if (tr)
		DeleteIORequest((struct IORequest *)tr);
	DeleteMsgPort(port);
	if (r == 0 && rt_ntp_battclock) {
		if (!BattClockBase)
			BattClockBase = (struct Library *)OpenResource((CONST_STRPTR)"battclock.resource");
		if (BattClockBase)
			WriteBattClock(local);
	}
	return r;
}

/* the time zone from the Locale preferences (loc_GMTOffset: minutes west) */
static void amiga_locale_tz(void)
{
	struct Locale *loc;

	LocaleBase = (struct LocaleBase *)OpenLibrary((CONST_STRPTR)"locale.library", 38);
	if (!LocaleBase)
		return;
	loc = OpenLocale(NULL);
	if (loc) {
		rt_tz_system = -(s32)loc->loc_GMTOffset * 60;
		if (!rt_tz_configured)
			rt_tz_offset = rt_tz_system;
		CloseLocale(loc);
	}
	CloseLibrary((struct Library *)LocaleBase);
	LocaleBase = NULL;
}

/* ---- memory ---- */

/* Packet buffers come from rt_malloc(): keep them in Fast RAM, where
 * MOVE16 is safe and the CPU is not slowed down by the chipset. If memory
 * runs short we take any public memory, and then MOVE16 is off for good. */
static int amiga_have_fast;
static int amiga_check_move16;      /* 68040+: watch for non-Fast buffers */

void *rt_malloc(size_t n)
{
	void *p;

	if (n == 0)
		n = 1;
	if (amiga_have_fast) {
		p = AllocVec((ULONG)n, MEMF_PUBLIC | MEMF_FAST);
		if (p)
			return p;
	}
	p = AllocVec((ULONG)n, MEMF_PUBLIC);
	if (p && amiga_check_move16 && !(TypeOfMem(p) & MEMF_FAST)) {
		amiga_check_move16 = 0;
		rt_arch_move16_veto("packet memory is not Fast RAM");
	}
	return p;
}

void *rt_zalloc(size_t n)
{
	if (n == 0)
		n = 1;
	return AllocVec((ULONG)n, MEMF_PUBLIC | MEMF_CLEAR);
}

void rt_free(void *p)
{
	if (p)
		FreeVec(p);
}

/* ---- entropy ---- */

static u32 rnd_state = 0x2545f491u;

u32 rt_random32(void)
{
	u32 x = rnd_state;

	if (TimerBase && !amiga_virtual_clock) {
		struct EClockVal ev;
		ReadEClock(&ev);
		x ^= ev.ev_lo * 0x9e3779b9u;
	}
	/* xorshift32 */
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (x == 0)
		x = 0x6d2b79f5u;
	rnd_state = x;
	return x * 0x2c1b3c6du;
}

void rt_random_bytes(void *buf, size_t n)
{
	u8 *p = buf;

	while (n) {
		u32 r = rt_random32();
		size_t k = n < 4 ? n : 4;
		memcpy(p, &r, k);
		p += k;
		n -= k;
	}
}

/* ---- logging: ring buffer, flushed by the stack process ---- */

int rt_log_level = RT_LOG_WARN;

#define LOG_RING 8192
static char log_ring[LOG_RING];
static u32 log_head, log_tail;            /* head: write, tail: read */
static u32 log_dropped;

static void log_put(const char *s, u32 n)
{
	u32 i;

	Forbid();
	if (LOG_RING - 1 - (log_head - log_tail) < n) {
		log_dropped++;
	} else {
		for (i = 0; i < n; i++)
			log_ring[(log_head + i) % LOG_RING] = s[i];
		log_head += n;
	}
	Permit();
}

void rt_log(int level, const char *fmt, ...)
{
	static const char *const lvl[] = { "error: ", "warning: ", "", "debug: " };
	char line[200];
	va_list ap;
	int n;

	/* "debug log": a copy in the debug ring (whose writers hold the lock) */
	if ((rt_dbg_mask & RT_DBG_LOG) && rt_lock_held()) {
		va_start(ap, fmt);
		rt_vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		rt_dbg_line(RT_DBG_LOG, line);
	}
	if (level > rt_log_level)
		return;
	n = rt_snprintf(line, sizeof(line), "Riptide: %s", lvl[level & 3]);
	va_start(ap, fmt);
	rt_vsnprintf(line + n, sizeof(line) - (u32)n - 1, fmt, ap);
	va_end(ap);
	n = (int)strlen(line);
	line[n++] = '\n';
	log_put(line, (u32)n);
	if (amiga_stack_task && FindTask(NULL) != amiga_stack_task)
		Signal(amiga_stack_task, amiga_kick_mask);
}

void amiga_log_line(const char *line)
{
	u32 n = (u32)strlen(line);

	log_put(line, n);
	if (n == 0 || line[n - 1] != '\n')
		log_put("\n", 1);
	if (amiga_stack_task && FindTask(NULL) != amiga_stack_task)
		Signal(amiga_stack_task, amiga_kick_mask);
}

void amiga_log_flush(void)
{
	char buf[256];
	u32 n;
	BPTR out;

	if (!DOSBase)
		return;
	out = Output();
	for (;;) {
		Forbid();
		n = 0;
		while (log_tail != log_head && n < sizeof(buf)) {
			buf[n++] = log_ring[log_tail % LOG_RING];
			log_tail++;
		}
		Permit();
		if (!n)
			break;
		if (out && amiga_log_to_stdout)
			Write(out, buf, (LONG)n);
	}
	if (log_dropped && out) {
		n = log_dropped;
		log_dropped = 0;
		amiga_out("Riptide: %lu log lines dropped\n", (unsigned long)n);
	}
}

/* "debug dump FILE" (rt_dbg_dump_hook). Runs in the Riptide process with
 * the lock held; the ring is copied first, the file written without it. */
int amiga_dbg_dump(const char *path, char *err, u32 errlen)
{
	u32 n, done = 0;
	char *buf;
	BPTR fh;
	LONG w;

	if (!DOSBase || FindTask(NULL) != amiga_stack_task) {
		rt_strlcpy(err, "debug dump: only the Riptide process can write files", errlen);
		return -RT_EPERM;
	}
	n = rt_dbg_snapshot(NULL, 0);
	buf = rt_malloc(n ? n : 1);
	if (!buf) {
		rt_strlcpy(err, "debug dump: out of memory", errlen);
		return -RT_ENOMEM;
	}
	n = rt_dbg_snapshot(buf, n);
	rt_unlock();
	fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	if (fh) {
		while (done < n && (w = Write(fh, buf + done, (LONG)(n - done))) > 0)
			done += (u32)w;
		Close(fh);
	}
	rt_lock();
	rt_free(buf);
	if (!fh || done < n) {
		rt_snprintf(err, errlen, "debug dump: cannot %s %s", fh ? "write" : "create", path);
		return -RT_EIO;
	}
	rt_snprintf(err, errlen, "debug: %lu bytes written to %s\n", (unsigned long)n, path);
	return 0;
}

void amiga_out(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	BPTR out;

	va_start(ap, fmt);
	rt_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (DOSBase && (out = Output()) != 0)
		Write(out, line, (LONG)strlen(line));
}

/* ---- kick ---- */

void rt_kick(void)
{
	if (amiga_stack_task)
		Signal(amiga_stack_task, amiga_kick_mask);
}

/* ---- sleepers ---- */

static struct MinList orphan_sleepers;   /* tasks without a library context */

int amiga_sleeper_init(struct rt_sleeper *s, const ULONG *breakmask)
{
	memset(s, 0, sizeof(*s));
	s->task = FindTask(NULL);
	s->breakmask = breakmask;
	s->sigbit = AllocSignal(-1);
	if (s->sigbit < 0)
		return -RT_EAGAIN;
	s->sigmask = 1UL << s->sigbit;
	s->tsigbit = AllocSignal(-1);
	if (s->tsigbit < 0) {
		FreeSignal(s->sigbit);
		return -RT_EAGAIN;
	}
	s->tport.mp_Node.ln_Type = NT_MSGPORT;
	s->tport.mp_Flags = PA_SIGNAL;
	s->tport.mp_SigBit = (UBYTE)s->tsigbit;
	s->tport.mp_SigTask = s->task;
	NewList(&s->tport.mp_MsgList);
	s->treq.tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	s->treq.tr_node.io_Message.mn_ReplyPort = &s->tport;
	s->treq.tr_node.io_Message.mn_Length = sizeof(s->treq);
	if (!amiga_virtual_clock &&
	    OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &s->treq.tr_node, 0) == 0)
		s->timer_open = 1;
	return 0;
}

void amiga_sleeper_fini(struct rt_sleeper *s)
{
	if (s->timer_open)
		CloseDevice(&s->treq.tr_node);
	s->timer_open = 0;
	if (s->tsigbit >= 0)
		FreeSignal(s->tsigbit);
	if (s->sigbit >= 0)
		FreeSignal(s->sigbit);
	s->sigbit = s->tsigbit = -1;
}

struct rt_sleeper *rt_sleeper_self(void)
{
	struct Task *me = FindTask(NULL);
	struct TaskCtx *c = amiga_ctx_find(me);
	struct MinNode *n;
	struct rt_sleeper *s;

	if (c)
		return &c->sl;
	/* A task without a bsdsocket/riptide context (the stack process, or a
	 * program calling through a library opened by another task). */
	Forbid();
	for (n = orphan_sleepers.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
		s = (struct rt_sleeper *)n;
		if (s->task == me) {
			Permit();
			return s;
		}
	}
	Permit();
	s = rt_zalloc(sizeof(*s));
	if (s && amiga_sleeper_init(s, NULL) < 0) {
		rt_free(s);
		s = NULL;
	}
	if (!s) {
		/* Cannot fail per contract: use a static fallback that only
		 * polls (sigmask 0 + timer). */
		static struct rt_sleeper fallback;
		fallback.task = me;
		return &fallback;
	}
	Forbid();
	AddTail((struct List *)&orphan_sleepers, (struct Node *)&s->node);
	Permit();
	return s;
}

static void sleeper_wake(struct rt_sleeper *s)
{
	s->woken = 1;
	if (s->task && s->sigmask)
		Signal(s->task, s->sigmask);
}

int rt_sleeper_wait(struct rt_sleeper *s, s32 timeout_ms, u32 *extra_signals)
{
	ULONG want = extra_signals ? *extra_signals : 0;
	/* A break signal the caller also asked for (WaitSelect's signal mask,
	 * e.g. SIGBREAKF_CTRL_C) is delivered to it as a plain signal, not as
	 * EINTR - AmiTCP semantics, and what Ctrl-C aware tools rely on. */
	ULONG brk = (s->breakmask ? *s->breakmask : 0) & ~want;
	ULONG tmask = 0, got, pending;
	int expired = 0;

	if (extra_signals)
		*extra_signals = 0;
	/* Clear a stale wakeup signal; the 'woken' flag is authoritative and
	 * every waker sets it (under the lock) before signalling. */
	if (s->sigmask)
		SetSignal(0, s->sigmask);
	if (s->woken) {
		s->woken = 0;
		return 0;
	}
	pending = SetSignal(0, 0) & (brk | want);
	if (pending) {
		SetSignal(0, pending);
		got = pending;
	} else {
		if (timeout_ms == 0)
			return -RT_ETIMEDOUT;
		if (timeout_ms > 0 && s->timer_open) {
			tmask = 1UL << s->tsigbit;
			SetSignal(0, tmask);
			s->treq.tr_node.io_Command = TR_ADDREQUEST;
			s->treq.tr_time.tv_secs = (ULONG)timeout_ms / 1000u;
			s->treq.tr_time.tv_micro = ((ULONG)timeout_ms % 1000u) * 1000u;
			SendIO(&s->treq.tr_node);
		}
		if (!(s->sigmask | brk | want | tmask)) {
			/* fallback sleeper: nothing to wait for, poll politely */
			rt_unlock();
			if (DOSBase)
				Delay(1);
			rt_lock();
			return timeout_ms >= 0 ? -RT_ETIMEDOUT : 0;
		}
		rt_unlock();
		got = Wait(s->sigmask | brk | want | tmask);
		if (tmask) {
			if (!CheckIO(&s->treq.tr_node))
				AbortIO(&s->treq.tr_node);
			WaitIO(&s->treq.tr_node);
			expired = s->treq.tr_node.io_Error == 0;
			SetSignal(0, tmask);
		}
		rt_lock();
	}
	if (extra_signals)
		*extra_signals = got & want;
	if (s->woken || (got & s->sigmask)) {
		s->woken = 0;
		/* keep a break that arrived together with the wakeup */
		if (got & brk)
			SetSignal(got & brk, got & brk);
		return 0;
	}
	if (got & brk)
		return -RT_EINTR;
	if (got & want)
		return 0;
	if (expired)
		return -RT_ETIMEDOUT;
	return 0;
}

void rt_waitq_add(struct rt_waitq *wq, struct rt_wait_entry *e, struct rt_sleeper *s)
{
	e->sleeper = s;
	e->prev = NULL;
	e->next = wq->head;
	if (wq->head)
		wq->head->prev = e;
	wq->head = e;
}

void rt_waitq_remove(struct rt_waitq *wq, struct rt_wait_entry *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else if (wq->head == e)
		wq->head = e->next;
	else
		return;
	if (e->next)
		e->next->prev = e->prev;
	e->next = e->prev = NULL;
}

void rt_waitq_wake(struct rt_waitq *wq)
{
	struct rt_wait_entry *e;

	for (e = wq->head; e; e = e->next)
		sleeper_wake(e->sleeper);
}

/* ---- init ---- */

/* CPU model from SysBase->AttnFlags (the 68060.library sets AFF_68060;
 * AFF_68040 and AFF_68030 are also set on the bigger CPUs). */
static u32 amiga_cpu_model(void)
{
	UWORD af = RT_AMIGA_ATTNFLAGS(SysBase);

	if (af & (1u << 7))
		return 68060;
	if (af & (1u << 3))
		return 68040;
	if (af & (1u << 2))
		return 68030;
	if (af & (1u << 1))
		return 68020;
	if (af & (1u << 0))
		return 68010;
	return 68000;
}

int amiga_os_init(void)
{
	BYTE kb;

	InitSemaphore(&stack_sem);
	rt_dbg_dump_hook = amiga_dbg_dump;
	rt_newminlist(&orphan_sleepers);
	rt_arch_cpu = amiga_cpu_model();
	amiga_have_fast = AvailMem(MEMF_FAST) != 0;
	amiga_check_move16 = rt_arch_cpu >= 68040;
	amiga_stack_task = FindTask(NULL);
	kb = AllocSignal(-1);
	if (kb < 0)
		return -1;
	amiga_kick_mask = 1UL << kb;
	if (!amiga_virtual_clock) {
		clock_port = CreateMsgPort();
		if (!clock_port)
			return -1;
		clock_req = (struct timerequest *)CreateIORequest(clock_port, sizeof(*clock_req));
		if (!clock_req)
			return -1;
		if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &clock_req->tr_node, 0) != 0)
			return -1;
		TimerBase = clock_req->tr_node.io_Device;
		{
			struct EClockVal ev;
			eclock_freq = ReadEClock(&ev);
		}
		if (eclock_freq == 0)
			eclock_freq = 709379;
		amiga_locale_tz();
		rt_set_wallclock_hook = amiga_set_clock;
	}
	return 0;
}

void amiga_os_fini(void)
{
	amiga_log_flush();
	if (amiga_kick_mask) {
		BYTE b = 0;
		while (!(amiga_kick_mask & (1UL << b)))
			b++;
		FreeSignal(b);
		amiga_kick_mask = 0;
	}
	if (clock_req) {
		if (TimerBase)
			CloseDevice(&clock_req->tr_node);
		DeleteIORequest((struct IORequest *)clock_req);
		clock_req = NULL;
	}
	if (clock_port)
		DeleteMsgPort(clock_port);
	clock_port = NULL;
	TimerBase = NULL;
}

/* ---- files (packet captures) ---- */

/* Only a process may call dos.library: client tasks get NULL. */
void *rt_file_create(const char *path)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR win;
	BPTR fh;

	if (me->pr_Task.tc_Node.ln_Type != NT_PROCESS)
		return NULL;
	win = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR)-1;     /* no "insert volume" requester */
	fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	me->pr_WindowPtr = win;
	return fh ? (void *)fh : NULL;
}

int rt_file_write(void *f, const void *p, u32 n)
{
	return Write((BPTR)f, (APTR)p, (LONG)n) == (LONG)n ? 0 : -1;
}

int rt_file_close(void *f)
{
	return Close((BPTR)f) ? 0 : -1;
}
