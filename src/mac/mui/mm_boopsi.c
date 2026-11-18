/*
 * Riptide - the Mac's MUI (see mm.h): classes and objects as BOOPSI makes them, the Notify class
 * (notifications, MUIM_Set, MUIM_FindUData) and muimaster.library's object calls.
 *
 * DoMethodA, DoSuperMethodA and CoerceMethodA are the editor's own (src/amiga/prefs/mui_util.c,
 * GCC branch): they call a class's hook through rp_hook_call (mui_thunks.S) with the class in
 * a0, the object in a2 and the message in a1, which is how BOOPSI calls a dispatcher on the
 * Amiga; the built-in classes here are entered the same way (mm_thunks.S).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mm.h"

int mm_error;
static struct MUI_GlobalInfo mm_gi;

/* ---- tag lists ---- */
int mm_tag_find(const struct TagItem *t, ULONG tag, ULONG *v)
{
	for (; t; t++) {
		switch (t->ti_Tag) {
		case TAG_DONE:
			return 0;
		case TAG_IGNORE:
			continue;
		case TAG_MORE:
			t = (const struct TagItem *)t->ti_Data - 1;
			continue;
		case TAG_SKIP:
			t += t->ti_Data;
			continue;
		}
		if (t->ti_Tag == tag) {
			if (v)
				*v = t->ti_Data;
			return 1;
		}
	}
	return 0;
}

ULONG mm_tag(const struct TagItem *t, ULONG tag, ULONG def)
{
	ULONG v;
	return mm_tag_find(t, tag, &v) ? v : def;
}

/* the next real tag of a list, NULL at the end */
static const struct TagItem *next_tag(const struct TagItem **tp)
{
	const struct TagItem *t = *tp;
	for (; t; t++) {
		switch (t->ti_Tag) {
		case TAG_DONE:
			*tp = NULL;
			return NULL;
		case TAG_IGNORE:
			continue;
		case TAG_MORE:
			t = (const struct TagItem *)t->ti_Data - 1;
			continue;
		case TAG_SKIP:
			t += t->ti_Data;
			continue;
		}
		*tp = t + 1;
		return t;
	}
	*tp = NULL;
	return NULL;
}

/* ---- classes ---- */
struct IClass *mm_make_class(struct IClass *super, ULONG datasize, APTR thunk)
{
	struct IClass *cl = mm_alloc(sizeof(*cl));
	if (!cl)
		return NULL;
	cl->cl_Dispatcher.h_Entry = (ULONG (*)(void))thunk;
	cl->cl_Super = super;
	cl->cl_InstOffset = super ? (UWORD)(super->cl_InstOffset + super->cl_InstSize) : 0;
	cl->cl_InstSize = (UWORD)datasize;
	if (super)
		super->cl_SubclassCount++;
	return cl;
}

struct MUI_CustomClass *MUI_CreateCustomClass(struct Library *base, CONST_STRPTR supername,
                                              struct MUI_CustomClass *supermcc, LONG datasize, APTR dispatcher)
{
	struct IClass *super = NULL;
	struct MUI_CustomClass *mcc;
	(void)base;
	if (supermcc)
		super = supermcc->mcc_Class;
	else if (supername) {
		Object *dummy = NULL;
		(void)dummy;
		if (!strcmp((const char *)supername, MUIC_Area))
			super = mm_cl_area;
		else if (!strcmp((const char *)supername, MUIC_Group))
			super = mm_cl_group;
		else if (!strcmp((const char *)supername, MUIC_Window))
			super = mm_cl_win;
		else if (!strcmp((const char *)supername, MUIC_List))
			super = mm_cl_list;
		else
			super = mm_cl_notify;
	}
	if (!super)
		return NULL;
	mcc = mm_alloc(sizeof(*mcc));
	if (!mcc)
		return NULL;
	mcc->mcc_Super = super;
	mcc->mcc_Class = mm_make_class(super, (ULONG)datasize, dispatcher);
	if (!mcc->mcc_Class) {
		mm_free(mcc);
		return NULL;
	}
	return mcc;
}

LONG MUI_DeleteCustomClass(struct MUI_CustomClass *mcc)
{
	if (!mcc)
		return FALSE;
	mm_free(mcc->mcc_Class);
	mm_free(mcc);
	return TRUE;
}

/* ---- objects ---- */
APTR NewObjectA(struct IClass *cl, CONST_STRPTR id, const struct TagItem *tags)
{
	struct opSet m;
	(void)id;
	if (!cl)
		return NULL;
	m.MethodID = OM_NEW;
	m.ops_AttrList = (struct TagItem *)tags;
	m.ops_GInfo = NULL;
	return (APTR)CoerceMethodA(cl, (Object *)cl, (Msg)&m);
}

void DisposeObject(APTR obj)
{
	ULONG m = OM_DISPOSE;
	if (obj)
		DoMethodA((Object *)obj, (Msg)&m);
}

ULONG SetAttrsA(APTR obj, const struct TagItem *tags)
{
	struct opSet m;
	if (!obj)
		return 0;
	m.MethodID = OM_SET;
	m.ops_AttrList = (struct TagItem *)tags;
	m.ops_GInfo = NULL;
	return DoMethodA((Object *)obj, (Msg)&m);
}

ULONG GetAttr(ULONG attr, APTR obj, ULONG *storage)
{
	struct opGet m;
	if (!obj)
		return 0;
	m.MethodID = OM_GET;
	m.opg_AttrID = attr;
	m.opg_Storage = storage;
	return DoMethodA((Object *)obj, (Msg)&m);
}

ULONG mm_setattrs(Object *obj, ULONG tag, ...)
{
	return SetAttrsA(obj, (const struct TagItem *)&tag);
}

void mm_set_quiet(Object *obj, ULONG attr, ULONG v)
{
	struct TagItem t[3];
	t[0].ti_Tag = MUIA_NoNotify;
	t[0].ti_Data = TRUE;
	t[1].ti_Tag = attr;
	t[1].ti_Data = v;
	t[2].ti_Tag = TAG_DONE;
	SetAttrsA(obj, t);
}

APTR MUI_NewObjectA(CONST_STRPTR name, const struct TagItem *tags)
{
	const char *n = (const char *)name;
	struct IClass *cl = NULL;
	if (!strcmp(n, MUIC_Application))
		cl = mm_cl_app;
	else if (!strcmp(n, MUIC_Window))
		cl = mm_cl_win;
	else if (!strcmp(n, MUIC_Group))
		cl = mm_cl_group;
	else if (!strcmp(n, MUIC_Area))
		cl = mm_cl_area;
	else if (!strcmp(n, MUIC_Rectangle))
		cl = mm_cl_rect;
	else if (!strcmp(n, MUIC_Image))
		cl = mm_cl_image;
	else if (!strcmp(n, MUIC_Text))
		cl = mm_cl_text;
	else if (!strcmp(n, MUIC_String))
		cl = mm_cl_string;
	else if (!strcmp(n, MUIC_Cycle))
		cl = mm_cl_cycle;
	else if (!strcmp(n, MUIC_List))
		cl = mm_cl_list;
	else if (!strcmp(n, MUIC_Listview))
		cl = mm_cl_listview;
	if (!cl) {
		mm_log("MUI_NewObject: no class %s", n);
		return NULL;
	}
	return NewObjectA(cl, NULL, tags);
}

void MUI_DisposeObject(APTR obj)
{
	DisposeObject(obj);
}

LONG MUI_Error(void)
{
	return mm_error;
}

/* the key of a label: the letter after '_' in lower case */
static ULONG label_key(const char *l)
{
	const char *u = l ? strchr(l, '_') : NULL;
	char c = u && u[1] ? u[1] : 0;
	return (ULONG)(unsigned char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

APTR MUI_MakeObjectA(LONG type, ULONG *p)
{
	struct TagItem t[8];
	switch (type) {
	case MUIO_String:
		t[0].ti_Tag = MUIA_Frame;            t[0].ti_Data = MUIV_Frame_String;
		t[1].ti_Tag = MUIA_String_MaxLen;    t[1].ti_Data = p[1];
		t[2].ti_Tag = MUIA_ControlChar;      t[2].ti_Data = label_key((const char *)p[0]);
		t[3].ti_Tag = TAG_DONE;
		return NewObjectA(mm_cl_string, NULL, t);
	case MUIO_Cycle:
		t[0].ti_Tag = MUIA_Cycle_Entries;    t[0].ti_Data = p[1];
		t[1].ti_Tag = MUIA_ControlChar;      t[1].ti_Data = label_key((const char *)p[0]);
		t[2].ti_Tag = TAG_DONE;
		return NewObjectA(mm_cl_cycle, NULL, t);
	case MUIO_Checkmark:
		t[0].ti_Tag = MUIA_Image_Spec;       t[0].ti_Data = MUII_CheckMark;
		t[1].ti_Tag = MUIA_InputMode;        t[1].ti_Data = MUIV_InputMode_Toggle;
		t[2].ti_Tag = MUIA_ControlChar;      t[2].ti_Data = label_key((const char *)p[0]);
		t[3].ti_Tag = MUIA_Frame;            t[3].ti_Data = MUIV_Frame_ImageButton;
		t[4].ti_Tag = TAG_DONE;
		return NewObjectA(mm_cl_image, NULL, t);
	case MUIO_MenustripNM:
		t[0].ti_Tag = TAG_USER + 1;          t[0].ti_Data = p[0];
		t[1].ti_Tag = TAG_DONE;
		return NewObjectA(mm_cl_menustrip, NULL, t);
	}
	mm_log("MUI_MakeObject: type %ld not supported", (long)type);
	return NULL;
}

/* ---- notifications ---- */
void mm_notify_fire(Object *obj, ULONG attr, ULONG val)
{
	struct mm_notify *n, *next;
	for (n = MMO(obj)->notify; n; n = next) {
		ULONG m[9];
		ULONG i;
		next = n->next;
		if (n->attr != attr || (n->val != MUIV_EveryTime && n->val != val))
			continue;
		for (i = 0; i < n->n && i < 8; i++) {
			ULONG a = n->args[i];
			if (a == MUIV_TriggerValue)
				a = val;
			else if (a == MUIV_NotTriggerValue)
				a = !val;
			m[i] = a;
		}
		if (n->n && n->dest)
			DoMethodA(n->dest, (Msg)m);
	}
}

struct MUIP_Notify { ULONG MethodID, attr, val; Object *dest; ULONG n; ULONG args[8]; };

#define NOTIFY_SIZE ((ULONG)sizeof(struct MUI_NotifyData))

DISPATCHER(mm_notify_dispatch)
{
	switch (msg->MethodID) {
	case OM_NEW: {
		/* obj is the true class here (BOOPSI's rootclass convention) */
		struct IClass *tc = (struct IClass *)obj;
		ULONG size = sizeof(struct mm_obj) + sizeof(struct _Object) + tc->cl_InstOffset + tc->cl_InstSize;
		struct mm_obj *mo = mm_alloc(size);
		Object *o;
		if (!mo)
			return 0;
		mo->magic = MM_MAGIC;
		mo->cls = tc;
		mo->hweight = mo->vweight = 100;
		mo->in_l = mo->in_r = mo->in_t = mo->in_b = -1;
		((struct _Object *)(mo + 1))->o_Class = tc;
		o = (Object *)((struct _Object *)(mo + 1) + 1);
		((struct MUI_NotifyData *)o)->mnd_GlobalInfo = &mm_gi;
		mo->udata = mm_tag(((struct opSet *)msg)->ops_AttrList, MUIA_UserData, 0);
		tc->cl_ObjectCount++;
		return (ULONG)o;
	}
	case OM_DISPOSE: {
		struct mm_obj *mo = MMO(obj);
		struct mm_notify *n, *next;
		for (n = mo->notify; n; n = next) {
			next = n->next;
			mm_free(n);
		}
		mo->cls->cl_ObjectCount--;
		mo->magic = 0;
		mm_free(mo);
		return 0;
	}
	case OM_SET: {
		const struct TagItem *tl = ((struct opSet *)msg)->ops_AttrList, *t;
		int quiet = (int)mm_tag(tl, MUIA_NoNotify, FALSE);
		while ((t = next_tag(&tl)) != NULL) {
			if (t->ti_Tag == MUIA_UserData)
				MMO(obj)->udata = t->ti_Data;
			if (!quiet && t->ti_Tag != MUIA_NoNotify)
				mm_notify_fire(obj, t->ti_Tag, t->ti_Data);
		}
		return 0;
	}
	case OM_GET: {
		struct opGet *g = (struct opGet *)msg;
		if (g->opg_AttrID == MUIA_UserData) {
			*g->opg_Storage = MMO(obj)->udata;
			return TRUE;
		}
		return FALSE;
	}
	case MUIM_Notify: {
		struct MUIP_Notify *p = (struct MUIP_Notify *)msg;
		struct mm_notify *n = mm_alloc(sizeof(*n)), **pp;
		ULONG i;
		if (!n)
			return 0;
		n->attr = p->attr;
		n->val = p->val;
		n->dest = p->dest;
		n->n = p->n > 8 ? 8 : p->n;
		for (i = 0; i < n->n; i++)
			n->args[i] = p->args[i];
		/* in the order they were made */
		for (pp = &MMO(obj)->notify; *pp; pp = &(*pp)->next)
			;
		*pp = n;
		return TRUE;
	}
	case MUIM_Set: {
		ULONG *p = (ULONG *)msg;
		struct TagItem t[2];
		t[0].ti_Tag = p[1];
		t[0].ti_Data = p[2];
		t[1].ti_Tag = TAG_DONE;
		SetAttrsA(obj, t);
		return TRUE;
	}
	case MUIM_NoNotifySet: {
		ULONG *p = (ULONG *)msg;
		mm_set_quiet(obj, p[1], p[2]);
		return TRUE;
	}
	case MUIM_FindUData: {
		ULONG u = ((ULONG *)msg)[1];
		Object *c;
		if (MMO(obj)->udata == u)
			return (ULONG)obj;
		for (c = MMO(obj)->child; c; c = MMO(c)->next) {
			ULONG r = DoMethodA(c, msg);
			if (r)
				return r;
		}
		return 0;
	}
	case MUIM_Application_ReturnID:
		mm_app_returnid(((ULONG *)msg)[1]);
		return 0;
	}
	return 0;
}

/* ---- the classes ---- */
extern char mm_notify_dispatch_thunk[], mm_app_dispatch_thunk[], mm_win_dispatch_thunk[],
	mm_area_dispatch_thunk[], mm_group_dispatch_thunk[], mm_rect_dispatch_thunk[], mm_image_dispatch_thunk[],
	mm_text_dispatch_thunk[], mm_string_dispatch_thunk[], mm_cycle_dispatch_thunk[], mm_list_dispatch_thunk[],
	mm_listview_dispatch_thunk[], mm_menustrip_dispatch_thunk[], mm_menuitem_dispatch_thunk[];
ULONG mm_app_datasize(void), mm_win_datasize(void), mm_group_datasize(void), mm_image_datasize(void),
	mm_text_datasize(void), mm_string_datasize(void), mm_cycle_datasize(void), mm_list_datasize(void),
	mm_listview_datasize(void), mm_menustrip_datasize(void), mm_menuitem_datasize(void);

struct IClass *mm_cl_notify, *mm_cl_app, *mm_cl_win, *mm_cl_area, *mm_cl_group, *mm_cl_rect, *mm_cl_image,
	*mm_cl_text, *mm_cl_string, *mm_cl_cycle, *mm_cl_list, *mm_cl_listview, *mm_cl_menustrip, *mm_cl_menuitem;

void mm_classes_init(void)
{
	mm_gi.mgi_ApplicationObject = NULL;
	mm_cl_notify = mm_make_class(NULL, NOTIFY_SIZE, mm_notify_dispatch_thunk);
	mm_cl_app = mm_make_class(mm_cl_notify, mm_app_datasize(), mm_app_dispatch_thunk);
	mm_cl_win = mm_make_class(mm_cl_notify, mm_win_datasize(), mm_win_dispatch_thunk);
	mm_cl_menustrip = mm_make_class(mm_cl_notify, mm_menustrip_datasize(), mm_menustrip_dispatch_thunk);
	mm_cl_menuitem = mm_make_class(mm_cl_notify, mm_menuitem_datasize(), mm_menuitem_dispatch_thunk);
	mm_cl_area = mm_make_class(mm_cl_notify, sizeof(struct MUI_AreaData), mm_area_dispatch_thunk);
	mm_cl_group = mm_make_class(mm_cl_area, mm_group_datasize(), mm_group_dispatch_thunk);
	mm_cl_rect = mm_make_class(mm_cl_area, 0, mm_rect_dispatch_thunk);
	mm_cl_image = mm_make_class(mm_cl_area, mm_image_datasize(), mm_image_dispatch_thunk);
	mm_cl_text = mm_make_class(mm_cl_area, mm_text_datasize(), mm_text_dispatch_thunk);
	mm_cl_string = mm_make_class(mm_cl_area, mm_string_datasize(), mm_string_dispatch_thunk);
	mm_cl_cycle = mm_make_class(mm_cl_area, mm_cycle_datasize(), mm_cycle_dispatch_thunk);
	mm_cl_list = mm_make_class(mm_cl_area, mm_list_datasize(), mm_list_dispatch_thunk);
	mm_cl_listview = mm_make_class(mm_cl_group, mm_listview_datasize(), mm_listview_dispatch_thunk);
}

/* the application object, for _app() of every object */
void mm_set_app(Object *app)
{
	mm_gi.mgi_ApplicationObject = app;
}
