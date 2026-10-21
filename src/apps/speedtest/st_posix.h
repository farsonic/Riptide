/*
 * Riptide - speedtest: the POSIX (Linux kernel sockets) shim, for the host
 * build (build/host/speedtest) and the host tests.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef ST_POSIX_H
#define ST_POSIX_H

#include "speedtest.h"

/* Install the SIGINT handler (Ctrl-C -> sys->brk()). */
void st_posix_init(void);
/* The shim; copy it to override out/flush (tests capture the output). */
extern const struct st_sys st_posix_sys;
/* errno (Linux) -> RT_E* */
int st_posix_errno(int e);

#endif
