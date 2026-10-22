/*
 * Riptide - minimal AmigaOS exec definitions (written from the documented
 * structure layouts; verified by tests/amiga/check_layouts.sh).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_EXEC_H
#define RT_AMIGA_EXEC_H

#include <stdint.h>
#include <stddef.h>

typedef void           *APTR;
typedef int32_t         LONG;
typedef uint32_t        ULONG;
typedef int16_t         WORD;
typedef uint16_t        UWORD;
typedef int8_t          BYTE;
typedef uint8_t         UBYTE;
typedef char           *STRPTR;
typedef const char     *CONST_STRPTR;
typedef LONG            BPTR;       /* BCPL pointer (address >> 2) */
typedef LONG            BOOL;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#define BADDR(b) ((APTR)((ULONG)(b) << 2))
#define MKBADDR(a) ((BPTR)((ULONG)(a) >> 2))

/* ---- nodes and lists ---- */
struct Node {
	struct Node *ln_Succ;
	struct Node *ln_Pred;
	UBYTE ln_Type;
	BYTE  ln_Pri;
	char *ln_Name;
};
struct MinNode {
	struct MinNode *mln_Succ;
	struct MinNode *mln_Pred;
};
struct List {
	struct Node *lh_Head;
	struct Node *lh_Tail;
	struct Node *lh_TailPred;
	UBYTE lh_Type;
	UBYTE l_pad;
};
struct MinList {
	struct MinNode *mlh_Head;
	struct MinNode *mlh_Tail;
	struct MinNode *mlh_TailPred;
};
static inline void NewList(struct List *l)
{
	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}
static inline void NewMinList(struct MinList *l)
{
	l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
	l->mlh_Tail = NULL;
	l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

#define NT_UNKNOWN    0
#define NT_TASK       1
#define NT_DEVICE     3
#define NT_MSGPORT    4
#define NT_MESSAGE    5
#define NT_FREEMSG    6
#define NT_REPLYMSG   7
#define NT_RESOURCE   8
#define NT_LIBRARY    9
#define NT_PROCESS    13
#define NT_SIGNALSEM  15

/* ---- libraries ---- */
struct Library {
	struct Node lib_Node;
	UBYTE lib_Flags;
	UBYTE lib_pad;
	UWORD lib_NegSize;
	UWORD lib_PosSize;
	UWORD lib_Version;
	UWORD lib_Revision;
	APTR  lib_IdString;
	ULONG lib_Sum;
	UWORD lib_OpenCnt;
};
#define LIBF_SUMMING  (1 << 0)
#define LIBF_CHANGED  (1 << 1)
#define LIBF_SUMUSED  (1 << 2)
#define LIBF_DELEXP   (1 << 3)

/* ---- tasks ---- */
struct Task {
	struct Node tc_Node;
	UBYTE tc_Flags;
	UBYTE tc_State;
	BYTE  tc_IDNestCnt;
	BYTE  tc_TDNestCnt;
	ULONG tc_SigAlloc;
	ULONG tc_SigWait;
	ULONG tc_SigRecvd;
	ULONG tc_SigExcept;
	UWORD tc_TrapAlloc;
	UWORD tc_TrapAble;
	APTR  tc_ExceptData;
	APTR  tc_ExceptCode;
	APTR  tc_TrapData;
	APTR  tc_TrapCode;
	APTR  tc_SPReg;
	APTR  tc_SPLower;
	APTR  tc_SPUpper;
	void (*tc_Switch)(void);
	void (*tc_Launch)(void);
	struct List tc_MemEntry;
	APTR  tc_UserData;
};

#define SIGBREAKB_CTRL_C 12
#define SIGBREAKB_CTRL_D 13
#define SIGBREAKB_CTRL_E 14
#define SIGBREAKB_CTRL_F 15
#define SIGBREAKF_CTRL_C (1UL << SIGBREAKB_CTRL_C)
#define SIGBREAKF_CTRL_D (1UL << SIGBREAKB_CTRL_D)
#define SIGBREAKF_CTRL_E (1UL << SIGBREAKB_CTRL_E)
#define SIGBREAKF_CTRL_F (1UL << SIGBREAKB_CTRL_F)
#define SIGF_SINGLE      (1UL << 4)

/* ---- message ports ---- */
struct MsgPort {
	struct Node mp_Node;
	UBYTE mp_Flags;
	UBYTE mp_SigBit;
	APTR  mp_SigTask;
	struct List mp_MsgList;
};
#define PA_SIGNAL  0
#define PA_SOFTINT 1
#define PA_IGNORE  2

struct Message {
	struct Node mn_Node;
	struct MsgPort *mn_ReplyPort;
	UWORD mn_Length;
};

/* ---- semaphores (layout is fixed by the OS; we treat it as opaque) ---- */
struct SemaphoreRequest {
	struct MinNode sr_Link;
	struct Task *sr_Waiter;
};
struct SignalSemaphore {
	struct Node ss_Link;
	WORD  ss_NestCount;
	struct MinList ss_WaitQueue;
	struct SemaphoreRequest ss_MultipleLink;
	struct Task *ss_Owner;
	WORD  ss_QueueCount;
};

/* ---- I/O ---- */
struct Device {
	struct Library dd_Library;
};
struct Unit {
	struct MsgPort unit_MsgPort;
	UBYTE unit_flags;
	UBYTE unit_pad;
	UWORD unit_OpenCnt;
};
struct IORequest {
	struct Message io_Message;
	struct Device *io_Device;
	struct Unit *io_Unit;
	UWORD io_Command;
	UBYTE io_Flags;
	BYTE  io_Error;
};
struct IOStdReq {
	struct Message io_Message;
	struct Device *io_Device;
	struct Unit *io_Unit;
	UWORD io_Command;
	UBYTE io_Flags;
	BYTE  io_Error;
	ULONG io_Actual;
	ULONG io_Length;
	APTR  io_Data;
	ULONG io_Offset;
};
#define IOB_QUICK 0
#define IOF_QUICK (1 << 0)
#define CMD_INVALID 0
#define CMD_RESET   1
#define CMD_READ    2
#define CMD_WRITE   3
#define CMD_UPDATE  4
#define CMD_CLEAR   5
#define CMD_STOP    6
#define CMD_START   7
#define CMD_FLUSH   8
#define CMD_NONSTD  9
#define IOERR_OPENFAIL   (-1)
#define IOERR_ABORTED    (-2)
#define IOERR_NOCMD      (-3)
#define IOERR_BADLENGTH  (-4)
#define IOERR_BADADDRESS (-5)
#define IOERR_UNITBUSY   (-6)
#define IOERR_SELFTEST   (-7)

/* ---- memory ---- */
#define MEMF_ANY     0
#define MEMF_PUBLIC  (1UL << 0)
#define MEMF_CHIP    (1UL << 1)
#define MEMF_FAST    (1UL << 2)
#define MEMF_CLEAR   (1UL << 16)

/* ---- ExecBase: only what we use, by offset ---- */
struct ExecBase;
#define EXECBASE_LIBLIST_OFS   378   /* struct List LibList */
#define EXECBASE_PORTLIST_OFS  392   /* struct List PortList */
#define EXECBASE_THISTASK_OFS  276   /* struct Task *ThisTask */
#define EXECBASE_ATTNFLAGS_OFS 296   /* UWORD AttnFlags */
#define EXECBASE_ECLOCK_OFS    568   /* ULONG ex_EClockFrequency */
static inline struct List *exec_liblist(struct ExecBase *eb)
{
	return (struct List *)((UBYTE *)eb + EXECBASE_LIBLIST_OFS);
}

/* ---- tag items (utility/tagitem.h) ---- */
typedef ULONG Tag;
struct TagItem {
	Tag   ti_Tag;
	ULONG ti_Data;
};
#define TAG_DONE   0UL
#define TAG_END    0UL
#define TAG_IGNORE 1UL
#define TAG_MORE   2UL
#define TAG_SKIP   3UL
#define TAG_USER   (1UL << 31)

/* ---- hooks (utility/hooks.h) ---- */
struct Hook {
	struct MinNode h_MinNode;
	ULONG (*h_Entry)(void);
	ULONG (*h_SubEntry)(void);
	APTR h_Data;
};

extern struct ExecBase *SysBase;

#endif
