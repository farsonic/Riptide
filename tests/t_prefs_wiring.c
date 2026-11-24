/*
 * Riptide - every RiptidePrefs setting is tied to something the stack
 * acts on (src/amiga/prefs/prefs_fields.c, conf_edit.c against the core):
 *   1. every setting of rp_fields[] and every list form (rp_lfields[]) is
 *      set to values other than its default - the lowest and the highest
 *      the editor lets through, and others - through the same calls the
 *      editors use (rp_fset, rp_fparse, rp_lform_put, rp_list_add); the
 *      model must pass rp_validate()
 *   2. riptide.conf written from it (rp_conf_text) is read line by line
 *      as the Amiga's Riptide reads it (rt_config_line with an unknown-
 *      keyword handler standing in for stackproc.c's amiga_config_unknown:
 *      lines naming a card turned off are skipped, "interface NAME
 *      device=..." makes a fake Ethernet card); no line may be refused
 *   3. what the lines said took effect: every sysctl of the table reads
 *      back its value, the names, NTP, time zone, mDNS, QoS rules, VLANs
 *      with their options, routes, hosts, addresses
 *   4. Apply: rp_live_commands() from one set of values to the next, and
 *      back to the defaults, sent through rt_ctl_command() as the RIPTIDE
 *      port does; none refused, and the values read back again
 *   5. the card options conf_edit writes on "interface" lines are the ones
 *      stackproc.c parses (a check of its source text, which cannot be
 *      built for the host)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/amiga/prefs/conf_edit.c"
#include "../src/amiga/prefs/prefs_fields.c"
#include "../src/amiga/amiga_defaults.h"
#include "rt/riptide.h"
#include "rt/rt_sysctl.h"
#include "rt/rt_qos.h"
#include "rt/rt_ctl.h"
#include "rt/rt_netsvc.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void *rp_alloc(size_t n) { return malloc(n); }
void rp_free(void *p) { free(p); }

/* ---- stackproc.c's options, from its source ---- */
static char *stackproc_src;

static void load_stackproc(void)
{
	char path[512];
	const char *slash = strrchr(__FILE__, '/');
	FILE *f;
	long n;

	rt_snprintf(path, sizeof(path), "%.*s/../src/amiga/stackproc.c", slash ? (int)(slash - __FILE__) : 1,
	            slash ? __FILE__ : ".");
	f = fopen(path, "rb");
	if (!f)
		f = fopen("src/amiga/stackproc.c", "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	stackproc_src = malloc((size_t)n + 1);
	CHECK(stackproc_src && fread(stackproc_src, 1, (size_t)n, f) == (size_t)n);
	stackproc_src[n] = 0;
	fclose(f);
}

/* amiga_config_unknown() takes "key=" (kv(argv[i], "key")) or the word
 * (rt_strcasecmp(argv[i], "word")) */
static int stackproc_takes(const char *opt)
{
	char pat[64];
	const char *eq = strchr(opt, '=');

	if (!stackproc_src)
		return 0;
	if (eq)
		rt_snprintf(pat, sizeof(pat), "kv(argv[i], \"%.*s\")", (int)(eq - opt), opt);
	else
		rt_snprintf(pat, sizeof(pat), "rt_strcasecmp(argv[i], \"%s\")", opt);
	return strstr(stackproc_src, pat) != NULL;
}

/* ---- the Amiga glue, for the host ---- */
#define NFAKE 6
static struct rt_netif fake[NFAKE];
static int fake_used[NFAKE];
static char off_cards[8][RT_IFNAMSIZ];
static int noff;

static int fake_output(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 et)
{
	(void)nif;
	(void)dsthw;
	(void)et;
	rt_buf_free(b);
	return 0;
}

static int fake_vlan_rx(struct rt_netif *nif, int on)
{
	(void)nif;
	(void)on;
	return 0;
}

static int fake_index(const char *name)
{
	int i;
	for (i = 0; i < NFAKE; i++)
		if (fake_used[i] && !strcmp(fake[i].name, name))
			return i;
	return -1;
}

/* stackproc.c's amiga_config_unknown(), as far as the editor's lines go */
static int amiga_unknown(int argc, char **argv, char *err, u32 errlen)
{
	int i, k;

	if (!strcmp(argv[0], "interface")) {
		long mtu = 1500;
		int drv = 0;
		if (argc == 3 && !strcmp(argv[2], "delete")) {
			k = fake_index(argv[1]);
			if (k < 0) {
				rt_snprintf(err, errlen, "no interface '%s'", argv[1]);
				return -RT_ENXIO;
			}
			rt_netif_remove(&fake[k]);
			fake_used[k] = 0;
			return 0;
		}
		if (argc == 3 && !strncmp(argv[2], "reads=", 6) && fake_index(argv[1]) >= 0)
			return 0;
		for (i = 2; i < argc; i++) {
			if (!stackproc_takes(argv[i])) {
				rt_snprintf(err, errlen, "interface: stackproc.c has no option '%s'", argv[i]);
				return -RT_EINVAL;
			}
			if (!strcmp(argv[i], "disabled")) {
				rt_strlcpy(off_cards[noff++], argv[1], RT_IFNAMSIZ);
				return 0;
			}
			drv += !strncmp(argv[i], "device=", 7) || !strncmp(argv[i], "netdev=", 7) ||
			       !strncmp(argv[i], "mni=", 4);
			if (!strncmp(argv[i], "mtu=", 4))
				mtu = strtol(argv[i] + 4, NULL, 10);
		}
		if (drv != 1 || fake_index(argv[1]) >= 0) {
			rt_snprintf(err, errlen, "interface: bad line");
			return -RT_EINVAL;
		}
		for (k = 0; k < NFAKE && fake_used[k]; k++)
			;
		CHECK(k < NFAKE);
		memset(&fake[k], 0, sizeof(fake[k]));
		rt_strlcpy(fake[k].name, argv[1], sizeof(fake[k].name));
		fake[k].type = RT_IFT_ETHER;
		fake[k].hwlen = 6;
		fake[k].hwaddr[0] = 2;
		fake[k].hwaddr[5] = (u8)(k + 1);
		fake[k].flags = RT_IFF_BROADCAST | RT_IFF_MULTICAST | RT_IFF_SIMPLEX;
		fake[k].mtu = (u16)(mtu > 1500 ? 1500 : mtu);
		fake[k].link_output = fake_output;
		fake[k].vlan_rx = fake_vlan_rx;
		if (rt_netif_add(&fake[k]) < 0)
			return -RT_ENOMEM;
		fake_used[k] = 1;
		return 0;
	}
	rt_snprintf(err, errlen, "unknown keyword '%s'", argv[0]);
	return -RT_EINVAL;
}

/* stackproc.c's names_disabled(): a line naming a card turned off */
static int names_off(const char *line)
{
	const char *s = line;
	int i;

	while (*s && *s != ' ')
		s++;
	while (*s) {
		char w[64];
		int n = 0;
		while (*s == ' ')
			s++;
		while (*s && *s != ' ' && n < (int)sizeof(w) - 1)
			w[n++] = *s++;
		w[n] = 0;
		for (i = 0; i < noff; i++) {
			const char *x = !strncmp(w, "parent=", 7) ? w + 7 : w;
			size_t l = strlen(off_cards[i]);
			if (!strncmp(x, off_cards[i], l) && (x[l] == 0 || x[l] == '.'))
				return 1;
		}
	}
	return 0;
}

/* riptide.conf as stackproc.c reads it at start: 0 or the lines refused */
static int read_conf(const char *text)
{
	char line[600], err[200];
	const char *p = text;
	int bad = 0;

	while (*p) {
		size_t n = strcspn(p, "\r\n");
		int r;
		if (n >= sizeof(line))
			n = sizeof(line) - 1;
		memcpy(line, p, n);
		line[n] = 0;
		p += n;
		while (*p == '\r' || *p == '\n')
			p++;
		if (!line[0] || line[0] == '#' || names_off(line))
			continue;
		rt_lock();
		r = rt_config_line(line, err, sizeof(err), amiga_unknown);
		rt_unlock();
		if (r < 0) {
			fprintf(stderr, "FAIL riptide.conf line refused: '%s': %s\n", line, err);
			bad++;
		}
	}
	return bad;
}

/* Apply: each line through the RIPTIDE port's RTCMD_COMMAND */
static int live_bad;
static void live_one(const char *line, void *arg)
{
	char out[400];
	int r;

	(void)arg;
	rt_lock();
	r = rt_ctl_command(line, out, sizeof(out), amiga_unknown);
	rt_unlock();
	if (getenv("T_VERBOSE"))
		fprintf(stderr, "apply: %s -> %d\n", line, r);
	if (r < 0) {
		fprintf(stderr, "FAIL Apply refused: '%s': %s", line, out);
		live_bad++;
	}
}

static int apply(struct rp_conf *c)
{
	int restart;

	live_bad = 0;
	restart = rp_live_commands(c, live_one, NULL);
	rp_conf_applied(c);
	return live_bad ? -1 : restart;
}

/* ---- values ---- */
/* the text a string setting gets in set s (0, 1, 2); NULL to leave it */
static const char *str_value(const struct rp_field *f, int card, int s)
{
	size_t o = f->off;

	if (f->flags & FL_IF) {
		static char name[16];
		if (o == offsetof(struct rp_iface, ifname)) {
			rt_snprintf(name, sizeof(name), "net%d", card);
			return name;
		}
		if (o == offsetof(struct rp_iface, device))
			return card == 0 ? (s == 1 ? "a2065.device" : "uaenet.device") : card == 1 ? "genet.device" :
			       "ariadne.device";
		if (o == offsetof(struct rp_iface, addr))
			return card == 1 ? (s == 1 ? "10.7.1.20" : "10.7.1.20/24") : "";
		if (o == offsetof(struct rp_iface, netmask))
			return card == 1 && s == 1 ? "255.255.255.0" : "";
		if (o == offsetof(struct rp_iface, gateway))
			return card == 1 ? (s == 1 ? "10.7.1.254" : "10.7.1.1") : "";
		return NULL;
	}
	if (o == offsetof(struct rp_model, hostname))
		return s == 1 ? "amiga4000" : "amy";
	if (o == offsetof(struct rp_model, domain))
		return s == 1 ? "example.org" : "home.arpa";
	if (o == offsetof(struct rp_model, dns))
		return s == 1 ? "10.7.1.53" : "10.7.1.1";
	if (o == offsetof(struct rp_model, dns) + RP_ADDR)
		return s == 1 ? "" : "2001:db8::53";
	if (o == offsetof(struct rp_model, dns) + 2 * RP_ADDR)
		return s == 1 ? "9.9.9.9" : "1.1.1.1";
	if (o == offsetof(struct rp_model, ntp_servers))
		return s == 1 ? "time.example.org" : "pool.ntp.org 10.7.1.1";
	if (o == offsetof(struct rp_model, timezone))
		return s == 1 ? "-05:30" : s == 2 ? "auto" : "+10:00";
	if (o == offsetof(struct rp_model, ss) + RP_SS_BG_PROGRAMS * RP_SSLEN)
		return s == 1 ? "AmiFTP" : "wget,curl*";
	if (o == offsetof(struct rp_model, ss) + RP_SS_BG_PORTS * RP_SSLEN)
		return s == 1 ? "6881-6889" : "20,21,119";
	if (o == offsetof(struct rp_model, ss) + RP_SS_BG_LPORTS * RP_SSLEN)
		return s == 1 ? "8080" : "21,22";
	CHECK(!"a string setting without a test value");
	fprintf(stderr, "  %s\n", f->label);
	return NULL;
}

/* the number a number setting gets in set s: the lowest, the highest,
 * then 0 where 0 is allowed (else the middle) */
static long num_value(int fi, int s)
{
	const struct rp_field *f = &rp_fields[fi];

	if (f->off == offsetof(struct rp_model, sc) + RP_SC_PORT_LAST * sizeof(long))
		return s == 0 ? 1024 : s == 1 ? 65535 : 60000;     /* not below the first */
	if (f->off == offsetof(struct rp_model, sc) + RP_SC_PORT_FIRST * sizeof(long))
		return s == 0 ? 1024 : s == 1 ? 65535 : 50000;
	if (f->off == offsetof(struct rp_model, sc) + RP_SC_SYN_RTO_MAX * sizeof(long))
		return s == 0 ? 3000 : s == 1 ? f->max : 4000;      /* not below the first RTO */
	if ((f->flags & FL_IF) && f->off == offsetof(struct rp_iface, unit))
		return s == 1 ? f->max : f->min;
	/* IPv6 needs 1280 (68 is taken, and IPv6 then stays off) */
	if ((f->flags & FL_IF) && f->off == offsetof(struct rp_iface, mtu))
		return s == 1 ? f->max : 1280;
	if (s == 0)
		return f->min;
	if (s == 1)
		return f->max;
	return (f->flags & FL_ZERO) ? 0 : f->min + (f->max - f->min) / 2;
}

/* every setting of rp_fields[] to set s; cards net0 (DHCP), net1
 * (static) and net2 (turned off) */
static void set_fields(struct rp_model *m, int s)
{
	char err[160];
	int fi, card;

	while (rp_iface_count(m) < 3)
		CHECK(rp_iface_add(m) >= 0);
	for (card = 0; card < 3; card++) {
		rp_ifsel = card;
		for (fi = 0; fi < rp_nfields; fi++) {
			const struct rp_field *f = &rp_fields[fi];
			if (!(f->flags & FL_IF) && card)
				continue;
			if (f->type == F_STR) {
				const char *v = str_value(f, card, s);
				if (v && rp_fparse(m, fi, v, err, sizeof(err)) < 0) {
					fprintf(stderr, "FAIL %s = '%s': %s\n", f->label, v, err);
					failures++;
				}
			} else if (f->type == F_NUM) {
				char t[16];
				rt_snprintf(t, sizeof(t), "%ld", num_value(fi, s));
				if (rp_fparse(m, fi, t, err, sizeof(err)) < 0) {
					fprintf(stderr, "FAIL %s = %s: %s\n", f->label, t, err);
					failures++;
				}
			} else if (f->type == F_CHECK) {
				/* the third card is turned off; IPv4 and IPv6 are not
				 * both off */
				long v = (f->flags & FL_IF) && f->off == offsetof(struct rp_iface, disabled) ? card == 2 :
				         f->off == offsetof(struct rp_model, ipv4) ? 1 :
				         f->off == offsetof(struct rp_model, ipv6) ? s != 1 : s != 1;
				rp_fset(m, fi, v);
			} else {
				int n;
				for (n = 0; f->cy[n]; n++)
					;
				/* the last value, the first that is not Default, one between */
				rp_fset(m, fi, f->cyv[s == 0 ? n - 1 : s == 1 ? (f->cyv[0] == RP_UNSET || f->cyv[0] == -1) :
				                     n / 2]);
				/* net0 DHCP, net1 static */
				if ((f->flags & FL_IF) && f->off == offsetof(struct rp_iface, dhcp))
					rp_fset(m, fi, card != 1);
			}
		}
	}
	rp_ifsel = 0;
}

/* a list form: cycles at their last label (s 0) or first (s 1); text
 * from the table */
static const char *lf_text(int k, int s)
{
	switch (k) {
	case LA_ADDR:   return s ? "10.8.2.2" : "10.8.1.2/24";
	case LA_MASK:   return s ? "255.255.0.0" : "";
	case LA_BCAST:  return s ? "10.8.255.255" : "";
	case LV_ID:     return s ? "4094" : "1";
	case LV_ADDR:   return "192.168.10.2/24";
	case LV_MTU:    return s ? "68" : "1280";
	case LV_TCPWIN: return s ? "0" : "16777216";
	case LQ_PORTS:  return s ? "22,23,513" : "119";
	case LQ_PROG:   return s ? "AmiFTP" : "wget*";
	case LQ_DSCP:   return s ? "AF41" : "63";
	case LR_DEST:   return s ? "2001:db8:5::/48" : "10.9.0.0/16";
	case LR_GW:     return s ? "" : "10.7.1.254";
	case LR_METRIC: return s ? "2147483647" : "0";
	case LH_NAME:   return s ? "printer" : "nas.home.lan";
	case LH_ADDR:   return s ? "2001:db8::9" : "10.7.1.9";
	case LN_ADDR:   return s ? "10.7.1.10" : "10.7.1.11";
	case LN_MAC:    return s ? "00:11:22:33:44:55" : "02:aa:bb:cc:dd:ee";
	case L6_ADDR:   return s ? "2001:db8:7::5/64" : "fd00:7::5/48";
	}
	return "";
}

static void add_entry(struct rp_conf *c, int page, int s, int which_if)
{
	struct rp_lnames nm;
	struct rp_lvals v;
	char entry[RP_LONG], err[160];
	int k, fi;

	rp_lform_names(&c->cur, page, &nm);
	rp_lform_get(page, NULL, &nm, &v);
	for (k = 0; k < rp_nlfields; k++) {
		const struct rp_lfield *f = &rp_lfields[k];
		int n = 0;
		if (f->page != page)
			continue;
		if (f->type == LF_CYCLE) {
			if (f->flags & LF_NAMES)
				n = nm.n;
			else
				while (f->cy[n])
					n++;
			v.cy[k] = s ? (n > 1) : n - 1;
			if (f->flags & LF_NAMES)
				v.cy[k] = which_if < nm.n ? which_if : 0;
		} else if (f->type != LF_INFO) {
			rt_strlcpy(v.tx[k], lf_text(k, s), RP_LFTEXT);
		}
	}
	if (page == P_VLAN)
		v.cy[LV_MODE] = s ? 1 : 2;              /* static address; up (IPv4 is off) */
	if (page == P_ROUTE && s)
		v.cy[LR_TYPE] = 4;                      /* an IPv6 blackhole */
	rp_lform_update(page, &v, &nm);
	if (rp_lform_put(page, &v, &nm, entry, sizeof(entry), &fi, err, sizeof(err)) < 0) {
		fprintf(stderr, "FAIL form of page %s: %s\n", rp_pages[page].title, err);
		failures++;
		return;
	}
	CHECK(rp_list_entry_valid(page, entry, err, sizeof(err)));
	CHECK(rp_list_add(c, rp_page_list(page), entry) >= 0);
}

/* a VLAN with IPv4 off and DHCP or an address: the stack refuses it, so
 * the form and the typed entry do too */
static void test_vlan_ipv4_off(void)
{
	struct rp_lnames nm;
	struct rp_lvals v;
	struct rp_conf c;
	char entry[RP_LONG], err[160];
	int fi;

	CHECK(rp_conf_load(&c, "", 0) == 0);
	rp_lform_names(&c.cur, P_VLAN, &nm);
	rp_lform_get(P_VLAN, NULL, &nm, &v);
	strcpy(v.tx[LV_ID], "10");
	v.cy[LV_IP4] = 2;
	v.cy[LV_MODE] = 0;
	CHECK(rp_lform_put(P_VLAN, &v, &nm, entry, sizeof(entry), &fi, err, sizeof(err)) < 0 && fi == LV_IP4);
	v.cy[LV_MODE] = 1;
	strcpy(v.tx[LV_ADDR], "10.1.0.2/24");
	CHECK(rp_lform_put(P_VLAN, &v, &nm, entry, sizeof(entry), &fi, err, sizeof(err)) < 0 && fi == LV_IP4);
	v.cy[LV_MODE] = 2;
	CHECK(rp_lform_put(P_VLAN, &v, &nm, entry, sizeof(entry), &fi, err, sizeof(err)) == 0 &&
	      !strcmp(entry, "10 up ipv4=off"));
	CHECK(!rp_valid_vlan("10 dhcp ipv4=off") && !rp_valid_vlan("10 10.1.0.2/24 ipv4=off"));
	CHECK(rp_valid_vlan("10 up ipv4=off") && rp_valid_vlan("10 ipv4=off") && rp_valid_vlan("10 dhcp ipv4=on"));
	/* a VLAN above its card's MTU: refused by the stack, so by the check */
	strcpy(c.cur.vlan[0], "10 up mtu=1500");
	c.cur.ifc[0].mtu = 1400;
	CHECK(rp_validate(&c.cur, &fi, entry, sizeof(entry)) < 0 && rp_vpage == P_VLAN);
	c.cur.ifc[0].mtu = 1500;
	CHECK(rp_validate(&c.cur, &fi, entry, sizeof(entry)) == 0);
	c.cur.ifc[0].mtu = RP_UNSET;            /* the driver's: not known here */
	CHECK(rp_validate(&c.cur, &fi, entry, sizeof(entry)) == 0);
	rt_lock();
	CHECK(rt_config_line("interface net0.79 vlan=79 parent=net0 mtu=1500", err, sizeof(err), NULL) ==
	      (rt_netif_find("net0")->mtu < 1500 ? -RT_EINVAL : 0));
	CHECK(rt_config_line("interface net0.77 vlan=77 parent=net0 dhcp ipv4=off", err, sizeof(err), NULL) < 0);
	CHECK(rt_config_line("interface net0.78 vlan=78 parent=net0 address=10.1.0.2/24 ipv4=off", err, sizeof(err),
	                     NULL) < 0);
	CHECK(!rt_netif_find("net0.77") && !rt_netif_find("net0.78"));
	rt_unlock();
	rp_conf_free(&c);
}

static void clear_lists(struct rp_conf *c)
{
	int w;
	for (w = RP_LIST_ALIAS; w <= RP_LIST_ADDR6; w++)
		while (rp_list_count(c, w))
			rp_list_set(c, w, 0, "");
}

/* every list form, twice: on the first card and on the second (or Any) */
static void set_lists(struct rp_conf *c, int s)
{
	static const int pages[] = { P_ALIAS, P_VLAN, P_QRULE, P_ROUTE, P_HOST, P_NEIGH, P_ADDR6 };
	int i;

	clear_lists(c);
	for (i = 0; i < (int)(sizeof(pages) / sizeof(pages[0])); i++)
		add_entry(c, pages[i], s, s ? 1 : 0);
}

static int rp_list_count_m(const struct rp_model *m, int which);

/* ---- what the stack has ---- */
static void sysctl_text(const char *name, char *buf, u32 len)
{
	const struct rt_sysctl *e = rt_sysctl_find(name);
	buf[0] = 0;
	CHECK(e != NULL);
	if (e)
		rt_sysctl_get_text(e, buf, len);
}

static void check_sysctl(const char *name, long want)
{
	char v[96];

	if (want == RP_UNSET)
		return;
	sysctl_text(name, v, sizeof(v));
	if (strtol(v, NULL, 10) != want) {
		fprintf(stderr, "FAIL %s: the editor wrote %ld, the stack has %s\n", name, want, v);
		failures++;
	}
	checks++;
}

static int count_routes(void)
{
	struct rt_route *r;
	int n = 0;

	for (r = rt_route_list(); r; r = r->next)
		if (!(r->flags & (RT_RTF_CONNECTED | RT_RTF_PMTU)))
			n++;
	return n;
}

static void check_stack(const struct rp_model *m)
{
	char v[RP_SSLEN + 32], b[200];
	int i;

	rt_lock();
	for (i = 0; i < RP_NSC; i++) {
		if (i == RP_SC_CC) {
			if (m->sc[i] != RP_UNSET) {
				sysctl_text(rp_sc_names[i], v, sizeof(v));
				CHECK(!strcmp(v, rp_cc_names[m->sc[i]]));
			}
			continue;
		}
		/* KB in the editor and in the stack */
		check_sysctl(rp_sc_names[i], m->sc[i]);
	}
	for (i = 0; i < RP_NSS; i++) {
		sysctl_text(rp_ss_names[i], v, sizeof(v));
		if (strcmp(v, m->ss[i])) {
			fprintf(stderr, "FAIL %s: the editor wrote '%s', the stack has '%s'\n", rp_ss_names[i], m->ss[i], v);
			failures++;
		}
		checks++;
	}
	check_sysctl("net.inet.tcp.quickack", m->quickack);
	check_sysctl("net.inet.tcp.delack_segs", m->delack_segs);
	check_sysctl("net.inet.tcp.delack_ms", m->delack_ms);
	if (m->hostname[0])
		CHECK(!strcmp(rt_hostname, m->hostname));
	if (m->domain[0])
		CHECK(!strcmp(rt_domainname, m->domain));
	CHECK(rt_ip4_auto_enable == m->ipv4);
	rt_ntp_status(b, sizeof(b));
	CHECK(m->ntp ? b[0] != 0 : 1);
	if (m->timezone[0] && strcmp(m->timezone, "auto"))
		CHECK(rt_tz_configured);
	rt_mdns_status(b, sizeof(b));
	if (m->mdns >= 0)
		CHECK(!strcmp(b, "mDNS: off") == !m->mdns);
	CHECK(rt_qos_nrules() == rp_list_count_m(m, RP_LIST_QRULE));
	/* the VLAN entries are interfaces with their options */
	for (i = 0; i < RP_NVLAN && m->vlan[i][0]; i++) {
		char ifn[RP_IFNAMELEN], name[RP_IFNAMELEN + 8];
		struct rt_netif *nif;
		const char *body = rp_entry_body(m->vlan[i]), *x;
		rp_entry_if(m, m->vlan[i], ifn, sizeof(ifn));
		rt_snprintf(name, sizeof(name), "%s.%lu", ifn, rt_strtoul(body, NULL, 10));
		nif = rt_netif_find(name);
		CHECK(nif != NULL);
		if (!nif) {
			fprintf(stderr, "  no VLAN %s\n", name);
			continue;
		}
		if ((x = strstr(body, "mtu=")) != NULL)
			CHECK(nif->mtu == rt_strtoul(x + 4, NULL, 10));
		if ((x = strstr(body, "tcpwindow=")) != NULL)
			CHECK(nif->tcp_window == rt_strtoul(x + 10, NULL, 10));
	}
	CHECK(count_routes() >= rp_list_count_m(m, RP_LIST_ROUTE));
	rt_unlock();
}

/* rp_list_count of a model */
static int rp_list_count_m(const struct rp_model *m, int which)
{
	struct rp_conf c;
	memset(&c, 0, sizeof(c));
	c.cur = *m;
	return rp_list_count(&c, which);
}

/* ---- the test ---- */
static void test_interface_options(void)
{
	static const char *const opts[] = { "device=", "netdev=", "mni=", "unit=", "mtu=", "reads=", "tcpwindow=",
	                                    "ipv4=", "ipv6=", "qos=", "options=", "raw", "disabled", NULL };
	int i;

	for (i = 0; opts[i]; i++) {
		if (!stackproc_takes(opts[i]))
			fprintf(stderr, "FAIL stackproc.c does not take '%s' on an interface line\n", opts[i]);
		CHECK(stackproc_takes(opts[i]));
	}
	CHECK(!stackproc_takes("card="));       /* the Mac's alone */
}

static char *text_of(struct rp_conf *c);

/* every value of every cycle that is a sysctl, as conf_edit writes it,
 * taken and read back; numbers: the editor's lowest and highest are inside
 * the stack's range */
static void test_cycles(void)
{
	struct rp_conf c;
	char err[200], *t;
	int fi, a;

	for (fi = 0; fi < rp_nfields; fi++) {
		const struct rp_field *f = &rp_fields[fi];
		size_t so = offsetof(struct rp_model, sc);
		if (f->flags & FL_IF || f->off < so || f->off >= so + RP_NSC * sizeof(long))
			continue;
		for (a = 0; f->type == F_CYCLE && f->cy[a]; a++) {
			const char *line;
			if (f->cyv[a] == RP_UNSET)
				continue;
			CHECK(rp_conf_load(&c, "", 0) == 0);
			rp_fset(&c.cur, fi, f->cyv[a]);
			t = text_of(&c);
			line = strstr(t, "sysctl ");
			CHECK(line != NULL);
			if (line) {
				char l[200];
				int i = (int)((f->off - so) / sizeof(long)), r;
				rt_strlcpy(l, line, sizeof(l));
				l[strcspn(l, "\n")] = 0;
				rt_lock();
				r = rt_config_line(l, err, sizeof(err), NULL);
				rt_unlock();
				if (r < 0)
					fprintf(stderr, "FAIL %s = %s: '%s': %s\n", f->label, f->cy[a], l, err);
				CHECK(r == 0);
				if (i == RP_SC_CC) {
					char v[32];
					sysctl_text(rp_sc_names[i], v, sizeof(v));
					CHECK(!strcmp(v, rp_cc_names[f->cyv[a]]));
				} else {
					check_sysctl(rp_sc_names[i], f->cyv[a]);
				}
			}
			free(t);
			rp_conf_free(&c);
		}
		if (f->type == F_NUM) {
			const struct rt_sysctl *e = rt_sysctl_find(rp_sc_names[(f->off - so) / sizeof(long)]);
			CHECK(e != NULL);
			if (e && ((f->min < e->min && !(f->flags & FL_ZERO)) || f->max > e->max ||
			          ((f->flags & FL_ZERO) && e->min > 0))) {
				fprintf(stderr, "FAIL %s: the editor takes %ld..%ld, the stack %ld..%ld\n", f->label, f->min,
				        f->max, (long)e->min, (long)e->max);
				failures++;
			}
			checks++;
		}
	}
}

static char *text_of(struct rp_conf *c)
{
	size_t len;
	char *t = rp_conf_text(c, &len);
	CHECK(t != NULL);
	return t;
}

static void test_wiring(void)
{
	static const char *const amiga[] = RT_AMIGA_DEFAULTS;
	struct rp_conf c, d;
	char err[200], *t;
	int fi, i, s;

	CHECK(rt_init() == 0);
	rt_log_level = 0;
	for (i = 0; amiga[i]; i++)
		CHECK(rt_config_line(amiga[i], err, sizeof(err), NULL) == 0);

	/* 1-3: set 0 written and read as at start */
	CHECK(rp_conf_load(&c, "", 0) == 0);
	set_fields(&c.cur, 0);
	set_lists(&c, 0);
	if (rp_validate(&c.cur, &fi, err, sizeof(err)) < 0) {
		fprintf(stderr, "FAIL validate: %s: %s\n", rp_fields[fi].label, err);
		failures++;
	}
	t = text_of(&c);
	if (getenv("T_VERBOSE"))
		fprintf(stderr, "--- riptide.conf\n%s---\n", t);
	CHECK(read_conf(t) == 0);
	check_stack(&c.cur);
	/* the card turned off was not opened, the others were */
	CHECK(fake_index("net0") >= 0 && fake_index("net1") >= 0 && fake_index("net2") < 0);

	/* 4: Apply of sets 1 and 2 (the cards stay as they are) */
	memset(&d, 0, sizeof(d));
	CHECK(rp_conf_load(&d, t, strlen(t)) == 0);
	free(t);
	for (s = 1; s <= 2; s++) {
		struct rp_iface keep[RP_NIF];
		memcpy(keep, d.cur.ifc, sizeof(keep));
		set_fields(&d.cur, s);
		memcpy(d.cur.ifc, keep, sizeof(keep));
		/* the Address page's IPv6 cycle is applied live: try it */
		d.cur.ifc[0].ipv6 = s == 1 ? 0 : 1;
		d.cur.ipv4 = c.cur.ipv4;
		d.cur.ipv6 = c.cur.ipv6;
		if (s == 1)
			set_lists(&d, 1);
		if (rp_validate(&d.cur, &fi, err, sizeof(err)) < 0) {
			fprintf(stderr, "FAIL validate set %d: %s: %s\n", s, fi >= 0 ? rp_fields[fi].label : "?", err);
			failures++;
		}
		CHECK(apply(&d) >= 0);
		check_stack(&d.cur);
	}
	/* back to the defaults: the entries go, the sysctls wait for a restart */
	{
		struct rp_model def;
		def = d.cur;
		for (i = 0; i < RP_NSC; i++)
			def.sc[i] = RP_UNSET;
		for (i = 0; i < RP_NSS; i++)
			def.ss[i][0] = 0;
		d.cur = def;
		clear_lists(&d);
		CHECK(apply(&d) == 1);
		rt_lock();
		CHECK(rt_qos_nrules() == 0);
		/* net1's own default route stays */
	CHECK(count_routes() == 1 || (fprintf(stderr, "%d routes left\n", count_routes()), 0));
		CHECK(!rt_netif_find("net0.1") && !rt_netif_find("net1.4094"));
		rt_unlock();
	}
	/* a new card with an address, a VLAN and an IPv6 address on it: they
	 * wait for the restart that opens the card (nothing is refused) */
	{
		int k = rp_iface_add(&d.cur);
		char e[RP_LONG];
		CHECK(k >= 0);
		strcpy(d.cur.ifc[k].device, "x-surf.device");
		rt_snprintf(e, sizeof(e), "%s|10.11.0.2/24", d.cur.ifc[k].ifname);
		CHECK(rp_list_add(&d, RP_LIST_ALIAS, e) >= 0);
		rt_snprintf(e, sizeof(e), "%s|12 dhcp", d.cur.ifc[k].ifname);
		CHECK(rp_list_add(&d, RP_LIST_VLAN, e) >= 0);
		rt_snprintf(e, sizeof(e), "%s|fd00:11::2/64", d.cur.ifc[k].ifname);
		CHECK(rp_list_add(&d, RP_LIST_ADDR6, e) >= 0);
		CHECK(rp_validate(&d.cur, &fi, err, sizeof(err)) == 0);
		CHECK(apply(&d) == 1);
		/* the card's MTU: only a restart */
		d.cur.ifc[1].mtu = 1400;
		CHECK(apply(&d) == 1);
	}
	/* a VLAN and an address on it in one Apply; the VLAN changed (made
	 * again) keeps the address: deleting it afterwards finds it */
	CHECK(rp_list_add(&d, RP_LIST_VLAN, "5 up") >= 0);
	CHECK(rp_list_add(&d, RP_LIST_ALIAS, "net0.5|10.12.0.2/24") >= 0);
	CHECK(apply(&d) == 0);
	rp_list_set(&d, RP_LIST_VLAN, rp_list_count(&d, RP_LIST_VLAN) - 1, "5 up pcp=3");
	CHECK(apply(&d) == 0);
	rt_lock();
	CHECK(rt_netif_find("net0.5") && rt_netif_find("net0.5")->vlan_pcp == 3);
	rt_unlock();
	rp_list_set(&d, RP_LIST_ALIAS, rp_list_count(&d, RP_LIST_ALIAS) - 1, "");
	CHECK(apply(&d) == 0);
	/* Card off on a card that runs: Apply closes it */
	CHECK(fake_index("net1") >= 0);
	d.cur.ifc[1].disabled = 1;
	CHECK(apply(&d) >= 0);
	CHECK(fake_index("net1") < 0);
	rp_conf_free(&c);
	rp_conf_free(&d);
}

int main(void)
{
	load_stackproc();
	test_interface_options();
	test_wiring();
	test_vlan_ipv4_off();
	test_cycles();
	printf("t_prefs_wiring: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
