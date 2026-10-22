/*
 * Riptide - speedtest: the command (configuration, server list, latency,
 * download and upload tests, output). See speedtest.h.
 *
 * Protocol (the legacy speedtest.net HTTP test, as used by speedtest-cli):
 *   GET  /speedtest-config.php              client address/ISP/position, test parameters
 *   GET  /speedtest-servers-static.php      <server url= lat= lon= ... id= host=/> list
 *   GET  <base>/latency.txt?x=...           "test=test"; 3 per candidate server
 *   GET  <base>/random<S>x<S>.jpg?x=...     download, several connections
 *   POST <base>/upload.php                  "content1=..." bodies, several connections
 * All connections are non-blocking and multiplexed with one wait() (WaitSelect
 * on AmigaOS), so one task drives every transfer and Ctrl-C works at any time.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "speedtest.h"

#define ST_MAXCONN   16
#define ST_RXBUF     32768
#define ST_PATSZ     8192          /* upload body pattern */
#define ST_REQMAX    1024
#define ST_MAXIDS    16
#define ST_KEEP      10            /* nearest servers kept for the latency test */
#define ST_LISTMAX   100           /* servers kept for --list */
#define ST_CANDIDATES 5
#define ST_MAXSIZES  16
#define ST_IGNOREMAX 128
#define ST_REDIRECTS 5
#define ST_LAT_TRIES 3
#define ST_LAT_FAIL  3600000000UL  /* 3600 s: an unreachable server */
#define ST_EPROTO    (-1000)       /* HTTP protocol error (resp.error) */
#define ST_EHTTP     (-1001)       /* unexpected HTTP status */

#define CONFIG_PATH "/speedtest-config.php"
static const char *const config_urls[] = {
	"http://www.speedtest.net/speedtest-config.php", NULL
};
static const char *const server_urls[] = {
	"http://www.speedtest.net/speedtest-servers-static.php",
	"http://c.speedtest.net/speedtest-servers-static.php",
	"http://www.speedtest.net/speedtest-servers.php",
	"http://c.speedtest.net/speedtest-servers.php", NULL
};
static const char *const mirror_paths[] = {
	"/speedtest-servers-static.php", "/speedtest-servers.php", NULL
};
static const u32 dl_default[] = { 350, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500, 4000 };
/* Upload bodies: smaller than speedtest-cli's largest (7 MB), which a 68020
 * cannot send within the test time anyway; the time limit ends the test. */
static const u32 ul_default[] = { 131072, 262144, 524288, 1048576, 2097152 };

struct st_server {
	u32 id, dist, latency_us;
	s32 lat, lon;
	int has_pos;
	char url[192], host[80], name[64], country[48], cc[8], sponsor[96], slat[16], slon[16];
};

enum { C_IDLE, C_CONNECT, C_SEND, C_RECV };
enum { PH_NONE, PH_XML, PH_LATENCY, PH_DOWNLOAD, PH_UPLOAD };
enum { RUN_DONE, RUN_LIMIT, RUN_BREAK, RUN_ERROR };

struct st;
struct st_conn {
	struct st *st;
	int fd, state, keep, reused, retried;
	u32 addr;
	u16 port;
	u32 t_start, t_last;
	u64 body_len, body_off, rx;
	u32 reqlen, reqoff, smalllen;
	char small[64];
	struct st_resp resp;
	char req[ST_REQMAX];
};

struct st {
	const struct st_sys *sys;
	void *ctx;
	/* options */
	int simple, json, csv, csv_header, list, bytes_units, no_dl, no_ul, single, debug, quiet, user_count;
	char csv_delim;
	u32 timeout_ms, threads, duration_s, count, test_break_ms;
	u32 ids[ST_MAXIDS], excl[ST_MAXIDS];
	int nids, nexcl;
	const char *server_url, *mirror;
	u32 dl_sizes[ST_MAXSIZES], ul_sizes[ST_MAXSIZES];
	int ndl, nul;
	/* configuration */
	char client_ip[48], client_isp[96], client_lat[16], client_lon[16];
	s32 clat, clon;
	int has_client, has_cpos;
	u32 cfg_threads, cfg_dl_len, cfg_dl_perurl, cfg_ul_len, cfg_ul_max, cfg_ul_threads;
	u32 ignore[ST_IGNOREMAX];
	int nignore;
	/* servers */
	struct st_server *srv;
	int nsrv, cap;
	u32 seen_srv, matched_srv;
	struct st_server custom, *best;
	/* the current target */
	struct st_url base, up, u1, u2;      /* u1, u2: scratch (keeps frames small) */
	u32 addr;
	/* transfers */
	struct st_conn *conn;
	char *rx, *pat;
	struct st_xml *xml;
	int phase, stop, raised;
	u32 next_req, nreqs, done_reqs, failed_reqs, t_prog;
	u64 bytes;
	int last_err, fetch_status;
	char last_msg[64];
	u32 lat_sum, lat_ok, lat_bad;
	/* results */
	u64 dl_bytes, ul_bytes;
	u32 dl_us, ul_us, ping_us;
	char location[ST_PATHMAX];
	char fbuf[8][40];                   /* formatted numbers */
	char line[640], tmp[ST_PATHMAX + 32], ustr[ST_PATHMAX + ST_HOSTMAX + 16];
};

/* ------------------------------------------------------------ output */

static void out_str(struct st *st, int e, const char *s)
{
	st->sys->out(st->ctx, e, s, (u32)strlen(s));
}

static void say(struct st *st, const char *fmt, ...) RT_PRINTF(2, 3);
static void say(struct st *st, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(st->line, sizeof(st->line), fmt, ap);
	va_end(ap);
	out_str(st, 0, st->line);
}

/* progress text of the normal output only */
static void info(struct st *st, const char *fmt, ...) RT_PRINTF(2, 3);
static void info(struct st *st, const char *fmt, ...)
{
	va_list ap;

	if (st->quiet)
		return;
	va_start(ap, fmt);
	rt_vsnprintf(st->line, sizeof(st->line), fmt, ap);
	va_end(ap);
	out_str(st, 0, st->line);
	st->sys->flush(st->ctx);
}

static void err(struct st *st, const char *fmt, ...) RT_PRINTF(2, 3);
static void err(struct st *st, const char *fmt, ...)
{
	va_list ap;
	u32 n;

	memcpy(st->line, "speedtest: ", 11);
	va_start(ap, fmt);
	rt_vsnprintf(st->line + 11, sizeof(st->line) - 12, fmt, ap);
	va_end(ap);
	n = (u32)strlen(st->line);
	st->line[n++] = '\n';
	st->line[n] = 0;
	st->sys->flush(st->ctx);
	out_str(st, 1, st->line);
}

static void dbg(struct st *st, const char *fmt, ...) RT_PRINTF(2, 3);
static void dbg(struct st *st, const char *fmt, ...)
{
	va_list ap;
	u32 n;

	if (!st->debug)
		return;
	memcpy(st->line, "debug: ", 7);
	va_start(ap, fmt);
	rt_vsnprintf(st->line + 7, sizeof(st->line) - 8, fmt, ap);
	va_end(ap);
	n = (u32)strlen(st->line);
	st->line[n++] = '\n';
	st->line[n] = 0;
	out_str(st, 1, st->line);
}

static const char *url_str(struct st *st, const struct st_url *u)
{
	if (u->port == 80)
		rt_snprintf(st->ustr, sizeof(st->ustr), "http://%s%s", u->host, u->path);
	else
		rt_snprintf(st->ustr, sizeof(st->ustr), "http://%s:%u%s", u->host, (unsigned)u->port, u->path);
	return st->ustr;
}

static const char *errstr(struct st *st, int e)
{
	if (e == ST_EPROTO || e == ST_EHTTP)
		return st->last_msg;
	return st->sys->strerror(st->ctx, -e);
}

static void note_err(struct st *st, struct st_conn *c, int e)
{
	st->last_err = e;
	if (e == ST_EPROTO && c)
		rt_strlcpy(st->last_msg, c->resp.error ? c->resp.error : "protocol error", sizeof(st->last_msg));
	else if (e == ST_EHTTP && c)
		rt_snprintf(st->last_msg, sizeof(st->last_msg), "HTTP status %d", c->resp.status);
}

static void https_error(struct st *st, const char *what, const char *from, const char *to)
{
	err(st, "%s: %s redirects to %s", what, from, to);
	out_str(st, 1,
	        "  speedtest.net now requires HTTPS for this, and Riptide has no TLS. Use\n"
	        "  --server-url http://HOST:PORT/speedtest/upload.php to test against a known\n"
	        "  server, or --mirror http://HOST[:PORT] to read the configuration and the\n"
	        "  server list from an HTTP mirror (such as tools/speedtest_mock.py).\n");
}

/* ------------------------------------------------------------ connections */

static void conn_close(struct st *st, struct st_conn *c)
{
	if (c->fd >= 0)
		st->sys->close(st->ctx, c->fd);
	c->fd = -1;
	c->keep = 0;
	c->state = C_IDLE;
}

static void close_all(struct st *st)
{
	int i;

	for (i = 0; i < ST_MAXCONN; i++)
		conn_close(st, &st->conn[i]);
}

static void conn_sink(void *arg, const char *p, u32 n)
{
	struct st_conn *c = arg;
	struct st *st = c->st;

	switch (st->phase) {
	case PH_XML:
		if (c->resp.status / 100 == 2)
			st_xml_feed(st->xml, p, n);
		break;
	case PH_DOWNLOAD:
		st->bytes += n;
		break;
	default:
		if (c->smalllen < sizeof(c->small) - 1) {
			u32 k = RT_MIN(n, (u32)sizeof(c->small) - 1 - c->smalllen);
			memcpy(c->small + c->smalllen, p, k);
			c->smalllen += k;
			c->small[c->smalllen] = 0;
		}
		break;
	}
}

/* (Re)connect c to the current target and send c->req from the start. */
static int conn_open(struct st *st, struct st_conn *c)
{
	u32 now = st->sys->now_us(st->ctx);
	int fd;

	c->reqoff = 0;
	c->body_off = 0;
	c->rx = 0;
	c->smalllen = 0;
	c->small[0] = 0;
	st_resp_init(&c->resp, 0, conn_sink, c);
	c->t_last = now;
	if (c->fd >= 0 && c->keep && c->addr == st->addr && c->port == st->base.port) {
		c->reused = 1;
		c->state = C_SEND;
		c->t_start = now;
		return 0;
	}
	conn_close(st, c);
	c->reused = 0;
	fd = st->sys->connect(st->ctx, st->addr, st->base.port);
	if (fd < 0)
		return fd;
	if (fd >= st->sys->maxfd || fd >= 256) {
		st->sys->close(st->ctx, fd);
		return -RT_EMFILE;
	}
	c->fd = fd;
	c->addr = st->addr;
	c->port = st->base.port;
	c->state = C_CONNECT;
	return 0;
}

/* Start a request on c: method and path on the current target. */
static int conn_request(struct st *st, struct st_conn *c, const char *method, const char *path, u64 body_len)
{
	char hostport[ST_HOSTMAX + 8];
	int n;

	if (st->base.port == 80)
		rt_strlcpy(hostport, st->base.host, sizeof(hostport));
	else
		rt_snprintf(hostport, sizeof(hostport), "%s:%u", st->base.host, (unsigned)st->base.port);
	n = rt_snprintf(c->req, ST_REQMAX,
	                "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (%s) speedtest-riptide/" ST_VERSION
	                "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nCache-Control: no-cache\r\n"
	                "Connection: keep-alive\r\n",
	                method, path, hostport, st->sys->ua ? st->sys->ua : "AmigaOS; m68k");
	if (body_len && n > 0 && n < ST_REQMAX)
		n += rt_snprintf(c->req + n, ST_REQMAX - (u32)n,
		                 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %llu\r\n",
		                 (unsigned long long)body_len);
	if (n > 0 && n < ST_REQMAX)
		n += rt_snprintf(c->req + n, ST_REQMAX - (u32)n, "\r\n");
	if (n <= 0 || n >= ST_REQMAX)
		return -RT_ENAMETOOLONG;
	c->reqlen = (u32)n;
	c->body_len = body_len;
	c->retried = 0;
	dbg(st, "%s http://%s%s%s", method, hostport, path, body_len ? " (POST body)" : "");
	return conn_open(st, c);
}

typedef int (*next_fn)(struct st *st, struct st_conn *c);
typedef void (*done_fn)(struct st *st, struct st_conn *c, int e);

/* The request on c ended (e = 0: complete response). */
static void finish(struct st *st, struct st_conn *c, int e, next_fn next, done_fn done)
{
	/* a kept-alive connection the server had already closed: retry once */
	if (e && e != ST_EHTTP && c->reused && !c->rx && !c->retried) {
		int r;
		conn_close(st, c);
		c->retried = 1;
		r = conn_open(st, c);
		if (r == 0)
			return;
		e = r;
	}
	if (e || !st_resp_reusable(&c->resp) || c->state == C_SEND)
		conn_close(st, c);
	else
		c->state = C_IDLE, c->keep = 1;
	if (e)
		note_err(st, c, e);
	done(st, c, e);
	/* the next request, skipping those that cannot even be started */
	while (!st->stop && next) {
		int r = next(st, c);
		if (r >= 0)
			break;
		note_err(st, c, r);
		done(st, c, r);
	}
}

static void do_send(struct st *st, struct st_conn *c, u32 now, next_fn next, done_fn done)
{
	int i;

	for (i = 0; i < 8; i++) {
		const char *p;
		u32 n;
		int k, body = 0;
		if (c->reqoff < c->reqlen) {
			p = c->req + c->reqoff;
			n = c->reqlen - c->reqoff;
		} else if (c->body_off < c->body_len) {
			/* "content1=" and then the pattern, over and over */
			u64 off = c->body_off;
			if (off >= ST_PATSZ + 9)
				off = 9 + (off - 9) % ST_PATSZ;
			p = st->pat + (u32)off;
			n = ST_PATSZ + 9 - (u32)off;
			if ((u64)n > c->body_len - c->body_off)
				n = (u32)(c->body_len - c->body_off);
			body = 1;
		} else {
			break;
		}
		k = st->sys->send(st->ctx, c->fd, p, n);
		if (k == -RT_EWOULDBLOCK || k == -RT_EINTR)
			return;
		if (k <= 0) {
			finish(st, c, k ? k : -RT_EPIPE, next, done);
			return;
		}
		c->t_last = now;
		if (body) {
			c->body_off += (u32)k;
			if (st->phase == PH_UPLOAD)
				st->bytes += (u32)k;
		} else {
			c->reqoff += (u32)k;
		}
		if ((u32)k < n)
			return;
	}
	if (c->reqoff >= c->reqlen && c->body_off >= c->body_len)
		c->state = C_RECV;
}

static void do_recv(struct st *st, struct st_conn *c, u32 now, next_fn next, done_fn done)
{
	int k = st->sys->recv(st->ctx, c->fd, st->rx, ST_RXBUF);
	u32 used;

	if (k == -RT_EWOULDBLOCK || k == -RT_EINTR)
		return;
	if (k < 0) {
		finish(st, c, k, next, done);
		return;
	}
	if (k == 0) {
		if (st_resp_eof(&c->resp) == 0) {
			c->resp.close = 1;
			finish(st, c, 0, next, done);
		} else {
			finish(st, c, c->rx ? ST_EPROTO : -RT_ECONNRESET, next, done);
		}
		return;
	}
	c->rx += (u32)k;
	c->t_last = now;
	used = st_resp_feed(&c->resp, st->rx, (u32)k);
	if (c->resp.state == ST_R_ERROR) {
		finish(st, c, ST_EPROTO, next, done);
	} else if (c->resp.state == ST_R_DONE) {
		if (used < (u32)k)
			c->resp.close = 1;                 /* unexpected extra data */
		finish(st, c, 0, next, done);
	}
}

static int fd_isset(const u32 *set, int fd)
{
	return (set[fd >> 5] >> (fd & 31)) & 1;
}

/* Drive nconn connections until every request is done, limit_ms passes
 * (0: no limit), or Ctrl-C. next() starts a request on an idle
 * connection (1), has nothing left (0) or fails (-errno); done() sees each
 * finished request. */
static int run(struct st *st, int nconn, u32 limit_ms, next_fn next, done_fn done)
{
	u32 t0 = st->sys->now_us(st->ctx), now;
	int i;

	st->stop = 0;
	for (i = 0; i < nconn; i++) {
		struct st_conn *c = &st->conn[i];
		for (;;) {
			int r = next(st, c);
			if (r >= 0)
				break;
			note_err(st, c, r);
			done(st, c, r);
		}
	}
	for (;;) {
		u32 rd[8], wr[8], wait_ms = 100;
		int maxfd = -1, r;
		if (st->sys->brk(st->ctx))
			return RUN_BREAK;
		now = st->sys->now_us(st->ctx);
		if (st->test_break_ms && !st->raised && now - st->t_prog >= st->test_break_ms * 1000UL) {
			st->raised = 1;
			st->sys->raise_brk(st->ctx);
		}
		memset(rd, 0, sizeof(rd));
		memset(wr, 0, sizeof(wr));
		for (i = 0; i < nconn; i++) {
			struct st_conn *c = &st->conn[i];
			if (c->state == C_IDLE)
				continue;
			if (c->state == C_CONNECT || c->state == C_SEND)
				wr[c->fd >> 5] |= 1UL << (c->fd & 31);
			if (c->state != C_CONNECT)
				rd[c->fd >> 5] |= 1UL << (c->fd & 31);
			if (c->fd > maxfd)
				maxfd = c->fd;
		}
		if (maxfd < 0)
			return RUN_DONE;
		if (limit_ms) {
			u32 el = (now - t0) / 1000UL;
			if (el >= limit_ms) {
				st->stop = 1;
				for (i = 0; i < nconn; i++)
					if (st->conn[i].state != C_IDLE)
						conn_close(st, &st->conn[i]);
				return RUN_LIMIT;
			}
			if (limit_ms - el < wait_ms)
				wait_ms = limit_ms - el;
		}
		r = st->sys->wait(st->ctx, maxfd + 1, rd, wr, wait_ms);
		if (r == -1)
			return RUN_BREAK;
		if (r < -1) {
			err(st, "wait failed");
			return RUN_ERROR;
		}
		now = st->sys->now_us(st->ctx);
		for (i = 0; i < nconn; i++) {
			struct st_conn *c = &st->conn[i];
			int canr, canw;
			if (c->state == C_IDLE)
				continue;
			canr = r > 0 && fd_isset(rd, c->fd);
			canw = r > 0 && fd_isset(wr, c->fd);
			if (c->state == C_CONNECT && canw) {
				int e = st->sys->conn_error(st->ctx, c->fd);
				if (e) {
					finish(st, c, -e, next, done);
					continue;
				}
				c->state = C_SEND;
				c->t_start = c->t_last = now;
			}
			if (c->state == C_SEND && canw)
				do_send(st, c, now, next, done);
			if ((c->state == C_SEND || c->state == C_RECV) && canr)
				do_recv(st, c, now, next, done);
			if (c->state != C_IDLE) {
				s32 idle = (s32)(now - c->t_last);
				if (idle > 0 && (u32)idle / 1000UL > st->timeout_ms)
					finish(st, c, -RT_ETIMEDOUT, next, done);
			}
		}
	}
}

/* ------------------------------------------------------------ fetching XML */

/* next() result from conn_request(): 1 started, or the error */
static int start(int r)
{
	return r < 0 ? r : 1;
}

static int next_fetch(struct st *st, struct st_conn *c)
{
	if (st->next_req++)
		return 0;
	return start(conn_request(st, c, "GET", st->base.path, 0));
}

static void done_fetch(struct st *st, struct st_conn *c, int e)
{
	st->fetch_status = e ? -1 : c->resp.status;
	st->last_err = e;
	rt_strlcpy(st->location, c->resp.location, sizeof(st->location));
	dbg(st, "-> %d, %llu body bytes", e ? e : c->resp.status, (unsigned long long)c->resp.body);
}

static int set_target(struct st *st, const struct st_url *u, const char *what, int quiet)
{
	u32 addr;

	if (st->sys->resolve(st->ctx, u->host, &addr) < 0) {
		if (quiet)
			dbg(st, "%s: cannot resolve %s", what, u->host);
		else
			err(st, "%s: cannot resolve %s", what, u->host);
		return -1;
	}
	st->base = *u;
	st->addr = addr;
	return 0;
}

/* GET url into the XML reader (callback cb), following http redirects.
 * 0 ok, -1 error (printed unless quiet_fail), -2 Ctrl-C. */
static int fetch_xml(struct st *st, const char *url, st_xml_cb cb, const char *what, int quiet_fail)
{
	struct st_url *u = &st->u1, *n = &st->u2;
	int r, redirects = 0;

	r = st_url_parse(url, u);
	if (r == ST_URL_HTTPS) {
		err(st, "%s: %s: HTTPS is not supported (no TLS)", what, url);
		return -1;
	}
	if (r < 0) {
		err(st, "%s: bad URL %s", what, url);
		return -1;
	}
	for (;;) {
		if (set_target(st, u, what, 0) < 0)
			return -1;
		st_xml_init(st->xml, cb, st);
		st->phase = PH_XML;
		st->next_req = 0;
		st->last_err = 0;
		r = run(st, 1, 0, next_fetch, done_fetch);
		close_all(st);
		if (r == RUN_BREAK)
			return -2;
		if (r != RUN_DONE)
			return -1;
		if (st->last_err) {
			if (!quiet_fail)
				err(st, "%s: %s: %s", what, url_str(st, u), errstr(st, st->last_err));
			return -1;
		}
		if (st->fetch_status / 100 == 3 && st->location[0] && st->fetch_status != 304) {
			if (++redirects > ST_REDIRECTS) {
				err(st, "%s: too many redirects", what);
				return -1;
			}
			r = st_url_join(u, st->location, n);
			if (r == ST_URL_HTTPS) {
				https_error(st, what, url_str(st, u), st->location);
				return -1;
			}
			if (r < 0) {
				err(st, "%s: bad redirect to %s", what, st->location);
				return -1;
			}
			dbg(st, "redirected to %s", st->location);
			*u = *n;
			continue;
		}
		if (st->fetch_status != 200) {
			if (!quiet_fail)
				err(st, "%s: %s: HTTP status %d", what, url_str(st, u), st->fetch_status);
			return -1;
		}
		return 0;
	}
}

static u32 attr_u32(int n, const struct st_xml_attr *a, const char *name, u32 dflt)
{
	const char *v = st_xml_attr(n, a, name), *end;
	unsigned long x;

	if (!v || !*v)
		return dflt;
	x = rt_strtoul(v, &end, 10);
	return end == v ? dflt : (u32)x;
}

static void copy_attr(char *dst, u32 size, int n, const struct st_xml_attr *a, const char *name)
{
	const char *v = st_xml_attr(n, a, name);

	rt_strlcpy(dst, v ? v : "", size);
}

static void config_tag(void *arg, const char *tag, int n, const struct st_xml_attr *a)
{
	struct st *st = arg;

	if (!strcmp(tag, "client")) {
		st->has_client = 1;
		copy_attr(st->client_ip, sizeof(st->client_ip), n, a, "ip");
		copy_attr(st->client_isp, sizeof(st->client_isp), n, a, "isp");
		copy_attr(st->client_lat, sizeof(st->client_lat), n, a, "lat");
		copy_attr(st->client_lon, sizeof(st->client_lon), n, a, "lon");
		st->has_cpos = st_parse_coord(st->client_lat, &st->clat) == 0 &&
		               st_parse_coord(st->client_lon, &st->clon) == 0;
	} else if (!strcmp(tag, "server-config")) {
		const char *ids = st_xml_attr(n, a, "ignoreids");
		st->cfg_threads = attr_u32(n, a, "threadcount", st->cfg_threads);
		while (ids && *ids && st->nignore < ST_IGNOREMAX) {
			const char *end;
			unsigned long v = rt_strtoul(ids, &end, 10);
			if (end != ids)
				st->ignore[st->nignore++] = (u32)v;
			ids = *end ? end + 1 : end;
		}
	} else if (!strcmp(tag, "download")) {
		st->cfg_dl_len = attr_u32(n, a, "testlength", st->cfg_dl_len);
		st->cfg_dl_perurl = attr_u32(n, a, "threadsperurl", st->cfg_dl_perurl);
	} else if (!strcmp(tag, "upload")) {
		st->cfg_ul_len = attr_u32(n, a, "testlength", st->cfg_ul_len);
		st->cfg_ul_max = attr_u32(n, a, "maxchunkcount", st->cfg_ul_max);
		st->cfg_ul_threads = attr_u32(n, a, "threads", st->cfg_ul_threads);
	}
}

static int id_in(const u32 *ids, int n, u32 id)
{
	int i;

	for (i = 0; i < n; i++)
		if (ids[i] == id)
			return 1;
	return 0;
}

static void server_tag(void *arg, const char *tag, int n, const struct st_xml_attr *a)
{
	struct st *st = arg;
	struct st_server *s;
	const char *url = st_xml_attr(n, a, "url");
	u32 id = attr_u32(n, a, "id", 0), d = 0;
	s32 lat = 0, lon = 0;
	int pos, has_pos;

	if (strcmp(tag, "server") || !url)
		return;
	st->seen_srv++;
	if (id_in(st->excl, st->nexcl, id))
		return;
	if (st->nids ? !id_in(st->ids, st->nids, id) : id_in(st->ignore, st->nignore, id))
		return;
	has_pos = st_parse_coord(st_xml_attr(n, a, "lat"), &lat) == 0 &&
	          st_parse_coord(st_xml_attr(n, a, "lon"), &lon) == 0;
	if (has_pos && st->has_cpos)
		d = st_distance(st->clat, st->clon, lat, lon);
	else if (st->has_cpos)
		d = 0xffffffffUL;                  /* unknown position: last */
	/* sorted by distance; equal distances keep the list order */
	for (pos = st->nsrv; pos > 0 && st->srv[pos - 1].dist > d; pos--)
		;
	st->matched_srv++;
	if (pos >= st->cap)
		return;
	if (st->nsrv == st->cap)
		st->nsrv--;
	memmove(&st->srv[pos + 1], &st->srv[pos], (u32)(st->nsrv - pos) * sizeof(*s));
	st->nsrv++;
	s = &st->srv[pos];
	memset(s, 0, sizeof(*s));
	s->id = id;
	s->dist = d == 0xffffffffUL ? 0 : d;
	s->lat = lat;
	s->lon = lon;
	s->has_pos = has_pos;
	rt_strlcpy(s->url, url, sizeof(s->url));
	copy_attr(s->host, sizeof(s->host), n, a, "host");
	copy_attr(s->name, sizeof(s->name), n, a, "name");
	copy_attr(s->country, sizeof(s->country), n, a, "country");
	copy_attr(s->cc, sizeof(s->cc), n, a, "cc");
	copy_attr(s->sponsor, sizeof(s->sponsor), n, a, "sponsor");
	copy_attr(s->slat, sizeof(s->slat), n, a, "lat");
	copy_attr(s->slon, sizeof(s->slon), n, a, "lon");
}

static const char *mirror_url(struct st *st, const char *path)
{
	u32 n = (u32)strlen(st->mirror);

	while (n && st->mirror[n - 1] == '/')
		n--;
	rt_snprintf(st->tmp, sizeof(st->tmp), "%.*s%s", (int)n, st->mirror, path);
	return st->tmp;
}

static int get_config(struct st *st)
{
	const char *url = st->mirror ? mirror_url(st, CONFIG_PATH) : config_urls[0];
	int r;

	info(st, "Retrieving speedtest.net configuration...\n");
	r = fetch_xml(st, url, config_tag, "configuration", 0);
	if (r == 0 && !st->has_client) {
		err(st, "configuration: no <client> element in the reply");
		r = -1;
	}
	if (r == -1)
		err(st, "Cannot retrieve speedtest configuration");
	return r;
}

static int get_servers(struct st *st)
{
	int i, r = -1;

	info(st, "Retrieving speedtest.net server list...\n");
	for (i = 0; ; i++) {
		const char *url = st->mirror ? (mirror_paths[i] ? mirror_url(st, mirror_paths[i]) : NULL) : server_urls[i];
		if (!url)
			break;
		st->nsrv = 0;
		st->seen_srv = st->matched_srv = 0;
		r = fetch_xml(st, url, server_tag, "server list", 0);
		if (r == -2)
			return -2;
		dbg(st, "%lu servers in the list, %d kept, %lu tags dropped", (unsigned long)st->seen_srv, st->nsrv,
		    (unsigned long)st->xml->dropped);
		if (r == 0 && st->seen_srv)
			break;
		r = -1;
	}
	if (r < 0) {
		err(st, "Cannot retrieve speedtest server list");
		return -1;
	}
	if (!st->nsrv) {
		if (st->nids)
			err(st, "No matched servers: %lu", (unsigned long)st->ids[0]);
		else
			err(st, "No servers left after the exclusions");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------ latency */

static int next_latency(struct st *st, struct st_conn *c)
{
	if (st->next_req >= ST_LAT_TRIES)
		return 0;
	rt_snprintf(st->tmp, sizeof(st->tmp), "%slatency.txt?x=%lu.%lu", st->base.path,
	            (unsigned long)st->sys->now_us(st->ctx), (unsigned long)st->next_req);
	st->next_req++;
	return start(conn_request(st, c, "GET", st->tmp, 0));
}

static void done_latency(struct st *st, struct st_conn *c, int e)
{
	if (!e && c->resp.status == 200 && !strncmp(c->small, "test=test", 9)) {
		u32 us = st->sys->now_us(st->ctx) - c->t_start;
		dbg(st, "latency: %lu us", (unsigned long)us);
		st->lat_sum += us;
		st->lat_ok++;
	} else {
		st->lat_bad++;
		if (!e)
			dbg(st, "latency: HTTP %d, body \"%.20s\"", c->resp.status, c->small);
	}
}

/* point st->base/up at server s; 0 or -1 (quiet: errors only with --debug) */
static int use_server(struct st *st, struct st_server *s, int quiet)
{
	struct st_url *u = &st->u1;
	int r = st_url_parse(s->url, u);

	if (r < 0) {
		if (quiet)
			dbg(st, "server %lu: unusable URL %s", (unsigned long)s->id, s->url);
		else if (r == ST_URL_HTTPS)
			err(st, "%s: HTTPS is not supported (no TLS)", s->url);
		else
			err(st, "bad server URL %s", s->url);
		return -1;
	}
	st->up = *u;
	st_url_dir(u);
	return set_target(st, u, "server", quiet);
}

/* average latency of s in microseconds; ST_LAT_FAIL when unreachable */
static int measure_latency(struct st *st, struct st_server *s)
{
	int r;

	s->latency_us = ST_LAT_FAIL;
	if (use_server(st, s, s != &st->custom) < 0)
		return 0;
	st->phase = PH_LATENCY;
	st->next_req = 0;
	st->lat_sum = st->lat_ok = st->lat_bad = 0;
	r = run(st, 1, 0, next_latency, done_latency);
	close_all(st);
	if (r == RUN_BREAK)
		return -2;
	if (st->lat_ok == ST_LAT_TRIES && !st->lat_bad)
		s->latency_us = st->lat_sum / ST_LAT_TRIES;
	dbg(st, "server %lu (%s): %lu us", (unsigned long)s->id, s->host, (unsigned long)s->latency_us);
	return 0;
}

static int pick_server(struct st *st)
{
	int i, n = st->nids ? st->nsrv : RT_MIN(st->nsrv, ST_CANDIDATES);

	info(st, "Selecting best server based on ping...\n");
	st->best = NULL;
	for (i = 0; i < n; i++) {
		if (measure_latency(st, &st->srv[i]) == -2)
			return -2;
		if (!st->best || st->srv[i].latency_us < st->best->latency_us)
			st->best = &st->srv[i];
	}
	if (!st->best || st->best->latency_us >= ST_LAT_FAIL) {
		if (st->last_err)
			err(st, "latency test: %s", errstr(st, st->last_err));
		err(st, "Unable to connect to servers to test latency.");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------ throughput */

static void dot(struct st *st)
{
	if (!st->quiet) {
		out_str(st, 0, ".");
		st->sys->flush(st->ctx);
	}
}

static int next_download(struct st *st, struct st_conn *c)
{
	u32 i = st->next_req, s;

	if (i >= st->nreqs)
		return 0;
	st->next_req++;
	s = st->dl_sizes[i / st->count];
	rt_snprintf(st->tmp, sizeof(st->tmp), "%srandom%lux%lu.jpg?x=%lu.%lu", st->base.path, (unsigned long)s,
	            (unsigned long)s, (unsigned long)st->sys->now_us(st->ctx), (unsigned long)i);
	return start(conn_request(st, c, "GET", st->tmp, 0));
}

static int next_upload(struct st *st, struct st_conn *c)
{
	u32 i = st->next_req;

	if (i >= st->nreqs)
		return 0;
	st->next_req++;
	rt_snprintf(st->tmp, sizeof(st->tmp), "%s%sx=%lu.%lu", st->up.path, strchr(st->up.path, '?') ? "&" : "?",
	            (unsigned long)st->sys->now_us(st->ctx), (unsigned long)i);
	return start(conn_request(st, c, "POST", st->tmp, st->ul_sizes[i / st->count]));
}

static void done_transfer(struct st *st, struct st_conn *c, int e)
{
	if (!e && c->resp.status != 200) {
		e = ST_EHTTP;
		note_err(st, c, e);
	}
	if (e) {
		st->failed_reqs++;
		dbg(st, "request failed: %s", errstr(st, e));
	} else {
		st->done_reqs++;
		dot(st);
	}
}

/* one throughput test; 0, -1 nothing transferred, -2 Ctrl-C */
static int transfer(struct st *st, int upload)
{
	/* speedtest-cli uses the configuration's threadcount * 2 (often 8) for
	 * the download; more than 4 only costs memory on a CPU bound Amiga */
	u32 nthreads = st->single ? 1 : st->threads ? st->threads :
	               upload ? (st->cfg_ul_threads ? RT_MIN(st->cfg_ul_threads, 4u) : 2) :
	               (st->cfg_threads ? RT_MIN(st->cfg_threads * 2, 4u) : 4);
	u32 secs = st->duration_s ? st->duration_s : upload ? st->cfg_ul_len : st->cfg_dl_len;
	u32 count = st->count, t0;
	int r;

	if (!count) {
		if (upload) {
			u32 m = st->cfg_ul_max ? st->cfg_ul_max : 50;
			count = (m + (u32)st->nul - 1) / (u32)st->nul;
		} else {
			count = st->cfg_dl_perurl ? st->cfg_dl_perurl : 4;
		}
	}
	if (nthreads > ST_MAXCONN)
		nthreads = ST_MAXCONN;
	if (!secs)
		secs = 10;
	st->count = count;
	st->nreqs = count * (u32)(upload ? st->nul : st->ndl);
	st->next_req = st->done_reqs = st->failed_reqs = 0;
	st->bytes = 0;
	st->last_err = 0;
	st->phase = upload ? PH_UPLOAD : PH_DOWNLOAD;
	dbg(st, "%s: %lu connections, %lu requests, %lu s", upload ? "upload" : "download", (unsigned long)nthreads,
	    (unsigned long)st->nreqs, (unsigned long)secs);
	t0 = st->sys->now_us(st->ctx);
	r = run(st, (int)nthreads, secs * 1000UL, upload ? next_upload : next_download, done_transfer);
	if (upload) {
		st->ul_us = st->sys->now_us(st->ctx) - t0;
		st->ul_bytes = st->bytes;
	} else {
		st->dl_us = st->sys->now_us(st->ctx) - t0;
		st->dl_bytes = st->bytes;
	}
	close_all(st);
	if (!st->user_count)
		st->count = 0;
	info(st, "\n");
	if (r == RUN_BREAK)
		return -2;
	dbg(st, "%llu bytes in %lu us, %lu requests done, %lu failed", (unsigned long long)st->bytes,
	    (unsigned long)(upload ? st->ul_us : st->dl_us), (unsigned long)st->done_reqs,
	    (unsigned long)st->failed_reqs);
	if (!st->bytes) {
		err(st, "%s failed: %s", upload ? "upload" : "download",
		    st->last_err ? errstr(st, st->last_err) : "no data");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------ results */

/* rate as "12.34" (Mbit/s or Mbyte/s) */
static char *rate(struct st *st, char *buf, u64 bytes, u32 us)
{
	return st_fmt_ratio(buf, 32, st->bytes_units ? bytes : bytes * 8, us, 2);
}

/* rate as "1234.5" KB/s (1024 bytes per second, as ftp shows it) */
static char *kbs(char *buf, u64 bytes, u32 us)
{
	return st_fmt_ratio(buf, 32, bytes * 1000000u, (u64)us * 1024u, 1);
}

static void json_str(struct st *st, const char *key, const char *v, int comma)
{
	char *o = st->line;
	u32 n = 0;

	n += (u32)rt_snprintf(o, sizeof(st->line), "%s\"%s\": \"", comma ? ", " : "", key);
	for (; *v && n < sizeof(st->line) - 8; v++) {
		u8 c = (u8)*v;
		if (c == '"' || c == '\\') {
			o[n++] = '\\';
			o[n++] = (char)c;
		} else if (c < 0x20) {
			n += (u32)rt_snprintf(o + n, 8, "\\u%04x", (unsigned)c);
		} else {
			o[n++] = (char)c;
		}
	}
	o[n++] = '"';
	o[n] = 0;
	out_str(st, 0, o);
}

static void csv_field(struct st *st, const char *v, int last)
{
	char *o = st->line;
	u32 n = 0;
	const char *p;
	int quote = 0;

	for (p = v; *p; p++)
		if (*p == st->csv_delim || *p == '"' || *p == '\n' || *p == '\r')
			quote = 1;
	if (quote)
		o[n++] = '"';
	for (; *v && n < sizeof(st->line) - 6; v++) {
		if (*v == '"')
			o[n++] = '"';
		o[n++] = *v;
	}
	if (quote)
		o[n++] = '"';
	o[n++] = last ? '\n' : st->csv_delim;
	o[n] = 0;
	out_str(st, 0, o);
}

static void csv_header(struct st *st)
{
	static const char *const h[] = {
		"Server ID", "Sponsor", "Server Name", "Timestamp", "Distance", "Ping", "Download", "Upload", "Share",
		"IP Address"
	};
	u32 i;

	for (i = 0; i < RT_ARRAY_SIZE(h); i++)
		csv_field(st, h[i], i + 1 == RT_ARRAY_SIZE(h));
}

static void report(struct st *st)
{
	struct st_server *s = st->best;
	char *ts = st->fbuf[0], *d = st->fbuf[1], *ping = st->fbuf[2], *dl = st->fbuf[3], *ul = st->fbuf[4];
	char *idb = st->fbuf[5], *t2 = st->fbuf[6];

	st_fmt_time(ts, 40, st->sys->unix_time(st->ctx));
	st_fmt_ratio(d, 40, s->dist, 100, 2);
	st_fmt_ratio(ping, 40, st->ping_us, 1000, 3);
	if (s->id)
		rt_snprintf(idb, 40, "%lu", (unsigned long)s->id);
	else
		idb[0] = 0;
	if (st->simple) {
		const char *u = st->bytes_units ? "byte" : "bit";
		say(st, "Ping: %s ms\n", ping);
		say(st, "Download: %s M%s/s (%s KB/s)\n", rate(st, dl, st->dl_bytes, st->dl_us), u,
		    kbs(t2, st->dl_bytes, st->dl_us));
		say(st, "Upload: %s M%s/s (%s KB/s)\n", rate(st, ul, st->ul_bytes, st->ul_us), u,
		    kbs(t2, st->ul_bytes, st->ul_us));
	} else if (st->csv) {
		/* bits per second, as speedtest-cli */
		st_fmt_ratio(dl, 40, st->dl_bytes * 8000000ULL, st->dl_us, 3);
		st_fmt_ratio(ul, 40, st->ul_bytes * 8000000ULL, st->ul_us, 3);
		csv_field(st, idb, 0);
		csv_field(st, s->sponsor, 0);
		csv_field(st, s->name, 0);
		rt_snprintf(t2, 40, "%s.000000Z", ts);
		csv_field(st, t2, 0);
		csv_field(st, d, 0);
		csv_field(st, ping, 0);
		csv_field(st, dl, 0);
		csv_field(st, ul, 0);
		csv_field(st, "", 0);
		csv_field(st, st->client_ip, 1);
	} else if (st->json) {
		st_fmt_ratio(dl, 40, st->dl_bytes * 8000000ULL, st->dl_us, 3);
		st_fmt_ratio(ul, 40, st->ul_bytes * 8000000ULL, st->ul_us, 3);
		say(st, "{\"download\": %s, \"upload\": %s, \"ping\": %s, \"server\": {", dl, ul, ping);
		json_str(st, "url", s->url, 0);
		json_str(st, "lat", s->slat, 1);
		json_str(st, "lon", s->slon, 1);
		json_str(st, "name", s->name, 1);
		json_str(st, "country", s->country, 1);
		json_str(st, "cc", s->cc, 1);
		json_str(st, "sponsor", s->sponsor, 1);
		json_str(st, "id", idb, 1);
		json_str(st, "host", s->host, 1);
		say(st, ", \"d\": %s, \"latency\": %s}, ", d, ping);
		rt_snprintf(st->tmp, sizeof(st->tmp), "%s.000000Z", ts);
		json_str(st, "timestamp", st->tmp, 0);
		say(st, ", \"bytes_sent\": %llu, \"bytes_received\": %llu, \"share\": null, \"client\": {",
		    (unsigned long long)st->ul_bytes, (unsigned long long)st->dl_bytes);
		json_str(st, "ip", st->client_ip, 0);
		json_str(st, "lat", st->client_lat, 1);
		json_str(st, "lon", st->client_lon, 1);
		json_str(st, "isp", st->client_isp, 1);
		say(st, "}}\n");
	}
}

/* ------------------------------------------------------------ options */

enum {
	O_NODL, O_NOUL, O_SINGLE, O_BYTES, O_SHARE, O_SIMPLE, O_CSV, O_CSVDELIM, O_CSVHDR, O_JSON, O_LIST,
	O_SERVER, O_EXCLUDE, O_MINI, O_SOURCE, O_TIMEOUT, O_SECURE, O_NOPRE, O_VERSION, O_HELP,
	O_SERVERURL, O_MIRROR, O_DURATION, O_THREADS, O_COUNT, O_DLSIZES, O_ULSIZES, O_DEBUG, O_TESTBREAK
};
static const struct opt {
	const char *name;
	int id, arg;
} opts[] = {
	{ "no-download", O_NODL, 0 }, { "no-upload", O_NOUL, 0 }, { "single", O_SINGLE, 0 },
	{ "bytes", O_BYTES, 0 }, { "share", O_SHARE, 0 }, { "simple", O_SIMPLE, 0 }, { "csv", O_CSV, 0 },
	{ "csv-delimiter", O_CSVDELIM, 1 }, { "csv-header", O_CSVHDR, 0 }, { "json", O_JSON, 0 },
	{ "list", O_LIST, 0 }, { "server", O_SERVER, 1 }, { "exclude", O_EXCLUDE, 1 }, { "mini", O_MINI, 1 },
	{ "source", O_SOURCE, 1 }, { "timeout", O_TIMEOUT, 1 }, { "secure", O_SECURE, 0 },
	{ "no-pre-allocate", O_NOPRE, 0 }, { "version", O_VERSION, 0 }, { "help", O_HELP, 0 },
	{ "server-url", O_SERVERURL, 1 }, { "mirror", O_MIRROR, 1 }, { "duration", O_DURATION, 1 },
	{ "threads", O_THREADS, 1 }, { "count", O_COUNT, 1 }, { "download-sizes", O_DLSIZES, 1 },
	{ "upload-sizes", O_ULSIZES, 1 }, { "debug", O_DEBUG, 0 }, { "test-break", O_TESTBREAK, 1 }
};

static void usage(struct st *st)
{
	out_str(st, 0,
	        "usage: speedtest [-h] [--no-download] [--no-upload] [--single] [--bytes]\n"
	        "                 [--simple] [--csv] [--csv-delimiter C] [--csv-header]\n"
	        "                 [--json] [--list] [--server ID] [--exclude ID]\n"
	        "                 [--timeout SECS] [--secure] [--version]\n"
	        "                 [--server-url URL] [--mirror URL] [--duration SECS]\n"
	        "                 [--threads N] [--count N] [--download-sizes S,...]\n"
	        "                 [--upload-sizes KB,...] [--debug]\n");
}

static void help(struct st *st)
{
	usage(st);
	out_str(st, 0,
	        "\nTest internet bandwidth using speedtest.net (options as speedtest-cli).\n\n"
	        "  --no-download, --no-upload  Skip a test\n"
	        "  --single              One connection instead of several\n"
	        "  --bytes               Bytes instead of bits\n"
	        "  --simple              Only Ping, Download and Upload\n"
	        "  --csv, --json         Only the results, as CSV or JSON (speeds in bit/s)\n"
	        "  --csv-delimiter C     CSV delimiter (default ,)\n"
	        "  --csv-header          Print the CSV header\n"
	        "  --list                List the servers by distance\n"
	        "  --server ID           Test against this server (repeatable)\n"
	        "  --exclude ID          Do not use this server (repeatable)\n"
	        "  --timeout SECS        HTTP timeout (default 10)\n"
	        "  --secure              HTTPS (not supported: no TLS)\n"
	        "  --version             Show the version\n"
	        "  --server-url URL      Test against http://HOST:PORT/speedtest/upload.php\n"
	        "                        without the configuration and server list\n"
	        "  --mirror URL          Read those from this http:// base\n"
	        "  --duration SECS       Length of each test (default 10)\n"
	        "  --threads N           Connections (default 4 down, 2 up)\n"
	        "  --count N             Requests per size\n"
	        "  --download-sizes S,.. Image sizes (350,500,...,4000)\n"
	        "  --upload-sizes KB,..  Upload sizes (128,256,512,1024,2048)\n"
	        "  --debug               Show the requests\n");
}

static int parse_num(const char *s, u32 max, u32 *out)
{
	const char *end;
	unsigned long v;

	if (!s || *s < '0' || *s > '9')
		return -1;
	v = rt_strtoul(s, &end, 10);
	if (*end || v > max)
		return -1;
	*out = (u32)v;
	return 0;
}

/* "1.5" seconds -> 1500 ms */
static int parse_secs(const char *s, u32 *ms)
{
	u32 v = 0, frac = 0, scale = 1000;

	if (!s || !*s)
		return -1;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (u32)(*s++ - '0');
		if (v > 1000)
			return -1;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			scale /= 10;
			frac += (u32)(*s++ - '0') * scale;
		}
	}
	if (*s)
		return -1;
	*ms = v * 1000 + frac;
	return 0;
}

static int parse_list(const char *s, u32 *out, int *n, u32 mult, u32 max)
{
	*n = 0;
	while (*s) {
		const char *end;
		unsigned long v = rt_strtoul(s, &end, 10);
		if (end == s || v == 0 || v > max || *n >= ST_MAXSIZES || (*end && *end != ','))
			return -1;
		out[(*n)++] = (u32)v * mult;
		s = *end ? end + 1 : end;
	}
	return *n ? 0 : -1;
}

/* 0 go on, 1 exit ok, -1 error */
static int parse_args(struct st *st, int argc, char **argv)
{
	int i, j;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *val = NULL, *eq;
		const struct opt *o = NULL;
		u32 nlen, v;
		if (!strcmp(a, "-h") || !strcmp(a, "-?") || !strcmp(a, "?")) {
			help(st);
			return 1;
		}
		if (a[0] != '-' || a[1] != '-' || !a[2]) {
			usage(st);
			err(st, "unrecognized arguments: %s", a);
			return -1;
		}
		a += 2;
		eq = strchr(a, '=');
		nlen = eq ? (u32)(eq - a) : (u32)strlen(a);
		/* exact name, else a unique prefix (as argparse) */
		for (j = 0; j < (int)RT_ARRAY_SIZE(opts); j++) {
			if (!strncmp(opts[j].name, a, nlen)) {
				if (strlen(opts[j].name) == nlen) {
					o = &opts[j];
					break;
				}
				if (o) {
					usage(st);
					err(st, "ambiguous option: --%.*s", (int)nlen, a);
					return -1;
				}
				o = &opts[j];
			}
		}
		if (!o) {
			usage(st);
			err(st, "unrecognized arguments: %s", argv[i]);
			return -1;
		}
		if (o->arg) {
			if (eq)
				val = eq + 1;
			else if (i + 1 < argc)
				val = argv[++i];
			else {
				usage(st);
				err(st, "argument --%s: expected one argument", o->name);
				return -1;
			}
		} else if (eq) {
			err(st, "argument --%s: ignored explicit argument '%s'", o->name, eq + 1);
			return -1;
		}
		switch (o->id) {
		case O_NODL: st->no_dl = 1; break;
		case O_NOUL: st->no_ul = 1; break;
		case O_SINGLE: st->single = 1; break;
		case O_BYTES: st->bytes_units = 1; break;
		case O_SIMPLE: st->simple = 1; break;
		case O_CSV: st->csv = 1; break;
		case O_CSVHDR: st->csv_header = 1; break;
		case O_JSON: st->json = 1; break;
		case O_LIST: st->list = 1; break;
		case O_NOPRE: break;
		case O_DEBUG: st->debug = 1; break;
		case O_VERSION:
			say(st, "speedtest-riptide " ST_VERSION "\n"
			        "A clean-room C implementation of the speedtest.net HTTP test for Riptide.\n"
			        "Options and output follow speedtest-cli by Matt Martz (Apache-2.0).\n");
			return 1;
		case O_HELP: help(st); return 1;
		case O_CSVDELIM:
			if (strlen(val) != 1) {
				err(st, "--csv-delimiter must be a single character");
				return -1;
			}
			st->csv_delim = val[0];
			break;
		case O_SERVER:
		case O_EXCLUDE:
			if (parse_num(val, 0xffffffffUL, &v) < 0) {
				err(st, "argument --%s: invalid server ID '%s'", o->name, val);
				return -1;
			}
			if (o->id == O_SERVER && st->nids < ST_MAXIDS)
				st->ids[st->nids++] = v;
			else if (o->id == O_EXCLUDE && st->nexcl < ST_MAXIDS)
				st->excl[st->nexcl++] = v;
			break;
		case O_TIMEOUT:
			if (parse_secs(val, &st->timeout_ms) < 0 || !st->timeout_ms) {
				err(st, "argument --timeout: invalid value '%s'", val);
				return -1;
			}
			break;
		case O_DURATION:
			if (parse_num(val, 3600, &st->duration_s) < 0 || !st->duration_s) {
				err(st, "argument --duration: invalid value '%s'", val);
				return -1;
			}
			break;
		case O_THREADS:
			if (parse_num(val, ST_MAXCONN, &st->threads) < 0 || !st->threads) {
				err(st, "argument --threads: 1 to %d", ST_MAXCONN);
				return -1;
			}
			break;
		case O_COUNT:
			if (parse_num(val, 1000, &st->count) < 0 || !st->count) {
				err(st, "argument --count: invalid value '%s'", val);
				return -1;
			}
			st->user_count = 1;
			break;
		case O_TESTBREAK:
			if (parse_num(val, 3600000UL, &st->test_break_ms) < 0) {
				err(st, "argument --test-break: invalid value '%s'", val);
				return -1;
			}
			break;
		case O_DLSIZES:
			if (parse_list(val, st->dl_sizes, &st->ndl, 1, 10000) < 0) {
				err(st, "argument --download-sizes: a list like 350,500,1000");
				return -1;
			}
			break;
		case O_ULSIZES:
			if (parse_list(val, st->ul_sizes, &st->nul, 1024, 65536) < 0) {
				err(st, "argument --upload-sizes: a list of KB like 128,256");
				return -1;
			}
			break;
		case O_SERVERURL: st->server_url = val; break;
		case O_MIRROR: st->mirror = val; break;
		case O_SECURE:
			err(st, "--secure: HTTPS is not supported (Riptide has no TLS)");
			return -1;
		case O_SHARE:
			err(st, "--share is not supported (the result upload needs HTTPS)");
			return -1;
		case O_MINI:
		case O_SOURCE:
			err(st, "--%s is not supported", o->name);
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------ main */

static int list_servers(struct st *st)
{
	int i;
	char d[40];

	for (i = 0; i < st->nsrv; i++) {
		struct st_server *s = &st->srv[i];
		say(st, "%5lu) %s (%s, %s) [%s km]\n", (unsigned long)s->id, s->sponsor, s->name, s->country,
		    st_fmt_ratio(d, 40, s->dist, 100, 2));
	}
	if (st->matched_srv > (u32)st->nsrv)
		say(st, "(%lu more servers not shown)\n", (unsigned long)(st->matched_srv - (u32)st->nsrv));
	return ST_EXIT_OK;
}

static int custom_server(struct st *st)
{
	struct st_server *s = &st->custom;
	struct st_url *u = &st->u1;
	int r = st_url_parse(st->server_url, u);

	if (r == ST_URL_HTTPS) {
		err(st, "--server-url %s: HTTPS is not supported (no TLS)", st->server_url);
		return -1;
	}
	if (r < 0) {
		err(st, "--server-url: bad URL %s (use http://HOST:PORT/speedtest/upload.php)", st->server_url);
		return -1;
	}
	rt_strlcpy(s->url, st->server_url, sizeof(s->url));
	if (u->port == 80)
		rt_strlcpy(s->host, u->host, sizeof(s->host));
	else
		rt_snprintf(s->host, sizeof(s->host), "%s:%u", u->host, (unsigned)u->port);
	rt_strlcpy(s->sponsor, s->host, sizeof(s->sponsor));
	rt_strlcpy(s->name, "custom server", sizeof(s->name));
	return 0;
}

static int st_run(struct st *st, int argc, char **argv)
{
	char *buf = st->fbuf[7], *d = st->fbuf[1];
	int r;

	r = parse_args(st, argc, argv);
	if (r)
		return r > 0 ? ST_EXIT_OK : ST_EXIT_ERROR;
	if (st->csv_header) {
		csv_header(st);
		return ST_EXIT_OK;
	}
	st->quiet = st->simple || st->json || st->csv || st->list;
	if (!st->ndl) {
		memcpy(st->dl_sizes, dl_default, sizeof(dl_default));
		st->ndl = (int)RT_ARRAY_SIZE(dl_default);
	}
	if (!st->nul) {
		memcpy(st->ul_sizes, ul_default, sizeof(ul_default));
		st->nul = (int)RT_ARRAY_SIZE(ul_default);
	}
	st->cap = st->list ? ST_LISTMAX : ST_KEEP;
	st->srv = st->sys->alloc(st->ctx, (u32)st->cap * sizeof(struct st_server));
	st->conn = st->sys->alloc(st->ctx, ST_MAXCONN * sizeof(struct st_conn));
	st->rx = st->sys->alloc(st->ctx, ST_RXBUF);
	st->pat = st->sys->alloc(st->ctx, ST_PATSZ + 9);
	st->xml = st->sys->alloc(st->ctx, sizeof(struct st_xml));
	if (!st->srv || !st->conn || !st->rx || !st->pat || !st->xml) {
		err(st, "out of memory");
		return ST_EXIT_FAIL;
	}
	{
		u32 i, x = 0x2545f491UL ^ st->sys->now_us(st->ctx);
		static const char an[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		memcpy(st->pat, "content1=", 9);
		for (i = 0; i < ST_PATSZ; i++) {
			x = x * 1103515245UL + 12345UL;
			st->pat[9 + i] = an[(x >> 16) % 36];
		}
		for (i = 0; i < ST_MAXCONN; i++) {
			st->conn[i].fd = -1;
			st->conn[i].st = st;
		}
	}
	if (st->server_url) {
		if (st->list) {
			err(st, "--list needs the server list (not --server-url)");
			return ST_EXIT_ERROR;
		}
		if (custom_server(st) < 0)
			return ST_EXIT_ERROR;
		info(st, "Testing against %s...\n", st->custom.url);
		if (measure_latency(st, &st->custom) == -2)
			return -2;
		if (st->custom.latency_us >= ST_LAT_FAIL) {
			if (st->last_err)
				err(st, "%s: %s", st->custom.url, errstr(st, st->last_err));
			err(st, "Unable to connect to the server to test latency.");
			return ST_EXIT_ERROR;
		}
		st->best = &st->custom;
		st_fmt_ratio(buf, 40, st->best->latency_us, 1000, 3);
		info(st, "Hosted by %s (%s): %s ms\n", st->best->sponsor, st->best->name, buf);
	} else {
		r = get_config(st);
		if (r < 0)
			return r == -2 ? -2 : ST_EXIT_ERROR;
		info(st, "Testing from %s (%s)...\n", st->client_isp, st->client_ip);
		r = get_servers(st);
		if (r < 0)
			return r == -2 ? -2 : ST_EXIT_ERROR;
		if (st->list)
			return list_servers(st);
		r = pick_server(st);
		if (r < 0)
			return r == -2 ? -2 : ST_EXIT_ERROR;
		st_fmt_ratio(buf, 40, st->best->latency_us, 1000, 3);
		info(st, "Hosted by %s (%s) [%s km]: %s ms\n", st->best->sponsor, st->best->name,
		     st_fmt_ratio(d, 40, st->best->dist, 100, 2), buf);
		if (use_server(st, st->best, 0) < 0)
			return ST_EXIT_ERROR;
	}
	st->ping_us = st->best->latency_us;
	if (st->no_dl) {
		info(st, "Skipping download test\n");
	} else {
		info(st, "Testing download speed");
		r = transfer(st, 0);
		if (r == -2)
			return -2;
		if (r < 0)
			return ST_EXIT_ERROR;
		info(st, "Download: %s M%s/s (%s KB/s)\n", rate(st, buf, st->dl_bytes, st->dl_us),
		     st->bytes_units ? "byte" : "bit", kbs(st->fbuf[6], st->dl_bytes, st->dl_us));
	}
	if (st->no_ul) {
		info(st, "Skipping upload test\n");
	} else {
		info(st, "Testing upload speed");
		r = transfer(st, 1);
		if (r == -2)
			return -2;
		if (r < 0)
			return ST_EXIT_ERROR;
		info(st, "Upload: %s M%s/s (%s KB/s)\n", rate(st, buf, st->ul_bytes, st->ul_us),
		     st->bytes_units ? "byte" : "bit", kbs(st->fbuf[6], st->ul_bytes, st->ul_us));
	}
	report(st);
	return ST_EXIT_OK;
}

int st_main(int argc, char **argv, const struct st_sys *sys)
{
	struct st *st = sys->alloc(sys->ctx, sizeof(*st));
	int rc;

	if (!st) {
		sys->out(sys->ctx, 1, "speedtest: out of memory\n", 25);
		return ST_EXIT_FAIL;
	}
	st->sys = sys;
	st->ctx = sys->ctx;
	st->csv_delim = ',';
	st->timeout_ms = 10000;
	st->cfg_dl_len = st->cfg_ul_len = 10;
	st->t_prog = sys->now_us(sys->ctx);
	rc = st_run(st, argc, argv);
	if (rc == -2) {
		sys->flush(sys->ctx);
		out_str(st, 1, "\nCancelling...\n");
		rc = ST_EXIT_BREAK;
	}
	if (st->conn)
		close_all(st);
	sys->flush(sys->ctx);
	if (st->srv)
		sys->free(sys->ctx, st->srv);
	if (st->conn)
		sys->free(sys->ctx, st->conn);
	if (st->rx)
		sys->free(sys->ctx, st->rx);
	if (st->pat)
		sys->free(sys->ctx, st->pat);
	if (st->xml)
		sys->free(sys->ctx, st->xml);
	sys->free(sys->ctx, st);
	return rc;
}
