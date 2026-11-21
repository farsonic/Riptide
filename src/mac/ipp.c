/* src/mac/ipp.c - Riptide on the classic Mac OS: the MacTCP programming interface, the .IPP
 * driver's Control calls (MacTCP Programmer's Guide; MacTCP.h of Universal Interfaces 3).
 *
 * Every call is taken off the Device Manager's queue (drvr.S, "pending"), put on a list here, and
 * handled by the stack's deferred task, which completes it with mac_complete: ioResult, then the
 * caller's ioCompletion. Synchronous callers spin on ioResult as they do with MacTCP.
 *
 * ipctlGetAddr, ipctlEchoICMP and ipctlLAPStats are here; TCP, UDP and the resolver's lookups are
 * in mactcp.c; .IPP6, the IPv6 interface of the same model, in ipp6.c (installed and polled from here); RiptidePrefs' private calls (RTMAC_*) too, and RTMAC_API, the address of the calls
 * programs make directly (api_mac.c: the Riptide Command Terminal's socket interface).
 *
 * What a program asks for shows in the log (PiLog's Mac tab on Aurora68), so that a photograph of
 * it tells what a program that fails wanted: the first call of each kind ("TCPCreate (30)"), every
 * call that ends in an error, once per call and result, with its stream, every csCode that is not
 * supported, once, and the driver's Opens and refused Closes.
 *
 * The driver also stands in for MacTCP where programs look for it: Gestalt 'mtcp' answers 3 (MacTCP
 * 2.0, the MacTCP Programmer's Guide, "The Gestalt call"), and at the first Open the folders Apple's
 * DNR glue searches for the resolver are checked (dnr_check).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_bsd.h"
#include "rt_ip.h"
#include "rt_netif.h"
#include "rt_socket.h"
#include "mac.h"
#include "ipp.h"
#include "rt_ctl.h"
#include "rt_sysctl.h"
#include "prefs/mac_ipp_private.h"
/* include/rt/riptide_port.h's numbers (that header needs the Amiga's types) */
#define RTCMD_STATUS   1
#define RTCMD_COMMAND  3
#define RTCMD_TABLES   4
#define RTMSGF_TRUNCATED 1
#define RIPTIDE_TEXTLEN 1024

/* MacTCP csCodes */
#define ipctlGetAddr   15
#define ipctlEchoICMP  17
#define ipctlLAPStats  19
/* MacTCP results */
#define icmpEchoTimeoutErr -23035
#define insufficientResources -23009
#define controlErr     -17
#define statusErr      -18

static void *volatile incoming;                   /* calls handed over by Control, linked by qLink */
static struct rt_sockctx ctx;
static int ctx_ok;

/* ---- what programs ask for, in the log ------------------------------------------------------- */
static const char *cs_name(int cs)
{
	static const char *const names[] = {
		"UDPCreate", "UDPRead", "UDPBfrReturn", "UDPWrite", "UDPRelease", "UDPMaxMTUSize", "UDPStatus",
		"UDPMultiCreate", "UDPMultiSend", "UDPMultiRead", "TCPCreate", "TCPPassiveOpen", "TCPActiveOpen",
		"csCode 33", "TCPSend", "TCPNoCopyRcv", "TCPRcvBfrReturn", "TCPRcv", "TCPClose", "TCPAbort",
		"TCPStatus", "TCPExtendedStat", "TCPRelease", "TCPGlobalInfo" };
	if (cs >= 20 && cs <= 43)
		return names[cs - 20];
	switch (cs) {
	case ipctlGetAddr: return "ipctlGetAddr";
	case ipctlEchoICMP: return "ipctlEchoICMP";
	case ipctlLAPStats: return "ipctlLAPStats";
	case RTMAC_PING: return "Riptide ping";
	case RTMAC_PORT: return "Riptide port";
	case RTMAC_DNS: return "Riptide DNS lookup";
	case RTMAC_DNSCANCEL: return "Riptide DNS cancel";
	case 65: return "accRun";
	case RTMAC_API: return "Riptide API";
	}
	return "unknown";
}
static u8 cs_seen[64];                            /* csCodes 0..63 already logged once */
static struct { s16 cs, r; } err_seen[48];        /* call and result pairs already logged */
static void note_call(int cs)
{
	if (cs < 0 || cs >= 64 || cs_seen[cs])
		return;
	cs_seen[cs] = 1;
	rt_log(RT_LOG_INFO, ".IPP: first %s (%d)", cs_name(cs), cs);
}
/* complete a call: ioResult, then ioCompletion; an error is logged the first time a call ends in it */
void ipp_result(void *pb, int r)
{
	int cs = PB_CSCODE(pb);
	if (r < 0) {
		unsigned i;
		for (i = 0; i < sizeof err_seen / sizeof err_seen[0]; i++) {
			if (err_seen[i].cs == cs && err_seen[i].r == r)
				break;
			if (!err_seen[i].cs && !err_seen[i].r) {
				err_seen[i].cs = (s16)cs;
				err_seen[i].r = (s16)r;
				rt_log(RT_LOG_WARN, ".IPP: %s (%d) on stream %08lx: error %d", cs_name(cs), cs,
				       (unsigned long)PB_L(pb, 28), r);
				break;
			}
		}
	}
	mac_complete(pb, r);
}

/* the A5 of the program that made a create call (TCPCreate, UDPCreate, UDPMultiCreate, an echo):
   its notification routines are called with it, as MacTCP does. Kept from the Control entry,
   which runs in the caller's context, until the stack's task takes the call. */
#define A5_SLOTS 16
static struct { void *pb, *a5; } a5_of[A5_SLOTS];
void ipp_keep_a5(void *pb, void *a5)
{
	unsigned i;
	u16 sr;
	if ((u32)a5 & 1 || !a5)
		a5 = *(void **)0x904;                     /* CurrentA5 */
	sr = mac_irq_off();                           /* a completion routine may make a call meanwhile */
	for (i = 0; i < A5_SLOTS; i++)
		if (!a5_of[i].pb || a5_of[i].pb == pb) {
			a5_of[i].a5 = a5;
			a5_of[i].pb = pb;
			break;
		}
	mac_irq_restore(sr);
}
void *ipp_creator_a5(void *pb)
{
	unsigned i;
	for (i = 0; i < A5_SLOTS; i++)
		if (a5_of[i].pb == pb) {
			u16 sr = mac_irq_off();
			void *a5 = a5_of[i].a5;
			a5_of[i].pb = NULL;
			mac_irq_restore(sr);
			return a5;
		}
	return *(void **)0x904;
}

/* ---- ipctlEchoICMP --------------------------------------------------------------------------- */
#define ECHOES 8
struct echo { void *pb, *a5; u32 own_lo, own_hi; u16 id, seq; u32 deadline_ms; u8 reply[256]; };
static struct echo echoes[ECHOES];
static int icmp_fd = -1;
static u16 echo_seq;
static u16 cksum(const u8 *p, u32 n)
{
	u32 s = 0;
	for (; n > 1; p += 2, n -= 2) s += (u32)p[0] << 8 | p[1];
	if (n) s += (u32)p[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (u16)~s;
}
static int echo_start(void *pb)
{
	struct echo *e = NULL;
	for (int i = 0; i < ECHOES; i++) if (!echoes[i].pb) { e = &echoes[i]; break; }
	if (!e) return insufficientResources;
	if (icmp_fd < 0) {
		icmp_fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_RAW, RT_IPPROTO_ICMP);
		if (icmp_fd < 0) return insufficientResources;
	}
	u32 dest = PB_L(pb, 28);
	u16 dlen = PB_W(pb, 32);
	const u8 *data = PB_P(pb, 34);
	u16 timeout = PB_W(pb, 38);
	u8 pkt[8 + 200];
	if (dlen > 200) dlen = 200;
	e->id = 0x5254; e->seq = ++echo_seq;
	pkt[0] = 8; pkt[1] = 0; pkt[2] = pkt[3] = 0;
	rt_put_be16(pkt + 4, e->id); rt_put_be16(pkt + 6, e->seq);
	if (dlen && data) memcpy(pkt + 8, data, dlen);
	rt_put_be16(pkt + 2, cksum(pkt, 8u + dlen));
	struct rt_sockaddr_in to;
	memset(&to, 0, sizeof to);
	to.sin_len = sizeof to; to.sin_family = RT_AF_INET; to.sin_addr = dest;
	int r = rt_api_sendto(&ctx, icmp_fd, pkt, 8u + dlen, RT_MSG_DONTWAIT, &to, sizeof to);
	if (r < 0) return -23033;                     /* ipDestDeadErr: no route, no ARP answer */
	e->pb = pb;
	e->a5 = ipp_creator_a5(pb);
	e->own_lo = e->own_hi = 0;
	e->deadline_ms = rt_now_ms() + (timeout ? timeout : 5) * 1000u;
	/* icmpEchoInfo.echoRequestOut sits where userDataPtr is (offset 50): keep the latter first */
	PB_L(pb, 68) = PB_L(pb, 50);
	PB_L(pb, 50) = mac_ticks();
	return 1;
}
static void echo_poll(void)
{
	if (icmp_fd < 0) return;
	for (;;) {
		u8 buf[300];
		int n = rt_api_recvfrom(&ctx, icmp_fd, buf, sizeof buf, RT_MSG_DONTWAIT, NULL, NULL);
		if (n <= 0) break;
		u32 ihl = (u32)(buf[0] & 15) * 4u;
		if ((u32)n < ihl + 8 || buf[ihl] != 0) continue;          /* echo replies only */
		u16 id = rt_get_be16(buf + ihl + 4), seq = rt_get_be16(buf + ihl + 6);
		for (int i = 0; i < ECHOES; i++) {
			struct echo *e = &echoes[i];
			if (!e->pb || e->id != id || e->seq != seq) continue;
			u32 dl = (u32)n - ihl - 8; if (dl > sizeof e->reply) dl = sizeof e->reply;
			memcpy(e->reply, buf + ihl + 8, dl);
			PB_W(e->pb, 58) = (u16)dl; PB_P(e->pb, 60) = e->reply;   /* echoedData */
			void *pb = e->pb;
			u32 ud = PB_L(pb, 68), out = PB_L(pb, 50);
			e->pb = NULL;
			PB_L(pb, 50) = out; PB_L(pb, 54) = mac_ticks(); PB_L(pb, 68) = ud;
			void *notify = PB_P(pb, 46);
			PB_RESULT(pb) = 0;
			if (notify) mac_call_notify(notify, pb, e->a5);
			mac_complete(pb, 0);
		}
	}
	u32 now = rt_now_ms();
	for (int i = 0; i < ECHOES; i++)
		if (echoes[i].pb && (s32)(now - echoes[i].deadline_ms) >= 0) {
			void *pb = echoes[i].pb; u32 ud = PB_L(pb, 68), out = PB_L(pb, 50);
			echoes[i].pb = NULL;
			PB_W(pb, 58) = 0; PB_P(pb, 60) = NULL;
			PB_L(pb, 50) = out; PB_L(pb, 54) = 0; PB_L(pb, 68) = ud;
			void *notify = PB_P(pb, 46);
			PB_RESULT(pb) = icmpEchoTimeoutErr;
			if (notify) mac_call_notify(notify, pb, echoes[i].a5);
			mac_complete(pb, icmpEchoTimeoutErr);
		}
}

/* ---- RiptidePrefs: the RIPTIDE port of the Amiga, as private Control calls --------------------- */
/* RTCMD_TABLES: the tables numbered in list (decimal, spaces between) into out, each as a number,
 * a length and the dump, 4-byte aligned (src/amiga/stackproc.c does the same for its port) */
static u32 read_tables(const char *list, char *out, u32 olen, u32 *flags)
{
	s32 mib[3];
	u32 used = 0;
	*flags = 0;
	mib[0] = 4;
	mib[1] = RT_CTL_MIB_RIPTIDE;
	while (*list) {
		const char *e;
		unsigned long w;
		u32 *hd, dl;
		while (*list == ' ') list++;
		w = rt_strtoul(list, &e, 10);
		if (e == list) break;
		list = e;
		if (used + 8 > olen) { *flags = RTMSGF_TRUNCATED; break; }
		hd = (u32 *)(out + used);
		dl = olen - used - 8;
		mib[2] = (s32)w;
		if (rt_sysctl_bymib(mib, 3, out + used + 8, &dl, NULL, 0) != 0) { dl = 0; *flags = RTMSGF_TRUNCATED; }
		hd[0] = (u32)w;
		hd[1] = dl;
		used += 8 + ((dl + 3u) & ~3u);
		if (used > olen) used = olen;
	}
	return used;
}

static int port_call(void *pb)
{
	struct rtmac_port_pb *p = pb;
	int r;
	if (!p->reply || p->replysize < 2) return controlErr;
	if (p->text && p->textlen) p->text[p->textlen - 1] = 0;
	p->flags = 0;
	switch (p->cmd) {
	case RTCMD_COMMAND: {
		/* the line may share the reply's buffer (a message without one): copy it first */
		char line[RIPTIDE_TEXTLEN];
		rt_strlcpy(line, p->text ? p->text : "", sizeof line);
		p->reply[0] = 0;
		rt_lock();
		r = rt_ctl_command(line, p->reply, p->replysize, mac_config_unknown);
		rt_unlock();
		p->result = r < 0 ? -r : 0;
		p->replylen = (u32)strlen(p->reply);
		return 0;
	}
	case RTCMD_TABLES:
		rt_lock();
		{
		u32 fl = 0;
		p->replylen = read_tables(p->text ? p->text : "", p->reply, p->replysize, &fl);
		p->flags = fl;
		}
		rt_unlock();
		p->result = 0;
		return 0;
	case RTCMD_STATUS:
		rt_snprintf(p->reply, p->replysize, "Riptide for the Mac: %s\n", mac_nif ? mac_nif->name : "no network");
		p->result = 0;
		p->replylen = (u32)strlen(p->reply);
		return 0;
	default:
		/* RTCMD_RESTART and RTCMD_QUIT: not on the Mac yet */
		rt_strlcpy(p->reply, "unknown command\n", p->replysize);
		p->result = RT_EINVAL;
		p->replylen = (u32)strlen(p->reply);
		return 0;
	}
}

/* ---- dispatch -------------------------------------------------------------------------------- */
/* ipctlLAPStats: the link's description (MacTCP.h's LAPStats). The pointer at 28 is the caller's
   structure to fill, or, when it is 0, it comes back pointing at the driver's own. */
static int lap_stats(void *pb)
{
	static u8 mine[24];
	static const u8 name[] = "Ethernet";
	u8 *l = PB_P(pb, 28) ? PB_P(pb, 28) : mine;
	memset(l, 0, 24);                                     /* sizeof (LAPStats) */
	PB_W(l, 0) = 1;                                       /* ifType: Ethernet */
	PB_P(l, 2) = (void *)name;                            /* ifString */
	PB_W(l, 6) = (u16)(mac_nif ? mac_nif->mtu : 1500);   /* ifMaxMTU */
	PB_L(l, 8) = 10000000;                                /* ifSpeed */
	PB_W(l, 12) = 6;                                      /* ifPhyAddrLength */
	PB_P(l, 14) = mac_nif ? (void *)mac_nif->hwaddr : NULL;
	PB_P(l, 18) = NULL;                                   /* the ARP table: not given */
	PB_W(l, 22) = 9;                                      /* slotNumber */
	PB_P(pb, 28) = l;
	return 0;
}

static int handle(void *pb)
{
	int cs = PB_CSCODE(pb);
	note_call(cs);
	switch (cs) {
	case ipctlGetAddr:
		PB_L(pb, 28) = mac_nif ? mac_nif->addr : 0;
		PB_L(pb, 32) = mac_nif ? mac_nif->netmask : 0;
		return 0;
	case ipctlEchoICMP:
		return echo_start(pb);
	case ipctlLAPStats:
		return lap_stats(pb);
	case RTMAC_PING:
		PB_L(pb, 28) = RTMAC_MAGIC;
		return 0;
	case RTMAC_PORT:
		return port_call(pb);
	case RTMAC_API:
		/* the calls programs make in their own context (api_mac.c) */
		PB_L(pb, 28) = (u32)&mac_api;
		return 0;
	default:
		if ((cs >= 20 && cs <= 43) || cs == RTMAC_DNS || cs == RTMAC_DNSCANCEL) {
			int r = mactcp_call(pb, &ctx);
			if (r != -17)
				return r;
		}
		{
			static u8 unsup[64];
			if (cs < 0 || cs >= 64 || !unsup[cs]) {
				if (cs >= 0 && cs < 64)
					unsup[cs] = 1;
				rt_log(RT_LOG_WARN, ".IPP csCode %d (%s) is not supported yet: controlErr", cs, cs_name(cs));
			}
		}
		return controlErr;
	}
}
/* the calls answered at once when they come from the stack's own context (ipp_control_a5) */
static int in_poll;
int ipp_in_poll(void) { return in_poll; }       /* ipp6.c: the same rule for .IPP6's immediate calls */
static int quick(int cs)
{
	return cs == ipctlGetAddr || cs == 40 || cs == 43 || cs == 25 || cs == 36 || cs == 22;   /* TCPStatus,
	       TCPGlobalInfo, UDPMaxMTUSize, TCPRcvBfrReturn, UDPBfrReturn */
}

/* ---- programs that quit without releasing what they made ------------------------------------- */
/* A MacTCP program that quits (or crashes) without TCPRelease leaves streams whose receive buffer,
   notification routine and waiting parameter blocks are in a partition the Process Manager gives
   to the next program: data or a notification would then land in that program's memory. MacTCP
   lets that happen. Riptide asks for time (dNeedTime: the Device Manager's SystemTask sends accRun
   every sixth of a second, in a program's context, where the Process Manager may be called), lists
   the partitions of the processes alive, and releases what belongs to a partition that is gone:
   streams (by their creator's A5), lookups and echoes (by their parameter block), never completing
   their calls. A completion or notification that falls due between the program's death and that
   check can still run in the freed partition (the testbed's Finder once stopped with "bad F-Line
   instruction" that way): programs must release their streams, as with MacTCP. Patching
   _ExitToShell to release them at the quit was tried on 19 November 2026 and left out: a patch made
   at startup is replaced by the Process Manager's, one made at the first accRun is not there for
   the other programs (System 7.1 keeps the trap table per program), and one made in every program
   stopped the Finder with "bad F-Line instruction". */
#define accRun 65
static struct mac_client reaper;                  /* the core, taken in a program's context */
static void reap(void)
{
	u32 lo[32], hi[32];
	u8 psn[8], info[60];
	int n = 0, i, k;
	memset(psn, 0, sizeof psn);                   /* kNoProcess */
	while (n < 32 && mac_next_process(psn) == 0) {
		memset(info, 0, sizeof info);
		PB_L(info, 0) = sizeof info;              /* processInfoLength; no name, no FSSpec */
		if (mac_process_info(psn, info) == 0 && PB_L(info, 32)) {
			lo[n] = PB_L(info, 28);               /* processLocation */
			hi[n] = lo[n] + PB_L(info, 32);       /* processSize */
			n++;
		}
	}
	if (!n)
		return;                                   /* no Process Manager yet */
	mac_core_enter(&reaper);
	mactcp_reap(&ctx, lo, hi, n);
	ipp6_reap(lo, hi, n);                         /* .IPP6's streams and lookups: the same check */
	for (i = 0; i < ECHOES; i++) {
		struct echo *e = &echoes[i];
		u32 a = (u32)e->a5;
		if (!e->pb)
			continue;
		if (!e->own_lo) {
			e->own_lo = 1;                        /* not a program's: kept */
			for (k = 0; k < n; k++)
				if (a >= lo[k] && a < hi[k]) {
					e->own_lo = lo[k];
					e->own_hi = hi[k];
				}
			continue;
		}
		if (e->own_lo == 1)
			continue;
		for (k = 0; k < n && !(lo[k] == e->own_lo && hi[k] == e->own_hi); k++)
			;
		if (k == n) {
			rt_log(RT_LOG_WARN, ".IPP: an echo of a program that has quit: dropped");
			e->pb = NULL;
		}
	}
	mac_core_leave();
}

/* the Control routine (any level): hand the call over to the stack's context */
int ipp_control(void *pb)
{
	return ipp_control_a5(pb, NULL, *(void **)0x904);
}
int ipp_control_a5(void *pb, void *dce, void *a5)
{
	int cs = PB_CSCODE(pb);
	(void)dce;
	if (cs == accRun) {
		reap();
		return 0;
	}
	if (in_poll && !(PB_IOTRAP(pb) & 0x400) && quick(cs)) {
		/* a synchronous call from a completion or notification routine, which the stack's task is
		   running: queued, it would wait for that task for ever. MacTCP answers these at once, so
		   programs make them there; so does Riptide (they only read, or give a buffer back). */
		int r = handle(pb);
		if (r != 1)
			return r;
	}
	if (PB_IOTRAP(pb) & 0x200)
		return controlErr;                        /* immediate calls cannot wait for the stack */
	if (cs == ipctlEchoICMP || cs == 20 || cs == 27 || cs == 30)
		ipp_keep_a5(pb, a5);
	u16 sr = mac_irq_off();
	PB_QLINK(pb) = NULL;
	void **pp = (void **)&incoming;
	while (*pp) pp = &PB_QLINK(*pp);
	*pp = pb;
	mac_irq_restore(sr);
	mac_schedule();
	return 1;
}
int ipp_status(void *pb) { (void)pb; return statusErr; }

/* ---- Open and Close -------------------------------------------------------------------------- */
/* Close: refused, as MacTCP refuses it ("PBClose returns a closeErr error code"): the driver stays
   open for every program, whichever of them quits first. Only counted here (this runs in the
   caller's context); the stack's task logs it. */
static volatile u32 closes, closes_logged;
int ipp_close(void *pb, void *dce)
{
	(void)pb; (void)dce;
	closes++;
	mac_schedule();
	return -24;                                   /* closErr */
}

/* the folders Apple's DNR glue (dnr.c, which every MacTCP program carries) searches for the file
   holding 'dnrp', in its order: the Control Panels folder for type 'cdev' creator 'ztcp', the System
   Folder for 'cdev' 'mtcp', the Control Panels folder for 'cdev' 'mtcp', each folder's files by
   index (name order), the first one holding 'dnrp' (1991 glue); the oldest glue (1988) only the
   System Folder for 'cdev' 'mtcp', the first such file whether it holds 'dnrp' or not. So:
   - Riptide DNR files made before 19 November 2026 have creator 'ztcp', which no glue looks for
     in the System Folder (OpenResolver failed, and StrToAddr answers notOpenErr, -28, even for a
     dotted address): changed to 'mtcp' here;
   - any other file the glue could take first is moved into the System Folder's "Disabled
     (Riptide)" folder, which the glue does not search: MacTCP's "MacTCP DNR", which MacTCP leaves
     in the System Folder and which holds MacTCP's own 'dnrp' (it comes before "Riptide DNR", so
     every glue loads it; it then asks .IPP for MacTCP's private settings, csCode 16, gets
     controlErr, and OpenResolver fails: notOpenErr, -28, from StrToAddr), and a MacTCP control
     panel left in the Control Panels folder (the same resolver, found first).
   Everything done is logged. */
#define T_CDEV 0x63646576UL
#define C_ZTCP 0x7A746370UL
#define C_MTCP 0x6D746370UL
#define ASIDE_MAX 8
static const char aside_folder[] = "Disabled (Riptide)";
static int pstr_is(const u8 *p, const char *c)
{
	u32 n = (u32)strlen(c);
	return p[0] == n && !memcmp(p + 1, c, n);
}
static void pstr_to(char *c, const u8 *p)
{
	memcpy(c, p + 1, p[0]);
	c[p[0]] = 0;
}
/* the folder things are moved into: its directory ID, made if need be; 0 if there is none */
static long aside_dir(short vref, long sysdir)
{
	u8 pb[108], name[32];
	name[0] = (u8)(sizeof aside_folder - 1);
	memcpy(name + 1, aside_folder, sizeof aside_folder - 1);
	memset(pb, 0, sizeof pb);
	PB_P(pb, 18) = name;
	PB_W(pb, 22) = (u16)vref;
	PB_L(pb, 48) = (u32)sysdir;
	if (mac_pb(pb, 0x0006A260) == 0)                    /* _DirCreate: ioDirID comes back */
		return (long)PB_L(pb, 48);
	memset(pb, 0, sizeof pb);                           /* there already: _GetCatInfo by name */
	PB_P(pb, 18) = name;
	PB_W(pb, 22) = (u16)vref;
	PB_L(pb, 48) = (u32)sysdir;
	if (mac_pb(pb, 0x0009A260) == 0 && (pb[30] & 0x10))
		return (long)PB_L(pb, 48);
	return 0;
}
static int dnr_scan(short vref, long dir, const char *where, int sysfolder, long sysdir)
{
	u8 pb[108], name[64], aside[ASIDE_MAX][32];
	int i, n_aside = 0, found = 0;
	for (i = 1; i < 500; i++) {
		u32 type, creator;
		memset(pb, 0, sizeof pb);
		PB_P(pb, 18) = name;
		PB_W(pb, 22) = (u16)vref;
		PB_W(pb, 28) = (u16)i;                    /* ioFDirIndex */
		PB_L(pb, 48) = (u32)dir;
		if (mac_pb(pb, 0xA20C) != 0)              /* _HGetFileInfo */
			break;
		type = PB_L(pb, 32);
		creator = PB_L(pb, 36);
		if (type != T_CDEV || (creator != C_ZTCP && creator != C_MTCP))
			continue;
		if (pstr_is(name, "Riptide DNR")) {
			found = 1;
			if (sysfolder && creator == C_ZTCP) {
				PB_W(pb, 28) = 0;
				PB_L(pb, 48) = (u32)dir;
				PB_L(pb, 36) = C_MTCP;
				if (mac_pb(pb, 0xA20D) == 0)      /* _HSetFileInfo */
					rt_log(RT_LOG_WARN, "resolver: Riptide DNR in the %s had creator 'ztcp', which Apple's DNR glue "
					       "never looks for there: changed to 'mtcp'", where);
				else
					rt_log(RT_LOG_ERR, "resolver: Riptide DNR in the %s has creator 'ztcp' (MacTCP programs will "
					       "not find it: notOpenErr) and could not be changed", where);
			} else {
				rt_log(RT_LOG_INFO, "resolver: Riptide DNR in the %s ('%s')", where, creator == C_MTCP ? "mtcp" : "ztcp");
			}
		} else if ((creator == C_MTCP || !sysfolder) && n_aside < ASIDE_MAX && name[0] < 32) {
			/* a file the glue could take before Riptide DNR: moved aside once the scan is done
			   (moving it now would change the indexes) */
			memcpy(aside[n_aside++], name, (u32)name[0] + 1);
		}
	}
	for (i = 0; i < n_aside; i++) {
		char cname[32];
		long to = aside_dir(vref, sysdir);
		int r = -1;
		pstr_to(cname, aside[i]);
		if (to) {
			memset(pb, 0, sizeof pb);
			PB_P(pb, 18) = aside[i];
			PB_W(pb, 22) = (u16)vref;
			PB_P(pb, 28) = NULL;                  /* ioNewName: the folder is ioNewDirID */
			PB_L(pb, 36) = (u32)to;
			PB_L(pb, 48) = (u32)dir;
			r = mac_pb(pb, 0x0005A260);           /* _CatMove */
		}
		if (r == 0)
			rt_log(RT_LOG_WARN, "resolver: '%s' moved from the %s into System Folder:%s, where Apple's DNR "
			       "glue does not look, so that MacTCP programs find Riptide DNR", cname, where, aside_folder);
		else
			rt_log(RT_LOG_ERR, "resolver: '%s' in the %s may be found by MacTCP programs before Riptide DNR, "
			       "and could not be moved (%d): move it out of the folder", cname, where, r);
	}
	return found;
}
static void dnr_check(void)
{
	short vref, cvref;
	long dir, cdir;
	int found = 0;
	if (mac_findfolder(-32768, 0x6D616373 /* 'macs' */, 0, &vref, &dir) != 0)
		return;
	found |= dnr_scan(vref, dir, "System Folder", 1, dir);
	if (mac_findfolder(-32768, 0x6374726C /* 'ctrl' */, 0, &cvref, &cdir) == 0 && cvref == vref)
		found |= dnr_scan(cvref, cdir, "Control Panels folder", 0, dir);
	if (!found)
		rt_log(RT_LOG_ERR, "resolver: no Riptide DNR in the System Folder: MacTCP programs cannot resolve names, "
		       "and StrToAddr answers notOpenErr (-28) even for a dotted address");
}

/* after every Open that succeeded (the INIT's at startup, then each program's): the first time, the
   Gestalt selector and the resolver check */
void ipp_opened(void)
{
	static u32 opens;
	if (opens++ == 0) {
		short r = mac_newgestalt(0x6D746370 /* 'mtcp' */, (void *)mac_gestalt_mtcp);
		if (r)
			rt_log(RT_LOG_WARN, "Gestalt 'mtcp' not installed (%d)", r);
		dnr_check();
		ipp6_install();                               /* .IPP6 beside .IPP (docs/IPP6.md) */
	} else if (opens <= 16) {
		rt_log(RT_LOG_INFO, ".IPP: opened by a program (open %lu)", (unsigned long)opens);
	}
}

void ipp_poll(void)
{
	if (!ctx_ok) { if (rt_ctx_init(&ctx, 64, NULL) < 0) return; ctx_ok = 1; }
	in_poll = 1;
	if (closes != closes_logged) {
		closes_logged = closes;
		if (closes_logged <= 16)
			rt_log(RT_LOG_INFO, ".IPP: Close refused (closErr, as MacTCP does): the driver stays open (close %lu)",
			       (unsigned long)closes_logged);
	}
	for (;;) {
		u16 sr = mac_irq_off();
		void *pb = incoming;
		if (pb) incoming = PB_QLINK(pb);
		mac_irq_restore(sr);
		if (!pb) break;
		int r = handle(pb);
		if (r != 1) ipp_result(pb, r);
	}
	echo_poll();
	mactcp_poll(&ctx);
	ipp6_poll();                                  /* .IPP6's calls, in the same round (ipp6.c) */
	in_poll = 0;
}
