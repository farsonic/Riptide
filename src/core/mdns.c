/*
 * Riptide - Multicast DNS responder (RFC 6762) for <hostname>.local.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Answers A, AAAA and the reverse PTR names of our addresses on every
 * multicast capable interface, over IPv4 (224.0.0.251) and IPv6 (ff02::fb)
 * port 5353, from one dual-stack kernel socket. The name is probed (three
 * queries 250 ms apart) and announced (twice, one second apart) at start,
 * when an interface or address appears and when the host name changes; a
 * conflict renames us to name-2.local, name-3.local... Replies go out per
 * interface with that interface's addresses: multicast, unicast to port
 * 5353 for QU questions, or as legacy unicast (RFC 6762 6.7) to queriers
 * on other ports, which is what our own resolver does for .local names
 * (resolv.c). No DNS-SD, no NSEC.
 */
#include "sock_internal.h"
#include "infra.h"
#include "resolv_internal.h"

#define MDNS_PORT     5353
#define MDNS_TTL      120              /* RFC 6762 10: records with host names */
#define MDNS_IFS      4
#define MDNS_V4       4                /* IPv4 addresses per interface */
#define MDNS_V6       4                /* IPv6 addresses per interface */
#define MDNS_NREC     (2 * MDNS_V4 + 2 * MDNS_V6)
/* record numbers k: A, AAAA, PTR (v4), PTR (v6) of each address */
#define K_A           0
#define K_AAAA        MDNS_V4
#define K_P4          (MDNS_V4 + MDNS_V6)
#define K_P6          (2 * MDNS_V4 + MDNS_V6)
#define TYPE_ANY      255
#define CLASS_FLUSH   0x8000           /* cache flush (answers) / QU (questions) */

enum { MS_OFF, MS_WAIT, MS_PROBE, MS_ANNOUNCE, MS_RUN };

static struct {
	u8  enabled;
	u8  state;
	u8  count;                /* probes / announcements sent */
	u8  conflicts;
	char base[64];            /* rt_hostname's first label */
	char name[64];            /* the label we claim */
	char fqdn[72];            /* name + ".local" */
	u32 sig;                  /* interfaces and addresses last announced */
	u32 answered, conflicts_total;
} md;

/* an interface we listen on, and the records it answers with */
static struct mdns_if {
	struct rt_netif *nif;
	u8  v4, v6;               /* groups joined */
	u32 last_mc;              /* rt_now_ms() of the last multicast answer */
	/* records: bits K_A.. A, K_AAAA.. AAAA, K_P4.. PTR (v4), K_P6.. PTR (v6) */
	int n4;
	u32 a4[MDNS_V4];
	int n6;
	rt_in6_addr a6[MDNS_V6];
} mif[MDNS_IFS];

#define REC_A     (((1u << MDNS_V4) - 1) << K_A)
#define REC_AAAA  (((1u << MDNS_V6) - 1) << K_AAAA)
#define REC_PTR4  (((1u << MDNS_V4) - 1) << K_P4)
#define REC_PTR6  (((1u << MDNS_V6) - 1) << K_P6)

static struct rt_socket *mdns_so;
static struct sock_upcall mdns_upcall;
static struct rt_timer mdns_timer, mdns_rx_timer;
static rt_in6_addr mdns_grp6;
static u8 mdns_buf[1024];

static u32 mdns_grp4(void)
{
	return rt_htonl(0xe00000fbu);
}

/* ---- names ---- */

static void rev_name(const struct mdns_if *f, int k, char *out, u32 len)
{
	if (k < K_P6) {
		u32 h = rt_ntohl(f->a4[k - K_P4]);
		rt_snprintf(out, len, "%lu.%lu.%lu.%lu.in-addr.arpa", (unsigned long)(h & 255),
		            (unsigned long)(h >> 8 & 255), (unsigned long)(h >> 16 & 255), (unsigned long)(h >> 24));
	} else {
		static const char hex[] = "0123456789abcdef";
		const u8 *a = f->a6[k - K_P6].b;
		char *p = out;
		int i;
		for (i = 15; i >= 0; i--) {
			*p++ = hex[a[i] & 15];
			*p++ = '.';
			*p++ = hex[a[i] >> 4];
			*p++ = '.';
		}
		rt_strlcpy(p, "ip6.arpa", len - (u32)(p - out));
	}
}

/* write name as labels; our host name is compressed after its first use */
static u32 w_name(u8 *b, u32 off, u32 max, const char *name, u16 *hoff)
{
	int host = hoff && dns_name_eq(name, md.fqdn);

	if (host && *hoff) {
		if (off + 2 > max)
			return 0;
		rt_put_be16(b + off, (u16)(0xc000u | *hoff));
		return off + 2;
	}
	if (host)
		*hoff = (u16)off;
	while (*name) {
		const char *e = strchr(name, '.');
		u32 l = e ? (u32)(e - name) : (u32)strlen(name);
		if (l == 0 || l > 63 || off + 1 + l > max)
			return 0;
		b[off] = (u8)l;
		memcpy(b + off + 1, name, l);
		off += 1 + l;
		name += l + (e ? 1 : 0);
	}
	if (off + 1 > max)
		return 0;
	b[off++] = 0;
	return off;
}

static u16 rec_type(int k)
{
	return k < K_AAAA ? DNS_TYPE_A : k < K_P4 ? DNS_TYPE_AAAA : DNS_TYPE_PTR;
}

static u32 w_rec(u8 *b, u32 off, u32 max, const struct mdns_if *f, int k, u32 ttl, int flush, u16 *hoff)
{
	char rn[80];
	u32 rd;

	if (k >= K_P4)
		rev_name(f, k, rn, sizeof(rn));
	off = w_name(b, off, max, k >= K_P4 ? rn : md.fqdn, hoff);
	if (!off || off + 10 > max)
		return 0;
	rt_put_be16(b + off, rec_type(k));
	rt_put_be16(b + off + 2, (u16)(DNS_CLASS_IN | (flush ? CLASS_FLUSH : 0)));
	rt_put_be32(b + off + 4, ttl);
	rd = off + 10;
	if (k < K_AAAA) {
		if (rd + 4 > max)
			return 0;
		memcpy(b + rd, &f->a4[k - K_A], 4);
		rd += 4;
	} else if (k < K_P4) {
		if (rd + 16 > max)
			return 0;
		memcpy(b + rd, f->a6[k - K_AAAA].b, 16);
		rd += 16;
	} else {
		rd = w_name(b, rd, max, md.fqdn, hoff);
		if (!rd)
			return 0;
	}
	rt_put_be16(b + off + 8, (u16)(rd - off - 10));
	return rd;
}

/* ---- interfaces and their records ---- */

static int if_eligible(const struct rt_netif *n)
{
	return (n->flags & RT_IFF_UP) && !(n->flags & (RT_IFF_LOOPBACK | RT_IFF_POINTOPOINT)) &&
	       (n->flags & (RT_IFF_BROADCAST | RT_IFF_MULTICAST));
}

static struct mdns_if *if_of(u32 ifindex)
{
	int i;

	for (i = 0; i < MDNS_IFS; i++)
		if (mif[i].nif && mif[i].nif->index == ifindex)
			return &mif[i];
	return NULL;
}

static u32 recs_of(const struct mdns_if *f)
{
	u32 m = 0;
	int i;

	for (i = 0; i < f->n4; i++)
		m |= (1u << (K_A + i)) | (1u << (K_P4 + i));
	for (i = 0; i < f->n6; i++)
		m |= (1u << (K_AAAA + i)) | (1u << (K_P6 + i));
	return m;
}

/* 224.0.0.251 is joined through the socket (IP_ADD_MEMBERSHIP): IPv4
 * multicast reaches only sockets that are members on that interface */
static int mc4(struct rt_netif *n, int join)
{
	struct rt_ip_mreqn mr;

	if (!mdns_so)
		return -RT_ENOTCONN;
	memset(&mr, 0, sizeof(mr));
	mr.imr_multiaddr = mdns_grp4();
	mr.imr_ifindex = (s32)n->index;
	return mdns_so->ops->setsockopt(mdns_so, RT_IPPROTO_IP,
	                                join ? RT_IP_ADD_MEMBERSHIP : RT_IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
}

static void if_leave(struct mdns_if *f)
{
	if (f->v4)
		mc4(f->nif, 0);
	if (f->v6)
		rt_ip6_mc_leave(f->nif, &mdns_grp6);
	memset(f, 0, sizeof(*f));
}

/* join/leave groups and refresh the records; returns a signature of the
 * whole set (changes when an interface or an address does) */
static u32 if_sync(void)
{
	struct rt_netif *n;
	struct rt_in6_ifaddr_info ai[8];
	u32 sig = 0;
	int i, j, k, na;

	for (i = 0; i < MDNS_IFS; i++) {          /* gone or no longer eligible */
		for (n = rt_netif_list(); n && n != mif[i].nif; n = n->next)
			;
		if (mif[i].nif && (!n || !if_eligible(n)))
			if_leave(&mif[i]);
	}
	for (n = rt_netif_list(); n; n = n->next) {
		struct mdns_if *f = if_of(n->index);
		if (!if_eligible(n))
			continue;
		if (!f) {
			for (i = 0; i < MDNS_IFS && mif[i].nif; i++)
				;
			if (i == MDNS_IFS)
				continue;
			f = &mif[i];
			f->nif = n;
		}
		{
			struct rt_ifaddr4 a4[MDNS_V4];
			f->n4 = RT_MIN(rt_netif_addrs(n, a4, MDNS_V4), MDNS_V4);
			for (j = 0; j < f->n4; j++)
				f->a4[j] = a4[j].addr;
		}
		if (f->n4 && !f->v4 && mc4(n, 1) == 0)
			f->v4 = 1;
		f->n6 = 0;
		if (rt_ip6_ifenabled(n)) {
			if (!f->v6 && rt_ip6_mc_join(n, &mdns_grp6) == 0)
				f->v6 = 1;
			na = rt_in6_addr_list(n, ai, 8);
			for (j = 0; j < na && j < 8 && f->n6 < MDNS_V6; j++)
				if (!(ai[j].flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED | RT_IN6_IFF_TEMPORARY)))
					f->a6[f->n6++] = ai[j].addr;
		}
		sig = sig * 31u + n->index;
		for (j = 0; j < f->n4; j++)
			sig = sig * 31u + rt_ntohl(f->a4[j]);
		for (j = 0; j < f->n6; j++)
			for (k = 0; k < 4; k++)
				sig = sig * 31u + f->a6[j].w[k];
	}
	return sig;
}

/* ---- sending ---- */

static void xmit(struct mdns_if *f, const rt_in6_addr *dst, u16 dport, const u8 *msg, u32 len)
{
	struct rt_buf *b;
	u8 *uh;
	u16 ck;
	int v4 = rt_in6_is_v4mapped(dst);

	if (v4 ? !f->n4 : !f->v6)
		return;
	b = rt_buf_alloc(RT_BUF_HEADROOM, 8 + len);
	if (!b)
		return;
	uh = rt_buf_put(b, 8 + len);
	rt_put_be16(uh, MDNS_PORT);
	rt_put_be16(uh + 2, dport);
	rt_put_be16(uh + 4, (u16)(8 + len));
	uh[6] = uh[7] = 0;
	memcpy(uh + 8, msg, len);
	if (v4) {
		struct rt_ip_out o;
		memset(&o, 0, sizeof(o));
		o.saddr = rt_netif_src(f->nif, rt_in6_v4(dst), 0);
		o.daddr = rt_in6_v4(dst);
		o.proto = RT_IPPROTO_UDP;
		o.ttl = 255;
		o.oif = rt_ip_is_multicast(o.daddr) ? f->nif : NULL;
		ck = rt_csum_fold(rt_csum_partial(uh, 8 + len, rt_csum_pseudo(o.saddr, o.daddr, RT_IPPROTO_UDP,
		                                                             (u16)(8 + len))));
		memcpy(uh + 6, &ck, 2);
		if (!(uh[6] | uh[7]))
			uh[6] = uh[7] = 0xff;
		rt_ip_output(b, &o);
	} else {
		struct rt_ip6_out o;
		memset(&o, 0, sizeof(o));
		o.dst = *dst;
		o.oif = f->nif;
		if (rt_ip6_select_source(dst, f->nif, &o.src) < 0) {
			rt_buf_free(b);
			return;
		}
		o.nxt = RT_IPPROTO_UDP;
		o.hlim = 255;
		o.mcast_noloop = 1;
		ck = rt_csum_fold(rt_csum_partial(uh, 8 + len, rt_csum_pseudo6(&o.src, dst, RT_IPPROTO_UDP, 8 + len)));
		memcpy(uh + 6, &ck, 2);
		if (!(uh[6] | uh[7]))
			uh[6] = uh[7] = 0xff;
		rt_ip6_output(b, &o);
	}
}

static void xmit_mcast(struct mdns_if *f, const u8 *msg, u32 len)
{
	rt_in6_addr g4;

	rt_in6_set_v4mapped(&g4, mdns_grp4());
	if (f->v4)
		xmit(f, &g4, MDNS_PORT, msg, len);
	if (f->v6)
		xmit(f, &mdns_grp6, MDNS_PORT, msg, len);
}

/* A response with records 'ans' (and 'add' as additional records). */
static u32 build_response(struct mdns_if *f, u8 *b, u32 max, u16 id, u32 ans, u32 add, u32 ttl, int flush,
                          const u8 *q, u32 qlen)
{
	u16 hoff = 0;
	u32 off = 12, na = 0, nr = 0;
	int k;

	memset(b, 0, 12);
	rt_put_be16(b, id);
	rt_put_be16(b + 2, 0x8400);             /* response, authoritative */
	if (q) {                                /* legacy unicast: echo the question */
		if (12 + qlen > max)
			return 0;
		memcpy(b + 12, q, qlen);
		rt_put_be16(b + 4, 1);
		off += qlen;
	}
	for (k = 0; k < MDNS_NREC; k++)
		if ((ans >> k) & 1) {
			u32 o = w_rec(b, off, max, f, k, ttl, flush, &hoff);
			if (!o)
				break;
			off = o;
			na++;
		}
	for (k = 0; k < MDNS_NREC; k++)
		if ((add >> k) & 1) {
			u32 o = w_rec(b, off, max, f, k, ttl, flush, &hoff);
			if (!o)
				break;
			off = o;
			nr++;
		}
	rt_put_be16(b + 6, (u16)na);
	rt_put_be16(b + 10, (u16)nr);
	return na ? off : 0;
}

/* announce (ttl MDNS_TTL) or say goodbye (ttl 0) on every interface */
static void announce(u32 ttl)
{
	int i;

	for (i = 0; i < MDNS_IFS; i++) {
		struct mdns_if *f = &mif[i];
		u32 len;
		if (!f->nif)
			continue;
		len = build_response(f, mdns_buf, sizeof(mdns_buf), 0, recs_of(f), 0, ttl, 1, NULL, 0);
		if (len)
			xmit_mcast(f, mdns_buf, len);
		f->last_mc = rt_now_ms();
	}
}

static void probe(void)
{
	int i, k;

	for (i = 0; i < MDNS_IFS; i++) {
		struct mdns_if *f = &mif[i];
		u8 *b = mdns_buf;
		u16 hoff = 0;
		u32 off, ns = 0, m = recs_of(f) & (REC_A | REC_AAAA);
		if (!f->nif || !m)
			continue;
		memset(b, 0, 12);
		off = w_name(b, 12, sizeof(mdns_buf), md.fqdn, &hoff);
		rt_put_be16(b + off, TYPE_ANY);
		rt_put_be16(b + off + 2, DNS_CLASS_IN | CLASS_FLUSH);   /* QU */
		off += 4;
		rt_put_be16(b + 4, 1);
		for (k = 0; k < MDNS_NREC; k++)
			if ((m >> k) & 1) {
				u32 o = w_rec(b, off, sizeof(mdns_buf), f, k, MDNS_TTL, 0, &hoff);
				if (!o)
					break;
				off = o;
				ns++;
			}
		rt_put_be16(b + 8, (u16)ns);
		xmit_mcast(f, b, off);
	}
}

/* ---- state machine ---- */

static void set_name(void)
{
	if (md.conflicts)
		rt_snprintf(md.name, sizeof(md.name), "%.56s-%u", md.base, (unsigned)md.conflicts + 1);
	else
		rt_strlcpy(md.name, md.base, sizeof(md.name));
	rt_snprintf(md.fqdn, sizeof(md.fqdn), "%s.local", md.name);
}

/* rt_hostname's first label, "amiga" when there is none */
static void base_name(char *out, u32 len)
{
	const char *h = rt_hostname[0] ? rt_hostname : "amiga";
	u32 l = 0;

	while (h[l] && h[l] != '.' && l + 1 < len && l < 56)
		l++;
	memcpy(out, h, l);
	out[l] = 0;
}

static void start_probe(u32 delay_ms)
{
	md.state = MS_PROBE;
	md.count = 0;
	rt_timer_after(&mdns_timer, delay_ms);
}

static void conflict(const char *why)
{
	md.conflicts_total++;
	if (md.conflicts < 98)
		md.conflicts++;
	rt_log(RT_LOG_WARN, "mdns: %s is in use (%s), trying %s-%u.local", md.fqdn, why, md.base,
	       (unsigned)md.conflicts + 1);
	set_name();
	/* RFC 6762 8.1: after 15 conflicts in 10 s wait 5 s before probing */
	start_probe(md.conflicts > 15 ? 5000 : rt_random32() % 250);
}

static void mdns_rx(void *arg);
static void mdns_upcall_fn(struct rt_socket *so, u32 events, void *arg)
{
	if (events & RT_SEV_READ)
		rt_timer_after(&mdns_rx_timer, 0);
}

static int mdns_socket(void)
{
	struct rt_sockaddr_in6 sin;
	s32 off = 0;
	int r;

	if (mdns_so)
		return 0;
	mdns_upcall.fn = mdns_upcall_fn;
	r = sock_kernel_create_af(RT_AF_INET6, RT_SOCK_DGRAM, RT_IPPROTO_UDP, &mdns_upcall, &mdns_so);
	if (r < 0)
		return r;
	mdns_so->options |= RT_SO_REUSEADDR | RT_SO_REUSEPORT;
	mdns_so->ops->setsockopt(mdns_so, RT_IPPROTO_IPV6, RT_IPV6_V6ONLY, &off, sizeof(off));
	memset(&sin, 0, sizeof(sin));
	sin.sin6_len = sizeof(sin);
	sin.sin6_family = RT_AF_INET6;
	sin.sin6_port = rt_htons(MDNS_PORT);
	r = mdns_so->ops->bind(mdns_so, &sin);
	if (r < 0) {
		sock_kernel_close(mdns_so);
		mdns_so = NULL;
	}
	return r;
}

static void mdns_tick(void *arg)
{
	char b[64];
	u32 sig;
	int i, any = 0;

	if (!md.enabled)
		return;
	if (mdns_socket() < 0) {
		rt_timer_after(&mdns_timer, 5000);
		return;
	}
	sig = if_sync();
	for (i = 0; i < MDNS_IFS; i++)
		if (mif[i].nif && (mif[i].v4 && mif[i].n4 ? 1 : mif[i].n6))
			any = 1;
	base_name(b, sizeof(b));
	if (strcmp(b, md.base)) {                 /* new host name */
		if (md.state >= MS_ANNOUNCE)
			announce(0);
		rt_strlcpy(md.base, b, sizeof(md.base));
		md.conflicts = 0;
		set_name();
		md.state = MS_WAIT;
	}
	if (!any) {
		md.state = MS_WAIT;
		rt_timer_after(&mdns_timer, 1000);
		return;
	}
	if (md.state == MS_WAIT || (md.state == MS_RUN && sig != md.sig)) {
		md.sig = sig;
		start_probe(rt_random32() % 250);
		return;
	}
	md.sig = sig;
	switch (md.state) {
	case MS_PROBE:
		if (md.count < 3) {
			probe();
			md.count++;
			rt_timer_after(&mdns_timer, 250);
			return;
		}
		md.state = MS_ANNOUNCE;
		md.count = 0;
		RT_DBG(RT_DBG_DNS, "mdns: %s is ours", md.fqdn);
		/* fall through */
	case MS_ANNOUNCE:
		announce(MDNS_TTL);
		if (++md.count >= 2) {
			md.state = MS_RUN;
			rt_log(RT_LOG_INFO, "mdns: responding as %s", md.fqdn);
		}
		rt_timer_after(&mdns_timer, 1000);
		return;
	default:
		rt_timer_after(&mdns_timer, 1000);
	}
}

/* ---- receiving ---- */

static int our_addr(u16 type, const u8 *rd, u16 rdlen)
{
	int i, j;

	for (i = 0; i < MDNS_IFS; i++) {
		if (!mif[i].nif)
			continue;
		for (j = 0; type == DNS_TYPE_A && rdlen == 4 && j < mif[i].n4; j++)
			if (!memcmp(rd, &mif[i].a4[j], 4))
				return 1;
		for (j = 0; type == DNS_TYPE_AAAA && rdlen == 16 && j < mif[i].n6; j++)
			if (!memcmp(rd, mif[i].a6[j].b, 16))
				return 1;
	}
	return 0;
}

struct rr {
	char name[DNS_NAMEBUF];
	u16 type, cls, rdlen;
	u32 ttl, rd;
};

/* the resource record at *off; 0 or -1 */
static int rr_read(const u8 *m, u32 len, u32 *off, struct rr *r)
{
	u32 p;

	if (dns_expand(m, len, *off, r->name, sizeof(r->name), &p) < 0 || p + 10 > len)
		return -1;
	r->type = rt_get_be16(m + p);
	r->cls = rt_get_be16(m + p + 2);
	r->ttl = rt_get_be32(m + p + 4);
	r->rdlen = rt_get_be16(m + p + 8);
	r->rd = p + 10;
	if (r->rd + r->rdlen > len)
		return -1;
	*off = r->rd + r->rdlen;
	return 0;
}

static int skip_questions(const u8 *m, u32 len, u32 *off, u16 qd, char *tmp)
{
	u16 i;

	for (i = 0; i < qd; i++) {
		if (dns_expand(m, len, *off, tmp, DNS_NAMEBUF, off) < 0 || *off + 4 > len)
			return -1;
		*off += 4;
	}
	return 0;
}

/* records of f that answer (name, type) */
static u32 rec_match(const struct mdns_if *f, const char *name, u16 type)
{
	u32 have = recs_of(f), m = 0;
	char rn[80];
	int k;

	if (dns_name_eq(name, md.fqdn)) {
		if (type == DNS_TYPE_A || type == TYPE_ANY)
			m |= REC_A;
		if (type == DNS_TYPE_AAAA || type == TYPE_ANY)
			m |= REC_AAAA;
	} else if (type == DNS_TYPE_PTR || type == TYPE_ANY) {
		for (k = K_P4; k < MDNS_NREC; k++)
			if ((have >> k) & 1) {
				rev_name(f, k, rn, sizeof(rn));
				if (dns_name_eq(name, rn))
					m |= 1u << k;
			}
	}
	return m & have;
}

/* is r (from msg m) one of f's records (known answer)? its bit or 0 */
static u32 known(const struct mdns_if *f, const u8 *m, u32 len, const struct rr *r)
{
	u32 c = rec_match(f, r->name, r->type), nx;
	char tmp[DNS_NAMEBUF];
	int k;

	if ((r->cls & 0x7fff) != DNS_CLASS_IN || r->ttl < MDNS_TTL / 2)
		return 0;
	for (k = 0; k < MDNS_NREC; k++) {
		if (!((c >> k) & 1))
			continue;
		if ((k < K_AAAA && r->rdlen == 4 && !memcmp(m + r->rd, &f->a4[k - K_A], 4)) ||
		    (k >= K_AAAA && k < K_P4 && r->rdlen == 16 && !memcmp(m + r->rd, f->a6[k - K_AAAA].b, 16)) ||
		    (k >= K_P4 && dns_expand(m, len, r->rd, tmp, sizeof(tmp), &nx) >= 0 && dns_name_eq(tmp, md.fqdn)))
			return 1u << k;
	}
	return 0;
}

/* a response: does someone else claim our name? */
static void check_response(const u8 *m, u32 len)
{
	u16 qd = rt_get_be16(m + 4), n = (u16)(rt_get_be16(m + 6) + rt_get_be16(m + 8) + rt_get_be16(m + 10)), i;
	struct rr *r = rt_malloc(sizeof(*r));
	u32 off = 12;

	if (!r)
		return;
	if (skip_questions(m, len, &off, qd, r->name) == 0)
		for (i = 0; i < n && rr_read(m, len, &off, r) == 0; i++)
			if ((r->type == DNS_TYPE_A || r->type == DNS_TYPE_AAAA) && (r->cls & 0x7fff) == DNS_CLASS_IN &&
			    r->ttl && dns_name_eq(r->name, md.fqdn) && !our_addr(r->type, m + r->rd, r->rdlen)) {
				conflict("answer from another host");
				break;
			}
	rt_free(r);
}

/* while probing: a simultaneous probe for our name (RFC 6762 8.2) */
static void check_probe(struct mdns_if *f, const u8 *m, u32 len)
{
	u16 qd = rt_get_be16(m + 4), an = rt_get_be16(m + 6), ns = rt_get_be16(m + 8), i;
	struct rr *r = rt_malloc(sizeof(*r));
	u32 off = 12;

	if (!r)
		return;
	if (skip_questions(m, len, &off, qd, r->name) == 0)
		for (i = 0; i < an + ns && rr_read(m, len, &off, r) == 0; i++) {
			u8 ours[16];
			u16 ot, ol;
			int c;
			if (i < an || !dns_name_eq(r->name, md.fqdn) ||
			    (r->type != DNS_TYPE_A && r->type != DNS_TYPE_AAAA) || our_addr(r->type, m + r->rd, r->rdlen))
				continue;
			/* compare our first record with theirs: type, then rdata */
			if (f->n4) {
				ot = DNS_TYPE_A;
				ol = 4;
				memcpy(ours, &f->a4[0], 4);
			} else {
				ot = DNS_TYPE_AAAA;
				ol = 16;
				memcpy(ours, f->a6[0].b, 16);
			}
			c = ot != r->type ? (ot < r->type ? -1 : 1) : memcmp(ours, m + r->rd, RT_MIN(ol, r->rdlen));
			if (c < 0) {
				/* they win: probe again in a second */
				RT_DBG(RT_DBG_DNS, "mdns: lost a simultaneous probe for %s", md.fqdn);
				start_probe(1000);
			}
			break;
		}
	rt_free(r);
}

static int on_link(const struct mdns_if *f, const rt_in6_addr *a)
{
	if (!rt_in6_is_v4mapped(a))
		return 1;       /* IPv6: hop limit / link-local scope is the check */
	return f->n4 && rt_netif_on_link(f->nif, rt_in6_v4(a));
}

static void answer_query(struct mdns_if *f, const u8 *m, u32 len, const struct rt_dgram_cb *cb)
{
	u16 qd = rt_get_be16(m + 4), an = rt_get_be16(m + 6), ns = rt_get_be16(m + 8), i;
	int legacy = cb->from.sin6_port != rt_htons(MDNS_PORT), qu = 0;
	u32 off = 12, q1end = 0, ans = 0, add = 0, ttl = MDNS_TTL, rlen;
	struct rr *r;
	rt_in6_addr dst;

	if (!on_link(f, &cb->from.sin6_addr) || qd == 0)
		return;
	r = rt_malloc(sizeof(*r));
	if (!r)
		return;
	for (i = 0; i < qd; i++) {
		u16 t, c;
		if (dns_expand(m, len, off, r->name, sizeof(r->name), &off) < 0 || off + 4 > len)
			goto out;
		t = rt_get_be16(m + off);
		c = rt_get_be16(m + off + 2);
		off += 4;
		if (i == 0)
			q1end = off;
		if ((c & 0x7fff) != DNS_CLASS_IN && (c & 0x7fff) != TYPE_ANY)
			continue;
		if (rec_match(f, r->name, t)) {
			ans |= rec_match(f, r->name, t);
			qu |= (c & CLASS_FLUSH) != 0;
		}
	}
	/* known-answer suppression (RFC 6762 7.1) */
	for (i = 0; i < an && ans && rr_read(m, len, &off, r) == 0; i++)
		ans &= ~known(f, m, len, r);
	if (!ans)
		goto out;
	if (ans & REC_A)
		add |= REC_AAAA;
	if (ans & REC_AAAA)
		add |= REC_A;
	add &= recs_of(f) & ~ans;
	md.answered++;
	if (legacy) {
		/* RFC 6762 6.7: our ID, the question, no cache flush, ttl <= 10 */
		rlen = build_response(f, mdns_buf, sizeof(mdns_buf), rt_get_be16(m), ans, add, 10, 0, m + 12,
		                      q1end - 12);
		if (rlen)
			xmit(f, &cb->from.sin6_addr, rt_ntohs(cb->from.sin6_port), mdns_buf, rlen);
		goto out;
	}
	rlen = build_response(f, mdns_buf, sizeof(mdns_buf), 0, ans, add, ttl, 1, NULL, 0);
	if (!rlen)
		goto out;
	if (qu && !ns) {
		xmit(f, &cb->from.sin6_addr, MDNS_PORT, mdns_buf, rlen);
		goto out;
	}
	/* multicast, at most once a second (probe defence: 250 ms) */
	if ((s32)(rt_now_ms() - f->last_mc) < (ns ? 250 : 1000))
		goto out;
	f->last_mc = rt_now_ms();
	if (rt_in6_is_v4mapped(&cb->from.sin6_addr))
		rt_in6_set_v4mapped(&dst, mdns_grp4());
	else
		dst = mdns_grp6;
	xmit(f, &dst, MDNS_PORT, mdns_buf, rlen);
out:
	rt_free(r);
}

static void mdns_input(const u8 *m, u32 len, const struct rt_dgram_cb *cb)
{
	struct mdns_if *f = if_of(cb->ifindex);
	u16 flags;

	if (!f || len < 12 || md.state < MS_PROBE)
		return;
	flags = rt_get_be16(m + 2);
	if (flags & 0x7800)
		return;                              /* opcode not QUERY: ignore */
	if (flags & 0x8000) {
		if (!(flags & 0x000f) && cb->from.sin6_port == rt_htons(MDNS_PORT))
			check_response(m, len);
		return;
	}
	if (md.state == MS_PROBE) {
		if (rt_get_be16(m + 8))
			check_probe(f, m, len);
		return;
	}
	answer_query(f, m, len, cb);
}

static void mdns_rx(void *arg)
{
	struct rt_buf *b;

	while (mdns_so && (b = mdns_so->rcvq.head) != NULL) {
		const struct rt_dgram_cb *cb = (const struct rt_dgram_cb *)(const void *)b->cb.b;
		mdns_input(b->data, b->len, cb);
		if (!mdns_so)
			break;
		rt_bufq_unlink(&mdns_so->rcvq, b);
		rt_buf_free(b);
	}
}

/* ---- the resolver's side: answers to a legacy unicast query ---- */

int mdns_parse_answer(const u8 *m, u32 len, u16 id, const char *name, u16 qtype, u32 *a4, rt_in6_addr *a6,
                      int max, u32 *ttl)
{
	struct rr *r;
	u16 flags, i, n;
	u32 off = 12;
	int cnt = 0;

	if (len < 12 || rt_get_be16(m) != id)
		return DNS_IGNORE;
	flags = rt_get_be16(m + 2);
	if (!(flags & 0x8000) || (flags & 0x780f))
		return DNS_IGNORE;
	r = rt_malloc(sizeof(*r));
	if (!r)
		return DNS_IGNORE;
	n = (u16)(rt_get_be16(m + 6) + rt_get_be16(m + 8) + rt_get_be16(m + 10));
	*ttl = MDNS_TTL;
	if (skip_questions(m, len, &off, rt_get_be16(m + 4), r->name) == 0)
		for (i = 0; i < n && cnt < max && rr_read(m, len, &off, r) == 0; i++) {
			if (r->type != qtype || (r->cls & 0x7fff) != DNS_CLASS_IN || !dns_name_eq(r->name, name))
				continue;
			if (qtype == DNS_TYPE_A && r->rdlen == 4)
				memcpy(&a4[cnt++], m + r->rd, 4);
			else if (qtype == DNS_TYPE_AAAA && r->rdlen == 16 && !(m[r->rd] == 0xfe && (m[r->rd + 1] & 0xc0) == 0x80))
				memcpy(a6[cnt++].b, m + r->rd, 16);   /* (link-local: no zone to give) */
			else
				continue;
			if (r->ttl < *ttl)
				*ttl = r->ttl;
		}
	rt_free(r);
	return cnt ? cnt : DNS_IGNORE;
}

/* ---- configuration and status ---- */

const char *rt_mdns_hostname(void)
{
	return md.enabled && md.state >= MS_ANNOUNCE ? md.name : "";
}

void rt_mdns_if_gone(struct rt_netif *nif)
{
	int i;

	for (i = 0; i < MDNS_IFS; i++)
		if (mif[i].nif == nif)
			if_leave(&mif[i]);   /* the socket's membership too */
}

static void mdns_enable(int on)
{
	int i;

	if (on && !md.enabled) {
		md.enabled = 1;
		md.state = MS_WAIT;
		md.base[0] = 0;                           /* picked up by the tick */
		rt_timer_after(&mdns_timer, 1000);
	} else if (!on && md.enabled) {
		if (md.state >= MS_ANNOUNCE)
			announce(0);
		md.enabled = 0;
		md.state = MS_OFF;
		rt_timer_del(&mdns_timer);
		for (i = 0; i < MDNS_IFS; i++)
			if (mif[i].nif)
				if_leave(&mif[i]);
		if (mdns_so) {
			rt_timer_del(&mdns_rx_timer);
			sock_kernel_close(mdns_so);
			mdns_so = NULL;
		}
	}
}

void rt_mdns_status(char *buf, u32 len)
{
	static const char *const st[] = { "off", "waiting for an address", "probing", "announcing", "responding" };
	char ifs[48];
	u32 o = 0;
	int i;

	ifs[0] = 0;
	for (i = 0; i < MDNS_IFS; i++)
		if (mif[i].nif && o < sizeof(ifs))
			o += (u32)rt_snprintf(ifs + o, sizeof(ifs) - o, "%s%s", o ? "," : "", mif[i].nif->name);
	if (!md.enabled)
		rt_strlcpy(buf, "mDNS: off", len);
	else
		rt_snprintf(buf, len, "mDNS: %s.local, %s%s%s, %lu answers, %lu conflicts",
		            md.name[0] ? md.name : "?", st[md.state], ifs[0] ? " on " : "", ifs,
		            (unsigned long)md.answered, (unsigned long)md.conflicts_total);
}

int rt_mdns_command(int argc, char **argv, char *out, u32 outlen)
{
	const char *a = argc > 1 ? argv[1] : "status";

	out[0] = 0;
	if (argc > 2)
		goto bad;
	if (!rt_strcasecmp(a, "status"))
		rt_mdns_status(out, outlen);
	else if (!rt_strcasecmp(a, "on"))
		mdns_enable(1);
	else if (!rt_strcasecmp(a, "off"))
		mdns_enable(0);
	else
		goto bad;
	return 0;
bad:
	rt_strlcpy(out, "usage: mdns on | off | status", outlen);
	return -RT_EINVAL;
}

void rt_mdns_init(void);
void rt_mdns_init(void)
{
	memset(&mdns_grp6, 0, sizeof(mdns_grp6));
	mdns_grp6.b[0] = 0xff;
	mdns_grp6.b[1] = 0x02;
	mdns_grp6.b[15] = 0xfb;
	rt_timer_init(&mdns_timer, mdns_tick, NULL);
	rt_timer_init(&mdns_rx_timer, mdns_rx, NULL);
	/* off until "mdns on": the Amiga glue turns it on before reading the config */
}
