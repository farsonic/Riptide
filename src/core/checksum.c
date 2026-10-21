/*
 * Riptide - Internet checksum (RFC 1071).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The one's complement sum is byte order independent (RFC 1071 2(B)), so
 * we add 16-bit words in *native* order; the folded result, stored back
 * natively, is the correct network order checksum on both big- and
 * little-endian machines.
 *
 * Chaining: rt_csum_partial(b, n2, rt_csum_partial(a, n1, 0)) equals the sum
 * over a||b as long as n1 is even. Callers keep odd lengths for the last
 * piece (all users do: headers are even sized).
 */
#include "rt/rt_ip.h"
#include "rt/rt_arch.h"

#if defined(__GNUC__)
typedef u16 __attribute__((may_alias)) u16_alias;
#else
typedef u16 u16_alias;
#endif

/* Native 16-bit word made of bytes b0 (first in memory) and b1. */
static inline u16 native16(u8 b0, u8 b1)
{
#if RT_BIG_ENDIAN
	return (u16)((b0 << 8) | b1);
#else
	return (u16)((b1 << 8) | b0);
#endif
}

static inline u32 add_carry(u32 a, u32 b)
{
	a += b;
	return a + (a < b);
}

/* Portable reference; rt_csum_partial() dispatches through rt_arch.csum. */
u32 rt_csum_partial_c(const void *data, u32 len, u32 sum)
{
	const u8 *p = (const u8 *)data;
	u32 acc = 0;

	if (((uintptr_t)p & 1) == 0) {
		const u16_alias *w = (const u16_alias *)p;
		while (len >= 2) {
			/* 32768 words of at most 0xffff cannot overflow 32 bits
			 * on top of a folded (<= 0x1ffff) accumulator. */
			u32 n = len >> 1;
			if (n > 32768)
				n = 32768;
			len -= n << 1;
			while (n >= 4) {
				acc += (u32)w[0] + w[1] + w[2] + w[3];
				w += 4;
				n -= 4;
			}
			while (n--)
				acc += *w++;
			acc = (acc & 0xffff) + (acc >> 16);
		}
		p = (const u8 *)w;
	} else {
		/* Odd address: assemble words byte by byte (68000 traps on odd
		 * word access). */
		while (len >= 2) {
			u32 n = len >> 1;
			if (n > 32768)
				n = 32768;
			len -= n << 1;
			while (n--) {
				acc += native16(p[0], p[1]);
				p += 2;
			}
			acc = (acc & 0xffff) + (acc >> 16);
		}
	}
	if (len)
		acc += native16(p[0], 0);
	return add_carry(sum, acc);
}

u32 rt_csum_copy_c(void *dst, const void *src, u32 len, u32 sum)
{
	memcpy(dst, src, len);
	return rt_csum_partial_c(dst, len, sum);
}

void rt_copy_c(void *dst, const void *src, u32 len)
{
	memcpy(dst, src, len);
}

u32 rt_csum_pseudo(u32 saddr, u32 daddr, u8 proto, u16 len)
{
	/* saddr/daddr are already in network order in memory, so their two
	 * native halves are exactly the two words of the wire format. */
	u32 s = (saddr >> 16) + (saddr & 0xffff);
	s += (daddr >> 16) + (daddr & 0xffff);
	s += rt_htons((u16)proto);
	s += rt_htons(len);
	return s;
}

/* rt_csum_partial() while profiling: the checksum as a section */
u32 rt_csum_partial_prof(const void *data, u32 len, u32 sum)
{
	RT_PROF_ENTER(RT_PROF_CSUM);
	sum = rt_arch.csum(data, len, sum);
	RT_PROF_EXIT(RT_PROF_CSUM);
	return sum;
}
