/*
 * Riptide - speedtest end-to-end scenarios against tools/speedtest_mock.py,
 * shared by t_speedtest_e2e.c (Linux sockets) and t_speedtest_tun.c
 * (Riptide's host stack over TUN). The test supplies 'test_sys' (the shim
 * under test) and the address the mock listens on.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <regex.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "speedtest.h"
#include "st_posix.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); dump_last(); } } while (0)

/* ------------------------------------------------------------ captured output */

static char out_buf[1 << 20], err_buf[1 << 16];
static u32 out_len, err_len;
static const char *last_cmd = "";

static void dump_last(void)
{
	fprintf(stderr, "  command: speedtest %s\n  stdout:\n%.3000s\n  stderr:\n%.2000s\n", last_cmd, out_buf, err_buf);
}

static void cap_out(void *ctx, int e, const char *s, u32 n)
{
	char *b = e ? err_buf : out_buf;
	u32 *len = e ? &err_len : &out_len, cap = e ? sizeof(err_buf) : sizeof(out_buf);

	(void)ctx;
	if (*len + n >= cap)
		n = cap - 1 - *len;
	memcpy(b + *len, s, n);
	*len += n;
	b[*len] = 0;
}

static void cap_flush(void *ctx)
{
	(void)ctx;
}

static struct st_sys test_sys;          /* filled in by the test */
static u32 run_ms;                      /* how long the last run took */

static u32 ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* run "speedtest ARGS" (ARGS split at spaces); returns the exit code */
static int run_st(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run_st(const char *fmt, ...)
{
	static char line[1024], keep[1024];
	char *argv[64], *p;
	int argc = 1, rc;
	u32 t0;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(keep, sizeof(keep), "%s", line);
	last_cmd = keep;
	argv[0] = "speedtest";
	for (p = strtok(line, " "); p && argc < 63; p = strtok(NULL, " "))
		argv[argc++] = p;
	argv[argc] = NULL;
	out_len = err_len = 0;
	out_buf[0] = err_buf[0] = 0;
	t0 = ms_now();
	rc = st_main(argc, argv, &test_sys);
	run_ms = ms_now() - t0;
	printf("speedtest %s -> %d (%u ms)\n", keep, rc, (unsigned)run_ms);
	fflush(stdout);
	return rc;
}

static int has(const char *buf, const char *s)
{
	return strstr(buf, s) != NULL;
}

static int re(const char *buf, const char *pattern)
{
	regex_t r;
	int ok;

	if (regcomp(&r, pattern, REG_EXTENDED | REG_NEWLINE | REG_NOSUB) != 0) {
		fprintf(stderr, "bad regex %s\n", pattern);
		return 0;
	}
	ok = regexec(&r, buf, 0, NULL, 0) == 0;
	regfree(&r);
	if (!ok)
		fprintf(stderr, "  no match for /%s/\n", pattern);
	return ok;
}

/* a number after "key": in JSON */
static double jnum(const char *buf, const char *key)
{
	char k[64];
	const char *p;

	snprintf(k, sizeof(k), "\"%s\": ", key);
	p = strstr(buf, k);
	return p ? strtod(p + strlen(k), NULL) : -1;
}

/* ------------------------------------------------------------ the mock */

static pid_t mock_pid;
static int mock_port;
static char mock_ip[32];
static const char *mock_script;

static const char *find_mock(const char *argv0)
{
	static char path[1024];
	const char *env = getenv("SPEEDTEST_MOCK");
	const char *slash;

	if (env)
		return env;
	if (access("tools/speedtest_mock.py", R_OK) == 0)
		return "tools/speedtest_mock.py";
	/* build/<dir>/tests/t_... -> tools/ */
	slash = strrchr(argv0, '/');
	if (slash) {
		snprintf(path, sizeof(path), "%.*s/../../../tools/speedtest_mock.py", (int)(slash - argv0), argv0);
		if (access(path, R_OK) == 0)
			return path;
	}
	return NULL;
}

static void mock_stop(void)
{
	if (mock_pid > 0) {
		kill(mock_pid, SIGTERM);
		waitpid(mock_pid, NULL, 0);
	}
	mock_pid = 0;
}

static int mock_start(const char *ip, const char *opts)
{
	int p[2], n = 0;
	char line[256], cmd[1024];
	u32 t0 = ms_now();

	mock_stop();
	snprintf(mock_ip, sizeof(mock_ip), "%s", ip);
	snprintf(cmd, sizeof(cmd), "exec python3 %s --bind %s --port 0 %s", mock_script, ip, opts);
	if (pipe(p) < 0)
		return -1;
	mock_pid = fork();
	if (mock_pid == 0) {
		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	while (n < (int)sizeof(line) - 1 && ms_now() - t0 < 15000) {
		struct pollfd pf = { p[0], POLLIN, 0 };
		if (poll(&pf, 1, 500) == 1) {
			ssize_t k = read(p[0], line + n, 1);
			if (k <= 0)
				break;
			if (line[n++] == '\n')
				break;
		}
	}
	line[n] = 0;
	close(p[0]);
	{
		const char *c = strrchr(line, ':');
		mock_port = c ? atoi(c + 1) : 0;
	}
	if (mock_port <= 0) {
		fprintf(stderr, "mock did not start (%s): %s\n", cmd, line);
		mock_stop();
		return -1;
	}
	printf("mock: %s (%s)\n", opts, line);
	return 0;
}

/* GET path from the mock with Linux sockets into buf */
static int mock_get(const char *path, char *buf, u32 size)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	u32 n = 0;
	char req[256];

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((u16)mock_port);
	inet_pton(AF_INET, mock_ip, &a.sin_addr);
	buf[0] = 0;
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", path);
	if (write(fd, req, strlen(req)) < 0) {
		close(fd);
		return -1;
	}
	for (;;) {
		ssize_t k = read(fd, buf + n, size - 1 - n);
		if (k <= 0)
			break;
		n += (u32)k;
	}
	buf[n] = 0;
	close(fd);
	return 0;
}

static double mock_stat(const char *key)
{
	static char b[4096];
	mock_get("/__stats", b, sizeof(b));
	return jnum(b, key);
}

static void mock_reset(void)
{
	char b[512];
	mock_get("/__reset", b, sizeof(b));
}

/* ------------------------------------------------------------ scenarios */

static const u32 dl_sizes[] = { 350, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500, 4000 };
#define DIV 50
#define COUNT 3
#define UL_ARGS "--upload-sizes 64,256 --count 3"
#define UL_BYTES (3ULL * (64 + 256) * 1024)

static u64 dl_expected(void)
{
	u64 t = 0;
	u32 i;
	for (i = 0; i < 10; i++)
		t += (u64)(dl_sizes[i] * dl_sizes[i] * 2 / DIV) * COUNT;
	return t;
}

static char mirror[64], surl[128];

static void set_urls(void)
{
	snprintf(mirror, sizeof(mirror), "http://%s:%d", mock_ip, mock_port);
	snprintf(surl, sizeof(surl), "http://%s:%d/speedtest/upload.php", mock_ip, mock_port);
}

/* exact byte counts in JSON output, against the mock's counters */
static void check_json_totals(const char *what)
{
	double dl = jnum(out_buf, "bytes_received"), ul = jnum(out_buf, "bytes_sent");
	double rdl = jnum(out_buf, "download"), rul = jnum(out_buf, "upload");

	printf("  %s: %.0f bytes down (%.2f Mbit/s), %.0f up (%.2f Mbit/s), ping %.3f ms\n", what, dl, rdl / 1e6, ul,
	       rul / 1e6, jnum(out_buf, "ping"));
	CHECK(dl == (double)dl_expected());
	CHECK(ul == (double)UL_BYTES);
	CHECK(mock_stat("download_bytes") == (double)dl_expected());
	CHECK(mock_stat("download_complete") == 10 * COUNT);
	CHECK(mock_stat("upload_bytes") == (double)UL_BYTES);
	CHECK(mock_stat("upload_complete") == 2 * COUNT);
	CHECK(mock_stat("upload_badprefix") <= 0);
	/* plausible rates: more than 0.1 Mbit/s, less than 1 Tbit/s */
	CHECK(rdl > 1e5 && rdl < 1e12);
	CHECK(rul > 1e5 && rul < 1e12);
}

static void scenarios(const char *ip, int full)
{
	int rc;
	char o[256];

	/* --- discovery, selection, both tests ------------------------------ */
	snprintf(o, sizeof(o), "--size-div %d --servers 5", DIV);
	CHECK(mock_start(ip, o) == 0);
	set_urls();
	rc = run_st("--mirror %s --duration 60 " UL_ARGS, mirror);
	CHECK(rc == 0);
	CHECK(re(out_buf, "^Retrieving speedtest.net configuration\\.\\.\\.$"));
	CHECK(re(out_buf, "^Testing from Mock ISP & Co \\([0-9.]+\\)\\.\\.\\.$"));
	CHECK(re(out_buf, "^Retrieving speedtest.net server list\\.\\.\\.$"));
	CHECK(re(out_buf, "^Selecting best server based on ping\\.\\.\\.$"));
	CHECK(re(out_buf, "^Hosted by Riptide & Friends \\(Mock City\\) \\[55\\.[0-9]{2} km\\]: [0-9]+\\.[0-9]{3} ms$"));
	CHECK(re(out_buf, "^Testing download speed\\.{30}$"));
	CHECK(re(out_buf, "^Download: [0-9]+\\.[0-9]{2} Mbit/s$"));
	CHECK(re(out_buf, "^Testing upload speed\\.{6}$"));
	CHECK(re(out_buf, "^Upload: [0-9]+\\.[0-9]{2} Mbit/s$"));
	CHECK(err_len == 0);
	mock_reset();
	rc = run_st("--mirror %s --json --duration 60 " UL_ARGS, mirror);
	CHECK(rc == 0);
	CHECK(re(out_buf, "^\\{\"download\": [0-9]+\\.[0-9]{3}, \"upload\": [0-9]+\\.[0-9]{3}, \"ping\": [0-9]+\\.[0-9]{3}, "
	                  "\"server\": \\{\"url\": \"http://[0-9.:]+/speedtest/upload.php\", \"lat\": \"-33.3600\", "
	                  "\"lon\": \"151.2100\", \"name\": \"Mock City\", \"country\": \"Mockland\", \"cc\": \"MK\", "
	                  "\"sponsor\": \"Riptide & Friends\", \"id\": \"1001\", \"host\": \"[0-9.:]+\", "
	                  "\"d\": 55\\.[0-9]{2}, \"latency\": [0-9.]+\\}, \"timestamp\": \"20[0-9]{2}-[0-9]{2}-[0-9]{2}T"
	                  "[0-9]{2}:[0-9]{2}:[0-9]{2}\\.000000Z\", \"bytes_sent\": [0-9]+, \"bytes_received\": [0-9]+, "
	                  "\"share\": null, \"client\": \\{\"ip\": \"[0-9.]+\", \"lat\": \"-33.8600\", "
	                  "\"lon\": \"151.2100\", \"isp\": \"Mock ISP & Co\"\\}\\}$"));
	check_json_totals("keep-alive");
	CHECK(mock_stat("config") == 1 && mock_stat("servers") == 1);
	/* 3 latency requests to 1002 and 1001; 1003 refuses, 1004 is ignored */
	CHECK(mock_stat("latency") == 6);
	/* keep-alive: far fewer connections than requests */
	CHECK(mock_stat("connections") < mock_stat("requests"));

	rc = run_st("--server-url %s --simple --duration 30 " UL_ARGS, surl);
	CHECK(rc == 0);
	CHECK(re(out_buf, "^Ping: [0-9]+\\.[0-9]{3} ms\nDownload: [0-9]+\\.[0-9]{2} Mbit/s\nUpload: [0-9]+\\.[0-9]{2} Mbit/s\n$"));
	rc = run_st("--server-url %s --simple --bytes --no-upload --duration 30 " UL_ARGS, surl);
	CHECK(rc == 0 && re(out_buf, "^Download: [0-9]+\\.[0-9]{2} Mbyte/s\nUpload: 0\\.00 Mbyte/s$"));
	rc = run_st("--server-url %s --no-download " UL_ARGS, surl);
	CHECK(rc == 0 && re(out_buf, "^Testing against http://.*/speedtest/upload.php\\.\\.\\.$") &&
	      re(out_buf, "^Hosted by [0-9.]+:[0-9]+ \\(custom server\\): [0-9.]+ ms$") &&
	      re(out_buf, "^Skipping download test$") && re(out_buf, "^Upload: "));
	rc = run_st("--mirror %s --csv --csv-delimiter ; --no-upload " UL_ARGS, mirror);
	CHECK(rc == 0 && re(out_buf, "^1001;Riptide & Friends;Mock City;20[0-9-]+T[0-9:]+\\.000000Z;55\\.[0-9]{2};"
	                             "[0-9.]+;[0-9]+\\.[0-9]{3};0\\.000;;[0-9.]+\n$"));

	if (full) {
		/* server choice */
		rc = run_st("--mirror %s --simple --server 1002 --no-download --no-upload", mirror);
		CHECK(rc == 0 && re(out_buf, "^Ping: (2[0-9]{2}|[3-9][0-9]{2})\\.[0-9]{3} ms$"));
		rc = run_st("--mirror %s --exclude 1001 --no-download --no-upload", mirror);
		CHECK(rc == 0 && re(out_buf, "^Hosted by Slow Twin \\(Slow Town\\) \\[11\\.[0-9]{2} km\\]: "));
		rc = run_st("--mirror %s --server 424242", mirror);
		CHECK(rc == 10 && has(err_buf, "No matched servers: 424242"));
		rc = run_st("--mirror %s --server 1003 --no-download", mirror);
		CHECK(rc == 10 && has(err_buf, "Unable to connect to servers to test latency"));
		rc = run_st("--server-url http://%s:1/speedtest/upload.php", ip);
		CHECK(rc == 10 && has(err_buf, "Connection refused"));
		rc = run_st("--server-url https://%s/speedtest/upload.php", ip);
		CHECK(rc == 10 && has(err_buf, "HTTPS is not supported"));
		rc = run_st("--mirror http://%s:1", ip);
		CHECK(rc == 10 && has(err_buf, "Cannot retrieve speedtest configuration"));
		rc = run_st("--secure");
		CHECK(rc == 10 && has(err_buf, "no TLS"));
		rc = run_st("--threads 99");
		CHECK(rc == 10);
		rc = run_st("--csv-header");
		CHECK(rc == 0 && !strcmp(out_buf, "Server ID,Sponsor,Server Name,Timestamp,Distance,Ping,Download,Upload,"
		                                  "Share,IP Address\n"));
		rc = run_st("--version");
		CHECK(rc == 0 && has(out_buf, "speedtest-cli") && has(out_buf, "Apache-2.0"));
		rc = run_st("--help");
		CHECK(rc == 0 && has(out_buf, "--server-url URL"));
	}

	/* --- chunked documents and images, behind redirects --------------- */
	snprintf(o, sizeof(o), "--size-div %d --chunked --redirect", DIV);
	CHECK(mock_start(ip, o) == 0);
	set_urls();
	rc = run_st("--mirror %s --json --duration 60 --debug " UL_ARGS, mirror);
	CHECK(rc == 0);
	check_json_totals("chunked + redirects");
	CHECK(mock_stat("redirects") == 2);
	CHECK(has(err_buf, "debug: redirected to /r/speedtest-config.php"));

	/* --- the discovery endpoints went HTTPS --------------------------- */
	CHECK(mock_start(ip, "--https-redirect") == 0);
	set_urls();
	rc = run_st("--mirror %s", mirror);
	CHECK(rc == 10 && has(err_buf, "redirects to https://www.speedtest.net/speedtest-config.php") &&
	      has(err_buf, "now requires HTTPS") && has(err_buf, "--server-url http://HOST:PORT/speedtest/upload.php"));

	if (full) {
		/* --- no keep-alive; a huge server list --------------------------- */
		snprintf(o, sizeof(o), "--size-div %d --close --servers 5000", DIV);
		CHECK(mock_start(ip, o) == 0);
		set_urls();
		rc = run_st("--mirror %s --json --duration 60 " UL_ARGS, mirror);
		CHECK(rc == 0);
		check_json_totals("Connection: close, 5000 servers");
		{
			double c = mock_stat("connections"), r = mock_stat("requests");
			printf("  %.0f connections, %.0f requests\n", c, r);
			CHECK(c >= r - 1);
		}
		rc = run_st("--mirror %s --list", mirror);
		CHECK(rc == 0 && re(out_buf, "^ 1003\\) Refused Inc \\(Nowhere, Mockland\\) \\[5\\.[0-9]{2} km\\]\n 1002\\) ") &&
		      !has(out_buf, " 1004)") && re(out_buf, "^\\(4903 more servers not shown\\)$"));
		{
			int lines = 0;
			const char *p;
			for (p = out_buf; *p; p++)
				lines += *p == '\n';
			CHECK(lines == 101);
		}
		rc = run_st("--mirror %s --list --exclude 1003 --exclude 1002", mirror);
		CHECK(rc == 0 && re(out_buf, "^ 1001\\) Riptide & Friends"));
	}

	/* --- time limit and Ctrl-C on slow connections ------------------- */
	CHECK(mock_start(ip, "--rate 100") == 0);
	set_urls();
	rc = run_st("--server-url %s --json --duration 3 --threads 2 --no-upload", surl);
	CHECK(rc == 0);
	{
		double b = jnum(out_buf, "bytes_received"), r = jnum(out_buf, "download");
		printf("  rate limited: %.0f bytes, %.3f Mbit/s (2 x 100 KB/s = 1.64 Mbit/s)\n", b, r / 1e6);
		CHECK(b > 200000 && b < 1200000);
		CHECK(r > 0.8e6 && r < 2.5e6);
		CHECK(run_ms < 3000 + 2500);
	}
	rc = run_st("--server-url %s --duration 30 --test-break 1500", surl);
	CHECK(rc == 5 && has(err_buf, "Cancelling...") && run_ms < 6000);
	CHECK(has(out_buf, "Testing download speed") && !has(out_buf, "Download:"));

	/* --- an unresponsive server --------------------------------------- */
	CHECK(mock_start(ip, "--delay 3000") == 0);
	set_urls();
	rc = run_st("--server-url %s --timeout 1", surl);
	CHECK(rc == 10 && has(err_buf, "Connection timed out") && run_ms < 6000);
	mock_stop();
}
