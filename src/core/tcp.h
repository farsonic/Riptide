/*
 * Riptide - TCP internals shared by the tcp_*.c files.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The structure follows the Linux TCP stack (net/ipv4/tcp*.c): a tcp_sock
 * holding the inet_connection_sock state (icsk_*), lightweight request
 * socks for the SYN queue, compact time-wait records, a write queue of
 * MSS-sized buffers with per-segment state in rt_buf.cb, an out-of-order
 * queue with SACK generation, RACK-TLP + PRR loss recovery and pluggable
 * congestion control (CUBIC default, NewReno).
 */
#ifndef RT_TCP_INTERNAL_H
#define RT_TCP_INTERNAL_H

#include "rt/riptide.h"
#include "infra.h"

/* ---- protocol constants (Linux include/net/tcp.h equivalents, ms) ---- */
#define TCP_MSS_DEFAULT      536
#define TCP_MIN_MSS          88
#define TCP_MIN_SND_MSS      48
#define TCP_MAX_WSCALE       14
#define TCP_MAX_WINDOW       65535u
#define TCP_MAX_QUICKACKS    16
#define TCP_ATO_MIN          40
#define TCP_DELACK_MIN       40
#define TCP_DELACK_MAX       200
#define TCP_RTO_MAX          120000u
#define TCP_TIMEOUT_INIT     1000u
#define TCP_TIMEOUT_FALLBACK 3000u
#define TCP_TIMEOUT_MIN      2u
#define TCP_RESOURCE_PROBE_INTERVAL 500u
#define TCP_TIMEWAIT_LEN     60000u
#define TCP_PAWS_24DAYS      (60u * 60u * 24u * 24u)
#define TCP_PAWS_MSL         60u
#define TCP_PAWS_WINDOW      1
#define TCP_SYNQ_INTERVAL    200u
#define TCP_MAX_REORDERING   300u
#define TCP_RACK_RECOVERY_THRESH 16
#define TCP_INFINITE_SSTHRESH 0x7fffffffu
#define TCP_ORPHAN_RETRIES   8
#define TCP_CHALLENGE_LIMIT  1000   /* per second, RFC 5961 */
#define TCP_INVALID_RATELIMIT 500u  /* ms between out-of-window ACKs */

/* header flags */
#define TCPHDR_FIN 0x01
#define TCPHDR_SYN 0x02
#define TCPHDR_RST 0x04
#define TCPHDR_PSH 0x08
#define TCPHDR_ACK 0x10
#define TCPHDR_URG 0x20
#define TCPHDR_ECE 0x40
#define TCPHDR_CWR 0x80

/* options */
#define TCPOPT_EOL        0
#define TCPOPT_NOP        1
#define TCPOPT_MSS        2
#define TCPOPT_WINDOW     3
#define TCPOPT_SACK_PERM  4
#define TCPOPT_SACK       5
#define TCPOPT_TIMESTAMP  8
#define TCPOLEN_MSS       4
#define TCPOLEN_WINDOW    3
#define TCPOLEN_SACK_PERM 2
#define TCPOLEN_TIMESTAMP 10
#define TCPOLEN_TSTAMP_ALIGNED 12
#define TCPOLEN_SACK_BASE 2
#define TCPOLEN_SACK_PERBLOCK 8
#define TCP_NUM_SACKS     4
#define TCP_HDR_LEN       20
#define TCP_MAX_OPT_LEN   40

/* ---- per segment state in rt_buf.cb (Linux struct tcp_skb_cb) ---- */
struct tcp_skb_cb {
	u64 tx_us;          /* last transmission time (rt_now_us) */
	u32 seq;            /* first sequence number */
	u32 end_seq;        /* seq + SYN + FIN + payload */
	u32 ack_seq;        /* input: acknowledgment field */
	u32 tx_ms;          /* last transmission time (rt_now_ms, TS units) */
	u32 first_tx_ms;    /* first transmission */
	u16 window;         /* input: raw window field */
	u16 urg_ptr;        /* input: raw urgent pointer */
	u8  tcp_flags;      /* TCPHDR_* */
	u8  sacked;         /* TCPCB_* scoreboard bits */
	u8  retrans;        /* number of retransmissions */
	u8  ip_tos;         /* input: IP TOS (ECN bits) */
	u8  hdr_len;        /* input: TCP header length incl. options */
	u8  eor;
};
#define TCP_SKB_CB(skb) ((struct tcp_skb_cb *)(void *)(skb)->cb.b)

#define TCPCB_SACKED_ACKED   0x01
#define TCPCB_SACKED_RETRANS 0x02
#define TCPCB_LOST           0x04
#define TCPCB_TAGBITS        0x07
#define TCPCB_EVER_RETRANS   0x80
#define TCPCB_RETRANS        (TCPCB_SACKED_RETRANS | TCPCB_EVER_RETRANS)

/* congestion avoidance states (icsk_ca_state) */
#define TCP_CA_Open     0
#define TCP_CA_Disorder 1
#define TCP_CA_CWR      2
#define TCP_CA_Recovery 3
#define TCP_CA_Loss     4

/* congestion control events */
#define CA_EVENT_TX_START     0
#define CA_EVENT_CWND_RESTART 1
#define CA_EVENT_COMPLETE_CWR 2
#define CA_EVENT_LOSS         3

/* icsk_pending: users of the retransmit timer */
#define ICSK_TIME_RETRANS     1
#define ICSK_TIME_PROBE0      3
#define ICSK_TIME_LOSS_PROBE  5
#define ICSK_TIME_REO_TIMEOUT 6

/* icsk_ack.pending */
#define ICSK_ACK_SCHED   1
#define ICSK_ACK_TIMER   2
#define ICSK_ACK_PUSHED  4
#define ICSK_ACK_PUSHED2 8
#define ICSK_ACK_NOW     16

/* sk_shutdown */
#define RCV_SHUTDOWN  1
#define SEND_SHUTDOWN 2
#define SHUTDOWN_MASK 3

/* nonagle */
#define TCP_NAGLE_OFF  1
#define TCP_NAGLE_CORK 2
#define TCP_NAGLE_PUSH 4

/* ecn_flags */
#define TCP_ECN_OK          1
#define TCP_ECN_QUEUE_CWR   2
#define TCP_ECN_DEMAND_CWR  4
#define TCP_ECN_SEEN        8
#define INET_ECN_MASK       3
#define INET_ECN_ECT_0      2
#define INET_ECN_CE         3

/* urg_data */
#define TCP_URG_VALID  0x0100
#define TCP_URG_NOTYET 0x0200
#define TCP_URG_READ   0x0400

/* tcp_ack() flags (Linux tcp_input.c) */
#define FLAG_DATA               0x01
#define FLAG_WIN_UPDATE         0x02
#define FLAG_DATA_ACKED         0x04
#define FLAG_RETRANS_DATA_ACKED 0x08
#define FLAG_SYN_ACKED          0x10
#define FLAG_DATA_SACKED        0x20
#define FLAG_ECE                0x40
#define FLAG_LOST_RETRANS       0x80
#define FLAG_SLOWPATH           0x100
#define FLAG_ORIG_SACK_ACKED    0x200
#define FLAG_SND_UNA_ADVANCED   0x400
#define FLAG_DSACKING_ACK       0x800
#define FLAG_SET_XMIT_TIMER     0x1000
#define FLAG_SACK_RENEGING      0x2000
#define FLAG_UPDATE_TS_RECENT   0x4000
#define FLAG_NO_CHALLENGE_ACK   0x8000
#define FLAG_ACK_MAYBE_DELAYED  0x10000
#define FLAG_DSACK_TLP          0x20000
#define FLAG_ACKED            (FLAG_DATA_ACKED | FLAG_SYN_ACKED)
#define FLAG_NOT_DUP          (FLAG_DATA | FLAG_WIN_UPDATE | FLAG_ACKED)
#define FLAG_CA_ALERT         (FLAG_DATA_SACKED | FLAG_ECE | FLAG_DSACKING_ACK)
#define FLAG_FORWARD_PROGRESS (FLAG_ACKED | FLAG_DATA_SACKED)

#define REXMIT_NONE 0
#define REXMIT_LOST 1
#define REXMIT_NEW  2

struct tcp_sock;

/* ---- congestion control (Linux struct tcp_congestion_ops) ---- */
struct tcp_congestion_ops {
	const char *name;
	void (*init)(struct tcp_sock *tp);
	u32  (*ssthresh)(struct tcp_sock *tp);
	void (*cong_avoid)(struct tcp_sock *tp, u32 ack, u32 acked);
	void (*set_state)(struct tcp_sock *tp, u8 new_state);
	void (*cwnd_event)(struct tcp_sock *tp, int ev);
	void (*pkts_acked)(struct tcp_sock *tp, u32 num_acked, s32 rtt_us);
	u32  (*undo_cwnd)(struct tcp_sock *tp);
};
#define TCP_CA_PRIV_U64 10

struct tcp_sack_block {
	u32 start_seq;
	u32 end_seq;
};

/* Kathleen Nichols' windowed min filter (Linux lib/win_minmax.c) */
struct minmax_sample { u32 t, v; };
struct minmax { struct minmax_sample s[3]; };
u32 minmax_running_min(struct minmax *m, u32 win, u32 t, u32 meas);
static inline u32 minmax_reset(struct minmax *m, u32 t, u32 meas)
{
	m->s[0].t = m->s[1].t = m->s[2].t = t;
	m->s[0].v = m->s[1].v = m->s[2].v = meas;
	return meas;
}
static inline u32 minmax_get(const struct minmax *m) { return m->s[0].v; }

/* Options parsed from an incoming segment (Linux tcp_options_received) */
struct tcp_rx_opt {
	u32 rcv_tsval, rcv_tsecr;
	u16 mss_clamp;       /* peer MSS (SYN) or our clamp */
	u8  saw_tstamp;
	u8  tstamp_ok, sack_ok, wscale_ok;
	u8  snd_wscale, rcv_wscale;
	u8  num_sacks;       /* SACK blocks we generate */
	u8  dsack;           /* D-SACK pending */
	u8  sack_off;        /* offset of SACK option in header (0 none) */
	u8  saw_wscale_raw;
	u16 user_mss;
};

/* Diagnostics (net.riptide.tcpinfo, TCP_INFO): plain counters and
 * Linux-style chronographs, all in ms. Updated with increments only. */
#define TCP_CHRONO_NONE   0
#define TCP_CHRONO_BUSY   1      /* data in flight or queued */
#define TCP_CHRONO_RWND   2      /* sending held back by the peer's window */
#define TCP_CHRONO_SNDBUF 3      /* the application is held back by the send buffer */
struct tcp_diag {
	u32 chrono_start;
	u32 chrono_stat[3];          /* busy, rwnd-limited, sndbuf-limited */
	u8  chrono_type;
	u32 unread_start, unread_ms; /* data waiting for the reader (start 0: idle) */
	u32 zwin_start, zwin_ms;     /* our advertised window at zero */
	u32 estab_ms;                /* when ESTABLISHED was entered (0 never) */
	u32 fast_recoveries, rto_timeouts, tlp_probes;
	u32 dupacks_in, dupacks_out, sack_blocks_out;
	u32 acks_out, delayed_acks_out;  /* pure ACKs; of those, sent by the delack timer */
	u32 zwin_out, zwin_in;       /* zero windows advertised / received */
};

/* ---- the connection (struct tcp_sock + inet_connection_sock) ---- */
struct tcp_request_sock;

struct tcp_sock {
	struct tcp_sock *ehash_next;
	struct tcp_sock *bhash_next;
	struct tcp_sock *lhash_next;
	struct tcp_sock *all_next, *all_prev;
	struct tcp_sock *reap_next;
	struct rt_socket *so;          /* NULL once orphaned */

	rt_in6_addr saddr, daddr;      /* IPv4 as IPv4-mapped (see inet.c) */
	u16 sport, dport;              /* network order */
	u32 scope;                     /* interface of a link-local endpoint */
	u8  family;                    /* socket domain: RT_AF_INET / RT_AF_INET6 */
	u8  v6only;                    /* IPV6_V6ONLY */
	u8  net_hlen;                  /* network header: 20 (IPv4) or 40 (IPv6) */
	u8  state;                     /* RT_TCPS_* */
	u8  ca_state;
	u8  shutdown;                  /* RCV_SHUTDOWN | SEND_SHUTDOWN */
	u8  in_ehash, in_bhash, in_lhash;
	u8  dead;                      /* orphaned (SOCK_DEAD) */
	u8  done;                      /* SOCK_DONE: FIN received / closed */
	u8  destroyed;
	u8  autobind;                  /* local port chosen by connect() */
	u8  reuse;                     /* SO_REUSEADDR at bind time */
	u8  reuseport;
	u8  nodelay, cork, noopt, quickack_req;
	u8  keepopen, oobinline;       /* cached SOL_SOCKET options */
	u8  ecn_flags;
	u8  frto;
	u8  is_sack_reneg;
	u8  is_cwnd_limited;
	u8  tlp_retrans;
	u8  rcvbuf_locked, sndbuf_locked;
	u8  tos, ttl;
	u8  pingpong;
	u8  user_keepcnt;
	u8  ever_connected;
	u8  write_space_wanted;

	/* sequence space */
	u32 iss, irs;
	u32 snd_una, snd_nxt, snd_wnd, snd_wl1, max_window;
	u32 snd_sml, snd_up, write_seq, pushed_seq;
	u32 rcv_nxt, rcv_wnd, rcv_wup, copied_seq, rcv_ssthresh, window_clamp;
	u32 pred_flags;
	u16 tcp_header_len;            /* 20 + fixed options (timestamps) */
	u16 mss_cache;                 /* current effective send MSS */
	u16 advmss;                    /* MSS we advertise */
	u16 pmtu_cookie;               /* MTU mss_cache was computed for */
	struct tcp_rx_opt rx_opt;
	u32 ts_recent, ts_recent_stamp;  /* stamp in seconds */
	u32 ts_offset;
	u32 rcv_rtt_last_tsecr;

	/* SACK we send */
	struct tcp_sack_block duplicate_sack[1];
	struct tcp_sack_block selective_acks[TCP_NUM_SACKS];

	/* queues */
	struct rt_bufq wq;             /* write queue: rtx part then unsent */
	struct rt_buf *send_head;      /* first unsent segment or NULL */
	u32 wq_bytes;                  /* payload bytes in wq */
	u32 wmem_queued;               /* truesize of wq */
	struct rt_bufq rcvq;           /* in-order data for the reader */
	struct rt_bufq ofoq;           /* out-of-order, sorted by seq */
	u32 rmem_alloc;                /* truesize of rcvq + ofoq */
	u32 rcvq_bytes;                /* unread bytes in rcvq */
	u32 ofo_bytes;
	u32 sndbuf, rcvbuf;            /* effective sizes (bytes) */
	u32 so_rcvbuf_seen, so_sndbuf_seen;
	u32 sndlowat;
	u32 urg_data, urg_seq;

	/* scoreboard / counters (packets) */
	u32 packets_out, retrans_out, sacked_out, lost_out;
	u32 max_packets_out, cwnd_usage_seq;
	u32 highest_sack_seq;          /* seq of highest SACKed segment */
	u32 reordering, reord_seen;
	u32 delivered, delivered_ce;
	u32 prr_delivered, prr_out;
	u32 high_seq, undo_marker;
	s32 undo_retrans;
	u32 retrans_stamp;             /* ms of first retransmission (0 none) */
	u32 tlp_high_seq;
	u32 total_retrans;
	u32 lsndtime;                  /* ms, last data sent */
	u32 rcv_tstamp;                /* ms, last ACK received */

	/* RTT (µs; srtt << 3, mdev << 2 like Linux) */
	u32 srtt_us, mdev_us, mdev_max_us, rttvar_us, rtt_seq;
	struct minmax rtt_min;
	struct { u32 rtt_us, seq; u64 time; } rcv_rtt_est;
	struct { u32 space, seq; u64 time; } rcvq_space;

	/* RACK (RFC 8985) */
	struct {
		u64 mstamp;
		u32 rtt_us;
		u32 end_seq;
		u32 last_delivered;
		u8  reo_wnd_steps;
		u8  reo_wnd_persist;
		u8  dsack_seen;
		u8  advanced;
	} rack;

	/* congestion control */
	u32 snd_cwnd, snd_ssthresh, snd_cwnd_cnt, snd_cwnd_clamp;
	u32 snd_cwnd_used, snd_cwnd_stamp;
	u32 prior_cwnd, prior_ssthresh;
	const struct tcp_congestion_ops *ca_ops;
	u64 ca_priv[TCP_CA_PRIV_U64];

	/* inet_connection_sock */
	struct rt_timer retrans_timer, delack_timer, keepalive_timer;
	u32 icsk_rto;                  /* ms */
	u32 icsk_timeout;
	u8  icsk_pending;
	u8  icsk_retransmits;
	u8  icsk_backoff;
	u8  icsk_probes_out;
	u32 icsk_probes_tstamp;
	u32 icsk_user_timeout;         /* ms, TCP_USER_TIMEOUT */
	u32 keepalive_time, keepalive_intvl, keepalive_probes; /* 0 = sysctl */
	u32 keepinit;                  /* s, TCP_KEEPINIT (0 = default) */
	u8  syn_retries;
	struct {
		u8  pending;
		u8  quick;
		u16 rcv_mss;
		u32 ato;
		u32 timeout;
		u32 lrcvtime;
		u32 last_seg_size;
	} ack;
	u32 last_oow_ack_time;
	u32 linger2;                   /* ms */

	/* listener */
	struct tcp_request_sock *synq;
	u32 synq_len;
	u32 synq_overflow_ts;
	u32 max_ack_backlog;

	int err_soft;
	/* stats for TCP_INFO and net.riptide.tcpinfo */
	u64 bytes_acked, bytes_received;
	u32 segs_in, segs_out, data_segs_in, data_segs_out;
	struct tcp_diag dg;
};

/* SYN queue entry (Linux tcp_request_sock) */
struct tcp_request_sock {
	struct tcp_request_sock *next;
	struct tcp_sock *listener;
	struct rt_timer timer;
	rt_in6_addr loc_addr, rmt_addr;
	u16 loc_port, rmt_port;
	u32 scope;
	u32 snt_isn, rcv_isn;
	u32 ts_recent, ts_off;
	u32 rcv_wnd, window_clamp;
	u32 snt_synack_ms;
	u64 snt_synack_us;
	u16 mss;                       /* MSS we advertise */
	u16 peer_mss;
	u8  snd_wscale, rcv_wscale;
	u8  tstamp_ok, sack_ok, wscale_ok, ecn_ok;
	u8  num_retrans;
	u8  acked;
	u8  tos;
};

/* Time-wait record (Linux inet_timewait_sock / tcp_timewait_sock) */
struct tcp_tw {
	struct tcp_tw *ehash_next;
	struct tcp_tw *bhash_next;
	struct tcp_tw *all_next, *all_prev;
	struct rt_timer timer;
	rt_in6_addr saddr, daddr;
	u16 sport, dport;
	u32 scope;
	u8  substate;                  /* RT_TCPS_TIME_WAIT or FIN_WAIT_2 */
	u8  tstamp_ok, rcv_wscale, reuse;
	u32 snd_nxt, rcv_nxt, rcv_wnd;
	u32 ts_recent, ts_recent_stamp, ts_offset;
	u32 last_oow_ack_time;
	u8  tos, ttl;
};

/* ---- tunables (tcp_main.c) ---- */
extern s32 sysctl_tcp_timestamps, sysctl_tcp_window_scaling, sysctl_tcp_sack,
           sysctl_tcp_dsack, sysctl_tcp_ecn, sysctl_tcp_initcwnd,
           sysctl_tcp_rto_min_ms, sysctl_tcp_syncookies, sysctl_tcp_synack_retries,
           sysctl_tcp_syn_retries, sysctl_tcp_retries1, sysctl_tcp_retries2,
           sysctl_tcp_keepcnt, sysctl_tcp_fin_timeout, sysctl_tcp_tw_reuse,
           sysctl_tcp_frto, sysctl_tcp_recovery, sysctl_tcp_reordering,
           sysctl_tcp_rmem_max, sysctl_tcp_wmem_max, sysctl_tcp_moderate_rcvbuf,
           sysctl_tcp_slow_start_after_idle, sysctl_tcp_max_orphans,
           sysctl_tcp_mssdflt,
           sysctl_tcp_keepidle, sysctl_tcp_keepintvl, sysctl_tcp_max_tw,
           sysctl_tcp_delack_segs, sysctl_tcp_delack_ms, sysctl_tcp_quickack;
extern char sysctl_tcp_cc[16];
#define TCP_RACK_LOSS_DETECTION 0x1
#define TCP_RACK_STATIC_REO_WND 0x2
#define TCP_RACK_NO_DUPTHRESH   0x4

extern u32 tcp_orphan_count;
extern u32 tcp_tw_count;

/* ---- small helpers ---- */
static inline u32 tcp_jiffies(void) { return rt_now_ms(); }
extern u64 tcp_clock_us;               /* refreshed at every entry point */
static inline void tcp_mstamp_refresh(void) { tcp_clock_us = rt_now_us(); }
u32 tcp_seconds(void);

static inline u32 tcp_rto_min(void) { return (u32)sysctl_tcp_rto_min_ms; }
static inline u32 tcp_rto_min_us(void) { return (u32)sysctl_tcp_rto_min_ms * 1000u; }
static inline u32 tcp_time_stamp_ts(const struct tcp_sock *tp) { return tcp_jiffies() + tp->ts_offset; }

static inline u32 tcp_receive_window(const struct tcp_sock *tp)
{
	s32 win = (s32)(tp->rcv_wup + tp->rcv_wnd - tp->rcv_nxt);
	return win < 0 ? 0 : (u32)win;
}
static inline u32 tcp_wnd_end(const struct tcp_sock *tp) { return tp->snd_una + tp->snd_wnd; }
static inline u32 tcp_left_out(const struct tcp_sock *tp) { return tp->sacked_out + tp->lost_out; }
static inline u32 tcp_packets_in_flight(const struct tcp_sock *tp)
{
	return tp->packets_out - tcp_left_out(tp) + tp->retrans_out;
}
static inline int tcp_is_sack(const struct tcp_sock *tp) { return tp->rx_opt.sack_ok; }
static inline int tcp_is_reno(const struct tcp_sock *tp) { return !tcp_is_sack(tp); }
static inline int tcp_is_rack(void) { return sysctl_tcp_recovery & TCP_RACK_LOSS_DETECTION; }
static inline int tcp_in_slow_start(const struct tcp_sock *tp) { return tp->snd_cwnd < tp->snd_ssthresh; }
static inline int tcp_in_cwnd_reduction(const struct tcp_sock *tp)
{
	return ((1u << tp->ca_state) & ((1u << TCP_CA_CWR) | (1u << TCP_CA_Recovery))) != 0;
}
static inline u32 tcp_min_rtt(const struct tcp_sock *tp) { return minmax_get(&tp->rtt_min); }
static inline struct rt_buf *tcp_rtx_head(const struct tcp_sock *tp)
{
	return tp->wq.head != tp->send_head ? tp->wq.head : NULL;
}
static inline int tcp_write_queue_empty(const struct tcp_sock *tp) { return tp->send_head == NULL; }
static inline u32 tcp_skb_len(const struct rt_buf *b) { return TCP_SKB_CB(b)->end_seq - TCP_SKB_CB(b)->seq; }
static inline u32 tcp_buf_truesize(const struct rt_buf *b) { return b->size + (u32)sizeof(struct rt_buf); }
static inline void *tcp_ca(struct tcp_sock *tp) { return tp->ca_priv; }
static inline int tcp_urg_mode(const struct tcp_sock *tp) { return tp->snd_up != tp->snd_una; }
static inline u32 tcp_current_ssthresh(const struct tcp_sock *tp)
{
	if (tcp_in_cwnd_reduction(tp))
		return tp->snd_ssthresh;
	return RT_MAX(tp->snd_ssthresh, ((tp->snd_cwnd >> 1) + (tp->snd_cwnd >> 2)));
}
static inline u32 tcp_highest_sack_seq(const struct tcp_sock *tp)
{
	if (!tp->sacked_out)
		return tp->snd_una;
	return tp->highest_sack_seq;
}
static inline int tcp_stream_is_writeable(const struct tcp_sock *tp)
{
	u32 used = tp->write_seq - tp->snd_una;
	u32 freesp = used < tp->sndbuf ? tp->sndbuf - used : 0;
	u32 lowat = RT_MAX(used >> 1, RT_MIN(tp->sndlowat, tp->sndbuf >> 1));
	return freesp > 0 && freesp >= lowat;
}
static inline int keepalive_time_when(const struct tcp_sock *tp)
{
	return (int)(tp->keepalive_time ? tp->keepalive_time : (u32)sysctl_tcp_keepidle * 1000u);
}
static inline u32 keepalive_intvl_when(const struct tcp_sock *tp)
{
	return tp->keepalive_intvl ? tp->keepalive_intvl : (u32)sysctl_tcp_keepintvl * 1000u;
}
static inline u32 keepalive_probes(const struct tcp_sock *tp)
{
	return tp->keepalive_probes ? tp->keepalive_probes : (u32)sysctl_tcp_keepcnt;
}
/* Linux tcp_chrono_start/stop: a higher type overrides a lower one */
static inline u32 tcp_diag_now(void)
{
	u32 t = tcp_jiffies();
	return t ? t : 1;                  /* 0 means "not running" */
}
void tcp_chrono_set(struct tcp_sock *tp, u8 type);
static inline void tcp_chrono_start(struct tcp_sock *tp, u8 type)
{
	if (type > tp->dg.chrono_type)
		tcp_chrono_set(tp, type);
}
void tcp_chrono_stop(struct tcp_sock *tp, u8 type);
static inline void tcp_diag_unread_stop(struct tcp_sock *tp)
{
	if (tp->dg.unread_start && !tp->rcvq_bytes) {
		tp->dg.unread_ms += tcp_jiffies() - tp->dg.unread_start;
		tp->dg.unread_start = 0;
	}
}

static inline void tcp_snd_cwnd_set(struct tcp_sock *tp, u32 val)
{
	if (val < 1)
		val = 1;
	tp->snd_cwnd = val;
}


/* ---- tcp_main.c ---- */
void tcp_set_state(struct tcp_sock *tp, int state);
void tcp_done(struct tcp_sock *tp);
void tcp_reap(void);
void tcp_destroy_sock(struct tcp_sock *tp);
struct tcp_sock *tcp_sock_alloc(void);
void tcp_init_sock(struct tcp_sock *tp);
void tcp_sock_wake(struct tcp_sock *tp, u32 events);
void tcp_sock_error(struct tcp_sock *tp, int err);
void tcp_write_queue_purge(struct tcp_sock *tp);
void tcp_ehash_insert(struct tcp_sock *tp);
void tcp_ehash_remove(struct tcp_sock *tp);
void tcp_bhash_insert(struct tcp_sock *tp);
void tcp_unhash(struct tcp_sock *tp);
struct tcp_sock *tcp_lookup_established(const rt_in6_addr *saddr, u16 sport, const rt_in6_addr *daddr,
                                        u16 dport);
struct tcp_sock *tcp_lookup_listener(const rt_in6_addr *daddr, u16 dport);
struct tcp_tw *tcp_tw_lookup(const rt_in6_addr *saddr, u16 sport, const rt_in6_addr *daddr, u16 dport);
u32 tcp_secure_seq(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport);
u32 tcp_secure_ts_off(const rt_in6_addr *saddr, const rt_in6_addr *daddr);
u32 tcp_cookie_hash(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport,
                    u32 count, u32 c);
u32 tcp_addr_fold(const rt_in6_addr *a);
void tcp_sync_rcvbuf(struct tcp_sock *tp);
void tcp_check_space(struct tcp_sock *tp);
void tcp_sndbuf_expand(struct tcp_sock *tp);
int  tcp_ca_select(struct tcp_sock *tp, const char *name);
void tcp_init_congestion_control(struct tcp_sock *tp);
const struct tcp_congestion_ops *tcp_ca_find(const char *name);
void tcp_ack_rcvbuf_adjust(struct tcp_sock *tp);
void tcp_cleanup_rbuf(struct tcp_sock *tp, int copied);
void tcp_rcv_space_adjust(struct tcp_sock *tp);
u32  tcp_full_space(const struct tcp_sock *tp);

/* ---- tcp_input.c ---- */
int  tcp_parse_options(const u8 *th, u32 hdr_len, struct tcp_rx_opt *opt, int estab);
void tcp_rcv_established(struct tcp_sock *tp, struct rt_buf *b);
int  tcp_rcv_state_process(struct tcp_sock *tp, struct rt_buf *b);
void tcp_reset(struct tcp_sock *tp);
void tcp_initialize_rcv_mss(struct tcp_sock *tp);
void tcp_init_buffer_space(struct tcp_sock *tp);
void tcp_init_transfer(struct tcp_sock *tp);
void tcp_fast_path_on(struct tcp_sock *tp);
void tcp_fast_path_check(struct tcp_sock *tp);
void tcp_enter_quickack_mode(struct tcp_sock *tp, u32 max_quickacks);
void tcp_send_challenge_ack(struct tcp_sock *tp);
int  tcp_paws_check(const struct tcp_sock *tp, u32 ts_recent, u32 ts_recent_stamp, u32 tsval, int paws_win);
void tcp_store_ts_recent(struct tcp_sock *tp);
int  tcp_ack_update_rtt(struct tcp_sock *tp, int flag, s32 seq_rtt_us, s32 sack_rtt_us, s32 ca_rtt_us);
void tcp_rtt_estimator(struct tcp_sock *tp, s32 mrtt_us);
void tcp_set_rto(struct tcp_sock *tp);
void tcp_update_rtt_min(struct tcp_sock *tp, u32 rtt_us, int flag);
void tcp_rcv_rtt_update(struct tcp_sock *tp, u32 sample, int win_dep);
void tcp_finish_connect(struct tcp_sock *tp);
u32  tcp_space(const struct tcp_sock *tp);
void tcp_ecn_check_ce(struct tcp_sock *tp, const struct rt_buf *b);
int  tcp_listen_input(struct tcp_sock *lp, struct rt_buf *b, const rt_in6_addr *saddr,
                      const rt_in6_addr *daddr, u32 tw_isn);
void tcp_api_enter(void);
void tcp_api_exit(void);

/* ---- tcp_output.c ---- */
int  tcp_transmit_skb(struct tcp_sock *tp, struct rt_buf *b, int clone);
void tcp_send_ack(struct tcp_sock *tp);
void tcp_send_delayed_ack(struct tcp_sock *tp);
void tcp_send_active_reset(struct tcp_sock *tp);
void tcp_send_fin(struct tcp_sock *tp);
int  tcp_send_synack(struct tcp_sock *tp);
int  tcp_connect(struct tcp_sock *tp);
int  tcp_write_xmit(struct tcp_sock *tp, u32 mss_now, int nonagle, int push_one);
void tcp_push_pending_frames(struct tcp_sock *tp);
void __tcp_push_pending_frames(struct tcp_sock *tp, u32 cur_mss, int nonagle);
void tcp_data_snd_check(struct tcp_sock *tp);
u32  tcp_current_mss(struct tcp_sock *tp);
u32  tcp_sync_mss(struct tcp_sock *tp, u32 pmtu);
u32  tcp_mss_to_mtu(const struct tcp_sock *tp, u32 mss);
u32  __tcp_select_window(struct tcp_sock *tp);
void tcp_select_initial_window(u32 space, u32 mss, u32 *rcv_wnd, u32 *window_clamp,
                               int wscale_ok, u8 *rcv_wscale);
int  tcp_fragment(struct tcp_sock *tp, struct rt_buf *b, u32 len);
int  tcp_retransmit_skb(struct tcp_sock *tp, struct rt_buf *b);
void tcp_xmit_retransmit_queue(struct tcp_sock *tp);
int  tcp_write_wakeup(struct tcp_sock *tp);
void tcp_send_probe0(struct tcp_sock *tp);
void tcp_check_probe_timer(struct tcp_sock *tp);
void tcp_send_loss_probe(struct tcp_sock *tp);
void tcp_send_reset_raw(const struct rt_buf *in, const rt_in6_addr *saddr, const rt_in6_addr *daddr,
                        u32 scope);
int  tcp_send_ctl(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport, u32 seq,
                  u32 ack, u8 flags, u16 win, int ts, u32 tsval, u32 tsecr, u8 tos, u8 ttl, u32 scope);
int  tcp_send_synack_req(struct tcp_request_sock *req, u32 tsval);
void tcp_cwnd_restart(struct tcp_sock *tp, s32 delta);
void tcp_mark_push(struct tcp_sock *tp, struct rt_buf *b);
struct rt_buf *tcp_stream_alloc_skb(struct tcp_sock *tp, u32 size);
void tcp_wmem_free_skb(struct tcp_sock *tp, struct rt_buf *b);
void tcp_simple_retransmit(struct tcp_sock *tp);
int  tcp_established_options_len(const struct tcp_sock *tp);
void tcp_event_ack_sent(struct tcp_sock *tp);

/* ---- tcp_timer.c ---- */
void tcp_init_xmit_timers(struct tcp_sock *tp);
void tcp_clear_xmit_timers(struct tcp_sock *tp);
void tcp_reset_xmit_timer(struct tcp_sock *tp, int what, u32 when);
void tcp_clear_xmit_timer(struct tcp_sock *tp, int what);
void tcp_reset_keepalive_timer(struct tcp_sock *tp, u32 when);
void tcp_delete_keepalive_timer(struct tcp_sock *tp);
void tcp_rearm_rto(struct tcp_sock *tp);
void tcp_set_xmit_timer(struct tcp_sock *tp);
int  tcp_schedule_loss_probe(struct tcp_sock *tp, int advancing_rto);
s64  tcp_rto_delta_us(const struct tcp_sock *tp);
void tcp_write_err(struct tcp_sock *tp);
u32  tcp_probe0_when(const struct tcp_sock *tp, u32 max_when);

/* ---- tcp_cong.c / tcp_cubic.c ---- */
extern const struct tcp_congestion_ops tcp_reno;
extern const struct tcp_congestion_ops tcp_cubic;
u32  tcp_slow_start(struct tcp_sock *tp, u32 acked);
void tcp_cong_avoid_ai(struct tcp_sock *tp, u32 w, u32 acked);
int  tcp_is_cwnd_limited(const struct tcp_sock *tp);
u32  tcp_reno_ssthresh(struct tcp_sock *tp);
u32  tcp_reno_undo_cwnd(struct tcp_sock *tp);
void tcp_reno_cong_avoid(struct tcp_sock *tp, u32 ack, u32 acked);
void tcp_ca_event(struct tcp_sock *tp, int ev);
void tcp_set_ca_state(struct tcp_sock *tp, u8 st);
u32  tcp_cubic_root(u64 a);      /* exported for unit tests */

/* ---- tcp_recovery.c ---- */
struct tcp_sacktag_state {
	u64 first_sackt, last_sackt;
	u32 reord;
	u32 sack_delivered;
};
int  tcp_sacktag_write_queue(struct tcp_sock *tp, const struct rt_buf *ack_skb,
                             u32 prior_snd_una, struct tcp_sacktag_state *state);
void tcp_fastretrans_alert(struct tcp_sock *tp, u32 prior_snd_una, int num_dupack,
                           int *ack_flag, int *rexmit);
void tcp_enter_loss(struct tcp_sock *tp);
void tcp_enter_cwr(struct tcp_sock *tp);
void tcp_cwnd_reduction(struct tcp_sock *tp, int newly_acked_sacked, int newly_lost, int flag);
void tcp_end_cwnd_reduction(struct tcp_sock *tp);
void tcp_init_cwnd_reduction(struct tcp_sock *tp);
void tcp_mark_skb_lost(struct tcp_sock *tp, struct rt_buf *b);
void tcp_rack_advance(struct tcp_sock *tp, u8 sacked, u32 end_seq, u64 xmit_us);
void tcp_rack_reo_timeout(struct tcp_sock *tp);
void tcp_rack_update_reo_wnd(struct tcp_sock *tp, u32 prior_delivered);
s32  tcp_rack_skb_timeout(struct tcp_sock *tp, const struct rt_buf *b, u32 reo_wnd);
void tcp_process_tlp_ack(struct tcp_sock *tp, u32 ack, int flag);
void tcp_remove_reno_sacks(struct tcp_sock *tp, int acked);
void tcp_check_sack_reordering(struct tcp_sock *tp, u32 low_seq, int ts);
void tcp_clear_retrans(struct tcp_sock *tp);
void tcp_xmit_recovery(struct tcp_sock *tp, int rexmit);
void tcp_try_keep_open(struct tcp_sock *tp);
void tcp_sack_new_ofo_skb(struct tcp_sock *tp, u32 seq, u32 end_seq);
void tcp_sack_remove(struct tcp_sock *tp);
void tcp_dsack_set(struct tcp_sock *tp, u32 seq, u32 end_seq);
void tcp_dsack_extend(struct tcp_sock *tp, u32 seq, u32 end_seq);
int  tcp_limit_reno_sacked(struct tcp_sock *tp);

/* ---- tcp_minisocks.c ---- */
void tcp_synq_purge(struct tcp_sock *lp);
void tcp_req_err(struct tcp_sock *lp, const rt_in6_addr *raddr, u16 rport, u32 seq, int abort);
void tcp_time_wait(struct tcp_sock *tp, int state, u32 timeo);
#define TCP_TW_SUCCESS 0
#define TCP_TW_RST     1
#define TCP_TW_ACK     2
#define TCP_TW_SYN     3
int  tcp_timewait_state_process(struct tcp_tw *tw, struct rt_buf *b, u32 *tw_isn);
void tcp_tw_ack(struct tcp_tw *tw);
void tcp_tw_kill(struct tcp_tw *tw);
int  tcp_twsk_unique(struct tcp_sock *tp, struct tcp_tw *tw);
struct tcp_tw *tcp_tw_first(void);
u32  tcp_cookie_init_seq(const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport, u16 dport,
                         u32 seq, u16 *mssp);
int  tcp_cookie_check(u32 cookie, const rt_in6_addr *saddr, const rt_in6_addr *daddr, u16 sport,
                      u16 dport, u32 seq);
extern const u16 tcp_msstab[4];
int  tcp_tw_port_used(u16 port, const rt_in6_addr *addr, int v6only, int reuse);
u32  tcp_tw_hashfn(const rt_in6_addr *laddr, u16 lport, const rt_in6_addr *faddr, u16 fport);
/* route MTU towards daddr (576 / 1280 when unknown) */
u32  tcp_route_mtu(const rt_in6_addr *daddr, u32 scope);

#endif
