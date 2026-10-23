/*
 * Riptide - packet buffers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A buffer is a single allocation: struct rt_buf followed by its data area.
 * Buffers come in two cached size classes: small ones (ACKs, DNS
 * queries, ARP) and MTU sized ones (an Ethernet frame plus headroom). Each
 * class is recycled through a free list, so the steady state of a busy
 * connection does no rt_malloc() calls. Bigger requests (loopback MTU,
 * reassembled datagrams) are allocated exactly. Small payloads never pin
 * MTU sized memory, which matters for receive queues of datagram sockets.
 *
 * Like the rest of the core, the allocator runs with the stack lock held.
 */
#include "rt/rt_buf.h"
#include "rt/rt_os.h"
#include "rt/rt_arch.h"
#include "rt/rt_diag.h"

#define RT_BUF_HDR_SIZE   ((sizeof(struct rt_buf) + 7u) & ~7u)
#define RT_BUF_CACHE_MAX  64u      /* buffers kept per class */

static const u32 class_size[2] = {
	384u,                          /* RT_BUF_HEADROOM + small payload */
	1792u                          /* >= 1500 + RT_BUF_HEADROOM, multiple of 8 */
};
static struct rt_buf *buf_cache[2];  /* singly linked through ->next */
static u32 buf_ncached[2];
static struct rt_buf_stats buf_stats;
static u32 buf_limit;

/* Data areas start on a 16 byte boundary, so packet buffer copies with
 * equal headroom can use whole cache lines (MOVE16 on the 68040/68060). */
#define RT_BUF_ALIGN      16u

static void buf_reset(struct rt_buf *b, u32 headroom)
{
	b->next = b->prev = NULL;
	b->head = (u8 *)(((uintptr_t)b + RT_BUF_HDR_SIZE + RT_BUF_ALIGN - 1) &
	                 ~(uintptr_t)(RT_BUF_ALIGN - 1));
	b->data = b->head + headroom;
	b->len = 0;
	b->netif = NULL;
	b->flags = 0;
	b->l3_proto = 0;
	b->prio = 0;
	b->users = 0;
	b->orphan = 0;
	b->owner = NULL;
	b->tstamp_ms = 0;
	/* inline stores: a memset() call costs more than the 64 bytes on a
	 * 68020, and every packet allocates two or three buffers */
	{
		u32 *w = b->cb.w;
		int i;

		for (i = 0; i < 16; i += 4) {
			w[i] = 0;
			w[i + 1] = 0;
			w[i + 2] = 0;
			w[i + 3] = 0;
		}
	}
}

static int buf_class(u32 alloc)
{
	return alloc == class_size[0] ? 0 : alloc == class_size[1] ? 1 : -1;
}

/* A buffer of 'alloc' bytes (class cl, or -1) from the cache or the
 * system, counted; not initialised but for 'size'. */
static struct rt_buf *buf_get(u32 alloc, int cl)
{
	struct rt_buf *b;

	if (buf_limit && buf_stats.bytes + alloc > buf_limit) {
		buf_stats.failures++;
		return NULL;
	}
	RT_PROF_EV(RT_EV_BUF);
	if (cl >= 0 && buf_cache[cl]) {
		b = buf_cache[cl];
		buf_cache[cl] = b->next;
		buf_ncached[cl]--;
		buf_stats.cached--;
	} else {
		RT_PROF_EV(RT_EV_BUF_MISS);
		b = rt_malloc(RT_BUF_HDR_SIZE + RT_BUF_ALIGN - 1 + alloc);
		if (!b) {
			buf_stats.failures++;
			return NULL;
		}
		b->size = alloc;
	}
	buf_stats.allocated++;
	buf_stats.bytes += b->size;
	if (buf_stats.allocated > buf_stats.peak)
		buf_stats.peak = buf_stats.allocated;
	return b;
}

struct rt_buf *rt_buf_alloc(u32 headroom, u32 size)
{
	struct rt_buf *b;
	u32 need, alloc;

	/* Reject absurd sizes before they can overflow the sums below. */
	if (headroom > 0x100000u || size > 0x1000000u) {
		buf_stats.failures++;
		return NULL;
	}
	need = headroom + size;
	if (need <= class_size[0])
		alloc = class_size[0];
	else if (need <= class_size[1])
		alloc = class_size[1];
	else
		alloc = (need + 7u) & ~7u;
	b = buf_get(alloc, buf_class(alloc));
	if (b)
		buf_reset(b, headroom);
	return b;
}

void rt_buf_free(struct rt_buf *b)
{
	int cl;

	if (!b)
		return;
	if (b->owner) {
		struct rt_buf *o = b->owner;

		/* back to the small buffer it was allocated as */
		b->owner = NULL;
		b->size = class_size[0];
		rt_buf_free(b);
		if (--o->users == 0 && o->orphan) {
			o->orphan = 0;
			rt_buf_free(o);
		}
		return;
	}
	if (b->users) {
		b->orphan = 1;   /* a driver still sends a clone of it */
		return;
	}
	buf_stats.allocated--;
	buf_stats.bytes -= b->size;
	cl = buf_class(b->size);
	if (cl >= 0 && buf_ncached[cl] < RT_BUF_CACHE_MAX) {
		b->next = buf_cache[cl];
		buf_cache[cl] = b;
		buf_ncached[cl]++;
		buf_stats.cached++;
		return;
	}
	rt_free(b);
}

void rt_buf_free_chain(struct rt_buf *b)
{
	while (b) {
		struct rt_buf *n = b->next;
		rt_buf_free(b);
		b = n;
	}
}

struct rt_buf *rt_buf_copy_expand(const struct rt_buf *b, u32 headroom)
{
	struct rt_buf *c;
	u32 hr = rt_buf_headroom(b);

	if (headroom < hr)
		headroom = hr;
	c = rt_buf_alloc(headroom, b->len + rt_buf_tailroom(b));
	if (!c)
		return NULL;
	rt_copy_buf(c->data, b->data, b->len);
	c->len = b->len;
	c->netif = b->netif;
	c->flags = b->flags;
	c->prio = b->prio;
	c->l3_proto = b->l3_proto;
	c->tstamp_ms = b->tstamp_ms;
	c->csum = b->csum;
	memcpy(&c->cb, &b->cb, sizeof(c->cb));
	/* Callers sometimes need to look back at the IP header that sits in the
	 * headroom (e.g. transport input); keep those bytes too. */
	if (hr)
		rt_copy_buf(c->data - hr, b->data - hr, hr);
	return c;
}

struct rt_buf *rt_buf_copy_csum(const struct rt_buf *b, u32 *sum)
{
	struct rt_buf *c = rt_buf_alloc(rt_buf_headroom(b), b->len + rt_buf_tailroom(b));

	if (!c)
		return NULL;
	*sum = rt_csum_copy(c->data, b->data, b->len, 0);
	c->len = b->len;
	c->netif = b->netif;
	c->flags = b->flags & (u16)~RT_BUFF_CSUM_PART;
	c->prio = b->prio;
	c->l3_proto = b->l3_proto;
	c->tstamp_ms = b->tstamp_ms;
	c->csum = b->csum;
	memcpy(&c->cb, &b->cb, sizeof(c->cb));
	return c;
}

struct rt_buf *rt_buf_clone(struct rt_buf *b)
{
	struct rt_buf *c;

	if (b->users == 0xff)
		return NULL;
	/* a small buffer's struct (its data area unused): from the same
	 * cache as ACKs, so clones add no small allocations of their own,
	 * which would fragment a machine with little memory */
	c = buf_get(class_size[0], 0);
	if (!c)
		return NULL;
	c->next = c->prev = NULL;
	c->head = b->head;
	c->data = b->data;
	c->len = b->len;
	c->size = (u32)(b->data - b->head) + b->len;
	c->netif = b->netif;
	c->flags = b->flags & (u16)~RT_BUFF_CSUM_PART;
	c->l3_proto = b->l3_proto;
	c->tstamp_ms = b->tstamp_ms;
	c->csum = 0;
	c->prio = b->prio;
	c->users = 0;
	c->orphan = 0;
	c->owner = b;
	b->users++;
	return c;
}

struct rt_buf *rt_buf_unshare(struct rt_buf *b)
{
	struct rt_buf *c;

	if (!b->owner)
		return b;
	/* as large as the shared buffer, as the copy TCP used to send was:
	 * equal sizes reuse each other's memory (16 KB loopback segments) */
	c = rt_buf_alloc(rt_buf_headroom(b), b->owner->size - rt_buf_headroom(b));
	if (c) {
		rt_copy_buf(c->data, b->data, b->len);
		c->len = b->len;
		c->netif = b->netif;
		c->flags = b->flags;
		c->prio = b->prio;
		c->l3_proto = b->l3_proto;
		c->tstamp_ms = b->tstamp_ms;
		c->csum = b->csum;
		memcpy(&c->cb, &b->cb, sizeof(c->cb));
	}
	rt_buf_free(b);
	return c;
}

struct rt_buf *rt_buf_copy(const struct rt_buf *b)
{
	return rt_buf_copy_expand(b, rt_buf_headroom(b));
}

void rt_buf_get_stats(struct rt_buf_stats *st)
{
	*st = buf_stats;
}

/* every allocated buffer with its header, bytes: an upper bound of what
 * the sockets hold (tcp_mem.c) */
u32 rt_buf_bytes_total(void)
{
	return buf_stats.bytes + buf_stats.allocated * (u32)sizeof(struct rt_buf);
}

void rt_buf_set_limit(u32 bytes)
{
	buf_limit = bytes;
}

/* ---- queue ---- */

void rt_bufq_tail(struct rt_bufq *q, struct rt_buf *b)
{
	b->next = NULL;
	b->prev = q->tail;
	if (q->tail)
		q->tail->next = b;
	else
		q->head = b;
	q->tail = b;
	q->qlen++;
	q->bytes += b->len;
}

void rt_bufq_headq(struct rt_bufq *q, struct rt_buf *b)
{
	b->prev = NULL;
	b->next = q->head;
	if (q->head)
		q->head->prev = b;
	else
		q->tail = b;
	q->head = b;
	q->qlen++;
	q->bytes += b->len;
}

void rt_bufq_unlink(struct rt_bufq *q, struct rt_buf *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		q->head = b->next;
	if (b->next)
		b->next->prev = b->prev;
	else
		q->tail = b->prev;
	b->next = b->prev = NULL;
	q->qlen--;
	q->bytes = q->bytes >= b->len ? q->bytes - b->len : 0;
}

struct rt_buf *rt_bufq_dequeue(struct rt_bufq *q)
{
	struct rt_buf *b = q->head;

	if (b)
		rt_bufq_unlink(q, b);
	return b;
}

void rt_bufq_insert_after(struct rt_bufq *q, struct rt_buf *pos, struct rt_buf *nb)
{
	if (!pos) {
		rt_bufq_headq(q, nb);
		return;
	}
	nb->prev = pos;
	nb->next = pos->next;
	if (pos->next)
		pos->next->prev = nb;
	else
		q->tail = nb;
	pos->next = nb;
	q->qlen++;
	q->bytes += nb->len;
}

void rt_bufq_purge(struct rt_bufq *q)
{
	struct rt_buf *b;

	while ((b = rt_bufq_dequeue(q)) != NULL)
		rt_buf_free(b);
	q->bytes = 0;
}

/* Not in the contract: lets tests verify that nothing leaked. */
void rt_buf_drain_cache(void);
void rt_buf_drain_cache(void)
{
	int cl;

	for (cl = 0; cl < 2; cl++) {
		while (buf_cache[cl]) {
			struct rt_buf *b = buf_cache[cl];
			buf_cache[cl] = b->next;
			rt_free(b);
		}
		buf_stats.cached -= buf_ncached[cl];
		buf_ncached[cl] = 0;
	}
}
