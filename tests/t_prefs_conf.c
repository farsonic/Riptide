/*
 * Riptide - RiptidePrefs' riptide.conf model (src/amiga/prefs/conf_edit.c):
 * byte-exact round trips (comments, unknown lines, CRLF, no final line
 * end), parsing, edits that touch only the lines of the changed settings
 * (keeping trailing comments), DHCP <-> static, the "other lines" list,
 * validation and the commands for the running stack; the Phase 3 settings
 * (ipv4, qos, sysctl table, extra addresses, QoS rules) and VLANs; the
 * editors' settings table (src/amiga/prefs/prefs_fields.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/amiga/prefs/conf_edit.c"
#include "../src/amiga/prefs/prefs_fields.c"

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
	CHECK(!strcmp(c.cur.ifc[0].ifname, "eth0") && !strcmp(c.cur.ifc[0].device, "uaenet.device"));
	CHECK(c.cur.ifc[0].unit == 0 && c.cur.ifc[0].mtu == 1500 && c.cur.ifc[0].tcpwindow == 1440 && c.cur.ifc[0].reads == RP_UNSET);
	CHECK(c.cur.ifc[0].dhcp == 1 && c.l_dhcp[0] && !c.l_ifconfig[0]);
	CHECK(c.cur.ipv6 == 1 && c.cur.ntp == 0 && c.cur.mdns == -1);
	CHECK(rp_other_count(&c) == 2);
	CHECK(!strcmp(rp_other_get(&c, 0), "host amiga 127.0.0.1"));
	CHECK(!strcmp(rp_other_get(&c, 1), "loglevel 1"));
	rp_conf_free(&c);

	CHECK(rp_conf_load(&c, static_conf, strlen(static_conf)) == 0);
	CHECK(!strcmp(c.cur.hostname, "a1200") && !strcmp(c.cur.domain, "lan"));
	CHECK(!strcmp(c.cur.ifc[0].device, "Networks/my card.device") && c.cur.ifc[0].unit == 1 && c.cur.ifc[0].reads == 64);
	CHECK(!strcmp(c.cur.ifc[0].ifextra, "options=\"x y\""));
	CHECK(c.cur.ifc[0].dhcp == 0 && !strcmp(c.cur.ifc[0].addr, "192.168.1.20") && !strcmp(c.cur.ifc[0].netmask, "255.255.255.0"));
	CHECK(!strcmp(c.cur.ifc[0].gateway, "192.168.1.1"));
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
	m->ifc[0].dhcp = 0;
	strcpy(m->ifc[0].addr, "192.168.0.50/24");
	strcpy(m->ifc[0].gateway, "192.168.0.1");
	strcpy(m->dns[0], "192.168.0.1");
}
static void e_dhcp(struct rp_model *m) { m->ifc[0].dhcp = 1; }
static void e_tcpwin_off(struct rp_model *m) { m->ifc[0].tcpwindow = RP_UNSET; m->ifc[0].unit = 2; }
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
	c.cur.ifc[0].tcpwindow = 1440;
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
	CHECK(!strcmp(c.cur.ifc[0].ifextra, "ipv4=on qos=on"));
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

/* the "MTU probing" cycle (Advanced: Connecting): net.inet.tcp.mtu_probing */
static const char *mtup_conf =
	"interface eth0 device=uaenet.device unit=2\n"
	"sysctl net.inet.tcp.mtu_probing=2   # always\n"
	"sysctl net.inet.tcp.keepidle=60\n";

static void e_mtup_off(struct rp_model *m) { m->sc[RP_SC_MTU_PROBING] = 0; }
static void e_mtup_default(struct rp_model *m) { m->sc[RP_SC_MTU_PROBING] = RP_UNSET; }
static void e_mtup_on(struct rp_model *m) { m->sc[RP_SC_MTU_PROBING] = 1; }

static void test_mtup(void)
{
	struct rp_conf c;
	char *t;

	roundtrip("mtu probing", mtup_conf);
	CHECK(rp_conf_load(&c, mtup_conf, strlen(mtup_conf)) == 0);
	CHECK(c.cur.sc[RP_SC_MTU_PROBING] == 2 && rp_other_count(&c) == 0);
	rp_conf_free(&c);
	t = edit(mtup_conf, e_mtup_off);
	CHECK(t && strstr(t, "sysctl net.inet.tcp.mtu_probing=0   # always\nsysctl net.inet.tcp.keepidle=60\n"));
	free(t);
	t = edit(mtup_conf, e_mtup_default);
	CHECK(t && !strstr(t, "mtu_probing") && strstr(t, "keepidle=60"));
	free(t);
	t = edit(inst_dhcp, e_mtup_on);
	CHECK(t && strstr(t, "\nsysctl net.inet.tcp.mtu_probing=1\n"));
	free(t);
}

/* the "ECN" cycle (TCP page): net.inet.tcp.ecn */
static const char *ecn_conf =
	"interface eth0 device=uaenet.device unit=2\n"
	"sysctl net.inet.tcp.ecn=2\n";

static void e_ecn_off(struct rp_model *m) { m->sc[RP_SC_ECN] = 0; }
static void e_ecn_default(struct rp_model *m) { m->sc[RP_SC_ECN] = RP_UNSET; }
static void e_ecn_on(struct rp_model *m) { m->sc[RP_SC_ECN] = 1; }

static void test_ecn(void)
{
	struct rp_conf c;
	char *t;

	roundtrip("ecn", ecn_conf);
	CHECK(rp_conf_load(&c, ecn_conf, strlen(ecn_conf)) == 0);
	CHECK(c.cur.sc[RP_SC_ECN] == 2 && rp_other_count(&c) == 0);
	rp_conf_free(&c);
	t = edit(ecn_conf, e_ecn_off);
	CHECK(t && strstr(t, "sysctl net.inet.tcp.ecn=0\n"));
	free(t);
	t = edit(ecn_conf, e_ecn_default);
	CHECK(t && !strstr(t, "tcp.ecn"));
	free(t);
	t = edit(inst_dhcp, e_ecn_on);
	CHECK(t && strstr(t, "\nsysctl net.inet.tcp.ecn=1\n"));
	free(t);
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
	strcpy(c.cur.ifc[0].ifname, "net0");
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

/* VLANs of the interface: "interface IF.ID vlan=ID parent=IF ..." lines */
static const char *vlan_conf =
	"interface eth0 device=uaenet.device unit=2\n"
	"dhcp eth0\n"
	"interface eth0.10 vlan=10 parent=eth0 address=10.10.0.2/24 pcp=4   # office\n"
	"interface eth0.20  vlan=20 parent=eth0 dhcp\n"
	"interface eth0.30 vlan=30 parent=eth0 mtu=1400 ipv6=off\n"
	"interface eth0.31 vlan=31 parent=eth0 foo=1\n"       /* unknown options: not in the list */
	"interface wlan.5 vlan=5 parent=eth0 up\n"            /* another name: not in the list */
	"hostname a\n";

/* "dhcp IF linklocal off" (RFC 3927 fallback, on by default) */
static void e_ll_off(struct rp_model *m) { m->ifc[0].linklocal = 0; }
static void e_ll_on(struct rp_model *m) { m->ifc[0].linklocal = 1; }
static void e_ll_off_static(struct rp_model *m) { m->ifc[0].linklocal = 0; e_static(m); }

static void test_linklocal(void)
{
	static const char *ll_off = "hostname a\ndhcp eth0 linklocal off\nloglevel 1\n";
	struct rp_conf c;
	char *t;

	roundtrip("linklocal off", ll_off);
	roundtrip("linklocal on", "dhcp eth0 start linklocal on\n");
	CHECK(rp_conf_load(&c, inst_dhcp, strlen(inst_dhcp)) == 0);
	CHECK(c.cur.ifc[0].dhcp == 1 && c.cur.ifc[0].linklocal == 1);
	rp_conf_free(&c);
	CHECK(rp_conf_load(&c, ll_off, strlen(ll_off)) == 0);
	CHECK(c.cur.ifc[0].dhcp == 1 && c.cur.ifc[0].linklocal == 0 && c.l_dhcp[0] && rp_other_count(&c) == 1);
	rp_conf_free(&c);
	/* not ours: kept as another line, the default stands */
	CHECK(rp_conf_load(&c, "dhcp eth0 linklocal maybe\n", 26) == 0);
	CHECK(c.cur.ifc[0].linklocal == 1 && !c.l_dhcp[0] && rp_other_count(&c) == 1);
	rp_conf_free(&c);

	t = edit(inst_dhcp, e_ll_off);          /* the dhcp line rewritten in place */
	CHECK(t && strstr(t, "# 1) DHCP.\ndhcp eth0 linklocal off\n\n# 2) Static.\n"));
	free(t);
	t = edit(ll_off, e_ll_on);
	CHECK(t && !strcmp(t, "hostname a\ndhcp eth0\nloglevel 1\n"));
	free(t);
	t = edit(ll_off, e_ll_off_static);     /* static: no dhcp line at all */
	CHECK(t && !strstr(t, "dhcp") && strstr(t, "ifconfig eth0 192.168.0.50/24 up\n"));
	free(t);
	t = edit(static_conf, e_ll_off);        /* static address: nothing changes */
	CHECK(t && !strcmp(t, static_conf));
	free(t);
}

static void test_vlan(void)
{
	struct rp_conf c;
	char *t;
	int i;

	roundtrip("vlans", vlan_conf);
	CHECK(rp_conf_load(&c, vlan_conf, strlen(vlan_conf)) == 0);
	CHECK(rp_list_count(&c, RP_LIST_VLAN) == 3);
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_VLAN, 0), "10 10.10.0.2/24 pcp=4"));
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_VLAN, 1), "20 dhcp"));
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_VLAN, 2), "30 mtu=1400 ipv6=off"));
	CHECK(rp_other_count(&c) == 2);          /* eth0.31 and wlan.5 */
	/* change one (keeps its comment), delete one, add two */
	CHECK(rp_list_set(&c, RP_LIST_VLAN, 0, "10 10.10.0.3/24 pcp=priority") == 0);
	CHECK(rp_list_set(&c, RP_LIST_VLAN, 1, "") == 0);
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "40 up") == 0);
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "50") == 0);
	t = text_of(&c);
	CHECK(!strcmp(t, "interface eth0 device=uaenet.device unit=2\n"
	                 "dhcp eth0\n"
	                 "interface eth0.10 vlan=10 parent=eth0 address=10.10.0.3/24 pcp=priority   # office\n"
	                 "interface eth0.30 vlan=30 parent=eth0 mtu=1400 ipv6=off\n"
	                 "interface eth0.40 vlan=40 parent=eth0 up\n"
	                 "interface eth0.50 vlan=50 parent=eth0\n"
	                 "interface eth0.31 vlan=31 parent=eth0 foo=1\n"
	                 "interface wlan.5 vlan=5 parent=eth0 up\n"
	                 "hostname a\n") || (fprintf(stderr, "%s", t), 0));
	free(t);
	rp_conf_free(&c);
	/* the live commands: removed and changed ones go, new ones are made */
	CHECK(rp_conf_load(&c, vlan_conf, strlen(vlan_conf)) == 0);
	cmds[0] = 0;
	CHECK(rp_list_set(&c, RP_LIST_VLAN, 0, "10 10.10.0.3/24") == 0);
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "60 dhcp pcp=1") == 0);
	CHECK(rp_live_commands(&c, collect, NULL) == 0);
	CHECK(!strcmp(cmds, "interface eth0.10 delete;"
	                    "interface eth0.10 vlan=10 parent=eth0 address=10.10.0.3/24;"
	                    "interface eth0.60 vlan=60 parent=eth0 dhcp pcp=1;") ||
	      (fprintf(stderr, "%s\n", cmds), 0));
	/* renaming the parent rewrites the VLAN lines (and needs a restart) */
	strcpy(c.cur.ifc[0].ifname, "net0");
	CHECK(rp_live_commands(&c, collect, NULL) == 1);
	t = text_of(&c);
	CHECK(strstr(t, "interface net0.10 vlan=10 parent=net0 address=10.10.0.3/24   # office\n") &&
	      strstr(t, "interface net0.20 vlan=20 parent=net0 dhcp\n") &&
	      strstr(t, "interface net0.60 vlan=60 parent=net0 dhcp pcp=1\n") &&
	      strstr(t, "interface net0.30 vlan=30 parent=net0 mtu=1400 ipv6=off\n") &&
	      strstr(t, "interface eth0.31 vlan=31 parent=eth0 foo=1\n"));
	free(t);
	rp_conf_free(&c);
	/* at most 8 */
	CHECK(rp_conf_load(&c, "", 0) == 0);
	for (i = 1; i <= RP_NVLAN; i++) {
		char e[16];
		sprintf(e, "%d", i);
		CHECK(rp_list_add(&c, RP_LIST_VLAN, e) == 0);
	}
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "100") < 0);
	rp_conf_free(&c);
	/* validation */
	CHECK(rp_valid_vlan("10") && rp_valid_vlan("10 dhcp") && rp_valid_vlan("4094 up pcp=7") &&
	      rp_valid_vlan("10 10.0.0.1/24") && rp_valid_vlan("10 pcp=priority 10.0.0.1/24"));
	CHECK(!rp_valid_vlan("") && !rp_valid_vlan("0") && !rp_valid_vlan("4095") && !rp_valid_vlan("x") &&
	      !rp_valid_vlan("10 dhcp up") && !rp_valid_vlan("10 10.0.0.1") && !rp_valid_vlan("10 pcp=8") &&
	      !rp_valid_vlan("10 pcp=1 pcp=2") && !rp_valid_vlan("10 foo"));
	/* the other per-VLAN settings (src/core/vlan.c) */
	CHECK(rp_valid_vlan("10 dhcp pcp=4 mtu=1400 ipv4=off ipv6=on qos=on tcpwindow=8192") &&
	      rp_valid_vlan("10 tcpwindow=0") && rp_valid_vlan("10 mtu=68") && rp_valid_vlan("10 qos=off up"));
	CHECK(!rp_valid_vlan("10 mtu=67") && !rp_valid_vlan("10 mtu=65536") && !rp_valid_vlan("10 mtu=") &&
	      !rp_valid_vlan("10 tcpwindow=535") && !rp_valid_vlan("10 tcpwindow=16777217") &&
	      !rp_valid_vlan("10 ipv4=maybe") && !rp_valid_vlan("10 mtu=1400 mtu=1300") && !rp_valid_vlan("10 vlan=3"));
	/* a hand-written line with all of them: in the list, written back as it was */
	{
		static const char in[] = "interface eth0.7 vlan=7 parent=eth0 tcpwindow=4096 qos=on ipv4=off dhcp "
		                         "mtu=1000 pcp=priority ipv6=off\n";
		roundtrip("vlan keys", in);
		CHECK(rp_conf_load(&c, in, strlen(in)) == 0);
		CHECK(rp_list_count(&c, RP_LIST_VLAN) == 1 &&
		      !strcmp(rp_list_get(&c, RP_LIST_VLAN, 0),
		              "7 dhcp pcp=priority mtu=1000 ipv4=off ipv6=off qos=on tcpwindow=4096"));
		CHECK(rp_list_add(&c, RP_LIST_VLAN, "8 192.168.8.2/24 mtu=1400 ipv4=on") == 0);
		t = text_of(&c);
		CHECK(!strcmp(t, "interface eth0.7 vlan=7 parent=eth0 tcpwindow=4096 qos=on ipv4=off dhcp mtu=1000 "
		                 "pcp=priority ipv6=off\n"
		                 "interface eth0.8 vlan=8 parent=eth0 address=192.168.8.2/24 mtu=1400 ipv4=on\n") ||
		      (fprintf(stderr, "%s", t), 0));
		free(t);
		rp_conf_free(&c);
	}
}

/* the names the Interface and Parent cycles offer: one card */
static struct rp_lnames nm_eth0 = { 1, { "eth0" } }, nm_net0 = { 1, { "net0" } };

/* the forms of the list pages: an entry into fields and back unchanged */
static int lform_round(int page, const char *entry)
{
	struct rp_lvals v;
	char out[RP_LONG], err[120];
	int fi;

	rp_lform_get(page, entry, &nm_eth0, &v);
	if (rp_lform_put(page, &v, &nm_eth0, out, sizeof(out), &fi, err, sizeof(err)) < 0 || strcmp(out, entry)) {
		fprintf(stderr, "lform %d: '%s' -> '%s' (%s)\n", page, entry, out, fi >= 0 ? err : "");
		return 0;
	}
	return 1;
}

/* a form into an entry: the field refused, -1 for none */
static int lform_try(int page, struct rp_lvals *v, char *out, char *err)
{
	int fi;
	if (rp_lform_put(page, v, &nm_eth0, out, RP_LONG, &fi, err, 120) == 0)
		return -1;
	return fi;
}

static void test_lforms(void)
{
	struct rp_lvals v;
	char out[RP_LONG], err[120], row[160];
	int i, p;

	CHECK(rp_nlfields == RP_NLFIELDS && rp_nlfields <= RP_NLFIELDS_MAX);
	/* the fields of a page in one run; keys unique and not those of the
	 * pages (P), the buttons (S, U, A, C) or the list (Add, Replace, Delete) */
	for (p = 0; p < RP_NPAGES; p++) {
		char keys[RP_NLFIELDS_MAX];
		int nk = 0, k;
		for (i = 0; i < rp_nlfields; i++) {
			char key;
			if (rp_lfields[i].page != p)
				continue;
			CHECK(rp_page_list(p) >= 0 && rp_page_list(p) != RP_LIST_OTHER);
			CHECK(i == 0 || rp_lfields[i - 1].page == p || !(rp_lfields[i].flags & LF_SAMEROW));
			CHECK(rp_lfields[i].help && strlen(rp_lfields[i].help) > 20 && strlen(rp_lfields[i].help) < 160);
			CHECK(rp_lfields[i].type != LF_CYCLE || rp_lfields[i].cy || (rp_lfields[i].flags & LF_NAMES));
			key = rp_lfkey(i);
			if (rp_lfields[i].type == LF_INFO) {
				CHECK(!key);
				continue;
			}
			CHECK(key && !strchr("psuacrde", key));
			for (k = 0; k < nk; k++)
				if (keys[k] == key)
					fprintf(stderr, "FAIL form of page %d: key %c twice\n", p, key), failures++;
			keys[nk++] = key;
		}
	}
	/* entries the forms show and write back unchanged */
	CHECK(lform_round(P_ALIAS, "10.0.1.2/24"));
	CHECK(lform_round(P_ALIAS, "10.0.1.2 netmask 255.255.255.0"));
	CHECK(lform_round(P_ALIAS, "10.0.1.2 netmask 255.255.255.0 broadcast 10.0.1.255"));
	CHECK(lform_round(P_VLAN, "10 dhcp"));
	CHECK(lform_round(P_VLAN, "10"));
	CHECK(lform_round(P_VLAN, "4094 up pcp=0"));
	CHECK(lform_round(P_VLAN, "10 dhcp pcp=4"));
	CHECK(lform_round(P_VLAN, "20 192.168.20.2/24 pcp=priority mtu=1400 ipv4=on ipv6=off qos=on tcpwindow=0"));
	CHECK(lform_round(P_QRULE, "port=22,23 priority=interactive"));
	CHECK(lform_round(P_QRULE, "proto=udp rport=53 dscp=EF"));
	CHECK(lform_round(P_QRULE, "proto=tcp lport=21 program=AmiFTP priority=bulk dscp=cs1"));
	CHECK(lform_round(P_QRULE, "program=wget* priority=3"));
	/* readable rows */
	rp_list_row(P_VLAN, "10 dhcp pcp=4 mtu=1400 ipv6=off", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "eth0.10: DHCP, PCP 4, MTU 1400, IPv6 off") || (fprintf(stderr, "%s\n", row), 0));
	rp_list_row(P_VLAN, "20 192.168.20.2/24 pcp=priority", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "eth0.20: 192.168.20.2/24, PCP from traffic") || (fprintf(stderr, "%s\n", row), 0));
	rp_list_row(P_VLAN, "30", &nm_net0, row, sizeof(row));
	CHECK(!strcmp(row, "net0.30: not started"));
	rp_list_row(P_ALIAS, "10.0.1.2 netmask 255.255.255.0", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "eth0: 10.0.1.2, netmask 255.255.255.0"));
	rp_list_row(P_QRULE, "proto=tcp port=22,23 priority=interactive", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "TCP ports 22,23: interactive") || (fprintf(stderr, "%s\n", row), 0));
	rp_list_row(P_QRULE, "program=AmiFTP dscp=CS1", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "program AmiFTP: DSCP CS1") || (fprintf(stderr, "%s\n", row), 0));
	rp_list_row(P_QRULE, "lport=21 program=AmiFTP priority=5", &nm_eth0, row, sizeof(row));
	CHECK(!strcmp(row, "local ports 21, program AmiFTP: priority 5") || (fprintf(stderr, "%s\n", row), 0));

	/* a new VLAN: DHCP; its name follows the number */
	rp_lform_get(P_VLAN, NULL, &nm_eth0, &v);
	CHECK(v.cy[LV_MODE] == 0 && !strcmp(v.tx[LV_NAME], "eth0.?"));
	CHECK(rp_lform_disabled(LV_ADDR, &v));
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_ID);
	strcpy(v.tx[LV_ID], "10");
	rp_lform_update(P_VLAN, &v, &nm_eth0);
	CHECK(!strcmp(v.tx[LV_NAME], "eth0.10"));
	v.cy[LV_PCP] = 5;                           /* "4 Video" */
	CHECK(lform_try(P_VLAN, &v, out, err) == -1 && !strcmp(out, "10 dhcp pcp=4"));
	v.cy[LV_MODE] = 1;                          /* static: an address with /BITS */
	CHECK(!rp_lform_disabled(LV_ADDR, &v));
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_ADDR);
	strcpy(v.tx[LV_ADDR], "192.168.10.2");
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_ADDR);
	strcpy(v.tx[LV_ADDR], " 192.168.10.2/24 ");
	strcpy(v.tx[LV_MTU], "60");
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_MTU && !strcmp(err, "enter 68..65535, or leave it empty"));
	strcpy(v.tx[LV_MTU], "1400");
	strcpy(v.tx[LV_TCPWIN], "100");
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_TCPWIN);
	v.tx[LV_TCPWIN][0] = 0;
	v.cy[LV_IP6] = 2;
	CHECK(lform_try(P_VLAN, &v, out, err) == -1 && !strcmp(out, "10 192.168.10.2/24 pcp=4 mtu=1400 ipv6=off"));
	CHECK(rp_list_entry_valid(P_VLAN, out, err, sizeof(err)));
	strcpy(v.tx[LV_ID], "4095");
	CHECK(lform_try(P_VLAN, &v, out, err) == LV_ID);

	/* extra addresses */
	rp_lform_get(P_ALIAS, "", &nm_eth0, &v);
	CHECK(lform_try(P_ALIAS, &v, out, err) == LA_ADDR);
	strcpy(v.tx[LA_ADDR], "10.0.1.2");
	strcpy(v.tx[LA_MASK], "255.255.255.0");
	strcpy(v.tx[LA_BCAST], "10.0.1.x");
	CHECK(lform_try(P_ALIAS, &v, out, err) == LA_BCAST);
	v.tx[LA_BCAST][0] = 0;
	CHECK(lform_try(P_ALIAS, &v, out, err) == -1 && !strcmp(out, "10.0.1.2 netmask 255.255.255.0"));
	strcpy(v.tx[LA_MASK], "255.255.255.0/8");
	CHECK(lform_try(P_ALIAS, &v, out, err) == LA_MASK);

	/* QoS rules: ports or a program, and a priority or a mark */
	rp_lform_get(P_QRULE, NULL, &nm_eth0, &v);
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_PORTS);
	strcpy(v.tx[LQ_PORTS], "22,23");
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_PRIO);
	v.cy[LQ_PRIO] = 1;
	CHECK(lform_try(P_QRULE, &v, out, err) == -1 && !strcmp(out, "port=22,23 priority=interactive"));
	strcpy(v.tx[LQ_PORTS], "1,2,3,4,5,6,7,8,9");
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_PORTS);
	strcpy(v.tx[LQ_PORTS], "6881-6889");
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_PORTS);
	v.tx[LQ_PORTS][0] = 0;
	strcpy(v.tx[LQ_PROG], "Ami FTP");
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_PROG);
	strcpy(v.tx[LQ_PROG], "AmiFTP");
	strcpy(v.tx[LQ_DSCP], "XX");
	CHECK(lform_try(P_QRULE, &v, out, err) == LQ_DSCP);
	strcpy(v.tx[LQ_DSCP], "AF41");
	v.cy[LQ_PRIO] = 0;
	v.cy[LQ_PROTO] = 1;
	CHECK(lform_try(P_QRULE, &v, out, err) == -1 && !strcmp(out, "proto=tcp program=AmiFTP dscp=AF41"));
	CHECK(rp_list_entry_valid(P_QRULE, out, err, sizeof(err)));
}

/* resolver: EDNS0 size and TCP fallback (Names page) */
static void e_edns_off(struct rp_model *m)
{
	m->sc[RP_SC_DNS_EDNS] = 0;
	m->sc[RP_SC_DNS_TCP] = RP_UNSET;
}

static void e_dns_new(struct rp_model *m)
{
	m->sc[RP_SC_DNS_EDNS] = 1232;
	m->sc[RP_SC_DNS_TCP] = 0;
}

static void test_dns(void)
{
	static const char in[] = "nameserver 9.9.9.9\nsysctl dns.edns_size=4096   # big\nsysctl dns.tcp_fallback 0\n";
	struct rp_conf c;
	char *t;

	roundtrip("dns", in);
	CHECK(rp_conf_load(&c, in, strlen(in)) == 0);
	CHECK(c.cur.sc[RP_SC_DNS_EDNS] == 4096 && c.cur.sc[RP_SC_DNS_TCP] == 0);
	cmds[0] = 0;
	c.cur.sc[RP_SC_DNS_EDNS] = 512;
	c.cur.sc[RP_SC_DNS_TCP] = 1;
	CHECK(rp_live_commands(&c, collect, NULL) == 0);
	CHECK(!strcmp(cmds, "sysctl dns.edns_size=512;sysctl dns.tcp_fallback=1;"));
	rp_conf_free(&c);
	t = edit(in, e_edns_off);
	CHECK(t && !strcmp(t, "nameserver 9.9.9.9\nsysctl dns.edns_size=0   # big\n"));
	free(t);
	t = edit(inst_dhcp, e_dns_new);
	CHECK(t && strstr(t, "loglevel 1\nsysctl dns.edns_size=1232\nsysctl dns.tcp_fallback=0\n"));
	free(t);
}

/* the settings table both editors build their pages from */
static void test_fields(void)
{
	struct rp_conf c;
	char err[200];
	int i, p, fi;

	CHECK(rp_nfields <= RP_NFIELDS_MAX);
	CHECK(sizeof(rp_pages) / sizeof(rp_pages[0]) == RP_NPAGES);
	for (p = 0; p < RP_NPAGES; p++) {
		char keys[RP_NFIELDS_MAX];
		int nk = 0, rows = 0, k;
		CHECK(rp_pages[p].name && rp_pages[p].title && rp_pages[p].note && rp_pages[p].help && rp_pages[p].help[0]);
		for (i = 0; i < rp_nfields; i++) {
			char key;
			if (rp_fields[i].page != p)
				continue;
			if (!(rp_fields[i].flags & FL_RIGHT))
				rows++;
			key = rp_fkey(i);
			/* every setting has a key; P (pages), S, U, A, C (buttons) are taken */
			CHECK(key && !strchr("psuac", key));
			for (k = 0; k < nk; k++)
				if (keys[k] == key)
					fprintf(stderr, "FAIL page %d: key %c twice\n", p, key), failures++;
			keys[nk++] = key;
		}
		/* GadTools: seven rows; list pages have none */
		CHECK(rows <= 7);
		CHECK(rp_page_list(p) < 0 || rows == 0);
	}
	for (i = 0; i < rp_nfields; i++) {
		CHECK(rp_fields[i].page < RP_NPAGES);
		CHECK(rp_fields[i].help && strlen(rp_fields[i].help) > 20 && strlen(rp_fields[i].help) < 160);
		CHECK(rp_fields[i].type != F_CYCLE || (rp_fields[i].cy && rp_fields[i].cyv));
	}
	CHECK(rp_conf_load(&c, "", 0) == 0);
	/* numbers: empty is "not set", out of range is refused */
	fi = rp_find(&c.cur, &c.cur.ifc[0].mtu);
	CHECK(fi >= 0 && rp_fparse(&c.cur, fi, " 1400 ", err, sizeof(err)) == 0 && c.cur.ifc[0].mtu == 1400);
	CHECK(rp_fparse(&c.cur, fi, "", err, sizeof(err)) == 0 && c.cur.ifc[0].mtu == RP_UNSET);
	CHECK(rp_fparse(&c.cur, fi, "20", err, sizeof(err)) < 0 && !strcmp(err, "enter 68..65535, or leave it empty"));
	CHECK(rp_fparse(&c.cur, fi, "12x", err, sizeof(err)) < 0);
	fi = rp_find(&c.cur, &c.cur.ifc[0].tcpwindow);
	CHECK(rp_fparse(&c.cur, fi, "0", err, sizeof(err)) == 0 && c.cur.ifc[0].tcpwindow == 0);
	/* strings lose surrounding spaces */
	fi = rp_find(&c.cur, c.cur.hostname);
	CHECK(rp_fparse(&c.cur, fi, "  amy  ", err, sizeof(err)) == 0 && !strcmp(c.cur.hostname, "amy"));
	/* cycles and dependent settings */
	fi = rp_find(&c.cur, &c.cur.ifc[0].dhcp);
	CHECK(rp_fcycle(&c.cur, fi) == 0);
	c.cur.ifc[0].dhcp = 0;
	CHECK(rp_fcycle(&c.cur, fi) == 1);
	CHECK(!rp_fdisabled(&c.cur, rp_find(&c.cur, c.cur.ifc[0].addr)));
	CHECK(rp_fdisabled(&c.cur, rp_find(&c.cur, &c.cur.ifc[0].linklocal)));
	/* the whole model: a static address is needed without DHCP */
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && fi == rp_find(&c.cur, c.cur.ifc[0].addr));
	strcpy(c.cur.ifc[0].addr, "192.168.1.20/24");
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) == 0);
	c.cur.sc[RP_SC_PORT_FIRST] = 50000;
	c.cur.sc[RP_SC_PORT_LAST] = 40000;
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && fi == rp_find(&c.cur, &c.cur.sc[RP_SC_PORT_LAST]));
	/* list entries */
	CHECK(rp_list_entry_valid(P_ALIAS, "10.0.1.2/24", err, sizeof(err)));
	CHECK(!rp_list_entry_valid(P_ALIAS, "10.0.1.300", err, sizeof(err)) && !strncmp(err, "Extra address", 13));
	CHECK(rp_list_entry_valid(P_VLAN, "10 dhcp pcp=4", err, sizeof(err)));
	CHECK(rp_list_entry_valid(P_OTHER, "anything at all", err, sizeof(err)));
	rp_conf_free(&c);
}

/* several cards: two interface lines, a VLAN and addresses on the second
 * card and on its VLAN, a default route with dev and metric */
static const char *multi_conf =
	"# two cards\n"
	"hostname amy\n"
	"interface eth0 device=uaenet.device unit=0\n"
	"dhcp eth0\n"
	"ifconfig eth0 alias 10.0.1.2/24\n"
	"interface eth1 device=a2065.device unit=0   # the second card\n"
	"ifconfig eth1 192.168.2.5/24 up\n"
	"route add default 192.168.2.1 dev eth1 metric 10\n"
	"interface eth1.20 vlan=20 parent=eth1 dhcp pcp=3\n"
	"ifconfig eth1.20 alias 10.20.0.2/24\n"
	"nameserver 192.168.2.1\n";

static void test_multi(void)
{
	struct rp_conf c;
	struct rp_lnames nm;
	struct rp_lvals v;
	char *t, out[RP_LONG], err[160], row[120];
	int k, fi;

	roundtrip("multi", multi_conf);
	CHECK(rp_conf_load(&c, multi_conf, strlen(multi_conf)) == 0);
	CHECK(rp_other_count(&c) == 0);
	CHECK(!strcmp(c.cur.ifc[0].ifname, "eth0") && c.cur.ifc[0].dhcp == 1);
	CHECK(!strcmp(c.cur.ifc[1].ifname, "eth1") && !strcmp(c.cur.ifc[1].device, "a2065.device"));
	CHECK(c.cur.ifc[1].dhcp == 0 && !strcmp(c.cur.ifc[1].addr, "192.168.2.5/24"));
	CHECK(!strcmp(c.cur.ifc[1].gateway, "192.168.2.1") && c.cur.ifc[1].metric == 10);
	CHECK(!c.cur.ifc[2].ifname[0] && !c.cur.ifc[3].ifname[0]);
	CHECK(rp_list_count(&c, RP_LIST_VLAN) == 1 && !strcmp(rp_list_get(&c, RP_LIST_VLAN, 0), "eth1|20 dhcp pcp=3"));
	CHECK(rp_list_count(&c, RP_LIST_ALIAS) == 2);
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_ALIAS, 0), "10.0.1.2/24"));
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_ALIAS, 1), "eth1.20|10.20.0.2/24"));
	CHECK(rp_iface_find(&c.cur, "ETH1") == 1 && rp_iface_find(&c.cur, "eth2") < 0);
	CHECK(rp_iface_users(&c.cur, 1) == 2);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) == 0);

	/* the forms: names and the cycles */
	rp_lform_names(&c.cur, P_ALIAS, &nm);
	CHECK(nm.n == 3 && !strcmp(nm.name[0], "eth0") && !strcmp(nm.name[1], "eth1") && !strcmp(nm.name[2], "eth1.20"));
	rp_lform_get(P_ALIAS, "eth1.20|10.20.0.2/24", &nm, &v);
	CHECK(v.cy[LA_IF] == 2 && !strcmp(v.tx[LA_ADDR], "10.20.0.2/24"));
	CHECK(rp_lform_put(P_ALIAS, &v, &nm, out, sizeof(out), &fi, err, sizeof(err)) == 0 &&
	      !strcmp(out, "eth1.20|10.20.0.2/24"));
	v.cy[LA_IF] = 0;
	CHECK(rp_lform_put(P_ALIAS, &v, &nm, out, sizeof(out), &fi, err, sizeof(err)) == 0 && !strcmp(out, "10.20.0.2/24"));
	rp_list_row(P_ALIAS, "eth1.20|10.20.0.2/24", &nm, row, sizeof(row));
	CHECK(!strcmp(row, "eth1.20: 10.20.0.2/24") || (fprintf(stderr, "%s\n", row), 0));
	rp_lform_names(&c.cur, P_VLAN, &nm);
	CHECK(nm.n == 2);
	rp_lform_get(P_VLAN, "eth1|20 dhcp pcp=3", &nm, &v);
	CHECK(v.cy[LV_PARENT] == 1 && !strcmp(v.tx[LV_NAME], "eth1.20") && v.cy[LV_PCP] == 4);
	CHECK(rp_lform_put(P_VLAN, &v, &nm, out, sizeof(out), &fi, err, sizeof(err)) == 0 &&
	      !strcmp(out, "eth1|20 dhcp pcp=3"));
	rp_list_row(P_VLAN, "eth1|20 dhcp pcp=3", &nm, row, sizeof(row));
	CHECK(!strcmp(row, "eth1.20: DHCP, PCP 3") || (fprintf(stderr, "%s\n", row), 0));
	rp_iface_row(&c.cur.ifc[1], row, sizeof(row));
	CHECK(!strcmp(row, "eth1: a2065.device unit 0, 192.168.2.5/24") || (fprintf(stderr, "%s\n", row), 0));

	/* the fields follow the card chosen */
	rp_ifsel = 1;
	fi = rp_find(&c.cur, c.cur.ifc[1].device);
	CHECK(fi >= 0 && !strcmp(rp_fstr(&c.cur, fi), "a2065.device"));
	CHECK(rp_fparse(&c.cur, fi, "x.device", err, sizeof(err)) == 0 && !strcmp(c.cur.ifc[1].device, "x.device"));
	CHECK(!strcmp(c.cur.ifc[0].device, "uaenet.device"));
	strcpy(c.cur.ifc[1].device, "a2065.device");
	rp_ifsel = 3;                               /* an empty slot: the first card */
	CHECK(rp_ifcur(&c.cur) == 0 && !strcmp(rp_fstr(&c.cur, fi), "uaenet.device"));
	rp_ifsel = 0;

	/* a third card, static, with a router */
	k = rp_iface_add(&c.cur);
	CHECK(k == 2 && !strcmp(c.cur.ifc[2].ifname, "eth2") && c.cur.ifc[2].dhcp == 1);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && rp_ifsel == 2);    /* no device yet */
	strcpy(c.cur.ifc[2].device, "a2065.device");
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && !strcmp(err, "another card uses this device and unit already"));
	c.cur.ifc[2].unit = 1;
	c.cur.ifc[2].dhcp = 0;
	strcpy(c.cur.ifc[2].addr, "172.16.0.9/16");
	strcpy(c.cur.ifc[2].gateway, "172.16.0.1");
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) == 0);
	t = text_of(&c);
	CHECK((t && strstr(t, "route add default 192.168.2.1 dev eth1 metric 10\n"
	                     "interface eth2 device=a2065.device unit=1\n"
	                     "ifconfig eth2 172.16.0.9/16 up\n"
	                     "route add default 172.16.0.1 dev eth2 metric 20\n"
	                     "interface eth1.20")) || (fprintf(stderr, "%s", t), 0));
	free(t);
	rp_conf_commit(&c);

	/* renaming the second card: its lines, VLAN and the VLAN's address follow */
	rp_ifsel = 1;
	fi = rp_find(&c.cur, c.cur.ifc[1].ifname);
	CHECK(rp_fparse(&c.cur, fi, "lan1", err, sizeof(err)) == 0);
	CHECK(rp_fparse(&c.cur, fi, "a.b", err, sizeof(err)) < 0);
	rp_ifsel = 0;
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_VLAN, 0), "lan1|20 dhcp pcp=3"));
	CHECK(!strcmp(rp_list_get(&c, RP_LIST_ALIAS, 1), "lan1.20|10.20.0.2/24"));
	cmds[0] = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 1);
	t = text_of(&c);
	CHECK((t && strstr(t, "interface lan1 device=a2065.device unit=0   # the second card\n"
	                     "ifconfig lan1 192.168.2.5/24 up\n"
	                     "route add default 192.168.2.1 dev lan1 metric 10\n") &&
	      strstr(t, "interface lan1.20 vlan=20 parent=lan1 dhcp pcp=3\n"
	                "ifconfig lan1.20 alias 10.20.0.2/24\n")) || (fprintf(stderr, "%s", t), 0));
	free(t);
	rp_conf_commit(&c);

	/* an address on the third card while Riptide runs */
	CHECK(rp_list_add(&c, RP_LIST_ALIAS, "eth2|172.17.0.9/16") == 0);
	cmds[0] = 0;
	CHECK(rp_live_commands(&c, collect, NULL) == 0 && !strcmp(cmds, "ifconfig eth2 alias 172.17.0.9/16;"));
	t = text_of(&c);
	CHECK(t && strstr(t, "ifconfig eth2 alias 172.17.0.9/16\n"));
	free(t);
	rp_conf_commit(&c);

	/* removing the second card takes its VLAN and their addresses with it */
	rp_iface_delete(&c.cur, 1);
	CHECK(!c.cur.ifc[1].ifname[0] && rp_list_count(&c, RP_LIST_VLAN) == 0 && rp_list_count(&c, RP_LIST_ALIAS) == 2);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) == 0);
	t = text_of(&c);
	CHECK((t && !strstr(t, "lan1") && strstr(t, "interface eth2") && strstr(t, "ifconfig eth0 alias 10.0.1.2/24\n")) ||
	      (fprintf(stderr, "%s", t), 0));
	free(t);
	rp_conf_free(&c);

	/* a VLAN or address on a card that is not there: kept as other lines;
	 * a fifth card too */
	{
		static const char *more =
			"interface eth0 device=a.device\n"
			"interface eth1 device=b.device\n"
			"interface eth2 device=c.device\n"
			"interface eth3 device=d.device\n"
			"interface eth4 device=e.device\n"
			"interface eth9.5 vlan=5 parent=eth9 dhcp\n"
			"ifconfig eth9 alias 10.9.0.1/24\n"
			"interface eth1 device=f.device\n";
		roundtrip("more cards", more);
		CHECK(rp_conf_load(&c, more, strlen(more)) == 0);
		CHECK(rp_other_count(&c) == 4 && !strcmp(c.cur.ifc[3].ifname, "eth3"));
		CHECK(rp_iface_add(&c.cur) < 0);
		strcpy(c.cur.ifc[3].ifname, "ETH1");
		CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && rp_ifsel == 3 &&
		      !strcmp(err, "another card has this name already"));
		rp_ifsel = 0;
		rp_conf_free(&c);
	}
	/* entries that name a missing card or VLAN */
	CHECK(rp_conf_load(&c, "", 0) == 0);
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "eth5|7 dhcp") == 0);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && strstr(err, "no card eth5"));
	rp_list_set(&c, RP_LIST_VLAN, 0, "");
	CHECK(rp_list_add(&c, RP_LIST_ALIAS, "eth0.7|10.7.0.1/24") == 0);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) < 0 && strstr(err, "no card or VLAN eth0.7"));
	CHECK(rp_list_add(&c, RP_LIST_VLAN, "7 up") == 0);
	CHECK(rp_validate(&c.cur, &fi, err, sizeof(err)) == 0);
	t = text_of(&c);
	CHECK(t && strstr(t, "ifconfig eth0.7 alias 10.7.0.1/24\n") && strstr(t, "interface eth0.7 vlan=7 parent=eth0 up\n"));
	free(t);
	rp_conf_free(&c);
}

int main(void)
{
	test_fields();
	test_roundtrips();
	test_parse();
	test_edits();
	test_other();
	test_valid();
	test_live();
	test_p3_parse();
	test_p3_edits();
	test_mtup();
	test_ecn();
	test_p3_valid();
	test_p3_live();
	test_vlan();
	test_lforms();
	test_dns();
	test_linklocal();
	test_multi();
	printf("t_prefs_conf: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
