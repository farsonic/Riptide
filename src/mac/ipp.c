/* src/mac/ipp.c - Riptide on the classic Mac OS: the MacTCP programming interface, the .IPP
 * driver's Control calls (MacTCP Programmer's Guide; MacTCP.h of Universal Interfaces 3).
 *
 * Every call is taken off the Device Manager's queue (drvr.S, "pending"), put on a list here, and
 * handled by the stack's deferred task, which completes it with mac_complete: ioResult, then the
 * caller's ioCompletion. Synchronous callers spin on ioResult as they do with MacTCP.
 *
 * ipctlGetAddr and ipctlEchoICMP are here; TCP, UDP and the resolver's lookups are in mactcp.c;
 * RiptidePrefs' private calls (RTMAC_*) too. Anything else answers "not supported yet" and is
 * logged with its csCode.
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
/* MacTCP results */
#define icmpEchoTimeoutErr -23035
#define insufficientResources -23009
#define controlErr     -17
#define statusErr      -18

static void *volatile incoming;                   /* calls handed over by Control, linked by qLink */
static struct rt_sockctx ctx;
static int ctx_ok;

/* ---- ipctlEchoICMP --------------------------------------------------------------------------- */
#define ECHOES 8
struct echo { void *pb; u16 id, seq; u32 deadline_ms; u8 reply[256]; };
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
			if (notify) mac_call_pascal1(notify, (u32)pb, pb);
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
			if (notify) mac_call_pascal1(notify, (u32)pb, pb);
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
static int handle(void *pb)
{
	int cs = PB_CSCODE(pb);
	switch (cs) {
	case ipctlGetAddr:
		PB_L(pb, 28) = mac_nif ? mac_nif->addr : 0;
		PB_L(pb, 32) = mac_nif ? mac_nif->netmask : 0;
		return 0;
	case ipctlEchoICMP:
		return echo_start(pb);
	case RTMAC_PING:
		PB_L(pb, 28) = RTMAC_MAGIC;
		return 0;
	case RTMAC_PORT:
		return port_call(pb);
	default:
		if ((cs >= 20 && cs <= 43) || cs == RTMAC_DNS) {
			int r = mactcp_call(pb, &ctx);
			if (r != -17)
				return r;
		}
		rt_log(RT_LOG_WARN, ".IPP csCode %d is not supported yet", cs);
		return controlErr;
	}
}
/* the Control routine (any level): hand the call over to the stack's context */
int ipp_control(void *pb)
{
	if (PB_IOTRAP(pb) & 0x200)
		return controlErr;                        /* immediate calls cannot wait for the stack */
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
void ipp_poll(void)
{
	if (!ctx_ok) { if (rt_ctx_init(&ctx, 64, NULL) < 0) return; ctx_ok = 1; }
	for (;;) {
		u16 sr = mac_irq_off();
		void *pb = incoming;
		if (pb) incoming = PB_QLINK(pb);
		mac_irq_restore(sr);
		if (!pb) break;
		int r = handle(pb);
		if (r != 1) mac_complete(pb, r);
	}
	echo_poll();
	mactcp_poll(&ctx);
}
