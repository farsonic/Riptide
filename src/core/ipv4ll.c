/*
 * Riptide - IPv4 link-local addresses (RFC 3927, "ZeroConf").
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * When DHCP gets no lease (dhcp.c starts us after net.inet.ip.linklocal_wait
 * seconds) the interface picks an address in 169.254.1.0 - 169.254.254.255:
 * the first candidate from a hash of the MAC address (the same one after
 * every boot when nobody else has it), later ones at random. Each candidate
 * is probed three times (ARP requests from 0.0.0.0, 1-2 s apart); an ARP
 * packet from anyone using it, or another host probing for it, is a
 * conflict and the next candidate is tried. Two seconds after the last
 * probe the address becomes the interface's primary address with a
 * 169.254.0.0/16 on-link route and no default route (so it is never used
 * to reach anything beyond the link), and is announced twice, two seconds
 * apart. A conflict later on is defended once (a gratuitous ARP); a second
 * one within ten seconds gives the address up and starts over. After ten
 * conflicts candidates are tried at most once a minute.
 *
 * DHCP keeps trying meanwhile; a lease replaces the address (dhcp.c calls
 * rt_ipv4ll_stop() before it configures the leased one), so a host never
 * has both (RFC 3927 1.9 allows dropping the link-local address).
 *
 * All entry points are called with the stack lock held.
 */
#include "infra.h"

/* RFC 3927 2.2 / 9 */
#define LL_PROBE_WAIT_MS     1000
#define LL_PROBE_NUM         3
#define LL_PROBE_MIN_MS      1000
#define LL_PROBE_MAX_MS      2000
#define LL_ANNOUNCE_WAIT_MS  2000
#define LL_ANNOUNCE_NUM      2
#define LL_ANNOUNCE_INT_MS   2000
#define LL_MAX_CONFLICTS     10
#define LL_RATE_LIMIT_MS     60000
#define LL_DEFEND_MS         10000

#define LL_NET   0xa9fe0000u          /* 169.254.0.0 */
#define LL_MASK  0xffff0000u

struct ll_if {
	struct ll_if *next;
	struct rt_netif *nif;
	struct rt_timer timer;
	u8  state;                     /* RT_LL_* */
	u8  count;                     /* probes or announcements sent */
	u32 cand;                      /* candidate or address, network order */
	u32 conflicts;
	u32 last_defend_ms;            /* 0 = never */
};

static struct ll_if *ll_list;

s32 rt_ll_enable = 1;              /* net.inet.ip.linklocal */
s32 rt_ll_wait_s = 10;             /* net.inet.ip.linklocal_wait */

static const struct rt_sysctl ll_sysctl[] = {
	{ "net.inet.ip.linklocal", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ll_enable, 0, 1, NULL, "IPv4 link-local address (169.254/16) when DHCP gets no lease" },
	{ "net.inet.ip.linklocal_wait", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_ll_wait_s, 1, 3600, NULL, "Seconds without a DHCP lease before a link-local address" },
};

int rt_ipv4ll_is_ll(u32 addr)
{
	return (rt_ntohl(addr) & LL_MASK) == LL_NET;
}

static struct ll_if *ll_find(const struct rt_netif *nif)
{
	struct ll_if *s;

	for (s = ll_list; s; s = s->next)
		if (s->nif == nif)
			return s;
	return NULL;
}

/* 169.254.1.0 - 169.254.254.255 (RFC 3927 2.1); the first try from the
 * MAC address so that a host gets the same address again */
static u32 pick(struct ll_if *s)
{
	u32 n, i;

	if (s->conflicts == 0 && s->nif->hwlen) {
		n = 2166136261u;                       /* FNV-1a */
		for (i = 0; i < s->nif->hwlen; i++)
			n = (n ^ s->nif->hwaddr[i]) * 16777619u;
	} else {
		n = rt_random32();
	}
	return rt_htonl(LL_NET + 0x100u + n % 0xfe00u);
}

static u32 rnd_ms(u32 lo, u32 hi)
{
	return lo + rt_random32() % (hi - lo + 1u);
}

static void give_up_addr(struct ll_if *s)
{
	if ((s->state == RT_LL_ANNOUNCE || s->state == RT_LL_BOUND) && s->nif->addr == s->cand)
		rt_netif_set_addr(s->nif, 0, 0, 0);
}

/* the next candidate after a conflict (or at start) */
static void restart(struct ll_if *s, int conflict)
{
	char a[16];

	if (conflict) {
		s->conflicts++;
		rt_log(RT_LOG_WARN, "ipv4ll: %s: %s is in use, trying another address", s->nif->name,
		       rt_format_ipv4(s->cand, a));
	}
	give_up_addr(s);
	s->cand = pick(s);
	s->state = RT_LL_PROBE;
	s->count = 0;
	rt_timer_after(&s->timer, s->conflicts >= LL_MAX_CONFLICTS ? LL_RATE_LIMIT_MS :
	                          rnd_ms(0, LL_PROBE_WAIT_MS));
}

static void ll_timeout(void *arg)
{
	struct ll_if *s = arg;
	struct rt_netif *nif = s->nif;
	char a[16];

	switch (s->state) {
	case RT_LL_PROBE:
		/* RFC 3927 1.9: not while the interface has a routable address
		 * (static or leased): look again later */
		if (nif->addr && !rt_ipv4ll_is_ll(nif->addr)) {
			rt_timer_after(&s->timer, LL_RATE_LIMIT_MS);
			return;
		}
		if (s->count < LL_PROBE_NUM) {
			rt_arp_probe(nif, s->cand);
			s->count++;
			rt_timer_after(&s->timer, s->count < LL_PROBE_NUM ?
			               rnd_ms(LL_PROBE_MIN_MS, LL_PROBE_MAX_MS) : LL_ANNOUNCE_WAIT_MS);
			return;
		}
		/* claim it: rt_netif_set_addr() sends the first announcement */
		if (rt_netif_set_addr(nif, s->cand, rt_htonl(LL_MASK), 0) < 0) {
			restart(s, 0);
			return;
		}
		s->state = RT_LL_ANNOUNCE;
		s->count = 1;
		rt_log(RT_LOG_INFO, "ipv4ll: %s: link-local address %s", nif->name,
		       rt_format_ipv4(s->cand, a));
		rt_timer_after(&s->timer, LL_ANNOUNCE_INT_MS);
		return;
	case RT_LL_ANNOUNCE:
		if (nif->addr != s->cand) {        /* replaced by hand */
			s->state = RT_LL_BOUND;
			return;
		}
		rt_arp_announce(nif, s->cand);
		if (++s->count < LL_ANNOUNCE_NUM)
			rt_timer_after(&s->timer, LL_ANNOUNCE_INT_MS);
		else
			s->state = RT_LL_BOUND;
		return;
	}
}

int rt_ipv4ll_start(struct rt_netif *nif)
{
	struct ll_if *s;

	if (!nif || nif->type != RT_IFT_ETHER || nif->ip4_disabled || !nif->hwlen)
		return -RT_EINVAL;
	s = ll_find(nif);
	if (s)
		return 0;
	s = rt_zalloc(sizeof(*s));
	if (!s)
		return -RT_ENOMEM;
	s->nif = nif;
	rt_timer_init(&s->timer, ll_timeout, s);
	s->next = ll_list;
	ll_list = s;
	RT_DBG(RT_DBG_DHCP, "%s: no DHCP lease, choosing a link-local address", nif->name);
	restart(s, 0);
	return 0;
}

void rt_ipv4ll_stop(struct rt_netif *nif)
{
	struct ll_if **pp, *s;

	for (pp = &ll_list; (s = *pp) != NULL; pp = &s->next)
		if (s->nif == nif)
			break;
	if (!s)
		return;
	*pp = s->next;
	rt_timer_del(&s->timer);
	give_up_addr(s);
	rt_free(s);
}

/* From ARP input, before its own handling (RFC 3927 2.2.1 and 2.5).
 * Returns 1 when the packet was a conflict with our address that this
 * code dealt with. */
int rt_ipv4ll_arp(struct rt_netif *nif, u16 op, const u8 *sha, u32 sip, u32 tip)
{
	struct ll_if *s = ll_find(nif);
	u32 now;

	if (!s || !s->cand || !memcmp(sha, nif->hwaddr, 6))
		return 0;
	if (s->state == RT_LL_PROBE) {
		/* someone uses the candidate, or probes for it too */
		if (s->count > 0 && (sip == s->cand || (op == 1 && sip == 0 && tip == s->cand)))
			restart(s, 1);
		return 0;
	}
	if (sip != s->cand || nif->addr != s->cand)
		return 0;
	now = rt_now_ms();
	if (s->last_defend_ms && (s32)(now - s->last_defend_ms) < LL_DEFEND_MS) {
		restart(s, 1);              /* second conflict: give it up */
		return 1;
	}
	s->last_defend_ms = now ? now : 1;
	rt_arp_announce(nif, s->cand);
	return 1;
}

u32 rt_ipv4ll_info(const struct rt_netif *nif, u8 *state, u32 *conflicts)
{
	const struct ll_if *s = ll_find(nif);

	if (state)
		*state = s ? s->state : RT_LL_OFF;
	if (conflicts)
		*conflicts = s ? s->conflicts : 0;
	return s && (s->state == RT_LL_ANNOUNCE || s->state == RT_LL_BOUND) && nif->addr == s->cand ?
	       s->cand : 0;
}

void rt_ipv4ll_if_gone(struct rt_netif *nif)
{
	rt_ipv4ll_stop(nif);
}

void rt_ipv4ll_init(void)
{
	ll_list = NULL;
	rt_ll_enable = 1;
	rt_ll_wait_s = 10;
	rt_sysctl_register(ll_sysctl, (int)RT_ARRAY_SIZE(ll_sysctl));
}
