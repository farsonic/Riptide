/*
 * Riptide - DHCP client (RFC 2131 / RFC 2132).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * All entry points are called with the stack lock held; everything else
 * runs from timers in the stack thread.
 *
 * Until the interface has an address, messages are built here as complete
 * IP/UDP datagrams from 0.0.0.0:68 to 255.255.255.255:67 and handed to
 * rt_netif_xmit() (dsthw NULL = link broadcast). Renewals go unicast to
 * the server through rt_ip_output(). Replies are received on one shared
 * kernel UDP socket bound to INADDR_ANY:68 (ip.c accepts 255.255.255.255
 * on any interface, configured or not); we always set the BROADCAST flag
 * so servers broadcast OFFER/ACK while we cannot receive unicast.
 *
 * Classless static routes (option 121, RFC 3442, and Microsoft's 249 when
 * 121 is absent) are installed as DHCP routes; with them option 3 is
 * ignored and the default route is the 0.0.0.0/0 entry, if any (RFC 3442
 * says so). The domain search list (option 119, RFC 3397, compressed
 * names) feeds the resolver's search list unless one was configured;
 * option 15 stays the domain name. Repeated options are concatenated
 * (RFC 3396) for these three.
 *
 * Without a lease after net.inet.ip.linklocal_wait seconds (default 10)
 * ipv4ll.c picks a 169.254/16 address (RFC 3927) while DISCOVERs go on; a
 * lease replaces it. "dhcp IF linklocal off" turns this off per interface.
 *
 * Not implemented: INIT-REBOOT, ARP probe/DECLINE, option overload (52).
 */
#include "sock_internal.h"
#include "infra.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u
#define DHCP_FIXED_LEN   236
#define DHCP_MIN_LEN     300          /* BOOTP minimum (RFC 1542) */
#define DHCP_MAX_LEN     576
#define DHCP_RXBUF       1500

/* message types (option 53) */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7

/* options */
#define OPT_PAD        0
#define OPT_MASK       1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_HOSTNAME   12
#define OPT_DOMAIN     15
#define OPT_SEARCH     119            /* RFC 3397 */
#define OPT_CLASSLESS  121            /* RFC 3442 */
#define OPT_MSCLASSLESS 249           /* Microsoft's pre-RFC 121 */
#define OPT_MTU        26
#define OPT_NTP        42
#define OPT_REQIP      50
#define OPT_LEASE      51
#define OPT_MSGTYPE    53
#define OPT_SERVERID   54
#define OPT_PARAMS     55
#define OPT_MAXSIZE    57
#define OPT_T1         58
#define OPT_T2         59
#define OPT_CLIENTID   61
#define OPT_END        255

enum { ST_INIT, ST_SELECTING, ST_REQUESTING, ST_BOUND, ST_RENEWING, ST_REBINDING };

#define DHCP_MAX_DNS 3
#define DHCP_DAY_S   86400u
#define DHCP_CAT_MAX 512              /* concatenated 119/121/249 data */

struct dhcp_opts {
	u8  type;
	u32 mask, router, server, lease, t1, t2;
	u32 dns[DHCP_MAX_DNS];
	u8  ndns;
	u32 ntp[4];
	u8  nntp;
	u16 mtu;
	char domain[64];
	char hostname[64];
	/* raw option data, concatenated (RFC 3396) */
	u16 lsearch, lcls, lmscls;
	u8  search[DHCP_CAT_MAX];
	u8  cls[DHCP_CAT_MAX];
	u8  mscls[DHCP_CAT_MAX];
};

struct dhcp_client {
	struct dhcp_client *next;
	struct rt_netif *nif;
	void (*cb)(struct rt_netif *nif, int event);
	struct rt_timer timer;
	u8  state;
	u8  chlen;
	u8  chaddr[16];
	u32 xid;
	u32 tries;
	u32 start_ms;               /* for the secs field */
	/* offer / lease */
	u32 offered, server;
	u32 addr, mask, router;
	u32 dns[DHCP_MAX_DNS];
	u8  ndns;
	u8  set_domain;             /* we set rt_domainname */
	u8  set_search;             /* we set the resolver search list */
	u8  nroutes;                /* classless routes in routes[] */
	u8  ncls;                   /* option 121/249 entries incl. 0/0 */
	struct rt_dhcp_route routes[RT_DHCP_MAX_ROUTES];   /* not 0/0 */
	char search[RT_DHCP_SEARCH_LEN];                    /* the list we set */
	s8  ll_on;                  /* link-local: 1/0, -1 = sysctl */
	u16 ll_wait_s;              /* 0 = sysctl */
	struct rt_timer ll_timer;   /* no lease yet: start ipv4ll.c */
	u32 lease_s, t1_s, t2_s;    /* relative to lease start */
	u32 acc_s, acc_ms;          /* seconds since lease start + base time */
};

static struct dhcp_client *dhcp_clients;
static struct rt_socket *dhcp_so;
static struct rt_timer dhcp_rx_timer;
static u8 *dhcp_rxbuf;
static struct sock_upcall dhcp_upcall;

static void dhcp_timeout(void *arg);

/* ---------------------------------------------------------------- utils */

static struct dhcp_client *client_of(const struct rt_netif *nif)
{
	struct dhcp_client *c;

	for (c = dhcp_clients; c; c = c->next)
		if (c->nif == nif)
			return c;
	return NULL;
}

/* RFC 2131 4.1: 4, 8, 16, 32, 64 s, randomised by +-1 s */
static u32 backoff_ms(u32 tries)
{
	u32 base = tries >= 4 ? 64u : 4u << tries;

	return base * 1000u - 1000u + rt_random32() % 2001u;
}

/* Advance the lease clock (rt_now_ms() wraps, leases may be long). */
static u32 lease_elapsed(struct dhcp_client *c)
{
	u32 d = (rt_now_ms() - c->acc_ms) / 1000u;

	c->acc_s += d;
	c->acc_ms += d * 1000u;
	return c->acc_s;
}

static void arm_s(struct dhcp_client *c, u32 secs)
{
	if (secs > DHCP_DAY_S)
		secs = DHCP_DAY_S;     /* re-evaluated when it fires */
	rt_timer_after(&c->timer, secs * 1000u);
}

/* ---------------------------------------------------------------- tx */

static u8 *opt_put(u8 *p, u8 code, u8 len, const void *data)
{
	*p++ = code;
	*p++ = len;
	memcpy(p, data, len);
	return p + len;
}

/* Build the BOOTP message for 'type' into a new buffer (data at BOOTP). */
static struct rt_buf *dhcp_build(struct dhcp_client *c, u8 type, u32 ciaddr)
{
	/* RFC 3442: 121 before 3, for servers that send options in request
	 * order */
	static const u8 params[] = { OPT_MASK, OPT_CLASSLESS, OPT_ROUTER, OPT_DNS, OPT_HOSTNAME,
	                             OPT_DOMAIN, OPT_SEARCH, OPT_MTU, OPT_NTP, OPT_LEASE,
	                             OPT_SERVERID, OPT_T1, OPT_T2, OPT_MSCLASSLESS };
	struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, DHCP_MAX_LEN);
	u8 *m, *p, v8[17];
	u32 secs, len, hl;
	u16 v16;

	if (!b)
		return NULL;
	m = b->data;
	memset(m, 0, DHCP_MAX_LEN);
	m[0] = 1;                                  /* BOOTREQUEST */
	m[1] = 1;                                  /* htype ethernet */
	m[2] = c->chlen;
	rt_put_be32(m + 4, c->xid);
	secs = (rt_now_ms() - c->start_ms) / 1000u;
	rt_put_be16(m + 8, (u16)(secs > 0xffff ? 0xffff : secs));
	if (!ciaddr)
		rt_put_be16(m + 10, 0x8000);           /* BROADCAST flag */
	memcpy(m + 12, &ciaddr, 4);
	memcpy(m + 28, c->chaddr, c->chlen);
	rt_put_be32(m + 236, DHCP_MAGIC);
	p = m + 240;
	p = opt_put(p, OPT_MSGTYPE, 1, &type);
	v8[0] = 1;
	memcpy(v8 + 1, c->chaddr, c->chlen);
	p = opt_put(p, OPT_CLIENTID, (u8)(1 + c->chlen), v8);
	if (type == DHCPREQUEST && c->state == ST_REQUESTING) {
		p = opt_put(p, OPT_REQIP, 4, &c->offered);
		p = opt_put(p, OPT_SERVERID, 4, &c->server);
	}
	if (type == DHCPRELEASE) {
		p = opt_put(p, OPT_SERVERID, 4, &c->server);
	} else {
		rt_put_be16(&v16, DHCP_MAX_LEN);
		p = opt_put(p, OPT_MAXSIZE, 2, &v16);
		hl = (u32)strlen(rt_hostname);
		if (hl && hl < 64) {
			/* only the host part (RFC 4702 suggests no domain here) */
			const char *dot = strchr(rt_hostname, '.');
			if (dot)
				hl = (u32)(dot - rt_hostname);
			if (hl)
				p = opt_put(p, OPT_HOSTNAME, (u8)hl, rt_hostname);
		}
		p = opt_put(p, OPT_PARAMS, (u8)sizeof(params), params);
	}
	*p++ = OPT_END;
	len = (u32)(p - m);
	if (len < DHCP_MIN_LEN)
		len = DHCP_MIN_LEN;
	rt_buf_put(b, len);
	return b;
}

static void udp_header(struct rt_buf *b, u32 saddr, u32 daddr)
{
	u8 *u = rt_buf_push(b, 8);
	u16 ck;

	rt_put_be16(u, DHCP_CLIENT_PORT);
	rt_put_be16(u + 2, DHCP_SERVER_PORT);
	rt_put_be16(u + 4, (u16)b->len);
	u[6] = u[7] = 0;
	ck = rt_csum_fold(rt_csum_partial(u, b->len,
	                  rt_csum_pseudo(saddr, daddr, RT_IPPROTO_UDP, (u16)b->len)));
	if (ck == 0)
		ck = 0xffff;
	memcpy(u + 6, &ck, 2);
}

/* Straight to the link as a link-level broadcast: from 0.0.0.0 (or ciaddr
 * when rebinding) to 255.255.255.255, or the RELEASE to the server (no ARP
 * needed while the address is being removed). */
static int send_link(struct dhcp_client *c, struct rt_buf *b, u32 saddr, u32 daddr)
{
	struct rt_netif *nif = c->nif;
	u8 *ip;
	u16 ck;

	udp_header(b, saddr, daddr);
	ip = rt_buf_push(b, 20);
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[1] = 0x10;                              /* low delay, as dhclient */
	rt_put_be16(ip + 2, (u16)b->len);
	rt_put_be16(ip + 4, rt_ip_next_id());
	ip[8] = 64;
	ip[9] = RT_IPPROTO_UDP;
	memcpy(ip + 12, &saddr, 4);
	memcpy(ip + 16, &daddr, 4);
	ck = rt_inet_csum(ip, 20);
	memcpy(ip + 10, &ck, 2);
	if (!nif->link_output) {
		rt_buf_free(b);
		return -RT_ENETDOWN;
	}
	nif->stats.opackets++;
	nif->stats.obytes += b->len;
	return rt_netif_xmit(nif, b, NULL, RT_ETH_P_IP);
}

static int send_unicast(struct dhcp_client *c, struct rt_buf *b)
{
	struct rt_ip_out o;

	udp_header(b, c->addr, c->server);
	memset(&o, 0, sizeof(o));
	o.saddr = c->addr;
	o.daddr = c->server;
	o.proto = RT_IPPROTO_UDP;
	return rt_ip_output(b, &o);
}

static void dhcp_send(struct dhcp_client *c, u8 type)
{
	int unicast = type != DHCPRELEASE && c->state == ST_RENEWING;
	u32 ciaddr = (c->state == ST_RENEWING || c->state == ST_REBINDING ||
	              type == DHCPRELEASE) ? c->addr : 0;

	RT_DBG(RT_DBG_DHCP, "%s: send message type %u%s, try %u", c->nif->name, (unsigned)type,
	       unicast ? " (unicast)" : "", (unsigned)c->tries);
	struct rt_buf *b = dhcp_build(c, type, ciaddr);

	if (!b)
		return;
	if (unicast)
		send_unicast(c, b);
	else
		send_link(c, b, ciaddr, type == DHCPRELEASE ? c->server : RT_INADDR_BROADCAST);
}

/* ---------------------------------------------------------------- state */

static void notify(struct dhcp_client *c, int ev)
{
	if (c->cb)
		c->cb(c->nif, ev);
}

static int route_in(const struct rt_dhcp_route *r, const struct rt_dhcp_route *list, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (list[i].dst == r->dst && list[i].mask == r->mask && list[i].gw == r->gw)
			return 1;
	return 0;
}

/* Remove our classless routes that are not in keep[] */
static void routes_remove(struct dhcp_client *c, const struct rt_dhcp_route *keep, int nkeep)
{
	int i, n = 0;

	for (i = 0; i < c->nroutes; i++) {
		const struct rt_dhcp_route *r = &c->routes[i];
		if (route_in(r, keep, nkeep))
			c->routes[n++] = *r;
		else
			rt_route_delete_match(r->dst, r->mask, r->gw, c->nif, RT_RTF_DHCP);
	}
	c->nroutes = (u8)n;
}

/* Our search list goes with the lease, unless it was replaced since */
static void search_drop(struct dhcp_client *c)
{
	if (c->set_search && rt_dns_search_is_auto() && !strcmp(rt_dns_search_list(), c->search))
		rt_dns_set_search_auto("");
	c->set_search = 0;
	c->search[0] = 0;
}

static int ll_enabled(const struct dhcp_client *c)
{
	return c->ll_on < 0 ? rt_ll_enable != 0 : c->ll_on != 0;
}

/* No lease yet: a link-local address after linklocal_wait seconds */
static void ll_arm(struct dhcp_client *c)
{
	u8 st;

	rt_ipv4ll_info(c->nif, &st, NULL);
	if (!ll_enabled(c) || st != RT_LL_OFF || rt_timer_pending(&c->ll_timer))
		return;
	rt_timer_after(&c->ll_timer, (c->ll_wait_s ? c->ll_wait_s : (u32)rt_ll_wait_s) * 1000u);
}

static void ll_timeout(void *arg)
{
	struct dhcp_client *c = arg;

	if (c->state < ST_BOUND && ll_enabled(c))
		rt_ipv4ll_start(c->nif);
}

static void unconfigure(struct dhcp_client *c)
{
	int i;

	if (!c->addr)
		return;
	if (c->router)   /* only our own route: a static one stays */
		rt_route_delete_match(0, 0, c->router, c->nif, RT_RTF_DHCP);
	routes_remove(c, NULL, 0);
	c->ncls = 0;
	for (i = 0; i < c->ndns; i++)
		rt_dns_remove_server(c->dns[i]);
	if (c->set_domain)
		rt_domainname[0] = 0;
	search_drop(c);
	rt_ntp_dhcp_servers(c->nif, NULL, 0);
	if (c->nif->addr == c->addr)
		rt_netif_set_addr(c->nif, 0, 0, 0);
	c->addr = c->router = 0;
	c->ndns = 0;
	c->set_domain = 0;
}

static void start_init(struct dhcp_client *c, u32 delay_ms)
{
	c->state = ST_INIT;
	RT_DBG(RT_DBG_DHCP, "%s: state INIT", c->nif->name);
	c->tries = 0;
	c->xid = rt_random32();
	rt_timer_after(&c->timer, delay_ms);
	ll_arm(c);
}

static void lost(struct dhcp_client *c)
{
	int was = c->addr != 0;

	unconfigure(c);
	if (was) {
		rt_log(RT_LOG_WARN, "dhcp: %s: lease lost", c->nif->name);
		notify(c, RT_DHCP_EV_LOST);
	}
	start_init(c, 500);
}

static u32 classful(u32 a)
{
	u32 h = rt_ntohl(a);

	return rt_htonl((h & 0x80000000u) == 0 ? 0xff000000u :
	                (h & 0xc0000000u) == 0x80000000u ? 0xffff0000u : 0xffffff00u);
}

static void bind_lease(struct dhcp_client *c, u32 yiaddr, const struct dhcp_opts *o)
{
	struct rt_netif *nif = c->nif;
	u32 mask = o->mask ? o->mask : classful(yiaddr), router = o->router;
	int renew = c->addr != 0 && c->addr == yiaddr, i, j, ncls = -1, nr = 0;
	struct rt_dhcp_route cls[RT_DHCP_MAX_ROUTES];
	char a[16], *search;

	/* RFC 3442: with classless routes option 3 is ignored; a malformed
	 * option counts as absent */
	if (o->lcls)
		ncls = rt_dhcp_parse_classless(o->cls, o->lcls, cls, RT_DHCP_MAX_ROUTES);
	else if (o->lmscls)
		ncls = rt_dhcp_parse_classless(o->mscls, o->lmscls, cls, RT_DHCP_MAX_ROUTES);
	if (ncls >= 0) {
		router = 0;
		for (i = 0; i < ncls; i++) {
			if (cls[i].mask == 0) {
				if (!router && cls[i].gw)
					router = cls[i].gw;
			} else {
				cls[nr++] = cls[i];
			}
		}
	}

	rt_timer_del(&c->ll_timer);
	rt_ipv4ll_stop(nif);       /* a lease replaces the link-local address */
	if (c->addr && (c->addr != yiaddr || c->mask != mask || c->router != router))
		unconfigure(c);     /* parameters changed: start over */
	if (!c->addr || nif->addr != yiaddr) {
		if (rt_netif_set_addr(nif, yiaddr, mask,
		                      nif->type == RT_IFT_PTP ? o->router : 0) < 0) {
			rt_log(RT_LOG_ERR, "dhcp: %s: cannot set address", nif->name);
			lost(c);
			return;
		}
		if (!(nif->flags & RT_IFF_UP))
			rt_netif_up(nif);
	}
	c->addr = yiaddr;
	c->mask = mask;
	if (o->mtu >= 68 && o->mtu <= nif->mtu) {
		nif->mtu = o->mtu;
		rt_vlan_parent_changed(nif);
	}
	if (router) {
		/* (Re)install on every ACK: the route may have been removed by
		 * hand. A static default route still wins (route.c orders DHCP
		 * routes last), and one with the same gateway makes this fail. */
		c->router = router;
		rt_route_add(0, 0, router, nif, RT_RTF_UP | RT_RTF_DHCP, 0);
	}
	/* classless routes: drop those that went away, (re)add the rest in
	 * option order (a router may be reachable only through an earlier
	 * one); router 0.0.0.0 means on-link */
	routes_remove(c, cls, nr);
	for (i = 0; i < nr; i++) {
		int r = rt_route_add(cls[i].dst, cls[i].mask, cls[i].gw, nif, RT_RTF_UP | RT_RTF_DHCP, 0);
		if (r < 0 && r != -RT_EEXIST)
			rt_log(RT_LOG_WARN, "dhcp: %s: cannot add route to %s", nif->name,
			       rt_format_ipv4(cls[i].dst, a));
		else if (!route_in(&cls[i], c->routes, c->nroutes))
			c->routes[c->nroutes++] = cls[i];
	}
	c->ncls = ncls > 0 ? (u8)ncls : 0;
	/* DNS servers: drop ours that disappeared, add new ones */
	for (i = 0; i < c->ndns; i++) {
		for (j = 0; j < o->ndns; j++)
			if (c->dns[i] == o->dns[j])
				break;
		if (j == o->ndns)
			rt_dns_remove_server(c->dns[i]);
	}
	c->ndns = 0;
	for (j = 0; j < o->ndns; j++)
		if (rt_dns_add_server(o->dns[j]) == 0)
			c->dns[c->ndns++] = o->dns[j];
	if (o->domain[0] && (!rt_domainname[0] || c->set_domain)) {
		rt_strlcpy(rt_domainname, o->domain, sizeof(rt_domainname));
		c->set_domain = 1;
	}
	/* search list: option 119, else the domain name; one configured by
	 * hand ("domain", "search") stays */
	search = rt_zalloc(RT_DHCP_SEARCH_LEN);
	if (search) {
		if (!o->lsearch || rt_dhcp_parse_search(o->search, o->lsearch, search,
		                                        RT_DHCP_SEARCH_LEN) <= 0)
			rt_strlcpy(search, o->domain, RT_DHCP_SEARCH_LEN);
		if (search[0]) {
			rt_dns_set_search_auto(search);    /* not over a static one */
			if (rt_dns_search_is_auto()) {
				rt_strlcpy(c->search, rt_dns_search_list(), sizeof(c->search));
				c->set_search = 1;
			}
		} else {
			search_drop(c);
		}
		rt_free(search);
	}
	rt_ntp_dhcp_servers(nif, o->ntp, o->nntp);
	if (o->hostname[0] && !rt_hostname[0])
		rt_strlcpy(rt_hostname, o->hostname, sizeof(rt_hostname));

	/* lease timers (RFC 2131 4.4.5 defaults) */
	c->server = o->server;
	c->lease_s = o->lease ? o->lease : 3600u;
	c->t1_s = o->t1 ? o->t1 : c->lease_s / 2u;
	c->t2_s = o->t2 ? o->t2 : (u32)(((u64)c->lease_s * 7u) / 8u);
	if (c->t2_s > c->lease_s || c->t2_s == 0)
		c->t2_s = (u32)(((u64)c->lease_s * 7u) / 8u);
	if (c->t1_s > c->t2_s)
		c->t1_s = c->lease_s / 2u;
	c->acc_s = 0;
	c->acc_ms = rt_now_ms();
	c->state = ST_BOUND;
	RT_DBG(RT_DBG_DHCP, "%s: state BOUND", c->nif->name);
	c->tries = 0;
	rt_timer_del(&c->timer);
	if (c->lease_s != 0xffffffffu)
		arm_s(c, c->t1_s);
	rt_log(RT_LOG_INFO, "dhcp: %s: bound to %s, lease %u s", nif->name,
	       rt_format_ipv4(yiaddr, a), (unsigned)c->lease_s);
	notify(c, renew ? RT_DHCP_EV_RENEWED : RT_DHCP_EV_BOUND);
}

static void dhcp_timeout(void *arg)
{
	struct dhcp_client *c = arg;
	u32 el, left;

	switch (c->state) {
	case ST_INIT:
	case ST_SELECTING:
		if (c->state == ST_INIT) {
			c->start_ms = rt_now_ms();
			c->state = ST_SELECTING;
			RT_DBG(RT_DBG_DHCP, "%s: state SELECTING", c->nif->name);
			if (!(c->nif->flags & RT_IFF_UP))
				rt_netif_up(c->nif);
		}
		dhcp_send(c, DHCPDISCOVER);
		rt_timer_after(&c->timer, backoff_ms(c->tries++));
		return;
	case ST_REQUESTING:
		if (c->tries >= 4) {          /* no ACK: back to discovery */
			start_init(c, 0);
			return;
		}
		dhcp_send(c, DHCPREQUEST);
		rt_timer_after(&c->timer, backoff_ms(c->tries++));
		return;
	case ST_BOUND:
	case ST_RENEWING:
	case ST_REBINDING:
		el = lease_elapsed(c);
		if (el >= c->lease_s) {
			lost(c);
			return;
		}
		if (el >= c->t2_s) {
			if (c->state != ST_REBINDING)
				c->xid = rt_random32();
			c->state = ST_REBINDING;
			RT_DBG(RT_DBG_DHCP, "%s: state REBINDING", c->nif->name);
			left = c->lease_s - el;
		} else if (el >= c->t1_s) {
			if (c->state == ST_BOUND)
				c->xid = rt_random32();
			c->state = ST_RENEWING;
			RT_DBG(RT_DBG_DHCP, "%s: state RENEWING", c->nif->name);
			left = c->t2_s - el;
		} else {
			arm_s(c, c->t1_s - el);    /* long lease: not yet */
			return;
		}
		dhcp_send(c, DHCPREQUEST);
		/* RFC 2131 4.4.5: wait half the remaining time, at least 60 s */
		arm_s(c, RT_MIN(left, RT_MAX(left / 2u, 60u)));
		return;
	}
}

/* ---------------------------------------------------------------- rx */

static void parse_options(const u8 *p, u32 len, struct dhcp_opts *o)
{
	u32 i = 0;

	while (i < len) {
		u8 code = p[i], l;
		const u8 *v;
		if (code == OPT_PAD) {
			i++;
			continue;
		}
		if (code == OPT_END || i + 1 >= len)
			break;
		l = p[i + 1];
		if (i + 2 + l > len)
			break;
		v = p + i + 2;
		switch (code) {
		case OPT_MSGTYPE:  if (l == 1) o->type = v[0]; break;
		case OPT_MASK:     if (l == 4) memcpy(&o->mask, v, 4); break;
		case OPT_ROUTER:   if (l >= 4) memcpy(&o->router, v, 4); break;
		case OPT_SERVERID: if (l == 4) memcpy(&o->server, v, 4); break;
		case OPT_LEASE:    if (l == 4) o->lease = rt_get_be32(v); break;
		case OPT_T1:       if (l == 4) o->t1 = rt_get_be32(v); break;
		case OPT_T2:       if (l == 4) o->t2 = rt_get_be32(v); break;
		case OPT_MTU:      if (l == 2) o->mtu = rt_get_be16(v); break;
		case OPT_DNS:
		case OPT_NTP: {
			u32 k, *out = code == OPT_DNS ? o->dns : o->ntp;
			u8 *n = code == OPT_DNS ? &o->ndns : &o->nntp;
			u32 max = code == OPT_DNS ? DHCP_MAX_DNS : 4;
			for (k = 0; k + 4 <= l && *n < max; k += 4) {
				u32 a;
				memcpy(&a, v + k, 4);
				if (a && a != RT_INADDR_BROADCAST)
					out[(*n)++] = a;
			}
			break;
		}
		case OPT_SEARCH:
		case OPT_CLASSLESS:
		case OPT_MSCLASSLESS: {
			/* RFC 3396: repeated instances are concatenated */
			u8 *buf = code == OPT_SEARCH ? o->search : code == OPT_CLASSLESS ? o->cls : o->mscls;
			u16 *bl = code == OPT_SEARCH ? &o->lsearch : code == OPT_CLASSLESS ? &o->lcls : &o->lmscls;
			if (*bl + l <= DHCP_CAT_MAX) {
				memcpy(buf + *bl, v, l);
				*bl = (u16)(*bl + l);
			}
			break;
		}
		case OPT_DOMAIN:
		case OPT_HOSTNAME: {
			char *dst = code == OPT_DOMAIN ? o->domain : o->hostname;
			u32 k, n = l < 63 ? l : 63;
			for (k = 0; k < n && v[k] > 0x20 && v[k] < 0x7f; k++)
				dst[k] = (char)v[k];
			dst[k] = 0;
			if (k && dst[k - 1] == '.')
				dst[k - 1] = 0;
			break;
		}
		}
		i += 2u + l;
	}
}

static void dhcp_input(const u8 *m, u32 len, u32 from)
{
	struct dhcp_opts *o;
	struct dhcp_client *c;
	u32 xid, yiaddr;

	if (len < 240 || m[0] != 2 || rt_get_be32(m + 236) != DHCP_MAGIC)
		return;
	xid = rt_get_be32(m + 4);
	for (c = dhcp_clients; c; c = c->next)
		if (c->xid == xid && m[2] == c->chlen && !memcmp(m + 28, c->chaddr, c->chlen))
			break;
	if (!c)
		return;
	o = rt_zalloc(sizeof(*o));
	if (!o)
		return;
	parse_options(m + 240, len - 240, o);
	if (!o->server)
		o->server = from;
	memcpy(&yiaddr, m + 16, 4);
	switch (o->type) {
	case DHCPOFFER:
		if (c->state != ST_SELECTING || !yiaddr || yiaddr == RT_INADDR_BROADCAST)
			break;
		c->offered = yiaddr;
		c->server = o->server;
		c->state = ST_REQUESTING;
		RT_DBG(RT_DBG_DHCP, "%s: state REQUESTING", c->nif->name);
		c->tries = 0;
		dhcp_send(c, DHCPREQUEST);
		rt_timer_after(&c->timer, backoff_ms(c->tries++));
		break;
	case DHCPACK:
		if (c->state != ST_REQUESTING && c->state != ST_RENEWING && c->state != ST_REBINDING)
			break;
		if (!yiaddr || yiaddr == RT_INADDR_BROADCAST)
			break;
		bind_lease(c, yiaddr, o);
		break;
	case DHCPNAK:
		if (c->state == ST_REQUESTING || c->state == ST_RENEWING || c->state == ST_REBINDING) {
			rt_log(RT_LOG_WARN, "dhcp: %s: NAK", c->nif->name);
			lost(c);
		}
		break;
	}
	rt_free(o);
}

static void dhcp_rx(void *arg)
{
	struct rt_sockaddr_in6 from;
	struct rt_iov v;
	int r, mf;

	while (dhcp_so && dhcp_rxbuf) {
		v.base = dhcp_rxbuf;
		v.len = DHCP_RXBUF;
		mf = 0;
		memset(&from, 0, sizeof(from));
		r = dhcp_so->ops->recv(dhcp_so, &v, 1, RT_MSG_DONTWAIT, &from, &mf);
		if (r < 0)
			break;
		if (from.sin6_port != rt_htons(DHCP_SERVER_PORT))
			continue;
		dhcp_input(dhcp_rxbuf, (u32)r, rt_in6_v4(&from.sin6_addr));
	}
}

static void dhcp_upcall_fn(struct rt_socket *so, u32 events, void *arg)
{
	/* inside UDP input: defer the work to the timer list */
	if (events & RT_SEV_READ)
		rt_timer_after(&dhcp_rx_timer, 0);
}

/* ---------------------------------------------------------------- API */

static int socket_open(void)
{
	struct rt_sockaddr_in6 sin;
	int r;

	if (dhcp_so)
		return 0;
	dhcp_rxbuf = rt_malloc(DHCP_RXBUF);
	if (!dhcp_rxbuf)
		return -RT_ENOMEM;
	dhcp_upcall.fn = dhcp_upcall_fn;
	dhcp_upcall.arg = NULL;
	rt_timer_init(&dhcp_rx_timer, dhcp_rx, NULL);
	r = sock_kernel_create(RT_SOCK_DGRAM, RT_IPPROTO_UDP, &dhcp_upcall, &dhcp_so);
	if (r < 0)
		goto fail;
	dhcp_so->options |= RT_SO_BROADCAST | RT_SO_REUSEADDR;
	rt_sin_fill(dhcp_so, &sin, NULL, rt_htons(DHCP_CLIENT_PORT), 0);
	r = dhcp_so->ops->bind(dhcp_so, &sin);
	if (r < 0) {
		sock_kernel_close(dhcp_so);
		dhcp_so = NULL;
		goto fail;
	}
	return 0;
fail:
	rt_free(dhcp_rxbuf);
	dhcp_rxbuf = NULL;
	return r;
}

static void socket_close(void)
{
	rt_timer_del(&dhcp_rx_timer);
	if (dhcp_so)
		sock_kernel_close(dhcp_so);
	dhcp_so = NULL;
	rt_free(dhcp_rxbuf);
	dhcp_rxbuf = NULL;
}

int rt_dhcp_start(struct rt_netif *nif, void (*cb)(struct rt_netif *nif, int event))
{
	struct dhcp_client *c;
	int r;

	if (!nif || nif->type == RT_IFT_LOOP)
		return -RT_EINVAL;
	if (nif->ip4_disabled)
		return -RT_EAFNOSUPPORT;
	c = client_of(nif);
	if (c) {
		c->cb = cb;
		return 0;
	}
	r = socket_open();
	if (r < 0)
		return r;
	c = rt_zalloc(sizeof(*c));
	if (!c) {
		if (!dhcp_clients)
			socket_close();
		return -RT_ENOMEM;
	}
	c->nif = nif;
	c->cb = cb;
	if (nif->hwlen && nif->hwlen <= 16) {
		c->chlen = nif->hwlen;
		memcpy(c->chaddr, nif->hwaddr, nif->hwlen);
	} else {
		/* no link address (PTP): a stable random locally administered one */
		c->chlen = 6;
		rt_random_bytes(c->chaddr, 6);
		c->chaddr[0] = (u8)((c->chaddr[0] & 0xfe) | 0x02);
	}
	rt_timer_init(&c->timer, dhcp_timeout, c);
	rt_timer_init(&c->ll_timer, ll_timeout, c);
	c->ll_on = -1;
	c->next = dhcp_clients;
	dhcp_clients = c;
	start_init(c, rt_random32() % 256u);
	return 0;
}

void rt_dhcp_stop(struct rt_netif *nif)
{
	struct dhcp_client **pp, *c;

	for (pp = &dhcp_clients; (c = *pp) != NULL; pp = &c->next)
		if (c->nif == nif)
			break;
	if (!c)
		return;
	*pp = c->next;
	rt_timer_del(&c->timer);
	rt_timer_del(&c->ll_timer);
	rt_ipv4ll_stop(nif);
	if (c->addr && (c->state == ST_BOUND || c->state == ST_RENEWING ||
	                c->state == ST_REBINDING)) {
		c->xid = rt_random32();
		dhcp_send(c, DHCPRELEASE);
	}
	unconfigure(c);
	rt_free(c);
	if (!dhcp_clients)
		socket_close();
}

int rt_dhcp_bound(const struct rt_netif *nif)
{
	const struct dhcp_client *c = client_of(nif);

	return c && (c->state == ST_BOUND || c->state == ST_RENEWING || c->state == ST_REBINDING);
}

static const u8 state_map[] = {
	RT_DHCPS_INIT, RT_DHCPS_SELECTING, RT_DHCPS_REQUESTING, RT_DHCPS_BOUND,
	RT_DHCPS_RENEWING, RT_DHCPS_REBINDING
};

int rt_dhcp_get_info(const struct rt_netif *nif, struct rt_dhcp_info *info)
{
	struct dhcp_client *c = client_of(nif);

	memset(info, 0, sizeof(*info));
	info->ll_addr = rt_ipv4ll_info(nif, &info->ll_state, &info->ll_conflicts);
	if (!c)
		return RT_DHCPS_NONE;
	info->state = state_map[c->state];
	info->nroutes = c->ncls;
	if (c->state == ST_BOUND || c->state == ST_RENEWING || c->state == ST_REBINDING) {
		u32 el = lease_elapsed(c);
		info->server = c->server;
		info->router = c->router;
		info->addr = c->addr;
		info->lease_s = c->lease_s;
		info->left_s = c->lease_s == 0xffffffffu ? 0xffffffffu :
		               el < c->lease_s ? c->lease_s - el : 0;
	}
	return info->state;
}

int rt_dhcp_renew(struct rt_netif *nif)
{
	struct dhcp_client *c = client_of(nif);

	if (!c)
		return -RT_ENOENT;
	if (c->state == ST_BOUND || c->state == ST_RENEWING || c->state == ST_REBINDING) {
		/* RENEWING now: unicast DHCPREQUEST to the server. The lease
		 * timer keeps running; without an answer the normal T1/T2
		 * schedule takes over. */
		c->xid = rt_random32();
		c->state = ST_RENEWING;
		RT_DBG(RT_DBG_DHCP, "%s: state RENEWING", c->nif->name);
		dhcp_send(c, DHCPREQUEST);
	} else {
		start_init(c, 0);
	}
	return 0;
}

int rt_dhcp_set_linklocal(struct rt_netif *nif, int on, int wait_s)
{
	struct dhcp_client *c = client_of(nif);

	if (!c)
		return -RT_ENOENT;
	if (wait_s < 0 || wait_s > 3600)
		return -RT_EINVAL;
	if (on >= -1)
		c->ll_on = (s8)(on < 0 ? -1 : on != 0);
	if (wait_s)
		c->ll_wait_s = (u16)wait_s;
	if (!ll_enabled(c)) {
		rt_timer_del(&c->ll_timer);
		rt_ipv4ll_stop(nif);
	} else if (c->state < ST_BOUND) {
		rt_timer_del(&c->ll_timer);    /* the wait may have changed */
		ll_arm(c);
	}
	return 0;
}

int rt_dhcp_get_linklocal(const struct rt_netif *nif, int *wait_s)
{
	const struct dhcp_client *c = client_of(nif);

	if (wait_s)
		*wait_s = c && c->ll_wait_s ? c->ll_wait_s : (int)rt_ll_wait_s;
	if (!c)
		return rt_ll_enable != 0;
	return ll_enabled(c);
}

/* ------------------------------------------------------- option decoding */

/* RFC 3442 compact encoding: width, the significant octets of the
 * destination, the router. Returns the number of routes (at most max are
 * kept), -1 when the option is malformed (then all of it is ignored). */
int rt_dhcp_parse_classless(const u8 *p, u32 len, struct rt_dhcp_route *out, int max)
{
	u32 i = 0, w, n, k;
	int cnt = 0;

	if (len < 5)
		return -1;
	while (i < len) {
		u32 dst = 0;
		w = p[i++];
		if (w > 32)
			return -1;
		n = (w + 7u) / 8u;
		if (i + n + 4u > len)
			return -1;
		for (k = 0; k < n; k++)
			dst |= (u32)p[i + k] << (24u - 8u * k);
		i += n;
		if (cnt < max) {
			u32 m = w ? 0xffffffffu << (32u - w) : 0;
			out[cnt].dst = rt_htonl(dst & m);   /* stray host bits dropped */
			out[cnt].mask = rt_htonl(m);
			memcpy(&out[cnt].gw, p + i, 4);
			cnt++;
		}
		i += 4;
	}
	return cnt;
}

/* RFC 3397: DNS names (RFC 1035 encoding; compression pointers are
 * offsets into the option data) into a space-separated list of at most
 * RT_DHCP_SEARCH_MAX names. Names of 64 characters or more do not fit the
 * resolver and are skipped. Returns the number of names, -1 when the
 * option is malformed (no list at all). */
int rt_dhcp_parse_search(const u8 *p, u32 len, char *out, u32 outlen)
{
	u32 i = 0, o = 0;
	int cnt = 0;

	if (!outlen)
		return -1;
	out[0] = 0;
	while (i < len) {
		char name[256];
		u32 j = i, nl = 0, end = 0, jumps = 0, lim = i, k;

		for (;;) {
			u8 l;
			if (j >= len)
				return -1;
			l = p[j];
			if ((l & 0xc0) == 0xc0) {
				u32 to;
				if (j + 1 >= len)
					return -1;
				to = ((u32)(l & 0x3f) << 8) | p[j + 1];
				if (!end)
					end = j + 2;
				/* strictly backwards, before anything followed
				 * so far: no loops */
				if (to >= lim || ++jumps > 64)
					return -1;
				lim = to;
				j = to;
				continue;
			}
			if (l & 0xc0)
				return -1;
			if (l == 0) {
				if (!end)
					end = j + 1;
				break;
			}
			if (j + 1u + l > len || nl + l + 1u >= sizeof(name))
				return -1;
			if (nl)
				name[nl++] = '.';
			for (k = 0; k < l; k++) {
				u8 ch = p[j + 1 + k];
				if (ch <= 0x20 || ch >= 0x7f || ch == ',')
					return -1;
				name[nl++] = (char)ch;
			}
			j += 1u + l;
		}
		name[nl] = 0;
		i = end;
		if (!nl || nl >= 64 || cnt >= RT_DHCP_SEARCH_MAX || o + nl + 2u > outlen)
			continue;
		if (o)
			out[o++] = ' ';
		memcpy(out + o, name, nl + 1u);
		o += nl;
		cnt++;
	}
	return cnt;
}
