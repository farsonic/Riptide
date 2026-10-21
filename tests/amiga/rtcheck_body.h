/*
 * Riptide - library-level checks shared by "Riptide SELFTEST" (single
 * task, under vamos) and the client program rtcheck (real AmigaOS with a
 * running Riptide). Every call goes through the real library jump tables
 * via the generated call inlines (gen/LIB_calls.h).
 *
 * The includer defines:
 *   RTC_LOG(fmt, ...)  print a line
 *   RTC_PUMP()         let the stack make progress (selftest: run
 *                      rt_stack_poll(); rtcheck: Delay(1))
 *   RTC_BLOCKING       1 if blocking calls may be used (real multitasking)
 *   RTC_COMMAND(line)  send one configuration line to the stack, returns
 *                      0 or an errno
 *   RTC_CLOSELIB(base) close a library opened by the includer
 * and opens SocketBase, RiptideBase, CompatBase (the compat alias) and
 * UserGroupBase before rtcheck_run().
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define RTC_AF_INET     2
#define RTC_SOCK_STREAM 1
#define RTC_SOCK_DGRAM  2
#define RTC_EBADF       9
#define RTC_EINPROGRESS 36
#define RTC_EWOULDBLOCK 35
#define RTC_FIONBIO     0x8004667eUL
#define RTC_SOL_SOCKET  0xffff
#define RTC_SO_ERROR    0x1007
#define RTC_AF_INET6    28
#define RTC_IPPROTO_IPV6 41
#define RTC_IPV6_V6ONLY 27

struct rtc_sin {
	UBYTE sin_len, sin_family;
	UWORD sin_port;
	ULONG sin_addr;
	UBYTE sin_zero[8];
};
struct rtc_sin6 {
	UBYTE sin6_len, sin6_family;
	UWORD sin6_port;
	ULONG sin6_flowinfo;
	UBYTE sin6_addr[16];
	ULONG sin6_scope_id;
};
struct rtc_tv {
	ULONG s, us;
};

static int rtc_fail, rtc_pass;
static int rtc_verbose;          /* rtcheck -v: one line per check */
static LONG rtc_errno;

#define CHECK(cond, what) do { \
	if (cond) { \
		rtc_pass++; \
		if (rtc_verbose) RTC_LOG("  ok   %3d %s", rtc_pass + rtc_fail, what); \
	} else { \
		rtc_fail++; \
		if (rtc_verbose) RTC_LOG("  FAIL %3d %s (line %d, errno %ld)", rtc_pass + rtc_fail, what, __LINE__, (long)rtc_errno); \
		else RTC_LOG("FAIL %s (line %d, errno %ld)", what, __LINE__, (long)rtc_errno); \
	} \
} while (0)
/* Section heading, verbose mode only */
#define RTC_SECTION(name) do { if (rtc_verbose) RTC_LOG("%s", name); } while (0)

static int rtc_streq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void rtc_sin(struct rtc_sin *s, ULONG addr, UWORD port)
{
	int i;

	s->sin_len = sizeof(*s);
	s->sin_family = RTC_AF_INET;
	s->sin_port = port;
	s->sin_addr = addr;
	for (i = 0; i < 8; i++)
		s->sin_zero[i] = 0;
}

static void rtc_nbio(LONG fd)
{
	LONG one = 1;

	IoctlSocket(fd, RTC_FIONBIO, &one);
}

/* WaitSelect() with a zero (or short) timeout until fd is ready. */
static int rtc_wait(LONG fd, int for_write)
{
	int i;

	for (i = 0; i < 500; i++) {
		ULONG set[2] = { 0, 0 };
		struct rtc_tv tv;
		LONG r;
		tv.s = 0;
		tv.us = RTC_BLOCKING ? 20000 : 0;
		set[fd / 32] = 1UL << (fd % 32);
		r = WaitSelect(fd + 1, for_write ? NULL : set, for_write ? set : NULL, NULL, &tv, NULL);
		if (r > 0)
			return 1;
		if (r < 0)
			return 0;
		RTC_PUMP();
	}
	return 0;
}

static void rtc_tcp(void)
{
	struct rtc_sin sa, ca, pa;
	LONG ls, cs, as = -1, len, r, got, i;
	static char out[3000], in[3000];

	ls = socket(RTC_AF_INET, RTC_SOCK_STREAM, 0);
	CHECK(ls >= 0, "tcp socket");
	rtc_sin(&sa, 0x7f000001UL, 0);
	CHECK(bind(ls, &sa, sizeof(sa)) == 0, "tcp bind 127.0.0.1:0");
	len = sizeof(sa);
	CHECK(getsockname(ls, &sa, &len) == 0 && sa.sin_port != 0, "getsockname gives a port");
	CHECK(listen(ls, 4) == 0, "listen");
	rtc_nbio(ls);

	cs = socket(RTC_AF_INET, RTC_SOCK_STREAM, 0);
	CHECK(cs >= 0, "tcp client socket");
	if (!RTC_BLOCKING)
		rtc_nbio(cs);
	r = connect(cs, &sa, sizeof(sa));
	CHECK(r == 0 || Errno() == RTC_EINPROGRESS, RTC_BLOCKING ? "blocking connect" : "non-blocking connect");
	CHECK(rtc_wait(ls, 0), "listener becomes readable");
	len = sizeof(pa);
	as = accept(ls, &pa, &len);
	CHECK(as >= 0, "accept");
	CHECK(len == sizeof(pa) && pa.sin_addr == 0x7f000001UL, "accept peer address");
	CHECK(rtc_wait(cs, 1), "client becomes writable");
	len = sizeof(r);
	r = -1;
	CHECK(getsockopt(cs, RTC_SOL_SOCKET, RTC_SO_ERROR, &r, &len) == 0 && r == 0, "SO_ERROR is 0");
	len = sizeof(ca);
	CHECK(getpeername(cs, &ca, &len) == 0 && ca.sin_port == sa.sin_port, "getpeername");

	for (i = 0; i < (LONG)sizeof(out); i++)
		out[i] = (char)(i * 7 + 3);
	r = send(cs, out, sizeof(out), 0);
	CHECK(r > 0, "send");
	if (as >= 0) {
		rtc_nbio(as);
		got = 0;
		for (i = 0; i < 200 && got < r; i++) {
			LONG n = recv(as, in + got, sizeof(in) - got, 0);
			if (n > 0)
				got += n;
			else if (n == 0 || Errno() != RTC_EWOULDBLOCK)
				break;
			else
				rtc_wait(as, 0);
		}
		CHECK(got == r, "recv all bytes");
		for (i = 0; i < got && in[i] == out[i]; i++)
			;
		CHECK(i == got, "data intact");
		CHECK(send(as, "pong", 4, 0) == 4, "send reply");
	}
	CHECK(rtc_wait(cs, 0), "reply readable");
	if (!RTC_BLOCKING)
		rtc_nbio(cs);
	r = recv(cs, in, sizeof(in), 0);
	CHECK(r == 4 && in[0] == 'p' && in[3] == 'g', "recv reply");
	CHECK(shutdown(cs, 1) == 0, "shutdown");
	if (as >= 0) {
		CHECK(rtc_wait(as, 0), "EOF readable");
		CHECK(recv(as, in, sizeof(in), 0) == 0, "recv EOF");
		CHECK(CloseSocket(as) == 0, "close accepted");
	}
	CHECK(CloseSocket(cs) == 0, "close client");
	CHECK(CloseSocket(ls) == 0, "close listener");
}

static void rtc_udp(void)
{
	struct rtc_sin a1, a2, from;
	LONG s1, s2, len, r;
	char buf[64];

	s1 = socket(RTC_AF_INET, RTC_SOCK_DGRAM, 0);
	s2 = socket(RTC_AF_INET, RTC_SOCK_DGRAM, 0);
	CHECK(s1 >= 0 && s2 >= 0, "udp sockets");
	rtc_sin(&a1, 0x7f000001UL, 0);
	rtc_sin(&a2, 0x7f000001UL, 0);
	CHECK(bind(s1, &a1, sizeof(a1)) == 0 && bind(s2, &a2, sizeof(a2)) == 0, "udp bind");
	len = sizeof(a2);
	getsockname(s2, &a2, &len);
	len = sizeof(a1);
	getsockname(s1, &a1, &len);
	rtc_nbio(s2);
	CHECK(sendto(s1, "datagram", 8, 0, &a2, sizeof(a2)) == 8, "sendto");
	CHECK(rtc_wait(s2, 0), "datagram readable");
	len = sizeof(from);
	r = recvfrom(s2, buf, sizeof(buf), 0, &from, &len);
	CHECK(r == 8 && buf[0] == 'd' && buf[7] == 'm', "recvfrom data");
	CHECK(from.sin_port == a1.sin_port && from.sin_addr == 0x7f000001UL, "recvfrom address");
	r = recvfrom(s2, buf, sizeof(buf), 0, NULL, NULL);
	CHECK(r == -1 && Errno() == RTC_EWOULDBLOCK, "empty queue: EWOULDBLOCK");
	CloseSocket(s1);
	CloseSocket(s2);
}

static void rtc_bsd_misc(void)
{
	struct TagItem tags[4];
	ULONG v;
	struct hostent *he;
	struct servent *se;
	struct protoent *pe;
	char name[64];
	ULONG ev;

	tags[0].ti_Tag = SBTM_SETVAL(SBTC_ERRNOLONGPTR);
	tags[0].ti_Data = (ULONG)&rtc_errno;
	tags[1].ti_Tag = SBTM_GETVAL(SBTC_DTABLESIZE);
	tags[1].ti_Data = 0;
	tags[2].ti_Tag = SBTM_GETVAL(SBTC_RELEASESTRPTR);
	tags[2].ti_Data = 0;
	tags[3].ti_Tag = TAG_DONE;
	CHECK(SocketBaseTagList(tags) == 0, "SocketBaseTagList");
	CHECK(tags[1].ti_Data >= 64, "DTABLESIZE >= 64");
	CHECK(tags[2].ti_Data != 0, "RELEASESTRPTR");
	if (tags[2].ti_Data)
		RTC_LOG("stack: %s", (const char *)tags[2].ti_Data);
	CHECK(getdtablesize() == (LONG)tags[1].ti_Data, "getdtablesize");
	tags[0].ti_Tag = 0x80000000UL | (999 << 1);    /* unknown code */
	tags[1].ti_Tag = TAG_DONE;
	CHECK(SocketBaseTagList(tags) == 1, "unknown tag -> index 1");

	rtc_errno = 0;
	CHECK(CloseSocket(60) == -1 && rtc_errno == RTC_EBADF && Errno() == RTC_EBADF, "errno via pointer");
	v = RTC_EBADF;
	tags[0].ti_Tag = SBTM_GETREF(SBTC_ERRNOSTRPTR);
	tags[0].ti_Data = (ULONG)&v;
	tags[1].ti_Tag = TAG_DONE;
	CHECK(SocketBaseTagList(tags) == 0 && v != RTC_EBADF && v != 0, "ERRNOSTRPTR");

	CHECK(inet_addr("127.0.0.1") == 0x7f000001UL, "inet_addr");
	CHECK(inet_addr("1.2.3") == 0x01020003UL, "inet_addr a.b.c");
	CHECK(inet_addr("0x7f.1") == 0x7f000001UL, "inet_addr hex");
	CHECK(inet_addr("1.2.3.256") == 0xffffffffUL, "inet_addr invalid");
	CHECK(rtc_streq(Inet_NtoA(0x0a000001UL), "10.0.0.1"), "Inet_NtoA");
	CHECK(Inet_MakeAddr(127, 1) == 0x7f000001UL, "Inet_MakeAddr");
	CHECK(Inet_NetOf(0x0a010203UL) == 10 && Inet_LnaOf(0x0a010203UL) == 0x010203UL, "NetOf/LnaOf");
	CHECK(Inet_NetOf(0xc0a80102UL) == 0xc0a801UL, "NetOf class C");
	CHECK(inet_network("10.1") == 0x0a01UL, "inet_network");

	he = gethostbyname("localhost");
	CHECK(he && he->h_length == 4 && he->h_addr_list[0] &&
	      *(ULONG *)he->h_addr_list[0] == 0x7f000001UL, "gethostbyname localhost");
	he = gethostbyname("127.0.0.2");
	CHECK(he && *(ULONG *)he->h_addr_list[0] == 0x7f000002UL, "gethostbyname numeric");
	CHECK(gethostname(name, sizeof(name)) == 0 && name[0], "gethostname");
	se = getservbyname("telnet", "tcp");
	CHECK(se && se->s_port == 23, "getservbyname telnet");
	se = getservbyport(21, "tcp");
	CHECK(se && rtc_streq(se->s_name, "ftp"), "getservbyport 21");
	pe = getprotobyname("udp");
	CHECK(pe && pe->p_proto == 17, "getprotobyname udp");
	pe = getprotobynumber(6);
	CHECK(pe && rtc_streq(pe->p_name, "tcp"), "getprotobynumber 6");
	CHECK(getnetbyname("loopback") != NULL, "getnetbyname loopback");
	CHECK(GetSocketEvents(&ev) == -1, "GetSocketEvents: none");
	CHECK(gethostid() != 0, "gethostid");
}

static void rtc_riptide(void)
{
	ULONG a = 0;
	char buf[32];
	APTR res = NULL;
	struct addrinfo *ai;
	LONG idx;

	CHECK(RiptideBase->lib_Version >= RIPTIDE_LIB_VERSION &&
	      rtc_streq(RiptideBase->lib_Node.ln_Name, RIPTIDE_LIB_NAME), "riptide.library name/version");
	CHECK(RiptideSupportsIPV6() == 1, "RiptideSupportsIPV6");
	CHECK(inet_pton(RTC_AF_INET, "192.168.1.2", &a) == 1 && a == 0xc0a80102UL, "inet_pton");
	CHECK(inet_pton(RTC_AF_INET, "192.168.1", &a) == 0, "inet_pton strict");
	CHECK(inet_aton("10.2", &a) == 1 && a == 0x0a000002UL, "inet_aton");
	CHECK(inet_ntop(RTC_AF_INET, &a, buf, sizeof(buf)) && rtc_streq(buf, "10.0.0.2"), "inet_ntop");
	CHECK(getaddrinfo("127.0.0.1", "80", NULL, &res) == 0 && res, "getaddrinfo");
	ai = res;
	if (ai) {
		struct rtc_sin *s = ai->ai_addr;
		CHECK(s && s->sin_port == 80 && s->sin_addr == 0x7f000001UL, "getaddrinfo result");
		freeaddrinfo(res);
	}
	CHECK(gai_strerror(8) != NULL, "gai_strerror");
	idx = if_nametoindex("lo0");
	CHECK(idx >= 1, "if_nametoindex lo0");
	CHECK(if_indextoname(idx, buf) && rtc_streq(buf, "lo0"), "if_indextoname");
	CHECK(RiptideIsOnline("lo0") == 1, "RiptideIsOnline lo0");
	CHECK(RiptideGetPid() != NULL, "RiptideGetPid");
}

/* ---- IPv6 over the loopback (::1) ---- */

static const UBYTE rtc_lo6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

static int rtc_memeq(const void *a, const void *b, int n)
{
	const UBYTE *x = a, *y = b;

	while (n-- > 0)
		if (*x++ != *y++)
			return 0;
	return 1;
}

static void rtc_sin6(struct rtc_sin6 *s, const UBYTE *addr, UWORD port)
{
	int i;

	s->sin6_len = sizeof(*s);
	s->sin6_family = RTC_AF_INET6;
	s->sin6_port = port;
	s->sin6_flowinfo = 0;
	for (i = 0; i < 16; i++)
		s->sin6_addr[i] = addr ? addr[i] : 0;
	s->sin6_scope_id = 0;
}

/* TCP between two sockets; the client is AF_INET (dual-stack check) when
 * v4client is set. Returns the accepted peer address in *peer. */
static void rtc_tcp6_pair(int v4client, int rfc2133, struct rtc_sin6 *peer)
{
	struct rtc_sin6 sa, ca;
	struct rtc_sin s4;
	LONG ls, cs, as, r, len;
	char in[8];

	ls = socket(RTC_AF_INET6, RTC_SOCK_STREAM, 0);
	CHECK(ls >= 0, "tcp6 socket");
	rtc_sin6(&sa, v4client ? NULL : rtc_lo6, 0);
	CHECK(bind(ls, &sa, sizeof(sa)) == 0, v4client ? "tcp6 bind [::]:0" : "tcp6 bind [::1]:0");
	len = sizeof(sa);
	CHECK(getsockname(ls, &sa, &len) == 0 && len == sizeof(sa) && sa.sin6_family == RTC_AF_INET6 &&
	      sa.sin6_port != 0, "tcp6 getsockname");
	CHECK(listen(ls, 4) == 0, "tcp6 listen");
	cs = socket(v4client ? RTC_AF_INET : RTC_AF_INET6, RTC_SOCK_STREAM, 0);
	rtc_nbio(cs);
	if (v4client) {
		rtc_sin(&s4, 0x7f000001UL, sa.sin6_port);
		r = connect(cs, &s4, sizeof(s4));
	} else {
		rtc_sin6(&ca, rtc_lo6, sa.sin6_port);
		/* RFC 2133 callers (Miami SDK) pass 24 bytes, no scope id */
		r = connect(cs, &ca, rfc2133 ? 24 : sizeof(ca));
	}
	CHECK(r == 0 || Errno() == RTC_EINPROGRESS, "tcp6 connect");
	CHECK(rtc_wait(ls, 0), "tcp6 listener readable");
	len = rfc2133 ? 24 : sizeof(*peer);
	as = accept(ls, peer, &len);
	CHECK(as >= 0 && len == (rfc2133 ? 24 : (LONG)sizeof(*peer)) && peer->sin6_family == RTC_AF_INET6,
	      rfc2133 ? "tcp6 accept, 24 byte address" : "tcp6 accept");
	CHECK(rtc_wait(cs, 1), "tcp6 client writable");
	CHECK(send(cs, "six", 3, 0) == 3, "tcp6 send");
	CHECK(rtc_wait(as, 0), "tcp6 data readable");
	CHECK(recv(as, in, sizeof(in), 0) == 3 && in[0] == 's' && in[2] == 'x', "tcp6 recv");
	CloseSocket(as);
	CloseSocket(cs);
	CloseSocket(ls);
}

static void rtc_ipv6(void)
{
	static const UBYTE doc[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	static const UBYTE mapped_lo[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1 };
	struct rtc_sin6 peer, a1, a2, from;
	struct addrinfo hints, *ai;
	struct hostent *he;
	APTR res = NULL;
	UBYTE a[16];
	char buf[64], serv[8];
	LONG s1, s2, v, len, r;
	int i, n4, n6;

	CHECK(inet_pton(RTC_AF_INET6, "2001:db8::1", a) == 1 && rtc_memeq(a, doc, 16), "inet_pton AF_INET6");
	CHECK(inet_pton(RTC_AF_INET6, "2001:db8:::1", a) == 0, "inet_pton AF_INET6 strict");
	CHECK(inet_ntop(RTC_AF_INET6, (APTR)doc, buf, sizeof(buf)) && rtc_streq(buf, "2001:db8::1"), "inet_ntop AF_INET6");
	CHECK(inet_ntop(RTC_AF_INET6, (APTR)mapped_lo, buf, sizeof(buf)) && rtc_streq(buf, "::ffff:127.0.0.1"),
	      "inet_ntop IPv4-mapped");

	/* TCP over ::1, with 28 and 24 byte addresses; dual-stack listener */
	rtc_tcp6_pair(0, 0, &peer);
	CHECK(rtc_memeq(peer.sin6_addr, rtc_lo6, 16), "tcp6 peer is ::1");
	rtc_tcp6_pair(0, 1, &peer);
	rtc_tcp6_pair(1, 0, &peer);
	CHECK(rtc_memeq(peer.sin6_addr, mapped_lo, 16), "dual-stack: IPv4 client seen as ::ffff:127.0.0.1");

	/* UDP over ::1 */
	s1 = socket(RTC_AF_INET6, RTC_SOCK_DGRAM, 0);
	s2 = socket(RTC_AF_INET6, RTC_SOCK_DGRAM, 0);
	CHECK(s1 >= 0 && s2 >= 0, "udp6 sockets");
	rtc_sin6(&a1, rtc_lo6, 0);
	rtc_sin6(&a2, rtc_lo6, 0);
	CHECK(bind(s1, &a1, sizeof(a1)) == 0 && bind(s2, &a2, sizeof(a2)) == 0, "udp6 bind");
	len = sizeof(a1);
	getsockname(s1, &a1, &len);
	len = sizeof(a2);
	getsockname(s2, &a2, &len);
	rtc_nbio(s2);
	CHECK(sendto(s1, "dgram6", 6, 0, &a2, sizeof(a2)) == 6, "udp6 sendto");
	CHECK(rtc_wait(s2, 0), "udp6 readable");
	len = sizeof(from);
	r = recvfrom(s2, buf, sizeof(buf), 0, &from, &len);
	CHECK(r == 6 && buf[0] == 'd' && buf[5] == '6', "udp6 recvfrom data");
	CHECK(len == sizeof(from) && from.sin6_port == a1.sin6_port && rtc_memeq(from.sin6_addr, rtc_lo6, 16),
	      "udp6 recvfrom address");
	v = 1;
	CHECK(setsockopt(s1, RTC_IPPROTO_IPV6, RTC_IPV6_V6ONLY, &v, sizeof(v)) == -1, "IPV6_V6ONLY after bind refused");
	v = -1;
	len = sizeof(v);
	CHECK(getsockopt(s1, RTC_IPPROTO_IPV6, RTC_IPV6_V6ONLY, &v, &len) == 0 && v == 0, "IPV6_V6ONLY default 0");
	CloseSocket(s1);
	CloseSocket(s2);

	/* resolver calls */
	for (i = 0; i < (int)sizeof(hints); i++)
		((UBYTE *)&hints)[i] = 0;
	hints.ai_family = RTC_AF_INET6;
	hints.ai_socktype = RTC_SOCK_STREAM;
	CHECK(getaddrinfo("::1", "80", &hints, &res) == 0 && res, "getaddrinfo ::1");
	ai = res;
	if (ai) {
		struct rtc_sin6 *s = ai->ai_addr;
		CHECK(ai->ai_family == RTC_AF_INET6 && ai->ai_addrlen == sizeof(*s) && s->sin6_port == 80 &&
		      rtc_memeq(s->sin6_addr, rtc_lo6, 16) && !ai->ai_next, "getaddrinfo ::1 result");
		freeaddrinfo(res);
	}
	res = NULL;
	CHECK(getaddrinfo("127.0.0.1", "80", &hints, &res) != 0, "getaddrinfo IPv4 literal for AF_INET6");
	hints.ai_flags = 0x800;           /* AI_V4MAPPED */
	res = NULL;
	CHECK(getaddrinfo("127.0.0.1", "80", &hints, &res) == 0 && res &&
	      ((struct addrinfo *)res)->ai_family == RTC_AF_INET6 &&
	      rtc_memeq(((struct rtc_sin6 *)((struct addrinfo *)res)->ai_addr)->sin6_addr, mapped_lo, 16),
	      "getaddrinfo AI_V4MAPPED");
	if (res)
		freeaddrinfo(res);
	hints.ai_family = 0;
	hints.ai_flags = 1;               /* AI_PASSIVE, AF_UNSPEC: :: and 0.0.0.0 */
	res = NULL;
	n4 = n6 = 0;
	CHECK(getaddrinfo(NULL, "21", &hints, &res) == 0 && res, "getaddrinfo passive AF_UNSPEC");
	for (ai = res; ai; ai = ai->ai_next) {
		n4 += ai->ai_family == RTC_AF_INET;
		n6 += ai->ai_family == RTC_AF_INET6;
	}
	CHECK(n4 == 1 && n6 == 1, "getaddrinfo passive: one of each family");
	if (res)
		freeaddrinfo(res);
	rtc_sin6(&a1, rtc_lo6, 21);
	CHECK(getnameinfo(&a1, sizeof(a1), buf, sizeof(buf), serv, sizeof(serv), 2 | 8) == 0 &&
	      rtc_streq(buf, "::1") && rtc_streq(serv, "21"), "getnameinfo AF_INET6 numeric");
	he = gethostbyname2("::1", RTC_AF_INET6);
	CHECK(he && he->h_addrtype == RTC_AF_INET6 && he->h_length == 16 && he->h_addr_list[0] &&
	      rtc_memeq(he->h_addr_list[0], rtc_lo6, 16), "gethostbyname2 AF_INET6");
}

/* The compat alias: its own per-task base on the same jump table and the
 * same per-task context as riptide.library. */
static void rtc_compat(void)
{
	struct Library *rb = RiptideBase;
	APTR pid, apid;
	LONG on;

	CHECK(CompatBase != RiptideBase, "compat alias: own base");
	CHECK(CompatBase->lib_Version >= RT_COMPAT_LIBVERSION &&
	      rtc_streq(CompatBase->lib_Node.ln_Name, RT_COMPAT_LIBNAME), "compat alias: name/version");
	pid = RiptideGetPid();
	RiptideBase = CompatBase;           /* call through the alias' vectors */
	apid = RiptideGetPid();
	on = RiptideIsOnline("lo0");
	RiptideBase = rb;
	CHECK(pid && apid == pid, "compat alias: shares the per-task context");
	CHECK(on == 1, "compat alias: RiptideIsOnline lo0");
}

static int rtc_lib_listed(const char *name)
{
	int r;

	Forbid();
	r = FindName(exec_liblist(SysBase), (CONST_STRPTR)name) != NULL;
	Permit();
	return r;
}

/* "compat aliases off" is refused while the alias is open and removes it
 * once it is closed; "on" brings it back. Closes CompatBase. */
static void rtc_compat_switch(void)
{
	rtc_errno = 0;
	if (CompatBase) {
		rtc_errno = RTC_COMMAND("compat aliases off");
		RTC_LOG("compat aliases off with the alias open: errno %ld, alias %s, open count %ld",
		        (long)rtc_errno, rtc_lib_listed(RT_COMPAT_LIBNAME) ? "listed" : "gone",
		        (long)CompatBase->lib_OpenCnt);
		CHECK(rtc_errno != 0 && rtc_lib_listed(RT_COMPAT_LIBNAME),
		      "compat aliases off refused while the alias is open");
		RTC_CLOSELIB(CompatBase);
		CompatBase = NULL;
	}
	rtc_errno = RTC_COMMAND("compat aliases off");
	CHECK(rtc_errno == 0 && !rtc_lib_listed(RT_COMPAT_LIBNAME) && rtc_lib_listed(RIPTIDE_LIB_NAME),
	      "compat aliases off removes the alias");
	rtc_errno = RTC_COMMAND("compat aliases on");
	CHECK(rtc_errno == 0 && rtc_lib_listed(RT_COMPAT_LIBNAME), "compat aliases on adds it again");
	RTC_LOG("compat alias switch: %d checks passed, %d failed so far", rtc_pass, rtc_fail);
}

static void rtc_usergroup(void)
{
	struct passwd *pw;
	const char *c;

	CHECK(getuid() == 0 && getgid() == 0, "getuid/getgid");
	pw = getpwnam("user");
	CHECK(pw && rtc_streq(pw->pw_dir, "SYS:"), "getpwnam user");
	CHECK(getpwuid(0) != NULL, "getpwuid 0");
	c = crypt("secret", "ab");
	CHECK(c && c[0] == 'a' && c[1] == 'b' && c[13] == 0, "crypt format");
	umask(027);
	CHECK(getumask() == 027, "umask");
	CHECK(getlogin() != NULL, "getlogin");
}

#if RTC_BLOCKING
/* Sleeping paths: timer.device timeouts, Ctrl-C -> EINTR, WaitSelect()
 * extra signals. Only possible with a separate stack process. */
static void rtc_blocking(void)
{
	struct rtc_sin sa;
	struct rtc_tv tv;
	ULONG set[2], sigs;
	LONG ls, r, len;

	ls = socket(RTC_AF_INET, RTC_SOCK_STREAM, 0);
	rtc_sin(&sa, 0x7f000001UL, 0);
	bind(ls, &sa, sizeof(sa));
	listen(ls, 1);
	len = sizeof(sa);
	getsockname(ls, &sa, &len);

	set[0] = set[1] = 0;
	set[ls / 32] = 1UL << (ls % 32);
	tv.s = 0;
	tv.us = 300000;
	r = WaitSelect(ls + 1, set, NULL, NULL, &tv, NULL);
	CHECK(r == 0 && set[ls / 32] == 0, "WaitSelect times out after 0.3 s");

	SetSignal(0, SIGBREAKF_CTRL_E);
	Signal(FindTask(NULL), SIGBREAKF_CTRL_E);
	sigs = SIGBREAKF_CTRL_E;
	set[ls / 32] = 1UL << (ls % 32);
	tv.s = 5;
	tv.us = 0;
	r = WaitSelect(ls + 1, set, NULL, NULL, &tv, &sigs);
	CHECK(r == 0 && sigs == SIGBREAKF_CTRL_E, "WaitSelect returns on an extra signal");

	Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
	len = sizeof(sa);
	r = accept(ls, &sa, &len);                 /* blocking, nothing pending */
	CHECK(r == -1 && Errno() == 4, "blocking accept interrupted by Ctrl-C (EINTR)");
	SetSignal(0, SIGBREAKF_CTRL_C);
	CloseSocket(ls);
}
#endif

static int rtcheck_run(void)
{
	/* counters are not reset: checks made before (opening the libraries)
	 * belong to the same run and numbering */
	RTC_SECTION("bsdsocket.library: misc (errno, tags, inet_*, netdb):");
	rtc_bsd_misc();
	RTC_SECTION("UDP over loopback:");
	rtc_udp();
	RTC_SECTION("TCP over loopback:");
	rtc_tcp();
#if RTC_BLOCKING
	RTC_SECTION("blocking calls, timeouts and Ctrl-C:");
	rtc_blocking();
#endif
	if (RiptideBase) {
		RTC_SECTION("riptide.library:");
		rtc_riptide();
		RTC_SECTION("IPv6 over ::1:");
		rtc_ipv6();
	}
	if (RiptideBase && CompatBase) {
		RTC_SECTION("compat alias library:");
		rtc_compat();
	}
	if (UserGroupBase) {
		RTC_SECTION("usergroup.library:");
		rtc_usergroup();
	}
	RTC_LOG("%d checks passed, %d failed", rtc_pass, rtc_fail);
	return rtc_fail;
}
