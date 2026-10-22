/*
 * Riptide - TCP passive open and TIME_WAIT: SYN queue of lightweight
 * request socks with SYN-ACK retransmission, SYN cookies, creation of
 * child connections for accept(), and compact time-wait records.
 * Follows Linux net/ipv4/tcp_minisocks.c, syncookies.c, tcp_ipv4.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "tcp.h"
#include "infra.h"

/* ======================================================================
 * SYN cookies (Linux syncookies.c)
 * cookie = H1(addrs) + their_isn + (minute_count << 24)
 *        + ((H2(addrs, count) + mss_index) & 0xffffff)
 * TCP options that do not fit are kept in the low 6 bits of our TSval.
 * ====================================================================== */
#define COOKIEBITS 24
#define COOKIEMASK ((1u << COOKIEBITS) - 1)
#define MAX_SYNCOOKIE_AGE 2
#define TSBITS 6
#define TS_OPT_WSCALE_MASK 0xf
#define TS_OPT_SACK (1u << 4)
#define TS_OPT_ECN  (1u << 5)
#define TCP_SYNCOOKIE_VALID 120u     /* s: accept cookies this long after overflow */

const u16 tcp_msstab[4] = { 536, 1300, 1440, 1460 };

static u32 tcp_cookie_time(void) { return tcp_seconds() / 64u; }

u32 tcp_cookie_init_seq(const rt_in6_addr *laddr, const rt_in6_addr *raddr, u16 lport, u16 rport,
                        u32 seq, u16 *mssp)
{
	u32 mssind, count = tcp_cookie_time();

	for (mssind = RT_ARRAY_SIZE(tcp_msstab) - 1; mssind; mssind--)
		if (*mssp >= tcp_msstab[mssind])
			break;
	*mssp = tcp_msstab[mssind];
	return tcp_cookie_hash(raddr, laddr, rport, lport, 0, 0) + seq + (count << COOKIEBITS) +
	       ((tcp_cookie_hash(raddr, laddr, rport, lport, count, 1) + mssind) & COOKIEMASK);
}

/* returns the MSS index, or -1 */
int tcp_cookie_check(u32 cookie, const rt_in6_addr *laddr, const rt_in6_addr *raddr, u16 lport,
                     u16 rport, u32 seq)
{
	u32 diff, count = tcp_cookie_time(), mssind;

	cookie -= tcp_cookie_hash(raddr, laddr, rport, lport, 0, 0) + seq;
	diff = (count - (cookie >> COOKIEBITS)) & ((u32)-1 >> COOKIEBITS);
	if (diff >= MAX_SYNCOOKIE_AGE)
		return -1;
	mssind = (cookie - tcp_cookie_hash(raddr, laddr, rport, lport, count - diff, 1)) & COOKIEMASK;
	if (mssind >= RT_ARRAY_SIZE(tcp_msstab))
		return -1;
	return (int)mssind;
}

static u32 cookie_init_timestamp(const struct tcp_request_sock *req)
{
	u32 ts, ts_now = tcp_jiffies(), options;

	options = req->wscale_ok ? req->snd_wscale : TS_OPT_WSCALE_MASK;
	if (req->sack_ok)
		options |= TS_OPT_SACK;
	if (req->ecn_ok)
		options |= TS_OPT_ECN;
	ts = (ts_now >> TSBITS) << TSBITS;
	ts |= options;
	if (rt_seq_after(ts, ts_now))
		ts -= 1u << TSBITS;
	return ts;
}

/* ======================================================================
 * request socks
 * ====================================================================== */
static void tcp_reqsk_free(struct tcp_request_sock *req)
{
	rt_timer_del(&req->timer);
	rt_free(req);
}

static void tcp_reqsk_unlink(struct tcp_sock *lp, struct tcp_request_sock *req)
{
	struct tcp_request_sock **pp = &lp->synq;

	while (*pp && *pp != req)
		pp = &(*pp)->next;
	if (*pp) {
		*pp = req->next;
		lp->synq_len--;
	}
}

void tcp_synq_purge(struct tcp_sock *lp)
{
	while (lp->synq) {
		struct tcp_request_sock *req = lp->synq;
		lp->synq = req->next;
		tcp_reqsk_free(req);
	}
	lp->synq_len = 0;
}

static struct tcp_request_sock *tcp_reqsk_find(struct tcp_sock *lp, const rt_in6_addr *raddr, u16 rport,
                                               const rt_in6_addr *laddr)
{
	struct tcp_request_sock *req;

	for (req = lp->synq; req; req = req->next)
		if (req->rmt_port == rport && rt_in6_eq(&req->rmt_addr, raddr) &&
		    (!laddr || rt_in6_eq(&req->loc_addr, laddr)))
			return req;
	return NULL;
}

/* Linux reqsk_timer_handler(): SYN-ACK retransmission 1 s, 2 s, 4 s ... */
static void tcp_reqsk_timer(void *arg)
{
	struct tcp_request_sock *req = arg;
	struct tcp_sock *lp = req->listener;
	u32 timeo;

	tcp_api_enter();
	if (req->num_retrans >= (u32)sysctl_tcp_synack_retries || lp->state != RT_TCPS_LISTEN) {
		tcp_reqsk_unlink(lp, req);
		tcp_reqsk_free(req);
		goto out;
	}
	if (!req->acked) {
		tcp_send_synack_req(req, tcp_jiffies() + req->ts_off);
		rt_tcpstat.RetransSegs++;
	}
	req->num_retrans++;
	timeo = TCP_TIMEOUT_INIT << RT_MIN(req->num_retrans, 7u);
	rt_timer_after(&req->timer, RT_MIN(timeo, TCP_RTO_MAX));
out:
	tcp_api_exit();
}

/* ICMP error for a SYN-ACK we sent (Linux tcp_req_err) */
void tcp_req_err(struct tcp_sock *lp, const rt_in6_addr *raddr, u16 rport, u32 seq, int abort)
{
	struct tcp_request_sock *req = tcp_reqsk_find(lp, raddr, rport, NULL);

	if (!req || seq != req->snt_isn)
		return;
	if (abort) {
		tcp_reqsk_unlink(lp, req);
		tcp_reqsk_free(req);
	}
}

u32 tcp_route_mtu(const rt_in6_addr *daddr, u32 scope)
{
	struct rt_inet_route rt;
	int v4 = rt_in6_is_v4mapped(daddr);

	if (rt_inet_route(daddr, scope, &rt) == 0 && rt.mtu >= (v4 ? 68u : RT_IP6_MIN_MTU))
		return rt.mtu;
	return v4 ? 576 : RT_IP6_MIN_MTU;
}

u32 tcp_route_window(const rt_in6_addr *daddr, u32 scope)
{
	struct rt_inet_route rt;

	if (rt_inet_route(daddr, scope, &rt) == 0 && rt.netif)
		return rt.netif->tcp_window;
	return 0;
}

static void tcp_req_window(struct tcp_sock *lp, struct tcp_request_sock *req)
{
	u32 clamp, limit = tcp_route_window(&req->rmt_addr, req->scope);

	if (lp->rcvbuf_locked || !sysctl_tcp_moderate_rcvbuf)
		clamp = tcp_full_space(lp);
	else
		clamp = RT_MAX((u32)sysctl_tcp_rmem_max, tcp_full_space(lp));
	if (limit && clamp > limit)
		clamp = limit;
	req->window_clamp = clamp;
	tcp_select_initial_window(tcp_full_space(lp),
	                          req->mss - (req->tstamp_ok ? TCPOLEN_TSTAMP_ALIGNED : 0),
	                          &req->rcv_wnd, &req->window_clamp, req->wscale_ok,
	                          &req->rcv_wscale);
	if (!req->wscale_ok)
		req->rcv_wscale = 0;
}

/* Linux tcp_conn_request(): a SYN for a listener */
static void tcp_conn_request(struct tcp_sock *lp, struct rt_buf *b, const rt_in6_addr *raddr,
                             const rt_in6_addr *laddr, u32 tw_isn)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_request_sock *req, local;
	struct tcp_rx_opt opt;
	u32 isn, tsval;
	u16 lport, rport, peer_mss;
	int want_cookie = 0;
	struct rt_socket *so = lp->so;

	if (!so)
		goto drop;
	memcpy(&rport, b->data, 2);
	memcpy(&lport, b->data + 2, 2);

	if (sysctl_tcp_syncookies == 2 || lp->synq_len >= lp->max_ack_backlog) {
		if (!sysctl_tcp_syncookies)
			goto drop;
		want_cookie = 1;
		lp->synq_overflow_ts = tcp_seconds();
	}
	if (so->acceptq_len > so->backlog) {
		rt_tcpstat.ListenOverflows++;
		goto drop;
	}

	memset(&opt, 0, sizeof(opt));
	opt.mss_clamp = TCP_MSS_DEFAULT;
	opt.user_mss = lp->rx_opt.user_mss;
	if (!lp->noopt)
		tcp_parse_options(b->data, cb->hdr_len, &opt, 0);
	if (want_cookie && !opt.saw_tstamp) {
		opt.wscale_ok = 0;
		opt.sack_ok = 0;
		opt.snd_wscale = 0;
	}
	peer_mss = opt.mss_clamp;

	if (want_cookie) {
		req = &local;
		memset(req, 0, sizeof(*req));
	} else {
		req = rt_zalloc(sizeof(*req));
		if (!req)
			goto drop;
	}
	req->listener = lp;
	req->loc_addr = *laddr;
	req->rmt_addr = *raddr;
	req->loc_port = lport;
	req->rmt_port = rport;
	req->scope = rt_in6_is_linklocal(raddr) && b->netif ? b->netif->index : 0;
	req->rcv_isn = cb->seq;
	req->tstamp_ok = opt.saw_tstamp;
	req->ts_recent = opt.saw_tstamp ? opt.rcv_tsval : 0;
	req->sack_ok = opt.sack_ok;
	req->wscale_ok = opt.wscale_ok;
	req->snd_wscale = opt.wscale_ok ? opt.snd_wscale : 0;
	req->ecn_ok = (cb->tcp_flags & TCPHDR_ECE) && (cb->tcp_flags & TCPHDR_CWR) && sysctl_tcp_ecn;
	req->tos = lp->tos;
	req->mss = (u16)(tcp_route_mtu(raddr, req->scope) - (rt_in6_is_v4mapped(raddr) ? 40 : 60));
	if (lp->rx_opt.user_mss && req->mss > lp->rx_opt.user_mss)
		req->mss = lp->rx_opt.user_mss;
	req->ts_off = tcp_secure_ts_off(laddr, raddr);
	tcp_req_window(lp, req);

	if (want_cookie) {
		isn = tcp_cookie_init_seq(laddr, raddr, lport, rport, cb->seq, &peer_mss);
		tsval = cookie_init_timestamp(req) + req->ts_off;
		rt_tcpstat.SyncookiesSent++;
	} else {
		isn = tw_isn ? tw_isn : tcp_secure_seq(laddr, raddr, lport, rport);
		tsval = tcp_jiffies() + req->ts_off;
	}
	req->snt_isn = isn;
	req->peer_mss = peer_mss;
	tcp_send_synack_req(req, tsval);
	if (want_cookie)
		return;
	rt_timer_init(&req->timer, tcp_reqsk_timer, req);
	rt_timer_after(&req->timer, TCP_TIMEOUT_INIT);
	req->next = lp->synq;
	lp->synq = req;
	lp->synq_len++;
	return;
drop:
	rt_tcpstat.ListenDrops++;
}

/* Linux tcp_create_openreq_child() + tcp_v4_syn_recv_sock(): build the
 * full connection once the handshake completed and queue it for
 * accept(). */
static struct tcp_sock *tcp_create_child(struct tcp_sock *lp, struct tcp_request_sock *req,
                                         const struct rt_buf *b)
{
	struct rt_socket *so;
	struct tcp_sock *tp;
	u32 mtu;

	if (!lp->so)
		return NULL;
	so = rt_sock_new_child(lp->so);
	if (!so) {
		rt_tcpstat.ListenOverflows++;
		rt_tcpstat.ListenDrops++;
		return NULL;
	}
	tp = tcp_sock_alloc();
	if (!tp) {
		rt_sock_child_drop(so);
		rt_tcpstat.ListenDrops++;
		return NULL;
	}
	tcp_init_sock(tp);
	tp->so = so;
	so->pcb = tp;
	tp->saddr = req->loc_addr;
	tp->daddr = req->rmt_addr;
	tp->sport = req->loc_port;
	tp->dport = req->rmt_port;
	tp->scope = req->scope;
	tp->family = lp->family;
	tp->v6only = lp->v6only;
	tp->net_hlen = rt_in6_is_v4mapped(&tp->daddr) ? 20 : 40;
	tp->reuse = lp->reuse;
	tp->reuseport = lp->reuseport;
	tp->nodelay = lp->nodelay;
	tp->cork = lp->cork;
	tp->noopt = lp->noopt;
	tp->rx_opt.user_mss = lp->rx_opt.user_mss;
	tp->keepalive_time = lp->keepalive_time;
	tp->keepalive_intvl = lp->keepalive_intvl;
	tp->keepalive_probes = lp->keepalive_probes;
	tp->keepinit = lp->keepinit;
	tp->icsk_user_timeout = lp->icsk_user_timeout;
	tp->tos = lp->tos;
	tp->ttl = lp->ttl;
	tp->ca_ops = lp->ca_ops;
	tp->linger2 = lp->linger2;
	tp->pingpong = lp->pingpong;
	tp->rcvbuf = RT_MAX(so->rcvbuf, 2048u);
	tp->sndbuf = RT_MAX(so->sndbuf, 2048u);
	tp->so_rcvbuf_seen = so->rcvbuf;
	tp->so_sndbuf_seen = so->sndbuf;
	tp->rcvbuf_locked = lp->rcvbuf_locked;
	tp->sndbuf_locked = lp->sndbuf_locked;
	tp->sndlowat = so->sndlowat;
	tp->keepopen = (so->options & RT_SO_KEEPALIVE) != 0;
	if (!tp->keepopen && sysctl_tcp_keepalive_all)
		tp->ka_auto = tp->keepopen = 1;      /* net.inet.tcp.keepalive_all */
	tp->oobinline = (so->options & RT_SO_OOBINLINE) != 0;

	tp->iss = req->snt_isn;
	tp->snd_una = tp->snd_nxt = tp->snd_sml = tp->snd_up = req->snt_isn + 1;
	tp->write_seq = tp->pushed_seq = req->snt_isn + 1;
	tp->irs = req->rcv_isn;
	tp->rcv_nxt = tp->rcv_wup = tp->copied_seq = req->rcv_isn + 1;
	tp->rx_opt.tstamp_ok = req->tstamp_ok;
	tp->rx_opt.sack_ok = req->sack_ok;
	tp->rx_opt.wscale_ok = req->wscale_ok;
	tp->rx_opt.snd_wscale = req->wscale_ok ? req->snd_wscale : 0;
	tp->rx_opt.rcv_wscale = req->wscale_ok ? req->rcv_wscale : 0;
	tp->ts_offset = req->ts_off;
	if (req->tstamp_ok) {
		tp->ts_recent = req->ts_recent;
		tp->ts_recent_stamp = tcp_seconds();
		tp->tcp_header_len = TCP_HDR_LEN + TCPOLEN_TSTAMP_ALIGNED;
	} else {
		tp->tcp_header_len = TCP_HDR_LEN;
	}
	tp->snd_wnd = (u32)TCP_SKB_CB(b)->window << tp->rx_opt.snd_wscale;
	tp->snd_wl1 = req->rcv_isn;
	tp->max_window = tp->snd_wnd;
	tp->rcv_wnd = req->rcv_wnd;
	tp->window_clamp = req->window_clamp;
	tp->window_limit = tcp_route_window(&tp->daddr, tp->scope);
	tp->rcv_ssthresh = req->rcv_wnd;
	tp->ecn_flags = req->ecn_ok ? TCP_ECN_OK : 0;
	tp->rx_opt.mss_clamp = req->peer_mss;
	mtu = tcp_route_mtu(&tp->daddr, tp->scope);
	tcp_sync_mss(tp, mtu);
	tp->advmss = req->mss;
	if (tp->rx_opt.tstamp_ok)
		tp->advmss -= TCPOLEN_TSTAMP_ALIGNED;
	tp->total_retrans = req->num_retrans;
	/* SYN-ACK RTT sample (Linux tcp_synack_rtt_meas) */
	tcp_mstamp_refresh();
	if (!req->num_retrans && req->snt_synack_us) {
		s32 us = (s32)(tcp_clock_us - req->snt_synack_us);
		tcp_ack_update_rtt(tp, FLAG_SYN_ACKED, us, -1, us);
	}
	tcp_set_state(tp, RT_TCPS_ESTABLISHED);
	tp->ack.lrcvtime = tcp_jiffies();
	tcp_bhash_insert(tp);
	tcp_ehash_insert(tp);
	tcp_init_transfer(tp);
	tp->rcv_tstamp = tcp_jiffies();
	tcp_initialize_rcv_mss(tp);
	tcp_fast_path_on(tp);
	so->state |= RT_SS_ISCONNECTED | RT_SS_ISBOUND;
	rt_tcpstat.PassiveOpens++;
	rt_sock_child_ready(lp->so, so);
	return tp;
}

static void tcp_req_ack(struct tcp_request_sock *req)
{
	tcp_send_ctl(&req->loc_addr, &req->rmt_addr, req->loc_port, req->rmt_port,
	             req->snt_isn + 1, req->rcv_isn + 1, TCPHDR_ACK,
	             (u16)(RT_MIN(req->rcv_wnd, TCP_MAX_WINDOW << req->rcv_wscale) >> req->rcv_wscale),
	             req->tstamp_ok, tcp_jiffies() + req->ts_off, req->ts_recent, req->tos, 0, req->scope);
}

/* Linux tcp_in_window() */
static int tcp_in_window(u32 seq, u32 end_seq, u32 s_win, u32 e_win)
{
	if (seq == s_win)
		return 1;
	if (rt_seq_after(end_seq, s_win) && rt_seq_before(seq, e_win))
		return 1;
	return seq == e_win && seq == end_seq;
}

/* Linux tcp_check_req(). Returns 1: send RST (b kept), 0: b consumed. */
static int tcp_check_req(struct tcp_sock *lp, struct tcp_request_sock *req, struct rt_buf *b)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_rx_opt opt;
	struct tcp_sock *child;
	u8 flg = cb->tcp_flags & (TCPHDR_SYN | TCPHDR_ACK | TCPHDR_RST | TCPHDR_FIN);
	int paws_reject = 0;

	memset(&opt, 0, sizeof(opt));
	opt.tstamp_ok = req->tstamp_ok;
	opt.sack_ok = req->sack_ok;
	if (cb->hdr_len > TCP_HDR_LEN) {
		tcp_parse_options(b->data, cb->hdr_len, &opt, 1);
		if (opt.saw_tstamp) {
			if (opt.rcv_tsecr)
				opt.rcv_tsecr -= req->ts_off;
			paws_reject = !(cb->tcp_flags & TCPHDR_RST) && req->ts_recent &&
			              (s32)(req->ts_recent - opt.rcv_tsval) > 0;
		}
	}
	/* retransmitted SYN: resend the SYN-ACK */
	if (cb->seq == req->rcv_isn && (flg & ~TCPHDR_FIN) == TCPHDR_SYN && !paws_reject) {
		tcp_send_synack_req(req, tcp_jiffies() + req->ts_off);
		rt_buf_free(b);
		return 0;
	}
	if ((flg & TCPHDR_ACK) && cb->ack_seq != req->snt_isn + 1)
		return 1;
	if (paws_reject || !tcp_in_window(cb->seq, cb->end_seq, req->rcv_isn + 1,
	                                  req->rcv_isn + 1 + req->rcv_wnd)) {
		if (!(flg & TCPHDR_RST))
			tcp_req_ack(req);
		if (paws_reject)
			rt_tcpstat.PAWSEstab++;
		rt_buf_free(b);
		return 0;
	}
	if (opt.saw_tstamp && !rt_seq_after(cb->seq, req->rcv_isn + 1))
		req->ts_recent = opt.rcv_tsval;
	if (cb->seq == req->rcv_isn)
		flg &= ~TCPHDR_SYN;
	if (flg & (TCPHDR_RST | TCPHDR_SYN)) {
		int rst = !(flg & TCPHDR_RST);
		rt_tcpstat.AttemptFails++;
		tcp_reqsk_unlink(lp, req);
		tcp_reqsk_free(req);
		if (rst)
			return 1;
		rt_buf_free(b);
		return 0;
	}
	if (!(flg & TCPHDR_ACK)) {
		rt_buf_free(b);
		return 0;
	}
	child = tcp_create_child(lp, req, b);
	if (!child) {
		/* accept queue full: forget the ACK, the SYN-ACK timer
		 * will retry (Linux without tcp_abort_on_overflow) */
		req->acked = 1;
		rt_buf_free(b);
		return 0;
	}
	tcp_reqsk_unlink(lp, req);
	tcp_reqsk_free(req);
	child->segs_in++;
	tcp_rcv_established(child, b);
	return 0;
}

/* Linux cookie_v4_check(). Returns 1: send RST (b kept), 0: consumed. */
static int tcp_cookie_check_ack(struct tcp_sock *lp, struct rt_buf *b, const rt_in6_addr *raddr,
                                const rt_in6_addr *laddr)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_request_sock req;
	struct tcp_rx_opt opt;
	struct tcp_sock *child;
	int mssind;

	if ((u32)(tcp_seconds() - lp->synq_overflow_ts) > TCP_SYNCOOKIE_VALID || !lp->synq_overflow_ts)
		return 1;
	memset(&req, 0, sizeof(req));
	memcpy(&req.rmt_port, b->data, 2);
	memcpy(&req.loc_port, b->data + 2, 2);
	mssind = tcp_cookie_check(cb->ack_seq - 1, laddr, raddr, req.loc_port, req.rmt_port, cb->seq - 1);
	if (mssind < 0) {
		rt_tcpstat.SyncookiesFailed++;
		return 1;
	}
	rt_tcpstat.SyncookiesRecv++;
	req.ts_off = tcp_secure_ts_off(laddr, raddr);
	memset(&opt, 0, sizeof(opt));
	opt.tstamp_ok = 1;
	tcp_parse_options(b->data, cb->hdr_len, &opt, 1);
	if (opt.saw_tstamp && opt.rcv_tsecr) {
		u32 options = (opt.rcv_tsecr - req.ts_off) & ((1u << TSBITS) - 1);
		if (!sysctl_tcp_timestamps)
			return 1;
		req.tstamp_ok = 1;
		req.ts_recent = opt.rcv_tsval;
		req.sack_ok = (options & TS_OPT_SACK) && sysctl_tcp_sack;
		req.ecn_ok = (options & TS_OPT_ECN) && sysctl_tcp_ecn;
		if ((options & TS_OPT_WSCALE_MASK) != TS_OPT_WSCALE_MASK && sysctl_tcp_window_scaling) {
			req.wscale_ok = 1;
			req.snd_wscale = (u8)(options & TS_OPT_WSCALE_MASK);
		}
	}
	req.listener = lp;
	req.loc_addr = *laddr;
	req.rmt_addr = *raddr;
	req.scope = rt_in6_is_linklocal(raddr) && b->netif ? b->netif->index : 0;
	req.snt_isn = cb->ack_seq - 1;
	req.rcv_isn = cb->seq - 1;
	req.peer_mss = tcp_msstab[mssind];
	req.tos = lp->tos;
	req.mss = (u16)(tcp_route_mtu(raddr, req.scope) - (rt_in6_is_v4mapped(raddr) ? 40 : 60));
	if (lp->rx_opt.user_mss && req.mss > lp->rx_opt.user_mss)
		req.mss = lp->rx_opt.user_mss;
	tcp_req_window(lp, &req);
	child = tcp_create_child(lp, &req, b);
	if (!child) {
		rt_buf_free(b);
		return 0;
	}
	child->segs_in++;
	tcp_rcv_established(child, b);
	return 0;
}

/* Segment for a listening socket. Returns 1 if the caller must answer
 * with RST (b not consumed); 0 if b was consumed. */
int tcp_listen_input(struct tcp_sock *lp, struct rt_buf *b, const rt_in6_addr *raddr,
                     const rt_in6_addr *laddr, u32 tw_isn)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_request_sock *req;
	u16 rport;
	u8 fl = cb->tcp_flags;

	memcpy(&rport, b->data, 2);
	req = tcp_reqsk_find(lp, raddr, rport, laddr);
	if (req)
		return tcp_check_req(lp, req, b);
	if (fl & TCPHDR_RST) {
		rt_buf_free(b);
		return 0;
	}
	if (fl & TCPHDR_ACK) {
		if (!(fl & TCPHDR_SYN) && sysctl_tcp_syncookies)
			return tcp_cookie_check_ack(lp, b, raddr, laddr);
		return 1;
	}
	if ((fl & TCPHDR_SYN) && !(fl & TCPHDR_FIN))
		tcp_conn_request(lp, b, raddr, laddr, tw_isn);
	rt_buf_free(b);
	return 0;
}

/* ======================================================================
 * TIME_WAIT (Linux inet_timewait_sock / tcp_timewait_state_process)
 * ====================================================================== */
#define TW_HASH_SIZE  256
#define TW_BHASH_SIZE 64
static struct tcp_tw *tw_ehash[TW_HASH_SIZE];
static struct tcp_tw *tw_bhash[TW_BHASH_SIZE];
static struct tcp_tw *tw_all;

static u32 tw_porthash(u16 port) { return ((u32)rt_ntohs(port) * 0x9e37u >> 3) & (TW_BHASH_SIZE - 1); }

struct tcp_tw *tcp_tw_first(void) { return tw_all; }

struct tcp_tw *tcp_tw_lookup(const rt_in6_addr *laddr, u16 lport, const rt_in6_addr *faddr, u16 fport)
{
	struct tcp_tw *tw = tw_ehash[tcp_tw_hashfn(laddr, lport, faddr, fport) & (TW_HASH_SIZE - 1)];

	for (; tw; tw = tw->ehash_next)
		if (tw->sport == lport && tw->dport == fport && rt_in6_eq(&tw->saddr, laddr) &&
		    rt_in6_eq(&tw->daddr, faddr))
			return tw;
	return NULL;
}

int tcp_tw_port_used(u16 port, const rt_in6_addr *addr, int v6only, int reuse)
{
	struct tcp_tw *tw = tw_bhash[tw_porthash(port)];

	if (reuse)
		return 0;
	for (; tw; tw = tw->bhash_next)
		if (tw->sport == port && (!addr || rt_inet_bind_overlap(addr, v6only, &tw->saddr, 1)))
			return 1;
	return 0;
}

void tcp_tw_kill(struct tcp_tw *tw)
{
	struct tcp_tw **pp;

	rt_timer_del(&tw->timer);
	pp = &tw_ehash[tcp_tw_hashfn(&tw->saddr, tw->sport, &tw->daddr, tw->dport) & (TW_HASH_SIZE - 1)];
	while (*pp && *pp != tw)
		pp = &(*pp)->ehash_next;
	if (*pp)
		*pp = tw->ehash_next;
	pp = &tw_bhash[tw_porthash(tw->sport)];
	while (*pp && *pp != tw)
		pp = &(*pp)->bhash_next;
	if (*pp)
		*pp = tw->bhash_next;
	if (tw->all_prev)
		tw->all_prev->all_next = tw->all_next;
	else
		tw_all = tw->all_next;
	if (tw->all_next)
		tw->all_next->all_prev = tw->all_prev;
	if (tcp_tw_count)
		tcp_tw_count--;
	rt_free(tw);
}

static void tcp_tw_timer(void *arg)
{
	tcp_api_enter();
	tcp_tw_kill(arg);
	tcp_api_exit();
}

/* Linux tcp_time_wait(): replace the connection by a compact record */
void tcp_time_wait(struct tcp_sock *tp, int state, u32 timeo)
{
	struct tcp_tw *tw = NULL;

	if (tcp_tw_count < (u32)sysctl_tcp_max_tw)
		tw = rt_zalloc(sizeof(*tw));
	if (tw) {
		u32 h;
		tw->saddr = tp->saddr;
		tw->daddr = tp->daddr;
		tw->sport = tp->sport;
		tw->dport = tp->dport;
		tw->scope = tp->scope;
		tw->substate = (u8)state;
		tw->tstamp_ok = tp->rx_opt.tstamp_ok;
		tw->rcv_wscale = tp->rx_opt.rcv_wscale;
		tw->reuse = tp->reuse;
		tw->snd_nxt = tp->snd_nxt;
		tw->rcv_nxt = tp->rcv_nxt;
		tw->rcv_wnd = tcp_receive_window(tp);
		tw->ts_recent = tp->ts_recent;
		tw->ts_recent_stamp = tp->ts_recent_stamp;
		tw->ts_offset = tp->ts_offset;
		tw->tos = tp->tos;
		tw->ttl = tp->ttl;
		if (state == RT_TCPS_TIME_WAIT || timeo == 0)
			timeo = TCP_TIMEWAIT_LEN;
		rt_timer_init(&tw->timer, tcp_tw_timer, tw);
		h = tcp_tw_hashfn(&tw->saddr, tw->sport, &tw->daddr, tw->dport) & (TW_HASH_SIZE - 1);
		tw->ehash_next = tw_ehash[h];
		tw_ehash[h] = tw;
		h = tw_porthash(tw->sport);
		tw->bhash_next = tw_bhash[h];
		tw_bhash[h] = tw;
		tw->all_next = tw_all;
		if (tw_all)
			tw_all->all_prev = tw;
		tw_all = tw;
		tcp_tw_count++;
		rt_timer_after(&tw->timer, timeo);
	}
	tcp_write_queue_purge(tp);
	tcp_done(tp);
}

void tcp_tw_ack(struct tcp_tw *tw)
{
	u32 win = tw->rcv_wnd >> tw->rcv_wscale;

	tcp_send_ctl(&tw->saddr, &tw->daddr, tw->sport, tw->dport, tw->snd_nxt, tw->rcv_nxt,
	             TCPHDR_ACK, (u16)RT_MIN(win, 65535u), tw->tstamp_ok,
	             tcp_jiffies() + tw->ts_offset, tw->ts_recent, tw->tos, tw->ttl, tw->scope);
}

static int tcp_tw_oow_rate_limited(struct tcp_tw *tw, const struct rt_buf *b)
{
	const struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	u32 now = tcp_jiffies();

	if (cb->seq != cb->end_seq && !(cb->tcp_flags & TCPHDR_SYN))
		return 0;
	if (tw->last_oow_ack_time && (u32)(now - tw->last_oow_ack_time) < TCP_INVALID_RATELIMIT)
		return 1;
	tw->last_oow_ack_time = now ? now : 1;
	return 0;
}

/* Linux tcp_timewait_state_process() incl. RFC 1122 4.2.2.13 / RFC 6191:
 * a new SYN may reopen the connection if its sequence number (or, with
 * timestamps, its TSval) is above the old incarnation's. */
int tcp_timewait_state_process(struct tcp_tw *tw, struct rt_buf *b, u32 *tw_isn)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(b);
	struct tcp_rx_opt opt;
	u8 fl = cb->tcp_flags;
	int paws_reject = 0;

	memset(&opt, 0, sizeof(opt));
	if (cb->hdr_len > TCP_HDR_LEN && tw->tstamp_ok) {
		opt.tstamp_ok = 1;
		tcp_parse_options(b->data, cb->hdr_len, &opt, 1);
		if (opt.saw_tstamp) {
			if (opt.rcv_tsecr)
				opt.rcv_tsecr -= tw->ts_offset;
			paws_reject = tw->ts_recent_stamp &&
			              !tcp_paws_check(NULL, tw->ts_recent, tw->ts_recent_stamp, opt.rcv_tsval, 0) &&
			              !((fl & TCPHDR_RST) &&
			                (s32)(tcp_seconds() - (tw->ts_recent_stamp + TCP_PAWS_MSL)) >= 0);
		}
	}

	if (tw->substate == RT_TCPS_FIN_WAIT_2) {
		/* orphan waiting for the peer's FIN */
		if (paws_reject || !tcp_in_window(cb->seq, cb->end_seq, tw->rcv_nxt,
		                                  tw->rcv_nxt + tw->rcv_wnd)) {
			if (tcp_tw_oow_rate_limited(tw, b))
				return TCP_TW_SUCCESS;
			return TCP_TW_ACK;
		}
		if (fl & TCPHDR_RST) {
			tcp_tw_kill(tw);
			return TCP_TW_SUCCESS;
		}
		if ((fl & TCPHDR_SYN) && !rt_seq_before(cb->seq, tw->rcv_nxt))
			return TCP_TW_RST;
		if (!(fl & TCPHDR_ACK) || !rt_seq_after(cb->end_seq, tw->rcv_nxt) || cb->end_seq == cb->seq)
			return TCP_TW_SUCCESS;
		/* new data after half-duplex close: reset */
		if (!(fl & TCPHDR_FIN) || cb->end_seq != tw->rcv_nxt + 1)
			return TCP_TW_RST;
		tw->substate = RT_TCPS_TIME_WAIT;
		tw->rcv_nxt = cb->end_seq;
		if (opt.saw_tstamp) {
			tw->ts_recent_stamp = tcp_seconds();
			tw->ts_recent = opt.rcv_tsval;
		}
		rt_timer_after(&tw->timer, TCP_TIMEWAIT_LEN);
		return TCP_TW_ACK;
	}

	if (!paws_reject && cb->seq == tw->rcv_nxt && (cb->seq == cb->end_seq || (fl & TCPHDR_RST))) {
		/* in-window: RST or bare ACK */
		if (fl & TCPHDR_RST) {
			/* RFC 1337 says ignore; Linux default kills */
			tcp_tw_kill(tw);
			return TCP_TW_SUCCESS;
		}
		rt_timer_after(&tw->timer, TCP_TIMEWAIT_LEN);
		if (opt.saw_tstamp) {
			tw->ts_recent = opt.rcv_tsval;
			tw->ts_recent_stamp = tcp_seconds();
		}
		return TCP_TW_SUCCESS;
	}
	if ((fl & TCPHDR_SYN) && !(fl & (TCPHDR_RST | TCPHDR_ACK)) && !paws_reject &&
	    (rt_seq_after(cb->seq, tw->rcv_nxt) ||
	     (opt.saw_tstamp && tw->ts_recent_stamp && (s32)(tw->ts_recent - opt.rcv_tsval) < 0))) {
		u32 isn = tw->snd_nxt + 65535 + 2;
		if (isn == 0)
			isn++;
		*tw_isn = isn;
		return TCP_TW_SYN;
	}
	if (paws_reject)
		rt_tcpstat.PAWSEstab++;
	if (!(fl & TCPHDR_RST)) {
		if (paws_reject || (fl & TCPHDR_ACK))
			rt_timer_after(&tw->timer, TCP_TIMEWAIT_LEN);
		if (tcp_tw_oow_rate_limited(tw, b))
			return TCP_TW_SUCCESS;
		return TCP_TW_ACK;
	}
	return TCP_TW_SUCCESS;
}

/* Linux tcp_twsk_unique(): may connect() reuse this TIME_WAIT 4-tuple?
 * Safe with timestamps (PAWS); tw_reuse=2 restricts it to loopback. */
int tcp_twsk_unique(struct tcp_sock *tp, struct tcp_tw *tw)
{
	int reuse = sysctl_tcp_tw_reuse;

	if (tw->substate != RT_TCPS_TIME_WAIT)
		return 0;
	if (reuse == 2) {
		reuse = (rt_in6_is_v4mapped(&tw->daddr) && tw->daddr.b[12] == 127) ||
		        (rt_in6_is_v4mapped(&tw->saddr) && tw->saddr.b[12] == 127) ||
		        rt_in6_is_loopback(&tw->daddr) || rt_in6_eq(&tw->daddr, &tw->saddr);
	}
	if (tw->ts_recent_stamp && reuse && (s32)(tcp_seconds() - tw->ts_recent_stamp) > 0) {
		tp->write_seq = tw->snd_nxt + 65535 + 2;
		if (tp->write_seq == 0)
			tp->write_seq = 1;
		tp->ts_recent = tw->ts_recent;
		tp->ts_recent_stamp = tw->ts_recent_stamp;
		return 1;
	}
	return 0;
}
