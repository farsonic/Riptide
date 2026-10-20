/*
 * Riptide - IPv4 fragmentation and reassembly (RFC 791, RFC 815).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Reassembly keeps one queue per (src, dst, id, protocol) with fragments
 * sorted by offset. Like Linux since 4.19 (and RFC 5722 for IPv6), any
 * overlap that is not an exact duplicate discards the whole datagram:
 * overlapping fragments are only ever produced by attackers (teardrop,
 * IDS evasion). Memory is bounded globally; when the high threshold is
 * reached the oldest queues are evicted (Linux ipfrag_high/low_thresh).
 * Incomplete datagrams time out after 30 s (RFC 1122 suggests 60-120 s;
 * BSD and Linux use 30 s) with ICMP time exceeded, code 1.
 */
#include "infra.h"

#define FRAG_TIMEOUT_MS   30000u
#define FRAG_HIGH_THRESH  (256u * 1024u)
#define FRAG_LOW_THRESH   (192u * 1024u)
#define FRAG_MAX_QUEUES   64
#define FRAG_MAX_PER_Q    64      /* fragments per datagram */

/* per fragment info kept in rt_buf.cb */
struct frag_cb {
	u32 off;          /* payload offset */
	u32 end;          /* payload offset + length */
	u32 hlen;         /* IP header length of this fragment */
};
static inline struct frag_cb *FCB(const struct rt_buf *fb)
{
	return (struct frag_cb *)(void *)fb->cb.b;
}

struct frag_q {
	struct frag_q *next;
	u32 saddr, daddr;
	u16 id;
	u8  proto;
	u8  first_in, last_in;
	u32 total;        /* payload length, valid when last_in */
	u32 meat;         /* payload bytes received */
	u32 mem;          /* buffer bytes held */
	struct rt_bufq frags;
	struct rt_timer timer;
};

static struct frag_q *fq_head;     /* oldest first */
static u32 frag_mem;
static u32 frag_nq;

void rt_ipfrag_stats(u32 *queues, u32 *bytes)
{
	if (queues)
		*queues = frag_nq;
	if (bytes)
		*bytes = frag_mem;
}

static void fq_destroy(struct frag_q *q)
{
	struct frag_q **pp;

	for (pp = &fq_head; *pp; pp = &(*pp)->next) {
		if (*pp == q) {
			*pp = q->next;
			break;
		}
	}
	rt_timer_del(&q->timer);
	frag_mem -= q->mem;
	frag_nq--;
	rt_bufq_purge(&q->frags);
	rt_free(q);
}

static void fq_kill(struct frag_q *q)
{
	rt_ipstat.ReasmFails++;
	fq_destroy(q);
}

static void fq_expire(void *arg)
{
	struct frag_q *q = arg;

	rt_ipstat.ReasmTimeout++;
	/* RFC 792: tell the sender, but only if we got fragment zero (it
	 * carries the transport header the error must quote). */
	if (q->first_in && q->frags.head)
		rt_icmp_send_error(q->frags.head, RT_ICMP_TIMXCEED, 1, 0);
	fq_kill(q);
}

static void frag_evict(u32 need)
{
	while (fq_head && frag_mem + need > FRAG_LOW_THRESH)
		fq_kill(fq_head);
}

static struct frag_q *fq_find(const struct rt_iphdr *ip)
{
	struct frag_q *q, **pp;

	for (q = fq_head; q; q = q->next)
		if (q->id == ip->id && q->saddr == ip->saddr && q->daddr == ip->daddr &&
		    q->proto == ip->protocol)
			return q;
	if (frag_nq >= FRAG_MAX_QUEUES)
		fq_kill(fq_head);
	q = rt_zalloc(sizeof(*q));
	if (!q)
		return NULL;
	q->saddr = ip->saddr;
	q->daddr = ip->daddr;
	q->id = ip->id;
	q->proto = ip->protocol;
	rt_bufq_init(&q->frags);
	rt_timer_init(&q->timer, fq_expire, q);
	rt_timer_after(&q->timer, FRAG_TIMEOUT_MS);
	for (pp = &fq_head; *pp; pp = &(*pp)->next)
		;
	*pp = q;
	frag_nq++;
	return q;
}

/* Build the complete datagram. Frees the queue. */
static struct rt_buf *fq_assemble(struct frag_q *q)
{
	struct rt_buf *f = q->frags.head, *b;
	struct rt_iphdr *ip;
	u32 hlen = FCB(f)->hlen;

	if (hlen + q->total > 65535) {
		fq_kill(q);
		return NULL;
	}
	b = rt_buf_alloc(RT_BUF_HEADROOM, hlen + q->total);
	if (!b) {
		fq_kill(q);
		return NULL;
	}
	memcpy(rt_buf_put(b, hlen), f->data, hlen);
	for (; f; f = f->next) {
		struct frag_cb *c = FCB(f);
		memcpy(b->data + hlen + c->off, f->data + c->hlen, c->end - c->off);
	}
	b->len = hlen + q->total;
	b->netif = q->frags.head->netif;
	b->flags = q->frags.head->flags;
	ip = (struct rt_iphdr *)b->data;
	ip->tot_len = rt_htons((u16)b->len);
	ip->frag_off = 0;
	ip->check = 0;
	ip->check = rt_inet_csum(ip, hlen);
	rt_ipstat.ReasmOKs++;
	fq_destroy(q);
	return b;
}

struct rt_buf *rt_ipfrag_reass(struct rt_buf *b)
{
	const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;
	u32 hlen = rt_ip_hlen(ip);
	u16 fo = rt_ntohs(ip->frag_off);
	u32 off = (u32)(fo & RT_IP_OFFMASK) << 3;
	u32 plen = b->len - hlen;
	u32 end = off + plen;
	int more = (fo & RT_IP_MF) != 0;
	struct frag_q *q;
	struct rt_buf *pos, *nx;

	rt_ipstat.ReasmReqds++;
	/* Nonsense geometry: oversize (ping of death), empty or unaligned
	 * middle fragments. */
	if (end + hlen > 65535 || (more && (plen == 0 || (plen & 7)))) {
		rt_ipstat.ReasmFails++;
		rt_buf_free(b);
		return NULL;
	}
	if (frag_mem + b->size > FRAG_HIGH_THRESH)
		frag_evict(b->size);
	q = fq_find(ip);
	if (!q) {
		rt_ipstat.ReasmFails++;
		rt_buf_free(b);
		return NULL;
	}
	if (!more) {
		if ((q->last_in && end != q->total) ||
		    (q->frags.tail && FCB(q->frags.tail)->end > end))
			goto kill;
		q->last_in = 1;
		q->total = end;
	} else if (q->last_in && end > q->total) {
		goto kill;
	}
	if (q->frags.qlen >= FRAG_MAX_PER_Q)
		goto kill;

	/* find the last fragment starting before us */
	for (pos = q->frags.tail; pos && FCB(pos)->off > off; pos = pos->prev)
		;
	nx = pos ? pos->next : q->frags.head;
	if (pos && FCB(pos)->off == off && FCB(pos)->end == end) {
		rt_buf_free(b);          /* exact duplicate: harmless */
		return NULL;
	}
	if ((pos && FCB(pos)->end > off) || (nx && FCB(nx)->off < end))
		goto kill;               /* overlap */

	FCB(b)->off = off;
	FCB(b)->end = end;
	FCB(b)->hlen = hlen;
	rt_bufq_insert_after(&q->frags, pos, b);
	q->meat += plen;
	q->mem += b->size;
	frag_mem += b->size;
	if (off == 0)
		q->first_in = 1;
	if (q->first_in && q->last_in && q->meat == q->total)
		return fq_assemble(q);
	return NULL;
kill:
	rt_buf_free(b);
	fq_kill(q);
	return NULL;
}

/* BSD ip_optcopy(): options with the "copied" bit go into every fragment.
 * Returns the new header length (multiple of 4). */
static u32 ip_optcopy(const u8 *src, u32 hlen, u8 *dst)
{
	u32 i = 20, o = 20;

	memcpy(dst, src, 20);
	while (i < hlen) {
		u8 opt = src[i];
		u32 olen;

		if (opt == 0)                 /* end of list */
			break;
		if (opt == 1) {               /* NOP */
			dst[o++] = 1;
			i++;
			continue;
		}
		if (i + 1 >= hlen)
			break;
		olen = src[i + 1];
		if (olen < 2 || i + olen > hlen)
			break;                /* malformed: stop copying */
		if (opt & 0x80) {
			memcpy(dst + o, src + i, olen);
			o += olen;
		}
		i += olen;
	}
	while (o & 3)
		dst[o++] = 0;
	dst[0] = (u8)(0x40 | (o >> 2));
	return o;
}

int rt_ipfrag_output(struct rt_buf *b, struct rt_netif *nif, u32 nexthop, u32 mtu)
{
	const struct rt_iphdr *ip = (const struct rt_iphdr *)b->data;
	u32 hlen = rt_ip_hlen(ip);
	u32 payload = b->len - hlen;
	u16 fo = rt_ntohs(ip->frag_off);
	u32 base = (u32)(fo & RT_IP_OFFMASK) << 3;
	u8 hdr2[60];
	u32 hlen2, off = 0;
	int r = 0;

	hlen2 = ip_optcopy(b->data, hlen, hdr2);
	if (mtu < hlen + 8 || mtu < hlen2 + 8 || hlen > b->len) {
		rt_ipstat.FragFails++;
		rt_buf_free(b);
		return -RT_EMSGSIZE;
	}
	while (off < payload) {
		const u8 *h = off ? hdr2 : b->data;
		u32 hl = off ? hlen2 : hlen;
		u32 len = (mtu - hl) & ~7u;
		struct rt_buf *f;
		struct rt_iphdr *fip;
		u16 nfo;

		if (len > payload - off)
			len = payload - off;
		f = rt_buf_alloc(RT_BUF_HEADROOM, hl + len);
		if (!f) {
			rt_ipstat.FragFails++;
			r = -RT_ENOBUFS;
			break;
		}
		memcpy(rt_buf_put(f, hl), h, hl);
		memcpy(rt_buf_put(f, len), b->data + hlen + off, len);
		fip = (struct rt_iphdr *)f->data;
		nfo = (u16)((base + off) >> 3);
		if (off + len < payload || (fo & RT_IP_MF))
			nfo |= RT_IP_MF;
		fip->frag_off = rt_htons(nfo);
		fip->tot_len = rt_htons((u16)f->len);
		fip->check = 0;
		fip->check = rt_inet_csum(fip, hl);
		rt_ipstat.FragCreates++;
		r = rt_netif_output_ip(nif, f, nexthop);
		if (r < 0)
			break;
		off += len;
	}
	if (off >= payload)
		rt_ipstat.FragOKs++;
	rt_buf_free(b);
	return r;
}

void rt_ipfrag_init(void)
{
	while (fq_head)
		fq_destroy(fq_head);
	frag_mem = 0;
	frag_nq = 0;
}
