/*
 * Riptide - tests of RiptidePrefs' driver kind and version detection
 * (src/amiga/prefs/prefs_driver.c): SANA-II, netdev (Emu68 genet.device
 * 4.x carries NETDEV_CMD_ATTACH, DETACH in its command list) and MNI from
 * the file's bytes, the $VER string, and the Interface page's line.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>

#include "../src/amiga/prefs/prefs_driver.c"

static int checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { failures++; \
	printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void test_classify(void)
{
	/* a netdev driver: code, then the command list 0x4000, 0x8900, 0x8901 ... */
	static const u8 netdev[] = { 0x4e, 0x75, 0x40, 0x00, 0x89, 0x00, 0x89, 0x01, 0x89, 0x02, 0, 0,
	                             '$', 'V', 'E', 'R', ':', ' ', 'e', 'm', 'u', '6', '8', '-', 'g', 'e', 'n', 'e', 't',
	                             ' ', '4', '.', '4', ' ', '(', '0', '2', '.', '1', '1', '.', '2', '0', '2', '6', ')',
	                             0, 'x' };
	/* the same bytes at an odd offset are not a word list */
	static const u8 odd[] = { 0x4e, 0x89, 0x00, 0x89, 0x01, 0x00 };
	static const u8 sana2[] = { 0x4e, 0x75, 0x00, 0x09, 0x00, 0x0a, '$', 'V', 'E', 'R', ':', ' ', ' ', 'a', '2',
	                            '0', '6', '5', '.', 'd', 'e', 'v', 'i', 'c', 'e', ' ', '4', '0', '.', '3', ' ', '\r',
	                            '\n' };
	char ver[64];

	CHECK(rp_drv_classify(netdev, sizeof(netdev), "genet.device", ver, sizeof(ver)) == RP_DRV_NETDEV);
	CHECK_STR(ver, "emu68-genet 4.4 (02.11.2026)");
	CHECK(rp_drv_classify(odd, sizeof(odd), "x.device", ver, sizeof(ver)) == RP_DRV_SANA2);
	CHECK_STR(ver, "");
	CHECK(rp_drv_classify(sana2, sizeof(sana2), "a2065.device", ver, sizeof(ver)) == RP_DRV_SANA2);
	CHECK_STR(ver, "a2065.device 40.3");
	CHECK(rp_drv_classify(sana2, sizeof(sana2), "Z2-AM7990.MNI", ver, 8) == RP_DRV_MNI);
	CHECK_STR(ver, "a2065.d");                 /* cut to the room given */
	CHECK(rp_drv_classify(NULL, 0, "x.device", NULL, 0) == RP_DRV_SANA2);
	CHECK(rp_drv_kind_of("netdev") == RP_DRV_NETDEV && rp_drv_kind_of("sana2") == RP_DRV_SANA2 &&
	      rp_drv_kind_of("mni") == RP_DRV_MNI && rp_drv_kind_of("tap") == RP_DRV_UNKNOWN);
}

static void test_line(void)
{
	struct rp_drvinfo i;
	char b[200];

	memset(&i, 0, sizeof(i));
	rp_drv_line(&i, "", b, sizeof(b));
	CHECK_STR(b, "Driver: none given");
	rp_drv_line(&i, "nosuch.device", b, sizeof(b));
	CHECK_STR(b, "Driver: nosuch.device not found (DEVS:Networks/, DEVS:)");

	i.kind = RP_DRV_NETDEV;
	i.found = 1;
	strcpy(i.where, "DEVS:Networks/genet.device");
	strcpy(i.ver, "emu68-genet-driver 4.4 (02.11.2026)");
	rp_drv_line(&i, "genet.device", b, sizeof(b));
	CHECK_STR(b, "Driver: netdev, emu68-genet-driver 4.4 (02.11.2026), DEVS:Networks/genet.device");
	i.from_stack = 1;
	rp_drv_line(&i, "genet.device", b, sizeof(b));
	CHECK_STR(b, "Driver: netdev (in use), emu68-genet-driver 4.4 (02.11.2026), DEVS:Networks/genet.device");
	/* the other genet in memory */
	i.from_stack = 0;
	i.loaded = 1;
	strcpy(i.mem_ver, "3.17");
	rp_drv_line(&i, "genet.device", b, sizeof(b));
	CHECK_STR(b, "Driver: netdev, emu68-genet-driver 4.4 (02.11.2026), DEVS:Networks/genet.device; "
	             "3.17 is in memory");
	strcpy(i.mem_ver, "4.4");
	rp_drv_line(&i, "genet.device", b, sizeof(b));
	CHECK_STR(b, "Driver: netdev, emu68-genet-driver 4.4 (02.11.2026), DEVS:Networks/genet.device");

	/* resident, no file (uaenet.device, a USB driver loaded by its stack) */
	memset(&i, 0, sizeof(i));
	i.kind = RP_DRV_SANA2;
	i.loaded = 1;
	strcpy(i.mem_ver, "uaenet.device 1.0");
	rp_drv_line(&i, "uaenet.device", b, sizeof(b));
	CHECK_STR(b, "Driver: SANA-II (assumed), uaenet.device 1.0, in memory");
}

int main(void)
{
	test_classify();
	test_line();
	printf("t_prefs_driver: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
