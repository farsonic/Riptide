/*
 * Riptide - TCP loss recovery: SACK scoreboard (RFC 2018/2883), RACK-TLP
 * loss detection (RFC 8985), Proportional Rate Reduction (RFC 6937),
 * NewReno for non-SACK peers, RTO recovery with F-RTO (RFC 5682), undo of
 * spurious reductions (DSACK / Eifel timestamps) and the congestion state
 * machine (Open/Disorder/CWR/Recovery/Loss). Also the receiver side SACK
 * block generation. Follows Linux tcp_input.c and tcp_recovery.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

static int tcp_skb_sent_after(u64 t1, u64 t2, u32 seq1, u32 seq2)
{
	return t1 > t2 || (t1 == t2 && rt_seq_after(seq1, seq2));
}

void tcp_clear_retrans(struct tcp_sock *tp)
{
	tp->retrans_out = 0;
	tp->lost_out = 0;
	tp->undo_marker = 0;
	tp->undo_retrans = -1;
	tp->sacked_out = 0;
}

static int tcp_any_retrans_done(const struct tcp_sock *tp)
{
	const struct rt_buf *b;

	if (tp->retrans_out)
		return 1;
	b = tcp_rtx_head(tp);
	return b && (TCP_SKB_CB(b)->sacked & TCPCB_EVER_RETRANS);
}

/* Linux tcp_mark_skb_lost() */
void tcp_mark_skb_lost(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);

	if (cb->sacked & TCPCB_SACKED_ACKED)
		return;
	if (cb->sacked & TCPCB_LOST) {
		if (cb->sacked & TCPCB_SACKED_RETRANS) {
			/* a retransmission that is lost again */
			cb->sacked &= ~TCPCB_SACKED_RETRANS;
			tp->retrans_out--;
		}
	} else {
		tp->lost_out++;
		cb->sacked |= TCPCB_LOST;
	}
}

/* ---- reordering ---- */
static void tcp_update_reordering(struct tcp_sock *tp, u32 metric)
{
	if (metric > tp->reordering) {
		tp->reordering = RT_MIN(metric, TCP_MAX_REORDERING);
		tp->reord_seen++;
		rt_tcpstat.TCPSACKReorder++;
	}
}

/* Linux tcp_check_sack_reordering() */
void tcp_check_sack_reordering(struct tcp_sock *tp, u32 low_seq, int ts)
{
	u32 mss = tp->mss_cache, fack, metric;

	fack = tcp_highest_sack_seq(tp);
	if (!rt_seq_before(low_seq, fack))
		return;
	metric = fack - low_seq;
	if (mss && metric > tp->reordering * mss)
		tp->reordering = RT_MIN((metric + mss - 1) / mss, TCP_MAX_REORDERING);
	tp->reord_seen++;
	rt_tcpstat.TCPSACKReorder++;
}

/* ---- NewReno: emulate SACK with duplicate ACK counting ---- */
int tcp_limit_reno_sacked(struct tcp_sock *tp)
{
	u32 holes = RT_MAX(tp->lost_out, 1u);

	holes = RT_MIN(holes, tp->packets_out);
	if (tp->sacked_out + holes > tp->packets_out) {
		tp->sacked_out = tp->packets_out - holes;
		return 1;
	}
	return 0;
}

static void tcp_check_reno_reordering(struct tcp_sock *tp, int addend)
{
	if (tcp_limit_reno_sacked(tp))
		tcp_update_reordering(tp, tp->packets_out + (u32)addend);
}

static void tcp_add_reno_sack(struct tcp_sock *tp, int num_dupack)
{
	if (num_dupack) {
		u32 prior_sacked = tp->sacked_out;
		tp->sacked_out += (u32)num_dupack;
		tcp_check_reno_reordering(tp, 0);
		if (tp->sacked_out > prior_sacked)
			tp->delivered += tp->sacked_out - prior_sacked;
	}
}

void tcp_remove_reno_sacks(struct tcp_sock *tp, int acked)
{
	if (acked > 0) {
		/* one ACK filled the hole, the rest eat duplicate ACKs */
		tp->delivered += (u32)RT_MAX(acked - (int)tp->sacked_out, 1);
		if ((u32)(acked - 1) >= tp->sacked_out)
			tp->sacked_out = 0;
		else
			tp->sacked_out -= (u32)(acked - 1);
	}
	tcp_check_reno_reordering(tp, acked);
}

static void tcp_reset_reno_sack(struct tcp_sock *tp)
{
	tp->sacked_out = 0;
}

/* ---- SACK scoreboard (Linux tcp_sacktag_write_queue) ---- */
static int tcp_is_sackblock_valid(const struct tcp_sock *tp, int is_dsack, u32 start_seq, u32 end_seq)
{
	if (rt_seq_after(end_seq, tp->snd_nxt) || !rt_seq_before(start_seq, end_seq))
		return 0;
	if (!rt_seq_before(start_seq, tp->snd_nxt))
		return 0;
	if (rt_seq_after(start_seq, tp->snd_una))
		return 1;
	if (!is_dsack || !tp->undo_marker)
		return 0;
	if (rt_seq_after(end_seq, tp->snd_una))
		return 0;
	if (!rt_seq_before(start_seq, tp->undo_marker))
		return 1;
	if (!rt_seq_after(end_seq, tp->undo_marker))
		return 0;
	return !rt_seq_before(start_seq, end_seq - tp->max_window);
}

/* RFC 2883 D-SACK detection (Linux tcp_check_dsack) */
static int tcp_check_dsack(struct tcp_sock *tp, const struct rt_buf *ack_skb, const u8 *sp,
                           int num_sacks, u32 prior_snd_una, int *flag)
{
	u32 start_seq_0 = rt_get_be32(sp), end_seq_0 = rt_get_be32(sp + 4);
	int dup_sack = 0;

	if (rt_seq_before(start_seq_0, TCP_SKB_CB(ack_skb)->ack_seq)) {
		dup_sack = 1;
	} else if (num_sacks > 1) {
		u32 end_seq_1 = rt_get_be32(sp + 12), start_seq_1 = rt_get_be32(sp + 8);
		if (!rt_seq_after(end_seq_0, end_seq_1) && !rt_seq_before(start_seq_0, start_seq_1))
			dup_sack = 1;
	}
	if (!dup_sack)
		return 0;
	rt_tcpstat.TCPDSACKRecv++;
	tp->rack.dsack_seen = 1;
	if (tp->tlp_high_seq && tp->tlp_retrans && end_seq_0 == tp->tlp_high_seq)
		*flag |= FLAG_DSACK_TLP;
	/* D-SACK for data already cumulatively acked: count it now */
	if (tp->undo_marker && tp->undo_retrans > 0 && !rt_seq_after(end_seq_0, prior_snd_una) &&
	    rt_seq_after(end_seq_0, tp->undo_marker))
		tp->undo_retrans--;
	return 1;
}

/* Linux tcp_sacktag_one() */
static u8 tcp_sacktag_one(struct tcp_sock *tp, struct tcp_sacktag_state *state, int *flag,
                          u8 sacked, u32 start_seq, u32 end_seq, int dup_sack, u64 xmit_us)
{
	if (dup_sack && (sacked & TCPCB_RETRANS)) {
		if (tp->undo_marker && tp->undo_retrans > 0 && rt_seq_after(end_seq, tp->undo_marker))
			tp->undo_retrans--;
		if ((sacked & TCPCB_SACKED_ACKED) && rt_seq_before(start_seq, state->reord))
			state->reord = start_seq;
	}
	if (!rt_seq_after(end_seq, tp->snd_una))
		return sacked;
	if (!(sacked & TCPCB_SACKED_ACKED)) {
		tcp_rack_advance(tp, sacked, end_seq, xmit_us);
		if (sacked & TCPCB_SACKED_RETRANS) {
			if (sacked & TCPCB_LOST) {
				sacked &= ~(TCPCB_LOST | TCPCB_SACKED_RETRANS);
				tp->lost_out--;
				tp->retrans_out--;
			}
		} else {
			if (!(sacked & TCPCB_RETRANS)) {
				/* new SACK for a never retransmitted segment
				 * below the highest SACK: reordering */
				if (rt_seq_before(start_seq, tcp_highest_sack_seq(tp)) &&
				    rt_seq_before(start_seq, state->reord))
					state->reord = start_seq;
				if (!rt_seq_after(end_seq, tp->high_seq))
					*flag |= FLAG_ORIG_SACK_ACKED;
				if (state->first_sackt == 0)
					state->first_sackt = xmit_us;
				state->last_sackt = xmit_us;
			}
			if (sacked & TCPCB_LOST) {
				sacked &= ~TCPCB_LOST;
				tp->lost_out--;
			}
		}
		sacked |= TCPCB_SACKED_ACKED;
		*flag |= FLAG_DATA_SACKED;
		if (!tp->sacked_out || rt_seq_after(start_seq, tp->highest_sack_seq))
			tp->highest_sack_seq = start_seq;
		tp->sacked_out++;
		tp->delivered++;
		state->sack_delivered++;
	}
	if (dup_sack && (sacked & TCPCB_SACKED_RETRANS)) {
		sacked &= ~TCPCB_SACKED_RETRANS;
		tp->retrans_out--;
	}
	return sacked;
}

int tcp_sacktag_write_queue(struct tcp_sock *tp, const struct rt_buf *ack_skb,
                            u32 prior_snd_una, struct tcp_sacktag_state *state)
{
	const u8 *opt = ack_skb->data + tp->rx_opt.sack_off;
	const u8 *sp = opt + 2;
	struct tcp_sack_block blk[TCP_NUM_SACKS];
	u8 dup[TCP_NUM_SACKS];
	int num_sacks, used = 0, i, j, flag = 0, found_dup_sack;

	if (tp->rx_opt.sack_off + 2u > TCP_SKB_CB(ack_skb)->hdr_len)
		return 0;
	num_sacks = (opt[1] - TCPOLEN_SACK_BASE) / TCPOLEN_SACK_PERBLOCK;
	if (num_sacks > TCP_NUM_SACKS)
		num_sacks = TCP_NUM_SACKS;
	if (num_sacks <= 0 || tp->rx_opt.sack_off + 2u + (u32)num_sacks * 8u > TCP_SKB_CB(ack_skb)->hdr_len)
		return 0;
	TCP_DBG(tp, "SACK: %d blocks, first %lu-%lu, snd_una %lu", num_sacks, (unsigned long)rt_get_be32(sp),
	        (unsigned long)rt_get_be32(sp + 4), (unsigned long)tp->snd_una);
	state->reord = tp->snd_nxt;
	found_dup_sack = tcp_check_dsack(tp, ack_skb, sp, num_sacks, prior_snd_una, &flag);
	if (found_dup_sack) {
		flag |= FLAG_DSACKING_ACK;
		tp->delivered++;
	}
	/* very old ACKs carry no usable SACK information */
	if (rt_seq_before(TCP_SKB_CB(ack_skb)->ack_seq, prior_snd_una - tp->max_window))
		return 0;
	if (!tp->packets_out)
		return flag;

	for (i = 0; i < num_sacks; i++) {
		int dup_sack = !i && found_dup_sack;
		u32 s = rt_get_be32(sp + 8 * i), e = rt_get_be32(sp + 8 * i + 4);

		if (!tcp_is_sackblock_valid(tp, dup_sack, s, e))
			continue;
		if (!rt_seq_after(e, prior_snd_una) && !dup_sack)
			continue;
		blk[used].start_seq = s;
		blk[used].end_seq = e;
		dup[used] = (u8)dup_sack;
		used++;
	}
	/* sort by start sequence (at most 4 blocks) */
	for (i = 1; i < used; i++) {
		for (j = i; j > 0 && rt_seq_after(blk[j - 1].start_seq, blk[j].start_seq); j--) {
			struct tcp_sack_block t = blk[j];
			u8 d = dup[j];
			blk[j] = blk[j - 1];
			dup[j] = dup[j - 1];
			blk[j - 1] = t;
			dup[j - 1] = d;
		}
	}
	for (i = 0; i < used; i++) {
		struct rt_buf *b;
		for (b = tcp_rtx_head(tp); b && b != tp->send_head; b = b->next) {
			struct tcp_skb_cb *cb = TCP_SKB_CB(b);
			if (!rt_seq_before(cb->seq, blk[i].end_seq))
				break;
			if (rt_seq_before(cb->seq, blk[i].start_seq) ||
			    rt_seq_after(cb->end_seq, blk[i].end_seq))
				continue;
			cb->sacked = tcp_sacktag_one(tp, state, &flag, cb->sacked, cb->seq,
			                             cb->end_seq, dup[i], cb->tx_us);
		}
	}
	if (tp->ca_state != TCP_CA_Loss || tp->undo_marker)
		tcp_check_sack_reordering(tp, state->reord, 0);
	return flag;
}

/* ---- RACK (RFC 8985) ---- */
static u32 tcp_rack_reo_wnd(const struct tcp_sock *tp)
{
	if (!tp->reord_seen) {
		/* no reordering seen: fall back to DupThresh behaviour */
		if (tp->ca_state >= TCP_CA_Recovery)
			return 0;
		if (tp->sacked_out >= tp->reordering && !(sysctl_tcp_recovery & TCP_RACK_NO_DUPTHRESH))
			return 0;
	}
	return RT_MIN((tcp_min_rtt(tp) >> 2) * tp->rack.reo_wnd_steps, tp->srtt_us >> 3);
}

s32 tcp_rack_skb_timeout(struct tcp_sock *tp, const struct rt_buf *b, u32 reo_wnd)
{
	return (s32)(tp->rack.rtt_us + reo_wnd) - (s32)(tcp_clock_us - TCP_SKB_CB(b)->tx_us);
}

/* RFC 8985 6.2 step 5: RACK detect loss */
static void tcp_rack_detect_loss(struct tcp_sock *tp, u32 *reo_timeout)
{
	struct rt_buf *b;
	u32 reo_wnd;

	*reo_timeout = 0;
	reo_wnd = tcp_rack_reo_wnd(tp);
	for (b = tcp_rtx_head(tp); b && b != tp->send_head; b = b->next) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(b);
		s32 remaining;

		if (cb->sacked & TCPCB_SACKED_ACKED)
			continue;
		if ((cb->sacked & TCPCB_LOST) && !(cb->sacked & TCPCB_SACKED_RETRANS))
			continue;
		if (!tcp_skb_sent_after(tp->rack.mstamp, cb->tx_us, tp->rack.end_seq, cb->end_seq))
			continue;
		remaining = tcp_rack_skb_timeout(tp, b, reo_wnd);
		if (remaining <= 0) {
			tcp_mark_skb_lost(tp, b);
			rt_tcpstat.TCPRackLoss++;
		} else if ((u32)remaining > *reo_timeout) {
			*reo_timeout = (u32)remaining;
		}
	}
}

static int tcp_rack_mark_lost(struct tcp_sock *tp)
{
	u32 timeout;

	if (!tp->rack.advanced)
		return 0;
	tp->rack.advanced = 0;
	tcp_rack_detect_loss(tp, &timeout);
	if (timeout) {
		tcp_reset_xmit_timer(tp, ICSK_TIME_REO_TIMEOUT, (timeout + 999) / 1000 + TCP_TIMEOUT_MIN);
		return 1;
	}
	return 0;
}

/* RFC 8985 6.2 step 2: record the most recently sent delivered segment */
void tcp_rack_advance(struct tcp_sock *tp, u8 sacked, u32 end_seq, u64 xmit_us)
{
	u32 rtt_us = (u32)(tcp_clock_us - xmit_us);

	/* ambiguous sample from a retransmission */
	if ((sacked & TCPCB_RETRANS) && rtt_us < tcp_min_rtt(tp))
		return;
	tp->rack.advanced = 1;
	tp->rack.rtt_us = rtt_us;
	if (tcp_skb_sent_after(xmit_us, tp->rack.mstamp, end_seq, tp->rack.end_seq)) {
		tp->rack.mstamp = xmit_us;
		tp->rack.end_seq = end_seq;
	}
}

/* RFC 8985 6.2 step 4: adapt the reordering window with DSACKs */
void tcp_rack_update_reo_wnd(struct tcp_sock *tp, u32 prior_delivered)
{
	if ((sysctl_tcp_recovery & TCP_RACK_STATIC_REO_WND) || !prior_delivered)
		return;
	if (rt_seq_before(prior_delivered, tp->rack.last_delivered))
		tp->rack.dsack_seen = 0;
	if (tp->rack.dsack_seen) {
		tp->rack.reo_wnd_steps = (u8)RT_MIN(0xffu, tp->rack.reo_wnd_steps + 1u);
		tp->rack.dsack_seen = 0;
		tp->rack.last_delivered = tp->delivered;
		tp->rack.reo_wnd_persist = TCP_RACK_RECOVERY_THRESH;
	} else if (!tp->rack.reo_wnd_persist) {
		tp->rack.reo_wnd_steps = 1;
	}
}

/* ---- PRR (RFC 6937) and cwnd reduction ---- */
void tcp_init_cwnd_reduction(struct tcp_sock *tp)
{
	tp->high_seq = tp->snd_nxt;
	tp->tlp_high_seq = 0;
	tp->snd_cwnd_cnt = 0;
	tp->prior_cwnd = tp->snd_cwnd;
	tp->prr_delivered = 0;
	tp->prr_out = 0;
	tp->snd_ssthresh = tp->ca_ops->ssthresh(tp);
	if (tp->ecn_flags & TCP_ECN_OK)
		tp->ecn_flags |= TCP_ECN_QUEUE_CWR;
}

/* Linux tcp_cwnd_reduction(): PRR-SSRB */
void tcp_cwnd_reduction(struct tcp_sock *tp, int newly_acked_sacked, int newly_lost, int flag)
{
	s32 sndcnt = 0;
	s32 delta = (s32)tp->snd_ssthresh - (s32)tcp_packets_in_flight(tp);

	if (newly_acked_sacked <= 0 || !tp->prior_cwnd)
		return;
	tp->prr_delivered += (u32)newly_acked_sacked;
	if (delta < 0) {
		u64 dividend = (u64)tp->snd_ssthresh * tp->prr_delivered + tp->prior_cwnd - 1;
		sndcnt = (s32)(dividend / tp->prior_cwnd) - (s32)tp->prr_out;
	} else {
		sndcnt = RT_MAX((s32)tp->prr_delivered - (s32)tp->prr_out, newly_acked_sacked);
		if ((flag & FLAG_SND_UNA_ADVANCED) && !newly_lost)
			sndcnt++;
		sndcnt = RT_MIN(delta, sndcnt);
	}
	/* force a fast retransmit upon entering fast recovery */
	sndcnt = RT_MAX(sndcnt, (tp->prr_out ? 0 : 1));
	tcp_snd_cwnd_set(tp, (u32)((s32)tcp_packets_in_flight(tp) + sndcnt));
}

void tcp_end_cwnd_reduction(struct tcp_sock *tp)
{
	if (tp->ca_state == TCP_CA_CWR || (tp->undo_marker && tp->snd_ssthresh < TCP_INFINITE_SSTHRESH)) {
		tp->snd_cwnd = tp->snd_ssthresh;
		tp->snd_cwnd_stamp = tcp_jiffies();
	}
	tcp_ca_event(tp, CA_EVENT_COMPLETE_CWR);
}

/* RFC 3168: react to ECE once per window */
void tcp_enter_cwr(struct tcp_sock *tp)
{
	tp->prior_ssthresh = 0;
	if (tp->ca_state < TCP_CA_CWR) {
		tp->undo_marker = 0;
		tcp_init_cwnd_reduction(tp);
		tcp_set_ca_state(tp, TCP_CA_CWR);
	}
}

void tcp_try_keep_open(struct tcp_sock *tp)
{
	u8 state = TCP_CA_Open;

	if (tcp_left_out(tp) || tcp_any_retrans_done(tp))
		state = TCP_CA_Disorder;
	if (tp->ca_state != state) {
		tcp_set_ca_state(tp, state);
		tp->high_seq = tp->snd_nxt;
	}
}

static void tcp_try_to_open(struct tcp_sock *tp, int flag)
{
	if (!tcp_any_retrans_done(tp))
		tp->retrans_stamp = 0;
	if (flag & FLAG_ECE)
		tcp_enter_cwr(tp);
	if (tp->ca_state != TCP_CA_CWR)
		tcp_try_keep_open(tp);
}

/* ---- undo (Eifel / DSACK) ---- */
static void tcp_init_undo(struct tcp_sock *tp)
{
	tp->undo_marker = tp->snd_una;
	tp->undo_retrans = (s32)tp->retrans_out;
	if (tp->tlp_high_seq && tp->tlp_retrans)
		tp->undo_retrans++;
	if (!tp->undo_retrans)
		tp->undo_retrans = -1;
}

/* the original transmission arrived (TSecr predates the retransmission) */
static int tcp_packet_delayed(const struct tcp_sock *tp)
{
	return tp->retrans_stamp && tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr &&
	       rt_seq_before(tp->rx_opt.rcv_tsecr, tp->retrans_stamp);
}

static int tcp_may_undo(const struct tcp_sock *tp)
{
	return tp->undo_marker && (!tp->undo_retrans || tcp_packet_delayed(tp));
}

static void tcp_undo_cwnd_reduction(struct tcp_sock *tp, int unmark_loss)
{
	if (unmark_loss) {
		struct rt_buf *b;
		for (b = tcp_rtx_head(tp); b && b != tp->send_head; b = b->next)
			TCP_SKB_CB(b)->sacked &= ~TCPCB_LOST;
		tp->lost_out = 0;
	}
	if (tp->prior_ssthresh) {
		tp->snd_cwnd = tp->ca_ops->undo_cwnd(tp);
		if (tp->prior_ssthresh > tp->snd_ssthresh) {
			tp->snd_ssthresh = tp->prior_ssthresh;
			tp->ecn_flags &= ~TCP_ECN_QUEUE_CWR;
		}
	}
	tp->snd_cwnd_stamp = tcp_jiffies();
	tp->undo_marker = 0;
	tp->rack.advanced = 1;
}

static int tcp_try_undo_recovery(struct tcp_sock *tp)
{
	if (tcp_may_undo(tp)) {
		tcp_undo_cwnd_reduction(tp, 0);
		rt_tcpstat.TCPFullUndo++;
		tp->undo_marker = 0;
	} else if (tp->rack.reo_wnd_persist) {
		tp->rack.reo_wnd_persist--;
	}
	if (tp->snd_una == tp->high_seq && tcp_is_reno(tp)) {
		/* hold state until something above high_seq is ACKed */
		if (!tcp_any_retrans_done(tp))
			tp->retrans_stamp = 0;
		return 1;
	}
	tcp_set_ca_state(tp, TCP_CA_Open);
	tp->is_sack_reneg = 0;
	return 0;
}

static int tcp_try_undo_dsack(struct tcp_sock *tp)
{
	if (tp->undo_marker && !tp->undo_retrans) {
		tp->rack.reo_wnd_persist = (u8)RT_MIN(TCP_RACK_RECOVERY_THRESH, tp->rack.reo_wnd_persist + 1);
		tcp_undo_cwnd_reduction(tp, 0);
		return 1;
	}
	return 0;
}

static int tcp_try_undo_loss(struct tcp_sock *tp, int frto_undo)
{
	if (frto_undo || tcp_may_undo(tp)) {
		tcp_undo_cwnd_reduction(tp, 1);
		if (frto_undo)
			rt_tcpstat.TCPSpuriousRTOs++;
		tp->icsk_retransmits = 0;
		if (frto_undo || tcp_is_sack(tp)) {
			tcp_set_ca_state(tp, TCP_CA_Open);
			tp->is_sack_reneg = 0;
		}
		return 1;
	}
	return 0;
}

static int tcp_force_fast_retransmit(const struct tcp_sock *tp)
{
	return rt_seq_after(tcp_highest_sack_seq(tp), tp->snd_una + tp->reordering * tp->mss_cache);
}

static int tcp_try_undo_partial(struct tcp_sock *tp, u32 prior_snd_una, int *do_lost)
{
	if (tp->undo_marker && tcp_packet_delayed(tp)) {
		tcp_check_sack_reordering(tp, prior_snd_una, 1);
		if (tp->retrans_out)
			return 1;
		if (!tcp_any_retrans_done(tp))
			tp->retrans_stamp = 0;
		tcp_undo_cwnd_reduction(tp, 1);
		tcp_try_keep_open(tp);
	} else {
		*do_lost = tcp_force_fast_retransmit(tp);
	}
	return 0;
}

/* ---- loss marking ---- */
static void tcp_newreno_mark_lost(struct tcp_sock *tp, int snd_una_advanced)
{
	u8 state = tp->ca_state;

	if ((state < TCP_CA_Recovery && tp->sacked_out >= tp->reordering) ||
	    (state == TCP_CA_Recovery && snd_una_advanced)) {
		struct rt_buf *b = tcp_rtx_head(tp);
		u32 mss = tcp_current_mss(tp);
		if (!b || (TCP_SKB_CB(b)->sacked & TCPCB_LOST))
			return;
		if (b->len > mss)
			tcp_fragment(tp, b, mss);
		tcp_mark_skb_lost(tp, b);
	}
}

/* RFC 6675 style marking when RACK is disabled (Linux tcp_mark_head_lost) */
static void tcp_mark_head_lost(struct tcp_sock *tp, int packets, int mark_head)
{
	struct rt_buf *b;
	int cnt = 0;

	for (b = tcp_rtx_head(tp); b && b != tp->send_head; b = b->next) {
		if (TCP_SKB_CB(b)->sacked & TCPCB_SACKED_ACKED)
			cnt++;
		if (cnt > packets)
			break;
		if (!(TCP_SKB_CB(b)->sacked & TCPCB_LOST))
			tcp_mark_skb_lost(tp, b);
		if (mark_head)
			break;
	}
}

static void tcp_update_scoreboard(struct tcp_sock *tp, int fast_rexmit)
{
	int sacked_upto = (int)tp->sacked_out - (int)tp->reordering;

	if (sacked_upto >= 0)
		tcp_mark_head_lost(tp, sacked_upto, 0);
	else if (fast_rexmit)
		tcp_mark_head_lost(tp, 1, 1);
}

static void tcp_identify_packet_loss(struct tcp_sock *tp, int *ack_flag)
{
	if (!tcp_rtx_head(tp))
		return;
	if (tcp_is_reno(tp)) {
		tcp_newreno_mark_lost(tp, *ack_flag & FLAG_SND_UNA_ADVANCED);
	} else if (tcp_is_rack()) {
		u32 prior_retrans = tp->retrans_out;
		if (tcp_rack_mark_lost(tp))
			*ack_flag &= ~FLAG_SET_XMIT_TIMER;
		if (prior_retrans > tp->retrans_out)
			*ack_flag |= FLAG_LOST_RETRANS;
	}
}

static int tcp_time_to_recover(const struct tcp_sock *tp)
{
	if (tp->lost_out)
		return 1;
	if (!tcp_is_rack() && tp->sacked_out + 1 > tp->reordering)
		return 1;
	return 0;
}

static void tcp_enter_recovery(struct tcp_sock *tp, int ece_ack)
{
	TCP_DBG(tp, "fast recovery%s: cwnd %lu, %lu in flight, %lu SACKed, %lu lost", ece_ack ? " (ECN)" : "",
	        (unsigned long)tp->snd_cwnd, (unsigned long)tp->packets_out, (unsigned long)tp->sacked_out,
	        (unsigned long)tp->lost_out);
	if (tcp_is_reno(tp))
		rt_tcpstat.TCPRenoRecovery++;
	else
		rt_tcpstat.TCPSackRecovery++;
	tp->prior_ssthresh = 0;
	tcp_init_undo(tp);
	if (tp->rack.reo_wnd_persist)
		tp->rack.reo_wnd_persist--;
	if (!tcp_in_cwnd_reduction(tp)) {
		if (!ece_ack)
			tp->prior_ssthresh = tcp_current_ssthresh(tp);
		tcp_init_cwnd_reduction(tp);
	}
	tcp_set_ca_state(tp, TCP_CA_Recovery);
	tp->dg.fast_recoveries++;
}

/* RTO: Linux tcp_timeout_mark_lost() + tcp_enter_loss() */
static void tcp_timeout_mark_lost(struct tcp_sock *tp)
{
	struct rt_buf *b, *head = tcp_rtx_head(tp);
	int is_reneg = head && (TCP_SKB_CB(head)->sacked & TCPCB_SACKED_ACKED);

	if (is_reneg) {
		tp->sacked_out = 0;
		tp->is_sack_reneg = 1;
	} else if (tcp_is_reno(tp)) {
		tcp_reset_reno_sack(tp);
	}
	for (b = head; b && b != tp->send_head; b = b->next) {
		if (is_reneg)
			TCP_SKB_CB(b)->sacked &= ~TCPCB_SACKED_ACKED;
		else if (b != head && tcp_rack_skb_timeout(tp, b, 0) > 0)
			continue;   /* recently sent: not lost yet */
		tcp_mark_skb_lost(tp, b);
	}
}

void tcp_enter_loss(struct tcp_sock *tp)
{
	int new_recovery = tp->ca_state < TCP_CA_Recovery;

	tcp_timeout_mark_lost(tp);
	if (tp->ca_state <= TCP_CA_Disorder || !rt_seq_after(tp->high_seq, tp->snd_una) ||
	    (tp->ca_state == TCP_CA_Loss && !tp->icsk_retransmits)) {
		tp->prior_ssthresh = tcp_current_ssthresh(tp);
		tp->prior_cwnd = tp->snd_cwnd;
		tp->snd_ssthresh = tp->ca_ops->ssthresh(tp);
		tcp_ca_event(tp, CA_EVENT_LOSS);
		tcp_init_undo(tp);
	}
	tcp_snd_cwnd_set(tp, tcp_packets_in_flight(tp) + 1);
	tp->snd_cwnd_cnt = 0;
	tp->snd_cwnd_stamp = tcp_jiffies();
	if (tp->ca_state <= TCP_CA_Disorder && tp->sacked_out >= (u32)sysctl_tcp_reordering)
		tp->reordering = RT_MIN(tp->reordering, (u32)sysctl_tcp_reordering);
	tcp_set_ca_state(tp, TCP_CA_Loss);
	tp->high_seq = tp->snd_nxt;
	tp->tlp_high_seq = 0;
	if (tp->ecn_flags & TCP_ECN_OK)
		tp->ecn_flags |= TCP_ECN_QUEUE_CWR;
	/* F-RTO RFC 5682 3.1 step 1 */
	tp->frto = sysctl_tcp_frto && (new_recovery || tp->icsk_retransmits) &&
	           tp->state != RT_TCPS_SYN_SENT && tp->state != RT_TCPS_SYN_RECEIVED;
}

/* Linux tcp_process_loss(): F-RTO (RFC 5682) and RTO recovery */
static void tcp_process_loss(struct tcp_sock *tp, int flag, int num_dupack, int *rexmit)
{
	int recovered = !rt_seq_before(tp->snd_una, tp->high_seq);

	if ((flag & FLAG_SND_UNA_ADVANCED) && tcp_try_undo_loss(tp, 0))
		return;
	if (tp->frto) {
		/* step 3.b: never retransmitted data (s)acked: spurious */
		if ((flag & FLAG_ORIG_SACK_ACKED) && tcp_try_undo_loss(tp, 1))
			return;
		if (rt_seq_after(tp->snd_nxt, tp->high_seq)) {
			if ((flag & FLAG_DATA_SACKED) || num_dupack)
				tp->frto = 0;               /* step 3.a: real loss */
		} else if ((flag & FLAG_SND_UNA_ADVANCED) && !recovered) {
			tp->high_seq = tp->snd_nxt;
			/* step 2.b: try to send new data */
			if (tp->send_head && rt_seq_after(tcp_wnd_end(tp), tp->snd_nxt)) {
				*rexmit = REXMIT_NEW;
				return;
			}
			tp->frto = 0;
		}
	}
	if (recovered) {
		tcp_try_undo_recovery(tp);
		return;
	}
	if (tcp_is_reno(tp)) {
		if (rt_seq_after(tp->snd_nxt, tp->high_seq) && num_dupack)
			tcp_add_reno_sack(tp, num_dupack);
		else if (flag & FLAG_SND_UNA_ADVANCED)
			tcp_reset_reno_sack(tp);
	}
	*rexmit = REXMIT_LOST;
}

static int tcp_check_sack_reneging(struct tcp_sock *tp, int *ack_flag)
{
	if ((*ack_flag & FLAG_SACK_RENEGING) && (*ack_flag & FLAG_SND_UNA_ADVANCED)) {
		u32 delay = RT_MAX((tp->srtt_us >> 4) / 1000u, 10u);
		tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, delay);
		*ack_flag &= ~FLAG_SET_XMIT_TIMER;
		return 1;
	}
	return 0;
}

/* Linux tcp_fastretrans_alert(): the congestion state machine, run for
 * every "dubious" ACK (dupack, SACK, ECE, or not in Open state). */
void tcp_fastretrans_alert(struct tcp_sock *tp, u32 prior_snd_una, int num_dupack,
                           int *ack_flag, int *rexmit)
{
	int flag = *ack_flag;
	int ece_ack = flag & FLAG_ECE;
	int fast_rexmit = 0;
	int do_lost = num_dupack || ((flag & FLAG_DATA_SACKED) && tcp_force_fast_retransmit(tp));

	if (!tp->packets_out && tp->sacked_out)
		tp->sacked_out = 0;
	if (ece_ack)
		tp->prior_ssthresh = 0;
	if (tcp_check_sack_reneging(tp, ack_flag))
		return;

	/* D. exit conditions: high_seq acknowledged */
	if (tp->ca_state != TCP_CA_Open && !rt_seq_before(tp->snd_una, tp->high_seq)) {
		switch (tp->ca_state) {
		case TCP_CA_CWR:
			if (tp->snd_una != tp->high_seq) {
				tcp_end_cwnd_reduction(tp);
				tcp_set_ca_state(tp, TCP_CA_Open);
			}
			break;
		case TCP_CA_Recovery:
			if (tcp_is_reno(tp))
				tcp_reset_reno_sack(tp);
			if (tcp_try_undo_recovery(tp))
				return;
			tcp_end_cwnd_reduction(tp);
			break;
		default:
			break;
		}
	}

	/* E. process state */
	switch (tp->ca_state) {
	case TCP_CA_Recovery:
		if (!(flag & FLAG_SND_UNA_ADVANCED)) {
			if (tcp_is_reno(tp))
				tcp_add_reno_sack(tp, num_dupack);
		} else if (tcp_try_undo_partial(tp, prior_snd_una, &do_lost)) {
			return;
		}
		if (tcp_try_undo_dsack(tp))
			tcp_try_to_open(tp, flag);
		tcp_identify_packet_loss(tp, ack_flag);
		if (tp->ca_state != TCP_CA_Recovery) {
			if (!tcp_time_to_recover(tp))
				return;
			tcp_enter_recovery(tp, ece_ack);
		}
		break;
	case TCP_CA_Loss:
		tcp_process_loss(tp, flag, num_dupack, rexmit);
		tcp_identify_packet_loss(tp, ack_flag);
		if (!(tp->ca_state == TCP_CA_Open || (*ack_flag & FLAG_LOST_RETRANS)))
			return;
		/* fall through */
	default:
		if (tcp_is_reno(tp)) {
			if (flag & FLAG_SND_UNA_ADVANCED)
				tcp_reset_reno_sack(tp);
			tcp_add_reno_sack(tp, num_dupack);
		}
		if (tp->ca_state <= TCP_CA_Disorder)
			tcp_try_undo_dsack(tp);
		tcp_identify_packet_loss(tp, ack_flag);
		if (!tcp_time_to_recover(tp)) {
			tcp_try_to_open(tp, flag);
			return;
		}
		tcp_enter_recovery(tp, ece_ack);
		fast_rexmit = 1;
		break;
	}
	if (!tcp_is_rack() && tcp_is_sack(tp) && do_lost)
		tcp_update_scoreboard(tp, fast_rexmit);
	*rexmit = REXMIT_LOST;
}

void tcp_xmit_recovery(struct tcp_sock *tp, int rexmit)
{
	if (rexmit == REXMIT_NONE || tp->state == RT_TCPS_SYN_SENT)
		return;
	if (rexmit == REXMIT_NEW) {
		__tcp_push_pending_frames(tp, tcp_current_mss(tp), TCP_NAGLE_OFF);
		if (rt_seq_after(tp->snd_nxt, tp->high_seq))
			return;
		tp->frto = 0;
	}
	tcp_xmit_retransmit_queue(tp);
}

/* RACK reorder timer expiry (Linux tcp_rack_reo_timeout) */
void tcp_rack_reo_timeout(struct tcp_sock *tp)
{
	u32 timeout, prior_inflight = tcp_packets_in_flight(tp);

	tcp_rack_detect_loss(tp, &timeout);
	if (prior_inflight != tcp_packets_in_flight(tp)) {
		if (tp->ca_state != TCP_CA_Recovery) {
			tcp_enter_recovery(tp, 0);
			tcp_cwnd_reduction(tp, 1, 0, 0);
		}
		tcp_xmit_retransmit_queue(tp);
	}
	if (tp->icsk_pending != ICSK_TIME_RETRANS)
		tcp_rearm_rto(tp);
}

/* TLP (RFC 8985 7.4): was the probe repairing a loss? */
void tcp_process_tlp_ack(struct tcp_sock *tp, u32 ack, int flag)
{
	if (rt_seq_before(ack, tp->tlp_high_seq))
		return;
	if (!tp->tlp_retrans) {
		tp->tlp_high_seq = 0;
	} else if (flag & FLAG_DSACK_TLP) {
		tp->tlp_high_seq = 0;
	} else if (rt_seq_after(ack, tp->tlp_high_seq)) {
		/* the probe repaired a loss: reduce cwnd once */
		tcp_init_cwnd_reduction(tp);
		tcp_set_ca_state(tp, TCP_CA_CWR);
		tcp_end_cwnd_reduction(tp);
		tcp_try_keep_open(tp);
		rt_tcpstat.TCPLossProbeRecovery++;
	} else if (!(flag & (FLAG_SND_UNA_ADVANCED | FLAG_NOT_DUP | FLAG_DATA_SACKED))) {
		tp->tlp_high_seq = 0;
	}
}

/* ---- receiver: SACK blocks we advertise (Linux tcp_sack_*) ---- */
static int tcp_sack_extend(struct tcp_sack_block *sp, u32 seq, u32 end_seq)
{
	if (!rt_seq_after(seq, sp->end_seq) && !rt_seq_after(sp->start_seq, end_seq)) {
		if (rt_seq_before(seq, sp->start_seq))
			sp->start_seq = seq;
		if (rt_seq_after(end_seq, sp->end_seq))
			sp->end_seq = end_seq;
		return 1;
	}
	return 0;
}

void tcp_dsack_set(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	if (tcp_is_sack(tp) && sysctl_tcp_dsack) {
		tp->rx_opt.dsack = 1;
		tp->duplicate_sack[0].start_seq = seq;
		tp->duplicate_sack[0].end_seq = end_seq;
	}
}

void tcp_dsack_extend(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	if (!tp->rx_opt.dsack)
		tcp_dsack_set(tp, seq, end_seq);
	else
		tcp_sack_extend(tp->duplicate_sack, seq, end_seq);
}

static void tcp_sack_maybe_coalesce(struct tcp_sock *tp)
{
	struct tcp_sack_block *sp = &tp->selective_acks[0];
	int this_sack = 1;

	while (this_sack < tp->rx_opt.num_sacks) {
		if (tcp_sack_extend(sp, tp->selective_acks[this_sack].start_seq,
		                    tp->selective_acks[this_sack].end_seq)) {
			int i;
			tp->rx_opt.num_sacks--;
			for (i = this_sack; i < tp->rx_opt.num_sacks; i++)
				tp->selective_acks[i] = tp->selective_acks[i + 1];
			continue;
		}
		this_sack++;
	}
}

/* most recently changed block first (RFC 2018 4) */
void tcp_sack_new_ofo_skb(struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	struct tcp_sack_block *sp = tp->selective_acks;
	int cur_sacks = tp->rx_opt.num_sacks, this_sack;

	for (this_sack = 0; this_sack < cur_sacks; this_sack++) {
		if (tcp_sack_extend(&sp[this_sack], seq, end_seq)) {
			for (; this_sack > 0; this_sack--) {
				struct tcp_sack_block t = sp[this_sack];
				sp[this_sack] = sp[this_sack - 1];
				sp[this_sack - 1] = t;
			}
			if (cur_sacks > 1)
				tcp_sack_maybe_coalesce(tp);
			return;
		}
	}
	if (this_sack >= TCP_NUM_SACKS) {
		this_sack--;
		tp->rx_opt.num_sacks--;
	}
	for (; this_sack > 0; this_sack--)
		sp[this_sack] = sp[this_sack - 1];
	sp[0].start_seq = seq;
	sp[0].end_seq = end_seq;
	tp->rx_opt.num_sacks++;
}

void tcp_sack_remove(struct tcp_sock *tp)
{
	int num_sacks = tp->rx_opt.num_sacks, this_sack = 0;

	if (rt_bufq_empty(&tp->ofoq)) {
		tp->rx_opt.num_sacks = 0;
		return;
	}
	while (this_sack < num_sacks) {
		if (!rt_seq_before(tp->rcv_nxt, tp->selective_acks[this_sack].start_seq)) {
			int i;
			for (i = this_sack + 1; i < num_sacks; i++)
				tp->selective_acks[i - 1] = tp->selective_acks[i];
			num_sacks--;
			continue;
		}
		this_sack++;
	}
	tp->rx_opt.num_sacks = (u8)num_sacks;
}
