/*
 * Riptide - host: DNS lookup utility (BIND host(1) style output).
 *
 *   host [-v] [-t TYPE] [-W secs] NAME|ADDR [SERVER]
 *
 * Without -t and SERVER, names are looked up with gethostbyname() (and
 * gethostbyname2(AF_INET6) of riptide.library for the IPv6 addresses) and
 * addresses (IPv4 or IPv6) with gethostbyaddr() (static host table, cache
 * and DNS, as every program sees them). With -t (A, NS, CNAME, SOA, PTR,
 * MX, TXT, AAAA, ANY) or a SERVER, host sends its own DNS query over UDP
 * (IPv4 or IPv6) to SERVER or the first configured name server, with an
 * EDNS0 OPT record (1232 bytes; again without it when the server answers
 * FORMERR or NOTIMP); a truncated answer is asked again over TCP.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"

#define DNSQ    320           /* query: header, name, question, OPT */
#define DNSBUF  1232          /* UDP answers: our EDNS0 size */

static u32 wait_ms = 5000;
static int verbose;

static void usage(void)
{
	cl_puts("Usage: host [-v] [-t type] [-W time] {name} [server]\n"
	        "       -t specifies the query type (A, NS, CNAME, SOA, PTR, MX, TXT, AAAA, ANY)\n"
	        "       -W specifies how long to wait for a reply (seconds)\n");
}

static const struct { const char *name; u16 type; } types[] = {
	{ "A", 1 }, { "NS", 2 }, { "CNAME", 5 }, { "SOA", 6 }, { "PTR", 12 }, { "MX", 15 },
	{ "TXT", 16 }, { "AAAA", 28 }, { "ANY", 255 },
};

static const char *rcode_name(int r)
{
	static const char *const n[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED" };
	return r >= 0 && r <= 5 ? n[r] : "UNKNOWN";
}

/* RFC 3596 2.5: nibbles in reverse order under ip6.arpa */
static void ptr_name6(const u8 *a, char *b, u32 len)
{
	static const char hx[] = "0123456789abcdef";
	u32 o = 0;
	int i;

	for (i = 15; i >= 0 && o + 4 < len; i--) {
		b[o++] = hx[a[i] & 15];
		b[o++] = '.';
		b[o++] = hx[a[i] >> 4];
		b[o++] = '.';
	}
	rt_strlcpy(b + o, "ip6.arpa", len - o);
}

static void ptr_name(u32 addr, char *b, u32 len)
{
	u32 h = rt_ntohl(addr);

	rt_snprintf(b, len, "%lu.%lu.%lu.%lu.in-addr.arpa", (unsigned long)(h & 255),
	            (unsigned long)((h >> 8) & 255), (unsigned long)((h >> 16) & 255),
	            (unsigned long)(h >> 24));
}

/* Expand a (possibly compressed) name at p; returns bytes consumed at p or -1. */
static int dn_expand(const u8 *msg, u32 mlen, const u8 *p, char *out, u32 outlen)
{
	const u8 *end = msg + mlen, *start = p;
	int consumed = -1, hops = 0;
	u32 o = 0;

	for (;;) {
		u8 l;
		if (p >= end)
			return -1;
		l = *p;
		if ((l & 0xc0) == 0xc0) {
			if (p + 1 >= end || ++hops > 20)
				return -1;
			if (consumed < 0)
				consumed = (int)(p + 2 - start);
			p = msg + (((u32)(l & 0x3f) << 8) | p[1]);
			continue;
		}
		if (l & 0xc0)
			return -1;
		p++;
		if (!l)
			break;
		if (p + l > end || o + l + 2 > outlen)
			return -1;
		if (o)
			out[o++] = '.';
		memcpy(out + o, p, l);
		o += l;
		p += l;
	}
	if (!o && outlen > 1)
		out[o++] = '.';
	out[o] = 0;
	if (consumed < 0)
		consumed = (int)(p - start);
	return consumed;
}

/* Build a standard query. Returns its length. */
static u32 build_query(u8 *b, const char *name, u16 type, u16 id)
{
	u32 o = 12;
	const char *p = name;

	memset(b, 0, 12);
	rt_put_be16(b, id);
	b[2] = 0x01;                      /* RD */
	rt_put_be16(b + 4, 1);
	while (*p) {
		const char *dot = strchr(p, '.');
		u32 l = dot ? (u32)(dot - p) : (u32)strlen(p);
		if (l > 63 || o + l + 6 > 300)
			return 0;
		if (l) {
			b[o++] = (u8)l;
			memcpy(b + o, p, l);
			o += l;
		}
		if (!dot)
			break;
		p = dot + 1;
	}
	b[o++] = 0;
	rt_put_be16(b + o, type);
	rt_put_be16(b + o + 2, 1);        /* IN */
	return o + 4;
}

static void print_rr(const u8 *m, u32 mlen, u32 off, const char *owner, u16 type, u16 rdlen)
{
	char n1[256], n2[256], a[RT_INET6_ADDRSTRLEN];
	const u8 *rd = m + off;
	u32 i;

	switch (type) {
	case 1:
		if (rdlen == 4) {
			u32 v;
			memcpy(&v, rd, 4);
			cl_printf("%s has address %s\n", owner, cl_ip(v, a));
		}
		break;
	case 28:
		if (rdlen == 16)
			cl_printf("%s has IPv6 address %s\n", owner, cl_ip6(rd, a));
		break;
	case 2: case 5: case 12:
		if (dn_expand(m, mlen, rd, n1, sizeof(n1)) < 0)
			break;
		if (type == 2)
			cl_printf("%s name server %s.\n", owner, n1);
		else if (type == 5)
			cl_printf("%s is an alias for %s.\n", owner, n1);
		else
			cl_printf("%s domain name pointer %s.\n", owner, n1);
		break;
	case 15:
		if (rdlen > 2 && dn_expand(m, mlen, rd + 2, n1, sizeof(n1)) >= 0)
			cl_printf("%s mail is handled by %lu %s.\n", owner, (unsigned long)rt_get_be16(rd), n1);
		break;
	case 16:
		cl_printf("%s descriptive text", owner);
		for (i = 0; i < rdlen;) {
			u32 l = rd[i++], k;
			cl_puts(" \"");
			for (k = 0; k < l && i < rdlen; k++, i++) {
				char ch[2];
				ch[0] = rd[i] >= 32 && rd[i] < 127 ? (char)rd[i] : '?';
				ch[1] = 0;
				cl_puts(ch);
			}
			cl_puts("\"");
		}
		cl_puts("\n");
		break;
	case 6: {
		int k = dn_expand(m, mlen, rd, n1, sizeof(n1)), k2;
		if (k < 0)
			break;
		k2 = dn_expand(m, mlen, rd + k, n2, sizeof(n2));
		if (k2 < 0 || (u32)(k + k2 + 20) > rdlen)
			break;
		rd += k + k2;
		cl_printf("%s has SOA record %s. %s. %lu %lu %lu %lu %lu\n", owner, n1, n2,
		          (unsigned long)rt_get_be32(rd), (unsigned long)rt_get_be32(rd + 4),
		          (unsigned long)rt_get_be32(rd + 8), (unsigned long)rt_get_be32(rd + 12),
		          (unsigned long)rt_get_be32(rd + 16));
		break;
	}
	default:
		cl_printf("%s has TYPE%u record (%u bytes)\n", owner, (unsigned)type, (unsigned)rdlen);
	}
}

/* UDP: send q (qlen bytes) and wait for the answer with our ID in r
 * (DNSBUF). Its length, -1 no answer, -2 Ctrl-C. */
static LONG udp_ask(int s, u8 *q, u32 qlen, u8 *r, u16 id)
{
	LONG n = -1;
	u32 try;

	for (try = 0; try < 2; try++) {
		u32 t0 = cl_now_us();
		send(s, q, (LONG)qlen, 0);
		for (;;) {
			u32 el = cl_now_us() - t0;
			int w;
			if (el >= wait_ms * 1000UL)
				break;
			w = cl_wait_fd(s, 0, (wait_ms * 1000UL - el + 999) / 1000);
			if (w < 0)
				return -2;
			if (w == 0)
				continue;
			n = recv(s, r, DNSBUF, 0);
			if (n >= 12 && rt_get_be16(r) == id && (r[2] & 0x80))
				return n;
		}
	}
	return -1;
}

static int recv_all(int s, u8 *b, u32 len)
{
	u32 got = 0;

	while (got < len) {
		LONG n;
		if (cl_wait_fd(s, 0, wait_ms) <= 0)
			return -1;
		n = recv(s, b + got, (LONG)(len - got), 0);
		if (n <= 0)
			return -1;
		got += (u32)n;
	}
	return 0;
}

/* TCP (RFC 7766): q (qlen bytes) with a two byte length; the answer in
 * a new buffer (*rp, FreeVec) or NULL. Its length or -1. */
static LONG tcp_ask(const struct cl_addr *server, u8 *q, u32 qlen, u8 **rp)
{
	u8 hdr[2], *r;
	u32 len;
	int s = socket(server->family, RT_SOCK_STREAM, 0);

	*rp = NULL;
	if (s < 0)
		return -1;
	if (cl_connect_addr(s, server, 53, wait_ms) != 0) {
		CloseSocket(s);
		return -1;
	}
	rt_put_be16(hdr, (u16)qlen);
	if (send(s, hdr, 2, 0) != 2 || send(s, q, (LONG)qlen, 0) != (LONG)qlen ||
	    recv_all(s, hdr, 2) < 0 || (len = rt_get_be16(hdr)) < 12 ||
	    !(r = AllocVec(len, MEMF_ANY))) {
		CloseSocket(s);
		return -1;
	}
	if (recv_all(s, r, len) < 0) {
		FreeVec(r);
		CloseSocket(s);
		return -1;
	}
	CloseSocket(s);
	*rp = r;
	return (LONG)len;
}

static int query(const char *name, u16 type, const struct cl_addr *server)
{
	struct rt_sockaddr_in6 sa;
	u32 salen;
	u8 *q, *r, *tcpbuf = NULL;
	u32 qlen;
	LONG n = -1;
	int s, rc = CL_ERROR;
	u16 id;

	q = AllocVec(DNSQ + DNSBUF, MEMF_ANY);
	if (!q) {
		cl_error("out of memory");
		return CL_FAIL;
	}
	r = q + DNSQ;
	id = (u16)(cl_now_us() ^ ((u32)FindTask(NULL) >> 3));
	qlen = build_query(q, name, type, id);
	if (!qlen) {
		cl_printf("host: '%s' is not a legal name\n", name);
		FreeVec(q);
		return CL_ERROR;
	}
	/* EDNS0 OPT record (RFC 6891): root, type 41, our UDP size */
	memset(q + qlen, 0, 11);
	rt_put_be16(q + qlen + 1, 41);
	rt_put_be16(q + qlen + 3, DNSBUF);
	rt_put_be16(q + 10, 1);
	s = socket(server->family, RT_SOCK_DGRAM, 0);
	salen = cl_sockaddr(server, 53, &sa);
	if (s < 0 || connect(s, &sa, (LONG)salen) < 0) {
		cl_error("cannot reach the name server");
		goto out;
	}
	n = udp_ask(s, q, qlen + 11, r, id);
	if (n >= 12 && ((r[3] & 15) == 1 || (r[3] & 15) == 4)) {
		/* FORMERR/NOTIMP: an old server; again without EDNS */
		if (verbose)
			cl_puts(";; no EDNS: retrying without\n");
		rt_put_be16(q + 10, 0);
		n = udp_ask(s, q, qlen, r, id);
	}
	if (n == -2) {
		cl_puts("***Break\n");
		goto out;
	}
	if (n < 12) {
		cl_printf(";; connection timed out; no servers could be reached\n");
		goto out;
	}
	if (r[2] & 0x02) {
		LONG t;
		cl_puts(";; Truncated, retrying in TCP mode.\n");
		rt_put_be16(q + 10, 0);
		t = tcp_ask(server, q, qlen, &tcpbuf);
		if (t >= 12 && rt_get_be16(tcpbuf) == id && (tcpbuf[2] & 0x80)) {
			r = tcpbuf;
			n = t;
		} else if (verbose) {
			cl_puts(";; TCP failed: the truncated answer\n");
		}
	}
	if (verbose)
		cl_printf(";; ->>HEADER<<- opcode: QUERY, status: %s, id: %u\n;; flags: qr%s%s%s%s; "
		          "QUERY: %u, ANSWER: %u, AUTHORITY: %u, ADDITIONAL: %u\n\n",
		          rcode_name(r[3] & 15), (unsigned)id, (r[2] & 4) ? " aa" : "", (r[2] & 2) ? " tc" : "",
		          (r[2] & 1) ? " rd" : "", (r[3] & 0x80) ? " ra" : "", (unsigned)rt_get_be16(r + 4),
		          (unsigned)rt_get_be16(r + 6), (unsigned)rt_get_be16(r + 8), (unsigned)rt_get_be16(r + 10));
	if ((r[3] & 15) != 0) {
		cl_printf("Host %s not found: %d(%s)\n", name, r[3] & 15, rcode_name(r[3] & 15));
		goto out;
	}
	{
		u32 off = 12, qd = rt_get_be16(r + 4), an = rt_get_be16(r + 6), i;
		char owner[256];
		for (i = 0; i < qd; i++) {
			int k = dn_expand(r, (u32)n, r + off, owner, sizeof(owner));
			if (k < 0)
				goto bad;
			off += (u32)k + 4;
		}
		if (!an) {
			cl_printf("%s has no %s record\n", name, type == 1 ? "A" : type == 28 ? "AAAA" : "such");
			rc = CL_OK;
			goto out;
		}
		for (i = 0; i < an; i++) {
			u16 t, rdlen;
			int k = dn_expand(r, (u32)n, r + off, owner, sizeof(owner));
			if (k < 0 || off + (u32)k + 10 > (u32)n)
				goto bad;
			off += (u32)k;
			t = rt_get_be16(r + off);
			rdlen = rt_get_be16(r + off + 8);
			off += 10;
			if (off + rdlen > (u32)n)
				goto bad;
			print_rr(r, (u32)n, off, owner, t, rdlen);
			off += rdlen;
		}
		rc = CL_OK;
		goto out;
	}
bad:
	cl_puts(";; malformed answer\n");
out:
	if (s >= 0)
		CloseSocket(s);
	if (tcpbuf)
		FreeVec(tcpbuf);
	FreeVec(q);
	return rc;
}

static int host_main(int argc, char **argv)
{
	struct cl_getopt g;
	const char *name, *tname = NULL;
	u32 addr, i;
	struct cl_addr server, a6;
	u16 type = 0;
	int c, isaddr, is6 = 0, have_server = 0;
	char b[80], a[CL_ADDRSTRLEN];

	memset(&g, 0, sizeof(g));
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage();
		return CL_OK;
	}
	while ((c = cl_getopt(argc, argv, "vadt:W:46", &g)) != -1) {
		switch (c) {
		case 'v': case 'd': verbose = 1; break;
		case 'a': verbose = 1; tname = "ANY"; break;
		case 't': tname = g.arg; break;
		case 'W': if (cl_parse_ms(g.arg, &wait_ms) < 0 || !wait_ms) { usage(); return CL_ERROR; } break;
		case '4': case '6': break;
		default: usage(); return CL_ERROR;
		}
	}
	if (g.ind >= argc) {
		usage();
		return CL_ERROR;
	}
	name = argv[g.ind];
	if (tname) {
		for (i = 0; i < RT_ARRAY_SIZE(types); i++)
			if (!rt_strcasecmp(tname, types[i].name))
				type = types[i].type;
		if (!type) {
			cl_printf("host: invalid type: %s\n", tname);
			return CL_ERROR;
		}
	}
	if (cl_need_socket() < 0 || cl_need_timer() < 0)
		return CL_FAIL;
	isaddr = cl_parse_ip(name, &addr) == 0;
	if (!isaddr && cl_parse_addr(name, &a6) == 0 && a6.family == RT_AF_INET6)
		isaddr = is6 = 1;
	if (g.ind + 1 < argc) {
		if (cl_resolve_af(argv[g.ind + 1], 0, &server) < 0)
			return CL_ERROR;
		have_server = 1;
		cl_printf("Using domain server:\nName: %s\nAddress: %s#53\nAliases: \n\n", argv[g.ind + 1],
		          cl_addr_str(&server, a));
		if (!type)
			type = isaddr ? 12 : 1;
	}
	if (type) {
		char q[256];
		if (!have_server) {
			/* the first server of either family (net.riptide.dns6), else the IPv4 list */
			struct rt_ctl_hdr h;
			void *d = cl_dump(RT_CTL_DNS6, &h);
			memset(&server, 0, sizeof(server));
			if (d) {
				struct rt_ctl_dns6 *dn = cl_rec(d, &h, 0);
				if (dn->nservers) {
					memcpy(server.a.b, dn->servers[0].addr, 16);
					server.family = rt_in6_is_v4mapped(&server.a) ? RT_AF_INET : RT_AF_INET6;
					server.scope = dn->servers[0].ifindex;
					have_server = 1;
				}
				FreeVec(d);
			} else if ((d = cl_dump(RT_CTL_DNS, &h)) != NULL) {
				struct rt_ctl_dns *dn = cl_rec(d, &h, 0);
				if (dn->nservers) {
					server.family = RT_AF_INET;
					rt_in6_set_v4mapped(&server.a, dn->servers[0]);
					have_server = 1;
				}
				FreeVec(d);
			}
			if (!have_server) {
				cl_puts(";; no name servers configured\n");
				return CL_ERROR;
			}
		}
		if (isaddr && type == 12 && is6)
			ptr_name6(a6.a.b, q, sizeof(q));
		else if (isaddr && type == 12)
			ptr_name(addr, q, sizeof(q));
		else
			rt_strlcpy(q, name, sizeof(q));
		return query(q, type, &server);
	}
	if (is6) {
		struct hostent *h = (struct hostent *)gethostbyaddr((APTR)a6.a.b, 16, RT_AF_INET6);
		ptr_name6(a6.a.b, b, sizeof(b));
		if (!h || !h->h_name) {
			cl_printf("Host %s. not found: 3(NXDOMAIN)\n", b);
			return CL_ERROR;
		}
		cl_printf("%s domain name pointer %s.\n", b, h->h_name);
		return CL_OK;
	}
	if (isaddr) {
		struct hostent *h = (struct hostent *)gethostbyaddr((APTR)&addr, 4, RT_AF_INET);
		ptr_name(addr, b, sizeof(b));
		if (!h || !h->h_name) {
			cl_printf("Host %s. not found: 3(NXDOMAIN)\n", b);
			return CL_ERROR;
		}
		cl_printf("%s domain name pointer %s.\n", b, h->h_name);
		return CL_OK;
	}
	{
		struct hostent *h = (struct hostent *)gethostbyname((CONST_STRPTR)name);
		int found = 0, alias = 0;
		if (h && h->h_addr_list && h->h_addr_list[0]) {
			if (h->h_name && rt_strcasecmp(h->h_name, name)) {
				cl_printf("%s is an alias for %s.\n", name, h->h_name);
				alias = 1;
			}
			rt_strlcpy(b, h->h_name ? h->h_name : name, sizeof(b));
			for (i = 0; h->h_addr_list[i]; i++) {
				memcpy(&addr, h->h_addr_list[i], 4);
				cl_printf("%s has address %s\n", b, cl_ip(addr, a));
			}
			found = 1;
		}
		/* AAAA through riptide.library (the hostent buffer is reused) */
		if (cl_need_riptide() == 0) {
			h = (struct hostent *)gethostbyname2((CONST_STRPTR)name, RT_AF_INET6);
			if (h && h->h_length == 16 && h->h_addr_list && h->h_addr_list[0]) {
				if (!alias && h->h_name && rt_strcasecmp(h->h_name, name))
					cl_printf("%s is an alias for %s.\n", name, h->h_name);
				for (i = 0; h->h_addr_list[i]; i++)
					cl_printf("%s has IPv6 address %s\n", h->h_name ? h->h_name : name,
					          cl_ip6((const u8 *)h->h_addr_list[i], a));
				found = 1;
			}
		}
		if (!found) {
			cl_printf("Host %s not found: 3(NXDOMAIN)\n", name);
			return CL_ERROR;
		}
	}
	return CL_OK;
}

CL_MAIN(host_main, "host")
