/* src/mac/net_mac.c - Riptide on the classic Mac OS: the Ethernet interface, through the .ENET
 * driver the way MacTCP's Ethernet link layer uses it (Inside Macintosh: Networking, "Ethernet
 * Driver"). The card is found with the Slot Manager (SGetTypeSRsrc: catNetwork, typeEtherNet)
 * and opened with OpenSlot, else ".ENET" is opened by name (a built-in Ethernet port).
 * Received frames: protocol handlers for IPv4, ARP and IPv6 (drvr.S, mac_enet_ph) copy each
 * frame into a ring the stack's deferred task drains here. Sent frames: copied into one of a few
 * transmit slots and handed to an asynchronous EWrite, whose completion frees the slot.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_buf.h"
#include "rt_netif.h"
#include "mac.h"

#define ENET_ATTACHPH 247
#define ENET_WRITE    249
#define ENET_GETINFO  252
#define E_PROTTYPE    28
#define E_POINTER     30
#define E_BUFFSIZE    34
#define ETH_HLEN      14

u8 *mac_rx_ring;
volatile u16 mac_rx_head, mac_rx_tail;
u16 mac_rx_slots = 32, mac_rx_mask = 31;
volatile u32 mac_rx_dropped;
struct rt_netif *mac_nif;
char mac_net_why[64];

static s16 enet_ref;
static struct rt_netif nif;
#define TX_SLOTS 8
struct txslot { u8 pb[64]; u16 wds[6]; u8 frame[1516]; volatile u8 busy; };
static struct txslot *tx;
static u32 tx_drops;

static void why(const char *s) { u32 i = 0; for (; s[i] && i < sizeof mac_net_why - 1; i++) mac_net_why[i] = s[i]; mac_net_why[i] = 0; rt_log(RT_LOG_WARN, "%s", s); }

static int enet_find_and_open(void)
{
	static const u8 name[] = "\x05.ENET";
	u8 spb[56], pb[64];
	memset(spb, 0, sizeof spb);
	PB_W(spb, 40) = 4;                        /* spCategory: catNetwork */
	PB_W(spb, 42) = 1;                        /* spCType: typeEtherNet */
	spb[48] = 3;                              /* spTBMask: any driver software and hardware */
	PB_L(spb, 24) = 1;                        /* spParamData: fAll */
	memset(pb, 0, sizeof pb);
	PB_NAME(pb) = (u8 *)name;
	if (mac_slotmanager(spb, 0x0C) == 0) {    /* SGetTypeSRsrc */
		pb[34] = spb[49];                     /* ioSlot */
		pb[35] = spb[50];                     /* ioID */
		if (mac_pbopen(pb, 1) == 0)
			return PB_REFNUM(pb);
	}
	memset(pb, 0, sizeof pb);
	PB_NAME(pb) = (u8 *)name;
	if (mac_pbopen(pb, 0) == 0)
		return PB_REFNUM(pb);
	return 0;
}
static int enet_ctl(int cs, u16 prot, void *ptr, u16 size)
{
	u8 pb[64];
	memset(pb, 0, sizeof pb);
	PB_REFNUM(pb) = enet_ref;
	PB_CSCODE(pb) = (s16)cs;
	PB_W(pb, E_PROTTYPE) = prot;
	PB_P(pb, E_POINTER) = ptr;
	PB_W(pb, E_BUFFSIZE) = size;
	return mac_pbcontrol(pb, 0);
}

static int mac_link_output(struct rt_netif *n, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct txslot *t = NULL;
	for (int i = 0; i < TX_SLOTS; i++) if (!tx[i].busy) { t = &tx[i]; break; }
	if (!t || b->len > 1500) { tx_drops++; rt_buf_free(b); return -RT_ENOBUFS; }
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
	PB_REFNUM(t->pb) = enet_ref;
	PB_CSCODE(t->pb) = ENET_WRITE;
	PB_P(t->pb, E_POINTER) = t->wds;
	PB_COMPLETION(t->pb) = (void *)mac_tx_done;
	if (mac_pbcontrol(t->pb, 1) < 0) { t->busy = 0; tx_drops++; return -RT_EIO; }
	return 0;
}
void net_mac_tx_done(void *pb)
{
	for (int i = 0; i < TX_SLOTS; i++) if (tx[i].pb == pb) { tx[i].busy = 0; return; }
}

int net_mac_open(void)
{
	u8 info[18];
	mac_rx_ring = mac_newptr_sys((u32)mac_rx_slots * MAC_RX_SLOT);
	tx = mac_newptr_sys(sizeof(struct txslot) * TX_SLOTS);
	if (!mac_rx_ring || !tx) { why("not enough memory for the Ethernet buffers"); return -RT_ENOMEM; }
	int ref = enet_find_and_open();
	if (!ref) { why("no Ethernet card (.ENET) found"); return -RT_ENODEV; }
	enet_ref = (s16)ref;
	memset(info, 0, sizeof info);
	if (enet_ctl(ENET_GETINFO, 0, info, sizeof info)) { why(".ENET EGetInfo failed"); return -RT_EIO; }
	nif.type = RT_IFT_ETHER;
	nif.hwlen = 6;
	memcpy(nif.hwaddr, info, 6);
	nif.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	nif.mtu = 1500;
	nif.link_output = mac_link_output;
	nif.driver = &enet_ref;
	rt_snprintf(nif.name, sizeof nif.name, "en0");
	rt_snprintf(nif.descr, sizeof nif.descr, ".ENET refnum %d", (int)enet_ref);
	rt_lock();
	int r = rt_netif_add(&nif);
	rt_unlock();
	if (r < 0) { why("rt_netif_add failed"); return r; }
	mac_nif = &nif;
	static const u16 types[3] = { 0x0800, 0x0806, 0x86dd };
	for (int i = 0; i < 3; i++)
		if (enet_ctl(ENET_ATTACHPH, types[i], (void *)mac_enet_ph, 0)) { why(".ENET EAttachPH failed: another stack (MacTCP?) has the protocol"); return -RT_EBUSY; }
	rt_log(RT_LOG_INFO, "en0: %02x:%02x:%02x:%02x:%02x:%02x on .ENET %d", nif.hwaddr[0], nif.hwaddr[1], nif.hwaddr[2], nif.hwaddr[3], nif.hwaddr[4], nif.hwaddr[5], (int)enet_ref);
	return 0;
}

void net_mac_poll(void)
{
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	while (mac_rx_tail != mac_rx_head) {
		u8 *s = mac_rx_ring + (u32)(mac_rx_tail & mac_rx_mask) * MAC_RX_SLOT;
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
				rt_netif_input(&nif, b, rt_get_be16(f + 12), f + 6, flags);
				rt_unlock();
			}
		}
		mac_rx_tail++;
	}
}
