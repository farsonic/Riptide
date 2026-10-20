/*
 * Riptide - DNS stub resolver (RFC 1035), static hosts table and a small
 * positive cache.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * rt_resolve_name()/rt_resolve_addr() are called WITHOUT the stack lock,
 * from client tasks (gethostbyname() in bsdsocket.library), so:
 *  - they talk UDP through a private rt_sockctx and the rt_api_* calls,
 *    which take the lock themselves;
 *  - shared state (servers, search domain, hosts, cache) is read and
 *    written under the lock; the configuration calls (rt_dns_add_server,
 *    rt_hosts_add, ...) expect the caller to hold it, like all of the core;
 *  - message buffers live in one heap block (struct dns_work) because the
 *    caller may only have a 4 KB stack.
 *
 * Behaviour follows the resolv.conf defaults: timeout 2 s, attempts 2,
 * ndots 1, one search domain. Queries use a random ID and a fresh socket
 * bound to port 0 (random ephemeral port); replies must come from the
 * server's address and port 53 and echo ID and question.
 */
#include "sock_internal.h"
#include "resolv_internal.h"

#define DNS_MAX_SERVERS  4
#define DNS_PORT         53
#define DNS_TIMEOUT_MS   2000
#define DNS_ATTEMPTS     2
#define DNS_QBUF         300      /* header + 255 byte name + question */
#define DNS_ABUF         1024     /* UDP answers are <= 512 without EDNS */
#define DNS_CACHE_MAX    32
#define DNS_CACHE_NAME   128
#define DNS_CACHE_ADDRS  4
#define DNS_CACHE_MAXTTL 86400u

int rt_dns_disallowed;

static u32 dns_servers[DNS_MAX_SERVERS];
static int dns_nservers;
static char dns_search[64];

struct host_ent {
	struct host_ent *next;
	u32 addr;
	char name[1];             /* allocated to length */
};
static struct host_ent *hosts_head, *hosts_tail;

struct cache_ent {
	char name[DNS_CACHE_NAME];
	char canon[DNS_CACHE_NAME];
	u32  addr[DNS_CACHE_ADDRS];
	u8   naddr;
	u32  expires;             /* rt_now_ms() */
	u32  stamp;               /* for LRU replacement */
};
static struct cache_ent dns_cache[DNS_CACHE_MAX];
static s32 dns_cache_size = DNS_CACHE_MAX;
static s32 dns_cache_enabled = 1;
static u32 dns_cache_clock;

/* ======================================================================
 * Configuration (lock held)
 * ====================================================================== */

int rt_dns_add_server(u32 addr)
{
	int i;

	if (addr == 0 || addr == RT_INADDR_BROADCAST)
		return -RT_EINVAL;
	for (i = 0; i < dns_nservers; i++)
		if (dns_servers[i] == addr)
			return 0;
	if (dns_nservers >= DNS_MAX_SERVERS)
		return -RT_ENOSPC;
	dns_servers[dns_nservers++] = addr;
	return 0;
}

void rt_dns_remove_server(u32 addr)
{
	int i, j;

	for (i = 0; i < dns_nservers; i++) {
		if (dns_servers[i] != addr)
			continue;
		for (j = i; j + 1 < dns_nservers; j++)
			dns_servers[j] = dns_servers[j + 1];
		dns_nservers--;
		return;
	}
}

void rt_dns_clear_servers(void)
{
	dns_nservers = 0;
}

int rt_dns_get_servers(u32 *out, int max)
{
	int i;

	for (i = 0; out && i < dns_nservers && i < max; i++)
		out[i] = dns_servers[i];
	return dns_nservers;
}

void rt_dns_set_search(const char *domain)
{
	if (!domain)
		domain = "";
	while (*domain == '.')
		domain++;
	rt_strlcpy(dns_search, domain, sizeof(dns_search));
	/* no trailing dot */
	if (dns_search[0] && dns_search[strlen(dns_search) - 1] == '.')
		dns_search[strlen(dns_search) - 1] = 0;
}

int rt_hosts_add(const char *name, u32 addr)
{
	struct host_ent *h;
	u32 len;

	if (!name || !*name)
		return -RT_EINVAL;
	len = (u32)strlen(name);
	if (len > 255)
		return -RT_ENAMETOOLONG;
	h = rt_malloc(sizeof(*h) + len);
	if (!h)
		return -RT_ENOMEM;
	h->next = NULL;
	h->addr = addr;
	memcpy(h->name, name, len + 1);
	if (len && h->name[len - 1] == '.')
		h->name[len - 1] = 0;
	if (hosts_tail)
		hosts_tail->next = h;
	else
		hosts_head = h;
	hosts_tail = h;
	return 0;
}

void rt_hosts_clear(void)
{
	struct host_ent *h;

	while ((h = hosts_head) != NULL) {
		hosts_head = h->next;
		rt_free(h);
	}
	hosts_tail = NULL;
}

/* ======================================================================
 * Message encoding / decoding (pure functions, unit tested)
 * ====================================================================== */

/* Compare two domain names ignoring case and one trailing dot. */
int dns_name_eq(const char *a, const char *b)
{
	u32 la = (u32)strlen(a), lb = (u32)strlen(b);

	if (la && a[la - 1] == '.')
		la--;
	if (lb && b[lb - 1] == '.')
		lb--;
	return la == lb && rt_strncasecmp(a, b, la) == 0;
}

int dns_build_query(u8 *buf, u32 buflen, u16 id, const char *name, u16 qtype)
{
	return dns_build_query_class(buf, buflen, id, name, DNS_CLASS_IN, qtype);
}

int dns_build_query_class(u8 *buf, u32 buflen, u16 id, const char *name, u16 qclass, u16 qtype)
{
	u32 pos = 12, lab;
	const char *p = name;

	if (!name || !*name || buflen < 12 + 1 + 4)
		return -1;
	if (name[0] == '.' && name[1] == 0)
		p = "";                          /* the root */
	memset(buf, 0, 12);
	rt_put_be16(buf, id);
	rt_put_be16(buf + 2, 0x0100);          /* RD */
	rt_put_be16(buf + 4, 1);               /* QDCOUNT */
	while (*p) {
		const char *dot = strchr(p, '.');
		lab = dot ? (u32)(dot - p) : (u32)strlen(p);
		if (lab == 0 || lab > 63)
			return -1;
		if (pos + 1 + lab + 1 + 4 > buflen || pos - 12 + 1 + lab + 1 > 255)
			return -1;
		buf[pos++] = (u8)lab;
		memcpy(buf + pos, p, lab);
		pos += lab;
		p += lab;
		if (*p == '.')
			p++;
	}
	buf[pos++] = 0;
	rt_put_be16(buf + pos, qtype);
	rt_put_be16(buf + pos + 2, qclass);
	return (int)(pos + 4);
}

/* Expand the (possibly compressed) name at 'off'. Every compression pointer
 * must point strictly before the previous one (and before itself), which
 * all real compressors satisfy and which makes loops impossible. Labels
 * with '.', '\\' or non-printable bytes are rejected. Returns 0 and sets
 * *next to the offset after the name, or -1. */
int dns_expand(const u8 *msg, u32 len, u32 off, char *out, u32 outlen, u32 *next)
{
	return dns_expand_ex(msg, len, off, out, outlen, next, 0);
}

/* lenient: escape '.', '\\' and non-printable bytes (\DDD) like BIND's
 * dn_expand() instead of rejecting the name. */
int dns_expand_ex(const u8 *msg, u32 len, u32 off, char *out, u32 outlen, u32 *next,
                  int lenient)
{
	u32 pos = off, olen = 0, limit = off, i;
	int jumped = 0;

	if (outlen == 0)
		return -1;
	for (;;) {
		u8 c;
		if (pos >= len)
			return -1;
		c = msg[pos];
		if ((c & 0xc0) == 0xc0) {
			u32 ptr;
			if (pos + 1 >= len)
				return -1;
			ptr = ((u32)(c & 0x3f) << 8) | msg[pos + 1];
			if (!jumped) {
				*next = pos + 2;
				jumped = 1;
			}
			if (ptr >= limit || ptr >= pos)
				return -1;
			limit = ptr;
			pos = ptr;
			continue;
		}
		if (c & 0xc0)
			return -1;                 /* obsolete label types */
		if (c == 0) {
			if (!jumped)
				*next = pos + 1;
			break;
		}
		if (pos + 1 + c > len)
			return -1;
		if (olen + (olen ? 1u : 0u) + c + 1 > outlen || (!lenient && olen + 1 + c > 254))
			return -1;
		if (olen)
			out[olen++] = '.';
		for (i = 0; i < c; i++) {
			u8 ch = msg[pos + 1 + i];
			if (ch <= 0x20 || ch >= 0x7f || ch == '.' || ch == '\\') {
				if (!lenient)
					return -1;
				if (ch == '.' || ch == '\\') {
					if (olen + 2 >= outlen)
						return -1;
					out[olen++] = '\\';
					out[olen++] = (char)ch;
				} else {
					if (olen + 4 >= outlen)
						return -1;
					out[olen++] = '\\';
					out[olen++] = (char)('0' + ch / 100);
					out[olen++] = (char)('0' + ch / 10 % 10);
					out[olen++] = (char)('0' + ch % 10);
				}
				continue;
			}
			if (olen + 1 >= outlen)
				return -1;
			out[olen++] = (char)ch;
		}
		pos += 1 + c;
	}
	out[olen] = 0;
	return 0;
}

/* Skip the RR header of the answer at *off; fills type/class/ttl/rdlen and
 * moves *off to the RDATA. Name is expanded into 'name'. */
static int rr_header(const u8 *msg, u32 len, u32 *off, char *name, u32 namelen,
                     u16 *type, u16 *cls, u32 *ttl, u16 *rdlen)
{
	u32 p;

	if (dns_expand(msg, len, *off, name, namelen, &p) < 0)
		return -1;
	if (p + 10 > len)
		return -1;
	*type = rt_get_be16(msg + p);
	*cls = rt_get_be16(msg + p + 2);
	*ttl = rt_get_be32(msg + p + 4);
	*rdlen = rt_get_be16(msg + p + 8);
	p += 10;
	if (p + *rdlen > len)
		return -1;
	*off = p;
	return 0;
}

/* Parse a response to the query (id, qname, qtype). Results go to 'res'.
 * Returns:
 *   >0  number of addresses (A) / 1 for a PTR name
 *    0  name exists but has no data of that type (NODATA)
 *   -RT_HOST_NOT_FOUND  NXDOMAIN
 *   -RT_TRY_AGAIN       server failure / refused / truncated without data
 *   -RT_NO_RECOVERY     format error / not implemented
 *   DNS_IGNORE          not an answer to our query: keep waiting */
int dns_parse_response(const u8 *msg, u32 len, u16 id, const char *qname, u16 qtype,
                       struct dns_result *res)
{
	u32 off, ans_off, p;
	u16 flags, qd, an, type, cls, rdlen, i;
	u32 ttl;
	int pass, changed, hops = 0, n = 0, tc;
	char *tmp = res->tmp, *target = res->target;

	res->naddr = 0;
	res->ttl = DNS_CACHE_MAXTTL;
	res->name[0] = 0;
	if (len < 12 || rt_get_be16(msg) != id)
		return DNS_IGNORE;
	flags = rt_get_be16(msg + 2);
	qd = rt_get_be16(msg + 4);
	an = rt_get_be16(msg + 6);
	if (!(flags & 0x8000) || (flags & 0x7800) != 0 || qd != 1)
		return DNS_IGNORE;
	if (dns_expand(msg, len, 12, tmp, DNS_NAMEBUF, &off) < 0 || off + 4 > len)
		return DNS_IGNORE;
	if (!dns_name_eq(tmp, qname) || rt_get_be16(msg + off) != qtype ||
	    rt_get_be16(msg + off + 2) != DNS_CLASS_IN)
		return DNS_IGNORE;
	ans_off = off + 4;
	tc = (flags & 0x0200) != 0;
	switch (flags & 0x000f) {
	case 0: break;
	case 3: return -RT_HOST_NOT_FOUND;
	case 1: case 4: return -RT_NO_RECOVERY;
	default: return -RT_TRY_AGAIN;
	}

	/* Follow the CNAME chain (records may come in any order). */
	rt_strlcpy(target, qname, DNS_NAMEBUF);
	if (target[0] && target[strlen(target) - 1] == '.')
		target[strlen(target) - 1] = 0;
	for (pass = 0, changed = 1; changed && pass < DNS_MAX_CNAME; pass++) {
		changed = 0;
		for (i = 0, off = ans_off; i < an; i++, off += rdlen) {
			if (rr_header(msg, len, &off, tmp, DNS_NAMEBUF, &type, &cls, &ttl, &rdlen) < 0)
				break;                  /* truncated: use what we have */
			if (type != DNS_TYPE_CNAME || cls != DNS_CLASS_IN || !dns_name_eq(tmp, target))
				continue;
			if (dns_expand(msg, len, off, tmp, DNS_NAMEBUF, &p) < 0)
				return -RT_NO_RECOVERY;
			if (++hops > DNS_MAX_CNAME)
				return -RT_NO_RECOVERY;    /* CNAME loop */
			if (ttl < res->ttl)
				res->ttl = ttl;
			rt_strlcpy(target, tmp, DNS_NAMEBUF);
			changed = 1;
			break;
		}
	}
	if (changed)
		return -RT_NO_RECOVERY;            /* chain too long or a loop */
	rt_strlcpy(res->canon, target, DNS_NAMEBUF);

	for (i = 0, off = ans_off; i < an; i++, off += rdlen) {
		if (rr_header(msg, len, &off, tmp, DNS_NAMEBUF, &type, &cls, &ttl, &rdlen) < 0)
			break;
		if (type != qtype || cls != DNS_CLASS_IN || !dns_name_eq(tmp, target))
			continue;
		if (type == DNS_TYPE_A) {
			u32 a;
			int k;
			if (rdlen != 4)
				continue;
			memcpy(&a, msg + off, 4);   /* network order */
			for (k = 0; k < res->naddr; k++)
				if (res->addrs[k] == a)
					break;
			if (k < res->naddr || res->naddr >= res->max)
				continue;
			res->addrs[res->naddr++] = a;
			n++;
		} else if (type == DNS_TYPE_PTR) {
			if (n)
				continue;
			if (dns_expand(msg, len, off, res->name, DNS_NAMEBUF, &p) < 0 || !res->name[0])
				continue;
			n = 1;
		} else {
			continue;
		}
		if (ttl < res->ttl)
			res->ttl = ttl;
	}
	if (n > 0)
		return n;
	return tc ? -RT_TRY_AGAIN : 0;
}

/* ======================================================================
 * Network lookups
 * ====================================================================== */

struct dns_work {
	struct rt_sockctx ctx;
	struct dns_result res;
	u32 servers[DNS_MAX_SERVERS];
	int nservers;
	char search[64];
	char qname[DNS_NAMEBUF];
	u8 q[DNS_QBUF];
	u8 a[DNS_ABUF];
};

static struct dns_work *work_alloc(u32 *addrs, int max)
{
	struct dns_work *w = rt_zalloc(sizeof(*w));

	if (!w)
		return NULL;
	if (rt_ctx_init(&w->ctx, 4, NULL) < 0) {
		rt_free(w);
		return NULL;
	}
	w->res.addrs = addrs;
	w->res.max = max;
	rt_lock();
	w->nservers = dns_nservers;
	memcpy(w->servers, dns_servers, sizeof(w->servers));
	rt_strlcpy(w->search, dns_search, sizeof(w->search));
	rt_unlock();
	return w;
}

static void work_free(struct dns_work *w)
{
	rt_ctx_destroy(&w->ctx);
	rt_free(w);
}

/* Send query q (ID 'id') to every server, DNS_ATTEMPTS rounds.
 * Parse mode (qname != NULL): the reply must answer (qname, qtype) and is
 * parsed into w->res; returns as dns_parse_response().
 * Raw mode (qname == NULL, res_send): any reply with our ID from the
 * server is accepted, left in w->a; returns its length or -RT_TRY_AGAIN. */
static int dns_exchange(struct dns_work *w, u32 qlen, u16 id, const char *qname, u16 qtype)
{
	struct rt_sockaddr_in sin;
	int fd, att, s, r, result = -RT_TRY_AGAIN;

	if (w->nservers == 0)
		return -RT_TRY_AGAIN;
	fd = rt_api_socket(&w->ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (fd < 0)
		return -RT_TRY_AGAIN;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = RT_AF_INET;
	if (rt_api_bind(&w->ctx, fd, &sin, sizeof(sin)) < 0)
		goto out;

	for (att = 0; att < DNS_ATTEMPTS; att++) {
		for (s = 0; s < w->nservers; s++) {
			u32 start = rt_now_ms();
			sin.sin_port = rt_htons(DNS_PORT);
			sin.sin_addr = w->servers[s];
			if (rt_api_sendto(&w->ctx, fd, w->q, qlen, 0, &sin, sizeof(sin)) < 0)
				continue;
			for (;;) {
				struct rt_sockaddr_in from;
				u32 el = rt_now_ms() - start, fl = sizeof(from), set;
				if (el >= DNS_TIMEOUT_MS)
					break;
				set = 1u << fd;
				r = rt_api_select(&w->ctx, fd + 1, &set, NULL, NULL,
				                  (s32)(DNS_TIMEOUT_MS - el), NULL);
				if (r == -RT_EINTR) {
					result = -RT_TRY_AGAIN;
					goto out;
				}
				if (r <= 0)
					break;
				r = rt_api_recvfrom(&w->ctx, fd, w->a, sizeof(w->a), RT_MSG_DONTWAIT,
				                    &from, &fl);
				if (r < 0)
					continue;
				if (from.sin_addr != w->servers[s] || from.sin_port != rt_htons(DNS_PORT))
					continue;
				if (!qname) {
					u16 rc;
					if (r < 12 || rt_get_be16(w->a) != id || !(w->a[2] & 0x80))
						continue;
					rc = w->a[3] & 0x0f;
					if (rc == 2 || rc == 4 || rc == 5)
						break;      /* SERVFAIL/NOTIMP/REFUSED: next server */
					result = r;
					goto out;
				}
				r = dns_parse_response(w->a, (u32)r, id, qname, qtype, &w->res);
				if (r == DNS_IGNORE)
					continue;
				if (r == -RT_TRY_AGAIN || r == -RT_NO_RECOVERY) {
					result = r;
					break;              /* next server */
				}
				result = r;
				goto out;
			}
		}
	}
out:
	rt_api_close(&w->ctx, fd);
	return result;
}

static int dns_query(struct dns_work *w, const char *name, u16 qtype)
{
	u16 id = (u16)rt_random32();
	int qlen = dns_build_query(w->q, sizeof(w->q), id, name, qtype);

	if (qlen < 0)
		return -RT_NO_RECOVERY;
	return dns_exchange(w, (u32)qlen, id, name, qtype);
}

/* ---- hosts table and cache (lock held) ---- */

static int hosts_lookup(const char *name, const char *search, u32 *addrs, int max)
{
	struct host_ent *h;
	int n = 0;
	u32 nl = (u32)strlen(name), sl = (u32)strlen(search);
	int single = strchr(name, '.') == NULL;

	if (nl && name[nl - 1] == '.')
		nl--;
	for (h = hosts_head; h && n < max; h = h->next) {
		u32 hl = (u32)strlen(h->name);
		int m = hl == nl && rt_strncasecmp(h->name, name, nl) == 0;
		if (!m && single && sl && hl == nl + 1 + sl && h->name[nl] == '.' &&
		    rt_strncasecmp(h->name, name, nl) == 0 &&
		    rt_strcasecmp(h->name + nl + 1, search) == 0)
			m = 1;
		if (m)
			addrs[n++] = h->addr;
	}
	return n;
}

static struct cache_ent *cache_find(const char *name)
{
	int i;

	if (!dns_cache_enabled)
		return NULL;
	for (i = 0; i < dns_cache_size && i < DNS_CACHE_MAX; i++) {
		struct cache_ent *c = &dns_cache[i];
		if (c->naddr && dns_name_eq(c->name, name)) {
			if ((s32)(c->expires - rt_now_ms()) <= 0) {
				c->naddr = 0;
				return NULL;
			}
			c->stamp = ++dns_cache_clock;
			return c;
		}
	}
	return NULL;
}

static void cache_add(const char *name, const char *canon, const u32 *addrs, int n, u32 ttl)
{
	struct cache_ent *c, *victim = NULL;
	int i;

	if (!dns_cache_enabled || n <= 0 || ttl == 0 || strlen(name) >= DNS_CACHE_NAME ||
	    strlen(canon) >= DNS_CACHE_NAME)
		return;
	if (ttl > DNS_CACHE_MAXTTL)
		ttl = DNS_CACHE_MAXTTL;
	for (i = 0; i < dns_cache_size && i < DNS_CACHE_MAX; i++) {
		c = &dns_cache[i];
		if (!c->naddr || dns_name_eq(c->name, name)) {
			victim = c;
			break;
		}
		if (!victim || (s32)(c->stamp - victim->stamp) < 0)
			victim = c;
	}
	if (!victim)
		return;
	rt_strlcpy(victim->name, name, DNS_CACHE_NAME);
	rt_strlcpy(victim->canon, canon, DNS_CACHE_NAME);
	victim->naddr = (u8)RT_MIN(n, DNS_CACHE_ADDRS);
	memcpy(victim->addr, addrs, victim->naddr * sizeof(u32));
	victim->expires = rt_now_ms() + ttl * 1000u;
	victim->stamp = ++dns_cache_clock;
}

static void put_canon(char *canon, u32 canonlen, const char *s)
{
	if (canon && canonlen)
		rt_strlcpy(canon, s, canonlen);
}

static int is_localhost(const char *name)
{
	return !rt_strcasecmp(name, "localhost") || !rt_strcasecmp(name, "localhost.");
}

int rt_resolve_name(const char *name, u32 *addrs, int max, char *canon, u32 canonlen)
{
	struct dns_work *w;
	struct cache_ent *c;
	u32 a, nl;
	int n, r, i, dots = 0, ncand, best = -RT_HOST_NOT_FOUND, disallowed;
	char search[64];

	if (!name || !addrs || max <= 0)
		return -RT_NO_RECOVERY;
	nl = (u32)strlen(name);
	if (nl == 0 || nl > 254)
		return -RT_HOST_NOT_FOUND;
	if (rt_parse_ipv4(name, &a) == 0) {
		addrs[0] = a;
		put_canon(canon, canonlen, name);
		return 1;
	}
	rt_lock();
	rt_strlcpy(search, dns_search, sizeof(search));
	n = hosts_lookup(name, search, addrs, max);
	if (n == 0 && is_localhost(name)) {
		addrs[0] = RT_INADDR_LOOPBACK;
		n = 1;
	}
	if (n == 0 && (c = cache_find(name)) != NULL) {
		n = RT_MIN(max, (int)c->naddr);
		memcpy(addrs, c->addr, (size_t)n * sizeof(u32));
		put_canon(canon, canonlen, c->canon);
		rt_unlock();
		return n;
	}
	disallowed = rt_dns_disallowed;
	rt_unlock();
	if (n > 0) {
		put_canon(canon, canonlen, name);
		return n;
	}
	if (disallowed)
		return -RT_HOST_NOT_FOUND;

	w = work_alloc(addrs, max);
	if (!w)
		return -RT_TRY_AGAIN;
	for (i = 0; i < (int)nl; i++)
		dots += name[i] == '.';
	/* candidates (ndots:1): absolute first if the name has a dot */
	ncand = (name[nl - 1] == '.' || !w->search[0]) ? 1 : 2;
	for (i = 0; i < ncand; i++) {
		int with_search = ncand == 2 && (dots ? i == 1 : i == 0);
		if (with_search) {
			if (nl + 1 + strlen(w->search) >= DNS_NAMEBUF)
				continue;
			rt_snprintf(w->qname, sizeof(w->qname), "%s.%s", name, w->search);
		} else {
			rt_strlcpy(w->qname, name, sizeof(w->qname));
		}
		r = dns_query(w, w->qname, DNS_TYPE_A);
		if (r > 0) {
			rt_lock();
			cache_add(name, w->res.canon, addrs, r, w->res.ttl);
			rt_unlock();
			put_canon(canon, canonlen, w->res.canon);
			work_free(w);
			return r;
		}
		if (r == 0 && best == -RT_HOST_NOT_FOUND)
			best = -RT_NO_DATA;
		else if (r == -RT_TRY_AGAIN || r == -RT_NO_RECOVERY)
			best = r;
	}
	work_free(w);
	return best;
}

int rt_resolve_addr(u32 addr, char *name, u32 namelen)
{
	struct dns_work *w;
	struct host_ent *h;
	u32 ha = rt_ntohl(addr);
	int r, found = 0, disallowed;

	if (!name || namelen == 0)
		return -RT_NO_RECOVERY;
	rt_lock();
	for (h = hosts_head; h; h = h->next) {
		if (h->addr == addr) {
			rt_strlcpy(name, h->name, namelen);
			found = 1;
			break;
		}
	}
	disallowed = rt_dns_disallowed;
	rt_unlock();
	if (found)
		return 0;
	if (addr == RT_INADDR_LOOPBACK) {
		rt_strlcpy(name, "localhost", namelen);
		return 0;
	}
	if (disallowed)
		return -RT_HOST_NOT_FOUND;
	w = work_alloc(NULL, 0);
	if (!w)
		return -RT_TRY_AGAIN;
	rt_snprintf(w->qname, sizeof(w->qname), "%u.%u.%u.%u.in-addr.arpa",
	            (unsigned)(ha & 255), (unsigned)((ha >> 8) & 255),
	            (unsigned)((ha >> 16) & 255), (unsigned)(ha >> 24));
	r = dns_query(w, w->qname, DNS_TYPE_PTR);
	if (r > 0) {
		rt_strlcpy(name, w->res.name, namelen);
		r = 0;
	} else if (r == 0) {
		r = -RT_NO_DATA;
	}
	work_free(w);
	return r;
}

/* ======================================================================
 * BIND style raw interface (res_mkquery/res_send/res_query/res_search,
 * dn_expand/dn_skipname) for miami.library. Called WITHOUT the lock.
 * ====================================================================== */

int rt_res_mkquery(int op, const char *dname, int cls, int type, u8 *buf, int buflen)
{
	if (op != 0 || !buf || buflen <= 0 || cls < 0 || cls > 0xffff || type < 0 || type > 0xffff)
		return -1;                      /* only QUERY */
	return dns_build_query_class(buf, (u32)buflen, (u16)rt_random32(), dname,
	                             (u16)cls, (u16)type);
}

int rt_res_send(const u8 *msg, int msglen, u8 *answer, int anslen)
{
	struct dns_work *w;
	int r, disallowed;

	if (!msg || msglen < 12 || msglen > DNS_QBUF || !answer || anslen <= 0)
		return -RT_NO_RECOVERY;
	rt_lock();
	disallowed = rt_dns_disallowed;
	rt_unlock();
	if (disallowed)
		return -RT_TRY_AGAIN;
	w = work_alloc(NULL, 0);
	if (!w)
		return -RT_TRY_AGAIN;
	memcpy(w->q, msg, (u32)msglen);
	r = dns_exchange(w, (u32)msglen, rt_get_be16(msg), NULL, 0);
	if (r > 0) {
		if (r > anslen)
			r = anslen;               /* truncated copy, as BIND */
		memcpy(answer, w->a, (u32)r);
	}
	work_free(w);
	return r;
}

int rt_res_query(const char *dname, int cls, int type, u8 *answer, int anslen)
{
	u8 *q;
	int qlen, r;

	if (!dname || !answer || anslen < 12)
		return -RT_NO_RECOVERY;
	q = rt_malloc(DNS_QBUF);
	if (!q)
		return -RT_TRY_AGAIN;
	qlen = rt_res_mkquery(0, dname, cls, type, q, DNS_QBUF);
	r = qlen < 0 ? -RT_NO_RECOVERY : rt_res_send(q, qlen, answer, anslen);
	rt_free(q);
	if (r < 12)
		return r < 0 ? r : -RT_NO_RECOVERY;
	switch (answer[3] & 0x0f) {
	case 0:
		return rt_get_be16(answer + 6) ? r : -RT_NO_DATA;
	case 3:
		return -RT_HOST_NOT_FOUND;
	case 2:
		return -RT_TRY_AGAIN;
	}
	return -RT_NO_RECOVERY;
}

int rt_res_querydomain(const char *name, const char *domain, int cls, int type,
                       u8 *answer, int anslen)
{
	char *full;
	int r;

	if (!name)
		return -RT_NO_RECOVERY;
	if (!domain || !*domain)
		return rt_res_query(name, cls, type, answer, anslen);
	full = rt_malloc(DNS_NAMEBUF);
	if (!full)
		return -RT_TRY_AGAIN;
	if (strlen(name) + 1 + strlen(domain) >= DNS_NAMEBUF) {
		rt_free(full);
		return -RT_NO_RECOVERY;
	}
	rt_snprintf(full, DNS_NAMEBUF, "%s.%s", name, domain);
	r = rt_res_query(full, cls, type, answer, anslen);
	rt_free(full);
	return r;
}

int rt_res_search(const char *dname, int cls, int type, u8 *answer, int anslen)
{
	char search[64];
	u32 nl;
	int r, r2;

	if (!dname || !*dname)
		return -RT_NO_RECOVERY;
	nl = (u32)strlen(dname);
	rt_lock();
	rt_strlcpy(search, dns_search, sizeof(search));
	rt_unlock();
	if (dname[nl - 1] == '.' || !search[0])
		return rt_res_query(dname, cls, type, answer, anslen);
	if (strchr(dname, '.')) {          /* ndots:1 - absolute first */
		r = rt_res_query(dname, cls, type, answer, anslen);
		if (r >= 0 || r == -RT_TRY_AGAIN)
			return r;
		r2 = rt_res_querydomain(dname, search, cls, type, answer, anslen);
		return r2 >= 0 ? r2 : r;
	}
	r = rt_res_querydomain(dname, search, cls, type, answer, anslen);
	if (r >= 0 || r == -RT_TRY_AGAIN)
		return r;
	r2 = rt_res_query(dname, cls, type, answer, anslen);
	return r2 >= 0 ? r2 : r;
}

int rt_dn_expand(const u8 *msg, const u8 *eom, const u8 *src, char *dst, int dstsize)
{
	u32 next;

	if (!msg || !eom || !src || !dst || dstsize <= 0 || eom <= msg || src < msg || src >= eom)
		return -1;
	if (dns_expand_ex(msg, (u32)(eom - msg), (u32)(src - msg), dst, (u32)dstsize, &next, 1) < 0)
		return -1;
	return (int)(next - (u32)(src - msg));
}

int rt_dn_skipname(const u8 *src, const u8 *eom)
{
	const u8 *p = src;

	if (!src || !eom)
		return -1;
	while (p < eom) {
		u8 c = *p;
		if ((c & 0xc0) == 0xc0)
			return p + 2 <= eom ? (int)(p + 2 - src) : -1;
		if (c & 0xc0)
			return -1;
		if (c == 0)
			return (int)(p + 1 - src);
		p += 1 + c;
	}
	return -1;
}

/* ======================================================================
 * sysctl (Miami CTL_DNS 9, dns.cache 0)
 * ====================================================================== */

static int cache_flush_handler(const struct rt_sysctl *e, void *old, u32 *oldlen,
                               const void *newp, u32 newlen)
{
	s32 zero = 0, v;

	if (oldlen) {
		if (old) {
			if (*oldlen < sizeof(s32))
				return -RT_ENOMEM;
			memcpy(old, &zero, sizeof(zero));
		}
		*oldlen = sizeof(s32);
	}
	if (newp) {
		if (newlen != sizeof(s32))
			return -RT_EINVAL;
		memcpy(&v, newp, sizeof(v));
		if (v)
			memset(dns_cache, 0, sizeof(dns_cache));
	}
	return 0;
}

static const struct rt_sysctl resolv_sysctls[] = {
	{ "dns.cache.size", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 0, 0 },
	  &dns_cache_size, 0, DNS_CACHE_MAX, NULL, "DNS cache entries in use" },
	{ "dns.cache.flush", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 0, 1 },
	  NULL, 0, 1, cache_flush_handler, "write 1 to flush the DNS cache" },
	{ "dns.cache.enabled", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 0, 2 },
	  &dns_cache_enabled, 0, 1, NULL, "DNS cache on/off" },
};

void rt_resolv_init(void)
{
	rt_sysctl_register(resolv_sysctls, (int)RT_ARRAY_SIZE(resolv_sysctls));
}
