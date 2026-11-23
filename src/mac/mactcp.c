/* src/mac/mactcp.c - Riptide on the classic Mac OS: MacTCP's TCP and UDP streams (stage 2) and the
 * resolver's lookups (stage 3), on the core's sockets (MacTCP Programmer's Guide, 1993; MacTCP.h).
 *
 * A MacTCP stream is a socket here. Every call arrives through ipp.c in the stack's deferred
 * task; one that cannot finish at once (an open waiting for the other end, a receive waiting for
 * data, a send waiting for room) is kept on its stream and looked at again each time the stack
 * runs, then completed through ipp_result (ioResult, then ioCompletion; errors are logged once per
 * kind). Synchronous callers spin on ioResult, as they do with MacTCP.
 *
 * The notification routines (ASRs) are called as MacTCP calls them: from the stack's context
 * (never from inside another notification), with A5 set to the creating program's A5 (the
 * guide: "A5 is already set up to point to application globals"), the event in D0, the reason in
 * D1, the user data in A2, and the pascal arguments on the stack. TCPDataArrival when data comes
 * and no receive waits for it (once, until a receive completes), TCPClosing when the other end
 * has closed and everything it sent has been delivered, TCPTerminate exactly once per connection,
 * however it ends (with TCPULPClose after both ends closed gracefully), TCPULPTimeout when a
 * stream asked for "report" and its sends have not moved for the ULP timeout.
 *
 * A stream outlives its connections: after TCPTerminate it is back where TCPCreate left it, and a
 * program may open it again (browsers do).
 *
 * The stream's receive buffer (TCPCreate, UDPCreate) is where TCPNoCopyRcv and UDPRead hand data
 * out (the caller gives it back with TCPRcvBfrReturn and UDPBfrReturn); TCPRcv copies into the
 * caller's own buffer. TCPSend completes once its data is in the core's send buffer: the WDS may
 * be reused from then on, which is earlier than MacTCP allows and so asks nothing more of the
 * caller. TCPClose completes at once (MacTCP waits for the FIN's acknowledgement).
 *
 * The lookups (RTMAC_DNS, from the 'dnrp' code of the Riptide DNR file): the hosts table and the
 * core's cache first (rt_resolve_local46), then a small DNS client of its own, asynchronous like
 * everything else here (the core's resolver waits in select, which the Mac's deferred task must
 * never do): A, AAAA and PTR queries over UDP to the configured name servers, each tried twice,
 * with the search domain for names without a dot.
 *
 * IPv6 for programs that know only IPv4 (standin.c): a name with no IPv4 address but an IPv6 one
 * is answered with a stand-in IPv4 address; a stream that opens a stand-in gets a socket of
 * family IPv6 to the real address, and the program sees the stand-in as the remote address and
 * its own IPv4 address as the local one; a UDP stream sends to a stand-in through a second,
 * IPv6 socket on the same port. AddrToName of a stand-in answers the name it was made for.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_bsd.h"
#include "rt_ip.h"
#include "rt_netif.h"
#include "rt_socket.h"
#include "riptide.h"
#include "mac.h"
#include "ipp.h"
#include "standin.h"
#include "rt_ip6.h"
#include "prefs/mac_ipp_private.h"

/* csCodes */
enum { UDPCreate = 20, UDPRead, UDPBfrReturn, UDPWrite, UDPRelease, UDPMaxMTUSize, UDPStatus,
       UDPMultiCreate, UDPMultiSend, UDPMultiRead,
       TCPCreate = 30, TCPPassiveOpen, TCPActiveOpen, TCPSend = 34, TCPNoCopyRcv, TCPRcvBfrReturn, TCPRcv,
       TCPClose, TCPAbort, TCPStatus, TCPExtendedStat, TCPRelease, TCPGlobalInfo };
/* results (the guide's appendix) */
#define connectionClosing     -23005
#define invalidLength         -23006
#define connectionExists      -23007
#define connectionDoesntExist -23008
#define insufficientResources -23009
#define invalidStreamPtr      -23010
#define streamAlreadyOpen     -23011
#define connectionTerminated  -23012
#define invalidBufPtr         -23013
#define invalidRDS            -23014
#define invalidWDS            -23014
#define openFailed            -23015
#define commandTimeout        -23016
#define duplicateSocket       -23017
#define ipDestDeadErr         -23033
#define noNameServer          -23044
#define authNameErr           -23045
#define noAnsErr              -23046
#define dnrErr                -23047
#define outOfMemory           -23048
#define controlErr            -17
/* TCPEventCode */
#define TCPClosing      1
#define TCPULPTimeout   2
#define TCPTerminate    3
#define TCPDataArrival  4
/* TCPTerminationReason */
#define TCPRemoteAbort  2
#define TCPNetworkFailure 3
#define TCPULPTimeoutTerminate 5
#define TCPULPAbort     6
#define TCPULPClose     7
#define UDPDataArrival  1
/* TCP connection states as TCPStatus gives them */
enum { stClosed = 0, stListen = 2, stSynSent = 6, stEstablished = 8, stFinWait1 = 10, stFinWait2 = 12,
       stCloseWait = 14, stClosing = 16 };
/* validity bits of the open, send and close calls */
#define V_ULPVALUE  0x80
#define V_ULPACTION 0x40
#define ULP_DEFAULT 120                 /* seconds: "0 means ... its default value of 2 minutes" */

#define NSTREAMS 32
#define S_MAGIC 0x5254534DUL            /* 'RTSM' */
struct mstream {
	u32 magic;
	int tcp, fd, state;
	u8 *rbuf;                       /* the caller's receive buffer */
	u32 rlen;
	void *notify, *udata, *a5;      /* the ASR, its user data, the creating program's A5 */
	char prog[32];                  /* the creating program's name (Background programs, QoS rules) */
	void *open_pb;                  /* a TCP open waiting */
	u32 open_deadline;
	int passive;
	u32 filt_host;                  /* TCPPassiveOpen: accept only this remote end (0: any) */
	u16 filt_port;
	void *rcv_q, *snd_q;            /* receives and sends waiting, linked by qLink */
	u32 rcv_deadline;
	u32 snd_off;                    /* bytes of the first send's WDS already sent */
	u32 snd_moved;                  /* rt_now_ms when a send last made progress */
	u8 ulp_secs, ulp_report;        /* the connection's ULP timeout and action */
	int conn;                       /* a connection is up (opened, not yet terminated) */
	int peer_fin, told_fin, told_data, closed, nodelay;
	int graceful;                   /* the last connection ended with both ends closing */
	int loaned;                     /* rbuf is out (TCPNoCopyRcv, UDPRead) */
	int v6;                         /* TCP: the socket is IPv6, to the stand-in rhost's address */
	int fd6;                        /* UDP: a second socket, IPv6, for stand-ins (-1 until needed) */
	u32 own_lo, own_hi;             /* the creator's partition (mactcp_reap); 1: not a program's */
	u32 rhost;
	u16 rport, lport;
};
static struct mstream streams[NSTREAMS];

static struct mstream *stream_of(void *pb)
{
	struct mstream *s = (struct mstream *)PB_L(pb, 28);
	if (s < streams || s >= streams + NSTREAMS || s->magic != S_MAGIC)
		return NULL;
	return s;
}

static void asr(struct mstream *s, u32 event, u32 reason)
{
	if (s->notify)
		mac_call_asr5(s->notify, s, event, s->udata, reason, NULL, s->tcp, s->a5);
}

static void q_add(void **q, void *pb)
{
	void **pp = q;
	PB_QLINK(pb) = NULL;
	while (*pp)
		pp = &PB_QLINK(*pp);
	*pp = pb;
}

static void *q_pop(void **q)
{
	void *pb = *q;
	if (pb)
		*q = PB_QLINK(pb);
	return pb;
}

/* what waits on the stream fails: receives with rr, sends and the open with r */
static void fail_all(struct mstream *s, int rr, int r)
{
	void *pb;
	while ((pb = q_pop(&s->rcv_q)) != NULL)
		ipp_result(pb, rr);
	while ((pb = q_pop(&s->snd_q)) != NULL)
		ipp_result(pb, r);
	if (s->open_pb) {
		pb = s->open_pb;
		s->open_pb = NULL;
		ipp_result(pb, r);
	}
}

static int nonblock(struct rt_sockctx *c, int fd)
{
	int one = 1;
	return rt_api_ioctl(c, fd, RT_FIONBIO, &one);
}

/* readable, writable, now */
static int ready(struct rt_sockctx *c, int fd, int wr)
{
	u32 set[2] = { 0, 0 };
	if (fd < 0 || fd >= 64)
		return 0;
	set[fd >> 5] = 1u << (fd & 31);
	return rt_api_select(c, fd + 1, wr ? NULL : set, wr ? set : NULL, NULL, 0, NULL) > 0;
}

static u32 pending(struct rt_sockctx *c, int fd)
{
	int n = 0;
	if (fd < 0 || rt_api_ioctl(c, fd, RT_FIONREAD, &n) < 0 || n < 0)
		return 0;
	return (u32)n;
}

static u32 secs_deadline(u32 secs)
{
	return secs ? rt_now_ms() + secs * 1000u : 0;
}

static int expired(u32 deadline)
{
	return deadline && (s32)(rt_now_ms() - deadline) >= 0;
}

static void fill_addr(struct rt_sockaddr_in *a, u32 host, u16 port)
{
	memset(a, 0, sizeof *a);
	a->sin_len = sizeof *a;
	a->sin_family = RT_AF_INET;
	a->sin_port = port;
	a->sin_addr = host;
}

/* a WDS: { length (word), pointer (long) } ... ending with a zero length */
static u32 wds_total(const u8 *w)
{
	u32 t = 0;
	for (; rt_get_be16(w); w += 6)
		t += rt_get_be16(w);
	return t;
}

static void stream_close(struct rt_sockctx *c, struct mstream *s)
{
	if (s->fd >= 0)
		rt_api_close(c, s->fd);
	s->fd = -1;
	if (s->v6)
		standin_hold(s->rhost, 0);
	s->v6 = 0;
	if (s->fd6 >= 0)
		rt_api_close(c, s->fd6);
	s->fd6 = -1;
}

static void fill_addr6(struct rt_sockaddr_in6 *a, const rt_in6_addr *host, u16 port)
{
	memset(a, 0, sizeof *a);
	a->sin6_len = sizeof *a;
	a->sin6_family = RT_AF_INET6;
	a->sin6_port = port;
	if (host)
		a->sin6_addr = *host;
}

/* one end of a socket (peer 1: the remote one) as MacTCP shows it: the port, and the IPv4 address
   (an IPv6 one, which only a stand-in's socket has, leaves *addr alone: the caller knows it) */
static int sock_end(struct rt_sockctx *c, int fd, int peer, u32 *addr, u16 *port)
{
	union { struct rt_sockaddr_in a4; struct rt_sockaddr_in6 a6; } u;
	u32 l = sizeof u;
	int r = peer ? rt_api_getpeername(c, fd, &u, &l) : rt_api_getsockname(c, fd, &u, &l);
	if (r < 0)
		return r;
	if (u.a4.sin_family == RT_AF_INET) {
		*addr = u.a4.sin_addr;
		*port = u.a4.sin_port;
	} else {
		if (rt_in6_is_v4mapped(&u.a6.sin6_addr))
			*addr = rt_in6_v4(&u.a6.sin6_addr);
		*port = u.a6.sin6_port;
	}
	return 0;
}

/* the ULP timeout and action of an open, send or close: bytes 32, 33 and the validity bits at 34 */
static void take_ulp(struct mstream *s, void *pb)
{
	u8 v = ((u8 *)pb)[34];
	if (v & V_ULPVALUE)
		s->ulp_secs = ((u8 *)pb)[32];
	if (v & V_ULPACTION)
		s->ulp_report = ((u8 *)pb)[33] == 0;
}

static u32 ulp_secs(struct mstream *s)
{
	u32 t = s->ulp_secs ? s->ulp_secs : ULP_DEFAULT;
	return t < 2 ? 2 : t;
}

/* ==================== TCP ==================== */
static int tcp_create(void *pb, struct rt_sockctx *c)
{
	int i;
	(void)c;
	if (!PB_P(pb, 32))
		return invalidBufPtr;
	if (PB_L(pb, 36) < 4096)
		return invalidLength;
	for (i = 0; i < NSTREAMS; i++)
		if (streams[i].magic == S_MAGIC && streams[i].rbuf == PB_P(pb, 32))
			return streamAlreadyOpen;
	for (i = 0; i < NSTREAMS; i++)
		if (streams[i].magic != S_MAGIC) {
			struct mstream *s = &streams[i];
			memset(s, 0, sizeof *s);
			s->magic = S_MAGIC;
			s->tcp = 1;
			s->fd = -1;
			s->fd6 = -1;
			s->rbuf = PB_P(pb, 32);
			s->rlen = PB_L(pb, 36);
			s->notify = PB_P(pb, 40);
			s->udata = PB_P(pb, 44);
			rt_strlcpy(s->prog, ipp_creator_prog(pb), sizeof s->prog);
			s->a5 = ipp_creator_a5(pb);
			PB_L(pb, 28) = (u32)s;
			return 0;
		}
	return insufficientResources;
}

static void tcp_reset_conn(struct mstream *s, void *pb)
{
	s->conn = s->peer_fin = s->told_fin = s->told_data = s->closed = s->loaned = s->nodelay = s->graceful = 0;
	s->snd_off = 0;
	s->ulp_secs = 0;
	s->ulp_report = 0;                              /* the default action is abort */
	take_ulp(s, pb);
}

/* TCPActiveOpen to a stand-in address: a socket of family IPv6 to the real address */
static int tcp_open6(void *pb, struct rt_sockctx *c, struct mstream *s, const rt_in6_addr *a6)
{
	struct rt_sockaddr_in6 a;
	int fd, r;
	mac_prog_name = s->prog;
	fd = rt_api_socket(c, RT_AF_INET6, RT_SOCK_STREAM, RT_IPPROTO_TCP);
	mac_prog_name = NULL;
	if (fd < 0)
		return insufficientResources;
	nonblock(c, fd);
	s->fd = fd;
	tcp_reset_conn(s, pb);
	s->lport = PB_W(pb, 46);
	if (s->lport) {
		int one = 1;
		rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof one);
		fill_addr6(&a, NULL, s->lport);
		if (rt_api_bind(c, fd, &a, sizeof a) < 0) {
			stream_close(c, s);
			return duplicateSocket;
		}
	}
	s->passive = 0;
	s->open_deadline = secs_deadline(ulp_secs(s));
	s->rhost = PB_L(pb, 36);
	s->rport = PB_W(pb, 40);
	s->v6 = 1;
	standin_hold(s->rhost, 1);
	fill_addr6(&a, a6, s->rport);
	r = rt_api_connect(c, fd, &a, sizeof a);
	if (r < 0 && r != -RT_EINPROGRESS && r != -RT_EALREADY) {
		stream_close(c, s);
		return r == -RT_ENETUNREACH || r == -RT_EHOSTUNREACH ? ipDestDeadErr : openFailed;
	}
	s->state = stSynSent;
	s->open_pb = pb;
	return 1;
}

static int tcp_open(void *pb, struct rt_sockctx *c, int passive)
{
	struct mstream *s = stream_of(pb);
	struct rt_sockaddr_in a;
	int fd, r;
	if (!s || !s->tcp)
		return invalidStreamPtr;
	if (s->fd >= 0 || s->open_pb)
		return connectionExists;
	if (!passive && (!PB_L(pb, 36) || !PB_W(pb, 40)))
		return invalidLength;                   /* "remote IP address; cannot be 0" */
	{
		/* a stand-in: the socket is IPv6, to the address it stands for */
		rt_in6_addr a6;
		if (!passive && standin_lookup(PB_L(pb, 36), &a6, NULL, 0))
			return tcp_open6(pb, c, s, &a6);
	}
	mac_prog_name = s->prog;
	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, RT_IPPROTO_TCP);
	mac_prog_name = NULL;
	if (fd < 0)
		return insufficientResources;
	nonblock(c, fd);
	s->fd = fd;
	tcp_reset_conn(s, pb);
	s->lport = PB_W(pb, 46);
	if (s->lport || passive) {
		int one = 1;
		rt_api_setsockopt(c, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof one);
		fill_addr(&a, 0, s->lport);
		if (rt_api_bind(c, fd, &a, sizeof a) < 0) {
			stream_close(c, s);
			return duplicateSocket;
		}
	}
	s->passive = passive;
	if (passive) {
		/* the command timeout (byte 35, 0 = for ever) and the remote end to accept (0 = any) */
		s->open_deadline = secs_deadline(((u8 *)pb)[35] && ((u8 *)pb)[35] < 2 ? 2 : ((u8 *)pb)[35]);
		s->filt_host = PB_L(pb, 36);
		s->filt_port = PB_W(pb, 40);
		if (rt_api_listen(c, fd, 1) < 0) {
			stream_close(c, s);
			return openFailed;
		}
		s->state = stListen;
	} else {
		/* "no command timeout is provided; if the connection cannot be established within the ULP
		   timeout period, the command is completed in error" */
		s->open_deadline = secs_deadline(ulp_secs(s));
		s->rhost = PB_L(pb, 36);
		s->rport = PB_W(pb, 40);
		fill_addr(&a, s->rhost, s->rport);
		r = rt_api_connect(c, fd, &a, sizeof a);
		if (r < 0 && r != -RT_EINPROGRESS && r != -RT_EALREADY) {
			stream_close(c, s);
			return r == -RT_ENETUNREACH || r == -RT_EHOSTUNREACH ? ipDestDeadErr : openFailed;
		}
		s->state = stSynSent;
	}
	s->open_pb = pb;
	return 1;
}

/* the two ends into the open's block (a stand-in's socket: the stand-in, and our IPv4 address) */
static void tcp_fill_ends(struct rt_sockctx *c, struct mstream *s, void *pb)
{
	u32 host = s->rhost, local = mac_nif ? mac_nif->addr : 0;
	u16 port;
	if (sock_end(c, s->fd, 1, &host, &port) == 0) {
		s->rhost = host;
		s->rport = port;
	}
	if (sock_end(c, s->fd, 0, &local, &port) == 0) {
		PB_L(pb, 42) = local;
		PB_W(pb, 46) = port;
		s->lport = port;
	}
	PB_L(pb, 36) = s->rhost;
	PB_W(pb, 40) = s->rport;
}

static void tcp_up(struct rt_sockctx *c, struct mstream *s, void *pb)
{
	s->state = stEstablished;
	s->conn = 1;
	s->snd_moved = rt_now_ms();
	tcp_fill_ends(c, s, pb);
	s->open_pb = NULL;
	ipp_result(pb, 0);
}

static void tcp_poll_open(struct rt_sockctx *c, struct mstream *s)
{
	void *pb = s->open_pb;
	if (s->passive) {
		struct rt_sockaddr_in a;
		u32 l = sizeof a;
		int nfd;
		while ((nfd = rt_api_accept(c, s->fd, &a, &l)) >= 0) {
			if ((s->filt_host && a.sin_addr != s->filt_host) || (s->filt_port && a.sin_port != s->filt_port)) {
				rt_api_close(c, nfd);           /* not the remote end the open asked for */
				l = sizeof a;
				continue;
			}
			rt_api_close(c, s->fd);
			s->fd = nfd;
			nonblock(c, nfd);
			tcp_up(c, s, pb);
			return;
		}
	} else if (ready(c, s->fd, 1)) {
		int err = 0;
		u32 l = sizeof err;
		rt_api_getsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l);
		if (err) {
			s->open_pb = NULL;
			stream_close(c, s);
			s->state = stClosed;
			ipp_result(pb, err == RT_ENETUNREACH || err == RT_EHOSTUNREACH ? ipDestDeadErr : openFailed);
			return;
		}
		tcp_up(c, s, pb);
		return;
	}
	if (expired(s->open_deadline)) {
		s->open_pb = NULL;
		stream_close(c, s);
		s->state = stClosed;
		ipp_result(pb, s->passive ? commandTimeout : openFailed);
	}
}

/* the connection is over: what waits fails, the ASR hears TCPTerminate once, and the stream is
   back where TCPCreate left it (a loaned buffer stays out until it is given back) */
static void tcp_terminate(struct rt_sockctx *c, struct mstream *s, int reason)
{
	if (!s->conn && !s->open_pb)
		return;
	s->conn = 0;
	s->graceful = reason == TCPULPClose;
	s->state = stClosed;
	fail_all(s, reason == TCPULPClose ? connectionClosing : connectionTerminated, connectionTerminated);
	stream_close(c, s);
	asr(s, TCPTerminate, (u32)reason);
}

static int sock_reason(int err)
{
	if (err == RT_ECONNRESET || err == RT_ECONNREFUSED || err == RT_EPIPE)
		return TCPRemoteAbort;
	if (err == RT_ETIMEDOUT)
		return TCPULPTimeoutTerminate;
	return TCPNetworkFailure;
}

static void tcp_poll_rcv(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->rcv_q) != NULL) {
		int nocopy = PB_CSCODE(pb) == TCPNoCopyRcv;
		u32 avail = pending(c, s->fd);
		if (!avail || (nocopy && s->loaned)) {
			if (!avail && (s->peer_fin || (ready(c, s->fd, 0) && !pending(c, s->fd)))) {
				/* readable with nothing to read: the other end has closed, all is delivered */
				s->peer_fin = 1;
				q_pop(&s->rcv_q);
				ipp_result(pb, connectionClosing);
				continue;
			}
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				ipp_result(pb, commandTimeout);
				s->rcv_deadline = s->rcv_q ? secs_deadline(((u8 *)s->rcv_q)[32]) : 0;
				continue;
			}
			return;                         /* nothing yet (or the buffer is still out) */
		}
		if (nocopy) {
			u8 *rds = PB_P(pb, 42);
			u32 n = avail < s->rlen ? avail : s->rlen;
			int r;
			if (n > 65535)
				n = 65535;
			r = rt_api_recv(c, s->fd, s->rbuf, n, RT_MSG_DONTWAIT);
			if (r <= 0) {
				if (r == 0)
					s->peer_fin = 1;
				return;
			}
			/* one piece, and the RDS's terminating zero */
			rt_put_be16(rds, (u16)r);
			memcpy(rds + 2, &s->rbuf, 4);
			rt_put_be16(rds + 6, 0);
			s->loaned = 1;
		} else {
			u8 *buf = PB_P(pb, 36);
			u32 len = PB_W(pb, 40);
			int r = rt_api_recv(c, s->fd, buf, len, RT_MSG_DONTWAIT);
			if (r <= 0) {
				if (r == 0)
					s->peer_fin = 1;
				return;
			}
			PB_W(pb, 40) = (u16)r;
		}
		((u8 *)pb)[34] = 0;     /* markFlag */
		((u8 *)pb)[35] = 0;     /* urgentFlag */
		q_pop(&s->rcv_q);
		s->told_data = 0;
		ipp_result(pb, 0);
		s->rcv_deadline = s->rcv_q ? secs_deadline(((u8 *)s->rcv_q)[32]) : 0;
	}
}

static void tcp_poll_snd(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->snd_q) != NULL) {
		const u8 *w = PB_P(pb, 38);
		u32 skip = s->snd_off;
		/* past what went already, then as much as the socket takes */
		for (; rt_get_be16(w); w += 6) {
			u32 len = rt_get_be16(w);
			const u8 *p;
			int r;
			memcpy(&p, w + 2, 4);
			if (skip >= len) {
				skip -= len;
				continue;
			}
			r = rt_api_send(c, s->fd, p + skip, len - skip, RT_MSG_DONTWAIT);
			if (r < 0) {
				if (r == -RT_EAGAIN || r == -RT_EWOULDBLOCK)
					return;
				tcp_terminate(c, s, sock_reason(-r));
				return;
			}
			if (r > 0)
				s->snd_moved = rt_now_ms();
			s->snd_off += (u32)r;
			if ((u32)r < len - skip)
				return;                         /* the send buffer is full: later */
			skip = 0;
		}
		q_pop(&s->snd_q);
		s->snd_off = 0;
		ipp_result(pb, 0);
	}
}

static int tcp_state(struct mstream *s)
{
	if (s->open_pb)
		return s->passive ? stListen : stSynSent;
	if (!s->conn)
		return stClosed;
	if (s->closed)
		return s->peer_fin ? stClosing : stFinWait1;
	return s->told_fin ? stCloseWait : stEstablished;
}

static void tcp_poll(struct rt_sockctx *c, struct mstream *s)
{
	int err = 0;
	u32 l = sizeof err;
	if (s->fd < 0)
		return;
	if (s->open_pb) {
		tcp_poll_open(c, s);
		return;
	}
	if (!s->conn)
		return;
	if (rt_api_getsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l) == 0 && err) {
		tcp_terminate(c, s, sock_reason(err));
		return;
	}
	tcp_poll_snd(c, s);
	if (!s->conn)
		return;
	tcp_poll_rcv(c, s);
	/* the ULP timeout: sends that have not moved for that long */
	if (s->snd_q && (u32)(rt_now_ms() - s->snd_moved) >= ulp_secs(s) * 1000u) {
		s->snd_moved = rt_now_ms();
		if (!s->ulp_report) {
			tcp_terminate(c, s, TCPULPTimeoutTerminate);
			return;
		}
		asr(s, TCPULPTimeout, 0);
	}
	/* the notifications: data that no receive waits for, the other end's close */
	if (!s->rcv_q && !s->told_data && pending(c, s->fd)) {
		s->told_data = 1;
		asr(s, TCPDataArrival, 0);
	}
	if (!s->told_fin && !pending(c, s->fd) && (s->peer_fin || ready(c, s->fd, 0))) {
		s->peer_fin = 1;
		s->told_fin = 1;
		asr(s, TCPClosing, 0);
	}
	/* both ends have closed and everything is delivered: the connection is over, gracefully */
	if (s->closed && s->told_fin && !s->snd_q)
		tcp_terminate(c, s, TCPULPClose);
	else
		s->state = tcp_state(s);
}

/* TCPStatus (the guide's table and TCPStatusPB: a long after the two ULP bytes, so remoteHost
   is at 38 and connectionState at 52) */
static int tcp_status(void *pb, struct rt_sockctx *c)
{
	static u8 connstats[96];        /* TCPConnectionStats, zero */
	struct mstream *s = stream_of(pb);
	u32 unread, local = mac_nif ? mac_nif->addr : 0;
	u16 port;
	if (!s || !s->tcp)
		return invalidStreamPtr;
	memset((u8 *)pb + 32, 0, 66);                           /* up to userDataPtr at 98 */
	((u8 *)pb)[32] = (u8)ulp_secs(s);
	((u8 *)pb)[33] = s->ulp_report ? 0 : 1;
	PB_L(pb, 38) = s->rhost;
	PB_W(pb, 42) = s->rport;
	if (s->fd >= 0 && sock_end(c, s->fd, 0, &local, &port) == 0) {
		PB_L(pb, 44) = local;
		PB_W(pb, 48) = port;
	} else {
		PB_L(pb, 44) = mac_nif ? mac_nif->addr : 0;
		PB_W(pb, 48) = s->lport;
	}
	((u8 *)pb)[52] = (u8)tcp_state(s);
	unread = pending(c, s->fd);
	PB_W(pb, 54) = 8192;                                    /* sendWindow */
	PB_W(pb, 56) = (u16)(s->rlen > 65535 ? 65535 : s->rlen); /* rcvWindow */
	PB_W(pb, 60) = (u16)(unread > 65535 ? 65535 : unread);  /* amtUnreadData */
	PB_L(pb, 82) = 100;                                     /* srtt, ms */
	PB_L(pb, 86) = 100;                                     /* lastRTT */
	PB_L(pb, 90) = 1460;                                    /* sendMaxSegSize */
	PB_P(pb, 94) = connstats;
	return s->conn || s->open_pb ? 0 : connectionDoesntExist;
}

static int tcp_global(void *pb)
{
	static u32 params[6], stats[13];
	static struct mstream *table[NSTREAMS];
	static const u8 rto[] = "\x0bVan Jacobson";
	int i;
	params[0] = (u32)rto;
	params[1] = 200;                /* minimum retransmission timeout, ms */
	params[2] = 60000;              /* maximum */
	params[3] = 1460;               /* MSS we can receive */
	params[4] = NSTREAMS;
	params[5] = 65535;              /* largest receive window */
	for (i = 0; i < NSTREAMS; i++)
		table[i] = streams[i].magic == S_MAGIC && streams[i].tcp ? &streams[i] : NULL;
	PB_P(pb, 32) = params;
	PB_P(pb, 36) = stats;
	PB_P(pb, 40) = table;
	PB_W(pb, 48) = NSTREAMS;
	return 0;
}

static void stream_free(struct rt_sockctx *c, struct mstream *s, int r)
{
	fail_all(s, r, r);
	stream_close(c, s);
	s->magic = 0;
}

/* ==================== UDP ==================== */
static int udp_create(void *pb, struct rt_sockctx *c)
{
	struct rt_sockaddr_in a;
	u32 l = sizeof a;
	int i, fd;
	if (!PB_P(pb, 32))
		return invalidBufPtr;
	if (PB_L(pb, 36) < 2048)
		return invalidLength;
	for (i = 0; i < NSTREAMS && streams[i].magic == S_MAGIC; i++)
		;
	if (i == NSTREAMS)
		return insufficientResources;
	mac_prog_name = ipp_creator_prog(pb);
	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, RT_IPPROTO_UDP);
	mac_prog_name = NULL;
	if (fd < 0)
		return insufficientResources;
	nonblock(c, fd);
	fill_addr(&a, 0, PB_W(pb, 44));
	if (rt_api_bind(c, fd, &a, sizeof a) < 0) {
		rt_api_close(c, fd);
		return duplicateSocket;
	}
	l = sizeof a;
	rt_api_getsockname(c, fd, &a, &l);
	{
		struct mstream *s = &streams[i];
		memset(s, 0, sizeof *s);
		s->magic = S_MAGIC;
		s->fd = fd;
		s->fd6 = -1;
		s->rbuf = PB_P(pb, 32);
		s->rlen = PB_L(pb, 36);
		s->notify = PB_P(pb, 40);
		s->udata = PB_P(pb, 46);
		rt_strlcpy(s->prog, ipp_creator_prog(pb), sizeof s->prog);
		s->a5 = ipp_creator_a5(pb);
		s->lport = a.sin_port;
		PB_W(pb, 44) = a.sin_port;
		PB_L(pb, 28) = (u32)s;
	}
	return 0;
}

/* a datagram from either socket into rbuf; the sender's IPv4 address (a stand-in for an IPv6 one) */
static int udp_recv(struct rt_sockctx *c, struct mstream *s, u32 *host, u16 *port)
{
	u32 len = s->rlen > 65535 ? 65535 : s->rlen;
	struct rt_sockaddr_in from;
	struct rt_sockaddr_in6 from6;
	u32 fl = sizeof from;
	int r = rt_api_recvfrom(c, s->fd, s->rbuf, len, RT_MSG_DONTWAIT, &from, &fl);
	if (r >= 0) {
		*host = from.sin_addr;
		*port = from.sin_port;
		return r;
	}
	if (s->fd6 < 0)
		return r;
	fl = sizeof from6;
	r = rt_api_recvfrom(c, s->fd6, s->rbuf, len, RT_MSG_DONTWAIT, &from6, &fl);
	if (r >= 0) {
		*host = standin_for(&from6.sin6_addr, NULL);
		*port = from6.sin6_port;
	}
	return r;
}

static void udp_poll(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->rcv_q) != NULL) {
		u32 host = 0;
		u16 port = 0;
		int r;
		if (s->loaned)
			return;
		r = udp_recv(c, s, &host, &port);
		if (r < 0) {
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				ipp_result(pb, commandTimeout);
				s->rcv_deadline = s->rcv_q ? secs_deadline(PB_W(s->rcv_q, 32)) : 0;
				continue;
			}
			return;
		}
		PB_L(pb, 34) = host;
		PB_W(pb, 38) = port;
		PB_P(pb, 40) = s->rbuf;
		PB_W(pb, 44) = (u16)r;
		PB_L(pb, 52) = mac_nif ? mac_nif->addr : 0;     /* destHost */
		PB_W(pb, 56) = s->lport;
		s->loaned = 1;
		q_pop(&s->rcv_q);
		s->told_data = 0;
		ipp_result(pb, 0);
		s->rcv_deadline = s->rcv_q ? secs_deadline(PB_W(s->rcv_q, 32)) : 0;
	}
	if (!s->told_data && !s->loaned && (ready(c, s->fd, 0) || ready(c, s->fd6, 0))) {
		s->told_data = 1;
		asr(s, UDPDataArrival, 0);
	}
}

/* the IPv6 socket of a UDP stream, on the same port, made when it first sends to a stand-in */
static int udp_fd6(struct rt_sockctx *c, struct mstream *s)
{
	struct rt_sockaddr_in6 a;
	int one = 1;
	if (s->fd6 >= 0)
		return s->fd6;
	mac_prog_name = s->prog;
	s->fd6 = rt_api_socket(c, RT_AF_INET6, RT_SOCK_DGRAM, RT_IPPROTO_UDP);
	mac_prog_name = NULL;
	if (s->fd6 < 0)
		return -1;
	nonblock(c, s->fd6);
	rt_api_setsockopt(c, s->fd6, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof one);
	fill_addr6(&a, NULL, s->lport);
	if (rt_api_bind(c, s->fd6, &a, sizeof a) < 0) {
		rt_api_close(c, s->fd6);
		s->fd6 = -1;
	}
	return s->fd6;
}

static int udp_write(void *pb, struct rt_sockctx *c)
{
	struct mstream *s = stream_of(pb);
	struct rt_sockaddr_in to;
	u8 buf[1600];
	const u8 *w = PB_P(pb, 40);
	u32 n = 0;
	int r;
	if (!s || s->tcp)
		return invalidStreamPtr;
	if (!w)
		return invalidWDS;
	for (; rt_get_be16(w); w += 6) {
		u32 len = rt_get_be16(w);
		const u8 *p;
		memcpy(&p, w + 2, 4);
		if (n + len > sizeof buf)
			return invalidLength;
		memcpy(buf + n, p, len);
		n += len;
	}
	{
		rt_in6_addr a6;
		if (standin_lookup(PB_L(pb, 34), &a6, NULL, 0)) {
			struct rt_sockaddr_in6 to6;
			int fd6 = udp_fd6(c, s);
			if (fd6 < 0)
				return insufficientResources;
			fill_addr6(&to6, &a6, PB_W(pb, 38));
			r = rt_api_sendto(c, fd6, buf, n, RT_MSG_DONTWAIT, &to6, sizeof to6);
			return r < 0 ? ipDestDeadErr : 0;
		}
	}
	fill_addr(&to, PB_L(pb, 34), PB_W(pb, 38));
	r = rt_api_sendto(c, s->fd, buf, n, RT_MSG_DONTWAIT, &to, sizeof to);
	return r < 0 ? ipDestDeadErr : 0;
}

/* ==================== the resolver's lookups (RTMAC_DNS) ==================== */
#define NDNS 8
struct dnsq {
	void *pb;
	u32 own_lo, own_hi;             /* the partition of the parameter block (mactcp_reap) */
	int fd, server, tries, ptr;
	u16 id, qtype;                  /* A (1), AAAA (28) or PTR (12) */
	int other_tried;                /* the other of A and AAAA asked for already */
	u32 deadline;
	char name[256];
	int searched;
};
static struct dnsq dnsq[NDNS];
static u16 dns_id = 0x5200;

static int numeric(const char *s, u32 *a)
{
	u32 v = 0, part = 0;
	int dots = 0, digits = 0;
	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			part = part * 10 + (u32)(*s - '0');
			if (part > 255 || ++digits > 3)
				return 0;
		} else if (*s == '.' && digits) {
			v = v << 8 | part;
			part = 0;
			digits = 0;
			dots++;
		} else {
			return 0;
		}
	}
	if (dots != 3 || !digits)
		return 0;
	*a = v << 8 | part;
	return 1;
}

static int dns_send(struct rt_sockctx *c, struct dnsq *q)
{
	u32 servers[4];
	int n = rt_dns_get_servers(servers, 4), len = 12, k;
	u8 pkt[300];
	const char *nm = q->name;
	struct rt_sockaddr_in to;
	if (n <= 0)
		return noNameServer;
	memset(pkt, 0, 12);
	rt_put_be16(pkt, q->id);
	pkt[2] = 0x01;                                  /* recursion desired */
	rt_put_be16(pkt + 4, 1);                        /* one question */
	/* the name in labels */
	while (*nm) {
		const char *dot = nm;
		while (*dot && *dot != '.')
			dot++;
		k = (int)(dot - nm);
		if (k < 1 || k > 63 || len + k + 6 > (int)sizeof pkt)
			return authNameErr;
		pkt[len++] = (u8)k;
		memcpy(pkt + len, nm, (u32)k);
		len += k;
		nm = *dot ? dot + 1 : dot;
	}
	pkt[len++] = 0;
	rt_put_be16(pkt + len, q->qtype);               /* A, AAAA or PTR */
	rt_put_be16(pkt + len + 2, 1);                  /* IN */
	len += 4;
	fill_addr(&to, servers[q->server % n], rt_htons(53));
	if (rt_api_sendto(c, q->fd, pkt, (u32)len, RT_MSG_DONTWAIT, &to, sizeof to) < 0)
		return noNameServer;
	q->deadline = rt_now_ms() + 2500;
	return 1;
}

/* a name of the answer at p into out (dotted), compressed names followed; the byte after it */
static int dns_name(const u8 *m, int mlen, int p, char *out, int olen)
{
	int o = 0, jumped = 0, end = -1, guard = 0;
	while (p < mlen && guard++ < 64) {
		int k = m[p];
		if (!k) {
			if (end < 0)
				end = p + 1;
			break;
		}
		if ((k & 0xC0) == 0xC0) {
			if (p + 1 >= mlen)
				return -1;
			if (end < 0)
				end = p + 2;
			p = ((k & 0x3F) << 8) | m[p + 1];
			jumped = 1;
			continue;
		}
		if (p + 1 + k > mlen)
			return -1;
		if (out) {
			if (o && o < olen - 1)
				out[o++] = '.';
			for (int i = 0; i < k && o < olen - 1; i++)
				out[o++] = (char)m[p + 1 + i];
		}
		p += 1 + k;
	}
	if (out && olen)
		out[o < olen ? o : olen - 1] = 0;
	(void)jumped;
	return end;
}

static void dns_done(struct rt_sockctx *c, struct dnsq *q, int r)
{
	if (q->pb)
		ipp_result(q->pb, r);
	q->pb = NULL;
	if (q->fd >= 0)
		rt_api_close(c, q->fd);
	q->fd = -1;
}

/* a name's answer as an IPv6 address: its stand-in (standin.c) */
static int dns_answer6(struct rtmac_dns_pb *d, const rt_in6_addr *a6, const char *name)
{
	u32 v4 = standin_for(a6, name);
	if (!v4)
		return 0;
	d->addrs[0] = v4;
	d->count = 1;
	rt_strlcpy(d->cname, name, sizeof d->cname);
	return 1;
}

/* the other of A and AAAA, once: for a name with no record of the kind asked for first */
static int dns_other(struct rt_sockctx *c, struct dnsq *q)
{
	if (q->ptr || q->other_tried)
		return 0;
	q->other_tried = 1;
	q->qtype = q->qtype == 1 ? 28 : 1;
	q->id = ++dns_id;
	q->tries = 0;
	return dns_send(c, q) == 1;
}

static void dns_poll(struct rt_sockctx *c)
{
	for (int i = 0; i < NDNS; i++) {
		struct dnsq *q = &dnsq[i];
		u8 m[600];
		int n;
		if (!q->pb)
			continue;
		while ((n = rt_api_recvfrom(c, q->fd, m, sizeof m, RT_MSG_DONTWAIT, NULL, NULL)) >= 12) {
			int qd = rt_get_be16(m + 4), an = rt_get_be16(m + 6), p = 12, rcode = m[3] & 15, got = 0;
			struct rtmac_dns_pb *d = q->pb;
			if (rt_get_be16(m) != q->id || !(m[2] & 0x80))
				continue;
			if (rcode == 3) {                       /* no such name: with the search domain, once */
				const char *dom = rt_dns_search_list();
				if (!q->ptr && !q->searched && dom && *dom && !strchr(q->name, '.')) {
					q->other_tried = 0;
					char *e = q->name + strlen(q->name);
					u32 k = 0;
					*e++ = '.';
					while (dom[k] && dom[k] != ' ' && e < q->name + sizeof q->name - 1)
						*e++ = dom[k++];
					*e = 0;
					q->searched = 1;
					q->id = ++dns_id;
					q->tries = 0;
					if (dns_send(c, q) == 1)
						continue;
				}
				dns_done(c, q, authNameErr);
				break;
			}
			if (rcode) {
				dns_done(c, q, noAnsErr);
				break;
			}
			for (int k = 0; k < qd && p > 0; k++) {
				p = dns_name(m, n, p, NULL, 0);
				p = p > 0 ? p + 4 : -1;
			}
			for (int k = 0; k < an && p > 0 && p + 10 <= n; k++) {
				char nm[256];
				int e = dns_name(m, n, p, nm, sizeof nm), type, rdlen;
				if (e < 0 || e + 10 > n)
					break;
				type = rt_get_be16(m + e);
				rdlen = rt_get_be16(m + e + 8);
				if (e + 10 + rdlen > n)
					break;
				if (q->qtype == 1 && type == 1 && rdlen == 4 && got < 4) {
					memcpy(&d->addrs[got], m + e + 10, 4);
					if (!got)
						rt_strlcpy(d->cname, nm, sizeof d->cname);
					got++;
				} else if (q->qtype == 28 && type == 28 && rdlen == 16 && !got) {
					rt_in6_addr a6;
					memcpy(&a6, m + e + 10, 16);
					got = dns_answer6(d, &a6, q->name);
				} else if (q->ptr && type == 12) {
					dns_name(m, n, e + 10, d->cname, sizeof d->cname);
					got = 1;
				}
				p = e + 10 + rdlen;
			}
			d->count = (u32)got;
			if (!got && dns_other(c, q))
				continue;                       /* no A: perhaps an AAAA (or the other way) */
			dns_done(c, q, got ? 0 : noAnsErr);
			break;
		}
		if (q->pb && (s32)(rt_now_ms() - q->deadline) >= 0) {
			if (++q->tries >= 4) {
				dns_done(c, q, noAnsErr);
				continue;
			}
			q->server++;
			q->id = ++dns_id;
			if (dns_send(c, q) != 1)
				dns_done(c, q, noNameServer);
		}
	}
}

static int dns_call(void *pb, struct rt_sockctx *c)
{
	struct rtmac_dns_pb *d = pb;
	struct dnsq *q = NULL;
	rt_in6_addr a6;
	u32 a;
	int r;
	d->count = 0;
	d->cname[0] = 0;
	if (!d->ptr && numeric(d->name, &a)) {
		d->addrs[0] = a;
		d->count = 1;
		rt_strlcpy(d->cname, d->name, sizeof d->cname);
		return 0;
	}
	if (d->ptr && standin_lookup(d->addrs[0], &a6, d->cname, sizeof d->cname)) {
		/* a stand-in: the name it was made for (or its IPv6 address, for one seen on its own) */
		if (!d->cname[0])
			rt_inet6_ntop(&a6, d->cname, sizeof d->cname);
		d->count = 1;
		return 0;
	}
	if (!d->ptr) {
		/* the hosts table and the cache: IPv4 addresses, else an IPv6 one as a stand-in
		   (with `mactcp ipv6 prefer`, the IPv6 one first) */
		u32 a4[4];
		int n4 = rt_resolve_local46(d->name, 0, a4, NULL, 4);
		int n6 = rt_resolve_local46(d->name, 1, NULL, &a6, 1);
		if (n4 && !(standin_prefer6 && n6)) {
			for (int i = 0; i < n4; i++)
				d->addrs[i] = a4[i];
			d->count = (u32)n4;
			rt_strlcpy(d->cname, d->name, sizeof d->cname);
			return 0;
		}
		if (n6 && dns_answer6(d, &a6, d->name))
			return 0;
	}
	for (int i = 0; i < NDNS; i++)
		if (!dnsq[i].pb) {
			q = &dnsq[i];
			break;
		}
	if (!q)
		return outOfMemory;
	memset(q, 0, sizeof *q);
	q->fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, RT_IPPROTO_UDP);
	if (q->fd < 0)
		return outOfMemory;
	nonblock(c, q->fd);
	if (d->ptr) {
		u32 x = d->addrs[0];
		rt_snprintf(q->name, sizeof q->name, "%u.%u.%u.%u.in-addr.arpa", (unsigned)(x & 255),
		            (unsigned)((x >> 8) & 255), (unsigned)((x >> 16) & 255), (unsigned)(x >> 24));
		q->ptr = 1;
		q->qtype = 12;
	} else {
		rt_strlcpy(q->name, d->name, sizeof q->name);
		q->qtype = standin_prefer6 ? 28 : 1;
		/* a trailing dot: the name as it is */
		if (q->name[0] && q->name[strlen(q->name) - 1] == '.') {
			q->name[strlen(q->name) - 1] = 0;
			q->searched = 1;
		}
	}
	q->id = ++dns_id;
	q->pb = pb;
	r = dns_send(c, q);
	if (r != 1) {
		q->pb = NULL;
		rt_api_close(c, q->fd);
		q->fd = -1;
		return r;
	}
	return 1;
}

/* RTMAC_DNSCANCEL: the lookups whose parameter blocks lie in [lo, hi) are withdrawn, never completed:
   the 'dnrp' code's CloseResolver, before Apple's DNR glue disposes of that code */
static int dns_cancel(struct rt_sockctx *c, u32 lo, u32 hi)
{
	for (int i = 0; i < NDNS; i++) {
		struct dnsq *q = &dnsq[i];
		if (q->pb && (u32)q->pb >= lo && (u32)q->pb < hi) {
			q->pb = NULL;
			if (q->fd >= 0)
				rt_api_close(c, q->fd);
			q->fd = -1;
		}
	}
	return 0;
}

/* ==================== programs that have quit ==================== */
/* where an address is: the index of the live partition holding it, or -1 */
static int part_of(u32 a, const u32 *lo, const u32 *hi, int n)
{
	for (int k = 0; k < n; k++)
		if (a >= lo[k] && a < hi[k])
			return k;
	return -1;
}

/* 1: the owner recorded is gone. The first look records the owner: the partition holding addr, or
   "not a program's" (1), which is never released. */
static int owner_gone(u32 *own_lo, u32 *own_hi, u32 addr, const u32 *lo, const u32 *hi, int n)
{
	int k;
	if (!*own_lo) {
		k = part_of(addr, lo, hi, n);
		*own_lo = k < 0 ? 1 : lo[k];
		*own_hi = k < 0 ? 1 : hi[k];
		return 0;
	}
	if (*own_lo == 1)
		return 0;
	for (k = 0; k < n; k++)
		if (lo[k] == *own_lo && hi[k] == *own_hi)
			return 0;
	return 1;
}

void mactcp_reap(struct rt_sockctx *c, const u32 *lo, const u32 *hi, int n)
{
	for (int i = 0; i < NSTREAMS; i++) {
		struct mstream *s = &streams[i];
		if (s->magic != S_MAGIC || !owner_gone(&s->own_lo, &s->own_hi, (u32)s->a5, lo, hi, n))
			continue;
		rt_log(RT_LOG_WARN, ".IPP: %s stream %08lx of a program that quit without releasing it: released",
		       s->tcp ? "TCP" : "UDP", (unsigned long)s);
		/* nothing of the program's is touched again: its calls are dropped, not completed */
		s->notify = NULL;
		s->rcv_q = s->snd_q = s->open_pb = NULL;
		if (s->tcp && s->conn && s->fd >= 0) {
			struct { s32 onoff, secs; } lg = { 1, 0 };
			rt_api_setsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof lg);
		}
		stream_close(c, s);
		s->magic = 0;
	}
	for (int i = 0; i < NDNS; i++) {
		struct dnsq *q = &dnsq[i];
		if (!q->pb || !owner_gone(&q->own_lo, &q->own_hi, (u32)q->pb, lo, hi, n))
			continue;
		rt_log(RT_LOG_WARN, ".IPP: a lookup of a program that has quit: dropped");
		q->pb = NULL;
		if (q->fd >= 0)
			rt_api_close(c, q->fd);
		q->fd = -1;
	}
}

/* ==================== dispatch ==================== */
/* the stream of a TCP call that needs a connection: 0 if there is one, else the result */
static int need_conn(void *pb, struct mstream **sp, int closing)
{
	struct mstream *s = stream_of(pb);
	*sp = s;
	if (!s || !s->tcp)
		return invalidStreamPtr;
	if (!s->conn)
		return s->graceful && closing ? connectionClosing : connectionDoesntExist;
	return 0;
}

int mactcp_call(void *pb, struct rt_sockctx *c)
{
	struct mstream *s;
	int r;
	switch (PB_CSCODE(pb)) {
	case TCPCreate:
		return tcp_create(pb, c);
	case TCPPassiveOpen:
		return tcp_open(pb, c, 1);
	case TCPActiveOpen:
		return tcp_open(pb, c, 0);
	case TCPSend: {
		u32 n;
		if ((r = need_conn(pb, &s, 0)) != 0)
			return r;
		if (s->closed)
			return connectionClosing;       /* "a TCPClose command was already issued" */
		if (!PB_P(pb, 38))
			return invalidWDS;
		n = wds_total(PB_P(pb, 38));
		if (n == 0 || n > 65535)
			return invalidLength;
		take_ulp(s, pb);
		if (((u8 *)pb)[35] && !s->nodelay) {    /* push: no waiting for more (Nagle off) */
			int one = 1;
			rt_api_setsockopt(c, s->fd, RT_IPPROTO_TCP, RT_TCP_NODELAY, &one, sizeof one);
			s->nodelay = 1;
		}
		if (!s->snd_q)
			s->snd_moved = rt_now_ms();
		q_add(&s->snd_q, pb);
		tcp_poll_snd(c, s);
		return 1;
	}
	case TCPRcv:
	case TCPNoCopyRcv:
		if ((r = need_conn(pb, &s, 1)) != 0)
			return r;
		if (PB_CSCODE(pb) == TCPRcv) {
			if (!PB_P(pb, 36))
				return invalidBufPtr;
			if (!PB_W(pb, 40))
				return invalidLength;
		} else {
			if (!PB_P(pb, 42))
				return invalidBufPtr;
			if (!PB_W(pb, 46))
				return invalidLength;   /* "the RDS has 0 entries"; one is enough */
		}
		if (!s->rcv_q)
			s->rcv_deadline = secs_deadline(((u8 *)pb)[32]);
		q_add(&s->rcv_q, pb);
		tcp_poll_rcv(c, s);
		return 1;
	case TCPRcvBfrReturn:
		if (!(s = stream_of(pb)) || !s->tcp)
			return invalidStreamPtr;
		if (!PB_P(pb, 42))
			return invalidBufPtr;
		if (!s->loaned)
			return invalidRDS;              /* "returned more than once" */
		s->loaned = 0;
		return 0;
	case TCPClose:
		if ((r = need_conn(pb, &s, 1)) != 0)
			return r;
		if (s->closed)
			return connectionClosing;
		take_ulp(s, pb);
		tcp_poll_snd(c, s);                     /* what is queued goes before the FIN */
		if (s->snd_q) {
			/* sends still waiting for room: they go first; the FIN follows them (tcp_poll) */
			s->closed = 2;
			return 0;
		}
		rt_api_shutdown(c, s->fd, 1);           /* SHUT_WR: our FIN */
		s->closed = 1;
		s->state = tcp_state(s);
		return 0;
	case TCPAbort:
		if (!(s = stream_of(pb)) || !s->tcp)
			return invalidStreamPtr;
		if (!s->conn && !s->open_pb)
			return connectionDoesntExist;
		if (s->fd >= 0) {
			struct { s32 onoff, secs; } lg = { 1, 0 };
			rt_api_setsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof lg);    /* a reset */
		}
		tcp_terminate(c, s, TCPULPAbort);
		s->graceful = 0;
		return 0;
	case TCPStatus:
		return tcp_status(pb, c);
	case TCPRelease:
		if (!(s = stream_of(pb)) || !s->tcp)
			return invalidStreamPtr;
		if (s->conn && s->fd >= 0) {            /* an open connection is aborted */
			struct { s32 onoff, secs; } lg = { 1, 0 };
			rt_api_setsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof lg);
		}
		PB_P(pb, 32) = s->rbuf;
		PB_L(pb, 36) = s->rlen;
		stream_free(c, s, connectionTerminated);
		return 0;
	case TCPGlobalInfo:
		return tcp_global(pb);
	case UDPCreate:
		return udp_create(pb, c);
	case UDPRead:
		if (!(s = stream_of(pb)) || s->tcp)
			return invalidStreamPtr;
		if (!s->rcv_q)
			s->rcv_deadline = secs_deadline(PB_W(pb, 32));
		q_add(&s->rcv_q, pb);
		udp_poll(c, s);
		return 1;
	case UDPBfrReturn:
		if (!(s = stream_of(pb)) || s->tcp)
			return invalidStreamPtr;
		if (!PB_P(pb, 40))
			return invalidBufPtr;
		s->loaned = 0;
		return 0;
	case UDPWrite:
		return udp_write(pb, c);
	case UDPRelease:
		if (!(s = stream_of(pb)) || s->tcp)
			return invalidStreamPtr;
		PB_P(pb, 32) = s->rbuf;
		PB_L(pb, 36) = s->rlen;
		stream_free(c, s, connectionTerminated);
		return 0;
	case UDPMaxMTUSize:
		PB_W(pb, 32) = (u16)((mac_nif ? mac_nif->mtu : 1500) - 28);
		return 0;
	case RTMAC_DNS:
		return dns_call(pb, c);
	case RTMAC_DNSCANCEL:
		return dns_cancel(c, PB_L(pb, 28), PB_L(pb, 32));
	}
	return controlErr;
}

void mactcp_poll(struct rt_sockctx *c)
{
	for (int i = 0; i < NSTREAMS; i++) {
		struct mstream *s = &streams[i];
		if (s->magic != S_MAGIC)
			continue;
		if (!s->tcp) {
			udp_poll(c, s);
			continue;
		}
		tcp_poll(c, s);
		/* a TCPClose that waited for the sends before it: the FIN, once they are all out */
		if (s->magic == S_MAGIC && s->conn && s->closed == 2 && !s->snd_q) {
			rt_api_shutdown(c, s->fd, 1);
			s->closed = 1;
		}
	}
	dns_poll(c);
}
