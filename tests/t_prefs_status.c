/*
 * Riptide - tests of RiptidePrefs' Status page model and wording
 * (src/amiga/prefs/prefs_status.c): rates over counters that wrap, the
 * CPU share, graph scales and history, the text of each part, and the
 * model fed with table records the way the editor reads them.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/amiga/prefs/prefs_status.c"

static int checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { failures++; \
	printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static u32 ip(int a, int b, int c, int d)
{
	u32 v;
	u8 *p = (u8 *)&v;
	p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)c; p[3] = (u8)d;
	return v;
}

static void test_rates(void)
{
	CHECK(rp_rate(100, 1100, 1000) == 1000);
	CHECK(rp_rate(0xfffffc00UL, 0x400, 1000) == 0x800);       /* wrapped */
	CHECK(rp_rate(0, 2000000000UL, 2000) == 1000000000UL);    /* no overflow */
	CHECK(rp_rate(5, 9, 0) == 0);
	CHECK(rp_rate(0, 4000000000UL, 4000) == 1000000000UL);
	CHECK(rp_cpu_permille(0, 0, 500, 1000) == 500);
	CHECK(rp_cpu_permille(0, 0, 0, 709379) == 1000);
	CHECK(rp_cpu_permille(100, 0, 100 + 709379 / 4, 709379) == 750);
	CHECK(rp_cpu_permille(0, 0, 2000, 1000) == 0);            /* more idle than time: 0 */
	CHECK(rp_cpu_permille(0, 0xffffff00UL, 0, 0x100) == 1000); /* clock wrapped */
	CHECK(rp_cpu_permille(0, 0, 0, 0) == 0);
	{ u32 x = rp_cpu_permille(0, 0, 7093790 / 10, 7093790); CHECK(x >= 899 && x <= 900); } /* 10 s at the E clock */
}

static void test_scale(void)
{
	struct rp_hist h;
	int i;

	CHECK(rp_nice_scale(0, 0) == 1);
	CHECK(rp_nice_scale(3, 0) == 5);
	CHECK(rp_nice_scale(7, 0) == 10);
	CHECK(rp_nice_scale(10, 0) == 10);
	CHECK(rp_nice_scale(1001, 0) == 2000);
	CHECK(rp_nice_scale(150, 1024) == 2000);
	CHECK(rp_nice_scale(4000000000UL, 0) == 0xffffffffUL);
	CHECK(rp_scale_y(50, 100, 40) == 20);
	CHECK(rp_scale_y(150, 100, 40) == 40);
	CHECK(rp_scale_y(0, 100, 40) == 0);
	CHECK(rp_scale_y(3000000000UL, 4000000000UL, 100) == 75);
	CHECK(rp_scale_y(5, 0, 40) == 0);
	rp_hist_clear(&h);
	CHECK(h.n == 0 && rp_hist_max(&h, -1) == 0);
	for (i = 0; i < 70; i++)
		rp_hist_push(&h, (u32)i, (u32)(200 - i));
	CHECK(h.n == RP_HIST);
	CHECK(rp_hist_at(&h, 0, 0) == 10);                       /* the oldest left */
	CHECK(rp_hist_at(&h, 0, RP_HIST - 1) == 69);
	CHECK(rp_hist_at(&h, 1, RP_HIST - 1) == 131);
	CHECK(rp_hist_max(&h, 0) == 69);
	CHECK(rp_hist_max(&h, 1) == 190);
	CHECK(rp_hist_max(&h, -1) == 190);
	CHECK(rp_hist_at(&h, 0, RP_HIST) == 0);
}

static void test_words(void)
{
	char b[80];
	u8 a6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	u8 a4[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 2 };

	rp_fmt_bytes(b, sizeof(b), 0, 512); CHECK_STR(b, "512 B");
	rp_fmt_bytes(b, sizeof(b), 0, 12595); CHECK_STR(b, "12.2 KB");
	rp_fmt_bytes(b, sizeof(b), 0, 1024); CHECK_STR(b, "1.00 KB");
	rp_fmt_bytes(b, sizeof(b), 0, 4781507); CHECK_STR(b, "4.56 MB");
	rp_fmt_bytes(b, sizeof(b), 0, 1048575); CHECK_STR(b, "1023 KB");
	rp_fmt_bytes(b, sizeof(b), 1, 0); CHECK_STR(b, "4.00 GB");
	rp_fmt_rate(b, sizeof(b), 0); CHECK_STR(b, "0 B/s");
	rp_fmt_rate(b, sizeof(b), 713728); CHECK_STR(b, "697 KB/s");
	rp_fmt_count(b, sizeof(b), 0); CHECK_STR(b, "0");
	rp_fmt_count(b, sizeof(b), 999); CHECK_STR(b, "999");
	rp_fmt_count(b, sizeof(b), 1234567); CHECK_STR(b, "1 234 567");
	rp_fmt_count(b, 5, 1234567); CHECK_STR(b, "1 23");      /* cut, still ended */
	rp_fmt_dur(b, sizeof(b), 45); CHECK_STR(b, "45 s");
	rp_fmt_dur(b, sizeof(b), 720); CHECK_STR(b, "12 min");
	rp_fmt_dur(b, sizeof(b), 3 * 3600 + 5 * 60); CHECK_STR(b, "3 h 05 min");
	rp_fmt_dur(b, sizeof(b), 2 * 86400 + 4 * 3600); CHECK_STR(b, "2 days 4 h");
	rp_fmt_dur(b, sizeof(b), 86400); CHECK_STR(b, "1 day 0 h");
	rp_fmt_uptime(b, sizeof(b), 45); CHECK_STR(b, "0:00:45");
	rp_fmt_uptime(b, sizeof(b), 3 * 3600 + 4 * 60 + 5); CHECK_STR(b, "3:04:05");
	rp_fmt_uptime(b, sizeof(b), 86400 + 61); CHECK_STR(b, "1 day 0:01:01");
	rp_fmt_offset(b, sizeof(b), 12); CHECK_STR(b, "+0.012 s");
	rp_fmt_offset(b, sizeof(b), -1250); CHECK_STR(b, "-1.250 s");
	rp_fmt_offset(b, sizeof(b), 12345); CHECK_STR(b, "+12 s");
	rp_fmt_offset(b, sizeof(b), (s32)0x80000000UL); CHECK(b[0] == '-');
	rp_fmt_ms(b, sizeof(b), 400); CHECK_STR(b, "0.4 ms");
	rp_fmt_ms(b, sizeof(b), 9950); CHECK_STR(b, "9.9 ms");
	rp_fmt_ms(b, sizeof(b), 12000); CHECK_STR(b, "12 ms");
	rp_fmt_ms(b, sizeof(b), 1200000); CHECK_STR(b, "1 200 ms");
	rp_fmt_ip4(b, sizeof(b), ip(192, 168, 1, 2)); CHECK_STR(b, "192.168.1.2");
	rp_fmt_ip4(b, sizeof(b), 0); CHECK_STR(b, "-");
	rp_fmt_ip4_plen(b, sizeof(b), ip(192, 168, 1, 2), ip(255, 255, 255, 0)); CHECK_STR(b, "192.168.1.2/24");
	rp_fmt_ip4_plen(b, sizeof(b), ip(10, 1, 2, 3), ip(255, 255, 240, 0)); CHECK_STR(b, "10.1.2.3/20");
	rp_fmt_addr_port(b, sizeof(b), a4, 80); CHECK_STR(b, "10.0.0.2:80");
	rp_fmt_addr_port(b, sizeof(b), a4, 0); CHECK_STR(b, "10.0.0.2");
	rp_fmt_addr_port(b, sizeof(b), a6, 443); CHECK_STR(b, "[2001:db8::1]:443");
	rp_fmt_addr_port(b, sizeof(b), a6, 0); CHECK_STR(b, "2001:db8::1");
	CHECK_STR(rp_dhcp_state_name(4), "bound");
	CHECK_STR(rp_dhcp_state_name(99), "unknown");
	CHECK_STR(rp_tcp_state_words(4), "Connected");
	CHECK_STR(rp_tcp_state_words(1), "Listening");
	CHECK_STR(rp_tcp_state_words(-1), "?");
}

/* ---- the model fed with records ---- */
static struct rt_ctl_status st;
static struct rt_ctl_iface ifs[3];
static struct rt_ctl_addr6 a6[3];
static struct rt_ctl_route routes[2];
static struct rt_ctl_tcpinfo tcp[3];
static struct rt_ctl_dns dns;
static struct rt_tcp_stats tcpst;
static struct rt_ip_stats ipst;

static void setup(void)
{
	memset(&st, 0, sizeof(st));
	strcpy(st.version, "0.23");
	st.uptime_s = 3725;
	st.mem_used_kb = 64;
	st.mem_pressure_kb = 384;
	st.mem_high_kb = 512;
	st.buf_kb = 100;
	st.cpu_hz = 1000;
	st.cpu_now = 10000;
	st.cpu_idle = 5000;
	st.cpu_on = 1;
	st.ntp_state = RT_CTL_NTP_SYNCED;
	st.ntp_ago_s = 120;
	st.ntp_next_s = 3480;
	st.ntp_off_ms = -12;
	st.ntp_delay_us = 23400;
	strcpy(st.ntp_server, "pool.ntp.org");
	st.dns_hits = 7;
	memset(ifs, 0, sizeof(ifs));
	strcpy(ifs[0].name, "lo0");
	ifs[0].index = 1;
	ifs[0].type = ST_IFT_LOOP;
	ifs[0].flags = ST_IFF_UP | ST_IFF_RUNNING;
	ifs[0].mtu = 16384;
	ifs[0].ibytes = ifs[0].obytes = 1000;
	strcpy(ifs[1].name, "eth0");
	ifs[1].index = 2;
	ifs[1].type = 1;
	ifs[1].flags = ST_IFF_UP | ST_IFF_RUNNING;
	ifs[1].mtu = 1500;
	strcpy(ifs[1].descr, "uaenet.device unit 0");
	ifs[1].addr = ip(192, 168, 1, 20);
	ifs[1].netmask = ip(255, 255, 255, 0);
	ifs[1].dhcp_state = RT_DHCPS_BOUND;
	ifs[1].dhcp_router = ip(192, 168, 1, 1);
	ifs[1].dhcp_left_s = 3 * 3600 + 5 * 60;
	ifs[1].ibytes = 0xfffff000UL;          /* wraps in the next sample */
	ifs[1].obytes = 1000;
	ifs[1].ipackets = 1234567;
	strcpy(ifs[2].name, "eth0.10");
	ifs[2].index = 3;
	ifs[2].type = 1;
	ifs[2].flags = ST_IFF_UP;
	ifs[2].mtu = 1496;
	ifs[2].vlan_parent = 2;
	ifs[2].vlan_id = 10;
	ifs[2].ll_addr = ip(169, 254, 7, 9);
	ifs[2].ip6_off = 1;
	memset(a6, 0, sizeof(a6));
	a6[0].addr[0] = 0xfe; a6[0].addr[1] = 0x80; a6[0].addr[15] = 1;
	a6[0].scope = 2;
	strcpy(a6[0].ifname, "eth0");
	a6[1].addr[0] = 0x20; a6[1].addr[1] = 0x01; a6[1].addr[2] = 0x0d; a6[1].addr[3] = 0xb8; a6[1].addr[15] = 2;
	a6[1].scope = 14;
	strcpy(a6[1].ifname, "eth0");
	a6[2] = a6[1];
	a6[2].addr[15] = 3;
	a6[2].flags = RT_CTL_IN6_TENTATIVE;
	memset(routes, 0, sizeof(routes));
	routes[0].dst = ip(192, 168, 1, 0);
	routes[0].mask = ip(255, 255, 255, 0);
	routes[0].ifindex = 2;
	routes[1].gateway = ip(192, 168, 1, 254);
	routes[1].ifindex = 3;
	memset(tcp, 0, sizeof(tcp));
	tcp[0].state = 1;
	tcp[0].lport = 21;
	tcp[0].laddr[10] = tcp[0].laddr[11] = 0xff;
	strcpy(tcp[0].prog, "ftpd");
	tcp[1].state = 4;
	tcp[1].lport = 1025;
	tcp[1].fport = 80;
	tcp[1].laddr[10] = tcp[1].laddr[11] = 0xff;
	tcp[1].laddr[12] = 192; tcp[1].laddr[13] = 168; tcp[1].laddr[14] = 1; tcp[1].laddr[15] = 20;
	tcp[1].faddr[10] = tcp[1].faddr[11] = 0xff;
	tcp[1].faddr[12] = 93; tcp[1].faddr[13] = 184; tcp[1].faddr[14] = 216; tcp[1].faddr[15] = 34;
	tcp[1].srtt_us = 23400;
	tcp[1].cwnd = 10;
	tcp[1].options = RT_CTL_TCPI_ECN;
	tcp[1].bytes_received_lo = 1000;
	strcpy(tcp[1].prog, "AWeb");
	strcpy(tcp[1].cc, "cubic");
	tcp[2] = tcp[1];
	tcp[2].lport = 1026;
	tcp[2].options = 0;
	tcp[2].prog[0] = 0;
	memset(&dns, 0, sizeof(dns));
	dns.nservers = 2;
	dns.servers[0] = ip(192, 168, 1, 1);
	dns.servers[1] = ip(1, 1, 1, 1);
	strcpy(dns.hostname, "amiga");
	strcpy(dns.domain, "lan");
	memset(&tcpst, 0, sizeof(tcpst));
	tcpst.CurrEstab = 2;
	tcpst.RetransSegs = 10;
	tcpst.OutSegs = 5000;
	tcpst.TCPECNNegotiated = 1;
	memset(&ipst, 0, sizeof(ipst));
	ipst.InDiscards = 3;
}

static void feed(struct rp_status_model *m, u32 now, int full)
{
	struct rp_st_input in;

	memset(&in, 0, sizeof(in));
	in.now_ms = now;
	in.st = &st;
	in.st_len = sizeof(st);
	in.ifaces = (const u8 *)ifs;
	in.nifaces = 3;
	in.ifrec = sizeof(ifs[0]);
	in.addrs6 = (const u8 *)a6;
	in.naddrs6 = 3;
	in.a6rec = sizeof(a6[0]);
	in.routes = (const u8 *)routes;
	in.nroutes = 2;
	in.rrec = sizeof(routes[0]);
	in.tcp = (const u8 *)tcp;
	in.ntcp = 3;
	/* an older stack: tcpinfo records without the program name */
	in.trec = full ? sizeof(tcp[0]) : (u32)offsetof(struct rt_ctl_tcpinfo, prog);
	if (!full) {
		/* records closer together, as that stack would send them */
		static u8 shortrec[3][sizeof(struct rt_ctl_tcpinfo)];
		int i;
		for (i = 0; i < 3; i++)
			memcpy(shortrec[0] + (size_t)i * in.trec, &tcp[i], in.trec);
		in.tcp = shortrec[0];
	}
	in.dns = &dns;
	in.dns_len = sizeof(dns);
	in.tcpst = &tcpst;
	in.tcpst_len = sizeof(tcpst);
	in.ipst = &ipst;
	in.ipst_len = sizeof(ipst);
	rp_status_update(m, &in);
}

static void test_model(void)
{
	static struct rp_status_model m;
	char l1[120], l2[120], l3[120], b[600];

	setup();
	rp_status_init(&m);
	rp_st_summary(&m, b, sizeof(b));
	CHECK(strstr(b, "not running") != NULL);
	feed(&m, 100000, 1);
	CHECK(m.running && m.nif == 3);
	CHECK(!m.cpu_ok);                             /* one sample: no CPU share yet */
	CHECK(m.ifs[1].rx_bps == 0 && m.ifs[1].hist.n == 0);
	rp_st_summary(&m, b, sizeof(b));
	CHECK_STR(b, "Riptide 0.23 is running. Up 1:02:05.");
	/* one second later */
	st.uptime_s++;
	st.cpu_now += 1000;
	st.cpu_idle += 750;                           /* busy a quarter of the time */
	ifs[1].ibytes += 0x2000;                      /* wraps: 8192 bytes */
	ifs[1].obytes += 2048;
	ifs[0].ibytes += 50000;                       /* loopback: not in the total */
	tcp[1].bytes_received_lo += 4096;
	tcpst.RetransSegs += 3;
	feed(&m, 101000, 1);
	CHECK(m.cpu_ok && m.cpu_pm == 250);
	CHECK(m.cpu_hist.n == 1);
	CHECK(m.ifs[1].rx_bps == 8192 && m.ifs[1].tx_bps == 2048);
	CHECK(m.rx_bps == 8192 && m.tx_bps == 2048);  /* without lo0 */
	CHECK(m.total.n == 1 && rp_hist_at(&m.total, 0, 0) == 8192);
	CHECK(m.ifs[1].hist.n == 1 && m.ifs[1].rx_peak == 8192);
	CHECK(m.retrans_ps == 3);
	rp_st_summary(&m, b, sizeof(b));
	CHECK_STR(b, "Riptide 0.23 is running. Up 1:02:06. CPU 25 %.");
	rp_st_memory(&m, b, sizeof(b));
	CHECK_STR(b, "Socket memory 64 of 512 KB (pressure at 384 KB), buffers 100 KB.");
	/* interfaces */
	rp_st_if_lines(&m.ifs[1], l1, l2, l3, sizeof(l1));
	CHECK_STR(l1, "eth0  up  uaenet.device unit 0  MTU 1500");
	CHECK_STR(l2, "192.168.1.20/24 via 192.168.1.1, DHCP bound, 3 h 05 min left");
	CHECK_STR(l3, "2001:db8::2, fe80::1");       /* global first, the tentative one left out */
	rp_st_if_lines(&m.ifs[2], l1, l2, l3, sizeof(l1));
	CHECK_STR(l1, "eth0.10  up, no link  VLAN 10  MTU 1496");
	CHECK_STR(l2, "169.254.7.9/16 via 192.168.1.254, link-local");
	CHECK_STR(l3, "IPv6 off");
	rp_st_if_rates(&m.ifs[1], b, sizeof(b));
	CHECK_STR(b, "In 8.00 KB/s  Out 2.00 KB/s");
	rp_st_if_totals(&m.ifs[1], b, sizeof(b));
	CHECK(strstr(b, "1 234 567 packets") != NULL);
	/* connections: the one moving data first, the listener last */
	CHECK(m.nconn == 3 && m.nconn_all == 3);
	CHECK(m.conns[0].lport == 1025 && m.conns[0].rx_bps == 4096);
	CHECK(m.conns[2].state == 1);
	rp_st_conn_row(&m.conns[0], b, sizeof(b));
	CHECK_STR(b, "Connected\t192.168.1.20:1025\t93.184.216.34:80\t23 ms\t10 ECN\t4.00 KB/s\tAWeb");
	rp_st_conn_row(&m.conns[2], b, sizeof(b));
	CHECK_STR(b, "Listening\t0.0.0.0:21\t*\t-\t-\t-\tftpd");
	rp_st_conn_row(&m.conns[1], b, sizeof(b));
	CHECK(strstr(b, "\t-") != NULL && strstr(b, "ECN") == NULL);
	CHECK(m.conns[0].hist.n == 2 && rp_hist_at(&m.conns[0].hist, 0, 1) == 234);
	/* names and counters */
	rp_st_names_text(&m, b, sizeof(b));
	CHECK(strstr(b, "Host name: amiga.lan\n") != NULL);
	CHECK(strstr(b, "Name servers: 192.168.1.1, 1.1.1.1\n") != NULL);
	CHECK(strstr(b, "synced with pool.ntp.org 2 min ago, clock was -0.012 s off, round trip 23 ms") != NULL);
	CHECK(strstr(b, "Next sync in 58 min") != NULL);
	rp_st_counters_text(&m, b, sizeof(b));
	CHECK(strstr(b, "Resent: 13 of 5 000 segments (3 in the last second)") != NULL);
	CHECK(strstr(b, "ECN: 1 connections") != NULL);
	CHECK(strstr(b, "IP: 3 packets dropped in") != NULL);
	/* a connection goes away, an interface too: the model follows */
	tcp[1] = tcp[2];
	feed(&m, 102000, 1);
	CHECK(m.nconn == 3);
	/* an older stack: shorter tcpinfo records (no program) */
	feed(&m, 103000, 0);
	CHECK(m.nconn == 3);
	{
		int i, named = 0;
		for (i = 0; i < m.nconn; i++)
			named += m.conns[i].prog[0] != 0;
		CHECK(named == 0);
	}
	/* NTP off and failed */
	st.ntp_state = RT_CTL_NTP_OFF;
	feed(&m, 104000, 1);
	rp_st_names_text(&m, b, sizeof(b));
	CHECK(strstr(b, "NTP is off") != NULL);
	st.ntp_state = RT_CTL_NTP_FAILED;
	strcpy(st.ntp_error, "no answer from pool.ntp.org");
	feed(&m, 105000, 1);
	rp_st_names_text(&m, b, sizeof(b));
	CHECK(strstr(b, "Last try failed: no answer from pool.ntp.org") != NULL);
	/* small buffers stay terminated */
	rp_st_names_text(&m, b, 20);
	CHECK(strlen(b) < 20);
	rp_st_counters_text(&m, b, 10);
	CHECK(strlen(b) < 10);
	/* stopped */
	rp_status_stopped(&m);
	CHECK(!m.running && m.nif == 0 && m.total.n == 0);
	/* a missing table is no problem */
	{
		struct rp_st_input in;
		memset(&in, 0, sizeof(in));
		in.now_ms = 1;
		rp_status_update(&m, &in);
		CHECK(m.running && m.nif == 0 && !m.have_tcpst);
		rp_st_counters_text(&m, b, sizeof(b));
		CHECK(strstr(b, "No counters") != NULL);
	}
}

int main(void)
{
	test_rates();
	test_scale();
	test_words();
	test_model();
	printf("t_prefs_status: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
