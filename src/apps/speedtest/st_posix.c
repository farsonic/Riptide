/*
 * Riptide - speedtest: the POSIX (Linux kernel sockets) shim.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "st_posix.h"

static volatile sig_atomic_t got_int;

static void on_sigint(int sig)
{
	(void)sig;
	got_int = 1;
}

void st_posix_init(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	got_int = 0;
}

int st_posix_errno(int e)
{
	switch (e) {
	case EAGAIN: return RT_EWOULDBLOCK;
	case EINPROGRESS: return RT_EINPROGRESS;
	case EALREADY: return RT_EALREADY;
	case EINTR: return RT_EINTR;
	case ECONNREFUSED: return RT_ECONNREFUSED;
	case ECONNRESET: return RT_ECONNRESET;
	case ECONNABORTED: return RT_ECONNABORTED;
	case ETIMEDOUT: return RT_ETIMEDOUT;
	case EPIPE: return RT_EPIPE;
	case EHOSTUNREACH: return RT_EHOSTUNREACH;
	case ENETUNREACH: return RT_ENETUNREACH;
	case ENETDOWN: return RT_ENETDOWN;
	case EMFILE: return RT_EMFILE;
	case ENFILE: return RT_ENFILE;
	case ENOMEM: return RT_ENOMEM;
	case ENOBUFS: return RT_ENOBUFS;
	case EADDRNOTAVAIL: return RT_EADDRNOTAVAIL;
	case EADDRINUSE: return RT_EADDRINUSE;
	case ENOTCONN: return RT_ENOTCONN;
	case EBADF: return RT_EBADF;
	case EINVAL: return RT_EINVAL;
	}
	return RT_EIO;
}

static int px_resolve(void *ctx, const char *host, u32 *addr)
{
	struct addrinfo hints, *res = NULL;
	struct in_addr a;

	(void)ctx;
	if (inet_pton(AF_INET, host, &a) == 1) {
		*addr = a.s_addr;
		return 0;
	}
#ifdef ST_POSIX_STATIC
	(void)hints;
	(void)res;
	return -1;
#else
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
		return -1;
	*addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(res);
	return 0;
#endif
}

static int px_connect(void *ctx, u32 addr, u16 port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	(void)ctx;
	if (fd < 0)
		return -st_posix_errno(errno);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = addr;
	sa.sin_port = htons(port);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
		int e = st_posix_errno(errno);
		close(fd);
		return -e;
	}
	return fd;
}

static int px_conn_error(void *ctx, int fd)
{
	int e = 0;
	socklen_t len = sizeof(e);

	(void)ctx;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) < 0)
		e = errno;
	return e ? st_posix_errno(e) : 0;
}

static int px_send(void *ctx, int fd, const void *p, u32 n)
{
	ssize_t k = send(fd, p, n, MSG_NOSIGNAL);

	(void)ctx;
	return k < 0 ? -st_posix_errno(errno) : (int)k;
}

static int px_recv(void *ctx, int fd, void *p, u32 n)
{
	ssize_t k = recv(fd, p, n, 0);

	(void)ctx;
	return k < 0 ? -st_posix_errno(errno) : (int)k;
}

static void px_close(void *ctx, int fd)
{
	(void)ctx;
	close(fd);
}

static int px_wait(void *ctx, int nfds, u32 *rd, u32 *wr, u32 ms)
{
	struct pollfd p[256];
	int n = 0, fd, r, i;

	(void)ctx;
	if (got_int) {
		got_int = 0;
		return -1;
	}
	for (fd = 0; fd < nfds && fd < 256; fd++) {
		short ev = 0;
		if ((rd[fd >> 5] >> (fd & 31)) & 1)
			ev |= POLLIN;
		if ((wr[fd >> 5] >> (fd & 31)) & 1)
			ev |= POLLOUT;
		if (ev) {
			p[n].fd = fd;
			p[n].events = ev;
			p[n].revents = 0;
			n++;
		}
	}
	memset(rd, 0, (size_t)(nfds + 31) / 32 * 4);
	memset(wr, 0, (size_t)(nfds + 31) / 32 * 4);
	r = poll(p, (nfds_t)n, (int)ms);
	if (r < 0) {
		if (errno != EINTR)
			return -2;
		if (got_int) {
			got_int = 0;
			return -1;
		}
		return 0;
	}
	r = 0;
	for (i = 0; i < n; i++) {
		short ev = p[i].revents;
		fd = p[i].fd;
		if (!ev)
			continue;
		r++;
		/* errors and hang-ups show up as readable/writable, as with select() */
		if (ev & (POLLIN | POLLHUP | POLLERR))
			if (p[i].events & POLLIN)
				rd[fd >> 5] |= 1u << (fd & 31);
		if (ev & (POLLOUT | POLLHUP | POLLERR))
			if (p[i].events & POLLOUT)
				wr[fd >> 5] |= 1u << (fd & 31);
	}
	return r;
}

static u32 px_now_us(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u32)((u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u);
}

static u32 px_unix_time(void *ctx)
{
	(void)ctx;
	return (u32)time(NULL);
}

static int px_brk(void *ctx)
{
	(void)ctx;
	if (got_int) {
		got_int = 0;
		return 1;
	}
	return 0;
}

static void px_raise_brk(void *ctx)
{
	(void)ctx;
	raise(SIGINT);
}

static void px_out(void *ctx, int to_stderr, const char *s, u32 n)
{
	(void)ctx;
	fwrite(s, 1, n, to_stderr ? stderr : stdout);
}

static void px_flush(void *ctx)
{
	(void)ctx;
	fflush(stdout);
}

static void *px_alloc(void *ctx, u32 n)
{
	(void)ctx;
	return calloc(1, n);
}

static void px_free(void *ctx, void *p)
{
	(void)ctx;
	free(p);
}

static const char *px_strerror(void *ctx, int e)
{
	(void)ctx;
	return st_strerror(e);
}

const struct st_sys st_posix_sys = {
	NULL, px_resolve, px_connect, px_conn_error, px_send, px_recv, px_close, px_wait,
	px_now_us, px_unix_time, px_brk, px_raise_brk, px_out, px_flush, px_alloc, px_free,
	px_strerror, 256, "Linux"
};
