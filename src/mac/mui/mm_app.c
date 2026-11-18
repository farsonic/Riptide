/*
 * Riptide - the Mac's MUI (see mm.h): the Application and Window classes.
 *
 * MUIM_Application_NewInput runs one turn of the Mac event loop (WaitNextEvent, so the program
 * sleeps while nothing happens) and returns the next ReturnID. Clicks and keys go first to the
 * event handlers the custom classes added (MUIM_Window_AddEventHandler) as IntuiMessages
 * (IDCMP_MOUSEBUTTONS, IDCMP_MOUSEMOVE, IDCMP_RAWKEY with Amiga raw key codes), then to the
 * built-in gadgets under the pointer. A Window is a Mac document window whose content is laid
 * out from its root object's sizes; it can be moved, resized and zoomed. The NewMenu table of
 * MUIA_Window_Menustrip becomes the Mac menu bar (the Apple menu first, Edit with the Mac's own
 * Undo, Cut, Copy, Paste and Clear before the editor's items). Help bubbles (MUIA_ShortHelp,
 * MUIM_CreateShortHelp) show after the pointer rests for a second.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"
#include "prefs/mui_theme.h"

Object *mm_app;
struct Screen mm_screen;
static struct TextAttr screen_font;
static ULONG dummy_colormap[4];
void mm_set_app(Object *app);
void mm_root_minmax(Object *root);
void mm_cycle_click(Object *o);
void mm_cycle_key(Object *o);
void mm_list_click(Object *o, int x, int y);
const struct NewMenu *mm_menustrip_nm(Object *strip);
void mm_menustrip_map(Object *strip, int row, int menu, int item);

/* ==================== Application ==================== */
#define RETQ 32
#define MAXIHN 16
#define MAXPUSH 16
struct push { Object *dest; ULONG n; ULONG args[8]; };
struct mm_appd {
	ULONG retq[RETQ];
	int qh, qt;
	struct MUI_InputHandlerNode *ihn[MAXIHN];
	unsigned long ihn_due[MAXIHN];
	struct push push[MAXPUSH];
	int np;
};
ULONG mm_app_datasize(void) { return sizeof(struct mm_appd); }
static struct mm_appd *appd(void) { return mm_app ? INST_DATA(mm_cl_app, mm_app) : NULL; }

void mm_app_returnid(ULONG id)
{
	struct mm_appd *a = appd();
	if (!a || ((a->qt + 1) % RETQ) == a->qh)
		return;
	a->retq[a->qt] = id;
	a->qt = (a->qt + 1) % RETQ;
}

static void mm_event_once(void);
static char app_title[64];
static int app_sleep;

DISPATCHER(mm_app_dispatch)
{
	struct mm_appd *a;
	switch (msg->MethodID) {
	case OM_NEW: {
		const char *title;
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		title = (const char *)mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_Application_Title, 0);
		if (title)
			rt_strlcpy(app_title, title, sizeof app_title);
		mm_app = obj;
		mm_set_app(obj);
		return (ULONG)obj;
	}
	case OM_DISPOSE: {
		Object *c, *next;
		for (c = MMO(obj)->child; c; c = next) {
			next = MMO(c)->next;
			DisposeObject(c);
		}
		MMO(obj)->child = NULL;
		if (mm_app == obj)
			mm_app = NULL;
		break;
	}
	case OM_ADDMEMBER: {
		Object *w = ((struct { ULONG id; Object *o; } *)msg)->o, **pp;
		if (!w)
			return 0;
		MMO(w)->parent = obj;
		MMO(w)->next = NULL;
		for (pp = &MMO(obj)->child; *pp; pp = &MMO(*pp)->next)
			;
		*pp = w;
		return TRUE;
	}
	case OM_REMMEMBER: {
		Object *w = ((struct { ULONG id; Object *o; } *)msg)->o, **pp;
		for (pp = &MMO(obj)->child; *pp; pp = &MMO(*pp)->next)
			if (*pp == w) {
				*pp = MMO(w)->next;
				break;
			}
		MMO(w)->parent = NULL;
		return TRUE;
	}
	case MUIM_Application_ReturnID:
		mm_app_returnid(((ULONG *)msg)[1]);
		return 0;
	case MUIM_Application_PushMethod: {
		ULONG *p = (ULONG *)msg, i;
		a = INST_DATA(cl, obj);
		if (a->np >= MAXPUSH)
			return FALSE;
		a->push[a->np].dest = (Object *)p[1];
		a->push[a->np].n = p[2] & 0xF;
		if (a->push[a->np].n > 8)
			a->push[a->np].n = 8;
		for (i = 0; i < a->push[a->np].n; i++)
			a->push[a->np].args[i] = p[3 + i];
		a->np++;
		return TRUE;
	}
	case MUIM_Application_AddInputHandler: {
		struct MUI_InputHandlerNode *ihn = ((struct { ULONG id; struct MUI_InputHandlerNode *n; } *)msg)->n;
		int i;
		a = INST_DATA(cl, obj);
		for (i = 0; i < MAXIHN; i++)
			if (!a->ihn[i]) {
				a->ihn[i] = ihn;
				a->ihn_due[i] = (unsigned long)TickCount() + (ihn->ihn_millis * 60UL + 999) / 1000;
				return TRUE;
			}
		return FALSE;
	}
	case MUIM_Application_RemInputHandler: {
		struct MUI_InputHandlerNode *ihn = ((struct { ULONG id; struct MUI_InputHandlerNode *n; } *)msg)->n;
		int i;
		a = INST_DATA(cl, obj);
		for (i = 0; i < MAXIHN; i++)
			if (a->ihn[i] == ihn)
				a->ihn[i] = NULL;
		return TRUE;
	}
	case MUIM_Application_NewInput: {
		ULONG *sigs = (ULONG *)((ULONG *)msg)[1];
		int i;
		a = INST_DATA(cl, obj);
		if (sigs)
			*sigs = 0;              /* the waiting happens in WaitNextEvent */
		while (a->np) {
			struct push p = a->push[0];
			for (i = 1; i < a->np; i++)
				a->push[i - 1] = a->push[i];
			a->np--;
			if (p.dest && p.n)
				DoMethodA(p.dest, (Msg)p.args);
		}
		if (a->qh == a->qt)
			mm_event_once();
		if (a->qh != a->qt) {
			ULONG id = a->retq[a->qh];
			a->qh = (a->qh + 1) % RETQ;
			return id;
		}
		return 0;
	}
	case OM_SET: {
		/* the whole program busy: the watch (Guidelines p. 286), the arrow again after */
		ULONG v;
		if (mm_tag_find(((struct opSet *)msg)->ops_AttrList, MUIA_Application_Sleep, &v)) {
			app_sleep += v ? 1 : -1;
			if (app_sleep < 0)
				app_sleep = 0;
			if (app_sleep)
				SetCursor(*(void **)GetCursor(4));
			else
				InitCursor();
		}
		break;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		if (g->opg_AttrID == MUIA_Application_Iconified) {
			*g->opg_Storage = 0;
			return TRUE;
		}
		if (g->opg_AttrID == MUIA_Application_Sleep) {
			*g->opg_Storage = app_sleep != 0;
			return TRUE;
		}
		break;
	}
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== Window ==================== */
struct mm_wind {
	struct mm_win w;
	const char *title;
	Object *strip;
	int open;
	LONG left, top;
	Object *refwin;
	unsigned char ptitle[80];
};
ULONG mm_win_datasize(void) { return sizeof(struct mm_wind); }
#define WD(o) ((struct mm_wind *)INST_DATA(mm_cl_win, o))

static int has_color_qd(void);

static struct mm_win *win_of_mac(void *mw)
{
	Object *c;
	if (!mm_app || !mw)
		return NULL;
	for (c = MMO(mm_app)->child; c; c = MMO(c)->next)
		if (WD(c)->w.macwin == mw)
			return &WD(c)->w;
	return NULL;
}

static void fill_bg(struct mm_win *w, Object *o)
{
	Object *p;
	Rect r;
	ULONG bg = 0;
	for (p = o; p; p = MMO(p)->parent)
		if (MMO(p)->bg) {
			bg = MMO(p)->bg & 0xFFFFFF;
			break;
		}
	if (!p)
		bg = mm_theme_rgb(T_WINDOW);
	mm_box(o, &r);
	mm_fill(w->macwin, &r, bg);
}

void MUI_Redraw(APTR o, ULONG flags)
{
	struct mm_win *w = MMO((Object *)o)->win;
	struct { ULONG id, flags; } m;
	if (!w || !w->macwin || !(MMO((Object *)o)->flags & MMF_SHOWN))
		return;
	SetPort(w->macwin);
	if (flags & MADF_DRAWOBJECT)
		fill_bg(w, (Object *)o);
	m.id = MUIM_Draw;
	m.flags = flags;
	DoMethodA((Object *)o, (Msg)&m);
}

static void draw_grip(struct mm_win *w)
{
	Rect *pr = PORT_RECT(w->macwin);
	int x = pr->right - 1, y = pr->bottom - 1, k;
	for (k = 3; k <= 11; k += 4)
		mm_line(w->macwin, x - k, y, x, y - k, mm_theme_rgb(T_BORDER));
}

void mm_win_redraw(struct mm_win *w)
{
	struct { ULONG id, flags; } m;
	if (!w->macwin)
		return;
	SetPort(w->macwin);
	if (!MMO(w->root)->bg)
		fill_bg(w, w->root);
	m.id = MUIM_Draw;
	m.flags = MADF_DRAWOBJECT;
	DoMethodA(w->root, (Msg)&m);
	if (w->resizable)
		draw_grip(w);
	ValidRect(PORT_RECT(w->macwin));
}

/* the screen the window can use, in global coordinates */
static void usable(Rect *r)
{
	void **gray = LM_GRAYRGN;
	*r = *RGN_BBOX(gray);
	/* the main screen only: the menu bar's screen */
	r->right = (short)(r->left + mm_screen.Width);
	r->top = (short)(LM_MBARHEIGHT + 20);
	r->bottom = (short)(r->top + mm_screen.Height);
}

void mm_win_layout(struct mm_win *w)
{
	struct MUI_MinMax *m;
	int nw, nh;
	ULONG msg;
	if (!w->macwin)
		return;
	mm_gfx_port_moved(w->macwin);
	mm_root_minmax(w->root);
	m = &muiAreaData(w->root)->mad_MinMax;
	nw = w->w < m->MinWidth ? m->MinWidth : w->w;
	nh = w->h < m->MinHeight ? m->MinHeight : w->h;
	if (nw != w->w || nh != w->h) {
		w->w = nw;
		w->h = nh;
		SizeWindow(w->macwin, nw, nh, 1);
	}
	mm_layout(w->root, 0, 0, w->w, w->h);
	/* children added since the last layout are shown now */
	msg = MUIM_Show;
	DoMethodA(w->root, (Msg)&msg);
	w->awin.Width = (WORD)w->w;
	w->awin.Height = (WORD)w->h;
	mm_win_redraw(w);
}

void mm_win_activate_string(struct mm_win *w, Object *s)
{
	if (!w || w->active == s)
		return;
	if (w->active)
		mm_string_activate(w->active, 0);
	w->active = s;
	if (s)
		mm_string_activate(s, 1);
}

/* ---- menus ---- */
#define APPLE_ID 128
#define MAXMENUS 8
static void *menus[MAXMENUS + 1];
static int nmenus;
static Object *menu_strip;
static char app_title[64];
static int menus_done;

/* What each Mac menu item does: the menus are the program's NewMenu strip arranged as the
   Human Interface Guidelines (1992) want them, so the item a Mac menu shows is not the NewMenu row
   it came from. One entry per item. */
enum { MA_NONE, MA_PROGRAM, MA_ABOUT, MA_DESK, MA_CLOSE, MA_QUIT, MA_EDIT };
#define MAXACTS 160
static struct { short menu, item, kind, edit; ULONG udata; } acts[MAXACTS];
static int nacts;
static void act(int menu, int item, int kind, ULONG udata, int edit)
{
	if (nacts < MAXACTS) {
		acts[nacts].menu = (short)menu; acts[nacts].item = (short)item; acts[nacts].kind = (short)kind;
		acts[nacts].udata = udata; acts[nacts].edit = (short)edit;
		nacts++;
	}
}

/* Hierarchical menus (Guidelines p. 79: for lists of attributes): the NM_SUB rows after an item
   become a submenu of it. Their ids are SUB_ID up; the Menu Manager takes 1 to 235 for submenus. */
#define SUB_ID 200
#define MAXSUBS 4
static void *subs[MAXSUBS];
static int nsubs;

void *mm_mac_menu(int id)
{
	if (id >= SUB_ID && id < SUB_ID + nsubs)
		return subs[id - SUB_ID];
	return id >= APPLE_ID && id <= APPLE_ID + MAXMENUS ? menus[id - APPLE_ID] : NULL;
}

/* A program whose window edits something other than a String gadget (Quill's page view) takes
   the Edit menu's commands itself: mm_edit_action gets them when no String is active (Undo 0,
   Cut 2, Copy 3, Paste 4, Clear 5, SystemEdit's numbers), and mm_edit_state says, just before a
   menu is pulled down or a Command key looked up, which of them can be chosen (bit n for command
   n). Without them the Edit menu works as before: the String being typed in, desk accessories. */
int (*mm_edit_action)(int edit);
unsigned (*mm_edit_state)(void);
static int edit_menu_id;                /* the Edit menu's id (menus_install) */
static struct mm_win *front_win(void);

static void edit_menu_state(void)
{
	void *m = mm_mac_menu(edit_menu_id);
	unsigned bits;
	int i;
	if (!m || !mm_edit_state)
		return;
	/* a desk accessory in front, or a String being typed in: all of them, as the Toolbox expects */
	if (!front_win() || front_win()->active)
		bits = 0x3D;
	else
		bits = mm_edit_state();
	for (i = 0; i < 6; i++) {
		if (i == 1)
			continue;               /* the separator */
		if (bits & (1u << i))
			EnableItem(m, i + 1);
		else
			DisableItem(m, i + 1);
	}
}

/* a NewMenu label as the Mac's menu text: tr(), then the guidelines' words (mm_hig.c: the
   ellipsis, Quit, no '_' markers) */
static void label_of(const struct NewMenu *e, char *t, int size)
{
	rt_strlcpy(t, e->nm_Label && e->nm_Label != NM_BARLABEL ? tr((const char *)e->nm_Label) : "", (size_t)size);
	mm_hig_label(t, size);
}
/* the label as the program wrote it, untranslated: what the reserved commands (Quit, Close,
   About, the Command keys of key_ok) are recognised by, so a program in German keeps them */
static void label_en(const struct NewMenu *e, char *t, int size)
{
	rt_strlcpy(t, e->nm_Label && e->nm_Label != NM_BARLABEL ? (const char *)e->nm_Label : "", (size_t)size);
	mm_hig_label(t, size);
}
static int starts(const char *s, const char *w)
{
	while (*w) {
		char a = *s++, b = *w++;
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (b >= 'a' && b <= 'z') b = (char)(b - 32);
		if (a != b)
			return 0;
	}
	return 1;
}
/* The keyboard equivalents Apple reserves (Guidelines p. 128, Table 4-1) keep their meaning: a
   program's command keeps such a key only if it is that command. */
static int key_ok(int k, const char *label)
{
	static const struct { char key; const char *cmd; } res[] = {
		{ 'N', "New" }, { 'O', "Open" }, { 'W', "Close" }, { 'S', "Save" }, { 'P', "Print" }, { 'Q', "Quit" },
		{ 'Z', "Undo" }, { 'X', "Cut" }, { 'C', "Copy" }, { 'V', "Paste" }, { 'A', "Select All" }, { '.', "" },
	};
	unsigned i;
	if (k >= 'a' && k <= 'z')
		k -= 32;
	for (i = 0; i < sizeof res / sizeof res[0]; i++)
		if (res[i].key == k) {
			int n = (int)strlen(res[i].cmd);
			if (!n || !starts(label, res[i].cmd))
				return 0;
			/* "Save As..." is not Save */
			return label[n] == 0 || (label[n] == (char)0xC9 && k != 'S');
		}
	return 1;
}
static int is_file_title(const char *t) { return starts(t, "Project") || starts(t, "File"); }
static int is_about(const char *t) { return starts(t, "About"); }
static int is_quit(const char *t) { return !strcmp(t, "Quit"); }
static int is_close(const char *t) { return starts(t, "Close"); }

static void *new_menu(const char *title)
{
	unsigned char pb[256];
	void *m;
	if (nmenus > MAXMENUS)
		return NULL;
	m = NewMenu(APPLE_ID + nmenus, mac_pstr(pb, title));
	menus[nmenus] = m;
	InsertMenu(m, 0);
	nmenus++;
	return m;
}
static int append(void *m, const char *text, int key, int disabled)
{
	unsigned char pb[256];
	int n;
	AppendMenu(m, mac_pstr(pb, " "));
	n = CountMItems(m);
	SetItem(m, n, mac_pstr(pb, text));
	if (key)
		SetItemCmd(m, n, (unsigned char)key);
	if (disabled)
		DisableItem(m, n);
	return n;
}
static void separator(void *m)
{
	unsigned char pb[8];
	int n = CountMItems(m);
	if (n) {
		/* no separator after a separator */
		unsigned char t[256];
		GetItem(m, n, t);
		if (t[0] == 1 && t[1] == '-')
			return;
	}
	AppendMenu(m, mac_pstr(pb, "(-"));
}
/* the standard Edit menu's six items (p. 101): the text fields and desk accessories use them */
static void edit_items(void *m, int id)
{
	static const struct { const char *t; char k; } e[] = {
		{ "Undo", 'Z' }, { "-", 0 }, { "Cut", 'X' }, { "Copy", 'C' }, { "Paste", 'V' }, { "Clear", 0 } };
	unsigned i;
	for (i = 0; i < 6; i++) {
		if (e[i].t[0] == '-') {
			separator(m);
			continue;
		}
		act(id, append(m, tr(e[i].t), e[i].k, i == 0), MA_EDIT, 0, (int)i);
	}
}
/* an item's submenu: the NM_SUB rows from row first, as a hierarchical menu hung on item n of m
   (its command key the hierarchical mark, its mark the submenu's id); the row after them */
static int sub_items(const struct NewMenu *nm, int first, void *m, int n, const char *title)
{
	unsigned char pb[256];
	char t[256], en[256];
	void *s;
	int j, k, sid = SUB_ID + nsubs;
	if (nsubs >= MAXSUBS || !(s = NewMenu(sid, mac_pstr(pb, title)))) {
		for (j = first; nm[j].nm_Type == NM_SUB; j++)
			;
		return j;
	}
	subs[nsubs++] = s;
	InsertMenu(s, -1);                      /* hierMenu: in the menu list, not the bar */
	SetItemCmd(m, n, 0x1B);                 /* hMenuCmd */
	SetItemMark(m, n, sid);
	for (j = first; nm[j].nm_Type == NM_SUB; j++) {
		if (nm[j].nm_Label == NM_BARLABEL) {
			separator(s);
			continue;
		}
		label_of(&nm[j], t, sizeof t);
		label_en(&nm[j], en, sizeof en);
		k = append(s, t, nm[j].nm_CommKey && nm[j].nm_CommKey[0] && key_ok(nm[j].nm_CommKey[0], en) ? nm[j].nm_CommKey[0] : 0, 0);
		if (nm[j].nm_Flags & CHECKED)
			CheckItem(s, k, 1);
		mm_menustrip_map(menu_strip, j, sid, k);
		act(sid, k, MA_PROGRAM, (ULONG)nm[j].nm_UserData, 0);
	}
	return j;
}

/* the program's items of one NewMenu title, from row first */
static int program_items(const struct NewMenu *nm, int first, void *m, int id, int file, ULONG *quit_udata, int *has_close)
{
	char t[256], en[256];
	int i, n;
	for (i = first; nm[i].nm_Type != NM_END && nm[i].nm_Type != NM_TITLE; i++) {
		if (nm[i].nm_Type != NM_ITEM)
			continue;
		if (nm[i].nm_Label == NM_BARLABEL) {
			separator(m);
			continue;
		}
		label_of(&nm[i], t, sizeof t);
		label_en(&nm[i], en, sizeof en);
		if (is_about(en))
			continue;                       /* the Apple menu has it */
		if (file && is_quit(en)) {
			*quit_udata = (ULONG)nm[i].nm_UserData;
			continue;                       /* last in File, after a separator */
		}
		if (file && is_close(en))
			*has_close = 1;
		if (nm[i + 1].nm_Type == NM_SUB) {
			/* an item with a submenu: no command of its own */
			n = append(m, t, 0, 0);
			i = sub_items(nm, i + 1, m, n, t) - 1;
			continue;
		}
		n = append(m, t, nm[i].nm_CommKey && nm[i].nm_CommKey[0] && key_ok(nm[i].nm_CommKey[0], en) ? nm[i].nm_CommKey[0] : 0, 0);
		if (nm[i].nm_Flags & CHECKED)
			CheckItem(m, n, 1);
		mm_menustrip_map(menu_strip, i, id, n);
		act(id, n, MA_PROGRAM, (ULONG)nm[i].nm_UserData, 0);
	}
	/* a trailing separator goes */
	n = CountMItems(m);
	if (n) {
		GetItem(m, n, (unsigned char *)t);
		if (t[0] == 1 && t[1] == '-')
			DeleteMenuItem(m, n);
	}
	return i;
}

/* Install the menu bar once (the first window that opens): the strip of the program, or none.
   Apple menu: About <program>… (the program's About command, wherever its strip had it), then
   the desk accessories. File: the program's Project or File menu, Close ⌘W, Quit ⌘Q last.
   Edit: the six standard commands, then the program's Edit commands if it has an Edit menu.
   Then the program's other menus. (Guidelines ch. 4: p. 95 the Apple menu, p. 87 File, p. 101
   Edit, p. 128 the reserved keys.) */
static void menus_install(Object *strip)
{
	const struct NewMenu *nm = strip ? mm_menustrip_nm(strip) : NULL;
	unsigned char pb[256];
	char t[256];
	ULONG about_udata = 0, quit_udata = (ULONG)MUIV_Application_ReturnID_Quit;
	void *m;
	int i, has_close = 0, file_row = -1, edit_row = -1, id;
	if (menus_done)
		return;
	menus_done = 1;
	menu_strip = strip;
	nacts = 0;
	for (i = 0; nm && nm[i].nm_Type != NM_END; i++) {
		if (nm[i].nm_Type == NM_TITLE) {
			label_en(&nm[i], t, sizeof t);
			if (file_row < 0 && is_file_title(t))
				file_row = i;
			else if (edit_row < 0 && !strcmp(t, "Edit"))
				edit_row = i;
		} else if (nm[i].nm_Type == NM_ITEM && nm[i].nm_Label != NM_BARLABEL) {
			label_en(&nm[i], t, sizeof t);
			if (is_about(t) && !about_udata)
				about_udata = (ULONG)nm[i].nm_UserData;
		}
	}
	/* Apple */
	pb[0] = 1;
	pb[1] = 0x14;
	menus[0] = NewMenu(APPLE_ID, pb);
	InsertMenu(menus[0], 0);
	nmenus = 1;
	rt_snprintf(t, sizeof t, "About %s\xC9", app_title);
	act(APPLE_ID, append(menus[0], t, 0, 0), MA_ABOUT, about_udata, 0);
	AppendMenu(menus[0], mac_pstr(pb, "(-"));
	AddResMenu(menus[0], MAC_TYPE('D', 'R', 'V', 'R'));
	/* File */
	id = APPLE_ID + nmenus;
	m = new_menu(tr("File"));
	if (m) {
		if (file_row >= 0)
			program_items(nm, file_row + 1, m, id, 1, &quit_udata, &has_close);
		if (!has_close) {
			separator(m);
			act(id, append(m, tr("Close"), 'W', 0), MA_CLOSE, 0, 0);
		}
		separator(m);
		act(id, append(m, tr("Quit"), 'Q', 0), MA_QUIT, quit_udata, 0);
	}
	/* Edit */
	id = APPLE_ID + nmenus;
	m = new_menu(tr("Edit"));
	if (m) {
		edit_menu_id = id;
		edit_items(m, id);
		if (edit_row >= 0) {
			separator(m);
			program_items(nm, edit_row + 1, m, id, 0, &quit_udata, &has_close);
		}
	}
	/* the others, in the program's order */
	for (i = 0; nm && nm[i].nm_Type != NM_END; i++) {
		if (nm[i].nm_Type != NM_TITLE || i == file_row || i == edit_row)
			continue;
		label_of(&nm[i], t, sizeof t);
		id = APPLE_ID + nmenus;
		m = new_menu(t);
		if (!m)
			break;
		program_items(nm, i + 1, m, id, 0, &quit_udata, &has_close);
	}
	DrawMenuBar();
}

static struct mm_win *front_win(void)
{
	return win_of_mac(FrontWindow());
}

/* Close (⌘W): what the front window's close box does */
static void close_front(void)
{
	struct mm_win *w = front_win();
	if (w)
		mm_notify_fire(w->obj, MUIA_Window_CloseRequest, TRUE);
}

static void do_menu(long r)
{
	int id = (int)(r >> 16), item = (int)(r & 0xFFFF), i;
	unsigned char pb[256];
	if (!id) {
		HiliteMenu(0);
		return;
	}
	for (i = 0; i < nacts; i++)
		if (acts[i].menu == id && acts[i].item == item)
			break;
	if (i == nacts) {
		if (id == APPLE_ID) {                   /* a desk accessory */
			GetItem(menus[0], item, pb);
			OpenDeskAcc(pb);
		}
		HiliteMenu(0);
		return;
	}
	switch (acts[i].kind) {
	case MA_ABOUT:
		if (acts[i].udata)
			mm_app_returnid(acts[i].udata);
		else {
			int mac_alert(int icon, const char *text, const char *const *btn, int n, int def, int cancel);
			static const char *const ok[] = { "OK" };
			mac_alert(1, app_title, ok, 1, 0, -1);
		}
		break;
	case MA_PROGRAM:
		if (acts[i].udata)
			mm_app_returnid(acts[i].udata);
		break;
	case MA_QUIT:
		mm_app_returnid(acts[i].udata);
		break;
	case MA_CLOSE:
		close_front();
		break;
	case MA_EDIT:
		/* SystemEdit: Undo 0, Cut 2, Copy 3, Paste 4, Clear 5 (the desk accessory in front, if any) */
		if (!SystemEdit(acts[i].edit)) {
			struct mm_win *w = front_win();
			if (w && w->active && acts[i].edit >= 2)
				mm_string_edit(w->active, acts[i].edit);
			else if (w && !w->active && mm_edit_action)
				mm_edit_action(acts[i].edit);
		}
		break;
	}
	HiliteMenu(0);
}

/* ---- the window ---- */
static void win_close(struct mm_wind *d)
{
	struct mm_win *w = &d->w;
	ULONG m;
	if (!d->open)
		return;
	if (w->active)
		mm_win_activate_string(w, NULL);
	m = MUIM_Hide;
	DoMethodA(w->root, (Msg)&m);
	m = MUIM_Cleanup;
	DoMethodA(w->root, (Msg)&m);
	mm_gfx_port_gone(w->macwin);
	DisposeWindow(w->macwin);
	w->macwin = NULL;
	mm_rp_free(w->rp);
	w->rp = NULL;
	w->neh = 0;
	d->open = 0;
}

static int has_color_qd(void)
{
	long r = 0;
	return mac_gestalt(MAC_TYPE('q', 'd', ' ', ' '), &r) == 0 && r >= 0x100;
}

/* the Mac window and the classes set up in it; procID: zoomDocProc or noGrowDocProc */
static int win_make(Object *obj, struct mm_wind *d, int proc)
{
	struct mm_win *w = &d->w;
	struct { ULONG id; struct MUI_RenderInfo *ri; } s;
	Rect b;
	b.left = 40;
	b.top = 60;
	b.right = 240;
	b.bottom = 200;
	if (has_color_qd())
		w->macwin = NewCWindow(NULL, &b, d->ptitle, 0, proc, (void *)-1, 1, 0);
	else
		w->macwin = NewWindow(NULL, &b, d->ptitle, 0, proc, (void *)-1, 1, 0);
	if (!w->macwin)
		return 0;
	SetPort(w->macwin);
	mm_use_font(0);
	w->obj = obj;
	w->rp = mm_rp_new(w->macwin);
	memset(&w->ri, 0, sizeof(w->ri));
	w->ri.mri_WindowObject = obj;
	w->ri.mri_Screen = &mm_screen;
	w->ri.mri_Pens = mm_mui_pens;
	w->ri.mri_Window = &w->awin;
	w->ri.mri_RastPort = w->rp;
	memset(&w->awin, 0, sizeof(w->awin));
	w->awin.WScreen = &mm_screen;
	w->awin.RPort = w->rp;
	w->awin.Title = (UBYTE *)d->title;
	w->neh = 0;
	w->active = NULL;
	s.id = MUIM_Setup;
	s.ri = &w->ri;
	if (!DoMethodA(w->root, (Msg)&s)) {
		mm_rp_free(w->rp);
		w->rp = NULL;
		DisposeWindow(w->macwin);
		w->macwin = NULL;
		return 0;
	}
	mm_root_minmax(w->root);
	return 1;
}

static int windows_opened;

static int win_open(Object *obj, struct mm_wind *d)
{
	struct mm_win *w = &d->w;
	struct MUI_MinMax *mm;
	Rect u;
	ULONG m;
	if (d->open)
		return 1;
	if (!w->root)
		return 0;
	mac_pstr(d->ptitle, d->title ? tr(d->title) : "");
	if (!win_make(obj, d, zoomDocProc))
		return 0;
	mm = &muiAreaData(w->root)->mad_MinMax;
	/* Guidelines p. 140-145: a window whose size cannot change has no size box and no zoom box.
	   MUI tells only after Setup, so such a window is made again as a plain document window. */
	w->resizable = mm->MaxWidth > mm->MinWidth || mm->MaxHeight > mm->MinHeight;
	if (!w->resizable) {
		m = MUIM_Cleanup;
		DoMethodA(w->root, (Msg)&m);
		mm_gfx_port_gone(w->macwin);
		DisposeWindow(w->macwin);
		w->macwin = NULL;
		mm_rp_free(w->rp);
		w->rp = NULL;
		if (!win_make(obj, d, 4 /* noGrowDocProc */))
			return 0;
		mm = &muiAreaData(w->root)->mad_MinMax;
	}
	usable(&u);
	w->w = mm->DefWidth;
	w->h = mm->DefHeight;
	if (w->w > u.right - u.left - 8)
		w->w = u.right - u.left - 8;
	if (w->h > u.bottom - u.top - 4)
		w->h = u.bottom - u.top - 4;
	if (w->w < mm->MinWidth)
		w->w = mm->MinWidth;
	if (w->h < mm->MinHeight)
		w->h = mm->MinHeight;
	SizeWindow(w->macwin, w->w, w->h, 0);
	{
		/* Where (Guidelines p. 147-149): on the main screen, wholly below the menu bar.
		   A window the program centres: across the screen, a third of the way down (where the
		   Mac puts its alerts and dialogs). Otherwise each new window below and to the right of
		   the last, from the top left. A position the program gives is kept, inside the screen. */
		int x, y, room_h = u.bottom - u.top - w->h;
		if (d->left == MUIV_Window_LeftEdge_Centered || d->left < 0)
			x = d->left == MUIV_Window_LeftEdge_Centered ? (u.left + u.right - w->w) / 2 : u.left + 8 + 20 * (windows_opened % 6);
		else
			x = (int)d->left;
		if (d->top == MUIV_Window_TopEdge_Centered || d->top < 0)
			y = d->top == MUIV_Window_TopEdge_Centered ? u.top + (room_h > 0 ? room_h / 3 : 0) : u.top + 4 + 20 * (windows_opened % 6);
		else
			y = (int)d->top;
		if (x + w->w > u.right - 2)
			x = u.right - 2 - w->w;
		if (y + w->h > u.bottom)
			y = u.bottom - w->h;
		if (x < u.left + 2)
			x = u.left + 2;
		if (y < u.top)
			y = u.top;
		MoveWindow(w->macwin, x, y, 1);
		w->awin.LeftEdge = (WORD)x;
		w->awin.TopEdge = (WORD)y;
		windows_opened++;
	}
	w->awin.Width = (WORD)w->w;
	w->awin.Height = (WORD)w->h;
	mm_layout(w->root, 0, 0, w->w, w->h);
	m = MUIM_Show;
	DoMethodA(w->root, (Msg)&m);
	menus_install(d->strip);
	ShowWindow(w->macwin);
	SelectWindow(w->macwin);
	mm_gfx_port_moved(w->macwin);
	d->open = 1;
	mm_log("window \"%s\" %dx%d (smallest %dx%d)", d->title ? d->title : "", w->w, w->h, mm->MinWidth, mm->MinHeight);
	return 1;
}

static void sort_handlers(struct mm_win *w)
{
	int i, j;
	for (i = 1; i < w->neh; i++)
		for (j = i; j > 0 && w->eh[j]->ehn_Priority > w->eh[j - 1]->ehn_Priority; j--) {
			struct MUI_EventHandlerNode *t = w->eh[j];
			w->eh[j] = w->eh[j - 1];
			w->eh[j - 1] = t;
		}
}

DISPATCHER(mm_win_dispatch)
{
	struct mm_wind *d;
	switch (msg->MethodID) {
	case OM_NEW: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		if (!(obj = (Object *)DoSuperMethodA(cl, obj, msg)))
			return 0;
		d = INST_DATA(cl, obj);
		d->title = (const char *)mm_tag(tl, MUIA_Window_Title, 0);
		d->w.root = (Object *)mm_tag(tl, MUIA_Window_RootObject, 0);
		d->strip = (Object *)mm_tag(tl, MUIA_Window_Menustrip, 0);
		d->left = (LONG)mm_tag(tl, MUIA_Window_LeftEdge, (ULONG)-1);
		d->top = (LONG)mm_tag(tl, MUIA_Window_TopEdge, (ULONG)-1);
		d->refwin = (Object *)mm_tag(tl, MUIA_Window_RefWindow, 0);
		d->w.obj = obj;
		if (d->w.root)
			MMO(d->w.root)->parent = NULL;
		return (ULONG)obj;
	}
	case OM_DISPOSE:
		d = INST_DATA(cl, obj);
		win_close(d);
		if (d->w.root)
			DisposeObject(d->w.root);
		if (d->strip) {
			if (menu_strip == d->strip)
				menu_strip = NULL;
			DisposeObject(d->strip);
		}
		break;
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList;
		ULONG v;
		d = INST_DATA(cl, obj);
		if (mm_tag_find(tl, MUIA_Window_Open, &v)) {
			if (v)
				win_open(obj, d);
			else
				win_close(d);
		}
		if (mm_tag_find(tl, MUIA_Window_Sleep, &v)) {
			d->w.sleep = v != 0;
			if (d->w.sleep)
				SetCursor(*(void **)GetCursor(4));      /* the watch */
			else
				InitCursor();
		}
		if (mm_tag_find(tl, MUIA_Window_ActiveObject, &v) && d->open) {
			Object *o = (Object *)v;
			if (o && (MMO(o)->flags & MMF_STRING))
				mm_win_activate_string(&d->w, o);
		}
		if (mm_tag_find(tl, MUIA_Window_Title, &v)) {
			d->title = (const char *)v;
			if (d->w.macwin)
				SetWTitle(d->w.macwin, mac_pstr(d->ptitle, tr(d->title)));
		}
		if (mm_tag_find(tl, MUIA_Window_Activate, &v) && v && d->w.macwin)
			SelectWindow(d->w.macwin);
		break;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		d = INST_DATA(cl, obj);
		switch (g->opg_AttrID) {
		case MUIA_Window_Open: *g->opg_Storage = (ULONG)d->open; return TRUE;
		case MUIA_Window_Window: *g->opg_Storage = d->open ? (ULONG)&d->w.awin : 0; return TRUE;
		case MUIA_Window_Screen: *g->opg_Storage = (ULONG)&mm_screen; return TRUE;
		case MUIA_Window_LeftEdge: *g->opg_Storage = (ULONG)(LONG)d->w.awin.LeftEdge; return TRUE;
		case MUIA_Window_TopEdge: *g->opg_Storage = (ULONG)(LONG)d->w.awin.TopEdge; return TRUE;
		case MUIA_Window_Width: *g->opg_Storage = (ULONG)d->w.w; return TRUE;
		case MUIA_Window_Height: *g->opg_Storage = (ULONG)d->w.h; return TRUE;
		case MUIA_Window_Sleep: *g->opg_Storage = (ULONG)d->w.sleep; return TRUE;
		case MUIA_Window_ActiveObject: *g->opg_Storage = (ULONG)d->w.active; return TRUE;
		case MUIA_Window_RootObject: *g->opg_Storage = (ULONG)d->w.root; return TRUE;
		}
		break;
	}
	case MUIM_Window_AddEventHandler: {
		struct MUI_EventHandlerNode *n = ((struct { ULONG id; struct MUI_EventHandlerNode *n; } *)msg)->n;
		int i;
		d = INST_DATA(cl, obj);
		for (i = 0; i < d->w.neh; i++)
			if (d->w.eh[i] == n)
				return TRUE;
		if (d->w.neh < MM_MAXHANDLERS) {
			d->w.eh[d->w.neh++] = n;
			sort_handlers(&d->w);
		}
		return TRUE;
	}
	case MUIM_Window_RemEventHandler: {
		struct MUI_EventHandlerNode *n = ((struct { ULONG id; struct MUI_EventHandlerNode *n; } *)msg)->n;
		int i, j;
		d = INST_DATA(cl, obj);
		for (i = 0; i < d->w.neh; i++)
			if (d->w.eh[i] == n) {
				for (j = i + 1; j < d->w.neh; j++)
					d->w.eh[j - 1] = d->w.eh[j];
				d->w.neh--;
				break;
			}
		return TRUE;
	}
	case MUIM_Window_ToFront:
	case MUIM_Window_ScreenToFront:
		d = INST_DATA(cl, obj);
		if (d->w.macwin)
			SelectWindow(d->w.macwin);
		return TRUE;
	}
	return DoSuperMethodA(cl, obj, msg);
}

/* ==================== events ==================== */
/* a Mac key as an Amiga raw key code (0xFF: none) */
static UWORD raw_key(int c)
{
	static const UBYTE letters[26] = { 0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
	                                   0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31 };
	if (c >= 'a' && c <= 'z')
		return letters[c - 'a'];
	if (c >= 'A' && c <= 'Z')
		return letters[c - 'A'];
	if (c >= '1' && c <= '9')
		return (UWORD)(c - '1' + 1);
	switch (c) {
	case '0': return 0x0A;
	case ' ': return 0x40;
	case 8: return 0x41;
	case 9: return 0x42;
	case 3: return 0x43;
	case '\r': return 0x44;
	case 0x1B: return 0x45;
	case 0x7F: return 0x46;
	case 0x1E: return 0x4C;
	case 0x1F: return 0x4D;
	case 0x1D: return 0x4E;
	case 0x1C: return 0x4F;
	case 0x05: return 0x5F;     /* help */
	}
	return 0xFF;
}

/* the Mac's modifier keys as Amiga qualifiers: Option is Alt (as on a PC keyboard by position),
   Command is the Amiga key, Control is Control */
static UWORD qualifier(short mods)
{
	UWORD q = 0;
	if (mods & shiftKey)
		q |= IEQUALIFIER_LSHIFT;
	if (mods & optionKey)
		q |= IEQUALIFIER_LALT;
	if (mods & cmdKey)
		q |= IEQUALIFIER_LCOMMAND;
	if (mods & controlKey)
		q |= IEQUALIFIER_CONTROL;
	return q;
}

/* the handlers that want class: 1 when one ate the message */
static int handlers(struct mm_win *w, ULONG class, UWORD code, int x, int y, short mods, LONG muikey)
{
	struct MUI_EventHandlerNode *list[MM_MAXHANDLERS];
	struct IntuiMessage im;
	struct { ULONG id; struct IntuiMessage *im; LONG muikey; } m;
	int n = w->neh, i, j;
	memcpy(list, w->eh, (unsigned long)n * sizeof(list[0]));
	memset(&im, 0, sizeof(im));
	im.Class = class;
	im.Code = code;
	im.Qualifier = qualifier(mods);
	im.MouseX = (WORD)x;
	im.MouseY = (WORD)y;
	im.IDCMPWindow = &w->awin;
	m.id = MUIM_HandleEvent;
	m.im = &im;
	m.muikey = muikey;
	for (i = 0; i < n; i++) {
		ULONG r;
		struct MUI_EventHandlerNode *e = list[i];
		/* still there (an earlier handler may have removed it) */
		for (j = 0; j < w->neh && w->eh[j] != e; j++)
			;
		if (j == w->neh || !(e->ehn_Events & class))
			continue;
		r = e->ehn_Class ? CoerceMethodA(e->ehn_Class, e->ehn_Object, (Msg)&m) : DoMethodA(e->ehn_Object, (Msg)&m);
		if (r & MUI_EventHandlerRC_Eat)
			return 1;
		if (!w->macwin)
			return 1;
	}
	return 0;
}

/* help bubbles */
static void *bubble;
static Object *bubble_obj;
static const char *bubble_text;
static unsigned long still_since;
static Point last_mouse = { -1, -1 };

static void bubble_hide(void)
{
	if (bubble) {
		mm_gfx_port_gone(bubble);
		DisposeWindow(bubble);
		bubble = NULL;
	}
	if (bubble_obj) {
		struct { ULONG id; const char *t; } m = { MUIM_DeleteShortHelp, bubble_text };
		DoMethodA(bubble_obj, (Msg)&m);
		bubble_obj = NULL;
	}
}

static void bubble_draw(void)
{
	const char *s = bubble_text;
	Rect r = *PORT_RECT(bubble);
	int y = 3 + mm_font.tf_Baseline, b = 0;
	SetPort(bubble);
	mm_fill(bubble, &r, MM_RGB(0xFF, 0xFF, 0xE1));
	mm_frame(bubble, &r, MM_RGB(0, 0, 0));
	while (*s) {
		char line[120];
		int n = 0;
		while (*s && *s != '\n' && n < (int)sizeof line - 1) {
			if (*s == 27 && s[1]) {
				b = s[1] == 'b';
				s += 2;
				continue;
			}
			line[n++] = *s++;
		}
		if (*s == '\n')
			s++;
		mm_text(bubble, 5, y, line, n, MM_RGB(0, 0, 0), b);
		y += mm_font.tf_YSize;
	}
}

static void bubble_show(struct mm_win *w, Point p)
{
	Object *o = mm_hit(w->root, p.h, p.v, 0);
	const char *t = NULL;
	if (!theme_tips())
		return;
	while (o) {
		struct { ULONG id; LONG x, y; } m = { MUIM_CreateShortHelp, p.h, p.v };
		t = (const char *)DoMethodA(o, (Msg)&m);
		if (t)
			break;
		o = MMO(o)->parent;
	}
	if (!o || !t)
		return;
	{
		/* its size: the widest line, the number of lines */
		const char *s = t;
		int lines = 0, wmax = 0, b = 0;
		Point g = p;
		Rect r;
		unsigned char e = 0;
		while (*s) {
			char line[120];
			int n = 0, lw;
			while (*s && *s != '\n' && n < (int)sizeof line - 1) {
				if (*s == 27 && s[1]) {
					b = s[1] == 'b';
					s += 2;
					continue;
				}
				line[n++] = *s++;
			}
			if (*s == '\n')
				s++;
			lw = mm_textw(line, n, b);
			if (lw > wmax)
				wmax = lw;
			lines++;
		}
		SetPort(w->macwin);
		LocalToGlobal(&g);
		r.left = (short)(g.h + 10);
		r.top = (short)(g.v + 18);
		r.right = (short)(r.left + wmax + 10);
		r.bottom = (short)(r.top + lines * mm_font.tf_YSize + 6);
		if (r.right > mm_screen.Width - 2) {
			int dx = r.right - (mm_screen.Width - 2);
			r.left -= dx;
			r.right -= dx;
		}
		if (r.bottom > mm_screen.Height - 2) {
			int dy = r.bottom - (g.v - 4);
			r.top -= dy;
			r.bottom -= dy;
		}
		bubble = has_color_qd() ? NewCWindow(NULL, &r, &e, 1, plainDBox, (void *)-1, 0, 0) :
		                          NewWindow(NULL, &r, &e, 1, plainDBox, (void *)-1, 0, 0);
		if (!bubble)
			return;
		mm_gfx_port_moved(bubble);
		bubble_obj = o;
		bubble_text = t;
		bubble_draw();
		SetPort(w->macwin);
	}
}

/* the pointer moved in the front window */
static void mouse_moved(struct mm_win *w, Point p, short mods)
{
	/* Guidelines p. 286: the I-beam over text that can be edited, the arrow elsewhere */
	static int ibeam = -1;
	Object *o = mm_hit(w->root, p.h, p.v, MMF_INPUT);
	int want = o && (MMO(o)->flags & MMF_STRING) && !MMO(o)->disabled;
	if (want != ibeam) {
		if (want)
			SetCursor(*(void **)GetCursor(1));          /* iBeamCursor */
		else
			InitCursor();
		ibeam = want;
	}
	handlers(w, IDCMP_MOUSEMOVE, 0, p.h, p.v, mods, -1);
}

static void content_click(struct mm_win *w, EventRecord *ev)
{
	Point p = ev->where;
	Object *o;
	SetPort(w->macwin);
	GlobalToLocal(&p);
	if (handlers(w, IDCMP_MOUSEBUTTONS, 0x68 /* SELECTDOWN */, p.h, p.v, ev->modifiers, -1))
		return;
	o = mm_hit(w->root, p.h, p.v, MMF_INPUT);
	if (!o) {
		return;
	}
	if (MMO(o)->flags & MMF_STRING) {
		mm_string_click(o, ev, p);
		return;
	}
	if (w->active)
		mm_win_activate_string(w, NULL);
	if (OCLASS(o) == mm_cl_cycle)
		mm_cycle_click(o);
	else if (OCLASS(o) == mm_cl_list)
		mm_list_click(o, p.h, p.v);
	else
		mm_area_click(o, p.h, p.v);
}

/* the strings of the cycle chain in window order */
static void collect_strings(Object *o, Object **out, int *n, int max)
{
	Object *c;
	if (!(MMO(o)->flags & MMF_SHOWN))
		return;
	if ((MMO(o)->flags & MMF_STRING) && !MMO(o)->disabled && *n < max)
		out[(*n)++] = o;
	if (MMO(o)->flags & MMF_GROUP)
		for (c = MMO(o)->child; c; c = MMO(c)->next)
			collect_strings(c, out, n, max);
}

static void next_string(struct mm_win *w, int back)
{
	Object *s[96];
	int n = 0, i, k = -1;
	collect_strings(w->root, s, &n, 96);
	if (!n)
		return;
	for (i = 0; i < n; i++)
		if (s[i] == w->active)
			k = i;
	k = k < 0 ? 0 : (k + (back ? n - 1 : 1)) % n;
	mm_win_activate_string(w, s[k]);
}

/* the shown object whose control character is c */
static Object *find_cchar(Object *o, int c)
{
	Object *ch;
	if (!(MMO(o)->flags & MMF_SHOWN))
		return NULL;
	if (MMO(o)->cchar == c && !MMO(o)->disabled)
		return o;
	if (MMO(o)->flags & MMF_GROUP)
		for (ch = MMO(o)->child; ch; ch = MMO(ch)->next) {
			Object *r = find_cchar(ch, c);
			if (r)
				return r;
		}
	return NULL;
}

static void key_down(struct mm_win *w, EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	UWORD raw = raw_key(c);
	LONG muikey = c == 0x1E ? MUIKEY_UP : c == 0x1F ? MUIKEY_DOWN : c == '\r' ? MUIKEY_PRESS : -1;
	if (w->active) {
		if (c == 9) {
			next_string(w, (ev->modifiers & shiftKey) != 0);
			return;
		}
		if (c == 0x1B) {
			mm_win_activate_string(w, NULL);
			return;
		}
		mm_string_key(w->active, ev);
		return;
	}
	/* A handler that asks for IDCMP_VANILLAKEY gets the character typed first, as on the Amiga,
	   in Code; on the Mac it is the Mac Roman character (an editor such as Quill's page view
	   turns it into Unicode itself), arrows and the other function keys as their Mac codes
	   (0x1C-0x1F, 1 home, 4 end, 11 and 12 page up and down, 8 delete, 0x7F forward delete). */
	if (handlers(w, IDCMP_VANILLAKEY, (UWORD)c, 0, 0, ev->modifiers, muikey))
		return;
	if (raw != 0xFF && handlers(w, IDCMP_RAWKEY, raw, 0, 0, ev->modifiers, muikey))
		return;
	if (c == 9) {
		next_string(w, (ev->modifiers & shiftKey) != 0);
		return;
	}
	if (c >= 'A' && c <= 'Z')
		c += 32;
	if (c > ' ' && c < 127) {
		Object *o = find_cchar(w->root, c);
		if (!o)
			return;
		if (MMO(o)->flags & MMF_STRING)
			mm_win_activate_string(w, o);
		else if (OCLASS(o) == mm_cl_cycle)
			mm_cycle_key(o);
		else
			mm_area_key(o);
	}
}

static void timers(void)
{
	struct mm_appd *a = appd();
	unsigned long now = (unsigned long)TickCount();
	int i;
	if (!a)
		return;
	for (i = 0; i < MAXIHN; i++) {
		struct MUI_InputHandlerNode *n = a->ihn[i];
		if (n && (n->ihn_Flags & MUIIHNF_TIMER) && (long)(now - a->ihn_due[i]) >= 0) {
			ULONG m = n->ihn_Method;
			a->ihn_due[i] = now + (n->ihn_millis * 60UL + 999) / 1000;
			DoMethodA(n->ihn_Object, (Msg)&m);
		}
	}
}

static long sleep_ticks(void)
{
	struct mm_appd *a = appd();
	unsigned long now = (unsigned long)TickCount();
	long s = 30;
	int i;
	if (a)
		for (i = 0; i < MAXIHN; i++)
			if (a->ihn[i]) {
				long d = (long)(a->ihn_due[i] - now);
				if (d < s)
					s = d;
			}
	if (bubble_obj == NULL && still_since)
		s = s > 10 ? 10 : s;
	return s < 1 ? 1 : s;
}

static int in_front = 1;

/* an update event for one of the windows: also called by the alerts while they are up
   (src/mac/app/mac_alert.c), so the windows behind them are drawn */
void mac_alert_update(void *event)
{
	EventRecord *ev = event;
	struct mm_win *w;
	void *save;
	GetPort(&save);
	if ((void *)ev->message == bubble) {
		BeginUpdate(bubble);
		bubble_draw();
		EndUpdate(bubble);
		SetPort(save);
		return;
	}
	w = win_of_mac((void *)ev->message);
	if (w) {
		mm_gfx_port_moved(w->macwin);   /* the Monitors control panel may have changed the depth */
		BeginUpdate(w->macwin);
		mm_win_redraw(w);
		EndUpdate(w->macwin);
	}
	SetPort(save);
}

static void mm_event_once(void)
{
	EventRecord ev;
	struct mm_win *w;
	void *rgn = NewRgn();
	Rect mr;
	Point gm;
	GetMouse(&gm);
	{
		void *port;
		GetPort(&port);
		LocalToGlobal(&gm);
		(void)port;
	}
	mr.left = gm.h;
	mr.top = gm.v;
	mr.right = (short)(gm.h + 1);
	mr.bottom = (short)(gm.v + 1);
	RectRgn(rgn, &mr);
	if (!WaitNextEvent(everyEvent, &ev, sleep_ticks(), rgn))
		ev.what = nullEvent;
	DisposeRgn(rgn);
	if (ev.what != nullEvent && ev.what != updateEvt && !(ev.what == osEvt && ((ev.message >> 24) & 0xFF) == mouseMovedMessage))
		bubble_hide();
	switch (ev.what) {
	case mouseDown: {
		void *mw = NULL;
		int part = FindWindow(pt_long(ev.where), &mw);
		w = win_of_mac(mw);
		still_since = 0;
		switch (part) {
		case inMenuBar:
			edit_menu_state();
			do_menu(MenuSelect(pt_long(ev.where)));
			break;
		case inSysWindow:
			SystemClick(&ev, mw);
			break;
		case inDrag:
			if (w && FrontWindow() != mw && w->sleep)
				break;
			DragWindow(mw, pt_long(ev.where), RGN_BBOX(LM_GRAYRGN));
			if (w) {
				Point o = { 0, 0 };
				SetPort(mw);
				LocalToGlobal(&o);
				w->awin.LeftEdge = o.h;
				w->awin.TopEdge = o.v;
				/* onto another screen: colour or one bit there, and drawn anew */
				{
					int was = mm_mono_port(mw);
					mm_gfx_port_moved(mw);
					if (was != mm_mono_port(mw))
						InvalRect(PORT_RECT(mw));
				}
			}
			break;
		case inGoAway:
			if (w && TrackGoAway(mw, pt_long(ev.where)))
				mm_notify_fire(w->obj, MUIA_Window_CloseRequest, TRUE);
			break;
		case inGrow:
			if (w && !w->sleep) {
				struct MUI_MinMax *m = &muiAreaData(w->root)->mad_MinMax;
				Rect lim;
				long sz;
				lim.top = m->MinHeight;
				lim.left = m->MinWidth;
				lim.bottom = m->MaxHeight < 4000 ? m->MaxHeight : 4000;
				lim.right = m->MaxWidth < 4000 ? m->MaxWidth : 4000;
				sz = GrowWindow(mw, pt_long(ev.where), &lim);
				if (sz) {
					w->w = (short)(sz & 0xFFFF);
					w->h = (short)(sz >> 16);
					SizeWindow(mw, w->w, w->h, 1);
					mm_win_layout(w);
				}
			}
			break;
		case inZoomIn:
		case inZoomOut:
			if (w && TrackBox(mw, pt_long(ev.where), part)) {
				SetPort(mw);
				ZoomWindow(mw, part, 0);
				w->w = PORT_RECT(mw)->right - PORT_RECT(mw)->left;
				w->h = PORT_RECT(mw)->bottom - PORT_RECT(mw)->top;
				mm_win_layout(w);
			}
			break;
		case inContent:
			if (FrontWindow() != mw) {
				SelectWindow(mw);
				break;
			}
			if (w && !w->sleep)
				content_click(w, &ev);
			break;
		}
		break;
	}
	case mouseUp:
		w = front_win();
		if (w && !w->sleep) {
			Point p = ev.where;
			SetPort(w->macwin);
			GlobalToLocal(&p);
			handlers(w, IDCMP_MOUSEBUTTONS, 0xE8 /* SELECTUP */, p.h, p.v, ev.modifiers, -1);
		}
		break;
	case keyDown:
	case autoKey:
		if (ev.modifiers & cmdKey) {
			if (ev.what == keyDown) {
				edit_menu_state();
				do_menu(MenuKey((int)(ev.message & charCodeMask)));
			}
			break;
		}
		w = front_win();
		if (w && !w->sleep)
			key_down(w, &ev);
		break;
	case updateEvt:
		mac_alert_update(&ev);
		break;
	case activateEvt:
		/* the string being typed in shows its caret only in the front window */
		w = win_of_mac((void *)ev.message);
		if (w && w->active)
			mm_string_activate(w->active, (ev.modifiers & activeFlag) != 0);
		break;
	case osEvt:
		if (((ev.message >> 24) & 0xFF) == suspendResumeMessage) {
			in_front = (ev.message & resumeFlag) != 0;
			w = front_win();
			if (w && w->active)
				mm_string_activate(w->active, in_front);
		}
		break;
	}
	/* the pointer, the caret, the timers, the bubbles */
	w = front_win();
	if (w && in_front) {
		Point p;
		SetPort(w->macwin);
		GetMouse(&p);
		if (p.h != last_mouse.h || p.v != last_mouse.v) {
			last_mouse = p;
			bubble_hide();
			still_since = (unsigned long)TickCount();
			if (!w->sleep)
				mouse_moved(w, p, ev.modifiers);
		} else if (still_since && !bubble && !Button() && (unsigned long)TickCount() - still_since > 50 &&
		           p.h >= 0 && p.v >= 0 && p.h < w->w && p.v < w->h && !w->sleep) {
			still_since = 0;
			bubble_show(w, p);
		}
		if (w->active)
			mm_string_idle(w->active);
	}
	timers();
}

/* ---- the screen the classes see ---- */
void mm_screen_init(void *qd_thePort_addr)
{
	Rect *sb = LM_SCREENBITS_BOUNDS(qd_thePort_addr);
	memset(&mm_screen, 0, sizeof(mm_screen));
	/* the height a window can have: the editor fits its page list to the screen's height less an
	   Amiga window's borders, so it gets the Mac screen less the menu bar and a title bar */
	mm_screen.Width = (WORD)(sb->right - sb->left);
	mm_screen.Height = (WORD)(sb->bottom - sb->top - LM_MBARHEIGHT - 20);
	/* The editor takes its compact layout under 400 lines (mui_run: theme_set_compact); its full
	   layout is made for an Amiga font of 8 or 9 pixels, and with the Mac's (Geneva 10) it needs
	   about 460 lines. So a Mac screen under 560 usable lines (every classic size up to 640 x 480)
	   reports 399 and gets the compact layout. */
	if (mm_screen.Height >= 400 && mm_screen.Height < 560)
		mm_screen.Height = 399;
	mm_screen.BarHeight = (BYTE)LM_MBARHEIGHT;
	screen_font.ta_YSize = (UWORD)mm_font.tf_YSize;
	screen_font.ta_Name = (STRPTR)"Geneva";
	mm_screen.Font = &screen_font;
	mm_screen.ViewPort.ColorMap = (struct ColorMap *)dummy_colormap;
}
