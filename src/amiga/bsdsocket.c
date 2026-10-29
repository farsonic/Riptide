/*
 * Riptide - bsdsocket.library (AmiTCP 4 API, 46 functions; the Roadshow
 * extensions after them are in roadshow.c).
 *
 * Every function runs in the calling task. Results follow the AmiTCP
 * conventions: -1 and the per-task errno (also written through the
 * pointer registered with SBTC_ERRNO*PTR / SetErrnoPtr) on failure.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/bsdsocket_impl.h"

#define RELEASE_STRING "Riptide " RIPTIDE_VERSION_STRING " (" RIPTIDE_DATE ")"
#define INADDR_NONE 0xffffffffUL

struct RtLibDef rtlib_bsdsocket = {
	"bsdsocket.library",
	"bsdsocket.library 4.1 (" RIPTIDE_DATE ") " RELEASE_STRING "\r\n",
	4, 1, bsdsocket_vectors, sizeof(struct RtBase), 1, NULL, NULL, NULL, { NULL, NULL, NULL }
};

/* struct iovec and struct rt_iov must have the same layout */
typedef char iov_layout_check[(sizeof(struct iovec) == sizeof(struct rt_iov) &&
                               offsetof(struct rt_iov, len) == offsetof(struct iovec, iov_len)) ? 1 : -1];

/* ---- errno ---- */

void bsd_set_errno(struct TaskCtx *c, LONG e)
{
	c->errno_ = e;
	if (c->errnoptr) {
		switch (c->errnosize) {
		case 1: *(UBYTE *)c->errnoptr = (UBYTE)e; break;
		case 2: *(UWORD *)c->errnoptr = (UWORD)e; break;
		case 4: *(ULONG *)c->errnoptr = (ULONG)e; break;
		}
	}
	if (c->errhook)
		amiga_rs_error_hook(c, 1, e);
}

static void set_herrno(struct TaskCtx *c, LONG h)
{
	c->herrno = h;
	if (c->herrnoptr)
		*c->herrnoptr = h;
	if (c->errhook)
		amiga_rs_error_hook(c, 2, h);
}

static LONG ret(struct TaskCtx *c, int r)
{
	if (r < 0) {
		bsd_set_errno(c, -r);
		return -1;
	}
	return r;
}

int bsd_ctx_init(struct TaskCtx *c)
{
	c->logmask = 0xff;
	c->logfacility = LOG_USER;
	c->logtag = NULL;
	return 0;
}

#define CTX struct TaskCtx *c = base_ctx(base)

/* ---- sockets ---- */

LONG bsd_socket(RTREG(a6) struct Library *base, RTREG(d0) LONG domain, RTREG(d1) LONG type,
                RTREG(d2) LONG protocol)
{
	CTX;
	return ret(c, rt_api_socket(&c->sc, domain, type, protocol));
}

LONG bsd_bind(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) const APTR name,
              RTREG(d1) LONG namelen)
{
	CTX;
	LONG e;

	if ((amiga_rs_caller_hooks & (1 << 6)) && (e = amiga_rs_mon_connect(c, 1, s, name, namelen)) != 0)
		return ret(c, -e);
	return ret(c, rt_api_bind(&c->sc, s, (const struct rt_sockaddr_in *)name, (u32)namelen));
}

LONG bsd_listen(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(d1) LONG backlog)
{
	CTX;
	return ret(c, rt_api_listen(&c->sc, s, backlog));
}

LONG bsd_accept(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR addr,
                RTREG(a1) LONG *addrlen)
{
	CTX;
	u32 len = (addr && addrlen) ? (u32)*addrlen : 0;
	int r = rt_api_accept(&c->sc, s, (struct rt_sockaddr_in *)addr, addr && addrlen ? &len : NULL);

	if (r >= 0 && addr && addrlen)
		*addrlen = (LONG)len;
	return ret(c, r);
}

LONG bsd_connect(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) const APTR name,
                 RTREG(d1) LONG namelen)
{
	CTX;
	LONG e;

	if ((amiga_rs_caller_hooks & (1 << 3)) && (e = amiga_rs_mon_connect(c, 0, s, name, namelen)) != 0)
		return ret(c, -e);
	return ret(c, rt_api_connect(&c->sc, s, (const struct rt_sockaddr_in *)name, (u32)namelen));
}

LONG bsd_sendto(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) const APTR msg,
                RTREG(d1) LONG len, RTREG(d2) LONG flags, RTREG(a1) const APTR to,
                RTREG(d3) LONG tolen)
{
	CTX;
	LONG e;

	if ((amiga_rs_caller_hooks & (1 << 4)) &&
	    (e = amiga_rs_mon_send(c, s, msg, len, flags, to, tolen, NULL)) != 0)
		return ret(c, -e);
	return ret(c, rt_api_sendto(&c->sc, s, msg, (u32)len, flags,
	                            (const struct rt_sockaddr_in *)to, (u32)tolen));
}

LONG bsd_send(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) const APTR msg,
              RTREG(d1) LONG len, RTREG(d2) LONG flags)
{
	CTX;
	LONG e;

	if ((amiga_rs_caller_hooks & (1 << 4)) &&
	    (e = amiga_rs_mon_send(c, s, msg, len, flags, NULL, 0, NULL)) != 0)
		return ret(c, -e);
	return ret(c, rt_api_send(&c->sc, s, msg, (u32)len, flags));
}

LONG bsd_recvfrom(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR buf,
                  RTREG(d1) LONG len, RTREG(d2) LONG flags, RTREG(a1) APTR from,
                  RTREG(a2) LONG *fromlen)
{
	CTX;
	u32 fl = (from && fromlen) ? (u32)*fromlen : 0;
	int r = rt_api_recvfrom(&c->sc, s, buf, (u32)len, flags, (struct rt_sockaddr_in *)from,
	                        from && fromlen ? &fl : NULL);

	if (r >= 0 && from && fromlen)
		*fromlen = (LONG)fl;
	return ret(c, r);
}

LONG bsd_recv(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR buf,
              RTREG(d1) LONG len, RTREG(d2) LONG flags)
{
	CTX;
	return ret(c, rt_api_recv(&c->sc, s, buf, (u32)len, flags));
}

LONG bsd_shutdown(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(d1) LONG how)
{
	CTX;
	return ret(c, rt_api_shutdown(&c->sc, s, how));
}

LONG bsd_setsockopt(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(d1) LONG level,
                    RTREG(d2) LONG optname, RTREG(a0) const APTR optval, RTREG(d3) LONG optlen)
{
	CTX;
	return ret(c, rt_api_setsockopt(&c->sc, s, level, optname, optval, (u32)optlen));
}

LONG bsd_getsockopt(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(d1) LONG level,
                    RTREG(d2) LONG optname, RTREG(a0) APTR optval, RTREG(a1) LONG *optlen)
{
	CTX;
	u32 l = optlen ? (u32)*optlen : 0;
	int r = rt_api_getsockopt(&c->sc, s, level, optname, optval, &l);

	if (r >= 0 && optlen)
		*optlen = (LONG)l;
	return ret(c, r);
}

LONG bsd_getsockname(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR hostname,
                     RTREG(a1) LONG *namelen)
{
	CTX;
	u32 l = namelen ? (u32)*namelen : 0;
	int r = rt_api_getsockname(&c->sc, s, (struct rt_sockaddr_in *)hostname, &l);

	if (r >= 0 && namelen)
		*namelen = (LONG)l;
	return ret(c, r);
}

LONG bsd_getpeername(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR hostname,
                     RTREG(a1) LONG *namelen)
{
	CTX;
	u32 l = namelen ? (u32)*namelen : 0;
	int r = rt_api_getpeername(&c->sc, s, (struct rt_sockaddr_in *)hostname, &l);

	if (r >= 0 && namelen)
		*namelen = (LONG)l;
	return ret(c, r);
}

LONG bsd_IoctlSocket(RTREG(a6) struct Library *base, RTREG(d0) LONG d, RTREG(d1) ULONG request,
                     RTREG(a0) APTR argp)
{
	CTX;
	return ret(c, rt_api_ioctl(&c->sc, d, request, argp));
}

LONG bsd_CloseSocket(RTREG(a6) struct Library *base, RTREG(d0) LONG d)
{
	CTX;
	return ret(c, rt_api_close(&c->sc, d));
}

LONG bsd_WaitSelect(RTREG(a6) struct Library *base, RTREG(d0) LONG nfds, RTREG(a0) APTR readfds,
                    RTREG(a1) APTR writefds, RTREG(a2) APTR exceptfds, RTREG(a3) APTR timeout,
                    RTREG(d1) ULONG *sigmask)
{
	CTX;
	s32 ms = -1;

	if (timeout) {
		const struct timeval *tv = timeout;
		u32 secs = tv->tv_secs + tv->tv_micro / 1000000u;
		u32 us = tv->tv_micro % 1000000u;
		if (secs >= 0x7fffffffu / 1000u - 1)
			ms = 0x7ffffffe;
		else
			ms = (s32)(secs * 1000u + (us + 999u) / 1000u);
	}
	return ret(c, rt_api_select(&c->sc, nfds, readfds, writefds, exceptfds, ms, (u32 *)sigmask));
}

void bsd_SetSocketSignals(RTREG(a6) struct Library *base, RTREG(d0) ULONG intrmask,
                          RTREG(d1) ULONG iomask, RTREG(d2) ULONG urgmask)
{
	CTX;
	c->sc.sigintr_mask = intrmask;
	c->sc.sigio_mask = iomask;
	c->sc.sigurg_mask = urgmask;
}

LONG bsd_getdtablesize(RTREG(a6) struct Library *base)
{
	CTX;
	return c->sc.nfds;
}

LONG bsd_ObtainSocket(RTREG(a6) struct Library *base, RTREG(d0) LONG id, RTREG(d1) LONG domain,
                      RTREG(d2) LONG type, RTREG(d3) LONG protocol)
{
	CTX;
	return ret(c, rt_api_obtain(&c->sc, id, domain, type, protocol));
}

LONG bsd_ReleaseSocket(RTREG(a6) struct Library *base, RTREG(d0) LONG fd, RTREG(d1) LONG id)
{
	CTX;
	return ret(c, rt_api_release(&c->sc, fd, id, 0));
}

LONG bsd_ReleaseCopyOfSocket(RTREG(a6) struct Library *base, RTREG(d0) LONG fd, RTREG(d1) LONG id)
{
	CTX;
	return ret(c, rt_api_release(&c->sc, fd, id, 1));
}

LONG bsd_Errno(RTREG(a6) struct Library *base)
{
	CTX;
	return c->errno_;
}

void bsd_SetErrnoPtr(RTREG(a6) struct Library *base, RTREG(a0) APTR errno_p, RTREG(d0) LONG size)
{
	CTX;
	if (size == 1 || size == 2 || size == 4) {
		c->errnoptr = errno_p;
		c->errnosize = size;
	}
}

/* ---- address conversion (4.4BSD semantics) ---- */

#define IN_CLASSA(i) (((u32)(i) & 0x80000000u) == 0)
#define IN_CLASSB(i) (((u32)(i) & 0xc0000000u) == 0x80000000u)
#define IN_CLASSC(i) (((u32)(i) & 0xe0000000u) == 0xc0000000u)

/* inet_aton(): a, a.b, a.b.c, a.b.c.d; decimal, 0octal, 0xhex. Result in
 * network order. Returns 1 on success. */
int amiga_inet_aton(const char *cp, u32 *addr)
{
	u32 parts[4], val;
	int n = 0;

	if (!cp)
		return 0;
	for (;;) {
		const char *end;
		int base = 10;
		if (*cp < '0' || *cp > '9')
			return 0;
		if (*cp == '0') {
			if (cp[1] == 'x' || cp[1] == 'X') {
				base = 16;
				cp += 2;
				if (!((*cp >= '0' && *cp <= '9') || ((*cp | 0x20) >= 'a' && (*cp | 0x20) <= 'f')))
					return 0;
			} else {
				base = 8;
			}
		}
		val = (u32)rt_strtoul(cp, &end, base);
		if (end == cp && base != 8)
			return 0;
		cp = end;
		if (*cp == '.') {
			if (n >= 3 || val > 255)
				return 0;
			parts[n++] = val;
			cp++;
			continue;
		}
		break;
	}
	if (*cp && *cp != ' ' && *cp != '\t' && *cp != '\n')
		return 0;
	switch (n) {
	case 0:
		break;
	case 1:
		if (val > 0xffffffu)
			return 0;
		val |= parts[0] << 24;
		break;
	case 2:
		if (val > 0xffffu)
			return 0;
		val |= (parts[0] << 24) | (parts[1] << 16);
		break;
	case 3:
		if (val > 0xffu)
			return 0;
		val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
		break;
	}
	if (addr)
		*addr = rt_htonl(val);
	return 1;
}

STRPTR amiga_ntoa(u32 in_net, char *buf)
{
	return (STRPTR)rt_format_ipv4(in_net, buf);
}

ULONG bsd_Inet_NtoA(RTREG(a6) struct Library *base, RTREG(d0) ULONG in)
{
	CTX;
	return (ULONG)amiga_ntoa(in, c->ntoa);
}

ULONG bsd_inet_addr(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR cp)
{
	u32 a;

	return amiga_inet_aton((const char *)cp, &a) ? a : INADDR_NONE;
}

ULONG bsd_Inet_LnaOf(RTREG(a6) struct Library *base, RTREG(d0) ULONG in)
{
	u32 i = rt_ntohl(in);

	if (IN_CLASSA(i))
		return i & 0x00ffffffu;
	if (IN_CLASSB(i))
		return i & 0x0000ffffu;
	return i & 0x000000ffu;
}

ULONG bsd_Inet_NetOf(RTREG(a6) struct Library *base, RTREG(d0) ULONG in)
{
	u32 i = rt_ntohl(in);

	if (IN_CLASSA(i))
		return (i & 0xff000000u) >> 24;
	if (IN_CLASSB(i))
		return (i & 0xffff0000u) >> 16;
	return (i & 0xffffff00u) >> 8;
}

ULONG bsd_Inet_MakeAddr(RTREG(a6) struct Library *base, RTREG(d0) ULONG net, RTREG(d1) ULONG host)
{
	u32 a;

	if (net < 128)
		a = (net << 24) | (host & 0x00ffffffu);
	else if (net < 65536)
		a = (net << 16) | (host & 0x0000ffffu);
	else if (net < 16777216)
		a = (net << 8) | (host & 0x000000ffu);
	else
		a = net | host;
	return rt_htonl(a);
}

ULONG bsd_inet_network(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR cp)
{
	const char *p = (const char *)cp;
	u32 val = 0, part;
	int n = 0;

	if (!p)
		return INADDR_NONE;
	for (;;) {
		const char *end;
		int b = 10;
		if (*p == '0') {
			b = 8;
			if (p[1] == 'x' || p[1] == 'X') {
				b = 16;
				p += 2;
			}
		}
		part = (u32)rt_strtoul(p, &end, b);
		if (end == p && b != 8)
			return INADDR_NONE;
		if (part > 255 || n >= 4)
			return INADDR_NONE;
		val = (val << 8) | part;
		n++;
		p = end;
		if (*p == '.') {
			p++;
			continue;
		}
		break;
	}
	if (*p && *p != ' ' && *p != '\t')
		return INADDR_NONE;
	return val;      /* host order, like 4.4BSD */
}

/* ---- netdb ---- */

struct hostent *bsd_make_hostent(struct TaskCtx *c, const char *name, const u32 *addrs, int n)
{
	int i;

	if (n > 8)
		n = 8;
	rt_strlcpy(c->host.name, name ? name : "", sizeof(c->host.name));
	c->host.aliases[0] = NULL;
	for (i = 0; i < n; i++) {
		c->host.addr[i] = addrs[i];
		c->host.addrs[i] = (char *)&c->host.addr[i];
	}
	c->host.addrs[n] = NULL;
	c->host.he.h_name = c->host.name;
	c->host.he.h_aliases = c->host.aliases;
	c->host.he.h_addrtype = RT_AF_INET;
	c->host.he.h_length = 4;
	c->host.he.h_addr_list = c->host.addrs;
	return &c->host.he;
}

struct hostent *bsd_make_hostent6(struct TaskCtx *c, const char *name, const rt_in6_addr *addrs, int n)
{
	int i;

	if (n > 8)
		n = 8;
	bsd_make_hostent(c, name, NULL, 0);
	for (i = 0; i < n; i++) {
		memcpy(&c->host.addr[i * 4], addrs[i].b, 16);
		c->host.addrs[i] = (char *)&c->host.addr[i * 4];
	}
	c->host.addrs[n] = NULL;
	c->host.he.h_addrtype = RT_AF_INET6;
	c->host.he.h_length = 16;
	return &c->host.he;
}

static LONG h_from_res(int r)
{
	switch (-r) {
	case RT_HOST_NOT_FOUND: return HOST_NOT_FOUND;
	case RT_TRY_AGAIN: return TRY_AGAIN;
	case RT_NO_RECOVERY: return NO_RECOVERY;
	case RT_NO_DATA: return NO_DATA;
	}
	return NO_RECOVERY;
}

ULONG bsd_gethostbyname(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	CTX;
	u32 addrs[8];
	char cname[256];
	int r;

	if (!name || !*name) {
		set_herrno(c, HOST_NOT_FOUND);
		return 0;
	}
	cname[0] = 0;
	r = rt_resolve_name((const char *)name, addrs, 8, cname, sizeof(cname));
	if (r <= 0) {
		set_herrno(c, r < 0 ? h_from_res(r) : NO_DATA);
		return 0;
	}
	set_herrno(c, 0);
	return (ULONG)bsd_make_hostent(c, cname[0] ? cname : (const char *)name, addrs, r);
}

ULONG bsd_gethostbyaddr(RTREG(a6) struct Library *base, RTREG(a0) const APTR addr,
                        RTREG(d0) LONG len, RTREG(d1) LONG type)
{
	CTX;
	char name[256];
	u32 a;
	int r;

	if (addr && type == RT_AF_INET6 && len == 16) {
		rt_in6_addr a6;
		memcpy(a6.b, addr, 16);
		r = rt_resolve_addr6(&a6, name, sizeof(name));
		if (r < 0) {
			set_herrno(c, h_from_res(r));
			return 0;
		}
		set_herrno(c, 0);
		return (ULONG)bsd_make_hostent6(c, name, &a6, 1);
	}
	if (!addr || type != RT_AF_INET || len != 4) {
		set_herrno(c, HOST_NOT_FOUND);
		return 0;
	}
	memcpy(&a, addr, 4);
	r = rt_resolve_addr(a, name, sizeof(name));
	if (r < 0) {
		set_herrno(c, h_from_res(r));
		return 0;
	}
	set_herrno(c, 0);
	return (ULONG)bsd_make_hostent(c, name, &a, 1);
}

static struct netent *loopback_net(struct TaskCtx *c)
{
	c->net.aliases[0] = NULL;
	c->net.ne.n_name = (char *)"loopback";
	c->net.ne.n_aliases = c->net.aliases;
	c->net.ne.n_addrtype = RT_AF_INET;
	c->net.ne.n_net = 127;
	return &c->net.ne;
}

ULONG bsd_getnetbyname(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	CTX;
	if (name && !rt_strcasecmp((const char *)name, "loopback"))
		return (ULONG)loopback_net(c);
	return 0;
}

ULONG bsd_getnetbyaddr(RTREG(a6) struct Library *base, RTREG(d0) ULONG net, RTREG(d1) LONG type)
{
	CTX;
	if (net == 127 && type == RT_AF_INET)
		return (ULONG)loopback_net(c);
	return 0;
}

ULONG bsd_make_servent(struct TaskCtx *c, const struct rt_servent_info *si)
{
	int i;

	if (!si)
		return 0;
	for (i = 0; i < 4 && si->aliases[i]; i++)
		c->serv.aliases[i] = (char *)si->aliases[i];
	c->serv.aliases[i] = NULL;
	c->serv.se.s_name = (char *)si->name;
	c->serv.se.s_aliases = c->serv.aliases;
	c->serv.se.s_port = (LONG)rt_htons(si->port);
	c->serv.se.s_proto = (char *)si->proto;
	return (ULONG)&c->serv.se;
}

ULONG bsd_getservbyname(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name,
                        RTREG(a1) CONST_STRPTR proto)
{
	CTX;
	if (!name)
		return 0;
	return bsd_make_servent(c, rt_getservbyname((const char *)name, (const char *)proto));
}

ULONG bsd_getservbyport(RTREG(a6) struct Library *base, RTREG(d0) LONG port, RTREG(a0) CONST_STRPTR proto)
{
	CTX;
	return bsd_make_servent(c, rt_getservbyport(rt_ntohs((u16)port), (const char *)proto));
}

ULONG bsd_make_protoent(struct TaskCtx *c, const struct rt_protoent_info *pi)
{
	int i;

	if (!pi)
		return 0;
	for (i = 0; i < 4 && pi->aliases[i]; i++)
		c->proto.aliases[i] = (char *)pi->aliases[i];
	c->proto.aliases[i] = NULL;
	c->proto.pe.p_name = (char *)pi->name;
	c->proto.pe.p_aliases = c->proto.aliases;
	c->proto.pe.p_proto = pi->proto;
	return (ULONG)&c->proto.pe;
}

ULONG bsd_make_netent(struct TaskCtx *c, const struct rt_netent_info *ni)
{
	int i;

	if (!ni)
		return 0;
	for (i = 0; i < 4 && ni->aliases[i] && i < (int)RT_ARRAY_SIZE(c->net.aliases) - 1; i++)
		c->net.aliases[i] = (char *)ni->aliases[i];
	c->net.aliases[i] = NULL;
	c->net.ne.n_name = (char *)ni->name;
	c->net.ne.n_aliases = c->net.aliases;
	c->net.ne.n_addrtype = RT_AF_INET;
	c->net.ne.n_net = ni->net;
	return (ULONG)&c->net.ne;
}

ULONG bsd_getprotobyname(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR name)
{
	CTX;
	if (!name)
		return 0;
	return bsd_make_protoent(c, rt_getprotobyname((const char *)name));
}

ULONG bsd_getprotobynumber(RTREG(a6) struct Library *base, RTREG(d0) LONG proto)
{
	CTX;
	return bsd_make_protoent(c, rt_getprotobynumber(proto));
}

/* ---- syslog ---- */

/* printf-style formatting of an Amiga va_list (an array of 32-bit
 * arguments): %[-0][width][.prec][l|h]{d,i,u,x,X,o,c,s,p,%} and %m. */
static void vformat(char *buf, u32 size, const char *fmt, const ULONG *args, LONG err)
{
	u32 o = 0;

	if (!size)
		return;
	while (*fmt && o + 1 < size) {
		char spec[16], tmp[64];
		int sl = 0;
		if (*fmt != '%') {
			buf[o++] = *fmt++;
			continue;
		}
		spec[sl++] = *fmt++;
		while (*fmt && strchr("-0123456789.", *fmt) && sl < 10)
			spec[sl++] = *fmt++;
		while (*fmt == 'l' || *fmt == 'h')
			fmt++;
		if (!*fmt)
			break;
		switch (*fmt) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
			spec[sl++] = *fmt;
			spec[sl] = 0;
			if (*fmt == 'd' || *fmt == 'i')
				rt_snprintf(tmp, sizeof(tmp), spec, (int)(LONG)*args++);
			else
				rt_snprintf(tmp, sizeof(tmp), spec, (unsigned)*args++);
			break;
		case 'p':
			rt_snprintf(tmp, sizeof(tmp), "%p", (void *)*args++);
			break;
		case 's': {
			const char *s = (const char *)*args++;
			spec[sl++] = 's';
			spec[sl] = 0;
			rt_snprintf(tmp, sizeof(tmp), spec, s ? s : "(null)");
			break;
		}
		case 'm':
			rt_strlcpy(tmp, rt_strerror(err), sizeof(tmp));
			break;
		case '%':
			tmp[0] = '%';
			tmp[1] = 0;
			break;
		default:
			tmp[0] = 0;
			break;
		}
		fmt++;
		o += (u32)rt_strlcpy(buf + o, tmp, size - o);
		if (o >= size)
			o = size - 1;
	}
	buf[o] = 0;
}

void amiga_log_line(const char *line);

void bsd_vsyslog(RTREG(a6) struct Library *base, RTREG(d0) ULONG pri, RTREG(a0) CONST_STRPTR msg,
                 RTREG(a1) APTR args)
{
	CTX;
	char line[200];
	int n;

	if (!msg)
		return;
	if (!((1UL << (pri & LOG_PRIMASK)) & (ULONG)c->logmask))
		return;
	n = rt_snprintf(line, sizeof(line), "%s: ", c->logtag ? (const char *)c->logtag : "syslog");
	vformat(line + n, sizeof(line) - (u32)n, (const char *)msg, (const ULONG *)args, c->errno_);
	if (!amiga_rs_log(c, pri, line))
		amiga_log_line(line);
}

/* ---- misc ---- */

LONG bsd_Dup2Socket(RTREG(a6) struct Library *base, RTREG(d0) LONG fd1, RTREG(d1) LONG fd2)
{
	CTX;
	return ret(c, rt_api_dup2(&c->sc, fd1, fd2));
}

LONG bsd_sendmsg(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR msg,
                 RTREG(d1) LONG flags)
{
	CTX;
	struct msghdr *m = msg;

	if (!m)
		return ret(c, -RT_EFAULT);
	if (amiga_rs_caller_hooks & (1 << 4)) {
		LONG e = amiga_rs_mon_send(c, s, NULL, 0, flags, NULL, 0, m);
		if (e)
			return ret(c, -e);
	}
	return ret(c, rt_api_sendmsg(&c->sc, s, (const struct rt_iov *)m->msg_iov, (int)m->msg_iovlen,
	                             flags, (const struct rt_sockaddr_in *)m->msg_name, m->msg_namelen));
}

LONG bsd_recvmsg(RTREG(a6) struct Library *base, RTREG(d0) LONG s, RTREG(a0) APTR msg,
                 RTREG(d1) LONG flags)
{
	CTX;
	struct msghdr *m = msg;
	u32 namelen;
	int mf = 0, r;

	if (!m)
		return ret(c, -RT_EFAULT);
	namelen = m->msg_name ? m->msg_namelen : 0;
	r = rt_api_recvmsg(&c->sc, s, (const struct rt_iov *)m->msg_iov, (int)m->msg_iovlen, flags,
	                   (struct rt_sockaddr_in *)m->msg_name, m->msg_name ? &namelen : NULL, &mf);
	if (r >= 0) {
		if (m->msg_name)
			m->msg_namelen = namelen;
		m->msg_controllen = 0;     /* no ancillary data */
		m->msg_flags = mf;
	}
	return ret(c, r);
}

LONG bsd_gethostname(RTREG(a6) struct Library *base, RTREG(a0) STRPTR name, RTREG(d0) LONG namelen)
{
	const char *h = rt_hostname[0] ? rt_hostname : "localhost";
	LONG l = (LONG)strlen(h);

	if (!name || namelen <= 0)
		return ret(base_ctx(base), -RT_EINVAL);
	if (l >= namelen) {
		memcpy(name, h, (size_t)namelen);   /* not terminated, as documented */
	} else {
		memcpy(name, h, (size_t)l + 1);
	}
	return 0;
}

ULONG bsd_gethostid(RTREG(a6) struct Library *base)
{
	struct rt_netif *n;
	u32 id = 0;

	rt_lock();
	for (n = rt_netif_list(); n; n = n->next) {
		if (n->type != RT_IFT_LOOP && n->addr) {
			id = n->addr;
			break;
		}
	}
	rt_unlock();
	return id ? id : rt_htonl(0x7f000001u);
}

/* ---- error strings ---- */

const char *amiga_herror_str(LONG h)
{
	static const char *const s[] = {
		"Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure",
		"Unknown server error", "No address associated with name"
	};
	if (h >= 0 && h <= 4)
		return s[h];
	return "Unknown resolver error";
}

const char *amiga_ioerr_str(LONG e)
{
	static const char *const s[] = {
		"no error", "device/unit failed to open", "request aborted",
		"command not supported", "invalid length", "invalid address",
		"unit is busy", "hardware failed self-test"
	};
	if (e < 0)
		e = -e;
	if (e <= 7)
		return s[e];
	return "unknown I/O error";
}

const char *amiga_s2err_str(LONG e)
{
	static const char *const s[] = {
		"no error", "resource allocation failure", "unknown error", "bad argument",
		"inappropriate state", "bad address", "requested packet too large",
		"unknown error", "command is not supported", "driver software error detected",
		"device driver is offline", "transmission attempt failed"
	};
	if (e >= 0 && e <= 11)
		return s[e];
	return "unknown error";
}

const char *amiga_s2werr_str(LONG e)
{
	static const char *const s[] = {
		"generic error", "unit not configured", "unit is currently online",
		"unit is currently offline", "protocol already tracked", "protocol not tracked",
		"buffer management function error", "source address error", "destination address error",
		"broadcast address error", "multicast address error", "multicast address list full",
		"unsupported event class", "statdata failed sanity check", "unknown error",
		"attempt to configure twice", "null pointer detected", "transmission failed - too many retries",
		"driver fixable hardware error"
	};
	if (e >= 0 && e <= 18)
		return s[e];
	return "unknown error";
}

/* ---- SocketBaseTagList ---- */

static LONG fd_callback_glue(struct rt_sockctx *sc, int fd, int action)
{
	struct TaskCtx *c = sc->owner;

	if (!c || !c->fdcb)
		return 0;
	return amiga_call_d0d1(c->fdcb, fd, action);
}

static int fdcb_wrapper(struct rt_sockctx *sc, int fd, int action)
{
	return (int)fd_callback_glue(sc, fd, action);
}

void amiga_set_fdcb(struct TaskCtx *c, APTR fn, LONG cpu)
{
	rt_lock();
	c->fdcb = fn;
	c->fdcb_cpu = fn ? cpu : 0;
	c->sc.fd_callback = fn ? fdcb_wrapper : NULL;
	rt_unlock();
}

/* One tag. Returns 0 or -1 (unknown code / bad argument). */
static int sbt_one(struct TaskCtx *c, struct TagItem *ti)
{
	ULONG tag = ti->ti_Tag;
	ULONG code = (tag >> SBTB_CODE) & SBTS_CODE;
	int ref = (tag & SBTF_REF) != 0;
	int set = (tag & SBTF_SET) != 0;
	ULONG val = 0;
	ULONG *where = ref ? (ULONG *)ti->ti_Data : &ti->ti_Data;

	if (ref && !where)
		return -1;
	if (set)
		val = *where;
	switch (code) {
	case SBTC_BREAKMASK:
		if (set) c->sc.sigintr_mask = val; else val = c->sc.sigintr_mask;
		break;
	case SBTC_SIGIOMASK:
		if (set) c->sc.sigio_mask = val; else val = c->sc.sigio_mask;
		break;
	case SBTC_SIGURGMASK:
		if (set) c->sc.sigurg_mask = val; else val = c->sc.sigurg_mask;
		break;
	case SBTC_SIGEVENTMASK:
		if (set) c->sc.sigevent_mask = val; else val = c->sc.sigevent_mask;
		break;
	case SBTC_ERRNO:
		if (set) bsd_set_errno(c, (LONG)val); else val = (ULONG)c->errno_;
		break;
	case SBTC_HERRNO:
		if (set) set_herrno(c, (LONG)val); else val = (ULONG)c->herrno;
		break;
	case SBTC_DTABLESIZE:
		if (set) {
			if (rt_ctx_set_dtablesize(&c->sc, (int)val) < 0)
				return -1;
		} else {
			val = (ULONG)c->sc.nfds;
		}
		break;
	case SBTC_FDCALLBACK:
		if (set) amiga_set_fdcb(c, (APTR)val, RIPTIDECPU_M68KREG); else val = (ULONG)c->fdcb;
		break;
	case SBTC_LOGSTAT:
		if (set) c->logstat = (LONG)val; else val = (ULONG)c->logstat;
		break;
	case SBTC_LOGTAGPTR:
		if (set) c->logtag = (STRPTR)val; else val = (ULONG)c->logtag;
		break;
	case SBTC_LOGFACILITY:
		if (set) c->logfacility = (LONG)val; else val = (ULONG)c->logfacility;
		break;
	case SBTC_LOGMASK:
		if (set) c->logmask = (LONG)val; else val = (ULONG)c->logmask;
		break;
	case SBTC_ERRNOSTRPTR:
	case SBTC_HERRNOSTRPTR:
	case SBTC_IOERRNOSTRPTR:
	case SBTC_S2ERRNOSTRPTR:
	case SBTC_S2WERRNOSTRPTR:
		if (set)
			return -1;
		val = *where;
		switch (code) {
		case SBTC_ERRNOSTRPTR: val = (ULONG)rt_strerror((int)val); break;
		case SBTC_HERRNOSTRPTR: val = (ULONG)amiga_herror_str((LONG)val); break;
		case SBTC_IOERRNOSTRPTR: val = (ULONG)amiga_ioerr_str((LONG)val); break;
		case SBTC_S2ERRNOSTRPTR: val = (ULONG)amiga_s2err_str((LONG)val); break;
		default: val = (ULONG)amiga_s2werr_str((LONG)val); break;
		}
		break;
	case SBTC_ERRNOBYTEPTR:
	case SBTC_ERRNOWORDPTR:
	case SBTC_ERRNOLONGPTR:
		if (set) {
			c->errnoptr = (APTR)val;
			c->errnosize = code == SBTC_ERRNOBYTEPTR ? 1 : code == SBTC_ERRNOWORDPTR ? 2 : 4;
		} else {
			val = (ULONG)c->errnoptr;
		}
		break;
	case SBTC_HERRNOLONGPTR:
		if (set) c->herrnoptr = (LONG *)val; else val = (ULONG)c->herrnoptr;
		break;
	case SBTC_RELEASESTRPTR:
		if (set)
			return -1;
		val = (ULONG)RELEASE_STRING;
		break;
	default:
		/* the Roadshow codes (roadshow.c) */
		return amiga_rs_sbt(c, code, set, where) == 0 ? 0 : -1;
	}
	if (!set)
		*where = val;
	return 0;
}

LONG bsd_SocketBaseTagList(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	CTX;
	struct TagItem *t = tags;
	LONG idx = 0;

	while (t) {
		switch (t->ti_Tag) {
		case TAG_DONE:
			return 0;
		case TAG_IGNORE:
			t++;
			idx++;
			continue;
		case TAG_MORE:
			t = (struct TagItem *)t->ti_Data;
			continue;
		case TAG_SKIP:
			idx += 1 + (LONG)t->ti_Data;
			t += 1 + t->ti_Data;
			continue;
		}
		if (!(t->ti_Tag & TAG_USER) || sbt_one(c, t) < 0)
			return idx + 1;
		t++;
		idx++;
	}
	return 0;
}

LONG bsd_GetSocketEvents(RTREG(a6) struct Library *base, RTREG(a0) ULONG *eventsp)
{
	CTX;
	u32 ev = 0;
	int r = rt_api_get_events(&c->sc, &ev);

	if (r >= 0 && eventsp)
		*eventsp = ev;
	return r < 0 ? -1 : r;
}

/* ---- asynchronous notification: Signal() the owner task ---- */

void amiga_async_hook(struct rt_socket *so, u32 events)
{
	struct TaskCtx *c = so->owner;
	ULONG mask = 0;

	if (!c)
		return;
	Forbid();
	if (!amiga_ctx_valid(c) || !c->task) {
		Permit();
		return;
	}
	if (so->state & RT_SS_ASYNC)
		mask |= c->sc.sigio_mask;
	/* SIGURG: BSD sends it to the socket's owner (FIOSETOWN) whether or
	 * not the socket is asynchronous */
	if ((events & RT_SEV_OOB) && ((so->state & RT_SS_ASYNC) || so->pgid))
		mask |= c->sc.sigurg_mask;
	if (events & so->event_mask)
		mask |= c->sc.sigevent_mask;
	if (mask)
		Signal(c->task, mask);
	Permit();
}

/* ---- Open/Close vectors ---- */

ULONG bsd_libopen(RTREG(a6) struct Library *base, RTREG(d0) ULONG version)
{
	return (ULONG)rtlib_open(&rtlib_bsdsocket, base);
}

ULONG bsd_libclose(RTREG(a6) struct Library *base)
{
	return rtlib_close((struct RtBase *)base);
}
