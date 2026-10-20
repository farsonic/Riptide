/*
 * Riptide - miami.library (version 21, the MiamiDx API). The documented
 * functions of the Miami SDK 2.11 are implemented; the reserved and the
 * MiamiDx-private vectors (up to -522) return 0. Socket related calls use
 * the per-task context shared with bsdsocket.library.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/miami_impl.h"

struct RtLibDef rtlib_miami = {
	"miami.library",
	"miami.library 21.1 (" RIPTIDE_DATE ") Riptide " RIPTIDE_VERSION_STRING "\r\n",
	21, 1, miami_vectors, sizeof(struct RtBase), 1, NULL, NULL, NULL, { NULL, NULL, NULL }
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

LONG miami_MiamiSysCtl(RTREG(a6) struct Library *base, RTREG(a0) LONG *name, RTREG(d0) ULONG namelen,
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

void miami_MiamiDisallowDNS(RTREG(a6) struct Library *base, RTREG(d0) LONG value)
{
	rt_dns_disallowed = value != 0;
}

ULONG miami_MiamiGetPid(RTREG(a6) struct Library *base)
{
	return (ULONG)base_ctx(base);
}

ULONG miami_MiamiPFAddHook(RTREG(a6) struct Library *base, RTREG(a0) APTR hook,
                           RTREG(a1) CONST_STRPTR name, RTREG(a2) APTR tags)
{
	return 0;      /* packet monitoring is not supported */
}

void miami_MiamiPFRemoveHook(RTREG(a6) struct Library *base, RTREG(a0) APTR handle)
{
}

LONG miami_MiamiGetHardwareLen(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
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

ULONG miami_MiamiOpenSSL(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	return 0;
}

void miami_MiamiCloseSSL(RTREG(a6) struct Library *base)
{
}

LONG miami_MiamiSetSocksConn(RTREG(a6) struct Library *base, RTREG(a0) APTR sa, RTREG(d0) LONG len)
{
	return 0;      /* no SOCKS */
}

static int netif_online(const struct rt_netif *n)
{
	return (n->flags & (RT_IFF_UP | RT_IFF_RUNNING)) == (RT_IFF_UP | RT_IFF_RUNNING) ||
	       (n->type == RT_IFT_LOOP && (n->flags & RT_IFF_UP));
}

LONG miami_MiamiIsOnline(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	struct rt_netif *n;
	LONG r = 0;

	rt_lock();
	if (!name || !strcmp((const char *)name, "mi0")) {
		/* MiamiDx: "mi0" or NULL means any (non loopback) interface */
		for (n = rt_netif_list(); n; n = n->next)
			if (n->type != RT_IFT_LOOP && netif_online(n))
				r = 1;
	} else if ((n = rt_netif_find((const char *)name)) != NULL) {
		r = netif_online(n);
	}
	rt_unlock();
	return r;
}

void miami_MiamiOnOffline(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name, RTREG(d0) LONG online)
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

ULONG miami_inet_ntop(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) const APTR addrptr,
                      RTREG(a1) STRPTR strptr, RTREG(d1) LONG len)
{
	CTX;
	char buf[16];
	u32 a;

	if (family != RT_AF_INET) {
		fail(c, RT_EAFNOSUPPORT);
		return 0;
	}
	if (!addrptr || !strptr)
		return 0;
	memcpy(&a, addrptr, 4);
	rt_format_ipv4(a, buf);
	if ((LONG)strlen(buf) >= len) {
		fail(c, RT_ENOSPC);
		return 0;
	}
	strcpy((char *)strptr, buf);
	return (ULONG)strptr;
}

LONG miami_inet_aton(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR cp, RTREG(a1) APTR addr)
{
	u32 a;

	if (!amiga_inet_aton((const char *)cp, &a))
		return 0;
	if (addr)
		memcpy(addr, &a, 4);
	return 1;
}

LONG miami_inet_pton(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) CONST_STRPTR strptr,
                     RTREG(a1) APTR addrptr)
{
	CTX;
	u32 a;

	if (family != RT_AF_INET)
		return fail(c, RT_EAFNOSUPPORT);
	if (!strptr || rt_parse_ipv4((const char *)strptr, &a) < 0)
		return 0;
	if (addrptr)
		memcpy(addrptr, &a, 4);
	return 1;
}

ULONG miami_gethostbyname2(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name, RTREG(d0) LONG fam)
{
	CTX;
	u32 addrs[8];
	char cname[256];
	int r;

	if (fam != RT_AF_INET || !name) {
		c->herrno = HOST_NOT_FOUND;
		return 0;
	}
	cname[0] = 0;
	r = rt_resolve_name((const char *)name, addrs, 8, cname, sizeof(cname));
	if (r <= 0) {
		c->herrno = r == -RT_TRY_AGAIN ? TRY_AGAIN : r == -RT_NO_DATA || r == 0 ? NO_DATA : HOST_NOT_FOUND;
		if (c->herrnoptr)
			*c->herrnoptr = c->herrno;
		return 0;
	}
	return (ULONG)bsd_make_hostent(c, cname[0] ? cname : (const char *)name, addrs, r);
}

ULONG miami_gai_strerror(RTREG(a6) struct Library *base, RTREG(d0) LONG err)
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

void miami_freeaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) APTR aihead)
{
	struct addrinfo *ai = aihead, *next;

	while (ai) {
		next = ai->ai_next;
		FreeVec(ai);
		ai = next;
	}
}

static int service_port(const char *serv, int socktype, u16 *port)
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
	si = rt_getservbyname(serv, socktype == RT_SOCK_DGRAM ? "udp" : "tcp");
	if (!si)
		return EAI_SERVICE;
	*port = si->port;
	return 0;
}

static struct addrinfo *ai_new(const struct addrinfo *hints, int socktype, u32 addr, u16 port,
                               const char *canon)
{
	u32 cl = canon ? (u32)strlen(canon) + 1 : 0;
	struct addrinfo *ai = AllocVec(sizeof(*ai) + sizeof(struct rt_sockaddr_in) + cl,
	                               MEMF_PUBLIC | MEMF_CLEAR);
	struct rt_sockaddr_in *sin;

	if (!ai)
		return NULL;
	sin = (struct rt_sockaddr_in *)(ai + 1);
	sin->sin_len = sizeof(*sin);
	sin->sin_family = RT_AF_INET;
	sin->sin_port = rt_htons(port);
	sin->sin_addr = addr;
	ai->ai_flags = hints ? hints->ai_flags : 0;
	ai->ai_family = RT_AF_INET;
	ai->ai_socktype = socktype;
	ai->ai_protocol = socktype == RT_SOCK_DGRAM ? 17 : socktype == RT_SOCK_STREAM ? 6 : 0;
	if (hints && hints->ai_protocol)
		ai->ai_protocol = hints->ai_protocol;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = sin;
	if (canon) {
		ai->ai_canonname = (char *)(sin + 1);
		memcpy(ai->ai_canonname, canon, cl);
	}
	return ai;
}

LONG miami_getaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR hostname,
                       RTREG(a1) CONST_STRPTR servname, RTREG(a2) const APTR hintsp, RTREG(a3) APTR *result)
{
	const struct addrinfo *hints = hintsp;
	struct addrinfo *head = NULL, **tail = &head;
	const char *host = (const char *)hostname, *serv = (const char *)servname;
	int types[2], ntypes = 0, i, j, naddr, e;
	u32 addrs[8];
	char cname[256];
	LONG flags = hints ? hints->ai_flags : 0;

	if (!result)
		return EAI_FAIL;
	*result = NULL;
	if (hints) {
		if (hints->ai_family != RT_AF_UNSPEC && hints->ai_family != RT_AF_INET)
			return EAI_FAMILY;
		if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST))
			return EAI_BADFLAGS;
		if (hints->ai_socktype && hints->ai_socktype != RT_SOCK_STREAM &&
		    hints->ai_socktype != RT_SOCK_DGRAM && hints->ai_socktype != RT_SOCK_RAW)
			return EAI_SOCKTYPE;
	}
	if (!host && !serv)
		return EAI_NONAME;
	if (hints && hints->ai_socktype)
		types[ntypes++] = hints->ai_socktype;
	else {
		types[ntypes++] = RT_SOCK_STREAM;
		types[ntypes++] = RT_SOCK_DGRAM;
	}
	cname[0] = 0;
	if (!host) {
		addrs[0] = (flags & AI_PASSIVE) ? 0 : rt_htonl(0x7f000001u);
		naddr = 1;
	} else if (amiga_inet_aton(host, &addrs[0])) {
		naddr = 1;
		rt_strlcpy(cname, host, sizeof(cname));
	} else if (flags & AI_NUMERICHOST) {
		return EAI_NONAME;
	} else {
		naddr = rt_resolve_name(host, addrs, 8, cname, sizeof(cname));
		if (naddr == -RT_TRY_AGAIN)
			return EAI_AGAIN;
		if (naddr == -RT_NO_DATA || naddr == 0)
			return EAI_NODATA;
		if (naddr == -RT_HOST_NOT_FOUND)
			return EAI_NONAME;
		if (naddr < 0)
			return EAI_FAIL;
		if (!cname[0])
			rt_strlcpy(cname, host, sizeof(cname));
	}
	for (i = 0; i < naddr; i++) {
		for (j = 0; j < ntypes; j++) {
			u16 port;
			struct addrinfo *ai;
			if (types[j] == RT_SOCK_RAW) {
				if (serv)
					continue;
				port = 0;
			} else if ((e = service_port(serv, types[j], &port)) != 0) {
				if (ntypes == 1) {
					miami_freeaddrinfo(base, head);
					return e;
				}
				continue;
			}
			ai = ai_new(hints, types[j], addrs[i], port,
			            (head == NULL && (flags & AI_CANONNAME)) ? cname : NULL);
			if (!ai) {
				miami_freeaddrinfo(base, head);
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

LONG miami_getnameinfo(RTREG(a6) struct Library *base, RTREG(a0) const APTR sa, RTREG(d0) LONG salen,
                       RTREG(a1) STRPTR host, RTREG(d1) LONG hostlen, RTREG(a2) STRPTR serv,
                       RTREG(d2) LONG servlen, RTREG(d3) LONG flags)
{
	const struct rt_sockaddr_in *sin = sa;
	char name[256];

	if (!sin || salen < (LONG)sizeof(*sin))
		return EAI_FAIL;
	if (sin->sin_family != RT_AF_INET)
		return EAI_FAMILY;
	if (host && hostlen > 0) {
		int done = 0;
		if (!(flags & NI_NUMERICHOST) && rt_resolve_addr(sin->sin_addr, name, sizeof(name)) == 0) {
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
			rt_format_ipv4(sin->sin_addr, name);
		}
		if ((LONG)strlen(name) >= hostlen)
			return EAI_MEMORY;
		strcpy((char *)host, name);
	}
	if (serv && servlen > 0) {
		const struct rt_servent_info *si = NULL;
		u16 port = rt_ntohs(sin->sin_port);
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

LONG miami_if_nametoindex(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR ifname)
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

ULONG miami_if_indextoname(RTREG(a6) struct Library *base, RTREG(d0) LONG ifindex, RTREG(a0) STRPTR ifname)
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

ULONG miami_if_nameindex(RTREG(a6) struct Library *base)
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

void miami_if_freenameindex(RTREG(a6) struct Library *base, RTREG(a0) APTR ptr)
{
	if (ptr)
		FreeVec(ptr);
}

LONG miami_MiamiSupportsIPV6(RTREG(a6) struct Library *base)
{
	return 0;
}

LONG miami_MiamiResGetOptions(RTREG(a6) struct Library *base)
{
	return res_options;
}

void miami_MiamiResSetOptions(RTREG(a6) struct Library *base, RTREG(d0) LONG options)
{
	res_options = options;
}

LONG miami_sockatmark(RTREG(a6) struct Library *base, RTREG(d0) LONG fd)
{
	CTX;
	LONG v = 0;
	int r = rt_api_ioctl(&c->sc, fd, RT_SIOCATMARK, &v);

	if (r < 0)
		return fail(c, -r);
	return v ? 1 : 0;
}

void miami_MiamiSupportedCPUs(RTREG(a6) struct Library *base, RTREG(a0) ULONG *apis,
                              RTREG(a1) ULONG *callbacks, RTREG(a2) ULONG *kernel)
{
	if (apis)
		*apis = MIAMICPU_M68KREG;
	if (callbacks)
		*callbacks = MIAMICPU_M68KREG;
	if (kernel)
		*kernel = MIAMICPU_M68KREG;
}

LONG miami_MiamiGetFdCallback(RTREG(a6) struct Library *base, RTREG(a0) APTR *cbptr)
{
	CTX;
	if (cbptr)
		*cbptr = c->fdcb;
	return c->fdcb ? c->fdcb_cpu : 0;
}

LONG miami_MiamiSetFdCallback(RTREG(a6) struct Library *base, RTREG(a0) APTR cbptr, RTREG(d0) LONG cputype)
{
	CTX;
	if (cbptr && cputype != MIAMICPU_M68KREG)
		return 0;
	amiga_set_fdcb(c, cbptr, cputype);
	return 1;
}

ULONG miami_libopen(RTREG(a6) struct Library *base, RTREG(d0) ULONG version)
{
	return (ULONG)rtlib_open(&rtlib_miami, base);
}

ULONG miami_libclose(RTREG(a6) struct Library *base)
{
	return rtlib_close((struct RtBase *)base);
}
