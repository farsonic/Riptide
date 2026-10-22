/*
 * Riptide - background transfers: LEDBAT++ congestion control (sender),
 * receiver-side LEDBAT (rLEDBAT) and the policy that marks connections as
 * background ones.
 *
 * LEDBAT (RFC 6817) keeps the queueing delay it adds near a target and
 * backs off as soon as other traffic builds a queue, so a background
 * transfer yields to interactive ones. LEDBAT++ (draft-irtf-iccrg-ledbat-
 * plus-plus) measures RTTs instead of one-way delays (no clock sync
 * needed), slows its slow start (gain 1/min(16, 2 * target / base RTT)),
 * decreases multiplicatively (W -= W * (delay / target - 1), at most W/2,
 * once per RTT) and every now and then drops to two segments for two RTTs
 * so that the base delay is re-measured with an empty queue (the
 * "periodic slowdown": the next one 9 times the last one's duration
 * later).
 *
 * The Amiga mostly downloads, so the receiver side matters most:
 * rLEDBAT (RFC 9840) runs the same controller on the receiver and limits
 * the window it advertises. RTT samples come from TCP timestamps (the
 * first data segment that echoes a new TSval of ours, the samples of
 * Linux tcp_rcv_rtt_measure_ts()); without timestamps rLEDBAT stays off.
 * The window is never shrunk (RFC 9293): a smaller limit is reached by
 * not opening the window as data arrives.
 *
 * Which connections: TCP_CONGESTION "ledbat" (sender) and TCP_BACKGROUND
 * (Riptide: 1 receive, 2 send, 3 both) on a socket, or, for the programs
 * that never set an option, rules: net.inet.tcp.bg_programs (names of the
 * programs that create the sockets), bg_ports (remote ports) and
 * bg_lports (local ports), the configuration keyword "background" sets
 * them. net.inet.tcp.bg_mode says what the rules turn on.
 *
 * The controller works in fixed point (segments * 256), no FPU.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"

s32 sysctl_tcp_ledbat_target = 60;     /* ms (LEDBAT++: 60) */
s32 sysctl_tcp_ledbat_slowdown = 1;
s32 sysctl_tcp_bg_mode = TCP_BG_RECV | TCP_BG_SEND;
static char bg_programs[128];
static char bg_ports[96];
static char bg_lports[96];

/* the controller lives in the sender's ca_priv */
typedef char ledbat_fits_ca_priv[sizeof(struct ledbat) <= TCP_CA_PRIV_U64 * sizeof(u64) ? 1 : -1];

/* ====================================================================
 * The controller
 * ==================================================================== */
#define LEDBAT_NONE 0xffffffffu

static u32 lb_min(const u32 *v, u32 n)
{
	u32 m = LEDBAT_NONE, i;

	for (i = 0; i < n; i++)
		if (v[i] < m)
			m = v[i];
	return m;
}

u32 ledbat_base(const struct ledbat *lb)
{
	return lb_min(lb->base, LEDBAT_BASE_N);
}

u32 ledbat_current(const struct ledbat *lb)
{
	return lb->cur_n ? lb_min(lb->cur, lb->cur_n) : LEDBAT_NONE;
}

/* one round: the current RTT in ms (at least 1) */
static u32 lb_rtt_ms(const struct ledbat *lb)
{
	u32 c = ledbat_current(lb);

	if (c == LEDBAT_NONE)
		return 100;
	c /= 1000u;
	return c ? c : 1;
}

void ledbat_init(struct ledbat *lb, u32 wnd, u32 now)
{
	u32 i;

	memset(lb, 0, sizeof(*lb));
	lb->wnd = RT_MAX(wnd, 2u) * LEDBAT_ONE;
	lb->ssthresh = LEDBAT_NONE;
	for (i = 0; i < LEDBAT_BASE_N; i++)
		lb->base[i] = LEDBAT_NONE;
	lb->base_ms = now;
	lb->round_end = now;
	lb->phase = LEDBAT_SS;
}

void ledbat_rtt(struct ledbat *lb, u32 rtt_us, u32 now)
{
	u32 b, c;

	if (!rtt_us)
		rtt_us = 1;
	/* base history: the minimum of each minute (RFC 6817 BASE_HISTORY) */
	if (now - lb->base_ms >= 60000u) {
		lb->base_i = (u8)((lb->base_i + 1) % LEDBAT_BASE_N);
		lb->base[lb->base_i] = rtt_us;
		lb->base_ms = now;
	} else if (rtt_us < lb->base[lb->base_i]) {
		lb->base[lb->base_i] = rtt_us;
	}
	/* current delay: the minimum of the last samples (CURRENT_FILTER) */
	lb->cur[lb->cur_i] = rtt_us;
	lb->cur_i = (u8)((lb->cur_i + 1) % LEDBAT_CUR_N);
	if (lb->cur_n < LEDBAT_CUR_N)
		lb->cur_n++;
	b = ledbat_base(lb);
	c = ledbat_current(lb);
	lb->qdelay = c > b ? c - b : 0;
}

/* leave a slow start: the initial one plans the first slowdown two RTTs
 * later, the one after a slowdown the next at 9 times its duration */
static void lb_end_ss(struct ledbat *lb, u32 now)
{
	lb->ssthresh = lb->wnd;
	if (lb->phase == LEDBAT_SS)
		lb->next_slow = now + 2u * lb_rtt_ms(lb);
	else if (lb->phase == LEDBAT_RAMP)
		lb->next_slow = now + RT_MAX(9u * (now - lb->slow_start), 2u * lb_rtt_ms(lb));
	if (!lb->next_slow)
		lb->next_slow = 1;
	lb->phase = LEDBAT_CA;
}

/* acked: segments * 256 acknowledged (sender) or received (receiver);
 * may_grow: the window is in use (RFC 7661) */
void ledbat_acked(struct ledbat *lb, u32 acked, u32 now, int may_grow)
{
	u32 target = (u32)sysctl_tcp_ledbat_target * 1000u;
	u32 base = ledbat_base(lb), gain_inv;

	if (!lb->cur_n)
		return;                     /* no delay measured yet */
	if ((s32)(now - lb->round_end) >= 0) {
		lb->round_end = now + lb_rtt_ms(lb);
		lb->dec_done = 0;
	}
	if (lb->phase == LEDBAT_SLOW) {
		if ((s32)(now - lb->slow_end) < 0) {
			lb->wnd = 2u * LEDBAT_ONE;
			return;
		}
		lb->phase = LEDBAT_RAMP;
		lb->ssthresh = lb->saved;
	}
	if (lb->phase == LEDBAT_CA && lb->next_slow && sysctl_tcp_ledbat_slowdown &&
	    (s32)(now - lb->next_slow) >= 0) {
		lb->saved = lb->wnd;
		lb->slow_start = now;
		lb->slow_end = now + 2u * lb_rtt_ms(lb);
		lb->wnd = 2u * LEDBAT_ONE;
		lb->phase = LEDBAT_SLOW;
		lb->next_slow = 0;
		return;
	}
	/* GAIN = 1 / min(16, ceil(2 * TARGET / base)) */
	gain_inv = base && base != LEDBAT_NONE ? (2u * target + base - 1) / base : 16u;
	if (gain_inv < 1)
		gain_inv = 1;
	if (gain_inv > 16)
		gain_inv = 16;
	if (lb->qdelay < target) {
		if (!may_grow)
			return;
		if (lb->phase == LEDBAT_SS || lb->phase == LEDBAT_RAMP) {
			lb->wnd += acked / gain_inv;
			if (lb->phase == LEDBAT_RAMP && lb->wnd >= lb->saved) {
				lb->wnd = lb->saved;
				lb_end_ss(lb, now);
			} else if (lb->wnd >= lb->ssthresh || lb->qdelay > target / 4u * 3u) {
				lb_end_ss(lb, now);
			}
		} else {
			/* W += GAIN * off_target * acked / W; the remainder is
			 * carried so large windows still grow */
			u64 num = (u64)acked * (target - lb->qdelay) * LEDBAT_ONE + lb->acc;
			u64 den = (u64)target * gain_inv * lb->wnd;
			lb->wnd += (u32)(num / den);
			lb->acc = (u32)(num % den);
		}
	} else {
		if (lb->phase == LEDBAT_SS || lb->phase == LEDBAT_RAMP)
			lb_end_ss(lb, now);
		if (!lb->dec_done) {
			/* W += GAIN - W * (delay / target - 1), at least W / 2 */
			u64 dec = (u64)lb->wnd * (lb->qdelay - target) / target;
			if (dec > lb->wnd / 2u)
				dec = lb->wnd / 2u;
			lb->wnd = lb->wnd - (u32)dec + LEDBAT_ONE / gain_inv;
			lb->acc = 0;
			lb->dec_done = 1;
		}
	}
	if (lb->wnd < 2u * LEDBAT_ONE)
		lb->wnd = 2u * LEDBAT_ONE;
}

/* a loss: halve, once per round */
void ledbat_loss(struct ledbat *lb, u32 now)
{
	if (lb->dec_done || lb->phase == LEDBAT_SLOW)
		return;
	lb->wnd = RT_MAX(lb->wnd / 2u, 2u * LEDBAT_ONE);
	if (lb->phase == LEDBAT_SS || lb->phase == LEDBAT_RAMP)
		lb_end_ss(lb, now);
	lb->ssthresh = lb->wnd;
	lb->acc = 0;
	lb->dec_done = 1;
	if (!lb->round_end || (s32)(now - lb->round_end) >= 0)
		lb->round_end = now + lb_rtt_ms(lb);
}

/* ====================================================================
 * The sender: tcp_congestion_ops "ledbat"
 * ==================================================================== */
static struct ledbat *lb_of(struct tcp_sock *tp)
{
	return (struct ledbat *)tcp_ca(tp);
}

static void lbca_init(struct tcp_sock *tp)
{
	ledbat_init(lb_of(tp), tp->snd_cwnd, tcp_jiffies());
}

/* the stack sets cwnd itself in loss recovery and after an RTO: follow;
 * a cwnd below ssthresh is a slow start back to it */
static void lbca_sync(struct tcp_sock *tp, struct ledbat *lb, u32 now)
{
	u32 w = lb->wnd / LEDBAT_ONE;

	if (tp->snd_cwnd == w)
		return;
	if (tp->snd_cwnd < w && tp->snd_cwnd < tp->snd_ssthresh && lb->phase == LEDBAT_CA) {
		lb->phase = LEDBAT_RAMP;
		lb->saved = RT_MIN(tp->snd_ssthresh, 65535u) * LEDBAT_ONE;
		lb->slow_start = now;
	}
	lb->wnd = tp->snd_cwnd * LEDBAT_ONE;
	lb->acc = 0;
}

static void lbca_cong_avoid(struct tcp_sock *tp, u32 ack, u32 acked)
{
	struct ledbat *lb = lb_of(tp);
	u32 now = tcp_jiffies(), w;

	lbca_sync(tp, lb, now);
	ledbat_acked(lb, acked * LEDBAT_ONE, now, tcp_is_cwnd_limited(tp));
	w = RT_MIN(lb->wnd / LEDBAT_ONE, tp->snd_cwnd_clamp);
	tcp_snd_cwnd_set(tp, RT_MAX(w, 2u));
	lb->wnd = tp->snd_cwnd * LEDBAT_ONE + lb->wnd % LEDBAT_ONE;
	if (lb->phase == LEDBAT_SS)
		tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	else if (lb->phase == LEDBAT_RAMP)
		tp->snd_ssthresh = RT_MAX(lb->saved / LEDBAT_ONE, 2u);
	else
		tp->snd_ssthresh = tp->snd_cwnd;
}

static u32 lbca_ssthresh(struct tcp_sock *tp)
{
	struct ledbat *lb = lb_of(tp);

	lbca_sync(tp, lb, tcp_jiffies());
	ledbat_loss(lb, tcp_jiffies());
	return RT_MAX(lb->wnd / LEDBAT_ONE, 2u);
}

static void lbca_acked(struct tcp_sock *tp, u32 num_acked, s32 rtt_us)
{
	if (rtt_us >= 0)
		ledbat_rtt(lb_of(tp), (u32)rtt_us, tcp_jiffies());
}

const struct tcp_congestion_ops tcp_ledbat = {
	"ledbat",
	lbca_init,
	lbca_ssthresh,
	lbca_cong_avoid,
	NULL,
	NULL,
	lbca_acked,
	tcp_reno_undo_cwnd
};

/* ====================================================================
 * The receiver: rLEDBAT
 * ==================================================================== */
static u32 rl_mss(const struct tcp_sock *tp)
{
	return RT_MAX((u32)tp->advmss, (u32)TCP_MSS_DEFAULT);
}

void tcp_rledbat_start(struct tcp_sock *tp)
{
	if (tp->rl)
		return;
	if (!tp->rx_opt.tstamp_ok) {
		TCP_DBG(tp, "rLEDBAT: the peer sends no timestamps, the window is not limited");
		return;
	}
	tp->rl = rt_zalloc(sizeof(*tp->rl));
	if (!tp->rl)
		return;
	/* start from the initial window, not from what the SYN offered */
	ledbat_init(&tp->rl->lb, (u32)sysctl_tcp_initcwnd, tcp_jiffies());
	TCP_DBG(tp, "rLEDBAT on: target %ld ms", (long)sysctl_tcp_ledbat_target);
}

void tcp_rledbat_stop(struct tcp_sock *tp)
{
	rt_free(tp->rl);
	tp->rl = NULL;
}

void tcp_rledbat_rtt(struct tcp_sock *tp, u32 rtt_us)
{
	if (tp->rl)
		ledbat_rtt(&tp->rl->lb, rtt_us, tcp_jiffies());
}

void tcp_rledbat_data(struct tcp_sock *tp, u32 len)
{
	struct ledbat *lb;
	u32 max;

	if (!tp->rl)
		return;
	lb = &tp->rl->lb;
	ledbat_acked(lb, (u32)(((u64)len * LEDBAT_ONE) / rl_mss(tp)), tcp_jiffies(), 1);
	/* no use growing past what the buffer can take */
	max = RT_MAX(tp->rcvbuf / rl_mss(tp), 2u) * LEDBAT_ONE;
	if (lb->wnd > max)
		lb->wnd = max;
}

void tcp_rledbat_loss(struct tcp_sock *tp)
{
	if (tp->rl)
		ledbat_loss(&tp->rl->lb, tcp_jiffies());
}

/* the receive window rLEDBAT allows, bytes */
u32 tcp_rledbat_window(const struct tcp_sock *tp)
{
	return (u32)(((u64)tp->rl->lb.wnd * rl_mss(tp)) / LEDBAT_ONE);
}

/* ====================================================================
 * Which connections are background ones
 * ==================================================================== */
static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

/* next comma separated item of *s into buf; 0 at the end */
static int next_item(const char **s, char *buf, u32 len)
{
	const char *p = *s;
	u32 n = 0;

	while (*p == ',' || is_space(*p))
		p++;
	if (!*p)
		return 0;
	while (*p && *p != ',') {
		if (n + 1 < len)
			buf[n++] = *p;
		p++;
	}
	while (n && is_space(buf[n - 1]))
		n--;
	buf[n] = 0;
	*s = p;
	return 1;
}

static int parse_port(const char *s, const char **end, u32 *v)
{
	u32 n = 0;
	int digits = 0;

	while (*s >= '0' && *s <= '9') {
		n = n * 10u + (u32)(*s - '0');
		if (n > 65535u)
			return -1;
		s++;
		digits++;
	}
	*end = s;
	*v = n;
	return digits && n ? 0 : -1;
}

/* "20,21,6881-6889": 1 if port is in it, 0 if not, -1 for a bad list */
static int port_list_match(const char *list, u32 port)
{
	char item[16];
	const char *s = list, *e;
	u32 lo, hi;
	int hit = 0;

	while (next_item(&s, item, sizeof(item))) {
		if (parse_port(item, &e, &lo) < 0)
			return -1;
		hi = lo;
		if (*e == '-' && parse_port(e + 1, &e, &hi) < 0)
			return -1;
		if (*e || hi < lo)
			return -1;
		if (port >= lo && port <= hi)
			hit = 1;
	}
	return hit;
}

/* program names, case-insensitive; "name*" matches a prefix */
static int prog_list_match(const char *list, const char *name)
{
	char item[32];
	const char *s = list;

	while (next_item(&s, item, sizeof(item))) {
		u32 n = (u32)strlen(item);
		if (n && item[n - 1] == '*') {
			u32 i;
			for (i = 0; i + 1 < n; i++)
				if ((name[i] | 0x20) != (item[i] | 0x20) || !name[i])
					break;
			if (i + 1 == n)
				return 1;
		} else if (!rt_strcasecmp(item, name)) {
			return 1;
		}
	}
	return 0;
}

/* receive side when the connection can receive */
static int tcp_can_receive(const struct tcp_sock *tp)
{
	return tp->state == RT_TCPS_ESTABLISHED || tp->state == RT_TCPS_FIN_WAIT_1 ||
	       tp->state == RT_TCPS_FIN_WAIT_2;
}

static void bg_apply(struct tcp_sock *tp, u32 want)
{
	tp->bg_want = (u8)want;
	if ((want & TCP_BG_SEND) && !tp->ca_user) {
		if (tp->ca_ops != &tcp_ledbat)
			tcp_ca_select(tp, "ledbat");
	} else if (!(want & TCP_BG_SEND) && tp->ca_ops == &tcp_ledbat && !tp->ca_user) {
		if (tcp_ca_select(tp, sysctl_tcp_cc) < 0)
			tcp_ca_select(tp, "cubic");
	}
	if (!(want & TCP_BG_RECV))
		tcp_rledbat_stop(tp);
	else if (tcp_can_receive(tp))
		tcp_rledbat_start(tp);
}

/* a new socket, in the creating program's context */
void tcp_bg_attach(struct tcp_sock *tp)
{
	char name[32];

	if (!bg_programs[0])
		return;
	rt_os_program_name(name, sizeof(name));
	tp->bg_prog = name[0] && prog_list_match(bg_programs, name);
}

/* connect() or a new passive connection: both ports are known */
void tcp_bg_classify(struct tcp_sock *tp)
{
	if (tp->bg_explicit || !sysctl_tcp_bg_mode)
		return;
	if (tp->bg_prog || port_list_match(bg_ports, rt_ntohs(tp->dport)) == 1 ||
	    port_list_match(bg_lports, rt_ntohs(tp->sport)) == 1) {
		TCP_DBG(tp, "background connection (%s%s)",
		        sysctl_tcp_bg_mode & TCP_BG_RECV ? "rLEDBAT" : "",
		        (sysctl_tcp_bg_mode & 3) == 3 ? ", LEDBAT" : sysctl_tcp_bg_mode & TCP_BG_SEND ? "LEDBAT" : "");
		bg_apply(tp, (u32)sysctl_tcp_bg_mode & 3u);
	}
}

/* setsockopt(TCP_BACKGROUND) */
void tcp_bg_set(struct tcp_sock *tp, u32 want)
{
	tp->bg_explicit = 1;
	bg_apply(tp, want & 3u);
}

/* ====================================================================
 * Tunables and the "background" keyword
 * ==================================================================== */
static int bg_list_sysctl(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen,
                          int ports)
{
	char *s = e->ptr;
	u32 n = (u32)strlen(s) + 1;

	if (old) {
		if (!oldlen || *oldlen < n)
			return -RT_ENOMEM;
		memcpy(old, s, n);
		*oldlen = n;
	} else if (oldlen) {
		*oldlen = n;
	}
	if (newp) {
		char v[128];
		if (newlen >= (u32)e->max || newlen >= sizeof(v))
			return -RT_EINVAL;
		memcpy(v, newp, newlen);
		v[newlen] = 0;
		if (ports && port_list_match(v, 0) < 0)
			return -RT_EINVAL;
		memcpy(s, v, newlen + 1);
	}
	return 0;
}

static int bg_ports_sysctl(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	return bg_list_sysctl(e, old, oldlen, newp, newlen, 1);
}

static int bg_progs_sysctl(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	return bg_list_sysctl(e, old, oldlen, newp, newlen, 0);
}

#define NO_MIB 0, { 0, 0, 0, 0, 0, 0 }
static const struct rt_sysctl ledbat_sysctls[] = {
	{ "net.inet.tcp.ledbat_target", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_ledbat_target, 5, 1000,
	  NULL, "LEDBAT/rLEDBAT queueing delay target (ms)" },
	{ "net.inet.tcp.ledbat_slowdown", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_ledbat_slowdown, 0, 1,
	  NULL, "LEDBAT++ periodic slowdowns" },
	{ "net.inet.tcp.bg_mode", RT_SYSCTL_INT, RT_SYSCTL_RW, NO_MIB, &sysctl_tcp_bg_mode, 0, 3,
	  NULL, "Background rules: 1 rLEDBAT, 2 LEDBAT, 3 both, 0 off" },
	{ "net.inet.tcp.bg_programs", RT_SYSCTL_STRING, RT_SYSCTL_RW, NO_MIB, bg_programs, 0,
	  sizeof(bg_programs), bg_progs_sysctl, "Background programs (name,name*,...)" },
	{ "net.inet.tcp.bg_ports", RT_SYSCTL_STRING, RT_SYSCTL_RW, NO_MIB, bg_ports, 0,
	  sizeof(bg_ports), bg_ports_sysctl, "Background remote ports (20,21,6881-6889)" },
	{ "net.inet.tcp.bg_lports", RT_SYSCTL_STRING, RT_SYSCTL_RW, NO_MIB, bg_lports, 0,
	  sizeof(bg_lports), bg_ports_sysctl, "Background local ports" },
};

void tcp_ledbat_init(void)
{
	rt_sysctl_register(ledbat_sysctls, (int)RT_ARRAY_SIZE(ledbat_sysctls));
}

static const char *const bg_mode_names[4] = { "off", "recv", "send", "both" };

static void bg_status(char *out, u32 outlen)
{
	rt_snprintf(out, outlen,
	            "background: mode %s, programs %s, ports %s, local ports %s; LEDBAT target %ld ms%s\n",
	            bg_mode_names[sysctl_tcp_bg_mode & 3], bg_programs[0] ? bg_programs : "-",
	            bg_ports[0] ? bg_ports : "-", bg_lports[0] ? bg_lports : "-",
	            (long)sysctl_tcp_ledbat_target, sysctl_tcp_ledbat_slowdown ? "" : ", no slowdowns");
}

/* for STATUS: the rules, "" when there are none */
void rt_tcp_bg_status(char *buf, u32 len)
{
	buf[0] = 0;
	if (bg_programs[0] || bg_ports[0] || bg_lports[0])
		bg_status(buf, len);
}

/*
 * background                         show the rules
 * background clear                   no rules
 * background [program=A,B] [port=P,P-Q] [lport=P] [mode=both|recv|send|off]
 *            [target=MS]             set what is given (a list replaces the old one;
 *                                    an empty one, "port=", clears it)
 * "key value" and "key = value" work too (AmigaDOS ReadArgs splits
 * "key=value" into two words). Rules apply to connections opened later.
 */
#define BG_KEYS 5
static const char *const bg_keys[BG_KEYS] = { "program", "port", "lport", "mode", "target" };

static int bg_key(const char *a, u32 len)
{
	int k;

	for (k = 0; k < BG_KEYS; k++)
		if (strlen(bg_keys[k]) == len && !rt_strncasecmp(a, bg_keys[k], len))
			return k;
	return -1;
}

static int bg_value_ok(int k, const char *v)
{
	const char *e;
	u32 t;

	switch (k) {
	case 0:
		return strlen(v) < sizeof(bg_programs);
	case 1:
	case 2:
		return strlen(v) < sizeof(bg_ports) && port_list_match(v, 0) >= 0;
	case 3:
		return !rt_strcasecmp(v, "off") || !rt_strcasecmp(v, "recv") || !rt_strcasecmp(v, "send") ||
		       !rt_strcasecmp(v, "both");
	default:
		t = (u32)rt_strtoul(v, &e, 10);
		return *v && !*e && t >= 5 && t <= 1000;
	}
}

int rt_tcp_bg_command(int argc, char **argv, char *out, u32 outlen)
{
	const char *val[BG_KEYS];
	int i, k;

	out[0] = 0;
	if (argc <= 1 || (argc == 2 && !rt_strcasecmp(argv[1], "status"))) {
		bg_status(out, outlen);
		return 0;
	}
	if (argc == 2 && !rt_strcasecmp(argv[1], "clear")) {
		bg_programs[0] = bg_ports[0] = bg_lports[0] = 0;
		return 0;
	}
	/* collect and check everything before changing anything */
	for (k = 0; k < BG_KEYS; k++)
		val[k] = NULL;
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *eq = strchr(a, '=');
		k = bg_key(a, eq ? (u32)(eq - a) : (u32)strlen(a));
		if (k < 0)
			goto bad;
		if (eq && eq[1]) {
			val[k] = eq + 1;
		} else if (eq) {
			/* "key=" alone: an empty list, unless a value follows */
			val[k] = i + 1 < argc && bg_key(argv[i + 1], (u32)strlen(argv[i + 1])) < 0 &&
			         !strchr(argv[i + 1], '=') ? argv[++i] : "";
		} else {
			if (i + 1 < argc && !strcmp(argv[i + 1], "="))
				i++;
			if (i + 1 >= argc)
				goto bad;
			val[k] = argv[++i];
		}
		if (!bg_value_ok(k, val[k]))
			goto bad;
	}
	if (val[0])
		rt_strlcpy(bg_programs, val[0], sizeof(bg_programs));
	if (val[1])
		rt_strlcpy(bg_ports, val[1], sizeof(bg_ports));
	if (val[2])
		rt_strlcpy(bg_lports, val[2], sizeof(bg_lports));
	if (val[3]) {
		u32 m;
		for (m = 0; m < 4 && rt_strcasecmp(val[3], bg_mode_names[m]); m++)
			;
		sysctl_tcp_bg_mode = (s32)m;
	}
	if (val[4])
		sysctl_tcp_ledbat_target = (s32)rt_strtoul(val[4], NULL, 10);
	return 0;
bad:
	rt_strlcpy(out, "usage: background [clear] [program=NAME,...] [port=P,P-Q,...] [lport=P,...] "
	                "[mode=both|recv|send|off] [target=MS]", outlen);
	return -RT_EINVAL;
}
