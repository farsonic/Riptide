/*
 * Riptide - random number generator for systems without one (AmigaOS):
 * ChaCha20 (RFC 8439) as a "fast key erasure" generator fed by an
 * entropy pool.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Every ChaCha20 block made from the current 256-bit key gives 64 bytes:
 * the first 32 replace the key at once, the other 32 are handed out. An
 * attacker who sees outputs cannot work back to the key, and one who
 * later reads the state cannot recompute earlier outputs.
 *
 * Entropy comes in two ways. rt_rng_stir() absorbs seed material (the
 * saved seed file, clocks, hardware addresses) into the key immediately.
 * rt_rng_add() is cheap enough for every received frame: it stirs one
 * word (a beam position, a timer reading) into a small pool, which is
 * folded into the key before the next block once enough events have come
 * in. Only 32-bit adds, xors and rotates: no 64-bit helpers on a 68060.
 *
 * The caller serialises rt_rng_bytes()/rt_rng_stir() (os_amiga.c uses
 * Forbid()); a race in rt_rng_add() only loses an event.
 */
#include "rt/riptide.h"
#include "rt/rt_os.h"

#define RNG_FOLD_EVENTS 32      /* events before the pool is folded in */

static u32 rng_key[8];
static u32 rng_ctr;
static u8  rng_out[32];
static u32 rng_avail;
static u32 rng_pool[8];
static u32 rng_pool_i;
static u32 rng_events;

static u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d) do { \
	a += b; d ^= a; d = rol32(d, 16); \
	c += d; b ^= c; b = rol32(b, 12); \
	a += b; d ^= a; d = rol32(d, 8); \
	c += d; b ^= c; b = rol32(b, 7); } while (0)

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u32 get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

void rt_chacha20_block(const u32 key[8], u32 counter, const u32 nonce[3], u8 out[64])
{
	u32 x[16], s[16];
	int i;

	s[0] = 0x61707865u;
	s[1] = 0x3320646eu;
	s[2] = 0x79622d32u;
	s[3] = 0x6b206574u;
	for (i = 0; i < 8; i++)
		s[4 + i] = key[i];
	s[12] = counter;
	s[13] = nonce[0];
	s[14] = nonce[1];
	s[15] = nonce[2];
	memcpy(x, s, sizeof(x));
	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}
	for (i = 0; i < 16; i++)
		put_le32(out + 4 * i, x[i] + s[i]);
}

/* one block: the first half becomes the new key, the second the output */
static void rng_block(const u32 nonce[3])
{
	u8 blk[64];
	int i;

	rt_chacha20_block(rng_key, rng_ctr++, nonce, blk);
	for (i = 0; i < 8; i++)
		rng_key[i] = get_le32(blk + 4 * i);
	memcpy(rng_out, blk + 32, 32);
	memset(blk, 0, sizeof(blk));
}

void rt_rng_stir(const void *data, u32 len)
{
	static const u32 stir_nonce[3] = { 0x72697473u, 0, 0 };   /* "stir" */
	const u8 *p = data;
	u32 i, n;

	/* xor each 32-byte chunk into the key, then step the key: the result
	 * depends on all of it and reveals none of it */
	do {
		n = len < 32 ? len : 32;
		for (i = 0; i < n; i++)
			rng_key[i / 4] ^= (u32)p[i] << (8 * (i & 3));
		rng_key[0] ^= n;
		rng_block(stir_nonce);
		p += n;
		len -= n;
	} while (len);
	rng_avail = 0;     /* nothing made from the old key goes out */
}

void rt_rng_add(u32 v)
{
	u32 i = rng_pool_i++;
	u32 *w = &rng_pool[i & 7];

	*w = rol32(*w, 7) ^ (v + i);
	rng_events++;
}

void rt_rng_bytes(void *buf, u32 len)
{
	static const u32 out_nonce[3] = { 0, 0, 0 };
	u8 *p = buf;
	u32 n, i;

	while (len) {
		if (rng_avail == 0) {
			if (rng_events >= RNG_FOLD_EVENTS) {
				for (i = 0; i < 8; i++) {
					rng_key[i] ^= rng_pool[i];
					rng_pool[i] = 0;
				}
				rng_events = 0;
			}
			rng_block(out_nonce);
			rng_avail = 32;
		}
		n = len < rng_avail ? len : rng_avail;
		memcpy(p, rng_out + 32 - rng_avail, n);
		/* handed-out bytes are not kept */
		memset(rng_out + 32 - rng_avail, 0, n);
		rng_avail -= n;
		p += n;
		len -= n;
	}
}
