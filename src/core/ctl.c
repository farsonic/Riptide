/*
 * Riptide - control interface for the network tools (see rt_ctl.h):
 * table dumps as OPAQUE sysctls under net.riptide.* / {4, 255, n}, and
 * rt_ctl_command(), the text command interpreter behind the AmigaOS
 * "RIPTIDE" port (configuration language plus query verbs).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Everything runs with the stack lock held.
 */
#include "sock_internal.h"
#include "infra.h"

#define DUMP_HDR ((u32)sizeof(struct rt_ctl_hdr))

/* Copy header + records to the caller following the sysctl rules in
 * rt_ctl.h. 'recs' may be NULL when count is 0. */
static int dump_emit(void *old, u32 *oldlen, u16 recsize, u32 count, const void *recs)
{
	struct rt_ctl_hdr h;
	u32 need = DUMP_HDR + count * recsize, fit;

	if (!oldlen)
		return 0;
	if (!old) {
		*oldlen = need;
		return 0;
	}
	if (*oldlen < DUMP_HDR) {
		*oldlen = need;
		return -RT_ENOMEM;
	}
	fit = (*oldlen - DUMP_HDR) / recsize;
	if (fit > count)
		fit = count;
	h.version = RT_CTL_VERSION;
	h.recsize = recsize;
	h.count = fit;
	memcpy(old, &h, sizeof(h));
	if (fit)
		memcpy((u8 *)old + DUMP_HDR, recs, fit * recsize);
	if (fit < count) {
		*oldlen = need;
		return -RT_ENOMEM;
	}
	*oldlen = need;
	return 0;
}

/* ---- interfaces ---- */

static void fill_iface(struct rt_ctl_iface *r, const struct rt_netif *n)
{
	struct rt_dhcp_info di;

	memset(r, 0, sizeof(*r));
	memcpy(r->name, n->name, sizeof(r->name));
	r->name[sizeof(r->name) - 1] = 0;
	r->index = n->index;
	r->flags = n->flags;
	r->type = n->type;
	r->hwlen = n->hwlen;
	r->mtu = n->mtu;
	memcpy(r->hwaddr, n->hwaddr, RT_MIN(sizeof(r->hwaddr), sizeof(n->hwaddr)));
	r->addr = n->addr;
	r->netmask = n->netmask;
	r->broadaddr = n->broadaddr;
	r->dstaddr = n->dstaddr;
	r->metric = n->metric;
	r->ipackets = n->stats.ipackets;
	r->ierrors = n->stats.ierrors;
	r->opackets = n->stats.opackets;
	r->oerrors = n->stats.oerrors;
	r->collisions = n->stats.collisions;
	r->ibytes = n->stats.ibytes;
	r->obytes = n->stats.obytes;
	r->imcasts = n->stats.imcasts;
	r->omcasts = n->stats.omcasts;
	r->iqdrops = n->stats.iqdrops;
	r->noproto = n->stats.noproto;
	r->dhcp_state = (u16)rt_dhcp_get_info(n, &di);
	r->dhcp_server = di.server;
	r->dhcp_router = di.router;
	r->dhcp_lease_s = di.lease_s;
	r->dhcp_left_s = di.left_s;
	rt_strlcpy(r->descr, n->descr, sizeof(r->descr));
}

static int h_ifaces(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_netif *n;
	struct rt_ctl_iface *recs;
	u32 count = 0, i = 0;
	int r;

	for (n = rt_netif_list(); n; n = n->next)
		count++;
	if (!old)
		return dump_emit(NULL, oldlen, sizeof(*recs), count, NULL);
	recs = count ? rt_malloc(count * sizeof(*recs)) : NULL;
	if (count && !recs)
		return -RT_ENOMEM;
	for (n = rt_netif_list(); n && i < count; n = n->next)
		fill_iface(&recs[i++], n);
	r = dump_emit(old, oldlen, sizeof(*recs), i, recs);
	rt_free(recs);
	return r;
}

/* ---- routes ---- */

static u8 route_proto(u32 flags)
{
	if (flags & (RT_RTF_CONNECTED | RT_RTF_PMTU))
		return RT_PROTO_KERNEL;
	if (flags & RT_RTF_DYNAMIC)
		return RT_PROTO_REDIRECT;
	if (flags & RT_RTF_DHCP)
		return RT_PROTO_DHCP;
	return RT_PROTO_STATIC;
}

static int h_routes(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_route *rt;
	struct rt_ctl_route *recs, *r;
	u32 count = 0, i = 0, now = rt_now_ms();
	int ret;

	for (rt = rt_route_list(); rt; rt = rt->next)
		count++;
	if (!old)
		return dump_emit(NULL, oldlen, sizeof(*recs), count, NULL);
	recs = count ? rt_malloc(count * sizeof(*recs)) : NULL;
	if (count && !recs)
		return -RT_ENOMEM;
	for (rt = rt_route_list(); rt && i < count; rt = rt->next) {
		r = &recs[i++];
		memset(r, 0, sizeof(*r));
		r->dst = rt->dst;
		r->mask = rt->mask;
		r->gateway = rt->gateway;
		r->flags = rt->flags;
		r->metric = rt->metric;
		r->mtu = rt->mtu;
		r->use = rt->use;
		if (rt->expire_ms)
			r->expire_s = (s32)(rt->expire_ms - now) > 0 ? (rt->expire_ms - now + 999) / 1000 : 0;
		r->proto = route_proto(rt->flags);
		if (rt->netif) {
			r->ifindex = rt->netif->index;
			rt_strlcpy(r->ifname, rt->netif->name, sizeof(r->ifname));
			r->linkdown = !(rt->netif->flags & RT_IFF_UP);
		}
	}
	ret = dump_emit(old, oldlen, sizeof(*recs), i, recs);
	rt_free(recs);
	return ret;
}

/* ---- neighbours ---- */

static int h_neigh(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_arp_entry_info *ai;
	struct rt_ctl_neigh *recs;
	struct rt_netif *n;
	int count = rt_arp_list(NULL, 0), i, ret;

	if (!old)
		return dump_emit(NULL, oldlen, sizeof(*recs), (u32)count, NULL);
	if (!count)
		return dump_emit(old, oldlen, sizeof(*recs), 0, NULL);
	ai = rt_malloc((u32)count * sizeof(*ai));
	recs = rt_malloc((u32)count * sizeof(*recs));
	if (!ai || !recs) {
		rt_free(ai);
		rt_free(recs);
		return -RT_ENOMEM;
	}
	count = RT_MIN(count, rt_arp_list(ai, count));
	for (i = 0; i < count; i++) {
		memset(&recs[i], 0, sizeof(recs[i]));
		recs[i].addr = ai[i].ip;
		recs[i].ifindex = ai[i].ifindex;
		recs[i].age_s = ai[i].age_s;
		memcpy(recs[i].hwaddr, ai[i].hw, 6);
		recs[i].state = ai[i].state;
		n = rt_netif_by_index(ai[i].ifindex);
		if (n)
			rt_strlcpy(recs[i].ifname, n->name, sizeof(recs[i].ifname));
	}
	ret = dump_emit(old, oldlen, sizeof(*recs), (u32)count, recs);
	rt_free(ai);
	rt_free(recs);
	return ret;
}

/* ---- sockets ---- */

static int h_sockets(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_pcb_info *pi;
	struct rt_ctl_sock *recs;
	int nt = rt_tcp_list(NULL, 0), nu = rt_udp_list(NULL, 0), nr = rt_raw_list(NULL, 0);
	int count = nt + nu + nr, i, n = 0, ret;

	if (!old)
		return dump_emit(NULL, oldlen, sizeof(*recs), (u32)count, NULL);
	if (!count)
		return dump_emit(old, oldlen, sizeof(*recs), 0, NULL);
	pi = rt_malloc((u32)count * sizeof(*pi));
	recs = rt_malloc((u32)count * sizeof(*recs));
	if (!pi || !recs) {
		rt_free(pi);
		rt_free(recs);
		return -RT_ENOMEM;
	}
	nt = RT_MIN(nt, rt_tcp_list(pi, nt));
	nu = RT_MIN(nu, rt_udp_list(pi + nt, nu));
	nr = RT_MIN(nr, rt_raw_list(pi + nt + nu, nr));
	for (i = 0; i < nt + nu + nr; i++) {
		struct rt_ctl_sock *s = &recs[n++];
		memset(s, 0, sizeof(*s));
		s->kind = i < nt ? RT_CTL_SOCK_TCP : i < nt + nu ? RT_CTL_SOCK_UDP : RT_CTL_SOCK_RAW;
		s->proto = pi[i].proto;
		s->state = pi[i].state;
		s->lport = pi[i].lport;
		s->fport = pi[i].fport;
		s->laddr = pi[i].laddr;
		s->faddr = pi[i].faddr;
		s->sendq = pi[i].sendq;
		s->recvq = pi[i].recvq;
		s->rtt_us = pi[i].rtt_us;
		s->cwnd = pi[i].cwnd;
		s->retrans = pi[i].retrans;
	}
	ret = dump_emit(old, oldlen, sizeof(*recs), (u32)n, recs);
	rt_free(pi);
	rt_free(recs);
	return ret;
}

/* ---- resolver and statistics ---- */

static int h_dns(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	struct rt_ctl_dns d;
	int n;

	memset(&d, 0, sizeof(d));
	n = rt_dns_get_servers(d.servers, RT_CTL_MAXNS);
	d.nservers = (u32)RT_MIN(n, RT_CTL_MAXNS);
	rt_strlcpy(d.domain, rt_domainname, sizeof(d.domain));
	rt_strlcpy(d.hostname, rt_hostname, sizeof(d.hostname));
	return dump_emit(old, oldlen, sizeof(d), 1, &d);
}

static int h_stats(const struct rt_sysctl *e, void *old, u32 *oldlen, const void *newp, u32 newlen)
{
	return dump_emit(old, oldlen, (u16)e->max, 1, e->ptr);
}

#define DUMP(name, n, fn, d) \
	{ "net.riptide." name, RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 3, { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, n }, \
	  NULL, 0, 0, fn, d }
#define STATS(name, n, var) \
	{ "net.riptide." name, RT_SYSCTL_OPAQUE, RT_SYSCTL_RO, 3, { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, n }, \
	  &var, 0, sizeof(var), h_stats, "rt_ctl_hdr + one struct " #var }

static const struct rt_sysctl ctl_sysctls[] = {
	DUMP("ifaces", RT_CTL_IFACES, h_ifaces, "rt_ctl_hdr + struct rt_ctl_iface[]"),
	DUMP("routes", RT_CTL_ROUTES, h_routes, "rt_ctl_hdr + struct rt_ctl_route[]"),
	DUMP("neigh", RT_CTL_NEIGH, h_neigh, "rt_ctl_hdr + struct rt_ctl_neigh[]"),
	DUMP("sockets", RT_CTL_SOCKETS, h_sockets, "rt_ctl_hdr + struct rt_ctl_sock[]"),
	DUMP("dns", RT_CTL_DNS, h_dns, "rt_ctl_hdr + one struct rt_ctl_dns"),
	STATS("ipstat", RT_CTL_IPSTAT, rt_ipstat),
	STATS("icmpstat", RT_CTL_ICMPSTAT, rt_icmpstat),
	STATS("udpstat", RT_CTL_UDPSTAT, rt_udpstat),
	STATS("tcpstat", RT_CTL_TCPSTAT, rt_tcpstat),
};

void rt_ctl_init(void)
{
	rt_sysctl_register(ctl_sysctls, (int)RT_ARRAY_SIZE(ctl_sysctls));
}

/* ======================================================================
 * Text commands
 * ====================================================================== */

struct outbuf {
	char *p;
	u32 len, used;
	int truncated;
};

static void out_add(struct outbuf *o, const char *fmt, ...) RT_PRINTF(2, 3);
static void out_add(struct outbuf *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->used + 1 >= o->len) {
		o->truncated = 1;
		return;
	}
	va_start(ap, fmt);
	n = rt_vsnprintf(o->p + o->used, o->len - o->used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((u32)n >= o->len - o->used) {
		o->used = o->len - 1;
		o->truncated = 1;
	} else {
		o->used += (u32)n;
	}
}

static void sysctl_print(struct outbuf *o, const struct rt_sysctl *e, int value_only)
{
	char v[96];

	if (rt_sysctl_get_text(e, v, sizeof(v)) < 0)
		v[0] = 0;
	if (value_only)
		out_add(o, "%s\n", v);
	else
		out_add(o, "%s = %s\n", e->name, v);
}

/* sysctl [-n] [-a] [-w] [NAME[=VALUE]...]: a NAME that is a prefix of
 * several entries ("net.inet.tcp") lists them all, like Linux sysctl. */
static int cmd_sysctl(int argc, char **argv, struct outbuf *o)
{
	const struct rt_sysctl *e;
	int i, k, all = 0, value_only = 0, any = 0, r;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		const char *f = argv[i] + 1;
		for (; *f; f++) {
			if (*f == 'a' || *f == 'A' || *f == 'X')
				all = 1;
			else if (*f == 'n')
				value_only = 1;
			else if (*f == 'w' || *f == 'q' || *f == 'e')
				;
			else {
				out_add(o, "sysctl: unknown option '-%c'\n", *f);
				return -RT_EINVAL;
			}
		}
	}
	if (all || i == argc) {
		for (k = 0; (e = rt_sysctl_iter(k)) != NULL; k++)
			if (e->type != RT_SYSCTL_OPAQUE)
				sysctl_print(o, e, value_only);
		return 0;
	}
	for (; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (eq) {
			*eq = 0;
			r = rt_sysctl_set_text(argv[i], eq + 1);
			if (r == -RT_ENOENT) {
				out_add(o, "sysctl: cannot stat %s: No such file or directory\n", argv[i]);
				return r;
			}
			if (r == -RT_EPERM) {
				out_add(o, "sysctl: setting key \"%s\": Read-only\n", argv[i]);
				return r;
			}
			if (r < 0) {
				out_add(o, "sysctl: setting key \"%s\": Invalid argument\n", argv[i]);
				return r;
			}
			e = rt_sysctl_find(argv[i]);
			if (e)
				sysctl_print(o, e, value_only);
			continue;
		}
		e = rt_sysctl_find(argv[i]);
		if (e) {
			sysctl_print(o, e, value_only);
			continue;
		}
		/* prefix listing */
		any = 0;
		for (k = 0; (e = rt_sysctl_iter(k)) != NULL; k++) {
			u32 l = (u32)strlen(argv[i]);
			if (!strncmp(e->name, argv[i], l) && e->name[l] == '.' &&
			    e->type != RT_SYSCTL_OPAQUE) {
				sysctl_print(o, e, value_only);
				any = 1;
			}
		}
		if (!any) {
			out_add(o, "sysctl: cannot stat %s: No such file or directory\n", argv[i]);
			return -RT_ENOENT;
		}
	}
	return 0;
}

#define CTL_MAXARGS 24

int rt_ctl_command(const char *line, char *out, u32 outlen,
                   int (*unknown)(int argc, char **argv, char *err, u32 errlen))
{
	struct outbuf o;
	char *argv[CTL_MAXARGS], *copy;
	u32 len;
	int argc, r;

	if (!out || !outlen)
		return -RT_EINVAL;
	out[0] = 0;
	o.p = out;
	o.len = outlen;
	o.used = 0;
	o.truncated = 0;
	if (!line)
		return 0;
	len = (u32)strlen(line);
	copy = rt_malloc(len + 1);
	if (!copy)
		return -RT_ENOMEM;
	memcpy(copy, line, len + 1);
	argc = rt_config_split(copy, argv, CTL_MAXARGS);
	if (argc && !strcmp(argv[0], "sysctl")) {
		r = cmd_sysctl(argc, argv, &o);
		rt_free(copy);
		return r;
	}
	rt_free(copy);
	r = rt_config_line(line, out, outlen, unknown);
	if (r < 0) {
		if (!out[0])
			rt_strlcpy(out, rt_strerror(-r), outlen);
		len = (u32)strlen(out);
		if (len + 2 <= outlen && out[len - 1] != '\n') {
			out[len] = '\n';
			out[len + 1] = 0;
		}
	}
	return r;
}
