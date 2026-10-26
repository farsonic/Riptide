/*
 * Riptide - udpblast: UDP send-rate client for the speed lab
 * (tests/amiga/run_fsuae_udpblast.sh), modelled on a multicast screen
 * sender: COUNT datagrams of SIZE bytes as fast as the stack takes them.
 *
 *   udpblast [-n COUNT] [-s SIZE] [-N] [-m BATCH] [-C] [-t TTL] [-l LABEL] A.B.C.D PORT
 *
 *   -N        non-blocking socket; when sendto() says EWOULDBLOCK or
 *             ENOBUFS, WaitSelect() for writability (pacing) and try again
 *   -m BATCH  send BATCH datagrams per call with RiptideSendMMsg()
 *             (riptide.library 3)
 *   -C        SO_NO_CHECK: no UDP checksum (IPv4)
 *   -t TTL    IP_MULTICAST_TTL (for a multicast A.B.C.D)
 *
 * Without -N the socket blocks: a stack that returns ENOBUFS when the
 * driver queue is full makes the loop retry at once. "errors" counts
 * those refusals (and ENOBUFS with -N), "waits" the WaitSelect()s.
 * Prints
 *
 *   udpblast: LABEL 5000 sent 1400 bytes 3.123456 s 1600 dgram/s 2187.50 KB/s
 *       624 us/dgram 312 us/call, errors 0, waits 12
 *
 * timed with the EClock from the first call to the return of the last.
 * Uses bsdsocket.library calls only, except -m.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "cmd/cmdlib.h"
#include "gen/bsdsocket_calls.h"
#include "gen/riptide_calls.h"

#define MAXBATCH 64

/* sdk/include/libraries/riptide.h (riptide.library 3) */
struct mmsg {
	struct msghdr hdr;
	ULONG len;
};

static void usage(void)
{
	cl_puts("usage: udpblast [-n COUNT] [-s SIZE] [-N] [-m BATCH] [-C] [-t TTL] [-l LABEL] A.B.C.D PORT\n");
}

static int udpblast_main(int argc, char **argv)
{
	struct cl_getopt g = { 1, 0, NULL };
	u32 count = 5000, size = 1400, batch = 0, addr, port, ttl = 0, sent = 0, errors = 0, waits = 0,
	    calls = 0;
	const char *label = "-";
	int c, nb = 0, nocheck = 0, s, i;
	struct rt_sockaddr_in sa;
	static struct mmsg vec[MAXBATCH];
	static struct iovec iov[MAXBATCH];
	char *buf;
	u64 t0, us;

	while ((c = cl_getopt(argc, argv, "n:s:Nm:Ct:l:", &g)) != -1) {
		switch (c) {
		case 'n': if (cl_parse_uint(g.arg, 0x7fffffff, &count) < 0 || !count) { usage(); return CL_ERROR; } break;
		case 's': if (cl_parse_uint(g.arg, 65507, &size) < 0) { usage(); return CL_ERROR; } break;
		case 'N': nb = 1; break;
		case 'm': if (cl_parse_uint(g.arg, MAXBATCH, &batch) < 0 || !batch) { usage(); return CL_ERROR; } break;
		case 'C': nocheck = 1; break;
		case 't': if (cl_parse_uint(g.arg, 255, &ttl) < 0) { usage(); return CL_ERROR; } break;
		case 'l': label = g.arg; break;
		default: usage(); return CL_ERROR;
		}
	}
	if (argc - g.ind != 2 || cl_parse_ip(argv[g.ind], &addr) < 0 ||
	    cl_parse_uint(argv[g.ind + 1], 65535, &port) < 0) {
		usage();
		return CL_ERROR;
	}
	if (cl_need_socket() < 0 || cl_need_timer() < 0)
		return CL_FAIL;
	if (batch) {
		RiptideBase = OpenLibrary((CONST_STRPTR)"riptide.library", 3);
		if (!RiptideBase) {
			cl_printf("udpblast: %s -m needs riptide.library 3\n", label);
			return CL_WARN;
		}
	}
	buf = AllocVec(size ? size : 1, MEMF_ANY);
	if (!buf)
		return CL_FAIL;
	for (i = 0; i < (int)size; i++)
		buf[i] = (char)i;
	s = socket(RT_AF_INET, RT_SOCK_DGRAM, 0);
	if (s < 0) {
		cl_error("socket() failed");
		return CL_FAIL;
	}
	if (nocheck) {
		LONG on = 1;
		if (setsockopt(s, RT_SOL_SOCKET, RT_SO_NO_CHECK, &on, sizeof(on)) < 0)
			cl_printf("udpblast: %s SO_NO_CHECK refused (errno %ld)\n", label, (long)Errno());
	}
	if (ttl) {
		UBYTE t = (UBYTE)ttl;
		setsockopt(s, RT_IPPROTO_IP, RT_IP_MULTICAST_TTL, &t, 1);
	}
	if (nb) {
		LONG on = 1;
		IoctlSocket(s, RT_FIONBIO, &on);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = RT_AF_INET;
	sa.sin_port = rt_htons((u16)port);
	sa.sin_addr = addr;
	for (i = 0; i < MAXBATCH; i++) {
		iov[i].iov_base = buf;
		iov[i].iov_len = size;
		vec[i].hdr.msg_name = (void *)&sa;
		vec[i].hdr.msg_namelen = sizeof(sa);
		vec[i].hdr.msg_iov = &iov[i];
		vec[i].hdr.msg_iovlen = 1;
	}
	t0 = cl_now_us64();
	while (sent < count) {
		LONG r;
		calls++;
		if (batch) {
			u32 k = count - sent < batch ? count - sent : batch;
			r = RiptideSendMMsg(s, vec, k, 0);
			if (r > 0) {
				sent += (u32)r;
				continue;
			}
		} else {
			r = sendto(s, buf, (LONG)size, 0, (struct sockaddr *)&sa, sizeof(sa));
			if (r >= 0) {
				sent++;
				continue;
			}
		}
		r = Errno();
		if (r != RT_ENOBUFS && r != RT_EWOULDBLOCK) {
			cl_printf("udpblast: %s sendto failed, errno %ld\n", label, (long)r);
			break;
		}
		/* EWOULDBLOCK on a non-blocking socket is pacing, not an error */
		if (r == RT_ENOBUFS || !nb)
			errors++;
		if (nb) {
			/* pacing: until the stack can take the next one */
			if (cl_wait_fd(s, 1, 1000) < 0)
				break;
			waits++;
		} else if (cl_break()) {
			break;
		}
	}
	us = cl_now_us64() - t0;
	if (!us)
		us = 1;
	{
		u32 secs = (u32)(us / 1000000u), frac = (u32)(us % 1000000u);
		u32 rate = (u32)(((u64)sent * 1000000u) / us);
		u32 kbs100 = (u32)(((u64)sent * size * 100000u) / 1024u / (us / 1000u ? us / 1000u : 1u));
		cl_printf("udpblast: %s %lu sent %lu bytes %lu.%06lu s %lu dgram/s %lu.%02lu KB/s\n", label,
		          (unsigned long)sent, (unsigned long)size, (unsigned long)secs, (unsigned long)frac,
		          (unsigned long)rate, (unsigned long)(kbs100 / 100u), (unsigned long)(kbs100 % 100u));
		cl_printf("udpblast: %s %lu us/dgram %lu us/call, calls %lu, errors %lu, waits %lu\n", label,
		          (unsigned long)(sent ? us / sent : 0), (unsigned long)(calls ? us / calls : 0),
		          (unsigned long)calls, (unsigned long)errors, (unsigned long)waits);
	}
	CloseSocket(s);
	FreeVec(buf);
	return sent == count ? CL_OK : CL_WARN;
}

CL_MAIN(udpblast_main, "udpblast")
