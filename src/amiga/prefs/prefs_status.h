/*
 * Riptide - RiptidePrefs: the Status page apart from the screen, so the
 * host tests can check it. The editor reads Riptide's tables once a
 * second (net.riptide.status, ifaces, addrs6, routes, tcpinfo, dns,
 * tcpstat, ipstat) and hands the records to rp_status_update(), which
 * works out rates from counters that wrap, keeps the history behind the
 * graphs and matches connections from one second to the next. The
 * rp_st_*() functions then give the page its text.
 *
 * Portable C (no AmigaOS calls): host-tested by tests/t_prefs_status.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_STATUS_H
#define RT_PREFS_STATUS_H

#include "rt/rt_types.h"
#include "rt/rt_ctl.h"
#include "rt/rt_stats.h"

/* ---- the history of a graph: the last RP_HIST samples of two series ---- */
#define RP_HIST 60
struct rp_hist {
	u32 v[2][RP_HIST];      /* ring: the newest at (head + n - 1) % RP_HIST */
	int head, n;            /* oldest sample; samples held (up to RP_HIST) */
	u32 pushes;             /* samples ever pushed (a graph draws only what is new) */
};
void rp_hist_clear(struct rp_hist *h);
void rp_hist_push(struct rp_hist *h, u32 a, u32 b);
/* sample i (0 the oldest of the n held) of series s */
u32  rp_hist_at(const struct rp_hist *h, int s, int i);
/* the largest sample of series s (-1: both) */
u32  rp_hist_max(const struct rp_hist *h, int s);

/* The top of a graph's scale: the smallest 1, 2 or 5 times a power of
 * ten that is at least v (and at least min). */
u32  rp_nice_scale(u32 v, u32 min);
/* y of value v in a plot of height h pixels with that scale: 0 at the
 * bottom, h at the top (clamped) */
int  rp_scale_y(u32 v, u32 scale, int h);

/* ---- rates ---- */
/* What a counter that may wrap at 2^32 grew per second over ms
 * milliseconds (0 when ms is 0). No 64-bit division. */
u32  rp_rate(u32 prev, u32 cur, u32 ms);
/* The share of the time (0..1000, per mille) the Riptide process was
 * busy between two reads of net.riptide.status: idle and now ticks. */
u32  rp_cpu_permille(u32 idle0, u32 now0, u32 idle1, u32 now1);

/* ---- wording ---- */
/* "512 B", "12.3 KB", "4.56 MB", "1.20 GB" (KB = 1024 bytes) */
void rp_fmt_bytes(char *buf, int len, u32 hi, u32 lo);
/* "0 B/s", "812 B/s", "12.3 KB/s", "1.20 MB/s" */
void rp_fmt_rate(char *buf, int len, u32 bytes_per_s);
/* a count with thin groups: "1 234 567" */
void rp_fmt_count(char *buf, int len, u32 v);
/* a duration: "45 s", "12 min", "3 h 05 min", "2 days 4 h" */
void rp_fmt_dur(char *buf, int len, u32 s);
/* an uptime: "0:00:45", "3:04:05", "2 days 3:04:05" */
void rp_fmt_uptime(char *buf, int len, u32 s);
/* milliseconds with a sign: "+0.012 s", "-1.250 s", "+12 s" (whole
 * seconds from 10 s up) */
void rp_fmt_offset(char *buf, int len, s32 ms);
/* microseconds as milliseconds: "0.4 ms", "12 ms", "1 200 ms" */
void rp_fmt_ms(char *buf, int len, u32 us);
/* an IPv4 address in network order, "-" for 0 */
void rp_fmt_ip4(char *buf, int len, u32 addr_n);
/* an address and netmask as "192.168.1.2/24" */
void rp_fmt_ip4_plen(char *buf, int len, u32 addr_n, u32 mask_n);
/* a table address (16 bytes, IPv4 as ::ffff:a.b.c.d) with a port (0:
 * none): "10.0.0.2:80", "[2001:db8::1]:443", "2001:db8::1" */
void rp_fmt_addr_port(char *buf, int len, const u8 *a16, u16 port);

/* in words: a DHCP state (RT_DHCPS_*), a TCP state (RT_TCPS_*) */
const char *rp_dhcp_state_name(int st);
const char *rp_tcp_state_words(int st);

/* ---- the model behind the page ---- */
#define RP_ST_NIF   8           /* interfaces shown */
#define RP_ST_NCONN 24          /* connections shown */
#define RP_ST_NV6   2           /* IPv6 addresses shown per interface */

struct rp_st_if {
	char name[16];
	char descr[32];
	u32  index;
	u16  flags, mtu;
	u16  dhcp_state, vlan_id;
	u32  addr, mask, router;    /* network order; router from DHCP or the default route */
	u32  ll_addr, dhcp_left;
	u8   ip4_off, ip6_off, ll_state, nv6;
	char v6[RP_ST_NV6][44];
	u32  ibytes, obytes, ipk, opk, ierr, oerr, drops;
	u32  rx_bps, tx_bps, rx_peak, tx_peak;
	struct rp_hist hist;        /* receive, send bytes per second */
};

struct rp_st_conn {
	u8   family, state, ecn, ca_state;
	u16  lport, fport;
	u8   laddr[16], faddr[16];
	u32  srtt_us, cwnd, mss, retrans;
	u32  rx_lo, tx_lo;          /* bytes received / acknowledged (low words) */
	u32  rx_bps, tx_bps;
	char cc[16], prog[16];
	struct rp_hist hist;        /* RTT in 0.1 ms, cwnd in packets */
};

struct rp_status_model {
	int  running;
	int  have_prev;             /* rates need two samples */
	u32  now_ms;
	struct rt_ctl_status st;
	int  cpu_ok;                /* cpu_pm is valid */
	u32  cpu_pm;                /* busy, per mille */
	struct rp_hist cpu_hist;    /* busy per mille, 0 */
	int  nif;
	struct rp_st_if ifs[RP_ST_NIF];
	int  nconn, nconn_all;      /* shown; all TCP connections (listeners too) */
	struct rp_st_conn conns[RP_ST_NCONN];
	u32  rx_bps, tx_bps, rx_peak, tx_peak;
	struct rp_hist total;       /* all interfaces but loopback */
	/* names */
	int  ndns;
	u32  dns[RT_CTL_MAXNS];
	char domain[64], hostname[64];
	/* counters (totals since Riptide started) */
	int  have_tcpst, have_ipst;
	struct rt_tcp_stats tcp;
	struct rt_ip_stats ip;
	u32  retrans_ps;            /* segments resent per second, last second */
};

/* what one read of the tables gave (records of recsize bytes each) */
struct rp_st_input {
	u32 now_ms;
	const struct rt_ctl_status *st; u32 st_len;
	const u8 *ifaces; int nifaces; u32 ifrec;
	const u8 *addrs6; int naddrs6; u32 a6rec;
	const u8 *routes; int nroutes; u32 rrec;
	const u8 *tcp; int ntcp; u32 trec;
	const struct rt_ctl_dns *dns; u32 dns_len;
	const struct rt_tcp_stats *tcpst; u32 tcpst_len;
	const struct rt_ip_stats *ipst; u32 ipst_len;
};

void rp_status_init(struct rp_status_model *m);
/* a new read; any pointer may be NULL (that table is missing) */
void rp_status_update(struct rp_status_model *m, const struct rp_st_input *in);
/* Riptide is not running (or stopped): forget the samples */
void rp_status_stopped(struct rp_status_model *m);

/* ---- the page's text ---- */
/* "Riptide 0.23 is running. Up 1:02:03. CPU 12 %." */
void rp_st_summary(const struct rp_status_model *m, char *buf, int len);
/* "Sockets use 64 KB of 512 KB (pressure at 384 KB)." */
void rp_st_memory(const struct rp_status_model *m, char *buf, int len);
/* an interface in three lines (no newline at the end of each):
 * l1 "eth0  up  uaenet.device unit 0"
 * l2 "192.168.1.2/24 via 192.168.1.1, DHCP: bound, 3 h 05 min left"
 * l3 "2001:db8::2, fe80::1" (or "IPv6 off") */
void rp_st_if_lines(const struct rp_st_if *f, char *l1, char *l2, char *l3, int len);
/* "↓ 12.3 KB/s  ↑ 1.20 KB/s" as "In 12.3 KB/s  Out 1.20 KB/s" */
void rp_st_if_rates(const struct rp_st_if *f, char *buf, int len);
/* "Received 12.3 MB in 9 123 packets, sent 1.20 MB in 4 000, 0 errors" */
void rp_st_if_totals(const struct rp_st_if *f, char *buf, int len);
/* a connection as columns separated by '\t':
 * state, local, remote, RTT, cwnd, rate, flags, program */
void rp_st_conn_row(const struct rp_st_conn *c, char *buf, int len);
/* the column titles in the same order */
const char *rp_st_conn_header(void);
/* "Names and time" and "Counters" as lines separated by '\n' */
void rp_st_names_text(const struct rp_status_model *m, char *buf, int len);
void rp_st_counters_text(const struct rp_status_model *m, char *buf, int len);

/* ---- AmigaOS only (status_amiga.c) ---- */
/* Read the tables into m when Riptide runs: 1, else 0 (m then says
 * "not running"). Opens and closes the libraries each time. */
int rp_status_read(struct rp_status_model *m);
/* the buffers it keeps between reads */
void rp_status_free(void);

#endif
