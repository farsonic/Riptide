/* src/mac/main_mac.c - Riptide on the classic Mac OS: the .IPP driver's Open, Control and Status
 * (drvr.S calls them with the parameter block and the DCE), and the start of the stack.
 *
 * Open (the INIT at startup, or the first application's OpenDriver): the OS layer and its memory
 * pool, the protocol core, the Ethernet interface on .ENET, then the configuration: riptide.conf
 * lines from the 'TEXT' 128 resource of the Riptide file (an interface address, routes, DNS
 * servers; the default is DHCP on en0). The stack then runs from its deferred task.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>
#include "rt_types.h"
#include "rt_os.h"
#include "rt_errno.h"
#include "riptide.h"
#include "mac.h"

#define HEAP_BYTES (1024u * 1024u)
#define ipBadLapErr  -23000
#define ipBadCnfgErr -23001

static int core_up, net_up;

static int unknown_line(int argc, char **argv, char *err, u32 errlen)
{
	(void)argc;
	rt_snprintf(err, errlen, "unknown keyword '%s'", argv[0]);
	return -RT_EINVAL;
}
static void configure(void)
{
	static const char deflt[] = "dhcp en0\n";
	void **h = mac_getresource(0x54455854u, 128);     /* 'TEXT' 128: riptide.conf */
	const char *text = deflt;
	u32 len = sizeof deflt - 1;
	u32 sz = 0;
	if (h && *h) {
		/* the handle's size: GetHandleSize is a trap; the text ends at its first NUL or at 16 KB */
		text = *h;
		for (sz = 0; sz < 16384 && text[sz]; sz++) ;
		len = sz;
	}
	char line[256], err[96];
	u32 i = 0;
	while (i < len) {
		u32 n = 0;
		while (i < len && text[i] != '\r' && text[i] != '\n') { if (n < sizeof line - 1) line[n++] = text[i]; i++; }
		while (i < len && (text[i] == '\r' || text[i] == '\n')) i++;
		line[n] = 0;
		if (!n || line[0] == '#') continue;
		err[0] = 0;
		rt_lock();
		int r = rt_config_line(line, err, sizeof err, unknown_line);
		rt_unlock();
		if (r < 0) rt_log(RT_LOG_WARN, "riptide.conf: %s: %s", line, err[0] ? err : "error");
	}
	if (h) mac_releaseresource(h);
}

int rt_mac_open(void *pb, void *dce)
{
	(void)pb; (void)dce;
	if (!core_up) {
		mac_log_line("Open: starting");
		int e = mac_os_init(HEAP_BYTES);
		if (e < 0) { rt_log(RT_LOG_ERR, "Open: no %u KB for the stack's memory pool (%d)", HEAP_BYTES / 1024, e); return -108; }
		rt_lock();
		int r = rt_init();
		rt_unlock();
		if (r < 0) { rt_log(RT_LOG_ERR, "rt_init: %d", r); return ipBadCnfgErr; }
		core_up = 1;
		rt_log(RT_LOG_INFO, "Riptide for the Mac: core up");
	}
	if (!net_up) {
		int e = net_mac_open();
		if (e < 0) { rt_log(RT_LOG_ERR, "Open: no network (%d): %s", e, mac_net_why); return ipBadLapErr; }
		net_up = 1;
		configure();
		mac_schedule();
	}
	return 0;
}
int rt_mac_control(void *pb, void *dce) { (void)dce; return ipp_control(pb); }
int rt_mac_status(void *pb, void *dce) { (void)dce; return ipp_status(pb); }
