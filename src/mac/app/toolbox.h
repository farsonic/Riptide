/* src/mac/app/toolbox.h - the classic Mac OS Toolbox for Riptide's applications: the structures,
 * constants and low-memory globals they use, and the calls (generated from traps.txt into
 * build/mac/gen/toolbox_gen.h; register-based ones in ostraps.S). Written for Riptide from Inside
 * Macintosh; nothing is copied from Apple's interfaces.
 *
 * It can be included next to the Amiga headers of the MUI editor (src/mac/mui): its names do not
 * collide with theirs (the Amiga prototypes of the Mac build leave out the Toolbox's names).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_TOOLBOX_H
#define RT_MAC_TOOLBOX_H

typedef struct { short top, left, bottom, right; } Rect;
typedef struct { short v, h; } Point;
typedef struct { unsigned short red, green, blue; } RGBColor;
typedef struct { short ascent, descent, widMax, leading; } FontInfo;
/* a Point passed by value: v in the high word */
static inline long pt_long(Point p) { return ((long)(unsigned short)p.v << 16) | (unsigned short)p.h; }
static inline long pt_hv(int h, int v) { return ((long)(unsigned short)v << 16) | (unsigned short)h; }

typedef struct {
	short what;
	long message, when;
	Point where;
	short modifiers;
} EventRecord;

enum { nullEvent, mouseDown, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt,
       osEvt = 15 };
#define everyEvent   0xFFFF
#define activeFlag   0x0001
#define btnState     0x0080
#define cmdKey       0x0100
#define shiftKey     0x0200
#define alphaLock    0x0400
#define optionKey    0x0800
#define controlKey   0x1000
#define charCodeMask 0xFF
#define keyCodeMask  0xFF00
#define suspendResumeMessage 1
#define mouseMovedMessage 0xFA
#define resumeFlag   1

enum { inDesk, inMenuBar, inSysWindow, inContent, inDrag, inGrow, inGoAway, inZoomIn, inZoomOut };
#define documentProc 0
#define dBoxProc     1
#define plainDBox    2
#define altDBoxProc  3
#define noGrowDocProc 4
#define movableDBoxProc 5
#define zoomDocProc  8
#define zoomNoGrow   12
#define rDocProc     16

/* fonts and styles */
#define systemFont   0
#define applFont     1
#define geneva       3
#define monaco       4
#define MAC_BOLD     1
#define srcOr        1
#define srcCopy      0
#define patCopy      8

/* the GrafPort fields read (Inside Macintosh I, QuickDraw) */
#define PORT_RECT(w)  ((Rect *)((char *)(w) + 16))
#define PORT_VISRGN(w) (*(void ***)((char *)(w) + 24))
/* a colour port (CGrafPort) has portVersion's top two bits set */
#define PORT_IS_COLOR(w) ((*(unsigned short *)((char *)(w) + 6) & 0xC000) == 0xC000)
/* the window record's refCon and visibility */
#define WIN_VISIBLE(w) (*((char *)(w) + 110))

/* TextEdit's record (Inside Macintosh I, TextEdit) */
typedef struct {
	Rect destRect, viewRect, selRect;
	short lineHeight, fontAscent;
	Point selPoint;
	short selStart, selEnd, active;
	long wordBreak, clikLoop, clickTime;
	short clickLoc;
	long caretTime;
	short caretState, just, teLength;
	char **hText;
	short recalBack, recalLines, clikStuff, crOnly, txFont;
	char txFace, filler;
	short txMode, txSize;
	void *inPort;
	long highHook, caretHook;
	short nLines;
	short lineStarts[1];
} TERec;
typedef TERec **TEHandle;
/* m68k GCC aligns every field to 2 bytes, as the Toolbox does */
_Static_assert(__builtin_offsetof(TERec, nLines) == 94, "TERec");
_Static_assert(sizeof(EventRecord) == 16, "EventRecord");

/* Standard File's System 7 reply (Inside Macintosh: Files) */
typedef struct {
	short vRefNum;
	long parID;
	unsigned char name[64];
} FSSpec;
typedef struct {
	char sfGood, sfReplacing;
	long sfType;
	FSSpec sfFile;
	short sfScript, sfFlags;
	char sfIsFolder, sfIsVolume;
	long sfReserved1;
	short sfReserved2;
} StandardFileReply;

/* low memory */
#define LM_TICKS      (*(volatile unsigned long *)0x16A)
#define LM_MMU32BIT   (*(volatile unsigned char *)0xCB2)
#define LM_GRAYRGN    (*(void ***)0x9EE)
#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
#define LM_SCREENBITS_BOUNDS(qd) ((Rect *)((char *)(qd) - 116))
#define LM_TESCRPLEN  (*(volatile short *)0xAB0)
#define LM_TESCRPH    (*(char ***)0xAB4)
#define LM_SYSVERSION (*(volatile short *)0x15A)
#define LM_DOUBLETIME (*(volatile unsigned long *)0x2F0)
#define LM_CARETTIME  (*(volatile unsigned long *)0x2F4)
/* a region's bounding box */
#define RGN_BBOX(h)   ((Rect *)((char *)*(h) + 2))

#define MAC_TYPE(a, b, c, d) (((long)(a) << 24) | ((long)(b) << 16) | ((long)(c) << 8) | (long)(d))

#include "gen/toolbox_gen.h"

/* ostraps.S */
void *mac_newptr(long size);
void  mac_disposeptr(void *p);
void **mac_newhandle(long size);
void  mac_disposehandle(void **h);
long  mac_gethandlesize(void **h);
void  mac_hlock(void **h);
void  mac_hunlock(void **h);
short mac_pb(void *pb, long trap);
long  mac_gestalt(long selector, long *response);
short mac_findfolder(int vref, long type, int create, short *foundvref, long *founddir);
void  mac_stdputfile(const unsigned char *prompt, const unsigned char *name, StandardFileReply *reply);
void  mac_stdgetfile(long ntypes, const long *types, StandardFileReply *reply);
/* a control's action procedure: calls the program's c_ctl_action(ctl, part) */
void  pas_ctl_action(void);
void  c_ctl_action(void *ctl, int part);

#define TRAP_OPEN     0xA000
#define TRAP_CLOSE    0xA001
#define TRAP_READ     0xA002
#define TRAP_WRITE    0xA003
#define TRAP_CONTROL  0xA004
#define TRAP_STATUS   0xA005
#define TRAP_GETEOF   0xA011
#define TRAP_SETEOF   0xA012
#define TRAP_SETFPOS  0xA044
#define TRAP_HOPEN    0xA200
#define TRAP_HCREATE  0xA208
#define TRAP_HDELETE  0xA209
#define TRAP_HGETFINFO 0xA20C
#define TRAP_HSETFINFO 0xA20D
#define TRAP_GETCATINFO 0x0009A260L

/* a C string as a Pascal string in buf (at most 255 characters); returns buf */
static inline unsigned char *mac_pstr(unsigned char *buf, const char *c)
{
	unsigned n = 0;
	while (c[n] && n < 255) { buf[n + 1] = (unsigned char)c[n]; n++; }
	buf[0] = (unsigned char)n;
	return buf;
}

#endif
