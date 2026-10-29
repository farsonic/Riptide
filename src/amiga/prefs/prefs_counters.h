/*
 * Riptide - RiptidePrefs: the Counters page apart from the screen, so the
 * host tests can check it. Every count Riptide keeps, grouped and named in
 * plain words: each card (packets, bytes, errors), its driver and QoS
 * bands, IPv4, ICMP, UDP, TCP, IPv6, ICMPv6 (with Neighbor Discovery and
 * MLD), IGMP, the name cache and the socket memory. The editor reads the
 * tables (the same ones netstat -s and ip -s link use) and hands them to
 * rp_cnt_update(), which builds the rows, works out the change per second
 * and keeps the values at "Zero" so the page can show counts since then.
 *
 * Portable C (no AmigaOS calls): host-tested by tests/t_prefs_status.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_COUNTERS_H
#define RT_PREFS_COUNTERS_H

#include "rt/rt_types.h"
#include "rt/rt_ctl.h"
#include "rt/rt_stats.h"
#include "rt/rt_ip6.h"

#define RP_CNT_MAX  640         /* rows at most (a group heading is a row too) */
#define RP_CNT_NAME 44

/* a row: a group heading ("TCP"), a count, or a value that is not a count
 * (a level such as the memory in use: no change per second, Zero leaves
 * it as it is) */
enum { RP_CNT_HEAD, RP_CNT_COUNT, RP_CNT_LEVEL };

struct rp_cnt_row {
	char name[RP_CNT_NAME];
	u8   kind;              /* RP_CNT_* */
	u8   have_prev;         /* a value from the read before (for the rate) */
	u8   have_base;         /* a value at Zero */
	u8   pad;
	u32  val, prev, base;
	u32  rate;              /* change per second over the last two reads */
};

struct rp_cnt_model {
	int  running;
	int  n;
	struct rp_cnt_row rows[RP_CNT_MAX];
	int  zeroed;            /* show counts since Zero */
	u32  zero_ms;           /* when (now_ms of that read) */
	u32  now_ms, prev_ms;
	int  have_prev;
	u32  reads;
};

/* what one read of the tables gave: records of their recsize bytes (an
 * older Riptide sends shorter ones: only what fits is used); a NULL
 * pointer or a 0 count when a table is missing */
struct rp_cnt_input {
	u32 now_ms;
	const struct rt_ip_stats *ip;      u32 ip_len;
	const struct rt_icmp_stats *icmp;  u32 icmp_len;
	const struct rt_udp_stats *udp;    u32 udp_len;
	const struct rt_tcp_stats *tcp;    u32 tcp_len;
	const struct rt_ip6_stats *ip6;    u32 ip6_len;
	const struct rt_icmp6_stats *icmp6; u32 icmp6_len;
	const struct rt_igmp_stats *igmp;  u32 igmp_len;
	const struct rt_ctl_status *st;    u32 st_len;
	const u8 *ifaces; int nifaces; u32 ifrec;
	const u8 *drivers; int ndrivers; u32 drvrec;
	const u8 *qos; int nqos; u32 qosrec;
};

void rp_cnt_init(struct rp_cnt_model *m);
/* a new read */
void rp_cnt_update(struct rp_cnt_model *m, const struct rp_cnt_input *in);
/* Riptide is not running (or stopped): no rows */
void rp_cnt_stopped(struct rp_cnt_model *m);
/* Zero: from now on show counts since this read; Since start: as Riptide
 * counts them */
void rp_cnt_zero(struct rp_cnt_model *m);
void rp_cnt_since_start(struct rp_cnt_model *m);

/* row i as the page shows it: its name (a heading as it is, a count
 * indented), the value ("1 234", counts since Zero when zeroed) and the
 * change per second ("+12/s", "" when none or not a count) */
void rp_cnt_text(const struct rp_cnt_model *m, int i, char *name, int nlen, char *val, int vlen, char *rate, int rlen);
/* the line above the rows: "Counts since Riptide started (up 1:02:03)."
 * or "Counts since Zero, 2 min ago." */
void rp_cnt_summary(const struct rp_cnt_model *m, char *buf, int len);

#endif
