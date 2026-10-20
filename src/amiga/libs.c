/*
 * Riptide - library construction for bsdsocket.library, miami.library and
 * usergroup.library.
 *
 * Like MiamiDx (docs/RE-NOTES.md 2.2) every library has a master base in
 * the system list; its Open vector creates (or reuses) a private base per
 * opener task with MakeLibrary() and the same vector table, and returns
 * that. Later calls and CloseLibrary() land in the private base.
 * bsdsocket and miami share one per-task context (struct TaskCtx), which
 * holds the socket descriptor table, errno state and the task's sleeper.
 * The Open and Close vectors are called by exec under Forbid().
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

static struct MinList ctx_list;
static struct TaskCtx *ctx_cache;
volatile int amiga_libs_closing;

static struct RtLibDef *const all_defs[] = { &rtlib_bsdsocket, &rtlib_miami, &rtlib_usergroup };

#define BASE_OF_LINK(n) ((struct RtBase *)((UBYTE *)(n) - offsetof(struct RtBase, link)))

struct TaskCtx *amiga_ctx_find(struct Task *t)
{
	struct MinNode *n;
	struct TaskCtx *c;

	Forbid();
	c = ctx_cache;
	if (!c || c->task != t) {
		c = NULL;
		for (n = ctx_list.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
			if (((struct TaskCtx *)n)->task == t) {
				c = (struct TaskCtx *)n;
				ctx_cache = c;
				break;
			}
		}
	}
	Permit();
	return c;
}

/* Is c a live context? (sockets released with ReleaseSocket() keep the
 * owner pointer of a context that may be gone.) Call under Forbid(). */
int amiga_ctx_valid(struct TaskCtx *c)
{
	struct MinNode *n;

	for (n = ctx_list.mlh_Head; n->mln_Succ; n = n->mln_Succ)
		if ((struct TaskCtx *)n == c)
			return 1;
	return 0;
}

static struct TaskCtx *ctx_create(struct Task *me)
{
	struct TaskCtx *c = AllocVec(sizeof(*c), MEMF_PUBLIC | MEMF_CLEAR);

	if (!c)
		return NULL;
	memset(c, 0, sizeof(*c));
	c->task = me;
	if (rt_ctx_init(&c->sc, 64, c) < 0) {
		FreeVec(c);
		return NULL;
	}
	if (amiga_sleeper_init(&c->sl, &c->sc.sigintr_mask) < 0) {
		rt_ctx_destroy(&c->sc);
		FreeVec(c);
		return NULL;
	}
	bsd_ctx_init(c);
	Forbid();
	AddTail((struct List *)&ctx_list, (struct Node *)&c->node);
	Permit();
	return c;
}

static void ctx_destroy(struct TaskCtx *c)
{
	Forbid();
	Remove((struct Node *)&c->node);
	if (ctx_cache == c)
		ctx_cache = NULL;
	Permit();
	rt_ctx_destroy(&c->sc);
	amiga_sleeper_fini(&c->sl);
	FreeVec(c);
}

static struct Library *make_lib(struct RtLibDef *def)
{
	struct Library *lib;

	lib = (struct Library *)MakeLibrary((APTR)def->vectors, NULL, NULL, def->possize, 0);
	if (!lib)
		return NULL;
	memset((UBYTE *)lib + sizeof(struct Library), 0, def->possize - sizeof(struct Library));
	lib->lib_Node.ln_Type = NT_LIBRARY;
	lib->lib_Node.ln_Pri = 0;
	lib->lib_Node.ln_Name = (char *)def->name;
	lib->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	lib->lib_Version = def->version;
	lib->lib_Revision = def->revision;
	lib->lib_IdString = (APTR)def->idstring;
	lib->lib_OpenCnt = 0;
	return lib;
}

static void free_lib(struct Library *lib)
{
	FreeMem((UBYTE *)lib - lib->lib_NegSize, (ULONG)lib->lib_NegSize + lib->lib_PosSize);
}

struct Library *rtlib_open(struct RtLibDef *def, struct Library *master)
{
	struct Task *me = FindTask(NULL);
	struct MinNode *n;
	struct RtBase *b;
	struct TaskCtx *c = NULL;
	struct Library *lib;

	if (amiga_libs_closing || master != def->master)
		return NULL;
	for (n = def->bases.mlh_Head; n->mln_Succ; n = n->mln_Succ) {
		b = BASE_OF_LINK(n);
		if (b->task == me) {
			b->lib.lib_OpenCnt++;
			return &b->lib;
		}
	}
	lib = make_lib(def);
	if (!lib)
		return NULL;
	b = (struct RtBase *)lib;
	b->def = def;
	b->task = me;
	if (def->needs_ctx) {
		c = amiga_ctx_find(me);
		if (!c)
			c = ctx_create(me);
		if (!c) {
			free_lib(lib);
			return NULL;
		}
		b->ctx = c;
		if (def == &rtlib_bsdsocket)
			c->bsdbase = lib;
		else
			c->miamibase = lib;
	}
	if (def->init && def->init(b) < 0) {
		if (c) {
			if (def == &rtlib_bsdsocket)
				c->bsdbase = NULL;
			else
				c->miamibase = NULL;
			if (!c->bsdbase && !c->miamibase)
				ctx_destroy(c);
		}
		free_lib(lib);
		return NULL;
	}
	lib->lib_OpenCnt = 1;
	Forbid();
	AddTail((struct List *)&def->bases, (struct Node *)&b->link);
	master->lib_OpenCnt++;
	master->lib_Flags &= ~LIBF_DELEXP;
	Permit();
	return lib;
}

ULONG rtlib_close(struct RtBase *b)
{
	struct RtLibDef *def = b->def;
	struct TaskCtx *c = b->ctx;

	if (!def || b->lib.lib_OpenCnt == 0)
		return 0;          /* the master or a stale base */
	if (--b->lib.lib_OpenCnt)
		return 0;
	if (def->fini)
		def->fini(b);
	if (c) {
		if (c->bsdbase == &b->lib)
			c->bsdbase = NULL;
		if (c->miamibase == &b->lib)
			c->miamibase = NULL;
		if (!c->bsdbase && !c->miamibase)
			ctx_destroy(c);
	}
	Forbid();
	Remove((struct Node *)&b->link);
	if (def->master->lib_OpenCnt)
		def->master->lib_OpenCnt--;
	Permit();
	free_lib(&b->lib);
	/* wake the stack process: it may be waiting for the last user to go */
	rt_kick();
	return 0;
}

ULONG rtlib_expunge(RTREG(a6) struct Library *base)
{
	/* Riptide removes its libraries itself when it quits. */
	base->lib_Flags |= LIBF_DELEXP;
	return 0;
}

int amiga_libs_present(void)
{
	static const char *const names[] = { "bsdsocket.library", "miami.library", "usergroup.library" };
	struct List *ll = exec_liblist(SysBase);
	int i, found = 0;

	Forbid();
	for (i = 0; i < 3; i++)
		if (FindName(ll, (CONST_STRPTR)names[i]))
			found = 1;
	Permit();
	return found;
}

/* Describe the TCP/IP stack libraries already in the system, one per line,
 * by its id string (or name and version) so the user can tell who owns them. */
void amiga_libs_describe(char *buf, unsigned len)
{
	static const char *const names[] = { "bsdsocket.library", "miami.library", "usergroup.library" };
	struct List *ll = exec_liblist(SysBase);
	struct Library *lib;
	unsigned n = 0;
	int i;

	buf[0] = '\0';
	Forbid();
	for (i = 0; i < 3 && n < len; i++) {
		lib = (struct Library *)FindName(ll, (CONST_STRPTR)names[i]);
		if (lib) {
			const char *id = (const char *)lib->lib_IdString;
			int k = (id && id[0])
				? rt_snprintf(buf + n, len - n, "  %s", id)
				: rt_snprintf(buf + n, len - n, "  %s %u.%u", names[i],
				              (unsigned)lib->lib_Version, (unsigned)lib->lib_Revision);
			if (k < 0)
				break;
			n += (unsigned)k;
			if (n >= len)
				n = len - 1;   /* truncated */
			while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
				buf[--n] = '\0';
			if (n + 1 < len) {
				buf[n++] = '\n';
				buf[n] = '\0';
			}
		}
	}
	Permit();
}

int amiga_libs_init(void)
{
	unsigned i;

	rt_newminlist(&ctx_list);
	amiga_libs_closing = 0;
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++) {
		struct RtLibDef *d = all_defs[i];
		rt_newminlist(&d->bases);
		d->master = make_lib(d);
		if (!d->master)
			goto fail;
	}
	Forbid();
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++)
		AddLibrary(all_defs[i]->master);
	Permit();
	return 0;
fail:
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++) {
		if (all_defs[i]->master)
			free_lib(all_defs[i]->master);
		all_defs[i]->master = NULL;
	}
	return -1;
}

int amiga_libs_users(void)
{
	unsigned i;
	int n = 0;

	Forbid();
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++)
		if (all_defs[i]->master)
			n += all_defs[i]->master->lib_OpenCnt;
	Permit();
	return n;
}

int amiga_libs_remove(void)
{
	unsigned i;

	Forbid();
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++) {
		if (all_defs[i]->master && all_defs[i]->master->lib_OpenCnt) {
			Permit();
			return -1;
		}
	}
	amiga_libs_closing = 1;
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++)
		if (all_defs[i]->master)
			Remove(&all_defs[i]->master->lib_Node);
	Permit();
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++) {
		if (all_defs[i]->master)
			free_lib(all_defs[i]->master);
		all_defs[i]->master = NULL;
	}
	return 0;
}

/* Signal every opener (shutdown request, like MiamiDx's Ctrl-C). */
void amiga_libs_signal_users(ULONG sigs)
{
	struct MinNode *n;
	unsigned i;

	Forbid();
	for (i = 0; i < RT_ARRAY_SIZE(all_defs); i++)
		for (n = all_defs[i]->bases.mlh_Head; n && n->mln_Succ; n = n->mln_Succ)
			Signal(BASE_OF_LINK(n)->task, sigs);
	Permit();
}

/* ---- calling application code with register arguments ---- */
#if defined(__VBCC__)
typedef LONG (*fn_d0d1_t)(__reg("d0") LONG, __reg("d1") LONG);
LONG amiga_call_d0d1(APTR fn, LONG d0, LONG d1)
{
	return ((fn_d0d1_t)fn)(d0, d1);
}
#endif
/* GCC: amiga_call_d0d1 is in gcc_thunks.S */
