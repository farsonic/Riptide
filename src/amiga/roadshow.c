/*
 * Riptide - the Roadshow extensions of bsdsocket.library (version 4):
 * packet filter channels, the interface, route, DNS, statistics and
 * monitoring APIs, network database enumeration, address conversion,
 * configuration data, the reentrant resolver calls and the Roadshow
 * flavour of getaddrinfo() (its own struct addrinfo layout and codes).
 *
 * Vector offsets are those of the Roadshow SDK (src/amiga/lib_lvos.txt).
 * Everything runs in the calling task. Reading the stack's state takes
 * the stack lock; changes that a riptide.conf line expresses (routes,
 * interface addresses and state, DHCP, adding and removing interfaces)
 * are sent to the Riptide process as RIPTIDE port commands, so they act
 * exactly like the ip/route/ifconfig commands and the configuration file.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/bsdsocket_impl.h"
#include "roadshow.h"
#include "rt/rt_bpf.h"
#include "rt/rt_bsdinfo.h"
#include "rt/rt_monitor.h"
#include "rt/rt_ctl.h"
#include "rt/rt_ip6.h"
#include "rt/rt_stats.h"

#define CTX struct TaskCtx *c = base_ctx(base)

/* riptide.library functions reused here (riptidelib.c) */
ULONG riptide_inet_ntop(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) const APTR addrptr,
                        RTREG(a1) STRPTR strptr, RTREG(d1) LONG len);
LONG riptide_inet_pton(RTREG(a6) struct Library *base, RTREG(d0) LONG family, RTREG(a0) CONST_STRPTR strptr,
                       RTREG(a1) APTR addrptr);
LONG riptide_getaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR hostname,
                         RTREG(a1) CONST_STRPTR servname, RTREG(a2) const APTR hintsp, RTREG(a3) APTR *result);
LONG riptide_getnameinfo(RTREG(a6) struct Library *base, RTREG(a0) const APTR sa, RTREG(d0) LONG salen,
                         RTREG(a1) STRPTR host, RTREG(d1) LONG hostlen, RTREG(a2) STRPTR serv,
                         RTREG(d2) LONG servlen, RTREG(d3) LONG flags);

static LONG fail(struct TaskCtx *c, int err)
{
	bsd_set_errno(c, err);
	return -1;
}

static LONG ret(struct TaskCtx *c, int r)
{
	return r < 0 ? fail(c, -r) : r;
}

/* utility.library NextTagItem(): TAG_DONE, TAG_IGNORE, TAG_MORE, TAG_SKIP */
static struct TagItem *next_tag(struct TagItem **tp)
{
	struct TagItem *t = *tp;

	while (t) {
		switch (t->ti_Tag) {
		case TAG_DONE:
			*tp = NULL;
			return NULL;
		case TAG_IGNORE:
			t++;
			continue;
		case TAG_MORE:
			t = (struct TagItem *)t->ti_Data;
			continue;
		case TAG_SKIP:
			t += 1 + t->ti_Data;
			continue;
		}
		*tp = t + 1;
		return t;
	}
	*tp = NULL;
	return NULL;
}

/* One riptide.conf line for the Riptide process (the RIPTIDE port, as
 * the Riptide command sends it). 0 or -errno. */
static int port_cmd(const char *line)
{
	struct MsgPort *reply, *port;
	struct RiptideMsg *m;
	char *out;
	int r;

	reply = CreateMsgPort();
	m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
	out = AllocVec(256, MEMF_PUBLIC | MEMF_CLEAR);
	if (!reply || !m || !out) {
		r = -RT_ENOMEM;
		goto done;
	}
	m->msg.mn_Node.ln_Type = NT_MESSAGE;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = RTCMD_COMMAND;
	rt_strlcpy(m->text, line, sizeof(m->text));
	m->reply = (STRPTR)out;
	m->replysize = 256;
	Forbid();
	port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
	if (port)
		PutMsg(port, &m->msg);
	Permit();
	if (!port) {
		r = -RT_ENETDOWN;
		goto done;
	}
	WaitPort(reply);
	GetMsg(reply);
	r = m->result ? -(int)m->result : 0;
done:
	if (out)
		FreeVec(out);
	if (m)
		FreeVec(m);
	if (reply)
		DeleteMsgPort(reply);
	return r;
}

/* A host name or dotted-decimal address (network order). */
static int parse_host(const char *s, u32 *a)
{
	if (!s || !*s)
		return -RT_EINVAL;
	if (amiga_inet_aton(s, a))
		return 0;
	return rt_resolve_name(s, a, 1, NULL, 0) > 0 ? 0 : -RT_EINVAL;
}

static char *fmt_ip(u32 a, char *buf)
{
	return rt_format_ipv4(a, buf);
}

static int valid_ifname(const char *n)
{
	return n && *n && strlen(n) < RT_IFNAMSIZ && !strchr(n, ' ') && !strchr(n, '\t');
}

/* ======================================================================
 * Packet filter (src/core/bpf.c)
 * ==================================================================== */

static void bpf_signal(void *task, u32 mask)
{
	Signal((struct Task *)task, mask);
}

LONG bsd_bpf_open(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_open(channel, c);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_close(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_close(channel, c);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_read(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(a0) APTR buffer,
                  RTREG(d1) LONG len)
{
	CTX;
	u32 intr;
	int r;

	rt_lock();
	intr = rt_bpf_intr_mask(channel, c);
	r = rt_bpf_read(channel, c, buffer, (u32)len, &intr);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_write(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(a0) APTR buffer,
                   RTREG(d1) LONG len)
{
	CTX;
	int r;

	if (len < 0)
		return fail(c, RT_EINVAL);
	rt_lock();
	r = rt_bpf_write(channel, c, buffer, (u32)len);
	rt_unlock();
	rt_kick();
	return ret(c, r);
}

LONG bsd_bpf_set_notify_mask(RTREG(a6) struct Library *base, RTREG(d1) LONG channel,
                             RTREG(d0) ULONG signal_mask)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_set_notify(channel, c, FindTask(NULL), signal_mask);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_set_interrupt_mask(RTREG(a6) struct Library *base, RTREG(d0) LONG channel,
                                RTREG(d1) ULONG signal_mask)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_set_intr(channel, c, signal_mask);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_ioctl(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(d1) ULONG command,
                   RTREG(a0) APTR buffer)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_ioctl(channel, c, command, buffer);
	rt_unlock();
	return ret(c, r);
}

LONG bsd_bpf_data_waiting(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	CTX;
	int r;

	rt_lock();
	r = rt_bpf_waiting(channel, c);
	rt_unlock();
	return ret(c, r);
}

/* ======================================================================
 * Routes
 * ==================================================================== */

/* Classful network of a destination without a host part (BSD route). */
static int classful_bits(u32 a)
{
	u32 h = rt_ntohl(a);

	if ((h & 0x80000000u) == 0)
		return (h & 0x00ffffffu) ? 32 : 8;
	if ((h & 0xc0000000u) == 0x80000000u)
		return (h & 0x0000ffffu) ? 32 : 16;
	return (h & 0x000000ffu) ? 32 : 24;
}

static int route_tags(struct TaskCtx *c, const char *op, struct TagItem *tags, int del)
{
	struct TagItem *t, *tp = tags;
	const char *dst = NULL, *gw = NULL, *defgw = NULL;
	int kind = 0;              /* 0 by address, 'h' host, 'n' net */
	u32 d, g;
	char line[96], a[16], b[16];
	int bits;

	while ((t = next_tag(&tp)) != NULL) {
		switch (t->ti_Tag) {
		case RTA_Destination:     dst = (const char *)t->ti_Data; kind = 0; break;
		case RTA_DestinationHost: dst = (const char *)t->ti_Data; kind = 'h'; break;
		case RTA_DestinationNet:  dst = (const char *)t->ti_Data; kind = 'n'; break;
		case RTA_Gateway:         gw = (const char *)t->ti_Data; break;
		case RTA_DefaultGateway:  defgw = (const char *)t->ti_Data; break;
		}
	}
	if (defgw) {
		if (dst || gw)
			return fail(c, RT_EINVAL);
		if (parse_host(defgw, &g) < 0)
			return fail(c, RT_EINVAL);
		rt_snprintf(line, sizeof(line), "route %s default via %s", op, fmt_ip(g, a));
		return ret(c, port_cmd(line));
	}
	if (!dst || parse_host(dst, &d) < 0)
		return fail(c, dst ? RT_EINVAL : RT_EFAULT);
	bits = kind == 'h' ? 32 : classful_bits(d);
	if (kind == 'n' && bits == 32)
		bits = 24;
	if (del) {
		rt_snprintf(line, sizeof(line), "route %s %s/%d", op, fmt_ip(d, a), bits);
	} else {
		if (!gw || parse_host(gw, &g) < 0)
			return fail(c, RT_EINVAL);
		rt_snprintf(line, sizeof(line), "route %s %s/%d via %s", op, fmt_ip(d, a), bits, fmt_ip(g, b));
	}
	return ret(c, port_cmd(line));
}

LONG bsd_AddRouteTagList(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	CTX;
	return route_tags(c, "add", tags, 0);
}

LONG bsd_DeleteRouteTagList(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	CTX;
	return route_tags(c, "del", tags, 1);
}

LONG bsd_ChangeRouteTagList(RTREG(a6) struct Library *base, RTREG(a0) APTR tags)
{
	CTX;
	return route_tags(c, "change", tags, 0);
}

void bsd_FreeRouteInfo(RTREG(a6) struct Library *base, RTREG(a0) APTR buf)
{
	if (buf)
		FreeVec(buf);
}

ULONG bsd_GetRouteInfo(RTREG(a6) struct Library *base, RTREG(d0) LONG address_family, RTREG(d1) LONG flags)
{
	CTX;
	u8 *buf;
	u32 n;

	if (address_family != 0 && address_family != RT_AF_INET) {
		fail(c, RT_EAFNOSUPPORT);
		return 0;
	}
	rt_lock();
	n = rt_bsd_route_dump((u32)flags, NULL, 0);
	buf = AllocVec(n, MEMF_PUBLIC | MEMF_CLEAR);
	if (buf)
		rt_bsd_route_dump((u32)flags, buf, n);
	rt_unlock();
	if (!buf)
		fail(c, RT_ENOMEM);
	return (ULONG)buf;
}

/* ======================================================================
 * Interfaces
 * ==================================================================== */

LONG bsd_AddInterfaceTagList(RTREG(a6) struct Library *base, RTREG(a0) STRPTR interface_name,
                             RTREG(a1) STRPTR device_name, RTREG(d0) LONG unit, RTREG(a2) APTR tags)
{
	CTX;
	struct TagItem *t, *tp = tags;
	char line[256];
	u32 n;

	if (!valid_ifname((const char *)interface_name) || !device_name || !*device_name ||
	    strchr((const char *)device_name, ' '))
		return fail(c, RT_EINVAL);
	n = (u32)rt_snprintf(line, sizeof(line), "interface %s device=%s unit=%ld", (char *)interface_name,
	                     (char *)device_name, (long)unit);
	while ((t = next_tag(&tp)) != NULL && n < sizeof(line)) {
		switch (t->ti_Tag) {
		case IFA_NumReadRequests:
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " reads=%lu", (unsigned long)t->ti_Data);
			break;
		case IFA_LimitMTU:
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " mtu=%lu", (unsigned long)t->ti_Data);
			break;
		case IFA_HardwareAddress:
			return fail(c, RT_EOPNOTSUPP);
		default:
			break;       /* SANA-II details Riptide handles itself */
		}
	}
	return ret(c, port_cmd(line));
}

LONG bsd_RemoveInterface(RTREG(a6) struct Library *base, RTREG(a0) STRPTR interface_name, RTREG(d0) LONG force)
{
	CTX;
	char line[64];
	int r;

	if (!valid_ifname((const char *)interface_name)) {
		fail(c, RT_EINVAL);
		return 0;
	}
	rt_snprintf(line, sizeof(line), "interface %s delete", (char *)interface_name);
	r = port_cmd(line);
	if (r < 0) {
		fail(c, -r);
		return 0;
	}
	return 1;
}

ULONG bsd_ObtainInterfaceList(RTREG(a6) struct Library *base)
{
	CTX;
	struct rt_netif *n;
	struct List *l;
	struct Node *nd;
	char *names;
	int cnt = 0, i = 0;

	rt_lock();
	for (n = rt_netif_list(); n; n = n->next)
		cnt++;
	l = AllocVec(sizeof(*l) + (ULONG)cnt * (sizeof(*nd) + RT_IFNAMSIZ), MEMF_PUBLIC | MEMF_CLEAR);
	if (l) {
		NewList(l);
		nd = (struct Node *)(l + 1);
		names = (char *)(nd + cnt);
		for (n = rt_netif_list(); n && i < cnt; n = n->next, i++) {
			rt_strlcpy(names + i * RT_IFNAMSIZ, n->name, RT_IFNAMSIZ);
			nd[i].ln_Name = names + i * RT_IFNAMSIZ;
			AddTail(l, &nd[i]);
		}
	}
	rt_unlock();
	if (!l)
		fail(c, RT_ENOMEM);
	return (ULONG)l;
}

void bsd_ReleaseInterfaceList(RTREG(a6) struct Library *base, RTREG(a0) APTR list)
{
	if (list)
		FreeVec(list);
}

static void put_sin(APTR p, u32 addr)
{
	struct rt_sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	sin.sin_addr = addr;
	memcpy(p, &sin, sizeof(sin));
}

/* AmigaDOS date of a wall clock time (seconds since 1970, UTC) */
static void datestamp_of(u32 t, struct DateStamp *ds)
{
	u32 s = t > 252460800u ? t - 252460800u : 0;   /* 1978-01-01 */

	ds->ds_Days = (LONG)(s / 86400u);
	ds->ds_Minute = (LONG)((s % 86400u) / 60u);
	ds->ds_Tick = (LONG)((s % 60u) * 50u);
}

/* The DNS servers DHCP (or IPv6 autoconfiguration) learned on nif */
static int if_dns(const struct rt_netif *nif, u32 *out, int max)
{
	rt_in6_addr a[8];
	u32 ix[8];
	u8 src[8];
	int n = rt_dns_get_servers6(a, ix, src, 8), i, k = 0;

	for (i = 0; i < n && i < 8 && k < max; i++)
		if (ix[i] == nif->index && src[i] != RT_DNS_SRC_STATIC && rt_in6_is_v4mapped(&a[i]))
			out[k++] = rt_in6_v4(&a[i]);
	return k;
}

static int bind_type(const struct rt_netif *nif)
{
	struct rt_dhcp_info di;
	int st = rt_dhcp_get_info(nif, &di);

	if (st == RT_DHCPS_BOUND || st == RT_DHCPS_RENEWING || st == RT_DHCPS_REBINDING)
		return di.lease_s == 0xffffffffu ? IFABT_Static : IFABT_Dynamic;
	if (st != RT_DHCPS_NONE)
		return IFABT_Unknown;
	return nif->addr ? IFABT_Static : IFABT_Unknown;
}

LONG bsd_QueryInterfaceTagList(RTREG(a6) struct Library *base, RTREG(a0) STRPTR interface_name,
                               RTREG(a1) APTR tags)
{
	CTX;
	struct TagItem *t, *tp = tags;
	struct rt_netif *nif;
	struct rt_ctl_driver *dr;
	int have_dr;

	if (!interface_name)
		return fail(c, RT_EFAULT);
	dr = AllocVec(sizeof(*dr), MEMF_ANY | MEMF_CLEAR);
	if (!dr)
		return fail(c, RT_ENOMEM);
	rt_lock();
	nif = rt_netif_find((const char *)interface_name);
	if (!nif) {
		rt_unlock();
		FreeVec(dr);
		return fail(c, RT_ENXIO);
	}
	have_dr = rt_drv_fill(nif, dr) == 0;
	while ((t = next_tag(&tp)) != NULL) {
		LONG *lp = (LONG *)t->ti_Data;
		u32 v = 0, dns[2];
		int k;
		if (!lp)
			continue;
		switch (t->ti_Tag) {
		case IFQ_DeviceName:
			rt_strlcpy(c->devname, have_dr && dr->driver[0] ? dr->driver : nif->name, sizeof(c->devname));
			*(STRPTR *)lp = (STRPTR)c->devname;
			continue;
		case IFQ_DeviceUnit:         v = have_dr ? dr->unit : 0; break;
		case IFQ_HardwareAddressSize: v = (u32)nif->hwlen * 8u; break;
		case IFQ_HardwareAddress:
			memcpy(lp, nif->hwaddr, nif->hwlen);
			continue;
		case IFQ_MTU:                v = nif->mtu; break;
		case IFQ_HardwareMTU:        v = have_dr && dr->dq_mtu ? dr->dq_mtu : nif->mtu; break;
		case IFQ_BPS:                v = have_dr ? dr->dq_bps : 0; break;
		case IFQ_HardwareType:
			v = have_dr && dr->dq_hwtype ? dr->dq_hwtype : nif->type == RT_IFT_ETHER ? 1 :
			    nif->type == RT_IFT_LOOP ? 24 : 0;
			break;
		case IFQ_PacketsReceived:    v = nif->stats.ipackets; break;
		case IFQ_PacketsSent:        v = nif->stats.opackets; break;
		case IFQ_BadData:            v = have_dr ? dr->gs_baddata : nif->stats.ierrors; break;
		case IFQ_Overruns:           v = have_dr ? dr->gs_overruns : 0; break;
		case IFQ_UnknownTypes:       v = nif->stats.noproto; break;
		case IFQ_LastStart:
			lp[0] = have_dr ? (LONG)dr->gs_laststart_s : 0;
			lp[1] = 0;
			continue;
		case IFQ_Address:            put_sin(lp, nif->addr); continue;
		case IFQ_DestinationAddress: put_sin(lp, nif->dstaddr); continue;
		case IFQ_BroadcastAddress:   put_sin(lp, nif->broadaddr); continue;
		case IFQ_NetMask:            put_sin(lp, nif->netmask); continue;
		case IFQ_PrimaryDNSAddress:
		case IFQ_SecondaryDNSAddress:
			k = if_dns(nif, dns, 2);
			put_sin(lp, t->ti_Tag == IFQ_PrimaryDNSAddress ? (k > 0 ? dns[0] : 0) : (k > 1 ? dns[1] : 0));
			continue;
		case IFQ_Metric:             v = nif->metric; break;
		case IFQ_State:              v = (nif->flags & RT_IFF_UP) ? SM_Up : SM_Down; break;
		case IFQ_AddressBindType:    v = (u32)bind_type(nif); break;
		case IFQ_AddressLeaseExpires: {
			struct rt_dhcp_info di;
			struct DateStamp *ds = (struct DateStamp *)lp;
			memset(ds, 0, sizeof(*ds));
			if (bind_type(nif) == IFABT_Dynamic && rt_dhcp_get_info(nif, &di) != RT_DHCPS_NONE &&
			    rt_wallclock())
				datestamp_of(rt_wallclock() + di.left_s, ds);
			continue;
		}
		case IFQ_NumReadRequests:
		case IFQ_MaxReadRequests:
			for (k = 0; have_dr && k < RT_CTL_DRV_RXQ; k++)
				v += dr->rxq[k].cfg;
			break;
		case IFQ_NumReadRequestsPending:
			for (k = 0; have_dr && k < RT_CTL_DRV_RXQ; k++)
				v += dr->rxq[k].queued;
			break;
		case IFQ_NumWriteRequests:
		case IFQ_MaxWriteRequests:   v = have_dr ? dr->tx_slots : 0; break;
		case IFQ_NumWriteRequestsPending: v = have_dr ? dr->tx_inflight : 0; break;
		case IFQ_GetBytesIn:
		case IFQ_GetBytesOut:
			lp[0] = 0;
			lp[1] = (LONG)(t->ti_Tag == IFQ_GetBytesIn ? nif->stats.ibytes : nif->stats.obytes);
			continue;
		case IFQ_GetDebugMode:       v = 0; break;
		case IFQ_GetSANA2CopyStats:
			memset(lp, 0, sizeof(struct SANA2CopyStats));
			continue;
		case IFQ_OutputDrops:        v = have_dr ? dr->tx_full : 0; break;
		case IFQ_InputDrops:         v = nif->stats.iqdrops; break;
		case IFQ_OutputErrors:       v = nif->stats.oerrors; break;
		case IFQ_InputErrors:        v = nif->stats.ierrors; break;
		case IFQ_OutputMulticasts:   v = nif->stats.omcasts; break;
		case IFQ_InputMulticasts:    v = nif->stats.imcasts; break;
		case IFQ_IPDrops:            v = rt_ipstat.InDiscards + rt_ipstat.OutDiscards; break;
		case IFQ_ARPDrops:           v = 0; break;
		default:
			continue;                /* not known: left alone */
		}
		*lp = (LONG)v;
	}
	rt_unlock();
	FreeVec(dr);
	return 0;
}

static void if_online(const char *name, int online)
{
	struct amiga_if *ai;

	rt_lock();
	for (ai = amiga_ifs; ai; ai = ai->next)
		if (!strcmp(name, ai->nif.name) && ai->set_online)
			ai->set_online(ai, online);
	rt_unlock();
	rt_kick();
}

LONG bsd_ConfigureInterfaceTagList(RTREG(a6) struct Library *base, RTREG(a0) STRPTR interface_name,
                                   RTREG(a1) APTR tags)
{
	CTX;
	struct TagItem *t, *tp = tags;
	const char *ifn = (const char *)interface_name;
	u32 addr = 0, mask = 0, dst = 0, bcast = 0, a;
	int have_addr = 0, have_mask = 0, have_dst = 0, have_bcast = 0, r;
	char line[160], b1[16], b2[16];
	u32 n;

	if (!valid_ifname(ifn))
		return fail(c, RT_EINVAL);
	rt_lock();
	r = rt_netif_find(ifn) != NULL;
	rt_unlock();
	if (!r)
		return fail(c, RT_ENXIO);
	/* the addresses first, so a state change applies to the new ones */
	while ((t = next_tag(&tp)) != NULL) {
		const char *s = (const char *)t->ti_Data;
		switch (t->ti_Tag) {
		case IFC_Address:
			if (parse_host(s, &addr) < 0)
				return fail(c, RT_EINVAL);
			have_addr = 1;
			break;
		case IFC_NetMask:
			if (!s || !amiga_inet_aton(s, &mask))
				return fail(c, RT_EINVAL);
			have_mask = 1;
			break;
		case IFC_DestinationAddress:
			if (parse_host(s, &dst) < 0)
				return fail(c, RT_EINVAL);
			have_dst = 1;
			break;
		case IFC_BroadcastAddress:
			if (!s || !amiga_inet_aton(s, &bcast))
				return fail(c, RT_EINVAL);
			have_bcast = 1;
			break;
		case IFC_GetPeerAddress:
		case IFC_GetDNS:
			if (t->ti_Data)
				return fail(c, RT_EOPNOTSUPP);    /* needs SANA-II R4 address queries */
			break;
		}
	}
	if (have_addr || have_mask || have_dst || have_bcast) {
		n = (u32)rt_snprintf(line, sizeof(line), "ifconfig %s", ifn);
		if (have_addr)
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " %s", fmt_ip(addr, b1));
		else if (have_dst || have_bcast || have_mask) {
			rt_lock();
			{
				struct rt_netif *nif = rt_netif_find(ifn);
				a = nif ? nif->addr : 0;
			}
			rt_unlock();
			if (!a)
				return fail(c, RT_EINVAL);
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " %s", fmt_ip(a, b1));
		}
		if (have_mask)
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " netmask %s", fmt_ip(mask, b2));
		if (have_dst)
			n += (u32)rt_snprintf(line + n, sizeof(line) - n, " peer %s", fmt_ip(dst, b2));
		if (have_bcast)
			rt_snprintf(line + n, sizeof(line) - n, " broadcast %s", fmt_ip(bcast, b2));
		r = port_cmd(line);
		if (r < 0)
			return fail(c, -r);
	}
	tp = tags;
	while ((t = next_tag(&tp)) != NULL) {
		const char *s = (const char *)t->ti_Data;
		line[0] = 0;
		switch (t->ti_Tag) {
		case IFC_Metric:
			rt_snprintf(line, sizeof(line), "ifconfig %s metric %lu", ifn, (unsigned long)t->ti_Data);
			break;
		case IFC_LimitMTU:
			rt_snprintf(line, sizeof(line), "ifconfig %s mtu %lu", ifn, (unsigned long)t->ti_Data);
			break;
		case IFC_AddAliasAddress:
		case IFC_DeleteAliasAddress:
			if (parse_host(s, &a) < 0)
				return fail(c, RT_EINVAL);
			rt_snprintf(line, sizeof(line), "ifconfig %s %s %s", ifn,
			            t->ti_Tag == IFC_AddAliasAddress ? "alias" : "delete", fmt_ip(a, b1));
			break;
		case IFC_State:
			if (t->ti_Data == SM_Online)
				if_online(ifn, 1);
			if (t->ti_Data == SM_Up || t->ti_Data == SM_Online)
				rt_snprintf(line, sizeof(line), "ifconfig %s up", ifn);
			else if (t->ti_Data == SM_Down || t->ti_Data == SM_Offline)
				rt_snprintf(line, sizeof(line), "ifconfig %s down", ifn);
			else
				return fail(c, RT_EINVAL);
			break;
		case IFC_ReleaseAddress:
			if (t->ti_Data)
				rt_snprintf(line, sizeof(line), "dhcp %s release", ifn);
			break;
		default:
			break;   /* IFC_AssociatedRoute/DNS, SetDebugMode, Complete: nothing to do */
		}
		if (line[0] && (r = port_cmd(line)) < 0)
			return fail(c, -r);
		if (t->ti_Tag == IFC_State && t->ti_Data == SM_Offline)
			if_online(ifn, 0);
	}
	return 0;
}

/* ======================================================================
 * Address allocation (DHCP), answered by a timer in the Riptide process
 * ==================================================================== */

#define AAM_MAGIC 0x52414d31UL     /* our messages: a header before them */
#define AAM_PENDING 8

struct aam_hdr {
	ULONG magic;
	ULONG size;
};

static struct {
	struct AddressAllocationMessage *m;
	u32 ifindex, deadline;
	u8  proto, started;
} pend[AAM_PENDING];
static struct rt_timer aam_timer;

static void aam_reply(int i, LONG result)
{
	struct AddressAllocationMessage *m = pend[i].m;

	pend[i].m = NULL;
	m->aam_Result = result;
	m->aam_RequestedAddress = 0;
	ReplyMsg(&m->aam_Message);
}

static void aam_fill(struct AddressAllocationMessage *m, struct rt_netif *nif, const struct rt_dhcp_info *di,
                     int ll)
{
	u32 dns[8];
	int n, i;

	m->aam_Address = ll ? di->ll_addr : nif->addr;
	m->aam_SubnetMask = ll ? rt_htonl(0xffff0000u) : nif->netmask;
	m->aam_ServerAddress = ll ? 0 : di->server;
	if (m->aam_RouterTable && m->aam_RouterTableSize > 0) {
		memset(m->aam_RouterTable, 0, (ULONG)m->aam_RouterTableSize * 4);
		if (!ll)
			m->aam_RouterTable[0] = di->router;
	}
	if (m->aam_DNSTable && m->aam_DNSTableSize > 0 && !ll) {
		memset(m->aam_DNSTable, 0, (ULONG)m->aam_DNSTableSize * 4);
		n = rt_dns_get_servers(dns, 8);
		for (i = 0; i < n && i < 8 && i < m->aam_DNSTableSize; i++)
			m->aam_DNSTable[i] = dns[i];
	}
	if (m->aam_StaticRouteTable && m->aam_StaticRouteTableSize > 0)
		memset(m->aam_StaticRouteTable, 0, (ULONG)m->aam_StaticRouteTableSize * 4);
	if (m->aam_DomainName && m->aam_DomainNameSize > 0)
		rt_strlcpy((char *)m->aam_DomainName, ll ? "" : rt_domainname, (u32)m->aam_DomainNameSize);
	if (m->aam_HostName && m->aam_HostNameSize > 0)
		rt_strlcpy((char *)m->aam_HostName, "", (u32)m->aam_HostNameSize);
	if (m->aam_NAKMessage && m->aam_NAKMessageSize > 0)
		m->aam_NAKMessage[0] = 0;
	if (m->aam_BOOTPMessage && m->aam_BOOTPMessageSize > 0)
		memset(m->aam_BOOTPMessage, 0, (ULONG)m->aam_BOOTPMessageSize);
	if (m->aam_LeaseExpires) {
		memset(m->aam_LeaseExpires, 0, sizeof(*m->aam_LeaseExpires));
		if (!ll && di->lease_s != 0xffffffffu && rt_wallclock())
			datestamp_of(rt_wallclock() + di->left_s, m->aam_LeaseExpires);
	}
}

/* Lock held: is the request done? Replies and returns 1 if so. */
static int aam_check(int i)
{
	struct rt_netif *nif = rt_netif_by_index(pend[i].ifindex);
	struct rt_dhcp_info di;
	int st;

	if (!nif) {
		aam_reply(i, AAMR_InterfaceNotKnown);
		return 1;
	}
	st = rt_dhcp_get_info(nif, &di);
	if (st == RT_DHCPS_BOUND || st == RT_DHCPS_RENEWING || st == RT_DHCPS_REBINDING) {
		aam_fill(pend[i].m, nif, &di, 0);
		aam_reply(i, AAMR_Success);
		return 1;
	}
	if (pend[i].proto >= AAMP_SLOWAUTO && di.ll_addr) {
		aam_fill(pend[i].m, nif, &di, 1);
		aam_reply(i, AAMR_Success);
		return 1;
	}
	if ((s32)(rt_now_ms() - pend[i].deadline) >= 0) {
		if (pend[i].started && st != RT_DHCPS_NONE)
			rt_dhcp_stop(nif);
		aam_reply(i, AAMR_Timeout);
		return 1;
	}
	return 0;
}

static void aam_tick(void *arg)
{
	int i, more = 0;

	for (i = 0; i < AAM_PENDING; i++)
		if (pend[i].m && !aam_check(i))
			more = 1;
	if (more)
		rt_timer_after(&aam_timer, 250);
}

LONG bsd_CreateAddrAllocMessageA(RTREG(a6) struct Library *base, RTREG(d0) LONG version,
                                 RTREG(d1) LONG protocol, RTREG(a0) STRPTR interface_name,
                                 RTREG(a1) APTR result_ptr, RTREG(a2) APTR tags)
{
	struct TagItem *t, *tp = tags;
	struct AddressAllocationMessage *m;
	struct aam_hdr *h;
	ULONG timeout = AAM_TIMEOUT_MIN, lease = 0, req = 0, size;
	LONG nak = 0, routers = 0, dns = 0, sroutes = 0, hostn = 0, domn = 0, bootp = 0, rec = 0, unicast = 0;
	const char *cid = NULL;
	struct MsgPort *rp = NULL;
	UBYTE *p;
	int found;

	if (!result_ptr)
		return CAAME_Invalid_result_ptr;
	*(APTR *)result_ptr = NULL;
	if (version < AAM_VERSION_MINIMUM || version > AAM_VERSION)
		return CAAME_Invalid_version;
	if (protocol != AAMP_BOOTP && protocol != AAMP_DHCP)
		return CAAME_Invalid_protocol;
	if (!interface_name || !*interface_name || strlen((char *)interface_name) >= 16)
		return CAAME_Invalid_interface_name;
	rt_lock();
	found = rt_netif_find((const char *)interface_name) != NULL;
	rt_unlock();
	if (!found)
		return CAAME_Interface_not_found;
	while ((t = next_tag(&tp)) != NULL) {
		LONG v = (LONG)t->ti_Data;
		switch (t->ti_Tag) {
		case CAAMTA_Timeout:          timeout = (ULONG)v; break;
		case CAAMTA_LeaseTime:        lease = (ULONG)v; break;
		case CAAMTA_RequestedAddress: req = (ULONG)v; break;
		case CAAMTA_ClientIdentifier:
			cid = (const char *)t->ti_Data;
			if (!cid || !*cid)
				return CAAME_Invalid_client_identifier;
			if (strlen(cid) < 2)
				return CAAME_Client_identifier_too_short;
			if (strlen(cid) > 255)
				return CAAME_Client_identifier_too_long;
			break;
		case CAAMTA_NAKMessageSize:       nak = v > 0 ? v : 0; break;
		case CAAMTA_RouterTableSize:      routers = v > 0 ? v : 0; break;
		case CAAMTA_DNSTableSize:         dns = v > 0 ? v : 0; break;
		case CAAMTA_StaticRouteTableSize: sroutes = v > 0 ? v : 0; break;
		case CAAMTA_HostNameSize:         hostn = v > 0 ? v : 0; break;
		case CAAMTA_DomainNameSize:       domn = v > 0 ? v : 0; break;
		case CAAMTA_BOOTPMessageSize:     bootp = v > 0 ? v : 0; break;
		case CAAMTA_RecordLeaseExpiration: rec = v != 0; break;
		case CAAMTA_ReplyPort:            rp = (struct MsgPort *)t->ti_Data; break;
		case CAAMTA_RequestUnicast:       unicast = v != 0; break;
		}
	}
	size = sizeof(*h) + sizeof(*m) + 4 * (ULONG)(routers + dns + sroutes) + (ULONG)(nak + hostn + domn + bootp) +
	       (cid ? strlen(cid) + 1 : 0) + (rec ? sizeof(struct DateStamp) : 0) + 16;
	h = AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
	if (!h)
		return CAAME_Not_enough_memory;
	h->magic = AAM_MAGIC;
	h->size = size;
	m = (struct AddressAllocationMessage *)(h + 1);
	p = (UBYTE *)(m + 1);
	p = (UBYTE *)(((ULONG)p + 3) & ~3UL);
	m->aam_Message.mn_Node.ln_Type = NT_MESSAGE;
	m->aam_Message.mn_ReplyPort = rp;
	m->aam_Message.mn_Length = sizeof(*m);
	m->aam_Version = version;
	m->aam_Protocol = protocol;
	rt_strlcpy(m->aam_InterfaceName, (const char *)interface_name, sizeof(m->aam_InterfaceName));
	m->aam_Timeout = (LONG)(timeout < AAM_TIMEOUT_MIN ? AAM_TIMEOUT_MIN : timeout);
	m->aam_LeaseTime = lease;
	m->aam_RequestedAddress = req;
	m->aam_Unicast = (WORD)unicast;
#define TAKE(field, sizefield, n, unit) \
	if (n) { m->field = (void *)p; m->sizefield = n; p += (ULONG)(n) * (unit); }
	TAKE(aam_RouterTable, aam_RouterTableSize, routers, 4)
	TAKE(aam_DNSTable, aam_DNSTableSize, dns, 4)
	TAKE(aam_StaticRouteTable, aam_StaticRouteTableSize, sroutes, 4)
#undef TAKE
	if (rec) {
		m->aam_LeaseExpires = (struct DateStamp *)p;
		p += sizeof(struct DateStamp);
	}
	if (nak) { m->aam_NAKMessage = (STRPTR)p; m->aam_NAKMessageSize = nak; p += nak; }
	if (hostn) { m->aam_HostName = (STRPTR)p; m->aam_HostNameSize = hostn; p += hostn; }
	if (domn) { m->aam_DomainName = (STRPTR)p; m->aam_DomainNameSize = domn; p += domn; }
	if (bootp) { m->aam_BOOTPMessage = p; m->aam_BOOTPMessageSize = bootp; p += bootp; }
	if (cid) {
		m->aam_ClientIdentifier = (STRPTR)p;
		strcpy((char *)p, cid);
	}
	*(APTR *)result_ptr = m;
	return CAAME_Success;
}

void bsd_DeleteAddrAllocMessage(RTREG(a6) struct Library *base, RTREG(a0) APTR aam)
{
	struct aam_hdr *h;

	if (!aam)
		return;
	h = (struct aam_hdr *)aam - 1;
	if (h->magic != AAM_MAGIC)
		return;                    /* not one of ours */
	h->magic = 0;
	FreeVec(h);
}

void bsd_BeginInterfaceConfig(RTREG(a6) struct Library *base, RTREG(a0) APTR message)
{
	struct AddressAllocationMessage *m = message;
	struct rt_netif *nif;
	struct rt_dhcp_info di;
	char name[17], line[80];
	LONG result = AAMR_Ignored;
	int i, slot = -1, st, start = 0;

	if (!m)
		return;
	if (m->aam_Version < AAM_VERSION_MINIMUM || m->aam_Version > AAM_VERSION) {
		m->aam_Result = AAMR_VersionUnknown;
		ReplyMsg(&m->aam_Message);
		return;
	}
	memcpy(name, m->aam_InterfaceName, 16);
	name[16] = 0;
	rt_lock();
	nif = rt_netif_find(name);
	if (!nif) {
		result = AAMR_InterfaceNotKnown;
	} else if (m->aam_Protocol < AAMP_BOOTP || m->aam_Protocol > AAMP_FASTAUTO) {
		result = AAMR_Ignored;
	} else if (nif->type != RT_IFT_ETHER) {
		result = AAMR_InterfaceWrongType;
	} else {
		for (i = 0; i < AAM_PENDING; i++) {
			if (pend[i].m && pend[i].ifindex == nif->index)
				result = AAMR_Busy;
			if (!pend[i].m && slot < 0)
				slot = i;
		}
		st = rt_dhcp_get_info(nif, &di);
		if (result == AAMR_Busy) {
			;
		} else if (st == RT_DHCPS_NONE && nif->addr) {
			result = AAMR_AddressKnown;
		} else if (slot < 0) {
			result = AAMR_NoMemory;
		} else {
			LONG to = m->aam_Timeout < AAM_TIMEOUT_MIN ? AAM_TIMEOUT_MIN : m->aam_Timeout;
			pend[slot].m = m;
			pend[slot].ifindex = nif->index;
			pend[slot].deadline = rt_now_ms() + (u32)to * 1000u;
			pend[slot].proto = (u8)m->aam_Protocol;
			pend[slot].started = st == RT_DHCPS_NONE;
			start = st == RT_DHCPS_NONE || m->aam_Protocol >= AAMP_SLOWAUTO;
			result = 0;
			if (!aam_timer.fn)
				rt_timer_init(&aam_timer, aam_tick, NULL);
		}
	}
	rt_unlock();
	if (result) {
		m->aam_Result = result;
		ReplyMsg(&m->aam_Message);
		return;
	}
	/* BOOTP is answered by DHCP servers too: Riptide's client asks for both */
	if (start) {
		rt_snprintf(line, sizeof(line), "dhcp %s start%s", name,
		            m->aam_Protocol >= AAMP_SLOWAUTO ? " linklocal on wait 1" : "");
		port_cmd(line);
	}
	rt_lock();
	if (pend[slot].m == m && !aam_check(slot))
		rt_timer_after(&aam_timer, 250);
	rt_unlock();
	rt_kick();
}

void bsd_AbortInterfaceConfig(RTREG(a6) struct Library *base, RTREG(a0) APTR message)
{
	int i;

	rt_lock();
	for (i = 0; i < AAM_PENDING; i++) {
		if (pend[i].m == message) {
			struct rt_netif *nif = rt_netif_by_index(pend[i].ifindex);
			struct rt_dhcp_info di;
			if (nif && pend[i].started && rt_dhcp_get_info(nif, &di) != RT_DHCPS_NONE &&
			    di.state != RT_DHCPS_BOUND)
				rt_dhcp_stop(nif);
			aam_reply(i, AAMR_Aborted);
		}
	}
	rt_unlock();
}

/* ======================================================================
 * Monitoring hooks
 * ==================================================================== */

struct mon_hook {
	struct mon_hook *next;
	struct Hook *hook;
	LONG type;
	struct TaskCtx *owner;
};

static struct mon_hook *mon_list;  /* lock held */
static int mon_calls;              /* hooks being called in callers' context */
volatile ULONG amiga_rs_caller_hooks;

#define CALLER_TYPES ((1 << MHT_Connect) | (1 << MHT_Send) | (1 << MHT_Bind))

static void mon_update(void)
{
	struct mon_hook *h;
	ULONG all = 0;

	for (h = mon_list; h; h = h->next)
		all |= 1UL << h->type;
	amiga_rs_caller_hooks = all & CALLER_TYPES;
	rt_mon_mask = (u8)(all & ~CALLER_TYPES);
}

/* In the stack (lock held): ICMP, UDP, TCP connection requests */
static int stack_hook(int type, const u32 *src, const u32 *dst, const void *hdr)
{
	struct ProtocolMonitorMessage pm;
	struct mon_hook *h;
	LONG r;

	pm.pm_Size = sizeof(pm);
	pm.pm_Src = (APTR)src;
	pm.pm_Dst = (APTR)dst;
	pm.pm_Header = (APTR)hdr;
	for (h = mon_list; h; h = h->next) {
		if (h->type != type)
			continue;
		r = (LONG)amiga_call_hook(h->hook, NULL, &pm);
		if (r != MA_Continue)
			return r;
	}
	return MA_Continue;
}

static int stack_packet_hook(int out, const void *lh, u32 lhlen, const void *data, u32 len)
{
	struct PacketMonitorMessage pm;
	struct mon_hook *h;
	LONG r;

	pm.pmm_Size = sizeof(pm);
	pm.pmm_Direction = out ? PMMD_Send : PMMD_Receive;
	pm.pmm_LinkLayerData = (APTR)lh;
	pm.pmm_LinkLayerSize = (LONG)lhlen;
	pm.pmm_PacketData = (APTR)data;
	pm.pmm_PacketSize = (LONG)len;
	for (h = mon_list; h; h = h->next) {
		if (h->type != MHT_Packet)
			continue;
		r = (LONG)amiga_call_hook(h->hook, NULL, &pm);
		if (r != MA_Continue)
			return r;
	}
	return MA_Continue;
}

/* In the caller's context: the hooks of 'type' with msg; 0 or an errno */
static LONG caller_hooks(struct TaskCtx *c, LONG type, APTR msg)
{
	struct Hook *hooks[16];
	struct mon_hook *h;
	int n = 0, i;
	LONG r = 0;

	rt_lock();
	for (h = mon_list; h && n < 16; h = h->next)
		if (h->type == type)
			hooks[n++] = h->hook;
	mon_calls++;
	rt_unlock();
	c->in_mon_hook++;
	for (i = 0; i < n && r <= 0; i++)
		r = (LONG)amiga_call_hook(hooks[i], NULL, msg);
	c->in_mon_hook--;
	rt_lock();
	mon_calls--;
	rt_unlock();
	return r > 0 ? r : 0;
}

static STRPTR caller_name(struct TaskCtx *c)
{
	if (c->logtag)
		return c->logtag;
	return (STRPTR)(c->task ? c->task->tc_Node.ln_Name : NULL);
}

LONG amiga_rs_mon_connect(struct TaskCtx *c, int bind, LONG s, APTR name, LONG namelen)
{
	struct ConnectMonitorMsg m;

	m.cmm_Size = sizeof(m);          /* struct BindMonitorMsg has the same layout */
	m.cmm_Caller = caller_name(c);
	m.cmm_Socket = s;
	m.cmm_Name = name;
	m.cmm_NameLen = namelen;
	return caller_hooks(c, bind ? MHT_Bind : MHT_Connect, &m);
}

LONG amiga_rs_mon_send(struct TaskCtx *c, LONG s, APTR buf, LONG len, LONG flags, APTR to, LONG tolen,
                       APTR msg)
{
	struct SendMonitorMessage m;

	m.smm_Size = sizeof(m);
	m.smm_Caller = caller_name(c);
	m.smm_Socket = s;
	m.smm_Buffer = buf;
	m.smm_Len = len;
	m.smm_Flags = flags;
	m.smm_To = to;
	m.smm_ToLen = tolen;
	m.smm_Msg = msg;
	return caller_hooks(c, MHT_Send, &m);
}

LONG bsd_AddNetMonitorHookTagList(RTREG(a6) struct Library *base, RTREG(d0) LONG type, RTREG(a0) APTR hook,
                                  RTREG(a1) APTR tags)
{
	CTX;
	struct mon_hook *h;

	if (!hook || !((struct Hook *)hook)->h_Entry)
		return fail(c, RT_EFAULT);
	if (type < MHT_ICMP || type > MHT_Bind)
		return fail(c, RT_EINVAL);
	h = AllocVec(sizeof(*h), MEMF_PUBLIC | MEMF_CLEAR);
	if (!h)
		return fail(c, RT_ENOMEM);
	h->hook = hook;
	h->type = type;
	h->owner = c;
	rt_lock();
	h->next = mon_list;
	mon_list = h;
	mon_update();
	rt_unlock();
	return 0;
}

/* Lock held. Unlink the hooks matching hook (or owner), wait for calls
 * in other tasks to end and free them. */
static void mon_remove(struct TaskCtx *c, struct Hook *hook, struct TaskCtx *owner)
{
	struct mon_hook **pp = &mon_list, *h, *gone = NULL;

	while ((h = *pp) != NULL) {
		if ((hook && h->hook == hook) || (owner && h->owner == owner)) {
			*pp = h->next;
			h->next = gone;
			gone = h;
		} else {
			pp = &h->next;
		}
	}
	mon_update();
	if (!gone)
		return;
	/* a hook removing itself cannot wait for its own call */
	while (mon_calls && !(c && c->in_mon_hook))
		rt_sleeper_wait(rt_sleeper_self(), 20, NULL);
	while (gone) {
		h = gone->next;
		FreeVec(gone);
		gone = h;
	}
}

void bsd_RemoveNetMonitorHook(RTREG(a6) struct Library *base, RTREG(a0) APTR hook)
{
	CTX;

	if (!hook)
		return;
	rt_lock();
	mon_remove(c, hook, NULL);
	rt_unlock();
}

/* ======================================================================
 * Statistics
 * ==================================================================== */

LONG bsd_GetNetworkStatistics(RTREG(a6) struct Library *base, RTREG(d0) LONG type, RTREG(d1) LONG version,
                              RTREG(a0) APTR destination, RTREG(d2) LONG size)
{
	CTX;
	int r;

	if (version != NETWORKSTATUS_VERSION)
		return fail(c, RT_EINVAL);
	if (destination && size < 0)
		return fail(c, RT_EINVAL);
	rt_lock();
	r = rt_bsd_netstat(type, destination, destination ? (u32)size : 0);
	rt_unlock();
	if (r >= 0 && destination && r > size)
		r = size;
	return ret(c, r);
}

/* ======================================================================
 * DNS
 * ==================================================================== */

#define DNS_ADDED 16
static struct { u32 addr; LONG count; } dns_added[DNS_ADDED];  /* lock held */

static int dns_slot(u32 a, int create)
{
	int i, f = -1;

	for (i = 0; i < DNS_ADDED; i++) {
		if (dns_added[i].count && dns_added[i].addr == a)
			return i;
		if (!dns_added[i].count && f < 0)
			f = i;
	}
	if (create && f >= 0) {
		dns_added[f].addr = a;
		dns_added[f].count = 0;
		return f;
	}
	return -1;
}

static int dns_configured(u32 a)
{
	u32 s[16];
	int n = rt_dns_get_servers(s, 16), i;

	for (i = 0; i < n && i < 16; i++)
		if (s[i] == a)
			return 1;
	return 0;
}

LONG bsd_AddDomainNameServer(RTREG(a6) struct Library *base, RTREG(a0) STRPTR address)
{
	CTX;
	u32 a;
	int i, r = 0;

	if (!address)
		return fail(c, RT_EFAULT);
	if (!amiga_inet_aton((const char *)address, &a) || !a)
		return fail(c, RT_EINVAL);
	rt_lock();
	i = dns_slot(a, 1);
	if (i < 0) {
		r = -RT_ENOBUFS;
	} else {
		if (!dns_added[i].count && !dns_configured(a))
			r = rt_dns_add_server(a);
		if (r >= 0) {
			dns_added[i].count++;
			r = 0;
		}
	}
	rt_unlock();
	return ret(c, r);
}

LONG bsd_RemoveDomainNameServer(RTREG(a6) struct Library *base, RTREG(a0) STRPTR address)
{
	CTX;
	u32 a;
	int i, r = 0;

	if (!address)
		return fail(c, RT_EFAULT);
	if (!amiga_inet_aton((const char *)address, &a))
		return fail(c, RT_EINVAL);
	rt_lock();
	i = dns_slot(a, 0);
	if (i >= 0) {
		if (--dns_added[i].count == 0)
			rt_dns_remove_server(a);
	} else if (dns_configured(a)) {
		rt_dns_remove_server(a);
	} else {
		r = -RT_ENOENT;
	}
	rt_unlock();
	return ret(c, r);
}

ULONG bsd_ObtainDomainNameServerList(RTREG(a6) struct Library *base)
{
	CTX;
	struct List *l;
	struct DomainNameServerNode *nd;
	char *str;
	u32 s[16];
	int n, i, k;

	rt_lock();
	n = rt_dns_get_servers(s, 16);
	if (n > 16)
		n = 16;
	l = AllocVec(sizeof(*l) + (ULONG)n * (sizeof(*nd) + 16), MEMF_PUBLIC | MEMF_CLEAR);
	if (l) {
		NewList(l);
		nd = (struct DomainNameServerNode *)(l + 1);
		str = (char *)(nd + n);
		for (i = 0; i < n; i++) {
			nd[i].dnsn_Size = sizeof(nd[i]);
			nd[i].dnsn_Address = (STRPTR)(str + i * 16);
			rt_format_ipv4(s[i], str + i * 16);
			k = dns_slot(s[i], 0);
			nd[i].dnsn_UseCount = k >= 0 ? dns_added[k].count : -1;
			AddTail(l, (struct Node *)&nd[i]);
		}
	}
	rt_unlock();
	if (!l)
		fail(c, RT_ENOMEM);
	return (ULONG)l;
}

void bsd_ReleaseDomainNameServerList(RTREG(a6) struct Library *base, RTREG(a0) APTR list)
{
	if (list)
		FreeVec(list);
}

LONG bsd_GetDefaultDomainName(RTREG(a6) struct Library *base, RTREG(a0) STRPTR buffer, RTREG(d0) LONG buffer_size)
{
	LONG r;

	rt_lock();
	r = rt_domainname[0] != 0;
	if (buffer && buffer_size > 0)
		rt_strlcpy((char *)buffer, rt_domainname, (u32)buffer_size);
	rt_unlock();
	return r;
}

void bsd_SetDefaultDomainName(RTREG(a6) struct Library *base, RTREG(a0) STRPTR buffer)
{
	const char *s = buffer ? (const char *)buffer : "";

	if (strlen(s) >= sizeof(rt_domainname))
		return;
	rt_lock();
	rt_strlcpy(rt_domainname, s, sizeof(rt_domainname));
	rt_dns_set_search(s);
	rt_unlock();
}

/* ======================================================================
 * Network database enumeration
 * ==================================================================== */

void bsd_setnetent(RTREG(a6) struct Library *base, RTREG(d0) LONG stay_open)
{
	base_ctx(base)->netidx = 0;
}

void bsd_endnetent(RTREG(a6) struct Library *base)
{
	base_ctx(base)->netidx = 0;
}

ULONG bsd_getnetent(RTREG(a6) struct Library *base)
{
	CTX;
	const struct rt_netent_info *ni = rt_netent_at((unsigned)c->netidx);

	if (ni)
		c->netidx++;
	return bsd_make_netent(c, ni);
}

void bsd_setprotoent(RTREG(a6) struct Library *base, RTREG(d0) LONG stay_open)
{
	base_ctx(base)->protoidx = 0;
}

void bsd_endprotoent(RTREG(a6) struct Library *base)
{
	base_ctx(base)->protoidx = 0;
}

ULONG bsd_getprotoent(RTREG(a6) struct Library *base)
{
	CTX;
	const struct rt_protoent_info *pi = rt_protoent_at((unsigned)c->protoidx);

	if (pi)
		c->protoidx++;
	return bsd_make_protoent(c, pi);
}

void bsd_setservent(RTREG(a6) struct Library *base, RTREG(d0) LONG stay_open)
{
	base_ctx(base)->servidx = 0;
}

void bsd_endservent(RTREG(a6) struct Library *base)
{
	base_ctx(base)->servidx = 0;
}

ULONG bsd_getservent(RTREG(a6) struct Library *base)
{
	CTX;
	const struct rt_servent_info *si = rt_servent_at((unsigned)c->servidx);

	if (si)
		c->servidx++;
	return bsd_make_servent(c, si);
}

/* ======================================================================
 * Address conversion
 * ==================================================================== */

LONG bsd_inet_aton(RTREG(a6) struct Library *base, RTREG(a0) STRPTR cp, RTREG(a1) APTR addr)
{
	u32 a;

	if (!cp || !amiga_inet_aton((const char *)cp, &a))
		return 0;
	if (addr)
		memcpy(addr, &a, 4);
	return 1;
}

ULONG bsd_inet_ntop(RTREG(a6) struct Library *base, RTREG(d0) LONG af, RTREG(a0) APTR src, RTREG(a1) STRPTR dst,
                    RTREG(d1) LONG size)
{
	return riptide_inet_ntop(base, af, src, dst, size);
}

LONG bsd_inet_pton(RTREG(a6) struct Library *base, RTREG(d0) LONG af, RTREG(a0) STRPTR src, RTREG(a1) APTR dst)
{
	return riptide_inet_pton(base, af, src, dst);
}

LONG bsd_In_LocalAddr(RTREG(a6) struct Library *base, RTREG(d0) ULONG address)
{
	struct rt_netif *n;
	LONG r = 0;

	rt_lock();
	for (n = rt_netif_list(); n && !r; n = n->next)
		if (rt_netif_has_addr(n, address) || rt_netif_on_link(n, address))
			r = 1;
	rt_unlock();
	return r;
}

LONG bsd_In_CanForward(RTREG(a6) struct Library *base, RTREG(d0) ULONG address)
{
	u32 h = rt_ntohl(address), net = h >> 24;

	if ((h & 0xe0000000u) == 0xe0000000u)     /* multicast, experimental */
		return 0;
	return net != 0 && net != 127;
}

/* ======================================================================
 * Kernel memory (mbuf_*) and IP filter (ipf_*) APIs: not provided.
 * SBTC_HAVE_KERNEL_MEMORY_API answers FALSE, SBTC_IPF_API_VERSION 0.
 * ==================================================================== */

ULONG bsd_mbuf_copym(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(d0) LONG off, RTREG(d1) LONG len)
{
	return 0;
}

LONG bsd_mbuf_copyback(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(d0) LONG off, RTREG(d1) LONG len,
                       RTREG(a1) APTR cp)
{
	return fail(base_ctx(base), RT_ENOSYS);
}

LONG bsd_mbuf_copydata(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(d0) LONG off, RTREG(d1) LONG len,
                       RTREG(a1) APTR cp)
{
	return fail(base_ctx(base), RT_ENOSYS);
}

ULONG bsd_mbuf_free(RTREG(a6) struct Library *base, RTREG(a0) APTR m)
{
	return 0;
}

void bsd_mbuf_freem(RTREG(a6) struct Library *base, RTREG(a0) APTR m)
{
}

ULONG bsd_mbuf_get(RTREG(a6) struct Library *base)
{
	return 0;
}

ULONG bsd_mbuf_gethdr(RTREG(a6) struct Library *base)
{
	return 0;
}

ULONG bsd_mbuf_prepend(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(d0) LONG len)
{
	return 0;
}

LONG bsd_mbuf_cat(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(a1) APTR n)
{
	return fail(base_ctx(base), RT_ENOSYS);
}

LONG bsd_mbuf_adj(RTREG(a6) struct Library *base, RTREG(a0) APTR mp, RTREG(d0) LONG req_len)
{
	return fail(base_ctx(base), RT_ENOSYS);
}

ULONG bsd_mbuf_pullup(RTREG(a6) struct Library *base, RTREG(a0) APTR m, RTREG(d0) LONG len)
{
	return 0;
}

LONG bsd_ipf_open(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_close(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_ioctl(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(d1) ULONG command,
                   RTREG(a0) APTR buffer)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_log_read(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(a0) APTR buffer,
                      RTREG(d1) LONG len)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_log_data_waiting(RTREG(a6) struct Library *base, RTREG(d0) LONG channel)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_set_notify_mask(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(d1) ULONG mask)
{
	return fail(base_ctx(base), RT_ENXIO);
}

LONG bsd_ipf_set_interrupt_mask(RTREG(a6) struct Library *base, RTREG(d0) LONG channel, RTREG(d1) ULONG mask)
{
	return fail(base_ctx(base), RT_ENXIO);
}

/* ======================================================================
 * Internet superserver: Riptide has none, so no program is a server.
 * ==================================================================== */

LONG bsd_ProcessIsServer(RTREG(a6) struct Library *base, RTREG(a0) APTR pr)
{
	return 0;
}

LONG bsd_ObtainServerSocket(RTREG(a6) struct Library *base)
{
	return -1;
}

/* ======================================================================
 * Configuration data (ObtainRoadshowData() & co.)
 * ==================================================================== */

#define RSD_MAGIC 0x52534431UL
#define RSD_LISTS 8

static const struct {
	const char *name;
	const char *sysctl;          /* NULL: special */
	UWORD flags;
} rsd_items[] = {
	{ "bpf.bufsize", NULL, 0 },
	{ "icmp.maskrepl", "net.inet.icmp.maskrepl", 0 },
	{ "icmp.processecho", "net.inet.icmp.process_echo", 0 },
	{ "icmp.procesststamp", "net.inet.icmp.process_tstamp", 0 },
	{ "ip.defttl", "net.inet.ip.ttl", 0 },
	{ "ip.forwarding", "net.inet.ip.forwarding", 0 },
	{ "ip.sendredirects", "net.inet.ip.redirect", 0 },
	{ "task.controller.priority", NULL, 0 },
	{ "tcp.do_rfc1323", "net.inet.tcp.rfc1323", 0 },
	{ "tcp.do_timestamps", "net.inet.tcp.timestamps", 0 },
	{ "tcp.do_win_scale", "net.inet.tcp.window_scaling", 0 },
	{ "tcp.mssdflt", "net.inet.tcp.mssdflt", 0 },
	{ "tcp.recvspace", "net.inet.tcp.recvspace", 0 },
	{ "tcp.sendspace", "net.inet.tcp.sendspace", 0 },
	{ "udp.cksum", "net.inet.udp.checksum", 0 },
	{ "udp.recvspace", "net.inet.udp.recvspace", 0 },
	{ "udp.sendspace", "net.inet.udp.maxdgram", 0 },
};
#define RSD_N ((int)RT_ARRAY_SIZE(rsd_items))

struct rsd_list {
	struct List list;            /* what the caller sees */
	ULONG magic;
	LONG  write;
	struct TaskCtx *owner;
	struct RoadshowDataNode node[RSD_N];
	LONG  value[RSD_N];
};

static struct rsd_list *rsd_issued[RSD_LISTS];   /* under Forbid() */
static struct rsd_list *rsd_writer;

/* Lock held: get (newv NULL) or set item i. 0 or -errno. */
static int rsd_access(int i, s32 *val, const s32 *newv)
{
	u32 len = 4;

	if (rsd_items[i].sysctl)
		return rt_sysctl_byname(rsd_items[i].sysctl, newv ? NULL : val, newv ? NULL : &len,
		                        newv, newv ? 4 : 0);
	if (!strcmp(rsd_items[i].name, "bpf.bufsize")) {
		if (newv) {
			if (*newv < RT_BPF_MINBUFSIZE || *newv > RT_BPF_MAXBUFSIZE)
				return -RT_EINVAL;
			rt_bpf_bufsize = (u32)*newv;
		} else {
			*val = (s32)rt_bpf_bufsize;
		}
		return 0;
	}
	/* task.controller.priority */
	if (!amiga_stack_task)
		return -RT_ENOENT;
	if (newv) {
		if (*newv < -128 || *newv > 127)
			return -RT_EINVAL;
		SetTaskPri(amiga_stack_task, *newv);
	} else {
		*val = amiga_stack_task->tc_Node.ln_Pri;
	}
	return 0;
}

static int rsd_find(struct rsd_list *l)
{
	int i;

	for (i = 0; i < RSD_LISTS; i++)
		if (l && rsd_issued[i] == l)
			return i;
	return -1;
}

ULONG bsd_ObtainRoadshowData(RTREG(a6) struct Library *base, RTREG(d0) LONG access)
{
	CTX;
	struct rsd_list *l;
	int i, slot = -1;

	if (access != ORD_ReadAccess && access != ORD_WriteAccess) {
		fail(c, RT_EINVAL);
		return 0;
	}
	l = AllocVec(sizeof(*l), MEMF_PUBLIC | MEMF_CLEAR);
	if (!l) {
		fail(c, RT_ENOMEM);
		return 0;
	}
	Forbid();
	for (i = 0; i < RSD_LISTS; i++)
		if (!rsd_issued[i] && slot < 0)
			slot = i;
	if (slot < 0 || (access == ORD_WriteAccess && rsd_writer)) {
		Permit();
		FreeVec(l);
		fail(c, slot < 0 ? RT_ENOMEM : RT_EBUSY);
		return 0;
	}
	rsd_issued[slot] = l;
	if (access == ORD_WriteAccess)
		rsd_writer = l;
	Permit();
	l->magic = RSD_MAGIC;
	l->write = access == ORD_WriteAccess;
	l->owner = c;
	NewList(&l->list);
	rt_lock();
	for (i = 0; i < RSD_N; i++) {
		s32 v = 0;
		rsd_access(i, &v, NULL);
		l->value[i] = v;
		l->node[i].rdn_Name = (STRPTR)rsd_items[i].name;
		l->node[i].rdn_Flags = rsd_items[i].flags;
		l->node[i].rdn_Type = RDNT_Integer;
		l->node[i].rdn_Length = 4;
		l->node[i].rdn_Data = &l->value[i];
		AddTail(&l->list, (struct Node *)&l->node[i]);
	}
	rt_unlock();
	return (ULONG)&l->list;
}

void bsd_ReleaseRoadshowData(RTREG(a6) struct Library *base, RTREG(a0) APTR list)
{
	struct rsd_list *l = list;
	int i;

	Forbid();
	i = rsd_find(l);
	if (i >= 0) {
		rsd_issued[i] = NULL;
		if (rsd_writer == l)
			rsd_writer = NULL;
	}
	Permit();
	if (i >= 0) {
		l->magic = 0;
		FreeVec(l);
	}
}

LONG bsd_ChangeRoadshowData(RTREG(a6) struct Library *base, RTREG(a0) APTR list, RTREG(a1) STRPTR name,
                            RTREG(d0) ULONG length, RTREG(a2) APTR data)
{
	CTX;
	struct rsd_list *l = list;
	s32 v;
	int i, r;

	Forbid();
	i = rsd_find(l);
	Permit();
	if (i < 0) {
		fail(c, RT_EINVAL);
		return 0;
	}
	if (!l->write) {
		fail(c, RT_EACCES);
		return 0;
	}
	for (i = 0; i < RSD_N; i++)
		if (name && !rt_strcasecmp((const char *)name, rsd_items[i].name))
			break;
	if (i == RSD_N) {
		fail(c, RT_ENOENT);
		return 0;
	}
	if (rsd_items[i].flags & RDNF_ReadOnly) {
		fail(c, RT_EPERM);
		return 0;
	}
	if (length != 4 || !data) {
		fail(c, RT_ENOSPC);
		return 0;
	}
	memcpy(&v, data, 4);
	rt_lock();
	r = rsd_access(i, NULL, &v);
	if (r == 0)
		l->value[i] = v;
	rt_unlock();
	if (r < 0) {
		fail(c, r == -RT_EINVAL ? RT_ENOSPC : -r);
		return 0;
	}
	return 1;
}

/* ======================================================================
 * Reentrant resolver calls
 * ==================================================================== */

/* hostent in the caller's buffer: aliases[1], addr_list[n + 1], the
 * addresses, the name. NULL (ERANGE) if it does not fit. */
static ULONG fill_hostent(struct TaskCtx *c, struct hostent *hp, UBYTE *buf, ULONG buflen, const char *name,
                          const u32 *addrs, int n, LONG *he)
{
	ULONG pad = (4 - ((ULONG)buf & 3)) & 3, need = pad + 4 * (ULONG)(1 + n + 1) + 4 * (ULONG)n + strlen(name) + 1;
	char **al, **pl;
	u32 *ad;
	int i;

	if (!hp || !buf || buflen < need) {
		if (he)
			*he = NETDB_INTERNAL;
		fail(c, RT_ERANGE);
		return 0;
	}
	al = (char **)(buf + pad);
	pl = al + 1;
	ad = (u32 *)(pl + n + 1);
	al[0] = NULL;
	for (i = 0; i < n; i++) {
		ad[i] = addrs[i];
		pl[i] = (char *)&ad[i];
	}
	pl[n] = NULL;
	strcpy((char *)(ad + n), name);
	hp->h_name = (char *)(ad + n);
	hp->h_aliases = al;
	hp->h_addrtype = RT_AF_INET;
	hp->h_length = 4;
	hp->h_addr_list = pl;
	if (he)
		*he = 0;
	return (ULONG)hp;
}

static LONG herr(int r)
{
	switch (-r) {
	case RT_HOST_NOT_FOUND: return HOST_NOT_FOUND;
	case RT_TRY_AGAIN: return TRY_AGAIN;
	case RT_NO_DATA: return NO_DATA;
	}
	return r == 0 ? NO_DATA : NO_RECOVERY;
}

ULONG bsd_gethostbyname_r(RTREG(a6) struct Library *base, RTREG(a0) STRPTR name, RTREG(a1) APTR hp,
                          RTREG(a2) APTR buf, RTREG(d0) ULONG buflen, RTREG(a3) APTR he)
{
	CTX;
	u32 addrs[8];
	char cname[256];
	int r;

	if (!name || !*name) {
		if (he)
			*(LONG *)he = HOST_NOT_FOUND;
		return 0;
	}
	cname[0] = 0;
	r = rt_resolve_name((const char *)name, addrs, 8, cname, sizeof(cname));
	if (r <= 0) {
		if (he)
			*(LONG *)he = herr(r);
		return 0;
	}
	return fill_hostent(c, hp, buf, buflen, cname[0] ? cname : (const char *)name, addrs, r, he);
}

ULONG bsd_gethostbyaddr_r(RTREG(a6) struct Library *base, RTREG(a0) STRPTR addr, RTREG(d0) LONG len,
                          RTREG(d1) LONG type, RTREG(a1) APTR hp, RTREG(a2) APTR buf, RTREG(d2) ULONG buflen,
                          RTREG(a3) APTR he)
{
	CTX;
	char name[256];
	u32 a;
	int r;

	if (!addr || type != RT_AF_INET || len != 4) {
		if (he)
			*(LONG *)he = HOST_NOT_FOUND;
		return 0;
	}
	memcpy(&a, addr, 4);
	r = rt_resolve_addr(a, name, sizeof(name));
	if (r < 0) {
		if (he)
			*(LONG *)he = herr(r);
		return 0;
	}
	return fill_hostent(c, hp, buf, buflen, name, &a, 1, he);
}

/* ======================================================================
 * getaddrinfo() & co., Roadshow flavour: struct addrinfo with ai_addr
 * before ai_canonname, negative EAI_* codes, its own AI_/NI_ flags.
 * riptide.library's implementation does the work.
 * ==================================================================== */

static LONG eai_to_rs(LONG e)
{
	static const signed char map[] = { 0, RS_EAI_ADDRFAMILY, RS_EAI_AGAIN, RS_EAI_BADFLAGS, RS_EAI_FAIL,
	                                   RS_EAI_FAMILY, RS_EAI_MEMORY, RS_EAI_NODATA, RS_EAI_NONAME,
	                                   RS_EAI_SERVICE, RS_EAI_SOCKTYPE, RS_EAI_SYSTEM };
	return e >= 0 && e < (LONG)sizeof(map) ? map[e] : RS_EAI_FAIL;
}

void bsd_freeaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) APTR ai)
{
	struct rs_addrinfo *a = ai, *next;

	while (a) {
		next = a->ai_next;
		FreeVec(a);
		a = next;
	}
}

LONG bsd_getaddrinfo(RTREG(a6) struct Library *base, RTREG(a0) STRPTR hostname, RTREG(a1) STRPTR servname,
                     RTREG(a2) APTR hints, RTREG(a3) APTR res)
{
	const struct rs_addrinfo *h = hints;
	struct addrinfo mh, *head = NULL, *a;
	LONG e, f = 0;

	if (!res)
		return RS_EAI_FAIL;
	*(APTR *)res = NULL;
	memset(&mh, 0, sizeof(mh));
	if (h) {
		if (h->ai_addrlen || h->ai_addr || h->ai_canonname || h->ai_next)
			return RS_EAI_BADHINTS;
		if (h->ai_flags & ~(RS_AI_PASSIVE | RS_AI_CANONNAME | RS_AI_NUMERICHOST | RS_AI_NUMERICSERV | RS_AI_EXT))
			return RS_EAI_BADFLAGS;
		if (h->ai_flags & RS_AI_PASSIVE) f |= AI_PASSIVE;
		if (h->ai_flags & RS_AI_CANONNAME) f |= AI_CANONNAME;
		if (h->ai_flags & RS_AI_NUMERICHOST) f |= AI_NUMERICHOST;
		if (h->ai_flags & RS_AI_NUMERICSERV) f |= AI_NUMERICSERV;
		mh.ai_flags = f;
		mh.ai_family = h->ai_family;
		mh.ai_socktype = h->ai_socktype;
		mh.ai_protocol = h->ai_protocol;
		if (h->ai_socktype == RT_SOCK_STREAM && h->ai_protocol && h->ai_protocol != 6)
			return RS_EAI_BADHINTS;
		if (h->ai_socktype == RT_SOCK_DGRAM && h->ai_protocol && h->ai_protocol != 17)
			return RS_EAI_BADHINTS;
	}
	e = riptide_getaddrinfo(base, hostname, servname, h ? &mh : NULL, (APTR *)&head);
	if (e)
		return eai_to_rs(e);
	/* same size and next pointer: swap ai_canonname and ai_addr in place */
	for (a = head; a; a = a->ai_next) {
		char *cn = a->ai_canonname;
		struct rs_addrinfo *r = (struct rs_addrinfo *)a;
		APTR sa = a->ai_addr;
		r->ai_addr = sa;
		r->ai_canonname = cn;
		r->ai_flags = h ? h->ai_flags : 0;
	}
	*(APTR *)res = head;
	return 0;
}

ULONG bsd_gai_strerror(RTREG(a6) struct Library *base, RTREG(a0) LONG errnum)
{
	static const char *const s[] = {
		"Success", "Invalid value for ai_flags", "hostname nor servname provided, or not known",
		"Temporary failure in name resolution", "Non-recoverable failure in name resolution",
		"No address associated with hostname", "ai_family not supported", "ai_socktype not supported",
		"servname not supported for ai_socktype", "Address family for hostname not supported",
		"Memory allocation failure", "System error returned in errno", "Invalid value for hints",
		"Resolved protocol is unknown"
	};
	if (errnum <= 0 && errnum >= -13)
		return (ULONG)s[-errnum];
	return (ULONG)"Unknown error";
}

LONG bsd_getnameinfo(RTREG(a6) struct Library *base, RTREG(a0) APTR sa, RTREG(d0) ULONG salen, RTREG(a1) STRPTR host,
                     RTREG(d1) ULONG hostlen, RTREG(a2) STRPTR serv, RTREG(d2) ULONG servlen, RTREG(d3) ULONG flags)
{
	LONG f = 0, e;

	if (flags & ~(ULONG)(RS_NI_NUMERICHOST | RS_NI_NUMERICSERV | RS_NI_NOFQDN | RS_NI_NAMEREQD | RS_NI_DGRAM |
	                     RS_NI_WITHSCOPEID))
		return RS_EAI_BADFLAGS;
	if (flags & RS_NI_NUMERICHOST) f |= NI_NUMERICHOST;
	if (flags & RS_NI_NUMERICSERV) f |= NI_NUMERICSERV;
	if (flags & RS_NI_NOFQDN) f |= NI_NOFQDN;
	if (flags & RS_NI_NAMEREQD) f |= NI_NAMEREQD;
	if (flags & RS_NI_DGRAM) f |= NI_DGRAM;
	e = riptide_getnameinfo(base, sa, (LONG)salen, host, (LONG)hostlen, serv, (LONG)servlen, f);
	return e ? eai_to_rs(e) : 0;
}

/* ======================================================================
 * SocketBaseTagList() codes, error and log hooks
 * ==================================================================== */

static struct Hook *log_hook;
static char log_file_name[] = "";

static int sysctl_int(const char *name, int set, ULONG *val)
{
	s32 v = (s32)*val;
	u32 len = 4;
	int r;

	rt_lock();
	r = set ? rt_sysctl_byname(name, NULL, NULL, &v, 4) : rt_sysctl_byname(name, &v, &len, NULL, 0);
	rt_unlock();
	if (!set && r == 0)
		*val = (ULONG)v;
	return r;
}

static ULONG system_status(void)
{
	struct rt_netif *n;
	struct rt_route *rt;
	u32 dns[1];
	ULONG s = 0;

	rt_lock();
	for (n = rt_netif_list(); n; n = n->next) {
		if (n->type == RT_IFT_LOOP || !n->addr || (n->flags & (RT_IFF_UP | RT_IFF_RUNNING)) !=
		                                         (RT_IFF_UP | RT_IFF_RUNNING))
			continue;
		s |= SBSYSSTAT_Interfaces;
		s |= n->type == RT_IFT_PTP ? SBSYSSTAT_PTP_Interfaces : SBSYSSTAT_BCast_Interfaces;
	}
	if (rt_dns_get_servers(dns, 1) > 0)
		s |= SBSYSSTAT_Resolver;
	for (rt = rt_route_list(); rt; rt = rt->next) {
		if ((rt->flags & RT_RTF_PMTU) || !rt->netif || rt->netif->type == RT_IFT_LOOP)
			continue;
		s |= SBSYSSTAT_Routes;
		if (rt->mask == 0)
			s |= SBSYSSTAT_DefaultRoute;
	}
	rt_unlock();
	return s;
}

static void bytes_total(int out, SBQUAD_T *q)
{
	struct rt_netif *n;
	ULONG lo = 0, hi = 0, v;

	rt_lock();
	for (n = rt_netif_list(); n; n = n->next) {
		if (n->type == RT_IFT_LOOP)
			continue;
		v = out ? n->stats.obytes : n->stats.ibytes;
		lo += v;
		if (lo < v)
			hi++;
	}
	rt_unlock();
	q->sbq_High = hi;
	q->sbq_Low = lo;
}

int amiga_rs_sbt(struct TaskCtx *c, ULONG code, int set, ULONG *where)
{
	ULONG val = set ? *where : 0;
	const char *sc = NULL;

	switch (code) {
	case SBTC_COMPAT43:                      /* 4.4BSD socket addresses only */
	case SBTC_IDN_DEFAULT_CHARACTER_SET:     /* no translation: IDNCS_ASCII */
		if (set)
			return val == 0 ? 0 : -1;
		val = 0;
		break;
	case SBTC_NUM_PACKET_FILTER_CHANNELS:
		if (set)
			return -1;
		val = RT_BPF_NCHAN;
		break;
	case SBTC_HAVE_ROUTING_API:
	case SBTC_HAVE_INTERFACE_API:
	case SBTC_HAVE_MONITORING_API:
	case SBTC_HAVE_STATUS_API:
	case SBTC_HAVE_DNS_API:
	case SBTC_HAVE_LOCAL_DATABASE_API:
	case SBTC_HAVE_ADDRESS_CONVERSION_API:
	case SBTC_HAVE_SERVER_API:
	case SBTC_HAVE_ROADSHOWDATA_API:
	case SBTC_HAVE_GETHOSTADDR_R_API:
		if (set)
			return -1;
		val = TRUE;
		break;
	case SBTC_HAVE_KERNEL_MEMORY_API:
	case SBTC_IPF_API_VERSION:
		if (set)
			return -1;
		val = 0;
		break;
	case SBTC_UDP_CHECKSUM:        sc = "net.inet.udp.checksum"; break;
	case SBTC_IP_FORWARDING:       sc = "net.inet.ip.forwarding"; break;
	case SBTC_IP_DEFAULT_TTL:      sc = "net.inet.ip.ttl"; break;
	case SBTC_ICMP_MASK_REPLY:     sc = "net.inet.icmp.maskrepl"; break;
	case SBTC_ICMP_SEND_REDIRECTS: sc = "net.inet.ip.redirect"; break;
	case SBTC_ICMP_PROCESS_ECHO:   sc = "net.inet.icmp.process_echo"; break;
	case SBTC_ICMP_PROCESS_TSTAMP: sc = "net.inet.icmp.process_tstamp"; break;
	case SBTC_CAN_SHARE_LIBRARY_BASES:
		if (set) c->share_bases = val != 0; else val = (ULONG)c->share_bases;
		break;
	case SBTC_LOG_FILE_NAME:       /* Riptide logs to its own output */
		if (set)
			return -1;
		val = (ULONG)log_file_name;
		break;
	case SBTC_LOG_HOOK:
		if (set) log_hook = (struct Hook *)val; else val = (ULONG)log_hook;
		break;
	case SBTC_SYSTEM_STATUS:
		if (set)
			return -1;
		val = system_status();
		break;
	case SBTC_SIG_ADDRESS_CHANGE_MASK:
		if (set) c->addrchg_mask = val; else val = c->addrchg_mask;
		break;
	case SBTC_IP_FILTER_HOOK:      /* no IP filter: only "none" */
		if (set)
			return val == 0 ? 0 : -1;
		val = 0;
		break;
	case SBTC_GET_BYTES_RECEIVED:
	case SBTC_GET_BYTES_SENT:
		/* by reference only: where points at an SBQUAD_T */
		if (set || where == NULL)
			return -1;
		bytes_total(code == SBTC_GET_BYTES_SENT, (SBQUAD_T *)where);
		return 0;
	case SBTC_ERROR_HOOK:
		if (set) c->errhook = (APTR)val; else val = (ULONG)c->errhook;
		break;
	default:
		return 1;
	}
	if (sc) {
		if (sysctl_int(sc, set, &val) < 0)
			return -1;
	}
	if (!set)
		*where = val;
	return 0;
}

void amiga_rs_error_hook(struct TaskCtx *c, ULONG action, LONG code)
{
	struct ErrorHookMsg m;
	APTR h = c->errhook;

	if (!h)
		return;
	m.ehm_Size = sizeof(m);
	m.ehm_Action = action;
	m.ehm_Code = code;
	c->errhook = NULL;               /* a hook calling the library must not recurse */
	amiga_call_hook(h, NULL, &m);
	c->errhook = h;
}

int amiga_rs_log(struct TaskCtx *c, ULONG pri, const char *line)
{
	struct LogHookMessage m;
	struct Hook *h = log_hook;

	if (!h)
		return 0;
	m.lhm_Size = sizeof(m);
	m.lhm_Priority = (LONG)(pri & LOG_PRIMASK);
	DateStamp(&m.lhm_Date);
	m.lhm_Tag = c->logtag;
	m.lhm_ID = 0;
	m.lhm_Message = (STRPTR)line;
	amiga_call_hook(h, NULL, &m);
	return 1;
}

/* ======================================================================
 * Setup and cleanup
 * ==================================================================== */

static void addr_changed(struct rt_netif *nif)
{
	amiga_ctx_signal_addrchg();
}

void amiga_rs_init(void)
{
	rt_bpf_signal = bpf_signal;
	rt_mon_hook = stack_hook;
	rt_mon_packet_hook = stack_packet_hook;
	rt_netif_addr_hook = addr_changed;
}

void amiga_rs_ctx_gone(struct TaskCtx *c)
{
	int i;

	rt_lock();
	rt_bpf_close_owner(c);
	mon_remove(NULL, NULL, c);
	rt_unlock();
	/* lists the program did not release */
	for (i = 0; i < RSD_LISTS; i++) {
		struct rsd_list *l = NULL;
		Forbid();
		if (rsd_issued[i] && rsd_issued[i]->owner == c) {
			l = rsd_issued[i];
			rsd_issued[i] = NULL;
			if (rsd_writer == l)
				rsd_writer = NULL;
		}
		Permit();
		if (l)
			FreeVec(l);
	}
}
