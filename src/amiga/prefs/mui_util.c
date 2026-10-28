/*
 * Riptide - RiptidePrefs (MUI): small helpers around muimaster.library
 * and BOOPSI (see mui.h). The varargs helpers pass the address of their
 * first variable argument as the tag list or method message: on the
 * 68000 family both compilers put the arguments on the stack one after
 * the other, 32 bits each, as amiga.lib's own varargs stubs expect.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/mui.h"

struct Library *MUIMasterBase;

Object *mui_new(const char *cls, ULONG tag, ...)
{
	return (Object *)MUI_NewObjectA((CONST_STRPTR)cls, (const struct TagItem *)&tag);
}

Object *mui_newc(struct MUI_CustomClass *mcc, ULONG tag, ...)
{
	return (Object *)NewObjectA(mcc->mcc_Class, NULL, (const struct TagItem *)&tag);
}

Object *mui_make(LONG type, ULONG p1, ...)
{
	return (Object *)MUI_MakeObjectA(type, &p1);
}

ULONG mui_do(Object *obj, ULONG id, ...)
{
	return DoMethodA(obj, (Msg)&id);
}

void mui_set(Object *obj, ULONG attr, ULONG v)
{
	struct TagItem t[2];

	t[0].ti_Tag = attr;
	t[0].ti_Data = v;
	t[1].ti_Tag = TAG_DONE;
	t[1].ti_Data = 0;
	SetAttrsA(obj, t);
}

void mui_nnset(Object *obj, ULONG attr, ULONG v)
{
	struct TagItem t[3];

	t[0].ti_Tag = MUIA_NoNotify;
	t[0].ti_Data = TRUE;
	t[1].ti_Tag = attr;
	t[1].ti_Data = v;
	t[2].ti_Tag = TAG_DONE;
	t[2].ti_Data = 0;
	SetAttrsA(obj, t);
}

ULONG mui_get(Object *obj, ULONG attr)
{
	ULONG v = 0;

	GetAttr(attr, obj, &v);
	return v;
}

#if !defined(__VBCC__)
/* call a dispatcher hook with a0 = hook, a2 = object, a1 = message
 * (mui_thunks.S) */
ULONG rp_hook_call(struct Hook *h, Object *obj, Msg msg);

ULONG DoMethodA(Object *obj, Msg msg)
{
	struct IClass *cl = OCLASS(obj);
	return rp_hook_call(&cl->cl_Dispatcher, obj, msg);
}

ULONG DoSuperMethodA(struct IClass *cl, Object *obj, Msg msg)
{
	return rp_hook_call(&cl->cl_Super->cl_Dispatcher, obj, msg);
}

ULONG CoerceMethodA(struct IClass *cl, Object *obj, Msg msg)
{
	return rp_hook_call(&cl->cl_Dispatcher, obj, msg);
}
#endif
