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
	P_QOS, P_QRULE, P_OTHER,
	P_STATUS,                   /* live state, no settings (shown first) */
	RP_NPAGES
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

/* The forms of the list pages (the MUI editor): an entry of the Extra
 * addresses, VLANs or QoS rules list as fields of its own instead of one
 * line to type. rp_lfields[] holds them, page by page in order; values
 * are kept in struct rp_lvals by field index. */
enum { LF_NUM, LF_STR, LF_CYCLE, LF_INFO };   /* LF_INFO: shown, not edited */
#define LF_SAMEROW 1            /* next to the field before, on its row */
struct rp_lfield {
	unsigned char page, type, flags;
	unsigned char size;         /* LF_STR: longest text */
	long min, max;              /* LF_NUM */
	const char *label;          /* '_' marks the keyboard shortcut */
	const char *const *cy;      /* LF_CYCLE: the labels */
	const char *help;
};
/* the fields, in this order in rp_lfields[] */
enum {
	LA_ADDR, LA_MASK, LA_BCAST,                                     /* Extra addresses */
	LV_ID, LV_NAME, LV_MODE, LV_ADDR, LV_PCP, LV_MTU, LV_TCPWIN,     /* VLANs */
	LV_IP4, LV_IP6, LV_QOS,
	LQ_PROTO, LQ_PORTS, LQ_SIDE, LQ_PROG, LQ_PRIO, LQ_DSCP,          /* QoS rules */
	RP_NLFIELDS
};
extern const struct rp_lfield rp_lfields[];
extern const int rp_nlfields;
#define RP_NLFIELDS_MAX 24
#define RP_LFTEXT 64
struct rp_lvals {
	int cy[RP_NLFIELDS_MAX];                /* LF_CYCLE: the label chosen */
	char tx[RP_NLFIELDS_MAX][RP_LFTEXT];    /* LF_NUM, LF_STR, LF_INFO: the text */
};
/* An entry (NULL or "": a new one) into the form of its page. */
void rp_lform_get(int page, const char *entry, const char *ifname, struct rp_lvals *v);
/* The form into an entry: 0, or -1 with *fi the field to show and err why. */
int  rp_lform_put(int page, const struct rp_lvals *v, char *entry, int len, int *fi, char *err, int errlen);
/* a field that does not apply with the others' values (IPv4 address
 * unless Static) */
int  rp_lform_disabled(int fi, const struct rp_lvals *v);
/* LF_INFO fields again after a change (the VLAN's interface name) */
void rp_lform_update(int page, struct rp_lvals *v, const char *ifname);
/* an entry as the list shows it: "eth0.10: DHCP, priority 4, MTU 1400" */
void rp_list_row(int page, const char *entry, const char *ifname, char *buf, int len);
/* the line under a form: how many entries fit, what needs a restart */
const char *rp_lform_note(int page);
/* the label without '_', and its shortcut key in lower case (0: none) */
void rp_lflabel(int fi, char *buf, int len);
char rp_lfkey(int fi);

#endif
