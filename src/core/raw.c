/*
 * Riptide - raw IP sockets (SOCK_RAW), as used by ping and traceroute.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * BSD semantics: a raw socket of protocol P receives a copy of every
 * incoming datagram of protocol P (protocol 0: all protocols), IP header
 * included, filtered by bound local and connected foreign address. The
 * header is delivered as received (network order fields). On output the
 * stack builds the IP header unless IP_HDRINCL is set (implied for
 * IPPROTO_RAW), in which case the application supplies it and the stack
 * fills in length, checksum and, when zero, id and source address.
 */
#include "infra.h"

struct raw_pcb {
	struct raw_pcb *next;
	struct rt_socket *so;
	u8  proto;
	u32 laddr, faddr;
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
	int n = 0;

	for (p = raw_pcbs; p; p = p->next) {
		struct rt_buf *c;

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
		memcpy(rt_buf_put(c, b->len), b->data, b->len);
		c->netif = b->netif;
		c->flags = b->flags;
		if (rt_dgram_deliver(p->so, c, ip->saddr, 0, ip->daddr, b->netif) == 0)
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
	pcb->opt.hdrincl = pcb->proto == RT_IPPROTO_RAW;
	pcb->opt.mcast_ttl = 1;
	pcb->opt.mcast_loop = 1;
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
	rt_bufq_purge(&so->rcvq);
	so->pcb = NULL;
	rt_free(pcb);
}

static int raw_bind(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	int r = rt_sin_check(sin);

	if (r < 0)
		return r;
	if (sin->sin_addr && !rt_netif_by_addr(sin->sin_addr) && !rt_ip_is_loopback_net(sin->sin_addr))
		return -RT_EADDRNOTAVAIL;
	pcb_of(so)->laddr = sin->sin_addr;
	so->state |= RT_SS_ISBOUND;
	return 0;
}

static int raw_listen(struct rt_socket *so, int backlog)
{
	return -RT_EOPNOTSUPP;
}

static int raw_connect(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	struct raw_pcb *pcb = pcb_of(so);
	int r;

	if (!sin)
		return -RT_EINVAL;
	if (sin->sin_family == RT_AF_UNSPEC && sin->sin_addr == 0) {
		pcb->faddr = 0;
		so->state &= ~(u32)RT_SS_ISCONNECTED;
		return 0;
	}
	r = rt_sin_check(sin);
	if (r < 0)
		return r;
	if (!sin->sin_addr)
		return -RT_EADDRNOTAVAIL;
	pcb->faddr = sin->sin_addr;
	so->state |= RT_SS_ISCONNECTED;
	return 0;
}

static int raw_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, const struct rt_sockaddr_in *to)
{
	struct raw_pcb *pcb = pcb_of(so);
	struct rt_buf *b;
	u32 len, daddr;
	int dontroute = (so->options & RT_SO_DONTROUTE) || (flags & RT_MSG_DONTROUTE);
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
	} else {
		if (!pcb->faddr)
			return -RT_ENOTCONN;
		daddr = pcb->faddr;
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
		o.ttl = rt_ip_is_multicast(daddr) ? pcb->opt.mcast_ttl : pcb->opt.ttl;
		o.tos = pcb->opt.tos;
		o.dontroute = (u8)dontroute;
		o.allow_broadcast = (so->options & RT_SO_BROADCAST) != 0;
		r = rt_ip_output(b, &o);
	}
	return r < 0 ? r : (int)len;
}

static int raw_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                    int flags, struct rt_sockaddr_in *from, int *msgflags)
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
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_setopt(&pcb_of(so)->opt, name, val, len, 1);
}

static int raw_getsockopt(struct rt_socket *so, int level, int name, void *val, u32 *len)
{
	if (level != RT_IPPROTO_IP)
		return -RT_ENOPROTOOPT;
	return rt_inet_getopt(&pcb_of(so)->opt, name, val, len, 1);
}

static int raw_getname(struct rt_socket *so, struct rt_sockaddr_in *sin, int peer)
{
	struct raw_pcb *pcb = pcb_of(so);
	u32 a = peer ? pcb->faddr : pcb->laddr;

	if (peer && !a)
		return -RT_ENOTCONN;
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = RT_AF_INET;
	sin->sin_addr = a;
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
		out[n].recvq = p->so->rcvq.bytes;
	}
	return n;
}

void rt_raw_init(void)
{
	raw_pcbs = NULL;
}
