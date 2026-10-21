/*
 * Riptide - SANA-II network interface driver.
 *
 *   interface NAME device=DEVS:Networks/xyz.device unit=N [mtu=N] [raw]
 *
 * The device is opened with buffer management callbacks that copy straight
 * into and out of rt_bufs (ios2_Data points at the rt_buf). The callbacks
 * may run in the driver's interrupt code, so they only copy. Ethernet-like
 * devices get CMD_READs for IPv4, ARP and IPv6 (S2_READS_IP6); point-to-point
 * devices (PPP, SLIP: HardwareType != Ethernet) the IPv4 and IPv6 ones.
 * IPv6 multicast groups (solicited-node, all-nodes, MLD joins) are passed to
 * the driver with S2_ADDMULTICASTADDRESS / S2_DELMULTICASTADDRESS; the core
 * asks under the stack lock from any task, so the requests are queued and
 * issued by the Riptide process in sana2_poll. Cooked mode is
 * the default; "raw" makes Riptide build and strip the Ethernet header
 * itself (SANA2IOF_RAW). All replies come to a port of the Riptide process,
 * which processes them under the stack lock (sana2_poll).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

/* Read requests queued per interface. A TCP sender (e.g. slirp in an
 * emulator, or a fast host on the LAN) delivers a whole window as one
 * burst; any frame that arrives while no read is queued is dropped by the
 * driver, so this must cover a typical burst. 'reads=N' overrides. */
#define S2_READS_IP_DEF 48
#define S2_READS_IP_MIN 4
#define S2_READS_IP_MAX 128
#define S2_READS_ARP  4
#define S2_READS_IP6  24           /* a 32 KB window of IPv6 segments */
#define S2_MCQ        16             /* queued multicast filter changes */
#define S2_MAX_WRITES 32
#define S2_RX_HEADROOM 64
#define ETH_HLEN 14

enum { REQ_READ = 1, REQ_WRITE = 2 };

struct s2req {
	struct IOSana2Req io;        /* must be first: replies are cast back */
	struct s2req *next;          /* free list / all list */
	struct s2req *all_next;
	struct rt_buf *b;
	UBYTE kind;
	UBYTE busy;
	UBYTE idle;                  /* read waiting for resubmission */
	UBYTE pad;
};

struct s2if {
	struct amiga_if ai;          /* must be first */
	struct MsgPort *port;        /* replies of reads and writes */
	struct MsgPort *ctlport;     /* synchronous control requests */
	struct IOSana2Req *ctl;
	APTR   bufmgmt;              /* cookie from OpenDevice */
	int    dev_open;
	int    ether, raw;
	ULONG  hwtype, bps, unit;
	char   device[96];
	struct s2req *all;           /* every request we allocated */
	struct s2req *wfree;
	int    nwrites, wbusy;
	int    want_online;          /* request from RiptideOnOffline */
	struct rt_timer retry;
	u32    rx_errors;
	/* multicast filter changes waiting for the Riptide process */
	struct { u8 hw[6]; u8 join; } mcq[S2_MCQ];
	int    mcq_n;
	int    mc_unsupported;      /* driver said S2ERR_NOT_SUPPORTED: warned */
};

/* ---- buffer management callbacks (may be called from interrupts) ---- */

#if defined(__VBCC__)
#define S2CB static
#else
#define S2CB
#endif

S2CB ULONG s2_copy_to_buff(RTREG(a0) APTR to, RTREG(a1) APTR from, RTREG(d0) ULONG n)
{
	struct rt_buf *b = to;

	if (!b || n > rt_buf_tailroom(b) + b->len)
		return FALSE;
	rt_copy(b->data, from, n);   /* reentrant; never MOVE16 (driver memory) */
	b->len = n;
	return TRUE;
}

S2CB ULONG s2_copy_from_buff(RTREG(a0) APTR to, RTREG(a1) APTR from, RTREG(d0) ULONG n)
{
	struct rt_buf *b = from;

	if (!b || n > b->len)
		return FALSE;
	rt_copy(to, b->data, n);
	return TRUE;
}

#if defined(__VBCC__)
#define CB_TO   ((ULONG)s2_copy_to_buff)
#define CB_FROM ((ULONG)s2_copy_from_buff)
#else
/* gcc_thunks.S: register -> stack argument thunks */
extern char s2_copy_to_buff_thunk[], s2_copy_from_buff_thunk[];
#define CB_TO   ((ULONG)s2_copy_to_buff_thunk)
#define CB_FROM ((ULONG)s2_copy_from_buff_thunk)
#endif

/* ---- helpers ---- */

static LONG s2_ctl(struct s2if *si, UWORD cmd)
{
	LONG r;

	si->ctl->ios2_Req.io_Command = cmd;
	si->ctl->ios2_Req.io_Flags = 0;
	si->ctl->ios2_BufferManagement = si->bufmgmt;
	r = DoIO((struct IORequest *)si->ctl);
	return r;
}

static struct s2req *s2_newreq(struct s2if *si, int kind)
{
	struct s2req *r = rt_zalloc(sizeof(*r));

	if (!r)
		return NULL;
	r->io.ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	r->io.ios2_Req.io_Message.mn_ReplyPort = si->port;
	r->io.ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
	r->io.ios2_Req.io_Device = si->ctl->ios2_Req.io_Device;
	r->io.ios2_Req.io_Unit = si->ctl->ios2_Req.io_Unit;
	r->io.ios2_BufferManagement = si->bufmgmt;
	r->kind = (UBYTE)kind;
	r->all_next = si->all;
	si->all = r;
	return r;
}

static u32 rx_size(struct s2if *si)
{
	return (u32)si->ai.nif.mtu + (si->raw ? ETH_HLEN : 0) + 4;
}

/* lock held */
static int s2_submit_read(struct s2if *si, struct s2req *r)
{
	if (!r->b) {
		r->b = rt_buf_alloc(S2_RX_HEADROOM, rx_size(si));
		if (!r->b) {
			r->idle = 1;
			return -1;
		}
	}
	r->b->len = 0;
	r->io.ios2_Req.io_Command = CMD_READ;
	r->io.ios2_Req.io_Flags = si->raw ? SANA2IOF_RAW : 0;
	r->io.ios2_Req.io_Error = 0;
	r->io.ios2_DataLength = 0;
	r->io.ios2_Data = r->b;
	r->idle = 0;
	r->busy = 1;
	SendIO((struct IORequest *)&r->io);
	return 0;
}

static void s2_retry_timer(void *arg)
{
	struct s2if *si = arg;
	struct s2req *r;
	int pending = 0;

	for (r = si->all; r; r = r->all_next) {
		if (r->kind == REQ_READ && r->idle && s2_submit_read(si, r) < 0)
			pending = 1;
	}
	if (pending)
		rt_timer_after(&si->retry, 1000);
}

static void s2_read_done(struct s2if *si, struct s2req *r)
{
	struct rt_netif *nif = &si->ai.nif;
	struct rt_buf *b = r->b;
	u16 type = (u16)r->io.ios2_PacketType, flags = 0;
	const u8 *src = r->io.ios2_SrcAddr;
	u8 srcbuf[6];

	r->busy = 0;
	if (r->io.ios2_Req.io_Error) {
		nif->stats.ierrors++;
		si->rx_errors++;
		r->idle = 1;
		if (r->io.ios2_Req.io_Error == S2ERR_OUTOFSERVICE || r->io.ios2_Req.io_Error == IOERR_ABORTED) {
			si->ai.online = 0;
			nif->flags &= ~RT_IFF_RUNNING;
		}
		if (!rt_timer_pending(&si->retry))
			rt_timer_after(&si->retry, 1000);
		return;
	}
	if (b->len == 0 || b->len > rx_size(si))
		b->len = RT_MIN(r->io.ios2_DataLength, rx_size(si));
	if (si->raw && si->ether) {
		if (b->len < ETH_HLEN) {
			nif->stats.ierrors++;
			s2_submit_read(si, r);
			return;
		}
		src = b->data + 6;
		type = rt_get_be16(b->data + 12);
		rt_buf_pull(b, ETH_HLEN);
	}
	if (r->io.ios2_Req.io_Flags & SANA2IOF_BCAST)
		flags |= RT_BUFF_BCAST;
	if (r->io.ios2_Req.io_Flags & SANA2IOF_MCAST)
		flags |= RT_BUFF_MCAST;
	nif->stats.ipackets++;
	nif->stats.ibytes += b->len;
	if (flags & (RT_BUFF_BCAST | RT_BUFF_MCAST))
		nif->stats.imcasts++;
	if (!si->ai.online) {
		si->ai.online = 1;
		nif->flags |= RT_IFF_RUNNING;
	}
	/* Give the driver a fresh buffer before processing this frame:
	 * protocol input can take a while on a slow CPU, and frames that
	 * arrive meanwhile need a queued read or they are lost. The request
	 * is reused at once, so copy the source address out of it first. */
	if (src && src != srcbuf) {
		memcpy(srcbuf, src, 6);
		src = srcbuf;
	}
	r->b = NULL;
	s2_submit_read(si, r);
	b->netif = nif;
	rt_netif_input(nif, b, type, si->ether ? src : NULL, flags);
}

static void s2_write_done(struct s2if *si, struct s2req *r)
{
	struct rt_netif *nif = &si->ai.nif;

	if (r->io.ios2_Req.io_Error)
		nif->stats.oerrors++;
	rt_buf_free(r->b);
	r->b = NULL;
	r->busy = 0;
	si->wbusy--;
	r->next = si->wfree;
	si->wfree = r;
}

/* rt_netif mcast hook: lock held, any task. Queue for sana2_poll. */
static int sana2_mcast(struct rt_netif *nif, const u8 *hw, int join)
{
	struct s2if *si = nif->driver;
	int i;

	if (!si->ether || nif->hwlen != 6)
		return 0;
	/* a pending opposite request for the same address cancels out */
	for (i = 0; i < si->mcq_n; i++) {
		if (!memcmp(si->mcq[i].hw, hw, 6) && si->mcq[i].join != (u8)join) {
			si->mcq_n--;
			memmove(&si->mcq[i], &si->mcq[i + 1], (u32)(si->mcq_n - i) * sizeof(si->mcq[0]));
			return 0;
		}
	}
	if (si->mcq_n >= S2_MCQ) {
		rt_log(RT_LOG_WARN, "%s: multicast filter queue full", nif->name);
		return -RT_ENOBUFS;
	}
	memcpy(si->mcq[si->mcq_n].hw, hw, 6);
	si->mcq[si->mcq_n].join = (u8)join;
	si->mcq_n++;
	rt_kick();
	return 0;
}

/* lock held (dropped around the device calls) */
static void s2_mcast_flush(struct s2if *si)
{
	while (si->mcq_n > 0 && si->dev_open) {
		u8 hw[6], join = si->mcq[0].join;
		LONG e;
		memcpy(hw, si->mcq[0].hw, 6);
		si->mcq_n--;
		memmove(&si->mcq[0], &si->mcq[1], (u32)si->mcq_n * sizeof(si->mcq[0]));
		rt_unlock();
		memset(si->ctl->ios2_SrcAddr, 0, sizeof(si->ctl->ios2_SrcAddr));
		memcpy(si->ctl->ios2_SrcAddr, hw, 6);
		e = s2_ctl(si, join ? S2_ADDMULTICASTADDRESS : S2_DELMULTICASTADDRESS);
		rt_lock();
		if (e == S2ERR_NOT_SUPPORTED) {
			if (!si->mc_unsupported)
				rt_log(RT_LOG_WARN, "%s: driver has no multicast filter: IPv6 neighbour "
				       "discovery may not work", si->ai.nif.name);
			si->mc_unsupported = 1;
		} else if (e && join) {
			rt_log(RT_LOG_WARN, "%s: S2_ADDMULTICASTADDRESS failed: %s/%s", si->ai.nif.name,
			       amiga_s2err_str(e), amiga_s2werr_str((LONG)si->ctl->ios2_WireError));
		}
	}
}

/* lock held */
static void sana2_poll(struct amiga_if *ai)
{
	struct s2if *si = (struct s2if *)ai;
	struct s2req *r;

	while ((r = (struct s2req *)GetMsg(si->port)) != NULL) {
		if (r->kind == REQ_READ)
			s2_read_done(si, r);
		else
			s2_write_done(si, r);
	}
	if (si->mcq_n)
		s2_mcast_flush(si);
	if (si->want_online >= 0 && si->dev_open) {
		int want = si->want_online;
		LONG e;
		si->want_online = -1;
		rt_unlock();
		e = s2_ctl(si, want ? S2_ONLINE : S2_OFFLINE);
		rt_lock();
		if (e == 0 || (want && si->ctl->ios2_WireError == S2WERR_UNIT_ONLINE)) {
			si->ai.online = want;
			if (want) {
				si->ai.nif.flags |= RT_IFF_RUNNING;
				s2_retry_timer(si);
			} else {
				si->ai.nif.flags &= ~RT_IFF_RUNNING;
			}
		} else {
			rt_log(RT_LOG_WARN, "%s: %s failed: %s", si->ai.nif.name, want ? "S2_ONLINE" : "S2_OFFLINE",
			       amiga_s2err_str(e));
		}
	}
}

static void sana2_set_online(struct amiga_if *ai, int online)
{
	((struct s2if *)ai)->want_online = online;
}

/* rt_netif link_output; lock held, may run in any task */
static int sana2_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct s2if *si = nif->driver;
	struct s2req *r = si->wfree;

	if (!si->dev_open) {
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	if (r) {
		si->wfree = r->next;
	} else if (si->nwrites < S2_MAX_WRITES) {
		r = s2_newreq(si, REQ_WRITE);
		if (r)
			si->nwrites++;
	}
	if (!r) {
		nif->stats.oerrors++;
		nif->stats.iqdrops++;
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
	r->io.ios2_Req.io_Flags = 0;
	if (si->ether) {
		if (si->raw) {
			u8 *h;
			if (rt_buf_headroom(b) < ETH_HLEN) {
				struct rt_buf *nb = rt_buf_copy_expand(b, ETH_HLEN);
				rt_buf_free(b);
				if (!nb) {
					r->next = si->wfree;
					si->wfree = r;
					nif->stats.oerrors++;
					return -RT_ENOBUFS;
				}
				b = nb;
			}
			h = rt_buf_push(b, ETH_HLEN);
			if (dsthw)
				memcpy(h, dsthw, 6);
			else
				memset(h, 0xff, 6);
			memcpy(h + 6, nif->hwaddr, 6);
			rt_put_be16(h + 12, ethertype);
			r->io.ios2_Req.io_Flags = SANA2IOF_RAW;
		}
		if (dsthw) {
			memcpy(r->io.ios2_DstAddr, dsthw, nif->hwlen);
			/* SANA-II wants S2_MULTICAST for group addresses; IPv4
			 * keeps CMD_WRITE as before */
			r->io.ios2_Req.io_Command = ethertype == RT_ETH_P_IPV6 && (dsthw[0] & 1) ?
			                            S2_MULTICAST : CMD_WRITE;
		} else {
			r->io.ios2_Req.io_Command = S2_BROADCAST;
		}
	} else {
		r->io.ios2_Req.io_Command = CMD_WRITE;
	}
	r->io.ios2_PacketType = ethertype;
	r->io.ios2_DataLength = b->len;
	r->io.ios2_Data = b;
	r->io.ios2_Req.io_Error = 0;
	r->b = b;
	r->busy = 1;
	si->wbusy++;
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	SendIO((struct IORequest *)&r->io);
	return 0;
}

static int sana2_status(struct amiga_if *ai, char *buf, u32 len)
{
	struct s2if *si = (struct s2if *)ai;

	return rt_snprintf(buf, len, "SANA-II %s unit %lu, %s%s, %lu bit/s, %s",
	                   si->device, (unsigned long)si->unit,
	                   si->ether ? "Ethernet" : "point-to-point", si->raw ? " (raw)" : "",
	                   (unsigned long)si->bps, ai->online ? "online" : "offline");
}

/* lock NOT held */
static void sana2_close(struct amiga_if *ai)
{
	struct s2if *si = (struct s2if *)ai;
	struct s2req *r, *n;

	rt_lock();
	rt_timer_del(&si->retry);
	if (si->ai.nif.index)
		rt_netif_remove(&si->ai.nif);
	rt_unlock();
	if (si->dev_open) {
		for (r = si->all; r; r = r->all_next) {
			if (r->busy) {
				AbortIO((struct IORequest *)&r->io);
				WaitIO((struct IORequest *)&r->io);
				r->busy = 0;
			}
		}
		CloseDevice((struct IORequest *)si->ctl);
		si->dev_open = 0;
	}
	rt_lock();
	for (r = si->all; r; r = n) {
		n = r->all_next;
		rt_buf_free(r->b);
		rt_free(r);
	}
	rt_unlock();
	if (si->ctl)
		DeleteIORequest((struct IORequest *)si->ctl);
	if (si->ctlport)
		DeleteMsgPort(si->ctlport);
	if (si->port)
		DeleteMsgPort(si->port);
	rt_free(si);
}

int sana2_open_if(const char *name, const char *device, ULONG unit, int mtu, int raw, int reads,
                  char *err, u32 errlen)
{
	struct s2if *si;
	struct Sana2DeviceQuery q;
	struct TagItem bm[3];
	struct rt_netif *nif;
	struct s2req *r;
	int i, nreads;
	LONG e;

	si = rt_zalloc(sizeof(*si));
	if (!si)
		return -RT_ENOMEM;
	si->want_online = -1;
	si->unit = unit;
	si->raw = raw;
	rt_strlcpy(si->device, device, sizeof(si->device));
	nif = &si->ai.nif;
	rt_strlcpy(nif->name, name, sizeof(nif->name));
	rt_snprintf(nif->descr, sizeof(nif->descr), "%s unit %lu", amiga_basename(device),
	            (unsigned long)unit);
	si->ai.poll = sana2_poll;
	si->ai.set_online = sana2_set_online;
	si->ai.close = sana2_close;
	si->ai.status = sana2_status;
	si->ai.drv = si;
	rt_timer_init(&si->retry, s2_retry_timer, si);

	si->port = CreateMsgPort();
	si->ctlport = CreateMsgPort();
	if (!si->port || !si->ctlport)
		goto nomem;
	si->ctl = (struct IOSana2Req *)CreateIORequest(si->ctlport, sizeof(struct IOSana2Req));
	if (!si->ctl)
		goto nomem;
	si->ai.sigmask = 1UL << si->port->mp_SigBit;
	bm[0].ti_Tag = S2_CopyToBuff;
	bm[0].ti_Data = CB_TO;
	bm[1].ti_Tag = S2_CopyFromBuff;
	bm[1].ti_Data = CB_FROM;
	bm[2].ti_Tag = TAG_DONE;
	bm[2].ti_Data = 0;
	si->ctl->ios2_BufferManagement = bm;
	e = OpenDevice((CONST_STRPTR)device, unit, (struct IORequest *)si->ctl, 0);
	if (e) {
		rt_snprintf(err, errlen, "cannot open %s unit %lu (error %ld)", device, (unsigned long)unit, (long)e);
		sana2_close(&si->ai);
		return -RT_ENXIO;
	}
	si->dev_open = 1;
	si->bufmgmt = si->ctl->ios2_BufferManagement;

	memset(&q, 0, sizeof(q));
	q.SizeAvailable = sizeof(q);
	si->ctl->ios2_StatData = &q;
	if (s2_ctl(si, S2_DEVICEQUERY) != 0) {
		/* very old drivers: assume Ethernet */
		q.HardwareType = S2WireType_Ethernet;
		q.MTU = 1500;
		q.AddrFieldSize = 48;
	}
	si->hwtype = q.HardwareType;
	si->bps = q.BPS;
	si->ether = q.HardwareType == S2WireType_Ethernet || q.HardwareType == S2WireType_IEEE802;
	if (!si->ether)
		si->raw = 0;
	nif->hwlen = (u8)RT_MIN((q.AddrFieldSize + 7) / 8, RT_MAX_HWADDR);
	nif->mtu = (u16)(q.MTU ? RT_MIN(q.MTU, 65535u) : 1500);
	if (mtu > 0 && mtu < nif->mtu)
		nif->mtu = (u16)mtu;

	if (s2_ctl(si, S2_GETSTATIONADDRESS) == 0) {
		static const u8 zero[RT_MAX_HWADDR];
		const u8 *a = si->ctl->ios2_SrcAddr;
		if (!memcmp(a, zero, nif->hwlen))
			a = si->ctl->ios2_DstAddr;          /* factory default */
		memcpy(nif->hwaddr, a, nif->hwlen);
		memcpy(si->ctl->ios2_SrcAddr, a, nif->hwlen);
		e = s2_ctl(si, S2_CONFIGINTERFACE);
		if (e && si->ctl->ios2_WireError != S2WERR_IS_CONFIGURED)
			rt_log(RT_LOG_WARN, "%s: S2_CONFIGINTERFACE failed: %s/%s", name, amiga_s2err_str(e),
			       amiga_s2werr_str((LONG)si->ctl->ios2_WireError));
	}
	e = s2_ctl(si, S2_ONLINE);
	si->ai.online = e == 0 || si->ctl->ios2_WireError == S2WERR_UNIT_ONLINE;

	if (si->ether) {
		nif->type = RT_IFT_ETHER;
		nif->flags = RT_IFF_BROADCAST | RT_IFF_SIMPLEX | RT_IFF_MULTICAST;
	} else {
		nif->type = RT_IFT_PTP;
		nif->flags = RT_IFF_POINTOPOINT | RT_IFF_NOARP;
		nif->hwlen = 0;
	}
	nif->link_output = sana2_output;
	nif->mcast = sana2_mcast;
	nif->driver = si;

	rt_lock();
	i = rt_netif_add(nif);
	if (i < 0) {
		rt_unlock();
		rt_snprintf(err, errlen, "cannot add interface %s", name);
		sana2_close(&si->ai);
		return i;
	}
	if (si->ai.online)
		nif->flags |= RT_IFF_RUNNING;
	if (reads <= 0)
		reads = S2_READS_IP_DEF;
	reads = RT_MAX(S2_READS_IP_MIN, RT_MIN(reads, S2_READS_IP_MAX));
	nreads = reads + (si->ether ? S2_READS_ARP : 0);
	for (i = 0; i < nreads; i++) {
		r = s2_newreq(si, REQ_READ);
		if (!r)
			break;
		r->io.ios2_PacketType = i < reads ? RT_ETH_P_IP : RT_ETH_P_ARP;
		s2_submit_read(si, r);
	}
	si->ai.next = amiga_ifs;
	amiga_ifs = &si->ai;
	/* IPv6 has its own reads: a burst of IPv6 segments must not depend
	 * on the IPv4 ones being queued */
	for (i = 0; i < S2_READS_IP6; i++) {
		r = s2_newreq(si, REQ_READ);
		if (!r)
			break;
		r->io.ios2_PacketType = RT_ETH_P_IPV6;
		s2_submit_read(si, r);
	}
	rt_unlock();
	rt_log(RT_LOG_INFO, "%s: %s unit %lu, %s, mtu %u, %d reads", name, device, (unsigned long)unit,
	       si->ether ? "Ethernet" : "point-to-point", (unsigned)nif->mtu, reads);
	return 0;

nomem:
	sana2_close(&si->ai);
	return -RT_ENOMEM;
}
