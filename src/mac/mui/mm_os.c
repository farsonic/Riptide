/*
 * Riptide - the Mac's MUI (see mm.h): the exec, dos and intuition calls the MUI editor makes,
 * and memory, logging and the file requester.
 *
 * Memory comes from the application heap (NewPtrClear). Libraries "open" as fixed bases, so the
 * editor's version checks pass. The ASL file requester is Standard File; its drawer is the full
 * path of the folder ("Disk:Folder"), which prefs_io_mac.c opens by path. URL_OpenA has no
 * browser to hand the address to: it fails, and the editor shows the address instead, as it
 * does on an Amiga without openurl.library.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdarg.h>
#include "mm.h"

/* ---- memory ---- */
void *mm_alloc(ULONG n)
{
	return mac_newptr((long)(n ? n : 1));
}

void mm_free(void *p)
{
	if (p)
		mac_disposeptr(p);
}

char *mm_strdup(const char *s)
{
	ULONG n = (ULONG)strlen(s) + 1;
	char *c = mm_alloc(n);
	if (c)
		memcpy(c, s, n);
	return c;
}

APTR AllocVec(ULONG size, ULONG req)
{
	(void)req;
	return mm_alloc(size);
}

void FreeVec(APTR p)
{
	mm_free(p);
}

APTR AllocMem(ULONG size, ULONG req)
{
	(void)req;
	return mm_alloc(size);
}

void FreeMem(APTR p, ULONG size)
{
	(void)size;
	mm_free(p);
}

/* ---- the log: development on Aurora68 sends it to the Pi's kernel log ---- */
void mm_log_line(const char *s);
void mm_log(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	va_start(ap, fmt);
	rt_vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	mm_log_line(buf);
}

#ifndef MM_LOG_NAME
#define MM_LOG_NAME "RiptidePrefs"            /* the program the log lines are from */
#endif
#ifdef AURORA68_LOG
void mm_log_line(const char *s)
{
	/* the Aurora68 card's mailbox, INFO command (src/mac/os_mac.c does the same for the driver) */
	char t[220];
	volatile UBYTE *c = (volatile UBYTE *)((*(volatile UBYTE *)0xCB2 ? 0xF9000000u : 0x00900000u) + 0x1000u);
	ULONG n, seq, i;
	rt_snprintf(t, sizeof t, MM_LOG_NAME ": %s", s);
	n = (ULONG)strlen(t);
	*(volatile UWORD *)(c + 8) = 2;
	*(const char *volatile *)(c + 28) = t;
	*(volatile ULONG *)(c + 32) = n;
	seq = *(volatile ULONG *)c + 1;
	*(volatile ULONG *)c = seq;
	for (i = 0; i < 200000u && *(volatile ULONG *)(c + 4) != seq; i++)
		;
}
#else
void mm_log_line(const char *s)
{
	(void)s;
}
#endif

/* ---- libraries ---- */
static struct Library libs[8];
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
	(void)name;
	libs[0].lib_Version = (UWORD)(version > 40 ? version : 40);
	libs[0].lib_Revision = 0;
	return &libs[0];
}

void CloseLibrary(struct Library *lib)
{
	(void)lib;
}

/* ---- intuition ---- */
struct Screen *LockPubScreen(CONST_STRPTR name)
{
	(void)name;
	return &mm_screen;
}

void UnlockPubScreen(CONST_STRPTR name, struct Screen *s)
{
	(void)name;
	(void)s;
}

LONG EasyRequestArgs(struct Window *w, const struct EasyStruct *es, ULONG *idcmp, const APTR args)
{
	(void)w;
	(void)es;
	(void)idcmp;
	(void)args;
	SysBeep(5);
	return 1;
}

/* ---- exec ---- */
ULONG Wait(ULONG sigs)
{
	(void)sigs;
	return 0;
}

/* ---- dos ---- */
LONG AddPart(STRPTR dir, CONST_STRPTR file, ULONG size)
{
	ULONG n = (ULONG)strlen((const char *)dir);
	if (n && dir[n - 1] != ':' && n + 1 < size)
		dir[n++] = ':';
	dir[n] = 0;
	return rt_strlcpy((char *)dir + n, (const char *)file, size - n) < size - n;
}

/* ---- ASL: Standard File ---- */
struct mm_filereq {
	struct rt_filereq fr;
	char drawer[256];
	char file[64];
	int save;
	int any;                        /* ASLFR_InitialPattern "#?": every file, not only text */
	char title[80];
	char initial[64];
};

/* the full path of folder dirID on volume vref, "Disk:Folder:Sub". A CInfoPBRec for a folder
 * (Inside Macintosh: Files): ioNamePtr at 18, ioVRefNum 22, ioFDirIndex 28, ioDrDirID 48,
 * ioDrParID 100; 108 bytes. */
int mm_dir_path(short vref, long dir, char *out, int len)
{
	char names[16][64];
	int n = 0, k, o = 0;
	while (n < 16) {
		UBYTE pb[108];
		unsigned char nm[64];
		memset(pb, 0, sizeof pb);
		nm[0] = 0;
		*(unsigned char **)(pb + 18) = nm;
		*(short *)(pb + 22) = vref;
		*(short *)(pb + 28) = -1;
		*(long *)(pb + 48) = dir;
		if (mac_pb(pb, TRAP_GETCATINFO) != 0)
			return -1;
		memcpy(names[n], nm + 1, nm[0]);
		names[n][nm[0]] = 0;
		n++;
		if (dir == 2)
			break;
		dir = *(long *)(pb + 100);
		if (dir < 2)
			break;
	}
	for (k = n - 1; k >= 0 && o < len - 1; k--) {
		int i;
		for (i = 0; names[k][i] && o < len - 2; i++)
			out[o++] = names[k][i];
		if (k > 0 || n == 1)
			out[o++] = ':';
	}
	out[o] = 0;
	return 0;
}

APTR MUI_AllocAslRequest(ULONG type, const struct TagItem *tags)
{
	struct mm_filereq *r = mm_alloc(sizeof(*r));
	(void)type;
	if (!r)
		return NULL;
	r->save = (int)mm_tag(tags, RT_ASLFR_DoSaveMode, 0);
	{
		const char *pat = (const char *)mm_tag(tags, RT_ASLFR_InitialPattern, 0);
		r->any = pat && pat[0] == '#' && pat[1] == '?' && !pat[2];
	}
	rt_strlcpy(r->title, (const char *)mm_tag(tags, RT_ASLFR_TitleText, (ULONG)""), sizeof r->title);
	rt_strlcpy(r->initial, (const char *)mm_tag(tags, RT_ASLFR_InitialFile, (ULONG)"riptide.conf"), sizeof r->initial);
	r->fr.fr_File = (STRPTR)r->file;
	r->fr.fr_Drawer = (STRPTR)r->drawer;
	return r;
}

LONG MUI_AslRequest(APTR req, const struct TagItem *tags)
{
	struct mm_filereq *r = req;
	StandardFileReply rep;
	unsigned char p1[256], p2[64];
	(void)tags;
	memset(&rep, 0, sizeof rep);
	if (r->save) {
		mac_stdputfile(mac_pstr(p1, r->title), mac_pstr(p2, r->initial), &rep);
	} else {
		long types[1] = { MAC_TYPE('T', 'E', 'X', 'T') };
		mac_stdgetfile(r->any ? -1 : 1, types, &rep);
	}
	if (!rep.sfGood)
		return FALSE;
	if (mm_dir_path(rep.sfFile.vRefNum, rep.sfFile.parID, r->drawer, sizeof r->drawer) < 0)
		return FALSE;
	memcpy(r->file, rep.sfFile.name + 1, rep.sfFile.name[0]);
	r->file[rep.sfFile.name[0]] = 0;
	return TRUE;
}

void MUI_FreeAslRequest(APTR req)
{
	mm_free(req);
}

LONG MUI_RequestA(APTR app, APTR win, ULONG flags, CONST_STRPTR title, CONST_STRPTR gadgets, CONST_STRPTR fmt,
                  APTR params)
{
	(void)app; (void)win; (void)flags; (void)title; (void)gadgets; (void)fmt; (void)params;
	SysBeep(5);
	return 1;
}

/* openurl.library: no browser on a classic Mac to hand the address to */
extern struct Library *OpenURLBase;   /* mui_main.c has it */
ULONG URL_OpenA(CONST_STRPTR url, const struct TagItem *tags)
{
	(void)url;
	(void)tags;
	return 0;
}

/* ==================== exec: lists and message ports ==================== */
void AddHead(struct List *l, struct Node *n)
{
	n->ln_Succ = l->lh_Head;
	n->ln_Pred = (struct Node *)&l->lh_Head;
	l->lh_Head->ln_Pred = n;
	l->lh_Head = n;
}

void AddTail(struct List *l, struct Node *n)
{
	n->ln_Succ = (struct Node *)&l->lh_Tail;
	n->ln_Pred = l->lh_TailPred;
	l->lh_TailPred->ln_Succ = n;
	l->lh_TailPred = n;
}

void Remove(struct Node *n)
{
	n->ln_Pred->ln_Succ = n->ln_Succ;
	n->ln_Succ->ln_Pred = n->ln_Pred;
}

struct Node *RemHead(struct List *l)
{
	struct Node *n = l->lh_Head;
	if (!n->ln_Succ)
		return NULL;
	Remove(n);
	return n;
}

struct Node *FindName(struct List *l, CONST_STRPTR name)
{
	struct Node *n;
	if (!l || !l->lh_Head)
		return NULL;
	for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ)
		if (n->ln_Name && !strcmp(n->ln_Name, (const char *)name))
			return n;
	return NULL;
}

static struct List ports;
static int ports_init;
static void ports_setup(void)
{
	if (!ports_init) {
		ports.lh_Head = (struct Node *)&ports.lh_Tail;
		ports.lh_Tail = NULL;
		ports.lh_TailPred = (struct Node *)&ports.lh_Head;
		ports_init = 1;
	}
}

struct MsgPort *mm_riptide_port(void);
void mm_riptide_put(struct Message *msg);

struct MsgPort *CreateMsgPort(void)
{
	struct MsgPort *p = mm_alloc(sizeof(*p));
	if (p) {
		p->mp_MsgList.lh_Head = (struct Node *)&p->mp_MsgList.lh_Tail;
		p->mp_MsgList.lh_Tail = NULL;
		p->mp_MsgList.lh_TailPred = (struct Node *)&p->mp_MsgList.lh_Head;
	}
	return p;
}

void DeleteMsgPort(struct MsgPort *p)
{
	mm_free(p);
}

void AddPort(struct MsgPort *p)
{
	ports_setup();
	AddTail(&ports, &p->mp_Node);
}

void RemPort(struct MsgPort *p)
{
	Remove(&p->mp_Node);
}

struct MsgPort *FindPort(CONST_STRPTR name)
{
	ports_setup();
	if (!strcmp((const char *)name, RIPTIDE_PORTNAME))
		return mm_riptide_port();
	return (struct MsgPort *)FindName(&ports, name);
}

void PutMsg(struct MsgPort *p, struct Message *m)
{
	if (p == mm_riptide_port()) {
		mm_riptide_put(m);
		return;
	}
	AddTail(&p->mp_MsgList, &m->mn_Node);
}

void ReplyMsg(struct Message *m)
{
	if (m->mn_ReplyPort)
		PutMsg(m->mn_ReplyPort, m);
}

struct Message *GetMsg(struct MsgPort *p)
{
	return (struct Message *)RemHead(&p->mp_MsgList);
}

struct Message *WaitPort(struct MsgPort *p)
{
	/* replies here are made at once, so the message is there or never comes */
	struct Node *n = p->mp_MsgList.lh_Head;
	return n && n->ln_Succ ? (struct Message *)n : NULL;
}

void Forbid(void) { }
void Permit(void) { }

static struct Task mm_task;
struct Task *FindTask(CONST_STRPTR name)
{
	(void)name;
	return &mm_task;
}

BYTE OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *io, ULONG flags)
{
	(void)name; (void)unit; (void)io; (void)flags;
	return -1;
}

void CloseDevice(struct IORequest *io)
{
	(void)io;
}

/* ==================== dos ==================== */
APTR DateStamp(APTR date)
{
	struct DateStamp *ds = date;
	/* Time (0x20C): seconds since 1904; the Amiga counts from 1978 */
	ULONG secs = *(volatile ULONG *)0x20C - 2337984000UL;
	ds->ds_Days = (LONG)(secs / 86400);
	ds->ds_Minute = (LONG)((secs % 86400) / 60);
	ds->ds_Tick = (LONG)((secs % 60) * 50 + (TickCount() % 60) * 50 / 60);
	return ds;
}

LONG PutStr(CONST_STRPTR s)
{
	mm_log("%s", (const char *)s);
	return 0;
}

LONG IoErr(void)
{
	return 0;
}

/* Amiga paths on the Mac: ENVARC:Riptide/x is the Riptide folder in the Preferences folder,
 * ENV:Riptide/x its "In Use" folder (which Riptide empties when the Mac starts, as ENV: goes
 * with a reboot); anything else is a Mac path already ("Disk:Folder:file"), '/' read as ':'. */
static char prefs_dir[200];

void mm_paths_init(void)
{
	short vref = 0;
	long dir = 0;
	if (mac_findfolder(-32768, MAC_TYPE('p', 'r', 'e', 'f'), 1, &vref, &dir) == 0 &&
	    mm_dir_path(vref, dir, prefs_dir, sizeof prefs_dir) == 0) {
		mm_log("Preferences folder: %s", prefs_dir);
		return;
	}
	prefs_dir[0] = 0;
}

static void mac_path(const char *a, unsigned char *p)
{
	char t[256];
	int n = 0, i;
	if (!strncmp(a, "ENVARC:", 7) || !strncmp(a, "ENV:", 4)) {
		int env = a[3] == ':';
		n = rt_snprintf(t, sizeof t, "%s", prefs_dir);
		if (n && t[n - 1] != ':' && n < 250)
			t[n++] = ':';
		a += env ? 4 : 7;
		/* ENV:Riptide... -> Riptide:In Use... */
		if (env && !strncmp(a, "Riptide", 7) && (a[7] == '/' || !a[7])) {
			n += rt_snprintf(t + n, sizeof t - (size_t)n, "Riptide:In Use");
			a += 7;
		}
	}
	for (; *a && n < 254; a++)
		t[n++] = *a == '/' ? ':' : *a;
	t[n] = 0;
	for (i = 0; i < n; i++)
		p[i + 1] = (unsigned char)t[i];
	p[0] = (unsigned char)n;
}

/* catalog information of a full path: 0 file, 1 folder, -1 none; size of a file */
static int cat_info(const unsigned char *path, LONG *size)
{
	UBYTE pb[108];
	memset(pb, 0, sizeof pb);
	*(const unsigned char **)(pb + 18) = path;
	if (mac_pb(pb, TRAP_GETCATINFO) != 0)
		return -1;
	if (pb[30] & 0x10)
		return 1;
	if (size)
		*size = *(LONG *)(pb + 54);
	return 0;
}

struct mm_lock { unsigned char path[256]; int dir; LONG size; };

BPTR Lock(CONST_STRPTR name, LONG mode)
{
	struct mm_lock *l;
	unsigned char p[256];
	LONG size = 0;
	int k;
	(void)mode;
	mac_path((const char *)name, p);
	if ((k = cat_info(p, &size)) < 0)
		return 0;
	l = mm_alloc(sizeof(*l));
	if (!l)
		return 0;
	memcpy(l->path, p, (unsigned long)p[0] + 1);
	l->dir = k;
	l->size = size;
	return (BPTR)l;
}

void UnLock(BPTR lock)
{
	mm_free((void *)lock);
}

LONG Examine(BPTR lock, struct FileInfoBlock *fib)
{
	struct mm_lock *l = (struct mm_lock *)lock;
	int n = l->path[0], i = n;
	memset(fib, 0, sizeof(*fib));
	fib->fib_DirEntryType = l->dir ? 2 : -3;
	fib->fib_EntryType = fib->fib_DirEntryType;
	fib->fib_Size = l->size;
	while (i > 0 && l->path[i] != ':')
		i--;
	memcpy(fib->fib_FileName, l->path + i + 1, (unsigned long)(n - i));
	fib->fib_FileName[n - i] = 0;
	return DOSTRUE;
}

BPTR CreateDir(CONST_STRPTR name)
{
	UBYTE pb[108];
	unsigned char p[256];
	mac_path((const char *)name, p);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = p;
	if (mac_pb(pb, 0x0006A260L) != 0)       /* _HFSDispatch 6: DirCreate */
		return 0;
	return Lock(name, ACCESS_READ);
}

LONG DeleteFile(CONST_STRPTR name)
{
	UBYTE pb[108];
	unsigned char p[256];
	mac_path((const char *)name, p);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = p;
	return mac_pb(pb, TRAP_HDELETE) == 0 ? DOSTRUE : 0;
}

LONG Rename(CONST_STRPTR from, CONST_STRPTR to)
{
	UBYTE pb[108];
	unsigned char p[256], q[256], nm[64];
	int i;
	mac_path((const char *)from, p);
	mac_path((const char *)to, q);
	/* the new name: the last part of the new path (Rename stays in its folder) */
	i = q[0];
	while (i > 0 && q[i] != ':')
		i--;
	nm[0] = (unsigned char)(q[0] - i);
	memcpy(nm + 1, q + i + 1, nm[0]);
	memset(pb, 0, sizeof pb);
	*(unsigned char **)(pb + 18) = p;
	*(unsigned char **)(pb + 28) = nm;
	return mac_pb(pb, 0xA20B) == 0 ? DOSTRUE : 0;       /* _HRename */
}

struct mm_fh { short ref; };

static short hopen(const unsigned char *p, int perm)
{
	UBYTE pb[108];
	memset(pb, 0, sizeof pb);
	*(const unsigned char **)(pb + 18) = p;
	pb[27] = (UBYTE)perm;
	if (mac_pb(pb, TRAP_HOPEN) != 0)
		return 0;
	return *(short *)(pb + 24);
}

static void set_type(const unsigned char *p)
{
	UBYTE pb[108];
	memset(pb, 0, sizeof pb);
	*(const unsigned char **)(pb + 18) = p;
	if (mac_pb(pb, TRAP_HGETFINFO) != 0)
		return;
	*(long *)(pb + 32) = MAC_TYPE('T', 'E', 'X', 'T');
	*(long *)(pb + 36) = MAC_TYPE('t', 't', 'x', 't');
	*(long *)(pb + 48) = 0;
	mac_pb(pb, TRAP_HSETFINFO);
}

BPTR Open(CONST_STRPTR name, LONG mode)
{
	unsigned char p[256];
	struct mm_fh *f;
	short ref;
	mac_path((const char *)name, p);
	if (mode == MODE_NEWFILE) {
		UBYTE pb[108];
		memset(pb, 0, sizeof pb);
		*(unsigned char **)(pb + 18) = p;
		mac_pb(pb, TRAP_HCREATE);               /* fails harmlessly when it is there */
		set_type(p);
		ref = hopen(p, 3);
		if (ref) {
			memset(pb, 0, sizeof pb);
			*(short *)(pb + 24) = ref;
			*(long *)(pb + 28) = 0;
			mac_pb(pb, TRAP_SETEOF);
		}
	} else {
		ref = hopen(p, mode == MODE_OLDFILE ? 1 : 3);
	}
	if (!ref)
		return 0;
	f = mm_alloc(sizeof(*f));
	if (!f)
		return 0;
	f->ref = ref;
	return (BPTR)f;
}

LONG Close(BPTR fh)
{
	struct mm_fh *f = (struct mm_fh *)fh;
	UBYTE pb[64];
	if (!f)
		return 0;
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = f->ref;
	mac_pb(pb, TRAP_CLOSE);
	memset(pb, 0, sizeof pb);
	mac_pb(pb, 0xA013);                     /* _FlushVol: the default volume */
	mm_free(f);
	return DOSTRUE;
}

/* text files: the Mac's line ends are CR, the editor's LF */
LONG Read(BPTR fh, APTR buf, LONG n)
{
	struct mm_fh *f = (struct mm_fh *)fh;
	UBYTE pb[64];
	LONG got, i;
	char *b = buf;
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = f->ref;
	*(char **)(pb + 32) = b;
	*(LONG *)(pb + 36) = n;
	mac_pb(pb, TRAP_READ);                  /* eofErr with what there was is fine */
	got = *(LONG *)(pb + 40);
	for (i = 0; i < got; i++)
		if (b[i] == '\r')
			b[i] = '\n';
	return got;
}

LONG Write(BPTR fh, APTR buf, LONG n)
{
	struct mm_fh *f = (struct mm_fh *)fh;
	const char *s = buf;
	char chunk[512];
	LONG done = 0;
	while (done < n) {
		UBYTE pb[64];
		LONG k = n - done > (LONG)sizeof chunk ? (LONG)sizeof chunk : n - done, i;
		for (i = 0; i < k; i++)
			chunk[i] = s[done + i] == '\n' ? '\r' : s[done + i];
		memset(pb, 0, sizeof pb);
		*(short *)(pb + 24) = f->ref;
		*(char **)(pb + 32) = chunk;
		*(LONG *)(pb + 36) = k;
		if (mac_pb(pb, TRAP_WRITE) != 0)
			return done ? done : -1;
		done += k;
	}
	return done;
}

LONG Seek(BPTR fh, LONG pos, LONG mode)
{
	struct mm_fh *f = (struct mm_fh *)fh;
	UBYTE pb[64];
	LONG old;
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = f->ref;
	*(short *)(pb + 44) = 0;                /* fsAtMark: where it is */
	mac_pb(pb, TRAP_SETFPOS);
	old = *(LONG *)(pb + 46);
	memset(pb, 0, sizeof pb);
	*(short *)(pb + 24) = f->ref;
	*(short *)(pb + 44) = (short)(mode == OFFSET_BEGINNING ? 1 : mode == OFFSET_END ? 2 : 3);
	*(LONG *)(pb + 46) = pos;
	if (mac_pb(pb, TRAP_SETFPOS) != 0)
		return -1;
	return old;
}

/* riptide.library: the editor uses it only for a Riptide too old to answer TABLES */
LONG RiptideSysCtl(LONG *name, ULONG namelen, APTR oldp, LONG *oldlenp, APTR newp, LONG newlen)
{
	(void)name; (void)namelen; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
	return -1;
}

/* library bases the Amiga files refer to: zeroed stand-ins (their lists are empty, so
 * FindName finds nothing, and nothing is called through them) */
static ULONG execbase_area[256];
struct ExecBase *SysBase = (struct ExecBase *)execbase_area;
struct Library *SocketBase, *RiptideBase;

/* the Toolbox calls a control's action procedure (ostraps.S); RiptidePrefs has no controls */
void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	(void)part;
}
