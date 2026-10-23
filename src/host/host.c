/*
 * Riptide - host stack thread (the equivalent of the Riptide process on
 * AmigaOS): waits for driver file descriptors, the kick pipe and the next
 * timer deadline.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

#include "rt/riptide.h"
#include "rt/rt_host.h"
#include "host_priv.h"

#define HOST_MAX_FDS 32

struct host_fd {
	int fd;
	void (*rx)(void *arg);
	void *arg;
};

/* The registry has its own mutex so drivers may register with or without
 * the stack lock held. */
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct host_fd reg[HOST_MAX_FDS];
static int reg_n;

static pthread_t stack_thread;
static volatile int stack_running;
static volatile int stack_stop;

int rt_host_register_fd(int fd, void (*rx)(void *arg), void *arg)
{
	int r = -RT_ENOSPC;

	pthread_mutex_lock(&reg_lock);
	if (reg_n < HOST_MAX_FDS) {
		reg[reg_n].fd = fd;
		reg[reg_n].rx = rx;
		reg[reg_n].arg = arg;
		reg_n++;
		r = 0;
	}
	pthread_mutex_unlock(&reg_lock);
	rt_kick();            /* make the thread pick up the new fd */
	return r;
}

void rt_host_unregister_fd(int fd)
{
	int i;

	pthread_mutex_lock(&reg_lock);
	for (i = 0; i < reg_n; i++) {
		if (reg[i].fd == fd) {
			reg[i] = reg[--reg_n];
			break;
		}
	}
	pthread_mutex_unlock(&reg_lock);
	rt_kick();
}

static void drain_kick(int kfd)
{
	char tmp[64];

	while (read(kfd, tmp, sizeof(tmp)) > 0)
		;
}

static void *stack_main(void *unused)
{
	struct pollfd pfd[HOST_MAX_FDS + 1];
	struct host_fd local[HOST_MAX_FDS];
	int kfd = rt_host_kick_fd();

	(void)unused;
	while (!stack_stop) {
		u32 ms;
		int n, i, timeout;

		rt_lock();
		ms = rt_stack_poll();
		rt_unlock();

		pthread_mutex_lock(&reg_lock);
		n = reg_n;
		for (i = 0; i < n; i++) {
			local[i] = reg[i];
			pfd[i].fd = reg[i].fd;
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
		pthread_mutex_unlock(&reg_lock);
		pfd[n].fd = kfd;
		pfd[n].events = POLLIN;
		pfd[n].revents = 0;

		timeout = ms == RT_TIMER_NONE ? -1 : ms > 0x7fffffffu ? 0x7fffffff : (int)ms;
		if (poll(pfd, (nfds_t)n + 1, timeout) < 0 && errno != EINTR)
			break;
		if (pfd[n].revents)
			drain_kick(kfd);
		for (i = 0; i < n; i++) {
			if (!(pfd[i].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;
			/* one receive batch per read, as the Amiga stack loop does */
			rt_lock();
			rt_rx_batch_begin();
			local[i].rx(local[i].arg);
			rt_rx_batch_end();
			rt_unlock();
		}
	}
	return NULL;
}

int rt_host_start(void)
{
	int r;

	if (stack_running)
		return -RT_EALREADY;
	r = rt_init();
	if (r < 0)
		return r;
	if (rt_host_kick_fd() < 0)
		return -RT_ENOMEM;
	stack_stop = 0;
	if (pthread_create(&stack_thread, NULL, stack_main, NULL) != 0)
		return -RT_ENOMEM;
	stack_running = 1;
	return 0;
}

void rt_host_stop(void)
{
	if (!stack_running)
		return;
	stack_stop = 1;
	rt_kick();
	pthread_join(stack_thread, NULL);
	stack_running = 0;
}
