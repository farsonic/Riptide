/*
 * Riptide - backend for netdev network drivers.
 *
 *   interface NAME device=genet.device [unit=N] [mtu=N]     (found by itself)
 *   interface NAME netdev=genet.device [unit=N] [mtu=N]     (forced)
 *
 * netdev is the NIC interface of lwip-amiga and the Emu68 driver stack
 * (devices/netdev.h, ABI version 1): instead of an IORequest per packet
 * the stack attaches once (NETDEV_CMD_ATTACH) and the two sides then call
 * each other directly. Emu68's genet.device 4.x (the Raspberry Pi 4's
 * Ethernet) speaks only netdev; its 3.x version is SANA-II.
 *
 * Receive: the driver hands batches of frames to nd_rx_input() on its unit
 * task. Each frame is copied into an rt_buf from a pool, summed in the same
 * pass (as the SANA-II path does), and the driver's buffer goes straight
 * back (ndo_RxRelease), so the stack holds none of them. The callback only
 * moves buffers between lists under Forbid() and Signal()s the Riptide
 * process, which feeds the stack under the lock (nd_poll).
 *
 * Calls go both ways with stack arguments; results come back in d0 (no
 * pointer result is declared as one: see ndo_DmaAlloc).
 *
 * Transmit: frames are copied, Ethernet header included, into transmit
 * buffers from the driver's DMA allocator (ndo_DmaAlloc), submitted with
 * ndo_TxSubmit and published with ndo_TxKick. The driver reports them done
 * (nso_TxDone) when it next services the device, so there are as many
 * buffers as it can have in flight.
 *
 * The structures below follow the published ABI (BSD-3-Clause header of
 * the Emu68 driver stack); the size checks are the ones it freezes.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "rt/rt_ip.h"

#define ETH_HLEN 14

/* ---- the netdev ABI, version 1 ---- */

#define NETDEV_ABI_VERSION       1
#define NETDEV_CMD_ATTACH        0x8900
#define NETDEV_CMD_DETACH        0x8901
#define NETDEV_CMD_START         0x8902
#define NETDEV_CMD_STOP          0x8903
#define NETDEV_CMD_SET_RXFILTER  0x8904
#define NETDEV_CMD_GET_STATS     0x8906
#define NETDEV_CMD_GET_LINK      0x8907

#define NDCF_LINK_EVENTS  (1UL << 5)
#define NDRF_BCAST        (1 << 2)
#define NDRF_MCAST        (1 << 3)
#define NDFF_ALLMULTI     (1 << 1)
#define NDLF_UP           (1 << 0)
#define NDLF_FULL_DUPLEX  (1 << 1)

#if defined(__GNUC__)
#pragma pack(2)
#endif

struct NetDevCaps {
	UWORD ndc_AbiVersion;
	UWORD ndc_Mtu;
	UBYTE ndc_Mac[6];
	UWORD ndc_TxMaxSegs;
	ULONG ndc_Features;
	UWORD ndc_TxRingSlots;
	UWORD ndc_RxRingSlots;
	UWORD ndc_TxAlign;
	UWORD ndc_TxInFlightMax;
	ULONG ndc_RxPoolBufs;
	ULONG ndc_Reserved[3];
};

struct NetDevSg {
	APTR  nsg_Data;
	ULONG nsg_Len;
};

struct NetDevTxDesc {
	const struct NetDevSg *ntd_Segs;
	UWORD ntd_NumSegs;
	UWORD ntd_Flags;
	UWORD ntd_CsumStart;
	UWORD ntd_CsumOffset;
	UWORD ntd_VlanTci;
	APTR  ntd_Cookie;
};

struct NetDevRxDesc {
	APTR  nrd_Data;
	ULONG nrd_Len;
	UWORD nrd_Flags;
	UWORD nrd_CsumRaw;
	UWORD nrd_VlanTci;
	APTR  nrd_Cookie;
};

struct NetDevLinkState {
	UWORD ndls_Flags;
	UWORD ndls_SpeedMbps;
};

/* plain C, stack arguments, in both directions */
struct NetDevDrvOps {
	LONG (*ndo_TxSubmit)(APTR drvctx, const struct NetDevTxDesc *descs, ULONG count);
	void (*ndo_TxKick)(APTR drvctx);
	void (*ndo_RxRelease)(APTR drvctx, APTR cookie);
	/* returns an APTR, in d0 as every AmigaOS compiler does: declared
	 * ULONG because the gcc we build with (m68k-linux-gnu, SVR4) takes a
	 * pointer result from a0 */
	ULONG (*ndo_DmaAlloc)(APTR drvctx, ULONG size, ULONG align);
	void (*ndo_DmaFree)(APTR drvctx, APTR ptr, ULONG size);
};

struct NetDevStackOps {
	ULONG (*nso_RxInput)(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count);
	void  (*nso_TxDone)(APTR stackctx, APTR const *cookies, ULONG count);
	void  (*nso_LinkChange)(APTR stackctx, const struct NetDevLinkState *state);
};

struct NetDevAttach {
	UWORD nda_AbiVersion;
	UWORD nda_RxHoldReq;
	UWORD nda_MtuReq;
	UWORD nda_RxBatch;
	APTR  nda_StackCtx;
	const struct NetDevStackOps *nda_StackOps;
	APTR  nda_DrvCtx;
	const struct NetDevDrvOps *nda_DrvOps;
	struct NetDevCaps nda_Caps;
};

struct NetDevRxFilter {
	UWORD ndrx_Flags;
	UWORD ndrx_NumMcast;
	const UBYTE (*ndrx_McastList)[6];
};

struct NetDevU64 {
	ULONG ndu_Hi, ndu_Lo;
};

struct NetDevStats {
	struct NetDevU64 nds_RxPackets, nds_RxBytes, nds_RxErrors, nds_RxDropped;
	struct NetDevU64 nds_TxPackets, nds_TxBytes, nds_TxErrors, nds_TxDropped;
	ULONG nds_RxOverruns;
	ULONG nds_RxFifoOvfl;
	ULONG nds_Reserved[8];
};

#if defined(__GNUC__)
#pragma pack()
#endif

typedef char nd_abi_check[(sizeof(struct NetDevSg) == 8 && sizeof(struct NetDevTxDesc) == 18 &&
                           sizeof(struct NetDevRxDesc) == 18 && sizeof(struct NetDevCaps) == 40 &&
                           sizeof(struct NetDevAttach) == 64 && sizeof(struct NetDevRxFilter) == 8 &&
                           sizeof(struct NetDevLinkState) == 4 && sizeof(struct NetDevStats) == 104) ? 1 : -1];

/* ---- the interface ---- */

#define ND_RX_POOL     128           /* receive buffers (the driver's own ring is in front) */
#define ND_RX_HEADROOM 64
#define ND_TX_MIN      32
#define ND_TX_MAX      256
#define ND_TX_DEFAULT  128           /* when the driver does not say (ndc_TxInFlightMax 0) */
#define ND_MC          32            /* multicast addresses in the filter */

struct nd_tx {
	struct nd_tx *next;
	UBYTE *mem;                   /* from ndo_DmaAlloc */
};

struct ndif {
	struct amiga_if ai;           /* must be first */
	struct MsgPort *port;
	struct IOStdReq *io;
	int    dev_open, attached, started;
	char   device[96];
	ULONG  unit;
	APTR   drvctx;
	const struct NetDevDrvOps *ops;
	struct NetDevCaps caps;
	struct NetDevStackOps sops;   /* must outlive the attachment */
	/* shared with the driver's callbacks: Forbid() */
	struct rt_buf *rx_free;       /* empty buffers (->next) */
	struct rt_buf *rx_head, *rx_tail;   /* received frames */
	struct nd_tx *tx_done;        /* completed transmit buffers */
	ULONG  rx_nfree;
	ULONG  overruns;              /* frames refused: no buffer */
	ULONG  toolong;               /* frames longer than a buffer */
	struct NetDevLinkState link;
	int    link_new;
	/* stack side (lock held) */
	ULONG  rx_pool;               /* buffers in circulation */
	ULONG  rx_size;               /* bytes per receive buffer */
	struct nd_tx *tx;             /* all transmit buffers */
	struct nd_tx *tx_free;
	ULONG  ntx, tx_size, tx_align;
	ULONG  overruns0;
	struct rt_mc_ref mcref[ND_MC];
	int    nmcref;
	int    allmulti;
	int    filter_due;            /* the filter changed: nd_poll sends it */
	int    tx_keep;               /* STOP failed: the driver may still use its buffers */
	int    want_online;
	struct rt_timer linkt;        /* asks the driver for its link (nd_link_timer) */
	int    rx_seen;               /* frames since the last link poll */
	struct rt_drv_stats drv;
};

static BYTE nd_sigbit = -1;
static void nd_set_filter(struct ndif *ni);

void netdev_cleanup(void)
{
	if (nd_sigbit >= 0)
		FreeSignal(nd_sigbit);
	nd_sigbit = -1;
}

/* a control op: 0 or the io_Error */
static LONG nd_ctl(struct ndif *ni, UWORD cmd, APTR data, ULONG len)
{
	ni->io->io_Command = cmd;
	ni->io->io_Flags = 0;
	ni->io->io_Data = data;
	ni->io->io_Length = len;
	ni->io->io_Actual = 0;
	return DoIO((struct IORequest *)ni->io);
}

/* ---- driver -> stack: on the driver's unit task, must not block ---- */

static ULONG nd_rx_input(APTR ctx, const struct NetDevRxDesc *d, ULONG count)
{
	struct ndif *ni = ctx;
	struct rt_buf *b;
	ULONG i;

	for (i = 0; i < count; i++, d++) {
		Forbid();
		b = ni->rx_free;
		if (b) {
			ni->rx_free = b->next;
			ni->rx_nfree--;
		}
		Permit();
		if (!b) {
			ni->overruns++;
			break;             /* backpressure: the driver recycles the rest */
		}
		if (d->nrd_Len < ETH_HLEN || d->nrd_Len > rt_buf_tailroom(b)) {
			/* consumed and dropped */
			ni->toolong++;
			ni->ops->ndo_RxRelease(ni->drvctx, d->nrd_Cookie);
			Forbid();
			b->next = ni->rx_free;
			ni->rx_free = b;
			ni->rx_nfree++;
			Permit();
			continue;
		}
		/* one pass copies and sums the frame; the driver's buffer goes back */
		b->csum = rt_csum_copy(b->data, d->nrd_Data, d->nrd_Len, 0);
		b->len = d->nrd_Len;
		b->flags = RT_BUFF_CSUM_PART | ((d->nrd_Flags & NDRF_BCAST) ? RT_BUFF_BCAST : 0) |
		           ((d->nrd_Flags & NDRF_MCAST) ? RT_BUFF_MCAST : 0);
		ni->ops->ndo_RxRelease(ni->drvctx, d->nrd_Cookie);
		b->next = NULL;
		Forbid();
		if (ni->rx_tail)
			ni->rx_tail->next = b;
		else
			ni->rx_head = b;
		ni->rx_tail = b;
		Permit();
	}
	if (i)
		Signal(amiga_stack_task, 1UL << nd_sigbit);
	return i;
}

static void nd_tx_done(APTR ctx, APTR const *cookies, ULONG count)
{
	struct ndif *ni = ctx;
	struct nd_tx *t;
	ULONG i;

	Forbid();
	for (i = 0; i < count; i++) {
		t = cookies[i];
		t->next = ni->tx_done;
		ni->tx_done = t;
	}
	Permit();
	Signal(amiga_stack_task, 1UL << nd_sigbit);
}

static void nd_link_change(APTR ctx, const struct NetDevLinkState *state)
{
	struct ndif *ni = ctx;

	Forbid();
	ni->link = *state;
	ni->link_new = 1;
	Permit();
	Signal(amiga_stack_task, 1UL << nd_sigbit);
}

/* ---- stack side (lock held) ---- */

static void nd_link_apply(struct ndif *ni)
{
	struct rt_netif *nif = &ni->ai.nif;
	int up = (ni->link.ndls_Flags & NDLF_UP) != 0;

	if (up == ni->ai.online)
		return;
	ni->ai.online = up;
	if (!nif->index) {
		/* while opening: the state rt_netif_up() starts from */
		nif->link_down = (u8)!up;
		return;
	}
	if (up)
		rt_log(RT_LOG_INFO, "%s: link up, %u Mb/s%s", nif->name, (unsigned)ni->link.ndls_SpeedMbps,
		       (ni->link.ndls_Flags & NDLF_FULL_DUPLEX) ? " full duplex" : "");
	else
		rt_log(RT_LOG_INFO, "%s: link down", nif->name);
	/* RUNNING, and on the way up DHCP and IPv6 look at the network again */
	rt_netif_link_change(nif, up);
}

/* The link from NETDEV_CMD_GET_LINK. A PHY is rarely up the moment the
 * driver starts (autonegotiation takes a few seconds), and not every
 * driver reports changes (NDCF_LINK_EVENTS): ask every second while the
 * link is down, every 5 s while it is up. Lock held. */
static void nd_link_timer(void *arg)
{
	struct ndif *ni = arg;
	struct NetDevLinkState ls;

	if (!ni->started)
		return;
	memset(&ls, 0, sizeof(ls));
	if (nd_ctl(ni, NETDEV_CMD_GET_LINK, &ls, sizeof(ls)) == 0) {
		/* frames came in meanwhile: the link is there */
		if (ni->rx_seen)
			ls.ndls_Flags |= NDLF_UP;
		Forbid();
		ni->link = ls;
		Permit();
		nd_link_apply(ni);
	}
	ni->rx_seen = 0;
	rt_timer_after(&ni->linkt, ni->ai.online ? 5000 : 1000);
}

/* refill the receive pool to ND_RX_POOL buffers */
static void nd_refill(struct ndif *ni)
{
	struct rt_buf *b;

	while (ni->rx_pool < ND_RX_POOL) {
		b = rt_buf_alloc(ND_RX_HEADROOM, ni->rx_size);
		if (!b) {
			rt_drv_rx_nobuf(&ni->ai.nif);
			break;
		}
		ni->rx_pool++;
		Forbid();
		b->next = ni->rx_free;
		ni->rx_free = b;
		ni->rx_nfree++;
		Permit();
	}
}

static void nd_poll(struct amiga_if *ai)
{
	struct ndif *ni = (struct ndif *)ai;
	struct rt_netif *nif = &ai->nif;
	struct rt_buf *b, *list;
	struct nd_tx *t, *done;
	u32 nrx = 0;
	ULONG nfree;

	Forbid();
	list = ni->rx_head;
	ni->rx_head = ni->rx_tail = NULL;
	done = ni->tx_done;
	ni->tx_done = NULL;
	nfree = ni->rx_nfree;
	Permit();
	if (list)
		rt_drv_rx_avail(nif, RT_DRV_RX_IP, nfree);
	ni->drv.rxq[RT_DRV_RX_IP].queued = nfree;
	if (list)
		rt_rx_batch_begin();
	while ((b = list) != NULL) {
		u8 src[6];
		u16 type;
		list = b->next;
		b->next = NULL;
		ni->rx_pool--;
		nrx++;
		rt_drv_rx_done(nif, RT_DRV_RX_IP, b->len, 0);
		memcpy(src, b->data + 6, 6);
		type = rt_get_be16(b->data + 12);
		/* the sum covered the Ethernet header too: take it out */
		b->csum = rt_csum_add(b->csum, rt_csum_fold(rt_csum_partial(b->data, ETH_HLEN, 0)));
		rt_buf_pull(b, ETH_HLEN);
		b->netif = nif;
		rt_netif_input(nif, b, type, src, b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST));
	}
	if (nrx) {
		rt_rx_batch_end();
		rt_drv_batch(nif, nrx);
		/* frames arrive: the link is there, whatever the driver said */
		ni->rx_seen = 1;
		if (!ai->online) {
			Forbid();
			ni->link.ndls_Flags |= NDLF_UP;
			Permit();
			nd_link_apply(ni);
		}
	}
	nd_refill(ni);
	while ((t = done) != NULL) {
		done = t->next;
		t->next = ni->tx_free;
		ni->tx_free = t;
		/* may send the next queued frame (QoS) */
		rt_drv_tx_done(nif, 0);
	}
	if (ni->link_new) {
		Forbid();
		ni->link_new = 0;
		Permit();
		nd_link_apply(ni);
	}
	if (ni->filter_due) {
		ni->filter_due = 0;
		nd_set_filter(ni);
	}
	if (ni->want_online >= 0) {
		int want = ni->want_online;
		ni->want_online = -1;
		if (want && !ni->started && nd_ctl(ni, NETDEV_CMD_START, NULL, 0) == 0) {
			ni->started = 1;
		} else if (!want && ni->started && nd_ctl(ni, NETDEV_CMD_STOP, NULL, 0) == 0) {
			ni->started = 0;
			ai->online = 0;
			rt_netif_link_change(nif, 0);
		}
	}
}

static void nd_set_online(struct amiga_if *ai, int online)
{
	((struct ndif *)ai)->want_online = online;
	Signal(amiga_stack_task, 1UL << nd_sigbit);
}

/* the driver's filter is declarative: the whole list each time */
static void nd_set_filter(struct ndif *ni)
{
	UBYTE list[ND_MC][6];
	struct NetDevRxFilter f;
	LONG e;
	int i;

	for (i = 0; i < ni->nmcref; i++)
		memcpy(list[i], ni->mcref[i].hw, 6);
	f.ndrx_Flags = ni->allmulti ? NDFF_ALLMULTI : 0;
	f.ndrx_NumMcast = (UWORD)ni->nmcref;
	f.ndrx_McastList = (const UBYTE (*)[6])list;
	e = nd_ctl(ni, NETDEV_CMD_SET_RXFILTER, &f, sizeof(f));
	if (e)
		rt_log(RT_LOG_WARN, "%s: netdev receive filter: error %ld", ni->ai.nif.name, (long)e);
}

static int nd_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct ndif *ni = nif->driver;

	if (!ni->attached)
		return -RT_ENETDOWN;
	if (!rt_mc_ref(ni->mcref, ND_MC, &ni->nmcref, hw, join))
		return 0;
	/* a full table: receive every group */
	if (join && ni->nmcref >= ND_MC && !ni->allmulti)
		ni->allmulti = 1;
	/* any task may join a group (setsockopt), but the request's reply port
	 * is the Riptide process's: the stack process sends it (nd_poll) */
	ni->filter_due = 1;
	Signal(amiga_stack_task, 1UL << nd_sigbit);
	return 0;
}

static int nd_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct ndif *ni = nif->driver;
	struct nd_tx *t = ni->tx_free;
	struct NetDevSg sg;
	struct NetDevTxDesc d;
	u32 len = b->len + ETH_HLEN;

	if (!t || !ni->started || len > ni->tx_size) {
		nif->stats.oerrors++;
		if (!t) {
			nif->stats.iqdrops++;
			rt_drv_tx_full(nif);
		}
		rt_buf_free(b);
		return !t ? -RT_ENOBUFS : !ni->started ? -RT_ENETDOWN : -RT_EMSGSIZE;
	}
	if (dsthw)
		memcpy(t->mem, dsthw, 6);
	else
		memset(t->mem, 0xff, 6);
	memcpy(t->mem + 6, nif->hwaddr, 6);
	rt_put_be16(t->mem + 12, ethertype);
	rt_copy(t->mem + ETH_HLEN, b->data, b->len);
	if (len < 60) {              /* the minimum Ethernet frame, FCS aside */
		memset(t->mem + len, 0, 60 - len);
		len = 60;
	}
	rt_buf_free(b);
	sg.nsg_Data = t->mem;
	sg.nsg_Len = len;
	memset(&d, 0, sizeof(d));
	d.ntd_Segs = &sg;
	d.ntd_NumSegs = 1;
	d.ntd_Cookie = t;
	/* The buffer leaves the free list and the frame is counted BEFORE the
	 * driver sees it: a driver whose unit task runs above the sender may
	 * complete it (nso_TxDone, which reuses t->next for the done list)
	 * before ndo_TxSubmit even returns. Unlinking afterwards crossed the
	 * free and done lists, and nd_poll could then loop for ever with the
	 * stack lock held (alice68net.device, unit task at priority 5). */
	ni->tx_free = t->next;
	t->next = NULL;
	rt_drv_tx_start(nif, len);
	RT_PROF_ENTER(RT_PROF_DRV_TX);
	if (ni->ops->ndo_TxSubmit(ni->drvctx, &d, 1) != 1) {
		RT_PROF_EXIT(RT_PROF_DRV_TX);
		/* the ring is full: the frame is lost, the buffer stays ours */
		t->next = ni->tx_free;
		ni->tx_free = t;
		rt_drv_tx_done(nif, -1);
		nif->stats.oerrors++;
		nif->stats.iqdrops++;
		rt_drv_tx_full(nif);
		return -RT_ENOBUFS;
	}
	ni->ops->ndo_TxKick(ni->drvctx);
	RT_PROF_EXIT(RT_PROF_DRV_TX);
	return 0;
}

static void nd_drv_info(struct rt_netif *nif, struct rt_ctl_driver *r)
{
	struct ndif *ni = nif->driver;

	rt_strlcpy(r->kind, "netdev", sizeof(r->kind));
	rt_strlcpy(r->driver, ni->device, sizeof(r->driver));
	r->unit = ni->unit;
	r->flags |= RT_CTL_DRVF_DROPS;
	if (ni->caps.ndc_Features & NDCF_LINK_EVENTS)
		r->flags |= RT_CTL_DRVF_LINKEV;
	r->drv_drops = ni->overruns - ni->overruns0;
	r->dq_mtu = ni->caps.ndc_Mtu;
	r->dq_bps = (u32)ni->link.ndls_SpeedMbps * 1000000u;
}

static void nd_drv_reset(struct rt_netif *nif)
{
	struct ndif *ni = nif->driver;

	ni->overruns0 = ni->overruns;
}

static int nd_status(struct amiga_if *ai, char *buf, u32 len)
{
	struct ndif *ni = (struct ndif *)ai;
	char link[32];

	if (ni->link.ndls_Flags & NDLF_UP)
		rt_snprintf(link, sizeof(link), "link %u Mb/s%s", (unsigned)ni->link.ndls_SpeedMbps,
		            (ni->link.ndls_Flags & NDLF_FULL_DUPLEX) ? " full duplex" : "");
	else
		rt_strlcpy(link, "no link", sizeof(link));
	return rt_snprintf(buf, len, "netdev %s unit %lu (ABI %u), %s, %lu transmit buffers, %lu overruns",
	                   ni->device, (unsigned long)ni->unit, (unsigned)ni->caps.ndc_AbiVersion, link,
	                   (unsigned long)ni->ntx, (unsigned long)ni->overruns);
}

static void nd_close(struct amiga_if *ai)
{
	struct ndif *ni = (struct ndif *)ai;
	struct rt_buf *b;
	ULONG i;

	rt_lock();
	rt_timer_del(&ni->linkt);
	if (ni->ai.nif.index)
		rt_netif_remove(&ni->ai.nif);
	rt_unlock();
	/* STOP completes every transmit; a driver that cannot stop may still
	 * read the transmit buffers, so they and the attachment stay */
	if (ni->started && nd_ctl(ni, NETDEV_CMD_STOP, NULL, 0) != 0) {
		/* the driver may still call us and use our buffers: everything it
		 * can reach stays allocated (a leak, not a crash) */
		rt_log(RT_LOG_WARN, "%s: netdev stop failed, the driver stays attached", ni->ai.nif.name);
		ni->tx_keep = 1;
		return;
	}
	ni->started = 0;
	if (ni->tx && !ni->tx_keep) {
		for (i = 0; i < ni->ntx; i++)
			if (ni->tx[i].mem)
				ni->ops->ndo_DmaFree(ni->drvctx, ni->tx[i].mem, ni->tx_size);
	}
	if (ni->attached && !ni->tx_keep)
		nd_ctl(ni, NETDEV_CMD_DETACH, NULL, 0);
	ni->attached = 0;
	if (ni->dev_open)
		CloseDevice((struct IORequest *)ni->io);
	if (ni->io)
		DeleteIORequest((struct IORequest *)ni->io);
	if (ni->port)
		DeleteMsgPort(ni->port);
	rt_lock();
	while ((b = ni->rx_free) != NULL) {
		ni->rx_free = b->next;
		rt_buf_free(b);
	}
	while ((b = ni->rx_head) != NULL) {
		ni->rx_head = b->next;
		rt_buf_free(b);
	}
	rt_unlock();
	rt_free(ni->tx);
	rt_free(ni);
}

int netdev_open_if(const char *name, const char *device, ULONG unit, int mtu, char *err, u32 errlen)
{
	struct ndif *ni;
	struct rt_netif *nif;
	struct NetDevAttach a;
	struct NetDevLinkState ls;
	ULONG i;
	LONG e;

	if (nd_sigbit < 0) {
		nd_sigbit = AllocSignal(-1);
		if (nd_sigbit < 0)
			return -RT_EAGAIN;
	}
	ni = rt_zalloc(sizeof(*ni));
	if (!ni)
		return -RT_ENOMEM;
	ni->want_online = -1;
	ni->unit = unit;
	rt_strlcpy(ni->device, device, sizeof(ni->device));
	nif = &ni->ai.nif;
	rt_strlcpy(nif->name, name, sizeof(nif->name));
	rt_snprintf(nif->descr, sizeof(nif->descr), "%s unit %lu", amiga_basename(device), (unsigned long)unit);
	ni->ai.poll = nd_poll;
	ni->ai.set_online = nd_set_online;
	ni->ai.close = nd_close;
	ni->ai.status = nd_status;
	ni->ai.drv = ni;
	ni->ai.sigmask = 1UL << nd_sigbit;
	ni->sops.nso_RxInput = nd_rx_input;
	ni->sops.nso_TxDone = nd_tx_done;
	ni->sops.nso_LinkChange = nd_link_change;
	rt_timer_init(&ni->linkt, nd_link_timer, ni);

	ni->port = CreateMsgPort();
	ni->io = ni->port ? (struct IOStdReq *)CreateIORequest(ni->port, sizeof(struct IOStdReq)) : NULL;
	if (!ni->io) {
		nd_close(&ni->ai);
		return -RT_ENOMEM;
	}
	e = amiga_open_device(name, device, unit, (struct IORequest *)ni->io);
	if (e) {
		rt_snprintf(err, errlen, "cannot open %s unit %lu (error %ld)", device, (unsigned long)unit, (long)e);
		nd_close(&ni->ai);
		return -RT_ENXIO;
	}
	ni->dev_open = 1;

	memset(&a, 0, sizeof(a));
	a.nda_AbiVersion = NETDEV_ABI_VERSION;
	a.nda_RxHoldReq = 0;          /* every frame is copied and given back at once */
	a.nda_MtuReq = mtu > 0 ? (UWORD)mtu : 0;
	a.nda_StackCtx = ni;
	a.nda_StackOps = &ni->sops;
	e = nd_ctl(ni, NETDEV_CMD_ATTACH, &a, sizeof(a));
	if (e || !a.nda_DrvOps || !a.nda_DrvOps->ndo_TxSubmit || !a.nda_DrvOps->ndo_RxRelease) {
		rt_snprintf(err, errlen, "%s unit %lu: netdev attach failed (error %ld%s)", device,
		            (unsigned long)unit, (long)e,
		            e == 41 ? ": the unit is in use, e.g. by another stack or a SANA-II program" :
		            e == 40 ? ": no common netdev version" : "");
		nd_close(&ni->ai);
		return -RT_ENXIO;
	}
	ni->attached = 1;
	ni->drvctx = a.nda_DrvCtx;
	ni->ops = a.nda_DrvOps;
	ni->caps = a.nda_Caps;
	ni->caps.ndc_AbiVersion = a.nda_AbiVersion;

	nif->type = RT_IFT_ETHER;
	nif->hwlen = 6;
	memcpy(nif->hwaddr, ni->caps.ndc_Mac, 6);
	nif->mtu = (u16)(ni->caps.ndc_Mtu ? RT_MIN(ni->caps.ndc_Mtu, 9000u) : 1500);
	if (mtu > 0 && mtu < nif->mtu)
		nif->mtu = (u16)mtu;
	nif->flags = RT_IFF_BROADCAST | RT_IFF_SIMPLEX | RT_IFF_MULTICAST;
	nif->link_output = nd_output;
	nif->mcast = nd_mcast;
	nif->driver = ni;

	/* transmit buffers from the driver's DMA memory: a frame with a VLAN tag */
	ni->tx_size = ((u32)nif->mtu + ETH_HLEN + 4 + 3) & ~3u;
	if (ni->tx_size < 64)
		ni->tx_size = 64;
	ni->tx_align = ni->caps.ndc_TxAlign > 4 ? ni->caps.ndc_TxAlign : 4;
	ni->ntx = ni->caps.ndc_TxInFlightMax ? ni->caps.ndc_TxInFlightMax : ND_TX_DEFAULT;
	ni->ntx = RT_MAX(ND_TX_MIN, RT_MIN(ni->ntx, ND_TX_MAX));
	ni->tx = rt_zalloc(ni->ntx * sizeof(*ni->tx));
	if (!ni->tx) {
		nd_close(&ni->ai);
		return -RT_ENOMEM;
	}
	for (i = 0; i < ni->ntx; i++) {
		ni->tx[i].mem = (UBYTE *)(uintptr_t)ni->ops->ndo_DmaAlloc(ni->drvctx, ni->tx_size, ni->tx_align);
		if (!ni->tx[i].mem)
			break;
		ni->tx[i].next = ni->tx_free;
		ni->tx_free = &ni->tx[i];
	}
	if (i < ND_TX_MIN) {
		rt_snprintf(err, errlen, "%s: out of transmit memory (%lu buffers)", device, (unsigned long)i);
		nd_close(&ni->ai);
		return -RT_ENOMEM;
	}

	ni->rx_size = (u32)nif->mtu + ETH_HLEN + 4;
	ni->drv.info = nd_drv_info;
	ni->drv.reset = nd_drv_reset;
	ni->drv.rxq[RT_DRV_RX_IP].cfg = ND_RX_POOL;
	ni->drv.tx_slots = i;
	rt_drv_attach(nif, &ni->drv);
	rt_lock();
	nd_refill(ni);
	rt_unlock();

	e = nd_ctl(ni, NETDEV_CMD_START, NULL, 0);
	if (e) {
		rt_snprintf(err, errlen, "%s unit %lu: netdev start failed (error %ld)", device, (unsigned long)unit,
		            (long)e);
		nd_close(&ni->ai);
		return -RT_ENXIO;
	}
	ni->started = 1;
	/* the link now (a driver with link events reports changes itself) */
	memset(&ls, 0, sizeof(ls));
	if (nd_ctl(ni, NETDEV_CMD_GET_LINK, &ls, sizeof(ls)) != 0)
		ls.ndls_Flags = NDLF_UP;      /* cannot tell: assume it is there */
	Forbid();
	if (!ni->link_new)
		ni->link = ls;
	Permit();

	rt_lock();
	nd_link_apply(ni);
	rt_timer_after(&ni->linkt, ni->ai.online ? 5000 : 1000);
	i = (ULONG)rt_netif_add(nif);
	if ((LONG)i >= 0) {
		ni->ai.next = amiga_ifs;
		amiga_ifs = &ni->ai;
	}
	rt_unlock();
	if ((LONG)i < 0) {
		rt_snprintf(err, errlen, "cannot add interface %s", name);
		nd_close(&ni->ai);
		return (int)(LONG)i;
	}
	rt_log(RT_LOG_INFO, "%s: netdev %s unit %lu, ABI %u, mtu %u, %lu transmit buffers%s", name, device,
	       (unsigned long)unit, (unsigned)ni->caps.ndc_AbiVersion, (unsigned)nif->mtu, (unsigned long)ni->ntx,
	       (ni->caps.ndc_Features & NDCF_LINK_EVENTS) ? ", link events" : "");
	return 0;
}
