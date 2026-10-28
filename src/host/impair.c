/*
 * Riptide - transmit-side link impairment for host tests (a tiny netem):
 * loss, duplication, reordering, delay + jitter, a rate limit and a size
 * limit without ICMP (a path MTU black hole), applied
 * by wrapping a netif's link_output.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"

#define IMPAIR_MAX 16

struct pending_cb {
	u64 due_us;
	u8  dst[6];
	u8  has_dst;
	u16 ethertype;
};

struct impair {
	struct rt_netif *nif;
	rt_link_output_fn orig;
	struct rt_host_impair cfg;
	struct rt_host_impair_stats st;
	u32 rng;
	u64 link_free_us;           /* rate limiter: when the "wire" is idle */
	struct rt_bufq q;           /* sorted by due time */
	struct rt_timer timer;
};

static struct impair impairs[IMPAIR_MAX];

static struct pending_cb *pcb(struct rt_buf *b)
{
	return (struct pending_cb *)(void *)b->cb.b;
}

static struct impair *impair_of(struct rt_netif *nif)
{
	int i;

	for (i = 0; i < IMPAIR_MAX; i++)
		if (impairs[i].nif == nif)
			return &impairs[i];
	return NULL;
}

static u32 rnd(struct impair *im)
{
	/* xorshift32: deterministic for a given seed */
	u32 x = im->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	im->rng = x;
	return x;
}

static int chance(struct impair *im, u32 per10000)
{
	return per10000 && rnd(im) % 10000u < per10000;
}

static void impair_flush_due(struct impair *im, u64 now)
{
	struct rt_buf *b;

	while ((b = im->q.head) != NULL && pcb(b)->due_us <= now) {
		struct pending_cb c = *pcb(b);
		rt_bufq_unlink(&im->q, b);
		im->st.sent++;
		im->orig(im->nif, b, c.has_dst ? c.dst : NULL, c.ethertype);
	}
	if (im->q.head) {
		u64 d = pcb(im->q.head)->due_us;
		rt_timer_mod(&im->timer, (u32)((d + 999) / 1000));
	}
}

static void impair_timer(void *arg)
{
	impair_flush_due(arg, rt_now_us() + 500);   /* timer has ms granularity */
}

static void enqueue(struct impair *im, struct rt_buf *b, const u8 *dsthw, u16 et, u64 due)
{
	struct rt_buf *pos;
	struct pending_cb *c = pcb(b);

	if (im->q.qlen >= im->cfg.queue_limit) {
		im->st.queue_drops++;
		rt_buf_free(b);
		return;
	}
	c->due_us = due;
	c->has_dst = dsthw != NULL;
	if (dsthw)
		memcpy(c->dst, dsthw, 6);
	c->ethertype = et;
	for (pos = im->q.tail; pos && pcb(pos)->due_us > due; pos = pos->prev)
		;
	rt_bufq_insert_after(&im->q, pos, b);
	if (im->q.head == b)
		rt_timer_mod(&im->timer, (u32)((due + 999) / 1000));
}

static int impair_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	struct impair *im = impair_of(nif);
	u64 now = rt_now_us(), due;
	int copies = 1, i;

	if (!im) {
		rt_buf_free(b);
		return -RT_ENODEV;
	}
	if (im->cfg.max_len && b->len > im->cfg.max_len) {
		im->st.too_big++;
		rt_buf_free(b);
		return 0;
	}
	if (chance(im, im->cfg.loss)) {
		im->st.dropped++;
		rt_buf_free(b);
		return 0;
	}
	if (chance(im, im->cfg.duplicate)) {
		im->st.duplicated++;
		copies = 2;
	}
	for (i = 0; i < copies; i++) {
		struct rt_buf *x = b;

		if (i == 0 && copies == 2) {
			x = rt_buf_copy(b);
			if (!x)
				continue;
		}
		due = now;
		if (im->cfg.rate_kbit) {
			u64 ser = (u64)x->len * 8u * 1000u / im->cfg.rate_kbit;   /* us */
			if (im->link_free_us < now)
				im->link_free_us = now;
			im->link_free_us += ser;
			due = im->link_free_us;
		}
		due += (u64)im->cfg.delay_ms * 1000u;
		if (im->cfg.jitter_ms)
			due += rnd(im) % ((u64)im->cfg.jitter_ms * 1000u + 1);
		if (chance(im, im->cfg.reorder)) {
			im->st.reordered++;
			due += (u64)(im->cfg.reorder_ms ? im->cfg.reorder_ms : 10) * 1000u;
		}
		if (due <= now && rt_bufq_empty(&im->q)) {
			im->st.sent++;
			im->orig(nif, x, dsthw, et);
		} else {
			enqueue(im, x, dsthw, et, due);
		}
	}
	return 0;
}

void rt_host_impair(struct rt_netif *nif, const struct rt_host_impair *cfg)
{
	static const struct rt_host_impair zero;
	struct impair *im;
	int off = !cfg || memcmp(cfg, &zero, sizeof(zero)) == 0;
	int i;

	rt_lock();
	im = impair_of(nif);
	if (off) {
		if (im) {
			impair_flush_due(im, ~(u64)0);
			rt_timer_del(&im->timer);
			nif->link_output = im->orig;
			im->nif = NULL;
		}
		rt_unlock();
		return;
	}
	if (!im) {
		for (i = 0; i < IMPAIR_MAX && impairs[i].nif; i++)
			;
		if (i == IMPAIR_MAX) {
			rt_unlock();
			return;
		}
		im = &impairs[i];
		memset(im, 0, sizeof(*im));
		im->nif = nif;
		im->orig = nif->link_output;
		rt_bufq_init(&im->q);
		rt_timer_init(&im->timer, impair_timer, im);
		nif->link_output = impair_output;
	}
	im->cfg = *cfg;
	if (!im->cfg.queue_limit)
		im->cfg.queue_limit = 1000;
	im->rng = cfg->seed ? cfg->seed : (rt_random32() | 1);
	rt_unlock();
}

void rt_host_impair_get_stats(struct rt_netif *nif, struct rt_host_impair_stats *st)
{
	struct impair *im;

	rt_lock();
	im = impair_of(nif);
	if (im)
		*st = im->st;
	else
		memset(st, 0, sizeof(*st));
	rt_unlock();
}
