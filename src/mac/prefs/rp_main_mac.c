/*
 * Riptide - RiptidePrefs on the classic Mac OS: the program's start. The editor itself is the
 * Amiga's MUI editor (src/amiga/prefs/mui_main.c and the files around it), compiled unchanged on
 * the Mac's MUI (src/mac/mui); this file does what riptideprefs.c does on the Amiga for it:
 * read riptide.conf, open the editor, and free everything at the end. See docs/MAC-GUI.md.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mui/mm.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_gui.h"
#include "prefs/prefs_fields.h"

void mm_gfx_lib_init(void);
void mm_screen_init(void *qd_thePort_addr);
void mm_paths_init(void);

/* The first run: no riptide.conf yet. On the Amiga the Installer writes one before the editor
 * ever runs (a card and DHCP); the Mac has no installer, so the editor writes the same start, the
 * Mac's card on DHCP, which is also what the driver does without a file. */
static void first_run(void)
{
	static const char start[] = "# Riptide for the Mac (RiptidePrefs wrote this start)\n"
	                            "interface en0 device=.ENET unit=0\ndhcp en0\n";
	BPTR l = Lock((CONST_STRPTR)RP_ENVARC_CONF, ACCESS_READ), f;
	if (l) {
		UnLock(l);
		return;
	}
	if ((l = Lock((CONST_STRPTR)RP_ENV_CONF, ACCESS_READ)) != 0) {
		UnLock(l);
		return;
	}
	if ((l = CreateDir((CONST_STRPTR)"ENVARC:Riptide")) != 0)
		UnLock(l);
	f = Open((CONST_STRPTR)RP_ENVARC_CONF, MODE_NEWFILE);
	if (f) {
		Write(f, (APTR)start, (LONG)(sizeof start - 1));
		Close(f);
		mm_log("first run: wrote the starting riptide.conf");
	}
}

int main(void)
{
	struct rp_gui_args ga;
	void *a5;
	int rc;
	__asm__ volatile("move.l %%a5,%0" : "=r"(a5));
	mm_gfx_lib_init();
	mm_classes_init();
	/* the SE's 512 x 342 screen and anything under 400 lines get the compact layout */
	{
		Rect *sb = LM_SCREENBITS_BOUNDS(*(void **)a5);
		mm_gfx_init(sb->bottom - sb->top - LM_MBARHEIGHT - 20 < 560);     /* as mm_screen_init decides */
	}
	mm_screen_init(*(void **)a5);
	mm_paths_init();
	mm_log("RiptidePrefs for the Mac starts");
	memset(&ga, 0, sizeof(ga));
	ga.page = -1;
	ga.theme = -1;
	rp_wb = 1;
	first_run();
	if (rp_load() < 0)
		return 0;
	rp_single_start();
	rc = mui_run(&ga);
	rp_single_end();
	mm_log("RiptidePrefs ends (%d)", rc);
	rp_conf_free(&rp_cfg);
	return 0;
}
