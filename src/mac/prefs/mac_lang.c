/*
 * Riptide - RiptidePrefs on the classic Mac OS: the editor's words for the Mac.
 *
 * Every string the MUI editor shows goes through tr() (src/amiga/prefs/mui_lang.c on the Amiga,
 * its translation hook). The Mac build has this tr() instead: the texts are the Amiga's, and
 * where they speak of the Amiga's own things (SANA-II drivers, DEVS:, ENV:, uaenet.device,
 * "the Amiga") the Mac's are put in their place. The shared files stay as they are, so a new
 * setting or page needs nothing here unless its text names something only the Amiga has.
 *
 * Rules are keyed on a distinctive piece of a text, not the whole of it, so a small change of
 * the Amiga's wording keeps working. WHOLE rules replace a text that contains the key; SUB rules
 * replace the key wherever it appears (the driver line and other texts made with printf). A text
 * is looked at once: the answer is kept with its address and contents (the editor keeps some,
 * such as the labels of its cycles, for as long as the window is open).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mui/mm.h"
#include "prefs/mui_theme.h"

struct rule { const char *key, *mac; int whole; };

static const struct rule rules[] = {
	/* ---- the Interface page ---- */
	{ "each one's SANA-II driver and unit",
	  "The network cards Riptide uses: each one's driver (.ENET, the Mac's Ethernet driver), which card it "
	  "is, and how big its packets may be.", 1 },
	{ "The network card's driver: SANA-II",
	  "The network card's driver. On the Mac it is always .ENET, the Ethernet driver every Mac network card "
	  "and built-in Ethernet port has.", 1 },
	{ "Which card the driver should use when it handles several",
	  "Which Ethernet card: 0 is the first one the Mac finds (the built-in port or the card in the lowest "
	  "slot), 1 the next.", 1 },
	{ "How many receive requests Riptide keeps waiting at the driver",
	  "Not used on the Mac: .ENET hands every frame to Riptide as it arrives.", 1 },
	{ "Only for old FS-UAE builds that drop frames",
	  "Caps TCP's receive window on this card. Leave it empty: a limit such as 1440 makes every download "
	  "crawl.", 1 },
	{ "uaenet.device (a new file)", ".ENET (the first Ethernet card)", 1 },
	{ "enter the SANA-II device", "enter .ENET, the Mac's Ethernet driver", 1 },
	{ "enter its SANA-II device and unit below",
	  "Adds a network card: its driver is .ENET; enter which card it is (0 is the first) below.", 1 },
	/* ---- the buttons, the About page, the Status page ---- */
	{ "Uses the settings until the Amiga is switched off",
	  "Uses the settings now and until the Mac restarts, without saving them for good.", 1 },
	{ "A TCP/IP stack for AmigaOS 3.x",
	  "A TCP/IP stack for the classic Mac OS on the\n68020 to 68040: modern TCP, IPv4 and IPv6, and\n"
	  "the MacTCP driver (.IPP) Mac programs use.", 1 },
	{ "Start it from its icon or a Shell",
	  "Riptide is not running. It starts with the Mac: put it in the Extensions folder.", 1 },
	/* ---- applying ---- */
	{ "Restart now saves the settings to ENV:",
	  "Some of the changes need a restart of Riptide's network, which\nRiptide for the Mac cannot do while "
	  "it runs yet. They take\neffect when the Mac restarts.", 1 },
	{ "cannot restart its network (0.39 or later can)",
	  "Riptide for the Mac cannot restart its network while it runs yet.\nThe settings take effect when the "
	  "Mac restarts.", 1 },
	{ "Apply restarts Riptide's network (no reboot)", "Card changes take effect when the Mac restarts", 1 },
	/* ---- words ---- */
	{ "ENVARC:Riptide/", "Preferences:Riptide:", 0 },
	{ "ENV:Riptide/", "Preferences:Riptide:In Use:", 0 },
	{ "(DEVS:Networks/, DEVS:)", "(the Mac's driver is .ENET)", 0 },
	{ "(Apply, then\nRestart now; or Riptide RESTART)", "(when the Mac restarts)", 0 },
	{ "Riptide RESTART", "a restart of the Mac", 0 },
	{ "ENV:", "the In Use folder", 0 },
	{ "SANA-II", "Ethernet", 0 },
	{ "uaenet.device", ".ENET", 0 },
	{ "such as amy.", "such as macse.", 0 },
	{ "This Amiga", "This Mac", 0 },
	{ "this Amiga", "this Mac", 0 },
	{ "The Amiga", "The Mac", 0 },
	{ "the Amiga", "the Mac", 0 },
	{ "Amiga", "Mac", 0 },
};
#define NRULES ((int)(sizeof(rules) / sizeof(rules[0])))

static const char *find(const char *s, const char *k)
{
	size_t n = strlen(k);
	for (; *s; s++)
		if (*s == *k && !strncmp(s, k, n))
			return s;
	return NULL;
}

/* the Mac's text of s, newly made (NULL: s needs no change) */
static char *macify(const char *s)
{
	char buf[1024];
	int i, o = 0, changed = 0;
	for (i = 0; i < NRULES; i++)
		if (rules[i].whole && find(s, rules[i].key))
			return mm_strdup(rules[i].mac);
	while (*s && o < (int)sizeof buf - 1) {
		int hit = -1;
		for (i = 0; i < NRULES; i++)
			if (!rules[i].whole && *s == *rules[i].key && !strncmp(s, rules[i].key, strlen(rules[i].key))) {
				hit = i;
				break;
			}
		if (hit < 0) {
			buf[o++] = *s++;
			continue;
		}
		changed = 1;
		o += (int)rt_strlcpy(buf + o, rules[hit].mac, sizeof buf - (size_t)o);
		if (o > (int)sizeof buf - 1)
			o = (int)sizeof buf - 1;
		s += strlen(rules[hit].key);
	}
	buf[o] = 0;
	return changed ? mm_strdup(buf) : NULL;
}

/* the texts seen: address, a hash of the contents, and the Mac's text (NULL: the same) */
#define NCACHE 512
static struct { const char *in; ULONG hash; char *out; } cache[NCACHE];

static ULONG hash(const char *s)
{
	ULONG h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

const char *tr(const char *s)
{
	ULONG h, k;
	int i;
	if (!s || !*s)
		return s;
	h = hash(s);
	k = ((ULONG)s >> 2) % NCACHE;
	for (i = 0; i < NCACHE; i++, k = (k + 1) % NCACHE) {
		if (cache[k].in == s) {
			if (cache[k].hash != h) {
				/* a buffer whose text changed: look again */
				mm_free(cache[k].out);
				cache[k].out = macify(s);
				cache[k].hash = h;
			}
			return cache[k].out ? cache[k].out : s;
		}
		if (!cache[k].in) {
			cache[k].in = s;
			cache[k].hash = h;
			cache[k].out = macify(s);
			return cache[k].out ? cache[k].out : s;
		}
	}
	/* full: no cache (still right, a little slower) */
	{
		static char *last;
		mm_free(last);
		last = macify(s);
		return last ? last : s;
	}
}
