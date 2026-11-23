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
 * The transmit slots are the driver's slots for the core (struct rt_drv_stats: tx_slots,
 * tx_inflight): a write is counted when it is handed to the driver (rt_drv_tx_start) and when its
 * completion has been seen (rt_drv_tx_done). The completion runs at interrupt time, so there it
 * only frees the slot, counts (tx_done_n, tx_err_n) and asks for the stack's deferred task; the
 * stack's poll (tx_reap) then tells the core, which sends what its QoS bands hold back while the
 * driver has qos_inflight frames (src/core/qos.c). The core's count can stay above the driver's for
 * as long as a program holds the core (os_mac.c, mac_core_enter): such frames wait in the bands.
 *
 * VLANs (src/core/vlan.c): the first VLAN interface on a card attaches the card's protocol handler
 * for type 0x8100 as well (vlan_rx, asynchronously), the last one gone detaches it; tagged frames
 * come in through the same ring and the core strips the tag (rt_vlan_input). A VLAN's frames go
 * out through mac_link_output with type 0x8100 and the tag in front of the payload. Whether a
 * card's .ENET driver passes tagged frames at all depends on the driver (docs/MAC.md).
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
#include "rt_ctl.h"
#include "mac.h"

#define ENET_DELMULTI 245
#define ENET_ADDMULTI 246
#define ENET_ATTACHPH 247
#define ENET_DETACHPH 248
#define ENET_WRITE    249
#define ENET_GETINFO  252
#define ENET_LINKSTATUS 230                       /* Aurora68's drivers' own (mac/enet.s) */
#define E_PROTTYPE    28
#define E_POINTER     30
#define E_BUFFSIZE    34
#define ETH_HLEN      14
#define ETH_P_8021Q   0x8100
#define VLAN_HLEN     4
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
#define MC_REFS 16
/* frame: the header and 1500 bytes, or 1504 with a VLAN tag */
/* counted: the write is the open card's, counted at the core (not one left from a closed card) */
struct txslot { u8 pb[64]; u16 wds[6]; u8 frame[ETH_HLEN + 1500 + VLAN_HLEN + 2]; volatile u8 busy, counted; };
/* an open card: an interface of the core */
static struct card {
	int open, index;                              /* index: the found[] entry; -1 the built-in .ENET */
	int stuck;                                    /* closed with a call still at the driver: not reused */
	s16 ref;
	struct rt_netif nif;
	struct txslot *tx;
	u8 mc_pb[MC_PBS][40];
	u8 mc_next;                                   /* the parameter block to use next, in turn */
	u8 link_pb[40];                               /* csLinkStatus, asked asynchronously */
	u8 link_known, link_up, link_asked;
	u32 link_tick, tx_drops;
	struct rt_drv_stats drv;                      /* the core's driver counters, QoS's tx_inflight */
	volatile u32 tx_done_n, tx_err_n;             /* completions seen at interrupt time, errors among them */
	volatile s16 tx_err_code;                     /* the last error's ioResult */
	u32 tx_reaped, tx_err_reaped;                 /* ... of which the core has been told */
	u32 rx_drop_base;                             /* mac_rx[].dropped at the last counter reset */
	struct rt_mc_ref mcref[MC_REFS];              /* link multicast groups, counted (VLANs share them) */
	int nmcref;
	u8 vlan_pb[40];                               /* EAttachPH / EDetachPH of type 0x8100 */
	u8 vlan_want, vlan_have, vlan_asked, vlan_failed;
} cards[MAC_CARDS];
static void (*const handlers[MAC_CARDS])(void) = { mac_enet_ph0, mac_enet_ph1, mac_enet_ph2, mac_enet_ph3 };
/* each card slot's transmit slots, kept when its card is closed (a restart): a write still with the
   driver then completes into memory that is still there, and frees its slot (net_mac_tx_done) */
static struct txslot *txmem[MAC_CARDS];
/* a restart runs in a program's context (net_mac_restart_begin): there a call may wait for the
   driver's earlier asynchronous ones to complete (interrupts on), which the deferred task must not */
static int may_wait;
static int wait_pb(const u8 *pb)
{
	u32 t0 = mac_ticks();
	while (PB_RESULT(pb) > 0 && may_wait && mac_ticks() - t0 < 120)
		;
	return PB_RESULT(pb) > 0;
}

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
	/* the first join and the last leave only: a VLAN joins the groups its parent has too */
	if (!rt_mc_ref(c->mcref, MC_REFS, &c->nmcref, hw, join))
		return 0;
	if (may_wait)
		wait_pb(c->mc_pb[c->mc_next]);            /* a restart leaves and joins many groups at once */
	for (int i = 0; i < MC_PBS; i++) {
		u8 *pb = c->mc_pb[(c->mc_next + i) % MC_PBS];
		if (PB_RESULT(pb) > 0)
			continue;                             /* still in progress */
		c->mc_next = (u8)((c->mc_next + i + 1) % MC_PBS);
		memset(pb, 0, sizeof c->mc_pb[i]);
		PB_REFNUM(pb) = c->ref;
		PB_CSCODE(pb) = join ? ENET_ADDMULTI : ENET_DELMULTI;
		memcpy(pb + 28, hw, 6);
		if (mac_pbcontrol(pb, 1) >= 0)
			return 0;
		break;
	}
	rt_mc_ref(c->mcref, MC_REFS, &c->nmcref, hw, !join);   /* not done: as it was */
	return -RT_EBUSY;
}

static int mac_link_output(struct rt_netif *n, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct card *c = n->driver;
	struct txslot *t = NULL;
	for (int i = 0; i < TX_SLOTS; i++) if (!c->tx[i].busy) { t = &c->tx[i]; break; }
	if (!t || b->len > (ethertype == ETH_P_8021Q ? 1500u + VLAN_HLEN : 1500u)) {
		if (!t)
			rt_drv_tx_full(n);
		c->tx_drops++;
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
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
	t->counted = 1;
	rt_drv_tx_start(n, len);
	int e = mac_pbcontrol(t->pb, 1);
	if (e < 0) {
		/* refused at once: counted as a completion with an error, unless the driver called the
		   completion routine for it already */
		u16 sr = mac_irq_off();
		if (t->busy) {
			t->busy = 0;
			t->counted = 0;
			c->tx_err_code = (s16)e;
			c->tx_err_n++;
			c->tx_done_n++;
		}
		mac_irq_restore(sr);
		c->tx_drops++;
		return -RT_EIO;
	}
	return 0;
}
/* an EWrite's completion, at interrupt time (drvr.S mac_tx_done): the slot is free; the core hears
   of it in the stack's poll (tx_reap) */
void net_mac_tx_done(void *pb)
{
	for (int k = 0; k < MAC_CARDS; k++)
		if (txmem[k])                             /* a closed card's slots too (a restart) */
			for (int i = 0; i < TX_SLOTS; i++) {
				struct txslot *t = &txmem[k][i];
				if (t->pb != pb)
					continue;
				if (t->busy) {
					s16 r = PB_RESULT(pb);
					t->busy = 0;
					if (!cards[k].open || !t->counted)
						return;                       /* nobody counts for a closed card */
					t->counted = 0;
					if (r) {
						cards[k].tx_err_code = r;
						cards[k].tx_err_n++;
					}
					cards[k].tx_done_n++;
					mac_schedule();
				}
				return;
			}
}

/* the stack's context: the completions seen since the last look, to the core (which may send
   what its QoS bands hold) */
static void tx_reap(struct card *c)
{
	u32 done = c->tx_done_n;
	if (done == c->tx_reaped)
		return;
	rt_lock();
	while (c->tx_reaped != done) {
		s32 err = 0;
		c->tx_reaped++;
		if (c->tx_err_reaped != c->tx_err_n) {
			c->tx_err_reaped++;
			err = c->tx_err_code ? c->tx_err_code : -1;
		}
		rt_drv_tx_done(&c->nif, err);
	}
	rt_unlock();
}

/* The handler for tagged frames (type 0x8100), attached while the card has VLAN interfaces: the
   same entry as the card's other types (its ring). Asynchronous, as the multicast calls are:
   the core asks from the stack's deferred task. */
static void vlan_ph_step(struct card *c)
{
	u8 *pb = c->vlan_pb;
	if (c->vlan_asked && may_wait)
		wait_pb(pb);                              /* a restart: VLANs go and come back at once */
	if (c->vlan_asked) {
		s16 r = PB_RESULT(pb);
		if (r > 0)
			return;                               /* still in progress */
		c->vlan_asked = 0;
		if (PB_CSCODE(pb) == ENET_ATTACHPH) {
			if (r == 0) {
				c->vlan_have = 1;
				rt_log(RT_LOG_INFO, "%s: receiving tagged frames (802.1Q)", c->nif.name);
			} else {
				c->vlan_failed = 1;
				rt_log(RT_LOG_WARN, "%s: .ENET EAttachPH for VLAN frames failed (%d): no VLAN frames", c->nif.name, (int)r);
			}
		} else
			c->vlan_have = 0;
	}
	if (c->vlan_want == c->vlan_have || (c->vlan_want && c->vlan_failed))
		return;
	memset(pb, 0, sizeof c->vlan_pb);
	PB_REFNUM(pb) = c->ref;
	PB_CSCODE(pb) = c->vlan_want ? ENET_ATTACHPH : ENET_DETACHPH;
	PB_W(pb, E_PROTTYPE) = ETH_P_8021Q;
	PB_P(pb, E_POINTER) = c->vlan_want ? (void *)handlers[c - cards] : NULL;
	int e = mac_pbcontrol(pb, 1);
	if (e >= 0) {
		c->vlan_asked = 1;                        /* the result is looked at in the next poll */
		return;
	}
	if (c->vlan_want) {
		c->vlan_failed = 1;
		rt_log(RT_LOG_WARN, "%s: .ENET EAttachPH for VLAN frames failed (%d): no VLAN frames", c->nif.name, e);
	} else
		c->vlan_have = 0;
}
static int mac_vlan_rx(struct rt_netif *n, int on)
{
	struct card *c = n->driver;
	c->vlan_want = (u8)(on != 0);
	if (on)
		c->vlan_failed = 0;                       /* a new first VLAN: try again */
	vlan_ph_step(c);
	return 0;
}

/* the driver's part of the core's driver record (ip -s link, the Counters page) */
static void mac_drv_info(struct rt_netif *n, struct rt_ctl_driver *r)
{
	struct card *c = n->driver;
	rt_snprintf(r->kind, sizeof r->kind, "enet");
	rt_snprintf(r->driver, sizeof r->driver, "%s", c->index >= 0 ? found[c->index].drvr : ".ENET");
	r->unit = c->index >= 0 ? (u32)c->index : 0;
	r->flags |= RT_CTL_DRVF_DROPS;
	r->drv_drops = mac_rx[c - cards].dropped - c->rx_drop_base;   /* the receive ring was full */
}
static void mac_drv_reset(struct rt_netif *n)
{
	struct card *c = n->driver;
	c->rx_drop_base = mac_rx[c - cards].dropped;
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
		if (!cards[k].open && !cards[k].stuck) { c = &cards[k]; break; }
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
	r->head = r->tail = 0;                        /* no handler fills it now (a card closed before) */
	if (!r->ring) {
		r->ring = mac_newptr_sys((u32)RX_SLOTS * MAC_RX_SLOT);
		r->slots = RX_SLOTS;
		r->mask = RX_SLOTS - 1;
	}
	if (!txmem[k]) {
		txmem[k] = mac_newptr_sys(sizeof(struct txslot) * TX_SLOTS);
		if (txmem[k])
			memset(txmem[k], 0, sizeof(struct txslot) * TX_SLOTS);
	}
	c->tx = txmem[k];
	if (!r->ring || !c->tx) { why("not enough memory for the Ethernet buffers"); return -RT_ENOMEM; }
	for (int i = 0; i < TX_SLOTS; i++)
		c->tx[i].counted = 0;                     /* a write of the card closed before is not this one's */
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
	n->vlan_rx = mac_vlan_rx;
	n->driver = c;
	n->metric = sel->metric >= 0 ? (u32)sel->metric : (idx >= 0 && found[idx].kind == KIND_WIFI) ? 20 : 10;
	rt_snprintf(n->name, sizeof n->name, "%s", name);
	if (idx >= 0)
		rt_snprintf(n->descr, sizeof n->descr, "%s slot %u id $%02x", found[idx].drvr, found[idx].slot, found[idx].id);
	else
		rt_snprintf(n->descr, sizeof n->descr, ".ENET refnum %d", (int)c->ref);
	c->drv.tx_slots = TX_SLOTS;
	c->drv.noqueue = 1;                           /* .ENET hands over every frame: no reads to count */
	c->drv.info = mac_drv_info;
	c->drv.reset = mac_drv_reset;
	c->rx_drop_base = r->dropped;
	rt_lock();
	int res = rt_netif_add(n);
	if (res >= 0)
		rt_drv_attach(n, &c->drv);                /* "qos on" by default: the bands, too */
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
		tx_reap(c);
		u32 got = 0;
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
					rt_drv_rx_done(&c->nif, -1, len, 0);
					rt_netif_input(&c->nif, b, rt_get_be16(f + 12), f + 6, flags);
					rt_unlock();
					got++;
				} else {
					rt_lock();
					rt_drv_rx_nobuf(&c->nif);
					rt_unlock();
				}
			}
			r->tail++;
		}
		if (got) {
			rt_lock();
			rt_drv_batch(&c->nif, got);
			rt_unlock();
		}
		if (c->vlan_asked || c->vlan_want != c->vlan_have)
			vlan_ph_step(c);
		link_poll(c);
	}
	pick_primary();
}

/* ---- a restart of the network while the Mac runs (main_mac.c mac_restart) ---------------------
 * It runs in a program's context (RiptidePrefs' synchronous RESTART call, or accRun), with the core
 * lent to it, so the driver calls here may be synchronous and may wait for the asynchronous ones
 * still at the driver. A card that stays (the same card under the same name) keeps its driver,
 * reference number, protocol handlers, receive ring and transmit slots: only the core forgets it
 * and takes it back (net_mac_renew). A card that goes is detached from its driver (net_mac_close);
 * its slot's memory stays, so nothing at interrupt time writes into freed memory. */
void net_mac_restart_begin(void) { may_wait = 1; }
void net_mac_restart_end(void) { may_wait = 0; }

/* the open cards' interfaces, one by one (i from 0; NULL past the last) */
struct rt_netif *net_mac_nif(int i)
{
	for (int k = 0; k < MAC_CARDS; k++)
		if (cards[k].open && i-- == 0)
			return &cards[k].nif;
	return NULL;
}

/* does the selection name this open card (as net_mac_open would choose it, the card counted free)? */
int net_mac_same(struct rt_netif *n, const struct mac_cardsel *sel)
{
	struct card *c = n->driver;
	int idx;
	c->open = 0;
	idx = select_card(sel);
	c->open = 1;
	return idx == c->index;
}

/* a card that stays: what the core set on it goes (addresses, IPv4 and IPv6 switches, window), as
   a card just opened has it; the driver's side, the counters and the link state stay. The core has
   removed it (rt_netif_remove) and adds it again after this; its index is kept (scope ids). */
void net_mac_renew(struct rt_netif *n, int mtu, int metric)
{
	struct card *c = n->driver;
	struct rt_netif k = *n;
	memset(n, 0, sizeof *n);
	n->type = k.type;
	n->hwlen = k.hwlen;
	memcpy(n->hwaddr, k.hwaddr, sizeof n->hwaddr);
	n->flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	n->mtu = mtu >= 576 && mtu < 1500 ? (u16)mtu : 1500;
	n->link_output = k.link_output;
	n->mcast = k.mcast;
	n->vlan_rx = k.vlan_rx;
	n->driver = c;
	n->stats = k.stats;
	n->drv = k.drv;
	n->link_down = k.link_down;
	n->index = k.index;
	n->metric = metric >= 0 ? (u32)metric : (c->index >= 0 && found[c->index].kind == KIND_WIFI) ? 20 : 10;
	memcpy(n->name, k.name, sizeof n->name);
	memcpy(n->descr, k.descr, sizeof n->descr);
	/* the transmit counts (drv: tx_inflight) are carried: the writes still at the driver complete
	   and are reaped as before. The multicast groups are counted again from the core's joins. */
	c->nmcref = 0;
}

/* a card that goes: its calls at the driver finish (two seconds at most), its protocol handlers are
   detached, and its slot is free; the core has removed it already */
void net_mac_close(struct rt_netif *n)
{
	struct card *c = n->driver;
	int k = (int)(c - cards), left = 0;
	static const u16 types[3] = { 0x0800, 0x0806, 0x86dd };
	for (int i = 0; i < 3; i++)
		if (enet_ctl(c, ENET_DETACHPH, types[i], NULL, 0)) {
			rt_log(RT_LOG_WARN, "%s: .ENET EDetachPH %04x failed: its slot is not used again", n->name, types[i]);
			left = 1;
		}
	/* the handler for tagged frames, if the last VLAN's detach has not happened yet */
	if (c->vlan_asked && wait_pb(c->vlan_pb))
		left = 1;
	else {
		if (c->vlan_asked) {
			c->vlan_asked = 0;
			if (PB_RESULT(c->vlan_pb) == 0)
				c->vlan_have = PB_CSCODE(c->vlan_pb) == ENET_ATTACHPH;
		}
		if (c->vlan_have && enet_ctl(c, ENET_DETACHPH, ETH_P_8021Q, NULL, 0)) {
			rt_log(RT_LOG_WARN, "%s: .ENET EDetachPH 8100 failed: its slot is not used again", n->name);
			left = 1;
		}
		c->vlan_have = c->vlan_want = 0;
	}
	for (int i = 0; i < MC_PBS; i++)
		left |= wait_pb(c->mc_pb[i]);
	if (c->link_asked)
		left |= wait_pb(c->link_pb);
	for (int i = 0; i < TX_SLOTS; i++) {
		u32 t0 = mac_ticks();
		while (c->tx[i].busy && mac_ticks() - t0 < 120)
			;
	}
	mac_rx[k].head = mac_rx[k].tail = 0;
	c->open = 0;
	c->stuck = left;
	if (left)
		rt_log(RT_LOG_WARN, "%s: a call is still at the driver: its slot is not used again", n->name);
	if (mac_nif == n) {
		mac_nif = NULL;
		pick_primary();
	}
}
