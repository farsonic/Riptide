/*
 * Riptide - speedtest: http:// URLs and an incremental HTTP/1.1 response
 * parser (RFC 9112: status line, header fields, Content-Length, chunked
 * transfer coding, close-delimited bodies, 1xx interim responses).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "speedtest.h"

/* ------------------------------------------------------------ URLs */

static int lc(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* host[:port][/path] after "http://" */
static int parse_rest(const char *h, struct st_url *u)
{
	const char *p = h;
	u32 n, port = 80;

	while (*p && *p != '/' && *p != '?' && *p != '#')
		p++;
	/* userinfo ("user@host") is not used by speedtest servers */
	{
		const char *a;
		for (a = h; a < p; a++)
			if (*a == '@')
				h = a + 1;
	}
	if (*h == '[')
		return ST_URL_BAD;                 /* IPv6 literal */
	for (n = 0; h + n < p && h[n] != ':'; n++)
		;
	if (!n || n >= ST_HOSTMAX)
		return ST_URL_BAD;
	memcpy(u->host, h, n);
	u->host[n] = 0;
	if (h + n < p) {                        /* :port */
		const char *q = h + n + 1;
		if (q == p)
			return ST_URL_BAD;
		port = 0;
		for (; q < p; q++) {
			if (*q < '0' || *q > '9')
				return ST_URL_BAD;
			port = port * 10 + (u32)(*q - '0');
			if (port > 65535)
				return ST_URL_BAD;
		}
		if (!port)
			return ST_URL_BAD;
	}
	u->port = (u16)port;
	n = 0;
	if (*p != '/')
		u->path[n++] = '/';
	while (*p && *p != '#' && n < ST_PATHMAX - 1) {
		char c = *p++;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			break;
		u->path[n++] = c;
	}
	if (*p && *p != '#' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
		return ST_URL_BAD;                 /* path too long */
	u->path[n] = 0;
	return 0;
}

int st_url_parse(const char *s, struct st_url *u)
{
	while (*s == ' ' || *s == '\t')
		s++;
	if (!rt_strncasecmp(s, "https://", 8))
		return ST_URL_HTTPS;
	if (rt_strncasecmp(s, "http://", 7))
		return ST_URL_BAD;
	return parse_rest(s + 7, u);
}

void st_url_dir(struct st_url *u)
{
	char *q = strchr(u->path, '?'), *sl;

	if (q)
		*q = 0;
	sl = strrchr(u->path, '/');
	if (sl)
		sl[1] = 0;
}

int st_url_join(const struct st_url *base, const char *loc, struct st_url *out)
{
	u32 n, k;

	while (*loc == ' ' || *loc == '\t')
		loc++;
	if (!rt_strncasecmp(loc, "https://", 8))
		return ST_URL_HTTPS;
	if (!rt_strncasecmp(loc, "http://", 7))
		return st_url_parse(loc, out);
	if (loc[0] == '/' && loc[1] == '/')             /* scheme relative */
		return parse_rest(loc + 2, out);
	{
		/* a scheme other than http(s): "ftp:", "mailto:" */
		const char *c = loc;
		while ((lc(*c) >= 'a' && lc(*c) <= 'z') || (*c >= '0' && *c <= '9') || *c == '+' || *c == '-' || *c == '.')
			c++;
		if (*c == ':' && c > loc)
			return ST_URL_BAD;
	}
	*out = *base;
	if (loc[0] == '/') {
		n = (u32)strlen(loc);
		if (n >= ST_PATHMAX)
			return ST_URL_BAD;
		memcpy(out->path, loc, n + 1);
	} else {
		st_url_dir(out);
		n = (u32)strlen(out->path);
		k = (u32)strlen(loc);
		if (n + k >= ST_PATHMAX)
			return ST_URL_BAD;
		memcpy(out->path + n, loc, k + 1);
	}
	{
		char *h = strchr(out->path, '#');
		if (h)
			*h = 0;
	}
	return 0;
}

/* ------------------------------------------------------------ responses */

void st_resp_init(struct st_resp *r, int head, st_sink sink, void *arg)
{
	memset(r, 0, sizeof(*r));
	r->state = ST_R_STATUS;
	r->head = head;
	r->sink = sink;
	r->arg = arg;
}

static void fail(struct st_resp *r, const char *why)
{
	r->state = ST_R_ERROR;
	r->error = why;
}

/* comma separated token list contains tok (case-insensitive)? */
static int has_token(const char *v, const char *tok)
{
	u32 n = (u32)strlen(tok);

	for (;;) {
		while (*v == ' ' || *v == '\t' || *v == ',')
			v++;
		if (!*v)
			return 0;
		if (!rt_strncasecmp(v, tok, n)) {
			const char *e = v + n;
			while (*e == ' ' || *e == '\t')
				e++;
			if (!*e || *e == ',' || *e == ';')
				return 1;
		}
		while (*v && *v != ',')
			v++;
	}
}

/* the last transfer coding of a Transfer-Encoding value */
static int last_is_chunked(const char *v)
{
	const char *last = v, *p;

	for (p = v; *p; p++)
		if (*p == ',')
			last = p + 1;
	while (*last == ' ' || *last == '\t')
		last++;
	return !rt_strncasecmp(last, "chunked", 7) &&
	       (last[7] == 0 || last[7] == ' ' || last[7] == '\t' || last[7] == ';');
}

static int parse_u64(const char *s, u64 *out)
{
	u64 v = 0;
	int digits = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	while (*s >= '0' && *s <= '9') {
		if (v > ((u64)1 << 58))
			return -1;
		v = v * 10 + (u64)(*s++ - '0');
		digits++;
	}
	while (*s == ' ' || *s == '\t')
		s++;
	if (!digits || *s)
		return -1;
	*out = v;
	return 0;
}

static void status_line(struct st_resp *r)
{
	const char *l = r->line;
	int i;

	if (!l[0] && r->hdrbytes <= 2)         /* tolerate a leading empty line */
		return;
	if (strncmp(l, "HTTP/1.", 7) || l[7] < '0' || l[7] > '9' || l[8] != ' ') {
		fail(r, "malformed status line");
		return;
	}
	r->minor = l[7] - '0';
	r->status = 0;
	for (i = 9; i < 12; i++) {
		if (l[i] < '0' || l[i] > '9') {
			fail(r, "malformed status code");
			return;
		}
		r->status = r->status * 10 + (l[i] - '0');
	}
	if ((l[12] != 0 && l[12] != ' ') || r->status < 100) {
		fail(r, "malformed status code");
		return;
	}
	r->state = ST_R_HEADER;
}

static void headers_done(struct st_resp *r)
{
	if (r->status < 200) {                 /* 100 Continue and friends */
		int head = r->head;
		st_sink sink = r->sink;
		void *arg = r->arg;
		u32 hb = r->hdrbytes;
		st_resp_init(r, head, sink, arg);
		r->hdrbytes = hb;
		return;
	}
	if (r->minor == 0 && !r->keepalive_hdr)
		r->close = 1;
	if (r->head || r->status == 204 || r->status == 304) {
		r->state = ST_R_DONE;
	} else if (r->chunked) {
		r->state = ST_R_CHUNK_SIZE;
	} else if (r->has_len) {
		r->left = r->clen;
		r->state = r->clen ? ST_R_BODY : ST_R_DONE;
	} else {
		r->close = 1;
		r->state = ST_R_BODY_CLOSE;
	}
}

static void header_line(struct st_resp *r)
{
	char *l = r->line, *v;

	if (!l[0]) {
		headers_done(r);
		return;
	}
	if (l[0] == ' ' || l[0] == '\t')       /* obsolete line folding: ignored */
		return;
	v = strchr(l, ':');
	if (!v || v == l) {
		fail(r, "malformed header field");
		return;
	}
	*v++ = 0;
	while (*v == ' ' || *v == '\t')
		v++;
	{
		/* trailing white space */
		u32 n = (u32)strlen(v);
		while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
			v[--n] = 0;
	}
	if (!rt_strcasecmp(l, "Content-Length")) {
		u64 len;
		if (parse_u64(v, &len) < 0 || (r->has_len && len != r->clen)) {
			fail(r, "bad Content-Length");
			return;
		}
		r->clen = len;
		r->has_len = 1;
	} else if (!rt_strcasecmp(l, "Transfer-Encoding")) {
		if (!last_is_chunked(v)) {
			fail(r, "unsupported Transfer-Encoding");
			return;
		}
		r->chunked = 1;
	} else if (!rt_strcasecmp(l, "Connection")) {
		if (has_token(v, "close"))
			r->close = 1;
		if (has_token(v, "keep-alive"))
			r->keepalive_hdr = 1;
	} else if (!rt_strcasecmp(l, "Location")) {
		rt_strlcpy(r->location, v, sizeof(r->location));
	}
}

static void chunk_size_line(struct st_resp *r)
{
	const char *l = r->line;
	u64 v = 0;
	int digits = 0, d;

	for (;;) {
		char c = *l;
		d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
		if (d < 0)
			break;
		if (v >> 56) {
			fail(r, "chunk size too large");
			return;
		}
		v = v * 16 + (u64)d;
		digits++;
		l++;
	}
	while (*l == ' ' || *l == '\t')
		l++;
	if (!digits || (*l && *l != ';')) {
		fail(r, "malformed chunk size");
		return;
	}
	if (v == 0) {
		r->state = ST_R_TRAILER;
	} else {
		r->left = v;
		r->state = ST_R_CHUNK_DATA;
	}
}

/* one complete line (CR stripped) in r->line */
static void line_done(struct st_resp *r)
{
	switch (r->state) {
	case ST_R_STATUS: status_line(r); break;
	case ST_R_HEADER: header_line(r); break;
	case ST_R_CHUNK_SIZE: chunk_size_line(r); break;
	case ST_R_CHUNK_END:
		if (r->line[0])
			fail(r, "missing CRLF after chunk");
		else
			r->state = ST_R_CHUNK_SIZE;
		break;
	case ST_R_TRAILER:
		if (!r->line[0])
			r->state = ST_R_DONE;
		break;
	}
}

static int line_state(int s)
{
	return s == ST_R_STATUS || s == ST_R_HEADER || s == ST_R_CHUNK_SIZE ||
	       s == ST_R_CHUNK_END || s == ST_R_TRAILER;
}

u32 st_resp_feed(struct st_resp *r, const char *p, u32 n)
{
	u32 used = 0;

	while (used < n && r->state != ST_R_DONE && r->state != ST_R_ERROR) {
		if (line_state(r->state)) {
			char c = p[used++];
			if (r->state == ST_R_STATUS || r->state == ST_R_HEADER) {
				if (++r->hdrbytes > ST_HDRMAX) {
					fail(r, "header too large");
					break;
				}
			}
			if (c == '\n') {
				if (r->linelen && r->line[r->linelen - 1] == '\r')
					r->linelen--;
				r->line[r->linelen] = 0;
				r->linelen = 0;
				line_done(r);
			} else if (r->linelen < ST_LINEMAX - 1) {
				r->line[r->linelen++] = c;
			} else if (r->state == ST_R_CHUNK_SIZE || r->state == ST_R_CHUNK_END) {
				fail(r, "chunk size line too long");
			}
			/* longer header lines are cut off: only the start matters */
			continue;
		}
		{
			u32 k = n - used;
			if (r->state != ST_R_BODY_CLOSE && (u64)k > r->left)
				k = (u32)r->left;
			if (r->sink && k)
				r->sink(r->arg, p + used, k);
			r->body += k;
			used += k;
			if (r->state == ST_R_BODY_CLOSE)
				continue;
			r->left -= k;
			if (!r->left)
				r->state = r->state == ST_R_BODY ? ST_R_DONE : ST_R_CHUNK_END;
		}
	}
	return used;
}

int st_resp_eof(struct st_resp *r)
{
	if (r->state == ST_R_BODY_CLOSE)
		r->state = ST_R_DONE;
	if (r->state == ST_R_DONE)
		return 0;
	if (r->state != ST_R_ERROR)
		fail(r, r->state == ST_R_STATUS && !r->hdrbytes ? "connection closed without a response"
		                                                : "connection closed in the middle of the response");
	return -1;
}

int st_resp_reusable(const struct st_resp *r)
{
	return r->state == ST_R_DONE && !r->close;
}
