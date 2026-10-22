/*
 * Riptide - ftp: interactive FTP client (RFC 959, RFC 2428 EPSV/EPRT) for
 * AmigaOS, in the style of the BSD/Linux ftp command.
 *
 *   ftp [-4|-6] [-n] [-v] [-p|-A] [-i] [-d] [-t secs] [host [port]]
 *   ftp [-o localfile] ftp://[user[:pass]@]host[:port]/path
 *   ftp -u ftp://[user[:pass]@]host[:port]/[path] localfile...
 *
 * IPv6 servers (-6, an IPv6 literal, "[addr]" in URLs) use EPSV in passive
 * and EPRT in active mode (RFC 2428; PASV/PORT cannot carry IPv6).
 *
 * Commands are read from standard input, so "ftp -n host <script" runs a
 * script. Passive mode (EPSV, then PASV) is the default; the address in a
 * PASV reply is ignored when it differs from the control connection's peer
 * (NAT). Ctrl-C aborts a transfer (data connection closed, ABOR sent, the
 * control connection recovered); at the prompt it asks to use "quit" and
 * quits when pressed twice. Uses only the standard bsdsocket.library API.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define BUFSZ    32768          /* transfer buffer (AllocVec) */
#define LINESZ   1024
#define MAXARGS  32
#define CTLBUF   1024

/* reply classes, as BSD ftp */
#define PRELIM   1
#define COMPLETE 2
#define CONTINUE 3
#define TRANSIENT 4
#define ERROR    5

/* where received data goes */
#define SINK_FILE   0
#define SINK_STDOUT 1
#define SINK_MEM    2

struct cmdtab {
	const char *name;
	void (*fn)(int argc, char **argv);
	int conn;                       /* needs a connection */
	const char *help;
};

/* connection */
static int ctl = -1;
static struct cl_addr peer, local;  /* control connection's addresses */
static int ftp_family;              /* -4 / -6 */
static char hostname[128];
static int loggedin;
static int epsv_ok = 1;             /* cleared when the server refuses EPSV */
static int data_listen = -1;        /* active mode: listening socket */
static int data_fd = -1;            /* passive mode: connected socket */

/* settings */
static int verbose, debug, passive = 1, use_epsv = 1, hashmark, prompting = 1;
static int interactive, autologin = 1, ascii_mode, show_replies, show_stats;
static u32 hashbytes = 1024, timeout_s = 60, restart_point, test_break;
static int stale_replies;     /* abort replies still expected from the server */
static void drain_stale(void);

/* replies */
static int code;                    /* code of the last reply */
static char reply_first[LINESZ];    /* first line of the last reply */
static char reply_last[LINESZ];     /* last line */
static char ctlbuf[CTLBUF];
static u32 ctlpos, ctllen;

/* input */
static char ibuf[512];
static LONG ipos, ilen;
static char line[LINESZ], argbuf[LINESZ], cmdbuf[LINESZ], mpat[LINESZ];
static char *margv[MAXARGS + 1];

static char *xbuf, *cbuf;           /* transfer and conversion buffers */
static char *mem;                   /* SINK_MEM buffer (NLST for mget) */
static u32 memlen, memsize;
static int aborted;                 /* last transfer was interrupted */
static int quit_now;
static int failed;                  /* last command failed (URL mode exit code) */
static BPTR orig_dir;
static int dir_changed;
static char errbuf[100];

struct tv64 { ULONG secs, micro; };

static void usage(void)
{
	cl_puts("usage: ftp [-46nvpAid] [-t secs] [host [port]]\n"
	        "       ftp [-o localfile] ftp://[user[:pass]@]host[:port]/path\n"
	        "       ftp -u ftp://[user[:pass]@]host[:port]/[path] localfile...\n");
}

static u64 now_us(void)
{
	return cl_now_us64();
}

static const char *doserr(LONG err)
{
	if (err == ERROR_DISK_FULL)
		return "No space left on device (disk full)";
	if (!Fault(err, NULL, (STRPTR)errbuf, sizeof(errbuf)) || !errbuf[0])
		rt_snprintf(errbuf, sizeof(errbuf), "DOS error %ld", (long)err);
	return errbuf;
}

static int brk_pending(void)
{
	return (CheckSignal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* ------------------------------------------------------------ input */

static int in_getc(void)
{
	if (ipos >= ilen) {
		cl_flush();
		ilen = Read(Input(), ibuf, sizeof(ibuf));
		ipos = 0;
		if (ilen <= 0) {
			ilen = 0;
			return -1;
		}
	}
	return (u8)ibuf[ipos++];
}

/* Wait for console input, watching Ctrl-C. 0 input, -2 Ctrl-C. */
static int in_wait(void)
{
	if (ipos < ilen || !interactive)
		return 0;
	for (;;) {
		if (WaitForChar(Input(), 200000))
			return 0;
		if (brk_pending())
			return -2;
	}
}

/* Read a line (without the newline). Returns its length, -1 at EOF,
 * -2 on Ctrl-C. noecho: console raw mode, characters are not shown. */
static int read_line(const char *prompt, char *buf, u32 size, int noecho)
{
	u32 n = 0;
	int c, raw = 0;

	if (prompt) {
		cl_puts(prompt);
		cl_flush();
	}
	if (noecho && interactive && ipos >= ilen && SetMode(Input(), 1))
		raw = 1;
	for (;;) {
		if (in_wait() < 0) {
			n = 0;
			c = -2;
			break;
		}
		c = in_getc();
		if (c < 0 || c == '\n' || (raw && c == '\r'))
			break;
		if (raw) {
			if (c == 3) {                   /* Ctrl-C as a character */
				c = -2;
				break;
			}
			if (c == 8 || c == 127) {
				if (n)
					n--;
				continue;
			}
			if (c == 0x9b || c == 27) {     /* skip CSI sequences */
				while ((c = in_getc()) >= 0 && !(c >= 0x40 && c <= 0x7e))
					;
				continue;
			}
		}
		if (c == '\r')
			continue;
		if (n + 1 < size)
			buf[n++] = (char)c;
	}
	if (raw) {
		SetMode(Input(), 0);
		cl_puts("\n");
	} else if (prompt && !interactive) {
		cl_puts("\n");                 /* the answer came from a script */
	}
	buf[n] = 0;
	if (c == -2) {
		if (!raw)
			cl_puts("\n");
		return -2;
	}
	if (c < 0 && n == 0)
		return -1;
	return (int)n;
}

/* Split 's' into margv (static); double quotes group. */
static int split(const char *s)
{
	char *w = argbuf;
	int argc = 0;

	rt_strlcpy(argbuf, s, sizeof(argbuf));
	for (;;) {
		char *o;
		while (*w == ' ' || *w == '\t')
			w++;
		if (!*w || argc >= MAXARGS)
			break;
		margv[argc++] = o = w;
		if (*w == '"') {
			w++;
			while (*w && *w != '"')
				*o++ = *w++;
			if (*w)
				w++;
		} else {
			while (*w && *w != ' ' && *w != '\t')
				*o++ = *w++;
		}
		if (*w)
			w++;
		*o = 0;
	}
	margv[argc] = NULL;
	return argc;
}

/* mget/mput confirmation: 1 yes, 0 no, -1 stop */
static int confirm(const char *cmd, const char *name)
{
	char b[16];
	int n;

	if (!prompting || !interactive)
		return 1;
	rt_snprintf(line, sizeof(line), "%s %s? ", cmd, name);
	n = read_line(line, b, sizeof(b), 0);
	if (n < 0 || b[0] == 'q' || b[0] == 'Q')
		return -1;
	if (b[0] == 'a' || b[0] == 'A') {
		prompting = 0;
		cl_printf("Prompting off for duration of %s.\n", cmd);
		return 1;
	}
	return !(b[0] == 'n' || b[0] == 'N');
}

/* ------------------------------------------------------------ control connection */

static void lostpeer(void)
{
	if (ctl >= 0)
		CloseSocket(ctl);
	if (data_fd >= 0)
		CloseSocket(data_fd);
	if (data_listen >= 0)
		CloseSocket(data_listen);
	ctl = data_fd = data_listen = -1;
	ctlpos = ctllen = 0;
	loggedin = 0;
}

/* Next byte of the control connection: >= 0 byte, -1 closed, -2 Ctrl-C,
 * -3 timeout, -4 error. */
static int ctl_getc(u32 ms)
{
	LONG n;
	int w;

	if (ctlpos < ctllen)
		return (u8)ctlbuf[ctlpos++];
	w = cl_wait_fd(ctl, 0, ms);
	if (w == 0)
		return -3;
	if (w == -1)
		return -2;
	if (w < 0)
		return -4;
	n = recv(ctl, ctlbuf, CTLBUF, 0);
	if (n == 0)
		return -1;
	if (n < 0)
		return Errno() == RT_EINTR ? -2 : -4;
	ctlpos = 1;
	ctllen = (u32)n;
	return (u8)ctlbuf[0];
}

/* One reply line; telnet commands are dropped. Length or ctl_getc error. */
static int ctl_line(char *buf, u32 size, u32 ms)
{
	u32 n = 0;
	int c;

	for (;;) {
		c = ctl_getc(ms);
		if (c < 0) {
			if (c == -1 && n)
				break;
			return c;
		}
		if (c == 255) {                 /* IAC */
			c = ctl_getc(ms);
			if (c >= 251 && c <= 254) {   /* WILL/WONT/DO/DONT option */
				ctl_getc(ms);
				continue;
			}
			if (c != 255)
				continue;
		}
		if (c == '\n')
			break;
		if (c == '\r' || c == 0)
			continue;
		if (n + 1 < size)
			buf[n++] = (char)c;
	}
	buf[n] = 0;
	return (int)n;
}

static int is_code(const char *s)
{
	return s[0] >= '1' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9';
}

/* Read one (possibly multi-line) reply within ms milliseconds. Returns the
 * code; 421 when the connection is lost (closed, timeout when 'fatal',
 * Ctrl-C), 0 on a non-fatal timeout. */
static int get_reply_t(u32 ms, int fatal)
{
	char *l = reply_last;
	int r, multi = 0, first = 1, c = 0, show;

	reply_first[0] = 0;
	for (;;) {
		r = ctl_line(l, LINESZ, ms);
		if (r < 0) {
			if ((r == -3 || r == -2) && !fatal)
				return 0;      /* non-fatal wait: timeout or Ctrl-C */
			cl_flush();
			if (r == -1)
				cl_puts("421 Service not available, remote server has closed connection.\n");
			else if (r == -2)
				cl_puts("Interrupted while waiting for the server; connection closed.\n");
			else if (r == -3)
				cl_puts("421 Control connection timed out; connection closed.\n");
			else
				cl_printf("ftp: control connection: %s\n", cl_strerror((int)Errno()));
			lostpeer();
			code = 421;
			reply_last[0] = 0;
			return 421;
		}
		if (first) {
			if (!is_code(l)) {
				/* stray text outside a reply: show it, keep reading */
				if (verbose)
					cl_printf("%s\n", l);
				continue;
			}
			c = (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0');
			multi = l[3] == '-';
			rt_strlcpy(reply_first, l, sizeof(reply_first));
			first = 0;
		} else if (is_code(l) && l[3] == ' ' && (l[0] - '0') * 100 + (l[1] - '0') * 10 + (l[2] - '0') == c) {
			multi = 0;                  /* "ddd text" ends a multi-line reply */
		}
		show = verbose || show_replies || c >= 400;
		if (show)
			cl_printf("%s\n", l);
		if (!multi)
			break;
	}
	cl_flush();
	code = c;
	return c;
}

static int get_reply(void)
{
	return get_reply_t(timeout_s * 1000UL, 1);
}

static int send_ctl(const char *s, u32 n)
{
	while (n) {
		LONG k = send(ctl, (APTR)s, (LONG)n, 0);
		if (k <= 0) {
			cl_printf("ftp: lost connection: %s\n", cl_strerror((int)Errno()));
			lostpeer();
			return -1;
		}
		s += k;
		n -= (u32)k;
	}
	return 0;
}

/* Send a command, read the reply; returns the reply class or -1 when not
 * connected / the connection was lost. */
static int command(const char *fmt, ...)
{
	va_list ap;
	u32 n;

	if (ctl < 0) {
		cl_puts("Not connected.\n");
		code = -1;
		return -1;
	}
	if (stale_replies)
		drain_stale();
	va_start(ap, fmt);
	rt_vsnprintf(cmdbuf, sizeof(cmdbuf) - 2, fmt, ap);
	va_end(ap);
	if (debug) {
		if (!rt_strncasecmp(cmdbuf, "PASS ", 5))
			cl_puts("---> PASS XXXX\n");
		else
			cl_printf("---> %s\n", cmdbuf);
		cl_flush();
	}
	n = (u32)strlen(cmdbuf);
	cmdbuf[n++] = '\r';
	cmdbuf[n++] = '\n';
	if (send_ctl(cmdbuf, n) < 0) {
		code = 421;
		return -1;
	}
	if (get_reply() == 421 && ctl < 0)
		return -1;
	return code / 100;
}

/* ------------------------------------------------------------ data connection */

static void close_data(void)
{
	if (data_fd >= 0)
		CloseSocket(data_fd);
	if (data_listen >= 0)
		CloseSocket(data_listen);
	data_fd = data_listen = -1;
}

/* "229 Entering Extended Passive Mode (|||port|)" */
static int parse_epsv(u16 *port)
{
	const char *p = strchr(reply_last, '(');
	unsigned long v;
	const char *end;
	char d;

	if (!p || !p[1])
		return -1;
	d = p[1];
	if (p[2] != d || p[3] != d)
		return -1;
	v = rt_strtoul(p + 4, &end, 10);
	if (end == p + 4 || *end != d || !v || v > 65535)
		return -1;
	*port = (u16)v;
	return 0;
}

/* "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)": the first run of six
 * comma separated numbers after the code. */
static int parse_pasv(u32 *addr, u16 *port)
{
	const char *p = reply_last + 3;

	for (; *p; p++) {
		unsigned long v[6];
		const char *q = p, *end;
		int i;

		if (*p < '0' || *p > '9')
			continue;
		for (i = 0; i < 6; i++) {
			if (*q < '0' || *q > '9')
				break;
			v[i] = rt_strtoul(q, &end, 10);
			if (v[i] > 255)
				break;
			q = end;
			if (i < 5) {
				if (*q != ',')
					break;
				q++;
			}
		}
		if (i == 6) {
			*addr = rt_htonl((u32)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3]));
			*port = (u16)((v[4] << 8) | v[5]);
			return 0;
		}
		while (p[1] >= '0' && p[1] <= '9')
			p++;
	}
	return -1;
}

/* Prepare the data connection before the transfer command: passive mode
 * connects now, active mode listens and sends PORT. 0 or -1. */
static int initconn(void)
{
	struct rt_sockaddr_in6 sa;
	struct cl_addr ad;
	LONG len;
	int r, s, v6 = peer.family == RT_AF_INET6;
	u16 port = 0;
	u32 addr = rt_in6_v4(&peer.a);
	char a[CL_ADDRSTRLEN], b[CL_ADDRSTRLEN];

	close_data();
	if (passive) {
		int got = 0;
		if ((use_epsv && epsv_ok) || v6) {
			r = command("EPSV");
			if (r < 0)
				return -1;
			if (r == COMPLETE && parse_epsv(&port) == 0) {
				got = 1;
			} else if (v6) {
				cl_puts("Extended passive mode refused (IPv6 needs EPSV). Try \"passive\" off.\n");
				return -1;
			} else if (r == COMPLETE || code == 500 || code == 501 || code == 502 || code == 504) {
				epsv_ok = 0;            /* not supported: PASV from now on */
				if (debug)
					cl_puts("EPSV refused, trying PASV.\n");
			} else {
				return -1;              /* e.g. 530 not logged in */
			}
		}
		if (!got) {
			r = command("PASV");
			if (r < 0)
				return -1;
			if (r != COMPLETE) {
				cl_puts("Passive mode refused. Turn off passive mode with \"passive\".\n");
				return -1;
			}
			if (parse_pasv(&addr, &port) < 0) {
				cl_puts("Passive mode address scan failure.\n");
				return -1;
			}
			/* Behind NAT (or with a misconfigured server) the advertised
			 * address is often private or wrong: use the control peer. */
			if (addr != rt_in6_v4(&peer.a)) {
				if (debug)
					cl_printf("Ignoring PASV address %s, using %s.\n", cl_ip(addr, a), cl_addr_str(&peer, b));
			}
		}
		/* the data connection always goes to the control peer */
		s = socket(peer.family, RT_SOCK_STREAM, 0);
		if (s < 0) {
			cl_printf("ftp: socket: %s\n", cl_strerror((int)Errno()));
			return -1;
		}
		r = cl_connect_addr(s, &peer, port, timeout_s * 1000UL);
		if (r) {
			cl_printf("ftp: connect to %s port %u: %s\n", cl_addr_str(&peer, a), (unsigned)port,
			          r == RT_EINTR ? "Interrupted" : cl_strerror(r));
			CloseSocket(s);
			if (r == RT_EINTR)
				aborted = 1;
			return -1;
		}
		data_fd = s;
		return 0;
	}
	/* active: listen on the control connection's local address */
	s = socket(local.family, RT_SOCK_STREAM, 0);
	if (s < 0) {
		cl_printf("ftp: socket: %s\n", cl_strerror((int)Errno()));
		return -1;
	}
	len = sizeof(sa);
	if (bind(s, &sa, (LONG)cl_sockaddr(&local, 0, &sa)) < 0 || listen(s, 1) < 0 ||
	    getsockname(s, &sa, &len) < 0) {
		cl_printf("ftp: data socket: %s\n", cl_strerror((int)Errno()));
		CloseSocket(s);
		return -1;
	}
	data_listen = s;
	cl_from_sockaddr(&sa, &ad, &port);
	if (v6) {
		/* RFC 2428 2: EPRT |2|address|port| (no zone) */
		rt_inet6_ntop(&local.a, a, RT_INET6_ADDRSTRLEN);
		r = command("EPRT |2|%s|%u|", a, (unsigned)port);
	} else {
		u32 h = rt_ntohl(rt_in6_v4(&local.a));
		r = command("PORT %lu,%lu,%lu,%lu,%u,%u", (unsigned long)(h >> 24), (unsigned long)((h >> 16) & 255),
		            (unsigned long)((h >> 8) & 255), (unsigned long)(h & 255), (unsigned)(port >> 8),
		            (unsigned)(port & 255));
	}
	if (r != COMPLETE) {
		close_data();
		return -1;
	}
	return 0;
}

/* After the 1xx reply: the connected data socket, or -1 (-2: Ctrl-C). */
static int dataconn(void)
{
	struct rt_sockaddr_in6 sa;
	LONG len = sizeof(sa);
	int s, w;

	if (data_fd >= 0) {
		s = data_fd;
		data_fd = -1;
		return s;
	}
	if (data_listen < 0)
		return -1;
	w = cl_wait_fd(data_listen, 0, timeout_s * 1000UL);
	if (w <= 0) {
		cl_puts(w == 0 ? "ftp: timed out waiting for the data connection\n" : "");
		close_data();
		return w == -1 ? -2 : -1;
	}
	s = accept(data_listen, &sa, &len);
	CloseSocket(data_listen);
	data_listen = -1;
	if (s < 0)
		cl_printf("ftp: accept: %s\n", cl_strerror((int)Errno()));
	return s;
}

/* After an interrupted transfer: ABOR, then read the transfer's final
 * reply and ABOR's (RFC 959: 426 + 226, or 226 + 225/226). */
static void abort_remote(void)
{
	if (ctl < 0)
		return;
	if (debug)
		cl_puts("---> ABOR\n");
	if (send_ctl("ABOR\r\n", 6) < 0)
		return;
	/* Servers answer an abort with two replies (426 + 226, or 226 +
	 * 225/226). Some take long or never send the second one, so neither
	 * a slow server nor another Ctrl-C closes the session: replies that
	 * come later are skipped before the next command. */
	stale_replies = 2;
	if (get_reply_t(15000, 0) == 0) {
		if (ctl >= 0)
			cl_puts("No reply to ABOR yet; continuing (late replies will be skipped).\n");
		return;
	}
	if (ctl < 0)
		return;
	stale_replies = 1;
	if (get_reply_t(5000, 0) != 0)
		stale_replies = 0;
}

/* Swallow replies left over from an abort before sending a new command. */
static void drain_stale(void)
{
	while (stale_replies > 0 && ctl >= 0) {
		if (get_reply_t(300, 0) == 0)
			break;             /* nothing (more) pending */
		stale_replies--;
	}
	stale_replies = 0;
}

static void print_stats(u64 bytes, u64 us, const char *what)
{
	u64 r10;

	if (!us)
		us = 1;
	r10 = bytes * 10000000ULL / (us * 1024ULL);       /* 0.1 KB/s */
	cl_printf("%llu bytes %s in %lu.%02lu secs (", (unsigned long long)bytes, what,
	          (unsigned long)(us / 1000000ULL), (unsigned long)((us / 10000ULL) % 100));
	if (r10 >= 10240) {
		r10 = bytes * 10000000ULL / (us * 1048576ULL);
		cl_printf("%lu.%lu MB/s)\n", (unsigned long)(r10 / 10), (unsigned long)(r10 % 10));
	} else {
		cl_printf("%lu.%lu KB/s)\n", (unsigned long)(r10 / 10), (unsigned long)(r10 % 10));
	}
}

static void hash_progress(u64 bytes, u64 *next, int *printed)
{
	if (!hashmark)
		return;
	while (bytes >= *next) {
		cl_puts("#");
		*next += hashbytes;
		*printed = 1;
	}
	cl_flush();
}

/* test hook ("testbreak N"): raise Ctrl-C once N bytes have moved */
static void test_hook(u64 bytes)
{
	if (test_break && bytes >= test_break) {
		test_break = 0;
		Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
	}
}

static int mem_add(const char *p, u32 n)
{
	if (memlen + n + 1 > memsize) {
		u32 ns = (memsize ? memsize * 2 : 4096) + n;
		char *nm = AllocVec(ns, MEMF_ANY);
		if (!nm)
			return -1;
		if (mem) {
			memcpy(nm, mem, memlen);
			FreeVec(mem);
		}
		mem = nm;
		memsize = ns;
	}
	memcpy(mem + memlen, p, n);
	memlen += n;
	mem[memlen] = 0;
	return 0;
}

/* RETR/LIST/NLST: receive into a local file, stdout or memory. ascii:
 * CRLF -> LF. Returns 0 on success. */
static int recvrequest(const char *cmd, const char *remote, const char *local, int sink, int ascii)
{
	BPTR fh = 0;
	int fd = -1, r, w, ok = 0, cr = 0, hashed = 0, brk = 0, werr = 0, listing = sink != SINK_FILE || strcmp(cmd, "RETR");
	u32 restart = listing ? 0 : restart_point;
	u64 bytes = 0, t0, t1, next = hashbytes;
	LONG n;

	aborted = 0;
	restart_point = 0;
	if (sink == SINK_FILE) {
		if (restart) {
			fh = Open((CONST_STRPTR)local, MODE_READWRITE);
			if (fh && Seek(fh, (LONG)restart, OFFSET_BEGINNING) < 0) {
				cl_printf("local: %s: cannot seek to %lu: %s\n", local, (unsigned long)restart, doserr(IoErr()));
				Close(fh);
				return -1;
			}
		} else {
			fh = Open((CONST_STRPTR)local, MODE_NEWFILE);
		}
		if (!fh) {
			cl_printf("local: %s: %s\n", local, doserr(IoErr()));
			return -1;
		}
	}
	if (sink == SINK_MEM)
		memlen = 0;
	if (initconn() < 0)
		goto out;
	if (restart && command("REST %lu", (unsigned long)restart) != CONTINUE)
		goto out;
	r = remote ? command("%s %s", cmd, remote) : command("%s", cmd);
	if (r != PRELIM) {
		if (r == COMPLETE && listing)   /* e.g. "226 no files" without data */
			ok = 1;
		goto out;
	}
	fd = dataconn();
	if (fd < 0) {
		if (fd == -2) {
			cl_puts("\nreceive aborted. waiting for remote to finish abort.\n");
			aborted = 1;
		}
		abort_remote();
		goto out;
	}
	if (sink == SINK_STDOUT)
		cl_flush();
	t0 = now_us();
	for (;;) {
		w = cl_wait_fd(fd, 0, timeout_s * 1000UL);
		if (w <= 0) {
			if (w == 0)
				cl_puts("\nftp: data connection timed out\n");
			brk = 1;
			break;
		}
		n = recv(fd, xbuf, BUFSZ, 0);
		if (n < 0) {
			if (Errno() != RT_EINTR)
				cl_printf("\nnetin: %s\n", cl_strerror((int)Errno()));
			brk = 1;
			break;
		}
		if (n == 0)
			break;
		bytes += (u64)n;
		{
			char *p = xbuf;
			LONG len = n;
			if (ascii) {
				char *o = cbuf;
				LONG i;
				if (cr && xbuf[0] != '\n')
					*o++ = '\r';
				cr = 0;
				for (i = 0; i < n; i++) {
					if (xbuf[i] == '\r') {
						if (i + 1 == n)
							cr = 1;
						else if (xbuf[i + 1] != '\n')
							*o++ = '\r';
						continue;
					}
					*o++ = xbuf[i];
				}
				p = cbuf;
				len = (LONG)(o - cbuf);
			}
			if (sink == SINK_FILE) {
				if (len && Write(fh, p, len) != len) {
					cl_printf("\nlocal: %s: %s\n", local, doserr(IoErr()));
					werr = brk = 1;
					break;
				}
			} else if (sink == SINK_STDOUT) {
				if (len)
					Write(Output(), p, len);
			} else if (mem_add(p, (u32)len) < 0) {
				cl_puts("\nftp: out of memory\n");
				werr = brk = 1;
				break;
			}
		}
		hash_progress(bytes, &next, &hashed);
		test_hook(bytes);
		/* WaitSelect() need not report a pending Ctrl-C while data keeps
		 * arriving: poll the signal too */
		if (brk_pending()) {
			brk = 1;
			break;
		}
	}
	t1 = now_us();
	if (cr && !brk) {
		if (sink == SINK_FILE)
			Write(fh, "\r", 1);
		else if (sink == SINK_STDOUT)
			Write(Output(), "\r", 1);
	}
	CloseSocket(fd);
	fd = -1;
	if (hashed)
		cl_puts("\n");
	if (brk) {
		if (!werr) {
			cl_puts("\nreceive aborted. waiting for remote to finish abort.\n");
			aborted = 1;
		}
		cl_flush();
		abort_remote();
		/* the Ctrl-C has been handled */
		CheckSignal(SIGBREAKF_CTRL_C);
	} else {
		r = get_reply();
		ok = r / 100 == COMPLETE;
	}
	if (bytes && (verbose || brk || (show_stats && !listing)) && (!listing || verbose))
		print_stats(bytes, t1 - t0, "received");
out:
	close_data();
	if (fd >= 0)
		CloseSocket(fd);
	if (fh)
		Close(fh);
	cl_flush();
	return ok ? 0 : -1;
}

/* send all of p, watching Ctrl-C; 0, -1 break/error */
static int send_data(int fd, const char *p, LONG n)
{
	while (n > 0) {
		LONG k;
		int w = cl_wait_fd(fd, 1, timeout_s * 1000UL);
		if (w <= 0) {
			if (w == 0)
				cl_puts("\nftp: data connection timed out\n");
			return -1;
		}
		k = send(fd, (APTR)p, n, 0);
		if (k <= 0) {
			if (Errno() != RT_EINTR)
				cl_printf("\nnetout: %s\n", cl_strerror((int)Errno()));
			return -1;
		}
		p += k;
		n -= k;
	}
	return 0;
}

/* STOR/APPE: send a local file. Returns 0 on success. */
static int sendrequest(const char *cmd, const char *local, const char *remote)
{
	BPTR fh;
	int fd = -1, r, ok = 0, hashed = 0, brk = 0, rerr = 0;
	u32 restart = restart_point;
	u64 bytes = 0, t0, t1, next = hashbytes;
	LONG n;

	aborted = 0;
	restart_point = 0;
	fh = Open((CONST_STRPTR)local, MODE_OLDFILE);
	if (!fh) {
		cl_printf("local: %s: %s\n", local, doserr(IoErr()));
		return -1;
	}
	if (restart && Seek(fh, (LONG)restart, OFFSET_BEGINNING) < 0) {
		cl_printf("local: %s: cannot seek to %lu\n", local, (unsigned long)restart);
		Close(fh);
		return -1;
	}
	if (initconn() < 0)
		goto out;
	if (restart && command("REST %lu", (unsigned long)restart) != CONTINUE)
		goto out;
	r = command("%s %s", cmd, remote);
	if (r != PRELIM)
		goto out;
	fd = dataconn();
	if (fd < 0) {
		if (fd == -2) {
			cl_puts("\nsend aborted. waiting for remote to finish abort.\n");
			aborted = 1;
		}
		abort_remote();
		goto out;
	}
	t0 = now_us();
	for (;;) {
		char *p = xbuf;
		n = Read(fh, xbuf, ascii_mode ? BUFSZ / 2 : BUFSZ);
		if (n < 0) {
			cl_printf("\nlocal: %s: %s\n", local, doserr(IoErr()));
			rerr = brk = 1;
			break;
		}
		if (n == 0)
			break;
		if (ascii_mode) {
			char *o = cbuf;
			LONG i;
			for (i = 0; i < n; i++) {
				if (xbuf[i] == '\n')
					*o++ = '\r';
				*o++ = xbuf[i];
			}
			p = cbuf;
			n = (LONG)(o - cbuf);
		}
		if (send_data(fd, p, n) < 0) {
			brk = 1;
			break;
		}
		bytes += (u64)n;
		hash_progress(bytes, &next, &hashed);
		test_hook(bytes);
		if (brk_pending()) {
			brk = 1;
			break;
		}
	}
	t1 = now_us();
	CloseSocket(fd);
	fd = -1;
	if (hashed)
		cl_puts("\n");
	if (brk) {
		if (!rerr) {
			cl_puts("\nsend aborted. waiting for remote to finish abort.\n");
			aborted = 1;
		}
		cl_flush();
		abort_remote();
		CheckSignal(SIGBREAKF_CTRL_C);
	} else {
		r = get_reply();
		ok = r / 100 == COMPLETE;
	}
	if (bytes && (verbose || brk || show_stats))
		print_stats(bytes, t1 - t0, "sent");
out:
	close_data();
	if (fd >= 0)
		CloseSocket(fd);
	Close(fh);
	cl_flush();
	return ok ? 0 : -1;
}

/* ------------------------------------------------------------ helpers */

static const char *fpart(const char *p)
{
	const char *b = p;

	for (; *p; p++)
		if (*p == '/' || *p == ':')
			b = p + 1;
	return *b ? b : NULL;
}

static int need_conn(void)
{
	if (ctl < 0) {
		cl_puts("Not connected.\n");
		return 0;
	}
	return 1;
}

static int onoff(int argc, char **argv, int cur)
{
	if (argc < 2)
		return !cur;
	if (!rt_strcasecmp(argv[1], "on") || !strcmp(argv[1], "1"))
		return 1;
	if (!rt_strcasecmp(argv[1], "off") || !strcmp(argv[1], "0"))
		return 0;
	return !cur;
}

static void settype(int a)
{
	if (command("TYPE %s", a ? "A" : "I") == COMPLETE)
		ascii_mode = a;
	else
		failed = 1;
}

/* ------------------------------------------------------------ connect / login */

static int login(const char *user, const char *pass, int quiet_defaults)
{
	char ubuf[64], pbuf[128], dflt[64];
	int r;

	if (!user) {
		if (GetVar((CONST_STRPTR)"USER", (STRPTR)dflt, sizeof(dflt), 0) <= 0 || !dflt[0])
			rt_strlcpy(dflt, "anonymous", sizeof(dflt));
		rt_snprintf(line, sizeof(line), "Name (%s:%s): ", hostname, dflt);
		r = read_line(line, ubuf, sizeof(ubuf), 0);
		if (r < 0)
			return -1;
		user = ubuf[0] ? ubuf : dflt;
	}
	r = command("USER %s", user);
	if (r == CONTINUE) {
		if (!pass) {
			if (read_line("Password:", pbuf, sizeof(pbuf), 1) < 0)
				return -1;
			pass = pbuf;
		}
		r = command("PASS %s", pass);
	}
	if (r == CONTINUE) {
		if (read_line("Account:", pbuf, sizeof(pbuf), 0) < 0)
			return -1;
		r = command("ACCT %s", pbuf);
	}
	memset(pbuf, 0, sizeof(pbuf));
	if (r != COMPLETE) {
		cl_puts("Login failed.\n");
		return -1;
	}
	loggedin = 1;
	if (command("SYST") == COMPLETE && verbose && !quiet_defaults) {
		char sys[32], *p = reply_last + 4;
		int i = 0;
		while (*p && *p != ' ' && i < (int)sizeof(sys) - 1)
			sys[i++] = *p++;
		sys[i] = 0;
		cl_printf("Remote system type is %s.\n", sys);
	}
	/* binary is the safe default for Amiga files */
	if (command("TYPE I") == COMPLETE) {
		ascii_mode = 0;
		if (verbose && !quiet_defaults)
			cl_puts("Using binary mode to transfer files.\n");
	}
	return 0;
}

static int ftp_open(const char *host, u32 port)
{
	struct rt_sockaddr_in6 sa;
	struct cl_addr addr;
	LONG len;
	int s, r, fam = ftp_family;
	char a[CL_ADDRSTRLEN], h[128];

	if (ctl >= 0) {
		cl_printf("Already connected to %s, use close first.\n", hostname);
		return -1;
	}
	/* "[2001:db8::1]" (URLs) */
	if (host[0] == '[') {
		rt_strlcpy(h, host + 1, sizeof(h));
		if (strchr(h, ']'))
			*strchr(h, ']') = 0;
		host = h;
	}
	if (!fam && cl_parse_addr(host, &addr) == 0)
		fam = addr.family == RT_AF_INET6 ? 6 : 4;
	if (cl_resolve_af(host, fam == 6 ? 6 : 4, &addr) < 0)
		return -1;
	s = socket(addr.family, RT_SOCK_STREAM, 0);
	if (s < 0) {
		cl_printf("ftp: socket: %s\n", cl_strerror((int)Errno()));
		return -1;
	}
	r = cl_connect_addr(s, &addr, (u16)port, timeout_s * 1000UL);
	if (r) {
		cl_printf("ftp: connect to %s port %lu: %s\n", cl_addr_str(&addr, a), (unsigned long)port,
		          r == RT_EINTR ? "Interrupted" : cl_strerror(r));
		CloseSocket(s);
		return -1;
	}
	{
		LONG one = 1;
		setsockopt(s, RT_SOL_SOCKET, RT_SO_KEEPALIVE, &one, sizeof(one));
	}
	ctl = s;
	ctlpos = ctllen = 0;
	peer = addr;
	len = sizeof(sa);
	if (getsockname(s, &sa, &len) == 0) {
		cl_from_sockaddr(&sa, &local, NULL);
	} else {
		memset(&local, 0, sizeof(local));
		local.family = addr.family;
	}
	rt_strlcpy(hostname, host, sizeof(hostname));
	epsv_ok = 1;
	if (verbose)
		cl_printf("Connected to %s (%s).\n", host, cl_addr_str(&addr, a));
	/* the greeting; 120 = "ready in nnn minutes" */
	do {
		r = get_reply();
	} while (ctl >= 0 && r / 100 == PRELIM);
	if (ctl < 0)
		return -1;
	if (r / 100 != COMPLETE) {
		lostpeer();
		return -1;
	}
	return 0;
}

static void disconnect(void)
{
	if (ctl < 0)
		return;
	command("QUIT");
	lostpeer();
}

/* ------------------------------------------------------------ commands */

static void c_open(int argc, char **argv)
{
	u32 port = 21;

	if (argc < 2) {
		char b[128];
		if (read_line("(to) ", b, sizeof(b), 0) <= 0)
			return;
		argc = split(b);
		if (argc < 1)
			return;
		/* split() reuses margv: shift */
		memmove(&margv[1], &margv[0], sizeof(char *) * MAXARGS);
		margv[0] = (char *)"open";
		argc++;
		argv = margv;
	}
	if (argc > 2 && cl_parse_uint(argv[2], 65535, &port) < 0) {
		cl_printf("%s: bad port number.\n", argv[2]);
		failed = 1;
		return;
	}
	if (ftp_open(argv[1], port) < 0) {
		failed = 1;
		return;
	}
	if (autologin && login(NULL, NULL, 0) < 0)
		failed = 1;
}

static void c_user(int argc, char **argv)
{
	if (argc < 2) {
		cl_puts("usage: user username [password]\n");
		failed = 1;
		return;
	}
	if (login(argv[1], argc > 2 ? argv[2] : NULL, 0) < 0)
		failed = 1;
}

static void c_close(int argc, char **argv)
{
	disconnect();
}

static void c_quit(int argc, char **argv)
{
	disconnect();
	quit_now = 1;
}

static void c_ls(int argc, char **argv)
{
	const char *cmd = !rt_strcasecmp(argv[0], "nlist") || !rt_strcasecmp(argv[0], "nls") ? "NLST" : "LIST";
	const char *local = argc > 2 ? argv[2] : NULL;

	if (local && !strcmp(local, "-"))
		local = NULL;
	if (recvrequest(cmd, argc > 1 ? argv[1] : NULL, local, local ? SINK_FILE : SINK_STDOUT, 1) < 0)
		failed = 1;
}

static void c_cd(int argc, char **argv)
{
	if (argc < 2) {
		cl_puts("usage: cd remote-directory\n");
		failed = 1;
		return;
	}
	if (command("CWD %s", argv[1]) != COMPLETE)
		failed = 1;
}

static void c_cdup(int argc, char **argv)
{
	if (command("CDUP") != COMPLETE)
		failed = 1;
}

static void c_pwd(int argc, char **argv)
{
	if (command("PWD") != COMPLETE) {
		failed = 1;
		return;
	}
	if (!verbose) {
		/* 257 "/dir" is current directory */
		const char *p = strchr(reply_first, '"'), *q;
		if (p && (q = strrchr(p + 1, '"')) != NULL)
			cl_printf("Remote directory: %.*s\n", (int)(q - p - 1), p + 1);
		else
			cl_printf("%s\n", reply_first);
	}
}

static void show_local_dir(void)
{
	BPTR l = CurrentDir(0);
	char b[256];

	CurrentDir(l);
	if (l && NameFromLock(l, (STRPTR)b, sizeof(b)))
		cl_printf("Local directory now %s\n", b);
	else
		cl_puts("Local directory now (unknown)\n");
}

static void c_lcd(int argc, char **argv)
{
	BPTR l, old;
	struct FileInfoBlock *fib;
	int isdir = 0;

	if (argc < 2) {
		show_local_dir();
		return;
	}
	l = Lock((CONST_STRPTR)argv[1], SHARED_LOCK);
	if (!l) {
		cl_printf("local: %s: %s\n", argv[1], doserr(IoErr()));
		failed = 1;
		return;
	}
	fib = AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
	if (fib && Examine(l, fib))
		isdir = fib->fib_DirEntryType > 0;
	if (fib)
		FreeVec(fib);
	if (!isdir) {
		cl_printf("local: %s: Not a directory\n", argv[1]);
		UnLock(l);
		failed = 1;
		return;
	}
	old = CurrentDir(l);
	if (!dir_changed) {
		orig_dir = old;              /* the CLI's lock: restored at exit */
		dir_changed = 1;
	} else {
		UnLock(old);
	}
	show_local_dir();
}

static void c_lpwd(int argc, char **argv)
{
	show_local_dir();
}

static void c_get(int argc, char **argv)
{
	const char *local;

	if (argc < 2) {
		cl_printf("usage: %s remote-file [local-file]\n", argv[0]);
		failed = 1;
		return;
	}
	local = argc > 2 ? argv[2] : fpart(argv[1]);
	if (!local) {
		cl_printf("%s: no local file name\n", argv[0]);
		failed = 1;
		return;
	}
	if (!rt_strcasecmp(argv[0], "reget")) {
		BPTR l = Lock((CONST_STRPTR)local, SHARED_LOCK);
		struct FileInfoBlock *fib = AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
		restart_point = 0;
		if (l && fib && Examine(l, fib) && fib->fib_DirEntryType < 0)
			restart_point = (u32)fib->fib_Size;
		if (fib)
			FreeVec(fib);
		if (l)
			UnLock(l);
		if (restart_point && verbose)
			cl_printf("Restarting at %lu.\n", (unsigned long)restart_point);
	}
	if (!strcmp(local, "-")) {
		if (recvrequest("RETR", argv[1], NULL, SINK_STDOUT, ascii_mode) < 0)
			failed = 1;
		return;
	}
	if (recvrequest("RETR", argv[1], local, SINK_FILE, ascii_mode) < 0)
		failed = 1;
}

static void c_put(int argc, char **argv)
{
	const char *remote;

	if (argc < 2) {
		cl_printf("usage: %s local-file [remote-file]\n", argv[0]);
		failed = 1;
		return;
	}
	remote = argc > 2 ? argv[2] : fpart(argv[1]);
	if (!remote) {
		cl_printf("%s: no remote file name\n", argv[0]);
		failed = 1;
		return;
	}
	if (sendrequest(!rt_strcasecmp(argv[0], "append") ? "APPE" : "STOR", argv[1], remote) < 0)
		failed = 1;
}

static void c_mget(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		cl_puts("usage: mget remote-files\n");
		failed = 1;
		return;
	}
	for (i = 1; i < argc && ctl >= 0; i++) {
		char *names, *p, *e;
		u32 n;
		int v = verbose, stop = 0;

		verbose = 0;
		if (recvrequest("NLST", argv[i], NULL, SINK_MEM, 1) < 0 || !memlen) {
			verbose = v;
			if (!aborted)
				cl_printf("mget: %s: no files\n", argv[i]);
			failed = 1;
			if (aborted)
				break;
			continue;
		}
		verbose = v;
		/* take the list: the transfers below reuse the memory buffer */
		names = mem;
		n = memlen;
		mem = NULL;
		memlen = memsize = 0;
		for (p = names; p < names + n && !stop; p = e + 1) {
			const char *local;
			int c;
			e = strchr(p, '\n');
			if (!e)
				e = names + n;
			*e = 0;
			if (!*p)
				continue;
			local = fpart(p);
			if (!local)
				continue;
			c = confirm("mget", p);
			if (c < 0)
				stop = 1;
			if (c <= 0)
				continue;
			if (recvrequest("RETR", p, local, SINK_FILE, ascii_mode) < 0)
				failed = 1;
			if (aborted || ctl < 0)
				stop = 1;
		}
		FreeVec(names);
		if (stop)
			break;
	}
}

static void c_mput(int argc, char **argv)
{
	struct AnchorPath *ap;
	int i, stop = 0;

	if (argc < 2) {
		cl_puts("usage: mput local-files\n");
		failed = 1;
		return;
	}
	ap = AllocVec(sizeof(*ap) + 512, MEMF_ANY | MEMF_CLEAR);
	if (!ap) {
		cl_puts("ftp: out of memory\n");
		return;
	}
	for (i = 1; i < argc && !stop && ctl >= 0; i++) {
		LONG err;
		int found = 0;
		memset(ap, 0, sizeof(*ap));
		ap->ap_BreakBits = SIGBREAKF_CTRL_C;
		ap->ap_Strlen = 512;
		/* '*' is a wildcard for MatchFirst() only with WildStar on: use #? */
		{
			const char *p = argv[i];
			u32 k = 0;
			for (; *p && k + 3 < sizeof(mpat); p++) {
				if (*p == '*' && (p == argv[i] || p[-1] != '\'')) {
					mpat[k++] = '#';
					mpat[k++] = '?';
				} else {
					mpat[k++] = *p;
				}
			}
			mpat[k] = 0;
		}
		for (err = MatchFirst((CONST_STRPTR)mpat, ap); !err; err = MatchNext(ap)) {
			int c;
			if (ap->ap_Info.fib_DirEntryType >= 0)
				continue;               /* directories are skipped */
			found = 1;
			c = confirm("mput", ap->ap_Buf);
			if (c < 0) {
				stop = 1;
				break;
			}
			if (!c)
				continue;
			if (sendrequest("STOR", ap->ap_Buf, ap->ap_Info.fib_FileName) < 0)
				failed = 1;
			if (aborted || ctl < 0) {
				stop = 1;
				break;
			}
		}
		MatchEnd(ap);
		if (err == ERROR_BREAK) {
			cl_puts("***Break\n");
			stop = 1;
		} else if (!found) {
			cl_printf("mput: %s: no match\n", argv[i]);
			failed = 1;
		}
	}
	FreeVec(ap);
}

/* simple one-argument commands */
static void simple(int argc, char **argv, const char *verb, const char *usage)
{
	if (argc < 2) {
		cl_printf("usage: %s %s\n", argv[0], usage);
		failed = 1;
		return;
	}
	if (command("%s %s", verb, argv[1]) != COMPLETE)
		failed = 1;
}

static void c_delete(int argc, char **argv) { simple(argc, argv, "DELE", "remote-file"); }
static void c_mkdir(int argc, char **argv) { simple(argc, argv, "MKD", "directory-name"); }
static void c_rmdir(int argc, char **argv) { simple(argc, argv, "RMD", "directory-name"); }

static void c_rename(int argc, char **argv)
{
	if (argc < 3) {
		cl_puts("usage: rename from-name to-name\n");
		failed = 1;
		return;
	}
	if (command("RNFR %s", argv[1]) != CONTINUE || command("RNTO %s", argv[2]) != COMPLETE)
		failed = 1;
}

static void c_size(int argc, char **argv)
{
	if (argc < 2) {
		cl_puts("usage: size remote-file\n");
		failed = 1;
		return;
	}
	if (command("SIZE %s", argv[1]) == COMPLETE) {
		if (!verbose)
			cl_printf("%s\t%s\n", argv[1], reply_last + 4);
	} else {
		failed = 1;
	}
}

static void c_modtime(int argc, char **argv)
{
	const char *t;

	if (argc < 2) {
		cl_puts("usage: modtime remote-file\n");
		failed = 1;
		return;
	}
	if (command("MDTM %s", argv[1]) != COMPLETE) {
		failed = 1;
		return;
	}
	t = reply_last + 4;
	if (strlen(t) >= 14)
		cl_printf("%s\t%.2s/%.2s/%.4s %.2s:%.2s:%.2s GMT\n", argv[1], t + 4, t + 6, t, t + 8, t + 10, t + 12);
	else
		cl_printf("%s\t%s\n", argv[1], t);
}

static void c_binary(int argc, char **argv)
{
	settype(0);
}

static void c_ascii(int argc, char **argv)
{
	settype(1);
}

static void c_type(int argc, char **argv)
{
	if (argc < 2) {
		cl_printf("Using %s mode to transfer files.\n", ascii_mode ? "ascii" : "binary");
		return;
	}
	if (!rt_strcasecmp(argv[1], "ascii") || !rt_strcasecmp(argv[1], "a"))
		settype(1);
	else if (!rt_strcasecmp(argv[1], "binary") || !rt_strcasecmp(argv[1], "image") || !rt_strcasecmp(argv[1], "i"))
		settype(0);
	else {
		cl_printf("%s: unknown mode.\n", argv[1]);
		failed = 1;
	}
}

static void c_passive(int argc, char **argv)
{
	passive = onoff(argc, argv, passive);
	cl_printf("Passive mode: %s.\n", passive ? "on" : "off");
}

static void c_epsv(int argc, char **argv)
{
	use_epsv = onoff(argc, argv, use_epsv);
	epsv_ok = 1;
	cl_printf("EPSV: %s.\n", use_epsv ? "on" : "off");
}

static void c_hash(int argc, char **argv)
{
	u32 n;

	if (argc > 1 && cl_parse_uint(argv[1], 0x7fffffff, &n) == 0 && n) {
		hashbytes = n;
		hashmark = 1;
	} else {
		hashmark = onoff(argc, argv, hashmark);
	}
	cl_printf("Hash mark printing %s", hashmark ? "on" : "off");
	if (hashmark)
		cl_printf(" (%lu bytes/hash mark)", (unsigned long)hashbytes);
	cl_puts(".\n");
}

static void c_verbose(int argc, char **argv)
{
	verbose = onoff(argc, argv, verbose);
	cl_printf("Verbose mode %s.\n", verbose ? "on" : "off");
}

static void c_debug(int argc, char **argv)
{
	debug = onoff(argc, argv, debug);
	cl_printf("Debugging %s (debug=%d).\n", debug ? "on" : "off", debug);
}

static void c_prompt(int argc, char **argv)
{
	prompting = onoff(argc, argv, prompting);
	cl_printf("Interactive mode %s.\n", prompting ? "on" : "off");
}

static void c_timeout(int argc, char **argv)
{
	u32 n;

	if (argc > 1) {
		if (cl_parse_uint(argv[1], 86400, &n) < 0 || !n) {
			cl_printf("%s: bad timeout value.\n", argv[1]);
			failed = 1;
			return;
		}
		timeout_s = n;
	}
	cl_printf("Timeout is %lu seconds.\n", (unsigned long)timeout_s);
}

static void c_restart(int argc, char **argv)
{
	u32 n;

	if (argc < 2 || cl_parse_uint(argv[1], 0x7fffffff, &n) < 0) {
		cl_puts("usage: restart byte-offset\n");
		failed = 1;
		return;
	}
	restart_point = n;
	cl_printf("Restarting at %lu. Execute get, put or append to initiate transfer\n", (unsigned long)n);
}

static void c_status(int argc, char **argv)
{
	char a[CL_ADDRSTRLEN];

	if (ctl >= 0)
		cl_printf("Connected to %s (%s).\n", hostname, cl_addr_str(&peer, a));
	else
		cl_puts("Not connected.\n");
	if (ctl >= 0)
		cl_printf("%s.\n", loggedin ? "Logged in" : "Not logged in");
	cl_printf("Mode: stream; Type: %s; Form: non-print; Structure: file\n", ascii_mode ? "ascii" : "binary");
	cl_printf("Passive mode: %s; EPSV: %s%s.\n", passive ? "on" : "off", use_epsv ? "on" : "off",
	          use_epsv && !epsv_ok ? " (refused by server, using PASV)" : "");
	cl_printf("Verbose: %s; Debugging: %s; Prompting: %s; Hash: %s", verbose ? "on" : "off",
	          debug ? "on" : "off", prompting ? "on" : "off", hashmark ? "on" : "off");
	if (hashmark)
		cl_printf(" (%lu bytes)", (unsigned long)hashbytes);
	cl_printf(".\nTimeout: %lu seconds.\n", (unsigned long)timeout_s);
	if (restart_point)
		cl_printf("Restart point: %lu.\n", (unsigned long)restart_point);
}

/* quote/literal/site: send as is, show the reply */
static void quote_line(int argc, char **argv, int first, const char *prefix)
{
	char b[LINESZ];
	u32 n = 0;
	int i, r;

	b[0] = 0;
	if (prefix)
		n = (u32)rt_strlcpy(b, prefix, sizeof(b));
	for (i = first; i < argc && n < sizeof(b) - 2; i++) {
		if (n)
			b[n++] = ' ';
		n += (u32)rt_strlcpy(b + n, argv[i], sizeof(b) - n);
		if (n >= sizeof(b))
			n = sizeof(b) - 1;
	}
	b[n] = 0;
	show_replies = 1;
	r = command("%s", b);
	while (r == PRELIM && ctl >= 0)
		r = get_reply() / 100;
	show_replies = 0;
	if (r != COMPLETE && r != CONTINUE)
		failed = 1;
}

static void c_quote(int argc, char **argv)
{
	if (argc < 2) {
		cl_puts("usage: quote line-to-send\n");
		failed = 1;
		return;
	}
	quote_line(argc, argv, 1, NULL);
}

static void c_site(int argc, char **argv)
{
	if (argc < 2) {
		cl_puts("usage: site arg1 [arg2] ...\n");
		failed = 1;
		return;
	}
	quote_line(argc, argv, 1, "SITE");
}

static void c_system(int argc, char **argv)
{
	show_replies = 1;
	if (command("SYST") != COMPLETE)
		failed = 1;
	show_replies = 0;
}

static void c_testbreak(int argc, char **argv)
{
	u32 n = 0;

	if (argc > 1)
		cl_parse_uint(argv[1], 0x7fffffff, &n);
	test_break = n;
}

static void c_help(int argc, char **argv);

static const struct cmdtab cmds[] = {
	{ "?", c_help, 0, "print local help information" },
	{ "append", c_put, 1, "append to a file" },
	{ "ascii", c_ascii, 1, "set ascii transfer type (CRLF <-> LF)" },
	{ "binary", c_binary, 1, "set binary transfer type" },
	{ "bye", c_quit, 0, "terminate ftp session and exit" },
	{ "cd", c_cd, 1, "change remote working directory" },
	{ "cdup", c_cdup, 1, "change remote working directory to parent directory" },
	{ "close", c_close, 1, "terminate ftp session" },
	{ "debug", c_debug, 0, "toggle/set debugging mode (show protocol)" },
	{ "delete", c_delete, 1, "delete remote file" },
	{ "dir", c_ls, 1, "list contents of remote directory" },
	{ "disconnect", c_close, 1, "terminate ftp session" },
	{ "epsv", c_epsv, 0, "toggle use of EPSV before PASV in passive mode" },
	{ "exit", c_quit, 0, "terminate ftp session and exit" },
	{ "get", c_get, 1, "receive file" },
	{ "hash", c_hash, 0, "toggle printing '#' for each buffer transferred [bytes]" },
	{ "help", c_help, 0, "print local help information" },
	{ "image", c_binary, 1, "set binary transfer type" },
	{ "lcd", c_lcd, 0, "change local working directory" },
	{ "literal", c_quote, 1, "send arbitrary ftp command" },
	{ "lpwd", c_lpwd, 0, "print local working directory" },
	{ "ls", c_ls, 1, "list contents of remote directory" },
	{ "mget", c_mget, 1, "get multiple files" },
	{ "mkdir", c_mkdir, 1, "make directory on the remote machine" },
	{ "modtime", c_modtime, 1, "show last modification time of remote file" },
	{ "mput", c_mput, 1, "send multiple files (* and ? patterns)" },
	{ "nlist", c_ls, 1, "nlist contents of remote directory" },
	{ "open", c_open, 0, "connect to remote ftp server" },
	{ "passive", c_passive, 0, "toggle passive transfer mode (default on)" },
	{ "prompt", c_prompt, 0, "toggle interactive prompting on multiple commands" },
	{ "put", c_put, 1, "send one file" },
	{ "pwd", c_pwd, 1, "print working directory on remote machine" },
	{ "quit", c_quit, 0, "terminate ftp session and exit" },
	{ "quote", c_quote, 1, "send arbitrary ftp command" },
	{ "recv", c_get, 1, "receive file" },
	{ "reget", c_get, 1, "get file restarting at end of local file" },
	{ "rename", c_rename, 1, "rename file" },
	{ "restart", c_restart, 0, "restart file transfer at bytecount" },
	{ "rmdir", c_rmdir, 1, "remove directory on the remote machine" },
	{ "send", c_put, 1, "send one file" },
	{ "site", c_site, 1, "send site specific command to remote server" },
	{ "size", c_size, 1, "show size of remote file" },
	{ "status", c_status, 0, "show current status" },
	{ "system", c_system, 1, "show remote system type" },
	{ "testbreak", c_testbreak, 0, NULL },      /* test hook: simulated Ctrl-C after N bytes */
	{ "timeout", c_timeout, 0, "set the network timeout in seconds" },
	{ "type", c_type, 1, "set file transfer type" },
	{ "user", c_user, 1, "send new user information" },
	{ "verbose", c_verbose, 0, "toggle verbose mode" },
	{ NULL, NULL, 0, NULL }
};

/* exact name, else a unique prefix; NULL (ambiguous: *amb set) */
static const struct cmdtab *lookup(const char *name, int *amb)
{
	const struct cmdtab *c, *found = NULL;
	int n = 0;

	*amb = 0;
	for (c = cmds; c->name; c++) {
		if (!rt_strcasecmp(c->name, name))
			return c;
		if (c->help && !rt_strncasecmp(c->name, name, strlen(name))) {
			found = c;
			n++;
		}
	}
	if (n > 1)
		*amb = 1;
	return n == 1 ? found : NULL;
}

static void c_help(int argc, char **argv)
{
	const struct cmdtab *c;
	int i, col = 0;

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			int amb;
			c = lookup(argv[i], &amb);
			if (!c || !c->help)
				cl_printf("?%s help command %s\n", amb ? "Ambiguous" : "Invalid", argv[i]);
			else
				cl_printf("%-15s %s\n", c->name, c->help);
		}
		return;
	}
	cl_puts("Commands may be abbreviated.  Commands are:\n\n");
	for (c = cmds; c->name; c++) {
		if (!c->help)
			continue;
		cl_printf("%-15s", c->name);
		if (++col == 5) {
			cl_puts("\n");
			col = 0;
		}
	}
	if (col)
		cl_puts("\n");
}

static void execute(const char *l)
{
	const struct cmdtab *c;
	int argc, amb;

	while (*l == ' ' || *l == '\t')
		l++;
	if (!*l || *l == '#')
		return;
	argc = split(l);
	if (!argc)
		return;
	failed = 0;
	c = lookup(margv[0], &amb);
	if (!c) {
		cl_printf("?%s command\n", amb ? "Ambiguous" : "Invalid");
		failed = 1;
		return;
	}
	if (c->conn && !need_conn()) {
		failed = 1;
		return;
	}
	c->fn(argc, margv);
	cl_flush();
}

static void cmdloop(void)
{
	int breaks = 0;

	while (!quit_now) {
		int n = read_line(interactive ? "ftp> " : NULL, line, sizeof(line), 0);
		if (n == -1)
			break;
		if (n == -2) {
			if (!interactive || ++breaks >= 2)
				break;
			cl_puts("Use \"quit\" to leave ftp (press Ctrl-C again to quit).\n");
			continue;
		}
		breaks = 0;
		if (!interactive && verbose && line[0] && line[0] != '#')
			cl_printf("ftp> %s\n", line);
		execute(line);
		if (!interactive && brk_pending())
			break;
	}
	disconnect();
}

/* ------------------------------------------------------------ URLs */

static void url_decode(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '%' && s[1] && s[2]) {
			char h[3];
			const char *end;
			h[0] = s[1];
			h[1] = s[2];
			h[2] = 0;
			{
				unsigned long v = rt_strtoul(h, &end, 16);
				if (!*end) {
					*o++ = (char)v;
					s += 2;
					continue;
				}
			}
		}
		*o++ = *s;
	}
	*o = 0;
}

/* ftp://[user[:pass]@]host[:port]/path, split in place */
static int parse_url(char *url, char **user, char **pass, char **host, u32 *port, char **path)
{
	char *p = url + 6, *at, *slash, *colon;

	*user = *pass = NULL;
	*port = 21;
	slash = strchr(p, '/');
	if (slash)
		*slash = 0;
	*path = slash ? slash + 1 : (char *)"";
	at = strrchr(p, '@');
	if (at) {
		*at = 0;
		*user = p;
		colon = strchr(p, ':');
		if (colon) {
			*colon = 0;
			*pass = colon + 1;
			url_decode(*pass);
		}
		url_decode(*user);
		p = at + 1;
	}
	colon = p[0] == '[' && strchr(p, ']') ? strchr(strchr(p, ']'), ':') : strchr(p, ':');
	if (colon) {
		*colon = 0;
		if (cl_parse_uint(colon + 1, 65535, port) < 0 || !*port)
			return -1;
	}
	*host = p;
	url_decode(*path);
	return **host ? 0 : -1;
}

static int url_mode(char *url, int upload, int nfiles, char **files, const char *outfile)
{
	char *user, *pass, *host, *path, rem[LINESZ];
	u32 port;
	int i, rc = CL_OK;

	if (parse_url(url, &user, &pass, &host, &port, &path) < 0) {
		cl_error("bad URL: %s", url);
		return CL_ERROR;
	}
	if (ftp_open(host, port) < 0)
		return CL_ERROR;
	if (login(user ? user : "anonymous", pass ? pass : (user ? NULL : "riptide@"), 1) < 0) {
		disconnect();
		return CL_ERROR;
	}
	if (upload) {
		for (i = 0; i < nfiles; i++) {
			u32 n = (u32)strlen(path);
			const char *b = fpart(files[i]);
			if (!n || path[n - 1] == '/')
				rt_snprintf(rem, sizeof(rem), "%s%s", path, b ? b : files[i]);
			else
				rt_strlcpy(rem, path, sizeof(rem));
			if (sendrequest("STOR", files[i], rem) < 0)
				rc = CL_ERROR;
		}
	} else if (!*path || path[strlen(path) - 1] == '/') {
		if (recvrequest("LIST", *path ? path : NULL, NULL, SINK_STDOUT, 1) < 0)
			rc = CL_ERROR;
	} else {
		const char *local = outfile ? outfile : fpart(path);
		if (!local)
			rc = CL_ERROR;
		else if (!strcmp(local, "-"))
			rc = recvrequest("RETR", path, NULL, SINK_STDOUT, 0) < 0 ? CL_ERROR : CL_OK;
		else
			rc = recvrequest("RETR", path, local, SINK_FILE, 0) < 0 ? CL_ERROR : CL_OK;
	}
	disconnect();
	return rc;
}

/* ------------------------------------------------------------ main */

static int ftp_main(int argc, char **argv)
{
	struct cl_getopt g;
	int c, rc = CL_OK, upload = 0, force_verbose = -1;
	const char *outfile = NULL;
	u32 port = 21, n;

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "nvpAidguo:t:P:h46", &g)) != -1) {
		switch (c) {
		case '4': ftp_family = 4; break;
		case '6': ftp_family = 6; break;
		case 'n': autologin = 0; break;
		case 'v': force_verbose = 1; break;
		case 'p': passive = 1; break;
		case 'A': passive = 0; break;
		case 'i': prompting = 0; break;
		case 'd': debug = 1; break;
		case 'g': break;
		case 'u': upload = 1; break;
		case 'o': outfile = g.arg; break;
		case 't':
			if (cl_parse_uint(g.arg, 86400, &n) < 0 || !n) {
				usage();
				return CL_ERROR;
			}
			timeout_s = n;
			break;
		case 'P':
			if (cl_parse_uint(g.arg, 65535, &port) < 0 || !port) {
				usage();
				return CL_ERROR;
			}
			break;
		case 'h': usage(); return CL_OK;
		default: usage(); return CL_ERROR;
		}
	}
	interactive = IsInteractive(Input()) ? 1 : 0;
	verbose = force_verbose > 0 ? 1 : interactive;
	if (cl_need_socket() < 0 || cl_need_timer() < 0)
		return CL_FAIL;
	xbuf = AllocVec(BUFSZ, MEMF_ANY);
	cbuf = AllocVec(BUFSZ + 2, MEMF_ANY);
	if (!xbuf || !cbuf) {
		cl_error("out of memory");
		rc = CL_FAIL;
		goto out;
	}
	if (g.ind < argc && !rt_strncasecmp(argv[g.ind], "ftp://", 6)) {
		if (upload && g.ind + 1 >= argc) {
			usage();
			rc = CL_ERROR;
			goto out;
		}
		/* like BSD ftp's auto-fetch: quiet, but with the transfer statistics */
		verbose = force_verbose > 0;
		show_stats = 1;
		rc = url_mode(argv[g.ind], upload, argc - g.ind - 1, argv + g.ind + 1, outfile);
		goto out;
	}
	if (upload) {
		usage();
		rc = CL_ERROR;
		goto out;
	}
	if (g.ind < argc) {
		if (g.ind + 1 < argc && (cl_parse_uint(argv[g.ind + 1], 65535, &port) < 0 || !port)) {
			cl_printf("%s: bad port number.\n", argv[g.ind + 1]);
			rc = CL_ERROR;
			goto out;
		}
		if (ftp_open(argv[g.ind], port) == 0 && autologin)
			login(NULL, NULL, 0);
	}
	cmdloop();
out:
	lostpeer();
	if (dir_changed)
		UnLock(CurrentDir(orig_dir));
	if (xbuf)
		FreeVec(xbuf);
	if (cbuf)
		FreeVec(cbuf);
	if (mem)
		FreeVec(mem);
	return rc;
}

CL_MAIN(ftp_main, "ftp")
