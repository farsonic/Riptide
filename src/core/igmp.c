/*
 * Riptide - IPv4 multicast group membership and IGMP host side
 * (RFC 1112 IGMPv1, RFC 2236 IGMPv2, RFC 3376 IGMPv3).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Memberships are per interface and reference counted (sockets and
 * internal users). Riptide is a host, not a multicast router: it reports
 * its groups to the network's querier and answers queries.
 *
 * IGMPv3 is spoken by default. Its state is the simplest the protocol
 * allows: every group in EXCLUDE mode with an empty source list (any
 * source), as no source filter API is offered. Joins and leaves are sent
 * as state change reports (CHANGE_TO_EXCLUDE / CHANGE_TO_INCLUDE)
 * Robustness times, general queries are answered with one MODE_IS_EXCLUDE
 * record per group, group-and-source queries with MODE_IS_INCLUDE of the
 * queried sources (they all pass an empty exclude list). When a v1 or v2
 * querier is heard the interface falls back to that version (RFC 3376
 * 7.2.1) until the Older Version Querier Present timeout: v1/v2 reports to
 * the group with report suppression, v2 leaves to 224.0.0.2.
 * net.inet.igmp.version forces a version (0 = automatic).
 *
 * 224.0.0.1 is joined on every multicast capable interface and never
 * reported. Messages carry the Router Alert option, TTL 1 and TOS 0xc0.
 */
#include "infra.h"

#define IGMP_QUERY       0x11
#define IGMP_V1_REPORT   0x12
#define IGMP_V2_REPORT   0x16
#define IGMP_V2_LEAVE    0x17
#define IGMP_V3_REPORT   0x22

#define IGMP_ROBUSTNESS        2
#define IGMP_V3_UNSOL_MS       1000u    /* RFC 3376 8.11 */
#define IGMP_V12_UNSOL_MS      10000u   /* RFC 2236 8.10 */
#define IGMP_V1_PRESENT_MS     400000u  /* RFC 2236 8.12 */
#define IGMP_OLDER_PRESENT_MS  260000u  /* RFC 3376 8.12: 2 * 125 s + 10 s */
#define IGMP_V1_MAXRESP_MS     10000u
#define IGMP_MAX_SOURCES       16       /* pending group-and-source query sources */

/* v3 record types (RFC 3376 4.2.12) */
#define MODE_IS_INCLUDE    1
#define MODE_IS_EXCLUDE    2
#define CHANGE_TO_INCLUDE  3
#define CHANGE_TO_EXCLUDE  4

s32 rt_igmp_version = 0;
struct rt_igmp_stats rt_igmpstat;

struct in_multi {
	struct in_multi *next;
	u32 grp;                 /* network order */
	u32 refcnt;              /* 0: left, still reporting the leave (v3) */
	u32 resp_ms;             /* pending report (query response / v1/v2 unsolicited), 0 none */
	u8  sc_left;             /* v3 state change reports still to send */
	u8  unsol;               /* v1/v2 unsolicited reports still to send */
	u8  reporter;            /* v1/v2: we sent the last report */
	u8  nsrc;                /* pending group-and-source query: sources, */
	u8  src_overflow;        /*   or too many: answer as a group query */
	u32 src[IGMP_MAX_SOURCES];
};

struct igmp_if {
	struct igmp_if *next;
	struct rt_netif *nif;
	struct in_multi *mcast;
	struct rt_timer timer;
	u32 v1_until_ms, v2_until_ms;   /* older querier present, 0 = not */
	u32 gen_resp_ms;                /* v3 general query response, 0 none */
	u32 sc_next_ms;                 /* next v3 state change report, 0 none */
	u8  qrv;                        /* robustness learned from v3 queries */
};

static struct igmp_if *igmp_ifs;

static const struct rt_sysctl igmp_sysctl[] = {
	{ "net.inet.igmp.version", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_igmp_version, 0, 3, NULL, "IGMP version to speak (0 = 3, falling back to older queriers)" },
};

static u32 nz(u32 t) { return t ? t : 1; }

static int due(u32 t, u32 now) { return t && (s32)(now - t) >= 0; }

static struct igmp_if *igmp_ifp(const struct rt_netif *nif)
{
	struct igmp_if *ifp;

	for (ifp = igmp_ifs; ifp; ifp = ifp->next)
		if (ifp->nif == nif)
			return ifp;
	return NULL;
}

static struct in_multi *mc_find(const struct igmp_if *ifp, u32 grp)
{
	struct in_multi *m;

	for (m = ifp->mcast; m; m = m->next)
		if (m->grp == grp)
			return m;
	return NULL;
}

/* IGMP never reports 224.0.0.1 (RFC 2236 6, RFC 3376 5) */
static int mc_reportable(const struct igmp_if *ifp, u32 grp)
{
	return grp != rt_htonl(RT_INADDR_ALLHOSTS_GROUP) && !(ifp->nif->flags & RT_IFF_LOOPBACK);
}

static int igmp_active(const struct igmp_if *ifp)
{
	return (ifp->nif->flags & RT_IFF_UP) && !(ifp->nif->flags & RT_IFF_LOOPBACK);
}

/* The version spoken on the interface (Host Compatibility Mode) */
static int igmp_mode(const struct igmp_if *ifp)
{
	u32 now = rt_now_ms();
	int v = 3;

	if (ifp->v1_until_ms && (s32)(ifp->v1_until_ms - now) > 0)
		v = 1;
	else if (ifp->v2_until_ms && (s32)(ifp->v2_until_ms - now) > 0)
		v = 2;
	if (rt_igmp_version > 0 && rt_igmp_version < v)
		v = (int)rt_igmp_version;
	return v;
}

static void mc_hwaddr(u8 *mac, u32 grp)
{
	u32 h = rt_ntohl(grp);

	mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
	mac[3] = (u8)((h >> 16) & 0x7f); mac[4] = (u8)(h >> 8); mac[5] = (u8)h;
}

/* Another joined group on the same 01:00:5e address (32 groups share one)? */
static int mc_mac_shared(const struct igmp_if *ifp, const struct in_multi *self)
{
	const struct in_multi *m;

	for (m = ifp->mcast; m; m = m->next)
		if (m != self && m->refcnt && ((m->grp ^ self->grp) & rt_htonl(0x007fffffu)) == 0)
			return 1;
	return 0;
}

static void mc_filter(struct igmp_if *ifp, struct in_multi *m, int join)
{
	struct rt_netif *nif = ifp->nif;
	u8 mac[6];

	if (!nif->mcast || nif->type != RT_IFT_ETHER || mc_mac_shared(ifp, m))
		return;
	mc_hwaddr(mac, m->grp);
	nif->mcast(nif, mac, join);
}

static void mc_unlink(struct igmp_if *ifp, struct in_multi *m)
{
	struct in_multi **pp;

	for (pp = &ifp->mcast; *pp; pp = &(*pp)->next) {
		if (*pp == m) {
			*pp = m->next;
			break;
		}
	}
	rt_free(m);
}

/* ---- timer ---- */

static void igmp_timer_fn(void *arg);

static void igmp_rearm(struct igmp_if *ifp)
{
	const struct in_multi *m;
	u32 now = rt_now_ms(), next = 0;
	int have = 0;

#define CONSIDER(t) do { if ((t) && (!have || (s32)((t) - next) < 0)) { next = (t); have = 1; } } while (0)
	CONSIDER(ifp->gen_resp_ms);
	CONSIDER(ifp->sc_next_ms);
	for (m = ifp->mcast; m; m = m->next)
		CONSIDER(m->resp_ms);
#undef CONSIDER
	if (!have) {
		rt_timer_del(&ifp->timer);
		return;
	}
	if ((s32)(next - now) < 0)
		next = now;
	rt_timer_mod(&ifp->timer, next);
}

/* ---- output ---- */

/* Send an IGMP message (complete, checksum filled in here) from nif with
 * the Router Alert option (RFC 2113), TTL 1, TOS 0xc0 (RFC 3376 4). */
static void igmp_send(struct igmp_if *ifp, struct rt_buf *b, u32 dst)
{
	struct rt_netif *nif = ifp->nif;
	struct rt_iphdr *ip;
	u8 *opt;

	*(u16 *)(void *)(b->data + 2) = 0;
	*(u16 *)(void *)(b->data + 2) = rt_inet_csum(b->data, b->len);
	opt = rt_buf_push(b, 4);
	opt[0] = 0x94; opt[1] = 4; opt[2] = 0; opt[3] = 0;
	ip = (struct rt_iphdr *)rt_buf_push(b, 20);
	ip->ver_ihl = 0x46;
	ip->tos = 0xc0;
	ip->tot_len = rt_htons((u16)b->len);
	ip->id = rt_htons(rt_ip_select_id(dst));
	ip->frag_off = rt_htons(RT_IP_DF);
	ip->ttl = 1;
	ip->protocol = RT_IPPROTO_IGMP;
	ip->check = 0;
	ip->saddr = nif->addr;
	ip->daddr = dst;
	ip->check = rt_inet_csum(ip, 24);
	rt_ipstat.OutRequests++;
	rt_igmpstat.OutMsgs++;
	rt_netif_output_ip(nif, b, dst);
}

static struct rt_buf *igmp_alloc(u32 len)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, len);

	if (b)
		memset(rt_buf_put(b, len), 0, len);
	return b;
}

/* v1/v2 report or v2 leave for one group */
static void igmp_send_v12(struct igmp_if *ifp, u8 type, u32 grp)
{
	struct rt_buf *b = igmp_alloc(8);

	if (!b)
		return;
	b->data[0] = type;
	memcpy(b->data + 4, &grp, 4);
	if (type == IGMP_V1_REPORT)
		rt_igmpstat.OutReportsV1++;
	else if (type == IGMP_V2_REPORT)
		rt_igmpstat.OutReportsV2++;
	else
		rt_igmpstat.OutLeaves++;
	igmp_send(ifp, b, type == IGMP_V2_LEAVE ? rt_htonl(RT_INADDR_ALLRTRS_GROUP) : grp);
}

/* What a v3 report says about m:
 *   1: state change records, 2: general query response, 3: query for m */
static u8 v3_rectype(const struct in_multi *m, int what)
{
	if (what == 1)
		return m->refcnt ? CHANGE_TO_EXCLUDE : CHANGE_TO_INCLUDE;
	if (what == 3 && m->nsrc && !m->src_overflow)
		return MODE_IS_INCLUDE;
	return MODE_IS_EXCLUDE;
}

static int v3_wanted(const struct igmp_if *ifp, const struct in_multi *m, int what)
{
	if (!mc_reportable(ifp, m->grp))
		return 0;
	if (what == 1)
		return m->sc_left > 0;
	return m->refcnt > 0;
}

/* IGMPv3 report to 224.0.0.22 for the groups selected by 'what' (only 'one'
 * for what == 3), split over as many datagrams as the MTU needs. */
static void igmp_send_v3(struct igmp_if *ifp, int what, struct in_multi *one)
{
	struct in_multi *m = what == 3 ? one : ifp->mcast;
	u32 room = ifp->nif->mtu > 24 + 8 ? ifp->nif->mtu - 24 - 8 : 0;

	while (m) {
		struct in_multi *s;
		struct rt_buf *b;
		u32 len = 8, n = 0;
		u8 *p;

		/* size this datagram */
		for (s = m; s; s = what == 3 ? NULL : s->next) {
			u32 rl;
			if (!v3_wanted(ifp, s, what))
				continue;
			rl = 8 + (v3_rectype(s, what) == MODE_IS_INCLUDE ? 4u * s->nsrc : 0);
			if (n && len - 8 + rl > room)
				break;
			len += rl;
			n++;
		}
		if (!n)
			return;
		b = igmp_alloc(len);
		if (!b)
			return;
		p = b->data;
		p[0] = IGMP_V3_REPORT;
		rt_put_be16(p + 6, (u16)n);
		p += 8;
		for (; m && n; m = what == 3 ? NULL : m->next) {
			u8 t;
			if (!v3_wanted(ifp, m, what))
				continue;
			t = v3_rectype(m, what);
			p[0] = t;
			memcpy(p + 4, &m->grp, 4);
			if (t == MODE_IS_INCLUDE) {
				rt_put_be16(p + 2, m->nsrc);
				memcpy(p + 8, m->src, 4u * m->nsrc);
				p += 4u * m->nsrc;
			}
			p += 8;
			n--;
		}
		rt_igmpstat.OutReportsV3++;
		igmp_send(ifp, b, rt_htonl(RT_INADDR_IGMPV3_GROUP));
		if (what == 3)
			return;
	}
}

/* ---- state changes ---- */

static void schedule_change(struct igmp_if *ifp, struct in_multi *m, int join)
{
	u32 now = rt_now_ms();
	int v = igmp_mode(ifp);

	m->resp_ms = 0;
	m->nsrc = m->src_overflow = 0;
	if (!igmp_active(ifp))
		return;
	if (v == 3) {
		/* RFC 3376 5.1: report at once, repeat [Robustness - 1] times
		 * at random intervals within the Unsolicited Report Interval */
		m->sc_left = ifp->qrv ? ifp->qrv : IGMP_ROBUSTNESS;
		m->unsol = 0;
		ifp->sc_next_ms = nz(now);
	} else if (join) {
		/* RFC 2236 3: report at once and once more after up to 10 s */
		m->unsol = IGMP_ROBUSTNESS;
		m->sc_left = 0;
		m->resp_ms = nz(now);
	}
	igmp_rearm(ifp);
}

static struct igmp_if *igmp_ifp_get(struct rt_netif *nif);

int rt_ip_mc_join(struct rt_netif *nif, u32 group)
{
	struct igmp_if *ifp;
	struct in_multi *m;

	if (!nif || !rt_ip_is_multicast(group))
		return -RT_EINVAL;
	if (!(nif->flags & RT_IFF_MULTICAST))
		return -RT_EADDRNOTAVAIL;
	ifp = igmp_ifp_get(nif);
	if (!ifp)
		return -RT_ENOMEM;
	m = mc_find(ifp, group);
	if (m && m->refcnt) {
		m->refcnt++;
		return 0;
	}
	if (!m) {
		m = rt_zalloc(sizeof(*m));
		if (!m)
			return -RT_ENOMEM;
		m->grp = group;
		m->next = ifp->mcast;
		ifp->mcast = m;
	}
	m->refcnt = 1;
	mc_filter(ifp, m, 1);
	if (mc_reportable(ifp, group))
		schedule_change(ifp, m, 1);
	return 0;
}

int rt_ip_mc_leave(struct rt_netif *nif, u32 group)
{
	struct igmp_if *ifp = nif ? igmp_ifp(nif) : NULL;
	struct in_multi *m = ifp ? mc_find(ifp, group) : NULL;

	if (!m || !m->refcnt)
		return -RT_EADDRNOTAVAIL;
	if (--m->refcnt)
		return 0;
	mc_filter(ifp, m, 0);
	if (mc_reportable(ifp, group) && igmp_active(ifp)) {
		int v = igmp_mode(ifp);
		if (v == 3) {
			schedule_change(ifp, m, 0);   /* the record goes after the reports */
			return 0;
		}
		/* RFC 2236 6: leave if we were the last to report */
		if (v == 2 && m->reporter)
			igmp_send_v12(ifp, IGMP_V2_LEAVE, group);
	}
	mc_unlink(ifp, m);
	igmp_rearm(ifp);
	return 0;
}

int rt_ip_mc_joined(const struct rt_netif *nif, u32 group)
{
	const struct igmp_if *ifp = nif ? igmp_ifp(nif) : NULL;
	const struct in_multi *m = ifp ? mc_find(ifp, group) : NULL;

	return m && m->refcnt;
}

int rt_ip_mc_member(const struct rt_netif *nif, u32 group)
{
	return group == rt_htonl(RT_INADDR_ALLHOSTS_GROUP) || rt_ip_mc_joined(nif, group);
}

void rt_ip_mc_flush(struct rt_netif *nif)
{
	rt_igmp_if_remove(nif);
}

struct rt_netif *rt_ip_mc_default_if(u32 group)
{
	struct rt_nexthop nh;
	struct rt_netif *n;

	if (rt_ip_route(group, &nh) == 0 && nh.netif && (nh.netif->flags & RT_IFF_MULTICAST))
		return nh.netif;
	for (n = rt_netif_list(); n; n = n->next)
		if ((n->flags & (RT_IFF_UP | RT_IFF_MULTICAST)) == (RT_IFF_UP | RT_IFF_MULTICAST) &&
		    !(n->flags & RT_IFF_LOOPBACK))
			return n;
	return NULL;
}

static struct igmp_if *igmp_ifp_get(struct rt_netif *nif)
{
	struct igmp_if *ifp = igmp_ifp(nif);

	if (ifp || !(nif->flags & RT_IFF_MULTICAST))
		return ifp;
	ifp = rt_zalloc(sizeof(*ifp));
	if (!ifp)
		return NULL;
	ifp->nif = nif;
	rt_timer_init(&ifp->timer, igmp_timer_fn, ifp);
	ifp->next = igmp_ifs;
	igmp_ifs = ifp;
	rt_ip_mc_join(nif, rt_htonl(RT_INADDR_ALLHOSTS_GROUP));
	return ifp;
}

/* ---- input ---- */

/* Report m within [0, maxresp] ms unless already due sooner. */
static void respond_within(u32 *t, u32 maxresp)
{
	u32 at = rt_now_ms() + (maxresp ? rt_random32() % (maxresp + 1) : 0);

	if (!*t || (s32)(at - *t) < 0)
		*t = nz(at);
}

/* Host Compatibility Mode changed: pending v3 work is cancelled
 * (RFC 3376 7.2.1); records of left groups are dropped */
static void mode_changed(struct igmp_if *ifp)
{
	struct in_multi *m, *n;

	ifp->gen_resp_ms = 0;
	ifp->sc_next_ms = 0;
	for (m = ifp->mcast; m; m = n) {
		n = m->next;
		m->sc_left = 0;
		m->resp_ms = 0;
		m->nsrc = m->src_overflow = 0;
		if (!m->refcnt)
			mc_unlink(ifp, m);
	}
}

static void igmp_query(struct igmp_if *ifp, const u8 *p, u32 len)
{
	struct in_multi *m;
	u32 grp, maxresp, nsrc = 0;
	int was = igmp_mode(ifp);

	memcpy(&grp, p + 4, 4);
	if (len == 8) {
		if (p[1] == 0) {
			rt_igmpstat.InQueriesV1++;
			maxresp = IGMP_V1_MAXRESP_MS;
			ifp->v1_until_ms = nz(rt_now_ms() + IGMP_V1_PRESENT_MS);
		} else {
			rt_igmpstat.InQueriesV2++;
			maxresp = p[1] * 100u;
			ifp->v2_until_ms = nz(rt_now_ms() + IGMP_OLDER_PRESENT_MS);
		}
	} else if (len >= 12) {
		u8 code = p[1];
		/* RFC 3376 4.1.1: exponential form from 128 */
		maxresp = 100u * (code < 128 ? code : (u32)((code & 0x0f) | 0x10) << (((code >> 4) & 7) + 3));
		nsrc = rt_get_be16(p + 10);
		if (len < 12 + 4 * nsrc) {
			rt_igmpstat.InBadQueries++;
			return;
		}
		rt_igmpstat.InQueriesV3++;
		ifp->qrv = (u8)(p[8] & 7);
	} else {
		rt_igmpstat.InBadQueries++;
		return;
	}
	if (grp && !rt_ip_is_multicast(grp)) {
		rt_igmpstat.InBadQueries++;
		return;
	}
	if (igmp_mode(ifp) != was)
		mode_changed(ifp);
	if (igmp_mode(ifp) == 3) {
		if (!grp) {
			respond_within(&ifp->gen_resp_ms, maxresp);
		} else if ((m = mc_find(ifp, grp)) != NULL && m->refcnt && mc_reportable(ifp, grp)) {
			/* RFC 3376 5.2: a group query wins over source queries */
			int pending = m->resp_ms != 0;
			if (!nsrc) {
				m->src_overflow = 1;
			} else if (!pending || m->nsrc || m->src_overflow) {
				u32 i, k;
				for (i = 0; i < nsrc && !m->src_overflow; i++) {
					u32 s;
					memcpy(&s, p + 12 + 4 * i, 4);
					for (k = 0; k < m->nsrc && m->src[k] != s; k++)
						;
					if (k < m->nsrc)
						continue;
					if (m->nsrc >= IGMP_MAX_SOURCES)
						m->src_overflow = 1;
					else
						m->src[m->nsrc++] = s;
				}
			}
			respond_within(&m->resp_ms, maxresp);
		}
	} else {
		/* RFC 2236 3: v3 queries count as v2 queries here */
		for (m = ifp->mcast; m; m = m->next)
			if (m->refcnt && mc_reportable(ifp, m->grp) && (!grp || grp == m->grp))
				respond_within(&m->resp_ms, maxresp);
	}
	igmp_rearm(ifp);
}

void rt_igmp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	struct rt_netif *nif = b->netif;
	struct igmp_if *ifp = nif ? igmp_ifp(nif) : NULL;
	const u8 *p = b->data;
	u32 len = b->len, grp;
	struct in_multi *m;

	rt_igmpstat.InMsgs++;
	if (len < 8 || rt_inet_csum(p, len) != 0) {
		rt_igmpstat.InErrors++;
		goto out;
	}
	if (!ifp || (nif->flags & RT_IFF_LOOPBACK))
		goto out;
	switch (p[0]) {
	case IGMP_QUERY:
		igmp_query(ifp, p, len);
		break;
	case IGMP_V1_REPORT:
	case IGMP_V2_REPORT:
		rt_igmpstat.InReports++;
		memcpy(&grp, p + 4, 4);
		m = mc_find(ifp, grp);
		if (!m || !m->refcnt || rt_ip_is_local(ip->saddr))
			break;
		rt_igmpstat.InOurReports++;
		/* RFC 2236 3: another member answered, suppress ours */
		if (igmp_mode(ifp) < 3 && m->resp_ms) {
			m->resp_ms = 0;
			m->unsol = 0;
			m->reporter = 0;
			igmp_rearm(ifp);
		}
		break;
	case IGMP_V2_LEAVE:
		rt_igmpstat.InLeaves++;
		break;
	case IGMP_V3_REPORT:
		rt_igmpstat.InReports++;
		break;
	}
out:
	rt_buf_free(b);
}

static void igmp_timer_fn(void *arg)
{
	struct igmp_if *ifp = arg;
	struct in_multi *m, *n;
	u32 now = rt_now_ms();
	int v = igmp_mode(ifp);

	if (!igmp_active(ifp))
		return;
	if (v != 3 && (ifp->sc_next_ms || ifp->gen_resp_ms))
		mode_changed(ifp);       /* net.inet.igmp.version lowered */
	if (v == 3 && due(ifp->sc_next_ms, now)) {
		int more = 0;
		igmp_send_v3(ifp, 1, NULL);
		for (m = ifp->mcast; m; m = n) {
			n = m->next;
			if (m->sc_left && --m->sc_left)
				more = 1;
			if (!m->refcnt && !m->sc_left)
				mc_unlink(ifp, m);
		}
		ifp->sc_next_ms = more ? nz(now + 1 + rt_random32() % IGMP_V3_UNSOL_MS) : 0;
	}
	if (v == 3 && due(ifp->gen_resp_ms, now)) {
		ifp->gen_resp_ms = 0;
		igmp_send_v3(ifp, 2, NULL);
	}
	for (m = ifp->mcast; m; m = m->next) {
		if (!due(m->resp_ms, now))
			continue;
		m->resp_ms = 0;
		if (!m->refcnt)
			continue;
		if (v == 3) {
			igmp_send_v3(ifp, 3, m);
			m->nsrc = m->src_overflow = 0;
			continue;
		}
		igmp_send_v12(ifp, v == 1 ? IGMP_V1_REPORT : IGMP_V2_REPORT, m->grp);
		m->reporter = 1;
		if (m->unsol && --m->unsol)
			m->resp_ms = nz(now + 1 + rt_random32() % IGMP_V12_UNSOL_MS);
	}
	igmp_rearm(ifp);
}

/* ---- interface events ---- */

void rt_igmp_if_up(struct rt_netif *nif)
{
	struct igmp_if *ifp = igmp_ifp_get(nif);
	struct in_multi *m;

	if (!ifp)
		return;
	/* (re)announce: the link may lead to another network now */
	for (m = ifp->mcast; m; m = m->next)
		if (m->refcnt && mc_reportable(ifp, m->grp))
			schedule_change(ifp, m, 1);
}

void rt_igmp_if_down(struct rt_netif *nif)
{
	struct igmp_if *ifp = igmp_ifp(nif);

	if (!ifp)
		return;
	ifp->v1_until_ms = ifp->v2_until_ms = 0;
	ifp->qrv = 0;
	mode_changed(ifp);
	rt_timer_del(&ifp->timer);
}

void rt_igmp_if_remove(struct rt_netif *nif)
{
	struct igmp_if **pp, *ifp;
	struct in_multi *m;

	for (pp = &igmp_ifs; (ifp = *pp) != NULL; pp = &ifp->next)
		if (ifp->nif == nif)
			break;
	if (!ifp)
		return;
	*pp = ifp->next;
	rt_timer_del(&ifp->timer);
	while ((m = ifp->mcast) != NULL) {
		if (m->refcnt) {
			m->refcnt = 0;
			mc_filter(ifp, m, 0);
		}
		ifp->mcast = m->next;
		rt_free(m);
	}
	rt_free(ifp);
}

/* ---- tables ---- */

static int fill(struct rt_ctl_mcast *out, int max, int n, const struct rt_netif *nif)
{
	if (n < max) {
		memset(&out[n], 0, sizeof(out[n]));
		rt_strlcpy(out[n].ifname, nif->name, sizeof(out[n].ifname));
		out[n].ifindex = nif->index;
	}
	return n < max;
}

int rt_mcast_list(struct rt_ctl_mcast *out, int max)
{
	const struct rt_netif *nif;
	int n = 0;

	for (nif = rt_netif_list(); nif; nif = nif->next) {
		const struct igmp_if *ifp = igmp_ifp(nif);
		const struct in_multi *m;
		rt_in6_addr *g6;
		u32 *ref6;
		int i, k;

		for (m = ifp ? ifp->mcast : NULL; m; m = m->next) {
			if (!m->refcnt)
				continue;
			if (fill(out, max, n, nif)) {
				struct rt_ctl_mcast *r = &out[n];
				r->family = RT_AF_INET;
				r->igmp = mc_reportable(ifp, m->grp) ? (u8)igmp_mode(ifp) : 0;
				r->reporter = m->reporter;
				r->refcnt = m->refcnt;
				r->group[10] = r->group[11] = 0xff;
				memcpy(r->group + 12, &m->grp, 4);
				if (nif->type == RT_IFT_ETHER) {
					r->hwlen = 6;
					mc_hwaddr(r->hwaddr, m->grp);
				}
			}
			n++;
		}
		k = rt_ip6_mc_list(nif, NULL, NULL, 0);
		if (!k)
			continue;
		if (n >= max) {
			n += k;
			continue;
		}
		g6 = rt_malloc((u32)k * (sizeof(*g6) + sizeof(*ref6)));
		if (!g6)
			continue;
		ref6 = (u32 *)(void *)(g6 + k);
		k = rt_ip6_mc_list(nif, g6, ref6, k);
		for (i = 0; i < k; i++, n++) {
			struct rt_ctl_mcast *r;
			if (!fill(out, max, n, nif))
				continue;
			r = &out[n];
			r->family = RT_AF_INET6;
			r->refcnt = ref6[i];
			memcpy(r->group, g6[i].b, 16);
			if (nif->type == RT_IFT_ETHER) {
				r->hwlen = 6;
				rt_in6_mc_mac(r->hwaddr, &g6[i]);
			}
		}
		rt_free(g6);
	}
	return n;
}

void rt_igmp_init(void)
{
	memset(&rt_igmpstat, 0, sizeof(rt_igmpstat));
	rt_sysctl_register(igmp_sysctl, (int)RT_ARRAY_SIZE(igmp_sysctl));
}
