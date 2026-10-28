/*
 * Riptide - RiptidePrefs (MUI): command IDs, in blocks with fixed starts
 * so adding a command never moves another block (CMD=n refers to them).
 * New commands go at the end of their block.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_IDS_H
#define RT_PREFS_MUI_IDS_H

enum {
	/* Project menu */
	ID_OPEN = 100, ID_SAVEAS, ID_SAVE, ID_USE, ID_APPLY, ID_ABOUT, ID_QUIT,
	/* Edit menu */
	ID_DEFAULTS = 200, ID_LASTSAVED, ID_RESTORE,
	/* Settings menu */
	ID_DARK = 300, ID_TIPS,
	/* not menu items: the window reporting */
	ID_CANCEL = 400, ID_PAGE, ID_FRONT,
	/* the buttons of a message: ID_DLGBTN + button */
	ID_DLGBTN = 450,
	/* list pages: ID_LIST + page * 4 + LIST_SEL/ADD/REPL/DEL */
	ID_LIST = 500,
	/* a field of a list page's form changed: ID_LFIELD + field */
	ID_LFIELD = 900,
	/* a check box or cycle gadget changed: ID_FIELD + field */
	ID_FIELD = 1000
};
enum { LIST_SEL, LIST_ADD, LIST_REPL, LIST_DEL };

#endif
