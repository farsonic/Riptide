/*
 * Riptide - httpd-test: a minimal speedtest server for testing the
 * speedtest tool on the Amiga itself (tests/amiga/run_fsuae_speedtest.sh),
 * like tools/speedtest_mock.py on a host. NOT a web server: it knows only
 * the speedtest URLs and serves generated data.
 *
 *   httpd-test [-p port] [-d div] [-c] [-r] [-s] [-k] [-v]
 *
 *   /speedtest-config.php           configuration (<client ip=...>)
 *   /speedtest-servers-static.php   this server (id 1001, at the Host the
 *   /speedtest-servers.php          client used), a refused one (1003) and
 *                                   an ignored one (1004)
 *   /speedtest/latency.txt          "test=test"
 *   /speedtest/random<S>x<S>.jpg    S*S*2/div bytes
 *   /speedtest/upload.php           POST: counts the body, replies "size=N"
 *   /__stats                        counters (text)
 *
 * -c chunked documents and images, -r the configuration and lists via 302,
 * -s they redirect to https:// (the client must give up), -k no
 * keep-alive. Up to 8 connections at once, multiplexed with WaitSelect;
 * Ctrl-C stops it and prints the counters.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define MAXCL  8
#define INSZ   2048
#define OUTSZ  4096
#define DOCSZ  2048

enum { S_FREE, S_HEAD, S_BODY, S_SEND };

struct client {
	int fd, state, chunked, close_after, last_chunk;
	u32 inlen, outlen, outoff, doclen, docoff;
	u64 body_left, body_got, gen_left;
	int is_upload, badprefix, counted;
	char in[INSZ];
	char out[OUTSZ + 16];
	char doc[DOCSZ];
	char host[64];
	u32 peer;
};

static struct client *cl;
static int verbose, opt_chunked, opt_redirect, opt_https, opt_close;
static u32 div_by = 1;
static u64 st_dl, st_ul;
static u32 st_req, st_conn, st_lat, st_imgs, st_ups, st_redir, st_bad;
static char pattern[1024];

static void body_bytes(struct client *c, const char *p, u32 n);

static int has_close(const char *v)
{
	for (; *v; v++)
		if (!rt_strncasecmp(v, "close", 5))
			return 1;
	return 0;
}

static void dlog(const char *fmt, ...) RT_PRINTF(1, 2);
static void dlog(const char *fmt, ...)
{
	va_list ap;
	char b[200];

	if (!verbose)
		return;
	va_start(ap, fmt);
	rt_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	cl_printf("httpd-test: %s\n", b);
	cl_flush();
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		CloseSocket(c->fd);
	c->fd = -1;
	c->state = S_FREE;
}

static void stats_text(char *b, u32 n)
{
	rt_snprintf(b, n, "connections=%lu requests=%lu latency=%lu images=%lu download_bytes=%llu uploads=%lu "
	            "upload_bytes=%llu redirects=%lu badprefix=%lu\n", (unsigned long)st_conn, (unsigned long)st_req,
	            (unsigned long)st_lat, (unsigned long)st_imgs, (unsigned long long)st_dl, (unsigned long)st_ups,
	            (unsigned long long)st_ul, (unsigned long)st_redir, (unsigned long)st_bad);
}

/* Start a response: header now, body from c->doc (doclen) or generated
 * (gen_left bytes). */
static void respond(struct client *c, int code, const char *reason, const char *ctype, const char *extra, int chunked)
{
	u64 len = c->doclen ? c->doclen : c->gen_left;
	int n;

	c->chunked = chunked;
	c->last_chunk = 0;
	c->docoff = 0;
	if (chunked)
		n = rt_snprintf(c->out, OUTSZ, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n%s%s\r\n",
		                code, reason, ctype, extra, c->close_after ? "Connection: close\r\n" : "");
	else
		n = rt_snprintf(c->out, OUTSZ, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n%s%s\r\n",
		                code, reason, ctype, (unsigned long long)len, extra,
		                c->close_after ? "Connection: close\r\n" : "");
	c->outlen = (u32)n;
	/* a small document goes out with the header, in one segment */
	if (!chunked && c->doclen && c->outlen + c->doclen <= OUTSZ) {
		memcpy(c->out + c->outlen, c->doc, c->doclen);
		c->outlen += c->doclen;
		c->docoff = c->doclen;
	}
	c->outoff = 0;
	c->state = S_SEND;
}

static void doc_config(struct client *c)
{
	char a[16];

	c->doclen = (u32)rt_snprintf(c->doc, DOCSZ,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<settings>\n"
		"<client ip=\"%s\" lat=\"-33.8600\" lon=\"151.2100\" isp=\"Amiga Loopback &amp; Co\" isprating=\"3.7\"/>\n"
		"<server-config threadcount=\"2\" ignoreids=\"1004\"/>\n"
		"<download testlength=\"10\" threadsperurl=\"2\"/>\n"
		"<upload testlength=\"10\" ratio=\"5\" threads=\"2\" maxchunkcount=\"10\"/>\n"
		"</settings>\n", cl_ip(c->peer, a));
}

static void doc_servers(struct client *c)
{
	const char *h = c->host[0] ? c->host : "127.0.0.1";

	c->doclen = (u32)rt_snprintf(c->doc, DOCSZ,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<settings><servers>\n"
		"<server url=\"http://%s/speedtest/upload.php\" lat=\"-33.3600\" lon=\"151.2100\" name=\"Loopback\" "
		"country=\"Amiga\" cc=\"AM\" sponsor=\"Riptide httpd-test\" id=\"1001\" host=\"%s\"/>\n"
		"<server id='1003' url='http://127.0.0.1:1/speedtest/upload.php' lat='-33.8100' lon='151.2100' "
		"name='Nowhere' country='Amiga' cc='AM' sponsor='Refused' host='127.0.0.1:1'/>\n"
		"<server url=\"http://%s/speedtest/upload.php\" lat=\"-33.8601\" lon=\"151.2100\" name=\"Ignored\" "
		"country=\"Amiga\" cc=\"AM\" sponsor=\"Ignored\" id=\"1004\" host=\"%s\"/>\n"
		"</servers></settings>\n", h, h, h, h);
}

static u32 parse_size(const char *p)
{
	u32 a = 0, b = 0;

	while (*p >= '0' && *p <= '9')
		a = a * 10 + (u32)(*p++ - '0');
	if (*p++ != 'x')
		return 0;
	while (*p >= '0' && *p <= '9')
		b = b * 10 + (u32)(*p++ - '0');
	if (a != b || a > 10000 || strncmp(p, ".jpg", 4))
		return 0;
	return a;
}

/* a complete request head in c->in (n bytes up to the empty line) */
static void request(struct client *c, u32 n)
{
	char *line = c->in, *p, *path, *q;
	u64 clen = 0;
	int is_post;

	c->in[n - 1] = 0;
	st_req++;
	c->host[0] = 0;
	c->close_after = opt_close;
	/* header fields */
	for (p = strchr(line, '\n'); p && p[1]; p = strchr(p + 1, '\n')) {
		char *h = p + 1, *e = strchr(h, '\r');
		if (e)
			*e = 0;
		if (!rt_strncasecmp(h, "Host:", 5)) {
			h += 5;
			while (*h == ' ')
				h++;
			rt_strlcpy(c->host, h, sizeof(c->host));
		} else if (!rt_strncasecmp(h, "Content-Length:", 15)) {
			const char *end;
			clen = rt_strtoul(h + 15, &end, 10);
		} else if (!rt_strncasecmp(h, "Connection:", 11) && has_close(h + 11)) {
			c->close_after = 1;
		}
		if (e)
			*e = '\r';
	}
	if ((q = strchr(line, '\r')) != NULL)
		*q = 0;
	dlog("%s", line);
	is_post = !strncmp(line, "POST ", 5);
	path = strchr(line, ' ');
	if (!path) {
		c->doclen = (u32)rt_snprintf(c->doc, DOCSZ, "bad request\n");
		c->close_after = 1;
		respond(c, 400, "Bad Request", "text/plain", "", 0);
		return;
	}
	path++;
	if ((q = strchr(path, ' ')) != NULL)
		*q = 0;
	if ((q = strchr(path, '?')) != NULL)
		*q = 0;
	c->doclen = 0;
	c->gen_left = 0;
	if (is_post) {
		if (strcmp(path, "/speedtest/upload.php")) {
			c->doclen = (u32)rt_snprintf(c->doc, DOCSZ, "not found\n");
			c->close_after = 1;
			respond(c, 404, "Not Found", "text/plain", "", 0);
			return;
		}
		st_ups++;
		c->is_upload = 1;
		c->body_left = clen;
		c->body_got = 0;
		c->badprefix = 0;
		c->state = S_BODY;
		if (!clen)
			body_bytes(c, "", 0);
		return;
	}
	if (!strcmp(path, "/__stats")) {
		stats_text(c->doc, DOCSZ);
		c->doclen = (u32)strlen(c->doc);
		respond(c, 200, "OK", "text/plain", "", 0);
		return;
	}
	if (!strcmp(path, "/speedtest-config.php") || !strcmp(path, "/speedtest-servers-static.php") ||
	    !strcmp(path, "/speedtest-servers.php")) {
		if (opt_https || opt_redirect) {
			char *loc = c->out + OUTSZ / 2;
			rt_snprintf(loc, OUTSZ / 2, "Location: %s%s\r\n", opt_https ? "https://www.speedtest.net" : "/r", path);
			st_redir++;
			c->doclen = 0;
			c->gen_left = 0;
			respond(c, opt_https ? 301 : 302, "Moved", "text/html", loc, 0);
			return;
		}
	} else if (!strncmp(path, "/r/", 3)) {
		path += 2;
	}
	if (!strcmp(path, "/speedtest-config.php")) {
		doc_config(c);
		respond(c, 200, "OK", "text/xml", "", opt_chunked);
	} else if (!strcmp(path, "/speedtest-servers-static.php") || !strcmp(path, "/speedtest-servers.php")) {
		doc_servers(c);
		respond(c, 200, "OK", "text/xml", "", opt_chunked);
	} else if (!strcmp(path, "/speedtest/latency.txt")) {
		st_lat++;
		c->doclen = (u32)rt_snprintf(c->doc, DOCSZ, "test=test\n");
		respond(c, 200, "OK", "text/plain", "", 0);
	} else if (!strncmp(path, "/speedtest/random", 17) && parse_size(path + 17)) {
		u32 s = parse_size(path + 17);
		st_imgs++;
		c->gen_left = (u64)s * s * 2 / div_by;
		if (!c->gen_left)
			c->gen_left = 1;
		respond(c, 200, "OK", "image/jpeg", "", opt_chunked);
	} else {
		c->doclen = (u32)rt_snprintf(c->doc, DOCSZ, "not found\n");
		respond(c, 404, "Not Found", "text/plain", "", 0);
	}
}

/* upload body bytes that came with the head */
static void drain_body(struct client *c)
{
	while (c->state == S_BODY && c->inlen) {
		u32 n = c->body_left < c->inlen ? (u32)c->body_left : c->inlen;
		body_bytes(c, c->in, n);
		memmove(c->in, c->in + n, c->inlen - n);
		c->inlen -= n;
	}
}

/* look for a complete request head in c->in */
static void scan_input(struct client *c)
{
	u32 i;

	for (i = 3; i < c->inlen; i++) {
		if (c->in[i] == '\n' && c->in[i - 1] == '\r' && c->in[i - 2] == '\n' && c->in[i - 3] == '\r') {
			u32 n = i + 1, rest = c->inlen - n;
			request(c, n);
			/* keep what follows (the start of an upload body) */
			memmove(c->in, c->in + n, rest);
			c->inlen = rest;
			drain_body(c);
			return;
		}
	}
	if (c->inlen >= INSZ - 1) {
		c->close_after = 1;
		close_client(c);
	}
}

/* upload body bytes p[0..n) */
static void body_bytes(struct client *c, const char *p, u32 n)
{
	if (!c->body_got && n && memcmp(p, "content1=", n < 9 ? n : 9))
		c->badprefix = 1;
	c->body_got += n;
	c->body_left -= n < c->body_left ? n : c->body_left;
	st_ul += n;
	if (!c->body_left) {
		if (c->badprefix)
			st_bad++;
		c->doclen = (u32)rt_snprintf(c->doc, DOCSZ, "size=%llu", (unsigned long long)c->body_got);
		c->is_upload = 0;
		respond(c, 200, "OK", "text/html", "", 0);
	}
}

static void on_read(struct client *c, char *scratch)
{
	LONG k;

	if (c->state == S_BODY) {
		u32 want = c->body_left > OUTSZ ? OUTSZ : (u32)c->body_left;
		k = recv(c->fd, scratch, (LONG)want, 0);
		if (k <= 0) {
			close_client(c);
			return;
		}
		body_bytes(c, scratch, (u32)k);
		return;
	}
	k = recv(c->fd, c->in + c->inlen, (LONG)(INSZ - 1 - c->inlen), 0);
	if (k <= 0) {
		close_client(c);
		return;
	}
	c->inlen += (u32)k;
	if (c->state == S_HEAD)
		scan_input(c);
}

/* refill c->out with the next piece of the body */
static int refill(struct client *c)
{
	u32 room = OUTSZ - 16, n = 0, off = c->chunked ? 8 : 0;
	char *d = c->out + off;

	if (c->last_chunk)
		return 0;
	if (c->doclen) {
		n = c->doclen - c->docoff;
		if (c->chunked && n > 300)
			n = 300;                    /* several chunks per document */
		if (n > room)
			n = room;
		memcpy(d, c->doc + c->docoff, n);
		c->docoff += n;
	} else {
		while (n < room && c->gen_left) {
			u32 k = room - n;
			if (k > sizeof(pattern))
				k = sizeof(pattern);
			if ((u64)k > c->gen_left)
				k = (u32)c->gen_left;
			memcpy(d + n, pattern, k);
			n += k;
			c->gen_left -= k;
			st_dl += k;
		}
	}
	if (c->chunked) {
		if (!n) {
			memcpy(c->out, "0\r\n\r\n", 5);
			c->outlen = 5;
			c->last_chunk = 1;
		} else {
			char h[10];
			int hl = rt_snprintf(h, sizeof(h), "%lx\r\n", (unsigned long)n);
			memcpy(c->out + off - hl, h, (u32)hl);
			memcpy(d + n, "\r\n", 2);
			memmove(c->out, c->out + off - hl, (u32)hl + n + 2);
			c->outlen = (u32)hl + n + 2;
		}
	} else {
		if (!n)
			return 0;
		c->outlen = n;
	}
	c->outoff = 0;
	return 1;
}

static void on_write(struct client *c)
{
	int i;

	for (i = 0; i < 4; i++) {
		LONG k;
		if (c->outoff >= c->outlen && !refill(c)) {
			/* response done */
			if (c->close_after) {
				close_client(c);
				return;
			}
			c->state = S_HEAD;
			if (c->inlen)
				scan_input(c);
			return;
		}
		k = send(c->fd, c->out + c->outoff, (LONG)(c->outlen - c->outoff), 0);
		if (k < 0) {
			if (Errno() != RT_EWOULDBLOCK)
				close_client(c);
			return;
		}
		c->outoff += (u32)k;
		if (c->outoff < c->outlen)
			return;
	}
}

static int httpd_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in sa;
	LONG one = 1;
	u32 port = 8080, i;
	int c, s, rc = CL_OK;
	char *scratch;

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "p:d:crskvh", &g)) != -1) {
		switch (c) {
		case 'p':
			if (cl_parse_uint(g.arg, 65535, &port) < 0 || !port)
				return CL_ERROR;
			break;
		case 'd':
			if (cl_parse_uint(g.arg, 1000000, &div_by) < 0 || !div_by)
				return CL_ERROR;
			break;
		case 'c': opt_chunked = 1; break;
		case 'r': opt_redirect = 1; break;
		case 's': opt_https = 1; break;
		case 'k': opt_close = 1; break;
		case 'v': verbose = 1; break;
		default:
			cl_puts("usage: httpd-test [-p port] [-d div] [-c] [-r] [-s] [-k] [-v]\n");
			return c == 'h' ? CL_OK : CL_ERROR;
		}
	}
	if (cl_need_socket() < 0)
		return CL_FAIL;
	cl = AllocVec(sizeof(*cl) * MAXCL, MEMF_ANY | MEMF_CLEAR);
	scratch = AllocVec(OUTSZ, MEMF_ANY);
	if (!cl || !scratch) {
		cl_error("out of memory");
		rc = CL_FAIL;
		goto out;
	}
	for (i = 0; i < MAXCL; i++)
		cl[i].fd = -1;
	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (char)(i * 7 + (i >> 3));
	s = socket(RT_AF_INET, RT_SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons((u16)port);
	if (s >= 0)
		setsockopt(s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, &sa, sizeof(sa)) < 0 || listen(s, 8) < 0) {
		cl_error("cannot listen on port %lu: %s", (unsigned long)port, cl_strerror((int)Errno()));
		if (s >= 0)
			CloseSocket(s);
		rc = CL_ERROR;
		goto out;
	}
	IoctlSocket(s, RT_FIONBIO, &one);
	cl_printf("httpd-test: listening on port %lu\n", (unsigned long)port);
	cl_flush();
	for (;;) {
		ULONG rd[8], wr[8], sigs = SIGBREAKF_CTRL_C;
		LONG n;
		int maxfd = s;
		memset(rd, 0, sizeof(rd));
		memset(wr, 0, sizeof(wr));
		rd[s / 32] |= 1UL << (s % 32);
		for (i = 0; i < MAXCL; i++) {
			struct client *k = &cl[i];
			if (k->state == S_FREE)
				continue;
			if (k->state == S_SEND)
				wr[k->fd / 32] |= 1UL << (k->fd % 32);
			else
				rd[k->fd / 32] |= 1UL << (k->fd % 32);
			if (k->fd > maxfd)
				maxfd = k->fd;
		}
		n = WaitSelect(maxfd + 1, rd, wr, NULL, NULL, &sigs);
		if ((sigs & SIGBREAKF_CTRL_C) || (n < 0 && Errno() == RT_EINTR))
			break;
		if (n < 0)
			continue;
		if (rd[s / 32] & (1UL << (s % 32))) {
			LONG len = sizeof(sa);
			int fd = accept(s, &sa, &len);
			if (fd >= 0) {
				for (i = 0; i < MAXCL && cl[i].state != S_FREE; i++)
					;
				if (i == MAXCL || fd >= 256) {
					CloseSocket(fd);
				} else {
					struct client *k = &cl[i];
					k->fd = fd;
					k->state = S_HEAD;
					k->inlen = k->outlen = k->outoff = k->doclen = 0;
					k->body_left = k->gen_left = 0;
					k->is_upload = k->close_after = k->chunked = k->last_chunk = 0;
					k->peer = sa.sin_addr;
					IoctlSocket(fd, RT_FIONBIO, &one);
					setsockopt(fd, RT_IPPROTO_TCP, RT_TCP_NODELAY, &one, sizeof(one));
					st_conn++;
				}
			}
		}
		for (i = 0; i < MAXCL; i++) {
			struct client *k = &cl[i];
			int fd = k->fd;
			if (k->state == S_FREE)
				continue;
			if (k->state == S_SEND && (wr[fd / 32] & (1UL << (fd % 32))))
				on_write(k);
			else if (k->state != S_SEND && (rd[fd / 32] & (1UL << (fd % 32))))
				on_read(k, scratch);
		}
	}
	CloseSocket(s);
	{
		char b[300];
		stats_text(b, sizeof(b));
		cl_printf("httpd-test: %s", b);
	}
out:
	if (cl) {
		for (i = 0; i < MAXCL; i++)
			if (cl[i].state != S_FREE)
				close_client(&cl[i]);
		FreeVec(cl);
	}
	if (scratch)
		FreeVec(scratch);
	return rc;
}

CL_MAIN(httpd_main, "httpd-test")
