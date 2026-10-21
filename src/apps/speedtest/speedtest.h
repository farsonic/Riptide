/*
 * Riptide - speedtest: a bandwidth test against speedtest.net servers (the
 * legacy HTTP protocol), in portable C. An original implementation; its
 * options and output follow speedtest-cli by Matt Martz
 * (https://github.com/sivel/speedtest-cli, Apache-2.0), which served as
 * the reference for the behaviour. No code was taken from it.
 *
 * The protocol, HTTP client, XML reader and measurement code here use no
 * OS calls: everything goes through struct st_sys, which a thin shim
 * implements for AmigaOS (src/amiga/cmd/speedtest.c), Linux sockets
 * (st_posix.c) and Riptide's host stack (tests/t_speedtest_tun.c).
 *
 * Rules as for the core: C99, no floating point (rates and distances are
 * fixed point), small stack frames (all state is allocated), no stdio.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef ST_SPEEDTEST_H
#define ST_SPEEDTEST_H

#include "rt/rt_types.h"
#include "rt/rt_errno.h"

#define ST_VERSION "1.0"

/* exit codes (AmigaDOS conventions, as the other tools) */
#define ST_EXIT_OK    0
#define ST_EXIT_BREAK 5
#define ST_EXIT_ERROR 10
#define ST_EXIT_FAIL  20

/* ---- the OS shim. Errors are negative RT_E* values (BSD numbering). ---- */
struct st_sys {
	void *ctx;
	/* host name or dotted quad -> IPv4 address (network order); 0 or -1 */
	int  (*resolve)(void *ctx, const char *host, u32 *addr);
	/* a non-blocking TCP socket with the connect started: fd >= 0 or -errno */
	int  (*connect)(void *ctx, u32 addr, u16 port);
	/* result of a connect that has become writable: 0 or the (positive) errno */
	int  (*conn_error)(void *ctx, int fd);
	/* bytes moved (recv: 0 = EOF), -RT_EWOULDBLOCK, or -errno */
	int  (*send)(void *ctx, int fd, const void *p, u32 n);
	int  (*recv)(void *ctx, int fd, void *p, u32 n);
	void (*close)(void *ctx, int fd);
	/* Wait until a descriptor of rd/wr is ready or ms milliseconds pass.
	 * Sets are u32 words, bit fd%32 of word fd/32 (the WaitSelect layout),
	 * and hold the ready descriptors on return. Returns the number ready,
	 * 0 on timeout, -1 on Ctrl-C (or EINTR), -2 on other errors. */
	int  (*wait)(void *ctx, int nfds, u32 *rd, u32 *wr, u32 ms);
	u32  (*now_us)(void *ctx);           /* monotonic microseconds, wraps */
	u32  (*unix_time)(void *ctx);        /* seconds since 1970-01-01 */
	int  (*brk)(void *ctx);              /* Ctrl-C pending? (clears it) */
	void (*raise_brk)(void *ctx);        /* test hook: deliver a Ctrl-C */
	void (*out)(void *ctx, int to_stderr, const char *s, u32 n);
	void (*flush)(void *ctx);
	void *(*alloc)(void *ctx, u32 n);    /* zeroed memory or NULL */
	void (*free)(void *ctx, void *p);
	const char *(*strerror)(void *ctx, int err);
	int  maxfd;                          /* descriptors must be below this (<= 256) */
	const char *ua;                      /* User-Agent platform: "AmigaOS; m68k" */
};

/* Run the speedtest command line (argv[0] is the program name). */
int st_main(int argc, char **argv, const struct st_sys *sys);

/* =====================================================================
 * Building blocks, exposed for the host unit tests.
 * ===================================================================== */

/* ---- streaming XML reader: reports start (and empty) tags with their
 * attributes; text, end tags, comments, CDATA, <?..?> and <!..> are
 * skipped. Input may be split anywhere. Entities in attribute values are
 * decoded (&amp; &lt; &gt; &quot; &apos; &#N; &#xH; -> UTF-8). ---- */
#define ST_XML_TAGMAX  1024     /* longer tags are dropped (counted) */
#define ST_XML_MAXATTR 24
struct st_xml_attr {
	const char *name, *value;
};
typedef void (*st_xml_cb)(void *arg, const char *tag, int nattr, const struct st_xml_attr *attr);
struct st_xml {
	st_xml_cb cb;
	void *arg;
	int state, quote, overflow;
	u32 len;
	u32 tags, dropped;           /* reported, dropped (too long) */
	char buf[ST_XML_TAGMAX];
};
void st_xml_init(struct st_xml *x, st_xml_cb cb, void *arg);
void st_xml_feed(struct st_xml *x, const char *p, u32 n);
const char *st_xml_attr(int nattr, const struct st_xml_attr *a, const char *name);
/* decode entities in place; returns the new length */
u32 st_xml_unescape(char *s, u32 n);

/* ---- URLs (http only) ---- */
#define ST_HOSTMAX 96
#define ST_PATHMAX 384
struct st_url {
	char host[ST_HOSTMAX];
	u16 port;
	char path[ST_PATHMAX];          /* starts with '/', may hold "?query" */
};
#define ST_URL_BAD   (-1)
#define ST_URL_HTTPS (-2)
int st_url_parse(const char *s, struct st_url *u);
/* a Location header relative to base (absolute, //host/x, /x or x) */
int st_url_join(const struct st_url *base, const char *loc, struct st_url *out);
/* the directory of u's path ("/speedtest/upload.php" -> "/speedtest/") */
void st_url_dir(struct st_url *u);

/* ---- HTTP/1.1 response parser (status line, headers, Content-Length,
 * chunked or close-delimited bodies). Body bytes go to sink. ---- */
#define ST_LINEMAX 512
#define ST_HDRMAX  65536
enum {
	ST_R_STATUS, ST_R_HEADER, ST_R_BODY, ST_R_BODY_CLOSE, ST_R_CHUNK_SIZE,
	ST_R_CHUNK_DATA, ST_R_CHUNK_END, ST_R_TRAILER, ST_R_DONE, ST_R_ERROR
};
typedef void (*st_sink)(void *arg, const char *p, u32 n);
struct st_resp {
	int state, status, minor;
	int chunked, close, has_len, head, keepalive_hdr;
	u64 clen, left, body;
	u32 hdrbytes, linelen;
	const char *error;
	st_sink sink;
	void *arg;
	char line[ST_LINEMAX];
	char location[ST_PATHMAX];
};
void st_resp_init(struct st_resp *r, int head, st_sink sink, void *arg);
/* Consume up to n bytes; returns how many were used (less than n only
 * when the response ends or fails: check r->state). */
u32  st_resp_feed(struct st_resp *r, const char *p, u32 n);
/* The connection closed: 0 if that completes the response, -1 if not. */
int  st_resp_eof(struct st_resp *r);
/* Can the connection carry another request after this response? */
int  st_resp_reusable(const struct st_resp *r);

/* ---- numbers and distances ---- */
/* "-33.8678" -> -33867800 micro-degrees; 0 or -1 */
int  st_parse_coord(const char *s, s32 *udeg);
/* Distance in units of 10 m (km * 100), equirectangular approximation
 * with an integer cosine table: within 0.5% up to ~1000 km, which is all
 * that ranking the nearest servers needs. */
u32  st_distance(s32 lat1, s32 lon1, s32 lat2, s32 lon2);
u64  st_isqrt64(u64 v);
/* bits per second * 1000 is not representable; these give the parts */
/* value*scale/us as integer and 'dec' decimals: "12.34" */
char *st_fmt_ratio(char *buf, u32 size, u64 num, u64 den, int dec);
/* text of a (positive) RT_E* error number */
const char *st_strerror(int e);
/* unix time -> "2026-10-21T12:34:56" */
char *st_fmt_time(char *buf, u32 size, u32 t);

#endif
