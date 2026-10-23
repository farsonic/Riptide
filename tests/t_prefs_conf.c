/*
 * Riptide - RiptidePrefs' riptide.conf model (src/amiga/prefs/conf_edit.c):
 * byte-exact round trips (comments, unknown lines, CRLF, no final line
 * end), parsing, edits that touch only the lines of the changed settings
 * (keeping trailing comments), DHCP <-> static, the "other lines" list,
 * validation and the commands for the running stack; the Phase 3 settings
 * (ipv4, qos, sysctl table, extra addresses, QoS rules).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/amiga/prefs/conf_edit.c"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void *rp_alloc(size_t n) { return malloc(n); }
void rp_free(void *p) { free(p); }

static char *text_of(struct rp_conf *c)
{
	size_t len;
	char *t = rp_conf_text(c, &len);
	CHECK(t != NULL && strlen(t) == len);
	return t;
}

static void roundtrip(const char *name, const char *in)
{
	struct rp_conf c;
	char *t;

	CHECK(rp_conf_load(&c, in, strlen(in)) == 0);
	CHECK(!rp_conf_changed(&c));
	t = text_of(&c);
	if (strcmp(t, in)) {
		fprintf(stderr, "FAIL round trip %s:\n--- in\n%s\n--- out\n%s\n", name, in, t);
		failures++;
	}
	checks++;
	free(t);
	rp_conf_free(&c);
}

/* the installer's configuration (DHCP) */
static const char *inst_dhcp =
	"# Riptide configuration, written by Install_Riptide 0.12.\n"
	"# Riptide reads ENV:Riptide/riptide.conf; keep this copy in\n"
	"# ENVARC:Riptide/.\n"
	"\n"
	"hostname amiga\n"
	"#domain  lan\n"
	"\n"
	"# FS-UAE needs uae_sana2 = 1 and network_card = a2065.\n"
	"interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440\n"
	"\n"
	"# 1) DHCP.\n"
	"dhcp eth0\n"
	"\n"
	"# 2) Static.\n"
	"#ifconfig  eth0 10.0.2.15/24 up\n"
	"#route     add default 10.0.2.2\n"
	"#nameserver 10.0.2.3\n"
	"\n"
	"host amiga 127.0.0.1\n"
	"loglevel 1\n";

static const char *static_conf =
	"hostname   a1200   # the name\n"
	"domain lan\n"
	"interface eth0 device=\"Networks/my card.device\" unit=1 reads=64 options=\"x y\"\n"
	"ifconfig eth0 192.168.1.20 netmask 255.255.255.0 up\n"
	"route add default via 192.168.1.1\n"
	"nameserver 192.168.1.1\n"
	"nameserver add 1.1.1.1\n"
	"sysctl net.inet.tcp.delack_segs=1\n"
	"sysctl net.inet.tcp.sack 1\n"
	"ntp pool.ntp.org time.cloudflare.com\n"
	"mdns on\n";

static void test_roundtrips(void)
{
	roundtrip("empty", "");
	roundtrip("installer", inst_dhcp);
	roundtrip("static", static_conf);
	roundtrip("no final newline", "hostname a\ndhcp eth0");
	roundtrip("crlf", "hostname a\r\ninterface eth0 device=uaenet.device unit=0\r\ndhcp eth0\r\n");
	roundtrip("blank lines only", "\n\n\n");
	roundtrip("odd", "  # x\n\tinterface   eth0   device=a.device  unit=0  \nweird \"unterminated\n;semi\n");
}

static void test_parse(void)
{
	struct rp_conf c;

	CHECK(rp_conf_load(&c, inst_dhcp, strlen(inst_dhcp)) == 0);
	CHECK(!strcmp(c.cur.hostname, "amiga") && !c.cur.domain[0]);
	CHECK(!strcmp(c.cur.ifname, "eth0") && !strcmp(c.cur.device, "uaenet.device"));
	CHECK(c.cur.unit == 0 && c.cur.mtu == 1500 && c.cur.tcpwindow == 1440 && c.cur.reads == RP_UNSET);
	CHECK(c.cur.dhcp == 1 && c.l_dhcp && !c.l_ifconfig);
	CHECK(c.cur.ipv6 == 1 && c.cur.ntp == 0 && c.cur.mdns == -1);
	CHECK(rp_other_count(&c) == 2);
	CHECK(!strcmp(rp_other_get(&c, 0), "host amiga 127.0.0.1"));
	CHECK(!strcmp(rp_other_get(&c, 1), "loglevel 1"));
	rp_conf_free(&c);

	CHECK(rp_conf_load(&c, static_conf, strlen(static_conf)) == 0);
	CHECK(!strcmp(c.cur.hostname, "a1200") && !strcmp(c.cur.domain, "lan"));
	CHECK(!strcmp(c.cur.device, "Networks/my card.device") && c.cur.unit == 1 && c.cur.reads == 64);
	CHECK(!strcmp(c.cur.ifextra, "options=\"x y\""));
	CHECK(c.cur.dhcp == 0 && !strcmp(c.cur.addr, "192.168.1.20") && !strcmp(c.cur.netmask, "255.255.255.0"));
	CHECK(!strcmp(c.cur.gateway, "192.168.1.1"));
	CHECK(!strcmp(c.cur.dns[0], "192.168.1.1") && !strcmp(c.cur.dns[1], "1.1.1.1") && !c.cur.dns[2][0]);
	CHECK(c.cur.delack_segs == 1 && c.cur.quickack == RP_UNSET);
	CHECK(c.cur.ntp == 1 && !strcmp(c.cur.ntp_servers, "pool.ntp.org time.cloudflare.com"));
	CHECK(c.cur.mdns == 1);
	CHECK(rp_other_count(&c) == 1 && !strcmp(rp_other_get(&c, 0), "sysctl net.inet.tcp.sack 1"));
	rp_conf_free(&c);
}

static char *edit(const char *in, void (*fn)(struct rp_model *m))
{
	struct rp_conf c;
	char *t, *t2;

	if (rp_conf_load(&c, in, strlen(in)) != 0)
		return NULL;
	fn(&c.cur);
	t = text_of(&c);
	t2 = text_of(&c);           /* idempotent */
	CHECK(!strcmp(t, t2));
	free(t2);
	rp_conf_free(&c);
	return t;
}

static void e_host(struct rp_model *m) { strcpy(m->hostname, "amy"); }
static void e_domain(struct rp_model *m) { strcpy(m->domain, "home.arpa"); }
static void e_static(struct rp_model *m)
{
	m->dhcp = 0;
	strcpy(m->addr, "192.168.0.50/24");
	strcpy(m->gateway, "192.168.0.1");
	strcpy(m->dns[0], "192.168.0.1");
}
static void e_dhcp(struct rp_model *m) { m->dhcp = 1; }
static void e_tcpwin_off(struct rp_model *m) { m->tcpwindow = RP_UNSET; m->unit = 2; }
static void e_dns(struct rp_model *m) { strcpy(m->dns[1], ""); strcpy(m->dns[2], "9.9.9.9"); }
static void e_time(struct rp_model *m)
{
	m->ntp = 1;
	strcpy(m->ntp_servers, "pool.ntp.org");
	strcpy(m->timezone, "+10:00");
	m->mdns = 1;
	m->quickack = 1;
	m->ipv6 = 0;
}
static void e_ntp_off(struct rp_model *m) { m->ntp = 0; m->mdns = -1; m->delack_segs = RP_UNSET; }
static void e_hostname_clear(struct rp_model *m) { m->hostname[0] = 0; }
static void e_ntp_on(struct rp_model *m) { m->ntp = 1; m->ntp_servers[0] = 0; }

static void test_edits(void)
{
	char *t;

	t = edit(inst_dhcp, e_host);
	CHECK(t && strstr(t, "\nhostname amy\n#domain  lan\n"));
	CHECK(t && strlen(t) == strlen(inst_dhcp) - 2);
	free(t);

	t = edit(inst_dhcp, e_domain);       /* inserted after hostname */
	CHECK(t && strstr(t, "hostname amiga\ndomain home.arpa\n#domain  lan\n"));
	free(t);

	t = edit(static_conf, e_host);       /* trailing comment kept */
	CHECK(t && strstr(t, "hostname amy   # the name\n"));
	free(t);

	t = edit(inst_dhcp, e_static);       /* dhcp line replaced in place */
	CHECK(t && strstr(t, "# 1) DHCP.\nifconfig eth0 192.168.0.50/24 up\nroute add default 192.168.0.1\n"
	                     "nameserver 192.168.0.1\n\n# 2) Static.\n"));
	CHECK(t && !strstr(t, "\ndhcp eth0"));
	free(t);

	t = edit(static_conf, e_dhcp);       /* static lines replaced by dhcp */
	CHECK(t && strstr(t, "options=\"x y\"\ndhcp eth0\nnameserver 192.168.1.1\n"));
	CHECK(t && !strstr(t, "ifconfig") && !strstr(t, "route add"));
	free(t);

	t = edit(inst_dhcp, e_tcpwin_off);   /* interface rewritten, mtu kept */
	CHECK(t && strstr(t, "\ninterface eth0 device=uaenet.device unit=2 mtu=1500\n"));
	free(t);

	t = edit(static_conf, e_tcpwin_off); /* quoted device and options kept */
	CHECK(t && strstr(t, "interface eth0 device=\"Networks/my card.device\" unit=2 reads=64 options=\"x y\"\n"));
	free(t);

	t = edit(static_conf, e_dns);
	CHECK(t && strstr(t, "nameserver 192.168.1.1\nnameserver 9.9.9.9\nsysctl"));
	CHECK(t && !strstr(t, "1.1.1.1"));
	free(t);

	t = edit(inst_dhcp, e_time);         /* appended at the end */
	CHECK(t && strstr(t, "loglevel 1\nipv6 off\nntp pool.ntp.org\ntimezone +10:00\nmdns on\n"
	                     "sysctl net.inet.tcp.quickack=1\n"));
	free(t);

	t = edit(static_conf, e_ntp_off);
	CHECK(t && strstr(t, "\nntp off\n") && !strstr(t, "mdns") && !strstr(t, "delack_segs"));
	free(t);

	t = edit(static_conf, e_ntp_on);     /* no servers: "ntp on" (DHCP or pool.ntp.org) */
	CHECK(t && strstr(t, "\nntp on\n") && !strstr(t, "ntp off"));
	free(t);

	t = edit(static_conf, e_hostname_clear);
	CHECK(t && !strstr(t, "hostname") && !strncmp(t, "domain lan\n", 11));
	free(t);

	/* nothing to start from: the defaults become lines when changed */
	t = edit("", e_static);
	CHECK(t && !strcmp(t, "ifconfig eth0 192.168.0.50/24 up\nroute add default 192.168.0.1\n"
	                       "nameserver 192.168.0.1\n"));
	free(t);
}

static void test_other(void)
{
	struct rp_conf c;
	char *t;

	CHECK(rp_conf_load(&c, inst_dhcp, strlen(inst_dhcp)) == 0);
	CHECK(rp_other_set(&c, 1, "loglevel 2") == 0);
	CHECK(rp_other_add(&c, "user bob uid=1") == 0);
	CHECK(rp_other_set(&c, 0, "") == 0);           /* delete "host ..." */
	CHECK(rp_other_count(&c) == 2);
	CHECK(rp_other_add(&c, "# comment") < 0);
	CHECK(rp_other_set(&c, 9, "x") < 0);
	t = text_of(&c);
	CHECK(strstr(t, "\n\nloglevel 2\nuser bob uid=1\n") && !strstr(t, "host amiga"));
	free(t);
	rp_conf_free(&c);
}

static void test_valid(void)
{
	CHECK(rp_valid_ip4("10.0.2.15", 0) && rp_valid_ip4("10.0.2.15/24", 1));
	CHECK(!rp_valid_ip4("10.0.2.15/24", 0) && !rp_valid_ip4("10.0.2.15/33", 1));
	CHECK(!rp_valid_ip4("256.1.1.1", 0) && !rp_valid_ip4("1.2.3", 0) && !rp_valid_ip4("1.2.3.4.5", 0));
	CHECK(!rp_valid_ip4("", 0) && !rp_valid_ip4("a.b.c.d", 0));
	CHECK(rp_valid_name("amiga") && rp_valid_name("home.arpa") && rp_valid_name("a-1.b_2"));
	CHECK(!rp_valid_name("") && !rp_valid_name("a..b") && !rp_valid_name("bad name") && !rp_valid_name("x/y"));
	CHECK(rp_valid_ip("1.1.1.1") && rp_valid_ip("2001:db8::1") && rp_valid_ip("::1") &&
	      rp_valid_ip("fe80::1%eth0") && rp_valid_ip("::ffff:1.2.3.4"));
	CHECK(!rp_valid_ip("1::2::3") && !rp_valid_ip("hello") && !rp_valid_ip("12345::1"));
	CHECK(rp_valid_tz("auto") && rp_valid_tz("+10:00") && rp_valid_tz("-5") && rp_valid_tz("+5:30") &&
	      rp_valid_tz("0"));
	CHECK(!rp_valid_tz("UTC+10") && !rp_valid_tz("+15") && !rp_valid_tz("+10:60") && !rp_valid_tz("+10:5") &&
	      !rp_valid_tz("") && !rp_valid_tz("+"));
	{
		struct rp_conf c;
		static const char on[] = "ntp on\n";
		CHECK(rp_conf_load(&c, on, strlen(on)) == 0);
		CHECK(c.cur.ntp == 1 && !c.cur.ntp_servers[0]);
		rp_conf_free(&c);
	}
}

static char cmds[1024];
static void collect(const char *line, void *arg)
{
	strcat(cmds, line);
	strcat(cmds, ";");
}

static void test_live(void)
{
	struct rp_conf c;

	CHECK(rp_conf_load(&c, static_conf, strlen(static_conf)) == 0);
	cmds[0] = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 0 && !cmds[0]);
	strcpy(c.cur.hostname, "amy");
	strcpy(c.cur.dns[2], "9.9.9.9");
	c.cur.quickack = 1;
	c.cur.mdns = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 0);
	CHECK(!strcmp(cmds, "hostname amy;nameserver flush;nameserver add 192.168.1.1;nameserver add 1.1.1.1;"
	                    "nameserver add 9.9.9.9;mdns off;sysctl net.inet.tcp.quickack=1;"));
	c.cur.tcpwindow = 1440;
	cmds[0] = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 1);
	rp_conf_commit(&c);
	CHECK(!rp_conf_changed(&c));
	rp_conf_free(&c);
}

/* ---- Phase 3 ---- */
static const char *p3_conf =
	"interface eth0 device=uaenet.device unit=2 ipv4=on qos=on\n"
	"ifconfig eth0 192.168.1.20/24 up\n"
	"route add default 192.168.1.1\n"
	"ifconfig eth0 alias 10.0.0.5/24\n"
	"ifconfig eth0 alias 10.0.1.5 netmask 255.255.255.0   # lab\n"
	"ifconfig eth1 alias 10.9.9.9/24\n"
	"ipv6 on\n"
	"ipv4 on\n"
	"qos on\n"
	"qos rule port=22,23 priority=interactive dscp=EF\n"
	"qos rule program=AmiFTP priority=bulk\n"
	"qos rule nonsense\n"
	"sysctl net.inet.tcp.fastopen=1\n"
	"sysctl net.inet.tcp.cc=ledbat\n"
	"sysctl net.inet.tcp.bg_programs=AmiFTP,wget*\n"
	"sysctl net.inet.he.prefer 6\n"
	"sysctl net.inet.tcp.mem_high=2048   # KB\n"
	"sysctl net.inet.tcp.cc=reno\n"
	"background program=x\n";

static void test_p3_parse(void)
{
	struct rp_conf c;

	roundtrip("phase 3", p3_conf);
	CHECK(rp_conf_load(&c, p3_conf, strlen(p3_conf)) == 0);
	CHECK(c.cur.ipv4 == 1 && c.l_ipv4 && c.cur.qos == 1 && c.l_qos);
	CHECK(!strcmp(c.cur.ifextra, "ipv4=on qos=on"));
	CHECK(rp_list_count(&c, RP_LIST_ALIAS) == 2);
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_ALIAS, 0), "10.0.0.5/24"));
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_ALIAS, 1), "10.0.1.5 netmask 255.255.255.0"));
	CHECK(rp_list_get(&c, RP_LIST_ALIAS, 2) == NULL);
	CHECK(rp_list_count(&c, RP_LIST_QRULE) == 2);
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_QRULE, 1), "program=AmiFTP priority=bulk"));
	CHECK(c.cur.sc[RP_SC_FASTOPEN] == 1 && c.cur.sc[RP_SC_CC] == 2 && c.cur.sc[RP_SC_HE_PREFER] == 6);
	CHECK(c.cur.sc[RP_SC_MEM_HIGH] == 2048 && c.cur.sc[RP_SC_MEM_LOW] == RP_UNSET);
	CHECK(!strcmp(c.cur.ss[RP_SS_BG_PROGRAMS], "AmiFTP,wget*") && !c.cur.ss[RP_SS_BG_PORTS][0]);
	/* not understood: another interface, a bad rule, a second cc, the background keyword */
	CHECK(rp_other_count(&c) == 4);
	CHECK(!strcmp(rp_other_get(&c, 0), "ifconfig eth1 alias 10.9.9.9/24"));
	CHECK(!strcmp(rp_other_get(&c, 1), "qos rule nonsense"));
	CHECK(!strcmp(rp_other_get(&c, 2), "sysctl net.inet.tcp.cc=reno"));
	rp_conf_free(&c);
}

static void e_p3(struct rp_model *m)
{
	m->ipv4 = 0;
	m->qos = 0;
	m->sc[RP_SC_FASTOPEN] = RP_UNSET;
	m->sc[RP_SC_CONNECT_TIMEOUT] = 10;
	m->sc[RP_SC_CC] = 0;
	m->sc[RP_SC_HE_PREFER] = 4;
	strcpy(m->ss[RP_SS_BG_PROGRAMS], "");
	strcpy(m->ss[RP_SS_BG_PORTS], "20,21,6881-6889");
}

static void e_none_p3(struct rp_model *m)
{
	m->ipv4 = 0;
	m->sc[RP_SC_KEEPIDLE] = 120;
	strcpy(m->ss[RP_SS_BG_LPORTS], "8080");
	m->qos = 1;
}

static void test_p3_edits(void)
{
	struct rp_conf c;
	char *t;
	int i;
	char a[32];

	t = edit(p3_conf, e_p3);
	CHECK(t && strstr(t, "ipv6 on\nipv4 off\nqos off\nqos rule port=22,23"));
	CHECK(t && !strstr(t, "fastopen") && !strstr(t, "bg_programs"));
	CHECK(t && strstr(t, "sysctl net.inet.tcp.cc=cubic\n"));
	CHECK(t && strstr(t, "sysctl net.inet.he.prefer=4\n"));
	CHECK(t && strstr(t, "sysctl net.inet.tcp.mem_high=2048   # KB\n"));       /* untouched */
	CHECK(t && strstr(t, "background program=x\nsysctl net.inet.tcp.connect_timeout=10\n"
	                     "sysctl net.inet.tcp.bg_ports=20,21,6881-6889\n"));
	free(t);

	/* from the defaults: ipv4 off, qos on and the sysctls are appended */
	t = edit(inst_dhcp, e_none_p3);
	CHECK(t && strstr(t, "loglevel 1\nipv4 off\nqos on\nsysctl net.inet.tcp.keepidle=120\n"
	                     "sysctl net.inet.tcp.bg_lports=8080\n"));
	free(t);

	/* lists: delete the first address, add one; add a QoS rule */
	CHECK(rp_conf_load(&c, p3_conf, strlen(p3_conf)) == 0);
	CHECK(rp_list_set(&c, RP_LIST_ALIAS, 0, "") == 0);
	CHECK(rp_list_count(&c, RP_LIST_ALIAS) == 1);
	CHECK(rp_list_add(&c, RP_LIST_ALIAS, "172.16.0.1/16") == 0);
	CHECK(rp_list_add(&c, RP_LIST_QRULE, "proto=udp rport=6667 dscp=AF41") == 0);
	CHECK(rp_list_set(&c, RP_LIST_QRULE, 5, "x") < 0);
	t = text_of(&c);
	CHECK(strstr(t, "route add default 192.168.1.1\nifconfig eth0 alias 10.0.1.5 netmask 255.255.255.0   # lab\n"
	                "ifconfig eth0 alias 172.16.0.1/16\nifconfig eth1 alias") != NULL);
	CHECK(strstr(t, "qos rule program=AmiFTP priority=bulk\nqos rule proto=udp rport=6667 dscp=AF41\n"
	                "qos rule nonsense\n") != NULL);
	free(t);
	rp_conf_free(&c);

	/* untouched entries keep their spelling; a replaced one keeps its comment */
	{
		static const char in[] = "dhcp eth0\nifconfig  eth0 add 10.0.0.5/24\nifconfig eth0 alias 10.0.1.5/24 # b\n";
		CHECK(rp_conf_load(&c, in, strlen(in)) == 0);
		CHECK(rp_list_set(&c, RP_LIST_ALIAS, 1, "10.0.2.5/24") == 0);
		CHECK(rp_list_add(&c, RP_LIST_ALIAS, "10.0.3.5/24") == 0);
		t = text_of(&c);
		CHECK(!strcmp(t, "dhcp eth0\nifconfig  eth0 add 10.0.0.5/24\nifconfig eth0 alias 10.0.2.5/24 # b\n"
		                 "ifconfig eth0 alias 10.0.3.5/24\n"));
		free(t);
		rp_conf_free(&c);
	}

	/* at most 8 addresses */
	CHECK(rp_conf_load(&c, "", 0) == 0);
	for (i = 0; i < RP_NALIAS; i++) {
		sprintf(a, "10.0.%d.1/24", i);
		CHECK(rp_list_add(&c, RP_LIST_ALIAS, a) == 0);
	}
	CHECK(rp_list_add(&c, RP_LIST_ALIAS, "10.1.0.1/24") < 0);
	t = text_of(&c);
	CHECK(!strncmp(t, "ifconfig eth0 alias 10.0.0.1/24\nifconfig eth0 alias 10.0.1.1/24\n", 64));
	free(t);
	rp_conf_free(&c);

	/* renaming the interface rewrites its address lines */
	CHECK(rp_conf_load(&c, p3_conf, strlen(p3_conf)) == 0);
	strcpy(c.cur.ifname, "net0");
	t = text_of(&c);
	CHECK(strstr(t, "ifconfig net0 alias 10.0.0.5/24\n") && strstr(t, "ifconfig eth1 alias"));
	free(t);
	rp_conf_free(&c);
}

static void test_p3_valid(void)
{
	CHECK(rp_valid_alias("10.0.0.5") && rp_valid_alias("10.0.0.5/24") &&
	      rp_valid_alias("10.0.0.5 netmask 255.255.0.0") && rp_valid_alias("10.0.0.5/24 broadcast 10.0.0.255"));
	CHECK(!rp_valid_alias("") && !rp_valid_alias("10.0.0.5 netmask") && !rp_valid_alias("10.0.0.5 up") &&
	      !rp_valid_alias("fe80::1") && !rp_valid_alias("10.0.0.5/40"));
	CHECK(rp_valid_ports("") && rp_valid_ports("21") && rp_valid_ports("20,21,6881-6889"));
	CHECK(!rp_valid_ports("0") && !rp_valid_ports("70000") && !rp_valid_ports("21,") && !rp_valid_ports("9-3") &&
	      !rp_valid_ports("a"));
	CHECK(rp_valid_programs("") && rp_valid_programs("AmiFTP,wget*") && !rp_valid_programs("a b") &&
	      !rp_valid_programs("a,,b"));
	CHECK(rp_valid_qrule("port=22 priority=interactive") && rp_valid_qrule("program=AmiFTP dscp=CS1") &&
	      rp_valid_qrule("proto=tcp lport=80,443 prio=5 dscp=46"));
	CHECK(!rp_valid_qrule("port=22") && !rp_valid_qrule("priority=bulk") && !rp_valid_qrule("port=1-9 dscp=EF") &&
	      !rp_valid_qrule("port=22 dscp=64") && !rp_valid_qrule("port=22 priority=8") &&
	      !rp_valid_qrule("port=1,2,3,4,5,6,7,8,9 dscp=EF") && !rp_valid_qrule("proto=icmp port=1 dscp=EF") &&
	      !rp_valid_qrule("foo=1 port=1 dscp=EF"));
}

static void test_p3_live(void)
{
	struct rp_conf c;

	CHECK(rp_conf_load(&c, p3_conf, strlen(p3_conf)) == 0);
	cmds[0] = 0;
	c.cur.sc[RP_SC_KA_ALL] = 120;
	c.cur.sc[RP_SC_CC] = 1;
	strcpy(c.cur.ss[RP_SS_BG_PROGRAMS], "");
	c.cur.qos = 0;
	CHECK(rp_list_set(&c, RP_LIST_QRULE, 0, "") == 0);
	CHECK(rp_list_set(&c, RP_LIST_ALIAS, 1, "10.0.2.5/24") == 0);
	CHECK(rp_live_commands(&c, collect, NULL) == 0);
	CHECK(!strcmp(cmds, "sysctl net.inet.tcp.keepalive_all=120;sysctl net.inet.tcp.cc=reno;"
	                    "sysctl net.inet.tcp.bg_programs=;qos off;qos rules flush;"
	                    "qos rule program=AmiFTP priority=bulk;ifconfig eth0 10.0.1.5 -alias;"
	                    "ifconfig eth0 alias 10.0.2.5/24;"));
	/* back to the built-in value, or IPv4 off: a restart */
	cmds[0] = 0;
	c.cur.sc[RP_SC_FASTOPEN] = RP_UNSET;
	CHECK(rp_live_commands(&c, collect, NULL) == 1);
	c.cur.sc[RP_SC_FASTOPEN] = 1;
	c.cur.ipv4 = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 1);
	rp_conf_free(&c);
}

int main(void)
{
	test_roundtrips();
	test_parse();
	test_edits();
	test_other();
	test_valid();
	test_live();
	test_p3_parse();
	test_p3_edits();
	test_p3_valid();
	test_p3_live();
	printf("t_prefs_conf: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
