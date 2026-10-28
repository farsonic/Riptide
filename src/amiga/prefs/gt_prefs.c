/*
 * Riptide - RiptidePrefs: the GadTools editor (AmigaOS 2.04+ and AROS;
 * used when MUI is not installed, or with GADTOOLS). One window: a page
 * cycle gadget, up to seven settings of the page (prefs_fields.c) and the
 * Save, Use, Apply and Cancel buttons; everything works from the keyboard
 * (the underlined letters, P for the next page).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_gui.h"

#if defined(__VBCC__)
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#else
#include "os/intuition.h"
#include "gen/intuition_calls.h"
#include "gen/gadtools_calls.h"
#include "gen/graphics_calls.h"
#endif

struct Library *GadToolsBase;

#define NFIELDS RP_NFIELDS_MAX
#define IS_LIST(p) (rp_page_list(p) >= 0)

/* gadget IDs: fields are 100 + index */
enum { G_PAGE = 1, G_SAVE, G_USE, G_APPLY, G_CANCEL, G_LIST, G_LINE, G_ADD, G_REPL, G_DEL, G_NOTE };
#define G_FIELD 100

/* ---- state ---- */
#define conf rp_cfg
static const char *page_names[RP_NPAGES + 1];
static struct Screen *scr;
static APTR vi;
static struct Window *win;
static struct Gadget *glist, *fgad[NFIELDS], *g_list, *g_line, *g_page;
static struct List olist;
static struct Node *onodes;
static int page, osel = -1;
static char linebuf[RP_LONG * 2];
static char numtext[NFIELDS][12];
static int stale;               /* gadgets rebuilt: queued gadget messages are old */

/* layout (font sensitive) */
static int fh, cw, lw, fw, rowh, ystep, x0, y0, body_y, btn_y, inner_w, inner_h;
#define NROWS 8

static const char *gstr(struct Gadget *g)
{
	/* GadTools string gadget: StringInfo, whose first field is Buffer */
	return *(const char **)g->SpecialInfo;
}

/* ---- messages ---- */
static long gt_request(const char *text, const char *buttons)
{
	struct EasyStruct es;
	const char *arg = text;

	es.es_StructSize = sizeof(es);
	es.es_Flags = 0;
	es.es_Title = (CONST_STRPTR)RP_TITLE;
	es.es_TextFormat = (CONST_STRPTR)"%s";
	es.es_GadgetFormat = (CONST_STRPTR)buttons;
	return EasyRequestArgs(win, &es, NULL, (APTR)&arg);
}

/* ---- validation ---- */
static int show_page(int p);

static int bad(int fi, const char *why)
{
	char msg[300], label[40];

	rp_flabel(fi, label, sizeof(label));
	rt_snprintf(msg, sizeof(msg), "%s: %s", label, why);
	if (win && rp_fields[fi].page != page)
		show_page(rp_fields[fi].page);
	rp_message(msg);
	if (win && fgad[fi] && (rp_fields[fi].type == F_STR || rp_fields[fi].type == F_NUM))
		ActivateGadget(fgad[fi], win, NULL);
	return -1;
}

static int validate(void)
{
	char err[200];
	int fi;

	if (rp_validate(&conf.cur, &fi, err, sizeof(err)) < 0)
		return bad(fi, err);
	return 0;
}

/* ---- gadgets ---- */
static void free_nodes(void)
{
	if (onodes)
		FreeVec(onodes);
	onodes = NULL;
}

static void build_list(void)
{
	int i, n = rp_list_n(page);

	olist.lh_Head = (struct Node *)&olist.lh_Tail;
	olist.lh_Tail = NULL;
	olist.lh_TailPred = (struct Node *)&olist.lh_Head;
	free_nodes();
	if (n <= 0 || !(onodes = AllocVec(sizeof(struct Node) * (ULONG)n, MEMF_ANY | MEMF_CLEAR)))
		return;
	for (i = 0; i < n; i++) {
		onodes[i].ln_Name = (char *)rp_list_at(page, i);
		AddTail(&olist, &onodes[i]);
	}
}

/* pixel width of a label without its '_' */
static int textw(const char *s)
{
	char t[64];
	int i = 0;

	for (; *s && i < (int)sizeof(t) - 1; s++)
		if (*s != '_')
			t[i++] = *s;
	t[i] = 0;
	return TextLength(&scr->RastPort, (CONST_STRPTR)t, (ULONG)i);
}

static void layout(void)
{
	int i, w;
	static const char *const btns[] = { "_Save", "_Use", "_Apply", "_Cancel" };

	fh = scr->Font->ta_YSize;
	cw = (TextLength(&scr->RastPort, (CONST_STRPTR)"abcdefghijklmnopqrstuvwxyz", 26) + 25) / 26;
	rowh = fh + 6;
	ystep = rowh + 3;
	lw = textw("_Page");
	for (i = 0; i < rp_nfields; i++)
		if ((w = textw(rp_fields[i].label)) > lw)
			lw = w;
	lw += 2 * cw;
	fw = 30 * cw;
	x0 = scr->WBorLeft + 8;
	y0 = scr->WBorTop + fh + 1 + 6;
	body_y = y0 + rowh + 8;
	btn_y = body_y + NROWS * ystep + 4;
	inner_w = 8 + lw + fw + 8;
	w = 0;
	for (i = 0; i < 4; i++)
		if (textw(btns[i]) > w)
			w = textw(btns[i]);
	w += 4 * cw;
	if (inner_w < 4 * w + 5 * 8)
		inner_w = 4 * w + 5 * 8;
	inner_h = btn_y + rowh + 6 - (scr->WBorTop + fh + 1);
}

static struct Gadget *mk(ULONG kind, struct Gadget *prev, int id, const char *label, ULONG flags,
                         int x, int y, int w, int h, struct TagItem *tags)
{
	struct NewGadget ng;

	memset(&ng, 0, sizeof(ng));
	ng.ng_LeftEdge = (WORD)x;
	ng.ng_TopEdge = (WORD)y;
	ng.ng_Width = (WORD)w;
	ng.ng_Height = (WORD)h;
	ng.ng_GadgetText = (CONST_STRPTR)label;
	ng.ng_TextAttr = scr->Font;
	ng.ng_GadgetID = (UWORD)id;
	ng.ng_Flags = flags;
	ng.ng_VisualInfo = vi;
	return prev ? CreateGadgetA(kind, prev, &ng, tags) : NULL;
}

static struct Gadget *mk_field(struct Gadget *g, int i, int y)
{
	const struct rp_field *f = &rp_fields[i];
	struct TagItem t[5];
	int n = 0, x = (f->flags & FL_RIGHT) ? x0 + lw + fw - 26 : x0 + lw, h = rowh;
	ULONG kind = STRING_KIND;

	t[n].ti_Tag = GT_Underscore;
	t[n++].ti_Data = '_';
	t[n].ti_Tag = GA_Disabled;
	t[n++].ti_Data = rp_fdisabled(&conf.cur, i);
	switch (f->type) {
	case F_STR:
		t[n].ti_Tag = GTST_String;
		t[n++].ti_Data = (ULONG)rp_fstr(&conf.cur, i);
		t[n].ti_Tag = GTST_MaxChars;
		t[n++].ti_Data = f->size - 1;
		break;
	case F_NUM:
		if (rp_fget(&conf.cur, i) == RP_UNSET)
			numtext[i][0] = 0;
		else
			rt_snprintf(numtext[i], sizeof(numtext[i]), "%ld", rp_fget(&conf.cur, i));
		t[n].ti_Tag = GTST_String;
		t[n++].ti_Data = (ULONG)numtext[i];
		t[n].ti_Tag = GTST_MaxChars;
		t[n++].ti_Data = 9;
		break;
	case F_CHECK:
		kind = CHECKBOX_KIND;
		t[n].ti_Tag = GTCB_Checked;
		t[n++].ti_Data = rp_fget(&conf.cur, i) != 0;
		h = 11;
		y += (rowh - 11) / 2;
		break;
	case F_CYCLE:
		kind = CYCLE_KIND;
		t[n].ti_Tag = GTCY_Labels;
		t[n++].ti_Data = (ULONG)f->cy;
		t[n].ti_Tag = GTCY_Active;
		t[n++].ti_Data = (ULONG)rp_fcycle(&conf.cur, i);
		break;
	}
	t[n].ti_Tag = TAG_DONE;
	t[n].ti_Data = 0;
	g = mk(kind, g, G_FIELD + i, f->label, PLACETEXT_LEFT, x,
	       y, kind == CHECKBOX_KIND ? 26 : kind == CYCLE_KIND ? fw / 2 : fw, h, t);
	fgad[i] = g;
	return g;
}

static void clear_body(void)
{
	SetAPen(win->RPort, 0);
	RectFill(win->RPort, win->BorderLeft, win->BorderTop, win->Width - win->BorderRight - 1,
	         win->Height - win->BorderBottom - 1);
}

static int build(void)
{
	struct TagItem t[6];
	struct Gadget *g;
	int i, y = body_y, bw, gap;
	static const char *const btn[4] = { "_Save", "_Use", "_Apply", "_Cancel" };

	if (win && glist)
		RemoveGList(win, glist, -1);
	FreeGadgets(glist);
	glist = NULL;
	g_list = g_line = NULL;
	memset(fgad, 0, sizeof(fgad));
	free_nodes();

	g = CreateContext(&glist);
	t[0].ti_Tag = GTCY_Labels;
	t[0].ti_Data = (ULONG)page_names;
	t[1].ti_Tag = GTCY_Active;
	t[1].ti_Data = (ULONG)page;
	t[2].ti_Tag = GT_Underscore;
	t[2].ti_Data = '_';
	t[3].ti_Tag = TAG_DONE;
	g = g_page = mk(CYCLE_KIND, g, G_PAGE, "_Page", PLACETEXT_LEFT, x0 + lw, y0, fw, rowh, t);

	if (IS_LIST(page)) {
		int lh = 5 * ystep - 8;
		build_list();
		t[0].ti_Tag = GTLV_Labels;
		t[0].ti_Data = (ULONG)&olist;
		t[1].ti_Tag = GTLV_ShowSelected;
		t[1].ti_Data = 0;
		t[2].ti_Tag = GTLV_Selected;
		t[2].ti_Data = osel >= 0 ? (ULONG)osel : ~0UL;
		t[3].ti_Tag = GTLV_Top;
		t[3].ti_Data = osel >= 0 ? (ULONG)osel : 0;
		t[4].ti_Tag = TAG_DONE;
		g = g_list = mk(LISTVIEW_KIND, g, G_LIST, NULL, 0, x0, y, lw + fw, lh, t);
		y += lh + 4;
		t[0].ti_Tag = GTST_String;
		t[0].ti_Data = (ULONG)linebuf;
		t[1].ti_Tag = GTST_MaxChars;
		t[1].ti_Data = sizeof(linebuf) - 1;
		t[2].ti_Tag = GT_Underscore;
		t[2].ti_Data = '_';
		t[3].ti_Tag = TAG_DONE;
		g = g_line = mk(STRING_KIND, g, G_LINE, "_Line", PLACETEXT_LEFT, x0 + textw("_Line") + 2 * cw, y,
		                lw + fw - textw("_Line") - 2 * cw, rowh, t);
		y += ystep;
		bw = (lw + fw - 16) / 3;
		t[0].ti_Tag = GT_Underscore;
		t[0].ti_Data = '_';
		t[1].ti_Tag = GA_Disabled;
		t[1].ti_Data = 0;
		t[2].ti_Tag = TAG_DONE;
		g = mk(BUTTON_KIND, g, G_ADD, "A_dd", PLACETEXT_IN, x0, y, bw, rowh, t);
		t[1].ti_Data = osel < 0;
		g = mk(BUTTON_KIND, g, G_REPL, "_Replace", PLACETEXT_IN, x0 + bw + 8, y, bw, rowh, t);
		g = mk(BUTTON_KIND, g, G_DEL, "D_elete", PLACETEXT_IN, x0 + 2 * (bw + 8), y, bw, rowh, t);
	} else {
		for (i = 0; i < rp_nfields; i++)
			if (rp_fields[i].page == page) {
				if (rp_fields[i].flags & FL_RIGHT)
					y -= ystep;
				g = mk_field(g, i, y);
				y += ystep;
			}
	}
	t[0].ti_Tag = GTTX_Text;
	t[0].ti_Data = (ULONG)rp_pages[page].note;
	t[1].ti_Tag = TAG_DONE;
	g = mk(TEXT_KIND, g, G_NOTE, NULL, 0, x0, body_y + (NROWS - 1) * ystep, lw + fw, rowh, t);

	gap = 8;
	bw = (inner_w - 5 * gap) / 4;
	t[0].ti_Tag = GT_Underscore;
	t[0].ti_Data = '_';
	t[1].ti_Tag = TAG_DONE;
	for (i = 0; i < 4; i++)
		g = mk(BUTTON_KIND, g, G_SAVE + i, btn[i], PLACETEXT_IN,
		       scr->WBorLeft + gap + i * (bw + gap), btn_y, bw, rowh, t);
	if (!g)
		return -1;
	if (win) {
		stale = 1;
		clear_body();
		AddGList(win, glist, (ULONG)~0, -1, NULL);
		RefreshGList(glist, win, NULL, -1);
		GT_RefreshWindow(win, NULL);
	}
	return 0;
}

/* Take the text of the string gadgets of the page into the model.
 * Returns -1 (message shown) on a bad number. */
static int harvest(void)
{
	char err[80];
	int i;

	if (IS_LIST(page)) {
		if (g_line)
			rt_strlcpy(linebuf, gstr(g_line), sizeof(linebuf));
		return 0;
	}
	for (i = 0; i < rp_nfields; i++) {
		if (!fgad[i] || (rp_fields[i].type != F_STR && rp_fields[i].type != F_NUM))
			continue;
		if (rp_fparse(&conf.cur, i, gstr(fgad[i]), err, sizeof(err)) < 0) {
			bad(i, err);
			return -1;
		}
	}
	return 0;
}

static int show_page(int p)
{
	if (p != page) {
		osel = -1;
		linebuf[0] = 0;
	}
	page = p;
	return build();
}

/* ---- the window ---- */
static void toggle(int i, int dir)
{
	const struct rp_field *f = &rp_fields[i];

	if (f->type == F_CHECK) {
		rp_fset(&conf.cur, i, !rp_fget(&conf.cur, i));
	} else if (f->type == F_CYCLE) {
		int a, n;
		for (n = 0; f->cy[n]; n++)
			;
		a = (rp_fcycle(&conf.cur, i) + n + dir) % n;
		rp_fset(&conf.cur, i, f->cyv[a]);
	}
}

/* 1: done (saved, used or cancelled) */
static int finish(int what)
{
	if (what == G_CANCEL)
		return 1;
	if (harvest() < 0 || validate() < 0)
		return 0;
	if (what == G_APPLY) {
		rp_apply(0);
		return 0;
	}
	if (rp_store(what == G_SAVE) < 0)
		return 0;
	rp_apply(1);
	return 1;
}

static int other_edit(int id)
{
	int r;

	harvest();
	if (id == G_ADD && linebuf[0])
		r = rp_list_edit(page, -1, linebuf);
	else if (id == G_REPL && osel >= 0 && linebuf[0])
		r = rp_list_edit(page, osel, linebuf);
	else if (id == G_DEL && osel >= 0)
		r = rp_list_edit(page, osel, NULL);
	else
		return 0;
	if (r == -2) {
		ActivateGadget(g_line, win, NULL);
		return 0;
	}
	osel = r;
	if (id == G_DEL)
		linebuf[0] = 0;
	return build();
}

static int event_loop(void)
{
	for (;;) {
		struct IntuiMessage *im;
		ULONG cls;
		UWORD code;
		struct Gadget *gad;

		Wait((1UL << win->UserPort->mp_SigBit) | rp_single_sig());
		if (rp_single_poll()) {
			/* another RiptidePrefs was started: this one comes to the front */
			ScreenToFront(win->WScreen);
			WindowToFront(win);
			ActivateWindow(win);
		}
		stale = 0;
		while ((im = GT_GetIMsg(win->UserPort)) != NULL) {
			int id, i;
			cls = im->Class;
			code = im->Code;
			gad = (struct Gadget *)im->IAddress;
			GT_ReplyIMsg(im);
			switch (cls) {
			case IDCMP_REFRESHWINDOW:
				GT_BeginRefresh(win);
				GT_EndRefresh(win, TRUE);
				break;
			case IDCMP_CLOSEWINDOW:
				return 0;
			case IDCMP_GADGETUP:
				/* IAddress of a message queued before a rebuild points
				 * to a freed gadget */
				if (stale)
					break;
				id = gad->GadgetID;
				if (id == G_PAGE) {
					if (harvest() < 0)
						build();
					else
						show_page(code);
				} else if (id >= G_SAVE && id <= G_CANCEL) {
					if (finish(id))
						return 0;
				} else if (id == G_LIST) {
					osel = code;
					rt_strlcpy(linebuf, rp_list_at(page, osel) ? rp_list_at(page, osel) : "",
					           sizeof(linebuf));
					build();
				} else if (id >= G_ADD && id <= G_DEL) {
					other_edit(id);
				} else if (id >= G_FIELD && id < G_FIELD + rp_nfields) {
					const struct rp_field *f = &rp_fields[id - G_FIELD];
					if (f->type == F_CHECK || f->type == F_CYCLE) {
						if (f->type == F_CHECK)
							rp_fset(&conf.cur, id - G_FIELD, (gad->Flags & GFLG_SELECTED) != 0);
						else
							rp_fset(&conf.cur, id - G_FIELD, f->cyv[code]);
						if (harvest() == 0)
							build();
					}
				}
				break;
			case IDCMP_VANILLAKEY: {
				char k = (char)(code >= 'A' && code <= 'Z' ? code + 32 : code);
				int shift = code >= 'A' && code <= 'Z';
				if (IS_LIST(page)) {
					if (k == 'l') {
						ActivateGadget(g_line, win, NULL);
						break;
					}
					if (k == 'd' || k == 'r' || k == 'e') {
						other_edit(k == 'd' ? G_ADD : k == 'r' ? G_REPL : G_DEL);
						break;
					}
				}
				for (i = 0; i < rp_nfields; i++)
					if (fgad[i] && rp_fkey(i) == k)
						break;
				if (i < rp_nfields) {
					if (rp_fdisabled(&conf.cur, i))
						break;
					if (rp_fields[i].type == F_STR || rp_fields[i].type == F_NUM) {
						ActivateGadget(fgad[i], win, NULL);
					} else if (harvest() == 0) {
						toggle(i, shift ? -1 : 1);
						build();
					}
				} else if (k == 'p') {
					if (harvest() == 0)
						show_page((page + (shift ? RP_NPAGES - 1 : 1)) % RP_NPAGES);
				} else if (k == 's' || k == 'u' || k == 'a' || k == 'c' || k == 27) {
					if (finish(k == 's' ? G_SAVE : k == 'u' ? G_USE : k == 'a' ? G_APPLY : G_CANCEL))
						return 0;
				}
				break;
			}
			}
		}
	}
}

int gt_run(const struct rp_gui_args *a)
{
	struct TagItem t[16];
	int n = 0, r = CL_FAIL, i;

	for (i = 0; i < RP_NPAGES; i++)
		page_names[i] = rp_pages[i].name;
	page_names[RP_NPAGES] = NULL;
	if (a->page >= 0)
		page = a->page;
	GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
	if (!GadToolsBase)
		goto out;
	rp_request_fn = gt_request;
	scr = LockPubScreen((CONST_STRPTR)a->pubscreen);
	if (!scr && a->pubscreen)
		scr = LockPubScreen(NULL);
	if (!scr || !(vi = GetVisualInfoA(scr, NULL)))
		goto out;
	layout();
	if (build() < 0) {
		rp_message("Cannot create the gadgets");
		goto out;
	}
#define TAG(a, b) (t[n].ti_Tag = (a), t[n++].ti_Data = (ULONG)(b))
	TAG(WA_Title, RP_TITLE);
	TAG(WA_PubScreen, scr);
	TAG(WA_Left, (scr->Width - inner_w) / 2 > 0 ? (scr->Width - inner_w) / 2 : 0);
	TAG(WA_Top, scr->BarHeight + 1);
	TAG(WA_InnerWidth, inner_w);
	TAG(WA_InnerHeight, inner_h);
	TAG(WA_Gadgets, glist);
	TAG(WA_IDCMP, BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP | CHECKBOXIDCMP | LISTVIEWIDCMP |
	    IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY);
	TAG(WA_DragBar, TRUE);
	TAG(WA_DepthGadget, TRUE);
	TAG(WA_CloseGadget, TRUE);
	TAG(WA_Activate, TRUE);
	TAG(WA_SimpleRefresh, TRUE);
	TAG(WA_AutoAdjust, TRUE);
	TAG(TAG_DONE, 0);
#undef TAG
	win = OpenWindowTagList(NULL, t);
	if (!win) {
		rp_message("Cannot open the window");
		goto out;
	}
	GT_RefreshWindow(win, NULL);
	r = event_loop();
out:
	rp_request_fn = NULL;
	if (win) {
		CloseWindow(win);
		win = NULL;
	}
	FreeGadgets(glist);
	glist = NULL;
	free_nodes();
	if (vi)
		FreeVisualInfo(vi);
	if (scr)
		UnlockPubScreen(NULL, scr);
	if (GadToolsBase)
		CloseLibrary(GadToolsBase);
	GadToolsBase = NULL;
	return r;
}
