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
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "riptide.h"
#include "mac.h"
#include "standin.h"
#include "rt_netif.h"
#include "rt_ip6.h"
#include "rt_qos.h"

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
static int interface_line(int argc, char **argv, char *err, u32 errlen)
{
	const char *dev = NULL, *v;
	struct mac_cardsel sel = { -1, -1, -1, NULL, -1 };
	int mtu = 0, ip4 = -1, ip6 = -1, qos = -1, i, r;
	long tcpwin = -1;
	struct rt_netif *nif;
	if (argc == 3 && (!rt_strcasecmp(argv[2], "delete") || !rt_strcasecmp(argv[2], "remove"))) {
		rt_snprintf(err, errlen, "interface %s cannot be removed while the Mac runs", argv[1]);
		return -RT_EBUSY;
	}
	if (argc < 3) {
		rt_snprintf(err, errlen, "usage: interface NAME device=.ENET [unit=N] [slot=S id=N] [card=ethernet|wifi] "
		            "[metric=N] [mtu=N] [tcpwindow=N] [ipv4=on|off] [ipv6=on|off] [qos=on|off] [disabled]");
		return -RT_EINVAL;
	}
	for (i = 2; i < argc; i++)
		if (!rt_strcasecmp(argv[i], "disabled"))
			return 0;               /* a card turned off: not opened */
	for (i = 2; i < argc; i++) {
		if ((v = kv(argv[i], "device")) != NULL || (v = kv(argv[i], "netdev")) != NULL)
			dev = v;
		else if ((v = kv(argv[i], "unit")) != NULL)
			sel.unit = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "slot")) != NULL)
			sel.slot = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "id")) != NULL)
			sel.id = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "card")) != NULL)
			sel.name = v;
		else if ((v = kv(argv[i], "metric")) != NULL)
			sel.metric = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "mtu")) != NULL)
			mtu = (int)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "tcpwindow")) != NULL)
			tcpwin = (long)rt_strtoul(v, NULL, 0);
		else if ((v = kv(argv[i], "reads")) != NULL)
			;
		else if ((v = kv(argv[i], "ipv4")) != NULL || (v = kv(argv[i], "ipv6")) != NULL || (v = kv(argv[i], "qos")) != NULL) {
			int on = !rt_strcasecmp(v, "on") || !strcmp(v, "1");
			if (!rt_strncasecmp(argv[i], "ipv4", 4)) ip4 = on;
			else if (!rt_strncasecmp(argv[i], "ipv6", 4)) ip6 = on;
			else qos = on;
		} else {
			rt_snprintf(err, errlen, "interface: unknown option '%s'", argv[i]);
			return -RT_EINVAL;
		}
	}
	if (!dev || rt_strcasecmp(dev, ".ENET")) {
		rt_snprintf(err, errlen, "interface: the Mac's driver is device=.ENET");
		return -RT_EINVAL;
	}
	if (rt_netif_find(argv[1]))
		return 0;                   /* the card is up already (a second Open) */
	if (sel.unit < 0 && sel.slot < 0 && sel.id < 0 && !sel.name)
		sel.unit = 0;               /* a line naming no card: the first, as before */
	rt_unlock();
	r = net_mac_open(argv[1], &sel, mtu);
	rt_lock();
	if (r < 0) {
		rt_snprintf(err, errlen, "%s", mac_net_why);
		return r;
	}
	net_up = 1;
	nif = rt_netif_find(argv[1]);
	if (!nif)
		return 0;
	if (tcpwin >= 0) nif->tcp_window = (u32)tcpwin;
	if (ip4 >= 0) rt_netif_ip4_enable(nif, ip4);
	if (ip6 == 0) rt_ip6_ifenable(nif, 0);
	else if (ip6 == 1) nif->ip6_disabled = 2;
	if (qos >= 0) rt_qos_enable(nif, qos);
	return 0;
}

int mac_config_unknown(int argc, char **argv, char *err, u32 errlen)
{
	if (!rt_strcasecmp(argv[0], "interface"))
		return interface_line(argc, argv, err, errlen);
	if (!rt_strcasecmp(argv[0], "mactcp"))
		return standin_config(argc, argv, err, errlen);      /* IPv6 for MacTCP programs (standin.c) */
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
		if (r < 0) rt_log(RT_LOG_WARN, "riptide.conf: %s: %s", line, err[0] ? err : "error");
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
		int r = net_mac_open(name, &sel, 0);
		if (r < 0)
			rt_log(RT_LOG_ERR, "%s: no network (%d): %s", name, r, mac_net_why);
		else
			net_up = 1;
		o += (u32)rt_snprintf(deflt + o, sizeof deflt - o, "dhcp %s\n", name);
	}
}

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
		rt_lock();
		int r = rt_init();
		rt_unlock();
		if (r < 0) { rt_log(RT_LOG_ERR, "rt_init: %d", r); return ipBadCnfgErr; }
		core_up = 1;
		rt_log(RT_LOG_INFO, "Riptide for the Mac: core up");
		configure();
		if (!net_up) { rt_log(RT_LOG_ERR, "Open: no network: %s", mac_net_why); return ipBadLapErr; }
		mac_schedule();
	}
	return net_up ? 0 : ipBadLapErr;
}
int rt_mac_control(void *pb, void *dce) { (void)dce; return ipp_control(pb); }
int rt_mac_status(void *pb, void *dce) { (void)dce; return ipp_status(pb); }
