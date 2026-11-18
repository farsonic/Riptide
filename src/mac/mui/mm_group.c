/*
 * Riptide - the Mac's MUI (see mm.h): the Group class and MUI's layout.
 *
 * A group lays its children out in a column (the default), a row (MUIA_Group_Horiz), a grid of
 * MUIA_Group_Columns columns, or on top of each other with one shown (MUIA_Group_PageMode).
 * Sizes come from each object's MUIM_AskMinMax, kept in its MUI_AreaData's mad_MinMax as MUI
 * does: a row or column is at least the sum of its children's minimums plus the spacing, and the
 * room beyond that goes to the children by their weights (MUIA_HorizWeight, MUIA_VertWeight;
 * weight 0 keeps the minimum) up to their maximums. Across the row or column, a child is as wide
 * or high as the group up to its maximum, and centred when it is smaller. MUIA_Group_SameSize
 * gives the children of a row or column the size of the largest.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"

struct mm_group {
	int horiz, columns, pagemode, active, samesize;
	int hsp, vsp;
};
ULONG mm_group_datasize(void) { return sizeof(struct mm_group); }

#define GD(o) ((struct mm_group *)INST_DATA(mm_cl_group, o))

void mm_group_add(Object *g, Object *c)
{
	Object **pp;
	MMO(c)->parent = g;
	MMO(c)->next = NULL;
	for (pp = &MMO(g)->child; *pp; pp = &MMO(*pp)->next)
		;
	*pp = c;
}

void mm_group_rem(Object *g, Object *c)
{
	Object **pp;
	for (pp = &MMO(g)->child; *pp; pp = &MMO(*pp)->next)
		if (*pp == c) {
			*pp = MMO(c)->next;
			break;
		}
	MMO(c)->parent = NULL;
	MMO(c)->next = NULL;
}

void mm_children_do(Object *o, Msg msg)
{
	Object *c, *next;
	for (c = MMO(o)->child; c; c = next) {
		next = MMO(c)->next;
		DoMethodA(c, msg);
	}
}

int mm_group_active_page(Object *g)
{
	return GD(g)->active;
}

/* the page group's shown child */
static Object *page_child(Object *g)
{
	Object *c;
	int i = 0;
	for (c = MMO(g)->child; c; c = MMO(c)->next, i++)
		if (i == GD(g)->active)
			return c;
	return MMO(g)->child;
}

/* is child c of group g on screen (the active page of a page group) */
static int child_visible(Object *g, Object *c)
{
	return !GD(g)->pagemode || page_child(g) == c;
}

/* obj's minimum, default and maximum size, asked anew and kept in mad_MinMax */
static void askminmax(Object *o)
{
	struct MUI_MinMax m;
	struct mm_obj *mo = MMO(o);
	struct MUIP_AskMinMax am;
	memset(&m, 0, sizeof(m));
	am.MethodID = MUIM_AskMinMax;
	am.MinMaxInfo = &m;
	DoMethodA(o, (Msg)&am);
	if (mo->fixh_lines || mo->fixh) {
		int h = mo->fixh ? mo->fixh : mo->fixh_lines * mm_font.tf_YSize + muiAreaData(o)->mad_subheight;
		m.MinHeight = m.DefHeight = m.MaxHeight = (WORD)h;
	}
	/* a push button as the Human Interface Guidelines size it (p. 204-205): 20 pixels high at
	   least, and no narrower than OK and Cancel's 59 */
	if (mo->frame == MUIV_Frame_Button) {
		if (m.MinHeight < 20) m.MinHeight = 20;
		if (m.DefHeight < 20) m.DefHeight = 20;
		if (m.MinWidth < 59) m.MinWidth = 59;
		if (m.DefWidth < 59) m.DefWidth = 59;
	}
	if (m.MaxWidth > MUI_MAXMAX || m.MaxWidth < 0)
		m.MaxWidth = MUI_MAXMAX;
	if (m.MaxHeight > MUI_MAXMAX || m.MaxHeight < 0)
		m.MaxHeight = MUI_MAXMAX;
	if (m.MaxWidth < m.MinWidth)
		m.MaxWidth = m.MinWidth;
	if (m.MaxHeight < m.MinHeight)
		m.MaxHeight = m.MinHeight;
	if (m.DefWidth < m.MinWidth)
		m.DefWidth = m.MinWidth;
	if (m.DefWidth > m.MaxWidth)
		m.DefWidth = m.MaxWidth;
	if (m.DefHeight < m.MinHeight)
		m.DefHeight = m.MinHeight;
	if (m.DefHeight > m.MaxHeight)
		m.DefHeight = m.MaxHeight;
	muiAreaData(o)->mad_MinMax = m;
}

static int nchildren(Object *g)
{
	int n = 0;
	Object *c;
	for (c = MMO(g)->child; c; c = MMO(c)->next)
		n++;
	return n;
}

static LONG cap(LONG v)
{
	return v > MUI_MAXMAX ? MUI_MAXMAX : v;
}

/* the group's size from its children's */
static void group_minmax(Object *g, struct MUI_MinMax *m)
{
	struct mm_group *d = GD(g);
	Object *c;
	int n = nchildren(g);
	LONG minw = 0, defw = 0, maxw = 0, minh = 0, defh = 0, maxh = 0;
	for (c = MMO(g)->child; c; c = MMO(c)->next)
		askminmax(c);
	if (!n) {
		maxw = maxh = MUI_MAXMAX;
	} else if (d->pagemode) {
		for (c = MMO(g)->child; c; c = MMO(c)->next) {
			struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
			if (cm->MinWidth > minw) minw = cm->MinWidth;
			if (cm->DefWidth > defw) defw = cm->DefWidth;
			if (cm->MaxWidth > maxw) maxw = cm->MaxWidth;
			if (cm->MinHeight > minh) minh = cm->MinHeight;
			if (cm->DefHeight > defh) defh = cm->DefHeight;
			if (cm->MaxHeight > maxh) maxh = cm->MaxHeight;
		}
	} else if (d->columns > 1) {
		int cols = d->columns, rows = (n + cols - 1) / cols, i, j;
		for (j = 0; j < cols; j++) {
			LONG cmin = 0, cdef = 0, cmax = 0;
			for (i = 0, c = MMO(g)->child; c; c = MMO(c)->next, i++)
				if (i % cols == j) {
					struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
					if (cm->MinWidth > cmin) cmin = cm->MinWidth;
					if (cm->DefWidth > cdef) cdef = cm->DefWidth;
					if (cm->MaxWidth > cmax) cmax = cm->MaxWidth;
				}
			minw += cmin;
			defw += cdef;
			maxw += cmax;
		}
		for (j = 0; j < rows; j++) {
			LONG rmin = 0, rdef = 0, rmax = 0;
			for (i = 0, c = MMO(g)->child; c; c = MMO(c)->next, i++)
				if (i / cols == j) {
					struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
					if (cm->MinHeight > rmin) rmin = cm->MinHeight;
					if (cm->DefHeight > rdef) rdef = cm->DefHeight;
					if (cm->MaxHeight > rmax) rmax = cm->MaxHeight;
				}
			minh += rmin;
			defh += rdef;
			maxh += rmax;
		}
		minw += d->hsp * (cols - 1);
		defw += d->hsp * (cols - 1);
		maxw += d->hsp * (cols - 1);
		minh += d->vsp * (rows - 1);
		defh += d->vsp * (rows - 1);
		maxh += d->vsp * (rows - 1);
	} else {
		/* a row (horiz) or a column: along it the sum, across it the largest */
		LONG same = 0;
		for (c = MMO(g)->child; c; c = MMO(c)->next) {
			struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
			LONG a = d->horiz ? cm->MinWidth : cm->MinHeight;
			if (a > same)
				same = a;
		}
		for (c = MMO(g)->child; c; c = MMO(c)->next) {
			struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
			LONG amin = d->horiz ? cm->MinWidth : cm->MinHeight, adef = d->horiz ? cm->DefWidth : cm->DefHeight,
			     amax = d->horiz ? cm->MaxWidth : cm->MaxHeight;
			LONG xmin = d->horiz ? cm->MinHeight : cm->MinWidth, xdef = d->horiz ? cm->DefHeight : cm->DefWidth,
			     xmax = d->horiz ? cm->MaxHeight : cm->MaxWidth;
			if (d->samesize) {
				amin = same;
				if (adef < same)
					adef = same;
				if (amax < same)
					amax = same;
			}
			if (d->horiz) {
				minw += amin; defw += adef; maxw += amax;
				if (xmin > minh) minh = xmin;
				if (xdef > defh) defh = xdef;
				if (xmax > maxh) maxh = xmax;
			} else {
				minh += amin; defh += adef; maxh += amax;
				if (xmin > minw) minw = xmin;
				if (xdef > defw) defw = xdef;
				if (xmax > maxw) maxw = xmax;
			}
		}
		if (d->horiz) {
			minw += d->hsp * (n - 1); defw += d->hsp * (n - 1); maxw += d->hsp * (n - 1);
		} else {
			minh += d->vsp * (n - 1); defh += d->vsp * (n - 1); maxh += d->vsp * (n - 1);
		}
	}
	m->MinWidth += (WORD)cap(minw);
	m->DefWidth += (WORD)cap(defw);
	m->MaxWidth = (WORD)cap(m->MaxWidth + maxw);
	m->MinHeight += (WORD)cap(minh);
	m->DefHeight += (WORD)cap(defh);
	m->MaxHeight = (WORD)cap(m->MaxHeight + maxh);
}

/* Share total among n items with minimums mn, maximums mx and weights wt: each gets its
 * minimum, then the rest by weight, never past its maximum, until nothing is left or every item
 * with a weight is full. out gets the sizes. */
static void share(int n, const LONG *mn, const LONG *mx, const LONG *wt, LONG total, LONG *out)
{
	LONG rest = total;
	int i, open;
	for (i = 0; i < n; i++) {
		out[i] = mn[i];
		rest -= mn[i];
	}
	while (rest > 0) {
		LONG wsum = 0, given = 0;
		open = 0;
		for (i = 0; i < n; i++)
			if (wt[i] > 0 && out[i] < mx[i]) {
				wsum += wt[i];
				open++;
			}
		if (!open || wsum <= 0)
			break;
		for (i = 0; i < n; i++)
			if (wt[i] > 0 && out[i] < mx[i]) {
				LONG add = rest * wt[i] / wsum;
				if (add < 1)
					add = 1;
				if (out[i] + add > mx[i])
					add = mx[i] - out[i];
				if (given + add > rest)
					add = rest - given;
				out[i] += add;
				given += add;
			}
		if (!given)
			break;
		rest -= given;
	}
}

#define MAXKIDS 64

/* a child in a cell: as big as the cell up to its maximum, centred */
static void place(Object *c, int l, int t, int w, int h)
{
	struct MUI_MinMax *cm = &muiAreaData(c)->mad_MinMax;
	int cw = w < cm->MaxWidth ? w : cm->MaxWidth, ch = h < cm->MaxHeight ? h : cm->MaxHeight;
	if (cw < cm->MinWidth)
		cw = cm->MinWidth;
	if (ch < cm->MinHeight)
		ch = cm->MinHeight;
	mm_layout(c, l + (w > cw ? (w - cw) / 2 : 0), t + (h > ch ? (h - ch) / 2 : 0), cw, ch);
}

static void group_layout(Object *g)
{
	struct mm_group *d = GD(g);
	Object *c, *kids[MAXKIDS];
	LONG mn[MAXKIDS], mx[MAXKIDS], wt[MAXKIDS], sz[MAXKIDS];
	int n = 0, i, l = _mleft(g), t = _mtop(g), w = _mwidth(g), h = _mheight(g);
	for (c = MMO(g)->child; c && n < MAXKIDS; c = MMO(c)->next)
		kids[n++] = c;
	if (!n)
		return;
	if (d->pagemode) {
		for (i = 0; i < n; i++)
			place(kids[i], l, t, w, h);
		return;
	}
	if (d->columns > 1) {
		int cols = d->columns, rows = (n + cols - 1) / cols, j;
		LONG cw[MAXKIDS], rh[MAXKIDS];
		for (j = 0; j < cols; j++) {
			mn[j] = 0; mx[j] = 0; wt[j] = 0;
			for (i = j; i < n; i += cols) {
				struct MUI_MinMax *cm = &muiAreaData(kids[i])->mad_MinMax;
				if (cm->MinWidth > mn[j]) mn[j] = cm->MinWidth;
				if (cm->MaxWidth > mx[j]) mx[j] = cm->MaxWidth;
				if (MMO(kids[i])->hweight > wt[j]) wt[j] = MMO(kids[i])->hweight;
			}
		}
		share(cols, mn, mx, wt, w - d->hsp * (cols - 1), cw);
		for (j = 0; j < rows; j++) {
			mn[j] = 0; mx[j] = 0; wt[j] = 0;
			for (i = j * cols; i < n && i < (j + 1) * cols; i++) {
				struct MUI_MinMax *cm = &muiAreaData(kids[i])->mad_MinMax;
				if (cm->MinHeight > mn[j]) mn[j] = cm->MinHeight;
				if (cm->MaxHeight > mx[j]) mx[j] = cm->MaxHeight;
				if (MMO(kids[i])->vweight > wt[j]) wt[j] = MMO(kids[i])->vweight;
			}
		}
		share(rows, mn, mx, wt, h - d->vsp * (rows - 1), rh);
		{
			int y = t;
			for (j = 0; j < rows; j++) {
				int x = l, k;
				for (k = 0; k < cols && j * cols + k < n; k++) {
					place(kids[j * cols + k], x, y, (int)cw[k], (int)rh[j]);
					x += (int)cw[k] + d->hsp;
				}
				y += (int)rh[j] + d->vsp;
			}
		}
		return;
	}
	{
		LONG same = 0;
		for (i = 0; i < n; i++) {
			struct MUI_MinMax *cm = &muiAreaData(kids[i])->mad_MinMax;
			mn[i] = d->horiz ? cm->MinWidth : cm->MinHeight;
			mx[i] = d->horiz ? cm->MaxWidth : cm->MaxHeight;
			wt[i] = d->horiz ? MMO(kids[i])->hweight : MMO(kids[i])->vweight;
			if (mn[i] > same)
				same = mn[i];
		}
		if (d->samesize)
			for (i = 0; i < n; i++) {
				mn[i] = same;
				if (mx[i] < same)
					mx[i] = same;
			}
		share(n, mn, mx, wt, (d->horiz ? w - d->hsp * (n - 1) : h - d->vsp * (n - 1)), sz);
		{
			int pos = d->horiz ? l : t;
			for (i = 0; i < n; i++) {
				if (d->horiz) {
					place(kids[i], pos, t, (int)sz[i], h);
					pos += (int)sz[i] + d->hsp;
				} else {
					place(kids[i], l, pos, w, (int)sz[i]);
					pos += (int)sz[i] + d->vsp;
				}
			}
		}
	}
}

void mm_layout(Object *o, int l, int t, int w, int h)
{
	struct MUI_AreaData *ad = muiAreaData(o);
	ad->mad_Box.Left = (WORD)l;
	ad->mad_Box.Top = (WORD)t;
	ad->mad_Box.Width = (WORD)w;
	ad->mad_Box.Height = (WORD)h;
	if (MMO(o)->flags & MMF_GROUP)
		group_layout(o);
}

/* the window's root: its size */
void mm_root_minmax(Object *root)
{
	askminmax(root);
}

Object *mm_hit(Object *o, int x, int y, ULONG need)
{
	Object *c, *best = NULL;
	if (!(MMO(o)->flags & MMF_SHOWN) || !mm_inside(o, x, y))
		return NULL;
	if (MMO(o)->flags & MMF_GROUP)
		for (c = MMO(o)->child; c; c = MMO(c)->next) {
			Object *h;
			if (!child_visible(o, c))
				continue;
			if ((h = mm_hit(c, x, y, need)) != NULL)
				best = h;
		}
	if (best)
		return best;
	return (!need || (MMO(o)->flags & need)) ? o : NULL;
}

static void show_children(Object *g, ULONG method)
{
	Object *c;
	ULONG m = method;
	for (c = MMO(g)->child; c; c = MMO(c)->next)
		if (child_visible(g, c))
			DoMethodA(c, (Msg)&m);
}

static void setup_child(Object *g, Object *c)
{
	struct { ULONG id; struct MUI_RenderInfo *ri; } s;
	s.id = MUIM_Setup;
	s.ri = muiRenderInfo(g);
	DoMethodA(c, (Msg)&s);
}

DISPATCHER(mm_group_dispatch)
{
	struct mm_group *d;
	switch (msg->MethodID) {
	case OM_NEW: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList, *t;
		ULONG v;
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		MMO(obj)->flags |= MMF_GROUP;
		d = INST_DATA(cl, obj);
		d->hsp = d->vsp = 4;
		d->horiz = (int)mm_tag(tl, MUIA_Group_Horiz, FALSE);
		d->columns = (int)mm_tag(tl, MUIA_Group_Columns, 0);
		d->pagemode = (int)mm_tag(tl, MUIA_Group_PageMode, FALSE);
		d->samesize = (int)mm_tag(tl, MUIA_Group_SameSize, FALSE);
		if (mm_tag_find(tl, MUIA_Group_Spacing, &v))
			d->hsp = d->vsp = (int)v;
		if (mm_tag_find(tl, MUIA_Group_HorizSpacing, &v))
			d->hsp = (int)v;
		if (mm_tag_find(tl, MUIA_Group_VertSpacing, &v))
			d->vsp = (int)v;
		d->active = (int)mm_tag(tl, MUIA_Group_ActivePage, 0);
		/* every MUIA_Group_Child, in order; a NULL child makes the group fail, as in MUI */
		for (t = tl; t && t->ti_Tag != TAG_DONE; t++) {
			if (t->ti_Tag == TAG_MORE) {
				t = (const struct TagItem *)t->ti_Data - 1;
				continue;
			}
			if (t->ti_Tag == MUIA_Group_Child) {
				if (!t->ti_Data) {
					ULONG m = OM_DISPOSE;
					CoerceMethodA(cl, obj, (Msg)&m);
					return 0;
				}
				mm_group_add(obj, (Object *)t->ti_Data);
			}
		}
		return (ULONG)obj;
	}
	case OM_DISPOSE: {
		Object *c, *next;
		for (c = MMO(obj)->child; c; c = next) {
			next = MMO(c)->next;
			DisposeObject(c);
		}
		MMO(obj)->child = NULL;
		break;
	}
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		ULONG v;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(tl, MUIA_Group_ActivePage, &v) && d->pagemode) {
			int n = nchildren(obj), p = (int)v;
			if (p < 0)
				p = 0;
			if (p >= n)
				p = n - 1;
			if (p != d->active) {
				Object *old = page_child(obj), *nw;
				ULONG m;
				if (old && (MMO(obj)->flags & MMF_SHOWN)) {
					m = MUIM_Hide;
					DoMethodA(old, (Msg)&m);
				}
				d->active = p;
				nw = page_child(obj);
				if (nw && (MMO(obj)->flags & MMF_SHOWN)) {
					m = MUIM_Show;
					DoMethodA(nw, (Msg)&m);
					/* the page area again: its background, then the page */
					MUI_Redraw(obj, MADF_DRAWOBJECT);
				}
			}
		}
		break;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		if (g->opg_AttrID == MUIA_Group_ActivePage) {
			*g->opg_Storage = (ULONG)d->active;
			return TRUE;
		}
		break;
	}
	case OM_ADDMEMBER: {
		Object *c = ((struct { ULONG id; Object *o; } *)msg)->o;
		if (!c)
			return 0;
		mm_group_add(obj, c);
		if (MMO(obj)->flags & MMF_SETUP)
			setup_child(obj, c);
		return TRUE;
	}
	case OM_REMMEMBER: {
		Object *c = ((struct { ULONG id; Object *o; } *)msg)->o;
		ULONG m;
		if (!c)
			return 0;
		if (MMO(c)->flags & MMF_SHOWN) {
			m = MUIM_Hide;
			DoMethodA(c, (Msg)&m);
		}
		if (MMO(c)->flags & MMF_SETUP) {
			m = MUIM_Cleanup;
			DoMethodA(c, (Msg)&m);
		}
		mm_group_rem(obj, c);
		return TRUE;
	}
	case MUIM_Group_InitChange:
		return TRUE;
	case MUIM_Group_ExitChange: {
		struct mm_win *w = MMO(obj)->win;
		if (w)
			mm_win_layout(w);
		return TRUE;
	}
	case MUIM_Setup: {
		Object *c;
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		for (c = MMO(obj)->child; c; c = MMO(c)->next)
			DoMethodA(c, msg);
		return TRUE;
	}
	case MUIM_Cleanup: {
		Object *c;
		for (c = MMO(obj)->child; c; c = MMO(c)->next)
			DoMethodA(c, msg);
		break;
	}
	case MUIM_AskMinMax: {
		DoSuperMethodA(cl, obj, msg);
		group_minmax(obj, ((struct MUIP_AskMinMax *)msg)->MinMaxInfo);
		return 0;
	}
	case MUIM_Show:
		DoSuperMethodA(cl, obj, msg);
		show_children(obj, MUIM_Show);
		return TRUE;
	case MUIM_Hide: {
		Object *c;
		ULONG m = MUIM_Hide;
		for (c = MMO(obj)->child; c; c = MMO(c)->next)
			if (MMO(c)->flags & MMF_SHOWN)
				DoMethodA(c, (Msg)&m);
		break;
	}
	case MUIM_Draw: {
		Object *c;
		DoSuperMethodA(cl, obj, msg);
		for (c = MMO(obj)->child; c; c = MMO(c)->next)
			if (child_visible(obj, c) && (MMO(c)->flags & MMF_SHOWN))
				DoMethodA(c, msg);
		return 0;
	}
	case MUIM_FindUData: {
		Object *c;
		if (MMO(obj)->udata == ((ULONG *)msg)[1])
			return (ULONG)obj;
		for (c = MMO(obj)->child; c; c = MMO(c)->next) {
			ULONG r = DoMethodA(c, msg);
			if (r)
				return r;
		}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}
