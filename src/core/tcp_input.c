/*
 * Riptide - TCP input: demultiplexing, option parsing, PAWS, RFC 5961
 * validation, ACK processing, RTT estimation, receive queues, delayed
 * ACKs and the state machine.
 * Follows Linux net/ipv4/tcp_input.c and tcp_ipv4.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"


/* ---- options ---- */

/* Linux tcp_parse_options(). 'th' is the header, hdr_len validated by the
 * caller (20..60). For SYNs the negotiation options are recorded; for
 * established connections only timestamps and SACK (when negotiated).
 * Malformed or truncated options end the parse. */
int tcp_parse_options(const u8 *th, u32 hdr_len, struct tcp_rx_opt *opt, int estab)
{
	const u8 *ptr = th + TCP_HDR_LEN;
	int length = (int)hdr_len - TCP_HDR_LEN;
	int syn = (th[13] & TCPHDR_SYN) != 0;

	opt->saw_tstamp = 0;
	opt->sack_off = 0;
	while (length > 0) {
		int opcode = *ptr++, opsize;

		if (opcode == TCPOPT_EOL)
			break;
		if (opcode == TCPOPT_NOP) {
			length--;
			continue;
		}
		if (length < 2)
			break;
		opsize = *ptr++;
		if (opsize < 2 || opsize > length)
			break;
		switch (opcode) {
		case TCPOPT_MSS:
			if (opsize == TCPOLEN_MSS && syn && !estab) {
				u16 in_mss = rt_get_be16(ptr);
				if (in_mss) {
					if (opt->user_mss && opt->user_mss < in_mss)
						in_mss = opt->user_mss;
					opt->mss_clamp = in_mss;
				}
			}
			break;
		case TCPOPT_WINDOW:
			if (opsize == TCPOLEN_WINDOW && syn && !estab && sysctl_tcp_window_scaling) {
				u8 ws = *ptr;
				opt->wscale_ok = 1;
				if (ws > TCP_MAX_WSCALE)
					ws = TCP_MAX_WSCALE;
				opt->snd_wscale = ws;
			}
			break;
		case TCPOPT_TIMESTAMP:
			if (opsize == TCPOLEN_TIMESTAMP &&
			    ((estab && opt->tstamp_ok) || (!estab && sysctl_tcp_timestamps))) {
				opt->saw_tstamp = 1;
				opt->rcv_tsval = rt_get_be32(ptr);
				opt->rcv_tsecr = rt_get_be32(ptr + 4);
			}
			break;
		case TCPOPT_SACK_PERM:
			if (opsize == TCPOLEN_SACK_PERM && syn && !estab && sysctl_tcp_sack) {
				opt->sack_ok = 1;
				opt->num_sacks = 0;
				opt->dsack = 0;
			}
			break;
		case TCPOPT_SACK:
			if (opsize >= TCPOLEN_SACK_BASE + TCPOLEN_SACK_PERBLOCK &&
			    !((opsize - TCPOLEN_SACK_BASE) % TCPOLEN_SACK_PERBLOCK) && opt->sack_ok)
				opt->sack_off = (u8)((ptr - 2) - th);
			break;
		default:
			break;
		}
		ptr += opsize - 2;
		length -= opsize;
	}
	return 0;
}

/* Linux tcp_fast_parse_options(): fast path for the common aligned TS. */
static int tcp_fast_parse_options(struct tcp_sock *tp, const u8 *th, u32 hdr_len)
{
	if (hdr_len == TCP_HDR_LEN) {
		tp->rx_opt.saw_tstamp = 0;
		tp->rx_opt.sack_off = 0;
		return 0;
	}
	if (tp->rx_opt.tstamp_ok && hdr_len == TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED &&
	    th[20] == TCPOPT_NOP && th[21] == TCPOPT_NOP &&
	    th[22] == TCPOPT_TIMESTAMP && th[23] == TCPOLEN_TIMESTAMP) {
		tp->rx_opt.saw_tstamp = 1;
		tp->rx_opt.sack_off = 0;
		tp->rx_opt.rcv_tsval = rt_get_be32(th + 24);
		tp->rx_opt.rcv_tsecr = rt_get_be32(th + 28);
	} else {
		tcp_parse_options(th, hdr_len, &tp->rx_opt, 1);
	}
	if (tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr)
		tp->rx_opt.rcv_tsecr -= tp->ts_offset;
	return 1;
}

/* ---- PAWS (RFC 7323 5) ---- */
int tcp_paws_check(const struct tcp_sock *tp, u32 ts_recent, u32 ts_recent_stamp,
                   u32 tsval, int paws_win)
{
	if ((s32)(ts_recent - tsval) <= paws_win)
		return 1;
	if ((s32)(tcp_seconds() - (ts_recent_stamp + TCP_PAWS_24DAYS)) >= 0)
		return 1;
	/* Ignore peers that send tsval 0 on SYN and real values later. */
	if (!ts_recent)
		return 1;
	return 0;
}

static int tcp_paws_reject(const struct tcp_sock *tp, int rst)
{
	if (tcp_paws_check(tp, tp->ts_recent, tp->ts_recent_stamp, tp->rx_opt.rcv_tsval, 0))
		return 0;
	/* RST: relax PAWS to MSL so peers that rebooted can reset us */
	if (rst && (s32)(tcp_seconds() - (tp->ts_recent_stamp + TCP_PAWS_MSL)) >= 0)
		return 0;
	return 1;
}

void tcp_store_ts_recent(struct tcp_sock *tp)
{
	tp->ts_recent = tp->rx_opt.rcv_tsval;
	tp->ts_recent_stamp = tcp_seconds();
}

static void tcp_replace_ts_recent(struct tcp_sock *tp, u32 seq)
{
	if (tp->rx_opt.saw_tstamp && !rt_seq_after(seq, tp->rcv_wup)) {
		if (tcp_paws_check(tp, tp->ts_recent, tp->ts_recent_stamp, tp->rx_opt.rcv_tsval, 0))
			tcp_store_ts_recent(tp);
	}
}

/* Linux tcp_disordered_ack(): a pure duplicate ACK that only failed PAWS
 * because of reordering is not an old duplicate. */
static int tcp_disordered_ack(const struct tcp_sock *tp, const struct rt_buf *b)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 seq = cb->seq, ack = cb->ack_seq;
	u32 nwin = (u32)cb->window << tp->rx_opt.snd_wscale;

	return (cb->tcp_flags & TCPHDR_ACK) && seq == cb->end_seq && seq == tp->rcv_nxt &&
	       ack == tp->snd_una &&
	       !(rt_seq_after(ack, tp->snd_una) || rt_seq_after(seq, tp->snd_wl1) ||
	         (seq == tp->snd_wl1 && (nwin > tp->snd_wnd || !nwin))) &&
	       (s32)(tp->ts_recent - tp->rx_opt.rcv_tsval) <= (s32)tp->icsk_rto;
}

static int tcp_paws_discard(const struct tcp_sock *tp, const struct rt_buf *b)
{
	return !tcp_paws_check(tp, tp->ts_recent, tp->ts_recent_stamp, tp->rx_opt.rcv_tsval,
	                       TCP_PAWS_WINDOW) &&
	       !tcp_disordered_ack(tp, b);
}

static int tcp_sequence(const struct tcp_sock *tp, u32 seq, u32 end_seq)
{
	return !rt_seq_before(end_seq, tp->rcv_wup) &&
	       !rt_seq_after(seq, tp->rcv_nxt + tcp_receive_window(tp));
}

/* ---- RTT estimation (RFC 6298, Linux tcp_rtt_estimator) ---- */
void tcp_rtt_estimator(struct tcp_sock *tp, s32 mrtt_us)
{
	s32 m = mrtt_us;
	u32 srtt = tp->srtt_us;

	if (srtt != 0) {
		m -= (s32)(srtt >> 3);
		srtt += (u32)m;
		if (m < 0) {
			m = -m;
			m -= (s32)(tp->mdev_us >> 2);
			if (m > 0)
				m >>= 3;
		} else {
			m -= (s32)(tp->mdev_us >> 2);
		}
		tp->mdev_us += (u32)m;
		if (tp->mdev_us > tp->mdev_max_us) {
			tp->mdev_max_us = tp->mdev_us;
			if (tp->mdev_max_us > tp->rttvar_us)
				tp->rttvar_us = tp->mdev_max_us;
		}
		if (rt_seq_after(tp->snd_una, tp->rtt_seq)) {
			if (tp->mdev_max_us < tp->rttvar_us)
				tp->rttvar_us -= (tp->rttvar_us - tp->mdev_max_us) >> 2;
			tp->rtt_seq = tp->snd_nxt;
			tp->mdev_max_us = tcp_rto_min_us();
		}
	} else {
		srtt = (u32)m << 3;
		tp->mdev_us = (u32)m << 1;
		tp->rttvar_us = RT_MAX(tp->mdev_us, tcp_rto_min_us());
		tp->mdev_max_us = tp->rttvar_us;
		tp->rtt_seq = tp->snd_nxt;
	}
	tp->srtt_us = RT_MAX(1u, srtt);
}

void tcp_set_rto(struct tcp_sock *tp)
{
	u32 rto_us = (tp->srtt_us >> 3) + tp->rttvar_us;
	u32 rto = (rto_us + 999) / 1000;

	if (rto > TCP_RTO_MAX)
		rto = TCP_RTO_MAX;
	tp->icsk_rto = rto;
}

void tcp_update_rtt_min(struct tcp_sock *tp, u32 rtt_us, int flag)
{
	if ((flag & FLAG_ACK_MAYBE_DELAYED) && rtt_us > tcp_min_rtt(tp))
		return;
	minmax_running_min(&tp->rtt_min, 300000u, tcp_jiffies(), rtt_us ? rtt_us : 1);
}

static s32 tcp_rtt_tsopt_us(const struct tcp_sock *tp)
{
	u32 delta = tcp_jiffies() - tp->rx_opt.rcv_tsecr;

	if (delta < 0x7fffffffu / 1000u) {
		if (!delta)
			delta = 1;
		return (s32)(delta * 1000u);
	}
	return -1;
}

/* Linux tcp_ack_update_rtt(): Karn's algorithm unless timestamps make
 * the sample unambiguous (RTTM). */
int tcp_ack_update_rtt(struct tcp_sock *tp, int flag, s32 seq_rtt_us, s32 sack_rtt_us, s32 ca_rtt_us)
{
	if (seq_rtt_us < 0)
		seq_rtt_us = sack_rtt_us;
	if (seq_rtt_us < 0 && tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr && (flag & FLAG_ACKED))
		seq_rtt_us = ca_rtt_us = tcp_rtt_tsopt_us(tp);
	if (seq_rtt_us < 0)
		return 0;
	tcp_update_rtt_min(tp, (u32)ca_rtt_us, flag);
	tcp_rtt_estimator(tp, seq_rtt_us);
	tcp_set_rto(tp);
	tp->icsk_backoff = 0;
	return 1;
}

/* Receiver side RTT estimate for buffer auto-tuning */
void tcp_rcv_rtt_update(struct tcp_sock *tp, u32 sample, int win_dep)
{
	u32 new_sample = tp->rcv_rtt_est.rtt_us;
	s32 m = (s32)sample;

	if (new_sample != 0) {
		if (!win_dep) {
			m -= (s32)(new_sample >> 3);
			new_sample += (u32)m;
		} else {
			m <<= 3;
			if ((u32)m < new_sample)
				new_sample = (u32)m;
		}
	} else {
		new_sample = (u32)m << 3;
	}
	tp->rcv_rtt_est.rtt_us = new_sample;
}

static void tcp_rcv_rtt_measure(struct tcp_sock *tp)
{
	u32 delta_us;

	if (tp->rcv_rtt_est.time == 0)
		goto new_measure;
	if (rt_seq_before(tp->rcv_nxt, tp->rcv_rtt_est.seq))
		return;
	delta_us = (u32)(tcp_clock_us - tp->rcv_rtt_est.time);
	if (!delta_us)
		delta_us = 1;
	tcp_rcv_rtt_update(tp, delta_us, 1);
new_measure:
	tp->rcv_rtt_est.seq = tp->rcv_nxt + tp->rcv_wnd;
	tp->rcv_rtt_est.time = tcp_clock_us;
}

static void tcp_rcv_rtt_measure_ts(struct tcp_sock *tp, const struct rt_buf *b)
{
	if (!tp->rx_opt.saw_tstamp || !tp->rx_opt.rcv_tsecr ||
	    tp->rx_opt.rcv_tsecr == tp->rcv_rtt_last_tsecr)
		return;
	tp->rcv_rtt_last_tsecr = tp->rx_opt.rcv_tsecr;
	if (TCP_SKB_CB(b)->end_seq - TCP_SKB_CB(b)->seq >= tp->ack.rcv_mss) {
		s32 delta = tcp_rtt_tsopt_us(tp);
		if (delta >= 0)
			tcp_rcv_rtt_update(tp, (u32)delta, 0);
	}
}

/* ---- receive buffer & window helpers ---- */
u32 tcp_space(const struct tcp_sock *tp)
{
	u32 used = tp->rcvq_bytes + tp->ofo_bytes;
	return used < tp->rcvbuf ? tp->rcvbuf - used : 0;
}

static u32 tcp_rmem_limit(const struct tcp_sock *tp)
{
	return tp->rcvbuf * 2u + 16384u;
}

void tcp_initialize_rcv_mss(struct tcp_sock *tp)
{
	u32 hint = RT_MIN((u32)tp->advmss, (u32)tp->mss_cache);

	hint = RT_MIN(hint, tp->rcv_wnd / 2);
	hint = RT_MIN(hint, (u32)TCP_MSS_DEFAULT);
	hint = RT_MAX(hint, (u32)TCP_MIN_MSS);
	tp->ack.rcv_mss = (u16)hint;
}

void tcp_init_buffer_space(struct tcp_sock *tp)
{
	tcp_mstamp_refresh();
	tp->rcvq_space.time = tcp_clock_us;
	tp->rcvq_space.seq = tp->copied_seq;
	tp->rcvq_space.space = RT_MIN(RT_MIN(tp->rcv_ssthresh, tp->rcv_wnd),
	                              (u32)sysctl_tcp_initcwnd * tp->advmss);
	if (tp->window_clamp > tcp_full_space(tp) && tp->rcvbuf_locked)
		tp->window_clamp = tcp_full_space(tp);
	tp->rcv_ssthresh = RT_MIN(tp->rcv_ssthresh, tp->window_clamp);
}

/* Linux tcp_grow_window(): open rcv_ssthresh as in-order data arrives */
static void tcp_grow_window(struct tcp_sock *tp, u32 len)
{
	u32 room = RT_MIN(tp->window_clamp, tcp_space(tp));

	if (room <= tp->rcv_ssthresh)
		return;
	if (tp->rmem_alloc > tcp_rmem_limit(tp) / 2)
		return;
	room -= tp->rcv_ssthresh;
	tp->rcv_ssthresh += RT_MIN(room, 2u * tp->advmss + len);
}

void tcp_fast_path_on(struct tcp_sock *tp)
{
	u32 wnd = tp->snd_wnd >> tp->rx_opt.snd_wscale;
	tp->pred_flags = ((u32)(tp->tcp_header_len >> 2) << 28) | ((u32)TCPHDR_ACK << 16) | (wnd & 0xffff);
}

void tcp_fast_path_check(struct tcp_sock *tp)
{
	if (rt_bufq_empty(&tp->ofoq) && tp->rcv_wnd && tcp_space(tp) && !tp->urg_data)
		tcp_fast_path_on(tp);
}

/* ---- delayed ACK machinery (Linux tcp_event_data_recv etc) ---- */
static void tcp_incr_quickack(struct tcp_sock *tp, u32 max_quickacks)
{
	u32 quickacks = tp->rcv_wnd / (2u * RT_MAX((u32)tp->ack.rcv_mss, 1u));

	if (quickacks == 0)
		quickacks = 2;
	quickacks = RT_MIN(quickacks, max_quickacks);
	if (quickacks > tp->ack.quick)
		tp->ack.quick = (u8)quickacks;
}

void tcp_enter_quickack_mode(struct tcp_sock *tp, u32 max_quickacks)
{
	tcp_incr_quickack(tp, max_quickacks);
	tp->pingpong = 0;
	tp->ack.ato = TCP_ATO_MIN;
}

static int tcp_in_quickack_mode(const struct tcp_sock *tp)
{
	return tp->ack.quick && !tp->pingpong;
}

static void tcp_measure_rcv_mss(struct tcp_sock *tp, const struct rt_buf *b)
{
	u32 lss = tp->ack.last_seg_size;
	u32 len = b->len;

	tp->ack.last_seg_size = 0;
	if (len >= tp->ack.rcv_mss) {
		tp->ack.rcv_mss = (u16)RT_MIN(len, (u32)tp->advmss);
	} else {
		len += TCP_SKB_CB(b)->hdr_len;
		if (len >= TCP_MSS_DEFAULT + TCP_HDR_LEN ||
		    (len >= TCP_MIN_MSS + TCP_HDR_LEN &&
		     !(TCP_SKB_CB(b)->tcp_flags & (TCPHDR_PSH | TCPHDR_URG | TCPHDR_FIN | TCPHDR_RST | TCPHDR_SYN)))) {
			len -= tp->tcp_header_len;
			tp->ack.last_seg_size = len;
			if (len == lss) {
				tp->ack.rcv_mss = (u16)len;
				return;
			}
		}
		if (tp->ack.pending & ICSK_ACK_PUSHED)
			tp->ack.pending |= ICSK_ACK_PUSHED2;
		tp->ack.pending |= ICSK_ACK_PUSHED;
	}
}

static void tcp_event_data_recv(struct tcp_sock *tp, const struct rt_buf *b)
{
	u32 now;

	tp->ack.pending |= ICSK_ACK_SCHED;
	tcp_measure_rcv_mss(tp, b);
	tcp_rcv_rtt_measure(tp);
	now = tcp_jiffies();
	if (!tp->ack.ato) {
		tcp_incr_quickack(tp, TCP_MAX_QUICKACKS);
		tp->ack.ato = TCP_ATO_MIN;
	} else {
		u32 m = now - tp->ack.lrcvtime;
		if (m <= TCP_ATO_MIN / 2) {
			tp->ack.ato = (tp->ack.ato >> 1) + TCP_ATO_MIN / 2;
		} else if (m < tp->ack.ato) {
			tp->ack.ato = (tp->ack.ato >> 1) + m;
			if (tp->ack.ato > tp->icsk_rto)
				tp->ack.ato = tp->icsk_rto;
		} else if (m > tp->icsk_rto) {
			tcp_incr_quickack(tp, TCP_MAX_QUICKACKS);
		}
	}
	tp->ack.lrcvtime = now;
	tcp_ecn_check_ce(tp, b);
	if (b->len >= 128)
		tcp_grow_window(tp, b->len);
}

/* Linux __tcp_ack_snd_check(): ACK every net.inet.tcp.delack_segs (2)
 * full segments, in quickack mode, or when requested; otherwise delay. */
static void __tcp_ack_snd_check(struct tcp_sock *tp, int ofo_possible)
{
	if (((tp->rcv_nxt - tp->rcv_wup) > (u32)(sysctl_tcp_delack_segs - 1) * tp->ack.rcv_mss &&
	     __tcp_select_window(tp) >= tp->rcv_wnd) ||
	    tcp_in_quickack_mode(tp) || (tp->ack.pending & ICSK_ACK_NOW) || sysctl_tcp_quickack) {
		tcp_send_ack(tp);
		return;
	}
	if (!ofo_possible || rt_bufq_empty(&tp->ofoq)) {
		tcp_send_delayed_ack(tp);
		return;
	}
	tcp_send_ack(tp);
}

static void tcp_ack_snd_check(struct tcp_sock *tp)
{
	if (!(tp->ack.pending & ICSK_ACK_SCHED))
		return;
	__tcp_ack_snd_check(tp, 1);
}

/* ---- ECN (RFC 3168) ---- */
void tcp_ecn_check_ce(struct tcp_sock *tp, const struct rt_buf *b)
{
	if (!(tp->ecn_flags & TCP_ECN_OK))
		return;
	switch (TCP_SKB_CB(b)->ip_tos & INET_ECN_MASK) {
	case 0:
		if (tp->ecn_flags & TCP_ECN_SEEN)
			tcp_enter_quickack_mode(tp, 2);
		break;
	case INET_ECN_CE:
		rt_tcpstat.TCPECNMarks++;
		tp->delivered_ce++;
		if (!(tp->ecn_flags & TCP_ECN_DEMAND_CWR)) {
			tcp_enter_quickack_mode(tp, 2);
			tp->ecn_flags |= TCP_ECN_DEMAND_CWR;
		}
		tp->ecn_flags |= TCP_ECN_SEEN;
		break;
	default:
		tp->ecn_flags |= TCP_ECN_SEEN;
		break;
	}
}

static void tcp_ecn_accept_cwr(struct tcp_sock *tp, const struct rt_buf *b)
{
	if (TCP_SKB_CB(b)->tcp_flags & TCPHDR_CWR) {
		tp->ecn_flags &= ~TCP_ECN_DEMAND_CWR;
		if (TCP_SKB_CB(b)->seq != TCP_SKB_CB(b)->end_seq)
			tp->ack.pending |= ICSK_ACK_NOW;
	}
}

/* ---- RFC 5961 challenge ACKs ---- */
static u32 challenge_timestamp, challenge_count;

static int tcp_oow_rate_limited(struct tcp_sock *tp, const struct rt_buf *b, u32 *last)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 now = tcp_jiffies();

	if (cb->seq != cb->end_seq && !(cb->tcp_flags & TCPHDR_SYN))
		return 0;
	if (*last) {
		s32 elapsed = (s32)(now - *last);
		if (elapsed >= 0 && (u32)elapsed < TCP_INVALID_RATELIMIT)
			return 1;
	}
	*last = now ? now : 1;
	return 0;
}

void tcp_send_challenge_ack(struct tcp_sock *tp)
{
	u32 now = tcp_jiffies() / 1000u;

	/* per socket limit first, then the global (randomised) one */
	if (tp->last_oow_ack_time && (u32)(tcp_jiffies() - tp->last_oow_ack_time) < TCP_INVALID_RATELIMIT)
		return;
	tp->last_oow_ack_time = tcp_jiffies() ? tcp_jiffies() : 1;
	if (now != challenge_timestamp) {
		challenge_timestamp = now;
		challenge_count = TCP_CHALLENGE_LIMIT / 2 + rt_random32() % TCP_CHALLENGE_LIMIT;
	}
	if (challenge_count > 0) {
		challenge_count--;
		rt_tcpstat.TCPChallengeACK++;
		tcp_send_ack(tp);
	}
}

static void tcp_send_dupack(struct tcp_sock *tp, const struct rt_buf *b)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(b);

	if (cb->end_seq != cb->seq && rt_seq_before(cb->seq, tp->rcv_nxt)) {
		tcp_enter_quickack_mode(tp, TCP_MAX_QUICKACKS);
		if (tcp_is_sack(tp) && sysctl_tcp_dsack) {
			u32 end_seq = cb->end_seq;
			if (rt_seq_after(end_seq, tp->rcv_nxt))
				end_seq = tp->rcv_nxt;
			tcp_dsack_set(tp, cb->seq, end_seq);
			rt_tcpstat.TCPDSACKOldSent++;
		}
	}
	tcp_send_ack(tp);
}

/* Linux tcp_reset(): peer reset the connection */
void tcp_reset(struct tcp_sock *tp)
{
	int err;

	switch (tp->state) {
	case RT_TCPS_SYN_SENT:   err = RT_ECONNREFUSED; break;
	case RT_TCPS_CLOSE_WAIT: err = RT_EPIPE; break;
	case RT_TCPS_CLOSED:     return;
	default:                 err = RT_ECONNRESET; break;
	}
	tcp_write_queue_purge(tp);
	tcp_done(tp);
	if (tp->so) {
		tcp_sock_error(tp, err);
		rt_sock_wakeup(tp->so, RT_SEV_ERROR);
	}
}

static int tcp_reset_check(const struct tcp_sock *tp, const struct rt_buf *b)
{
	return TCP_SKB_CB(b)->seq == tp->rcv_nxt - 1 &&
	       (tp->state == RT_TCPS_CLOSE_WAIT || tp->state == RT_TCPS_LAST_ACK ||
	        tp->state == RT_TCPS_CLOSING);
}

/* Linux tcp_validate_incoming(): PAWS, sequence check, RST and SYN
 * handling per RFC 5961. Returns 1 to continue; otherwise b is freed. */
static int tcp_validate_incoming(struct tcp_sock *tp, struct rt_buf *b, int syn_inerr)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u8 fl = cb->tcp_flags;

	if (tcp_fast_parse_options(tp, b->data, cb->hdr_len) && tp->rx_opt.saw_tstamp &&
	    tcp_paws_discard(tp, b)) {
		if (!(fl & TCPHDR_RST)) {
			if (fl & TCPHDR_SYN)
				goto syn_challenge;
			rt_tcpstat.PAWSEstab++;
			if (!tcp_oow_rate_limited(tp, b, &tp->last_oow_ack_time))
				tcp_send_dupack(tp, b);
			goto discard;
		}
	}
	if (!tcp_sequence(tp, cb->seq, cb->end_seq)) {
		if (!(fl & TCPHDR_RST)) {
			if (fl & TCPHDR_SYN)
				goto syn_challenge;
			if (!tcp_oow_rate_limited(tp, b, &tp->last_oow_ack_time))
				tcp_send_dupack(tp, b);
		} else if (tcp_reset_check(tp, b)) {
			goto reset;
		}
		goto discard;
	}
	if (fl & TCPHDR_RST) {
		/* RFC 5961 3.2: exact match (or rcv_nxt-1 after FIN, or right
		 * edge of our highest SACK block) resets, else challenge. */
		if (cb->seq == tp->rcv_nxt || tcp_reset_check(tp, b))
			goto reset;
		if (tcp_is_sack(tp) && tp->rx_opt.num_sacks > 0) {
			u32 max_sack = tp->selective_acks[0].end_seq;
			int i;
			for (i = 1; i < tp->rx_opt.num_sacks; i++)
				if (rt_seq_after(tp->selective_acks[i].end_seq, max_sack))
					max_sack = tp->selective_acks[i].end_seq;
			if (cb->seq == max_sack)
				goto reset;
		}
		tcp_send_challenge_ack(tp);
		goto discard;
	}
	if (fl & TCPHDR_SYN) {
		/* crossed SYN-ACK in simultaneous open is legitimate */
		if (tp->state == RT_TCPS_SYN_RECEIVED && (fl & TCPHDR_ACK) &&
		    cb->seq + 1 == cb->end_seq && cb->seq + 1 == tp->rcv_nxt &&
		    cb->ack_seq == tp->snd_nxt)
			return 1;
syn_challenge:
		if (syn_inerr)
			rt_tcpstat.InErrs++;
		rt_tcpstat.TCPSYNChallenge++;
		tcp_send_challenge_ack(tp);
		goto discard;
	}
	return 1;
discard:
	rt_buf_free(b);
	return 0;
reset:
	tcp_reset(tp);
	rt_buf_free(b);
	return 0;
}

/* ---- ACK processing (Linux tcp_ack and helpers) ---- */
static int tcp_may_update_window(const struct tcp_sock *tp, u32 ack, u32 ack_seq, u32 nwin)
{
	return rt_seq_after(ack, tp->snd_una) || rt_seq_after(ack_seq, tp->snd_wl1) ||
	       (ack_seq == tp->snd_wl1 && (nwin > tp->snd_wnd || !nwin));
}

static void tcp_snd_una_update(struct tcp_sock *tp, u32 ack)
{
	tp->snd_una = ack;
}

static int tcp_ack_update_window(struct tcp_sock *tp, const struct rt_buf *b, u32 ack, u32 ack_seq)
{
	int flag = 0;
	u32 nwin = TCP_SKB_CB(b)->window;

	if (!(TCP_SKB_CB(b)->tcp_flags & TCPHDR_SYN))
		nwin <<= tp->rx_opt.snd_wscale;
	if (tcp_may_update_window(tp, ack, ack_seq, nwin)) {
		flag |= FLAG_WIN_UPDATE;
		tp->snd_wl1 = ack_seq;
		if (tp->snd_wnd != nwin) {
			if (!nwin)
				tp->dg.zwin_in++;
			tp->snd_wnd = nwin;
			tp->pred_flags = 0;
			tcp_fast_path_check(tp);
			if (nwin > tp->max_window) {
				tp->max_window = nwin;
				tcp_sync_mss(tp, tp->pmtu_cookie);
			}
		}
	}
	tcp_snd_una_update(tp, ack);
	return flag;
}

static void tcp_ack_probe(struct tcp_sock *tp)
{
	struct rt_buf *head = tp->send_head;

	if (!head)
		return;
	if (!rt_seq_after(TCP_SKB_CB(head)->end_seq, tcp_wnd_end(tp))) {
		tp->icsk_backoff = 0;
		tp->icsk_probes_tstamp = 0;
		tcp_clear_xmit_timer(tp, ICSK_TIME_PROBE0);
	} else {
		tcp_reset_xmit_timer(tp, ICSK_TIME_PROBE0, tcp_probe0_when(tp, TCP_RTO_MAX));
	}
}

static int tcp_ack_is_dubious(const struct tcp_sock *tp, int flag)
{
	return !(flag & FLAG_NOT_DUP) || (flag & FLAG_CA_ALERT) || tp->ca_state != TCP_CA_Open;
}

static int tcp_may_raise_cwnd(const struct tcp_sock *tp, int flag)
{
	if (tp->reordering > (u32)sysctl_tcp_reordering)
		return flag & FLAG_FORWARD_PROGRESS;
	return flag & FLAG_DATA_ACKED;
}

static void tcp_cong_control(struct tcp_sock *tp, u32 ack, u32 acked_sacked, int flag)
{
	if (tcp_in_cwnd_reduction(tp))
		tcp_cwnd_reduction(tp, (int)acked_sacked, 0, flag);
	else if (tcp_may_raise_cwnd(tp, flag))
		tp->ca_ops->cong_avoid(tp, ack, acked_sacked);
}

/* Linux tcp_clean_rtx_queue(): free fully acknowledged segments, take RTT
 * samples (Karn), update scoreboard counters. */
static int tcp_clean_rtx_queue(struct tcp_sock *tp, u32 prior_fack, u32 prior_snd_una,
                               struct tcp_sacktag_state *sack)
{
	u64 first_ackt = 0, last_ackt = 0;
	u32 pkts_acked = 0, reord = tp->snd_nxt;
	s32 seq_rtt_us = -1, ca_rtt_us = -1, sack_rtt_us = -1;
	int flag = 0, rtt_update;
	struct rt_buf *b;

	while ((b = tcp_rtx_head(tp)) != NULL) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(b);
		u8 sacked = cb->sacked;

		if (rt_seq_after(cb->end_seq, tp->snd_una))
			break;
		if (sacked & TCPCB_RETRANS) {
			if (sacked & TCPCB_SACKED_RETRANS)
				tp->retrans_out--;
			flag |= FLAG_RETRANS_DATA_ACKED;
		} else if (!(sacked & TCPCB_SACKED_ACKED)) {
			last_ackt = cb->tx_us;
			if (!first_ackt)
				first_ackt = last_ackt;
			if (rt_seq_before(cb->seq, reord))
				reord = cb->seq;
			if (!rt_seq_after(cb->end_seq, tp->high_seq))
				flag |= FLAG_ORIG_SACK_ACKED;
		}
		if (sacked & TCPCB_SACKED_ACKED) {
			tp->sacked_out--;
		} else if (tcp_is_sack(tp)) {
			tp->delivered++;
			tcp_rack_advance(tp, sacked, cb->end_seq, cb->tx_us);
		}
		if (sacked & TCPCB_LOST)
			tp->lost_out--;
		tp->packets_out--;
		pkts_acked++;
		if (!(cb->tcp_flags & TCPHDR_SYN)) {
			flag |= FLAG_DATA_ACKED;
			tp->bytes_acked += b->len;
		} else {
			flag |= FLAG_SYN_ACKED;
			tp->retrans_stamp = 0;
		}
		rt_bufq_unlink(&tp->wq, b);
		tcp_wmem_free_skb(tp, b);
	}
	if (rt_seq_between(tp->snd_up, prior_snd_una, tp->snd_una))
		tp->snd_up = tp->snd_una;
	b = tcp_rtx_head(tp);
	if (b && (TCP_SKB_CB(b)->sacked & TCPCB_SACKED_ACKED))
		flag |= FLAG_SACK_RENEGING;
	if (!b && tp->dg.chrono_type)
		tcp_chrono_stop(tp, TCP_CHRONO_BUSY);

	if (first_ackt && !(flag & FLAG_RETRANS_DATA_ACKED)) {
		seq_rtt_us = (s32)(tcp_clock_us - first_ackt);
		ca_rtt_us = (s32)(tcp_clock_us - last_ackt);
	}
	if (sack->first_sackt) {
		sack_rtt_us = (s32)(tcp_clock_us - sack->first_sackt);
		ca_rtt_us = (s32)(tcp_clock_us - sack->last_sackt);
	}
	rtt_update = tcp_ack_update_rtt(tp, flag, seq_rtt_us, sack_rtt_us, ca_rtt_us);

	if (flag & FLAG_ACKED) {
		flag |= FLAG_SET_XMIT_TIMER;
		if (tcp_is_reno(tp)) {
			tcp_remove_reno_sacks(tp, (int)pkts_acked);
			if (flag & FLAG_RETRANS_DATA_ACKED)
				flag &= ~FLAG_ORIG_SACK_ACKED;
		} else if (rt_seq_before(reord, prior_fack)) {
			tcp_check_sack_reordering(tp, reord, 0);
		}
	} else if (b && rtt_update && sack_rtt_us >= 0 &&
	           (u64)sack_rtt_us > tcp_clock_us - TCP_SKB_CB(b)->tx_us) {
		flag |= FLAG_SET_XMIT_TIMER;
	}
	if (tp->ca_ops->pkts_acked && (pkts_acked || sack->sack_delivered))
		tp->ca_ops->pkts_acked(tp, pkts_acked, ca_rtt_us >= 0 ? ca_rtt_us :
		                       (rtt_update ? (s32)(tp->srtt_us >> 3) : -1));
	if (pkts_acked)
		tcp_check_space(tp);
	return flag;
}

/* Linux tcp_ack(). Returns >0 if the ACK was acceptable, 0 for an old
 * duplicate, <0 for an invalid ACK (beyond snd_nxt or too old). */
static int tcp_ack(struct tcp_sock *tp, struct rt_buf *b, int flag)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_sacktag_state sack;
	u32 prior_snd_una = tp->snd_una;
	u32 ack_seq = cb->seq, ack = cb->ack_seq;
	u32 prior_packets = tp->packets_out, prior_fack, prior_delivered = tp->delivered;
	int rexmit = REXMIT_NONE, num_dupack = 0;

	memset(&sack, 0, sizeof(sack));
	sack.reord = tp->snd_nxt;

	if (rt_seq_before(ack, prior_snd_una)) {
		/* RFC 5961 5.2 blind data injection mitigation */
		if (rt_seq_before(ack, prior_snd_una - tp->max_window)) {
			if (!(flag & FLAG_NO_CHALLENGE_ACK))
				tcp_send_challenge_ack(tp);
			return -1;
		}
		goto old_ack;
	}
	if (rt_seq_after(ack, tp->snd_nxt))
		return -1;
	if (rt_seq_after(ack, prior_snd_una)) {
		flag |= FLAG_SND_UNA_ADVANCED;
		tp->icsk_retransmits = 0;
	}
	prior_fack = tcp_is_sack(tp) ? tcp_highest_sack_seq(tp) : tp->snd_una;
	if (flag & FLAG_UPDATE_TS_RECENT)
		tcp_replace_ts_recent(tp, cb->seq);

	if ((flag & (FLAG_SLOWPATH | FLAG_SND_UNA_ADVANCED)) == FLAG_SND_UNA_ADVANCED) {
		/* window constant, pure forward advance */
		tp->snd_wl1 = ack_seq;
		tcp_snd_una_update(tp, ack);
		flag |= FLAG_WIN_UPDATE;
		rt_tcpstat.TCPHPAcks++;
	} else {
		if (ack_seq != cb->end_seq)
			flag |= FLAG_DATA;
		else
			rt_tcpstat.TCPPureAcks++;
		flag |= tcp_ack_update_window(tp, b, ack, ack_seq);
		if (tp->rx_opt.sack_off)
			flag |= tcp_sacktag_write_queue(tp, b, prior_snd_una, &sack);
		if ((cb->tcp_flags & TCPHDR_ECE) && !(cb->tcp_flags & TCPHDR_SYN) &&
		    (tp->ecn_flags & TCP_ECN_OK))
			flag |= FLAG_ECE;
	}
	tcp_ecn_accept_cwr(tp, b);
	tp->err_soft = 0;
	tp->icsk_probes_out = 0;
	tp->rcv_tstamp = tcp_jiffies();
	if (!prior_packets)
		goto no_queue;

	flag |= tcp_clean_rtx_queue(tp, prior_fack, prior_snd_una, &sack);
	tcp_rack_update_reo_wnd(tp, prior_delivered);
	if (tp->tlp_high_seq)
		tcp_process_tlp_ack(tp, ack, flag);
	if (tcp_ack_is_dubious(tp, flag)) {
		if (!(flag & (FLAG_SND_UNA_ADVANCED | FLAG_NOT_DUP | FLAG_DSACKING_ACK))) {
			num_dupack = 1;
			tp->dg.dupacks_in++;
		}
		tcp_fastretrans_alert(tp, prior_snd_una, num_dupack, &flag, &rexmit);
	}
	if (tp->state == RT_TCPS_CLOSED)
		return 1;
	if (flag & FLAG_SET_XMIT_TIMER)
		tcp_set_xmit_timer(tp);
	tcp_cong_control(tp, ack, tp->delivered - prior_delivered, flag);
	tcp_xmit_recovery(tp, rexmit);
	return 1;

no_queue:
	if (flag & FLAG_DSACKING_ACK)
		tcp_fastretrans_alert(tp, prior_snd_una, num_dupack, &flag, &rexmit);
	tcp_ack_probe(tp);
	if (tp->tlp_high_seq)
		tcp_process_tlp_ack(tp, ack, flag);
	return 1;

old_ack:
	if (tp->rx_opt.sack_off) {
		flag |= tcp_sacktag_write_queue(tp, b, prior_snd_una, &sack);
		tcp_fastretrans_alert(tp, prior_snd_una, num_dupack, &flag, &rexmit);
		tcp_xmit_recovery(tp, rexmit);
	}
	return 0;
}

/* ---- receive queues ---- */

/* Append in-order payload (b->data at payload) to the reader's queue,
 * copying small segments into the tail buffer (Linux tcp_try_coalesce). */
static void tcp_queue_rcv(struct tcp_sock *tp, struct rt_buf *b)
{
	struct rt_buf *tail = tp->rcvq.tail;
	u32 len = b->len;

	tp->bytes_received += len;
	if (!tp->rcvq_bytes && len && !tp->dg.unread_start)
		tp->dg.unread_start = tcp_diag_now();
	tp->rcvq_bytes += len;
	if (tail && len <= rt_buf_tailroom(tail) &&
	    TCP_SKB_CB(tail)->end_seq == TCP_SKB_CB(b)->seq) {
		rt_copy_buf(rt_buf_put(tail, len), b->data, len);
		TCP_SKB_CB(tail)->end_seq += len;
		tp->rcvq.bytes += len;
		rt_buf_free(b);
		return;
	}
	TCP_SKB_CB(b)->end_seq = TCP_SKB_CB(b)->seq + len;
	tp->rmem_alloc += tcp_buf_truesize(b);
	rt_bufq_tail(&tp->rcvq, b);
}

/* Linux tcp_collapse(): rewrite the receive queue into densely filled
 * buffers when small segments waste memory. */
static void tcp_collapse_rcvq(struct tcp_sock *tp)
{
	struct rt_bufq nq;
	struct rt_buf *b, *nb = NULL;

	rt_bufq_init(&nq);
	while ((b = rt_bufq_dequeue(&tp->rcvq)) != NULL) {
		u32 off = 0, seq = TCP_SKB_CB(b)->seq;
		if (rt_seq_before(seq, tp->copied_seq)) {
			off = tp->copied_seq - seq;
			if (off > b->len)
				off = b->len;
			seq += off;
		}
		while (off < b->len) {
			u32 n;
			if (!nb || !rt_buf_tailroom(nb)) {
				nb = rt_buf_alloc(0, 1792 - 64);
				if (!nb) {
					/* out of memory: keep the remainder as is */
					rt_buf_pull(b, off);
					TCP_SKB_CB(b)->seq = seq;
					rt_bufq_tail(&nq, b);
					b = NULL;
					break;
				}
				TCP_SKB_CB(nb)->seq = TCP_SKB_CB(nb)->end_seq = seq;
				rt_bufq_tail(&nq, nb);
			}
			n = RT_MIN(b->len - off, rt_buf_tailroom(nb));
			rt_copy_buf(rt_buf_put(nb, n), b->data + off, n);
			TCP_SKB_CB(nb)->end_seq += n;
			nq.bytes += n;
			off += n;
			seq += n;
		}
		if (b)
			rt_buf_free(b);
		if (!b)
			nb = NULL;
	}
	tp->rcvq = nq;
	tp->rmem_alloc = 0;
	for (b = nq.head; b; b = b->next)
		tp->rmem_alloc += tcp_buf_truesize(b);
	for (b = tp->ofoq.head; b; b = b->next)
		tp->rmem_alloc += tcp_buf_truesize(b);
}

/* Linux tcp_prune_ofo_queue(): drop out-of-order data from the right
 * edge (the receiver reneges; SACK state is reset). */
static int tcp_prune_ofo_queue(struct tcp_sock *tp)
{
	struct rt_buf *b;
	int pruned = 0;

	while ((b = tp->ofoq.tail) != NULL && tp->rmem_alloc > tcp_rmem_limit(tp) / 2) {
		u32 ts = tcp_buf_truesize(b);
		rt_bufq_unlink(&tp->ofoq, b);
		tp->ofo_bytes -= b->len;
		tp->rmem_alloc = tp->rmem_alloc > ts ? tp->rmem_alloc - ts : 0;
		rt_buf_free(b);
		rt_tcpstat.TCPOFODrop++;
		pruned = 1;
	}
	if (pruned) {
		tp->rx_opt.num_sacks = 0;
		tp->pred_flags = 0;
	}
	return pruned;
}

/* Linux tcp_try_rmem_schedule() + tcp_prune_queue() */
static int tcp_try_rmem_schedule(struct tcp_sock *tp, u32 size)
{
	if (tp->rmem_alloc + size <= tcp_rmem_limit(tp))
		return 0;
	tcp_collapse_rcvq(tp);
	if (tp->rmem_alloc + size <= tcp_rmem_limit(tp))
		return 0;
	tcp_prune_ofo_queue(tp);
	if (tp->rmem_alloc + size <= tcp_rmem_limit(tp))
		return 0;
	return -1;
}

static void tcp_data_ready(struct tcp_sock *tp)
{
	if (tp->so && tp->rcvq_bytes >= RT_MAX(tp->so->rcvlowat, 1u))
		rt_sock_wakeup(tp->so, RT_SEV_READ);
}

/* Linux tcp_fin() */
static void tcp_fin(struct tcp_sock *tp)
{
	tp->ack.pending |= ICSK_ACK_SCHED;
	tp->shutdown |= RCV_SHUTDOWN;
	tp->done = 1;
	switch (tp->state) {
	case RT_TCPS_SYN_RECEIVED:
	case RT_TCPS_ESTABLISHED:
		tcp_set_state(tp, RT_TCPS_CLOSE_WAIT);
		tp->pingpong = 1;
		break;
	case RT_TCPS_CLOSE_WAIT:
	case RT_TCPS_CLOSING:
	case RT_TCPS_LAST_ACK:
		break;
	case RT_TCPS_FIN_WAIT_1:
		/* simultaneous close */
		tcp_send_ack(tp);
		tcp_set_state(tp, RT_TCPS_CLOSING);
		break;
	case RT_TCPS_FIN_WAIT_2:
		tcp_send_ack(tp);
		tcp_time_wait(tp, RT_TCPS_TIME_WAIT, 0);
		break;
	default:
		break;
	}
	/* out-of-order data after FIN is dropped */
	if (!rt_bufq_empty(&tp->ofoq)) {
		struct rt_buf *b;
		while ((b = rt_bufq_dequeue(&tp->ofoq)) != NULL) {
			u32 ts = tcp_buf_truesize(b);
			tp->rmem_alloc = tp->rmem_alloc > ts ? tp->rmem_alloc - ts : 0;
			rt_buf_free(b);
		}
		tp->ofo_bytes = 0;
	}
	tp->rx_opt.num_sacks = 0;
	if (tp->so) {
		tp->so->state |= RT_SS_CANTRCVMORE;
		rt_sock_wakeup(tp->so, RT_SEV_READ | RT_SEV_CLOSE);
	}
}

/* Linux tcp_ofo_queue(): move segments that became in-order. */
static void tcp_ofo_queue(struct tcp_sock *tp)
{
	u32 dsack_high = tp->rcv_nxt;
	struct rt_buf *b;

	while ((b = tp->ofoq.head) != NULL) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(b);
		u32 ts, end;
		int fin;

		if (rt_seq_after(cb->seq, tp->rcv_nxt))
			break;
		if (rt_seq_before(cb->seq, dsack_high)) {
			u32 dsack = dsack_high;
			if (rt_seq_before(cb->end_seq, dsack_high))
				dsack_high = cb->end_seq;
			tcp_dsack_extend(tp, cb->seq, dsack);
		}
		rt_bufq_unlink(&tp->ofoq, b);
		tp->ofo_bytes -= b->len;
		ts = tcp_buf_truesize(b);
		tp->rmem_alloc = tp->rmem_alloc > ts ? tp->rmem_alloc - ts : 0;
		if (!rt_seq_after(cb->end_seq, tp->rcv_nxt)) {
			rt_buf_free(b);
			continue;
		}
		if (rt_seq_before(cb->seq, tp->rcv_nxt)) {
			rt_buf_pull(b, tp->rcv_nxt - cb->seq);
			cb->seq = tp->rcv_nxt;
		}
		end = cb->end_seq;
		fin = (cb->tcp_flags & TCPHDR_FIN) != 0;
		if (b->len)
			tcp_queue_rcv(tp, b);
		else
			rt_buf_free(b);
		tp->rcv_nxt = end;
		if (fin) {
			tcp_fin(tp);
			break;
		}
	}
}

/* Linux tcp_data_queue_ofo(): sorted insert with overlap handling, D-SACK
 * for duplicates and SACK block generation. b->data is at the payload. */
static void tcp_data_queue_ofo(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 seq = cb->seq, end_seq = cb->end_seq;
	struct rt_buf *p, *n;

	tcp_ecn_check_ce(tp, b);
	if (tcp_try_rmem_schedule(tp, tcp_buf_truesize(b))) {
		rt_tcpstat.TCPOFODrop++;
		rt_buf_free(b);
		return;
	}
	tp->pred_flags = 0;
	tp->ack.pending |= ICSK_ACK_SCHED;
	rt_tcpstat.TCPOFOQueue++;

	/* find the last segment starting at or before seq (scan from the
	 * tail: new data usually extends the right edge) */
	for (p = tp->ofoq.tail; p && rt_seq_after(TCP_SKB_CB(p)->seq, seq); p = p->prev)
		;
	if (p) {
		struct tcp_skb_cb *pcb = TCP_SKB_CB(p);
		if (rt_seq_before(seq, pcb->end_seq)) {
			if (!rt_seq_after(end_seq, pcb->end_seq)) {
				/* all bits present */
				tcp_dsack_set(tp, seq, end_seq);
				rt_tcpstat.TCPDSACKOfoSent++;
				rt_buf_free(b);
				goto add_sack;
			}
			if (rt_seq_after(seq, pcb->seq)) {
				/* partial overlap: trim our head */
				tcp_dsack_set(tp, seq, pcb->end_seq);
				rt_buf_pull(b, pcb->end_seq - seq);
				cb->seq = pcb->end_seq;
			} else {
				/* same start, we cover p: replace it */
				struct rt_buf *pp = p->prev;
				tcp_dsack_extend(tp, pcb->seq, pcb->end_seq);
				rt_bufq_unlink(&tp->ofoq, p);
				tp->ofo_bytes -= p->len;
				tp->rmem_alloc -= RT_MIN(tp->rmem_alloc, tcp_buf_truesize(p));
				rt_buf_free(p);
				p = pp;
			}
		} else if (pcb->end_seq == seq && !(pcb->tcp_flags & TCPHDR_FIN) &&
		           b->len <= rt_buf_tailroom(p)) {
			/* coalesce into the left neighbour */
			rt_copy_buf(rt_buf_put(p, b->len), b->data, b->len);
			tp->ofo_bytes += b->len;
			tp->ofoq.bytes += b->len;
			pcb->end_seq = end_seq;
			pcb->tcp_flags |= cb->tcp_flags & TCPHDR_FIN;
			rt_buf_free(b);
			b = p;
			goto merge_right;
		}
	}
	rt_bufq_insert_after(&tp->ofoq, p, b);
	tp->ofo_bytes += b->len;
	tp->rmem_alloc += tcp_buf_truesize(b);
merge_right:
	/* remove segments covered by b */
	while ((n = b->next) != NULL) {
		struct tcp_skb_cb *ncb = TCP_SKB_CB(n);
		u32 bend = TCP_SKB_CB(b)->end_seq;
		if (!rt_seq_after(bend, ncb->seq))
			break;
		if (rt_seq_before(bend, ncb->end_seq)) {
			tcp_dsack_extend(tp, ncb->seq, bend);
			/* trim n's head so queue entries do not overlap */
			rt_buf_pull(n, RT_MIN(bend - ncb->seq, n->len));
			tp->ofo_bytes -= RT_MIN(bend - ncb->seq, tp->ofo_bytes);
			ncb->seq = bend;
			break;
		}
		tcp_dsack_extend(tp, ncb->seq, ncb->end_seq);
		rt_bufq_unlink(&tp->ofoq, n);
		tp->ofo_bytes -= n->len;
		tp->rmem_alloc -= RT_MIN(tp->rmem_alloc, tcp_buf_truesize(n));
		rt_buf_free(n);
	}
add_sack:
	if (tcp_is_sack(tp))
		tcp_sack_new_ofo_skb(tp, seq, end_seq);
}

/* in-sequence data (and FIN) for the reader; b->data at the payload */
static void tcp_queue_in_order(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 end;
	int fin;

	if (b->len && tcp_try_rmem_schedule(tp, tcp_buf_truesize(b))) {
		tp->ack.pending |= ICSK_ACK_NOW | ICSK_ACK_SCHED;
		tcp_data_ready(tp);
		if (tp->rcvq_bytes) {
			rt_tcpstat.TCPRcvQDrop++;
			rt_buf_free(b);
			return;
		}
	}
	end = cb->end_seq;
	fin = (cb->tcp_flags & TCPHDR_FIN) != 0;
	if (b->len) {
		tcp_event_data_recv(tp, b);
		tcp_queue_rcv(tp, b);
	} else {
		rt_buf_free(b);
	}
	tp->rcv_nxt = end;
	if (fin)
		tcp_fin(tp);
	if (!rt_bufq_empty(&tp->ofoq)) {
		tcp_ofo_queue(tp);
		/* RFC 5681 4.2: ACK immediately when a hole is filled */
		if (rt_bufq_empty(&tp->ofoq))
			tp->ack.pending |= ICSK_ACK_NOW;
	}
	if (tp->rx_opt.num_sacks)
		tcp_sack_remove(tp);
	tcp_fast_path_check(tp);
	tcp_data_ready(tp);
}

static void tcp_out_of_window(struct tcp_sock *tp, struct rt_buf *b)
{
	tcp_enter_quickack_mode(tp, TCP_MAX_QUICKACKS);
	tp->ack.pending |= ICSK_ACK_SCHED;
	rt_buf_free(b);
}

/* Linux tcp_data_queue(). b->data at the TCP header. Consumes b. */
static void tcp_data_queue(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);

	if (cb->seq == cb->end_seq) {
		rt_buf_free(b);
		return;
	}
	rt_buf_pull(b, cb->hdr_len);
	tcp_ecn_accept_cwr(tp, b);
	tp->rx_opt.dsack = 0;

	if (cb->seq == tp->rcv_nxt) {
		/* accept a bare FIN even into a zero window */
		if (tcp_receive_window(tp) == 0 && (b->len || !(cb->tcp_flags & TCPHDR_FIN))) {
			rt_tcpstat.TCPZeroWindowDrop++;
			tcp_out_of_window(tp, b);
			return;
		}
		tcp_queue_in_order(tp, b);
		return;
	}
	if (!rt_seq_after(cb->end_seq, tp->rcv_nxt)) {
		/* a retransmission of data we have: D-SACK and ACK now */
		tcp_dsack_set(tp, cb->seq, cb->end_seq);
		rt_tcpstat.TCPDSACKOldSent++;
		tcp_out_of_window(tp, b);
		return;
	}
	/* beyond the window, e.g. a zero window probe */
	if (!rt_seq_before(cb->seq, tp->rcv_nxt + tcp_receive_window(tp))) {
		tcp_out_of_window(tp, b);
		return;
	}
	if (rt_seq_before(cb->seq, tp->rcv_nxt)) {
		/* partial: seq < rcv_nxt < end_seq */
		tcp_dsack_set(tp, cb->seq, tp->rcv_nxt);
		if (!tcp_receive_window(tp)) {
			rt_tcpstat.TCPZeroWindowDrop++;
			tcp_out_of_window(tp, b);
			return;
		}
		rt_buf_pull(b, RT_MIN(tp->rcv_nxt - cb->seq, b->len));
		cb->seq = tp->rcv_nxt;
		tcp_queue_in_order(tp, b);
		return;
	}
	tcp_data_queue_ofo(tp, b);
}

/* ---- urgent data (Linux tcp_check_urg / tcp_urg) ---- */
static void tcp_check_urg(struct tcp_sock *tp, const struct rt_buf *b)
{
	u32 ptr = TCP_SKB_CB(b)->urg_ptr;

	if (ptr)
		ptr--;          /* BSD semantics: pointer is one past the byte */
	ptr += TCP_SKB_CB(b)->seq;
	if (rt_seq_after(tp->copied_seq, ptr))
		return;
	if (rt_seq_before(ptr, tp->rcv_nxt))
		return;
	if (tp->urg_data && !rt_seq_after(ptr, tp->urg_seq))
		return;
	if (tp->so)
		rt_sock_wakeup(tp->so, RT_SEV_OOB);
	if (tp->urg_seq == tp->copied_seq && tp->urg_data && !tp->oobinline &&
	    tp->copied_seq != tp->rcv_nxt) {
		struct rt_buf *h = tp->rcvq.head;
		tp->copied_seq++;
		if (tp->rcvq_bytes)
			tp->rcvq_bytes--;
		if (h && !rt_seq_before(tp->copied_seq, TCP_SKB_CB(h)->end_seq)) {
			u32 ts = tcp_buf_truesize(h);
			rt_bufq_unlink(&tp->rcvq, h);
			tp->rmem_alloc = tp->rmem_alloc > ts ? tp->rmem_alloc - ts : 0;
			rt_buf_free(h);
		}
	}
	tp->urg_data = TCP_URG_NOTYET;
	tp->urg_seq = ptr;
	tp->pred_flags = 0;
}

static void tcp_urg(struct tcp_sock *tp, const struct rt_buf *b)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(b);

	if (RT_UNLIKELY(cb->tcp_flags & TCPHDR_URG))
		tcp_check_urg(tp, b);
	if (RT_UNLIKELY(tp->urg_data == TCP_URG_NOTYET)) {
		u32 ptr = tp->urg_seq - cb->seq + cb->hdr_len - ((cb->tcp_flags & TCPHDR_SYN) ? 1 : 0);
		if (ptr < b->len) {
			tp->urg_data = TCP_URG_VALID | b->data[ptr];
			tcp_sock_wake(tp, RT_SEV_OOB | RT_SEV_READ);
		}
	}
}

/* ---- connection establishment ---- */
void tcp_init_transfer(struct tcp_sock *tp)
{
	tcp_init_buffer_space(tp);
	if (tp->srtt_us == 0) {
		/* RFC 6298 5.7: no RTT sample from the handshake (SYN or
		 * SYN-ACK was retransmitted): fall back to a 3 s RTO. */
		if (tp->total_retrans) {
			tp->rttvar_us = TCP_TIMEOUT_FALLBACK * 1000u;
			tp->mdev_us = tp->mdev_max_us = tp->rttvar_us;
			tp->icsk_rto = TCP_TIMEOUT_FALLBACK;
		}
	}
	if (tp->total_retrans > 1 && tp->undo_marker)
		tcp_snd_cwnd_set(tp, 1);
	else
		tcp_snd_cwnd_set(tp, (u32)sysctl_tcp_initcwnd);
	tp->snd_cwnd_stamp = tcp_jiffies();
	tcp_clear_retrans(tp);
	tp->undo_marker = 0;
	tp->ca_state = TCP_CA_Open;
	tcp_init_congestion_control(tp);
	tcp_sndbuf_expand(tp);
	tp->lsndtime = tcp_jiffies();
	if (tp->keepopen)
		tcp_reset_keepalive_timer(tp, (u32)keepalive_time_when(tp));
	if (!tp->rx_opt.snd_wscale)
		tcp_fast_path_on(tp);
	else
		tp->pred_flags = 0;
	tp->ever_connected = 1;
}

/* Linux tcp_finish_connect() */
void tcp_finish_connect(struct tcp_sock *tp)
{
	tcp_set_state(tp, RT_TCPS_ESTABLISHED);
	tp->ack.lrcvtime = tcp_jiffies();
	tcp_init_transfer(tp);
	tp->rcv_tstamp = tcp_jiffies();
	if (tp->so) {
		tp->so->state &= ~RT_SS_ISCONNECTING;
		tp->so->state |= RT_SS_ISCONNECTED;
		rt_sock_wakeup(tp->so, RT_SEV_CONNECT | RT_SEV_WRITE);
	}
}

/* Linux tcp_rcv_synsent_state_process(). Returns 1: send RST, 0: done,
 * -1: continue (send pending data). b is freed unless 1 is returned. */
static int tcp_rcv_synsent_state_process(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_rx_opt opt;
	u8 fl = cb->tcp_flags;

	memset(&opt, 0, sizeof(opt));
	opt.user_mss = tp->rx_opt.user_mss;
	opt.mss_clamp = tp->rx_opt.mss_clamp;
	tcp_parse_options(b->data, cb->hdr_len, &opt, 0);
	if (opt.saw_tstamp && opt.rcv_tsecr)
		opt.rcv_tsecr -= tp->ts_offset;

	if (fl & TCPHDR_ACK) {
		if (!rt_seq_after(cb->ack_seq, tp->snd_una) || rt_seq_after(cb->ack_seq, tp->snd_nxt)) {
			if (tp->icsk_retransmits == 0)
				tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, TCP_TIMEOUT_MIN);
			return 1;
		}
		if (opt.saw_tstamp && opt.rcv_tsecr && tp->retrans_stamp &&
		    !rt_seq_between(opt.rcv_tsecr, tp->retrans_stamp, tcp_jiffies()))
			return 1;
		if (fl & TCPHDR_RST) {
			tcp_reset(tp);
			rt_buf_free(b);
			return 0;
		}
		if (!(fl & TCPHDR_SYN)) {
			rt_buf_free(b);
			return 0;
		}
		/* SYN-ACK: negotiate */
		tp->rx_opt.saw_tstamp = opt.saw_tstamp;
		tp->rx_opt.rcv_tsval = opt.rcv_tsval;
		tp->rx_opt.rcv_tsecr = opt.rcv_tsecr;
		tp->rx_opt.sack_ok = opt.sack_ok && tp->rx_opt.sack_ok;
		tp->rx_opt.mss_clamp = opt.mss_clamp;
		if (tp->rx_opt.wscale_ok && opt.wscale_ok) {
			tp->rx_opt.snd_wscale = opt.snd_wscale;
		} else {
			tp->rx_opt.wscale_ok = 0;
			tp->rx_opt.snd_wscale = tp->rx_opt.rcv_wscale = 0;
			tp->window_clamp = RT_MIN(tp->window_clamp, 65535u);
		}
		/* RFC 3168: ECN-setup SYN-ACK has ECE without CWR */
		if ((tp->ecn_flags & TCP_ECN_OK) &&
		    (!(fl & TCPHDR_ECE) || (fl & TCPHDR_CWR)))
			tp->ecn_flags &= ~TCP_ECN_OK;
		tp->snd_wl1 = cb->seq;
		tp->rx_opt.sack_off = 0;
		tcp_ack(tp, b, FLAG_SLOWPATH);
		if (tp->state == RT_TCPS_CLOSED) {
			rt_buf_free(b);
			return 0;
		}
		tp->rcv_nxt = cb->seq + 1;
		tp->rcv_wup = cb->seq + 1;
		tp->irs = cb->seq;
		tp->snd_wnd = cb->window;   /* SYN windows are never scaled */
		tp->max_window = RT_MAX(tp->max_window, tp->snd_wnd);
		if (tp->rx_opt.saw_tstamp && tp->rx_opt.tstamp_ok) {
			tp->tcp_header_len = TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED;
			tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;
			tcp_store_ts_recent(tp);
		} else {
			tp->rx_opt.tstamp_ok = 0;
			tp->tcp_header_len = TCP_HDR_LEN;
		}
		tcp_sync_mss(tp, tp->pmtu_cookie);
		tcp_initialize_rcv_mss(tp);
		tp->copied_seq = tp->rcv_nxt;
		tcp_finish_connect(tp);
		if (tp->pingpong) {
			tp->ack.pending |= ICSK_ACK_SCHED;
			tcp_enter_quickack_mode(tp, TCP_MAX_QUICKACKS);
			tcp_send_delayed_ack(tp);
			rt_buf_free(b);
			return 0;
		}
		tcp_send_ack(tp);
		rt_buf_free(b);
		return -1;
	}

	if (fl & TCPHDR_RST) {
		rt_buf_free(b);
		return 0;
	}
	tp->rx_opt.rcv_tsval = opt.rcv_tsval;
	if (tp->ts_recent_stamp && opt.saw_tstamp && tcp_paws_reject(tp, 0)) {
		rt_buf_free(b);
		return 0;
	}
	if (fl & TCPHDR_SYN) {
		/* crossed SYNs: simultaneous open (RFC 9293 3.5) */
		tcp_set_state(tp, RT_TCPS_SYN_RECEIVED);
		tp->rx_opt.saw_tstamp = opt.saw_tstamp;
		tp->rx_opt.rcv_tsval = opt.rcv_tsval;
		if (opt.saw_tstamp && tp->rx_opt.tstamp_ok) {
			tcp_store_ts_recent(tp);
			tp->tcp_header_len = TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED;
		} else {
			tp->rx_opt.tstamp_ok = 0;
			tp->tcp_header_len = TCP_HDR_LEN;
		}
		tp->rx_opt.sack_ok = opt.sack_ok && tp->rx_opt.sack_ok;
		tp->rx_opt.mss_clamp = opt.mss_clamp;
		if (tp->rx_opt.wscale_ok && opt.wscale_ok) {
			tp->rx_opt.snd_wscale = opt.snd_wscale;
		} else {
			tp->rx_opt.wscale_ok = 0;
			tp->rx_opt.snd_wscale = tp->rx_opt.rcv_wscale = 0;
			tp->window_clamp = RT_MIN(tp->window_clamp, 65535u);
		}
		if (!((fl & TCPHDR_ECE) && (fl & TCPHDR_CWR)))
			tp->ecn_flags &= ~TCP_ECN_OK;
		tp->rcv_nxt = cb->seq + 1;
		tp->copied_seq = tp->rcv_nxt;
		tp->rcv_wup = cb->seq + 1;
		tp->irs = cb->seq;
		tp->snd_wnd = cb->window;
		tp->snd_wl1 = cb->seq;
		tp->max_window = tp->snd_wnd;
		tcp_sync_mss(tp, tp->pmtu_cookie);
		tcp_initialize_rcv_mss(tp);
		tcp_send_synack(tp);
		rt_buf_free(b);
		return 0;
	}
	rt_buf_free(b);
	return 0;
}

/* Linux tcp_rcv_state_process() for every state but ESTABLISHED and
 * LISTEN. Returns 1 if the caller must answer with RST (b not freed). */
int tcp_rcv_state_process(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u8 fl = cb->tcp_flags;
	int acceptable;

	switch (tp->state) {
	case RT_TCPS_CLOSED:
		return 1;
	case RT_TCPS_SYN_SENT: {
		int q = tcp_rcv_synsent_state_process(tp, b);
		if (q >= 0)
			return q;
		tcp_data_snd_check(tp);
		return 0;
	}
	default:
		break;
	}
	if (!(fl & (TCPHDR_ACK | TCPHDR_RST | TCPHDR_SYN))) {
		rt_buf_free(b);
		return 0;
	}
	if (!tcp_validate_incoming(tp, b, 0))
		return 0;

	/* step 5: check the ACK field */
	if (!(fl & TCPHDR_ACK)) {
		rt_buf_free(b);
		return 0;
	}
	acceptable = tcp_ack(tp, b, FLAG_SLOWPATH | FLAG_UPDATE_TS_RECENT | FLAG_NO_CHALLENGE_ACK);
	if (tp->state == RT_TCPS_CLOSED) {
		rt_buf_free(b);
		return 0;
	}
	if (acceptable <= 0) {
		if (tp->state == RT_TCPS_SYN_RECEIVED)
			return 1;
		if (acceptable < 0) {
			tcp_send_challenge_ack(tp);
			rt_buf_free(b);
			return 0;
		}
	}
	switch (tp->state) {
	case RT_TCPS_SYN_RECEIVED:
		tp->snd_una = cb->ack_seq;
		tp->snd_wnd = (u32)cb->window << tp->rx_opt.snd_wscale;
		tp->snd_wl1 = cb->seq;
		if (tp->rx_opt.tstamp_ok)
			tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;
		tcp_sync_mss(tp, tp->pmtu_cookie);
		tcp_finish_connect(tp);
		tcp_initialize_rcv_mss(tp);
		tcp_fast_path_on(tp);
		break;
	case RT_TCPS_FIN_WAIT_1:
		if (tp->snd_una != tp->write_seq)
			break;
		tcp_set_state(tp, RT_TCPS_FIN_WAIT_2);
		tp->shutdown |= SEND_SHUTDOWN;
		if (!tp->dead) {
			tcp_sock_wake(tp, RT_SEV_WRITE);
			break;
		}
		if (cb->end_seq != cb->seq &&
		    rt_seq_after(cb->end_seq - ((fl & TCPHDR_FIN) ? 1 : 0), tp->rcv_nxt)) {
			/* data after close() */
			rt_tcpstat.TCPAbortOnData++;
			tcp_done(tp);
			return 1;
		}
		if (fl & TCPHDR_FIN) {
			tcp_reset_keepalive_timer(tp, tp->linger2);
		} else {
			tcp_time_wait(tp, RT_TCPS_FIN_WAIT_2, tp->linger2);
			rt_buf_free(b);
			return 0;
		}
		break;
	case RT_TCPS_CLOSING:
		if (tp->snd_una == tp->write_seq) {
			tcp_time_wait(tp, RT_TCPS_TIME_WAIT, 0);
			rt_buf_free(b);
			return 0;
		}
		break;
	case RT_TCPS_LAST_ACK:
		if (tp->snd_una == tp->write_seq) {
			tcp_done(tp);
			rt_buf_free(b);
			return 0;
		}
		break;
	default:
		break;
	}

	/* step 6: urgent */
	tcp_urg(tp, b);

	/* step 7: segment text */
	switch (tp->state) {
	case RT_TCPS_CLOSE_WAIT:
	case RT_TCPS_CLOSING:
	case RT_TCPS_LAST_ACK:
		if (!rt_seq_before(cb->seq, tp->rcv_nxt)) {
			rt_buf_free(b);
			b = NULL;
			break;
		}
		/* fall through */
	case RT_TCPS_FIN_WAIT_1:
	case RT_TCPS_FIN_WAIT_2:
		/* RFC 1122: data after our receive side was shut down by
		 * close() resets the connection. */
		if (tp->dead && cb->end_seq != cb->seq &&
		    rt_seq_after(cb->end_seq - ((fl & TCPHDR_FIN) ? 1 : 0), tp->rcv_nxt)) {
			rt_tcpstat.TCPAbortOnData++;
			tcp_reset(tp);
			return 1;
		}
		/* fall through */
	case RT_TCPS_ESTABLISHED:
		tcp_data_queue(tp, b);
		b = NULL;
		break;
	default:
		break;
	}
	if (b)
		rt_buf_free(b);
	if (tp->state != RT_TCPS_CLOSED) {
		tcp_data_snd_check(tp);
		tcp_ack_snd_check(tp);
	}
	return 0;
}

/* Linux tcp_rcv_established(): header prediction fast path, slow path. */
void tcp_rcv_established(struct tcp_sock *tp, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	const u8 *th = b->data;
	u32 len = b->len;
	u32 hdr = cb->hdr_len;

	tp->rx_opt.saw_tstamp = 0;
	tp->rx_opt.sack_off = 0;
	/* header prediction: flags (minus PSH) and window as expected */
	if ((rt_get_be32(th + 12) & 0xf0ff0000u & ~((u32)TCPHDR_PSH << 16)) ==
	    (tp->pred_flags & 0xf0ff0000u) &&
	    cb->window == (tp->pred_flags & 0xffff) && tp->pred_flags &&
	    cb->seq == tp->rcv_nxt && !rt_seq_after(cb->ack_seq, tp->snd_nxt)) {
		if (hdr == TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED) {
			if (!(th[20] == TCPOPT_NOP && th[21] == TCPOPT_NOP &&
			      th[22] == TCPOPT_TIMESTAMP && th[23] == TCPOLEN_TIMESTAMP))
				goto slow_path;
			tp->rx_opt.saw_tstamp = 1;
			tp->rx_opt.rcv_tsval = rt_get_be32(th + 24);
			tp->rx_opt.rcv_tsecr = rt_get_be32(th + 28);
			if (tp->rx_opt.rcv_tsecr)
				tp->rx_opt.rcv_tsecr -= tp->ts_offset;
			if ((s32)(tp->rx_opt.rcv_tsval - tp->ts_recent) < 0)
				goto slow_path;
		}
		if (len <= hdr) {
			if (len == hdr) {
				/* bulk sender: pure ACK */
				if (hdr == TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED && tp->rcv_nxt == tp->rcv_wup)
					tcp_store_ts_recent(tp);
				tcp_ack(tp, b, 0);
				rt_buf_free(b);
				if (tp->state != RT_TCPS_CLOSED)
					tcp_data_snd_check(tp);
				tp->rcv_rtt_last_tsecr = tp->rx_opt.rcv_tsecr;
				return;
			}
			goto discard;
		} else {
			/* bulk receiver: in-order data */
			u32 dlen = len - hdr;
			if (dlen > tcp_space(tp) || tp->rmem_alloc + tcp_buf_truesize(b) > tcp_rmem_limit(tp))
				goto step5;
			if (hdr == TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED && tp->rcv_nxt == tp->rcv_wup)
				tcp_store_ts_recent(tp);
			tcp_rcv_rtt_measure_ts(tp, b);
			rt_tcpstat.TCPHPHits++;
			rt_buf_pull(b, hdr);
			tcp_event_data_recv(tp, b);
			tp->rcv_nxt = cb->end_seq;
			if (cb->ack_seq != tp->snd_una) {
				tcp_ack(tp, b, FLAG_DATA);
				tcp_queue_rcv(tp, b);
				if (tp->state != RT_TCPS_CLOSED)
					tcp_data_snd_check(tp);
				if (!(tp->ack.pending & ICSK_ACK_SCHED))
					goto no_ack;
			} else {
				tp->snd_wl1 = cb->seq;
				tcp_queue_rcv(tp, b);
			}
			if (tp->state != RT_TCPS_CLOSED)
				__tcp_ack_snd_check(tp, 0);
no_ack:
			tcp_data_ready(tp);
			return;
		}
	}
slow_path:
	if (!(cb->tcp_flags & (TCPHDR_ACK | TCPHDR_RST | TCPHDR_SYN)))
		goto discard;
	if (!tcp_validate_incoming(tp, b, 1))
		return;
step5:
	if (tcp_ack(tp, b, FLAG_SLOWPATH | FLAG_UPDATE_TS_RECENT) < 0)
		goto discard;
	if (tp->state == RT_TCPS_CLOSED)
		goto discard;
	tcp_rcv_rtt_measure_ts(tp, b);
	tcp_urg(tp, b);
	tcp_data_queue(tp, b);
	if (tp->state != RT_TCPS_CLOSED) {
		tcp_data_snd_check(tp);
		tcp_ack_snd_check(tp);
	}
	return;
discard:
	rt_buf_free(b);
}

/* ---- ICMP errors (Linux tcp_v4_err / tcp_v6_err) ---- */
static const u8 icmp_unreach_fatal[16] = {
	0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1
};
static const u8 icmp_unreach_errno[16] = {
	RT_ENETUNREACH, RT_EHOSTUNREACH, RT_ENOPROTOOPT, RT_ECONNREFUSED,
	RT_EMSGSIZE, RT_EOPNOTSUPP, RT_ENETUNREACH, RT_EHOSTDOWN,
	RT_ENETUNREACH, RT_ENETUNREACH, RT_EHOSTUNREACH, RT_ENETUNREACH,
	RT_EHOSTUNREACH, RT_EHOSTUNREACH, RT_EHOSTUNREACH, RT_EHOSTUNREACH
};

/* A smaller path MTU for tp (RFC 1191 / RFC 8201). */
static void tcp_mtu_reduced(struct tcp_sock *tp, u32 mtu)
{
	u32 floor = tp->net_hlen == 40 ? RT_IP6_MIN_MTU : 552;

	if (mtu < (tp->net_hlen == 40 ? RT_IP6_MIN_MTU : 68u) || mtu >= tp->pmtu_cookie)
		return;
	if (mtu < floor)
		mtu = floor > tp->pmtu_cookie ? tp->pmtu_cookie : floor;
	rt_inet_pmtu_update(&tp->daddr, mtu);
	tp->err_soft = RT_EMSGSIZE;
	if (tp->pmtu_cookie > mtu) {
		tcp_sync_mss(tp, mtu);
		tcp_simple_retransmit(tp);
	}
}

/* Common part: the quoted segment was (saddr:sport -> daddr:dport), seq. */
static void tcp_err(const rt_in6_addr *saddr, u16 sport, const rt_in6_addr *daddr, u16 dport,
                    u32 seq, int err, int fatal, int req_abort, u32 mtu)
{
	struct tcp_sock *tp;

	tcp_api_enter();
	tp = tcp_lookup_established(saddr, sport, daddr, dport);
	if (!tp) {
		struct tcp_sock *lp = tcp_lookup_listener(saddr, sport);
		if (lp && req_abort >= 0)
			tcp_req_err(lp, daddr, dport, seq, req_abort);
		goto out;
	}
	/* RFC 5927: the quoted sequence number must be in flight */
	if (!rt_seq_between(seq, tp->snd_una, tp->snd_nxt))
		goto out;
	if (mtu) {
		tcp_mtu_reduced(tp, mtu);
		goto out;
	}
	if (!err)
		goto out;
	if ((tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED) && fatal) {
		/* hard error while connecting: abort */
		tcp_done(tp);
		tcp_sock_error(tp, err);
		goto out;
	}
	/* soft error: reported only if the connection times out */
	tp->err_soft = err;
out:
	tcp_api_exit();
}

void rt_tcp_icmp_error(u8 type, u8 code, u32 info, const struct rt_iphdr *inner, u32 inner_len)
{
	const u8 *th;
	u32 ihl, seq, a;
	rt_in6_addr saddr, daddr;
	u16 sport, dport;
	int err = 0, fatal = 0, req_abort = -1;
	u32 mtu = 0;

	if (!inner || inner_len < 20)
		return;
	ihl = rt_ip_hlen(inner);
	if (ihl < 20 || inner_len < ihl + 8)
		return;
	th = (const u8 *)inner + ihl;
	memcpy(&a, &inner->saddr, 4);
	rt_in6_set_v4mapped(&saddr, a);
	memcpy(&a, &inner->daddr, 4);
	rt_in6_set_v4mapped(&daddr, a);
	memcpy(&sport, th, 2);
	memcpy(&dport, th + 2, 2);
	seq = rt_get_be32(th + 4);

	switch (type) {
	case RT_ICMP_SOURCEQUENCH:
	case RT_ICMP_REDIRECT:
		return;
	case RT_ICMP_PARAMPROB:
		err = RT_ENOPROTOOPT;
		fatal = 1;
		break;
	case RT_ICMP_UNREACH:
		if (code >= 16)
			return;
		req_abort = icmp_unreach_fatal[code];
		if (code == RT_ICMP_UNREACH_NEEDFRAG) {
			if (info == 0)
				return;
			mtu = info;
			req_abort = -1;
			break;
		}
		err = icmp_unreach_errno[code];
		fatal = icmp_unreach_fatal[code];
		break;
	case RT_ICMP_TIMXCEED:
		err = RT_EHOSTUNREACH;
		break;
	default:
		return;
	}
	if (type == RT_ICMP_UNREACH && code == RT_ICMP_UNREACH_NEEDFRAG && mtu < 68)
		return;
	tcp_err(&saddr, sport, &daddr, dport, seq, err, fatal, req_abort, mtu);
}

void rt_tcp_icmp6_error(u8 type, u8 code, u32 info, const struct rt_ip6hdr *inner,
                        const u8 *th, u32 thlen)
{
	u16 sport, dport;
	u32 seq, mtu = 0;
	int err = 0, fatal = 0, req_abort = -1;

	if (thlen < 8)
		return;
	memcpy(&sport, th, 2);
	memcpy(&dport, th + 2, 2);
	seq = rt_get_be32(th + 4);
	switch (type) {
	case RT_ICMP6_PACKET_TOO_BIG:
		if (info < RT_IP6_MIN_MTU)
			info = RT_IP6_MIN_MTU;
		mtu = info;
		break;
	case RT_ICMP6_DST_UNREACH:
		/* Linux icmpv6_err_convert() */
		switch (code) {
		case RT_ICMP6_DST_UNREACH_NOROUTE:
			err = RT_ENETUNREACH;
			break;
		case RT_ICMP6_DST_UNREACH_ADMIN:
			err = RT_EACCES;
			fatal = 1;
			break;
		case RT_ICMP6_DST_UNREACH_NOPORT:
			err = RT_ECONNREFUSED;
			fatal = 1;
			break;
		default:
			err = RT_EHOSTUNREACH;
			break;
		}
		req_abort = fatal;
		break;
	case RT_ICMP6_PARAM_PROB:
		err = RT_ENOPROTOOPT;
		fatal = 1;
		req_abort = 1;
		break;
	case RT_ICMP6_TIME_EXCEEDED:
		err = RT_EHOSTUNREACH;
		break;
	default:
		return;
	}
	tcp_err(&inner->src, sport, &inner->dst, dport, seq, err, fatal, req_abort, mtu);
}

/* ---- demultiplexing (Linux tcp_v4_rcv / tcp_v6_rcv) ----
 * saddr/daddr: IPv4-mapped for IPv4; 'scope' is the arrival interface
 * for link-local peers (replies must leave there). */
static void tcp_rcv(struct rt_buf *b, const rt_in6_addr *saddr, const rt_in6_addr *daddr, u8 tos,
                    u32 scope)
{
	struct tcp_skb_cb *cb;
	struct tcp_sock *tp;
	struct tcp_tw *tw;
	const u8 *th = b->data;
	u32 doff = (u32)(th[12] >> 4) << 2, tw_isn = 0;
	u16 sport, dport;

	memcpy(&sport, th, 2);
	memcpy(&dport, th + 2, 2);
	if (dport == 0)
		goto drop;
	cb = TCP_SKB_CB(b);
	memset(cb, 0, sizeof(*cb));
	cb->seq = rt_get_be32(th + 4);
	cb->ack_seq = rt_get_be32(th + 8);
	cb->tcp_flags = th[13];
	cb->window = rt_get_be16(th + 14);
	cb->urg_ptr = rt_get_be16(th + 18);
	cb->hdr_len = (u8)doff;
	cb->ip_tos = tos;
	cb->end_seq = cb->seq + (b->len - doff) +
	              ((cb->tcp_flags & TCPHDR_SYN) ? 1 : 0) + ((cb->tcp_flags & TCPHDR_FIN) ? 1 : 0);

	tp = tcp_lookup_established(daddr, dport, saddr, sport);
	if (tp)
		goto process;
	tw = tcp_tw_lookup(daddr, dport, saddr, sport);
	if (tw) {
		switch (tcp_timewait_state_process(tw, b, &tw_isn)) {
		case TCP_TW_SYN: {
			struct tcp_sock *lp = tcp_lookup_listener(daddr, dport);
			if (lp) {
				tcp_tw_kill(tw);
				if (tcp_listen_input(lp, b, saddr, daddr, tw_isn)) {
					tcp_send_reset_raw(b, saddr, daddr, scope);
					goto drop;
				}
				goto out;
			}
			tcp_tw_ack(tw);
			goto drop;
		}
		case TCP_TW_ACK:
			tcp_tw_ack(tw);
			goto drop;
		case TCP_TW_RST:
			tcp_send_reset_raw(b, saddr, daddr, scope);
			tcp_tw_kill(tw);
			goto drop;
		default:
			goto drop;
		}
	}
	tp = tcp_lookup_listener(daddr, dport);
	if (tp) {
		if (tcp_listen_input(tp, b, saddr, daddr, 0)) {
			tcp_send_reset_raw(b, saddr, daddr, scope);
			goto drop;
		}
		goto out;
	}
	/* no socket: RFC 9293 3.10.7.1 */
	if (!(cb->tcp_flags & TCPHDR_RST))
		tcp_send_reset_raw(b, saddr, daddr, scope);
	goto drop;

process:
	tp->segs_in++;
	if (b->len > cb->hdr_len)
		tp->data_segs_in++;
	if (tp->state == RT_TCPS_ESTABLISHED) {
		tcp_rcv_established(tp, b);
		goto out;
	}
	if (tcp_rcv_state_process(tp, b)) {
		tcp_send_reset_raw(b, saddr, daddr, scope);
		goto drop;
	}
	goto out;
drop:
	rt_buf_free(b);
out:
	return;
}

void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	const u8 *th;
	u32 saddr, daddr, doff;
	rt_in6_addr sa, da;

	tcp_api_enter();
	rt_tcpstat.InSegs++;
	if (b->len < TCP_HDR_LEN)
		goto bad;
	th = b->data;
	doff = (u32)(th[12] >> 4) << 2;
	if (doff < TCP_HDR_LEN || doff > b->len)
		goto bad;
	memcpy(&saddr, &ip->saddr, 4);
	memcpy(&daddr, &ip->daddr, 4);
	if (b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))
		goto drop;
	if (daddr == RT_INADDR_BROADCAST || (rt_ntohl(daddr) & 0xf0000000u) == 0xe0000000u ||
	    (rt_ntohl(saddr) & 0xf0000000u) == 0xe0000000u || saddr == RT_INADDR_BROADCAST)
		goto drop;
	if (!(b->flags & (RT_BUFF_CSUM_OK | RT_BUFF_LOOP))) {
		u32 sum = rt_csum_pseudo(saddr, daddr, RT_IPPROTO_TCP, (u16)b->len);
		if (rt_csum_fold(rt_csum_partial(th, b->len, sum)) != 0) {
			rt_tcpstat.InCsumErrors++;
			goto bad;
		}
	}
	rt_in6_set_v4mapped(&sa, saddr);
	rt_in6_set_v4mapped(&da, daddr);
	tcp_rcv(b, &sa, &da, ip->tos, 0);
	tcp_api_exit();
	return;
bad:
	rt_tcpstat.InErrs++;
drop:
	rt_buf_free(b);
	tcp_api_exit();
}

void rt_tcp_input6(struct rt_buf *b, const struct rt_ip6hdr *ip6)
{
	const u8 *th;
	u32 doff, scope = 0;

	tcp_api_enter();
	rt_tcpstat.InSegs++;
	if (b->len < TCP_HDR_LEN)
		goto bad;
	th = b->data;
	doff = (u32)(th[12] >> 4) << 2;
	if (doff < TCP_HDR_LEN || doff > b->len)
		goto bad;
	if (rt_in6_is_mcast(&ip6->dst) || (b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)))
		goto drop;
	if (!(b->flags & (RT_BUFF_CSUM_OK | RT_BUFF_LOOP))) {
		u32 sum = rt_csum_pseudo6(&ip6->src, &ip6->dst, RT_IPPROTO_TCP, b->len);
		if (rt_csum_fold(rt_csum_partial(th, b->len, sum)) != 0) {
			rt_tcpstat.InCsumErrors++;
			goto bad;
		}
	}
	if (rt_in6_is_linklocal(&ip6->src) && b->netif && b->netif != rt_loopback_if)
		scope = b->netif->index;
	tcp_rcv(b, &ip6->src, &ip6->dst, (u8)(rt_ntohl(ip6->vtcfl) >> 20), scope);
	tcp_api_exit();
	return;
bad:
	rt_tcpstat.InErrs++;
drop:
	rt_buf_free(b);
	tcp_api_exit();
}
