/*
 * Riptide - riptide.library (version 1), Riptide's own API library: sysctl
 * access to the stack and its tables (RiptideSysCtl()), interface state,
 * the protocol-independent resolver calls (getaddrinfo() & co.) and the
 * if_* functions. Socket related calls use the per-task context shared
 * with bsdsocket.library.
 *
 * Its jump table keeps the layout of a historical Amiga API library
 * (53 documented vectors, reserved ones returning 0, extending to -522),
 * so the same table is also published under that library's name as a
 * compat alias (rtlib_compat, "compat aliases on|off" in riptide.conf).
 * The alias is a second master base on the same vectors; its openers get
 * per-task bases exactly like riptide.library's.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/riptide_impl.h"

struct RtLibDef rtlib_riptide = {
	"riptide.library",
	"riptide.library " RIPTIDE_LIB_VERSTR " (" RIPTIDE_DATE ") Riptide " RIPTIDE_VERSION_STRING "\r\n",
	RIPTIDE_LIB_VERSION, RIPTIDE_LIB_REVISION, riptide_vectors, sizeof(struct RtBase), 1, NULL, NULL, NULL,
	{ NULL, NULL, NULL }
};

/* The compat alias: name and version of the historical library whose API
 * riptide.library provides (checked against its SDK by
 * tests/amiga/check_layouts.sh). */
struct RtLibDef rtlib_compat = {
	RT_COMPAT_LIBNAME,
	RT_COMPAT_LIBNAME " 21.1 (" RIPTIDE_DATE ") Riptide " RIPTIDE_VERSION_STRING " compat alias\r\n",
	RT_COMPAT_LIBVERSION, 1, riptide_vectors, sizeof(struct RtBase), 1, NULL, NULL, NULL, { NULL, NULL, NULL }
};

int amiga_inet_aton(const char *cp, u32 *addr);
STRPTR amiga_ntoa(u32 in_net, char *buf);
void amiga_set_fdcb(struct TaskCtx *c, APTR fn, LONG cpu);

#define CTX struct TaskCtx *c = base_ctx(base)
#define RES_DEFAULT_OPTS 0x2c1     /* RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH */

static LONG res_options = RES_DEFAULT_OPTS;

static LONG fail(struct TaskCtx *c, int err)
{
	bsd_set_errno(c, err);
	return -1;
}

LONG riptide_RiptideSysCtl(RTREG(a6) struct Library *base, RTREG(a0) LONG *name, RTREG(d0) ULONG namelen,
                       RTREG(a1) APTR oldp, RTREG(a2) LONG *oldlenp, RTREG(a3) APTR newp,
                       RTREG(d1) LONG newlen)
{
	CTX;
	u32 ol = oldlenp ? (u32)*oldlenp : 0;
	int r;

	if (!name || namelen == 0 || namelen > RT_SYSCTL_MAXMIB + 2)
		return fail(c, RT_EINVAL);
	rt_lock();
	r = rt_sysctl_bymib((const s32 *)name, namelen, oldp, oldlenp ? &ol : NULL, newp, (u32)newlen);
	rt_unlock();
	if (oldlenp && (r == 0 || r == -RT_ENOMEM))
		*oldlenp = (LONG)ol;
	if (r < 0)
		return fail(c, -r);
	return 0;
}

void riptide_RiptideDisallowDNS(RTREG(a6) struct Library *base, RTREG(d0) LONG value)
{
	rt_dns_disallowed = value != 0;
}

ULONG riptide_RiptideGetPid(RTREG(a6) struct Library *base)
{
	return (ULONG)base_ctx(base);
}

ULONG riptide_RiptidePFAddHook(RTREG(a6) struct Library *base, RTREG(a0) APTR hook,
                           RTREG(a1) CONST_STRPTR name, RTREG(a2) APTR tags)
{
	return 0;      /* packet monitoring is not supported */
}

void riptide_RiptidePFRemoveHook(RTREG(a6) struct Library *base, RTREG(a0) APTR handle)
{
}

LONG riptide_RiptideGetHardwareLen(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	struct rt_netif *n;
	LONG len = 0;

	if (!name)
		return 0;
	rt_lock();
	n = rt_netif_find((const char *)name);
	if (n)
		len = n->hwlen;
	rt_unlock();
	return len;
}

ULONG riptide_RiptideOpenSSL(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	return 0;
}

void riptide_RiptideCloseSSL(RTREG(a6) struct Library *base)
{
}

LONG riptide_RiptideSetSocksConn(RTREG(a6) struct Library *base, RTREG(a0) APTR sa, RTREG(d0) LONG len)
{
	return 0;      /* no SOCKS */
}

static int netif_online(const struct rt_netif *n)
{
	return (n->flags & (RT_IFF_UP | RT_IFF_RUNNING)) == (RT_IFF_UP | RT_IFF_RUNNING) ||
	       (n->type == RT_IFT_LOOP && (n->flags & RT_IFF_UP));
}

LONG riptide_RiptideIsOnline(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	struct rt_netif *n;
	LONG r = 0;

	rt_lock();
	if (!name || !strcmp((const char *)name, "mi0")) {
		/* "mi0" (the historical API's name) or NULL: any non-loopback interface */
		for (n = rt_netif_list(); n; n = n->next)
			if (n->type != RT_IFT_LOOP && netif_online(n))
				r = 1;
	} else if ((n = rt_netif_find((const char *)name)) != NULL) {
		r = netif_online(n);
	}
	rt_unlock();
	return r;
}

void riptide_RiptideOnOffline(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name, RTREG(d0) LONG online)
{
	struct amiga_if *ai;

	rt_lock();
	for (ai = amiga_ifs; ai; ai = ai->next) {
		if (name && strcmp((const char *)name, "mi0") && strcmp((const char *)name, ai->nif.name))
			continue;
		if (ai->set_online)
			ai->set_online(ai, online != 0);
	}
	rt_unlock();
	rt_kick();
}

ULONG riptide_inet_ntop(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) const APTR addrptr,
                      RTREG(a1) STRPTR strptr, RTREG(d1) LONG len)
{
	CTX;
	char buf[RT_INET6_ADDRSTRLEN];
	u32 a;

	if (family != RT_AF_INET && family != RT_AF_INET6) {
		fail(c, RT_EAFNOSUPPORT);
		return 0;
	}
	if (!addrptr || !strptr)
		return 0;
	if (family == RT_AF_INET6) {
		rt_in6_addr a6;
		memcpy(a6.b, addrptr, 16);
		rt_inet6_ntop(&a6, buf, sizeof(buf));
	} else {
		memcpy(&a, addrptr, 4);
		rt_format_ipv4(a, buf);
	}
	if ((LONG)strlen(buf) >= len) {
		fail(c, RT_ENOSPC);
		return 0;
	}
	strcpy((char *)strptr, buf);
	return (ULONG)strptr;
}

LONG riptide_inet_aton(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR cp, RTREG(a1) APTR addr)
{
	u32 a;

	if (!amiga_inet_aton((const char *)cp, &a))
		return 0;
	if (addr)
		memcpy(addr, &a, 4);
	return 1;
}

LONG riptide_inet_pton(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) CONST_STRPTR strptr,
                     RTREG(a1) APTR addrptr)
{
	CTX;
	u32 a;

	if (family == RT_AF_INET6) {
		rt_in6_addr a6;
		if (!strptr || rt_inet6_pton((const char *)strptr, &a6) < 0)
			return 0;
		if (addrptr)
			memcpy(addrptr, a6.b, 16);
		return 1;
	}
	if (family != RT_AF_INET)
		return fail(c, RT_EAFNOSUPPORT);
	if (!strptr || rt_parse_ipv4((const char *)strptr, &a) < 0)
		return 0;
	if (addrptr)
		memcpy(addrptr, &a, 4);
	return 1;
}

static LONG herr_of(int r)
{
	return r == -RT_TRY_AGAIN ? TRY_AGAIN : r == -RT_NO_DATA || r == 0 ? NO_DATA :
	       r == -RT_NO_RECOVERY ? NO_RECOVERY : HOST_NOT_FOUND;
}

ULONG riptide_gethostbyname2(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name, RTREG(d0) LONG fam)
{
	CTX;
	u32 addrs[8];
	rt_in6_addr a6[8];
	char cname[256];
	int r;

	if ((fam != RT_AF_INET && fam != RT_AF_INET6) || !name) {
		c->herrno = HOST_NOT_FOUND;
		if (c->herrnoptr)
			*c->herrnoptr = c->herrno;
		return 0;
	}
	cname[0] = 0;
	if (fam == RT_AF_INET6)
		r = rt_resolve_name6((const char *)name, a6, 8, cname, sizeof(cname));
	else
		r = rt_resolve_name((const char *)name, addrs, 8, cname, sizeof(cname));
	if (r <= 0) {
		c->herrno = herr_of(r);
		if (c->herrnoptr)
			*c->herrnoptr = c->herrno;
		return 0;
	}
	if (fam == RT_AF_INET6)
		return (ULONG)bsd_make_hostent6(c, cname[0] ? cname : (const char *)name, a6, r);
	return (ULONG)bsd_make_hostent(c, cname[0] ? cname : (const char *)name, addrs, r);
}

ULONG riptide_gai_strerror(RTREG(a6) struct Library *base, RTREG(d0) LONG err)
{
	static const char *const s[] = {
		"Success", "Address family for hostname not supported", "Temporary failure in name resolution",
		"Invalid value for ai_flags", "Non-recoverable failure in name resolution",
		"ai_family not supported", "Memory allocation failure", "No address associated with hostname",
		"hostname nor servname provided, or not known", "servname not supported for ai_socktype",
		"ai_socktype not supported", "System error returned in errno"
	};
	if (err >= 0 && err <= 11)
		return (ULONG)s[err];
	return (ULONG)"Unknown error";
}

void riptide_freeaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) APTR aihead)
{
	struct addrinfo *ai = aihead, *next;

	while (ai) {
		next = ai->ai_next;
		FreeVec(ai);
		ai = next;
	}
}

static int service_port(const char *serv, int socktype, LONG flags, u16 *port)
{
	const char *end;
	unsigned long v;
	const struct rt_servent_info *si;

	if (!serv) {
		*port = 0;
		return 0;
	}
	v = rt_strtoul(serv, &end, 10);
	if (end != serv && !*end) {
		if (v > 65535)
			return EAI_SERVICE;
		*port = (u16)v;
		return 0;
	}
	if (flags & AI_NUMERICSERV)
		return EAI_NONAME;
	si = rt_getservbyname(serv, socktype == RT_SOCK_DGRAM ? "udp" : "tcp");
	if (!si)
		return EAI_SERVICE;
	*port = si->port;
	return 0;
}

/* One result. as6: return a sockaddr_in6 (IPv6, or IPv4-mapped for
 * AI_V4MAPPED); otherwise a is IPv4-mapped and a sockaddr_in is built. */
static struct addrinfo *ai_new(const struct addrinfo *hints, int socktype, const rt_in6_addr *a, int as6,
                               u32 scope, u16 port, const char *canon)
{
	u32 cl = canon ? (u32)strlen(canon) + 1 : 0;
	u32 sl = as6 ? sizeof(struct rt_sockaddr_in6) : sizeof(struct rt_sockaddr_in);
	struct addrinfo *ai = AllocVec(sizeof(*ai) + sl + cl, MEMF_PUBLIC | MEMF_CLEAR);

	if (!ai)
		return NULL;
	if (as6) {
		struct rt_sockaddr_in6 *sin6 = (struct rt_sockaddr_in6 *)(ai + 1);
		sin6->sin6_len = sizeof(*sin6);
		sin6->sin6_family = RT_AF_INET6;
		sin6->sin6_port = rt_htons(port);
		sin6->sin6_addr = *a;
		sin6->sin6_scope_id = scope;
		ai->ai_family = RT_AF_INET6;
		ai->ai_addr = sin6;
	} else {
		struct rt_sockaddr_in *sin = (struct rt_sockaddr_in *)(ai + 1);
		sin->sin_len = sizeof(*sin);
		sin->sin_family = RT_AF_INET;
		sin->sin_port = rt_htons(port);
		sin->sin_addr = rt_in6_v4(a);
		ai->ai_family = RT_AF_INET;
		ai->ai_addr = sin;
	}
	ai->ai_flags = hints ? hints->ai_flags : 0;
	ai->ai_socktype = socktype;
	ai->ai_protocol = socktype == RT_SOCK_DGRAM ? 17 : socktype == RT_SOCK_STREAM ? 6 : 0;
	if (hints && hints->ai_protocol)
		ai->ai_protocol = hints->ai_protocol;
	ai->ai_addrlen = sl;
	if (canon) {
		ai->ai_canonname = (char *)(ai + 1) + sl;
		memcpy(ai->ai_canonname, canon, cl);
	}
	return ai;
}

#define GAI_MAX 16

/* "fe80::1%eth0" / "fe80::1%2": IPv6 literal with an optional zone */
static int parse_literal6(const char *host, rt_in6_addr *a, u32 *scope)
{
	char zone[RT_IFNAMSIZ + 1];
	int plen;

	zone[0] = 0;
	if (rt_inet6_parse_prefix(host, a, &plen, zone, sizeof(zone)) < 0 || plen >= 0)
		return -1;
	*scope = 0;
	if (zone[0]) {
		const char *end;
		unsigned long v = rt_strtoul(zone, &end, 10);
		if (end != zone && !*end) {
			*scope = (u32)v;
		} else {
			struct rt_netif *n;
			rt_lock();
			n = rt_netif_find(zone);
			*scope = n ? n->index : 0;
			rt_unlock();
			if (!*scope)
				return -1;
		}
	}
	return 0;
}

LONG riptide_getaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR hostname,
                       RTREG(a1) CONST_STRPTR servname, RTREG(a2) const APTR hintsp, RTREG(a3) APTR *result)
{
	const struct addrinfo *hints = hintsp;
	struct addrinfo *head = NULL, **tail = &head;
	const char *host = (const char *)hostname, *serv = (const char *)servname;
	int types[2], ntypes = 0, i, j, naddr = 0, e;
	rt_in6_addr addrs[GAI_MAX];
	u32 scope = 0, v4;
	char cname[256];
	LONG flags = hints ? hints->ai_flags : 0;
	LONG family = hints ? hints->ai_family : RT_AF_UNSPEC;
	int want4, want6, mapped;

	if (!result)
		return EAI_FAIL;
	*result = NULL;
	if (family != RT_AF_UNSPEC && family != RT_AF_INET && family != RT_AF_INET6)
		return EAI_FAMILY;
	if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV | AI_ALL |
	              AI_V4MAPPED_CFG | AI_ADDRCONFIG | AI_V4MAPPED))
		return EAI_BADFLAGS;
	if (hints && hints->ai_socktype && hints->ai_socktype != RT_SOCK_STREAM &&
	    hints->ai_socktype != RT_SOCK_DGRAM && hints->ai_socktype != RT_SOCK_RAW)
		return EAI_SOCKTYPE;
	if (!host && !serv)
		return EAI_NONAME;
	if (hints && hints->ai_socktype)
		types[ntypes++] = hints->ai_socktype;
	else {
		types[ntypes++] = RT_SOCK_STREAM;
		types[ntypes++] = RT_SOCK_DGRAM;
	}
	/* IPv4 results as IPv4-mapped IPv6 (RFC 3493 6.1) */
	mapped = family == RT_AF_INET6 && (flags & AI_V4MAPPED);
	want6 = family != RT_AF_INET;
	want4 = family != RT_AF_INET6 || mapped;
	/* no AAAA while IPv6 cannot reach anything (net.inet.he.aaaa_filter):
	 * programs that try the first address only would wait for nothing */
	if (family == RT_AF_UNSPEC && host && rt_he_aaaa_filter) {
		int usable;
		rt_lock();
		usable = rt_he_v6_usable();
		rt_unlock();
		if (!usable)
			want6 = 0;
	}
	if ((flags & AI_ADDRCONFIG) && host) {
		int h4, h6;
		rt_lock();
		h4 = rt_inet_have_addr(RT_AF_INET);
		h6 = rt_inet_have_addr(RT_AF_INET6);
		rt_unlock();
		if (h4 || h6) {         /* nothing configured: do not filter at all */
			want4 &= h4;
			want6 &= h6;
		}
		if (!want4 && !want6)
			return EAI_NONAME;
	}
	cname[0] = 0;
	if (!host) {
		/* RFC 3493 6.1: the unspecified or the loopback address */
		if (want6)
			addrs[naddr++] = (flags & AI_PASSIVE) ? rt_in6addr_any : rt_in6addr_loopback;
		if (want4 && family != RT_AF_INET6)
			rt_in6_set_v4mapped(&addrs[naddr++], (flags & AI_PASSIVE) ? 0 : rt_htonl(0x7f000001u));
	} else if (amiga_inet_aton(host, &v4)) {
		if (family == RT_AF_INET6 && !mapped)
			return EAI_ADDRFAMILY;
		rt_in6_set_v4mapped(&addrs[naddr++], v4);
		rt_strlcpy(cname, host, sizeof(cname));
	} else if (parse_literal6(host, &addrs[0], &scope) == 0) {
		if (family == RT_AF_INET)
			return EAI_ADDRFAMILY;
		naddr = 1;
		rt_strlcpy(cname, host, sizeof(cname));
	} else if (strchr(host, ':') || (flags & AI_NUMERICHOST)) {
		return EAI_NONAME;
	} else {
		int r6 = -RT_NO_DATA, r4 = -RT_NO_DATA;
		if (want6 && want4 && !mapped && rt_he_enable) {
			/* both families: one parallel lookup (RFC 8305 3) */
			r6 = rt_resolve_name46(host, addrs, GAI_MAX, RT_AF_UNSPEC, cname, sizeof(cname));
			if (r6 > 0)
				naddr = r6;
			r4 = r6;
			want4 = want6 = 0;
		}
		if (want6) {
			r6 = rt_resolve_name6(host, addrs, GAI_MAX, cname, sizeof(cname));
			if (r6 > 0)
				naddr = r6;
		}
		/* with AI_V4MAPPED, IPv4 only when there is no AAAA, unless AI_ALL */
		if (want4 && (!mapped || naddr == 0 || (flags & AI_ALL)) && naddr < GAI_MAX) {
			u32 a4[8];
			char c4[256];
			c4[0] = 0;
			r4 = rt_resolve_name(host, a4, 8, c4, sizeof(c4));
			for (i = 0; i < r4 && naddr < GAI_MAX; i++)
				rt_in6_set_v4mapped(&addrs[naddr++], a4[i]);
			if (!cname[0])
				rt_strlcpy(cname, c4, sizeof(cname));
		}
		if (naddr == 0) {
			int r = r6 == -RT_TRY_AGAIN || r4 == -RT_TRY_AGAIN ? -RT_TRY_AGAIN :
			        r6 == -RT_HOST_NOT_FOUND || r4 == -RT_HOST_NOT_FOUND ? -RT_HOST_NOT_FOUND :
			        r6 == -RT_NO_DATA || r4 == -RT_NO_DATA || r6 == 0 || r4 == 0 ? -RT_NO_DATA : -RT_NO_RECOVERY;
			if (r == -RT_TRY_AGAIN)
				return EAI_AGAIN;
			if (r == -RT_NO_DATA)
				return EAI_NODATA;
			if (r == -RT_HOST_NOT_FOUND)
				return EAI_NONAME;
			return EAI_FAIL;
		}
		if (!cname[0])
			rt_strlcpy(cname, host, sizeof(cname));
		if (naddr > 1) {
			rt_lock();
			rt_inet_sort_dest(addrs, naddr);
			rt_unlock();
		}
	}
	for (i = 0; i < naddr; i++) {
		int as6 = mapped || !rt_in6_is_v4mapped(&addrs[i]);
		for (j = 0; j < ntypes; j++) {
			u16 port;
			struct addrinfo *ai;
			if (types[j] == RT_SOCK_RAW) {
				if (serv)
					continue;
				port = 0;
			} else if ((e = service_port(serv, types[j], flags, &port)) != 0) {
				if (ntypes == 1 || e == EAI_NONAME) {
					riptide_freeaddrinfo(base, head);
					return e;
				}
				continue;
			}
			ai = ai_new(hints, types[j], &addrs[i], as6, as6 ? scope : 0, port,
			            (head == NULL && (flags & AI_CANONNAME)) ? cname : NULL);
			if (!ai) {
				riptide_freeaddrinfo(base, head);
				return EAI_MEMORY;
			}
			*tail = ai;
			tail = &ai->ai_next;
		}
	}
	if (!head)
		return EAI_SERVICE;
	*result = head;
	return 0;
}

LONG riptide_getnameinfo(RTREG(a6) struct Library *base, RTREG(a0) const APTR sa, RTREG(d0) LONG salen,
                       RTREG(a1) STRPTR host, RTREG(d1) LONG hostlen, RTREG(a2) STRPTR serv,
                       RTREG(d2) LONG servlen, RTREG(d3) LONG flags)
{
	const struct rt_sockaddr_in *sin = sa;
	const struct rt_sockaddr_in6 *sin6 = sa;
	rt_in6_addr a6;
	u32 scope = 0;
	u16 nport;
	int v6;
	char name[256];

	if (!sin || salen < 2)
		return EAI_FAIL;
	if (sin->sin_family == RT_AF_INET) {
		if (salen < (LONG)sizeof(*sin))
			return EAI_FAIL;
		v6 = 0;
		rt_in6_set_v4mapped(&a6, sin->sin_addr);
		nport = sin->sin_port;
	} else if (sin->sin_family == RT_AF_INET6) {
		if (salen < RT_SIN6_LEN_RFC2133)
			return EAI_FAIL;
		v6 = 1;
		memcpy(a6.b, sin6->sin6_addr.b, 16);
		if (salen >= RT_SIN6_LEN)
			scope = sin6->sin6_scope_id;
		nport = sin6->sin6_port;
	} else {
		return EAI_FAMILY;
	}
	if (host && hostlen > 0) {
		int done = 0;
		if (!(flags & NI_NUMERICHOST) &&
		    (v6 ? rt_resolve_addr6(&a6, name, sizeof(name)) : rt_resolve_addr(sin->sin_addr, name, sizeof(name))) == 0) {
			if (flags & NI_NOFQDN) {
				char *dot = strchr(name, '.');
				if (dot)
					*dot = 0;
			}
			done = 1;
		}
		if (!done) {
			if (flags & NI_NAMEREQD)
				return EAI_NONAME;
			if (!v6) {
				rt_format_ipv4(sin->sin_addr, name);
			} else {
				rt_inet6_ntop(&a6, name, sizeof(name));
				/* RFC 4007 11: a zone for scoped addresses */
				if (scope && (rt_in6_is_linklocal(&a6) || rt_in6_is_mc_linklocal(&a6))) {
					struct rt_netif *n;
					u32 l = (u32)strlen(name);
					rt_lock();
					n = rt_netif_by_index(scope);
					if (n)
						rt_snprintf(name + l, sizeof(name) - l, "%%%s", n->name);
					else
						rt_snprintf(name + l, sizeof(name) - l, "%%%lu", (unsigned long)scope);
					rt_unlock();
				}
			}
		}
		if ((LONG)strlen(name) >= hostlen)
			return EAI_MEMORY;
		strcpy((char *)host, name);
	}
	if (serv && servlen > 0) {
		const struct rt_servent_info *si = NULL;
		u16 port = rt_ntohs(nport);
		if (!(flags & NI_NUMERICSERV))
			si = rt_getservbyport(port, (flags & NI_DGRAM) ? "udp" : "tcp");
		if (si)
			rt_strlcpy(name, si->name, sizeof(name));
		else
			rt_snprintf(name, sizeof(name), "%u", (unsigned)port);
		if ((LONG)strlen(name) >= servlen)
			return EAI_MEMORY;
		strcpy((char *)serv, name);
	}
	return 0;
}

LONG riptide_if_nametoindex(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR ifname)
{
	struct rt_netif *n;
	LONG r = 0;

	if (!ifname)
		return 0;
	rt_lock();
	n = rt_netif_find((const char *)ifname);
	if (n)
		r = (LONG)n->index;
	rt_unlock();
	return r;
}

ULONG riptide_if_indextoname(RTREG(a6) struct Library *base, RTREG(d0) LONG ifindex, RTREG(a0) STRPTR ifname)
{
	CTX;
	struct rt_netif *n;
	ULONG r = 0;

	rt_lock();
	n = rt_netif_by_index((u32)ifindex);
	if (n && ifname) {
		rt_strlcpy((char *)ifname, n->name, RT_IFNAMSIZ);
		r = (ULONG)ifname;
	}
	rt_unlock();
	if (!r)
		fail(c, RT_ENXIO);
	return r;
}

ULONG riptide_if_nameindex(RTREG(a6) struct Library *base)
{
	struct rt_netif *n;
	struct if_nameindex *ix;
	char *names;
	int cnt = 0, i = 0;

	rt_lock();
	for (n = rt_netif_list(); n; n = n->next)
		cnt++;
	ix = AllocVec((ULONG)(cnt + 1) * (sizeof(*ix) + RT_IFNAMSIZ), MEMF_PUBLIC | MEMF_CLEAR);
	if (ix) {
		names = (char *)(ix + cnt + 1);
		for (n = rt_netif_list(); n && i < cnt; n = n->next, i++) {
			ix[i].if_index = n->index;
			ix[i].if_name = names + i * RT_IFNAMSIZ;
			rt_strlcpy(ix[i].if_name, n->name, RT_IFNAMSIZ);
		}
	}
	rt_unlock();
	return (ULONG)ix;
}

void riptide_if_freenameindex(RTREG(a6) struct Library *base, RTREG(a0) APTR ptr)
{
	if (ptr)
		FreeVec(ptr);
}

LONG riptide_RiptideSupportsIPV6(RTREG(a6) struct Library *base)
{
	return 1;      /* AF_INET6 sockets, sockaddr_in6, getaddrinfo() & co. */
}

LONG riptide_RiptideResGetOptions(RTREG(a6) struct Library *base)
{
	return res_options;
}

void riptide_RiptideResSetOptions(RTREG(a6) struct Library *base, RTREG(d0) LONG options)
{
	res_options = options;
}

LONG riptide_sockatmark(RTREG(a6) struct Library *base, RTREG(d0) LONG fd)
{
	CTX;
	LONG v = 0;
	int r = rt_api_ioctl(&c->sc, fd, RT_SIOCATMARK, &v);

	if (r < 0)
		return fail(c, -r);
	return v ? 1 : 0;
}

LONG riptide_RiptideConnectName(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR host,
                                 RTREG(d0) ULONG port, RTREG(a1) APTR tags)
{
	CTX;
	struct TagItem *t = tags;
	struct RiptideConnectResult *out = NULL;
	struct rt_he_result res;
	LONG family = RT_AF_UNSPEC, type = RT_SOCK_STREAM;
	ULONG timeout = 0;
	int r;

	while (t && t->ti_Tag != TAG_DONE) {
		switch (t->ti_Tag) {
		case TAG_IGNORE:
			t++;
			continue;
		case TAG_MORE:
			t = (struct TagItem *)t->ti_Data;
			continue;
		case TAG_SKIP:
			t += 1 + t->ti_Data;
			continue;
		case RTCN_Family:
			family = (LONG)t->ti_Data;
			break;
		case RTCN_Type:
			type = (LONG)t->ti_Data;
			break;
		case RTCN_Timeout:
			timeout = t->ti_Data;
			break;
		case RTCN_Result:
			out = (struct RiptideConnectResult *)t->ti_Data;
			break;
		}
		t++;
	}
	if (!host || port == 0 || port > 65535 ||
	    (family != RT_AF_UNSPEC && family != RT_AF_INET && family != RT_AF_INET6) ||
	    (type != RT_SOCK_STREAM && type != RT_SOCK_DGRAM))
		return fail(c, RT_EINVAL);
	r = rt_he_connect(&c->sc, (const char *)host, (u16)port, (int)family, (int)type, (u32)timeout, &res);
	if (r < 0) {
		bsd_set_errno(c, -r == RT_HOST_NOT_FOUND || -r == RT_NO_DATA || -r == RT_TRY_AGAIN ||
		              -r == RT_NO_RECOVERY ? RT_EHOSTUNREACH : -r);
		return -1;
	}
	if (out) {
		memcpy(out->rcr_Addr, res.addr.b, 16);
		out->rcr_Millis = res.ms;
		out->rcr_Family = rt_in6_is_v4mapped(&res.addr) ? RT_AF_INET : RT_AF_INET6;
		out->rcr_NumAddrs = res.naddrs;
		out->rcr_Attempts = res.attempts;
		out->rcr_Index = res.index;
	}
	return r;
}

/* RiptideSendMMsg/RecvMMsg: through rt_mmsg in chunks (one stack lock
 * each); struct iovec has the layout of rt_iov */
#define MMSG_CHUNK 16

static LONG mmsg(struct Library *base, LONG fd, struct RiptideMMsgHdr *vec, ULONG vlen, ULONG flags, int rx)
{
	CTX;
	struct rt_mmsg m[MMSG_CHUNK];
	ULONG done = 0, i, k;
	int r = 0;

	if (vlen && !vec)
		return fail(c, RT_EFAULT);
	while (done < vlen) {
		k = vlen - done < MMSG_CHUNK ? vlen - done : MMSG_CHUNK;
		for (i = 0; i < k; i++) {
			struct msghdr *h = &vec[done + i].rmm_Hdr;
			m[i].iov = (struct rt_iov *)h->msg_iov;
			m[i].iovcnt = (int)h->msg_iovlen;
			m[i].name = h->msg_name;
			m[i].namelen = h->msg_name ? h->msg_namelen : 0;
			m[i].len = 0;
			m[i].msgflags = 0;
		}
		/* after the first chunk a receive takes only what is queued */
		r = rx ? rt_api_recvmmsg(&c->sc, (int)fd, m, k, (int)flags | (done ? RT_MSG_DONTWAIT : 0)) :
		         rt_api_sendmmsg(&c->sc, (int)fd, m, k, (int)flags);
		if (r <= 0)
			break;
		for (i = 0; i < (ULONG)r; i++) {
			vec[done + i].rmm_Len = m[i].len;
			if (rx) {
				vec[done + i].rmm_Hdr.msg_flags = m[i].msgflags;
				if (vec[done + i].rmm_Hdr.msg_name)
					vec[done + i].rmm_Hdr.msg_namelen = m[i].namelen;
			}
		}
		done += (ULONG)r;
		if ((ULONG)r < k)
			break;
	}
	if (done)
		return (LONG)done;
	if (r < 0)
		return fail(c, -r);
	return 0;
}

LONG riptide_RiptideSendMMsg(RTREG(a6) struct Library *base, RTREG(d0) LONG fd, RTREG(a0) APTR vec,
                             RTREG(d1) ULONG vlen, RTREG(d2) ULONG flags)
{
	return mmsg(base, fd, vec, vlen, flags, 0);
}

LONG riptide_RiptideRecvMMsg(RTREG(a6) struct Library *base, RTREG(d0) LONG fd, RTREG(a0) APTR vec,
                             RTREG(d1) ULONG vlen, RTREG(d2) ULONG flags)
{
	return mmsg(base, fd, vec, vlen, flags, 1);
}

void riptide_RiptideSupportedCPUs(RTREG(a6) struct Library *base, RTREG(a0) ULONG *apis,
                              RTREG(a1) ULONG *callbacks, RTREG(a2) ULONG *kernel)
{
	if (apis)
		*apis = RIPTIDECPU_M68KREG;
	if (callbacks)
		*callbacks = RIPTIDECPU_M68KREG;
	if (kernel)
		*kernel = RIPTIDECPU_M68KREG;
}

LONG riptide_RiptideGetFdCallback(RTREG(a6) struct Library *base, RTREG(a0) APTR *cbptr)
{
	CTX;
	if (cbptr)
		*cbptr = c->fdcb;
	return c->fdcb ? c->fdcb_cpu : 0;
}

LONG riptide_RiptideSetFdCallback(RTREG(a6) struct Library *base, RTREG(a0) APTR cbptr, RTREG(d0) LONG cputype)
{
	CTX;
	if (cbptr && cputype != RIPTIDECPU_M68KREG)
		return 0;
	amiga_set_fdcb(c, cbptr, cputype);
	return 1;
}

ULONG riptide_libopen(RTREG(a6) struct Library *base, RTREG(d0) ULONG version)
{
	/* exec calls Open on a master: riptide.library's or the alias' */
	return (ULONG)rtlib_open(base == rtlib_compat.master ? &rtlib_compat : &rtlib_riptide, base);
}

ULONG riptide_libclose(RTREG(a6) struct Library *base)
{
	return rtlib_close((struct RtBase *)base);
}
