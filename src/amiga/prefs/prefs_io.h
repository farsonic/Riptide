/*
 * Riptide - RiptidePrefs: what both editors (GadTools and MUI) share on
 * the Amiga side: the configuration being edited, reading and writing
 * riptide.conf, applying changes to the running stack, the list pages and
 * requesters.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_IO_H
#define RT_PREFS_IO_H

#include "prefs/conf_edit.h"
#include "prefs/prefs_fields.h"

#define RP_ENV_CONF    "ENV:Riptide/riptide.conf"
#define RP_ENVARC_CONF "ENVARC:Riptide/riptide.conf"
#define RP_TITLE       "Riptide Preferences"

extern struct rp_conf rp_cfg;          /* the configuration being edited */
extern const char *rp_from_path;       /* FROM=, or NULL */
extern int rp_wb;                      /* started from Workbench: no CLI output */

/* The front end's requester (NULL: the text goes to the CLI). Returns the
 * number of the button pressed, counted as EasyRequest does (the last
 * one is 0). */
extern long (*rp_request_fn)(const char *text, const char *buttons);
long rp_request(const char *text, const char *buttons);
void rp_message(const char *text);

/* riptide.conf: ENV: (or ENVARC: when ENV: has none, or FROM). 0 or -1
 * (message shown). */
int  rp_load(void);
/* save = 1: ENVARC: (old file kept as .bak) and ENV:; 0: ENV: only */
int  rp_store(int save);
/* Send the changes a running Riptide can take to its port. ask = 1 (after
 * Save or Use): only when something changed, and after asking. */
void rp_apply(int ask);

/* the entries of a list page (rp_page_list() != -1) */
int  rp_list_n(int page);
const char *rp_list_at(int page, int i);
/* Add (sel < 0) or replace entry sel by text, or delete it (text NULL).
 * Returns the entry to select afterwards, -1 for none, or -2 when the
 * text was refused (message shown). */
int  rp_list_edit(int page, int sel, const char *text);

/* ENVARC:Riptide/RiptidePrefs.prefs: the editor's own settings ("key=value"
 * lines; the MUI editor's theme and help bubbles) */
int  rp_eprefs_get(const char *key, int def);
void rp_eprefs_set(const char *key, int v);
void rp_eprefs_save(void);

void *rp_read_file(const char *path, long *len);

#endif
