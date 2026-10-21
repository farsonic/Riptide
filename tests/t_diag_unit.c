/*
 * Riptide - unit tests of the diagnostics (diag.c): the section profiler
 * (exclusive times when sections nest, counters, reset, sections closed by
 * a lock release, the net.riptide.profile table, probes in a real UDP
 * loopback exchange) and the debug log ring (categories, wrapping, show,
 * dump hook, arguments not evaluated when a category is off, the periodic
 * tcpstat lines of a loopback TCP connection).
 * Single threaded, no stack thread.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"
#include "../src/core/infra.h"

#include "t_infra_stubs.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char out[16384];

static int cmd(const char *line)
{
	int r;

	rt_lock();
	r = rt_ctl_command(line, out, sizeof(out), NULL);
	rt_unlock();
	return r;
}

static void busy_us(u32 us)
{
	u32 t0 = rt_prof_ticks();

	while (rt_prof_ticks() - t0 < us)
		;
}

/* the table, by name; returns the record for 'name' (or NULL) */
static struct rt_ctl_prof recs[32];
static int nrecs;

static const struct rt_ctl_prof *prof_rec(const char *name)
{
	u8 buf[2048];
	struct rt_ctl_hdr h;
	u32 len = sizeof(buf);
	int i, r;

	rt_lock();
	r = rt_sysctl_byname("net.riptide.profile", buf, &len, NULL, 0);
	rt_unlock();
	if (r < 0)
		return NULL;
	memcpy(&h, buf, sizeof(h));
	nrecs = (int)h.count;
	for (i = 0; i < nrecs && i < 32; i++)
		memcpy(&recs[i], buf + sizeof(h) + (u32)i * h.recsize, sizeof(recs[i]));
	for (i = 0; i < nrecs && i < 32; i++)
		if (!strcmp(recs[i].name, name))
			return &recs[i];
	return NULL;
}

static void test_profiler(void)
{
	const struct rt_ctl_prof *p;
	u32 len = 0;
	s32 mib[3] = { RT_CTL_NET, RT_CTL_MIB_RIPTIDE, RT_CTL_PROFILE };

	CHECK(cmd("profile show") == 0 && strstr(out, "not started"));
	CHECK(rt_prof_on == 0);
	CHECK(cmd("profile start") == 0 && rt_prof_on == 1);

	/* nested sections: exclusive totals, inclusive max */
	rt_lock();
	RT_PROF_ENTER(RT_PROF_TCP_IN);
	busy_us(2000);
	RT_PROF_ENTER(RT_PROF_CSUM);
	busy_us(3000);
	RT_PROF_EXIT(RT_PROF_CSUM);
	busy_us(1000);
	RT_PROF_EXIT(RT_PROF_TCP_IN);
	rt_unlock();
	p = prof_rec("tcp input");
	CHECK(p && p->kind == RT_CTL_PROF_SECTION && p->count == 1);
	CHECK(p && p->total_us >= 3000 && p->total_us < 4500);   /* not the 6000 inclusive */
	CHECK(p && p->max_us >= 6000);
	p = prof_rec("checksum");
	CHECK(p && p->count == 1 && p->total_us >= 3000 && p->total_us < 4500);
	CHECK(nrecs == RT_PROF_N + 1);
	CHECK(!strcmp(recs[0].name, "wall") && recs[0].kind == RT_CTL_PROF_WALL && recs[0].count == 1);
	CHECK(recs[0].total_us >= 6000);

	/* by MIB, and the size query */
	rt_lock();
	CHECK(rt_sysctl_bymib(mib, 3, NULL, &len, NULL, 0) == 0);
	rt_unlock();
	CHECK(len == sizeof(struct rt_ctl_hdr) + (RT_PROF_N + 1) * sizeof(struct rt_ctl_prof));

	/* counters */
	rt_prof_add(RT_PROF_IDLE, 500);
	rt_prof_add(RT_PROF_IDLE, 700);
	p = prof_rec("idle");
	CHECK(p && p->kind == RT_CTL_PROF_COUNTER && p->count == 2 && p->total_us == 1200 && p->max_us == 700);

	/* a section still open when the lock is released is closed there */
	rt_lock();
	RT_PROF_ENTER(RT_PROF_UDP_IN);
	CHECK(rt_prof_depth == 1);
	rt_unlock();
	CHECK(rt_prof_depth == 0);
	RT_PROF_EXIT(RT_PROF_UDP_IN);      /* ignored: no longer open */
	p = prof_rec("udp input");
	CHECK(p && p->count == 1);

	/* an exit without an entry (profiling started inside a section) */
	rt_lock();
	RT_PROF_EXIT(RT_PROF_IP_IN);
	rt_unlock();
	p = prof_rec("ip input");
	CHECK(p && p->count == 0);

	CHECK(cmd("profile show") == 0);
	CHECK(strstr(out, "profile: running"));
	CHECK(strstr(out, "tcp input"));
	CHECK(strstr(out, "checksum"));
	CHECK(strstr(out, "not in Riptide"));
	CHECK(strstr(out, "cut short"));

	/* reset keeps it running and zeroes everything */
	CHECK(cmd("profile reset") == 0 && rt_prof_on == 1);
	p = prof_rec("tcp input");
	CHECK(p && p->count == 0 && p->total_us == 0);

	/* stop: no more accounting */
	CHECK(cmd("profile stop") == 0 && rt_prof_on == 0);
	rt_lock();
	RT_PROF_ENTER(RT_PROF_TCP_IN);
	RT_PROF_EXIT(RT_PROF_TCP_IN);
	rt_unlock();
	p = prof_rec("tcp input");
	CHECK(p && p->count == 0);
	CHECK(recs[0].count == 0);
	CHECK(cmd("profile show") == 0 && strstr(out, "profile: stopped"));
	CHECK(cmd("profile bogus") < 0 && strstr(out, "usage"));
}

/* probes on the real paths: UDP over the loopback interface */
static void test_probes(void)
{
	struct rt_sockctx ctx;
	struct rt_sockaddr_in a;
	char buf[1024], rx[1024];
	const struct rt_ctl_prof *p;
	int fd, r, i;

	CHECK(rt_ctx_init(&ctx, 8, NULL) == 0);
	fd = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(9999);
	a.sin_addr = rt_htonl(0x7f000001);
	CHECK(rt_api_bind(&ctx, fd, &a, sizeof(a)) == 0);
	memset(buf, 'x', sizeof(buf));
	CHECK(cmd("profile start") == 0);
	for (i = 0; i < 5; i++) {
		r = rt_api_sendto(&ctx, fd, buf, sizeof(buf), 0, &a, sizeof(a));
		CHECK(r == (int)sizeof(buf));
		rt_lock();
		RT_PROF_ENTER(RT_PROF_STACK);
		rt_stack_poll();
		RT_PROF_EXIT(RT_PROF_STACK);
		rt_unlock();
		r = rt_api_recvfrom(&ctx, fd, rx, sizeof(rx), RT_MSG_DONTWAIT, NULL, NULL);
		CHECK(r == (int)sizeof(buf));
	}
	CHECK(cmd("profile stop") == 0);
	p = prof_rec("ip input");
	CHECK(p && p->count == 5);
	p = prof_rec("udp input");
	CHECK(p && p->count == 5);
	p = prof_rec("copy to app");
	CHECK(p && p->count == 5);
	p = prof_rec("copy from app");
	CHECK(p && p->count >= 5);
	p = prof_rec("stack loop");
	CHECK(p && p->count == 5);
	p = prof_rec("timers");
	CHECK(p && p->count == 5);
	CHECK(rt_prof_depth == 0);
	CHECK(recs[0].rx_bytes >= 5 * sizeof(buf));
	rt_api_close(&ctx, fd);
	rt_ctx_destroy(&ctx);
}

static int evaluated;
static int side_effect(void)
{
	return ++evaluated;
}

static char dumped[65536];
static u32 dumped_len;
static int test_dump_hook(const char *path, char *err, u32 errlen)
{
	dumped_len = rt_dbg_snapshot(dumped, sizeof(dumped));
	rt_snprintf(err, errlen, "dumped %lu to %s\n", (unsigned long)dumped_len, path);
	return 0;
}

static void test_debug(void)
{
	int i;
	u32 n;
	char *p;

	CHECK(cmd("debug") == 0 && strstr(out, "debug: off"));
	/* off: arguments are not evaluated */
	RT_DBG(RT_DBG_TCP, "x %d", side_effect());
	CHECK(evaluated == 0);
	CHECK(cmd("debug show") == 0 && strstr(out, "empty"));

	CHECK(cmd("debug tcp,driver") == 0);
	CHECK(rt_dbg_mask == (RT_DBG_TCP | RT_DBG_DRIVER));
	CHECK(strstr(out, "tcp,driver") && strstr(out, "16 KB"));
	CHECK(cmd("debug +dns -driver") == 0 && rt_dbg_mask == (RT_DBG_TCP | RT_DBG_DNS));
	CHECK(cmd("debug sana2 nd") == 0 && rt_dbg_mask == (RT_DBG_DRIVER | RT_DBG_ARP));
	CHECK(cmd("debug bogus") < 0 && strstr(out, "unknown category"));
	CHECK(rt_dbg_mask == (RT_DBG_DRIVER | RT_DBG_ARP));
	CHECK(cmd("debug all") == 0 && rt_dbg_mask == RT_DBG_ALL);
	CHECK(cmd("debug size=0") < 0);
	CHECK(cmd("debug size") < 0);
	CHECK(cmd("debug tcp SIZE 32") == 0 && strstr(out, "ring 32 KB"));
	CHECK(cmd("debug SIZE = 16 all") == 0 && strstr(out, "ring 16 KB"));

	rt_lock();
	RT_DBG(RT_DBG_TCP, "hello %d", side_effect());
	rt_unlock();
	CHECK(evaluated == 1);
	RT_DBG(RT_DBG_DNS, "from outside the lock");     /* takes the lock itself */
	CHECK(cmd("debug show") == 0);
	CHECK(strstr(out, " tcp: hello 1\n"));
	CHECK(strstr(out, " dns: from outside the lock\n"));

	/* category filter */
	CHECK(cmd("debug ip") == 0);
	rt_lock();
	RT_DBG(RT_DBG_TCP, "filtered");
	RT_DBG(RT_DBG_IP, "kept");
	rt_unlock();
	CHECK(cmd("debug show 2") == 0);
	CHECK(!strstr(out, "filtered") && strstr(out, " ip: kept\n"));
	CHECK(strstr(out, "dns:"));       /* show 2: the last two lines */
	CHECK(!strstr(out, "hello"));

	/* log category: rt_log copies (with the lock held) */
	CHECK(cmd("debug log") == 0);
	rt_lock();
	rt_log(RT_LOG_DEBUG, "a log line %d", 42);
	rt_unlock();
	CHECK(cmd("debug show 1") == 0 && strstr(out, " log: a log line 42\n"));

	/* wrap: a 1 KB ring keeps the newest complete lines */
	CHECK(cmd("debug tcp size=1") == 0 && strstr(out, "ring 1 KB"));
	rt_lock();
	for (i = 0; i < 200; i++)
		RT_DBG(RT_DBG_TCP, "line %03d", i);
	rt_unlock();
	CHECK(cmd("debug") == 0 && strstr(out, "bytes overwritten") && !strstr(out, " 0 bytes overwritten"));
	n = rt_dbg_snapshot(NULL, 0);
	CHECK(n > 0 && n <= 1024);
	CHECK(rt_dbg_snapshot(dumped, sizeof(dumped)) == n);
	dumped[n] = 0;
	CHECK(strstr(dumped, "tcp: line 199\n"));
	CHECK(!strstr(dumped, "line 000"));
	/* starts at a line boundary: the first line is complete */
	CHECK(dumped[0] == ' ' || (dumped[0] >= '0' && dumped[0] <= '9'));
	p = strchr(dumped, '\n');
	CHECK(p && strstr(dumped, "tcp: line ") < p);
	CHECK(dumped[n - 1] == '\n');

	/* show limited by the output buffer */
	rt_lock();
	CHECK(rt_ctl_command("debug show 1000", out, 100, NULL) == 0);
	rt_unlock();
	CHECK(strlen(out) < 100 && strstr(out, "line 199"));

	/* dump */
	CHECK(cmd("debug dump RAM:x") < 0 && strstr(out, "not supported"));
	rt_dbg_dump_hook = test_dump_hook;
	CHECK(cmd("debug dump RAM:x") == 0 && strstr(out, "to RAM:x"));
	CHECK(dumped_len == n);
	rt_dbg_dump_hook = NULL;

	CHECK(cmd("debug clear") == 0 && rt_dbg_snapshot(NULL, 0) == 0);
	CHECK(cmd("debug none") == 0 && rt_dbg_mask == 0);
	CHECK(cmd("debug status") == 0 && strstr(out, "debug: off"));

	/* also accepted as a configuration line */
	rt_lock();
	CHECK(rt_config_line("debug tcp", out, sizeof(out), NULL) == 0);
	CHECK(rt_config_line("profile start", out, sizeof(out), NULL) == 0);
	rt_unlock();
	CHECK(rt_dbg_mask == RT_DBG_TCP && rt_prof_on);
	cmd("profile stop");
	cmd("debug none");
}

static void poll_ms(u32 ms)
{
	u32 t0 = rt_now_ms();

	do {
		rt_lock();
		rt_stack_poll();
		rt_unlock();
	} while (rt_now_ms() - t0 < ms);
}

/* tcpstat: periodic per-connection lines over a loopback TCP connection */
static void test_tcpstat(void)
{
	struct rt_sockctx ctx;
	struct rt_sockaddr_in a;
	static char buf[20000];
	int ls, cs, as, r, got = 0;

	CHECK(rt_ctx_init(&ctx, 8, NULL) == 0);
	ls = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	cs = rt_api_socket(&ctx, RT_AF_INET, RT_SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = RT_AF_INET;
	a.sin_port = rt_htons(5555);
	a.sin_addr = rt_htonl(0x7f000001);
	CHECK(rt_api_bind(&ctx, ls, &a, sizeof(a)) == 0);
	CHECK(rt_api_listen(&ctx, ls, 1) == 0);
	{
		int on = 1;
		CHECK(rt_api_ioctl(&ctx, cs, RT_FIONBIO, &on) == 0);
	}
	r = rt_api_connect(&ctx, cs, &a, sizeof(a));
	CHECK(r == 0 || r == -RT_EINPROGRESS);
	poll_ms(5);
	as = rt_api_accept(&ctx, ls, NULL, NULL);
	CHECK(as >= 0);

	CHECK(cmd("debug tcpstat interval=25") == 0 && strstr(out, "tcpstat every 25 ms"));
	CHECK(cmd("debug interval=5") < 0 && strstr(out, "20..60000"));
	CHECK(cmd("debug tcpstat interval=20") == 0);
	memset(buf, 'y', sizeof(buf));
	CHECK(rt_api_send(&ctx, as, buf, 8000, 0) == 8000);
	poll_ms(30);
	while ((r = rt_api_recv(&ctx, cs, buf, sizeof(buf), RT_MSG_DONTWAIT)) > 0)
		got += r;
	CHECK(got == 8000);
	poll_ms(50);
	CHECK(cmd("debug show 100") == 0);
	CHECK(strstr(out, " tcpstat: ") != NULL);
	CHECK(strstr(out, ">5555 st 4 rx ") != NULL);     /* the client side, ESTABLISHED */
	CHECK(strstr(out, " rtt ") && strstr(out, " cwnd ") && strstr(out, " unread "));
	CHECK(cmd("debug none") == 0);
	rt_dbg_dump_hook = test_dump_hook;
	CHECK(cmd("debug clear") == 0);
	poll_ms(50);                                       /* off: no more samples */
	CHECK(rt_dbg_snapshot(NULL, 0) == 0);
	rt_dbg_dump_hook = NULL;
	rt_api_close(&ctx, cs);
	rt_api_close(&ctx, as);
	rt_api_close(&ctx, ls);
	poll_ms(5);
	rt_ctx_destroy(&ctx);
}

int main(void)
{
	CHECK(rt_init() == 0);
	CHECK(rt_prof_hz() == 1000000u);
	test_profiler();
	test_probes();
	test_debug();
	test_tcpstat();
	printf("t_diag_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
