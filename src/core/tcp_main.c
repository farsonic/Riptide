/*
 * Riptide - TCP: connection tables, port selection, ISN/secret hashing,
 * socket layer glue (struct rt_proto_ops rt_tcp_ops), sendmsg/recvmsg,
 * socket options, tunables and netstat listing.
 * Structure follows Linux net/ipv4/tcp.c, inet_hashtables.c, tcp_ipv4.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"
#include "infra.h"

struct rt_tcp_stats rt_tcpstat;

/* ---- tunables ---- */
s32 sysctl_tcp_timestamps = 1;
s32 sysctl_tcp_window_scaling = 1;
s32 sysctl_tcp_sack = 1;
s32 sysctl_tcp_dsack = 1;
s32 sysctl_tcp_ecn = 2;
s32 sysctl_tcp_initcwnd = 10;
s32 sysctl_tcp_rto_min_ms = 200;
s32 sysctl_tcp_syncookies = 1;
s32 sysctl_tcp_synack_retries = 5;
s32 sysctl_tcp_syn_retries = 6;
s32 sysctl_tcp_retries1 = 3;
s32 sysctl_tcp_retries2 = 15;
s32 sysctl_tcp_keepcnt = 9;
s32 sysctl_tcp_fin_timeout = 60;
s32 sysctl_tcp_tw_reuse = 1;
s32 sysctl_tcp_frto = 2;
s32 sysctl_tcp_recovery = TCP_RACK_LOSS_DETECTION;
s32 sysctl_tcp_reordering = 3;
s32 sysctl_tcp_rmem_max = 262144;
s32 sysctl_tcp_wmem_max = 262144;
s32 sysctl_tcp_moderate_rcvbuf = 1;
s32 sysctl_tcp_slow_start_after_idle = 1;
s32 sysctl_tcp_max_orphans = 128;
s32 sysctl_tcp_mssdflt = TCP_MSS_DEFAULT;
s32 sysctl_tcp_keepidle = 7200;
s32 sysctl_tcp_keepintvl = 75;
s32 sysctl_tcp_max_tw = 1024;
s32 sysctl_tcp_delack_segs = 2;
s32 sysctl_tcp_delack_ms = TCP_DELACK_MAX;
s32 sysctl_tcp_quickack = 0;
char sysctl_tcp_cc[16] = "cubic";
/* connecting: first SYN RTO, its growth and cap, and the whole connect
 * (Linux: 1 s doubling to 120 s, 6 retries = 127 s; a desktop program
 * blocked in connect() to a dead host should not hang that long) */
s32 sysctl_tcp_syn_rto_initial_ms = 1000;
s32 sysctl_tcp_syn_rto_max_ms = 4000;
s32 sysctl_tcp_syn_backoff = 1;
s32 sysctl_tcp_connect_timeout = 20;
s32 sysctl_tcp_syn_icmp_abort = 1;
s32 sysctl_tcp_keepalive_all = 0;

u32 tcp_orphan_count;
u32 tcp_tw_count;
u64 tcp_clock_us;

/* ---- monotonic seconds (for ts_recent_stamp / syncookies) ---- */
static u32 secs_last_ms, secs_acc_ms, secs_now;

u32 tcp_seconds(void)
{
	u32 now = rt_now_ms();
	u32 d = now - secs_last_ms;

	secs_last_ms = now;
	secs_acc_ms += d;
	while (secs_acc_ms >= 1000) {
		secs_acc_ms -= 1000;
		secs_now++;
	}
	return secs_now;
}

/* ---- SipHash-2-4 (keyed hash for ISNs, timestamps offsets, cookies,
 * ephemeral port offsets: RFC 6528 / RFC 6056) ---- */
struct sip_key { u64 k0, k1; };
static struct sip_key isn_key, tsoff_key, cookie_key, port_key;
static u32 ehash_seed;

static u64 rotl64(u64 x, int b) { return (x << b) | (x >> (64 - b)); }

static void sipround(u64 *v)
{
	v[0] += v[1]; v[1] = rotl64(v[1], 13); v[1] ^= v[0]; v[0] = rotl64(v[0], 32);
	v[2] += v[3]; v[3] = rotl64(v[3], 16); v[3] ^= v[2];
	v[0] += v[3]; v[3] = rotl64(v[3], 21); v[3] ^= v[0];
	v[2] += v[1]; v[1] = rotl64(v[1], 17); v[1] ^= v[2]; v[2] = rotl64(v[2], 32);
}

static u64 siphash_4u32(u32 a, u32 b, u32 c, u32 d, const struct sip_key *k)
{
	u64 v[4], m;
	int i;

	v[0] = k->k0 ^ 0x736f6d6570736575ULL;
	v[1] = k->k1 ^ 0x646f72616e646f6dULL;
	v[2] = k->k0 ^ 0x6c7967656e657261ULL;
	v[3] = k->k1 ^ 0x7465646279746573ULL;
	m = ((u64)b << 32) | a;
	v[3] ^= m; sipround(v); sipround(v); v[0] ^= m;
	m = ((u64)d << 32) | c;
	v[3] ^= m; sipround(v); sipround(v); v[0] ^= m;
	m = (u64)16 << 56;
	v[3] ^= m; sipround(v); sipround(v); v[0] ^= m;
	v[2] ^= 0xff;
	for (i = 0; i < 4; i++)
		sipround(v);
	return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/* SipHash-2-4 over both addresses and two more words: IPv6 (and mixed)
 * address pairs hash all 128 bits (RFC 6528 asks for the full 4-tuple). */
static u64 siphash_addrs(const rt_in6_addr *a, const rt_in6_addr *b, u32 c, u32 d,
                         const struct sip_key *k)
{
	u8 key[16], msg[40];

	if (rt_in6_is_v4mapped(a) && rt_in6_is_v4mapped(b))
		return siphash_4u32(rt_in6_v4(a), rt_in6_v4(b), c, d, k);
	memcpy(key, &k->k0, 8);
	memcpy(key + 8, &k->k1, 8);
	memcpy(msg, a->b, 16);
	memcpy(msg + 16, b->b, 16);
	memcpy(msg + 32, &c, 4);
	memcpy(msg + 36, &d, 4);
	return rt_siphash(msg, sizeof(msg), key);
}

u32 tcp_addr_fold(const rt_in6_addr *a)
{
	return a->w[0] ^ a->w[1] ^ a->w[2] ^ a->w[3];
}

/* RFC 6528: ISN = M + F(localip, localport, remoteip, remoteport, secret),
 * M a 64 ns-ish clock like Linux secure_tcp_seq(). */
u32 tcp_secure_seq(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport)
{
	u32 h = (u32)siphash_addrs(saddr, daddr, ((u32)sport << 16) | dport, 0, &isn_key);
	return h + (u32)(rt_now_us() << 4);
}

/* Per address pair timestamp offset (Linux secure_tcp_ts_off). */
u32 tcp_secure_ts_off(const rt_in6_addr *saddr, const rt_in6_addr *daddr)
{
	return (u32)siphash_addrs(saddr, daddr, 0, 0, &tsoff_key);
}

u32 tcp_cookie_hash(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport,
                    u32 count, u32 c)
{
	return (u32)siphash_addrs(saddr, daddr, ((u32)sport << 16) | dport,
	                          count ^ (c << 31), &cookie_key);
}

/* ---- windowed min filter (Linux lib/win_minmax.c) ---- */
static u32 minmax_subwin_update(struct minmax *m, u32 win, const struct minmax_sample *val)
{
	u32 dt = val->t - m->s[0].t;

	if (RT_UNLIKELY(dt > win)) {
		m->s[0] = m->s[1];
		m->s[1] = m->s[2];
		m->s[2] = *val;
		if (RT_UNLIKELY(val->t - m->s[0].t > win)) {
			m->s[0] = m->s[1];
			m->s[1] = m->s[2];
			m->s[2] = *val;
		}
	} else if (RT_UNLIKELY(m->s[1].t == m->s[0].t) && dt > win / 4) {
		m->s[2] = m->s[1] = *val;
	} else if (RT_UNLIKELY(m->s[2].t == m->s[1].t) && dt > win / 2) {
		m->s[2] = *val;
	}
	return m->s[0].v;
}

u32 minmax_running_min(struct minmax *m, u32 win, u32 t, u32 meas)
{
	struct minmax_sample val;

	val.t = t;
	val.v = meas;
	if (RT_UNLIKELY(val.v <= m->s[0].v) || RT_UNLIKELY(val.t - m->s[2].t > win))
		return minmax_reset(m, t, meas);
	if (RT_UNLIKELY(val.v <= m->s[1].v))
		m->s[2] = m->s[1] = val;
	else if (RT_UNLIKELY(val.v <= m->s[2].v))
		m->s[2] = val;
	return minmax_subwin_update(m, win, &val);
}

/* ---- connection tables (Linux inet_hashtables) ----
 * ehash: synchronized connections by 4-tuple; lhash: listeners by port;
 * bhash: everything that owns a local port. All keys in network order. */
#define TCP_EHASH_SIZE 256
#define TCP_LHASH_SIZE 32
#define TCP_BHASH_SIZE 64

static struct tcp_sock *tcp_ehash[TCP_EHASH_SIZE];
static struct tcp_sock *tcp_lhash[TCP_LHASH_SIZE];
static struct tcp_sock *tcp_bhash[TCP_BHASH_SIZE];
static struct tcp_sock *tcp_all;
static struct tcp_sock *tcp_reap_list;
static int tcp_nest;

static u32 tcp_ehashfn(const rt_in6_addr *la, u16 lport, const rt_in6_addr *fa, u16 fport)
{
	u32 h = tcp_addr_fold(la) * 0x9e3779b1u;

	h ^= tcp_addr_fold(fa) + ehash_seed;
	h ^= ((u32)lport << 16) | fport;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	h *= 0x297a2d39u;
	h ^= h >> 15;
	return h & (TCP_EHASH_SIZE - 1);
}

u32 tcp_tw_hashfn(const rt_in6_addr *laddr, u16 lport, const rt_in6_addr *faddr, u16 fport)
{
	return tcp_ehashfn(laddr, lport, faddr, fport);
}

static u32 tcp_porthash(u16 port) { return (u32)(rt_ntohs(port) * 0x9e37u) >> 3; }

void tcp_ehash_insert(struct tcp_sock *tp)
{
	u32 h;

	if (tp->in_ehash)
		return;
	h = tcp_ehashfn(&tp->saddr, tp->sport, &tp->daddr, tp->dport);
	tp->ehash_next = tcp_ehash[h];
	tcp_ehash[h] = tp;
	tp->in_ehash = 1;
}

void tcp_ehash_remove(struct tcp_sock *tp)
{
	struct tcp_sock **pp;

	if (!tp->in_ehash)
		return;
	pp = &tcp_ehash[tcp_ehashfn(&tp->saddr, tp->sport, &tp->daddr, tp->dport)];
	while (*pp && *pp != tp)
		pp = &(*pp)->ehash_next;
	if (*pp)
		*pp = tp->ehash_next;
	tp->in_ehash = 0;
}

static void tcp_lhash_insert(struct tcp_sock *tp)
{
	u32 h = tcp_porthash(tp->sport) & (TCP_LHASH_SIZE - 1);

	if (tp->in_lhash)
		return;
	tp->lhash_next = tcp_lhash[h];
	tcp_lhash[h] = tp;
	tp->in_lhash = 1;
}

static void tcp_lhash_remove(struct tcp_sock *tp)
{
	struct tcp_sock **pp;

	if (!tp->in_lhash)
		return;
	pp = &tcp_lhash[tcp_porthash(tp->sport) & (TCP_LHASH_SIZE - 1)];
	while (*pp && *pp != tp)
		pp = &(*pp)->lhash_next;
	if (*pp)
		*pp = tp->lhash_next;
	tp->in_lhash = 0;
}

void tcp_bhash_insert(struct tcp_sock *tp)
{
	u32 h = tcp_porthash(tp->sport) & (TCP_BHASH_SIZE - 1);

	if (tp->in_bhash)
		return;
	tp->bhash_next = tcp_bhash[h];
	tcp_bhash[h] = tp;
	tp->in_bhash = 1;
}

static void tcp_bhash_remove(struct tcp_sock *tp)
{
	struct tcp_sock **pp;

	if (!tp->in_bhash)
		return;
	pp = &tcp_bhash[tcp_porthash(tp->sport) & (TCP_BHASH_SIZE - 1)];
	while (*pp && *pp != tp)
		pp = &(*pp)->bhash_next;
	if (*pp)
		*pp = tp->bhash_next;
	tp->in_bhash = 0;
}

void tcp_unhash(struct tcp_sock *tp)
{
	tcp_ehash_remove(tp);
	tcp_lhash_remove(tp);
	tcp_bhash_remove(tp);
}

struct tcp_sock *tcp_lookup_established(const rt_in6_addr *laddr, u16 lport, const rt_in6_addr *faddr,
                                        u16 fport)
{
	struct tcp_sock *tp = tcp_ehash[tcp_ehashfn(laddr, lport, faddr, fport)];

	for (; tp; tp = tp->ehash_next)
		if (tp->sport == lport && tp->dport == fport &&
		    rt_in6_eq(&tp->daddr, faddr) && rt_in6_eq(&tp->saddr, laddr))
			return tp;
	return NULL;
}

/* Best listener: exact local address beats wildcard, and for IPv4 an
 * AF_INET wildcard beats a dual-stack :: one (Linux compute_score). */
struct tcp_sock *tcp_lookup_listener(const rt_in6_addr *laddr, u16 lport)
{
	struct tcp_sock *tp = tcp_lhash[tcp_porthash(lport) & (TCP_LHASH_SIZE - 1)];
	struct tcp_sock *best = NULL;
	int bestscore = 0;

	for (; tp; tp = tp->lhash_next) {
		int m, score;
		if (tp->sport != lport || tp->state != RT_TCPS_LISTEN)
			continue;
		m = rt_inet_laddr_match(&tp->saddr, tp->v6only, laddr);
		if (m == 2)
			return tp;
		if (!m)
			continue;
		score = rt_in6_is_v4mapped(&tp->saddr) ? 2 : 1;
		if (score > bestscore) {
			best = tp;
			bestscore = score;
		}
	}
	return best;
}

/* ---- port allocation ---- */
static void tcp_port_range(u32 *low, u32 *high)
{
	s32 lo = rt_ip_portrange_first, hi = rt_ip_portrange_last;

	if (lo <= 0 || lo > 65535 || hi <= 0 || hi > 65535) {
		lo = 49152;
		hi = 65535;
	}
	if (hi < lo) {
		s32 t = lo;
		lo = hi;
		hi = t;
	}
	*low = (u32)lo;
	*high = (u32)hi;
}

/* Linux inet_csk_bind_conflict(): sockets sharing a port must not overlap
 * in local address unless both set SO_REUSEADDR (and the other is not
 * listening) or both set SO_REUSEPORT. TIME_WAIT owners only block a bind
 * without SO_REUSEADDR. */
static int tcp_bind_conflict(const struct tcp_sock *tp, u16 port, const rt_in6_addr *addr, int listening)
{
	struct tcp_sock *sk2 = tcp_bhash[tcp_porthash(port) & (TCP_BHASH_SIZE - 1)];

	for (; sk2; sk2 = sk2->bhash_next) {
		if (sk2 == tp || sk2->sport != port)
			continue;
		if (!rt_inet_bind_overlap(addr, tp->v6only, &sk2->saddr, sk2->v6only))
			continue;
		if (tp->reuseport && sk2->reuseport)
			continue;
		if (listening) {
			if (sk2->state == RT_TCPS_LISTEN)
				return 1;
			continue;
		}
		if (tp->reuse && sk2->reuse && sk2->state != RT_TCPS_LISTEN)
			continue;
		return 1;
	}
	if (!listening && tcp_tw_port_used(port, addr, tp->v6only, tp->reuse))
		return 1;
	return 0;
}

static int tcp_port_in_use(u16 port)
{
	struct tcp_sock *sk2 = tcp_bhash[tcp_porthash(port) & (TCP_BHASH_SIZE - 1)];

	for (; sk2; sk2 = sk2->bhash_next)
		if (sk2->sport == port)
			return 1;
	return tcp_tw_port_used(port, NULL, 0, 0);
}

/* bind(port 0): random start, first port nobody owns. */
static int tcp_autobind(struct tcp_sock *tp)
{
	u32 low, high, remaining, offset, i;

	tcp_port_range(&low, &high);
	remaining = high - low + 1;
	offset = rt_random32() % remaining;
	for (i = 0; i < remaining; i++) {
		u16 port = rt_htons((u16)(low + (offset + i) % remaining));
		if (!tcp_port_in_use(port)) {
			tp->sport = port;
			tcp_bhash_insert(tp);
			return 0;
		}
	}
	return -RT_EADDRINUSE;
}

/* RFC 6056 algorithm 4 (double-hash port selection), as Linux
 * __inet_hash_connect(): offset = F(addresses, secret) + table[G(...)],
 * a port may be shared by several connections as long as the 4-tuple is
 * unique and every owner got it from connect(). */
#define TCP_PERTURB_SIZE 64
static u16 tcp_perturb[TCP_PERTURB_SIZE];

static int tcp_check_established(struct tcp_sock *tp, u16 lport)
{
	struct tcp_tw *tw;

	if (tcp_lookup_established(&tp->saddr, lport, &tp->daddr, tp->dport))
		return -RT_EADDRNOTAVAIL;
	tw = tcp_tw_lookup(&tp->saddr, lport, &tp->daddr, tp->dport);
	if (tw) {
		if (!tcp_twsk_unique(tp, tw))
			return -RT_EADDRNOTAVAIL;
		rt_tcpstat.TimeWaitRecycled++;
		tcp_tw_kill(tw);
	}
	return 0;
}

static int tcp_hash_connect(struct tcp_sock *tp)
{
	u32 low, high, remaining, offset, i, idx;
	u64 h;

	if (tp->sport) {
		int err = tcp_check_established(tp, tp->sport);
		if (err)
			return err;
		tcp_ehash_insert(tp);
		return 0;
	}
	tcp_port_range(&low, &high);
	remaining = high - low + 1;
	h = siphash_addrs(&tp->saddr, &tp->daddr, tp->dport, 0, &port_key);
	idx = (u32)(h >> 32) & (TCP_PERTURB_SIZE - 1);
	offset = (u32)h + tcp_perturb[idx];
	for (i = 0; i < remaining; i++) {
		u16 port = rt_htons((u16)(low + (offset + i) % remaining));
		struct tcp_sock *sk2 = tcp_bhash[tcp_porthash(port) & (TCP_BHASH_SIZE - 1)];
		int bad = 0;

		for (; sk2; sk2 = sk2->bhash_next)
			if (sk2->sport == port && !sk2->autobind) {
				bad = 1;
				break;
			}
		if (bad)
			continue;
		if (tcp_check_established(tp, port))
			continue;
		tp->sport = port;
		tp->autobind = 1;
		tcp_bhash_insert(tp);
		tcp_ehash_insert(tp);
		tcp_perturb[idx] = (u16)(tcp_perturb[idx] + i + 2);
		return 0;
	}
	return -RT_EADDRNOTAVAIL;
}

/* ---- socket allocation / lifetime ---- */
struct tcp_sock *tcp_first(void)
{
	return tcp_all;
}

struct tcp_sock *tcp_sock_alloc(void)
{
	struct tcp_sock *tp = rt_zalloc(sizeof(*tp));

	if (!tp)
		return NULL;
	tp->all_next = tcp_all;
	if (tcp_all)
		tcp_all->all_prev = tp;
	tcp_all = tp;
	return tp;
}

/* Linux tcp_init_sock() */
void tcp_init_sock(struct tcp_sock *tp)
{
	rt_bufq_init(&tp->wq);
	rt_bufq_init(&tp->rcvq);
	rt_bufq_init(&tp->ofoq);
	tcp_init_xmit_timers(tp);
	tp->state = RT_TCPS_CLOSED;
	tp->icsk_rto = TCP_TIMEOUT_INIT;
	tp->rttvar_us = TCP_TIMEOUT_INIT * 1000u;
	tp->mdev_us = TCP_TIMEOUT_INIT * 1000u;
	minmax_reset(&tp->rtt_min, tcp_jiffies(), ~0u);
	tp->snd_cwnd = (u32)sysctl_tcp_initcwnd;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	tp->snd_cwnd_clamp = ~0u;
	tp->mss_cache = TCP_MSS_DEFAULT;
	tp->reordering = (u32)sysctl_tcp_reordering;
	tp->rack.reo_wnd_steps = 1;
	tp->rx_opt.mss_clamp = TCP_MAX_WINDOW;
	tp->sndbuf = 32768;
	tp->rcvbuf = 65536;
	tp->linger2 = (u32)sysctl_tcp_fin_timeout * 1000u;
	tp->ca_ops = tcp_ca_find(sysctl_tcp_cc);
	if (!tp->ca_ops)
		tp->ca_ops = &tcp_cubic;
	tp->ack.ato = 0;
	tp->family = RT_AF_INET;
	tp->net_hlen = 20;
	tp->fo_len = TCP_FASTOPEN_NONE;
	rt_inaddr_v4any(&tp->saddr);
	rt_inaddr_v4any(&tp->daddr);
}

void tcp_write_queue_purge(struct tcp_sock *tp)
{
	rt_bufq_purge(&tp->wq);
	tp->send_head = NULL;
	tp->wq_bytes = 0;
	tp->wmem_queued = 0;
	tp->packets_out = 0;
	tcp_clear_retrans(tp);
}

static void tcp_rcv_purge(struct tcp_sock *tp)
{
	rt_bufq_purge(&tp->rcvq);
	rt_bufq_purge(&tp->ofoq);
	tp->rmem_alloc = 0;
	tp->rcvq_bytes = 0;
	tp->ofo_bytes = 0;
	tp->rx_opt.num_sacks = 0;
}

/* Linux inet_csk_destroy_sock(): drop every resource; the memory itself is
 * freed by tcp_reap() once the current entry point has finished with it. */
void tcp_destroy_sock(struct tcp_sock *tp)
{
	if (tp->destroyed)
		return;
	tp->destroyed = 1;
	tcp_batch_forget(tp);
	tcp_rledbat_stop(tp);
	tcp_unhash(tp);
	tcp_clear_xmit_timers(tp);
	tcp_delete_keepalive_timer(tp);
	tcp_write_queue_purge(tp);
	tcp_rcv_purge(tp);
	tcp_synq_purge(tp);
	if (tp->dead && tcp_orphan_count)
		tcp_orphan_count--;
	if (tp->all_prev)
		tp->all_prev->all_next = tp->all_next;
	else
		tcp_all = tp->all_next;
	if (tp->all_next)
		tp->all_next->all_prev = tp->all_prev;
	if (tp->so) {
		tp->so->pcb = NULL;
		tp->so = NULL;
	}
	tp->reap_next = tcp_reap_list;
	tcp_reap_list = tp;
}

void tcp_reap(void)
{
	if (tcp_nest)
		return;
	while (tcp_reap_list) {
		struct tcp_sock *tp = tcp_reap_list;
		tcp_reap_list = tp->reap_next;
		rt_free(tp);
	}
}

/* ---- chronographs (Linux tcp_chrono_set / tcp_chrono_stop) ---- */
void tcp_chrono_set(struct tcp_sock *tp, u8 type)
{
	u32 now = tcp_jiffies();

	if (tp->dg.chrono_type > TCP_CHRONO_NONE)
		tp->dg.chrono_stat[tp->dg.chrono_type - 1] += now - tp->dg.chrono_start;
	tp->dg.chrono_start = now;
	tp->dg.chrono_type = type;
}

void tcp_chrono_stop(struct tcp_sock *tp, u8 type)
{
	if (!tp->packets_out && !tp->send_head)
		tcp_chrono_set(tp, TCP_CHRONO_NONE);
	else if (type == tp->dg.chrono_type)
		tcp_chrono_set(tp, TCP_CHRONO_BUSY);
}
#ifndef RT_NO_DIAG
void tcp_dbg(const struct tcp_sock *tp, const char *fmt, ...)
{
	char line[180];
	va_list ap;
	int n;

	n = rt_snprintf(line, sizeof(line), "%u>%u ", (unsigned)rt_ntohs(tp->sport), (unsigned)rt_ntohs(tp->dport));
	va_start(ap, fmt);
	rt_vsnprintf(line + n, sizeof(line) - (u32)n, fmt, ap);
	va_end(ap);
	rt_dbg_line(RT_DBG_TCP, line);
}
#endif

void tcp_set_state(struct tcp_sock *tp, int state)
{
	int old = tp->state;

#ifndef RT_NO_DIAG
	static const char *const names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED",
	                                     "CLOSE_WAIT", "FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2",
	                                     "TIME_WAIT" };
	if (state != old)
		TCP_DBG(tp, "state %s -> %s", old <= RT_TCPS_TIME_WAIT ? names[old] : "?",
		        state <= RT_TCPS_TIME_WAIT ? names[state] : "?");
#endif
	if (state == RT_TCPS_ESTABLISHED && old != RT_TCPS_ESTABLISHED) {
		rt_tcpstat.CurrEstab++;
		if (!tp->dg.estab_ms)
			tp->dg.estab_ms = tcp_diag_now();
	}
	if (old == RT_TCPS_ESTABLISHED && state != RT_TCPS_ESTABLISHED && rt_tcpstat.CurrEstab)
		rt_tcpstat.CurrEstab--;
	if (state == RT_TCPS_CLOSED) {
		if (old == RT_TCPS_ESTABLISHED || old == RT_TCPS_CLOSE_WAIT)
			rt_tcpstat.EstabResets++;
		tcp_ehash_remove(tp);
		tcp_lhash_remove(tp);
		if (tp->autobind) {
			tcp_bhash_remove(tp);
			tp->autobind = 0;
			tp->sport = 0;
		}
	}
	tp->state = (u8)state;
}

void tcp_sock_wake(struct tcp_sock *tp, u32 events)
{
	if (tp->so)
		rt_sock_wakeup(tp->so, events);
}

void tcp_sock_error(struct tcp_sock *tp, int err)
{
	if (tp->so)
		rt_sock_set_error(tp->so, err);
}

/* Linux tcp_done() */
void tcp_done(struct tcp_sock *tp)
{
	if (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED)
		rt_tcpstat.AttemptFails++;
	tcp_set_state(tp, RT_TCPS_CLOSED);
	tcp_clear_xmit_timers(tp);
	tcp_delete_keepalive_timer(tp);
	tp->shutdown = SHUTDOWN_MASK;
	if (tp->so) {
		struct rt_socket *so = tp->so;
		so->state &= ~(RT_SS_ISCONNECTING | RT_SS_ISCONNECTED);
		so->state |= RT_SS_CANTRCVMORE | RT_SS_CANTSENDMORE;
		rt_sock_wakeup(so, RT_SEV_CLOSE | RT_SEV_READ | RT_SEV_WRITE);
	} else {
		tcp_destroy_sock(tp);
	}
}

/* ---- buffer sizing ---- */

/* Pick up SOL_SOCKET state the socket layer stores in 'so'. A change of
 * so->rcvbuf/sndbuf we did not make ourselves is a user SO_RCVBUF/SNDBUF
 * and disables auto-tuning (Linux SOCK_RCVBUF_LOCK). */
void tcp_sync_rcvbuf(struct tcp_sock *tp)
{
	struct rt_socket *so = tp->so;
	int keep;

	if (!so)
		return;
	if (so->rcvbuf != tp->so_rcvbuf_seen) {
		tp->rcvbuf = RT_MAX(so->rcvbuf, 2048u);
		tp->so_rcvbuf_seen = so->rcvbuf;
		tp->rcvbuf_locked = 1;
		if (tp->state != RT_TCPS_CLOSED && tp->state != RT_TCPS_LISTEN &&
		    tp->window_clamp > tp->rcvbuf)
			tp->window_clamp = RT_MAX(tp->rcvbuf, (u32)tp->advmss * 2u);
		tcp_apply_window_limit(tp);
	}
	if (so->sndbuf != tp->so_sndbuf_seen) {
		tp->sndbuf = RT_MAX(so->sndbuf, 2048u);
		tp->so_sndbuf_seen = so->sndbuf;
		tp->sndbuf_locked = 1;
	}
	tp->sndlowat = so->sndlowat;
	tp->oobinline = (so->options & RT_SO_OOBINLINE) != 0;
	keep = (so->options & RT_SO_KEEPALIVE) != 0 || tp->ka_auto;
	if (keep != tp->keepopen) {
		tp->keepopen = (u8)keep;
		if (tp->state == RT_TCPS_ESTABLISHED || tp->state == RT_TCPS_CLOSE_WAIT ||
		    tp->state == RT_TCPS_FIN_WAIT_1 || tp->state == RT_TCPS_FIN_WAIT_2 ||
		    tp->state == RT_TCPS_CLOSING || tp->state == RT_TCPS_LAST_ACK) {
			if (keep)
				tcp_reset_keepalive_timer(tp, (u32)keepalive_time_when(tp));
			else
				tcp_delete_keepalive_timer(tp);
		}
	}
}

static void tcp_set_so_buffers(struct tcp_sock *tp)
{
	if (!tp->so)
		return;
	tp->so->rcvbuf = tp->rcvbuf;
	tp->so->sndbuf = tp->sndbuf;
	tp->so_rcvbuf_seen = tp->rcvbuf;
	tp->so_sndbuf_seen = tp->sndbuf;
}

u32 tcp_full_space(const struct tcp_sock *tp)
{
	return tp->rcvbuf;
}

/* Receive buffer auto-tuning (Linux tcp_rcv_space_adjust, DRS): measure
 * what the application consumed in one receiver RTT and size the buffer
 * to twice that plus growth headroom, up to net.inet.tcp.rmem_max. */
void tcp_rcv_space_adjust(struct tcp_sock *tp)
{
	u32 copied, time_us;

	tcp_mstamp_refresh();
	time_us = (u32)(tcp_clock_us - tp->rcvq_space.time);
	if (tp->rcv_rtt_est.rtt_us == 0 || time_us < (tp->rcv_rtt_est.rtt_us >> 3))
		return;
	copied = tp->copied_seq - tp->rcvq_space.seq;
	if (copied <= tp->rcvq_space.space)
		goto new_measure;
	if (sysctl_tcp_moderate_rcvbuf && !tp->rcvbuf_locked && !tcp_under_memory_pressure()) {
		u32 rcvwin, grow, sp = RT_MAX(tp->rcvq_space.space, 1u);

		rcvwin = (copied << 1) + 16u * tp->advmss;
		grow = (u32)(((u64)rcvwin * (copied - tp->rcvq_space.space)) / sp);
		rcvwin += grow << 1;
		if (rcvwin > (u32)sysctl_tcp_rmem_max)
			rcvwin = (u32)sysctl_tcp_rmem_max;
		if (rcvwin > tp->rcvbuf) {
			tp->rcvbuf = rcvwin;
			tp->window_clamp = RT_MIN(rcvwin, TCP_MAX_WINDOW << tp->rx_opt.rcv_wscale);
			tcp_apply_window_limit(tp);
			tcp_set_so_buffers(tp);
		}
	}
	tp->rcvq_space.space = copied;
new_measure:
	tp->rcvq_space.seq = tp->copied_seq;
	tp->rcvq_space.time = tcp_clock_us;
}

/* Linux tcp_cleanup_rbuf(): ACK after the reader made room. */
void tcp_cleanup_rbuf(struct tcp_sock *tp, int copied)
{
	int time_to_ack = 0;

	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN ||
	    tp->state == RT_TCPS_SYN_SENT)
		return;
	if (tp->ack.pending & ICSK_ACK_SCHED) {
		/* once per net.inet.tcp.delack_segs segments, if tcp_input.c did not */
		if (tp->rcv_nxt - tp->rcv_wup > (u32)(sysctl_tcp_delack_segs - 1) * tp->ack.rcv_mss ||
		    sysctl_tcp_quickack ||
		    (copied > 0 &&
		     ((tp->ack.pending & ICSK_ACK_PUSHED2) ||
		      ((tp->ack.pending & ICSK_ACK_PUSHED) && !tp->pingpong)) &&
		     !tp->rcvq_bytes))
			time_to_ack = 1;
	}
	if (copied > 0 && !time_to_ack && !(tp->shutdown & RCV_SHUTDOWN)) {
		u32 rcv_window_now = tcp_receive_window(tp);
		if (2 * rcv_window_now <= tp->window_clamp) {
			u32 new_window = __tcp_select_window(tp);
			/* Unlike Linux, not for every small read that doubles a
			 * nearly closed window: a reader slower than the link
			 * (nc's 2 KB recv()s on a 68020) would cost a window
			 * update per read. While data is still queued, open it
			 * by 4 segments (or a quarter of the buffer) at a time;
			 * the queue holds the rest of the buffer for the reader
			 * meanwhile. A drained queue always gets its update. */
			if (new_window && new_window >= 2 * rcv_window_now &&
			    (!tp->rcvq_bytes ||
			     new_window - rcv_window_now >= RT_MIN(tcp_full_space(tp) / 4, 4u * tp->ack.rcv_mss)))
				time_to_ack = 1;
		}
	}
	if (time_to_ack)
		tcp_send_ack(tp);
}

/* Linux tcp_sndbuf_expand(): room for two windows of the larger of the
 * congestion window, the initial window and the reordering degree, so a
 * sender is not limited by its buffer (unless SO_SNDBUF was set). */
void tcp_sndbuf_expand(struct tcp_sock *tp)
{
	u32 nr_segs, sndmem;

	if (tp->sndbuf_locked || tcp_under_memory_pressure())
		return;
	nr_segs = RT_MAX(RT_MAX((u32)sysctl_tcp_initcwnd, tp->snd_cwnd), tp->reordering + 1);
	sndmem = 2u * nr_segs * RT_MAX((u32)tp->mss_cache, 536u);
	if (sndmem > (u32)sysctl_tcp_wmem_max)
		sndmem = (u32)sysctl_tcp_wmem_max;
	if (sndmem > tp->sndbuf) {
		tp->sndbuf = sndmem;
		tcp_set_so_buffers(tp);
	}
}

/* Wake writers once enough send space is free (sk_stream_write_space). */
void tcp_check_space(struct tcp_sock *tp)
{
	/* Linux tcp_new_space(): grow the buffer when cwnd is not the limit */
	if (tcp_packets_in_flight(tp) < tp->snd_cwnd && tp->state == RT_TCPS_ESTABLISHED)
		tcp_sndbuf_expand(tp);
	if (tp->dg.chrono_type == TCP_CHRONO_SNDBUF && tcp_stream_is_writeable(tp))
		tcp_chrono_stop(tp, TCP_CHRONO_SNDBUF);
	if (tp->so && tp->write_space_wanted && tcp_stream_is_writeable(tp)) {
		tp->write_space_wanted = 0;
		rt_sock_wakeup(tp->so, RT_SEV_WRITE);
	}
}

/* ---- entry/exit bracket: deferred frees happen only at the outermost
 * level, so nested calls (loopback delivery) never see freed memory. ---- */
static void tcp_enter(void)
{
	tcp_nest++;
	tcp_mstamp_refresh();
}

static void tcp_exit(void)
{
	tcp_nest--;
	tcp_reap();
}

void tcp_api_enter(void) { tcp_enter(); }
void tcp_api_exit(void) { tcp_exit(); }

/* ======================================================================
 * Protocol operations
 * ====================================================================== */
static int tcp_v4_attach(struct rt_socket *so)
{
	struct tcp_sock *tp;

	tp = tcp_sock_alloc();
	if (!tp)
		return -RT_ENOBUFS;
	tcp_init_sock(tp);
	tp->so = so;
	so->pcb = tp;
	/* the socket layer set the defaults (net.inet.tcp.sendspace/recvspace) */
	tp->rcvbuf = RT_MAX(so->rcvbuf, 2048u);
	tp->sndbuf = RT_MAX(so->sndbuf, 2048u);
	tp->so_rcvbuf_seen = so->rcvbuf;
	tp->so_sndbuf_seen = so->sndbuf;
	tp->sndlowat = so->sndlowat;
	tcp_bg_attach(tp);
	if (so->domain == RT_AF_INET6) {
		tp->family = RT_AF_INET6;
		tp->v6only = rt_ip6_v6only != 0;
		memset(&tp->saddr, 0, sizeof(tp->saddr));
		memset(&tp->daddr, 0, sizeof(tp->daddr));
	}
	/* net.inet.tcp.keepalive_all: keep idle connections of programs that
	 * never ask for keepalive open through NAT routers */
	if (sysctl_tcp_keepalive_all)
		tp->ka_auto = tp->keepopen = 1;
	return 0;
}

/* Linux tcp_close_state() */
static int tcp_close_state(struct tcp_sock *tp)
{
	int ns, action = 0;

	switch (tp->state) {
	case RT_TCPS_ESTABLISHED:
	case RT_TCPS_SYN_RECEIVED:
		ns = RT_TCPS_FIN_WAIT_1; action = 1; break;
	case RT_TCPS_CLOSE_WAIT:
		ns = RT_TCPS_LAST_ACK; action = 1; break;
	case RT_TCPS_FIN_WAIT_1:
	case RT_TCPS_FIN_WAIT_2:
	case RT_TCPS_LAST_ACK:
	case RT_TCPS_CLOSING:
		ns = tp->state; break;
	default:
		ns = RT_TCPS_CLOSED; break;
	}
	tcp_set_state(tp, ns);
	return action;
}

/* a shutdown(SHUT_WR) that waited for the handshake */
void tcp_pending_fin(struct tcp_sock *tp)
{
	if (!tp->fin_pending)
		return;
	tp->fin_pending = 0;
	if (tcp_close_state(tp))
		tcp_send_fin(tp);
}

static int tcp_need_reset(int state)
{
	return state == RT_TCPS_ESTABLISHED || state == RT_TCPS_CLOSE_WAIT ||
	       state == RT_TCPS_FIN_WAIT_1 || state == RT_TCPS_FIN_WAIT_2 ||
	       state == RT_TCPS_SYN_RECEIVED;
}

/* Linux tcp_disconnect() for the abortive close paths */
static void tcp_abort_conn(struct tcp_sock *tp)
{
	if (tcp_need_reset(tp->state))
		tcp_send_active_reset(tp);
	tcp_write_queue_purge(tp);
	tcp_rcv_purge(tp);
	tcp_done(tp);
}

/* Linux __tcp_close() */
static void tcp_v4_close(struct rt_socket *so)
{
	struct tcp_sock *tp = so->pcb;
	u32 data_was_unread;
	int linger0;

	if (!tp)
		return;
	tcp_enter();
	tcp_sync_rcvbuf(tp);
	tp->shutdown = SHUTDOWN_MASK;
	linger0 = so->linger_on && so->linger_s == 0;
	/* Linux: closing a child that was never accepted resets it. */
	if (so->listen_parent)
		linger0 = 1;

	if (tp->state == RT_TCPS_LISTEN) {
		tcp_set_state(tp, RT_TCPS_CLOSED);
		tcp_synq_purge(tp);
		goto adjudge_to_death;
	}
	data_was_unread = tp->rcvq_bytes;
	tcp_rcv_purge(tp);
	if (tp->state == RT_TCPS_CLOSED)
		goto adjudge_to_death;
	if (data_was_unread) {
		/* RFC 2525 2.17: data was lost, send RST */
		rt_tcpstat.TCPAbortOnClose++;
		tcp_set_state(tp, RT_TCPS_CLOSED);
		tcp_send_active_reset(tp);
	} else if (linger0) {
		rt_tcpstat.TCPAbortOnLinger++;
		tcp_abort_conn(tp);
	} else if (tcp_close_state(tp)) {
		tcp_send_fin(tp);
	}

adjudge_to_death:
	tp->so = NULL;
	so->pcb = NULL;
	tp->dead = 1;
	if (tp->state != RT_TCPS_CLOSED && !tp->destroyed) {
		tcp_orphan_count++;
		if (tp->state == RT_TCPS_FIN_WAIT_2) {
			tcp_time_wait(tp, RT_TCPS_FIN_WAIT_2, tp->linger2);
			goto out;
		}
		if (tcp_orphan_count > (u32)sysctl_tcp_max_orphans) {
			tcp_set_state(tp, RT_TCPS_CLOSED);
			tcp_send_active_reset(tp);
		}
	}
	if (tp->state == RT_TCPS_CLOSED)
		tcp_destroy_sock(tp);
out:
	tcp_exit();
}

static int tcp_v4_bind(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct tcp_sock *tp = so->pcb;
	rt_in6_addr addr, old;
	u16 port;
	int err = 0;

	if (!tp)
		return -RT_EINVAL;
	err = rt_sin_check(so, sin, tp->v6only);
	if (err)
		return err;
	tcp_enter();
	if (tp->sport || tp->state != RT_TCPS_CLOSED) {
		err = -RT_EINVAL;
		goto out;
	}
	addr = sin->sin6_addr;
	if (!rt_inaddr_is_any(&addr) && !rt_inet_is_local(&addr)) {
		err = -RT_EADDRNOTAVAIL;
		goto out;
	}
	if (rt_in6_is_linklocal(&addr)) {
		if (!sin->sin6_scope_id) {
			struct rt_netif *own = rt_in6_addr_owner(&addr, 1);
			tp->scope = own ? own->index : 0;
		} else {
			tp->scope = sin->sin6_scope_id;
		}
	}
	tp->reuse = (so->options & RT_SO_REUSEADDR) != 0;
	tp->reuseport = (so->options & RT_SO_REUSEPORT) != 0;
	port = sin->sin6_port;
	old = tp->saddr;
	tp->saddr = addr;
	if (port == 0) {
		err = tcp_autobind(tp);
		if (err)
			tp->saddr = old;
	} else if (tcp_bind_conflict(tp, port, &addr, 0)) {
		tp->saddr = old;
		err = -RT_EADDRINUSE;
	} else {
		tp->sport = port;
		tcp_bhash_insert(tp);
	}
	if (!err)
		so->state |= RT_SS_ISBOUND;
out:
	tcp_exit();
	return err;
}

static int tcp_v4_listen(struct rt_socket *so, int backlog)
{
	struct tcp_sock *tp = so->pcb;
	int err = 0;

	if (!tp)
		return -RT_EINVAL;
	tcp_enter();
	if (backlog < 1)
		backlog = 1;
	if (backlog > RT_SOMAXCONN)
		backlog = RT_SOMAXCONN;
	if (tp->state == RT_TCPS_LISTEN) {
		tp->max_ack_backlog = (u32)backlog;
		goto out;
	}
	if (tp->state != RT_TCPS_CLOSED) {
		err = -RT_EINVAL;
		goto out;
	}
	tcp_sync_rcvbuf(tp);
	if (!tp->sport) {
		tp->reuse = (so->options & RT_SO_REUSEADDR) != 0;
		tp->reuseport = (so->options & RT_SO_REUSEPORT) != 0;
		err = tcp_autobind(tp);
		if (err)
			goto out;
	} else if (tcp_bind_conflict(tp, tp->sport, &tp->saddr, 1)) {
		err = -RT_EADDRINUSE;
		goto out;
	}
	tp->max_ack_backlog = (u32)backlog;
	tcp_set_state(tp, RT_TCPS_LISTEN);
	tcp_lhash_insert(tp);
	so->state |= RT_SS_ISLISTENING | RT_SS_ISBOUND;
out:
	tcp_exit();
	return err;
}

/* Linux tcp_fastopen_defer_connect(): decide on the TFO option for the
 * SYN (tp->fo_len). 1: a cookie is known and the socket asked for TFO, so
 * the connect waits for data. */
static int tcp_fastopen_prepare(struct tcp_sock *tp)
{
	u8 len = 0;
	int r;

	tp->fo_len = TCP_FASTOPEN_NONE;
	if ((!(tp->fo_flags & TCP_FO_WANT) && !sysctl_tcp_fastopen_all) || tp->noopt)
		return 0;
	r = tcp_fastopen_lookup(&tp->daddr, tp->fo_cookie, &len, &tp->fo_mss);
	if (r < 0)
		return 0;
	tp->fo_len = r ? len : 0;
	return r > 0;
}

/* Linux tcp_v4_connect() */
static int tcp_v4_connect(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct tcp_sock *tp = so->pcb;
	struct rt_inet_route nh;
	rt_in6_addr daddr;
	u32 scope;
	int err, v4;

	if (!tp)
		return -RT_EINVAL;
	if (sin->sin6_family != (so->domain == RT_AF_INET6 ? RT_AF_INET6 : RT_AF_INET))
		return -RT_EAFNOSUPPORT;
	if (tp->v6only && rt_in6_is_v4mapped(&sin->sin6_addr))
		return -RT_ENETUNREACH;     /* as Linux */
	switch (tp->state) {
	case RT_TCPS_CLOSED:
		break;
	case RT_TCPS_SYN_SENT:
	case RT_TCPS_SYN_RECEIVED:
		return -RT_EALREADY;
	case RT_TCPS_LISTEN:
		return -RT_EOPNOTSUPP;
	default:
		return -RT_EISCONN;
	}
	if (sin->sin6_port == 0)
		return -RT_EADDRNOTAVAIL;
	daddr = sin->sin6_addr;
	v4 = rt_in6_is_v4mapped(&daddr);
	if (v4) {
		u32 d = rt_in6_v4(&daddr);
		if (d == RT_INADDR_ANY)
			daddr.w[3] = d = RT_INADDR_LOOPBACK;
		if (d == RT_INADDR_BROADCAST || (rt_ntohl(d) & 0xf0000000u) == 0xe0000000u)
			return -RT_ENETUNREACH;
	} else {
		if (rt_in6_is_unspec(&daddr))
			daddr = rt_in6addr_loopback;
		if (rt_in6_is_mcast(&daddr))
			return -RT_ENETUNREACH;
	}
	/* the bound address and the peer must be of one family */
	if (!rt_in6_is_unspec(&tp->saddr) && rt_in6_is_v4mapped(&tp->saddr) != v4)
		return -RT_EAFNOSUPPORT;
	scope = sin->sin6_scope_id ? sin->sin6_scope_id : tp->scope;
	if (!rt_in6_is_linklocal(&daddr))
		scope = rt_in6_is_linklocal(&tp->saddr) ? tp->scope : 0;
	tcp_enter();
	tcp_sync_rcvbuf(tp);
	err = rt_inet_route(&daddr, scope, &nh);
	if (err)
		goto out;
	if (v4 && nh.bcast) {
		err = -RT_ENETUNREACH;
		goto out;
	}
	if (rt_inaddr_is_any(&tp->saddr)) {
		tp->saddr = nh.src;
		if (rt_inaddr_is_any(&tp->saddr)) {
			err = -RT_EADDRNOTAVAIL;
			goto out;
		}
	}
	if (tp->ts_recent_stamp && !rt_in6_eq(&tp->daddr, &daddr)) {
		tp->ts_recent = 0;
		tp->ts_recent_stamp = 0;
		tp->write_seq = 0;
	}
	tp->daddr = daddr;
	tp->dport = sin->sin6_port;
	tp->net_hlen = v4 ? 20 : 40;
	if (rt_in6_is_linklocal(&daddr) && nh.netif)
		tp->scope = nh.netif->index;
	tcp_set_state(tp, RT_TCPS_SYN_SENT);
	err = tcp_hash_connect(tp);
	if (err) {
		tcp_set_state(tp, RT_TCPS_CLOSED);
		tp->dport = 0;
		if (tp->family == RT_AF_INET6)
			memset(&tp->daddr, 0, sizeof(tp->daddr));
		else
			rt_inaddr_v4any(&tp->daddr);
		goto out;
	}
	if (!tp->write_seq)
		tp->write_seq = tcp_secure_seq(&tp->saddr, &tp->daddr, tp->sport, tp->dport);
	tp->ts_offset = tcp_secure_ts_off(&tp->saddr, &tp->daddr);
	tcp_bg_classify(tp);
	tp->pmtu_cookie = (u16)(nh.mtu ? nh.mtu : v4 ? 576 : RT_IP6_MIN_MTU);
	so->state |= RT_SS_ISCONNECTING | RT_SS_ISBOUND;
	so->state &= ~(RT_SS_CANTRCVMORE | RT_SS_CANTSENDMORE);
	tp->shutdown = 0;
	tp->done = 0;
	if (tcp_fastopen_prepare(tp)) {
		/* TCP_FASTOPEN_CONNECT with a cookie: connect() returns, the
		 * SYN waits for the first send() and carries its data */
		tp->fo_flags |= TCP_FO_DEFER;
		so->state &= ~RT_SS_ISCONNECTING;
		so->state |= RT_SS_DEFERCONN;
		err = 0;
		goto out;
	}
	err = tcp_connect(tp);
	if (err) {
		so->state &= ~RT_SS_ISCONNECTING;
		tcp_set_state(tp, RT_TCPS_CLOSED);
		goto out;
	}
	err = -RT_EINPROGRESS;
out:
	tcp_exit();
	return err;
}

/* Queue the SYN of a deferred connect (data queued behind it goes along
 * when tcp_connect_xmit() sends it). 0 or -errno (connection failed). */
static int tcp_fastopen_start(struct tcp_sock *tp)
{
	struct rt_socket *so = tp->so;
	int err;

	tp->fo_flags &= (u8)~TCP_FO_DEFER;
	if (so) {
		so->state &= ~RT_SS_DEFERCONN;
		if (!(so->state & RT_SS_ISCONNECTED))
			so->state |= RT_SS_ISCONNECTING;
	}
	err = tcp_connect_init(tp);
	if (err) {
		tcp_done(tp);
		tcp_sock_error(tp, -err);
	}
	return err;
}

/* A deferred connect that is read from (or waited on for reading) before
 * anything was sent: send a SYN without data (the server may speak first). */
static void tcp_fastopen_kick(struct tcp_sock *tp)
{
	if ((tp->fo_flags & TCP_FO_DEFER) && !tcp_fastopen_start(tp))
		tcp_connect_xmit(tp);
}

/* Linux tcp_sendmsg_locked(), non-blocking */
static int tcp_v4_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                       int flags, const struct rt_sockaddr_in6 *to)
{
	struct tcp_sock *tp = so->pcb;
	u32 total, copied = 0, mss_now;
	int err, fo_syn = 0;

	if (!tp)
		return -RT_ENOTCONN;
	tcp_enter();
	tcp_sync_rcvbuf(tp);
	if (so->so_error) {
		err = -so->so_error;
		so->so_error = 0;
		goto out;
	}
	if ((flags & RT_MSG_FASTOPEN) && to && tp->state == RT_TCPS_CLOSED && !tp->ever_connected) {
		/* Linux tcp_sendmsg_fastopen(): connect with the data */
		tp->fo_flags |= TCP_FO_WANT;
		err = tcp_v4_connect(so, to);
		if (err && err != -RT_EINPROGRESS)
			goto out;
	}
	if (tp->fo_flags & TCP_FO_DEFER) {
		/* the first send() of a deferred connect: SYN with this data */
		err = tcp_fastopen_start(tp);
		if (err)
			goto out;
		fo_syn = 1;
	}
	if (tp->shutdown & SEND_SHUTDOWN) {
		err = -RT_EPIPE;
		goto out;
	}
	if (!fo_syn && (tp->state == RT_TCPS_SYN_SENT || tp->state == RT_TCPS_SYN_RECEIVED)) {
		err = -RT_EWOULDBLOCK;
		goto out;
	}
	if (!fo_syn && tp->state != RT_TCPS_ESTABLISHED && tp->state != RT_TCPS_CLOSE_WAIT) {
		err = tp->ever_connected ? -RT_EPIPE : -RT_ENOTCONN;
		goto out;
	}
	total = rt_iov_total(iov, iovcnt);
	if (total == 0) {
		if (fo_syn)
			tcp_connect_xmit(tp);
		err = 0;
		goto out;
	}
	mss_now = tcp_current_mss(tp);
	while (copied < total) {
		struct rt_buf *skb = tp->wq.tail;
		u32 used = tp->write_seq - tp->snd_una;
		u32 space = used < tp->sndbuf ? tp->sndbuf - used : 0;
		u32 copy = 0;

		if (tcp_mem_snd_blocked(tp))
			space = 0;             /* socket memory at net.inet.tcp.mem_high */

		if (space == 0) {
			tcp_chrono_start(tp, TCP_CHRONO_SNDBUF);
			break;
		}
		if (skb && tp->send_head && skb->len < mss_now &&
		    !(TCP_SKB_CB(skb)->tcp_flags & (TCPHDR_SYN | TCPHDR_FIN)) &&
		    !TCP_SKB_CB(skb)->eor) {
			copy = mss_now - skb->len;
			if (copy > rt_buf_tailroom(skb))
				copy = rt_buf_tailroom(skb);
		}
		if (copy == 0) {
			skb = tcp_stream_alloc_skb(tp, mss_now);
			if (!skb)
				break;
			TCP_SKB_CB(skb)->seq = TCP_SKB_CB(skb)->end_seq = tp->write_seq;
			TCP_SKB_CB(skb)->tcp_flags = TCPHDR_ACK;
			rt_bufq_tail(&tp->wq, skb);
			if (!tp->send_head)
				tp->send_head = skb;
			tp->wmem_queued += tcp_buf_truesize(skb);
			tcp_chrono_start(tp, TCP_CHRONO_BUSY);
			copy = mss_now;
			if (copy > rt_buf_tailroom(skb))
				copy = rt_buf_tailroom(skb);
		}
		if (copy > total - copied)
			copy = total - copied;
		if (copy > space)
			copy = space;
		{
			/* sum the payload while copying it: transmitting the
			 * segment then takes no pass over it (RT_BUFF_CSUM_PART) */
			u32 off = skb->len, sum;

			rt_iov_to_buf_csum(iov, iovcnt, copied, rt_buf_put(skb, copy), copy, &sum);
			if (off == 0) {
				skb->csum = sum;
				skb->flags |= RT_BUFF_CSUM_PART;
			} else if (skb->flags & RT_BUFF_CSUM_PART) {
				skb->csum = rt_csum_block_add(skb->csum, sum, off);
			}
		}
		TCP_SKB_CB(skb)->end_seq += copy;
		tp->write_seq += copy;
		tp->wq_bytes += copy;
		tp->wq.bytes += copy;
		copied += copy;
	}
	if (fo_syn) {
		/* the SYN goes now, with as much of the data as fits */
		tcp_connect_xmit(tp);
		err = copied ? (int)copied : -RT_EWOULDBLOCK;
	} else if (copied) {
		struct rt_buf *tail = tp->wq.tail;
		int nonagle = tp->nodelay ? TCP_NAGLE_OFF : 0;

		if (flags & RT_MSG_OOB)
			tp->snd_up = tp->write_seq;            /* tcp_mark_urg */
		if (tail && tp->send_head)
			tcp_mark_push(tp, tail);
		if (flags & RT_MSG_EOR)
			TCP_SKB_CB(tail)->eor = 1;
		if (tp->cork || (flags & RT_MSG_MORE))
			nonagle = TCP_NAGLE_CORK;
		if (flags & RT_MSG_OOB)
			nonagle = TCP_NAGLE_PUSH;
		__tcp_push_pending_frames(tp, mss_now, nonagle);
		/* TCP_CORK and MSG_MORE: a partial segment waits at most 200 ms */
		if (nonagle == TCP_NAGLE_CORK && (tp->cork_linux || (flags & RT_MSG_MORE)))
			tcp_arm_cork_timer(tp);
		err = (int)copied;
	} else {
		tp->write_space_wanted = 1;
		err = -RT_EWOULDBLOCK;
	}
out:
	tcp_exit();
	return err;
}

/* Linux tcp_recv_urg() */
static int tcp_recv_urg(struct tcp_sock *tp, const struct rt_iov *iov, int iovcnt,
                        int flags, int *msgflags)
{
	if (tp->oobinline || !tp->urg_data || tp->urg_data == TCP_URG_READ)
		return -RT_EINVAL;
	if (tp->state == RT_TCPS_CLOSED && !tp->done)
		return -RT_ENOTCONN;
	if (tp->urg_data & TCP_URG_VALID) {
		u8 c = (u8)tp->urg_data;
		if (!(flags & RT_MSG_PEEK))
			tp->urg_data = TCP_URG_READ;
		if (msgflags)
			*msgflags |= RT_MSG_OOB;
		if (rt_iov_total(iov, iovcnt) > 0) {
			rt_buf_to_iov(iov, iovcnt, 0, &c, 1);
			return 1;
		}
		if (msgflags)
			*msgflags |= RT_MSG_TRUNC;
		return 0;
	}
	if (tp->state == RT_TCPS_CLOSED || (tp->shutdown & RCV_SHUTDOWN))
		return 0;
	return -RT_EWOULDBLOCK;
}

static void tcp_rcvq_free_head(struct tcp_sock *tp)
{
	struct rt_buf *b = rt_bufq_dequeue(&tp->rcvq);
	u32 ts = tcp_buf_truesize(b);

	tp->rmem_alloc = tp->rmem_alloc > ts ? tp->rmem_alloc - ts : 0;
	rt_buf_free(b);
}

/* Linux tcp_recvmsg_locked(), non-blocking */
static int tcp_v4_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                       int flags, struct rt_sockaddr_in6 *from, int *msgflags)
{
	struct tcp_sock *tp = so->pcb;
	struct rt_buf *b;
	u32 len, copied = 0, seq;
	int peek = (flags & RT_MSG_PEEK) != 0;
	int err;

	if (!tp)
		return -RT_ENOTCONN;
	tcp_enter();
	tcp_sync_rcvbuf(tp);
	if (tp->state == RT_TCPS_LISTEN) {
		err = -RT_ENOTCONN;
		goto out;
	}
	tcp_fastopen_kick(tp);
	if (flags & RT_MSG_OOB) {
		err = tcp_recv_urg(tp, iov, iovcnt, flags, msgflags);
		goto out;
	}
	len = rt_iov_total(iov, iovcnt);
	seq = tp->copied_seq;
	b = tp->rcvq.head;
	while (copied < len && b) {
		struct tcp_skb_cb *cb = TCP_SKB_CB(b);
		u32 offset = seq - cb->seq, used;

		if (offset >= b->len) {
			struct rt_buf *nb = b->next;
			if (!peek)
				tcp_rcvq_free_head(tp);
			b = nb;
			continue;
		}
		/* Stop at the urgent mark (BSD/Linux semantics, SIOCATMARK) */
		if (copied && tp->urg_data && tp->urg_seq == seq)
			break;
		used = b->len - offset;
		if (used > len - copied)
			used = len - copied;
		if (tp->urg_data) {
			u32 urg_offset = tp->urg_seq - seq;
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!tp->oobinline) {
						seq++;
						if (!peek && tp->rcvq_bytes)
							tp->rcvq_bytes--;
						used--;
						offset++;
						if (!used)
							goto skip_copy;
					}
				} else {
					used = urg_offset;
				}
			}
		}
		rt_buf_to_iov(iov, iovcnt, copied, b->data + offset, used);
		seq += used;
		copied += used;
		if (!peek)
			tp->rcvq_bytes -= used;
skip_copy:
		if (tp->urg_data && rt_seq_after(seq, tp->urg_seq) && !peek) {
			tp->urg_data = 0;
			tcp_fast_path_check(tp);
		}
		if (seq - cb->seq >= b->len) {
			struct rt_buf *nb = b->next;
			if (!peek)
				tcp_rcvq_free_head(tp);
			b = nb;
		} else if (copied >= len) {
			break;
		}
	}
	if (!peek)
		tp->copied_seq = seq;
	if (copied) {
		if (!peek) {
			tcp_diag_unread_stop(tp);
			tcp_rcv_space_adjust(tp);
			tcp_cleanup_rbuf(tp, (int)copied);
		}
		err = (int)copied;
		goto out;
	}
	if (len == 0) {
		err = 0;
		goto out;
	}
	if (so->so_error) {
		err = -so->so_error;
		so->so_error = 0;
		goto out;
	}
	if (tp->done || (tp->shutdown & RCV_SHUTDOWN)) {
		err = 0;
		goto out;
	}
	if (tp->state == RT_TCPS_CLOSED) {
		err = tp->ever_connected ? 0 : -RT_ENOTCONN;
		goto out;
	}
	err = -RT_EWOULDBLOCK;
out:
	if (from && err >= 0 && tp)
		rt_sin_fill(so, from, &tp->daddr, tp->dport, tp->scope);
	tcp_exit();
	return err;
}

/* Linux tcp_shutdown() */
static int tcp_v4_shutdown(struct rt_socket *so, int how)
{
	struct tcp_sock *tp = so->pcb;
	int err = 0;

	if (!tp)
		return -RT_ENOTCONN;
	if (how < 0 || how > 2)
		return -RT_EINVAL;
	tcp_enter();
	tcp_fastopen_kick(tp);
	if (tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN) {
		if (tp->state == RT_TCPS_LISTEN && how != 1) {
			/* Linux: shutdown of a listener stops it */
			tcp_set_state(tp, RT_TCPS_CLOSED);
			tcp_synq_purge(tp);
		} else {
			err = -RT_ENOTCONN;
		}
		goto out;
	}
	if (how == 0 || how == 2) {
		tp->shutdown |= RCV_SHUTDOWN;
		so->state |= RT_SS_CANTRCVMORE;
	}
	if (how == 1 || how == 2) {
		tp->shutdown |= SEND_SHUTDOWN;
		so->state |= RT_SS_CANTSENDMORE;
		if (tp->state == RT_TCPS_SYN_SENT && (tp->fo_flags & TCP_FO_WANT)) {
			/* TCP Fast Open: connect() returned before the handshake
			 * and the request may already be queued: the FIN follows
			 * once the connection is up (tcp_finish_connect) */
			tp->fin_pending = 1;
		} else if (tp->state == RT_TCPS_SYN_SENT) {
			tcp_write_queue_purge(tp);
			tcp_done(tp);
		} else if (tcp_close_state(tp)) {
			tcp_send_fin(tp);
		}
	}
	rt_sock_wakeup(so, RT_SEV_READ | RT_SEV_WRITE);
out:
	tcp_exit();
	return err;
}

/* Chronograph and receive-side times up to now (ms) */
struct tcp_diag_times {
	u32 busy, rwnd, sndbuf, unread, zwin;
};

static void tcp_diag_times(const struct tcp_sock *tp, struct tcp_diag_times *t)
{
	u32 now = tcp_jiffies(), st[3];

	st[0] = tp->dg.chrono_stat[0];
	st[1] = tp->dg.chrono_stat[1];
	st[2] = tp->dg.chrono_stat[2];
	if (tp->dg.chrono_type > TCP_CHRONO_NONE)
		st[tp->dg.chrono_type - 1] += now - tp->dg.chrono_start;
	t->busy = st[0] + st[1] + st[2];
	t->rwnd = st[1];
	t->sndbuf = st[2];
	t->unread = tp->dg.unread_ms + (tp->dg.unread_start && tp->rcvq_bytes ? now - tp->dg.unread_start : 0);
	t->zwin = tp->dg.zwin_ms + (tp->dg.zwin_start ? now - tp->dg.zwin_start : 0);
}

static u32 tcp_ago(u32 then)
{
	return then ? tcp_jiffies() - then : 0;
}

static void tcp_fill_ctl_info(struct tcp_sock *tp, struct rt_ctl_tcpinfo *r)
{
	struct tcp_diag_times t;
	const struct rt_buf *b;

	memset(r, 0, sizeof(*r));
	r->family = tp->family;
	r->state = tp->state;
	r->ca_state = tp->ca_state;
	if (tp->rx_opt.tstamp_ok)
		r->options |= RT_CTL_TCPI_TS;
	if (tp->rx_opt.sack_ok)
		r->options |= RT_CTL_TCPI_SACK;
	if (tp->rx_opt.wscale_ok) {
		r->options |= RT_CTL_TCPI_WSCALE;
		r->snd_wscale = tp->rx_opt.snd_wscale;
		r->rcv_wscale = tp->rx_opt.rcv_wscale;
	}
	if (tp->ecn_flags & TCP_ECN_OK)
		r->options |= RT_CTL_TCPI_ECN;
	r->backoff = tp->icsk_backoff;
	r->lport = rt_ntohs(tp->sport);
	r->fport = rt_ntohs(tp->dport);
	memcpy(r->laddr, tp->saddr.b, 16);
	memcpy(r->faddr, tp->daddr.b, 16);
	r->srtt_us = tp->srtt_us >> 3;
	r->rttvar_us = tp->mdev_us >> 2;
	r->min_rtt_us = tcp_min_rtt(tp);
	r->rto_us = tp->icsk_rto * 1000u;
	r->snd_mss = tp->mss_cache;
	r->rcv_mss = tp->ack.rcv_mss;
	r->advmss = tp->advmss;
	r->cwnd = tp->snd_cwnd;
	r->ssthresh = tp->snd_ssthresh;
	r->snd_wnd = tp->snd_wnd;
	r->rcv_wnd = tp->rcv_wnd;
	r->rcv_ssthresh = tp->rcv_ssthresh;
	r->sndbuf = tp->sndbuf;
	r->rcvbuf = tp->rcvbuf;
	r->sendq = tp->state == RT_TCPS_LISTEN ? 0 : tp->write_seq - tp->snd_una;
	r->recvq = tp->state == RT_TCPS_LISTEN ? tp->synq_len : tp->rcvq_bytes;
	r->ofo_bytes = tp->ofo_bytes;
	for (b = tp->ofoq.head; b; b = b->next)
		r->ofo_segs++;
	r->packets_out = tp->packets_out;
	r->retrans_out = tp->retrans_out;
	r->sacked_out = tp->sacked_out;
	r->lost_out = tp->lost_out;
	r->total_retrans = tp->total_retrans;
	r->fast_recoveries = tp->dg.fast_recoveries;
	r->rto_timeouts = tp->dg.rto_timeouts;
	r->tlp_probes = tp->dg.tlp_probes;
	r->dupacks_in = tp->dg.dupacks_in;
	r->dupacks_out = tp->dg.dupacks_out;
	r->sack_blocks_out = tp->dg.sack_blocks_out;
	r->acks_out = tp->dg.acks_out;
	r->delayed_acks_out = tp->dg.delayed_acks_out;
	r->zwin_out = tp->dg.zwin_out;
	r->zwin_in = tp->dg.zwin_in;
	r->segs_in = tp->segs_in;
	r->segs_out = tp->segs_out;
	r->data_segs_in = tp->data_segs_in;
	r->data_segs_out = tp->data_segs_out;
	r->bytes_received_hi = (u32)(tp->bytes_received >> 32);
	r->bytes_received_lo = (u32)tp->bytes_received;
	r->bytes_acked_hi = (u32)(tp->bytes_acked >> 32);
	r->bytes_acked_lo = (u32)tp->bytes_acked;
	tcp_diag_times(tp, &t);
	r->busy_ms = t.busy;
	r->rwnd_limited_ms = t.rwnd;
	r->sndbuf_limited_ms = t.sndbuf;
	r->unread_ms = t.unread;
	r->zwin_ms = t.zwin;
	r->age_ms = tcp_ago(tp->dg.estab_ms);
	if (tp->bytes_received)
		r->last_data_recv_ago = tcp_ago(tp->ack.lrcvtime);
	if (tp->data_segs_out)
		r->last_data_sent_ago = tcp_ago(tp->lsndtime);
	r->last_ack_recv_ago = tcp_ago(tp->rcv_tstamp);
	rt_strlcpy(r->cc, tp->ca_ops ? tp->ca_ops->name : "", sizeof(r->cc));
	r->bg = (u8)((tp->bg_want & TCP_BG_RECV ? RT_CTL_TCPBG_RECV : 0) |
	             (tp->bg_want & TCP_BG_SEND ? RT_CTL_TCPBG_SEND : 0) |
	             (tp->rl ? RT_CTL_TCPBG_RL_ON : 0) |
	             (tp->bg_want && !tp->bg_explicit ? RT_CTL_TCPBG_RULE : 0));
	if (tp->rl || tp->ca_ops == &tcp_ledbat) {
		const struct ledbat *lb = tp->rl ? &tp->rl->lb : (const struct ledbat *)tp->ca_priv;
		u32 base = ledbat_base(lb);
		r->ledbat_phase = lb->phase;
		r->ledbat_qdelay_us = lb->qdelay;
		r->ledbat_base_us = base == 0xffffffffu ? 0 : base;
		if (tp->rl)
			r->rledbat_wnd = tcp_rledbat_window(tp);
	}
}

/* net.riptide.tcpinfo (ctl.c): returns the total count */
int rt_tcp_info_list(struct rt_ctl_tcpinfo *out, int max)
{
	struct tcp_sock *tp;
	int n = 0;

	for (tp = tcp_all; tp; tp = tp->all_next) {
		if (tp->destroyed)
			continue;
		if (n < max && out)
			tcp_fill_ctl_info(tp, &out[n]);
		n++;
	}
	return n;
}

static void tcp_get_info(struct tcp_sock *tp, struct rt_tcp_info *info)
{
	struct tcp_diag_times t;

	memset(info, 0, sizeof(*info));
	info->tcpi_state = tp->state;
	info->tcpi_ca_state = tp->ca_state;
	info->tcpi_retransmits = tp->icsk_retransmits;
	info->tcpi_backoff = tp->icsk_backoff;
	if (tp->rx_opt.tstamp_ok)
		info->tcpi_options |= 1;
	if (tp->rx_opt.sack_ok)
		info->tcpi_options |= 2;
	if (tp->rx_opt.wscale_ok) {
		info->tcpi_options |= 4;
		info->tcpi_snd_wscale = tp->rx_opt.snd_wscale;
		info->tcpi_rcv_wscale = tp->rx_opt.rcv_wscale;
	}
	if (tp->ecn_flags & TCP_ECN_OK)
		info->tcpi_options |= 8;
	if (tp->fo_flags & TCP_FO_DATA_ACKED)
		info->tcpi_options |= 32;      /* Linux TCPI_OPT_SYN_DATA */
	info->tcpi_rto_us = tp->icsk_rto * 1000u;
	info->tcpi_snd_mss = tp->mss_cache;
	info->tcpi_rcv_mss = tp->ack.rcv_mss;
	info->tcpi_unacked = tp->packets_out;
	info->tcpi_sacked = tp->sacked_out;
	info->tcpi_lost = tp->lost_out;
	info->tcpi_retrans = tp->retrans_out;
	info->tcpi_rtt_us = tp->srtt_us >> 3;
	info->tcpi_rttvar_us = tp->mdev_us >> 2;
	info->tcpi_min_rtt_us = tcp_min_rtt(tp);
	info->tcpi_snd_ssthresh = tp->snd_ssthresh;
	info->tcpi_snd_cwnd = tp->snd_cwnd;
	info->tcpi_rcv_space = tp->rcvq_space.space;
	info->tcpi_snd_wnd = tp->snd_wnd;
	info->tcpi_rcv_wnd = tp->rcv_wnd;
	info->tcpi_total_retrans = tp->total_retrans;
	info->tcpi_bytes_acked = tp->bytes_acked;
	info->tcpi_bytes_received = tp->bytes_received;
	info->tcpi_segs_out = tp->segs_out;
	info->tcpi_segs_in = tp->segs_in;
	info->tcpi_reordering = tp->reordering;
	rt_strlcpy(info->tcpi_ca_name, tp->ca_ops->name, sizeof(info->tcpi_ca_name));
	tcp_diag_times(tp, &t);
	info->tcpi_busy_ms = t.busy;
	info->tcpi_rwnd_limited_ms = t.rwnd;
	info->tcpi_sndbuf_limited_ms = t.sndbuf;
	info->tcpi_unread_ms = t.unread;
	info->tcpi_zwin_ms = t.zwin;
	info->tcpi_fast_recoveries = tp->dg.fast_recoveries;
	info->tcpi_rto_timeouts = tp->dg.rto_timeouts;
	info->tcpi_tlp_probes = tp->dg.tlp_probes;
	info->tcpi_dupacks_in = tp->dg.dupacks_in;
	info->tcpi_dupacks_out = tp->dg.dupacks_out;
	info->tcpi_sack_blocks_out = tp->dg.sack_blocks_out;
	info->tcpi_acks_out = tp->dg.acks_out;
	info->tcpi_delayed_acks_out = tp->dg.delayed_acks_out;
	info->tcpi_zwin_out = tp->dg.zwin_out;
	info->tcpi_zwin_in = tp->dg.zwin_in;
	info->tcpi_data_segs_in = tp->data_segs_in;
	info->tcpi_data_segs_out = tp->data_segs_out;
	info->tcpi_ofo_bytes = tp->ofo_bytes;
	info->tcpi_rcvbuf = tp->rcvbuf;
	info->tcpi_sndbuf = tp->sndbuf;
}

static int get_int(const void *val, u32 len, s32 *out)
{
	if (!val || len < sizeof(s32)) {
		if (val && len == 1) {
			*out = *(const u8 *)val;
			return 0;
		}
		return -RT_EINVAL;
	}
	memcpy(out, val, sizeof(s32));
	return 0;
}

static int tcp_v4_setsockopt(struct rt_socket *so, int level, int name, const void *val, u32 len)
{
	struct tcp_sock *tp = so->pcb;
	s32 v = 0;
	int err = 0;

	if (!tp)
		return -RT_EINVAL;
	tcp_enter();
	if (level == RT_SOL_SOCKET) {
		/* The socket layer stored the value; react to it. An explicit
		 * SO_KEEPALIVE replaces net.inet.tcp.keepalive_all. */
		if (name == RT_SO_KEEPALIVE)
			tp->ka_auto = 0;
		tcp_sync_rcvbuf(tp);
		goto out;
	}
	if (level == RT_IPPROTO_IPV6 && tp->family == RT_AF_INET6) {
		err = get_int(val, len, &v);
		if (err)
			goto out;
		switch (name) {
		case RT_IPV6_V6ONLY:
			if (tp->sport || tp->state != RT_TCPS_CLOSED)
				err = -RT_EINVAL;
			else
				tp->v6only = v != 0;
			break;
		case RT_IPV6_UNICAST_HOPS:
			if (v < -1 || v > 255)
				err = -RT_EINVAL;
			else
				tp->ttl = (u8)(v < 0 ? 0 : v);
			break;
		case RT_IPV6_TCLASS:
			if (v < -1 || v > 255)
				err = -RT_EINVAL;
			else
				tp->tos = (u8)((v < 0 ? 0 : v) & ~INET_ECN_MASK);
			break;
		default:
			err = -RT_ENOPROTOOPT;
			break;
		}
		goto out;
	}
	if (level == RT_IPPROTO_IP) {
		if (name != RT_IP_TOS && name != RT_IP_TTL) {
			err = -RT_ENOPROTOOPT;
			goto out;
		}
		err = get_int(val, len, &v);
		if (err)
			goto out;
		if (name == RT_IP_TOS) {
			tp->tos = (u8)(v & ~INET_ECN_MASK);
		} else {
			if (v < 0 || v > 255) {
				err = -RT_EINVAL;
				goto out;
			}
			tp->ttl = (u8)v;
		}
		goto out;
	}
	if (level != RT_IPPROTO_TCP) {
		err = -RT_ENOPROTOOPT;
		goto out;
	}
	if (name == RT_TCP_CONGESTION) {
		char nm[16];
		u32 n = len < sizeof(nm) - 1 ? len : sizeof(nm) - 1;
		if (!val) {
			err = -RT_EINVAL;
			goto out;
		}
		memcpy(nm, val, n);
		nm[n] = 0;
		err = tcp_ca_select(tp, nm);
		if (!err)
			tp->ca_user = 1;
		goto out;
	}
	err = get_int(val, len, &v);
	if (err)
		goto out;
	switch (name) {
	case RT_TCP_NODELAY:
		tp->nodelay = v != 0;
		if (v)
			tcp_push_pending_frames(tp);
		break;
	case RT_TCP_NOPUSH:
	case RT_TCP_CORK:
		tp->cork = v != 0;
		tp->cork_linux = name == RT_TCP_CORK && v;
		if (!v) {
			rt_timer_del(&tp->cork_timer);
			tcp_push_pending_frames(tp);
		}
		break;
	case RT_TCP_FASTOPEN_CONNECT:
	case RT_TCP_FASTOPEN:
		/* TCP_FASTOPEN on a listener (the server side) is not offered */
		if (tp->state == RT_TCPS_LISTEN) {
			err = -RT_EOPNOTSUPP;
			break;
		}
		if (tp->state != RT_TCPS_CLOSED) {
			err = -RT_EISCONN;
			break;
		}
		if (v)
			tp->fo_flags |= TCP_FO_WANT;
		else
			tp->fo_flags &= (u8)~TCP_FO_WANT;
		break;
	case RT_TCP_NOOPT:
		tp->noopt = v != 0;
		break;
	case RT_TCP_MAXSEG:
		if (v && (v < TCP_MIN_MSS || v > 65535)) {
			err = -RT_EINVAL;
			break;
		}
		tp->rx_opt.user_mss = (u16)v;
		break;
	case RT_TCP_KEEPIDLE:
		if (v < 1 || v > 32767) {
			err = -RT_EINVAL;
			break;
		}
		tp->keepalive_time = (u32)v * 1000u;
		if (tp->keepopen && tp->state != RT_TCPS_CLOSED && tp->state != RT_TCPS_LISTEN &&
		    tp->state != RT_TCPS_SYN_SENT)
			tcp_reset_keepalive_timer(tp, tp->keepalive_time);
		break;
	case RT_TCP_KEEPINTVL:
		if (v < 1 || v > 32767) {
			err = -RT_EINVAL;
			break;
		}
		tp->keepalive_intvl = (u32)v * 1000u;
		break;
	case RT_TCP_KEEPCNT:
		if (v < 1 || v > 127) {
			err = -RT_EINVAL;
			break;
		}
		tp->keepalive_probes = (u32)v;
		break;
	case RT_TCP_KEEPINIT:
		if (v < 0) {
			err = -RT_EINVAL;
			break;
		}
		tp->keepinit = (u32)v;
		break;
	case RT_TCP_QUICKACK:
		if (v) {
			tp->pingpong = 0;
			if (tp->ack.pending & ICSK_ACK_SCHED)
				tcp_send_ack(tp);
			tcp_enter_quickack_mode(tp, TCP_MAX_QUICKACKS);
		} else {
			tp->pingpong = 1;
		}
		break;
	case RT_TCP_USER_TIMEOUT:
		if (v < 0) {
			err = -RT_EINVAL;
			break;
		}
		tp->icsk_user_timeout = (u32)v;
		break;
	case RT_TCP_BACKGROUND:
		if (v < 0 || v > 3)
			err = -RT_EINVAL;
		else
			tcp_bg_set(tp, (u32)v);
		break;
	default:
		err = -RT_ENOPROTOOPT;
		break;
	}
out:
	tcp_exit();
	return err;
}

static int put_int(void *val, u32 *len, s32 v)
{
	if (!val || !len || *len < sizeof(s32))
		return -RT_EINVAL;
	memcpy(val, &v, sizeof(s32));
	*len = sizeof(s32);
	return 0;
}

static int tcp_v4_getsockopt(struct rt_socket *so, int level, int name, void *val, u32 *len)
{
	struct tcp_sock *tp = so->pcb;
	int err = -RT_ENOPROTOOPT;

	if (!tp)
		return -RT_EINVAL;
	tcp_enter();
	tcp_sync_rcvbuf(tp);
	if (level == RT_IPPROTO_IP) {
		if (name == RT_IP_TOS)
			err = put_int(val, len, tp->tos);
		else if (name == RT_IP_TTL)
			err = put_int(val, len, tp->ttl ? tp->ttl : 64);
		goto out;
	}
	if (level == RT_IPPROTO_IPV6 && tp->family == RT_AF_INET6) {
		if (name == RT_IPV6_V6ONLY)
			err = put_int(val, len, tp->v6only);
		else if (name == RT_IPV6_UNICAST_HOPS)
			err = put_int(val, len, tp->ttl ? tp->ttl : rt_ip6_hlim);
		else if (name == RT_IPV6_TCLASS)
			err = put_int(val, len, tp->tos);
		goto out;
	}
	if (level != RT_IPPROTO_TCP)
		goto out;
	switch (name) {
	case RT_TCP_NODELAY:  err = put_int(val, len, tp->nodelay); break;
	case RT_TCP_NOPUSH:   err = put_int(val, len, tp->cork && !tp->cork_linux); break;
	case RT_TCP_CORK:     err = put_int(val, len, tp->cork_linux); break;
	case RT_TCP_FASTOPEN_CONNECT:
	case RT_TCP_FASTOPEN: err = put_int(val, len, (tp->fo_flags & TCP_FO_WANT) != 0); break;
	case RT_TCP_NOOPT:    err = put_int(val, len, tp->noopt); break;
	case RT_TCP_MAXSEG:
		err = put_int(val, len, tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN ?
		              (tp->rx_opt.user_mss ? tp->rx_opt.user_mss : sysctl_tcp_mssdflt) :
		              (s32)tp->mss_cache);
		break;
	case RT_TCP_KEEPIDLE:  err = put_int(val, len, keepalive_time_when(tp) / 1000); break;
	case RT_TCP_KEEPINTVL: err = put_int(val, len, (s32)(keepalive_intvl_when(tp) / 1000u)); break;
	case RT_TCP_KEEPCNT:   err = put_int(val, len, (s32)keepalive_probes(tp)); break;
	case RT_TCP_KEEPINIT:  err = put_int(val, len, (s32)tp->keepinit); break;
	case RT_TCP_QUICKACK:  err = put_int(val, len, !tp->pingpong); break;
	case RT_TCP_USER_TIMEOUT: err = put_int(val, len, (s32)tp->icsk_user_timeout); break;
	case RT_TCP_BACKGROUND: err = put_int(val, len, (s32)tp->bg_want); break;
	case RT_TCP_CONGESTION: {
		u32 n = (u32)strlen(tp->ca_ops->name) + 1;
		if (!val || !len) {
			err = -RT_EINVAL;
			break;
		}
		if (n > *len)
			n = *len;
		memcpy(val, tp->ca_ops->name, n);
		*len = n;
		err = 0;
		break;
	}
	case RT_TCP_INFO: {
		struct rt_tcp_info info;
		u32 n = sizeof(info);
		if (!val || !len) {
			err = -RT_EINVAL;
			break;
		}
		tcp_get_info(tp, &info);
		if (n > *len)
			n = *len;
		memcpy(val, &info, n);
		*len = n;
		err = 0;
		break;
	}
	default:
		break;
	}
out:
	tcp_exit();
	return err;
}

/* Linux tcp_poll() */
static u32 tcp_v4_poll(struct rt_socket *so)
{
	struct tcp_sock *tp = so->pcb;
	u32 mask = 0;
	int state;

	if (!tp)
		return RT_POLLHUP | RT_POLLERR;
	tcp_sync_rcvbuf(tp);
	state = tp->state;
	if (state == RT_TCPS_LISTEN)
		return so->acceptq_head ? RT_POLLIN : 0;
	if (tp->fo_flags & TCP_FO_DEFER)
		return RT_POLLOUT;          /* deferred connect: waits for data */
	/* Never connected (a connection that ended has shutdown set by
	 * tcp_done(), a failed one so_error): BSD/AmiTCP soreadable() and
	 * sowriteable() are false, so WaitSelect() reports nothing. Linux
	 * would say POLLOUT | POLLHUP. */
	if (state == RT_TCPS_CLOSED && !tp->shutdown && !so->so_error)
		return 0;
	if (tp->shutdown == SHUTDOWN_MASK || state == RT_TCPS_CLOSED)
		mask |= RT_POLLHUP;
	if (tp->shutdown & RCV_SHUTDOWN)
		mask |= RT_POLLIN;
	if (state != RT_TCPS_SYN_SENT && state != RT_TCPS_SYN_RECEIVED) {
		u32 target = so->rcvlowat ? so->rcvlowat : 1;
		u32 avail = tp->rcvq_bytes;
		if (tp->urg_data && !tp->oobinline && (tp->urg_data & TCP_URG_VALID) && avail)
			avail--;
		if (avail >= target || (avail && tp->done))
			mask |= RT_POLLIN;
		if (!(tp->shutdown & SEND_SHUTDOWN)) {
			if (tcp_stream_is_writeable(tp))
				mask |= RT_POLLOUT;
			else
				tp->write_space_wanted = 1;
		} else {
			mask |= RT_POLLOUT;
		}
		if (tp->urg_data & TCP_URG_VALID)
			mask |= RT_POLLPRI;
	}
	if (so->so_error)
		mask |= RT_POLLERR;
	return mask;
}

static int tcp_v4_getname(struct rt_socket *so, struct rt_sockaddr_in6 *sin, int peer)
{
	struct tcp_sock *tp = so->pcb;

	if (!tp)
		return -RT_EINVAL;
	if (peer) {
		if (!tp->dport || tp->state == RT_TCPS_CLOSED || tp->state == RT_TCPS_LISTEN ||
		    (tp->state == RT_TCPS_SYN_SENT && !(tp->fo_flags & TCP_FO_DEFER)))
			return -RT_ENOTCONN;
		rt_sin_fill(so, sin, &tp->daddr, tp->dport, tp->scope);
	} else {
		rt_sin_fill(so, sin, &tp->saddr, tp->sport, tp->scope);
	}
	return 0;
}

static int tcp_v4_ioctl(struct rt_socket *so, u32 req, void *arg)
{
	struct tcp_sock *tp = so->pcb;
	s32 v;

	if (!tp)
		return -RT_EINVAL;
	if (req == RT_SIOC_KICKCONN) {
		tcp_enter();
		tcp_fastopen_kick(tp);
		tcp_exit();
		return 0;
	}
	if (req == RT_FIONREAD) {
		if (tp->state == RT_TCPS_LISTEN)
			return -RT_EINVAL;
		v = (s32)tp->rcvq_bytes;
		if (v && tp->urg_data && !tp->oobinline &&
		    !rt_seq_before(tp->urg_seq, tp->copied_seq) && rt_seq_before(tp->urg_seq, tp->rcv_nxt))
			v--;
		memcpy(arg, &v, sizeof(v));
		return 0;
	}
	if (req == RT_FIONWRITE) {
		/* written but not acknowledged yet (Linux SIOCOUTQ) */
		v = tp->state == RT_TCPS_LISTEN ? 0 : (s32)(tp->write_seq - tp->snd_una);
		memcpy(arg, &v, sizeof(v));
		return 0;
	}
	if (req == RT_SIOCATMARK) {
		v = tp->urg_data && tp->urg_seq == tp->copied_seq;
		memcpy(arg, &v, sizeof(v));
		return 0;
	}
	return -RT_EINVAL;
}

const struct rt_proto_ops rt_tcp_ops = {
	"tcp",
	tcp_v4_attach,
	tcp_v4_close,
	tcp_v4_bind,
	tcp_v4_listen,
	tcp_v4_connect,
	tcp_v4_send,
	tcp_v4_recv,
	tcp_v4_shutdown,
	tcp_v4_setsockopt,
	tcp_v4_getsockopt,
	tcp_v4_poll,
	tcp_v4_getname,
	tcp_v4_ioctl
};

/* ---- netstat ---- */
int rt_tcp_list(struct rt_pcb_info *out, int max)
{
	struct tcp_sock *tp;
	struct tcp_tw *tw;
	int n = 0;

	for (tp = tcp_all; tp; tp = tp->all_next) {
		if (tp->destroyed)
			continue;
		if (n < max && out) {
			struct rt_pcb_info *p = &out[n];
			memset(p, 0, sizeof(*p));
			p->proto = RT_IPPROTO_TCP;
			p->state = tp->state;
			p->lport = rt_ntohs(tp->sport);
			p->fport = rt_ntohs(tp->dport);
			p->laddr = rt_in6_is_v4mapped(&tp->saddr) ? rt_in6_v4(&tp->saddr) : 0;
			p->faddr = rt_in6_is_v4mapped(&tp->daddr) ? rt_in6_v4(&tp->daddr) : 0;
			p->v6 = tp->family == RT_AF_INET6;
			p->v6only = tp->v6only;
			p->laddr6 = tp->saddr;
			p->faddr6 = tp->daddr;
			p->sendq = tp->write_seq - tp->snd_una;
			p->recvq = tp->state == RT_TCPS_LISTEN ? tp->synq_len : tp->rcvq_bytes;
			p->rtt_us = tp->srtt_us >> 3;
			p->cwnd = tp->snd_cwnd;
			p->retrans = tp->total_retrans;
		}
		n++;
	}
	for (tw = tcp_tw_first(); tw; tw = tw->all_next) {
		if (n < max && out) {
			struct rt_pcb_info *p = &out[n];
			memset(p, 0, sizeof(*p));
			p->proto = RT_IPPROTO_TCP;
			p->state = tw->substate;
			p->lport = rt_ntohs(tw->sport);
			p->fport = rt_ntohs(tw->dport);
			p->laddr = rt_in6_is_v4mapped(&tw->saddr) ? rt_in6_v4(&tw->saddr) : 0;
			p->faddr = rt_in6_is_v4mapped(&tw->daddr) ? rt_in6_v4(&tw->daddr) : 0;
			p->v6 = !rt_in6_is_v4mapped(&tw->daddr);
			p->laddr6 = tw->saddr;
			p->faddr6 = tw->daddr;
		}
		n++;
	}
	return n;
}

/* ---- sysctl table ---- */
static int tcp_sysctl_rfc1323(const struct rt_sysctl *e, void *old, u32 *oldlen,
                              const void *newp, u32 newlen)
{
	s32 v = sysctl_tcp_timestamps && sysctl_tcp_window_scaling;

	if (old) {
		if (!oldlen || *oldlen < sizeof(v))
			return -RT_ENOMEM;
		memcpy(old, &v, sizeof(v));
		*oldlen = sizeof(v);
	} else if (oldlen) {
		*oldlen = sizeof(v);
	}
	if (newp) {
		if (newlen != sizeof(v))
			return -RT_EINVAL;
		memcpy(&v, newp, sizeof(v));
		if (v < 0 || v > 1)
			return -RT_EINVAL;
		sysctl_tcp_timestamps = v;
		sysctl_tcp_window_scaling = v;
	}
	return 0;
}

static int tcp_sysctl_cc(const struct rt_sysctl *e, void *old, u32 *oldlen,
                         const void *newp, u32 newlen)
{
	u32 n = (u32)strlen(sysctl_tcp_cc) + 1;

	if (old) {
		if (!oldlen || *oldlen < n)
			return -RT_ENOMEM;
		memcpy(old, sysctl_tcp_cc, n);
		*oldlen = n;
	} else if (oldlen) {
		*oldlen = n;
	}
	if (newp) {
		char nm[16];
		const struct tcp_congestion_ops *ca;
		u32 l = newlen < sizeof(nm) - 1 ? newlen : sizeof(nm) - 1;
		memcpy(nm, newp, l);
		nm[l] = 0;
		ca = tcp_ca_find(nm);
		if (!ca)
			return -RT_ENOENT;
		rt_strlcpy(sysctl_tcp_cc, ca->name, sizeof(sysctl_tcp_cc));
	}
	return 0;
}

#define TCP_MIB(n) 4, { RT_CTL_NET, RT_PF_INET, RT_IPPROTO_TCP, (n), 0, 0 }
#define NO_MIB     0, { 0, 0, 0, 0, 0, 0 }
#define INT_RW(name, mib, var, lo, hi, d) \
	{ name, RT_SYSCTL_INT, RT_SYSCTL_RW, mib, &var, lo, hi, NULL, d }

static const struct rt_sysctl tcp_sysctls[] = {
	{ "net.inet.tcp.rfc1323", RT_SYSCTL_INT, RT_SYSCTL_RW, TCP_MIB(1), NULL, 0, 1,
	  tcp_sysctl_rfc1323, "RFC 7323 timestamps + window scaling" },
	INT_RW("net.inet.tcp.mssdflt", TCP_MIB(3), sysctl_tcp_mssdflt, 88, 65495, "Default MSS"),
	INT_RW("net.inet.tcp.keepidle", TCP_MIB(6), sysctl_tcp_keepidle, 1, 32767, "Keepalive idle (s)"),
	INT_RW("net.inet.tcp.keepintvl", TCP_MIB(7), sysctl_tcp_keepintvl, 1, 32767, "Keepalive interval (s)"),
	INT_RW("net.inet.tcp.timestamps", NO_MIB, sysctl_tcp_timestamps, 0, 1, "RFC 7323 timestamps"),
	INT_RW("net.inet.tcp.window_scaling", NO_MIB, sysctl_tcp_window_scaling, 0, 1, "RFC 7323 window scaling"),
	INT_RW("net.inet.tcp.sack", NO_MIB, sysctl_tcp_sack, 0, 1, "RFC 2018 SACK"),
	INT_RW("net.inet.tcp.dsack", NO_MIB, sysctl_tcp_dsack, 0, 1, "RFC 2883 D-SACK"),
	INT_RW("net.inet.tcp.ecn", NO_MIB, sysctl_tcp_ecn, 0, 2, "ECN: 1 request, 2 accept"),
	{ "net.inet.tcp.cc", RT_SYSCTL_STRING, RT_SYSCTL_RW, NO_MIB, sysctl_tcp_cc, 0,
	  sizeof(sysctl_tcp_cc), tcp_sysctl_cc, "Congestion control (cubic, reno, ledbat)" },
	INT_RW("net.inet.tcp.hystart", NO_MIB, sysctl_tcp_hystart, 0, 2,
	       "CUBIC slow start exit: 0 off, 1 HyStart, 2 HyStart++ (RFC 9406)"),
	INT_RW("net.inet.tcp.initcwnd", NO_MIB, sysctl_tcp_initcwnd, 1, 100, "Initial cwnd (RFC 6928)"),
	INT_RW("net.inet.tcp.rto_min_ms", NO_MIB, sysctl_tcp_rto_min_ms, 10, 2000, "Minimum RTO"),
	INT_RW("net.inet.tcp.syncookies", NO_MIB, sysctl_tcp_syncookies, 0, 2, "SYN cookies"),
	INT_RW("net.inet.tcp.synack_retries", NO_MIB, sysctl_tcp_synack_retries, 1, 255, "SYN-ACK retries"),
	INT_RW("net.inet.tcp.syn_retries", NO_MIB, sysctl_tcp_syn_retries, 1, 127, "SYN retries"),
	INT_RW("net.inet.tcp.retries1", NO_MIB, sysctl_tcp_retries1, 1, 255, "Retries before reroute"),
	INT_RW("net.inet.tcp.retries2", NO_MIB, sysctl_tcp_retries2, 1, 255, "Retries before abort"),
	INT_RW("net.inet.tcp.keepcnt", NO_MIB, sysctl_tcp_keepcnt, 1, 127, "Keepalive probes"),
	INT_RW("net.inet.tcp.fin_timeout", NO_MIB, sysctl_tcp_fin_timeout, 1, 3600, "Orphan FIN_WAIT_2 timeout (s)"),
	INT_RW("net.inet.tcp.tw_reuse", NO_MIB, sysctl_tcp_tw_reuse, 0, 2, "TIME_WAIT reuse for connect"),
	INT_RW("net.inet.tcp.frto", NO_MIB, sysctl_tcp_frto, 0, 2, "F-RTO (RFC 5682)"),
	INT_RW("net.inet.tcp.recovery", NO_MIB, sysctl_tcp_recovery, 0, 7, "Bit 1: RACK loss detection"),
	INT_RW("net.inet.tcp.reordering", NO_MIB, sysctl_tcp_reordering, 1, 300, "Initial reordering degree"),
	INT_RW("net.inet.tcp.rmem_max", NO_MIB, sysctl_tcp_rmem_max, 4096, 16777216, "Max auto-tuned receive buffer"),
	INT_RW("net.inet.tcp.wmem_max", NO_MIB, sysctl_tcp_wmem_max, 4096, 16777216, "Max send buffer"),
	INT_RW("net.inet.tcp.moderate_rcvbuf", NO_MIB, sysctl_tcp_moderate_rcvbuf, 0, 1, "Receive buffer auto-tuning"),
	INT_RW("net.inet.tcp.slow_start_after_idle", NO_MIB, sysctl_tcp_slow_start_after_idle, 0, 1, "Restart cwnd after idle"),
	INT_RW("net.inet.tcp.max_orphans", NO_MIB, sysctl_tcp_max_orphans, 0, 65536, "Max orphaned connections"),
	INT_RW("net.inet.tcp.max_tw_buckets", NO_MIB, sysctl_tcp_max_tw, 0, 65536, "Max TIME_WAIT records"),
	INT_RW("net.inet.tcp.delack_segs", NO_MIB, sysctl_tcp_delack_segs, 1, 64, "ACK every N full segments"),
	INT_RW("net.inet.tcp.delack_ms", NO_MIB, sysctl_tcp_delack_ms, 1, 500, "Max delayed ACK time (ms)"),
	INT_RW("net.inet.tcp.quickack", NO_MIB, sysctl_tcp_quickack, 0, 1, "ACK every segment at once"),
	INT_RW("net.inet.tcp.syn_rto_initial_ms", NO_MIB, sysctl_tcp_syn_rto_initial_ms, 200, 3000,
	       "First SYN retransmission timeout (ms)"),
	INT_RW("net.inet.tcp.syn_rto_max_ms", NO_MIB, sysctl_tcp_syn_rto_max_ms, 200, 120000,
	       "Largest SYN retransmission timeout (ms)"),
	INT_RW("net.inet.tcp.syn_backoff", NO_MIB, sysctl_tcp_syn_backoff, 0, 1,
	       "SYN timeout growth: 1 doubling, 0 linear"),
	INT_RW("net.inet.tcp.connect_timeout", NO_MIB, sysctl_tcp_connect_timeout, 0, 600,
	       "Give up connecting after N s (0: syn_retries only)"),
	INT_RW("net.inet.tcp.syn_icmp_abort", NO_MIB, sysctl_tcp_syn_icmp_abort, 0, 2,
	       "Unreachable while connecting: 0 wait, 1 abort on the 2nd, 2 abort"),
	INT_RW("net.inet.tcp.keepalive_all", NO_MIB, sysctl_tcp_keepalive_all, 0, 32767,
	       "Keepalive on every connection after N s idle (0 off)"),
};

void rt_tcp_init(void)
{
	rt_random_bytes(&isn_key, sizeof(isn_key));
	rt_random_bytes(&tsoff_key, sizeof(tsoff_key));
	rt_random_bytes(&cookie_key, sizeof(cookie_key));
	rt_random_bytes(&port_key, sizeof(port_key));
	ehash_seed = rt_random32();
	secs_last_ms = rt_now_ms();
	secs_now = 1;
	rt_sysctl_register(tcp_sysctls, (int)RT_ARRAY_SIZE(tcp_sysctls));
	tcp_fastopen_init();
	tcp_mtup_module_init();
	tcp_ledbat_init();
	tcp_mem_init();
}
