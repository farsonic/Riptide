/*
 * Riptide - RiptidePrefs (MUI): the custom classes (see mui_classes.h).
 * Each draws only in MUIM_Draw, inside _mleft.._mright / _mtop.._mbottom,
 * and only while shown; sizes follow the font height.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_theme.h"
#include "prefs/prefs_fields.h"
#include "prefs/mui_ids.h"

struct MUI_CustomClass *nav_mcc, *head_mcc, *qbutton_mcc, *qlabel_mcc, *status_mcc, *line_mcc;

#ifndef SELECTDOWN
#define SELECTDOWN 0x68
#endif
#define RAW_P      0x19
#define RAW_UP     0x4C
#define RAW_DOWN   0x4D
#define RAW_ESC    0x45
#define RAW_WHEEL_UP   0x7A     /* a mouse wheel, as Intuition reports it */
#define RAW_WHEEL_DOWN 0x7B
#define SHIFTS     (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)

/* the tag's value in an OM_NEW/OM_SET message, or def */
ULONG mc_tagval(Msg msg, ULONG tag, ULONG def)
{
	struct TagItem *t = ((struct opSet *)msg)->ops_AttrList;

	for (; t && t->ti_Tag != TAG_DONE; t++) {
		if (t->ti_Tag == TAG_MORE) {
			t = (struct TagItem *)t->ti_Data - 1;
			continue;
		}
		if (t->ti_Tag == tag)
			return t->ti_Data;
	}
	return def;
}

int mc_has_tag(Msg msg, ULONG tag)
{
	return mc_tagval(msg, tag, 0x7fffffffUL) != 0x7fffffffUL;
}

/* a text measuring rastport in the theme's font */
LONG mc_measure(const char *s, int key)
{
	struct RastPort rp;

	InitRastPort(&rp);
	return key ? theme_keytextlen(&rp, s) : theme_textlen(&rp, s);
}

/* the rows of a MUI_MinMax */
void mc_minmax(Msg msg, LONG minw, LONG defw, LONG maxw, LONG minh, LONG defh, LONG maxh)
{
	struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;

	m->MinWidth += (WORD)minw;
	m->DefWidth += (WORD)defw;
	m->MaxWidth += (WORD)maxw;
	m->MinHeight += (WORD)minh;
	m->DefHeight += (WORD)defh;
	m->MaxHeight += (WORD)maxh;
}

/* the pointer in an object */
int mc_inside(Object *obj, struct IntuiMessage *im)
{
	return im->MouseX >= _mleft(obj) && im->MouseX <= _mright(obj) && im->MouseY >= _mtop(obj) &&
	       im->MouseY <= _mbottom(obj);
}

/* add or remove an object's event handler (between MUIM_Setup and Cleanup) */
void mc_handler(Object *obj, struct IClass *cl, struct MUI_EventHandlerNode *ehn, ULONG events, int add)
{
	if (add) {
		ehn->ehn_Priority = 0;
		ehn->ehn_Flags = 0;
		ehn->ehn_Object = obj;
		ehn->ehn_Class = cl;
		ehn->ehn_Events = events;
		mui_do(_win(obj), MUIM_Window_AddEventHandler, (ULONG)ehn);
	} else {
		mui_do(_win(obj), MUIM_Window_RemEventHandler, (ULONG)ehn);
	}
}

/* ==================================================================
 * nav: the page list
 * ================================================================== */
struct nav_data {
	struct MUI_EventHandlerNode ehn;
	int active, hot, shown;
	/* when the rows do not fit the height it got: the first row shown and
	 * how many fit (the headings are left out then) */
	int scroll, top, vis;
};

/* The rows: as tall as the theme likes (compact screens: just over the
 * font), made lower until every page fits the screen beside the rest of
 * the window; below a certain height the icons go. nav_fit() sets them
 * once the screen is known. */
static int nav_rh, nav_hh, nav_is;

static int nav_rowh(void)
{
	return nav_rh ? nav_rh : theme_fh() + (theme_compact() ? 2 : 10);
}

static int nav_headh(void)
{
	return nav_rh ? nav_hh : theme_compact() ? 0 : theme_fh() + 8;
}

static int nav_isz(void)
{
	return nav_rh ? nav_is : theme_compact() ? 0 : nav_rowh() - 2 < 16 ? nav_rowh() - 2 : 16;
}

/* the height of all rows with the headings, for a row height rh */
static LONG nav_fullh(int rh, int hh)
{
	return 2 * hh + (LONG)RP_NPAGES * rh + 4;
}

/* the window's other parts: title bar, head, buttons, status line, borders */
#define NAV_CHROME (theme_compact() ? 70 : 100)

static void nav_fit(Object *obj)
{
	LONG avail = _screen(obj)->Height - NAV_CHROME, fh = theme_fh();
	int compact = theme_compact();

	nav_rh = fh + (compact ? 2 : 10);
	nav_hh = compact ? 0 : fh + 8;
	while (nav_rh > fh + 1 && nav_fullh(nav_rh, nav_hh) > avail) {
		nav_rh--;
		if (nav_hh > nav_rh)
			nav_hh = nav_rh;
	}
	nav_is = compact || nav_rh - 2 < 12 ? 0 : nav_rh - 2 < 16 ? nav_rh - 2 : 16;
}

/* The list's order: Status first, then the settings in page order. pos
 * is a row of the list, page a P_* number. */
static int nav_page_at(int pos)
{
	return pos == 0 ? P_STATUS : pos - 1;
}

static int nav_pos(int page)
{
	return page == P_STATUS ? 0 : page + 1;
}

/* the top of row pos below the object's top ("Settings" heads row 1,
 * "Advanced" the first advanced page; scrolled: no headings, from top) */
static LONG nav_rowy(const struct nav_data *d, int pos)
{
	if (d->scroll)
		return (LONG)(pos - d->top) * nav_rowh();
	return nav_headh() * ((pos >= 1) + (pos >= RP_FIRST_ADVANCED + 1)) + (LONG)pos * nav_rowh();
}

/* whether the rows fit the height the object got, and which are shown
 * (the active one always) */
static void nav_geom(struct nav_data *d, Object *obj)
{
	int pos = nav_pos(d->active);

	d->scroll = nav_fullh(nav_rowh(), nav_headh()) > _mheight(obj);
	d->vis = d->scroll ? (int)((_mheight(obj) - 4) / nav_rowh()) : RP_NPAGES;
	if (d->vis < 1)
		d->vis = 1;
	if (!d->scroll)
		d->top = 0;
	else if (pos < d->top)
		d->top = pos;
	else if (pos >= d->top + d->vis)
		d->top = pos - d->vis + 1;
	if (d->top > RP_NPAGES - d->vis)
		d->top = RP_NPAGES - d->vis;
	if (d->top < 0)
		d->top = 0;
}

/* is row pos on screen? */
static int nav_shown(const struct nav_data *d, int pos)
{
	return !d->scroll || (pos >= d->top && pos < d->top + d->vis);
}

/* the page under y, or -1 */
static int nav_hit(struct nav_data *d, Object *obj, LONG y)
{
	int i;

	for (i = 0; i < RP_NPAGES; i++) {
		LONG t = _mtop(obj) + nav_rowy(d, i);
		if (nav_shown(d, i) && y >= t && y < t + nav_rowh())
			return nav_page_at(i);
	}
	return -1;
}

/* scrolled: the rows by n (the page shown stays) */
static void nav_scroll(struct nav_data *d, Object *obj, int n)
{
	int top = d->top + n;

	if (!d->scroll)
		return;
	if (top > RP_NPAGES - d->vis)
		top = RP_NPAGES - d->vis;
	if (top < 0)
		top = 0;
	if (top != d->top) {
		d->top = top;
		MUI_Redraw(obj, MADF_DRAWOBJECT);
	}
}

static void nav_draw(struct nav_data *d, Object *obj)
{
	struct RastPort *rp = _rp(obj);
	LONG x = _mleft(obj), r = _mright(obj), fh = theme_fh();
	int i, isz = nav_isz(), gap = isz ? isz + 8 : 0;

	nav_geom(d, obj);
	theme_fill(rp, x, _mtop(obj), r - 1, _mbottom(obj), tpen(T_PANEL));
	theme_line(rp, r, _mtop(obj), r, _mbottom(obj), tpen(T_BORDER));
	if (nav_headh() && !d->scroll) {
		theme_text(rp, x + 10, _mtop(obj) + nav_rowy(d, 1) - nav_headh() + (nav_headh() - fh) / 2 + 1, "Settings",
		           tpen(T_DIM), 1);
		theme_text(rp, x + 10,
		           _mtop(obj) + nav_rowy(d, RP_FIRST_ADVANCED + 1) - nav_headh() + (nav_headh() - fh) / 2 + 1,
		           "Advanced", tpen(T_DIM), 1);
	}
	/* scrolled: an accent line at an edge rows are hidden behind */
	if (d->scroll && d->top > 0)
		theme_fill(rp, x, _mtop(obj), r - 1, _mtop(obj) + 1, tpen(T_ACCENT));
	if (d->scroll && d->top + d->vis < RP_NPAGES)
		theme_fill(rp, x, _mbottom(obj) - 1, r - 1, _mbottom(obj), tpen(T_ACCENT));
	for (i = 0; i < RP_NPAGES; i++) {
		int p = nav_page_at(i);
		LONG t = _mtop(obj) + nav_rowy(d, i), b = t + nav_rowh() - 1;
		if (!nav_shown(d, i))
			continue;
		if (p == d->active) {
			theme_fill(rp, x, t, r - 1, b, tpen(T_PRESSED));
			theme_fill(rp, x, t, x + 2, b, tpen(T_ACCENT));
		} else if (p == d->hot) {
			theme_fill(rp, x, t, r - 1, b, tpen(T_HOVER));
		}
		if (isz)
			theme_icon(rp, p, x + 10, t + (nav_rowh() - isz) / 2, isz, 0);
		theme_text(rp, x + 10 + gap, t + (nav_rowh() - fh) / 2, rp_pages[p].title, tpen(T_TEXT),
		           p == d->active);
	}
}

/* change the page, through OM_SET so notifications go out */
static void nav_go(Object *obj, int page)
{
	struct TagItem t[2];

	t[0].ti_Tag = MUIA_Nav_Active;
	t[0].ti_Data = (ULONG)page;
	t[1].ti_Tag = TAG_DONE;
	t[1].ti_Data = 0;
	SetAttrsA(obj, t);
}

DISPATCHER(nav_dispatch)
{
	struct nav_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->active = (int)mc_tagval(msg, MUIA_Nav_Active, 0);
		d->hot = -1;
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Nav_Active)) {
			int p = (int)mc_tagval(msg, MUIA_Nav_Active, 0);
			if (p >= 0 && p < RP_NPAGES && p != d->active) {
				d->active = p;
				if (d->shown)
					MUI_Redraw(obj, MADF_DRAWOBJECT);   /* nav_geom() scrolls it into view */
			}
		}
		break;
	case OM_GET:
		if (((struct opGet *)msg)->opg_AttrID == MUIA_Nav_Active) {
			d = INST_DATA(cl, obj);
			*((struct opGet *)msg)->opg_Storage = (ULONG)d->active;
			return TRUE;
		}
		break;
	case MUIM_AskMinMax: {
		LONG w = 0, ww, avail = _screen(obj)->Height - NAV_CHROME;
		int i;
		DoSuperMethodA(cl, obj, msg);
		for (i = 0; i < RP_NPAGES; i++)
			if ((ww = mc_measure(rp_pages[i].title, 0) + 1) > w)
				w = ww;
		w += 10 + (nav_isz() ? nav_isz() + 8 : 0) + 14;
		/* every row when they fit the screen, else what fits (then scrolled) */
		ww = nav_fullh(nav_rowh(), nav_headh());
		if (ww > avail)
			ww = avail > 6 * nav_rowh() ? avail : 6 * nav_rowh();
		mc_minmax(msg, w, w, w, ww, ww, MUI_MAXMAX);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		nav_fit(obj);
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY, 1);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct nav_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct nav_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			nav_draw(INST_DATA(cl, obj), obj);
		return 0;
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		int i;
		d = INST_DATA(cl, obj);
		if (!im || !d->shown)
			return 0;
		if (im->Class == IDCMP_MOUSEMOVE) {
			i = mc_inside(obj, im) ? nav_hit(d, obj, im->MouseY) : -1;
			if (i != d->hot) {
				d->hot = i;
				MUI_Redraw(obj, MADF_DRAWOBJECT);
			}
			return 0;
		}
		if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN && mc_inside(obj, im)) {
			i = nav_hit(d, obj, im->MouseY);
			if (i >= 0)
				nav_go(obj, i);
			else if (d->scroll && im->MouseY < _mtop(obj) + nav_rowh())
				nav_scroll(d, obj, -1);
			else if (d->scroll)
				nav_scroll(d, obj, 1);
			return MUI_EventHandlerRC_Eat;
		}
		if (im->Class == IDCMP_RAWKEY && (im->Code == RAW_WHEEL_UP || im->Code == RAW_WHEEL_DOWN) &&
		    mc_inside(obj, im)) {
			nav_scroll(d, obj, im->Code == RAW_WHEEL_UP ? -1 : 1);
			return MUI_EventHandlerRC_Eat;
		}
		if (im->Class == IDCMP_RAWKEY && !(im->Code & IECODE_UP_PREFIX)) {
			int back = (im->Qualifier & SHIFTS) != 0;
			if (im->Code == RAW_P) {
				nav_go(obj, nav_page_at((nav_pos(d->active) + (back ? RP_NPAGES - 1 : 1)) % RP_NPAGES));
				return MUI_EventHandlerRC_Eat;
			}
			if (im->Code == RAW_ESC) {
				/* Esc: Cancel, as the close gadget */
				mui_do(_app(obj), MUIM_Application_ReturnID, ID_CANCEL);
				return MUI_EventHandlerRC_Eat;
			}
			{
				int pos = nav_pos(d->active);
				if ((im->Code == RAW_UP && pos > 0) || (im->Code == RAW_DOWN && pos < RP_NPAGES - 1)) {
					nav_go(obj, nav_page_at(pos + (im->Code == RAW_UP ? -1 : 1)));
					return MUI_EventHandlerRC_Eat;
				}
			}
		}
		return 0;
	}
	case MUIM_CreateShortHelp: {
		struct MUIP_CreateShortHelp *m = (struct MUIP_CreateShortHelp *)msg;
		int i = nav_hit(INST_DATA(cl, obj), obj, m->my);
		if (i < 0)
			return 0;
		return (ULONG)theme_bubble(rp_pages[i].title, rp_pages[i].help);
	}
	case MUIM_DeleteShortHelp:
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * head: the page's icon, name and description
 * ================================================================== */
struct head_data {
	struct MUI_EventHandlerNode ehn;
	int page, shown;
};


#define HEAD_LINES 3

/* compact: the page's name on one line, no icon or description */
static LONG head_h(void)
{
	if (theme_compact())
		return theme_fh() + 7;
	return 8 + theme_fh() + 4 + HEAD_LINES * (theme_fh() + 1) + 8;
}

static LONG head_isz(void)
{
	return theme_fh() * 2 + 8 > 32 ? theme_fh() * 2 + 8 : 32;
}

/* the coffee cup at the head's right: the "support Riptide" badge */
static LONG head_cup_x(Object *obj) { return _mright(obj) - 10 - head_isz(); }
static int head_on_cup(Object *obj, LONG mx, LONG my)
{
	LONG isz = head_isz(), x = head_cup_x(obj), y = _mtop(obj) + 6;
	return !theme_compact() && mx >= x - 2 && mx <= x + isz + 2 && my >= y - 2 && my <= y + isz + 2;
}

/* draw s in lines of at most w pixels from x,y; at most n lines */
static void wrap_text(struct RastPort *rp, LONG x, LONG y, LONG w, const char *s, int n, LONG pen)
{
	char line[120];
	int len = 0;

	s = tr(s);
	while (*s && n > 0) {
		const char *e = s;
		int k = len;
		while (*e == ' ')
			e++;
		if (k)
			line[k++] = ' ';
		while (*e && *e != ' ' && k < (int)sizeof(line) - 1)
			line[k++] = *e++;
		line[k] = 0;
		if (len && TextLength(rp, (CONST_STRPTR)line, (ULONG)k) > w) {
			line[len] = 0;
			theme_text(rp, x, y, line, pen, 0);
			y += theme_fh() + 1;
			n--;
			len = 0;
			continue;
		}
		len = k;
		s = e;
	}
	if (len && n > 0)
		theme_text(rp, x, y, line, pen, 0);
}

static void head_draw(struct head_data *d, Object *obj)
{
	struct RastPort *rp = _rp(obj);
	LONG x = _mleft(obj), y = _mtop(obj), isz = head_isz(), tx = x + 12 + isz + 12;

	theme_fill(rp, x, y, _mright(obj), _mbottom(obj) - 1, tpen(T_HEAD));
	theme_line(rp, x, _mbottom(obj), _mright(obj), _mbottom(obj), tpen(T_BORDER));
	if (theme_compact()) {
		theme_text(rp, x + 12, y + 3, rp_pages[d->page].title, tpen(T_TEXT), 1);
		return;
	}
	theme_icon(rp, d->page, x + 12, y + (_mheight(obj) - isz) / 2, isz, 0);
	theme_icon(rp, IC_COFFEE, head_cup_x(obj), y + 6, isz, 0);
	theme_text(rp, tx, y + 8, rp_pages[d->page].title, tpen(T_TEXT), 1);
	if (theme_font())
		SetFont(rp, theme_font());
	wrap_text(rp, tx, y + 8 + theme_fh() + 4, _mright(obj) - tx - 8, rp_pages[d->page].help, HEAD_LINES,
	          tpen(T_DIM));
}

DISPATCHER(head_dispatch)
{
	struct head_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->page = (int)mc_tagval(msg, MUIA_Head_Page, 0);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Head_Page)) {
			d->page = (int)mc_tagval(msg, MUIA_Head_Page, 0);
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWOBJECT);
		}
		break;
	case MUIM_AskMinMax: {
		LONG h = head_h(), w = theme_fh() * 36;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, w, w, MUI_MAXMAX, h, h, h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, IDCMP_MOUSEBUTTONS, 1);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct head_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct head_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			head_draw(INST_DATA(cl, obj), obj);
		return 0;
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		d = INST_DATA(cl, obj);
		if (im && d->shown && im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN &&
		    head_on_cup(obj, im->MouseX, im->MouseY)) {
			mui_do(_app(obj), MUIM_Application_ReturnID, ID_SUPPORT);
			return MUI_EventHandlerRC_Eat;
		}
		return 0;
	}
	case MUIM_CreateShortHelp: {
		struct MUIP_CreateShortHelp *m = (struct MUIP_CreateShortHelp *)msg;
		if (head_on_cup(obj, m->mx, m->my))
			return (ULONG)theme_bubble(tr("Support Riptide"),
			                           tr("Riptide is free. If it is useful to you, buy its author a coffee: "
			                              "https://buymeacoffee.com/farsonic (click for the address)."));
		return 0;
	}
	case MUIM_DeleteShortHelp:
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * qbutton: a flat button (Area's MUIV_InputMode_RelVerify does the
 * clicking and the key; we draw and light it under the pointer)
 * ================================================================== */
struct qbutton_data {
	struct MUI_EventHandlerNode ehn;
	const char *label;
	int hot, shown;
	UWORD key;              /* raw key that presses it too, 0: none */
	ULONG id;               /* ... giving this return ID */
};

static void qbutton_draw(struct qbutton_data *d, Object *obj)
{
	struct RastPort *rp = _rp(obj);
	LONG x0 = _mleft(obj), y0 = _mtop(obj), x1 = _mright(obj), y1 = _mbottom(obj);
	int sel = mui_get(obj, MUIA_Selected) != 0, dis = mui_get(obj, MUIA_Disabled) != 0;
	LONG face = sel ? tpen(T_PRESSED) : d->hot && !dis ? tpen(T_HOVER) : tpen(T_BUTTON);
	LONG w = theme_keytextlen(rp, d->label);

	theme_fill(rp, x0 + 1, y0 + 1, x1 - 1, y1 - 1, face);
	theme_rect(rp, x0, y0, x1, y1, sel ? tpen(T_ACCENT) : tpen(T_BORDER));
	theme_keytext(rp, x0 + (x1 - x0 + 1 - w) / 2, y0 + (y1 - y0 + 1 - theme_fh()) / 2, d->label,
	              dis ? tpen(T_DISABLED) : tpen(T_TEXT));
}

DISPATCHER(qbutton_dispatch)
{
	struct qbutton_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->label = (const char *)mc_tagval(msg, MUIA_QButton_Label, (ULONG)"");
		d->key = (UWORD)mc_tagval(msg, MUIA_QButton_Key, 0);
		d->id = mc_tagval(msg, MUIA_QButton_ID, 0);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if ((mc_has_tag(msg, MUIA_Selected) || mc_has_tag(msg, MUIA_Disabled)) && d->shown) {
			ULONG r;
			/* let go (also before a requester takes the input): not lit
			 * either, until the pointer moves over it again */
			if (mc_has_tag(msg, MUIA_Selected) && !mc_tagval(msg, MUIA_Selected, 0))
				d->hot = 0;
			r = DoSuperMethodA(cl, obj, msg);
			MUI_Redraw(obj, MADF_DRAWOBJECT);
			return r;
		}
		break;
	case MUIM_AskMinMax: {
		LONG w = mc_measure(((struct qbutton_data *)INST_DATA(cl, obj))->label, 1) + 24, h = theme_fh() + 8;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, w, w, MUI_MAXMAX, h, h, h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, IDCMP_MOUSEMOVE | (d->key ? IDCMP_RAWKEY : 0), 1);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct qbutton_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct qbutton_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			qbutton_draw(INST_DATA(cl, obj), obj);
		return 0;
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		int in;
		d = INST_DATA(cl, obj);
		if (!im || !d->shown)
			return 0;
		if (im->Class == IDCMP_RAWKEY && d->key && im->Code == d->key) {
			mui_do(_app(obj), MUIM_Application_ReturnID, d->id);
			return MUI_EventHandlerRC_Eat;
		}
		if (im->Class != IDCMP_MOUSEMOVE)
			return 0;
		in = mc_inside(obj, im);
		if (in != d->hot) {
			d->hot = in;
			MUI_Redraw(obj, MADF_DRAWOBJECT);
		}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * qlabel: a setting's label, a note or a title
 * ================================================================== */
struct qlabel_data {
	const char *text;
	int kind, shown;
};

/* the lines of a QL_TEXT: their number, and the widest */
static int text_lines(struct RastPort *rp, const char *s, LONG *w)
{
	int n = 1;
	char line[120];

	*w = 0;
	for (;;) {
		int k = 0;
		LONG lw;
		while (*s && *s != '\n' && k < (int)sizeof(line) - 1)
			line[k++] = *s++;
		line[k] = 0;
		if ((lw = theme_textlen(rp, line)) > *w)
			*w = lw;
		if (*s != '\n')
			return n;
		s++;
		n++;
	}
}

static void qlabel_draw(struct qlabel_data *d, Object *obj)
{
	struct RastPort *rp = _rp(obj);
	LONG y = _mtop(obj) + (_mheight(obj) - theme_fh()) / 2;
	int dis = mui_get(obj, MUIA_Disabled) != 0;

	if (d->kind == QL_TEXT) {
		const char *s = tr(d->text);
		char line[120];
		y = _mtop(obj);
		while (*s) {
			int k = 0;
			while (*s && *s != '\n' && k < (int)sizeof(line) - 1)
				line[k++] = *s++;
			line[k] = 0;
			theme_text(rp, _mleft(obj), y, line, tpen(T_TEXT), 0);
			y += theme_fh() + 1;
			if (*s == '\n')
				s++;
		}
		return;
	}

	if (d->kind == QL_LABEL) {
		LONG w = theme_keytextlen(rp, d->text);
		theme_keytext(rp, _mright(obj) - w, y, d->text, dis ? tpen(T_DISABLED) : tpen(T_TEXT));
	} else {
		theme_text(rp, _mleft(obj), y, d->text, d->kind == QL_NOTE ? tpen(T_DIM) : tpen(T_TEXT),
		           d->kind == QL_TITLE);
	}
}

DISPATCHER(qlabel_dispatch)
{
	struct qlabel_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->text = (const char *)mc_tagval(msg, MUIA_QLabel_Text, (ULONG)"");
		d->kind = (int)mc_tagval(msg, MUIA_QLabel_Kind, QL_LABEL);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_QLabel_Text) || mc_has_tag(msg, MUIA_Disabled)) {
			ULONG r;
			if (mc_has_tag(msg, MUIA_QLabel_Text))
				d->text = (const char *)mc_tagval(msg, MUIA_QLabel_Text, (ULONG)"");
			r = DoSuperMethodA(cl, obj, msg);
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWOBJECT);
			return r;
		}
		break;
	case MUIM_AskMinMax: {
		d = INST_DATA(cl, obj);
		LONG w = mc_measure(d->text, d->kind == QL_LABEL) + 2, h = theme_fh() + 2;
		if (d->kind == QL_TEXT) {
			struct RastPort rp;
			InitRastPort(&rp);
			h = text_lines(&rp, tr(d->text), &w) * (theme_fh() + 1);
			w += 2;
			DoSuperMethodA(cl, obj, msg);
			mc_minmax(msg, w, w, w, h, h, h);
			return 0;
		}
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, d->kind == QL_NOTE ? 0 : w, w, MUI_MAXMAX, h, h, d->kind == QL_LABEL ? MUI_MAXMAX : h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		return TRUE;
	case MUIM_Cleanup:
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct qlabel_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct qlabel_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			qlabel_draw(INST_DATA(cl, obj), obj);
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * status: the line at the bottom
 * ================================================================== */
struct status_data {
	const char *text;
	int shown;
};

DISPATCHER(status_dispatch)
{
	struct status_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->text = "";
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Status_Text)) {
			d->text = (const char *)mc_tagval(msg, MUIA_Status_Text, (ULONG)"");
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWOBJECT);
		}
		break;
	case MUIM_AskMinMax: {
		LONG h = theme_fh() + 6;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 0, 100, MUI_MAXMAX, h, h, h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		return TRUE;
	case MUIM_Cleanup:
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct status_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct status_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)) {
			struct RastPort *rp = _rp(obj);
			d = INST_DATA(cl, obj);
			theme_fill(rp, _mleft(obj), _mtop(obj) + 1, _mright(obj), _mbottom(obj), tpen(T_PANEL));
			theme_line(rp, _mleft(obj), _mtop(obj), _mright(obj), _mtop(obj), tpen(T_BORDER));
			theme_text(rp, _mleft(obj) + 8, _mtop(obj) + 3, d->text, tpen(T_DIM), 0);
		}
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * line: a separator
 * ================================================================== */
struct line_data {
	int vert;
};

DISPATCHER(line_dispatch)
{
	struct line_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->vert = (int)mc_tagval(msg, MUIA_Line_Vert, 0);
		return (ULONG)obj;
	case MUIM_AskMinMax:
		d = INST_DATA(cl, obj);
		DoSuperMethodA(cl, obj, msg);
		if (d->vert)
			mc_minmax(msg, 1, 1, 1, 0, 0, MUI_MAXMAX);
		else
			mc_minmax(msg, 0, 0, MUI_MAXMAX, 1, 1, 1);
		return 0;
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		return TRUE;
	case MUIM_Cleanup:
		theme_cleanup(obj);
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			theme_fill(_rp(obj), _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_BORDER));
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * making and freeing the classes
 * ================================================================== */
static struct MUI_CustomClass *mk(int size, APTR disp)
{
	return MUI_CreateCustomClass(NULL, (CONST_STRPTR)MUIC_Area, NULL, size, disp);
}

int rp_classes_make(void)
{
	nav_mcc = mk(sizeof(struct nav_data), DISPATCHER_REF(nav_dispatch));
	head_mcc = mk(sizeof(struct head_data), DISPATCHER_REF(head_dispatch));
	qbutton_mcc = mk(sizeof(struct qbutton_data), DISPATCHER_REF(qbutton_dispatch));
	qlabel_mcc = mk(sizeof(struct qlabel_data), DISPATCHER_REF(qlabel_dispatch));
	status_mcc = mk(sizeof(struct status_data), DISPATCHER_REF(status_dispatch));
	line_mcc = mk(sizeof(struct line_data), DISPATCHER_REF(line_dispatch));
	return nav_mcc && head_mcc && qbutton_mcc && qlabel_mcc && status_mcc && line_mcc ? 0 : -1;
}

void rp_classes_free(void)
{
	struct MUI_CustomClass **c[] = { &nav_mcc, &head_mcc, &qbutton_mcc, &qlabel_mcc, &status_mcc, &line_mcc };
	int i;

	for (i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++)
		if (*c[i]) {
			MUI_DeleteCustomClass(*c[i]);
			*c[i] = NULL;
		}
}
