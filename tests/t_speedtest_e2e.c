/*
 * Riptide - speedtest end to end over the Linux kernel's sockets, against
 * tools/speedtest_mock.py on 127.0.0.1 (needs python3; exits 77 without):
 * discovery through --mirror, server selection by distance and latency,
 * exact byte counts of both tests, keep-alive and Connection: close,
 * chunked bodies, redirects, the HTTPS-only case, a 5000 server list,
 * every output format, the time limit, Ctrl-C (SIGINT) and timeouts.
 * The same scenarios run over Riptide's stack in t_speedtest_tun.c.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_speedtest_common.h"

int main(int argc, char **argv)
{
	(void)argc;
	mock_script = find_mock(argv[0]);
	if (!mock_script || system("python3 -c 'import http.server' >/dev/null 2>&1") != 0) {
		printf("t_speedtest_e2e: SKIP (needs python3 and tools/speedtest_mock.py)\n");
		return 77;
	}
	st_posix_init();
	test_sys = st_posix_sys;
	test_sys.out = cap_out;
	test_sys.flush = cap_flush;
	scenarios("127.0.0.1", 1);
	mock_stop();
	printf("t_speedtest_e2e: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
