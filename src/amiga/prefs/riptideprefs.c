/*
 * Riptide - RiptidePrefs: preferences editor for riptide.conf (AmigaOS
 * 2.04+ and AROS; started from Workbench or the CLI).
 *
 *   RiptidePrefs [FROM=file] [EDIT] [USE] [SAVE] [PUBSCREEN=name]
 *                [GADTOOLS] [PAGE=name] [CMD=n] [THEME=DARK|LIGHT] [DEBUG=file]
 *
 * Edits ENV:Riptide/riptide.conf (or ENVARC: when ENV: has none, or FROM).
 * "Save" writes ENVARC:Riptide/riptide.conf (the old one becomes
 * riptide.conf.bak) and ENV:, "Use" writes ENV: only, "Apply" sends the
 * changes that a running Riptide can take (host name, name servers, time,
 * sysctls, extra addresses, QoS) to the RIPTIDE port. USE or SAVE on the
 * command line writes the file without opening the window, like the
 * system prefs editors. conf_edit.c keeps every line it does not
 * understand as it is.
 *
 * With MUI 3.8 or later installed the MUI editor opens (mui_main.c), else
 * the GadTools one (gt_prefs.c); GADTOOLS asks for the GadTools one.
 * Started from Workbench, the icon's tool types PAGE=, GADTOOLS,
 * PUBSCREEN=, THEME= and DEBUG= do the same as the arguments.
 * PAGE opens on a page (its name, e.g. PAGE=Ports, or number), CMD runs a
 * menu command once the window is open and THEME picks the MUI editor's
 * colours for this run (for screenshots and testing); DEBUG writes a log.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "rt/rt_version.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_gui.h"

#if defined(__VBCC__)
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>
#else
#include "os/intuition.h"
#include "gen/icon_calls.h"
#endif

struct Library *IconBase;
static struct WBStartup *wbstart;      /* the Workbench start, NULL from a Shell */

const char riptideprefs_version[] = "$VER: RiptidePrefs " RIPTIDE_VERSION_STRING " (" RIPTIDE_DATE ")";

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

/* MUI with our classes, and the message windows' input loops inside
 * commands, need far more than a Shell's 4 KB: without it a message
 * window overflowed the stack and corrupted memory */
#define RP_STACK 65536
#if defined(__VBCC__)
size_t __stack = RP_STACK;      /* vbcc's start-up swaps to a stack this big */
#endif

/* lower case of an ASCII letter */
static char lc(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* PAGE=: a page number, or its title or name (case does not matter) */
static int page_arg(const char *s)
{
	int i;

	if (!s)
		return -1;
	if (*s >= '0' && *s <= '9') {
		const char *e;
		unsigned long v = rt_strtoul(s, &e, 10);
		return !*e && v < RP_NPAGES ? (int)v : -1;
	}
	for (i = 0; i < RP_NPAGES; i++) {
		const char *a = rp_pages[i].title, *b = s;
		while (*a && *b && lc(*a) == lc(*b))
			a++, b++;
		if (!*a && !*b)
			return i;
	}
	return -1;
}

/* Workbench: the tool types of our icon into args (PUBSCREEN, GADTOOLS,
 * PAGE, THEME, DEBUG as in the template below); the strings are copied */
static void wb_tooltypes(LONG *args)
{
	static char pub[64], pg[32], theme[8], dbg[64];
	struct DiskObject *dob;
	BPTR old;
	const char *v;

	if (!wbstart || wbstart->sm_NumArgs < 1 || !wbstart->sm_ArgList)
		return;
	IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36);
	if (!IconBase)
		return;
	old = CurrentDir(wbstart->sm_ArgList[0].wa_Lock);
	dob = GetDiskObject((CONST_STRPTR)wbstart->sm_ArgList[0].wa_Name);
	if (dob) {
		CONST_STRPTR *tt = (CONST_STRPTR *)dob->do_ToolTypes;
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"PUBSCREEN")) != NULL && *v) {
			rt_strlcpy(pub, v, sizeof(pub));
			args[4] = (LONG)pub;
		}
		if (FindToolType(tt, (CONST_STRPTR)"GADTOOLS"))
			args[5] = 1;
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"PAGE")) != NULL && *v) {
			rt_strlcpy(pg, v, sizeof(pg));
			args[6] = (LONG)pg;
		}
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"THEME")) != NULL && *v) {
			rt_strlcpy(theme, v, sizeof(theme));
			args[8] = (LONG)theme;
		}
		if ((v = (const char *)FindToolType(tt, (CONST_STRPTR)"DEBUG")) != NULL && *v) {
			rt_strlcpy(dbg, v, sizeof(dbg));
			args[9] = (LONG)dbg;
		}
		FreeDiskObject(dob);
	}
	CurrentDir(old);
	CloseLibrary(IconBase);
	IconBase = NULL;
}

/* CLI: FROM,EDIT/S,USE/S,SAVE/S,PUBSCREEN/K,GADTOOLS/S,PAGE/K,CMD/K/N,THEME/K,DEBUG/K */
static int prefs_main(int from_cli)
{
	LONG args[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	struct RDArgs *rda = NULL;
	struct rp_gui_args ga;
	int rc = CL_OK;

	if (from_cli) {
		rda = (struct RDArgs *)ReadArgs(
			(CONST_STRPTR)"FROM,EDIT/S,USE/S,SAVE/S,PUBSCREEN/K,GADTOOLS/S,PAGE/K,CMD/K/N,THEME/K,DEBUG/K", args, NULL);
		if (!rda) {
			PrintFault(IoErr(), (CONST_STRPTR)"RiptidePrefs");
			return CL_ERROR;
		}
		rp_from_path = (const char *)args[0];
		rp_log_open((const char *)args[9]);
	} else {
		wb_tooltypes(args);
		rp_log_open((const char *)args[9]);
	}
	ga.pubscreen = (const char *)args[4];
	ga.page = page_arg((const char *)args[6]);
	ga.cmd = args[7] ? *(LONG *)args[7] : 0;
	ga.theme = -1;
	if (args[8])
		ga.theme = lc(*(const char *)args[8]) == 'd';
	if (rp_load() < 0) {
		rc = CL_ERROR;
	} else if (args[2] || args[3]) {
		rc = rp_store(args[3] != 0) < 0 ? CL_ERROR : CL_OK;
	} else if (rp_single_start()) {
		/* another editor is open: it comes to the front, this copy ends */
		rc = CL_OK;
	} else {
		IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
		GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 37);
		rc = CL_FAIL;
		if (IntuitionBase && GfxBase) {
			if (!args[5])
				rc = mui_run(&ga);
			if (rc == CL_FAIL) {
				rp_log("GadTools editor");
				rc = gt_run(&ga);
			}
		}
		if (GfxBase)
			CloseLibrary((struct Library *)GfxBase);
		if (IntuitionBase)
			CloseLibrary((struct Library *)IntuitionBase);
		IntuitionBase = NULL;
		GfxBase = NULL;
		rp_single_end();
	}
	rp_log("exit %d", rc);
	rp_log_close();
	rp_conf_free(&rp_cfg);
	if (rda)
		FreeArgs(rda);
	return rc;
}

#if defined(__VBCC__)
int main(int argc, char **argv)
{
	rp_wb = argc == 0;
	if (rp_wb)
		wbstart = (struct WBStartup *)argv;
	return prefs_main(!rp_wb);
}
#else
struct rp_stackswap {           /* exec's struct StackSwapStruct */
	APTR  stk_Lower;
	ULONG stk_Upper;
	APTR  stk_Pointer;
};
/* mui_thunks.S: fn(arg) on the stack sss describes (exec StackSwap()) */
LONG rp_bigstack(LONG (*fn)(LONG), LONG arg, struct rp_stackswap *sss);

static LONG prefs_entry(LONG from_cli)
{
	return prefs_main((int)from_cli);
}

long amiga_main(char *s, long l)
{
	struct Process *me;
	struct Message *wbmsg = NULL;
	struct rp_stackswap sss;
	APTR stk;
	int rc;

	(void)s;
	(void)l;
	DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
	me = (struct Process *)FindTask(NULL);
	if (!me->pr_CLI) {
		WaitPort(&me->pr_MsgPort);
		wbmsg = GetMsg(&me->pr_MsgPort);
		wbstart = (struct WBStartup *)wbmsg;
		rp_wb = 1;
	}
	rc = CL_FAIL;
	if (DOSBase) {
		/* the editor on a stack of its own (see RP_STACK) */
		stk = AllocVec(RP_STACK, MEMF_ANY);
		if (stk) {
			sss.stk_Lower = stk;
			sss.stk_Upper = (ULONG)stk + RP_STACK;
			sss.stk_Pointer = (APTR)sss.stk_Upper;
			rc = (int)rp_bigstack(prefs_entry, !rp_wb, &sss);
			FreeVec(stk);
		} else {
			rc = prefs_main(!rp_wb);
		}
	}
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);
	if (wbmsg) {
		Forbid();
		ReplyMsg(wbmsg);
	}
	return rc;
}
#endif
