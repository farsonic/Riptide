/*
 * Riptide - UDP (RFC 768, RFC 1122 4.1, RFC 8200 8.1) over IPv4 and IPv6,
 * and helpers shared by the datagram socket protocols (UDP and raw IP).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Semantics follow 4.4BSD (what AmiTCP-style programs expect): send
 * buffer size bounds the datagram size (EMSGSIZE), unconnected sends
 * without a destination fail with ENOTCONN, broadcasts need SO_BROADCAST,
 * and ICMP errors are only reported on connected sockets (RFC 1122
 * 4.1.3.3). Address reuse follows Linux: two sockets may share a port if
 * both set SO_REUSEADDR or both set SO_REUSEPORT.
 *
 * Addresses are kept as rt_in6_addr (IPv4 as IPv4-mapped): an AF_INET6
 * socket bound to :: also receives IPv4 unless IPV6_V6ONLY. Over IPv6 the
 * checksum is mandatory (zero checksums are dropped).
 */
#include "infra.h"

s32 rt_udp_checksum = 1;
s32 rt_udp_recvspace = 41600;        /* 4.4BSD: 40 * (1024 + sizeof(sockaddr_in)) */
s32 rt_udp_maxdgram = 9216;          /* 4.4BSD udp_sendspace */

static const struct rt_sysctl udp_sysctl[] = {
	{ "net.inet.udp.checksum", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 1 },
	  &rt_udp_checksum, 0, 1, NULL, "Compute UDP checksums on output (IPv4; always over IPv6)" },
	{ "net.inet.udp.maxdgram", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 3 },
	  &rt_udp_maxdgram, 512, 65507, NULL, "Default send buffer (max datagram) of UDP sockets" },
	{ "net.inet.udp.recvspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 4 },
	  &rt_udp_recvspace, 1024, 4 * 1024 * 1024, NULL, "Default receive buffer of UDP sockets" },
};

#define UDP_MAX_PAYLOAD (65535 - 20 - 8)
#define UDP_MAX_GROUPS  8

struct udp_pcb {
	struct udp_pcb *next;
	struct rt_socket *so;
	rt_in6_addr laddr, faddr;  /* IPv4-mapped for IPv4 */
	u16 lport, fport;          /* network order */
	u8  laddr_implicit;        /* laddr chosen by connect() */
	u8  connected;
	u32 scope;                 /* interface of a link-local peer / binding */
	struct rt_inet_opts opt;
	/* IPV6_JOIN_GROUP memberships, dropped at close */
	struct { rt_in6_addr grp; u32 ifindex; } groups[UDP_MAX_GROUPS];
	u8  ngroups;
};

static struct udp_pcb *udp_pcbs;

/* =================== shared datagram socket helpers =================== */

int rt_sin_check(const struct rt_socket *so, const struct rt_sockaddr_in6 *sin, int v6only)
{
	if (!sin)
		return -RT_EINVAL;
	if (so->domain == RT_AF_INET6) {
		if (sin->sin6_family != RT_AF_INET6)
			return -RT_EAFNOSUPPORT;
		if (v6only && rt_in6_is_v4mapped(&sin->sin6_addr))
			return -RT_EAFNOSUPPORT;
		return 0;
	}
	/* Old Amiga programs often leave sin_family/sin_len zero. */
	if (sin->sin6_family != RT_AF_INET && sin->sin6_family != RT_AF_UNSPEC)
		return -RT_EAFNOSUPPORT;
	return 0;
}

void rt_sin_fill(const struct rt_socket *so, struct rt_sockaddr_in6 *sin, const rt_in6_addr *a,
                 u16 port, u32 scope)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin6_len = RT_SIN6_LEN;
	sin->sin6_family = (u8)(so ? so->domain : RT_AF_INET6);
	sin->sin6_port = port;
	if (a)
		sin->sin6_addr = *a;
	else if (!so || so->domain != RT_AF_INET6)
		rt_inaddr_v4any(&sin->sin6_addr);
	if (a && (rt_in6_is_linklocal(a) || rt_in6_is_mc_linklocal(a)))
		sin->sin6_scope_id = scope;
}

static int opt_int(const void *val, u32 len, s32 *v)
{
	if (!val)
		return -RT_EFAULT;
	if (len >= 4) {
		memcpy(v, val, 4);
		return 0;
	}
	if (len >= 1) {        /* Linux accepts char sized values */
		*v = *(const u8 *)val;
		return 0;
	}
	return -RT_EINVAL;
}

/* ---- IPv4 multicast socket options ---- */

/* The interface of IP_MULTICAST_IF / IP_ADD_MEMBERSHIP: struct in_addr
 * (at 'off') or, with 12 bytes, struct ip_mreqn whose index wins.
 * *n = NULL: none named (INADDR_ANY). */
static int mreq_netif(const void *val, u32 len, u32 off, struct rt_netif **n)
{
	struct rt_ip_mreqn mr;
	u32 a;

	*n = NULL;
	if (!val)
		return -RT_EFAULT;
	if (len < off + 4)
		return -RT_EINVAL;
	if (len >= sizeof(mr)) {
		memcpy(&mr, val, sizeof(mr));
		if (mr.imr_ifindex) {
			*n = rt_netif_by_index((u32)mr.imr_ifindex);
			return *n ? 0 : -RT_ENXIO;
		}
	}
	memcpy(&a, (const u8 *)val + off, 4);
	if (a == RT_INADDR_ANY)
		return 0;
	*n = rt_netif_by_addr(a);
	return *n ? 0 : -RT_EADDRNOTAVAIL;
}

static int inet_mc_group(struct rt_inet_opts *o, int join, const void *val, u32 len)
{
	struct rt_netif *n;
	u32 grp;
	int i, r;

	if (!val)
		return -RT_EFAULT;
	if (len < sizeof(struct rt_ip_mreq))
		return -RT_EINVAL;
	memcpy(&grp, val, 4);
	if (!rt_ip_is_multicast(grp))
		return -RT_EINVAL;
	r = mreq_netif(val, len, 4, &n);
	if (r < 0)
		return r;
	if (!n && !(n = rt_ip_mc_default_if(grp)))
		return -RT_EADDRNOTAVAIL;
	for (i = 0; i < o->nmships; i++)
		if (o->mships[i].grp == grp && o->mships[i].ifindex == n->index)
			break;
	if (!join) {
		if (i >= o->nmships)
			return -RT_EADDRNOTAVAIL;
		rt_ip_mc_leave(n, grp);
		o->mships[i] = o->mships[--o->nmships];
		return 0;
	}
	if (i < o->nmships)
		return -RT_EADDRINUSE;
	if (o->nmships >= RT_IP_MAX_MEMBERSHIPS)
		return -RT_ETOOMANYREFS;
	if (!o->mships) {
		o->mships = rt_malloc(RT_IP_MAX_MEMBERSHIPS * sizeof(*o->mships));
		if (!o->mships)
			return -RT_ENOBUFS;
	}
	r = rt_ip_mc_join(n, grp);
	if (r < 0)
		return r;
	o->mships[o->nmships].grp = grp;
	o->mships[o->nmships].ifindex = n->index;
	o->nmships++;
	return 0;
}

void rt_inet_mc_drop_all(struct rt_inet_opts *o)
{
	int i;

	for (i = 0; i < o->nmships; i++) {
		struct rt_netif *n = rt_netif_by_index(o->mships[i].ifindex);
		if (n)
			rt_ip_mc_leave(n, o->mships[i].grp);
	}
	o->nmships = 0;
	rt_free(o->mships);
	o->mships = NULL;
}

int rt_inet_mc_member(const struct rt_inet_opts *o, u32 grp, u32 ifindex)
{
	int i;

	for (i = 0; i < o->nmships; i++)
		if (o->mships[i].grp == grp && o->mships[i].ifindex == ifindex)
			return 1;
	return 0;
}

struct rt_netif *rt_inet_mc_oif(const struct rt_inet_opts *o)
{
	return o->mcast_ifindex ? rt_netif_by_index(o->mcast_ifindex) : NULL;
}

void rt_inet_opts_init(struct rt_inet_opts *o, int domain)
{
	memset(o, 0, sizeof(*o));
	o->mcast_ttl = 1;
	o->mcast_loop = 1;
	o->hlim = -1;
	o->mcast_hops = -1;
	o->mcast_loop6 = 1;
	o->v6only = domain == RT_AF_INET6 && rt_ip6_v6only;
}

int rt_inet_setopt(struct rt_inet_opts *o, int name, const void *val, u32 len, int allow_hdrincl)
{
	s32 v;
	int r;

	if (name == RT_IP_OPTIONS)
		return len == 0 ? 0 : -RT_EOPNOTSUPP;
	if (name == RT_IP_ADD_MEMBERSHIP || name == RT_IP_DROP_MEMBERSHIP)
		return inet_mc_group(o, name == RT_IP_ADD_MEMBERSHIP, val, len);
	if (name == RT_IP_MULTICAST_IF) {
		struct rt_netif *n;
		/* struct in_addr, or (Linux) struct ip_mreq / ip_mreqn */
		r = mreq_netif(val, len, len >= sizeof(struct rt_ip_mreq) ? 4 : 0, &n);
		if (r < 0)
			return r;
		if (n && !(n->flags & RT_IFF_MULTICAST))
			return -RT_EADDRNOTAVAIL;
		o->mcast_ifindex = n ? n->index : 0;
		return 0;
	}
	r = opt_int(val, len, &v);
	if (r < 0)
		return r;
	switch (name) {
	case RT_IP_TTL:
		if (v == -1)
			v = 0;                  /* Linux: back to default */
		else if (v < 1 || v > 255)
			return -RT_EINVAL;
		o->ttl = (u8)v;
		return 0;
	case RT_IP_TOS:
		o->tos = (u8)v;
		return 0;
	case RT_IP_RECVDSTADDR:
		o->recvdstaddr = v != 0;
		return 0;
	case RT_IP_HDRINCL:
		if (!allow_hdrincl)
			return -RT_ENOPROTOOPT;
		o->hdrincl = v != 0;
		return 0;
	case RT_IP_MULTICAST_TTL:
		if (v < 0 || v > 255)
			return -RT_EINVAL;
		o->mcast_ttl = (u8)v;
		return 0;
	case RT_IP_MULTICAST_LOOP:
		o->mcast_loop = v != 0;
		return 0;
	}
	return -RT_ENOPROTOOPT;
}

int rt_inet_getopt(const struct rt_inet_opts *o, int name, void *val, u32 *len, int allow_hdrincl)
{
	s32 v;

	switch (name) {
	case RT_IP_TTL:
		v = o->ttl ? o->ttl : rt_ip_defttl;
		break;
	case RT_IP_TOS:
		v = o->tos;
		break;
	case RT_IP_RECVDSTADDR:
		v = o->recvdstaddr;
		break;
	case RT_IP_HDRINCL:
		if (!allow_hdrincl)
			return -RT_ENOPROTOOPT;
		v = o->hdrincl;
		break;
	case RT_IP_MULTICAST_TTL:
		v = o->mcast_ttl;
		break;
	case RT_IP_MULTICAST_LOOP:
		v = o->mcast_loop;
		break;
	case RT_IP_OPTIONS:
		*len = 0;
		return 0;
	case RT_IP_MULTICAST_IF: {
		/* struct in_addr: the interface's address, INADDR_ANY when none */
		struct rt_netif *n = rt_inet_mc_oif(o);
		u32 a = n ? n->addr : RT_INADDR_ANY;
		if (!val || !len || *len < 4)
			return -RT_EINVAL;
		memcpy(val, &a, 4);
		*len = 4;
		return 0;
	}
	default:
		return -RT_ENOPROTOOPT;
	}
	if (!val || !len || *len < 1)
		return -RT_EINVAL;
	if (*len >= 4) {
		memcpy(val, &v, 4);
		*len = 4;
	} else {
		*(u8 *)val = (u8)v;
		*len = 1;
	}
	return 0;
}

int rt_inet6_setopt(struct rt_socket *so, struct rt_inet_opts *o, int name, const void *val, u32 len)
{
	s32 v;
	int r;

	if (name == RT_IPV6_JOIN_GROUP || name == RT_IPV6_LEAVE_GROUP)
		return -RT_EOPNOTSUPP;          /* datagram protocols handle these */
	r = opt_int(val, len, &v);
	if (r < 0)
		return r;
	switch (name) {
	case RT_IPV6_UNICAST_HOPS:
		if (v < -1 || v > 255)
			return -RT_EINVAL;
		o->hlim = (s16)v;
		return 0;
	case RT_IPV6_MULTICAST_HOPS:
		if (v < -1 || v > 255)
			return -RT_EINVAL;
		o->mcast_hops = (s16)v;
		return 0;
	case RT_IPV6_MULTICAST_IF:
		if (v && !rt_netif_by_index((u32)v))
			return -RT_ENXIO;
		o->mcast_if = (u32)v;
		return 0;
	case RT_IPV6_MULTICAST_LOOP:
		if (v < 0 || v > 1)
			return -RT_EINVAL;
		o->mcast_loop6 = (u8)v;
		return 0;
	case RT_IPV6_V6ONLY:
		if (so && (so->state & RT_SS_ISBOUND))
			return -RT_EINVAL;      /* BSD/Linux: only before bind */
		o->v6only = v != 0;
		return 0;
	case RT_IPV6_TCLASS:
		if (v < -1 || v > 255)
			return -RT_EINVAL;
		o->tclass = (u8)(v < 0 ? 0 : v);
		return 0;
	case RT_IPV6_DONTFRAG:
		o->dontfrag = v != 0;
		return 0;
	}
	return -RT_ENOPROTOOPT;
}

int rt_inet6_getopt(const struct rt_inet_opts *o, int name, void *val, u32 *len)
{
	s32 v;

	switch (name) {
	case RT_IPV6_UNICAST_HOPS:
		v = o->hlim >= 0 ? o->hlim : rt_ip6_hlim;
		break;
	case RT_IPV6_MULTICAST_HOPS:
		v = o->mcast_hops >= 0 ? o->mcast_hops : 1;
		break;
	case RT_IPV6_MULTICAST_IF:
		v = (s32)o->mcast_if;
		break;
	case RT_IPV6_MULTICAST_LOOP:
		v = o->mcast_loop6;
		break;
	case RT_IPV6_V6ONLY:
		v = o->v6only;
		break;
	case RT_IPV6_TCLASS:
		v = o->tclass;
		break;
	case RT_IPV6_DONTFRAG:
		v = o->dontfrag;
		break;
	default:
		return -RT_ENOPROTOOPT;
	}
	if (!val || !len || *len < 4)
		return -RT_EINVAL;
	memcpy(val, &v, 4);
	*len = 4;
	return 0;
}

int rt_dgram_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                  int flags, struct rt_sockaddr_in6 *from, int *msgflags)
{
	struct rt_buf *b = so->rcvq.head;
	const struct rt_dgram_cb *cb;
	u32 want, n;

	if (msgflags)
		*msgflags = 0;
	if (flags & RT_MSG_OOB)
		return -RT_EOPNOTSUPP;
	if (!b) {
		if (so->so_error) {
			int e = so->so_error;
			so->so_error = 0;
			return -e;
		}
		if (so->state & RT_SS_CANTRCVMORE)
			return 0;
		return -RT_EWOULDBLOCK;
	}
	cb = (const struct rt_dgram_cb *)(const void *)b->cb.b;
	want = rt_iov_total(iov, iovcnt);
	n = b->len < want ? b->len : want;
	if (n)
		n = rt_buf_to_iov(iov, iovcnt, 0, b->data, n);
	if (b->len > want && msgflags)
		*msgflags |= RT_MSG_TRUNC;
	if (from)
		*from = cb->from;
	if (!(flags & RT_MSG_PEEK)) {
		rt_bufq_unlink(&so->rcvq, b);
		rt_buf_free(b);
	}
	return (int)n;
}

u32 rt_dgram_poll(struct rt_socket *so)
{
	u32 m = RT_POLLOUT;

	if (!rt_bufq_empty(&so->rcvq) || so->so_error || (so->state & RT_SS_CANTRCVMORE))
		m |= RT_POLLIN;
	if (so->so_error)
		m |= RT_POLLERR;
	return m;
}

int rt_dgram_ioctl(struct rt_socket *so, u32 req, void *arg)
{
	u32 v;

	switch (req) {
	case RT_FIONREAD:
		v = so->rcvq.bytes;          /* BSD: all queued bytes */
		memcpy(arg, &v, 4);
		return 0;
	case RT_SIOCATMARK:
		v = 0;
		memcpy(arg, &v, 4);
		return 0;
	}
	return -RT_EINVAL;
}

int rt_dgram_deliver(struct rt_socket *so, struct rt_buf *b, const rt_in6_addr *saddr, u16 sport,
                     const rt_in6_addr *daddr, const struct rt_netif *nif)
{
	struct rt_dgram_cb *cb;

	/* A small datagram should not pin an MTU sized buffer while it sits
	 * in a receive queue: copy it into a buffer of the right class. */
	if (b->size > 512 && b->len <= 256) {
		struct rt_buf *c = rt_buf_alloc(0, b->len);
		if (c && c->size < b->size) {
			memcpy(rt_buf_put(c, b->len), b->data, b->len);
			c->netif = b->netif;
			c->flags = b->flags;
			rt_buf_free(b);
			b = c;
		} else {
			rt_buf_free(c);
		}
	}
	cb = (struct rt_dgram_cb *)(void *)b->cb.b;
	memset(cb, 0, sizeof(*cb));
	rt_sin_fill(so, &cb->from, saddr, sport, nif ? nif->index : 0);
	cb->dst = *daddr;
	cb->ifindex = nif ? nif->index : 0;
	if (rt_sock_queue_dgram(so, b) < 0) {     /* wakes readers on success */
		rt_buf_free(b);
		return -RT_ENOBUFS;
	}
	return 0;
}

/* ============================== UDP ============================== */

static struct udp_pcb *pcb_of(struct rt_socket *so)
{
	return (struct udp_pcb *)so->pcb;
}

static int reuse_ok(const struct rt_socket *a, const struct rt_socket *b)
{
	return ((a->options & b->options & RT_SO_REUSEPORT) != 0) ||
	       ((a->options & b->options & RT_SO_REUSEADDR) != 0);
}

/* Is (laddr, port) taken for 'self'? check_reuse = 0 ignores the reuse
 * options (ephemeral port selection wants a really free port). */
static int udp_port_busy(const struct udp_pcb *self, const rt_in6_addr *laddr, u16 port, int check_reuse)
{
	const struct udp_pcb *p;

	for (p = udp_pcbs; p; p = p->next) {
		if (p == self || p->lport != port)
			continue;
		if (!rt_inet_bind_overlap(&p->laddr, p->opt.v6only, laddr, self->opt.v6only))
			continue;
		if (check_reuse && reuse_ok(self->so, p->so))
			continue;
		return 1;
	}
	return 0;
}

static int udp_bind_port(struct udp_pcb *pcb, const rt_in6_addr *laddr, u16 port)
{
	if (port == 0) {
		u32 first = (u32)rt_ip_portrange_first, last = (u32)rt_ip_portrange_last, n, i, start;

		if (first > last) {
			u32 t = first;
			first = last;
			last = t;
		}
		n = last - first + 1;
		start = rt_random32() % n;
		for (i = 0; i < n; i++) {
			u16 p = rt_htons((u16)(first + (start + i) % n));
			if (!udp_port_busy(pcb, laddr, p, 0)) {
				port = p;
				break;
			}
		}
		if (!port)
			return -RT_EADDRNOTAVAIL;
	} else if (udp_port_busy(pcb, laddr, port, 1)) {
		return -RT_EADDRINUSE;
	}
	pcb->laddr = *laddr;
	pcb->lport = port;
	pcb->so->state |= RT_SS_ISBOUND;
	return 0;
}

static void any_addr(const struct udp_pcb *pcb, rt_in6_addr *a)
{
	if (pcb->so->domain == RT_AF_INET6)
		memset(a, 0, sizeof(*a));
	else
		rt_inaddr_v4any(a);
}

static int udp_attach(struct rt_socket *so)
{
	struct udp_pcb *pcb = rt_zalloc(sizeof(*pcb));

	if (!pcb)
		return -RT_ENOBUFS;
	pcb->so = so;
	rt_inet_opts_init(&pcb->opt, so->domain);
	any_addr(pcb, &pcb->laddr);
	any_addr(pcb, &pcb->faddr);
	so->pcb = pcb;
	/* BSD udp_attach(): soreserve(udp_sendspace, udp_recvspace) */
	so->sndbuf = (u32)rt_udp_maxdgram;
	so->rcvbuf = (u32)rt_udp_recvspace;
	pcb->next = udp_pcbs;
	udp_pcbs = pcb;
	return 0;
}

static void udp_close(struct rt_socket *so)
{
	struct udp_pcb *pcb = pcb_of(so), **pp;
	int i;

	if (!pcb)
		return;
	for (pp = &udp_pcbs; *pp; pp = &(*pp)->next) {
		if (*pp == pcb) {
			*pp = pcb->next;
			break;
		}
	}
	for (i = 0; i < pcb->ngroups; i++) {
		struct rt_netif *n = rt_netif_by_index(pcb->groups[i].ifindex);
		if (n)
			rt_ip6_mc_leave(n, &pcb->groups[i].grp);
	}
	rt_inet_mc_drop_all(&pcb->opt);
	rt_bufq_purge(&so->rcvq);
	so->pcb = NULL;
	rt_free(pcb);
}

static int local_bindable(const rt_in6_addr *a)
{
	if (rt_inaddr_is_any(a))
		return 1;
	if (rt_in6_is_v4mapped(a)) {
		u32 v = rt_in6_v4(a);
		return rt_netif_by_addr(v) || rt_ip_is_loopback_net(v) || rt_ip_is_multicast(v) ||
		       rt_ip_is_broadcast(v, NULL);
	}
	return rt_in6_is_mcast(a) || (rt_in6_is_v4mapped(a) && rt_ip_is_multicast(rt_in6_v4(a))) ||
	       rt_inet_is_local(a);
}

static int udp_bind(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct udp_pcb *pcb = pcb_of(so);
	int r = rt_sin_check(so, sin, pcb->opt.v6only);

	if (r < 0)
		return r;
	if (pcb->lport)
		return -RT_EINVAL;
	if (!local_bindable(&sin->sin6_addr))
		return -RT_EADDRNOTAVAIL;
	if (rt_in6_is_linklocal(&sin->sin6_addr))
		pcb->scope = sin->sin6_scope_id;
	return udp_bind_port(pcb, &sin->sin6_addr, sin->sin6_port);
}

static int udp_listen(struct rt_socket *so, int backlog)
{
	return -RT_EOPNOTSUPP;
}

static void udp_disconnect(struct udp_pcb *pcb)
{
	any_addr(pcb, &pcb->faddr);
	pcb->fport = 0;
	pcb->connected = 0;
	if (pcb->laddr_implicit) {
		any_addr(pcb, &pcb->laddr);
		pcb->laddr_implicit = 0;
	}
	pcb->so->state &= ~(u32)RT_SS_ISCONNECTED;
}

/* "this host": 0.0.0.0 -> 127.0.0.1, :: -> ::1 (BSD) */
static void dst_fixup(rt_in6_addr *d)
{
	if (rt_in6_is_v4mapped(d) && d->w[3] == RT_INADDR_ANY)
		d->w[3] = RT_INADDR_LOOPBACK;
	else if (rt_in6_is_unspec(d))
		*d = rt_in6addr_loopback;
}

/* Families of a binding and a peer must agree (IPv4-mapped vs IPv6). */
static int family_clash(const rt_in6_addr *la, const rt_in6_addr *fa)
{
	if (rt_in6_is_unspec(la))
		return 0;
	return rt_in6_is_v4mapped(la) != rt_in6_is_v4mapped(fa);
}

static int udp_connect(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct udp_pcb *pcb = pcb_of(so);
	struct rt_inet_route rt;
	rt_in6_addr faddr;
	u32 scope;
	int r;

	if (!sin)
		return -RT_EINVAL;
	if (sin->sin6_family == RT_AF_UNSPEC &&
	    (so->domain == RT_AF_INET6 || (rt_inaddr_is_any(&sin->sin6_addr) && sin->sin6_port == 0))) {
		udp_disconnect(pcb);         /* Linux/FreeBSD: dissolve */
		return 0;
	}
	r = rt_sin_check(so, sin, pcb->opt.v6only);
	if (r < 0)
		return r;
	if (sin->sin6_port == 0)
		return -RT_EADDRNOTAVAIL;
	faddr = sin->sin6_addr;
	dst_fixup(&faddr);
	if (family_clash(&pcb->laddr, &faddr))
		return -RT_EAFNOSUPPORT;
	scope = sin->sin6_scope_id;
	if (!scope && (rt_in6_is_linklocal(&faddr) || rt_in6_is_mc_linklocal(&faddr)))
		scope = pcb->scope;
	if (rt_in6_is_v4mapped(&faddr) && rt_ip_is_multicast(rt_in6_v4(&faddr))) {
		/* the interface udp_send() will use */
		struct rt_netif *n = rt_inet_mc_oif(&pcb->opt);
		if (!n)
			n = rt_ip_mc_default_if(rt_in6_v4(&faddr));
		if (!n)
			return -RT_ENETUNREACH;
		memset(&rt, 0, sizeof(rt));
		rt.netif = n;
		rt_in6_set_v4mapped(&rt.src, n->addr);
	} else {
		r = rt_inet_route(&faddr, scope, &rt);
		if (r < 0)
			return r;
	}
	if (!pcb->lport) {
		rt_in6_addr any;
		if (rt_in6_is_v4mapped(&faddr))
			rt_inaddr_v4any(&any);
		else
			memset(&any, 0, sizeof(any));
		r = udp_bind_port(pcb, rt_in6_is_unspec(&pcb->laddr) ? &pcb->laddr : &any, 0);
		if (r < 0)
			return r;
	}
	if (rt_inaddr_is_any(&pcb->laddr) || pcb->laddr_implicit) {
		if (rt_inaddr_is_any(&rt.src))
			return -RT_EADDRNOTAVAIL;
		pcb->laddr = rt.src;
		pcb->laddr_implicit = 1;
	}
	pcb->faddr = faddr;
	pcb->fport = sin->sin6_port;
	pcb->scope = (rt_in6_is_linklocal(&faddr) || rt_in6_is_mc_linklocal(&faddr)) && rt.netif ?
	             rt.netif->index : pcb->scope;
	pcb->connected = 1;
	so->state |= RT_SS_ISCONNECTED;
	return 0;
}

/* Pacing. A datagram socket remembers the interface its last datagram
 * left by (so->tx_ifindex). While that interface has no transmit room
 * (driver slots or QoS band full), the socket is not writable and a send
 * says EWOULDBLOCK: sock_sendit() then sleeps for a blocking socket, a
 * non-blocking one waits with WaitSelect(). rt_drv_tx_done() wakes the
 * waiters (rt_udp_tx_wake()); so does a stall timer when the driver
 * completes nothing for RT_NETIF_TX_STALL_MS, after which the interface
 * counts as having room (its frames are dropped as before pacing). The
 * first datagram, and one to another interface, is sent as before. */
static int udp_tx_room(struct rt_socket *so)
{
	struct rt_netif *n;

	if (!so->tx_ifindex)
		return 1;
	n = rt_netif_by_index(so->tx_ifindex);
	if (!n) {
		so->tx_ifindex = 0;
		return 1;
	}
	if (rt_netif_tx_room(n, rt_qos_sock_prio(so)))
		return 1;
	so->tx_wait = 1;
	rt_netif_tx_wait(n);
	return 0;
}

void rt_udp_tx_wake(struct rt_netif *nif)
{
	struct udp_pcb *p;

	for (p = udp_pcbs; p; p = p->next) {
		struct rt_socket *so = p->so;
		if (so && so->tx_wait && so->tx_ifindex == nif->index) {
			so->tx_wait = 0;
			rt_sock_wakeup(so, RT_SEV_WRITE);
		}
	}
}

static u32 udp_poll(struct rt_socket *so)
{
	u32 m = rt_dgram_poll(so);

	if ((m & RT_POLLOUT) && !udp_tx_room(so))
		m &= ~RT_POLLOUT;
	return m;
}

static int udp_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, const struct rt_sockaddr_in6 *to)
{
	struct udp_pcb *pcb = pcb_of(so);
	struct rt_inet_out o;
	struct rt_buf *b;
	rt_in6_addr daddr, saddr;
	u32 len, dsum, scope;
	u16 dport;
	u8 *uh;
	int r, v4, csum;

	if (so->state & RT_SS_CANTSENDMORE)
		return -RT_EPIPE;
	if (to) {
		r = rt_sin_check(so, to, pcb->opt.v6only);
		if (r < 0)
			return r;
		if (pcb->connected)
			return -RT_EISCONN;
		daddr = to->sin6_addr;
		dport = to->sin6_port;
		scope = to->sin6_scope_id;
	} else {
		if (!pcb->connected)
			return -RT_ENOTCONN;
		daddr = pcb->faddr;
		dport = pcb->fport;
		scope = pcb->scope;
	}
	if (dport == 0)
		return -RT_EINVAL;
	dst_fixup(&daddr);
	v4 = rt_in6_is_v4mapped(&daddr);
	if (family_clash(&pcb->laddr, &daddr))
		return -RT_EAFNOSUPPORT;
	if (!v4 && rt_in6_is_mcast(&daddr) && pcb->opt.mcast_if)
		scope = pcb->opt.mcast_if;
	if (v4 && rt_ip_is_multicast(rt_in6_v4(&daddr))) {
		/* IP_MULTICAST_IF, else the route to the group, else the
		 * first multicast interface (no default route needed) */
		struct rt_netif *n = rt_inet_mc_oif(&pcb->opt);
		if (!n && pcb->opt.mcast_ifindex)
			return -RT_ENXIO;
		if (!n)
			n = rt_ip_mc_default_if(rt_in6_v4(&daddr));
		if (!n)
			return -RT_ENETUNREACH;
		scope = n->index;
	}
	else if (!scope && !v4 && (rt_in6_is_linklocal(&daddr) || rt_in6_is_mc_linklocal(&daddr)))
		scope = pcb->scope;
	len = rt_iov_total(iov, iovcnt);
	if (len > UDP_MAX_PAYLOAD || (so->sndbuf && len > so->sndbuf))
		return -RT_EMSGSIZE;
	if (!pcb->lport) {
		rt_in6_addr any;
		if (v4)
			rt_inaddr_v4any(&any);
		else
			memset(&any, 0, sizeof(any));
		r = udp_bind_port(pcb, rt_in6_is_unspec(&pcb->laddr) ? &pcb->laddr : &any, 0);
		if (r < 0)
			return r;
	}
	saddr = pcb->laddr;
	if (rt_inaddr_is_any(&saddr) ||
	    (v4 && (rt_ip_is_multicast(rt_in6_v4(&saddr)) || rt_ip_is_broadcast(rt_in6_v4(&saddr), NULL))) ||
	    (!v4 && rt_in6_is_mcast(&saddr))) {
		struct rt_inet_route rt;
		if (v4 && rt_ip_is_multicast(rt_in6_v4(&daddr))) {
			rt_in6_set_v4mapped(&saddr, rt_netif_by_index(scope)->addr);
		} else if (v4) {
			/* IPv4: errors (no route, broadcast) come from rt_ip_output() */
			rt_in6_set_v4mapped(&saddr, rt_ip_select_source(rt_in6_v4(&daddr)));
		} else {
			r = rt_inet_route(&daddr, scope, &rt);
			if (r < 0)
				return r;
			saddr = rt.src;
			if (rt_in6_is_unspec(&saddr))
				return -RT_EADDRNOTAVAIL;
		}
	}
	csum = !v4 || (rt_udp_checksum && !so->no_check);
	if (!udp_tx_room(so)) {
		rt_udpstat.TxNoRoom++;
		return -RT_EWOULDBLOCK;
	}

	b = rt_buf_alloc(RT_BUF_HEADROOM, 8 + len);
	if (!b)
		return -RT_ENOBUFS;
	uh = rt_buf_put(b, 8 + len);
	/* copy the payload and checksum it in the same pass */
	dsum = 0;
	if (len && (csum ? rt_iov_to_buf_csum(iov, iovcnt, 0, uh + 8, len, &dsum) :
	                   rt_iov_to_buf(iov, iovcnt, 0, uh + 8, len)) != len) {
		rt_buf_free(b);
		return -RT_EFAULT;
	}
	memcpy(uh, &pcb->lport, 2);
	memcpy(uh + 2, &dport, 2);
	rt_put_be16(uh + 4, (u16)(8 + len));
	uh[6] = uh[7] = 0;
	if (csum) {
		u16 ck = rt_csum_fold(rt_csum_add(rt_csum_partial(uh, 8,
		                      rt_inet_pseudo(&saddr, &daddr, RT_IPPROTO_UDP, 8 + len)), dsum));
		if (ck == 0)
			ck = 0xffff;         /* RFC 768: 0 means "no checksum" */
		memcpy(uh + 6, &ck, 2);
	}
	memset(&o, 0, sizeof(o));
	o.src = saddr;
	o.dst = daddr;
	o.proto = RT_IPPROTO_UDP;
	if (v4) {
		o.tos = rt_qos_sock_tos(so, pcb->opt.tos);
		if (rt_ip_is_multicast(rt_in6_v4(&daddr))) {
			o.ttl = pcb->opt.mcast_ttl ? pcb->opt.mcast_ttl : 1;
			o.mcast_ttl0 = pcb->opt.mcast_ttl == 0;
			o.mcast_noloop = !pcb->opt.mcast_loop;
			o.scope = scope;
		} else {
			o.ttl = pcb->opt.ttl;
		}
	} else {
		o.ttl = rt_in6_is_mcast(&daddr) ? (pcb->opt.mcast_hops >= 0 ? pcb->opt.mcast_hops : 1)
		                                : pcb->opt.hlim;
		if (o.ttl == 0)
			o.ttl = -1;          /* hop limit 0 is not supported */
		o.tos = rt_qos_sock_tos(so, pcb->opt.tclass);
		o.df = pcb->opt.dontfrag;
		o.mcast_noloop = !pcb->opt.mcast_loop6;
		o.scope = scope;
	}
	b->prio = rt_qos_sock_prio(so);
	o.dontroute = (so->options & RT_SO_DONTROUTE) || (flags & RT_MSG_DONTROUTE);
	o.allow_broadcast = (so->options & RT_SO_BROADCAST) != 0;
	rt_netif_tx_last = 0;
	r = rt_inet_output(b, &o);
	if (rt_netif_tx_last)
		so->tx_ifindex = rt_netif_tx_last;
	if (r == -RT_ENOBUFS && !udp_tx_room(so)) {
		/* the driver filled up after all: wait for room, then this
		 * datagram is sent again from the caller's buffer */
		rt_udpstat.TxNoRoom++;
		return -RT_EWOULDBLOCK;
	}
	if (r < 0)
		return r;
	rt_udpstat.OutDatagrams++;
	return (int)len;
}

static int udp_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, struct rt_sockaddr_in6 *from, int *msgflags)
{
	return rt_dgram_recv(so, iov, iovcnt, flags, from, msgflags);
}

static int udp_shutdown(struct rt_socket *so, int how)
{
	if (how < 0 || how > 2)
		return -RT_EINVAL;
	if (how != 1)
		so->state |= RT_SS_CANTRCVMORE;
	if (how != 0)
		so->state |= RT_SS_CANTSENDMORE;
	rt_sock_wakeup(so, RT_SEV_READ | RT_SEV_WRITE);
	return 0;
}

static int udp_group(struct udp_pcb *pcb, int join, const void *val, u32 len)
{
	struct rt_ipv6_mreq mr;
	struct rt_netif *n;
	int i, r;

	if (!val || len < sizeof(mr))
		return -RT_EINVAL;
	memcpy(&mr, val, sizeof(mr));
	if (!rt_in6_is_mcast(&mr.ipv6mr_multiaddr))
		return -RT_EINVAL;
	if (mr.ipv6mr_interface) {
		n = rt_netif_by_index(mr.ipv6mr_interface);
	} else {
		struct rt_inet_route rt;
		n = rt_inet_route(&mr.ipv6mr_multiaddr, 0, &rt) == 0 ? rt.netif : NULL;
	}
	if (!n)
		return -RT_ENXIO;
	for (i = 0; i < pcb->ngroups; i++)
		if (pcb->groups[i].ifindex == n->index && rt_in6_eq(&pcb->groups[i].grp, &mr.ipv6mr_multiaddr))
			break;
	if (join) {
		if (i < pcb->ngroups)
			return -RT_EADDRINUSE;
		if (pcb->ngroups >= UDP_MAX_GROUPS)
			return -RT_ENOBUFS;
		r = rt_ip6_mc_join(n, &mr.ipv6mr_multiaddr);
		if (r < 0)
			return r;
		pcb->groups[pcb->ngroups].grp = mr.ipv6mr_multiaddr;
		pcb->groups[pcb->ngroups].ifindex = n->index;
		pcb->ngroups++;
		return 0;
	}
	if (i >= pcb->ngroups)
		return -RT_EADDRNOTAVAIL;
	rt_ip6_mc_leave(n, &mr.ipv6mr_multiaddr);
	pcb->groups[i] = pcb->groups[--pcb->ngroups];
	return 0;
}

static int udp_setsockopt(struct rt_socket *so, int level, int name, const void *val, u32 len)
{
	struct udp_pcb *pcb = pcb_of(so);

	if (level == RT_IPPROTO_IPV6 && so->domain == RT_AF_INET6) {
		if (name == RT_IPV6_JOIN_GROUP || name == RT_IPV6_LEAVE_GROUP)
			return udp_group(pcb, name == RT_IPV6_JOIN_GROUP, val, len);
		return rt_inet6_setopt(so, &pcb->opt, name, val, len);
	}
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_setopt(&pcb->opt, name, val, len, 0);
}

static int udp_getsockopt(struct rt_socket *so, int level, int name, void *val, u32 *len)
{
	if (level == RT_IPPROTO_IPV6 && so->domain == RT_AF_INET6)
		return rt_inet6_getopt(&pcb_of(so)->opt, name, val, len);
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_getopt(&pcb_of(so)->opt, name, val, len, 0);
}

static int udp_getname(struct rt_socket *so, struct rt_sockaddr_in6 *sin, int peer)
{
	struct udp_pcb *pcb = pcb_of(so);

	if (peer) {
		if (!pcb->connected)
			return -RT_ENOTCONN;
		rt_sin_fill(so, sin, &pcb->faddr, pcb->fport, pcb->scope);
	} else {
		rt_sin_fill(so, sin, &pcb->laddr, pcb->lport, pcb->scope);
	}
	return 0;
}

const struct rt_proto_ops rt_udp_ops = {
	"udp",
	udp_attach,
	udp_close,
	udp_bind,
	udp_listen,
	udp_connect,
	udp_send,
	udp_recv,
	udp_shutdown,
	udp_setsockopt,
	udp_getsockopt,
	udp_poll,
	udp_getname,
	rt_dgram_ioctl,
};

/* ---- input ---- */

static void udp_deliver(struct udp_pcb *pcb, struct rt_buf *b, const rt_in6_addr *saddr, u16 sport,
                        const rt_in6_addr *daddr)
{
	if (rt_dgram_deliver(pcb->so, b, saddr, sport, daddr, b->netif) < 0)
		rt_udpstat.RcvbufErrors++;
	else
		rt_udpstat.InDatagrams++;
}

static int udp_faddr_match(const struct udp_pcb *p, const rt_in6_addr *saddr, u16 sport)
{
	return !p->connected || (rt_in6_eq(&p->faddr, saddr) && p->fport == sport);
}

/* IPv4 multicast goes to members of the group on the receiving interface
 * only (BSD; Linux IP_MULTICAST_ALL 0). 224.0.0.1 to all. */
static int udp_mc_wanted(const struct udp_pcb *p, const struct rt_buf *b, const rt_in6_addr *daddr)
{
	u32 g;

	if (!rt_in6_is_v4mapped(daddr))
		return 1;
	g = rt_in6_v4(daddr);
	if (!rt_ip_is_multicast(g) || g == rt_htonl(RT_INADDR_ALLHOSTS_GROUP))
		return 1;
	return b->netif && rt_inet_mc_member(&p->opt, g, b->netif->index);
}

/* Common input: b->data at the UDP header, length checked by the caller.
 * Returns 1 if b was consumed, 0 if nobody wants it (caller sends port
 * unreachable and frees b, data at the UDP payload). */
static int udp_rcv(struct rt_buf *b, const rt_in6_addr *saddr, const rt_in6_addr *daddr,
                   u16 sport, u16 dport, int bcast)
{
	struct udp_pcb *p, *best = NULL, *last = NULL;
	int bestscore = -1;

	if (bcast) {
		/* BSD: every socket bound to the port (and to the wildcard or
		 * this destination) gets a copy. */
		for (p = udp_pcbs; p; p = p->next) {
			if (p->lport != dport)
				continue;
			if (!rt_inet_laddr_match(&p->laddr, p->opt.v6only, daddr))
				continue;
			if (!udp_faddr_match(p, saddr, sport))
				continue;
			if (!udp_mc_wanted(p, b, daddr))
				continue;
			if (last) {
				struct rt_buf *c = rt_buf_copy(b);
				if (c)
					udp_deliver(last, c, saddr, sport, daddr);
			}
			last = p;
		}
		if (last) {
			udp_deliver(last, b, saddr, sport, daddr);
			return 1;
		}
		rt_udpstat.NoPorts++;
		rt_buf_free(b);
		return 1;
	}
	/* Unicast: the most specific match wins (exact local address beats
	 * wildcard, connected beats unconnected; an AF_INET socket's IPv4
	 * wildcard beats a dual-stack :: one). */
	for (p = udp_pcbs; p; p = p->next) {
		int m, score;
		if (p->lport != dport)
			continue;
		m = rt_inet_laddr_match(&p->laddr, p->opt.v6only, daddr);
		if (!m)
			continue;
		if (!udp_faddr_match(p, saddr, sport))
			continue;
		score = ((m == 2) + p->connected) * 4 + (m == 1 && rt_in6_is_v4mapped(&p->laddr));
		if (score > bestscore) {
			best = p;
			bestscore = score;
		}
	}
	if (best) {
		udp_deliver(best, b, saddr, sport, daddr);
		return 1;
	}
	rt_udpstat.NoPorts++;
	return 0;
}

static void udp_input_body(struct rt_buf *b, const struct rt_iphdr *ip)
{
	rt_in6_addr sa, da;
	u32 ulen;
	u16 sport, dport;
	int bcast;
	u8 *uh = b->data;

	if (b->len < 8)
		goto err;
	ulen = rt_get_be16(uh + 4);
	if (ulen < 8 || ulen > b->len)
		goto err;
	rt_buf_trim(b, ulen);
	if ((uh[6] | uh[7]) && !(b->flags & RT_BUFF_CSUM_OK) &&
	    rt_csum_fold(rt_buf_csum(b, rt_csum_pseudo(ip->saddr, ip->daddr, RT_IPPROTO_UDP, (u16)ulen))) != 0) {
		rt_udpstat.InCsumErrors++;
		goto err;
	}
	memcpy(&sport, uh, 2);
	memcpy(&dport, uh + 2, 2);
	if (dport == 0)
		goto err;
	rt_buf_pull(b, 8);
	rt_in6_set_v4mapped(&sa, ip->saddr);
	rt_in6_set_v4mapped(&da, ip->daddr);
	bcast = (b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)) || rt_ip_is_multicast(ip->daddr) ||
	        rt_ip_is_broadcast(ip->daddr, NULL);
	if (udp_rcv(b, &sa, &da, sport, dport, bcast))
		return;
	/* RFC 1122 4.1.3.1: port unreachable. Restore the IP header for the
	 * quote; it is still in front of the data. */
	rt_buf_push(b, (u32)(b->data - (const u8 *)ip));
	rt_icmp_send_error(b, RT_ICMP_UNREACH, RT_ICMP_UNREACH_PORT, 0);
	rt_buf_free(b);
	return;
err:
	rt_udpstat.InErrors++;
	rt_buf_free(b);
}

void rt_udp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	RT_PROF_ENTER(RT_PROF_UDP_IN);
	udp_input_body(b, ip);
	RT_PROF_EXIT(RT_PROF_UDP_IN);
}

static void udp_input6_body(struct rt_buf *b, const struct rt_ip6hdr *ip6)
{
	u32 ulen;
	u16 sport, dport;
	u8 *uh = b->data;

	if (b->len < 8)
		goto err;
	ulen = rt_get_be16(uh + 4);
	if (ulen < 8 || ulen > b->len)
		goto err;
	rt_buf_trim(b, ulen);
	/* RFC 8200 8.1: the checksum is not optional */
	if (!(uh[6] | uh[7]))
		goto err;
	if (!(b->flags & (RT_BUFF_CSUM_OK | RT_BUFF_LOOP)) &&
	    rt_csum_fold(rt_buf_csum(b, rt_csum_pseudo6(&ip6->src, &ip6->dst, RT_IPPROTO_UDP, ulen))) != 0) {
		rt_udpstat.InCsumErrors++;
		goto err;
	}
	memcpy(&sport, uh, 2);
	memcpy(&dport, uh + 2, 2);
	if (dport == 0)
		goto err;
	rt_buf_pull(b, 8);
	if (udp_rcv(b, &ip6->src, &ip6->dst, sport, dport, rt_in6_is_mcast(&ip6->dst)))
		return;
	rt_buf_push(b, (u32)(b->data - (const u8 *)ip6));
	rt_icmp6_send_error(b, RT_ICMP6_DST_UNREACH, RT_ICMP6_DST_UNREACH_NOPORT, 0);
	rt_buf_free(b);
	return;
err:
	rt_udpstat.InErrors++;
	rt_buf_free(b);
}

void rt_udp_input6(struct rt_buf *b, const struct rt_ip6hdr *ip6)
{
	RT_PROF_ENTER(RT_PROF_UDP_IN);
	udp_input6_body(b, ip6);
	RT_PROF_EXIT(RT_PROF_UDP_IN);
}

static void udp_err(const rt_in6_addr *src, const rt_in6_addr *dst, u16 sport, u16 dport, int err)
{
	struct udp_pcb *p;

	for (p = udp_pcbs; p; p = p->next) {
		if (p->lport != sport || !p->connected || !rt_in6_eq(&p->faddr, dst) || p->fport != dport)
			continue;
		if (!rt_inaddr_is_any(&p->laddr) && !rt_in6_eq(&p->laddr, src))
			continue;
		rt_sock_set_error(p->so, err);
	}
}

void rt_udp_icmp_error(u8 type, u8 code, u32 info, const struct rt_iphdr *inner, u32 inner_len)
{
	u32 ihl = rt_ip_hlen(inner);
	const u8 *uh = (const u8 *)inner + ihl;
	rt_in6_addr s, d;
	u16 sport, dport;
	int err;

	if (inner_len < ihl + 8)
		return;
	if (type != RT_ICMP_UNREACH)
		return;      /* time exceeded, quench, param problem: transient */
	switch (code) {
	case RT_ICMP_UNREACH_PORT:
		err = RT_ECONNREFUSED;
		break;
	case RT_ICMP_UNREACH_PROTOCOL:
		err = RT_ENOPROTOOPT;
		break;
	case RT_ICMP_UNREACH_NET:
		err = RT_ENETUNREACH;
		break;
	case RT_ICMP_UNREACH_NEEDFRAG:
		return;      /* PMTU already updated; datagrams are not resent */
	default:
		err = RT_EHOSTUNREACH;
		break;
	}
	memcpy(&sport, uh, 2);
	memcpy(&dport, uh + 2, 2);
	rt_in6_set_v4mapped(&s, inner->saddr);
	rt_in6_set_v4mapped(&d, inner->daddr);
	udp_err(&s, &d, sport, dport, err);
}

void rt_udp_icmp6_error(u8 type, u8 code, u32 info, const struct rt_ip6hdr *inner,
                        const u8 *uh, u32 uhlen)
{
	u16 sport, dport;
	int err;

	if (uhlen < 8 || type != RT_ICMP6_DST_UNREACH)
		return;      /* too big, time exceeded, param problem: not reported */
	switch (code) {
	case RT_ICMP6_DST_UNREACH_NOPORT:
		err = RT_ECONNREFUSED;
		break;
	case RT_ICMP6_DST_UNREACH_ADMIN:
		err = RT_EACCES;
		break;
	case RT_ICMP6_DST_UNREACH_NOROUTE:
		err = RT_ENETUNREACH;
		break;
	default:
		err = RT_EHOSTUNREACH;
		break;
	}
	memcpy(&sport, uh, 2);
	memcpy(&dport, uh + 2, 2);
	udp_err(&inner->src, &inner->dst, sport, dport, err);
}

int rt_udp_list(struct rt_pcb_info *out, int max)
{
	const struct udp_pcb *p;
	int n = 0;

	for (p = udp_pcbs; p; p = p->next, n++) {
		if (!out || n >= max)
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		out[n].proto = RT_IPPROTO_UDP;
		out[n].lport = rt_ntohs(p->lport);
		out[n].fport = rt_ntohs(p->fport);
		out[n].laddr = rt_in6_is_v4mapped(&p->laddr) ? rt_in6_v4(&p->laddr) : 0;
		out[n].faddr = p->connected && rt_in6_is_v4mapped(&p->faddr) ? rt_in6_v4(&p->faddr) : 0;
		out[n].v6 = p->so->domain == RT_AF_INET6;
		out[n].v6only = p->opt.v6only;
		out[n].laddr6 = p->laddr;
		if (p->connected)
			out[n].faddr6 = p->faddr;
		else if (p->so->domain != RT_AF_INET6)
			rt_inaddr_v4any(&out[n].faddr6);
		out[n].recvq = p->so->rcvq.bytes;
	}
	return n;
}

void rt_udp_init(void)
{
	udp_pcbs = NULL;
	rt_sysctl_register(udp_sysctl, (int)RT_ARRAY_SIZE(udp_sysctl));
}
