/*
 * Riptide - unit tests of the socket layer (socket.c / socket_api.c) on a
 * fake in-memory stream protocol: descriptor tables, dup2, blocking and
 * non-blocking connect/accept/send/recv, timeouts, select, options,
 * ObtainSocket/ReleaseSocket, events, close while blocked. No TUN.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "rt/riptide.h"
#include "../src/core/sock_internal.h"

__attribute__((weak)) struct rt_tcp_stats rt_tcpstat;
__attribute__((weak)) const struct rt_proto_ops rt_tcp_ops;
__attribute__((weak)) void rt_tcp_init(void) {}
__attribute__((weak)) void rt_tcp_input(struct rt_buf *b, const struct rt_iphdr *ip) { rt_buf_free(b); }
__attribute__((weak)) void rt_tcp_icmp_error(u8 t, u8 c, u32 i, const struct rt_iphdr *in, u32 l) {}
__attribute__((weak)) int rt_tcp_list(struct rt_pcb_info *o, int m) { return 0; }

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ======================================================================
 * Fake stream protocol: a pair of 256 byte pipes. connect() to a port
 * finds the listener of that port; completion is immediate (fk_instant)
 * or done later by fk_complete() from another thread.
 * ====================================================================== */
#define FK_CAP 256
struct fk_pcb {
	struct rt_socket *so;
	struct fk_pcb *peer;
	u8  buf[FK_CAP];
	u32 len;
	int eof;
	u16 port;
	struct rt_socket *pending_child;  /* connector: child waiting for completion */
	struct rt_socket *pending_listener;
};
static struct rt_socket *fk_listeners[64];
static int fk_instant = 1, fk_closes, fk_sockopt_calls;

static int fk_attach(struct rt_socket *so)
{
	struct fk_pcb *p = calloc(1, sizeof(*p));
	p->so = so;
	so->pcb = p;
	return 0;
}
static void fk_close(struct rt_socket *so)
{
	struct fk_pcb *p = so->pcb;
	int i;
	fk_closes++;
	if (!p)
		return;
	for (i = 0; i < 64; i++)
		if (fk_listeners[i] == so)
			fk_listeners[i] = NULL;
	if (p->peer) {
		p->peer->eof = 1;
		p->peer->peer = NULL;
		rt_sock_wakeup(p->peer->so, RT_SEV_CLOSE);
	}
	if (p->pending_child) {
		struct fk_pcb *cp = p->pending_child->pcb;
		free(cp);
		rt_sock_child_drop(p->pending_child);
	}
	free(p);
	so->pcb = NULL;
}
static int fk_bind(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	((struct fk_pcb *)so->pcb)->port = rt_ntohs(sin->sin_port);
	return 0;
}
static int fk_listen(struct rt_socket *so, int backlog)
{
	fk_listeners[((struct fk_pcb *)so->pcb)->port % 64] = so;
	return 0;
}
static void fk_link(struct rt_socket *so)
{
	struct fk_pcb *p = so->pcb;
	struct rt_socket *ch = p->pending_child;
	struct fk_pcb *cp = ch->pcb;
	p->peer = cp;
	cp->peer = p;
	p->pending_child = NULL;
	ch->state |= RT_SS_ISCONNECTED;
	so->state |= RT_SS_ISCONNECTED;
	so->state &= ~RT_SS_ISCONNECTING;
	rt_sock_child_ready(p->pending_listener, ch);
	rt_sock_wakeup(so, RT_SEV_CONNECT);
}
static int fk_connect(struct rt_socket *so, const struct rt_sockaddr_in *sin)
{
	struct fk_pcb *p = so->pcb;
	struct rt_socket *l = fk_listeners[rt_ntohs(sin->sin_port) % 64], *ch;
	struct fk_pcb *cp;
	if (!l)
		return -RT_ECONNREFUSED;
	ch = rt_sock_new_child(l);
	if (!ch)
		return -RT_ECONNREFUSED;
	cp = calloc(1, sizeof(*cp));
	cp->so = ch;
	cp->port = rt_ntohs(sin->sin_port);
	ch->pcb = cp;
	p->pending_child = ch;
	p->pending_listener = l;
	if (fk_instant)
		fk_link(so);
	return -RT_EINPROGRESS;
}
static void fk_complete(struct rt_socket *so) { fk_link(so); }
static void fk_fail(struct rt_socket *so, int err)
{
	struct fk_pcb *p = so->pcb;
	free(p->pending_child->pcb);
	rt_sock_child_drop(p->pending_child);
	p->pending_child = NULL;
	so->state &= ~RT_SS_ISCONNECTING;
	rt_sock_set_error(so, err);
}
static int fk_send(struct rt_socket *so, const struct rt_iov *iov, int iovcnt, int flags,
                   const struct rt_sockaddr_in *to)
{
	struct fk_pcb *p = so->pcb, *q = p->peer;
	u32 n, total = rt_iov_total(iov, iovcnt);
	if (!q)
		return p->eof ? -RT_EPIPE : -RT_ENOTCONN;
	n = RT_MIN(total, FK_CAP - q->len);
	if (n == 0)
		return -RT_EWOULDBLOCK;
	rt_iov_to_buf(iov, iovcnt, 0, q->buf + q->len, n);
	q->len += n;
	rt_sock_wakeup(q->so, RT_SEV_READ);
	return (int)n;
}
static int fk_recv(struct rt_socket *so, const struct rt_iov *iov, int iovcnt, int flags,
                   struct rt_sockaddr_in *from, int *mf)
{
	struct fk_pcb *p = so->pcb;
	u32 n;
	if (p->len == 0)
		return p->eof ? 0 : -RT_EWOULDBLOCK;
	n = rt_buf_to_iov(iov, iovcnt, 0, p->buf, p->len);
	if (!(flags & RT_MSG_PEEK)) {
		memmove(p->buf, p->buf + n, p->len - n);
		p->len -= n;
		if (p->peer)
			rt_sock_wakeup(p->peer->so, RT_SEV_WRITE);
	}
	return (int)n;
}
static int fk_shutdown(struct rt_socket *so, int how)
{
	struct fk_pcb *p = so->pcb;
	if (how != 0 && p->peer) {
		p->peer->eof = 1;
		rt_sock_wakeup(p->peer->so, RT_SEV_READ);
	}
	return 0;
}
static int fk_setsockopt(struct rt_socket *so, int level, int name, const void *v, u32 len)
{
	fk_sockopt_calls++;
	if (level == RT_SOL_SOCKET)
		return name == RT_SO_KEEPALIVE ? 0 : -RT_ENOPROTOOPT;
	if (level == 6 && name == 1)
		return 0;
	return -RT_ENOPROTOOPT;
}
static int fk_getsockopt(struct rt_socket *so, int level, int name, void *v, u32 *len)
{
	return -RT_ENOPROTOOPT;
}
static u32 fk_poll(struct rt_socket *so)
{
	struct fk_pcb *p = so->pcb;
	u32 m = 0;
	if (p->len || p->eof)
		m |= RT_POLLIN;
	if (p->peer && p->peer->len < FK_CAP)
		m |= RT_POLLOUT;
	return m;
}
static int fk_getname(struct rt_socket *so, struct rt_sockaddr_in *sin, int peer)
{
	struct fk_pcb *p = so->pcb;
	if (peer && !p->peer)
		return -RT_ENOTCONN;
	sin->sin_port = rt_htons(peer ? p->peer->port : p->port);
	sin->sin_addr = rt_htonl(0x7f000001);
	return 0;
}
static int fk_ioctl(struct rt_socket *so, u32 req, void *arg)
{
	struct fk_pcb *p = so->pcb;
	if (req == RT_FIONREAD) {
		s32 v = (s32)p->len;
		memcpy(arg, &v, 4);
		return 0;
	}
	return -RT_EINVAL;
}
static const struct rt_proto_ops fk_ops = {
	"fake", fk_attach, fk_close, fk_bind, fk_listen, fk_connect, fk_send, fk_recv,
	fk_shutdown, fk_setsockopt, fk_getsockopt, fk_poll, fk_getname, fk_ioctl
};
static const struct rt_proto_ops *fk_hook(int type, int proto)
{
	return &fk_ops;
}

/* ====================================================================== */
static struct rt_sockctx C, C2;

static struct rt_sockaddr_in sa(u16 port)
{
	struct rt_sockaddr_in s;
	memset(&s, 0, sizeof(s));
	s.sin_len = sizeof(s);
	s.sin_family = RT_AF_INET;
	s.sin_port = rt_htons(port);
	s.sin_addr = rt_htonl(0x7f000001);
	return s;
}

static int mksock(struct rt_sockctx *c)
{
	return rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, 0);
}

/* listener on port, a connected pair (*cl, *sv) */
static int mkpair(u16 port, int *lfd, int *cl, int *sv)
{
	struct rt_sockaddr_in s = sa(port);
	*lfd = mksock(&C);
	if (rt_api_bind(&C, *lfd, &s, sizeof(s)) < 0 || rt_api_listen(&C, *lfd, 5) < 0)
		return -1;
	*cl = mksock(&C);
	if (rt_api_connect(&C, *cl, &s, sizeof(s)) != 0)
		return -1;
	*sv = rt_api_accept(&C, *lfd, NULL, NULL);
	return *sv >= 0 ? 0 : -1;
}

static u32 now_ms(void) { return rt_now_ms(); }

static void test_fdtable(void)
{
	int fds[70], i, n;

	CHECK(mksock(&C) == 0);
	CHECK(mksock(&C) == 1);
	CHECK(mksock(&C) == 2);
	CHECK(rt_api_close(&C, 1) == 0);
	CHECK(rt_api_close(&C, 1) == -RT_EBADF);
	CHECK(mksock(&C) == 1);
	CHECK(rt_api_socket(&C, 1, RT_SOCK_STREAM, 0) == -RT_EAFNOSUPPORT);
	CHECK(rt_api_socket(&C, RT_AF_INET, 9, 0) == -RT_ESOCKTNOSUPPORT);
	for (i = 0; i < 3; i++)
		rt_api_close(&C, i);
	for (n = 0; n < 70; n++) {
		fds[n] = mksock(&C);
		if (fds[n] < 0)
			break;
	}
	CHECK(n == 64 && fds[64] == -RT_EMFILE);
	CHECK(rt_ctx_set_dtablesize(&C, 32) == -RT_EINVAL);
	CHECK(rt_ctx_set_dtablesize(&C, 128) == 0 && C.nfds == 128);
	CHECK(mksock(&C) == 64);
	CHECK(rt_ctx_set_dtablesize(&C, 5000) == -RT_EINVAL);
	for (i = 0; i <= 64; i++)
		rt_api_close(&C, i);
	CHECK(rt_ctx_set_dtablesize(&C, 64) == 0);
	CHECK(rt_api_close(&C, -1) == -RT_EBADF && rt_api_close(&C, 64) == -RT_EBADF);
}

static void test_dup(void)
{
	int l, a, b, d, closes;
	char buf[8];

	CHECK(mkpair(10, &l, &a, &b) == 0);
	d = rt_api_dup2(&C, a, -1);
	CHECK(d >= 0 && d != a);
	closes = fk_closes;
	CHECK(rt_api_close(&C, a) == 0 && fk_closes == closes);    /* still referenced */
	CHECK(rt_api_send(&C, d, "hi", 2, 0) == 2);
	CHECK(rt_api_recv(&C, b, buf, sizeof(buf), 0) == 2 && !memcmp(buf, "hi", 2));
	CHECK(rt_api_dup2(&C, d, 20) == 20);
	CHECK(rt_api_dup2(&C, d, d) == d);
	CHECK(rt_api_dup2(&C, d, 64) == -RT_EBADF);
	CHECK(rt_api_dup2(&C, 63, 5) == -RT_EBADF);
	CHECK(rt_api_close(&C, d) == 0 && fk_closes == closes);
	CHECK(rt_api_close(&C, 20) == 0 && fk_closes == closes + 1);
	/* the peer sees EOF */
	CHECK(rt_api_recv(&C, b, buf, sizeof(buf), 0) == 0);
	/* dup2 onto an open descriptor closes it */
	d = mksock(&C);
	CHECK(rt_api_dup2(&C, b, d) == d && fk_closes == closes + 2);
	rt_api_close(&C, b);
	rt_api_close(&C, d);
	rt_api_close(&C, l);
	/* Dup2Socket(-1, fd) reserves fd */
	CHECK(rt_api_dup2(&C, -1, 0) == 0);
	d = mksock(&C);
	CHECK(d == 1);
	CHECK(rt_api_send(&C, 0, "x", 1, 0) == -RT_ENOTSOCK);
	CHECK(rt_api_close(&C, 0) == 0);
	CHECK(mksock(&C) == 0);
	rt_api_close(&C, 0);
	rt_api_close(&C, 1);
}

struct thr_arg { int fd; int r; u32 delay; char buf[2048]; u32 len; };

static void *recv_thread(void *v)
{
	struct thr_arg *t = v;
	t->r = rt_api_recv(&C, t->fd, t->buf, t->len, 0);
	return NULL;
}
static void *drain_thread(void *v)
{
	struct thr_arg *t = v;
	u32 got = 0;
	usleep(t->delay * 1000);
	while (got < t->len) {
		int r = rt_api_recv(&C, t->fd, t->buf + got, t->len - got, 0);
		if (r <= 0)
			break;
		got += (u32)r;
		usleep(2000);
	}
	t->r = (int)got;
	return NULL;
}
static void *dribble_thread(void *v)
{
	struct thr_arg *t = v;
	int i;
	for (i = 0; i < 10; i++) {
		usleep(3000);
		rt_api_send(&C, t->fd, "0123456789", 10, 0);
	}
	return NULL;
}

static void test_blocking(void)
{
	int l, a, b, r, on = 1, off = 0;
	struct rt_timeval tv = { 0, 100000 };
	struct thr_arg t;
	pthread_t th;
	char big[1000], buf[1024];
	u32 t0, len;

	CHECK(mkpair(11, &l, &a, &b) == 0);
	/* SO_RCVTIMEO: EWOULDBLOCK after ~100 ms */
	CHECK(rt_api_setsockopt(&C, b, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
	t0 = now_ms();
	CHECK(rt_api_recv(&C, b, buf, 10, 0) == -RT_EWOULDBLOCK);
	r = (int)(now_ms() - t0);
	CHECK(r >= 95 && r < 1000);
	memset(&tv, 0xff, sizeof(tv));
	len = sizeof(tv);
	CHECK(rt_api_getsockopt(&C, b, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, &len) == 0 &&
	      tv.tv_secs == 0 && tv.tv_micro == 100000 && len == 8);
	tv.tv_micro = 1000000;
	CHECK(rt_api_setsockopt(&C, b, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv)) == -RT_EDOM);
	tv.tv_micro = 0;
	CHECK(rt_api_setsockopt(&C, b, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

	/* blocking recv woken by data from another thread */
	memset(&t, 0, sizeof(t));
	t.fd = b;
	t.len = 100;
	pthread_create(&th, NULL, recv_thread, &t);
	usleep(30000);
	CHECK(rt_api_send(&C, a, "wake", 4, 0) == 4);
	pthread_join(th, NULL);
	CHECK(t.r == 4 && !memcmp(t.buf, "wake", 4));

	/* non-blocking: MSG_DONTWAIT and FIONBIO */
	CHECK(rt_api_recv(&C, b, buf, 10, RT_MSG_DONTWAIT) == -RT_EWOULDBLOCK);
	CHECK(rt_api_ioctl(&C, b, RT_FIONBIO, &on) == 0);
	CHECK(rt_api_recv(&C, b, buf, 10, 0) == -RT_EWOULDBLOCK);
	CHECK(rt_api_ioctl(&C, b, RT_FIONBIO, &off) == 0);

	/* partial sends: non-blocking returns what fit */
	memset(big, 'z', sizeof(big));
	CHECK(rt_api_send(&C, a, big, sizeof(big), RT_MSG_DONTWAIT) == FK_CAP);
	CHECK(rt_api_send(&C, a, big, sizeof(big), RT_MSG_DONTWAIT) == -RT_EWOULDBLOCK);
	{
		s32 n = 0;
		CHECK(rt_api_ioctl(&C, b, RT_FIONREAD, &n) == 0 && n == FK_CAP);
	}
	/* blocking send of 1000 bytes completes while a reader drains */
	memset(&t, 0, sizeof(t));
	t.fd = b;
	t.len = FK_CAP + sizeof(big);
	t.delay = 20;
	pthread_create(&th, NULL, drain_thread, &t);
	CHECK(rt_api_send(&C, a, big, sizeof(big), 0) == (int)sizeof(big));
	pthread_join(th, NULL);
	CHECK(t.r == FK_CAP + (int)sizeof(big));

	/* SO_SNDTIMEO with partial send: returns the bytes sent */
	tv.tv_secs = 0;
	tv.tv_micro = 50000;
	CHECK(rt_api_setsockopt(&C, a, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv)) == 0);
	CHECK(rt_api_send(&C, a, big, sizeof(big), 0) == FK_CAP);
	CHECK(rt_api_send(&C, a, big, sizeof(big), 0) == -RT_EWOULDBLOCK);
	CHECK(rt_api_recv(&C, b, buf, sizeof(buf), 0) == FK_CAP);

	/* MSG_WAITALL collects 100 bytes from ten 10 byte writes */
	memset(&t, 0, sizeof(t));
	t.fd = a;
	pthread_create(&th, NULL, dribble_thread, &t);
	CHECK(rt_api_recv(&C, b, buf, 100, RT_MSG_WAITALL) == 100);
	pthread_join(th, NULL);
	CHECK(!memcmp(buf + 90, "0123456789", 10));

	/* PEEK, recvmsg with iov */
	CHECK(rt_api_send(&C, a, "abcdef", 6, 0) == 6);
	CHECK(rt_api_recv(&C, b, buf, 3, RT_MSG_PEEK) == 3 && !memcmp(buf, "abc", 3));
	{
		char x[2], y[10];
		struct rt_iov iov[2] = { { x, 2 }, { y, 10 } };
		int mf = -1;
		struct rt_sockaddr_in from;
		u32 fl = sizeof(from);
		CHECK(rt_api_recvmsg(&C, b, iov, 2, 0, &from, &fl, &mf) == 6 &&
		      !memcmp(x, "ab", 2) && !memcmp(y, "cdef", 4) && mf == 0 && fl == 0);
		CHECK(rt_api_recvmsg(&C, b, iov, -1, 0, NULL, NULL, NULL) == -RT_EINVAL);
		CHECK(rt_api_sendmsg(&C, a, iov, 2000, 0, NULL, 0) == -RT_EINVAL);
	}

	/* shutdown: write side -> EPIPE locally, EOF at the peer */
	CHECK(rt_api_shutdown(&C, a, 1) == 0);
	CHECK(rt_api_send(&C, a, "x", 1, 0) == -RT_EPIPE);
	CHECK(rt_api_recv(&C, b, buf, 10, 0) == 0);
	CHECK(rt_api_shutdown(&C, a, 3) == -RT_EINVAL);
	CHECK(rt_api_shutdown(&C, l, 2) == -RT_ENOTCONN);
	rt_api_close(&C, a);
	rt_api_close(&C, b);
	rt_api_close(&C, l);
}

static void test_connect_accept(void)
{
	struct rt_sockaddr_in s = sa(12), peer;
	int l, c, c2, d, on = 1, r;
	u32 len;

	l = mksock(&C);
	CHECK(rt_api_bind(&C, l, &s, sizeof(s)) == 0);
	CHECK(rt_api_bind(&C, l, &s, 4) == -RT_EINVAL);
	CHECK(rt_api_accept(&C, l, NULL, NULL) == -RT_EINVAL);    /* not listening */
	CHECK(rt_api_listen(&C, l, 1) == 0);
	CHECK(rt_api_ioctl(&C, l, RT_FIONBIO, &on) == 0);
	CHECK(rt_api_accept(&C, l, NULL, NULL) == -RT_EWOULDBLOCK);
	{
		s32 v;
		len = 4;
		CHECK(rt_api_getsockopt(&C, l, RT_SOL_SOCKET, RT_SO_ACCEPTCONN, &v, &len) == 0 && v != 0);
	}

	/* non-blocking connect: EINPROGRESS, EALREADY, completion, EISCONN */
	fk_instant = 0;
	c = mksock(&C);
	CHECK(rt_api_ioctl(&C, c, RT_FIONBIO, &on) == 0);
	CHECK(rt_api_connect(&C, c, &s, sizeof(s)) == -RT_EINPROGRESS);
	CHECK(rt_api_connect(&C, c, &s, sizeof(s)) == -RT_EALREADY);
	{
		u32 wr[2] = { 1u << c, 0 };
		CHECK(rt_api_select(&C, c + 1, NULL, wr, NULL, 0, NULL) == 0);
		rt_lock();
		fk_complete(C.fds[c]);
		rt_unlock();
		wr[0] = 1u << c;
		CHECK(rt_api_select(&C, c + 1, NULL, wr, NULL, 0, NULL) == 1 && wr[0] == 1u << c);
	}
	CHECK(rt_api_connect(&C, c, &s, sizeof(s)) == -RT_EISCONN);
	len = sizeof(peer);
	d = rt_api_accept(&C, l, &peer, &len);
	CHECK(d >= 0 && len == sizeof(peer) && peer.sin_family == RT_AF_INET && peer.sin_len == 16 &&
	      peer.sin_addr == rt_htonl(0x7f000001));
	CHECK(rt_api_getpeername(&C, c, &peer, &len) == 0 && peer.sin_port == rt_htons(12));
	{   /* child inherited NBIO from the listener (4.4BSD) */
		char b;
		CHECK(rt_api_recv(&C, d, &b, 1, 0) == -RT_EWOULDBLOCK);
	}
	rt_api_close(&C, d);

	/* failed non-blocking connect is reported via SO_ERROR */
	c2 = mksock(&C);
	CHECK(rt_api_ioctl(&C, c2, RT_FIONBIO, &on) == 0);
	CHECK(rt_api_connect(&C, c2, &s, sizeof(s)) == -RT_EINPROGRESS);
	rt_lock();
	fk_fail(C.fds[c2], RT_ECONNREFUSED);
	rt_unlock();
	{
		u32 wr[2] = { 1u << c2, 0 };
		s32 err = 0;
		CHECK(rt_api_select(&C, c2 + 1, NULL, wr, NULL, 0, NULL) == 1);
		len = 4;
		CHECK(rt_api_getsockopt(&C, c2, RT_SOL_SOCKET, RT_SO_ERROR, &err, &len) == 0 && err == RT_ECONNREFUSED);
		len = 4;
		CHECK(rt_api_getsockopt(&C, c2, RT_SOL_SOCKET, RT_SO_ERROR, &err, &len) == 0 && err == 0);
	}
	rt_api_close(&C, c2);

	rt_api_close(&C, c);

	/* backlog (Linux: backlog + 1 queued) */
	fk_instant = 1;
	{
		int cs[4], i;
		for (i = 0; i < 4; i++) {
			cs[i] = mksock(&C);
			r = rt_api_connect(&C, cs[i], &s, sizeof(s));
			CHECK(i < 2 ? r == 0 : r == -RT_ECONNREFUSED);
		}
		for (i = 0; i < 4; i++)
			rt_api_close(&C, cs[i]);
	}
	/* closing the listener aborts queued connections */
	r = fk_closes;
	CHECK(rt_api_close(&C, l) == 0);
	CHECK(fk_closes == r + 3);
	CHECK(rt_api_listen(&C, 63, 1) == -RT_EBADF);
}

static void *completer(void *v)
{
	int fd = *(int *)v;
	usleep(30000);
	rt_lock();
	fk_complete(C.fds[fd]);
	rt_unlock();
	return NULL;
}

static void test_blocking_connect(void)
{
	struct rt_sockaddr_in s = sa(13);
	int l, c, d;
	pthread_t th;
	u32 t0;

	l = mksock(&C);
	rt_api_bind(&C, l, &s, sizeof(s));
	rt_api_listen(&C, l, 5);
	fk_instant = 0;
	c = mksock(&C);
	pthread_create(&th, NULL, completer, &c);
	t0 = now_ms();
	CHECK(rt_api_connect(&C, c, &s, sizeof(s)) == 0);
	CHECK(now_ms() - t0 >= 25);
	pthread_join(th, NULL);
	d = rt_api_accept(&C, l, NULL, NULL);       /* blocking, already queued */
	CHECK(d >= 0);
	/* connect timeout through SO_SNDTIMEO */
	{
		struct rt_timeval tv = { 0, 40000 };
		int c2 = mksock(&C);
		rt_api_setsockopt(&C, c2, RT_SOL_SOCKET, RT_SO_SNDTIMEO, &tv, sizeof(tv));
		CHECK(rt_api_connect(&C, c2, &s, sizeof(s)) == -RT_ETIMEDOUT);
		CHECK(rt_api_connect(&C, c2, &s, sizeof(s)) == -RT_EALREADY);
		rt_api_close(&C, c2);
	}
	/* blocking accept woken by a connection */
	{
		struct rt_timeval tv = { 0, 50000 };
		CHECK(rt_api_setsockopt(&C, l, RT_SOL_SOCKET, RT_SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
		CHECK(rt_api_accept(&C, l, NULL, NULL) == -RT_EWOULDBLOCK);
	}
	fk_instant = 1;
	rt_api_close(&C, c);
	rt_api_close(&C, d);
	rt_api_close(&C, l);
}

static void *sender_later(void *v)
{
	int fd = *(int *)v;
	usleep(40000);
	rt_api_send(&C, fd, "s", 1, 0);
	return NULL;
}

static void test_select(void)
{
	int l1, a1, b1, l2, a2, b2, r;
	u32 rd[2], wr[2], ex[2], t0, sig;
	pthread_t th;

	CHECK(mkpair(20, &l1, &a1, &b1) == 0);
	CHECK(mkpair(21, &l2, &a2, &b2) == 0);
	/* nothing readable: timeout clears the sets */
	rd[0] = (1u << b1) | (1u << b2);
	t0 = now_ms();
	r = rt_api_select(&C, 64, rd, NULL, NULL, 50, NULL);
	CHECK(r == 0 && rd[0] == 0 && now_ms() - t0 >= 45);
	/* writable: both connected */
	wr[0] = (1u << a1) | (1u << a2);
	CHECK(rt_api_select(&C, 64, NULL, wr, NULL, 0, NULL) == 2 && wr[0] == ((1u << a1) | (1u << a2)));
	/* data on one */
	rt_api_send(&C, a2, "q", 1, 0);
	rd[0] = (1u << b1) | (1u << b2);
	wr[0] = 1u << a1;
	ex[0] = (1u << b1);
	CHECK(rt_api_select(&C, 64, rd, wr, ex, 100, NULL) == 2 && rd[0] == (1u << b2) &&
	      wr[0] == (1u << a1) && ex[0] == 0);
	/* listening socket readable when a connection is pending */
	{
		int c = mksock(&C);
		struct rt_sockaddr_in s = sa(20);
		rd[0] = 1u << l1;
		CHECK(rt_api_select(&C, 64, rd, NULL, NULL, 0, NULL) == 0);
		rt_api_connect(&C, c, &s, sizeof(s));
		rd[0] = 1u << l1;
		CHECK(rt_api_select(&C, 64, rd, NULL, NULL, 0, NULL) == 1);
		rt_api_close(&C, c);
	}
	/* sleeping select woken by another thread */
	{
		char x;
		rt_api_recv(&C, b2, &x, 1, 0);
	}
	pthread_create(&th, NULL, sender_later, &a1);
	rd[0] = (1u << b1) | (1u << b2);
	t0 = now_ms();
	r = rt_api_select(&C, 64, rd, NULL, NULL, -1, NULL);
	CHECK(r == 1 && rd[0] == (1u << b1) && now_ms() - t0 >= 30);
	pthread_join(th, NULL);
	/* EBADF for a closed descriptor; nfds clamps; nfds < 0 */
	rd[0] = 0;
	rd[1] = 1u << (40 - 32);
	CHECK(rt_api_select(&C, 64, rd, NULL, NULL, 0, NULL) == -RT_EBADF);
	rd[0] = 0;
	rd[1] = 1u << 30;                         /* fd 62: beyond nfds=40 */
	CHECK(rt_api_select(&C, 40, rd, NULL, NULL, 0, NULL) == 0);
	CHECK(rt_api_select(&C, -1, rd, NULL, NULL, 0, NULL) == -RT_EINVAL);
	/* pure delay with no descriptors, signal mask passes through */
	t0 = now_ms();
	sig = 0x1000;
	CHECK(rt_api_select(&C, 0, NULL, NULL, NULL, 30, &sig) == 0 && sig == 0 && now_ms() - t0 >= 25);
	/* many descriptors (heap entry array) */
	{
		int fds[10], i;
		u32 set[2] = { 0, 0 };
		for (i = 0; i < 10; i++) {
			fds[i] = mksock(&C);
			set[fds[i] >> 5] |= 1u << (fds[i] & 31);
		}
		CHECK(rt_api_select(&C, 64, set, NULL, NULL, 20, NULL) == 0);
		for (i = 0; i < 10; i++)
			rt_api_close(&C, fds[i]);
	}
	rt_api_close(&C, a1); rt_api_close(&C, b1); rt_api_close(&C, l1);
	rt_api_close(&C, a2); rt_api_close(&C, b2); rt_api_close(&C, l2);
}

static void test_options(void)
{
	int fd = mksock(&C), on = 1;
	s32 v;
	u32 len;
	struct rt_linger lg = { 1, 7 };
	int calls = fk_sockopt_calls;

	len = 4;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_TYPE, &v, &len) == 0 && v == RT_SOCK_STREAM);
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &on, 4) == 0);
	len = 4;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &v, &len) == 0 && v == RT_SO_REUSEADDR);
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, &on, 4) == 0);
	CHECK(fk_sockopt_calls == calls + 2);              /* protocol told */
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_KEEPALIVE, &on, 2) == -RT_EINVAL);
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof(lg)) == 0);
	memset(&lg, 0, sizeof(lg));
	len = sizeof(lg);
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, &len) == 0 && lg.l_onoff && lg.l_linger == 7);
	v = 100000000;
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_RCVBUF, &v, 4) == 0);
	len = 4;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_RCVBUF, &v, &len) == 0 && v == sock_maxsockbuf);
	v = 0;
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_SNDBUF, &v, 4) == -RT_EINVAL);
	v = 10;
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_SNDBUF, &v, 4) == 0);
	len = 4;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_SNDBUF, &v, &len) == 0 && v == 512);
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_TYPE, &v, 4) == -RT_ENOPROTOOPT);
	CHECK(rt_api_setsockopt(&C, fd, RT_SOL_SOCKET, 0x7777, &v, 4) == -RT_ENOPROTOOPT);
	len = 4;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, 0x7777, &v, &len) == -RT_ENOPROTOOPT);
	len = 2;
	CHECK(rt_api_getsockopt(&C, fd, RT_SOL_SOCKET, RT_SO_TYPE, &v, &len) == -RT_EINVAL);
	CHECK(rt_api_setsockopt(&C, fd, 6, 1, &on, 4) == 0);         /* other level -> proto */
	CHECK(rt_api_setsockopt(&C, fd, 6, 2, &on, 4) == -RT_ENOPROTOOPT);
	v = 0x1234;
	CHECK(rt_api_ioctl(&C, fd, RT_FIOSETOWN, &v) == 0);
	v = 0;
	CHECK(rt_api_ioctl(&C, fd, RT_SIOCGPGRP, &v) == 0 && v == 0x1234);
	CHECK(rt_api_ioctl(&C, fd, RT_SIOCATMARK, &v) == 0 && v == 0);
	CHECK(rt_api_ioctl(&C, fd, 0x12345678u, &v) == -RT_EINVAL);
	CHECK(rt_api_ioctl(&C, fd, RT_FIONBIO, NULL) == -RT_EFAULT);
	CHECK(rt_api_getsockname(&C, fd, NULL, &len) == -RT_EFAULT);
	{
		struct rt_sockaddr_in a;
		len = sizeof(a);
		CHECK(rt_api_getpeername(&C, fd, &a, &len) == -RT_ENOTCONN);
	}
	rt_api_close(&C, fd);
}

static int hook_calls;
static u32 hook_events;
static void my_hook(struct rt_socket *so, u32 ev) { hook_calls++; hook_events |= ev; }

static void test_events_release(void)
{
	int l, a, b, id, fd2, fd3;
	s32 mask = RT_SEV_READ | RT_SEV_CLOSE;
	u32 ev;
	char buf[4];

	CHECK(mkpair(30, &l, &a, &b) == 0);
	rt_sock_async_hook = my_hook;
	CHECK(rt_api_get_events(&C, &ev) == -1);
	CHECK(rt_api_setsockopt(&C, b, RT_SOL_SOCKET, RT_SO_EVENTMASK, &mask, 4) == 0);
	CHECK(rt_api_setsockopt(&C, a, RT_SOL_SOCKET, RT_SO_EVENTMASK, &mask, 4) == 0);
	rt_api_send(&C, a, "e", 1, 0);
	CHECK(hook_calls == 1 && (hook_events & RT_SEV_READ));
	rt_api_send(&C, b, "f", 1, 0);
	ev = 0;
	CHECK(rt_api_get_events(&C, &ev) == (a < b ? a : b) && ev == RT_SEV_READ);
	CHECK(rt_api_get_events(&C, &ev) == (a < b ? b : a) && ev == RT_SEV_READ);
	CHECK(rt_api_get_events(&C, &ev) == -1);
	/* FIOASYNC delivers every event to the hook, none pending */
	{
		int on = 1;
		s32 zero = 0;
		rt_api_setsockopt(&C, b, RT_SOL_SOCKET, RT_SO_EVENTMASK, &zero, 4);
		rt_api_ioctl(&C, b, RT_FIOASYNC, &on);
		hook_calls = 0;
		rt_api_send(&C, a, "g", 1, 0);
		CHECK(hook_calls == 1 && rt_api_get_events(&C, &ev) == -1);
	}
	rt_sock_async_hook = NULL;
	rt_api_recv(&C, a, buf, 4, 0);
	rt_api_recv(&C, b, buf, 4, 0);

	/* ReleaseSocket(UNIQUE_ID) -> ObtainSocket from another context */
	id = rt_api_release(&C, b, -1, 0);
	CHECK(id >= 0x10000);
	CHECK(rt_api_send(&C, b, "x", 1, 0) == -RT_EBADF);       /* fd gone */
	CHECK(rt_api_obtain(&C2, id, RT_AF_INET, RT_SOCK_DGRAM, 0) == -RT_EINVAL);
	CHECK(rt_api_obtain(&C2, id, 7, RT_SOCK_STREAM, 0) == -RT_EAFNOSUPPORT);
	fd2 = rt_api_obtain(&C2, id, RT_AF_INET, RT_SOCK_STREAM, 0);
	CHECK(fd2 == 0);
	CHECK(rt_api_obtain(&C2, id, RT_AF_INET, RT_SOCK_STREAM, 0) == -RT_EINVAL);   /* taken */
	CHECK(rt_api_send(&C, a, "y", 1, 0) == 1);
	CHECK(rt_api_recv(&C2, fd2, buf, 4, 0) == 1 && buf[0] == 'y');
	/* ReleaseCopyOfSocket keeps the descriptor; user chosen id */
	CHECK(rt_api_release(&C2, fd2, 77, 1) == 77);
	CHECK(rt_api_release(&C2, fd2, 78, 1) == -RT_EBUSY);
	CHECK(rt_api_release(&C, a, 77, 0) == -RT_EINVAL);        /* id in use */
	CHECK(rt_api_send(&C2, fd2, "z", 1, 0) == 1);
	fd3 = rt_api_obtain(&C, 77, RT_AF_INET, RT_SOCK_STREAM, RT_IPPROTO_TCP == 6 ? 0 : 0);
	CHECK(fd3 >= 0);
	CHECK(rt_api_close(&C2, fd2) == 0);
	CHECK(rt_api_recv(&C, a, buf, 4, 0) == 1 && buf[0] == 'z');
	CHECK(rt_api_send(&C, fd3, "w", 1, 0) == 1);
	rt_api_close(&C, fd3);
	rt_api_close(&C, a);
	rt_api_close(&C, l);
}

static void *close_later(void *v)
{
	int fd = *(int *)v;
	usleep(30000);
	rt_api_close(&C, fd);
	return NULL;
}

static void test_close_while_blocked(void)
{
	int l, a, b;
	pthread_t th;
	char buf[4];
	u32 rd[2];

	CHECK(mkpair(40, &l, &a, &b) == 0);
	pthread_create(&th, NULL, close_later, &b);
	CHECK(rt_api_recv(&C, b, buf, 4, 0) == -RT_EBADF);
	pthread_join(th, NULL);
	/* select sleeping on a socket that gets closed */
	pthread_create(&th, NULL, close_later, &a);
	rd[0] = 1u << a;
	rd[1] = 0;
	CHECK(rt_api_select(&C, 64, rd, NULL, NULL, 2000, NULL) == 1);
	pthread_join(th, NULL);
	rt_api_close(&C, l);
}

static void test_internals(void)
{
	u8 a[4] = { 1, 2, 3, 4 }, b[3] = { 5, 6, 7 }, out[7];
	struct rt_iov iov[2] = { { a, 4 }, { b, 3 } };
	struct rt_socket *so, *ch;
	int i;

	CHECK(rt_iov_total(iov, 2) == 7);
	CHECK(rt_iov_to_buf(iov, 2, 2, out, 4) == 4 && out[0] == 3 && out[3] == 6);
	CHECK(rt_iov_to_buf(iov, 2, 6, out, 4) == 1 && out[0] == 7);
	CHECK(rt_buf_to_iov(iov, 2, 3, "xyz", 3) == 3 && a[3] == 'x' && b[1] == 'z');
	CHECK(rt_buf_to_iov(iov, 2, 9, "xyz", 3) == 0);

	rt_lock();
	/* datagram queue accounting */
	so = sock_alloc(RT_SOCK_DGRAM, 17, &fk_ops);
	so->rcvbuf = 1000;
	for (i = 0; i < 10; i++) {
		struct rt_buf *bb = rt_buf_alloc(0, 300);
		rt_buf_put(bb, 300);
		if (rt_sock_queue_dgram(so, bb) < 0) {
			rt_buf_free(bb);
			break;
		}
	}
	CHECK(i == 1);     /* 2nd: 300 + 2*256 + 300 > 1000 */
	so->rcvbuf = 100;
	CHECK(so->rcvq.qlen == 1 && so->rcvq.bytes == 300);
	sock_free(so);
	/* child helpers: drop before accept */
	so = sock_alloc(RT_SOCK_STREAM, 6, &fk_ops);
	so->state |= RT_SS_ISLISTENING;
	so->backlog = 0;
	ch = rt_sock_new_child(so);
	CHECK(ch && ch->listen_parent == so && (ch->state & RT_SS_NOFDREF));
	rt_sock_child_ready(so, ch);
	CHECK(so->acceptq_len == 1 && rt_sock_new_child(so) == NULL);
	rt_sock_child_drop(ch);
	CHECK(so->acceptq_len == 0 && so->acceptq_head == NULL && so->acceptq_tail == NULL);
	ch = rt_sock_new_child(so);
	rt_sock_child_drop(ch);                    /* never queued */
	rt_sock_set_error(so, -RT_ECONNRESET);
	CHECK(so->so_error == RT_ECONNRESET);
	sock_free(so);
	rt_unlock();
}

static void test_destroy(void)
{
	int l, a, b, n;
	struct rt_sockctx X;

	CHECK(rt_ctx_init(&X, 0, NULL) == 0 && X.nfds == 64 && X.sigintr_mask == (1u << 12));
	n = fk_closes;
	CHECK(rt_api_socket(&X, RT_AF_INET, RT_SOCK_STREAM, 0) == 0);
	CHECK(rt_api_socket(&X, RT_AF_INET, RT_SOCK_STREAM, 0) == 1);
	CHECK(rt_api_dup2(&X, 1, 10) == 10);
	rt_ctx_destroy(&X);
	CHECK(fk_closes == n + 2);
	(void)l; (void)a; (void)b;
}

static int cb_free, cb_alloc;
static int fdcb(struct rt_sockctx *c, int fd, int action)
{
	if (action == RT_FDCB_CHECK)
		return fd == 0;                         /* fd 0 taken by the C library */
	if (action == RT_FDCB_ALLOC)
		cb_alloc++;
	else
		cb_free++;
	return 0;
}

static void test_fdcallback(void)
{
	int fd;

	C.fd_callback = fdcb;
	fd = mksock(&C);
	CHECK(fd == 1 && cb_alloc == 1);
	rt_api_close(&C, fd);
	CHECK(cb_free == 1);
	C.fd_callback = NULL;
}

int main(void)
{
	rt_sock_proto_hook = fk_hook;
	rt_lock();
	rt_sysctl_init();
	rt_socket_init();
	rt_unlock();
	CHECK(rt_ctx_init(&C, 64, NULL) == 0);
	CHECK(rt_ctx_init(&C2, 16, NULL) == 0);
	test_fdtable();
	test_dup();
	test_blocking();
	test_connect_accept();
	test_blocking_connect();
	test_select();
	test_options();
	test_events_release();
	test_close_while_blocked();
	test_internals();
	test_destroy();
	test_fdcallback();
	rt_ctx_destroy(&C);
	rt_ctx_destroy(&C2);
	{
		int n = 0;
		struct rt_socket *s;
		rt_lock();
		for (s = rt_all_sockets; s; s = s->next_all)
			n++;
		rt_unlock();
		CHECK(n == 0);                            /* nothing leaked */
	}
	printf("t_sock_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
