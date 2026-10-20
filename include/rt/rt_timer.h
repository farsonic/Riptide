/*
 * Riptide - one-shot timers with millisecond resolution.
 * All timer callbacks run in the stack thread with the stack lock held.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_TIMER_H
#define RT_TIMER_H

#include "rt_types.h"

struct rt_timer {
	struct rt_timer *next;
	struct rt_timer *prev;
	u32  expires;                 /* absolute rt_now_ms() value */
	void (*fn)(void *arg);
	void *arg;
	u8   pending;
};

void rt_timer_init(struct rt_timer *t, void (*fn)(void *), void *arg);
/* (Re)arm to fire at absolute time 'expires'. Calls rt_kick() if this
 * became the earliest timer. */
void rt_timer_mod(struct rt_timer *t, u32 expires);
static inline void rt_timer_after(struct rt_timer *t, u32 delta_ms);
void rt_timer_del(struct rt_timer *t);           /* no-op if not pending */
static inline int rt_timer_pending(const struct rt_timer *t) { return t->pending; }

/* Run every expired timer. Returns milliseconds until the next timer is
 * due (0 if one is due already, RT_TIMER_NONE when none pending).
 * Called by the stack thread with the lock held. */
#define RT_TIMER_NONE 0xffffffffu
u32 rt_timers_run(u32 now);
/* ms until next due timer, same encoding as rt_timers_run(). */
u32 rt_timers_next(u32 now);

#include "rt_os.h"
static inline void rt_timer_after(struct rt_timer *t, u32 delta_ms)
{
	rt_timer_mod(t, rt_now_ms() + delta_ms);
}

#endif
