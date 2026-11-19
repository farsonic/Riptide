/* src/mac/net_mac.c - Riptide on the classic Mac OS: the Ethernet interfaces, through the .ENET
 * drivers the way MacTCP's Ethernet link layer uses them (Inside Macintosh: Networking, "Ethernet
 * Driver"). Every card the Slot Manager lists as catNetwork/typeEtherNet (SGetTypeSRsrc, all of
 * them) can be an interface of its own, each opened with OpenSlot on its slot and sResource id, with
 * its own refnum, protocol handlers, receive ring and transmit slots; without a card in a slot,
 * ".ENET" is opened by name (a built-in Ethernet port).
 *
 * One NuBus card may hold several: Aurora68's slot-9 card has its wired port (sResource $81,
 * .Network_EtherNet_PiStorm) and its WiFi ($83, .Network_WiFi_PiStorm) side by side, so a card is
 * named by slot AND sResource id, by its kind ("ethernet", "wifi", from the driver's name), by part
 * of its driver's name, or by its place in the list (unit=N, the Amiga's form). riptide.conf:
 *     interface en0 device=.ENET card=ethernet        (or unit=0, or slot=9 id=0x81)
 *     interface en1 device=.ENET card=wifi            (or unit=1, or slot=9 id=0x83)
 * Received frames: protocol handlers for IPv4, ARP and IPv6 (drvr.S, mac_enet_phN: one entry per
 * card, since a driver does not say which card it is) copy each frame into that card's ring, which
 * the stack's deferred task drains here. Sent frames: copied into one of a few transmit slots and
 * handed to an asynchronous EWrite, whose completion frees the slot.
 *
 * Two cards, one default route: each interface has a metric (wired 10, WiFi 20, or metric= on the
 * interface line), which DHCP gives the routes it learns, so the wired link wins while it is up. A
 * driver that can say whether its link is up (Aurora68's: csLinkStatus, 230, answered with 'A68L')
 * is asked every second; the core then stops using an interface whose link is down
 * (rt_netif_link_change) and the next route, the WiFi's, takes over until it comes back. Other
 * drivers are taken as always up.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_buf.h"
#include "rt_netif.h"
#include "mac.h"

#define ENET_DELMULTI 245
#define ENET_ADDMULTI 246
#define ENET_ATTACHPH 247
#define ENET_WRITE    249
#define ENET_GETINFO  252
#define ENET_LINKSTATUS 230                       /* Aurora68's drivers' own (mac/enet.s) */
#define E_PROTTYPE    28
#define E_POINTER     30
#define E_BUFFSIZE    34
#define ETH_HLEN      14
#define RX_SLOTS      32

struct mac_rxring mac_rx[MAC_CARDS];
struct rt_netif *mac_nif;
char mac_net_why[64];

enum { KIND_ETHERNET = 1, KIND_WIFI = 2 };
/* a card as the Slot Manager lists it */
struct found {
	u8 slot, id, kind;
	char drvr[34];                                /* the driver's name, ".Network_WiFi_PiStorm" */
};
static struct found found[MAC_CARDS];
static int nfound = -1;

#define TX_SLOTS 8
#define MC_PBS 4
struct txslot { u8 pb[64]; u16 wds[6]; u8 frame[1516]; volatile u8 busy; };
/* an open card: an interface of the core */
static struct card {
	int open, index;                              /* index: the found[] entry; -1 the built-in .ENET */
	s16 ref;
	struct rt_netif nif;
	struct txslot *tx;
	u8 mc_pb[MC_PBS][40];
	u8 link_pb[40];                               /* csLinkStatus, asked asynchronously */
	u8 link_known, link_up, link_asked;
	u32 link_tick, tx_drops;
} cards[MAC_CARDS];
static void (*const handlers[MAC_CARDS])(void) = { mac_enet_ph0, mac_enet_ph1, mac_enet_ph2, mac_enet_ph3 };

static void why(const char *s) { u32 i = 0; for (; s[i] && i < sizeof mac_net_why - 1; i++) mac_net_why[i] = s[i]; mac_net_why[i] = 0; rt_log(RT_LOG_WARN, "%s", s); }

static int has_word(const char *s, const char *w)
{
	u32 n = (u32)strlen(w);
	for (; *s; s++)
		if (!rt_strncasecmp(s, w, n))
			return 1;
	return 0;
}

/* The Slot Manager's list of Ethernet cards: SGetTypeSRsrc from slot 0, id 0, then from each one
 * found, with fAll so that a card the Start Manager did not open is listed too. The driver's name
 * (SReadDrvrName) says what kind of card it is: "WiFi", "Wireless" or "AirPort" in it is a WiFi
 * card, anything else wired Ethernet. */
static void find_cards(void)
{
	u8 spb[56];
	nfound = 0;
	memset(spb, 0, sizeof spb);
	while (nfound < MAC_CARDS) {
		u8 slot = spb[49], id = spb[50], ext = spb[51];
		memset(spb, 0, sizeof spb);
		PB_W(spb, 40) = 4;                        /* spCategory: catNetwork */
		PB_W(spb, 42) = 1;                        /* spCType: typeEtherNet */
		spb[48] = 3;                              /* spTBMask: any driver software and hardware */
		PB_L(spb, 24) = 1;                        /* spParamData: fAll */
		spb[49] = slot; spb[50] = id; spb[51] = ext;
		if (mac_slotmanager(spb, 0x0C) != 0)      /* SGetTypeSRsrc: the next one after slot/id */
			break;
		struct found *f = &found[nfound];
		u8 name[64], spb2[56];
		memset(f, 0, sizeof *f);
		f->slot = spb[49];
		f->id = spb[50];
		memset(spb2, 0, sizeof spb2);
		memset(name, 0, sizeof name);
		spb2[49] = f->slot;
		spb2[50] = f->id;
		PB_P(spb2, 0) = name;                     /* spResult: where the Pascal string goes */
		if (mac_slotmanager(spb2, 0x19) == 0 && name[0]) {     /* SReadDrvrName */
			u32 n = name[0] < sizeof f->drvr - 1 ? name[0] : sizeof f->drvr - 1;
			memcpy(f->drvr, name + 1, n);
			f->drvr[n] = 0;
		} else
			rt_snprintf(f->drvr, sizeof f->drvr, ".ENET");
		f->kind = has_word(f->drvr, "wifi") || has_word(f->drvr, "wireless") || has_word(f->drvr, "airport") ? KIND_WIFI : KIND_ETHERNET;
		rt_log(RT_LOG_INFO, "card %d: slot %u sResource $%02x %s (%s)", nfound, f->slot, f->id, f->drvr,
		       f->kind == KIND_WIFI ? "wifi" : "ethernet");
		nfound++;
		if (spb[49] == slot && spb[50] == id)
			break;                                /* no progress: stop */
	}
}

int net_mac_cards(void)
{
	if (nfound < 0)
		find_cards();
	return nfound;
}
int net_mac_card_info(int i, char *buf, u32 len)
{
	if (i < 0 || i >= net_mac_cards())
		return -RT_ENODEV;
	return rt_snprintf(buf, len, "slot %u id $%02x %s (%s)", found[i].slot, found[i].id, found[i].drvr,
	                   found[i].kind == KIND_WIFI ? "wifi" : "ethernet");
}

static int card_taken(int index)
{
	for (int i = 0; i < MAC_CARDS; i++)
		if (cards[i].open && cards[i].index == index)
			return 1;
	return 0;
}

/* which found[] card a selection names: its index, -1 the built-in .ENET, -2 none */
static int select_card(const struct mac_cardsel *sel)
{
	int n = net_mac_cards();
	if (!n)
		return sel->unit <= 0 && sel->slot < 0 && sel->id < 0 && !sel->name ? -1 : -2;
	if (sel->unit >= 0 && sel->slot < 0 && sel->id < 0 && !sel->name)
		return sel->unit < n ? sel->unit : -2;
	for (int i = 0; i < n; i++) {
		const struct found *f = &found[i];
		if (sel->slot >= 0 && f->slot != sel->slot)
			continue;
		if (sel->id >= 0 && f->id != sel->id)
			continue;
		if (sel->name) {
			if (!rt_strcasecmp(sel->name, "wifi") || !rt_strcasecmp(sel->name, "wireless")) {
				if (f->kind != KIND_WIFI)
					continue;
			} else if (!rt_strcasecmp(sel->name, "ethernet") || !rt_strcasecmp(sel->name, "wired")) {
				if (f->kind != KIND_ETHERNET)
					continue;
			} else if (!has_word(f->drvr, sel->name))
				continue;
		}
		if (card_taken(i))
			continue;                             /* two lines naming one kind: the next such card */
		return i;
	}
	return -2;
}

static int enet_ctl(struct card *c, int cs, u16 prot, void *ptr, u16 size)
{
	u8 pb[64];
	memset(pb, 0, sizeof pb);
	PB_REFNUM(pb) = c->ref;
	PB_CSCODE(pb) = (s16)cs;
	PB_W(pb, E_PROTTYPE) = prot;
	PB_P(pb, E_POINTER) = ptr;
	PB_W(pb, E_BUFFSIZE) = size;
	return mac_pbcontrol(pb, 0);
}

/* A link layer multicast group in or out of the card's filter (IPv6's all-nodes and solicited-node
 * groups, 33:33:..., mDNS's 01:00:5e:00:00:fb): EAddMulti / EDelMulti with the address in csParam.
 * The core calls this from the stack's deferred task, where a synchronous Device Manager call could
 * wait on a busy driver for ever, so it is asynchronous, from a few parameter blocks used in turn. */
static int mac_mcast(struct rt_netif *n, const u8 *hw, int join)
{
	struct card *c = n->driver;
	for (int i = 0; i < MC_PBS; i++) {
		u8 *pb = c->mc_pb[i];
		if (PB_RESULT(pb) > 0)
			continue;                             /* still in progress */
		memset(pb, 0, sizeof c->mc_pb[i]);
		PB_REFNUM(pb) = c->ref;
		PB_CSCODE(pb) = join ? ENET_ADDMULTI : ENET_DELMULTI;
		memcpy(pb + 28, hw, 6);
		return mac_pbcontrol(pb, 1) < 0 ? -RT_EIO : 0;
	}
	return -RT_EBUSY;
}

static int mac_link_output(struct rt_netif *n, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct card *c = n->driver;
	struct txslot *t = NULL;
	for (int i = 0; i < TX_SLOTS; i++) if (!c->tx[i].busy) { t = &c->tx[i]; break; }
	if (!t || b->len > 1500) { c->tx_drops++; rt_buf_free(b); return -RT_ENOBUFS; }
	t->busy = 1;
	memcpy(t->frame, dsthw ? dsthw : bcast, 6);
	memcpy(t->frame + 6, n->hwaddr, 6);           /* the driver puts its own in as well */
	rt_put_be16(t->frame + 12, ethertype);
	memcpy(t->frame + ETH_HLEN, b->data, b->len);
	u32 len = ETH_HLEN + b->len;
	rt_buf_free(b);
	/* WDS: one entry (length word, pointer long), then a zero length */
	u8 *fp = t->frame;
	t->wds[0] = (u16)len; memcpy(&t->wds[1], &fp, 4); t->wds[3] = 0;
	memset(t->pb, 0, sizeof t->pb);
	PB_REFNUM(t->pb) = c->ref;
	PB_CSCODE(t->pb) = ENET_WRITE;
	PB_P(t->pb, E_POINTER) = t->wds;
	PB_COMPLETION(t->pb) = (void *)mac_tx_done;
	if (mac_pbcontrol(t->pb, 1) < 0) { t->busy = 0; c->tx_drops++; return -RT_EIO; }
	return 0;
}
void net_mac_tx_done(void *pb)
{
	for (int k = 0; k < MAC_CARDS; k++)
		if (cards[k].open)
			for (int i = 0; i < TX_SLOTS; i++)
				if (cards[k].tx[i].pb == pb) { cards[k].tx[i].busy = 0; return; }
}

/* Open one card as interface NAME (riptide.conf's "interface NAME device=.ENET [unit=N] [slot=S]
 * [id=N] [card=ethernet|wifi|NAME] [metric=N]", else en0, en1 ... in the Slot Manager's order). */
int net_mac_open(const char *name, const struct mac_cardsel *sel, int mtu)
{
	static const u8 shell[] = "\x05.ENET";
	u8 info[18], pb[64];
	struct card *c = NULL;
	int k, idx;
	for (k = 0; k < MAC_CARDS; k++)
		if (!cards[k].open) { c = &cards[k]; break; }
	if (!c) { why("Riptide for the Mac drives at most four cards"); return -RT_EEXIST; }
	idx = select_card(sel);
	if (idx == -2) {
		char m[64];
		rt_snprintf(m, sizeof m, "%s: no such Ethernet card (%d found)", name, net_mac_cards());
		why(m);
		return -RT_ENODEV;
	}
	memset(c, 0, sizeof *c);
	c->index = idx;
	struct mac_rxring *r = &mac_rx[k];
	if (!r->ring) {
		r->ring = mac_newptr_sys((u32)RX_SLOTS * MAC_RX_SLOT);
		r->slots = RX_SLOTS;
		r->mask = RX_SLOTS - 1;
	}
	c->tx = mac_newptr_sys(sizeof(struct txslot) * TX_SLOTS);
	if (!r->ring || !c->tx) { why("not enough memory for the Ethernet buffers"); return -RT_ENOMEM; }
	memset(c->tx, 0, sizeof(struct txslot) * TX_SLOTS);
	/* OpenSlot on the .ENET shell, with the card's slot and sResource id: the shell opens that
	   sResource's driver, so two functions of one card are two drivers with two refnums */
	memset(pb, 0, sizeof pb);
	PB_NAME(pb) = (u8 *)shell;
	int e;
	if (idx >= 0) {
		pb[34] = found[idx].slot;                 /* ioSlot */
		pb[35] = found[idx].id;                   /* ioID */
		e = mac_pbopen(pb, 1);
	} else
		e = mac_pbopen(pb, 0);                    /* a built-in port */
	if (e) {
		char m[64];
		rt_snprintf(m, sizeof m, "%s: .ENET would not open (%d)", name, e);
		why(m);
		return -RT_ENODEV;
	}
	c->ref = PB_REFNUM(pb);
	memset(info, 0, sizeof info);
	if (enet_ctl(c, ENET_GETINFO, 0, info, sizeof info)) { why(".ENET EGetInfo failed"); return -RT_EIO; }
	for (int j = 0; j < MAC_CARDS; j++)
		if (cards[j].open && cards[j].ref == c->ref) {
			char m[64];
			rt_snprintf(m, sizeof m, "%s: the same driver as %s (refnum %d)", name, cards[j].nif.name, (int)c->ref);
			why(m);
			return -RT_EEXIST;
		}
	struct rt_netif *n = &c->nif;
	n->type = RT_IFT_ETHER;
	n->hwlen = 6;
	memcpy(n->hwaddr, info, 6);
	n->flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	n->mtu = mtu >= 576 && mtu < 1500 ? (u16)mtu : 1500;
	n->link_output = mac_link_output;
	n->mcast = mac_mcast;
	n->driver = c;
	n->metric = sel->metric >= 0 ? (u32)sel->metric : (idx >= 0 && found[idx].kind == KIND_WIFI) ? 20 : 10;
	rt_snprintf(n->name, sizeof n->name, "%s", name);
	if (idx >= 0)
		rt_snprintf(n->descr, sizeof n->descr, "%s slot %u id $%02x", found[idx].drvr, found[idx].slot, found[idx].id);
	else
		rt_snprintf(n->descr, sizeof n->descr, ".ENET refnum %d", (int)c->ref);
	rt_lock();
	int res = rt_netif_add(n);
	rt_unlock();
	if (res < 0) { why("rt_netif_add failed"); return res; }
	c->open = 1;
	c->link_known = idx >= 0 && has_word(found[idx].drvr, "PiStorm") ? 0 : 2;    /* who answers csLinkStatus */
	static const u16 types[3] = { 0x0800, 0x0806, 0x86dd };
	for (int i = 0; i < 3; i++)
		if (enet_ctl(c, ENET_ATTACHPH, types[i], (void *)handlers[k], 0)) {
			why(".ENET EAttachPH failed: another stack (MacTCP?) has the protocol");
			return -RT_EBUSY;
		}
	if (!mac_nif)
		mac_nif = n;
	rt_log(RT_LOG_INFO, "%s: %02x:%02x:%02x:%02x:%02x:%02x on %s (refnum %d, metric %u)", n->name, n->hwaddr[0], n->hwaddr[1],
	       n->hwaddr[2], n->hwaddr[3], n->hwaddr[4], n->hwaddr[5], n->descr, (int)c->ref, (unsigned)n->metric);
	return 0;
}

/* the card's link, from drivers that can tell (csLinkStatus), once a second, asynchronously */
static void link_poll(struct card *c)
{
	u8 *pb = c->link_pb;
	if (c->link_asked) {
		if (PB_RESULT(pb) > 0)
			return;                               /* not answered yet */
		c->link_asked = 0;
		if (PB_RESULT(pb) == 0 && PB_L(pb, 32) == 0x4136384Cu) {       /* 'A68L': a driver that knows */
			int up = PB_W(pb, 28) != 0;
			if (!c->link_known || up != c->link_up) {
				rt_log(RT_LOG_INFO, "%s: link %s%s", c->nif.name, up ? "up" : "down", up ? "" : " (the next route takes over)");
				rt_lock();
				rt_netif_link_change(&c->nif, up);
				rt_unlock();
			}
			c->link_known = 1;
			c->link_up = (u8)up;
		} else
			c->link_known = 2;                    /* this driver has no such call: always up */
	}
	if (c->link_known == 2 || mac_ticks() - c->link_tick < 60)
		return;
	c->link_tick = mac_ticks();
	memset(pb, 0, sizeof c->link_pb);
	PB_REFNUM(pb) = c->ref;
	PB_CSCODE(pb) = ENET_LINKSTATUS;
	if (mac_pbcontrol(pb, 1) >= 0)
		c->link_asked = 1;
}

/* the interface MacTCP's calls report (ipctlGetAddr, the MTU): the running one with an address and
 * the lowest metric, which is the one the default route goes through */
static void pick_primary(void)
{
	struct rt_netif *best = NULL;
	for (int k = 0; k < MAC_CARDS; k++) {
		struct rt_netif *n = &cards[k].nif;
		if (!cards[k].open)
			continue;
		if (!best)
			best = n;
		int run = (n->flags & RT_IFF_RUNNING) && n->addr, brun = (best->flags & RT_IFF_RUNNING) && best->addr;
		if ((run && !brun) || (run == brun && n->metric < best->metric))
			best = n;
	}
	if (best && best != mac_nif) {
		if (mac_nif)
			rt_log(RT_LOG_INFO, "MacTCP's address is now %s's", best->name);
		mac_nif = best;
	}
}

void net_mac_poll(void)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	for (int k = 0; k < MAC_CARDS; k++) {
		struct card *c = &cards[k];
		struct mac_rxring *r = &mac_rx[k];
		if (!c->open)
			continue;
		while (r->tail != r->head) {
			u8 *s = r->ring + (u32)(r->tail & r->mask) * MAC_RX_SLOT;
			u16 len = *(u16 *)s;
			u8 *f = s + 2;
			if (len >= ETH_HLEN) {
				struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, len - ETH_HLEN);
				if (b) {
					u16 flags = 0;
					memcpy(b->data, f + ETH_HLEN, len - ETH_HLEN);
					b->len = len - ETH_HLEN;
					if (!memcmp(f, bcast, 6)) flags = RT_BUFF_BCAST;
					else if (f[0] & 1) flags = RT_BUFF_MCAST;
					rt_rng_add(rt_os_jitter());
					rt_lock();
					rt_netif_input(&c->nif, b, rt_get_be16(f + 12), f + 6, flags);
					rt_unlock();
				}
			}
			r->tail++;
		}
		link_poll(c);
	}
	pick_primary();
}
