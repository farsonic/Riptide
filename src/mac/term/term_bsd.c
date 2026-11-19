/*
 * Riptide - the Riptide Command Terminal on the classic Mac OS: bsdsocket.library and riptide.library,
 * as the network tools (src/amiga/cmd) call them, on the stack's calls for programs (RTMAC_API,
 * src/mac/api_mac.c). The socket calls are the core's own (rt_api_*), run in this program's
 * context as they run in the calling task on the Amiga: a blocking recv waits, select waits,
 * a name lookup waits for its answer. While they wait the driver calls term_yield (the window
 * stays alive) and asks term_break_pending (Command-period).
 *
 * Results follow the AmiTCP conventions: -1 and Errno() on failure. hostent, servent, protoent
 * and addrinfo are built in this program's memory, one of each at a time like the Amiga's
 * per-task buffers (addrinfo lists are allocated and freed with freeaddrinfo).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "term.h"
#include "rt/rt_ip6.h"

/* term_thunks.S: the callbacks, with the program's A5 */
void term_yield_thunk(void *arg);
int term_break_thunk(void *arg);
extern void *term_a5;

static const struct rtmac_api *api;
static void *cl;
static LONG err_no;

/* ---- the driver ---- */
int term_api_open(void)
{
	UBYTE pb[80];
	unsigned char name[6] = { 4, '.', 'I', 'P', 'P', 0 };
	short ref;
	if (cl)
		return 0;
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = name;
	if (mac_pb(pb, TRAP_OPEN) != 0)
		return -1;
	ref = *(short *)(pb + 24);
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = ref;
	*(short *)(pb + 26) = RTMAC_API;
	if (mac_pb(pb, TRAP_CONTROL) != 0)
		return -1;                              /* MacTCP, or a Riptide without RTMAC_API */
	api = *(const struct rtmac_api **)(pb + 28);
	if (!api || api->magic != RTMAC_API_MAGIC || api->version < 1)
		return -1;
	__asm__ volatile("move.l %%a5,%0" : "=m"(term_a5));
	cl = api->attach(term_yield_thunk, term_break_thunk, NULL);
	return cl ? 0 : -1;
}

void term_api_close(void)
{
	if (cl)
		api->detach(cl);
	cl = NULL;
}

int term_api_ok(void)
{
	return cl != NULL;
}

int term_api_command(const char *line, char *out, ULONG outlen)
{
	if (!cl) {
		rt_strlcpy(out, "Riptide is not running\n", outlen);
		return -1;
	}
	return api->command(cl, line, out, outlen);
}

/* a core result to the AmiTCP form; the break is consumed with the EINTR it caused */
static LONG ret(int r)
{
	if (r < 0) {
		err_no = -r;
		if (r == -RT_EINTR)
			term_break_take();
		return -1;
	}
	return r;
}
#define NEED() do { if (!cl) { err_no = RT_ENETDOWN; return -1; } } while (0)

/* ---- bsdsocket.library ---- */
LONG socket(LONG domain, LONG type, LONG protocol) { NEED(); return ret(api->socket(cl, (int)domain, (int)type, (int)protocol)); }
LONG bind(LONG s, const APTR name, LONG len) { NEED(); return ret(api->bind(cl, (int)s, name, (ULONG)len)); }
LONG listen(LONG s, LONG backlog) { NEED(); return ret(api->listen(cl, (int)s, (int)backlog)); }
LONG accept(LONG s, APTR addr, LONG *len) { NEED(); return ret(api->accept(cl, (int)s, addr, (unsigned long *)len)); }
LONG connect(LONG s, const APTR name, LONG len) { NEED(); return ret(api->connect(cl, (int)s, name, (ULONG)len)); }
LONG send(LONG s, const APTR msg, LONG len, LONG flags) { NEED(); return ret(api->sendto(cl, (int)s, msg, (ULONG)len, (int)flags, NULL, 0)); }
LONG recv(LONG s, APTR buf, LONG len, LONG flags) { NEED(); return ret(api->recvfrom(cl, (int)s, buf, (ULONG)len, (int)flags, NULL, NULL)); }
LONG sendto(LONG s, const APTR msg, LONG len, LONG flags, const APTR to, LONG tolen)
{
	NEED();
	return ret(api->sendto(cl, (int)s, msg, (ULONG)len, (int)flags, to, (ULONG)tolen));
}
LONG recvfrom(LONG s, APTR buf, LONG len, LONG flags, APTR from, LONG *fromlen)
{
	unsigned long fl = fromlen ? (unsigned long)*fromlen : 0;
	LONG r;
	NEED();
	if (from && fromlen) {
		r = ret(api->recvfrom(cl, (int)s, buf, (ULONG)len, (int)flags, from, &fl));
		*fromlen = (LONG)fl;
		return r;
	}
	return ret(api->recvfrom(cl, (int)s, buf, (ULONG)len, (int)flags, NULL, NULL));
}
LONG shutdown(LONG s, LONG how) { NEED(); return ret(api->shutdown(cl, (int)s, (int)how)); }
LONG setsockopt(LONG s, LONG level, LONG name, const APTR val, LONG len)
{
	NEED();
	return ret(api->setsockopt(cl, (int)s, (int)level, (int)name, val, (ULONG)len));
}
LONG getsockopt(LONG s, LONG level, LONG name, APTR val, LONG *len)
{
	NEED();
	return ret(api->getsockopt(cl, (int)s, (int)level, (int)name, val, (unsigned long *)len));
}
LONG getsockname(LONG s, APTR addr, LONG *len) { NEED(); return ret(api->getsockname(cl, (int)s, addr, (unsigned long *)len)); }
LONG getpeername(LONG s, APTR addr, LONG *len) { NEED(); return ret(api->getpeername(cl, (int)s, addr, (unsigned long *)len)); }
LONG IoctlSocket(LONG s, ULONG req, APTR arg) { NEED(); return ret(api->ioctl(cl, (int)s, req, arg)); }
LONG CloseSocket(LONG s) { NEED(); return ret(api->close(cl, (int)s)); }
LONG Errno(void) { return err_no; }

LONG WaitSelect(LONG nfds, APTR rd, APTR wr, APTR ex, APTR timeout, ULONG *sigmask)
{
	long ms = -1;
	unsigned long sig = sigmask ? (*sigmask & SIGBREAKF_CTRL_C) : 0;
	LONG r;
	NEED();
	if (timeout) {
		const ULONG *tv = timeout;              /* struct timeval: seconds, microseconds */
		ULONG secs = tv[0] + tv[1] / 1000000u, us = tv[1] % 1000000u;
		ms = secs >= 0x7fffffffu / 1000u - 1 ? 0x7ffffffe : (long)(secs * 1000u + (us + 999u) / 1000u);
	}
	/* a break already there: WaitSelect returns at once with it */
	if (sig && term_break_pending(NULL)) {
		term_break_take();
		*sigmask = SIGBREAKF_CTRL_C;
		return 0;
	}
	r = ret(api->select(cl, (int)nfds, rd, wr, ex, ms, sigmask ? &sig : NULL));
	if (sigmask) {
		*sigmask = sig;
		if (sig & SIGBREAKF_CTRL_C)
			term_break_take();
	}
	return r;
}

/* ---- names ---- */
static struct hostent he;
static char he_name[256];
static char *he_aliases[1];
static char *he_list[9];
static u8 he_addrs[8][16];

static struct hostent *make_hostent(const char *name, const rt_in6_addr *a, int n, int fam)
{
	int i;
	rt_strlcpy(he_name, name, sizeof he_name);
	he.h_name = he_name;
	he.h_aliases = he_aliases;
	he_aliases[0] = NULL;
	he.h_addrtype = fam;
	he.h_length = fam == RT_AF_INET6 ? 16 : 4;
	for (i = 0; i < n && i < 8; i++) {
		if (fam == RT_AF_INET6)
			memcpy(he_addrs[i], a[i].b, 16);
		else
			memcpy(he_addrs[i], a[i].b + 12, 4);
		he_list[i] = (char *)he_addrs[i];
	}
	he_list[i] = NULL;
	he.h_addr_list = he_list;
	return &he;
}

static APTR lookup(CONST_STRPTR name, int fam)
{
	rt_in6_addr a[8];
	char canon[256];
	int n;
	if (!cl || !name)
		return NULL;
	canon[0] = 0;
	n = api->resolve(cl, (const char *)name, a, 8, fam, canon, sizeof canon);
	if (n <= 0) {
		ret(n < 0 ? n : -RT_ENOENT);
		return NULL;
	}
	return make_hostent(canon[0] ? canon : (const char *)name, a, n, fam);
}

APTR gethostbyname(CONST_STRPTR name) { return lookup(name, RT_AF_INET); }
APTR gethostbyname2(CONST_STRPTR name, LONG fam) { return lookup(name, fam == RT_AF_INET6 ? RT_AF_INET6 : RT_AF_INET); }

APTR gethostbyaddr(const APTR addr, LONG len, LONG type)
{
	char name[256];
	rt_in6_addr a;
	int r;
	if (!cl || !addr)
		return NULL;
	memset(&a, 0, sizeof a);
	if (type == RT_AF_INET6 && len == 16) {
		memcpy(a.b, addr, 16);
		r = api->resolve_addr6(cl, addr, name, sizeof name);
	} else if (len == 4) {
		ULONG v4;
		memcpy(&v4, addr, 4);
		rt_in6_set_v4mapped(&a, v4);
		r = api->resolve_addr(cl, v4, name, sizeof name);
		type = RT_AF_INET;
	} else {
		return NULL;
	}
	if (r < 0) {
		ret(r);
		return NULL;
	}
	return make_hostent(name, &a, 1, (int)type);
}

LONG gethostname(STRPTR name, LONG len)
{
	if (!cl) {
		rt_strlcpy((char *)name, "localhost", (u32)len);
		return 0;
	}
	return ret(api->hostname(cl, (char *)name, (ULONG)len));
}

static struct servent se;
static char *se_aliases[1];
APTR getservbyport(LONG port, CONST_STRPTR proto)
{
	const struct rt_servent_info *si = rt_getservbyport(rt_ntohs((u16)port), (const char *)proto);
	if (!si)
		return NULL;
	se.s_name = (char *)si->name;
	se.s_aliases = se_aliases;
	se.s_port = (LONG)rt_htons(si->port);
	se.s_proto = (char *)si->proto;
	return &se;
}

static struct protoent pe;
static char *pe_aliases[1];
APTR getprotobynumber(LONG proto)
{
	const struct rt_protoent_info *pi = rt_getprotobynumber((int)proto);
	if (!pi)
		return NULL;
	pe.p_name = (char *)pi->name;
	pe.p_aliases = pe_aliases;
	pe.p_proto = pi->proto;
	return &pe;
}

/* ---- riptide.library ---- */
LONG RiptideSysCtl(LONG *name, ULONG namelen, APTR oldp, LONG *oldlenp, APTR newp, LONG newlen)
{
	NEED();
	return ret(api->sysctl(cl, (const long *)name, namelen, oldp, (unsigned long *)oldlenp, newp, (ULONG)newlen)) < 0 ? -1 : 0;
}

/* version 1 of riptide.library is offered (term_os.c), so the tools resolve and connect
   themselves; this is never reached */
LONG RiptideConnectName(CONST_STRPTR host, ULONG port, APTR tags)
{
	(void)host; (void)port; (void)tags;
	err_no = RT_EOPNOTSUPP;
	return -1;
}

LONG if_nametoindex(CONST_STRPTR ifname)
{
	return cl && ifname ? api->if_index(cl, (const char *)ifname) : 0;
}

STRPTR if_indextoname(LONG index, STRPTR ifname)
{
	if (!cl || !ifname || api->if_name(cl, (ULONG)index, (char *)ifname) < 0)
		return NULL;
	return ifname;
}

STRPTR gai_strerror(LONG e)
{
	static const char *const s[] = {
		"Success", "Address family for hostname not supported", "Temporary failure in name resolution",
		"Invalid value for ai_flags", "Non-recoverable failure in name resolution",
		"ai_family not supported", "Memory allocation failure", "No address associated with hostname",
		"hostname nor servname provided, or not known", "servname not supported for ai_socktype",
		"ai_socktype not supported", "System error returned in errno"
	};
	return (STRPTR)(e >= 0 && e <= 11 ? s[e] : "Unknown error");
}

void freeaddrinfo(APTR head)
{
	struct addrinfo *ai = head, *next;
	while (ai) {
		next = ai->ai_next;
		FreeVec(ai);
		ai = next;
	}
}

/* hosts only (the tools give no service): IPv6 results first, as the core orders them */
LONG getaddrinfo(CONST_STRPTR host, CONST_STRPTR serv, const APTR hintsp, APTR *result)
{
	const struct addrinfo *hints = hintsp;
	struct addrinfo *head = NULL, **tail = &head;
	rt_in6_addr a[8];
	char canon[256];
	int n, i, fam = hints ? (int)hints->ai_family : RT_AF_UNSPEC;
	int socktype = hints && hints->ai_socktype ? (int)hints->ai_socktype : RT_SOCK_STREAM;
	u16 port = 0;
	if (!result)
		return EAI_FAIL;
	*result = NULL;
	if (!host)
		return EAI_NONAME;
	if (serv) {
		const char *end;
		unsigned long v = rt_strtoul((const char *)serv, &end, 10);
		if (end == (const char *)serv || *end || v > 65535)
			return EAI_SERVICE;
		port = (u16)v;
	}
	if (!cl)
		return EAI_FAIL;
	canon[0] = 0;
	n = api->resolve(cl, (const char *)host, a, 8, fam, canon, sizeof canon);
	if (n == -RT_ENOENT || n == 0)
		return EAI_NONAME;
	if (n < 0)
		return EAI_AGAIN;
	for (i = 0; i < n; i++) {
		int v4 = rt_in6_is_v4mapped(&a[i]);
		ULONG sl = v4 ? sizeof(struct rt_sockaddr_in) : sizeof(struct rt_sockaddr_in6);
		struct addrinfo *ai = AllocVec(sizeof(*ai) + sl, MEMF_ANY | MEMF_CLEAR);
		if (!ai)
			break;
		if (v4) {
			struct rt_sockaddr_in *s4 = (struct rt_sockaddr_in *)(ai + 1);
			s4->sin_len = sizeof(*s4);
			s4->sin_family = RT_AF_INET;
			s4->sin_port = rt_htons(port);
			s4->sin_addr = rt_in6_v4(&a[i]);
			ai->ai_family = RT_AF_INET;
		} else {
			struct rt_sockaddr_in6 *s6 = (struct rt_sockaddr_in6 *)(ai + 1);
			s6->sin6_len = sizeof(*s6);
			s6->sin6_family = RT_AF_INET6;
			s6->sin6_port = rt_htons(port);
			s6->sin6_addr = a[i];
			ai->ai_family = RT_AF_INET6;
		}
		ai->ai_addr = ai + 1;
		ai->ai_addrlen = sl;
		ai->ai_socktype = socktype;
		ai->ai_protocol = socktype == RT_SOCK_DGRAM ? 17 : socktype == RT_SOCK_STREAM ? 6 : 0;
		*tail = ai;
		tail = &ai->ai_next;
	}
	*result = head;
	return head ? 0 : EAI_MEMORY;
}
