/*
 * Riptide - pluggable TCP congestion control and NewReno.
 * Follows Linux net/ipv4/tcp_cong.c (cwnd counted in packets, ABC-like
 * accounting of acknowledged packets through snd_cwnd_cnt).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

static const struct tcp_congestion_ops *const tcp_ca_list[] = {
	&tcp_cubic,
	&tcp_reno,
};

const struct tcp_congestion_ops *tcp_ca_find(const char *name)
{
	u32 i;

	if (!name)
		return NULL;
	for (i = 0; i < RT_ARRAY_SIZE(tcp_ca_list); i++)
		if (rt_strcasecmp(tcp_ca_list[i]->name, name) == 0)
			return tcp_ca_list[i];
	if (rt_strcasecmp(name, "newreno") == 0)
		return &tcp_reno;
	return NULL;
}

void tcp_init_congestion_control(struct tcp_sock *tp)
{
	memset(tp->ca_priv, 0, sizeof(tp->ca_priv));
	if (tp->ca_ops->init)
		tp->ca_ops->init(tp);
}

/* TCP_CONGESTION: switching on a live connection reinitialises the
 * algorithm state (Linux tcp_reinit_congestion_control). */
int tcp_ca_select(struct tcp_sock *tp, const char *name)
{
	const struct tcp_congestion_ops *ca = tcp_ca_find(name);

	if (!ca)
		return -RT_ENOENT;
	tp->ca_ops = ca;
	if (tp->state != RT_TCPS_CLOSED && tp->state != RT_TCPS_LISTEN &&
	    tp->state != RT_TCPS_SYN_SENT)
		tcp_init_congestion_control(tp);
	return 0;
}

void tcp_ca_event(struct tcp_sock *tp, int ev)
{
	if (tp->ca_ops->cwnd_event)
		tp->ca_ops->cwnd_event(tp, ev);
}

void tcp_set_ca_state(struct tcp_sock *tp, u8 st)
{
	if (tp->ca_ops->set_state)
		tp->ca_ops->set_state(tp, st);
	tp->ca_state = st;
}

/* Linux tcp_slow_start(): grow by the number of packets ACKed, capped at
 * ssthresh; returns the leftover for congestion avoidance. */
u32 tcp_slow_start(struct tcp_sock *tp, u32 acked)
{
	u32 cwnd = RT_MIN(tp->snd_cwnd + acked, tp->snd_ssthresh);

	acked -= cwnd - tp->snd_cwnd;
	tp->snd_cwnd = RT_MIN(cwnd, tp->snd_cwnd_clamp);
	return acked;
}

/* Linux tcp_cong_avoid_ai(): cwnd += 1 per w packets ACKed */
void tcp_cong_avoid_ai(struct tcp_sock *tp, u32 w, u32 acked)
{
	if (w == 0)
		w = 1;
	if (tp->snd_cwnd_cnt >= w) {
		tp->snd_cwnd_cnt = 0;
		tp->snd_cwnd++;
	}
	tp->snd_cwnd_cnt += acked;
	if (tp->snd_cwnd_cnt >= w) {
		u32 delta = tp->snd_cwnd_cnt / w;
		tp->snd_cwnd_cnt -= delta * w;
		tp->snd_cwnd += delta;
	}
	tp->snd_cwnd = RT_MIN(tp->snd_cwnd, tp->snd_cwnd_clamp);
}

/* Linux tcp_is_cwnd_limited(): RFC 7661 - only grow a used window */
int tcp_is_cwnd_limited(const struct tcp_sock *tp)
{
	if (tp->is_cwnd_limited)
		return 1;
	if (tcp_in_slow_start(tp))
		return tp->snd_cwnd < 2 * tp->max_packets_out;
	return 0;
}

void tcp_reno_cong_avoid(struct tcp_sock *tp, u32 ack, u32 acked)
{
	if (!tcp_is_cwnd_limited(tp))
		return;
	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	tcp_cong_avoid_ai(tp, tp->snd_cwnd, acked);
}

u32 tcp_reno_ssthresh(struct tcp_sock *tp)
{
	return RT_MAX(tp->snd_cwnd >> 1, 2u);
}

u32 tcp_reno_undo_cwnd(struct tcp_sock *tp)
{
	return RT_MAX(tp->snd_cwnd, tp->prior_cwnd);
}

const struct tcp_congestion_ops tcp_reno = {
	"reno",
	NULL,
	tcp_reno_ssthresh,
	tcp_reno_cong_avoid,
	NULL,
	NULL,
	NULL,
	tcp_reno_undo_cwnd
};
