/*
 * Riptide - RiptidePrefs' riptide.conf model (src/amiga/prefs/conf_edit.c):
 * byte-exact round trips (comments, unknown lines, CRLF, no final line
 * end), parsing, edits that touch only the lines of the changed settings
 * (keeping trailing comments), DHCP <-> static, the "other lines" list,
 * validation and the commands for the running stack.
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
	strcpy(m->timezone, "UTC+10");
	m->mdns = 1;
	m->quickack = 1;
	m->ipv6 = 0;
}
static void e_ntp_off(struct rp_model *m) { m->ntp = 0; m->mdns = -1; m->delack_segs = RP_UNSET; }
static void e_hostname_clear(struct rp_model *m) { m->hostname[0] = 0; }

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
	CHECK(t && strstr(t, "loglevel 1\nipv6 off\nntp pool.ntp.org\ntimezone UTC+10\nmdns on\n"
	                     "sysctl net.inet.tcp.quickack=1\n"));
	free(t);

	t = edit(static_conf, e_ntp_off);
	CHECK(t && strstr(t, "\nntp off\n") && !strstr(t, "mdns") && !strstr(t, "delack_segs"));
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

int main(void)
{
	test_roundtrips();
	test_parse();
	test_edits();
	test_other();
	test_valid();
	test_live();
	printf("t_prefs_conf: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
