/*
 * Riptide - IPv6 address text forms (RFC 4291 2.2, RFC 5952) and the
 * address helpers of rt_ip6.h. Self contained (only rt_types.h), so the
 * network tools link it without the rest of the core.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "rt/rt_ip6.h"

const rt_in6_addr rt_in6addr_any = { { 0 } };
const rt_in6_addr rt_in6addr_loopback = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };

static int hexdig(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* dotted quad at s (exactly to the end of the string or 'end'), 4 bytes */
static int parse_quad(const char *s, const char *end, u8 *out)
{
	int part;

	for (part = 0; part < 4; part++) {
		u32 v = 0;
		int digits = 0;
		while (s < end && *s >= '0' && *s <= '9') {
			if (digits && v == 0)
				return -1;          /* no leading zeros (inet_pton) */
			v = v * 10 + (u32)(*s - '0');
			if (++digits > 3 || v > 255)
				return -1;
			s++;
		}
		if (!digits)
			return -1;
		out[part] = (u8)v;
		if (part < 3) {
			if (s >= end || *s != '.')
				return -1;
			s++;
		}
	}
	return s == end ? 0 : -1;
}

static int pton_range(const char *s, const char *end, rt_in6_addr *out)
{
	u8 a[16];
	int n = 0, gap = -1;
	const char *p = s;

	memset(a, 0, sizeof(a));
	if (p == end)
		return -1;
	if (*p == ':') {
		if (p + 1 >= end || p[1] != ':')
			return -1;          /* a lone leading ':' */
	}
	while (p < end) {
		const char *q;
		u32 v = 0;
		int d, digits = 0;

		if (*p == ':') {
			if (p + 1 < end && p[1] == ':') {
				if (gap >= 0)
					return -1;  /* two "::" */
				gap = n;
				p += 2;
				if (p == end)
					break;
				if (*p == ':')
					return -1;  /* ":::" */
				continue;
			}
			if (n == 0 && gap < 0)
				return -1;
			p++;
			if (p == end)
				return -1;      /* trailing single ':' */
		}
		/* a group, or the trailing dotted quad */
		for (q = p; q < end && *q != ':'; q++)
			if (*q == '.')
				break;
		if (q < end && *q == '.') {
			if (n > 12)
				return -1;
			if (parse_quad(p, end, a + n) < 0)
				return -1;
			n += 4;
			p = end;
			break;
		}
		while (p < end && (d = hexdig(*p)) >= 0) {
			v = (v << 4) | (u32)d;
			if (++digits > 4)
				return -1;
			p++;
		}
		if (!digits || (p < end && *p != ':'))
			return -1;
		if (n >= 16)
			return -1;
		a[n++] = (u8)(v >> 8);
		a[n++] = (u8)v;
	}
	if (gap >= 0) {
		int tail = n - gap;
		if (n == 16)
			return -1;          /* "::" must stand for at least one group */
		memmove(a + 16 - tail, a + gap, (size_t)tail);
		memset(a + gap, 0, (size_t)(16 - tail - gap));
	} else if (n != 16) {
		return -1;
	}
	memcpy(out->b, a, 16);
	return 0;
}

int rt_inet6_pton(const char *s, rt_in6_addr *out)
{
	if (!s || !out)
		return -1;
	return pton_range(s, s + strlen(s), out);
}

int rt_inet6_parse_prefix(const char *s, rt_in6_addr *out, int *plen, char *zone, u32 zonelen)
{
	const char *end, *pct, *slash;
	u32 zl;

	if (!s || !out)
		return -1;
	end = s + strlen(s);
	slash = strchr(s, '/');
	if (plen)
		*plen = -1;
	if (slash) {
		const char *d = slash + 1;
		int v = 0, digits = 0;
		if (!plen)
			return -1;
		while (*d >= '0' && *d <= '9') {
			v = v * 10 + (*d - '0');
			if (++digits > 3 || v > 128)
				return -1;
			d++;
		}
		if (!digits || *d)
			return -1;
		*plen = v;
		end = slash;
	}
	pct = s;
	while (pct < end && *pct != '%')
		pct++;
	if (zone && zonelen)
		zone[0] = 0;
	if (pct < end) {
		zl = (u32)(end - pct - 1);
		if (!zone || zl == 0 || zl >= zonelen)
			return -1;
		memcpy(zone, pct + 1, zl);
		zone[zl] = 0;
		end = pct;
	}
	return pton_range(s, end, out);
}

char *rt_inet6_ntop(const rt_in6_addr *a, char *buf, u32 len)
{
	char tmp[RT_INET6_ADDRSTRLEN];
	int best = -1, bestlen = 0, run = -1, i, n = 0;
	int quad = 0;

	if (!buf || len == 0)
		return NULL;
	/* RFC 5952 5: IPv4-mapped (and the old compatible form, as BSD and
	 * glibc print it) end in a dotted quad. */
	if (rt_in6_is_v4mapped(a))
		quad = 1;
	else if ((a->w[0] | a->w[1] | a->w[2]) == 0 && (a->b[12] | a->b[13]) != 0)
		quad = 1;
	for (i = 0; i < (quad ? 6 : 8); i++) {
		if (a->h[i] == 0) {
			if (run < 0)
				run = i;
			if (i - run + 1 > bestlen) {
				best = run;
				bestlen = i - run + 1;
			}
		} else {
			run = -1;
		}
	}
	if (bestlen < 2)
		best = -1;                  /* a single zero group is not shortened */
	for (i = 0; i < (quad ? 6 : 8);) {
		if (i == best) {
			tmp[n++] = ':';
			tmp[n++] = ':';
			i += bestlen;
			continue;
		}
		if (n && tmp[n - 1] != ':')
			tmp[n++] = ':';
		n += rt_snprintf(tmp + n, sizeof(tmp) - (u32)n, "%x", (unsigned)rt_get_be16(a->b + 2 * i));
		i++;
	}
	if (quad) {
		if (n && tmp[n - 1] != ':')
			tmp[n++] = ':';
		n += rt_snprintf(tmp + n, sizeof(tmp) - (u32)n, "%u.%u.%u.%u", a->b[12], a->b[13],
		                 a->b[14], a->b[15]);
	}
	tmp[n] = 0;
	if ((u32)n >= len)
		return NULL;
	memcpy(buf, tmp, (u32)n + 1);
	return buf;
}

/* ---- helpers ---- */

int rt_in6_scope(const rt_in6_addr *a)
{
	if (rt_in6_is_mcast(a))
		return rt_in6_mc_scope(a);
	if (rt_in6_is_linklocal(a) || rt_in6_is_loopback(a))
		return RT_IN6_SCOPE_LINK;
	if (rt_in6_is_v4mapped(a)) {
		/* RFC 6724 3.2: 127/8 and 169.254/16 link-local, else global */
		if (a->b[12] == 127 || (a->b[12] == 169 && a->b[13] == 254))
			return RT_IN6_SCOPE_LINK;
		return RT_IN6_SCOPE_GLOBAL;
	}
	if (rt_in6_is_sitelocal(a))
		return RT_IN6_SCOPE_SITE;
	return RT_IN6_SCOPE_GLOBAL;
}

int rt_in6_prefix_eq(const rt_in6_addr *a, const rt_in6_addr *b, int plen)
{
	int bytes = plen >> 3, bits = plen & 7;

	if (plen <= 0)
		return 1;
	if (plen > 128)
		plen = 128;
	if (bytes && memcmp(a->b, b->b, (size_t)bytes) != 0)
		return 0;
	if (bits && bytes < 16) {
		u8 m = (u8)(0xff << (8 - bits));
		return ((a->b[bytes] ^ b->b[bytes]) & m) == 0;
	}
	return 1;
}

int rt_in6_common_prefix(const rt_in6_addr *a, const rt_in6_addr *b)
{
	int i, n = 0;

	for (i = 0; i < 16; i++) {
		u8 x = a->b[i] ^ b->b[i];
		if (!x) {
			n += 8;
			continue;
		}
		while (!(x & 0x80)) {
			n++;
			x <<= 1;
		}
		break;
	}
	return n;
}

void rt_in6_mask(rt_in6_addr *out, const rt_in6_addr *a, int plen)
{
	int i;

	for (i = 0; i < 16; i++) {
		int bits = plen - i * 8;
		u8 m = bits >= 8 ? 0xff : bits <= 0 ? 0 : (u8)(0xff << (8 - bits));
		out->b[i] = a->b[i] & m;
	}
}

void rt_in6_solnode(rt_in6_addr *out, const rt_in6_addr *a)
{
	u8 lo[3];

	memcpy(lo, a->b + 13, 3);
	memset(out, 0, sizeof(*out));
	out->b[0] = 0xff;
	out->b[1] = 0x02;
	out->b[11] = 0x01;
	out->b[12] = 0xff;
	memcpy(out->b + 13, lo, 3);
}

void rt_in6_mc_mac(u8 *mac, const rt_in6_addr *g)
{
	mac[0] = 0x33;
	mac[1] = 0x33;
	memcpy(mac + 2, g->b + 12, 4);
}
