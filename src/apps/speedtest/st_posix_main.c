/*
 * Riptide - speedtest for Linux (build/host/speedtest), on the kernel's
 * sockets: for trying the tool and the mock server on the host.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "st_posix.h"

int main(int argc, char **argv)
{
	st_posix_init();
	return st_main(argc, argv, &st_posix_sys);
}
