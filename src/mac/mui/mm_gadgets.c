/*
 * Riptide - the Mac's MUI (see mm.h): the gadgets.
 *
 *   String     a TextEdit record of its own in the window; typing, Return (MUIA_String_Acknowledge),
 *              Tab to the next string of the cycle chain, Cut, Copy and Paste
 *   Cycle      a box with the chosen entry and a pop-up menu of the others (PopUpMenuSelect)
 *   List       the rows (MUIM_List_InsertSingle, MUIM_List_Clear, MUIA_List_Active, MUIA_List_Quiet)
 *              with a scroll bar of its own when they do not fit
 *   Listview   a group around its list (MUIA_Listview_List)
 *   Menustrip  a NewMenu table (MUIO_MenustripNM); MUIM_FindUData gives a Menuitem, whose
 *              MUIA_Menuitem_Checked and _Enabled reach the Mac menu (mm_app.c builds the menus)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"
#include "prefs/mui_theme.h"

/* ==================== String ==================== */
struct mm_string {
	TEHandle te;
	char *buf;
	int maxlen;
	int active;
};
ULONG mm_string_datasize(void) { return sizeof(struct mm_string); }
#define SD(o) ((struct mm_string *)INST_DATA(mm_cl_string, o))

/* the text into buf (TextEdit's own copy is the live one while the window is open) */
static void str_sync(Object *o)
{
	struct mm_string *d = SD(o);
	if (d->te && *d->te) {
		TERec *t = *d->te;
		int n = t->teLength < d->maxlen - 1 ? t->teLength : d->maxlen - 1;
		memcpy(d->buf, *t->hText, (unsigned long)n);
		d->buf[n] = 0;
	}
}

static void str_rects(Object *o)
{
	struct mm_string *d = SD(o);
	TERec *t;
	Rect v;
	if (!d->te)
		return;
	t = *d->te;
	v.left = (short)_mleft(o);
	v.top = (short)(_mtop(o) + (_mheight(o) - mm_font.tf_YSize) / 2);
	v.right = (short)(_mright(o) + 1);
	v.bottom = (short)(v.top + mm_font.tf_YSize);
	if (v.top < _top(o) + 1)
		v.top = (short)(_top(o) + 1);
	t->viewRect = v;
	t->destRect = v;
	t->destRect.right = (short)(v.left + 4000);      /* one line: no wrapping */
}

void mm_string_activate(Object *o, int on)
{
	struct mm_string *d = SD(o);
	if (!d->te || d->active == on || !MMO(o)->win)
		return;
	SetPort(MMO(o)->win->macwin);
	mm_use_font(0);
	str_rects(o);
	d->active = on;
	if (on) {
		TESetSelect(0, 32767, d->te);
		TEActivate(d->te);
	} else {
		TEDeactivate(d->te);
		str_sync(o);
	}
}

void mm_string_idle(Object *o)
{
	struct mm_string *d = SD(o);
	if (d->te && d->active && MMO(o)->win) {
		SetPort(MMO(o)->win->macwin);
		TEIdle(d->te);
	}
}

static void str_changed(Object *o)
{
	str_sync(o);
	mm_notify_fire(o, MUIA_String_Contents, (ULONG)SD(o)->buf);
}

void mm_string_key(Object *o, EventRecord *ev)
{
	struct mm_string *d = SD(o);
	int c = (int)(ev->message & charCodeMask);
	if (!d->te || MMO(o)->disabled)
		return;
	SetPort(MMO(o)->win->macwin);
	mm_use_font(0);
	if (c == '\r' || c == 3) {
		str_sync(o);
		mm_setattrs(o, MUIA_String_Acknowledge, (ULONG)d->buf, TAG_DONE);
		return;
	}
	/* room left? (control characters, the arrows and backspace always pass) */
	if (c >= 32 && c != 127 && (*d->te)->teLength - ((*d->te)->selEnd - (*d->te)->selStart) >= d->maxlen - 1) {
		SysBeep(2);
		return;
	}
	if (c == 0x1B)
		return;
	TEKey(c, d->te);
	str_changed(o);
}

void mm_string_edit(Object *o, int cmd)
{
	struct mm_string *d = SD(o);
	if (!d->te || !d->active)
		return;
	SetPort(MMO(o)->win->macwin);
	mm_use_font(0);
	switch (cmd) {
	case 2:
		TECut(d->te);
		ZeroScrap();
		PutScrap(LM_TESCRPLEN, MAC_TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
		break;
	case 3:
		TECopy(d->te);
		ZeroScrap();
		PutScrap(LM_TESCRPLEN, MAC_TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
		return;
	case 4: {
		void **h = mac_newhandle(0);
		long off = 0, n;
		if (!h)
			return;
		n = (long)GetScrap(h, MAC_TYPE('T', 'E', 'X', 'T'), &off);
		if (n > 0) {
			long room = d->maxlen - 1 - (*d->te)->teLength + ((*d->te)->selEnd - (*d->te)->selStart);
			long i, k = 0;
			char *s = (char *)*h;
			/* one line: up to the first line end, and what fits */
			for (i = 0; i < n && s[i] != '\r' && s[i] != '\n'; i++)
				k++;
			if (k > room)
				k = room;
			if (k > 0) {
				TEDelete(d->te);
				TEInsert(s, k, d->te);
			}
		}
		mac_disposehandle(h);
		break;
	}
	case 5:
		TEDelete(d->te);
		break;
	}
	str_changed(o);
}

void mm_string_click(Object *o, EventRecord *ev, Point p)
{
	struct mm_string *d = SD(o);
	if (!d->te || MMO(o)->disabled)
		return;
	mm_win_activate_string(MMO(o)->win, o);
	SetPort(MMO(o)->win->macwin);
	mm_use_font(0);
	str_rects(o);
	TEClick(pt_long(p), (ev->modifiers & shiftKey) != 0, d->te);
}

DISPATCHER(mm_string_dispatch)
{
	struct mm_string *d;
	switch (msg->MethodID) {
	case OM_NEW: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		const char *s;
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->maxlen = (int)mm_tag(tl, MUIA_String_MaxLen, 80);
		if (d->maxlen < 2)
			d->maxlen = 2;
		d->buf = mm_alloc((ULONG)d->maxlen);
		if (!d->buf)
			return 0;
		s = (const char *)mm_tag(tl, MUIA_String_Contents, 0);
		if (s)
			rt_strlcpy(d->buf, s, (size_t)d->maxlen);
		if (!MMO(obj)->frame)
			MMO(obj)->frame = MUIV_Frame_String;
		MMO(obj)->flags |= MMF_STRING | MMF_INPUT;
		return (ULONG)obj;
	}
	case OM_DISPOSE:
		d = INST_DATA(cl, obj);
		if (d->te)
			TEDispose(d->te);
		mm_free(d->buf);
		break;
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		ULONG v;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(tl, MUIA_String_Contents, &v)) {
			rt_strlcpy(d->buf, v ? (const char *)v : "", (size_t)d->maxlen);
			if (d->te && MMO(obj)->win) {
				SetPort(MMO(obj)->win->macwin);
				mm_use_font(0);
				str_rects(obj);
				TESetText(d->buf, (long)strlen(d->buf), d->te);
				if (MMO(obj)->flags & MMF_SHOWN)
					MUI_Redraw(obj, MADF_DRAWOBJECT);
			}
		}
		if (mm_tag_find(tl, MUIA_Disabled, &v) && (MMO(obj)->flags & MMF_SHOWN)) {
			DoSuperMethodA(cl, obj, msg);
			if (v && d->active)
				mm_win_activate_string(MMO(obj)->win, NULL);
			MUI_Redraw(obj, MADF_DRAWOBJECT);
			return 0;
		}
		break;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		if (g->opg_AttrID == MUIA_String_Contents || g->opg_AttrID == MUIA_String_Acknowledge) {
			str_sync(obj);
			*g->opg_Storage = (ULONG)d->buf;
			return TRUE;
		}
		if (g->opg_AttrID == MUIA_String_MaxLen) {
			*g->opg_Storage = (ULONG)d->maxlen;
			return TRUE;
		}
		break;
	}
	case MUIM_Setup: {
		Rect r = { 0, 0, 10, 100 };
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		d = INST_DATA(cl, obj);
		SetPort(MMO(obj)->win->macwin);
		mm_use_font(0);
		d->te = (TEHandle)TENew(&r, &r);
		if (d->te) {
			(*d->te)->crOnly = -1;
			TESetText(d->buf, (long)strlen(d->buf), d->te);
		}
		d->active = 0;
		return TRUE;
	}
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		str_sync(obj);
		if (d->te)
			TEDispose(d->te);
		d->te = NULL;
		d->active = 0;
		break;
	case MUIM_Hide:
		d = INST_DATA(cl, obj);
		if (d->active && MMO(obj)->win)
			mm_win_activate_string(MMO(obj)->win, NULL);
		break;
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		int cw = mm_textw("0", 1, 0);
		d = INST_DATA(cl, obj);
		DoSuperMethodA(cl, obj, msg);
		m->MinWidth += (WORD)(cw * 6);
		m->DefWidth += (WORD)(cw * (d->maxlen < 24 ? d->maxlen + 1 : 24));
		m->MaxWidth = MUI_MAXMAX;
		m->MinHeight += (WORD)mm_font.tf_YSize;
		m->DefHeight += (WORD)mm_font.tf_YSize;
		m->MaxHeight += (WORD)mm_font.tf_YSize;
		return 0;
	}
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		d = INST_DATA(cl, obj);
		if ((((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)) && MMO(obj)->win) {
			void *port = MMO(obj)->win->macwin;
			SetPort(port);
			if (MMO(obj)->disabled || !d->te) {
				str_sync(obj);
				mm_text(port, _mleft(obj), _mtop(obj) + (_mheight(obj) - mm_font.tf_YSize) / 2 + mm_font.tf_Baseline,
				        d->buf, (int)strlen(d->buf), mm_theme_rgb(MMO(obj)->disabled ? T_DISABLED : T_TEXT), 0);
			} else {
				Rect v;
				mm_use_font(0);
				ForeColor(33);
				BackColor(30);
				str_rects(obj);
				v = (*d->te)->viewRect;
				EraseRect(&v);
				TEUpdate(&v, d->te);
			}
		}
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== Cycle ==================== */
struct mm_cycle {
	const char **entries;
	int n, active;
};
ULONG mm_cycle_datasize(void) { return sizeof(struct mm_cycle); }
#define CD(o) ((struct mm_cycle *)INST_DATA(mm_cl_cycle, o))

static int cyc_count(const char **e)
{
	int n = 0;
	while (e && e[n])
		n++;
	return n;
}

static void cyc_draw(Object *o)
{
	struct mm_cycle *d = CD(o);
	void *port = MMO(o)->win->macwin;
	Rect r;
	int dis = MMO(o)->disabled, x;
	const char *s = d->n ? tr(d->entries[d->active]) : "";
	mm_box(o, &r);
	mm_fill(port, &r, mm_theme_rgb(T_BUTTON));
	mm_frame(port, &r, mm_theme_rgb(dis ? T_DISABLED : T_BORDER));
	/* the shadow under it, as the Mac's pop-up menus have */
	mm_line(port, r.left + 2, r.bottom, r.right, r.bottom, mm_theme_rgb(T_BORDER));
	mm_line(port, r.right, r.top + 2, r.right, r.bottom, mm_theme_rgb(T_BORDER));
	mm_text(port, r.left + 6, r.top + (r.bottom - r.top - mm_font.tf_YSize) / 2 + mm_font.tf_Baseline, s,
	        (int)strlen(s), mm_theme_rgb(dis ? T_DISABLED : T_TEXT), 0);
	/* the triangle */
	x = r.right - 13;
	{
		int y = r.top + (r.bottom - r.top) / 2 - 2, k;
		for (k = 0; k < 4; k++)
			mm_line(port, x + k, y + k, x + 6 - k, y + k, mm_theme_rgb(dis ? T_DISABLED : T_TEXT));
	}
}

static void cyc_click(Object *o)
{
	struct mm_cycle *d = CD(o);
	void *menu;
	Point p;
	long r;
	int i;
	unsigned char pb[256];
	if (MMO(o)->disabled || !d->n)
		return;
	menu = NewMenu(250, mac_pstr(pb, ""));
	if (!menu)
		return;
	for (i = 0; i < d->n; i++) {
		AppendMenu(menu, mac_pstr(pb, " "));
		SetItem(menu, i + 1, mac_pstr(pb, tr(d->entries[i])));
	}
	CheckItem(menu, d->active + 1, 1);
	InsertMenu(menu, -1);
	p.h = _left(o);
	p.v = _top(o);
	SetPort(MMO(o)->win->macwin);
	LocalToGlobal(&p);
	mm_use_font(0);
	r = PopUpMenuSelect(menu, p.v + 1, p.h + 1, d->active + 1);
	DeleteMenu(250);
	DisposeMenu(menu);
	if ((r >> 16) == 250 && (r & 0xFFFF) && (int)(r & 0xFFFF) - 1 != d->active)
		mm_setattrs(o, MUIA_Cycle_Active, (ULONG)((r & 0xFFFF) - 1), TAG_DONE);
}

void mm_cycle_click(Object *o) { cyc_click(o); }
void mm_cycle_key(Object *o)
{
	struct mm_cycle *d = CD(o);
	if (d->n && !MMO(o)->disabled)
		mm_setattrs(o, MUIA_Cycle_Active, (ULONG)((d->active + 1) % d->n), TAG_DONE);
}

DISPATCHER(mm_cycle_dispatch)
{
	struct mm_cycle *d;
	switch (msg->MethodID) {
	case OM_NEW: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->entries = (const char **)mm_tag(tl, MUIA_Cycle_Entries, 0);
		d->n = cyc_count(d->entries);
		d->active = (int)mm_tag(tl, MUIA_Cycle_Active, 0);
		if (d->active >= d->n)
			d->active = 0;
		MMO(obj)->flags |= MMF_INPUT;
		return (ULONG)obj;
	}
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		ULONG v;
		int again = 0;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(tl, MUIA_Cycle_Active, &v)) {
			int a = (int)v;
			if (a < 0 || a >= d->n)
				a = 0;
			if (a != d->active) {
				d->active = a;
				again = 1;
			}
		}
		if (mm_tag_find(tl, MUIA_Disabled, &v))
			again = 1;
		DoSuperMethodA(cl, obj, msg);
		if (again && (MMO(obj)->flags & MMF_SHOWN))
			MUI_Redraw(obj, MADF_DRAWOBJECT);
		return 0;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		if (g->opg_AttrID == MUIA_Cycle_Active) {
			*g->opg_Storage = (ULONG)d->active;
			return TRUE;
		}
		break;
	}
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		int i, w = 0;
		d = INST_DATA(cl, obj);
		DoSuperMethodA(cl, obj, msg);
		for (i = 0; i < d->n; i++) {
			const char *s = tr(d->entries[i]);
			int tw = mm_textw(s, (int)strlen(s), 0);
			if (tw > w)
				w = tw;
		}
		m->MinWidth += (WORD)(w + 26);
		m->DefWidth += (WORD)(w + 26);
		m->MaxWidth += (WORD)(w + 26);
		m->MinHeight += (WORD)(mm_font.tf_YSize + 5);
		m->DefHeight += (WORD)(mm_font.tf_YSize + 5);
		m->MaxHeight += (WORD)(mm_font.tf_YSize + 5);
		return 0;
	}
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if ((((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)) && MMO(obj)->win)
			cyc_draw(obj);
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== List ==================== */
struct mm_list {
	char **items;
	int n, cap;
	int active, top;
	int quiet;
};
ULONG mm_list_datasize(void) { return sizeof(struct mm_list); }
#define LD(o) ((struct mm_list *)INST_DATA(mm_cl_list, o))
#define SBW 13

static int row_h(void)
{
	return mm_font.tf_YSize + 1;
}

static int list_rows(Object *o)
{
	int r = _mheight(o) / row_h();
	return r > 0 ? r : 1;
}

/* the text of a row without MUI's escape codes ("\33b" and the like) */
static int clean(const char *s, char *out, int len, int *b)
{
	int n = 0;
	*b = 0;
	while (*s && n < len - 1) {
		if (*s == 27 && s[1]) {
			if (s[1] == 'b')
				*b = 1;
			s += 2;
			continue;
		}
		out[n++] = *s++;
	}
	out[n] = 0;
	return n;
}

static void list_draw(Object *o)
{
	struct mm_list *d = LD(o);
	void *port = MMO(o)->win->macwin;
	int rows = list_rows(o), i, rh = row_h(), sb = d->n > rows;
	int l = _mleft(o), t = _mtop(o), r = _mright(o) - (sb ? SBW : 0);
	Rect clip;
	mm_box(o, &clip);
	for (i = 0; i < rows; i++) {
		int k = d->top + i;
		Rect row;
		row.left = (short)l;
		row.right = (short)(r + 1);
		row.top = (short)(t + i * rh);
		row.bottom = (short)(row.top + rh);
		if (row.bottom > _mbottom(o) + 1)
			row.bottom = (short)(_mbottom(o) + 1);
		/* the chosen row: the theme's colour, or on a one-bit screen inverted, as Mac lists show it */
		int sel = k == d->active && k < d->n, inv = sel && mm_mono_port(port);
		mm_fill(port, &row, inv ? MM_RGB(0, 0, 0) : sel ? mm_theme_rgb(T_PRESSED) : mm_theme_rgb(T_WHITE));
		if (k < d->n) {
			char txt[160];
			/* through the program's translation hook, as the other gadgets' texts are (MUI shows a
			   list's rows as they are; the Mac build's tr() puts the Mac's words in) */
			int b, n = clean(tr(d->items[k]), txt, sizeof txt, &b);
			/* cut to the row's width */
			while (n > 0 && mm_textw(txt, n, b) > r - l - 4)
				n--;
			mm_text(port, l + 3, row.top + mm_font.tf_Baseline, txt, n,
			        inv ? MM_RGB(255, 255, 255) : mm_theme_rgb(MMO(o)->disabled ? T_DISABLED : T_TEXT), b);
		}
	}
	if (sb) {
		Rect tr, th;
		int x0 = _mright(o) - SBW + 1, y0 = t, y1 = _mbottom(o), span, ts, tp;
		tr.left = (short)x0;
		tr.right = (short)(_mright(o) + 1);
		tr.top = (short)y0;
		tr.bottom = (short)(y1 + 1);
		mm_fill(port, &tr, mm_theme_rgb(T_PANEL));
		mm_line(port, x0, y0, x0, y1, mm_theme_rgb(T_BORDER));
		/* the arrows */
		{
			int cx = x0 + SBW / 2, k;
			for (k = 0; k < 4; k++) {
				mm_line(port, cx - k, y0 + 3 + k, cx + k, y0 + 3 + k, mm_theme_rgb(T_TEXT));
				mm_line(port, cx - k, y1 - 3 - k, cx + k, y1 - 3 - k, mm_theme_rgb(T_TEXT));
			}
		}
		span = (y1 - y0 + 1) - 2 * 10;
		ts = span * rows / d->n;
		if (ts < 8)
			ts = 8;
		tp = d->n > rows ? (span - ts) * d->top / (d->n - rows) : 0;
		th.left = (short)(x0 + 2);
		th.right = (short)(_mright(o) - 1);
		th.top = (short)(y0 + 10 + tp);
		th.bottom = (short)(th.top + ts);
		mm_fill(port, &th, mm_theme_rgb(T_BORDER));
	}
}

static void list_redraw(Object *o)
{
	if ((MMO(o)->flags & MMF_SHOWN) && !LD(o)->quiet)
		MUI_Redraw(o, MADF_DRAWOBJECT);
}

static void list_scroll(Object *o, int top)
{
	struct mm_list *d = LD(o);
	int rows = list_rows(o), mx = d->n - rows;
	if (mx < 0)
		mx = 0;
	if (top > mx)
		top = mx;
	if (top < 0)
		top = 0;
	if (top != d->top) {
		d->top = top;
		list_redraw(o);
	}
}

static void list_jump(Object *o, int k)
{
	struct mm_list *d = LD(o);
	int rows = list_rows(o);
	if (k < 0)
		return;
	if (k < d->top)
		list_scroll(o, k);
	else if (k >= d->top + rows)
		list_scroll(o, k - rows + 1);
}

void mm_list_click(Object *o, int x, int y)
{
	struct mm_list *d = LD(o);
	int rows = list_rows(o);
	if (MMO(o)->disabled)
		return;
	if (d->n > rows && x > _mright(o) - SBW) {
		/* the scroll bar: arrows (held down they repeat), the track pages, the thumb follows */
		int y0 = _mtop(o), y1 = _mbottom(o);
		unsigned long next = 0;
		do {
			Point p;
			GetMouse(&p);
			if (TickCount() >= (long)next) {
				if (y < y0 + 10)
					list_scroll(o, d->top - 1);
				else if (y > y1 - 10)
					list_scroll(o, d->top + 1);
				else {
					int span = y1 - y0 + 1 - 20, pos = p.v - y0 - 10;
					list_scroll(o, (d->n - rows) * pos / (span > 0 ? span : 1));
				}
				next = (unsigned long)TickCount() + (next ? 3 : 12);
			}
			if (y >= y0 + 10 && y <= y1 - 10)
				y = p.v;
		} while (StillDown());
		return;
	}
	{
		int k = d->top + (y - _mtop(o)) / row_h();
		if (k >= 0 && k < d->n && k != d->active)
			mm_setattrs(o, MUIA_List_Active, (ULONG)k, TAG_DONE);
	}
}

DISPATCHER(mm_list_dispatch)
{
	struct mm_list *d;
	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->active = -1;
		MMO(obj)->flags |= MMF_INPUT;
		return (ULONG)obj;
	case OM_DISPOSE: {
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; i < d->n; i++)
			mm_free(d->items[i]);
		mm_free(d->items);
		break;
	}
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		ULONG v;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(tl, MUIA_List_Quiet, &v)) {
			d->quiet = v != 0;
			if (!d->quiet)
				list_redraw(obj);
		}
		if (mm_tag_find(tl, MUIA_List_Active, &v)) {
			int a = (int)(LONG)v;
			if (a < 0 || a >= d->n)
				a = -1;
			if (a != d->active) {
				d->active = a;
				list_jump(obj, a);
				list_redraw(obj);
			}
		}
		break;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		if (g->opg_AttrID == MUIA_List_Active) {
			*g->opg_Storage = (ULONG)(LONG)d->active;
			return TRUE;
		}
		break;
	}
	case MUIM_List_InsertSingle: {
		const char *s = (const char *)((ULONG *)msg)[1];
		LONG pos = (LONG)((ULONG *)msg)[2];
		char *c;
		int i;
		d = INST_DATA(cl, obj);
		if (d->n >= d->cap) {
			int nc = d->cap ? d->cap * 2 : 16;
			char **ni = mm_alloc((ULONG)nc * sizeof(char *));
			if (!ni)
				return 0;
			if (d->items) {
				memcpy(ni, d->items, (unsigned long)d->n * sizeof(char *));
				mm_free(d->items);
			}
			d->items = ni;
			d->cap = nc;
		}
		c = mm_strdup(s ? s : "");
		if (!c)
			return 0;
		if (pos < 0 || pos > d->n)
			pos = d->n;
		for (i = d->n; i > pos; i--)
			d->items[i] = d->items[i - 1];
		d->items[pos] = c;
		d->n++;
		if (d->active >= pos && d->active >= 0 && pos < d->n - 1)
			d->active++;
		list_redraw(obj);
		return (ULONG)pos;
	}
	case MUIM_List_Clear: {
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; i < d->n; i++)
			mm_free(d->items[i]);
		d->n = 0;
		d->top = 0;
		d->active = -1;
		list_redraw(obj);
		return 0;
	}
	case MUIM_List_Jump:
		list_jump(obj, (int)(LONG)((ULONG *)msg)[1]);
		return 0;
	case MUIM_AskMinMax: {
		struct MUI_MinMax *m = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
		DoSuperMethodA(cl, obj, msg);
		m->MinWidth += 80;
		m->DefWidth += 200;
		m->MaxWidth = MUI_MAXMAX;
		m->MinHeight += (WORD)(row_h() * 2);
		m->DefHeight += (WORD)(row_h() * 5);
		m->MaxHeight = MUI_MAXMAX;
		return 0;
	}
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if ((((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)) && MMO(obj)->win)
			list_draw(obj);
		return 0;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== Listview: a group around its list ==================== */
struct mm_listview { Object *list; };
ULONG mm_listview_datasize(void) { return sizeof(struct mm_listview); }

DISPATCHER(mm_listview_dispatch)
{
	struct mm_listview *d;
	switch (msg->MethodID) {
	case OM_NEW: {
		Object *l = (Object *)mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_Listview_List, 0);
		if (!l || !(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->list = l;
		mm_group_add(obj, l);
		return (ULONG)obj;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		if (g->opg_AttrID == MUIA_Listview_List) {
			*g->opg_Storage = (ULONG)d->list;
			return TRUE;
		}
		break;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== Menustrip and Menuitem ==================== */
#define MM_MAXMENU 64
struct mm_menustrip {
	const struct NewMenu *nm;
	Object *item[MM_MAXMENU];       /* MUIM_FindUData's answers, made when first asked */
	short mac_menu[MM_MAXMENU];     /* per NewMenu row: the Mac menu id and item (mm_app.c fills) */
	short mac_item[MM_MAXMENU];
};
struct mm_menuitem { Object *strip; int row; };
ULONG mm_menustrip_datasize(void) { return sizeof(struct mm_menustrip); }
ULONG mm_menuitem_datasize(void) { return sizeof(struct mm_menuitem); }

const struct NewMenu *mm_menustrip_nm(Object *strip)
{
	return ((struct mm_menustrip *)INST_DATA(mm_cl_menustrip, strip))->nm;
}

void mm_menustrip_map(Object *strip, int row, int menu, int item)
{
	struct mm_menustrip *d = INST_DATA(mm_cl_menustrip, strip);
	if (row >= 0 && row < MM_MAXMENU) {
		d->mac_menu[row] = (short)menu;
		d->mac_item[row] = (short)item;
	}
}

void *mm_mac_menu(int id);       /* mm_app.c: the Mac menu of an id */

DISPATCHER(mm_menustrip_dispatch)
{
	struct mm_menustrip *d;
	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->nm = (const struct NewMenu *)mm_tag(((struct opSet *)msg)->ops_AttrList, TAG_USER + 1, 0);
		return (ULONG)obj;
	case OM_DISPOSE: {
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; i < MM_MAXMENU; i++)
			if (d->item[i])
				DisposeObject(d->item[i]);
		break;
	}
	case MUIM_FindUData: {
		ULONG u = ((ULONG *)msg)[1];
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; d->nm && d->nm[i].nm_Type != NM_END && i < MM_MAXMENU; i++)
			if ((ULONG)d->nm[i].nm_UserData == u && d->nm[i].nm_Type == NM_ITEM) {
				if (!d->item[i]) {
					struct TagItem t[1] = { { TAG_DONE, 0 } };
					d->item[i] = NewObjectA(mm_cl_menuitem, NULL, t);
					if (d->item[i]) {
						struct mm_menuitem *mi = INST_DATA(mm_cl_menuitem, d->item[i]);
						mi->strip = obj;
						mi->row = i;
						MMO(d->item[i])->udata = u;
					}
				}
				return (ULONG)d->item[i];
			}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

DISPATCHER(mm_menuitem_dispatch)
{
	if (msg->MethodID == OM_SET) {
		struct mm_menuitem *mi = INST_DATA(cl, obj);
		struct mm_menustrip *sd = INST_DATA(mm_cl_menustrip, mi->strip);
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		void *menu = mm_mac_menu(sd->mac_menu[mi->row]);
		ULONG v;
		if (menu && mm_tag_find(tl, MUIA_Menuitem_Checked, &v))
			CheckItem(menu, sd->mac_item[mi->row], v != 0);
		if (menu && mm_tag_find(tl, MUIA_Menuitem_Enabled, &v)) {
			if (v)
				EnableItem(menu, sd->mac_item[mi->row]);
			else
				DisableItem(menu, sd->mac_item[mi->row]);
		}
	}
	return DoSuperMethodA(cl, obj, msg);
}
