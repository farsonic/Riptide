/*
 * Riptide - RiptidePrefs (MUI): the Counters page (Advanced). Every count
 * Riptide keeps, read while the page is shown (status_amiga.c reads the
 * tables, prefs_counters.c makes the rows), at the Status page's Update
 * period, with Refresh, Zero (counts from now on) and Since start.
 *
 * One class of its own, cview: the rows in three columns (name, value,
 * change per second), group headings in bold, and a flat scroll bar at the
 * right: a click above or below its knob moves a page, dragging the knob
 * moves the rows. Only the lines whose text changed are drawn again. It
 * owns the page's timer: it starts when the page is shown and stops when
 * it is hidden; while Riptide is not running it only looks every
 * STOPPED_MS for it to start and draws nothing until it does.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_theme.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_counters.h"
#include "prefs/mui_counters.h"

#define MUIM_CView_Tick    (RTAG + 0xc80)  /* the timer, or Refresh */
#define MUIM_CView_Zero    (RTAG + 0xc81)
#define MUIM_CView_Start   (RTAG + 0xc82)  /* Since start */
#define MUIM_CView_Update  (RTAG + 0xc83)  /* the Update cycle changed: (value) */

#ifndef SELECTDOWN
#define SELECTDOWN 0x68
#endif
#ifndef SELECTUP
#define SELECTUP (SELECTDOWN | 0x80)
#endif

#define STOPPED_MS 3000
#define CV_LINES 80             /* lines on screen at most */
#define CV_BAR 8                /* the scroll bar's width */

static const ULONG upd_ms[4] = { 1000, 2000, 5000, 0 };
static struct rp_cnt_model model;
static Object *o_view, *o_sum, *o_upd, *o_btn[3];
static char t_sum[120];
static char *helps[5];

struct cview_data {
	struct MUI_EventHandlerNode ehn;
	struct MUI_InputHandlerNode ihn;
	int shown, timer_on, upd, running;
	int top;                    /* the first row shown */
	int drag;                   /* the knob is being dragged: */
	LONG drag_y, drag_top;      /* where it was taken */
	/* what is on screen */
	int valid, drawn_top, drawn_n, drawn_vis;
	char cache[CV_LINES][96];
};

static struct MUI_CustomClass *cview_mcc;

/* ---- drawing ---- */
static LONG cv_rowh(void)
{
	return theme_fh() + (theme_compact() ? 1 : 3);
}

/* the lines that fit */
static int cv_vis(Object *obj)
{
	int v = (int)(_mheight(obj) / cv_rowh());
	return v < 1 ? 1 : v > CV_LINES ? CV_LINES : v;
}

/* the first row: within the rows */
static int cv_clamp(struct cview_data *d, Object *obj)
{
	int vis = cv_vis(obj), max = model.n - vis;

	if (max < 0)
		max = 0;
	if (d->top > max)
		d->top = max;
	if (d->top < 0)
		d->top = 0;
	return vis;
}

/* the knob of the scroll bar: its top and height in pixels */
static void cv_knob(struct cview_data *d, Object *obj, int vis, LONG *ky, LONG *kh)
{
	LONG h = _mheight(obj);

	if (model.n <= vis) {
		*ky = 0;
		*kh = h;
		return;
	}
	*kh = h * vis / model.n;
	if (*kh < 8)
		*kh = 8;
	*ky = (h - *kh) * d->top / (model.n - vis);
}

static void cv_bar(struct cview_data *d, Object *obj, int vis)
{
	struct RastPort *rp = _rp(obj);
	LONG x0 = _mright(obj) - CV_BAR + 1, ky, kh;

	theme_fill(rp, x0, _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_PANEL));
	if (model.n <= vis)
		return;
	cv_knob(d, obj, vis, &ky, &kh);
	theme_fill(rp, x0 + 1, _mtop(obj) + ky, _mright(obj) - 1, _mtop(obj) + ky + kh - 1,
	           tpen(d->drag ? T_ACCENT : T_BORDER));
}

/* line i (row r): heading or name, value right-aligned, change per second */
static void cv_line(struct cview_data *d, Object *obj, int i, int r, const char *key)
{
	struct RastPort *rp = _rp(obj);
	LONG y = _mtop(obj) + (LONG)i * cv_rowh(), x = _mleft(obj), right = _mright(obj) - CV_BAR - 4;
	LONG wrate = theme_textlen(rp, "+999 999/s") + 8, xv = right - wrate;
	char name[RP_CNT_NAME], val[24], rate[24];
	int head;

	theme_fill(rp, x, y, _mright(obj) - CV_BAR, y + cv_rowh() - 1, tpen(T_HEAD));
	rt_strlcpy(d->cache[i], key, sizeof(d->cache[i]));
	if (r < 0)
		return;
	rp_cnt_text(&model, r, name, sizeof(name), val, sizeof(val), rate, sizeof(rate));
	head = model.rows[r].kind == RP_CNT_HEAD;
	y += theme_compact() ? 0 : 1;
	if (head) {
		theme_text(rp, x + 2, y, name, tpen(T_TEXT), 1);
		return;
	}
	theme_text(rp, x + 14, y, name, tpen(T_TEXT), 0);
	theme_text(rp, xv - theme_textlen(rp, val), y, val, tpen(model.rows[r].kind == RP_CNT_LEVEL ? T_DIM : T_TEXT),
	           0);
	if (rate[0])
		theme_text(rp, right - theme_textlen(rp, rate), y, rate, tpen(T_ACCENT), 0);
}

static void cv_draw(struct cview_data *d, Object *obj, int update)
{
	struct RastPort *rp = _rp(obj);
	char key[96], name[RP_CNT_NAME], val[24], rate[24];
	int vis = cv_clamp(d, obj), i, full;

	if (!model.running) {
		theme_fill(rp, _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_HEAD));
		theme_text(rp, _mleft(obj) + 4, _mtop(obj) + 2,
		           tr("Riptide is not running. The counts appear when it starts."), tpen(T_DIM), 0);
		d->valid = 0;
		return;
	}
	full = !update || !d->valid || d->drawn_top != d->top || d->drawn_n != model.n || d->drawn_vis != vis;
	if (full)
		theme_fill(rp, _mleft(obj), _mtop(obj), _mright(obj), _mbottom(obj), tpen(T_HEAD));
	for (i = 0; i < vis; i++) {
		int r = d->top + i;
		if (r < model.n) {
			rp_cnt_text(&model, r, name, sizeof(name), val, sizeof(val), rate, sizeof(rate));
			rt_snprintf(key, sizeof(key), "%d|%s|%s|%s", r, name, val, rate);
		} else {
			key[0] = 0;
		}
		if (full || strcmp(key, d->cache[i]))
			cv_line(d, obj, i, r < model.n ? r : -1, key);
	}
	if (full)
		cv_bar(d, obj, vis);
	d->valid = 1;
	d->drawn_top = d->top;
	d->drawn_n = model.n;
	d->drawn_vis = vis;
}

/* ---- the timer ---- */
static ULONG cv_period(struct cview_data *d)
{
	ULONG ms = upd_ms[d->upd & 3];

	if (ms && !d->running && ms < STOPPED_MS)
		ms = STOPPED_MS;
	return ms;
}

static void cv_timer(Object *obj, struct cview_data *d, int on)
{
	if (on == d->timer_on || (on && !cv_period(d)))
		return;
	if (on) {
		memset(&d->ihn, 0, sizeof(d->ihn));
		d->ihn.ihn_Object = obj;
		d->ihn.ihn_millis = cv_period(d);
		d->ihn.ihn_Flags = MUIIHNF_TIMER;
		d->ihn.ihn_Method = MUIM_CView_Tick;
		mui_do(_app(obj), MUIM_Application_AddInputHandler, (ULONG)&d->ihn);
	} else {
		mui_do(_app(obj), MUIM_Application_RemInputHandler, (ULONG)&d->ihn);
	}
	d->timer_on = on;
}

/* the line above the rows */
static void cv_summary(void)
{
	char t[sizeof(t_sum)];

	rp_cnt_summary(&model, t, sizeof(t));
	if (strcmp(t, t_sum)) {
		rt_strlcpy(t_sum, t, sizeof(t_sum));
		if (o_sum)
			mui_set(o_sum, MUIA_QLabel_Text, (ULONG)t_sum);
	}
}

/* a read: rows, the summary, the lines that changed */
static void cv_tick(Object *obj, struct cview_data *d)
{
	int was = d->running;
	ULONG period = cv_period(d);

	rp_counters_read(&model);
	d->running = model.running;
	if (cv_period(d) != period && d->timer_on) {
		cv_timer(obj, d, 0);
		cv_timer(obj, d, 1);
	}
	if (!d->running && !was)
		return;             /* still not running: nothing to draw */
	cv_summary();
	if (d->shown)
		MUI_Redraw(obj, MADF_DRAWUPDATE);
}

/* ---- the class ---- */
DISPATCHER(cview_dispatch)
{
	struct cview_data *d;

	switch (msg->MethodID) {
	case OM_NEW:
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		memset(d, 0, sizeof(*d));
		d->upd = rp_eprefs_get("status_update", 0) & 3;
		return (ULONG)obj;
	case MUIM_AskMinMax: {
		LONG h = cv_rowh();
		DoSuperMethodA(cl, obj, msg);
		mc_minmax(msg, 200, 400, MUI_MAXMAX, 3 * h, 12 * h, MUI_MAXMAX);
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
		cv_timer(obj, d, 0);
		mc_handler(obj, cl, &d->ehn, 0, 0);
		theme_cleanup(obj);
		break;
	case MUIM_Show:
		d = INST_DATA(cl, obj);
		d->shown = 1;
		d->valid = 0;
		/* the Update setting may have changed on the Status page */
		d->upd = rp_eprefs_get("status_update", 0) & 3;
		if (o_upd)
			mui_nnset(o_upd, MUIA_Cycle_Active, (ULONG)d->upd);
		cv_timer(obj, d, 1);
		/* the first read at once, after this layout */
		mui_do(_app(obj), MUIM_Application_PushMethod, (ULONG)obj, 1, MUIM_CView_Tick);
		break;
	case MUIM_Hide:
		d = INST_DATA(cl, obj);
		d->shown = 0;
		cv_timer(obj, d, 0);
		break;
	case MUIM_CView_Tick:
		d = INST_DATA(cl, obj);
		if (d->shown)
			cv_tick(obj, d);
		return 0;
	case MUIM_CView_Zero:
	case MUIM_CView_Start:
		d = INST_DATA(cl, obj);
		if (msg->MethodID == MUIM_CView_Zero)
			rp_cnt_zero(&model);
		else
			rp_cnt_since_start(&model);
		cv_summary();
		if (d->shown)
			MUI_Redraw(obj, MADF_DRAWUPDATE);
		return 0;
	case MUIM_CView_Update:
		d = INST_DATA(cl, obj);
		d->upd = (int)((ULONG *)msg)[1] & 3;
		rp_eprefs_set("status_update", d->upd);
		rp_eprefs_save();
		if (d->shown) {
			cv_timer(obj, d, 0);
			cv_timer(obj, d, 1);
		}
		return 0;
	case MUIM_Draw: {
		ULONG f;
		/* the Area class fills in the flags (MUI_Redraw() leaves them to it) */
		DoSuperMethodA(cl, obj, msg);
		f = ((struct MUIP_Draw *)msg)->flags;
		if (f & (MADF_DRAWOBJECT | MADF_DRAWUPDATE))
			cv_draw(INST_DATA(cl, obj), obj, !(f & MADF_DRAWOBJECT));
		return 0;
	}
	case MUIM_HandleEvent: {
		struct IntuiMessage *im = ((struct MUIP_HandleEvent *)msg)->imsg;
		int vis;
		LONG ky, kh, y;
		d = INST_DATA(cl, obj);
		if (!im || !d->shown || !model.running)
			return 0;
		vis = cv_clamp(d, obj);
		y = im->MouseY - _mtop(obj);
		if (im->Class == IDCMP_MOUSEMOVE && d->drag) {
			LONG h = _mheight(obj);
			cv_knob(d, obj, vis, &ky, &kh);
			if (h > kh)
				d->top = (int)(d->drag_top + (y - d->drag_y) * (model.n - vis) / (h - kh));
			cv_clamp(d, obj);
			if (d->top != d->drawn_top)
				MUI_Redraw(obj, MADF_DRAWUPDATE);
			return MUI_EventHandlerRC_Eat;
		}
		if (im->Class != IDCMP_MOUSEBUTTONS)
			return 0;
		if (im->Code == SELECTUP && d->drag) {
			d->drag = 0;
			d->valid = 0;
			MUI_Redraw(obj, MADF_DRAWUPDATE);
			return MUI_EventHandlerRC_Eat;
		}
		if (im->Code != SELECTDOWN || !mc_inside(obj, im) || im->MouseX < _mright(obj) - CV_BAR - 2 ||
		    model.n <= vis)
			return 0;
		/* in the scroll bar: above or below the knob a page, on it a drag */
		cv_knob(d, obj, vis, &ky, &kh);
		if (y < ky)
			d->top -= vis - 1;
		else if (y >= ky + kh)
			d->top += vis - 1;
		else {
			d->drag = 1;
			d->drag_y = y;
			d->drag_top = d->top;
			d->valid = 0;
		}
		cv_clamp(d, obj);
		MUI_Redraw(obj, MADF_DRAWUPDATE);
		return MUI_EventHandlerRC_Eat;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

int counters_classes_make(void)
{
	cview_mcc = MUI_CreateCustomClass(NULL, (CONST_STRPTR)MUIC_Area, NULL, sizeof(struct cview_data),
	                                  DISPATCHER_REF(cview_dispatch));
	return cview_mcc ? 0 : -1;
}

void counters_classes_free(void)
{
	if (cview_mcc)
		MUI_DeleteCustomClass(cview_mcc);
	cview_mcc = NULL;
}

/* a flat button that sends m to the view */
static Object *cbutton(const char *label, ULONG m, int k, const char *help)
{
	const char *u = strchr(label, '_');
	char name[24];
	int n = 0;
	Object *b = mui_newc(qbutton_mcc, MUIA_QButton_Label, (ULONG)label, MUIA_InputMode, MUIV_InputMode_RelVerify,
	                     MUIA_ControlChar, (ULONG)(u && u[1] ? (u[1] | 0x20) : 0), MUIA_CycleChain, 1, TAGEND);

	if (!b)
		return NULL;
	for (; *label && n < (int)sizeof(name) - 1; label++)
		if (*label != '_')
			name[n++] = *label;
	name[n] = 0;
	helps[k] = rp_alloc(400);
	if (helps[k]) {
		theme_bubble_to(helps[k], 400, name, help);
		mui_set(b, MUIA_ShortHelp, (ULONG)helps[k]);
	}
	mui_do(b, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)o_view, 1, m);
	o_btn[k] = b;
	return b;
}

Object *counters_page_build(void)
{
	static const char *upd_labels[5];
	Object *bar, *ul;

	rp_cnt_init(&model);
	rt_strlcpy(t_sum, tr("Counts since Riptide started."), sizeof(t_sum));
	o_view = mui_newc(cview_mcc, TAGEND);
	o_sum = mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)t_sum, MUIA_QLabel_Kind, QL_TEXT, TAGEND);
	upd_labels[0] = tr("Every second");
	upd_labels[1] = tr("Every 2 s");
	upd_labels[2] = tr("Every 5 s");
	upd_labels[3] = tr("Paused");
	upd_labels[4] = NULL;
	o_upd = mui_make(MUIO_Cycle, (ULONG)tr("Up_date"), (ULONG)upd_labels);
	ul = mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)"Up_date", MUIA_QLabel_Kind, QL_LABEL, TAGEND);
	bar = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 6, MUIA_VertWeight, 0, TAGEND);
	if (!o_view || !o_sum || !o_upd || !ul || !bar)
		return NULL;
	if (!cbutton("Re_fresh", MUIM_CView_Tick, 0, "Reads the counts again now.") ||
	    !cbutton("_Zero", MUIM_CView_Zero, 1,
	             "Shows the counts from now on, so you can see what one transfer or test does. Riptide's own "
	             "counts are not changed.") ||
	    !cbutton("Since s_tart", MUIM_CView_Start, 2, "Shows the counts since Riptide started again."))
		return NULL;
	mui_do(bar, OM_ADDMEMBER, (ULONG)o_btn[0]);
	mui_do(bar, OM_ADDMEMBER, (ULONG)o_btn[1]);
	mui_do(bar, OM_ADDMEMBER, (ULONG)o_btn[2]);
	mui_do(bar, OM_ADDMEMBER, (ULONG)mui_new(MUIC_Rectangle, TAGEND));
	mui_do(bar, OM_ADDMEMBER, (ULONG)ul);
	mui_do(bar, OM_ADDMEMBER, (ULONG)o_upd);
	mui_set(o_upd, MUIA_HorizWeight, 0);
	mui_set(ul, MUIA_HorizWeight, 0);
	mui_set(o_upd, MUIA_CycleChain, 1);
	mui_set(o_upd, MUIA_Cycle_Active, (ULONG)(rp_eprefs_get("status_update", 0) & 3));
	helps[3] = rp_alloc(400);
	if (helps[3]) {
		theme_bubble_to(helps[3], 400, "Update",
		                "How often the page reads the counts again. It is the Status page's setting too.");
		mui_set(o_upd, MUIA_ShortHelp, (ULONG)helps[3]);
		mui_set(ul, MUIA_ShortHelp, (ULONG)helps[3]);
	}
	helps[4] = rp_alloc(400);
	if (helps[4]) {
		theme_bubble_to(helps[4], 400, "Counts",
		                "Each count with how much it grew in the last second. Click the bar at the right "
		                "to see more, or drag its knob.");
		mui_set(o_view, MUIA_ShortHelp, (ULONG)helps[4]);
	}
	mui_do(o_upd, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (ULONG)o_view, 2, MUIM_CView_Update,
	       MUIV_TriggerValue);
	return mui_new(MUIC_Group, MUIA_Group_VertSpacing, theme_compact() ? 2 : 6, MUIA_Group_Child, (ULONG)o_sum,
	               MUIA_Group_Child, (ULONG)o_view, MUIA_Group_Child, (ULONG)bar, TAGEND);
}

void counters_tips(int on)
{
	int k;

	for (k = 0; k < 3; k++)
		if (o_btn[k])
			mui_set(o_btn[k], MUIA_ShortHelp, on ? (ULONG)helps[k] : 0);
	if (o_upd)
		mui_set(o_upd, MUIA_ShortHelp, on ? (ULONG)helps[3] : 0);
	if (o_view)
		mui_set(o_view, MUIA_ShortHelp, on ? (ULONG)helps[4] : 0);
}

void counters_page_free(void)
{
	int k;

	for (k = 0; k < 5; k++) {
		rp_free(helps[k]);
		helps[k] = NULL;
	}
	o_view = o_sum = o_upd = o_btn[0] = o_btn[1] = o_btn[2] = NULL;
	t_sum[0] = 0;
}
