/*
 * Riptide test helper - fakenetdev: a netdev driver (devices/netdev.h ABI
 * version 1, as Emu68's genet.device 4.x) on top of a SANA-II driver, so
 * Riptide's netdev backend can be tested in an emulator without a
 * Raspberry Pi 4.
 *
 *   fakenetdev [DEVICE] [UNIT] [STATSFILE] [PRI]   (default uaenet.device unit 2, priority 5)
 *
 * It puts "fakenet.device" into exec's device list and stays running as
 * the driver's unit task: frames the stack submits (ndo_TxSubmit) go out
 * as raw SANA-II writes, raw reads of IPv4, ARP and IPv6 come back through
 * the stack's nso_RxInput in batches, and completed writes through
 * nso_TxDone. Every callback into the stack runs on this task, as the ABI
 * demands. Control commands are answered at once in the caller's context
 * (DoIO). Built with vbcc: the device vectors take register arguments, the
 * netdev entries plain C ones. Never exits; meant for a test boot.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include "../../src/amiga/os/sana2.h"
#include <devices/newstyle.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdlib.h>

#define NETDEV_CMD_ATTACH        0x8900
#define NETDEV_CMD_DETACH        0x8901
#define NETDEV_CMD_START         0x8902
#define NETDEV_CMD_STOP          0x8903
#define NETDEV_CMD_SET_RXFILTER  0x8904
#define NETDEV_CMD_SET_COALESCE  0x8905
#define NETDEV_CMD_GET_LINK      0x8907

struct NetDevCaps {
	UWORD ndc_AbiVersion, ndc_Mtu;
	UBYTE ndc_Mac[6];
	UWORD ndc_TxMaxSegs;
	ULONG ndc_Features;
	UWORD ndc_TxRingSlots, ndc_RxRingSlots, ndc_TxAlign, ndc_TxInFlightMax;
	ULONG ndc_RxPoolBufs;
	ULONG ndc_Reserved[3];
};
struct NetDevSg { APTR nsg_Data; ULONG nsg_Len; };
struct NetDevTxDesc {
	const struct NetDevSg *ntd_Segs;
	UWORD ntd_NumSegs, ntd_Flags, ntd_CsumStart, ntd_CsumOffset, ntd_VlanTci;
	APTR  ntd_Cookie;
};
struct NetDevRxDesc {
	APTR  nrd_Data;
	ULONG nrd_Len;
	UWORD nrd_Flags, nrd_CsumRaw, nrd_VlanTci;
	APTR  nrd_Cookie;
};
struct NetDevLinkState { UWORD ndls_Flags, ndls_SpeedMbps; };
struct NetDevDrvOps {
	LONG (*ndo_TxSubmit)(APTR, const struct NetDevTxDesc *, ULONG);
	void (*ndo_TxKick)(APTR);
	void (*ndo_RxRelease)(APTR, APTR);
	APTR (*ndo_DmaAlloc)(APTR, ULONG, ULONG);
	void (*ndo_DmaFree)(APTR, APTR, ULONG);
};
struct NetDevStackOps {
	ULONG (*nso_RxInput)(APTR, const struct NetDevRxDesc *, ULONG);
	void  (*nso_TxDone)(APTR, APTR const *, ULONG);
	void  (*nso_LinkChange)(APTR, const struct NetDevLinkState *);
};
struct NetDevAttach {
	UWORD nda_AbiVersion, nda_RxHoldReq, nda_MtuReq, nda_RxBatch;
	APTR  nda_StackCtx;
	const struct NetDevStackOps *nda_StackOps;
	APTR  nda_DrvCtx;
	const struct NetDevDrvOps *nda_DrvOps;
	struct NetDevCaps nda_Caps;
};
typedef char abi_check[(sizeof(struct NetDevAttach) == 64 && sizeof(struct NetDevRxDesc) == 18 &&
                        sizeof(struct NetDevTxDesc) == 18) ? 1 : -1];

#define NRX   32
#define NTX   48
#define BATCH 16

struct rxbuf {
	struct IOSana2Req *io;
	int   state;                  /* 0 free, 1 reading, 2 held by the stack */
	ULONG len;
	UBYTE data[1536];
};
struct txreq {
	struct IOSana2Req *io;
	APTR  cookie;
	int   busy;
};

static struct Device *dev;
static struct Task *unit_task;
static struct MsgPort *s2port;
static struct IOSana2Req *s2ctl;
static struct rxbuf rx[NRX];
static struct txreq tx[NTX];
static UBYTE mac[6];
static APTR stackctx;
static const struct NetDevStackOps *sops;
static volatile int attached, started, link_due, txbusy;
static ULONG sig_kick;
static ULONG rx_frames, tx_frames, rx_errors, tx_errors, rx_refused;
static struct DateStamp last_stats;

/* the counters, at most once a second, to the file named by the third argument */
static const char *stats_path;
static void write_stats(void)
{
	struct DateStamp now;
	BPTR fh;
	LONG ticks;

	DateStamp(&now);
	ticks = (now.ds_Minute - last_stats.ds_Minute) * 3000 + (now.ds_Tick - last_stats.ds_Tick);
	if (ticks < 50 && ticks >= 0)
		return;
	last_stats = now;
	if (!stats_path)
		return;
	fh = Open((STRPTR)stats_path, MODE_NEWFILE);
	if (fh) {
		FPrintf(fh, "rx %ld tx %ld rx_errors %ld tx_errors %ld refused %ld attached %ld started %ld\n",
		        (long)rx_frames, (long)tx_frames, (long)rx_errors, (long)tx_errors, (long)rx_refused,
		        (long)attached, (long)started);
		Close(fh);
	}
}

/* ---- SANA-II buffer management (register arguments) ---- */

static ULONG __saveds copy_to(__reg("a0") APTR to, __reg("a1") APTR from, __reg("d0") ULONG n)
{
	struct rxbuf *b = to;

	if (n > sizeof(b->data) - 14)
		return FALSE;
	CopyMem(from, b->data + 14, n);   /* the header is rebuilt in front */
	b->len = n + 14;
	return TRUE;
}

static ULONG __saveds copy_from(__reg("a0") APTR to, __reg("a1") APTR from, __reg("d0") ULONG n)
{
	CopyMem(from, to, n);         /* 'from' is the stack's segment */
	return TRUE;
}

/* ---- netdev entries (plain C, any task) ---- */

static LONG __saveds tx_submit(APTR ctx, const struct NetDevTxDesc *d, ULONG count)
{
	ULONG i, k;
	UBYTE *f;

	for (i = 0; i < count; i++, d++) {
		if (d->ntd_NumSegs != 1 || d->ntd_Segs[0].nsg_Len < 14)
			break;
		Forbid();
		for (k = 0; k < NTX && tx[k].busy; k++)
			;
		if (k < NTX) {
			tx[k].busy = 1;
			txbusy++;
		}
		Permit();
		if (k == NTX)
			break;                /* ring full: a short return */
		f = d->ntd_Segs[0].nsg_Data;
		tx[k].cookie = d->ntd_Cookie;
		/* uaenet.device ignores SANA2IOF_RAW on writes: the frame's header
		 * goes into the request, its payload is the data */
		tx[k].io->ios2_Req.io_Command = (f[0] == 0xff && f[1] == 0xff && f[2] == 0xff) ? S2_BROADCAST :
		                                (f[0] & 1) ? S2_MULTICAST : CMD_WRITE;
		tx[k].io->ios2_Req.io_Flags = 0;
		tx[k].io->ios2_PacketType = ((ULONG)f[12] << 8) | f[13];
		memcpy(tx[k].io->ios2_DstAddr, f, 6);
		tx[k].io->ios2_DataLength = d->ntd_Segs[0].nsg_Len - 14;
		tx[k].io->ios2_Data = f + 14;
		SendIO((struct IORequest *)tx[k].io);
	}
	return (LONG)i;
}

static void __saveds tx_kick(APTR ctx)
{
}

static void __saveds rx_release(APTR ctx, APTR cookie)
{
	struct rxbuf *b = cookie;

	Forbid();
	b->state = 0;
	Permit();
	Signal(unit_task, sig_kick);
}

/* a0 = 0: AmigaOS returns results in d0 only, and a stack that took a
 * pointer result from a0 (SVR4 gcc) must not pass by luck */
void zero_a0(void) = "\tsub.l\ta0,a0";

static APTR __saveds dma_alloc(APTR ctx, ULONG size, ULONG align)
{
	APTR p = AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);

	zero_a0();
	return p;
}

static void __saveds dma_free(APTR ctx, APTR p, ULONG size)
{
	FreeVec(p);
}

static const struct NetDevDrvOps drvops = { tx_submit, tx_kick, rx_release, dma_alloc, dma_free };

/* ---- the device (register arguments) ---- */

static UWORD cmds[] = { NSCMD_DEVICEQUERY, NETDEV_CMD_ATTACH, NETDEV_CMD_DETACH, NETDEV_CMD_START,
                        NETDEV_CMD_STOP, NETDEV_CMD_SET_RXFILTER, NETDEV_CMD_SET_COALESCE,
                        NETDEV_CMD_GET_LINK, 0 };

static void __saveds dev_open(__reg("a6") struct Device *d, __reg("a1") struct IORequest *io,
                              __reg("d0") ULONG unit, __reg("d1") ULONG flags)
{
	d->dd_Library.lib_OpenCnt++;
	io->io_Device = d;
	io->io_Unit = NULL;
	io->io_Error = 0;
}

static APTR __saveds dev_close(__reg("a6") struct Device *d, __reg("a1") struct IORequest *io)
{
	d->dd_Library.lib_OpenCnt--;
	io->io_Device = (struct Device *)-1;
	return NULL;
}

static APTR __saveds dev_expunge(__reg("a6") struct Device *d)
{
	return NULL;
}

static APTR __saveds dev_null(void)
{
	return NULL;
}

static void __saveds dev_beginio(__reg("a6") struct Device *d, __reg("a1") struct IOStdReq *io)
{
	io->io_Error = 0;
	switch (io->io_Command) {
	case NSCMD_DEVICEQUERY: {
		struct NSDeviceQueryResult *q = io->io_Data;
		q->nsdqr_DevQueryFormat = 0;
		q->nsdqr_SizeAvailable = sizeof(*q);
		q->nsdqr_DeviceType = NSDEVTYPE_SANA2;   /* close enough: a network device */
		q->nsdqr_DeviceSubType = 0;
		q->nsdqr_SupportedCommands = cmds;
		io->io_Actual = sizeof(*q);
		break;
	}
	case NETDEV_CMD_ATTACH: {
		struct NetDevAttach *a = io->io_Data;
		if (attached) {
			io->io_Error = 41;
			break;
		}
		a->nda_AbiVersion = 1;
		stackctx = a->nda_StackCtx;
		sops = a->nda_StackOps;
		a->nda_DrvCtx = (APTR)0x1234;
		a->nda_DrvOps = &drvops;
		memset(&a->nda_Caps, 0, sizeof(a->nda_Caps));
		a->nda_Caps.ndc_AbiVersion = 1;
		a->nda_Caps.ndc_Mtu = 1500;
		memcpy(a->nda_Caps.ndc_Mac, mac, 6);
		a->nda_Caps.ndc_TxMaxSegs = 1;
		a->nda_Caps.ndc_Features = 1UL << 5;   /* NDCF_LINK_EVENTS */
		a->nda_Caps.ndc_TxRingSlots = NTX;
		a->nda_Caps.ndc_RxRingSlots = NRX;
		a->nda_Caps.ndc_TxInFlightMax = NTX;
		a->nda_Caps.ndc_RxPoolBufs = NRX;
		attached = 1;
		break;
	}
	case NETDEV_CMD_START:
		started = 1;
		link_due = 1;
		Signal(unit_task, sig_kick);
		break;
	case NETDEV_CMD_STOP: {
		int n;
		started = 0;
		/* every transmit completes before the op replies */
		for (n = 0; txbusy && n < 100; n++)
			Delay(2);
		break;
	}
	case NETDEV_CMD_DETACH:
		attached = 0;
		break;
	case NETDEV_CMD_SET_RXFILTER:
	case NETDEV_CMD_SET_COALESCE:
		break;
	case NETDEV_CMD_GET_LINK: {
		struct NetDevLinkState *l = io->io_Data;
		l->ndls_Flags = 3;
		l->ndls_SpeedMbps = 1000;
		break;
	}
	default:
		io->io_Error = IOERR_NOCMD;
	}
	/* answered at once: DoIO() sees IOF_QUICK */
	if (!(io->io_Flags & IOF_QUICK))
		ReplyMsg(&io->io_Message);
}

static ULONG __saveds dev_abortio(__reg("a6") struct Device *d, __reg("a1") struct IORequest *io)
{
	return 0;
}

static APTR vectors[] = { (APTR)dev_open, (APTR)dev_close, (APTR)dev_expunge, (APTR)dev_null,
                          (APTR)dev_beginio, (APTR)dev_abortio, (APTR)-1 };

/* ---- the unit task ---- */

static const UWORD rx_types[3] = { 0x0800, 0x0806, 0x86dd };

static void rx_submit(int i)
{
	struct IOSana2Req *io = rx[i].io;

	rx[i].state = 1;
	io->ios2_Req.io_Command = CMD_READ;
	io->ios2_Req.io_Flags = 0;
	io->ios2_PacketType = rx_types[i % 3];
	io->ios2_Data = &rx[i];
	SendIO((struct IORequest *)io);
}

/* a batch to the stack: the buffers are the stack's before the call (it
 * may give them back inside it), the refused tail reads again */
static void deliver(struct NetDevRxDesc *batch, int n)
{
	ULONG k, j;

	for (j = 0; j < (ULONG)n; j++)
		((struct rxbuf *)batch[j].nrd_Cookie)->state = 2;
	k = sops->nso_RxInput(stackctx, batch, (ULONG)n);
	for (j = k; j < (ULONG)n; j++) {
		rx_refused++;
		rx_submit((int)((struct rxbuf *)batch[j].nrd_Cookie - rx));
	}
}

int main(int argc, char **argv)
{
	struct TagItem bm[3];
	const char *devname = argc > 1 ? argv[1] : "uaenet.device";
	ULONG unit = argc > 2 ? (ULONG)atoi(argv[2]) : 2;
	struct NetDevRxDesc batch[BATCH];
	APTR done[NTX];
	int i, n, nd;

	stats_path = argc > 3 ? argv[3] : NULL;
	unit_task = FindTask(NULL);
	/* above the stack and its programs, as Alice68's alice68net.device
	 * (priority 5): a write that completes at once then reaches the stack
	 * (nso_TxDone) before ndo_TxSubmit has returned to it */
	SetTaskPri(unit_task, argc > 4 ? atoi(argv[4]) : 5);
	sig_kick = 1UL << AllocSignal(-1);
	s2port = CreateMsgPort();
	s2ctl = (struct IOSana2Req *)CreateIORequest(s2port, sizeof(struct IOSana2Req));
	bm[0].ti_Tag = S2_CopyToBuff;
	bm[0].ti_Data = (ULONG)copy_to;
	bm[1].ti_Tag = S2_CopyFromBuff;
	bm[1].ti_Data = (ULONG)copy_from;
	bm[2].ti_Tag = TAG_DONE;
	s2ctl->ios2_BufferManagement = bm;
	if (OpenDevice((STRPTR)devname, unit, (struct IORequest *)s2ctl, 0)) {
		Printf("fakenetdev: cannot open %s unit %ld\n", devname, (long)unit);
		return 20;
	}
	s2ctl->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
	DoIO((struct IORequest *)s2ctl);
	memcpy(mac, s2ctl->ios2_DstAddr, 6);      /* the factory address */
	memcpy(s2ctl->ios2_SrcAddr, mac, 6);
	s2ctl->ios2_Req.io_Command = S2_CONFIGINTERFACE;
	DoIO((struct IORequest *)s2ctl);
	s2ctl->ios2_Req.io_Command = S2_ONLINE;
	DoIO((struct IORequest *)s2ctl);
	for (i = 0; i < NRX; i++) {
		rx[i].io = AllocVec(sizeof(struct IOSana2Req), MEMF_PUBLIC | MEMF_CLEAR);
		*rx[i].io = *s2ctl;
		rx[i].io->ios2_Req.io_Message.mn_ReplyPort = s2port;
	}
	for (i = 0; i < NTX; i++) {
		tx[i].io = AllocVec(sizeof(struct IOSana2Req), MEMF_PUBLIC | MEMF_CLEAR);
		*tx[i].io = *s2ctl;
		tx[i].io->ios2_Req.io_Message.mn_ReplyPort = s2port;
	}

	dev = (struct Device *)MakeLibrary(vectors, NULL, NULL, sizeof(struct Device), 0);
	if (!dev) {
		Printf("fakenetdev: cannot make the device\n");
		return 20;
	}
	dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
	dev->dd_Library.lib_Node.ln_Name = "fakenet.device";
	dev->dd_Library.lib_IdString = "fakenet.device 4.0 (05.11.2026) netdev over SANA-II";
	dev->dd_Library.lib_Version = 4;
	dev->dd_Library.lib_Revision = 0;
	AddDevice(dev);
	Printf("fakenetdev: fakenet.device on %s unit %ld, %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", devname,
	       (long)unit, (long)mac[0], (long)mac[1], (long)mac[2], (long)mac[3], (long)mac[4], (long)mac[5]);
	Flush(Output());

	for (i = 0; i < NRX; i++)
		rx_submit(i);
	for (;;) {
		struct IOSana2Req *io;
		Wait((1UL << s2port->mp_SigBit) | sig_kick);
		nd = 0;
		n = 0;
		while ((io = (struct IOSana2Req *)GetMsg(s2port)) != NULL) {
			struct rxbuf *b = NULL;
			for (i = 0; i < NRX && !b; i++)
				if (rx[i].io == io)
					b = &rx[i];
			if (!b) {
				for (i = 0; i < NTX; i++)
					if (tx[i].io == io && tx[i].busy) {
						if (attached)
							done[nd++] = tx[i].cookie;
						if (io->ios2_Req.io_Error)
							tx_errors++;
						tx_frames++;
						Forbid();
						tx[i].busy = 0;
						txbusy--;
						Permit();
					}
				continue;
			}
			if (io->ios2_Req.io_Error || !started || !attached || n == BATCH) {
				if (n == BATCH && started && attached && !io->ios2_Req.io_Error) {
					/* deliver the full batch first */
					deliver(batch, n);
					n = 0;
				} else {
					if (io->ios2_Req.io_Error)
						rx_errors++;
					rx_submit((int)(b - rx));
					continue;
				}
			}
			rx_frames++;
			/* the Ethernet header from the request */
			memcpy(b->data, io->ios2_DstAddr, 6);
			memcpy(b->data + 6, io->ios2_SrcAddr, 6);
			b->data[12] = (UBYTE)(io->ios2_PacketType >> 8);
			b->data[13] = (UBYTE)io->ios2_PacketType;
			batch[n].nrd_Data = b->data;
			batch[n].nrd_Len = b->len;
			batch[n].nrd_Flags = ((io->ios2_Req.io_Flags & SANA2IOF_BCAST) ? 4 : 0) |
			                     ((io->ios2_Req.io_Flags & SANA2IOF_MCAST) ? 8 : 0);
			batch[n].nrd_CsumRaw = 0;
			batch[n].nrd_VlanTci = 0;
			batch[n].nrd_Cookie = b;
			n++;
		}
		if (n)
			deliver(batch, n);
		write_stats();
		if (nd)
			sops->nso_TxDone(stackctx, done, nd);
		if (link_due && attached) {
			struct NetDevLinkState l;
			link_due = 0;
			l.ndls_Flags = 3;
			l.ndls_SpeedMbps = 1000;
			sops->nso_LinkChange(stackctx, &l);
		}
		/* buffers the stack gave back read again */
		for (i = 0; i < NRX; i++) {
			int idle;
			Forbid();
			idle = rx[i].state == 0;
			Permit();
			if (idle)
				rx_submit(i);
		}
	}
	return 0;
}
