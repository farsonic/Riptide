/*
 * Riptide - the MUI of the classic Mac OS build of RiptidePrefs (src/mac/mui): the part of
 * MUI 3.8 that the MUI editor uses (src/amiga/prefs/mui.h lists it), written on the Mac Toolbox,
 * so the editor's own files (mui_main.c, mui_classes.c, mui_status.c, mui_counters.c,
 * mui_theme.c, mui_util.c, mui_thunks.S) compile for the Mac unchanged. See docs/MAC-GUI.md.
 *
 *   mm_boopsi.c   classes, objects, DoMethod and friends, the Notify class (notifications)
 *   mm_app.c      Application and Window: the event loop, windows, menus, help bubbles
 *   mm_area.c     Area (frames, backgrounds, input modes), Rectangle, Image (check marks), Text
 *   mm_group.c    Group: MUI's layout (rows, columns, pages, weights, spacing)
 *   mm_gadgets.c  String (TextEdit), Cycle (pop-up menus), List and Listview
 *   mm_gfx.c      graphics.library on QuickDraw: rastports, pens, fonts
 *   mm_os.c       the exec, dos and intuition calls the editor makes
 *   mm_thunks.S   the built-in classes' dispatchers in BOOPSI's registers
 *
 * Coordinates are the Mac window's content coordinates, which is what MUI's _mleft() and an
 * IntuiMessage's MouseX/MouseY are measured in on the Amiga too (the window's top left).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_MM_H
#define RT_MAC_MM_H

#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui.h"
#include "toolbox.h"

/* ---- objects ---- */
/* Each object is [struct mm_obj][struct _Object][instance data]; Object * points at the
 * instance data, as BOOPSI's does. mm_obj holds what every object of this MUI keeps. */
struct mm_notify {
	struct mm_notify *next;
	ULONG attr, val;
	Object *dest;
	ULONG n;
	ULONG args[8];
};
struct mm_win;
struct mm_obj {
	ULONG magic;
	struct IClass *cls;             /* the object's true class (also in struct _Object) */
	Object *parent, *child, *next;  /* groups, windows of an application, the menu strip */
	struct mm_notify *notify;
	struct mm_win *win;             /* between MUIM_Setup and MUIM_Cleanup */
	ULONG udata;
	/* Area */
	UWORD flags;
	UWORD frame;
	WORD hweight, vweight;
	BYTE in_l, in_r, in_t, in_b;    /* MUIA_Inner* (-1: the frame's default) */
	UBYTE inmode, cchar;
	UBYTE disabled, selected, pressed, cycle;
	UBYTE fixh_lines, showsel;
	ULONG bg;                       /* 0x01RRGGBB when set, else 0 */
	const char *help;
	WORD fixh;
};
#define MM_MAGIC 0x4D554921UL           /* 'MUI!' */
#define MMO(o) ((struct mm_obj *)((struct _Object *)(o) - 1) - 1)
/* flags */
#define MMF_AREA     0x0001             /* an Area (has a box, is drawn) */
#define MMF_SHOWN    0x0002             /* between MUIM_Show and MUIM_Hide */
#define MMF_SETUP    0x0004
#define MMF_GROUP    0x0008
#define MMF_STRING   0x0010
#define MMF_INPUT    0x0020             /* takes clicks itself (String, Cycle, List, Image) */
#define MMF_NODRAWBG 0x0040

/* built-in classes */
extern struct IClass *mm_cl_notify, *mm_cl_app, *mm_cl_win, *mm_cl_area, *mm_cl_group, *mm_cl_rect,
	*mm_cl_image, *mm_cl_text, *mm_cl_string, *mm_cl_cycle, *mm_cl_list, *mm_cl_listview,
	*mm_cl_menustrip, *mm_cl_menuitem;
void mm_classes_init(void);
struct IClass *mm_make_class(struct IClass *super, ULONG datasize, APTR dispatcher_thunk);
/* the tag's value in a tag list, or def; 1 when present (mm_tag_find) */
ULONG mm_tag(const struct TagItem *t, ULONG tag, ULONG def);
int   mm_tag_find(const struct TagItem *t, ULONG tag, ULONG *v);
/* fire the notifications of attribute attr (now val) of obj */
void  mm_notify_fire(Object *obj, ULONG attr, ULONG val);
void  mm_set_quiet(Object *obj, ULONG attr, ULONG v);       /* SetAttrs without notifying */
/* SetAttrsA with the tags that follow (TAG_DONE last) */
ULONG mm_setattrs(Object *obj, ULONG tag, ...);
/* input on an Area with an input mode (mm_area.c) */
void  mm_area_click(Object *obj, int x, int y);
void  mm_area_key(Object *obj);

/* ---- windows and the application ---- */
#define MM_MAXHANDLERS 64
struct mm_win {
	Object *obj;                    /* the Window object */
	Object *root;
	void *macwin;                   /* the Mac window (WindowPtr) */
	struct MUI_RenderInfo ri;
	struct Window awin;             /* the Amiga window the classes see */
	struct RastPort *rp;
	struct MUI_EventHandlerNode *eh[MM_MAXHANDLERS];
	int neh;
	Object *active;                 /* the String gadget being typed in */
	int sleep;
	int w, h;                       /* the content's size */
	int needs_layout;
	int resizable;                  /* a grow box and a zoom box (MUI: MaxWidth > MinWidth, or heights) */
};
extern Object *mm_app;
extern struct Screen mm_screen;
/* the window of an object, NULL before Setup */
static inline struct mm_win *mm_win_of(Object *o) { return MMO(o)->win; }
void mm_app_returnid(ULONG id);
void mm_win_layout(struct mm_win *w);            /* AskMinMax, layout, redraw */
void mm_win_redraw(struct mm_win *w);
void mm_win_activate_string(struct mm_win *w, Object *s);
/* the object box, inclusive */
void mm_box(Object *o, Rect *r);
/* recursive helpers over the tree */
void mm_children_do(Object *o, Msg msg);         /* send msg to each child */
Object *mm_hit(Object *o, int x, int y, ULONG need); /* the deepest shown object at x,y with flags need */
int mm_inside(Object *o, int x, int y);

/* ---- groups ---- */
void mm_group_add(Object *g, Object *child);
void mm_group_rem(Object *g, Object *child);
/* lay out obj (an Area) in the box l,t,w,h: sets its box and, for groups, its children's */
void mm_layout(Object *o, int l, int t, int w, int h);
int  mm_group_active_page(Object *g);

/* ---- gadgets ---- */
void mm_string_key(Object *s, EventRecord *ev);
void mm_string_click(Object *s, EventRecord *ev, Point p);
void mm_string_activate(Object *s, int on);
void mm_string_idle(Object *s);
void mm_string_edit(Object *s, int cmd);         /* 2 cut, 3 copy, 4 paste, 5 clear */

/* ---- drawing (mm_gfx.c) ---- */
/* the rastport of a Mac port: what _rp() gives the classes */
struct RastPort *mm_rp_new(void *port);
void mm_rp_free(struct RastPort *rp);
void *mm_rp_port(struct RastPort *rp);
extern struct TextFont mm_font;                  /* the interface font (Geneva 9 or 10) */
void  mm_gfx_init(int compact);
/* pens: MUI's eight (MPEN_*) and the theme's RGB ones */
extern UWORD mm_mui_pens[8];
ULONG mm_pen_rgb(LONG pen);
/* drawing in a pen, also on one-bit screens (patterns for greys) */
void  mm_fill(void *port, const Rect *r, ULONG rgb);
void  mm_frame(void *port, const Rect *r, ULONG rgb);
void  mm_line(void *port, int x0, int y0, int x1, int y1, ULONG rgb);
void  mm_text(void *port, int x, int y, const char *s, int n, ULONG rgb, int bold);
int   mm_textw(const char *s, int n, int bold);
void  mm_use_font(int bold);
int   mm_mono_port(void *port);                  /* the port shows one bit per pixel (NULL: the current port) */
void  mm_gfx_port_moved(void *port);             /* a window opened, moved or changed size: its screen again */
void  mm_gfx_port_gone(void *port);
#define MM_RGB(r, g, b) (((ULONG)(r) << 16) | ((ULONG)(g) << 8) | (ULONG)(b))
/* the theme's colours for the built-in classes (mm_gfx.c reads the theme's tokens) */
ULONG mm_theme_rgb(int token);

/* a program's action procedure for its own controls' TrackControl (mm_os.c c_ctl_action), or NULL */
extern void (*mm_ctl_action)(void *ctl, int part);
/* a program's own Edit menu commands when no String is active (Undo 0, Cut 2, Copy 3, Paste 4,
   Clear 5), and which of them can be chosen now (bit n: command n), or NULL (mm_app.c) */
extern int (*mm_edit_action)(int edit);
extern unsigned (*mm_edit_state)(void);
/* A program whose window is a dialog in the Guidelines' sense (p. 205-206) sets this: Return and
   Enter, also in a text field (after its Acknowledge), go to the window's handlers as the raw key
   Return (0x44), the default button's; Escape and Command-period as Escape (0x45), Cancel's. Off:
   Escape only leaves a text field and Command-period is a menu key, as before (mm_app.c). */
extern int mm_dialog_keys;
/* dos: IoErr() is the File Manager's last error (an OSErr); in words for a message; the Mac's
   full path of an Amiga path such as ENVARC:Riptide/riptide.conf (mm_os.c) */
const char *mm_os_errtext(LONG err);
void mm_os_macpath(const char *amiga, char *out, int len);
/* ---- small things ---- */
void *mm_alloc(ULONG n);                         /* cleared; NULL when out of memory */
void  mm_free(void *p);
char *mm_strdup(const char *s);
void  mm_log(const char *fmt, ...);
extern int mm_error;

/* mm_hig.c: the Human Interface Guidelines' words and characters in place of the Amiga's
   ("..." the ellipsis, Drawer Folder); label: also Exit -> Quit, and no '_' markers */
void mm_hig_text(char *s, int size);
void mm_hig_label(char *s, int size);
#endif
