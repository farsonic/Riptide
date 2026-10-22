/*
 * Riptide - socket layer <-> protocol contract.
 *
 * The socket layer (src/core/socket.c) owns 'struct rt_socket', descriptor
 * tables, blocking/timeouts, select and the BSD API semantics. Protocols
 * (TCP, UDP, raw) implement 'struct rt_proto_ops' and never sleep: every
 * op is non-blocking and returns -RT_EWOULDBLOCK / -RT_EINPROGRESS when it
 * cannot complete; the socket layer then sleeps on so->wq and retries.
 * Protocols call rt_sock_wakeup() whenever something a sleeper may wait for
 * changed (data or FIN arrived, send space freed, connection established,
 * error set, new connection queued for accept).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_SOCKET_H
#define RT_SOCKET_H

#include "rt_types.h"
#include "rt_os.h"
#include "rt_buf.h"
#include "rt_errno.h"
#include "rt_bsd.h"

struct rt_socket;
struct rt_sockctx;

/* Scatter/gather vector (host-native layout; the Amiga glue converts). */
struct rt_iov {
	void *base;
	u32   len;
};

/* poll() style readiness bits returned by ops->poll */
#define RT_POLLIN   0x0001   /* readable: data, FIN, pending accept, error */
#define RT_POLLPRI  0x0002   /* out of band data / urgent mark */
#define RT_POLLOUT  0x0004   /* writable (send space >= low water mark) */
#define RT_POLLERR  0x0008   /* so_error set */
#define RT_POLLHUP  0x0010   /* both directions shut / connection gone */

/* so->state bits (cf. BSD SS_*) */
#define RT_SS_NOFDREF        0x0001  /* no descriptor references it any more */
#define RT_SS_ISCONNECTED    0x0002
#define RT_SS_ISCONNECTING   0x0004
#define RT_SS_ISDISCONNECTING 0x0008
#define RT_SS_CANTSENDMORE   0x0010
#define RT_SS_CANTRCVMORE    0x0020
#define RT_SS_RCVATMARK      0x0040
#define RT_SS_NBIO           0x0100  /* FIONBIO */
#define RT_SS_ASYNC          0x0200  /* FIOASYNC */
#define RT_SS_ISLISTENING    0x0400
#define RT_SS_ISBOUND        0x0800
#define RT_SS_DEFERCONN      0x1000  /* TCP Fast Open: connect() returned, no SYN yet */

/* Internal ioctl: a deferred connect is waited on for reading (WaitSelect),
 * so the protocol sends its SYN now (the server may speak first). */
#define RT_SIOC_KICKCONN     0x7f000001u

/* Addresses between the socket layer and the protocols are always a
 * struct rt_sockaddr_in6 in normalised form: IPv4 addresses (AF_INET
 * sockets, or IPv4-mapped destinations of AF_INET6 sockets) are
 * IPv4-mapped (::ffff:a.b.c.d), and sin6_family carries the family the
 * caller used (RT_AF_INET, RT_AF_INET6 or RT_AF_UNSPEC for a datagram
 * "disconnect"). The socket layer converts to and from the caller's
 * sockaddr_in / sockaddr_in6 according to so->domain. */
struct rt_proto_ops {
	const char *name;
	/* Create protocol state (so->pcb). */
	int  (*attach)(struct rt_socket *so);
	/* Descriptor closed. Protocol may keep running (TCP FIN handshake,
	 * TIME_WAIT) but must drop every reference to 'so' before returning;
	 * the socket layer frees 'so' right after. If so->linger_on and
	 * so->linger_s == 0 the protocol must abort (TCP: send RST). */
	void (*close)(struct rt_socket *so);
	int  (*bind)(struct rt_socket *so, const struct rt_sockaddr_in6 *sin);
	int  (*listen)(struct rt_socket *so, int backlog);
	/* Start connecting. Returns 0 if connected at once (UDP/raw),
	 * -RT_EINPROGRESS for TCP (socket layer waits for RT_SS_ISCONNECTED
	 * or so_error), or -errno. */
	int  (*connect)(struct rt_socket *so, const struct rt_sockaddr_in6 *sin);
	/* Send: copy up to the total iov length from user memory. Returns the
	 * number of bytes accepted (>0), or -RT_EWOULDBLOCK if nothing could
	 * be queued, or -errno (-RT_EPIPE, -RT_ENOTCONN, -RT_EMSGSIZE ...).
	 * Datagram protocols send atomically or not at all. 'to' may be NULL. */
	int  (*send)(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
	             int flags, const struct rt_sockaddr_in6 *to);
	/* Receive into iov. Returns bytes (>=0, 0 = EOF for streams) or
	 * -RT_EWOULDBLOCK or -errno. 'from' (may be NULL) gets the peer address.
	 * *msgflags gets RT_MSG_TRUNC etc. Must honour RT_MSG_PEEK and
	 * RT_MSG_OOB. */
	int  (*recv)(struct rt_socket *so, const struct rt_iov *iov, int iovcnt,
	             int flags, struct rt_sockaddr_in6 *from, int *msgflags);
	int  (*shutdown)(struct rt_socket *so, int how);   /* 0 rd, 1 wr, 2 both */
	/* For level RT_SOL_SOCKET the socket layer has already validated and
	 * stored the option (so->options, linger, sndbuf/rcvbuf, timeouts,
	 * event_mask) and calls this only so the protocol can react (TCP:
	 * SO_KEEPALIVE, buffer sizes...). Return 0, or -RT_ENOPROTOOPT for
	 * "nothing to do" (ignored); other errors reach the caller.
	 * getsockopt(SOL_SOCKET) never reaches the protocol. */
	int  (*setsockopt)(struct rt_socket *so, int level, int name, const void *val, u32 len);
	int  (*getsockopt)(struct rt_socket *so, int level, int name, void *val, u32 *len);
	u32  (*poll)(struct rt_socket *so);
	/* Local (peer=0) or remote (peer=1) address. -RT_ENOTCONN if none. */
	int  (*getname)(struct rt_socket *so, struct rt_sockaddr_in6 *sin, int peer);
	/* FIONREAD, SIOCATMARK; others -RT_EINVAL (socket layer handles the
	 * generic and interface ioctls). */
	int  (*ioctl)(struct rt_socket *so, u32 req, void *arg);
};

struct rt_socket {
	struct rt_socket *next_all;   /* global list (netstat, ObtainSocket) */
	u16  type;                    /* RT_SOCK_STREAM/DGRAM/RAW */
	u16  protocol;
	u32  state;                   /* RT_SS_* */
	u32  options;                 /* RT_SO_* boolean options (SOL_SOCKET) */
	int  so_error;                /* pending error (positive errno) */
	u8   linger_on;
	u16  linger_s;
	u32  sndbuf;                  /* SO_SNDBUF (bytes) */
	u32  rcvbuf;                  /* SO_RCVBUF (bytes) */
	u32  sndlowat, rcvlowat;
	s32  sndtimeo_ms, rcvtimeo_ms; /* <0 none */
	struct rt_waitq wq;           /* every sleeper interested in this socket */
	const struct rt_proto_ops *ops;
	void *pcb;                    /* protocol control block */

	/* Datagram receive queue (UDP/raw); stream protocols keep their own.
	 * For each buffer in rcvq, cb holds a struct rt_dgram_cb. */
	struct rt_bufq rcvq;

	/* Listening sockets: completed connections waiting for accept(). */
	struct rt_socket *acceptq_head, *acceptq_tail, *acceptq_next;
	u16  acceptq_len;
	u16  backlog;
	struct rt_socket *listen_parent; /* set while queued on a parent */

	/* Owner for asynchronous notification (FIOASYNC / SBTC_SIGEVENTMASK /
	 * urgent data). Opaque to the core; see rt_sock_async_hook. */
	void *owner;
	u32  events_pending;          /* RT_SEV_* not yet fetched (GetSocketEvents) */
	u32  event_mask;              /* RT_SEV_* the owner wants */

	/* ObtainSocket/ReleaseSocket */
	s32  release_id;              /* -1: not released */
	u32  refcnt;                  /* descriptor references (Dup2Socket) */
	u32  pgid;                    /* FIOSETOWN/SIOCSPGRP value (Amiga: task), 0 = none */
	u16  domain;                  /* RT_AF_INET or RT_AF_INET6 */
};

/* Ancillary info stored in rt_buf.cb of buffers on so->rcvq */
struct rt_dgram_cb {
	struct rt_sockaddr_in6 from;  /* normalised (see rt_proto_ops) */
	rt_in6_addr dst;              /* IP destination (IP_RECVDSTADDR) */
	u32 ifindex;
	u8  hlim, tclass;
};

/* Asynchronous socket events (values of GetSocketEvents / FD_* in the
 * AmiTCP ABI) */
#define RT_SEV_ACCEPT   0x01
#define RT_SEV_CONNECT  0x02
#define RT_SEV_OOB      0x04
#define RT_SEV_READ     0x08
#define RT_SEV_WRITE    0x10
#define RT_SEV_ERROR    0x20
#define RT_SEV_CLOSE    0x40

/* ---- services the socket layer offers to protocols ---- */

/* Wake sleepers; 'events' (RT_SEV_*) are also posted to the owner for
 * asynchronous notification. */
void rt_sock_wakeup(struct rt_socket *so, u32 events);
/* Record an error (positive errno) and wake everybody. */
void rt_sock_set_error(struct rt_socket *so, int err);
/* For listening sockets: allocate a socket for an incoming connection.
 * The child inherits type, protocol, ops, options, buffer sizes and owner.
 * Returns NULL if the accept queue is full (backlog) or no memory. The
 * protocol then attaches its pcb to child->pcb itself (ops->attach is NOT
 * called for children). */
struct rt_socket *rt_sock_new_child(struct rt_socket *listener);
/* Child connection is established: queue it for accept() and wake. */
void rt_sock_child_ready(struct rt_socket *listener, struct rt_socket *child);
/* A child (queued or not) died before accept(): unlink and free it. The
 * protocol must have dropped its own references to child first. */
void rt_sock_child_drop(struct rt_socket *child);
/* Allocate a datagram buffer queue entry respecting so->rcvbuf. Returns
 * 0 on success (b consumed) or -RT_ENOBUFS (b NOT consumed). */
int  rt_sock_queue_dgram(struct rt_socket *so, struct rt_buf *b);

/* Copy helpers between iov and linear memory. They return bytes copied. */
u32  rt_iov_total(const struct rt_iov *iov, int iovcnt);
/* copy 'len' bytes starting at byte 'offset' of the iov into dst */
u32  rt_iov_to_buf(const struct rt_iov *iov, int iovcnt, u32 offset, void *dst, u32 len);
/* rt_iov_to_buf() that also returns the partial checksum of the copied
 * bytes in *sum (computed in the same pass). */
u32  rt_iov_to_buf_csum(const struct rt_iov *iov, int iovcnt, u32 offset, void *dst, u32 len,
                        u32 *sum);
/* copy 'len' bytes of src into the iov starting at byte 'offset' */
u32  rt_buf_to_iov(const struct rt_iov *iov, int iovcnt, u32 offset, const void *src, u32 len);

/* Hook set by the OS glue to deliver asynchronous events to so->owner
 * (AmigaOS: Signal() the owner task with its SIGIO/SIGURG/SIGEVENT masks). */
extern void (*rt_sock_async_hook)(struct rt_socket *so, u32 events);

/* Protocol op tables */
extern const struct rt_proto_ops rt_tcp_ops;
extern const struct rt_proto_ops rt_udp_ops;
extern const struct rt_proto_ops rt_raw_ops;

/* Global list of sockets */
extern struct rt_socket *rt_all_sockets;

/* ========================================================================
 * Descriptor-based API (src/core/socket_api.c). One rt_sockctx per client
 * (AmigaOS: per bsdsocket.library opener; host: per test program).
 * All functions take the stack lock themselves (call WITHOUT it held) and
 * return >=0 on success or -errno; the glue converts to -1 + errno.
 * Addresses are struct rt_sockaddr_in (AF_INET sockets) or struct
 * rt_sockaddr_in6 (AF_INET6 sockets) with their length, in the BSD layout
 * of the Amiga ABI. Returned addresses follow BSD: at most *alen bytes are
 * copied and *alen gets the full length (28, or 24 for callers whose
 * buffer is too small for 28: see rt_bsd.h).
 * ======================================================================== */
struct rt_sockctx {
	struct rt_socket **fds;
	int  nfds;                    /* size of table (getdtablesize) */
	u32  sigintr_mask;            /* Amiga: SBTC_BREAKMASK, default Ctrl-C */
	u32  sigio_mask, sigurg_mask, sigevent_mask;
	void *owner;                  /* glue data (Amiga: struct SocketBase*) */
	struct rt_sockctx *next;
	/* fd callback (RiptideSetFdCallback / SBTC_FDCALLBACK); may be NULL */
	int (*fd_callback)(struct rt_sockctx *ctx, int fd, int action);
	int  ev_next;                 /* GetSocketEvents round robin position */
};
/* fd_callback actions (AmiTCP FDCB_*). For CHECK a non-zero return means
 * "fd is in use by the C library", and the socket layer skips that fd. */
#define RT_FDCB_FREE  0
#define RT_FDCB_ALLOC 1
#define RT_FDCB_CHECK 2

int  rt_ctx_init(struct rt_sockctx *ctx, int nfds, void *owner);
void rt_ctx_destroy(struct rt_sockctx *ctx);        /* closes all fds */
int  rt_ctx_set_dtablesize(struct rt_sockctx *ctx, int nfds);

int  rt_api_socket(struct rt_sockctx *c, int domain, int type, int protocol);
int  rt_api_bind(struct rt_sockctx *c, int fd, const void *a, u32 alen);
int  rt_api_listen(struct rt_sockctx *c, int fd, int backlog);
int  rt_api_accept(struct rt_sockctx *c, int fd, void *a, u32 *alen);
int  rt_api_connect(struct rt_sockctx *c, int fd, const void *a, u32 alen);
int  rt_api_sendmsg(struct rt_sockctx *c, int fd, const struct rt_iov *iov, int iovcnt,
                    int flags, const void *to, u32 tolen);
int  rt_api_recvmsg(struct rt_sockctx *c, int fd, const struct rt_iov *iov, int iovcnt,
                    int flags, void *from, u32 *fromlen, int *msgflags);
int  rt_api_send(struct rt_sockctx *c, int fd, const void *buf, u32 len, int flags);
int  rt_api_recv(struct rt_sockctx *c, int fd, void *buf, u32 len, int flags);
int  rt_api_sendto(struct rt_sockctx *c, int fd, const void *buf, u32 len, int flags,
                   const void *to, u32 tolen);
int  rt_api_recvfrom(struct rt_sockctx *c, int fd, void *buf, u32 len, int flags,
                     void *from, u32 *fromlen);
int  rt_api_shutdown(struct rt_sockctx *c, int fd, int how);
int  rt_api_setsockopt(struct rt_sockctx *c, int fd, int level, int name, const void *val, u32 len);
int  rt_api_getsockopt(struct rt_sockctx *c, int fd, int level, int name, void *val, u32 *len);
int  rt_api_getsockname(struct rt_sockctx *c, int fd, void *a, u32 *alen);
int  rt_api_getpeername(struct rt_sockctx *c, int fd, void *a, u32 *alen);
int  rt_api_ioctl(struct rt_sockctx *c, int fd, u32 req, void *arg);
int  rt_api_close(struct rt_sockctx *c, int fd);
int  rt_api_dup2(struct rt_sockctx *c, int oldfd, int newfd);
/* select: fd sets are arrays of u32 in host order, bit (fd%32) of word
 * fd/32. On return they hold the ready descriptors. timeout_ms < 0 = wait
 * forever. *sigmask (may be NULL): in = extra exec signals to wait for,
 * out = which arrived (WaitSelect semantics). Returns number of ready
 * descriptors, 0 on timeout or signal, -errno. */
int  rt_api_select(struct rt_sockctx *c, int nfds, u32 *rd, u32 *wr, u32 *ex,
                   s32 timeout_ms, u32 *sigmask);
/* ObtainSocket / ReleaseSocket / ReleaseCopyOfSocket (AmiTCP semantics:
 * id == -1 (UNIQUE_ID) allocates a unique id; returns the id or -errno). */
int  rt_api_release(struct rt_sockctx *c, int fd, s32 id, int copy);
int  rt_api_obtain(struct rt_sockctx *c, s32 id, int domain, int type, int protocol);
/* GetSocketEvents: returns fd with pending events (and stores them), or -1
 * if none (not an error). */
int  rt_api_get_events(struct rt_sockctx *c, u32 *events);

#endif
