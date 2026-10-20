/*
 * Riptide - UDP (RFC 768, RFC 1122 4.1) and helpers shared by the
 * datagram socket protocols (UDP and raw IP).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Semantics follow 4.4BSD (what AmiTCP-style programs expect): send
 * buffer size bounds the datagram size (EMSGSIZE), unconnected sends
 * without a destination fail with ENOTCONN, broadcasts need SO_BROADCAST,
 * and ICMP errors are only reported on connected sockets (RFC 1122
 * 4.1.3.3). Address reuse follows Linux: two sockets may share a port if
 * both set SO_REUSEADDR or both set SO_REUSEPORT.
 */
#include "infra.h"

s32 rt_udp_checksum = 1;
s32 rt_udp_recvspace = 41600;        /* 4.4BSD: 40 * (1024 + sizeof(sockaddr_in)) */
s32 rt_udp_maxdgram = 9216;          /* 4.4BSD udp_sendspace */

static const struct rt_sysctl udp_sysctl[] = {
	{ "net.inet.udp.checksum", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 1 },
	  &rt_udp_checksum, 0, 1, NULL, "Compute UDP checksums on output" },
	{ "net.inet.udp.maxdgram", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 3 },
	  &rt_udp_maxdgram, 512, 65507, NULL, "Default send buffer (max datagram) of UDP sockets" },
	{ "net.inet.udp.recvspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, 2, 17, 4 },
	  &rt_udp_recvspace, 1024, 4 * 1024 * 1024, NULL, "Default receive buffer of UDP sockets" },
};

#define UDP_MAX_PAYLOAD (65535 - 20 - 8)

struct udp_pcb {
	struct udp_pcb *next;
	struct rt_socket *so;
	u32 laddr, faddr;          /* network order */
	u16 lport, fport;          /* network order */
	u8  laddr_implicit;        /* laddr chosen by connect() */
	struct rt_inet_opts opt;
};

static struct udp_pcb *udp_pcbs;

/* =================== shared datagram socket helpers =================== */

int rt_sin_check(const struct rt_sockaddr_in *sin)
{
	if (!sin)
		return -RT_EINVAL;
	/* Old Amiga programs often leave sin_family/sin_len zero. */
	if (sin->sin_family != RT_AF_INET && sin->sin_family != RT_AF_UNSPEC)
		return -RT_EAFNOSUPPORT;
	return 0;
}

static void sin_fill(struct rt_sockaddr_in *sin, u32 addr, u16 port)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = RT_AF_INET;
	sin->sin_port = port;
	sin->sin_addr = addr;
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

int rt_inet_setopt(struct rt_inet_opts *o, int name, const void *val, u32 len, int allow_hdrincl)
{
	s32 v;
	int r;

	if (name == RT_IP_OPTIONS)
		return len == 0 ? 0 : -RT_EOPNOTSUPP;
	if (name == RT_IP_ADD_MEMBERSHIP || name == RT_IP_DROP_MEMBERSHIP)
		return -RT_EOPNOTSUPP;
	if (name == RT_IP_MULTICAST_IF) {
		u32 a;
		if (len < 4)
			return -RT_EINVAL;
		memcpy(&a, val, 4);
		return a == 0 || rt_netif_by_addr(a) ? 0 : -RT_EADDRNOTAVAIL;
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

int rt_dgram_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                  int flags, struct rt_sockaddr_in *from, int *msgflags)
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

int rt_dgram_deliver(struct rt_socket *so, struct rt_buf *b, u32 saddr, u16 sport,
                     u32 daddr, const struct rt_netif *nif)
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
	sin_fill(&cb->from, saddr, sport);
	cb->dst_addr = daddr;
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
static int udp_port_busy(const struct udp_pcb *self, u32 laddr, u16 port, int check_reuse)
{
	const struct udp_pcb *p;

	for (p = udp_pcbs; p; p = p->next) {
		if (p == self || p->lport != port)
			continue;
		if (p->laddr && laddr && p->laddr != laddr)
			continue;
		if (check_reuse && reuse_ok(self->so, p->so))
			continue;
		return 1;
	}
	return 0;
}

static int udp_bind_port(struct udp_pcb *pcb, u32 laddr, u16 port)
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
	pcb->laddr = laddr;
	pcb->lport = port;
	pcb->so->state |= RT_SS_ISBOUND;
	return 0;
}

static int udp_attach(struct rt_socket *so)
{
	struct udp_pcb *pcb = rt_zalloc(sizeof(*pcb));

	if (!pcb)
		return -RT_ENOBUFS;
	pcb->so = so;
	pcb->opt.mcast_ttl = 1;
	pcb->opt.mcast_loop = 1;
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

	if (!pcb)
		return;
	for (pp = &udp_pcbs; *pp; pp = &(*pp)->next) {
		if (*pp == pcb) {
			*pp = pcb->next;
			break;
		}
	}
	rt_bufq_purge(&so->rcvq);
	so->pcb = NULL;
	rt_free(pcb);
}

static int local_bindable(u32 a)
{
	return a == RT_INADDR_ANY || rt_netif_by_addr(a) || rt_ip_is_loopback_net(a) ||
	       rt_ip_is_multicast(a) || rt_ip_is_broadcast(a, NULL);
}

static int udp_bind(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	struct udp_pcb *pcb = pcb_of(so);
	int r = rt_sin_check(sin);

	if (r < 0)
		return r;
	if (pcb->lport)
		return -RT_EINVAL;
	if (!local_bindable(sin->sin_addr))
		return -RT_EADDRNOTAVAIL;
	return udp_bind_port(pcb, sin->sin_addr, sin->sin_port);
}

static int udp_listen(struct rt_socket *so, int backlog)
{
	return -RT_EOPNOTSUPP;
}

static void udp_disconnect(struct udp_pcb *pcb)
{
	pcb->faddr = 0;
	pcb->fport = 0;
	if (pcb->laddr_implicit) {
		pcb->laddr = 0;
		pcb->laddr_implicit = 0;
	}
	pcb->so->state &= ~(u32)RT_SS_ISCONNECTED;
}

static int udp_connect(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	struct udp_pcb *pcb = pcb_of(so);
	struct rt_nexthop nh;
	u32 faddr;
	int r;

	if (!sin)
		return -RT_EINVAL;
	if (sin->sin_family == RT_AF_UNSPEC && sin->sin_addr == 0 && sin->sin_port == 0) {
		udp_disconnect(pcb);         /* Linux/FreeBSD: dissolve */
		return 0;
	}
	r = rt_sin_check(sin);
	if (r < 0)
		return r;
	if (sin->sin_port == 0)
		return -RT_EADDRNOTAVAIL;
	faddr = sin->sin_addr;
	if (faddr == RT_INADDR_ANY)
		faddr = RT_INADDR_LOOPBACK;  /* BSD: "this host" */
	r = rt_ip_route(faddr, &nh);
	if (r < 0)
		return r;
	if (!pcb->lport) {
		r = udp_bind_port(pcb, 0, 0);
		if (r < 0)
			return r;
	}
	if (!pcb->laddr || pcb->laddr_implicit) {
		pcb->laddr = nh.src;
		pcb->laddr_implicit = 1;
	}
	pcb->faddr = faddr;
	pcb->fport = sin->sin_port;
	so->state |= RT_SS_ISCONNECTED;
	return 0;
}

static int udp_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, const struct rt_sockaddr_in *to)
{
	struct udp_pcb *pcb = pcb_of(so);
	struct rt_ip_out o;
	struct rt_buf *b;
	u32 len, daddr, saddr;
	u16 dport;
	u8 *uh;
	int r;

	if (so->state & RT_SS_CANTSENDMORE)
		return -RT_EPIPE;
	if (to) {
		r = rt_sin_check(to);
		if (r < 0)
			return r;
		if (pcb->faddr)
			return -RT_EISCONN;
		daddr = to->sin_addr;
		dport = to->sin_port;
	} else {
		if (!pcb->faddr)
			return -RT_ENOTCONN;
		daddr = pcb->faddr;
		dport = pcb->fport;
	}
	if (dport == 0)
		return -RT_EINVAL;
	if (daddr == RT_INADDR_ANY)
		daddr = RT_INADDR_LOOPBACK;
	len = rt_iov_total(iov, iovcnt);
	if (len > UDP_MAX_PAYLOAD || (so->sndbuf && len > so->sndbuf))
		return -RT_EMSGSIZE;
	if (!pcb->lport) {
		r = udp_bind_port(pcb, 0, 0);
		if (r < 0)
			return r;
	}
	saddr = pcb->laddr;
	if (!saddr || rt_ip_is_multicast(saddr) || rt_ip_is_broadcast(saddr, NULL))
		saddr = rt_ip_select_source(daddr);

	b = rt_buf_alloc(RT_BUF_HEADROOM, 8 + len);
	if (!b)
		return -RT_ENOBUFS;
	uh = rt_buf_put(b, 8 + len);
	if (len && rt_iov_to_buf(iov, iovcnt, 0, uh + 8, len) != len) {
		rt_buf_free(b);
		return -RT_EFAULT;
	}
	memcpy(uh, &pcb->lport, 2);
	memcpy(uh + 2, &dport, 2);
	rt_put_be16(uh + 4, (u16)(8 + len));
	uh[6] = uh[7] = 0;
	if (rt_udp_checksum) {
		u16 ck = rt_csum_fold(rt_csum_partial(uh, 8 + len,
		                      rt_csum_pseudo(saddr, daddr, RT_IPPROTO_UDP, (u16)(8 + len))));
		if (ck == 0)
			ck = 0xffff;         /* RFC 768: 0 means "no checksum" */
		memcpy(uh + 6, &ck, 2);
	}
	memset(&o, 0, sizeof(o));
	o.saddr = saddr;
	o.daddr = daddr;
	o.proto = RT_IPPROTO_UDP;
	o.ttl = rt_ip_is_multicast(daddr) ? pcb->opt.mcast_ttl : pcb->opt.ttl;
	o.tos = pcb->opt.tos;
	o.dontroute = (so->options & RT_SO_DONTROUTE) || (flags & RT_MSG_DONTROUTE);
	o.allow_broadcast = (so->options & RT_SO_BROADCAST) != 0;
	r = rt_ip_output(b, &o);
	if (r < 0)
		return r;
	rt_udpstat.OutDatagrams++;
	return (int)len;
}

static int udp_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, struct rt_sockaddr_in *from, int *msgflags)
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

static int udp_setsockopt(struct rt_socket *so, int level, int name, const void *val, u32 len)
{
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_setopt(&pcb_of(so)->opt, name, val, len, 0);
}

static int udp_getsockopt(struct rt_socket *so, int level, int name, void *val, u32 *len)
{
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_getopt(&pcb_of(so)->opt, name, val, len, 0);
}

static int udp_getname(struct rt_socket *so, struct rt_sockaddr_in *sin, int peer)
{
	struct udp_pcb *pcb = pcb_of(so);

	if (peer) {
		if (!pcb->faddr)
			return -RT_ENOTCONN;
		sin_fill(sin, pcb->faddr, pcb->fport);
	} else {
		sin_fill(sin, pcb->laddr, pcb->lport);
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
	rt_dgram_poll,
	udp_getname,
	rt_dgram_ioctl,
};

/* ---- input ---- */

static void udp_deliver(struct udp_pcb *pcb, struct rt_buf *b, const struct rt_iphdr *ip, u16 sport)
{
	if (rt_dgram_deliver(pcb->so, b, ip->saddr, sport, ip->daddr, b->netif) < 0)
		rt_udpstat.RcvbufErrors++;
	else
		rt_udpstat.InDatagrams++;
}

void rt_udp_input(struct rt_buf *b, const struct rt_iphdr *ip)
{
	struct udp_pcb *p, *best = NULL, *last = NULL;
	u32 ulen;
	u16 sport, dport;
	int bcast, bestwild = 3;
	u8 *uh = b->data;

	if (b->len < 8)
		goto err;
	ulen = rt_get_be16(uh + 4);
	if (ulen < 8 || ulen > b->len)
		goto err;
	rt_buf_trim(b, ulen);
	if ((uh[6] | uh[7]) && !(b->flags & RT_BUFF_CSUM_OK) &&
	    rt_csum_fold(rt_csum_partial(uh, ulen,
	                 rt_csum_pseudo(ip->saddr, ip->daddr, RT_IPPROTO_UDP, (u16)ulen))) != 0) {
		rt_udpstat.InCsumErrors++;
		goto err;
	}
	memcpy(&sport, uh, 2);
	memcpy(&dport, uh + 2, 2);
	if (dport == 0)
		goto err;
	rt_buf_pull(b, 8);

	bcast = (b->flags & (RT_BUFF_BCAST | RT_BUFF_MCAST)) || rt_ip_is_multicast(ip->daddr) ||
	        rt_ip_is_broadcast(ip->daddr, NULL);
	if (bcast) {
		/* BSD: every socket bound to the port (and to the wildcard or
		 * this destination) gets a copy. */
		for (p = udp_pcbs; p; p = p->next) {
			if (p->lport != dport)
				continue;
			if (p->laddr && p->laddr != ip->daddr)
				continue;
			if (p->faddr && (p->faddr != ip->saddr || p->fport != sport))
				continue;
			if (last) {
				struct rt_buf *c = rt_buf_copy(b);
				if (c)
					udp_deliver(last, c, ip, sport);
			}
			last = p;
		}
		if (last) {
			udp_deliver(last, b, ip, sport);
			return;
		}
		rt_udpstat.NoPorts++;
		rt_buf_free(b);
		return;
	}
	/* Unicast: the most specific match (fewest wildcards) wins. */
	for (p = udp_pcbs; p; p = p->next) {
		int wild = 0;
		if (p->lport != dport)
			continue;
		if (p->laddr) {
			if (p->laddr != ip->daddr)
				continue;
		} else {
			wild++;
		}
		if (p->faddr) {
			if (p->faddr != ip->saddr || p->fport != sport)
				continue;
		} else {
			wild++;
		}
		if (wild < bestwild) {
			best = p;
			bestwild = wild;
			if (!wild)
				break;
		}
	}
	if (best) {
		udp_deliver(best, b, ip, sport);
		return;
	}
	rt_udpstat.NoPorts++;
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

void rt_udp_icmp_error(u8 type, u8 code, u32 info, const struct rt_iphdr *inner, u32 inner_len)
{
	u32 ihl = rt_ip_hlen(inner);
	const u8 *uh = (const u8 *)inner + ihl;
	u16 sport, dport;
	int err;
	struct udp_pcb *p;

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
	for (p = udp_pcbs; p; p = p->next) {
		if (p->lport != sport || p->faddr != inner->daddr || p->fport != dport)
			continue;
		if (p->laddr && p->laddr != inner->saddr)
			continue;
		rt_sock_set_error(p->so, err);
	}
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
		out[n].laddr = p->laddr;
		out[n].faddr = p->faddr;
		out[n].recvq = p->so->rcvq.bytes;
	}
	return n;
}

void rt_udp_init(void)
{
	udp_pcbs = NULL;
	rt_sysctl_register(udp_sysctl, (int)RT_ARRAY_SIZE(udp_sysctl));
}
