/*
 * Riptide - stateless DHCPv6 client (RFC 8415 6.1, 18.2.6): when a router
 * advertisement sets the O (or M) flag, ask for DNS servers (RFC 3646)
 * and the domain search list with an Information-request and refresh them
 * after the information refresh time (RFC 8415 21.23).
 *
 * Stateful DHCPv6 (address and prefix leases) is not implemented: with M
 * set Riptide still configures addresses by SLAAC if the RA allows it and
 * only asks for the other configuration.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "sock_internal.h"
#include "ip6_internal.h"

#define DHCP6_CLIENT_PORT 546
#define DHCP6_SERVER_PORT 547
#define DHCP6_INFO_REQUEST 11
#define DHCP6_REPLY        7
#define OPT_CLIENTID       1
#define OPT_SERVERID       2
#define OPT_ORO            6
#define OPT_ELAPSED        8
#define OPT_STATUS         13
#define OPT_DNS_SERVERS    23
#define OPT_DOMAIN_LIST    24
#define OPT_INFO_REFRESH   32
#define INF_MAX_DELAY_MS   1000u
#define INF_TIMEOUT_MS     1000u
#define INF_MAX_RT_MS      3600000u
#define IRT_DEFAULT_S      86400u
#define IRT_MINIMUM_S      600u
#define DHCP6_MAX_IF       4
#define DHCP6_BUF          1024

struct dhcp6_if {
	u32 ifindex;               /* 0 = slot free */
	u32 xid;
	u32 start_ms;              /* first transmission of this exchange */
	u32 rt_ms;                 /* current retransmission timeout */
	struct rt_timer timer;
	u8  have_reply;
};

static struct dhcp6_if d6[DHCP6_MAX_IF];
static struct rt_socket *d6_so;
static struct sock_upcall d6_upcall;
static struct rt_timer d6_rx_timer;
static u8 *d6_buf;

static struct dhcp6_if *slot_of(u32 ifindex)
{
	int i;

	for (i = 0; i < DHCP6_MAX_IF; i++)
		if (d6[i].ifindex == ifindex)
			return &d6[i];
	return NULL;
}

/* randomised timeout, RFC 8415 15: RT +/- 10% */
static u32 jitter(u32 ms)
{
	return ms - ms / 10 + rt_random32() % (ms / 5 + 1);
}

static void d6_send(struct dhcp6_if *s)
{
	static const rt_in6_addr servers = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2 } };
	struct rt_netif *nif = rt_netif_by_index(s->ifindex);
	struct rt_sockaddr_in6 to;
	struct rt_iov v;
	u8 m[64];
	u32 n = 0, el;

	if (!nif || !d6_so || !in6_ifa_linklocal(nif, 1))
		return;
	m[n++] = DHCP6_INFO_REQUEST;
	m[n++] = (u8)(s->xid >> 16);
	m[n++] = (u8)(s->xid >> 8);
	m[n++] = (u8)s->xid;
	if (nif->hwlen == 6) {
		/* DUID-LL (RFC 8415 11.4): type 3, hardware type 1 */
		rt_put_be16(m + n, OPT_CLIENTID);
		rt_put_be16(m + n + 2, 10);
		rt_put_be16(m + n + 4, 3);
		rt_put_be16(m + n + 6, 1);
		memcpy(m + n + 8, nif->hwaddr, 6);
		n += 14;
	}
	rt_put_be16(m + n, OPT_ORO);
	rt_put_be16(m + n + 2, 6);
	rt_put_be16(m + n + 4, OPT_DNS_SERVERS);
	rt_put_be16(m + n + 6, OPT_DOMAIN_LIST);
	rt_put_be16(m + n + 8, OPT_INFO_REFRESH);
	n += 10;
	el = (rt_now_ms() - s->start_ms) / 10;     /* hundredths of a second */
	rt_put_be16(m + n, OPT_ELAPSED);
	rt_put_be16(m + n + 2, 2);
	rt_put_be16(m + n + 4, (u16)(el > 0xffff ? 0xffff : el));
	n += 6;
	rt_sin_fill(d6_so, &to, &servers, rt_htons(DHCP6_SERVER_PORT), s->ifindex);
	v.base = m;
	v.len = n;
	d6_so->ops->send(d6_so, &v, 1, 0, &to);
}

static void d6_timer(void *arg)
{
	struct dhcp6_if *s = arg;
	struct rt_netif *nif = rt_netif_by_index(s->ifindex);

	if (!nif || !nif->ip6) {
		s->ifindex = 0;
		return;
	}
	if (s->have_reply) {
		/* information refresh: a new exchange */
		s->have_reply = 0;
		s->xid = rt_random32() & 0xffffff;
		s->start_ms = rt_now_ms();
		s->rt_ms = jitter(INF_TIMEOUT_MS);
	} else if (!s->start_ms) {
		s->start_ms = rt_now_ms() | 1;
		s->rt_ms = jitter(INF_TIMEOUT_MS);
	} else {
		/* RFC 8415 15: RT = 2 * RTprev + RAND * RTprev, up to MRT */
		s->rt_ms = jitter(s->rt_ms * 2);
		if (s->rt_ms > INF_MAX_RT_MS)
			s->rt_ms = jitter(INF_MAX_RT_MS);
	}
	d6_send(s);
	rt_timer_after(&s->timer, s->rt_ms);
}

/* domain list option: the first name becomes the search domain */
static void d6_domains(const u8 *p, u32 len)
{
	char name[64];
	u32 off = 0, n = 0;

	while (off < len && p[off]) {
		u32 l = p[off];
		if (l > 63 || off + 1 + l > len || n + l + 2 > sizeof(name))
			return;
		if (n)
			name[n++] = '.';
		memcpy(name + n, p + off + 1, l);
		n += l;
		off += 1 + l;
	}
	if (!n)
		return;
	name[n] = 0;
	rt_dns_set_search_auto(name);
}

static void d6_input(const u8 *m, u32 len, u32 ifindex)
{
	struct dhcp6_if *s = slot_of(ifindex);
	u32 off, xid, irt = IRT_DEFAULT_S;
	const u8 *dns = NULL, *dom = NULL;
	u32 dnslen = 0, domlen = 0;
	int have_server = 0;

	if (!s || len < 4 || m[0] != DHCP6_REPLY)
		return;
	xid = ((u32)m[1] << 16) | ((u32)m[2] << 8) | m[3];
	if (xid != s->xid || s->have_reply)
		return;
	for (off = 4; off + 4 <= len;) {
		u16 code = rt_get_be16(m + off), olen = rt_get_be16(m + off + 2);
		const u8 *v = m + off + 4;
		if (off + 4 + olen > len)
			return;                     /* malformed: ignore the reply */
		switch (code) {
		case OPT_SERVERID:
			have_server = 1;
			break;
		case OPT_STATUS:
			if (olen >= 2 && rt_get_be16(v) != 0)
				return;                 /* an error status */
			break;
		case OPT_DNS_SERVERS:
			dns = v;
			dnslen = olen;
			break;
		case OPT_DOMAIN_LIST:
			dom = v;
			domlen = olen;
			break;
		case OPT_INFO_REFRESH:
			if (olen == 4)
				irt = rt_get_be32(v);
			break;
		}
		off += 4u + olen;
	}
	if (!have_server)
		return;
	if (irt < IRT_MINIMUM_S)
		irt = IRT_MINIMUM_S;
	s->have_reply = 1;
	if (dns) {
		u32 k;
		for (k = 0; k + 16 <= dnslen; k += 16) {
			rt_in6_addr a;
			memcpy(a.b, dns + k, 16);
			/* kept a little longer than the refresh time */
			rt_dns_add_server6(&a, ifindex, irt == RT_IN6_INFINITE ? irt : irt + irt / 2,
			                   RT_DNS_SRC_DHCP6);
		}
	}
	if (dom)
		d6_domains(dom, domlen);
	rt_timer_after(&s->timer, irt > 0x7fffffffu / 1000u ? 0x7fffffffu : irt * 1000u);
}

static void d6_rx(void *arg)
{
	struct rt_sockaddr_in6 from;
	struct rt_iov v;
	int r, mf;

	while (d6_so && d6_buf) {
		v.base = d6_buf;
		v.len = DHCP6_BUF;
		mf = 0;
		memset(&from, 0, sizeof(from));
		r = d6_so->ops->recv(d6_so, &v, 1, RT_MSG_DONTWAIT, &from, &mf);
		if (r < 0)
			break;
		if (from.sin6_port != rt_htons(DHCP6_SERVER_PORT) || !rt_in6_is_linklocal(&from.sin6_addr))
			continue;
		d6_input(d6_buf, (u32)r, from.sin6_scope_id);
	}
}

static void d6_upcall_fn(struct rt_socket *so, u32 events, void *arg)
{
	if (events & RT_SEV_READ)
		rt_timer_after(&d6_rx_timer, 0);
}

static int d6_open(void)
{
	struct rt_sockaddr_in6 sin;
	int r, one = 1;

	if (d6_so)
		return 0;
	d6_buf = rt_malloc(DHCP6_BUF);
	if (!d6_buf)
		return -RT_ENOMEM;
	d6_upcall.fn = d6_upcall_fn;
	d6_upcall.arg = NULL;
	rt_timer_init(&d6_rx_timer, d6_rx, NULL);
	r = sock_kernel_create_af(RT_AF_INET6, RT_SOCK_DGRAM, RT_IPPROTO_UDP, &d6_upcall, &d6_so);
	if (r < 0)
		goto fail;
	d6_so->options |= RT_SO_REUSEADDR;
	d6_so->ops->setsockopt(d6_so, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof(one));
	rt_sin_fill(d6_so, &sin, NULL, rt_htons(DHCP6_CLIENT_PORT), 0);
	r = d6_so->ops->bind(d6_so, &sin);
	if (r < 0) {
		sock_kernel_close(d6_so);
		d6_so = NULL;
		goto fail;
	}
	return 0;
fail:
	rt_free(d6_buf);
	d6_buf = NULL;
	return r;
}

void dhcp6_start_info(struct rt_netif *nif)
{
	struct dhcp6_if *s = slot_of(nif->index);

	if (s)
		return;
	if (d6_open() < 0)
		return;
	s = slot_of(0);
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->ifindex = nif->index;
	s->xid = rt_random32() & 0xffffff;
	rt_timer_init(&s->timer, d6_timer, s);
	/* RFC 8415 18.2.6: delay the first message by up to INF_MAX_DELAY */
	rt_timer_after(&s->timer, rt_random32() % INF_MAX_DELAY_MS);
}

void dhcp6_stop(struct rt_netif *nif)
{
	struct dhcp6_if *s = slot_of(nif->index);
	int i, any = 0;

	if (!s)
		return;
	rt_timer_del(&s->timer);
	s->ifindex = 0;
	if (nif->ip6)
		ip6_ifp(nif)->dhcp6_started = 0;
	for (i = 0; i < DHCP6_MAX_IF; i++)
		any |= d6[i].ifindex != 0;
	if (!any && d6_so) {
		rt_timer_del(&d6_rx_timer);
		sock_kernel_close(d6_so);
		d6_so = NULL;
		rt_free(d6_buf);
		d6_buf = NULL;
	}
}

/* DHCPv6 state of nif for tools: 0 none, 1 requesting, 2 got a reply */
int rt_dhcp6_state(const struct rt_netif *nif);
int rt_dhcp6_state(const struct rt_netif *nif)
{
	struct dhcp6_if *s = slot_of(nif->index);

	return !s ? 0 : s->have_reply ? 2 : 1;
}

void dhcp6_init(void)
{
	memset(d6, 0, sizeof(d6));
	d6_so = NULL;
	d6_buf = NULL;
}
