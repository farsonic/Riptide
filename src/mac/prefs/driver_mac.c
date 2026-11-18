/*
 * Riptide - RiptidePrefs on the classic Mac OS: the driver line of the Interface page
 * (prefs_driver.h, rp_drv_probe; driver_amiga.c on the Amiga looks for a SANA-II file in DEVS:).
 * A Mac network card always has the same kind of driver, .ENET, which the Slot Manager finds in
 * the card's ROM (or the System for built-in Ethernet); there is no file to look at.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "mui/mm.h"
#include "prefs/prefs_driver.h"

void rp_drv_probe(const char *device, const char *ifname, struct rp_drvinfo *i)
{
	(void)ifname;
	memset(i, 0, sizeof(*i));
	i->kind = RP_DRV_UNKNOWN;
	if (!device || rt_strcasecmp(device, ".ENET"))
		return;                 /* "Driver: X not found": tr() says the Mac's is .ENET */
	i->kind = RP_DRV_SANA2;         /* the line says "Ethernet" (mac_lang.c) */
	i->found = 1;
	rt_strlcpy(i->ver, ".ENET", sizeof(i->ver));
	rt_strlcpy(i->where, "the card's own driver, found by the Slot Manager", sizeof(i->where));
}
