/*
 * Riptide - one-shot millisecond timers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Pending timers live on one list sorted by expiry time. Most new timers
 * expire after the ones already queued (retransmit, delayed ACK, keepalive
 * re-arms), so insertion scans from the tail and is usually O(1).
 *
 * Lists are circular with a sentinel node so a timer can be unlinked
 * without knowing which list it is on: rt_timers_run() first moves every
 * expired timer to a private "firing" list and then fires them one by one.
 * A callback that re-arms its own timer for "now" therefore runs again on
 * the next call, not in an endless loop, and a callback may freely delete
 * or re-arm any other timer, including one that is about to fire.
 */
#include "rt/rt_timer.h"
#include "rt/rt_os.h"

static struct rt_timer timer_list = { &timer_list, &timer_list, 0, NULL, NULL, 0 };
static struct rt_timer firing_list = { &firing_list, &firing_list, 0, NULL, NULL, 0 };

static int time_before(u32 a, u32 b) { return (s32)(a - b) < 0; }

static void timer_unlink(struct rt_timer *t)
{
	t->prev->next = t->next;
	t->next->prev = t->prev;
	t->next = t->prev = NULL;
}

static void timer_insert_after(struct rt_timer *pos, struct rt_timer *t)
{
	t->prev = pos;
	t->next = pos->next;
	pos->next->prev = t;
	pos->next = t;
}

void rt_timer_init(struct rt_timer *t, void (*fn)(void *), void *arg)
{
	t->next = t->prev = NULL;
	t->expires = 0;
	t->fn = fn;
	t->arg = arg;
	t->pending = 0;
}

void rt_timer_mod(struct rt_timer *t, u32 expires)
{
	struct rt_timer *pos;

	if (t->pending)
		timer_unlink(t);
	t->expires = expires;
	t->pending = 1;
	/* Find the last timer that does not expire after us (FIFO among
	 * equal expiry times). */
	for (pos = timer_list.prev; pos != &timer_list; pos = pos->prev)
		if (!time_before(expires, pos->expires))
			break;
	timer_insert_after(pos, t);
	if (timer_list.next == t)
		rt_kick();
}

void rt_timer_del(struct rt_timer *t)
{
	if (!t->pending)
		return;
	timer_unlink(t);
	t->pending = 0;
}

u32 rt_timers_next(u32 now)
{
	struct rt_timer *t = timer_list.next;

	if (t == &timer_list)
		return RT_TIMER_NONE;
	if (!time_before(now, t->expires))
		return 0;
	return t->expires - now;
}

u32 rt_timers_run(u32 now)
{
	struct rt_timer *t;

	/* Detach the expired prefix of the sorted list onto firing_list. */
	while ((t = timer_list.next) != &timer_list && !time_before(now, t->expires)) {
		timer_unlink(t);
		timer_insert_after(firing_list.prev, t);
	}
	while ((t = firing_list.next) != &firing_list) {
		timer_unlink(t);
		t->pending = 0;
		t->fn(t->arg);
	}
	return rt_timers_next(now);
}
