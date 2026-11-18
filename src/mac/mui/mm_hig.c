/*
 * Riptide - the Mac's MUI (see mm.h): the words and characters of the Macintosh Human Interface
 * Guidelines (Apple, 1992) in place of the Amiga's, for every text the layer shows of its own
 * accord (menus, buttons, requesters). A program's own texts go through its tr() hook first;
 * these are the rules every MUI program needs on the Mac, whatever it is:
 *   "..."    the ellipsis character (Guidelines p. 67-71: "…", not three periods)
 *   Drawer   Folder (an Amiga directory is a Mac folder; Guidelines ch. 11, the Macintosh
 *            vocabulary)
 * and for labels (menu items, buttons) also:
 *   Exit     Quit (the File menu's last command, p. 87)
 *   '_'      dropped (MUI's keyboard-shortcut marker; Mac buttons and menus show none)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"

static const struct { const char *amiga, *mac; } words[] = {
	{ "Drawers", "Folders" }, { "drawers", "folders" }, { "Drawer", "Folder" }, { "drawer", "folder" },
};

static int is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

void mm_hig_text(char *s, int size)
{
	char out[1024];
	int o = 0, i = 0, k;
	if (!s)
		return;
	while (s[i] && o < (int)sizeof out - 8 && o < size - 1) {
		if (s[i] == '.' && s[i + 1] == '.' && s[i + 2] == '.') {
			out[o++] = (char)0xC9;
			i += 3;
			continue;
		}
		for (k = 0; k < (int)(sizeof words / sizeof words[0]); k++) {
			int n = (int)strlen(words[k].amiga);
			if (!strncmp(s + i, words[k].amiga, (size_t)n) && (i == 0 || !is_word(s[i - 1])) && !is_word(s[i + n]))
				break;
		}
		if (k < (int)(sizeof words / sizeof words[0])) {
			const char *m = words[k].mac;
			while (*m && o < size - 1)
				out[o++] = *m++;
			i += (int)strlen(words[k].amiga);
			continue;
		}
		out[o++] = s[i++];
	}
	out[o] = 0;
	rt_strlcpy(s, out, (size_t)size);
}

void mm_hig_label(char *s, int size)
{
	char *d = s, *r = s;
	if (!s)
		return;
	while (*r) {
		if (*r != '_')
			*d++ = *r;
		r++;
	}
	*d = 0;
	if (!strcmp(s, "Exit"))
		rt_strlcpy(s, "Quit", (size_t)size);
	mm_hig_text(s, size);
}
