/*
 * Riptide test helper - taskprobe FILE SECS [TASKNAME]: every SECS seconds
 * write a snapshot of one task to FILE.0 .. FILE.9 (a ring; "seq N" on the
 * first line orders them). The task is Riptide's stack process (the
 * RIPTIDE port's mp_SigTask) or the task named TASKNAME. A snapshot holds
 * the task's state, signals, priority and nesting counts, its saved stack
 * (return addresses for a symbol lookup), exec's TaskReady and TaskWait
 * lists, and whether the task is in either: a task in state TS_READY that
 * is in neither list has been orphaned and never runs again.
 *
 * Found that way: under load, the AROS ROM on FS-UAE 3.2.35 sometimes
 * loses a task that a Signal() made ready (tests/amiga/execrace.c shows it
 * without Riptide); run_fsuae_bsdsocktest.sh runs this probe to tell such
 * a stall from a Riptide bug.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct ExecBase *SysBase;

static char lists[4096];

/* the ready and wait lists (name, priority, state); under Disable() */
static void walk(struct Task *t, int *inready, int *inwait)
{
	struct Node *nd;
	char *o = lists;
	struct Task *me = SysBase->ThisTask;

	*inready = *inwait = 0;
	Disable();
	o += sprintf(o, "this %p '%.20s' pri %d\nready:", (void *)me,
	             me->tc_Node.ln_Name ? me->tc_Node.ln_Name : "?", (int)me->tc_Node.ln_Pri);
	for (nd = SysBase->TaskReady.lh_Head; nd->ln_Succ && o < lists + 1800; nd = nd->ln_Succ) {
		if ((struct Task *)nd == t)
			*inready = 1;
		o += sprintf(o, " [%p '%.16s' %d st%d]", (void *)nd, nd->ln_Name ? nd->ln_Name : "?",
		             (int)nd->ln_Pri, (int)((struct Task *)nd)->tc_State);
	}
	o += sprintf(o, "\nwait:");
	for (nd = SysBase->TaskWait.lh_Head; nd->ln_Succ && o < lists + 3800; nd = nd->ln_Succ) {
		if ((struct Task *)nd == t)
			*inwait = 1;
		o += sprintf(o, " [%p '%.16s' %d st%d w%08lx]", (void *)nd, nd->ln_Name ? nd->ln_Name : "?",
		             (int)nd->ln_Pri, (int)((struct Task *)nd)->tc_State,
		             (unsigned long)((struct Task *)nd)->tc_SigWait);
	}
	Enable();
	*o++ = '\n';
	*o = 0;
}

int main(int argc, char **argv)
{
	char name[128];
	long secs = argc > 2 ? atol(argv[2]) : 3, n = 0;

	if (argc < 2)
		return 20;
	for (;;) {
		struct MsgPort *p;
		struct Task *t;
		ULONG st[96], sw = 0, sr = 0, sa = 0, sp = 0, lo = 0, hi = 0;
		int i, k = 0, state = 0, pri = 0, idn = 0, tdn = 0, inready = 0, inwait = 0;
		char tn[40] = "?";
		FILE *f;

		Forbid();
		if (argc > 3) {
			t = FindTask((STRPTR)argv[3]);
		} else {
			p = FindPort((STRPTR)"RIPTIDE");
			t = p ? p->mp_SigTask : NULL;
		}
		if (t) {
			state = t->tc_State;
			sw = t->tc_SigWait;
			sr = t->tc_SigRecvd;
			sa = t->tc_SigAlloc;
			sp = (ULONG)t->tc_SPReg;
			lo = (ULONG)t->tc_SPLower;
			hi = (ULONG)t->tc_SPUpper;
			pri = t->tc_Node.ln_Pri;
			idn = t->tc_IDNestCnt;
			tdn = t->tc_TDNestCnt;
			if (t->tc_Node.ln_Name)
				strncpy(tn, t->tc_Node.ln_Name, sizeof(tn) - 1);
			for (i = 0; i < 96 && sp + 4 * i < hi; i++)
				st[k++] = ((ULONG *)sp)[i];
		}
		walk(t, &inready, &inwait);
		Permit();
		sprintf(name, "%s.%ld", argv[1], n % 10);
		f = fopen(name, "w");
		if (f) {
			fprintf(f, "seq %ld task %p '%s' state %d sigwait %08lx sigrecvd %08lx sigalloc %08lx "
			        "sp %08lx lower %08lx upper %08lx pri %d idnest %d tdnest %d\n",
			        n, (void *)t, tn, state, (unsigned long)sw, (unsigned long)sr, (unsigned long)sa,
			        (unsigned long)sp, (unsigned long)lo, (unsigned long)hi, pri, idn, tdn);
			fputs(lists, f);
			fprintf(f, "in ready list %d, in wait list %d, succ %p pred %p%s\n", inready, inwait,
			        t ? (void *)t->tc_Node.ln_Succ : NULL, t ? (void *)t->tc_Node.ln_Pred : NULL,
			        t && t != SysBase->ThisTask && !inready && !inwait ? " ORPHANED" : "");
			for (i = 0; i < k; i++)
				fprintf(f, "%08lx%c", (unsigned long)st[i], (i % 8 == 7) ? '\n' : ' ');
			fprintf(f, "\n");
			fclose(f);
		}
		n++;
		Delay(secs * 50);
	}
	return 0;
}
