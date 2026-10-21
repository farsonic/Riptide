/*
 * Riptide - ftpd-test: a minimal FTP server for testing the ftp client
 * (tests/amiga/run_fsuae_ftp.sh). NOT a production server: one session at
 * a time, any user name and password are accepted (except "baduser"), no
 * access control beyond staying below the root directory.
 *
 *   ftpd-test [-6] [-p port] [-1] [-v] ROOT
 *
 * -6 listens on [::] (dual-stack: IPv4 clients too); IPv6 sessions use
 * EPSV and EPRT (RFC 2428).
 * ROOT is an AmigaDOS directory (RAM:ftp, DH0:ftproot). Commands: USER
 * PASS SYST TYPE PWD CWD CDUP PASV EPSV PORT EPRT LIST NLST RETR STOR APPE SIZE
 * MDTM DELE MKD RMD RNFR RNTO REST NOOP FEAT HELP ABOR QUIT, and for tests
 * "SITE NOEPSV" (EPSV is refused from now on) and "SITE PASVIP a.b.c.d"
 * (PASV advertises that address). -1 exits after one session; Ctrl-C stops.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define BUFSZ 32768
#define PATHSZ 512

static int ctl = -1, pasv_fd = -1, verbose;
static u32 pasv_ip;
static struct cl_addr port_a, local;   /* PORT/EPRT target, control's local address */
static u16 port_port;
static int ascii, noepsv, loggedin, last_aborted;
static u32 rest;
static const char *root;
static char cwd[PATHSZ], rnfr[PATHSZ];
static char line[1024], out[1024];
static char ctlbuf[1024];
static u32 ctlpos, ctllen;
static char *buf, *cbuf;
static struct FileInfoBlock *fib;

static void dlog(const char *fmt, ...) RT_PRINTF(1, 2);
static void dlog(const char *fmt, ...)
{
	va_list ap;
	char b[256];

	if (!verbose)
		return;
	va_start(ap, fmt);
	rt_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	cl_printf("ftpd-test: %s\n", b);
	cl_flush();
}

static void reply(const char *fmt, ...) RT_PRINTF(1, 2);
static void reply(const char *fmt, ...)
{
	va_list ap;
	u32 n;
	const char *p = out;

	va_start(ap, fmt);
	rt_vsnprintf(out, sizeof(out) - 2, fmt, ap);
	va_end(ap);
	dlog("<-- %s", out);
	n = (u32)strlen(out);
	out[n++] = '\r';
	out[n++] = '\n';
	while (n) {
		LONG k = send(ctl, (APTR)p, (LONG)n, 0);
		if (k <= 0)
			return;
		p += k;
		n -= (u32)k;
	}
}

/* control line: length, -1 closed/error, -2 Ctrl-C */
static int get_line(void)
{
	u32 n = 0;

	for (;;) {
		char c;
		if (ctlpos >= ctllen) {
			LONG k;
			int w = cl_wait_fd(ctl, 0, 600000);
			if (w == -1)
				return -2;
			if (w <= 0)
				return -1;
			k = recv(ctl, ctlbuf, sizeof(ctlbuf), 0);
			if (k <= 0)
				return n ? (int)n : -1;
			ctlpos = 0;
			ctllen = (u32)k;
		}
		c = ctlbuf[ctlpos++];
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 < sizeof(line))
			line[n++] = c;
	}
	line[n] = 0;
	return (int)n;
}

/* ------------------------------------------------------------ paths */

/* virtual path of arg (relative to cwd) into v; Amiga path into a */
static void mkpath(const char *arg, char *v, char *a)
{
	char tmp[PATHSZ], *p, *seg;
	u32 n;

	if (!arg || !*arg)
		arg = ".";
	if (arg[0] == '/')
		rt_strlcpy(tmp, arg, sizeof(tmp));
	else
		rt_snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);
	v[0] = 0;
	n = 0;
	for (p = tmp; *p;) {
		while (*p == '/')
			p++;
		seg = p;
		while (*p && *p != '/')
			p++;
		if (*p)
			*p++ = 0;
		if (!*seg || !strcmp(seg, "."))
			continue;
		if (!strcmp(seg, "..")) {
			char *s = strrchr(v, '/');
			if (s)
				*s = 0;
			n = (u32)strlen(v);
			continue;
		}
		n += (u32)rt_snprintf(v + n, PATHSZ - n, "/%s", seg);
		if (n >= PATHSZ)
			n = PATHSZ - 1;
	}
	if (!v[0])
		rt_strlcpy(v, "/", PATHSZ);
	n = (u32)strlen(root);
	if (!strcmp(v, "/"))
		rt_strlcpy(a, root, PATHSZ);
	else
		rt_snprintf(a, PATHSZ, "%s%s%s", root, n && (root[n - 1] == ':' || root[n - 1] == '/') ? "" : "/", v + 1);
}

/* 1 directory, 0 file, -1 missing (fib filled) */
static int stat_path(const char *a)
{
	BPTR l = Lock((CONST_STRPTR)a, SHARED_LOCK);
	int r = -1;

	if (!l)
		return -1;
	if (Examine(l, fib))
		r = fib->fib_DirEntryType > 0 ? 1 : 0;
	UnLock(l);
	return r;
}

static int wild(const char *s)
{
	return strchr(s, '*') || strchr(s, '?');
}

static int lc(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int wmatch(const char *p, const char *s)
{
	if (!*p)
		return !*s;
	if (*p == '*')
		return wmatch(p + 1, s) || (*s && wmatch(p, s + 1));
	if (!*s)
		return 0;
	return (*p == '?' || lc(*p) == lc(*s)) && wmatch(p + 1, s + 1);
}

/* days since 1978-01-01 -> y m d */
static void ymd(LONG days, int *y, int *m, int *d)
{
	static const u8 ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int yy = 1978, mm = 0;

	for (;;) {
		int len = (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
		if (days < len)
			break;
		days -= len;
		yy++;
	}
	for (;;) {
		int len = ml[mm] + (mm == 1 && yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0));
		if (days < len)
			break;
		days -= len;
		mm++;
	}
	*y = yy;
	*m = mm + 1;
	*d = (int)days + 1;
}

/* ------------------------------------------------------------ data connection */

static int open_data(void)
{
	int s;

	if (pasv_fd >= 0) {
		struct rt_sockaddr_in6 sa;
		LONG len = sizeof(sa);
		int w = cl_wait_fd(pasv_fd, 0, 30000);
		s = w == 1 ? accept(pasv_fd, &sa, &len) : -1;
		CloseSocket(pasv_fd);
		pasv_fd = -1;
		return s;
	}
	if (!port_port)
		return -1;
	s = socket(port_a.family, RT_SOCK_STREAM, 0);
	if (s >= 0 && cl_connect_addr(s, &port_a, port_port, 30000)) {
		CloseSocket(s);
		s = -1;
	}
	port_port = 0;
	return s;
}

static int send_all(int fd, const char *p, LONG n)
{
	while (n > 0) {
		LONG k;
		if (cl_wait_fd(fd, 1, 60000) != 1)
			return -1;
		k = send(fd, (APTR)p, n, 0);
		if (k <= 0)
			return -1;
		p += k;
		n -= k;
	}
	return 0;
}

static int start_data(const char *what)
{
	int fd;

	reply("150 Opening %s mode data connection for %s.", ascii ? "ASCII" : "BINARY", what);
	fd = open_data();
	if (fd < 0)
		reply("425 Can't open data connection.");
	return fd;
}

/* ------------------------------------------------------------ commands */

static void do_list(const char *arg, int nlst)
{
	char v[PATHSZ], a[PATHSZ], pat[108], prefix[PATHSZ], l[256];
	BPTR lk;
	int fd, t, single = 0, err = 0;

	pat[0] = 0;
	prefix[0] = 0;
	while (arg && *arg == '-') {           /* ls options: ignored */
		while (*arg && *arg != ' ')
			arg++;
		while (*arg == ' ')
			arg++;
	}
	if (arg && !*arg)
		arg = NULL;
	if (arg && wild(arg)) {
		const char *s = strrchr(arg, '/');
		rt_strlcpy(pat, s ? s + 1 : arg, sizeof(pat));
		if (s) {
			u32 n = (u32)(s - arg) + 1;
			if (n >= sizeof(prefix))
				n = sizeof(prefix) - 1;
			memcpy(prefix, arg, n);
			prefix[n] = 0;
			prefix[n - 1] = 0;
			mkpath(prefix[0] ? prefix : "/", v, a);
			prefix[n - 1] = '/';
		} else {
			mkpath(".", v, a);
		}
	} else {
		mkpath(arg, v, a);
	}
	t = stat_path(a);
	if (t < 0) {
		reply("550 %s: No such file or directory.", arg ? arg : ".");
		return;
	}
	if (t == 0)
		single = 1;
	else if (arg && !pat[0] && nlst) {
		rt_snprintf(prefix, sizeof(prefix), "%s%s", arg, arg[strlen(arg) - 1] == '/' ? "" : "/");
	}
	fd = start_data(nlst ? "file list" : "/bin/ls");
	if (fd < 0)
		return;
	lk = Lock((CONST_STRPTR)a, SHARED_LOCK);
	if (lk && Examine(lk, fib)) {
		int more = single ? 1 : ExNext(lk, fib) != 0;
		while (more) {
			if (!pat[0] || wmatch(pat, fib->fib_FileName)) {
				if (nlst) {
					if (single)
						rt_snprintf(l, sizeof(l), "%s\r\n", arg);
					else
						rt_snprintf(l, sizeof(l), "%s%s\r\n", prefix, fib->fib_FileName);
				} else {
					static const char *mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
					                              "Sep", "Oct", "Nov", "Dec" };
					int y, m, d, dir = fib->fib_DirEntryType > 0;
					ymd(fib->fib_Date.ds_Days, &y, &m, &d);
					rt_snprintf(l, sizeof(l), "%s 1 ftp ftp %10ld %s %2d %02ld:%02ld %s\r\n",
					            dir ? "drwxr-xr-x" : "-rw-r--r--", (long)(dir ? 0 : fib->fib_Size), mn[m - 1], d,
					            (long)(fib->fib_Date.ds_Minute / 60), (long)(fib->fib_Date.ds_Minute % 60),
					            fib->fib_FileName);
				}
				if (send_all(fd, l, (LONG)strlen(l)) < 0) {
					err = 1;
					break;
				}
			}
			if (single)
				break;
			more = ExNext(lk, fib) != 0;
		}
	}
	if (lk)
		UnLock(lk);
	CloseSocket(fd);
	if (err)
		reply("426 Connection closed; transfer aborted.");
	else
		reply("226 Transfer complete.");
}

static void do_retr(const char *arg)
{
	char v[PATHSZ], a[PATHSZ];
	BPTR fh;
	int fd, err = 0;
	LONG n;
	u32 off = rest;

	rest = 0;
	mkpath(arg, v, a);
	if (stat_path(a) != 0 || !(fh = Open((CONST_STRPTR)a, MODE_OLDFILE))) {
		reply("550 %s: No such file or directory.", arg);
		return;
	}
	if (off && Seek(fh, (LONG)off, OFFSET_BEGINNING) < 0) {
		Close(fh);
		reply("554 Restart offset %lu is invalid.", (unsigned long)off);
		return;
	}
	fd = start_data(arg);
	if (fd < 0) {
		Close(fh);
		return;
	}
	while ((n = Read(fh, buf, ascii ? BUFSZ / 2 : BUFSZ)) > 0) {
		char *p = buf;
		if (ascii) {
			char *o = cbuf;
			LONG i;
			for (i = 0; i < n; i++) {
				if (buf[i] == '\n')
					*o++ = '\r';
				*o++ = buf[i];
			}
			p = cbuf;
			n = (LONG)(o - cbuf);
		}
		if (send_all(fd, p, n) < 0) {
			err = 1;
			break;
		}
	}
	Close(fh);
	CloseSocket(fd);
	last_aborted = err;
	if (err)
		reply("426 Connection closed; transfer aborted.");
	else
		reply("226 Transfer complete.");
}

static void do_stor(const char *arg, int append)
{
	char v[PATHSZ], a[PATHSZ];
	BPTR fh;
	int fd, err = 0, cr = 0;
	LONG n;
	u32 off = rest;

	rest = 0;
	mkpath(arg, v, a);
	if (stat_path(a) > 0) {
		reply("553 %s: Is a directory.", arg);
		return;
	}
	if (append || off) {
		fh = Open((CONST_STRPTR)a, MODE_READWRITE);
		if (fh && Seek(fh, append ? 0 : (LONG)off, append ? OFFSET_END : OFFSET_BEGINNING) < 0) {
			Close(fh);
			reply("554 Restart offset %lu is invalid.", (unsigned long)off);
			return;
		}
	} else {
		fh = Open((CONST_STRPTR)a, MODE_NEWFILE);
	}
	if (!fh) {
		reply("553 %s: Cannot create file.", arg);
		return;
	}
	fd = start_data(arg);
	if (fd < 0) {
		Close(fh);
		return;
	}
	for (;;) {
		char *p = buf;
		if (cl_wait_fd(fd, 0, 60000) != 1) {
			err = 1;
			break;
		}
		n = recv(fd, buf, BUFSZ, 0);
		if (n <= 0) {
			err = n < 0;
			break;
		}
		if (ascii) {
			char *o = cbuf;
			LONG i;
			if (cr && buf[0] != '\n')
				*o++ = '\r';
			cr = 0;
			for (i = 0; i < n; i++) {
				if (buf[i] == '\r') {
					if (i + 1 == n)
						cr = 1;
					else if (buf[i + 1] != '\n')
						*o++ = '\r';
					continue;
				}
				*o++ = buf[i];
			}
			p = cbuf;
			n = (LONG)(o - cbuf);
		}
		if (n && Write(fh, p, n) != n) {
			err = 2;
			break;
		}
	}
	if (cr)
		Write(fh, "\r", 1);
	Close(fh);
	CloseSocket(fd);
	if (err == 2)
		reply("452 Error writing file.");
	else if (err)
		reply("426 Connection closed; transfer aborted.");
	else
		reply("226 Transfer complete.");
}

static void do_pasv(int ext)
{
	struct rt_sockaddr_in6 sa;
	struct cl_addr x;
	LONG len = sizeof(sa);
	u32 h;
	u16 p;

	if (!ext && local.family == RT_AF_INET6 && !rt_in6_is_v4mapped(&local.a)) {
		reply("500 PASV cannot carry an IPv6 address, use EPSV.");
		return;
	}
	if (pasv_fd >= 0)
		CloseSocket(pasv_fd);
	pasv_fd = socket(local.family, RT_SOCK_STREAM, 0);
	if (pasv_fd < 0 || bind(pasv_fd, &sa, (LONG)cl_sockaddr(&local, 0, &sa)) < 0 || listen(pasv_fd, 1) < 0 ||
	    getsockname(pasv_fd, &sa, &len) < 0) {
		if (pasv_fd >= 0)
			CloseSocket(pasv_fd);
		pasv_fd = -1;
		reply("425 Can't open passive connection.");
		return;
	}
	cl_from_sockaddr(&sa, &x, &p);
	if (ext) {
		reply("229 Entering Extended Passive Mode (|||%u|)", (unsigned)p);
		return;
	}
	h = rt_ntohl(pasv_ip ? pasv_ip : rt_in6_v4(&local.a));
	reply("227 Entering Passive Mode (%lu,%lu,%lu,%lu,%u,%u).", (unsigned long)(h >> 24),
	      (unsigned long)((h >> 16) & 255), (unsigned long)((h >> 8) & 255), (unsigned long)(h & 255),
	      (unsigned)(p >> 8), (unsigned)(p & 255));
}

static void do_port(const char *arg)
{
	unsigned long v[6];
	const char *end;
	int i;

	for (i = 0; i < 6; i++) {
		v[i] = rt_strtoul(arg, &end, 10);
		if (end == arg || v[i] > 255 || (i < 5 && *end != ','))
			break;
		arg = end + 1;
	}
	if (i < 6) {
		reply("501 Illegal PORT command.");
		return;
	}
	memset(&port_a, 0, sizeof(port_a));
	port_a.family = local.family;      /* IPv4-mapped on a dual-stack session */
	rt_in6_set_v4mapped(&port_a.a, rt_htonl((u32)((v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3])));
	port_port = (u16)((v[4] << 8) | v[5]);
	if (pasv_fd >= 0)
		CloseSocket(pasv_fd);
	pasv_fd = -1;
	reply("200 PORT command successful.");
}

/* EPRT |af|address|port| (RFC 2428 2) */
static void do_eprt(const char *arg)
{
	char d, t[64];
	const char *p = arg, *q;
	struct cl_addr x;
	u32 port, n;

	d = *p++;
	if (!d || (p[0] != '1' && p[0] != '2') || p[1] != d) {
		reply("522 Network protocol not supported, use (1,2)");
		return;
	}
	p += 2;
	q = strchr(p, d);
	n = q ? (u32)(q - p) : 0;
	if (!q || n >= sizeof(t)) {
		reply("501 Illegal EPRT command.");
		return;
	}
	memcpy(t, p, n);
	t[n] = 0;
	p = q + 1;
	q = strchr(p, d);
	if (!q || cl_parse_addr(t, &x) < 0) {
		reply("501 Illegal EPRT command.");
		return;
	}
	n = (u32)(q - p);
	if (n >= sizeof(t)) {
		reply("501 Illegal EPRT command.");
		return;
	}
	memcpy(t, p, n);
	t[n] = 0;
	if (cl_parse_uint(t, 65535, &port) < 0 || !port) {
		reply("501 Illegal EPRT command.");
		return;
	}
	if (x.family == RT_AF_INET && local.family == RT_AF_INET6)
		x.family = RT_AF_INET6;        /* IPv4-mapped on a dual-stack session */
	port_a = x;
	port_port = (u16)port;
	if (pasv_fd >= 0)
		CloseSocket(pasv_fd);
	pasv_fd = -1;
	reply("200 EPRT command successful.");
}

static void session(void)
{
	char v[PATHSZ], a[PATHSZ], user[64];
	struct rt_sockaddr_in6 sa;
	LONG len = sizeof(sa);

	rt_strlcpy(cwd, "", sizeof(cwd));
	ascii = 1;
	noepsv = loggedin = last_aborted = 0;
	pasv_ip = rest = 0;
	port_port = 0;
	ctlpos = ctllen = 0;
	user[0] = 0;
	memset(&local, 0, sizeof(local));
	local.family = RT_AF_INET;
	if (getsockname(ctl, &sa, &len) == 0)
		cl_from_sockaddr(&sa, &local, NULL);
	reply("220-Riptide ftpd-test: a minimal FTP server for testing.");
	reply("220-Not for production use.");
	reply("220 Ready.");
	for (;;) {
		char *arg, *p;
		int n = get_line();
		if (n < 0)
			break;
		for (p = line; *p && *p != ' '; p++)
			if (*p >= 'a' && *p <= 'z')
				*p -= 32;
		arg = p;
		if (*arg)
			*arg++ = 0;
		dlog("--> %s %s", line, !strcmp(line, "PASS") ? "****" : arg);
		if (!strcmp(line, "USER")) {
			rt_strlcpy(user, arg, sizeof(user));
			loggedin = 0;
			if (!rt_strcasecmp(arg, "anonymous") || !rt_strcasecmp(arg, "ftp"))
				reply("331 Anonymous login ok, send your e-mail address as password.");
			else
				reply("331 Password required for %s.", arg);
		} else if (!strcmp(line, "PASS")) {
			if (!user[0])
				reply("503 Login with USER first.");
			else if (!strcmp(user, "baduser"))
				reply("530 Login incorrect.");
			else {
				loggedin = 1;
				reply("230 User %s logged in.", user);
			}
		} else if (!strcmp(line, "QUIT")) {
			reply("221 Goodbye.");
			break;
		} else if (!strcmp(line, "NOOP")) {
			reply("200 NOOP command successful.");
		} else if (!strcmp(line, "FEAT")) {
			reply("211-Features:");
			reply(" EPSV");
			reply(" PASV");
			reply(" SIZE");
			reply(" MDTM");
			reply(" REST STREAM");
			reply("211 End");
		} else if (!strcmp(line, "HELP")) {
			reply("214-The following commands are recognized:");
			reply(" USER PASS SYST TYPE PWD CWD CDUP PASV EPSV PORT EPRT LIST NLST RETR STOR");
			reply(" APPE SIZE MDTM DELE MKD RMD RNFR RNTO REST NOOP FEAT HELP ABOR QUIT SITE");
			reply("214 Direct comments to nobody.");
		} else if (!loggedin) {
			reply("530 Please login with USER and PASS.");
		} else if (!strcmp(line, "SYST")) {
			reply("215 UNIX Type: L8");
		} else if (!strcmp(line, "TYPE")) {
			if (arg[0] == 'A' || arg[0] == 'a') {
				ascii = 1;
				reply("200 Type set to A.");
			} else if (arg[0] == 'I' || arg[0] == 'i' || arg[0] == 'L' || arg[0] == 'l') {
				ascii = 0;
				reply("200 Type set to I.");
			} else {
				reply("504 Type %s not implemented.", arg);
			}
		} else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD")) {
			reply("257 \"%s\" is the current directory.", cwd[0] ? cwd : "/");
		} else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP") || !strcmp(line, "XCWD")) {
			mkpath(!strcmp(line, "CDUP") ? ".." : arg, v, a);
			if (stat_path(a) != 1) {
				reply("550 %s: No such file or directory.", arg);
			} else {
				rt_strlcpy(cwd, strcmp(v, "/") ? v : "", sizeof(cwd));
				reply("250 CWD command successful.");
			}
		} else if (!strcmp(line, "PASV")) {
			do_pasv(0);
		} else if (!strcmp(line, "EPSV")) {
			if (noepsv)
				reply("502 EPSV not implemented.");
			else if (!rt_strcasecmp(arg, "ALL"))
				reply("200 EPSV ALL ok.");
			else
				do_pasv(1);
		} else if (!strcmp(line, "PORT")) {
			do_port(arg);
		} else if (!strcmp(line, "EPRT")) {
			do_eprt(arg);
		} else if (!strcmp(line, "LIST") || !strcmp(line, "NLST")) {
			do_list(arg, line[0] == 'N');
		} else if (!strcmp(line, "RETR")) {
			do_retr(arg);
		} else if (!strcmp(line, "STOR") || !strcmp(line, "APPE")) {
			do_stor(arg, line[0] == 'A');
		} else if (!strcmp(line, "SIZE") || !strcmp(line, "MDTM")) {
			mkpath(arg, v, a);
			if (stat_path(a) != 0) {
				reply("550 %s: not a plain file.", arg);
			} else if (line[0] == 'S') {
				reply("213 %ld", (long)fib->fib_Size);
			} else {
				int y, m, d;
				ymd(fib->fib_Date.ds_Days, &y, &m, &d);
				reply("213 %04d%02d%02d%02ld%02ld%02ld", y, m, d, (long)(fib->fib_Date.ds_Minute / 60),
				      (long)(fib->fib_Date.ds_Minute % 60), (long)(fib->fib_Date.ds_Tick / 50));
			}
		} else if (!strcmp(line, "DELE")) {
			mkpath(arg, v, a);
			if (stat_path(a) != 0 || !DeleteFile((CONST_STRPTR)a))
				reply("550 %s: No such file or directory.", arg);
			else
				reply("250 DELE command successful.");
		} else if (!strcmp(line, "MKD") || !strcmp(line, "XMKD")) {
			BPTR l;
			mkpath(arg, v, a);
			if (stat_path(a) >= 0 || !(l = CreateDir((CONST_STRPTR)a))) {
				reply("550 %s: Cannot create directory.", arg);
			} else {
				UnLock(l);
				reply("257 \"%s\" directory created.", v);
			}
		} else if (!strcmp(line, "RMD") || !strcmp(line, "XRMD")) {
			mkpath(arg, v, a);
			if (stat_path(a) != 1 || !strcmp(v, "/") || !DeleteFile((CONST_STRPTR)a))
				reply("550 %s: Cannot remove directory.", arg);
			else
				reply("250 RMD command successful.");
		} else if (!strcmp(line, "RNFR")) {
			mkpath(arg, v, a);
			if (stat_path(a) < 0) {
				rnfr[0] = 0;
				reply("550 %s: No such file or directory.", arg);
			} else {
				rt_strlcpy(rnfr, a, sizeof(rnfr));
				reply("350 File exists, ready for destination name.");
			}
		} else if (!strcmp(line, "RNTO")) {
			mkpath(arg, v, a);
			if (!rnfr[0])
				reply("503 Bad sequence of commands.");
			else if (!Rename((CONST_STRPTR)rnfr, (CONST_STRPTR)a))
				reply("550 %s: Rename failed.", arg);
			else
				reply("250 Rename successful.");
			rnfr[0] = 0;
		} else if (!strcmp(line, "REST")) {
			const char *end;
			rest = (u32)rt_strtoul(arg, &end, 10);
			if (end == arg) {
				rest = 0;
				reply("501 Bad restart offset.");
			} else {
				reply("350 Restarting at %lu. Send STORE or RETRIEVE to initiate transfer.",
				      (unsigned long)rest);
			}
		} else if (!strcmp(line, "ABOR")) {
			if (last_aborted)
				reply("226 ABOR command successful.");
			else
				reply("225 ABOR command successful; no transfer in progress.");
			last_aborted = 0;
		} else if (!strcmp(line, "SITE")) {
			if (!rt_strcasecmp(arg, "NOEPSV")) {
				noepsv = 1;
				reply("200 EPSV disabled.");
			} else if (!rt_strncasecmp(arg, "PASVIP ", 7) && cl_parse_ip(arg + 7, &pasv_ip) == 0) {
				reply("200 PASV will advertise %s.", arg + 7);
			} else if (!rt_strcasecmp(arg, "HELP")) {
				reply("214-SITE commands:");
				reply(" NOEPSV PASVIP HELP");
				reply("214 End.");
			} else {
				reply("500 'SITE %s': command not understood.", arg);
			}
		} else {
			reply("500 '%s': command not understood.", line);
		}
	}
	if (pasv_fd >= 0)
		CloseSocket(pasv_fd);
	pasv_fd = -1;
}

static int ftpd_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in6 sa;
	struct cl_addr any;
	u32 port = 2121;
	int c, s, once = 0, rc = CL_OK, v6 = 0;
	LONG one = 1;

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "p:1vh6", &g)) != -1) {
		switch (c) {
		case '6': v6 = 1; break;
		case 'p':
			if (cl_parse_uint(g.arg, 65535, &port) < 0 || !port)
				return CL_ERROR;
			break;
		case '1': once = 1; break;
		case 'v': verbose = 1; break;
		default:
			cl_puts("usage: ftpd-test [-6] [-p port] [-1] [-v] ROOT\n");
			return c == 'h' ? CL_OK : CL_ERROR;
		}
	}
	if (g.ind >= argc) {
		cl_puts("usage: ftpd-test [-6] [-p port] [-1] [-v] ROOT\n");
		return CL_ERROR;
	}
	root = argv[g.ind];
	if (cl_need_socket() < 0)
		return CL_FAIL;
	buf = AllocVec(BUFSZ, MEMF_ANY);
	cbuf = AllocVec(BUFSZ + 2, MEMF_ANY);
	fib = AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
	if (!buf || !cbuf || !fib) {
		rc = CL_FAIL;
		goto out;
	}
	if (stat_path(root) != 1) {
		cl_error("%s: not a directory", root);
		rc = CL_ERROR;
		goto out;
	}
	memset(&any, 0, sizeof(any));
	any.family = v6 ? RT_AF_INET6 : RT_AF_INET;
	if (!v6)
		rt_in6_set_v4mapped(&any.a, 0);
	s = socket(any.family, RT_SOCK_STREAM, 0);
	if (s >= 0)
		setsockopt(s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, &sa, (LONG)cl_sockaddr(&any, (u16)port, &sa)) < 0 || listen(s, 4) < 0) {
		cl_error("cannot listen on port %lu: %s", (unsigned long)port, cl_strerror((int)Errno()));
		if (s >= 0)
			CloseSocket(s);
		rc = CL_ERROR;
		goto out;
	}
	dlog("listening on port %lu, root %s", (unsigned long)port, root);
	for (;;) {
		LONG len = sizeof(sa);
		int w = cl_wait_fd(s, 0, 0);
		if (w != 1)
			break;
		ctl = accept(s, &sa, &len);
		if (ctl < 0)
			continue;
		{
			char a[CL_ADDRSTRLEN];
			struct cl_addr from;
			cl_from_sockaddr(&sa, &from, NULL);
			dlog("connection from %s", cl_addr_str(&from, a));
		}
		session();
		CloseSocket(ctl);
		ctl = -1;
		dlog("session closed");
		if (once)
			break;
	}
	CloseSocket(s);
out:
	if (buf)
		FreeVec(buf);
	if (cbuf)
		FreeVec(cbuf);
	if (fib)
		FreeVec(fib);
	return rc;
}

CL_MAIN(ftpd_main, "ftpd-test")
