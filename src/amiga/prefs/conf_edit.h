/*
 * Riptide - RiptidePrefs: a model of riptide.conf that can be edited and
 * written back without disturbing what it does not understand.
 *
 * The file is kept as a list of lines. Lines the editor understands
 * (the first interface line, dhcp/ifconfig/route for that interface,
 * nameserver, hostname, domain, ipv6, ntp, timezone, mdns and a few
 * sysctls) are parsed into struct rp_model; everything else - comments,
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

#define RP_STR   64
#define RP_ADDR  48
#define RP_NDNS  3
#define RP_LONG  160

/* "not set": numbers absent from the file */
#define RP_UNSET (-1L)

struct rp_model {
	/* interface (the first "interface NAME device=..." line) */
	char ifname[16];
	char device[RP_STR];
	long unit, mtu, reads, tcpwindow;     /* RP_UNSET when absent */
	int  raw;
	char ifextra[RP_LONG];                /* other options, verbatim */
	/* IPv4 address */
	int  dhcp;                            /* 1 dhcp, 0 static */
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

/* Validation: 1 if valid. */
int  rp_valid_ip4(const char *s, int allow_prefix);
int  rp_valid_name(const char *s);         /* host or domain name */
int  rp_valid_ip(const char *s);           /* IPv4 or IPv6 literal */
int  rp_valid_tz(const char *s);           /* "auto" or [+-]HH[:MM] (timezone) */

/* Commands that apply the changes to a running stack (RIPTIDE port), one
 * per call of fn. Returns 1 when some changes need a restart of Riptide
 * (interface or address), else 0. */
int  rp_live_commands(const struct rp_conf *c, void (*fn)(const char *line, void *arg), void *arg);

#endif
