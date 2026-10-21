/*
 * Riptide - speedtest: a small streaming XML reader. It only reports start
 * tags and their attributes, which is all the speedtest.net configuration
 * and server list carry; the input can arrive in pieces of any size.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "speedtest.h"

enum { X_TEXT, X_TAG, X_COMMENT, X_CDATA };

void st_xml_init(struct st_xml *x, st_xml_cb cb, void *arg)
{
	memset(x, 0, sizeof(*x));
	x->cb = cb;
	x->arg = arg;
	x->state = X_TEXT;
}

static int is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

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

static u32 put_utf8(char *d, u32 cp)
{
	if (cp < 0x80) {
		d[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		d[0] = (char)(0xc0 | (cp >> 6));
		d[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		d[0] = (char)(0xe0 | (cp >> 12));
		d[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		d[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	d[0] = (char)(0xf0 | (cp >> 18));
	d[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	d[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	d[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* One entity at s[0] == '&' (n bytes available): its value in out, the
 * length of the entity text in *used. 0 if it is not a known entity. */
static u32 entity(const char *s, u32 n, char *out, u32 *used)
{
	static const struct { const char *name; char c; } named[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' }
	};
	u32 i, cp = 0, digits = 0;

	if (n >= 3 && s[1] == '#') {
		int hex = s[2] == 'x' || s[2] == 'X';
		for (i = hex ? 3 : 2; i < n && i < 12; i++) {
			int d = hex ? hexdig(s[i]) : (s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : -1);
			if (d < 0)
				break;
			cp = cp * (hex ? 16 : 10) + (u32)d;
			digits++;
		}
		if (!digits || i >= n || s[i] != ';' || cp == 0 || cp > 0x10ffff)
			return 0;
		*used = i + 1;
		return put_utf8(out, cp);
	}
	for (i = 0; i < RT_ARRAY_SIZE(named); i++) {
		u32 l = (u32)strlen(named[i].name);
		if (n > l && !memcmp(s + 1, named[i].name, l)) {
			*used = l + 1;
			out[0] = named[i].c;
			return 1;
		}
	}
	return 0;
}

u32 st_xml_unescape(char *s, u32 n)
{
	u32 r = 0, w = 0;

	while (r < n) {
		char tmp[4];
		u32 used, k;
		if (s[r] == '&' && (k = entity(s + r, n - r, tmp, &used)) != 0) {
			/* the encoding is never longer than the entity text */
			memcpy(s + w, tmp, k);
			w += k;
			r += used;
		} else {
			s[w++] = s[r++];
		}
	}
	return w;
}

const char *st_xml_attr(int nattr, const struct st_xml_attr *a, const char *name)
{
	int i;

	for (i = 0; i < nattr; i++)
		if (!strcmp(a[i].name, name))
			return a[i].value;
	return NULL;
}

/* A complete tag in x->buf (without '<' and '>'). */
static void tag_done(struct st_xml *x)
{
	struct st_xml_attr attr[ST_XML_MAXATTR];
	char *s = x->buf, *end = x->buf + x->len, *name;
	int n = 0;

	*end = 0;
	if (x->len == 0 || *s == '/' || *s == '?' || *s == '!')
		return;
	if (end[-1] == '/')                    /* <empty/> */
		*--end = 0;
	name = s;
	while (s < end && !is_ws(*s))
		s++;
	if (s < end)
		*s++ = 0;
	if (!*name)
		return;
	while (s < end) {
		char *an, *av, q;
		while (s < end && is_ws(*s))
			s++;
		if (s >= end)
			break;
		an = s;
		while (s < end && !is_ws(*s) && *s != '=')
			s++;
		q = *s;
		*s++ = 0;
		if (q != '=') {
			while (s < end && is_ws(*s))
				s++;
			if (s < end && *s == '=') {
				q = '=';
				s++;
			}
		}
		av = end;                          /* "" for an attribute without value */
		if (q == '=') {
			while (s < end && is_ws(*s))
				s++;
			if (s < end && (*s == '"' || *s == '\'')) {
				char *v;
				q = *s++;
				v = s;
				while (s < end && *s != q)
					s++;
				v[st_xml_unescape(v, (u32)(s - v))] = 0;
				if (s < end)
					s++;
				av = v;
			} else {
				char *v = s;
				while (s < end && !is_ws(*s))
					s++;
				if (s < end)
					*s++ = 0;
				v[st_xml_unescape(v, (u32)strlen(v))] = 0;
				av = v;
			}
		}
		if (n < ST_XML_MAXATTR && *an) {
			attr[n].name = an;
			attr[n].value = av;
			n++;
		}
	}
	x->tags++;
	x->cb(x->arg, name, n, attr);
}

void st_xml_feed(struct st_xml *x, const char *p, u32 n)
{
	const char *e = p + n;

	while (p < e) {
		char c;
		if (x->state == X_TEXT) {
			while (p < e && *p != '<')
				p++;
			if (p == e)
				return;
			p++;
			x->state = X_TAG;
			x->len = 0;
			x->quote = 0;
			x->overflow = 0;
			continue;
		}
		c = *p++;
		if (x->state == X_COMMENT || x->state == X_CDATA) {
			/* x->len holds how much of the terminator has been seen */
			char t = x->state == X_COMMENT ? '-' : ']';
			if (c == t) {
				x->len++;
			} else if (c == '>' && x->len >= 2) {
				x->state = X_TEXT;
			} else {
				x->len = 0;
			}
			continue;
		}
		/* X_TAG */
		if (x->quote) {
			if (c == x->quote)
				x->quote = 0;
		} else if (c == '>') {
			if (x->overflow)
				x->dropped++;
			else
				tag_done(x);
			x->state = X_TEXT;
			continue;
		} else if ((c == '"' || c == '\'') && x->len && x->buf[0] != '!') {
			x->quote = c;
		}
		if (x->len < ST_XML_TAGMAX - 1)
			x->buf[x->len++] = c;
		else
			x->overflow = 1;
		if (x->len == 3 && !memcmp(x->buf, "!--", 3)) {
			x->state = X_COMMENT;
			x->len = 0;
		} else if (x->len == 8 && !memcmp(x->buf, "![CDATA[", 8)) {
			x->state = X_CDATA;
			x->len = 0;
		}
	}
}
