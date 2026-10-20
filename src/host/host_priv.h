/*
 * Riptide - private interfaces between the files of src/host/.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_HOST_PRIV_H
#define RT_HOST_PRIV_H

#include "rt/rt_types.h"

/* Read end of the rt_kick() self-pipe (non-blocking), created on demand. */
int rt_host_kick_fd(void);

#endif
