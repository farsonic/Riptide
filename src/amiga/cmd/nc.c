/*
 * Riptide - nc: minimal netcat for testing.
 *
 *   nc [-v] [-d] [-w secs] HOST PORT         connect
 *   nc [-v] [-d] -l [-p] PORT                accept one connection
 *   nc -u [-w secs] HOST PORT                send stdin as one UDP datagram,
 *                                            print the answers
 *
 * Without -d, standard input is sent first (until EOF, then the sending
 * side is shut down); everything received is written to standard output
 * until the peer closes. AmigaDOS input cannot be waited on together with
 * a socket, hence this half-duplex scheme. Ctrl-C stops.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmdlib.h"
#include "gen/bsdsocket_calls.h"

#define BUFSZ 2048
struct tv { ULONG secs, micro; };

static void usage(void)
{
	cl_puts("usage: nc [-dvu] [-w secs] host port\n"
	        "       nc [-dv] -l [-p] port\n");
}

static int nc_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in sa;
	u32 port = 0, wait_ms = 0, addr = 0;
	int c, listen_mode = 0, nostdin = 0, verbose = 0, udp = 0, s = -1, fd = -1, rc = CL_ERROR;
	LONG n, one = 1;
	char *buf, a[16];

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "ldvuw:p:hn", &g)) != -1) {
		switch (c) {
		case 'l': listen_mode = 1; break;
		case 'd': nostdin = 1; break;
		case 'v': verbose = 1; break;
		case 'u': udp = 1; break;
		case 'n': break;
		case 'w': if (cl_parse_ms(g.arg, &wait_ms) < 0) { usage(); return CL_ERROR; } break;
		case 'p': if (cl_parse_uint(g.arg, 65535, &port) < 0) { usage(); return CL_ERROR; } break;
		case 'h': usage(); return CL_OK;
		default: usage(); return CL_ERROR;
		}
	}
	if (listen_mode) {
		if (g.ind < argc && cl_parse_uint(argv[g.ind], 65535, &port) < 0) {
			usage();
			return CL_ERROR;
		}
	} else {
		if (g.ind + 1 >= argc || cl_parse_uint(argv[g.ind + 1], 65535, &port) < 0) {
			usage();
			return CL_ERROR;
		}
		if (cl_resolve(argv[g.ind], &addr) < 0)
			return CL_ERROR;
	}
	if (!port || cl_need_socket() < 0) {
		if (!port)
			usage();
		return CL_ERROR;
	}
	buf = AllocVec(BUFSZ, MEMF_ANY);
	if (!buf)
		return CL_FAIL;
	s = socket(RT_AF_INET, udp ? RT_SOCK_DGRAM : RT_SOCK_STREAM, 0);
	if (s < 0)
		goto out;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons((u16)port);
	sa.sin_addr = addr;
	if (listen_mode) {
		LONG l = sizeof(sa);
		setsockopt(s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
		if (bind(s, &sa, sizeof(sa)) < 0 || listen(s, 1) < 0) {
			cl_error("cannot listen on port %lu: %s", (unsigned long)port, cl_strerror((int)Errno()));
			goto out;
		}
		if (verbose)
			cl_printf("Listening on 0.0.0.0 %lu\n", (unsigned long)port);
		cl_flush();
		fd = accept(s, &sa, &l);
		CloseSocket(s);
		s = -1;
		if (fd < 0) {
			cl_error("accept: %s", cl_strerror((int)Errno()));
			goto out;
		}
		if (verbose)
			cl_printf("Connection received on %s %u\n", cl_ip(sa.sin_addr, a), (unsigned)rt_ntohs(sa.sin_port));
	} else {
		if (connect(s, &sa, sizeof(sa)) < 0) {
			cl_error("connect to %s port %lu (tcp) failed: %s", argv[g.ind], (unsigned long)port,
			         cl_strerror((int)Errno()));
			goto out;
		}
		if (verbose)
			cl_printf("Connection to %s %lu port succeeded!\n", argv[g.ind], (unsigned long)port);
		fd = s;
		s = -1;
	}
	cl_flush();
	if (!nostdin) {
		u32 used = 0;
		while ((n = Read(Input(), buf + used, (LONG)(udp ? BUFSZ - used : BUFSZ))) > 0) {
			if (udp) {
				used += (u32)n;
				if (used >= BUFSZ)
					break;
				continue;
			}
			if (send(fd, buf, n, 0) != n)
				break;
		}
		if (udp)
			send(fd, buf, (LONG)used, 0);
		else
			shutdown(fd, 1);
	}
	for (;;) {
		if (wait_ms) {
			ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
			struct tv tmo;
			tmo.secs = wait_ms / 1000;
			tmo.micro = (wait_ms % 1000) * 1000;
			rd[fd / 32] |= 1UL << (fd % 32);
			if (WaitSelect(fd + 1, rd, NULL, NULL, &tmo, &sigs) <= 0)
				break;
		}
		n = recv(fd, buf, BUFSZ, 0);
		if (n <= 0)
			break;
		Write(Output(), buf, n);
	}
	rc = CL_OK;
out:
	if (fd >= 0)
		CloseSocket(fd);
	if (s >= 0)
		CloseSocket(s);
	FreeVec(buf);
	return rc;
}

CL_MAIN(nc_main, "nc")
