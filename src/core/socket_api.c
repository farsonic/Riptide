/*
 * Riptide - descriptor based socket API (bsdsocket.library semantics:
 * AmiTCP 4 / Miami / 4.4BSD).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Every rt_api_* function takes the stack lock itself. Blocking calls run
 * the non-blocking protocol op, and if it says -RT_EWOULDBLOCK they put a
 * wait entry (on the caller's stack) on so->wq, re-check and sleep. Wakeups
 * are sticky (rt_os.h), so adding the entry first and then re-running the
 * op cannot lose an event. SO_RCVTIMEO/SO_SNDTIMEO expiry reports
 * -RT_EWOULDBLOCK as in 4.4BSD; the break signal reports -RT_EINTR (or the
 * byte count if some data had already been transferred).
 *
 * Frames are kept small: these functions run on Amiga client stacks.
 */
#include "sock_internal.h"

#define SOCK_DEFAULT_DTABLE 64
#define SOCK_MAX_DTABLE     1024
#define SOCK_IOV_MAX        1024     /* BSD UIO_MAXIOV */
#define SOCK_IOV_LOCAL      8
#define SOCK_SEL_LOCAL      4
#define SIGBREAKF_CTRL_C    (1u << 12)

/* Dup2Socket(-1, fd) marks fd as used without a socket behind it. */
static struct rt_socket sock_reserved_obj;
#define SOCK_RESERVED (&sock_reserved_obj)

static struct rt_sockctx *ctx_list;
static s32 release_next_id = 0x10000;   /* UNIQUE_ID allocations */

/* ======================================================================
 * Descriptor tables
 * ====================================================================== */

static int fd_get(struct rt_sockctx *c, int fd, struct rt_socket **out)
{
	struct rt_socket *so;

	if (!c || !c->fds || fd < 0 || fd >= c->nfds || (so = c->fds[fd]) == NULL)
		return -RT_EBADF;
	if (so == SOCK_RESERVED)
		return -RT_ENOTSOCK;
	*out = so;
	return 0;
}

/* Lowest free descriptor. The fd callback (SBTC_FDCALLBACK) runs with the
 * stack lock held and must not call back into the socket API. */
static int fd_alloc(struct rt_sockctx *c)
{
	int i;

	for (i = 0; i < c->nfds; i++) {
		if (c->fds[i])
			continue;
		if (c->fd_callback && c->fd_callback(c, i, RT_FDCB_CHECK) != 0)
			continue;
		return i;
	}
	return -RT_EMFILE;
}

static void fd_install(struct rt_sockctx *c, int fd, struct rt_socket *so, int addref)
{
	c->fds[fd] = so;
	if (addref)
		so->refcnt++;
	if (c->fd_callback)
		c->fd_callback(c, fd, RT_FDCB_ALLOC);
}

static void fd_release(struct rt_sockctx *c, int fd)
{
	struct rt_socket *so = c->fds[fd];

	c->fds[fd] = NULL;
	if (!so || so == SOCK_RESERVED)
		return;
	if (c->fd_callback)
		c->fd_callback(c, fd, RT_FDCB_FREE);
	if (so->refcnt)
		so->refcnt--;
	if (so->refcnt == 0)
		sock_close(so);
}

int rt_ctx_init(struct rt_sockctx *ctx, int nfds, void *owner)
{
	if (!ctx)
		return -RT_EINVAL;
	if (nfds <= 0)
		nfds = SOCK_DEFAULT_DTABLE;
	if (nfds > SOCK_MAX_DTABLE)
		nfds = SOCK_MAX_DTABLE;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fds = rt_zalloc((size_t)nfds * sizeof(struct rt_socket *));
	if (!ctx->fds)
		return -RT_ENOMEM;
	ctx->nfds = nfds;
	ctx->sigintr_mask = SIGBREAKF_CTRL_C;
	ctx->owner = owner;
	rt_lock();
	ctx->next = ctx_list;
	ctx_list = ctx;
	rt_unlock();
	return 0;
}

void rt_ctx_destroy(struct rt_sockctx *ctx)
{
	struct rt_sockctx **pp;
	int i;

	if (!ctx)
		return;
	rt_lock();
	ctx->fd_callback = NULL;     /* the program is going away */
	if (ctx->fds) {
		for (i = 0; i < ctx->nfds; i++)
			if (ctx->fds[i])
				fd_release(ctx, i);
		rt_free(ctx->fds);
		ctx->fds = NULL;
	}
	ctx->nfds = 0;
	for (pp = &ctx_list; *pp; pp = &(*pp)->next) {
		if (*pp == ctx) {
			*pp = ctx->next;
			break;
		}
	}
	rt_unlock();
}

int rt_ctx_set_dtablesize(struct rt_sockctx *ctx, int nfds)
{
	struct rt_socket **nf;
	int i, r = 0;

	if (!ctx || nfds < 1 || nfds > SOCK_MAX_DTABLE)
		return -RT_EINVAL;
	nf = rt_zalloc((size_t)nfds * sizeof(*nf));
	if (!nf)
		return -RT_ENOMEM;
	rt_lock();
	for (i = nfds; i < ctx->nfds; i++) {
		if (ctx->fds[i]) {
			r = -RT_EINVAL;     /* would drop a descriptor in use */
			break;
		}
	}
	if (r == 0) {
		for (i = 0; i < ctx->nfds && i < nfds; i++)
			nf[i] = ctx->fds[i];
		rt_free(ctx->fds);
		ctx->fds = nf;
		ctx->nfds = nfds;
		nf = NULL;
	}
	rt_unlock();
	rt_free(nf);
	return r;
}

/* ======================================================================
 * Sleeping helpers
 * ====================================================================== */

/* Milliseconds left of 'timeo' since 'start'; -1 = no timeout. */
static s32 time_left(u32 start, s32 timeo)
{
	u32 el;

	if (timeo < 0)
		return -1;
	el = rt_now_ms() - start;
	return el >= (u32)timeo ? 0 : (s32)((u32)timeo - el);
}

/* Sleep on so->wq (entry already added). Returns 0 to retry,
 * -RT_EWOULDBLOCK on timeout, -RT_EINTR, -RT_EBADF if closed meanwhile. */
static int sock_sleep(struct rt_socket *so, struct rt_wait_entry *we, u32 start, s32 timeo)
{
	s32 left = time_left(start, timeo);
	int r;

	if (left == 0)
		return -RT_EWOULDBLOCK;
	r = rt_sleeper_wait(we->sleeper, left, NULL);
	if (so->state & SOCK_SS_DEAD)
		return -RT_EBADF;
	if (r == -RT_ETIMEDOUT)
		return -RT_EWOULDBLOCK;
	return r < 0 ? r : 0;
}

static void wait_begin(struct rt_socket *so, struct rt_wait_entry *we)
{
	rt_waitq_add(&so->wq, we, rt_sleeper_self());
}

/* Must be the last use of 'so' by the caller: may free it. */
static void wait_end(struct rt_socket *so, struct rt_wait_entry *we)
{
	rt_waitq_remove(&so->wq, we);
	sock_waiter_done(so);
}

static int sock_nbio(const struct rt_socket *so, int flags)
{
	return (so->state & RT_SS_NBIO) || (flags & RT_MSG_DONTWAIT);
}

static int take_error(struct rt_socket *so)
{
	int e = so->so_error;

	so->so_error = 0;
	return -e;
}

/* Advance a private iov copy by n bytes. */
static void iov_advance(struct rt_iov **piov, int *pcnt, u32 n)
{
	struct rt_iov *v = *piov;
	int cnt = *pcnt;

	while (cnt > 0 && n >= v->len) {
		n -= v->len;
		v++;
		cnt--;
	}
	if (cnt > 0 && n) {
		v->base = (u8 *)v->base + n;
		v->len -= n;
	}
	*piov = v;
	*pcnt = cnt;
}

/* Copy of the caller's iov that the stream loops may advance. */
static struct rt_iov *iov_dup(const struct rt_iov *iov, int iovcnt, struct rt_iov *local)
{
	struct rt_iov *v = local;

	if (iovcnt > SOCK_IOV_LOCAL) {
		v = rt_malloc((size_t)iovcnt * sizeof(*v));
		if (!v)
			return NULL;
	}
	memcpy(v, iov, (size_t)iovcnt * sizeof(*v));
	return v;
}

static int check_addr(const struct rt_sockaddr_in *a, u32 alen, struct rt_sockaddr_in *out)
{
	if (!a)
		return -RT_EFAULT;
	if (alen < 8)
		return -RT_EINVAL;
	memset(out, 0, sizeof(*out));
	memcpy(out, a, alen < sizeof(*out) ? alen : sizeof(*out));
	out->sin_len = sizeof(*out);
	return 0;
}

static void copy_addr_out(const struct rt_sockaddr_in *sin, struct rt_sockaddr_in *a, u32 *alen)
{
	u32 n;

	if (!a || !alen)
		return;
	n = *alen < sizeof(*sin) ? *alen : sizeof(*sin);
	memcpy(a, sin, n);
	*alen = sizeof(*sin);
}

/* ======================================================================
 * socket / bind / listen / accept / connect
 * ====================================================================== */

int rt_api_socket(struct rt_sockctx *c, int domain, int type, int protocol)
{
	const struct rt_proto_ops *ops;
	struct rt_socket *so;
	int fd, r;

	if (domain != RT_AF_INET)
		return -RT_EAFNOSUPPORT;
	if (type != RT_SOCK_STREAM && type != RT_SOCK_DGRAM && type != RT_SOCK_RAW)
		return -RT_ESOCKTNOSUPPORT;
	ops = sock_proto_lookup(type, &protocol);
	if (!ops)
		return -RT_EPROTONOSUPPORT;
	rt_lock();
	fd = c && c->fds ? fd_alloc(c) : -RT_EBADF;
	if (fd < 0) {
		rt_unlock();
		return fd;
	}
	so = sock_alloc(type, protocol, ops);
	if (!so) {
		rt_unlock();
		return -RT_ENOBUFS;
	}
	so->owner = c->owner;
	r = ops->attach ? ops->attach(so) : 0;
	if (r < 0) {
		sock_free(so);
		rt_unlock();
		return r;
	}
	fd_install(c, fd, so, 1);
	rt_unlock();
	return fd;
}

int rt_api_bind(struct rt_sockctx *c, int fd, const struct rt_sockaddr_in *a, u32 alen)
{
	struct rt_sockaddr_in sin;
	struct rt_socket *so;
	int r = check_addr(a, alen, &sin);

	if (r < 0)
		return r;
	/* Old Amiga programs often leave sin_family 0 (AF_UNSPEC). */
	if (sin.sin_family != RT_AF_INET && sin.sin_family != RT_AF_UNSPEC)
		return -RT_EAFNOSUPPORT;
	sin.sin_family = RT_AF_INET;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r == 0) {
		r = so->ops->bind ? so->ops->bind(so, &sin) : -RT_EOPNOTSUPP;
		if (r == 0)
			so->state |= RT_SS_ISBOUND;
	}
	rt_unlock();
	return r;
}

int rt_api_listen(struct rt_sockctx *c, int fd, int backlog)
{
	struct rt_socket *so;
	u16 old;
	int r;

	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (so->type != RT_SOCK_STREAM || !so->ops->listen) {
		r = -RT_EOPNOTSUPP;
		goto out;
	}
	if (so->state & (RT_SS_ISCONNECTED | RT_SS_ISCONNECTING)) {
		r = -RT_EINVAL;
		goto out;
	}
	if (backlog < 0)
		backlog = 0;
	if (backlog > sock_somaxconn)
		backlog = sock_somaxconn;
	old = so->backlog;
	so->backlog = (u16)backlog;
	r = so->ops->listen(so, backlog);
	if (r == 0) {
		so->state |= RT_SS_ISLISTENING;
		so->options |= RT_SO_ACCEPTCONN;
	} else {
		so->backlog = old;
	}
out:
	rt_unlock();
	return r;
}

int rt_api_accept(struct rt_sockctx *c, int fd, struct rt_sockaddr_in *a, u32 *alen)
{
	struct rt_wait_entry we;
	struct rt_socket *so, *ch = NULL;
	struct rt_sockaddr_in sin;
	int r, nfd = -1, waiting = 0;
	u32 start = rt_now_ms();

	if (a && !alen)
		return -RT_EFAULT;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out_unlock;
	if (!(so->state & RT_SS_ISLISTENING)) {
		r = -RT_EINVAL;
		goto out_unlock;
	}
	for (;;) {
		if (so->so_error) {
			r = take_error(so);
			break;
		}
		if (so->acceptq_head) {
			nfd = fd_alloc(c);
			if (nfd < 0) {
				r = nfd;
				break;
			}
			ch = so->acceptq_head;
			so->acceptq_head = ch->acceptq_next;
			if (!so->acceptq_head)
				so->acceptq_tail = NULL;
			so->acceptq_len--;
			ch->acceptq_next = NULL;
			ch->listen_parent = NULL;
			ch->state &= ~RT_SS_NOFDREF;
			r = 0;
			break;
		}
		if (so->state & SOCK_SS_DEAD) {
			r = -RT_EBADF;
			break;
		}
		if (sock_nbio(so, 0)) {
			r = -RT_EWOULDBLOCK;
			break;
		}
		if (!waiting) {
			wait_begin(so, &we);
			waiting = 1;
			continue;
		}
		r = sock_sleep(so, &we, start, so->rcvtimeo_ms);
		if (r < 0)
			break;
	}
	if (ch) {
		fd_install(c, nfd, ch, 1);
		if (a) {
			memset(&sin, 0, sizeof(sin));
			if (ch->ops->getname)
				ch->ops->getname(ch, &sin, 1);
			sin.sin_len = sizeof(sin);
			sin.sin_family = RT_AF_INET;
			copy_addr_out(&sin, a, alen);
		}
		r = nfd;
	}
	if (waiting)
		wait_end(so, &we);
out_unlock:
	rt_unlock();
	return r;
}

int rt_api_connect(struct rt_sockctx *c, int fd, const struct rt_sockaddr_in *a, u32 alen)
{
	struct rt_wait_entry we;
	struct rt_sockaddr_in sin;
	struct rt_socket *so;
	int r = check_addr(a, alen, &sin), waiting = 0;
	u32 start = rt_now_ms();

	if (r < 0)
		return r;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (sin.sin_family != RT_AF_INET &&
	    !(sin.sin_family == RT_AF_UNSPEC && so->type != RT_SOCK_STREAM)) {
		r = -RT_EAFNOSUPPORT;
		goto out;
	}
	if (so->state & RT_SS_ISLISTENING) {
		r = -RT_EOPNOTSUPP;
		goto out;
	}
	if (so->type == RT_SOCK_STREAM) {
		if (so->state & RT_SS_ISCONNECTED) {
			r = -RT_EISCONN;
			goto out;
		}
		if (so->state & RT_SS_ISCONNECTING) {
			r = -RT_EALREADY;
			goto out;
		}
		/* A failed non-blocking connect is reported once (Linux). */
		if (so->so_error) {
			r = take_error(so);
			goto out;
		}
	}
	if (!so->ops->connect) {
		r = -RT_EOPNOTSUPP;
		goto out;
	}
	r = so->ops->connect(so, &sin);
	if (r == 0) {
		if (sin.sin_family == RT_AF_UNSPEC)
			so->state &= ~RT_SS_ISCONNECTED;     /* datagram disconnect */
		else
			so->state |= RT_SS_ISCONNECTED;
		so->state &= ~RT_SS_ISCONNECTING;
		goto out;
	}
	if (r != -RT_EINPROGRESS) {
		so->state &= ~RT_SS_ISCONNECTING;
		goto out;
	}
	/* The protocol sets RT_SS_ISCONNECTED (and clears ISCONNECTING) when
	 * the handshake completes, or reports failure via rt_sock_set_error. */
	if (!(so->state & RT_SS_ISCONNECTED))
		so->state |= RT_SS_ISCONNECTING;
	if (sock_nbio(so, 0) && !(so->state & RT_SS_ISCONNECTED))
		goto out;
	for (;;) {
		if (so->state & RT_SS_ISCONNECTED) {
			so->state &= ~RT_SS_ISCONNECTING;
			r = 0;
			break;
		}
		if (so->so_error) {
			so->state &= ~RT_SS_ISCONNECTING;
			r = take_error(so);
			break;
		}
		if (!(so->state & RT_SS_ISCONNECTING)) {
			r = -RT_ECONNREFUSED;
			break;
		}
		if (!waiting) {
			wait_begin(so, &we);
			waiting = 1;
			continue;
		}
		r = sock_sleep(so, &we, start, so->sndtimeo_ms);
		if (r == -RT_EWOULDBLOCK) {
			/* SO_SNDTIMEO expired; the attempt goes on in the
			 * background (connect() again gives EALREADY). */
			r = -RT_ETIMEDOUT;
			break;
		}
		if (r < 0)
			break;   /* EINTR: connection continues (BSD) */
	}
	if (waiting)
		wait_end(so, &we);
out:
	rt_unlock();
	return r;
}

/* ======================================================================
 * send / recv
 * ====================================================================== */

static int sock_sendit(struct rt_socket *so, const struct rt_iov *iov0, int iovcnt,
                       int flags, const struct rt_sockaddr_in *to)
{
	struct rt_iov local[SOCK_IOV_LOCAL];
	struct rt_wait_entry we;
	struct rt_iov *iov = (struct rt_iov *)iov0, *alloc = NULL;
	int stream = so->type == RT_SOCK_STREAM, waiting = 0, r, cnt = iovcnt;
	u32 total = rt_iov_total(iov0, iovcnt), sent = 0, start = rt_now_ms();

	if (so->state & RT_SS_CANTSENDMORE)
		return -RT_EPIPE;      /* no SIGPIPE on AmigaOS */
	if (so->so_error)
		return take_error(so);
	if (!so->ops->send)
		return -RT_EOPNOTSUPP;
	if (stream) {
		if (total == 0 && (so->state & RT_SS_ISCONNECTED))
			return 0;
		iov = iov_dup(iov0, iovcnt, local);
		if (!iov)
			return -RT_ENOBUFS;
		if (iov != local)
			alloc = iov;
		to = NULL;             /* ignored on connected streams */
	}
	for (;;) {
		r = so->ops->send(so, iov, cnt, flags, to);
		if (r > 0) {
			sent += (u32)r;
			if (!stream || sent >= total)
				break;
			iov_advance(&iov, &cnt, (u32)r);
			continue;
		}
		if (r == 0 && !stream)
			break;             /* empty datagram */
		if (r == 0)
			r = -RT_EWOULDBLOCK;
		if (r != -RT_EWOULDBLOCK || sock_nbio(so, flags))
			break;
		if (so->so_error) {
			r = take_error(so);
			break;
		}
		if (!waiting) {
			wait_begin(so, &we);
			waiting = 1;
			continue;
		}
		r = sock_sleep(so, &we, start, so->sndtimeo_ms);
		if (r < 0)
			break;
	}
	rt_free(alloc);
	if (sent > 0)
		r = (int)sent;
	if (waiting)
		wait_end(so, &we);
	return r;
}

static int sock_recvit(struct rt_socket *so, const struct rt_iov *iov0, int iovcnt,
                       int flags, struct rt_sockaddr_in *from, int *msgflags)
{
	struct rt_iov local[SOCK_IOV_LOCAL];
	struct rt_wait_entry we;
	struct rt_iov *iov = (struct rt_iov *)iov0, *alloc = NULL;
	int stream = so->type == RT_SOCK_STREAM, waiting = 0, r, cnt = iovcnt, mf;
	int waitall = stream && (flags & RT_MSG_WAITALL) && !(flags & (RT_MSG_PEEK | RT_MSG_OOB));
	u32 total = rt_iov_total(iov0, iovcnt), got = 0, start = rt_now_ms();

	if (!so->ops->recv)
		return -RT_EOPNOTSUPP;
	if (stream && total == 0)
		return 0;
	if (waitall) {
		iov = iov_dup(iov0, iovcnt, local);
		if (!iov)
			return -RT_ENOBUFS;
		if (iov != local)
			alloc = iov;
	}
	for (;;) {
		mf = 0;
		r = so->ops->recv(so, iov, cnt, flags, stream ? NULL : from, &mf);
		if (msgflags)
			*msgflags |= mf;
		if (r > 0) {
			got += (u32)r;
			if (waitall && got < total) {
				iov_advance(&iov, &cnt, (u32)r);
				continue;
			}
			break;
		}
		if (r != -RT_EWOULDBLOCK)
			break;             /* EOF (0) or error */
		if (so->so_error) {
			if (got == 0)
				r = take_error(so);
			break;
		}
		if (so->state & RT_SS_CANTRCVMORE) {
			r = 0;
			break;
		}
		if (sock_nbio(so, flags))
			break;
		if (!waiting) {
			wait_begin(so, &we);
			waiting = 1;
			continue;
		}
		r = sock_sleep(so, &we, start, so->rcvtimeo_ms);
		if (r < 0)
			break;
	}
	rt_free(alloc);
	if (got > 0)
		r = (int)got;
	if (waiting)
		wait_end(so, &we);
	return r;
}

int rt_api_sendmsg(struct rt_sockctx *c, int fd, const struct rt_iov *iov, int iovcnt,
                   int flags, const struct rt_sockaddr_in *to, u32 tolen)
{
	struct rt_sockaddr_in sin;
	struct rt_socket *so;
	int r;

	if (iovcnt < 0 || iovcnt > SOCK_IOV_MAX || (iovcnt && !iov))
		return -RT_EINVAL;
	if (to) {
		r = check_addr(to, tolen, &sin);
		if (r < 0)
			return r;
		if (sin.sin_family != RT_AF_INET && sin.sin_family != RT_AF_UNSPEC)
			return -RT_EAFNOSUPPORT;
		sin.sin_family = RT_AF_INET;
		to = &sin;
	}
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r == 0)
		r = sock_sendit(so, iov, iovcnt, flags, to);
	rt_unlock();
	return r;
}

int rt_api_recvmsg(struct rt_sockctx *c, int fd, const struct rt_iov *iov, int iovcnt,
                   int flags, struct rt_sockaddr_in *from, u32 *fromlen, int *msgflags)
{
	struct rt_sockaddr_in sin;
	struct rt_socket *so;
	int r;

	if (iovcnt < 0 || iovcnt > SOCK_IOV_MAX || (iovcnt && !iov))
		return -RT_EINVAL;
	if (from && !fromlen)
		return -RT_EFAULT;
	if (msgflags)
		*msgflags = 0;
	memset(&sin, 0, sizeof(sin));
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r == 0) {
		int stream = so->type == RT_SOCK_STREAM;
		/* 'so' may be freed by sock_recvit if it was closed meanwhile */
		r = sock_recvit(so, iov, iovcnt, flags, from ? &sin : NULL, msgflags);
		if (from && r >= 0) {
			if (stream) {
				*fromlen = 0;    /* no address for streams (BSD) */
			} else {
				sin.sin_len = sizeof(sin);
				sin.sin_family = RT_AF_INET;
				copy_addr_out(&sin, from, fromlen);
			}
		}
	}
	rt_unlock();
	return r;
}

int rt_api_send(struct rt_sockctx *c, int fd, const void *buf, u32 len, int flags)
{
	struct rt_iov v;

	v.base = (void *)buf;
	v.len = len;
	return rt_api_sendmsg(c, fd, &v, 1, flags, NULL, 0);
}

int rt_api_recv(struct rt_sockctx *c, int fd, void *buf, u32 len, int flags)
{
	struct rt_iov v;

	v.base = buf;
	v.len = len;
	return rt_api_recvmsg(c, fd, &v, 1, flags, NULL, NULL, NULL);
}

int rt_api_sendto(struct rt_sockctx *c, int fd, const void *buf, u32 len, int flags,
                  const struct rt_sockaddr_in *to, u32 tolen)
{
	struct rt_iov v;

	v.base = (void *)buf;
	v.len = len;
	return rt_api_sendmsg(c, fd, &v, 1, flags, to, tolen);
}

int rt_api_recvfrom(struct rt_sockctx *c, int fd, void *buf, u32 len, int flags,
                    struct rt_sockaddr_in *from, u32 *fromlen)
{
	struct rt_iov v;

	v.base = buf;
	v.len = len;
	return rt_api_recvmsg(c, fd, &v, 1, flags, from, fromlen, NULL);
}

int rt_api_shutdown(struct rt_sockctx *c, int fd, int how)
{
	struct rt_socket *so;
	int r;

	if (how < 0 || how > 2)
		return -RT_EINVAL;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (so->type == RT_SOCK_STREAM &&
	    !(so->state & (RT_SS_ISCONNECTED | RT_SS_ISCONNECTING))) {
		r = -RT_ENOTCONN;
		goto out;
	}
	if (how != 1)
		so->state |= RT_SS_CANTRCVMORE;
	if (how != 0)
		so->state |= RT_SS_CANTSENDMORE;
	if (how != 1)
		rt_bufq_purge(&so->rcvq);
	r = so->ops->shutdown ? so->ops->shutdown(so, how) : 0;
	rt_sock_wakeup(so, 0);
out:
	rt_unlock();
	return r;
}

/* ======================================================================
 * Socket options
 *
 * Convention for level RT_SOL_SOCKET: the socket layer validates and stores
 * every SOL_SOCKET option itself (so->options, linger, buffer sizes,
 * timeouts, event mask), then calls ops->setsockopt(so, RT_SOL_SOCKET,
 * name, val, len) so the protocol can react (TCP: SO_KEEPALIVE, SO_SNDBUF/
 * SO_RCVBUF re-reading so->sndbuf/rcvbuf, SO_LINGER ...). The protocol
 * returns 0, or -RT_ENOPROTOOPT meaning "nothing to do for me" (ignored);
 * any other error is returned to the caller. getsockopt(SOL_SOCKET) is
 * answered by the socket layer alone. Other levels go to the protocol.
 * ====================================================================== */

static int opt_int(const void *val, u32 len, s32 *out)
{
	if (!val || len < sizeof(s32))
		return -RT_EINVAL;
	memcpy(out, val, sizeof(s32));
	return 0;
}

static int sol_set(struct rt_socket *so, int name, const void *val, u32 len)
{
	struct rt_linger lg;
	struct rt_timeval tv;
	s32 v = 0;
	u32 ms;
	int r;

	switch (name) {
	case RT_SO_DEBUG: case RT_SO_REUSEADDR: case RT_SO_KEEPALIVE:
	case RT_SO_DONTROUTE: case RT_SO_BROADCAST: case RT_SO_USELOOPBACK:
	case RT_SO_OOBINLINE: case RT_SO_REUSEPORT:
		if ((r = opt_int(val, len, &v)) < 0)
			return r;
		if (v)
			so->options |= (u32)name;
		else
			so->options &= ~(u32)name;
		return 0;
	case RT_SO_LINGER:
		if (!val || len < sizeof(lg))
			return -RT_EINVAL;
		memcpy(&lg, val, sizeof(lg));
		so->linger_on = lg.l_onoff ? 1 : 0;
		so->linger_s = (u16)(lg.l_linger < 0 ? 0 : lg.l_linger > 65535 ? 65535 : lg.l_linger);
		if (lg.l_onoff)
			so->options |= RT_SO_LINGER;
		else
			so->options &= ~(u32)RT_SO_LINGER;
		return 0;
	case RT_SO_SNDBUF: case RT_SO_RCVBUF:
		if ((r = opt_int(val, len, &v)) < 0)
			return r;
		if (v <= 0)
			return -RT_EINVAL;
		if (v > sock_maxsockbuf)
			v = sock_maxsockbuf;
		if (v < 512)
			v = 512;
		if (name == RT_SO_SNDBUF) {
			so->sndbuf = (u32)v;
			if (so->sndlowat > so->sndbuf)
				so->sndlowat = so->sndbuf;
		} else {
			so->rcvbuf = (u32)v;
		}
		return 0;
	case RT_SO_SNDLOWAT: case RT_SO_RCVLOWAT:
		if ((r = opt_int(val, len, &v)) < 0)
			return r;
		if (v < 1)
			v = 1;
		if (name == RT_SO_SNDLOWAT)
			so->sndlowat = RT_MIN((u32)v, so->sndbuf);
		else
			so->rcvlowat = RT_MIN((u32)v, so->rcvbuf);
		return 0;
	case RT_SO_SNDTIMEO: case RT_SO_RCVTIMEO:
		if (!val || len < sizeof(tv))
			return -RT_EINVAL;
		memcpy(&tv, val, sizeof(tv));
		if (tv.tv_micro >= 1000000u)
			return -RT_EDOM;
		if (tv.tv_secs >= 0x7fffffffu / 1000u - 1)
			ms = 0x7fffffffu;
		else
			ms = tv.tv_secs * 1000u + (tv.tv_micro + 999u) / 1000u;
		if (name == RT_SO_SNDTIMEO)
			so->sndtimeo_ms = ms ? (s32)ms : -1;
		else
			so->rcvtimeo_ms = ms ? (s32)ms : -1;
		return 0;
	case RT_SO_EVENTMASK:
		if ((r = opt_int(val, len, &v)) < 0)
			return r;
		so->event_mask = (u32)v & 0x7f;
		so->events_pending &= so->event_mask;
		return 0;
	}
	return -RT_ENOPROTOOPT;
}

int rt_api_setsockopt(struct rt_sockctx *c, int fd, int level, int name, const void *val, u32 len)
{
	struct rt_socket *so;
	int r;

	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (level == RT_SOL_SOCKET) {
		r = sol_set(so, name, val, len);
		if (r == 0 && name == RT_SO_EVENTMASK && c->owner)
			so->owner = c->owner;
		if (r == 0 && so->ops->setsockopt) {
			int pr = so->ops->setsockopt(so, level, name, val, len);
			if (pr < 0 && pr != -RT_ENOPROTOOPT)
				r = pr;
		}
	} else {
		r = so->ops->setsockopt ? so->ops->setsockopt(so, level, name, val, len)
		                        : -RT_ENOPROTOOPT;
	}
out:
	rt_unlock();
	return r;
}

static int put_opt(void *val, u32 *len, const void *src, u32 n)
{
	if (*len < n) {
		if (n == sizeof(s32))
			return -RT_EINVAL;
		n = *len;            /* structures are truncated (BSD) */
	}
	memcpy(val, src, n);
	*len = n;
	return 0;
}

int rt_api_getsockopt(struct rt_sockctx *c, int fd, int level, int name, void *val, u32 *len)
{
	struct rt_socket *so;
	struct rt_linger lg;
	struct rt_timeval tv;
	s32 v, ms;
	int r;

	if (!val || !len)
		return -RT_EFAULT;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (level != RT_SOL_SOCKET) {
		r = so->ops->getsockopt ? so->ops->getsockopt(so, level, name, val, len)
		                        : -RT_ENOPROTOOPT;
		goto out;
	}
	switch (name) {
	case RT_SO_DEBUG: case RT_SO_REUSEADDR: case RT_SO_KEEPALIVE:
	case RT_SO_DONTROUTE: case RT_SO_BROADCAST: case RT_SO_USELOOPBACK:
	case RT_SO_OOBINLINE: case RT_SO_REUSEPORT: case RT_SO_ACCEPTCONN:
		v = (s32)(so->options & (u32)name);   /* 4.4BSD: the flag value */
		break;
	case RT_SO_LINGER:
		lg.l_onoff = so->linger_on ? RT_SO_LINGER : 0;
		lg.l_linger = so->linger_s;
		r = put_opt(val, len, &lg, sizeof(lg));
		goto out;
	case RT_SO_SNDBUF:  v = (s32)so->sndbuf; break;
	case RT_SO_RCVBUF:  v = (s32)so->rcvbuf; break;
	case RT_SO_SNDLOWAT: v = (s32)so->sndlowat; break;
	case RT_SO_RCVLOWAT: v = (s32)so->rcvlowat; break;
	case RT_SO_SNDTIMEO: case RT_SO_RCVTIMEO:
		ms = name == RT_SO_SNDTIMEO ? so->sndtimeo_ms : so->rcvtimeo_ms;
		if (ms < 0)
			ms = 0;
		tv.tv_secs = (u32)ms / 1000u;
		tv.tv_micro = ((u32)ms % 1000u) * 1000u;
		r = put_opt(val, len, &tv, sizeof(tv));
		goto out;
	case RT_SO_ERROR:   v = so->so_error; so->so_error = 0; break;
	case RT_SO_TYPE:    v = so->type; break;
	case RT_SO_EVENTMASK: v = (s32)so->event_mask; break;
	default:
		r = -RT_ENOPROTOOPT;
		goto out;
	}
	r = put_opt(val, len, &v, sizeof(v));
out:
	rt_unlock();
	return r;
}

static int sock_name(struct rt_sockctx *c, int fd, struct rt_sockaddr_in *a, u32 *alen, int peer)
{
	struct rt_sockaddr_in sin;
	struct rt_socket *so;
	int r;

	if (!a || !alen)
		return -RT_EFAULT;
	rt_lock();
	r = fd_get(c, fd, &so);
	if (r == 0) {
		memset(&sin, 0, sizeof(sin));
		if (peer && so->type == RT_SOCK_STREAM && !(so->state & RT_SS_ISCONNECTED))
			r = -RT_ENOTCONN;
		else
			r = so->ops->getname ? so->ops->getname(so, &sin, peer) : -RT_EOPNOTSUPP;
		if (r == 0) {
			sin.sin_len = sizeof(sin);
			sin.sin_family = RT_AF_INET;
			copy_addr_out(&sin, a, alen);
		}
	}
	rt_unlock();
	return r;
}

int rt_api_getsockname(struct rt_sockctx *c, int fd, struct rt_sockaddr_in *a, u32 *alen)
{
	return sock_name(c, fd, a, alen, 0);
}

int rt_api_getpeername(struct rt_sockctx *c, int fd, struct rt_sockaddr_in *a, u32 *alen)
{
	return sock_name(c, fd, a, alen, 1);
}

/* ======================================================================
 * ioctl
 * ====================================================================== */

static u32 classful_mask(u32 addr)
{
	u32 h = rt_ntohl(addr);

	if ((h & 0x80000000u) == 0)
		return rt_htonl(0xff000000u);
	if ((h & 0xc0000000u) == 0x80000000u)
		return rt_htonl(0xffff0000u);
	if ((h & 0xe0000000u) == 0xc0000000u)
		return rt_htonl(0xffffff00u);
	return rt_htonl(0xffffffffu);
}

static void ifr_put_addr(struct rt_ifreq *ifr, u32 addr)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = addr;
	memcpy(&ifr->ifr_ifru.ifru_addr, &sin, sizeof(sin));
}

static int ifr_get_addr(const struct rt_ifreq *ifr, u32 *addr)
{
	struct rt_sockaddr_in sin;

	memcpy(&sin, &ifr->ifr_ifru.ifru_addr, sizeof(sin));
	if (sin.sin_family != RT_AF_INET && sin.sin_family != RT_AF_UNSPEC)
		return -RT_EAFNOSUPPORT;
	*addr = sin.sin_addr;
	return 0;
}

static int ifconf(struct rt_ifconf *ifc)
{
	struct rt_netif *nif;
	struct rt_ifreq ifr;
	u8 *p = ifc->ifc_buf;
	s32 used = 0;

	if (ifc->ifc_len < 0)
		return -RT_EINVAL;
	if (!p && ifc->ifc_len > 0)
		return -RT_EFAULT;
	for (nif = rt_netif_list(); nif; nif = nif->next) {
		if (!nif->addr)
			continue;
		if (ifc->ifc_len - used < (s32)sizeof(ifr))
			break;
		memset(&ifr, 0, sizeof(ifr));
		memcpy(ifr.ifr_name, nif->name, RT_IFNAMSIZ);
		ifr_put_addr(&ifr, nif->addr);
		memcpy(p + used, &ifr, sizeof(ifr));
		used += sizeof(ifr);
	}
	ifc->ifc_len = used;
	return 0;
}

/* Interface ioctls (no privilege checks on AmigaOS). */
static int ifioctl(u32 req, struct rt_ifreq *ifr)
{
	char name[RT_IFNAMSIZ + 1];
	struct rt_netif *nif;
	u32 a, peer;
	s32 v;
	s16 f;
	int r;

	if (!ifr)
		return -RT_EFAULT;
	memcpy(name, ifr->ifr_name, RT_IFNAMSIZ);
	name[RT_IFNAMSIZ] = 0;
	nif = rt_netif_find(name);
	if (!nif)
		return -RT_ENXIO;
	peer = (nif->flags & RT_IFF_POINTOPOINT) ? nif->dstaddr : 0;
	switch (req) {
	case RT_SIOCGIFADDR:
		if (!nif->addr)
			return -RT_EADDRNOTAVAIL;
		ifr_put_addr(ifr, nif->addr);
		return 0;
	case RT_SIOCGIFNETMASK:
		if (!nif->addr)
			return -RT_EADDRNOTAVAIL;
		ifr_put_addr(ifr, nif->netmask);
		return 0;
	case RT_SIOCGIFBRDADDR:
		if (!(nif->flags & RT_IFF_BROADCAST))
			return -RT_EINVAL;
		ifr_put_addr(ifr, nif->broadaddr);
		return 0;
	case RT_SIOCGIFDSTADDR:
		if (!(nif->flags & RT_IFF_POINTOPOINT))
			return -RT_EINVAL;
		ifr_put_addr(ifr, nif->dstaddr);
		return 0;
	case RT_SIOCGIFFLAGS:
		f = (s16)nif->flags;
		memcpy(&ifr->ifr_ifru.ifru_flags, &f, sizeof(f));
		return 0;
	case RT_SIOCGIFMTU:
		v = nif->mtu;
		memcpy(&ifr->ifr_ifru.ifru_mtu, &v, sizeof(v));
		return 0;
	case RT_SIOCGIFMETRIC:
		v = (s32)nif->metric;
		memcpy(&ifr->ifr_ifru.ifru_metric, &v, sizeof(v));
		return 0;
	case RT_SIOCSIFADDR:
		if ((r = ifr_get_addr(ifr, &a)) < 0)
			return r;
		r = rt_netif_set_addr(nif, a, nif->netmask ? nif->netmask : classful_mask(a), peer);
		if (r == 0 && !(nif->flags & RT_IFF_UP))
			rt_netif_up(nif);   /* as BSD ether_ioctl(SIOCSIFADDR) */
		return r;
	case RT_SIOCSIFNETMASK:
		if ((r = ifr_get_addr(ifr, &a)) < 0)
			return r;
		if (!nif->addr) {
			nif->netmask = a;
			return 0;
		}
		return rt_netif_set_addr(nif, nif->addr, a, peer);
	case RT_SIOCSIFBRDADDR:
		if (!(nif->flags & RT_IFF_BROADCAST))
			return -RT_EINVAL;
		if ((r = ifr_get_addr(ifr, &a)) < 0)
			return r;
		nif->broadaddr = a;
		return 0;
	case RT_SIOCSIFDSTADDR:
		if (!(nif->flags & RT_IFF_POINTOPOINT))
			return -RT_EINVAL;
		if ((r = ifr_get_addr(ifr, &a)) < 0)
			return r;
		if (!nif->addr) {
			nif->dstaddr = a;
			return 0;
		}
		return rt_netif_set_addr(nif, nif->addr, nif->netmask, a);
	case RT_SIOCSIFFLAGS: {
		/* BSD IFF_CANTCHANGE bits keep their value. */
		const u16 cantchange = RT_IFF_BROADCAST | RT_IFF_POINTOPOINT | RT_IFF_RUNNING |
		                       RT_IFF_SIMPLEX | RT_IFF_MULTICAST | RT_IFF_LOOPBACK;
		u16 nf;
		memcpy(&f, &ifr->ifr_ifru.ifru_flags, sizeof(f));
		nf = (u16)((nif->flags & cantchange) | ((u16)f & ~cantchange));
		if ((nif->flags & RT_IFF_UP) && !(nf & RT_IFF_UP))
			rt_netif_down(nif);
		else if (!(nif->flags & RT_IFF_UP) && (nf & RT_IFF_UP))
			rt_netif_up(nif);
		nf = (u16)((nf & ~RT_IFF_UP) | (nif->flags & RT_IFF_UP));
		if (nf != nif->flags && nif->ioctl_flags)
			nif->ioctl_flags(nif, nf);
		nif->flags = nf;
		return 0;
	}
	case RT_SIOCSIFMTU:
		memcpy(&v, &ifr->ifr_ifru.ifru_mtu, sizeof(v));
		if (v < 68 || v > (nif->type == RT_IFT_ETHER ? 1500 : 65535))
			return -RT_EINVAL;
		nif->mtu = (u16)v;
		return 0;
	case RT_SIOCSIFMETRIC:
		memcpy(&v, &ifr->ifr_ifru.ifru_metric, sizeof(v));
		nif->metric = (u32)v;
		return 0;
	case RT_SIOCDIFADDR:
		if (!nif->addr)
			return -RT_EADDRNOTAVAIL;
		if ((r = ifr_get_addr(ifr, &a)) < 0)
			return r;
		if (a && a != nif->addr)
			return -RT_EADDRNOTAVAIL;
		return rt_netif_set_addr(nif, 0, 0, 0);
	}
	return -RT_EINVAL;
}

int rt_api_ioctl(struct rt_sockctx *c, int fd, u32 req, void *arg)
{
	struct rt_socket *so;
	s32 v;
	int r;

	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (!arg && req != RT_SIOCGIFCONF) {
		r = -RT_EFAULT;
		goto out;
	}
	switch (req) {
	case RT_FIONBIO:
	case RT_FIOASYNC: {
		u32 bit = req == RT_FIONBIO ? RT_SS_NBIO : RT_SS_ASYNC;
		memcpy(&v, arg, sizeof(v));
		if (v)
			so->state |= bit;
		else
			so->state &= ~bit;
		if (bit == RT_SS_ASYNC && v && c->owner)
			so->owner = c->owner;
		break;
	}
	case RT_FIOSETOWN:
	case RT_SIOCSPGRP:
		memcpy(&v, arg, sizeof(v));
		so->pgid = (u32)v;
		break;
	case RT_FIOGETOWN:
	case RT_SIOCGPGRP:
		v = (s32)so->pgid;
		memcpy(arg, &v, sizeof(v));
		break;
	case RT_FIONREAD:
		r = so->ops->ioctl ? so->ops->ioctl(so, req, arg) : -RT_EINVAL;
		if (r == -RT_EINVAL && so->type != RT_SOCK_STREAM) {
			/* datagram queue kept by the socket layer: bytes queued */
			v = (s32)so->rcvq.bytes;
			memcpy(arg, &v, sizeof(v));
			r = 0;
		}
		break;
	case RT_SIOCATMARK:
		r = so->ops->ioctl ? so->ops->ioctl(so, req, arg) : -RT_EINVAL;
		if (r == -RT_EINVAL) {
			v = (so->state & RT_SS_RCVATMARK) ? 1 : 0;
			memcpy(arg, &v, sizeof(v));
			r = 0;
		}
		break;
	case RT_SIOCGIFCONF:
		r = arg ? ifconf(arg) : -RT_EFAULT;
		break;
	case RT_SIOCGIFADDR: case RT_SIOCGIFNETMASK: case RT_SIOCGIFBRDADDR:
	case RT_SIOCGIFDSTADDR: case RT_SIOCGIFFLAGS: case RT_SIOCGIFMTU:
	case RT_SIOCGIFMETRIC: case RT_SIOCSIFADDR: case RT_SIOCSIFNETMASK:
	case RT_SIOCSIFBRDADDR: case RT_SIOCSIFDSTADDR: case RT_SIOCSIFFLAGS:
	case RT_SIOCSIFMTU: case RT_SIOCSIFMETRIC: case RT_SIOCDIFADDR:
		r = ifioctl(req, arg);
		break;
	default:
		r = so->ops->ioctl ? so->ops->ioctl(so, req, arg) : -RT_EINVAL;
		break;
	}
out:
	rt_unlock();
	return r;
}

/* ======================================================================
 * close / dup2
 * ====================================================================== */

/* SO_LINGER with a timeout on a blocking stream socket: send FIN and wait
 * until the connection is gone or the time is up (4.4BSD soclose()). */
static void linger_wait(struct rt_socket *so)
{
	struct rt_wait_entry we;
	u32 start = rt_now_ms();
	s32 timeo = (s32)so->linger_s * 1000;

	if (so->ops->shutdown)
		so->ops->shutdown(so, 1);
	so->state |= RT_SS_CANTSENDMORE;
	wait_begin(so, &we);
	for (;;) {
		u32 m = so->ops->poll ? so->ops->poll(so) : RT_POLLHUP;
		if ((m & (RT_POLLHUP | RT_POLLERR)) || so->so_error ||
		    !(so->state & RT_SS_ISCONNECTED))
			break;
		if (sock_sleep(so, &we, start, timeo) < 0)
			break;
	}
	rt_waitq_remove(&so->wq, &we);
	/* caller still holds a descriptor reference: so stays alive */
}

int rt_api_close(struct rt_sockctx *c, int fd)
{
	struct rt_socket *so;

	rt_lock();
	if (!c || !c->fds || fd < 0 || fd >= c->nfds || !(so = c->fds[fd])) {
		rt_unlock();
		return -RT_EBADF;
	}
	if (so != SOCK_RESERVED && so->refcnt == 1 && so->linger_on && so->linger_s &&
	    so->type == RT_SOCK_STREAM && (so->state & RT_SS_ISCONNECTED) &&
	    !(so->state & RT_SS_NBIO)) {
		linger_wait(so);
		if (c->fds[fd] != so) {    /* closed by someone else meanwhile */
			rt_unlock();
			return -RT_EBADF;
		}
	}
	fd_release(c, fd);
	rt_unlock();
	return 0;
}

int rt_api_dup2(struct rt_sockctx *c, int oldfd, int newfd)
{
	struct rt_socket *so = NULL;
	int r;

	rt_lock();
	if (!c || !c->fds) {
		r = -RT_EBADF;
		goto out;
	}
	if (oldfd == -1) {
		/* AmiTCP: Dup2Socket(-1, fd) marks fd as in use */
		if (newfd < 0 || newfd >= c->nfds) {
			r = -RT_EBADF;
			goto out;
		}
		if (c->fds[newfd])
			fd_release(c, newfd);
		c->fds[newfd] = SOCK_RESERVED;
		r = newfd;
		goto out;
	}
	r = fd_get(c, oldfd, &so);
	if (r < 0)
		goto out;
	if (newfd == -1) {
		r = fd_alloc(c);
		if (r < 0)
			goto out;
		newfd = r;
	} else if (newfd < 0 || newfd >= c->nfds) {
		r = -RT_EBADF;
		goto out;
	}
	if (newfd == oldfd) {
		r = newfd;
		goto out;
	}
	so->refcnt++;           /* keep it alive while newfd is recycled */
	if (c->fds[newfd])
		fd_release(c, newfd);
	fd_install(c, newfd, so, 0);
	r = newfd;
out:
	rt_unlock();
	return r;
}

/* ======================================================================
 * WaitSelect
 * ====================================================================== */

struct sel_ent {
	struct rt_wait_entry we;
	struct rt_socket *so;
};

#define FD_ISSET_W(set, fd) ((set) && ((set)[(fd) >> 5] & (1u << ((fd) & 31))))

/* Count ready descriptors; with 'write' set, also reduce the sets to the
 * ready ones. Returns count or -RT_EBADF. */
static int sel_scan(struct rt_sockctx *c, int nfds, u32 *rd, u32 *wr, u32 *ex, int write)
{
	int fd, n = 0;

	for (fd = 0; fd < nfds; fd++) {
		int ir = FD_ISSET_W(rd, fd), iw = FD_ISSET_W(wr, fd), ie = FD_ISSET_W(ex, fd);
		struct rt_socket *so;
		u32 m, bit = 1u << (fd & 31);

		if (!(ir | iw | ie))
			continue;
		so = c->fds[fd];
		if (!so || so == SOCK_RESERVED)
			return -RT_EBADF;
		m = sock_poll(so);
		if (ir && !(m & (RT_POLLIN | RT_POLLERR | RT_POLLHUP)))
			ir = 0;
		if (iw && !(m & (RT_POLLOUT | RT_POLLERR)) && !(so->state & RT_SS_CANTSENDMORE))
			iw = 0;
		if (ie && !(m & RT_POLLPRI))
			ie = 0;
		n += (ir != 0) + (iw != 0) + (ie != 0);
		if (write) {
			if (rd && !ir) rd[fd >> 5] &= ~bit;
			if (wr && !iw) wr[fd >> 5] &= ~bit;
			if (ex && !ie) ex[fd >> 5] &= ~bit;
		}
	}
	return n;
}

static void sel_clear(u32 *set, int nfds)
{
	if (set)
		memset(set, 0, (size_t)((nfds + 31) >> 5) * sizeof(u32));
}

int rt_api_select(struct rt_sockctx *c, int nfds, u32 *rd, u32 *wr, u32 *ex,
                  s32 timeout_ms, u32 *sigmask)
{
	struct sel_ent local[SOCK_SEL_LOCAL];
	struct sel_ent *ents = NULL;
	struct rt_sleeper *self;
	u32 sigin = sigmask ? *sigmask : 0, got = 0, start = rt_now_ms();
	int fd, n = 0, i, ready, w;

	if (sigmask)
		*sigmask = 0;
	if (!c || nfds < 0)
		return -RT_EINVAL;
	rt_lock();
	if (nfds > c->nfds)
		nfds = c->nfds;
	ready = sel_scan(c, nfds, rd, wr, ex, 0);
	if (ready != 0 || timeout_ms == 0)
		goto done;

	for (fd = 0; fd < nfds; fd++)
		if (FD_ISSET_W(rd, fd) || FD_ISSET_W(wr, fd) || FD_ISSET_W(ex, fd))
			n++;
	ents = n <= SOCK_SEL_LOCAL ? local : rt_malloc((size_t)n * sizeof(*ents));
	if (!ents) {
		ready = -RT_ENOMEM;
		goto done;
	}
	self = rt_sleeper_self();
	for (fd = 0, i = 0; fd < nfds; fd++) {
		if (!(FD_ISSET_W(rd, fd) || FD_ISSET_W(wr, fd) || FD_ISSET_W(ex, fd)))
			continue;
		ents[i].so = c->fds[fd];
		rt_waitq_add(&ents[i].so->wq, &ents[i].we, self);
		i++;
	}
	for (;;) {
		u32 sig = sigin;
		s32 left;

		ready = sel_scan(c, nfds, rd, wr, ex, 0);
		if (ready != 0)
			break;
		left = time_left(start, timeout_ms);
		if (left == 0)
			break;
		w = rt_sleeper_wait(self, left, sigmask ? &sig : NULL);
		if (w == -RT_EINTR) {
			ready = -RT_EINTR;
			break;
		}
		if (sig & sigin) {
			got = sig & sigin;
			break;
		}
		if (w == -RT_ETIMEDOUT)
			break;
	}
	for (i = 0; i < n; i++)
		rt_waitq_remove(&ents[i].so->wq, &ents[i].we);
	for (i = 0; i < n; i++) {
		struct rt_socket *so = ents[i].so;
		int j;
		if (!so || !(so->state & SOCK_SS_DEAD))
			continue;
		for (j = i; j < n; j++)       /* the same socket may appear twice */
			if (ents[j].so == so)
				ents[j].so = NULL;
		sock_waiter_done(so);
	}
	if (ents != local)
		rt_free(ents);
	if (ready == 0)
		ready = sel_scan(c, nfds, rd, wr, ex, 0);   /* last chance */
done:
	if (ready > 0) {
		sel_scan(c, nfds, rd, wr, ex, 1);
	} else if (ready == 0) {
		sel_clear(rd, nfds);
		sel_clear(wr, nfds);
		sel_clear(ex, nfds);
	}
	if (sigmask)
		*sigmask = got;
	rt_unlock();
	return ready;
}

/* ======================================================================
 * ObtainSocket / ReleaseSocket / ReleaseCopyOfSocket
 * ====================================================================== */

static struct rt_socket *released_find(s32 id)
{
	struct rt_socket *so;

	for (so = rt_all_sockets; so; so = so->next_all)
		if (so->release_id == id && !(so->state & SOCK_SS_DEAD))
			return so;
	return NULL;
}

int rt_api_release(struct rt_sockctx *c, int fd, s32 id, int copy)
{
	struct rt_socket *so;
	int r;

	rt_lock();
	r = fd_get(c, fd, &so);
	if (r < 0)
		goto out;
	if (so->release_id != -1) {
		r = -RT_EBUSY;          /* already on the list */
		goto out;
	}
	if (id == -1) {
		do {
			id = release_next_id++;
			if (release_next_id <= 0)
				release_next_id = 0x10000;
		} while (released_find(id));
	} else if (id < 0 || released_find(id)) {
		r = -RT_EINVAL;
		goto out;
	}
	so->release_id = id;
	so->refcnt++;              /* the list's reference */
	if (!copy)
		fd_release(c, fd);
	r = (int)id;
out:
	rt_unlock();
	return r;
}

int rt_api_obtain(struct rt_sockctx *c, s32 id, int domain, int type, int protocol)
{
	struct rt_socket *so;
	int fd;

	if (domain != RT_AF_INET)
		return -RT_EAFNOSUPPORT;
	rt_lock();
	so = released_find(id);
	if (!so || id == -1 || so->type != type ||
	    (protocol != 0 && so->protocol != protocol)) {
		fd = -RT_EINVAL;
		goto out;
	}
	fd = c && c->fds ? fd_alloc(c) : -RT_EBADF;
	if (fd < 0)
		goto out;
	so->release_id = -1;
	so->owner = c->owner;
	fd_install(c, fd, so, 0);     /* takes over the list's reference */
out:
	rt_unlock();
	return fd;
}

/* ======================================================================
 * GetSocketEvents
 * ====================================================================== */

int rt_api_get_events(struct rt_sockctx *c, u32 *events)
{
	int i, fd, r = -1;

	if (!c || !c->fds)
		return -1;
	rt_lock();
	for (i = 0; i < c->nfds; i++) {
		struct rt_socket *so;
		fd = (c->ev_next + i) % c->nfds;
		so = c->fds[fd];
		if (!so || so == SOCK_RESERVED || !so->events_pending)
			continue;
		if (events)
			*events = so->events_pending;
		so->events_pending = 0;
		c->ev_next = (fd + 1) % c->nfds;
		r = fd;
		break;
	}
	rt_unlock();
	return r;
}
