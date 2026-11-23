/* src/mac/main_mac.c - Riptide on the classic Mac OS: the .IPP driver's Open, Control and Status
 * (drvr.S calls them with the parameter block and the DCE), and the start of the stack.
 *
 * Open (the INIT at startup, or the first application's OpenDriver): the OS layer and its memory
 * pool, the protocol core, then riptide.conf: the one RiptidePrefs saves in the Preferences folder,
 * else the 'TEXT' 128 resource of the Riptide file, else DHCP on every card. Its "interface" lines
 * open the cards on .ENET (without one every card is opened: en0, en1 ... in the Slot Manager's
 * order). The stack then runs from its deferred task.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include <stdarg.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "riptide.h"
#include "mac.h"
#include "ipp.h"
#include "standin.h"
#include "rt_netif.h"
#include "rt_ip6.h"
#include "rt_qos.h"
#include "rt_netsvc.h"
#include "rt_timer.h"

#define HEAP_BYTES (1024u * 1024u)
#define ipBadLapErr  -23000
#define ipBadCnfgErr -23001

static int core_up, net_up;

static const char *kv(const char *a, const char *key)
{
	u32 n = (u32)strlen(key);
	return !rt_strncasecmp(a, key, n) && a[n] == '=' ? a + n + 1 : NULL;
}

/* "interface NAME device=.ENET [unit=N] [slot=S] [id=N] [card=ethernet|wifi|NAME] [metric=N]
 * [mtu=N] [tcpwindow=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off] [reads=N] [disabled]": a card (the
 * Amiga's form of the line, which RiptidePrefs writes; device= is the Mac's .ENET, and reads= is the
 * Amiga's and does nothing here). Which card: unit=N is the Nth Ethernet card the Slot Manager
 * lists; slot= and id= name one by its slot and sResource id (Aurora68's slot-9 card has its wired
 * port at id=0x81 and its WiFi at id=0x83); card= by its kind or part of its driver's name. */
struct cardline {
	const char *dev;
	struct mac_cardsel sel;
	int mtu, ip4, ip6, qos, disabled;
	long tcpwin;
};
static int card_parse(int argc, char **argv, struct cardline *cl, char *err, u32 errlen)
{
	const char *v;
	int i;
	memset(cl, 0, sizeof *cl);
	cl->sel.unit = cl->sel.slot = cl->sel.id = cl->sel.metric = -1;
	cl->ip4 = cl->ip6 = cl->qos = -1;
	cl->tcpwin = -1;
	for (i = 2; i < argc; i++)
		if (!rt_strcasecmp(argv[i], "disabled"))
			cl->disabled = 1;
	if (cl->disabled)
		return 0;                   /* a card turned off: not opened */
	for (i = 2; i < argc; i++) {
		if ((v = kv(argv[i], "device")) != NULL || (v = kv(argv[i], "netdev")) != NULL)
			cl->dev = v;
		else if ((v = kv(argv[i], "unit")) != NULL)
			cl->sel.unit = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "slot")) != NULL)
			cl->sel.slot = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "id")) != NULL)
			cl->sel.id = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "card")) != NULL)
			cl->sel.name = v;
		else if ((v = kv(argv[i], "metric")) != NULL)
			cl->sel.metric = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "mtu")) != NULL)
			cl->mtu = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "tcpwindow")) != NULL)
			cl->tcpwin = (long)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "reads")) != NULL)
			;
		else if ((v = kv(argv[i], "ipv4")) != NULL || (v = kv(argv[i], "ipv6")) != NULL || (v = kv(argv[i], "qos")) != NULL) {
			int on = !rt_strcasecmp(v, "on") || !strcmp(v, "1");
			if (!rt_strncasecmp(argv[i], "ipv4", 4)) cl->ip4 = on;
			else if (!rt_strncasecmp(argv[i], "ipv6", 4)) cl->ip6 = on;
			else cl->qos = on;
		} else {
			rt_snprintf(err, errlen, "interface: unknown option '%s'", argv[i]);
			return -RT_EINVAL;
		}
	}
	if (!cl->dev || rt_strcasecmp(cl->dev, ".ENET")) {
		rt_snprintf(err, errlen, "interface: the Mac's driver is device=.ENET");
		return -RT_EINVAL;
	}
	if (cl->sel.unit < 0 && cl->sel.slot < 0 && cl->sel.id < 0 && !cl->sel.name)
		cl->sel.unit = 0;           /* a line naming no card: the first, as before */
	return 0;
}

static int interface_line(int argc, char **argv, char *err, u32 errlen)
{
	struct cardline cl;
	int r;
	struct rt_netif *nif;
	if (argc == 3 && (!rt_strcasecmp(argv[2], "delete") || !rt_strcasecmp(argv[2], "remove"))) {
		rt_snprintf(err, errlen, "interface %s is removed by turning it off and a restart (Save in RiptidePrefs)", argv[1]);
		return -RT_EBUSY;
	}
	if (argc < 3) {
		rt_snprintf(err, errlen, "usage: interface NAME device=.ENET [unit=N] [slot=S id=N] [card=ethernet|wifi] "
		            "[metric=N] [mtu=N] [tcpwindow=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off] [disabled]");
		return -RT_EINVAL;
	}
	r = card_parse(argc, argv, &cl, err, errlen);
	if (r < 0 || cl.disabled)
		return r;
	nif = rt_netif_find(argv[1]);
	if (nif) {
		/* the card is open already: a card kept by a restart (mac_restart), or a second line */
		if (cl.mtu)
			nif->mtu = cl.mtu >= 576 && cl.mtu < 1500 ? (u16)cl.mtu : 1500;
		if (cl.sel.metric >= 0)
			nif->metric = (u32)cl.sel.metric;
	} else {
		rt_unlock();
		r = net_mac_open(argv[1], &cl.sel, cl.mtu);
		rt_lock();
		if (r < 0) {
			rt_snprintf(err, errlen, "%s", mac_net_why);
			return r;
		}
		net_up = 1;
		nif = rt_netif_find(argv[1]);
		if (!nif)
			return 0;
	}
	if (cl.tcpwin >= 0) nif->tcp_window = (u32)cl.tcpwin;
	if (cl.ip4 >= 0) rt_netif_ip4_enable(nif, cl.ip4);
	if (cl.ip6 == 0) rt_ip6_ifenable(nif, 0);
	else if (cl.ip6 == 1) nif->ip6_disabled = 2;
	if (cl.qos >= 0) rt_qos_enable(nif, cl.qos);
	return 0;
}

int mac_config_unknown(int argc, char **argv, char *err, u32 errlen)
{
	if (!rt_strcasecmp(argv[0], "interface"))
		return interface_line(argc, argv, err, errlen);
	if (!rt_strcasecmp(argv[0], "mactcp"))
		return standin_config(argc, argv, err, errlen);      /* IPv6 for MacTCP programs (standin.c) */
	if (!rt_strcasecmp(argv[0], "ipp6"))
		return ipp6_config(argc, argv, err, errlen);         /* .IPP6's lookups (ipp6.c) */
	rt_snprintf(err, errlen, "unknown keyword '%s'", argv[0]);
	return -RT_EINVAL;
}

/* A file of the Riptide folder in the Preferences folder (":Riptide:NAME"), read whole into the
 * stack's pool and NUL-terminated, or NULL. RiptidePrefs saves riptide.conf there (its
 * ENVARC:Riptide/), and keeps the settings of Use in the folder "In Use" (its ENV:Riptide/). */
static short prefs_vref;
static long prefs_dir;
static void *prefs_pb(void *pb, const char *name, unsigned char *pname)
{
	u32 n = (u32)strlen(name), i;
	memset(pb, 0, 80);
	pname[0] = (unsigned char)n;
	for (i = 0; i < n; i++)
		pname[i + 1] = (unsigned char)name[i];
	*(unsigned char **)((u8 *)pb + 18) = pname;
	*(short *)((u8 *)pb + 22) = prefs_vref;
	*(long *)((u8 *)pb + 48) = prefs_dir;
	return pb;
}
static char *prefs_read(const char *name, u32 *len)
{
	u8 pb[80];
	unsigned char pname[64];
	short ref;
	long eof;
	char *buf;
	if (!prefs_dir)
		return NULL;
	prefs_pb(pb, name, pname);
	pb[27] = 1;                                     /* fsRdPerm */
	if (mac_pb(pb, 0xA200) != 0)                    /* _HOpen */
		return NULL;
	ref = *(short *)(pb + 24);
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = ref;
	mac_pb(pb, 0xA011);                             /* _GetEOF */
	eof = *(long *)(pb + 28);
	buf = eof > 0 && eof < 65536 ? rt_malloc((u32)eof + 1) : NULL;
	if (buf) {
		memset(pb, 0, sizeof pb);
		*(short *)(pb + 24) = ref;
		*(char **)(pb + 32) = buf;
		*(long *)(pb + 36) = eof;
		mac_pb(pb, 0xA002);                     /* _Read */
		eof = *(long *)(pb + 40);
		buf[eof] = 0;
		*len = (u32)eof;
	}
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = ref;
	mac_pb(pb, 0xA001);                             /* _Close */
	return buf;
}

/* while mac_restart reads the file: the lines that fail go into its report as well as the log,
   except those that fail only because they name a card the file turns off (its address lines stay
   in riptide.conf for when it is turned on again) */
static char *report;
static u32 report_len;
static char off_cards[4][RT_IFNAMSIZ];
static int noff;
static int names_off_card(const char *err)
{
	char t[RT_IFNAMSIZ + 16];
	for (int i = 0; i < noff; i++) {
		u32 n = (u32)rt_snprintf(t, sizeof t, "no interface '%s'", off_cards[i]);
		for (const char *p = err; *p; p++)
			if (!strncmp(p, t, n))
				return 1;
	}
	return 0;
}
static void report_add(const char *fmt, ...)
{
	va_list ap;
	u32 o;
	if (!report)
		return;
	o = (u32)strlen(report);
	if (o + 1 >= report_len)
		return;
	va_start(ap, fmt);
	rt_vsnprintf(report + o, report_len - o, fmt, ap);
	va_end(ap);
}

static void config_text(const char *text, u32 len)
{
	char line[256], err[96];
	u32 i = 0;
	while (i < len) {
		u32 n = 0;
		while (i < len && text[i] != '\r' && text[i] != '\n') { if (n < sizeof line - 1) line[n++] = text[i]; i++; }
		while (i < len && (text[i] == '\r' || text[i] == '\n')) i++;
		line[n] = 0;
		if (!n || line[0] == '#') continue;
		err[0] = 0;
		rt_lock();
		int r = rt_config_line(line, err, sizeof err, mac_config_unknown);
		rt_unlock();
		if (r < 0) {
			rt_log(RT_LOG_WARN, "riptide.conf: %s: %s", line, err[0] ? err : "error");
			if (!names_off_card(err))
				report_add("riptide.conf: %s: %s\n", line, err[0] ? err : "error");
		}
	}
}

/* does the text name a card ("interface NAME device=...")? */
static int names_card(const char *t, u32 len)
{
	u32 i = 0;
	while (i < len) {
		while (i < len && (t[i] == ' ' || t[i] == '\t')) i++;
		if (len - i > 10 && !rt_strncasecmp(t + i, "interface ", 10)) {
			u32 k = i;
			while (k < len && t[k] != '\r' && t[k] != '\n') {
				if (!rt_strncasecmp(t + k, "device=", 7)) return 1;
				k++;
			}
		}
		while (i < len && t[i] != '\r' && t[i] != '\n') i++;
		while (i < len && (t[i] == '\r' || t[i] == '\n')) i++;
	}
	return 0;
}

/* The Mac's own name for itself: the "Macintosh Name" of the Sharing Setup control panel, 'STR '
 * -16413 in the System file (System 7), made into a host name label (letters, digits and hyphens,
 * lower case; spaces become hyphens). Riptide's host name when riptide.conf names none, and with
 * it the mDNS name: "Mac SE" answers as mac-se.local. Without one, "macintosh". */
static void default_hostname(void)
{
	void **h;
	u32 o = 0;
	if (rt_hostname[0])
		return;
	h = mac_getresource(0x53545220u /* 'STR ' */, -16413);
	if (h && *h) {
		const u8 *p = *h;
		u32 n = p[0];
		for (u32 i = 1; i <= n && o < sizeof rt_hostname - 1; i++) {
			u8 c = p[i];
			if (c >= 'A' && c <= 'Z')
				c = (u8)(c + 32);
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				rt_hostname[o++] = (char)c;
			else if ((c == ' ' || c == '-' || c == '_' || c == '.') && o && rt_hostname[o - 1] != '-')
				rt_hostname[o++] = '-';
		}
		while (o && rt_hostname[o - 1] == '-')
			o--;
	}
	/* the System file's resource stays: Sharing Setup uses it */
	if (!o)
		o = (u32)rt_strlcpy(rt_hostname, "macintosh", sizeof rt_hostname);
	rt_hostname[o] = 0;
	rt_log(RT_LOG_INFO, "host name %s (the Macintosh Name in Sharing Setup)", rt_hostname);
}

/* riptide.conf: the Preferences folder's (RiptidePrefs saves it), else the 'TEXT' 128 resource of
 * the Riptide file (make CONF=), else DHCP on every card. Without an interface line every card is opened (en0, en1 ...). */
/* without an interface line: every Ethernet card the Slot Manager lists, en0, en1 ... in its order
   (on Aurora68 the wired port, then the WiFi), with DHCP on each unless the text says otherwise */
static char deflt[64];
static void open_all_cards(void)
{
	int n = net_mac_cards(), k;
	u32 o = 0;
	if (!n)
		n = 1;                      /* no card in a slot: a built-in .ENET, if there is one */
	for (k = 0; k < n && k < MAC_CARDS; k++) {
		char name[8];
		struct mac_cardsel sel = { k, -1, -1, NULL, -1 };
		rt_snprintf(name, sizeof name, "en%d", k);
		o += (u32)rt_snprintf(deflt + o, sizeof deflt - o, "dhcp %s\n", name);
		if (rt_netif_find(name)) {
			net_up = 1;                 /* kept by a restart */
			continue;
		}
		int r = net_mac_open(name, &sel, 0);
		if (r < 0)
			rt_log(RT_LOG_ERR, "%s: no network (%d): %s", name, r, mac_net_why);
		else
			net_up = 1;
	}
}

static char *boot_text;                         /* the 'TEXT' 128 riptide.conf, if Riptide started with it */
static u32 boot_len;
static void configure(void)
{
	void **h = NULL;
	const char *text = "";
	u32 len = 0;
	char *file;
	u8 pb[80];
	unsigned char pname[64];
	if (mac_findfolder(-32768, 0x70726566 /* 'pref' */, 0, &prefs_vref, &prefs_dir) != 0)
		prefs_dir = 0;
	/* the settings of Use last until the Mac restarts, as ENV: does on the Amiga */
	if (prefs_dir)
		mac_pb(prefs_pb(pb, ":Riptide:In Use:riptide.conf", pname), 0xA209);     /* _HDelete */
	file = prefs_read(":Riptide:riptide.conf", &len);
	if (file && !len) {                             /* an empty file says nothing: as none */
		rt_free(file);
		file = NULL;
	}
	if (file) {
		text = file;
		rt_log(RT_LOG_INFO, "riptide.conf from the Preferences folder (%u bytes)", len);
	} else if ((h = mac_getresource(0x54455854u, 128)) != NULL && *h) {    /* 'TEXT' 128 */
		u32 sz;
		text = *h;
		for (sz = 0; sz < 16384 && text[sz]; sz++) ;
		len = sz;
		/* kept for a restart, which runs when the Riptide file is no longer open */
		if (len && (boot_text = rt_malloc(len + 1)) != NULL) {
			memcpy(boot_text, text, len);
			boot_text[len] = 0;
			boot_len = len;
		}
	}
	if (!names_card(text, len)) {
		open_all_cards();
		if (!len) {                                 /* no configuration at all: DHCP on every card */
			text = deflt;
			len = (u32)strlen(deflt);
		}
	}
	/* the mDNS responder (name.local) is on unless riptide.conf says "mdns off", as on the Amiga */
	config_text("mdns on\n", 8);
	config_text(text, len);
	default_hostname();
	if (h) mac_releaseresource(h);
	if (file) rt_free(file);
}

int rt_mac_open(void *pb, void *dce)
{
	(void)pb; (void)dce;
	if (!core_up) {
		mac_log_line("Open: starting");
		int e = mac_os_init(HEAP_BYTES);
		if (e < 0) { rt_log(RT_LOG_ERR, "Open: no %u KB for the stack's memory pool (%d)", HEAP_BYTES / 1024, e); return -108; }
		/* the clock is set in whole seconds: NTP steps it from 1 s off (riptide.conf: "ntp step MS") */
		rt_ntp_step_default_ms = 1000;
		rt_lock();
		int r = rt_init();
		rt_unlock();
		if (r < 0) { rt_log(RT_LOG_ERR, "rt_init: %d", r); return ipBadCnfgErr; }
		core_up = 1;
		/* QoS rules by program: the MacTCP program a socket is made for (os_mac.c) */
		rt_qos_program_hook = rt_os_program_name;
		mac_clock_init();                       /* the Map control panel's time zone, NTP's clock hook */
		rt_log(RT_LOG_INFO, "Riptide for the Mac: core up");
		configure();
		if (!net_up) { rt_log(RT_LOG_ERR, "Open: no network: %s", mac_net_why); return ipBadLapErr; }
		mac_schedule();
	}
	return net_up ? 0 : ipBadLapErr;
}
int rt_mac_control(void *pb, void *dce) { (void)dce; return ipp_control(pb); }
int rt_mac_status(void *pb, void *dce) { (void)dce; return ipp_status(pb); }

/* ---- RTCMD_RESTART: the network restarted while the Mac runs ----------------------------------
 * RiptidePrefs' Save and Apply send it when a change needs it (rp_restart_what: the cards, their
 * addresses, IPv4 or IPv6 on or off, a setting set back to Default). It runs in a program's
 * context with the core lent to it (ipp.c), as the Open at startup ran in the INIT's: driver and
 * File Manager calls are allowed there, and nothing else runs the core meanwhile.
 *
 * What it does, as "Riptide RESTART" does on the Amiga (stackproc.c), but with the cards kept:
 * - the file is read first; one that cannot be read leaves everything running;
 * - every card leaves the core (rt_netif_remove: DHCP stopped and its lease released, the
 *   addresses, routes, ARP and neighbour entries, IPv6, the multicast groups, the VLANs on it);
 * - a card the file still names, as the same card under the same name, stays open at its driver
 *   (reference number, protocol handlers, receive ring and transmit slots) and comes back into the
 *   core as a card just opened, with its interface index; any other card is detached from its
 *   driver, and the cards the file adds are opened;
 * - rt_config_reset: what the old file set goes back to the start (sysctls, names, name servers,
 *   hosts, static routes, QoS, NTP, mDNS), and the Mac's own lines too (mactcp, ipp6 order);
 * - the file is read as at startup, and the host name from Sharing Setup when it names none.
 * Sockets are not touched: MacTCP streams, .IPP6 endpoints, the Terminal's sockets and their calls
 * stay. A TCP connection whose local address is still there after RESTART_GRACE_MS (a DHCP card
 * has its address again by then, as a rule the same one) goes on; one whose address went away is
 * dropped then with RT_ENETDOWN, which MacTCP and .IPP6 programs see as TCPTerminate with
 * TCPNetworkFailure, their waiting calls ending in connectionTerminated (an open still under way:
 * ipDestDeadErr). The completions and notifications run from the stack's deferred task as always. */
#define RESTART_GRACE_MS 15000u
static struct rt_timer lost_timer;
static int lost_timer_ok;
static void lost_timer_fn(void *arg)
{
	int n = rt_tcp_drop_lost_err(RT_ENETDOWN);
	(void)arg;
	if (n)
		rt_log(RT_LOG_INFO, "restart: %d connection%s ended, their address is gone", n, n == 1 ? "" : "s");
}

/* a card the new file names: its line, parsed (the card= word copied out of the line) */
struct plan_card {
	char name[RT_IFNAMSIZ], cardname[32];
	struct cardline cl;
};
#define PLAN_MAX 8
static int plan_cards(const char *text, u32 len, struct plan_card *pl)
{
	int np = 0;
	u32 i = 0;
	while (i < len && np < PLAN_MAX) {
		char line[256], *argv[24], err[96];
		u32 n = 0;
		int argc, k, vlan = 0;
		while (i < len && text[i] != '\r' && text[i] != '\n') { if (n < sizeof line - 1) line[n++] = text[i]; i++; }
		while (i < len && (text[i] == '\r' || text[i] == '\n')) i++;
		line[n] = 0;
		if (!n || line[0] == '#')
			continue;
		argc = rt_config_split(line, argv, 24);
		if (argc < 3 || rt_strcasecmp(argv[0], "interface"))
			continue;
		for (k = 2; k < argc; k++)
			if (kv(argv[k], "vlan"))
				vlan = 1;
		if (vlan || (argc == 3 && (!rt_strcasecmp(argv[2], "delete") || !rt_strcasecmp(argv[2], "remove"))))
			continue;
		struct plan_card *p = &pl[np];
		if (card_parse(argc, argv, &p->cl, err, sizeof err) < 0)
			continue;                           /* reported when the line is read */
		rt_strlcpy(p->name, argv[1], sizeof p->name);
		if (p->cl.sel.name) {
			rt_strlcpy(p->cardname, p->cl.sel.name, sizeof p->cardname);
			p->cl.sel.name = p->cardname;
		}
		p->cl.dev = NULL;
		np++;
	}
	return np;
}

/* RiptidePrefs' names of its files, as the Preferences folder's: ENV:Riptide/x is In Use:x */
static int restart_path(const char *which, char *path, u32 len)
{
	if (!strncmp(which, "ENV:Riptide/", 12))
		return rt_snprintf(path, len, ":Riptide:In Use:%s", which + 12) < (int)len ? 0 : -1;
	if (!strncmp(which, "ENVARC:Riptide/", 15))
		return rt_snprintf(path, len, ":Riptide:%s", which + 15) < (int)len ? 0 : -1;
	if (which[0] == ':')
		return rt_strlcpy(path, which, len) < len ? 0 : -1;
	return -1;
}

int mac_restart(const char *which, char *out, u32 olen)
{
	struct plan_card plan[PLAN_MAX];
	struct rt_netif *open[MAC_CARDS], *n;
	int keep[MAC_CARDS], nopen = 0, np = 0, named, i, k, kept = 0, nifs = 0;
	const char *text = "";
	char *file = NULL, path[64];
	u32 len = 0, o;
	if (!prefs_dir && mac_findfolder(-32768, 0x70726566 /* 'pref' */, 0, &prefs_vref, &prefs_dir) != 0)
		prefs_dir = 0;
	out[0] = 0;
	if (which && *which) {
		if (restart_path(which, path, sizeof path) < 0) {
			rt_snprintf(out, olen, "Riptide: %s is not a file Riptide reads; not restarting.\n", which);
			return -RT_EINVAL;
		}
		file = prefs_read(path, &len);
		if (!file || !len) {
			if (file) rt_free(file);
			rt_snprintf(out, olen, "Riptide: cannot read %s; not restarting.\n", which);
			return -RT_ENOENT;
		}
		text = file;
	} else {
		/* as at the Mac's start, but what Apply left in the In Use folder first (ENV: before ENVARC:) */
		file = prefs_read(":Riptide:In Use:riptide.conf", &len);
		if (file && !len) { rt_free(file); file = NULL; }
		if (!file) file = prefs_read(":Riptide:riptide.conf", &len);
		if (file && !len) { rt_free(file); file = NULL; }
		if (file) text = file;
		else if (boot_text) { text = boot_text; len = boot_len; }
	}
	rt_log(RT_LOG_INFO, "restart: riptide.conf read again (%s, %lu bytes)", which && *which ? which :
	       file ? "the Preferences folder" : boot_text ? "the Riptide file" : "none", (unsigned long)len);
	named = names_card(text, len);
	if (named)
		np = plan_cards(text, len, plan);
	noff = 0;
	for (k = 0; k < np && noff < 4; k++)
		if (plan[k].cl.disabled)
			rt_strlcpy(off_cards[noff++], plan[k].name, RT_IFNAMSIZ);
	if (!lost_timer_ok) {
		rt_timer_init(&lost_timer, lost_timer_fn, NULL);
		lost_timer_ok = 1;
	}
	report = out;
	report_len = olen;
	rt_lock();
	net_mac_restart_begin();
	rt_timer_del(&lost_timer);
	/* which cards stay: the same card under the same name (without card lines every card is
	   en0, en1 ... in the Slot Manager's order, as at the start) */
	while (nopen < MAC_CARDS && (n = net_mac_nif(nopen)) != NULL) {
		open[nopen] = n;
		keep[nopen] = -1;
		if (!named) {
			struct mac_cardsel sel = { -1, -1, -1, NULL, -1 };
			for (k = 0; k < MAC_CARDS; k++) {
				char nm[8];
				rt_snprintf(nm, sizeof nm, "en%d", k);
				sel.unit = k;
				if (!strcmp(nm, n->name) && net_mac_same(n, &sel))
					keep[nopen] = PLAN_MAX;
			}
		}
		for (k = 0; k < np && keep[nopen] < 0; k++)
			if (!plan[k].cl.disabled && !strcmp(plan[k].name, n->name) && net_mac_same(n, &plan[k].cl.sel))
				keep[nopen] = k;
		nopen++;
	}
	for (i = 0; i < nopen; i++) {
		n = open[i];
		rt_netif_remove(n);                        /* VLANs, DHCP (released), addresses, routes, IPv6 */
		if (keep[i] >= 0) {
			const struct plan_card *p = keep[i] < PLAN_MAX ? &plan[keep[i]] : NULL;
			net_mac_renew(n, p ? p->cl.mtu : 0, p ? p->cl.sel.metric : -1);
		} else {
			rt_log(RT_LOG_INFO, "restart: %s closed", n->name);
			report_add("%s: closed\n", n->name);
			net_mac_close(n);
		}
	}
	rt_config_reset();
	standin_restart_begin();
	{
		char e[64];
		rt_config_line("ipp6 order default", e, sizeof e, mac_config_unknown);
	}
	net_up = 0;
	for (i = 0; i < nopen; i++)
		if (keep[i] >= 0) {
			u32 index = open[i]->index;
			if (rt_netif_add(open[i]) == 0) {
				open[i]->index = index;            /* the same interface: scope ids stay valid */
				net_up = 1;
				kept++;
			}
		}
	if (!named) {
		open_all_cards();
		if (!len) {
			text = deflt;
			len = (u32)strlen(deflt);
		}
	}
	config_text("mdns on\n", 8);
	config_text(text, len);
	report = NULL;
	noff = 0;
	default_hostname();
	standin_restart_end();
	rt_timer_after(&lost_timer, RESTART_GRACE_MS);
	for (n = rt_netif_list(); n; n = n->next)
		if (n->type != RT_IFT_LOOP)
			nifs++;
	net_mac_restart_end();
	/* what happened to each card */
	for (k = 0; (n = net_mac_nif(k)) != NULL; k++) {
		int was = 0;
		for (i = 0; i < nopen; i++)
			if (open[i] == n && keep[i] >= 0)
				was = 1;
		rt_log(RT_LOG_INFO, "restart: %s %s (MTU %u)", n->name, was ? "kept open" : "opened", (unsigned)n->mtu);
		o = (u32)strlen(out);
		if (o + 1 < olen)
			rt_snprintf(out + o, olen - o, "%s: %s\n", n->name, was ? "kept" : "opened");
	}
	rt_unlock();
	if (file)
		rt_free(file);
	o = (u32)strlen(out);
	if (o + 1 < olen)
		rt_snprintf(out + o, olen - o, "Riptide: restarted (%d interface%s)\n", nifs, nifs == 1 ? "" : "s");
	rt_log(RT_LOG_INFO, "restart: done, %d interface%s (%d kept)", nifs, nifs == 1 ? "" : "s", kept);
	return 0;
}
