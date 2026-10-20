/*
 * Riptide - operating system abstraction.
 *
 * Implemented by src/host/os_host.c (POSIX threads, for tests) and by
 * src/amiga/os_amiga.c (exec.library).
 *
 * Concurrency model: the whole stack is protected by ONE lock (the "stack
 * lock", like the old BSD splnet()). Protocol code always runs with it held:
 *  - API calls (socket layer) take it on entry, in the caller's context
 *    (on AmigaOS: the client task that called bsdsocket.library);
 *  - the stack thread/process takes it while handling received frames and
 *    timers.
 * Blocking API calls sleep on a wait queue, which atomically drops the lock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_OS_H
#define RT_OS_H

#include "rt_types.h"

void rt_lock(void);
void rt_unlock(void);
int  rt_lock_held(void);          /* debugging aid; may always return 1 */

/* Monotonic clocks. rt_now_ms() wraps after ~49 days; always compare with
 * (s32)(a - b). */
u32  rt_now_ms(void);
u64  rt_now_us(void);
/* Wall clock seconds since 1970 (for DHCP lease logging etc); may be 0. */
u32  rt_wallclock(void);

void *rt_malloc(size_t n);
void *rt_zalloc(size_t n);
void  rt_free(void *p);           /* rt_free(NULL) is a no-op */

/* Entropy: should mix in a high resolution clock. Used for ISNs,
 * ephemeral ports, DNS ids, DHCP xids, hash secrets. */
u32  rt_random32(void);
void rt_random_bytes(void *buf, size_t n);

/* Logging. level: 0=err 1=warn 2=info 3=debug */
#define RT_LOG_ERR   0
#define RT_LOG_WARN  1
#define RT_LOG_INFO  2
#define RT_LOG_DEBUG 3
void rt_log(int level, const char *fmt, ...) RT_PRINTF(2, 3);
extern int rt_log_level;

/* Ask the stack thread to wake up and recompute its next timer deadline
 * (call after arming a timer that may be earlier than the current one, or
 * after queueing work for it). Safe to call with the lock held. */
void rt_kick(void);

/* ---- Sleeping ----------------------------------------------------------
 * A "sleeper" represents one thread/task that can block. A wait queue holds
 * entries pointing at sleepers. One sleeper may be on many queues at once
 * (WaitSelect). Wakeups are sticky: a wakeup that happens between
 * rt_waitq_add() and rt_sleeper_wait() is not lost.
 */
struct rt_sleeper;                /* OS specific */

struct rt_wait_entry {
	struct rt_wait_entry *next;
	struct rt_wait_entry *prev;
	struct rt_sleeper *sleeper;
};

struct rt_waitq {
	struct rt_wait_entry *head;
};

/* The sleeper of the calling thread/task (never NULL in API context). */
struct rt_sleeper *rt_sleeper_self(void);

static inline void rt_waitq_init(struct rt_waitq *wq) { wq->head = NULL; }
void rt_waitq_add(struct rt_waitq *wq, struct rt_wait_entry *e, struct rt_sleeper *s);
void rt_waitq_remove(struct rt_waitq *wq, struct rt_wait_entry *e);
void rt_waitq_wake(struct rt_waitq *wq);   /* wake every sleeper on wq */

/* Called with the stack lock held; releases it while sleeping and
 * re-acquires it before returning.
 *  timeout_ms < 0 : no timeout
 *  extra_signals  : (AmigaOS) in: additional exec signal mask to wait for,
 *                   out: which of those arrived. May be NULL. Ignored on host.
 * Returns 0 when woken (or extra signal arrived), -RT_ETIMEDOUT on timeout,
 * -RT_EINTR when the task's break signal (Ctrl-C mask from
 * SBTC_BREAKMASK) arrived. */
int rt_sleeper_wait(struct rt_sleeper *s, s32 timeout_ms, u32 *extra_signals);

#endif
