/*
 * Riptide - RiptidePrefs: reading and writing riptide.conf, applying it
 * to the running stack, the list pages and requesters (see prefs_io.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/prefs_io.h"
#include "prefs/prefs_status.h"

struct rp_conf rp_cfg;
const char *rp_from_path;
int rp_wb;
long (*rp_request_fn)(const char *text, const char *buttons);

void *rp_alloc(size_t n)
{
	return AllocVec((ULONG)n, MEMF_ANY);
}

void rp_free(void *p)
{
	if (p)
		FreeVec(p);
}

long rp_request(const char *text, const char *buttons)
{
	if (rp_request_fn)
		return rp_request_fn(text, buttons);
	if (!rp_wb) {
		PutStr((CONST_STRPTR)text);
		PutStr((CONST_STRPTR)"\n");
	}
	return 0;
}

void rp_message(const char *text)
{
	rp_request(text, "OK");
}

/* the whole file at path, 0-terminated, or NULL */
void *rp_read_file(const char *path, long *len)
{
	BPTR fh, lock;
	struct FileInfoBlock *fib;
	char *buf = NULL;
	LONG size = -1;

	lock = Lock((CONST_STRPTR)path, ACCESS_READ);
	if (!lock)
		return NULL;
	fib = AllocVec(sizeof(*fib), MEMF_ANY | MEMF_CLEAR);
	if (fib && Examine(lock, fib))
		size = fib->fib_Size;
	if (fib)
		FreeVec(fib);
	UnLock(lock);
	if (size < 0)
		return NULL;
	buf = AllocVec((ULONG)size + 1, MEMF_ANY);
	if (!buf)
		return NULL;
	fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	if (!fh || Read(fh, buf, size) != size) {
		if (fh)
			Close(fh);
		FreeVec(buf);
		return NULL;
	}
	Close(fh);
	buf[size] = 0;
	*len = size;
	return buf;
}

/* path = "VOL:Dir/file": create VOL:Dir when missing */
static void make_dir(const char *path)
{
	char dir[128];
	const char *slash = strrchr(path, '/');
	BPTR l;

	if (!slash || (size_t)(slash - path) >= sizeof(dir))
		return;
	memcpy(dir, path, (size_t)(slash - path));
	dir[slash - path] = 0;
	l = Lock((CONST_STRPTR)dir, ACCESS_READ);
	if (!l)
		l = CreateDir((CONST_STRPTR)dir);
	if (l)
		UnLock(l);
}

/* write text to path; backup: keep the old file as path.bak */
static int write_file(const char *path, const char *text, size_t len, int backup)
{
	char bak[140];
	BPTR f;
	LONG n;

	make_dir(path);
	if (backup) {
		BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
		if (l) {
			UnLock(l);
			rt_snprintf(bak, sizeof(bak), "%s.bak", path);
			DeleteFile((CONST_STRPTR)bak);
			Rename((CONST_STRPTR)path, (CONST_STRPTR)bak);
		}
	}
	f = Open((CONST_STRPTR)path, MODE_NEWFILE);
	if (!f)
		return -1;
	n = len ? Write(f, (APTR)text, (LONG)len) : 0;
	Close(f);
	return n == (LONG)len ? 0 : -1;
}

int rp_load(void)
{
	char *text;
	long len = 0;
	int r;

	if (rp_from_path)
		text = rp_read_file(rp_from_path, &len);
	else if (!(text = rp_read_file(RP_ENV_CONF, &len)))
		text = rp_read_file(RP_ENVARC_CONF, &len);
	if (!text && rp_from_path) {
		char msg[200];
		rt_snprintf(msg, sizeof(msg), "Cannot read %s", rp_from_path);
		rp_message(msg);
		return -1;
	}
	{
		/* the stack still has what was last applied (Restore reloads) */
		struct rp_model *keep = rp_cfg.have_applied ? rp_alloc(sizeof(*keep)) : NULL;
		if (keep)
			*keep = rp_cfg.applied;
		rp_conf_free(&rp_cfg);
		r = rp_conf_load(&rp_cfg, text ? text : "", text ? (size_t)len : 0);
		if (keep) {
			rp_cfg.applied = *keep;
			rp_cfg.have_applied = 1;
			rp_free(keep);
		}
	}
	if (text)
		FreeVec(text);
	return r;
}

#ifdef MM_MAC_HIG
const char *mm_os_errtext(LONG err);
void mm_os_macpath(const char *amiga, char *out, int len);
#endif
/* "Cannot write PATH", and on the Mac its full path and the File Manager's reason */
static void cannot_write(char *msg, size_t size, const char *path)
{
#ifdef MM_MAC_HIG
	char mp[256];
	LONG e = IoErr();

	mm_os_macpath(path, mp, sizeof(mp));
	rt_snprintf(msg, size, "The settings were not saved.\nCannot write %s:\n%s (error %ld).", mp, mm_os_errtext(e),
	            (long)e);
#else
	rt_snprintf(msg, size, "Cannot write %s", path);
#endif
}

int rp_store(int save)
{
	size_t len;
	char *text = rp_conf_text(&rp_cfg, &len);
	char msg[400];
	int r = 0;

	if (!text) {
		rp_message("Out of memory");
		return -1;
	}
	if (save && write_file(RP_ENVARC_CONF, text, len, 1) < 0) {
		cannot_write(msg, sizeof(msg), RP_ENVARC_CONF);
		r = -1;
	} else if (write_file(RP_ENV_CONF, text, len, 0) < 0) {
		cannot_write(msg, sizeof(msg), RP_ENV_CONF);
		r = -1;
	}
	rp_free(text);
	if (r < 0)
		rp_message(msg);
	return r;
}

/* ---- apply to the running stack ---- */
struct apply_state {
	int n, failed;
	char report[400];
};

static void apply_one(const char *line, void *arg)
{
	struct apply_state *st = arg;
	size_t l = strlen(st->report);
	int r = cl_command(line, 0);

	st->n++;
	if (r) {
		const char *rep = cl_reply();
		st->failed++;
		if (l < sizeof(st->report) - 2)
			rt_snprintf(st->report + l, sizeof(st->report) - l, "%s: %s\n", line,
			            rep && rep[0] ? rep : "failed");
	}
}

/* an Apply left changes that need a restart for later: the applied
 * baseline no longer shows them, so remember */
static int restart_owed;

static void count_one(const char *line, void *arg)
{
	(*(int *)arg)++;
}

static int contains(const char *s, const char *word)
{
	size_t n = strlen(word);

	for (; *s; s++)
		if (!strncmp(s, word, n))
			return 1;
	return 0;
}

/* RTCMD_RESTART with the file rp_store() wrote, and its answer shown */
static void restart_now(void)
{
	char msg[600];
	const char *rep;
	int r = cl_request(RTCMD_RESTART, RP_ENV_CONF, 0);

	rep = cl_reply();
	if (!rep)
		rep = "";
	if (r == 0 || contains(rep, "restarted")) {
		/* the stack has what the file says, errors or not */
		rp_conf_applied(&rp_cfg);
		restart_owed = 0;
		rt_snprintf(msg, sizeof(msg), "%s", rep[0] ? rep : "Riptide restarted.");
	} else if (r < 0) {
		rt_snprintf(msg, sizeof(msg), "Riptide is not running.\nThe settings take effect when it starts.");
	} else if (contains(rep, "unknown command")) {
		rt_snprintf(msg, sizeof(msg), "This Riptide cannot restart its network (0.39 or later can).\n"
		            "The settings take effect when it starts again.");
	} else {
		rt_snprintf(msg, sizeof(msg), "Riptide did not restart:\n%s", rep[0] ? rep : cl_strerror(r));
	}
	rp_message(msg);
}

void rp_apply(int ask)
{
	struct apply_state st;
	char msg[600];
	int restart, n = 0;

	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		if (!ask)
			rp_message("Riptide is not running.\nThe settings take effect when it starts.");
		return;
	}
	/* after an Apply: only what differs from what was applied */
	if (ask && !restart_owed && !(rp_cfg.have_applied ? rp_live_pending(&rp_cfg) : rp_conf_changed(&rp_cfg)))
		return;
	if (rp_live_commands(&rp_cfg, count_one, &n) || restart_owed) {
		/* Use and Save have written the file; Apply writes ENV: first */
		if (rp_request(ask ? "Riptide is running. Some of the changes need a restart of its\n"
		                     "network: the programs keep running, and connections over an\n"
		                     "address that goes away are closed.\nRestart Riptide's network now?" :
		                     "Some of the changes need a restart of Riptide's network: the\n"
		                     "programs keep running, and connections over an address that goes\n"
		                     "away are closed. Restart now saves the settings to ENV: (as Use\n"
		                     "does) and restarts it.",
		               "Restart now|Later")) {
			if (!ask && rp_store(0) < 0)
				return;
			restart_now();
			return;
		}
		if (ask)
			return;            /* the file is saved: a later restart takes it */
	} else if (ask && !rp_request("Riptide is running.\nApply the changes to it now?", "Apply|Later")) {
		return;
	}
	memset(&st, 0, sizeof(st));
	restart = rp_live_commands(&rp_cfg, apply_one, &st) || restart_owed;
	rp_conf_applied(&rp_cfg);
	restart_owed = restart;
	if (st.failed)
		rt_snprintf(msg, sizeof(msg), "Applied %d of %d setting%s.\n%s", st.n - st.failed, st.n,
		            rp_plural((unsigned long)st.n), st.report);
	else if (st.n)
		rt_snprintf(msg, sizeof(msg), "Applied %d setting%s to Riptide.", st.n, rp_plural((unsigned long)st.n));
	else
		rt_snprintf(msg, sizeof(msg), "Nothing to apply.");
	if (restart)
		rt_strlcpy(msg + strlen(msg),
		           "\nInterface, address and IPv4/IPv6 changes, and\nsettings set back to Default, take effect\n"
		           "when Riptide's network restarts (Apply, then\nRestart now; or Riptide RESTART).",
		           sizeof(msg) - strlen(msg));
	rp_message(msg);
}

#ifdef MM_MAC_HIG
/* the Mac: what an Apply left waiting for a restart of the Mac, in words */
static char owed_what[100];
static int owed_unsaved;        /* some of it was only applied, not saved yet */

static void owe(const char *what)
{
	size_t k = strlen(owed_what);

	if (what[0] && !contains(owed_what, what))
		rt_snprintf(owed_what + k, sizeof(owed_what) - k, "%s%s", k ? ", " : "", what);
}

/* rp_live_commands' line for a card turned off ("interface en0 delete"; a VLAN's has a dot) */
static int card_off_line(const char *line)
{
	size_t n = strlen(line);

	return !strncmp(line, "interface ", 10) && n > 17 && !strcmp(line + n - 7, " delete") && !strchr(line + 10, '.');
}

/* apply_one, but a card turned off ("interface en0 delete") waits for the Mac's next start: a
 * Riptide for the Mac older than 23 November 2026 cannot close a card while it runs (a newer one
 * restarts its network instead, mac_restart_net) */
static void mac_apply_one(const char *line, void *arg)
{
	struct apply_state *st = arg;
	size_t n = strlen(line);
	int failed = st->failed;

	if (card_off_line(line)) {
		owe("a card turned off");
		return;
	}
	apply_one(line, arg);
	/* "nameserver flush" before the new servers: one change, not two */
	if (n > 6 && !strcmp(line + n - 6, " flush") && st->failed == failed)
		st->n--;
}

/* The Mac's Riptide restarts its network while the Mac runs (23 November 2026): the In Use copy of
 * riptide.conf (which Save writes too) is read again, the cards that stay kept open. 1 when it
 * did, and the status line says so; 0 when this Riptide cannot (an older one), the line untouched. */
static int mac_restart_net(int saved, const char *what, char *line, int len)
{
	char msg[600];
	const char *rep, *e;
	int r;

	if (!saved && rp_store(0) < 0) {
		rt_snprintf(line, (size_t)len, "Not applied: the In Use folder's riptide.conf could not be written.");
		return 1;
	}
	r = cl_request(RTCMD_RESTART, RP_ENV_CONF, 0);
	rep = cl_reply();
	if (!rep)
		rep = "";
	if (r != 0 && !contains(rep, "restarted")) {
		if (contains(rep, "unknown command"))
			return 0;
		rt_snprintf(msg, sizeof(msg), "Riptide did not restart its network:\n%s", rep[0] ? rep : cl_strerror(r));
		rp_message(msg);
		rt_snprintf(line, (size_t)len, "%s", saved ? "Saved; Riptide's network did not restart." :
		            "Not applied: Riptide's network did not restart.");
		return 1;
	}
	/* the stack has what the file says, the lines it refused aside */
	rp_conf_applied(&rp_cfg);
	owed_what[0] = 0;
	owed_unsaved = 0;
	for (e = rep; *e && strncmp(e, "riptide.conf: ", 14); e++)
		;
	if (*e) {
		rt_snprintf(msg, sizeof(msg), "Riptide restarted its network, but refused:\n%s", e);
		rp_message(msg);
	}
	rt_snprintf(line, (size_t)len, "%s; Riptide's network restarted for: %s.", saved ? "Saved" : "Applied", what);
	return 1;
}

static void find_card_off(const char *line, void *arg)
{
	if (card_off_line(line))
		*(int *)arg = 1;
}

void rp_apply_mac(int saved, char *line, int len)
{
	struct apply_state st;
	char what[100], msg[600];
	size_t k;
	int restart, off = 0;

	rp_restart_what(&rp_cfg, what, sizeof(what));
	restart = rp_live_commands(&rp_cfg, find_card_off, &off);
	if (off && !contains(what, "the cards"))
		rt_snprintf(what + strlen(what), sizeof(what) - strlen(what), "%sa card turned off", what[0] ? ", " : "");
	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		owe(what);
		if (saved)
			owed_unsaved = 0;
		else if (what[0])
			owed_unsaved = 1;
		rt_snprintf(line, (size_t)len, "%s", saved ? "Saved. Riptide is not running: it takes them when the Mac starts." :
		            "Not applied: Riptide is not running. Save, and it takes them when the Mac starts.");
		return;
	}
	if (restart || off || what[0] || owed_what[0]) {
		/* what the restart is for: this change's, and what an older Apply left waiting */
		char all[100];
		rt_strlcpy(all, owed_what, sizeof(all));
		if (what[0] && !contains(all, what))
			rt_snprintf(all + strlen(all), sizeof(all) - strlen(all), "%s%s", all[0] ? ", " : "", what);
		if (!all[0])
			rt_strlcpy(all, "some settings", sizeof(all));
		if (mac_restart_net(saved, all, line, len))
			return;
	}
	owe(what);
	if (saved)
		owed_unsaved = 0;
	else if (what[0])
		owed_unsaved = 1;
	memset(&st, 0, sizeof(st));
	k = strlen(owed_what);
	restart = rp_live_commands(&rp_cfg, mac_apply_one, &st);
	if (!saved && strlen(owed_what) != k)
		owed_unsaved = 1;               /* a card turned off, only applied */
	rp_conf_applied(&rp_cfg);
	if (restart && !owed_what[0])
		rt_strlcpy(owed_what, "some settings", sizeof(owed_what));
	if (st.failed) {
		rt_snprintf(msg, sizeof(msg), "Riptide did not take %d of the %d change%s:\n%s", st.failed, st.n,
		            rp_plural((unsigned long)st.n), st.report);
		rp_message(msg);
	}
	if (!st.n)
		k = (size_t)rt_snprintf(line, (size_t)len, "%s", saved ? "Saved" : "Nothing to apply");
	else if (saved)
		k = (size_t)rt_snprintf(line, (size_t)len, "Saved; Riptide took %d change%s", st.n - st.failed,
		                        rp_plural((unsigned long)(st.n - st.failed)));
	else
		k = (size_t)rt_snprintf(line, (size_t)len, "Applied %d change%s", st.n - st.failed,
		                        rp_plural((unsigned long)(st.n - st.failed)));
	if (st.failed && k < (size_t)len)
		k += (size_t)rt_snprintf(line + k, (size_t)len - k, " (%d refused)", st.failed);
	if (k < (size_t)len)
		rt_snprintf(line + k, (size_t)len - k, owed_what[0] ? (owed_unsaved ? ". Save and restart the Mac for: %s." :
		                                                      ". Restart the Mac for: %s.") : ".",
		            owed_what);
}
#endif

/* ---- list pages ---- */
int rp_list_n(int page)
{
	int k = rp_page_list(page);
	return k == RP_LIST_OTHER ? rp_other_count(&rp_cfg) : k >= 0 ? rp_list_count(&rp_cfg, k) : 0;
}

const char *rp_list_at(int page, int i)
{
	int k = rp_page_list(page);
	return k == RP_LIST_OTHER ? rp_other_get(&rp_cfg, i) : k >= 0 ? rp_list_get(&rp_cfg, k, i) : NULL;
}

int rp_list_edit(int page, int sel, const char *text)
{
	int k = rp_page_list(page);
	char err[300];

	if (k < 0)
		return -1;
	if (!text) {
		if (sel < 0)
			return -1;
		if (k == RP_LIST_OTHER)
			rp_other_set(&rp_cfg, sel, "");
		else
			rp_list_set(&rp_cfg, k, sel, "");
		return -1;
	}
	if (!text[0])
		return sel;
	if (!rp_list_entry_valid(page, text, err, sizeof(err))) {
		rp_message(err);
		return -2;
	}
	if (sel >= 0) {
		if (k == RP_LIST_OTHER)
			rp_other_set(&rp_cfg, sel, text);
		else
			rp_list_set(&rp_cfg, k, sel, text);
		return sel;
	}
	if (k == RP_LIST_OTHER) {
		rp_other_add(&rp_cfg, text);
	} else if (rp_list_add(&rp_cfg, k, text) < 0) {
		rp_message(rp_list_full_msg(page));
		return -2;
	}
	return rp_list_n(page) - 1;
}

/* ---- the debug log ---- */
static BPTR logf;

void rp_log_open(const char *path)
{
	if (path)
		logf = Open((CONST_STRPTR)path, MODE_NEWFILE);
}

void rp_log_close(void)
{
	if (logf)
		Close(logf);
	logf = 0;
}

void rp_log(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	int n;

	if (!logf)
		return;
	va_start(ap, fmt);
	n = rt_vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	Write(logf, buf, n);
}

/* ---- the editor's own settings ---- */
#define EPREFS_PATH    "ENVARC:Riptide/RiptidePrefs.prefs"
#define EPREFS_ENVPATH "ENV:Riptide/RiptidePrefs.prefs"
#define EPREFS_MAX 8

static struct { char key[16]; int v; } eprefs[EPREFS_MAX];
static int neprefs = -1;

/* read the file once */
static void eprefs_load(void)
{
	char *t, *p;
	long len;

	if (neprefs >= 0)
		return;
	neprefs = 0;
	if (!(t = rp_read_file(EPREFS_ENVPATH, &len)) && !(t = rp_read_file(EPREFS_PATH, &len)))
		return;
	for (p = t; *p && neprefs < EPREFS_MAX;) {
		char *eq = p, *nl;
		const char *end;
		while (*eq && *eq != '=' && *eq != '\n')
			eq++;
		nl = eq;
		while (*nl && *nl != '\n')
			nl++;
		if (*eq == '=' && eq - p > 0 && eq - p < 16) {
			memcpy(eprefs[neprefs].key, p, (size_t)(eq - p));
			eprefs[neprefs].key[eq - p] = 0;
			eprefs[neprefs].v = (int)rt_strtoul(eq + 1, &end, 10);
			neprefs++;
		}
		p = *nl ? nl + 1 : nl;
	}
	FreeVec(t);
}

int rp_eprefs_get(const char *key, int def)
{
	int i;

	eprefs_load();
	for (i = 0; i < neprefs; i++)
		if (!strcmp(eprefs[i].key, key))
			return eprefs[i].v;
	return def;
}

void rp_eprefs_set(const char *key, int v)
{
	int i;

	eprefs_load();
	for (i = 0; i < neprefs; i++)
		if (!strcmp(eprefs[i].key, key))
			break;
	if (i == neprefs) {
		if (neprefs >= EPREFS_MAX)
			return;
		rt_strlcpy(eprefs[i].key, key, sizeof(eprefs[i].key));
		neprefs++;
	}
	eprefs[i].v = v;
}

void rp_eprefs_save(void)
{
	char buf[EPREFS_MAX * 24];
	size_t o = 0;
	int i;

	eprefs_load();
	for (i = 0; i < neprefs; i++)
		o += (size_t)rt_snprintf(buf + o, sizeof(buf) - o, "%s=%d\n", eprefs[i].key, eprefs[i].v);
	write_file(EPREFS_PATH, buf, o, 0);
	write_file(EPREFS_ENVPATH, buf, o, 0);
}

/* ---- one editor at a time ---- */
static struct MsgPort *single_port;
static char single_name[] = RP_PORTNAME;

int rp_single_start(void)
{
	struct MsgPort *other;
	struct Message *m;

	m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
	single_port = CreateMsgPort();
	/* looking and adding in one go: of two copies started at once, one
	 * makes the port and the other finds it */
	Forbid();
	other = FindPort((CONST_STRPTR)single_name);
	if (other && m) {
		m->mn_Node.ln_Type = NT_MESSAGE;
		m->mn_Length = sizeof(*m);
		PutMsg(other, m);       /* the other editor frees it */
		m = NULL;
	} else if (!other && single_port) {
		single_port->mp_Node.ln_Name = single_name;
		single_port->mp_Node.ln_Pri = 0;
		AddPort(single_port);
	}
	Permit();
	rp_free(m);
	if (other) {
		if (single_port)
			DeleteMsgPort(single_port);
		single_port = NULL;
		rp_log("another RiptidePrefs is open: asked it to come to the front");
		return 1;
	}
	return 0;
}

int rp_single_poll(void)
{
	struct Message *m;
	int n = 0;

	if (!single_port)
		return 0;
	while ((m = GetMsg(single_port)) != NULL) {
		FreeVec(m);
		n = 1;
	}
	if (n)
		rp_log("another RiptidePrefs was started: to the front");
	return n;
}

unsigned long rp_single_sig(void)
{
	return single_port ? 1UL << single_port->mp_SigBit : 0;
}

void rp_single_end(void)
{
	if (!single_port)
		return;
	Forbid();
	RemPort(single_port);
	Permit();
	rp_single_poll();       /* messages that came before RemPort() */
	DeleteMsgPort(single_port);
	single_port = NULL;
}
