/*
 * Riptide test helper - wbprobe FILE SECS: every SECS seconds write a
 * snapshot of the system to FILE (rewritten each time; "seq N" on the
 * first line): every task in exec's ready and wait lists (name, priority,
 * state, signals waited for), how many "RiptidePrefs" processes run and
 * whether its port exists, whether the Workbench screen's layers can be locked right now
 * (someone holding them would stop Workbench from selecting icons) and
 * the free memory.
 *
 * For tests/amiga/run_fsuae_ks314_wb.sh: a report said Workbench stopped
 * letting icons be selected after RiptidePrefs was opened and saved. The
 * test reads these snapshots after each round.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/screens.h>
#include <graphics/layers.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct ExecBase *SysBase;
struct IntuitionBase *IntuitionBase;
struct Library *LayersBase;

static char buf[6000];

static int nprefs;    /* tasks named RiptidePrefs */

/* append the tasks of list l, state tag s */
static int walk(char *o, int left, struct List *l, const char *s)
{
	struct Node *nd;
	int n = 0;

	for (nd = l->lh_Head; nd->ln_Succ && left > 80; nd = nd->ln_Succ) {
		struct Task *t = (struct Task *)nd;
		if (nd->ln_Name && !strcmp(nd->ln_Name, "RiptidePrefs"))
			nprefs++;
		int k = sprintf(o, "%s %s pri %d sigwait %08lx\n", s, nd->ln_Name ? nd->ln_Name : "(none)",
		                (int)nd->ln_Pri, (unsigned long)t->tc_SigWait);
		o += k;
		left -= k;
		n += k;
	}
	return n;
}

int main(int argc, char **argv)
{
	LONG secs, seq = 0;
	BPTR fh;

	if (argc < 3)
		return 20;
	secs = atol(argv[2]);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
	LayersBase = OpenLibrary((STRPTR)"layers.library", 39);
	if (!IntuitionBase || !LayersBase)
		return 20;
	for (;;) {
		char *o = buf;
		int left = sizeof(buf), k;
		struct Screen *wb;
		int layers = -1;
		struct Task *me = FindTask(NULL);

		nprefs = 0;
		Disable();
		k = sprintf(o, "seq %ld\nrun %s %s\n", (long)++seq, me->tc_Node.ln_Name ? me->tc_Node.ln_Name : "?",
		            "(this)");
		o += k;
		left -= k;
		k = walk(o, left, &SysBase->TaskReady, "ready");
		o += k;
		left -= k;
		k = walk(o, left, &SysBase->TaskWait, "wait");
		o += k;
		left -= k;
		Enable();
		/* the Workbench screen's layers: can they be locked now? */
		if ((wb = LockPubScreen((STRPTR)"Workbench")) != NULL) {
			int i;
			layers = 0;
			for (i = 0; i < 20; i++) {           /* a busy layer lock passes quickly */
				if (AttemptSemaphore(&wb->LayerInfo.Lock)) {
					ReleaseSemaphore(&wb->LayerInfo.Lock);
					layers = 1;
					break;
				}
				Delay(5);
			}
			UnlockPubScreen(NULL, wb);
		}
		Forbid();
		k = sprintf(o, "riptideprefs port %s, tasks %d\n", FindPort((STRPTR)"RiptidePrefs") ? "yes" : "no",
		            nprefs);
		Permit();
		o += k;
		left -= k;
		k = sprintf(o, "layers %s\nfree chip %lu fast %lu\nend\n",
		            layers < 0 ? "no screen" : layers ? "free" : "LOCKED",
		            (unsigned long)AvailMem(MEMF_CHIP), (unsigned long)AvailMem(MEMF_FAST));
		o += k;
		if ((fh = Open((STRPTR)argv[1], MODE_NEWFILE)) != 0) {
			Write(fh, buf, o - buf);
			Close(fh);
		}
		Delay(secs * 50);
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
			break;
	}
	CloseLibrary(LayersBase);
	CloseLibrary((struct Library *)IntuitionBase);
	return 0;
}
