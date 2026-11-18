/* src/mac/mactcp.c - Riptide on the classic Mac OS: MacTCP's TCP and UDP streams (stage 2) and the
 * resolver's lookups (stage 3), on the core's sockets (MacTCP Programmer's Guide; MacTCP.h).
 *
 * A MacTCP stream is a socket here. Every call arrives through ipp.c in the stack's deferred
 * task; one that cannot finish at once (an open waiting for the other end, a receive waiting for
 * data, a send waiting for room) is kept on its stream and looked at again each time the stack
 * runs, then completed with mac_complete (ioResult, then ioCompletion). Synchronous callers spin
 * on ioResult, as they do with MacTCP. The notification routines (ASRs) are called as MacTCP
 * calls them, from the stack's context: TCPDataArrival when data comes and no receive waits for
 * it, TCPClosing when the other end has closed, TCPTerminate when the connection is gone.
 *
 * The stream's receive buffer (TCPCreate, UDPCreate) is where TCPNoCopyRcv and UDPRead hand data
 * out (the caller gives it back with TCPRcvBfrReturn and UDPBfrReturn); TCPRcv copies into the
 * caller's own buffer. TCPSend completes once its data is in the core's send buffer: the WDS may
 * be reused from then on, which is earlier than MacTCP allows and so asks nothing more of the
 * caller.
 *
 * The lookups (RTMAC_DNS, from the 'dnrp' code of the Riptide DNR file): a small DNS client of
 * its own, asynchronous like everything else here (the core's resolver waits in select, which the
 * Mac's deferred task must never do): A and PTR queries over UDP to the configured name servers,
 * each tried twice, with the search domain for names without a dot.
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
#include "prefs/mac_ipp_private.h"

/* csCodes */
enum { UDPCreate = 20, UDPRead, UDPBfrReturn, UDPWrite, UDPRelease, UDPMaxMTUSize, UDPStatus,
       UDPMultiCreate, UDPMultiSend, UDPMultiRead,
       TCPCreate = 30, TCPPassiveOpen, TCPActiveOpen, TCPSend = 34, TCPNoCopyRcv, TCPRcvBfrReturn, TCPRcv,
       TCPClose, TCPAbort, TCPStatus, TCPExtendedStat, TCPRelease, TCPGlobalInfo };
/* results */
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
/* notifications */
#define TCPClosing      1
#define TCPTerminate    3
#define TCPDataArrival  4
#define TCPRemoteAbort  2
#define TCPNetworkFailure 3
#define TCPULPAbort     6
#define UDPDataArrival  1
/* TCP connection states as TCPStatus gives them */
enum { stClosed = 0, stListen = 2, stSynSent = 6, stEstablished = 8, stFinWait1 = 10, stCloseWait = 14 };

#define NSTREAMS 32
#define S_MAGIC 0x5254534DUL            /* 'RTSM' */
struct mstream {
	u32 magic;
	int tcp, fd, state;
	u8 *rbuf;                       /* the caller's receive buffer */
	u32 rlen;
	void *notify, *udata;
	void *open_pb;                  /* a TCP open waiting */
	u32 open_deadline;
	int passive;
	void *rcv_q, *snd_q;            /* receives and sends waiting, linked by qLink */
	u32 rcv_deadline;
	u32 snd_off;                    /* bytes of the first send's WDS already sent */
	int peer_fin, told_fin, told_data, dead, reason;
	int loaned;                     /* rbuf is out (TCPNoCopyRcv, UDPRead) */
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
		mac_call_asr(s->notify, s, event, s->udata, reason, NULL, s->tcp);
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

static void fail_all(struct mstream *s, int r)
{
	void *pb;
	while ((pb = q_pop(&s->rcv_q)) != NULL)
		mac_complete(pb, r);
	while ((pb = q_pop(&s->snd_q)) != NULL)
		mac_complete(pb, r);
	if (s->open_pb) {
		mac_complete(s->open_pb, r);
		s->open_pb = NULL;
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
	if (rt_api_ioctl(c, fd, RT_FIONREAD, &n) < 0 || n < 0)
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
}

/* ==================== TCP ==================== */
static int tcp_create(void *pb, struct rt_sockctx *c)
{
	int i;
	(void)c;
	if (!PB_P(pb, 32) || PB_L(pb, 36) < 4096)
		return PB_P(pb, 32) ? invalidLength : invalidBufPtr;
	for (i = 0; i < NSTREAMS; i++)
		if (streams[i].magic != S_MAGIC) {
			struct mstream *s = &streams[i];
			memset(s, 0, sizeof *s);
			s->magic = S_MAGIC;
			s->tcp = 1;
			s->fd = -1;
			s->rbuf = PB_P(pb, 32);
			s->rlen = PB_L(pb, 36);
			s->notify = PB_P(pb, 40);
			s->udata = PB_P(pb, 44);
			PB_L(pb, 28) = (u32)s;
			return 0;
		}
	return insufficientResources;
}

static int tcp_open(void *pb, struct rt_sockctx *c, int passive)
{
	struct mstream *s = stream_of(pb);
	struct rt_sockaddr_in a;
	int fd, r;
	if (!s)
		return invalidStreamPtr;
	if (s->fd >= 0 || s->open_pb)
		return connectionExists;
	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_STREAM, RT_IPPROTO_TCP);
	if (fd < 0)
		return insufficientResources;
	nonblock(c, fd);
	s->fd = fd;
	s->peer_fin = s->told_fin = s->told_data = s->dead = s->loaned = 0;
	s->snd_off = 0;
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
	s->open_deadline = secs_deadline(((u8 *)pb)[35]);       /* commandTimeoutValue */
	if (passive) {
		if (rt_api_listen(c, fd, 1) < 0) {
			stream_close(c, s);
			return openFailed;
		}
		s->state = stListen;
	} else {
		s->rhost = PB_L(pb, 36);
		s->rport = PB_W(pb, 40);
		fill_addr(&a, s->rhost, s->rport);
		r = rt_api_connect(c, fd, &a, sizeof a);
		if (r < 0 && r != -RT_EINPROGRESS && r != -RT_EALREADY) {
			stream_close(c, s);
			return openFailed;
		}
		s->state = stSynSent;
	}
	s->open_pb = pb;
	return 1;
}

static void tcp_fill_ends(struct rt_sockctx *c, struct mstream *s, void *pb)
{
	struct rt_sockaddr_in a;
	u32 l = sizeof a;
	if (rt_api_getpeername(c, s->fd, &a, &l) == 0) {
		s->rhost = a.sin_addr;
		s->rport = a.sin_port;
	}
	l = sizeof a;
	if (rt_api_getsockname(c, s->fd, &a, &l) == 0) {
		PB_L(pb, 42) = a.sin_addr;
		PB_W(pb, 46) = a.sin_port;
		s->lport = a.sin_port;
	}
	PB_L(pb, 36) = s->rhost;
	PB_W(pb, 40) = s->rport;
}

static void tcp_poll_open(struct rt_sockctx *c, struct mstream *s)
{
	void *pb = s->open_pb;
	if (s->passive) {
		struct rt_sockaddr_in a;
		u32 l = sizeof a;
		int nfd = rt_api_accept(c, s->fd, &a, &l);
		if (nfd >= 0) {
			rt_api_close(c, s->fd);
			s->fd = nfd;
			nonblock(c, nfd);
			s->state = stEstablished;
			tcp_fill_ends(c, s, pb);
			s->open_pb = NULL;
			mac_complete(pb, 0);
			return;
		}
	} else if (ready(c, s->fd, 1)) {
		int err = 0;
		u32 l = sizeof err;
		rt_api_getsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l);
		s->open_pb = NULL;
		if (err) {
			stream_close(c, s);
			s->state = stClosed;
			mac_complete(pb, openFailed);
			return;
		}
		s->state = stEstablished;
		tcp_fill_ends(c, s, pb);
		mac_complete(pb, 0);
		return;
	}
	if (expired(s->open_deadline)) {
		s->open_pb = NULL;
		stream_close(c, s);
		s->state = stClosed;
		mac_complete(pb, commandTimeout);
	}
}

/* the connection died: what waits fails, and the ASR hears of it once */
static void tcp_dead(struct rt_sockctx *c, struct mstream *s, int reason)
{
	if (s->dead)
		return;
	s->dead = 1;
	s->reason = reason;
	s->state = stClosed;
	fail_all(s, connectionTerminated);
	asr(s, TCPTerminate, (u32)reason);
	(void)c;
}

static void tcp_poll_rcv(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->rcv_q) != NULL) {
		int nocopy = PB_CSCODE(pb) == TCPNoCopyRcv;
		u32 avail = pending(c, s->fd);
		if (!avail) {
			if (s->peer_fin || (ready(c, s->fd, 0) && !pending(c, s->fd))) {
				/* readable with nothing to read: the other end has closed */
				s->peer_fin = 1;
				q_pop(&s->rcv_q);
				mac_complete(pb, connectionClosing);
				continue;
			}
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				mac_complete(pb, commandTimeout);
				s->rcv_deadline = s->rcv_q ? secs_deadline(((u8 *)s->rcv_q)[32]) : 0;
				continue;
			}
			return;
		}
		if (nocopy) {
			u8 *rds = PB_P(pb, 42);
			u32 nrds = PB_W(pb, 46), n;
			int r;
			if (s->loaned)
				return;                         /* the buffer is still out */
			if (!rds || nrds < 2) {
				q_pop(&s->rcv_q);
				mac_complete(pb, invalidRDS);
				continue;
			}
			n = avail < s->rlen ? avail : s->rlen;
			if (n > 65535)
				n = 65535;
			r = rt_api_recv(c, s->fd, s->rbuf, n, RT_MSG_DONTWAIT);
			if (r <= 0) {
				if (r == 0)
					s->peer_fin = 1;
				return;
			}
			rt_put_be16(rds, (u16)r);
			memcpy(rds + 2, &s->rbuf, 4);
			rt_put_be16(rds + 6, 0);
			s->loaned = 1;
		} else {
			u8 *buf = PB_P(pb, 36);
			u32 len = PB_W(pb, 40);
			int r;
			if (!buf || !len) {
				q_pop(&s->rcv_q);
				mac_complete(pb, invalidBufPtr);
				continue;
			}
			r = rt_api_recv(c, s->fd, buf, len, RT_MSG_DONTWAIT);
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
		mac_complete(pb, 0);
		s->rcv_deadline = s->rcv_q ? secs_deadline(((u8 *)s->rcv_q)[32]) : 0;
	}
}

static void tcp_poll_snd(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->snd_q) != NULL) {
		const u8 *w = PB_P(pb, 38);
		u32 off = s->snd_off, skip = off;
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
				tcp_dead(c, s, TCPNetworkFailure);
				return;
			}
			s->snd_off += (u32)r;
			if ((u32)r < len - skip)
				return;                         /* the send buffer is full: later */
			skip = 0;
		}
		q_pop(&s->snd_q);
		s->snd_off = 0;
		mac_complete(pb, 0);
	}
}

static void tcp_poll(struct rt_sockctx *c, struct mstream *s)
{
	if (s->fd < 0)
		return;
	if (s->open_pb) {
		tcp_poll_open(c, s);
		return;
	}
	if (s->state != stEstablished && s->state != stCloseWait && s->state != stFinWait1)
		return;
	{
		int err = 0;
		u32 l = sizeof err;
		if (rt_api_getsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l) == 0 && err) {
			tcp_dead(c, s, err == RT_ECONNRESET ? TCPRemoteAbort : TCPNetworkFailure);
			return;
		}
	}
	tcp_poll_snd(c, s);
	tcp_poll_rcv(c, s);
	if (s->dead)
		return;
	/* the notifications: data that no receive waits for, the other end's close */
	if (!s->rcv_q && !s->told_data && pending(c, s->fd)) {
		s->told_data = 1;
		asr(s, TCPDataArrival, 0);
	}
	if (!s->told_fin && !pending(c, s->fd) && (s->peer_fin || ready(c, s->fd, 0))) {
		s->peer_fin = 1;
		s->told_fin = 1;
		if (s->state == stEstablished)
			s->state = stCloseWait;
		asr(s, TCPClosing, 0);
	}
}

static int tcp_status(void *pb, struct rt_sockctx *c)
{
	struct mstream *s = stream_of(pb);
	struct rt_sockaddr_in a;
	u32 l = sizeof a;
	if (!s)
		return invalidStreamPtr;
	memset((u8 *)pb + 32, 0, 64);
	PB_L(pb, 36) = s->rhost;
	PB_W(pb, 40) = s->rport;
	if (s->fd >= 0 && rt_api_getsockname(c, s->fd, &a, &l) == 0) {
		PB_L(pb, 42) = a.sin_addr;
		PB_W(pb, 46) = a.sin_port;
	}
	((u8 *)pb)[50] = (u8)(s->fd < 0 ? stClosed : s->state);
	PB_W(pb, 52) = 8192;                                    /* sendWindow */
	PB_W(pb, 54) = (u16)(s->rlen > 65535 ? 65535 : s->rlen); /* rcvWindow */
	PB_W(pb, 58) = (u16)(s->fd >= 0 ? (pending(c, s->fd) > 65535 ? 65535 : pending(c, s->fd)) : 0);
	PB_L(pb, 88) = 1460;                                    /* sendMaxSegSize */
	return s->fd < 0 && !s->open_pb ? connectionDoesntExist : 0;
}

static int tcp_global(void *pb)
{
	static u32 params[16], stats[32];
	PB_P(pb, 32) = params;
	PB_P(pb, 36) = stats;
	PB_P(pb, 40) = NULL;
	PB_W(pb, 48) = NSTREAMS;
	return 0;
}

static void stream_free(struct rt_sockctx *c, struct mstream *s, int r)
{
	fail_all(s, r);
	stream_close(c, s);
	s->magic = 0;
}

/* ==================== UDP ==================== */
static int udp_create(void *pb, struct rt_sockctx *c)
{
	struct rt_sockaddr_in a;
	u32 l = sizeof a;
	int i, fd;
	if (!PB_P(pb, 32) || PB_L(pb, 36) < 2048)
		return PB_P(pb, 32) ? invalidLength : invalidBufPtr;
	for (i = 0; i < NSTREAMS && streams[i].magic == S_MAGIC; i++)
		;
	if (i == NSTREAMS)
		return insufficientResources;
	fd = rt_api_socket(c, RT_AF_INET, RT_SOCK_DGRAM, RT_IPPROTO_UDP);
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
		s->rbuf = PB_P(pb, 32);
		s->rlen = PB_L(pb, 36);
		s->notify = PB_P(pb, 40);
		s->udata = PB_P(pb, 46);
		s->lport = a.sin_port;
		PB_W(pb, 44) = a.sin_port;
		PB_L(pb, 28) = (u32)s;
	}
	return 0;
}

static void udp_poll(struct rt_sockctx *c, struct mstream *s)
{
	void *pb;
	while ((pb = s->rcv_q) != NULL) {
		struct rt_sockaddr_in from;
		u32 fl = sizeof from;
		int r;
		if (s->loaned)
			return;
		r = rt_api_recvfrom(c, s->fd, s->rbuf, s->rlen > 65535 ? 65535 : s->rlen, RT_MSG_DONTWAIT, &from, &fl);
		if (r < 0) {
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				mac_complete(pb, commandTimeout);
				s->rcv_deadline = s->rcv_q ? secs_deadline(PB_W(s->rcv_q, 32)) : 0;
				continue;
			}
			return;
		}
		PB_L(pb, 34) = from.sin_addr;
		PB_W(pb, 38) = from.sin_port;
		PB_P(pb, 40) = s->rbuf;
		PB_W(pb, 44) = (u16)r;
		PB_L(pb, 52) = mac_nif ? mac_nif->addr : 0;     /* destHost */
		PB_W(pb, 56) = s->lport;
		s->loaned = 1;
		q_pop(&s->rcv_q);
		s->told_data = 0;
		mac_complete(pb, 0);
		s->rcv_deadline = s->rcv_q ? secs_deadline(PB_W(s->rcv_q, 32)) : 0;
	}
	if (!s->told_data && !s->loaned && ready(c, s->fd, 0)) {
		s->told_data = 1;
		asr(s, UDPDataArrival, 0);
	}
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
		return invalidRDS;
	for (; rt_get_be16(w); w += 6) {
		u32 len = rt_get_be16(w);
		const u8 *p;
		memcpy(&p, w + 2, 4);
		if (n + len > sizeof buf)
			return invalidLength;
		memcpy(buf + n, p, len);
		n += len;
	}
	fill_addr(&to, PB_L(pb, 34), PB_W(pb, 38));
	r = rt_api_sendto(c, s->fd, buf, n, RT_MSG_DONTWAIT, &to, sizeof to);
	return r < 0 ? ipDestDeadErr : 0;
}

/* ==================== the resolver's lookups (RTMAC_DNS) ==================== */
#define NDNS 8
struct dnsq {
	void *pb;
	int fd, server, tries, ptr;
	u16 id;
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
	rt_put_be16(pkt + len, q->ptr ? 12 : 1);        /* PTR or A */
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
		mac_complete(q->pb, r);
	q->pb = NULL;
	if (q->fd >= 0)
		rt_api_close(c, q->fd);
	q->fd = -1;
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
				if (!q->ptr && type == 1 && rdlen == 4 && got < 4) {
					memcpy(&d->addrs[got], m + e + 10, 4);
					if (!got)
						rt_strlcpy(d->cname, nm, sizeof d->cname);
					got++;
				} else if (q->ptr && type == 12) {
					dns_name(m, n, e + 10, d->cname, sizeof d->cname);
					got = 1;
				}
				p = e + 10 + rdlen;
			}
			d->count = (u32)got;
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
	} else {
		rt_strlcpy(q->name, d->name, sizeof q->name);
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

/* ==================== dispatch ==================== */
int mactcp_call(void *pb, struct rt_sockctx *c)
{
	struct mstream *s;
	switch (PB_CSCODE(pb)) {
	case TCPCreate:
		return tcp_create(pb, c);
	case TCPPassiveOpen:
		return tcp_open(pb, c, 1);
	case TCPActiveOpen:
		return tcp_open(pb, c, 0);
	case TCPSend:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
		if (s->dead)
			return connectionTerminated;
		if (s->fd < 0 || s->state == stListen || s->open_pb)
			return connectionDoesntExist;
		if (!PB_P(pb, 38))
			return invalidRDS;
		if (!wds_total(PB_P(pb, 38)))
			return 0;
		q_add(&s->snd_q, pb);
		tcp_poll_snd(c, s);
		return 1;
	case TCPRcv:
	case TCPNoCopyRcv:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
		if (s->dead)
			return connectionTerminated;
		if (s->fd < 0 || s->open_pb)
			return connectionDoesntExist;
		if (!s->rcv_q)
			s->rcv_deadline = secs_deadline(((u8 *)pb)[32]);
		q_add(&s->rcv_q, pb);
		tcp_poll_rcv(c, s);
		return 1;
	case TCPRcvBfrReturn:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
		s->loaned = 0;
		return 0;
	case TCPClose:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
		if (s->fd < 0)
			return connectionDoesntExist;
		rt_api_shutdown(c, s->fd, 1);     /* SHUT_WR: our FIN */
		s->state = s->peer_fin ? stClosed : stFinWait1;
		return 0;
	case TCPAbort:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
		if (s->fd < 0)
			return connectionDoesntExist;
		{
			struct { s32 onoff, secs; } lg = { 1, 0 };
			rt_api_setsockopt(c, s->fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof lg);    /* a reset */
		}
		fail_all(s, connectionTerminated);
		stream_close(c, s);
		s->state = stClosed;
		asr(s, TCPTerminate, TCPULPAbort);
		return 0;
	case TCPStatus:
		return tcp_status(pb, c);
	case TCPRelease:
		if (!(s = stream_of(pb)))
			return invalidStreamPtr;
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
	}
	return controlErr;
}

void mactcp_poll(struct rt_sockctx *c)
{
	for (int i = 0; i < NSTREAMS; i++) {
		struct mstream *s = &streams[i];
		if (s->magic != S_MAGIC)
			continue;
		if (s->tcp)
			tcp_poll(c, s);
		else
			udp_poll(c, s);
	}
	dns_poll(c);
}
