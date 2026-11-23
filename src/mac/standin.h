/* src/mac/standin.h - Riptide on the classic Mac OS: stand-in IPv4 addresses for IPv6 hosts
 * (standin.c), for the MacTCP interface (mactcp.c) and riptide.conf's `mactcp` lines.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_MAC_STANDIN_H
#define RT_MAC_STANDIN_H
#include "rt_types.h"

extern int standin_prefer6;                                       /* mactcp ipv6 prefer */
int  standin_is(u32 v4);                                          /* in the stand-in range */
u32  standin_for(const rt_in6_addr *a6, const char *name);        /* 0: none free */
int  standin_lookup(u32 v4, rt_in6_addr *a6, char *name, u32 namelen);
void standin_hold(u32 v4, int hold);
int  standin_config(int argc, char **argv, char *err, u32 errlen);
void standin_restart_begin(void);                                 /* a restart: back to the start */
void standin_restart_end(void);

#endif
