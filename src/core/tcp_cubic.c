/*
 * Riptide - CUBIC congestion control (RFC 9438) following Linux
 * net/ipv4/tcp_cubic.c: integer arithmetic only (BICTCP_HZ time scaling,
 * integer cube root), TCP friendliness, fast convergence. The slow start
 * exit is HyStart++ (RFC 9406, net.inet.tcp.hystart=2, the default) or
 * Linux's classic delay/ACK-train HyStart (1).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

#define BICTCP_BETA_SCALE 1024
#define BICTCP_HZ         10          /* time in 2^-10 s units */
#define CUBIC_BETA        717         /* 0.7 * 1024 (RFC 9438 beta_cubic) */
#define CUBIC_BIC_SCALE   41          /* C = 0.4 -> 41/102.4 */
#define CUBIC_FAST_CONVERGENCE 1
#define CUBIC_TCP_FRIENDLINESS 1

#define HYSTART_ACK_TRAIN 0x1
#define HYSTART_DELAY     0x2
#define HYSTART_DETECT    (HYSTART_ACK_TRAIN | HYSTART_DELAY)
#define HYSTART_LOW_WINDOW 16
#define HYSTART_ACK_DELTA_US 2000
#define HYSTART_MIN_SAMPLES 8
#define HYSTART_DELAY_MIN 4000u
#define HYSTART_DELAY_MAX 16000u

/* RFC 9406 4.3 constants. Riptide does not pace TCP, so L = 8. */
#define HSPP_N_RTT_SAMPLE  8
#define HSPP_CSS_DIV_SHIFT 2          /* CSS_GROWTH_DIVISOR = 4 */
#define HSPP_CSS_ROUNDS    5
#define HSPP_L             8

/* 0 off, 1 classic Linux HyStart, 2 HyStart++ */
s32 sysctl_tcp_hystart = 2;

/* 8 * (1024 + 717) / 3 / (1024 - 717): TCP friendly window scaling */
#define CUBIC_BETA_SCALE  ((8u * (BICTCP_BETA_SCALE + CUBIC_BETA)) / 3u / (BICTCP_BETA_SCALE - CUBIC_BETA))
#define CUBE_RTT_SCALE    (CUBIC_BIC_SCALE * 10u)
/* 2^(10 + 3*BICTCP_HZ) / cube_rtt_scale */
#define CUBE_FACTOR       ((((u64)1) << (10 + 3 * BICTCP_HZ)) / CUBE_RTT_SCALE)

struct bictcp {
	u32 cnt;               /* increase cwnd by 1 after cnt ACKs */
	u32 last_max_cwnd;     /* W_max */
	u32 last_cwnd;
	u32 last_time;
	u32 bic_origin_point;
	u32 bic_K;             /* time to origin point, 2^-10 s */
	u32 delay_min;         /* min delay (µs) */
	u32 epoch_start;       /* ms */
	u32 ack_cnt;
	u32 tcp_cwnd;          /* estimated Reno cwnd */
	u8  sample_cnt;
	u8  found;             /* slow start exit done (either HyStart) */
	u8  css;               /* HyStart++: in Conservative Slow Start */
	u8  css_rounds;        /* HyStart++: CSS rounds completed */
	u32 round_start;       /* µs */
	u32 end_seq;
	u32 last_ack;          /* µs */
	u32 curr_rtt;          /* µs: this round's min RTT */
	u32 last_round_rtt;    /* µs: HyStart++ lastRoundMinRTT */
	u32 css_baseline;      /* µs: HyStart++ cssBaselineMinRtt */
	u32 css_acc;           /* HyStart++: CSS growth remainder */
};

typedef char bictcp_fits_ca_priv[sizeof(struct bictcp) <= TCP_CA_PRIV_U64 * 8 ? 1 : -1];

static u32 bictcp_clock_us(void) { return (u32)tcp_clock_us; }

/* 64 * cbrt(i), i = 0..63: seed for the Newton iteration below */
static const u8 cubic_v[64] = {
	0, 64, 81, 92, 102, 109, 116, 122, 128, 133, 138, 142, 147, 150, 154, 158,
	161, 165, 168, 171, 174, 177, 179, 182, 185, 187, 190, 192, 194, 197, 199, 201,
	203, 205, 207, 209, 211, 213, 215, 217, 219, 221, 222, 224, 226, 228, 229, 231,
	233, 234, 236, 237, 239, 240, 242, 243, 245, 246, 248, 249, 251, 252, 253, 255
};

/* floor(cbrt(a)): table lookup on the top bits, two Newton-Raphson steps
 * x' = (2x + a/x^2) / 3, then an exact correction. */
u32 tcp_cubic_root(u64 a)
{
	u32 k = 0, x, i;
	u64 top;

	if (a < 64) {
		x = 0;
		while ((x + 1) * (x + 1) * (x + 1) <= (u32)a)
			x++;
		return x;
	}
	if (a > ((u64)1 << 62))
		a = (u64)1 << 62;
	top = a;
	while (top >= 64) {
		top >>= 3;
		k++;
	}
	x = (u32)((((u64)cubic_v[(u32)top] + 1) << k) >> 6);
	if (x < 1)
		x = 1;
	for (i = 0; i < 2; i++)
		x = (u32)((2 * (u64)x + a / ((u64)x * x)) / 3);
	if (x < 1)
		x = 1;
	while ((u64)x * x * x > a)
		x--;
	while ((u64)(x + 1) * (x + 1) * (x + 1) <= a)
		x++;
	return x;
}

static void bictcp_reset(struct bictcp *ca)
{
	memset(ca, 0, sizeof(*ca));
}

static void bictcp_hystart_reset(struct tcp_sock *tp)
{
	struct bictcp *ca = tcp_ca(tp);

	ca->round_start = ca->last_ack = bictcp_clock_us();
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0u;
	ca->sample_cnt = 0;
}

static void cubictcp_init(struct tcp_sock *tp)
{
	struct bictcp *ca = tcp_ca(tp);

	bictcp_reset(ca);
	bictcp_hystart_reset(tp);
	ca->last_round_rtt = ca->css_baseline = ~0u;
}

/* HyStart++ runs only in the initial slow start (RFC 9406 4.3); later
 * slow starts exit at the ssthresh they inherited. */
static int hspp_active(const struct tcp_sock *tp, const struct bictcp *ca)
{
	return sysctl_tcp_hystart == 2 && !ca->found &&
	       tp->snd_ssthresh == TCP_INFINITE_SSTHRESH;
}

static void cubictcp_cwnd_event(struct tcp_sock *tp, int event)
{
	struct bictcp *ca = tcp_ca(tp);

	if (event == CA_EVENT_TX_START) {
		/* after an idle period shift the epoch so the cubic curve is
		 * not "advanced" by application-limited time */
		u32 now = tcp_jiffies();
		s32 delta = (s32)(now - tp->lsndtime);
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += (u32)delta;
			if (rt_seq_after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
	}
}

/* Linux bictcp_update(): compute ca->cnt from the cubic function */
static void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt, now = tcp_jiffies();
	u64 offs, t;

	ca->ack_cnt += acked;
	if (ca->last_cwnd == cwnd && (s32)(now - ca->last_time) <= 1000 / 32)
		return;
	if (ca->epoch_start && now == ca->last_time)
		goto tcp_friendliness;
	ca->last_cwnd = cwnd;
	ca->last_time = now;
	if (ca->epoch_start == 0) {
		ca->epoch_start = now ? now : 1;
		ca->ack_cnt = acked;
		ca->tcp_cwnd = cwnd;
		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* K = cbrt((W_max - cwnd) / C) in 2^-10 s */
			ca->bic_K = tcp_cubic_root(CUBE_FACTOR * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}
	/* t = elapsed time since epoch start + min RTT, in 2^-10 s */
	t = (u64)(u32)(now - ca->epoch_start) + ca->delay_min / 1000u;
	t = (t << BICTCP_HZ) / 1000u;
	if (t < ca->bic_K)
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;
	if (offs > ((u64)1 << 18))
		offs = (u64)1 << 18;
	/* C/rtt * (t-K)^3 */
	delta = (u32)((CUBE_RTT_SCALE * offs * offs * offs) >> (10 + 3 * BICTCP_HZ));
	if (t < ca->bic_K)
		bic_target = ca->bic_origin_point > delta ? ca->bic_origin_point - delta : 1;
	else
		bic_target = ca->bic_origin_point + delta;
	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = 100 * cwnd;
	/* conservative initial growth when W_max is unknown */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;

tcp_friendliness:
	if (CUBIC_TCP_FRIENDLINESS) {
		u32 scale = CUBIC_BETA_SCALE;
		delta = (cwnd * scale) >> 3;
		if (delta == 0)
			delta = 1;
		while (ca->ack_cnt > delta) {
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}
		if (ca->tcp_cwnd > cwnd) {
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}
	/* at most 1.5x per RTT */
	ca->cnt = RT_MAX(ca->cnt, 2u);
}

static void cubictcp_cong_avoid(struct tcp_sock *tp, u32 ack, u32 acked)
{
	struct bictcp *ca = tcp_ca(tp);

	if (!tcp_is_cwnd_limited(tp))
		return;
	if (tcp_in_slow_start(tp)) {
		if (hspp_active(tp, ca)) {
			/* cwnd += min(N, L), a quarter of that in CSS */
			if (acked > HSPP_L)
				acked = HSPP_L;
			if (ca->css) {
				ca->css_acc += acked;
				acked = ca->css_acc >> HSPP_CSS_DIV_SHIFT;
				ca->css_acc &= (1u << HSPP_CSS_DIV_SHIFT) - 1;
			}
			tp->snd_cwnd = RT_MIN(tp->snd_cwnd + acked, tp->snd_cwnd_clamp);
			return;
		}
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	bictcp_update(ca, tp->snd_cwnd, acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 cubictcp_recalc_ssthresh(struct tcp_sock *tp)
{
	struct bictcp *ca = tcp_ca(tp);

	ca->epoch_start = 0;
	/* loss or ECN in slow start or CSS ends HyStart++ (RFC 9406 4.2) */
	ca->found = 1;
	ca->css = 0;
	/* fast convergence: release bandwidth for new flows */
	if (tp->snd_cwnd < ca->last_max_cwnd && CUBIC_FAST_CONVERGENCE)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + CUBIC_BETA)) /
		                    (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;
	return RT_MAX((tp->snd_cwnd * CUBIC_BETA) / BICTCP_BETA_SCALE, 2u);
}

static void cubictcp_state(struct tcp_sock *tp, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		cubictcp_init(tp);
		/* ssthresh is finite now: HyStart++ stays off, as RFC 9406
		 * asks; classic HyStart runs again as in Linux */
	}
}

/* Linux hystart_update(): leave slow start early on ACK-train length
 * reaching min RTT / 2, or on a delay increase over the round minimum. */
static void hystart_update(struct tcp_sock *tp, u32 delay)
{
	struct bictcp *ca = tcp_ca(tp);
	u32 threshold;

	if (rt_seq_after(tp->snd_una, ca->end_seq))
		bictcp_hystart_reset(tp);
	if (HYSTART_DETECT & HYSTART_ACK_TRAIN) {
		u32 now = bictcp_clock_us();
		if ((s32)(now - ca->last_ack) <= HYSTART_ACK_DELTA_US) {
			ca->last_ack = now;
			threshold = ca->delay_min >> 1;     /* no pacing */
			if ((s32)(now - ca->round_start) > (s32)threshold) {
				ca->found = 1;
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}
	if (HYSTART_DETECT & HYSTART_DELAY) {
		if (ca->curr_rtt > delay)
			ca->curr_rtt = delay;
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			ca->sample_cnt++;
		} else {
			u32 thresh = ca->delay_min >> 3;
			if (thresh < HYSTART_DELAY_MIN)
				thresh = HYSTART_DELAY_MIN;
			if (thresh > HYSTART_DELAY_MAX)
				thresh = HYSTART_DELAY_MAX;
			if (ca->curr_rtt > ca->delay_min + thresh) {
				ca->found = 1;
				tp->snd_ssthresh = tp->snd_cwnd;
			}
		}
	}
}

/* RFC 9406 4.2, per RTT sample in the initial slow start or CSS. Rounds
 * end when snd_una passes end_seq (windowEnd). */
static void hspp_update(struct tcp_sock *tp, u32 delay)
{
	struct bictcp *ca = tcp_ca(tp);
	u32 thresh;

	if (rt_seq_after(tp->snd_una, ca->end_seq)) {
		if (ca->css && ++ca->css_rounds >= HSPP_CSS_ROUNDS) {
			/* RTT stayed up through CSS_ROUNDS: congestion avoidance */
			ca->css = 0;
			ca->found = 1;
			tp->snd_ssthresh = tp->snd_cwnd;
			return;
		}
		ca->last_round_rtt = ca->curr_rtt;
		ca->curr_rtt = ~0u;
		ca->sample_cnt = 0;
		ca->end_seq = tp->snd_nxt;
	}
	if (ca->curr_rtt > delay)
		ca->curr_rtt = delay;
	if (ca->sample_cnt < 255)
		ca->sample_cnt++;
	if (ca->sample_cnt < HSPP_N_RTT_SAMPLE)
		return;
	if (ca->css) {
		if (ca->curr_rtt < ca->css_baseline) {
			/* the RTT rise was not ours: resume slow start */
			ca->css = 0;
			ca->css_baseline = ~0u;
		}
		return;
	}
	if (ca->last_round_rtt == ~0u)
		return;
	thresh = ca->last_round_rtt >> 3;
	if (thresh < HYSTART_DELAY_MIN)
		thresh = HYSTART_DELAY_MIN;
	if (thresh > HYSTART_DELAY_MAX)
		thresh = HYSTART_DELAY_MAX;
	if (ca->curr_rtt >= ca->last_round_rtt + thresh) {
		ca->css = 1;
		ca->css_rounds = 0;
		ca->css_acc = 0;
		ca->css_baseline = ca->curr_rtt;
	}
}

static void cubictcp_acked(struct tcp_sock *tp, u32 num_acked, s32 rtt_us)
{
	struct bictcp *ca = tcp_ca(tp);
	u32 delay;

	if (rtt_us < 0)
		return;
	/* discard samples right after a congestion event */
	if (ca->epoch_start && (s32)(tcp_jiffies() - ca->epoch_start) < 1000)
		return;
	delay = rtt_us ? (u32)rtt_us : 1;
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;
	if (!tcp_in_slow_start(tp))
		return;
	if (hspp_active(tp, ca))
		hspp_update(tp, delay);
	else if (sysctl_tcp_hystart == 1 && !ca->found && tp->snd_cwnd >= HYSTART_LOW_WINDOW)
		hystart_update(tp, delay);
}

const struct tcp_congestion_ops tcp_cubic = {
	"cubic",
	cubictcp_init,
	cubictcp_recalc_ssthresh,
	cubictcp_cong_avoid,
	cubictcp_state,
	cubictcp_cwnd_event,
	cubictcp_acked,
	tcp_reno_undo_cwnd
};
