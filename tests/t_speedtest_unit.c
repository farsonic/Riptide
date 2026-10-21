/*
 * Riptide - speedtest building blocks (src/apps/speedtest): the streaming
 * XML reader (attributes, quoting, entities, comments/CDATA, splits at
 * every byte, oversized tags, random input), URLs and redirects, the HTTP
 * response parser (Content-Length, chunked, close-delimited, 1xx, errors,
 * every split point), distances and ranking, fixed-point formatting.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speedtest.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 rnd_state = 12345;
static u32 rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

/* ------------------------------------------------------------ XML */

static char log_buf[1 << 20];
static u32 log_len;

static void log_tag(void *arg, const char *tag, int n, const struct st_xml_attr *a)
{
	int i;
	(void)arg;
	log_len += (u32)snprintf(log_buf + log_len, sizeof(log_buf) - log_len, "<%s", tag);
	for (i = 0; i < n && log_len < sizeof(log_buf) - 200; i++)
		log_len += (u32)snprintf(log_buf + log_len, sizeof(log_buf) - log_len, " %s=[%s]", a[i].name, a[i].value);
	if (log_len < sizeof(log_buf) - 2)
		log_buf[log_len++] = '>', log_buf[log_len] = 0;
}

static struct st_xml xml;

static const char *xml_whole(const char *doc)
{
	log_len = 0;
	log_buf[0] = 0;
	st_xml_init(&xml, log_tag, NULL);
	st_xml_feed(&xml, doc, (u32)strlen(doc));
	return log_buf;
}

/* feed in pieces of random size (or 1 byte), compare with the whole */
static int xml_split_same(const char *doc, int bytewise)
{
	static char whole[1 << 16];
	u32 n = (u32)strlen(doc), pos = 0;

	snprintf(whole, sizeof(whole), "%s", xml_whole(doc));
	log_len = 0;
	log_buf[0] = 0;
	st_xml_init(&xml, log_tag, NULL);
	while (pos < n) {
		u32 k = bytewise ? 1 : 1 + rnd() % 37;
		if (k > n - pos)
			k = n - pos;
		st_xml_feed(&xml, doc + pos, k);
		pos += k;
	}
	if (strcmp(whole, log_buf))
		fprintf(stderr, "split differs:\n  %s\n  %s\n", whole, log_buf);
	return !strcmp(whole, log_buf);
}

static const char *sample =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<!DOCTYPE settings [ <!ENTITY x \"<server id='0'/>\"> ]>\n"
	"<settings>\n"
	"<!-- <server url=\"http://commented/\" id=\"666\"/> -- still a comment -->\n"
	"<client ip=\"1.2.3.4\" lat=\"-33.86\" lon=\"151.21\" isp=\"A &amp; B &lt;c&gt; &quot;q&quot; &apos;s&apos;\"/>\n"
	"<server-config threadcount = '4' ignoreids=\"1,2,3\" />\n"
	"<![CDATA[ <server id=\"777\"/> ]] ]]>\n"
	"<server url='http://h:8080/speedtest/upload.php' lat=\"1\" name=\"x > y\" sponsor='it\"s' id=7 host=\"h:8080\"/>\n"
	"<server id=\"8\" url=\"http://a/b\" name=\"M&#252;nchen &#x20AC; &#128512;\" country=\"&unknown; & raw\"></server>\n"
	"<odd  novalue  other = \"v\"/>\n"
	"text with > and & outside tags\n"
	"</settings>\n";

static void test_xml(void)
{
	const char *r = xml_whole(sample);
	char *big;
	u32 i, n;

	CHECK(strstr(r, "<settings>"));
	CHECK(!strstr(r, "666") && !strstr(r, "777") && !strstr(r, "id=[0]"));
	CHECK(strstr(r, "<client ip=[1.2.3.4] lat=[-33.86] lon=[151.21] isp=[A & B <c> \"q\" 's']>"));
	CHECK(strstr(r, "<server-config threadcount=[4] ignoreids=[1,2,3]>"));
	CHECK(strstr(r, "<server url=[http://h:8080/speedtest/upload.php] lat=[1] name=[x > y] sponsor=[it\"s] id=[7] host=[h:8080]>"));
	CHECK(strstr(r, "name=[M\xc3\xbcnchen \xe2\x82\xac \xf0\x9f\x98\x80]"));
	CHECK(strstr(r, "country=[&unknown; & raw]"));
	CHECK(strstr(r, "<odd novalue=[] other=[v]>"));
	CHECK(xml.tags == 6);
	CHECK(xml_split_same(sample, 1));
	for (i = 0; i < 200; i++)
		CHECK(xml_split_same(sample, 0));

	/* oversized tags are dropped, the next one is read */
	big = malloc(10000);
	n = (u32)snprintf(big, 10000, "<server a=\"");
	memset(big + n, 'x', 5000);
	n += 5000;
	n += (u32)snprintf(big + n, 10000 - n, "\" id=\"1\"/><server id=\"2\"/>");
	r = xml_whole(big);
	CHECK(xml.dropped == 1 && xml.tags == 1 && !strcmp(r, "<server id=[2]>"));
	/* attribute count limit */
	n = (u32)snprintf(big, 10000, "<t");
	for (i = 0; i < 40; i++)
		n += (u32)snprintf(big + n, 10000 - n, " a%u=\"%u\"", (unsigned)i, (unsigned)i);
	snprintf(big + n, 10000 - n, "/>");
	r = xml_whole(big);
	CHECK(strstr(r, "a23=[23]>") && !strstr(r, "a24"));
	/* unterminated things at the end do nothing */
	CHECK(!strcmp(xml_whole("<a x=\"1\"/><b y=\"unterminated"), "<a x=[1]>"));
	CHECK(!strcmp(xml_whole("<!-- open comment <a/>"), ""));
	CHECK(!strcmp(xml_whole("<a/><!----><b/>"), "<a><b>"));
	CHECK(!strcmp(xml_whole("<a/><!-- x --->< b/><c/>"), "<a><c>"));
	CHECK(!strcmp(xml_whole("<x a=\"&#0;&#1114112;&#xZZ;&#;\"/>"), "<x a=[&#0;&#1114112;&#xZZ;&#;]>"));

	/* random input: no crash (ASan), and splitting does not matter */
	for (i = 0; i < 300; i++) {
		static const char alpha[] = "<>/=\"' !-[]?&#;axmpCDATAquot\n";
		u32 j, len = 1 + rnd() % 600;
		for (j = 0; j < len; j++)
			big[j] = (rnd() % 8) ? alpha[rnd() % (sizeof(alpha) - 1)] : (char)(1 + rnd() % 255);
		big[len] = 0;
		CHECK(xml_split_same(big, i & 1));
	}
	free(big);

	/* a huge list: 20000 servers in 7 byte pieces */
	{
		u32 cap = 20000 * 200, len = 0, pos = 0;
		char *doc = malloc(cap);
		len += (u32)snprintf(doc, cap, "<settings><servers>");
		for (i = 0; i < 20000; i++)
			len += (u32)snprintf(doc + len, cap - len,
			                     "<server url=\"http://s%u.example:8080/speedtest/upload.php\" lat=\"%d.%u\" lon=\"%d\" "
			                     "name=\"N%u\" id=\"%u\"/>\n", (unsigned)i, (int)(i % 180) - 90, (unsigned)i % 10,
			                     (int)(i % 360) - 180, (unsigned)i, (unsigned)i);
		len += (u32)snprintf(doc + len, cap - len, "</servers></settings>");
		st_xml_init(&xml, log_tag, NULL);
		log_len = 0;
		while (pos < len) {
			u32 k = RT_MIN(7u, len - pos);
			if (log_len > sizeof(log_buf) / 2)
				log_len = 0;
			st_xml_feed(&xml, doc + pos, k);
			pos += k;
		}
		CHECK(xml.tags == 20002 && xml.dropped == 0);
		free(doc);
	}
}

/* ------------------------------------------------------------ URLs */

static void test_url(void)
{
	struct st_url u, b, o;

	CHECK(st_url_parse("http://www.speedtest.net/speedtest-config.php", &u) == 0 &&
	      !strcmp(u.host, "www.speedtest.net") && u.port == 80 && !strcmp(u.path, "/speedtest-config.php"));
	CHECK(st_url_parse("HTTP://h:8080", &u) == 0 && !strcmp(u.host, "h") && u.port == 8080 && !strcmp(u.path, "/"));
	CHECK(st_url_parse("http://h:8080?x=1#frag", &u) == 0 && !strcmp(u.path, "/?x=1"));
	CHECK(st_url_parse("http://user:pw@h/x", &u) == 0 && !strcmp(u.host, "h") && !strcmp(u.path, "/x"));
	CHECK(st_url_parse("  http://10.0.2.2:8080/speedtest/upload.php", &u) == 0 && !strcmp(u.host, "10.0.2.2"));
	CHECK(st_url_parse("https://www.speedtest.net/", &u) == ST_URL_HTTPS);
	CHECK(st_url_parse("ftp://h/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://:80/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://h:/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://h:0/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://h:65536/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://h:8x/", &u) == ST_URL_BAD);
	CHECK(st_url_parse("http://[::1]:80/", &u) == ST_URL_BAD);
	{
		char longp[600];
		memset(longp, 'a', sizeof(longp));
		memcpy(longp, "http://h/", 9);
		longp[599] = 0;
		CHECK(st_url_parse(longp, &u) == ST_URL_BAD);
		memcpy(longp, "http://", 7);
		memset(longp + 7, 'h', 200);
		longp[207] = 0;
		CHECK(st_url_parse(longp, &u) == ST_URL_BAD);
	}
	st_url_parse("http://h:8080/speedtest/upload.php?a=b", &u);
	st_url_dir(&u);
	CHECK(!strcmp(u.path, "/speedtest/"));
	st_url_parse("http://h/upload.php", &u);
	st_url_dir(&u);
	CHECK(!strcmp(u.path, "/"));

	st_url_parse("http://base:81/dir/page.php?q", &b);
	CHECK(st_url_join(&b, "https://www.speedtest.net/x", &o) == ST_URL_HTTPS);
	CHECK(st_url_join(&b, "http://other/y", &o) == 0 && !strcmp(o.host, "other") && o.port == 80 && !strcmp(o.path, "/y"));
	CHECK(st_url_join(&b, "//cdn:82/z", &o) == 0 && !strcmp(o.host, "cdn") && o.port == 82 && !strcmp(o.path, "/z"));
	CHECK(st_url_join(&b, "/abs?x=1#f", &o) == 0 && !strcmp(o.host, "base") && o.port == 81 && !strcmp(o.path, "/abs?x=1"));
	CHECK(st_url_join(&b, "rel.php", &o) == 0 && !strcmp(o.path, "/dir/rel.php"));
	CHECK(st_url_join(&b, "mailto:x@y", &o) == ST_URL_BAD);
	{
		char longp[500];
		memset(longp, 'r', sizeof(longp) - 1);
		longp[499] = 0;
		CHECK(st_url_join(&b, longp, &o) == ST_URL_BAD);
	}
}

/* ------------------------------------------------------------ HTTP */

static char body[1 << 16];
static u32 body_len;

static void sink(void *arg, const char *p, u32 n)
{
	(void)arg;
	if (body_len + n < sizeof(body)) {
		memcpy(body + body_len, p, n);
		body_len += n;
		body[body_len] = 0;
	}
}

static struct st_resp resp;

/* feed in pieces of 'step' bytes (0: all at once); returns bytes used */
static u32 feed(const char *s, u32 step, int head)
{
	u32 n = (u32)strlen(s), pos = 0;

	st_resp_init(&resp, head, sink, NULL);
	body_len = 0;
	body[0] = 0;
	while (pos < n) {
		u32 k = step ? RT_MIN(step, n - pos) : n - pos, used;
		used = st_resp_feed(&resp, s + pos, k);
		pos += used;
		if (used < k)
			break;
	}
	return pos;
}

static void test_http_split(const char *s, int status, const char *expect_body, int reusable)
{
	u32 step, n = (u32)strlen(s);

	for (step = 0; step <= n; step = step ? step + 1 + (step > 20) * 7 : 1) {
		u32 used = feed(s, step, 0);
		int ok = resp.state == ST_R_DONE && resp.status == status && !strcmp(body, expect_body) &&
		         st_resp_reusable(&resp) == reusable && used == n;
		CHECK(ok);
		if (!ok) {
			fprintf(stderr, "  step %u: state %d status %d body [%s] used %u/%u err %s\n", (unsigned)step,
			        resp.state, resp.status, body, (unsigned)used, (unsigned)n, resp.error ? resp.error : "-");
			break;
		}
	}
}

static void expect_error(const char *s, const char *why)
{
	feed(s, 0, 0);
	CHECK(resp.state == ST_R_ERROR);
	if (resp.state != ST_R_ERROR)
		fprintf(stderr, "  expected an error (%s): state %d\n", why, resp.state);
	feed(s, 1, 0);
	CHECK(resp.state == ST_R_ERROR);
}

static void test_http(void)
{
	char *big;
	u32 i, n;

	test_http_split("HTTP/1.1 200 OK\r\nContent-Length: 9\r\nContent-Type: text/plain\r\n\r\ntest=test", 200,
	                "test=test", 1);
	test_http_split("HTTP/1.1 200 OK\r\ncontent-length:  5 \r\nConnection: keep-alive, Close\r\n\r\nhello", 200, "hello", 0);
	test_http_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                "4\r\nWiki\r\n5;ext=1\r\npedia\r\nE \r\n in\r\n\r\nchunks.\r\n0\r\nX-Trailer: 1\r\n\r\n", 200,
	                "Wikipedia in\r\n\r\nchunks.", 1);
	/* chunked wins over Content-Length (RFC 9112 6.3) */
	test_http_split("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
	                "3\r\nabc\r\n0\r\n\r\n", 200, "abc", 1);
	/* LF only */
	test_http_split("HTTP/1.1 200 OK\nContent-Length: 2\n\nok", 200, "ok", 1);
	/* interim responses */
	test_http_split("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 102 Processing\r\nX: y\r\n\r\n"
	                "HTTP/1.1 201 Created\r\nContent-Length: 1\r\n\r\nz", 201, "z", 1);
	/* no body */
	test_http_split("HTTP/1.1 204 No Content\r\n\r\n", 204, "", 1);
	test_http_split("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", 304, "", 1);
	test_http_split("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, "", 1);
	/* HTTP/1.0 */
	test_http_split("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nab", 200, "ab", 0);
	test_http_split("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nab", 200, "ab", 1);
	/* redirect */
	feed("HTTP/1.1 302 Found\r\nLocation:   /r/speedtest-config.php  \r\nContent-Length: 0\r\n\r\n", 0, 0);
	CHECK(resp.state == ST_R_DONE && resp.status == 302 && !strcmp(resp.location, "/r/speedtest-config.php"));
	/* HEAD: no body although Content-Length says so */
	feed("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", 0, 1);
	CHECK(resp.state == ST_R_DONE && resp.body == 0);
	/* close-delimited body, then EOF */
	CHECK(feed("HTTP/1.1 200 OK\r\n\r\nuntil the end", 3, 0) == 32 && resp.state == ST_R_BODY_CLOSE);
	CHECK(st_resp_eof(&resp) == 0 && resp.state == ST_R_DONE && !strcmp(body, "until the end") &&
	      !st_resp_reusable(&resp));
	/* EOF too early */
	feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", 0, 0);
	CHECK(st_resp_eof(&resp) < 0 && resp.state == ST_R_ERROR);
	feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc", 0, 0);
	CHECK(st_resp_eof(&resp) < 0);
	feed("", 0, 0);
	CHECK(st_resp_eof(&resp) < 0 && strstr(resp.error, "without a response"));
	/* extra data after the response is not consumed */
	CHECK(feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nabEXTRA", 0, 0) == 40 && resp.state == ST_R_DONE);
	/* errors */
	expect_error("HTTP/2 200 OK\r\n\r\n", "version");
	expect_error("SSH-2.0-OpenSSH\r\n", "not http");
	expect_error("HTTP/1.1 2x0 OK\r\n\r\n", "status");
	expect_error("HTTP/1.1 20 OK\r\n\r\n", "short status");
	expect_error("HTTP/1.1 2000 OK\r\n\r\n", "long status");
	expect_error("HTTP/1.1 099 Low\r\n\r\n", "low status");
	expect_error("HTTP/1.1 200 OK\r\nno colon here\r\n\r\n", "header");
	expect_error("HTTP/1.1 200 OK\r\n: empty name\r\n\r\n", "header name");
	expect_error("HTTP/1.1 200 OK\r\nContent-Length: 12a\r\n\r\n", "length");
	expect_error("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", "negative length");
	expect_error("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", "huge length");
	expect_error("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", "two lengths");
	expect_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", "coding");
	expect_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", "chunk size");
	expect_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n", "empty chunk size");
	expect_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffffff\r\n", "chunk overflow");
	expect_error("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n0\r\n\r\n", "chunk CRLF");
	/* same Content-Length twice is fine */
	test_http_split("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nq", 200, "q", 1);
	/* long header lines are cut, a header over 64 KB fails */
	big = malloc(200000);
	n = (u32)snprintf(big, 200000, "HTTP/1.1 200 OK\r\nX-Long: ");
	memset(big + n, 'l', 3000);
	n += 3000;
	snprintf(big + n, 200000 - n, "\r\nContent-Length: 3\r\n\r\nend");
	test_http_split(big, 200, "end", 1);
	n = (u32)snprintf(big, 200000, "HTTP/1.1 200 OK\r\n");
	for (i = 0; i < 3000; i++)
		n += (u32)snprintf(big + n, 200000 - n, "X-Header-%u: some value here\r\n", (unsigned)i);
	snprintf(big + n, 200000 - n, "\r\n");
	expect_error(big, "header too large");
	/* a large chunked body in odd chunk sizes, every split */
	n = (u32)snprintf(big, 200000, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
	{
		u32 total = 0;
		for (i = 1; i < 120; i += 7) {
			u32 j;
			n += (u32)snprintf(big + n, 200000 - n, "%x\r\n", (unsigned)i);
			for (j = 0; j < i; j++)
				big[n++] = (char)('a' + (total + j) % 26);
			total += i;
			big[n++] = '\r';
			big[n++] = '\n';
		}
		snprintf(big + n, 200000 - n, "0\r\n\r\n");
		feed(big, 0, 0);
		CHECK(resp.state == ST_R_DONE && resp.body == total);
		for (i = 0; i < total; i++)
			if (body[i] != (char)('a' + i % 26))
				break;
		CHECK(i == total);
		for (i = 1; i < 64; i++) {
			feed(big, i, 0);
			CHECK(resp.state == ST_R_DONE && resp.body == total);
		}
	}
	/* random garbage after a valid status line never crashes */
	for (i = 0; i < 2000; i++) {
		u32 j, len = (u32)snprintf(big, 200000, "HTTP/1.1 200 OK\r\n");
		u32 extra = rnd() % 300;
		static const char alpha[] = "\r\n:0123456789abcdefTransfer-Encoding chunked Content-Length;";
		for (j = 0; j < extra; j++)
			big[len++] = alpha[rnd() % (sizeof(alpha) - 1)];
		big[len] = 0;
		feed(big, 1 + rnd() % 9, 0);
		CHECK(resp.state >= ST_R_STATUS && resp.state <= ST_R_ERROR);
	}
	free(big);
}

/* ------------------------------------------------------------ numbers */

static void test_numbers(void)
{
	s32 v;
	char b[64];
	u32 d;

	CHECK(st_parse_coord("-33.8678", &v) == 0 && v == -33867800);
	CHECK(st_parse_coord("151.2073", &v) == 0 && v == 151207300);
	CHECK(st_parse_coord("0", &v) == 0 && v == 0);
	CHECK(st_parse_coord("+1.0000001", &v) == 0 && v == 1000000);
	CHECK(st_parse_coord(".5", &v) == 0 && v == 500000);
	CHECK(st_parse_coord("", &v) < 0 && st_parse_coord("-", &v) < 0 && st_parse_coord("1.2.3", &v) < 0);
	CHECK(st_parse_coord("abc", &v) < 0 && st_parse_coord("4000", &v) < 0 && st_parse_coord(NULL, &v) < 0);

	CHECK(st_isqrt64(0) == 0 && st_isqrt64(1) == 1 && st_isqrt64(15) == 3 && st_isqrt64(16) == 4);
	CHECK(st_isqrt64(0xffffffffffffffffULL) == 0xffffffffULL);
	CHECK(st_isqrt64(1000000000000ULL) == 1000000);

	/* km * 100, against great-circle distances (R = 6371 km) */
#define NEAR(got, km100, pct) ((got) * 100 >= (km100) * (100 - (pct)) && (got) * 100 <= (km100) * (100 + (pct)))
	d = st_distance(-33868800, 151209300, -37813600, 144963100);   /* Sydney - Melbourne 713.4 km */
	CHECK(NEAR(d, 71340, 1));
	d = st_distance(-33868800, 151209300, -35280900, 149130000);   /* Sydney - Canberra 247 km */
	CHECK(NEAR(d, 24700, 1));
	d = st_distance(51507400, -127800, 48856600, 2352200);         /* London - Paris 343.6 km */
	CHECK(NEAR(d, 34360, 1));
	d = st_distance(59329300, 18068600, 60169900, 24938400);       /* Stockholm - Helsinki 396 km */
	CHECK(NEAR(d, 39600, 1));
	CHECK(st_distance(10000000, 20000000, 10000000, 20000000) == 0);
	d = st_distance(0, 179500000, 0, -179500000);                   /* across the date line: 111.19 km */
	CHECK(d >= 11110 && d <= 11130);
	d = st_distance(-90000000, 0, 90000000, 0);
	CHECK(d == 2001508);
	/* ranking: nearer servers sort first */
	{
		s32 pts[][2] = { { -33860000, 151210000 }, { -33360000, 151210000 }, { -33760000, 151210000 },
		                 { -37813600, 144963100 }, { 51507400, -127800 }, { -33860000, 151710000 } };
		u32 prev = 0, i;
		u32 order[] = { 0, 2, 5, 1, 3, 4 };
		for (i = 0; i < 6; i++) {
			u32 dd = st_distance(-33860000, 151210000, pts[order[i]][0], pts[order[i]][1]);
			CHECK(dd >= prev);
			prev = dd;
		}
	}

	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 1, 3, 2), "0.33"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 2, 3, 2), "0.67"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 999, 1000, 2), "1.00"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 12345, 1000, 3), "12.345"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 5, 0, 2), "0.00"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 7, 2, 0), "4"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 80000000000ULL * 8, 10000000, 2), "64000.00"));
	CHECK(!strcmp(st_fmt_ratio(b, sizeof(b), 5555, 100, 2), "55.55"));

	CHECK(!strcmp(st_fmt_time(b, sizeof(b), 0), "1970-01-01T00:00:00"));
	CHECK(!strcmp(st_fmt_time(b, sizeof(b), 951782400), "2000-02-29T00:00:00"));
	CHECK(!strcmp(st_fmt_time(b, sizeof(b), 1792577096), "2026-10-21T10:04:56"));
	CHECK(!strcmp(st_fmt_time(b, sizeof(b), 4102444799u), "2099-12-31T23:59:59"));
	CHECK(!strcmp(st_strerror(RT_ECONNREFUSED), "Connection refused"));
}

int main(void)
{
	test_xml();
	test_url();
	test_http();
	test_numbers();
	printf("t_speedtest_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
