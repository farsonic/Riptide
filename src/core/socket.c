/*
 * Riptide - socket objects and the services the socket layer offers to
 * protocols (wakeups, accept queues, datagram queues, iov copies).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Lifetime: a socket is referenced by descriptors (so->refcnt), by the
 * ObtainSocket list (release_id != -1, counted in refcnt as well) and,
 * while unaccepted, by its listener's accept queue. When the last reference
 * goes, sock_close() closes the protocol. If a task is still asleep on
 * so->wq (blocking call or WaitSelect from another task sharing the
 * socket), the socket is switched to dead ops and freed by the last waiter
 * (sock_waiter_done), so sleepers never touch freed memory.
 */
#include "sock_internal.h"

struct rt_socket *rt_all_sockets;
void (*rt_sock_async_hook)(struct rt_socket *so, u32 events);
const struct rt_proto_ops *(*rt_sock_proto_hook)(int type, int protocol);

/* Default buffer sizes. TCP reads so->sndbuf/rcvbuf at attach time. */
s32 sock_tcp_sendspace = 32768;
s32 sock_tcp_recvspace = 65536;
s32 sock_udp_sendspace = 9216;     /* BSD udp_sendspace: max datagram */
s32 sock_udp_recvspace = 41600;    /* BSD: 40 * (1024 + sizeof(sin)) */
s32 sock_raw_sendspace = 8192;
s32 sock_raw_recvspace = 8192;
s32 sock_maxsockbuf = 262144;      /* kern.ipc.maxsockbuf */
s32 sock_somaxconn = RT_SOMAXCONN;

/* Rough per-datagram overhead charged against rcvbuf in addition to the
 * payload (buffer header, headroom), cf. BSD mbuf / Linux truesize. */
#define SOCK_DGRAM_OVERHEAD 256u

/* ---- dead ops: what a closed socket answers while waiters drain ---- */
static int dead_bind(struct rt_socket *so, const struct rt_sockaddr_in *sin) { return -RT_EBADF; }
static int dead_listen(struct rt_socket *so, int backlog) { return -RT_EBADF; }
static int dead_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                     int flags, const struct rt_sockaddr_in *to) { return -RT_EBADF; }
static int dead_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
                     int flags, struct rt_sockaddr_in *from, int *msgflags) { return -RT_EBADF; }
static int dead_shutdown(struct rt_socket *so, int how) { return -RT_EBADF; }
static int dead_setsockopt(struct rt_socket *so, int l, int n, const void *v, u32 len) { return -RT_EBADF; }
static int dead_getsockopt(struct rt_socket *so, int l, int n, void *v, u32 *len) { return -RT_EBADF; }
static u32 dead_poll(struct rt_socket *so) { return RT_POLLIN | RT_POLLOUT | RT_POLLERR | RT_POLLHUP; }
static int dead_getname(struct rt_socket *so, struct rt_sockaddr_in *sin, int peer) { return -RT_EBADF; }
static int dead_ioctl(struct rt_socket *so, u32 req, void *arg) { return -RT_EBADF; }
static void dead_close(struct rt_socket *so) { }

static const struct rt_proto_ops sock_dead_ops = {
	"dead", NULL, dead_close, dead_bind, dead_listen, dead_bind, dead_send,
	dead_recv, dead_shutdown, dead_setsockopt, dead_getsockopt, dead_poll,
	dead_getname, dead_ioctl
};

/* ---- allocation ---- */

const struct rt_proto_ops *sock_proto_lookup(int type, int *protocol)
{
	const struct rt_proto_ops *ops;
	int p = *protocol;

	if (rt_sock_proto_hook && (ops = rt_sock_proto_hook(type, p)) != NULL)
		return ops;
	switch (type) {
	case RT_SOCK_STREAM:
		if (p != 0 && p != RT_IPPROTO_TCP)
			return NULL;
		*protocol = RT_IPPROTO_TCP;
		return &rt_tcp_ops;
	case RT_SOCK_DGRAM:
		if (p != 0 && p != RT_IPPROTO_UDP)
			return NULL;
		*protocol = RT_IPPROTO_UDP;
		return &rt_udp_ops;
	case RT_SOCK_RAW:
		if (p < 0 || p > 255)
			return NULL;
		return &rt_raw_ops;
	}
	return NULL;
}

struct rt_socket *sock_alloc(int type, int protocol, const struct rt_proto_ops *ops)
{
	struct rt_socket *so = rt_zalloc(sizeof(*so));

	if (!so)
		return NULL;
	so->type = (u16)type;
	so->protocol = (u16)protocol;
	so->ops = ops;
	so->release_id = -1;
	so->sndtimeo_ms = -1;
	so->rcvtimeo_ms = -1;
	rt_waitq_init(&so->wq);
	rt_bufq_init(&so->rcvq);
	switch (type) {
	case RT_SOCK_STREAM:
		so->sndbuf = (u32)sock_tcp_sendspace;
		so->rcvbuf = (u32)sock_tcp_recvspace;
		break;
	case RT_SOCK_DGRAM:
		so->sndbuf = (u32)sock_udp_sendspace;
		so->rcvbuf = (u32)sock_udp_recvspace;
		break;
	default:
		so->sndbuf = (u32)sock_raw_sendspace;
		so->rcvbuf = (u32)sock_raw_recvspace;
		break;
	}
	so->sndlowat = RT_MIN(2048u, so->sndbuf);   /* BSD: MCLBYTES */
	so->rcvlowat = 1;
	so->next_all = rt_all_sockets;
	rt_all_sockets = so;
	return so;
}

void sock_free(struct rt_socket *so)
{
	struct rt_socket **pp;

	for (pp = &rt_all_sockets; *pp; pp = &(*pp)->next_all) {
		if (*pp == so) {
			*pp = so->next_all;
			break;
		}
	}
	rt_bufq_purge(&so->rcvq);
	rt_free(so);
}

void sock_waiter_done(struct rt_socket *so)
{
	if ((so->state & SOCK_SS_DEAD) && so->wq.head == NULL)
		sock_free(so);
}

/* Unlink a child from its listener's accept queue. Returns 1 if it was
 * queued. */
static int acceptq_unlink(struct rt_socket *child)
{
	struct rt_socket *l = child->listen_parent, *p, *prev = NULL;

	if (!l)
		return 0;
	for (p = l->acceptq_head; p; prev = p, p = p->acceptq_next) {
		if (p != child)
			continue;
		if (prev)
			prev->acceptq_next = p->acceptq_next;
		else
			l->acceptq_head = p->acceptq_next;
		if (l->acceptq_tail == p)
			l->acceptq_tail = prev;
		p->acceptq_next = NULL;
		l->acceptq_len--;
		return 1;
	}
	return 0;
}

void sock_close(struct rt_socket *so)
{
	struct rt_socket *ch, *s;

	so->state |= RT_SS_NOFDREF;
	/* 4.4BSD soclose(): abort connections nobody accepted yet. */
	while ((ch = so->acceptq_head) != NULL) {
		so->acceptq_head = ch->acceptq_next;
		ch->acceptq_next = NULL;
		ch->listen_parent = NULL;
		ch->linger_on = 1;
		ch->linger_s = 0;           /* abort (RST) */
		if (ch->ops && ch->ops->close)
			ch->ops->close(ch);
		sock_free(ch);
	}
	so->acceptq_tail = NULL;
	so->acceptq_len = 0;
	if (so->ops && so->ops->close)
		so->ops->close(so);
	/* Embryonic children the protocol did not drop must not point at us. */
	for (s = rt_all_sockets; s; s = s->next_all)
		if (s->listen_parent == so)
			s->listen_parent = NULL;
	if (so->listen_parent)
		acceptq_unlink(so);
	so->ops = &sock_dead_ops;
	so->pcb = NULL;
	rt_bufq_purge(&so->rcvq);
	if (so->wq.head) {
		so->state |= SOCK_SS_DEAD;
		rt_waitq_wake(&so->wq);
	} else {
		sock_free(so);
	}
}

int sock_kernel_create(int type, int protocol, struct sock_upcall *up,
                       struct rt_socket **out)
{
	const struct rt_proto_ops *ops = sock_proto_lookup(type, &protocol);
	struct rt_socket *so;
	int r;

	if (!ops)
		return -RT_EPROTONOSUPPORT;
	so = sock_alloc(type, protocol, ops);
	if (!so)
		return -RT_ENOMEM;
	so->state |= RT_SS_NBIO;
	if (up) {
		so->state |= SOCK_SS_UPCALL;
		so->owner = up;
	}
	so->refcnt = 1;
	r = ops->attach ? ops->attach(so) : 0;
	if (r < 0) {
		sock_free(so);
		return r;
	}
	*out = so;
	return 0;
}

void sock_kernel_close(struct rt_socket *so)
{
	if (!so)
		return;
	so->state &= ~SOCK_SS_UPCALL;
	so->owner = NULL;
	so->refcnt = 0;
	sock_close(so);
}

u32 sock_poll(struct rt_socket *so)
{
	u32 m = so->ops && so->ops->poll ? so->ops->poll(so) : 0;

	if (so->so_error)
		m |= RT_POLLERR;
	if (so->acceptq_head)
		m |= RT_POLLIN;
	if (!rt_bufq_empty(&so->rcvq) || (so->state & RT_SS_CANTRCVMORE))
		m |= RT_POLLIN;
	return m;
}

/* ---- services for protocols ---- */

void rt_sock_wakeup(struct rt_socket *so, u32 events)
{
	u32 pend;

	if (!so)
		return;
	rt_waitq_wake(&so->wq);
	if (!events)
		return;
	if (so->state & SOCK_SS_UPCALL) {
		struct sock_upcall *up = so->owner;
		if (up && up->fn)
			up->fn(so, events, up->arg);
		return;
	}
	/* Events the owner selected with SO_EVENTMASK are remembered for
	 * GetSocketEvents(); FIOASYNC owners get every event (SIGIO/SIGURG). */
	pend = events & so->event_mask;
	if (pend)
		so->events_pending |= pend;
	if ((pend || (so->state & RT_SS_ASYNC)) && rt_sock_async_hook)
		rt_sock_async_hook(so, events);
}

void rt_sock_set_error(struct rt_socket *so, int err)
{
	if (!so)
		return;
	if (err < 0)
		err = -err;
	so->so_error = err;
	rt_sock_wakeup(so, RT_SEV_ERROR);
}

struct rt_socket *rt_sock_new_child(struct rt_socket *l)
{
	struct rt_socket *ch;

	if (!l || !(l->state & RT_SS_ISLISTENING) || (l->state & RT_SS_NOFDREF))
		return NULL;
	/* Linux sk_acceptq_is_full(): up to backlog + 1 queued connections. */
	if (l->acceptq_len > l->backlog)
		return NULL;
	ch = sock_alloc(l->type, l->protocol, l->ops);
	if (!ch)
		return NULL;
	ch->state = RT_SS_NOFDREF | (l->state & (RT_SS_NBIO | RT_SS_ASYNC));
	ch->options = l->options & ~RT_SO_ACCEPTCONN;
	ch->linger_on = l->linger_on;
	ch->linger_s = l->linger_s;
	ch->sndbuf = l->sndbuf;
	ch->rcvbuf = l->rcvbuf;
	ch->sndlowat = l->sndlowat;
	ch->rcvlowat = l->rcvlowat;
	ch->sndtimeo_ms = l->sndtimeo_ms;
	ch->rcvtimeo_ms = l->rcvtimeo_ms;
	ch->owner = l->owner;
	ch->pgid = l->pgid;
	ch->event_mask = l->event_mask;
	ch->listen_parent = l;
	return ch;
}

void rt_sock_child_ready(struct rt_socket *l, struct rt_socket *ch)
{
	if (!l || !ch)
		return;
	if (ch->listen_parent && ch->listen_parent != l)
		acceptq_unlink(ch);
	ch->listen_parent = l;
	ch->acceptq_next = NULL;
	/* queued children are established by definition (soisconnected()) */
	ch->state = (ch->state & ~(u32)RT_SS_ISCONNECTING) | RT_SS_ISCONNECTED;
	if (l->acceptq_tail)
		l->acceptq_tail->acceptq_next = ch;
	else
		l->acceptq_head = ch;
	l->acceptq_tail = ch;
	l->acceptq_len++;
	rt_sock_wakeup(l, RT_SEV_ACCEPT);
}

void rt_sock_child_drop(struct rt_socket *ch)
{
	if (!ch)
		return;
	acceptq_unlink(ch);
	ch->listen_parent = NULL;
	if (ch->wq.head) {
		/* should not happen: nobody can wait on an unaccepted socket */
		ch->ops = &sock_dead_ops;
		ch->pcb = NULL;
		ch->state |= SOCK_SS_DEAD;
		rt_waitq_wake(&ch->wq);
		return;
	}
	sock_free(ch);
}

int rt_sock_queue_dgram(struct rt_socket *so, struct rt_buf *b)
{
	struct rt_bufq *q = &so->rcvq;

	if (so->state & (RT_SS_CANTRCVMORE | RT_SS_NOFDREF))
		return -RT_ENOBUFS;
	/* An empty queue always takes one datagram (Linux behaviour), so a
	 * small SO_RCVBUF never blocks reception completely. */
	if (q->qlen && q->bytes + (q->qlen + 1) * SOCK_DGRAM_OVERHEAD + b->len > so->rcvbuf)
		return -RT_ENOBUFS;
	rt_bufq_tail(q, b);
	rt_sock_wakeup(so, RT_SEV_READ);
	return 0;
}

/* ---- iov helpers ---- */

u32 rt_iov_total(const struct rt_iov *iov, int iovcnt)
{
	u32 t = 0;
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (iov[i].len > 0x7fffffffu - t)
			return 0x7fffffffu;
		t += iov[i].len;
	}
	return t;
}

u32 rt_iov_to_buf(const struct rt_iov *iov, int iovcnt, u32 offset, void *dst, u32 len)
{
	u8 *d = dst;
	u32 done = 0;
	int i;

	for (i = 0; i < iovcnt && done < len; i++) {
		u32 n;
		if (offset >= iov[i].len) {
			offset -= iov[i].len;
			continue;
		}
		n = RT_MIN(iov[i].len - offset, len - done);
		memcpy(d + done, (const u8 *)iov[i].base + offset, n);
		done += n;
		offset = 0;
	}
	return done;
}

u32 rt_buf_to_iov(const struct rt_iov *iov, int iovcnt, u32 offset, const void *src, u32 len)
{
	const u8 *s = src;
	u32 done = 0;
	int i;

	for (i = 0; i < iovcnt && done < len; i++) {
		u32 n;
		if (offset >= iov[i].len) {
			offset -= iov[i].len;
			continue;
		}
		n = RT_MIN(iov[i].len - offset, len - done);
		memcpy((u8 *)iov[i].base + offset, s + done, n);
		done += n;
		offset = 0;
	}
	return done;
}

/* ---- sysctl ---- */

static const struct rt_sysctl sock_sysctls[] = {
	/* MIBs: 4.4BSD/Miami TCPCTL_SENDSPACE 8, TCPCTL_RECVSPACE 9; Miami
	 * CTL_SOCKET(11).maxqlen(0). The UDP buffer defaults belong to udp.c
	 * (net.inet.udp.recvspace / maxdgram), whose attach sets them. */
	{ "net.inet.tcp.sendspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, RT_AF_INET, RT_IPPROTO_TCP, 8 },
	  &sock_tcp_sendspace, 512, 16 * 1024 * 1024, NULL, "default TCP send buffer" },
	{ "net.inet.tcp.recvspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 4, { RT_CTL_NET, RT_AF_INET, RT_IPPROTO_TCP, 9 },
	  &sock_tcp_recvspace, 512, 16 * 1024 * 1024, NULL, "default TCP receive buffer" },
	{ "net.inet.raw.sendspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &sock_raw_sendspace, 512, 65535, NULL, "default raw send buffer" },
	{ "net.inet.raw.recvspace", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &sock_raw_recvspace, 512, 16 * 1024 * 1024, NULL, "default raw receive buffer" },
	{ "kern.ipc.maxsockbuf", RT_SYSCTL_INT, RT_SYSCTL_RW, 0, { 0 },
	  &sock_maxsockbuf, 4096, 64 * 1024 * 1024, NULL, "maximum SO_SNDBUF/SO_RCVBUF" },
	{ "socket.maxqlen", RT_SYSCTL_INT, RT_SYSCTL_RW, 2, { 11, 0 },
	  &sock_somaxconn, 1, RT_SOMAXCONN, NULL, "maximum listen() backlog" },
};

void rt_socket_init(void)
{
	rt_sysctl_register(sock_sysctls, (int)RT_ARRAY_SIZE(sock_sysctls));
}
