/*
 * Riptide - RiptidePrefs: what kind of network driver a card names
 * (SANA-II, netdev or MNI) and its version, for the Interface page.
 *
 * The kind comes from the running stack when the card is up there
 * (net.riptide.drivers: what Riptide really uses), else from the driver
 * file without opening it (opening a driver starts its hardware):
 *   - a name ending in .mni is an MNI driver (a library)
 *   - a device whose code carries netdev's command list (NETDEV_CMD_ATTACH
 *     0x8900 followed by NETDEV_CMD_DETACH 0x8901, as its NSCMD_DEVICEQUERY
 *     answer) is netdev, such as Emu68's genet.device 4.x
 *   - any other device is taken for SANA-II
 * The version is the file's "$VER:" string, or the copy in memory's ID
 * string (resident drivers have no file).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RP_PREFS_DRIVER_H
#define RP_PREFS_DRIVER_H

#include "rt/rt_types.h"

enum { RP_DRV_UNKNOWN, RP_DRV_SANA2, RP_DRV_NETDEV, RP_DRV_MNI };

struct rp_drvinfo {
	int  kind;                 /* RP_DRV_* */
	int  from_stack;           /* kind as the running stack uses it */
	int  found;                /* the file was found (where) */
	int  loaded;               /* a driver of that name is in memory */
	u32  size;                 /* of the file */
	char where[96];            /* the file found */
	char ver[64];              /* "name V.R (date)" */
	char mem_ver[64];          /* the copy in memory: ID string or V.R */
};

/* the kind and $VER string of a driver file's contents (name: its file
 * name, for .mni); ver gets "" without one */
int  rp_drv_classify(const u8 *d, u32 len, const char *name, char *ver, int vlen);
const char *rp_drv_kind_name(int kind);
/* the kind as net.riptide.drivers names it ("sana2", "netdev", "mni") */
int  rp_drv_kind_of(const char *stack_kind);
/* one line for the Interface page */
void rp_drv_line(const struct rp_drvinfo *i, const char *device, char *buf, int len);

/* Amiga (driver_amiga.c): fill i for card ifname's device. The file is
 * read once per device name (cached); the stack is asked each time. */
void rp_drv_probe(const char *device, const char *ifname, struct rp_drvinfo *i);

#endif
