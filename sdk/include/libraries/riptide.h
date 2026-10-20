#ifndef LIBRARIES_RIPTIDE_H
#define LIBRARIES_RIPTIDE_H
/*
 * riptide.library - definitions for programs that use Riptide's own API.
 *
 *   #include <proto/riptide.h>
 *   RiptideBase = OpenLibrary(RIPTIDE_LIB_NAME, RIPTIDE_LIB_VERSION);
 *
 * Open the library from the task that uses it: like bsdsocket.library,
 * every opener task gets its own base, and both share the task's socket
 * descriptor table and errno. See docs/AMIGA.md, "riptide.library".
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define RIPTIDE_LIB_NAME    "riptide.library"
#define RIPTIDE_LIB_VERSION 1

/* RiptideSupportedCPUs() / RiptideSetFdCallback() CPU types */
#define RIPTIDECPU_M68KREG 1     /* 68k code, arguments in registers */
#define RIPTIDECPU_PPCV4   2     /* PowerPC SysV ABI (not supported) */

/* RiptideSysCtl() MIB of Riptide's tables, {CTL_NET, RIPTIDE_CTL, n}.
 * Each is a header (version, record size, count) followed by fixed-layout
 * records, as defined in include/rt/rt_ctl.h of the Riptide sources. */
#define RIPTIDE_CTL_NET       4      /* CTL_NET */
#define RIPTIDE_CTL           255
#define RIPTIDE_CTL_IFACES    1
#define RIPTIDE_CTL_ROUTES    2
#define RIPTIDE_CTL_NEIGH     3
#define RIPTIDE_CTL_SOCKETS   4
#define RIPTIDE_CTL_DNS       5
#define RIPTIDE_CTL_IPSTAT    6
#define RIPTIDE_CTL_ICMPSTAT  7
#define RIPTIDE_CTL_UDPSTAT   8
#define RIPTIDE_CTL_TCPSTAT   9

#endif /* LIBRARIES_RIPTIDE_H */
