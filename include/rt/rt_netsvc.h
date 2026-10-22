/*
 * Riptide - network services in the core: the SNTP client (ntp.c) and the
 * mDNS responder and .local resolver (mdns.c). Lock held unless noted.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_NETSVC_H
#define RT_NETSVC_H

#include "rt_types.h"

struct rt_netif;

/* ---- time ---- */
/* Local time = UTC + rt_tz_offset seconds ("timezone +10:00"; the Amiga
 * glue fills it from locale.library unless rt_tz_configured). */
extern s32 rt_tz_offset;
extern u8  rt_tz_configured;
extern s32 rt_tz_system;   /* the system's offset ("timezone auto") */
/* Set by the OS glue: step the wall clock to sec.usec (UTC since 1970).
 * Returns 0 or -errno. NULL: NTP only measures. */
extern int (*rt_set_wallclock_hook)(u32 sec, u32 usec);
/* "ntp battclock on": the glue also writes the battery backed clock */
extern u8  rt_ntp_battclock;

/* ---- SNTP client ---- */
/* "ntp SERVER... | on | off | sync | status | interval S | step MS |
 *  battclock on|off" and "timezone [+-]HH[:MM]|auto". Output in out. */
int  rt_ntp_command(int argc, char **argv, char *out, u32 outlen);
int  rt_tz_command(int argc, char **argv, char *out, u32 outlen);
/* Servers from DHCP option 42 (network order), n = 0 when the lease goes */
void rt_ntp_dhcp_servers(const struct rt_netif *nif, const u32 *addrs, int n);
/* One status line (no newline), "" when NTP is off */
void rt_ntp_status(char *buf, u32 len);

/* ---- mDNS ---- */
/* "mdns on|off|status" */
int  rt_mdns_command(int argc, char **argv, char *out, u32 outlen);
void rt_mdns_status(char *buf, u32 len);
/* netif.c: an interface is being removed */
void rt_mdns_if_gone(struct rt_netif *nif);
/* our current mDNS host name without ".local" ("" when off) */
const char *rt_mdns_hostname(void);

void rt_netsvc_init(void);

#endif
