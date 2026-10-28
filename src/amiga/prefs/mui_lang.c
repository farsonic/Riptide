/*
 * Riptide - RiptidePrefs (MUI): translation. Every string the MUI editor
 * shows goes through tr(), which looks it up (binary search on the
 * English text) in the table below and returns the text of the current
 * language, or the English text itself when there is no translation.
 * English only so far: a translation is a column added to rows[] (sorted
 * by the English text, as strcmp() orders it) and a name in langs[].
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <string.h>
#include "prefs/mui_theme.h"

#define NLANG 1                 /* columns after the English one */

struct row {
	const char *en;
	const char *t[NLANG];
};

/* sorted by en (strcmp); NULL: not translated yet */
static const struct row rows[] = {
	{ "", { NULL } },
};
static int lang = -1;           /* column, -1: English */

const char *tr(const char *s)
{
	int lo = 0, hi = (int)(sizeof(rows) / sizeof(rows[0])) - 1;

	if (lang < 0 || !s)
		return s;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(s, rows[mid].en);
		if (!c)
			return rows[mid].t[lang] ? rows[mid].t[lang] : s;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return s;
}
