/*
 * Riptide - RiptidePrefs: the MUI editor (muimaster.library 19, MUI 3.8,
 * or later). One window in the theme of mui_theme.c: the page list on the
 * left (nav), the page's head and settings on the right, the Save, Use,
 * Apply and Cancel buttons and a status line below. Every command is in
 * the menus and goes through command(); the settings come from
 * prefs_fields.c like the GadTools editor's, so both show the same pages.
 * Keyboard: the underlined letters, P and Shift-P (or the cursor keys)
 * for the pages, Esc to cancel.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "rt/rt_version.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_gui.h"
#include "prefs/mui_theme.h"
#include "prefs/mui_classes.h"
#include "prefs/mui_ids.h"
#include "prefs/mui_status.h"

#define NF RP_NFIELDS_MAX
#define IS_LIST(p) (rp_page_list(p) >= 0)

static Object *app, *win, *strip, *root, *nav, *head, *pages, *status, *bar;
static Object *btn[4];
static char *bhelp[4];
static Object *fobj[NF], *flab[NF];
static char *fhelp[NF];                  /* bubble texts, allocated */
static const char *cyl[NF][8];           /* translated cycle labels */
static Object *lst[RP_NPAGES], *lline[RP_NPAGES], *lbtn[RP_NPAGES][3], *lhelp_obj[RP_NPAGES][4];
static char *lhelp[RP_NPAGES][4];
/* the forms of the list pages (prefs_fields.c's rp_lfields[]) */
static Object *lfobj[RP_NLFIELDS_MAX], *lflab[RP_NLFIELDS_MAX];
static char *lfhelp[RP_NLFIELDS_MAX];
static const char *lcyl[RP_NLFIELDS_MAX][14];
static struct rp_lvals lv;
static int page;
/* the cards: the Interface page's list and buttons, the Address page's
 * card cycle; the cycles of names (DYN_*) are made anew when the names
 * change, inside a group of their own (dynbox) */
enum { DYN_ADDR, DYN_ALIAS, DYN_VLAN, DYN_N };
static Object *iflist, *ifbtn[2], *dynbox[DYN_N], *dyncyc[DYN_N];
static char *ifhelp[3], *dynhelp;
static struct rp_lnames dynnm[DYN_N];
static const char *dynlab[DYN_N][RP_NIF + RP_NVLAN + 1];
static int dynfield[DYN_N] = { -1, LA_IF, LV_PARENT };
static char status_text[120];
static const char *pubname;     /* PUBSCREEN=, for the message windows */

static const char *const btn_label[4] = { "_Save", "_Use", "_Apply", "_Cancel" };
static const ULONG btn_id[4] = { ID_SAVE, ID_USE, ID_APPLY, ID_CANCEL };
static const char *const btn_help[4] = {
	"Saves the settings for good and uses them now. Riptide is asked to take the changes it can.",
	"Uses the settings until the Amiga is switched off, without saving them for good.",
	"Gives the changes Riptide can take to the running stack now. The window stays open.",
	"Closes the window. Changes that were not saved or used are forgotten.",
};

/* ---- small things ---- */
static Object *rect(void)
{
	return mui_new(MUIC_Rectangle, TAGEND);
}

static Object *hgroup2(Object *a, Object *b)
{
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)a,
	               MUIA_Group_Child, (ULONG)b, TAGEND);
}

static Object *label(const char *text, int kind)
{
	return mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)text, MUIA_QLabel_Kind, (ULONG)kind, TAGEND);
}

static Object *button(const char *text)
{
	const char *u = strchr(text, '_');
	char key = u && u[1] ? (char)(u[1] >= 'A' && u[1] <= 'Z' ? u[1] + 32 : u[1]) : 0;
	return mui_newc(qbutton_mcc, MUIA_QButton_Label, (ULONG)text, MUIA_InputMode, MUIV_InputMode_RelVerify,
	                MUIA_ControlChar, (ULONG)key, MUIA_CycleChain, 1, TAGEND);
}

/* a bubble text kept with its object */
static char *bubble_dup(const char *name, const char *body)
{
	char buf[400], *p;

	theme_bubble_to(buf, sizeof(buf), name, body);
	p = rp_alloc(strlen(buf) + 1);
	if (p)
		strcpy(p, buf);
	return p;
}

/* the main window again after something covered it (MUI does not always
 * repair custom-drawn areas uncovered by a closing window) */
static void redraw_window(void)
{
	if (root && mui_get(win, MUIA_Window_Open)) {
		mui_do(root, MUIM_Group_InitChange);
		mui_do(root, MUIM_Group_ExitChange);
	}
}

/* A message in a small window of the theme, with buttons from "OK" or
 * "Apply|Later"; Return presses the first button, Esc (and the close
 * gadget) the last. Returns as EasyRequest() does: 1, 2 ... from the
 * left, 0 for the last button. */
static long mui_request(const char *text, const char *buttons)
{
	static char dlgspec[32];
	char labels[4][32];
	const char *b = tr(buttons);
	Object *dlg, *dlgroot, *txt, *row, *bt;
	ULONG sigs = 0;
	long r = 0;
	int n = 0, i;

	/* "_OK|_Later": each button's first letter is its key */
	while (*b && n < 4) {
		int k = 0;
		labels[n][k++] = '_';
		while (*b && *b != '|' && k < (int)sizeof(labels[n]) - 1)
			labels[n][k++] = *b++;
		labels[n][k] = 0;
		n++;
		if (*b == '|')
			b++;
	}
	rt_strlcpy(dlgspec, theme_spec(T_WINDOW), sizeof(dlgspec));
	txt = mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)text, MUIA_QLabel_Kind, QL_TEXT, TAGEND);
	row = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, TAGEND);
	if (!txt || !row || n == 0)
		return 0;
	/* the buttons sit right, at their own width */
	if ((bt = rect()) != NULL)
		mui_do(row, OM_ADDMEMBER, (ULONG)bt);
	for (i = 0; i < n; i++) {
		/* Return presses the first button, Esc the last (one button: both) */
		ULONG key = i == n - 1 && n > 1 ? 0x45 : i == 0 ? 0x44 : 0;
		bt = mui_newc(qbutton_mcc, MUIA_QButton_Label, (ULONG)labels[i], MUIA_InputMode,
		                      MUIV_InputMode_RelVerify, MUIA_ControlChar, (ULONG)(labels[i][1] | 0x20), MUIA_CycleChain,
		                      1, MUIA_QButton_Key, key, MUIA_QButton_ID, ID_DLGBTN + i, MUIA_HorizWeight, 0, TAGEND);
		if (!bt)
			return 0;
		mui_do(row, OM_ADDMEMBER, (ULONG)bt);
		mui_do(bt, MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_DLGBTN + i);
	}
	dlgroot = mui_new(MUIC_Group, MUIA_Group_Spacing, 10, MUIA_InnerLeft, 14, MUIA_InnerRight, 14, MUIA_InnerTop, 12,
	                  MUIA_InnerBottom, 10, MUIA_Background, (ULONG)dlgspec, MUIA_Group_Child, (ULONG)txt,
	                  MUIA_Group_Child, (ULONG)row, TAGEND);
	dlg = dlgroot ? mui_new(MUIC_Window, MUIA_Window_Title, (ULONG)tr(RP_TITLE), MUIA_Window_RootObject, (ULONG)dlgroot,
	                        MUIA_Window_RefWindow, (ULONG)win, MUIA_Window_LeftEdge, MUIV_Window_LeftEdge_Centered,
	                        MUIA_Window_TopEdge, MUIV_Window_TopEdge_Centered, MUIA_Window_Activate, TRUE,
	                        pubname ? MUIA_Window_PublicScreen : TAG_IGNORE, (ULONG)pubname, TAGEND) : NULL;
	if (!dlg)
		return 0;
	mui_do(app, OM_ADDMEMBER, (ULONG)dlg);
	mui_do(dlg, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)app, 2, MUIM_Application_ReturnID,
	       ID_DLGBTN + n - 1);
	mui_set(win, MUIA_Window_Sleep, TRUE);
	mui_set(dlg, MUIA_Window_Open, TRUE);
	rp_log("message: %s", text);
	for (;;) {
		LONG id = (LONG)mui_do(app, MUIM_Application_NewInput, (ULONG)&sigs);
		if (id >= ID_DLGBTN && id < ID_DLGBTN + n) {
			r = id == ID_DLGBTN + n - 1 ? 0 : id - ID_DLGBTN + 1;
			break;
		}
		if (!mui_get(dlg, MUIA_Window_Open))
			break;
		if (sigs)
			sigs = Wait(sigs);
	}
	rp_log("message answered %ld", r);
	mui_set(dlg, MUIA_Window_Open, FALSE);
	mui_do(app, OM_REMMEMBER, (ULONG)dlg);
	MUI_DisposeObject(dlg);
	mui_set(win, MUIA_Window_Sleep, FALSE);
	redraw_window();
	return r;
}

/* ---- the model and the gadgets ---- */
static void field_to_gadget(int i)
{
	const struct rp_field *f = &rp_fields[i];
	char num[16];

	if (!fobj[i])
		return;
	switch (f->type) {
	case F_STR:
		mui_nnset(fobj[i], MUIA_String_Contents, (ULONG)rp_fstr(&rp_cfg.cur, i));
		break;
	case F_NUM:
		num[0] = 0;
		if (rp_fget(&rp_cfg.cur, i) != RP_UNSET)
			rt_snprintf(num, sizeof(num), "%ld", rp_fget(&rp_cfg.cur, i));
		mui_nnset(fobj[i], MUIA_String_Contents, (ULONG)num);
		break;
	case F_CHECK:
		mui_nnset(fobj[i], MUIA_Selected, rp_fget(&rp_cfg.cur, i) != 0);
		break;
	case F_CYCLE:
		mui_nnset(fobj[i], MUIA_Cycle_Active, (ULONG)rp_fcycle(&rp_cfg.cur, i));
		break;
	}
}

/* enable and disable the settings that depend on DHCP and NTP */
static void update_disabled(void)
{
	int i;

	for (i = 0; i < rp_nfields; i++) {
		ULONG dis = (ULONG)rp_fdisabled(&rp_cfg.cur, i);
		if (fobj[i] && mui_get(fobj[i], MUIA_Disabled) != dis) {
			mui_set(fobj[i], MUIA_Disabled, dis);
			mui_set(flab[i], MUIA_Disabled, dis);
		}
	}
}

/* does page p edit its entries with a form? */
static int has_form(int p)
{
	int i;

	for (i = 0; i < rp_nlfields; i++)
		if (rp_lfields[i].page == p)
			return 1;
	return 0;
}

/* the names a form's cycle offers (the cards, and for addresses their
 * VLANs too) */
static const struct rp_lnames *form_names(int p)
{
	return &dynnm[p == P_ALIAS ? DYN_ALIAS : DYN_VLAN];
}

/* which fields of the form apply (the address only with Static), and
 * what the shown-only ones say (the VLAN's name) */
static void form_disabled(int p)
{
	int i;

	for (i = 0; i < rp_nlfields; i++)
		if (rp_lfields[i].page == p && lfobj[i]) {
			ULONG dis = (ULONG)rp_lform_disabled(i, &lv);
			if (rp_lfields[i].type == LF_INFO)
				mui_set(lfobj[i], MUIA_QLabel_Text, (ULONG)lv.tx[i]);
			if (mui_get(lfobj[i], MUIA_Disabled) != dis) {
				mui_set(lfobj[i], MUIA_Disabled, dis);
				mui_set(lflab[i], MUIA_Disabled, dis);
			}
		}
}

/* the form's values into its gadgets */
static void form_show(int p)
{
	int i;

	for (i = 0; i < rp_nlfields; i++) {
		if (rp_lfields[i].page != p || !lfobj[i])
			continue;
		if (rp_lfields[i].type == LF_CYCLE)
			mui_nnset(lfobj[i], MUIA_Cycle_Active, (ULONG)lv.cy[i]);
		else if (rp_lfields[i].type != LF_INFO)
			mui_nnset(lfobj[i], MUIA_String_Contents, (ULONG)lv.tx[i]);
	}
	form_disabled(p);
}

/* the gadgets' contents into the form's values */
static void form_read(int p)
{
	int i;

	for (i = 0; i < rp_nlfields; i++) {
		if (rp_lfields[i].page != p || !lfobj[i])
			continue;
		if (rp_lfields[i].type == LF_CYCLE)
			lv.cy[i] = (int)mui_get(lfobj[i], MUIA_Cycle_Active);
		else if (rp_lfields[i].type != LF_INFO)
			rt_strlcpy(lv.tx[i], (const char *)mui_get(lfobj[i], MUIA_String_Contents), RP_LFTEXT);
	}
	rp_lform_update(p, &lv, form_names(p));
}

static void fill_list(int p)
{
	char row[160];
	int i, n = rp_list_n(p);

	if (!lst[p])
		return;
	mui_set(lst[p], MUIA_List_Quiet, TRUE);
	mui_do(lst[p], MUIM_List_Clear);
	for (i = 0; i < n; i++) {
		const char *e = rp_list_at(p, i);
		if (has_form(p)) {
			rp_list_row(p, e, form_names(p), row, sizeof(row));
			e = row;
		}
		mui_do(lst[p], MUIM_List_InsertSingle, (ULONG)e, MUIV_List_Insert_Bottom);
	}
	mui_set(lst[p], MUIA_List_Quiet, FALSE);
}

/* a list page's form (or line) ready for a new entry */
static void form_clear(int p)
{
	if (has_form(p)) {
		rp_lform_get(p, NULL, form_names(p), &lv);
		form_show(p);
	} else if (lline[p]) {
		mui_nnset(lline[p], MUIA_String_Contents, (ULONG)"");
	}
}

/* the card a cycle or list position stands for (they skip unused slots) */
static int card_at(int pos)
{
	int k;

	for (k = 0; k < RP_NIF; k++)
		if (rp_cfg.cur.ifc[k].ifname[0] && pos-- == 0)
			return k;
	return 0;
}

static int card_pos(int k)
{
	int i, pos = 0;

	for (i = 0; i < k; i++)
		pos += rp_cfg.cur.ifc[i].ifname[0] != 0;
	return pos;
}

/* A cycle of names (DYN_*) with the names now in dynnm[d]: made anew in its
 * group when they changed (MUI 3.8's cycles cannot change their labels). */
static void dyn_set(int d, const struct rp_lnames *nm)
{
	static const char *const labels[DYN_N] = { "Car_d", NULL, NULL };
	const char *lab = d == DYN_ADDR ? labels[d] : rp_lfields[dynfield[d]].label;
	Object *c;
	int i, live, same = dyncyc[d] && nm->n == dynnm[d].n;

	for (i = 0; same && i < nm->n; i++)
		same = !strcmp(nm->name[i], dynnm[d].name[i]);
	if (same || !dynbox[d])
		return;
	dynnm[d] = *nm;
	for (i = 0; i < nm->n; i++)
		dynlab[d][i] = dynnm[d].name[i];
	dynlab[d][i] = NULL;
	if (!nm->n) {
		dynlab[d][0] = "eth0";
		dynlab[d][1] = NULL;
	}
	c = mui_make(MUIO_Cycle, (ULONG)tr(lab), (ULONG)dynlab[d]);
	if (!c)
		return;
	mui_set(c, MUIA_CycleChain, 1);
	mui_set(c, MUIA_HorizWeight, 0);
	if (d == DYN_ADDR) {
		if (theme_tips())
			mui_set(c, MUIA_ShortHelp, (ULONG)dynhelp);
		mui_do(c, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_IFCYC);
	} else {
		if (theme_tips())
			mui_set(c, MUIA_ShortHelp, (ULONG)lfhelp[dynfield[d]]);
		mui_do(c, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID,
		       ID_LFIELD + dynfield[d]);
	}
	live = win && mui_get(win, MUIA_Window_Open);
	if (live)
		mui_do(dynbox[d], MUIM_Group_InitChange);
	if (dyncyc[d]) {
		mui_do(dynbox[d], OM_REMMEMBER, (ULONG)dyncyc[d]);
		MUI_DisposeObject(dyncyc[d]);
	}
	mui_do(dynbox[d], OM_ADDMEMBER, (ULONG)c);
	if (live)
		mui_do(dynbox[d], MUIM_Group_ExitChange);
	dyncyc[d] = c;
	if (d != DYN_ADDR)
		lfobj[dynfield[d]] = c;
}

/* the Interface page's list of cards and the Address page's cycle */
static void cards_show(void)
{
	struct rp_lnames nm;
	char row[160];
	int k;

	if (iflist) {
		mui_set(iflist, MUIA_List_Quiet, TRUE);
		mui_do(iflist, MUIM_List_Clear);
		for (k = 0; k < RP_NIF; k++)
			if (rp_cfg.cur.ifc[k].ifname[0]) {
				rp_iface_row(&rp_cfg.cur.ifc[k], row, sizeof(row));
				mui_do(iflist, MUIM_List_InsertSingle, (ULONG)row, MUIV_List_Insert_Bottom);
			}
		mui_set(iflist, MUIA_List_Quiet, FALSE);
		mui_nnset(iflist, MUIA_List_Active, (ULONG)card_pos(rp_ifcur(&rp_cfg.cur)));
	}
	nm.n = rp_iface_names(&rp_cfg.cur, nm.name, RP_NIF, 0);
	dyn_set(DYN_ADDR, &nm);
	if (dyncyc[DYN_ADDR])
		mui_nnset(dyncyc[DYN_ADDR], MUIA_Cycle_Active, (ULONG)card_pos(rp_ifcur(&rp_cfg.cur)));
	if (ifbtn[1])
		mui_set(ifbtn[1], MUIA_Disabled, (ULONG)(rp_ifcur(&rp_cfg.cur) == 0));
}

/* the names the address and VLAN forms offer, after cards or VLANs changed */
static void names_show(void)
{
	struct rp_lnames nm;
	int p;

	for (p = P_ALIAS; p <= P_VLAN; p++) {
		int d = p == P_ALIAS ? DYN_ALIAS : DYN_VLAN, same, i;
		rp_lform_names(&rp_cfg.cur, p, &nm);
		same = nm.n == dynnm[d].n;
		for (i = 0; same && i < nm.n; i++)
			same = !strcmp(nm.name[i], dynnm[d].name[i]);
		if (same)
			continue;
		dyn_set(d, &nm);
		if (lst[p]) {
			int sel = (int)(LONG)mui_get(lst[p], MUIA_List_Active);
			fill_list(p);
			mui_nnset(lst[p], MUIA_List_Active, sel >= 0 ? (ULONG)sel : (ULONG)MUIV_List_Active_Off);
			if (sel >= 0 && rp_list_at(p, sel))
				rp_lform_get(p, rp_list_at(p, sel), form_names(p), &lv);
			else
				rp_lform_get(p, NULL, form_names(p), &lv);
			form_show(p);
		}
	}
}

/* The text of the chosen card's string fields into the model (before
 * another card is shown, the page changes or the card is renamed); with
 * loud, a bad one is shown with a message and -1 returned. */
static int harvest_card(int loud)
{
	char err[80], msg[200], name[48];
	int i;

	for (i = 0; i < rp_nfields; i++) {
		const struct rp_field *f = &rp_fields[i];
		if (!fobj[i] || !(f->flags & FL_IF) || (f->type != F_STR && f->type != F_NUM))
			continue;
		if (rp_fparse(&rp_cfg.cur, i, (const char *)mui_get(fobj[i], MUIA_String_Contents), err, sizeof(err)) < 0 &&
		    loud) {
			rp_flabel(i, name, sizeof(name));
			rt_snprintf(msg, sizeof(msg), "%s: %s", tr(name), tr(err));
			mui_set(win, MUIA_Window_ActiveObject, (ULONG)fobj[i]);
			rp_message(msg);
			return -1;
		}
	}
	return 0;
}

/* card k on the Interface and Address pages */
static void card_choose(int k)
{
	int i;

	rp_ifsel = k;
	for (i = 0; i < rp_nfields; i++)
		if (rp_fields[i].flags & FL_IF)
			field_to_gadget(i);
	update_disabled();
	cards_show();
}

/* everything from the model, after it was replaced (Open, Restore ...) */
static void refresh_all(void)
{
	int i;

	rp_ifsel = rp_ifcur(&rp_cfg.cur);
	cards_show();
	names_show();
	for (i = 0; i < rp_nfields; i++)
		field_to_gadget(i);
	for (i = 0; i < RP_NPAGES; i++)
		if (IS_LIST(i)) {
			fill_list(i);
			form_clear(i);
		}
	update_disabled();
}

static void show_status(void)
{
	int running = FindPort((CONST_STRPTR)RIPTIDE_PORTNAME) != NULL;

	rt_snprintf(status_text, sizeof(status_text), "%s%s", tr(running ? "Riptide is running." :
	            "Riptide is not running."),
	            rp_conf_changed(&rp_cfg) ? tr("  Changes not saved yet.") : "");
	mui_set(status, MUIA_Status_Text, (ULONG)status_text);
}

/* show page p (the page list tells the head and the page group) */
static void go_page(int p)
{
	mui_set(nav, MUIA_Nav_Active, (ULONG)p);
	page = p;
}

/* the text of the string fields into the model; -1 (and the field shown
 * with a message) on a bad number */
static int harvest(void)
{
	char err[80], msg[200], name[48];
	int i;

	for (i = 0; i < rp_nfields; i++) {
		const struct rp_field *f = &rp_fields[i];
		if (!fobj[i] || (f->type != F_STR && f->type != F_NUM))
			continue;
		if (rp_fparse(&rp_cfg.cur, i, (const char *)mui_get(fobj[i], MUIA_String_Contents), err,
		              sizeof(err)) < 0) {
			rp_flabel(i, name, sizeof(name));
			rt_snprintf(msg, sizeof(msg), "%s: %s", tr(name), tr(err));
			go_page(f->page);
			mui_set(win, MUIA_Window_ActiveObject, (ULONG)fobj[i]);
			rp_message(msg);
			return -1;
		}
	}
	return 0;
}

static int validate(void)
{
	char err[200], msg[300], name[48];
	int fi;

	if (rp_validate(&rp_cfg.cur, &fi, err, sizeof(err)) == 0)
		return 0;
	if (rp_vpage >= 0) {
		/* a VLAN or address names a card that is not there */
		go_page(rp_vpage);
		rp_message(tr(err));
		return -1;
	}
	card_choose(rp_ifsel);
	rp_flabel(fi, name, sizeof(name));
	rt_snprintf(msg, sizeof(msg), "%s: %s", tr(name), tr(err));
	go_page(rp_fields[fi].page);
	if (fobj[fi])
		mui_set(win, MUIA_Window_ActiveObject, (ULONG)fobj[fi]);
	rp_message(msg);
	return -1;
}

/* ---- building the window ---- */
static Object *field_gadget(int i)
{
	const struct rp_field *f = &rp_fields[i];
	Object *g = NULL;
	int k;

	switch (f->type) {
	case F_STR:
	case F_NUM:
		g = mui_make(MUIO_String, (ULONG)tr(f->label), f->type == F_STR ? (ULONG)f->size : 11UL);
		break;
	case F_CHECK:
		/* MUIO_Checkmark stretches with its row (FreeVert): this one keeps its size */
		g = mui_new(MUIC_Image, MUIA_Frame, MUIV_Frame_ImageButton, MUIA_InputMode, MUIV_InputMode_Toggle,
		            MUIA_Image_Spec, MUII_CheckMark, MUIA_Image_FreeVert, FALSE, MUIA_Background, MUII_ButtonBack,
		            MUIA_ShowSelState, FALSE, MUIA_ControlChar, (ULONG)(unsigned char)rp_fkey(i), TAGEND);
		break;
	case F_CYCLE:
		for (k = 0; f->cy[k] && k < 7; k++)
			cyl[i][k] = tr(f->cy[k]);
		cyl[i][k] = NULL;
		g = mui_make(MUIO_Cycle, (ULONG)tr(f->label), (ULONG)cyl[i]);
		break;
	}
	if (g)
		mui_set(g, MUIA_CycleChain, 1);
	return g;
}

/* The Interface page's list of cards with New card and Remove card
 * beside it: a few rows, the settings of the card chosen below. */
static Object *cards_build(void)
{
	static const char *const names[2] = { "N_ew card", "Rem_ove card" };
	static const char *const helps[3] = {
		"The network cards Riptide uses. Click one to see and change its settings below and on the "
		"Address page.",
		"Adds a network card: enter its SANA-II device and unit below.",
		"Removes the card chosen, with its VLANs and their addresses. The first card stays.",
	};
	Object *lvo, *col;
	int k;

	iflist = mui_new(MUIC_List, MUIA_Frame, MUIV_Frame_InputList, MUIA_List_ConstructHook,
	                 MUIV_List_ConstructHook_String, MUIA_List_DestructHook, MUIV_List_DestructHook_String, TAGEND);
	lvo = iflist ? mui_new(MUIC_Listview, MUIA_Listview_List, (ULONG)iflist, MUIA_CycleChain, 1, MUIA_FixHeightTxt,
	                       (ULONG)(theme_compact() ? "M\nM" : "M\nM\nM"), TAGEND) : NULL;
	col = mui_new(MUIC_Group, MUIA_Group_Spacing, 4, MUIA_HorizWeight, 0, TAGEND);
	if (!lvo || !col)
		return NULL;
	ifhelp[0] = bubble_dup("Cards", helps[0]);
	mui_set(lvo, MUIA_ShortHelp, (ULONG)ifhelp[0]);
	for (k = 0; k < 2; k++) {
		char name[16];
		const char *s;
		int n = 0;
		ifbtn[k] = button(names[k]);
		if (!ifbtn[k])
			return NULL;
		for (s = names[k]; *s && n < (int)sizeof(name) - 1; s++)
			if (*s != '_')
				name[n++] = *s;
		name[n] = 0;
		ifhelp[k + 1] = bubble_dup(name, helps[k + 1]);
		mui_set(ifbtn[k], MUIA_ShortHelp, (ULONG)ifhelp[k + 1]);
		mui_do(col, OM_ADDMEMBER, (ULONG)ifbtn[k]);
		mui_do(ifbtn[k], MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID,
		       k ? ID_IFDEL : ID_IFADD);
	}
	mui_do(col, OM_ADDMEMBER, (ULONG)rect());
	mui_do(iflist, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_IFLIST);
	return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, MUIA_VertWeight, 0, MUIA_Group_Child,
	               (ULONG)lvo, MUIA_Group_Child, (ULONG)col, TAGEND);
}

/* the settings of a page: a label and a gadget per row */
static Object *field_page(int p)
{
	Object *cols = mui_new(MUIC_Group, MUIA_Group_Columns, 2, MUIA_Group_HorizSpacing, 8,
	                       MUIA_Group_VertSpacing, theme_compact() ? 2 : 4, TAGEND);
	int i;

	if (!cols)
		return NULL;
	if (p == P_ADDR) {
		/* which card's address: a cycle of the cards' names */
		Object *l = label("Car_d", QL_LABEL);
		dynbox[DYN_ADDR] = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, TAGEND);
		dynhelp = bubble_dup("Card", "Whose address this page shows. Add and remove cards on the Interface page.");
		if (!l || !dynbox[DYN_ADDR])
			return NULL;
		mui_set(l, MUIA_ShortHelp, (ULONG)dynhelp);
		mui_do(cols, OM_ADDMEMBER, (ULONG)l);
		mui_do(cols, OM_ADDMEMBER, (ULONG)hgroup2(dynbox[DYN_ADDR], rect()));
	}
	for (i = 0; i < rp_nfields; i++) {
		const struct rp_field *f = &rp_fields[i];
		Object *g, *cell;
		char name[48];
		if (f->page != p)
			continue;
		g = field_gadget(i);
		flab[i] = label(f->label, QL_LABEL);
		if (!g || !flab[i])
			return NULL;
		fobj[i] = g;
		rp_flabel(i, name, sizeof(name));
		fhelp[i] = bubble_dup(name, f->help);
		mui_set(g, MUIA_ShortHelp, (ULONG)fhelp[i]);
		mui_set(flab[i], MUIA_ShortHelp, (ULONG)fhelp[i]);
		/* strings fill the column; check boxes and cycles keep their size */
		cell = f->type == F_STR || f->type == F_NUM ? g : hgroup2(g, rect());
		mui_do(cols, OM_ADDMEMBER, (ULONG)flab[i]);
		mui_do(cols, OM_ADDMEMBER, (ULONG)cell);
		if (f->type == F_CHECK || f->type == F_CYCLE)
			mui_do(g, MUIM_Notify, f->type == F_CHECK ? MUIA_Selected : MUIA_Cycle_Active, MUIV_EveryTime,
			       (ULONG)app, 2, MUIM_Application_ReturnID, ID_FIELD + i);
		field_to_gadget(i);
	}
	if (p == P_IFACE) {
		Object *cards = cards_build();
		return cards ? mui_new(MUIC_Group, MUIA_Group_Spacing, theme_compact() ? 2 : 6, MUIA_Group_Child, (ULONG)cards,
		                       MUIA_Group_Child, (ULONG)cols, TAGEND) : NULL;
	}
	return cols;
}

/* the gadget of a form field */
static Object *lform_gadget(int i)
{
	const struct rp_lfield *f = &rp_lfields[i];
	Object *g = NULL;
	int k;

	switch (f->type) {
	case LF_NUM:
		g = mui_make(MUIO_String, (ULONG)tr(f->label), 9UL);
		break;
	case LF_STR:
		g = mui_make(MUIO_String, (ULONG)tr(f->label), (ULONG)f->size);
		break;
	case LF_CYCLE:
		if (f->flags & LF_NAMES)
			/* made by dyn_set() into this group */
			return mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_HorizWeight, 0, TAGEND);
		for (k = 0; f->cy[k] && k < 13; k++)
			lcyl[i][k] = tr(f->cy[k]);
		lcyl[i][k] = NULL;
		g = mui_make(MUIO_Cycle, (ULONG)tr(f->label), (ULONG)lcyl[i]);
		break;
	case LF_INFO:
		/* as wide as the longest name it shows */
		g = mui_newc(qlabel_mcc, MUIA_QLabel_Text, (ULONG)"eth0.4094  ", MUIA_QLabel_Kind, QL_TEXT, TAGEND);
		break;
	}
	if (!g)
		return NULL;
	if (f->type == LF_CYCLE || f->type == LF_INFO)
		mui_set(g, MUIA_HorizWeight, 0);
	if (f->type != LF_INFO)
		mui_set(g, MUIA_CycleChain, 1);
	if (f->type == LF_CYCLE)
		mui_do(g, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID,
		       ID_LFIELD + i);
	else if (f->type != LF_INFO)
		mui_do(g, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID,
		       ID_LFIELD + i);
	return g;
}

/* The form of a list page: a label and a row per field, the fields
 * marked LF_SAMEROW on the row of the one before with their own label.
 * A row of strings fills the width; cycles keep their size. */
static Object *lform_build(int p)
{
	Object *cols = mui_new(MUIC_Group, MUIA_Group_Columns, 2, MUIA_Group_HorizSpacing, 8,
	                       MUIA_Group_VertSpacing, theme_compact() ? 1 : 3, MUIA_VertWeight, 0, TAGEND);
	int i, k;

	if (!cols)
		return NULL;
	for (i = 0; i < rp_nlfields; i++) {
		const struct rp_lfield *f = &rp_lfields[i];
		Object *row, *r;
		int last = i, fills = 0;
		if (f->page != p || (f->flags & LF_SAMEROW))
			continue;
		while (last + 1 < rp_nlfields && rp_lfields[last + 1].page == p && (rp_lfields[last + 1].flags & LF_SAMEROW))
			last++;
		row = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 8, TAGEND);
		if (!row)
			return NULL;
		for (k = i; k <= last; k++) {
			char name[48];
			Object *g = lform_gadget(k);
			lflab[k] = label(rp_lfields[k].label, QL_LABEL);
			if (!g || !lflab[k])
				return NULL;
			rp_lflabel(k, name, sizeof(name));
			lfhelp[k] = bubble_dup(name, rp_lfields[k].help);
			mui_set(lflab[k], MUIA_ShortHelp, (ULONG)lfhelp[k]);
			if (rp_lfields[k].flags & LF_NAMES) {
				/* the cycle of names comes in dyn_set() */
				dynbox[p == P_ALIAS ? DYN_ALIAS : DYN_VLAN] = g;
				lfobj[k] = NULL;
			} else {
				lfobj[k] = g;
				mui_set(g, MUIA_ShortHelp, (ULONG)lfhelp[k]);
			}
			if (k > i)
				mui_do(row, OM_ADDMEMBER, (ULONG)lflab[k]);
			mui_do(row, OM_ADDMEMBER, (ULONG)g);
			fills |= rp_lfields[k].type == LF_NUM || rp_lfields[k].type == LF_STR;
		}
		if (!fills && (r = rect()) != NULL)
			mui_do(row, OM_ADDMEMBER, (ULONG)r);
		mui_do(cols, OM_ADDMEMBER, (ULONG)lflab[i]);
		mui_do(cols, OM_ADDMEMBER, (ULONG)row);
	}
	{
		struct rp_lnames nm;
		rp_lform_names(&rp_cfg.cur, p, &nm);
		dyn_set(p == P_ALIAS ? DYN_ALIAS : DYN_VLAN, &nm);
	}
	rp_lform_get(p, NULL, form_names(p), &lv);
	form_show(p);
	return cols;
}

/* a list page: the list, then the form of an entry (or, for the other
 * lines, the line being edited) and Add/Replace/Delete */
static Object *list_page(int p)
{
	static const char *const names[3] = { "A_dd", "_Replace", "D_elete" };
	static const char *const helps[4] = {
		"The entries of this page. Click one to edit it in the line below.",
		"Adds the line below as a new entry.",
		"Puts the line below in place of the entry chosen in the list.",
		"Removes the entry chosen in the list.",
	};
	static const char *const fhelps[4] = {
		"The entries of this page. Click one to see and change its settings below.",
		"Adds the settings below as a new entry.",
		"Puts the settings below in place of the entry chosen in the list.",
		"Removes the entry chosen in the list.",
	};
	const char *const *hp = has_form(p) ? fhelps : helps;
	Object *lvo, *row, *btns, *l;
	int k;

	lst[p] = mui_new(MUIC_List, MUIA_Frame, MUIV_Frame_InputList, MUIA_List_ConstructHook,
	                 MUIV_List_ConstructHook_String, MUIA_List_DestructHook, MUIV_List_DestructHook_String, TAGEND);
	lvo = mui_new(MUIC_Listview, MUIA_Listview_List, (ULONG)lst[p], MUIA_CycleChain, 1, TAGEND);
	if (!lvo)
		return NULL;
	if (has_form(p)) {
		row = lform_build(p);
	} else {
		lline[p] = mui_make(MUIO_String, (ULONG)tr("_Line"), RP_LONG * 2);
		l = label("_Line", QL_LABEL);
		if (!lline[p] || !l)
			return NULL;
		mui_set(lline[p], MUIA_CycleChain, 1);
		row = hgroup2(l, lline[p]);
		if (row)
			mui_set(row, MUIA_Group_Spacing, 8);
	}
	btns = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_SameSize, TRUE, MUIA_VertWeight, 0, TAGEND);
	if (!row || !btns)
		return NULL;
	lhelp[p][0] = bubble_dup("List", hp[0]);
	mui_set(lvo, MUIA_ShortHelp, (ULONG)lhelp[p][0]);
	lhelp_obj[p][0] = lvo;
	for (k = 0; k < 3; k++) {
		char name[16];
		const char *s;
		int n = 0;
		lbtn[p][k] = button(names[k]);
		if (!lbtn[p][k])
			return NULL;
		for (s = names[k]; *s && n < (int)sizeof(name) - 1; s++)
			if (*s != '_')
				name[n++] = *s;
		name[n] = 0;
		lhelp[p][k + 1] = bubble_dup(name, hp[k + 1]);
		mui_set(lbtn[p][k], MUIA_ShortHelp, (ULONG)lhelp[p][k + 1]);
		lhelp_obj[p][k + 1] = lbtn[p][k];
		mui_do(btns, OM_ADDMEMBER, (ULONG)lbtn[p][k]);
		mui_do(lbtn[p][k], MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID,
		       ID_LIST + p * 4 + LIST_ADD + k);
	}
	mui_do(lst[p], MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID,
	       ID_LIST + p * 4 + LIST_SEL);
	fill_list(p);
	return mui_new(MUIC_Group, MUIA_Group_VertSpacing, theme_compact() ? 2 : 4, MUIA_Group_Child, (ULONG)lvo,
	               MUIA_Group_Child, (ULONG)row, MUIA_Group_Child, (ULONG)btns, TAGEND);
}

static Object *build_page(int p)
{
	Object *body, *filler, *note;

	if (p == P_STATUS) {
		ULONG in = theme_compact() ? 4 : 10;
		body = status_page_build(app, ID_STATUS_CONN, ID_STATUS_UPD);
		return body ? mui_new(MUIC_Group, MUIA_InnerLeft, 12, MUIA_InnerRight, 12, MUIA_InnerTop, in,
		                      MUIA_InnerBottom, in - 2, MUIA_Group_Child, (ULONG)body, TAGEND) : NULL;
	}
	body = IS_LIST(p) ? list_page(p) : field_page(p);

	note = label(has_form(p) ? rp_lform_note(p) : rp_pages[p].note, QL_NOTE);
	filler = IS_LIST(p) ? NULL : rect();
	if (!body || !note || (!IS_LIST(p) && !filler))
		return NULL;
	{
		ULONG in = theme_compact() ? 4 : 10;
		if (filler)
			return mui_new(MUIC_Group, MUIA_InnerLeft, 12, MUIA_InnerRight, 12, MUIA_InnerTop, in,
			               MUIA_InnerBottom, in - 2, MUIA_Group_VertSpacing, theme_compact() ? 2 : 4,
			               MUIA_Group_Child, (ULONG)body, MUIA_Group_Child, (ULONG)filler, MUIA_Group_Child,
			               (ULONG)note, TAGEND);
		return mui_new(MUIC_Group, MUIA_InnerLeft, 12, MUIA_InnerRight, 12, MUIA_InnerTop, in, MUIA_InnerBottom,
		               in - 2, MUIA_Group_VertSpacing, theme_compact() ? 2 : 4, MUIA_Group_Child, (ULONG)body,
		               MUIA_Group_Child, (ULONG)note, TAGEND);
	}
}

/* the menus: every command, with its Amiga-key shortcut */
static struct NewMenu menus[] = {
	{ NM_TITLE, (CONST_STRPTR)"Project", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Open...", (CONST_STRPTR)"O", 0, 0, (APTR)ID_OPEN },
	{ NM_ITEM, (CONST_STRPTR)"Save As...", (CONST_STRPTR)"A", 0, 0, (APTR)ID_SAVEAS },
	{ NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Save", (CONST_STRPTR)"S", 0, 0, (APTR)ID_SAVE },
	{ NM_ITEM, (CONST_STRPTR)"Use", (CONST_STRPTR)"U", 0, 0, (APTR)ID_USE },
	{ NM_ITEM, (CONST_STRPTR)"Apply", (CONST_STRPTR)"Y", 0, 0, (APTR)ID_APPLY },
	{ NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"About...", (CONST_STRPTR)"?", 0, 0, (APTR)ID_ABOUT },
	{ NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Quit", (CONST_STRPTR)"Q", 0, 0, (APTR)ID_QUIT },
	{ NM_TITLE, (CONST_STRPTR)"Edit", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Reset to Defaults", (CONST_STRPTR)"D", 0, 0, (APTR)ID_DEFAULTS },
	{ NM_ITEM, (CONST_STRPTR)"Last Saved", (CONST_STRPTR)"L", 0, 0, (APTR)ID_LASTSAVED },
	{ NM_ITEM, (CONST_STRPTR)"Restore", (CONST_STRPTR)"R", 0, 0, (APTR)ID_RESTORE },
	{ NM_TITLE, (CONST_STRPTR)"Settings", 0, 0, 0, 0 },
	{ NM_ITEM, (CONST_STRPTR)"Dark Theme", (CONST_STRPTR)"T", CHECKIT | MENUTOGGLE, 0, (APTR)ID_DARK },
	{ NM_ITEM, (CONST_STRPTR)"Help Bubbles", (CONST_STRPTR)"H", CHECKIT | MENUTOGGLE, 0, (APTR)ID_TIPS },
	{ NM_END, 0, 0, 0, 0, 0 },
};

static Object *build_menus(void)
{
	int i;

	for (i = 0; menus[i].nm_Type != NM_END; i++) {
		if (menus[i].nm_Label != NM_BARLABEL)
			menus[i].nm_Label = (CONST_STRPTR)tr((const char *)menus[i].nm_Label);
		if ((ULONG)menus[i].nm_UserData == ID_DARK)
			menus[i].nm_Flags = (UWORD)(CHECKIT | MENUTOGGLE | (theme_dark() ? CHECKED : 0));
		if ((ULONG)menus[i].nm_UserData == ID_TIPS)
			menus[i].nm_Flags = (UWORD)(CHECKIT | MENUTOGGLE | (theme_tips() ? CHECKED : 0));
	}
	return mui_make(MUIO_MenustripNM, (ULONG)menus, 0);
}

static int build_window(const char *pubscreen)
{
	Object *right, *top;
	int i;

	nav = mui_newc(nav_mcc, MUIA_Nav_Active, (ULONG)page, MUIA_ShortHelp, (ULONG)" ", TAGEND);
	head = mui_newc(head_mcc, MUIA_Head_Page, (ULONG)page, TAGEND);
	pages = mui_new(MUIC_Group, MUIA_Group_PageMode, TRUE, TAGEND);
	status = mui_newc(status_mcc, TAGEND);
	bar = mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_SameSize, TRUE, MUIA_Group_Spacing, 8,
	              MUIA_InnerLeft, 8, MUIA_InnerRight, 8, MUIA_InnerTop, theme_compact() ? 3 : 6,
	              MUIA_InnerBottom, theme_compact() ? 3 : 6,
	              MUIA_Background, (ULONG)theme_spec(T_PANEL), TAGEND);
	if (!nav || !head || !pages || !status || !bar) {
		rp_log("objects: nav %p head %p pages %p status %p bar %p", nav, head, pages, status, bar);
		return -1;
	}
	for (i = 0; i < RP_NPAGES; i++) {
		Object *pg = build_page(i);
		if (!pg) {
			rp_log("page %d", i);
			return -1;
		}
		mui_do(pages, OM_ADDMEMBER, (ULONG)pg);
	}
	for (i = 0; i < 4; i++) {
		char name[16];
		const char *s;
		int n = 0;
		btn[i] = button(btn_label[i]);
		if (!btn[i])
			return -1;
		for (s = btn_label[i]; *s && n < (int)sizeof(name) - 1; s++)
			if (*s != '_')
				name[n++] = *s;
		name[n] = 0;
		bhelp[i] = bubble_dup(name, btn_help[i]);
		mui_set(btn[i], MUIA_ShortHelp, (ULONG)bhelp[i]);
		mui_do(bar, OM_ADDMEMBER, (ULONG)btn[i]);
		mui_do(btn[i], MUIM_Notify, MUIA_Pressed, FALSE, (ULONG)app, 2, MUIM_Application_ReturnID, btn_id[i]);
	}
	mui_set(pages, MUIA_Group_ActivePage, (ULONG)page);
	right = mui_new(MUIC_Group, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)head, MUIA_Group_Child,
	                (ULONG)pages, TAGEND);
	top = right ? mui_new(MUIC_Group, MUIA_Group_Horiz, TRUE, MUIA_Group_Spacing, 0, MUIA_Group_Child, (ULONG)nav,
	                      MUIA_Group_Child, (ULONG)right, TAGEND) : NULL;
	root = top ? mui_new(MUIC_Group, MUIA_Group_Spacing, 0, MUIA_InnerLeft, 0, MUIA_InnerRight, 0, MUIA_InnerTop, 0,
	                     MUIA_InnerBottom, 0, MUIA_Background, (ULONG)theme_spec(T_WINDOW), MUIA_Group_Child,
	                     (ULONG)top, MUIA_Group_Child, (ULONG)mui_newc(line_mcc, TAGEND), MUIA_Group_Child,
	                     (ULONG)bar, MUIA_Group_Child, (ULONG)status, TAGEND) : NULL;
	strip = build_menus();
	if (!root || !strip) {
		rp_log("root %p strip %p", root, strip);
		return -1;
	}
	win = mui_new(MUIC_Window, MUIA_Window_Title, (ULONG)tr(RP_TITLE), MUIA_Window_ID, 0x52505246UL /* RPRF */,
	              MUIA_Window_Menustrip, (ULONG)strip, MUIA_Window_RootObject, (ULONG)root,
	              pubscreen ? MUIA_Window_PublicScreen : TAG_IGNORE, (ULONG)pubscreen, TAGEND);
	if (!win) {
		rp_log("window object");
		return -1;
	}
	mui_do(app, OM_ADDMEMBER, (ULONG)win);
	mui_do(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (ULONG)app, 2, MUIM_Application_ReturnID, ID_CANCEL);
	mui_do(app, MUIM_Notify, MUIA_Application_DoubleStart, MUIV_EveryTime, (ULONG)app, 2,
	       MUIM_Application_ReturnID, ID_FRONT);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)pages, 3, MUIM_Set, MUIA_Group_ActivePage,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)head, 3, MUIM_Set, MUIA_Head_Page,
	       MUIV_TriggerValue);
	mui_do(nav, MUIM_Notify, MUIA_Nav_Active, MUIV_EveryTime, (ULONG)app, 2, MUIM_Application_ReturnID, ID_PAGE);
	cards_show();
	update_disabled();
	return 0;
}

/* the window again after the theme changed: pens are taken anew */
static void reopen(void)
{
	mui_set(win, MUIA_Window_Open, FALSE);
	mui_set(root, MUIA_Background, (ULONG)theme_spec(T_WINDOW));
	mui_set(bar, MUIA_Background, (ULONG)theme_spec(T_PANEL));
	mui_set(win, MUIA_Window_Open, TRUE);
}

static void set_tips(int on)
{
	int i, k;

	theme_set_tips(on);
	for (i = 0; i < rp_nfields; i++)
		if (fobj[i]) {
			mui_set(fobj[i], MUIA_ShortHelp, on ? (ULONG)fhelp[i] : 0);
			mui_set(flab[i], MUIA_ShortHelp, on ? (ULONG)fhelp[i] : 0);
		}
	for (i = 0; i < RP_NPAGES; i++)
		for (k = 0; k < 4; k++)
			if (lhelp_obj[i][k])
				mui_set(lhelp_obj[i][k], MUIA_ShortHelp, on ? (ULONG)lhelp[i][k] : 0);
	for (i = 0; i < rp_nlfields; i++)
		if (lfobj[i]) {
			mui_set(lfobj[i], MUIA_ShortHelp, on ? (ULONG)lfhelp[i] : 0);
			mui_set(lflab[i], MUIA_ShortHelp, on ? (ULONG)lfhelp[i] : 0);
		}
	for (i = 0; i < 4; i++)
		if (btn[i])
			mui_set(btn[i], MUIA_ShortHelp, on ? (ULONG)bhelp[i] : 0);
	for (i = 0; i < 2; i++)
		if (ifbtn[i])
			mui_set(ifbtn[i], MUIA_ShortHelp, on ? (ULONG)ifhelp[i + 1] : 0);
	for (i = 0; i < DYN_N; i++)
		if (dyncyc[i])
			mui_set(dyncyc[i], MUIA_ShortHelp, !on ? 0 : i == DYN_ADDR ? (ULONG)dynhelp : (ULONG)lfhelp[dynfield[i]]);
}

/* ---- commands ---- */
static void menu_check(ULONG id, int on)
{
	Object *item = (Object *)mui_do(strip, MUIM_FindUData, id);
	if (item)
		mui_nnset(item, MUIA_Menuitem_Checked, (ULONG)on);
}

/* the model from a file */
static int load_text(const char *path)
{
	struct rp_conf c;
	char *text;
	long len = 0;

	text = rp_read_file(path, &len);
	if (!text) {
		char msg[200];
		rt_snprintf(msg, sizeof(msg), tr("Cannot read %s"), path);
		rp_message(msg);
		return -1;
	}
	memset(&c, 0, sizeof(c));
	if (rp_conf_load(&c, text, (size_t)len) < 0) {
		FreeVec(text);
		return -1;
	}
	FreeVec(text);
	rp_conf_free(&rp_cfg);
	rp_cfg = c;
	return 0;
}

/* an ASL file requester; the path in buf */
static int ask_file(const char *title, int save, char *buf, int len)
{
	struct TagItem t[5];
	struct rt_filereq *fr;
	int ok = 0;

	t[0].ti_Tag = RT_ASLFR_TitleText;
	t[0].ti_Data = (ULONG)tr(title);
	t[1].ti_Tag = RT_ASLFR_DoSaveMode;
	t[1].ti_Data = (ULONG)save;
	t[2].ti_Tag = RT_ASLFR_InitialDrawer;
	t[2].ti_Data = (ULONG)"ENVARC:Riptide";
	t[3].ti_Tag = RT_ASLFR_InitialFile;
	t[3].ti_Data = (ULONG)"riptide.conf";
	t[4].ti_Tag = TAG_DONE;
	t[4].ti_Data = 0;
	fr = MUI_AllocAslRequest(RT_ASL_FileRequest, t);
	if (!fr) {
		rp_message(tr("The file requester needs asl.library."));
		return 0;
	}
	if (MUI_AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
		rt_strlcpy(buf, fr->fr_Drawer ? (const char *)fr->fr_Drawer : "", (size_t)len);
		ok = AddPart((STRPTR)buf, (CONST_STRPTR)fr->fr_File, (ULONG)len) != 0;
	}
	MUI_FreeAslRequest(fr);
	return ok;
}

/* Save, Use or Apply; 1 when the window should close */
static int finish(ULONG id)
{
	if (harvest() < 0 || validate() < 0)
		return 0;
	if (id == ID_APPLY) {
		rp_apply(0);
		return 0;
	}
	if (rp_store(id == ID_SAVE) < 0)
		return 0;
	rp_apply(1);
	return 1;
}

/* the form's field fi shown with a message: what is wrong with it */
static void form_complain(int fi, const char *err)
{
	char msg[300], name[48];

	rp_lflabel(fi, name, sizeof(name));
	rt_snprintf(msg, sizeof(msg), "%s: %s", tr(name), tr(err));
	if (lfobj[fi])
		mui_set(win, MUIA_Window_ActiveObject, (ULONG)lfobj[fi]);
	rp_message(msg);
}

/* the Add, Replace and Delete buttons of a list page, and choosing an
 * entry in its list */
static void list_cmd(int p, int op)
{
	int sel = (int)(LONG)mui_get(lst[p], MUIA_List_Active), r, fi;
	const char *text;
	char entry[RP_LONG], err[160];

	if (op == LIST_SEL) {
		if (sel >= 0 && rp_list_at(p, sel)) {
			if (has_form(p)) {
				rp_lform_get(p, rp_list_at(p, sel), form_names(p), &lv);
				form_show(p);
			} else {
				mui_nnset(lline[p], MUIA_String_Contents, (ULONG)rp_list_at(p, sel));
			}
		}
		return;
	}
	if (has_form(p) && op != LIST_DEL) {
		form_read(p);
		if (rp_lform_put(p, &lv, form_names(p), entry, sizeof(entry), &fi, err, sizeof(err)) < 0) {
			form_complain(fi, err);
			return;
		}
		text = entry;
	} else {
		text = has_form(p) ? "" : (const char *)mui_get(lline[p], MUIA_String_Contents);
	}
	if (op == LIST_ADD)
		r = rp_list_edit(p, -1, text);
	else if (op == LIST_REPL && sel >= 0 && text[0])
		r = rp_list_edit(p, sel, text);
	else if (op == LIST_DEL && sel >= 0)
		r = rp_list_edit(p, sel, NULL);
	else
		return;
	if (r == -2) {
		if (!has_form(p))
			mui_set(win, MUIA_Window_ActiveObject, (ULONG)lline[p]);
		return;
	}
	fill_list(p);
	if (op == LIST_DEL)
		form_clear(p);
	mui_nnset(lst[p], MUIA_List_Active, r >= 0 ? (ULONG)r : (ULONG)MUIV_List_Active_Off);
	if (p == P_VLAN)
		names_show();         /* the addresses may go on the new VLAN */
}

/* New card and Remove card on the Interface page */
static void card_cmd(ULONG id)
{
	char msg[200];
	int k = rp_ifcur(&rp_cfg.cur), n;

	if (harvest_card(1) < 0)
		return;
	if (id == ID_IFADD) {
		k = rp_iface_add(&rp_cfg.cur);
		if (k < 0) {
			rt_snprintf(msg, sizeof(msg), tr("Riptide takes up to %ld cards here."), (long)RP_NIF);
			rp_message(msg);
			return;
		}
		card_choose(k);
		names_show();
		/* the driver is what a new card needs first */
		for (n = 0; n < rp_nfields; n++)
			if ((rp_fields[n].flags & FL_IF) && rp_fields[n].off == offsetof(struct rp_iface, device) && fobj[n])
				mui_set(win, MUIA_Window_ActiveObject, (ULONG)fobj[n]);
		return;
	}
	if (k == 0)
		return;
	n = rp_iface_users(&rp_cfg.cur, k);
	if (n) {
		rt_snprintf(msg, sizeof(msg), tr("Remove %s with its %ld VLANs and addresses?"), rp_cfg.cur.ifc[k].ifname,
		            (long)n);
		if (!rp_request(msg, "Remove|Keep"))
			return;
	}
	rp_iface_delete(&rp_cfg.cur, k);
	refresh_all();
}

/* run a command; 1 when the editor should close */
static int command(ULONG id)
{
	char path[256], msg[300];

	if (id >= ID_FIELD && id < ID_FIELD + (ULONG)rp_nfields) {
		int i = (int)(id - ID_FIELD);
		const struct rp_field *f = &rp_fields[i];
		if (f->type == F_CHECK)
			rp_fset(&rp_cfg.cur, i, mui_get(fobj[i], MUIA_Selected) != 0);
		else if (f->type == F_CYCLE)
			rp_fset(&rp_cfg.cur, i, f->cyv[mui_get(fobj[i], MUIA_Cycle_Active)]);
		update_disabled();
		show_status();
		return 0;
	}
	if (id >= ID_LFIELD && id < ID_LFIELD + (ULONG)rp_nlfields) {
		/* a form field changed: which fields apply, the VLAN's name */
		int p = rp_lfields[id - ID_LFIELD].page;
		form_read(p);
		form_disabled(p);
		return 0;
	}
	if (id >= ID_LIST && id < ID_LIST + RP_NPAGES * 4) {
		list_cmd((int)(id - ID_LIST) / 4, (int)(id - ID_LIST) % 4);
		show_status();
		return 0;
	}
	switch (id) {
	case ID_IFLIST:
	case ID_IFCYC: {
		LONG pos = (LONG)mui_get(id == ID_IFLIST ? iflist : dyncyc[DYN_ADDR],
		                         id == ID_IFLIST ? MUIA_List_Active : MUIA_Cycle_Active);
		if (pos < 0 || card_at((int)pos) == rp_ifcur(&rp_cfg.cur))
			return 0;
		if (harvest_card(1) < 0) {
			cards_show();             /* back to the card shown */
			return 0;
		}
		card_choose(card_at((int)pos));
		return 0;
	}
	case ID_IFADD:
	case ID_IFDEL:
		card_cmd(id);
		show_status();
		return 0;
	case ID_STATUS_CONN:
		status_table_chosen();
		return 0;
	case ID_STATUS_UPD:
		status_update_chosen();
		return 0;
	case ID_FRONT:
		/* another RiptidePrefs was started: this window to the front */
		mui_set(app, MUIA_Application_Iconified, FALSE);
		mui_set(win, MUIA_Window_Open, TRUE);
		mui_do(win, MUIM_Window_ScreenToFront);
		mui_do(win, MUIM_Window_ToFront);
		mui_set(win, MUIA_Window_Activate, TRUE);
		return 0;
	case ID_PAGE:
		page = (int)mui_get(nav, MUIA_Nav_Active);
		/* a card may have been renamed or its driver changed: the list of
		 * cards and the names the forms offer follow */
		harvest_card(0);
		cards_show();
		names_show();
		return 0;
	case ID_SAVE:
	case ID_USE:
	case ID_APPLY:
		if (finish(id))
			return 1;
		show_status();
		return 0;
	case ID_CANCEL:
	case ID_QUIT:
		return 1;
	case ID_OPEN:
		if (ask_file("Open riptide.conf", 0, path, sizeof(path)) && load_text(path) == 0)
			refresh_all();
		show_status();
		return 0;
	case ID_SAVEAS:
		if (harvest() < 0 || validate() < 0)
			return 0;
		if (ask_file("Save riptide.conf as", 1, path, sizeof(path))) {
			size_t len;
			char *text = rp_conf_text(&rp_cfg, &len);
			BPTR f = text ? Open((CONST_STRPTR)path, MODE_NEWFILE) : 0;
			if (!f || Write(f, text, (LONG)len) != (LONG)len) {
				rt_snprintf(msg, sizeof(msg), tr("Cannot write %s"), path);
				rp_message(msg);
			}
			if (f)
				Close(f);
			rp_free(text);
		}
		return 0;
	case ID_ABOUT:
		rt_snprintf(msg, sizeof(msg), "%s %s (%s)\n\n%s", "RiptidePrefs", RIPTIDE_VERSION_STRING, RIPTIDE_DATE,
		            tr("Preferences for the Riptide TCP/IP stack.\nThis window uses MUI; without MUI, "
		               "RiptidePrefs\nopens a GadTools window with the same settings."));
		rp_message(msg);
		return 0;
	case ID_DEFAULTS: {
		struct rp_conf d;
		memset(&d, 0, sizeof(d));
		if (rp_conf_load(&d, "", 0) == 0) {
			rp_cfg.cur = d.cur;
			rp_conf_free(&d);
			refresh_all();
		}
		show_status();
		return 0;
	}
	case ID_LASTSAVED:
		if (load_text(RP_ENVARC_CONF) == 0)
			refresh_all();
		show_status();
		return 0;
	case ID_RESTORE:
		if (rp_load() == 0)
			refresh_all();
		show_status();
		return 0;
	case ID_DARK:
		theme_set_dark(!theme_dark());
		rp_eprefs_set("dark", theme_dark());
		rp_eprefs_save();
		reopen();
		menu_check(ID_DARK, theme_dark());
		show_status();
		return 0;
	case ID_TIPS:
		set_tips(!theme_tips());
		rp_eprefs_set("bubbles", theme_tips());
		rp_eprefs_save();
		menu_check(ID_TIPS, theme_tips());
		return 0;
	}
	return 0;
}

static void free_all(void)
{
	int i, k;

	if (app)
		MUI_DisposeObject(app);
	app = win = NULL;
	status_page_free();
	status_classes_free();
	rp_classes_free();
	for (i = 0; i < NF; i++) {
		rp_free(fhelp[i]);
		fhelp[i] = NULL;
		fobj[i] = flab[i] = NULL;
	}
	for (i = 0; i < RP_NPAGES; i++)
		for (k = 0; k < 4; k++) {
			rp_free(lhelp[i][k]);
			lhelp[i][k] = NULL;
			lhelp_obj[i][k] = NULL;
		}
	for (i = 0; i < RP_NLFIELDS_MAX; i++) {
		rp_free(lfhelp[i]);
		lfhelp[i] = NULL;
		lfobj[i] = lflab[i] = NULL;
	}
	for (i = 0; i < 4; i++) {
		rp_free(bhelp[i]);
		bhelp[i] = NULL;
		btn[i] = NULL;
	}
	for (i = 0; i < 3; i++) {
		rp_free(ifhelp[i]);
		ifhelp[i] = NULL;
	}
	rp_free(dynhelp);
	dynhelp = NULL;
	iflist = ifbtn[0] = ifbtn[1] = NULL;
	for (i = 0; i < DYN_N; i++) {
		dynbox[i] = dyncyc[i] = NULL;
		dynnm[i].n = 0;
	}
}

int mui_run(const struct rp_gui_args *a)
{
	ULONG sigs = 0;
	int r = CL_FAIL;

	MUIMasterBase = OpenLibrary((CONST_STRPTR)"muimaster.library", 19);
	if (!MUIMasterBase) {
		rp_log("no muimaster.library 19");
		return CL_FAIL;
	}
	rp_log("muimaster.library %ld.%ld", (long)MUIMasterBase->lib_Version, (long)MUIMasterBase->lib_Revision);
	{
		struct Screen *s = LockPubScreen((CONST_STRPTR)a->pubscreen);
		if (s) {
			rp_log("screen %ldx%ld, bar %ld, font %ld", (long)s->Width, (long)s->Height, (long)s->BarHeight,
			       (long)s->Font->ta_YSize);
			theme_set_compact(s->Height < 400);
			UnlockPubScreen(NULL, s);
		}
	}
	theme_set_dark(a->theme >= 0 ? a->theme : rp_eprefs_get("dark", 0));
	theme_set_tips(rp_eprefs_get("bubbles", 1));
	page = a->page >= 0 ? a->page : P_STATUS;
	pubname = a->pubscreen;
	if (rp_classes_make() < 0 || status_classes_make() < 0) {
		rp_log("cannot make the classes");
		goto out;
	}
	app = mui_new(MUIC_Application, MUIA_Application_Title, (ULONG)"RiptidePrefs", MUIA_Application_Version,
	              (ULONG)"$VER: RiptidePrefs " RIPTIDE_VERSION_STRING " (" RIPTIDE_DATE ")",
	              MUIA_Application_Copyright, (ULONG)"GPL-2.0", MUIA_Application_Author, (ULONG)"The Riptide authors",
	              MUIA_Application_Description, (ULONG)tr("Preferences for the Riptide TCP/IP stack"),
	              MUIA_Application_Base, (ULONG)"RIPTIDEPREFS", MUIA_Application_UseCommodities, FALSE,
	              MUIA_Application_UseRexx, FALSE, MUIA_Application_SingleTask, TRUE, TAGEND);
	if (!app) {
		/* SingleTask: another MUI RiptidePrefs runs (one that did not
		 * make the port of rp_single_start()); it comes to the front
		 * through MUIA_Application_DoubleStart, this copy just ends */
		if (MUI_Error() == MUIE_SingleTask) {
			rp_log("another MUI RiptidePrefs runs");
			r = CL_OK;
		} else {
			rp_log("cannot make the application");
		}
		goto out;
	}
	if (build_window(a->pubscreen) < 0) {
		rp_log("cannot make the window");
		goto out;
	}
	if (!theme_tips())
		set_tips(0);
	mui_set(win, MUIA_Window_Open, TRUE);
	if (!mui_get(win, MUIA_Window_Open)) {
		rp_log("cannot open the window (MUI error %ld)", (long)MUI_Error());
		goto out;
	}
	rp_log("window open, page %d, %s theme", page, theme_dark() ? "dark" : "light");
	rp_log("window %ldx%ld at %ld,%ld", (long)mui_get(win, MUIA_Window_Width), (long)mui_get(win, MUIA_Window_Height),
	       (long)mui_get(win, MUIA_Window_LeftEdge), (long)mui_get(win, MUIA_Window_TopEdge));
	rp_request_fn = mui_request;
	show_status();
	{
		/* for the DEBUG log: the bubbles of the first setting and page */
		const char *h1 = (const char *)mui_get(fobj[0], MUIA_ShortHelp);
		const char *h2 = (const char *)mui_do(nav, MUIM_CreateShortHelp, (ULONG)_mleft(nav) + 20,
		                                      (ULONG)_mtop(nav) + 2);
		rp_log("bubble field: %s", h1 ? h1 : "(none)");
		rp_log("bubble page: %s", h2 ? h2 : "(none)");
	}
	r = CL_OK;
	if (a->cmd && command((ULONG)a->cmd))
		goto out;
	for (;;) {
		LONG id = (LONG)mui_do(app, MUIM_Application_NewInput, (ULONG)&sigs);
		if (id == MUIV_Application_ReturnID_Quit)
			break;
		if (id)
			rp_log("command %ld", (long)id);
		if (id && command((ULONG)id))
			break;
		if (rp_single_poll())
			command(ID_FRONT);
		if (sigs) {
			sigs = Wait(sigs | SIGBREAKF_CTRL_C | rp_single_sig());
			if (sigs & SIGBREAKF_CTRL_C)
				break;
		}
	}
out:
	rp_request_fn = NULL;
	free_all();
	CloseLibrary(MUIMasterBase);
	MUIMasterBase = NULL;
	return r;
}
