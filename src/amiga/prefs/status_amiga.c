/*
 * Riptide - RiptidePrefs: reading Riptide's tables for the Status page.
 * Once a second while the page is shown: bsdsocket.library and
 * riptide.library are opened, the tables read with RiptideSysCtl() and
 * both libraries closed again, so the editor never keeps Riptide from
 * quitting. Nothing is read when the RIPTIDE port is missing.
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
		if (k < D_SLOW || reads % 5 == 0 || !have[k])
			have[k] = fetch(k) == 0;
	reads++;
	CloseLibrary(RiptideBase);
	CloseLibrary(SocketBase);
	RiptideBase = SocketBase = NULL;
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
