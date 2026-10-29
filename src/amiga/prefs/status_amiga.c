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

/* a dump of net.riptide.<what>: the records after the header, or NULL */
static u8 *dump(int what, struct rt_ctl_hdr *h, u32 *size)
{
	LONG mib[3], len = 0;
	u8 *buf;

	mib[0] = 4;             /* CTL_NET */
	mib[1] = RT_CTL_MIB_RIPTIDE;
	mib[2] = what;
	if (RiptideSysCtl(mib, 3, NULL, &len, NULL, 0) != 0 || len <= 0)
		return NULL;
	/* tables can grow between the two calls: a little more room */
	len += len / 4 + 512;
	buf = AllocVec((ULONG)len, MEMF_ANY | MEMF_CLEAR);
	if (!buf)
		return NULL;
	if (RiptideSysCtl(mib, 3, buf, &len, NULL, 0) != 0 || len < (LONG)sizeof(*h)) {
		FreeVec(buf);
		return NULL;
	}
	memcpy(h, buf, sizeof(*h));
	if (h->version != RT_CTL_VERSION || h->recsize == 0 ||
	    (u32)len < sizeof(*h) + (u32)h->count * h->recsize) {
		FreeVec(buf);
		return NULL;
	}
	*size = (u32)len;
	return buf;
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
	enum { D_STATUS, D_IFACES, D_ADDRS6, D_ROUTES, D_TCP, D_DNS, D_TCPST, D_IPST, D_N };
	static const int what[D_N] = { RT_CTL_STATUS, RT_CTL_IFACES, RT_CTL_ADDRS6, RT_CTL_ROUTES, RT_CTL_TCPINFO,
	                               RT_CTL_DNS, RT_CTL_TCPSTAT, RT_CTL_IPSTAT };
	struct rt_ctl_hdr h[D_N];
	struct rp_st_input in;
	u8 *d[D_N];
	u32 sz;
	int i;

	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		if (m->running)
			rp_status_stopped(m);
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
		return 0;
	}
	memset(&in, 0, sizeof(in));
	memset(h, 0, sizeof(h));
	for (i = 0; i < D_N; i++)
		d[i] = dump(what[i], &h[i], &sz);
	in.now_ms = now_ms();
#define RECS(k) (d[k] ? d[k] + sizeof(struct rt_ctl_hdr) : NULL)
	if (d[D_STATUS] && h[D_STATUS].count) {
		in.st = (const struct rt_ctl_status *)RECS(D_STATUS);
		in.st_len = h[D_STATUS].recsize;
	}
	in.ifaces = RECS(D_IFACES);
	in.nifaces = h[D_IFACES].count;
	in.ifrec = h[D_IFACES].recsize;
	in.addrs6 = RECS(D_ADDRS6);
	in.naddrs6 = h[D_ADDRS6].count;
	in.a6rec = h[D_ADDRS6].recsize;
	in.routes = RECS(D_ROUTES);
	in.nroutes = h[D_ROUTES].count;
	in.rrec = h[D_ROUTES].recsize;
	in.tcp = RECS(D_TCP);
	in.ntcp = h[D_TCP].count;
	in.trec = h[D_TCP].recsize;
	if (d[D_DNS] && h[D_DNS].count) {
		in.dns = (const struct rt_ctl_dns *)RECS(D_DNS);
		in.dns_len = h[D_DNS].recsize;
	}
	if (d[D_TCPST] && h[D_TCPST].count) {
		in.tcpst = (const struct rt_tcp_stats *)RECS(D_TCPST);
		in.tcpst_len = h[D_TCPST].recsize;
	}
	if (d[D_IPST] && h[D_IPST].count) {
		in.ipst = (const struct rt_ip_stats *)RECS(D_IPST);
		in.ipst_len = h[D_IPST].recsize;
	}
#undef RECS
	rp_status_update(m, &in);
	for (i = 0; i < D_N; i++)
		if (d[i])
			FreeVec(d[i]);
	CloseLibrary(RiptideBase);
	CloseLibrary(SocketBase);
	RiptideBase = SocketBase = NULL;
	return 1;
}
