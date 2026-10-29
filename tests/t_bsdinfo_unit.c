/*
 * Riptide - unit tests of the stack side of the Roadshow API: the
 * routing table as 4.4BSD routing messages and the statistics tables
 * (src/core/bsdinfo.c), the network database enumeration (netdb.c), the
 * ICMP echo and time stamp settings, the in-stack monitoring hooks
 * (rt_monitor.h: ICMP, UDP, TCP connection requests, packets) and the
 * address change hook. Frames are fed to a fake Ethernet driver.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "rt/rt_bsdinfo.h"
#include "rt/rt_stats.h"
#include "../src/core/infra.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct { u16 et; u32 len; u8 data[1600]; } rec[16];
static int nrec;

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	if (nrec < 16) {
		rec[nrec].et = et;
		rec[nrec].len = b->len;
		memcpy(rec[nrec].data, b->data, b->len < 1600 ? b->len : 1600);
		nrec++;
	}
	rt_buf_free(b);
	return 0;
}

static struct rt_netif eth0;
static const u8 mac0[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 peer[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };
#define OUR  0xc0a80a02u     /* 192.168.10.2 */
#define PEER 0xc0a80a63u     /* 192.168.10.99 */

static int cfg(const char *line)
{
	char err[200], buf[200];
	int r;

	rt_strlcpy(buf, line, sizeof(buf));
	r = rt_config_line(buf, err, sizeof(err), NULL);
	if (r < 0 && getenv("T_VERBOSE"))
		fprintf(stderr, "%s -> %d %s\n", line, r, err);
	return r;
}

static u32 get32(const u8 *p)
{
	u32 v;
	memcpy(&v, p, 4);
	return v;
}

static u16 get16(const u8 *p)
{
	u16 v;
	memcpy(&v, p, 2);
	return v;
}

/* ---- routes ---- */

static void test_routes(void)
{
	static u8 buf[4096];
	u32 n, off = 0, total, i, found_net = 0, found_def = 0, entries = 0;

	CHECK(cfg("route add 10.1.0.0/16 via 192.168.10.1") == 0);
	CHECK(cfg("route add default via 192.168.10.1") == 0);
	CHECK(cfg("route add 10.9.9.9/32 via 192.168.10.1") == 0);
	total = rt_bsd_route_dump(0, NULL, 0);
	CHECK(total > RT_RTM_HDRLEN && total < sizeof(buf));
	memset(buf, 0xee, sizeof(buf));
	n = rt_bsd_route_dump(0, buf, sizeof(buf));
	CHECK(n == total);
	for (;;) {
		u16 len = get16(buf + off);
		u32 flags, addrs, a;
		if (!len)
			break;
		entries++;
		CHECK(buf[off + RT_RTM_OFS_VERSION] == RT_RTM_VERSION && buf[off + RT_RTM_OFS_TYPE] == RT_RTM_GET);
		flags = get32(buf + off + RT_RTM_OFS_FLAGS);
		addrs = get32(buf + off + RT_RTM_OFS_ADDRS);
		CHECK(flags & RT_RTF_UP);
		CHECK(addrs & RT_RTA_DST);
		/* the sockaddrs follow the header, 16 bytes each */
		CHECK(len == RT_RTM_HDRLEN + 16 * (((addrs & 1) != 0) + ((addrs & 2) != 0) + ((addrs & 4) != 0) +
		                                  ((addrs & 0x20) != 0)));
		CHECK(buf[off + RT_RTM_HDRLEN] == 16 && buf[off + RT_RTM_HDRLEN + 1] == RT_AF_INET);
		memcpy(&a, buf + off + RT_RTM_HDRLEN + 4, 4);
		if (a == rt_htonl(0x0a010000u)) {
			u32 g, m;
			found_net = 1;
			CHECK((flags & RT_RTF_GATEWAY) && !(flags & RT_RTF_HOST));
			CHECK((addrs & (RT_RTA_GATEWAY | RT_RTA_NETMASK | RT_RTA_IFA)) ==
			      (RT_RTA_GATEWAY | RT_RTA_NETMASK | RT_RTA_IFA));
			memcpy(&g, buf + off + RT_RTM_HDRLEN + 16 + 4, 4);
			memcpy(&m, buf + off + RT_RTM_HDRLEN + 32 + 4, 4);
			CHECK(g == rt_htonl(0xc0a80a01u) && m == rt_htonl(0xffff0000u));
			CHECK(get16(buf + off + RT_RTM_OFS_INDEX) == eth0.index);
		}
		if (a == 0 && (flags & RT_RTF_GATEWAY))
			found_def = 1;
		if (a == rt_htonl(0x0a090909u))
			CHECK((flags & RT_RTF_HOST) && !(addrs & RT_RTA_NETMASK));
		off += len;
		if (off >= n)
			break;
	}
	CHECK(found_net && found_def && entries >= 4);
	CHECK(off + RT_RTM_HDRLEN == n);
	/* the flags filter: gateway routes only */
	n = rt_bsd_route_dump(RT_RTF_GATEWAY, buf, sizeof(buf));
	for (off = 0, i = 0; get16(buf + off); off += get16(buf + off), i++)
		CHECK(get32(buf + off + RT_RTM_OFS_FLAGS) & RT_RTF_GATEWAY);
	CHECK(i == 3);
	n = rt_bsd_route_dump(RT_RTF_GATEWAY | RT_RTF_HOST, buf, sizeof(buf));
	CHECK(n == RT_RTM_HDRLEN * 2 + 3 * 16);
	/* a short buffer gets a prefix, the size is the whole table */
	CHECK(rt_bsd_route_dump(0, buf, 10) == total);
	CHECK(cfg("route del 10.9.9.9/32") == 0);
}

/* ---- frames ---- */

static u16 l4sum(u32 s, u32 d, u8 proto, const u8 *p, u32 len)
{
	return rt_csum_fold(rt_csum_partial(p, len, rt_csum_pseudo(s, d, proto, (u16)len)));
}

/* an IPv4 packet from PEER to OUR with payload pl (checksums filled) */
static void rx_ip(u8 proto, u8 *pl, u32 len)
{
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20 + len);
	u8 *p = rt_buf_put(b, 20 + len);
	u32 s = rt_htonl(PEER), d = rt_htonl(OUR);
	u16 sum;

	memset(p, 0, 20);
	p[0] = 0x45;
	rt_put_be16(p + 2, (u16)(20 + len));
	p[8] = 64;
	p[9] = proto;
	memcpy(p + 12, &s, 4);
	memcpy(p + 16, &d, 4);
	sum = rt_inet_csum(p, 20);
	memcpy(p + 10, &sum, 2);
	if (proto == RT_IPPROTO_ICMP) {
		pl[2] = pl[3] = 0;
		sum = rt_inet_csum(pl, len);
		memcpy(pl + 2, &sum, 2);
	} else if (proto == RT_IPPROTO_UDP) {
		pl[6] = pl[7] = 0;
		sum = l4sum(s, d, proto, pl, len);
		memcpy(pl + 6, &sum, 2);
	} else if (proto == RT_IPPROTO_TCP) {
		pl[16] = pl[17] = 0;
		sum = l4sum(s, d, proto, pl, len);
		memcpy(pl + 16, &sum, 2);
	}
	memcpy(p + 20, pl, len);
	nrec = 0;
	rt_netif_input(&eth0, b, RT_ETH_P_IP, peer, 0);
}

static void icmp_echo(void)
{
	u8 ic[16];

	memset(ic, 0x11, sizeof(ic));
	ic[0] = RT_ICMP_ECHO;
	ic[1] = 0;
	rx_ip(RT_IPPROTO_ICMP, ic, sizeof(ic));
}

static int sent_icmp(u8 type)
{
	int i;

	for (i = 0; i < nrec; i++)
		if (rec[i].et == RT_ETH_P_IP && rec[i].data[9] == RT_IPPROTO_ICMP && rec[i].data[20] == type)
			return i;
	return -1;
}

static int sent_tcp(u8 flags)
{
	int i;

	for (i = 0; i < nrec; i++)
		if (rec[i].et == RT_ETH_P_IP && rec[i].data[9] == RT_IPPROTO_TCP && rec[i].data[33] == flags)
			return i;
	return -1;
}

static int verdict[8], calls[8], pkt_verdict, pkt_calls[2];
static u32 hook_src;

static int mon(int type, const u32 *src, const u32 *dst, const void *hdr)
{
	calls[type]++;
	hook_src = *src;
	CHECK(*dst == rt_htonl(OUR) && hdr != NULL);
	return verdict[type];
}

static int mon_pkt(int out, const void *lh, u32 lhlen, const void *data, u32 len)
{
	pkt_calls[out]++;
	CHECK(lh == NULL && lhlen == 0 && data && len >= 20);
	return pkt_verdict;
}

static void test_icmp_and_hooks(void)
{
	u8 ts[20], uh[12], th[20];
	u32 errs;
	s32 v;
	u32 len = 4;

	CHECK(rt_arp_add_static(&eth0, rt_htonl(PEER), peer) == 0);

	/* echo: answered, ignored, dropped */
	icmp_echo();
	CHECK(sent_icmp(RT_ICMP_ECHOREPLY) >= 0);
	v = 1;
	CHECK(rt_sysctl_byname("net.inet.icmp.process_echo", NULL, NULL, &v, 4) == 0);
	icmp_echo();
	CHECK(sent_icmp(RT_ICMP_ECHOREPLY) < 0);
	v = 2;
	CHECK(rt_sysctl_byname("net.inet.icmp.process_echo", NULL, NULL, &v, 4) == 0);
	errs = rt_icmpstat.InErrors;
	icmp_echo();
	CHECK(sent_icmp(RT_ICMP_ECHOREPLY) < 0 && rt_icmpstat.InErrors == errs + 1);
	v = 3;
	CHECK(rt_sysctl_byname("net.inet.icmp.process_echo", NULL, NULL, &v, 4) < 0);
	v = 0;
	CHECK(rt_sysctl_byname("net.inet.icmp.process_echo", NULL, NULL, &v, 4) == 0);
	CHECK(rt_sysctl_byname("net.inet.icmp.process_echo", &v, &len, NULL, 0) == 0 && v == 0);

	/* time stamp request: originate copied, receive = transmit */
	memset(ts, 0, sizeof(ts));
	ts[0] = RT_ICMP_TSTAMP;
	rt_put_be16(ts + 4, 0x1234);
	rt_put_be32(ts + 8, 0xdeadbeef);
	rx_ip(RT_IPPROTO_ICMP, ts, sizeof(ts));
	{
		int i = sent_icmp(RT_ICMP_TSTAMPREPLY);
		CHECK(i >= 0);
		if (i >= 0) {
			const u8 *r = rec[i].data + 20;
			CHECK(rec[i].len == 40 && rt_get_be16(r + 4) == 0x1234 && rt_get_be32(r + 8) == 0xdeadbeef);
			CHECK(rt_get_be32(r + 12) == rt_get_be32(r + 16) && rt_get_be32(r + 12) < 86400000u);
			CHECK(rt_inet_csum(r, 20) == 0);
		}
	}
	v = 1;
	CHECK(rt_sysctl_byname("net.inet.icmp.process_tstamp", NULL, NULL, &v, 4) == 0);
	rx_ip(RT_IPPROTO_ICMP, ts, sizeof(ts));
	CHECK(sent_icmp(RT_ICMP_TSTAMPREPLY) < 0);
	v = 0;
	rt_sysctl_byname("net.inet.icmp.process_tstamp", NULL, NULL, &v, 4);

	rt_mon_hook = mon;
	rt_mon_packet_hook = mon_pkt;

	/* MHT_ICMP */
	rt_mon_mask = 1 << RT_MON_ICMP;
	verdict[RT_MON_ICMP] = RT_MA_CONTINUE;
	icmp_echo();
	CHECK(calls[RT_MON_ICMP] == 1 && hook_src == rt_htonl(PEER) && sent_icmp(RT_ICMP_ECHOREPLY) >= 0);
	verdict[RT_MON_ICMP] = RT_MA_IGNORE;
	errs = rt_icmpstat.InErrors;
	icmp_echo();
	CHECK(sent_icmp(RT_ICMP_ECHOREPLY) < 0 && rt_icmpstat.InErrors == errs);
	verdict[RT_MON_ICMP] = RT_MA_DROP;
	icmp_echo();
	CHECK(sent_icmp(RT_ICMP_ECHOREPLY) < 0 && rt_icmpstat.InErrors == errs + 1);

	/* MHT_UDP: a datagram to a closed port gets "port unreachable" */
	memset(uh, 0, sizeof(uh));
	rt_put_be16(uh, 4000);
	rt_put_be16(uh + 2, 4001);
	rt_put_be16(uh + 4, sizeof(uh));
	rt_mon_mask = 1 << RT_MON_UDP;
	verdict[RT_MON_UDP] = RT_MA_CONTINUE;
	rx_ip(RT_IPPROTO_UDP, uh, sizeof(uh));
	CHECK(calls[RT_MON_UDP] == 1 && sent_icmp(RT_ICMP_UNREACH) >= 0);
	verdict[RT_MON_UDP] = RT_MA_DROP;
	errs = rt_udpstat.InCsumErrors;
	rx_ip(RT_IPPROTO_UDP, uh, sizeof(uh));
	CHECK(calls[RT_MON_UDP] == 2 && sent_icmp(RT_ICMP_UNREACH) < 0 && rt_udpstat.InCsumErrors == errs + 1);

	/* MHT_TCP_Connect: a SYN to a closed port gets a reset anyway; Drop
	 * suppresses it, DropWithReset answers it from the hook */
	memset(th, 0, sizeof(th));
	rt_put_be16(th, 5000);
	rt_put_be16(th + 2, 5001);
	rt_put_be32(th + 4, 1000);
	th[12] = 5 << 4;
	th[13] = 0x02;
	rt_put_be16(th + 14, 8192);
	rt_mon_mask = 1 << RT_MON_TCP_CONNECT;
	verdict[RT_MON_TCP_CONNECT] = RT_MA_CONTINUE;
	rx_ip(RT_IPPROTO_TCP, th, sizeof(th));
	CHECK(calls[RT_MON_TCP_CONNECT] == 1 && sent_tcp(0x14) >= 0);
	verdict[RT_MON_TCP_CONNECT] = RT_MA_DROP;
	rx_ip(RT_IPPROTO_TCP, th, sizeof(th));
	CHECK(calls[RT_MON_TCP_CONNECT] == 2 && sent_tcp(0x14) < 0);
	verdict[RT_MON_TCP_CONNECT] = RT_MA_DROPRESET;
	rx_ip(RT_IPPROTO_TCP, th, sizeof(th));
	{
		int i = sent_tcp(0x14);
		CHECK(calls[RT_MON_TCP_CONNECT] == 3 && i >= 0);
		if (i >= 0)
			CHECK(rt_get_be32(rec[i].data + 28) == 1001 && rt_get_be16(rec[i].data + 20) == 5001);
	}
	th[13] = 0x10;                       /* an ACK is not a connection request */
	rx_ip(RT_IPPROTO_TCP, th, sizeof(th));
	CHECK(calls[RT_MON_TCP_CONNECT] == 3);

	/* MHT_Packet: both directions */
	rt_mon_mask = 1 << RT_MON_PACKET;
	pkt_verdict = RT_MA_CONTINUE;
	icmp_echo();
	CHECK(pkt_calls[0] == 1 && pkt_calls[1] == 1 && sent_icmp(RT_ICMP_ECHOREPLY) >= 0);
	pkt_verdict = RT_MA_DROP;
	errs = eth0.stats.iqdrops;
	icmp_echo();
	CHECK(pkt_calls[0] == 2 && pkt_calls[1] == 1 && nrec == 0 && eth0.stats.iqdrops == errs + 1);
	{
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 20);
		memset(rt_buf_put(b, 20), 0x45, 20);
		CHECK(rt_netif_xmit(&eth0, b, peer, RT_ETH_P_IP) == -RT_EACCES && nrec == 0);
	}
	rt_mon_mask = 0;
}

/* ---- statistics ---- */

static void test_netstat(void)
{
	static u8 buf[1024];
	struct rt_sockctx sc;
	struct rt_sockaddr_in sin;
	u32 v[64];
	int fd, n, i, found = 0;

	CHECK(rt_bsd_netstat(RT_NETSTATUS_ICMP, NULL, 0) == RT_BSD_ICMPSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_IGMP, NULL, 0) == RT_BSD_IGMPSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_IP, NULL, 0) == RT_BSD_IPSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_MB, NULL, 0) == RT_BSD_MBSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_MRT, NULL, 0) == RT_BSD_MRTSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_RT, NULL, 0) == RT_BSD_RTSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_TCP, NULL, 0) == RT_BSD_TCPSTAT_SIZE);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_UDP, NULL, 0) == RT_BSD_UDPSTAT_SIZE);
	CHECK(rt_bsd_netstat(8, NULL, 0) == -RT_EINVAL);
	CHECK(rt_bsd_netstat(99, buf, sizeof(buf)) == -RT_EINVAL);

	/* icmpstat: echo requests in inhist[8], replies in outhist[0] */
	CHECK(rt_bsd_netstat(RT_NETSTATUS_ICMP, (u8 *)v, sizeof(v)) == RT_BSD_ICMPSTAT_SIZE);
	CHECK(v[3 + 19 + 5 + 8] == rt_icmpstat.InEchos && rt_icmpstat.InEchos >= 5);
	CHECK(v[3 + 0] == rt_icmpstat.OutEchoReps && v[3 + 0] >= 2);
	CHECK(v[3 + 3] == rt_icmpstat.OutDestUnreachs && v[3 + 3] >= 1);
	CHECK(v[3 + 19 + 4] == rt_icmpstat.OutEchoReps);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_IP, (u8 *)v, sizeof(v)) == RT_BSD_IPSTAT_SIZE);
	CHECK(v[0] == rt_ipstat.InReceives && v[0] > 0 && v[13] == rt_ipstat.InDelivers && v[14] == rt_ipstat.OutRequests);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_UDP, (u8 *)v, sizeof(v)) == RT_BSD_UDPSTAT_SIZE);
	CHECK(v[4] == rt_udpstat.NoPorts && v[4] >= 1 && v[2] == rt_udpstat.InCsumErrors && v[8] == rt_udpstat.OutDatagrams);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_TCP, (u8 *)v, sizeof(v)) == RT_BSD_TCPSTAT_SIZE);
	CHECK(v[25] == rt_tcpstat.InSegs && v[25] >= 3 && v[15] == rt_tcpstat.OutSegs && v[24] == rt_tcpstat.OutRsts);
	/* a short copy */
	memset(v, 0xff, sizeof(v));
	CHECK(rt_bsd_netstat(RT_NETSTATUS_TCP, (u8 *)v, 8) == RT_BSD_TCPSTAT_SIZE && v[2] == 0xffffffffu);

	/* a bound UDP socket in the socket table */
	CHECK(rt_ctx_init(&sc, 16, NULL) == 0);
	fd = rt_api_socket(&sc, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_port = rt_htons(5353);
	CHECK(rt_api_bind(&sc, fd, &sin, sizeof(sin)) == 0);
	rt_lock();
	n = rt_bsd_netstat(RT_NETSTATUS_UDP_SOCKETS, NULL, 0);
	CHECK(n >= RT_BSD_PCD_SIZE && n % RT_BSD_PCD_SIZE == 0);
	CHECK(rt_bsd_netstat(RT_NETSTATUS_UDP_SOCKETS, buf, sizeof(buf)) == n);
	rt_unlock();
	for (i = 0; i < n; i += RT_BSD_PCD_SIZE)
		if (get16(buf + i + 10) == rt_htons(5353) && get32(buf + i + 20) == 0xffffffffu)
			found = 1;
	CHECK(found);
	rt_lock();
	CHECK(rt_bsd_netstat(RT_NETSTATUS_TCP_SOCKETS, NULL, 0) % RT_BSD_PCD_SIZE == 0);
	rt_unlock();
	rt_api_close(&sc, fd);
	rt_ctx_destroy(&sc);
}

/* ---- netdb enumeration ---- */

static void test_netdb(void)
{
	unsigned i;
	int tcp = 0, ftp = 0;

	for (i = 0; rt_protoent_at(i); i++)
		if (rt_protoent_at(i)->proto == RT_IPPROTO_TCP)
			tcp = 1;
	CHECK(tcp && i >= 5 && rt_protoent_at(i) == NULL);
	for (i = 0; rt_servent_at(i); i++)
		if (!strcmp(rt_servent_at(i)->name, "ftp"))
			ftp = 1;
	CHECK(ftp && i >= 20);
	CHECK(rt_netent_at(0) && !strcmp(rt_netent_at(0)->name, "loopback") && rt_netent_at(0)->net == 127);
	CHECK(rt_netent_at(1) == NULL);
}

/* ---- address change hook ---- */

static int addr_changes;

static void addr_hook(struct rt_netif *nif)
{
	addr_changes++;
}

static void test_addr_hook(void)
{
	rt_netif_addr_hook = addr_hook;
	CHECK(rt_netif_addr_add(&eth0, rt_htonl(0xc0a80b02u), rt_htonl(0xffffff00u), 0) == 0);
	CHECK(addr_changes == 1);
	CHECK(rt_netif_addr_del(&eth0, rt_htonl(0xc0a80b02u)) == 0);
	CHECK(addr_changes == 2);
	CHECK(rt_netif_set_addr(&eth0, rt_htonl(OUR), rt_htonl(0xffffff00u), 0) == 0);
	CHECK(addr_changes == 3);
	rt_netif_addr_hook = NULL;
}

int main(void)
{
	CHECK(rt_init() == 0);
	rt_log_level = 0;
	rt_strlcpy(eth0.name, "eth0", sizeof(eth0.name));
	eth0.type = RT_IFT_ETHER;
	eth0.hwlen = 6;
	memcpy(eth0.hwaddr, mac0, 6);
	eth0.flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
	eth0.mtu = 1500;
	eth0.link_output = fake_output;
	CHECK(rt_netif_add(&eth0) == 0);
	CHECK(rt_netif_set_addr(&eth0, rt_htonl(OUR), rt_htonl(0xffffff00u), 0) == 0);
	rt_netif_up(&eth0);
	test_routes();
	test_icmp_and_hooks();
	test_netstat();
	test_netdb();
	test_addr_hook();
	printf("t_bsdinfo_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
