/*
 * Riptide - RiptidePrefs (MUI): the Status page. What Riptide is doing
 * now, read once a second while the page is shown (status_amiga.c reads
 * the tables, prefs_status.c turns them into numbers and text):
 *
 *   a summary (running, uptime, CPU, memory),
 *   a graph of all traffic and one of the CPU share,
 *   tabs: Interfaces (each with its own graph), Connections (a table and
 *   a graph of the chosen connection's round trip time and window), Names
 *   and time, Counters.
 *
 * The classes here draw in the theme like those of mui_classes.c:
 *   graph   a 60 second history of two values (a struct rp_hist)
 *   tabs    a row of labels that switches a page group (keys 1 to 4)
 *   table   rows of columns separated by '\t'; a row can be chosen
 *   ifview  every interface: three lines, its speeds and a small graph
 *   stext   a fixed number of lines of text that change
 * The total-traffic graph also owns the timer: it starts when the page is
 * shown and stops when it is hidden, so nothing is read on other pages.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_theme.h"
#include "prefs/prefs_fields.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_status.h"
#include "prefs/mui_status.h"

#define MUIA_Graph_Hist    (RTAG + 0x701)  /* (init, set) struct rp_hist *; set again to redraw */
#define MUIA_Graph_Kind    (RTAG + 0x702)  /* (init) GK_* */
#define MUIA_Graph_Title   (RTAG + 0x703)  /* (init) the name in the legend */
#define MUIA_Graph_Lines   (RTAG + 0x704)  /* (init) height in text lines */
#define MUIA_Graph_Ticker  (RTAG + 0x705)  /* (init) runs the page's timer */
#define MUIM_Graph_Tick    (RTAG + 0x780)  /* the timer */
#define MUIA_Tabs_Labels   (RTAG + 0x801)  /* (init) const char *const *, NULL-ended */
#define MUIA_Tabs_Active   (RTAG + 0x802)  /* (init, set, get, notify) */
#define MUIA_Table_Rows    (RTAG + 0x901)  /* (set) const char *const *; set again to redraw */
#define MUIA_Table_NRows   (RTAG + 0x902)  /* (set, before Rows) */
#define MUIA_Table_Header  (RTAG + 0x903)  /* (init) the column titles, '\t' between */
#define MUIA_Table_Active  (RTAG + 0x904)  /* (set, get, notify) the chosen row, -1 none */
#define MUIA_IfView_Model  (RTAG + 0xa01)  /* (init) struct rp_status_model *; set Update to redraw */
#define MUIA_IfView_Update (RTAG + 0xa02)  /* (set) */
#define MUIA_SText_Text    (RTAG + 0xb01)  /* (init, set) lines separated by '\n' (not copied) */
#define MUIA_SText_Lines   (RTAG + 0xb02)  /* (init) the lines it has room for */

enum { GK_BYTES, GK_CPU, GK_CONN };

#define RAW_1 0x01
#define RAW_LEFT  0x4f
#define RAW_RIGHT 0x4e
#ifndef SELECTDOWN
#define SELECTDOWN 0x68
#endif

static struct MUI_CustomClass *graph_mcc, *tabs_mcc, *table_mcc, *ifview_mcc, *stext_mcc;

/* ---- drawing helpers ---- */
/* text cut to fit maxw pixels (the longest start that fits, found by
 * halving: a few TextLength() calls, not one per character) */
static void text_fit(struct RastPort *rp, LONG x, LONG y, LONG maxw, const char *s, LONG pen, int bold)
{
	char t[160];
	int n, lo, hi;

	rt_strlcpy(t, s, sizeof(t));
	n = (int)strlen(t);
	if (n && theme_textlen(rp, t) > maxw) {
		lo = 0;
		hi = n;
		while (lo < hi) {
			int mid = (lo + hi + 1) / 2;
			char c = t[mid];
			t[mid] = 0;
			if (theme_textlen(rp, t) <= maxw)
				lo = mid;
			else
				hi = mid - 1;
			t[mid] = c;
		}
		n = lo;
		t[n] = 0;
	}
	if (n > 0)
		theme_text(rp, x, y, t, pen, bold);
}

/* A line of text that changes: drawn again only when it differs from what
 * is on screen (cache, at least 120 bytes), its old pixels cleared first.
 * The 68020 and a planar screen make every pixel count. */
static void text_line(struct RastPort *rp, LONG x, LONG y, LONG w, const char *s, LONG pen, int bold, char *cache,
                      int redraw)
{
	if (!redraw && !strcmp(cache, s))
		return;
	if (!redraw)
		theme_fill(rp, x, y, x + w - 1, y + theme_fh(), tpen(T_WINDOW));
	text_fit(rp, x, y, w, s, pen, bold);
	rt_strlcpy(cache, s, 120);
}

/* ---- graphs ----
 * A graph of an rp_hist in a box: a legend line on top, then the plot.
 * Bytes: receive as filled columns (accent), send as a line (green), one
 * scale. CPU: busy share as columns, scale 100 %. Connection: the window
 * as columns, the round trip time as a line (orange), each on its own
 * scale. Every sample is a column cw pixels wide; the newest is at the
 * right. When one sample was added and the scale stayed, the plot is
 * scrolled by one column and only that is drawn (graph_update()). */
struct gstate {
	const struct rp_hist *h;
	u32 pushes, sc0, sc1;
	LONG x0, y0, x1, y1;
	int legend, valid;
};

struct gbox {
	LONG py0, py1, ph, cw, xr;
};

static void graph_box(LONG x0, LONG y0, LONG x1, LONG y1, int legend, struct gbox *b)
{
	b->py0 = legend ? y0 + theme_fh() + 2 : y0;
	b->py1 = y1;
	b->ph = b->py1 - b->py0 - 1;
	b->cw = (x1 - x0 - 1) / RP_HIST;
	if (b->cw < 1)
		b->cw = 1;
	b->xr = x1 - 1;
}

static void graph_scales(const struct rp_hist *h, int kind, u32 *sc0, u32 *sc1)
{
	if (kind == GK_CPU) {
		*sc0 = *sc1 = 1000;
	} else if (kind == GK_CONN) {
		*sc0 = rp_nice_scale(rp_hist_max(h, 0), 10);       /* RTT, 0.1 ms */
		*sc1 = rp_nice_scale(rp_hist_max(h, 1), 10);       /* window, packets */
	} else {
		*sc0 = *sc1 = rp_nice_scale(rp_hist_max(h, -1), 1024);
	}
}

/* the legend: name and newest values; the scale on the right */
static void graph_legend(struct RastPort *rp, LONG x0, LONG y0, LONG x1, const struct rp_hist *h, int kind,
                         const char *title, u32 sc0, u32 sc1)
{
	char lg[120], a[24], b[24], sc[24];
	u32 v0 = h->n ? rp_hist_at(h, 0, h->n - 1) : 0, v1 = h->n ? rp_hist_at(h, 1, h->n - 1) : 0;
	LONG sw;

	if (kind == GK_CPU) {
		rt_snprintf(lg, sizeof(lg), "%s %lu %%", title, (unsigned long)((v0 + 5) / 10));
		rt_strlcpy(sc, "100 %", sizeof(sc));
	} else if (kind == GK_CONN) {
		rp_fmt_ms(a, sizeof(a), v0 * 100u);
		rt_snprintf(lg, sizeof(lg), "%s: round trip %s, window %lu", title, a, (unsigned long)v1);
		rp_fmt_ms(b, sizeof(b), sc0 * 100u);
		rt_snprintf(sc, sizeof(sc), "%s / %lu", b, (unsigned long)sc1);
	} else {
		rp_fmt_rate(a, sizeof(a), v0);
		rp_fmt_rate(b, sizeof(b), v1);
		rt_snprintf(lg, sizeof(lg), "%s%s%s in, %s out", title, title[0] ? ": " : "", a, b);
		rp_fmt_rate(sc, sizeof(sc), sc0);
	}
	theme_fill(rp, x0, y0, x1, y0 + theme_fh(), tpen(T_WINDOW));
	sw = theme_textlen(rp, sc);
	text_fit(rp, x0, y0, x1 - x0 - sw - 8, lg, tpen(T_TEXT), 0);
	theme_text(rp, x1 - sw, y0, sc, tpen(T_DIM), 0);
}

/* the column of sample i (0 the oldest of h->n) and the line to it from
 * sample i - 1; mid: the half-scale line under it too (a scrolled plot) */
static void graph_column(struct RastPort *rp, const struct gbox *b, LONG x0, const struct rp_hist *h, int i, int kind,
                         u32 sc0, u32 sc1, int mid_line)
{
	int slot = RP_HIST - h->n + i;
	LONG cx1 = b->xr - (LONG)(RP_HIST - 1 - slot) * b->cw, cx0 = cx1 - b->cw + 1, mid = b->py0 + b->ph / 2;
	u32 v = rp_hist_at(h, kind == GK_CONN ? 1 : 0, i);
	int yy = rp_scale_y(v, kind == GK_CONN ? sc1 : sc0, (int)(b->ph - 1));

	if (cx1 <= x0)
		return;
	if (cx0 <= x0)
		cx0 = x0 + 1;
	if (mid_line)
		theme_line(rp, cx0, mid, cx1, mid, tpen(T_HOVER));
	if (yy > 0)
		theme_fill(rp, cx0, b->py1 - yy, cx1, b->py1 - 1, tpen(T_ACCENT));
	if (kind != GK_CPU) {
		LONG pen = tpen(kind == GK_CONN ? T_ORANGE : T_GREEN);
		int s = kind == GK_CONN ? 0 : 1;
		u32 sc = kind == GK_CONN ? sc0 : sc1;
		LONG cy = b->py1 - 1 - rp_scale_y(rp_hist_at(h, s, i), sc, (int)(b->ph - 2));
		LONG cx = (cx0 + cx1) / 2;
		if (i > 0) {
			LONG px = cx - b->cw;
			LONG py = b->py1 - 1 - rp_scale_y(rp_hist_at(h, s, i - 1), sc, (int)(b->ph - 2));
			if (px > x0) {
				theme_line(rp, px, py, cx, cy, pen);
				theme_line(rp, px, py - 1, cx, cy - 1, pen);
			}
		} else {
			theme_fill(rp, cx, cy - 1, cx, cy, pen);
		}
	}
}

static void graph_full(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, const struct rp_hist *h, int kind,
                       const char *title, int legend, struct gstate *gs)
{
	struct gbox b;
	u32 sc0, sc1;
	int i;

	gs->valid = 0;
	if (x1 - x0 < 20 || y1 - y0 < 8)
		return;
	graph_box(x0, y0, x1, y1, legend, &b);
	if (b.ph < 4)
		return;
	graph_scales(h, kind, &sc0, &sc1);
	if (legend)
		graph_legend(rp, x0, y0, x1, h, kind, title, sc0, sc1);
	theme_fill(rp, x0, b.py0, x1, b.py1, tpen(T_HEAD));
	theme_rect(rp, x0, b.py0, x1, b.py1, tpen(T_BORDER));
	theme_line(rp, x0 + 1, b.py0 + b.ph / 2, x1 - 1, b.py0 + b.ph / 2, tpen(T_HOVER));
	for (i = 0; i < h->n; i++)
		graph_column(rp, &b, x0, h, i, kind, sc0, sc1, 0);
	gs->h = h;
	gs->pushes = h->pushes;
	gs->sc0 = sc0;
	gs->sc1 = sc1;
	gs->x0 = x0;
	gs->y0 = y0;
	gs->x1 = x1;
	gs->y1 = y1;
	gs->legend = legend;
	gs->valid = 1;
}

/* only what is new since the last drawing, when that is one sample on the
 * same scale; else everything */
static void graph_update(struct RastPort *rp, LONG x0, LONG y0, LONG x1, LONG y1, const struct rp_hist *h, int kind,
                         const char *title, int legend, struct gstate *gs)
{
	struct gbox b;
	u32 sc0, sc1;

	graph_scales(h, kind, &sc0, &sc1);
	if (!gs->valid || gs->h != h || h->pushes != gs->pushes + 1 || sc0 != gs->sc0 || sc1 != gs->sc1 ||
	    gs->x0 != x0 || gs->y0 != y0 || gs->x1 != x1 || gs->y1 != y1 || gs->legend != legend) {
		if (!gs->valid || gs->h != h || h->pushes != gs->pushes)
			graph_full(rp, x0, y0, x1, y1, h, kind, title, legend, gs);
		return;
	}
	graph_box(x0, y0, x1, y1, legend, &b);
	if (legend)
		graph_legend(rp, x0, y0, x1, h, kind, title, sc0, sc1);
	/* everything one column to the left; the new one at the right */
	SetBPen(rp, (ULONG)tpen(T_HEAD));
	ScrollRaster(rp, b.cw, 0, x0 + 1, b.py0 + 1, x1 - 1, b.py1 - 1);
	graph_column(rp, &b, x0, h, h->n - 1, kind, sc0, sc1, 1);
	gs->pushes = h->pushes;
}

/* ==================================================================
 * graph
 * ================================================================== */
struct graph_data {
	const struct rp_hist *hist;
	const char *title;
	int kind, lines, shown, ticker, timer_on;
	struct gstate gs;
	struct MUI_InputHandlerNode ihn;
};

static void (*tick_fn)(void);

static void graph_timer(Object *obj, struct graph_data *d, int on)
{
	if (!d->ticker || on == d->timer_on)
		return;
	if (on) {
		memset(&d->ihn, 0, sizeof(d->ihn));
		d->ihn.ihn_Object = obj;
		d->ihn.ihn_millis = 1000;
		d->ihn.ihn_Flags = MUIIHNF_TIMER;
		d->ihn.ihn_Method = MUIM_Graph_Tick;
		mui_do(_app(obj), MUIM_Application_AddInputHandler, (ULONG)&d->ihn);
	} else {
		mui_do(_app(obj), MUIM_Application_RemInputHandler, (ULONG)&d->ihn);
	}
	d->timer_on = on;
}

DISPATCHER(graph_dispatch)
{
	struct graph_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->hist = (const struct rp_hist *)mc_tagval(msg, MUIA_Graph_Hist, 0);
		d->kind = (int)mc_tagval(msg, MUIA_Graph_Kind, GK_BYTES);
		d->title = (const char *)mc_tagval(msg, MUIA_Graph_Title, (ULONG)"");
		d->lines = (int)mc_tagval(msg, MUIA_Graph_Lines, 4);
		d->ticker = (int)mc_tagval(msg, MUIA_Graph_Ticker, 0);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Graph_Hist)) {
			d->hist = (const struct rp_hist *)mc_tagval(msg, MUIA_Graph_Hist, 0);
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWUPDATE);
		}
		break;
	case MUIM_AskMinMax: {
		LONG h;
		d = INST_DATA(cl, obj);
		h = (LONG)d->lines * (theme_fh() + 1) + 4;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 80, 240, MUI_MAXMAX, 3 * (theme_fh() + 1) + 4, h, h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		graph_timer(obj, d, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		d = INST_DATA(cl, obj);
		d->shown = 1;
		d->gs.valid = 0;
		if (d->ticker && !d->timer_on) {
			graph_timer(obj, d, 1);
			/* the first read at once, after this layout */
			mui_do(_app(obj), MUIM_Application_PushMethod, (ULONG)obj, 1, MUIM_Graph_Tick);
		}
		break;
	case MUIM_Hide:
		d = INST_DATA(cl, obj);
		d->shown = 0;
		graph_timer(obj, d, 0);
		break;
	case MUIM_Graph_Tick:
		d = INST_DATA(cl, obj);
		if (d->shown && tick_fn)
			tick_fn();
		return 0;
	case MUIM_Draw: {
		ULONG f;
		/* the Area class fills in the flags (MUI_Redraw() leaves them to it) */
		DoSuperMethodA(cl, obj, msg);
		f = ((struct MUIP_Draw *)msg)->flags;
		d = INST_DATA(cl, obj);
		if (!d->hist)
			return 0;
		if (f & MADF_DRAWOBJECT)
			graph_full(_rp(obj), _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), d->hist, d->kind,
			           tr(d->title), 1, &d->gs);
		else if (f & MADF_DRAWUPDATE)
			graph_update(_rp(obj), _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), d->hist, d->kind,
			             tr(d->title), 1, &d->gs);
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * tabs
 * ================================================================== */
struct tabs_data {
	struct MUI_EventHandlerNode ehn;
	const char *const *labels;
	int n, active, hot, shown;
};

static LONG tab_w(struct RastPort *rp, const char *s)
{
	return theme_textlen(rp, s) + 20;
}

/* the tab under x, or -1 */
static int tabs_hit(struct tabs_data *d, Object *obj, LONG x)
{
	struct RastPort rp;
	LONG lx = _mleft(obj);
	int i;

	InitRastPort(&rp);
	for (i = 0; i < d->n; i++) {
		LONG w = tab_w(&rp, d->labels[i]);
		if (x >= lx && x < lx + w)
			return i;
		lx += w;
	}
	return -1;
}

static void tabs_draw(struct tabs_data *d, Object *obj)
{
	struct RastPort *rp = _rp(obj);
	LONG x = _mleft(obj), t = _mtop(obj), b = _mbottom(obj);
	int i;

	theme_line(rp, _mleft(obj), b, _mright(obj), b, tpen(T_BORDER));
	for (i = 0; i < d->n; i++) {
		LONG w = tab_w(rp, d->labels[i]);
		if (i == d->hot && i != d->active)
			theme_fill(rp, x, t, x + w - 1, b - 1, tpen(T_HOVER));
		theme_text(rp, x + 10, t + (b - t - theme_fh()) / 2, d->labels[i], tpen(i == d->active ? T_TEXT : T_DIM),
		           i == d->active);
		if (i == d->active)
			theme_fill(rp, x + 4, b - 2, x + w - 5, b, tpen(T_ACCENT));
		x += w;
	}
}

static void tabs_go(Object *obj, int i)
{
	struct TagItem t[2];

	t[0].ti_Tag = MUIA_Tabs_Active;
	t[0].ti_Data = (ULONG)i;
	t[1].ti_Tag = TAG_DONE;
	t[1].ti_Data = 0;
	SetAttrsA(obj, t);
}

DISPATCHER(tabs_dispatch)
{
	struct tabs_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->labels = (const char *const *)mc_tagval(msg, MUIA_Tabs_Labels, 0);
		while (d->labels && d->labels[d->n])
			d->n++;
		d->active = (int)mc_tagval(msg, MUIA_Tabs_Active, 0);
		d->hot = -1;
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Tabs_Active)) {
			int a = (int)mc_tagval(msg, MUIA_Tabs_Active, 0);
			if (a >= 0 && a < d->n && a != d->active) {
				d->active = a;
				if (d->shown)
					MUI_Redraw(obj, MADF_DRAWOBJECT);
			}
		}
		break;
	case OM_GET:
		if (((struct opGet *)msg)->opg_AttrID == MUIA_Tabs_Active) {
			*((struct opGet *)msg)->opg_Storage = (ULONG)((struct tabs_data *)INST_DATA(cl, obj))->active;
			return TRUE;
		}
		break;
	case MUIM_AskMinMax: {
		LONG w = 0, h = theme_fh() + (theme_compact() ? 4 : 8);
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; i < d->n; i++)
			w += mc_measure(d->labels[i], 0) + 20;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, w, w, MUI_MAXMAX, h, h, h);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY, 1);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		((struct tabs_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct tabs_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw:
		DoSuperMethodA(cl, obj, msg);
		if (((struct MUIP_Draw *)msg)->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			tabs_draw(INST_DATA(cl, obj), obj);
		return 0;
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		int i;
		d = INST_DATA(cl, obj);
		if (!im || !d->shown)
			return 0;
		if (im->Class == IDCMP_MOUSEMOVE) {
			i = mc_inside(obj, im) ? tabs_hit(d, obj, im->MouseX) : -1;
			if (i != d->hot) {
				d->hot = i;
				MUI_Redraw(obj, MADF_DRAWOBJECT);
			}
			return 0;
		}
		if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN && mc_inside(obj, im)) {
			i = tabs_hit(d, obj, im->MouseX);
			if (i >= 0)
				tabs_go(obj, i);
			return MUI_EventHandlerRC_Eat;
		}
		/* 1 to 4 choose a tab; cursor left and right step */
		if (im->Class == IDCMP_RAWKEY && !(im->Code & IECODE_UP_PREFIX)) {
			if (im->Code >= RAW_1 && im->Code < RAW_1 + d->n) {
				tabs_go(obj, im->Code - RAW_1);
				return MUI_EventHandlerRC_Eat;
			}
			if (im->Code == RAW_LEFT || im->Code == RAW_RIGHT) {
				tabs_go(obj, (d->active + (im->Code == RAW_LEFT ? d->n - 1 : 1)) % d->n);
				return MUI_EventHandlerRC_Eat;
			}
		}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * table
 * ================================================================== */
#define TB_COLS 8
#define TB_ROWS RP_ST_NCONN
struct table_data {
	struct MUI_EventHandlerNode ehn;
	const char *header;
	const char *const *rows;
	int n, active, hot, shown;
	/* what is on screen, for updates */
	int valid, drawn_n, drawn_active, drawn_hot, drawn_fit;
	LONG drawn_x[TB_COLS + 1], drawn_w;
	char cache[TB_ROWS][120];
};

static LONG tb_rowh(void)
{
	return theme_fh() + (theme_compact() ? 1 : 3);
}

/* cell c of a '\t' row into buf */
static void cell(const char *row, int c, char *buf, int len)
{
	int k = 0;

	while (c > 0 && *row) {
		if (*row++ == '\t')
			c--;
	}
	while (*row && *row != '\t' && k < len - 1)
		buf[k++] = *row++;
	buf[k] = 0;
}

static int ncols(const char *row)
{
	int n = 1;

	while (*row)
		n += *row++ == '\t';
	return n > TB_COLS ? TB_COLS : n;
}

/* The x of each column in a width of w: each as wide as its widest cell,
 * the addresses (columns 1 and 2) giving way when that is too wide. */
static void tb_layout(struct table_data *d, struct RastPort *rp, LONG w, LONG *x, int *nc)
{
	LONG cw[TB_COLS], sum = 0;
	char t[64];
	int c, r, n = ncols(d->header);

	for (c = 0; c < n; c++) {
		cell(d->header, c, t, sizeof(t));
		cw[c] = theme_textlen(rp, t) + 10;
		for (r = 0; r < d->n; r++) {
			LONG tw;
			cell(d->rows[r], c, t, sizeof(t));
			if ((tw = theme_textlen(rp, t) + 10) > cw[c])
				cw[c] = tw;
		}
		sum += cw[c];
	}
	while (sum > w && n > 2) {
		/* shrink the wider of the two address columns */
		int k = cw[1] >= cw[2] ? 1 : 2;
		LONG cut = sum - w, room = cw[k] - 60;
		if (room <= 0)
			break;
		if (cut > room)
			cut = room;
		cw[k] -= cut;
		sum -= cut;
	}
	x[0] = 0;
	for (c = 1; c <= n; c++)
		x[c] = x[c - 1] + cw[c - 1];
	*nc = n;
}

/* row r at y: its background (chosen, under the pointer, plain) and cells */
static void tb_row(struct table_data *d, Object *obj, struct RastPort *rp, const LONG *x, int nc, int r, LONG ry)
{
	LONG l = _mleft(obj) + 2, rh = tb_rowh();
	char t[64];
	int c;

	if (r == d->active) {
		theme_fill(rp, _mleft(obj) + 1, ry, _mright(obj) - 1, ry + rh - 1, tpen(T_PRESSED));
		theme_fill(rp, _mleft(obj) + 1, ry, _mleft(obj) + 2, ry + rh - 1, tpen(T_ACCENT));
	} else {
		theme_fill(rp, _mleft(obj) + 1, ry, _mright(obj) - 1, ry + rh - 1, tpen(r == d->hot ? T_HOVER : T_HEAD));
	}
	for (c = 0; c < nc; c++) {
		cell(d->rows[r], c, t, sizeof(t));
		text_fit(rp, l + x[c], ry + (rh - theme_fh()) / 2, x[c + 1] - x[c] - 6, t, tpen(T_TEXT), 0);
	}
	rt_strlcpy(d->cache[r], d->rows[r], sizeof(d->cache[r]));
}

static void tb_draw(struct table_data *d, Object *obj, int update)
{
	struct RastPort *rp = _rp(obj);
	LONG x[TB_COLS + 1], l = _mleft(obj) + 2, rh = tb_rowh(), y = _mtop(obj), fh = theme_fh();
	LONG w = _mwidth(obj) - 4;
	char t[64];
	int nc, c, r, fit;

	tb_layout(d, rp, w, x, &nc);
	fit = (int)((_mbottom(obj) - y - rh) / rh);
	/* only the rows that changed, when the columns and rows stayed */
	if (update && d->valid && d->drawn_n == d->n && d->drawn_fit == fit && d->drawn_w == w &&
	    d->drawn_active == d->active && d->drawn_hot == d->hot && d->n <= fit && d->n > 0 &&
	    !memcmp(d->drawn_x, x, sizeof(LONG) * (size_t)(nc + 1))) {
		for (r = 0; r < d->n; r++)
			if (strcmp(d->cache[r], d->rows[r]))
				tb_row(d, obj, rp, x, nc, r, y + rh + (LONG)r * rh);
		return;
	}
	theme_fill(rp, _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_HEAD));
	theme_rect(rp, _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_BORDER));
	for (c = 0; c < nc; c++) {
		cell(d->header, c, t, sizeof(t));
		text_fit(rp, l + x[c], y + (rh - fh) / 2, x[c + 1] - x[c] - 6, t, tpen(T_DIM), 1);
	}
	y += rh;
	theme_line(rp, _mleft(obj) + 1, y - 1, _mright(obj) - 1, y - 1, tpen(T_BORDER));
	d->valid = 1;
	d->drawn_n = d->n;
	d->drawn_fit = fit;
	d->drawn_w = w;
	d->drawn_active = d->active;
	d->drawn_hot = d->hot;
	memcpy(d->drawn_x, x, sizeof(d->drawn_x));
	if (d->n == 0) {
		theme_text(rp, l, y + 2, "No TCP connections.", tpen(T_DIM), 0);
		return;
	}
	for (r = 0; r < d->n && r < fit; r++) {
		LONG ry = y + (LONG)r * rh;
		if (r == fit - 1 && d->n > fit) {
			rt_snprintf(t, sizeof(t), "%d more ...", d->n - r);
			theme_text(rp, l, ry + (rh - fh) / 2, t, tpen(T_DIM), 0);
			break;
		}
		tb_row(d, obj, rp, x, nc, r, ry);
	}
}

static int tb_hit(struct table_data *d, Object *obj, LONG y)
{
	LONG top = _mtop(obj) + tb_rowh();
	int r;

	if (y < top)
		return -1;
	r = (int)((y - top) / tb_rowh());
	return r < d->n ? r : -1;
}

DISPATCHER(table_dispatch)
{
	struct table_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->header = (const char *)mc_tagval(msg, MUIA_Table_Header, (ULONG)"");
		d->active = -1;
		d->hot = -1;
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_Table_NRows))
			d->n = (int)mc_tagval(msg, MUIA_Table_NRows, 0);
		if (mc_has_tag(msg, MUIA_Table_Rows)) {
			d->rows = (const char *const *)mc_tagval(msg, MUIA_Table_Rows, 0);
			if (!d->rows)
				d->n = 0;
			if (d->n > TB_ROWS)
				d->n = TB_ROWS;
			if (d->active >= d->n)
				d->active = d->n - 1;
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWUPDATE);
		}
		if (mc_has_tag(msg, MUIA_Table_Active)) {
			int a = (int)(LONG)mc_tagval(msg, MUIA_Table_Active, (ULONG)-1);
			if (a != d->active) {
				d->active = a;
				if (d->shown)
					MUI_Redraw(obj, MADF_DRAWUPDATE);
			}
		}
		break;
	case OM_GET:
		if (((struct opGet *)msg)->opg_AttrID == MUIA_Table_Active) {
			*((struct opGet *)msg)->opg_Storage = (ULONG)(LONG)((struct table_data *)INST_DATA(cl, obj))->active;
			return TRUE;
		}
		break;
	case MUIM_AskMinMax: {
		LONG h = tb_rowh() * 3 + 4;
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 160, 400, MUI_MAXMAX, h, tb_rowh() * 8, MUI_MAXMAX);
		return 0;
	}
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE, 1);
		return TRUE;
	case MUIM_Cleanup:
		d = INST_DATA(cl, obj);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		d = INST_DATA(cl, obj);
		d->shown = 1;
		d->valid = 0;
		break;
	case MUIM_Hide:
		((struct table_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw: {
		ULONG f;
		/* the Area class fills in the flags (MUI_Redraw() leaves them to it) */
		DoSuperMethodA(cl, obj, msg);
		f = ((struct MUIP_Draw *)msg)->flags;
		d = INST_DATA(cl, obj);
		if (f & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			tb_draw(d, obj, !(f & MADF_DRAWOBJECT));
		return 0;
	}
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		int i;
		d = INST_DATA(cl, obj);
		if (!im || !d->shown)
			return 0;
		i = mc_inside(obj, im) ? tb_hit(d, obj, im->MouseY) : -1;
		if (im->Class == IDCMP_MOUSEMOVE) {
			if (i != d->hot) {
				d->hot = i;
				MUI_Redraw(obj, MADF_DRAWUPDATE);
			}
			return 0;
		}
		if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN && mc_inside(obj, im)) {
			if (i >= 0) {
				struct TagItem t[2];
				t[0].ti_Tag = MUIA_Table_Active;
				t[0].ti_Data = (ULONG)i;
				t[1].ti_Tag = TAG_DONE;
				t[1].ti_Data = 0;
				SetAttrsA(obj, t);
			}
			return MUI_EventHandlerRC_Eat;
		}
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * ifview: every interface
 * ================================================================== */
#define IFV_LINES 4
struct ifview_data {
	const struct rp_status_model *m;
	int shown;
	/* what is on screen, for updates */
	int valid, drawn_nif;
	LONG drawn_w, drawn_h;
	char cache[RP_ST_NIF][IFV_LINES][120];
	struct gstate gs[RP_ST_NIF];
};

static LONG ifv_blockh(void)
{
	return (LONG)(theme_compact() ? 3 : 4) * (theme_fh() + 1) + (theme_compact() ? 3 : 7);
}

static void ifv_draw(struct ifview_data *d, Object *obj, int update)
{
	struct RastPort *rp = _rp(obj);
	LONG y = _mtop(obj), bh = ifv_blockh(), fh = theme_fh(), l = _mleft(obj), r = _mright(obj);
	LONG gw = (r - l) / 3, tw;
	char ln[IFV_LINES][120];
	int i, k, nl = theme_compact() ? 3 : 4;

	if (gw < 90)
		gw = 90;
	tw = r - l - gw - 10;
	if (!d->m || !d->m->running || !d->m->nif) {
		if (!update || d->valid) {
			if (update)
				theme_fill(rp, l, _mtop(obj), r, _mbottom(obj), tpen(T_WINDOW));
			theme_text(rp, l, y + 2, d->m && d->m->running ? "No interfaces." : "Riptide is not running.",
			           tpen(T_DIM), 0);
		}
		d->valid = 0;
		return;
	}
	/* everything again when the interfaces or the size changed */
	if (update && (!d->valid || d->drawn_nif != d->m->nif || d->drawn_w != _mwidth(obj) ||
	               d->drawn_h != _mheight(obj))) {
		theme_fill(rp, l, _mtop(obj), r, _mbottom(obj), tpen(T_WINDOW));
		update = 0;
	}
	if (!update) {
		for (i = 0; i < RP_ST_NIF; i++) {
			d->gs[i].valid = 0;
			for (k = 0; k < IFV_LINES; k++)
				d->cache[i][k][0] = 0;
		}
	}
	for (i = 0; i < d->m->nif; i++) {
		const struct rp_st_if *f = &d->m->ifs[i];
		LONG yy = y + 2;
		if (y + bh - 1 > _mbottom(obj)) {
			if (!update) {
				rt_snprintf(ln[0], sizeof(ln[0]), "%d more interface%s ...", d->m->nif - i,
				            d->m->nif - i == 1 ? "" : "s");
				theme_text(rp, l, y > _mbottom(obj) - fh ? _mbottom(obj) - fh : y, ln[0], tpen(T_DIM), 0);
			}
			break;
		}
		rp_st_if_lines(f, ln[0], ln[1], ln[2], sizeof(ln[0]));
		rp_st_if_rates(f, ln[3], sizeof(ln[3]));
		if (nl == 3)
			rt_strlcpy(ln[2], ln[3], sizeof(ln[2]));
		for (k = 0; k < nl; k++) {
			text_line(rp, l, yy, tw, ln[k], tpen(k == 0 || k == 1 ? T_TEXT : T_DIM), k == 0, d->cache[i][k], !update);
			yy += fh + 1;
		}
		if (update)
			graph_update(rp, r - gw + 1, y + 2, r, y + bh - 4, &f->hist, GK_BYTES, "", 0, &d->gs[i]);
		else
			graph_full(rp, r - gw + 1, y + 2, r, y + bh - 4, &f->hist, GK_BYTES, "", 0, &d->gs[i]);
		y += bh;
		if (!update && i < d->m->nif - 1)
			theme_line(rp, l, y - 2, r, y - 2, tpen(T_BORDER));
	}
	d->valid = 1;
	d->drawn_nif = d->m->nif;
	d->drawn_w = _mwidth(obj);
	d->drawn_h = _mheight(obj);
}

DISPATCHER(ifview_dispatch)
{
	struct ifview_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->m = (const struct rp_status_model *)mc_tagval(msg, MUIA_IfView_Model, 0);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_IfView_Update) && d->shown)
			MUI_Redraw(obj, MADF_DRAWUPDATE);
		break;
	case MUIM_AskMinMax:
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 160, 400, MUI_MAXMAX, ifv_blockh(), ifv_blockh() * 3, MUI_MAXMAX);
		return 0;
	case MUIM_Setup:
		if (!DoSuperMethodA(cl, obj, msg))
			return FALSE;
		theme_setup(obj);
		return TRUE;
	case MUIM_Cleanup:
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		d = INST_DATA(cl, obj);
		d->shown = 1;
		d->valid = 0;
		break;
	case MUIM_Hide:
		((struct ifview_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw: {
		ULONG f;
		/* the Area class fills in the flags (MUI_Redraw() leaves them to it) */
		DoSuperMethodA(cl, obj, msg);
		f = ((struct MUIP_Draw *)msg)->flags;
		if (f & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			ifv_draw(INST_DATA(cl, obj), obj, !(f & MADF_DRAWOBJECT));
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * stext: lines of text that change
 * ================================================================== */
#define ST_LINES 10
struct stext_data {
	const char *text;
	int lines, shown;
	char cache[ST_LINES][120];
};

static void st_draw(struct stext_data *d, Object *obj, int update)
{
	struct RastPort *rp = _rp(obj);
	const char *s = d->text;
	LONG y = _mtop(obj);
	char line[160];
	int n;

	for (n = 0; n < d->lines && n < ST_LINES; n++) {
		int k = 0;
		while (*s && *s != '\n' && k < (int)sizeof(line) - 1)
			line[k++] = *s++;
		line[k] = 0;
		if (*s == '\n')
			s++;
		if (!update)
			d->cache[n][0] = 0;
		text_line(rp, _mleft(obj), y, _mwidth(obj), line, tpen(T_TEXT), 0, d->cache[n], !update);
		y += theme_fh() + 1;
	}
}

DISPATCHER(stext_dispatch)
{
	struct stext_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->text = (const char *)mc_tagval(msg, MUIA_SText_Text, (ULONG)"");
		d->lines = (int)mc_tagval(msg, MUIA_SText_Lines, 1);
		return (ULONG)obj;
	case OM_SET:
		d = INST_DATA(cl, obj);
		if (mc_has_tag(msg, MUIA_SText_Text)) {
			d->text = (const char *)mc_tagval(msg, MUIA_SText_Text, (ULONG)"");
			if (d->shown)
				MUI_Redraw(obj, MADF_DRAWUPDATE);
		}
		break;
	case MUIM_AskMinMax: {
		LONG h;
		d = INST_DATA(cl, obj);
		h = (LONG)d->lines * (theme_fh() + 1);
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 100, 300, MUI_MAXMAX, h, h, h);
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
		((struct stext_data *)INST_DATA(cl, obj))->shown = 1;
		break;
	case MUIM_Hide:
		((struct stext_data *)INST_DATA(cl, obj))->shown = 0;
		break;
	case MUIM_Draw: {
		ULONG f;
		/* the Area class fills in the flags (MUI_Redraw() leaves them to it) */
		DoSuperMethodA(cl, obj, msg);
		f = ((struct MUIP_Draw *)msg)->flags;
		if (f & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			st_draw(INST_DATA(cl, obj), obj, !(f & MADF_DRAWOBJECT));
		return 0;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================================================================
 * the page
 * ================================================================== */
static struct rp_status_model model;
static Object *o_sum, *o_total, *o_cpu, *o_tabs, *o_tpages, *o_ifv, *o_table, *o_cgraph, *o_names, *o_counters;
static char t_sum[200], t_names[1200], t_counters[1200];
static char rowbuf[RP_ST_NCONN][200];
static const char *rows[RP_ST_NCONN];
static char cg_title[64];
static int cur_conn;
static struct { u16 lport, fport; u8 faddr[16]; int set; } cur_key;   /* the chosen connection */
static const struct rp_hist empty_hist;

static const char *const tab_labels[5] = { "Interfaces", "Connections", "Names and time", "Counters", NULL };

static struct MUI_CustomClass *mk(int size, APTR disp)
{
	return MUI_CreateCustomClass(NULL, (CONST_STRPTR)MUIC_Area, NULL, size, disp);
}

int status_classes_make(void)
{
	graph_mcc = mk(sizeof(struct graph_data), DISPATCHER_REF(graph_dispatch));
	tabs_mcc = mk(sizeof(struct tabs_data), DISPATCHER_REF(tabs_dispatch));
	table_mcc = mk(sizeof(struct table_data), DISPATCHER_REF(table_dispatch));
	ifview_mcc = mk(sizeof(struct ifview_data), DISPATCHER_REF(ifview_dispatch));
	stext_mcc = mk(sizeof(struct stext_data), DISPATCHER_REF(stext_dispatch));
	return graph_mcc && tabs_mcc && table_mcc && ifview_mcc && stext_mcc ? 0 : -1;
}

void status_classes_free(void)
{
	struct MUI_CustomClass **c[] = { &graph_mcc, &tabs_mcc, &table_mcc, &ifview_mcc, &stext_mcc };
	int i;

	for (i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++)
		if (*c[i]) {
			MUI_DeleteCustomClass(*c[i]);
			*c[i] = NULL;
		}
}

/* the connection graph follows the chosen row (the first when none) */
static void show_conn_graph(void)
{
	const struct rp_st_conn *c;
	char a[52];

	/* the chosen one moves in the list as the list is sorted again */
	if (cur_key.set) {
		int i;
		cur_conn = -1;
		for (i = 0; i < model.nconn; i++)
			if (model.conns[i].lport == cur_key.lport && model.conns[i].fport == cur_key.fport &&
			    !memcmp(model.conns[i].faddr, cur_key.faddr, 16))
				cur_conn = i;
		if (cur_conn < 0)
			cur_key.set = 0;
		mui_nnset(o_table, MUIA_Table_Active, (ULONG)(LONG)cur_conn);
	}
	if (cur_conn < 0 || cur_conn >= model.nconn)
		cur_conn = model.nconn ? 0 : -1;
	if (cur_conn < 0) {
		rt_strlcpy(cg_title, "No connection", sizeof(cg_title));
		mui_set(o_cgraph, MUIA_Graph_Hist, (ULONG)&empty_hist);
		return;
	}
	c = &model.conns[cur_conn];
	rp_fmt_addr_port(a, sizeof(a), c->faddr, c->fport);
	rt_snprintf(cg_title, sizeof(cg_title), "%s", c->state == 1 ? "Listening" : a);
	mui_set(o_cgraph, MUIA_Graph_Hist, (ULONG)&c->hist);
}

/* milliseconds of dos.library's clock (1/50 s steps), for the DEBUG log */
static u32 log_ms(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return (u32)ds.ds_Minute * 60000u + (u32)ds.ds_Tick * 20u;
}

/* once a second while the page is shown */
static void tick(void)
{
	char mem[160];
	int i;
	u32 t0 = log_ms(), t1;

	rp_status_read(&model);
	t1 = log_ms();
	rp_st_summary(&model, t_sum, sizeof(t_sum));
	if (model.running) {
		rp_st_memory(&model, mem, sizeof(mem));
		rt_strlcpy(t_sum + strlen(t_sum), "\n", sizeof(t_sum) - strlen(t_sum));
		rt_strlcpy(t_sum + strlen(t_sum), mem, sizeof(t_sum) - strlen(t_sum));
	} else {
		rt_strlcpy(t_sum + strlen(t_sum), "\nThe page fills in by itself when it starts.",
		           sizeof(t_sum) - strlen(t_sum));
	}
	{
		static u32 acc[4], nacc;
		u32 a = log_ms(), b, c, e;
		mui_set(o_sum, MUIA_SText_Text, (ULONG)t_sum);
		b = log_ms();
		mui_set(o_total, MUIA_Graph_Hist, (ULONG)&model.total);
		mui_set(o_cpu, MUIA_Graph_Hist, (ULONG)&model.cpu_hist);
		c = log_ms();
		mui_set(o_ifv, MUIA_IfView_Update, TRUE);
		e = log_ms();
		acc[0] += b - a;
		acc[1] += c - b;
		acc[2] += e - c;
		if (++nacc == 10) {
			rp_log("DBG 10 ticks: summary %lu ms, graphs %lu ms, interfaces %lu ms", (unsigned long)acc[0],
			       (unsigned long)acc[1], (unsigned long)acc[2]);
			acc[0] = acc[1] = acc[2] = nacc = 0;
		}
	}
	for (i = 0; i < model.nconn; i++) {
		rp_st_conn_row(&model.conns[i], rowbuf[i], sizeof(rowbuf[i]));
		rows[i] = rowbuf[i];
	}
	mui_set(o_table, MUIA_Table_NRows, (ULONG)model.nconn);
	mui_set(o_table, MUIA_Table_Rows, (ULONG)rows);
	show_conn_graph();
	if (model.running) {
		rp_st_names_text(&model, t_names, sizeof(t_names));
		rp_st_counters_text(&model, t_counters, sizeof(t_counters));
	} else {
		t_names[0] = t_counters[0] = 0;
	}
	mui_set(o_names, MUIA_SText_Text, (ULONG)t_names);
	mui_set(o_counters, MUIA_SText_Text, (ULONG)t_counters);
	rp_log("status: %s, %d interfaces, %d connections, in %lu out %lu B/s, cpu %lu; read %lu ms, shown %lu ms",
	       model.running ? "running" : "stopped", model.nif, model.nconn, (unsigned long)model.rx_bps,
	       (unsigned long)model.tx_bps, (unsigned long)model.cpu_pm, (unsigned long)(t1 - t0),
	       (unsigned long)(log_ms() - t1));
}

void status_table_chosen(void)
{
	cur_conn = (int)(LONG)mui_get(o_table, MUIA_Table_Active);
	cur_key.set = cur_conn >= 0 && cur_conn < model.nconn;
	if (cur_key.set) {
		cur_key.lport = model.conns[cur_conn].lport;
		cur_key.fport = model.conns[cur_conn].fport;
		memcpy(cur_key.faddr, model.conns[cur_conn].faddr, 16);
	}
	show_conn_graph();
}

/* o above a filler that takes the spare height */
static Object *vfill(Object *o)
{
	return mui_new(MUIC_Group, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)o, MUIA_Group_Child,
	               (ULONG)mui_new(MUIC_Rectangle, TAGEND), TAGEND);
}

Object *status_page_build(Object *app, ULONG chosen_id)
{
	Object *top, *conn, *g;
	int compact = theme_compact();

	rp_status_init(&model);
	cur_conn = -1;
	cur_key.set = 0;
	tick_fn = tick;
	rt_strlcpy(t_sum, "Reading Riptide's state ...\n", sizeof(t_sum));
	o_sum = mui_newc(stext_mcc, MUIA_SText_Text, (ULONG)t_sum, MUIA_SText_Lines, 2, TAGEND);
	o_total = mui_newc(graph_mcc, MUIA_Graph_Hist, (ULONG)&model.total, MUIA_Graph_Kind, GK_BYTES,
	                   MUIA_Graph_Title, (ULONG)"Traffic", MUIA_Graph_Lines, compact ? 3 : 6, MUIA_Graph_Ticker, 1,
	                   TAGEND);
	o_cpu = mui_newc(graph_mcc, MUIA_Graph_Hist, (ULONG)&model.cpu_hist, MUIA_Graph_Kind, GK_CPU, MUIA_Graph_Title,
	                 (ULONG)"CPU", MUIA_Graph_Lines, compact ? 3 : 6, MUIA_HorizWeight, 35, TAGEND);
	o_tabs = mui_newc(tabs_mcc, MUIA_Tabs_Labels, (ULONG)tab_labels, MUIA_Tabs_Active, 0, TAGEND);
	o_ifv = mui_newc(ifview_mcc, MUIA_IfView_Model, (ULONG)&model, TAGEND);
	o_table = mui_newc(table_mcc, MUIA_Table_Header, (ULONG)rp_st_conn_header(), TAGEND);
	o_cgraph = mui_newc(graph_mcc, MUIA_Graph_Hist, (ULONG)&empty_hist, MUIA_Graph_Kind, GK_CONN, MUIA_Graph_Title,
	                    (ULONG)cg_title, MUIA_Graph_Lines, compact ? 3 : 5, TAGEND);
	o_names = mui_newc(stext_mcc, MUIA_SText_Text, (ULONG)"", MUIA_SText_Lines, compact ? 6 : 8, TAGEND);
	o_counters = mui_newc(stext_mcc, MUIA_SText_Text, (ULONG)"", MUIA_SText_Lines, compact ? 6 : 8, TAGEND);
	if (!o_sum || !o_total || !o_cpu || !o_tabs || !o_ifv || !o_table || !o_cgraph || !o_names || !o_counters)
		return NULL;
	rt_strlcpy(cg_title, "No connection", sizeof(cg_title));
	{
		static char h0[400], h1[400], h2[400], h3[400];
		theme_bubble_to(h0, sizeof(h0), "Traffic", "What all interfaces but loopback received (columns) and "
		                "sent (line) each second over the last minute.");
		mui_set(o_total, MUIA_ShortHelp, (ULONG)h0);
		theme_bubble_to(h1, sizeof(h1), "CPU", "How busy the Riptide process was each second (columns), "
		                "from 0 to 100 % of the processor.");
		theme_bubble_to(h2, sizeof(h2), "Connections", "Every TCP connection: its state, both ends, round trip "
		                "time, window (packets; ECN when it uses it), speed and the program it belongs to. "
		                "Click one to see its graph below.");
		theme_bubble_to(h3, sizeof(h3), "Connection graph", "The chosen connection over the last minute: "
		                "the window in packets (columns) and the round trip time (line), each on its own scale.");
		mui_set(o_cpu, MUIA_ShortHelp, (ULONG)h1);
		mui_set(o_table, MUIA_ShortHelp, (ULONG)h2);
		mui_set(o_cgraph, MUIA_ShortHelp, (ULONG)h3);
	}
	top = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, MUIA_Group_Child, (ULONG)o_total,
	              MUIA_Group_Child, (ULONG)o_cpu, TAGEND);
	conn = mui_new(MUIC_Group, MUIA_Group_Spacing, 4, MUIA_Group_Child, (ULONG)o_table, MUIA_Group_Child,
	               (ULONG)o_cgraph, TAGEND);
	/* every tab may grow, so the page takes the window's spare height */
	o_tpages = mui_new(MUIC_Group, MUIA_Group_PageMode, TRUE, MUIA_Group_Child, (ULONG)o_ifv, MUIA_Group_Child,
	                   (ULONG)conn, MUIA_Group_Child, (ULONG)vfill(o_names), MUIA_Group_Child,
	                   (ULONG)vfill(o_counters), TAGEND);
	if (!top || !conn || !o_tpages)
		return NULL;
	mui_do(o_tabs, MUIM_Notify, MUIA_Tabs_Active, MUIV_EveryTime, (ULONG)o_tpages, 3, MUIM_Set,
	       MUIA_Group_ActivePage, MUIV_TriggerValue);
	mui_do(o_table, MUIM_Notify, MUIA_Table_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID,
	       chosen_id);
	g = mui_new(MUIC_Group, MUIA_Group_VertSpacing, compact ? 2 : 6, MUIA_Group_Child, (ULONG)o_sum,
	            MUIA_Group_Child, (ULONG)top, MUIA_Group_Child, (ULONG)o_tabs, MUIA_Group_Child, (ULONG)o_tpages,
	            TAGEND);
	return g;
}

void status_page_tab(int t)
{
	if (o_tabs)
		mui_set(o_tabs, MUIA_Tabs_Active, (ULONG)t);
}

void status_page_free(void)
{
	tick_fn = NULL;
	rp_status_free();
	o_sum = o_total = o_cpu = o_tabs = o_tpages = o_ifv = o_table = o_cgraph = o_names = o_counters = NULL;
}
