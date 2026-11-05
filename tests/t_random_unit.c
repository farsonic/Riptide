/*
 * Riptide - unit test of the random generator for AmigaOS (src/core/random.c):
 * ChaCha20 against RFC 8439, the generator's construction against a
 * known answer, and some sanity on its output.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>

#include "rt/riptide.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const char *s, u8 *out)
{
	unsigned v;

	while (sscanf(s, "%2x", &v) == 1) {
		*out++ = (u8)v;
		s += 2;
	}
}

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void test_chacha_rfc8439(void)
{
	/* RFC 8439 2.3.2 */
	u8 k[32], out[64], want[64];
	u32 key[8], nonce[3] = { 0x09000000u, 0x4a000000u, 0 };
	int i;

	for (i = 0; i < 32; i++)
		k[i] = (u8)i;
	for (i = 0; i < 8; i++)
		key[i] = le32(k + 4 * i);
	rt_chacha20_block(key, 1, nonce, out);
	hex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
	    "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", want);
	CHECK(!memcmp(out, want, 64));
}

static void test_generator(void)
{
	u8 seed[32], out[16], want[16], a[64], b[64];
	u32 ones = 0, i, j;
	static u8 big[8192];

	/* fresh state (all-zero key): stir a known seed, the first output is
	 * the second half of the next block (computed independently) */
	for (i = 0; i < 32; i++)
		seed[i] = (u8)(0x40 + i);
	rt_rng_stir(seed, sizeof(seed));
	rt_rng_bytes(out, sizeof(out));
	hex("d38cbc28e5e3b2e03657972aceb48080", want);
	CHECK(!memcmp(out, want, 16));

	/* consecutive outputs differ, also across blocks */
	rt_rng_bytes(a, sizeof(a));
	rt_rng_bytes(b, sizeof(b));
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	CHECK(memcmp(a, a + 32, 32) != 0);

	/* pool events change the stream */
	for (i = 0; i < 40; i++)
		rt_rng_add(i * 7919u);
	rt_rng_bytes(a, sizeof(a));
	CHECK(memcmp(a, b, sizeof(a)) != 0);

	/* about half the bits are set */
	rt_rng_bytes(big, sizeof(big));
	for (i = 0; i < sizeof(big); i++)
		for (j = 0; j < 8; j++)
			ones += (big[i] >> j) & 1;
	CHECK(ones > sizeof(big) * 4 - 600 && ones < sizeof(big) * 4 + 600);

	/* a stir of odd length, longer than one chunk */
	rt_rng_stir(big, 77);
	rt_rng_bytes(b, 16);
	CHECK(memcmp(b, big, 16) != 0);
}

int main(void)
{
	test_chacha_rfc8439();
	test_generator();
	printf("t_random_unit: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
