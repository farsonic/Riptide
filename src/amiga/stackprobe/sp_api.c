/*
 * stackprobe - libraries present, every bsdsocket.library call once,
 * SocketBaseTagList() tags, the netdb calls, and the extension APIs
 * (Roadshow, miami.library, usergroup.library).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "stackprobe.h"

/* ---- libs ---- */
static struct Library *lib_check(const char *id, const char *name, const char *desc, int keep)
{
	struct Library *l = OpenLibrary((STRPTR)name, 0);

	if (!l) {
		sp_result(id, desc, R_UNSUP, 0, "not found");
		return NULL;
	}
	sp_result(id, desc, R_PASS, 0, "%u.%u", l->lib_Version, l->lib_Revision);
	if (!keep) {
		CloseLibrary(l);
		return NULL;
	}
	return l;
}

static void port_check(const char *id, const char *name)
{
	struct MsgPort *p;

	Forbid();
	p = FindPort((STRPTR)name);
	Permit();
	sp_result(id, name, R_INFO, 0, p ? "present" : "absent");
}

void sp_test_libraries(void)
{
	SocketBase = lib_check("libs.bsdsocket", "bsdsocket.library", "BSD sockets (AmiTCP API)", 1);
	UserGroupBase = lib_check("libs.usergroup", "usergroup.library", "users and groups (AmiTCP)", 1);
	MiamiBase = lib_check("libs.miami", "miami.library", "Miami extensions (IPv6 calls)", 1);
	lib_check("libs.socket", "socket.library", "AS225 / I-Net 225 sockets", 0);
	lib_check("libs.inet", "inet.library", "AS225r2 inet.library", 0);
	lib_check("libs.miamibpf", "miamibpf.library", "Miami packet filter", 0);
	lib_check("libs.miamipcap", "miamipcap.library", "Miami packet capture", 0);
	lib_check("libs.ifconfig", "ifconfig.library", "AmiTCP 4 interface config", 0);
	lib_check("libs.riptide", "riptide.library", "Riptide extensions", 0);
	port_check("libs.port_amitcp", "AMITCP");
	port_check("libs.port_miami", "MIAMI.1");
	port_check("libs.port_genesis", "GENESIS");
	port_check("libs.port_riptide", "RIPTIDE");
}

/* ---- api: every call of the standard table once ---- */
#define NEED(f, id, desc) if (!SP_HAVE_BSD(f)) { sp_unsup_call(id, desc); } else

static void ok_or_fail(const char *id, const char *desc, int ok, const char *fmt, const char *v)
{
	if (ok)
		sp_result(id, desc, R_PASS, 0, fmt, v);
	else
		sp_result(id, desc, R_FAIL, sp_errno(), fmt, v);
}

static void api_dgram(void)
{
	struct sp_sockaddr_in a, from;
	LONG u = sp_socket(AF_INET, SOCK_DGRAM, 0), len = sizeof(a), fl = sizeof(from), r;
	char buf[32], b1[8], b2[8];
	struct sp_iovec iov[2];
	struct sp_msghdr mh;

	if (u < 0) {
		sp_result("api.sendto", "sendto() UDP to itself", R_FAIL, sp_errno(), NULL);
		return;
	}
	sp_sin(&a, INADDR_LOOPBACK, 0);
	sp_bind(u, (APTR)&a, sizeof(a));
	sp_getsockname(u, (APTR)&a, &len);
	r = sp_sendto(u, (APTR)"probe", 5, 0, (APTR)&a, sizeof(a));
	ok_or_fail("api.sendto", "sendto() UDP to itself", r == 5, NULL, NULL);
	r = -1;
	if (sp_wait(u, 0, 2000) == 1)
		r = sp_recvfrom(u, buf, sizeof(buf), 0, (APTR)&from, &fl);
	ok_or_fail("api.recvfrom", "recvfrom() gives data and sender", r == 5 && from.sin_port == a.sin_port, NULL, NULL);
	NEED(sendmsg, "api.sendmsg", "sendmsg() with two buffers") {
		memset(&mh, 0, sizeof(mh));
		iov[0].iov_base = (APTR)"ab";
		iov[0].iov_len = 2;
		iov[1].iov_base = (APTR)"cde";
		iov[1].iov_len = 3;
		mh.msg_name = &a;
		mh.msg_namelen = sizeof(a);
		mh.msg_iov = iov;
		mh.msg_iovlen = 2;
		r = sp_sendmsg(u, (APTR)&mh, 0);
		ok_or_fail("api.sendmsg", "sendmsg() with two buffers", r == 5, NULL, NULL);
	}
	NEED(recvmsg, "api.recvmsg", "recvmsg() into two buffers") {
		memset(&mh, 0, sizeof(mh));
		memset(b1, 0, sizeof(b1));
		memset(b2, 0, sizeof(b2));
		iov[0].iov_base = b1;
		iov[0].iov_len = 2;
		iov[1].iov_base = b2;
		iov[1].iov_len = 6;
		mh.msg_name = &from;
		mh.msg_namelen = sizeof(from);
		mh.msg_iov = iov;
		mh.msg_iovlen = 2;
		r = -1;
		if (sp_wait(u, 0, 2000) == 1)
			r = sp_recvmsg(u, (APTR)&mh, 0);
		ok_or_fail("api.recvmsg", "recvmsg() into two buffers", r == 5 && !memcmp(b1, "ab", 2) && !memcmp(b2, "cde", 3),
		           NULL, NULL);
	}
	sp_CloseSocket(u);
}

void sp_test_api(void)
{
	UWORD port;
	LONG lst, c = -1, s = -1, r, len, v, e;
	struct sp_sockaddr_in a;
	char buf[64];
	static LONG my_errno;

	r = sp_socket(AF_INET, SOCK_STREAM, 0);
	ok_or_fail("api.socket", "socket() TCP", r >= 0, NULL, NULL);
	sp_close(&r);
	lst = sp_tcp_listener(&port);
	ok_or_fail("api.bind_listen", "bind(), listen(), getsockname()", lst >= 0, NULL, NULL);
	if (lst >= 0) {
		ok_or_fail("api.connect_accept", "connect() and accept() on loopback", sp_tcp_pair(&c, &s, lst, port) == 0, NULL, NULL);
	}
	if (c >= 0) {
		r = sp_send(c, (APTR)"hello", 5, 0);
		ok_or_fail("api.send", "send()", r == 5, NULL, NULL);
		r = sp_recv_all(s, buf, 5, 2000);
		ok_or_fail("api.recv", "recv()", r == 5 && !memcmp(buf, "hello", 5), NULL, NULL);
		len = sizeof(a);
		r = sp_getpeername(c, (APTR)&a, &len);
		ok_or_fail("api.getpeername", "getpeername()", r == 0 && a.sin_port == port, NULL, NULL);
		v = 1;
		r = sp_setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, (APTR)&v, sizeof(v));
		ok_or_fail("api.setsockopt", "setsockopt() SO_KEEPALIVE", r == 0, NULL, NULL);
		v = 0;
		len = sizeof(v);
		r = sp_getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, (APTR)&v, &len);
		ok_or_fail("api.getsockopt", "getsockopt() reads it back", r == 0 && v != 0, NULL, NULL);
		sp_send(c, (APTR)"xyz", 3, 0);
		sp_wait(s, 0, 2000);
		Delay(2);
		v = -1;
		r = sp_IoctlSocket(s, FIONREAD, (APTR)&v);
		sp_result("api.ioctl_fionread", "IoctlSocket() FIONREAD", r == 0 && v == 3 ? R_PASS : R_FAIL, r ? sp_errno() : 0,
		          "%ld", (long)v);
		sp_recv_all(s, buf, 3, 1000);
		r = sp_shutdown(c, 1);
		ok_or_fail("api.shutdown", "shutdown() then the peer reads EOF", r == 0 && sp_wait(s, 0, 2000) == 1 &&
		           sp_recv(s, buf, sizeof(buf), 0) == 0, NULL, NULL);
	}
	r = c >= 0 ? sp_CloseSocket(c) : -1;
	c = -1;
	ok_or_fail("api.closesocket", "CloseSocket()", r == 0, NULL, NULL);
	sp_close(&s);
	sp_close(&lst);
	api_dgram();

	NEED(WaitSelect, "api.waitselect", "WaitSelect() with a timeout") {
		struct sp_timeval tv = { 0, 100000 };
		r = sp_WaitSelect(0, NULL, NULL, NULL, (APTR)&tv, NULL);
		ok_or_fail("api.waitselect", "WaitSelect() with a timeout", r == 0, NULL, NULL);
	}
	NEED(getdtablesize, "api.getdtablesize", "getdtablesize()") {
		r = sp_getdtablesize();
		sp_result("api.getdtablesize", "getdtablesize()", r > 0 ? R_PASS : R_FAIL, 0, "%ld", (long)r);
	}
	NEED(SetSocketSignals, "api.setsocketsignals", "SetSocketSignals()") {
		sp_SetSocketSignals(SIGBREAKF_CTRL_C, 0, 0);
		sp_result("api.setsocketsignals", "SetSocketSignals()", R_PASS, 0, NULL);
	}
	NEED(ReleaseSocket, "api.release_obtain", "ReleaseSocket()/ObtainSocket()") {
		LONG id;
		s = sp_socket(AF_INET, SOCK_STREAM, 0);
		id = sp_ReleaseSocket(s, -1);
		r = -1;
		if (id != -1 && SP_HAVE_BSD(ObtainSocket))
			r = sp_ObtainSocket(id, AF_INET, SOCK_STREAM, 0);
		ok_or_fail("api.release_obtain", "ReleaseSocket()/ObtainSocket()", id != -1 && r >= 0, NULL, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
		else if (id == -1)
			sp_CloseSocket(s);
	}
	NEED(ReleaseCopyOfSocket, "api.releasecopy", "ReleaseCopyOfSocket()") {
		LONG id;
		s = sp_socket(AF_INET, SOCK_DGRAM, 0);
		id = sp_ReleaseCopyOfSocket(s, -1);
		r = id != -1 ? sp_ObtainSocket(id, AF_INET, SOCK_DGRAM, 0) : -1;
		ok_or_fail("api.releasecopy", "ReleaseCopyOfSocket()", id != -1 && r >= 0, NULL, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
		sp_CloseSocket(s);
	}
	NEED(Dup2Socket, "api.dup2socket", "Dup2Socket() to a new descriptor") {
		s = sp_socket(AF_INET, SOCK_STREAM, 0);
		r = sp_Dup2Socket(s, -1);
		ok_or_fail("api.dup2socket", "Dup2Socket() to a new descriptor", r >= 0 && r != s, NULL, NULL);
		if (r >= 0)
			sp_CloseSocket(r);
		sp_CloseSocket(s);
	}
	NEED(Errno, "api.errno", "Errno() after a bad call") {
		sp_CloseSocket(4000);
		e = sp_Errno();
		sp_result("api.errno", "Errno() after a bad call", e == EBADF || e == ENOTSOCK ? R_PASS : R_FAIL, 0, "%s",
		          sp_errname(e));
	}
	NEED(SetErrnoPtr, "api.seterrnoptr", "SetErrnoPtr() then a bad call") {
		my_errno = 0;
		sp_SetErrnoPtr(&my_errno, sizeof(my_errno));
		sp_CloseSocket(4000);
		sp_result("api.seterrnoptr", "SetErrnoPtr() then a bad call", my_errno ? R_PASS : R_FAIL, 0, "%s",
		          sp_errname(my_errno));
	}
	NEED(Inet_NtoA, "api.inet_ntoa", "Inet_NtoA(127.0.0.1)") {
		STRPTR p = sp_Inet_NtoA(INADDR_LOOPBACK);
		sp_result("api.inet_ntoa", "Inet_NtoA(127.0.0.1)", p && !strcmp((char *)p, "127.0.0.1") ? R_PASS : R_FAIL, 0, NULL);
	}
	NEED(inet_addr, "api.inet_addr", "inet_addr(\"10.1.2.3\")") {
		sp_result("api.inet_addr", "inet_addr(\"10.1.2.3\")", sp_inet_addr((STRPTR)"10.1.2.3") == 0x0a010203UL ? R_PASS : R_FAIL,
		          0, NULL);
	}
	NEED(Inet_LnaOf, "api.inet_lnaof", "Inet_LnaOf/NetOf/MakeAddr") {
		ULONG lna = sp_Inet_LnaOf(0x0a010203UL), net = SP_HAVE_BSD(Inet_NetOf) ? sp_Inet_NetOf(0x0a010203UL) : 0;
		ULONG mk = SP_HAVE_BSD(Inet_MakeAddr) ? sp_Inet_MakeAddr(net, lna) : 0;
		sp_result("api.inet_lnaof", "Inet_LnaOf/NetOf/MakeAddr", lna == 0x010203UL && net == 10 && mk == 0x0a010203UL ?
		          R_PASS : R_FAIL, 0, NULL);
	}
	NEED(inet_network, "api.inet_network", "inet_network(\"10.1\")") {
		ULONG n = sp_inet_network((STRPTR)"10.1");
		sp_result("api.inet_network", "inet_network(\"10.1\")", n == 0x0a01UL ? R_PASS : R_FAIL, 0, "%08lx", (unsigned long)n);
	}
	NEED(gethostname, "api.gethostname", "gethostname()") {
		buf[0] = 0;
		r = sp_gethostname((STRPTR)buf, sizeof(buf));
		buf[sizeof(buf) - 1] = 0;
		sp_result("api.gethostname", "gethostname()", r == 0 ? R_PASS : R_FAIL, r ? sp_errno() : 0, "%s", buf);
	}
	NEED(gethostid, "api.gethostid", "gethostid()") {
		sp_result("api.gethostid", "gethostid()", R_PASS, 0, "%08lx", (unsigned long)sp_gethostid());
	}
	NEED(vsyslog, "api.vsyslog", "vsyslog() a debug message") {
		sp_vsyslog(7, (STRPTR)"stackprobe: vsyslog test", NULL);
		sp_result("api.vsyslog", "vsyslog() a debug message", R_PASS, 0, NULL);
	}
	NEED(SocketBaseTagList, "api.socketbasetaglist", "SocketBaseTagList() DTABLESIZE") {
		ULONG t[3] = { SBTM_GETVAL(SBTC_DTABLESIZE), 0, TAG_DONE };
		r = sp_SocketBaseTagList((APTR)t);
		sp_result("api.socketbasetaglist", "SocketBaseTagList() DTABLESIZE", r == 0 ? R_PASS : R_FAIL, 0, "%lu",
		          (unsigned long)t[1]);
	}
	NEED(GetSocketEvents, "api.getsocketevents", "GetSocketEvents() on accept") {
		ULONG t[3] = { SBTM_SETVAL(SBTC_SIGEVENTMASK), 0, TAG_DONE };
		BYTE sig = AllocSignal(-1);
		ULONG ev = 0;
		LONG got = -1, mask = 0x01 | 0x08; /* FD_ACCEPT | FD_READ */
		lst = sp_tcp_listener(&port);
		t[1] = sig >= 0 ? 1UL << sig : 0;
		if (sig >= 0 && lst >= 0 && !sp_SocketBaseTagList((APTR)t) &&
		    !sp_setsockopt(lst, SOL_SOCKET, SO_EVENTMASK, (APTR)&mask, sizeof(mask))) {
			c = sp_socket(AF_INET, SOCK_STREAM, 0);
			sp_nonblock(c, 1);
			sp_sin(&a, INADDR_LOOPBACK, port);
			sp_connect(c, (APTR)&a, sizeof(a));
			Delay(10);
			got = sp_GetSocketEvents(&ev);
			sp_result("api.getsocketevents", "GetSocketEvents() on accept", got == lst && (ev & 1) ? R_PASS : R_FAIL, 0,
			          "socket %ld events %lx", (long)got, (unsigned long)ev);
			sp_close(&c);
			t[1] = 0;
			sp_SocketBaseTagList((APTR)t);
		} else {
			sp_result("api.getsocketevents", "GetSocketEvents() on accept", R_UNSUP, sp_errno(), "no SO_EVENTMASK");
		}
		sp_close(&lst);
		if (sig >= 0)
			FreeSignal(sig);
	}
}

/* ---- tags: every SocketBaseTagList() code ---- */
static LONG tag_get(ULONG code, ULONG in, ULONG *out)
{
	ULONG t[3];
	t[0] = SBTM_GETVAL(code);
	t[1] = in;
	t[2] = TAG_DONE;
	if (sp_SocketBaseTagList((APTR)t))
		return -1;
	*out = t[1];
	return 0;
}

static void tag_str(const char *id, const char *desc, ULONG code, ULONG arg)
{
	ULONG v;
	if (tag_get(code, arg, &v) < 0 || !v)
		sp_result(id, desc, R_UNSUP, 0, NULL);
	else
		sp_result(id, desc, R_PASS, 0, "\"%.40s\"", (char *)v);
}

static void tag_val(const char *id, const char *desc, ULONG code)
{
	ULONG v;
	if (tag_get(code, 0, &v) < 0)
		sp_result(id, desc, R_UNSUP, 0, NULL);
	else
		sp_result(id, desc, R_PASS, 0, "%lx", (unsigned long)v);
}

void sp_test_tags(void)
{
	ULONG v, old, t[3];
	static LONG e4;
	static UBYTE e1;
	static UWORD e2;
	static LONG he;

	if (!SP_HAVE_BSD(SocketBaseTagList)) {
		sp_unsup_call("tags.socketbasetaglist", "SocketBaseTagList()");
		return;
	}
	tag_val("tags.breakmask", "SBTC_BREAKMASK get", SBTC_BREAKMASK);
	tag_val("tags.sigiomask", "SBTC_SIGIOMASK get", SBTC_SIGIOMASK);
	tag_val("tags.sigurgmask", "SBTC_SIGURGMASK get", SBTC_SIGURGMASK);
	tag_val("tags.sigeventmask", "SBTC_SIGEVENTMASK get (AmiTCP 4)", SBTC_SIGEVENTMASK);
	tag_val("tags.errno", "SBTC_ERRNO get", SBTC_ERRNO);
	tag_val("tags.herrno", "SBTC_HERRNO get", SBTC_HERRNO);
	tag_val("tags.dtablesize", "SBTC_DTABLESIZE get", SBTC_DTABLESIZE);
	tag_val("tags.fdcallback", "SBTC_FDCALLBACK get", SBTC_FDCALLBACK);
	tag_val("tags.logstat", "SBTC_LOGSTAT get", SBTC_LOGSTAT);
	tag_val("tags.logtagptr", "SBTC_LOGTAGPTR get", SBTC_LOGTAGPTR);
	tag_val("tags.logfacility", "SBTC_LOGFACILITY get", SBTC_LOGFACILITY);
	tag_val("tags.logmask", "SBTC_LOGMASK get", SBTC_LOGMASK);
	tag_str("tags.errnostrptr", "SBTC_ERRNOSTRPTR (ECONNREFUSED)", SBTC_ERRNOSTRPTR, ECONNREFUSED);
	tag_str("tags.herrnostrptr", "SBTC_HERRNOSTRPTR (1)", SBTC_HERRNOSTRPTR, 1);
	tag_str("tags.ioerrnostrptr", "SBTC_IOERRNOSTRPTR (-1)", SBTC_IOERRNOSTRPTR, (ULONG)-1);
	tag_str("tags.s2errnostrptr", "SBTC_S2ERRNOSTRPTR (1)", SBTC_S2ERRNOSTRPTR, 1);
	tag_str("tags.s2werrnostrptr", "SBTC_S2WERRNOSTRPTR (1)", SBTC_S2WERRNOSTRPTR, 1);
	tag_str("tags.releasestrptr", "SBTC_RELEASESTRPTR", SBTC_RELEASESTRPTR, 0);
	tag_val("tags.compat43", "SBTC_COMPAT43 get", SBTC_COMPAT43);

	/* set and read back, then restore */
	if (tag_get(SBTC_BREAKMASK, 0, &old) == 0) {
		t[0] = SBTM_SETVAL(SBTC_BREAKMASK);
		t[1] = SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D;
		t[2] = TAG_DONE;
		sp_SocketBaseTagList((APTR)t);
		v = 0;
		tag_get(SBTC_BREAKMASK, 0, &v);
		sp_result("tags.set_breakmask", "SBTC_BREAKMASK set and read back", v == (SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D) ?
		          R_PASS : R_FAIL, 0, "%lx", (unsigned long)v);
		t[1] = old;
		sp_SocketBaseTagList((APTR)t);
	}
	if (tag_get(SBTC_DTABLESIZE, 0, &old) == 0) {
		t[0] = SBTM_SETVAL(SBTC_DTABLESIZE);
		t[1] = old + 64;
		t[2] = TAG_DONE;
		sp_SocketBaseTagList((APTR)t);
		v = 0;
		tag_get(SBTC_DTABLESIZE, 0, &v);
		sp_result("tags.set_dtablesize", "SBTC_DTABLESIZE raised by 64", v >= old + 64 ? R_PASS : R_FAIL, 0, "%lu -> %lu",
		          (unsigned long)old, (unsigned long)v);
	}
	/* errno pointers of other sizes */
	t[0] = SBTM_SETVAL(SBTC_ERRNOBYTEPTR);
	t[1] = (ULONG)&e1;
	t[2] = TAG_DONE;
	if (!sp_SocketBaseTagList((APTR)t)) {
		e1 = 0;
		sp_CloseSocket(4000);
		sp_result("tags.errnobyteptr", "SBTC_ERRNOBYTEPTR gets the error", e1 ? R_PASS : R_FAIL, 0, "%u", e1);
	} else {
		sp_result("tags.errnobyteptr", "SBTC_ERRNOBYTEPTR gets the error", R_UNSUP, 0, NULL);
	}
	t[0] = SBTM_SETVAL(SBTC_ERRNOWORDPTR);
	t[1] = (ULONG)&e2;
	if (!sp_SocketBaseTagList((APTR)t)) {
		e2 = 0;
		sp_CloseSocket(4000);
		sp_result("tags.errnowordptr", "SBTC_ERRNOWORDPTR gets the error", e2 ? R_PASS : R_FAIL, 0, "%u", e2);
	} else {
		sp_result("tags.errnowordptr", "SBTC_ERRNOWORDPTR gets the error", R_UNSUP, 0, NULL);
	}
	t[0] = SBTM_SETVAL(SBTC_ERRNOLONGPTR);
	t[1] = (ULONG)&e4;
	if (!sp_SocketBaseTagList((APTR)t)) {
		e4 = 0;
		sp_CloseSocket(4000);
		sp_result("tags.errnolongptr", "SBTC_ERRNOLONGPTR gets the error", e4 ? R_PASS : R_FAIL, 0, "%ld", (long)e4);
	} else {
		sp_result("tags.errnolongptr", "SBTC_ERRNOLONGPTR gets the error", R_UNSUP, 0, NULL);
	}
	t[0] = SBTM_SETVAL(SBTC_HERRNOLONGPTR);
	t[1] = (ULONG)&he;
	if (!sp_SocketBaseTagList((APTR)t)) {
		he = 0;
		sp_gethostbyname((STRPTR)"nonexistent.invalid");
		sp_result("tags.herrnolongptr", "SBTC_HERRNOLONGPTR after a failed lookup", he ? R_PASS : R_FAIL, 0, "%ld",
		          (long)he);
	} else {
		sp_result("tags.herrnolongptr", "SBTC_HERRNOLONGPTR after a failed lookup", R_UNSUP, 0, NULL);
	}
}

/* ---- netdb ---- */
void sp_test_netdb(void)
{
	struct sp_hostent *h;
	struct sp_servent *se;
	struct sp_protoent *pe;
	struct sp_netent *ne;
	ULONG a = INADDR_LOOPBACK, t0;
	char name[64];

	t0 = sp_now_ms();
	h = (struct sp_hostent *)sp_gethostbyname((STRPTR)"localhost");
	sp_result_ms("netdb.localhost", "gethostbyname(\"localhost\")", h && h->h_addr_list && h->h_addr_list[0] &&
	             !memcmp(h->h_addr_list[0], &a, 4) ? R_PASS : R_FAIL, 0, sp_now_ms() - t0, "%s",
	             h ? (char *)h->h_name : "none");
	h = (struct sp_hostent *)sp_gethostbyname((STRPTR)"127.0.0.1");
	sp_result("netdb.numeric", "gethostbyname(\"127.0.0.1\")", h && h->h_addr_list && h->h_addr_list[0] &&
	          !memcmp(h->h_addr_list[0], &a, 4) ? R_PASS : R_FAIL, 0, NULL);
	t0 = sp_now_ms();
	h = (struct sp_hostent *)sp_gethostbyaddr((APTR)&a, 4, AF_INET);
	sp_result_ms("netdb.reverse", "gethostbyaddr(127.0.0.1)", h && h->h_name ? R_PASS : R_FAIL, 0, sp_now_ms() - t0,
	             "%s", h && h->h_name ? (char *)h->h_name : "none");
	name[0] = 0;
	if (SP_HAVE_BSD(gethostname) && !sp_gethostname((STRPTR)name, sizeof(name)) && name[0]) {
		h = (struct sp_hostent *)sp_gethostbyname((STRPTR)name);
		sp_result("netdb.own_name", "gethostbyname(own host name)", R_INFO, 0, "%s %s", name, h ? "found" : "not found");
	}
	t0 = sp_now_ms();
	h = (struct sp_hostent *)sp_gethostbyname((STRPTR)"nonexistent.invalid");
	sp_result_ms("netdb.nxdomain", "lookup of a name that does not exist", !h ? R_PASS : R_FAIL, 0, sp_now_ms() - t0,
	             NULL);
	if (sp_peer) {
		t0 = sp_now_ms();
		h = (struct sp_hostent *)sp_gethostbyname((STRPTR)"aminet.net");
		sp_result_ms("netdb.dns", "gethostbyname(\"aminet.net\") (DNS)", h ? R_PASS : R_FAIL, 0, sp_now_ms() - t0,
		             NULL);
	} else {
		sp_result("netdb.dns", "gethostbyname(\"aminet.net\") (DNS)", R_SKIP, 0, "needs PEER (a network)");
	}
	se = (struct sp_servent *)sp_getservbyname((STRPTR)"ftp", (STRPTR)"tcp");
	sp_result("netdb.getservbyname", "getservbyname(\"ftp\", \"tcp\")", se && se->s_port == 21 ? R_PASS : R_FAIL, 0,
	          "%ld", se ? (long)se->s_port : -1L);
	se = (struct sp_servent *)sp_getservbyport(21, (STRPTR)"tcp");
	sp_result("netdb.getservbyport", "getservbyport(21, \"tcp\")", se && se->s_name ? R_PASS : R_FAIL, 0, "%s",
	          se && se->s_name ? (char *)se->s_name : "none");
	pe = (struct sp_protoent *)sp_getprotobyname((STRPTR)"udp");
	sp_result("netdb.getprotobyname", "getprotobyname(\"udp\")", pe && pe->p_proto == 17 ? R_PASS : R_FAIL, 0, NULL);
	pe = (struct sp_protoent *)sp_getprotobynumber(6);
	sp_result("netdb.getprotobynumber", "getprotobynumber(6)", pe && pe->p_name && !sp_strnicmp((char *)pe->p_name, "tcp", 4) ?
	          R_PASS : R_FAIL, 0, "%s", pe && pe->p_name ? (char *)pe->p_name : "none");
	ne = (struct sp_netent *)sp_getnetbyname((STRPTR)"loopback");
	sp_result("netdb.getnetbyname", "getnetbyname(\"loopback\")", ne ? R_PASS : R_FAIL, 0, NULL);
	ne = (struct sp_netent *)sp_getnetbyaddr(127, AF_INET);
	sp_result("netdb.getnetbyaddr", "getnetbyaddr(127)", ne ? R_PASS : R_FAIL, 0, "%s",
	          ne && ne->n_name ? (char *)ne->n_name : "none");
}

/* ---- ext: Roadshow, miami.library, usergroup.library ---- */
#define NEED_RS(f, id, desc) if (!rs || !SP_HAVE_BSD(f)) { sp_unsup_call(id, desc); } else
#define NEED_MI(f, id, desc) if (!SP_HAVE_MIAMI(f)) { sp_unsup_call(id, desc); } else
#define NEED_UG(f, id, desc) if (!SP_HAVE_UG(f)) { sp_unsup_call(id, desc); } else

static LONG list_len(struct List *l)
{
	struct Node *n;
	LONG c = 0;
	if (!l)
		return -1;
	for (n = l->lh_Head; n && n->ln_Succ; n = n->ln_Succ)
		c++;
	return c;
}

static void ext_roadshow(void)
{
	int rs = SocketBase->lib_Version >= 4;
	UBYTE a4[4], a16[16];
	char txt[48];

	NEED_RS(ObtainInterfaceList, "ext.rs_interfaces", "ObtainInterfaceList() (Roadshow)") {
		struct List *l = (struct List *)sp_ObtainInterfaceList();
		sp_result("ext.rs_interfaces", "ObtainInterfaceList() (Roadshow)", l ? R_PASS : R_FAIL, sp_errno(), "%ld",
		          (long)list_len(l));
		if (l && SP_HAVE_BSD(ReleaseInterfaceList))
			sp_ReleaseInterfaceList(l);
	}
	NEED_RS(GetRouteInfo, "ext.rs_routes", "GetRouteInfo() (Roadshow)") {
		APTR r = sp_GetRouteInfo(AF_INET, 0);
		sp_result("ext.rs_routes", "GetRouteInfo() (Roadshow)", r ? R_PASS : R_FAIL, sp_errno(), NULL);
		if (r && SP_HAVE_BSD(FreeRouteInfo))
			sp_FreeRouteInfo(r);
	}
	NEED_RS(ObtainDomainNameServerList, "ext.rs_dns", "ObtainDomainNameServerList()") {
		struct List *l = (struct List *)sp_ObtainDomainNameServerList();
		sp_result("ext.rs_dns", "ObtainDomainNameServerList()", l ? R_PASS : R_FAIL, sp_errno(), "%ld",
		          (long)list_len(l));
		if (l && SP_HAVE_BSD(ReleaseDomainNameServerList))
			sp_ReleaseDomainNameServerList(l);
	}
	NEED_RS(getprotoent, "ext.rs_getprotoent", "setprotoent()/getprotoent()") {
		LONG n = 0;
		sp_setprotoent(1);
		while (n < 500 && sp_getprotoent())
			n++;
		sp_endprotoent();
		sp_result("ext.rs_getprotoent", "setprotoent()/getprotoent()", n > 0 ? R_PASS : R_FAIL, 0, "%ld", (long)n);
	}
	NEED_RS(getservent, "ext.rs_getservent", "setservent()/getservent()") {
		LONG n = 0;
		sp_setservent(1);
		while (n < 2000 && sp_getservent())
			n++;
		sp_endservent();
		sp_result("ext.rs_getservent", "setservent()/getservent()", n > 0 ? R_PASS : R_FAIL, 0, "%ld", (long)n);
	}
	NEED_RS(getnetent, "ext.rs_getnetent", "setnetent()/getnetent()") {
		LONG n = 0;
		sp_setnetent(1);
		while (n < 500 && sp_getnetent())
			n++;
		sp_endnetent();
		sp_result("ext.rs_getnetent", "setnetent()/getnetent()", R_PASS, 0, "%ld", (long)n);
	}
	NEED_RS(rs_inet_aton, "ext.rs_inet_aton", "inet_aton() (Roadshow)") {
		LONG r = sp_rs_inet_aton((STRPTR)"192.168.1.2", a4);
		sp_result("ext.rs_inet_aton", "inet_aton() (Roadshow)", r && a4[0] == 192 && a4[3] == 2 ? R_PASS : R_FAIL, 0, NULL);
	}
	NEED_RS(rs_inet_pton, "ext.rs_inet_pton", "inet_pton()/inet_ntop() IPv4 (Roadshow)") {
		LONG r = sp_rs_inet_pton(AF_INET, (STRPTR)"192.168.7.9", a16);
		STRPTR p = r > 0 && SP_HAVE_BSD(rs_inet_ntop) ? sp_rs_inet_ntop(AF_INET, a16, (STRPTR)txt, sizeof(txt)) : NULL;
		sp_result("ext.rs_inet_pton", "inet_pton()/inet_ntop() IPv4 (Roadshow)", p && !strcmp(txt, "192.168.7.9") ?
		          R_PASS : R_FAIL, 0, "%s", p ? txt : "none");
	}
	NEED_RS(In_LocalAddr, "ext.rs_in_localaddr", "In_LocalAddr(127.0.0.1)") {
		sp_result("ext.rs_in_localaddr", "In_LocalAddr(127.0.0.1)", R_PASS, 0, "%ld", (long)sp_In_LocalAddr(INADDR_LOOPBACK));
	}
	NEED_RS(ProcessIsServer, "ext.rs_processisserver", "ProcessIsServer() (inetd API)") {
		sp_result("ext.rs_processisserver", "ProcessIsServer() (inetd API)", R_PASS, 0, "%ld",
		          (long)sp_ProcessIsServer(FindTask(NULL)));
	}
	NEED_RS(bpf_open, "ext.rs_bpf", "bpf_open()/bpf_close() (packet filter)") {
		LONG ch = sp_bpf_open(0);
		LONG e = ch < 0 ? sp_errno() : 0;
		/* ENXIO: no packet filter channel (the documented answer of a stack without one) */
		sp_result("ext.rs_bpf", "bpf_open()/bpf_close() (packet filter)", ch >= 0 ? R_PASS : e == 6 ? R_UNSUP : R_FAIL, e,
		          NULL);
		if (ch >= 0 && SP_HAVE_BSD(bpf_close))
			sp_bpf_close(ch);
	}
}

static void ext_miami(void)
{
	UBYTE a16[16];
	char txt[64], serv[16];

	if (!MiamiBase) {
		sp_result("ext.miami", "miami.library", R_UNSUP, 0, "not found");
		return;
	}
	NEED_MI(MiamiSupportsIPV6, "ext.mi_supportsipv6", "MiamiSupportsIPV6()") {
		sp_result("ext.mi_supportsipv6", "MiamiSupportsIPV6()", R_PASS, 0, "%ld", (long)sp_MiamiSupportsIPV6());
	}
	NEED_MI(MiamiGetPid, "ext.mi_getpid", "MiamiGetPid()") {
		sp_result("ext.mi_getpid", "MiamiGetPid()", sp_MiamiGetPid() ? R_PASS : R_FAIL, 0, NULL);
	}
	NEED_MI(MiamiIsOnline, "ext.mi_isonline", "MiamiIsOnline(NULL)") {
		sp_result("ext.mi_isonline", "MiamiIsOnline(NULL)", R_PASS, 0, "%ld", (long)sp_MiamiIsOnline(NULL));
	}
	NEED_MI(MiamiGetHardwareLen, "ext.mi_gethardwarelen", "MiamiGetHardwareLen(\"lo0\")") {
		sp_result("ext.mi_gethardwarelen", "MiamiGetHardwareLen(\"lo0\")", R_PASS, 0, "%ld",
		          (long)sp_MiamiGetHardwareLen((STRPTR)"lo0"));
	}
	NEED_MI(MiamiResGetOptions, "ext.mi_resgetoptions", "MiamiResGetOptions()") {
		sp_result("ext.mi_resgetoptions", "MiamiResGetOptions()", R_PASS, 0, "%lx", (unsigned long)sp_MiamiResGetOptions());
	}
	NEED_MI(MiamiSupportedCPUs, "ext.mi_supportedcpus", "MiamiSupportedCPUs()") {
		ULONG apis = 0, cbs = 0, kern = 0;
		sp_MiamiSupportedCPUs(&apis, &cbs, &kern);
		sp_result("ext.mi_supportedcpus", "MiamiSupportedCPUs()", R_PASS, 0, "%lx %lx %lx", (unsigned long)apis,
		          (unsigned long)cbs, (unsigned long)kern);
	}
	NEED_MI(inet_pton, "ext.mi_inet_pton", "inet_pton()/inet_ntop() IPv6") {
		LONG r = sp_inet_pton(AF_INET6, (STRPTR)"2001:db8::1", a16);
		STRPTR p = r > 0 && SP_HAVE_MIAMI(inet_ntop) ? sp_inet_ntop(AF_INET6, a16, (STRPTR)txt, sizeof(txt)) : NULL;
		sp_result("ext.mi_inet_pton", "inet_pton()/inet_ntop() IPv6", p && !strcmp(txt, "2001:db8::1") ? R_PASS : R_FAIL,
		          0, "%s", p ? txt : "none");
	}
	NEED_MI(inet_aton, "ext.mi_inet_aton", "inet_aton()") {
		UBYTE a4[4];
		LONG r = sp_inet_aton((STRPTR)"10.0.0.7", a4);
		sp_result("ext.mi_inet_aton", "inet_aton()", r && a4[0] == 10 && a4[3] == 7 ? R_PASS : R_FAIL, 0, NULL);
	}
	NEED_MI(getaddrinfo, "ext.mi_getaddrinfo", "getaddrinfo(\"localhost\", \"21\")") {
		struct sp_addrinfo hints, *res = NULL, *p;
		LONG r, n = 0, fams = 0;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_STREAM;
		r = sp_getaddrinfo((STRPTR)"localhost", (STRPTR)"21", &hints, (APTR *)&res);
		for (p = res; p && n < 16; p = p->ai_next, n++)
			fams |= p->ai_family == AF_INET ? 1 : p->ai_family == AF_INET6 ? 2 : 4;
		sp_result("ext.mi_getaddrinfo", "getaddrinfo(\"localhost\", \"21\")", r == 0 && n ? R_PASS : R_FAIL, r, "%ld results%s%s",
		          (long)n, fams & 1 ? " IPv4" : "", fams & 2 ? " IPv6" : "");
		if (res && SP_HAVE_MIAMI(freeaddrinfo))
			sp_freeaddrinfo(res);
	}
	NEED_MI(getnameinfo, "ext.mi_getnameinfo", "getnameinfo(127.0.0.1:21)") {
		struct sp_sockaddr_in sa;
		LONG r;
		sp_sin(&sa, INADDR_LOOPBACK, 21);
		txt[0] = serv[0] = 0;
		r = sp_getnameinfo(&sa, sizeof(sa), (STRPTR)txt, sizeof(txt), (STRPTR)serv, sizeof(serv), 0);
		sp_result("ext.mi_getnameinfo", "getnameinfo(127.0.0.1:21)", r == 0 ? R_PASS : R_FAIL, r, "%s %s", txt, serv);
	}
	NEED_MI(gai_strerror, "ext.mi_gai_strerror", "gai_strerror()") {
		STRPTR s = sp_gai_strerror(8);
		sp_result("ext.mi_gai_strerror", "gai_strerror()", s ? R_PASS : R_FAIL, 0, "%.30s", s ? (char *)s : "");
	}
	NEED_MI(gethostbyname2, "ext.mi_gethostbyname2", "gethostbyname2(\"localhost\", AF_INET)") {
		struct sp_hostent *h = (struct sp_hostent *)sp_gethostbyname2((STRPTR)"localhost", AF_INET);
		sp_result("ext.mi_gethostbyname2", "gethostbyname2(\"localhost\", AF_INET)", h ? R_PASS : R_FAIL, 0, NULL);
	}
	NEED_MI(if_nameindex, "ext.mi_if_nameindex", "if_nameindex() and friends") {
		struct { ULONG idx; STRPTR name; } *ni = sp_if_nameindex(), *p;
		LONG n = 0, ok = 1;
		char nm[32];
		for (p = (void *)ni; p && p->idx && n < 64; p++, n++)
			if (SP_HAVE_MIAMI(if_nametoindex) && sp_if_nametoindex(p->name) != (LONG)p->idx)
				ok = 0;
			else if (SP_HAVE_MIAMI(if_indextoname) && (!sp_if_indextoname(p->idx, (STRPTR)nm) || strcmp(nm, (char *)p->name)))
				ok = 0;
		sp_result("ext.mi_if_nameindex", "if_nameindex() and friends", ni && n && ok ? R_PASS : R_FAIL, 0, "%ld interfaces",
		          (long)n);
		if (ni && SP_HAVE_MIAMI(if_freenameindex))
			sp_if_freenameindex(ni);
	}
}

static void ext_usergroup(void)
{
	struct sp_passwd *pw;
	struct sp_group *gr;
	STRPTR s;
	LONG n, gs[16];

	if (!UserGroupBase) {
		sp_result("ext.usergroup", "usergroup.library", R_UNSUP, 0, "not found");
		return;
	}
	NEED_UG(ug_SetupContextTagList, "ext.ug_context", "ug_SetupContextTagList()") {
		ULONG t[1] = { TAG_DONE };
		n = sp_ug_SetupContextTagList((STRPTR)"stackprobe", (APTR)t);
		sp_result("ext.ug_context", "ug_SetupContextTagList()", n == 0 ? R_PASS : R_FAIL, n, NULL);
	}
	NEED_UG(getuid, "ext.ug_ids", "getuid/geteuid/getgid/getegid") {
		sp_result("ext.ug_ids", "getuid/geteuid/getgid/getegid", R_PASS, 0, "%ld %ld %ld %ld", (long)sp_getuid(),
		          (long)sp_geteuid(), (long)sp_getgid(), (long)sp_getegid());
	}
	NEED_UG(getgroups, "ext.ug_getgroups", "getgroups()") {
		n = sp_getgroups(16, gs);
		sp_result("ext.ug_getgroups", "getgroups()", n >= 0 ? R_PASS : R_FAIL, 0, "%ld", (long)n);
	}
	NEED_UG(getpwuid, "ext.ug_getpwuid", "getpwuid(own uid)") {
		pw = (struct sp_passwd *)sp_getpwuid(sp_getuid());
		sp_result("ext.ug_getpwuid", "getpwuid(own uid)", pw ? R_PASS : R_FAIL, 0, "%s",
		          pw && pw->pw_name ? (char *)pw->pw_name : "none");
		if (pw && pw->pw_name && SP_HAVE_UG(getpwnam)) {
			char nm[32];
			strncpy(nm, (char *)pw->pw_name, sizeof(nm) - 1);
			nm[sizeof(nm) - 1] = 0;
			pw = (struct sp_passwd *)sp_getpwnam((STRPTR)nm);
			sp_result("ext.ug_getpwnam", "getpwnam(that name)", pw ? R_PASS : R_FAIL, 0, NULL);
		}
	}
	NEED_UG(getpwent, "ext.ug_getpwent", "setpwent()/getpwent()") {
		n = 0;
		sp_setpwent();
		while (n < 200 && sp_getpwent())
			n++;
		sp_endpwent();
		sp_result("ext.ug_getpwent", "setpwent()/getpwent()", n > 0 ? R_PASS : R_FAIL, 0, "%ld users", (long)n);
	}
	NEED_UG(getgrgid, "ext.ug_getgrgid", "getgrgid(own gid)") {
		gr = (struct sp_group *)sp_getgrgid(sp_getgid());
		sp_result("ext.ug_getgrgid", "getgrgid(own gid)", gr ? R_PASS : R_FAIL, 0, "%s",
		          gr && gr->gr_name ? (char *)gr->gr_name : "none");
	}
	NEED_UG(getlogin, "ext.ug_getlogin", "getlogin()") {
		s = sp_getlogin();
		sp_result("ext.ug_getlogin", "getlogin()", R_PASS, 0, "%s", s ? (char *)s : "(none)");
	}
	NEED_UG(crypt, "ext.ug_crypt", "crypt(\"test\", \"ab\")") {
		s = sp_crypt((STRPTR)"test", (STRPTR)"ab");
		sp_result("ext.ug_crypt", "crypt(\"test\", \"ab\")", s && s[0] ? R_PASS : R_FAIL, 0, "%.20s", s ? (char *)s : "");
	}
	NEED_UG(getumask, "ext.ug_getumask", "getumask()") {
		sp_result("ext.ug_getumask", "getumask()", R_PASS, 0, "%03lo", (unsigned long)sp_getumask());
	}
	NEED_UG(ug_StrError, "ext.ug_strerror", "ug_StrError()") {
		s = sp_ug_StrError(1);
		sp_result("ext.ug_strerror", "ug_StrError()", s ? R_PASS : R_FAIL, 0, "%.30s", s ? (char *)s : "");
	}
}

void sp_test_ext(void)
{
	ext_roadshow();
	ext_miami();
	ext_usergroup();
}
