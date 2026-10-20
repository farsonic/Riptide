/*
 * Riptide - netstat: Linux net-tools style connection, routing, interface
 * and protocol statistics listing.
 *
 *   netstat [-antuwl] [-c]        sockets
 *   netstat -r [-n] [-e]          routing table
 *   netstat -i                    interfaces
 *   netstat -s                    protocol statistics
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/miami_calls.h"

static int opt_n, opt_all, opt_listen, opt_t, opt_u, opt_w, opt_e;

static void usage(void)
{
	cl_puts("usage: netstat [-veenNcCF] [<Af>] -r         netstat {-V|--version|-h|--help}\n"
	        "       netstat [-vnNcaeol] [<Socket> ...]\n"
	        "       netstat { [-veenNac] -i | [-cnNe] -M | -s [-6tuw] }\n"
	        "\n"
	        "        -r, --route              display routing table\n"
	        "        -i, --interfaces         display interface table\n"
	        "        -s, --statistics         display networking statistics\n"
	        "        -n, --numeric            don't resolve names\n"
	        "        -e, --extend             display other/more information\n"
	        "        -c, --continuous         continuous listing (Ctrl-C stops)\n"
	        "        -l, --listening          display listening server sockets\n"
	        "        -a, --all                display all sockets (default: connected)\n"
	        "  <Socket>={-t|--tcp} {-u|--udp} {-w|--raw}\n");
}

static void endpoint(char *b, u32 len, u32 addr, u32 port, const char *proto, int raw)
{
	char h[64];
	const char *svc = NULL;
	struct servent *se;

	if (!addr && !opt_n)
		rt_strlcpy(h, "0.0.0.0", sizeof(h));
	else
		cl_hostname(addr, opt_n || !addr, h, sizeof(h));
	if (raw) {
		struct protoent *pe = opt_n ? NULL : (struct protoent *)getprotobynumber((LONG)port);
		if (pe && pe->p_name)
			svc = pe->p_name;
	} else if (!port) {
		svc = "*";
	} else if (!opt_n && (se = (struct servent *)getservbyport((LONG)rt_htons((u16)port),
	                                                           (CONST_STRPTR)proto)) != NULL) {
		svc = se->s_name;
	}
	/* like net-tools: shorten the host part so that the port fits */
	{
		char p[24];
		u32 hl, pl;
		if (svc)
			rt_strlcpy(p, svc, sizeof(p));
		else
			rt_snprintf(p, sizeof(p), "%lu", (unsigned long)port);
		hl = (u32)strlen(h);
		pl = (u32)strlen(p);
		if (hl + 1 + pl > 23 && pl < 22)
			h[22 - pl] = 0;
		rt_snprintf(b, len, "%s:%s", h, p);
	}
}

static int show_sockets(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_SOCKETS, &h);
	u32 k;
	char l[64], f[64];

	if (!d)
		return CL_FAIL;
	cl_printf("Active Internet connections %s\n",
	          opt_listen ? "(only servers)" : opt_all ? "(servers and established)" : "(w/o servers)");
	cl_puts("Proto Recv-Q Send-Q Local Address           Foreign Address         State      ");
	cl_puts(opt_e ? " RTT(ms) Cwnd Retrans\n" : "\n");
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_sock *s = cl_rec(d, &h, k);
		int listening;
		const char *pn;
		if (s->kind == RT_CTL_SOCK_TCP) {
			if ((opt_t || opt_u || opt_w) && !opt_t)
				continue;
			listening = s->state == RT_TCPS_LISTEN;
			pn = "tcp";
		} else if (s->kind == RT_CTL_SOCK_UDP) {
			if ((opt_t || opt_u || opt_w) && !opt_u)
				continue;
			listening = !s->faddr;
			pn = "udp";
		} else {
			if ((opt_t || opt_u || opt_w) && !opt_w)
				continue;
			listening = !s->faddr;
			pn = "raw";
		}
		if (opt_listen ? !listening : (!opt_all && listening))
			continue;
		endpoint(l, sizeof(l), s->laddr, s->kind == RT_CTL_SOCK_RAW ? s->proto : s->lport, pn,
		         s->kind == RT_CTL_SOCK_RAW);
		endpoint(f, sizeof(f), s->faddr, s->fport, pn, 0);
		cl_printf("%-4s  %6lu %6lu %-23s %-23s %-11s", pn, (unsigned long)s->recvq,
		          (unsigned long)s->sendq, l, f,
		          s->kind == RT_CTL_SOCK_TCP ? rt_ctl_tcp_state_name(s->state) :
		          s->kind == RT_CTL_SOCK_UDP && s->faddr ? "ESTABLISHED" : "");
		if (opt_e && s->kind == RT_CTL_SOCK_TCP)
			cl_printf(" %7lu.%01lu %4lu %7lu", (unsigned long)(s->rtt_us / 1000),
			          (unsigned long)(s->rtt_us % 1000 / 100), (unsigned long)s->cwnd,
			          (unsigned long)s->retrans);
		cl_puts("\n");
	}
	FreeVec(d);
	return CL_OK;
}

static int show_routes(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_ROUTES, &h);
	u32 k;
	char dst[64], gw[64], m[16], fl[8];

	if (!d)
		return CL_FAIL;
	cl_sort_routes(d, &h);
	cl_puts("Kernel IP routing table\n");
	cl_puts("Destination     Gateway         Genmask         Flags   MSS Window  irtt Iface\n");
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_route *r = cl_rec(d, &h, k);
		u32 o = 0;
		if (r->flags & RT_RTF_PMTU)
			continue;
		if (!r->dst && !r->mask && !opt_n)
			rt_strlcpy(dst, "default", sizeof(dst));
		else
			cl_ip(r->dst, dst);
		if (r->flags & RT_RTF_GATEWAY)
			cl_hostname(r->gateway, opt_n, gw, sizeof(gw));
		else
			rt_strlcpy(gw, "0.0.0.0", sizeof(gw));
		dst[15] = gw[15] = 0;
		if (r->flags & RT_RTF_UP) fl[o++] = 'U';
		if (r->flags & RT_RTF_GATEWAY) fl[o++] = 'G';
		if (r->flags & RT_RTF_HOST) fl[o++] = 'H';
		if (r->flags & RT_RTF_DYNAMIC) fl[o++] = 'D';
		if (r->flags & RT_RTF_REJECT) fl[o++] = '!';
		fl[o] = 0;
		cl_printf("%-15s %-15s %-15s %-5s %5lu %-6u %4u %s\n", dst, gw, cl_ip(r->mask, m), fl,
		          (unsigned long)(r->mtu ? r->mtu - 40 : 0), 0U, 0U, r->ifname);
	}
	FreeVec(d);
	return CL_OK;
}

static int show_ifaces(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_IFACES, &h);
	u32 k;

	if (!d)
		return CL_FAIL;
	cl_puts("Kernel Interface table\n");
	cl_puts("Iface             MTU    RX-OK RX-ERR RX-DRP RX-OVR    TX-OK TX-ERR TX-DRP TX-OVR Flg\n");
	for (k = 0; k < h.count; k++) {
		struct rt_ctl_iface *f = cl_rec(d, &h, k);
		char fl[12];
		u32 o = 0;
		if (f->flags & RT_IFF_ALLMULTI) fl[o++] = 'A';
		if (f->flags & RT_IFF_BROADCAST) fl[o++] = 'B';
		if (f->flags & RT_IFF_LOOPBACK) fl[o++] = 'L';
		if (f->flags & RT_IFF_MULTICAST) fl[o++] = 'M';
		if (f->flags & RT_IFF_NOARP) fl[o++] = 'O';
		if (f->flags & RT_IFF_POINTOPOINT) fl[o++] = 'P';
		if (f->flags & RT_IFF_RUNNING) fl[o++] = 'R';
		if (f->flags & RT_IFF_UP) fl[o++] = 'U';
		fl[o] = 0;
		cl_printf("%-16s %5u %8lu %6lu %6lu %-6u %8lu %6lu %6lu %6u %s\n", f->name, (unsigned)f->mtu,
		          (unsigned long)f->ipackets, (unsigned long)f->ierrors, (unsigned long)f->iqdrops, 0U,
		          (unsigned long)f->opackets, (unsigned long)f->oerrors, 0UL, 0U, fl);
	}
	FreeVec(d);
	return CL_OK;
}

static void *stat_dump(int what, u32 size)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(what, &h);

	if (d && (h.count < 1 || h.recsize < size)) {
		FreeVec(d);
		cl_error("statistics table too short");
		return NULL;
	}
	return d;
}

#define P(fmt, v) cl_printf("    " fmt "\n", (unsigned long)(v))

static int show_stats(void)
{
	void *d;
	int all = !opt_t && !opt_u && !opt_w;
	u32 fwd = 0;
	LONG mib[4] = { 4, 2, 0, 1 }, len = 4;

	if (all) {
		struct rt_ip_stats *s;
		struct rt_icmp_stats *c;
		if (!(d = stat_dump(RT_CTL_IPSTAT, sizeof(*s))))
			return CL_FAIL;
		s = (struct rt_ip_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		{
			LONG v = 0;
			if (MiamiSysCtl(mib, 4, &v, &len, NULL, 0) == 0)
				fwd = (u32)v;
		}
		cl_puts("Ip:\n");
		P("Forwarding: %lu", fwd ? 1 : 2);
		P("%lu total packets received", s->InReceives);
		P("%lu with invalid headers", s->InHdrErrors);
		P("%lu with invalid addresses", s->InAddrErrors);
		P("%lu forwarded", s->ForwDatagrams);
		P("%lu with unknown protocol", s->InUnknownProtos);
		P("%lu incoming packets discarded", s->InDiscards);
		P("%lu incoming packets delivered", s->InDelivers);
		P("%lu requests sent out", s->OutRequests);
		P("%lu outgoing packets dropped", s->OutDiscards);
		P("%lu dropped because of missing route", s->OutNoRoutes);
		P("%lu reassemblies required", s->ReasmReqds);
		P("%lu packets reassembled ok", s->ReasmOKs);
		P("%lu packet reassemblies failed", s->ReasmFails);
		P("%lu fragments created", s->FragCreates);
		FreeVec(d);
		if (!(d = stat_dump(RT_CTL_ICMPSTAT, sizeof(*c))))
			return CL_FAIL;
		c = (struct rt_icmp_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		cl_puts("Icmp:\n");
		P("%lu ICMP messages received", c->InMsgs);
		P("%lu input ICMP message failed", c->InErrors);
		cl_puts("    ICMP input histogram:\n");
		if (c->InDestUnreachs) P("    destination unreachable: %lu", c->InDestUnreachs);
		if (c->InTimeExcds) P("    timeout in transit: %lu", c->InTimeExcds);
		if (c->InRedirects) P("    redirects: %lu", c->InRedirects);
		if (c->InEchos) P("    echo requests: %lu", c->InEchos);
		if (c->InEchoReps) P("    echo replies: %lu", c->InEchoReps);
		P("%lu ICMP messages sent", c->OutMsgs);
		P("%lu ICMP messages failed", c->OutErrors);
		P("%lu ICMP messages rate limited", c->OutRateLimited);
		cl_puts("    ICMP output histogram:\n");
		if (c->OutDestUnreachs) P("    destination unreachable: %lu", c->OutDestUnreachs);
		if (c->OutEchoReps) P("    echo replies: %lu", c->OutEchoReps);
		FreeVec(d);
	}
	if (all || opt_t) {
		struct rt_tcp_stats *t;
		if (!(d = stat_dump(RT_CTL_TCPSTAT, sizeof(*t))))
			return CL_FAIL;
		t = (struct rt_tcp_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		cl_puts("Tcp:\n");
		P("%lu active connection openings", t->ActiveOpens);
		P("%lu passive connection openings", t->PassiveOpens);
		P("%lu failed connection attempts", t->AttemptFails);
		P("%lu connection resets received", t->EstabResets);
		P("%lu connections established", t->CurrEstab);
		P("%lu segments received", t->InSegs);
		P("%lu segments sent out", t->OutSegs);
		P("%lu segments retransmitted", t->RetransSegs);
		P("%lu bad segments received", t->InErrs);
		P("%lu resets sent", t->OutRsts);
		cl_puts("TcpExt:\n");
		P("%lu SYN cookies sent", t->SyncookiesSent);
		P("%lu SYN cookies received", t->SyncookiesRecv);
		P("%lu invalid SYN cookies received", t->SyncookiesFailed);
		P("%lu times the listen queue of a socket overflowed", t->ListenOverflows);
		P("%lu SYNs to LISTEN sockets dropped", t->ListenDrops);
		P("%lu delayed acks sent", t->DelayedACKs);
		P("%lu packet headers predicted", t->TCPHPHits);
		P("%lu acknowledgments not containing data payload received", t->TCPPureAcks);
		P("%lu predicted acknowledgments", t->TCPHPAcks);
		P("%lu fast retransmits", t->TCPFastRetrans);
		P("%lu retransmits in slow start", t->TCPSlowStartRetrans);
		P("%lu other TCP timeouts", t->TCPTimeouts);
		P("%lu TCP loss probes", t->TCPLossProbes);
		P("%lu TCP loss probe recoveries", t->TCPLossProbeRecovery);
		P("%lu DSACKs received", t->TCPDSACKRecv);
		P("%lu connections reset due to unexpected data", t->TCPAbortOnData);
		P("%lu connections reset due to early user close", t->TCPAbortOnClose);
		P("%lu connections aborted due to timeout", t->TCPAbortOnTimeout);
		P("%lu packets queued out of order", t->TCPOFOQueue);
		FreeVec(d);
	}
	if (all || opt_u) {
		struct rt_udp_stats *u;
		if (!(d = stat_dump(RT_CTL_UDPSTAT, sizeof(*u))))
			return CL_FAIL;
		u = (struct rt_udp_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		cl_puts("Udp:\n");
		P("%lu packets received", u->InDatagrams);
		P("%lu packets to unknown port received", u->NoPorts);
		P("%lu packet receive errors", u->InErrors);
		P("%lu packets sent", u->OutDatagrams);
		P("%lu receive buffer errors", u->RcvbufErrors);
		P("%lu send buffer errors", 0);
		P("%lu InCsumErrors", u->InCsumErrors);
		FreeVec(d);
	}
	return CL_OK;
}

static int netstat_main(int argc, char **argv)
{
	int i, mode = 0, cont = 0, rc;
	struct cl_getopt g;
	int c;

	for (i = 1; i < argc; i++) {
		/* long options */
		const char *a = argv[i];
		if (a[0] != '-' || a[1] != '-')
			continue;
		if (!strcmp(a, "--help")) {
			usage();
			return CL_OK;
		}
		if (!strcmp(a, "--route")) argv[i] = "-r";
		else if (!strcmp(a, "--interfaces")) argv[i] = "-i";
		else if (!strcmp(a, "--statistics")) argv[i] = "-s";
		else if (!strcmp(a, "--numeric")) argv[i] = "-n";
		else if (!strcmp(a, "--all")) argv[i] = "-a";
		else if (!strcmp(a, "--listening")) argv[i] = "-l";
		else if (!strcmp(a, "--tcp")) argv[i] = "-t";
		else if (!strcmp(a, "--udp")) argv[i] = "-u";
		else if (!strcmp(a, "--raw")) argv[i] = "-w";
		else if (!strcmp(a, "--continuous")) argv[i] = "-c";
		else if (!strcmp(a, "--extend")) argv[i] = "-e";
	}
	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "anNtuwlrisceveopF4CV", &g)) != -1) {
		switch (c) {
		case 'a': opt_all = 1; break;
		case 'n': case 'N': opt_n = 1; break;
		case 't': opt_t = 1; break;
		case 'u': opt_u = 1; break;
		case 'w': opt_w = 1; break;
		case 'l': opt_listen = 1; break;
		case 'e': opt_e = 1; break;
		case 'c': cont = 1; break;
		case 'r': case 'i': case 's': mode = c; break;
		case 'V': cl_puts("netstat (Riptide) 0.10\n"); return CL_OK;
		case '?': usage(); return CL_ERROR;
		default: break;
		}
	}
	if (cl_need_miami() < 0)
		return CL_FAIL;
	for (;;) {
		rc = mode == 'r' ? show_routes() : mode == 'i' ? show_ifaces() : mode == 's' ? show_stats() :
		     show_sockets();
		cl_flush();
		if (!cont || rc)
			return rc;
		{
			/* one second, Ctrl-C aware */
			ULONG sigs = SIGBREAKF_CTRL_C;
			struct { ULONG s, us; } tv = { 1, 0 };
			WaitSelect(0, NULL, NULL, NULL, &tv, &sigs);
			if ((sigs & SIGBREAKF_CTRL_C) || cl_break())
				return CL_OK;
		}
	}
}

CL_MAIN(netstat_main, "netstat")
