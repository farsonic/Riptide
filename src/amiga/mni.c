/*
 * Riptide - direct backend for MNI (.mni) network drivers.
 *
 *   interface NAME mni=z2-am7990.mni unit=N [options="FT=2 MEDIA=AUTO"] [mtu=N]
 *
 * The ABI was recovered from the historical stack and its drivers (docs/RE-NOTES.md
 * section 4): the driver is a library; Riptide opens a handle with three
 * callbacks (supply an rx buffer, rx done, tx done) that may run in the
 * driver's interrupt code. Receive is zero copy: the driver writes frames
 * straight into rt_bufs taken from a pre-filled pool. The callbacks only
 * move records between lists under Disable() and Signal() the Riptide
 * process, which feeds the stack under the lock (mni_poll). IPv6 is added
 * as a third packet type; multicast groups (IPv6, and IPv4 ones joined
 * with IGMP) go to the driver's hash
 * filter with AddMulticast/RemMulticast (-78/-84), called directly like
 * Send, under the stack lock.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/mni_calls.h"

#define MNI_API_VERSION 3
#define MNI_RX_POOL     32
#define MNI_TX_POOL     32
#define MNI_RX_HEADROOM 64
#define MNI_FLAG_BCAST  2

struct mni_frag {
	struct mni_frag *next;
	ULONG reserved;
	APTR  data;
	ULONG len;
};

/* Packet descriptor exchanged with the driver (0x50 bytes, as sanamni) */
struct mni_pkt {
	UBYTE  priv0[0x10];          /* driver queue links */
	struct mni_frag *frags;      /* 0x10 */
	UBYTE  addr[6];              /* 0x14 tx: destination, rx: source */
	UBYTE  pad1[0x24 - 0x1a];
	ULONG  flags;                /* 0x24 bit1: broadcast */
	ULONG  type;                 /* 0x28 ethertype */
	LONG   status;               /* 0x2c */
	ULONG  length;               /* 0x30 */
	APTR   userdata;             /* 0x34 */
	APTR   stackpriv;            /* 0x38 */
	APTR   owner;                /* 0x3c */
	UBYTE  srcaddr[6];           /* 0x40 */
	UBYTE  pad2[0x50 - 0x46];
};
typedef char mni_pkt_size_check[(sizeof(struct mni_pkt) == 0x50 &&
                                 offsetof(struct mni_pkt, flags) == 0x24 &&
                                 offsetof(struct mni_pkt, stackpriv) == 0x38) ? 1 : -1];

struct mni_open {
	ULONG  unit;
	ULONG  reserved;
	APTR   cb_getbuf;
	APTR   cb_rxdone;
	APTR   cb_txdone;
	APTR   userdata;
	STRPTR options;
	APTR   handle;
};

struct mni_rx {
	struct MinNode node;
	struct mni_frag frag;
	struct rt_buf *b;
	ULONG  len, type, flags;
	LONG   status;
	UBYTE  src[6];
};

struct mni_tx {
	struct mni_pkt pkt;          /* first: the driver hands it back */
	struct MinNode node;
	struct mni_frag frag;
	struct rt_buf *b;
	int    busy;
};

#define MNI_MCREF 32                 /* multicast addresses counted (rt_mc_ref) */

struct mniif {
	struct amiga_if ai;          /* must be first */
	struct Library *base;
	APTR   handle;
	ULONG  apiver;
	char   driver[96];
	char   options[96];
	ULONG  unit;
	int    want_online;
	/* lists shared with the callbacks: Disable() */
	struct MinList rx_free, rx_done, rx_nobuf, tx_done;
	struct mni_tx *tx_free;
	struct mni_rx rx[MNI_RX_POOL];
	struct mni_tx tx[MNI_TX_POOL];
	ULONG  overruns;
	struct rt_drv_stats drv;     /* net.riptide.drivers */
	ULONG  overruns0;            /* overruns at the last counter reset */
	struct rt_mc_ref mcref[MNI_MCREF];
	int    nmcref;
};

static BYTE mni_sigbit = -1;

static struct MinNode *remhead_min(struct MinList *l)
{
	struct MinNode *n = l->mlh_Head;

	if (!n->mln_Succ)
		return NULL;
	l->mlh_Head = n->mln_Succ;
	n->mln_Succ->mln_Pred = (struct MinNode *)&l->mlh_Head;
	return n;
}

static void addtail_min(struct MinList *l, struct MinNode *n)
{
	n->mln_Succ = (struct MinNode *)&l->mlh_Tail;
	n->mln_Pred = l->mlh_TailPred;
	l->mlh_TailPred->mln_Succ = n;
	l->mlh_TailPred = n;
}

/* ---- driver callbacks: plain C, one stack argument, maybe in an interrupt ---- */

static LONG mni_cb_getbuf(struct mni_pkt *p)
{
	struct mniif *mi = p->userdata;
	struct mni_rx *r;

	Disable();
	r = (struct mni_rx *)remhead_min(&mi->rx_free);
	Enable();
	if (!r) {
		mi->overruns++;
		return -3;
	}
	if (p->length > r->frag.len) {
		Disable();
		addtail_min(&mi->rx_free, &r->node);
		Enable();
		mi->overruns++;
		return -1;
	}
	p->frags = &r->frag;
	p->stackpriv = r;
	return 0;
}

static void mni_cb_rxdone(struct mni_pkt *p)
{
	struct mniif *mi = p->userdata;
	struct mni_rx *r = p->stackpriv;

	if (!r)
		return;
	p->stackpriv = NULL;
	r->len = p->length;
	r->type = p->type;
	r->flags = p->flags;
	r->status = p->status;
	memcpy(r->src, p->addr, 6);
	Disable();
	addtail_min(&mi->rx_done, &r->node);
	Enable();
	Signal(amiga_stack_task, 1UL << mni_sigbit);
}

static void mni_cb_txdone(struct mni_pkt *p)
{
	struct mniif *mi = p->userdata;
	struct mni_tx *t = (struct mni_tx *)p;

	Disable();
	addtail_min(&mi->tx_done, &t->node);
	Enable();
	Signal(amiga_stack_task, 1UL << mni_sigbit);
}

/* ---- stack side (lock held) ---- */

static int rx_attach_buf(struct mniif *mi, struct mni_rx *r)
{
	if (!r->b) {
		r->b = rt_buf_alloc(MNI_RX_HEADROOM, (u32)mi->ai.nif.mtu + 4);
		if (!r->b)
			return -1;
	}
	r->b->len = 0;
	r->frag.next = NULL;
	r->frag.data = r->b->data;
	r->frag.len = rt_buf_tailroom(r->b);
	return 0;
}

/* Receive buffers the driver can still take at this wakeup (one pool for
 * every packet type). Lock held. */
static void mni_sample_rx(struct mniif *mi)
{
	struct MinNode *n;
	u32 nfree = 0;
	int any;

	Disable();
	any = mi->rx_done.mlh_Head->mln_Succ != NULL;
	for (n = mi->rx_free.mlh_Head; n->mln_Succ; n = n->mln_Succ)
		nfree++;
	Enable();
	mi->drv.rxq[RT_DRV_RX_IP].queued = nfree;
	if (any)
		rt_drv_rx_avail(&mi->ai.nif, RT_DRV_RX_IP, nfree);
}

/* rt_drv_stats hooks: lock held */
static void mni_drv_info(struct rt_netif *nif, struct rt_ctl_driver *r)
{
	struct mniif *mi = nif->driver;

	rt_strlcpy(r->kind, "mni", sizeof(r->kind));
	rt_strlcpy(r->driver, mi->driver, sizeof(r->driver));
	r->unit = mi->unit;
	r->flags |= RT_CTL_DRVF_DROPS | RT_CTL_DRVF_SHARED;
	r->drv_drops = mi->overruns - mi->overruns0;
}

static void mni_drv_reset(struct rt_netif *nif)
{
	struct mniif *mi = nif->driver;

	mi->overruns0 = mi->overruns;
}

static void mni_poll(struct amiga_if *ai)
{
	struct mniif *mi = (struct mniif *)ai;
	struct rt_netif *nif = &ai->nif;
	struct mni_rx *r;
	struct mni_tx *t;
	u32 nrx = 0;

	mni_sample_rx(mi);
	for (;;) {
		Disable();
		r = (struct mni_rx *)remhead_min(&mi->rx_done);
		Enable();
		if (!r)
			break;
		nrx++;
		if (r->status < 0 || r->len > r->frag.len || r->len == 0) {
			rt_drv_rx_done(nif, -1, 0, r->status < 0 ? (s32)r->status : (s32)RT_CTL_DRV_ERR_LENGTH);
			nif->stats.ierrors++;
		} else {
			rt_drv_rx_done(nif, -1, r->len, 0);
			struct rt_buf *b = r->b;
			u16 fl = 0;
			r->b = NULL;
			b->len = r->len;
			b->netif = nif;
			if (r->flags & MNI_FLAG_BCAST)
				fl |= RT_BUFF_BCAST;
			rt_netif_input(nif, b, (u16)r->type, r->src, fl);
		}
		if (rx_attach_buf(mi, r) < 0) {
			rt_drv_rx_nobuf(nif);
			addtail_min(&mi->rx_nobuf, &r->node);
		} else {
			Disable();
			addtail_min(&mi->rx_free, &r->node);
			Enable();
		}
	}
	rt_drv_batch(nif, nrx);
	/* records that could not get a buffer earlier */
	while (mi->rx_nobuf.mlh_Head->mln_Succ) {
		r = (struct mni_rx *)mi->rx_nobuf.mlh_Head;
		if (rx_attach_buf(mi, r) < 0)
			break;
		remhead_min(&mi->rx_nobuf);
		Disable();
		addtail_min(&mi->rx_free, &r->node);
		Enable();
	}
	for (;;) {
		Disable();
		t = (struct mni_tx *)((UBYTE *)remhead_min(&mi->tx_done));
		Enable();
		if (!t)
			break;
		t = (struct mni_tx *)((UBYTE *)t - offsetof(struct mni_tx, node));
		if (t->pkt.status < 0)
			nif->stats.oerrors++;
		rt_buf_free(t->b);
		t->b = NULL;
		t->busy = 0;
		t->node.mln_Succ = (struct MinNode *)mi->tx_free;
		mi->tx_free = t;
		/* last: it may send the next queued frame (QoS) */
		rt_drv_tx_done(nif, t->pkt.status < 0 ? (s32)t->pkt.status : 0);
	}
	if (mi->want_online >= 0) {
		int want = mi->want_online;
		LONG e;
		mi->want_online = -1;
		e = want ? MNI_Online(mi->base, mi->handle) : MNI_Offline(mi->base, mi->handle);
		if (e == 0) {
			ai->online = want;
			if (want)
				nif->flags |= RT_IFF_RUNNING;
			else
				nif->flags &= ~RT_IFF_RUNNING;
		} else {
			rt_log(RT_LOG_WARN, "%s: MNI %s failed (%ld)", nif->name, want ? "online" : "offline", (long)e);
		}
	}
}

static void mni_set_online(struct amiga_if *ai, int online)
{
	((struct mniif *)ai)->want_online = online;
}

/* rt_netif mcast hook: lock held */
static int mni_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct mniif *mi = nif->driver;
	UBYTE a[6];
	LONG e;

	if (!mi->handle)
		return -RT_ENETDOWN;
	/* first join and last leave only (VLAN interfaces share the filter) */
	if (!rt_mc_ref(mi->mcref, MNI_MCREF, &mi->nmcref, hw, join))
		return 0;
	memcpy(a, hw, 6);
	e = join ? MNI_AddMulticast(mi->base, mi->handle, a) : MNI_RemMulticast(mi->base, mi->handle, a);
	if (e && join)
		rt_log(RT_LOG_WARN, "%s: MNI AddMulticast failed (%ld)", nif->name, (long)e);
	return 0;
}

static int mni_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct mniif *mi = nif->driver;
	struct mni_tx *t = mi->tx_free;
	LONG r;

	if (!t || !mi->handle) {
		nif->stats.oerrors++;
		if (!t)
			rt_drv_tx_full(nif);
		rt_buf_free(b);
		return t ? -RT_ENETDOWN : -RT_ENOBUFS;
	}
	mi->tx_free = (struct mni_tx *)t->node.mln_Succ;
	memset(&t->pkt, 0, sizeof(t->pkt));
	t->frag.next = NULL;
	t->frag.data = b->data;
	t->frag.len = b->len;
	t->pkt.frags = &t->frag;
	if (dsthw)
		memcpy(t->pkt.addr, dsthw, 6);
	else
		t->pkt.flags = MNI_FLAG_BCAST;
	t->pkt.type = ethertype;
	t->pkt.length = b->len;
	t->b = b;
	t->busy = 1;
	rt_drv_tx_start(nif, b->len);
	RT_PROF_ENTER(RT_PROF_DRV_TX);
	r = MNI_Send(mi->base, mi->handle, &t->pkt);
	RT_PROF_EXIT(RT_PROF_DRV_TX);
	if (r == 1)
		return 0;          /* completion comes through the tx callback */
	/* refused (0) or not online (-9): we keep the descriptor */
	rt_drv_tx_done(nif, r ? (s32)r : -1);
	nif->stats.oerrors++;
	t->b = NULL;
	t->busy = 0;
	t->node.mln_Succ = (struct MinNode *)mi->tx_free;
	mi->tx_free = t;
	rt_buf_free(b);
	return r == -9 ? -RT_ENETDOWN : -RT_ENOBUFS;
}

static int mni_status(struct amiga_if *ai, char *buf, u32 len)
{
	struct mniif *mi = (struct mniif *)ai;

	return rt_snprintf(buf, len, "MNI %s unit %lu (API %lu), %s, %lu overruns", mi->driver,
	                   (unsigned long)mi->unit, (unsigned long)mi->apiver,
	                   ai->online ? "online" : "offline", (unsigned long)mi->overruns);
}

static void mni_close(struct amiga_if *ai)
{
	struct mniif *mi = (struct mniif *)ai;
	int i;

	rt_lock();
	if (mi->ai.nif.index)
		rt_netif_remove(&mi->ai.nif);
	rt_unlock();
	if (mi->handle) {
		MNI_Offline(mi->base, mi->handle);    /* aborts pending transmits */
		MNI_CloseHandle(mi->base, mi->handle);
		mi->handle = NULL;
	}
	if (mi->base)
		CloseLibrary(mi->base);
	rt_lock();
	for (i = 0; i < MNI_RX_POOL; i++)
		rt_buf_free(mi->rx[i].b);
	for (i = 0; i < MNI_TX_POOL; i++)
		rt_buf_free(mi->tx[i].b);
	rt_unlock();
	rt_free(mi);
}

int mni_open_if(const char *name, const char *driver, ULONG unit, const char *options,
                int mtu, char *err, u32 errlen)
{
	struct mniif *mi;
	struct rt_netif *nif;
	struct mni_open mo;
	ULONG info[8], dq[3];
	UBYTE factory[6], current[6];
	char path[128];
	LONG e;
	int i;

	if (mni_sigbit < 0) {
		mni_sigbit = AllocSignal(-1);
		if (mni_sigbit < 0)
			return -RT_EAGAIN;
	}
	mi = rt_zalloc(sizeof(*mi));
	if (!mi)
		return -RT_ENOMEM;
	mi->want_online = -1;
	mi->unit = unit;
	rt_strlcpy(mi->driver, driver, sizeof(mi->driver));
	rt_strlcpy(mi->options, options ? options : "", sizeof(mi->options));
	rt_newminlist(&mi->rx_free);
	rt_newminlist(&mi->rx_done);
	rt_newminlist(&mi->rx_nobuf);
	rt_newminlist(&mi->tx_done);
	nif = &mi->ai.nif;
	rt_strlcpy(nif->name, name, sizeof(nif->name));
	rt_snprintf(nif->descr, sizeof(nif->descr), "%s unit %lu", amiga_basename(driver),
	            (unsigned long)unit);
	mi->ai.poll = mni_poll;
	mi->ai.set_online = mni_set_online;
	mi->ai.close = mni_close;
	mi->ai.status = mni_status;
	mi->ai.drv = mi;
	mi->ai.sigmask = 1UL << mni_sigbit;

	mi->base = OpenLibrary((CONST_STRPTR)driver, 0);
	if (!mi->base && !strchr(driver, ':') && !strchr(driver, '/')) {
		static const char *const dirs[] = { "DEVS:Networks/", RT_COMPAT_MNI_DIR };
		unsigned d;
		for (d = 0; d < RT_ARRAY_SIZE(dirs) && !mi->base; d++) {
			rt_snprintf(path, sizeof(path), "%s%s", dirs[d], driver);
			mi->base = OpenLibrary((CONST_STRPTR)path, 0);
		}
	}
	if (!mi->base) {
		rt_snprintf(err, errlen, "cannot open MNI driver %s", driver);
		mni_close(&mi->ai);
		return -RT_ENXIO;
	}
	memset(info, 0, sizeof(info));
	info[0] = MNI_API_VERSION;
	MNI_GetInfo(mi->base, info);
	mi->apiver = info[1] ? RT_MIN(info[1], MNI_API_VERSION) : MNI_API_VERSION;
	dq[0] = 48;
	dq[1] = 1500;
	dq[2] = S2WireType_Ethernet;
	MNI_DeviceQuery(mi->base, mi->apiver, dq);
	nif->type = RT_IFT_ETHER;
	nif->hwlen = 6;
	nif->mtu = (u16)(dq[1] ? RT_MIN(dq[1], 9000u) : 1500);
	if (mtu > 0 && mtu < nif->mtu)
		nif->mtu = (u16)mtu;
	nif->flags = RT_IFF_BROADCAST | RT_IFF_SIMPLEX | RT_IFF_MULTICAST;
	nif->link_output = mni_output;
	nif->driver = mi;
	mi->drv.info = mni_drv_info;
	mi->drv.reset = mni_drv_reset;
	mi->drv.rxq[RT_DRV_RX_IP].cfg = MNI_RX_POOL;
	mi->drv.tx_slots = MNI_TX_POOL;
	rt_drv_attach(nif, &mi->drv);

	rt_lock();
	for (i = 0; i < MNI_RX_POOL; i++) {
		if (rx_attach_buf(mi, &mi->rx[i]) == 0)
			addtail_min(&mi->rx_free, &mi->rx[i].node);
		else
			addtail_min(&mi->rx_nobuf, &mi->rx[i].node);
	}
	for (i = 0; i < MNI_TX_POOL; i++) {
		mi->tx[i].node.mln_Succ = (struct MinNode *)mi->tx_free;
		mi->tx_free = &mi->tx[i];
	}
	rt_unlock();

	memset(&mo, 0, sizeof(mo));
	mo.unit = unit;
	mo.cb_getbuf = (APTR)mni_cb_getbuf;
	mo.cb_rxdone = (APTR)mni_cb_rxdone;
	mo.cb_txdone = (APTR)mni_cb_txdone;
	mo.userdata = mi;
	mo.options = (STRPTR)mi->options;
	e = MNI_OpenHandle(mi->base, mi->apiver, &mo);
	if (e != 0 || !mo.handle) {
		rt_snprintf(err, errlen, "MNI driver %s: cannot open unit %lu (%ld)", driver,
		            (unsigned long)unit, (long)e);
		mni_close(&mi->ai);
		return -RT_ENXIO;
	}
	mi->handle = mo.handle;
	memset(factory, 0, 6);
	memset(current, 0, 6);
	MNI_GetStationAddress(mi->base, mi->handle, factory, current);
	memcpy(nif->hwaddr, memcmp(current, "\0\0\0\0\0\0", 6) ? current : factory, 6);
	MNI_AddPacketType(mi->base, mi->handle, RT_ETH_P_IP);
	MNI_AddPacketType(mi->base, mi->handle, RT_ETH_P_ARP);
	MNI_AddPacketType(mi->base, mi->handle, RT_ETH_P_IPV6);
	nif->mcast = mni_mcast;
	e = MNI_Online(mi->base, mi->handle);
	mi->ai.online = e == 0;
	if (e)
		rt_log(RT_LOG_WARN, "%s: MNI online failed (%ld)", name, (long)e);

	rt_lock();
	if (mi->ai.online)
		nif->flags |= RT_IFF_RUNNING;
	i = rt_netif_add(nif);
	if (i >= 0) {
		mi->ai.next = amiga_ifs;
		amiga_ifs = &mi->ai;
	}
	rt_unlock();
	if (i < 0) {
		rt_snprintf(err, errlen, "cannot add interface %s", name);
		mni_close(&mi->ai);
		return i;
	}
	rt_log(RT_LOG_INFO, "%s: MNI %s unit %lu, API %lu, mtu %u", name, driver, (unsigned long)unit,
	       (unsigned long)mi->apiver, (unsigned)nif->mtu);
	return 0;
}
