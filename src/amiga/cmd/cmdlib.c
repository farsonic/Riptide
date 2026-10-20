/*
 * Riptide - shared code of the network tools (see cmdlib.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"

struct Library *SocketBase, *RiptideBase;
struct Device *TimerBase;

#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"

#if !defined(__VBCC__)
struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
#endif

const char *cl_prog = "";

#define MAXARGS   64
#define OUTBUF    1024
#define REPLYSIZE 16384

static char outbuf[OUTBUF];
static u32 outlen;
static struct MsgPort *tport;
static struct timerequest *treq;
static char *replybuf;
static int sock_fd = -1;

/* ------------------------------------------------------------ output */

void cl_flush(void)
{
	if (outlen) {
		Write(Output(), outbuf, (LONG)outlen);
		outlen = 0;
	}
}

static void out_raw(const char *s, u32 n)
{
	while (n) {
		u32 k = OUTBUF - outlen;
		if (k > n)
			k = n;
		memcpy(outbuf + outlen, s, k);
		outlen += k;
		s += k;
		n -= k;
		if (outlen == OUTBUF)
			cl_flush();
	}
}

void cl_puts(const char *s)
{
	out_raw(s, (u32)strlen(s));
}

void cl_printf(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = rt_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(b) - 1)
		n = (int)sizeof(b) - 1;
	if (n > 0)
		out_raw(b, (u32)n);
}

void cl_error(const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	cl_printf("%s: %s\n", cl_prog, b);
}

int cl_break(void)
{
	if (CheckSignal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
		cl_flush();
		PutStr((CONST_STRPTR)"***Break\n");
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------ args */

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Split the CLI argument string into words. Quotes group, and inside
 * quotes AmigaDOS escapes (*" *n *e) work. */
static int split_args(char *s, char **argv, int max)
{
	int argc = 1;
	char *w;

	for (;;) {
		while (is_space(*s))
			s++;
		if (!*s || argc >= max)
			break;
		argv[argc++] = w = s;
		if (*s == '"') {
			s++;
			while (*s && *s != '"' && *s != '\n') {
				if (*s == '*' && s[1]) {
					s++;
					*w++ = *s == 'n' || *s == 'N' ? '\n' : *s == 'e' || *s == 'E' ? 27 : *s;
					s++;
				} else {
					*w++ = *s++;
				}
			}
			if (*s == '"')
				s++;
		} else {
			while (*s && !is_space(*s))
				*w++ = *s++;
		}
		if (*s)
			s++;
		*w = 0;
	}
	argv[argc] = NULL;
	return argc;
}

int cl_run(cl_main_fn fn, const char *name)
{
	char **argv, *copy;
	const char *as;
	u32 len;
	int argc, rc;

#if !defined(__VBCC__)
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	if (!DOSBase)
		return CL_FAIL;
#endif
	cl_prog = name;
	as = (const char *)GetArgStr();
	if (!as)
		as = "";
	len = (u32)strlen(as);
	argv = AllocVec(sizeof(char *) * (MAXARGS + 1) + len + 1, MEMF_ANY | MEMF_CLEAR);
	if (!argv) {
		PutStr((CONST_STRPTR)"out of memory\n");
		rc = CL_FAIL;
		goto out;
	}
	copy = (char *)(argv + MAXARGS + 1);
	memcpy(copy, as, len + 1);
	argv[0] = (char *)name;
	argc = split_args(copy, argv, MAXARGS);
	rc = fn(argc, argv);
	cl_flush();
	FreeVec(argv);
out:
	if (sock_fd >= 0 && SocketBase)
		CloseSocket(sock_fd);
	if (RiptideBase)
		CloseLibrary(RiptideBase);
	if (SocketBase)
		CloseLibrary(SocketBase);
	if (treq) {
		CloseDevice((struct IORequest *)treq);
		DeleteIORequest((struct IORequest *)treq);
	}
	if (tport)
		DeleteMsgPort(tport);
	if (replybuf)
		FreeVec(replybuf);
#if !defined(__VBCC__)
	CloseLibrary((struct Library *)DOSBase);
#endif
	return rc;
}

int cl_match(const char *arg, const char *word)
{
	u32 n = (u32)strlen(arg);

	return n && n <= strlen(word) && !strncmp(arg, word, n);
}

int cl_streq_nocase(const char *a, const char *b)
{
	return rt_strcasecmp(a, b) == 0;
}

int cl_getopt(int argc, char **argv, const char *spec, struct cl_getopt *g)
{
	char c, *a;
	const char *p;

	g->arg = NULL;
	if (g->ind < 1)
		g->ind = 1;
	if (!g->pos) {
		if (g->ind >= argc)
			return -1;
		a = argv[g->ind];
		if (a[0] != '-' || !a[1])
			return -1;
		if (!strcmp(a, "--")) {
			g->ind++;
			return -1;
		}
		g->pos = 1;
	}
	a = argv[g->ind];
	c = a[g->pos++];
	p = strchr(spec, c);
	if (!p || c == ':') {
		cl_error("invalid option -- '%c'", c);
		if (!a[g->pos]) {
			g->ind++;
			g->pos = 0;
		}
		return '?';
	}
	if (p[1] == ':') {
		if (a[g->pos]) {
			g->arg = a + g->pos;
		} else if (g->ind + 1 < argc) {
			g->arg = argv[++g->ind];
		} else {
			cl_error("option requires an argument -- '%c'", c);
			g->ind++;
			g->pos = 0;
			return '?';
		}
		g->ind++;
		g->pos = 0;
		return c;
	}
	if (!a[g->pos]) {
		g->ind++;
		g->pos = 0;
	}
	return c;
}

int cl_parse_uint(const char *s, u32 max, u32 *out)
{
	const char *end;
	unsigned long v;

	if (!s || !*s)
		return -1;
	v = rt_strtoul(s, &end, 0);
	if (*end || v > max)
		return -1;
	*out = (u32)v;
	return 0;
}

int cl_parse_ms(const char *s, u32 *ms)
{
	u32 v = 0, frac = 0, scale = 1000;

	if (!s || !*s)
		return -1;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (u32)(*s++ - '0');
		if (v > 4000000)
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

/* ------------------------------------------------------------ libraries */

int cl_need_socket(void)
{
	if (SocketBase)
		return 0;
	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (!SocketBase) {
		cl_error("no TCP/IP stack running (cannot open bsdsocket.library 4)");
		return -1;
	}
	return 0;
}

int cl_need_riptide(void)
{
	if (cl_need_socket() < 0)
		return -1;
	if (RiptideBase)
		return 0;
	RiptideBase = OpenLibrary((CONST_STRPTR)RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
	if (!RiptideBase) {
		cl_error("cannot open " RIPTIDE_LIB_NAME " (is Riptide running?)");
		return -1;
	}
	return 0;
}

int cl_need_timer(void)
{
	if (TimerBase)
		return 0;
	tport = CreateMsgPort();
	treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof(*treq)) : NULL;
	if (!treq || OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)treq, 0)) {
		if (treq)
			DeleteIORequest((struct IORequest *)treq);
		treq = NULL;
		cl_error("cannot open timer.device");
		return -1;
	}
	TimerBase = treq->tr_node.io_Device;
	return 0;
}

u32 cl_now_us(void)
{
	struct timeval t;

	GetSysTime(&t);
	return (u32)t.tv_secs * 1000000UL + (u32)t.tv_micro;
}

int cl_socket(int type, int proto)
{
	if (cl_need_socket() < 0)
		return -1;
	if (type == RT_SOCK_DGRAM && !proto) {
		if (sock_fd < 0)
			sock_fd = socket(RT_AF_INET, RT_SOCK_DGRAM, 0);
		if (sock_fd < 0)
			cl_error("cannot create a socket");
		return sock_fd;
	}
	return socket(RT_AF_INET, type, proto);
}

int cl_ioctl(int fd, u32 req, void *arg)
{
	if (IoctlSocket(fd, req, arg) < 0)
		return (int)Errno();
	return 0;
}

/* ------------------------------------------------------------ addresses */

int cl_parse_ip(const char *s, u32 *addr)
{
	u32 a = 0;
	int part, digits;

	for (part = 0; part < 4; part++) {
		u32 v = 0;
		for (digits = 0; *s >= '0' && *s <= '9'; s++, digits++) {
			v = v * 10 + (u32)(*s - '0');
			if (digits >= 3 || v > 255)
				return -1;
		}
		if (!digits)
			return -1;
		a = (a << 8) | v;
		if (part < 3 && *s++ != '.')
			return -1;
	}
	if (*s)
		return -1;
	*addr = rt_htonl(a);
	return 0;
}

u32 cl_mask(int bits)
{
	return bits <= 0 ? 0 : rt_htonl(0xffffffffUL << (32 - bits));
}

int cl_parse_prefix(const char *s, u32 *addr, int *bits)
{
	char b[20];
	const char *slash = strchr(s, '/');
	u32 n;

	*bits = -1;
	if (!slash)
		return cl_parse_ip(s, addr);
	if ((u32)(slash - s) >= sizeof(b))
		return -1;
	memcpy(b, s, (u32)(slash - s));
	b[slash - s] = 0;
	if (cl_parse_uint(slash + 1, 32, &n) < 0)
		return -1;
	*bits = (int)n;
	return cl_parse_ip(b, addr);
}

char *cl_ip(u32 addr, char *buf)
{
	u32 h = rt_ntohl(addr);

	rt_snprintf(buf, 16, "%lu.%lu.%lu.%lu", (unsigned long)(h >> 24), (unsigned long)((h >> 16) & 255),
	            (unsigned long)((h >> 8) & 255), (unsigned long)(h & 255));
	return buf;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int cl_parse_mac(const char *s, u8 *mac)
{
	int i, h, l;

	for (i = 0; i < 6; i++) {
		if ((h = hexval(*s)) < 0)
			return -1;
		s++;
		if ((l = hexval(*s)) >= 0) {
			h = h * 16 + l;
			s++;
		}
		mac[i] = (u8)h;
		if (i < 5) {
			if (*s != ':' && *s != '-')
				return -1;
			s++;
		}
	}
	return *s ? -1 : 0;
}

char *cl_mac(const u8 *mac, int len, char *buf)
{
	static const char hx[] = "0123456789abcdef";
	int i;
	char *p = buf;

	for (i = 0; i < len; i++) {
		if (i)
			*p++ = ':';
		*p++ = hx[mac[i] >> 4];
		*p++ = hx[mac[i] & 15];
	}
	*p = 0;
	return buf;
}

u32 cl_classful(u32 addr)
{
	u32 h = rt_ntohl(addr);

	if (!(h & 0x80000000UL))
		return rt_htonl(0xff000000UL);
	if ((h & 0xc0000000UL) == 0x80000000UL)
		return rt_htonl(0xffff0000UL);
	return rt_htonl(0xffffff00UL);
}

int cl_resolve(const char *name, u32 *addr)
{
	struct hostent *h;

	if (cl_parse_ip(name, addr) == 0)
		return 0;
	if (cl_need_socket() < 0)
		return -1;
	h = (struct hostent *)gethostbyname((CONST_STRPTR)name);
	if (!h || !h->h_addr_list || !h->h_addr_list[0]) {
		cl_error("%s: Name or service not known", name);
		return -1;
	}
	memcpy(addr, h->h_addr_list[0], 4);
	return 0;
}

char *cl_hostname(u32 addr, int numeric, char *buf, u32 len)
{
	struct hostent *h;

	if (!numeric && addr && cl_need_socket() == 0) {
		h = (struct hostent *)gethostbyaddr((APTR)&addr, 4, RT_AF_INET);
		if (h && h->h_name && h->h_name[0]) {
			rt_strlcpy(buf, h->h_name, len);
			return buf;
		}
	}
	if (len >= 16)
		cl_ip(addr, buf);
	else
		buf[0] = 0;
	return buf;
}

const char *cl_strerror(int err)
{
	switch (err) {
	case RT_EPERM: return "Operation not permitted";
	case RT_ENOENT: return "No such file or directory";
	case RT_ESRCH: return "No such process";
	case RT_EINTR: return "Interrupted system call";
	case RT_ENXIO: return "No such device or address";
	case RT_ENOMEM: return "Cannot allocate memory";
	case RT_EACCES: return "Permission denied";
	case RT_EBUSY: return "Device or resource busy";
	case RT_EEXIST: return "File exists";
	case RT_ENODEV: return "No such device";
	case RT_EINVAL: return "Invalid argument";
	case RT_ENOSPC: return "No space left on device";
	case RT_EOPNOTSUPP: return "Operation not supported";
	case RT_EAFNOSUPPORT: return "Address family not supported by protocol";
	case RT_EADDRINUSE: return "Address already in use";
	case RT_EADDRNOTAVAIL: return "Cannot assign requested address";
	case RT_ENETDOWN: return "Network is down";
	case RT_ENETUNREACH: return "Network is unreachable";
	case RT_ENOBUFS: return "No buffer space available";
	case RT_ESHUTDOWN: return "Cannot send after transport endpoint shutdown";
	case RT_ETIMEDOUT: return "Connection timed out";
	case RT_ECONNREFUSED: return "Connection refused";
	case RT_EHOSTDOWN: return "Host is down";
	case RT_EHOSTUNREACH: return "No route to host";
	case RT_EMSGSIZE: return "Message too long";
	}
	return "Unknown error";
}

/* ------------------------------------------------------------ Riptide */

void *cl_dump(int what, struct rt_ctl_hdr *hdr)
{
	LONG mib[3], len;
	u8 *buf;
	int tries;

	if (cl_need_riptide() < 0)
		return NULL;
	mib[0] = 4;             /* CTL_NET */
	mib[1] = RT_CTL_MIB_RIPTIDE;
	mib[2] = what;
	for (tries = 0; tries < 4; tries++) {
		len = 0;
		if (RiptideSysCtl(mib, 3, NULL, &len, NULL, 0) != 0) {
			cl_error("cannot read the Riptide tables (not Riptide 0.10 or later?)");
			return NULL;
		}
		len += len / 4 + 256;           /* room for growth meanwhile */
		buf = AllocVec((ULONG)len, MEMF_ANY | MEMF_CLEAR);
		if (!buf) {
			cl_error("out of memory");
			return NULL;
		}
		if (RiptideSysCtl(mib, 3, buf, &len, NULL, 0) == 0) {
			memcpy(hdr, buf, sizeof(*hdr));
			if (hdr->version != RT_CTL_VERSION || hdr->recsize == 0) {
				FreeVec(buf);
				cl_error("unknown table format %u", (unsigned)hdr->version);
				return NULL;
			}
			return buf;
		}
		FreeVec(buf);
		if (Errno() != RT_ENOMEM)
			break;
	}
	cl_error("cannot read the Riptide tables");
	return NULL;
}

struct rt_ctl_iface *cl_find_iface(void *dump, const struct rt_ctl_hdr *h, const char *name)
{
	u32 i;

	for (i = 0; i < h->count; i++) {
		struct rt_ctl_iface *r = cl_rec(dump, h, i);
		if (!strcmp(r->name, name))
			return r;
	}
	return NULL;
}

struct rt_ctl_iface *cl_iface_by_index(void *dump, const struct rt_ctl_hdr *h, u32 index)
{
	u32 i;

	for (i = 0; i < h->count; i++) {
		struct rt_ctl_iface *r = cl_rec(dump, h, i);
		if (r->index == index)
			return r;
	}
	return NULL;
}

static int route_before(const struct rt_ctl_route *a, const struct rt_ctl_route *b)
{
	u32 da = rt_ntohl(a->dst), db = rt_ntohl(b->dst);
	int ad = !a->dst && !a->mask, bd = !b->dst && !b->mask;

	if (ad != bd)
		return ad;
	if (da != db)
		return da < db;
	return rt_ntohl(a->mask) > rt_ntohl(b->mask);
}

void cl_sort_routes(void *dump, const struct rt_ctl_hdr *h)
{
	struct rt_ctl_route tmp;
	u32 i, j;

	if (h->recsize != sizeof(struct rt_ctl_route))
		return;
	for (i = 1; i < h->count; i++) {
		memcpy(&tmp, cl_rec(dump, h, i), sizeof(tmp));
		for (j = i; j > 0 && route_before(&tmp, cl_rec(dump, h, j - 1)); j--)
			memcpy(cl_rec(dump, h, j), cl_rec(dump, h, j - 1), sizeof(tmp));
		memcpy(cl_rec(dump, h, j), &tmp, sizeof(tmp));
	}
}

const char *cl_reply(void)
{
	return replybuf ? replybuf : "";
}

const char *cl_line(const char *fmt, ...)
{
	static char line[RIPTIDE_TEXTLEN];
	va_list ap;

	va_start(ap, fmt);
	rt_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	return line;
}

int cl_command(const char *line, int print)
{
	struct MsgPort *reply, *port;
	struct RiptideMsg *m;
	int r;

	if (!replybuf)
		replybuf = AllocVec(REPLYSIZE, MEMF_PUBLIC | MEMF_CLEAR);
	reply = CreateMsgPort();
	m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
	if (!replybuf || !reply || !m) {
		cl_error("out of memory");
		r = RT_ENOMEM;
		goto out;
	}
	replybuf[0] = 0;
	m->msg.mn_Node.ln_Type = NT_MESSAGE;
	m->msg.mn_ReplyPort = reply;
	m->msg.mn_Length = sizeof(*m);
	m->cmd = RTCMD_COMMAND;
	m->reply = (STRPTR)replybuf;
	m->replysize = REPLYSIZE;
	rt_strlcpy(m->text, line, sizeof(m->text));
	Forbid();
	port = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME);
	if (port)
		PutMsg(port, &m->msg);
	Permit();
	if (!port) {
		cl_error("Riptide is not running");
		r = -1;
		goto out;
	}
	/* Riptide replies to every message, also when it shuts down */
	WaitPort(reply);
	GetMsg(reply);
	replybuf[REPLYSIZE - 1] = 0;
	r = (int)m->result;
	if (print && replybuf[0])
		cl_puts(replybuf);
out:
	if (m)
		FreeVec(m);
	if (reply)
		DeleteMsgPort(reply);
	return r;
}
