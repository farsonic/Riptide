/*
 * Riptide - TCP output: segment construction and options, window
 * selection, MSS handling, Nagle/cork, the transmit loop, retransmission,
 * zero window probes, tail loss probes, ACK/RST/FIN/SYN generation.
 * Follows Linux net/ipv4/tcp_output.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"
#include "infra.h"

#define OPTION_SACK_ADVERTISE 0x01
#define OPTION_TS             0x02
#define OPTION_MSS            0x04
#define OPTION_WSCALE         0x08
#define OPTION_SACK_BLOCKS    0x10

struct tcp_out_options {
	u16 options;
	u16 mss;
	u8  ws;
	u8  num_sack_blocks;
	u8  dsack;
	u32 tsval, tsecr;
	const struct tcp_sack_block *dsack_blk;
	const struct tcp_sack_block *sacks;
};

/* Linux tcp_options_write(): MSS, SACK_PERM+TS packed, WSCALE, SACK. */
static u32 tcp_options_write(u8 *p, const struct tcp_out_options *o)
{
	u8 *start = p;

	if (o->options & OPTION_MSS) {
		p[0] = TCPOPT_MSS; p[1] = TCPOLEN_MSS;
		rt_put_be16(p + 2, o->mss);
		p += 4;
	}
	if (o->options & OPTION_TS) {
		if (o->options & OPTION_SACK_ADVERTISE) {
			p[0] = TCPOPT_SACK_PERM; p[1] = TCPOLEN_SACK_PERM;
		} else {
			p[0] = TCPOPT_NOP; p[1] = TCPOPT_NOP;
		}
		p[2] = TCPOPT_TIMESTAMP; p[3] = TCPOLEN_TIMESTAMP;
		rt_put_be32(p + 4, o->tsval);
		rt_put_be32(p + 8, o->tsecr);
		p += 12;
	} else if (o->options & OPTION_SACK_ADVERTISE) {
		p[0] = TCPOPT_NOP; p[1] = TCPOPT_NOP;
		p[2] = TCPOPT_SACK_PERM; p[3] = TCPOLEN_SACK_PERM;
		p += 4;
	}
	if (o->options & OPTION_WSCALE) {
		p[0] = TCPOPT_NOP; p[1] = TCPOPT_WINDOW; p[2] = TCPOLEN_WINDOW; p[3] = o->ws;
		p += 4;
	}
	if (o->num_sack_blocks) {
		int i;
		p[0] = TCPOPT_NOP; p[1] = TCPOPT_NOP; p[2] = TCPOPT_SACK;
		p[3] = (u8)(TCPOLEN_SACK_BASE + o->num_sack_blocks * TCPOLEN_SACK_PERBLOCK);
		p += 4;
		for (i = 0; i < o->num_sack_blocks; i++) {
			const struct tcp_sack_block *sp;
			if (o->dsack)
				sp = i == 0 ? o->dsack_blk : &o->sacks[i - 1];
			else
				sp = &o->sacks[i];
			rt_put_be32(p, sp->start_seq);
			rt_put_be32(p + 4, sp->end_seq);
			p += 8;
		}
	}
	return (u32)(p - start);
}

static u32 tcp_syn_options(struct tcp_sock *tp, struct tcp_out_options *o)
{
	u32 size = 0;

	if (tp->noopt)
		return 0;
	o->options |= OPTION_MSS;
	o->mss = tp->advmss;
	size += 4;
	if (tp->rx_opt.tstamp_ok) {
		o->options |= OPTION_TS;
		o->tsval = tcp_time_stamp_ts(tp);
		o->tsecr = tp->ts_recent;
		size += 12;
	}
	if (tp->rx_opt.wscale_ok) {
		o->options |= OPTION_WSCALE;
		o->ws = tp->rx_opt.rcv_wscale;
		size += 4;
	}
	if (tp->rx_opt.sack_ok) {
		o->options |= OPTION_SACK_ADVERTISE;
		if (!(o->options & OPTION_TS))
			size += 4;
	}
	return size;
}

/* Linux tcp_established_options() */
static u32 tcp_established_options(struct tcp_sock *tp, struct tcp_out_options *o)
{
	u32 size = 0, eff_sacks;

	if (tp->rx_opt.tstamp_ok) {
		o->options |= OPTION_TS;
		o->tsval = tcp_time_stamp_ts(tp);
		o->tsecr = tp->ts_recent;
		size += 12;
	}
	eff_sacks = tp->rx_opt.num_sacks + tp->rx_opt.dsack;
	if (eff_sacks) {
		u32 remaining = TCP_MAX_OPT_LEN - size;
		if (remaining >= 4 + TCPOLEN_SACK_PERBLOCK) {
			o->num_sack_blocks = (u8)RT_MIN(eff_sacks, (remaining - 4) / TCPOLEN_SACK_PERBLOCK);
			o->dsack = tp->rx_opt.dsack;
			o->dsack_blk = tp->duplicate_sack;
			o->sacks = tp->selective_acks;
			size += 4 + o->num_sack_blocks * TCPOLEN_SACK_PERBLOCK;
		}
	}
	return size;
}

int tcp_established_options_len(const struct tcp_sock *tp)
{
	struct tcp_out_options o;

	memset(&o, 0, sizeof(o));
	return (int)tcp_established_options((struct tcp_sock *)tp, &o);
}

/* ---- window selection (Linux __tcp_select_window / tcp_select_window) ---- */
u32 __tcp_select_window(struct tcp_sock *tp)
{
	s32 mss = tp->ack.rcv_mss;
	s32 free_space = (s32)tcp_space(tp);
	s32 full_space = (s32)RT_MIN(tp->window_clamp, tcp_full_space(tp));
	s32 window;
	u8 ws = tp->rx_opt.rcv_wscale;

	if (full_space <= 0)
		return 0;
	if (mss > full_space)
		mss = full_space;
	if (mss <= 0)
		mss = 1;
	if (free_space < (full_space >> 1)) {
		tp->ack.quick = 0;
		free_space = (s32)(((u32)free_space >> ws) << ws);
		if (free_space < (full_space >> 4) || free_space < mss)
			return 0;
	}
	if (free_space > (s32)tp->rcv_ssthresh)
		free_space = (s32)tp->rcv_ssthresh;
	window = (s32)tp->rcv_wnd;
	if (ws) {
		window = free_space;
		if ((((u32)window >> ws) << ws) != (u32)window)
			window = (s32)((((u32)window >> ws) + 1) << ws);
	} else {
		if (window <= free_space - mss || window > free_space)
			window = (free_space / mss) * mss;
		else if (mss == full_space && free_space > window + (full_space >> 1))
			window = free_space;
	}
	return (u32)window;
}

static u16 tcp_select_window(struct tcp_sock *tp)
{
	u32 cur_win = tcp_receive_window(tp);
	u32 new_win = __tcp_select_window(tp);
	u8 ws = tp->rx_opt.rcv_wscale;

	if (new_win < cur_win) {
		/* RFC 9293: never shrink the offered window */
		new_win = ((cur_win + (1u << ws) - 1) >> ws) << ws;
	}
	tp->rcv_wnd = new_win;
	tp->rcv_wup = tp->rcv_nxt;
	new_win = RT_MIN(new_win, TCP_MAX_WINDOW << ws);
	new_win >>= ws;
	if (new_win == 0) {
		tp->pred_flags = 0;
		TCP_DBG(tp, "advertising a zero window (%lu bytes unread)",
		        (unsigned long)(tp->rcv_nxt - tp->copied_seq));
		tp->dg.zwin_out++;
		if (!tp->dg.zwin_start)
			tp->dg.zwin_start = tcp_diag_now();
	} else if (tp->dg.zwin_start) {
		tp->dg.zwin_ms += tcp_jiffies() - tp->dg.zwin_start;
		tp->dg.zwin_start = 0;
	}
	return (u16)new_win;
}

/* Linux tcp_select_initial_window() */
void tcp_select_initial_window(u32 space, u32 mss, u32 *rcv_wnd, u32 *window_clamp,
                               int wscale_ok, u8 *rcv_wscale)
{
	if (*window_clamp == 0)
		*window_clamp = TCP_MAX_WINDOW << TCP_MAX_WSCALE;
	if (mss == 0)
		mss = TCP_MSS_DEFAULT;
	space = RT_MIN(*window_clamp, space);
	if (space > mss)
		space = (space / mss) * mss;
	*rcv_wnd = RT_MIN(space, TCP_MAX_WINDOW);
	*rcv_wscale = 0;
	if (wscale_ok) {
		u32 s = RT_MAX(space, (u32)sysctl_tcp_rmem_max);
		int l = 0;
		s = RT_MIN(s, *window_clamp);
		while ((s >> l) > 1)
			l++;
		l -= 15;
		if (l < 0)
			l = 0;
		if (l > TCP_MAX_WSCALE)
			l = TCP_MAX_WSCALE;
		*rcv_wscale = (u8)l;
	}
	*window_clamp = RT_MIN(TCP_MAX_WINDOW << *rcv_wscale, *window_clamp);
}

/* ---- MSS ---- */
static u32 tcp_mtu_to_mss(const struct tcp_sock *tp, u32 pmtu)
{
	s32 mss = (s32)pmtu - tp->net_hlen - TCP_HDR_LEN;

	if (mss > (s32)tp->rx_opt.mss_clamp)
		mss = tp->rx_opt.mss_clamp;
	mss -= tp->tcp_header_len - TCP_HDR_LEN;
	if (mss < TCP_MIN_SND_MSS)
		mss = TCP_MIN_SND_MSS;
	return (u32)mss;
}

u32 tcp_mss_to_mtu(const struct tcp_sock *tp, u32 mss)
{
	return mss + tp->tcp_header_len + tp->net_hlen;
}

static u32 tcp_bound_to_half_wnd(const struct tcp_sock *tp, u32 pktsize)
{
	u32 cutoff = tp->max_window > TCP_MSS_DEFAULT ? tp->max_window >> 1 : tp->max_window;

	if (cutoff && pktsize > cutoff)
		return RT_MAX(cutoff, 68u - tp->tcp_header_len);
	return pktsize;
}

u32 tcp_sync_mss(struct tcp_sock *tp, u32 pmtu)
{
	u32 mss;

	if (pmtu < 68)
		pmtu = 68;
	if (tp->net_hlen == 40 && pmtu < RT_IP6_MIN_MTU)
		pmtu = RT_IP6_MIN_MTU;
	mss = tcp_mtu_to_mss(tp, pmtu);
	mss = tcp_bound_to_half_wnd(tp, mss);
	tp->pmtu_cookie = (u16)pmtu;
	tp->mss_cache = (u16)mss;
	return mss;
}

/* Linux tcp_current_mss(): account for variable options (SACK blocks) */
u32 tcp_current_mss(struct tcp_sock *tp)
{
	u32 mss_now = tp->mss_cache;
	u32 header_len = (u32)tcp_established_options_len(tp) + TCP_HDR_LEN;

	if (header_len > tp->tcp_header_len) {
		u32 delta = header_len - tp->tcp_header_len;
		mss_now = mss_now > delta + TCP_MIN_SND_MSS ? mss_now - delta : TCP_MIN_SND_MSS;
	}
	return mss_now;
}

/* ---- buffers ---- */
struct rt_buf *tcp_stream_alloc_skb(struct tcp_sock *tp, u32 size)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, size);
	return b;
}

void tcp_wmem_free_skb(struct tcp_sock *tp, struct rt_buf *b)
{
	u32 ts = tcp_buf_truesize(b);

	tp->wq_bytes -= RT_MIN(tp->wq_bytes, b->len);
	tp->wmem_queued -= RT_MIN(tp->wmem_queued, ts);
	rt_buf_free(b);
}

void tcp_mark_push(struct tcp_sock *tp, struct rt_buf *b)
{
	TCP_SKB_CB(b)->tcp_flags |= TCPHDR_PSH;
	tp->pushed_seq = tp->write_seq;
}

/* ---- events ---- */
void tcp_event_ack_sent(struct tcp_sock *tp)
{
	if (tp->ack.quick) {
		if (tp->ack.quick <= 1) {
			tp->ack.quick = 0;
			tp->ack.ato = TCP_ATO_MIN;
		} else {
			tp->ack.quick--;
		}
	}
	tp->ack.pending = 0;
	rt_timer_del(&tp->delack_timer);
}

static void tcp_event_data_sent(struct tcp_sock *tp)
{
	u32 now = tcp_jiffies();

	if (tcp_packets_in_flight(tp) == 0)
		tcp_ca_event(tp, CA_EVENT_TX_START);
	tp->lsndtime = now;
	/* a reply within ato of the last received packet: interactive */
	if ((u32)(now - tp->ack.lrcvtime) < tp->ack.ato)
		tp->pingpong = 1;
}

/* ---- segment transmission (Linux __tcp_transmit_skb) ---- */
static int tcp_ip_send(struct tcp_sock *tp, struct rt_buf *b, u8 tos)
{
	struct rt_inet_out o;

	memset(&o, 0, sizeof(o));
	o.src = tp->saddr;
	o.dst = tp->daddr;
	o.proto = RT_IPPROTO_TCP;
	o.ttl = tp->ttl;
	o.tos = rt_qos_sock_tos(tp->so, tos);
	o.df = 1;
	b->prio = rt_qos_sock_prio(tp->so);
	o.dontroute = tp->so && (tp->so->options & RT_SO_DONTROUTE);
	o.scope = tp->scope;
	return rt_inet_output(b, &o);
}

static void tcp_fill_csum(struct rt_buf *b, const rt_in6_addr *saddr, const rt_in6_addr *daddr)
{
	u32 sum = rt_inet_pseudo(saddr, daddr, RT_IPPROTO_TCP, b->len);
	u16 c;

	b->data[16] = b->data[17] = 0;
	c = rt_csum_fold(rt_csum_partial(b->data, b->len, sum));
	memcpy(b->data + 16, &c, 2);
}

/* As tcp_fill_csum(), with the payload behind the hdr_len (even) header
 * bytes already summed into dsum. */
static void tcp_fill_csum_hdr(struct rt_buf *b, const rt_in6_addr *saddr, const rt_in6_addr *daddr,
                              u32 hdr_len, u32 dsum)
{
	u32 sum = rt_inet_pseudo(saddr, daddr, RT_IPPROTO_TCP, b->len);
	u16 c;

	b->data[16] = b->data[17] = 0;
	sum = rt_csum_add(rt_csum_hdr(b->data, hdr_len, sum), dsum);
	c = rt_csum_fold(sum);
	memcpy(b->data + 16, &c, 2);
}

static int tcp_transmit_skb_body(struct tcp_sock *tp, struct rt_buf *skb, int clone_it)
{
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	struct tcp_out_options opts;
	struct rt_buf *b;
	u32 opt_size, hdr_len, data_len, dsum;
	u8 *th, flags, tos = tp->tos;
	u16 win;

	tcp_mstamp_refresh();
	if (clone_it) {
		tcb->tx_us = tcp_clock_us;
		tcb->tx_ms = tcp_jiffies();
		if (!tcb->first_tx_ms)
			tcb->first_tx_ms = tcb->tx_ms;
		/* copy the payload and checksum it in the same pass */
		b = rt_buf_copy_csum(skb, &dsum);
		if (!b)
			return -RT_ENOBUFS;
	} else {
		b = skb;
		dsum = b->len ? rt_csum_partial(b->data, b->len, 0) : 0;
	}
	data_len = b->len;
	memset(&opts, 0, sizeof(opts));
	flags = tcb->tcp_flags;
	if (flags & TCPHDR_SYN)
		opt_size = tcp_syn_options(tp, &opts);
	else
		opt_size = tcp_established_options(tp, &opts);
	hdr_len = TCP_HDR_LEN + opt_size;
	th = rt_buf_push(b, hdr_len);
	memcpy(th, &tp->sport, 2);
	memcpy(th + 2, &tp->dport, 2);
	rt_put_be32(th + 4, tcb->seq);
	rt_put_be32(th + 8, tp->rcv_nxt);
	th[12] = (u8)((hdr_len >> 2) << 4);
	if (flags & TCPHDR_SYN)
		win = (u16)RT_MIN(tp->rcv_wnd, TCP_MAX_WINDOW);
	else
		win = tcp_select_window(tp);
	rt_put_be16(th + 14, win);
	rt_put_be16(th + 18, 0);
	/* urgent pointer (BSD: one past the urgent byte) */
	if (RT_UNLIKELY(tcp_urg_mode(tp) && rt_seq_before(tcb->seq, tp->snd_up))) {
		if (rt_seq_before(tp->snd_up, tcb->seq + 0x10000)) {
			rt_put_be16(th + 18, (u16)(tp->snd_up - tcb->seq));
			flags |= TCPHDR_URG;
		} else if (rt_seq_after(tcb->seq + 0xffff, tp->snd_nxt)) {
			rt_put_be16(th + 18, 0xffff);
			flags |= TCPHDR_URG;
		}
	}
	tcp_options_write(th + TCP_HDR_LEN, &opts);
	if (opts.num_sack_blocks) {
		tp->rx_opt.dsack = 0;
		tp->dg.sack_blocks_out += opts.num_sack_blocks;
	}
	/* ECN (Linux tcp_ecn_send) */
	if ((tp->ecn_flags & TCP_ECN_OK) && !(flags & TCPHDR_SYN)) {
		if (data_len && !rt_seq_before(tcb->seq, tp->snd_nxt)) {
			tos |= INET_ECN_ECT_0;
			if (tp->ecn_flags & TCP_ECN_QUEUE_CWR) {
				tp->ecn_flags &= ~TCP_ECN_QUEUE_CWR;
				flags |= TCPHDR_CWR;
			}
		}
		if (tp->ecn_flags & TCP_ECN_DEMAND_CWR)
			flags |= TCPHDR_ECE;
	}
	th[13] = flags;
	tcp_fill_csum_hdr(b, &tp->saddr, &tp->daddr, hdr_len, dsum);

	if (flags & TCPHDR_ACK)
		tcp_event_ack_sent(tp);
	if (data_len)
		tcp_event_data_sent(tp);
	rt_tcpstat.OutSegs++;
	tp->segs_out++;
	if (data_len)
		tp->data_segs_out++;
	if (flags & TCPHDR_RST)
		rt_tcpstat.OutRsts++;
	/* Errors past this point are treated as losses in the network: the
	 * retransmission machinery recovers. */
	(void)tcp_ip_send(tp, b, tos);
	return 0;
}

int tcp_transmit_skb(struct tcp_sock *tp, struct rt_buf *skb, int clone_it)
{
	int r;

	RT_PROF_ENTER(RT_PROF_TCP_OUT);
	r = tcp_transmit_skb_body(tp, skb, clone_it);
	RT_PROF_EXIT(RT_PROF_TCP_OUT);
	return r;
}

/* ---- stateless segments (RST, time-wait ACKs, SYN-ACKs of requests) ---- */
static int tcp_send_built(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport,
                          u32 seq, u32 ack, u8 flags, u16 win, const struct tcp_out_options *o,
                          u8 tos, u8 ttl, u32 scope)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 0);
	struct rt_inet_out io;
	u32 hdr_len;
	u8 opt[TCP_MAX_OPT_LEN];
	u32 olen = 0;
	u8 *th;

	if (!b)
		return -RT_ENOBUFS;
	if (o)
		olen = tcp_options_write(opt, o);
	hdr_len = TCP_HDR_LEN + olen;
	th = rt_buf_push(b, hdr_len);
	memcpy(th, &sport, 2);
	memcpy(th + 2, &dport, 2);
	rt_put_be32(th + 4, seq);
	rt_put_be32(th + 8, ack);
	th[12] = (u8)((hdr_len >> 2) << 4);
	th[13] = flags;
	rt_put_be16(th + 14, win);
	rt_put_be16(th + 18, 0);
	if (olen)
		memcpy(th + TCP_HDR_LEN, opt, olen);
	tcp_fill_csum(b, saddr, daddr);
	memset(&io, 0, sizeof(io));
	io.src = *saddr;
	io.dst = *daddr;
	io.proto = RT_IPPROTO_TCP;
	io.tos = tos;
	io.ttl = ttl;
	io.df = 1;
	io.scope = scope;
	rt_tcpstat.OutSegs++;
	if (flags & TCPHDR_RST)
		rt_tcpstat.OutRsts++;
	return rt_inet_output(b, &io);
}

int tcp_send_ctl(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport, u32 seq,
                 u32 ack, u8 flags, u16 win, int ts, u32 tsval, u32 tsecr, u8 tos, u8 ttl, u32 scope)
{
	struct tcp_out_options o;

	memset(&o, 0, sizeof(o));
	if (ts) {
		o.options = OPTION_TS;
		o.tsval = tsval;
		o.tsecr = tsecr;
	}
	return tcp_send_built(saddr, daddr, sport, dport, seq, ack, flags, win, ts ? &o : NULL, tos, ttl,
	                      scope);
}

/* Linux tcp_v4_send_reset(): RST for a segment that has no connection.
 * 'in' still has data at the TCP header and a filled cb. */
void tcp_send_reset_raw(const struct rt_buf *in, const rt_in6_addr *saddr, const rt_in6_addr *daddr,
                        u32 scope)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(in);
	u16 sport, dport;

	if (cb->tcp_flags & TCPHDR_RST)
		return;
	memcpy(&sport, in->data, 2);
	memcpy(&dport, in->data + 2, 2);
	if (cb->tcp_flags & TCPHDR_ACK)
		tcp_send_ctl(daddr, saddr, dport, sport, cb->ack_seq, 0, TCPHDR_RST, 0, 0, 0, 0, 0, 0, scope);
	else
		tcp_send_ctl(daddr, saddr, dport, sport, 0, cb->end_seq, TCPHDR_RST | TCPHDR_ACK,
		             0, 0, 0, 0, 0, 0, scope);
}

/* SYN-ACK for a request sock (Linux tcp_make_synack) */
int tcp_send_synack_req(struct tcp_request_sock *req, u32 tsval)
{
	struct tcp_out_options o;
	u8 flags = TCPHDR_SYN | TCPHDR_ACK;

	memset(&o, 0, sizeof(o));
	o.options = OPTION_MSS;
	o.mss = req->mss;
	if (req->wscale_ok) {
		o.options |= OPTION_WSCALE;
		o.ws = req->rcv_wscale;
	}
	if (req->sack_ok)
		o.options |= OPTION_SACK_ADVERTISE;
	if (req->tstamp_ok) {
		o.options |= OPTION_TS;
		o.tsval = tsval;
		o.tsecr = req->ts_recent;
	}
	if (req->ecn_ok)
		flags |= TCPHDR_ECE;
	req->snt_synack_us = rt_now_us();
	req->snt_synack_ms = rt_now_ms();
	return tcp_send_built(&req->loc_addr, &req->rmt_addr, req->loc_port, req->rmt_port,
	                      req->snt_isn, req->rcv_isn + 1, flags,
	                      (u16)RT_MIN(req->rcv_wnd, TCP_MAX_WINDOW), &o, req->tos, 0, req->scope);
}

/* ---- ACKs ---- */
static u32 tcp_acceptable_seq(const struct tcp_sock *tp)
{
	if (!rt_seq_before(tcp_wnd_end(tp), tp->snd_nxt) ||
	    (tp->rx_opt.wscale_ok && (tp->snd_nxt - tcp_wnd_end(tp)) < (1u << tp->rx_opt.rcv_wscale)))
		return tp->snd_nxt;
	return tcp_wnd_end(tp);
}

void tcp_send_ack(struct tcp_sock *tp)
{
	struct rt_buf *b;

	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN)
		return;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 0);
	if (!b) {
		/* retry from the delayed ACK timer */
		tp->ack.pending |= ICSK_ACK_SCHED | ICSK_ACK_TIMER;
		tp->ack.timeout = tcp_jiffies() + TCP_DELACK_MAX;
		rt_timer_mod(&tp->delack_timer, tp->ack.timeout);
		return;
	}
	TCP_SKB_CB(b)->seq = TCP_SKB_CB(b)->end_seq = tcp_acceptable_seq(tp);
	TCP_SKB_CB(b)->tcp_flags = TCPHDR_ACK;
	tp->dg.acks_out++;
	if (!rt_bufq_empty(&tp->ofoq) || tp->rx_opt.dsack)
		tp->dg.dupacks_out++;
	tcp_transmit_skb(tp, b, 0);
}

/* Linux tcp_send_delayed_ack(): ato bounded by 200 ms and srtt */
void tcp_send_delayed_ack(struct tcp_sock *tp)
{
	u32 ato = tp->ack.ato, timeout, now = tcp_jiffies();

	if (ato > TCP_DELACK_MIN) {
		u32 max_ato = 500;
		if (tp->pingpong || (tp->ack.pending & ICSK_ACK_PUSHED))
			max_ato = (u32)sysctl_tcp_delack_ms;
		if (tp->srtt_us) {
			u32 rtt = RT_MAX((tp->srtt_us >> 3) / 1000u, (u32)TCP_DELACK_MIN);
			if (rtt < max_ato)
				max_ato = rtt;
		}
		ato = RT_MIN(ato, max_ato);
	}
	ato = RT_MIN(ato, (u32)sysctl_tcp_delack_ms);
	timeout = now + ato;
	if (tp->ack.pending & ICSK_ACK_TIMER) {
		if ((s32)(tp->ack.timeout - (now + (ato >> 2))) <= 0) {
			tcp_send_ack(tp);
			return;
		}
		if ((s32)(timeout - tp->ack.timeout) >= 0)
			timeout = tp->ack.timeout;
	}
	tp->ack.pending |= ICSK_ACK_SCHED | ICSK_ACK_TIMER;
	tp->ack.timeout = timeout;
	rt_timer_mod(&tp->delack_timer, timeout);
}

void tcp_send_active_reset(struct tcp_sock *tp)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 0);

	if (!b)
		return;
	TCP_SKB_CB(b)->seq = TCP_SKB_CB(b)->end_seq = tcp_acceptable_seq(tp);
	TCP_SKB_CB(b)->tcp_flags = TCPHDR_ACK | TCPHDR_RST;
	tcp_transmit_skb(tp, b, 0);
}

/* Linux tcp_send_fin(): piggyback on unsent tail or queue a FIN segment */
void tcp_send_fin(struct tcp_sock *tp)
{
	struct rt_buf *tail = tp->wq.tail;

	if (tail && tp->send_head && !(TCP_SKB_CB(tail)->tcp_flags & TCPHDR_SYN)) {
		TCP_SKB_CB(tail)->tcp_flags |= TCPHDR_FIN;
		TCP_SKB_CB(tail)->end_seq++;
		tp->write_seq++;
	} else {
		struct rt_buf *b = tcp_stream_alloc_skb(tp, 0);
		if (!b) {
			/* Linux falls back to a forced allocation; we retry
			 * later from the probe timer by marking a tail FIN
			 * impossible: abort cleanly instead. */
			tcp_send_active_reset(tp);
			tcp_done(tp);
			return;
		}
		TCP_SKB_CB(b)->seq = tp->write_seq;
		TCP_SKB_CB(b)->end_seq = tp->write_seq + 1;
		TCP_SKB_CB(b)->tcp_flags = TCPHDR_ACK | TCPHDR_FIN;
		rt_bufq_tail(&tp->wq, b);
		tp->wmem_queued += tcp_buf_truesize(b);
		if (!tp->send_head)
			tp->send_head = b;
		tp->write_seq++;
	}
	__tcp_push_pending_frames(tp, tcp_current_mss(tp), TCP_NAGLE_OFF);
}

/* SYN-ACK of a full socket after crossed SYNs (simultaneous open) */
int tcp_send_synack(struct tcp_sock *tp)
{
	struct rt_buf *b = tcp_rtx_head(tp);

	if (!b || !(TCP_SKB_CB(b)->tcp_flags & TCPHDR_SYN))
		return -RT_EINVAL;
	TCP_SKB_CB(b)->tcp_flags |= TCPHDR_ACK;
	if (tp->ecn_flags & TCP_ECN_OK) {
		TCP_SKB_CB(b)->tcp_flags &= ~TCPHDR_CWR;
		TCP_SKB_CB(b)->tcp_flags |= TCPHDR_ECE;
	} else {
		TCP_SKB_CB(b)->tcp_flags &= ~(TCPHDR_CWR | TCPHDR_ECE);
	}
	return tcp_transmit_skb(tp, b, 1);
}

/* Linux tcp_connect_init() + tcp_connect() */
int tcp_connect(struct tcp_sock *tp)
{
	struct rt_buf *b;
	u32 clamp;
	int opts = !tp->noopt;

	tp->rx_opt.tstamp_ok = (u8)(opts && sysctl_tcp_timestamps);
	tp->rx_opt.wscale_ok = (u8)(opts && sysctl_tcp_window_scaling);
	tp->rx_opt.sack_ok = (u8)(opts && sysctl_tcp_sack);
	tp->tcp_header_len = (u16)(TCP_HDR_LEN + (tp->rx_opt.tstamp_ok ? TCPOLEN_TSTAMP_ALIGNED : 0));
	tp->rx_opt.mss_clamp = tp->rx_opt.user_mss ? tp->rx_opt.user_mss : TCP_MSS_DEFAULT;
	tp->max_window = 0;
	tcp_sync_mss(tp, tp->pmtu_cookie);
	tp->advmss = (u16)(tp->pmtu_cookie - tp->net_hlen - TCP_HDR_LEN);
	if (tp->rx_opt.user_mss && tp->advmss > tp->rx_opt.user_mss)
		tp->advmss = tp->rx_opt.user_mss;
	if (tp->rcvbuf_locked || !sysctl_tcp_moderate_rcvbuf)
		clamp = tcp_full_space(tp);
	else
		clamp = RT_MAX((u32)sysctl_tcp_rmem_max, tcp_full_space(tp));
	tp->window_limit = tcp_route_window(&tp->daddr, tp->scope);
	if (tp->window_limit && clamp > tp->window_limit)
		clamp = tp->window_limit;
	tp->window_clamp = clamp;
	tcp_select_initial_window(tcp_full_space(tp),
	                          tp->advmss - (tp->rx_opt.tstamp_ok ? TCPOLEN_TSTAMP_ALIGNED : 0),
	                          &tp->rcv_wnd, &tp->window_clamp, tp->rx_opt.wscale_ok,
	                          &tp->rx_opt.rcv_wscale);
	tp->rcv_ssthresh = tp->rcv_wnd;
	tcp_initialize_rcv_mss(tp);
	tp->snd_wnd = 0;
	tp->snd_wl1 = 0;
	tcp_write_queue_purge(tp);
	tp->snd_una = tp->write_seq;
	tp->snd_sml = tp->write_seq;
	tp->snd_up = tp->write_seq;
	tp->snd_nxt = tp->write_seq;
	tp->iss = tp->write_seq;
	tp->rcv_nxt = 0;
	tp->rcv_wup = 0;
	tp->copied_seq = 0;
	tp->icsk_rto = TCP_TIMEOUT_INIT;
	tp->icsk_retransmits = 0;
	tp->icsk_backoff = 0;
	tp->total_retrans = 0;
	tcp_clear_retrans(tp);
	tp->ecn_flags = 0;

	b = tcp_stream_alloc_skb(tp, 0);
	if (!b)
		return -RT_ENOBUFS;
	TCP_SKB_CB(b)->seq = tp->write_seq;
	TCP_SKB_CB(b)->end_seq = tp->write_seq + 1;
	TCP_SKB_CB(b)->tcp_flags = TCPHDR_SYN;
	if (sysctl_tcp_ecn == 1 && opts) {
		/* RFC 3168 6.1.1: ECN-setup SYN */
		TCP_SKB_CB(b)->tcp_flags |= TCPHDR_ECE | TCPHDR_CWR;
		tp->ecn_flags = TCP_ECN_OK;
	}
	tp->write_seq++;
	tp->retrans_stamp = tcp_jiffies();
	rt_bufq_tail(&tp->wq, b);
	tp->wmem_queued += tcp_buf_truesize(b);
	tp->packets_out = 1;
	tcp_transmit_skb(tp, b, 1);
	tp->snd_nxt = tp->write_seq;
	tp->pushed_seq = tp->write_seq;
	rt_tcpstat.ActiveOpens++;
	tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, tp->icsk_rto);
	return 0;
}

/* ---- segmentation ---- */

/* Linux tcp_fragment(): split b so that it carries 'len' bytes; the rest
 * becomes a new segment right after it (inheriting FIN/PSH). */
int tcp_fragment(struct tcp_sock *tp, struct rt_buf *b, u32 len)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b), *ncb;
	struct rt_buf *nb;
	u32 nlen;
	u8 flags;
	int sent = !tp->send_head || rt_seq_before(cb->seq, TCP_SKB_CB(tp->send_head)->seq);

	if (len >= b->len || len == 0)
		return 0;
	nlen = b->len - len;
	nb = tcp_stream_alloc_skb(tp, nlen);
	if (!nb)
		return -RT_ENOMEM;
	rt_copy_buf(rt_buf_put(nb, nlen), b->data + len, nlen);
	b->len = len;
	ncb = TCP_SKB_CB(nb);
	*ncb = *cb;
	ncb->seq = cb->seq + len;
	flags = cb->tcp_flags;
	cb->tcp_flags = flags & (u8)~(TCPHDR_FIN | TCPHDR_PSH);
	ncb->tcp_flags = flags;
	cb->end_seq = ncb->seq;
	cb->eor = 0;
	tp->wq.bytes -= RT_MIN(tp->wq.bytes, nlen);
	rt_bufq_insert_after(&tp->wq, b, nb);
	tp->wmem_queued += tcp_buf_truesize(nb);
	if (sent) {
		tp->packets_out++;
		if (cb->sacked & TCPCB_LOST)
			tp->lost_out++;
		if (cb->sacked & TCPCB_SACKED_RETRANS)
			tp->retrans_out++;
		if (cb->sacked & TCPCB_SACKED_ACKED)
			tp->sacked_out++;
	}
	return 0;
}

/* Linux tcp_cwnd_restart() (RFC 7661 style idle restart) */
void tcp_cwnd_restart(struct tcp_sock *tp, s32 delta)
{
	u32 restart_cwnd = RT_MIN((u32)sysctl_tcp_initcwnd, tp->snd_cwnd);
	u32 cwnd = tp->snd_cwnd;

	tcp_ca_event(tp, CA_EVENT_CWND_RESTART);
	tp->snd_ssthresh = tcp_current_ssthresh(tp);
	restart_cwnd = RT_MIN(restart_cwnd, cwnd);
	while ((delta -= (s32)tp->icsk_rto) > 0 && cwnd > restart_cwnd)
		cwnd >>= 1;
	tp->snd_cwnd = RT_MAX(cwnd, restart_cwnd);
	tp->snd_cwnd_stamp = tcp_jiffies();
	tp->snd_cwnd_used = 0;
}

/* Linux tcp_cwnd_validate() / tcp_cwnd_application_limited() */
static void tcp_cwnd_validate(struct tcp_sock *tp, int is_cwnd_limited)
{
	if (!rt_seq_before(tp->snd_una, tp->cwnd_usage_seq) || is_cwnd_limited ||
	    (!tp->is_cwnd_limited && tp->packets_out > tp->max_packets_out)) {
		tp->is_cwnd_limited = (u8)is_cwnd_limited;
		tp->max_packets_out = tp->packets_out;
		tp->cwnd_usage_seq = tp->snd_nxt;
	}
	if (tcp_is_cwnd_limited(tp)) {
		tp->snd_cwnd_used = 0;
		tp->snd_cwnd_stamp = tcp_jiffies();
	} else {
		if (tp->packets_out > tp->snd_cwnd_used)
			tp->snd_cwnd_used = tp->packets_out;
		if (sysctl_tcp_slow_start_after_idle &&
		    (s32)(tcp_jiffies() - tp->snd_cwnd_stamp) >= (s32)tp->icsk_rto) {
			if (tp->ca_state == TCP_CA_Open) {
				u32 init_win = (u32)sysctl_tcp_initcwnd;
				u32 win_used = RT_MAX(tp->snd_cwnd_used, init_win);
				if (win_used < tp->snd_cwnd) {
					tp->snd_ssthresh = tcp_current_ssthresh(tp);
					tp->snd_cwnd = (tp->snd_cwnd + win_used) >> 1;
				}
				tp->snd_cwnd_used = 0;
			}
			tp->snd_cwnd_stamp = tcp_jiffies();
		}
	}
}

static int tcp_minshall_check(const struct tcp_sock *tp)
{
	return rt_seq_after(tp->snd_sml, tp->snd_una) && !rt_seq_after(tp->snd_sml, tp->snd_nxt);
}

/* Linux tcp_nagle_test(): RFC 896 with Minshall's refinement */
static int tcp_nagle_test(const struct tcp_sock *tp, const struct rt_buf *b, u32 cur_mss, int nonagle)
{
	int partial;

	if (nonagle & TCP_NAGLE_PUSH)
		return 1;
	if (tcp_urg_mode(tp) || (TCP_SKB_CB(b)->tcp_flags & TCPHDR_FIN))
		return 1;
	partial = b->len < cur_mss;
	if (partial && ((nonagle & TCP_NAGLE_CORK) ||
	                (!nonagle && tp->packets_out && tcp_minshall_check(tp))))
		return 0;
	return 1;
}

static int tcp_snd_wnd_test(const struct tcp_sock *tp, const struct rt_buf *b, u32 cur_mss)
{
	u32 end_seq = TCP_SKB_CB(b)->end_seq;

	if (b->len > cur_mss)
		end_seq = TCP_SKB_CB(b)->seq + cur_mss;
	return !rt_seq_after(end_seq, tcp_wnd_end(tp));
}

static void tcp_event_new_data_sent(struct tcp_sock *tp, struct rt_buf *b)
{
	u32 prior_packets = tp->packets_out;

	tp->send_head = b->next;
	tp->snd_nxt = TCP_SKB_CB(b)->end_seq;
	tp->packets_out++;
	if (!prior_packets || tp->icsk_pending == ICSK_TIME_LOSS_PROBE)
		tcp_rearm_rto(tp);
}

/* Linux tcp_write_xmit(): send as much of the write queue as cwnd, the
 * peer's window and Nagle allow. push_one: 1 = one segment, 2 = TLP. */
int tcp_write_xmit(struct tcp_sock *tp, u32 mss_now, int nonagle, int push_one)
{
	struct rt_buf *b;
	u32 sent = 0;
	int is_cwnd_limited = 0, is_rwnd_limited = 0;

	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_SYN_SENT)
		return 0;
	if (!tp->packets_out && sysctl_tcp_slow_start_after_idle && tp->send_head) {
		s32 delta = (s32)(tcp_jiffies() - tp->lsndtime);
		if (delta > (s32)tp->icsk_rto)
			tcp_cwnd_restart(tp, delta);
	}
	while ((b = tp->send_head) != NULL) {
		u32 in_flight = tcp_packets_in_flight(tp);
		u32 quota = in_flight < tp->snd_cwnd ? tp->snd_cwnd - in_flight : 0;

		if (b->len > mss_now && tcp_fragment(tp, b, mss_now))
			break;
		if (!quota) {
			if (push_one == 2) {
				quota = 1;
			} else {
				is_cwnd_limited = 1;
				break;
			}
		}
		if (!tcp_snd_wnd_test(tp, b, mss_now)) {
			is_rwnd_limited = 1;
			break;
		}
		if (!tcp_nagle_test(tp, b, mss_now, b == tp->wq.tail ? nonagle : TCP_NAGLE_PUSH))
			break;
		if (tcp_transmit_skb(tp, b, 1))
			break;
		tcp_event_new_data_sent(tp, b);
		if (b->len < mss_now)
			tp->snd_sml = TCP_SKB_CB(b)->end_seq;      /* tcp_minshall_update */
		if (tcp_in_cwnd_reduction(tp))
			tp->prr_out++;
		sent++;
		if (push_one)
			break;
	}
	if (is_rwnd_limited)
		tcp_chrono_start(tp, TCP_CHRONO_RWND);
	else if (tp->dg.chrono_type == TCP_CHRONO_RWND)
		tcp_chrono_stop(tp, TCP_CHRONO_RWND);
	if (sent) {
		if (push_one != 2)
			tcp_schedule_loss_probe(tp, 0);
		is_cwnd_limited |= tcp_packets_in_flight(tp) >= tp->snd_cwnd;
		tcp_cwnd_validate(tp, is_cwnd_limited);
		return 0;
	}
	return !tp->packets_out && tp->send_head;
}

void __tcp_push_pending_frames(struct tcp_sock *tp, u32 cur_mss, int nonagle)
{
	if (tp->state == RT_TCPS_CLOSED)
		return;
	if (tcp_write_xmit(tp, cur_mss, nonagle, 0))
		tcp_check_probe_timer(tp);
}

void tcp_push_pending_frames(struct tcp_sock *tp)
{
	int nonagle;

	if (!tp->send_head)
		return;
	nonagle = tp->cork ? TCP_NAGLE_CORK : (tp->nodelay ? TCP_NAGLE_OFF : 0);
	__tcp_push_pending_frames(tp, tcp_current_mss(tp), nonagle);
}

void tcp_data_snd_check(struct tcp_sock *tp)
{
	tcp_push_pending_frames(tp);
	tcp_check_space(tp);
}

void tcp_check_probe_timer(struct tcp_sock *tp)
{
	if (!tp->packets_out && !tp->icsk_pending)
		tcp_reset_xmit_timer(tp, ICSK_TIME_PROBE0, tcp_probe0_when(tp, TCP_RTO_MAX));
}

/* ---- retransmission ---- */

/* Linux __tcp_retransmit_skb() + tcp_retransmit_skb() */
int tcp_retransmit_skb(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 cur_mss;
	int err;

	if (rt_seq_before(cb->seq, tp->snd_una)) {
		u32 trim;
		if (!rt_seq_after(cb->end_seq, tp->snd_una) || (cb->tcp_flags & TCPHDR_SYN))
			return -RT_EINVAL;
		trim = tp->snd_una - cb->seq;
		if (trim > b->len)
			trim = b->len;
		rt_buf_pull(b, trim);
		tp->wq.bytes -= RT_MIN(tp->wq.bytes, trim);
		tp->wq_bytes -= RT_MIN(tp->wq_bytes, trim);
		cb->seq = tp->snd_una;
	}
	cur_mss = tcp_current_mss(tp);
	if (!rt_seq_before(cb->seq, tcp_wnd_end(tp)) && cb->seq != tp->snd_una)
		return -RT_EAGAIN;
	if (b->len > cur_mss && tcp_fragment(tp, b, cur_mss))
		return -RT_ENOMEM;
	/* RFC 3168 6.1.1: a retransmitted SYN gives up on ECN */
	if ((cb->tcp_flags & TCPHDR_SYN) && (cb->tcp_flags & (TCPHDR_ECE | TCPHDR_CWR)) &&
	    tp->state == RT_TCPS_SYN_SENT) {
		cb->tcp_flags &= ~(TCPHDR_ECE | TCPHDR_CWR);
		tp->ecn_flags &= ~TCP_ECN_OK;
	}
	err = tcp_transmit_skb(tp, b, 1);
	if (err)
		return err;
	if (!(cb->sacked & TCPCB_SACKED_RETRANS))
		tp->retrans_out++;
	cb->sacked |= TCPCB_RETRANS;
	cb->retrans++;
	tp->total_retrans++;
	rt_tcpstat.RetransSegs++;
	if (!tp->retrans_stamp)
		tp->retrans_stamp = tcp_jiffies() ? tcp_jiffies() : 1;
	if (tp->undo_retrans < 0)
		tp->undo_retrans = 0;
	tp->undo_retrans++;
	return 0;
}

/* Linux tcp_xmit_retransmit_queue(): retransmit segments marked lost,
 * within cwnd (PRR controls cwnd during recovery). */
void tcp_xmit_retransmit_queue(struct tcp_sock *tp)
{
	struct rt_buf *b, *rtx_head = tcp_rtx_head(tp);
	int rearm = 0;

	if (!tp->packets_out)
		return;
	for (b = rtx_head; b && b != tp->send_head; b = b->next) {
		u8 sacked = TCP_SKB_CB(b)->sacked;
		u32 in_flight = tcp_packets_in_flight(tp);

		if (in_flight >= tp->snd_cwnd)
			break;
		if (tp->retrans_out >= tp->lost_out)
			break;
		if (!(sacked & TCPCB_LOST))
			continue;
		if (sacked & (TCPCB_SACKED_ACKED | TCPCB_SACKED_RETRANS))
			continue;
		if (tcp_retransmit_skb(tp, b))
			break;
		if (tp->ca_state != TCP_CA_Loss)
			rt_tcpstat.TCPFastRetrans++;
		else
			rt_tcpstat.TCPSlowStartRetrans++;
		if (tcp_in_cwnd_reduction(tp))
			tp->prr_out++;
		if (b == rtx_head && tp->icsk_pending != ICSK_TIME_REO_TIMEOUT)
			rearm = 1;
	}
	if (rearm)
		tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, tp->icsk_rto);
}

/* Linux tcp_simple_retransmit(): after a PMTU decrease */
void tcp_simple_retransmit(struct tcp_sock *tp)
{
	u32 mss = tcp_current_mss(tp);
	struct rt_buf *b;

	for (b = tcp_rtx_head(tp); b && b != tp->send_head; b = b->next)
		if (b->len > mss)
			tcp_mark_skb_lost(tp, b);
	if (!tp->lost_out)
		return;
	if (tcp_is_reno(tp))
		tcp_limit_reno_sacked(tp);
	if (tp->ca_state != TCP_CA_Loss) {
		tp->high_seq = tp->snd_nxt;
		tp->snd_ssthresh = tcp_current_ssthresh(tp);
		tp->prior_ssthresh = 0;
		tp->undo_marker = 0;
		tcp_set_ca_state(tp, TCP_CA_Loss);
	}
	tcp_xmit_retransmit_queue(tp);
}

/* ---- probes ---- */
static int tcp_xmit_probe_skb(struct tcp_sock *tp, int urgent)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 0);

	if (!b)
		return -1;
	/* old sequence number: elicits an ACK without being accepted */
	TCP_SKB_CB(b)->seq = TCP_SKB_CB(b)->end_seq = tp->snd_una - (urgent ? 0 : 1);
	TCP_SKB_CB(b)->tcp_flags = TCPHDR_ACK;
	return tcp_transmit_skb(tp, b, 0);
}

/* Linux tcp_write_wakeup(): window probe / keepalive */
int tcp_write_wakeup(struct tcp_sock *tp)
{
	struct rt_buf *b;

	if (tp->state == RT_TCPS_CLOSED)
		return -1;
	b = tp->send_head;
	if (b && rt_seq_before(TCP_SKB_CB(b)->seq, tcp_wnd_end(tp))) {
		u32 mss = tcp_current_mss(tp);
		u32 seg_size = tcp_wnd_end(tp) - TCP_SKB_CB(b)->seq;
		int err;

		if (rt_seq_before(tp->pushed_seq, TCP_SKB_CB(b)->end_seq))
			tp->pushed_seq = TCP_SKB_CB(b)->end_seq;
		if (seg_size < TCP_SKB_CB(b)->end_seq - TCP_SKB_CB(b)->seq || b->len > mss) {
			seg_size = RT_MIN(seg_size, mss);
			TCP_SKB_CB(b)->tcp_flags |= TCPHDR_PSH;
			if (seg_size < b->len && tcp_fragment(tp, b, seg_size))
				return -1;
		}
		TCP_SKB_CB(b)->tcp_flags |= TCPHDR_PSH;
		err = tcp_transmit_skb(tp, b, 1);
		if (!err)
			tcp_event_new_data_sent(tp, b);
		return err;
	}
	if (rt_seq_between(tp->snd_up, tp->snd_una + 1, tp->snd_una + 0xffff))
		tcp_xmit_probe_skb(tp, 1);
	return tcp_xmit_probe_skb(tp, 0);
}

/* Linux tcp_send_probe0() */
void tcp_send_probe0(struct tcp_sock *tp)
{
	int err = tcp_write_wakeup(tp);
	u32 timeout;

	rt_tcpstat.TCPWinProbe++;
	if (tp->packets_out || !tp->send_head) {
		tp->icsk_probes_out = 0;
		tp->icsk_backoff = 0;
		tp->icsk_probes_tstamp = 0;
		return;
	}
	tp->icsk_probes_out++;
	TCP_DBG(tp, "zero window probe %lu", (unsigned long)tp->icsk_probes_out);
	if (err <= 0) {
		if (tp->icsk_backoff < (u32)sysctl_tcp_retries2)
			tp->icsk_backoff++;
		timeout = tcp_probe0_when(tp, TCP_RTO_MAX);
	} else {
		timeout = TCP_RESOURCE_PROBE_INTERVAL;
	}
	if (tp->icsk_user_timeout && tp->icsk_probes_tstamp) {
		s32 remaining = (s32)(tp->icsk_probes_tstamp + tp->icsk_user_timeout - tcp_jiffies());
		if (remaining <= 0)
			remaining = 1;
		if ((u32)remaining < timeout)
			timeout = (u32)remaining;
	}
	tcp_reset_xmit_timer(tp, ICSK_TIME_PROBE0, timeout);
}

/* RFC 8985 7.3 / Linux tcp_send_loss_probe(): probe with new data if
 * possible, else retransmit the highest sequence segment. */
void tcp_send_loss_probe(struct tcp_sock *tp)
{
	struct rt_buf *b = tp->send_head;
	u32 mss = tcp_current_mss(tp);

	if (b && tcp_snd_wnd_test(tp, b, mss)) {
		u32 pcount = tp->packets_out;
		tcp_write_xmit(tp, mss, TCP_NAGLE_OFF, 2);
		if (tp->packets_out > pcount)
			goto probe_sent;
		goto rearm_timer;
	}
	b = tp->send_head ? tp->send_head->prev : tp->wq.tail;
	if (!b || tp->tlp_high_seq)
		goto rearm_timer;
	if (b->len > mss && tcp_fragment(tp, b, mss))
		goto rearm_timer;
	b = tp->send_head ? tp->send_head->prev : tp->wq.tail;
	if (tcp_retransmit_skb(tp, b))
		goto rearm_timer;
	tp->tlp_retrans = 1;
probe_sent:
	tp->tlp_high_seq = tp->snd_nxt;
	rt_tcpstat.TCPLossProbes++;
	tp->dg.tlp_probes++;
	tp->icsk_pending = 0;
rearm_timer:
	tcp_rearm_rto(tp);
}
