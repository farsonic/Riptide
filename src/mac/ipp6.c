/* src/mac/ipp6.c - Riptide on the classic Mac OS: .IPP6, the public IPv6 interface for Mac programs
 * in MacTCP's style (the specification is docs/IPP6.md; programs include sdk/mac/IPP6.h, which this
 * file uses too, so the driver and the programs share one definition of the blocks).
 *
 * .IPP6 is a second driver of the same stack (drvr.S: its own header, _drvr6_start, and entry
 * points), installed in a free unit table entry when .IPP opens (ipp6_install). Its calls take the
 * way .IPP's take: the Control entry takes the block off the Device Manager's queue, this file
 * keeps it on its own list, the stack's deferred task handles it (ipp6_poll, called from ipp.c's
 * ipp_poll in the same round) and completes it with ioResult and ioCompletion (mac_complete).
 * Three calls that never touch the stack's state (ip6GetInfo, ip6AddrToStr, ip6StrToAddr) are
 * answered in the Control call when they are made synchronously; the immediate ones (TCPStatus, TCPGlobalInfo, TCPRcvBfrReturn,
 * UDPBfrReturn, UDPMaxMTUSize, ip6GetAddrs) are answered at once when they come synchronously from
 * a completion or notification routine, which the stack's task is running (as ipp.c does for .IPP).
 *
 * The streams are mactcp.c's design with IPv6 addresses: each TCP or UDP stream is one socket of
 * the core, of family IPv6 with IPV6_V6ONLY off, so an IPv4-mapped address (::ffff:a.b.c.d) goes
 * over IPv4 and every other over IPv6, through the same stream. What cannot finish at once (an
 * open, a receive with nothing to receive, a send with the send buffer full) stays on its stream
 * and is looked at again each time the stack runs. The notification routines are called as
 * MacTCP calls them (mac_call_asr5: pascal arguments, A5 the creator's, D0 the event, D1 the
 * reason, A2 the user data).
 *
 * Names: a small DNS client of its own, asynchronous like everything here (the core's resolver
 * waits in select, which the deferred task must never do): an AAAA and an A query at once on one
 * dual-stack socket, to the configured servers of either family, each sent again to the next
 * server every 2.5 seconds; the results ordered by RFC 6724 (rt_inet_sort_dest) unless the call or
 * riptide.conf (`ipp6 order`) asks for IPv6 or IPv4 first. The hosts table and the core's cache are
 * asked first.
 *
 * .IPP is not touched: its streams are mactcp.c's, its stand-in addresses standin.c's. The two
 * drivers share the stack, the A5 table of ipp.c (the creator's A5 of a create call) and its check
 * for programs that quit (ipp.c reap(), which calls ipp6_reap).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "rt_bsd.h"
#include "rt_ip.h"
#include "rt_ip6.h"
#include "rt_netif.h"
#include "rt_socket.h"
#include "riptide.h"
#include "mac.h"
#include "ipp.h"
#include "../../sdk/mac/IPP6.h"

#define controlErr -17
#define statusErr  -18
#define closErr    -24
/* TCPStatus's connection states are the header's kIP6State...; the validity bits of the open, send
   and close calls are MacTCP's */
#define V_ULPVALUE  0x80
#define V_ULPACTION 0x40
#define ULP_DEFAULT 120                 /* seconds: MacTCP's "0 means ... its default value of 2 minutes" */

/* sizes of the driver's tables, which ip6GetInfo reports */
#define NSTREAMS6 32                    /* TCP and UDP streams together, as mactcp.c's 32 */
#define NLOOK     8                     /* lookups at once, as mactcp.c's 8 */
#define NFDS6     64                    /* descriptors of this file's socket context (select's sets are 2 words) */
#define S6_MAGIC  0x52545336UL          /* 'RTS6': a stream of .IPP6 (mactcp.c's are 'RTSM') */

static struct rt_sockctx ctx6;
static int ctx6_ok;
static void *volatile incoming6;        /* calls handed over by Control, linked by qLink */
static volatile u32 closes6, closes6_logged, opens6;
static int order_default = -1;                  /* riptide.conf `ipp6 order`; -1: net.inet.he.prefer's */
/* the order of a lookup that asks for none: `ipp6 order`, else RiptidePrefs' Happy Eyeballs
   "Prefer" (net.inet.he.prefer), which on the Mac has no other user */
static int order_now(void)
{
	if (order_default >= 0)
		return order_default;
	return rt_he_prefer == 4 ? kIP6OrderIPv4First : rt_he_prefer == 6 ? kIP6OrderIPv6First : kIP6OrderRFC6724;
}
static volatile u32 features_cache;     /* ip6GetInfo's features, refreshed by the stack's task */
static u32 features_ms;

/* ---- what programs ask for, in the log ------------------------------------------------------- */
static const char *cs_name6(int cs)
{
	static const char *const tcpudp[] = {
		"UDPCreate", "UDPRead", "UDPBfrReturn", "UDPWrite", "UDPRelease", "UDPMaxMTUSize", "UDPStatus",
		"UDPMultiCreate", "UDPMultiSend", "UDPMultiRead", "TCPCreate", "TCPPassiveOpen", "TCPActiveOpen",
		"csCode 33", "TCPSend", "TCPNoCopyRcv", "TCPRcvBfrReturn", "TCPRcv", "TCPClose", "TCPAbort",
		"TCPStatus", "TCPExtendedStat", "TCPRelease", "TCPGlobalInfo" };
	static const char *const own[] = { "ip6GetInfo", "ip6GetAddrs", "ip6Resolve", "ip6ResolveAddr",
		"ip6ResolveCancel", "ip6AddrToStr", "ip6StrToAddr" };
	if (cs >= 20 && cs <= 43)
		return tcpudp[cs - 20];
	if (cs >= ip6GetInfo && cs <= ip6StrToAddr)
		return own[cs - ip6GetInfo];
	if (cs == 1)
		return "killCode";
	return "unknown";
}
static u8 cs_seen6[96];
static struct { s16 cs, r; } err_seen6[48];
static void note_call6(int cs)
{
	if (cs < 0 || cs >= (int)sizeof cs_seen6 || cs_seen6[cs])
		return;
	cs_seen6[cs] = 1;
	rt_log(RT_LOG_INFO, ".IPP6: first %s (%d)", cs_name6(cs), cs);
}
/* complete a call: ioResult, then ioCompletion; an error is logged the first time a call ends in it */
static void result6(void *pb, int r)
{
	int cs = PB_CSCODE(pb);
	if (r < 0) {
		unsigned i;
		for (i = 0; i < sizeof err_seen6 / sizeof err_seen6[0]; i++) {
			if (err_seen6[i].cs == cs && err_seen6[i].r == r)
				break;
			if (!err_seen6[i].cs && !err_seen6[i].r) {
				err_seen6[i].cs = (s16)cs;
				err_seen6[i].r = (s16)r;
				rt_log(RT_LOG_WARN, ".IPP6: %s (%d) on %08lx: error %d", cs_name6(cs), cs,
				       (unsigned long)PB_L(pb, 28), r);
				break;
			}
		}
	}
	mac_complete(pb, r);
}

/* ---- addresses -------------------------------------------------------------------------------- */
static int a6_unspec(const IP6Address *a)
{
	for (int i = 0; i < 16; i++)
		if (a->addr[i])
			return 0;
	return 1;
}
static int a6_v4any(const IP6Address *a)
{
	return IP6_IS_V4(a) && !a->addr[12] && !a->addr[13] && !a->addr[14] && !a->addr[15];
}
static int a6_mcast(const IP6Address *a)
{
	return a->addr[0] == 0xFF || (IP6_IS_V4(a) && (a->addr[12] & 0xF0) == 0xE0);
}
/* does the address need an interface to mean anything (link-local, or link-scoped multicast)? */
static int a6_scoped(const IP6Address *a)
{
	return IP6_IS_LINKLOCAL(a) || (a->addr[0] == 0xFF && (a->addr[1] & 0x0F) <= 2);
}
/* an IP6Address into the core's sockaddr (its scope checked, a link-local one without a scope on
   the interface IPv4's default route uses, as the specification says) */
static int to_sin6(const IP6Address *a, struct rt_sockaddr_in6 *s)
{
	memset(s, 0, sizeof *s);
	s->sin6_len = sizeof *s;
	s->sin6_family = RT_AF_INET6;
	s->sin6_port = a->port;                         /* the 68k's order is the network's */
	memcpy(&s->sin6_addr, a->addr, 16);
	if (a6_scoped(a)) {
		if (a->scope) {
			if (!rt_netif_by_index(a->scope))
				return ip6ScopeErr;
			s->sin6_scope_id = a->scope;
		} else if (mac_nif) {
			s->sin6_scope_id = mac_nif->index;
		}
	}
	return 0;
}
static void from_sin6(const struct rt_sockaddr_in6 *s, IP6Address *a)
{
	memcpy(a->addr, &s->sin6_addr, 16);
	a->port = s->sin6_port;
	a->scope = a6_scoped(a) ? (u16)s->sin6_scope_id : 0;
}
static void set_v4(IP6Address *a, u32 v4, u16 port)
{
	rt_in6_addr m;
	rt_in6_set_v4mapped(&m, v4);
	memcpy(a->addr, &m, 16);
	a->port = port;
	a->scope = 0;
}
/* the core's errors of connect and sendto, as the specification's results */
static int dest_err(int e, int other)
{
	if (e == RT_ENETUNREACH || e == RT_EHOSTUNREACH || e == RT_EHOSTDOWN)
		return ipDestDeadErr;
	if (e == RT_EADDRNOTAVAIL)
		return ip6NoSourceErr;
	return other;
}

/* ---- the text form ------------------------------------------------------------------------------ */
/* "fe80::1%en0", "fe80::1%2", "[fd68::1]", "192.168.68.1" into a; 0, nameSyntaxErr or ip6ScopeErr */
static int parse_literal(const char *s, IP6Address *a)
{
	char t[80], zone[16];
	u32 n = (u32)strlen(s), v4;
	int plen;
	rt_in6_addr x;
	if (n && s[0] == '[' && s[n - 1] == ']') {
		s++;
		n -= 2;
	}
	if (!n || n >= sizeof t)
		return nameSyntaxErr;
	memcpy(t, s, n);
	t[n] = 0;
	memset(a, 0, sizeof *a);
	if (rt_parse_ipv4(t, &v4) == 0) {
		set_v4(a, v4, 0);
		return 0;
	}
	if (!strchr(t, ':') || rt_inet6_parse_prefix(t, &x, &plen, zone, sizeof zone) < 0 || plen >= 0)
		return nameSyntaxErr;
	memcpy(a->addr, &x, 16);
	if (zone[0]) {
		struct rt_netif *nif = NULL;
		if (zone[0] >= '0' && zone[0] <= '9')
			nif = rt_netif_by_index((u32)rt_strtoul(zone, NULL, 10));
		else
			nif = rt_netif_find(zone);
		if (!nif)
			return ip6ScopeErr;
		a->scope = (u16)nif->index;
	}
	return 0;
}
/* a into the caller's buffer; 0 or ip6TruncatedErr (what fitted is there) */
static int format_addr(const IP6Address *a, char *out, u32 len)
{
	char t[80];
	rt_in6_addr x;
	u32 n;
	memcpy(&x, a->addr, 16);
	if (IP6_IS_V4(a))                               /* an IPv4 host as IPv4 is written: 192.168.68.1 */
		rt_snprintf(t, sizeof t, "%u.%u.%u.%u", a->addr[12], a->addr[13], a->addr[14], a->addr[15]);
	else
		rt_inet6_ntop(&x, t, sizeof t);
	if (a->scope && a6_scoped(a)) {
		struct rt_netif *nif = rt_netif_by_index(a->scope);
		n = (u32)strlen(t);
		if (nif)
			rt_snprintf(t + n, sizeof t - n, "%%%s", nif->name);
		else
			rt_snprintf(t + n, sizeof t - n, "%%%u", (unsigned)a->scope);
	}
	n = (u32)strlen(t);
	rt_strlcpy(out, t, len);
	return n + 1 > len ? ip6TruncatedErr : 0;
}

/* ---- ip6GetInfo and ip6GetAddrs ------------------------------------------------------------- */
static u32 features_now(void)
{
	u32 f = 0;
	struct rt_in6_ifaddr_info info[16];
	rt_lock();
	for (struct rt_netif *n = rt_netif_list(); n; n = n->next) {
		if (n->type == RT_IFT_LOOP)
			continue;
		if (n->addr && !n->ip4_disabled)
			f |= kIP6HasIPv4;
		int k = rt_in6_addr_list(n, info, 16);
		for (int i = 0; i < k && i < 16; i++) {
			if (info[i].flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED))
				continue;
			f |= rt_in6_is_linklocal(&info[i].addr) ? kIP6HasLinkLocal : kIP6HasIPv6;
		}
	}
	if (rt_he_v6_usable())
		f |= kIP6HasIPv6Route;
	{
		rt_in6_addr srv[1];
		u32 idx[1];
		u8 src[1];
		if (rt_dns_get_servers6(srv, idx, src, 1) > 0)
			f |= kIP6HasNameServer;
	}
	rt_unlock();
	return f;
}
static int get_info(IP6iopb *pb)
{
	static const char impl[] = "Riptide .IPP6 1 (" __DATE__ ")";
	IP6InfoPB *p = &pb->csParam.info;
	p->version = IPP6_VERSION;
	p->features = features_cache;
	p->maxTCPStreams = NSTREAMS6;
	p->maxUDPStreams = NSTREAMS6;
	p->maxLookups = NLOOK;
	p->order = (u16)order_now();
	p->implementation = impl;
	return 0;
}
static u8 mask_bits(u32 m)
{
	u8 n = 0;
	m = rt_ntohl(m);
	while (m & 0x80000000u) {
		n++;
		m <<= 1;
	}
	return n;
}
static int get_addrs(IP6iopb *pb)
{
	IP6AddrsPB *p = &pb->csParam.addrs;
	u32 max = p->bufferLen / sizeof(IP6AddrInfo), total = 0, count = 0;
	struct rt_in6_ifaddr_info info[16];
	if (!p->buffer && p->bufferLen)
		return invalidBufPtr;
	rt_lock();
	for (struct rt_netif *n = rt_netif_list(); n; n = n->next) {
		u32 a4[1 + RT_IF_MAXADDR4], m4[1 + RT_IF_MAXADDR4], k4 = 0;
		if (n->type == RT_IFT_LOOP)
			continue;
		if (n->addr && !n->ip4_disabled) {
			a4[k4] = n->addr;
			m4[k4++] = n->netmask;
			for (u32 i = 0; i < n->nalias && i < RT_IF_MAXADDR4; i++) {
				a4[k4] = n->alias[i].addr;
				m4[k4++] = n->alias[i].netmask;
			}
		}
		for (u32 i = 0; i < k4; i++, total++) {
			IP6AddrInfo *e;
			if (count >= max)
				continue;
			e = &p->buffer[count++];
			memset(e, 0, sizeof *e);
			set_v4(&e->addr, a4[i], 0);
			e->prefixLen = mask_bits(m4[i]);
			e->flags = kIP6AddrIPv4 | (n == mac_nif ? kIP6AddrPrimary : 0);
			e->ifIndex = (u16)n->index;
			rt_strlcpy(e->ifName, n->name, sizeof e->ifName);
		}
		int k6 = rt_in6_addr_list(n, info, 16);
		for (int i = 0; i < k6 && i < 16; i++, total++) {
			IP6AddrInfo *e;
			u16 f = info[i].flags;
			if (count >= max)
				continue;
			e = &p->buffer[count++];
			memset(e, 0, sizeof *e);
			memcpy(e->addr.addr, &info[i].addr, 16);
			e->prefixLen = info[i].plen;
			e->ifIndex = (u16)n->index;
			e->flags = (n == mac_nif ? kIP6AddrPrimary : 0) |
			           (f & RT_IN6_IFF_TEMPORARY ? kIP6AddrTemporary : 0) |
			           (f & RT_IN6_IFF_DEPRECATED ? kIP6AddrDeprecated : 0) |
			           (f & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED) ? kIP6AddrTentative : 0) |
			           (f & RT_IN6_IFF_AUTOCONF ? kIP6AddrAutoconf : 0);
			if (rt_in6_is_linklocal(&info[i].addr)) {
				e->flags |= kIP6AddrLinkLocal;
				e->addr.scope = (u16)n->index;
			}
			rt_strlcpy(e->ifName, n->name, sizeof e->ifName);
		}
	}
	rt_unlock();
	p->count = (u16)count;
	p->total = (u16)total;
	return 0;
}

/* ==================== streams ==================== */
struct s6 {
	u32 magic;
	int tcp, fd;
	u8 *rbuf;                       /* the caller's receive buffer */
	u32 rlen;
	void *notify, *udata, *a5;      /* the ASR, its user data, the creating program's A5 */
	char prog[32];                  /* the creating program's name (Background programs, QoS rules) */
	void *open_pb;                  /* a TCP open waiting */
	u32 open_deadline;
	int passive;
	IP6Address filt;                /* TCPPassiveOpen: the remote end to accept (:: and 0: any) */
	void *rcv_q, *snd_q;            /* receives and sends waiting, linked by qLink */
	u32 rcv_deadline;
	u32 snd_off;                    /* bytes of the first send's WDS already sent */
	u32 snd_moved;                  /* rt_now_ms when a send last made progress */
	u8 ulp_secs, ulp_report;        /* the connection's ULP timeout and action */
	int conn;                       /* a connection is up (opened, not yet terminated) */
	int peer_fin, told_fin, told_data, closed, nodelay;
	int graceful;                   /* the last connection ended with both ends closing */
	int loaned;                     /* rbuf is out (TCPNoCopyRcv, UDPRead) */
	u32 own_lo, own_hi;             /* the creator's partition (ipp6_reap); 1: not a program's */
	IP6Address remote, local;       /* the connection's ends (TCP), as TCPStatus reports them */
	u16 lport;                      /* UDP: the stream's port */
};
static struct s6 streams6[NSTREAMS6];

static struct s6 *stream_of(void *pb, int tcp)
{
	struct s6 *s = (struct s6 *)PB_L(pb, 28);
	if (s < streams6 || s >= streams6 + NSTREAMS6 || s->magic != S6_MAGIC || s->tcp != tcp)
		return NULL;
	return s;
}
static void asr6(struct s6 *s, u32 event, u32 reason)
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
static void fail_all(struct s6 *s, int rr, int r)
{
	void *pb;
	while ((pb = q_pop(&s->rcv_q)) != NULL)
		result6(pb, rr);
	while ((pb = q_pop(&s->snd_q)) != NULL)
		result6(pb, r);
	if (s->open_pb) {
		pb = s->open_pb;
		s->open_pb = NULL;
		result6(pb, r);
	}
}
static void nonblock(int fd)
{
	int one = 1;
	rt_api_ioctl(&ctx6, fd, RT_FIONBIO, &one);
}
/* a dual-stack socket: IPv4-mapped addresses over IPv4, the rest over IPv6 */
static int socket6(int type, int proto)
{
	int fd = rt_api_socket(&ctx6, RT_AF_INET6, type, proto), zero = 0;
	if (fd < 0)
		return fd;
	nonblock(fd);
	rt_api_setsockopt(&ctx6, fd, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &zero, sizeof zero);
	return fd;
}
static int ready(int fd, int wr)
{
	u32 set[2] = { 0, 0 };
	if (fd < 0 || fd >= NFDS6)
		return 0;
	set[fd >> 5] = 1u << (fd & 31);
	return rt_api_select(&ctx6, fd + 1, wr ? NULL : set, wr ? set : NULL, NULL, 0, NULL) > 0;
}
static u32 pending(int fd)
{
	int n = 0;
	if (fd < 0 || rt_api_ioctl(&ctx6, fd, RT_FIONREAD, &n) < 0 || n < 0)
		return 0;
	return (u32)n;
}
static u32 secs_deadline(u32 secs) { return secs ? rt_now_ms() + secs * 1000u : 0; }
static int expired(u32 deadline) { return deadline && (s32)(rt_now_ms() - deadline) >= 0; }
/* a WDS: { length (word), pointer (long) } ... ending with a zero length */
static u32 wds_total(const u8 *w)
{
	u32 t = 0;
	for (; rt_get_be16(w); w += 6)
		t += rt_get_be16(w);
	return t;
}
static void stream_close(struct s6 *s)
{
	if (s->fd >= 0)
		rt_api_close(&ctx6, s->fd);
	s->fd = -1;
}
/* the ULP timeout and action of an open, send or close: bytes 32, 33 and the validity bits at 34
   (the same place in all three blocks) */
static void take_ulp(struct s6 *s, void *pb)
{
	u8 v = ((u8 *)pb)[34];
	if (v & V_ULPVALUE)
		s->ulp_secs = ((u8 *)pb)[32];
	if (v & V_ULPACTION)
		s->ulp_report = ((u8 *)pb)[33] == 0;
}
static u32 ulp_secs(struct s6 *s)
{
	u32 t = s->ulp_secs ? s->ulp_secs : ULP_DEFAULT;
	return t < 2 ? 2 : t;
}
static struct s6 *new_stream(void)
{
	for (int i = 0; i < NSTREAMS6; i++)
		if (streams6[i].magic != S6_MAGIC) {
			memset(&streams6[i], 0, sizeof streams6[i]);
			streams6[i].fd = -1;
			return &streams6[i];
		}
	return NULL;
}
static int buffer_taken(const void *buf)
{
	for (int i = 0; i < NSTREAMS6; i++)
		if (streams6[i].magic == S6_MAGIC && streams6[i].rbuf == buf)
			return 1;
	return 0;
}

/* ==================== TCP ==================== */
static int tcp_create(TCP6iopb *pb)
{
	TCP6CreatePB *p = &pb->csParam.create;
	struct s6 *s;
	if (!p->rcvBuff)
		return invalidBufPtr;
	if (p->rcvBuffLen < 4096)
		return invalidLength;
	if (buffer_taken(p->rcvBuff))
		return streamAlreadyOpen;
	if (!(s = new_stream()))
		return insufficientResources;
	s->magic = S6_MAGIC;
	s->tcp = 1;
	s->rbuf = (u8 *)p->rcvBuff;
	s->rlen = p->rcvBuffLen;
	s->notify = (void *)p->notifyProc;
	s->udata = p->userDataPtr;
	rt_strlcpy(s->prog, ipp_creator_prog(pb), sizeof s->prog);
	s->a5 = ipp_creator_a5(pb);
	pb->tcpStream = (u32)s;
	return 0;
}

static void tcp_reset_conn(struct s6 *s, void *pb)
{
	s->conn = s->peer_fin = s->told_fin = s->told_data = s->closed = s->loaned = s->nodelay = s->graceful = 0;
	s->snd_off = 0;
	s->ulp_secs = 0;
	s->ulp_report = 0;                              /* the default action is abort */
	take_ulp(s, pb);
}

static int tcp_open(TCP6iopb *pb, int passive)
{
	TCP6OpenPB *p = &pb->csParam.open;
	struct s6 *s = stream_of(pb, 1);
	struct rt_sockaddr_in6 ra, la;
	int fd, r, one = 1;
	if (!s)
		return invalidStreamPtr;
	if (s->fd >= 0 || s->open_pb)
		return connectionExists;
	if (!passive) {
		if (a6_unspec(&p->remoteAddr) || a6_mcast(&p->remoteAddr) || a6_v4any(&p->remoteAddr))
			return ip6BadAddrErr;           /* "remote IP address; cannot be 0" */
		if (!p->remoteAddr.port)
			return invalidLength;
	}
	/* a local address of one family and a remote one of the other cannot be one connection */
	if (!a6_unspec(&p->localAddr) && !a6_unspec(&p->remoteAddr) &&
	    !IP6_IS_V4(&p->remoteAddr) != !IP6_IS_V4(&p->localAddr))
		return ip6BadAddrErr;
	if ((r = to_sin6(&p->remoteAddr, &ra)) != 0 || (r = to_sin6(&p->localAddr, &la)) != 0)
		return r;
	mac_prog_name = s->prog;
	fd = socket6(RT_SOCK_STREAM, RT_IPPROTO_TCP);
	mac_prog_name = NULL;
	if (fd < 0)
		return insufficientResources;
	s->fd = fd;
	tcp_reset_conn(s, pb);
	if (passive || p->localAddr.port || !a6_unspec(&p->localAddr)) {
		rt_api_setsockopt(&ctx6, fd, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof one);
		if (passive && !a6_unspec(&p->localAddr) && !IP6_IS_V4(&p->localAddr))
			rt_api_setsockopt(&ctx6, fd, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &one, sizeof one);
		r = rt_api_bind(&ctx6, fd, &la, sizeof la);
		if (r < 0) {
			stream_close(s);
			return r == -RT_EADDRNOTAVAIL ? ip6BadAddrErr : duplicateSocket;
		}
	}
	s->passive = passive;
	memset(&s->remote, 0, sizeof s->remote);
	memset(&s->local, 0, sizeof s->local);
	if (passive) {
		u8 t = p->commandTimeoutValue;
		/* the command timeout (0 = for ever) and the remote end to accept (:: and port 0: any) */
		s->open_deadline = secs_deadline(t && t < 2 ? 2 : t);
		s->filt = p->remoteAddr;
		if (rt_api_listen(&ctx6, fd, 1) < 0) {
			stream_close(s);
			return openFailed;
		}
	} else {
		/* "no command timeout is provided; if the connection cannot be established within the ULP
		   timeout period, the command is completed in error" */
		s->open_deadline = secs_deadline(ulp_secs(s));
		s->remote = p->remoteAddr;
		r = rt_api_connect(&ctx6, fd, &ra, sizeof ra);
		if (r < 0 && r != -RT_EINPROGRESS && r != -RT_EALREADY) {
			stream_close(s);
			return dest_err(-r, openFailed);
		}
	}
	s->open_pb = pb;
	return 1;
}

/* the two ends into the open's block, and into the stream for TCPStatus */
static void tcp_fill_ends(struct s6 *s, TCP6iopb *pb)
{
	struct rt_sockaddr_in6 a;
	u32 l = sizeof a;
	if (rt_api_getpeername(&ctx6, s->fd, &a, &l) == 0)
		from_sin6(&a, &s->remote);
	l = sizeof a;
	if (rt_api_getsockname(&ctx6, s->fd, &a, &l) == 0)
		from_sin6(&a, &s->local);
	pb->csParam.open.remoteAddr = s->remote;
	pb->csParam.open.localAddr = s->local;
}

static void tcp_up(struct s6 *s, TCP6iopb *pb)
{
	s->conn = 1;
	s->snd_moved = rt_now_ms();
	tcp_fill_ends(s, pb);
	s->open_pb = NULL;
	result6(pb, 0);
}

/* does the passive open's filter let this remote end in? */
static int filter_ok(const IP6Address *f, const struct rt_sockaddr_in6 *a)
{
	if (f->port && f->port != a->sin6_port)
		return 0;
	return a6_unspec(f) || !memcmp(f->addr, &a->sin6_addr, 16);
}

static void tcp_poll_open(struct s6 *s)
{
	TCP6iopb *pb = s->open_pb;
	if (s->passive) {
		struct rt_sockaddr_in6 a;
		u32 l = sizeof a;
		int nfd;
		while ((nfd = rt_api_accept(&ctx6, s->fd, &a, &l)) >= 0) {
			if (!filter_ok(&s->filt, &a)) {
				rt_api_close(&ctx6, nfd);       /* not the remote end the open asked for */
				l = sizeof a;
				continue;
			}
			rt_api_close(&ctx6, s->fd);
			s->fd = nfd;
			nonblock(nfd);
			tcp_up(s, pb);
			return;
		}
	} else if (ready(s->fd, 1)) {
		int err = 0;
		u32 l = sizeof err;
		rt_api_getsockopt(&ctx6, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l);
		if (err) {
			s->open_pb = NULL;
			stream_close(s);
			result6(pb, dest_err(err, openFailed));
			return;
		}
		tcp_up(s, pb);
		return;
	}
	if (expired(s->open_deadline)) {
		s->open_pb = NULL;
		stream_close(s);
		result6(pb, s->passive ? commandTimeout : openFailed);
	}
}

/* the connection is over: what waits fails, the ASR hears TCPTerminate once, and the stream is
   back where TCPCreate left it (a loaned buffer stays out until it is given back) */
static void tcp_terminate(struct s6 *s, int reason)
{
	if (!s->conn && !s->open_pb)
		return;
	s->conn = 0;
	s->graceful = reason == TCPULPClose;
	fail_all(s, reason == TCPULPClose ? connectionClosing : connectionTerminated, connectionTerminated);
	stream_close(s);
	asr6(s, TCPTerminate, (u32)reason);
}

static int sock_reason(int err)
{
	if (err == RT_ECONNRESET || err == RT_ECONNREFUSED || err == RT_EPIPE)
		return TCPRemoteAbort;
	if (err == RT_ETIMEDOUT)
		return TCPULPTimeoutTerminate;
	return TCPNetworkFailure;
}

static void next_rcv_deadline(struct s6 *s)
{
	s->rcv_deadline = s->rcv_q ? secs_deadline(((TCP6iopb *)s->rcv_q)->csParam.receive.commandTimeoutValue) : 0;
}

/* a receive that got nothing: the other end's FIN (0), or an error the socket hands over only once
   (a reset: recv returns it and SO_ERROR is clear afterwards), which ends the connection here */
static void recv_failed(struct s6 *s, int r)
{
	if (r == 0)
		s->peer_fin = 1;
	else if (r != -RT_EAGAIN && r != -RT_EWOULDBLOCK && r != -RT_EINTR)
		tcp_terminate(s, sock_reason(-r));
}

static void tcp_poll_rcv(struct s6 *s)
{
	TCP6iopb *pb;
	while ((pb = s->rcv_q) != NULL) {
		TCP6ReceivePB *p = &pb->csParam.receive;
		int nocopy = pb->csCode == TCPNoCopyRcv;
		u32 avail = pending(s->fd);
		if (!avail || (nocopy && s->loaned)) {
			if (!avail && (s->peer_fin || (ready(s->fd, 0) && !pending(s->fd)))) {
				/* readable with nothing to read: the other end has closed, all is delivered */
				s->peer_fin = 1;
				q_pop(&s->rcv_q);
				result6(pb, connectionClosing);
				continue;
			}
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				result6(pb, commandTimeout);
				next_rcv_deadline(s);
				continue;
			}
			return;                         /* nothing yet (or the buffer is still out) */
		}
		if (nocopy) {
			u8 *rds = (u8 *)p->rdsPtr;
			u32 n = avail < s->rlen ? avail : s->rlen;
			int r;
			if (n > 65535)
				n = 65535;
			r = rt_api_recv(&ctx6, s->fd, s->rbuf, n, RT_MSG_DONTWAIT);
			if (r <= 0) {
				recv_failed(s, r);
				return;
			}
			/* one piece, and the RDS's terminating zero */
			rt_put_be16(rds, (u16)r);
			memcpy(rds + 2, &s->rbuf, 4);
			rt_put_be16(rds + 6, 0);
			s->loaned = 1;
		} else {
			int r = rt_api_recv(&ctx6, s->fd, p->rcvBuff, p->rcvBuffLen, RT_MSG_DONTWAIT);
			if (r <= 0) {
				recv_failed(s, r);
				return;
			}
			p->rcvBuffLen = (u16)r;
		}
		p->markFlag = 0;
		p->urgentFlag = 0;
		q_pop(&s->rcv_q);
		s->told_data = 0;
		result6(pb, 0);
		next_rcv_deadline(s);
	}
}

static void tcp_poll_snd(struct s6 *s)
{
	TCP6iopb *pb;
	while ((pb = s->snd_q) != NULL) {
		const u8 *w = (const u8 *)pb->csParam.send.wdsPtr;
		u32 skip = s->snd_off;
		/* past what went already, then as much as the socket takes */
		for (; rt_get_be16(w); w += 6) {
			u32 len = rt_get_be16(w);
			const u8 *d;
			int r;
			memcpy(&d, w + 2, 4);
			if (skip >= len) {
				skip -= len;
				continue;
			}
			r = rt_api_send(&ctx6, s->fd, d + skip, len - skip, RT_MSG_DONTWAIT);
			if (r < 0) {
				if (r == -RT_EAGAIN || r == -RT_EWOULDBLOCK)
					return;
				tcp_terminate(s, sock_reason(-r));
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
		result6(pb, 0);
	}
}

static int tcp_state(struct s6 *s)
{
	if (s->open_pb)
		return s->passive ? kIP6StateListen : kIP6StateSynSent;
	if (!s->conn)
		return kIP6StateClosed;
	if (s->closed)
		return s->peer_fin ? kIP6StateClosing : kIP6StateFinWait1;
	return s->told_fin ? kIP6StateCloseWait : kIP6StateEstablished;
}

static void tcp_poll(struct s6 *s)
{
	int err = 0;
	u32 l = sizeof err;
	if (s->fd < 0)
		return;
	if (s->open_pb) {
		tcp_poll_open(s);
		return;
	}
	if (!s->conn)
		return;
	if (rt_api_getsockopt(&ctx6, s->fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &l) == 0 && err) {
		tcp_terminate(s, sock_reason(err));
		return;
	}
	tcp_poll_snd(s);
	if (!s->conn)
		return;
	tcp_poll_rcv(s);
	if (!s->conn)
		return;                                 /* a receive found a reset */
	/* the ULP timeout: sends that have not moved for that long */
	if (s->snd_q && (u32)(rt_now_ms() - s->snd_moved) >= ulp_secs(s) * 1000u) {
		s->snd_moved = rt_now_ms();
		if (!s->ulp_report) {
			tcp_terminate(s, TCPULPTimeoutTerminate);
			return;
		}
		asr6(s, TCPULPTimeout, 0);
	}
	/* the notifications: data that no receive waits for, the other end's close */
	if (!s->rcv_q && !s->told_data && pending(s->fd)) {
		s->told_data = 1;
		asr6(s, TCPDataArrival, 0);
	}
	if (!s->told_fin && !pending(s->fd) && (s->peer_fin || ready(s->fd, 0))) {
		s->peer_fin = 1;
		s->told_fin = 1;
		asr6(s, TCPClosing, 0);
	}
	/* both ends have closed and everything is delivered: the connection is over, gracefully */
	if (s->closed && s->told_fin && !s->snd_q)
		tcp_terminate(s, TCPULPClose);
	/* a TCPClose that waited for the sends before it: the FIN, once they are all out */
	if (s->conn && s->closed == 2 && !s->snd_q) {
		rt_api_shutdown(&ctx6, s->fd, 1);
		s->closed = 1;
	}
}

static int tcp_status(TCP6iopb *pb)
{
	static u8 connstats[96];        /* MacTCP's TCPConnectionStats, zero */
	TCP6StatusPB *p = &pb->csParam.status;
	struct s6 *s = stream_of(pb, 1);
	u32 unread, mss = 1220;
	if (!s)
		return invalidStreamPtr;
	memset((u8 *)pb + 32, 0, 94);                           /* up to userDataPtr at 126 */
	p->ulpTimeoutValue = (u8)ulp_secs(s);
	p->ulpTimeoutAction = s->ulp_report ? 0 : 1;
	p->remoteAddr = s->open_pb && s->passive ? s->filt : s->remote;
	p->localAddr = s->local;
	p->connectionState = (u8)tcp_state(s);
	unread = pending(s->fd);
	if (s->fd >= 0) {
		int m = 0;
		u32 l = sizeof m;
		if (rt_api_getsockopt(&ctx6, s->fd, RT_IPPROTO_TCP, RT_TCP_MAXSEG, &m, &l) == 0 && m > 0)
			mss = (u32)m;
	}
	p->sendWindow = 8192;
	p->rcvWindow = (u16)(s->rlen > 65535 ? 65535 : s->rlen);
	p->amtUnreadData = (u16)(unread > 65535 ? 65535 : unread);
	p->srtt = 100;
	p->lastRTT = 100;
	p->sendMaxSegSize = mss;
	p->connStatPtr = (char *)connstats;
	return s->conn || s->open_pb ? 0 : connectionDoesntExist;
}

static int tcp_global(TCP6iopb *pb)
{
	static u32 params[6], stats[13];
	static IP6UInt32 table[NSTREAMS6];
	static const u8 rto[] = "\x0bVan Jacobson";
	TCP6GlobalInfoPB *p = &pb->csParam.globalInfo;
	params[0] = (u32)rto;
	params[1] = 200;                /* minimum retransmission timeout, ms */
	params[2] = 60000;              /* maximum */
	params[3] = 1460;               /* MSS we can receive */
	params[4] = NSTREAMS6;
	params[5] = 65535;              /* largest receive window */
	for (int i = 0; i < NSTREAMS6; i++)
		table[i] = streams6[i].magic == S6_MAGIC && streams6[i].tcp ? (IP6UInt32)&streams6[i] : 0;
	p->tcpParamPtr = (char *)params;
	p->tcpStatsPtr = (char *)stats;
	p->tcpCDBTable = table;
	p->maxTCPConnections = NSTREAMS6;
	return 0;
}

static void stream_free(struct s6 *s, int r)
{
	fail_all(s, r, r);
	stream_close(s);
	s->magic = 0;
}

static void linger0(struct s6 *s)
{
	struct { s32 onoff, secs; } lg = { 1, 0 };
	if (s->fd >= 0)
		rt_api_setsockopt(&ctx6, s->fd, RT_SOL_SOCKET, RT_SO_LINGER, &lg, sizeof lg);    /* a reset */
}

/* the stream of a TCP call that needs a connection: 0 if there is one, else the result */
static int need_conn(void *pb, struct s6 **sp, int closing)
{
	struct s6 *s = stream_of(pb, 1);
	*sp = s;
	if (!s)
		return invalidStreamPtr;
	if (!s->conn)
		return s->graceful && closing ? connectionClosing : connectionDoesntExist;
	return 0;
}

static int tcp_call(TCP6iopb *pb)
{
	struct s6 *s;
	int r;
	switch (pb->csCode) {
	case TCPCreate:
		return tcp_create(pb);
	case TCPPassiveOpen:
		return tcp_open(pb, 1);
	case TCPActiveOpen:
		return tcp_open(pb, 0);
	case TCPSend: {
		u32 n;
		if ((r = need_conn(pb, &s, 0)) != 0)
			return r;
		if (s->closed)
			return connectionClosing;       /* "a TCPClose command was already issued" */
		if (!pb->csParam.send.wdsPtr)
			return invalidWDS;
		n = wds_total((const u8 *)pb->csParam.send.wdsPtr);
		if (n == 0 || n > 65535)
			return invalidLength;
		take_ulp(s, pb);
		if (pb->csParam.send.pushFlag && !s->nodelay) {     /* push: no waiting for more (Nagle off) */
			int one = 1;
			rt_api_setsockopt(&ctx6, s->fd, RT_IPPROTO_TCP, RT_TCP_NODELAY, &one, sizeof one);
			s->nodelay = 1;
		}
		if (!s->snd_q)
			s->snd_moved = rt_now_ms();
		q_add(&s->snd_q, pb);
		tcp_poll_snd(s);
		return 1;
	}
	case TCPRcv:
	case TCPNoCopyRcv:
		if ((r = need_conn(pb, &s, 1)) != 0)
			return r;
		if (pb->csCode == TCPRcv) {
			if (!pb->csParam.receive.rcvBuff)
				return invalidBufPtr;
			if (!pb->csParam.receive.rcvBuffLen)
				return invalidLength;
		} else {
			if (!pb->csParam.receive.rdsPtr)
				return invalidBufPtr;
			if (!pb->csParam.receive.rdsLength)
				return invalidLength;   /* "the RDS has 0 entries"; one is enough */
		}
		if (!s->rcv_q)
			s->rcv_deadline = secs_deadline(pb->csParam.receive.commandTimeoutValue);
		q_add(&s->rcv_q, pb);
		tcp_poll_rcv(s);
		return 1;
	case TCPRcvBfrReturn:
		if (!(s = stream_of(pb, 1)))
			return invalidStreamPtr;
		if (!pb->csParam.receive.rdsPtr)
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
		tcp_poll_snd(s);                        /* what is queued goes before the FIN */
		if (s->snd_q) {
			s->closed = 2;                  /* the FIN follows the sends (tcp_poll) */
			return 0;
		}
		rt_api_shutdown(&ctx6, s->fd, 1);       /* SHUT_WR: our FIN */
		s->closed = 1;
		return 0;
	case TCPAbort:
		if (!(s = stream_of(pb, 1)))
			return invalidStreamPtr;
		if (!s->conn && !s->open_pb)
			return connectionDoesntExist;
		linger0(s);
		tcp_terminate(s, TCPULPAbort);
		s->graceful = 0;
		return 0;
	case TCPStatus:
		return tcp_status(pb);
	case TCPRelease:
		if (!(s = stream_of(pb, 1)))
			return invalidStreamPtr;
		if (s->conn)
			linger0(s);                     /* an open connection is aborted */
		pb->csParam.create.rcvBuff = (char *)s->rbuf;
		pb->csParam.create.rcvBuffLen = s->rlen;
		stream_free(s, connectionTerminated);
		return 0;
	case TCPGlobalInfo:
		return tcp_global(pb);
	}
	return controlErr;
}

/* ==================== UDP ==================== */
static int udp_create(UDP6iopb *pb)
{
	UDP6CreatePB *p = &pb->csParam.create;
	struct rt_sockaddr_in6 a;
	u32 l = sizeof a;
	struct s6 *s;
	int fd;
	if (!p->rcvBuff)
		return invalidBufPtr;
	if (p->rcvBuffLen < 2048)
		return invalidLength;
	if (buffer_taken(p->rcvBuff))
		return streamAlreadyOpen;
	if (!(s = new_stream()))
		return insufficientResources;
	mac_prog_name = ipp_creator_prog(pb);
	fd = socket6(RT_SOCK_DGRAM, RT_IPPROTO_UDP);
	mac_prog_name = NULL;
	if (fd < 0)
		return insufficientResources;
	memset(&a, 0, sizeof a);
	a.sin6_len = sizeof a;
	a.sin6_family = RT_AF_INET6;
	a.sin6_port = p->localPort;
	if (rt_api_bind(&ctx6, fd, &a, sizeof a) < 0) {
		rt_api_close(&ctx6, fd);
		return duplicateSocket;
	}
	rt_api_getsockname(&ctx6, fd, &a, &l);
	s->magic = S6_MAGIC;
	s->fd = fd;
	s->rbuf = (u8 *)p->rcvBuff;
	s->rlen = p->rcvBuffLen;
	s->notify = (void *)p->notifyProc;
	s->udata = p->userDataPtr;
	rt_strlcpy(s->prog, ipp_creator_prog(pb), sizeof s->prog);
	s->a5 = ipp_creator_a5(pb);
	s->lport = a.sin6_port;
	p->localPort = a.sin6_port;
	pb->udpStream = (u32)s;
	return 0;
}

/* the address this Mac answers a sender from: the one a datagram to it would carry */
static void local_for(const IP6Address *from, IP6Address *out, u16 port)
{
	memset(out, 0, sizeof *out);
	if (IP6_IS_V4(from)) {
		u32 src;
		rt_lock();
		src = rt_ip_select_source(rt_htonl(IP6_GET_V4(from)));
		rt_unlock();
		set_v4(out, src ? src : (mac_nif ? mac_nif->addr : 0), port);
	} else {
		rt_in6_addr d, src;
		memcpy(&d, from->addr, 16);
		rt_lock();
		if (rt_ip6_select_source(&d, from->scope ? rt_netif_by_index(from->scope) : NULL, &src) == 0)
			memcpy(out->addr, &src, 16);
		rt_unlock();
		out->port = port;
		out->scope = a6_scoped(out) ? from->scope : 0;
	}
}

static void udp_poll(struct s6 *s)
{
	UDP6iopb *pb;
	while ((pb = s->rcv_q) != NULL) {
		UDP6ReceivePB *p = &pb->csParam.receive;
		struct rt_sockaddr_in6 from;
		u32 fl = sizeof from, len = s->rlen > 65535 ? 65535 : s->rlen;
		int r;
		if (s->loaned)
			return;
		r = rt_api_recvfrom(&ctx6, s->fd, s->rbuf, len, RT_MSG_DONTWAIT, &from, &fl);
		if (r < 0) {
			if (expired(s->rcv_deadline)) {
				q_pop(&s->rcv_q);
				result6(pb, commandTimeout);
				s->rcv_deadline = s->rcv_q ? secs_deadline(((UDP6iopb *)s->rcv_q)->csParam.receive.timeOut) : 0;
				continue;
			}
			return;
		}
		from_sin6(&from, &p->remoteAddr);
		p->rcvBuff = (char *)s->rbuf;
		p->rcvBuffLen = (u16)r;
		local_for(&p->remoteAddr, &p->destAddr, s->lport);
		s->loaned = 1;
		q_pop(&s->rcv_q);
		s->told_data = 0;
		result6(pb, 0);
		s->rcv_deadline = s->rcv_q ? secs_deadline(((UDP6iopb *)s->rcv_q)->csParam.receive.timeOut) : 0;
	}
	if (!s->told_data && !s->loaned && ready(s->fd, 0)) {
		s->told_data = 1;
		asr6(s, UDPDataArrival, 0);
	}
}

static int udp_write(UDP6iopb *pb)
{
	UDP6SendPB *p = &pb->csParam.send;
	struct s6 *s = stream_of(pb, 0);
	struct rt_sockaddr_in6 to;
	u8 buf[1600];                   /* a datagram at most this big: one Ethernet frame and some */
	const u8 *w = (const u8 *)p->wdsPtr;
	u32 n = 0;
	int r;
	if (!s)
		return invalidStreamPtr;
	if (!w)
		return invalidWDS;
	if (a6_unspec(&p->remoteAddr) || a6_v4any(&p->remoteAddr))
		return ip6BadAddrErr;
	if ((r = to_sin6(&p->remoteAddr, &to)) != 0)
		return r;
	for (; rt_get_be16(w); w += 6) {
		u32 len = rt_get_be16(w);
		const u8 *d;
		memcpy(&d, w + 2, 4);
		if (n + len > sizeof buf)
			return invalidLength;
		memcpy(buf + n, d, len);
		n += len;
	}
	r = rt_api_sendto(&ctx6, s->fd, buf, n, RT_MSG_DONTWAIT, &to, sizeof to);
	return r < 0 ? dest_err(-r, ipDestDeadErr) : 0;
}

static int udp_mtu(UDP6iopb *pb)
{
	const IP6Address *a = &pb->csParam.mtu.remoteAddr;
	u32 m4 = (mac_nif ? mac_nif->mtu : 1500) - 28, m6 = (mac_nif ? rt_ip6_link_mtu(mac_nif) : 1500) - 48;
	if (!IP6_IS_V4(a) && !a6_unspec(a)) {
		struct rt_nexthop6 nh;
		rt_in6_addr d;
		memcpy(&d, a->addr, 16);
		rt_lock();
		if (rt_ip6_route(&d, a->scope ? rt_netif_by_index(a->scope) : NULL, &nh) == 0 && nh.mtu)
			m6 = nh.mtu - 48;
		rt_unlock();
	}
	pb->csParam.mtu.mtuSize = (u16)(IP6_IS_V4(a) ? m4 : !a6_unspec(a) ? m6 : m4 < m6 ? m4 : m6);
	return 0;
}

static int udp_call(UDP6iopb *pb)
{
	struct s6 *s;
	switch (pb->csCode) {
	case UDPCreate:
		return udp_create(pb);
	case UDPRead:
		if (!(s = stream_of(pb, 0)))
			return invalidStreamPtr;
		if (!s->rcv_q)
			s->rcv_deadline = secs_deadline(pb->csParam.receive.timeOut);
		q_add(&s->rcv_q, pb);
		udp_poll(s);
		return 1;
	case UDPBfrReturn:
		if (!(s = stream_of(pb, 0)))
			return invalidStreamPtr;
		if (!pb->csParam.receive.rcvBuff)
			return invalidBufPtr;
		s->loaned = 0;
		return 0;
	case UDPWrite:
		return udp_write(pb);
	case UDPRelease:
		if (!(s = stream_of(pb, 0)))
			return invalidStreamPtr;
		pb->csParam.create.rcvBuff = (char *)s->rbuf;
		pb->csParam.create.rcvBuffLen = s->rlen;
		stream_free(s, connectionTerminated);
		return 0;
	case UDPMaxMTUSize:
		return udp_mtu(pb);
	}
	return controlErr;
}

/* ==================== names (ip6Resolve, ip6ResolveAddr) ==================== */
#define DNS_TRY_MS   2500               /* a query sent again (to the next server) after this */
#define DNS_TRIES    4                  /* sends of one query before it is given up */
#define DNS_DEFAULT  10                 /* seconds of a lookup whose block says 0 */
#define LOOK_ADDRS   16                 /* addresses kept per lookup (of each family, 8) */
enum { QW_IDLE = 0, QW_WAIT, QW_DONE };
struct dq {
	u16 id, type;                   /* AAAA (28), A (1) or PTR (12) */
	u8 state, tries;
	u8 nx, answered;                /* no such name; a NOERROR answer came */
	u32 deadline;
};
struct look {
	IP6iopb *pb;
	u32 own_lo, own_hi;             /* the partition of the parameter block (ipp6_reap) */
	int fd;
	int ptr, searched, server;
	u32 end;                        /* the whole lookup's deadline */
	char name[256];                 /* the name asked for now */
	char canon[256];
	struct dq q[2];                 /* forward: AAAA and A; reverse: PTR in q[0] */
	rt_in6_addr a6[LOOK_ADDRS / 2], a4[LOOK_ADDRS / 2];
	int n6, n4;
};
static struct look looks[NLOOK];
static u16 dns_id = 0x6600;

static int dns_send(struct look *L, struct dq *q)
{
	rt_in6_addr srv[4];
	u32 idx[4];
	u8 src[4], pkt[300];
	int n = rt_dns_get_servers6(srv, idx, src, 4), len = 12, k;
	const char *nm = L->name;
	struct rt_sockaddr_in6 to;
	if (n <= 0)
		return noNameServer;
	if (n > 4)
		n = 4;
	memset(pkt, 0, 12);
	q->id = ++dns_id;
	rt_put_be16(pkt, q->id);
	pkt[2] = 0x01;                                  /* recursion desired */
	rt_put_be16(pkt + 4, 1);                        /* one question */
	while (*nm) {                                   /* the name in labels */
		const char *dot = nm;
		while (*dot && *dot != '.')
			dot++;
		k = (int)(dot - nm);
		if (k < 1 || k > 63 || len + k + 6 > (int)sizeof pkt)
			return nameSyntaxErr;
		pkt[len++] = (u8)k;
		memcpy(pkt + len, nm, (u32)k);
		len += k;
		nm = *dot ? dot + 1 : dot;
	}
	pkt[len++] = 0;
	rt_put_be16(pkt + len, q->type);
	rt_put_be16(pkt + len + 2, 1);                  /* IN */
	len += 4;
	memset(&to, 0, sizeof to);
	to.sin6_len = sizeof to;
	to.sin6_family = RT_AF_INET6;
	to.sin6_port = rt_htons(53);
	to.sin6_addr = srv[(u32)L->server % (u32)n];
	to.sin6_scope_id = idx[(u32)L->server % (u32)n];
	q->state = QW_WAIT;
	q->tries++;
	q->deadline = rt_now_ms() + DNS_TRY_MS;
	if (rt_api_sendto(&ctx6, L->fd, pkt, (u32)len, RT_MSG_DONTWAIT, &to, sizeof to) < 0)
		return 1;                               /* a send that failed is a try that got no answer */
	return 1;
}

/* a name in the message at p into out (dotted), compressed names followed; the byte after it */
static int dns_name(const u8 *m, int mlen, int p, char *out, int olen)
{
	int o = 0, end = -1, guard = 0;
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
	return end;
}

static void look_free(struct look *L)
{
	L->pb = NULL;
	if (L->fd >= 0)
		rt_api_close(&ctx6, L->fd);
	L->fd = -1;
}

/* the forward lookup's answer: the addresses in the order asked for, into the caller's array */
static void look_finish(struct look *L)
{
	IP6iopb *pb = L->pb;
	IP6ResolvePB *p = &pb->csParam.resolve;
	rt_in6_addr all[LOOK_ADDRS];
	int n = 0, order = p->flags & kIP6OrderMask, r;
	if (order == kIP6OrderDefault)
		order = order_now();
	if (order == kIP6OrderIPv4First) {
		for (int i = 0; i < L->n4; i++) all[n++] = L->a4[i];
		for (int i = 0; i < L->n6; i++) all[n++] = L->a6[i];
	} else {
		for (int i = 0; i < L->n6; i++) all[n++] = L->a6[i];
		for (int i = 0; i < L->n4; i++) all[n++] = L->a4[i];
		if (order == kIP6OrderRFC6724 && n > 1) {
			rt_lock();
			rt_inet_sort_dest(all, n);
			rt_unlock();
		}
	}
	if (n > p->maxResults && L->n6 && L->n4) {
		/* More addresses than the caller has room for, and both families: alternate them, starting with
		   the family the order chose first (RFC 8305, section 4), so that the answer holds some of each.
		   Cut off in plain order, a name with eight IPv6 addresses (NPO's radio streams) filled an
		   eight-entry array with IPv6 only, and a program on a network whose IPv6 did not reach the
		   server never tried IPv4. */
		rt_in6_addr v6[LOOK_ADDRS], v4[LOOK_ADDRS];
		int c6 = 0, c4 = 0, i6 = 0, i4 = 0, m = 0, six = !rt_in6_is_v4mapped(&all[0]);
		for (int i = 0; i < n; i++)                 /* each family keeps the order chosen above */
			if (rt_in6_is_v4mapped(&all[i]))
				v4[c4++] = all[i];
			else
				v6[c6++] = all[i];
		while (i6 < c6 || i4 < c4) {
			if ((six && i6 < c6) || i4 >= c4)
				all[m++] = v6[i6++];
			else
				all[m++] = v4[i4++];
			six = !six;
		}
	}
	if (n > p->maxResults)
		n = p->maxResults;
	for (int i = 0; i < n; i++) {
		memcpy(p->results[i].addr, &all[i], 16);
		p->results[i].port = p->port;
		p->results[i].scope = 0;
	}
	p->count = (u16)n;
	if (p->canonName && p->canonNameLen)
		rt_strlcpy(p->canonName, L->canon[0] ? L->canon : L->name, p->canonNameLen);
	if (n)
		r = 0;
	else if (L->q[0].nx || L->q[1].nx)
		r = authNameErr;
	else
		r = noAnsErr;
	look_free(L);
	result6(pb, r);
}

/* both queries are over: the answer, or the name again with the search domain */
static int look_retry_search(struct look *L)
{
	const char *dom = rt_dns_search_list();
	u32 k = 0;
	char *e;
	if (L->ptr || L->searched || !dom || !*dom || strchr(L->name, '.') || L->n4 || L->n6 ||
	    !(L->q[0].nx || L->q[1].nx))
		return 0;
	L->searched = 1;
	e = L->name + strlen(L->name);
	*e++ = '.';
	while (dom[k] && dom[k] != ' ' && dom[k] != ',' && e < L->name + sizeof L->name - 1)
		*e++ = dom[k++];
	*e = 0;
	for (int i = 0; i < 2; i++)
		if (L->q[i].type) {
			L->q[i].tries = 0;
			L->q[i].nx = L->q[i].answered = 0;
			dns_send(L, &L->q[i]);
		}
	return 1;
}

static void look_answer(struct look *L, const u8 *m, int n)
{
	int qd = rt_get_be16(m + 4), an = rt_get_be16(m + 6), p = 12, rcode = m[3] & 15;
	struct dq *q = NULL;
	for (int i = 0; i < 2; i++)
		if (L->q[i].type && L->q[i].state == QW_WAIT && L->q[i].id == rt_get_be16(m))
			q = &L->q[i];
	if (!q || !(m[2] & 0x80))
		return;                                 /* not an answer to a query still waiting */
	if (rcode == 3) {
		q->state = QW_DONE;
		q->nx = 1;
		return;
	}
	if (rcode) {
		/* the server could not answer (SERVFAIL, REFUSED): the next server, at once */
		L->server++;
		if (q->tries < DNS_TRIES)
			dns_send(L, q);
		else
			q->state = QW_DONE;
		return;
	}
	q->state = QW_DONE;
	q->answered = 1;
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
		if (q->type == 28 && type == 28 && rdlen == 16 && L->n6 < LOOK_ADDRS / 2) {
			memcpy(&L->a6[L->n6++], m + e + 10, 16);
			if (!L->canon[0])
				rt_strlcpy(L->canon, nm, sizeof L->canon);
		} else if (q->type == 1 && type == 1 && rdlen == 4 && L->n4 < LOOK_ADDRS / 2) {
			u32 v4;
			memcpy(&v4, m + e + 10, 4);
			rt_in6_set_v4mapped(&L->a4[L->n4++], v4);
			if (!L->canon[0])
				rt_strlcpy(L->canon, nm, sizeof L->canon);
		} else if (q->type == 12 && type == 12 && !L->canon[0]) {
			dns_name(m, n, e + 10, L->canon, sizeof L->canon);
		}
		p = e + 10 + rdlen;
	}
}

/* the reverse lookup's answer: the name into the caller's buffer */
static void look_finish_ptr(struct look *L)
{
	IP6iopb *pb = L->pb;
	IP6ResolvePB *p = &pb->csParam.resolve;
	int r;
	if (L->canon[0]) {
		rt_strlcpy(p->hostName, L->canon, p->hostNameLen);
		r = strlen(L->canon) + 1 > p->hostNameLen ? ip6TruncatedErr : 0;
	} else {
		r = L->q[0].nx ? authNameErr : noAnsErr;
	}
	look_free(L);
	result6(pb, r);
}

static void looks_poll(void)
{
	for (int i = 0; i < NLOOK; i++) {
		struct look *L = &looks[i];
		u8 m[700];
		int n, waiting = 0;
		if (!L->pb)
			continue;
		while ((n = rt_api_recvfrom(&ctx6, L->fd, m, sizeof m, RT_MSG_DONTWAIT, NULL, NULL)) >= 12)
			look_answer(L, m, n);
		for (int k = 0; k < 2; k++) {
			struct dq *q = &L->q[k];
			if (q->state != QW_WAIT)
				continue;
			if ((s32)(rt_now_ms() - q->deadline) >= 0) {
				if (q->tries >= DNS_TRIES) {
					q->state = QW_DONE;
					continue;
				}
				L->server++;
				dns_send(L, q);
			}
			waiting++;
		}
		/* the whole lookup's time: what has come is the answer (an AAAA answer is not held up for
		   ever by an A query that gets none, nor the other way round) */
		if (waiting && !expired(L->end))
			continue;
		if (!waiting && look_retry_search(L))
			continue;
		if (L->ptr)
			look_finish_ptr(L);
		else
			look_finish(L);
	}
}

static struct look *look_new(IP6iopb *pb, int *err)
{
	struct look *L = NULL;
	u16 t = pb->csParam.resolve.timeout;
	for (int i = 0; i < NLOOK; i++)
		if (!looks[i].pb) {
			L = &looks[i];
			break;
		}
	if (!L) {
		*err = outOfMemory;
		return NULL;
	}
	memset(L, 0, sizeof *L);
	L->fd = socket6(RT_SOCK_DGRAM, RT_IPPROTO_UDP);
	if (L->fd < 0) {
		*err = outOfMemory;
		return NULL;
	}
	L->end = rt_now_ms() + (t ? t : DNS_DEFAULT) * 1000u;
	L->pb = pb;
	return L;
}

static int resolve_call(IP6iopb *pb)
{
	IP6ResolvePB *p = &pb->csParam.resolve;
	const char *name = p->hostName;
	IP6Address lit;
	struct look *L;
	u32 a4[LOOK_ADDRS / 2];
	rt_in6_addr a6[LOOK_ADDRS / 2];
	int n4 = 0, n6 = 0, err, r;
	u16 fl = p->flags;
	p->count = 0;
	if (!name || !p->results)
		return invalidBufPtr;
	if (!p->maxResults)
		return invalidLength;
	if (!name[0] || strlen(name) > 253)
		return nameSyntaxErr;
	/* an address literal is its own answer */
	r = parse_literal(name, &lit);
	if (r == ip6ScopeErr)
		return r;
	if (r == 0) {
		lit.port = p->port;
		p->results[0] = lit;
		p->count = 1;
		if (p->canonName && p->canonNameLen)
			rt_strlcpy(p->canonName, name, p->canonNameLen);
		return 0;
	}
	/* the hosts table and the cache: a family found there is not asked for again */
	if (!(fl & kIP6OnlyIPv6))
		n4 = rt_resolve_local46(name, 0, a4, NULL, LOOK_ADDRS / 2);
	if (!(fl & kIP6OnlyIPv4))
		n6 = rt_resolve_local46(name, 1, NULL, a6, LOOK_ADDRS / 2);
	if (!(L = look_new(pb, &err)))
		return err;
	for (int i = 0; i < n4; i++)
		rt_in6_set_v4mapped(&L->a4[L->n4++], a4[i]);
	for (int i = 0; i < n6; i++)
		L->a6[L->n6++] = a6[i];
	rt_strlcpy(L->name, name, sizeof L->name);
	if (L->name[strlen(L->name) - 1] == '.') {      /* a trailing dot: the name as it is */
		L->name[strlen(L->name) - 1] = 0;
		L->searched = 1;
	}
	if (fl & kIP6NoSearch)
		L->searched = 1;
	if ((n4 || (fl & kIP6OnlyIPv6)) && (n6 || (fl & kIP6OnlyIPv4))) {
		look_finish(L);                         /* everything asked for was known */
		return 1;                               /* completed already (result6 ran) */
	}
	if (!n6 && !(fl & kIP6OnlyIPv4))
		L->q[0].type = 28;
	if (!n4 && !(fl & kIP6OnlyIPv6))
		L->q[1].type = 1;
	for (int i = 0; i < 2; i++)
		if (L->q[i].type && (r = dns_send(L, &L->q[i])) != 1) {
			if (n4 || n6) {                 /* no server, but the hosts table knew one family */
				look_finish(L);
				return 1;
			}
			look_free(L);
			return r;
		}
	return 1;
}

static int resolve_addr_call(IP6iopb *pb)
{
	IP6ResolvePB *p = &pb->csParam.resolve;
	const u8 *a = p->addr.addr;
	struct look *L;
	int err, r;
	if (!p->hostName || !p->hostNameLen)
		return invalidBufPtr;
	if (!(L = look_new(pb, &err)))
		return err;
	L->ptr = 1;
	L->searched = 1;
	if (IP6_IS_V4(&p->addr)) {
		rt_snprintf(L->name, sizeof L->name, "%u.%u.%u.%u.in-addr.arpa", a[15], a[14], a[13], a[12]);
	} else {
		static const char hex[] = "0123456789abcdef";
		char *o = L->name;
		for (int i = 15; i >= 0; i--) {
			*o++ = hex[a[i] & 15];
			*o++ = '.';
			*o++ = hex[a[i] >> 4];
			*o++ = '.';
		}
		rt_strlcpy(o, "ip6.arpa", sizeof L->name - (u32)(o - L->name));
	}
	L->q[0].type = 12;
	if ((r = dns_send(L, &L->q[0])) != 1) {
		look_free(L);
		return r;
	}
	return 1;
}

static int cancel_call(IP6iopb *pb)
{
	IP6iopb *t = pb->csParam.cancel.target;
	for (int i = 0; i < NLOOK; i++)
		if (looks[i].pb && looks[i].pb == t) {
			look_free(&looks[i]);
			result6(t, ip6CancelledErr);    /* its completion runs before this call's */
			return 0;
		}
	return ip6NotPendingErr;
}

/* ==================== dispatch ==================== */
static int anywhere(void *pb);
static int handle6(void *pb)
{
	int cs = PB_CSCODE(pb);
	note_call6(cs);
	if (cs >= 30 && cs <= 43) {
		int r = tcp_call(pb);
		if (r != controlErr)
			return r;
	} else if (cs >= 20 && cs <= 25) {
		int r = udp_call(pb);
		if (r != controlErr)
			return r;
	} else {
		switch (cs) {
		case ip6GetInfo:
		case ip6AddrToStr:
		case ip6StrToAddr:
			return anywhere(pb);            /* made asynchronously: completed from here */
		case ip6GetAddrs:
			return get_addrs(pb);
		case ip6Resolve:
			return resolve_call(pb);
		case ip6ResolveAddr:
			return resolve_addr_call(pb);
		case ip6ResolveCancel:
			return cancel_call(pb);
		}
	}
	{
		static u8 unsup[96];
		if (cs < 0 || cs >= 96 || !unsup[cs]) {
			if (cs >= 0 && cs < 96)
				unsup[cs] = 1;
			rt_log(RT_LOG_WARN, ".IPP6 csCode %d (%s) is not supported: controlErr", cs, cs_name6(cs));
		}
	}
	return controlErr;
}

/* answered in the Control call, from any context: they read nothing the stack's task changes */
static int anywhere(void *pb)
{
	IP6iopb *p = pb;
	switch (p->csCode) {
	case ip6GetInfo:
		return get_info(p);
	case ip6AddrToStr:
		if (!p->csParam.resolve.hostName || !p->csParam.resolve.hostNameLen)
			return invalidBufPtr;
		return format_addr(&p->csParam.resolve.addr, p->csParam.resolve.hostName, p->csParam.resolve.hostNameLen);
	case ip6StrToAddr: {
		int r;
		if (!p->csParam.resolve.hostName)
			return invalidBufPtr;
		r = parse_literal(p->csParam.resolve.hostName, &p->csParam.resolve.addr);
		if (r == 0)
			p->csParam.resolve.addr.port = p->csParam.resolve.port;
		return r;
	}
	}
	return 1;
}
/* the immediate calls: answered on the spot when made synchronously from a completion or
   notification routine (which the stack's task is running), as MacTCP answers them */
static int immediate(int cs)
{
	return cs == TCPStatus || cs == TCPGlobalInfo || cs == TCPRcvBfrReturn || cs == UDPBfrReturn ||
	       cs == UDPMaxMTUSize || cs == ip6GetAddrs;
}

/* drvr.S: the Control entry of .IPP6 (any level), with the caller's A5 */
int ipp6_control_a5(void *pb, void *dce, void *a5)
{
	int cs = PB_CSCODE(pb), r;
	if (PB_IOTRAP(pb) & 0x200)
		return controlErr;                        /* immediate calls cannot wait for the stack */
	/* made synchronously, these are answered here, in any context; made asynchronously they are
	   queued like the rest, so that their completion routine is called as every other's (A0 and the
	   stack: the Device Manager's IODone gives it A0 only) */
	if ((cs == ip6GetInfo || cs == ip6AddrToStr || cs == ip6StrToAddr) && !(PB_IOTRAP(pb) & 0x400)) {
		if (!cs_seen6[cs])
			note_call6(cs);
		return anywhere(pb);
	}
	if (ipp_in_poll() && !(PB_IOTRAP(pb) & 0x400) && immediate(cs) && ctx6_ok) {
		r = handle6(pb);
		if (r != 1)
			return r;
	}
	if (cs == TCPCreate || cs == UDPCreate)
		ipp_keep_a5(pb, a5);                      /* its notification routine gets it back */
	{
		u16 sr = mac_irq_off();
		void **pp = (void **)&incoming6;
		ipp_dm_dequeue(pb, dce);                  /* off the Device Manager's queue before its qLink is ours (ipp.c) */
		PB_QLINK(pb) = NULL;
		while (*pp)
			pp = &PB_QLINK(*pp);
		*pp = pb;
		mac_irq_restore(sr);
	}
	mac_schedule();
	return 1;
}

/* drvr.S: Open (the stack is running: .IPP6 is installed only once it is), Close (refused, as
   .IPP's: the driver stays open for every program) */
int ipp6_open(void *pb, void *dce)
{
	(void)pb; (void)dce;
	opens6++;
	mac_schedule();
	return 0;
}
int ipp6_close(void *pb, void *dce)
{
	(void)pb; (void)dce;
	closes6++;
	mac_schedule();
	return closErr;
}

/* ipp.c ipp_poll, in the stack's task, every round */
void ipp6_poll(void)
{
	static u32 opens_logged;
	if (!ctx6_ok) {
		if (rt_ctx_init(&ctx6, NFDS6, NULL) < 0)
			return;
		ctx6_ok = 1;
	}
	if (opens6 != opens_logged) {
		opens_logged = opens6;
		if (opens_logged <= 16)
			rt_log(RT_LOG_INFO, ".IPP6: opened by a program (open %lu)", (unsigned long)opens_logged);
	}
	if (closes6 != closes6_logged) {
		closes6_logged = closes6;
		if (closes6_logged <= 16)
			rt_log(RT_LOG_INFO, ".IPP6: Close refused (closErr): the driver stays open (close %lu)",
			       (unsigned long)closes6_logged);
	}
	if (!features_ms || (u32)(rt_now_ms() - features_ms) >= 500) {
		features_cache = features_now();
		features_ms = rt_now_ms() | 1;
	}
	for (;;) {
		u16 sr = mac_irq_off();
		void *pb = incoming6;
		if (pb)
			incoming6 = PB_QLINK(pb);
		mac_irq_restore(sr);
		if (!pb)
			break;
		int r = handle6(pb);
		if (r != 1)
			result6(pb, r);
	}
	for (int i = 0; i < NSTREAMS6; i++) {
		struct s6 *s = &streams6[i];
		if (s->magic != S6_MAGIC)
			continue;
		if (s->tcp)
			tcp_poll(s);
		else
			udp_poll(s);
	}
	looks_poll();
}

/* ==================== programs that have quit (ipp.c reap) ==================== */
static int part_of(u32 a, const u32 *lo, const u32 *hi, int n)
{
	for (int k = 0; k < n; k++)
		if (a >= lo[k] && a < hi[k])
			return k;
	return -1;
}
/* 1: the owner recorded is gone; the first look records the owner (1: not a program's, kept) */
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
void ipp6_reap(const u32 *lo, const u32 *hi, int n)
{
	if (!ctx6_ok)
		return;
	for (int i = 0; i < NSTREAMS6; i++) {
		struct s6 *s = &streams6[i];
		if (s->magic != S6_MAGIC || !owner_gone(&s->own_lo, &s->own_hi, (u32)s->a5, lo, hi, n))
			continue;
		rt_log(RT_LOG_WARN, ".IPP6: %s stream %08lx of a program that quit without releasing it: released",
		       s->tcp ? "TCP" : "UDP", (unsigned long)s);
		/* nothing of the program's is touched again: its calls are dropped, not completed */
		s->notify = NULL;
		s->rcv_q = s->snd_q = s->open_pb = NULL;
		if (s->tcp && s->conn)
			linger0(s);
		stream_close(s);
		s->magic = 0;
	}
	for (int i = 0; i < NLOOK; i++) {
		struct look *L = &looks[i];
		if (!L->pb || !owner_gone(&L->own_lo, &L->own_hi, (u32)L->pb, lo, hi, n))
			continue;
		rt_log(RT_LOG_WARN, ".IPP6: a lookup of a program that has quit: dropped");
		look_free(L);
	}
}

/* ==================== riptide.conf: `ipp6 order rfc6724|ipv6|ipv4` ==================== */
int ipp6_config(int argc, char **argv, char *err, u32 errlen)
{
	if (argc == 3 && !rt_strcasecmp(argv[1], "order")) {
		if (!rt_strcasecmp(argv[2], "rfc6724"))
			order_default = kIP6OrderRFC6724;
		else if (!rt_strcasecmp(argv[2], "ipv6"))
			order_default = kIP6OrderIPv6First;
		else if (!rt_strcasecmp(argv[2], "ipv4"))
			order_default = kIP6OrderIPv4First;
		else
			goto usage;
		return 0;
	}
usage:
	rt_snprintf(err, errlen, "usage: ipp6 order rfc6724|ipv6|ipv4");
	return -RT_EINVAL;
}

/* ==================== installing .IPP6 (ipp.c ipp_opened, at the stack's start) ==================== */
/* the unit table (Inside Macintosh: Devices): UTableBase, UnitNtryCnt; a DCE's dCtlDriver at 0,
   dCtlFlags at 4 (dRAMBased 0x40 in the low byte: the driver is a handle), dCtlDelay 34, dCtlEMask 36,
   dCtlMenu 38; a driver's name at 18 */
#define UTableBase  (*(u32 **)0x11C)
#define UnitNtryCnt (*(s16 *)0x1D2)
#define FIRST_UNIT  48                  /* the INIT's rule: the ROM, AppleTalk and the slots use the low ones */
static int unit_named(const u8 *pname)
{
	for (int u = 0; u < UnitNtryCnt; u++) {
		u8 **h = (u8 **)UTableBase[u], *dce, *drv;
		if (!h || !(dce = *h) || !(drv = *(u8 **)dce))
			continue;
		if (dce[5] & 0x40) {                    /* dRAMBased: dCtlDriver is a handle */
			drv = *(u8 **)drv;
			if (!drv)
				continue;
		}
		if (drv[18] == pname[0] && !rt_strncasecmp((const char *)drv + 19, (const char *)pname + 1, pname[0]))
			return u;
	}
	return -1;
}
void ipp6_install(void)
{
	static const u8 name[] = "\x05.IPP6";
	u8 *drv = (u8 *)_drvr6_start, *dce;
	int u = unit_named(name);
	short r;
	if (u >= 0) {
		rt_log(RT_LOG_WARN, ".IPP6: a driver of that name is installed already (unit %d): left as it is", u);
		return;
	}
	for (u = FIRST_UNIT; u < UnitNtryCnt && UTableBase[u]; u++)
		;
	if (u >= UnitNtryCnt) {
		rt_log(RT_LOG_ERR, ".IPP6: no free unit table entry: .IPP6 is not installed");
		return;
	}
	r = mac_drvrinstall(drv, (short)-(u + 1));
	if (r || !UTableBase[u] || !(dce = *(u8 **)UTableBase[u])) {
		rt_log(RT_LOG_ERR, ".IPP6: DrvrInstall failed (%d): .IPP6 is not installed", r);
		return;
	}
	/* _DrvrInstall makes the device control entry; the driver and its header's settings go in by
	   hand, as the INIT does for .IPP (a pointer, not a handle: no dRAMBased) */
	*(u8 **)dce = drv;
	PB_W(dce, 4) = PB_W(drv, 0) & 0xFF00;
	PB_W(dce, 34) = PB_W(drv, 2);
	PB_W(dce, 36) = PB_W(drv, 4);
	PB_W(dce, 38) = PB_W(drv, 6);
	features_cache = features_now();
	r = mac_newgestalt(gestaltIPP6, (void *)mac_gestalt_ipp6);
	rt_log(RT_LOG_INFO, ".IPP6 installed (unit %d, reference number %d), interface version %d%s", u, -(u + 1),
	       IPP6_VERSION, r ? ", but Gestalt 'ipp6' could not be" : ", Gestalt 'ipp6'");
}
