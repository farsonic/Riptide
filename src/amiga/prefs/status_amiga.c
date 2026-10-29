/*
 * Riptide - RiptidePrefs: reading Riptide's tables for the Status page,
 * once a second while the page is shown. Riptide 0.23 and later answer
 * one RTCMD_TABLES message on the RIPTIDE port with all of them, which
 * needs no library (opening and closing riptide.library costs more than
 * the reads on a 68020, and an open library would keep Riptide from
 * quitting). Older versions: bsdsocket.library and riptide.library are
 * opened, the tables read with RiptideSysCtl() and both closed again.
 * Nothing is read when the RIPTIDE port is missing.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_status.h"

/* The tables are read into buffers kept from one second to the next (one
 * call each; the size is asked again only when a table outgrew its
 * buffer). The routes, IPv6 addresses and name servers change seldom:
 * they are read every fifth time, their last read used in between. */
enum { D_STATUS, D_IFACES, D_TCP, D_TCPST, D_IPST, D_ADDRS6, D_ROUTES, D_DNS, D_N };
#define D_SLOW D_ADDRS6         /* this one and those after it: every fifth read */
static const int what[D_N] = { RT_CTL_STATUS, RT_CTL_IFACES, RT_CTL_TCPINFO, RT_CTL_TCPSTAT, RT_CTL_IPSTAT,
                               RT_CTL_ADDRS6, RT_CTL_ROUTES, RT_CTL_DNS };
static u8 *buf[D_N];
static u32 cap[D_N];
static struct rt_ctl_hdr hdr[D_N];
static int have[D_N];
static u32 reads;

static int fetch(int k)
{
	LONG mib[3], len;
	int tries;

	mib[0] = 4;             /* CTL_NET */
	mib[1] = RT_CTL_MIB_RIPTIDE;
	mib[2] = what[k];
	for (tries = 0; tries < 3; tries++) {
		if (!cap[k]) {
			len = 0;
			if (RiptideSysCtl(mib, 3, NULL, &len, NULL, 0) != 0 || len <= 0)
				return -1;
			/* room to grow: tables change between reads */
			cap[k] = (u32)len + (u32)len / 2 + 512;
			buf[k] = AllocVec(cap[k], MEMF_ANY | MEMF_CLEAR);
			if (!buf[k]) {
				cap[k] = 0;
				return -1;
			}
		}
		len = (LONG)cap[k];
		if (RiptideSysCtl(mib, 3, buf[k], &len, NULL, 0) == 0)
			break;
		/* too small (or gone): ask the size again */
		FreeVec(buf[k]);
		buf[k] = NULL;
		cap[k] = 0;
	}
	if (!buf[k] || len < (LONG)sizeof(hdr[k]))
		return -1;
	memcpy(&hdr[k], buf[k], sizeof(hdr[k]));
	if (hdr[k].version != RT_CTL_VERSION || hdr[k].recsize == 0 ||
	    (u32)len < sizeof(hdr[k]) + (u32)hdr[k].count * hdr[k].recsize)
		return -1;
	return 0;
}

/* keep the dump of table k (len bytes at d) in its buffer */
static int store(int k, const u8 *d, u32 len)
{
	if (len < sizeof(hdr[k]))
		return -1;
	if (cap[k] < len) {
		if (buf[k])
			FreeVec(buf[k]);
		cap[k] = len + len / 2 + 256;
		buf[k] = AllocVec(cap[k], MEMF_ANY);
		if (!buf[k]) {
			cap[k] = 0;
			return -1;
		}
	}
	memcpy(buf[k], d, len);
	memcpy(&hdr[k], d, sizeof(hdr[k]));
	if (hdr[k].version != RT_CTL_VERSION || hdr[k].recsize == 0 ||
	    len < sizeof(hdr[k]) + (u32)hdr[k].count * hdr[k].recsize)
		return -1;
	return 0;
}

/* the port: one message and its reply buffer, made on the first read */
#define REPLY_SIZE 65536
static struct MsgPort *rport;
static struct RiptideMsg *rmsg;
static u8 *rbuf;
static int port_old;            /* Riptide answered EINVAL: use the library */

/* The tables of mask (bit k: table k) through the port. 0, or -1 when
 * this Riptide cannot (then port_old is set) or is gone. */
static int port_read(u32 mask)
{
	struct MsgPort *port;
	char *t;
	u32 off = 0;
	int k;

	if (!rport && !(rport = CreateMsgPort()))
		return -1;
	if (!rmsg && !(rmsg = AllocVec(sizeof(*rmsg), MEMF_PUBLIC | MEMF_CLEAR)))
		return -1;
	if (!rbuf && !(rbuf = AllocVec(REPLY_SIZE, MEMF_PUBLIC)))
		return -1;
	memset(rmsg, 0, sizeof(*rmsg));
	rmsg->msg.mn_Node.ln_Type = NT_MESSAGE;
	rmsg->msg.mn_ReplyPort = rport;
	rmsg->msg.mn_Length = sizeof(*rmsg);
	rmsg->cmd = RTCMD_TABLES;
	rmsg->reply = (STRPTR)rbuf;
	rmsg->replysize = REPLY_SIZE;
	t = rmsg->text;
	for (k = 0; k < D_N; k++)
		if (mask & (1u << k))
			t += rt_snprintf(t, 8, "%d ", what[k]);
	Forbid();
	port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
	if (port)
		PutMsg(port, &rmsg->msg);
	Permit();
	if (!port)
		return -1;
	WaitPort(rport);
	GetMsg(rport);
	if (rmsg->result) {
		if (rmsg->result == RT_EINVAL)
			port_old = 1;
		return -1;
	}
	/* number, length, dump; 4-byte aligned */
	while (off + 8 <= rmsg->replylen && off + 8 <= REPLY_SIZE) {
		u32 w = *(u32 *)(rbuf + off), len = *(u32 *)(rbuf + off + 4);
		if (off + 8 + len > REPLY_SIZE)
			break;
		for (k = 0; k < D_N; k++)
			if ((u32)what[k] == w && (mask & (1u << k)))
				have[k] = len && store(k, rbuf + off + 8, len) == 0;
		off += 8 + ((len + 3u) & ~3u);
	}
	return 0;
}

void rp_status_free(void)
{
	int k;

	for (k = 0; k < D_N; k++) {
		if (buf[k])
			FreeVec(buf[k]);
		buf[k] = NULL;
		cap[k] = 0;
		have[k] = 0;
	}
	reads = 0;
	if (rbuf)
		FreeVec(rbuf);
	if (rmsg)
		FreeVec(rmsg);
	if (rport)
		DeleteMsgPort(rport);
	rbuf = NULL;
	rmsg = NULL;
	rport = NULL;
	port_old = 0;
}

/* milliseconds from dos.library's clock (1/50 s steps are enough for rates) */
static u32 now_ms(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return (u32)ds.ds_Days * 86400000u + (u32)ds.ds_Minute * 60000u + (u32)ds.ds_Tick * 20u;
}

int rp_status_read(struct rp_status_model *m)
{
	struct rp_st_input in;
	int k;

	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		if (m->running)
			rp_status_stopped(m);
		rp_status_free();
		return 0;
	}
	{
		u32 mask = 0;
		for (k = 0; k < D_N; k++)
			if (k < D_SLOW || reads % 5 == 0 || !have[k])
				mask |= 1u << k;
		if (port_old || port_read(mask) < 0) {
			if (!port_old) {
				if (m->running)
					rp_status_stopped(m);
				return 0;
			}
			/* an older Riptide: through the library */
			SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
			RiptideBase = SocketBase ? OpenLibrary((CONST_STRPTR)RIPTIDE_LIB_NAME, 1) : NULL;
			if (!RiptideBase) {
				if (SocketBase)
					CloseLibrary(SocketBase);
				SocketBase = NULL;
				if (m->running)
					rp_status_stopped(m);
				rp_status_free();
				return 0;
			}
			for (k = 0; k < D_N; k++)
				if (mask & (1u << k))
					have[k] = fetch(k) == 0;
			CloseLibrary(RiptideBase);
			CloseLibrary(SocketBase);
			RiptideBase = SocketBase = NULL;
		}
		reads++;
	}
	memset(&in, 0, sizeof(in));
	in.now_ms = now_ms();
#define RECS(k) (have[k] ? buf[k] + sizeof(struct rt_ctl_hdr) : NULL)
#define ONE(k) (have[k] && hdr[k].count)
	if (ONE(D_STATUS)) {
		in.st = (const struct rt_ctl_status *)RECS(D_STATUS);
		in.st_len = hdr[D_STATUS].recsize;
	}
	in.ifaces = RECS(D_IFACES);
	in.nifaces = have[D_IFACES] ? hdr[D_IFACES].count : 0;
	in.ifrec = hdr[D_IFACES].recsize;
	in.addrs6 = RECS(D_ADDRS6);
	in.naddrs6 = have[D_ADDRS6] ? hdr[D_ADDRS6].count : 0;
	in.a6rec = hdr[D_ADDRS6].recsize;
	in.routes = RECS(D_ROUTES);
	in.nroutes = have[D_ROUTES] ? hdr[D_ROUTES].count : 0;
	in.rrec = hdr[D_ROUTES].recsize;
	in.tcp = RECS(D_TCP);
	in.ntcp = have[D_TCP] ? hdr[D_TCP].count : 0;
	in.trec = hdr[D_TCP].recsize;
	if (ONE(D_DNS)) {
		in.dns = (const struct rt_ctl_dns *)RECS(D_DNS);
		in.dns_len = hdr[D_DNS].recsize;
	}
	if (ONE(D_TCPST)) {
		in.tcpst = (const struct rt_tcp_stats *)RECS(D_TCPST);
		in.tcpst_len = hdr[D_TCPST].recsize;
	}
	if (ONE(D_IPST)) {
		in.ipst = (const struct rt_ip_stats *)RECS(D_IPST);
		in.ipst_len = hdr[D_IPST].recsize;
	}
#undef RECS
#undef ONE
	rp_status_update(m, &in);
	return 1;
}
