/*
 * Riptide - quality of service (see include/rt/rt_qos.h): transmit
 * priority bands per interface, socket priority and DiffServ marking,
 * and "qos rule" policies.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "infra.h"
#include "sock_internal.h"

s32 rt_qos_default = 0;
s32 rt_qos_inflight = 4;
s32 rt_qos_limit = 128;
s32 rt_qos_prio_dscp = 1;

/* the Amiga glue sets this: the calling program's name, for "qos rule
 * program=NAME" (Amiga: the CLI command name or the task name) */
void (*rt_qos_program_hook)(char *buf, u32 len);

struct qos_if {
	struct rt_bufq q[RT_QOS_NBANDS];
	struct rt_timer timer;      /* failsafe: a driver that never completes */
	struct rt_netif *nif;
	u8  running;
	u32 last_done_ms;
	u32 maxq[RT_QOS_NBANDS], packets[RT_QOS_NBANDS], bytes[RT_QOS_NBANDS], drops[RT_QOS_NBANDS];
	u32 bypass;
};

/* what rt_qos_xmit() keeps in b->cb while a frame waits */
struct qos_cb {
	u16 ethertype;
	u8  has_dst;
	u8  hwlen;
	u8  dst[RT_MAX_HWADDR];
};

/* ---- classification ---- */

/* DSCP -> band: EF, CS4..CS7, AF4x and the old TOS "low delay" (4) are
 * interactive; CS1, LE (RFC 8622), AF1x and TOS "throughput" (2) bulk */
static int dscp_band(u8 dscp)
{
	if (dscp == 46 || dscp >= 32 || dscp == 4)
		return RT_QOS_BAND_INTERACTIVE;
	if (dscp == 8 || dscp == 1 || dscp == 2 || dscp == 10 || dscp == 12 || dscp == 14)
		return RT_QOS_BAND_BULK;
	return -1;
}

/* Linux pfifo_fast priomap for priorities 0..7 */
static const u8 prio_band[8] = { 1, 2, 2, 2, 1, 2, 0, 0 };

/* transport part of a packet: proto, header at off, len bytes of it */
static int transport_band(u8 proto, const u8 *t, u32 len)
{
	switch (proto) {
	case RT_IPPROTO_ICMP:
	case 58:                    /* ICMPv6 (neighbour discovery, MLD, ping) */
	case RT_IPPROTO_IGMP:
		return RT_QOS_BAND_INTERACTIVE;
	case RT_IPPROTO_UDP:
		if (len >= 4) {
			u16 sp = rt_get_be16(t), dp = rt_get_be16(t + 2);
			if (sp == 53 || dp == 53 || sp == 123 || dp == 123 || sp == 5353 || dp == 5353 ||
			    dp == 67 || dp == 68 || dp == 546 || dp == 547)
				return RT_QOS_BAND_INTERACTIVE;
		}
		break;
	case RT_IPPROTO_TCP:
		if (len >= 20) {
			u32 thl = (u32)(t[12] >> 4) * 4u;
			/* no data: ACK, SYN, FIN, RST */
			if (thl >= 20 && len <= thl)
				return RT_QOS_BAND_INTERACTIVE;
		}
		break;
	}
	return -1;
}

int rt_qos_band(const struct rt_buf *b, u16 ethertype)
{
	const u8 *p = b->data;
	u32 len = b->len, hl;
	int band, small = 0;
	u8 dscp, proto;

	/* a VLAN interface's frame: classify what the tag carries */
	if (ethertype == RT_ETH_P_8021Q && len >= 4) {
		struct rt_buf inner = *b;
		inner.data += 4;
		inner.len -= 4;
		return rt_qos_band(&inner, rt_get_be16(p + 2));
	}
	if (ethertype == RT_ETH_P_ARP)
		return RT_QOS_BAND_INTERACTIVE;
	if (ethertype == RT_ETH_P_IP && len >= 20 && (p[0] >> 4) == 4) {
		hl = (u32)(p[0] & 15) * 4u;
		dscp = p[1] >> 2;
		proto = p[9];
		if (hl > len)
			hl = len;
		/* first fragment or no fragment: the transport header is here */
		if ((rt_get_be16(p + 6) & 0x1fffu) == 0) {
			band = transport_band(proto, p + hl, len - hl);
			if (band >= 0)
				return band;
			small = proto == RT_IPPROTO_TCP && len - hl >= 20 &&
			        len - hl - (u32)(p[hl + 12] >> 4) * 4u <= 64;
		}
	} else if (ethertype == RT_ETH_P_IPV6 && len >= 40) {
		dscp = (u8)(((p[0] & 15) << 2) | (p[1] >> 6));
		proto = p[6];
		band = transport_band(proto, p + 40, len - 40);
		if (band >= 0)
			return band;
		small = proto == RT_IPPROTO_TCP && len - 40 >= 20 && len - 40 - (u32)(p[52] >> 4) * 4u <= 64;
	} else {
		return RT_QOS_BAND_DEFAULT;
	}
	if (b->prio)
		return prio_band[(b->prio - 1) & 7];
	band = dscp_band(dscp);
	if (band >= 0)
		return band;
	return small ? RT_QOS_BAND_INTERACTIVE : RT_QOS_BAND_DEFAULT;
}

/* ---- the bands ---- */

static u32 inflight(const struct rt_netif *nif)
{
	return nif->drv ? nif->drv->tx_inflight : 0;
}

static int queued(const struct qos_if *q)
{
	return q->q[0].qlen + q->q[1].qlen + q->q[2].qlen;
}

static int xmit_now(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	if (rt_capture_on)
		rt_capture_frame(nif, b, RT_CAP_OUT, ethertype, NULL, 0);
	return nif->link_output(nif, b, dsthw, ethertype);
}

static void send_one(struct qos_if *q, int band)
{
	struct rt_buf *b = rt_bufq_dequeue(&q->q[band]);
	struct qos_cb cb;

	if (!b)
		return;
	memcpy(&cb, b->cb.b, sizeof(cb));
	q->packets[band]++;
	q->bytes[band] += b->len;
	if (xmit_now(q->nif, b, cb.has_dst ? cb.dst : NULL, cb.ethertype) < 0)
		q->nif->stats.oerrors++;
}

static int first_band(const struct qos_if *q)
{
	int i;

	for (i = 0; i < RT_QOS_NBANDS; i++)
		if (q->q[i].qlen)
			return i;
	return -1;
}

/* send what the driver may take; the failsafe timer runs while frames wait */
static void pump(struct qos_if *q)
{
	int band;

	if (q->running)
		return;
	q->running = 1;
	while ((band = first_band(q)) >= 0 && inflight(q->nif) < (u32)rt_qos_inflight)
		send_one(q, band);
	q->running = 0;
	if (!queued(q))
		rt_timer_del(&q->timer);
	else if (!rt_timer_pending(&q->timer))
		rt_timer_after(&q->timer, 250);
}

void rt_qos_run(struct rt_netif *nif)
{
	struct qos_if *q = nif->qos;

	if (!q)
		return;
	q->last_done_ms = rt_now_ms();     /* the driver made progress */
	pump(q);
}

/* A driver that stopped reporting completions must not hold the queue
 * forever: after 250 ms without one, send the next frame anyway. */
static void qos_timer(void *arg)
{
	struct qos_if *q = arg;
	int band;

	if (q->running)
		return;
	if ((band = first_band(q)) >= 0 && (s32)(rt_now_ms() - q->last_done_ms) >= 250) {
		q->running = 1;
		send_one(q, band);
		q->running = 0;
		q->last_done_ms = rt_now_ms();
	}
	pump(q);
	if (queued(q) && !rt_timer_pending(&q->timer))
		rt_timer_after(&q->timer, 250);
}

int rt_netif_tx_room(struct rt_netif *nif, u8 prio)
{
	const struct rt_drv_stats *d = nif->drv;

	if (!(nif->flags & RT_IFF_UP))
		return 1;
	if (nif->qos) {
		const struct qos_if *q = nif->qos;
		int band = prio ? prio_band[(prio - 1) & 7] : RT_QOS_BAND_DEFAULT;
		return q->q[band].qlen < (u32)rt_qos_limit;
	}
	if (!d || !d->tx_slots)
		return 1;
	return d->tx_inflight < d->tx_slots;
}

int rt_qos_xmit(struct rt_netif *nif, struct rt_buf *b, const u8 *dsthw, u16 ethertype)
{
	struct qos_if *q = nif->qos;
	struct qos_cb cb;
	int band;

	if (!queued(q) && inflight(nif) < (u32)rt_qos_inflight) {
		q->bypass++;
		return xmit_now(nif, b, dsthw, ethertype);
	}
	band = rt_qos_band(b, ethertype);
	if (q->q[band].qlen >= (u32)rt_qos_limit) {
		q->drops[band]++;
		nif->stats.iqdrops++;
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
	memset(&cb, 0, sizeof(cb));
	cb.ethertype = ethertype;
	if (dsthw) {
		cb.has_dst = 1;
		cb.hwlen = nif->hwlen;
		memcpy(cb.dst, dsthw, RT_MIN((u32)nif->hwlen, (u32)RT_MAX_HWADDR));
	}
	memcpy(b->cb.b, &cb, sizeof(cb));
	rt_bufq_tail(&q->q[band], b);
	if (q->q[band].qlen > q->maxq[band])
		q->maxq[band] = q->q[band].qlen;
	if (queued(q) == 1)
		q->last_done_ms = rt_now_ms();  /* the wait starts now */
	pump(q);
	return 0;
}

int rt_qos_enable(struct rt_netif *nif, int on)
{
	struct qos_if *q = nif->qos;
	int i;

	if (on) {
		if (q)
			return 0;
		if (!nif->drv || nif->type == RT_IFT_LOOP)
			return -RT_EOPNOTSUPP;    /* no completions to pace by */
		q = rt_zalloc(sizeof(*q));
		if (!q)
			return -RT_ENOMEM;
		for (i = 0; i < RT_QOS_NBANDS; i++)
			rt_bufq_init(&q->q[i]);
		rt_timer_init(&q->timer, qos_timer, q);
		q->nif = nif;
		nif->qos = q;
		return 0;
	}
	if (!q)
		return 0;
	nif->qos = NULL;
	rt_timer_del(&q->timer);
	/* what waits goes out in order (the driver drops what it can't take) */
	q->running = 1;
	while ((i = first_band(q)) >= 0) {
		struct rt_buf *b = rt_bufq_dequeue(&q->q[i]);
		struct qos_cb cb;
		memcpy(&cb, b->cb.b, sizeof(cb));
		if (nif->flags & RT_IFF_UP)
			xmit_now(nif, b, cb.has_dst ? cb.dst : NULL, cb.ethertype);
		else
			rt_buf_free(b);
	}
	rt_free(q);
	return 0;
}

void rt_qos_fill(const struct rt_netif *nif, struct rt_ctl_qos *r)
{
	const struct qos_if *q = nif->qos;
	int i;

	memset(r, 0, sizeof(*r));
	rt_strlcpy(r->ifname, nif->name, sizeof(r->ifname));
	r->ifindex = nif->index;
	r->limit = (u32)rt_qos_limit;
	if (!q)
		return;
	for (i = 0; i < RT_QOS_NBANDS; i++) {
		r->queued[i] = q->q[i].qlen;
		r->maxq[i] = q->maxq[i];
		r->packets[i] = q->packets[i];
		r->bytes[i] = q->bytes[i];
		r->drops[i] = q->drops[i];
	}
	r->bypass = q->bypass;
}

/* ---- socket priority and DSCP ---- */

u8 rt_qos_prio2dscp(u8 p)
{
	/* 0 best effort, 1 CS1 (background), 2 AF11, 3 AF21, 4 AF31,
	 * 5 AF41, 6 EF (interactive), 7 CS6 */
	static const u8 map[8] = { 0, 8, 10, 18, 26, 34, 46, 48 };

	return map[p & 7];
}

u8 rt_qos_sock_prio(const struct rt_socket *so)
{
	if (!so || !(so->qos_flags & (RT_SOQ_PRIO | RT_SOQ_RULE_PRIO)))
		return 0;
	return (u8)(so->priority + 1);
}

u8 rt_qos_sock_tos(const struct rt_socket *so, u8 tos)
{
	if (!so || (tos & 0xfc))
		return tos;
	if (so->qos_flags & RT_SOQ_RULE_DSCP)
		return (u8)((so->rule_dscp << 2) | (tos & 3));
	if ((so->qos_flags & (RT_SOQ_PRIO | RT_SOQ_RULE_PRIO)) && rt_qos_prio_dscp)
		return (u8)((rt_qos_prio2dscp(so->priority) << 2) | (tos & 3));
	return tos;
}

/* ---- rules ---- */

#define QOS_MAXRULES 16
#define QOS_MAXPORTS 8

struct qos_rule {
	u8  proto;                  /* 0 any, RT_IPPROTO_TCP, RT_IPPROTO_UDP */
	u8  dir;                    /* ports: 0 either end, 1 local, 2 remote */
	u8  nports;
	s8  prio;                   /* -1: leave */
	s16 dscp;                   /* -1: leave */
	u16 ports[QOS_MAXPORTS];
	char program[32];           /* "" = any */
};

static struct qos_rule rules[QOS_MAXRULES];
static int nrules;

int rt_qos_nrules(void)
{
	return nrules;
}

/* the file name part of a command ("Work:Net/AmiFTP" -> "AmiFTP") */
static const char *base_name(const char *s)
{
	const char *p = s;

	for (; *s; s++)
		if (*s == ':' || *s == '/')
			p = s + 1;
	return p;
}

static int port_match(const struct qos_rule *r, u16 lport, u16 rport)
{
	int i;

	if (!r->nports)
		return 1;
	for (i = 0; i < r->nports; i++)
		if ((r->dir != 2 && lport && r->ports[i] == lport) || (r->dir != 1 && rport && r->ports[i] == rport))
			return 1;
	return 0;
}

static void rule_apply(struct rt_socket *so, const struct qos_rule *r)
{
	if (r->prio >= 0 && !(so->qos_flags & RT_SOQ_PRIO)) {
		so->priority = (u8)r->prio;
		so->qos_flags |= RT_SOQ_RULE_PRIO;
	}
	if (r->dscp >= 0) {
		so->rule_dscp = (u8)r->dscp;
		so->qos_flags |= RT_SOQ_RULE_DSCP;
	}
}

void rt_qos_sock_rules(struct rt_socket *so, const char *program, u16 lport, u16 rport)
{
	char name[64];
	int i;

	if (!nrules || !so)
		return;
	if (!lport && !rport) {
		/* at creation: which program rules match this program */
		if (!program) {
			name[0] = 0;
			if (rt_qos_program_hook)
				rt_qos_program_hook(name, sizeof(name));
			program = name;
		}
		program = base_name(program);
		so->qos_prog = 0;
		for (i = 0; i < nrules; i++)
			if (rules[i].program[0] && !rt_strcasecmp(rules[i].program, program))
				so->qos_prog |= (u16)(1u << i);
	}
	for (i = 0; i < nrules; i++) {
		const struct qos_rule *r = &rules[i];
		if (r->program[0] && !((so->qos_prog >> i) & 1))
			continue;
		if (r->proto && r->proto != so->protocol)
			continue;
		if (r->nports ? !port_match(r, lport, rport) : (lport || rport) && r->program[0])
			continue;          /* program-only rules applied at creation */
		if (!r->nports && !r->program[0] && !r->proto)
			continue;          /* a rule that matches nothing in particular */
		rule_apply(so, r);
	}
}

static int parse_prio(const char *v, s8 *out)
{
	const char *e;
	unsigned long n;

	if (!rt_strcasecmp(v, "interactive"))
		*out = 6;
	else if (!rt_strcasecmp(v, "default") || !rt_strcasecmp(v, "normal"))
		*out = 0;
	else if (!rt_strcasecmp(v, "bulk") || !rt_strcasecmp(v, "background"))
		*out = 1;
	else {
		n = rt_strtoul(v, &e, 10);
		if (*e || e == v || n > 7)
			return -RT_EINVAL;
		*out = (s8)n;
	}
	return 0;
}

static const struct { const char *name; u8 v; } dscp_names[] = {
	{ "BE", 0 }, { "CS0", 0 }, { "LE", 1 }, { "CS1", 8 }, { "AF11", 10 }, { "AF12", 12 }, { "AF13", 14 },
	{ "CS2", 16 }, { "AF21", 18 }, { "AF22", 20 }, { "AF23", 22 }, { "CS3", 24 }, { "AF31", 26 },
	{ "AF32", 28 }, { "AF33", 30 }, { "CS4", 32 }, { "AF41", 34 }, { "AF42", 36 }, { "AF43", 38 },
	{ "CS5", 40 }, { "VA", 44 }, { "EF", 46 }, { "CS6", 48 }, { "CS7", 56 },
};

static int parse_dscp(const char *v, s16 *out)
{
	const char *e;
	unsigned long n;
	u32 i;

	for (i = 0; i < RT_ARRAY_SIZE(dscp_names); i++)
		if (!rt_strcasecmp(v, dscp_names[i].name)) {
			*out = dscp_names[i].v;
			return 0;
		}
	n = rt_strtoul(v, &e, 0);
	if (*e || e == v || n > 63)
		return -RT_EINVAL;
	*out = (s16)n;
	return 0;
}

static const char *dscp_name(u8 v, char *buf)
{
	u32 i;

	for (i = 1; i < RT_ARRAY_SIZE(dscp_names); i++)
		if (dscp_names[i].v == v)
			return dscp_names[i].name;
	rt_snprintf(buf, 8, "%u", (unsigned)v);
	return buf;
}

static int parse_ports(const char *v, struct qos_rule *r)
{
	while (*v) {
		const char *e;
		unsigned long n = rt_strtoul(v, &e, 10);
		if (e == v || n == 0 || n > 65535 || r->nports >= QOS_MAXPORTS || (*e && *e != ','))
			return -RT_EINVAL;
		r->ports[r->nports++] = (u16)n;
		v = *e ? e + 1 : e;
	}
	return r->nports ? 0 : -RT_EINVAL;
}

/* "qos rule [proto=tcp|udp] [port=P,..|lport=..|rport=..] [program=NAME]
 *  [priority=interactive|default|bulk|0-7] [dscp=EF|CS1|AF41|N]" */
static int rule_add(int argc, char **argv, char *out, u32 outlen)
{
	struct qos_rule r;
	int i;

	memset(&r, 0, sizeof(r));
	r.prio = -1;
	r.dscp = -1;
	for (i = 2; i < argc; i++) {
		const char *k = argv[i], *v = strchr(k, '=');
		u32 kl;
		if (!v)
			goto bad;
		kl = (u32)(v - k);
		v++;
		if (kl == 5 && !rt_strncasecmp(k, "proto", 5)) {
			if (!rt_strcasecmp(v, "tcp"))
				r.proto = RT_IPPROTO_TCP;
			else if (!rt_strcasecmp(v, "udp"))
				r.proto = RT_IPPROTO_UDP;
			else
				goto bad;
		} else if ((kl == 4 && !rt_strncasecmp(k, "port", 4)) || (kl == 5 && !rt_strncasecmp(k, "lport", 5)) ||
		           (kl == 5 && !rt_strncasecmp(k, "rport", 5))) {
			r.dir = kl == 4 ? 0 : (k[0] == 'l' || k[0] == 'L') ? 1 : 2;
			if (parse_ports(v, &r) < 0)
				goto bad;
		} else if (kl == 7 && !rt_strncasecmp(k, "program", 7)) {
			if (!*v || strlen(v) >= sizeof(r.program))
				goto bad;
			rt_strlcpy(r.program, base_name(v), sizeof(r.program));
		} else if ((kl == 8 && !rt_strncasecmp(k, "priority", 8)) || (kl == 4 && !rt_strncasecmp(k, "prio", 4))) {
			if (parse_prio(v, &r.prio) < 0)
				goto bad;
		} else if (kl == 4 && !rt_strncasecmp(k, "dscp", 4)) {
			if (parse_dscp(v, &r.dscp) < 0)
				goto bad;
		} else {
			goto bad;
		}
	}
	if ((!r.nports && !r.program[0]) || (r.prio < 0 && r.dscp < 0)) {
		rt_snprintf(out, outlen, "qos rule: give port=, lport=, rport= or program=, and priority= or dscp=");
		return -RT_EINVAL;
	}
	if (nrules >= QOS_MAXRULES) {
		rt_snprintf(out, outlen, "qos rule: at most %d rules", QOS_MAXRULES);
		return -RT_ENOSPC;
	}
	rules[nrules++] = r;
	return 0;
bad:
	rt_snprintf(out, outlen, "usage: qos rule [proto=tcp|udp] [port=P,..|lport=P,..|rport=P,..] "
	            "[program=NAME] [priority=interactive|default|bulk|0-7] [dscp=EF|CS1|AF41|0-63]");
	return -RT_EINVAL;
}

static u32 out_rule(char *o, u32 len, const struct qos_rule *r)
{
	u32 n = 0;
	int i;
	char b[8];

	n += (u32)rt_snprintf(o + n, len - n, "qos rule");
	if (r->proto)
		n += (u32)rt_snprintf(o + n, len - n, " proto=%s", r->proto == RT_IPPROTO_TCP ? "tcp" : "udp");
	if (r->nports) {
		n += (u32)rt_snprintf(o + n, len - n, " %s=", r->dir == 1 ? "lport" : r->dir == 2 ? "rport" : "port");
		for (i = 0; i < r->nports; i++)
			n += (u32)rt_snprintf(o + n, len - n, "%s%u", i ? "," : "", (unsigned)r->ports[i]);
	}
	if (r->program[0])
		n += (u32)rt_snprintf(o + n, len - n, " program=%s", r->program);
	if (r->prio >= 0)
		n += (u32)rt_snprintf(o + n, len - n, " priority=%s", r->prio == 6 ? "interactive" :
		                      r->prio == 1 ? "bulk" : r->prio == 0 ? "default" :
		                      (rt_snprintf(b, sizeof(b), "%d", r->prio), b));
	if (r->dscp >= 0)
		n += (u32)rt_snprintf(o + n, len - n, " dscp=%s", dscp_name((u8)r->dscp, b));
	n += (u32)rt_snprintf(o + n, len - n, "\n");
	return n;
}

static int on_off(const char *v)
{
	if (!rt_strcasecmp(v, "on") || !strcmp(v, "1"))
		return 1;
	if (!rt_strcasecmp(v, "off") || !strcmp(v, "0"))
		return 0;
	return -1;
}

static void status(char *o, u32 len)
{
	struct rt_netif *n;
	u32 k = 0;
	int i;

	k += (u32)rt_snprintf(o + k, len - k, "qos: %s by default, %ld frames in the driver, %ld per band, "
	                      "DSCP from SO_PRIORITY %s\n", rt_qos_default ? "on" : "off",
	                      (long)rt_qos_inflight, (long)rt_qos_limit, rt_qos_prio_dscp ? "on" : "off");
	for (n = rt_netif_list(); n && k < len; n = n->next) {
		struct rt_ctl_qos r;
		if (n->type == RT_IFT_LOOP)
			continue;
		if (!n->qos) {
			k += (u32)rt_snprintf(o + k, len - k, "%s: off\n", n->name);
			continue;
		}
		rt_qos_fill(n, &r);
		k += (u32)rt_snprintf(o + k, len - k, "%s: on, direct %lu", n->name, (unsigned long)r.bypass);
		for (i = 0; i < RT_QOS_NBANDS && k < len; i++)
			k += (u32)rt_snprintf(o + k, len - k, ", %s %lu (queued %lu, most %lu, dropped %lu)",
			                      i == 0 ? "interactive" : i == 1 ? "default" : "bulk",
			                      (unsigned long)r.packets[i], (unsigned long)r.queued[i],
			                      (unsigned long)r.maxq[i], (unsigned long)r.drops[i]);
		if (k < len)
			k += (u32)rt_snprintf(o + k, len - k, "\n");
	}
	for (i = 0; i < nrules && k < len; i++)
		k += out_rule(o + k, len - k, &rules[i]);
}

int rt_qos_command(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: qos on|off [IF] | inflight N | limit N | priodscp on|off | "
	                            "rule ... | rules flush | status";
	struct rt_netif *n;
	const char *e;
	unsigned long v;
	int on, r;

	if (out && outlen)
		out[0] = 0;
	if (argc < 2 || !rt_strcasecmp(argv[1], "status")) {
		status(out, outlen);
		return 0;
	}
	if ((on = on_off(argv[1])) >= 0) {
		if (argc == 2) {
			rt_qos_default = on;
			for (n = rt_netif_list(); n; n = n->next)
				if (n->drv && n->type != RT_IFT_LOOP)
					rt_qos_enable(n, on);
			return 0;
		}
		n = rt_netif_find(argv[2]);
		if (!n) {
			rt_snprintf(out, outlen, "qos: no interface '%s'", argv[2]);
			return -RT_ENXIO;
		}
		r = rt_qos_enable(n, on);
		if (r < 0)
			rt_snprintf(out, outlen, "qos: %s: %s", n->name,
			            r == -RT_EOPNOTSUPP ? "its driver reports no transmit completions" : rt_strerror(-r));
		return r;
	}
	if (argc == 3 && (!rt_strcasecmp(argv[1], "inflight") || !rt_strcasecmp(argv[1], "limit"))) {
		int inf = !rt_strcasecmp(argv[1], "inflight");
		v = rt_strtoul(argv[2], &e, 10);
		if (*e || e == argv[2] || (inf ? v < 1 || v > 32 : v < 8 || v > 1024)) {
			rt_snprintf(out, outlen, inf ? "qos inflight: 1..32 frames" : "qos limit: 8..1024 frames");
			return -RT_EINVAL;
		}
		if (inf)
			rt_qos_inflight = (s32)v;
		else
			rt_qos_limit = (s32)v;
		return 0;
	}
	if (argc == 3 && !rt_strcasecmp(argv[1], "priodscp") && (on = on_off(argv[2])) >= 0) {
		rt_qos_prio_dscp = on;
		return 0;
	}
	if (!rt_strcasecmp(argv[1], "rule"))
		return rule_add(argc, argv, out, outlen);
	if (argc == 3 && !rt_strcasecmp(argv[1], "rules") && !rt_strcasecmp(argv[2], "flush")) {
		nrules = 0;
		return 0;
	}
	rt_snprintf(out, outlen, "%s", usage);
	return -RT_EINVAL;
}

static int h_qos_default(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen);

static const struct rt_sysctl qos_sysctl[] = {
	{ "net.inet.ip.qos_default", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_qos_default, 0, 1, h_qos_default, "Transmit priority bands on interfaces (qos on|off)" },
	{ "net.inet.ip.qos_inflight", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_qos_inflight, 1, 32, NULL, "QoS: frames handed to the driver at most" },
	{ "net.inet.ip.qos_limit", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_qos_limit, 8, 1024, NULL, "QoS: frames waiting per band at most" },
	{ "net.inet.ip.qos_prio_dscp", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &rt_qos_prio_dscp, 0, 1, NULL, "Mark DSCP from SO_PRIORITY when the program sets no TOS" },
};

/* the default applies to the interfaces there are, too */
static int h_qos_default(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_netif *n;
	s32 v;

	if (old) {
		if (!oldlen || *oldlen < sizeof(s32))
			return -RT_ENOMEM;
		memcpy(old, &rt_qos_default, sizeof(s32));
	}
	if (oldlen)
		*oldlen = sizeof(s32);
	if (!newp)
		return 0;
	if (newlen != sizeof(s32))
		return -RT_EINVAL;
	memcpy(&v, newp, sizeof(v));
	if (v < 0 || v > 1)
		return -RT_EINVAL;
	rt_qos_default = v;
	for (n = rt_netif_list(); n; n = n->next)
		if (n->drv && n->type != RT_IFT_LOOP)
			rt_qos_enable(n, v);
	return 0;
}

void rt_qos_init(void)
{
	nrules = 0;
	rt_sysctl_register(qos_sysctl, (int)RT_ARRAY_SIZE(qos_sysctl));
}
