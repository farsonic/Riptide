/*
 * Riptide - nc: minimal netcat for testing.
 *
 *   nc [-46] [-v] [-d] [-w secs] HOST PORT   connect
 *   nc [-46] [-v] [-d] -l [-p] PORT          accept one connection
 *   nc [-46] -u [-w secs] HOST PORT          send stdin as one UDP datagram,
 *                                            print the answers
 *   nc -u -l [-v] [-w secs] [-c n] [-m group [-I ifaddr]] PORT
 *                                            print received datagrams (until
 *                                            -w seconds of silence or n of them),
 *                                            after joining an IPv4 group with -m
 *
 * TCP: -F connects with TCP Fast Open (TCP_FASTOPEN_CONNECT: with a
 * cookie from an earlier connection the first data goes in the SYN), -K
 * secs turns keepalive on after secs idle, probing every secs (SO_KEEPALIVE,
 * TCP_KEEPIDLE, TCP_KEEPINTVL).
 * IPv4 multicast: -T ttl (IP_MULTICAST_TTL), -I ifaddr (IP_MULTICAST_IF
 * when sending, the interface to join on with -m), -L no local copy
 * (IP_MULTICAST_LOOP 0).
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
	cl_puts("usage: nc [-46dvuF] [-w secs] [-K secs] [-T ttl] [-I ifaddr] [-L] host port\n"
	        "       nc [-46dv] -l [-p] port\n"
	        "       nc -u -l [-v] [-w secs] [-c count] [-m group [-I ifaddr]] [-p] port\n");
}

static int mcast_opts(LONG s, u32 ifaddr, LONG ttl, int noloop)
{
	UBYTE b;

	if (ifaddr && setsockopt(s, RT_IPPROTO_IP, RT_IP_MULTICAST_IF, &ifaddr, 4) < 0)
		return -1;
	b = (UBYTE)ttl;
	if (ttl >= 0 && setsockopt(s, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &b, 1) < 0)
		return -1;
	b = 0;
	if (noloop && setsockopt(s, RT_IPPROTO_IP, RT_IP_MULTICAST_LOOP, &b, 1) < 0)
		return -1;
	return 0;
}

/* nc -u -l: print datagrams as they come */
static int udp_listen(LONG s, int verbose, u32 wait_ms, u32 count, char *buf)
{
	u32 got = 0;

	for (;;) {
		struct rt_sockaddr_in6 from;
		struct cl_addr peer;
		LONG n, fl = sizeof(from);
		u16 pp = 0;
		char a[CL_ADDRSTRLEN];
		ULONG rd[2] = { 0, 0 }, sigs = SIGBREAKF_CTRL_C;
		struct tv tmo, *tp = NULL;

		if (wait_ms) {
			tmo.secs = wait_ms / 1000;
			tmo.micro = (wait_ms % 1000) * 1000;
			tp = &tmo;
		}
		rd[s / 32] |= 1UL << (s % 32);
		if (WaitSelect(s + 1, rd, NULL, NULL, tp, &sigs) <= 0)
			break;
		n = recvfrom(s, buf, BUFSZ, 0, &from, &fl);
		if (n < 0)
			break;
		if (verbose) {
			cl_from_sockaddr(&from, &peer, &pp);
			cl_printf("Received %ld bytes from %s %u\n", (long)n, cl_addr_str(&peer, a), (unsigned)pp);
			cl_flush();
		}
		Write(Output(), buf, n);
		if (count && ++got >= count)
			break;
	}
	return CL_OK;
}

static int nc_main(int argc, char **argv)
{
	struct cl_getopt g;
	struct rt_sockaddr_in6 sa;
	struct cl_addr addr;
	u32 port = 0, wait_ms = 0, salen;
	int c, listen_mode = 0, nostdin = 0, verbose = 0, udp = 0, s = -1, fd = -1, rc = CL_ERROR, family = 0;
	int noloop = 0, fastopen = 0;
	u32 group = 0, ifaddr = 0, count = 0, v, keepidle = 0;
	LONG n, one = 1, ttl = -1;
	char *buf, a[CL_ADDRSTRLEN];
	const char *tcp_host = NULL;

	memset(&g, 0, sizeof(g));
	while ((c = cl_getopt(argc, argv, "ldvuw:p:hn46m:I:T:Lc:FK:", &g)) != -1) {
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
		case 'm':
			if (cl_parse_ip(g.arg, &group) < 0 || (rt_ntohl(group) >> 28) != 14) {
				cl_error("%s: not an IPv4 multicast group", g.arg);
				return CL_ERROR;
			}
			break;
		case 'I':
			if (cl_parse_ip(g.arg, &ifaddr) < 0) { usage(); return CL_ERROR; }
			break;
		case 'T':
			if (cl_parse_uint(g.arg, 255, &v) < 0) { usage(); return CL_ERROR; }
			ttl = (LONG)v;
			break;
		case 'L': noloop = 1; break;
		case 'F': fastopen = 1; break;
		case 'K': if (cl_parse_uint(g.arg, 32767, &keepidle) < 0 || !keepidle) { usage(); return CL_ERROR; } break;
		case 'c': if (cl_parse_uint(g.arg, 0x7fffffff, &count) < 0) { usage(); return CL_ERROR; } break;
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
		if (!udp)
			tcp_host = argv[g.ind];    /* connected by name below */
		else if (cl_resolve_af(argv[g.ind], family ? family :
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
	if (tcp_host) {
		/* IPv6 and IPv4 raced (Happy Eyeballs) with riptide.library 2 */
		fd = cl_connect_name(tcp_host, (u16)port, family, wait_ms ? wait_ms : 75000, &addr);
		if (fd < 0) {
			if (fd != -RT_EHOSTUNREACH || cl_parse_addr(tcp_host, &addr) == 0)
				cl_error("connect to %s port %lu (tcp) failed: %s", tcp_host, (unsigned long)port,
				         cl_strerror(-fd));
			fd = -1;
			goto out;
		}
		if (verbose)
			cl_printf("Connection to %s (%s) %lu port succeeded!\n", tcp_host, cl_addr_str(&addr, a),
			          (unsigned long)port);
		goto connected;
	}
	s = socket(addr.family, udp ? RT_SOCK_DGRAM : RT_SOCK_STREAM, 0);
	if (s < 0)
		goto out;
	salen = cl_sockaddr(&addr, (u16)port, &sa);
	if (listen_mode && udp) {
		setsockopt(s, RT_SOL_SOCKET, RT_SO_REUSEADDR, &one, sizeof(one));
		if (bind(s, &sa, (LONG)salen) < 0) {
			cl_error("cannot bind port %lu: %s", (unsigned long)port, cl_strerror((int)Errno()));
			goto out;
		}
		if (group) {
			struct rt_ip_mreq mr;
			mr.imr_multiaddr = group;
			mr.imr_interface = ifaddr;
			if (setsockopt(s, RT_IPPROTO_IP, RT_IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
				cl_error("cannot join the group: %s", cl_strerror((int)Errno()));
				goto out;
			}
		}
		if (verbose)
			cl_printf("Listening on %s %lu (udp)\n", addr.family == RT_AF_INET6 ? "::" : "0.0.0.0",
			          (unsigned long)port);
		cl_flush();
		rc = udp_listen(s, verbose, wait_ms, count, buf);
		goto out;
	}
	if (!listen_mode && udp && (ttl >= 0 || ifaddr || noloop) &&
	    mcast_opts(s, ifaddr, ttl, noloop) < 0) {
		cl_error("multicast options: %s", cl_strerror((int)Errno()));
		goto out;
	}
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
		if (fastopen && setsockopt(s, RT_IPPROTO_TCP, RT_TCP_FASTOPEN_CONNECT, &one, sizeof(one)) < 0)
			cl_error("TCP_FASTOPEN_CONNECT: %s", cl_strerror((int)Errno()));
		if (keepidle) {
			LONG ki = (LONG)keepidle;
			setsockopt(s, RT_SOL_SOCKET, RT_SO_KEEPALIVE, &one, sizeof(one));
			setsockopt(s, RT_IPPROTO_TCP, RT_TCP_KEEPIDLE, &ki, sizeof(ki));
			setsockopt(s, RT_IPPROTO_TCP, RT_TCP_KEEPINTVL, &ki, sizeof(ki));
		}
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
connected:
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
