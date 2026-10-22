/*
 * Riptide - TCP timers: the retransmit timer shared by RTO, zero window
 * probe, tail loss probe and RACK reorder timeout (icsk_pending), the
 * delayed ACK timer and the keepalive / orphan FIN_WAIT_2 timer.
 * Follows Linux net/ipv4/tcp_timer.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

static void tcp_write_timer(void *arg);
static void tcp_delack_timer(void *arg);
static void tcp_keepalive_timer(void *arg);
static void tcp_cork_timer(void *arg);

void tcp_init_xmit_timers(struct tcp_sock *tp)
{
	rt_timer_init(&tp->retrans_timer, tcp_write_timer, tp);
	rt_timer_init(&tp->delack_timer, tcp_delack_timer, tp);
	rt_timer_init(&tp->keepalive_timer, tcp_keepalive_timer, tp);
	rt_timer_init(&tp->cork_timer, tcp_cork_timer, tp);
}

void tcp_clear_xmit_timers(struct tcp_sock *tp)
{
	tp->icsk_pending = 0;
	tp->ack.pending = 0;
	rt_timer_del(&tp->retrans_timer);
	rt_timer_del(&tp->delack_timer);
	rt_timer_del(&tp->cork_timer);
}

/* TCP_CORK and MSG_MORE hold a partial segment back at most
 * TCP_CORK_MAX_MS (Linux tcp(7)): armed when such data stays queued */
void tcp_arm_cork_timer(struct tcp_sock *tp)
{
	if (tp->send_head && !rt_timer_pending(&tp->cork_timer))
		rt_timer_mod(&tp->cork_timer, tcp_jiffies() + TCP_CORK_MAX_MS);
}

static void tcp_cork_timer(void *arg)
{
	struct tcp_sock *tp = arg;

	tcp_api_enter();
	if (tp->send_head && (tp->state == RT_TCPS_ESTABLISHED || tp->state == RT_TCPS_CLOSE_WAIT)) {
		rt_tcpstat.TCPCorkFlush++;
		__tcp_push_pending_frames(tp, tcp_current_mss(tp), TCP_NAGLE_PUSH);
	}
	tcp_api_exit();
}

void tcp_reset_xmit_timer(struct tcp_sock *tp, int what, u32 when)
{
	if (when > TCP_RTO_MAX)
		when = TCP_RTO_MAX;
	tp->icsk_pending = (u8)what;
	tp->icsk_timeout = tcp_jiffies() + when;
	rt_timer_mod(&tp->retrans_timer, tp->icsk_timeout);
}

void tcp_clear_xmit_timer(struct tcp_sock *tp, int what)
{
	tp->icsk_pending = 0;
	rt_timer_del(&tp->retrans_timer);
}

void tcp_reset_keepalive_timer(struct tcp_sock *tp, u32 when)
{
	rt_timer_mod(&tp->keepalive_timer, tcp_jiffies() + when);
}

void tcp_delete_keepalive_timer(struct tcp_sock *tp)
{
	rt_timer_del(&tp->keepalive_timer);
}

static u32 tcp_probe0_base(const struct tcp_sock *tp)
{
	return RT_MAX(tp->icsk_rto, tcp_rto_min());
}

u32 tcp_probe0_when(const struct tcp_sock *tp, u32 max_when)
{
	u32 base = tcp_probe0_base(tp), when = base;
	u32 i;

	for (i = 0; i < tp->icsk_backoff && when < max_when; i++)
		when <<= 1;
	return RT_MIN(when, max_when);
}

/* These run for every ACK. The RTO (<= TCP_RTO_MAX ms) in µs fits 32 bits,
 * so they avoid the compilers' 64-bit multiply/divide helpers, which use
 * the 64-bit mulu.l/divu.l forms that the 68060 traps to 68060.library. */
static s64 tcp_rto_us(const struct tcp_sock *tp)
{
	if (tp->icsk_rto <= 4294967u)
		return (s64)(tp->icsk_rto * 1000u);
	return (s64)tp->icsk_rto * 1000;
}

/* (us + add) / 1000 for 0 < us, add < 1000 */
static u32 tcp_us_div1000(s64 us, u32 add)
{
	if (us <= 0x7fff0000)
		return ((u32)us + add) / 1000u;
	return (u32)((us + add) / 1000);
}

/* time until the RTO of the head segment fires (µs, may be negative) */
s64 tcp_rto_delta_us(const struct tcp_sock *tp)
{
	const struct rt_buf *b = tcp_rtx_head(tp);
	u64 rto_time;

	if (!b)
		return (s64)tcp_rto_us(tp);
	rto_time = TCP_SKB_CB(b)->tx_us + (u64)tcp_rto_us(tp);
	return (s64)(rto_time - tcp_clock_us);
}

/* Linux tcp_rearm_rto() */
void tcp_rearm_rto(struct tcp_sock *tp)
{
	if (!tp->packets_out) {
		tcp_clear_xmit_timer(tp, ICSK_TIME_RETRANS);
	} else {
		u32 rto = tp->icsk_rto;
		if (tp->icsk_pending == ICSK_TIME_REO_TIMEOUT ||
		    tp->icsk_pending == ICSK_TIME_LOSS_PROBE) {
			s64 delta_us = tcp_rto_delta_us(tp);
			rto = tcp_us_div1000(RT_MAX(delta_us, (s64)1), 999);
		}
		tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, rto);
	}
}

/* RFC 8985 7.2 / Linux tcp_schedule_loss_probe() */
int tcp_schedule_loss_probe(struct tcp_sock *tp, int advancing_rto)
{
	u32 timeout;
	s64 rto_delta_us;

	if (!tp->packets_out || !tcp_is_sack(tp) ||
	    (tp->ca_state != TCP_CA_Open && tp->ca_state != TCP_CA_CWR) ||
	    (tp->state != RT_TCPS_ESTABLISHED && tp->state != RT_TCPS_CLOSE_WAIT))
		return 0;
	if (tp->srtt_us) {
		timeout = (tp->srtt_us >> 2) / 1000u;    /* 2 * srtt */
		if (tp->packets_out == 1)
			timeout += tcp_rto_min();            /* allow for a delayed ACK */
		else
			timeout += TCP_TIMEOUT_MIN;
	} else {
		timeout = TCP_TIMEOUT_INIT;
	}
	if (timeout < 1)
		timeout = 1;
	rto_delta_us = advancing_rto ? tcp_rto_us(tp) : tcp_rto_delta_us(tp);
	/* timeout * 1000 > rto_delta_us  <=>  timeout > floor(rto_delta_us / 1000) */
	if (rto_delta_us > 0 && timeout > tcp_us_div1000(rto_delta_us, 0))
		timeout = tcp_us_div1000(rto_delta_us, 999);
	tcp_reset_xmit_timer(tp, ICSK_TIME_LOSS_PROBE, timeout);
	return 1;
}

void tcp_set_xmit_timer(struct tcp_sock *tp)
{
	if (!tcp_schedule_loss_probe(tp, 1))
		tcp_rearm_rto(tp);
}

/* Linux tcp_write_err() */
void tcp_write_err(struct tcp_sock *tp)
{
	int err = tp->err_soft ? tp->err_soft : RT_ETIMEDOUT;

	rt_tcpstat.TCPAbortOnTimeout++;
	tcp_write_queue_purge(tp);
	tcp_done(tp);
	tcp_sock_error(tp, err);
}

/* Linux tcp_model_timeout(): time the given number of exponentially
 * backed off retransmissions take, starting from rto_base. */
static u32 tcp_model_timeout(u32 boundary, u32 rto_base)
{
	u32 linear_backoff_thresh = 0, t = TCP_RTO_MAX / rto_base;

	while (t > 1) {
		t >>= 1;
		linear_backoff_thresh++;
	}
	if (boundary <= linear_backoff_thresh)
		return ((2u << boundary) - 1) * rto_base;
	return ((2u << linear_backoff_thresh) - 1) * rto_base +
	       (boundary - linear_backoff_thresh) * TCP_RTO_MAX;
}

/* Linux retransmits_timed_out(): time based retry limits */
static int retransmits_timed_out(const struct tcp_sock *tp, u32 boundary, u32 timeout)
{
	u32 start_ts;

	if (!tp->icsk_retransmits)
		return 0;
	start_ts = tp->retrans_stamp;
	if (!start_ts)
		return 0;
	if (timeout == 0) {
		u32 rto_base = tcp_rto_min();
		if (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED)
			rto_base = TCP_TIMEOUT_INIT;
		timeout = tcp_model_timeout(boundary, rto_base);
	}
	return (s32)(tcp_jiffies() - start_ts - timeout) >= 0;
}

static int tcp_out_of_resources(struct tcp_sock *tp, int do_reset)
{
	if (tcp_orphan_count > (u32)sysctl_tcp_max_orphans) {
		if (do_reset)
			tcp_send_active_reset(tp);
		tcp_done(tp);
		return 1;
	}
	return 0;
}

static u32 tcp_orphan_retries(const struct tcp_sock *tp, int alive)
{
	u32 retries = TCP_ORPHAN_RETRIES;
	if (tp->err_soft && !alive)
		retries = 0;
	return retries;
}

static void tcp_refresh_route(struct tcp_sock *tp)
{
	struct rt_inet_route rt;

	if (rt_inet_route(&tp->daddr, tp->scope, &rt) == 0 && rt.mtu >= 68 && rt.mtu != tp->pmtu_cookie)
		tcp_sync_mss(tp, rt.mtu);
}

/* How long a connect may take (ms, 0 = only syn_retries): TCP_KEEPINIT,
 * else net.inet.tcp.connect_timeout. TCP_USER_TIMEOUT wins over both. */
static u32 tcp_connect_timeout(const struct tcp_sock *tp)
{
	if (tp->keepinit)
		return tp->keepinit * 1000u;
	return (u32)sysctl_tcp_connect_timeout * 1000u;
}

/* Linux tcp_write_timeout(): 1 if the connection was aborted */
static int tcp_write_timeout(struct tcp_sock *tp)
{
	int expired = 0;
	u32 retry_until, user_timeout = tp->icsk_user_timeout;

	if (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED) {
		retry_until = tp->syn_retries ? tp->syn_retries : (u32)sysctl_tcp_syn_retries;
		expired = tp->icsk_retransmits >= retry_until;
		if (!user_timeout)
			user_timeout = tcp_connect_timeout(tp);
		if (expired || retransmits_timed_out(tp, retry_until, user_timeout)) {
			if (tp->state == RT_TCPS_SYN_SENT)
				rt_tcpstat.TCPConnectTimeouts++;
			tcp_write_err(tp);
			return 1;
		}
		return 0;
	} else {
		/* black hole / route change: re-resolve the route and its
		 * path MTU after retries1 timeouts (Linux dst_negative_advice) */
		if (retransmits_timed_out(tp, (u32)sysctl_tcp_retries1, 0))
			tcp_refresh_route(tp);
		retry_until = (u32)sysctl_tcp_retries2;
		if (tp->dead) {
			int alive = tp->icsk_rto < TCP_RTO_MAX;
			retry_until = tcp_orphan_retries(tp, alive);
			if (tcp_out_of_resources(tp, alive || !retransmits_timed_out(tp, retry_until, 0)))
				return 1;
		}
	}
	if (!expired)
		expired = retransmits_timed_out(tp, retry_until, user_timeout);
	if (expired) {
		tcp_write_err(tp);
		return 1;
	}
	return 0;
}

static u32 tcp_clamp_rto_to_user_timeout(const struct tcp_sock *tp)
{
	u32 ut = tp->icsk_user_timeout;
	s32 remaining;

	if (!ut && (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED))
		ut = tcp_connect_timeout(tp);
	if (!ut || !tp->retrans_stamp)
		return tp->icsk_rto;
	remaining = (s32)(tp->retrans_stamp + ut - tcp_jiffies());
	if (remaining <= 0)
		return 1;
	return RT_MIN(tp->icsk_rto, (u32)remaining);
}

/* Linux tcp_retransmit_timer(): RTO expiry */
static void tcp_retransmit_timer(struct tcp_sock *tp)
{
	struct rt_buf *b;

	if (!tp->packets_out)
		return;
	b = tcp_rtx_head(tp);
	if (!b)
		return;
	if (!tp->snd_wnd && !tp->dead &&
	    tp->state != RT_TCPS_SYN_SENT && tp->state != RT_TCPS_SYN_RECEIVED) {
		/* Receiver shrank the window: retransmissions are zero window
		 * probes; do not time out while the peer answers them. */
		u32 rtx_delta = tcp_jiffies() - (tp->retrans_stamp ? tp->retrans_stamp : TCP_SKB_CB(b)->tx_ms);
		u32 limit = tp->icsk_user_timeout ? tp->icsk_user_timeout :
		            tcp_model_timeout((u32)sysctl_tcp_retries2, tcp_rto_min()) + TCP_RTO_MAX;
		if (rtx_delta > limit) {
			tcp_write_err(tp);
			return;
		}
		tcp_enter_loss(tp);
		tcp_retransmit_skb(tp, b);
		goto out_reset_timer;
	}
	rt_tcpstat.TCPTimeouts++;
	tp->dg.rto_timeouts++;
	TCP_DBG(tp, "RTO after %lu ms: retransmit %lu, cwnd %lu, %lu in flight", (unsigned long)tp->icsk_rto,
	        (unsigned long)TCP_SKB_CB(b)->seq, (unsigned long)tp->snd_cwnd, (unsigned long)tp->packets_out);
	if (tcp_write_timeout(tp))
		return;
	if (tp->state == RT_TCPS_SYN_SENT && tp->fo_len != TCP_FASTOPEN_NONE) {
		/* The SYN with the TFO option was lost: further SYNs go
		 * without it (a middlebox may drop it); the SYN-ACK of a
		 * plain one then marks the destination (tcp_fastopen.c). */
		if (tp->fo_flags & TCP_FO_OPT_SENT)
			tp->fo_flags |= TCP_FO_SYN_LOST;
		tp->fo_len = TCP_FASTOPEN_NONE;
	}
	tcp_enter_loss(tp);
	tp->icsk_retransmits++;
	b = tcp_rtx_head(tp);
	if (b && tcp_retransmit_skb(tp, b) == -RT_ENOBUFS) {
		tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, TCP_RESOURCE_PROBE_INTERVAL);
		return;
	}
out_reset_timer:
	tp->icsk_backoff++;
	if (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED) {
		/* net.inet.tcp.syn_backoff: 1 doubles (RFC 6298), 0 adds the
		 * initial RTO; either way at most syn_rto_max_ms */
		u32 rto = sysctl_tcp_syn_backoff ? tp->icsk_rto << 1
		                                 : tp->icsk_rto + (u32)sysctl_tcp_syn_rto_initial_ms;
		tp->icsk_rto = RT_MIN(rto, (u32)sysctl_tcp_syn_rto_max_ms);
	} else {
		tp->icsk_rto = RT_MIN(tp->icsk_rto << 1, TCP_RTO_MAX);
	}
	tcp_reset_xmit_timer(tp, ICSK_TIME_RETRANS, tcp_clamp_rto_to_user_timeout(tp));
}

/* Linux tcp_probe_timer(): zero window probe expiry */
static void tcp_probe_timer(struct tcp_sock *tp)
{
	u32 max_probes;

	if (tp->packets_out || !tp->send_head) {
		tp->icsk_probes_out = 0;
		tp->icsk_probes_tstamp = 0;
		return;
	}
	if (!tp->icsk_probes_tstamp) {
		tp->icsk_probes_tstamp = tcp_jiffies() ? tcp_jiffies() : 1;
	} else if (tp->icsk_user_timeout &&
	           (s32)(tcp_jiffies() - tp->icsk_probes_tstamp) >= (s32)tp->icsk_user_timeout) {
		tcp_write_err(tp);
		return;
	}
	max_probes = (u32)sysctl_tcp_retries2;
	if (tp->dead) {
		int alive = tcp_probe0_when(tp, TCP_RTO_MAX) < TCP_RTO_MAX;
		max_probes = tcp_orphan_retries(tp, alive);
		if (!alive && tp->icsk_backoff >= max_probes) {
			tcp_write_err(tp);
			return;
		}
		if (tcp_out_of_resources(tp, 1))
			return;
	}
	if (tp->icsk_probes_out >= max_probes)
		tcp_write_err(tp);
	else
		tcp_send_probe0(tp);
}

static void tcp_write_timer(void *arg)
{
	struct tcp_sock *tp = arg;
	int event;

	tcp_api_enter();
	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN || !tp->icsk_pending)
		goto out;
	if ((s32)(tp->icsk_timeout - tcp_jiffies()) > 0) {
		rt_timer_mod(&tp->retrans_timer, tp->icsk_timeout);
		goto out;
	}
	event = tp->icsk_pending;
	switch (event) {
	case ICSK_TIME_REO_TIMEOUT:
		tcp_rack_reo_timeout(tp);
		break;
	case ICSK_TIME_LOSS_PROBE:
		tcp_send_loss_probe(tp);
		break;
	case ICSK_TIME_RETRANS:
		tp->icsk_pending = 0;
		tcp_retransmit_timer(tp);
		break;
	case ICSK_TIME_PROBE0:
		tp->icsk_pending = 0;
		tcp_probe_timer(tp);
		break;
	default:
		break;
	}
out:
	tcp_api_exit();
}

/* Linux tcp_delack_timer_handler() */
static void tcp_delack_timer(void *arg)
{
	struct tcp_sock *tp = arg;

	tcp_api_enter();
	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN ||
	    !(tp->ack.pending & ICSK_ACK_TIMER))
		goto out;
	if ((s32)(tp->ack.timeout - tcp_jiffies()) > 0) {
		rt_timer_mod(&tp->delack_timer, tp->ack.timeout);
		goto out;
	}
	tp->ack.pending &= ~ICSK_ACK_TIMER;
	if (tp->ack.pending & ICSK_ACK_SCHED) {
		if (!tp->pingpong) {
			tp->ack.ato = RT_MIN(tp->ack.ato << 1, tp->icsk_rto);
		} else {
			tp->pingpong = 0;
			tp->ack.ato = TCP_ATO_MIN;
		}
		rt_tcpstat.DelayedACKs++;
		tp->dg.delayed_acks_out++;
		RT_DBG(RT_DBG_TIMER, "delayed ACK timer, ato %lu ms", (unsigned long)tp->ack.ato);
		tcp_send_ack(tp);
	}
out:
	tcp_api_exit();
}

/* Linux tcp_keepalive_timer(): keepalive probes and orphan FIN_WAIT_2 */
static void tcp_keepalive_timer(void *arg)
{
	struct tcp_sock *tp = arg;
	u32 elapsed, when;

	tcp_api_enter();
	if (tp->state == RT_TCPS_FIN_WAIT_2 && tp->dead) {
		/* FIN arrived late or never: give up (tcp_fin_timeout) */
		tcp_send_active_reset(tp);
		tcp_done(tp);
		goto out;
	}
	if (!tp->keepopen || tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_SYN_SENT ||
	    tp->state == RT_TCPS_LISTEN)
		goto out;
	when = (u32)keepalive_time_when(tp);
	if (tp->packets_out || tp->send_head)
		goto resched;
	if ((s32)(tcp_jiffies() - tp->rcv_tstamp) < (s32)(tcp_jiffies() - tp->ack.lrcvtime))
		elapsed = tcp_jiffies() - tp->rcv_tstamp;
	else
		elapsed = tcp_jiffies() - tp->ack.lrcvtime;
	if (elapsed >= when) {
		u32 ut = tp->icsk_user_timeout;
		if ((ut && elapsed >= ut && tp->icsk_probes_out > 0) ||
		    (!ut && tp->icsk_probes_out >= keepalive_probes(tp))) {
			tcp_send_active_reset(tp);
			tcp_write_err(tp);
			goto out;
		}
		if (tcp_write_wakeup(tp) <= 0) {
			rt_tcpstat.TCPKeepAlive++;
			tp->icsk_probes_out++;
			when = keepalive_intvl_when(tp);
		} else {
			when = TCP_RESOURCE_PROBE_INTERVAL;
		}
	} else {
		when = when - elapsed;
	}
resched:
	tcp_reset_keepalive_timer(tp, when);
out:
	tcp_api_exit();
}
