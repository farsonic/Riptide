/*
 * Riptide - the parts of intuition, gadtools, graphics and workbench that
 * RiptidePrefs uses, for the GCC build (vbcc uses the NDK). Layouts and
 * values are checked against the NDK by tests/amiga/check_layouts.sh.
 * Structures are declared up to the last field used. The call inlines
 * are build/.../gen/{intuition,gadtools,graphics}_calls.h.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_OS_INTUITION_H
#define RT_AMIGA_OS_INTUITION_H

#include "exec.h"

struct TextAttr {
	STRPTR ta_Name;
	UWORD  ta_YSize;
	UBYTE  ta_Style;
	UBYTE  ta_Flags;
};

struct RastPort {
	UBYTE rp_opaque[100];
};

struct Window;
struct ColorMap;

struct ViewPort {
	struct ViewPort *Next;
	struct ColorMap *ColorMap;
	UBYTE  vp_opaque[32];
};

struct Screen {
	struct Screen *NextScreen;
	struct Window *FirstWindow;
	WORD   LeftEdge, TopEdge, Width, Height;
	WORD   MouseY, MouseX;
	UWORD  Flags;
	UBYTE *Title;
	UBYTE *DefaultTitle;
	BYTE   BarHeight, BarVBorder, BarHBorder, MenuVBorder, MenuHBorder;
	BYTE   WBorTop, WBorLeft, WBorRight, WBorBottom;
	struct TextAttr *Font;
	struct ViewPort ViewPort;
	struct RastPort RastPort;
};

struct Window {
	struct Window *NextWindow;
	WORD   LeftEdge, TopEdge, Width, Height;
	WORD   MouseY, MouseX;
	WORD   MinWidth, MinHeight;
	UWORD  MaxWidth, MaxHeight;
	ULONG  Flags;
	APTR   MenuStrip;
	UBYTE *Title;
	APTR   FirstRequest, DMRequest;
	WORD   ReqCount;
	struct Screen *WScreen;
	struct RastPort *RPort;
	BYTE   BorderLeft, BorderTop, BorderRight, BorderBottom;
	struct RastPort *BorderRPort;
	struct Gadget *FirstGadget;
	struct Window *Parent, *Descendant;
	UWORD *Pointer;
	BYTE   PtrHeight, PtrWidth, XOffset, YOffset;
	ULONG  IDCMPFlags;
	struct MsgPort *UserPort;
};

struct IntuiMessage {
	struct Message ExecMessage;
	ULONG  Class;
	UWORD  Code;
	UWORD  Qualifier;
	APTR   IAddress;
	WORD   MouseX, MouseY;
	ULONG  Seconds, Micros;
	struct Window *IDCMPWindow;
	struct IntuiMessage *SpecialLink;
};

struct Gadget {
	struct Gadget *NextGadget;
	WORD   LeftEdge, TopEdge, Width, Height;
	UWORD  Flags, Activation, GadgetType;
	APTR   GadgetRender, SelectRender;
	APTR   GadgetText;
	LONG   MutualExclude;
	APTR   SpecialInfo;
	UWORD  GadgetID;
	APTR   UserData;
};

struct NewGadget {
	WORD   ng_LeftEdge, ng_TopEdge, ng_Width, ng_Height;
	CONST_STRPTR ng_GadgetText;
	struct TextAttr *ng_TextAttr;
	UWORD  ng_GadgetID;
	ULONG  ng_Flags;
	APTR   ng_VisualInfo;
	APTR   ng_UserData;
};

struct EasyStruct {
	ULONG  es_StructSize;
	ULONG  es_Flags;
	CONST_STRPTR es_Title;
	CONST_STRPTR es_TextFormat;
	CONST_STRPTR es_GadgetFormat;
};

struct WBArg {
	BPTR   wa_Lock;
	BYTE  *wa_Name;
};

struct WBStartup {
	struct Message sm_Message;
	struct MsgPort *sm_Process;
	BPTR   sm_Segment;
	LONG   sm_NumArgs;
	char  *sm_ToolWindow;
	struct WBArg *sm_ArgList;
};

#define GFLG_SELECTED      0x0080
#define GFLG_DISABLED      0x0100

#define IDCMP_REFRESHWINDOW 0x00000004UL
#define IDCMP_MOUSEBUTTONS  0x00000008UL
#define IDCMP_MOUSEMOVE     0x00000010UL
#define IDCMP_GADGETDOWN    0x00000020UL
#define IDCMP_GADGETUP      0x00000040UL
#define IDCMP_CLOSEWINDOW   0x00000200UL
#define IDCMP_RAWKEY        0x00000400UL
#define IDCMP_VANILLAKEY    0x00200000UL
#define IDCMP_INTUITICKS    0x00400000UL

#define WA_Left            0x80000064UL
#define WA_Top             0x80000065UL
#define WA_Width           0x80000066UL
#define WA_Height          0x80000067UL
#define WA_IDCMP           0x8000006aUL
#define WA_Gadgets         0x8000006cUL
#define WA_Title           0x8000006eUL
#define WA_InnerWidth      0x80000076UL
#define WA_InnerHeight     0x80000077UL
#define WA_PubScreen       0x80000079UL
#define WA_DragBar         0x80000082UL
#define WA_DepthGadget     0x80000083UL
#define WA_CloseGadget     0x80000084UL
#define WA_Activate        0x80000089UL
#define WA_SimpleRefresh   0x8000008cUL
#define WA_SmartRefresh    0x8000008dUL
#define WA_AutoAdjust      0x80000090UL
#define WA_NewLookMenus    0x80000093UL

#define GA_Disabled        0x8003000eUL

#define GT_TagBase         0x80080000UL
#define GTCB_Checked       (GT_TagBase + 4)
#define GTLV_Top           (GT_TagBase + 5)
#define GTLV_Labels        (GT_TagBase + 6)
#define GTTX_Text          (GT_TagBase + 11)
#define GTCY_Labels        (GT_TagBase + 14)
#define GTCY_Active        (GT_TagBase + 15)
#define GTST_String        (GT_TagBase + 45)
#define GTST_MaxChars      (GT_TagBase + 46)
#define GTIN_Number        (GT_TagBase + 47)
#define GTIN_MaxChars      (GT_TagBase + 48)
#define GTLV_ShowSelected  (GT_TagBase + 53)
#define GTLV_Selected      (GT_TagBase + 54)
#define GTTX_Border        (GT_TagBase + 57)
#define GT_Underscore      (GT_TagBase + 64)
#define GTCB_Scaled        (GT_TagBase + 68)
#define GTLV_MakeVisible   (GT_TagBase + 78)

#define BUTTON_KIND   1
#define CHECKBOX_KIND 2
#define INTEGER_KIND  3
#define LISTVIEW_KIND 4
#define CYCLE_KIND    7
#define STRING_KIND   12
#define TEXT_KIND     13

#define ARROWIDCMP    (IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_INTUITICKS | IDCMP_MOUSEBUTTONS)
#define BUTTONIDCMP   IDCMP_GADGETUP
#define CHECKBOXIDCMP IDCMP_GADGETUP
#define INTEGERIDCMP  IDCMP_GADGETUP
#define LISTVIEWIDCMP (IDCMP_GADGETUP | IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE | ARROWIDCMP)
#define CYCLEIDCMP    IDCMP_GADGETUP
#define STRINGIDCMP   IDCMP_GADGETUP
#define TEXTIDCMP     0UL

/* graphics: fonts, pens, drawing modes */
struct TextFont {
	struct Message tf_Message;
	UWORD  tf_YSize;
	UBYTE  tf_Style;
	UBYTE  tf_Flags;
	UWORD  tf_XSize;
	UWORD  tf_Baseline;
	UWORD  tf_BoldSmear;
};
#define JAM1            0
#define FSF_BOLD        0x02
#define OBP_Precision   0x84000000UL
#define PRECISION_GUI   32

/* BOOPSI (intuition/classes.h, classusr.h) */
typedef ULONG Object;
typedef struct { ULONG MethodID; } *Msg;
struct IClass {
	struct Hook cl_Dispatcher;
	ULONG  cl_Reserved;
	struct IClass *cl_Super;
	UBYTE *cl_ID;
	UWORD  cl_InstOffset;
	UWORD  cl_InstSize;
	ULONG  cl_UserData;
	ULONG  cl_SubclassCount;
	ULONG  cl_ObjectCount;
	ULONG  cl_Flags;
};
struct _Object {
	struct MinNode o_Node;
	struct IClass *o_Class;
};
#define OCLASS(o)       (((struct _Object *)(o) - 1)->o_Class)
#define INST_DATA(cl, o) ((void *)((UBYTE *)(o) + (cl)->cl_InstOffset))
struct opSet {
	ULONG  MethodID;
	struct TagItem *ops_AttrList;
	APTR   ops_GInfo;
};
struct opGet {
	ULONG  MethodID;
	ULONG  opg_AttrID;
	ULONG *opg_Storage;
};
#define OM_NEW          0x101
#define OM_DISPOSE      0x102
#define OM_SET          0x103
#define OM_GET          0x104
#define OM_ADDMEMBER    0x109
#define OM_REMMEMBER    0x10A

struct IBox {
	WORD   Left, Top, Width, Height;
};

/* menus (libraries/gadtools.h) */
struct NewMenu {
	UBYTE  nm_Type;
	CONST_STRPTR nm_Label;
	CONST_STRPTR nm_CommKey;
	UWORD  nm_Flags;
	LONG   nm_MutualExclude;
	APTR   nm_UserData;
};
#define NM_END          0
#define NM_TITLE        1
#define NM_ITEM         2
#define NM_BARLABEL     ((CONST_STRPTR)-1)
#define CHECKIT         0x0001
#define MENUTOGGLE      0x0008
#define CHECKED         0x0100

#define IDCMP_MENUPICK  0x00000100UL
#define IECODE_UP_PREFIX 0x80
#define IEQUALIFIER_LSHIFT 0x0001
#define IEQUALIFIER_RSHIFT 0x0002

#define PLACETEXT_LEFT  0x0001
#define PLACETEXT_RIGHT 0x0002
#define PLACETEXT_ABOVE 0x0004
#define PLACETEXT_IN    0x0010
#define NG_HIGHLABEL    0x0020

#endif
