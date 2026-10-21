/*
 * Riptide - IPv6 fragmentation (source only) and reassembly (RFC 8200 4.5).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Reassembly queues are keyed by (source, destination, identification).
 * Overlapping fragments discard the whole datagram (RFC 5722), the first
 * fragment must hold the whole header chain (RFC 7112, checked by the
 * caller when it walks the reassembled packet: we only check that the
 * upper layer header starts in it), and the memory used is bounded with
 * the oldest datagrams evicted first. Incomplete datagrams time out after
 * 60 s (RFC 8200) with an ICMPv6 time exceeded if fragment zero arrived.
 */
#include "ip6_internal.h"

#define FRAG6_TIMEOUT_MS   60000u
#define FRAG6_HIGH_THRESH  (256u * 1024u)
#define FRAG6_LOW_THRESH   (192u * 1024u)
#define FRAG6_MAX_QUEUES   64
#define FRAG6_MAX_PER_Q    64

struct f6cb {
	u32 off, end;         /* payload offsets of this fragment */
	u32 hoff;             /* offset of its fragmentable part in the buffer */
};
static inline struct f6cb *F6(const struct rt_buf *fb)
{
	return (struct f6cb *)(void *)fb->cb.b;
}

struct f6q {
	struct f6q *next;
	rt_in6_addr src, dst;
	u32 id;
	u8  first_in, last_in;
	u8  nxt;              /* next header after the fragment header (first fragment) */
	u32 unfrag;           /* bytes of the unfragmentable part (first fragment) */
	u32 prev_nxt;         /* offset of the byte naming the fragment header */
	u32 total, meat, mem;
	struct rt_bufq frags;
	struct rt_timer timer;
};

static struct f6q *fq6_head;
static u32 frag6_mem, frag6_nq;

void rt_ip6frag_stats(u32 *queues, u32 *bytes);
void rt_ip6frag_stats(u32 *queues, u32 *bytes)
{
	if (queues)
		*queues = frag6_nq;
	if (bytes)
		*bytes = frag6_mem;
}

static void fq6_destroy(struct f6q *q)
{
	struct f6q **pp;

	for (pp = &fq6_head; *pp; pp = &(*pp)->next) {
		if (*pp == q) {
			*pp = q->next;
			break;
		}
	}
	rt_timer_del(&q->timer);
	frag6_mem -= q->mem;
	frag6_nq--;
	rt_bufq_purge(&q->frags);
	rt_free(q);
}

static void fq6_kill(struct f6q *q)
{
	rt_ip6stat.ReasmFails++;
	fq6_destroy(q);
}

static void fq6_expire(void *arg)
{
	struct f6q *q = arg;

	rt_ip6stat.ReasmTimeout++;
	if (q->first_in && q->frags.head && F6(q->frags.head)->off == 0)
		rt_icmp6_send_error(q->frags.head, RT_ICMP6_TIME_EXCEEDED,
		                    RT_ICMP6_TIME_EXCEED_REASSEMBLY, 0);
	fq6_kill(q);
}

static struct f6q *fq6_find(const struct rt_ip6hdr *ip6, u32 id)
{
	struct f6q *q, **pp;

	for (q = fq6_head; q; q = q->next)
		if (q->id == id && rt_in6_eq(&q->src, &ip6->src) && rt_in6_eq(&q->dst, &ip6->dst))
			return q;
	if (frag6_nq >= FRAG6_MAX_QUEUES)
		fq6_kill(fq6_head);
	q = rt_zalloc(sizeof(*q));
	if (!q)
		return NULL;
	q->src = ip6->src;
	q->dst = ip6->dst;
	q->id = id;
	rt_bufq_init(&q->frags);
	rt_timer_init(&q->timer, fq6_expire, q);
	rt_timer_after(&q->timer, FRAG6_TIMEOUT_MS);
	for (pp = &fq6_head; *pp; pp = &(*pp)->next)
		;
	*pp = q;
	frag6_nq++;
	return q;
}

static struct rt_buf *fq6_assemble(struct f6q *q)
{
	struct rt_buf *first = q->frags.head, *f, *b;
	struct rt_ip6hdr *ip6;
	u32 len = q->unfrag + q->total;

	if (len - RT_IP6_HLEN > 65535) {
		fq6_kill(q);
		return NULL;
	}
	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b) {
		fq6_kill(q);
		return NULL;
	}
	memcpy(rt_buf_put(b, q->unfrag), first->data, q->unfrag);
	for (f = first; f; f = f->next) {
		struct f6cb *c = F6(f);
		rt_copy_buf(b->data + q->unfrag + c->off, f->data + c->hoff, c->end - c->off);
	}
	b->len = len;
	b->netif = first->netif;
	b->flags = first->flags;
	b->l3_proto = first->l3_proto;
	b->data[q->prev_nxt] = q->nxt;
	ip6 = (struct rt_ip6hdr *)b->data;
	ip6->plen = rt_htons((u16)(len - RT_IP6_HLEN));
	rt_ip6stat.ReasmOKs++;
	fq6_destroy(q);
	return b;
}

struct rt_buf *ip6frag_reass(struct rt_buf *b, u32 fhoff, u32 prev_nxt_off)
{
	const struct rt_ip6hdr *ip6 = (const struct rt_ip6hdr *)b->data;
	const u8 *fh = b->data + fhoff;
	u16 fo = rt_get_be16(fh + 2);
	u32 id = rt_get_be32(fh + 4);
	u32 off = fo & 0xfff8u;
	int more = fo & 1;
	u32 hoff = fhoff + 8;
	u32 plen = b->len - hoff;
	u32 end = off + plen;
	struct f6q *q;
	struct rt_buf *pos, *nx;

	rt_ip6stat.ReasmReqds++;
	if (more && (plen & 7)) {
		/* RFC 8200 4.5: payload length must be a multiple of 8 */
		rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_HEADER, 4);
		goto fail;
	}
	if (fhoff - RT_IP6_HLEN + 8 + end > 65535) {
		rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, RT_ICMP6_PARAMPROB_HEADER, fhoff + 2);
		goto fail;
	}
	if (more && plen == 0)
		goto fail;
	if (off == 0) {
		/* RFC 7112: the first fragment must contain the upper layer
		 * header; walk the chain after the fragment header. */
		u8 n = fh[0];
		u32 o = hoff;
		int hops = 0;
		while (n == RT_IPPROTO_HOPOPTS || n == RT_IPPROTO_DSTOPTS || n == RT_IPPROTO_ROUTING) {
			if (o + 8 > b->len || ++hops > 8)
				goto fail;
			n = b->data[o];
			o += ((u32)b->data[o + 1] + 1) * 8;
		}
		if (o > b->len || (n != RT_IPPROTO_NONE && o >= b->len)) {
			rt_icmp6_send_error(b, RT_ICMP6_PARAM_PROB, 3 /* RFC 7112 */, 0);
			goto fail;
		}
	}
	if (frag6_mem + b->size > FRAG6_HIGH_THRESH)
		while (fq6_head && frag6_mem + b->size > FRAG6_LOW_THRESH)
			fq6_kill(fq6_head);
	q = fq6_find(ip6, id);
	if (!q)
		goto fail;
	if (!more) {
		if ((q->last_in && end != q->total) || (q->frags.tail && F6(q->frags.tail)->end > end))
			goto kill;
		q->last_in = 1;
		q->total = end;
	} else if (q->last_in && end > q->total) {
		goto kill;
	}
	if (q->frags.qlen >= FRAG6_MAX_PER_Q)
		goto kill;
	for (pos = q->frags.tail; pos && F6(pos)->off > off; pos = pos->prev)
		;
	nx = pos ? pos->next : q->frags.head;
	if (pos && F6(pos)->off == off && F6(pos)->end == end) {
		rt_buf_free(b);             /* exact duplicate */
		return NULL;
	}
	if ((pos && F6(pos)->end > off) || (nx && F6(nx)->off < end))
		goto kill;                  /* RFC 5722 */
	F6(b)->off = off;
	F6(b)->end = end;
	F6(b)->hoff = hoff;
	if (off == 0) {
		q->first_in = 1;
		q->nxt = fh[0];
		q->unfrag = fhoff;
		q->prev_nxt = prev_nxt_off;
	}
	rt_bufq_insert_after(&q->frags, pos, b);
	q->meat += plen;
	q->mem += b->size;
	frag6_mem += b->size;
	if (q->first_in && q->last_in && q->meat == q->total)
		return fq6_assemble(q);
	return NULL;
kill:
	rt_buf_free(b);
	fq6_kill(q);
	return NULL;
fail:
	rt_ip6stat.ReasmFails++;
	rt_buf_free(b);
	return NULL;
}

int ip6frag_output(struct rt_buf *b, u32 unfrag_len, struct rt_netif *nif,
                   const rt_in6_addr *nexthop, u32 mtu)
{
	u32 payload = b->len - unfrag_len, off = 0, id;
	u32 chunk = (mtu - unfrag_len - 8) & ~7u;
	u32 nxt_off = unfrag_len > RT_IP6_HLEN ? RT_IP6_HLEN : 6;
	u8 nxt = b->data[nxt_off];
	int r = 0;

	if (mtu < unfrag_len + 8 + 8 || unfrag_len > b->len) {
		rt_ip6stat.FragFails++;
		rt_buf_free(b);
		return -RT_EMSGSIZE;
	}
	do
		id = rt_random32();
	while (id == 0);
	while (off < payload) {
		u32 len = payload - off > chunk ? chunk : payload - off;
		struct rt_buf *f = rt_buf_alloc(RT_BUF_HEADROOM, unfrag_len + 8 + len);
		struct rt_ip6hdr *ip6;
		u8 *fh;

		if (!f) {
			rt_ip6stat.FragFails++;
			r = -RT_ENOBUFS;
			break;
		}
		memcpy(rt_buf_put(f, unfrag_len), b->data, unfrag_len);
		f->data[nxt_off] = RT_IPPROTO_FRAGMENT;
		fh = rt_buf_put(f, 8);
		fh[0] = nxt;
		fh[1] = 0;
		rt_put_be16(fh + 2, (u16)(off | (off + len < payload ? 1u : 0u)));
		rt_put_be32(fh + 4, id);
		rt_copy_buf(rt_buf_put(f, len), b->data + unfrag_len + off, len);
		ip6 = (struct rt_ip6hdr *)f->data;
		ip6->plen = rt_htons((u16)(f->len - RT_IP6_HLEN));
		rt_ip6stat.FragCreates++;
		r = ip6_send_link(nif, f, nexthop);
		if (r < 0)
			break;
		off += len;
	}
	if (off >= payload)
		rt_ip6stat.FragOKs++;
	rt_buf_free(b);
	return r;
}

void ip6frag_init(void)
{
	while (fq6_head)
		fq6_destroy(fq6_head);
	frag6_mem = 0;
	frag6_nq = 0;
}
