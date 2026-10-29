/*
 * Riptide - Berkeley Packet Filter channels (see rt_bpf.h).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The filter machine and its validator follow the classic BSD semantics
 * (McCanne & Jacobson): forward jumps only, a return at the end, no
 * division by a constant zero, scratch memory indices in range; loads
 * outside the packet reject it.
 */
#include "infra.h"
#include "rt/rt_bpf.h"

u8 rt_bpf_on;
u32 rt_bpf_bufsize = RT_BPF_BUFSIZE;
void (*rt_bpf_signal)(void *task, u32 mask);

/* ---- the filter machine ---- */

int rt_bpf_validate(const struct rt_bpf_insn *f, u32 n)
{
	u32 i;

	if (!f || n == 0 || n > RT_BPF_MAXINSNS)
		return -RT_EINVAL;
	for (i = 0; i < n; i++) {
		const struct rt_bpf_insn *p = &f[i];
		switch (RT_BPF_CLASS(p->code)) {
		case RT_BPF_LD:
		case RT_BPF_LDX:
			switch (RT_BPF_MODE(p->code)) {
			case RT_BPF_IMM:
			case RT_BPF_ABS:
			case RT_BPF_IND:
			case RT_BPF_LEN:
				break;
			case RT_BPF_MSH:
				if (RT_BPF_CLASS(p->code) != RT_BPF_LDX)
					return -RT_EINVAL;
				break;
			case RT_BPF_MEM:
				if (p->k >= RT_BPF_MEMWORDS)
					return -RT_EINVAL;
				break;
			default:
				return -RT_EINVAL;
			}
			break;
		case RT_BPF_ST:
		case RT_BPF_STX:
			if (p->k >= RT_BPF_MEMWORDS)
				return -RT_EINVAL;
			break;
		case RT_BPF_ALU:
			switch (RT_BPF_OP(p->code)) {
			case RT_BPF_ADD: case RT_BPF_SUB: case RT_BPF_MUL: case RT_BPF_OR:
			case RT_BPF_AND: case RT_BPF_LSH: case RT_BPF_RSH: case RT_BPF_NEG:
				break;
			case RT_BPF_DIV:
				if (RT_BPF_SRC(p->code) == RT_BPF_K && p->k == 0)
					return -RT_EINVAL;
				break;
			default:
				return -RT_EINVAL;
			}
			break;
		case RT_BPF_JMP:
			if (RT_BPF_OP(p->code) == RT_BPF_JA) {
				if (p->k >= n - i - 1)
					return -RT_EINVAL;
			} else {
				switch (RT_BPF_OP(p->code)) {
				case RT_BPF_JEQ: case RT_BPF_JGT: case RT_BPF_JGE: case RT_BPF_JSET:
					break;
				default:
					return -RT_EINVAL;
				}
				if ((u32)p->jt >= n - i - 1 || (u32)p->jf >= n - i - 1)
					return -RT_EINVAL;
			}
			break;
		case RT_BPF_RET:
		case RT_BPF_MISC:
			break;
		}
	}
	return RT_BPF_CLASS(f[n - 1].code) == RT_BPF_RET ? 0 : -RT_EINVAL;
}

/* A packet in up to two pieces (link header, payload). */
struct pkt {
	const u8 *a, *b;
	u32 alen, blen;
};

static int load(const struct pkt *p, u32 off, int size, u32 *v)
{
	u32 r = 0;
	int i;

	if (off >= p->alen + p->blen || (u32)size > p->alen + p->blen - off)
		return -1;
	for (i = 0; i < size; i++, off++)
		r = (r << 8) | (off < p->alen ? p->a[off] : p->b[off - p->alen]);
	*v = r;
	return 0;
}

static u32 run(const struct rt_bpf_insn *pc, const struct pkt *p, u32 wirelen);

u32 rt_bpf_filter(const struct rt_bpf_insn *f, const u8 *p, u32 wirelen, u32 buflen)
{
	struct pkt pk;

	pk.a = p;
	pk.alen = buflen;
	pk.b = NULL;
	pk.blen = 0;
	return f ? run(f, &pk, wirelen) : (u32)-1;
}

static u32 run(const struct rt_bpf_insn *pc, const struct pkt *p, u32 wirelen)
{
	u32 A = 0, X = 0, k, mem[RT_BPF_MEMWORDS];
	int size;

	memset(mem, 0, sizeof(mem));
	for (;; pc++) {
		switch (pc->code) {
		case RT_BPF_RET | RT_BPF_K:
			return pc->k;
		case RT_BPF_RET | RT_BPF_A:
			return A;
		case RT_BPF_LD | RT_BPF_W | RT_BPF_ABS:
		case RT_BPF_LD | RT_BPF_H | RT_BPF_ABS:
		case RT_BPF_LD | RT_BPF_B | RT_BPF_ABS:
		case RT_BPF_LD | RT_BPF_W | RT_BPF_IND:
		case RT_BPF_LD | RT_BPF_H | RT_BPF_IND:
		case RT_BPF_LD | RT_BPF_B | RT_BPF_IND:
			size = RT_BPF_SIZE(pc->code) == RT_BPF_W ? 4 : RT_BPF_SIZE(pc->code) == RT_BPF_H ? 2 : 1;
			k = pc->k;
			if (RT_BPF_MODE(pc->code) == RT_BPF_IND) {
				if (k + X < k)
					return 0;
				k += X;
			}
			if (load(p, k, size, &A) < 0)
				return 0;
			continue;
		case RT_BPF_LD | RT_BPF_W | RT_BPF_LEN:
			A = wirelen;
			continue;
		case RT_BPF_LDX | RT_BPF_W | RT_BPF_LEN:
			X = wirelen;
			continue;
		case RT_BPF_LDX | RT_BPF_B | RT_BPF_MSH:
			if (load(p, pc->k, 1, &k) < 0)
				return 0;
			X = (k & 0xf) << 2;
			continue;
		case RT_BPF_LD | RT_BPF_IMM:
			A = pc->k;
			continue;
		case RT_BPF_LDX | RT_BPF_IMM:
			X = pc->k;
			continue;
		case RT_BPF_LD | RT_BPF_MEM:
			A = mem[pc->k];
			continue;
		case RT_BPF_LDX | RT_BPF_MEM:
			X = mem[pc->k];
			continue;
		case RT_BPF_ST:
			mem[pc->k] = A;
			continue;
		case RT_BPF_STX:
			mem[pc->k] = X;
			continue;
		case RT_BPF_JMP | RT_BPF_JA:
			pc += pc->k;
			continue;
		case RT_BPF_JMP | RT_BPF_JGT | RT_BPF_K:
			pc += A > pc->k ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JGE | RT_BPF_K:
			pc += A >= pc->k ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_K:
			pc += A == pc->k ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JSET | RT_BPF_K:
			pc += (A & pc->k) ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JGT | RT_BPF_X:
			pc += A > X ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JGE | RT_BPF_X:
			pc += A >= X ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JEQ | RT_BPF_X:
			pc += A == X ? pc->jt : pc->jf;
			continue;
		case RT_BPF_JMP | RT_BPF_JSET | RT_BPF_X:
			pc += (A & X) ? pc->jt : pc->jf;
			continue;
		case RT_BPF_ALU | RT_BPF_ADD | RT_BPF_X: A += X; continue;
		case RT_BPF_ALU | RT_BPF_SUB | RT_BPF_X: A -= X; continue;
		case RT_BPF_ALU | RT_BPF_MUL | RT_BPF_X: A *= X; continue;
		case RT_BPF_ALU | RT_BPF_DIV | RT_BPF_X:
			if (X == 0)
				return 0;
			A /= X;
			continue;
		case RT_BPF_ALU | RT_BPF_AND | RT_BPF_X: A &= X; continue;
		case RT_BPF_ALU | RT_BPF_OR | RT_BPF_X:  A |= X; continue;
		case RT_BPF_ALU | RT_BPF_LSH | RT_BPF_X: A = X < 32 ? A << X : 0; continue;
		case RT_BPF_ALU | RT_BPF_RSH | RT_BPF_X: A = X < 32 ? A >> X : 0; continue;
		case RT_BPF_ALU | RT_BPF_ADD | RT_BPF_K: A += pc->k; continue;
		case RT_BPF_ALU | RT_BPF_SUB | RT_BPF_K: A -= pc->k; continue;
		case RT_BPF_ALU | RT_BPF_MUL | RT_BPF_K: A *= pc->k; continue;
		case RT_BPF_ALU | RT_BPF_DIV | RT_BPF_K: A /= pc->k; continue;
		case RT_BPF_ALU | RT_BPF_AND | RT_BPF_K: A &= pc->k; continue;
		case RT_BPF_ALU | RT_BPF_OR | RT_BPF_K:  A |= pc->k; continue;
		case RT_BPF_ALU | RT_BPF_LSH | RT_BPF_K: A = pc->k < 32 ? A << pc->k : 0; continue;
		case RT_BPF_ALU | RT_BPF_RSH | RT_BPF_K: A = pc->k < 32 ? A >> pc->k : 0; continue;
		case RT_BPF_ALU | RT_BPF_NEG:
			A = (u32)-(s32)A;
			continue;
		case RT_BPF_MISC | RT_BPF_TAX:
			X = A;
			continue;
		case RT_BPF_MISC | RT_BPF_TXA:
			A = X;
			continue;
		default:
			return 0;             /* the validator lets no such code through */
		}
	}
}

/* ---- channels ---- */

struct bpf_chan {
	void *owner;               /* NULL: free */
	u32   ifindex;             /* 0: not attached */
	u8    dlt, immediate, promisc;
	u8   *sbuf, *hbuf;         /* store and hold buffer */
	u32   slen, hlen, blen;
	u32   rtimeout_ms;         /* 0: none */
	u32   recv, drop;
	struct rt_bpf_insn *filter;
	struct rt_waitq wq;
	void *ntask;
	u32   nmask, imask;
};

static struct bpf_chan chans[RT_BPF_NCHAN];

static void update_on(void)
{
	int i;

	rt_bpf_on = 0;
	for (i = 0; i < RT_BPF_NCHAN; i++)
		if (chans[i].owner && chans[i].ifindex)
			rt_bpf_on = 1;
}

static int chan_get(int ch, void *owner, struct bpf_chan **out)
{
	if (ch < 0 || ch >= RT_BPF_NCHAN || !chans[ch].owner)
		return -RT_ENXIO;
	if (chans[ch].owner != owner)
		return -RT_EPERM;
	*out = &chans[ch];
	return 0;
}

static void reset_buffers(struct bpf_chan *c)
{
	c->slen = c->hlen = 0;
	c->recv = c->drop = 0;
}

static int alloc_buffers(struct bpf_chan *c)
{
	u8 *s = rt_malloc(c->blen), *h = rt_malloc(c->blen);

	if (!s || !h) {
		rt_free(s);
		rt_free(h);
		return -RT_ENOBUFS;
	}
	rt_free(c->sbuf);
	rt_free(c->hbuf);
	c->sbuf = s;
	c->hbuf = h;
	reset_buffers(c);
	return 0;
}

static void set_promisc(struct bpf_chan *c, int on)
{
	struct rt_netif *nif = c->ifindex ? rt_netif_by_index(c->ifindex) : NULL;
	int i, others = 0;
	u16 nf;

	if (!c->promisc || !nif)
		return;
	if (!on) {
		for (i = 0; i < RT_BPF_NCHAN; i++)
			if (&chans[i] != c && chans[i].owner && chans[i].promisc && chans[i].ifindex == c->ifindex)
				others = 1;
		if (others)
			return;
	}
	nf = on ? (u16)(nif->flags | RT_IFF_PROMISC) : (u16)(nif->flags & ~RT_IFF_PROMISC);
	if (nf != nif->flags && nif->ioctl_flags)
		nif->ioctl_flags(nif, nf);
	nif->flags = nf;
}

static void detach(struct bpf_chan *c)
{
	set_promisc(c, 0);
	c->promisc = 0;
	c->ifindex = 0;
	update_on();
}

int rt_bpf_open(int ch, void *owner)
{
	struct bpf_chan *c;

	if (!owner)
		return -RT_EINVAL;
	if (ch < 0) {
		for (ch = 0; ch < RT_BPF_NCHAN && chans[ch].owner; ch++)
			;
		if (ch == RT_BPF_NCHAN)
			return -RT_EBUSY;
	} else if (ch >= RT_BPF_NCHAN) {
		return -RT_ENXIO;
	} else if (chans[ch].owner) {
		return -RT_EBUSY;
	}
	c = &chans[ch];
	memset(c, 0, sizeof(*c));
	c->blen = rt_bpf_bufsize;
	if (c->blen < RT_BPF_MINBUFSIZE || c->blen > RT_BPF_MAXBUFSIZE)
		c->blen = RT_BPF_BUFSIZE;
	rt_waitq_init(&c->wq);
	c->owner = owner;
	return ch;
}

static void chan_free(struct bpf_chan *c)
{
	detach(c);
	rt_free(c->sbuf);
	rt_free(c->hbuf);
	rt_free(c->filter);
	c->sbuf = c->hbuf = NULL;
	c->filter = NULL;
	c->owner = NULL;
	rt_waitq_wake(&c->wq);
}

int rt_bpf_close(int ch, void *owner)
{
	struct bpf_chan *c;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	chan_free(c);
	return 0;
}

void rt_bpf_close_owner(void *owner)
{
	int i;

	if (!owner)
		return;
	for (i = 0; i < RT_BPF_NCHAN; i++)
		if (chans[i].owner == owner)
			chan_free(&chans[i]);
}

static void rotate(struct bpf_chan *c)
{
	u8 *t = c->hbuf;

	c->hbuf = c->sbuf;
	c->hlen = c->slen;
	c->sbuf = t;
	c->slen = 0;
}

static void notify(struct bpf_chan *c)
{
	rt_waitq_wake(&c->wq);
	if (c->nmask && c->ntask && rt_bpf_signal)
		rt_bpf_signal(c->ntask, c->nmask);
}

int rt_bpf_waiting(int ch, void *owner)
{
	struct bpf_chan *c;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	return c->hlen || (c->slen && c->immediate) ? 1 : 0;
}

int rt_bpf_read(int ch, void *owner, void *buf, u32 len, u32 *intr)
{
	struct bpf_chan *c;
	struct rt_wait_entry we;
	u32 n, sig;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	if (len != c->blen || !buf)
		return -RT_EINVAL;
	if (!c->ifindex || !c->sbuf)
		return -RT_EINVAL;
	while (c->hlen == 0) {
		if (c->slen && c->immediate) {
			rotate(c);
			break;
		}
		sig = intr ? *intr : 0;
		rt_waitq_add(&c->wq, &we, rt_sleeper_self());
		r = rt_sleeper_wait(we.sleeper, c->rtimeout_ms ? (s32)c->rtimeout_ms : -1, &sig);
		rt_waitq_remove(&c->wq, &we);
		/* closed or detached meanwhile (library expunge): look again */
		if (chan_get(ch, owner, &c) < 0 || !c->ifindex)
			return -RT_ENXIO;
		if (sig) {
			if (intr)
				*intr = sig;
			return -RT_EINTR;
		}
		if (r == -RT_EINTR)
			return r;
		if (r == -RT_ETIMEDOUT) {
			if (c->hlen)
				break;
			if (!c->slen)
				return 0;
			rotate(c);
			break;
		}
	}
	n = c->hlen;
	memcpy(buf, c->hbuf, n);
	c->hlen = 0;
	return (int)n;
}

static void put32(u8 *p, u32 v)
{
	memcpy(p, &v, 4);
}

/* Store one packet (link header lh of lhlen bytes, then the payload). */
static void catch_packet(struct bpf_chan *c, const u8 *lh, u32 lhlen, const u8 *pl, u32 pllen, u32 snap)
{
	u32 wirelen = lhlen + pllen, caplen = RT_MIN(snap, wirelen);
	u32 hdrlen = RT_BPF_WORDALIGN(lhlen + RT_BPF_HDRLEN) - lhlen;
	u32 tot = hdrlen + caplen, cur, sec, usec;
	u16 hl = (u16)hdrlen;
	u8 *h;

	if (tot > c->blen) {
		tot = c->blen;
		caplen = tot - hdrlen;
	}
	cur = RT_BPF_WORDALIGN(c->slen);
	if (cur + tot > c->blen) {
		if (c->hlen) {
			c->drop++;
			return;
		}
		rotate(c);
		cur = 0;
		notify(c);
	}
	h = c->sbuf + cur;
	rt_wallclock_tv(&sec, &usec);
	put32(h, sec);
	put32(h + 4, usec);
	put32(h + 8, caplen);
	put32(h + 12, wirelen);
	memcpy(h + 16, &hl, 2);
	memset(h + RT_BPF_HDRLEN, 0, hdrlen - RT_BPF_HDRLEN);
	h += hdrlen;
	if (caplen <= lhlen) {
		memcpy(h, lh, caplen);
	} else {
		memcpy(h, lh, lhlen);
		memcpy(h + lhlen, pl, caplen - lhlen);
	}
	c->slen = cur + tot;
	if (c->immediate)
		notify(c);
}

/* the multicast MAC of an IPv4 or IPv6 destination */
static int mcast_mac(const struct rt_buf *b, u16 type, u8 *mac)
{
	if (type == RT_ETH_P_IP && b->len >= 20) {
		mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
		mac[3] = b->data[17] & 0x7f; mac[4] = b->data[18]; mac[5] = b->data[19];
		return 1;
	}
	if (type == RT_ETH_P_IPV6 && b->len >= 40) {
		mac[0] = mac[1] = 0x33;
		memcpy(mac + 2, b->data + 36, 4);
		return 1;
	}
	return 0;
}

void rt_bpf_tap(struct rt_netif *nif, const struct rt_buf *b, int out, u16 ethertype,
                const u8 *srchw, const u8 *dsthw, u16 flags)
{
	u8 lh[14];
	u32 lhlen, snap;
	int i, ether = nif->type == RT_IFT_ETHER;

	if (out && nif->type == RT_IFT_LOOP)
		return;                    /* seen when lo0 delivers it */
	if (ether) {
		static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		u8 *d = lh, *s = lh + 6;
		if (out) {
			if (dsthw)
				memcpy(d, dsthw, 6);
			else if (!mcast_mac(b, ethertype, d))
				memcpy(d, bcast, 6);
			memcpy(s, nif->hwaddr, 6);
		} else {
			if (flags & RT_BUFF_BCAST)
				memcpy(d, bcast, 6);
			else if (!(flags & RT_BUFF_MCAST) || !mcast_mac(b, ethertype, d))
				memcpy(d, nif->hwaddr, 6);
			if (srchw)
				memcpy(s, srchw, 6);
			else
				memset(s, 0, 6);
		}
		rt_put_be16(lh + 12, ethertype);
		lhlen = 14;
	} else {
		u32 af = ethertype == RT_ETH_P_IPV6 ? RT_AF_INET6 : RT_AF_INET;
		if (ethertype != RT_ETH_P_IP && ethertype != RT_ETH_P_IPV6)
			return;
		memcpy(lh, &af, 4);
		lhlen = 4;
	}
	for (i = 0; i < RT_BPF_NCHAN; i++) {
		struct bpf_chan *c = &chans[i];
		if (!c->owner || c->ifindex != nif->index || !c->sbuf)
			continue;
		c->recv++;
		if (c->filter) {
			struct pkt pk;
			pk.a = lh;
			pk.alen = lhlen;
			pk.b = b->data;
			pk.blen = b->len;
			snap = run(c->filter, &pk, lhlen + b->len);
		} else {
			snap = (u32)-1;
		}
		if (snap)
			catch_packet(c, lh, lhlen, b->data, b->len, snap);
	}
}

void rt_bpf_if_gone(struct rt_netif *nif)
{
	int i;

	for (i = 0; i < RT_BPF_NCHAN; i++) {
		if (chans[i].owner && chans[i].ifindex == nif->index) {
			chans[i].promisc = 0;
			chans[i].ifindex = 0;
			rt_waitq_wake(&chans[i].wq);
		}
	}
	update_on();
}

int rt_bpf_write(int ch, void *owner, const void *buf, u32 len)
{
	struct bpf_chan *c;
	struct rt_netif *nif;
	struct rt_buf *b;
	const u8 *p = buf, *dst = NULL;
	u32 hl;
	u16 type;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	nif = c->ifindex ? rt_netif_by_index(c->ifindex) : NULL;
	if (!nif)
		return -RT_EINVAL;
	if (!buf)
		return -RT_EFAULT;
	if (c->dlt == RT_DLT_EN10MB) {
		static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		hl = 14;
		if (len < hl)
			return -RT_EINVAL;
		if (memcmp(p, bcast, 6))
			dst = p;
		type = rt_get_be16(p + 12);
	} else {
		u32 af;
		hl = 4;
		if (len < hl)
			return -RT_EINVAL;
		memcpy(&af, p, 4);
		if (af == RT_AF_INET)
			type = RT_ETH_P_IP;
		else if (af == RT_AF_INET6)
			type = RT_ETH_P_IPV6;
		else
			return -RT_EAFNOSUPPORT;
	}
	if (len - hl > nif->mtu)
		return -RT_EMSGSIZE;
	if (!(nif->flags & RT_IFF_UP))
		return -RT_ENETDOWN;
	b = rt_buf_alloc(RT_BUF_HEADROOM, len - hl);
	if (!b)
		return -RT_ENOBUFS;
	memcpy(rt_buf_put(b, len - hl), p + hl, len - hl);
	r = rt_netif_xmit(nif, b, dst, type);
	return r < 0 ? r : (int)len;
}

int rt_bpf_set_notify(int ch, void *owner, void *task, u32 mask)
{
	struct bpf_chan *c;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	c->ntask = task;
	c->nmask = mask;
	return 0;
}

int rt_bpf_set_intr(int ch, void *owner, u32 mask)
{
	struct bpf_chan *c;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	c->imask = mask;
	return 0;
}

u32 rt_bpf_intr_mask(int ch, void *owner)
{
	struct bpf_chan *c;

	return chan_get(ch, owner, &c) < 0 ? 0 : c->imask;
}

int rt_bpf_ioctl(int ch, void *owner, u32 cmd, void *arg)
{
	struct bpf_chan *c;
	struct rt_netif *nif;
	u32 v;
	int r = chan_get(ch, owner, &c);

	if (r < 0)
		return r;
	if (!arg && cmd != RT_BIOCFLUSH && cmd != RT_BIOCPROMISC)
		return -RT_EINVAL;
	nif = c->ifindex ? rt_netif_by_index(c->ifindex) : NULL;
	switch (cmd) {
	case RT_FIONREAD:
		v = c->hlen ? c->hlen : c->slen;
		memcpy(arg, &v, 4);
		return 0;
	case RT_SIOCGIFADDR: {
		struct rt_sockaddr_in sin;
		if (!nif)
			return -RT_EINVAL;
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = RT_AF_INET;
		sin.sin_addr = nif->addr;
		memcpy(arg, &sin, sizeof(sin));
		return 0;
	}
	case RT_BIOCGBLEN:
		memcpy(arg, &c->blen, 4);
		return 0;
	case RT_BIOCSBLEN:
		if (c->ifindex)
			return -RT_EINVAL;
		memcpy(&v, arg, 4);
		if (v > RT_BPF_MAXBUFSIZE)
			v = RT_BPF_MAXBUFSIZE;
		if (v < RT_BPF_MINBUFSIZE)
			v = RT_BPF_MINBUFSIZE;
		c->blen = v;
		memcpy(arg, &v, 4);
		return 0;
	case RT_BIOCSETF: {
		const struct rt_bpf_program *pr = arg;
		struct rt_bpf_insn *f = NULL;
		if (pr->bf_insns) {
			if (rt_bpf_validate(pr->bf_insns, pr->bf_len) < 0)
				return -RT_EINVAL;
			f = rt_malloc(pr->bf_len * sizeof(*f));
			if (!f)
				return -RT_ENOBUFS;
			memcpy(f, pr->bf_insns, pr->bf_len * sizeof(*f));
		} else if (pr->bf_len) {
			return -RT_EINVAL;
		}
		rt_free(c->filter);
		c->filter = f;
		reset_buffers(c);
		return 0;
	}
	case RT_BIOCFLUSH:
		reset_buffers(c);
		return 0;
	case RT_BIOCPROMISC:
		if (!nif)
			return -RT_EINVAL;
		if (!c->promisc) {
			c->promisc = 1;
			set_promisc(c, 1);
		}
		return 0;
	case RT_BIOCGDLT:
		if (!nif)
			return -RT_EINVAL;
		v = c->dlt;
		memcpy(arg, &v, 4);
		return 0;
	case RT_BIOCGETIF:
		if (!nif)
			return -RT_EINVAL;
		memset(arg, 0, RT_IFNAMSIZ);
		rt_strlcpy(arg, nif->name, RT_IFNAMSIZ);
		return 0;
	case RT_BIOCSETIF: {
		char name[RT_IFNAMSIZ + 1];
		struct rt_netif *n;
		memcpy(name, arg, RT_IFNAMSIZ);
		name[RT_IFNAMSIZ] = 0;
		n = rt_netif_find(name);
		if (!n)
			return -RT_ENXIO;
		if (!c->sbuf || !c->ifindex) {      /* BIOCSBLEN may have changed blen */
			r = alloc_buffers(c);
			if (r < 0)
				return r;
		}
		if (c->ifindex != n->index) {
			int was = c->promisc;
			detach(c);
			c->promisc = (u8)was;
			c->ifindex = n->index;
			set_promisc(c, 1);
		}
		c->dlt = n->type == RT_IFT_ETHER ? RT_DLT_EN10MB : RT_DLT_NULL;
		reset_buffers(c);
		update_on();
		return 0;
	}
	case RT_BIOCSRTIMEOUT: {
		struct rt_bpf_timeval tv;
		memcpy(&tv, arg, sizeof(tv));
		if (tv.tv_usec >= 1000000u)
			return -RT_EINVAL;
		c->rtimeout_ms = tv.tv_sec * 1000u + (tv.tv_usec + 999u) / 1000u;
		return 0;
	}
	case RT_BIOCGRTIMEOUT: {
		struct rt_bpf_timeval tv;
		tv.tv_sec = c->rtimeout_ms / 1000u;
		tv.tv_usec = (c->rtimeout_ms % 1000u) * 1000u;
		memcpy(arg, &tv, sizeof(tv));
		return 0;
	}
	case RT_BIOCGSTATS: {
		struct rt_bpf_stat st;
		st.bs_recv = c->recv;
		st.bs_drop = c->drop;
		memcpy(arg, &st, sizeof(st));
		return 0;
	}
	case RT_BIOCIMMEDIATE:
		memcpy(&v, arg, 4);
		c->immediate = v != 0;
		if (c->immediate && c->slen)
			notify(c);
		return 0;
	case RT_BIOCVERSION: {
		struct rt_bpf_version bv;
		bv.bv_major = RT_BPF_MAJOR_VERSION;
		bv.bv_minor = RT_BPF_MINOR_VERSION;
		memcpy(arg, &bv, sizeof(bv));
		return 0;
	}
	}
	return -RT_EINVAL;
}
