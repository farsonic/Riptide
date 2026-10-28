/*
 * Riptide - CUBIC slow start exit on a bare tcp_sock, driven round by
 * round with synthetic RTTs (one ACK per two segments, as with delayed
 * ACKs):
 *   - HyStart++ (RFC 9406): a steady RTT keeps slow start; an RTT rise
 *     enters Conservative Slow Start (a quarter of the growth); a lower
 *     RTT in CSS resumes slow start; an RTT that stays up for CSS_ROUNDS
 *     rounds ends in congestion avoidance (ssthresh = cwnd); stretch
 *     ACKs grow cwnd by at most L = 8; a loss ends HyStart++ for good;
 *   - classic HyStart (net.inet.tcp.hystart=1) exits at once;
 *   - net.inet.tcp.hystart=0: slow start continues.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_tcp_common.h"
#include "../src/core/tcp.h"

#define MSS 1448u

static struct tcp_sock *new_sock(u32 cwnd)
{
	struct tcp_sock *tp = calloc(1, sizeof(*tp));

	tp->snd_cwnd = cwnd;
	tp->snd_cwnd_clamp = 1000000;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	tp->is_cwnd_limited = 1;
	tp->snd_una = 1000;
	tp->snd_nxt = tp->snd_una + cwnd * MSS;
	tp->ca_ops = &tcp_cubic;
	tcp_clock_us = 1000000;
	tcp_cubic.init(tp);
	return tp;
}

/* one round trip: everything in flight is ACKed two segments at a time
 * with the given RTT; the sender refills the window after every ACK.
 * Returns the growth in segments. */
static u32 round_trip(struct tcp_sock *tp, u32 rtt_us)
{
	u32 cwnd0 = tp->snd_cwnd, end = tp->snd_nxt;

	while (rt_seq_before(tp->snd_una, end)) {
		tp->snd_una += 2 * MSS;
		tcp_clock_us += 100;
		tcp_cubic.pkts_acked(tp, 2, (s32)rtt_us);
		tcp_cubic.cong_avoid(tp, tp->snd_una, 2);
		tp->snd_nxt = tp->snd_una + tp->snd_cwnd * MSS;
	}
	return tp->snd_cwnd - cwnd0;
}

static int in_ca(const struct tcp_sock *tp)
{
	return tp->snd_ssthresh != TCP_INFINITE_SSTHRESH;
}

static void test_hystart_pp(void)
{
	struct tcp_sock *tp;
	u32 i, g, c0;

	sysctl_tcp_hystart = 2;

	/* steady 20 ms: slow start doubles every round */
	tp = new_sock(10);
	for (i = 0; i < 4; i++) {
		c0 = tp->snd_cwnd;
		g = round_trip(tp, 20000);
		CHECKV(g >= c0 - 1, "round %u: %u -> +%u", i, c0, g);
	}
	CHECK(!in_ca(tp));

	/* RTT up by 10 ms (> max(4 ms, 20/8 ms)): CSS, growth a quarter */
	round_trip(tp, 30000);
	c0 = tp->snd_cwnd;
	g = round_trip(tp, 30000);
	printf("CSS round: cwnd %u +%u\n", c0, g);
	CHECKV(g <= c0 / 4 + 2 && g >= c0 / 4 - 2, "CSS growth %u of %u", g, c0);
	CHECK(!in_ca(tp));

	/* RTT back to 20 ms: the exit was spurious, slow start again */
	round_trip(tp, 20000);
	c0 = tp->snd_cwnd;
	g = round_trip(tp, 20000);
	printf("resumed: cwnd %u +%u\n", c0, g);
	CHECKV(g >= c0 - 1, "resumed growth %u of %u", g, c0);
	CHECK(!in_ca(tp));

	/* RTT stays up: CSS for CSS_ROUNDS (5) rounds, then CA */
	for (i = 0; i < 4; i++) {
		round_trip(tp, 40000);
		CHECKV(!in_ca(tp), "CA after %u CSS rounds", i + 1);
	}
	round_trip(tp, 40000);
	round_trip(tp, 40000);
	CHECK(in_ca(tp));
	printf("congestion avoidance at cwnd %u, ssthresh %u\n", tp->snd_cwnd, tp->snd_ssthresh);
	CHECK(tp->snd_ssthresh <= tp->snd_cwnd);
	free(tp);

	/* stretch ACK of 20 segments: cwnd grows by L = 8 */
	tp = new_sock(40);
	tp->snd_una += 20 * MSS;
	tcp_cubic.pkts_acked(tp, 20, 20000);
	tcp_cubic.cong_avoid(tp, tp->snd_una, 20);
	CHECKV(tp->snd_cwnd == 48, "cwnd %u", tp->snd_cwnd);
	free(tp);

	/* loss in CSS: CUBIC's ssthresh, and no HyStart++ in the next
	 * slow start */
	tp = new_sock(10);
	for (i = 0; i < 4; i++)
		round_trip(tp, 20000);
	round_trip(tp, 30000);
	c0 = tp->snd_cwnd;
	tp->snd_ssthresh = tcp_cubic.ssthresh(tp);
	CHECKV(tp->snd_ssthresh == c0 * 717 / 1024, "ssthresh %u cwnd %u", tp->snd_ssthresh, c0);
	tp->snd_cwnd = 2;
	tp->snd_nxt = tp->snd_una + 2 * MSS;
	round_trip(tp, 20000);
	for (i = 0; i < 3; i++) {
		c0 = tp->snd_cwnd;
		g = round_trip(tp, 40000);
		CHECKV(g >= c0 - 1, "post-loss slow start growth %u of %u", g, c0);
	}
	CHECK(tp->snd_cwnd < tp->snd_ssthresh);
	free(tp);
}

static void test_classic_and_off(void)
{
	struct tcp_sock *tp;
	u32 i;

	/* classic: the first round with 8 samples over min + thresh exits */
	sysctl_tcp_hystart = 1;
	tp = new_sock(10);
	for (i = 0; i < 3; i++)
		round_trip(tp, 20000);
	CHECK(!in_ca(tp));
	round_trip(tp, 30000);
	CHECK(in_ca(tp));
	printf("classic HyStart exit at cwnd %u\n", tp->snd_ssthresh);
	free(tp);

	sysctl_tcp_hystart = 0;
	tp = new_sock(10);
	for (i = 0; i < 3; i++)
		round_trip(tp, 20000);
	for (i = 0; i < 6; i++)
		round_trip(tp, 40000);
	CHECK(!in_ca(tp));
	CHECK(tp->snd_cwnd > 1000);
	free(tp);
	sysctl_tcp_hystart = 2;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_hystart_pp();
	test_classic_and_off();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
