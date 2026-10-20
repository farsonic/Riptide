/*
 * Riptide - minimal AmigaOS dos.library definitions.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_DOS_H
#define RT_AMIGA_DOS_H

#include "exec.h"

#define MODE_OLDFILE   1005
#define MODE_NEWFILE   1006
#define MODE_READWRITE 1004

#define GVF_GLOBAL_ONLY (1UL << 8)
#define GVF_LOCAL_ONLY  (1UL << 9)
#define GVF_BINARY_VAR  (1UL << 10)

#define SYS_Dummy  (TAG_USER + 32)
#define SYS_Input  (SYS_Dummy + 1)
#define SYS_Output (SYS_Dummy + 2)
#define SYS_Asynch (SYS_Dummy + 3)

#define ERROR_REQUIRED_ARG_MISSING 116
#define ERROR_OBJECT_NOT_FOUND     205

struct DateStamp {
	LONG ds_Days;
	LONG ds_Minute;
	LONG ds_Tick;            /* 1/50 s */
};
#define TICKS_PER_SECOND 50

/* struct Process: the embedded Task and MsgPort plus the fields we use */
struct Process {
	struct Task    pr_Task;
	struct MsgPort pr_MsgPort;
	WORD  pr_Pad;
	BPTR  pr_SegList;
	LONG  pr_StackSize;
	APTR  pr_GlobVec;
	LONG  pr_TaskNum;
	BPTR  pr_StackBase;
	LONG  pr_Result2;
	BPTR  pr_CurrentDir;
	BPTR  pr_CIS;
	BPTR  pr_COS;
	APTR  pr_ConsoleTask;
	APTR  pr_FileSystemTask;
	BPTR  pr_CLI;
	APTR  pr_ReturnAddr;
	APTR  pr_PktWait;
	APTR  pr_WindowPtr;          /* (APTR)-1: no "insert volume" requesters */
};

/* struct RDArgs is only passed around as a pointer (ReadArgs/FreeArgs). */
struct RDArgs;

struct DosLibrary;
extern struct DosLibrary *DOSBase;

#endif
