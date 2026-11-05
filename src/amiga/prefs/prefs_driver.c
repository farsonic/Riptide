/*
 * Riptide - RiptidePrefs: the kind and version of a network driver from
 * its file (see prefs_driver.h). Plain C, tested on the host
 * (tests/t_prefs_driver.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stddef.h>
#include <string.h>
#include "prefs_driver.h"

static void lf_add(char *buf, int len, size_t *n, const char *fmt, const char *a, const char *b)
{
	if (*n < (size_t)len)
		*n += (size_t)rt_snprintf(buf + *n, (size_t)len - *n, fmt, a, b);
}

/* is needle in hay? (no strstr in the freestanding Amiga build) */
static int has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!strncmp(hay, needle, n))
			return 1;
	return n == 0;
}

static int ends_with(const char *s, const char *tail)
{
	size_t n = strlen(s), t = strlen(tail), i;

	if (n < t)
		return 0;
	for (i = 0; i < t; i++) {
		char a = s[n - t + i], b = tail[i];
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

int rp_drv_classify(const u8 *d, u32 len, const char *name, char *ver, int vlen)
{
	int kind = RP_DRV_SANA2;
	u32 i;

	if (ver && vlen > 0)
		ver[0] = 0;
	if (name && ends_with(name, ".mni"))
		kind = RP_DRV_MNI;
	for (i = 0; d && i + 4 <= len; i++) {
		/* netdev's NSCMD_DEVICEQUERY list: ATTACH then DETACH, big-endian
		 * UWORDs on a word boundary */
		if (kind == RP_DRV_SANA2 && !(i & 1) && d[i] == 0x89 && d[i + 1] == 0x00 && d[i + 2] == 0x89 &&
		    d[i + 3] == 0x01)
			kind = RP_DRV_NETDEV;
		if (ver && vlen > 0 && !ver[0] && i + 5 <= len && !memcmp(d + i, "$VER:", 5)) {
			u32 k = i + 5;
			int n = 0;
			while (k < len && d[k] == ' ')
				k++;
			while (k < len && d[k] && d[k] != '\r' && d[k] != '\n' && n < vlen - 1) {
				ver[n++] = (d[k] >= 32 && d[k] < 127) ? (char)d[k] : '?';
				k++;
			}
			while (n > 0 && ver[n - 1] == ' ')
				n--;
			ver[n] = 0;
		}
	}
	return kind;
}

const char *rp_drv_kind_name(int kind)
{
	switch (kind) {
	case RP_DRV_SANA2:  return "SANA-II";
	case RP_DRV_NETDEV: return "netdev";
	case RP_DRV_MNI:    return "MNI";
	}
	return "unknown";
}

int rp_drv_kind_of(const char *k)
{
	if (!k)
		return RP_DRV_UNKNOWN;
	if (!strcmp(k, "sana2"))
		return RP_DRV_SANA2;
	if (!strcmp(k, "netdev"))
		return RP_DRV_NETDEV;
	if (!strcmp(k, "mni"))
		return RP_DRV_MNI;
	return RP_DRV_UNKNOWN;
}

void rp_drv_line(const struct rp_drvinfo *i, const char *device, char *buf, int len)
{
	size_t n = 0;

	buf[0] = 0;
	if (!device || !device[0]) {
		lf_add(buf, len, &n, "Driver: none given%s%s", "", "");
		return;
	}
	if (!i->found && !i->loaded && !i->from_stack) {
		lf_add(buf, len, &n, "Driver: %s not found (DEVS:Networks/, DEVS:)%s", device, "");
		return;
	}
	lf_add(buf, len, &n, "Driver: %s%s", i->kind != RP_DRV_UNKNOWN ? rp_drv_kind_name(i->kind) : "kind unknown",
	       i->from_stack ? " (in use)" : (!i->found && i->kind == RP_DRV_SANA2) ? " (assumed)" : "");
	if (i->ver[0])
		lf_add(buf, len, &n, ", %s%s", i->ver, "");
	else if (i->mem_ver[0])
		lf_add(buf, len, &n, ", %s%s", i->mem_ver, "");
	if (i->found)
		lf_add(buf, len, &n, ", %s%s", i->where, "");
	else if (i->loaded)
		lf_add(buf, len, &n, ", in memory%s%s", "", "");
	/* another version of the same name in memory (Emu68's two genet.device) */
	if (i->found && i->loaded && i->mem_ver[0] && i->ver[0] && !has(i->ver, i->mem_ver))
		lf_add(buf, len, &n, "; %s is in memory", i->mem_ver, "");
}
