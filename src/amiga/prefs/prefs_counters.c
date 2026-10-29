/*
 * Riptide - RiptidePrefs: the Counters page's rows (see prefs_counters.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <string.h>
#include "prefs_counters.h"
#include "prefs_status.h"

/* a count of a record: where it is and what it is called */
struct cdef {
	unsigned short off;
	unsigned char kind;     /* RP_CNT_COUNT or RP_CNT_LEVEL */
	const char *name;
};
#define C(s, f, n) { offsetof(struct s, f), RP_CNT_COUNT, n }
#define L(s, f, n) { offsetof(struct s, f), RP_CNT_LEVEL, n }

static const struct cdef d_ip[] = {
	C(rt_ip_stats, InReceives, "Packets received"),
	C(rt_ip_stats, InDelivers, "Delivered to programs"),
	C(rt_ip_stats, InHdrErrors, "Bad headers"),
	C(rt_ip_stats, InAddrErrors, "Not for this Amiga"),
	C(rt_ip_stats, InUnknownProtos, "Unknown protocol"),
	C(rt_ip_stats, InDiscards, "Dropped on receive"),
	C(rt_ip_stats, InBcast, "Broadcasts received"),
	C(rt_ip_stats, OutRequests, "Packets sent"),
	C(rt_ip_stats, OutDiscards, "Dropped on send"),
	C(rt_ip_stats, OutNoRoutes, "No route to send"),
	C(rt_ip_stats, ForwDatagrams, "Forwarded"),
	C(rt_ip_stats, ReasmReqds, "Fragments received"),
	C(rt_ip_stats, ReasmOKs, "Packets put together"),
	C(rt_ip_stats, ReasmFails, "Could not be put together"),
	C(rt_ip_stats, ReasmTimeout, "Fragments timed out"),
	C(rt_ip_stats, FragOKs, "Packets split up"),
	C(rt_ip_stats, FragCreates, "Fragments sent"),
	C(rt_ip_stats, FragFails, "Could not be split up"),
};
static const struct cdef d_icmp[] = {
	C(rt_icmp_stats, InMsgs, "Messages received"),
	C(rt_icmp_stats, InErrors, "Bad messages"),
	C(rt_icmp_stats, InEchos, "Pings received"),
	C(rt_icmp_stats, InEchoReps, "Ping answers received"),
	C(rt_icmp_stats, InDestUnreachs, "Unreachable received"),
	C(rt_icmp_stats, InTimeExcds, "Time exceeded received"),
	C(rt_icmp_stats, InRedirects, "Redirects received"),
	C(rt_icmp_stats, OutMsgs, "Messages sent"),
	C(rt_icmp_stats, OutEchoReps, "Ping answers sent"),
	C(rt_icmp_stats, OutDestUnreachs, "Unreachable sent"),
	C(rt_icmp_stats, OutErrors, "Could not be sent"),
	C(rt_icmp_stats, OutRateLimited, "Held back (rate limit)"),
};
static const struct cdef d_udp[] = {
	C(rt_udp_stats, InDatagrams, "Datagrams received"),
	C(rt_udp_stats, OutDatagrams, "Datagrams sent"),
	C(rt_udp_stats, NoPorts, "For no open port"),
	C(rt_udp_stats, InErrors, "Bad datagrams"),
	C(rt_udp_stats, InCsumErrors, "Bad checksums"),
	C(rt_udp_stats, RcvbufErrors, "Dropped: buffer full"),
	C(rt_udp_stats, TxNoRoom, "Sends that waited (pacing)"),
	C(rt_udp_stats, MMsgDatagrams, "Sent in batches (SendMMsg)"),
};
static const struct cdef d_tcp[] = {
	L(rt_tcp_stats, CurrEstab, "Connections open now"),
	C(rt_tcp_stats, ActiveOpens, "Connections made"),
	C(rt_tcp_stats, PassiveOpens, "Connections accepted"),
	C(rt_tcp_stats, AttemptFails, "Connects that failed"),
	C(rt_tcp_stats, EstabResets, "Connections reset"),
	C(rt_tcp_stats, InSegs, "Segments received"),
	C(rt_tcp_stats, OutSegs, "Segments sent"),
	C(rt_tcp_stats, RetransSegs, "Segments sent again"),
	C(rt_tcp_stats, InErrs, "Bad segments"),
	C(rt_tcp_stats, InCsumErrors, "Bad checksums"),
	C(rt_tcp_stats, OutRsts, "Resets sent"),
	C(rt_tcp_stats, TCPTimeouts, "Retransmit timeouts"),
	C(rt_tcp_stats, TCPLossProbes, "Loss probes (TLP)"),
	C(rt_tcp_stats, TCPLossProbeRecovery, "Losses a probe repaired"),
	C(rt_tcp_stats, TCPRackLoss, "Losses found by RACK"),
	C(rt_tcp_stats, TCPFastRetrans, "Fast retransmits"),
	C(rt_tcp_stats, TCPSlowStartRetrans, "Retransmits in slow start"),
	C(rt_tcp_stats, TCPSackRecovery, "Recoveries with SACK"),
	C(rt_tcp_stats, TCPRenoRecovery, "Recoveries without SACK"),
	C(rt_tcp_stats, TCPSACKReorder, "Reordering seen"),
	C(rt_tcp_stats, TCPDSACKRecv, "Duplicates reported (D-SACK)"),
	C(rt_tcp_stats, TCPDSACKOldSent, "D-SACK sent (old data)"),
	C(rt_tcp_stats, TCPDSACKOfoSent, "D-SACK sent (out of order)"),
	C(rt_tcp_stats, TCPFullUndo, "Slowdowns undone"),
	C(rt_tcp_stats, TCPSpuriousRTOs, "Needless timeouts (F-RTO)"),
	C(rt_tcp_stats, TCPOFOQueue, "Segments out of order"),
	C(rt_tcp_stats, TCPOFODrop, "Out of order, dropped"),
	C(rt_tcp_stats, TCPRcvQDrop, "Dropped: receive buffer full"),
	C(rt_tcp_stats, TCPZeroWindowDrop, "Dropped: window closed"),
	C(rt_tcp_stats, DelayedACKs, "Delayed acknowledgements"),
	C(rt_tcp_stats, TCPPureAcks, "Acknowledgements received"),
	C(rt_tcp_stats, TCPHPHits, "Fast path: data"),
	C(rt_tcp_stats, TCPHPAcks, "Fast path: acknowledgements"),
	C(rt_tcp_stats, TCPWinProbe, "Window probes"),
	C(rt_tcp_stats, TCPKeepAlive, "Keepalive probes"),
	C(rt_tcp_stats, TCPChallengeACK, "Challenge ACKs (RFC 5961)"),
	C(rt_tcp_stats, TCPSYNChallenge, "Challenge ACKs for a SYN"),
	C(rt_tcp_stats, PAWSEstab, "Old segments (PAWS)"),
	C(rt_tcp_stats, TCPAbortOnData, "Aborted: data after close"),
	C(rt_tcp_stats, TCPAbortOnClose, "Aborted: closed with data"),
	C(rt_tcp_stats, TCPAbortOnTimeout, "Aborted: timed out"),
	C(rt_tcp_stats, TCPAbortOnLinger, "Aborted: linger"),
	C(rt_tcp_stats, ListenOverflows, "Listen queue full"),
	C(rt_tcp_stats, ListenDrops, "Connects dropped on listen"),
	C(rt_tcp_stats, SyncookiesSent, "SYN cookies sent"),
	C(rt_tcp_stats, SyncookiesRecv, "SYN cookies received"),
	C(rt_tcp_stats, SyncookiesFailed, "SYN cookies refused"),
	C(rt_tcp_stats, TimeWaitRecycled, "Closed connections reused"),
	C(rt_tcp_stats, TCPECNNegotiated, "ECN: connections using it"),
	C(rt_tcp_stats, TCPECNNotAgreed, "ECN: peer did not agree"),
	C(rt_tcp_stats, TCPECNFallback, "ECN: SYN sent again without"),
	C(rt_tcp_stats, TCPECNEceRcvd, "ECN: congestion reported"),
	C(rt_tcp_stats, TCPECNMarks, "ECN: marks received"),
	C(rt_tcp_stats, TCPMTUPBlackhole, "Path MTU black holes"),
	C(rt_tcp_stats, TCPMTUPSuccess, "Path MTU probes that worked"),
	C(rt_tcp_stats, TCPMTUPFail, "Path MTU probes that failed"),
	C(rt_tcp_stats, TCPFastOpenActive, "Fast Open: data in the SYN"),
	C(rt_tcp_stats, TCPFastOpenActiveFail, "Fast Open: refused"),
	C(rt_tcp_stats, TCPFastOpenCookieReqd, "Fast Open: cookies asked"),
	C(rt_tcp_stats, TCPFastOpenSynLost, "Fast Open: SYN lost"),
	C(rt_tcp_stats, TCPFastOpenBlackhole, "Fast Open: switched off"),
	C(rt_tcp_stats, TCPConnectTimeouts, "Connects timed out"),
	C(rt_tcp_stats, TCPConnectUnreach, "Connects ended: unreachable"),
	C(rt_tcp_stats, TCPCorkFlush, "Corked data sent when full"),
	C(rt_tcp_stats, TCPMemoryPressures, "Memory pressure reached"),
	C(rt_tcp_stats, TCPMemoryPressuresMs, "Time under pressure (ms)"),
	C(rt_tcp_stats, TCPMemLimitDrops, "Dropped: memory limit"),
};
static const struct cdef d_ip6[] = {
	C(rt_ip6_stats, InReceives, "Packets received"),
	C(rt_ip6_stats, InDelivers, "Delivered to programs"),
	C(rt_ip6_stats, InHdrErrors, "Bad headers"),
	C(rt_ip6_stats, InAddrErrors, "Not for this Amiga"),
	C(rt_ip6_stats, InUnknownProtos, "Unknown protocol"),
	C(rt_ip6_stats, InTruncatedPkts, "Cut short"),
	C(rt_ip6_stats, InTooBigErrors, "Too big"),
	C(rt_ip6_stats, InNoRoutes, "No route (received)"),
	C(rt_ip6_stats, InDiscards, "Dropped on receive"),
	C(rt_ip6_stats, InMcastPkts, "Multicasts received"),
	C(rt_ip6_stats, OutRequests, "Packets sent"),
	C(rt_ip6_stats, OutMcastPkts, "Multicasts sent"),
	C(rt_ip6_stats, OutDiscards, "Dropped on send"),
	C(rt_ip6_stats, OutNoRoutes, "No route to send"),
	C(rt_ip6_stats, OutForwDatagrams, "Forwarded"),
	C(rt_ip6_stats, ReasmReqds, "Fragments received"),
	C(rt_ip6_stats, ReasmOKs, "Packets put together"),
	C(rt_ip6_stats, ReasmFails, "Could not be put together"),
	C(rt_ip6_stats, ReasmTimeout, "Fragments timed out"),
	C(rt_ip6_stats, FragOKs, "Packets split up"),
	C(rt_ip6_stats, FragCreates, "Fragments sent"),
	C(rt_ip6_stats, FragFails, "Could not be split up"),
};
static const struct cdef d_icmp6[] = {
	C(rt_icmp6_stats, InMsgs, "Messages received"),
	C(rt_icmp6_stats, InErrors, "Bad messages"),
	C(rt_icmp6_stats, InEchos, "Pings received"),
	C(rt_icmp6_stats, InEchoReplies, "Ping answers received"),
	C(rt_icmp6_stats, InDestUnreachs, "Unreachable received"),
	C(rt_icmp6_stats, InPktTooBigs, "Too big received"),
	C(rt_icmp6_stats, InTimeExcds, "Time exceeded received"),
	C(rt_icmp6_stats, InParmProblems, "Parameter problems received"),
	C(rt_icmp6_stats, InRedirects, "Redirects received"),
	C(rt_icmp6_stats, InRouterAdvertisements, "Router adverts received"),
	C(rt_icmp6_stats, InRouterSolicits, "Router queries received"),
	C(rt_icmp6_stats, InNeighborSolicits, "Neighbor queries received"),
	C(rt_icmp6_stats, InNeighborAdvertisements, "Neighbor adverts received"),
	C(rt_icmp6_stats, InMLDQueries, "Multicast queries (MLD)"),
	C(rt_icmp6_stats, OutMsgs, "Messages sent"),
	C(rt_icmp6_stats, OutErrors, "Could not be sent"),
	C(rt_icmp6_stats, OutEchoReplies, "Ping answers sent"),
	C(rt_icmp6_stats, OutDestUnreachs, "Unreachable sent"),
	C(rt_icmp6_stats, OutPktTooBigs, "Too big sent"),
	C(rt_icmp6_stats, OutTimeExcds, "Time exceeded sent"),
	C(rt_icmp6_stats, OutParmProblems, "Parameter problems sent"),
	C(rt_icmp6_stats, OutRouterSolicits, "Router queries sent"),
	C(rt_icmp6_stats, OutNeighborSolicits, "Neighbor queries sent"),
	C(rt_icmp6_stats, OutNeighborAdvertisements, "Neighbor adverts sent"),
	C(rt_icmp6_stats, OutUnsolicitedNAs, "Address announcements"),
	C(rt_icmp6_stats, OutMLDv2Reports, "Multicast reports (MLDv2)"),
	C(rt_icmp6_stats, OutRateLimited, "Held back (rate limit)"),
	C(rt_icmp6_stats, DadDuplicates, "Duplicate addresses found"),
};
static const struct cdef d_igmp[] = {
	C(rt_igmp_stats, InMsgs, "Messages received"),
	C(rt_igmp_stats, InErrors, "Bad messages"),
	C(rt_igmp_stats, InQueriesV1, "Queries (version 1)"),
	C(rt_igmp_stats, InQueriesV2, "Queries (version 2)"),
	C(rt_igmp_stats, InQueriesV3, "Queries (version 3)"),
	C(rt_igmp_stats, InBadQueries, "Bad queries"),
	C(rt_igmp_stats, InReports, "Reports received"),
	C(rt_igmp_stats, InOurReports, "Reports for our groups"),
	C(rt_igmp_stats, InLeaves, "Leaves received"),
	C(rt_igmp_stats, OutMsgs, "Messages sent"),
	C(rt_igmp_stats, OutReportsV1, "Reports sent (version 1)"),
	C(rt_igmp_stats, OutReportsV2, "Reports sent (version 2)"),
	C(rt_igmp_stats, OutReportsV3, "Reports sent (version 3)"),
	C(rt_igmp_stats, OutLeaves, "Leaves sent"),
};
static const struct cdef d_if[] = {
	C(rt_ctl_iface, ipackets, "Packets received"),
	C(rt_ctl_iface, ibytes, "Bytes received"),
	C(rt_ctl_iface, ierrors, "Receive errors"),
	C(rt_ctl_iface, iqdrops, "Dropped on receive"),
	C(rt_ctl_iface, imcasts, "Multicasts received"),
	C(rt_ctl_iface, noproto, "Unknown protocol"),
	C(rt_ctl_iface, opackets, "Packets sent"),
	C(rt_ctl_iface, obytes, "Bytes sent"),
	C(rt_ctl_iface, oerrors, "Send errors"),
	C(rt_ctl_iface, omcasts, "Multicasts sent"),
	C(rt_ctl_iface, collisions, "Collisions"),
};
static const struct cdef d_drv[] = {
	C(rt_ctl_driver, rx_frames, "Frames received"),
	C(rt_ctl_driver, rx_bytes, "Bytes received"),
	C(rt_ctl_driver, rx_errors, "Receive errors"),
	C(rt_ctl_driver, rx_nobuf, "No buffer for a frame"),
	C(rt_ctl_driver, tx_frames, "Frames sent"),
	C(rt_ctl_driver, tx_bytes, "Bytes sent"),
	C(rt_ctl_driver, tx_errors, "Send errors"),
	C(rt_ctl_driver, tx_full, "Dropped: driver queue full"),
	C(rt_ctl_driver, drv_drops, "Dropped by the driver"),
	L(rt_ctl_driver, tx_inflight, "Frames at the driver now"),
	L(rt_ctl_driver, tx_inflight_max, "Most at the driver"),
	L(rt_ctl_driver, batch_max, "Most frames in one wakeup"),
	C(rt_ctl_driver, gs_baddata, "Driver: bad frames"),
	C(rt_ctl_driver, gs_overruns, "Driver: overruns"),
	C(rt_ctl_driver, gs_unknown, "Driver: unknown type"),
};
static const struct cdef d_dns[] = {
	C(rt_ctl_status, dns_hits, "Answers from the cache"),
	C(rt_ctl_status, dns_neg_hits, "Names not found (cached)"),
	C(rt_ctl_status, dns_fail_hits, "Failures (cached)"),
};
static const struct cdef d_mem[] = {
	L(rt_ctl_status, mem_used_kb, "Sockets use (KB)"),
	L(rt_ctl_status, mem_pressure_kb, "Pressure from (KB)"),
	L(rt_ctl_status, mem_high_kb, "Limit (KB)"),
	L(rt_ctl_status, buf_kb, "Packet buffers (KB)"),
};
#define N(a) (int)(sizeof(a) / sizeof(a[0]))

/* the rows being built, and the old ones their values carry over from */
struct build {
	struct rp_cnt_model *m;
	struct rp_cnt_row *old;
	int nold, hint;
};

/* a row like the old one of that name (looked for from where the last
 * one was found: the order rarely changes) */
static struct rp_cnt_row *add_row(struct build *b, const char *name, int kind, u32 val)
{
	struct rp_cnt_model *m = b->m;
	struct rp_cnt_row *r;
	int i, k;

	if (m->n >= RP_CNT_MAX)
		return NULL;
	r = &m->rows[m->n++];
	memset(r, 0, sizeof(*r));
	rt_strlcpy(r->name, name, sizeof(r->name));
	r->kind = (u8)kind;
	r->val = val;
	for (k = 0; k < b->nold; k++) {
		const struct rp_cnt_row *o;
		i = (b->hint + k) % b->nold;
		o = &b->old[i];
		if (o->kind != kind || strcmp(o->name, r->name))
			continue;
		b->hint = i + 1;
		r->have_prev = (u8)(m->have_prev && kind == RP_CNT_COUNT);
		r->prev = o->val;
		r->have_base = o->have_base;
		r->base = o->base;
		break;
	}
	if (m->zeroed && !r->have_base && kind == RP_CNT_COUNT) {
		/* new since Zero (a card that came up): counts from now on */
		r->have_base = 1;
		r->base = val;
	}
	if (r->have_prev && m->now_ms != m->prev_ms)
		r->rate = rp_rate(r->prev, r->val, m->now_ms - m->prev_ms);
	return r;
}

/* a heading and the counts of one record (len bytes: only what is in it) */
static void add_group(struct build *b, const char *head, const struct cdef *d, int n, const void *rec, u32 len)
{
	char name[RP_CNT_NAME];
	int i, head_len;

	if (!rec)
		return;
	add_row(b, head, RP_CNT_HEAD, 0);
	head_len = (int)strlen(head);
	for (i = 0; i < n; i++) {
		u32 v;
		if ((u32)d[i].off + 4 > len)
			continue;
		memcpy(&v, (const u8 *)rec + d[i].off, 4);
		/* the heading in the name keeps rows of different groups apart */
		rt_snprintf(name, sizeof(name), "%.*s\t%s", head_len, head, d[i].name);
		add_row(b, name, d[i].kind, v);
	}
}

static struct rp_cnt_row old_rows[RP_CNT_MAX];

void rp_cnt_init(struct rp_cnt_model *m)
{
	memset(m, 0, sizeof(*m));
}

void rp_cnt_stopped(struct rp_cnt_model *m)
{
	rp_cnt_init(m);
}

void rp_cnt_update(struct rp_cnt_model *m, const struct rp_cnt_input *in)
{
	struct build b;
	char head[RP_CNT_NAME];
	int i, k;

	memcpy(old_rows, m->rows, (size_t)m->n * sizeof(m->rows[0]));
	b.m = m;
	b.old = old_rows;
	b.nold = m->n;
	b.hint = 0;
	m->prev_ms = m->now_ms;
	m->now_ms = in->now_ms;
	m->have_prev = m->running && m->reads > 0;
	m->running = 1;
	m->reads++;
	m->n = 0;
	/* each card: its packets, its driver, its QoS bands */
	for (i = 0; i < in->nifaces && in->ifaces; i++) {
		const struct rt_ctl_iface *f = (const struct rt_ctl_iface *)(in->ifaces + (u32)i * in->ifrec);
		rt_snprintf(head, sizeof(head), "Interface %.15s", f->name);
		add_group(&b, head, d_if, N(d_if), f, in->ifrec);
		for (k = 0; k < in->ndrivers && in->drivers; k++) {
			const struct rt_ctl_driver *dv = (const struct rt_ctl_driver *)(in->drivers + (u32)k * in->drvrec);
			if (strncmp(dv->name, f->name, sizeof(dv->name)))
				continue;
			rt_snprintf(head, sizeof(head), "Driver of %.15s", f->name);
			add_group(&b, head, d_drv, N(d_drv), dv, in->drvrec);
		}
		for (k = 0; k < in->nqos && in->qos; k++) {
			static const char *const band[RT_QOS_BANDS] = { "interactive", "default", "bulk" };
			const struct rt_ctl_qos *q = (const struct rt_ctl_qos *)(in->qos + (u32)k * in->qosrec);
			char name[RP_CNT_NAME];
			int bnd;
			if (strncmp(q->ifname, f->name, sizeof(q->ifname)) || in->qosrec < sizeof(*q))
				continue;
			rt_snprintf(head, sizeof(head), "QoS of %.15s", f->name);
			add_row(&b, head, RP_CNT_HEAD, 0);
			for (bnd = 0; bnd < RT_QOS_BANDS; bnd++) {
				rt_snprintf(name, sizeof(name), "%s\tPackets %s", head, band[bnd]);
				add_row(&b, name, RP_CNT_COUNT, q->packets[bnd]);
				rt_snprintf(name, sizeof(name), "%s\tDropped %s", head, band[bnd]);
				add_row(&b, name, RP_CNT_COUNT, q->drops[bnd]);
			}
			rt_snprintf(name, sizeof(name), "%s\tStraight to the driver", head);
			add_row(&b, name, RP_CNT_COUNT, q->bypass);
		}
	}
	add_group(&b, "IPv4", d_ip, N(d_ip), in->ip, in->ip_len);
	add_group(&b, "ICMP", d_icmp, N(d_icmp), in->icmp, in->icmp_len);
	add_group(&b, "IGMP (multicast)", d_igmp, N(d_igmp), in->igmp, in->igmp_len);
	add_group(&b, "UDP", d_udp, N(d_udp), in->udp, in->udp_len);
	add_group(&b, "TCP", d_tcp, N(d_tcp), in->tcp, in->tcp_len);
	add_group(&b, "IPv6", d_ip6, N(d_ip6), in->ip6, in->ip6_len);
	add_group(&b, "ICMPv6, Neighbor Discovery, MLD", d_icmp6, N(d_icmp6), in->icmp6, in->icmp6_len);
	add_group(&b, "Names (DNS cache)", d_dns, N(d_dns), in->st, in->st_len);
	add_group(&b, "Memory", d_mem, N(d_mem), in->st, in->st_len);
}

void rp_cnt_zero(struct rp_cnt_model *m)
{
	int i;

	for (i = 0; i < m->n; i++) {
		m->rows[i].base = m->rows[i].val;
		m->rows[i].have_base = 1;
	}
	m->zeroed = 1;
	m->zero_ms = m->now_ms;
}

void rp_cnt_since_start(struct rp_cnt_model *m)
{
	int i;

	for (i = 0; i < m->n; i++)
		m->rows[i].have_base = 0;
	m->zeroed = 0;
}

void rp_cnt_text(const struct rp_cnt_model *m, int i, char *name, int nlen, char *val, int vlen, char *rate, int rlen)
{
	const struct rp_cnt_row *r = &m->rows[i];
	const char *tab = strchr(r->name, '\t');

	val[0] = rate[0] = 0;
	rt_strlcpy(name, tab ? tab + 1 : r->name, (size_t)nlen);
	if (r->kind == RP_CNT_HEAD)
		return;
	rp_fmt_count(val, vlen, m->zeroed && r->have_base && r->kind == RP_CNT_COUNT ? r->val - r->base : r->val);
	if (r->kind == RP_CNT_COUNT && r->have_prev && r->rate) {
		char t[24];
		rp_fmt_count(t, sizeof(t), r->rate);
		rt_snprintf(rate, (size_t)rlen, "+%s/s", t);
	}
}

void rp_cnt_summary(const struct rp_cnt_model *m, char *buf, int len)
{
	char t[32];

	if (!m->running) {
		rt_strlcpy(buf, "Riptide is not running.", (size_t)len);
	} else if (m->zeroed) {
		rp_fmt_dur(t, sizeof(t), (m->now_ms - m->zero_ms) / 1000);
		rt_snprintf(buf, (size_t)len, "Counts since Zero, %s ago.", t);
	} else {
		rt_strlcpy(buf, "Counts since Riptide started.", (size_t)len);
	}
}
