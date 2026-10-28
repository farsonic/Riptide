/*
 * Riptide - RiptidePrefs: riptide.conf model and line editor (see
 * conf_edit.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt/rt_types.h"
#include "conf_edit.h"

#define MAXTOK 24
#define LINEMAX 512

const char *const rp_sc_names[RP_NSC] = {
	"net.inet.tcp.fastopen", "net.inet.tcp.fastopen_all", "net.inet.tcp.fastopen_blackhole",
	"net.inet.tcp.syn_rto_initial_ms", "net.inet.tcp.syn_rto_max_ms", "net.inet.tcp.syn_backoff",
	"net.inet.tcp.connect_timeout", "net.inet.tcp.syn_icmp_abort", "net.inet.tcp.tw_reuse",
	"net.inet.tcp.max_tw_buckets", "net.inet.ip.portrange.first", "net.inet.ip.portrange.last",
	"net.inet.tcp.keepalive_all", "net.inet.tcp.keepidle", "net.inet.tcp.keepintvl", "net.inet.tcp.keepcnt",
	"net.inet.tcp.cc", "net.inet.tcp.ledbat_target", "net.inet.tcp.ledbat_slowdown", "net.inet.tcp.bg_mode",
	"net.inet.tcp.mem_low", "net.inet.tcp.mem_pressure", "net.inet.tcp.mem_high", "net.inet.he.enable",
	"net.inet.he.attempt_delay", "net.inet.he.resolution_delay", "net.inet.he.prefer",
	"net.inet.he.aaaa_filter", "net.inet.ip.qos_inflight", "net.inet.ip.qos_limit",
	"net.inet.ip.qos_prio_dscp", "dns.edns_size", "dns.tcp_fallback"
};
const char *const rp_ss_names[RP_NSS] = {
	"net.inet.tcp.bg_programs", "net.inet.tcp.bg_ports", "net.inet.tcp.bg_lports"
};
const char *const rp_cc_names[] = { "cubic", "reno", "ledbat", NULL };

struct toks {
	int n;
	char *t[MAXTOK];          /* tokens, quotes removed */
	char *raw[MAXTOK];        /* tokens as written */
	char buf[LINEMAX * 2 + 8];
	const char *comment;      /* trailing "  # ..." of the line, or "" */
};

/* ---- small helpers ---- */

static char *dup_n(const char *s, size_t n)
{
	char *p = rp_alloc(n + 1);

	if (p) {
		memcpy(p, s, n);
		p[n] = 0;
	}
	return p;
}

static char *dup_s(const char *s)
{
	return dup_n(s, strlen(s));
}

static void copy(char *dst, const char *src, size_t size)
{
	rt_strlcpy(dst, src, size);
}

static int eq(const char *a, const char *b)
{
	return rt_strcasecmp(a, b) == 0;
}

static int is_space(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r';
}

/* blank or comment line */
static int is_passive(const char *s)
{
	while (is_space(*s))
		s++;
	return *s == 0 || *s == '#' || *s == ';';
}

static int tokenize(const char *s, struct toks *tk)
{
	char *o = tk->buf, *end = tk->buf + sizeof(tk->buf) - 2;
	const char *p = s;

	tk->n = 0;
	tk->comment = "";
	while (*p && tk->n < MAXTOK) {
		const char *start;
		char *t, *r;
		int q = 0;
		while (is_space(*p))
			p++;
		if (!*p)
			break;
		if (*p == '#' || *p == ';') {
			/* keep the spaces before the comment */
			const char *c = p;
			while (c > s && is_space(c[-1]))
				c--;
			tk->comment = c;
			break;
		}
		start = p;
		/* unquoted token */
		t = o;
		while (*p && (q || !is_space(*p)) && o < end) {
			if (*p == '"')
				q = !q;
			else
				*o++ = *p;
			p++;
		}
		*o++ = 0;
		/* the same token as written */
		r = o;
		if ((size_t)(p - start) >= (size_t)(end - o))
			return -1;
		memcpy(r, start, (size_t)(p - start));
		o += p - start;
		*o++ = 0;
		tk->t[tk->n] = t;
		tk->raw[tk->n] = r;
		tk->n++;
	}
	return tk->n;
}

static int ncase(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int x = (unsigned char)a[i], y = (unsigned char)b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return x - y;
		if (!x)
			return 0;
	}
	return 0;
}

static const char *kv(const char *tok, const char *key)
{
	size_t l = strlen(key);

	if (!ncase(tok, key, l) && tok[l] == '=')
		return tok + l + 1;
	return NULL;
}

static long num(const char *s)
{
	const char *e;
	unsigned long v;

	if (!s || !*s)
		return RP_UNSET;
	v = rt_strtoul(s, &e, 0);
	if (*e || v > 0x7fffffffUL)
		return RP_UNSET;
	return (long)v;
}

/* tokens from..n-1 as written, separated by one space */
static void join_raw(const struct toks *tk, int from, char *out, size_t size)
{
	size_t o = 0;
	int i;

	out[0] = 0;
	for (i = from; i < tk->n; i++)
		o += (size_t)rt_snprintf(out + o, size > o ? size - o : 0, "%s%s", i > from ? " " : "", tk->raw[i]);
}

/* the value of a sysctl line for a known name: 1 if it was taken */
static int parse_sc(struct rp_conf *c, struct rp_line *l, const char *name, const char *val)
{
	struct rp_model *m = &c->orig;
	int i, k;

	for (i = 0; i < RP_NSC; i++)
		if (eq(name, rp_sc_names[i])) {
			long v = RP_UNSET;
			if (c->l_sc[i])
				return 0;
			if (i == RP_SC_CC) {
				for (k = 0; rp_cc_names[k]; k++)
					if (eq(val, rp_cc_names[k]))
						v = k;
			} else {
				v = num(val);
			}
			if (v == RP_UNSET)
				return 0;
			c->l_sc[i] = l;
			m->sc[i] = v;
			return 1;
		}
	for (i = 0; i < RP_NSS; i++)
		if (eq(name, rp_ss_names[i])) {
			if (c->l_ss[i] || strlen(val) >= RP_SSLEN || strchr(val, ' '))
				return 0;
			c->l_ss[i] = l;
			copy(m->ss[i], val, sizeof(m->ss[i]));
			return 1;
		}
	return 0;
}

/* ---- line list ---- */

static struct rp_line *line_new(const char *text)
{
	struct rp_line *l = rp_alloc(sizeof(*l));

	if (!l)
		return NULL;
	l->next = l->prev = NULL;
	l->text = dup_s(text);
	if (!l->text) {
		rp_free(l);
		return NULL;
	}
	return l;
}

static void line_append(struct rp_conf *c, struct rp_line *l)
{
	l->prev = c->tail;
	l->next = NULL;
	if (c->tail)
		c->tail->next = l;
	else
		c->head = l;
	c->tail = l;
}

/* after == NULL: at the end */
static void line_insert_after(struct rp_conf *c, struct rp_line *after, struct rp_line *l)
{
	if (!after) {
		line_append(c, l);
		return;
	}
	l->prev = after;
	l->next = after->next;
	if (after->next)
		after->next->prev = l;
	else
		c->tail = l;
	after->next = l;
}

static void line_unlink(struct rp_conf *c, struct rp_line *l)
{
	if (l->prev)
		l->prev->next = l->next;
	else
		c->head = l->next;
	if (l->next)
		l->next->prev = l->prev;
	else
		c->tail = l->prev;
	rp_free(l->text);
	rp_free(l);
}

static int line_set(struct rp_line *l, const char *text)
{
	char *t;

	if (!strcmp(l->text, text))
		return 0;
	t = dup_s(text);
	if (!t)
		return -1;
	rp_free(l->text);
	l->text = t;
	return 0;
}

/* the first line that is not a comment or blank (insert before it), as
 * the line after which to insert: the last of the leading comments */
static struct rp_line *top_anchor(struct rp_conf *c)
{
	struct rp_line *l, *last = NULL;

	for (l = c->head; l && is_passive(l->text); l = l->next)
		last = l;
	return last;
}

/* ---- parsing ---- */

static void model_defaults(struct rp_model *m)
{
	int i;

	memset(m, 0, sizeof(*m));
	copy(m->ifname, "eth0", sizeof(m->ifname));
	copy(m->device, "uaenet.device", sizeof(m->device));
	m->unit = 0;
	m->mtu = m->reads = m->tcpwindow = RP_UNSET;
	m->dhcp = 1;
	m->linklocal = 1;
	m->ipv6 = 1;
	m->mdns = -1;
	m->quickack = m->delack_segs = m->delack_ms = RP_UNSET;
	m->ipv4 = 1;
	m->qos = -1;
	for (i = 0; i < RP_NSC; i++)
		m->sc[i] = RP_UNSET;
}

static void parse_interface(struct rp_conf *c, struct rp_line *l, struct toks *tk)
{
	struct rp_model *m = &c->orig;
	const char *v;
	int i, have_dev = 0;

	for (i = 2; i < tk->n; i++)
		if (kv(tk->t[i], "device"))
			have_dev = 1;
	if (!have_dev || c->l_iface)
		return;
	c->l_iface = l;
	copy(m->ifname, tk->t[1], sizeof(m->ifname));
	m->unit = 0;
	for (i = 2; i < tk->n; i++) {
		if ((v = kv(tk->t[i], "device")) != NULL)
			copy(m->device, v, sizeof(m->device));
		else if ((v = kv(tk->t[i], "unit")) != NULL)
			m->unit = num(v);
		else if ((v = kv(tk->t[i], "mtu")) != NULL)
			m->mtu = num(v);
		else if ((v = kv(tk->t[i], "reads")) != NULL)
			m->reads = num(v);
		else if ((v = kv(tk->t[i], "tcpwindow")) != NULL)
			m->tcpwindow = num(v);
		else if (eq(tk->t[i], "raw"))
			m->raw = 1;
		else {
			size_t o = strlen(m->ifextra);
			rt_snprintf(m->ifextra + o, sizeof(m->ifextra) - o, "%s%s", o ? " " : "", tk->raw[i]);
		}
	}
	if (m->unit == RP_UNSET)
		m->unit = 0;
}

static void parse_line(struct rp_conf *c, struct rp_line *l, struct toks *tk)
{
	struct rp_model *m = &c->orig;
	const char *k = tk->t[0];
	int i;

	if (eq(k, "interface") && tk->n >= 3 && c->l_iface != l) {
		/* interface IF.ID vlan=ID parent=IF [dhcp|address=A/BITS|up] [pcp=N] */
		char e[RP_VLANLEN], name[24];
		const char *vid = NULL, *par = NULL, *mode = "", *pcp = NULL, *v;
		int ok = 1;
		for (i = 2; i < tk->n && ok; i++) {
			if ((v = kv(tk->t[i], "vlan")) != NULL)
				vid = v;
			else if ((v = kv(tk->t[i], "parent")) != NULL)
				par = v;
			else if ((v = kv(tk->t[i], "address")) != NULL)
				mode = v;
			else if ((v = kv(tk->t[i], "pcp")) != NULL)
				pcp = v;
			else if (eq(tk->t[i], "dhcp") || eq(tk->t[i], "up"))
				mode = tk->t[i];
			else
				ok = 0;
		}
		if (!ok || !vid || !par || !eq(par, m->ifname))
			return;
		rt_snprintf(name, sizeof(name), "%s.%s", m->ifname, vid);
		if (!eq(tk->t[1], name))
			return;
		rt_snprintf(e, sizeof(e), "%s%s%s%s%s", vid, mode[0] ? " " : "", mode, pcp ? " pcp=" : "", pcp ? pcp : "");
		if (strlen(e) + 1 < sizeof(e) && rp_valid_vlan(e))
			for (i = 0; i < RP_NVLAN; i++)
				if (!c->l_vlan[i]) {
					c->l_vlan[i] = l;
					copy(m->vlan[i], e, sizeof(m->vlan[i]));
					break;
				}
	} else if (eq(k, "dhcp")) {
		/* dhcp IF [start] [linklocal on|off] */
		i = tk->n >= 3 && eq(tk->t[2], "start") ? 3 : 2;
		if (!c->l_dhcp && tk->n >= 2 && eq(tk->t[1], m->ifname) &&
		    (tk->n == i || (tk->n == i + 2 && eq(tk->t[i], "linklocal") &&
		                    (eq(tk->t[i + 1], "on") || eq(tk->t[i + 1], "off"))))) {
			c->l_dhcp = l;
			m->dhcp = 1;
			m->linklocal = tk->n == i || eq(tk->t[i + 1], "on");
		}
	} else if (eq(k, "ifconfig") && tk->n >= 4 && eq(tk->t[1], m->ifname) &&
	           (eq(tk->t[2], "alias") || eq(tk->t[2], "add"))) {
		/* ifconfig IF alias A.B.C.D[/BITS] [netmask M] [broadcast B] */
		char spec[RP_ALIASLEN + 8];
		join_raw(tk, 3, spec, sizeof(spec));
		if (strlen(spec) < RP_ALIASLEN && rp_valid_alias(spec))
			for (i = 0; i < RP_NALIAS; i++)
				if (!c->l_alias[i]) {
					c->l_alias[i] = l;
					copy(m->alias[i], spec, sizeof(m->alias[i]));
					break;
				}
	} else if (eq(k, "ifconfig")) {
		/* ifconfig IF A.B.C.D[/BITS] [netmask M] [up] */
		const char *addr = NULL, *mask = NULL;
		int ok = !c->l_ifconfig && tk->n >= 3 && eq(tk->t[1], m->ifname);
		for (i = 2; ok && i < tk->n; i++) {
			if (eq(tk->t[i], "up") || eq(tk->t[i], "inet"))
				continue;
			if (eq(tk->t[i], "netmask") && i + 1 < tk->n && rp_valid_ip4(tk->t[i + 1], 0)) {
				mask = tk->t[++i];
				continue;
			}
			if (!addr && rp_valid_ip4(tk->t[i], 1)) {
				addr = tk->t[i];
				continue;
			}
			ok = 0;
		}
		if (ok && addr) {
			c->l_ifconfig = l;
			copy(m->addr, addr, sizeof(m->addr));
			copy(m->netmask, mask ? mask : "", sizeof(m->netmask));
			if (!c->l_dhcp)
				m->dhcp = 0;
		}
	} else if (eq(k, "route")) {
		/* route add default [via|gw] GW */
		if (!c->l_route && tk->n >= 4 && eq(tk->t[1], "add") && eq(tk->t[2], "default")) {
			i = 3;
			if (tk->n == 5 && (eq(tk->t[3], "via") || eq(tk->t[3], "gw")))
				i = 4;
			if (i == tk->n - 1 && rp_valid_ip4(tk->t[i], 0)) {
				c->l_route = l;
				copy(m->gateway, tk->t[i], sizeof(m->gateway));
			}
		}
	} else if (eq(k, "nameserver")) {
		const char *a = NULL;
		if (tk->n == 2)
			a = tk->t[1];
		else if (tk->n == 3 && eq(tk->t[1], "add"))
			a = tk->t[2];
		if (a && rp_valid_ip(a))
			for (i = 0; i < RP_NDNS; i++)
				if (!c->l_dns[i]) {
					c->l_dns[i] = l;
					copy(m->dns[i], a, sizeof(m->dns[i]));
					break;
				}
	} else if (eq(k, "hostname") && tk->n == 2 && !c->l_hostname) {
		c->l_hostname = l;
		copy(m->hostname, tk->t[1], sizeof(m->hostname));
	} else if (eq(k, "domain") && tk->n == 2 && !c->l_domain) {
		c->l_domain = l;
		copy(m->domain, tk->t[1], sizeof(m->domain));
	} else if (eq(k, RP_KW_IPV6) && tk->n == 2 && (eq(tk->t[1], "on") || eq(tk->t[1], "off")) &&
	           !c->l_ipv6) {
		c->l_ipv6 = l;
		m->ipv6 = eq(tk->t[1], "on");
	} else if (eq(k, RP_KW_IPV4) && tk->n == 2 && (eq(tk->t[1], "on") || eq(tk->t[1], "off")) &&
	           !c->l_ipv4) {
		c->l_ipv4 = l;
		m->ipv4 = eq(tk->t[1], "on");
	} else if (eq(k, RP_KW_QOS) && tk->n == 2 && (eq(tk->t[1], "on") || eq(tk->t[1], "off")) && !c->l_qos) {
		c->l_qos = l;
		m->qos = eq(tk->t[1], "on");
	} else if (eq(k, RP_KW_QOS) && tk->n >= 3 && eq(tk->t[1], "rule")) {
		char spec[RP_LONG + 8];
		join_raw(tk, 2, spec, sizeof(spec));
		if (strlen(spec) < RP_LONG && rp_valid_qrule(spec))
			for (i = 0; i < RP_NQRULE; i++)
				if (!c->l_qrule[i]) {
					c->l_qrule[i] = l;
					copy(m->qrule[i], spec, sizeof(m->qrule[i]));
					break;
				}
	} else if (eq(k, RP_KW_NTP) && tk->n >= 2 && !c->l_ntp) {
		c->l_ntp = l;
		if (tk->n == 2 && eq(tk->t[1], RP_KW_NTP_OFF)) {
			m->ntp = 0;
		} else if (tk->n == 2 && eq(tk->t[1], RP_KW_NTP_ON)) {
			m->ntp = 1;                    /* servers from DHCP, else pool.ntp.org */
		} else {
			m->ntp = 1;
			for (i = 1; i < tk->n; i++) {
				size_t o = strlen(m->ntp_servers);
				rt_snprintf(m->ntp_servers + o, sizeof(m->ntp_servers) - o, "%s%s", o ? " " : "", tk->t[i]);
			}
		}
	} else if (eq(k, RP_KW_TIMEZONE) && tk->n == 2 && !c->l_tz) {
		c->l_tz = l;
		copy(m->timezone, tk->t[1], sizeof(m->timezone));
	} else if (eq(k, RP_KW_MDNS) && tk->n == 2 && (eq(tk->t[1], "on") || eq(tk->t[1], "off")) && !c->l_mdns) {
		c->l_mdns = l;
		m->mdns = eq(tk->t[1], "on");
	} else if (eq(k, "sysctl") && (tk->n == 2 || tk->n == 3)) {
		char name[64];
		const char *val, *eqs = strchr(tk->t[1], '=');
		struct rp_line **slot = NULL;
		long *field = NULL;
		if (tk->n == 2 && eqs) {
			size_t n = (size_t)(eqs - tk->t[1]);
			if (n >= sizeof(name))
				return;
			memcpy(name, tk->t[1], n);
			name[n] = 0;
			val = eqs + 1;
		} else if (tk->n == 3 && !eqs) {
			copy(name, tk->t[1], sizeof(name));
			val = tk->t[2];
		} else {
			return;
		}
		if (eq(name, "net.inet.tcp.quickack")) {
			slot = &c->l_quickack;
			field = &m->quickack;
		} else if (eq(name, "net.inet.tcp.delack_segs")) {
			slot = &c->l_delack_segs;
			field = &m->delack_segs;
		} else if (eq(name, "net.inet.tcp.delack_ms")) {
			slot = &c->l_delack_ms;
			field = &m->delack_ms;
		}
		if (slot && !*slot && num(val) != RP_UNSET) {
			*slot = l;
			*field = num(val);
		} else if (!slot) {
			parse_sc(c, l, name, val);
		}
	}
}

int rp_conf_load(struct rp_conf *c, const char *text, size_t len)
{
	struct toks *tk;
	struct rp_line *l;
	size_t i = 0;
	int crlf = 0, lf = 0;

	memset(c, 0, sizeof(*c));
	model_defaults(&c->orig);
	c->final_eol = 1;
	while (i < len) {
		size_t j = i, e;
		while (j < len && text[j] != '\n')
			j++;
		e = j;
		if (j < len) {
			lf++;
			if (e > i && text[e - 1] == '\r') {
				crlf++;
				e--;
			}
		}
		l = rp_alloc(sizeof(*l));
		if (!l)
			goto oom;
		l->text = dup_n(text + i, e - i);
		if (!l->text) {
			rp_free(l);
			goto oom;
		}
		line_append(c, l);
		c->final_eol = j < len;
		i = j + 1;
	}
	c->eol_crlf = lf && crlf == lf;
	tk = rp_alloc(sizeof(*tk));
	if (!tk)
		goto oom;
	/* the interface first: the address lines refer to its name */
	for (l = c->head; l; l = l->next)
		if (!is_passive(l->text) && tokenize(l->text, tk) >= 3 && eq(tk->t[0], "interface"))
			parse_interface(c, l, tk);
	for (l = c->head; l; l = l->next)
		if (!is_passive(l->text) && tokenize(l->text, tk) >= 1)
			parse_line(c, l, tk);
	rp_free(tk);
	c->cur = c->orig;
	memcpy(c->h_alias, c->orig.alias, sizeof(c->h_alias));
	memcpy(c->h_qrule, c->orig.qrule, sizeof(c->h_qrule));
	memcpy(c->h_vlan, c->orig.vlan, sizeof(c->h_vlan));
	copy(c->h_ifname, c->orig.ifname, sizeof(c->h_ifname));
	copy(c->h_vifname, c->orig.ifname, sizeof(c->h_vifname));
	return 0;
oom:
	rp_conf_free(c);
	return -1;
}

void rp_conf_free(struct rp_conf *c)
{
	while (c->head)
		line_unlink(c, c->head);
}

/* ---- writing ---- */

static int str_changed(const char *a, const char *b)
{
	return strcmp(a, b) != 0;
}

static int iface_changed(const struct rp_model *o, const struct rp_model *n)
{
	return str_changed(o->ifname, n->ifname) || str_changed(o->device, n->device) || o->unit != n->unit ||
	       o->mtu != n->mtu || o->reads != n->reads || o->tcpwindow != n->tcpwindow || o->raw != n->raw ||
	       str_changed(o->ifextra, n->ifextra);
}

static int addr_changed(const struct rp_model *o, const struct rp_model *n)
{
	return o->dhcp != n->dhcp || str_changed(o->ifname, n->ifname) ||
	       (n->dhcp && o->linklocal != n->linklocal) ||
	       (!n->dhcp && (str_changed(o->addr, n->addr) || str_changed(o->netmask, n->netmask) ||
	                     str_changed(o->gateway, n->gateway)));
}

static int list_changed(const struct rp_model *o, const struct rp_model *n, int which)
{
	int i;

	if (which == RP_LIST_ALIAS) {
		for (i = 0; i < RP_NALIAS; i++)
			if (str_changed(o->alias[i], n->alias[i]))
				return 1;
	} else if (which == RP_LIST_VLAN) {
		for (i = 0; i < RP_NVLAN; i++)
			if (str_changed(o->vlan[i], n->vlan[i]))
				return 1;
	} else {
		for (i = 0; i < RP_NQRULE; i++)
			if (str_changed(o->qrule[i], n->qrule[i]))
				return 1;
	}
	return 0;
}

static int dns_changed(const struct rp_model *o, const struct rp_model *n)
{
	int i;

	for (i = 0; i < RP_NDNS; i++)
		if (str_changed(o->dns[i], n->dns[i]))
			return 1;
	return 0;
}

int rp_conf_changed(const struct rp_conf *c)
{
	return memcmp(&c->orig, &c->cur, sizeof(c->orig)) != 0;
}

void rp_conf_commit(struct rp_conf *c)
{
	c->orig = c->cur;
}

/* rewrite (keeping a trailing comment), insert after 'anchor', or with
 * text NULL remove the line of *slot */
static int put(struct rp_conf *c, struct rp_line **slot, struct rp_line *anchor, const char *text)
{
	char buf[LINEMAX];
	struct rp_line *l;

	if (!text) {
		if (*slot) {
			line_unlink(c, *slot);
			*slot = NULL;
		}
		return 0;
	}
	if (*slot) {
		struct toks *tk = rp_alloc(sizeof(*tk));
		const char *comment = "";
		int r;
		if (!tk)
			return -1;
		if (tokenize((*slot)->text, tk) >= 0)
			comment = tk->comment;
		rt_snprintf(buf, sizeof(buf), "%s%s", text, comment);
		r = line_set(*slot, buf);
		rp_free(tk);
		return r;
	}
	l = line_new(text);
	if (!l)
		return -1;
	line_insert_after(c, anchor, l);
	*slot = l;
	return 0;
}

static const char *quoted(const char *s, char *buf, size_t size)
{
	if (strchr(s, ' ') || strchr(s, '\t') || strchr(s, '#'))
		rt_snprintf(buf, size, "\"%s\"", s);
	else
		rt_snprintf(buf, size, "%s", s);
	return buf;
}

static struct rp_line *first_of(struct rp_line *a, struct rp_line *b)
{
	return a ? a : b;
}

/* is text among the n entries (of len bytes) from 'from' on? */
static int in_list(const char *base, int from, int n, size_t len, const char *text)
{
	for (; from < n && base[(size_t)from * len]; from++)
		if (!strcmp(base + (size_t)from * len, text))
			return 1;
	return 0;
}

/* "interface IF.ID vlan=ID parent=IF [dhcp|address=A/BITS|up] [pcp=N]"
 * for the VLAN entry "ID [MODE] [pcp=N]" */
static void vlan_line(const char *ifname, const char *e, char *buf, size_t size)
{
	char w[3][RP_VLANLEN];
	int n = 0, k;
	size_t i;

	memset(w, 0, sizeof(w));
	while (*e && n < 3) {
		while (*e == ' ')
			e++;
		for (i = 0; *e && *e != ' '; e++)
			if (i < sizeof(w[0]) - 1)
				w[n][i++] = *e;
		if (i)
			n++;
	}
	k = rt_snprintf(buf, size, "interface %s.%s vlan=%s parent=%s", ifname, w[0], w[0], ifname);
	for (i = 1; i < (size_t)n && (size_t)k < size; i++) {
		if (!strncmp(w[i], "pcp=", 4) || eq(w[i], "dhcp") || eq(w[i], "up"))
			k += rt_snprintf(buf + k, size - (size_t)k, " %s", w[i]);
		else
			k += rt_snprintf(buf + k, size - (size_t)k, " address=%s", w[i]);
	}
}

static void list_line(const struct rp_conf *c, int which, const char *e, char *buf, size_t size)
{
	if (which == RP_LIST_ALIAS)
		rt_snprintf(buf, size, "ifconfig %s alias %s", c->cur.ifname, e);
	else if (which == RP_LIST_VLAN)
		vlan_line(c->cur.ifname, e, buf, size);
	else
		rt_snprintf(buf, size, "%s rule %s", RP_KW_QOS, e);
}

/* Bring a list's lines in line with want[]: have[] is what the lines say
 * now (slot i: have[i]). Lines of entries that stay keep their place and
 * comment, a replaced entry is rewritten in place, deleted ones go, new
 * ones follow the entry before them (the first one 'anchor'). */
static int sync_list(struct rp_conf *c, int which, struct rp_line **slot, char *have, const char *want, int n,
                     size_t len, struct rp_line *anchor)
{
	struct rp_line *ns[RP_NQRULE], *prev = anchor, *l;
	char buf[LINEMAX];
	int i = 0, j = 0, k = 0, r = 0;

	for (;;) {
		const char *h = i < n && have[(size_t)i * len] ? have + (size_t)i * len : NULL;
		const char *w = j < n && want[(size_t)j * len] ? want + (size_t)j * len : NULL;
		if (!h && !w)
			break;
		if (h && w && (!strcmp(h, w) || (!in_list(want, j + 1, n, len, h) && !in_list(have, i + 1, n, len, w)))) {
			/* the same (untouched unless the interface was renamed), or
			 * replaced: in place */
			if (strcmp(h, w) || (which == RP_LIST_ALIAS && strcmp(c->h_ifname, c->cur.ifname)) ||
			    (which == RP_LIST_VLAN && strcmp(c->h_vifname, c->cur.ifname))) {
				list_line(c, which, w, buf, sizeof(buf));
				r |= put(c, &slot[i], NULL, buf);
			}
			prev = ns[k++] = slot[i];
			i++;
			j++;
		} else if (h && !in_list(want, j, n, len, h)) {
			line_unlink(c, slot[i]);
			i++;
		} else {
			list_line(c, which, w, buf, sizeof(buf));
			l = line_new(buf);
			if (!l)
				return -1;
			line_insert_after(c, prev, l);
			prev = ns[k++] = l;
			j++;
		}
	}
	for (i = 0; i < n; i++)
		slot[i] = i < k ? ns[i] : NULL;
	memcpy(have, want, (size_t)n * len);
	if (which == RP_LIST_ALIAS)
		copy(c->h_ifname, c->cur.ifname, sizeof(c->h_ifname));
	if (which == RP_LIST_VLAN)
		copy(c->h_vifname, c->cur.ifname, sizeof(c->h_vifname));
	return r;
}

/* bring the lines in line with c->cur */
static int sync_lines(struct rp_conf *c)
{
	const struct rp_model *o = &c->orig, *n = &c->cur;
	char buf[LINEMAX], q[RP_STR + 4];
	struct rp_line *anchor;
	int i, r = 0;

	if (str_changed(o->hostname, n->hostname)) {
		rt_snprintf(buf, sizeof(buf), "hostname %s", n->hostname);
		r |= put(c, &c->l_hostname, top_anchor(c), n->hostname[0] ? buf : NULL);
	}
	if (str_changed(o->domain, n->domain)) {
		rt_snprintf(buf, sizeof(buf), "domain %s", n->domain);
		r |= put(c, &c->l_domain, first_of(c->l_hostname, top_anchor(c)), n->domain[0] ? buf : NULL);
	}
	if (iface_changed(o, n)) {
		int k = rt_snprintf(buf, sizeof(buf), "interface %s device=%s unit=%ld", n->ifname,
		                    quoted(n->device, q, sizeof(q)), n->unit < 0 ? 0L : n->unit);
		if (n->mtu > 0)
			k += rt_snprintf(buf + k, sizeof(buf) - (size_t)k, " mtu=%ld", n->mtu);
		if (n->reads > 0)
			k += rt_snprintf(buf + k, sizeof(buf) - (size_t)k, " reads=%ld", n->reads);
		if (n->tcpwindow > 0)
			k += rt_snprintf(buf + k, sizeof(buf) - (size_t)k, " tcpwindow=%ld", n->tcpwindow);
		if (n->raw)
			k += rt_snprintf(buf + k, sizeof(buf) - (size_t)k, " raw");
		if (n->ifextra[0])
			rt_snprintf(buf + k, sizeof(buf) - (size_t)k, " %s", n->ifextra);
		r |= put(c, &c->l_iface, first_of(c->l_domain, first_of(c->l_hostname, top_anchor(c))), buf);
	}
	if (addr_changed(o, n)) {
		anchor = c->l_iface;
		if (n->dhcp) {
			if (c->l_ifconfig && !c->l_dhcp) {
				anchor = c->l_ifconfig->prev;
				if (anchor && anchor == c->l_route)
					anchor = anchor->prev;
			}
			r |= put(c, &c->l_ifconfig, NULL, NULL);
			r |= put(c, &c->l_route, NULL, NULL);
			rt_snprintf(buf, sizeof(buf), "dhcp %s%s", n->ifname, n->linklocal ? "" : " linklocal off");
			r |= put(c, &c->l_dhcp, anchor, buf);
		} else {
			if (c->l_dhcp && !c->l_ifconfig)
				anchor = c->l_dhcp->prev;
			r |= put(c, &c->l_dhcp, NULL, NULL);
			if (n->netmask[0])
				rt_snprintf(buf, sizeof(buf), "ifconfig %s %s netmask %s up", n->ifname, n->addr, n->netmask);
			else
				rt_snprintf(buf, sizeof(buf), "ifconfig %s %s up", n->ifname, n->addr);
			r |= put(c, &c->l_ifconfig, anchor, n->addr[0] ? buf : NULL);
			rt_snprintf(buf, sizeof(buf), "route add default %s", n->gateway);
			r |= put(c, &c->l_route, first_of(c->l_ifconfig, anchor), n->gateway[0] ? buf : NULL);
		}
	}
	if (dns_changed(o, n)) {
		anchor = first_of(c->l_route, first_of(c->l_ifconfig, first_of(c->l_dhcp, c->l_iface)));
		for (i = 0; i < RP_NDNS; i++) {
			rt_snprintf(buf, sizeof(buf), "nameserver %s", n->dns[i]);
			r |= put(c, &c->l_dns[i], anchor, n->dns[i][0] ? buf : NULL);
			if (c->l_dns[i])
				anchor = c->l_dns[i];
		}
	}
	if (o->ipv6 != n->ipv6) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_IPV6, n->ipv6 ? "on" : "off");
		r |= put(c, &c->l_ipv6, NULL, (n->ipv6 && !c->l_ipv6) ? NULL : buf);
	}
	if (o->ntp != n->ntp || str_changed(o->ntp_servers, n->ntp_servers)) {
		if (n->ntp && n->ntp_servers[0])
			rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_NTP, n->ntp_servers);
		else
			rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_NTP, n->ntp ? RP_KW_NTP_ON : RP_KW_NTP_OFF);
		r |= put(c, &c->l_ntp, NULL, (!n->ntp && !c->l_ntp) ? NULL : buf);
	}
	if (str_changed(o->timezone, n->timezone)) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_TIMEZONE, n->timezone);
		r |= put(c, &c->l_tz, c->l_ntp, n->timezone[0] ? buf : NULL);
	}
	if (o->mdns != n->mdns) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_MDNS, n->mdns ? "on" : "off");
		r |= put(c, &c->l_mdns, NULL, n->mdns < 0 ? NULL : buf);
	}
#define SYSCTL(f, slot, name) \
	if (o->f != n->f) { \
		rt_snprintf(buf, sizeof(buf), "sysctl %s=%ld", name, n->f); \
		r |= put(c, &c->slot, NULL, n->f == RP_UNSET ? NULL : buf); \
	}
	SYSCTL(quickack, l_quickack, "net.inet.tcp.quickack")
	SYSCTL(delack_segs, l_delack_segs, "net.inet.tcp.delack_segs")
	SYSCTL(delack_ms, l_delack_ms, "net.inet.tcp.delack_ms")
#undef SYSCTL
	if (o->ipv4 != n->ipv4) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_IPV4, n->ipv4 ? "on" : "off");
		r |= put(c, &c->l_ipv4, c->l_ipv6, (n->ipv4 && !c->l_ipv4) ? NULL : buf);
	}
	if (o->qos != n->qos) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_QOS, n->qos ? "on" : "off");
		r |= put(c, &c->l_qos, NULL, n->qos < 0 ? NULL : buf);
	}
	for (i = 0; i < RP_NSC; i++)
		if (o->sc[i] != n->sc[i]) {
			if (i == RP_SC_CC && n->sc[i] >= 0)
				rt_snprintf(buf, sizeof(buf), "sysctl %s=%s", rp_sc_names[i], rp_cc_names[n->sc[i]]);
			else
				rt_snprintf(buf, sizeof(buf), "sysctl %s=%ld", rp_sc_names[i], n->sc[i]);
			r |= put(c, &c->l_sc[i], NULL, n->sc[i] == RP_UNSET ? NULL : buf);
		}
	for (i = 0; i < RP_NSS; i++)
		if (str_changed(o->ss[i], n->ss[i])) {
			rt_snprintf(buf, sizeof(buf), "sysctl %s=%s", rp_ss_names[i], n->ss[i]);
			r |= put(c, &c->l_ss[i], NULL, n->ss[i][0] ? buf : NULL);
		}
	r |= sync_list(c, RP_LIST_ALIAS, c->l_alias, c->h_alias[0], n->alias[0], RP_NALIAS, RP_ALIASLEN,
	               first_of(c->l_route, first_of(c->l_ifconfig, first_of(c->l_dhcp, c->l_iface))));
	r |= sync_list(c, RP_LIST_QRULE, c->l_qrule, c->h_qrule[0], n->qrule[0], RP_NQRULE, RP_LONG, c->l_qos);
	/* new VLAN lines after the interface's address lines */
	anchor = first_of(c->l_route, first_of(c->l_ifconfig, first_of(c->l_dhcp, c->l_iface)));
	for (i = 0; i < RP_NALIAS; i++)
		if (c->l_alias[i])
			anchor = c->l_alias[i];
	r |= sync_list(c, RP_LIST_VLAN, c->l_vlan, c->h_vlan[0], n->vlan[0], RP_NVLAN, RP_VLANLEN, anchor);
	return r ? -1 : 0;
}

char *rp_conf_text(struct rp_conf *c, size_t *len)
{
	struct rp_line *l;
	size_t n = 0, eol = c->eol_crlf ? 2 : 1;
	char *t, *p;

	if (sync_lines(c) < 0)
		return NULL;
	for (l = c->head; l; l = l->next)
		n += strlen(l->text) + eol;
	t = rp_alloc(n + 1);
	if (!t)
		return NULL;
	p = t;
	for (l = c->head; l; l = l->next) {
		size_t k = strlen(l->text);
		memcpy(p, l->text, k);
		p += k;
		if (l->next || c->final_eol) {
			if (c->eol_crlf)
				*p++ = '\r';
			*p++ = '\n';
		}
	}
	*p = 0;
	*len = (size_t)(p - t);
	return t;
}

/* ---- other lines ---- */

static int is_bound(const struct rp_conf *c, const struct rp_line *l)
{
	int i;

	if (l == c->l_iface || l == c->l_dhcp || l == c->l_ifconfig || l == c->l_route || l == c->l_hostname ||
	    l == c->l_domain || l == c->l_ipv6 || l == c->l_ntp || l == c->l_tz || l == c->l_mdns ||
	    l == c->l_quickack || l == c->l_delack_segs || l == c->l_delack_ms)
		return 1;
	for (i = 0; i < RP_NDNS; i++)
		if (l == c->l_dns[i])
			return 1;
	if (l == c->l_ipv4 || l == c->l_qos)
		return 1;
	for (i = 0; i < RP_NSC; i++)
		if (l == c->l_sc[i])
			return 1;
	for (i = 0; i < RP_NSS; i++)
		if (l == c->l_ss[i])
			return 1;
	for (i = 0; i < RP_NALIAS; i++)
		if (l == c->l_alias[i])
			return 1;
	for (i = 0; i < RP_NQRULE; i++)
		if (l == c->l_qrule[i])
			return 1;
	for (i = 0; i < RP_NVLAN; i++)
		if (l == c->l_vlan[i])
			return 1;
	return 0;
}

static struct rp_line *other_line(const struct rp_conf *c, int idx)
{
	struct rp_line *l;

	for (l = c->head; l; l = l->next)
		if (!is_passive(l->text) && !is_bound(c, l) && idx-- == 0)
			return l;
	return NULL;
}

int rp_other_count(const struct rp_conf *c)
{
	struct rp_line *l;
	int n = 0;

	for (l = c->head; l; l = l->next)
		if (!is_passive(l->text) && !is_bound(c, l))
			n++;
	return n;
}

const char *rp_other_get(const struct rp_conf *c, int i)
{
	struct rp_line *l = other_line(c, i);

	return l ? l->text : NULL;
}

int rp_other_set(struct rp_conf *c, int i, const char *text)
{
	struct rp_line *l = other_line(c, i);

	if (!l)
		return -1;
	if (is_passive(text)) {
		line_unlink(c, l);
		return 0;
	}
	return line_set(l, text);
}

int rp_other_add(struct rp_conf *c, const char *text)
{
	struct rp_line *l;

	if (is_passive(text))
		return -1;
	l = line_new(text);
	if (!l)
		return -1;
	line_append(c, l);
	return 0;
}

/* ---- lists of the model ---- */

static char *list_entry(struct rp_model *m, int which, int i, int *n, size_t *size)
{
	if (which == RP_LIST_ALIAS) {
		*n = RP_NALIAS;
		*size = RP_ALIASLEN;
		return i >= 0 && i < RP_NALIAS ? m->alias[i] : NULL;
	}
	if (which == RP_LIST_VLAN) {
		*n = RP_NVLAN;
		*size = RP_VLANLEN;
		return i >= 0 && i < RP_NVLAN ? m->vlan[i] : NULL;
	}
	*n = RP_NQRULE;
	*size = RP_LONG;
	return i >= 0 && i < RP_NQRULE ? m->qrule[i] : NULL;
}

int rp_list_count(const struct rp_conf *c, int which)
{
	int i, n;
	size_t size;

	for (i = 0;; i++) {
		const char *e = list_entry((struct rp_model *)&c->cur, which, i, &n, &size);
		if (!e || !e[0])
			return i;
	}
}

const char *rp_list_get(const struct rp_conf *c, int which, int i)
{
	int n;
	size_t size;
	const char *e = list_entry((struct rp_model *)&c->cur, which, i, &n, &size);

	return e && e[0] ? e : NULL;
}

int rp_list_set(struct rp_conf *c, int which, int i, const char *text)
{
	int n, k;
	size_t size;
	char *e = list_entry(&c->cur, which, i, &n, &size);

	if (!e || !e[0] || strlen(text) >= size)
		return -1;
	if (text[0]) {
		memset(e, 0, size);
		copy(e, text, size);
		return 0;
	}
	/* delete: the later entries move up */
	for (k = i; k + 1 < n; k++)
		memcpy(list_entry(&c->cur, which, k, &n, &size), list_entry(&c->cur, which, k + 1, &n, &size), size);
	memset(list_entry(&c->cur, which, n - 1, &n, &size), 0, size);
	return 0;
}

int rp_list_add(struct rp_conf *c, int which, const char *text)
{
	int n;
	size_t size;
	char *e = list_entry(&c->cur, which, rp_list_count(c, which), &n, &size);

	if (!e || !text[0] || strlen(text) >= size)
		return -1;
	memset(e, 0, size);
	copy(e, text, size);
	return 0;
}

/* ---- validation ---- */

int rp_valid_ip4(const char *s, int allow_prefix)
{
	int parts = 0;

	for (;;) {
		unsigned v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9' && digits < 4) {
			v = v * 10 + (unsigned)(*s++ - '0');
			digits++;
		}
		if (!digits || v > 255)
			return 0;
		if (++parts == 4)
			break;
		if (*s++ != '.')
			return 0;
	}
	if (*s == '/' && allow_prefix) {
		unsigned b = 0;
		int digits = 0;
		s++;
		while (*s >= '0' && *s <= '9' && digits < 3) {
			b = b * 10 + (unsigned)(*s++ - '0');
			digits++;
		}
		if (!digits || b > 32)
			return 0;
	}
	return *s == 0;
}

int rp_valid_name(const char *s)
{
	int label = 0, total = 0;

	if (!*s)
		return 0;
	for (; *s; s++, total++) {
		char ch = *s;
		if (ch == '.') {
			if (!label)
				return 0;
			label = 0;
			continue;
		}
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
		      ch == '_'))
			return 0;
		if (++label > 63)
			return 0;
	}
	return total <= 253;
}

/* A.B.C.D[/BITS] [netmask M] [broadcast B] */
int rp_valid_alias(const char *s)
{
	char w[4][RP_ALIASLEN];
	int n = 0, i;
	size_t k;

	while (*s && n < 4) {
		while (*s == ' ')
			s++;
		for (k = 0; *s && *s != ' '; s++)
			if (k < sizeof(w[0]) - 1)
				w[n][k++] = *s;
		if (k)
			w[n++][k] = 0;
		while (*s == ' ')
			s++;
	}
	if (*s || !n || !rp_valid_ip4(w[0], 1))
		return 0;
	for (i = 1; i < n; i += 2)
		if (i + 1 >= n || !(eq(w[i], "netmask") || eq(w[i], "broadcast")) || !rp_valid_ip4(w[i + 1], 0))
			return 0;
	return 1;
}

/* ID [dhcp|up|A.B.C.D/BITS] [pcp=0-7|pcp=priority] */
int rp_valid_vlan(const char *s)
{
	char w[4][RP_VLANLEN];
	int n = 0, i, mode = 0, pcp = 0;
	size_t k;
	unsigned long id;
	const char *e;

	while (*s && n < 4) {
		while (*s == ' ')
			s++;
		for (k = 0; *s && *s != ' '; s++)
			if (k < sizeof(w[0]) - 1)
				w[n][k++] = *s;
		if (k)
			w[n++][k] = 0;
		while (*s == ' ')
			s++;
	}
	if (*s || !n || n > 3)
		return 0;
	id = rt_strtoul(w[0], &e, 10);
	if (*e || e == w[0] || id < 1 || id > 4094)
		return 0;
	for (i = 1; i < n; i++) {
		if (!strncmp(w[i], "pcp=", 4)) {
			unsigned long p = rt_strtoul(w[i] + 4, &e, 10);
			if (pcp++ || !(eq(w[i] + 4, "priority") || (!*e && e != w[i] + 4 && p <= 7)))
				return 0;
		} else if (eq(w[i], "dhcp") || eq(w[i], "up") || (strchr(w[i], '/') && rp_valid_ip4(w[i], 1))) {
			if (mode++)
				return 0;
		} else {
			return 0;
		}
	}
	return 1;
}

int rp_valid_ports(const char *s)
{
	while (*s) {
		unsigned long a, b;
		const char *e;
		a = rt_strtoul(s, &e, 10);
		if (e == s || a < 1 || a > 65535)
			return 0;
		s = e;
		if (*s == '-') {
			b = rt_strtoul(++s, &e, 10);
			if (e == s || b < a || b > 65535)
				return 0;
			s = e;
		}
		if (*s == ',' && s[1])
			s++;
		else if (*s)
			return 0;
	}
	return 1;
}

int rp_valid_programs(const char *s)
{
	if (strlen(s) >= RP_SSLEN)
		return 0;
	for (; *s; s++)
		if (*s == ' ' || *s == '\t' || *s == '"' || *s == '#' || *s == ';' || (*s == ',' && (s[1] == ',' || !s[1])))
			return 0;
	return 1;
}

/* "qos rule" words (as src/core/qos.c takes them): [proto=tcp|udp]
 * [port=|lport=|rport=P,.. (at most 8)] [program=NAME] [priority=
 * interactive|default|normal|bulk|background|0-7] [dscp=NAME|0-63]; a port
 * list or a program, and a priority or a DSCP */
int rp_valid_qrule(const char *s)
{
	static const char *const dscp[] = { "be", "ef", "cs0", "cs1", "cs2", "cs3", "cs4", "cs5", "cs6", "cs7",
		"af11", "af12", "af13", "af21", "af22", "af23", "af31", "af32", "af33", "af41", "af42", "af43",
		"va", "le", NULL };
	char w[RP_LONG];
	int what = 0, match = 0;

	if (strlen(s) >= RP_LONG)
		return 0;
	while (*s) {
		const char *v;
		size_t k;
		while (*s == ' ')
			s++;
		if (!*s)
			break;
		for (k = 0; *s && *s != ' '; s++)
			w[k++] = *s;
		w[k] = 0;
		if ((v = kv(w, "proto")) != NULL) {
			if (!eq(v, "tcp") && !eq(v, "udp"))
				return 0;
		} else if ((v = kv(w, "port")) != NULL || (v = kv(w, "lport")) != NULL || (v = kv(w, "rport")) != NULL) {
			int n = 1;
			const char *p;
			if (!*v || strchr(v, '-') || !rp_valid_ports(v))
				return 0;
			for (p = v; *p; p++)
				n += *p == ',';
			if (n > 8)
				return 0;
			match = 1;
		} else if ((v = kv(w, "program")) != NULL) {
			if (!*v || strlen(v) >= 32 || strchr(v, ',') || !rp_valid_programs(v))
				return 0;
			match = 1;
		} else if ((v = kv(w, "priority")) != NULL || (v = kv(w, "prio")) != NULL) {
			long p = num(v);
			if (!eq(v, "interactive") && !eq(v, "default") && !eq(v, "normal") && !eq(v, "bulk") &&
			    !eq(v, "background") && (p < 0 || p > 7))
				return 0;
			what = 1;
		} else if ((v = kv(w, "dscp")) != NULL) {
			long d = num(v);
			int i, ok = d >= 0 && d <= 63;
			for (i = 0; !ok && dscp[i]; i++)
				ok = eq(v, dscp[i]);
			if (!ok)
				return 0;
			what = 1;
		} else {
			return 0;
		}
	}
	return what && match;
}

int rp_valid_tz(const char *s)
{
	int h = 0, m = 0, d = 0;

	if (!strcmp(s, "auto"))
		return 1;
	if (*s == '+' || *s == '-')
		s++;
	while (*s >= '0' && *s <= '9' && d < 3)
		h = h * 10 + (*s++ - '0'), d++;
	if (!d || d > 2 || h > 14)
		return 0;
	if (*s == ':') {
		s++;
		if (!(s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9'))
			return 0;
		m = (s[0] - '0') * 10 + (s[1] - '0');
		s += 2;
	}
	return !*s && m < 60;
}

int rp_valid_ip(const char *s)
{
	int colons = 0, dbl = 0, group = 0, dots = 0;
	const char *p;

	if (rp_valid_ip4(s, 0))
		return 1;
	for (p = s;; p++) {
		if (*p == ':' || *p == 0 || *p == '%') {
			/* end of a group: up to 4 hex digits, or an IPv4 tail */
			if (dots ? (dots != 3 || *p == ':') : group > 4)
				return 0;
			if (*p != ':')
				break;
			if (p[1] == ':' && dbl++)
				return 0;
			colons++;
			group = dots = 0;
		} else if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
			group++;
		} else if (*p == '.') {
			dots++;
		} else {
			return 0;
		}
	}
	return colons >= 2 && colons <= 8 && (dbl || colons == 7 || strchr(s, '.'));
}

/* ---- the running stack ---- */

int rp_live_commands(const struct rp_conf *c, void (*fn)(const char *line, void *arg), void *arg)
{
	const struct rp_model *o = &c->orig, *n = &c->cur;
	char buf[LINEMAX];
	int i, restart = 0;

	if (str_changed(o->hostname, n->hostname) && n->hostname[0]) {
		rt_snprintf(buf, sizeof(buf), "hostname %s", n->hostname);
		fn(buf, arg);
	}
	if (str_changed(o->domain, n->domain) && n->domain[0]) {
		rt_snprintf(buf, sizeof(buf), "domain %s", n->domain);
		fn(buf, arg);
	}
	if (dns_changed(o, n)) {
		fn("nameserver flush", arg);
		for (i = 0; i < RP_NDNS; i++)
			if (n->dns[i][0]) {
				rt_snprintf(buf, sizeof(buf), "nameserver add %s", n->dns[i]);
				fn(buf, arg);
			}
	}
	if (o->ntp != n->ntp || str_changed(o->ntp_servers, n->ntp_servers)) {
		if (n->ntp && n->ntp_servers[0])
			rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_NTP, n->ntp_servers);
		else
			rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_NTP, n->ntp ? RP_KW_NTP_ON : RP_KW_NTP_OFF);
		fn(buf, arg);
	}
	if (str_changed(o->timezone, n->timezone) && n->timezone[0]) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_TIMEZONE, n->timezone);
		fn(buf, arg);
	}
	if (o->mdns != n->mdns && n->mdns >= 0) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_MDNS, n->mdns ? "on" : "off");
		fn(buf, arg);
	}
#define SYSCTL(f, name) \
	if (o->f != n->f && n->f != RP_UNSET) { \
		rt_snprintf(buf, sizeof(buf), "sysctl %s=%ld", name, n->f); \
		fn(buf, arg); \
	}
	SYSCTL(quickack, "net.inet.tcp.quickack")
	SYSCTL(delack_segs, "net.inet.tcp.delack_segs")
	SYSCTL(delack_ms, "net.inet.tcp.delack_ms")
#undef SYSCTL
	for (i = 0; i < RP_NSC; i++)
		if (o->sc[i] != n->sc[i]) {
			if (n->sc[i] == RP_UNSET) {
				restart = 1;            /* back to the built-in value */
				continue;
			}
			if (i == RP_SC_CC)
				rt_snprintf(buf, sizeof(buf), "sysctl %s=%s", rp_sc_names[i], rp_cc_names[n->sc[i]]);
			else
				rt_snprintf(buf, sizeof(buf), "sysctl %s=%ld", rp_sc_names[i], n->sc[i]);
			fn(buf, arg);
		}
	for (i = 0; i < RP_NSS; i++)
		if (str_changed(o->ss[i], n->ss[i])) {
			/* an empty list clears the rules at once */
			rt_snprintf(buf, sizeof(buf), "sysctl %s=%s", rp_ss_names[i], n->ss[i]);
			fn(buf, arg);
		}
	if (o->qos != n->qos && n->qos >= 0) {
		rt_snprintf(buf, sizeof(buf), "%s %s", RP_KW_QOS, n->qos ? "on" : "off");
		fn(buf, arg);
	}
	if (list_changed(o, n, RP_LIST_QRULE)) {
		fn("qos rules flush", arg);
		for (i = 0; i < RP_NQRULE && n->qrule[i][0]; i++) {
			rt_snprintf(buf, sizeof(buf), "%s rule %s", RP_KW_QOS, n->qrule[i]);
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_ALIAS) && !str_changed(o->ifname, n->ifname)) {
		int j, k;
		/* removed addresses first, then the new ones */
		for (i = 0; i < RP_NALIAS && o->alias[i][0]; i++) {
			for (j = 0; j < RP_NALIAS && str_changed(o->alias[i], n->alias[j]); j++)
				;
			if (j < RP_NALIAS)
				continue;
			for (k = 0; o->alias[i][k] && o->alias[i][k] != '/' && o->alias[i][k] != ' '; k++)
				;
			rt_snprintf(buf, sizeof(buf), "ifconfig %s %.*s -alias", n->ifname, k, o->alias[i]);
			fn(buf, arg);
		}
		for (i = 0; i < RP_NALIAS && n->alias[i][0]; i++) {
			for (j = 0; j < RP_NALIAS && str_changed(n->alias[i], o->alias[j]); j++)
				;
			if (j < RP_NALIAS)
				continue;
			rt_snprintf(buf, sizeof(buf), "ifconfig %s alias %s", n->ifname, n->alias[i]);
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_VLAN) && !str_changed(o->ifname, n->ifname)) {
		int j;
		/* removed (or changed) VLANs go, then the new ones are made */
		for (i = 0; i < RP_NVLAN && o->vlan[i][0]; i++) {
			for (j = 0; j < RP_NVLAN && str_changed(o->vlan[i], n->vlan[j]); j++)
				;
			if (j < RP_NVLAN)
				continue;
			rt_snprintf(buf, sizeof(buf), "interface %s.%lu delete", n->ifname,
			            rt_strtoul(o->vlan[i], NULL, 10));
			fn(buf, arg);
		}
		for (i = 0; i < RP_NVLAN && n->vlan[i][0]; i++) {
			for (j = 0; j < RP_NVLAN && str_changed(n->vlan[i], o->vlan[j]); j++)
				;
			if (j < RP_NVLAN)
				continue;
			vlan_line(n->ifname, n->vlan[i], buf, sizeof(buf));
			fn(buf, arg);
		}
	}
	if (iface_changed(o, n) || addr_changed(o, n) || o->ipv6 != n->ipv6 || o->ipv4 != n->ipv4 ||
	    ((list_changed(o, n, RP_LIST_ALIAS) || list_changed(o, n, RP_LIST_VLAN)) &&
	     str_changed(o->ifname, n->ifname)))
		restart = 1;
	return restart;
}
