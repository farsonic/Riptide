/*
 * Riptide - the Riptide Terminal: the Amiga's network tools on the classic Mac OS.
 *
 * Riptide for the Amiga comes with command-line tools (src/amiga/cmd): ip, ifconfig, route, arp,
 * netstat, ping, ping6, traceroute, traceroute6, host, hostname, nc, ftp, sysctl, speedtest. The
 * Mac has no command line, so this program is one: a window (movable, resizable, zoomable) with
 * a scrolling console in Monaco 9 and a prompt. A line typed there runs the tool it names, the
 * very same source as on the Amiga, compiled for the Mac with the Amiga's types and plain
 * prototypes (as RiptidePrefs is, docs/MAC-GUI.md): its output goes to the console, Command-period
 * stops it (the Amiga's Ctrl-C), and nc and ftp read lines typed while they run (Command-D or
 * Control-D ends that input, the Amiga's Ctrl-\). Their socket calls reach the stack through
 * term_bsd.c, the network stack's calls for programs.
 *
 * Every tool's main() is compiled under its own name (Makefile.mac: -Damiga_main=main_ip ...).
 * On the Amiga each run of a tool is a fresh program; here the tools stay loaded, so their static
 * data is put back before every run (term.ld gathers it between __tool_data and __tool_bss_end).
 *
 * Built in: help, clear, history, conf LINE (a riptide.conf line for the running stack, as the
 * RIPTIDE port's COMMAND), quit. At the start the lines of Preferences:Riptide:Terminal Startup,
 * if there is such a file, are run as if typed.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "term.h"

#define main_decl(n) long main_##n(char *, long)
main_decl(arp); main_decl(ftp); main_decl(host); main_decl(hostname); main_decl(ifconfig); main_decl(ip);
main_decl(nc); main_decl(netstat); main_decl(ping); main_decl(ping6); main_decl(route); main_decl(sysctl);
main_decl(traceroute); main_decl(traceroute6); main_decl(speedtest);

static const struct tool { const char *name; long (*main)(char *, long); const char *what; } tools[] = {
	{ "ip", main_ip, "addresses, routes, neighbours, links (iproute2)" },
	{ "ifconfig", main_ifconfig, "interfaces (net-tools)" },
	{ "route", main_route, "the routing table" },
	{ "arp", main_arp, "the ARP cache" },
	{ "netstat", main_netstat, "connections, statistics (-s), routes (-r)" },
	{ "ping", main_ping, "ICMP echo (-c COUNT, -6)" },
	{ "ping6", main_ping6, "ICMPv6 echo" },
	{ "traceroute", main_traceroute, "the path to a host" },
	{ "traceroute6", main_traceroute6, "the path to a host over IPv6" },
	{ "host", main_host, "DNS lookups" },
	{ "hostname", main_hostname, "this Mac's host name" },
	{ "nc", main_nc, "TCP and UDP connections (netcat)" },
	{ "ftp", main_ftp, "FTP client" },
	{ "sysctl", main_sysctl, "the stack's settings (-a)" },
	{ "speedtest", main_speedtest, "speedtest.net measurement" },
};
#define NTOOLS ((int)(sizeof tools / sizeof tools[0]))

extern char __tool_data[], __tool_data_end[], __tool_bss[], __tool_bss_end[];
static char *tool_data_copy;

/* ---- the window and the console ---- */
#define APPLE_ID 128
#define FILE_ID  129
#define EDIT_ID  130
#define SBW      16
#define MAXTEXT  30000          /* TextEdit holds 32767 characters: the oldest go past this */
#define PROMPT   "riptide> "

static void *win, *sbar, *menus[3];
static TEHandle te;
static short in_start;          /* where the line being typed begins */
static int running, quit_asked, brk, follow = 1;
static ULONG last_poll;
static int col;                 /* the console's column, for tabs */
const char *term_args = "";

/* what was typed while a command runs: complete lines, and the one being typed */
static char in_q[2048];
static int in_qlen;
static char in_cur[256];
static int in_curlen, in_eof;

static char *hist[32];
static int nhist, hpos;

static Rect content(void)
{
	Rect r = *PORT_RECT(win);
	return r;
}

static void layout(void)
{
	Rect r = content(), v = r;
	MoveControl(sbar, r.right - SBW + 1, r.top - 1);
	SizeControl(sbar, SBW, r.bottom - r.top - SBW + 3);
	v.right -= SBW;
	v.bottom -= SBW;
	InsetRect(&v, 4, 2);
	(*te)->viewRect = v;
	(*te)->destRect.right = v.right;
	(*te)->destRect.left = v.left;
	TECalText(te);
}

static int line_height(void) { return (*te)->lineHeight ? (*te)->lineHeight : 11; }
static int visible_lines(void) { Rect v = (*te)->viewRect; return (v.bottom - v.top) / line_height(); }
static int top_line(void) { return ((*te)->viewRect.top - (*te)->destRect.top) / line_height(); }

static void sync_bar(void)
{
	int max = (*te)->nLines - visible_lines() + 1;
	if (max < 0)
		max = 0;
	SetCtlMax(sbar, max);
	SetCtlValue(sbar, top_line());
}

static void scroll_to(int line)
{
	int max = GetCtlMax(sbar);
	if (line > max)
		line = max;
	if (line < 0)
		line = 0;
	TEScroll(0, (top_line() - line) * line_height(), te);
	SetCtlValue(sbar, line);
}

static void to_end(void)
{
	sync_bar();
	scroll_to(GetCtlMax(sbar));
}

/* the scroll bar's arrows and grey areas (ostraps.S pas_ctl_action) */
void c_ctl_action(void *ctl, int part)
{
	int page = visible_lines() - 1;
	(void)ctl;
	if (page < 1)
		page = 1;
	switch (part) {
	case 20: scroll_to(top_line() - 1); break;
	case 21: scroll_to(top_line() + 1); break;
	case 22: scroll_to(top_line() - page); break;
	case 23: scroll_to(top_line() + page); break;
	}
	follow = top_line() >= GetCtlMax(sbar);
}

static void trim(void)
{
	short len = (*te)->teLength, cut;
	char *t;
	if (len < MAXTEXT)
		return;
	cut = len - MAXTEXT + 4000;
	t = *(*te)->hText;
	while (cut < len && t[cut - 1] != '\r')
		cut++;
	TESetSelect(0, cut, te);
	TEDelete(te);
	in_start = in_start > cut ? in_start - cut : 0;
}

/* text to the end of the console (LF line ends; tabs to every eighth column) */
static void append(const char *s, LONG n)
{
	char buf[512];
	int k = 0;
	short sel0 = (*te)->selStart, sel1 = (*te)->selEnd, end = (*te)->teLength;
	int at_end = sel0 == end;
	TESetSelect(end, end, te);
	while (n > 0) {
		char ch = *s++;
		n--;
		if (ch == '\r')
			continue;
		if (ch == '\t') {
			do
				buf[k++] = ' ';
			while (++col % 8 && k < (int)sizeof buf);
		} else if (ch == '\n') {
			buf[k++] = '\r';
			col = 0;
		} else if ((unsigned char)ch >= 32 || ch == 27) {
			buf[k++] = ch == 27 ? '^' : ch;
			col++;
		}
		if (k > (int)sizeof buf - 10 || !n) {
			TEInsert(buf, k, te);
			k = 0;
		}
	}
	if (k)
		TEInsert(buf, k, te);
	trim();
	if (!at_end)
		TESetSelect(sel0, sel1, te);
	if (follow)
		to_end();
	else
		sync_bar();
}

void term_out(const char *s, LONG n)
{
	append(s, n);
	if (running)
		term_poll();
}

static void prompt(void)
{
	if (col)
		append("\n", 1);
	append(PROMPT, sizeof PROMPT - 1);
	in_start = (*te)->teLength;
	follow = 1;
	to_end();
}

/* ---- input for running commands ---- */
int term_in_ready(void) { return in_qlen > 0 || in_eof; }

LONG term_in(char *buf, LONG n)
{
	LONG k = 0;
	while (!in_qlen && !in_eof && !brk)
		term_yield(NULL);
	if (in_eof && !in_qlen) {
		in_eof = 0;
		return 0;
	}
	while (k < n && k < in_qlen) {
		buf[k] = in_q[k];
		if (buf[k++] == '\n')
			break;
	}
	memmove(in_q, in_q + k, (size_t)(in_qlen - k));
	in_qlen -= k;
	return k;
}

static void running_key(int ch)
{
	if (ch == 4) {
		/* Control-D or Command-D: the end of the input (what is typed so far goes first) */
		if (in_curlen && in_qlen + in_curlen < (int)sizeof in_q) {
			memcpy(in_q + in_qlen, in_cur, (size_t)in_curlen);
			in_qlen += in_curlen;
		}
		in_curlen = 0;
		in_eof = 1;
		append("\n", 1);
	} else if (ch == '\r' || ch == 3) {
		if (in_qlen + in_curlen + 1 < (int)sizeof in_q) {
			memcpy(in_q + in_qlen, in_cur, (size_t)in_curlen);
			in_qlen += in_curlen;
			in_q[in_qlen++] = '\n';
		}
		in_curlen = 0;
		append("\n", 1);
	} else if (ch == 8) {
		if (in_curlen) {
			short e = (*te)->teLength;
			in_curlen--;
			TESetSelect(e - 1, e, te);
			TEDelete(te);
			col--;
		}
	} else if (ch >= 32 && in_curlen < (int)sizeof in_cur - 1) {
		char c = (char)ch;
		in_cur[in_curlen++] = c;
		append(&c, 1);
	}
}

/* ---- breaks ---- */
int term_break_pending(void *arg) { (void)arg; return brk; }
void term_break_take(void) { brk = 0; }

/* ---- the history ---- */
static void hist_add(const char *line)
{
	if (!*line || (nhist && !strcmp(hist[nhist - 1], line)))
		return;
	if (nhist == 32) {
		mm_free(hist[0]);
		memmove(hist, hist + 1, sizeof(hist[0]) * 31);
		nhist--;
	}
	hist[nhist++] = mm_strdup(line);
}

static void set_input(const char *s)
{
	TESetSelect(in_start, (*te)->teLength, te);
	TEDelete(te);
	TEInsert(s, (long)strlen(s), te);
	col = (int)(PROMPT[0] ? sizeof PROMPT - 1 : 0) + (int)strlen(s);
	to_end();
}

/* ---- running a line ---- */
static void run_line(char *line);

static void help(void)
{
	int i;
	static const char head[] = "Riptide's network tools (Command-period stops one):\n";
	term_out(head, (LONG)sizeof head - 1);
	for (i = 0; i < NTOOLS; i++) {
		char b[120];
		rt_snprintf(b, sizeof b, "  %-12s %s\n", tools[i].name, tools[i].what);
		term_out(b, (LONG)strlen(b));
	}
	{
		static const char more[] =
			"  conf LINE    a riptide.conf line for the running stack (dns, host, route ...)\n"
			"  history      the lines typed; Up and Down arrows bring them back\n"
			"  clear        empty the window\n"
			"While a tool runs, Command-D (or Control-D) ends the lines it reads (nc, ftp).\n"
			"  quit\n"
			"\"TOOL -h\" or \"TOOL --help\" shows a tool's options.\n";
		term_out(more, (LONG)sizeof more - 1);
	}
}

static void run_tool(const struct tool *t, const char *args)
{
	char *a = mm_alloc((ULONG)strlen(args) + 2);
	if (!a)
		return;
	strcpy(a, args);
	strcat(a, "\n");
	term_args = a;
	/* the tool's static data as it was at the start */
	memcpy(__tool_data, tool_data_copy, (size_t)(__tool_data_end - __tool_data));
	memset(__tool_bss, 0, (size_t)(__tool_bss_end - __tool_bss));
	brk = 0;
	in_qlen = in_curlen = in_eof = 0;
	running = 1;
	SetCursor(*(void **)GetCursor(4));               /* the watch */
	t->main(NULL, 0);
	InitCursor();
	running = 0;
	if (brk)
		term_out("***Break\n", 9);
	brk = 0;
	term_args = "";
	mm_free(a);
}

static void run_line(char *line)
{
	char cmd[32];
	const char *args;
	int i = 0;
	while (*line == ' ')
		line++;
	while (line[i] && line[i] != ' ' && i < (int)sizeof cmd - 1) {
		cmd[i] = line[i];
		i++;
	}
	cmd[i] = 0;
	args = line + i;
	while (*args == ' ')
		args++;
	if (!cmd[0])
		return;
	if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
		help();
	} else if (!strcmp(cmd, "clear")) {
		TESetSelect(0, (*te)->teLength, te);
		TEDelete(te);
		col = 0;
	} else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
		quit_asked = 1;
	} else if (!strcmp(cmd, "history")) {
		for (i = 0; i < nhist; i++) {
			char b[300];
			rt_snprintf(b, sizeof b, "%4d  %s\n", i + 1, hist[i]);
			term_out(b, (LONG)strlen(b));
		}
	} else if (!strcmp(cmd, "conf")) {
		char *out = mm_alloc(4096);
		if (out) {
			int r = term_api_ok() || term_api_open() == 0 ? term_api_command(args, out, 4096) : -1;
			if (r < 0 && !out[0])
				rt_strlcpy(out, term_api_ok() ? "error\n" : "Riptide is not running\n", 4096);
			term_out(out, (LONG)strlen(out));
			mm_free(out);
		}
	} else {
		for (i = 0; i < NTOOLS; i++)
			if (!strcmp(cmd, tools[i].name)) {
				run_tool(&tools[i], args);
				return;
			}
		{
			char b[80];
			rt_snprintf(b, sizeof b, "%s: unknown command (\"help\" lists them)\n", cmd);
			term_out(b, (LONG)strlen(b));
		}
	}
}

/* the line typed at the prompt */
static void enter_line(void)
{
	short len = (*te)->teLength, n = len - in_start;
	char *line;
	if (n < 0)
		n = 0;
	line = mm_alloc((ULONG)n + 1);
	if (!line)
		return;
	memcpy(line, *(*te)->hText + in_start, (size_t)n);
	line[n] = 0;
	TESetSelect(len, len, te);
	append("\n", 1);
	hist_add(line);
	hpos = nhist;
	run_line(line);
	mm_free(line);
	if (!quit_asked)
		prompt();
}

/* ---- events ---- */
static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	if (id == APPLE_ID) {
		if (item == 1) {
			/* an About box (Human Interface Guidelines p. 98), not text in the console */
			int mac_alert(int icon, const char *text, const char *const *btn, int n, int def, int cancel);
			static const char *const ok[] = { "OK" };
			mac_alert(1, "Riptide Terminal\n\nRiptide's network tools (ip, ping, arp, route, netstat, host, "
			             "traceroute, nc, ftp ...) in a Mac window, running on the Riptide TCP/IP stack. "
			             "Type help for the list.", ok, 1, 0, -1);
		} else {
			unsigned char name[64];
			GetItem(menus[0], item, name);
			OpenDeskAcc(name);
		}
	} else if (id == FILE_ID) {
		if (item == 1) {
			TESetSelect(0, in_start, te);
			TEDelete(te);
			in_start = 0;
			if (!running) {
				TESetSelect(0, 0, te);
				TEInsert(PROMPT, sizeof PROMPT - 1, te);
				in_start = sizeof PROMPT - 1;
				TESetSelect((*te)->teLength, (*te)->teLength, te);
			}
			to_end();
		} else if (item == 3) {
			quit_asked = 1;
			if (running)
				brk = 1;
		}
	} else if (id == EDIT_ID && !SystemEdit(item - 1)) {
		if (item == 4) {
			TECopy(te);
			ZeroScrap();
			PutScrap(LM_TESCRPLEN, MAC_TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
		} else if (item == 5 && !running) {
			if ((*te)->selStart < in_start)
				TESetSelect((*te)->teLength, (*te)->teLength, te);
			TEPaste(te);
		}
	}
	HiliteMenu(0);
}

static void key(EventRecord *ev)
{
	int ch = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) {
		if (ch == '.') {
			if (running)
				brk = 1;
			return;
		}
		if (running && (ch == 'd' || ch == 'D')) {
			running_key(4);
			return;
		}
		if (ch == 'k' || ch == 'K') {
			do_menu(((long)FILE_ID << 16) | 1);
			return;
		}
		do_menu(MenuKey(ch));
		return;
	}
	if (running) {
		running_key(ch);
		return;
	}
	ObscureCursor();
	follow = 1;
	switch (ch) {
	case '\r':
	case 3:
		enter_line();
		return;
	case 8:
		if ((*te)->selStart > in_start || ((*te)->selEnd > (*te)->selStart && (*te)->selStart >= in_start))
			TEKey(8, te);
		break;
	case 28:
		if ((*te)->selStart > in_start)
			TEKey(28, te);
		break;
	case 29:
		TEKey(29, te);
		break;
	case 30:
	case 31:
		if (!nhist)
			break;
		hpos += ch == 30 ? -1 : 1;
		if (hpos < 0)
			hpos = 0;
		if (hpos >= nhist) {
			hpos = nhist;
			set_input("");
		} else {
			set_input(hist[hpos]);
		}
		return;
	default:
		if (ch < 32)
			break;
		if ((*te)->selStart < in_start)
			TESetSelect((*te)->teLength, (*te)->teLength, te);
		TEKey(ch, te);
		col++;
	}
	to_end();
}

static void grow(EventRecord *ev)
{
	Rect lim = { 80, 200, 4000, 4000 };
	long s = GrowWindow(win, pt_long(ev->where), &lim);
	if (!s)
		return;
	SizeWindow(win, (int)(s & 0xFFFF), (int)(s >> 16), 1);
	{
		Rect r = content();
		InvalRect(&r);
		EraseRect(&r);
	}
	layout();
	to_end();
}

static void mouse(EventRecord *ev)
{
	void *w;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar:
		do_menu(MenuSelect(pt_long(ev->where)));
		break;
	case inSysWindow:
		SystemClick(ev, w);
		break;
	case inDrag: {
		Rect lim = *RGN_BBOX(LM_GRAYRGN);
		DragWindow(w, pt_long(ev->where), &lim);
		break;
	}
	case inGrow:
		grow(ev);
		break;
	case inGoAway:
		quit_asked = 1;
		if (running)
			brk = 1;
		break;
	case inZoomIn:
	case inZoomOut:
		ZoomWindow(w, part, 1);
		{
			Rect r = content();
			EraseRect(&r);
			InvalRect(&r);
		}
		layout();
		to_end();
		break;
	case inContent:
		if (w != FrontWindow()) {
			SelectWindow(w);
			break;
		}
		{
			Point p = ev->where;
			void *ctl;
			int cp;
			SetPort(win);
			GlobalToLocal(&p);
			cp = FindControl(pt_long(p), win, &ctl);
			if (cp == 129) {
				TrackControl(ctl, pt_long(p), NULL);
				scroll_to(GetCtlValue(sbar));
				follow = top_line() >= GetCtlMax(sbar);
			} else if (cp) {
				TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
			} else {
				TEClick(pt_long(p), (ev->modifiers & shiftKey) != 0, te);
			}
		}
		break;
	}
}

static void update(void)
{
	Rect r = content();
	SetPort(win);
	BeginUpdate(win);
	EraseRect(&r);
	TEUpdate(&r, te);
	DrawControls(win);
	DrawGrowIcon(win);
	EndUpdate(win);
}

static void event(EventRecord *ev)
{
	switch (ev->what) {
	case mouseDown:
		mouse(ev);
		break;
	case keyDown:
	case autoKey:
		key(ev);
		break;
	case updateEvt:
		if ((void *)ev->message == win)
			update();
		break;
	case activateEvt:
		if ((void *)ev->message == win) {
			if (ev->modifiers & activeFlag) {
				TEActivate(te);
				HiliteControl(sbar, 0);
			} else {
				TEDeactivate(te);
				HiliteControl(sbar, 255);
			}
			DrawGrowIcon(win);
		}
		break;
	case osEvt:
		if (((ULONG)ev->message >> 24) == suspendResumeMessage) {
			if (ev->message & resumeFlag)
				TEActivate(te);
			else
				TEDeactivate(te);
		}
		break;
	}
}

/* while a command runs: one round of events (the stack's calls wait in here) */
void term_yield(void *arg)
{
	EventRecord ev;
	(void)arg;
	if (WaitNextEvent(everyEvent, &ev, 1, NULL))
		event(&ev);
	else
		TEIdle(te);
	last_poll = TickCount();
}

void term_poll(void)
{
	if (TickCount() - last_poll >= 6) {
		EventRecord ev;
		if (WaitNextEvent(everyEvent, &ev, 0, NULL))
			event(&ev);
		last_poll = TickCount();
	}
}

/* ---- start ---- */
static void menus_init(void)
{
	unsigned char pb[100];
	menus[0] = NewMenu(APPLE_ID, mac_pstr(pb, "\x14"));
	AppendMenu(menus[0], mac_pstr(pb, "About Riptide Terminal;(-"));
	AddResMenu(menus[0], MAC_TYPE('D', 'R', 'V', 'R'));
	InsertMenu(menus[0], 0);
	menus[1] = NewMenu(FILE_ID, mac_pstr(pb, "File"));
	AppendMenu(menus[1], mac_pstr(pb, "Clear Window/K;(-;Quit/Q"));
	InsertMenu(menus[1], 0);
	menus[2] = NewMenu(EDIT_ID, mac_pstr(pb, "Edit"));
	AppendMenu(menus[2], mac_pstr(pb, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(menus[2], 0);
	DrawMenuBar();
}

static void run_startup(void)
{
	BPTR f = Open((CONST_STRPTR)"ENVARC:Riptide/Terminal Startup", MODE_OLDFILE);
	char buf[2048], *p, *e;
	LONG n;
	if (!f)
		return;
	n = Read(f, buf, sizeof buf - 1);
	Close(f);
	if (n <= 0)
		return;
	buf[n] = 0;
	for (p = buf; *p && !quit_asked; p = e) {
		e = p;
		while (*e && *e != '\n')
			e++;
		if (*e)
			*e++ = 0;
		if (!*p || *p == '#')
			continue;
		append(p, (LONG)strlen(p));
		append("\n", 1);
		hist_add(p);
		hpos = nhist;
		run_line(p);
		prompt();
	}
}

int main(void)
{
	void *a5;
	Rect r, sb;
	unsigned char title[32];
	EventRecord ev;
	__asm__ volatile("move.l %%a5,%0" : "=r"(a5));
	sb = *LM_SCREENBITS_BOUNDS(*(void **)a5);
	mm_paths_init();
	tool_data_copy = mm_alloc((ULONG)(__tool_data_end - __tool_data) + 4);
	if (tool_data_copy)
		memcpy(tool_data_copy, __tool_data, (size_t)(__tool_data_end - __tool_data));
	menus_init();
	r.left = sb.left + 6;
	r.top = sb.top + LM_MBARHEIGHT + 22;
	r.right = sb.right - 6 < r.left + 620 ? sb.right - 6 : r.left + 620;
	r.bottom = sb.bottom - 6 < r.top + 420 ? sb.bottom - 6 : r.top + 420;
	win = NewWindow(NULL, &r, mac_pstr(title, "Riptide Terminal"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(monaco);
	TextSize(9);
	r = content();
	{
		Rect s = r;
		s.left = s.right - SBW + 1;
		s.top -= 1;
		s.bottom = s.bottom - SBW + 2;
		s.right += 1;
		sbar = NewControl(win, &s, mac_pstr(title, ""), 1, 0, 0, 0, 16, 0);   /* scrollBarProc */
	}
	r.right -= SBW;
	r.bottom -= SBW;
	InsetRect(&r, 4, 2);
	te = (TEHandle)TENew(&r, &r);
	layout();
	if (!tool_data_copy) {
		append("Not enough memory for the tools.\n", 33);
	} else if (term_api_open() < 0) {
		static const char no[] = "Riptide is not running: the tools need Riptide's .IPP driver\n"
		                         "(Riptide in the Extensions folder, the Mac restarted).\n";
		append(no, sizeof no - 1);
	} else {
		static const char hello[] = "Riptide Terminal. \"help\" lists the tools.\n";
		append(hello, sizeof hello - 1);
	}
	prompt();
	TEActivate(te);
	run_startup();
	while (!quit_asked) {
		if (WaitNextEvent(everyEvent, &ev, 20, NULL))
			event(&ev);
		else
			TEIdle(te);
	}
	term_api_close();
	return 0;
}
