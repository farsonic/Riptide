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

#define OFFSET_BEGINNING -1
#define OFFSET_CURRENT   0
#define OFFSET_END       1
#define SHARED_LOCK      -2
#define ACCESS_READ      -2
#define EXCLUSIVE_LOCK   -1
#define ACCESS_WRITE     -1

#define DOSTRUE  -1
#define DOSFALSE 0

#define GVF_GLOBAL_ONLY (1UL << 8)
#define GVF_LOCAL_ONLY  (1UL << 9)
#define GVF_BINARY_VAR  (1UL << 10)

#define SYS_Dummy  (TAG_USER + 32)
#define SYS_Input  (SYS_Dummy + 1)
#define SYS_Output (SYS_Dummy + 2)
#define SYS_Asynch (SYS_Dummy + 3)

#define ERROR_REQUIRED_ARG_MISSING 116
#define ERROR_OBJECT_NOT_FOUND     205
#define ERROR_DISK_FULL            221
#define ERROR_NO_MORE_ENTRIES      232
#define ERROR_BREAK                304

struct DateStamp {
	LONG ds_Days;
	LONG ds_Minute;
	LONG ds_Tick;            /* 1/50 s */
};
#define TICKS_PER_SECOND 50

/* Examine()/ExNext(); must be longword aligned (AllocVec it) */
struct FileInfoBlock {
	LONG  fib_DiskKey;
	LONG  fib_DirEntryType;  /* < 0 file, > 0 directory */
	char  fib_FileName[108];
	LONG  fib_Protection;
	LONG  fib_EntryType;
	LONG  fib_Size;
	LONG  fib_NumBlocks;
	struct DateStamp fib_Date;
	char  fib_Comment[80];
	UWORD fib_OwnerUID;
	UWORD fib_OwnerGID;
	UBYTE fib_Reserved[32];
};

/* MatchFirst()/MatchNext() (dos/dosasl.h) */
struct AChain;
struct AnchorPath {
	struct AChain *ap_Base;
	struct AChain *ap_Last;
	LONG  ap_BreakBits;
	LONG  ap_FoundBreak;
	BYTE  ap_Flags;
	BYTE  ap_Reserved;
	WORD  ap_Strlen;          /* size of ap_Buf; 0: no full path */
	struct FileInfoBlock ap_Info;
	char  ap_Buf[1];
};

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
