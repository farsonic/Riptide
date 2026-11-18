/*
 * Riptide - the Mac's MUI (see mm.h): the Area class (every object with a box: frames,
 * backgrounds, inner spacing, weights, help bubbles, input modes) and the small classes on it:
 * Rectangle (a spacer), Image (the check mark) and Text.
 *
 * Input modes as MUI has them: RelVerify (a button: MUIA_Pressed goes TRUE when the mouse
 * button goes down on it and FALSE when it is let go there, which is what a button's
 * notification waits for; MUIA_Selected shows it held) and Toggle (a check box: MUIA_Selected
 * changes when the click ends on it).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"
#include "prefs/mui_theme.h"

/* the frame's border and its default inner spacing */
static void frame_metrics(ULONG frame, int *bw, int *inner_h, int *inner_v)
{
	switch (frame) {
	case MUIV_Frame_None:
		*bw = 0; *inner_h = 0; *inner_v = 0;
		break;
	case MUIV_Frame_String:
		*bw = 1; *inner_h = 3; *inner_v = 2;
		break;
	case MUIV_Frame_InputList:
		*bw = 1; *inner_h = 1; *inner_v = 1;
		break;
	case MUIV_Frame_Button:
		*bw = 2; *inner_h = 8; *inner_v = 2;
		break;
	default:
		*bw = 1; *inner_h = 1; *inner_v = 1;
		break;
	}
}

/* "2:rrrrrrrr,gggggggg,bbbbbbbb" (theme_spec) or one of MUI's image numbers */
static ULONG bg_from_spec(ULONG spec)
{
	if (spec > 0x1000) {
		const char *s = (const char *)spec;
		ULONG c[3] = { 0, 0, 0 };
		int k = 0;
		if (s[0] != '2' || s[1] != ':')
			return 0;
		s += 2;
		while (*s && k < 3) {
			ULONG v = 0;
			int n = 0;
			while (*s && *s != ',') {
				char ch = *s++;
				int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
				        ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 0;
				if (n < 2)
					v = v * 16 + (ULONG)d;
				n++;
			}
			c[k++] = v;
			if (*s == ',')
				s++;
		}
		return 0x01000000UL | MM_RGB(c[0], c[1], c[2]);
	}
	return 0;
}

static ULONG count_lines(const char *s)
{
	ULONG n = 1;
	if (!s)
		return 0;
	for (; *s; s++)
		if (*s == '\n')
			n++;
	return n;
}

static void area_tags(Object *obj, const struct TagItem *tl, int init)
{
	struct mm_obj *mo = MMO(obj);
	ULONG v;
	if (mm_tag_find(tl, MUIA_Frame, &v)) mo->frame = (UWORD)v;
	if (mm_tag_find(tl, MUIA_Background, &v)) mo->bg = bg_from_spec(v);
	if (mm_tag_find(tl, MUIA_InnerLeft, &v)) mo->in_l = (BYTE)v;
	if (mm_tag_find(tl, MUIA_InnerRight, &v)) mo->in_r = (BYTE)v;
	if (mm_tag_find(tl, MUIA_InnerTop, &v)) mo->in_t = (BYTE)v;
	if (mm_tag_find(tl, MUIA_InnerBottom, &v)) mo->in_b = (BYTE)v;
	if (mm_tag_find(tl, MUIA_HorizWeight, &v)) mo->hweight = (WORD)v;
	if (mm_tag_find(tl, MUIA_VertWeight, &v)) mo->vweight = (WORD)v;
	if (mm_tag_find(tl, MUIA_ShortHelp, &v)) mo->help = (const char *)v;
	if (mm_tag_find(tl, MUIA_ControlChar, &v)) mo->cchar = (UBYTE)v;
	if (mm_tag_find(tl, MUIA_CycleChain, &v)) mo->cycle = (UBYTE)v;
	if (mm_tag_find(tl, MUIA_InputMode, &v)) mo->inmode = (UBYTE)v;
	if (mm_tag_find(tl, MUIA_FixHeightTxt, &v)) mo->fixh_lines = (UBYTE)count_lines((const char *)v);
	if (mm_tag_find(tl, MUIA_FixHeight, &v)) mo->fixh = (WORD)v;
	if (mm_tag_find(tl, MUIA_ShowSelState, &v)) mo->showsel = (UBYTE)!v;
	if (init) {
		if (mm_tag_find(tl, MUIA_Disabled, &v)) mo->disabled = (UBYTE)(v != 0);
		if (mm_tag_find(tl, MUIA_Selected, &v)) mo->selected = (UBYTE)(v != 0);
	}
}

void mm_box(Object *o, Rect *r)
{
	r->left = _left(o);
	r->top = _top(o);
	r->right = (short)(_left(o) + _width(o));
	r->bottom = (short)(_top(o) + _height(o));
}

int mm_inside(Object *o, int x, int y)
{
	return x >= _left(o) && x < _left(o) + _width(o) && y >= _top(o) && y < _top(o) + _height(o);
}

static void redraw(Object *obj)
{
	if (MMO(obj)->flags & MMF_SHOWN)
		MUI_Redraw(obj, MADF_DRAWOBJECT);
}

/* a click on an object with an input mode: track the mouse until the button is let go */
void mm_area_click(Object *obj, int x, int y)
{
	struct mm_obj *mo = MMO(obj);
	int in = 1;
	(void)x;
	(void)y;
	if (mo->disabled)
		return;
	if (mo->inmode == MUIV_InputMode_RelVerify) {
		mm_setattrs(obj, MUIA_Selected, TRUE, MUIA_Pressed, TRUE, TAG_DONE);
		while (StillDown()) {
			Point p;
			int now;
			GetMouse(&p);
			now = mm_inside(obj, p.h, p.v);
			if (now != in) {
				in = now;
				mm_set_quiet(obj, MUIA_Selected, (ULONG)in);
				redraw(obj);
			}
		}
		mm_set_quiet(obj, MUIA_Selected, FALSE);
		if (in)
			mm_setattrs(obj, MUIA_Pressed, FALSE, TAG_DONE);
		else
			mm_set_quiet(obj, MUIA_Pressed, FALSE);
		redraw(obj);
	} else if (mo->inmode == MUIV_InputMode_Toggle) {
		while (StillDown()) {
			Point p;
			GetMouse(&p);
			in = mm_inside(obj, p.h, p.v);
		}
		if (in)
			mm_setattrs(obj, MUIA_Selected, (ULONG)!mo->selected, TAG_DONE);
	}
}

/* the control character of obj was typed: what a click would do */
void mm_area_key(Object *obj)
{
	struct mm_obj *mo = MMO(obj);
	if (mo->disabled)
		return;
	if (mo->inmode == MUIV_InputMode_RelVerify) {
		mm_setattrs(obj, MUIA_Selected, TRUE, MUIA_Pressed, TRUE, TAG_DONE);
		redraw(obj);
		mm_set_quiet(obj, MUIA_Selected, FALSE);
		mm_setattrs(obj, MUIA_Pressed, FALSE, TAG_DONE);
		redraw(obj);
	} else if (mo->inmode == MUIV_InputMode_Toggle) {
		mm_setattrs(obj, MUIA_Selected, (ULONG)!mo->selected, TAG_DONE);
	}
}

DISPATCHER(mm_area_dispatch)
{
	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		MMO(obj)->flags |= MMF_AREA;
		area_tags(obj, ((struct opSet *)msg)->ops_AttrList, 1);
		if (MMO(obj)->inmode)
			MMO(obj)->flags |= MMF_INPUT;
		return (ULONG)obj;
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		struct mm_obj *mo = MMO(obj);
		ULONG v;
		int again = 0;
		area_tags(obj, tl, 0);
		if (mm_tag_find(tl, MUIA_Disabled, &v) && mo->disabled != (v != 0)) {
			mo->disabled = (UBYTE)(v != 0);
			again = 1;
		}
		if (mm_tag_find(tl, MUIA_Selected, &v) && mo->selected != (v != 0)) {
			mo->selected = (UBYTE)(v != 0);
			again = 1;
		}
		if (mm_tag_find(tl, MUIA_Pressed, &v))
			mo->pressed = (UBYTE)(v != 0);
		if (mm_tag_find(tl, MUIA_Background, &v))
			again = 1;
		DoSuperMethodA(cl, obj, msg);
		/* only the built-in classes redraw here: custom ones redraw themselves */
		if (again && OCLASS(obj) == mm_cl_image)
			redraw(obj);
		return 0;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		struct mm_obj *mo = MMO(obj);
		switch (g->opg_AttrID) {
		case MUIA_Selected: *g->opg_Storage = mo->selected; return TRUE;
		case MUIA_Pressed: *g->opg_Storage = mo->pressed; return TRUE;
		case MUIA_Disabled: *g->opg_Storage = mo->disabled; return TRUE;
		case MUIA_ShortHelp: *g->opg_Storage = (ULONG)mo->help; return TRUE;
		case MUIA_HorizWeight: *g->opg_Storage = (ULONG)mo->hweight; return TRUE;
		case MUIA_VertWeight: *g->opg_Storage = (ULONG)mo->vweight; return TRUE;
		case MUIA_ControlChar: *g->opg_Storage = mo->cchar; return TRUE;
		}
		return DoSuperMethodA(cl, obj, msg);
	}
	case MUIM_Setup: {
		struct MUI_RenderInfo *ri = ((struct { ULONG id; struct MUI_RenderInfo *ri; } *)msg)->ri;
		struct mm_obj *mo = MMO(obj);
		struct MUI_AreaData *ad = muiAreaData(obj);
		int bw, ih, iv;
		ad->mad_RenderInfo = ri;
		ad->mad_Font = &mm_font;
		frame_metrics(mo->frame, &bw, &ih, &iv);
		ad->mad_addleft = (BYTE)(bw + (mo->in_l >= 0 ? mo->in_l : ih));
		ad->mad_addtop = (BYTE)(bw + (mo->in_t >= 0 ? mo->in_t : iv));
		ad->mad_subwidth = (BYTE)(ad->mad_addleft + bw + (mo->in_r >= 0 ? mo->in_r : ih));
		ad->mad_subheight = (BYTE)(ad->mad_addtop + bw + (mo->in_b >= 0 ? mo->in_b : iv));
		mo->win = (struct mm_win *)((char *)ri - offsetof(struct mm_win, ri));
		mo->flags |= MMF_SETUP;
		theme_setup(obj);
		return TRUE;
	}
	case MUIM_Cleanup:
		theme_cleanup(obj);
		MMO(obj)->flags &= (UWORD)~(MMF_SETUP | MMF_SHOWN);
		MMO(obj)->win = NULL;
		return 0;
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		struct MUI_AreaData *ad = muiAreaData(obj);
		m->MinWidth = m->DefWidth = m->MaxWidth = ad->mad_subwidth;
		m->MinHeight = m->DefHeight = m->MaxHeight = ad->mad_subheight;
		return 0;
	}
	case MUIM_Show:
		MMO(obj)->flags |= MMF_SHOWN;
		return TRUE;
	case MUIM_Hide:
		MMO(obj)->flags &= (UWORD)~MMF_SHOWN;
		return 0;
	case MUIM_Draw: {
		struct mm_obj *mo = MMO(obj);
		Rect r;
		void *port;
		if (!(((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT) || !mo->win)
			return 0;
		port = mo->win->macwin;
		mm_box(obj, &r);
		if (mo->bg)
			mm_fill(port, &r, mo->bg & 0xFFFFFF);
		if (mo->frame == MUIV_Frame_Button) {
			/* a push button: white, black when held down, a shadow right and below */
			Rect in = r;
			in.right--;
			in.bottom--;
			mm_fill(port, &in, mo->selected && !mo->disabled ? mm_theme_rgb(T_TEXT) : mm_theme_rgb(T_WHITE));
			mm_frame(port, &in, mm_theme_rgb(T_BORDER));
			mm_line(port, in.left + 1, r.bottom - 1, r.right - 1, r.bottom - 1, mm_theme_rgb(T_BORDER));
			mm_line(port, r.right - 1, in.top + 1, r.right - 1, r.bottom - 1, mm_theme_rgb(T_BORDER));
		} else if (mo->frame != MUIV_Frame_None) {
			Rect in = r;
			InsetRect(&in, 1, 1);
			if (mo->frame == MUIV_Frame_String || mo->frame == MUIV_Frame_InputList)
				mm_fill(port, &in, mo->disabled ? mm_theme_rgb(T_WINDOW) : mm_theme_rgb(T_WHITE));
			mm_frame(port, &r, mm_theme_rgb(T_BORDER));
		}
		return 0;
	}
	case MUIM_HandleEvent:
		return 0;
	case MUIM_CreateShortHelp:
		return (ULONG)MMO(obj)->help;
	case MUIM_DeleteShortHelp:
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ---- Rectangle: room that stretches ---- */
DISPATCHER(mm_rect_dispatch)
{
	if (msg->MethodID == MUIM_AskMinMax) {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		DoSuperMethodA(cl, obj, msg);
		m->MaxWidth = MUI_MAXMAX;
		m->MaxHeight = MUI_MAXMAX;
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ---- Image: MUII_CheckMark, a check box ---- */
struct mm_image { ULONG spec; int freevert; };
ULONG mm_image_datasize(void) { return sizeof(struct mm_image); }

DISPATCHER(mm_image_dispatch)
{
	struct mm_image *d;
	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->spec = mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_Image_Spec, MUII_CheckMark);
		d->freevert = (int)mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_Image_FreeVert, FALSE);
		MMO(obj)->frame = MUIV_Frame_None;      /* the box is drawn below */
		MMO(obj)->flags |= MMF_INPUT;
		return (ULONG)obj;
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		int s = mm_font.tf_YSize + 1;
		DoSuperMethodA(cl, obj, msg);
		m->MinWidth += (WORD)(s + 2);
		m->DefWidth += (WORD)(s + 2);
		m->MaxWidth += (WORD)(s + 2);
		m->MinHeight += (WORD)s;
		m->DefHeight += (WORD)s;
		m->MaxHeight += (WORD)s;
		return 0;
	}
	case MUIM_Draw: {
		struct mm_obj *mo = MMO(obj);
		Rect r, b;
		int s = mm_font.tf_YSize - 1;
		void *port;
		DoSuperMethodA(cl, obj, msg);
		if (!(((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)) || !mo->win)
			return 0;
		port = mo->win->macwin;
		mm_box(obj, &r);
		b.left = (short)(r.left + 1);
		b.top = (short)(r.top + (r.bottom - r.top - s) / 2);
		b.right = (short)(b.left + s);
		b.bottom = (short)(b.top + s);
		mm_fill(port, &b, mo->disabled ? mm_theme_rgb(T_WINDOW) : mm_theme_rgb(T_WHITE));
		mm_frame(port, &b, mo->disabled ? mm_theme_rgb(T_DISABLED) : mm_theme_rgb(T_TEXT));
		if (mo->selected) {
			ULONG ink = mo->disabled ? mm_theme_rgb(T_DISABLED) : mm_theme_rgb(T_TEXT);
			mm_line(port, b.left + 2, b.top + 2, b.right - 3, b.bottom - 3, ink);
			mm_line(port, b.left + 2, b.bottom - 3, b.right - 3, b.top + 2, ink);
		}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ---- Text: a line of text (the editor draws its own labels; this is for completeness) ---- */
/* the text: copied, as MUI copies it (Lagoon's front end keeps no buffers for its labels), with
 * MUI's leading escapes understood: ESC c centre, ESC r right, ESC l left, ESC b bold, ESC n
 * normal (others skipped). A Text in a button frame (MUIV_Frame_Button) is drawn white on black
 * while it is held down. */
struct mm_textd { char *s; UBYTE align, bold; };
ULONG mm_text_datasize(void) { return sizeof(struct mm_textd); }
#define MUIA_Text_Contents 0x8042f8dcUL

static void text_set(struct mm_textd *d, const char *s)
{
	if (!s)
		s = "";
	d->align = 0;
	d->bold = 0;
	while (s[0] == 033 && s[1]) {
		switch (s[1]) {
		case 'c': d->align = 1; break;
		case 'r': d->align = 2; break;
		case 'l': d->align = 0; break;
		case 'b': d->bold = 1; break;
		case 'n': d->bold = 0; break;
		}
		s += 2;
	}
	if (d->s && !strcmp(d->s, s))
		return;
	mm_free(d->s);
	d->s = mm_strdup(s);
}

DISPATCHER(mm_text_dispatch)
{
	struct mm_textd *d;
	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->s = NULL;
		text_set(d, (const char *)mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_Text_Contents, (ULONG)""));
		return (ULONG)obj;
	case OM_DISPOSE:
		d = INST_DATA(cl, obj);
		mm_free(d->s);
		d->s = NULL;
		break;
	case OM_SET: {
		ULONG v, r;
		int changed = 0;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(((struct opSet *)msg)->ops_AttrList, MUIA_Text_Contents, &v)) {
			char *old = d->s;
			d->s = NULL;
			text_set(d, (const char *)v);
			changed = !old || !d->s || strcmp(old, d->s);
			mm_free(old);
		}
		r = DoSuperMethodA(cl, obj, msg);
		/* new contents on a shown text: drawn now, over the background it stands on */
		if (changed && (MMO(obj)->flags & MMF_SHOWN) && MMO(obj)->win) {
			struct mm_obj *mo = MMO(obj);
			Object *p = mo->parent;
			ULONG bg = mo->bg;
			Rect box;
			while (!bg && p) {
				bg = MMO(p)->bg;
				p = MMO(p)->parent;
			}
			mm_box(obj, &box);
			if (mo->frame == MUIV_Frame_None)
				mm_fill(mo->win->macwin, &box, bg ? bg & 0xFFFFFF : mm_theme_rgb(T_WINDOW));
			MUI_Redraw(obj, MADF_DRAWOBJECT);
		}
		return r;
	}
	case OM_GET:
		if (((struct opGet *)msg)->opg_AttrID == MUIA_Text_Contents) {
			d = INST_DATA(cl, obj);
			*((struct opGet *)msg)->opg_Storage = (ULONG)(d->s ? d->s : "");
			return TRUE;
		}
		break;
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		int w;
		d = INST_DATA(cl, obj);
		DoSuperMethodA(cl, obj, msg);
		w = mm_textw(d->s ? d->s : "", d->s ? (int)strlen(d->s) : 0, d->bold);
		m->MinWidth += (WORD)w;
		m->DefWidth += (WORD)w;
		m->MaxWidth = MUI_MAXMAX;
		m->MinHeight += (WORD)mm_font.tf_YSize;
		m->DefHeight += (WORD)mm_font.tf_YSize;
		m->MaxHeight += (WORD)mm_font.tf_YSize;
		return 0;
	}
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		d = INST_DATA(cl, obj);
		if ((((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT) && MMO(obj)->win && d->s) {
			struct mm_obj *mo = MMO(obj);
			int n = (int)strlen(d->s), x = _mleft(obj), w = mm_textw(d->s, n, d->bold);
			ULONG rgb = mo->frame == MUIV_Frame_Button && mo->selected && !mo->disabled ? mm_theme_rgb(T_WHITE)
			                                                                        : mm_theme_rgb(T_TEXT);
			if (d->align == 1 && _mwidth(obj) > w)
				x += (_mwidth(obj) - w) / 2;
			else if (d->align == 2 && _mwidth(obj) > w)
				x += _mwidth(obj) - w;
			mm_text(mo->win->macwin, x, _mtop(obj) + mm_font.tf_Baseline, d->s, n, rgb, d->bold);
		}
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}
