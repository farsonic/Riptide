/*
 * Riptide - DHCP client (RFC 2131 / RFC 2132).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * All entry points are called with the stack lock held; everything else
 * runs from timers in the stack thread.
 *
 * Until the interface has an address, messages are built here as complete
 * IP/UDP datagrams from 0.0.0.0:68 to 255.255.255.255:67 and handed to
 * nif->link_output() (dsthw NULL = link broadcast). Renewals go unicast to
 * the server through rt_ip_output(). Replies are received on one shared
 * kernel UDP socket bound to INADDR_ANY:68 (ip.c accepts 255.255.255.255
 * on any interface, configured or not); we always set the BROADCAST flag
 * so servers broadcast OFFER/ACK while we cannot receive unicast.
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
#define OPT_MTU        26
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

struct dhcp_opts {
	u8  type;
	u32 mask, router, server, lease, t1, t2;
	u32 dns[DHCP_MAX_DNS];
	u8  ndns;
	u16 mtu;
	char domain[64];
	char hostname[64];
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
	static const u8 params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_HOSTNAME, OPT_DOMAIN,
	                             OPT_MTU, OPT_LEASE, OPT_SERVERID, OPT_T1, OPT_T2 };
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
	return nif->link_output(nif, b, NULL, RT_ETH_P_IP);
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

static void unconfigure(struct dhcp_client *c)
{
	int i;

	if (!c->addr)
		return;
	if (c->router)   /* only our own route: a static one stays */
		rt_route_delete_match(0, 0, c->router, c->nif, RT_RTF_DHCP);
	for (i = 0; i < c->ndns; i++)
		rt_dns_remove_server(c->dns[i]);
	if (c->set_domain)
		rt_domainname[0] = 0;
	if (c->nif->addr == c->addr)
		rt_netif_set_addr(c->nif, 0, 0, 0);
	c->addr = c->router = 0;
	c->ndns = 0;
	c->set_domain = 0;
}

static void start_init(struct dhcp_client *c, u32 delay_ms)
{
	c->state = ST_INIT;
	c->tries = 0;
	c->xid = rt_random32();
	rt_timer_after(&c->timer, delay_ms);
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
	u32 mask = o->mask ? o->mask : classful(yiaddr);
	int renew = c->addr != 0 && c->addr == yiaddr, i, j;
	char a[16];

	if (c->addr && (c->addr != yiaddr || c->mask != mask || c->router != o->router))
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
	if (o->mtu >= 68 && o->mtu <= nif->mtu)
		nif->mtu = o->mtu;
	if (o->router) {
		/* (Re)install on every ACK: the route may have been removed by
		 * hand. A static default route still wins (route.c orders DHCP
		 * routes last), and one with the same gateway makes this fail. */
		c->router = o->router;
		rt_route_add(0, 0, o->router, nif, RT_RTF_UP | RT_RTF_DHCP, 0);
	}
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
		rt_dns_set_search(o->domain);
		c->set_domain = 1;
	}
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
			left = c->lease_s - el;
		} else if (el >= c->t1_s) {
			if (c->state == ST_BOUND)
				c->xid = rt_random32();
			c->state = ST_RENEWING;
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
		case OPT_DNS: {
			u32 k;
			for (k = 0; k + 4 <= l && o->ndns < DHCP_MAX_DNS; k += 4) {
				u32 a;
				memcpy(&a, v + k, 4);
				if (a && a != RT_INADDR_BROADCAST)
					o->dns[o->ndns++] = a;
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
	if (!c)
		return RT_DHCPS_NONE;
	info->state = state_map[c->state];
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
		dhcp_send(c, DHCPREQUEST);
	} else {
		start_init(c, 0);
	}
	return 0;
}
