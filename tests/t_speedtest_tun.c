/*
 * Riptide - speedtest end to end over Riptide's own TCP/IP stack (host
 * build) through TUN, against tools/speedtest_mock.py on the Linux side
 * (needs root, /dev/net/tun, iproute2 and python3; exits 77 otherwise):
 *   Linux 10.81.1.1/30 (mock)  <-- tun -->  Riptide 10.81.1.2 (speedtest)
 * The shim below maps struct st_sys onto rt_api_* (non-blocking sockets,
 * rt_api_select), the calls the Amiga's bsdsocket.library glue uses.
 * Runs the scenarios of t_speedtest_common.h, and checks that the stack
 * has no sockets or buffers left over afterwards.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "t_speedtest_common.h"

#include "rt/riptide.h"
#include "rt/rt_host.h"

#define OUR_IP   0x0a510102u
#define LINUX_IP 0x0a510101u

static struct rt_sockctx sctx;
static volatile int brk_flag;

static int r_resolve(void *ctx, const char *host, u32 *addr)
{
	struct in_addr a;

	(void)ctx;
	if (inet_pton(AF_INET, host, &a) != 1)
		return -1;              /* no DNS on this link */
	*addr = a.s_addr;
	return 0;
}

static int r_connect(void *ctx, u32 addr, u16 port)
{
	struct rt_sockaddr_in sa;
	int fd, on = 1, r;

	(void)ctx;
	fd = rt_api_socket(&sctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	if (fd < 0)
		return fd;
	rt_api_ioctl(&sctx, fd, RT_FIONBIO, &on);
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_addr = addr;
	sa.sin_port = rt_htons(port);
	r = rt_api_connect(&sctx, fd, &sa, sizeof(sa));
	if (r < 0 && r != -RT_EINPROGRESS) {
		rt_api_close(&sctx, fd);
		return r;
	}
	return fd;
}

static int r_conn_error(void *ctx, int fd)
{
	s32 e = 0;
	u32 len = sizeof(e);

	(void)ctx;
	if (rt_api_getsockopt(&sctx, fd, RT_SOL_SOCKET, RT_SO_ERROR, &e, &len) < 0)
		return RT_EIO;
	return (int)e;
}

static int r_send(void *ctx, int fd, const void *p, u32 n)
{
	(void)ctx;
	return rt_api_send(&sctx, fd, p, n, 0);
}

static int r_unsent(void *ctx, int fd)
{
	s32 v = 0;

	(void)ctx;
	return rt_api_ioctl(&sctx, fd, RT_FIONWRITE, &v) < 0 ? -1 : (int)v;
}

static int r_recv(void *ctx, int fd, void *p, u32 n)
{
	(void)ctx;
	return rt_api_recv(&sctx, fd, p, n, 0);
}

static void r_close(void *ctx, int fd)
{
	(void)ctx;
	rt_api_close(&sctx, fd);
}

static int r_wait(void *ctx, int nfds, u32 *rd, u32 *wr, u32 ms)
{
	int r;

	(void)ctx;
	if (brk_flag) {
		brk_flag = 0;
		return -1;
	}
	r = rt_api_select(&sctx, nfds, rd, wr, NULL, (s32)ms, NULL);
	if (r == -RT_EINTR)
		return -1;
	return r < 0 ? -2 : r;
}

static int r_brk(void *ctx)
{
	(void)ctx;
	if (brk_flag) {
		brk_flag = 0;
		return 1;
	}
	return 0;
}

static void r_raise_brk(void *ctx)
{
	(void)ctx;
	brk_flag = 1;
}

static int open_sockets(void)
{
	int fd, n = 0;

	for (fd = 0; fd < sctx.nfds; fd++)
		n += sctx.fds[fd] != NULL;
	return n;
}

int main(int argc, char **argv)
{
	struct rt_netif *tun;
	char name[16], cmd[256];

	(void)argc;
	if (access("/dev/net/tun", R_OK | W_OK) != 0 || geteuid() != 0 || system("ip -V >/dev/null 2>&1") != 0) {
		printf("t_speedtest_tun: SKIP (needs root, /dev/net/tun and iproute2)\n");
		return 77;
	}
	mock_script = find_mock(argv[0]);
	if (!mock_script || system("python3 -c 'import http.server' >/dev/null 2>&1") != 0) {
		printf("t_speedtest_tun: SKIP (needs python3 and tools/speedtest_mock.py)\n");
		return 77;
	}
	if (!rt_tcp_ops.connect) {
		printf("t_speedtest_tun: SKIP (TCP module not linked)\n");
		return 77;
	}
	if (rt_host_start() != 0)
		return 1;
	snprintf(name, sizeof(name), "rts%d", (int)(getpid() % 100000));
	tun = rt_host_tun_open(name, rt_htonl(OUR_IP), rt_htonl(LINUX_IP), rt_htonl(0xfffffffcu));
	if (!tun) {
		printf("t_speedtest_tun: SKIP (cannot create TUN device)\n");
		return 77;
	}
	snprintf(cmd, sizeof(cmd), "ip addr add 10.81.1.1/30 dev %s && ip link set %s up", name, name);
	if (system(cmd) != 0) {
		printf("t_speedtest_tun: SKIP (cannot configure %s)\n", name);
		return 77;
	}
	if (rt_ctx_init(&sctx, 64, NULL) != 0)
		return 1;

	st_posix_init();
	test_sys = st_posix_sys;
	test_sys.resolve = r_resolve;
	test_sys.connect = r_connect;
	test_sys.conn_error = r_conn_error;
	test_sys.send = r_send;
	test_sys.recv = r_recv;
	test_sys.close = r_close;
	test_sys.wait = r_wait;
	test_sys.unsent = r_unsent;
	test_sys.brk = r_brk;
	test_sys.raise_brk = r_raise_brk;
	test_sys.maxfd = 64;
	test_sys.ua = "Riptide host test";
	test_sys.out = cap_out;
	test_sys.flush = cap_flush;

	scenarios("10.81.1.1", 0);
	mock_stop();
	/* every connection was closed */
	CHECK(open_sockets() == 0);
	if (open_sockets())
		fprintf(stderr, "  %d sockets left open\n", open_sockets());

	rt_ctx_destroy(&sctx);
	rt_host_stop();
	printf("t_speedtest_tun: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
