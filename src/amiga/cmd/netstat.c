/*
 * Riptide - netstat: Linux net-tools style connection, routing, interface
 * and protocol statistics listing.
 *
 *   netstat [-antuwl] [-c]        sockets
 *   netstat -r [-n] [-e]          routing table
 *   netstat -i                    interfaces
 *   netstat -s                    protocol statistics
 *   netstat -g [-46]              multicast group memberships
 *   netstat -T [-46anc]           TCP connection details (net.riptide.tcpinfo)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"

static int opt_n, opt_all, opt_listen, opt_t, opt_u, opt_w, opt_e;
static int opt_family;           /* 0 both, 4 (-4 / -A inet), 6 (-6 / -A inet6) */

static void usage(void)
{
	cl_puts("usage: netstat [-veenNcCF] [<Af>] -r         netstat {-V|--version|-h|--help}\n"
	        "       netstat [-vnNcaeol] [<Socket> ...]\n"
	        "       netstat { [-veenNac] -i | [-cnNe] -M | -s [-6tuw] | -g [-46] }\n"
	        "       netstat -T [-46anlc]\n"
	        "\n"
	        "        -r, --route              display routing table\n"
	        "        -i, --interfaces         display interface table\n"
	        "        -s, --statistics         display networking statistics\n"
	        "        -g, --groups             display multicast group memberships\n"
	        "        -T, --tcp-info           display TCP connection details\n"
	        "        -n, --numeric            don't resolve names\n"
	        "        -e, --extend             display other/more information\n"
	        "        -c, --continuous         continuous listing (Ctrl-C stops)\n"
	        "        -l, --listening          display listening server sockets\n"
	        "        -a, --all                display all sockets (default: connected)\n"
	        "  <Socket>={-t|--tcp} {-u|--udp} {-w|--raw}\n"
	        "  <Af>={-4|--inet} {-6|--inet6}\n");
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

/* IPv6 endpoint: "addr:port" (net-tools prints IPv6 without brackets) */
static void endpoint6(char *b, u32 len, const u8 *addr, u32 port, const char *proto, int raw)
{
	struct cl_addr a;
	char h[CL_ADDRSTRLEN], p[24];
	const char *svc = NULL;
	struct servent *se;
	u32 hl, pl;
	static const u8 zero[16];

	memset(&a, 0, sizeof(a));
	a.family = RT_AF_INET6;
	memcpy(a.a.b, addr, 16);
	if (!memcmp(addr, zero, 16))
		rt_strlcpy(h, "::", sizeof(h));
	else
		cl_addr_name(&a, opt_n, h, sizeof(h));
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

static int want_kind(u8 kind)
{
	if (!opt_t && !opt_u && !opt_w)
		return 1;
	return kind == RT_CTL_SOCK_TCP ? opt_t : kind == RT_CTL_SOCK_UDP ? opt_u : opt_w;
}

static void show_sockets6(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_SOCKETS6, &h);
	u32 k;
	char l[64], f[64];
	static const u8 zero[16];

	for (k = 0; d && k < h.count && !cl_break(); k++) {
		struct rt_ctl_sock6 *s = cl_rec(d, &h, k);
		int listening, connected = memcmp(s->faddr, zero, 16) != 0;
		const char *pn;
		if (!want_kind(s->kind))
			continue;
		if (s->kind == RT_CTL_SOCK_TCP) {
			listening = s->state == RT_TCPS_LISTEN;
			pn = "tcp6";
		} else {
			listening = !connected;
			pn = s->kind == RT_CTL_SOCK_UDP ? "udp6" : "raw6";
		}
		if (opt_listen ? !listening : (!opt_all && listening))
			continue;
		endpoint6(l, sizeof(l), s->laddr, s->kind == RT_CTL_SOCK_RAW ? s->proto : s->lport, pn,
		          s->kind == RT_CTL_SOCK_RAW);
		endpoint6(f, sizeof(f), s->faddr, s->fport, pn, 0);
		cl_printf("%-4s  %6lu %6lu %-23s %-23s %-11s", pn, (unsigned long)s->recvq,
		          (unsigned long)s->sendq, l, f,
		          s->kind == RT_CTL_SOCK_TCP ? rt_ctl_tcp_state_name(s->state) :
		          s->kind == RT_CTL_SOCK_UDP && connected ? "ESTABLISHED" : "");
		if (opt_e && s->kind == RT_CTL_SOCK_TCP)
			cl_printf(" %7lu.%01lu %4lu %7lu", (unsigned long)(s->rtt_us / 1000),
			          (unsigned long)(s->rtt_us % 1000 / 100), (unsigned long)s->cwnd,
			          (unsigned long)s->retrans);
		cl_puts("\n");
	}
	if (d)
		FreeVec(d);
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
	for (k = 0; opt_family != 6 && k < h.count && !cl_break(); k++) {
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
	if (opt_family != 4)
		show_sockets6();
	return CL_OK;
}

/* ---- netstat -T: TCP connection details ---- */

static int v4mapped(const u8 *a)
{
	static const u8 pfx[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
	return !memcmp(a, pfx, 12);
}

static void tcpi_endpoint(char *b, u32 len, const u8 *addr, u32 port, int v4)
{
	u32 a;

	if (v4) {
		memcpy(&a, addr + 12, 4);
		endpoint(b, len, a, port, "tcp", 0);
	} else {
		endpoint6(b, len, addr, port, "tcp", 0);
	}
}

/* a 64-bit byte count, shortened past 4 GB */
static const char *tcpi_bytes(u32 hi, u32 lo, char *b, u32 len)
{
	if (hi)
		rt_snprintf(b, len, "%luG", (unsigned long)(hi * 4u + (lo >> 30)));
	else
		rt_snprintf(b, len, "%lu", (unsigned long)lo);
	return b;
}

/* ms as "12.3s" from 10 s on, else "1234ms" */
static const char *tcpi_ms(u32 ms, char *b, u32 len)
{
	if (ms >= 10000)
		rt_snprintf(b, len, "%lu.%01lus", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
	else
		rt_snprintf(b, len, "%lums", (unsigned long)ms);
	return b;
}

static void show_tcpinfo_rec(const struct rt_ctl_tcpinfo *t, int v4)
{
	static const char *const ca[] = { "open", "disorder", "cwr", "recovery", "loss" };
	char l[64], f[64], b1[16], b2[16], b3[16], b4[16], b5[16];
	u32 rate = 0;

	tcpi_endpoint(l, sizeof(l), t->laddr, t->lport, v4);
	tcpi_endpoint(f, sizeof(f), t->faddr, t->fport, v4);
	cl_printf("%s %s %s %s\n", v4 ? "tcp " : "tcp6", l, f, rt_ctl_tcp_state_name(t->state));
	if (t->state == RT_TCPS_LISTEN) {
		cl_printf("  syn queue %lu  rcvbuf %lu  sndbuf %lu\n", (unsigned long)t->recvq,
		          (unsigned long)t->rcvbuf, (unsigned long)t->sndbuf);
		return;
	}
	cl_printf("  %s %s age %s  rtt %lu.%01lu/%lu.%01lu ms min %lu.%01lu rto %lu ms backoff %u\n",
	          t->cc, t->ca_state < 5 ? ca[t->ca_state] : "?", tcpi_ms(t->age_ms, b1, sizeof(b1)),
	          (unsigned long)(t->srtt_us / 1000), (unsigned long)(t->srtt_us % 1000 / 100),
	          (unsigned long)(t->rttvar_us / 1000), (unsigned long)(t->rttvar_us % 1000 / 100),
	          (unsigned long)(t->min_rtt_us / 1000), (unsigned long)(t->min_rtt_us % 1000 / 100),
	          (unsigned long)(t->rto_us / 1000), (unsigned)t->backoff);
	cl_printf("  opts%s%s%s%s wscale %u/%u mss %lu/%lu advmss %lu cwnd %lu ssthresh ",
	          t->options & RT_CTL_TCPI_TS ? " ts" : "", t->options & RT_CTL_TCPI_SACK ? " sack" : "",
	          t->options & RT_CTL_TCPI_WSCALE ? " ws" : "", t->options & RT_CTL_TCPI_ECN ? " ecn" : "",
	          (unsigned)t->snd_wscale, (unsigned)t->rcv_wscale, (unsigned long)t->snd_mss,
	          (unsigned long)t->rcv_mss, (unsigned long)t->advmss, (unsigned long)t->cwnd);
	if (t->ssthresh >= 0x7fffffffu)
		cl_puts("-\n");
	else
		cl_printf("%lu\n", (unsigned long)t->ssthresh);
	if (t->bg || t->ledbat_base_us) {
		static const char *const ph[] = { "slow start", "normal", "slowdown", "ramp up" };
		cl_printf("  background%s%s%s", t->bg & RT_CTL_TCPBG_RECV ? " receive" : "",
		          t->bg & RT_CTL_TCPBG_SEND ? " send" : "", t->bg & RT_CTL_TCPBG_RULE ? " (rule)" : "");
		if (t->bg & RT_CTL_TCPBG_RL_ON)
			cl_printf("  rLEDBAT window %lu", (unsigned long)t->rledbat_wnd);
		else if (t->bg & RT_CTL_TCPBG_RECV)
			cl_puts("  rLEDBAT off (no timestamps)");
		if (t->ledbat_base_us)
			cl_printf("  delay %lu.%01lu ms over base %lu.%01lu ms, %s",
			          (unsigned long)(t->ledbat_qdelay_us / 1000), (unsigned long)(t->ledbat_qdelay_us % 1000 / 100),
			          (unsigned long)(t->ledbat_base_us / 1000), (unsigned long)(t->ledbat_base_us % 1000 / 100),
			          ph[t->ledbat_phase & 3]);
		cl_puts("\n");
	}
	cl_printf("  wnd snd %lu rcv %lu (limit %lu)  buf rcv %lu snd %lu\n", (unsigned long)t->snd_wnd,
	          (unsigned long)t->rcv_wnd, (unsigned long)t->rcv_ssthresh, (unsigned long)t->rcvbuf,
	          (unsigned long)t->sndbuf);
	cl_printf("  queue recv %lu send %lu ooo %lu/%lu seg  inflight %lu sacked %lu lost %lu\n",
	          (unsigned long)t->recvq, (unsigned long)t->sendq, (unsigned long)t->ofo_bytes,
	          (unsigned long)t->ofo_segs, (unsigned long)t->packets_out, (unsigned long)t->sacked_out,
	          (unsigned long)t->lost_out);
	if (t->age_ms >= 100 && !t->bytes_received_hi)
		rate = t->bytes_received_lo / (t->age_ms / 100) * 10u / 1024u;     /* KB/s */
	cl_printf("  rx %s bytes, %lu segs (%lu with data), %lu KB/s average\n",
	          tcpi_bytes(t->bytes_received_hi, t->bytes_received_lo, b2, sizeof(b2)),
	          (unsigned long)t->segs_in, (unsigned long)t->data_segs_in, (unsigned long)rate);
	cl_printf("  tx %s bytes acked, %lu segs (%lu with data)\n",
	          tcpi_bytes(t->bytes_acked_hi, t->bytes_acked_lo, b3, sizeof(b3)),
	          (unsigned long)t->segs_out, (unsigned long)t->data_segs_out);
	cl_printf("  acks out %lu (delayed %lu, dup %lu, sack blocks %lu)  dupacks in %lu\n",
	          (unsigned long)t->acks_out, (unsigned long)t->delayed_acks_out, (unsigned long)t->dupacks_out,
	          (unsigned long)t->sack_blocks_out, (unsigned long)t->dupacks_in);
	cl_printf("  retrans %lu  fast recovery %lu  rto %lu  tlp %lu  zero wnd out %lu in %lu\n",
	          (unsigned long)t->total_retrans, (unsigned long)t->fast_recoveries,
	          (unsigned long)t->rto_timeouts, (unsigned long)t->tlp_probes, (unsigned long)t->zwin_out,
	          (unsigned long)t->zwin_in);
	cl_printf("  time busy %s rwnd-limited %s sndbuf-limited %s\n", tcpi_ms(t->busy_ms, b1, sizeof(b1)),
	          tcpi_ms(t->rwnd_limited_ms, b2, sizeof(b2)), tcpi_ms(t->sndbuf_limited_ms, b3, sizeof(b3)));
	/* "last ... ago": 0 means never */
	cl_printf("       unread %s zero-wnd %s  last rx %s tx %s ack %s ago\n",
	          tcpi_ms(t->unread_ms, b1, sizeof(b1)), tcpi_ms(t->zwin_ms, b2, sizeof(b2)),
	          t->last_data_recv_ago ? tcpi_ms(t->last_data_recv_ago, b3, sizeof(b3)) : "-",
	          t->last_data_sent_ago ? tcpi_ms(t->last_data_sent_ago, b4, sizeof(b4)) : "-",
	          t->last_ack_recv_ago ? tcpi_ms(t->last_ack_recv_ago, b5, sizeof(b5)) : "-");
}

static int show_tcpinfo(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_TCPINFO, &h);
	u32 k, shown = 0;

	if (!d)
		return CL_FAIL;
	cl_printf("TCP connection details %s\n",
	          opt_listen ? "(only servers)" : opt_all ? "(servers and established)" : "(w/o servers)");
	for (k = 0; k < h.count && !cl_break(); k++) {
		struct rt_ctl_tcpinfo t;
		int listening, v4;
		/* records may be longer (newer Riptide) or, in theory, shorter */
		memset(&t, 0, sizeof(t));
		memcpy(&t, cl_rec(d, &h, k), RT_MIN((u32)h.recsize, (u32)sizeof(t)));
		listening = t.state == RT_TCPS_LISTEN;
		if (opt_listen ? !listening : (!opt_all && listening))
			continue;
		v4 = t.family == RT_AF_INET || v4mapped(t.faddr);
		if ((opt_family == 4 && !v4) || (opt_family == 6 && v4))
			continue;
		if (shown++)
			cl_puts("\n");
		show_tcpinfo_rec(&t, v4);
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

/* Linux net-tools: IPv6 groups first, then IPv4 (numeric) */
static int show_groups(void)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(RT_CTL_MCAST, &h);
	u32 k;
	int pass;

	if (!d)
		return CL_FAIL;
	cl_puts("IPv6/IPv4 Group Memberships\n"
	        "Interface       RefCnt Group\n"
	        "--------------- ------ ---------------------\n");
	for (pass = 0; pass < 2; pass++) {
		u8 fam = pass ? RT_AF_INET : RT_AF_INET6;
		if (opt_family && opt_family != (pass ? 4 : 6))
			continue;
		for (k = 0; k < h.count; k++) {
			struct rt_ctl_mcast *m = cl_rec(d, &h, k);
			struct cl_addr a;
			char g[CL_ADDRSTRLEN];
			if (m->family != fam)
				continue;
			memset(&a, 0, sizeof(a));
			a.family = fam;
			memcpy(a.a.b, m->group, 16);
			cl_printf("%-15s %-6lu %s\n", m->ifname, (unsigned long)m->refcnt, cl_addr_str(&a, g));
		}
	}
	FreeVec(d);
	return CL_OK;
}

static u32 stat_recsize;          /* of the last stat_dump() */

static void *stat_dump(int what, u32 size)
{
	struct rt_ctl_hdr h;
	void *d = cl_dump(what, &h);

	stat_recsize = d ? h.recsize : 0;
	if (d && (h.count < 1 || h.recsize < size)) {
		FreeVec(d);
		cl_error("statistics table too short");
		return NULL;
	}
	return d;
}

#define P(fmt, v) cl_printf("    " fmt "\n", (unsigned long)(v))

static void show_stats6(void)
{
	struct rt_ip6_stats *s;
	struct rt_icmp6_stats *c;
	void *d;

	if ((d = stat_dump(RT_CTL_IP6STAT, sizeof(*s))) != NULL) {
		s = (struct rt_ip6_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		cl_puts("Ip6:\n");
		P("%lu total packets received", s->InReceives);
		P("%lu with invalid headers", s->InHdrErrors);
		P("%lu with packets too big", s->InTooBigErrors);
		P("%lu with no route", s->InNoRoutes);
		P("%lu with invalid addresses", s->InAddrErrors);
		P("%lu with unknown protocol", s->InUnknownProtos);
		P("%lu truncated packets", s->InTruncatedPkts);
		P("%lu incoming packets discarded", s->InDiscards);
		P("%lu incoming packets delivered", s->InDelivers);
		P("%lu forwarded", s->OutForwDatagrams);
		P("%lu requests sent out", s->OutRequests);
		P("%lu outgoing packets dropped", s->OutDiscards);
		P("%lu dropped because of missing route", s->OutNoRoutes);
		P("%lu reassemblies required", s->ReasmReqds);
		P("%lu packets reassembled ok", s->ReasmOKs);
		P("%lu packet reassemblies failed", s->ReasmFails);
		P("%lu fragments created", s->FragCreates);
		P("%lu incoming multicast packets", s->InMcastPkts);
		P("%lu outgoing multicast packets", s->OutMcastPkts);
		FreeVec(d);
	}
	if ((d = stat_dump(RT_CTL_ICMP6STAT, sizeof(*c))) != NULL) {
		c = (struct rt_icmp6_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		cl_puts("Icmp6:\n");
		P("%lu ICMPv6 messages received", c->InMsgs);
		P("%lu input ICMPv6 message failed", c->InErrors);
		P("%lu ICMPv6 messages sent", c->OutMsgs);
		P("%lu ICMPv6 messages failed", c->OutErrors);
		P("%lu ICMPv6 messages rate limited", c->OutRateLimited);
		cl_puts("    ICMPv6 input histogram:\n");
		if (c->InDestUnreachs) P("    destination unreachable: %lu", c->InDestUnreachs);
		if (c->InPktTooBigs) P("    packets too big: %lu", c->InPktTooBigs);
		if (c->InTimeExcds) P("    received time exceeded: %lu", c->InTimeExcds);
		if (c->InEchos) P("    echo requests: %lu", c->InEchos);
		if (c->InEchoReplies) P("    echo replies: %lu", c->InEchoReplies);
		if (c->InRouterAdvertisements) P("    router advertisements: %lu", c->InRouterAdvertisements);
		if (c->InNeighborSolicits) P("    neighbour solicits: %lu", c->InNeighborSolicits);
		if (c->InNeighborAdvertisements) P("    neighbour advertisements: %lu", c->InNeighborAdvertisements);
		cl_puts("    ICMPv6 output histogram:\n");
		if (c->OutDestUnreachs) P("    destination unreachable: %lu", c->OutDestUnreachs);
		if (c->OutEchoReplies) P("    echo replies: %lu", c->OutEchoReplies);
		if (c->OutRouterSolicits) P("    router solicits: %lu", c->OutRouterSolicits);
		if (c->OutNeighborSolicits) P("    neighbour solicits: %lu", c->OutNeighborSolicits);
		if (c->OutNeighborAdvertisements) P("    neighbour advertisements: %lu", c->OutNeighborAdvertisements);
		if (c->OutUnsolicitedNAs) P("    of which address announcements: %lu", c->OutUnsolicitedNAs);
		if (c->OutMLDv2Reports) P("    MLDv2 reports: %lu", c->OutMLDv2Reports);
		FreeVec(d);
	}
}

static void show_stats_igmp(void)
{
	struct rt_igmp_stats *g;
	void *d = stat_dump(RT_CTL_IGMPSTAT, sizeof(*g));

	if (!d)
		return;
	g = (struct rt_igmp_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
	cl_puts("Igmp:\n");
	P("%lu IGMP messages received", g->InMsgs);
	P("%lu input IGMP messages failed", g->InErrors);
	P("%lu V1 membership queries received", g->InQueriesV1);
	P("%lu V2 membership queries received", g->InQueriesV2);
	P("%lu V3 membership queries received", g->InQueriesV3);
	P("%lu bad membership queries received", g->InBadQueries);
	P("%lu membership reports received", g->InReports);
	P("%lu membership reports received for groups we are in", g->InOurReports);
	P("%lu leave messages received", g->InLeaves);
	P("%lu IGMP messages sent", g->OutMsgs);
	P("%lu V1 membership reports sent", g->OutReportsV1);
	P("%lu V2 membership reports sent", g->OutReportsV2);
	P("%lu V3 membership reports sent", g->OutReportsV3);
	P("%lu leave messages sent", g->OutLeaves);
	FreeVec(d);
}

static int show_stats(void)
{
	void *d;
	int all = !opt_t && !opt_u && !opt_w;
	u32 fwd = 0;
	LONG mib[4] = { 4, 2, 0, 1 }, len = 4;

	if (opt_family == 6) {
		show_stats6();
		return CL_OK;
	}
	if (all) {
		struct rt_ip_stats *s;
		struct rt_icmp_stats *c;
		if (!(d = stat_dump(RT_CTL_IPSTAT, sizeof(*s))))
			return CL_FAIL;
		s = (struct rt_ip_stats *)((u8 *)d + sizeof(struct rt_ctl_hdr));
		{
			LONG v = 0;
			if (RiptideSysCtl(mib, 4, &v, &len, NULL, 0) == 0)
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
		show_stats_igmp();
		if (opt_family == 0)
			show_stats6();
	}
	if (all || opt_t) {
		struct rt_tcp_stats *t;
		/* stacks before 0.15 have a shorter record */
		if (!(d = stat_dump(RT_CTL_TCPSTAT, offsetof(struct rt_tcp_stats, TCPFastOpenActive))))
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
		P("%lu keepalive probes sent", t->TCPKeepAlive);
		P("%lu TIME_WAIT connections reused", t->TimeWaitRecycled);
		/* Riptide 0.15 counters: only from a stack that has them */
		if (((struct rt_ctl_hdr *)d)->recsize >= offsetof(struct rt_tcp_stats, TCPMTUPBlackhole)) {
			P("%lu connects with data in the SYN (TCP Fast Open)", t->TCPFastOpenActive);
			P("%lu TCP Fast Open SYN data not acknowledged", t->TCPFastOpenActiveFail);
			P("%lu TCP Fast Open cookie requests", t->TCPFastOpenCookieReqd);
			P("%lu TCP Fast Open SYNs lost", t->TCPFastOpenSynLost);
			P("%lu times TCP Fast Open switched off (SYNs lost in a row)", t->TCPFastOpenBlackhole);
			P("%lu connects timed out", t->TCPConnectTimeouts);
			P("%lu connects ended by ICMP unreachable", t->TCPConnectUnreach);
			P("%lu corked segments sent by the 200 ms limit", t->TCPCorkFlush);
			P("%lu times socket memory came under pressure", t->TCPMemoryPressures);
			P("%lu ms under memory pressure", t->TCPMemoryPressuresMs);
			P("%lu segments dropped at the socket memory limit", t->TCPMemLimitDrops);
		}
		P("%lu MTU probes acknowledged", t->TCPMTUPSuccess);
		P("%lu MTU probes lost", t->TCPMTUPFail);
		if (((struct rt_ctl_hdr *)d)->recsize >= offsetof(struct rt_tcp_stats, TCPECNNegotiated))
			P("%lu MSS reductions on a suspected path MTU black hole", t->TCPMTUPBlackhole);
		P("%lu segments received with ECN congestion experienced", t->TCPECNMarks);
		if (((struct rt_ctl_hdr *)d)->recsize >= sizeof(*t)) {
			P("%lu connections using ECN", t->TCPECNNegotiated);
			P("%lu ECN requests not agreed by the peer", t->TCPECNNotAgreed);
			P("%lu SYNs resent without ECN", t->TCPECNFallback);
			P("%lu acknowledgments with ECN echo received", t->TCPECNEceRcvd);
		}
		FreeVec(d);
	}
	if (all || opt_u) {
		struct rt_udp_stats *u;
		/* stacks before 0.17 have a shorter record */
		if (!(d = stat_dump(RT_CTL_UDPSTAT, offsetof(struct rt_udp_stats, TxNoRoom))))
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
		if (stat_recsize >= sizeof(*u)) {
			P("%lu sends that waited for transmit room", u->TxNoRoom);
			P("%lu datagrams sent with RiptideSendMMsg()", u->MMsgDatagrams);
		}
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
		else if (!strcmp(a, "--groups")) argv[i] = "-g";
		else if (!strcmp(a, "--numeric")) argv[i] = "-n";
		else if (!strcmp(a, "--all")) argv[i] = "-a";
		else if (!strcmp(a, "--listening")) argv[i] = "-l";
		else if (!strcmp(a, "--tcp")) argv[i] = "-t";
		else if (!strcmp(a, "--udp")) argv[i] = "-u";
		else if (!strcmp(a, "--raw")) argv[i] = "-w";
		else if (!strcmp(a, "--continuous")) argv[i] = "-c";
		else if (!strcmp(a, "--extend")) argv[i] = "-e";
		else if (!strcmp(a, "--inet")) argv[i] = "-4";
		else if (!strcmp(a, "--inet6")) argv[i] = "-6";
		else if (!strcmp(a, "--tcp-info")) argv[i] = "-T";
	}
	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "anNtuwlrisgceveopF46CVTA:", &g)) != -1) {
		switch (c) {
		case 'a': opt_all = 1; break;
		case 'n': case 'N': opt_n = 1; break;
		case 't': opt_t = 1; break;
		case 'u': opt_u = 1; break;
		case 'w': opt_w = 1; break;
		case 'l': opt_listen = 1; break;
		case 'e': opt_e = 1; break;
		case 'c': cont = 1; break;
		case '4': opt_family = 4; break;
		case '6': opt_family = 6; break;
		case 'A': opt_family = !strcmp(g.arg, "inet6") ? 6 : !strcmp(g.arg, "inet") ? 4 : 0; break;
		case 'r': case 'i': case 's': case 'g': case 'T': mode = c; break;
		case 'V': cl_puts("netstat (Riptide) 0.21\n"); return CL_OK;
		case '?': usage(); return CL_ERROR;
		default: break;
		}
	}
	if (cl_need_riptide() < 0)
		return CL_FAIL;
	for (;;) {
		rc = mode == 'r' ? show_routes() : mode == 'i' ? show_ifaces() : mode == 's' ? show_stats() :
		     mode == 'g' ? show_groups() : mode == 'T' ? show_tcpinfo() : show_sockets();
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
