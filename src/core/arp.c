/*
 * Riptide - ARP (RFC 826) with a Linux style neighbour state machine.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * States (Linux neigh NUD_*):
 *   INCOMPLETE  request broadcast, up to ARP_UNRES_QLEN packets queued;
 *               ARP_MCAST_PROBES tries ARP_RETRANS_MS apart, then the
 *               queued packets get ICMP host unreachable and the entry dies.
 *   REACHABLE   confirmed by a reply; becomes STALE after reachable time
 *               (ARP_REACHABLE_MS, randomised 0.5..1.5 as Linux does).
 *   STALE       still used for sending; the first use starts PROBE.
 *   PROBE       unicast requests (ARP_UCAST_PROBES, ARP_RETRANS_MS apart)
 *               while still sending; no answer -> entry deleted.
 *   STATIC      administratively set, never expires.
 * Unused STALE entries are garbage collected after ARP_GC_STALE_MS.
 *
 * As in Linux there is no DELAY state waiting for upper layer
 * confirmation: our transports do not report forward progress, so a stale
 * entry is re-validated by a unicast probe right away.
 */
#include "infra.h"

#define ARP_UNRES_QLEN    3
#define ARP_MCAST_PROBES  3
#define ARP_UCAST_PROBES  3
#define ARP_RETRANS_MS    1000u
#define ARP_REACHABLE_MS  60000u
#define ARP_GC_STALE_MS   600000u
#define ARP_MAX_ENTRIES   256
#define ARP_HASH          64

/* public states (rt_arp_entry_info.state) */
#define ST_INCOMPLETE 0
#define ST_REACHABLE  1
#define ST_STALE      2
#define ST_STATIC     3
/* internal only (reported as STALE) */
#define ST_PROBE      4

#define ARP_HLEN 28
#define ARPOP_REQUEST 1
#define ARPOP_REPLY   2

struct arp_entry {
	struct arp_entry *next;     /* hash chain */
	struct rt_netif *nif;
	u32 ip;
	u8  hw[6];
	u8  state;
	u8  probes;                 /* requests sent in this state */
	u32 updated_ms;             /* last confirmation / state change */
	u32 used_ms;                /* last transmit through the entry */
	struct rt_bufq pending;     /* INCOMPLETE queue */
	struct rt_timer timer;
};

static struct arp_entry *arp_tab[ARP_HASH];
static int arp_count;

static unsigned arp_hash(u32 ip)
{
	u32 h = ip ^ (ip >> 16);
	return (h ^ (h >> 8)) & (ARP_HASH - 1);
}

static struct arp_entry *arp_lookup(struct rt_netif *nif, u32 ip)
{
	struct arp_entry *e;

	for (e = arp_tab[arp_hash(ip)]; e; e = e->next)
		if (e->ip == ip && e->nif == nif)
			return e;
	return NULL;
}

static void arp_timer_fn(void *arg);

static void arp_destroy(struct arp_entry *e)
{
	struct arp_entry **pp;

	for (pp = &arp_tab[arp_hash(e->ip)]; *pp; pp = &(*pp)->next) {
		if (*pp == e) {
			*pp = e->next;
			break;
		}
	}
	rt_timer_del(&e->timer);
	rt_bufq_purge(&e->pending);
	rt_free(e);
	arp_count--;
}

/* Make room: drop the least recently used non-static, non-incomplete
 * entry (Linux neigh_forced_gc). */
static int arp_forced_gc(void)
{
	struct arp_entry *e, *victim = NULL;
	u32 now = rt_now_ms();
	int i;

	for (i = 0; i < ARP_HASH; i++)
		for (e = arp_tab[i]; e; e = e->next)
			if (e->state != ST_STATIC &&
			    (!victim || (s32)((now - e->used_ms) - (now - victim->used_ms)) > 0))
				victim = e;
	if (!victim)
		return -1;
	arp_destroy(victim);
	return 0;
}

static struct arp_entry *arp_create(struct rt_netif *nif, u32 ip)
{
	struct arp_entry *e;
	unsigned h;

	if (arp_count >= ARP_MAX_ENTRIES && arp_forced_gc() < 0)
		return NULL;
	e = rt_zalloc(sizeof(*e));
	if (!e)
		return NULL;
	e->nif = nif;
	e->ip = ip;
	e->state = ST_INCOMPLETE;
	e->updated_ms = e->used_ms = rt_now_ms();
	rt_bufq_init(&e->pending);
	rt_timer_init(&e->timer, arp_timer_fn, e);
	h = arp_hash(ip);
	e->next = arp_tab[h];
	arp_tab[h] = e;
	arp_count++;
	return e;
}

static u32 reachable_time(void)
{
	/* uniform in [0.5, 1.5) * base (Linux neigh_rand_reach_time) */
	return ARP_REACHABLE_MS / 2 + rt_random32() % ARP_REACHABLE_MS;
}

/* Build and send one ARP packet. tha NULL = zeros. dst NULL = broadcast. */
static void arp_send(struct rt_netif *nif, u16 op, const u8 *dst, u32 sip,
                     const u8 *tha, u32 tip)
{
	struct rt_buf *b;
	u8 *p;

	if (!(nif->flags & RT_IFF_UP) || (nif->flags & RT_IFF_NOARP))
		return;
	b = rt_buf_alloc(RT_BUF_HEADROOM, ARP_HLEN);
	if (!b)
		return;
	p = rt_buf_put(b, ARP_HLEN);
	rt_put_be16(p + 0, 1);              /* hardware: Ethernet */
	rt_put_be16(p + 2, RT_ETH_P_IP);
	p[4] = 6;
	p[5] = 4;
	rt_put_be16(p + 6, op);
	memcpy(p + 8, nif->hwaddr, 6);
	memcpy(p + 14, &sip, 4);
	if (tha)
		memcpy(p + 18, tha, 6);
	else
		memset(p + 18, 0, 6);
	memcpy(p + 24, &tip, 4);
	RT_DBG(RT_DBG_ARP, "%s: send %s for %u.%u.%u.%u", nif->name, op == ARPOP_REQUEST ? "request" : "reply",
	       p[24], p[25], p[26], p[27]);
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	if (nif->link_output(nif, b, dst, RT_ETH_P_ARP) < 0)
		nif->stats.oerrors++;
}

static void arp_solicit(struct arp_entry *e, int unicast)
{
	struct rt_netif *nif = e->nif;
	u32 sip = nif->addr;

	e->probes++;
	arp_send(nif, ARPOP_REQUEST, unicast ? e->hw : NULL, sip, NULL, e->ip);
}

static void arp_xmit(struct arp_entry *e, struct rt_buf *b)
{
	struct rt_netif *nif = e->nif;

	e->used_ms = rt_now_ms();
	if (nif->link_output(nif, b, e->hw, RT_ETH_P_IP) < 0)
		nif->stats.oerrors++;
}

static void arp_set_state(struct arp_entry *e, u8 st, u32 timeout_ms)
{
	e->state = st;
	e->probes = 0;
	e->updated_ms = rt_now_ms();
	if (timeout_ms)
		rt_timer_after(&e->timer, timeout_ms);
	else
		rt_timer_del(&e->timer);
}

static void arp_timer_fn(void *arg)
{
	struct arp_entry *e = arg;
	struct rt_buf *b;
	u32 now = rt_now_ms();

	switch (e->state) {
	case ST_INCOMPLETE:
		if (e->probes < ARP_MCAST_PROBES) {
			arp_solicit(e, 0);
			rt_timer_after(&e->timer, ARP_RETRANS_MS);
			return;
		}
		RT_DBG(RT_DBG_ARP, "%s: no answer from %u.%u.%u.%u", e->nif->name, ((u8 *)&e->ip)[0],
		       ((u8 *)&e->ip)[1], ((u8 *)&e->ip)[2], ((u8 *)&e->ip)[3]);
		/* Resolution failed: tell the senders (RFC 1122 2.3.2.2). The
		 * error to a local source is looped back to the transport. */
		{
			/* Destroy first: sending the errors may create entries. */
			struct rt_bufq q = e->pending;
			rt_bufq_init(&e->pending);
			arp_destroy(e);
			while ((b = rt_bufq_dequeue(&q)) != NULL) {
				rt_icmp_send_error(b, RT_ICMP_UNREACH, RT_ICMP_UNREACH_HOST, 0);
				rt_buf_free(b);
			}
		}
		return;
	case ST_REACHABLE:
		arp_set_state(e, ST_STALE, ARP_GC_STALE_MS);
		return;
	case ST_STALE:
		if ((s32)(now - e->used_ms) >= (s32)ARP_GC_STALE_MS) {
			arp_destroy(e);
			return;
		}
		rt_timer_mod(&e->timer, e->used_ms + ARP_GC_STALE_MS);
		return;
	case ST_PROBE:
		if (e->probes < ARP_UCAST_PROBES) {
			arp_solicit(e, 1);
			rt_timer_after(&e->timer, ARP_RETRANS_MS);
			return;
		}
		arp_destroy(e);
		return;
	}
}

int rt_arp_resolve_output(struct rt_netif *nif, struct rt_buf *b, u32 nexthop)
{
	struct arp_entry *e = arp_lookup(nif, nexthop);

	if (nif->flags & RT_IFF_NOARP) {
		/* no resolution possible: broadcast at link level */
		return nif->link_output(nif, b, NULL, RT_ETH_P_IP);
	}
	if (!e) {
		e = arp_create(nif, nexthop);
		if (!e) {
			nif->stats.oerrors++;
			rt_buf_free(b);
			return -RT_ENOBUFS;
		}
		rt_bufq_tail(&e->pending, b);
		arp_solicit(e, 0);
		rt_timer_after(&e->timer, ARP_RETRANS_MS);
		return 0;
	}
	switch (e->state) {
	case ST_INCOMPLETE:
		/* Linux unres_qlen: drop the oldest when full */
		if (e->pending.qlen >= ARP_UNRES_QLEN) {
			nif->stats.oerrors++;
			rt_buf_free(rt_bufq_dequeue(&e->pending));
		}
		rt_bufq_tail(&e->pending, b);
		return 0;
	case ST_STALE:
		arp_set_state(e, ST_PROBE, ARP_RETRANS_MS);
		arp_solicit(e, 1);
		break;
	default:
		break;
	}
	arp_xmit(e, b);
	return 0;
}

/* Learn ip -> hw. 'confirmed': reply addressed to us (REACHABLE) rather
 * than just overheard information (STALE). */
static void arp_update(struct arp_entry *e, const u8 *hw, int confirmed)
{
	struct rt_buf *b;
	int changed = memcmp(e->hw, hw, 6) != 0;

	if (e->state == ST_STATIC)
		return;
	if (!confirmed && !changed && e->state != ST_INCOMPLETE)
		return;               /* nothing new; keep the current state */
	memcpy(e->hw, hw, 6);
	if (confirmed)
		arp_set_state(e, ST_REACHABLE, reachable_time());
	else
		arp_set_state(e, ST_STALE, ARP_GC_STALE_MS);
	while ((b = rt_bufq_dequeue(&e->pending)) != NULL)
		arp_xmit(e, b);
}

void rt_arp_input(struct rt_netif *nif, struct rt_buf *b)
{
	const u8 *p = b->data;
	u8 sha[6], tha[6];
	u32 sip, tip;
	u16 op;
	int for_us;
	struct arp_entry *e;

	if (b->len < ARP_HLEN || rt_get_be16(p) != 1 || rt_get_be16(p + 2) != RT_ETH_P_IP ||
	    p[4] != 6 || p[5] != 4)
		goto drop;
	op = rt_get_be16(p + 6);
	if (op != ARPOP_REQUEST && op != ARPOP_REPLY)
		goto drop;
	memcpy(sha, p + 8, 6);
	memcpy(&sip, p + 14, 4);
	memcpy(tha, p + 18, 6);
	memcpy(&tip, p + 24, 4);
	RT_DBG(RT_DBG_ARP, "%s: %s from %u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x)", nif->name,
	       op == ARPOP_REQUEST ? "request" : "reply", p[14], p[15], p[16], p[17],
	       sha[0], sha[1], sha[2], sha[3], sha[4], sha[5]);

	/* Our own frames looped back, or senders with nonsense addresses. */
	if (memcmp(sha, nif->hwaddr, 6) == 0 || (sha[0] & 1))
		goto drop;
	if (rt_ip_is_multicast(sip) || sip == RT_INADDR_BROADCAST || rt_ip_is_loopback_net(sip))
		goto drop;

	if (nif->addr && sip == nif->addr) {
		/* Someone else claims our address (RFC 5227 2.4): defend by
		 * announcing once, and leave the table alone. */
		static u32 last_defend;
		u32 now = rt_now_ms();

		if (last_defend && (s32)(now - last_defend) < 10000)
			goto drop;    /* RFC 5227 DEFEND_INTERVAL */
		last_defend = now ? now : 1;
		rt_log(RT_LOG_WARN, "arp: address conflict on %s with %02x:%02x:%02x:%02x:%02x:%02x",
		       nif->name, sha[0], sha[1], sha[2], sha[3], sha[4], sha[5]);
		rt_arp_announce(nif, nif->addr);
		goto drop;
	}

	for_us = nif->addr && tip == nif->addr;
	e = sip ? arp_lookup(nif, sip) : NULL;

	if (sip && op == ARPOP_REPLY && !e && memcmp(tha, nif->hwaddr, 6) == 0) {
		/* Reply to one of our probes (e.g. DHCP conflict detection with
		 * sender 0.0.0.0): record it so the caller can see it. */
		e = arp_create(nif, sip);
	}
	if (sip && !e && for_us) {
		/* RFC 826 "merge": a peer that talks to us will be talked to. */
		e = arp_create(nif, sip);
	}
	if (e)
		arp_update(e, sha, op == ARPOP_REPLY && (for_us || memcmp(tha, nif->hwaddr, 6) == 0));

	if (op == ARPOP_REQUEST && for_us)
		arp_send(nif, ARPOP_REPLY, sha, nif->addr, sha, sip);
drop:
	rt_buf_free(b);
}

void rt_arp_flush(struct rt_netif *nif, int keep_static)
{
	int i;

	for (i = 0; i < ARP_HASH; i++) {
		struct arp_entry *e = arp_tab[i], *n;
		for (; e; e = n) {
			n = e->next;
			if ((!nif || e->nif == nif) && !(keep_static && e->state == ST_STATIC))
				arp_destroy(e);
		}
	}
}

void rt_arp_flush_if(struct rt_netif *nif)
{
	rt_arp_flush(nif, 0);
}

int rt_arp_add_static(struct rt_netif *nif, u32 ip, const u8 *hw)
{
	struct arp_entry *e;

	if (!nif || nif->type != RT_IFT_ETHER || !ip || (hw[0] & 1))
		return -RT_EINVAL;
	e = arp_lookup(nif, ip);
	if (!e && !(e = arp_create(nif, ip)))
		return -RT_ENOMEM;
	arp_update(e, hw, 1);
	arp_set_state(e, ST_STATIC, 0);
	return 0;
}

int rt_arp_delete(u32 ip)
{
	int i, n = 0;

	for (i = 0; i < ARP_HASH; i++) {
		struct arp_entry *e = arp_tab[i], *nx;
		for (; e; e = nx) {
			nx = e->next;
			if (e->ip == ip) {
				arp_destroy(e);
				n++;
			}
		}
	}
	return n ? 0 : -RT_ENOENT;
}

int rt_arp_list(struct rt_arp_entry_info *out, int max)
{
	u32 now = rt_now_ms();
	int i, n = 0;

	for (i = 0; i < ARP_HASH; i++) {
		struct arp_entry *e;
		for (e = arp_tab[i]; e; e = e->next, n++) {
			if (n >= max || !out)
				continue;
			out[n].ip = e->ip;
			memcpy(out[n].hw, e->hw, 6);
			out[n].state = e->state == ST_PROBE ? ST_STALE : e->state;
			out[n].age_s = (now - e->updated_ms) / 1000;
			out[n].ifindex = e->nif->index;
		}
	}
	return n;
}

int rt_arp_get(const struct rt_netif *nif, u32 ip, struct rt_arp_entry_info *out)
{
	struct arp_entry *e;

	for (e = arp_tab[arp_hash(ip)]; e; e = e->next) {
		if (e->ip != ip || (nif && e->nif != nif))
			continue;
		out->ip = e->ip;
		memcpy(out->hw, e->hw, 6);
		out->state = e->state == ST_PROBE ? ST_STALE : e->state;
		out->age_s = (rt_now_ms() - e->updated_ms) / 1000;
		out->ifindex = e->nif->index;
		return 0;
	}
	return -RT_ENOENT;
}

/* ip == interface address: gratuitous ARP announcement (RFC 5227 3);
 * otherwise an address probe with sender 0.0.0.0 (RFC 5227 2.1.1). */
void rt_arp_announce(struct rt_netif *nif, u32 ip)
{
	if (nif->type != RT_IFT_ETHER)
		return;
	if (ip == nif->addr)
		arp_send(nif, ARPOP_REQUEST, NULL, ip, NULL, ip);
	else
		arp_send(nif, ARPOP_REQUEST, NULL, 0, NULL, ip);
}

void rt_arp_init(void)
{
	memset(arp_tab, 0, sizeof(arp_tab));
	arp_count = 0;
}
