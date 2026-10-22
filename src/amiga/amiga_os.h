/*
 * Riptide - AmigaOS portability layer of the glue code. Two toolchains:
 *
 *  vbcc (primary): real NDK 3.2 headers and protos; library functions
 *      take register arguments declared with __reg().
 *  m68k-linux-gnu-gcc (secondary): our own minimal OS headers (src/amiga/os)
 *      and generated jump-table inlines; library vectors go through
 *      generated register->stack trampolines.
 *
 * RTREG(d0) marks a register parameter of a function that is called
 * directly by AmigaOS code (library vectors, SANA-II callbacks).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_OS_H
#define RT_AMIGA_OS_H

#if defined(__VBCC__)

#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/semaphores.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/devices.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dosasl.h>
#include <dos/var.h>
#include <dos/rdargs.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include "os/sana2.h"

#define RTREG(r) __reg(#r)
#define exec_liblist(eb) (&(eb)->LibList)
#ifndef NewList
static inline void rt_newlist(struct List *l)
{
	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}
#define NewList rt_newlist
#endif
#define RT_AMIGA_ECLOCKFREQ(eb) ((eb)->ex_EClockFrequency)
#define RT_AMIGA_ATTNFLAGS(eb) ((eb)->AttnFlags)
#define RT_AMIGA_THISTASK(eb) ((eb)->ThisTask)

#else /* GCC */

#include "os/amiga.h"
#define RTREG(r)
#define RT_AMIGA_ECLOCKFREQ(eb) (*(ULONG *)((UBYTE *)(eb) + EXECBASE_ECLOCK_OFS))
#define RT_AMIGA_ATTNFLAGS(eb) (*(UWORD *)((UBYTE *)(eb) + EXECBASE_ATTNFLAGS_OFS))
#define RT_AMIGA_THISTASK(eb) (*(struct Task *volatile *)((UBYTE *)(eb) + EXECBASE_THISTASK_OFS))

#endif

/* exec's NewMinList() needs Kickstart 3.2: use our own */
static inline void rt_newminlist(struct MinList *l)
{
	l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
	l->mlh_Tail = NULL;
	l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

#endif
