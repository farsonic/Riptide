/*
 * Riptide - packet buffers (a deliberately small cousin of Linux sk_buff).
 *
 * A buffer is one contiguous area: [head .. data .. data+len .. head+size].
 * Headers are prepended with rt_buf_push(), stripped with rt_buf_pull().
 * Every buffer carries a 64 byte control block 'cb' that the current owner
 * layer may use freely (TCP keeps its per-segment state there, UDP keeps
 * the source address of queued datagrams, ...).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_BUF_H
#define RT_BUF_H

#include "rt_types.h"

/* Room reserved in front of every locally generated packet: link (max 32)
 * + IPv4 (max 60) + TCP (max 60) rounded up. */
#define RT_BUF_HEADROOM 160

struct rt_netif;

struct rt_buf {
	struct rt_buf *next;
	struct rt_buf *prev;
	u8  *head;          /* start of allocation */
	u8  *data;          /* start of valid data */
	u32  len;           /* bytes of valid data */
	u32  size;          /* bytes allocated at head */
	struct rt_netif *netif; /* receive interface (input path) */
	u16  flags;         /* RT_BUFF_* */
	u16  l3_proto;      /* ethertype of the payload, host order */
	u32  tstamp_ms;     /* free for owner (e.g. reassembly / queue time) */
	u32  csum;          /* RT_BUFF_CSUM_PART: partial sum of data[0, len) */
	u8   prio;          /* output: socket priority + 1 (rt_qos.h), 0 = none */
	u8   users;         /* clones sharing this buffer's data (rt_buf_clone) */
	u8   orphan;        /* freed while clones were alive: free with the last */
	u8   resv;
	struct rt_buf *owner; /* clone: the buffer whose data it shares */
	union {
		u8  b[64];
		u32 w[16];
		u64 q[8];       /* forces 8 byte alignment of the cb */
	} cb;
};

#define RT_BUFF_BCAST   0x0001  /* received as link broadcast */
#define RT_BUFF_MCAST   0x0002  /* received as link multicast */
#define RT_BUFF_CSUM_OK 0x0004  /* transport checksum already verified */
#define RT_BUFF_LOOP    0x0008  /* came through the loopback interface */
/* The driver summed the frame while copying it in: 'csum' is the one's
 * complement partial sum of data[0, len). It survives pulling a header
 * whose own sum is zero (a valid IPv4 header); every other change to the
 * covered bytes must adjust it or clear the flag (rt_buf_trim() does). */
#define RT_BUFF_CSUM_PART 0x0010

/* Allocate a buffer with 'headroom' bytes in front and room for 'size'
 * bytes of data; data == head + headroom, len == 0. NULL if out of memory.
 * Allocation of MTU sized buffers is served from a free-list cache. */
struct rt_buf *rt_buf_alloc(u32 headroom, u32 size);
void rt_buf_free(struct rt_buf *b);              /* NULL ok */
void rt_buf_free_chain(struct rt_buf *b);        /* follows ->next */
/* Deep copy (data, len, flags, netif, cb). Headroom of copy >= headroom
 * of original. */
struct rt_buf *rt_buf_copy(const struct rt_buf *b);
/* Copy with at least 'headroom' bytes of headroom. */
struct rt_buf *rt_buf_copy_expand(const struct rt_buf *b, u32 headroom);
/* Like rt_buf_copy() but the headroom bytes are not copied, and *sum gets
 * the partial checksum of the data, computed while copying (TCP
 * transmit). */
struct rt_buf *rt_buf_copy_csum(const struct rt_buf *b, u32 *sum);
/* Header-only clone of b (TCP transmit): its own struct (that of a small
 * buffer) with data, len, flags (less RT_BUFF_CSUM_PART), netif and prio
 * of b, sharing b's data area. The cb is not initialised. Headers may be
 * pushed into the shared headroom (b must not have another clone alive:
 * check b->users); the clone has no tailroom, and nobody may write to the
 * shared data. Freeing b while clones are alive defers the free to the
 * last clone's. */
struct rt_buf *rt_buf_clone(struct rt_buf *b);
/* A clone becomes a private copy (frees the clone); other buffers are
 * returned as they are. NULL if out of memory (b is freed). */
struct rt_buf *rt_buf_unshare(struct rt_buf *b);

static inline u32 rt_buf_headroom(const struct rt_buf *b) { return (u32)(b->data - b->head); }
static inline u32 rt_buf_tailroom(const struct rt_buf *b)
{
	return b->size - (u32)(b->data - b->head) - b->len;
}
static inline u8 *rt_buf_tail(const struct rt_buf *b) { return b->data + b->len; }
/* Prepend n bytes; caller guarantees headroom. */
static inline u8 *rt_buf_push(struct rt_buf *b, u32 n) { b->data -= n; b->len += n; return b->data; }
/* Remove n bytes from the front (n <= len). */
static inline u8 *rt_buf_pull(struct rt_buf *b, u32 n) { b->data += n; b->len -= n; return b->data; }
/* Append n bytes at the tail; caller guarantees tailroom. Returns old tail. */
static inline u8 *rt_buf_put(struct rt_buf *b, u32 n) { u8 *t = b->data + b->len; b->len += n; return t; }
static inline void rt_buf_trim(struct rt_buf *b, u32 len)
{
	if (len < b->len) {
		b->len = len;
		b->flags &= (u16)~RT_BUFF_CSUM_PART;
	}
}

/* Statistics of the allocator (for netstat -m). */
struct rt_buf_stats {
	u32 allocated;   /* currently allocated */
	u32 peak;
	u32 cached;      /* on free list */
	u32 failures;
	u32 bytes;       /* bytes currently allocated */
};
void rt_buf_get_stats(struct rt_buf_stats *st);
/* bytes of all allocated buffers and their headers (cheap) */
u32 rt_buf_bytes_total(void);
/* Global cap on buffer memory (bytes); allocation fails beyond it. 0=none */
void rt_buf_set_limit(u32 bytes);

/* ---- Doubly linked buffer queue ---- */
struct rt_bufq {
	struct rt_buf *head;
	struct rt_buf *tail;
	u32 qlen;            /* number of buffers */
	u32 bytes;           /* sum of ->len at enqueue time (owner keeps in sync) */
};

static inline void rt_bufq_init(struct rt_bufq *q) { q->head = q->tail = NULL; q->qlen = 0; q->bytes = 0; }
static inline int  rt_bufq_empty(const struct rt_bufq *q) { return q->head == NULL; }
void rt_bufq_tail(struct rt_bufq *q, struct rt_buf *b);     /* enqueue at tail */
void rt_bufq_headq(struct rt_bufq *q, struct rt_buf *b);    /* enqueue at head */
struct rt_buf *rt_bufq_dequeue(struct rt_bufq *q);          /* from head */
void rt_bufq_unlink(struct rt_bufq *q, struct rt_buf *b);
/* insert 'nb' after 'pos' (pos == NULL: at head) */
void rt_bufq_insert_after(struct rt_bufq *q, struct rt_buf *pos, struct rt_buf *nb);
void rt_bufq_purge(struct rt_bufq *q);                       /* free all */

#endif
