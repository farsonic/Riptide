/*
 * Riptide - usergroup.library (version 4, AmiTCP 4 API, 39 functions).
 *
 * AmigaOS is a single user system, so this is a minimal implementation:
 * one user (default "user", uid 0, gid 0, home SYS:, configurable with the
 * "user" configuration keyword) and one group. Credentials changes are
 * kept per opener task. crypt() is NOT DES: it is a placeholder hash in
 * the crypt(3) output format (2 salt characters + 11 characters), so
 * password files written by other stacks will not verify.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"
#include "gen/usergroup_impl.h"

struct UgBase {
	struct RtBase rb;
	APTR  errnoptr;
	LONG  errnosize;
	LONG  err;
	ULONG intrmask;
	LONG  ruid, euid, rgid, egid;
	ULONG umask_;
	LONG  ngroups;
	LONG  groups[NGROUPS];
	char  login[MAXLOGNAME];
	int   pwpos, grpos;
	struct passwd pw;
	struct group gr;
	char *grmem[2];
	struct UserGroupCredentials cred;
	char  cryptbuf[16];
	char  passbuf[64];
};

static int ug_init(struct RtBase *b);

struct RtLibDef rtlib_usergroup = {
	"usergroup.library",
	"usergroup.library 4.1 (" RIPTIDE_DATE ") Riptide " RIPTIDE_VERSION_STRING "\r\n",
	4, 1, usergroup_vectors, sizeof(struct UgBase), 0, ug_init, NULL, NULL, { NULL, NULL, NULL }
};

/* the configured user ("user NAME [uid=N] [gid=N] [home=DIR] [realname=TEXT]") */
char amiga_user_name[32] = "user";
char amiga_user_home[64] = "SYS:";
char amiga_user_realname[64] = "Amiga User";
LONG amiga_user_uid = 0, amiga_user_gid = 0;
static const char group_name[] = "users";

#define UG struct UgBase *u = (struct UgBase *)base

static void ug_seterr(struct UgBase *u, LONG e)
{
	u->err = e;
	if (u->errnoptr) {
		switch (u->errnosize) {
		case 1: *(UBYTE *)u->errnoptr = (UBYTE)e; break;
		case 2: *(UWORD *)u->errnoptr = (UWORD)e; break;
		default: *(ULONG *)u->errnoptr = (ULONG)e; break;
		}
	}
}

static int ug_init(struct RtBase *b)
{
	struct UgBase *u = (struct UgBase *)b;

	u->ruid = u->euid = amiga_user_uid;
	u->rgid = u->egid = amiga_user_gid;
	u->umask_ = 022;
	u->ngroups = 1;
	u->groups[0] = amiga_user_gid;
	rt_strlcpy(u->login, amiga_user_name, sizeof(u->login));
	return 0;
}

LONG ug_ug_SetupContextTagList(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR pname,
                               RTREG(a1) APTR tags)
{
	UG;
	struct TagItem *t = tags;

	while (t && t->ti_Tag != TAG_DONE) {
		switch (t->ti_Tag) {
		case TAG_MORE:
			t = (struct TagItem *)t->ti_Data;
			continue;
		case UGT_ERRNOBPTR: u->errnoptr = (APTR)t->ti_Data; u->errnosize = 1; break;
		case UGT_ERRNOWPTR: u->errnoptr = (APTR)t->ti_Data; u->errnosize = 2; break;
		case UGT_ERRNOLPTR: u->errnoptr = (APTR)t->ti_Data; u->errnosize = 4; break;
		case UGT_INTRMASK: u->intrmask = t->ti_Data; break;
		default: break;     /* UGT_OWNER and unknown tags are accepted */
		}
		t++;
	}
	return 0;
}

LONG ug_ug_GetErr(RTREG(a6) struct Library *base)
{
	UG;
	return u->err;
}

ULONG ug_ug_StrError(RTREG(a6) struct Library *base, RTREG(d1) LONG code)
{
	return (ULONG)rt_strerror(code);
}

LONG ug_getuid(RTREG(a6) struct Library *base) { UG; return u->ruid; }
LONG ug_geteuid(RTREG(a6) struct Library *base) { UG; return u->euid; }
LONG ug_getgid(RTREG(a6) struct Library *base) { UG; return u->rgid; }
LONG ug_getegid(RTREG(a6) struct Library *base) { UG; return u->egid; }

LONG ug_setreuid(RTREG(a6) struct Library *base, RTREG(d0) LONG ruid, RTREG(d1) LONG euid)
{
	UG;
	if (ruid != -1)
		u->ruid = ruid;
	if (euid != -1)
		u->euid = euid;
	return 0;
}

LONG ug_setuid(RTREG(a6) struct Library *base, RTREG(d0) LONG uid)
{
	UG;
	u->ruid = u->euid = uid;
	return 0;
}

LONG ug_setregid(RTREG(a6) struct Library *base, RTREG(d0) LONG rgid, RTREG(d1) LONG egid)
{
	UG;
	if (rgid != -1)
		u->rgid = rgid;
	if (egid != -1)
		u->egid = egid;
	return 0;
}

LONG ug_setgid(RTREG(a6) struct Library *base, RTREG(d0) LONG gid)
{
	UG;
	u->rgid = u->egid = gid;
	return 0;
}

LONG ug_getgroups(RTREG(a6) struct Library *base, RTREG(d0) LONG gidsetlen, RTREG(a1) LONG *gidset)
{
	UG;
	LONG i;

	if (gidsetlen == 0)
		return u->ngroups;
	if (gidsetlen < u->ngroups || !gidset) {
		ug_seterr(u, RT_EINVAL);
		return -1;
	}
	for (i = 0; i < u->ngroups; i++)
		gidset[i] = u->groups[i];
	return u->ngroups;
}

LONG ug_setgroups(RTREG(a6) struct Library *base, RTREG(d0) LONG gidsetlen, RTREG(a1) const LONG *gidset)
{
	UG;
	LONG i;

	if (gidsetlen < 0 || gidsetlen > NGROUPS || (gidsetlen && !gidset)) {
		ug_seterr(u, RT_EINVAL);
		return -1;
	}
	for (i = 0; i < gidsetlen; i++)
		u->groups[i] = gidset[i];
	u->ngroups = gidsetlen;
	return 0;
}

LONG ug_initgroups(RTREG(a6) struct Library *base, RTREG(a1) CONST_STRPTR name, RTREG(d0) LONG basegid)
{
	UG;
	u->groups[0] = basegid;
	u->ngroups = 1;
	return 0;
}

static struct passwd *the_user(struct UgBase *u)
{
	u->pw.pw_name = amiga_user_name;
	u->pw.pw_passwd = (char *)"*";
	u->pw.pw_uid = amiga_user_uid;
	u->pw.pw_gid = amiga_user_gid;
	u->pw.pw_gecos = amiga_user_realname;
	u->pw.pw_dir = amiga_user_home;
	u->pw.pw_shell = (char *)"noshell";
	return &u->pw;
}

static struct group *the_group(struct UgBase *u)
{
	u->grmem[0] = amiga_user_name;
	u->grmem[1] = NULL;
	u->gr.gr_name = (char *)group_name;
	u->gr.gr_passwd = (char *)"*";
	u->gr.gr_gid = amiga_user_gid;
	u->gr.gr_mem = u->grmem;
	return &u->gr;
}

ULONG ug_getpwnam(RTREG(a6) struct Library *base, RTREG(a1) CONST_STRPTR login)
{
	UG;
	if (login && !rt_strcasecmp((const char *)login, amiga_user_name))
		return (ULONG)the_user(u);
	ug_seterr(u, RT_ENOENT);
	return 0;
}

ULONG ug_getpwuid(RTREG(a6) struct Library *base, RTREG(d0) LONG uid)
{
	UG;
	if (uid == amiga_user_uid)
		return (ULONG)the_user(u);
	ug_seterr(u, RT_ENOENT);
	return 0;
}

void ug_setpwent(RTREG(a6) struct Library *base) { UG; u->pwpos = 0; }
void ug_endpwent(RTREG(a6) struct Library *base) { UG; u->pwpos = 0; }

ULONG ug_getpwent(RTREG(a6) struct Library *base)
{
	UG;
	if (u->pwpos++ == 0)
		return (ULONG)the_user(u);
	return 0;
}

ULONG ug_getgrnam(RTREG(a6) struct Library *base, RTREG(a1) CONST_STRPTR name)
{
	UG;
	if (name && !rt_strcasecmp((const char *)name, group_name))
		return (ULONG)the_group(u);
	ug_seterr(u, RT_ENOENT);
	return 0;
}

ULONG ug_getgrgid(RTREG(a6) struct Library *base, RTREG(d0) LONG gid)
{
	UG;
	if (gid == amiga_user_gid)
		return (ULONG)the_group(u);
	ug_seterr(u, RT_ENOENT);
	return 0;
}

void ug_setgrent(RTREG(a6) struct Library *base) { UG; u->grpos = 0; }
void ug_endgrent(RTREG(a6) struct Library *base) { UG; u->grpos = 0; }

ULONG ug_getgrent(RTREG(a6) struct Library *base)
{
	UG;
	if (u->grpos++ == 0)
		return (ULONG)the_group(u);
	return 0;
}

/* Placeholder for crypt(3): NOT DES. FNV-1a over salt and key, printed
 * with the crypt alphabet in the classic 13 character format. */
ULONG ug_crypt(RTREG(a6) struct Library *base, RTREG(a0) CONST_STRPTR key, RTREG(a1) CONST_STRPTR salt)
{
	UG;
	static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	u32 h1 = 0x811c9dc5u, h2 = 0x01000193u;
	const char *k = key ? (const char *)key : "";
	char s0 = salt && salt[0] ? salt[0] : '.', s1 = salt && salt[0] && salt[1] ? salt[1] : '.';
	int i, round;

	for (round = 0; round < 25; round++) {
		h1 = (h1 ^ (u8)s0) * 0x01000193u;
		h1 = (h1 ^ (u8)s1) * 0x01000193u;
		for (i = 0; k[i] && i < 8; i++) {
			h1 = (h1 ^ (u8)k[i]) * 0x01000193u;
			h2 = (h2 ^ (h1 >> 7)) * 0x2c1b3c6du;
		}
		h2 ^= h1 + (u32)round;
	}
	u->cryptbuf[0] = s0;
	u->cryptbuf[1] = s1;
	for (i = 0; i < 11; i++) {
		u32 v = i < 5 ? h1 >> (i * 6) : h2 >> ((i - 5) * 5);
		u->cryptbuf[2 + i] = abc[v & 63];
	}
	u->cryptbuf[13] = 0;
	return (ULONG)u->cryptbuf;
}

ULONG ug_ug_GetSalt(RTREG(a6) struct Library *base, RTREG(a0) APTR user, RTREG(a1) STRPTR buf,
                    RTREG(d0) ULONG size)
{
	const struct passwd *pw = user;
	const char *p = pw && pw->pw_passwd && strlen(pw->pw_passwd) >= 2 ? pw->pw_passwd : "Ri";

	if (!buf || size < 3)
		return 0;
	buf[0] = p[0];
	buf[1] = p[1];
	buf[2] = 0;
	return (ULONG)buf;
}

ULONG ug_getpass(RTREG(a6) struct Library *base, RTREG(a1) CONST_STRPTR prompt)
{
	UG;
	struct Task *me = FindTask(NULL);
	LONG n = 0;
	BPTR in, out;

	/* Only a process has a console; this runs in the caller's context. */
	if (me->tc_Node.ln_Type != NT_PROCESS)
		return 0;
	out = Output();
	in = ((struct Process *)me)->pr_CIS;
	if (prompt && out)
		Write(out, (APTR)prompt, (LONG)strlen((const char *)prompt));
	if (!in)
		return 0;
	while (n < (LONG)sizeof(u->passbuf) - 1) {
		char ch;
		if (Read(in, &ch, 1) != 1 || ch == '\n' || ch == '\r')
			break;
		u->passbuf[n++] = ch;
	}
	u->passbuf[n] = 0;
	return (ULONG)u->passbuf;
}

ULONG ug_umask(RTREG(a6) struct Library *base, RTREG(d0) ULONG mask)
{
	UG;
	ULONG old = u->umask_;

	u->umask_ = mask & 0777;
	return old;
}

ULONG ug_getumask(RTREG(a6) struct Library *base)
{
	UG;
	return u->umask_;
}

LONG ug_setsid(RTREG(a6) struct Library *base)
{
	return (LONG)FindTask(NULL);
}

LONG ug_getpgrp(RTREG(a6) struct Library *base)
{
	return (LONG)FindTask(NULL);
}

ULONG ug_getlogin(RTREG(a6) struct Library *base)
{
	UG;
	return (ULONG)u->login;
}

LONG ug_setlogin(RTREG(a6) struct Library *base, RTREG(a1) CONST_STRPTR name)
{
	UG;
	if (!name || strlen((const char *)name) >= sizeof(u->login)) {
		ug_seterr(u, RT_EINVAL);
		return -1;
	}
	rt_strlcpy(u->login, (const char *)name, sizeof(u->login));
	return 0;
}

void ug_setutent(RTREG(a6) struct Library *base) { }
void ug_endutent(RTREG(a6) struct Library *base) { }
ULONG ug_getutent(RTREG(a6) struct Library *base) { return 0; }
ULONG ug_getlastlog(RTREG(a6) struct Library *base, RTREG(d0) LONG uid) { return 0; }

LONG ug_setlastlog(RTREG(a6) struct Library *base, RTREG(d0) LONG uid, RTREG(a0) STRPTR name,
                   RTREG(a1) STRPTR host)
{
	return 0;
}

ULONG ug_getcredentials(RTREG(a6) struct Library *base, RTREG(a0) APTR task)
{
	UG;
	LONG i;

	memset(&u->cred, 0, sizeof(u->cred));
	u->cred.cr_ruid = u->ruid;
	u->cred.cr_rgid = u->rgid;
	u->cred.cr_umask = (UWORD)u->umask_;
	u->cred.cr_euid = u->euid;
	u->cred.cr_ngroups = (WORD)u->ngroups;
	for (i = 0; i < u->ngroups; i++)
		u->cred.cr_groups[i] = u->groups[i];
	u->cred.cr_session = task ? (struct Task *)task : FindTask(NULL);
	rt_strlcpy(u->cred.cr_login, u->login, sizeof(u->cred.cr_login));
	return (ULONG)&u->cred;
}

ULONG ug_libopen(RTREG(a6) struct Library *base, RTREG(d0) ULONG version)
{
	return (ULONG)rtlib_open(&rtlib_usergroup, base);
}

ULONG ug_libclose(RTREG(a6) struct Library *base)
{
	return rtlib_close((struct RtBase *)base);
}
