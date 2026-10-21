/*
 * Riptide - nc: minimal netcat for testing.
 *
 *   nc [-46] [-v] [-d] [-w secs] HOST PORT   connect
 *   nc [-46] [-v] [-d] -l [-p] PORT          accept one connection
 *   nc [-46] -u [-w secs] HOST PORT          send stdin as one UDP datagram,
 *                                            print the answers
 *
 * IPv6 with -6 or an IPv6 literal HOST; "nc -6 -l" listens on [::] and,
 * being dual-stack, also accepts IPv4 clients. Host names resolve to IPv4
 * unless -6 is given.
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
	cl_puts("usage: nc [-46dvu] [-w secs] host port\n"
	        "       nc [-46dv] -l [-p] port\n");
}

static int nc_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in6 sa;
	struct cl_addr addr;
	u32 port = 0, wait_ms = 0, salen;
	int c, listen_mode = 0, nostdin = 0, verbose = 0, udp = 0, s = -1, fd = -1, rc = CL_ERROR, family = 0;
	LONG n, one = 1;
	char *buf, a[CL_ADDRSTRLEN];

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "ldvuw:p:hn46", &g)) != -1) {
		switch (c) {
		case '4': family = 4; break;
		case '6': family = 6; break;
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
	memset(&addr, 0, sizeof(addr));
	if (listen_mode) {
		if (g.ind < argc && cl_parse_uint(argv[g.ind], 65535, &port) < 0) {
			usage();
			return CL_ERROR;
		}
		addr.family = family == 6 ? RT_AF_INET6 : RT_AF_INET;
		if (family != 6)
			rt_in6_set_v4mapped(&addr.a, 0);
	} else {
		if (g.ind + 1 >= argc || cl_parse_uint(argv[g.ind + 1], 65535, &port) < 0) {
			usage();
			return CL_ERROR;
		}
		if (cl_resolve_af(argv[g.ind], family ? family :
		                  (cl_parse_addr(argv[g.ind], &addr) == 0 && addr.family == RT_AF_INET6) ? 6 : 4,
		                  &addr) < 0)
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
	s = socket(addr.family, udp ? RT_SOCK_DGRAM : RT_SOCK_STREAM, 0);
	if (s < 0)
		goto out;
	salen = cl_sockaddr(&addr, (u16)port, &sa);
	if (listen_mode) {
		LONG l = sizeof(sa);
		struct cl_addr peer;
		u16 pp = 0;
		setsockopt(s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
		if (bind(s, &sa, (LONG)salen) < 0 || listen(s, 1) < 0) {
			cl_error("cannot listen on port %lu: %s", (unsigned long)port, cl_strerror((int)Errno()));
			goto out;
		}
		if (verbose)
			cl_printf("Listening on %s %lu\n", addr.family == RT_AF_INET6 ? "::" : "0.0.0.0",
			          (unsigned long)port);
		cl_flush();
		fd = accept(s, &sa, &l);
		CloseSocket(s);
		s = -1;
		if (fd < 0) {
			cl_error("accept: %s", cl_strerror((int)Errno()));
			goto out;
		}
		cl_from_sockaddr(&sa, &peer, &pp);
		if (verbose)
			cl_printf("Connection received on %s %u\n", cl_addr_str(&peer, a), (unsigned)pp);
	} else {
		if (connect(s, &sa, (LONG)salen) < 0) {
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
