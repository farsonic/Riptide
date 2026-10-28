/*
 * Riptide - RiptidePrefs: the pages and settings, once for both editors
 * (see prefs_fields.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <string.h>
#include "rt/rt_types.h"
#include "prefs/prefs_fields.h"

const struct rp_page rp_pages[RP_NPAGES] = {
	{ "Interface", "Interface", "Interface changes need a restart of Riptide",
	  "The network card Riptide uses: its SANA-II driver and unit, and how big its packets may be." },
	{ "Address", "Address", "Address changes need a restart of Riptide",
	  "How the Amiga gets its IPv4 address: from a DHCP server, or typed in here." },
	{ "Extra addresses", "Extra addresses", "A.B.C.D/BITS or A.B.C.D netmask M (up to 8)",
	  "More IPv4 addresses on the same card, for example to reach a second network on the same cable." },
	{ "VLANs", "VLANs", "ID dhcp|A.B.C.D/BITS|up [pcp=0-7] (up to 8)",
	  "Tagged networks (802.1Q) on the same card. Each VLAN is an interface of its own." },
	{ "Names", "Names", "Empty fields are left out of the file",
	  "The Amiga's name, its domain and the name servers that turn names into addresses." },
	{ "Time", "Time", "Zone: auto or +10:00. No servers: from DHCP",
	  "Setting the clock from the network, the time zone, and answering to amiga.local." },
	{ "TCP", "TCP", "Empty or Default: Riptide's built-in value",
	  "How quickly Riptide acknowledges data, and whether it uses ECN." },
	{ "Advanced: Connecting", "Connecting", "Empty or Default: Riptide's built-in value",
	  "Fast Open, and how long and how often Riptide tries to reach a server." },
	{ "Advanced: Ports", "Ports", "Empty or Default: Riptide's built-in value",
	  "Giving up on connections, closed connections and the local port numbers." },
	{ "Advanced: Keepalive", "Keepalive", "Keepalive all: idle seconds, 0 = off",
	  "Probes that keep idle connections alive through routers and notice dead ones." },
	{ "Advanced: Background", "Background", "Programs a,b*  Ports 20,21,6881-6889",
	  "Background transfers give way to everything else, so big downloads do not slow down browsing." },
	{ "Advanced: Memory", "Memory", "Empty: from the free memory at start",
	  "How much memory connections may use for their buffers before Riptide holds back." },
	{ "Advanced: Happy Eyeballs", "Happy Eyeballs", "Empty or Default: Riptide's built-in value",
	  "Trying IPv6 and IPv4 side by side when a name has both, and caching failed lookups." },
	{ "Advanced: QoS", "QoS", "QoS bands: for slow links (off by default)",
	  "Priority queues: interactive traffic goes out before bulk transfers." },
	{ "Advanced: QoS rules", "QoS rules", "port=22,23 priority=interactive dscp=EF",
	  "Rules that give connections a priority by port or program." },
	{ "Other lines", "Other lines", "Lines RiptidePrefs does not edit itself",
	  "Lines of riptide.conf that have no setting here. They are kept as they are." },
};

int rp_page_list(int page)
{
	switch (page) {
	case P_ALIAS: return RP_LIST_ALIAS;
	case P_QRULE: return RP_LIST_QRULE;
	case P_VLAN:  return RP_LIST_VLAN;
	case P_OTHER: return RP_LIST_OTHER;
	}
	return -1;
}

#define M(f) offsetof(struct rp_model, f)
#define MS(f) M(f), sizeof(((struct rp_model *)0)->f)
#define SC(i) M(sc) + (i) * sizeof(long), sizeof(long)
#define SS(i) M(ss) + (i) * RP_SSLEN, RP_SSLEN

static const char *const cy_dhcp[] = { "DHCP", "Static", NULL };
static const long cyv_dhcp[] = { 1, 0 };
static const char *const cy_tri[] = { "Default", "On", "Off", NULL };
static const long cyv_tri[] = { -1, 1, 0 };
static const char *const cy_ack[] = { "Default", "Off", "On", NULL };
static const long cyv_ack[] = { RP_UNSET, 0, 1 };
static const char *const cy_sw[] = { "Default", "On", "Off", NULL };
static const long cyv_sw[] = { RP_UNSET, 1, 0 };
static const char *const cy_backoff[] = { "Default", "Linear", "Doubling", NULL };
static const long cyv_backoff[] = { RP_UNSET, 0, 1 };
static const char *const cy_icmp[] = { "Default", "Keep trying", "2nd report", "At once", NULL };
static const long cyv_icmp[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_mtup[] = { "Default", "Off", "On black hole", "Always", NULL };
static const long cyv_mtup[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_ecn[] = { "Default", "Off", "On", "Accept only", NULL };
static const long cyv_ecn[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_tw[] = { "Default", "Off", "On", "Loopback only", NULL };
static const long cyv_tw[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_cc[] = { "Default", "CUBIC", "Reno", "LEDBAT", NULL };
static const long cyv_cc[] = { RP_UNSET, 0, 1, 2 };   /* rp_cc_names */
static const char *const cy_bg[] = { "Default", "Off", "Receive", "Send", "Both", NULL };
static const long cyv_bg[] = { RP_UNSET, 0, 1, 2, 3 };
static const char *const cy_prefer[] = { "Default", "RFC 6724", "IPv6", "IPv4", NULL };
static const long cyv_prefer[] = { RP_UNSET, 0, 6, 4 };

const struct rp_field rp_fields[] = {
	{ P_IFACE, F_STR,   0, MS(ifname),    0, 0,        "_Interface name", NULL, NULL,
	  "The name Riptide gives the card, such as eth0. Commands and the other pages use it." },
	{ P_IFACE, F_STR,   0, MS(device),    0, 0,        "_Device", NULL, NULL,
	  "The SANA-II driver of the network card, such as a2065.device or uaenet.device." },
	{ P_IFACE, F_NUM,   0, MS(unit),      0, 255,      "U_nit", NULL, NULL,
	  "Which card the driver should use when it handles several. Usually 0." },
	{ P_IFACE, F_NUM,   0, MS(mtu),       68, 65535,   "_MTU", NULL, NULL,
	  "The largest packet in bytes. Leave it empty to use the driver's value (1500 on Ethernet)." },
	{ P_IFACE, F_NUM,   FL_ZERO, MS(tcpwindow), 536, 16777216, "TCP _window", NULL, NULL,
	  "Limits how much data a connection on this card may have on the way. Empty or 0: no limit." },
	{ P_IFACE, F_NUM,   0, MS(reads),     4, 128,      "_Reads", NULL, NULL,
	  "How many receive requests Riptide keeps waiting at the driver. More can help fast cards." },
	{ P_IFACE, F_CHECK, 0, MS(ipv4),      0, 0,        "IPv_4", NULL, NULL,
	  "Use IPv4 on this card. Turn it off for an IPv6-only network." },
	{ P_IFACE, F_CHECK, FL_RIGHT, MS(ipv6), 0, 0,      "IP_v6", NULL, NULL,
	  "Use IPv6 on this card. Riptide then configures its addresses by itself." },
	{ P_ADDR,  F_CYCLE, 0, MS(dhcp),      0, 0,        "_Mode", cy_dhcp, cyv_dhcp,
	  "DHCP asks the network for an address, router and name servers. Static uses the fields below." },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(addr),    0, 0,  "Add_ress", NULL, NULL,
	  "The Amiga's IPv4 address, such as 192.168.1.20, or 192.168.1.20/24 with the network size." },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(netmask), 0, 0,  "_Netmask", NULL, NULL,
	  "The size of the local network, such as 255.255.255.0. Not needed when the address ends in /24." },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(gateway), 0, 0,  "_Gateway", NULL, NULL,
	  "The router that leads to the Internet, such as 192.168.1.1." },
	{ P_ADDR,  F_CHECK, FL_DHCP, MS(linklocal), 0, 0,  "_Link-local if no DHCP", NULL, NULL,
	  "When no DHCP server answers, take a 169.254.x.y address so a direct cable still works." },
	{ P_NAMES, F_STR,   0, MS(hostname),  0, 0,        "_Host name", NULL, NULL,
	  "The Amiga's name on the network, such as amy." },
	{ P_NAMES, F_STR,   0, MS(domain),    0, 0,        "_Domain", NULL, NULL,
	  "Added to short names when looking them up, such as home.lan." },
	{ P_NAMES, F_STR,   0, M(dns), RP_ADDR, 0, 0,      "DNS server _1", NULL, NULL,
	  "A name server's IPv4 or IPv6 address. Empty: the ones DHCP gives are used." },
	{ P_NAMES, F_STR,   0, M(dns) + RP_ADDR, RP_ADDR, 0, 0, "DNS server _2", NULL, NULL,
	  "A second name server, asked when the first one does not answer." },
	{ P_NAMES, F_STR,   0, M(dns) + 2 * RP_ADDR, RP_ADDR, 0, 0, "DNS server _3", NULL, NULL,
	  "A third name server." },
	{ P_NAMES, F_NUM,   FL_ZERO, SC(RP_SC_DNS_EDNS), 512, 4096, "_EDNS size (0 = off)", NULL, NULL,
	  "The largest name server answer asked for over UDP, in bytes. 0 turns EDNS off for old servers." },
	{ P_NAMES, F_CYCLE, 0, SC(RP_SC_DNS_TCP), 0, 0,    "DNS over _TCP", cy_sw, cyv_sw,
	  "Ask again over TCP when an answer is too big for UDP." },
	{ P_TIME,  F_CHECK, 0, MS(ntp),       0, 0,        "_NTP time sync", NULL, NULL,
	  "Set the Amiga's clock from time servers on the network." },
	{ P_TIME,  F_STR,   FL_NTP, MS(ntp_servers), 0, 0, "NTP ser_vers", NULL, NULL,
	  "Time servers separated by spaces. Empty: the ones DHCP gives, else pool.ntp.org." },
	{ P_TIME,  F_STR,   0, MS(timezone),  0, 0,        "_Time zone", NULL, NULL,
	  "auto takes it from the Locale preferences. Or the offset from UTC, such as +10:00." },
	{ P_TIME,  F_CYCLE, 0, MS(mdns),      0, 0,        "_mDNS responder", cy_tri, cyv_tri,
	  "Answer to the host name with .local added, so other computers find the Amiga by name." },
	{ P_TCP,   F_CYCLE, 0, MS(quickack),  0, 0,        "_Quick ACK", cy_ack, cyv_ack,
	  "Acknowledge every packet at once instead of waiting. Costs a little speed." },
	{ P_TCP,   F_NUM,   0, MS(delack_segs), 1, 64,     "Delayed ACK se_gments", NULL, NULL,
	  "How many packets may arrive before Riptide acknowledges them." },
	{ P_TCP,   F_NUM,   0, MS(delack_ms), 1, 500,      "Delayed ACK _limit (ms)", NULL, NULL,
	  "The longest Riptide waits before acknowledging data." },
	{ P_TCP,   F_CYCLE, 0, SC(RP_SC_ECN), 0, 0,        "_ECN", cy_ecn, cyv_ecn,
	  "Lets routers mark packets instead of dropping them, so fewer have to be sent again." },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_FASTOPEN), 0, 0,   "_Fast Open", cy_sw, cyv_sw,
	  "Send the first request together with the connection setup to servers that allow it." },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_FASTOPEN_ALL), 0, 0, "Fast Open _defer all", cy_sw, cyv_sw,
	  "Use Fast Open for every program, not only those that ask for it." },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_FO_BLACKHOLE), 0, 86400, "Fast Open _backoff (s)", NULL, NULL,
	  "How long Fast Open stays off after a network dropped it." },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_SYN_RTO_INIT), 200, 3000, "SYN first _RTO (ms)", NULL, NULL,
	  "How long Riptide waits for a server before it tries to connect again." },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_SYN_RTO_MAX), 200, 120000, "SYN _max RTO (ms)", NULL, NULL,
	  "The longest wait between two tries to connect." },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_SYN_BACKOFF), 0, 0, "SYN back_off", cy_backoff, cyv_backoff,
	  "Whether the wait between tries grows steadily or doubles each time." },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_MTU_PROBING), 0, 0, "M_TU probing", cy_mtup, cyv_mtup,
	  "Find a smaller packet size when big packets vanish on the way, as behind some VPNs." },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_CONNECT_TIMEOUT), 0, 600, "Connect ti_meout (s)", NULL, NULL,
	  "Give up connecting after this many seconds. 0: keep trying as long as TCP does." },
	{ P_PORTS, F_CYCLE, 0, SC(RP_SC_SYN_ICMP_ABORT), 0, 0, "_ICMP unreachable", cy_icmp, cyv_icmp,
	  "What a router's \"unreachable\" message does to a connection that is being set up." },
	{ P_PORTS, F_CYCLE, 0, SC(RP_SC_TW_REUSE), 0, 0,   "Time-wait _reuse", cy_tw, cyv_tw,
	  "Reuse the port of a recently closed connection for a new one to the same server." },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_MAX_TW), 0, 65536, "Time-wait _limit", NULL, NULL,
	  "The most recently closed connections Riptide remembers." },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_PORT_FIRST), 1024, 65535, "Local ports _from", NULL, NULL,
	  "The first port number Riptide picks for outgoing connections." },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_PORT_LAST), 1024, 65535, "Local ports _to", NULL, NULL,
	  "The last port number Riptide picks for outgoing connections." },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KA_ALL), 0, 32767, "_Keepalive all (s)", NULL, NULL,
	  "Send keepalive probes on every connection idle this many seconds. 0: only when a program asks." },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPIDLE), 1, 32767, "_Idle time (s)", NULL, NULL,
	  "How long a connection is idle before the first probe." },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPINTVL), 1, 32767, "Probe int_erval (s)", NULL, NULL,
	  "The time between two probes that get no answer." },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPCNT), 1, 127,  "Probe c_ount", NULL, NULL,
	  "How many unanswered probes end the connection." },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_CC), 0, 0,         "Default _TCP algorithm", cy_cc, cyv_cc,
	  "How connections share the line. LEDBAT makes every connection a background one." },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_BG_MODE), 0, 0,    "Background _mode", cy_bg, cyv_bg,
	  "Which directions of the listed programs and ports are treated as background." },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_PROGRAMS), 0, 0, "Background p_rograms", NULL, NULL,
	  "Programs whose transfers are background ones, separated by commas. * matches any ending." },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_PORTS), 0, 0,   "Remote p_orts", NULL, NULL,
	  "Server ports whose connections are background ones, such as 20,21,6881-6889." },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_LPORTS), 0, 0,  "_Local ports", NULL, NULL,
	  "Local ports whose connections are background ones." },
	{ P_BG,    F_NUM,   0, SC(RP_SC_LEDBAT_TARGET), 5, 1000, "LEDBAT tar_get (ms)", NULL, NULL,
	  "How much delay a background transfer may add before it slows down." },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_LEDBAT_SLOWDOWN), 0, 0, "LEDBAT slow_downs", cy_sw, cyv_sw,
	  "Pause background transfers now and then to measure the line without them." },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_LOW), 16, 4194304, "_Low water (KB)", NULL, NULL,
	  "Below this much buffer memory Riptide does not limit connections." },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_PRESSURE), 16, 4194304, "Pr_essure (KB)", NULL, NULL,
	  "Above this much buffer memory Riptide shrinks the buffers of busy connections." },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_HIGH), 16, 4194304, "_High water (KB)", NULL, NULL,
	  "The most buffer memory all connections together may use." },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_ENABLE), 0, 0,  "_Happy Eyeballs", cy_sw, cyv_sw,
	  "Try a name's IPv6 and IPv4 addresses side by side and keep the first that connects." },
	{ P_HE,    F_NUM,   0, SC(RP_SC_HE_DELAY), 10, 2000, "Attempt _delay (ms)", NULL, NULL,
	  "How long one address may take before the next one is tried as well." },
	{ P_HE,    F_NUM,   0, SC(RP_SC_HE_RESDELAY), 0, 1000, "_Resolution delay (ms)", NULL, NULL,
	  "How long to wait for the IPv6 answer when the IPv4 one came first." },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_PREFER), 0, 0,  "Pre_fer", cy_prefer, cyv_prefer,
	  "Which kind of address is tried first." },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_AAAA), 0, 0,    "_IPv6 lookup filter", cy_sw, cyv_sw,
	  "Do not ask for IPv6 addresses while the Amiga has no IPv6 route to the Internet." },
	{ P_HE,    F_NUM,   0, SC(RP_SC_DNS_NEGTTL), 0, 86400, "DNS _negative cache (s)", NULL, NULL,
	  "How long a name that does not exist is remembered. 0 asks again every time." },
	{ P_QOS,   F_CYCLE, 0, MS(qos),       0, 0,        "_QoS bands", cy_tri, cyv_tri,
	  "Send interactive packets before bulk ones. Helps on slow lines." },
	{ P_QOS,   F_NUM,   0, SC(RP_SC_QOS_INFLIGHT), 1, 32, "Frames in _flight", NULL, NULL,
	  "How many packets may wait at the driver. Fewer keeps priorities sharp." },
	{ P_QOS,   F_NUM,   0, SC(RP_SC_QOS_LIMIT), 8, 1024, "Band _limit (frames)", NULL, NULL,
	  "The most packets each priority band holds." },
	{ P_QOS,   F_CYCLE, 0, SC(RP_SC_QOS_PRIODSCP), 0, 0, "Priority sets _DSCP", cy_sw, cyv_sw,
	  "Mark packets for routers according to their priority." },
};
const int rp_nfields = (int)(sizeof(rp_fields) / sizeof(rp_fields[0]));

long rp_fget(const struct rp_model *m, int fi)
{
	const void *p = (const char *)m + rp_fields[fi].off;
	return rp_fields[fi].size == sizeof(long) ? *(const long *)p : (long)*(const int *)p;
}

void rp_fset(struct rp_model *m, int fi, long v)
{
	void *p = (char *)m + rp_fields[fi].off;
	if (rp_fields[fi].size == sizeof(long))
		*(long *)p = v;
	else
		*(int *)p = (int)v;
}

char *rp_fstr(struct rp_model *m, int fi)
{
	return (char *)m + rp_fields[fi].off;
}

int rp_fcycle(const struct rp_model *m, int fi)
{
	const struct rp_field *f = &rp_fields[fi];
	long v = rp_fget(m, fi);
	int a;

	for (a = 0; f->cy[a]; a++)
		if (f->cyv[a] == v)
			return a;
	return 0;
}

int rp_fdisabled(const struct rp_model *m, int fi)
{
	int fl = rp_fields[fi].flags;
	return ((fl & FL_STATIC) && m->dhcp) || ((fl & FL_DHCP) && !m->dhcp) || ((fl & FL_NTP) && !m->ntp);
}

void rp_flabel(int fi, char *buf, int len)
{
	const char *s = rp_fields[fi].label;
	int i = 0;

	for (; *s && i < len - 1; s++)
		if (*s != '_')
			buf[i++] = *s;
	buf[i] = 0;
}

char rp_fkey(int fi)
{
	const char *u = strchr(rp_fields[fi].label, '_');
	char ch = u ? u[1] : 0;
	return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
}

int rp_find(const struct rp_model *m, const void *addr)
{
	int i;
	for (i = 0; i < rp_nfields; i++)
		if ((const char *)m + rp_fields[i].off == (const char *)addr)
			return i;
	return -1;
}

int rp_fparse(struct rp_model *m, int fi, const char *text, char *err, int errlen)
{
	const struct rp_field *f = &rp_fields[fi];
	const char *s = text, *end;
	unsigned long v;

	while (*s == ' ')
		s++;
	if (f->type == F_STR) {
		char *d = rp_fstr(m, fi);
		size_t n;
		rt_strlcpy(d, s, f->size);
		n = strlen(d);
		while (n > 0 && d[n - 1] == ' ')
			d[--n] = 0;
		return 0;
	}
	if (f->type != F_NUM)
		return 0;
	if (!*s) {
		rp_fset(m, fi, RP_UNSET);
		return 0;
	}
	v = rt_strtoul(s, &end, 10);
	while (*end == ' ')
		end++;
	if (end == s || *end || (((long)v < f->min || (long)v > f->max) && !(v == 0 && (f->flags & FL_ZERO)))) {
		if (f->flags & FL_ZERO)
			rt_snprintf(err, (size_t)errlen, "enter 0 or %ld..%ld, or leave it empty", f->min, f->max);
		else
			rt_snprintf(err, (size_t)errlen, "enter %ld..%ld, or leave it empty", f->min, f->max);
		return -1;
	}
	rp_fset(m, fi, (long)v);
	return 0;
}

/* each word of s is a host name or an address */
static int all_names(const char *s)
{
	char tok[RP_STR];
	int n;

	while (*s) {
		while (*s == ' ')
			s++;
		for (n = 0; *s && *s != ' '; s++)
			if (n < (int)sizeof(tok) - 1)
				tok[n++] = *s;
		tok[n] = 0;
		if (n && !rp_valid_name(tok) && !rp_valid_ip(tok))
			return 0;
	}
	return 1;
}

/* the failing field and its reason */
static int bad(const struct rp_model *m, const void *addr, const char *why, int *fi, char *err, int errlen)
{
	*fi = rp_find(m, addr);
	if (*fi < 0)
		*fi = 0;
	rt_strlcpy(err, why, (size_t)errlen);
	return -1;
}

int rp_validate(const struct rp_model *m, int *fi, char *err, int errlen)
{
	int i;

	if (!m->ifname[0] || strchr(m->ifname, ' '))
		return bad(m, m->ifname, "enter a name without spaces, e.g. eth0", fi, err, errlen);
	if (!m->device[0])
		return bad(m, m->device, "enter the SANA-II device, e.g. uaenet.device", fi, err, errlen);
	if (!m->dhcp) {
		if (!rp_valid_ip4(m->addr, 1))
			return bad(m, m->addr, "enter an IPv4 address, e.g. 192.168.1.20 or 192.168.1.20/24", fi, err,
			           errlen);
		if (m->netmask[0] && !rp_valid_ip4(m->netmask, 0))
			return bad(m, m->netmask, "enter a mask, e.g. 255.255.255.0", fi, err, errlen);
		if (m->gateway[0] && !rp_valid_ip4(m->gateway, 0))
			return bad(m, m->gateway, "enter the router's IPv4 address", fi, err, errlen);
	}
	if (m->hostname[0] && !rp_valid_name(m->hostname))
		return bad(m, m->hostname, "letters, digits, '-' and '.' only", fi, err, errlen);
	if (m->domain[0] && !rp_valid_name(m->domain))
		return bad(m, m->domain, "letters, digits, '-' and '.' only", fi, err, errlen);
	for (i = 0; i < RP_NDNS; i++)
		if (m->dns[i][0] && !rp_valid_ip(m->dns[i]))
			return bad(m, m->dns[i], "enter an IPv4 or IPv6 address", fi, err, errlen);
	if (m->ntp && m->ntp_servers[0] && !all_names(m->ntp_servers))
		return bad(m, m->ntp_servers, "host names or addresses, separated by spaces", fi, err, errlen);
	if (m->timezone[0] && !rp_valid_tz(m->timezone))
		return bad(m, m->timezone, "auto, or the offset from UTC: +10:00, -5, +5:30", fi, err, errlen);
	if (!rp_valid_programs(m->ss[RP_SS_BG_PROGRAMS]))
		return bad(m, m->ss[RP_SS_BG_PROGRAMS], "names separated by commas, e.g. AmiFTP,wget*", fi, err,
		           errlen);
	for (i = RP_SS_BG_PORTS; i <= RP_SS_BG_LPORTS; i++)
		if (!rp_valid_ports(m->ss[i]))
			return bad(m, m->ss[i], "ports or ranges separated by commas, e.g. 20,21,6881-6889", fi, err,
			           errlen);
	if (m->sc[RP_SC_PORT_FIRST] != RP_UNSET && m->sc[RP_SC_PORT_LAST] != RP_UNSET &&
	    m->sc[RP_SC_PORT_FIRST] > m->sc[RP_SC_PORT_LAST])
		return bad(m, &m->sc[RP_SC_PORT_LAST], "the last port must not be below the first", fi, err, errlen);
	if (m->sc[RP_SC_SYN_RTO_INIT] != RP_UNSET && m->sc[RP_SC_SYN_RTO_MAX] != RP_UNSET &&
	    m->sc[RP_SC_SYN_RTO_INIT] > m->sc[RP_SC_SYN_RTO_MAX])
		return bad(m, &m->sc[RP_SC_SYN_RTO_MAX], "must not be below the first RTO", fi, err, errlen);
	if ((m->sc[RP_SC_MEM_LOW] != RP_UNSET && m->sc[RP_SC_MEM_PRESSURE] != RP_UNSET &&
	     m->sc[RP_SC_MEM_LOW] > m->sc[RP_SC_MEM_PRESSURE]) ||
	    (m->sc[RP_SC_MEM_PRESSURE] != RP_UNSET && m->sc[RP_SC_MEM_HIGH] != RP_UNSET &&
	     m->sc[RP_SC_MEM_PRESSURE] > m->sc[RP_SC_MEM_HIGH]))
		return bad(m, &m->sc[RP_SC_MEM_PRESSURE], "low <= pressure <= high water", fi, err, errlen);
	return 0;
}

int rp_list_entry_valid(int page, const char *s, char *err, int errlen)
{
	if (page == P_ALIAS && !rp_valid_alias(s)) {
		rt_strlcpy(err, "Extra address: enter A.B.C.D/BITS, e.g. 10.0.0.5/24,\nor A.B.C.D netmask M",
		           (size_t)errlen);
		return 0;
	}
	if (page == P_VLAN && !rp_valid_vlan(s)) {
		rt_strlcpy(err, "VLAN: the id (1-4094), then dhcp, an address\nA.B.C.D/BITS or up, and pcp=0-7 or "
		           "pcp=priority\nif the frames need a priority; e.g. 10 dhcp", (size_t)errlen);
		return 0;
	}
	if (page == P_QRULE && !rp_valid_qrule(s)) {
		rt_strlcpy(err, "QoS rule: port=, lport=, rport= or program=, and\npriority=interactive|default|bulk|0-7 "
		           "or dscp=EF|CS1|AF41|0-63\n(proto=tcp|udp optional; at most 8 ports)", (size_t)errlen);
		return 0;
	}
	return 1;
}

const char *rp_list_full_msg(int page)
{
	return page == P_ALIAS ? "At most 8 extra addresses" : page == P_VLAN ? "At most 8 VLANs" :
	       "At most 16 QoS rules";
}
