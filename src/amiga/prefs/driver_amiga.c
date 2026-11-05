/*
 * Riptide - RiptidePrefs: finding a card's driver file and copy in memory
 * for the Interface page (prefs_driver.h). The file is looked for where
 * OpenDevice() would find it (the path as given, else DEVS:Networks/ and
 * DEVS:; MNI drivers also in Miami's drawer) and read once per name; the
 * running stack says which kind it really uses.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "prefs/prefs_driver.h"
#include "prefs/prefs_status.h"

#define FILE_MAX (2L * 1024 * 1024)    /* no driver is bigger */

/* one entry per card is enough: the file is read once per name */
#define NCACHE 4
static char cache_dev[NCACHE][96];
static struct rp_drvinfo cache[NCACHE];
static int cache_next;

/* read path into memory and classify it; 0 when it cannot be read */
static int read_file(const char *path, const char *name, struct rp_drvinfo *i)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
	LONG size, got = 0, r;
	u8 *d;

	if (!fh)
		return 0;
	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	d = size > 0 && size <= FILE_MAX ? AllocVec((ULONG)size, MEMF_ANY) : NULL;
	while (d && got < size && (r = Read(fh, d + got, size - got)) > 0)
		got += r;
	Close(fh);
	if (!d)
		return 0;
	i->kind = rp_drv_classify(d, (u32)got, name, i->ver, sizeof(i->ver));
	i->size = (u32)got;
	i->found = 1;
	rt_strlcpy(i->where, path, sizeof(i->where));
	FreeVec(d);
	return 1;
}

static void find_file(const char *device, struct rp_drvinfo *i)
{
	static const char *const dirs[] = { "DEVS:Networks/", "DEVS:", RT_COMPAT_MNI_DIR };
	const char *base = device;
	char path[128];
	unsigned k;

	for (k = 0; device[k]; k++)
		if (device[k] == ':' || device[k] == '/')
			base = device + k + 1;
	if (base != device) {
		read_file(device, base, i);
		return;
	}
	for (k = 0; k < RT_ARRAY_SIZE(dirs) && !i->found; k++) {
		rt_snprintf(path, sizeof(path), "%s%s", dirs[k], device);
		read_file(path, base, i);
	}
}

/* a driver of that name in memory: its version (and ID string without a file) */
static void find_loaded(const char *device, struct rp_drvinfo *i)
{
	const char *base = device;
	struct Library *l;
	unsigned k;

	for (k = 0; device[k]; k++)
		if (device[k] == ':' || device[k] == '/')
			base = device + k + 1;
	Forbid();
	l = (struct Library *)FindName(i->kind == RP_DRV_MNI ? exec_liblist(SysBase) : exec_devlist(SysBase),
	                               (CONST_STRPTR)base);
	if (l) {
		i->loaded = 1;
		if (i->found || !l->lib_IdString) {
			rt_snprintf(i->mem_ver, sizeof(i->mem_ver), "%u.%u", (unsigned)l->lib_Version,
			            (unsigned)l->lib_Revision);
		} else {
			const char *s = (const char *)l->lib_IdString;
			int n = 0;
			while (s[n] && s[n] != '\r' && s[n] != '\n' && n < (int)sizeof(i->mem_ver) - 1) {
				i->mem_ver[n] = (s[n] >= 32 && (u8)s[n] < 127) ? s[n] : '?';
				n++;
			}
			i->mem_ver[n] = 0;
		}
	}
	Permit();
}

void rp_drv_probe(const char *device, const char *ifname, struct rp_drvinfo *i)
{
	char kind[16];
	int c;

	memset(i, 0, sizeof(*i));
	if (!device || !device[0])
		return;
	for (c = 0; c < NCACHE && strcmp(cache_dev[c], device); c++)
		;
	if (c == NCACHE) {
		c = cache_next;
		cache_next = (cache_next + 1) % NCACHE;
		memset(&cache[c], 0, sizeof(cache[c]));
		cache[c].kind = RP_DRV_SANA2;
		find_file(device, &cache[c]);
		rt_strlcpy(cache_dev[c], device, sizeof(cache_dev[c]));
	}
	*i = cache[c];
	/* memory changes (a driver loaded or flushed): looked at each time */
	find_loaded(device, i);
	if (ifname && ifname[0] && rp_status_driver(ifname, kind, sizeof(kind)) &&
	    rp_drv_kind_of(kind) != RP_DRV_UNKNOWN) {
		i->kind = rp_drv_kind_of(kind);
		i->from_stack = 1;
	}
}
