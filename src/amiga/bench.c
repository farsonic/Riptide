/*
 * Riptide - "Riptide BENCH": throughput of the portable C and the 680x0
 * assembly checksum and copy routines, timed with the EClock.
 *
 * Every measurement moves 512 KB in buffers of 64, 128, 256 (around the
 * RT_COPY_MIN dispatch threshold), 576, 1460 and 8192 bytes (source and
 * destination 16-byte aligned, like packet buffers) inside Forbid(), so
 * only interrupts disturb it. Results are KB/s. The
 * MOVE16 copy runs only on a 68040/68060.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "amiga_priv.h"

#define BENCH_TOTAL (512u * 1024u)
#define BENCH_MAX   8192u

enum { B_CSUM_C, B_CSUM_020, B_COPY_C, B_COPY_020, B_COPY_040, B_2PASS_C, B_2PASS_020,
       B_CSUM_COPY_020, B_COUNT };
static const char *const bench_names[B_COUNT] = {
	"csum_c", "csum_020", "memcpy", "copy_020", "copy_040", "cpy+sum_c", "cpy+sum_020", "csum_copy_020"
};

static volatile u32 bench_sink;    /* keeps the sums alive */

static u32 eclock_ticks(const struct EClockVal *a, const struct EClockVal *b)
{
	return b->ev_lo - a->ev_lo;    /* < 2^32 ticks: well under an hour */
}

static void bench_run(int which, u8 *dst, const u8 *src, u32 size, u32 n)
{
	u32 s = 0;

	while (n--) {
		switch (which) {
		case B_CSUM_C:        s += rt_csum_partial_c(src, size, 0); break;
		case B_CSUM_020:      s += rt_csum_020(src, size, 0); break;
		case B_COPY_C:        memcpy(dst, src, size); break;
		case B_COPY_020:      rt_copy_020(dst, src, size); break;
		case B_COPY_040:      rt_copy_040(dst, src, size); break;
		case B_2PASS_C:       memcpy(dst, src, size); s += rt_csum_partial_c(dst, size, 0); break;
		case B_2PASS_020:     rt_copy_020(dst, src, size); s += rt_csum_020(dst, size, 0); break;
		case B_CSUM_COPY_020: s += rt_csum_copy_020(dst, src, size, 0); break;
		}
	}
	bench_sink += s;
}

static void *bench_alloc(int *chip)
{
	void *p = AllocVec(BENCH_MAX + 32, MEMF_PUBLIC | MEMF_FAST);

	if (!p) {
		p = AllocVec(BENCH_MAX + 32, MEMF_PUBLIC);
		*chip = 1;
	}
	return p;
}

int amiga_bench(void)
{
	static const u32 sizes[] = { 64, 128, 256, 576, 1460, 8192 };
	struct EClockVal t0, t1;
	char line[160];
	u8 *sa, *da, *src, *dst;
	u32 freq, i, o;
	int w, chip = 0, m16 = rt_arch_cpu >= 68040;

	sa = bench_alloc(&chip);
	da = bench_alloc(&chip);
	if (!sa || !da) {
		amiga_out("Riptide BENCH: out of memory\n");
		if (sa)
			FreeVec(sa);
		if (da)
			FreeVec(da);
		return 20;
	}
	src = (u8 *)(((uintptr_t)sa + 15) & ~(uintptr_t)15);
	dst = (u8 *)(((uintptr_t)da + 15) & ~(uintptr_t)15);
	for (i = 0; i < BENCH_MAX; i++)
		src[i] = (u8)(i * 13 + 7);
	freq = ReadEClock(&t0);
	rt_snprintf(line, sizeof(line), "Riptide BENCH: cpu %lu, EClock %lu Hz, buffers in %s RAM; KB/s, 512 KB per cell\n",
	            (unsigned long)rt_arch_cpu, (unsigned long)freq, chip ? "Chip" : "Fast");
	amiga_out("%s", line);
	o = (u32)rt_snprintf(line, sizeof(line), "%6s", "bytes");
	for (w = 0; w < B_COUNT; w++)
		o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %13s", bench_names[w]);
	amiga_out("%s\n", line);
	for (i = 0; i < RT_ARRAY_SIZE(sizes); i++) {
		u32 size = sizes[i], n = BENCH_TOTAL / size, kb = n * size / 1024u;
		o = (u32)rt_snprintf(line, sizeof(line), "%6lu", (unsigned long)size);
		for (w = 0; w < B_COUNT; w++) {
			u32 ticks, kbs;
			if (w == B_COPY_040 && !m16) {
				o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %13s", "-");
				continue;
			}
			bench_run(w, dst, src, size, 4);    /* warm the caches */
			Forbid();
			ReadEClock(&t0);
			bench_run(w, dst, src, size, n);
			ReadEClock(&t1);
			Permit();
			ticks = eclock_ticks(&t0, &t1);
			if (!ticks)
				ticks = 1;
			kbs = freq < 4000000u ? kb * freq / ticks : (u32)((u64)kb * freq / ticks);
			o += (u32)rt_snprintf(line + o, sizeof(line) - o, " %13lu", (unsigned long)kbs);
		}
		amiga_out("%s\n", line);
	}
	FreeVec(sa);
	FreeVec(da);
	return 0;
}
