/*
 * Riptide - RiptidePrefs: reading and writing riptide.conf, applying it
 * to the running stack, the list pages and requesters (see prefs_io.h).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include "cmd/cmdlib.h"
#include "prefs/prefs_io.h"

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
	rp_conf_free(&rp_cfg);
	r = rp_conf_load(&rp_cfg, text ? text : "", text ? (size_t)len : 0);
	if (text)
		FreeVec(text);
	return r;
}

int rp_store(int save)
{
	size_t len;
	char *text = rp_conf_text(&rp_cfg, &len);
	char msg[160];
	int r = 0;

	if (!text) {
		rp_message("Out of memory");
		return -1;
	}
	if (save && write_file(RP_ENVARC_CONF, text, len, 1) < 0) {
		rt_snprintf(msg, sizeof(msg), "Cannot write %s", RP_ENVARC_CONF);
		r = -1;
	} else if (write_file(RP_ENV_CONF, text, len, 0) < 0) {
		rt_snprintf(msg, sizeof(msg), "Cannot write %s", RP_ENV_CONF);
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

void rp_apply(int ask)
{
	struct apply_state st;
	char msg[600];
	int restart;

	if (!FindPort((CONST_STRPTR)RIPTIDE_PORTNAME)) {
		if (!ask)
			rp_message("Riptide is not running.\nThe settings take effect when it starts.");
		return;
	}
	if (ask && !rp_conf_changed(&rp_cfg))
		return;
	if (ask && !rp_request("Riptide is running.\nApply the changes to it now?", "Apply|Later"))
		return;
	memset(&st, 0, sizeof(st));
	restart = rp_live_commands(&rp_cfg, apply_one, &st);
	if (st.failed)
		rt_snprintf(msg, sizeof(msg), "Applied %d of %d settings.\n%s", st.n - st.failed, st.n, st.report);
	else if (st.n)
		rt_snprintf(msg, sizeof(msg), "Applied %d settings to Riptide.", st.n);
	else
		rt_snprintf(msg, sizeof(msg), "Nothing to apply.");
	if (restart)
		rt_strlcpy(msg + strlen(msg),
		           "\nInterface, address and IPv4/IPv6 changes, and\nsettings set back to Default, take effect\n"
		           "when Riptide is restarted.",
		           sizeof(msg) - strlen(msg));
	rp_message(msg);
}

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
