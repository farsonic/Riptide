/*
 * Riptide - RiptidePrefs: the Status page's arithmetic and wording (see
 * prefs_status.h). No floating point and no 64-bit division: both cost a
 * library call on the 68020 and this runs once a second.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdarg.h>
#include <string.h>
#include "rt/rt_ip6.h"
#include "prefs_status.h"

/* rt_ctl_iface.type and .flags (BSD IFF_* values) */
#define ST_IFT_LOOP     3
#define ST_IFF_UP       0x0001
#define ST_IFF_RUNNING  0x0040

/* ---- history ---- */
void rp_hist_clear(struct rp_hist *h)
{
	memset(h, 0, sizeof(*h));
}

void rp_hist_push(struct rp_hist *h, u32 a, u32 b)
{
	int i;

	if (h->n < RP_HIST) {
		i = (h->head + h->n) % RP_HIST;
		h->n++;
	} else {
		i = h->head;
		h->head = (h->head + 1) % RP_HIST;
	}
	h->v[0][i] = a;
	h->v[1][i] = b;
	h->pushes++;
}

u32 rp_hist_at(const struct rp_hist *h, int s, int i)
{
	if (i < 0 || i >= h->n || s < 0 || s > 1)
		return 0;
	return h->v[s][(h->head + i) % RP_HIST];
}

u32 rp_hist_max(const struct rp_hist *h, int s)
{
	u32 m = 0, v;
	int i;

	for (i = 0; i < h->n; i++) {
		if (s != 1 && (v = rp_hist_at(h, 0, i)) > m)
			m = v;
		if (s != 0 && (v = rp_hist_at(h, 1, i)) > m)
			m = v;
	}
	return m;
}

u32 rp_nice_scale(u32 v, u32 min)
{
	static const u32 step[3] = { 1, 2, 5 };
	u32 p = 1;

	if (v < min)
		v = min;
	if (v == 0)
		return 1;
	for (;;) {
		int k;
		for (k = 0; k < 3; k++)
			if (step[k] * p >= v)
				return step[k] * p;
		if (p > 0xffffffffUL / 50)
			return 0xffffffffUL;
		p *= 10;
	}
}

int rp_scale_y(u32 v, u32 scale, int h)
{
	if (scale == 0 || h <= 0)
		return 0;
	if (v >= scale)
		return h;
	/* v * h must fit 32 bits: drop the same low bits of both */
	while (v > 0xffffffffUL / (u32)h) {
		v >>= 1;
		scale >>= 1;
	}
	return scale ? (int)(v * (u32)h / scale) : 0;
}

/* ---- rates ---- */
u32 rp_rate(u32 prev, u32 cur, u32 ms)
{
	u32 d = cur - prev;

	if (ms == 0)
		return 0;
	if (d <= 0xffffffffUL / 1000u)
		return d * 1000u / ms;
	return d / ms * 1000u + d % ms * 1000u / ms;
}

u32 rp_cpu_permille(u32 idle0, u32 now0, u32 idle1, u32 now1)
{
	u32 dn = now1 - now0, di = idle1 - idle0, busy;

	if (dn == 0 || di >= dn)
		return 0;
	busy = dn - di;
	while (dn > 4000000u) {
		dn >>= 1;
		busy >>= 1;
	}
	return dn ? busy * 1000u / dn : 0;
}

/* ---- wording ---- */
/* v hundredths: "4.56", "45.6", "456" */
static void three_digits(char *buf, int len, u32 v100, const char *unit)
{
	if (v100 < 1000)
		rt_snprintf(buf, (size_t)len, "%lu.%02lu %s", (unsigned long)(v100 / 100), (unsigned long)(v100 % 100), unit);
	else if (v100 < 10000)
		rt_snprintf(buf, (size_t)len, "%lu.%lu %s", (unsigned long)(v100 / 100), (unsigned long)(v100 / 10 % 10),
		            unit);
	else
		rt_snprintf(buf, (size_t)len, "%lu %s", (unsigned long)(v100 / 100), unit);
}

void rp_fmt_bytes(char *buf, int len, u32 hi, u32 lo)
{
	static const char *const unit[5] = { "B", "KB", "MB", "GB", "TB" };
	u64 v = ((u64)hi << 32) | lo;
	int u = 0;

	if (v < 1024) {
		rt_snprintf(buf, (size_t)len, "%lu B", (unsigned long)lo);
		return;
	}
	while (u < 4 && (v >> (10 * (u + 1))) >= 1)
		u++;
	if ((v >> (10 * u)) >= 1024 && u < 4)
		u++;
	three_digits(buf, len, (u32)((v * 100) >> (10 * u)), unit[u]);
}

void rp_fmt_rate(char *buf, int len, u32 bps)
{
	char b[24];

	rp_fmt_bytes(b, sizeof(b), 0, bps);
	rt_snprintf(buf, (size_t)len, "%s/s", b);
}

void rp_fmt_count(char *buf, int len, u32 v)
{
	char t[16];
	int n = 0, i, k = 0;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (i = n - 1; i >= 0 && k < len - 1; i--) {
		buf[k++] = t[i];
		if (i && i % 3 == 0 && k < len - 1)
			buf[k++] = ' ';
	}
	buf[k] = 0;
}

void rp_fmt_dur(char *buf, int len, u32 s)
{
	if (s < 60)
		rt_snprintf(buf, (size_t)len, "%lu s", (unsigned long)s);
	else if (s < 3600)
		rt_snprintf(buf, (size_t)len, "%lu min", (unsigned long)(s / 60));
	else if (s < 86400)
		rt_snprintf(buf, (size_t)len, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
	else
		rt_snprintf(buf, (size_t)len, "%lu day%s %lu h", (unsigned long)(s / 86400), s < 2 * 86400 ? "" : "s",
		            (unsigned long)(s / 3600 % 24));
}

void rp_fmt_uptime(char *buf, int len, u32 s)
{
	u32 d = s / 86400;

	s %= 86400;
	if (d)
		rt_snprintf(buf, (size_t)len, "%lu day%s %lu:%02lu:%02lu", (unsigned long)d, d == 1 ? "" : "s",
		            (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
	else
		rt_snprintf(buf, (size_t)len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
		            (unsigned long)(s % 60));
}

void rp_fmt_offset(char *buf, int len, s32 ms)
{
	char sign = ms < 0 ? '-' : '+';
	u32 a = ms < 0 ? (u32)(-(ms + 1)) + 1u : (u32)ms;

	if (a < 10000)
		rt_snprintf(buf, (size_t)len, "%c%lu.%03lu s", sign, (unsigned long)(a / 1000), (unsigned long)(a % 1000));
	else
		rt_snprintf(buf, (size_t)len, "%c%lu s", sign, (unsigned long)(a / 1000));
}

void rp_fmt_ms(char *buf, int len, u32 us)
{
	char t[16];

	if (us < 10000) {
		rt_snprintf(buf, (size_t)len, "%lu.%lu ms", (unsigned long)(us / 1000), (unsigned long)(us / 100 % 10));
		return;
	}
	rp_fmt_count(t, sizeof(t), us / 1000);
	rt_snprintf(buf, (size_t)len, "%s ms", t);
}

void rp_fmt_ip4(char *buf, int len, u32 addr_n)
{
	const u8 *b = (const u8 *)&addr_n;

	if (!addr_n)
		rt_strlcpy(buf, "-", (size_t)len);
	else
		rt_snprintf(buf, (size_t)len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void rp_fmt_ip4_plen(char *buf, int len, u32 addr_n, u32 mask_n)
{
	const u8 *m = (const u8 *)&mask_n;
	char a[20];
	int i, n = 0;

	for (i = 0; i < 4; i++) {
		u8 x = m[i];
		while (x & 0x80) {
			n++;
			x = (u8)(x << 1);
		}
	}
	rp_fmt_ip4(a, sizeof(a), addr_n);
	if (addr_n && mask_n)
		rt_snprintf(buf, (size_t)len, "%s/%d", a, n);
	else
		rt_strlcpy(buf, a, (size_t)len);
}

void rp_fmt_addr_port(char *buf, int len, const u8 *a16, u16 port)
{
	static const u8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
	char a[48];

	if (!memcmp(a16, mapped, 12)) {
		rt_snprintf(a, sizeof(a), "%u.%u.%u.%u", a16[12], a16[13], a16[14], a16[15]);
		if (port)
			rt_snprintf(buf, (size_t)len, "%s:%u", a, (unsigned)port);
		else
			rt_strlcpy(buf, a, (size_t)len);
		return;
	}
	{
		rt_in6_addr x;
		memcpy(x.b, a16, 16);
		rt_inet6_ntop(&x, a, sizeof(a));
	}
	if (port)
		rt_snprintf(buf, (size_t)len, "[%s]:%u", a, (unsigned)port);
	else
		rt_strlcpy(buf, a, (size_t)len);
}

const char *rp_dhcp_state_name(int st)
{
	static const char *const n[] = { "off", "starting", "looking for a server", "asking for an address", "bound",
	                                 "renewing", "rebinding" };
	return st >= 0 && st < (int)(sizeof(n) / sizeof(n[0])) ? n[st] : "unknown";
}

const char *rp_tcp_state_words(int st)
{
	static const char *const n[] = { "Closed", "Listening", "Connecting", "Answering", "Connected", "Closing (them)",
	                                 "Closing", "Closing", "Closing", "Closing", "Time wait" };
	return st >= 0 && st < (int)(sizeof(n) / sizeof(n[0])) ? n[st] : "?";
}

/* ---- the model ---- */
void rp_status_init(struct rp_status_model *m)
{
	memset(m, 0, sizeof(*m));
}

void rp_status_stopped(struct rp_status_model *m)
{
	rp_status_init(m);
}

/* record i of a table into a zeroed struct of size sz (older stacks send
 * shorter records; later ones longer) */
static void rec(void *dst, u32 sz, const u8 *base, u32 recsize, int i)
{
	memset(dst, 0, sz);
	memcpy(dst, base + (u32)i * recsize, recsize < sz ? recsize : sz);
}

static void copy_part(void *dst, u32 sz, const void *src, u32 srclen)
{
	memset(dst, 0, sz);
	if (src)
		memcpy(dst, src, srclen < sz ? srclen : sz);
}

static struct rp_st_if *find_if(struct rp_status_model *m, const char *name)
{
	int i;

	for (i = 0; i < m->nif; i++)
		if (!strcmp(m->ifs[i].name, name))
			return &m->ifs[i];
	return NULL;
}

static void update_ifs(struct rp_status_model *m, const struct rp_st_input *in, u32 ms)
{
	struct rp_st_if old[RP_ST_NIF];
	int nold = m->nif, i, k;
	u32 defgw = 0, defif = 0;

	memcpy(old, m->ifs, sizeof(old));
	/* the default route's gateway, for interfaces without a DHCP router */
	for (i = 0; in->routes && i < in->nroutes; i++) {
		struct rt_ctl_route r;
		rec(&r, sizeof(r), in->routes, in->rrec, i);
		if (r.dst == 0 && r.mask == 0 && r.gateway && !defgw) {
			defgw = r.gateway;
			defif = r.ifindex;
		}
	}
	m->nif = 0;
	m->rx_bps = m->tx_bps = 0;
	for (i = 0; in->ifaces && i < in->nifaces && m->nif < RP_ST_NIF; i++) {
		struct rt_ctl_iface r;
		struct rp_st_if *f = &m->ifs[m->nif], *o = NULL;
		rec(&r, sizeof(r), in->ifaces, in->ifrec, i);
		for (k = 0; k < nold; k++)
			if (!strncmp(old[k].name, r.name, sizeof(r.name)))
				o = &old[k];
		if (o)
			*f = *o;
		else
			memset(f, 0, sizeof(*f));
		rt_strlcpy(f->name, r.name, sizeof(f->name));
		rt_strlcpy(f->descr, r.descr, sizeof(f->descr));
		f->index = r.index;
		f->flags = r.flags;
		f->mtu = r.mtu;
		f->dhcp_state = r.dhcp_state;
		f->vlan_id = r.vlan_parent ? r.vlan_id : 0;
		f->addr = r.addr;
		f->mask = r.netmask;
		f->router = r.dhcp_state >= RT_DHCPS_BOUND && r.dhcp_router ? r.dhcp_router :
		            defif == r.index ? defgw : 0;
		f->ll_addr = r.ll_addr;
		f->ll_state = r.ll_state;
		f->dhcp_left = r.dhcp_left_s;
		f->ip4_off = r.ip4_off;
		f->ip6_off = r.ip6_off;
		f->ierr = r.ierrors;
		f->oerr = r.oerrors;
		f->drops = r.iqdrops;
		if (o && m->have_prev) {
			f->rx_bps = rp_rate(o->ibytes, r.ibytes, ms);
			f->tx_bps = rp_rate(o->obytes, r.obytes, ms);
			if (f->rx_bps > f->rx_peak)
				f->rx_peak = f->rx_bps;
			if (f->tx_bps > f->tx_peak)
				f->tx_peak = f->tx_bps;
			rp_hist_push(&f->hist, f->rx_bps, f->tx_bps);
		} else {
			f->rx_bps = f->tx_bps = 0;
		}
		f->ibytes = r.ibytes;
		f->obytes = r.obytes;
		f->ipk = r.ipackets;
		f->opk = r.opackets;
		f->nv6 = 0;
		/* loopback is not network traffic */
		if (r.type != ST_IFT_LOOP) {
			m->rx_bps += f->rx_bps;
			m->tx_bps += f->tx_bps;
		}
		m->nif++;
	}
	/* IPv6 addresses: global ones first, then link-local */
	for (k = 0; k < 2; k++)
		for (i = 0; in->addrs6 && i < in->naddrs6; i++) {
			struct rt_ctl_addr6 a;
			struct rp_st_if *f;
			rec(&a, sizeof(a), in->addrs6, in->a6rec, i);
			a.ifname[sizeof(a.ifname) - 1] = 0;
			f = find_if(m, a.ifname);
			if (!f || f->nv6 >= RP_ST_NV6 || (k == 0) != (a.scope != 2) || a.scope == 1 ||
			    (a.flags & RT_CTL_IN6_TENTATIVE))
				continue;
			rp_fmt_addr_port(f->v6[f->nv6], sizeof(f->v6[0]), a.addr, 0);
			f->nv6++;
		}
	if (m->have_prev) {
		if (m->rx_bps > m->rx_peak)
			m->rx_peak = m->rx_bps;
		if (m->tx_bps > m->tx_peak)
			m->tx_peak = m->tx_bps;
		rp_hist_push(&m->total, m->rx_bps, m->tx_bps);
	}
}

/* connections that move data first, then the established ones, then the rest */
static int conn_rank(const struct rp_st_conn *c)
{
	if (c->rx_bps || c->tx_bps)
		return 0;
	if (c->state == 4)
		return 1;
	if (c->state == 1)
		return 3;
	return 2;
}

static void update_conns(struct rp_status_model *m, const struct rp_st_input *in, u32 ms)
{
	static struct rp_st_conn old[RP_ST_NCONN];
	int nold = m->nconn, i, k;

	memcpy(old, m->conns, sizeof(old));
	m->nconn = 0;
	m->nconn_all = in->tcp ? in->ntcp : 0;
	for (i = 0; in->tcp && i < in->ntcp; i++) {
		struct rt_ctl_tcpinfo t;
		struct rp_st_conn c, *o = NULL;
		rec(&t, sizeof(t), in->tcp, in->trec, i);
		for (k = 0; k < nold; k++)
			if (old[k].lport == t.lport && old[k].fport == t.fport && !memcmp(old[k].faddr, t.faddr, 16) &&
			    !memcmp(old[k].laddr, t.laddr, 16))
				o = &old[k];
		if (o)
			c = *o;
		else
			memset(&c, 0, sizeof(c));
		c.family = t.family;
		c.state = t.state;
		c.ca_state = t.ca_state;
		c.ecn = (t.options & RT_CTL_TCPI_ECN) != 0;
		c.lport = t.lport;
		c.fport = t.fport;
		memcpy(c.laddr, t.laddr, 16);
		memcpy(c.faddr, t.faddr, 16);
		c.srtt_us = t.srtt_us;
		c.cwnd = t.cwnd;
		c.mss = t.snd_mss;
		c.retrans = t.total_retrans;
		rt_strlcpy(c.cc, t.cc, sizeof(c.cc));
		memcpy(c.prog, t.prog, sizeof(c.prog));
		c.prog[sizeof(c.prog) - 1] = 0;
		if (o && m->have_prev) {
			c.rx_bps = rp_rate(o->rx_lo, t.bytes_received_lo, ms);
			c.tx_bps = rp_rate(o->tx_lo, t.bytes_acked_lo, ms);
		} else {
			c.rx_bps = c.tx_bps = 0;
		}
		c.rx_lo = t.bytes_received_lo;
		c.tx_lo = t.bytes_acked_lo;
		if (t.state != 1)
			rp_hist_push(&c.hist, t.srtt_us / 100u, t.cwnd);
		/* keep the best RP_ST_NCONN: insert by rank */
		{
			int r = conn_rank(&c), pos = m->nconn;
			while (pos > 0 && conn_rank(&m->conns[pos - 1]) > r)
				pos--;
			if (pos >= RP_ST_NCONN)
				continue;
			if (m->nconn < RP_ST_NCONN)
				m->nconn++;
			memmove(&m->conns[pos + 1], &m->conns[pos], (size_t)(m->nconn - 1 - pos) * sizeof(c));
			m->conns[pos] = c;
		}
	}
}

void rp_status_update(struct rp_status_model *m, const struct rp_st_input *in)
{
	u32 ms = m->have_prev ? in->now_ms - m->now_ms : 0;
	struct rt_ctl_status prev = m->st;
	u32 prev_retrans = m->tcp.RetransSegs;
	int had_tcp = m->have_tcpst;

	m->running = 1;
	if (in->st) {
		copy_part(&m->st, sizeof(m->st), in->st, in->st_len);
		m->st.version[sizeof(m->st.version) - 1] = 0;
		m->st.ntp_server[sizeof(m->st.ntp_server) - 1] = 0;
		m->st.ntp_error[sizeof(m->st.ntp_error) - 1] = 0;
		m->cpu_ok = m->have_prev && prev.cpu_hz && prev.cpu_on && m->st.cpu_on;
		m->cpu_pm = m->cpu_ok ? rp_cpu_permille(prev.cpu_idle, prev.cpu_now, m->st.cpu_idle, m->st.cpu_now) : 0;
		if (m->cpu_ok)
			rp_hist_push(&m->cpu_hist, m->cpu_pm, 0);
	}
	update_ifs(m, in, ms);
	update_conns(m, in, ms);
	if (in->dns) {
		struct rt_ctl_dns d;
		int i;
		copy_part(&d, sizeof(d), in->dns, in->dns_len);
		m->ndns = d.nservers > RT_CTL_MAXNS ? RT_CTL_MAXNS : (int)d.nservers;
		for (i = 0; i < m->ndns; i++)
			m->dns[i] = d.servers[i];
		rt_strlcpy(m->domain, d.domain, sizeof(m->domain));
		rt_strlcpy(m->hostname, d.hostname, sizeof(m->hostname));
	}
	m->have_tcpst = in->tcpst != NULL;
	if (in->tcpst)
		copy_part(&m->tcp, sizeof(m->tcp), in->tcpst, in->tcpst_len);
	m->retrans_ps = had_tcp && m->have_tcpst && m->have_prev ? rp_rate(prev_retrans, m->tcp.RetransSegs, ms) : 0;
	m->have_ipst = in->ipst != NULL;
	if (in->ipst)
		copy_part(&m->ip, sizeof(m->ip), in->ipst, in->ipst_len);
	m->now_ms = in->now_ms;
	m->have_prev = 1;
}

/* ---- text ---- */
/* append to buf (len bytes) at k; returns the new end */
static int addf(char *buf, int len, int k, const char *fmt, ...)
{
	va_list ap;

	if (k >= len - 1)
		return k;
	va_start(ap, fmt);
	rt_vsnprintf(buf + k, (size_t)(len - k), fmt, ap);
	va_end(ap);
	return k + (int)strlen(buf + k);
}

void rp_st_summary(const struct rp_status_model *m, char *buf, int len)
{
	char up[32], cpu[24];

	if (!m->running) {
		rt_strlcpy(buf, "Riptide is not running. Start it from its icon or a Shell.", (size_t)len);
		return;
	}
	rp_fmt_uptime(up, sizeof(up), m->st.uptime_s);
	if (m->cpu_ok)
		rt_snprintf(cpu, sizeof(cpu), " CPU %lu %%.", (unsigned long)((m->cpu_pm + 5) / 10));
	else
		cpu[0] = 0;
	rt_snprintf(buf, (size_t)len, "Riptide %s is running. Up %s.%s", m->st.version[0] ? m->st.version : "", up, cpu);
}

void rp_st_memory(const struct rp_status_model *m, char *buf, int len)
{
	static const char *const state[3] = { "Sockets", "Under pressure: sockets", "At the limit: sockets" };

	/* the state first: the line may be cut at its end */
	rt_snprintf(buf, (size_t)len, "%s use %lu of %lu KB, buffers %lu KB.",
	            state[m->st.mem_state < 3 ? m->st.mem_state : 0], (unsigned long)m->st.mem_used_kb,
	            (unsigned long)m->st.mem_high_kb, (unsigned long)m->st.buf_kb);
}

void rp_st_if_lines(const struct rp_st_if *f, char *l1, char *l2, char *l3, int len)
{
	char a[24], gw[20], dhcp[64], left[24];
	const char *state = !(f->flags & ST_IFF_UP) ? "down" : (f->flags & ST_IFF_RUNNING) ? "up" : "up, no link";
	int i, k;

	if (f->vlan_id)
		rt_snprintf(l1, (size_t)len, "%s  %s  VLAN %u  MTU %u", f->name, state, (unsigned)f->vlan_id,
		            (unsigned)f->mtu);
	else
		rt_snprintf(l1, (size_t)len, "%s  %s  %s  MTU %u", f->name, state, f->descr[0] ? f->descr : "",
		            (unsigned)f->mtu);
	if (f->ip4_off) {
		rt_strlcpy(l2, "IPv4 off", (size_t)len);
	} else {
		if (f->addr)
			rp_fmt_ip4_plen(a, sizeof(a), f->addr, f->mask);
		else if (f->ll_addr) {
			char l[20];
			rp_fmt_ip4(l, sizeof(l), f->ll_addr);
			rt_snprintf(a, sizeof(a), "%s/16", l);
		}
		else
			rt_strlcpy(a, "no IPv4 address", sizeof(a));
		gw[0] = 0;
		if (f->router) {
			char r[20];
			rp_fmt_ip4(r, sizeof(r), f->router);
			rt_snprintf(gw, sizeof(gw), " via %s", r);
		}
		dhcp[0] = 0;
		if (f->dhcp_state >= RT_DHCPS_BOUND && f->dhcp_left != 0xffffffffUL) {
			rp_fmt_dur(left, sizeof(left), f->dhcp_left);
			rt_snprintf(dhcp, sizeof(dhcp), ", DHCP %s, %s left", rp_dhcp_state_name(f->dhcp_state), left);
		} else if (f->dhcp_state != RT_DHCPS_NONE) {
			rt_snprintf(dhcp, sizeof(dhcp), ", DHCP %s", rp_dhcp_state_name(f->dhcp_state));
		}
		if (!f->addr && f->ll_addr)
			rt_strlcpy(dhcp + strlen(dhcp), ", link-local", sizeof(dhcp) - strlen(dhcp));
		rt_snprintf(l2, (size_t)len, "%s%s%s", a, gw, dhcp);
	}
	if (f->ip6_off) {
		rt_strlcpy(l3, "IPv6 off", (size_t)len);
		return;
	}
	if (!f->nv6) {
		rt_strlcpy(l3, "no IPv6 address", (size_t)len);
		return;
	}
	k = 0;
	l3[0] = 0;
	for (i = 0; i < f->nv6; i++)
		k += rt_snprintf(l3 + k, (size_t)(len - k > 0 ? len - k : 0), "%s%s", i ? ", " : "", f->v6[i]);
}

void rp_st_if_rates(const struct rp_st_if *f, char *buf, int len)
{
	char r[20], t[20];

	rp_fmt_rate(r, sizeof(r), f->rx_bps);
	rp_fmt_rate(t, sizeof(t), f->tx_bps);
	rt_snprintf(buf, (size_t)len, "In %s  Out %s", r, t);
}

void rp_st_if_totals(const struct rp_st_if *f, char *buf, int len)
{
	char rb[16], tb[16], rp[16], tp[16];

	rp_fmt_bytes(rb, sizeof(rb), 0, f->ibytes);
	rp_fmt_bytes(tb, sizeof(tb), 0, f->obytes);
	rp_fmt_count(rp, sizeof(rp), f->ipk);
	rp_fmt_count(tp, sizeof(tp), f->opk);
	rt_snprintf(buf, (size_t)len, "Received %s in %s packets, sent %s in %s; %lu errors, %lu dropped", rb, rp, tb, tp,
	            (unsigned long)(f->ierr + f->oerr), (unsigned long)f->drops);
}

const char *rp_st_conn_header(void)
{
	return "State\tRTT\tWindow\tSpeed\tProgram";
}

void rp_st_conn_row(const struct rp_st_conn *c, char *buf, int len)
{
	char l[52], r[52], rtt[16], spd[24], cw[24];
	int k;

	rp_fmt_addr_port(l, sizeof(l), c->laddr, c->lport);
	if (c->state == 1)
		rt_strlcpy(r, "*", sizeof(r));
	else
		rp_fmt_addr_port(r, sizeof(r), c->faddr, c->fport);
	if (c->state == 1 || !c->srtt_us)
		rt_strlcpy(rtt, "-", sizeof(rtt));
	else
		rp_fmt_ms(rtt, sizeof(rtt), c->srtt_us);
	if (c->state == 1)
		rt_strlcpy(cw, "-", sizeof(cw));
	else
		rt_snprintf(cw, sizeof(cw), "%lu%s", (unsigned long)c->cwnd, c->ecn ? " ECN" : "");
	if (c->rx_bps || c->tx_bps)
		rp_fmt_rate(spd, sizeof(spd), c->rx_bps > c->tx_bps ? c->rx_bps : c->tx_bps);
	else
		rt_strlcpy(spd, "-", sizeof(spd));
	if (c->state == 1)
		rt_snprintf(buf, (size_t)len, "%s\n", l);
	else
		rt_snprintf(buf, (size_t)len, "%s  to  %s\n", l, r);
	k = (int)strlen(buf);
	rt_snprintf(buf + k, (size_t)(len - k), "%s\t%s\t%s\t%s\t%s", rp_tcp_state_words(c->state), rtt, cw, spd,
	            c->prog[0] ? c->prog : "-");
}

void rp_st_names_text(const struct rp_status_model *m, char *buf, int len)
{
	char a[24], t[48];
	int k = 0, i;

	buf[0] = 0;
	k = addf(buf, len, k, "Host name: %s", m->hostname[0] ? m->hostname : "(none)");
	if (m->domain[0])
		k = addf(buf, len, k, ".%s", m->domain);
	k = addf(buf, len, k, "\nName servers:");
	if (!m->ndns)
		k = addf(buf, len, k, " none");
	for (i = 0; i < m->ndns; i++) {
		rp_fmt_ip4(a, sizeof(a), m->dns[i]);
		k = addf(buf, len, k, "%s %s", i ? "," : "", a);
	}
	k = addf(buf, len, k, "\nName cache: %lu answers from the cache, %lu \"no such name\", %lu failures not asked again",
	    (unsigned long)m->st.dns_hits, (unsigned long)m->st.dns_neg_hits, (unsigned long)m->st.dns_fail_hits);
	switch (m->st.ntp_state) {
	case RT_CTL_NTP_OFF:
		k = addf(buf, len, k, "\nTime: NTP is off (turn it on on the Time page)");
		break;
	case RT_CTL_NTP_WAIT:
		k = addf(buf, len, k, "\nTime: NTP is on, not synced yet");
		break;
	default:
		if (m->st.ntp_ago_s != 0xffffffffUL) {
			rp_fmt_dur(t, sizeof(t), m->st.ntp_ago_s);
			k = addf(buf, len, k, "\nTime: synced with %s %s ago", m->st.ntp_server[0] ? m->st.ntp_server : "the server", t);
			rp_fmt_offset(t, sizeof(t), m->st.ntp_off_ms);
			k = addf(buf, len, k, ", clock was %s off", t);
			rp_fmt_ms(t, sizeof(t), m->st.ntp_delay_us);
			k = addf(buf, len, k, ", round trip %s", t);
		} else {
			k = addf(buf, len, k, "\nTime: NTP has not synced yet");
		}
		if (m->st.ntp_state == RT_CTL_NTP_FAILED)
			k = addf(buf, len, k, "\nLast try failed: %s", m->st.ntp_error[0] ? m->st.ntp_error : "no answer");
		rp_fmt_dur(t, sizeof(t), m->st.ntp_next_s);
		k = addf(buf, len, k, "\nNext sync in %s", t);
		break;
	}
}

void rp_st_counters_text(const struct rp_status_model *m, char *buf, int len)
{
	char a[16], b[16], c[16];
	int k = 0;

	buf[0] = 0;
	if (m->have_tcpst) {
		rp_fmt_count(a, sizeof(a), m->tcp.CurrEstab);
		rp_fmt_count(b, sizeof(b), m->tcp.ActiveOpens);
		rp_fmt_count(c, sizeof(c), m->tcp.PassiveOpens);
		k = addf(buf, len, k, "Connections: %s open; %s made, %s accepted since start", a, b, c);
		rp_fmt_count(a, sizeof(a), m->tcp.RetransSegs);
		rp_fmt_count(b, sizeof(b), m->tcp.OutSegs);
		k = addf(buf, len, k, "\nResent: %s of %s segments (%lu in the last second)", a, b, (unsigned long)m->retrans_ps);
		k = addf(buf, len, k, "\nTimeouts: %lu; recoveries: %lu fast, %lu by loss probe", (unsigned long)m->tcp.TCPTimeouts,
		    (unsigned long)(m->tcp.TCPSackRecovery + m->tcp.TCPRenoRecovery),
		    (unsigned long)m->tcp.TCPLossProbeRecovery);
		k = addf(buf, len, k, "\nECN: %lu connections, %lu congestion marks", (unsigned long)m->tcp.TCPECNNegotiated,
		    (unsigned long)m->tcp.TCPECNEceRcvd);
		k = addf(buf, len, k, "\nPath MTU black holes: %lu", (unsigned long)m->tcp.TCPMTUPBlackhole);
		k = addf(buf, len, k, "\nDropped for memory: %lu", (unsigned long)m->tcp.TCPMemLimitDrops);
	}
	if (m->have_ipst)
		k = addf(buf, len, k, "%sIP: %lu packets dropped in, %lu out, %lu without a route", k ? "\n" : "",
		    (unsigned long)m->ip.InDiscards, (unsigned long)m->ip.OutDiscards, (unsigned long)m->ip.OutNoRoutes);
	if (!k)
		rt_strlcpy(buf, "No counters (Riptide is older than 0.10?)", (size_t)len);
}
