/*
 * Riptide - RiptidePrefs: preferences editor for riptide.conf (GadTools,
 * AmigaOS 2.04+ and AROS; started from Workbench or the CLI).
 *
 *   RiptidePrefs [FROM=file] [EDIT] [USE] [SAVE] [PUBSCREEN=name]
 *
 * Edits ENV:Riptide/riptide.conf (or ENVARC: when ENV: has none, or FROM).
 * "Save" writes ENVARC:Riptide/riptide.conf (the old one becomes
 * riptide.conf.bak) and ENV:, "Use" writes ENV: only, "Apply" sends the
 * changes that a running Riptide can take (host name, name servers, time,
 * sysctls, extra addresses, QoS) to the RIPTIDE port. The "Advanced:"
 * pages hold the connection, background, memory, Happy Eyeballs and QoS
 * settings. USE or SAVE on the command line writes
 * the file without opening the window, like the system prefs editors.
 * conf_edit.c keeps every line it does not understand as it is.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "rt/rt_version.h"
#include "prefs/conf_edit.h"

#if defined(__VBCC__)
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <workbench/startup.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#else
#include "os/intuition.h"
#include "gen/intuition_calls.h"
#include "gen/gadtools_calls.h"
#include "gen/graphics_calls.h"
#endif

const char riptideprefs_version[] = "$VER: RiptidePrefs " RIPTIDE_VERSION_STRING " (" RIPTIDE_DATE ")";

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;

#define ENV_CONF    "ENV:Riptide/riptide.conf"
#define ENVARC_CONF "ENVARC:Riptide/riptide.conf"
#define TITLE       "Riptide Preferences"

void *rp_alloc(size_t n)
{
	return AllocVec((ULONG)n, MEMF_ANY);
}

void rp_free(void *p)
{
	if (p)
		FreeVec(p);
}

/* ---- the settings, page by page ---- */
enum {
	P_IFACE, P_ADDR, P_ALIAS, P_VLAN, P_NAMES, P_TIME, P_TCP, P_CONN, P_PORTS, P_KA, P_BG, P_MEM, P_HE,
	P_QOS, P_QRULE, P_OTHER, NPAGES
};
static const char *const page_names[] = {
	"Interface", "Address", "Extra addresses", "VLANs", "Names", "Time", "TCP", "Advanced: Connecting",
	"Advanced: Ports", "Advanced: Keepalive", "Advanced: Background", "Advanced: Memory",
	"Advanced: Happy Eyeballs", "Advanced: QoS", "Advanced: QoS rules", "Other lines", NULL
};
/* pages that are a list of lines */
static int list_kind(int p)
{
	return p == P_ALIAS ? RP_LIST_ALIAS : p == P_QRULE ? RP_LIST_QRULE : p == P_VLAN ? RP_LIST_VLAN : -1;
}
#define IS_LIST(p) ((p) == P_OTHER || list_kind(p) >= 0)

enum { F_STR, F_NUM, F_CHECK, F_CYCLE };
#define FL_STATIC 1             /* disabled with DHCP */
#define FL_NTP    2             /* disabled without NTP */
#define FL_ZERO   4             /* F_NUM: 0 is allowed below min */
#define FL_RIGHT  8             /* F_CHECK: on the previous row, at the right */
#define FL_DHCP   16            /* disabled with a static address */

struct field {
	UBYTE page, type, flags;
	UWORD off, size;        /* in struct rp_model; size of the string or number */
	long min, max;
	const char *label;      /* '_' marks the keyboard shortcut */
	const char *const *cy;  /* F_CYCLE: labels and values */
	const long *cyv;
};

#define M(f) offsetof(struct rp_model, f)
#define MS(f) M(f), sizeof(((struct rp_model *)0)->f)
static const char *const cy_dhcp[] = { "DHCP", "Static", NULL };
static const long cyv_dhcp[] = { 1, 0 };
static const char *const cy_tri[] = { "Default", "On", "Off", NULL };
static const long cyv_tri[] = { -1, 1, 0 };
static const char *const cy_ack[] = { "Default", "Off", "On", NULL };
static const long cyv_ack[] = { RP_UNSET, 0, 1 };
static const char *const cy_sw[] = { "Default", "On", "Off", NULL };
static const long cyv_sw[] = { RP_UNSET, 1, 0 };
static const char *const cy_backoff[] = { "Default", "Linear", "Doubling", NULL };
static const long cyv_backoff[] = { RP_UNSET, 0, 1 };
static const char *const cy_icmp[] = { "Default", "Keep trying", "2nd report", "At once", NULL };
static const long cyv_icmp[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_mtup[] = { "Default", "Off", "On black hole", "Always", NULL };
static const long cyv_mtup[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_ecn[] = { "Default", "Off", "On", "Accept only", NULL };
static const long cyv_ecn[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_tw[] = { "Default", "Off", "On", "Loopback only", NULL };
static const long cyv_tw[] = { RP_UNSET, 0, 1, 2 };
static const char *const cy_cc[] = { "Default", "CUBIC", "Reno", "LEDBAT", NULL };
static const long cyv_cc[] = { RP_UNSET, 0, 1, 2 };   /* rp_cc_names */
static const char *const cy_bg[] = { "Default", "Off", "Receive", "Send", "Both", NULL };
static const long cyv_bg[] = { RP_UNSET, 0, 1, 2, 3 };
static const char *const cy_prefer[] = { "Default", "RFC 6724", "IPv6", "IPv4", NULL };
static const long cyv_prefer[] = { RP_UNSET, 0, 6, 4 };
#define SC(i) M(sc) + (i) * sizeof(long), sizeof(long)
#define SS(i) M(ss) + (i) * RP_SSLEN, RP_SSLEN

static const struct field fields[] = {
	{ P_IFACE, F_STR,   0, MS(ifname),    0, 0,        "_Interface name", NULL, NULL },
	{ P_IFACE, F_STR,   0, MS(device),    0, 0,        "_Device", NULL, NULL },
	{ P_IFACE, F_NUM,   0, MS(unit),      0, 255,      "U_nit", NULL, NULL },
	{ P_IFACE, F_NUM,   0, MS(mtu),       68, 65535,   "_MTU", NULL, NULL },
	{ P_IFACE, F_NUM,   FL_ZERO, MS(tcpwindow), 536, 16777216, "TCP _window", NULL, NULL },
	{ P_IFACE, F_NUM,   0, MS(reads),     4, 128,      "_Reads", NULL, NULL },
	{ P_IFACE, F_CHECK, 0, MS(ipv4),      0, 0,        "IPv_4", NULL, NULL },
	{ P_IFACE, F_CHECK, FL_RIGHT, MS(ipv6), 0, 0,      "IP_v6", NULL, NULL },
	{ P_ADDR,  F_CYCLE, 0, MS(dhcp),      0, 0,        "_Mode", cy_dhcp, cyv_dhcp },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(addr),    0, 0,  "Add_ress", NULL, NULL },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(netmask), 0, 0,  "_Netmask", NULL, NULL },
	{ P_ADDR,  F_STR,   FL_STATIC, MS(gateway), 0, 0,  "_Gateway", NULL, NULL },
	{ P_ADDR,  F_CHECK, FL_DHCP, MS(linklocal), 0, 0,  "_Link-local if no DHCP", NULL, NULL },
	{ P_NAMES, F_STR,   0, MS(hostname),  0, 0,        "_Host name", NULL, NULL },
	{ P_NAMES, F_STR,   0, MS(domain),    0, 0,        "_Domain", NULL, NULL },
	{ P_NAMES, F_STR,   0, M(dns), RP_ADDR, 0, 0,      "DNS server _1", NULL, NULL },
	{ P_NAMES, F_STR,   0, M(dns) + RP_ADDR, RP_ADDR, 0, 0, "DNS server _2", NULL, NULL },
	{ P_NAMES, F_STR,   0, M(dns) + 2 * RP_ADDR, RP_ADDR, 0, 0, "DNS server _3", NULL, NULL },
	{ P_NAMES, F_NUM,   FL_ZERO, SC(RP_SC_DNS_EDNS), 512, 4096, "_EDNS size (0 = off)", NULL, NULL },
	{ P_NAMES, F_CYCLE, 0, SC(RP_SC_DNS_TCP), 0, 0,    "DNS over _TCP", cy_sw, cyv_sw },
	{ P_TIME,  F_CHECK, 0, MS(ntp),       0, 0,        "_NTP time sync", NULL, NULL },
	{ P_TIME,  F_STR,   FL_NTP, MS(ntp_servers), 0, 0, "NTP ser_vers", NULL, NULL },
	{ P_TIME,  F_STR,   0, MS(timezone),  0, 0,        "_Time zone", NULL, NULL },
	{ P_TIME,  F_CYCLE, 0, MS(mdns),      0, 0,        "_mDNS responder", cy_tri, cyv_tri },
	{ P_TCP,   F_CYCLE, 0, MS(quickack),  0, 0,        "_Quick ACK", cy_ack, cyv_ack },
	{ P_TCP,   F_NUM,   0, MS(delack_segs), 1, 64,     "Delayed ACK se_gments", NULL, NULL },
	{ P_TCP,   F_NUM,   0, MS(delack_ms), 1, 500,      "Delayed ACK _limit (ms)", NULL, NULL },
	{ P_TCP,   F_CYCLE, 0, SC(RP_SC_ECN), 0, 0,        "_ECN", cy_ecn, cyv_ecn },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_FASTOPEN), 0, 0,   "_Fast Open", cy_sw, cyv_sw },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_FASTOPEN_ALL), 0, 0, "Fast Open _defer all", cy_sw, cyv_sw },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_FO_BLACKHOLE), 0, 86400, "Fast Open _backoff (s)", NULL, NULL },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_SYN_RTO_INIT), 200, 3000, "SYN first _RTO (ms)", NULL, NULL },
	{ P_CONN,  F_NUM,   0, SC(RP_SC_SYN_RTO_MAX), 200, 120000, "SYN _max RTO (ms)", NULL, NULL },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_SYN_BACKOFF), 0, 0, "SYN back_off", cy_backoff, cyv_backoff },
	{ P_CONN,  F_CYCLE, 0, SC(RP_SC_MTU_PROBING), 0, 0, "M_TU probing", cy_mtup, cyv_mtup },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_CONNECT_TIMEOUT), 0, 600, "Connect ti_meout (s)", NULL, NULL },
	{ P_PORTS, F_CYCLE, 0, SC(RP_SC_SYN_ICMP_ABORT), 0, 0, "_ICMP unreachable", cy_icmp, cyv_icmp },
	{ P_PORTS, F_CYCLE, 0, SC(RP_SC_TW_REUSE), 0, 0,   "Time-wait _reuse", cy_tw, cyv_tw },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_MAX_TW), 0, 65536, "Time-wait _limit", NULL, NULL },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_PORT_FIRST), 1024, 65535, "Local ports _from", NULL, NULL },
	{ P_PORTS, F_NUM,   0, SC(RP_SC_PORT_LAST), 1024, 65535, "Local ports _to", NULL, NULL },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KA_ALL), 0, 32767, "_Keepalive all (s)", NULL, NULL },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPIDLE), 1, 32767, "_Idle time (s)", NULL, NULL },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPINTVL), 1, 32767, "Probe int_erval (s)", NULL, NULL },
	{ P_KA,    F_NUM,   0, SC(RP_SC_KEEPCNT), 1, 127,  "Probe c_ount", NULL, NULL },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_CC), 0, 0,         "Default _TCP algorithm", cy_cc, cyv_cc },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_BG_MODE), 0, 0,    "Background _mode", cy_bg, cyv_bg },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_PROGRAMS), 0, 0, "Background p_rograms", NULL, NULL },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_PORTS), 0, 0,   "Remote p_orts", NULL, NULL },
	{ P_BG,    F_STR,   0, SS(RP_SS_BG_LPORTS), 0, 0,  "_Local ports", NULL, NULL },
	{ P_BG,    F_NUM,   0, SC(RP_SC_LEDBAT_TARGET), 5, 1000, "LEDBAT tar_get (ms)", NULL, NULL },
	{ P_BG,    F_CYCLE, 0, SC(RP_SC_LEDBAT_SLOWDOWN), 0, 0, "LEDBAT slow_downs", cy_sw, cyv_sw },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_LOW), 16, 4194304, "_Low water (KB)", NULL, NULL },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_PRESSURE), 16, 4194304, "Pr_essure (KB)", NULL, NULL },
	{ P_MEM,   F_NUM,   0, SC(RP_SC_MEM_HIGH), 16, 4194304, "_High water (KB)", NULL, NULL },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_ENABLE), 0, 0,  "_Happy Eyeballs", cy_sw, cyv_sw },
	{ P_HE,    F_NUM,   0, SC(RP_SC_HE_DELAY), 10, 2000, "Attempt _delay (ms)", NULL, NULL },
	{ P_HE,    F_NUM,   0, SC(RP_SC_HE_RESDELAY), 0, 1000, "_Resolution delay (ms)", NULL, NULL },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_PREFER), 0, 0,  "Pre_fer", cy_prefer, cyv_prefer },
	{ P_HE,    F_CYCLE, 0, SC(RP_SC_HE_AAAA), 0, 0,    "_IPv6 lookup filter", cy_sw, cyv_sw },
	{ P_HE,    F_NUM,   0, SC(RP_SC_DNS_NEGTTL), 0, 86400, "DNS _negative cache (s)", NULL, NULL },
	{ P_QOS,   F_CYCLE, 0, MS(qos),       0, 0,        "_QoS bands", cy_tri, cyv_tri },
	{ P_QOS,   F_NUM,   0, SC(RP_SC_QOS_INFLIGHT), 1, 32, "Frames in _flight", NULL, NULL },
	{ P_QOS,   F_NUM,   0, SC(RP_SC_QOS_LIMIT), 8, 1024, "Band _limit (frames)", NULL, NULL },
	{ P_QOS,   F_CYCLE, 0, SC(RP_SC_QOS_PRIODSCP), 0, 0, "Priority sets _DSCP", cy_sw, cyv_sw },
};
#define NFIELDS ((int)(sizeof(fields) / sizeof(fields[0])))

static const char *const page_notes[NPAGES] = {
	"Interface changes need a restart of Riptide",
	"Address changes need a restart of Riptide",
	"A.B.C.D/BITS or A.B.C.D netmask M (up to 8)",
	"ID dhcp|A.B.C.D/BITS|up [pcp=0-7] (up to 8)",
	"Empty fields are left out of the file",
	"Zone: auto or +10:00. No servers: from DHCP",
	"Empty or Default: Riptide's built-in value",
	"Empty or Default: Riptide's built-in value",
	"Empty or Default: Riptide's built-in value",
	"Keepalive all: idle seconds, 0 = off",
	"Programs a,b*  Ports 20,21,6881-6889",
	"Empty: from the free memory at start",
	"Empty or Default: Riptide's built-in value",
	"QoS bands: for slow links (off by default)",
	"port=22,23 priority=interactive dscp=EF",
	"Lines RiptidePrefs does not edit itself",
};

/* gadget IDs: fields are 100 + index */
enum { G_PAGE = 1, G_SAVE, G_USE, G_APPLY, G_CANCEL, G_LIST, G_LINE, G_ADD, G_REPL, G_DEL, G_NOTE };
#define G_FIELD 100

/* ---- state ---- */
static struct rp_conf conf;
static const char *from_path;
static struct Screen *scr;
static APTR vi;
static struct Window *win;
static struct Gadget *glist, *fgad[NFIELDS], *g_list, *g_line, *g_page;
static struct List olist;
static struct Node *onodes;
static int page, osel = -1;
static char linebuf[RP_LONG * 2];
static char numtext[NFIELDS][12];
static int wb;                  /* started from Workbench: no CLI output */
static int stale;               /* gadgets rebuilt: queued gadget messages are old */

/* layout (font sensitive) */
static int fh, cw, lw, fw, rowh, ystep, x0, y0, body_y, btn_y, inner_w, inner_h;
#define NROWS 8

static long getnum(const struct field *f)
{
	void *p = (char *)&conf.cur + f->off;
	return f->size == sizeof(long) ? *(long *)p : (long)*(int *)p;
}

static void setnum(const struct field *f, long v)
{
	void *p = (char *)&conf.cur + f->off;
	if (f->size == sizeof(long))
		*(long *)p = v;
	else
		*(int *)p = (int)v;
}

static char *fstr(const struct field *f)
{
	return (char *)&conf.cur + f->off;
}

static char fkey(const char *label)
{
	const char *u = strchr(label, '_');
	char ch = u ? u[1] : 0;
	return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
}

static int field_disabled(const struct field *f)
{
	return ((f->flags & FL_STATIC) && conf.cur.dhcp) || ((f->flags & FL_DHCP) && !conf.cur.dhcp) ||
	       ((f->flags & FL_NTP) && !conf.cur.ntp);
}

static const char *gstr(struct Gadget *g)
{
	/* GadTools string gadget: StringInfo, whose first field is Buffer */
	return *(const char **)g->SpecialInfo;
}

/* ---- messages ---- */
static LONG request(const char *text, const char *buttons)
{
	struct EasyStruct es;
	const char *arg = text;

	if (!IntuitionBase) {
		if (!wb) {
			PutStr((CONST_STRPTR)text);
			PutStr((CONST_STRPTR)"\n");
		}
		return 0;
	}
	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (CONST_STRPTR)TITLE;
	es.es_TextFormat = (CONST_STRPTR)"%s";
	es.es_GadgetFormat = (CONST_STRPTR)buttons;
	return EasyRequestArgs(win, &es, NULL, (APTR)&arg);
}

static void message(const char *text)
{
	request(text, "OK");
}

/* ---- reading and writing files ---- */
static char *read_file(const char *path, LONG *len)
{
	BPTR fh_, lock;
	struct FileInfoBlock *fib;
	char *buf = NULL;
	LONG size = -1;

	lock = Lock((CONST_STRPTR)path, ACCESS_READ);
	if (!lock)
		return NULL;
	fib = AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
	if (fib && Examine(lock, fib))
		size = fib->fib_Size;
	if (fib)
		FreeVec(fib);
	UnLock(lock);
	if (size < 0)
		return NULL;
	buf = AllocVec((ULONG)size + 1, MEMF_ANY);
	if (!buf)
		return NULL;
	fh_ = Open((CONST_STRPTR)path, MODE_OLDFILE);
	if (!fh_ || Read(fh_, buf, size) != size) {
		if (fh_)
			Close(fh_);
		FreeVec(buf);
		return NULL;
	}
	Close(fh_);
	buf[size] = 0;
	*len = size;
	return buf;
}

/* path = "VOL:Dir/file": create VOL:Dir when missing */
static void make_dir(const char *path)
{
	char dir[128];
	const char *slash = strrchr(path, '/');
	BPTR l;

	if (!slash || (size_t)(slash - path) >= sizeof(dir))
		return;
	memcpy(dir, path, (size_t)(slash - path));
	dir[slash - path] = 0;
	l = Lock((CONST_STRPTR)dir, ACCESS_READ);
	if (!l)
		l = CreateDir((CONST_STRPTR)dir);
	if (l)
		UnLock(l);
}

static int write_file(const char *path, const char *text, size_t len, int backup)
{
	char bak[140];
	BPTR f;
	LONG n;

	make_dir(path);
	if (backup) {
		BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
		if (l) {
			UnLock(l);
			rt_snprintf(bak, sizeof(bak), "%s.bak", path);
			DeleteFile((CONST_STRPTR)bak);
			Rename((CONST_STRPTR)path, (CONST_STRPTR)bak);
		}
	}
	f = Open((CONST_STRPTR)path, MODE_NEWFILE);
	if (!f)
		return -1;
	n = len ? Write(f, (APTR)text, (LONG)len) : 0;
	Close(f);
	return n == (LONG)len ? 0 : -1;
}

static int load(void)
{
	char *text;
	LONG len = 0;
	int r;

	if (from_path)
		text = read_file(from_path, &len);
	else if (!(text = read_file(ENV_CONF, &len)))
		text = read_file(ENVARC_CONF, &len);
	if (!text && from_path) {
		char msg[200];
		rt_snprintf(msg, sizeof(msg), "Cannot read %s", from_path);
		message(msg);
		return -1;
	}
	r = rp_conf_load(&conf, text ? text : "", text ? (size_t)len : 0);
	if (text)
		FreeVec(text);
	return r;
}

/* save = 1: ENVARC: (with backup) and ENV:, 0: ENV: only */
static int store(int save)
{
	size_t len;
	char *text = rp_conf_text(&conf, &len);
	char msg[160];
	int r = 0;

	if (!text) {
		message("Out of memory");
		return -1;
	}
	if (save && write_file(ENVARC_CONF, text, len, 1) < 0) {
		rt_snprintf(msg, sizeof(msg), "Cannot write %s", ENVARC_CONF);
		r = -1;
	} else if (write_file(ENV_CONF, text, len, 0) < 0) {
		rt_snprintf(msg, sizeof(msg), "Cannot write %s", ENV_CONF);
		r = -1;
	}
	rp_free(text);
	if (r < 0)
		message(msg);
	return r;
}

/* ---- validation ---- */
static int show_page(int p);

static int bad(int fi, const char *why)
{
	char msg[200], label[40];
	const char *s = fields[fi].label;
	int i = 0;

	for (; *s && i < (int)sizeof(label) - 1; s++)
		if (*s != '_')
			label[i++] = *s;
	label[i] = 0;
	rt_snprintf(msg, sizeof(msg), "%s: %s", label, why);
	if (win && fields[fi].page != page)
		show_page(fields[fi].page);
	message(msg);
	if (win && fgad[fi] && (fields[fi].type == F_STR || fields[fi].type == F_NUM))
		ActivateGadget(fgad[fi], win, NULL);
	return -1;
}

static int find_field(void *addr)
{
	int i;
	for (i = 0; i < NFIELDS; i++)
		if ((char *)&conf.cur + fields[i].off == (char *)addr)
			return i;
	return 0;
}

static int all_names(const char *s)
{
	char tok[RP_STR];
	int n;

	while (*s) {
		while (*s == ' ')
			s++;
		for (n = 0; *s && *s != ' '; s++)
			if (n < (int)sizeof(tok) - 1)
				tok[n++] = *s;
		tok[n] = 0;
		if (n && !rp_valid_name(tok) && !rp_valid_ip(tok))
			return 0;
	}
	return 1;
}

static int validate(void)
{
	struct rp_model *m = &conf.cur;
	int i;

	if (!m->ifname[0] || strchr(m->ifname, ' '))
		return bad(find_field(m->ifname), "enter a name without spaces, e.g. eth0");
	if (!m->device[0])
		return bad(find_field(m->device), "enter the SANA-II device, e.g. uaenet.device");
	if (!m->dhcp) {
		if (!rp_valid_ip4(m->addr, 1))
			return bad(find_field(m->addr), "enter an IPv4 address, e.g. 192.168.1.20 or 192.168.1.20/24");
		if (m->netmask[0] && !rp_valid_ip4(m->netmask, 0))
			return bad(find_field(m->netmask), "enter a mask, e.g. 255.255.255.0");
		if (m->gateway[0] && !rp_valid_ip4(m->gateway, 0))
			return bad(find_field(m->gateway), "enter the router's IPv4 address");
	}
	if (m->hostname[0] && !rp_valid_name(m->hostname))
		return bad(find_field(m->hostname), "letters, digits, '-' and '.' only");
	if (m->domain[0] && !rp_valid_name(m->domain))
		return bad(find_field(m->domain), "letters, digits, '-' and '.' only");
	for (i = 0; i < RP_NDNS; i++)
		if (m->dns[i][0] && !rp_valid_ip(m->dns[i]))
			return bad(find_field(m->dns[i]), "enter an IPv4 or IPv6 address");
	if (m->ntp && m->ntp_servers[0] && !all_names(m->ntp_servers))
		return bad(find_field(m->ntp_servers), "host names or addresses, separated by spaces");
	if (m->timezone[0] && !rp_valid_tz(m->timezone))
		return bad(find_field(m->timezone), "auto, or the offset from UTC: +10:00, -5, +5:30");
	if (!rp_valid_programs(m->ss[RP_SS_BG_PROGRAMS]))
		return bad(find_field(m->ss[RP_SS_BG_PROGRAMS]), "names separated by commas, e.g. AmiFTP,wget*");
	for (i = RP_SS_BG_PORTS; i <= RP_SS_BG_LPORTS; i++)
		if (!rp_valid_ports(m->ss[i]))
			return bad(find_field(m->ss[i]), "ports or ranges separated by commas, e.g. 20,21,6881-6889");
	if (m->sc[RP_SC_PORT_FIRST] != RP_UNSET && m->sc[RP_SC_PORT_LAST] != RP_UNSET &&
	    m->sc[RP_SC_PORT_FIRST] > m->sc[RP_SC_PORT_LAST])
		return bad(find_field(&m->sc[RP_SC_PORT_LAST]), "the last port must not be below the first");
	if (m->sc[RP_SC_SYN_RTO_INIT] != RP_UNSET && m->sc[RP_SC_SYN_RTO_MAX] != RP_UNSET &&
	    m->sc[RP_SC_SYN_RTO_INIT] > m->sc[RP_SC_SYN_RTO_MAX])
		return bad(find_field(&m->sc[RP_SC_SYN_RTO_MAX]), "must not be below the first RTO");
	if ((m->sc[RP_SC_MEM_LOW] != RP_UNSET && m->sc[RP_SC_MEM_PRESSURE] != RP_UNSET &&
	     m->sc[RP_SC_MEM_LOW] > m->sc[RP_SC_MEM_PRESSURE]) ||
	    (m->sc[RP_SC_MEM_PRESSURE] != RP_UNSET && m->sc[RP_SC_MEM_HIGH] != RP_UNSET &&
	     m->sc[RP_SC_MEM_PRESSURE] > m->sc[RP_SC_MEM_HIGH]))
		return bad(find_field(&m->sc[RP_SC_MEM_PRESSURE]), "low <= pressure <= high water");
	return 0;
}

/* ---- gadgets ---- */
static void free_nodes(void)
{
	if (onodes)
		FreeVec(onodes);
	onodes = NULL;
}

/* the list of a list page */
static int list_count(void)
{
	return page == P_OTHER ? rp_other_count(&conf) : rp_list_count(&conf, list_kind(page));
}

static const char *list_get(int i)
{
	return page == P_OTHER ? rp_other_get(&conf, i) : rp_list_get(&conf, list_kind(page), i);
}

static void build_list(void)
{
	int i, n = list_count();

	olist.lh_Head = (struct Node *)&olist.lh_Tail;
	olist.lh_Tail = NULL;
	olist.lh_TailPred = (struct Node *)&olist.lh_Head;
	free_nodes();
	if (n <= 0 || !(onodes = AllocVec(sizeof(struct Node) * (ULONG)n, MEMF_ANY | MEMF_CLEAR)))
		return;
	for (i = 0; i < n; i++) {
		onodes[i].ln_Name = (char *)list_get(i);
		AddTail(&olist, &onodes[i]);
	}
}

/* pixel width of a label without its '_' */
static int textw(const char *s)
{
	char t[64];
	int i = 0;

	for (; *s && i < (int)sizeof(t) - 1; s++)
		if (*s != '_')
			t[i++] = *s;
	t[i] = 0;
	return TextLength(&scr->RastPort, (CONST_STRPTR)t, (ULONG)i);
}

static void layout(void)
{
	int i, w;
	static const char *const btns[] = { "_Save", "_Use", "_Apply", "_Cancel" };

	fh = scr->Font->ta_YSize;
	cw = (TextLength(&scr->RastPort, (CONST_STRPTR)"abcdefghijklmnopqrstuvwxyz", 26) + 25) / 26;
	rowh = fh + 6;
	ystep = rowh + 3;
	lw = textw("_Page");
	for (i = 0; i < NFIELDS; i++)
		if ((w = textw(fields[i].label)) > lw)
			lw = w;
	lw += 2 * cw;
	fw = 30 * cw;
	x0 = scr->WBorLeft + 8;
	y0 = scr->WBorTop + fh + 1 + 6;
	body_y = y0 + rowh + 8;
	btn_y = body_y + NROWS * ystep + 4;
	inner_w = 8 + lw + fw + 8;
	w = 0;
	for (i = 0; i < 4; i++)
		if (textw(btns[i]) > w)
			w = textw(btns[i]);
	w += 4 * cw;
	if (inner_w < 4 * w + 5 * 8)
		inner_w = 4 * w + 5 * 8;
	inner_h = btn_y + rowh + 6 - (scr->WBorTop + fh + 1);
}

static struct Gadget *mk(ULONG kind, struct Gadget *prev, int id, const char *label, ULONG flags,
                         int x, int y, int w, int h, struct TagItem *tags)
{
	struct NewGadget ng;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = (WORD)x;
	ng.ng_TopEdge = (WORD)y;
	ng.ng_Width = (WORD)w;
	ng.ng_Height = (WORD)h;
	ng.ng_GadgetText = (CONST_STRPTR)label;
	ng.ng_TextAttr = scr->Font;
	ng.ng_GadgetID = (UWORD)id;
	ng.ng_Flags = flags;
	ng.ng_VisualInfo = vi;
	return prev ? CreateGadgetA(kind, prev, &ng, tags) : NULL;
}

static struct Gadget *mk_field(struct Gadget *g, int i, int y)
{
	const struct field *f = &fields[i];
	struct TagItem t[5];
	int n = 0, x = (f->flags & FL_RIGHT) ? x0 + lw + fw - 26 : x0 + lw, h = rowh;
	ULONG kind = STRING_KIND;

	t[n].ti_Tag = GT_Underscore;
	t[n++].ti_Data = '_';
	t[n].ti_Tag = GA_Disabled;
	t[n++].ti_Data = field_disabled(f);
	switch (f->type) {
	case F_STR:
		t[n].ti_Tag = GTST_String;
		t[n++].ti_Data = (ULONG)fstr(f);
		t[n].ti_Tag = GTST_MaxChars;
		t[n++].ti_Data = f->size - 1;
		break;
	case F_NUM:
		if (getnum(f) == RP_UNSET)
			numtext[i][0] = 0;
		else
			rt_snprintf(numtext[i], sizeof(numtext[i]), "%ld", getnum(f));
		t[n].ti_Tag = GTST_String;
		t[n++].ti_Data = (ULONG)numtext[i];
		t[n].ti_Tag = GTST_MaxChars;
		t[n++].ti_Data = 9;
		break;
	case F_CHECK:
		kind = CHECKBOX_KIND;
		t[n].ti_Tag = GTCB_Checked;
		t[n++].ti_Data = getnum(f) != 0;
		h = 11;
		y += (rowh - 11) / 2;
		break;
	case F_CYCLE: {
		int a;
		for (a = 0; f->cy[a] && f->cyv[a] != getnum(f); a++)
			;
		kind = CYCLE_KIND;
		t[n].ti_Tag = GTCY_Labels;
		t[n++].ti_Data = (ULONG)f->cy;
		t[n].ti_Tag = GTCY_Active;
		t[n++].ti_Data = f->cy[a] ? (ULONG)a : 0;
		break;
	}
	}
	t[n].ti_Tag = TAG_DONE;
	t[n].ti_Data = 0;
	g = mk(kind, g, G_FIELD + i, f->label, PLACETEXT_LEFT, x,
	       y, kind == CHECKBOX_KIND ? 26 : kind == CYCLE_KIND ? fw / 2 : fw, h, t);
	fgad[i] = g;
	return g;
}

static void clear_body(void)
{
	SetAPen(win->RPort, 0);
	RectFill(win->RPort, win->BorderLeft, win->BorderTop, win->Width - win->BorderRight - 1,
	         win->Height - win->BorderBottom - 1);
}

static int build(void)
{
	struct TagItem t[6];
	struct Gadget *g;
	int i, y = body_y, bw, gap;
	static const char *const btn[4] = { "_Save", "_Use", "_Apply", "_Cancel" };

	if (win && glist)
		RemoveGList(win, glist, -1);
	FreeGadgets(glist);
	glist = NULL;
	g_list = g_line = NULL;
	memset(fgad, 0, sizeof(fgad));
	free_nodes();

	g = CreateContext(&glist);
	t[0].ti_Tag = GTCY_Labels;
	t[0].ti_Data = (ULONG)page_names;
	t[1].ti_Tag = GTCY_Active;
	t[1].ti_Data = (ULONG)page;
	t[2].ti_Tag = GT_Underscore;
	t[2].ti_Data = '_';
	t[3].ti_Tag = TAG_DONE;
	g = g_page = mk(CYCLE_KIND, g, G_PAGE, "_Page", PLACETEXT_LEFT, x0 + lw, y0, fw, rowh, t);

	if (IS_LIST(page)) {
		int lh = 5 * ystep - 8;
		build_list();
		t[0].ti_Tag = GTLV_Labels;
		t[0].ti_Data = (ULONG)&olist;
		t[1].ti_Tag = GTLV_ShowSelected;
		t[1].ti_Data = 0;
		t[2].ti_Tag = GTLV_Selected;
		t[2].ti_Data = osel >= 0 ? (ULONG)osel : ~0UL;
		t[3].ti_Tag = GTLV_Top;
		t[3].ti_Data = osel >= 0 ? (ULONG)osel : 0;
		t[4].ti_Tag = TAG_DONE;
		g = g_list = mk(LISTVIEW_KIND, g, G_LIST, NULL, 0, x0, y, lw + fw, lh, t);
		y += lh + 4;
		t[0].ti_Tag = GTST_String;
		t[0].ti_Data = (ULONG)linebuf;
		t[1].ti_Tag = GTST_MaxChars;
		t[1].ti_Data = sizeof(linebuf) - 1;
		t[2].ti_Tag = GT_Underscore;
		t[2].ti_Data = '_';
		t[3].ti_Tag = TAG_DONE;
		g = g_line = mk(STRING_KIND, g, G_LINE, "_Line", PLACETEXT_LEFT, x0 + textw("_Line") + 2 * cw, y,
		                lw + fw - textw("_Line") - 2 * cw, rowh, t);
		y += ystep;
		bw = (lw + fw - 16) / 3;
		t[0].ti_Tag = GT_Underscore;
		t[0].ti_Data = '_';
		t[1].ti_Tag = GA_Disabled;
		t[1].ti_Data = 0;
		t[2].ti_Tag = TAG_DONE;
		g = mk(BUTTON_KIND, g, G_ADD, "A_dd", PLACETEXT_IN, x0, y, bw, rowh, t);
		t[1].ti_Data = osel < 0;
		g = mk(BUTTON_KIND, g, G_REPL, "_Replace", PLACETEXT_IN, x0 + bw + 8, y, bw, rowh, t);
		g = mk(BUTTON_KIND, g, G_DEL, "D_elete", PLACETEXT_IN, x0 + 2 * (bw + 8), y, bw, rowh, t);
	} else {
		for (i = 0; i < NFIELDS; i++)
			if (fields[i].page == page) {
				if (fields[i].flags & FL_RIGHT)
					y -= ystep;
				g = mk_field(g, i, y);
				y += ystep;
			}
	}
	t[0].ti_Tag = GTTX_Text;
	t[0].ti_Data = (ULONG)page_notes[page];
	t[1].ti_Tag = TAG_DONE;
	g = mk(TEXT_KIND, g, G_NOTE, NULL, 0, x0, body_y + (NROWS - 1) * ystep, lw + fw, rowh, t);

	gap = 8;
	bw = (inner_w - 5 * gap) / 4;
	t[0].ti_Tag = GT_Underscore;
	t[0].ti_Data = '_';
	t[1].ti_Tag = TAG_DONE;
	for (i = 0; i < 4; i++)
		g = mk(BUTTON_KIND, g, G_SAVE + i, btn[i], PLACETEXT_IN,
		       scr->WBorLeft + gap + i * (bw + gap), btn_y, bw, rowh, t);
	if (!g)
		return -1;
	if (win) {
		stale = 1;
		clear_body();
		AddGList(win, glist, (ULONG)~0, -1, NULL);
		RefreshGList(glist, win, NULL, -1);
		GT_RefreshWindow(win, NULL);
	}
	return 0;
}

/* Take the text of the string gadgets of the page into the model.
 * Returns -1 (message shown) on a bad number. */
static int harvest(void)
{
	int i;

	if (IS_LIST(page)) {
		if (g_line)
			rt_strlcpy(linebuf, gstr(g_line), sizeof(linebuf));
		return 0;
	}
	for (i = 0; i < NFIELDS; i++) {
		const struct field *f = &fields[i];
		const char *s, *end;
		unsigned long v;

		if (!fgad[i])
			continue;
		if (f->type == F_STR) {
			s = gstr(fgad[i]);
			while (*s == ' ')
				s++;
			rt_strlcpy(fstr(f), s, f->size);
			s = fstr(f) + strlen(fstr(f));
			while (s > fstr(f) && s[-1] == ' ')
				*(char *)--s = 0;
		} else if (f->type == F_NUM) {
			char msg[80];
			s = gstr(fgad[i]);
			while (*s == ' ')
				s++;
			if (!*s) {
				setnum(f, RP_UNSET);
				continue;
			}
			v = rt_strtoul(s, &end, 10);
			while (*end == ' ')
				end++;
			if (end == s || *end || (((long)v < f->min || (long)v > f->max) && !(v == 0 && (f->flags & FL_ZERO)))) {
				if (f->flags & FL_ZERO)
					rt_snprintf(msg, sizeof(msg), "enter 0 or %ld..%ld, or leave it empty", f->min, f->max);
				else
					rt_snprintf(msg, sizeof(msg), "enter %ld..%ld, or leave it empty", f->min, f->max);
				bad(i, msg);
				return -1;
			}
			setnum(f, (long)v);
		}
	}
	return 0;
}

static int show_page(int p)
{
	if (p != page) {
		osel = -1;
		linebuf[0] = 0;
	}
	page = p;
	return build();
}

/* ---- apply to the running stack ---- */
struct apply_state {
	int n, failed;
	char report[400];
};

static void apply_one(const char *line, void *arg)
{
	struct apply_state *st = arg;
	size_t l = strlen(st->report);
	int r = cl_command(line, 0);

	st->n++;
	if (r) {
		const char *rep = cl_reply();
		st->failed++;
		if (l < sizeof(st->report) - 2)
			rt_snprintf(st->report + l, sizeof(st->report) - l, "%s: %s\n", line,
			            rep && rep[0] ? rep : "failed");
	}
}

static void apply(int ask)
{
	struct apply_state st;
	char msg[600];
	int restart;

	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		if (!ask)
			message("Riptide is not running.\nThe settings take effect when it starts.");
		return;
	}
	if (ask && !rp_conf_changed(&conf))
		return;
	if (ask && !request("Riptide is running.\nApply the changes to it now?", "Apply|Later"))
		return;
	memset(&st, 0, sizeof(st));
	restart = rp_live_commands(&conf, apply_one, &st);
	if (st.failed)
		rt_snprintf(msg, sizeof(msg), "Applied %d of %d settings.\n%s", st.n - st.failed, st.n, st.report);
	else if (st.n)
		rt_snprintf(msg, sizeof(msg), "Applied %d settings to Riptide.", st.n);
	else
		rt_snprintf(msg, sizeof(msg), "Nothing to apply.");
	if (restart)
		rt_strlcpy(msg + strlen(msg),
		           "\nInterface, address and IPv4/IPv6 changes, and\nsettings set back to Default, take effect\n"
		           "when Riptide is restarted.",
		           sizeof(msg) - strlen(msg));
	message(msg);
}

/* ---- the window ---- */
static void toggle(int i, int dir)
{
	const struct field *f = &fields[i];

	if (f->type == F_CHECK) {
		setnum(f, !getnum(f));
	} else if (f->type == F_CYCLE) {
		int a, n;
		for (n = 0; f->cy[n]; n++)
			;
		for (a = 0; a < n && f->cyv[a] != getnum(f); a++)
			;
		if (a >= n)
			a = 0;
		a = (a + n + dir) % n;
		setnum(f, f->cyv[a]);
	}
}

/* 1: done (saved, used or cancelled) */
static int finish(int what)
{
	if (what == G_CANCEL)
		return 1;
	if (harvest() < 0 || validate() < 0)
		return 0;
	if (what == G_APPLY) {
		apply(0);
		return 0;
	}
	if (store(what == G_SAVE) < 0)
		return 0;
	apply(1);
	return 1;
}

/* the entry of an address or QoS rule list page is valid (or a message) */
static int list_valid(const char *s)
{
	if (page == P_ALIAS && !rp_valid_alias(s)) {
		message("Extra address: enter A.B.C.D/BITS, e.g. 10.0.0.5/24,\nor A.B.C.D netmask M");
		return 0;
	}
	if (page == P_VLAN && !rp_valid_vlan(s)) {
		message("VLAN: the id (1-4094), then dhcp, an address\nA.B.C.D/BITS or up, and pcp=0-7 or pcp=priority\n"
		        "if the frames need a priority; e.g. 10 dhcp");
		return 0;
	}
	if (page == P_QRULE && !rp_valid_qrule(s)) {
		message("QoS rule: port=, lport=, rport= or program=, and\npriority=interactive|default|bulk|0-7 "
		        "or dscp=EF|CS1|AF41|0-63\n(proto=tcp|udp optional; at most 8 ports)");
		return 0;
	}
	return 1;
}

static int other_edit(int id)
{
	int k = list_kind(page);

	harvest();
	if ((id == G_ADD || id == G_REPL) && linebuf[0] && !list_valid(linebuf)) {
		ActivateGadget(g_line, win, NULL);
		return 0;
	}
	if (id == G_ADD && linebuf[0]) {
		if (page == P_OTHER) {
			rp_other_add(&conf, linebuf);
		} else if (rp_list_add(&conf, k, linebuf) < 0) {
			message(k == RP_LIST_ALIAS ? "At most 8 extra addresses" :
			        k == RP_LIST_VLAN ? "At most 8 VLANs" : "At most 16 QoS rules");
			return 0;
		}
		osel = list_count() - 1;
	} else if (id == G_REPL && osel >= 0 && linebuf[0]) {
		if (page == P_OTHER)
			rp_other_set(&conf, osel, linebuf);
		else
			rp_list_set(&conf, k, osel, linebuf);
	} else if (id == G_DEL && osel >= 0) {
		if (page == P_OTHER)
			rp_other_set(&conf, osel, "");
		else
			rp_list_set(&conf, k, osel, "");
		osel = -1;
		linebuf[0] = 0;
	} else {
		return 0;
	}
	return build();
}

static int event_loop(void)
{
	for (;;) {
		struct IntuiMessage *im;
		ULONG cls;
		UWORD code;
		struct Gadget *gad;

		WaitPort(win->UserPort);
		stale = 0;
		while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
			int id, i;
			cls = im->Class;
			code = im->Code;
			gad = (struct Gadget *)im->IAddress;
			GT_ReplyIMsg(im);
			switch (cls) {
			case IDCMP_REFRESHWINDOW:
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
				break;
			case IDCMP_CLOSEWINDOW:
				return 0;
			case IDCMP_GADGETUP:
				/* IAddress of a message queued before a rebuild points
				 * to a freed gadget */
				if (stale)
					break;
				id = gad->GadgetID;
				if (id == G_PAGE) {
					if (harvest() < 0)
						build();
					else
						show_page(code);
				} else if (id >= G_SAVE && id <= G_CANCEL) {
					if (finish(id))
						return 0;
				} else if (id == G_LIST) {
					osel = code;
					rt_strlcpy(linebuf, list_get(osel) ? list_get(osel) : "", sizeof(linebuf));
					build();
				} else if (id >= G_ADD && id <= G_DEL) {
					other_edit(id);
				} else if (id >= G_FIELD && id < G_FIELD + NFIELDS) {
					const struct field *f = &fields[id - G_FIELD];
					if (f->type == F_CHECK || f->type == F_CYCLE) {
						if (f->type == F_CHECK)
							setnum(f, (gad->Flags & GFLG_SELECTED) != 0);
						else
							setnum(f, f->cyv[code]);
						if (harvest() == 0)
							build();
					}
				}
				break;
			case IDCMP_VANILLAKEY: {
				char k = (char)(code >= 'A' && code <= 'Z' ? code + 32 : code);
				int shift = code >= 'A' && code <= 'Z';
				if (IS_LIST(page)) {
					if (k == 'l') {
						ActivateGadget(g_line, win, NULL);
						break;
					}
					if (k == 'd' || k == 'r' || k == 'e') {
						other_edit(k == 'd' ? G_ADD : k == 'r' ? G_REPL : G_DEL);
						break;
					}
				}
				for (i = 0; i < NFIELDS; i++)
					if (fgad[i] && fkey(fields[i].label) == k)
						break;
				if (i < NFIELDS) {
					if (field_disabled(&fields[i]))
						break;
					if (fields[i].type == F_STR || fields[i].type == F_NUM) {
						ActivateGadget(fgad[i], win, NULL);
					} else if (harvest() == 0) {
						toggle(i, shift ? -1 : 1);
						build();
					}
				} else if (k == 'p') {
					if (harvest() == 0)
						show_page((page + (shift ? NPAGES - 1 : 1)) % NPAGES);
				} else if (k == 's' || k == 'u' || k == 'a' || k == 'c' || k == 27) {
					if (finish(k == 's' ? G_SAVE : k == 'u' ? G_USE : k == 'a' ? G_APPLY : G_CANCEL))
						return 0;
				}
				break;
			}
			}
		}
	}
}

static int run_gui(const char *pubscreen)
{
	struct TagItem t[16];
	int n = 0, r = CL_FAIL;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
	GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
	if (!IntuitionBase || !GfxBase || !GadToolsBase)
		goto out;
	scr = LockPubScreen((CONST_STRPTR)pubscreen);
	if (!scr && pubscreen)
		scr = LockPubScreen(NULL);
	if (!scr || !(vi = GetVisualInfoA(scr, NULL)))
		goto out;
	layout();
	if (build() < 0) {
		message("Cannot create the gadgets");
		goto out;
	}
#define TAG(a, b) (t[n].ti_Tag = (a), t[n++].ti_Data = (ULONG)(b))
	TAG(WA_Title, TITLE);
	TAG(WA_PubScreen, scr);
	TAG(WA_Left, (scr->Width - inner_w) / 2 > 0 ? (scr->Width - inner_w) / 2 : 0);
	TAG(WA_Top, scr->BarHeight + 1);
	TAG(WA_InnerWidth, inner_w);
	TAG(WA_InnerHeight, inner_h);
	TAG(WA_Gadgets, glist);
	TAG(WA_IDCMP, BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | CHECKBOXIDCMP | LISTVIEWIDCMP |
	    IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY);
	TAG(WA_DragBar, TRUE);
	TAG(WA_DepthGadget, TRUE);
	TAG(WA_CloseGadget, TRUE);
	TAG(WA_Activate, TRUE);
	TAG(WA_SimpleRefresh, TRUE);
	TAG(WA_AutoAdjust, TRUE);
	TAG(TAG_DONE, 0);
#undef TAG
	win = OpenWindowTagList(NULL, t);
	if (!win) {
		message("Cannot open the window");
		goto out;
	}
	GT_RefreshWindow(win, NULL);
	r = event_loop();
out:
	if (win) {
		CloseWindow(win);
		win = NULL;
	}
	FreeGadgets(glist);
	glist = NULL;
	free_nodes();
	if (vi)
		FreeVisualInfo(vi);
	if (scr)
		UnlockPubScreen(NULL, scr);
	if (GadToolsBase)
		CloseLibrary(GadToolsBase);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	IntuitionBase = NULL;
	return r;
}

/* CLI: FROM,EDIT/S,USE/S,SAVE/S,PUBSCREEN/K */
static int prefs_main(int from_cli)
{
	LONG args[5] = { 0, 0, 0, 0, 0 };
	struct RDArgs *rda = NULL;
	int rc = CL_OK;

	if (from_cli) {
		rda = (struct RDArgs *)ReadArgs((CONST_STRPTR)"FROM,EDIT/S,USE/S,SAVE/S,PUBSCREEN/K", args, NULL);
		if (!rda) {
			PrintFault(IoErr(), (CONST_STRPTR)"RiptidePrefs");
			return CL_ERROR;
		}
		from_path = (const char *)args[0];
	}
	if (load() < 0) {
		rc = CL_ERROR;
	} else if (args[2] || args[3]) {
		rc = store(args[3] != 0) < 0 ? CL_ERROR : CL_OK;
	} else {
		rc = run_gui((const char *)args[4]);
	}
	rp_conf_free(&conf);
	if (rda)
		FreeArgs(rda);
	return rc;
}

#if defined(__VBCC__)
int main(int argc, char **argv)
{
	wb = argc == 0;
	return prefs_main(!wb);
}
#else
long amiga_main(char *s, long l)
{
	struct Process *me;
	struct Message *wbmsg = NULL;
	int rc;

	(void)s;
	(void)l;
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	me = (struct Process *)FindTask(NULL);
	if (!me->pr_CLI) {
		WaitPort(&me->pr_MsgPort);
		wbmsg = GetMsg(&me->pr_MsgPort);
		wb = 1;
	}
	rc = DOSBase ? prefs_main(!wb) : CL_FAIL;
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);
	if (wbmsg) {
		Forbid();
		ReplyMsg(wbmsg);
	}
	return rc;
}
#endif
