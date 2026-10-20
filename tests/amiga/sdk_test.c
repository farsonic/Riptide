/*
 * Riptide - sdk_test: a client built only from the developer SDK in sdk/
 * (proto/riptide.h, the vbcc inlines, libraries/riptide.h) and the NDK, to
 * check that the SDK compiles and calls riptide.library correctly.
 * Prints "sdk_test: ok" and returns 0, or names the failing call and
 * returns 10 (20 if riptide.library cannot be opened).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/riptide.h>

struct Library *RiptideBase;

int main(void)
{
	LONG mib[3], len = 0, idx;
	char name[16];
	ULONG apis = 0, cbs = 0, kernel = 0;
	int rc = 0;

	RiptideBase = OpenLibrary((CONST_STRPTR)RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
	if (!RiptideBase) {
		PutStr((CONST_STRPTR)"sdk_test: cannot open " RIPTIDE_LIB_NAME "\n");
		return 20;
	}
	mib[0] = RIPTIDE_CTL_NET;
	mib[1] = RIPTIDE_CTL;
	mib[2] = RIPTIDE_CTL_IFACES;
	if (RiptideSysCtl(mib, 3, NULL, &len, NULL, 0) != 0 || len <= 0) {
		PutStr((CONST_STRPTR)"sdk_test: FAIL RiptideSysCtl\n");
		rc = 10;
	}
	if (!RiptideGetPid()) {
		PutStr((CONST_STRPTR)"sdk_test: FAIL RiptideGetPid\n");
		rc = 10;
	}
	idx = if_nametoindex((CONST_STRPTR)"lo0");
	if (idx < 1 || !if_indextoname(idx, (STRPTR)name) || name[0] != 'l') {
		PutStr((CONST_STRPTR)"sdk_test: FAIL if_nametoindex/if_indextoname\n");
		rc = 10;
	}
	RiptideSupportedCPUs(&apis, &cbs, &kernel);
	if (apis != RIPTIDECPU_M68KREG || RiptideIsOnline((CONST_STRPTR)"lo0") != 1) {
		PutStr((CONST_STRPTR)"sdk_test: FAIL RiptideSupportedCPUs/RiptideIsOnline\n");
		rc = 10;
	}
	if (!rc)
		PutStr((CONST_STRPTR)"sdk_test: ok\n");
	CloseLibrary(RiptideBase);
	return rc;
}
