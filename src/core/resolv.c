/*
 * Riptide - DNS stub resolver (RFC 1035), static hosts table, a small
 * positive cache and a negative one (RFC 2308, RFC 9520).
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
 * ndots 1, a search list of up to six domains (the first is the "domain";
 * DHCP option 119, RA DNSSL). Queries use a random ID and a fresh socket
 * bound to port 0 (random ephemeral port); replies must come from the
 * server's address and port 53 and echo ID and question.
 *
 * IPv6: servers may be IPv6 (kept with the interface for link-local
 * ones); those learned from router advertisements (RFC 8106) or DHCPv6
 * expire. AAAA lookups and ip6.arpa PTR lookups use the same machinery;
 * the hosts table holds both families (IPv4 as IPv4-mapped).
 *
 * EDNS0 (RFC 6891): our own queries carry an OPT record advertising
 * dns.edns_size (1232, the DNS Flag Day 2020 value) so answers up to that
 * size come over UDP. A server that answers FORMERR/NOTIMP to it is asked
 * again without (once) and remembered as one without EDNS for 10 minutes;
 * so is one whose answers carry no OPT record. Answers with TC set are
 * asked again over TCP (RFC 7766, dns.tcp_fallback), from the same server;
 * if that fails the UDP part is used as before. The BIND style raw calls
 * (res_send & co.) send the caller's message unchanged (no OPT, as BIND
 * without RES_USE_EDNS0) and also retry truncated answers over TCP.
 */
#include "sock_internal.h"
#include "resolv_internal.h"

#define DNS_MAX_SERVERS  6
#define DNS_MAX_STATIC   4
#define DNS_PORT         53
#define DNS_TIMEOUT_MS   2000
#define DNS_ATTEMPTS     2
#define DNS_QBUF         300      /* header + 255 byte name + question */
#define DNS_UDP_MIN      512      /* UDP answers without EDNS (RFC 1035) */
#define DNS_EDNS_MAX     4096     /* largest dns.edns_size */
#define DNS_OPT_LEN      11       /* our OPT record: root, type, class, ttl, rdlen */
#define DNS_TCP_TIMEOUT_MS 5000   /* connect + query + answer over TCP */
#define DNS_TCP_MAX      16384    /* largest TCP answer we parse */
#define DNS_NOEDNS_MS    600000u  /* a server without EDNS is remembered */
#define DNS_CACHE_MAX    32
#define DNS_CACHE_NAME   128
#define DNS_CACHE_ADDRS  8        /* as many as gethostbyname() returns */
#define DNS_CACHE_MAXTTL 86400u

int rt_dns_disallowed;
s32 rt_dns_edns_size = 1232;   /* 0: no EDNS */
s32 rt_dns_tcp = 1;            /* truncated answers again over TCP */
u32 rt_dns_stat_tcp, rt_dns_stat_tcp_fail, rt_dns_stat_noedns;

struct dns_server {
	rt_in6_addr addr;         /* IPv4-mapped for IPv4 */
	u32 ifindex;              /* zone of link-local servers */
	u32 expires_s;            /* rt_ip6_seconds() deadline, 0 = never */
	u8  source;               /* RT_DNS_SRC_* */
	u8  noedns;               /* answered FORMERR to EDNS or without OPT */
	u32 noedns_until;         /* rt_now_ms() */
};
static struct dns_server dns_servers[DNS_MAX_SERVERS];
static int dns_nservers;
/* The search list, space separated ("lan example.org"); its first domain
 * is the one rt_dns_search_domain() returns. */
#define DNS_SEARCH_MAX   6
#define DNS_SEARCH_LEN   256
static char dns_search[DNS_SEARCH_LEN];
static char dns_search_first[64];
static u8 dns_search_auto;    /* dns_search came from DNSSL / DHCPv6 / DHCP 119 */

/* Domain i of a search list into out (NUL terminated); 0 when the list
 * has no domain i. */
static int search_nth(const char *list, int i, char *out, u32 outlen)
{
	const char *p = list;
	u32 n;

	for (;;) {
		while (*p == ' ')
			p++;
		if (!*p)
			return 0;
		for (n = 0; p[n] && p[n] != ' '; n++)
			;
		if (i-- == 0) {
			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, p, n);
			out[n] = 0;
			return 1;
		}
		p += n;
	}
}

static int search_count(const char *list)
{
	char d[64];
	int n = 0;

	while (search_nth(list, n, d, sizeof(d)))
		n++;
	return n;
}

struct host_ent {
	struct host_ent *next;
	rt_in6_addr addr;         /* IPv4-mapped for IPv4 */
	char name[1];             /* allocated to length */
};
static struct host_ent *hosts_head, *hosts_tail;

struct cache_ent {
	char name[DNS_CACHE_NAME];
	char canon[DNS_CACHE_NAME];
	rt_in6_addr addr[DNS_CACHE_ADDRS];   /* IPv4-mapped for A */
	u16  qtype;
	u8   naddr;
	u32  expires;             /* rt_now_ms() */
	u32  stamp;               /* for LRU replacement */
};
static struct cache_ent dns_cache[DNS_CACHE_MAX];
static s32 dns_cache_size = DNS_CACHE_MAX;
static s32 dns_cache_enabled = 1;
static u32 dns_cache_clock;

/* Negative cache (RFC 2308) and resolution failures (RFC 9520), per fully
 * qualified query name (each search list candidate on its own):
 *  - NXDOMAIN: the name does not exist, for any type (RFC 8020);
 *  - NODATA: the name exists without records of this type (so an AAAA
 *    NODATA never hides the A records);
 *  - FAIL: SERVFAIL, REFUSED, FORMERR or no answer from any server:
 *    1 s, doubled for each repeated failure up to dns.cache.fail_max s
 *    (RFC 9520 3: at least 1 s, at most 5 minutes). Not for a break
 *    (Ctrl-C), nor when no query could be sent at all; forgotten when the
 *    server list changes. */
#define DNS_NEG_MAX      32
#define NEG_NXDOMAIN     1
#define NEG_NODATA       2
#define NEG_FAIL         3
struct neg_ent {
	char name[DNS_CACHE_NAME];
	u16  qtype;               /* 0 for NXDOMAIN: every type */
	u8   kind;                /* NEG_*, 0 = unused */
	u8   fails;               /* NEG_FAIL: failures in a row */
	s16  err;                 /* NEG_FAIL: -RT_TRY_AGAIN or -RT_NO_RECOVERY */
	u32  expires;             /* rt_now_ms() */
	u32  stamp;               /* for LRU replacement */
};
static struct neg_ent dns_neg[DNS_NEG_MAX];
static s32 dns_neg_maxttl = 3600;   /* dns.cache.neg_maxttl, 0 = no negative caching */
static s32 dns_fail_max = 30;       /* dns.cache.fail_max, 0 = no failure caching */
static u32 dns_stat_hits, dns_stat_neg_hits, dns_stat_fail_hits;
static void neg_flush(int fail_only);
static int neg_find(const char *name, u16 qtype);
static void neg_add(const char *name, u16 qtype, int r, u32 negttl);
static void neg_fail(const char *name, u16 qtype, int err);

/* ======================================================================
 * Configuration (lock held)
 * ====================================================================== */

static void servers_expire(void)
{
	u32 now = rt_ip6_seconds();
	int i = 0, j;

	while (i < dns_nservers) {
		if (dns_servers[i].expires_s && (s32)(now - dns_servers[i].expires_s) >= 0) {
			for (j = i; j + 1 < dns_nservers; j++)
				dns_servers[j] = dns_servers[j + 1];
			dns_nservers--;
			continue;
		}
		i++;
	}
}

int rt_dns_add_server6(const rt_in6_addr *a, u32 ifindex, u32 lifetime_s, int source)
{
	int i;

	if (rt_in6_is_unspec(a) || rt_in6_is_mcast(a) ||
	    (rt_in6_is_v4mapped(a) && (a->w[3] == 0 || a->w[3] == RT_INADDR_BROADCAST)))
		return -RT_EINVAL;
	if (!rt_in6_is_linklocal(a))
		ifindex = 0;
	servers_expire();
	for (i = 0; i < dns_nservers; i++) {
		if (!rt_in6_eq(&dns_servers[i].addr, a))
			continue;
		/* a configured server stays configured; learned ones refresh */
		if (dns_servers[i].source != RT_DNS_SRC_STATIC) {
			dns_servers[i].source = (u8)source;
			dns_servers[i].ifindex = ifindex;
			dns_servers[i].expires_s = source == RT_DNS_SRC_STATIC || lifetime_s == RT_IN6_INFINITE ?
			                           0 : rt_ip6_seconds() + lifetime_s;
		}
		return 0;
	}
	if (dns_nservers >= DNS_MAX_SERVERS)
		return -RT_ENOSPC;
	if (source == RT_DNS_SRC_STATIC) {
		/* as before IPv6: four configured servers; the other slots are
		 * for servers learned from RAs / DHCPv6 */
		int nstatic = 0;
		for (i = 0; i < dns_nservers; i++)
			nstatic += dns_servers[i].source == RT_DNS_SRC_STATIC;
		if (nstatic >= DNS_MAX_STATIC)
			return -RT_ENOSPC;
	}
	dns_servers[dns_nservers].addr = *a;
	dns_servers[dns_nservers].ifindex = ifindex;
	dns_servers[dns_nservers].source = (u8)source;
	dns_servers[dns_nservers].expires_s = source == RT_DNS_SRC_STATIC || lifetime_s == RT_IN6_INFINITE ?
	                                      0 : rt_ip6_seconds() + lifetime_s;
	dns_servers[dns_nservers].noedns = 0;
	dns_nservers++;
	neg_flush(1);                     /* a new server may answer */
	return 0;
}

int rt_dns_add_server(u32 addr)
{
	rt_in6_addr a;

	if (addr == 0 || addr == RT_INADDR_BROADCAST)
		return -RT_EINVAL;
	rt_in6_set_v4mapped(&a, addr);
	return rt_dns_add_server6(&a, 0, RT_IN6_INFINITE, RT_DNS_SRC_STATIC);
}

void rt_dns_remove_server6(const rt_in6_addr *a)
{
	int i, j;

	for (i = 0; i < dns_nservers; i++) {
		if (!rt_in6_eq(&dns_servers[i].addr, a))
			continue;
		for (j = i; j + 1 < dns_nservers; j++)
			dns_servers[j] = dns_servers[j + 1];
		dns_nservers--;
		neg_flush(1);
		return;
	}
}

void rt_dns_remove_server(u32 addr)
{
	rt_in6_addr a;

	rt_in6_set_v4mapped(&a, addr);
	rt_dns_remove_server6(&a);
}

void rt_dns_clear_servers(void)
{
	dns_nservers = 0;
	neg_flush(1);
}

/* IPv4 servers only (the rt_ctl_dns record and the old API) */
int rt_dns_get_servers(u32 *out, int max)
{
	int i, n = 0;

	servers_expire();
	for (i = 0; i < dns_nservers; i++) {
		if (!rt_in6_is_v4mapped(&dns_servers[i].addr))
			continue;
		if (out && n < max)
			out[n] = rt_in6_v4(&dns_servers[i].addr);
		n++;
	}
	return n;
}

int rt_dns_get_servers6(rt_in6_addr *out, u32 *ifindex, u8 *source, int max)
{
	int i;

	servers_expire();
	for (i = 0; i < dns_nservers && i < max; i++) {
		if (out)
			out[i] = dns_servers[i].addr;
		if (ifindex)
			ifindex[i] = dns_servers[i].ifindex;
		if (source)
			source[i] = dns_servers[i].source;
	}
	return dns_nservers;
}

const char *rt_dns_search_domain(void)
{
	return dns_search_first;
}

const char *rt_dns_search_list(void)
{
	return dns_search;
}

/* RFC 8106 DNSSL / DHCPv6 domain list: only when none is configured */
void rt_dns_set_search_auto(const char *domain);
void rt_dns_set_search_auto(const char *domain)
{
	if (dns_search[0] && !dns_search_auto)
		return;
	rt_dns_set_search(domain);
	dns_search_auto = 1;
}

int rt_dns_search_is_auto(void)
{
	return dns_search_auto;
}

/* One domain, or a list separated by spaces or commas: at most six, each
 * shorter than 64 characters, without leading or trailing dots, no
 * duplicates. */
void rt_dns_set_search(const char *domain)
{
	const char *p = domain ? domain : "";
	u32 o = 0, n, k;
	int count = 0;

	dns_search[0] = 0;
	while (*p && count < DNS_SEARCH_MAX) {
		while (*p == ' ' || *p == ',' || *p == '\t' || *p == '.')
			p++;
		for (n = 0; p[n] && p[n] != ' ' && p[n] != ',' && p[n] != '\t'; n++)
			;
		k = n;
		while (k && p[k - 1] == '.')
			k--;
		if (k && k < 64 && o + k + 2 <= sizeof(dns_search)) {
			char d[64];
			int i, dup = 0;
			memcpy(d, p, k);
			d[k] = 0;
			for (i = 0; i < count && !dup; i++) {
				char e[64];
				dup = search_nth(dns_search, i, e, sizeof(e)) && !rt_strcasecmp(e, d);
			}
			if (!dup) {
				if (o)
					dns_search[o++] = ' ';
				memcpy(dns_search + o, d, k + 1);
				o += k;
				count++;
			}
		}
		p += n;
	}
	dns_search_auto = 0;
	if (!search_nth(dns_search, 0, dns_search_first, sizeof(dns_search_first)))
		dns_search_first[0] = 0;
}

int rt_hosts_add(const char *name, u32 addr)
{
	rt_in6_addr a;

	rt_in6_set_v4mapped(&a, addr);
	return rt_hosts_add6(name, &a);
}

int rt_hosts_add6(const char *name, const rt_in6_addr *addr)
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
	h->addr = *addr;
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

/* Append an EDNS0 OPT record (RFC 6891 6.1.2: root owner, type 41,
 * class = our UDP payload size, extended RCODE/version/flags 0, no
 * options) to the query of length len and set ARCOUNT to 1. Returns the
 * new length or -1. */
int dns_add_opt(u8 *buf, u32 len, u32 buflen, u16 udpsize)
{
	u8 *p = buf + len;

	if (len < 12 || len + DNS_OPT_LEN > buflen)
		return -1;
	p[0] = 0;                              /* root */
	rt_put_be16(p + 1, DNS_TYPE_OPT);
	rt_put_be16(p + 3, udpsize);
	rt_put_be32(p + 5, 0);
	rt_put_be16(p + 9, 0);
	rt_put_be16(buf + 10, 1);
	return (int)(len + DNS_OPT_LEN);
}

/* Offset after the (possibly compressed) name at off, or 0. */
static u32 skip_name(const u8 *msg, u32 len, u32 off)
{
	while (off < len) {
		u8 c = msg[off];
		if (c == 0)
			return off + 1;
		if ((c & 0xc0) == 0xc0)
			return off + 2 <= len ? off + 2 : 0;
		if (c & 0xc0)
			return 0;
		off += 1u + c;
	}
	return 0;
}

/* Is there an OPT record in the additional section? 1 (its UDP payload
 * size in *udpsize), 0 none, -1 malformed. Walks every section. */
int dns_find_opt(const u8 *msg, u32 len, u16 *udpsize)
{
	u32 off = 12, i, n, p;

	if (len < 12)
		return -1;
	for (i = rt_get_be16(msg + 4); i; i--) {
		p = skip_name(msg, len, off);
		if (!p || p + 4 > len)
			return -1;
		off = p + 4;
	}
	n = (u32)rt_get_be16(msg + 6) + rt_get_be16(msg + 8);
	for (i = 0; i < n + rt_get_be16(msg + 10); i++) {
		u16 type, rdlen;
		p = skip_name(msg, len, off);
		if (!p || p + 10 > len)
			return -1;
		type = rt_get_be16(msg + p);
		rdlen = rt_get_be16(msg + p + 8);
		if (p + 10 + rdlen > len)
			return -1;
		if (i >= n && type == DNS_TYPE_OPT) {
			if (udpsize)
				*udpsize = rt_get_be16(msg + p + 2);
			return 1;
		}
		off = p + 10 + rdlen;
	}
	return 0;
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

/* RFC 2308 5: how long a negative answer (NXDOMAIN / NODATA) may be
 * cached: the smaller of the SOA record's TTL and its MINIMUM field, and
 * of the TTLs of the CNAME records that led there. 0 when the authority
 * section has no SOA (then it is not cached, RFC 2308 5). */
static u32 neg_ttl(const u8 *msg, u32 len, u32 off, u16 an, u16 ns, char *tmp)
{
	u32 ttl, min = DNS_CACHE_MAXTTL, p;
	u16 type, cls, rdlen, i;

	for (i = 0; i < an; i++, off += rdlen) {
		if (rr_header(msg, len, &off, tmp, DNS_NAMEBUF, &type, &cls, &ttl, &rdlen) < 0)
			return 0;
		if (ttl < min)
			min = ttl;
	}
	for (i = 0; i < ns; i++, off += rdlen) {
		u32 soamin;
		if (rr_header(msg, len, &off, tmp, DNS_NAMEBUF, &type, &cls, &ttl, &rdlen) < 0)
			return 0;
		if (type != DNS_TYPE_SOA || cls != DNS_CLASS_IN)
			continue;
		/* MNAME, RNAME, then SERIAL REFRESH RETRY EXPIRE MINIMUM */
		if (dns_expand(msg, len, off, tmp, DNS_NAMEBUF, &p) < 0 ||
		    dns_expand(msg, len, p, tmp, DNS_NAMEBUF, &p) < 0 || p + 20 != off + rdlen)
			return 0;
		soamin = rt_get_be32(msg + p + 16);
		if (ttl < min)
			min = ttl;
		return soamin < min ? soamin : min;
	}
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
	res->negttl = 0;
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
	case 3:
		res->negttl = neg_ttl(msg, len, ans_off, an, rt_get_be16(msg + 8), tmp);
		return -RT_HOST_NOT_FOUND;
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
		} else if (type == DNS_TYPE_AAAA) {
			rt_in6_addr a;
			int k;
			if (rdlen != 16 || !res->addrs6)
				continue;
			memcpy(a.b, msg + off, 16);
			for (k = 0; k < res->naddr; k++)
				if (rt_in6_eq(&res->addrs6[k], &a))
					break;
			if (k < res->naddr || res->naddr >= res->max)
				continue;
			res->addrs6[res->naddr++] = a;
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
	if (tc)
		return -RT_TRY_AGAIN;
	res->negttl = neg_ttl(msg, len, ans_off, an, rt_get_be16(msg + 8), tmp);
	return 0;
}

/* ======================================================================
 * Network lookups
 * ====================================================================== */

struct dns_work {
	struct rt_sockctx ctx;
	struct dns_result res;
	struct dns_server servers[DNS_MAX_SERVERS];
	int nservers;
	char search[DNS_SEARCH_LEN];
	char qname[DNS_NAMEBUF];
	u8 q[DNS_QBUF];
	u8 *a;                    /* UDP answers, alen bytes */
	u32 alen;
	u16 edns;                 /* our UDP payload size, 0 = no EDNS */
	u8 tcp;                   /* dns.tcp_fallback */
	u8 *ans;                  /* raw mode: the answer (a or tcpbuf) */
	u8 *tcpbuf;               /* answer read over TCP */
	u32 rawcap;               /* raw mode: the caller's buffer size */
	const u8 *rawq;           /* raw mode: the caller's query */
	u8 intr;                  /* the last exchange ended with a break */
	u16 sent;                 /* queries sent by it (0: nothing went out) */
	/* dns_exchange46(): the AAAA query and its result */
	struct dns_result res6;
	u8 q6[DNS_QBUF];
};

static struct dns_work *work_alloc(u32 *addrs, int max)
{
	struct dns_work *w = rt_zalloc(sizeof(*w));
	u32 now = rt_now_ms();
	int i;

	if (!w)
		return NULL;
	rt_lock();
	w->edns = (u16)rt_dns_edns_size;
	w->tcp = rt_dns_tcp != 0;
	rt_unlock();
	if (w->edns && w->edns < DNS_UDP_MIN)
		w->edns = DNS_UDP_MIN;
	/* answers are as large as we advertise (at least the classic 512);
	 * short of memory: no EDNS rather than no lookup */
	w->alen = w->edns > DNS_UDP_MIN ? w->edns : DNS_UDP_MIN;
	w->a = rt_malloc(w->alen);
	if (!w->a && w->alen > DNS_UDP_MIN) {
		w->edns = 0;
		w->alen = DNS_UDP_MIN;
		w->a = rt_malloc(w->alen);
	}
	if (!w->a || rt_ctx_init(&w->ctx, 4, NULL) < 0) {
		rt_free(w->a);
		rt_free(w);
		return NULL;
	}
	w->ans = w->a;
	w->res.addrs = addrs;
	w->res.max = max;
	rt_lock();
	servers_expire();
	w->nservers = dns_nservers;
	memcpy(w->servers, dns_servers, sizeof(w->servers));
	rt_strlcpy(w->search, dns_search, sizeof(w->search));
	rt_unlock();
	for (i = 0; i < w->nservers; i++)
		if (w->servers[i].noedns && (s32)(w->servers[i].noedns_until - now) <= 0)
			w->servers[i].noedns = 0;
	return w;
}

static void work_free(struct dns_work *w)
{
	rt_ctx_destroy(&w->ctx);
	rt_free(w->tcpbuf);
	rt_free(w->a);
	rt_free(w);
}

/* Server s answered FORMERR/NOTIMP to EDNS, or without an OPT record:
 * no EDNS to it for a while (also for later lookups). */
static void server_noedns(struct dns_work *w, int s)
{
	u32 until = rt_now_ms() + DNS_NOEDNS_MS;
	int i;

	if (w->servers[s].noedns)
		return;
	w->servers[s].noedns = 1;
	rt_lock();
	for (i = 0; i < dns_nservers; i++)
		if (rt_in6_eq(&dns_servers[i].addr, &w->servers[s].addr)) {
			dns_servers[i].noedns = 1;
			dns_servers[i].noedns_until = until;
		}
	rt_dns_stat_noedns++;
	rt_unlock();
	RT_DBG(RT_DBG_DNS, "server %d: no EDNS for %lu s", s + 1, (unsigned long)(DNS_NOEDNS_MS / 1000u));
}

/* A query built without OPT (length len) followed by our OPT record:
 * the length to send with or without EDNS (ARCOUNT set to match). */
static u32 q_edns(u8 *q, u32 len, int on)
{
	rt_put_be16(q + 10, on ? 1 : 0);
	return on ? len + DNS_OPT_LEN : len;
}

/* Build our query for name into q (DNS_QBUF): the plain length, with the
 * OPT record after it when EDNS is on; -1 if the name does not fit. */
static int build_q(struct dns_work *w, u8 *q, u16 id, const char *name, u16 qtype)
{
	int len = dns_build_query(q, DNS_QBUF, id, name, qtype);

	if (len < 0 || (w->edns && dns_add_opt(q, (u32)len, DNS_QBUF, w->edns) < 0))
		return -1;
	return len;
}

static int sock_wait(struct dns_work *w, int fd, int wr, u32 deadline)
{
	u32 set = 1u << fd;
	s32 left = (s32)(deadline - rt_now_ms());

	if (left <= 0)
		return 0;
	return rt_api_select(&w->ctx, fd + 1, wr ? NULL : &set, wr ? &set : NULL, NULL, left, NULL);
}

/* Ask server sv the query q (length qlen, without OPT) over TCP (RFC
 * 7766: two byte length prefix). The answer, at most cap bytes (the rest
 * is dropped; then TC is set in it as BIND does), goes to w->tcpbuf.
 * Returns its length or -errno. */
static int dns_tcp(struct dns_work *w, const struct dns_server *sv, const u8 *q, u32 qlen, u32 cap)
{
	int v6 = !rt_in6_is_v4mapped(&sv->addr), fd, r;
	s32 one = 1, err;
	u32 deadline = rt_now_ms() + DNS_TCP_TIMEOUT_MS, got = 0, need, len = 0, el;
	u8 hdr[2];

	rt_free(w->tcpbuf);
	w->tcpbuf = NULL;
	fd = rt_api_socket(&w->ctx, v6 ? RT_AF_INET6 : RT_AF_INET, RT_SOCK_STREAM, 0);
	if (fd < 0)
		return fd;
	rt_api_ioctl(&w->ctx, fd, RT_FIONBIO, &one);
	if (v6) {
		struct rt_sockaddr_in6 to;
		memset(&to, 0, sizeof(to));
		to.sin6_len = sizeof(to);
		to.sin6_family = RT_AF_INET6;
		to.sin6_port = rt_htons(DNS_PORT);
		to.sin6_addr = sv->addr;
		to.sin6_scope_id = sv->ifindex;
		r = rt_api_connect(&w->ctx, fd, &to, sizeof(to));
	} else {
		struct rt_sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_len = sizeof(to);
		to.sin_family = RT_AF_INET;
		to.sin_port = rt_htons(DNS_PORT);
		to.sin_addr = rt_in6_v4(&sv->addr);
		r = rt_api_connect(&w->ctx, fd, &to, sizeof(to));
	}
	if (r < 0 && r != -RT_EINPROGRESS)
		goto out;
	if (r < 0) {
		r = sock_wait(w, fd, 1, deadline);
		if (r <= 0) {
			r = r < 0 ? r : -RT_ETIMEDOUT;
			goto out;
		}
		el = sizeof(err);
		err = 0;
		rt_api_getsockopt(&w->ctx, fd, RT_SOL_SOCKET, RT_SO_ERROR, &err, &el);
		if (err) {
			r = -(int)err;
			goto out;
		}
	}
	/* the query: length prefix and message, possibly in pieces */
	rt_put_be16(hdr, (u16)qlen);
	for (got = 0; got < qlen + 2; ) {
		const u8 *p = got < 2 ? hdr + got : q + (got - 2);
		u32 n = got < 2 ? 2 - got : qlen + 2 - got;
		r = rt_api_send(&w->ctx, fd, p, n, 0);
		if (r == -RT_EWOULDBLOCK) {
			if ((r = sock_wait(w, fd, 1, deadline)) <= 0) {
				r = r < 0 ? r : -RT_ETIMEDOUT;
				goto out;
			}
			continue;
		}
		if (r <= 0) {
			r = r < 0 ? r : -RT_ECONNRESET;
			goto out;
		}
		got += (u32)r;
	}
	/* the answer: length, then up to cap bytes of it */
	for (got = 0, need = 2; got < need; ) {
		u8 *p = got < 2 ? hdr + got : w->tcpbuf + (got - 2);
		u32 n = got < 2 ? 2 - got : need - got;
		r = rt_api_recv(&w->ctx, fd, p, n, 0);
		if (r == -RT_EWOULDBLOCK) {
			if ((r = sock_wait(w, fd, 0, deadline)) <= 0) {
				r = r < 0 ? r : -RT_ETIMEDOUT;
				goto out;
			}
			continue;
		}
		if (r <= 0) {
			r = r < 0 ? r : -RT_ECONNRESET;
			goto out;
		}
		got += (u32)r;
		if (got == 2 && !w->tcpbuf) {
			len = rt_get_be16(hdr);
			if (len < 12) {
				r = -RT_ECONNRESET;
				goto out;
			}
			w->tcpbuf = rt_malloc(len < cap ? len : cap);
			if (!w->tcpbuf) {
				r = -RT_ENOMEM;
				goto out;
			}
			need = 2 + (len < cap ? len : cap);
		}
	}
	r = (int)(need - 2);
	if (len > cap)
		w->tcpbuf[2] |= 0x02;          /* TC: the rest did not fit */
out:
	rt_api_close(&w->ctx, fd);
	rt_lock();
	if (r < 0)
		rt_dns_stat_tcp_fail++;
	else
		rt_dns_stat_tcp++;
	rt_unlock();
	RT_DBG(RT_DBG_DNS, "TCP query to server: %d", r);
	if (r < 0) {
		rt_free(w->tcpbuf);
		w->tcpbuf = NULL;
	}
	return r;
}

/* A socket of the server's family (opened on first use) bound to an
 * ephemeral port. */
static int dns_socket(struct dns_work *w, int v6, int *fd)
{
	struct rt_sockaddr_in sin;
	struct rt_sockaddr_in6 sin6;
	int r;

	if (*fd >= 0)
		return *fd;
	r = rt_api_socket(&w->ctx, v6 ? RT_AF_INET6 : RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (r < 0)
		return r;
	if (v6) {
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_len = sizeof(sin6);
		sin6.sin6_family = RT_AF_INET6;
		if (rt_api_bind(&w->ctx, r, &sin6, sizeof(sin6)) < 0) {
			rt_api_close(&w->ctx, r);
			return -RT_EADDRNOTAVAIL;
		}
	} else {
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = RT_AF_INET;
		if (rt_api_bind(&w->ctx, r, &sin, sizeof(sin)) < 0) {
			rt_api_close(&w->ctx, r);
			return -RT_EADDRNOTAVAIL;
		}
	}
	*fd = r;
	return r;
}

/* Send len bytes of q to server sv. */
static int send_q(struct dns_work *w, int fd, const struct dns_server *sv, const u8 *q, u32 len)
{
	if (!rt_in6_is_v4mapped(&sv->addr)) {
		struct rt_sockaddr_in6 to;
		memset(&to, 0, sizeof(to));
		to.sin6_len = sizeof(to);
		to.sin6_family = RT_AF_INET6;
		to.sin6_port = rt_htons(DNS_PORT);
		to.sin6_addr = sv->addr;
		to.sin6_scope_id = sv->ifindex;
		return rt_api_sendto(&w->ctx, fd, q, len, 0, &to, sizeof(to));
	} else {
		struct rt_sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_len = sizeof(to);
		to.sin_family = RT_AF_INET;
		to.sin_port = rt_htons(DNS_PORT);
		to.sin_addr = rt_in6_v4(&sv->addr);
		return rt_api_sendto(&w->ctx, fd, q, len, 0, &to, sizeof(to));
	}
}

/* Wait (until start + limit ms) for a datagram from server sv's port 53
 * carrying a response with our ID: its length in w->a, 0 on timeout,
 * -RT_EINTR on a break. */
static int recv_reply(struct dns_work *w, int fd, const struct dns_server *sv, u32 start, u32 limit,
                      u16 id, u16 id2)
{
	int v6 = !rt_in6_is_v4mapped(&sv->addr), r, ok;

	for (;;) {
		struct rt_sockaddr_in6 from;
		u32 el = rt_now_ms() - start, fl = sizeof(from), set;
		if (el >= limit)
			return 0;
		set = 1u << fd;
		r = rt_api_select(&w->ctx, fd + 1, &set, NULL, NULL, (s32)(limit - el), NULL);
		if (r == -RT_EINTR)
			return r;
		if (r <= 0)
			return 0;
		memset(&from, 0, sizeof(from));
		r = rt_api_recvfrom(&w->ctx, fd, w->a, w->alen, RT_MSG_DONTWAIT, &from, &fl);
		if (r < 12)
			continue;
		if (v6) {
			ok = rt_in6_eq(&from.sin6_addr, &sv->addr) && from.sin6_port == rt_htons(DNS_PORT);
		} else {
			const struct rt_sockaddr_in *f4 = (const struct rt_sockaddr_in *)(const void *)&from;
			ok = f4->sin_addr == rt_in6_v4(&sv->addr) && f4->sin_port == rt_htons(DNS_PORT);
		}
		if (ok && (w->a[2] & 0x80) && (rt_get_be16(w->a) == id || rt_get_be16(w->a) == id2))
			return r;
	}
}

/* rcode of a response */
#define RCODE(m) ((m)[3] & 0x0f)
#define TC(m)    (((m)[2] & 0x02) != 0)

/* Parse mode: the answer in w->a (length n) to our query, EDNS used or
 * not, from server s: remember a server without EDNS; a truncated one is
 * asked again over TCP (the plain query q, qlen bytes) when allowed.
 * Returns as dns_parse_response() into *res. */
static int take_answer(struct dns_work *w, int s, u32 n, int edns, u8 *q, u32 qlen, u16 id,
                       const char *qname, u16 qtype, struct dns_result *res)
{
	int r = dns_parse_response(w->a, n, id, qname, qtype, res);

	if (r == DNS_IGNORE)
		return r;
	if (edns && (RCODE(w->a) == 0 || RCODE(w->a) == 3) && dns_find_opt(w->a, n, NULL) == 0)
		server_noedns(w, s);
	if (TC(w->a) && w->tcp) {
		int t;
		q_edns(q, qlen, 0);
		t = dns_tcp(w, &w->servers[s], q, qlen, DNS_TCP_MAX);
		if (t > 0) {
			int r2 = dns_parse_response(w->tcpbuf, (u32)t, id, qname, qtype, res);
			if (r2 != DNS_IGNORE)
				return r2;
			r = dns_parse_response(w->a, n, id, qname, qtype, res);
		}
	}
	return r;
}

/* Raw mode: is the answer at m (n bytes) one to hand back, or should the
 * next server be asked (SERVFAIL/NOTIMP/REFUSED)? */
static int raw_ok(const u8 *m, int n, u16 id)
{
	u16 rc;

	if (n < 12 || rt_get_be16(m) != id || !(m[2] & 0x80))
		return 0;
	rc = RCODE(m);
	return rc != 2 && rc != 4 && rc != 5;
}

/* Send our query to every server, DNS_ATTEMPTS rounds.
 * Parse mode (qname != NULL): the query is w->q, qlen bytes without our
 * OPT record (which build_q() put after it); it goes with EDNS unless the
 * server cannot take it, and FORMERR/NOTIMP to EDNS gets it once more
 * without. The reply must answer (qname, qtype) and is parsed into w->res;
 * returns as dns_parse_response().
 * Raw mode (qname == NULL, res_send): the caller's query w->rawq goes
 * unchanged (UDP, or TCP when longer than 512 bytes, as BIND); any reply
 * with our ID from the server is accepted, w->ans points to it; returns
 * its length or -RT_TRY_AGAIN.
 * Both: a truncated (TC) answer is asked again over TCP from the same
 * server when dns.tcp_fallback is on; if that fails, it is used as is. */
static int dns_exchange(struct dns_work *w, u32 qlen, u16 id, const char *qname, u16 qtype)
{
	int fd4 = -1, fd6 = -1, att, s, r, result = -RT_TRY_AGAIN;

	w->intr = 0;
	w->sent = 0;
	if (w->nservers == 0)
		return -RT_TRY_AGAIN;
	for (att = 0; att < DNS_ATTEMPTS; att++) {
		for (s = 0; s < w->nservers; s++) {
			const struct dns_server *sv = &w->servers[s];
			int v6 = !rt_in6_is_v4mapped(&sv->addr), edns, fd;
			u32 start;
			if (!qname && qlen > DNS_UDP_MIN) {
				r = dns_tcp(w, sv, w->rawq, qlen, w->rawcap);
				if (r > 0 && raw_ok(w->tcpbuf, r, id)) {
					w->ans = w->tcpbuf;
					result = r;
					goto out;
				}
				continue;
			}
			fd = dns_socket(w, v6, v6 ? &fd6 : &fd4);
			if (fd < 0)
				continue;
			edns = qname && w->edns && !sv->noedns;
		resend:
			start = rt_now_ms();
			if (qname)
				r = send_q(w, fd, sv, w->q, q_edns(w->q, qlen, edns));
			else
				r = send_q(w, fd, sv, w->rawq, qlen);
			if (r < 0)
				continue;
			w->sent++;
			RT_DBG(RT_DBG_DNS, "query %s type %u to server %d, attempt %d%s", qname ? qname : "(raw)",
			       (unsigned)qtype, s + 1, att + 1, edns ? ", EDNS" : "");
			for (;;) {
				r = recv_reply(w, fd, sv, start, DNS_TIMEOUT_MS, id, id);
				if (r == -RT_EINTR) {
					w->intr = 1;
					result = -RT_TRY_AGAIN;
					goto out;
				}
				if (r <= 0) {
					RT_DBG(RT_DBG_DNS, "no answer from server %d", s + 1);
					break;
				}
				if (!qname) {
					int t;
					if (!raw_ok(w->a, r, id))
						break;      /* SERVFAIL/NOTIMP/REFUSED: next server */
					w->ans = w->a;
					result = r;
					if (TC(w->a) && w->tcp) {
						t = dns_tcp(w, sv, w->rawq, qlen, w->rawcap);
						if (t > 0 && raw_ok(w->tcpbuf, t, id)) {
							w->ans = w->tcpbuf;
							result = t;
						}
					}
					goto out;
				}
				if (edns && (RCODE(w->a) == 1 || RCODE(w->a) == 4)) {
					/* FORMERR/NOTIMP: perhaps the OPT record; once without */
					server_noedns(w, s);
					edns = 0;
					goto resend;
				}
				r = take_answer(w, s, (u32)r, edns, w->q, qlen, id, qname, qtype, &w->res);
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
	RT_DBG(RT_DBG_DNS, "query %s: result %d", qname ? qname : "(raw)", result);
	if (fd4 >= 0)
		rt_api_close(&w->ctx, fd4);
	if (fd6 >= 0)
		rt_api_close(&w->ctx, fd6);
	return result;
}

static int dns_query(struct dns_work *w, const char *name, u16 qtype)
{
	u16 id = (u16)rt_random32();
	int qlen = build_q(w, w->q, id, name, qtype);

	if (qlen < 0)
		return -RT_NO_RECOVERY;
	return dns_exchange(w, (u32)qlen, id, name, qtype);
}

/* After a query (name, qtype) that sent something and was not broken
 * off: remember a negative answer r (for negttl s) or a failure. */
static void neg_note(struct dns_work *w, const char *name, u16 qtype, int r, u32 negttl)
{
	if (w->intr || !w->sent)
		return;
	rt_lock();
	if (r == 0 || r == -RT_HOST_NOT_FOUND)
		neg_add(name, qtype, r, negttl);
	else if (r == -RT_TRY_AGAIN || r == -RT_NO_RECOVERY)
		neg_fail(name, qtype, r);
	rt_unlock();
}

/* dns_query() through the negative cache */
static int dns_query_neg(struct dns_work *w, const char *name, u16 qtype)
{
	int r;

	rt_lock();
	r = neg_find(name, qtype);
	rt_unlock();
	if (r != DNS_IGNORE) {
		RT_DBG(RT_DBG_DNS, "query %s type %u: cached negative %d", name, (unsigned)qtype, r);
		return r;
	}
	r = dns_query(w, name, qtype);
	neg_note(w, name, qtype, r, w->res.negttl);
	return r;
}

/* ---- hosts table and cache (lock held) ---- */

/* v6 = 0: IPv4 entries into a4; v6 = 1: IPv6 entries into a6 */
static int hosts_lookup(const char *name, const char *search, u32 *a4, rt_in6_addr *a6, int max)
{
	struct host_ent *h;
	int n = 0, i;
	u32 nl = (u32)strlen(name);
	int single = strchr(name, '.') == NULL;
	char d[64];

	if (nl && name[nl - 1] == '.')
		nl--;
	for (h = hosts_head; h && n < max; h = h->next) {
		u32 hl = (u32)strlen(h->name);
		int m = hl == nl && rt_strncasecmp(h->name, name, nl) == 0;
		/* a single label also matches NAME.DOMAIN for a search domain */
		for (i = 0; !m && single && hl > nl + 1 && h->name[nl] == '.' &&
		            rt_strncasecmp(h->name, name, nl) == 0 && search_nth(search, i, d, sizeof(d)); i++)
			m = rt_strcasecmp(h->name + nl + 1, d) == 0;
		if (!m || rt_in6_is_v4mapped(&h->addr) == (a6 != NULL))
			continue;
		if (a6)
			a6[n++] = h->addr;
		else
			a4[n++] = rt_in6_v4(&h->addr);
	}
	return n;
}

static struct cache_ent *cache_find(const char *name, u16 qtype)
{
	int i;

	if (!dns_cache_enabled)
		return NULL;
	for (i = 0; i < dns_cache_size && i < DNS_CACHE_MAX; i++) {
		struct cache_ent *c = &dns_cache[i];
		if (c->naddr && c->qtype == qtype && dns_name_eq(c->name, name)) {
			if ((s32)(c->expires - rt_now_ms()) <= 0) {
				c->naddr = 0;
				return NULL;
			}
			c->stamp = ++dns_cache_clock;
			dns_stat_hits++;
			return c;
		}
	}
	return NULL;
}

static void cache_add(const char *name, const char *canon, u16 qtype, const u32 *a4,
                      const rt_in6_addr *a6, int n, u32 ttl)
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
		if (!c->naddr || (c->qtype == qtype && dns_name_eq(c->name, name))) {
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
	victim->qtype = qtype;
	victim->naddr = (u8)RT_MIN(n, DNS_CACHE_ADDRS);
	for (i = 0; i < victim->naddr; i++) {
		if (a6)
			victim->addr[i] = a6[i];
		else
			rt_in6_set_v4mapped(&victim->addr[i], a4[i]);
	}
	victim->expires = rt_now_ms() + ttl * 1000u;
	victim->stamp = ++dns_cache_clock;
}

/* The negative cache entry of (name, qtype), expired or not; NULL if none */
static struct neg_ent *neg_lookup(const char *name, u16 qtype, int kind)
{
	int i;

	for (i = 0; i < DNS_NEG_MAX; i++) {
		struct neg_ent *e = &dns_neg[i];
		if (e->kind && (kind ? e->kind == kind : 1) &&
		    (e->kind == NEG_NXDOMAIN || e->qtype == qtype) && dns_name_eq(e->name, name))
			return e;
	}
	return NULL;
}

/* A cached answer for the query (name, qtype): 0 (NODATA),
 * -RT_HOST_NOT_FOUND, -RT_TRY_AGAIN / -RT_NO_RECOVERY (failure), or
 * DNS_IGNORE when there is none (lock held). */
static int neg_find(const char *name, u16 qtype)
{
	u32 now = rt_now_ms();
	int i;

	if (!dns_cache_enabled)
		return DNS_IGNORE;
	for (i = 0; i < DNS_NEG_MAX; i++) {
		struct neg_ent *e = &dns_neg[i];
		if (!e->kind || (e->kind != NEG_NXDOMAIN && e->qtype != qtype) ||
		    (s32)(e->expires - now) <= 0 || !dns_name_eq(e->name, name))
			continue;
		e->stamp = ++dns_cache_clock;
		if (e->kind == NEG_FAIL) {
			dns_stat_fail_hits++;
			return e->err;
		}
		dns_stat_neg_hits++;
		return e->kind == NEG_NXDOMAIN ? -RT_HOST_NOT_FOUND : 0;
	}
	return DNS_IGNORE;
}

/* a slot for (name, qtype): its own, an unused or expired one, else the
 * least recently used */
static struct neg_ent *neg_slot(const char *name, u16 qtype)
{
	struct neg_ent *e, *victim = NULL;
	u32 now = rt_now_ms();
	int i;

	for (i = 0; i < DNS_NEG_MAX; i++) {
		e = &dns_neg[i];
		if (e->kind && e->qtype == qtype && dns_name_eq(e->name, name))
			return e;
	}
	for (i = 0; i < DNS_NEG_MAX; i++) {
		e = &dns_neg[i];
		if (!e->kind || (s32)(e->expires - now) <= 0)
			return e;
		if (!victim || (s32)(e->stamp - victim->stamp) < 0)
			victim = e;
	}
	return victim;
}

/* Remember the result r of the query (name, qtype) when it is negative:
 * 0 (NODATA) or -RT_HOST_NOT_FOUND for negttl seconds (lock held). */
static void neg_add(const char *name, u16 qtype, int r, u32 negttl)
{
	struct neg_ent *e;
	u16 t = r == -RT_HOST_NOT_FOUND ? 0 : qtype;

	if (!dns_cache_enabled || (r != 0 && r != -RT_HOST_NOT_FOUND) || strlen(name) >= DNS_CACHE_NAME)
		return;
	if (negttl > (u32)dns_neg_maxttl)
		negttl = (u32)dns_neg_maxttl;
	if (negttl == 0)
		return;
	e = neg_slot(name, t);
	rt_strlcpy(e->name, name, DNS_CACHE_NAME);
	e->qtype = t;
	e->kind = r ? NEG_NXDOMAIN : NEG_NODATA;
	e->fails = 0;
	e->expires = rt_now_ms() + negttl * 1000u;
	e->stamp = ++dns_cache_clock;
}

/* The query (name, qtype) failed with err (-RT_TRY_AGAIN or
 * -RT_NO_RECOVERY): no new query for 1, 2, 4, ... seconds, at most
 * dns.cache.fail_max; a failure long after the last one starts at 1 s
 * again (lock held). */
static void neg_fail(const char *name, u16 qtype, int err)
{
	struct neg_ent *e;
	u32 now = rt_now_ms(), secs;

	if (!dns_cache_enabled || dns_fail_max <= 0 || strlen(name) >= DNS_CACHE_NAME)
		return;
	e = neg_lookup(name, qtype, NEG_FAIL);
	if (e && e->qtype == qtype && (s32)(now - e->expires) < dns_fail_max * 1000)
		e->fails = (u8)RT_MIN(e->fails + 1, 16);
	else if ((e = neg_slot(name, qtype)) != NULL)
		e->fails = 0;
	if (!e)
		return;
	secs = 1u << RT_MIN(e->fails, 9);
	if (secs > (u32)dns_fail_max)
		secs = (u32)dns_fail_max;
	rt_strlcpy(e->name, name, DNS_CACHE_NAME);
	e->qtype = qtype;
	e->kind = NEG_FAIL;
	e->err = (s16)err;
	e->expires = now + secs * 1000u;
	e->stamp = ++dns_cache_clock;
}

/* Forget the failures (the servers changed) or everything (lock held). */
static void neg_flush(int fail_only)
{
	int i;

	for (i = 0; i < DNS_NEG_MAX; i++)
		if (!fail_only || dns_neg[i].kind == NEG_FAIL)
			dns_neg[i].kind = 0;
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

/* ---- .local names: multicast DNS (RFC 6762 5.1 one-shot queries) ---- */

#define MDNS_WAIT_MS 1000        /* per attempt, two attempts */

static int is_mdns_name(const char *name)
{
	u32 l = (u32)strlen(name);

	if (l && name[l - 1] == '.')
		l--;
	return l > 6 && !rt_strncasecmp(name + l - 6, ".local", 6);
}

/* our own .local name: the interface addresses (lock held) */
static int mdns_self(const char *name, u32 *a4, rt_in6_addr *a6, int max)
{
	const char *h = rt_mdns_hostname();
	u32 hl = (u32)strlen(h);
	struct rt_netif *nif;
	struct rt_in6_ifaddr_info ai[8];
	int n = 0, i, na;

	if (!hl || rt_strncasecmp(name, h, hl) || !is_mdns_name(name) || name[hl] != '.' ||
	    (name[hl + 6] && name[hl + 6] != '.'))
		return 0;
	if (!a6) {
		for (nif = rt_netif_list(); nif && n < max; nif = nif->next) {
			struct rt_ifaddr4 ia[1 + RT_IF_MAXADDR4];
			int k, c;
			if (nif->flags & RT_IFF_LOOPBACK)
				continue;
			c = rt_netif_addrs(nif, ia, 1 + RT_IF_MAXADDR4);
			for (k = 0; k < c && n < max; k++)
				a4[n++] = ia[k].addr;
		}
		return n;
	}
	na = rt_in6_addr_list(NULL, ai, 8);
	for (i = 0; i < na && i < 8 && n < max; i++)
		if (ai[i].scope == RT_IN6_SCOPE_GLOBAL &&
		    !(ai[i].flags & (RT_IN6_IFF_TENTATIVE | RT_IN6_IFF_DUPLICATED)))
			a6[n++] = ai[i].addr;
	return n;
}

static int mdns_resolve(struct dns_work *w, const char *name, u16 qtype, u32 *a4, rt_in6_addr *a6,
                        int max, u32 *ttl)
{
	struct rt_sockaddr_in sin;
	int fd, att, r, qlen, result = -RT_HOST_NOT_FOUND;

	fd = rt_api_socket(&w->ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (fd < 0)
		return -RT_TRY_AGAIN;
	for (att = 0; att < 2; att++) {
		u16 id = (u16)rt_random32();
		u32 start = rt_now_ms();
		qlen = dns_build_query(w->q, sizeof(w->q), id, name, qtype);
		if (qlen < 0) {
			result = -RT_NO_RECOVERY;
			break;
		}
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = RT_AF_INET;
		sin.sin_port = rt_htons(5353);
		sin.sin_addr = rt_htonl(0xe00000fbu);
		r = rt_api_sendto(&w->ctx, fd, w->q, (u32)qlen, 0, &sin, sizeof(sin));
		if (r < 0) {
			result = -RT_TRY_AGAIN;
			break;
		}
		RT_DBG(RT_DBG_DNS, "mdns query %s type %u, attempt %d", name, (unsigned)qtype, att + 1);
		for (;;) {
			struct rt_sockaddr_in from;
			u32 el = rt_now_ms() - start, fl = sizeof(from), set = 1u << fd;
			if (el >= MDNS_WAIT_MS)
				break;
			r = rt_api_select(&w->ctx, fd + 1, &set, NULL, NULL, (s32)(MDNS_WAIT_MS - el), NULL);
			if (r == -RT_EINTR) {
				att = 2;
				result = -RT_TRY_AGAIN;
				break;
			}
			if (r <= 0)
				break;
			r = rt_api_recvfrom(&w->ctx, fd, w->a, w->alen, RT_MSG_DONTWAIT, &from, &fl);
			if (r < 0 || from.sin_port != rt_htons(5353))
				continue;
			r = mdns_parse_answer(w->a, (u32)r, id, name, qtype, a4, a6, max, ttl);
			if (r > 0) {
				result = r;
				att = 2;
				break;
			}
		}
	}
	rt_api_close(&w->ctx, fd);
	return result;
}

/* Answers without the network: the hosts table, localhost, our own .local
 * name, the cache. Count, 0 for none, or -RT_HOST_NOT_FOUND when DNS is
 * disallowed (RiptideDisallowDNS) and nothing local matched. */
static int resolve_local(const char *name, u16 qtype, u32 *a4, rt_in6_addr *a6, int max,
                         char *canon, u32 canonlen)
{
	struct cache_ent *c;
	int n, i, disallowed;

	rt_lock();
	n = hosts_lookup(name, dns_search, a4, a6, max);
	if (n == 0 && is_localhost(name)) {
		if (a6)
			a6[0] = rt_in6addr_loopback;
		else
			a4[0] = RT_INADDR_LOOPBACK;
		n = 1;
	}
	if (n == 0)
		n = mdns_self(name, a4, a6, max);
	if (n == 0 && (c = cache_find(name, qtype)) != NULL) {
		n = RT_MIN(max, (int)c->naddr);
		for (i = 0; i < n; i++) {
			if (a6)
				a6[i] = c->addr[i];
			else
				a4[i] = rt_in6_v4(&c->addr[i]);
		}
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
	return disallowed ? -RT_HOST_NOT_FOUND : 0;
}

/* the names to ask for (ndots:1): the name itself and NAME.DOMAIN for
 * every search domain */
static int candidates(struct dns_work *w, const char *name)
{
	u32 nl = (u32)strlen(name);

	return name[nl - 1] == '.' ? 1 : 1 + search_count(w->search);
}

/* candidate i of name into w->qname; 0, or -1 when it does not fit */
static int candidate(struct dns_work *w, const char *name, int i, int ncand)
{
	u32 nl = (u32)strlen(name), dots = 0, k;
	int dom;
	char d[64];

	for (k = 0; k < nl; k++)
		dots += name[k] == '.';
	/* with a dot the name itself first, else the search domains first */
	dom = ncand == 1 ? -1 : dots ? i - 1 : (i < ncand - 1 ? i : -1);
	if (dom >= 0) {
		if (!search_nth(w->search, dom, d, sizeof(d)) || nl + 1 + strlen(d) >= DNS_NAMEBUF)
			return -1;
		rt_snprintf(w->qname, sizeof(w->qname), "%s.%s", name, d);
	} else {
		rt_strlcpy(w->qname, name, sizeof(w->qname));
	}
	return 0;
}

/* Forward lookup of qtype A (a4) or AAAA (a6). */
static int resolve(const char *name, u16 qtype, u32 *a4, rt_in6_addr *a6, int max,
                   char *canon, u32 canonlen)
{
	struct dns_work *w;
	u32 nl;
	int n, r, i, ncand, best = -RT_HOST_NOT_FOUND;

	nl = (u32)strlen(name);
	if (nl == 0 || nl > 254)
		return -RT_HOST_NOT_FOUND;
	n = resolve_local(name, qtype, a4, a6, max, canon, canonlen);
	if (n != 0)
		return n;

	w = work_alloc(a4, max);
	if (!w)
		return -RT_TRY_AGAIN;
	w->res.addrs6 = a6;
	if (is_mdns_name(name)) {
		u32 ttl = 0;
		r = mdns_resolve(w, name, qtype, a4, a6, max, &ttl);
		if (r > 0) {
			rt_lock();
			cache_add(name, name, qtype, a4, a6, r, ttl);
			rt_unlock();
			put_canon(canon, canonlen, name);
		}
		work_free(w);
		return r;
	}
	ncand = candidates(w, name);
	for (i = 0; i < ncand; i++) {
		if (candidate(w, name, i, ncand) < 0)
			continue;
		r = dns_query_neg(w, w->qname, qtype);
		if (r > 0) {
			rt_lock();
			cache_add(name, w->res.canon, qtype, a4, a6, r, w->res.ttl);
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

/* ---- A and AAAA at once (Happy Eyeballs v2, RFC 8305 3) ---- */

s32 rt_he_resolution_delay = 50;

#define R_PENDING (-2000)

/* is r a final answer for its type (data, no data, no such name)? */
static int final(int r)
{
	return r >= 0 || r == -RT_HOST_NOT_FOUND;
}

/* Both queries for w->qname to each server in turn: A into w->res, AAAA
 * into w->res6. Done when both have a final answer, or the A records are
 * in and the AAAA answer is rt_he_resolution_delay ms late (RFC 8305 3:
 * a slow AAAA must not hold up IPv4); a positive AAAA answer waits for the
 * A answer as usual (the IPv4 fallback needs it). The caller sets *r4
 * and *r6 to R_PENDING, or to the answer from the negative cache: that
 * query is then not sent. */
static void dns_exchange46(struct dns_work *w, int *r4, int *r6)
{
	int fd4 = -1, fd6 = -1, att, s, r, pre4 = *r4 != R_PENDING, pre6 = *r6 != R_PENDING;
#define DONE4 (pre4 || final(*r4))
#define DONE6 (pre6 || final(*r6))
	u16 id4 = (u16)rt_random32(), id6 = (u16)(id4 ^ 0x5a5a);
	int l4 = build_q(w, w->q, id4, w->qname, DNS_TYPE_A);
	int l6 = build_q(w, w->q6, id6, w->qname, DNS_TYPE_AAAA);

	w->intr = 0;
	w->sent = 0;
	if (l4 < 0 || l6 < 0 || w->nservers == 0) {
		if (*r4 == R_PENDING)
			*r4 = l4 < 0 || l6 < 0 ? -RT_NO_RECOVERY : -RT_TRY_AGAIN;
		if (*r6 == R_PENDING)
			*r6 = l4 < 0 || l6 < 0 ? -RT_NO_RECOVERY : -RT_TRY_AGAIN;
		return;
	}
	for (att = 0; att < DNS_ATTEMPTS; att++) {
		for (s = 0; s < w->nservers; s++) {
			const struct dns_server *sv = &w->servers[s];
			int v6 = !rt_in6_is_v4mapped(&sv->addr), k, edns;
			u32 start, got4 = 0;
			int fd = dns_socket(w, v6, v6 ? &fd6 : &fd4);
			if (fd < 0)
				continue;
			edns = w->edns && !sv->noedns;
		resend:
			start = rt_now_ms();
			/* AAAA first, as RFC 8305 3 asks */
			for (k = 0; k < 2; k++) {
				int *rr = k ? r4 : r6;
				u8 *q = k ? w->q : w->q6;
				u32 ql = (u32)(k ? l4 : l6);
				if (k ? DONE4 : DONE6)
					continue;
				*rr = R_PENDING;
				if (send_q(w, fd, sv, q, q_edns(q, ql, edns)) >= 0)
					w->sent++;
			}
			RT_DBG(RT_DBG_DNS, "query %s A+AAAA to server %d, attempt %d%s", w->qname, s + 1, att + 1,
			       edns ? ", EDNS" : "");
			for (;;) {
				u32 now = rt_now_ms(), el = now - start, limit = DNS_TIMEOUT_MS;
				if (DONE4 && DONE6)
					goto out;
				if (*r4 > 0 && got4 && !final(*r6) && *r6 == R_PENDING &&
				    got4 - start + (u32)rt_he_resolution_delay < limit)
					limit = got4 - start + (u32)rt_he_resolution_delay;
				if (el >= limit) {
					if (*r4 > 0)
						goto out;   /* AAAA late: go with the A records */
					break;
				}
				r = recv_reply(w, fd, sv, start, limit, id4, id6);
				if (r == -RT_EINTR) {
					w->intr = 1;
					goto out;
				}
				if (r <= 0)
					continue;
				if (edns && (RCODE(w->a) == 1 || RCODE(w->a) == 4) &&
				    ((rt_get_be16(w->a) == id4 && *r4 == R_PENDING) ||
				     (rt_get_be16(w->a) == id6 && *r6 == R_PENDING))) {
					/* FORMERR/NOTIMP: perhaps the OPT record; once without */
					server_noedns(w, s);
					edns = 0;
					goto resend;
				}
				if (rt_get_be16(w->a) == id4 && *r4 == R_PENDING) {
					int x = take_answer(w, s, (u32)r, edns, w->q, (u32)l4, id4, w->qname,
					                    DNS_TYPE_A, &w->res);
					if (x != DNS_IGNORE) {
						*r4 = x;
						if (x > 0)
							got4 = rt_now_ms();
					}
				} else if (rt_get_be16(w->a) == id6 && *r6 == R_PENDING) {
					int x = take_answer(w, s, (u32)r, edns, w->q6, (u32)l6, id6, w->qname,
					                    DNS_TYPE_AAAA, &w->res6);
					if (x != DNS_IGNORE)
						*r6 = x;
				}
				/* SERVFAIL & co. for both: the next server */
				if (*r4 != R_PENDING && *r6 != R_PENDING && !(DONE4 && DONE6) &&
				    !(*r4 > 0))
					break;
			}
			if (*r4 > 0 || *r6 > 0)
				goto out;
		}
	}
out:
	if (*r4 == R_PENDING)
		*r4 = -RT_TRY_AGAIN;
	if (*r6 == R_PENDING)
		*r6 = -RT_TRY_AGAIN;
	RT_DBG(RT_DBG_DNS, "query %s: A %d, AAAA %d", w->qname, *r4, *r6);
	if (fd4 >= 0)
		rt_api_close(&w->ctx, fd4);
	if (fd6 >= 0)
		rt_api_close(&w->ctx, fd6);
#undef DONE4
#undef DONE6
}

/* the stronger of two failures (getaddrinfo's EAI_* mapping) */
static int worse(int a, int b)
{
	if (a == -RT_TRY_AGAIN || b == -RT_TRY_AGAIN)
		return -RT_TRY_AGAIN;
	if (a == -RT_NO_RECOVERY || b == -RT_NO_RECOVERY)
		return -RT_NO_RECOVERY;
	if (a == 0 || b == 0 || a == -RT_NO_DATA || b == -RT_NO_DATA)
		return -RT_NO_DATA;
	return -RT_HOST_NOT_FOUND;
}

int rt_resolve_name46(const char *name, rt_in6_addr *addrs, int max, int family, char *canon, u32 canonlen)
{
	u32 a4[8], nl;
	rt_in6_addr a6[8];
	int n4 = 0, n6 = 0, e4 = -RT_NO_DATA, e6 = -RT_NO_DATA, n = 0, i;
	int want4 = family != RT_AF_INET6, want6 = family != RT_AF_INET;
	struct dns_work *w;
	char c4[DNS_NAMEBUF];

	if (!name || !addrs || max <= 0)
		return -RT_NO_RECOVERY;
	nl = (u32)strlen(name);
	if (nl == 0 || nl > 254)
		return -RT_HOST_NOT_FOUND;
	c4[0] = 0;
	if (canon && canonlen)
		canon[0] = 0;
	if (want6) {
		n6 = resolve_local(name, DNS_TYPE_AAAA, NULL, a6, 8, canon, canonlen);
		if (n6 < 0)
			want6 = 0, e6 = n6, n6 = 0;
	}
	if (want4) {
		n4 = resolve_local(name, DNS_TYPE_A, a4, NULL, 8, c4, sizeof(c4));
		if (n4 < 0)
			want4 = 0, e4 = n4, n4 = 0;
	}
	if (n6 > 0)
		want6 = 0;
	if (n4 > 0)
		want4 = 0;
	if (want4 && want6 && !is_mdns_name(name)) {
		/* nothing local: both queries in parallel */
		w = work_alloc(a4, 8);
		if (!w)
			return -RT_TRY_AGAIN;
		w->res6.addrs6 = a6;
		w->res6.max = 8;
		e4 = e6 = -RT_HOST_NOT_FOUND;
		{
			int ncand = candidates(w, name), r4, r6;
			for (i = 0; i < ncand; i++) {
				int k4, k6;
				if (candidate(w, name, i, ncand) < 0)
					continue;
				rt_lock();
				k4 = neg_find(w->qname, DNS_TYPE_A);
				k6 = neg_find(w->qname, DNS_TYPE_AAAA);
				rt_unlock();
				if (k4 != DNS_IGNORE && k6 != DNS_IGNORE) {
					r4 = k4;
					r6 = k6;
				} else {
					/* only the type without a cached answer is asked */
					r4 = k4 = k4 != DNS_IGNORE ? k4 : R_PENDING;
					r6 = k6 = k6 != DNS_IGNORE ? k6 : R_PENDING;
					dns_exchange46(w, &r4, &r6);
					if (k4 == R_PENDING && (final(r4) || !final(r6)))
						neg_note(w, w->qname, DNS_TYPE_A, r4, w->res.negttl);
					if (k6 == R_PENDING && (final(r6) || !final(r4)))
						neg_note(w, w->qname, DNS_TYPE_AAAA, r6, w->res6.negttl);
				}
				if (r4 > 0 || r6 > 0) {
					rt_lock();
					if (r4 > 0)
						cache_add(name, w->res.canon, DNS_TYPE_A, a4, NULL, r4, w->res.ttl);
					if (r6 > 0)
						cache_add(name, w->res6.canon, DNS_TYPE_AAAA, NULL, a6, r6, w->res6.ttl);
					rt_unlock();
					n4 = r4 > 0 ? r4 : 0;
					n6 = r6 > 0 ? r6 : 0;
					put_canon(c4, sizeof(c4), r6 > 0 ? w->res6.canon : w->res.canon);
					break;
				}
				e4 = worse(e4, r4);
				e6 = worse(e6, r6);
			}
		}
		work_free(w);
	} else {
		if (want6) {
			n6 = resolve(name, DNS_TYPE_AAAA, NULL, a6, 8, canon, canonlen);
			if (n6 < 0)
				e6 = n6, n6 = 0;
		}
		if (want4) {
			n4 = resolve(name, DNS_TYPE_A, a4, NULL, 8, c4, sizeof(c4));
			if (n4 < 0)
				e4 = n4, n4 = 0;
		}
	}
	for (i = 0; i < n6 && n < max; i++)
		addrs[n++] = a6[i];
	for (i = 0; i < n4 && n < max; i++)
		rt_in6_set_v4mapped(&addrs[n++], a4[i]);
	if (canon && canonlen && !canon[0])
		rt_strlcpy(canon, c4[0] ? c4 : name, canonlen);
	return n ? n : worse(e4, e6);
}

int rt_resolve_name(const char *name, u32 *addrs, int max, char *canon, u32 canonlen)
{
	u32 a;

	if (!name || !addrs || max <= 0)
		return -RT_NO_RECOVERY;
	if (rt_parse_ipv4(name, &a) == 0) {
		addrs[0] = a;
		put_canon(canon, canonlen, name);
		return 1;
	}
	return resolve(name, DNS_TYPE_A, addrs, NULL, max, canon, canonlen);
}

int rt_resolve_name6(const char *name, rt_in6_addr *addrs, int max, char *canon, u32 canonlen)
{
	rt_in6_addr a;
	int plen;

	if (!name || !addrs || max <= 0)
		return -RT_NO_RECOVERY;
	if (rt_inet6_parse_prefix(name, &a, &plen, NULL, 0) == 0 && plen < 0) {
		addrs[0] = a;
		put_canon(canon, canonlen, name);
		return 1;
	}
	{
		u32 v4;
		if (rt_parse_ipv4(name, &v4) == 0)
			return -RT_NO_DATA;   /* an IPv4 literal has no AAAA */
	}
	return resolve(name, DNS_TYPE_AAAA, NULL, addrs, max, canon, canonlen);
}

static int resolve_ptr(const char *qname, char *name, u32 namelen)
{
	struct dns_work *w = work_alloc(NULL, 0);
	int r;

	if (!w)
		return -RT_TRY_AGAIN;
	rt_strlcpy(w->qname, qname, sizeof(w->qname));
	r = dns_query_neg(w, w->qname, DNS_TYPE_PTR);
	if (r > 0) {
		rt_strlcpy(name, w->res.name, namelen);
		r = 0;
	} else if (r == 0) {
		r = -RT_NO_DATA;
	}
	work_free(w);
	return r;
}

static int hosts_reverse(const rt_in6_addr *a, char *name, u32 namelen)
{
	struct host_ent *h;

	for (h = hosts_head; h; h = h->next) {
		if (rt_in6_eq(&h->addr, a)) {
			rt_strlcpy(name, h->name, namelen);
			return 1;
		}
	}
	return 0;
}

int rt_resolve_addr(u32 addr, char *name, u32 namelen)
{
	rt_in6_addr a;
	char q[32];
	u32 ha = rt_ntohl(addr);
	int found, disallowed;

	if (!name || namelen == 0)
		return -RT_NO_RECOVERY;
	rt_in6_set_v4mapped(&a, addr);
	rt_lock();
	found = hosts_reverse(&a, name, namelen);
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
	rt_snprintf(q, sizeof(q), "%u.%u.%u.%u.in-addr.arpa",
	            (unsigned)(ha & 255), (unsigned)((ha >> 8) & 255),
	            (unsigned)((ha >> 16) & 255), (unsigned)(ha >> 24));
	return resolve_ptr(q, name, namelen);
}

int rt_resolve_addr6(const rt_in6_addr *addr, char *name, u32 namelen)
{
	static const char hex[] = "0123456789abcdef";
	char *q;
	int i, n = 0, found, disallowed, r;

	if (!addr || !name || namelen == 0)
		return -RT_NO_RECOVERY;
	if (rt_in6_is_v4mapped(addr))
		return rt_resolve_addr(rt_in6_v4(addr), name, namelen);
	rt_lock();
	found = hosts_reverse(addr, name, namelen);
	disallowed = rt_dns_disallowed;
	rt_unlock();
	if (found)
		return 0;
	if (rt_in6_is_loopback(addr)) {
		rt_strlcpy(name, "localhost", namelen);
		return 0;
	}
	if (disallowed)
		return -RT_HOST_NOT_FOUND;
	q = rt_malloc(80);
	if (!q)
		return -RT_TRY_AGAIN;
	/* RFC 3596 2.5: nibbles in reverse order under ip6.arpa */
	for (i = 15; i >= 0; i--) {
		q[n++] = hex[addr->b[i] & 15];
		q[n++] = '.';
		q[n++] = hex[addr->b[i] >> 4];
		q[n++] = '.';
	}
	memcpy(q + n, "ip6.arpa", 9);
	r = resolve_ptr(q, name, namelen);
	rt_free(q);
	return r;
}

/* ======================================================================
 * BIND style raw interface (res_mkquery/res_send/res_query/res_search,
 * dn_expand/dn_skipname) for library clients. Called WITHOUT the lock.
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
	u16 size;

	if (!msg || msglen < 12 || msglen > 0xffff || !answer || anslen <= 0)
		return -RT_NO_RECOVERY;
	rt_lock();
	disallowed = rt_dns_disallowed;
	rt_unlock();
	if (disallowed)
		return -RT_TRY_AGAIN;
	w = work_alloc(NULL, 0);
	if (!w)
		return -RT_TRY_AGAIN;
	/* the caller's own OPT record may ask for larger UDP answers */
	if (dns_find_opt(msg, (u32)msglen, &size) == 1 && size > w->alen) {
		u8 *a = rt_malloc(size < DNS_EDNS_MAX ? size : DNS_EDNS_MAX);
		if (a) {
			rt_free(w->a);
			w->a = w->ans = a;
			w->alen = size < DNS_EDNS_MAX ? size : DNS_EDNS_MAX;
		}
	}
	w->rawq = msg;
	w->rawcap = (u32)anslen;
	r = dns_exchange(w, (u32)msglen, rt_get_be16(msg), NULL, 0);
	if (r > 0) {
		if (r > anslen)
			r = anslen;               /* truncated copy, as BIND */
		memcpy(answer, w->ans, (u32)r);
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

/* domain i of the search list (copied under the lock); 0 = none */
static int search_domain(int i, char *out, u32 outlen)
{
	int r;

	rt_lock();
	r = search_nth(dns_search, i, out, outlen);
	rt_unlock();
	return r;
}

int rt_res_search(const char *dname, int cls, int type, u8 *answer, int anslen)
{
	char d[64];
	u32 nl;
	int r = -RT_HOST_NOT_FOUND, r2, i;

	if (!dname || !*dname)
		return -RT_NO_RECOVERY;
	nl = (u32)strlen(dname);
	if (dname[nl - 1] == '.' || !search_domain(0, d, sizeof(d)))
		return rt_res_query(dname, cls, type, answer, anslen);
	if (strchr(dname, '.')) {          /* ndots:1 - absolute first */
		r = rt_res_query(dname, cls, type, answer, anslen);
		if (r >= 0 || r == -RT_TRY_AGAIN)
			return r;
	}
	for (i = 0; search_domain(i, d, sizeof(d)); i++) {
		r2 = rt_res_querydomain(dname, d, cls, type, answer, anslen);
		if (r2 >= 0 || r2 == -RT_TRY_AGAIN)
			return r2;
		if (i == 0 && !strchr(dname, '.'))
			r = r2;
	}
	if (!strchr(dname, '.')) {
		r2 = rt_res_query(dname, cls, type, answer, anslen);
		return r2 >= 0 ? r2 : r;
	}
	return r;
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
 * "dns" command
 * ====================================================================== */

int rt_dns_command(int argc, char **argv, char *out, u32 outlen)
{
	static const char usage[] = "usage: dns edns_size N (0 = off, 512..4096) | edns on|off | "
	                            "tcp on|off | negttl N (0..86400) | failttl N (0..300) | "
	                            "flush | reset | status";
	const char *v = argc >= 3 ? argv[2] : NULL, *e;
	unsigned long n;
	int i, k;

	if (out && outlen)
		out[0] = 0;
	if (argc < 2 || !rt_strcasecmp(argv[1], "status")) {
		for (i = k = 0; i < dns_nservers; i++)
			k += dns_servers[i].noedns && (s32)(dns_servers[i].noedns_until - rt_now_ms()) > 0;
		if (rt_dns_edns_size)
			rt_snprintf(out, outlen, "EDNS0 %ld bytes, ", (long)(rt_dns_edns_size < DNS_UDP_MIN ?
			            DNS_UDP_MIN : rt_dns_edns_size));
		else
			rt_snprintf(out, outlen, "EDNS0 off, ");
		i = (int)strlen(out);
		rt_snprintf(out + i, outlen > (u32)i ? outlen - (u32)i : 0,
		            "TCP fallback %s; %d server(s) without EDNS; %lu TCP queries, %lu failed\n",
		            rt_dns_tcp ? "on" : "off", k, (unsigned long)rt_dns_stat_tcp,
		            (unsigned long)rt_dns_stat_tcp_fail);
		{
			u32 now = rt_now_ms();
			int np = 0, nn = 0, nf = 0;
			for (i = 0; i < dns_cache_size && i < DNS_CACHE_MAX; i++)
				np += dns_cache[i].naddr && (s32)(dns_cache[i].expires - now) > 0;
			for (i = 0; i < DNS_NEG_MAX; i++) {
				if (!dns_neg[i].kind || (s32)(dns_neg[i].expires - now) <= 0)
					continue;
				if (dns_neg[i].kind == NEG_FAIL)
					nf++;
				else
					nn++;
			}
			i = (int)strlen(out);
			rt_snprintf(out + i, outlen > (u32)i ? outlen - (u32)i : 0,
			            "cache %s: %d names, %d negative (up to %ld s), %d failed (up to %ld s); "
			            "%lu hits, %lu negative, %lu failed\n", dns_cache_enabled ? "on" : "off",
			            np, nn, (long)dns_neg_maxttl, nf, (long)dns_fail_max,
			            (unsigned long)dns_stat_hits, (unsigned long)dns_stat_neg_hits,
			            (unsigned long)dns_stat_fail_hits);
		}
		return 0;
	}
	if (argc == 3 && (!rt_strcasecmp(argv[1], "negttl") || !rt_strcasecmp(argv[1], "failttl"))) {
		int neg = !rt_strcasecmp(argv[1], "negttl");
		n = rt_strtoul(v, &e, 10);
		if (*e || e == v || n > (neg ? 86400ul : 300ul)) {
			rt_snprintf(out, outlen, neg ? "dns: negttl is 0 (off) to 86400 seconds" :
			                               "dns: failttl is 0 (off) to 300 seconds");
			return -RT_EINVAL;
		}
		if (neg)
			dns_neg_maxttl = (s32)n;
		else
			dns_fail_max = (s32)n;
		return 0;
	}
	if (argc == 2 && !rt_strcasecmp(argv[1], "flush")) {
		memset(dns_cache, 0, sizeof(dns_cache));
		neg_flush(0);
		return 0;
	}
	if (argc == 3 && (!rt_strcasecmp(argv[1], "edns_size") || !rt_strcasecmp(argv[1], "edns"))) {
		if (!rt_strcasecmp(v, "on") || !rt_strcasecmp(v, "off")) {
			rt_dns_edns_size = !rt_strcasecmp(v, "on") ? 1232 : 0;
			return 0;
		}
		n = rt_strtoul(v, &e, 10);
		if (*e || e == v || (n && (n < DNS_UDP_MIN || n > DNS_EDNS_MAX))) {
			rt_snprintf(out, outlen, "dns: edns_size is 0 (off) or 512..4096");
			return -RT_EINVAL;
		}
		rt_dns_edns_size = (s32)n;
		return 0;
	}
	if (argc == 3 && !rt_strcasecmp(argv[1], "tcp") &&
	    (!rt_strcasecmp(v, "on") || !rt_strcasecmp(v, "off"))) {
		rt_dns_tcp = !rt_strcasecmp(v, "on");
		return 0;
	}
	if (argc == 2 && !rt_strcasecmp(argv[1], "reset")) {
		/* forget which servers lack EDNS */
		for (i = 0; i < dns_nservers; i++)
			dns_servers[i].noedns = 0;
		return 0;
	}
	rt_snprintf(out, outlen, "%s", usage);
	return -RT_EINVAL;
}

/* ======================================================================
 * sysctl (CTL_DNS 9, dns.cache 0)
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
		if (v) {
			memset(dns_cache, 0, sizeof(dns_cache));
			neg_flush(0);
		}
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
	{ "dns.cache.neg_maxttl", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 0, 3 },
	  &dns_neg_maxttl, 0, 86400, NULL, "longest time NXDOMAIN / NODATA answers are kept, s (0 = never)" },
	{ "dns.cache.fail_max", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 0, 4 },
	  &dns_fail_max, 0, 300, NULL, "longest time a failed lookup is not repeated, s (0 = never)" },
	{ "dns.edns_size", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 1, 0 },
	  &rt_dns_edns_size, 0, DNS_EDNS_MAX, NULL, "EDNS0 UDP payload size (0 = no EDNS)" },
	{ "dns.tcp_fallback", RT_SYSCTL_INT, RT_SYSCTL_RW, 3, { 9, 1, 1 },
	  &rt_dns_tcp, 0, 1, NULL, "ask truncated answers again over TCP" },
	{ "dns.stats.tcp", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 0 },
	  &rt_dns_stat_tcp, 0, 0, NULL, "queries answered over TCP" },
	{ "dns.stats.tcp_failed", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 1 },
	  &rt_dns_stat_tcp_fail, 0, 0, NULL, "TCP queries that failed" },
	{ "dns.stats.noedns", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 2 },
	  &rt_dns_stat_noedns, 0, 0, NULL, "servers found without EDNS" },
	{ "dns.stats.cache_hits", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 3 },
	  &dns_stat_hits, 0, 0, NULL, "lookups answered from the cache" },
	{ "dns.stats.neg_hits", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 4 },
	  &dns_stat_neg_hits, 0, 0, NULL, "queries answered by a cached NXDOMAIN / NODATA" },
	{ "dns.stats.fail_hits", RT_SYSCTL_INT, RT_SYSCTL_RO, 3, { 9, 2, 5 },
	  &dns_stat_fail_hits, 0, 0, NULL, "queries not repeated after a recent failure" },
};

void rt_resolv_init(void)
{
	rt_sysctl_register(resolv_sysctls, (int)RT_ARRAY_SIZE(resolv_sysctls));
}
