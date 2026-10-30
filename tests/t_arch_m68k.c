/*
 * Riptide - differential test of the 680x0 assembly checksum and copy
 * routines (src/arch/m68k/rt_m68k.s) against the portable C versions.
 * Only meaningful in the m68k build (make CROSS=m68k-linux-gnu-, run under
 * qemu-m68k); elsewhere it skips. Run it on every CPU model:
 *   QEMU_CPU=m68020 RT_CPU=68020 build/m68k-linux-gnu/tests/t_arch_m68k
 *   QEMU_CPU=m68040 RT_CPU=68040 ...    (MOVE16 path)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt/riptide.h"

#ifndef RT_ARCH_M68K_ASM
int main(void)
{
	printf("skip: needs the m68k build with the assembly routines (make CROSS=m68k-linux-gnu-)\n");
	return 77;
}
#else

#include <setjmp.h>
#include <signal.h>

/* Output through rt_snprintf(): glibc's number formatting (built for the
 * 68020) uses 64-bit mulu.l, which qemu-m68k -cpu m68060 does not have
 * (on a real 68060, 68060.library emulates it). */
static char outbuf[256];
#define SAY(f, ...) do { rt_snprintf(outbuf, sizeof(outbuf), __VA_ARGS__); fputs(outbuf, f); } while (0)
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	if (failures < 30) SAY(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { failures++; if (failures < 30) { \
	SAY(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); \
	SAY(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

static u32 rnd = 0x12345678u;
static u32 xr(void)
{
	rnd ^= rnd << 13;
	rnd ^= rnd >> 17;
	rnd ^= rnd << 5;
	return rnd;
}

#define GUARD 32
#define AREA  (140000 + 2 * GUARD + 32)
static u8 src_area[AREA] __attribute__((aligned(16)));
static u8 dst_area[AREA] __attribute__((aligned(16)));

static const u32 init_sums[] = { 0, 0xffffffffu, 0xffffu, 0x80000000u, 0x0001fffeu };

static void fill(u8 *p, u32 n, int pattern)
{
	u32 i;

	for (i = 0; i < n; i++)
		p[i] = pattern == 0 ? (u8)xr() : pattern == 1 ? 0xff : pattern == 2 ? 0 : (u8)(i * 7);
}

typedef void (*copy_fn)(void *, const void *, u32);

static int have_move16;
static sigjmp_buf ill_jb;
static void on_sigill(int s)
{
	(void)s;
	siglongjmp(ill_jb, 1);
}

static int probe_move16(void)
{
	struct sigaction sa, old;
	volatile int ok = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigill;
	sigaction(SIGILL, &sa, &old);
	if (!sigsetjmp(ill_jb, 1)) {
		fill(src_area, 256, 0);
		rt_copy_040(dst_area, src_area, 256);
		ok = memcmp(dst_area, src_area, 256) == 0 ? 1 : -1;
	}
	sigaction(SIGILL, &old, NULL);
	return ok;
}

/* one case: src at offset sa, dst at offset da (0..15) */
static void one(u32 len, u32 sa, u32 da, u32 sum, int pattern)
{
	u8 *s = src_area + GUARD + sa, *d = dst_area + GUARD + da;
	u32 ref, got, i;
	copy_fn copies[3];
	int k, nc = 0;

	fill(src_area, len + 2 * GUARD + 16, pattern);
	ref = rt_csum_partial_c(s, len, sum);

	got = rt_csum_020(s, len, sum);
	CHECKF(rt_csum_fold(got) == rt_csum_fold(ref), "csum_020 len %u sa %u sum %08x: %08x vs %08x",
	       (unsigned)len, (unsigned)sa, (unsigned)sum, (unsigned)got, (unsigned)ref);
	CHECK((got == 0) == (ref == 0));

	copies[nc++] = rt_copy_020;
	copies[nc++] = rt_copy_020m;
	if (have_move16 > 0)
		copies[nc++] = rt_copy_040;
	for (k = -1; k < nc; k++) {
		memset(dst_area, 0xa5, len + 2 * GUARD + 16);
		if (k < 0)
			got = rt_csum_copy_020(d, s, len, sum);
		else
			copies[k](d, s, len);
		if (k < 0)
			CHECKF(rt_csum_fold(got) == rt_csum_fold(ref),
			       "csum_copy_020 len %u sa %u da %u sum %08x: %08x vs %08x", (unsigned)len,
			       (unsigned)sa, (unsigned)da, (unsigned)sum, (unsigned)got, (unsigned)ref);
		CHECKF(memcmp(d, s, len) == 0, "%s len %u sa %u da %u: data",
		       k < 0 ? "csum_copy_020" : k == 0 ? "copy_020" : k == 1 ? "copy_020m" : "copy_040",
		       (unsigned)len, (unsigned)sa, (unsigned)da);
		for (i = 0; i < GUARD + da; i++)
			if (dst_area[i] != 0xa5)
				break;
		CHECKF(i == GUARD + da, "%d: guard before, len %u da %u", k, (unsigned)len, (unsigned)da);
		for (i = GUARD + da + len; i < len + 2 * GUARD + 16; i++)
			if (dst_area[i] != 0xa5)
				break;
		CHECKF(i == len + 2 * GUARD + 16, "%d: guard after, len %u sa %u da %u", k,
		       (unsigned)len, (unsigned)sa, (unsigned)da);
	}
}

static void test_exhaustive_small(void)
{
	u32 len, sa, da;

	for (len = 0; len <= 160; len++)
		for (sa = 0; sa < 16; sa++)
			for (da = 0; da < 16; da++)
				one(len, sa, da, init_sums[(len + sa + da) % 5], (int)((len + da) & 3));
}

static void test_random(void)
{
	int i;

	for (i = 0; i < 3000; i++) {
		u32 len = xr() % 4097, sa = xr() & 15, da = xr() & 15;
		u32 sum = (i & 3) == 3 ? xr() : init_sums[i % 5];
		one(len, sa, da, sum, i & 3);
	}
}

static void test_big(void)
{
	static const u32 lens[] = { 65535, 65536, 65537, 65551, 131071, 140000 };
	static const u32 sas[] = { 0, 1, 2, 3, 15 }, das[] = { 0, 1, 8 };
	u32 i, j, k;

	for (i = 0; i < RT_ARRAY_SIZE(lens); i++)
		for (j = 0; j < RT_ARRAY_SIZE(sas); j++)
			for (k = 0; k < RT_ARRAY_SIZE(das); k++)
				one(lens[i], sas[j], das[k], init_sums[(i + j + k) % 5], (int)((i + j) & 3));
}

/* > 0x10000 blocks of 64 bytes: the upper word of the dbra counter */
static void test_huge(void)
{
	u32 n = 4u * 1024 * 1024 + 4096 + 77, i;
	u8 *s = malloc(n + 16), *d = malloc(n + 16);
	u32 ref;

	if (!s || !d) {
		fputs("no memory for the huge test\n", stderr);
		failures++;
		return;
	}
	for (i = 0; i < n + 16; i++)
		s[i] = (u8)xr();
	ref = rt_csum_partial_c(s + 1, n, 0x1234);
	CHECK(rt_csum_fold(rt_csum_020(s + 1, n, 0x1234)) == rt_csum_fold(ref));
	CHECK(rt_csum_fold(rt_csum_copy_020(d + 1, s + 1, n, 0x1234)) == rt_csum_fold(ref));
	CHECK(memcmp(d + 1, s + 1, n) == 0);
	memset(d, 0, n + 16);
	rt_copy_020(d, s, n);
	CHECK(memcmp(d, s, n) == 0 && d[n] == 0);
	memset(d, 0, n + 16);
	rt_copy_020m(d, s, n);
	CHECK(memcmp(d, s, n) == 0 && d[n] == 0);
	if (have_move16 > 0) {
		memset(d, 0, n + 16);
		rt_copy_040(d, s, n);
		CHECK(memcmp(d, s, n) == 0 && d[n] == 0);
	}
	free(s);
	free(d);
}

/* Chaining pieces with odd lengths (rt_iov_to_buf_csum) and the
 * rt_buf copy + checksum of the TCP transmit path, through rt_arch. */
static void test_chaining(void)
{
	struct rt_iov iov[5];
	u32 lens[5], total, sum, i, t;
	int n, r;

	for (t = 0; t < 400; t++) {
		u8 *p = src_area + GUARD + (xr() & 15);
		n = 1 + (int)(xr() % 5);
		total = 0;
		for (r = 0; r < n; r++) {
			lens[r] = xr() % 300;
			iov[r].base = p + total + (u32)r * 3;   /* gaps between pieces */
			iov[r].len = lens[r];
			total += lens[r];
		}
		fill(src_area, total + 64 + GUARD, 0);
		u8 *d = dst_area + GUARD + (xr() & 15);
		CHECK(rt_iov_to_buf_csum(iov, n, 0, d, total, &sum) == total);
		for (i = 0, r = 0; r < n; r++) {
			CHECK(memcmp(d + i, iov[r].base, lens[r]) == 0);
			i += lens[r];
		}
		CHECKF(rt_csum_fold(sum) == rt_csum_fold(rt_csum_partial_c(d, total, 0)),
		       "iov chain t %u", (unsigned)t);
	}
	for (t = 0; t < 50; t++) {
		struct rt_buf *b = rt_buf_alloc(RT_BUF_HEADROOM, 1460), *c;
		u32 len = xr() % 1461;
		fill(rt_buf_put(b, len), len, (int)(t & 3));
		c = rt_buf_copy_csum(b, &sum);
		CHECK(c && c->len == len && memcmp(c->data, b->data, len) == 0);
		CHECK(c && rt_csum_fold(sum) == rt_csum_fold(rt_csum_partial_c(b->data, len, 0)));
		rt_buf_free(c);
		c = rt_buf_copy(b);
		CHECK(c && c->len == len && memcmp(c->data, b->data, len) == 0);
		CHECK(c && ((uintptr_t)c->head & 15) == 0);
		rt_buf_free(c);
		rt_buf_free(b);
	}
}

static void test_dispatch(void)
{
	char d[160];
	u32 cpu = rt_arch_cpu;

	rt_arch_describe(d, sizeof(d));
	SAY(stdout, "selected: %s\n", d);
	CHECK(rt_arch.csum == rt_csum_020 && rt_arch.csum_copy == rt_csum_copy_020);
	CHECK(rt_arch.copy == rt_copy_020m);
	CHECK(rt_arch.copy_buf == (cpu >= 68040 && have_move16 > 0 ? rt_copy_040 : rt_copy_020m));
	CHECK(rt_arch_select("generic") == 0 && rt_arch.csum == rt_csum_partial_c &&
	      rt_arch.copy == rt_copy_c && rt_arch.copy_buf == rt_copy_c);
	CHECK(rt_arch_select("bogus") == -RT_EINVAL);
	CHECK(rt_arch_select("020") == 0 && rt_arch.csum == rt_csum_020 && rt_arch.copy_buf == rt_copy_020m);
	rt_arch_cpu = 68030;
	CHECK(rt_arch_select("040") == -RT_EOPNOTSUPP && rt_arch_select("060") == -RT_EOPNOTSUPP);
	rt_arch_cpu = 68060;
	CHECK(rt_sysctl_set_text("riptide.cpu.move16", "1") == 0);
	CHECK(rt_arch_select("060") == 0 && rt_arch.copy_buf == rt_copy_040);
	CHECK(rt_sysctl_set_text("riptide.cpu.move16", "0") == 0 && rt_arch.copy_buf == rt_copy_020m);
	{
		char v[96];
		u32 l = sizeof(v);
		CHECK(rt_sysctl_byname("riptide.cpu.routines", v, &l, NULL, 0) == 0 &&
		      strstr(v, "copy_buf=copy_020") != NULL);
		l = sizeof(v);
		CHECK(rt_sysctl_byname("riptide.cpu.model", v, &l, NULL, 0) == 0 && !strcmp(v, "68060"));
		CHECK(rt_sysctl_set_text("riptide.cpu.model", "68000") == -RT_EPERM);
	}
	CHECK(rt_sysctl_set_text("riptide.cpu.move16", "1") == 0 && rt_arch.copy_buf == rt_copy_040);
	rt_arch_cpu = cpu;
	if (have_move16 <= 0)
		CHECK(rt_sysctl_set_text("riptide.cpu.move16", "0") == 0);
	CHECK(rt_arch_select("auto") == 0);
	rt_arch_describe(d, sizeof(d));
	SAY(stdout, "restored: %s\n", d);
}

int main(void)
{
	char d[160];

	if (rt_init() < 0) {
		fputs("rt_init failed\n", stderr);
		return 1;
	}
	have_move16 = probe_move16();
	CHECK(have_move16 >= 0);
	rt_arch_describe(d, sizeof(d));
	SAY(stdout, "RT_CPU %lu, %s; MOVE16 %s\n", (unsigned long)rt_arch_cpu, d,
	       have_move16 > 0 ? "executes (copy_040 tested)" : "traps (copy_040 not tested)");
	if (rt_arch_cpu >= 68040 && have_move16 <= 0) {
		/* qemu's m68060 model lacks MOVE16 (a real 68060 has it): use
		 * the kill switch, as a user with a MOVE16 problem would */
		fputs("RT_CPU says 68040+ but MOVE16 traps (qemu -cpu m68060 lacks it): "
		      "sysctl riptide.cpu.move16=0\n", stdout);
		CHECK(rt_sysctl_set_text("riptide.cpu.move16", "0") == 0);
		CHECK(rt_arch.copy_buf == rt_copy_020);
	}
	test_exhaustive_small();
	test_random();
	test_big();
	test_huge();
	test_chaining();
	test_dispatch();   /* pointer checks only: runs on every CPU */
	SAY(stdout, "%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
#endif
