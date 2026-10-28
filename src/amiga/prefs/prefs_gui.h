/*
 * Riptide - RiptidePrefs: the two editors. riptideprefs.c opens the MUI
 * editor (mui_main.c) when muimaster.library 19 (MUI 3.8) or later is
 * installed, else the GadTools one (gt_prefs.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_GUI_H
#define RT_PREFS_GUI_H

struct IntuitionBase;
struct GfxBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;

struct rp_gui_args {
	const char *pubscreen;  /* PUBSCREEN=, or NULL for the default one */
	int page;               /* PAGE=: the page to open on, or -1 */
	long cmd;               /* CMD=: a menu command to run once open (MUI), or 0 */
	int theme;              /* THEME=: 1 dark, 0 light, -1 the saved choice (MUI) */
};

/* both return CL_OK when closed normally, CL_FAIL when they could not start */
int gt_run(const struct rp_gui_args *a);
int mui_run(const struct rp_gui_args *a);

#endif
