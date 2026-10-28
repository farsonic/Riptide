/*
 * Riptide - RiptidePrefs: a model of riptide.conf that can be edited and
 * written back without disturbing what it does not understand.
 *
 * The file is kept as a list of lines. Lines the editor understands
 * (the first interface line, dhcp/ifconfig/route and "ifconfig IF alias"
 * for that interface, nameserver, hostname, domain, ipv4, ipv6, ntp,
 * timezone, mdns, "qos on|off", "qos rule" and the sysctls of the table
 * below) are parsed into struct rp_model; everything else - comments,
 * blank lines, unknown keywords - stays as it is. rp_conf_text() with an
 * unchanged model gives back the file byte for byte; with a changed model
 * only the lines of the changed settings are rewritten, inserted or
 * removed.
 *
 * Portable C (no AmigaOS calls): built into RiptidePrefs and into the host
 * test tests/t_prefs_conf.c. The program provides rp_alloc()/rp_free().
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_PREFS_CONF_EDIT_H
#define RT_PREFS_CONF_EDIT_H

#include <stddef.h>

/* Keywords of the time and name service settings. Riptide's core defines
 * them; they live here in one place so the editor follows the core. */
#define RP_KW_NTP       "ntp"         /* ntp SERVER [SERVER...] | ntp on | ntp off */
#define RP_KW_NTP_ON    "on"          /* servers from DHCP, else pool.ntp.org */
#define RP_KW_NTP_OFF   "off"
#define RP_KW_TIMEZONE  "timezone"    /* timezone [+-]HH[:MM] | auto (Locale prefs) */
#define RP_KW_MDNS      "mdns"        /* mdns on | mdns off */
#define RP_KW_IPV6      "ipv6"        /* ipv6 on | ipv6 off (default on) */
#define RP_KW_IPV4      "ipv4"        /* ipv4 on | ipv4 off (default on) */
#define RP_KW_QOS       "qos"         /* qos on|off; qos rule ... */

#define RP_STR   64
#define RP_ADDR  48
#define RP_NDNS  3
#define RP_LONG  160

/* "not set": numbers absent from the file */
#define RP_UNSET (-1L)

/* Sysctls edited as "sysctl NAME=VALUE" lines (Phase 3 settings; the
 * names, ranges and defaults are those of the core, see docs/AMIGA.md).
 * Numbers in rp_model.sc[] (RP_UNSET: no line), strings in ss[] ("": no
 * line). RP_SC_CC is a string sysctl kept as an index of rp_cc_names. */
enum {
	RP_SC_FASTOPEN, RP_SC_FASTOPEN_ALL, RP_SC_FO_BLACKHOLE, RP_SC_SYN_RTO_INIT, RP_SC_SYN_RTO_MAX,
	RP_SC_SYN_BACKOFF, RP_SC_CONNECT_TIMEOUT, RP_SC_SYN_ICMP_ABORT, RP_SC_TW_REUSE, RP_SC_MAX_TW,
	RP_SC_PORT_FIRST, RP_SC_PORT_LAST, RP_SC_KA_ALL, RP_SC_KEEPIDLE, RP_SC_KEEPINTVL, RP_SC_KEEPCNT,
	RP_SC_CC, RP_SC_LEDBAT_TARGET, RP_SC_LEDBAT_SLOWDOWN, RP_SC_BG_MODE, RP_SC_MEM_LOW,
	RP_SC_MEM_PRESSURE, RP_SC_MEM_HIGH, RP_SC_HE_ENABLE, RP_SC_HE_DELAY, RP_SC_HE_RESDELAY,
	RP_SC_HE_PREFER, RP_SC_HE_AAAA, RP_SC_QOS_INFLIGHT, RP_SC_QOS_LIMIT, RP_SC_QOS_PRIODSCP,
	RP_SC_DNS_EDNS, RP_SC_DNS_TCP, RP_SC_MTU_PROBING, RP_SC_DNS_NEGTTL, RP_SC_ECN, RP_NSC
};
enum { RP_SS_BG_PROGRAMS, RP_SS_BG_PORTS, RP_SS_BG_LPORTS, RP_NSS };
#define RP_SSLEN 128
extern const char *const rp_sc_names[RP_NSC];
extern const char *const rp_ss_names[RP_NSS];
extern const char *const rp_cc_names[];     /* "cubic", "reno", "ledbat" */

/* lists: extra IPv4 addresses ("ifconfig IF alias SPEC"), QoS rules
 * ("qos rule SPEC") and VLANs of the interface; entries are packed (no
 * empty ones in between). A VLAN entry is "ID [dhcp|A.B.C.D/BITS|up]
 * [pcp=N|pcp=priority]" for the line "interface IF.ID vlan=ID parent=IF
 * [dhcp|address=A.B.C.D/BITS|up] [pcp=...]". */
#define RP_NALIAS   8
#define RP_ALIASLEN 64
#define RP_NQRULE   16
#define RP_NVLAN    8
#define RP_VLANLEN  48
enum { RP_LIST_ALIAS, RP_LIST_QRULE, RP_LIST_VLAN };

struct rp_model {
	/* interface (the first "interface NAME device=..." line) */
	char ifname[16];
	char device[RP_STR];
	long unit, mtu, reads, tcpwindow;     /* RP_UNSET when absent */
	int  raw;
	char ifextra[RP_LONG];                /* other options, verbatim */
	/* IPv4 address */
	int  dhcp;                            /* 1 dhcp, 0 static */
	int  linklocal;                       /* 1 (default), 0 "dhcp IF linklocal off" */
	char addr[RP_ADDR];                   /* A.B.C.D or A.B.C.D/BITS */
	char netmask[RP_ADDR];
	char gateway[RP_ADDR];
	char dns[RP_NDNS][RP_ADDR];
	/* names */
	char hostname[RP_STR];
	char domain[RP_STR];
	/* IPv6, time, mDNS */
	int  ipv6;                            /* 1 on (default), 0 off */
	int  ntp;                             /* 0 off/not configured, 1 on */
	char ntp_servers[RP_LONG];            /* space separated */
	char timezone[RP_STR];
	int  mdns;                            /* -1 not configured, 0 off, 1 on */
	/* TCP tuning (sysctl lines; RP_UNSET = not in the file) */
	long quickack, delack_segs, delack_ms;
	/* Phase 3 */
	int  ipv4;                            /* 1 on (default), 0 off */
	int  qos;                             /* "qos on|off": -1 no line, 0 off, 1 on */
	long sc[RP_NSC];                      /* sysctls, RP_UNSET = no line */
	char ss[RP_NSS][RP_SSLEN];            /* string sysctls, "" = no line */
	char alias[RP_NALIAS][RP_ALIASLEN];   /* "A.B.C.D/BITS" or "A.B.C.D netmask M" */
	char qrule[RP_NQRULE][RP_LONG];       /* the words after "qos rule" */
	char vlan[RP_NVLAN][RP_VLANLEN];      /* "ID MODE [pcp=N]" (see above) */
};

struct rp_line {
	struct rp_line *next, *prev;
	char *text;                           /* without the line end */
};

struct rp_conf {
	struct rp_line *head, *tail;
	int eol_crlf;                         /* lines ended with \r\n */
	int final_eol;                        /* the last line had a line end */
	struct rp_model orig, cur;
	/* where each setting lives (NULL: not in the file) */
	struct rp_line *l_iface, *l_dhcp, *l_ifconfig, *l_route, *l_dns[RP_NDNS];
	struct rp_line *l_hostname, *l_domain, *l_ipv6, *l_ntp, *l_tz, *l_mdns;
	struct rp_line *l_quickack, *l_delack_segs, *l_delack_ms;
	struct rp_line *l_ipv4, *l_qos, *l_sc[RP_NSC], *l_ss[RP_NSS];
	struct rp_line *l_alias[RP_NALIAS], *l_qrule[RP_NQRULE], *l_vlan[RP_NVLAN];
	/* what the list lines say now (conf_edit.c's sync_list()) */
	char h_alias[RP_NALIAS][RP_ALIASLEN];
	char h_qrule[RP_NQRULE][RP_LONG];
	char h_vlan[RP_NVLAN][RP_VLANLEN];
	char h_ifname[16];
	char h_vifname[16];                   /* the parent named in the VLAN lines */
};

/* provided by the program */
void *rp_alloc(size_t n);
void rp_free(void *p);

/* Parse text (len bytes; need not be terminated). Defaults when the file
 * is empty: interface eth0 on uaenet.device unit 0, DHCP. 0 or -1. */
int  rp_conf_load(struct rp_conf *c, const char *text, size_t len);
void rp_conf_free(struct rp_conf *c);
/* The file for the current model, allocated with rp_alloc(); *len gets
 * its length. NULL when out of memory. */
char *rp_conf_text(struct rp_conf *c, size_t *len);
/* Did the model change since loading (or the last rp_conf_commit())? */
int  rp_conf_changed(const struct rp_conf *c);
/* After saving: the current model becomes the original one. */
void rp_conf_commit(struct rp_conf *c);

/* Lines the editor does not interpret (not comments or blank lines):
 * numbered 0..n-1 in file order, for the "Other lines" list. */
int  rp_other_count(const struct rp_conf *c);
const char *rp_other_get(const struct rp_conf *c, int i);
int  rp_other_set(struct rp_conf *c, int i, const char *text);   /* "" deletes */
int  rp_other_add(struct rp_conf *c, const char *text);

/* The lists of the current model (RP_LIST_*): count, entry, replace
 * ("" deletes; the rest moves up), append (-1 when full or empty). */
int  rp_list_count(const struct rp_conf *c, int which);
const char *rp_list_get(const struct rp_conf *c, int which, int i);
int  rp_list_set(struct rp_conf *c, int which, int i, const char *text);
int  rp_list_add(struct rp_conf *c, int which, const char *text);

/* Validation: 1 if valid. */
int  rp_valid_ip4(const char *s, int allow_prefix);
int  rp_valid_name(const char *s);         /* host or domain name */
int  rp_valid_ip(const char *s);           /* IPv4 or IPv6 literal */
int  rp_valid_tz(const char *s);           /* "auto" or [+-]HH[:MM] (timezone) */
int  rp_valid_alias(const char *s);        /* A.B.C.D[/BITS] [netmask M] [broadcast B] */
int  rp_valid_qrule(const char *s);        /* key=value words of "qos rule" */
int  rp_valid_vlan(const char *s);         /* "ID [dhcp|A.B.C.D/BITS|up] [pcp=0-7|pcp=priority]" */
int  rp_valid_ports(const char *s);        /* P,P-Q,... (empty: valid) */
int  rp_valid_programs(const char *s);     /* name,name*,... without spaces */

/* Commands that apply the changes to a running stack (RIPTIDE port), one
 * per call of fn. Returns 1 when some changes need a restart of Riptide
 * (interface, address, IPv4/IPv6 on or off, a sysctl set back to its
 * default), else 0. */
int  rp_live_commands(const struct rp_conf *c, void (*fn)(const char *line, void *arg), void *arg);

#endif
