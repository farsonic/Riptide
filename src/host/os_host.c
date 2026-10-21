/*
 * Riptide - rt_os.h on POSIX (Linux host tests, also under qemu-m68k).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "rt/rt_os.h"
#include "rt/rt_errno.h"
#include "rt/rt_arch.h"
#include "rt/rt_host.h"
#include "host_priv.h"

/* ---- stack lock ---- */

static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t lock_owner;
static volatile int lock_owned;

void rt_lock(void)
{
	pthread_mutex_lock(&stack_lock);
	lock_owner = pthread_self();
	lock_owned = 1;
}

void rt_unlock(void)
{
	lock_owned = 0;
	pthread_mutex_unlock(&stack_lock);
}

int rt_lock_held(void)
{
	return lock_owned && pthread_equal(lock_owner, pthread_self());
}

/* ---- clocks ---- */

/* rt_host_advance_clock(): unit tests jump ahead in time */
static volatile u32 clock_skew_ms;

void rt_host_advance_clock(u32 ms)
{
	clock_skew_ms += ms;
}

u64 rt_now_us(void)
{
	struct timespec ts;

	/* 32-bit arithmetic plus rt_mul32x32(), so the m68k test build runs
	 * under qemu-m68k -cpu m68060 (no 64-bit mulu.l/divu.l there) */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return rt_mul32x32((u32)ts.tv_sec + clock_skew_ms / 1000u, 1000000u) +
	       (u32)ts.tv_nsec / 1000u + (clock_skew_ms % 1000u) * 1000u;
}

u32 rt_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u32)ts.tv_sec * 1000u + (u32)ts.tv_nsec / 1000000u + clock_skew_ms;
}

u32 rt_wallclock(void)
{
	return (u32)time(NULL);
}

/* ---- memory (with accounting, so tests can check for leaks) ---- */

#define MEM_HDR 16          /* keeps 8/16 byte alignment of the payload */

static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 mem_blocks, mem_bytes;

void *rt_malloc(size_t n)
{
	u8 *p;

	if (n > 0x7fffffffu - MEM_HDR)
		return NULL;
	p = malloc(n + MEM_HDR);
	if (!p)
		return NULL;
	memcpy(p, &n, sizeof(n));
	pthread_mutex_lock(&mem_lock);
	mem_blocks++;
	mem_bytes += (u32)n;
	pthread_mutex_unlock(&mem_lock);
	return p + MEM_HDR;
}

void *rt_zalloc(size_t n)
{
	void *p = rt_malloc(n);

	if (p)
		memset(p, 0, n);
	return p;
}

void rt_free(void *ptr)
{
	u8 *p = ptr;
	size_t n;

	if (!p)
		return;
	p -= MEM_HDR;
	memcpy(&n, p, sizeof(n));
	pthread_mutex_lock(&mem_lock);
	mem_blocks--;
	mem_bytes -= (u32)n;
	pthread_mutex_unlock(&mem_lock);
	free(p);
}

void rt_host_mem_stats(u32 *blocks, u32 *bytes)
{
	pthread_mutex_lock(&mem_lock);
	if (blocks)
		*blocks = mem_blocks;
	if (bytes)
		*bytes = mem_bytes;
	pthread_mutex_unlock(&mem_lock);
}

/* ---- entropy ----
 * A pool refilled from the kernel (getrandom(2), else /dev/urandom), each
 * word additionally mixed with the high resolution clock. If the kernel
 * gives nothing, a clock-seeded xorshift keeps things working. */

static pthread_mutex_t rnd_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 rnd_pool[16];
static unsigned rnd_avail;
static u32 rnd_state = 0x9e3779b9u;

static int kernel_random(void *buf, size_t n)
{
#ifdef SYS_getrandom
	if (syscall(SYS_getrandom, buf, n, 0) == (long)n)
		return 0;
#endif
	{
		int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
		ssize_t r = -1;
		if (fd >= 0) {
			r = read(fd, buf, n);
			close(fd);
		}
		return r == (ssize_t)n ? 0 : -1;
	}
}

u32 rt_random32(void)
{
	u32 v;
	u64 t = rt_now_us();

	pthread_mutex_lock(&rnd_lock);
	if (rnd_avail == 0) {
		if (kernel_random(rnd_pool, sizeof(rnd_pool)) == 0) {
			rnd_avail = RT_ARRAY_SIZE(rnd_pool);
		} else {
			unsigned i;
			for (i = 0; i < RT_ARRAY_SIZE(rnd_pool); i++) {
				rnd_state ^= (u32)t + i;
				rnd_state ^= rnd_state << 13;
				rnd_state ^= rnd_state >> 17;
				rnd_state ^= rnd_state << 5;
				rnd_pool[i] = rnd_state;
			}
			rnd_avail = RT_ARRAY_SIZE(rnd_pool);
		}
	}
	v = rnd_pool[--rnd_avail];
	pthread_mutex_unlock(&rnd_lock);
	return v ^ (u32)(t * 2654435761u);
}

void rt_random_bytes(void *buf, size_t n)
{
	u8 *p = buf;

	while (n) {
		u32 r = rt_random32();
		size_t k = n < 4 ? n : 4;
		memcpy(p, &r, k);
		p += k;
		n -= k;
	}
}

/* ---- logging ---- */

int rt_log_level = RT_LOG_WARN;

void rt_log(int level, const char *fmt, ...)
{
	static const char *const names[] = { "ERR", "WARN", "INFO", "DEBUG" };
	char line[256];
	va_list ap;

	if (level > rt_log_level)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[%u.%03u riptide %s] %s\n", rt_now_ms() / 1000, rt_now_ms() % 1000,
	        names[level < 0 ? 0 : level > 3 ? 3 : level], line);
}

__attribute__((constructor)) static void log_level_from_env(void)
{
	const char *e = getenv("RT_LOG_LEVEL");
	if (e)
		rt_log_level = atoi(e);
	/* CPU model for the routine choice (m68k builds under qemu-m68k:
	 * RT_CPU=68060 together with QEMU_CPU=m68060) */
	e = getenv("RT_CPU");
	if (e)
		rt_arch_cpu = (u32)atoi(e);
}

/* ---- kick: a self-pipe the stack thread watches (see host.c) ---- */

static int kick_pipe[2] = { -1, -1 };
static pthread_once_t kick_once = PTHREAD_ONCE_INIT;

static void kick_create(void)
{
	if (pipe2(kick_pipe, O_NONBLOCK | O_CLOEXEC) < 0)
		kick_pipe[0] = kick_pipe[1] = -1;
}

int rt_host_kick_fd(void)
{
	pthread_once(&kick_once, kick_create);
	return kick_pipe[0];
}

void rt_kick(void)
{
	char c = 1;

	pthread_once(&kick_once, kick_create);
	if (kick_pipe[1] >= 0) {
		/* EAGAIN means the pipe already holds a wakeup: fine. */
		ssize_t r = write(kick_pipe[1], &c, 1);
		(void)r;
	}
}

/* ---- sleepers ---- */

struct rt_sleeper {
	pthread_mutex_t m;
	pthread_cond_t  c;
	int woken;              /* sticky until rt_sleeper_wait() returns */
};

static pthread_key_t sleeper_key;
static pthread_once_t sleeper_once = PTHREAD_ONCE_INIT;
static __thread struct rt_sleeper *sleeper_tls;

static void sleeper_destroy(void *p)
{
	struct rt_sleeper *s = p;

	pthread_cond_destroy(&s->c);
	pthread_mutex_destroy(&s->m);
	free(s);
}

static void sleeper_key_create(void)
{
	pthread_key_create(&sleeper_key, sleeper_destroy);
}

struct rt_sleeper *rt_sleeper_self(void)
{
	struct rt_sleeper *s = sleeper_tls;
	pthread_condattr_t ca;

	if (s)
		return s;
	pthread_once(&sleeper_once, sleeper_key_create);
	s = calloc(1, sizeof(*s));
	if (!s)
		abort();       /* host test environment: nothing sensible to do */
	pthread_mutex_init(&s->m, NULL);
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&s->c, &ca);
	pthread_condattr_destroy(&ca);
	sleeper_tls = s;
	pthread_setspecific(sleeper_key, s);
	return s;
}

static void sleeper_wake(struct rt_sleeper *s)
{
	pthread_mutex_lock(&s->m);
	s->woken = 1;
	pthread_cond_signal(&s->c);
	pthread_mutex_unlock(&s->m);
}

int rt_sleeper_wait(struct rt_sleeper *s, s32 timeout_ms, u32 *extra_signals)
{
	struct timespec ts;
	int r = 0;

	if (extra_signals)
		*extra_signals = 0;
	if (timeout_ms >= 0) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec += timeout_ms / 1000;
		ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
	}
	/* Take the sleeper mutex before dropping the stack lock: a waker needs
	 * the stack lock to find us, then our mutex to set 'woken', so no
	 * wakeup can slip between the two. */
	pthread_mutex_lock(&s->m);
	rt_unlock();
	while (!s->woken) {
		int e;
		if (timeout_ms < 0) {
			pthread_cond_wait(&s->c, &s->m);
			continue;
		}
		e = pthread_cond_timedwait(&s->c, &s->m, &ts);
		if (e == ETIMEDOUT && !s->woken) {
			r = -RT_ETIMEDOUT;
			break;
		}
	}
	s->woken = 0;
	pthread_mutex_unlock(&s->m);
	rt_lock();
	return r;
}

/* ---- wait queues (generic; the stack lock protects them) ---- */

void rt_waitq_add(struct rt_waitq *wq, struct rt_wait_entry *e, struct rt_sleeper *s)
{
	e->sleeper = s;
	e->prev = NULL;
	e->next = wq->head;
	if (wq->head)
		wq->head->prev = e;
	wq->head = e;
}

void rt_waitq_remove(struct rt_waitq *wq, struct rt_wait_entry *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else if (wq->head == e)
		wq->head = e->next;
	else
		return;         /* not on this queue */
	if (e->next)
		e->next->prev = e->prev;
	e->next = e->prev = NULL;
}

void rt_waitq_wake(struct rt_waitq *wq)
{
	struct rt_wait_entry *e;

	for (e = wq->head; e; e = e->next)
		sleeper_wake(e->sleeper);
}

/* ---- files (packet captures) ---- */

void *rt_file_create(const char *path)
{
	return fopen(path, "wb");
}

int rt_file_write(void *f, const void *p, u32 n)
{
	return fwrite(p, 1, n, f) == n ? 0 : -1;
}

int rt_file_close(void *f)
{
	return fclose(f) == 0 ? 0 : -1;
}
