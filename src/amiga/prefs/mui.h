/*
 * Riptide - the parts of MUI (muimaster.library 19, MUI 3.8) that
 * RiptidePrefs' MUI editor uses, written for Riptide: structures up to
 * the last field used, attribute, method and value numbers, the macros of
 * custom classes (_mleft() and friends) and small helpers (mui_util.c).
 * Nothing from the MUI SDK is copied here; tests/amiga/check_layouts.py
 * compares the numbers and layouts with the SDK when MUI_SDK= names it.
 * Both toolchains: vbcc takes the system structures from the NDK, the
 * GCC build from src/amiga/os.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_MUI_H
#define RT_PREFS_MUI_H

#if defined(__VBCC__)
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/screens.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <libraries/gadtools.h>
#include <utility/hooks.h>
#include <devices/inputevent.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#else
#include "os/intuition.h"
#if !defined(RT_MUI_NO_CALLS)          /* (tests/amiga/check_layouts.py) */
#include "gen/intuition_calls.h"
#include "gen/graphics_calls.h"
#endif
#endif
/* amiga.lib's BOOPSI calls (GCC build: mui_util.c) */
ULONG DoMethodA(Object *obj, Msg msg);
ULONG DoSuperMethodA(struct IClass *cl, Object *obj, Msg msg);
ULONG CoerceMethodA(struct IClass *cl, Object *obj, Msg msg);

/* ---- structures ---- */
struct MUI_GlobalInfo {
	ULONG  mgi_priv0;
	Object *mgi_ApplicationObject;
};

struct MUI_NotifyData {
	struct MUI_GlobalInfo *mnd_GlobalInfo;
	ULONG  mnd_UserData;
	ULONG  mnd_ObjectID;
	ULONG  mnd_priv[4];
};

struct MUI_MinMax {
	WORD   MinWidth, MinHeight;
	WORD   MaxWidth, MaxHeight;
	WORD   DefWidth, DefHeight;
};

struct MUI_RenderInfo {
	Object *mri_WindowObject;
	struct Screen *mri_Screen;
	APTR   mri_DrawInfo;
	UWORD *mri_Pens;
	struct Window *mri_Window;
	struct RastPort *mri_RastPort;
	ULONG  mri_Flags;
};

struct MUI_AreaData {
	struct MUI_RenderInfo *mad_RenderInfo;
	ULONG  mad_priv7;
	struct TextFont *mad_Font;
	struct MUI_MinMax mad_MinMax;
	struct IBox mad_Box;
	BYTE   mad_addleft, mad_addtop, mad_subwidth, mad_subheight;
	ULONG  mad_Flags;
};

/* the start of every MUI area object's instance data */
struct rt_mui_objhead {
	struct MUI_NotifyData mnd;
	struct MUI_AreaData mad;
};
#define muiAreaData(o)    (&((struct rt_mui_objhead *)(o))->mad)
#define muiRenderInfo(o)  (((struct rt_mui_objhead *)(o))->mad.mad_RenderInfo)
#define muiGlobalInfo(o)  (((struct rt_mui_objhead *)(o))->mnd.mnd_GlobalInfo)
#define _app(o)       (muiGlobalInfo(o)->mgi_ApplicationObject)
#define _win(o)       (muiRenderInfo(o)->mri_WindowObject)
#define _screen(o)    (muiRenderInfo(o)->mri_Screen)
#define _pens(o)      (muiRenderInfo(o)->mri_Pens)
#define _window(o)    (muiRenderInfo(o)->mri_Window)
#define _rp(o)        (muiRenderInfo(o)->mri_RastPort)
#define _font(o)      (muiAreaData(o)->mad_Font)
#define _left(o)      (muiAreaData(o)->mad_Box.Left)
#define _top(o)       (muiAreaData(o)->mad_Box.Top)
#define _width(o)     (muiAreaData(o)->mad_Box.Width)
#define _height(o)    (muiAreaData(o)->mad_Box.Height)
#define _mleft(o)     (_left(o) + muiAreaData(o)->mad_addleft)
#define _mtop(o)      (_top(o) + muiAreaData(o)->mad_addtop)
#define _mwidth(o)    (_width(o) - muiAreaData(o)->mad_subwidth)
#define _mheight(o)   (_height(o) - muiAreaData(o)->mad_subheight)
#define _mright(o)    (_mleft(o) + _mwidth(o) - 1)
#define _mbottom(o)   (_mtop(o) + _mheight(o) - 1)

struct MUI_CustomClass {
	APTR   mcc_UserData;
	struct Library *mcc_UtilityBase, *mcc_DOSBase, *mcc_GfxBase, *mcc_IntuitionBase;
	struct IClass *mcc_Super;
	struct IClass *mcc_Class;
};

struct MUI_EventHandlerNode {
	struct MinNode ehn_Node;
	BYTE   ehn_Reserved;
	BYTE   ehn_Priority;
	UWORD  ehn_Flags;
	Object *ehn_Object;
	struct IClass *ehn_Class;
	ULONG  ehn_Events;
};

/* asl.library's file requester, as MUI_AllocAslRequest() gives it */
struct rt_filereq {
	UBYTE  fr_Reserved0[4];
	STRPTR fr_File;
	STRPTR fr_Drawer;
};
#define RT_ASL_FileRequest    0
#define RT_ASLFR_TitleText    (TAG_USER + 0x80000 + 1)
#define RT_ASLFR_InitialFile  (TAG_USER + 0x80000 + 8)
#define RT_ASLFR_InitialDrawer (TAG_USER + 0x80000 + 9)
#define RT_ASLFR_DoSaveMode   (TAG_USER + 0x80000 + 44)

/* method messages */
struct MUIP_AskMinMax { ULONG MethodID; struct MUI_MinMax *MinMaxInfo; };
struct MUIP_Draw { ULONG MethodID; ULONG flags; };
struct MUIP_HandleEvent { ULONG MethodID; struct IntuiMessage *imsg; LONG muikey; };
struct MUIP_CreateShortHelp { ULONG MethodID; LONG mx; LONG my; };

/* ---- numbers ---- */
#define MUIC_Application  "Application.mui"
#define MUIC_Window       "Window.mui"
#define MUIC_Group        "Group.mui"
#define MUIC_Area         "Area.mui"
#define MUIC_String       "String.mui"
#define MUIC_Cycle        "Cycle.mui"
#define MUIC_List         "List.mui"
#define MUIC_Listview     "Listview.mui"
#define MUIC_Text         "Text.mui"
#define MUIC_Rectangle    "Rectangle.mui"
#define MUIC_Image        "Image.mui"

#define MUIA_Application_Title       0x804281b8UL
#define MUIA_Application_Version     0x8042b33fUL
#define MUIA_Application_Copyright   0x8042ef4dUL
#define MUIA_Application_Author      0x80424842UL
#define MUIA_Application_Description 0x80421fc6UL
#define MUIA_Application_Base        0x8042e07aUL
#define MUIA_Application_Window      0x8042bfe0UL
#define MUIA_Application_Sleep       0x80425711UL
#define MUIA_Application_UseCommodities 0x80425ee5UL
#define MUIA_Application_UseRexx     0x80422387UL
#define MUIA_Application_SingleTask  0x8042a2c8UL
#define MUIA_Application_DoubleStart 0x80423bc6UL
#define MUIA_Application_Iconified   0x8042a07fUL
#define MUIA_Window_Title            0x8042ad3dUL
#define MUIA_Window_ID               0x804201bdUL
#define MUIA_Window_RootObject       0x8042cba5UL
#define MUIA_Window_Open             0x80428aa0UL
#define MUIA_Window_CloseRequest     0x8042e86eUL
#define MUIA_Window_Menustrip        0x8042855eUL
#define MUIA_Window_PublicScreen     0x804278e4UL
#define MUIA_Window_ActiveObject     0x80427925UL
#define MUIA_Window_Window           0x80426a42UL
#define MUIA_Window_Screen           0x8042df4fUL
#define MUIA_Window_ScreenTitle      0x804234b0UL
#define MUIA_Window_LeftEdge         0x80426c65UL
#define MUIA_Window_TopEdge          0x80427c66UL
#define MUIA_Window_Sleep            0x8042e7dbUL
#define MUIA_Window_RefWindow        0x804201f4UL
#define MUIA_Window_Activate         0x80428d2fUL
#define MUIA_Group_Child             0x804226e6UL
#define MUIA_Group_Horiz             0x8042536bUL
#define MUIA_Group_Columns           0x8042f416UL
#define MUIA_Group_PageMode          0x80421a5fUL
#define MUIA_Group_ActivePage        0x80424199UL
#define MUIA_Group_Spacing           0x8042866dUL
#define MUIA_Group_HorizSpacing      0x8042c651UL
#define MUIA_Group_VertSpacing       0x8042e1bfUL
#define MUIA_Group_SameSize          0x80420860UL
#define MUIA_InnerLeft               0x804228f8UL
#define MUIA_InnerRight              0x804297ffUL
#define MUIA_InnerTop                0x80421eb6UL
#define MUIA_InnerBottom             0x8042f2c0UL
#define MUIA_HorizWeight             0x80426db9UL
#define MUIA_VertWeight              0x804298d0UL
#define MUIA_Frame                   0x8042ac64UL
#define MUIA_Background              0x8042545bUL
#define MUIA_ShortHelp               0x80428fe3UL
#define MUIA_ControlChar             0x8042120bUL
#define MUIA_CycleChain              0x80421ce7UL
#define MUIA_Disabled                0x80423661UL
#define MUIA_FillArea                0x804294a3UL
#define MUIA_InputMode               0x8042fb04UL
#define MUIA_Selected                0x8042654bUL
#define MUIA_Pressed                 0x80423535UL
#define MUIA_FixHeight               0x8042a92bUL
#define MUIA_UserData                0x80420313UL
#define MUIA_NoNotify                0x804237f9UL
#define MUIA_String_Contents         0x80428ffdUL
#define MUIA_String_MaxLen           0x80424984UL
#define MUIA_String_Acknowledge      0x8042026cUL
#define MUIA_Cycle_Entries           0x80420629UL
#define MUIA_Cycle_Active            0x80421788UL
#define MUIA_List_ConstructHook      0x8042894fUL
#define MUIA_List_DestructHook       0x804297ceUL
#define MUIA_List_Active             0x8042391cUL
#define MUIA_List_Quiet              0x8042d8c7UL
#define MUIA_Listview_List           0x8042bcceUL
#define MUIA_Menuitem_Checked        0x8042562aUL
#define MUIA_Menuitem_Enabled        0x8042ae0fUL

#define MUIM_Application_NewInput    0x80423ba6UL
#define MUIM_Application_ReturnID    0x804276efUL
#define MUIM_Application_PushMethod  0x80429ef8UL
#define MUIM_Notify                  0x8042c9cbUL
#define MUIM_Set                     0x8042549aUL
#define MUIM_NoNotifySet             0x8042216fUL
#define MUIM_List_Clear              0x8042ad89UL
#define MUIM_List_InsertSingle       0x804254d5UL
#define MUIM_List_Jump               0x8042baabUL
#define MUIM_FindUData               0x8042c196UL
#define MUIM_Window_AddEventHandler  0x804203b7UL
#define MUIM_Window_RemEventHandler  0x8042679eUL
#define MUIM_Window_ToFront          0x8042554fUL
#define MUIM_Window_ScreenToFront    0x804227a4UL
#define MUIM_Setup                   0x80428354UL
#define MUIM_Cleanup                 0x8042d985UL
#define MUIM_AskMinMax               0x80423874UL
#define MUIM_Draw                    0x80426f3fUL
#define MUIM_Show                    0x8042cc84UL
#define MUIM_Hide                    0x8042f20fUL
#define MUIM_HandleEvent             0x80426d66UL
#define MUIM_CreateShortHelp         0x80428e93UL
#define MUIM_DeleteShortHelp         0x8042d35aUL
#define MUIM_Group_InitChange        0x80420887UL
#define MUIM_Group_ExitChange        0x8042d1ccUL

#define MUIV_EveryTime               0x49893131UL
#define MUIV_TriggerValue            0x49893131UL
#define MUIV_NotTriggerValue         0x49893133UL
#define MUIV_Application_ReturnID_Quit (-1L)
#define MUIV_Window_LeftEdge_Centered (-1L)
#define MUIV_Window_TopEdge_Centered (-1L)
#define MUIV_Frame_None              0
#define MUIV_Frame_String            4
#define MUIV_Frame_InputList         6
#define MUIV_InputMode_RelVerify     1
#define MUIV_InputMode_Toggle        3
#define MUIV_Frame_ImageButton       2
#define MUII_ButtonBack              2
#define MUII_CheckMark               15
#define MUIA_ShowSelState            0x8042caacUL
#define MUIA_Image_Spec              0x804233d5UL
#define MUIA_Image_FreeVert          0x8042ea28UL
#define MUIV_List_Active_Off         (-1L)
#define MUIV_List_Insert_Bottom      (-3L)
#define MUIV_List_ConstructHook_String (-1L)
#define MUIV_List_DestructHook_String (-1L)

#define MUIO_Checkmark               3
#define MUIO_Cycle                   4
#define MUIO_String                  7
#define MUIO_MenustripNM             13

#define MUI_EventHandlerRC_Eat       1
#define MUI_MAXMAX                   10000
#define MUIE_SingleTask              6           /* MUI_Error(): another copy is running */
#define MADF_DRAWOBJECT              1
#define MADF_DRAWUPDATE              2
#define MPEN_SHINE                   0
#define MPEN_HALFSHINE               1
#define MPEN_BACKGROUND              2
#define MPEN_HALFSHADOW              3
#define MPEN_SHADOW                  4
#define MPEN_TEXT                    5
#define MPEN_FILL                    6
#define MPEN_MARK                    7
#define MUIKEY_PRESS                 0
#define MUIKEY_UP                    2
#define MUIKEY_DOWN                  3

/* ---- helpers (mui_util.c) ---- */
extern struct Library *MUIMasterBase;
#if !defined(RT_MUI_NO_CALLS)
#include "gen/muimaster_calls.h"
#endif

#define TAGEND 0UL
/* MUI_NewObjectA() of class cls with the tags that follow (TAGEND last) */
Object *mui_new(const char *cls, ULONG tag, ...);
/* NewObjectA() of a custom class */
Object *mui_newc(struct MUI_CustomClass *mcc, ULONG tag, ...);
/* MUI_MakeObjectA() */
Object *mui_make(LONG type, ULONG p1, ...);
/* DoMethodA() with the message on the stack */
ULONG mui_do(Object *obj, ULONG id, ...);
void  mui_set(Object *obj, ULONG attr, ULONG v);
void  mui_nnset(Object *obj, ULONG attr, ULONG v);    /* without notifying */
ULONG mui_get(Object *obj, ULONG attr);

/* A custom class dispatcher: ULONG name(cl, obj, msg), called by BOOPSI
 * with them in a0, a2 and a1. DISPATCHER_REF(name) is what
 * MUI_CreateCustomClass() wants (GCC: the register thunk of mui_thunks.S). */
#if defined(__VBCC__)
#define DISPATCHER(n) static ULONG n(__reg("a0") struct IClass *cl, __reg("a2") Object *obj, __reg("a1") Msg msg)
#define DISPATCHER_REF(n) ((APTR)(n))
#else
#define DISPATCHER(n) extern char n##_thunk[]; ULONG n(struct IClass *cl, Object *obj, Msg msg); \
	ULONG n(struct IClass *cl, Object *obj, Msg msg)
#define DISPATCHER_REF(n) ((APTR)(n##_thunk))
#endif

#endif
