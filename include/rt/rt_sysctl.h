/*
 * Riptide - tunables ("sysctl"). Every module registers a static table of
 * entries at init time. Entries are addressable by dotted name
 * ("net.inet.tcp.sack") and, when they have a BSD-style numeric MIB, by MIB
 * (RiptideSysCtl() in riptide.library uses MIBs, e.g. {CTL_NET, PF_INET,
 * IPPROTO_TCP, TCPCTL_DO_RFC1323}).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_SYSCTL_H
#define RT_SYSCTL_H

#include "rt_types.h"

#define RT_SYSCTL_INT    1   /* ptr -> s32 */
#define RT_SYSCTL_STRING 2   /* ptr -> char[max] (max = buffer size) */
#define RT_SYSCTL_OPAQUE 3   /* read only blob produced by handler */

#define RT_SYSCTL_RW     0x00
#define RT_SYSCTL_RO     0x01

#define RT_SYSCTL_MAXMIB 6

struct rt_sysctl {
	const char *name;
	u8   type;
	u8   flags;
	u8   miblen;                 /* 0 = no numeric MIB */
	s16  mib[RT_SYSCTL_MAXMIB];
	void *ptr;
	s32  min, max;               /* INT: inclusive range; STRING: max = buffer size */
	/* Optional. Called instead of the default copy logic. old/oldlen and
	 * new/newlen follow BSD sysctl(3) conventions. Return 0 or -errno. */
	int  (*handler)(const struct rt_sysctl *e, void *old, u32 *oldlen,
	                const void *newp, u32 newlen);
	const char *descr;
};

/* CTL_NET, PF_INET and protocol numbers for building MIBs */
#define RT_CTL_NET 4

void rt_sysctl_register(const struct rt_sysctl *tab, int n);
/* BSD sysctl semantics: if old != NULL copy current value (and set
 * *oldlen), if newp != NULL set new value. INT values are s32 in host
 * order. Returns 0 or -errno (-RT_ENOENT unknown, -RT_EINVAL range,
 * -RT_EPERM read only, -RT_ENOMEM old buffer too small). */
int  rt_sysctl_byname(const char *name, void *old, u32 *oldlen, const void *newp, u32 newlen);
int  rt_sysctl_bymib(const s32 *mib, u32 miblen, void *old, u32 *oldlen, const void *newp, u32 newlen);
/* Text interface for configuration files and the control tool:
 * "name=value" or "name value". */
int  rt_sysctl_set_text(const char *name, const char *value);
/* Format the value of entry e as text. */
int  rt_sysctl_get_text(const struct rt_sysctl *e, char *buf, u32 len);
const struct rt_sysctl *rt_sysctl_find(const char *name);
/* Enumerate: returns the i-th registered entry or NULL. */
const struct rt_sysctl *rt_sysctl_iter(int i);

#endif
