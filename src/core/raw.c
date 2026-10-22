/*
 * Riptide - raw IP sockets (SOCK_RAW), as used by ping and traceroute.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * IPv4 (BSD semantics): a raw socket of protocol P receives a copy of
 * every incoming datagram of protocol P (protocol 0: all protocols), IP
 * header included, filtered by bound local and connected foreign address.
 * The header is delivered as received (network order fields). On output
 * the stack builds the IP header unless IP_HDRINCL is set (implied for
 * IPPROTO_RAW), in which case the application supplies it and the stack
 * fills in length, checksum and, when zero, id and source address.
 *
 * IPv6 (RFC 3542): AF_INET6 raw sockets get the payload only (no IPv6
 * header; the source comes with recvfrom()), and the stack computes the
 * checksum at offset 2 for ICMPv6 (always) or at IPV6_CHECKSUM's offset.
 * ICMP6_FILTER (KAME semantics: set bit = pass) selects ICMPv6 types.
 */
#include "infra.h"

struct raw_pcb {
	struct raw_pcb *next;
	struct rt_socket *so;
	u8  proto;
	u8  v6;
	s16 csum_off;              /* IPv6: checksum offset, -1 none */
	u32 laddr, faddr;          /* IPv4 sockets */
	rt_in6_addr laddr6, faddr6; /* IPv6 sockets */
	u32 scope;
	u32 filter[8];             /* ICMP6_FILTER */
	struct rt_inet_opts opt;
};

static struct raw_pcb *raw_pcbs;

static struct raw_pcb *pcb_of(struct rt_socket *so)
{
	return (struct raw_pcb *)so->pcb;
}

int rt_raw_input(const struct rt_buf *b, const struct rt_iphdr *ip)
{
	struct raw_pcb *p;
	u32 hlen = (u32)(b->data - (const u8 *)ip);
	rt_in6_addr sa, da;
	int n = 0;

	rt_in6_set_v4mapped(&sa, ip->saddr);
	rt_in6_set_v4mapped(&da, ip->daddr);
	for (p = raw_pcbs; p; p = p->next) {
		struct rt_buf *c;

		if (p->v6)
			continue;
		if (p->proto && p->proto != ip->protocol)
			continue;
		if (p->laddr && p->laddr != ip->daddr)
			continue;
		if (p->faddr && p->faddr != ip->saddr)
			continue;
		if (p->so->state & RT_SS_CANTRCVMORE)
			continue;
		c = rt_buf_alloc(0, hlen + b->len);
		if (!c)
			continue;
		memcpy(rt_buf_put(c, hlen), ip, hlen);
		rt_copy_buf(rt_buf_put(c, b->len), b->data, b->len);
		c->netif = b->netif;
		c->flags = b->flags;
		if (rt_dgram_deliver(p->so, c, &sa, 0, &da, b->netif) == 0)
			n++;
	}
	return n;
}

int rt_raw6_input(const struct rt_buf *b, const struct rt_ip6hdr *ip6, u8 nxt)
{
	struct raw_pcb *p;
	int n = 0;

	for (p = raw_pcbs; p; p = p->next) {
		struct rt_buf *c;

		if (!p->v6 || p->proto != nxt)
			continue;
		if (!rt_in6_is_unspec(&p->laddr6) && !rt_in6_eq(&p->laddr6, &ip6->dst))
			continue;
		if (!rt_in6_is_unspec(&p->faddr6) && !rt_in6_eq(&p->faddr6, &ip6->src))
			continue;
		if (p->so->state & RT_SS_CANTRCVMORE)
			continue;
		if (nxt == RT_IPPROTO_ICMPV6) {
			u8 t;
			if (b->len < 1)
				continue;
			t = b->data[0];
			if (!(p->filter[t >> 5] & (1u << (t & 31))))
				continue;
		}
		c = rt_buf_alloc(0, b->len);
		if (!c)
			continue;
		rt_copy_buf(rt_buf_put(c, b->len), b->data, b->len);
		c->netif = b->netif;
		c->flags = b->flags;
		if (rt_dgram_deliver(p->so, c, &ip6->src, 0, &ip6->dst, b->netif) == 0)
			n++;
	}
	return n;
}

static int raw_attach(struct rt_socket *so)
{
	struct raw_pcb *pcb;

	if (so->protocol > 255)
		return -RT_EPROTONOSUPPORT;
	pcb = rt_zalloc(sizeof(*pcb));
	if (!pcb)
		return -RT_ENOBUFS;
	pcb->so = so;
	pcb->proto = (u8)so->protocol;
	pcb->v6 = so->domain == RT_AF_INET6;
	rt_inet_opts_init(&pcb->opt, so->domain);
	pcb->opt.hdrincl = !pcb->v6 && pcb->proto == RT_IPPROTO_RAW;
	pcb->csum_off = pcb->v6 && pcb->proto == RT_IPPROTO_ICMPV6 ? 2 : -1;
	memset(pcb->filter, 0xff, sizeof(pcb->filter));
	so->pcb = pcb;
	pcb->next = raw_pcbs;
	raw_pcbs = pcb;
	return 0;
}

static void raw_close(struct rt_socket *so)
{
	struct raw_pcb *pcb = pcb_of(so), **pp;

	if (!pcb)
		return;
	for (pp = &raw_pcbs; *pp; pp = &(*pp)->next) {
		if (*pp == pcb) {
			*pp = pcb->next;
			break;
		}
	}
	rt_inet_mc_drop_all(&pcb->opt);
	rt_bufq_purge(&so->rcvq);
	so->pcb = NULL;
	rt_free(pcb);
}

static int raw_bind(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct raw_pcb *pcb = pcb_of(so);
	int r = rt_sin_check(so, sin, 1);

	if (r < 0)
		return r;
	if (pcb->v6) {
		if (!rt_in6_is_unspec(&sin->sin6_addr) && !rt_inet_is_local(&sin->sin6_addr))
			return -RT_EADDRNOTAVAIL;
		pcb->laddr6 = sin->sin6_addr;
		pcb->scope = sin->sin6_scope_id;
	} else {
		u32 a = rt_in6_v4(&sin->sin6_addr);
		if (a && !rt_netif_by_addr(a) && !rt_ip_is_loopback_net(a))
			return -RT_EADDRNOTAVAIL;
		pcb->laddr = a;
	}
	so->state |= RT_SS_ISBOUND;
	return 0;
}

static int raw_listen(struct rt_socket *so, int backlog)
{
	return -RT_EOPNOTSUPP;
}

static int raw_connect(struct rt_socket *so, const struct rt_sockaddr_in6 *sin)
{
	struct raw_pcb *pcb = pcb_of(so);
	int r;

	if (!sin)
		return -RT_EINVAL;
	if (sin->sin6_family == RT_AF_UNSPEC && (pcb->v6 || rt_inaddr_is_any(&sin->sin6_addr))) {
		pcb->faddr = 0;
		memset(&pcb->faddr6, 0, sizeof(pcb->faddr6));
		so->state &= ~(u32)RT_SS_ISCONNECTED;
		return 0;
	}
	r = rt_sin_check(so, sin, 1);
	if (r < 0)
		return r;
	if (rt_inaddr_is_any(&sin->sin6_addr))
		return -RT_EADDRNOTAVAIL;
	if (pcb->v6) {
		pcb->faddr6 = sin->sin6_addr;
		if (sin->sin6_scope_id)
			pcb->scope = sin->sin6_scope_id;
	} else {
		pcb->faddr = rt_in6_v4(&sin->sin6_addr);
	}
	so->state |= RT_SS_ISCONNECTED;
	return 0;
}

static int raw_send6(struct raw_pcb *pcb, struct rt_buf *b, const rt_in6_addr *dst, u32 scope, int dontroute)
{
	struct rt_inet_out o;
	rt_in6_addr src = pcb->laddr6;
	u32 len = b->len;

	if (rt_in6_is_mcast(dst) && pcb->opt.mcast_if)
		scope = pcb->opt.mcast_if;
	if (rt_in6_is_unspec(&src)) {
		struct rt_inet_route rt;
		int r = rt_inet_route(dst, scope, &rt);
		if (r < 0) {
			rt_buf_free(b);
			return r;
		}
		src = rt.src;
		if (rt_in6_is_unspec(&src)) {
			rt_buf_free(b);
			return -RT_EADDRNOTAVAIL;
		}
	}
	if (pcb->csum_off >= 0) {
		u16 ck;
		if ((u32)pcb->csum_off + 2 > len) {
			rt_buf_free(b);
			return -RT_EINVAL;
		}
		b->data[pcb->csum_off] = b->data[pcb->csum_off + 1] = 0;
		ck = rt_csum_fold(rt_csum_partial(b->data, len, rt_csum_pseudo6(&src, dst, pcb->proto, len)));
		memcpy(b->data + pcb->csum_off, &ck, 2);
	}
	memset(&o, 0, sizeof(o));
	o.src = src;
	o.dst = *dst;
	o.proto = pcb->proto;
	o.ttl = rt_in6_is_mcast(dst) ? (pcb->opt.mcast_hops >= 0 ? pcb->opt.mcast_hops : 1) : pcb->opt.hlim;
	if (o.ttl == 0)
		o.ttl = -1;
	o.tos = rt_qos_sock_tos(pcb->so, pcb->opt.tclass);
	b->prio = rt_qos_sock_prio(pcb->so);
	o.df = pcb->opt.dontfrag;
	o.dontroute = (u8)dontroute;
	o.mcast_noloop = !pcb->opt.mcast_loop6;
	o.scope = scope;
	return rt_inet_output(b, &o);
}

static int raw_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, const struct rt_sockaddr_in6 *to)
{
	struct raw_pcb *pcb = pcb_of(so);
	struct rt_buf *b;
	u32 len, daddr = 0, scope = pcb->scope;
	rt_in6_addr daddr6;
	int dontroute = (so->options & RT_SO_DONTROUTE) || (flags & RT_MSG_DONTROUTE);
	int r;

	if (so->state & RT_SS_CANTSENDMORE)
		return -RT_EPIPE;
	if (to) {
		r = rt_sin_check(so, to, 1);
		if (r < 0)
			return r;
		if (pcb->v6 ? !rt_in6_is_unspec(&pcb->faddr6) : pcb->faddr != 0)
			return -RT_EISCONN;
		daddr = rt_in6_v4(&to->sin6_addr);
		daddr6 = to->sin6_addr;
		if (to->sin6_scope_id)
			scope = to->sin6_scope_id;
	} else {
		if (pcb->v6 ? rt_in6_is_unspec(&pcb->faddr6) : !pcb->faddr)
			return -RT_ENOTCONN;
		daddr = pcb->faddr;
		daddr6 = pcb->faddr6;
	}
	len = rt_iov_total(iov, iovcnt);
	if (len > 65535 || (so->sndbuf && len > so->sndbuf))
		return -RT_EMSGSIZE;
	b = rt_buf_alloc(RT_BUF_HEADROOM, len);
	if (!b)
		return -RT_ENOBUFS;
	if (len && rt_iov_to_buf(iov, iovcnt, 0, rt_buf_put(b, len), len) != len) {
		rt_buf_free(b);
		return -RT_EFAULT;
	}
	if (pcb->v6) {
		if (rt_in6_is_unspec(&daddr6) || rt_in6_is_v4mapped(&daddr6)) {
			rt_buf_free(b);
			return -RT_EADDRNOTAVAIL;
		}
		r = raw_send6(pcb, b, &daddr6, scope, dontroute);
		return r < 0 ? r : (int)len;
	}

	if (pcb->opt.hdrincl) {
		struct rt_iphdr *ip = (struct rt_iphdr *)b->data;

		if (len < 20 || rt_ip_hlen(ip) < 20 || rt_ip_hlen(ip) > len) {
			rt_buf_free(b);
			return -RT_EINVAL;
		}
		if (!ip->daddr)
			ip->daddr = daddr;
		if (rt_ip_is_broadcast(ip->daddr, NULL) && !(so->options & RT_SO_BROADCAST)) {
			rt_buf_free(b);
			return -RT_EACCES;
		}
		r = rt_ip_output_raw(b, dontroute);
	} else {
		struct rt_ip_out o;

		if (len > 65535 - 20) {
			rt_buf_free(b);
			return -RT_EMSGSIZE;
		}
		memset(&o, 0, sizeof(o));
		o.saddr = pcb->laddr;
		o.daddr = daddr;
		o.proto = pcb->proto ? pcb->proto : RT_IPPROTO_RAW;
		o.ttl = pcb->opt.ttl;
		if (rt_ip_is_multicast(daddr)) {
			o.ttl = pcb->opt.mcast_ttl ? pcb->opt.mcast_ttl : 1;
			o.mcast_ttl0 = pcb->opt.mcast_ttl == 0;
			o.mcast_noloop = !pcb->opt.mcast_loop;
			o.oif = rt_inet_mc_oif(&pcb->opt);
			if (!o.oif)
				o.oif = rt_ip_mc_default_if(daddr);
		}
		o.tos = rt_qos_sock_tos(pcb->so, pcb->opt.tos);
		b->prio = rt_qos_sock_prio(pcb->so);
		o.dontroute = (u8)dontroute;
		o.allow_broadcast = (so->options & RT_SO_BROADCAST) != 0;
		r = rt_ip_output(b, &o);
	}
	return r < 0 ? r : (int)len;
}

static int raw_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, struct rt_sockaddr_in6 *from, int *msgflags)
{
	return rt_dgram_recv(so, iov, iovcnt, flags, from, msgflags);
}

static int raw_shutdown(struct rt_socket *so, int how)
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

static int raw_setsockopt(struct rt_socket *so, int level, int name, const void *val, u32 len)
{
	struct raw_pcb *pcb = pcb_of(so);

	if (pcb->v6) {
		if (level == RT_IPPROTO_ICMPV6 && name == RT_ICMP6_FILTER) {
			if (pcb->proto != RT_IPPROTO_ICMPV6)
				return -RT_EINVAL;
			if (!val || len < sizeof(pcb->filter))
				return -RT_EINVAL;
			memcpy(pcb->filter, val, sizeof(pcb->filter));
			return 0;
		}
		if (level == RT_IPPROTO_IPV6 && name == RT_IPV6_CHECKSUM) {
			s32 v;
			if (!val || len < 4)
				return -RT_EINVAL;
			memcpy(&v, val, 4);
			if (pcb->proto == RT_IPPROTO_ICMPV6)
				return -RT_EINVAL;   /* RFC 3542 3.1: fixed for ICMPv6 */
			if (v >= 0 && (v & 1))
				return -RT_EINVAL;
			pcb->csum_off = (s16)(v < 0 ? -1 : v);
			return 0;
		}
		if (level == RT_IPPROTO_IPV6)
			return rt_inet6_setopt(so, &pcb->opt, name, val, len);
		return -RT_ENOPROTOOPT;
	}
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_setopt(&pcb->opt, name, val, len, 1);
}

static int raw_getsockopt(struct rt_socket *so, int level, int name, void *val, u32 *len)
{
	struct raw_pcb *pcb = pcb_of(so);

	if (pcb->v6) {
		if (level == RT_IPPROTO_ICMPV6 && name == RT_ICMP6_FILTER) {
			if (!val || !len || *len < sizeof(pcb->filter))
				return -RT_EINVAL;
			memcpy(val, pcb->filter, sizeof(pcb->filter));
			*len = sizeof(pcb->filter);
			return 0;
		}
		if (level == RT_IPPROTO_IPV6 && name == RT_IPV6_CHECKSUM) {
			s32 v = pcb->csum_off;
			if (!val || !len || *len < 4)
				return -RT_EINVAL;
			memcpy(val, &v, 4);
			*len = 4;
			return 0;
		}
		if (level == RT_IPPROTO_IPV6)
			return rt_inet6_getopt(&pcb->opt, name, val, len);
		return -RT_ENOPROTOOPT;
	}
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_getopt(&pcb->opt, name, val, len, 1);
}

static int raw_getname(struct rt_socket *so, struct rt_sockaddr_in6 *sin, int peer)
{
	struct raw_pcb *pcb = pcb_of(so);
	rt_in6_addr a;

	if (pcb->v6) {
		a = peer ? pcb->faddr6 : pcb->laddr6;
		if (peer && rt_in6_is_unspec(&a))
			return -RT_ENOTCONN;
	} else {
		u32 v = peer ? pcb->faddr : pcb->laddr;
		if (peer && !v)
			return -RT_ENOTCONN;
		rt_in6_set_v4mapped(&a, v);
	}
	rt_sin_fill(so, sin, &a, 0, pcb->scope);
	return 0;
}

const struct rt_proto_ops rt_raw_ops = {
	"raw",
	raw_attach,
	raw_close,
	raw_bind,
	raw_listen,
	raw_connect,
	raw_send,
	raw_recv,
	raw_shutdown,
	raw_setsockopt,
	raw_getsockopt,
	rt_dgram_poll,
	raw_getname,
	rt_dgram_ioctl,
};

int rt_raw_list(struct rt_pcb_info *out, int max)
{
	const struct raw_pcb *p;
	int n = 0;

	for (p = raw_pcbs; p; p = p->next, n++) {
		if (!out || n >= max)
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		out[n].proto = p->proto;
		out[n].laddr = p->laddr;
		out[n].faddr = p->faddr;
		out[n].v6 = p->v6;
		if (p->v6) {
			out[n].laddr6 = p->laddr6;
			out[n].faddr6 = p->faddr6;
		} else {
			rt_in6_set_v4mapped(&out[n].laddr6, p->laddr);
			rt_in6_set_v4mapped(&out[n].faddr6, p->faddr);
		}
		out[n].recvq = p->so->rcvq.bytes;
	}
	return n;
}

void rt_raw_init(void)
{
	raw_pcbs = NULL;
}
