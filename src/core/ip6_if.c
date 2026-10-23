/*
 * Riptide - IPv6 interface state: addresses and their lifetimes, duplicate
 * address detection and stateless autoconfiguration (RFC 4862), stable
 * interface identifiers (RFC 7217), temporary addresses (RFC 8981),
 * multicast membership with MLDv2 reports (RFC 3810, MLDv1 fallback) and
 * source address selection (RFC 6724).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Interface identifiers: net.inet6.ip6.addr_gen_mode 1 (default) derives
 * them as RFC 7217 recommends, F(prefix, interface name, DAD counter,
 * secret) with SipHash-2-4 as F. The secret is net.inet6.ip6.stable_secret
 * when set (any text; set it in riptide.conf to get addresses that do not
 * reveal the hardware address). Without it the key is derived from the
 * interface's MAC address: the addresses are stable, differ per prefix and
 * do not embed the MAC, but someone who knows the MAC and this algorithm
 * can recompute them. Mode 0 uses modified EUI-64 (RFC 4291 2.5.1).
 */
#include "ip6_internal.h"

#define MAX_RTR_SOLICITATIONS     3
#define RTR_SOLICITATION_INTERVAL 4000u
#define MAX_RTR_SOLICIT_DELAY     1000u
#define REACHABLE_TIME            30000u
#define RETRANS_TIMER             1000u
#define MLD_ROBUSTNESS            2
#define MLD_UNSOL_INTERVAL        1000u
#define MLD_V1_PRESENT_S          260u    /* RFC 3810 9.12 Older Version Querier Present Timeout */
#define MLD_MAX_RECORDS           50
#define IDGEN_RETRIES             3
#define TEMP_VALID_S              (2u * 86400u)   /* RFC 8981 3.8 */
#define TEMP_PREFERRED_S          86400u
#define TEMP_REGEN_ADVANCE_S      5u

/* ---- SipHash-2-4 ---- */
static u64 rotl64(u64 x, int b) { return (x << b) | (x >> (64 - b)); }

static void sipround(u64 *v)
{
	v[0] += v[1]; v[1] = rotl64(v[1], 13); v[1] ^= v[0]; v[0] = rotl64(v[0], 32);
	v[2] += v[3]; v[3] = rotl64(v[3], 16); v[3] ^= v[2];
	v[0] += v[3]; v[3] = rotl64(v[3], 21); v[3] ^= v[0];
	v[2] += v[1]; v[1] = rotl64(v[1], 17); v[1] ^= v[2]; v[2] = rotl64(v[2], 32);
}

static u64 le64(const u8 *p)
{
	u64 v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

u64 rt_siphash(const void *data, u32 len, const u8 key[16])
{
	const u8 *p = data;
	u64 k0 = le64(key), k1 = le64(key + 8), v[4], m;
	u8 last[8];
	u32 i, left;

	v[0] = k0 ^ 0x736f6d6570736575ULL;
	v[1] = k1 ^ 0x646f72616e646f6dULL;
	v[2] = k0 ^ 0x6c7967656e657261ULL;
	v[3] = k1 ^ 0x7465646279746573ULL;
	for (i = 0; i + 8 <= len; i += 8) {
		m = le64(p + i);
		v[3] ^= m; sipround(v); sipround(v); v[0] ^= m;
	}
	left = len - i;
	memset(last, 0, sizeof(last));
	memcpy(last, p + i, left);
	last[7] = (u8)len;
	m = le64(last);
	v[3] ^= m; sipround(v); sipround(v); v[0] ^= m;
	v[2] ^= 0xff;
	sipround(v); sipround(v); sipround(v); sipround(v);
	return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/* ---- small helpers ---- */

static u32 secs_after(u32 lifetime)
{
	u32 t;

	if (lifetime == RT_IN6_INFINITE)
		return 0;
	t = rt_ip6_seconds() + lifetime;
	return t ? t : 1;
}

static u32 secs_left(u32 until)
{
	u32 now = rt_ip6_seconds();

	if (!until)
		return RT_IN6_INFINITE;
	return (s32)(until - now) > 0 ? until - now : 0;
}

u32 rt_ip6_link_mtu(const struct rt_netif *nif)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	u32 mtu = nif->mtu;

	if (ifp && ifp->mtu && ifp->mtu < mtu)
		mtu = ifp->mtu;
	return mtu < RT_IP6_MIN_MTU ? RT_IP6_MIN_MTU : mtu;
}

u8 ip6_default_hlim(const struct rt_netif *nif)
{
	const struct ip6_if *ifp = ip6_ifp(nif);

	if (ifp && ifp->cur_hlim)
		return ifp->cur_hlim;
	return (u8)rt_ip6_hlim;
}

u32 ip6_reachable_ms(struct ip6_if *ifp)
{
	u32 now = rt_now_ms();

	/* RFC 4861 6.3.4: re-randomise at least every few hours */
	if (!ifp->reachable_ms || (u32)(now - ifp->reachable_set_ms) > 3600000u) {
		ifp->reachable_ms = ifp->base_reachable_ms / 2 + rt_random32() % (ifp->base_reachable_ms + 1);
		ifp->reachable_set_ms = now;
	}
	return ifp->reachable_ms;
}

int rt_ip6_ifenabled(const struct rt_netif *nif)
{
	return nif && nif->ip6 != NULL;
}

/* ---- timers ---- */

static void ifp_timer_fn(void *arg);

static void ifp_rearm(struct ip6_if *ifp)
{
	u32 now = rt_now_ms(), next = 0;
	int have = 0;

#define CONSIDER(t) do { if (!have || (s32)((t) - next) < 0) { next = (t); have = 1; } } while (0)
	if (ifp->rs_left)
		CONSIDER(ifp->rs_next_ms);
	if (ifp->mld_pending)
		CONSIDER(ifp->mld_next_ms);
	if (ifp->mld_resp_ms)
		CONSIDER(ifp->mld_resp_ms);
#undef CONSIDER
	if (!have) {
		rt_timer_del(&ifp->timer);
		return;
	}
	if ((s32)(next - now) < 0)
		next = now;
	rt_timer_mod(&ifp->timer, next);
}

/* ---- multicast and MLD ---- */

static struct in6_multi *mc_find(const struct ip6_if *ifp, const rt_in6_addr *g)
{
	struct in6_multi *m;

	for (m = ifp->mcast; m; m = m->next)
		if (rt_in6_eq(&m->grp, g))
			return m;
	return NULL;
}

/* MLD speaks about groups of link scope and wider, never about ff02::1 */
static int mc_reportable(const rt_in6_addr *g)
{
	static const u8 allnodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	return rt_in6_mc_scope(g) >= RT_IN6_SCOPE_LINK && memcmp(g->b, allnodes, 16) != 0;
}

/* Is another joined group on the same 33:33 address? */
static int mc_mac_shared(const struct ip6_if *ifp, const struct in6_multi *self)
{
	const struct in6_multi *m;

	for (m = ifp->mcast; m; m = m->next)
		if (m != self && m->refcnt && !memcmp(m->grp.b + 12, self->grp.b + 12, 4))
			return 1;
	return 0;
}

int rt_ip6_mc_joined(const struct rt_netif *nif, const rt_in6_addr *g)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	const struct in6_multi *m;

	if (!ifp)
		return 0;
	m = mc_find(ifp, g);
	return m && m->refcnt;
}

int rt_ip6_mc_list(const struct rt_netif *nif, rt_in6_addr *grp, u32 *refcnt, int max)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	const struct in6_multi *m;
	int n = 0;

	if (!ifp)
		return 0;
	for (m = ifp->mcast; m; m = m->next) {
		if (!m->refcnt)
			continue;
		if (n < max) {
			grp[n] = m->grp;
			refcnt[n] = m->refcnt;
		}
		n++;
	}
	return n;
}

static void mld_schedule_change(struct ip6_if *ifp)
{
	if (!ifp->nd || !(ifp->nif->flags & RT_IFF_UP))
		return;
	ifp->mld_pending = MLD_ROBUSTNESS;
	ifp->mld_next_ms = rt_now_ms() + rt_random32() % 200u;
	ifp_rearm(ifp);
}

int rt_ip6_mc_join(struct rt_netif *nif, const rt_in6_addr *g)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_multi *m;
	u8 mac[6];

	if (!ifp)
		return -RT_ENETDOWN;
	if (!rt_in6_is_mcast(g))
		return -RT_EINVAL;
	m = mc_find(ifp, g);
	if (m && m->refcnt) {
		m->refcnt++;
		return 0;
	}
	if (!m) {
		m = rt_zalloc(sizeof(*m));
		if (!m)
			return -RT_ENOMEM;
		m->grp = *g;
		m->next = ifp->mcast;
		ifp->mcast = m;
	}
	m->refcnt = 1;
	if (nif->mcast && !mc_mac_shared(ifp, m)) {
		rt_in6_mc_mac(mac, g);
		nif->mcast(nif, mac, 1);
	}
	if (mc_reportable(g)) {
		m->unsolicited = MLD_ROBUSTNESS;
		mld_schedule_change(ifp);
	}
	return 0;
}

static void mc_unlink(struct ip6_if *ifp, struct in6_multi *m)
{
	struct in6_multi **pp;

	for (pp = &ifp->mcast; *pp; pp = &(*pp)->next) {
		if (*pp == m) {
			*pp = m->next;
			break;
		}
	}
	rt_free(m);
}

int rt_ip6_mc_leave(struct rt_netif *nif, const rt_in6_addr *g)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_multi *m;
	u8 mac[6];

	if (!ifp)
		return -RT_ENETDOWN;
	m = mc_find(ifp, g);
	if (!m || !m->refcnt)
		return -RT_EADDRNOTAVAIL;
	if (--m->refcnt)
		return 0;
	if (nif->mcast && !mc_mac_shared(ifp, m)) {
		rt_in6_mc_mac(mac, g);
		nif->mcast(nif, mac, 0);
	}
	if (mc_reportable(g) && ifp->nd && (nif->flags & RT_IFF_UP)) {
		m->unsolicited = MLD_ROBUSTNESS;   /* report the leave, then free */
		mld_schedule_change(ifp);
	} else {
		mc_unlink(ifp, m);
	}
	return 0;
}

/* Build an MLDv2 report: changes = 1: state change records of the groups
 * with unsolicited reports pending; 0: current state of every group. */
static void mld_send_v2(struct ip6_if *ifp, int changes)
{
	static const u8 mld_routers[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x16 };
	struct in6_multi *m;
	struct in6_ifaddr *ll = in6_ifa_linklocal(ifp->nif, 1);
	struct rt_buf *b;
	rt_in6_addr dst, src;
	u32 n = 0, len;
	u8 *p;
	u16 ck;

	for (m = ifp->mcast; m; m = m->next)
		if (mc_reportable(&m->grp) && (changes ? m->unsolicited > 0 : m->refcnt > 0))
			n++;
	if (!n)
		return;
	if (n > MLD_MAX_RECORDS)
		n = MLD_MAX_RECORDS;
	len = 8 + n * 20;
	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b)
		return;
	p = rt_buf_put(b, len);
	memset(p, 0, len);
	p[0] = RT_ICMP6_MLDV2_REPORT;
	rt_put_be16(p + 6, (u16)n);
	n = 0;
	for (m = ifp->mcast; m && n < MLD_MAX_RECORDS; m = m->next) {
		u8 *r;
		if (!mc_reportable(&m->grp) || !(changes ? m->unsolicited > 0 : m->refcnt > 0))
			continue;
		r = p + 8 + n * 20;
		/* RFC 3810 5.2.12: 2 MODE_IS_EXCLUDE, 3 CHANGE_TO_INCLUDE, 4 CHANGE_TO_EXCLUDE */
		r[0] = !changes ? 2 : m->refcnt ? 4 : 3;
		memcpy(r + 4, m->grp.b, 16);
		n++;
	}
	memcpy(dst.b, mld_routers, 16);
	if (ll)
		src = ll->addr;
	else
		memset(&src, 0, sizeof(src));      /* RFC 3810 5.2.13: during DAD */
	ck = rt_csum_fold(rt_csum_partial(p, len, rt_csum_pseudo6(&src, &dst, RT_IPPROTO_ICMPV6, len)));
	memcpy(p + 2, &ck, 2);
	rt_icmp6stat.OutMsgs++;
	rt_icmp6stat.OutMLDv2Reports++;
	ip6_output_direct(b, ifp->nif, &src, &dst, RT_IPPROTO_ICMPV6, 1, 1);
}

/* MLDv1 (RFC 2710) report or done for one group */
static void mld_send_v1(struct ip6_if *ifp, const rt_in6_addr *g, int done)
{
	static const u8 all_routers[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
	struct in6_ifaddr *ll = in6_ifa_linklocal(ifp->nif, 1);
	struct rt_buf *b;
	rt_in6_addr dst, src;
	u8 *p;
	u16 ck;

	if (!ll)
		return;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 24);
	if (!b)
		return;
	p = rt_buf_put(b, 24);
	memset(p, 0, 24);
	p[0] = done ? RT_ICMP6_MLD_DONE : RT_ICMP6_MLD_REPORT;
	memcpy(p + 8, g->b, 16);
	if (done)
		memcpy(dst.b, all_routers, 16);
	else
		dst = *g;
	src = ll->addr;
	ck = rt_csum_fold(rt_csum_partial(p, 24, rt_csum_pseudo6(&src, &dst, RT_IPPROTO_ICMPV6, 24)));
	memcpy(p + 2, &ck, 2);
	rt_icmp6stat.OutMsgs++;
	ip6_output_direct(b, ifp->nif, &src, &dst, RT_IPPROTO_ICMPV6, 1, 1);
}

static int mld_v1_mode(const struct ip6_if *ifp)
{
	return ifp->mldv1_until_s && (s32)(ifp->mldv1_until_s - rt_ip6_seconds()) > 0;
}

static void mld_report(struct ip6_if *ifp, int changes)
{
	struct in6_multi *m;

	if (!mld_v1_mode(ifp)) {
		mld_send_v2(ifp, changes);
		return;
	}
	for (m = ifp->mcast; m; m = m->next)
		if (mc_reportable(&m->grp) && (changes ? m->unsolicited > 0 : m->refcnt > 0))
			mld_send_v1(ifp, &m->grp, m->refcnt == 0);
}

void mld6_input(struct rt_netif *nif, const struct rt_ip6hdr *ip6, const u8 *m, u32 len)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	u32 maxresp, delay;
	rt_in6_addr g;

	if (!ifp || !ifp->nd || len < 24)
		return;
	/* RFC 3810 5.1.14 / 6.2: link-local source, hop limit 1 */
	if (!rt_in6_is_linklocal(&ip6->src) || ip6->hlim != 1)
		return;
	if (m[0] != RT_ICMP6_MLD_QUERY)
		return;                  /* other hosts' reports: no suppression in MLDv2 */
	rt_icmp6stat.InMLDQueries++;
	memcpy(g.b, m + 8, 16);
	if (len == 24) {
		/* MLDv1 querier */
		maxresp = rt_get_be16(m + 4);
		ifp->mldv1_until_s = rt_ip6_seconds() + MLD_V1_PRESENT_S;
	} else if (len >= 28) {
		u16 code = rt_get_be16(m + 4);
		/* RFC 3810 5.1.3 Maximum Response Code */
		maxresp = code < 32768 ? code : (u32)((code & 0x0fff) | 0x1000) << (((code >> 12) & 7) + 3);
	} else {
		return;
	}
	if (!rt_in6_is_unspec(&g) && !rt_ip6_mc_joined(nif, &g))
		return;
	delay = maxresp ? rt_random32() % (maxresp + 1) : 0;
	if (!ifp->mld_resp_ms || (s32)(rt_now_ms() + delay - ifp->mld_resp_ms) < 0)
		ifp->mld_resp_ms = (rt_now_ms() + delay) | 1;
	ifp_rearm(ifp);
}

static void ifp_timer_fn(void *arg)
{
	struct ip6_if *ifp = arg;
	u32 now = rt_now_ms();

	if (ifp->rs_left && (s32)(now - ifp->rs_next_ms) >= 0) {
		if (in6_ifa_linklocal(ifp->nif, 1) && !rt_ip6_forwarding) {
			nd6_send_rs(ifp->nif);
			ifp->rs_left--;
		}
		ifp->rs_next_ms = now + RTR_SOLICITATION_INTERVAL;
	}
	if (ifp->mld_pending && (s32)(now - ifp->mld_next_ms) >= 0) {
		struct in6_multi *m, *n;
		mld_report(ifp, 1);
		ifp->mld_pending--;
		for (m = ifp->mcast; m; m = n) {
			n = m->next;
			if (m->unsolicited)
				m->unsolicited--;
			if (!m->refcnt && !m->unsolicited)
				mc_unlink(ifp, m);
		}
		ifp->mld_next_ms = now + MLD_UNSOL_INTERVAL;
	}
	if (ifp->mld_resp_ms && (s32)(now - ifp->mld_resp_ms) >= 0) {
		ifp->mld_resp_ms = 0;
		mld_report(ifp, 0);
	}
	ifp_rearm(ifp);
}

/* ---- addresses ---- */

struct in6_ifaddr *in6_ifa_find(const struct rt_netif *nif, const rt_in6_addr *a)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_ifaddr *ia;

	if (!ifp)
		return NULL;
	for (ia = ifp->addrs; ia; ia = ia->next)
		if (rt_in6_eq(&ia->addr, a))
			return ia;
	return NULL;
}

struct in6_ifaddr *in6_ifa_linklocal(const struct rt_netif *nif, int usable)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_ifaddr *ia;

	if (!ifp)
		return NULL;
	for (ia = ifp->addrs; ia; ia = ia->next) {
		if (!rt_in6_is_linklocal(&ia->addr))
			continue;
		if (usable && (ia->flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED)))
			continue;
		return ia;
	}
	return NULL;
}

struct rt_netif *rt_in6_addr_owner(const rt_in6_addr *a, int any)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next) {
		struct in6_ifaddr *ia = in6_ifa_find(n, a);
		if (!ia)
			continue;
		if (!any && (ia->flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED)))
			continue;
		return n;
	}
	return NULL;
}

int rt_ip6_is_local(const rt_in6_addr *a)
{
	struct rt_netif *n = rt_in6_addr_owner(a, 0);

	if (rt_in6_is_loopback(a))
		return rt_loopback_if && (rt_loopback_if->flags & RT_IFF_UP) && rt_loopback_if->ip6;
	return n && (n->flags & RT_IFF_UP);
}

static void ifa_ready(struct in6_ifaddr *ia)
{
	struct ip6_if *ifp = ia->ifp;

	ia->flags &= (u16)~RT_IN6_IFF_TENTATIVE;
	rt_timer_del(&ia->dad_timer);
	if (rt_in6_is_linklocal(&ia->addr) && ifp->nd && rt_ip6_accept_rtadv && !rt_ip6_forwarding &&
	    ifp->rs_left == 0 && ifp->rs_next_ms == 0) {
		/* RFC 4861 6.3.7: solicit routers after a random delay */
		ifp->rs_left = MAX_RTR_SOLICITATIONS;
		ifp->rs_next_ms = rt_now_ms() + rt_random32() % MAX_RTR_SOLICIT_DELAY;
		if (!ifp->rs_next_ms)
			ifp->rs_next_ms = 1;
		ifp_rearm(ifp);
	}
	if (rt_in6_is_linklocal(&ia->addr) && ifp->mld_pending)
		ifp_rearm(ifp);
}

static void dad_timer_fn(void *arg)
{
	struct in6_ifaddr *ia = arg;
	struct ip6_if *ifp = ia->ifp;

	if (!(ia->flags & RT_IN6_IFF_TENTATIVE))
		return;
	if (ia->dad_left == 0) {
		ifa_ready(ia);
		return;
	}
	ia->dad_left--;
	rt_random_bytes(ia->nonce, sizeof(ia->nonce));
	nd6_send_ns(ifp->nif, &ia->addr, NULL, NULL, ia->nonce);
	rt_timer_after(&ia->dad_timer, ifp->retrans_ms ? ifp->retrans_ms : RETRANS_TIMER);
}

static void dad_start(struct in6_ifaddr *ia)
{
	struct ip6_if *ifp = ia->ifp;

	if (!(ia->flags & RT_IN6_IFF_TENTATIVE))
		return;
	if (!(ifp->nif->flags & RT_IFF_UP))
		return;                  /* restarted by rt_ip6_if_up() */
	if (!ifp->nd || (ia->flags & RT_IN6_IFF_NODAD) || rt_ip6_dad_count <= 0) {
		ifa_ready(ia);
		return;
	}
	ia->dad_left = (u8)rt_ip6_dad_count;
	/* RFC 4862 5.4.2: random delay before the first probe */
	rt_timer_after(&ia->dad_timer, rt_random32() % MAX_RTR_SOLICIT_DELAY);
}

int in6_dad_own_nonce(const struct rt_netif *nif, const u8 *nonce)
{
	const struct ip6_if *ifp = ip6_ifp(nif);
	const struct in6_ifaddr *ia;

	if (!ifp)
		return 0;
	for (ia = ifp->addrs; ia; ia = ia->next)
		if ((ia->flags & RT_IN6_IFF_TENTATIVE) && !memcmp(ia->nonce, nonce, 6))
			return 1;
	return 0;
}

static void prefix_route_put(struct in6_ifaddr *ia);
static int stable_iid(const struct ip6_if *ifp, const rt_in6_addr *prefix, u8 counter, u8 *iid);

void in6_dad_duplicate(struct in6_ifaddr *ia)
{
	char buf[RT_INET6_ADDRSTRLEN];
	struct ip6_if *ifp = ia->ifp;

	rt_icmp6stat.DadDuplicates++;
	rt_log(RT_LOG_WARN, "ip6: duplicate address %s on %s", rt_inet6_ntop(&ia->addr, buf, sizeof(buf)),
	       ifp->nif->name);
	rt_timer_del(&ia->dad_timer);
	/* RFC 7217 6: try the next DAD_Counter for generated addresses */
	if ((ia->flags & RT_IN6_IFF_AUTOCONF) && rt_ip6_addr_gen_mode == 1 &&
	    ia->dad_counter < IDGEN_RETRIES && !(ia->flags & RT_IN6_IFF_TEMPORARY)) {
		rt_in6_addr na, sol;
		u8 iid[8];
		ia->dad_counter++;
		if (stable_iid(ifp, &ia->addr, ia->dad_counter, iid) == 0) {
			na = ia->addr;
			memcpy(na.b + 8, iid, 8);
			if (!in6_ifa_find(ifp->nif, &na)) {
				rt_in6_solnode(&sol, &ia->addr);
				rt_ip6_mc_leave(ifp->nif, &sol);
				ia->addr = na;
				rt_in6_solnode(&sol, &na);
				rt_ip6_mc_join(ifp->nif, &sol);
				dad_start(ia);
				return;
			}
		}
	}
	ia->flags |= RT_IN6_IFF_DUPLICATED;
	ia->flags &= (u16)~RT_IN6_IFF_TENTATIVE;
}

/* Modified EUI-64 from a 48 bit MAC (RFC 4291 appendix A). */
static void eui64_from_mac(const u8 *mac, u8 *iid)
{
	iid[0] = mac[0] ^ 0x02;
	iid[1] = mac[1];
	iid[2] = mac[2];
	iid[3] = 0xff;
	iid[4] = 0xfe;
	iid[5] = mac[3];
	iid[6] = mac[4];
	iid[7] = mac[5];
}

static int iid_reserved(const u8 *iid)
{
	static const u8 zero[8];

	if (!memcmp(iid, zero, 8))
		return 1;                         /* subnet-router anycast */
	/* RFC 5453: reserved subnet anycast fdff:ffff:ffff:ff80..ffff */
	if (iid[0] == 0xfd && iid[1] == 0xff && iid[2] == 0xff && iid[3] == 0xff &&
	    iid[4] == 0xff && iid[5] == 0xff && iid[6] == 0xff && iid[7] >= 0x80)
		return 1;
	/* proxy mobile IPv6 0200:5efe:... and 0000:5efe (ISATAP) look-alikes */
	if ((iid[0] == 0x02 || iid[0] == 0x00) && iid[1] == 0x00 && iid[2] == 0x5e && iid[3] == 0xfe)
		return 1;
	return 0;
}

static void stable_key(const struct ip6_if *ifp, u8 *key)
{
	static const u8 zero[16];
	u64 a, b;
	u8 buf[48];
	u32 n;

	if (rt_ip6_stable_secret[0]) {
		n = (u32)strlen(rt_ip6_stable_secret);
		a = rt_siphash(rt_ip6_stable_secret, n, zero);
		memcpy(buf, rt_ip6_stable_secret, n < 40 ? n : 40);
		buf[n < 40 ? n : 40] = 0x5a;
		b = rt_siphash(buf, (n < 40 ? n : 40) + 1, zero);
	} else {
		memset(buf, 0, sizeof(buf));
		memcpy(buf, ifp->nif->hwaddr, ifp->nif->hwlen <= 16 ? ifp->nif->hwlen : 16);
		memcpy(buf + 16, "riptide-rfc7217-", 16);
		a = rt_siphash(buf, 32, zero);
		buf[31] ^= 1;
		b = rt_siphash(buf, 32, zero);
	}
	memcpy(key, &a, 8);
	memcpy(key + 8, &b, 8);
}

/* RFC 7217 5: RID = F(Prefix, Net_Iface, Network_ID, DAD_Counter, secret) */
static int stable_iid(const struct ip6_if *ifp, const rt_in6_addr *prefix, u8 counter, u8 *iid)
{
	u8 key[16], msg[8 + RT_IFNAMSIZ + 1];
	u64 h;
	int tries;

	stable_key(ifp, key);
	memcpy(msg, prefix->b, 8);
	memcpy(msg + 8, ifp->nif->name, RT_IFNAMSIZ);
	for (tries = 0; tries < 4; tries++) {
		msg[8 + RT_IFNAMSIZ] = (u8)(counter + tries * 16);
		h = rt_siphash(msg, sizeof(msg), key);
		rt_put_be32(iid, (u32)(h >> 32));
		rt_put_be32(iid + 4, (u32)h);
		if (!iid_reserved(iid))
			return 0;
	}
	return -1;
}

/* Interface identifier for prefix. Links without a MAC always use the
 * stable form (it is keyed on the interface name). */
static int make_iid(const struct ip6_if *ifp, const rt_in6_addr *prefix, u8 *iid)
{
	if (rt_ip6_addr_gen_mode == 0 && ifp->nif->hwlen == 6) {
		memcpy(iid, ifp->iid_eui64, 8);
		return 0;
	}
	return stable_iid(ifp, prefix, 0, iid);
}

static void prefix_route_put(struct in6_ifaddr *ia)
{
	struct ip6_if *ifp = ia->ifp;
	struct in6_ifaddr *o;

	if (!ia->prefix_route)
		return;
	ia->prefix_route = 0;
	for (o = ifp->addrs; o; o = o->next)
		if (o != ia && o->prefix_route && o->plen == ia->plen &&
		    rt_in6_prefix_eq(&o->addr, &ia->addr, ia->plen))
			return;                 /* still needed */
	rt_route6_delete(&ia->addr, ia->plen, NULL, ifp->nif, RT_RTF_CONNECTED);
}

static struct in6_ifaddr *ifa_create(struct ip6_if *ifp, const rt_in6_addr *a, int plen,
                                     u32 flags, u32 valid_s, u32 pref_s)
{
	struct in6_ifaddr *ia, **pp;
	rt_in6_addr sol;

	ia = rt_zalloc(sizeof(*ia));
	if (!ia)
		return NULL;
	ia->ifp = ifp;
	ia->addr = *a;
	ia->plen = (u8)plen;
	ia->flags = (u16)(flags & ~(u32)(RT_IN6_IFF_DEPRECATED | RT_IN6_IFF_DUPLICATED));
	ia->flags |= RT_IN6_IFF_TENTATIVE;
	ia->valid_until = secs_after(valid_s);
	ia->pref_until = secs_after(pref_s);
	if (pref_s == 0)
		ia->flags |= RT_IN6_IFF_DEPRECATED;
	ia->created_s = rt_ip6_seconds();
	rt_timer_init(&ia->dad_timer, dad_timer_fn, ia);
	/* append: the order is the order of the configuration */
	for (pp = &ifp->addrs; *pp; pp = &(*pp)->next)
		;
	*pp = ia;
	rt_in6_solnode(&sol, a);
	rt_ip6_mc_join(ifp->nif, &sol);
	if (plen < 128 && !(rt_in6_is_linklocal(a) && plen == 64)) {
		if (rt_route6_add(a, plen, NULL, ifp->nif, RT_RTF_CONNECTED, 256) == 0 ||
		    route6_find(a, plen, NULL, ifp->nif))
			ia->prefix_route = 1;
	}
	dad_start(ia);
	return ia;
}

static void ifa_destroy(struct in6_ifaddr *ia)
{
	struct ip6_if *ifp = ia->ifp;
	struct in6_ifaddr **pp;
	rt_in6_addr sol;

	for (pp = &ifp->addrs; *pp; pp = &(*pp)->next) {
		if (*pp == ia) {
			*pp = ia->next;
			break;
		}
	}
	rt_timer_del(&ia->dad_timer);
	rt_in6_solnode(&sol, &ia->addr);
	rt_ip6_mc_leave(ifp->nif, &sol);
	prefix_route_put(ia);
	rt_free(ia);
}

int rt_in6_addr_add(struct rt_netif *nif, const rt_in6_addr *a, int plen, u32 flags,
                    u32 valid_s, u32 pref_s)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_ifaddr *ia;

	if (!ifp)
		return -RT_ENETDOWN;
	if (plen < 1 || plen > 128 || rt_in6_is_unspec(a) || rt_in6_is_mcast(a) ||
	    rt_in6_is_v4mapped(a) || (rt_in6_is_loopback(a) && nif->type != RT_IFT_LOOP))
		return -RT_EINVAL;
	if (pref_s != RT_IN6_INFINITE && valid_s != RT_IN6_INFINITE && pref_s > valid_s)
		return -RT_EINVAL;
	if (valid_s == RT_IN6_INFINITE && pref_s == RT_IN6_INFINITE)
		flags |= RT_IN6_IFF_PERMANENT;
	ia = in6_ifa_find(nif, a);
	if (ia) {
		ia->valid_until = secs_after(valid_s);
		ia->pref_until = secs_after(pref_s);
		if (pref_s)
			ia->flags &= (u16)~RT_IN6_IFF_DEPRECATED;
		return -RT_EEXIST;
	}
	if (nif->type == RT_IFT_LOOP || nif->type == RT_IFT_PTP)
		flags |= RT_IN6_IFF_NODAD;
	return ifa_create(ifp, a, plen, flags, valid_s, pref_s) ? 0 : -RT_ENOMEM;
}

int rt_in6_addr_del(struct rt_netif *nif, const rt_in6_addr *a)
{
	struct in6_ifaddr *ia = in6_ifa_find(nif, a);

	if (!ia)
		return -RT_EADDRNOTAVAIL;
	ifa_destroy(ia);
	return 0;
}

int rt_in6_addr_list(const struct rt_netif *nif, struct rt_in6_ifaddr_info *out, int max)
{
	const struct rt_netif *n;
	int cnt = 0;

	for (n = nif ? nif : rt_netif_list(); n; n = nif ? NULL : n->next) {
		const struct ip6_if *ifp = ip6_ifp(n);
		const struct in6_ifaddr *ia;
		if (!ifp)
			continue;
		for (ia = ifp->addrs; ia; ia = ia->next, cnt++) {
			struct rt_in6_ifaddr_info *o;
			if (!out || cnt >= max)
				continue;
			o = &out[cnt];
			memset(o, 0, sizeof(*o));
			o->addr = ia->addr;
			o->ifindex = n->index;
			o->valid_s = secs_left(ia->valid_until);
			o->pref_s = (ia->flags & RT_IN6_IFF_DEPRECATED) ? 0 : secs_left(ia->pref_until);
			o->flags = ia->flags;
			o->plen = ia->plen;
			o->scope = (u8)rt_in6_scope(&ia->addr);
		}
	}
	return cnt;
}

/* ---- SLAAC (RFC 4862 5.5.3) and temporary addresses (RFC 8981) ---- */

static void temp_create(struct ip6_if *ifp, const struct in6_ifaddr *pub)
{
	rt_in6_addr a;
	u32 valid = TEMP_VALID_S, pref = TEMP_PREFERRED_S - rt_random32() % 600u;
	u32 pv = secs_left(pub->valid_until), pp = secs_left(pub->pref_until);
	int i;

	if (pub->flags & RT_IN6_IFF_DEPRECATED)
		return;
	if (pv < valid)
		valid = pv;
	if (pp < pref)
		pref = pp;
	if (pref <= TEMP_REGEN_ADVANCE_S)
		return;
	a = pub->addr;
	for (i = 0; i < 4; i++) {
		rt_random_bytes(a.b + 8, 8);
		a.b[8] &= (u8)~0x02;               /* RFC 8981 3.3.1: not universal */
		if (!iid_reserved(a.b + 8) && !in6_ifa_find(ifp->nif, &a))
			break;
	}
	if (i == 4)
		return;
	ifa_create(ifp, &a, pub->plen, RT_IN6_IFF_AUTOCONF | RT_IN6_IFF_TEMPORARY, valid, pref);
}

static int temp_exists(const struct ip6_if *ifp, const struct in6_ifaddr *pub)
{
	const struct in6_ifaddr *ia;

	for (ia = ifp->addrs; ia; ia = ia->next)
		if ((ia->flags & RT_IN6_IFF_TEMPORARY) && !(ia->flags & RT_IN6_IFF_DEPRECATED) &&
		    ia->plen == pub->plen && rt_in6_prefix_eq(&ia->addr, &pub->addr, pub->plen))
			return 1;
	return 0;
}

void in6_slaac_prefix(struct rt_netif *nif, const rt_in6_addr *prefix, int plen,
                      u32 valid_s, u32 pref_s)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_ifaddr *ia, *pub = NULL;
	int found = 0;

	if (!ifp || rt_in6_is_linklocal(prefix) || pref_s > valid_s)
		return;
	for (ia = ifp->addrs; ia; ia = ia->next) {
		u32 left;
		if (!(ia->flags & RT_IN6_IFF_AUTOCONF) || ia->plen != plen ||
		    !rt_in6_prefix_eq(&ia->addr, prefix, plen))
			continue;
		found = 1;
		if (ia->flags & RT_IN6_IFF_TEMPORARY) {
			/* RFC 8981 3.4: never extend beyond the public lifetimes */
			u32 tl = secs_left(ia->valid_until);
			if (valid_s < tl)
				ia->valid_until = secs_after(valid_s);
			if (pref_s < secs_left(ia->pref_until))
				ia->pref_until = secs_after(pref_s);
			continue;
		}
		pub = ia;
		ia->pref_until = secs_after(pref_s);
		if (pref_s)
			ia->flags &= (u16)~RT_IN6_IFF_DEPRECATED;
		else
			ia->flags |= RT_IN6_IFF_DEPRECATED;
		/* the "two hour rule" (RFC 4862 5.5.3 e) */
		left = secs_left(ia->valid_until);
		if (valid_s > 7200u || valid_s > left)
			ia->valid_until = secs_after(valid_s);
		else if (left > 7200u)
			ia->valid_until = secs_after(7200u);
	}
	if (!found && valid_s) {
		rt_in6_addr a;
		u8 iid[8];
		if (plen != 64) {
			char buf[RT_INET6_ADDRSTRLEN];
			rt_log(RT_LOG_INFO, "ip6: %s: ignoring /%d prefix %s for autoconfiguration",
			       nif->name, plen, rt_inet6_ntop(prefix, buf, sizeof(buf)));
			return;
		}
		if (make_iid(ifp, prefix, iid) < 0)
			return;
		rt_in6_mask(&a, prefix, 64);
		memcpy(a.b + 8, iid, 8);
		pub = ifa_create(ifp, &a, plen, RT_IN6_IFF_AUTOCONF, valid_s, pref_s);
	}
	if (pub && rt_ip6_use_tempaddr > 0 && !temp_exists(ifp, pub))
		temp_create(ifp, pub);
}

/* ---- lifetimes ---- */

void ip6_if_slowtimo(u32 now_s)
{
	struct rt_netif *n;

	for (n = rt_netif_list(); n; n = n->next) {
		struct ip6_if *ifp = ip6_ifp(n);
		struct in6_ifaddr *ia, *nx;
		if (!ifp)
			continue;
		for (ia = ifp->addrs; ia; ia = nx) {
			nx = ia->next;
			if (ia->valid_until && (s32)(now_s - ia->valid_until) >= 0) {
				ifa_destroy(ia);
				continue;
			}
			if (ia->pref_until && !(ia->flags & RT_IN6_IFF_DEPRECATED) &&
			    (s32)(now_s - ia->pref_until) >= 0)
				ia->flags |= RT_IN6_IFF_DEPRECATED;
			/* RFC 8981 3.6: a new temporary address before the old one
			 * is deprecated */
			if ((ia->flags & RT_IN6_IFF_TEMPORARY) && rt_ip6_use_tempaddr > 0 && ia->pref_until &&
			    !(ia->flags & RT_IN6_IFF_DEPRECATED) &&
			    (s32)(ia->pref_until - TEMP_REGEN_ADVANCE_S - now_s) <= 0) {
				struct in6_ifaddr *p;
				for (p = ifp->addrs; p; p = p->next)
					if ((p->flags & (RT_IN6_IFF_AUTOCONF | RT_IN6_IFF_TEMPORARY)) == RT_IN6_IFF_AUTOCONF &&
					    p->plen == ia->plen && rt_in6_prefix_eq(&p->addr, &ia->addr, ia->plen))
						break;
				ia->flags |= RT_IN6_IFF_DEPRECATED;
				if (p && !temp_exists(ifp, p))
					temp_create(ifp, p);
			}
		}
	}
}

/* ---- enable / disable, link up / down ---- */

void rt_route6_flush_learned(struct rt_netif *nif);

static void if_start(struct ip6_if *ifp)
{
	struct in6_ifaddr *ia;

	ifp->rs_left = 0;
	ifp->rs_next_ms = 0;
	for (ia = ifp->addrs; ia; ia = ia->next)
		if (ia->flags & RT_IN6_IFF_TENTATIVE)
			dad_start(ia);
		else if (rt_in6_is_linklocal(&ia->addr))
			ifa_ready(ia);
	if (ifp->nd) {
		struct in6_multi *m;
		for (m = ifp->mcast; m; m = m->next)
			if (m->refcnt && mc_reportable(&m->grp))
				m->unsolicited = MLD_ROBUSTNESS;
		mld_schedule_change(ifp);
	}
}

void rt_ip6_if_up(struct rt_netif *nif);
void rt_ip6_if_up(struct rt_netif *nif)
{
	struct ip6_if *ifp = ip6_ifp(nif);

	if (!ifp) {
		/* ip6_disabled: 0 the default (net.inet6.ip6.enable), 1 off,
		 * 2 on for this interface whatever the default */
		if ((rt_ip6_auto_enable && nif->ip6_disabled != 1) || nif->ip6_disabled == 2)
			rt_ip6_ifenable(nif, 1);
		return;
	}
	if_start(ifp);
}

void rt_ip6_if_down(struct rt_netif *nif);
void rt_ip6_if_down(struct rt_netif *nif)
{
	struct ip6_if *ifp = ip6_ifp(nif);
	struct in6_ifaddr *ia, *nx;

	if (!ifp)
		return;
	rt_timer_del(&ifp->timer);
	ifp->rs_left = 0;
	ifp->rs_next_ms = 0;
	ifp->mld_pending = 0;
	ifp->mld_resp_ms = 0;
	ifp->mtu = 0;
	ifp->cur_hlim = 0;
	/* learned state is stale when the link comes back (other network?) */
	for (ia = ifp->addrs; ia; ia = nx) {
		nx = ia->next;
		if (ia->flags & RT_IN6_IFF_AUTOCONF) {
			ifa_destroy(ia);
			continue;
		}
		rt_timer_del(&ia->dad_timer);
		if (ifp->nd && !(ia->flags & RT_IN6_IFF_NODAD))
			ia->flags |= RT_IN6_IFF_TENTATIVE;
		ia->flags &= (u16)~RT_IN6_IFF_DUPLICATED;
	}
	rt_nd6_flush(nif, 1);
	rt_route6_flush_learned(nif);
	dhcp6_stop(nif);
}

int rt_ip6_ifenable(struct rt_netif *nif, int on)
{
	static const rt_in6_addr allnodes = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
	struct ip6_if *ifp = ip6_ifp(nif);
	rt_in6_addr ll, llnet;
	u8 iid[8];

	if (!nif)
		return -RT_EINVAL;
	if (!on) {
		struct in6_multi *m;
		nif->ip6_disabled = 1;
		if (!ifp)
			return 0;
		rt_ip6_if_down(nif);
		while (ifp->addrs)
			ifa_destroy(ifp->addrs);
		while ((m = ifp->mcast) != NULL) {
			ifp->mcast = m->next;
			if (nif->mcast && m->refcnt) {
				u8 mac[6];
				rt_in6_mc_mac(mac, &m->grp);
				nif->mcast(nif, mac, 0);
			}
			rt_free(m);
		}
		rt_timer_del(&ifp->timer);
		rt_nd6_flush(nif, 0);
		rt_route6_flush_if(nif);
		nif->ip6 = NULL;
		rt_free(ifp);
		return 0;
	}
	nif->ip6_disabled = 0;
	if (ifp)
		return 0;
	if (nif->mtu < RT_IP6_MIN_MTU && nif->type != RT_IFT_LOOP) {
		rt_log(RT_LOG_INFO, "ip6: %s: MTU %u is below 1280, no IPv6", nif->name, (unsigned)nif->mtu);
		return -RT_EINVAL;
	}
	ifp = rt_zalloc(sizeof(*ifp));
	if (!ifp)
		return -RT_ENOMEM;
	ifp->nif = nif;
	ifp->nd = nif->type == RT_IFT_ETHER;
	ifp->base_reachable_ms = REACHABLE_TIME;
	ifp->retrans_ms = RETRANS_TIMER;
	rt_timer_init(&ifp->timer, ifp_timer_fn, ifp);
	if (nif->hwlen == 6)
		eui64_from_mac(nif->hwaddr, ifp->iid_eui64);
	nif->ip6 = ifp;
	rt_ip6_mc_join(nif, &allnodes);
	if (nif->type == RT_IFT_LOOP) {
		rt_in6_addr_add(nif, &rt_in6addr_loopback, 128, RT_IN6_IFF_NODAD, RT_IN6_INFINITE,
		                RT_IN6_INFINITE);
		return 0;
	}
	/* link-local address and the fe80::/64 route */
	memset(&llnet, 0, sizeof(llnet));
	llnet.b[0] = 0xfe;
	llnet.b[1] = 0x80;
	rt_route6_add(&llnet, 64, NULL, nif, RT_RTF_CONNECTED, 256);
	if (make_iid(ifp, &llnet, iid) == 0) {
		ll = llnet;
		memcpy(ll.b + 8, iid, 8);
		rt_in6_addr_add(nif, &ll, 64, 0, RT_IN6_INFINITE, RT_IN6_INFINITE);
	}
	if (nif->flags & RT_IFF_UP)
		if_start(ifp);
	return 0;
}

/* ---- source address selection (RFC 6724 5) ---- */

struct policy { rt_in6_addr prefix; u8 plen, prec, label; };

static const struct policy policy_table[] = {
	{ { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }, 128, 50, 0 },
	{ { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0 } }, 96, 35, 4 },
	{ { { 0x20, 0x01, 0, 0 } }, 32, 5, 5 },
	{ { { 0x20, 0x02 } }, 16, 30, 2 },
	{ { { 0x3f, 0xfe } }, 16, 1, 12 },
	{ { { 0xfe, 0xc0 } }, 10, 1, 11 },
	{ { { 0xfc } }, 7, 3, 13 },
	{ { { 0 } }, 96, 1, 3 },
	{ { { 0 } }, 0, 40, 1 },
};

static const struct policy *policy_of(const rt_in6_addr *a)
{
	const struct policy *best = &policy_table[RT_ARRAY_SIZE(policy_table) - 1];
	unsigned i;

	for (i = 0; i < RT_ARRAY_SIZE(policy_table); i++) {
		const struct policy *p = &policy_table[i];
		if (p->plen > best->plen && rt_in6_prefix_eq(a, &p->prefix, p->plen))
			best = p;
	}
	return best;
}

int rt_in6_precedence(const rt_in6_addr *a)
{
	return policy_of(a)->prec;
}

int rt_in6_label(const rt_in6_addr *a)
{
	return policy_of(a)->label;
}

/* Is SA better than SB for D (RFC 6724 5, rules 1, 2, 3, 5, 6, 7, 8)? */
static int src_better(const struct in6_ifaddr *a, const struct in6_ifaddr *b,
                      const rt_in6_addr *d, const struct rt_netif *oif)
{
	int sa, sb, sd, la, lb, ld, ca, cb;

	if (rt_in6_eq(&a->addr, d) != rt_in6_eq(&b->addr, d))
		return rt_in6_eq(&a->addr, d);
	sa = rt_in6_scope(&a->addr);
	sb = rt_in6_scope(&b->addr);
	sd = rt_in6_scope(d);
	if (sa != sb) {
		if (sa < sb)
			return sa >= sd ? 1 : 0;
		return sb >= sd ? 0 : 1;
	}
	if ((a->flags & RT_IN6_IFF_DEPRECATED) != (b->flags & RT_IN6_IFF_DEPRECATED))
		return !(a->flags & RT_IN6_IFF_DEPRECATED);
	if (oif && (a->ifp->nif == oif) != (b->ifp->nif == oif))
		return a->ifp->nif == oif;
	ld = rt_in6_label(d);
	la = rt_in6_label(&a->addr);
	lb = rt_in6_label(&b->addr);
	if ((la == ld) != (lb == ld))
		return la == ld;
	if ((a->flags & RT_IN6_IFF_TEMPORARY) != (b->flags & RT_IN6_IFF_TEMPORARY)) {
		int want_temp = rt_ip6_use_tempaddr >= 2;
		return ((a->flags & RT_IN6_IFF_TEMPORARY) != 0) == want_temp;
	}
	ca = rt_in6_common_prefix(&a->addr, d);
	cb = rt_in6_common_prefix(&b->addr, d);
	if (ca > a->plen)
		ca = a->plen;
	if (cb > b->plen)
		cb = b->plen;
	return ca > cb;
}

int rt_ip6_select_source(const rt_in6_addr *dst, const struct rt_netif *oif, rt_in6_addr *src)
{
	const struct in6_ifaddr *best = NULL;
	struct rt_netif *n;
	int zone = rt_in6_is_linklocal(dst) || rt_in6_is_mc_linklocal(dst);

	if (rt_in6_is_loopback(dst) || (oif && oif->type == RT_IFT_LOOP && rt_in6_is_loopback(dst))) {
		*src = rt_in6addr_loopback;
		return 0;
	}
	for (n = rt_netif_list(); n; n = n->next) {
		const struct ip6_if *ifp = ip6_ifp(n);
		const struct in6_ifaddr *ia;
		if (!ifp || !(n->flags & RT_IFF_UP))
			continue;
		if (zone && oif && n != oif && oif->type != RT_IFT_LOOP)
			continue;
		for (ia = ifp->addrs; ia; ia = ia->next) {
			if (ia->flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED))
				continue;
			if (rt_in6_is_loopback(&ia->addr))
				continue;       /* ::1 only talks to ::1 */
			if (!best || src_better(ia, best, dst, oif))
				best = ia;
		}
	}
	if (!best)
		return -RT_EADDRNOTAVAIL;
	*src = best->addr;
	return 0;
}

void ip6_if_init(void)
{
}
