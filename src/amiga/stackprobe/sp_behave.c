/*
 * stackprobe - how the stack behaves: TCP and UDP on loopback, socket
 * options, WaitSelect(), descriptors, breaks, IPv6, loopback speed and,
 * with PEER=, a real network path to tests/amiga/stackprobe_peer.py.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stackprobe.h"

#define PASSIF(c) ((c) ? R_PASS : R_FAIL)

/* a connected loopback pair; reports id as FAIL when it cannot be made */
static int pair(const char *id, const char *desc, LONG *lst, UWORD *port, LONG *c, LONG *s)
{
	*c = *s = -1;
	*lst = sp_tcp_listener(port);
	if (*lst < 0 || sp_tcp_pair(c, s, *lst, *port) < 0) {
		sp_result(id, desc, R_FAIL, sp_errno(), "no loopback connection");
		sp_close(lst);
		return -1;
	}
	return 0;
}

static void unpair(LONG *lst, LONG *c, LONG *s)
{
	sp_close(c);
	sp_close(s);
	sp_close(lst);
}

/* ---- tcp ---- */
static void tcp_echo(void)
{
	LONG lst, c, s, n, total, sent, got, r, len;
	UWORD port;
	char *buf, *in;
	ULONG t0;
	int ok = 1;

	len = sp_quick ? 16384 : 65536;
	if (pair("tcp.echo", "64 KB both ways, checked", &lst, &port, &c, &s) < 0)
		return;
	buf = malloc(4096);
	in = malloc(4096);
	if (!buf || !in) {
		free(buf);
		free(in);
		unpair(&lst, &c, &s);
		return;
	}
	for (n = 0; n < 4096; n++)
		buf[n] = (char)(n * 7 + 3);
	sp_nonblock(c, 1);
	sp_nonblock(s, 1);
	t0 = sp_now_ms();
	sent = got = 0;
	total = len;
	while (got < total && sp_now_ms() - t0 < 20000) {
		if (sent < total) {
			r = sp_send(c, buf + (sent & 4095), (total - sent) < 4096 - (sent & 4095) ? total - sent :
			            4096 - (sent & 4095), 0);
			if (r > 0)
				sent += r;
		}
		r = sp_recv(s, in, 4096, 0);
		if (r > 0) {
			for (n = 0; n < r; n++)
				if (in[n] != buf[(got + n) & 4095])
					ok = 0;
			got += r;
		} else if (r == 0) {
			break;
		} else if (sp_errno() == EWOULDBLOCK) {
			sp_wait(s, 0, 200);
		}
	}
	sp_result_ms("tcp.echo", "loopback transfer, data checked", PASSIF(got == total && ok), 0, sp_now_ms() - t0,
	             "%ld of %ld bytes", (long)got, (long)total);
	free(buf);
	free(in);
	unpair(&lst, &c, &s);
}

static void tcp_close_semantics(void)
{
	LONG lst, c, s, r, e;
	UWORD port;
	char buf[64];
	struct sp_linger lg;

	/* half close: the peer reads EOF and can still send */
	if (pair("tcp.half_close", "shutdown(write): peer reads EOF, still sends", &lst, &port, &c, &s) == 0) {
		sp_shutdown(c, 1);
		r = sp_wait(s, 0, 2000) == 1 ? sp_recv(s, buf, sizeof(buf), 0) : -2;
		e = sp_send(s, (APTR)"back", 4, 0);
		e = e == 4 && sp_recv_all(c, buf, 4, 2000) == 4;
		sp_result("tcp.half_close", "shutdown(write): peer reads EOF, still sends", PASSIF(r == 0 && e), 0, NULL);
		unpair(&lst, &c, &s);
	}
	/* SO_LINGER 0: close sends RST */
	if (pair("tcp.linger0_rst", "SO_LINGER 0 close resets the peer", &lst, &port, &c, &s) == 0) {
		lg.l_onoff = 1;
		lg.l_linger = 0;
		r = sp_setsockopt(c, SOL_SOCKET, SO_LINGER, (APTR)&lg, sizeof(lg));
		sp_close(&c);
		e = 0;
		if (sp_wait(s, 0, 2000) == 1 && sp_recv(s, buf, sizeof(buf), 0) < 0)
			e = sp_errno();
		sp_result("tcp.linger0_rst", "SO_LINGER 0 close resets the peer", r < 0 ? R_UNSUP : PASSIF(e == ECONNRESET), e,
		          NULL);
		unpair(&lst, &c, &s);
	}
	/* close with unread data: RST (RFC 2525) */
	if (pair("tcp.close_unread_rst", "close with unread data resets the peer", &lst, &port, &c, &s) == 0) {
		sp_send(c, (APTR)"unread", 6, 0);
		Delay(5);
		sp_close(&s);
		Delay(5);
		e = 0;
		r = sp_send(c, (APTR)"x", 1, 0);
		if (sp_wait(c, 0, 1000) == 1 && sp_recv(c, buf, sizeof(buf), 0) < 0)
			e = sp_errno();
		sp_result("tcp.close_unread_rst", "close with unread data resets the peer", PASSIF(e == ECONNRESET), e, NULL);
		unpair(&lst, &c, &s);
	}
	/* writing to a reset connection: EPIPE or ECONNRESET */
	if (pair("tcp.send_after_reset", "send() after the peer reset", &lst, &port, &c, &s) == 0) {
		lg.l_onoff = 1;
		lg.l_linger = 0;
		sp_setsockopt(s, SOL_SOCKET, SO_LINGER, (APTR)&lg, sizeof(lg));
		sp_close(&s);
		Delay(5);
		r = sp_send(c, (APTR)"x", 1, 0);
		if (r >= 0)
			r = sp_send(c, (APTR)"x", 1, 0);
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.send_after_reset", "send() after the peer reset", PASSIF(e == EPIPE || e == ECONNRESET), e, "%s",
		          r < 0 ? sp_errname(e) : "accepted");
		unpair(&lst, &c, &s);
	}
}

static void tcp_errors(void)
{
	struct sp_sockaddr_in a;
	LONG c, lst, s, r, e, len, n, i, fds[12];
	UWORD port;
	char buf[8];
	ULONG t0;

	/* refused */
	c = sp_socket(AF_INET, SOCK_STREAM, 0);
	sp_sin(&a, INADDR_LOOPBACK, 9);
	sp_watchdog(5000);
	t0 = sp_now_ms();
	r = sp_connect(c, (APTR)&a, sizeof(a));
	e = r < 0 ? sp_errno() : 0;
	sp_watchdog(0);
	sp_result_ms("tcp.refused", "connect() to a closed port", PASSIF(e == ECONNREFUSED), e, sp_now_ms() - t0, NULL);
	sp_close(&c);

	/* non-blocking connect: EINPROGRESS, writable, then EISCONN */
	lst = sp_tcp_listener(&port);
	c = sp_socket(AF_INET, SOCK_STREAM, 0);
	if (lst >= 0 && c >= 0) {
		sp_nonblock(c, 1);
		sp_sin(&a, INADDR_LOOPBACK, port);
		r = sp_connect(c, (APTR)&a, sizeof(a));
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.nb_connect", "non-blocking connect() says EINPROGRESS", PASSIF(r == 0 || e == EINPROGRESS), e,
		          "%s", r == 0 ? "connected at once" : "");
		r = sp_wait(c, 1, 2000);
		sp_result("tcp.nb_connect_writable", "...and becomes writable", PASSIF(r == 1), 0, NULL);
		r = sp_connect(c, (APTR)&a, sizeof(a));
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.eisconn", "connect() again says EISCONN", PASSIF(e == EISCONN), e, NULL);
		len = sizeof(a);
		s = sp_wait(lst, 0, 1000) == 1 ? sp_accept(lst, (APTR)&a, &len) : -1;
		sp_close(&s);
	}
	sp_close(&c);
	/* non-blocking accept and recv on nothing */
	if (lst >= 0) {
		sp_nonblock(lst, 1);
		len = sizeof(a);
		r = sp_accept(lst, (APTR)&a, &len);
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.nb_accept", "non-blocking accept() with none says EWOULDBLOCK", PASSIF(e == EWOULDBLOCK), e, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
		sp_nonblock(lst, 0);
		if (sp_tcp_pair(&c, &s, lst, port) == 0) {
			sp_nonblock(s, 1);
			r = sp_recv(s, buf, sizeof(buf), 0);
			e = r < 0 ? sp_errno() : 0;
			sp_result("tcp.nb_recv", "non-blocking recv() with none says EWOULDBLOCK", PASSIF(e == EWOULDBLOCK), e, NULL);
			sp_close(&c);
			sp_close(&s);
		}
	}
	/* backlog: connections the stack completes before accept() */
	if (lst >= 0) {
		sp_close(&lst);
		lst = sp_tcp_listener(&port);
	}
	if (lst >= 0) {
		n = 0;
		for (i = 0; i < 8; i++) {
			fds[i] = sp_socket(AF_INET, SOCK_STREAM, 0);
			sp_nonblock(fds[i], 1);
			sp_sin(&a, INADDR_LOOPBACK, port);
			sp_connect(fds[i], (APTR)&a, sizeof(a));
		}
		Delay(25);
		for (i = 0; i < 8; i++)
			if (sp_wait(fds[i], 1, 0) == 1) {
				LONG err = 0;
				len = sizeof(err);
				sp_getsockopt(fds[i], SOL_SOCKET, SO_ERROR, (APTR)&err, &len);
				if (!err)
					n++;
			}
		sp_result("tcp.backlog", "connections completed with listen(5), no accept", R_INFO, 0, "%ld of 8", (long)n);
		for (i = 0; i < 8; i++)
			sp_CloseSocket(fds[i]);
	}
	sp_close(&lst);

	/* address in use, and SO_REUSEADDR */
	lst = sp_tcp_listener(&port);
	c = sp_socket(AF_INET, SOCK_STREAM, 0);
	if (lst >= 0 && c >= 0) {
		sp_sin(&a, INADDR_LOOPBACK, port);
		r = sp_bind(c, (APTR)&a, sizeof(a));
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.eaddrinuse", "bind() to a listening port says EADDRINUSE", PASSIF(e == EADDRINUSE), e, NULL);
	}
	sp_close(&c);
	sp_close(&lst);
	{
		LONG s1, s2, cl, sv, one = 1;
		lst = sp_tcp_listener(&port);
		if (lst >= 0 && sp_tcp_pair(&cl, &sv, lst, port) == 0) {
			sp_close(&sv);   /* we close first: TIME_WAIT on our side */
			Delay(5);
			sp_close(&cl);
			sp_close(&lst);
			s1 = sp_socket(AF_INET, SOCK_STREAM, 0);
			sp_setsockopt(s1, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof(one));
			sp_sin(&a, INADDR_LOOPBACK, port);
			r = sp_bind(s1, (APTR)&a, sizeof(a));
			sp_result("tcp.reuseaddr", "SO_REUSEADDR binds a port in TIME_WAIT", PASSIF(r == 0), r ? sp_errno() : 0, NULL);
			sp_close(&s1);
			s2 = sp_socket(AF_INET, SOCK_STREAM, 0);
			sp_setsockopt(s2, SOL_SOCKET, SO_REUSEPORT, (APTR)&one, sizeof(one));
			sp_close(&s2);
		}
		sp_close(&lst);
	}
	/* ephemeral ports */
	{
		ULONG lo = 65535, hi = 0;
		for (i = 0; i < 10; i++) {
			fds[i] = sp_socket(AF_INET, SOCK_STREAM, 0);
			sp_sin(&a, INADDR_ANY, 0);
			len = sizeof(a);
			if (fds[i] >= 0 && !sp_bind(fds[i], (APTR)&a, sizeof(a)) && !sp_getsockname(fds[i], (APTR)&a, &len)) {
				if (a.sin_port < lo)
					lo = a.sin_port;
				if (a.sin_port > hi)
					hi = a.sin_port;
			}
		}
		for (i = 0; i < 10; i++)
			sp_close(&fds[i]);
		sp_result("tcp.ephemeral", "ports bind(0) picks (10 sockets)", R_INFO, 0, "%lu-%lu", (unsigned long)lo,
		          (unsigned long)hi);
	}
}

static void tcp_flags(void)
{
	LONG lst, c, s, r, e, v;
	UWORD port;
	char buf[16];
	BYTE sig;
	ULONG t[3];

	/* MSG_PEEK */
	if (pair("tcp.msg_peek", "MSG_PEEK leaves the data", &lst, &port, &c, &s) == 0) {
		sp_send(c, (APTR)"peek", 4, 0);
		sp_wait(s, 0, 2000);
		r = sp_recv(s, buf, 4, MSG_PEEK);
		v = sp_recv(s, buf + 4, 4, 0);
		sp_result("tcp.msg_peek", "MSG_PEEK leaves the data", PASSIF(r == 4 && v == 4 && !memcmp(buf, buf + 4, 4)), 0, NULL);
		/* MSG_DONTWAIT */
		r = sp_recv(s, buf, sizeof(buf), MSG_DONTWAIT);
		e = r < 0 ? sp_errno() : 0;
		sp_result("tcp.msg_dontwait", "MSG_DONTWAIT on a blocking socket", PASSIF(e == EWOULDBLOCK), e, NULL);
		/* MSG_WAITALL: 6 bytes sent in two parts, a little apart */
		sp_send(c, (APTR)"abc", 3, 0);
		sp_watchdog(3000);
		r = -1;
		if (sp_send(c, (APTR)"def", 3, 0) == 3)
			r = sp_recv(s, buf, 6, MSG_WAITALL);
		sp_watchdog(0);
		sp_result("tcp.msg_waitall", "MSG_WAITALL gets the whole 6 bytes", PASSIF(r == 6), r < 0 ? sp_errno() : 0, "%ld",
		          (long)r);
		unpair(&lst, &c, &s);
	}
	/* out of band */
	if (pair("tcp.oob", "MSG_OOB urgent byte", &lst, &port, &c, &s) == 0) {
		sig = AllocSignal(-1);
		t[0] = SBTM_SETVAL(SBTC_SIGURGMASK);
		t[1] = sig >= 0 ? 1UL << sig : 0;
		t[2] = TAG_DONE;
		if (sig >= 0)
			SetSignal(0, 1UL << sig);
		if (SP_HAVE_BSD(SocketBaseTagList))
			sp_SocketBaseTagList((APTR)t);
		v = (LONG)FindTask(NULL);
		sp_IoctlSocket(s, FIOSETOWN, (APTR)&v);
		sp_send(c, (APTR)"ab", 2, 0);
		r = sp_send(c, (APTR)"U", 1, MSG_OOB);
		Delay(10);
		{
			sp_fdset ex;
			struct sp_timeval tv = { 1, 0 };
			SP_FD_ZERO(&ex);
			SP_FD_SET(s, &ex);
			v = sp_WaitSelect(s + 1, NULL, NULL, (APTR)&ex, (APTR)&tv, NULL);
			sp_result("tcp.oob_select", "urgent data shows in the except set", PASSIF(v == 1 && SP_FD_ISSET(s, &ex)), 0,
			          NULL);
		}
		sp_result("tcp.sigurg", "SIGURG signal for urgent data", PASSIF(sig >= 0 && (SetSignal(0, 0) & (1UL << sig))), 0,
		          NULL);
		buf[0] = 0;
		e = sp_recv(s, buf, 1, MSG_OOB);
		sp_result("tcp.oob_recv", "recv(MSG_OOB) gets the urgent byte", PASSIF(r == 1 && e == 1 && buf[0] == 'U'),
		          e < 0 ? sp_errno() : 0, NULL);
		v = -1;
		sp_recv(s, buf, 2, 0);
		r = sp_IoctlSocket(s, SIOCATMARK, (APTR)&v);
		sp_result("tcp.siocatmark", "SIOCATMARK at the mark", PASSIF(r == 0 && v == 1), r ? sp_errno() : 0, "%ld", (long)v);
		t[1] = 0;
		if (SP_HAVE_BSD(SocketBaseTagList))
			sp_SocketBaseTagList((APTR)t);
		if (sig >= 0)
			FreeSignal(sig);
		unpair(&lst, &c, &s);
	}
	if (pair("tcp.oobinline", "SO_OOBINLINE puts the byte in line", &lst, &port, &c, &s) == 0) {
		v = 1;
		r = sp_setsockopt(s, SOL_SOCKET, SO_OOBINLINE, (APTR)&v, sizeof(v));
		sp_send(c, (APTR)"ab", 2, 0);
		sp_send(c, (APTR)"U", 1, MSG_OOB);
		e = sp_recv_all(s, buf, 3, 2000);
		sp_result("tcp.oobinline", "SO_OOBINLINE puts the byte in line", r < 0 ? R_UNSUP : PASSIF(e == 3 && buf[2] == 'U'),
		          r < 0 ? sp_errno() : 0, NULL);
		unpair(&lst, &c, &s);
	}
	/* FIOASYNC: SIGIO when data arrives */
	if (pair("tcp.fioasync", "FIOASYNC signals arriving data", &lst, &port, &c, &s) == 0) {
		sig = AllocSignal(-1);
		t[0] = SBTM_SETVAL(SBTC_SIGIOMASK);
		t[1] = sig >= 0 ? 1UL << sig : 0;
		t[2] = TAG_DONE;
		if (sig >= 0) {
			SetSignal(0, 1UL << sig);
			if (SP_HAVE_BSD(SocketBaseTagList))
				sp_SocketBaseTagList((APTR)t);
			v = (LONG)FindTask(NULL);
			sp_IoctlSocket(s, FIOSETOWN, (APTR)&v);
			v = 1;
			r = sp_IoctlSocket(s, FIOASYNC, (APTR)&v);
			sp_send(c, (APTR)"io", 2, 0);
			Delay(10);
			sp_result("tcp.fioasync", "FIOASYNC signals arriving data", r < 0 ? R_UNSUP :
			          PASSIF(SetSignal(0, 0) & (1UL << sig)), r < 0 ? sp_errno() : 0, NULL);
			v = 0;
			sp_IoctlSocket(s, FIOASYNC, (APTR)&v);
			t[1] = 0;
			if (SP_HAVE_BSD(SocketBaseTagList))
				sp_SocketBaseTagList((APTR)t);
			FreeSignal(sig);
		}
		unpair(&lst, &c, &s);
	}
}

/* descriptor passing to another process: ReleaseSocket() here,
 * ObtainSocket() there */
static volatile LONG xfer_id, xfer_result;
static volatile int xfer_done;
static struct Task *xfer_parent;
static BYTE xfer_sig;

static SP_ENTRY void xfer_entry(void)
{
	struct Library *SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 0);
	LONG s;

	xfer_result = -2;
	if (SocketBase) {
		s = SP_HAVE(SocketBase, LVO_bsdsocket_ObtainSocket) ? __bsdsocket_ObtainSocket(SocketBase, xfer_id, AF_INET,
		                                                                               SOCK_STREAM, 0) : -1;
		if (s >= 0) {
			xfer_result = __bsdsocket_send(SocketBase, s, (APTR)"moved", 5, 0);
			__bsdsocket_CloseSocket(SocketBase, s);
		} else {
			xfer_result = -1;
		}
		CloseLibrary(SocketBase);
	}
	Forbid();
	xfer_done = 1;
	Signal(xfer_parent, 1UL << xfer_sig);
}

static void misc_transfer(void)
{
	LONG lst, c, s, id, i;
	UWORD port;
	char buf[8];

	if (!SP_HAVE_BSD(ReleaseSocket) || !SP_HAVE_BSD(ObtainSocket)) {
		sp_unsup_call("misc.socket_to_process", "a socket handed to another process");
		return;
	}
	if (pair("misc.socket_to_process", "a socket handed to another process", &lst, &port, &c, &s) < 0)
		return;
	xfer_sig = AllocSignal(-1);
	xfer_parent = FindTask(NULL);
	xfer_done = 0;
	id = sp_ReleaseSocket(c, -1);
	if (id == -1 || xfer_sig < 0) {
		sp_result("misc.socket_to_process", "a socket handed to another process", R_FAIL, sp_errno(), "ReleaseSocket");
	} else {
		c = -1;
		xfer_id = id;
		if (CreateNewProcTags(NP_Entry, (ULONG)xfer_entry, NP_Name, (ULONG)"stackprobe child", NP_StackSize, 8192, TAG_DONE)) {
			for (i = 0; i < 250 && !xfer_done; i++)
				Delay(2);
			i = sp_recv_all(s, buf, 5, 2000);
			sp_result("misc.socket_to_process", "a socket handed to another process", PASSIF(xfer_done && xfer_result == 5 &&
			          i == 5 && !memcmp(buf, "moved", 5)), 0, "%ld", (long)xfer_result);
		}
	}
	if (xfer_sig >= 0)
		FreeSignal(xfer_sig);
	unpair(&lst, &c, &s);
}

void sp_test_tcp(void)
{
	tcp_echo();
	tcp_close_semantics();
	tcp_errors();
	tcp_flags();
}

/* ---- udp ---- */
static LONG udp_bound(struct sp_sockaddr_in *a)
{
	LONG u = sp_socket(AF_INET, SOCK_DGRAM, 0), len = sizeof(*a);
	if (u < 0)
		return -1;
	sp_sin(a, INADDR_LOOPBACK, 0);
	if (sp_bind(u, (APTR)a, sizeof(*a)) < 0 || sp_getsockname(u, (APTR)a, &len) < 0) {
		sp_CloseSocket(u);
		return -1;
	}
	return u;
}

void sp_test_udp(void)
{
	struct sp_sockaddr_in a, b, from;
	LONG u, w, r, e, len, v, i;
	char *big, buf[64];
	ULONG t0;

	u = udp_bound(&a);
	w = udp_bound(&b);
	if (u < 0 || w < 0) {
		sp_result("udp.loopback", "UDP datagram on loopback", R_FAIL, sp_errno(), NULL);
		sp_close(&u);
		sp_close(&w);
		return;
	}
	r = sp_sendto(w, (APTR)"dgram", 5, 0, (APTR)&a, sizeof(a));
	len = sizeof(from);
	e = sp_wait(u, 0, 2000) == 1 ? sp_recvfrom(u, buf, sizeof(buf), 0, (APTR)&from, &len) : -1;
	sp_result("udp.loopback", "UDP datagram on loopback", PASSIF(r == 5 && e == 5 && from.sin_port == b.sin_port), 0, NULL);
	/* truncation: the rest of the datagram is dropped */
	sp_sendto(w, (APTR)"0123456789", 10, 0, (APTR)&a, sizeof(a));
	sp_sendto(w, (APTR)"next", 4, 0, (APTR)&a, sizeof(a));
	sp_wait(u, 0, 2000);
	r = sp_recv(u, buf, 4, 0);
	e = sp_wait(u, 0, 1000) == 1 ? sp_recv(u, buf + 8, 8, 0) : -1;
	sp_result("udp.truncate", "a short read drops the rest of the datagram", PASSIF(r == 4 && e == 4 &&
	          !memcmp(buf + 8, "next", 4)), 0, NULL);
	/* big datagrams (fragments on loopback when MTU is small); 4.4BSD
	 * limits a UDP send to the socket's send buffer, so raise both first */
	v = 65535;
	sp_setsockopt(w, SOL_SOCKET, SO_SNDBUF, (APTR)&v, sizeof(v));
	sp_setsockopt(u, SOL_SOCKET, SO_RCVBUF, (APTR)&v, sizeof(v));
	big = malloc(65000);
	if (big) {
		static const LONG sizes[] = { 1472, 8192, 32768, 65000 };
		for (i = 0; i < 4; i++) {
			char id[24], d[48];
			memset(big, (int)i + 1, sizes[i]);
			r = sp_sendto(w, big, sizes[i], 0, (APTR)&a, sizeof(a));
			e = r < 0 ? sp_errno() : 0;
			v = sp_wait(u, 0, 2000) == 1 ? sp_recv(u, big, 65000, 0) : -1;
			sprintf(id, "udp.size_%ld", (long)sizes[i]);
			sprintf(d, "%ld byte datagram on loopback", (long)sizes[i]);
			sp_result(id, d, PASSIF(v == sizes[i]), e, "%ld", (long)v);
		}
		free(big);
	}
	/* connected UDP */
	r = sp_connect(w, (APTR)&a, sizeof(a));
	e = r == 0 ? sp_send(w, (APTR)"conn", 4, 0) : -1;
	v = sp_wait(u, 0, 2000) == 1 ? sp_recv(u, buf, sizeof(buf), 0) : -1;
	sp_result("udp.connected", "connect()ed UDP socket with send()", PASSIF(r == 0 && e == 4 && v == 4), r ? sp_errno() : 0,
	          NULL);
	sp_close(&u);
	/* ICMP port unreachable reported to a connected socket */
	sp_send(w, (APTR)"gone", 4, 0);
	Delay(10);
	r = sp_send(w, (APTR)"gone", 4, 0);
	e = r < 0 ? sp_errno() : 0;
	if (!e && sp_wait(w, 0, 500) == 1 && sp_recv(w, buf, sizeof(buf), 0) < 0)
		e = sp_errno();
	sp_result("udp.refused", "connected UDP learns ECONNREFUSED", PASSIF(e == ECONNREFUSED), e, NULL);
	sp_close(&w);

	/* broadcast needs SO_BROADCAST */
	u = sp_socket(AF_INET, SOCK_DGRAM, 0);
	sp_sin(&a, INADDR_BROADCAST, 9);
	r = sp_sendto(u, (APTR)"b", 1, 0, (APTR)&a, sizeof(a));
	e = r < 0 ? sp_errno() : 0;
	sp_result("udp.broadcast_denied", "broadcast without SO_BROADCAST is refused", e == ENETUNREACH ||
	          e == EHOSTUNREACH ? R_SKIP : PASSIF(e == EACCES), e, e == ENETUNREACH || e == EHOSTUNREACH ?
	          "no broadcast interface" : NULL);
	v = 1;
	sp_setsockopt(u, SOL_SOCKET, SO_BROADCAST, (APTR)&v, sizeof(v));
	r = sp_sendto(u, (APTR)"b", 1, 0, (APTR)&a, sizeof(a));
	e = r < 0 ? sp_errno() : 0;
	sp_result("udp.broadcast", "broadcast with SO_BROADCAST", r == 1 ? R_PASS : e == ENETUNREACH || e == EHOSTUNREACH ?
	          R_SKIP : R_FAIL, e, r < 0 && (e == ENETUNREACH || e == EHOSTUNREACH) ? "no broadcast interface" : NULL);
	sp_close(&u);

	/* multicast to ourselves */
	{
		struct sp_ip_mreq mr;
		struct sp_sockaddr_in g;
		LONG one = 1, ttl = 1;
		u = sp_socket(AF_INET, SOCK_DGRAM, 0);
		sp_setsockopt(u, SOL_SOCKET, SO_REUSEADDR, (APTR)&one, sizeof(one));
		sp_sin(&g, INADDR_ANY, 5353 + 17);
		sp_bind(u, (APTR)&g, sizeof(g));
		mr.imr_multiaddr.s_addr = 0xef010203UL; /* 239.1.2.3 */
		mr.imr_interface.s_addr = INADDR_ANY;
		r = sp_setsockopt(u, IPPROTO_IP, IP_ADD_MEMBERSHIP, (APTR)&mr, sizeof(mr));
		e = r < 0 ? sp_errno() : 0;
		sp_result("udp.mcast_join", "IP_ADD_MEMBERSHIP 239.1.2.3", r == 0 ? R_PASS : e == EADDRNOTAVAIL ||
		          e == ENETUNREACH ? R_SKIP : e == EINVAL || e == ENOPROTOOPT || e == EOPNOTSUPP ? R_UNSUP : R_FAIL, e,
		          r < 0 && (e == EADDRNOTAVAIL || e == ENETUNREACH) ? "no multicast interface" : NULL);
		if (r == 0) {
			w = sp_socket(AF_INET, SOCK_DGRAM, 0);
			sp_setsockopt(w, IPPROTO_IP, IP_MULTICAST_LOOP, (APTR)&one, sizeof(one));
			sp_setsockopt(w, IPPROTO_IP, IP_MULTICAST_TTL, (APTR)&ttl, sizeof(ttl));
			sp_sin(&g, 0xef010203UL, 5353 + 17);
			r = sp_sendto(w, (APTR)"mc", 2, 0, (APTR)&g, sizeof(g));
			e = r < 0 ? sp_errno() : 0;
			v = sp_wait(u, 0, 1500) == 1 ? sp_recv(u, buf, sizeof(buf), 0) : -1;
			sp_result("udp.mcast_loop", "multicast looped back to a member", r == 2 ? PASSIF(v == 2) :
			          e == ENETUNREACH ? R_SKIP : R_FAIL, e, r < 0 && e == ENETUNREACH ? "no multicast interface" : NULL);
			sp_close(&w);
			r = sp_setsockopt(u, IPPROTO_IP, IP_DROP_MEMBERSHIP, (APTR)&mr, sizeof(mr));
			sp_result("udp.mcast_leave", "IP_DROP_MEMBERSHIP", PASSIF(r == 0), r ? sp_errno() : 0, NULL);
		}
		sp_close(&u);
	}

	/* raw ICMP echo to 127.0.0.1 */
	u = sp_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (u < 0) {
		e = sp_errno();
		sp_result("udp.raw_icmp", "raw ICMP socket: ping 127.0.0.1", e == EPROTONOSUPPORT || e == ESOCKTNOSUPPORT ||
		          e == EACCES ? R_UNSUP : R_FAIL, e, NULL);
	} else {
		UBYTE pkt[16], in[128];
		ULONG sum = 0;
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 8;            /* echo request */
		pkt[4] = 0x12;
		pkt[5] = 0x34;         /* id */
		pkt[7] = 1;            /* seq */
		memcpy(pkt + 8, "stackprb", 8);
		for (i = 0; i < 16; i += 2)
			sum += (pkt[i] << 8) | pkt[i + 1];
		sum = (sum & 0xffff) + (sum >> 16);
		sum = ~(sum + (sum >> 16)) & 0xffff;
		pkt[2] = (UBYTE)(sum >> 8);
		pkt[3] = (UBYTE)sum;
		sp_sin(&a, INADDR_LOOPBACK, 0);
		t0 = sp_now_us();
		r = sp_sendto(u, pkt, 16, 0, (APTR)&a, sizeof(a));
		v = -1;
		for (i = 0; i < 4 && v < 0; i++) {
			if (sp_wait(u, 0, 1000) != 1)
				break;
			e = sp_recv(u, in, sizeof(in), 0);
			/* an IP header first, then ICMP: type 0 with our id */
			if (e >= 28) {
				LONG hl = (in[0] & 15) * 4;
				if (in[hl] == 0 && in[hl + 4] == 0x12 && in[hl + 5] == 0x34)
					v = (LONG)((sp_now_us() - t0) / 1000);
			}
		}
		sp_result_ms("udp.raw_icmp", "raw ICMP socket: ping 127.0.0.1", PASSIF(r == 16 && v >= 0), r < 0 ? sp_errno() : 0,
		             v, NULL);
		sp_close(&u);
	}
}

/* ---- options ---- */
static void opt_flag(const char *id, const char *desc, LONG s, LONG level, LONG name)
{
	LONG v = 1, got = 0, len = sizeof(got), r;

	r = sp_setsockopt(s, level, name, (APTR)&v, sizeof(v));
	if (r < 0) {
		LONG e = sp_errno();
		sp_result(id, desc, e == ENOPROTOOPT || e == EOPNOTSUPP || e == EINVAL ? R_UNSUP : R_FAIL, e, NULL);
		return;
	}
	r = sp_getsockopt(s, level, name, (APTR)&got, &len);
	sp_result(id, desc, PASSIF(r == 0 && got != 0), r ? sp_errno() : 0, "%ld", (long)got);
}

static void opt_value(const char *id, const char *desc, LONG s, LONG level, LONG name, LONG set)
{
	LONG got = 0, len = sizeof(got), r;

	if (set >= 0 && sp_setsockopt(s, level, name, (APTR)&set, sizeof(set)) < 0) {
		LONG e = sp_errno();
		sp_result(id, desc, e == ENOPROTOOPT || e == EOPNOTSUPP || e == EINVAL ? R_UNSUP : R_FAIL, e, NULL);
		return;
	}
	r = sp_getsockopt(s, level, name, (APTR)&got, &len);
	if (r < 0) {
		LONG e = sp_errno();
		sp_result(id, desc, e == ENOPROTOOPT || e == EOPNOTSUPP || e == EINVAL ? R_UNSUP : R_FAIL, e, NULL);
		return;
	}
	sp_result(id, desc, set >= 0 ? PASSIF(got >= set) : R_PASS, 0, "%ld", (long)got);
}

void sp_test_options(void)
{
	LONG t = sp_socket(AF_INET, SOCK_STREAM, 0), u = sp_socket(AF_INET, SOCK_DGRAM, 0), r, e, v, len;
	LONG lst, c, s;
	UWORD port;
	struct sp_timeval tv;
	struct sp_linger lg;
	char buf[8];
	ULONG t0;

	if (t < 0 || u < 0) {
		sp_result("options.sockets", "TCP and UDP sockets", R_FAIL, sp_errno(), NULL);
		sp_close(&t);
		sp_close(&u);
		return;
	}
	opt_flag("options.so_reuseaddr", "SO_REUSEADDR", t, SOL_SOCKET, SO_REUSEADDR);
	opt_flag("options.so_reuseport", "SO_REUSEPORT", t, SOL_SOCKET, SO_REUSEPORT);
	opt_flag("options.so_keepalive", "SO_KEEPALIVE", t, SOL_SOCKET, SO_KEEPALIVE);
	opt_flag("options.so_dontroute", "SO_DONTROUTE", t, SOL_SOCKET, SO_DONTROUTE);
	opt_flag("options.so_broadcast", "SO_BROADCAST", u, SOL_SOCKET, SO_BROADCAST);
	opt_flag("options.so_oobinline", "SO_OOBINLINE", t, SOL_SOCKET, SO_OOBINLINE);
	opt_value("options.so_sndbuf", "SO_SNDBUF set 32768", t, SOL_SOCKET, SO_SNDBUF, 32768);
	opt_value("options.so_rcvbuf", "SO_RCVBUF set 32768", t, SOL_SOCKET, SO_RCVBUF, 32768);
	opt_value("options.so_sndlowat", "SO_SNDLOWAT", t, SOL_SOCKET, SO_SNDLOWAT, -1);
	opt_value("options.so_rcvlowat", "SO_RCVLOWAT set 1", t, SOL_SOCKET, SO_RCVLOWAT, 1);
	opt_value("options.so_type", "SO_TYPE of a UDP socket", u, SOL_SOCKET, SO_TYPE, -1);
	opt_value("options.so_error", "SO_ERROR", t, SOL_SOCKET, SO_ERROR, -1);
	opt_flag("options.tcp_nodelay", "TCP_NODELAY", t, IPPROTO_TCP, TCP_NODELAY);
	opt_value("options.tcp_maxseg", "TCP_MAXSEG", t, IPPROTO_TCP, TCP_MAXSEG, -1);
	opt_value("options.ip_tos", "IP_TOS set 0x10", t, IPPROTO_IP, IP_TOS, 0x10);
	opt_value("options.ip_ttl", "IP_TTL set 33", t, IPPROTO_IP, IP_TTL, 33);
	opt_value("options.ip_multicast_ttl", "IP_MULTICAST_TTL", u, IPPROTO_IP, IP_MULTICAST_TTL, -1);
	opt_value("options.ip_multicast_loop", "IP_MULTICAST_LOOP", u, IPPROTO_IP, IP_MULTICAST_LOOP, -1);
	lg.l_onoff = 1;
	lg.l_linger = 5;
	r = sp_setsockopt(t, SOL_SOCKET, SO_LINGER, (APTR)&lg, sizeof(lg));
	memset(&lg, 0, sizeof(lg));
	len = sizeof(lg);
	if (!r)
		r = sp_getsockopt(t, SOL_SOCKET, SO_LINGER, (APTR)&lg, &len);
	sp_result("options.so_linger", "SO_LINGER 5 s read back", PASSIF(r == 0 && lg.l_onoff && lg.l_linger == 5), r ?
	          sp_errno() : 0, "%ld", (long)lg.l_linger);
	/* an unknown option */
	v = 1;
	r = sp_setsockopt(t, SOL_SOCKET, 0x7777, (APTR)&v, sizeof(v));
	e = r < 0 ? sp_errno() : 0;
	sp_result("options.unknown", "an unknown option says ENOPROTOOPT", PASSIF(e == ENOPROTOOPT), e, NULL);
	/* SO_ACCEPTCONN on a listener */
	lst = sp_tcp_listener(&port);
	if (lst >= 0)
		opt_value("options.so_acceptconn", "SO_ACCEPTCONN of a listener", lst, SOL_SOCKET, SO_ACCEPTCONN, -1);
	/* IP_HDRINCL on a raw socket */
	r = sp_socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
	if (r >= 0) {
		opt_flag("options.ip_hdrincl", "IP_HDRINCL on a raw socket", r, IPPROTO_IP, IP_HDRINCL);
		sp_CloseSocket(r);
	} else {
		sp_result("options.ip_hdrincl", "IP_HDRINCL on a raw socket", R_UNSUP, sp_errno(), "no raw socket");
	}
	/* SO_RCVTIMEO: recv() on an empty socket returns after the time */
	if (lst >= 0 && sp_tcp_pair(&c, &s, lst, port) == 0) {
		tv.tv_sec = 0;
		tv.tv_usec = 500000;
		r = sp_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (APTR)&tv, sizeof(tv));
		if (r < 0) {
			sp_result("options.so_rcvtimeo", "SO_RCVTIMEO 0.5 s ends recv()", R_UNSUP, sp_errno(), NULL);
		} else {
			sp_watchdog(4000);
			t0 = sp_now_ms();
			r = sp_recv(s, buf, sizeof(buf), 0);
			e = r < 0 ? sp_errno() : 0;
			t0 = sp_now_ms() - t0;
			sp_result_ms("options.so_rcvtimeo", "SO_RCVTIMEO 0.5 s ends recv()", sp_watchdog_fired() ? R_FAIL :
			             PASSIF(r < 0 && e == EWOULDBLOCK && t0 < 2000), e, t0, "%s", sp_watchdog_fired() ?
			             "ignored: stopped by Ctrl-C after 4 s" : "");
			sp_watchdog(0);
		}
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = sp_setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (APTR)&tv, sizeof(tv));
		sp_result("options.so_sndtimeo", "SO_SNDTIMEO set", r < 0 ? R_UNSUP : R_PASS, r < 0 ? sp_errno() : 0, NULL);
		sp_close(&c);
		sp_close(&s);
	}
	sp_close(&lst);
	sp_close(&t);
	sp_close(&u);
}

/* ---- select ---- */
void sp_test_select(void)
{
	struct sp_timeval tv;
	LONG r, i, n, lst, c, s, fds[100], maxfd;
	UWORD port;
	ULONG t0, sigs;
	sp_fdset rd;

	/* timeout accuracy */
	{
		static const LONG wait_ms[] = { 20, 100, 500 };
		for (i = 0; i < 3; i++) {
			char id[32], d[40];
			tv.tv_sec = 0;
			tv.tv_usec = wait_ms[i] * 1000;
			t0 = sp_now_us();
			r = sp_WaitSelect(0, NULL, NULL, NULL, (APTR)&tv, NULL);
			t0 = (sp_now_us() - t0) / 1000;
			sprintf(id, "select.timeout_%ld", (long)wait_ms[i]);
			sprintf(d, "WaitSelect() %ld ms timeout lasts", (long)wait_ms[i]);
			sp_result_ms(id, d, PASSIF(r == 0 && (LONG)t0 >= wait_ms[i] - 5 && (LONG)t0 <= wait_ms[i] * 2 + 40), 0, t0,
			             "%lu ms", (unsigned long)t0);
		}
	}
	/* a zero timeout polls */
	tv.tv_sec = tv.tv_usec = 0;
	t0 = sp_now_ms();
	r = sp_WaitSelect(0, NULL, NULL, NULL, (APTR)&tv, NULL);
	sp_result("select.poll", "zero timeout returns at once", PASSIF(r == 0 && sp_now_ms() - t0 < 50), 0, NULL);
	/* a break signal in the mask ends the wait */
	SetSignal(SIGBREAKF_CTRL_E, SIGBREAKF_CTRL_E);
	sigs = SIGBREAKF_CTRL_E;
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	t0 = sp_now_ms();
	r = sp_WaitSelect(0, NULL, NULL, NULL, (APTR)&tv, &sigs);
	sp_result("select.signal", "a signal in the mask ends WaitSelect()", PASSIF(r == 0 && (sigs & SIGBREAKF_CTRL_E) &&
	          sp_now_ms() - t0 < 1000), 0, NULL);
	SetSignal(0, SIGBREAKF_CTRL_E);
	/* Ctrl-C (the break mask) ends a blocking accept() */
	lst = sp_tcp_listener(&port);
	if (lst >= 0) {
		struct sp_sockaddr_in a;
		LONG len = sizeof(a), e;
		sp_watchdog(500);
		t0 = sp_now_ms();
		r = sp_accept(lst, (APTR)&a, &len);
		e = r < 0 ? sp_errno() : 0;
		sp_result_ms("select.break_accept", "Ctrl-C ends a blocking accept()", PASSIF(r < 0 && e == EINTR), e,
		             sp_now_ms() - t0, NULL);
		sp_watchdog(0);
		if (r >= 0)
			sp_CloseSocket(r);
	}
	/* readable and writable */
	if (lst >= 0 && sp_tcp_pair(&c, &s, lst, port) == 0) {
		r = sp_wait(s, 0, 0);
		n = sp_wait(c, 1, 0);
		sp_send(c, (APTR)"r", 1, 0);
		i = sp_wait(s, 0, 1000);
		sp_result("select.rw", "not readable, writable, readable after data", PASSIF(r == 0 && n == 1 && i == 1), 0, NULL);
		sp_close(&c);
		i = sp_wait(s, 0, 1000);
		sp_result("select.eof", "readable at the peer's close", PASSIF(i == 1), 0, NULL);
		sp_close(&s);
	}
	sp_close(&lst);
	/* many descriptors: past 64 */
	{
		ULONG t[3] = { SBTM_SETVAL(SBTC_DTABLESIZE), 256, TAG_DONE };
		if (SP_HAVE_BSD(SocketBaseTagList))
			sp_SocketBaseTagList((APTR)t);
	}
	n = 0;
	maxfd = -1;
	for (i = 0; i < 100; i++) {
		fds[i] = sp_socket(AF_INET, SOCK_DGRAM, 0);
		if (fds[i] < 0 || fds[i] >= SP_FDWORDS * 32)
			break;
		n++;
		if (fds[i] > maxfd)
			maxfd = fds[i];
	}
	if (n > 70 && maxfd >= 64) {
		struct sp_sockaddr_in a;
		LONG last = fds[n - 1], len = sizeof(a);
		sp_sin(&a, INADDR_LOOPBACK, 0);
		sp_bind(last, (APTR)&a, sizeof(a));
		sp_getsockname(last, (APTR)&a, &len);
		sp_sendto(fds[0], (APTR)"hi", 2, 0, (APTR)&a, sizeof(a));
		Delay(5);
		SP_FD_ZERO(&rd);
		for (i = 0; i < n; i++)
			SP_FD_SET(fds[i], &rd);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = sp_WaitSelect(maxfd + 1, (APTR)&rd, NULL, NULL, (APTR)&tv, NULL);
		sp_result("select.over64", "WaitSelect() on a descriptor above 64", PASSIF(r == 1 && SP_FD_ISSET(last, &rd)), 0,
		          "fd %ld", (long)last);
	} else {
		sp_result("select.over64", "WaitSelect() on a descriptor above 64", R_FAIL, sp_errno(), "only %ld sockets", (long)n);
	}
	for (i = 0; i < n; i++)
		sp_CloseSocket(fds[i]);
}

/* ---- misc ---- */
void sp_test_misc(void)
{
	LONG i, n = 0, *fds, e = 0, max = sp_quick ? 128 : 512;
	ULONG t[3];
	ULONG t0;

	/* how many sockets one program can have */
	t[0] = SBTM_SETVAL(SBTC_DTABLESIZE);
	t[1] = max + 8;
	t[2] = TAG_DONE;
	if (SP_HAVE_BSD(SocketBaseTagList))
		sp_SocketBaseTagList((APTR)t);
	fds = malloc(max * sizeof(LONG));
	if (fds) {
		t0 = sp_now_ms();
		for (i = 0; i < max; i++) {
			fds[i] = sp_socket(AF_INET, SOCK_STREAM, 0);
			if (fds[i] < 0) {
				e = sp_errno();
				break;
			}
			n++;
		}
		for (i = 0; i < n; i++)
			sp_CloseSocket(fds[i]);
		sp_result_ms("misc.many_sockets", "sockets one program can open", n >= 64 ? R_PASS : R_FAIL, n < max ? e : 0,
		             sp_now_ms() - t0, "%ld%s", (long)n, n == max ? " (all asked for)" : "");
		free(fds);
	}
	misc_transfer();
	/* errors on bad descriptors */
	{
		char b[4];
		LONG r = sp_recv(4000, b, 4, 0);
		e = r < 0 ? sp_errno() : 0;
		sp_result("misc.ebadf", "recv() on a bad descriptor says EBADF", PASSIF(e == EBADF || e == ENOTSOCK), e, NULL);
		r = sp_socket(AF_INET, SOCK_STREAM, IPPROTO_UDP);
		e = r < 0 ? sp_errno() : 0;
		sp_result("misc.eprotonosupport", "socket(STREAM, UDP) is refused", PASSIF(r < 0), e, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
		r = sp_socket(77, SOCK_STREAM, 0);
		e = r < 0 ? sp_errno() : 0;
		sp_result("misc.eafnosupport", "socket() of an unknown family", PASSIF(r < 0 && (e == EAFNOSUPPORT ||
		          e == EPROTONOSUPPORT)), e, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
	}
}

/* ---- ipv6 ---- */
static void sin6_loop(struct sp_sockaddr_in6 *a, UWORD port)
{
	memset(a, 0, sizeof(*a));
	a->sin6_len = sizeof(*a);
	a->sin6_family = AF_INET6;
	a->sin6_port = port;
	a->sin6_addr[15] = 1;
}

void sp_test_ipv6(void)
{
	struct sp_sockaddr_in6 a;
	struct sp_sockaddr_in a4;
	LONG l, c, s, r, e, len, v;
	char buf[8];

	l = sp_socket(AF_INET6, SOCK_STREAM, 0);
	if (l < 0) {
		e = sp_errno();
		sp_result("ipv6.socket", "AF_INET6 socket", R_UNSUP, e, NULL);
		return;
	}
	sp_result("ipv6.socket", "AF_INET6 socket", R_PASS, 0, NULL);
	sin6_loop(&a, 0);
	r = sp_bind(l, (APTR)&a, sizeof(a));
	len = sizeof(a);
	if (r == 0)
		r = sp_getsockname(l, (APTR)&a, &len);
	if (r == 0)
		r = sp_listen(l, 5);
	sp_result("ipv6.bind_listen", "bind() and listen() on ::1", PASSIF(r == 0), r ? sp_errno() : 0, NULL);
	if (r == 0) {
		c = sp_socket(AF_INET6, SOCK_STREAM, 0);
		sp_nonblock(c, 1);
		r = sp_connect(c, (APTR)&a, sizeof(a));
		e = r < 0 ? sp_errno() : 0;
		s = -1;
		if ((r == 0 || e == EINPROGRESS) && sp_wait(l, 0, 2000) == 1) {
			struct sp_sockaddr_in6 p;
			len = sizeof(p);
			s = sp_accept(l, (APTR)&p, &len);
		}
		v = 0;
		if (s >= 0 && sp_wait(c, 1, 2000) == 1) {
			sp_nonblock(c, 0);
			sp_send(c, (APTR)"six", 3, 0);
			v = sp_recv_all(s, buf, 3, 2000);
		}
		sp_result("ipv6.tcp_loopback", "TCP over ::1 with data", PASSIF(v == 3 && !memcmp(buf, "six", 3)), e && e !=
		          EINPROGRESS ? e : 0, NULL);
		sp_close(&c);
		sp_close(&s);
	}
	sp_close(&l);
	/* IPV6_V6ONLY and IPv4-mapped connections to a dual-stack listener */
	l = sp_socket(AF_INET6, SOCK_STREAM, 0);
	v = 0;
	len = sizeof(v);
	r = sp_getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, (APTR)&v, &len);
	sp_result("ipv6.v6only", "IPV6_V6ONLY readable", r == 0 ? R_PASS : R_UNSUP, r ? sp_errno() : 0, "%ld", (long)v);
	v = 0;
	sp_setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, (APTR)&v, sizeof(v));
	memset(&a, 0, sizeof(a));
	a.sin6_len = sizeof(a);
	a.sin6_family = AF_INET6;
	r = sp_bind(l, (APTR)&a, sizeof(a));
	len = sizeof(a);
	if (r == 0)
		r = sp_getsockname(l, (APTR)&a, &len);
	if (r == 0)
		r = sp_listen(l, 5);
	if (r == 0) {
		c = sp_socket(AF_INET, SOCK_STREAM, 0);
		sp_nonblock(c, 1);
		sp_sin(&a4, INADDR_LOOPBACK, a.sin6_port);
		sp_connect(c, (APTR)&a4, sizeof(a4));
		s = sp_wait(l, 0, 2000) == 1 ? sp_accept(l, NULL, NULL) : -1;
		sp_result("ipv6.dual_stack", "IPv6 listener accepts an IPv4 client", PASSIF(s >= 0), 0, NULL);
		sp_close(&s);
		sp_close(&c);
	} else {
		sp_result("ipv6.dual_stack", "IPv6 listener accepts an IPv4 client", R_FAIL, sp_errno(), NULL);
	}
	sp_close(&l);
	/* UDP */
	l = sp_socket(AF_INET6, SOCK_DGRAM, 0);
	sin6_loop(&a, 0);
	r = l >= 0 ? sp_bind(l, (APTR)&a, sizeof(a)) : -1;
	len = sizeof(a);
	if (r == 0)
		r = sp_getsockname(l, (APTR)&a, &len);
	if (r == 0)
		r = sp_sendto(l, (APTR)"u6", 2, 0, (APTR)&a, sizeof(a)) == 2 ? 0 : -1;
	v = r == 0 && sp_wait(l, 0, 2000) == 1 ? sp_recv(l, buf, sizeof(buf), 0) : -1;
	sp_result("ipv6.udp_loopback", "UDP over ::1", PASSIF(v == 2), r ? sp_errno() : 0, NULL);
	sp_close(&l);
	/* names */
	if (SP_HAVE_MIAMI(gethostbyname2)) {
		struct sp_hostent *h = (struct sp_hostent *)sp_gethostbyname2((STRPTR)"localhost", AF_INET6);
		sp_result("ipv6.localhost", "gethostbyname2(\"localhost\", AF_INET6)", PASSIF(h && h->h_length == 16), 0, NULL);
	} else {
		sp_unsup_call("ipv6.localhost", "gethostbyname2(\"localhost\", AF_INET6)");
	}
}

/* ---- perf: loopback, so stack and CPU together ---- */
void sp_test_perf(void)
{
	LONG lst, c, s, r, total, sent, got, n, i;
	UWORD port;
	char *buf;
	ULONG t0, ms;
	struct sp_sockaddr_in a, b;

	buf = malloc(8192);
	if (!buf)
		return;
	memset(buf, 'p', 8192);
	/* TCP loopback throughput, both ends in this task */
	total = sp_quick ? 256L * 1024 : 2048L * 1024;
	if (pair("perf.tcp_loopback", "TCP loopback throughput", &lst, &port, &c, &s) == 0) {
		sp_nonblock(c, 1);
		sp_nonblock(s, 1);
		sent = got = 0;
		t0 = sp_now_ms();
		while (got < total && sp_now_ms() - t0 < 60000) {
			if (sent < total && (r = sp_send(c, buf, total - sent < 8192 ? total - sent : 8192, 0)) > 0)
				sent += r;
			while ((r = sp_recv(s, buf, 8192, 0)) > 0)
				got += r;
			if (r == 0)
				break;
		}
		ms = sp_now_ms() - t0;
		sp_result_ms("perf.tcp_loopback", "TCP loopback throughput", PASSIF(got == total), 0, ms, "%lu KB/s",
		             (unsigned long)(ms ? (ULONG)got / ms * 1000 / 1024 : 0));
		unpair(&lst, &c, &s);
	}
	/* connection setup */
	lst = sp_tcp_listener(&port);
	if (lst >= 0) {
		n = sp_quick ? 20 : 100;
		t0 = sp_now_ms();
		for (i = 0; i < n; i++) {
			if (sp_tcp_pair(&c, &s, lst, port) < 0)
				break;
			sp_close(&c);
			sp_close(&s);
		}
		ms = sp_now_ms() - t0;
		sp_result_ms("perf.connect_rate", "loopback connect+accept+close", PASSIF(i == n), 0, ms, "%lu per s",
		             (unsigned long)(ms ? (ULONG)i * 1000 / ms : 0));
		sp_close(&lst);
	}
	/* UDP datagrams */
	c = udp_bound(&a);
	s = udp_bound(&b);
	if (c >= 0 && s >= 0) {
		n = sp_quick ? 500 : 2000;
		got = 0;
		t0 = sp_now_ms();
		for (i = 0; i < n; i++) {
			if (sp_sendto(c, buf, 512, 0, (APTR)&b, sizeof(b)) == 512 && sp_wait(s, 0, 200) == 1 &&
			    sp_recv(s, buf, 8192, 0) == 512)
				got++;
		}
		ms = sp_now_ms() - t0;
		sp_result_ms("perf.udp_rate", "512-byte UDP send+receive", PASSIF(got > n * 9 / 10), 0, ms, "%lu per s",
		             (unsigned long)(ms ? (ULONG)got * 1000 / ms : 0));
	}
	sp_close(&c);
	sp_close(&s);
	/* the cost of one WaitSelect() poll */
	{
		struct sp_timeval tv = { 0, 0 };
		ULONG us;
		n = sp_quick ? 200 : 1000;
		t0 = sp_now_us();
		for (i = 0; i < n; i++)
			sp_WaitSelect(0, NULL, NULL, NULL, (APTR)&tv, NULL);
		us = sp_now_us() - t0;
		sp_result("perf.waitselect", "a WaitSelect() poll", R_INFO, 0, "%lu us", (unsigned long)(us / n));
	}
	free(buf);
}

/* ---- peer: a real path to stackprobe_peer.py ---- */
#define PEER_ECHO    7007
#define PEER_DISCARD 7009
#define PEER_CHARGEN 7019
#define PEER_CLOSED  7099

static LONG peer_connect(UWORD port, LONG *ms)
{
	struct sp_sockaddr_in a;
	LONG c = sp_socket(AF_INET, SOCK_STREAM, 0), r;
	ULONG t0 = sp_now_ms();

	if (c < 0)
		return -1;
	sp_nonblock(c, 1);
	sp_sin(&a, sp_peer_addr, port);
	r = sp_connect(c, (APTR)&a, sizeof(a));
	if ((r == 0 || sp_errno() == EINPROGRESS) && sp_wait(c, 1, 5000) == 1) {
		LONG err = 0, len = sizeof(err);
		sp_getsockopt(c, SOL_SOCKET, SO_ERROR, (APTR)&err, &len);
		if (!err) {
			sp_nonblock(c, 0);
			if (ms)
				*ms = sp_now_ms() - t0;
			return c;
		}
	}
	sp_CloseSocket(c);
	return -1;
}

void sp_test_peer(void)
{
	LONG c, ms = 0, r, total, n, i;
	char *buf;
	ULONG t0, el;
	struct sp_sockaddr_in a;

	if (!sp_peer || !sp_peer_addr || sp_peer_addr == 0xffffffffUL) {
		sp_result("peer.tcp_echo", "TCP echo with the peer", R_SKIP, 0, "no PEER=");
		return;
	}
	buf = malloc(8192);
	if (!buf)
		return;
	c = peer_connect(PEER_ECHO, &ms);
	if (c < 0) {
		sp_result("peer.tcp_echo", "TCP echo with the peer", R_FAIL, sp_errno(), "no connection");
	} else {
		ULONG best = 0xffffffffUL, us;
		for (i = 0; i < 10; i++) {
			us = sp_now_us();
			if (sp_send(c, (APTR)"ping", 4, 0) != 4 || sp_recv_all(c, buf, 4, 2000) != 4)
				break;
			us = sp_now_us() - us;
			if (us < best)
				best = us;
		}
		sp_result_ms("peer.tcp_echo", "TCP echo with the peer (best of 10)", PASSIF(i == 10), 0, ms, "%lu.%lu ms",
		             (unsigned long)(best / 1000), (unsigned long)(best / 100 % 10));
		sp_close(&c);
	}
	/* upload to discard */
	total = sp_quick ? 256L * 1024 : 1024L * 1024;
	memset(buf, 'u', 8192);
	c = peer_connect(PEER_DISCARD, NULL);
	if (c >= 0) {
		n = 0;
		t0 = sp_now_ms();
		while (n < total && (r = sp_send(c, buf, 8192, 0)) > 0)
			n += r;
		sp_shutdown(c, 1);
		sp_wait(c, 0, 10000);   /* the peer closes after reading all */
		el = sp_now_ms() - t0;
		sp_result_ms("peer.upload", "TCP upload to the peer", PASSIF(n >= total), 0, el, "%lu KB/s",
		             (unsigned long)(el ? (ULONG)n / el * 1000 / 1024 : 0));
		sp_close(&c);
	} else {
		sp_result("peer.upload", "TCP upload to the peer", R_FAIL, sp_errno(), "no connection");
	}
	/* download from chargen */
	c = peer_connect(PEER_CHARGEN, NULL);
	if (c >= 0) {
		n = 0;
		t0 = sp_now_ms();
		while (n < total && sp_wait(c, 0, 5000) == 1 && (r = sp_recv(c, buf, 8192, 0)) > 0)
			n += r;
		el = sp_now_ms() - t0;
		sp_result_ms("peer.download", "TCP download from the peer", PASSIF(n >= total), 0, el, "%lu KB/s",
		             (unsigned long)(el ? (ULONG)n / el * 1000 / 1024 : 0));
		sp_close(&c);
	} else {
		sp_result("peer.download", "TCP download from the peer", R_FAIL, sp_errno(), "no connection");
	}
	/* refused by the peer */
	c = sp_socket(AF_INET, SOCK_STREAM, 0);
	sp_sin(&a, sp_peer_addr, PEER_CLOSED);
	sp_watchdog(8000);
	r = sp_connect(c, (APTR)&a, sizeof(a));
	sp_watchdog(0);
	sp_result("peer.refused", "connect() to a closed port on the peer", PASSIF(r < 0 && sp_errno() == ECONNREFUSED),
	          r < 0 ? sp_errno() : 0, NULL);
	sp_close(&c);
	/* UDP echo */
	c = sp_socket(AF_INET, SOCK_DGRAM, 0);
	sp_sin(&a, sp_peer_addr, PEER_ECHO);
	r = -1;
	t0 = sp_now_us();
	if (sp_sendto(c, (APTR)"udp?", 4, 0, (APTR)&a, sizeof(a)) == 4 && sp_wait(c, 0, 2000) == 1)
		r = sp_recv(c, buf, 64, 0);
	el = sp_now_us() - t0;
	sp_result("peer.udp_echo", "UDP echo with the peer", PASSIF(r == 4), r < 0 ? sp_errno() : 0, "%lu.%lu ms",
	          (unsigned long)(el / 1000), (unsigned long)(el / 100 % 10));
	sp_close(&c);
	free(buf);
}
