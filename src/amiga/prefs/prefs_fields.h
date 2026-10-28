/*
 * Riptide - RiptidePrefs: the pages and settings, once for both editors.
 *
 * Every setting the editors show is a line of rp_fields[]: its page, kind
 * (string, number, check box, cycle), where it lives in struct rp_model,
 * its limits, its label (a '_' marks the keyboard shortcut) and a help
 * text for the bubble. The GadTools editor (riptideprefs.c) and the MUI
 * editor (mui_main.c) both build their pages from this table, so a new
 * setting is one line here.
 *
 * Portable C (no AmigaOS calls): host-tested by tests/t_prefs_conf.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_FIELDS_H
#define RT_PREFS_FIELDS_H

#include "conf_edit.h"

enum {
	P_IFACE, P_ADDR, P_ALIAS, P_VLAN, P_NAMES, P_TIME, P_TCP, P_CONN, P_PORTS, P_KA, P_BG, P_MEM, P_HE,
	P_QOS, P_QRULE, P_OTHER, RP_NPAGES
};
#define RP_FIRST_ADVANCED P_CONN    /* pages from here on are "Advanced" */

struct rp_page {
	const char *name;       /* "Advanced: Ports" (the GadTools cycle) */
	const char *title;      /* "Ports" (the MUI page list and head) */
	const char *note;       /* one line under the settings */
	const char *help;       /* what the page is about (bubble, head) */
};
extern const struct rp_page rp_pages[RP_NPAGES];

/* the list behind a list page (RP_LIST_*), -1 for none; RP_LIST_OTHER is
 * the "Other lines" page (lines the editors do not interpret) */
#define RP_LIST_OTHER 99
int rp_page_list(int page);

enum { F_STR, F_NUM, F_CHECK, F_CYCLE };
#define FL_STATIC 1             /* disabled with DHCP */
#define FL_NTP    2             /* disabled without NTP */
#define FL_ZERO   4             /* F_NUM: 0 is allowed below min */
#define FL_RIGHT  8             /* F_CHECK: GadTools: on the previous row, at the right */
#define FL_DHCP   16            /* disabled with a static address */

struct rp_field {
	unsigned char page, type, flags;
	unsigned short off, size;   /* in struct rp_model; size of the string or number */
	long min, max;
	const char *label;          /* '_' marks the keyboard shortcut */
	const char *const *cy;      /* F_CYCLE: labels and values */
	const long *cyv;
	const char *help;           /* one or two plain sentences: what it does */
};
extern const struct rp_field rp_fields[];
extern const int rp_nfields;
#define RP_NFIELDS_MAX 80       /* for per-field arrays of the editors (checked by the host test) */

/* values in the model */
long rp_fget(const struct rp_model *m, int fi);
void rp_fset(struct rp_model *m, int fi, long v);
char *rp_fstr(struct rp_model *m, int fi);
/* F_CYCLE: index of the current value in cy[] (0 when unknown) */
int  rp_fcycle(const struct rp_model *m, int fi);
/* disabled by another setting (DHCP, NTP) */
int  rp_fdisabled(const struct rp_model *m, int fi);
/* the label without '_', and its shortcut key in lower case (0: none) */
void rp_flabel(int fi, char *buf, int len);
char rp_fkey(int fi);
/* the field of a model address, -1 when none */
int  rp_find(const struct rp_model *m, const void *addr);

/* Take text typed into a string or number field into the model. Numbers:
 * empty is "not set"; out of range or not a number gives -1 and a message
 * ("enter 1..64, or leave it empty") in err. Strings lose surrounding
 * spaces. */
int  rp_fparse(struct rp_model *m, int fi, const char *text, char *err, int errlen);

/* Check the whole model: 0, or -1 with *fi the field to show and err the
 * reason ("enter an IPv4 address, e.g. ..."). */
int  rp_validate(const struct rp_model *m, int *fi, char *err, int errlen);

/* Check an entry of a list page before it is added: 1, or 0 with the
 * message to show in err. */
int  rp_list_entry_valid(int page, const char *s, char *err, int errlen);
/* the message when a list page is full */
const char *rp_list_full_msg(int page);

#endif
