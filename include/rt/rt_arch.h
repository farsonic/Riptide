/*
 * Riptide - CPU specific checksum and copy routines ("arch ops").
 *
 * The hot loops of the stack (Internet checksum, bulk payload copies, and
 * copy + checksum in one pass) go through the function pointers in
 * rt_arch. They default to portable C. Builds with RT_ARCH_M68K_ASM link
 * the 680x0 assembly in src/arch/m68k/rt_m68k.s and start with its 68020
 * routines; the OS layer reports the CPU model (rt_arch_cpu) before
 * rt_init(), which then picks the best set (rt_arch_select("auto")).
 *
 * Contract of every implementation:
 *  - csum(data, len, sum): 32-bit partial one's complement sum like
 *    rt_csum_partial_c(): 16-bit words paired from 'data' (whatever its
 *    alignment), an odd trailing byte padded with zero, 'sum' added with
 *    end-around carry. The 32-bit value may differ between implementations
 *    but is congruent modulo 0xffff, so rt_csum_fold() of it is identical
 *    (and it is 0 only if 'sum' and all bytes are 0).
 *  - csum_copy(dst, src, len, sum): copies like memcpy (no overlap) and
 *    returns csum(src, len, sum).
 *  - copy(dst, src, len): memcpy for any memory (user buffers, driver
 *    buffers, Chip RAM); must be reentrant (called from SANA-II interrupt
 *    callbacks).
 *  - copy_buf(dst, src, len): memcpy where both areas are Riptide packet
 *    buffers (rt_buf data); may use MOVE16 on the 68040/68060.
 * Chaining pieces: rt_csum_block_add() adds the sum of a piece that starts
 * at byte offset 'off' of the checksummed area.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_ARCH_H
#define RT_ARCH_H

#include "rt_types.h"

struct rt_arch_ops {
	u32  (*csum)(const void *data, u32 len, u32 sum);
	u32  (*csum_copy)(void *dst, const void *src, u32 len, u32 sum);
	void (*copy)(void *dst, const void *src, u32 len);
	void (*copy_buf)(void *dst, const void *src, u32 len);
};
extern struct rt_arch_ops rt_arch;

/* Copies shorter than this are plain memcpy calls (no dispatch): the
 * assembly copy only wins from about 128 bytes (Riptide BENCH, 68020). */
#define RT_COPY_MIN 128

/* Portable reference versions. */
u32  rt_csum_partial_c(const void *data, u32 len, u32 sum);
u32  rt_csum_copy_c(void *dst, const void *src, u32 len, u32 sum);
void rt_copy_c(void *dst, const void *src, u32 len);

#ifdef RT_ARCH_M68K_ASM
/* src/arch/m68k/rt_m68k.s */
u32  rt_csum_020(const void *data, u32 len, u32 sum);
u32  rt_csum_copy_020(void *dst, const void *src, u32 len, u32 sum);
void rt_copy_020(void *dst, const void *src, u32 len);
void rt_copy_040(void *dst, const void *src, u32 len);   /* MOVE16: 68040/68060 */
void rt_copy_020m(void *dst, const void *src, u32 len);  /* movem.l blocks */
#endif

#ifndef RT_TOOL_BUILD  /* tools (src/amiga/cmd) link without the core */
static inline void rt_copy(void *dst, const void *src, u32 len)
{
	if (len < RT_COPY_MIN)
		memcpy(dst, src, len);
	else
		rt_arch.copy(dst, src, len);
}
static inline void rt_copy_buf(void *dst, const void *src, u32 len)
{
	if (len < RT_COPY_MIN)
		memcpy(dst, src, len);
	else
		rt_arch.copy_buf(dst, src, len);
}
static inline u32 rt_csum_copy(void *dst, const void *src, u32 len, u32 sum)
{
	return rt_arch.csum_copy(dst, src, len, sum);
}
#endif

/* One's complement addition of two partial sums. */
static inline u32 rt_csum_add(u32 a, u32 b)
{
	a += b;
	return a + (a < b);
}
/* Add the partial sum s2 of a piece starting at byte offset 'off' of the
 * checksummed area (odd offset: its bytes pair the other way round; a
 * rotation by 8 bits swaps the bytes modulo 0xffff, as Linux does). */
static inline u32 rt_csum_block_add(u32 sum, u32 s2, u32 off)
{
	if (off & 1)
		s2 = (s2 >> 8) | (s2 << 24);
	return rt_csum_add(sum, s2);
}

/* 32 x 32 -> 64 bit product from 16-bit pieces: the compilers' 64-bit
 * multiply uses the 64-bit mulu.l form, which the 68060 traps to
 * 68060.library. For per-packet code (clocks). */
static inline u64 rt_mul32x32(u32 a, u32 b)
{
	u32 al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = b >> 16;
	u32 ll = al * bl, mid = al * bh, hl = ah * bl, hi = ah * bh, lo;

	mid += hl;
	if (mid < hl)
		hi += 0x10000u;
	lo = ll + (mid << 16);
	if (lo < ll)
		hi++;
	hi += mid >> 16;
	return ((u64)hi << 32) | lo;
}

/* CPU model as 68000, 68010, 68020, 68030, 68040, 68060, or 0 if the OS
 * layer does not know it. Set before rt_init(). */
extern u32 rt_arch_cpu;
/* Choose the routines: "auto" (best for rt_arch_cpu), "generic" (C),
 * "020", "040" or "060". Returns 0, -RT_EINVAL for an unknown name, or
 * -RT_EOPNOTSUPP if the build or the CPU lacks those routines. */
int  rt_arch_select(const char *routines);
/* MOVE16 permission (sysctl riptide.cpu.move16). The OS layer clears it
 * for good with rt_arch_move16_veto() when packet buffers may live in
 * memory unsafe for MOVE16 (e.g. Chip RAM). */
void rt_arch_move16_veto(const char *why);
/* "cpu: 68060, checksum: csum_020, copy: copy_020, buffer copy: copy_040 (move16)" */
int  rt_arch_describe(char *buf, u32 len);
/* Registers the riptide.cpu.* sysctls and selects "auto" (from rt_init). */
void rt_arch_init(void);

#endif
