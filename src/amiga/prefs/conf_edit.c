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
	"net.inet.ip.qos_prio_dscp", "dns.edns_size", "dns.tcp_fallback", "net.inet.tcp.mtu_probing",
	"dns.cache.neg_maxttl", "net.inet.tcp.ecn", "net.inet6.ip6.accept_rtadv", "net.inet6.ip6.use_tempaddr",
	"net.inet6.ip6.addr_gen_mode", "net.inet6.ip6.dad_count", "net.inet6.ip6.gratuitous_na"
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

/* a card's settings before its lines are read */
static void iface_defaults(struct rp_iface *f, const char *name, const char *device)
{
	memset(f, 0, sizeof(*f));
	copy(f->ifname, name, sizeof(f->ifname));
	copy(f->device, device, sizeof(f->device));
	f->unit = 0;
	f->mtu = f->reads = f->tcpwindow = f->metric = f->dhcp_wait = RP_UNSET;
	f->dhcp = 1;
	f->linklocal = 1;
	f->ipv6 = -1;
}

/* the card or VLAN of a card named by name: its card's index (0 for a
 * VLAN of the first card), -1 when neither */
static int iface_or_vlan(const struct rp_model *m, const char *name)
{
	const char *dot = strchr(name, '.');
	char par[RP_IFNAME];
	int fk = rp_iface_find(m, name);

	if (fk >= 0)
		return fk;
	if (!dot || (size_t)(dot - name) >= RP_IFNAME || strlen(name) >= RP_IFNAMELEN || strchr(name, RP_IFSEP))
		return -1;
	memcpy(par, name, (size_t)(dot - name));
	par[dot - name] = 0;
	return rp_iface_find(m, par);
}

/* an entry of a list on card fk (or a VLAN of it, name): the "IF|" prefix
 * it needs (none on the first card) and the body, into e */
static void prefixed(const struct rp_model *m, int fk, const char *name, const char *body, char *e, size_t size)
{
	if (fk > 0 || strcmp(name, m->ifc[0].ifname))
		rt_snprintf(e, size, "%s%c%s", name, RP_IFSEP, body);
	else
		rt_snprintf(e, size, "%s", body);
}

/* the first free slot of a list gets entry e (n slots of len bytes) and
 * line l; 1 when it was taken */
static int list_take(struct rp_line **slot, char *base, int n, size_t len, struct rp_line *l, const char *e)
{
	int i;

	if (strlen(e) >= len)
		return 0;
	for (i = 0; i < n; i++)
		if (!slot[i]) {
			slot[i] = l;
			copy(base + (size_t)i * len, e, len);
			return 1;
		}
	return 0;
}

/* "route add ..." that is not a card's default route: its entry "DEST
 * [via GW] [dev IF] [metric N] [TYPE]" into e, or 0 when the line says
 * something the editor does not keep (mtu, src ...) */
static int route_entry(const struct toks *tk, char *e, size_t size)
{
	const char *dest = NULL, *gw = NULL, *dev = NULL, *metric = NULL, *type = NULL, *mask = NULL;
	int i, host = 0;

	for (i = 2; i < tk->n; i++) {
		const char *w = tk->t[i];
		if (eq(w, "-inet6") || eq(w, "-6") || eq(w, "-net"))
			continue;
		if (eq(w, "-host")) {
			host = 1;
			continue;
		}
		if (!dest) {
			dest = eq(w, "0/0") ? "default" : w;
			continue;
		}
		if ((eq(w, "via") || eq(w, "gw") || eq(w, "gateway")) && !gw && i + 1 < tk->n)
			gw = tk->t[++i];
		else if (eq(w, "dev") && !dev && i + 1 < tk->n)
			dev = tk->t[++i];
		else if ((eq(w, "metric") || eq(w, "preference") || eq(w, "priority")) && !metric && i + 1 < tk->n)
			metric = tk->t[++i];
		else if ((eq(w, "netmask") || eq(w, "genmask")) && !mask && i + 1 < tk->n)
			mask = tk->t[++i];
		else if ((eq(w, "reject") || eq(w, "prohibit") || eq(w, "unreachable") || eq(w, "blackhole")) && !type)
			type = w;
		else if (!gw && rp_valid_ip(w))
			gw = w;
		else
			return 0;
	}
	if (!dest)
		return 0;
	if (mask) {
		/* A.B.C.D netmask M: as A.B.C.D/BITS */
		unsigned long v = 0, b = 0;
		const char *q = mask;
		int k;
		if (!rp_valid_ip4(mask, 0) || strchr(dest, '/') || !rp_valid_ip4(dest, 0))
			return 0;
		for (k = 0; k < 4; k++) {
			v = (v << 8) | rt_strtoul(q, &q, 10);
			if (*q == '.')
				q++;
		}
		while (b < 32 && (v & (0x80000000UL >> b)))
			b++;
		if (v != (b ? (0xffffffffUL << (32 - b)) & 0xffffffffUL : 0))
			return 0;
		rt_snprintf(e, size, "%s/%lu", dest, b);
	} else if (host && !strchr(dest, '/') && !eq(dest, "default")) {
		rt_snprintf(e, size, "%s/%s", dest, strchr(dest, ':') ? "128" : "32");
	} else {
		rt_snprintf(e, size, "%s", dest);
	}
	if (gw)
		rt_snprintf(e + strlen(e), size - strlen(e), " via %s", gw);
	if (dev)
		rt_snprintf(e + strlen(e), size - strlen(e), " dev %s", dev);
	if (metric)
		rt_snprintf(e + strlen(e), size - strlen(e), " metric %s", metric);
	if (type)
		rt_snprintf(e + strlen(e), size - strlen(e), " %s", type);
	return strlen(e) + 1 < size && rp_valid_route(e);
}

static void model_defaults(struct rp_model *m)
{
	int i;

	memset(m, 0, sizeof(*m));
	iface_defaults(&m->ifc[0], "eth0", "uaenet.device");
	m->ipv6 = 1;
	m->mdns = -1;
	m->quickack = m->delack_segs = m->delack_ms = RP_UNSET;
	m->ipv4 = 1;
	m->qos = -1;
	for (i = 0; i < RP_NSC; i++)
		m->sc[i] = RP_UNSET;
}

int rp_iface_find(const struct rp_model *m, const char *name)
{
	int k;

	for (k = 0; k < RP_NIF; k++)
		if (m->ifc[k].ifname[0] && eq(m->ifc[k].ifname, name))
			return k;
	return -1;
}

/* is l the interface line of a card? */
static int is_iface_line(const struct rp_conf *c, const struct rp_line *l)
{
	int k;

	for (k = 0; k < RP_NIF; k++)
		if (c->l_iface[k] == l)
			return 1;
	return 0;
}

/* "interface NAME device=D ...": the next card (up to RP_NIF; a name used
 * twice or a VLAN line is not one) */
static void parse_interface(struct rp_conf *c, struct rp_line *l, struct toks *tk)
{
	struct rp_model *m = &c->orig;
	struct rp_iface *f;
	const char *v;
	int i, k, have_dev = 0;

	for (i = 2; i < tk->n; i++)
		if (kv(tk->t[i], "device"))
			have_dev = 1;
	if (!have_dev || strlen(tk->t[1]) >= RP_IFNAME || strchr(tk->t[1], RP_IFSEP))
		return;
	for (k = 0; k < RP_NIF && c->l_iface[k]; k++)
		if (eq(m->ifc[k].ifname, tk->t[1]))
			return;
	if (k == RP_NIF)
		return;
	c->l_iface[k] = l;
	f = &m->ifc[k];
	iface_defaults(f, tk->t[1], "");
	for (i = 2; i < tk->n; i++) {
		if ((v = kv(tk->t[i], "device")) != NULL)
			copy(f->device, v, sizeof(f->device));
		else if ((v = kv(tk->t[i], "unit")) != NULL)
			f->unit = num(v);
		else if ((v = kv(tk->t[i], "mtu")) != NULL)
			f->mtu = num(v);
		else if ((v = kv(tk->t[i], "reads")) != NULL)
			f->reads = num(v);
		else if ((v = kv(tk->t[i], "tcpwindow")) != NULL)
			f->tcpwindow = num(v);
		else if (eq(tk->t[i], "raw"))
			f->raw = 1;
		else {
			size_t o = strlen(f->ifextra);
			rt_snprintf(f->ifextra + o, sizeof(f->ifextra) - o, "%s%s", o ? " " : "", tk->raw[i]);
		}
	}
	if (f->unit == RP_UNSET)
		f->unit = 0;
}

static void parse_line(struct rp_conf *c, struct rp_line *l, struct toks *tk)
{
	struct rp_model *m = &c->orig;
	const char *k = tk->t[0];
	int i;

	if (eq(k, "interface") && tk->n >= 3 && !is_iface_line(c, l)) {
		/* interface IF.ID vlan=ID parent=IF [dhcp|address=A/BITS|up] [pcp=N]
		 * [mtu=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off] [tcpwindow=N] */
		static const char *const keys[RP_VLAN_NKEYS] = { "pcp", "mtu", "ipv4", "ipv6", "qos", "tcpwindow" };
		const char *val[RP_VLAN_NKEYS];
		char e[RP_VLANLEN], name[RP_IFNAMELEN];
		const char *vid = NULL, *par = NULL, *mode = "", *v;
		int ok = 1, j, pk;
		size_t n;
		memset(val, 0, sizeof(val));
		for (i = 2; i < tk->n && ok; i++) {
			if ((v = kv(tk->t[i], "vlan")) != NULL)
				vid = v;
			else if ((v = kv(tk->t[i], "parent")) != NULL)
				par = v;
			else if ((v = kv(tk->t[i], "address")) != NULL)
				mode = v;
			else if (eq(tk->t[i], "dhcp") || eq(tk->t[i], "up"))
				mode = tk->t[i];
			else {
				for (j = 0; j < RP_VLAN_NKEYS && !(v = kv(tk->t[i], keys[j])); j++)
					;
				if (j < RP_VLAN_NKEYS)
					val[j] = v;
				else
					ok = 0;
			}
		}
		if (!ok || !vid || !par || (pk = rp_iface_find(m, par)) < 0)
			return;
		rt_snprintf(name, sizeof(name), "%s.%s", m->ifc[pk].ifname, vid);
		if (!eq(tk->t[1], name))
			return;
		/* a VLAN of a later card: "PARENT|" first */
		n = 0;
		if (pk > 0)
			n = (size_t)rt_snprintf(e, sizeof(e), "%s%c", m->ifc[pk].ifname, RP_IFSEP);
		n += (size_t)rt_snprintf(e + n, sizeof(e) - n, "%s%s%s", vid, mode[0] ? " " : "", mode);
		for (j = 0; j < RP_VLAN_NKEYS && n < sizeof(e); j++)
			if (val[j])
				n += (size_t)rt_snprintf(e + n, sizeof(e) - n, " %s=%s", keys[j], val[j]);
		if (n + 1 < sizeof(e) && rp_valid_vlan(rp_entry_body(e)))
			for (i = 0; i < RP_NVLAN; i++)
				if (!c->l_vlan[i]) {
					c->l_vlan[i] = l;
					copy(m->vlan[i], e, sizeof(m->vlan[i]));
					break;
				}
	} else if (eq(k, "dhcp")) {
		/* dhcp IF [start] [linklocal on|off] [wait S] */
		int fk = tk->n >= 2 ? rp_iface_find(m, tk->t[1]) : -1, ll = 1, ok = fk >= 0 && !c->l_dhcp[fk];
		long wait = RP_UNSET;
		i = tk->n >= 3 && eq(tk->t[2], "start") ? 3 : 2;
		for (; ok && i < tk->n; i += 2) {
			if (i + 1 >= tk->n)
				ok = 0;
			else if (eq(tk->t[i], "linklocal") && (eq(tk->t[i + 1], "on") || eq(tk->t[i + 1], "off")))
				ll = eq(tk->t[i + 1], "on");
			else if (eq(tk->t[i], "wait") && num(tk->t[i + 1]) >= 1 && num(tk->t[i + 1]) <= 3600)
				wait = num(tk->t[i + 1]);
			else
				ok = 0;
		}
		if (ok) {
			c->l_dhcp[fk] = l;
			m->ifc[fk].dhcp = 1;
			m->ifc[fk].linklocal = ll;
			m->ifc[fk].dhcp_wait = wait;
		}
	} else if (eq(k, "ifconfig") && tk->n >= 4 && eq(tk->t[2], "inet6")) {
		/* ifconfig IF inet6 ADDR[/PLEN] [prefixlen N] [nodad] [alias|add] [up] */
		char body[RP_ADDR6LEN], addr[RP_ADDR], e[RP_ADDR6LEN + RP_IFNAMELEN];
		const char *plen = NULL;
		int fk = iface_or_vlan(m, tk->t[1]), nodad = 0;
		addr[0] = 0;
		for (i = 3; fk >= 0 && i < tk->n; i++) {
			const char *w = tk->t[i];
			if (eq(w, "nodad") || eq(w, "-dad"))
				nodad = 1;
			else if (eq(w, "alias") || eq(w, "add") || eq(w, "up"))
				;
			else if (eq(w, "prefixlen") && i + 1 < tk->n && !plen && num(tk->t[i + 1]) >= 0 &&
			         num(tk->t[i + 1]) <= 128)
				plen = tk->t[++i];
			else if (!addr[0] && rp_valid_ip6(w, 1) && !strchr(w, '%'))
				copy(addr, w, sizeof(addr));
			else
				fk = -1;
		}
		if (fk < 0 || !addr[0])
			return;
		rt_snprintf(body, sizeof(body), "%s%s%s%s", addr, strchr(addr, '/') ? "" : "/",
		            strchr(addr, '/') ? "" : plen ? plen : "64", nodad ? " nodad" : "");
		if (!rp_valid_addr6(body))
			return;
		prefixed(m, fk, tk->t[1], body, e, sizeof(e));
		list_take(c->l_addr6, m->addr6[0], RP_NADDR6, RP_ADDR6LEN, l, e);
	} else if (eq(k, "ifconfig") && tk->n >= 4 && (eq(tk->t[2], "alias") || eq(tk->t[2], "add"))) {
		/* ifconfig IF alias A.B.C.D[/BITS] [netmask M] [broadcast B], IF
		 * a card or a VLAN of one (PARENT.ID) */
		char spec[RP_ALIASLEN + 8], e[RP_ALIASLEN + 8];
		const char *dot = strchr(tk->t[1], '.');
		int fk = rp_iface_find(m, tk->t[1]);
		size_t n = 0;
		if (fk < 0 && dot && (size_t)(dot - tk->t[1]) < RP_IFNAME) {
			char par[RP_IFNAME];
			memcpy(par, tk->t[1], (size_t)(dot - tk->t[1]));
			par[dot - tk->t[1]] = 0;
			if (rp_iface_find(m, par) < 0 || strlen(tk->t[1]) >= RP_IFNAMELEN || strchr(tk->t[1], RP_IFSEP))
				return;
		} else if (fk < 0) {
			return;
		}
		join_raw(tk, 3, spec, sizeof(spec));
		if (fk != 0)
			n = (size_t)rt_snprintf(e, sizeof(e), "%s%c", fk > 0 ? m->ifc[fk].ifname : tk->t[1], RP_IFSEP);
		rt_snprintf(e + n, sizeof(e) - n, "%s", spec);
		if (strlen(e) < RP_ALIASLEN && rp_valid_alias(spec))
			for (i = 0; i < RP_NALIAS; i++)
				if (!c->l_alias[i]) {
					c->l_alias[i] = l;
					copy(m->alias[i], e, sizeof(m->alias[i]));
					break;
				}
	} else if (eq(k, "ifconfig")) {
		/* ifconfig IF A.B.C.D[/BITS] [netmask M] [up] */
		const char *addr = NULL, *mask = NULL;
		int fk = tk->n >= 3 ? rp_iface_find(m, tk->t[1]) : -1;
		int ok = fk >= 0 && !c->l_ifconfig[fk];
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
			c->l_ifconfig[fk] = l;
			copy(m->ifc[fk].addr, addr, sizeof(m->ifc[fk].addr));
			copy(m->ifc[fk].netmask, mask ? mask : "", sizeof(m->ifc[fk].netmask));
			if (!c->l_dhcp[fk])
				m->ifc[fk].dhcp = 0;
		}
	} else if (eq(k, "route")) {
		/* route add default [via|gw] GW [dev IF] [metric N]: the first
		 * card's without dev, else the card named */
		if (tk->n >= 4 && eq(tk->t[1], "add") && eq(tk->t[2], "default")) {
			const char *gw, *dev = NULL;
			long metric = RP_UNSET;
			int fk = 0, ok = 1;
			i = 3;
			if (eq(tk->t[3], "via") || eq(tk->t[3], "gw"))
				i = 4;
			gw = i < tk->n ? tk->t[i] : "";
			for (i++; ok && i < tk->n; i += 2) {
				if (i + 1 >= tk->n)
					ok = 0;
				else if (eq(tk->t[i], "dev") && !dev)
					dev = tk->t[i + 1];
				else if (eq(tk->t[i], "metric") && metric == RP_UNSET && num(tk->t[i + 1]) != RP_UNSET)
					metric = num(tk->t[i + 1]);
				else
					ok = 0;
			}
			if (dev)
				fk = rp_iface_find(m, dev);
			if (ok && fk >= 0 && !c->l_route[fk] && rp_valid_ip4(gw, 0) && (fk == 0 || dev)) {
				c->l_route[fk] = l;
				copy(m->ifc[fk].gateway, gw, sizeof(m->ifc[fk].gateway));
				m->ifc[fk].metric = metric;
				return;
			}
		}
		/* any other static route */
		if (tk->n >= 3 && (eq(tk->t[1], "add") || eq(tk->t[1], "append"))) {
			char e[RP_ROUTELEN];
			if (route_entry(tk, e, sizeof(e)))
				list_take(c->l_sroute, m->route[0], RP_NROUTE, RP_ROUTELEN, l, e);
		}
	} else if (eq(k, "host") && tk->n == 3) {
		char e[RP_HOSTLEN];
		rt_snprintf(e, sizeof(e), "%s %s", tk->t[1], tk->t[2]);
		if (rp_valid_host(e))
			list_take(c->l_host, m->host[0], RP_NHOST, RP_HOSTLEN, l, e);
	} else if (eq(k, "arp") && tk->n >= 4 && eq(tk->t[1], "add")) {
		/* arp add ADDR [lladdr] MAC [dev IF] */
		char e[RP_NEIGHLEN];
		i = eq(tk->t[3], "lladdr") ? 4 : 3;
		if (i < tk->n && (tk->n == i + 1 || (tk->n == i + 3 && eq(tk->t[i + 1], "dev")))) {
			rt_snprintf(e, sizeof(e), "%s %s%s%s", tk->t[2], tk->t[i], tk->n > i + 1 ? " dev " : "",
			            tk->n > i + 1 ? tk->t[i + 2] : "");
			if (rp_valid_neigh(e))
				list_take(c->l_neigh, m->neigh[0], RP_NNEIGH, RP_NEIGHLEN, l, e);
		}
	} else if (eq(k, RP_KW_IPV6) && tk->n == 3 && (eq(tk->t[2], "on") || eq(tk->t[2], "off"))) {
		int fk = rp_iface_find(m, tk->t[1]);
		if (fk >= 0 && !c->l_ipv6if[fk]) {
			c->l_ipv6if[fk] = l;
			m->ifc[fk].ipv6 = eq(tk->t[2], "on");
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
	memcpy(c->h_sroute, c->orig.route, sizeof(c->h_sroute));
	memcpy(c->h_host, c->orig.host, sizeof(c->h_host));
	memcpy(c->h_neigh, c->orig.neigh, sizeof(c->h_neigh));
	memcpy(c->h_addr6, c->orig.addr6, sizeof(c->h_addr6));
	copy(c->h_ifname, c->orig.ifc[0].ifname, sizeof(c->h_ifname));
	copy(c->h_vifname, c->orig.ifc[0].ifname, sizeof(c->h_vifname));
	copy(c->h_a6ifname, c->orig.ifc[0].ifname, sizeof(c->h_a6ifname));
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

static int iface_changed(const struct rp_iface *o, const struct rp_iface *n)
{
	return str_changed(o->ifname, n->ifname) || str_changed(o->device, n->device) || o->unit != n->unit ||
	       o->mtu != n->mtu || o->reads != n->reads || o->tcpwindow != n->tcpwindow || o->raw != n->raw ||
	       str_changed(o->ifextra, n->ifextra);
}

static int addr_changed(const struct rp_iface *o, const struct rp_iface *n)
{
	return o->dhcp != n->dhcp || str_changed(o->ifname, n->ifname) ||
	       (n->dhcp && (o->linklocal != n->linklocal || o->dhcp_wait != n->dhcp_wait)) ||
	       (!n->dhcp && (str_changed(o->addr, n->addr) || str_changed(o->netmask, n->netmask) ||
	                     str_changed(o->gateway, n->gateway) || o->metric != n->metric));
}

/* any card or its address changed, added or removed */
static int ifaces_changed(const struct rp_model *o, const struct rp_model *n)
{
	int k;

	for (k = 0; k < RP_NIF; k++)
		if (str_changed(o->ifc[k].ifname, n->ifc[k].ifname) ||
		    (n->ifc[k].ifname[0] && (iface_changed(&o->ifc[k], &n->ifc[k]) ||
		                             addr_changed(&o->ifc[k], &n->ifc[k]))))
			return 1;
	return 0;
}

/* a list of a model: its entries (n of len bytes each) */
static char *list_base(struct rp_model *m, int which, int *n, size_t *len)
{
	switch (which) {
	case RP_LIST_ALIAS: *n = RP_NALIAS; *len = RP_ALIASLEN; return m->alias[0];
	case RP_LIST_VLAN:  *n = RP_NVLAN;  *len = RP_VLANLEN;  return m->vlan[0];
	case RP_LIST_ROUTE: *n = RP_NROUTE; *len = RP_ROUTELEN; return m->route[0];
	case RP_LIST_HOST:  *n = RP_NHOST;  *len = RP_HOSTLEN;  return m->host[0];
	case RP_LIST_NEIGH: *n = RP_NNEIGH; *len = RP_NEIGHLEN; return m->neigh[0];
	case RP_LIST_ADDR6: *n = RP_NADDR6; *len = RP_ADDR6LEN; return m->addr6[0];
	}
	*n = RP_NQRULE;
	*len = RP_LONG;
	return m->qrule[0];
}

static int list_changed(const struct rp_model *o, const struct rp_model *n, int which)
{
	int i, cnt;
	size_t len;
	const char *a = list_base((struct rp_model *)o, which, &cnt, &len);
	const char *b = list_base((struct rp_model *)n, which, &cnt, &len);

	for (i = 0; i < cnt; i++)
		if (str_changed(a + (size_t)i * len, b + (size_t)i * len))
			return 1;
	return 0;
}

/* is entry text in list which of model m? */
static int list_has(const struct rp_model *m, int which, const char *text)
{
	int i, cnt;
	size_t len;
	const char *a = list_base((struct rp_model *)m, which, &cnt, &len);

	for (i = 0; i < cnt && a[(size_t)i * len]; i++)
		if (!strcmp(a + (size_t)i * len, text))
			return 1;
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

/* "interface IF.ID vlan=ID parent=IF [dhcp|address=A/BITS|up] [pcp=N] ..."
 * for the VLAN entry "[PARENT|]ID [MODE] [pcp=N] [mtu=N] ..." (IF: the
 * parent named, else ifname) */
static void vlan_line(const char *first, const char *e, char *buf, size_t size)
{
	char w[RP_VLAN_NKEYS + 2][RP_VLANLEN], ifname[RP_IFNAME];
	const char *sep = strchr(e, RP_IFSEP);
	int n = 0, k;
	size_t i;

	if (sep && (size_t)(sep - e) < sizeof(ifname)) {
		memcpy(ifname, e, (size_t)(sep - e));
		ifname[sep - e] = 0;
		e = sep + 1;
	} else {
		copy(ifname, first, sizeof(ifname));
	}
	memset(w, 0, sizeof(w));
	while (*e && n < RP_VLAN_NKEYS + 2) {
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
		if (strchr(w[i], '=') || eq(w[i], "dhcp") || eq(w[i], "up"))
			k += rt_snprintf(buf + k, size - (size_t)k, " %s", w[i]);
		else
			k += rt_snprintf(buf + k, size - (size_t)k, " address=%s", w[i]);
	}
}

static void list_line(const struct rp_conf *c, int which, const char *e, char *buf, size_t size)
{
	char ifn[RP_IFNAMELEN];

	switch (which) {
	case RP_LIST_ALIAS:
		rp_entry_if(&c->cur, e, ifn, sizeof(ifn));
		rt_snprintf(buf, size, "ifconfig %s alias %s", ifn, rp_entry_body(e));
		break;
	case RP_LIST_VLAN:
		vlan_line(c->cur.ifc[0].ifname, e, buf, size);
		break;
	case RP_LIST_ROUTE:
		rt_snprintf(buf, size, "route add %s", e);
		break;
	case RP_LIST_HOST:
		rt_snprintf(buf, size, "host %s", e);
		break;
	case RP_LIST_NEIGH:
		rt_snprintf(buf, size, "arp add %s", e);
		break;
	case RP_LIST_ADDR6:
		rp_entry_if(&c->cur, e, ifn, sizeof(ifn));
		rt_snprintf(buf, size, "ifconfig %s inet6 %s", ifn, rp_entry_body(e));
		break;
	default:
		rt_snprintf(buf, size, "%s rule %s", RP_KW_QOS, e);
	}
}

/* Bring a list's lines in line with want[]: have[] is what the lines say
 * now (slot i: have[i]). Lines of entries that stay keep their place and
 * comment, a replaced entry is rewritten in place, deleted ones go, new
 * ones follow the entry before them (the first one 'anchor'). */
static int sync_list(struct rp_conf *c, int which, struct rp_line **slot, char *have, const char *want, int n,
                     size_t len, struct rp_line *anchor)
{
	struct rp_line *ns[RP_LIST_MAXN], *prev = anchor, *l;
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
			if (strcmp(h, w) || (which == RP_LIST_ALIAS && strcmp(c->h_ifname, c->cur.ifc[0].ifname)) ||
			    (which == RP_LIST_VLAN && strcmp(c->h_vifname, c->cur.ifc[0].ifname)) ||
			    (which == RP_LIST_ADDR6 && strcmp(c->h_a6ifname, c->cur.ifc[0].ifname))) {
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
		copy(c->h_ifname, c->cur.ifc[0].ifname, sizeof(c->h_ifname));
	if (which == RP_LIST_VLAN)
		copy(c->h_vifname, c->cur.ifc[0].ifname, sizeof(c->h_vifname));
	if (which == RP_LIST_ADDR6)
		copy(c->h_a6ifname, c->cur.ifc[0].ifname, sizeof(c->h_a6ifname));
	return r;
}

/* the last line (in the file) of cards 0..upto-1: their interface,
 * dhcp, ifconfig and route lines; NULL if none */
static struct rp_line *iface_last(struct rp_conf *c, int upto)
{
	struct rp_line *l;
	int k;

	for (l = c->tail; l; l = l->prev)
		for (k = 0; k < upto; k++)
			if (l == c->l_iface[k] || l == c->l_dhcp[k] || l == c->l_ifconfig[k] || l == c->l_route[k] ||
			    l == c->l_ipv6if[k])
				return l;
	return NULL;
}

/* bring the lines of card k in line with c->cur: a new card's lines follow
 * the cards before it, a removed card's lines go */
static int sync_iface(struct rp_conf *c, int k)
{
	const struct rp_iface *o = &c->orig.ifc[k], *n = &c->cur.ifc[k];
	char buf[LINEMAX], q[RP_STR + 4];
	struct rp_line *anchor;
	int r = 0, fresh = !o->ifname[0];

	if (!n->ifname[0]) {
		r |= put(c, &c->l_ipv6if[k], NULL, NULL);
		r |= put(c, &c->l_route[k], NULL, NULL);
		r |= put(c, &c->l_ifconfig[k], NULL, NULL);
		r |= put(c, &c->l_dhcp[k], NULL, NULL);
		r |= put(c, &c->l_iface[k], NULL, NULL);
		return r;
	}
	if (fresh || iface_changed(o, n)) {
		int x = rt_snprintf(buf, sizeof(buf), "interface %s device=%s unit=%ld", n->ifname,
		                    quoted(n->device, q, sizeof(q)), n->unit < 0 ? 0L : n->unit);
		if (n->mtu > 0)
			x += rt_snprintf(buf + x, sizeof(buf) - (size_t)x, " mtu=%ld", n->mtu);
		if (n->reads > 0)
			x += rt_snprintf(buf + x, sizeof(buf) - (size_t)x, " reads=%ld", n->reads);
		if (n->tcpwindow > 0)
			x += rt_snprintf(buf + x, sizeof(buf) - (size_t)x, " tcpwindow=%ld", n->tcpwindow);
		if (n->raw)
			x += rt_snprintf(buf + x, sizeof(buf) - (size_t)x, " raw");
		if (n->ifextra[0])
			rt_snprintf(buf + x, sizeof(buf) - (size_t)x, " %s", n->ifextra);
		anchor = k == 0 ? first_of(c->l_domain, first_of(c->l_hostname, top_anchor(c))) : iface_last(c, k);
		r |= put(c, &c->l_iface[k], anchor, buf);
	}
	if (fresh || addr_changed(o, n)) {
		anchor = c->l_iface[k];
		if (n->dhcp) {
			if (c->l_ifconfig[k] && !c->l_dhcp[k]) {
				anchor = c->l_ifconfig[k]->prev;
				if (anchor && anchor == c->l_route[k])
					anchor = anchor->prev;
			}
			r |= put(c, &c->l_ifconfig[k], NULL, NULL);
			r |= put(c, &c->l_route[k], NULL, NULL);
			rt_snprintf(buf, sizeof(buf), "dhcp %s%s", n->ifname, n->linklocal ? "" : " linklocal off");
			if (n->dhcp_wait != RP_UNSET)
				rt_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " wait %ld", n->dhcp_wait);
			r |= put(c, &c->l_dhcp[k], anchor, buf);
		} else {
			if (c->l_dhcp[k] && !c->l_ifconfig[k])
				anchor = c->l_dhcp[k]->prev;
			r |= put(c, &c->l_dhcp[k], NULL, NULL);
			if (n->netmask[0])
				rt_snprintf(buf, sizeof(buf), "ifconfig %s %s netmask %s up", n->ifname, n->addr, n->netmask);
			else
				rt_snprintf(buf, sizeof(buf), "ifconfig %s %s up", n->ifname, n->addr);
			r |= put(c, &c->l_ifconfig[k], anchor, n->addr[0] ? buf : NULL);
			if (k == 0 && n->metric == RP_UNSET)
				rt_snprintf(buf, sizeof(buf), "route add default %s", n->gateway);
			else
				rt_snprintf(buf, sizeof(buf), "route add default %s dev %s metric %ld", n->gateway, n->ifname,
				            n->metric != RP_UNSET ? n->metric : 10L * k);
			r |= put(c, &c->l_route[k], first_of(c->l_ifconfig[k], anchor), n->gateway[0] ? buf : NULL);
		}
	}
	/* "ipv6 IF on|off" after the card's lines */
	if (fresh || o->ipv6 != n->ipv6 || str_changed(o->ifname, n->ifname)) {
		rt_snprintf(buf, sizeof(buf), "%s %s %s", RP_KW_IPV6, n->ifname, n->ipv6 ? "on" : "off");
		r |= put(c, &c->l_ipv6if[k], iface_last(c, k + 1), n->ipv6 < 0 ? NULL : buf);
	}
	return r;
}

/* bring the lines in line with c->cur */
static int sync_lines(struct rp_conf *c)
{
	const struct rp_model *o = &c->orig, *n = &c->cur;
	char buf[LINEMAX];
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
	for (i = 0; i < RP_NIF; i++)
		r |= sync_iface(c, i);
	if (dns_changed(o, n)) {
		anchor = iface_last(c, RP_NIF);
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
	               iface_last(c, RP_NIF));
	r |= sync_list(c, RP_LIST_QRULE, c->l_qrule, c->h_qrule[0], n->qrule[0], RP_NQRULE, RP_LONG, c->l_qos);
	/* new VLAN lines after the interface's address lines */
	anchor = iface_last(c, RP_NIF);
	for (i = 0; i < RP_NALIAS; i++)
		if (c->l_alias[i])
			anchor = c->l_alias[i];
	r |= sync_list(c, RP_LIST_VLAN, c->l_vlan, c->h_vlan[0], n->vlan[0], RP_NVLAN, RP_VLANLEN, anchor);
	/* IPv6 addresses after the VLANs (an address may be on one), then the
	 * routes (through them) and the neighbours; the hosts after the name
	 * servers */
	for (i = 0; i < RP_NVLAN; i++)
		if (c->l_vlan[i])
			anchor = c->l_vlan[i];
	r |= sync_list(c, RP_LIST_ADDR6, c->l_addr6, c->h_addr6[0], n->addr6[0], RP_NADDR6, RP_ADDR6LEN, anchor);
	for (i = 0; i < RP_NADDR6; i++)
		if (c->l_addr6[i])
			anchor = c->l_addr6[i];
	r |= sync_list(c, RP_LIST_ROUTE, c->l_sroute, c->h_sroute[0], n->route[0], RP_NROUTE, RP_ROUTELEN, anchor);
	for (i = 0; i < RP_NROUTE; i++)
		if (c->l_sroute[i])
			anchor = c->l_sroute[i];
	r |= sync_list(c, RP_LIST_NEIGH, c->l_neigh, c->h_neigh[0], n->neigh[0], RP_NNEIGH, RP_NEIGHLEN, anchor);
	for (i = 0; i < RP_NNEIGH; i++)
		if (c->l_neigh[i])
			anchor = c->l_neigh[i];
	for (i = 0; i < RP_NDNS; i++)
		if (c->l_dns[i])
			anchor = c->l_dns[i];
	r |= sync_list(c, RP_LIST_HOST, c->l_host, c->h_host[0], n->host[0], RP_NHOST, RP_HOSTLEN, anchor);
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

	if (l == c->l_hostname || l == c->l_domain || l == c->l_ipv6 || l == c->l_ntp || l == c->l_tz ||
	    l == c->l_mdns || l == c->l_quickack || l == c->l_delack_segs || l == c->l_delack_ms)
		return 1;
	for (i = 0; i < RP_NIF; i++)
		if (l == c->l_iface[i] || l == c->l_dhcp[i] || l == c->l_ifconfig[i] || l == c->l_route[i] ||
		    l == c->l_ipv6if[i])
			return 1;
	for (i = 0; i < RP_NDNS; i++)
		if (l == c->l_dns[i])
			return 1;
	for (i = 0; i < RP_NROUTE; i++)
		if (l == c->l_sroute[i])
			return 1;
	for (i = 0; i < RP_NHOST; i++)
		if (l == c->l_host[i])
			return 1;
	for (i = 0; i < RP_NNEIGH; i++)
		if (l == c->l_neigh[i])
			return 1;
	for (i = 0; i < RP_NADDR6; i++)
		if (l == c->l_addr6[i])
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
	char *base = list_base(m, which, n, size);

	return i >= 0 && i < *n ? base + (size_t)i * *size : NULL;
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

/* ---- interfaces of the model ---- */

void rp_entry_if(const struct rp_model *m, const char *e, char *buf, size_t size)
{
	const char *sep = strchr(e, RP_IFSEP);

	if (sep && (size_t)(sep - e) < size) {
		memcpy(buf, e, (size_t)(sep - e));
		buf[sep - e] = 0;
	} else {
		copy(buf, m->ifc[0].ifname, size);
	}
}

const char *rp_entry_body(const char *e)
{
	const char *sep = strchr(e, RP_IFSEP);

	return sep ? sep + 1 : e;
}

/* does an entry's prefix name card 'name' (or, with vlans, a VLAN of it)? */
static int on_iface(const char *e, const char *name, int vlans)
{
	const char *sep = strchr(e, RP_IFSEP);
	size_t l = strlen(name);

	if (!sep || (size_t)(sep - e) < l || ncase(e, name, l))
		return 0;
	return e + l == sep || (vlans && e[l] == '.');
}

int rp_iface_add(struct rp_model *m)
{
	char name[RP_IFNAME];
	int k, x;

	for (k = 1; k < RP_NIF && m->ifc[k].ifname[0]; k++)
		;
	if (k == RP_NIF)
		return -1;
	for (x = 1;; x++) {
		rt_snprintf(name, sizeof(name), "eth%d", x);
		if (rp_iface_find(m, name) < 0)
			break;
	}
	iface_defaults(&m->ifc[k], name, "");
	return k;
}

void rp_entry_dev(const char *e, char *buf, size_t size)
{
	const char *p = e;
	size_t k = 0;

	buf[0] = 0;
	while (*p) {
		while (*p == ' ')
			p++;
		if (!strncmp(p, "dev ", 4) && (p == e || p[-1] == ' ')) {
			p += 4;
			while (*p && *p != ' ' && k < size - 1)
				buf[k++] = *p++;
			buf[k] = 0;
			return;
		}
		while (*p && *p != ' ')
			p++;
	}
}

/* does the "dev IF" of a route or neighbour entry name card 'name' or a
 * VLAN of it? */
static int dev_on_iface(const char *e, const char *name)
{
	char dev[RP_IFNAMELEN];
	size_t l = strlen(name);

	rp_entry_dev(e, dev, sizeof(dev));
	return dev[0] && !ncase(dev, name, l) && (dev[l] == 0 || dev[l] == '.');
}

int rp_iface_users(const struct rp_model *m, int k)
{
	int i, n = 0;

	if (k <= 0 || k >= RP_NIF || !m->ifc[k].ifname[0])
		return 0;
	for (i = 0; i < RP_NALIAS && m->alias[i][0]; i++)
		n += on_iface(m->alias[i], m->ifc[k].ifname, 1);
	for (i = 0; i < RP_NVLAN && m->vlan[i][0]; i++)
		n += on_iface(m->vlan[i], m->ifc[k].ifname, 0);
	for (i = 0; i < RP_NADDR6 && m->addr6[i][0]; i++)
		n += on_iface(m->addr6[i], m->ifc[k].ifname, 1);
	for (i = 0; i < RP_NROUTE && m->route[i][0]; i++)
		n += dev_on_iface(m->route[i], m->ifc[k].ifname);
	for (i = 0; i < RP_NNEIGH && m->neigh[i][0]; i++)
		n += dev_on_iface(m->neigh[i], m->ifc[k].ifname);
	return n;
}

/* drop the routes or neighbours through card 'name' or its VLANs */
static void dev_drop(char *base, int n, size_t len, const char *name)
{
	int i = 0, j;

	while (i < n && base[(size_t)i * len]) {
		if (!dev_on_iface(base + (size_t)i * len, name)) {
			i++;
			continue;
		}
		for (j = i; j + 1 < n; j++)
			memcpy(base + (size_t)j * len, base + (size_t)(j + 1) * len, len);
		memset(base + (size_t)(n - 1) * len, 0, len);
	}
}

/* the "dev OLD" (or "dev OLD.ID") of routes or neighbours say name */
static void dev_rename(char *base, int n, size_t len, const char *old, const char *name)
{
	char buf[RP_ROUTELEN + RP_IFNAMELEN];
	int i;

	for (i = 0; i < n && base[(size_t)i * len]; i++) {
		char *e = base + (size_t)i * len, *at;
		if (!dev_on_iface(e, old))
			continue;
		/* the name follows " dev " */
		for (at = e; *at && strncmp(at, " dev ", 5); at++)
			;
		if (!*at)
			continue;
		at += 5;
		rt_snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(at - e), e, name, at + strlen(old));
		if (strlen(buf) < len)
			copy(e, buf, len);
	}
}

/* drop the entries of a list on card 'name'; the rest move up */
static void list_drop(char *base, int n, size_t len, const char *name, int vlans)
{
	int i = 0, j;

	while (i < n && base[(size_t)i * len]) {
		if (!on_iface(base + (size_t)i * len, name, vlans)) {
			i++;
			continue;
		}
		for (j = i; j + 1 < n; j++)
			memcpy(base + (size_t)j * len, base + (size_t)(j + 1) * len, len);
		memset(base + (size_t)(n - 1) * len, 0, len);
	}
}

void rp_iface_delete(struct rp_model *m, int k)
{
	if (k <= 0 || k >= RP_NIF || !m->ifc[k].ifname[0])
		return;
	list_drop(m->alias[0], RP_NALIAS, RP_ALIASLEN, m->ifc[k].ifname, 1);
	list_drop(m->vlan[0], RP_NVLAN, RP_VLANLEN, m->ifc[k].ifname, 0);
	list_drop(m->addr6[0], RP_NADDR6, RP_ADDR6LEN, m->ifc[k].ifname, 1);
	dev_drop(m->route[0], RP_NROUTE, RP_ROUTELEN, m->ifc[k].ifname);
	dev_drop(m->neigh[0], RP_NNEIGH, RP_NEIGHLEN, m->ifc[k].ifname);
	memset(&m->ifc[k], 0, sizeof(m->ifc[k]));
}

/* the prefixes of a list that name card old (or a VLAN of it) say name */
static void list_rename(char *base, int n, size_t len, const char *old, const char *name, int vlans)
{
	char buf[RP_VLANLEN + RP_ALIASLEN];
	int i;

	for (i = 0; i < n && base[(size_t)i * len]; i++) {
		char *e = base + (size_t)i * len;
		if (!on_iface(e, old, vlans))
			continue;
		rt_snprintf(buf, sizeof(buf), "%s%s", name, e + strlen(old));
		if (strlen(buf) < len)
			copy(e, buf, len);
	}
}

void rp_iface_rename(struct rp_model *m, int k, const char *name)
{
	char old[RP_IFNAME];

	if (k < 0 || k >= RP_NIF || !m->ifc[k].ifname[0] || !strcmp(m->ifc[k].ifname, name))
		return;
	copy(old, m->ifc[k].ifname, sizeof(old));
	copy(m->ifc[k].ifname, name, sizeof(m->ifc[k].ifname));
	list_rename(m->alias[0], RP_NALIAS, RP_ALIASLEN, old, name, 1);
	list_rename(m->addr6[0], RP_NADDR6, RP_ADDR6LEN, old, name, 1);
	if (k > 0)
		list_rename(m->vlan[0], RP_NVLAN, RP_VLANLEN, old, name, 0);
	dev_rename(m->route[0], RP_NROUTE, RP_ROUTELEN, old, name);
	dev_rename(m->neigh[0], RP_NNEIGH, RP_NEIGHLEN, old, name);
}

int rp_iface_names(const struct rp_model *m, char (*names)[RP_IFNAMELEN], int max, int vlans)
{
	char par[RP_IFNAME];
	int k, i, n = 0;

	for (k = 0; k < RP_NIF && n < max; k++)
		if (m->ifc[k].ifname[0])
			copy(names[n++], m->ifc[k].ifname, RP_IFNAMELEN);
	for (i = 0; vlans && i < RP_NVLAN && m->vlan[i][0] && n < max; i++) {
		rp_entry_if(m, m->vlan[i], par, sizeof(par));
		rt_snprintf(names[n++], RP_IFNAMELEN, "%s.%lu", par, rt_strtoul(rp_entry_body(m->vlan[i]), NULL, 10));
	}
	return n;
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
	char w[5][RP_ALIASLEN];
	int n = 0, i;
	size_t k;

	while (*s && n < 5) {
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

/* ID [dhcp|up|A.B.C.D/BITS] [pcp=0-7|pcp=priority] [mtu=68-65535]
 * [ipv4=on|off] [ipv6=on|off] [qos=on|off] [tcpwindow=0|536-16777216],
 * each at most once */
int rp_valid_vlan(const char *s)
{
	static const char *const keys[RP_VLAN_NKEYS] = { "pcp", "mtu", "ipv4", "ipv6", "qos", "tcpwindow" };
	char w[RP_VLAN_NKEYS + 3][RP_VLANLEN];
	int n = 0, i, j, mode = 0, seen = 0;
	size_t k;
	unsigned long id;
	const char *e, *v;

	while (*s && n < RP_VLAN_NKEYS + 3) {
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
	if (*s || !n || n > RP_VLAN_NKEYS + 2)
		return 0;
	id = rt_strtoul(w[0], &e, 10);
	if (*e || e == w[0] || id < 1 || id > 4094)
		return 0;
	for (i = 1; i < n; i++) {
		for (j = 0; j < RP_VLAN_NKEYS && !(v = kv(w[i], keys[j])); j++)
			;
		if (j < RP_VLAN_NKEYS) {
			unsigned long x = rt_strtoul(v, &e, 10);
			int isnum = *v && !*e;
			if (seen & (1 << j))
				return 0;
			seen |= 1 << j;
			if (j == 0 ? !(eq(v, "priority") || (isnum && x <= 7)) :
			    j == 1 ? !(isnum && x >= 68 && x <= 65535) :
			    j == 5 ? !(isnum && (x == 0 || (x >= 536 && x <= 16777216UL))) :
			    !(eq(v, "on") || eq(v, "off")))
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

/* an IPv6 literal up to a '/' (with or without a %zone); *end gets the
 * rest ("" or "/...") */
static int ip6_literal(const char *s, const char **end)
{
	int colons = 0, dbl = 0, group = 0, dots = 0;
	const char *p;

	*end = s;
	for (p = s;; p++) {
		if (*p == ':' || *p == 0 || *p == '%' || *p == '/') {
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
	if (*p == '%') {
		/* a zone: an interface name */
		for (p++; *p && *p != '/'; p++)
			if (*p == ' ' || *p == '%')
				return 0;
	}
	*end = p;
	return colons >= 2 && colons <= 8 && (dbl || colons == 7 || dots == 3);
}

int rp_valid_ip6(const char *s, int allow_prefix)
{
	const char *end;
	long b;

	if (!ip6_literal(s, &end))
		return 0;
	if (!*end)
		return 1;
	if (*end != '/' || !allow_prefix)
		return 0;
	b = num(end + 1);
	return b >= 0 && b <= 128;
}

int rp_valid_mac(const char *s)
{
	int i;

	for (i = 0; i < 6; i++) {
		int d = 0;
		while (((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) && d < 2)
			s++, d++;
		if (d != 2)
			return 0;
		if (i < 5 && *s++ != ':')
			return 0;
	}
	return *s == 0;
}

/* the next word of *s into w (len bytes); 0 at the end */
static int next_word(const char **s, char *w, size_t len)
{
	size_t k = 0;

	while (**s == ' ')
		(*s)++;
	if (!**s)
		return 0;
	for (; **s && **s != ' '; (*s)++)
		if (k < len - 1)
			w[k++] = **s;
	w[k] = 0;
	return 1;
}

/* DEST [via GW] [dev IF] [metric N] [reject|prohibit|unreachable|
 * blackhole], the keys in this order and at most once; a gateway, an
 * interface or a type; DEST default, A.B.C.D[/BITS] or IPv6[/PLEN],
 * the gateway of DEST's family */
int rp_valid_route(const char *s)
{
	char w[RP_ROUTELEN], v[RP_ROUTELEN];
	const char *e;
	int step = 0, v6 = -1, gw = 0, dev = 0, type = 0;

	if (strlen(s) >= RP_ROUTELEN || !next_word(&s, w, sizeof(w)))
		return 0;
	if (eq(w, "default"))
		v6 = -1;
	else if (rp_valid_ip4(w, 1))
		v6 = 0;
	else if (rp_valid_ip6(w, 1) && !strchr(w, '%'))
		v6 = 1;
	else
		return 0;
	while (next_word(&s, w, sizeof(w))) {
		if (eq(w, "via") && step < 1) {
			if (!next_word(&s, v, sizeof(v)))
				return 0;
			if (v6 == 0 ? !rp_valid_ip4(v, 0) : v6 == 1 ? !rp_valid_ip6(v, 0) : !rp_valid_ip(v))
				return 0;
			if (v6 < 0)
				v6 = rp_valid_ip4(v, 0) ? 0 : 1;
			step = 1;
			gw = 1;
		} else if (eq(w, "dev") && step < 2) {
			if (!next_word(&s, v, sizeof(v)) || strlen(v) >= RP_IFNAMELEN || strchr(v, RP_IFSEP))
				return 0;
			step = 2;
			dev = 1;
		} else if (eq(w, "metric") && step < 3) {
			if (!next_word(&s, v, sizeof(v)))
				return 0;
			rt_strtoul(v, &e, 10);
			if (e == v || *e || num(v) < 0)
				return 0;
			step = 3;
		} else if ((eq(w, "reject") || eq(w, "prohibit") || eq(w, "unreachable") || eq(w, "blackhole")) &&
		           step < 4) {
			step = 4;
			type = 1;
		} else {
			return 0;
		}
	}
	return gw || dev || type;
}

/* NAME ADDR: a host name (not an address) and an IPv4 or IPv6 address */
int rp_valid_host(const char *s)
{
	char n[RP_HOSTLEN], a[RP_HOSTLEN], x[8];

	if (strlen(s) >= RP_HOSTLEN || !next_word(&s, n, sizeof(n)) || !next_word(&s, a, sizeof(a)) ||
	    next_word(&s, x, sizeof(x)))
		return 0;
	return rp_valid_name(n) && !rp_valid_ip4(n, 0) && strlen(n) < RP_STR && rp_valid_ip(a) && !strchr(a, '%');
}

/* ADDR MAC [dev IF] */
int rp_valid_neigh(const char *s)
{
	char a[RP_NEIGHLEN], m[RP_NEIGHLEN], d[RP_NEIGHLEN], x[8];

	if (strlen(s) >= RP_NEIGHLEN || !next_word(&s, a, sizeof(a)) || !next_word(&s, m, sizeof(m)))
		return 0;
	if (!rp_valid_ip(a) || strchr(a, '%') || !rp_valid_mac(m))
		return 0;
	if (!next_word(&s, d, sizeof(d)))
		return 1;
	return eq(d, "dev") && next_word(&s, d, sizeof(d)) && strlen(d) < RP_IFNAMELEN && !strchr(d, RP_IFSEP) &&
	       !next_word(&s, x, sizeof(x));
}

/* ADDR/PLEN [nodad] */
int rp_valid_addr6(const char *s)
{
	char a[RP_ADDR6LEN], w[RP_ADDR6LEN], x[8];

	if (strlen(s) >= RP_ADDR6LEN || !next_word(&s, a, sizeof(a)))
		return 0;
	if (!strchr(a, '/') || strchr(a, '%') || !rp_valid_ip6(a, 1))
		return 0;
	if (!next_word(&s, w, sizeof(w)))
		return 1;
	return eq(w, "nodad") && !next_word(&s, x, sizeof(x));
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
	if (list_changed(o, n, RP_LIST_ALIAS) && !str_changed(o->ifc[0].ifname, n->ifc[0].ifname)) {
		char ifn[RP_IFNAMELEN];
		const char *b;
		int j, k;
		/* removed addresses first, then the new ones */
		for (i = 0; i < RP_NALIAS && o->alias[i][0]; i++) {
			for (j = 0; j < RP_NALIAS && str_changed(o->alias[i], n->alias[j]); j++)
				;
			if (j < RP_NALIAS)
				continue;
			rp_entry_if(o, o->alias[i], ifn, sizeof(ifn));
			b = rp_entry_body(o->alias[i]);
			for (k = 0; b[k] && b[k] != '/' && b[k] != ' '; k++)
				;
			rt_snprintf(buf, sizeof(buf), "ifconfig %s %.*s -alias", ifn, k, b);
			fn(buf, arg);
		}
		for (i = 0; i < RP_NALIAS && n->alias[i][0]; i++) {
			for (j = 0; j < RP_NALIAS && str_changed(n->alias[i], o->alias[j]); j++)
				;
			if (j < RP_NALIAS)
				continue;
			rp_entry_if(n, n->alias[i], ifn, sizeof(ifn));
			rt_snprintf(buf, sizeof(buf), "ifconfig %s alias %s", ifn, rp_entry_body(n->alias[i]));
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_VLAN) && !str_changed(o->ifc[0].ifname, n->ifc[0].ifname)) {
		char ifn[RP_IFNAMELEN];
		int j;
		/* removed (or changed) VLANs go, then the new ones are made */
		for (i = 0; i < RP_NVLAN && o->vlan[i][0]; i++) {
			for (j = 0; j < RP_NVLAN && str_changed(o->vlan[i], n->vlan[j]); j++)
				;
			if (j < RP_NVLAN)
				continue;
			rp_entry_if(o, o->vlan[i], ifn, sizeof(ifn));
			rt_snprintf(buf, sizeof(buf), "interface %s.%lu delete", ifn,
			            rt_strtoul(rp_entry_body(o->vlan[i]), NULL, 10));
			fn(buf, arg);
		}
		for (i = 0; i < RP_NVLAN && n->vlan[i][0]; i++) {
			for (j = 0; j < RP_NVLAN && str_changed(n->vlan[i], o->vlan[j]); j++)
				;
			if (j < RP_NVLAN)
				continue;
			vlan_line(n->ifc[0].ifname, n->vlan[i], buf, sizeof(buf));
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_ADDR6) && !str_changed(o->ifc[0].ifname, n->ifc[0].ifname)) {
		char ifn[RP_IFNAMELEN];
		const char *b;
		int k;
		for (i = 0; i < RP_NADDR6 && o->addr6[i][0]; i++) {
			if (list_has(n, RP_LIST_ADDR6, o->addr6[i]))
				continue;
			rp_entry_if(o, o->addr6[i], ifn, sizeof(ifn));
			b = rp_entry_body(o->addr6[i]);
			for (k = 0; b[k] && b[k] != '/' && b[k] != ' '; k++)
				;
			rt_snprintf(buf, sizeof(buf), "ifconfig %s inet6 %.*s delete", ifn, k, b);
			fn(buf, arg);
		}
		for (i = 0; i < RP_NADDR6 && n->addr6[i][0]; i++) {
			if (list_has(o, RP_LIST_ADDR6, n->addr6[i]))
				continue;
			rp_entry_if(n, n->addr6[i], ifn, sizeof(ifn));
			rt_snprintf(buf, sizeof(buf), "ifconfig %s inet6 %s", ifn, rp_entry_body(n->addr6[i]));
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_ROUTE)) {
		for (i = 0; i < RP_NROUTE && o->route[i][0]; i++) {
			size_t k;
			if (list_has(n, RP_LIST_ROUTE, o->route[i]))
				continue;
			/* the entry without its type word */
			rt_snprintf(buf, sizeof(buf), "route del %s", o->route[i]);
			k = strlen(buf);
			while (k && buf[k - 1] != ' ')
				k--;
			if (k && (eq(buf + k, "reject") || eq(buf + k, "prohibit") || eq(buf + k, "unreachable") ||
			          eq(buf + k, "blackhole")))
				buf[k - 1] = 0;
			fn(buf, arg);
		}
		for (i = 0; i < RP_NROUTE && n->route[i][0]; i++) {
			if (list_has(o, RP_LIST_ROUTE, n->route[i]))
				continue;
			rt_snprintf(buf, sizeof(buf), "route add %s", n->route[i]);
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_NEIGH)) {
		for (i = 0; i < RP_NNEIGH && o->neigh[i][0]; i++) {
			char dev[RP_IFNAMELEN];
			const char *e = o->neigh[i];
			int k;
			if (list_has(n, RP_LIST_NEIGH, e))
				continue;
			for (k = 0; e[k] && e[k] != ' '; k++)
				;
			rp_entry_dev(e, dev, sizeof(dev));
			rt_snprintf(buf, sizeof(buf), "arp del %.*s%s%s", k, e, dev[0] ? " dev " : "", dev);
			fn(buf, arg);
		}
		for (i = 0; i < RP_NNEIGH && n->neigh[i][0]; i++) {
			if (list_has(o, RP_LIST_NEIGH, n->neigh[i]))
				continue;
			rt_snprintf(buf, sizeof(buf), "arp add %s", n->neigh[i]);
			fn(buf, arg);
		}
	}
	if (list_changed(o, n, RP_LIST_HOST)) {
		for (i = 0; i < RP_NHOST && o->host[i][0]; i++) {
			const char *e = o->host[i];
			int k;
			if (list_has(n, RP_LIST_HOST, e))
				continue;
			for (k = 0; e[k] && e[k] != ' '; k++)
				;
			rt_snprintf(buf, sizeof(buf), "host del %.*s", k, e);
			fn(buf, arg);
		}
		for (i = 0; i < RP_NHOST && n->host[i][0]; i++) {
			if (list_has(o, RP_LIST_HOST, n->host[i]))
				continue;
			rt_snprintf(buf, sizeof(buf), "host %s", n->host[i]);
			fn(buf, arg);
		}
	}
	/* IPv6 on or off on one card (back to the default needs a restart) */
	for (i = 0; i < RP_NIF; i++)
		if (n->ifc[i].ifname[0] && o->ifc[i].ipv6 != n->ifc[i].ipv6 &&
		    !str_changed(o->ifc[i].ifname, n->ifc[i].ifname)) {
			if (n->ifc[i].ipv6 < 0) {
				restart = 1;
				continue;
			}
			rt_snprintf(buf, sizeof(buf), "%s %s %s", RP_KW_IPV6, n->ifc[i].ifname, n->ifc[i].ipv6 ? "on" : "off");
			fn(buf, arg);
		}
	if (ifaces_changed(o, n) || o->ipv6 != n->ipv6 || o->ipv4 != n->ipv4 ||
	    ((list_changed(o, n, RP_LIST_ALIAS) || list_changed(o, n, RP_LIST_VLAN) ||
	      list_changed(o, n, RP_LIST_ADDR6)) && str_changed(o->ifc[0].ifname, n->ifc[0].ifname)))
		restart = 1;
	return restart;
}
