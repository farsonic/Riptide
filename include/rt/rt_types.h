/*
 * Riptide TCP/IP stack - basic types, byte order and helpers.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The core is freestanding C99: it only needs <stdint.h>, <stddef.h>,
 * <stdarg.h> and memcpy/memmove/memset/memcmp/strlen/strcmp/strncmp
 * from <string.h> (the Amiga build supplies its own tiny libc).
 */
#ifndef RT_TYPES_H
#define RT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define RT_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define RT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define RT_MAX(a, b) ((a) > (b) ? (a) : (b))
#define RT_UNUSED(x) ((void)(x))
#define RT_CONTAINER_OF(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#if defined(__GNUC__)
#define RT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define RT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define RT_PACKED      __attribute__((packed))
#define RT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define RT_LIKELY(x)   (x)
#define RT_UNLIKELY(x) (x)
#define RT_PACKED
#define RT_PRINTF(a, b)
#endif

#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || \
    defined(__M68K__) /* vbcc (Amiga build) does not define __BYTE_ORDER__ */
#define RT_BIG_ENDIAN 1
#else
#define RT_BIG_ENDIAN 0
#endif

static inline u16 rt_swap16(u16 x) { return (u16)((x >> 8) | (x << 8)); }
static inline u32 rt_swap32(u32 x)
{
	return (x >> 24) | ((x >> 8) & 0xff00u) | ((x << 8) & 0xff0000u) | (x << 24);
}

#if RT_BIG_ENDIAN
#define rt_htons(x) ((u16)(x))
#define rt_ntohs(x) ((u16)(x))
#define rt_htonl(x) ((u32)(x))
#define rt_ntohl(x) ((u32)(x))
#else
#define rt_htons(x) rt_swap16((u16)(x))
#define rt_ntohs(x) rt_swap16((u16)(x))
#define rt_htonl(x) rt_swap32((u32)(x))
#define rt_ntohl(x) rt_swap32((u32)(x))
#endif

/* Unaligned big-endian accessors (68000 traps on odd word access, and
 * received frames give no alignment guarantees for options). */
static inline u16 rt_get_be16(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u16)((b[0] << 8) | b[1]);
}
static inline u32 rt_get_be32(const void *p)
{
	const u8 *b = (const u8 *)p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static inline void rt_put_be16(void *p, u16 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v >> 8); b[1] = (u8)v;
}
static inline void rt_put_be32(void *p, u32 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16); b[2] = (u8)(v >> 8); b[3] = (u8)v;
}

/* An IPv6 address (network order bytes). Transports also keep IPv4
 * endpoints in it, as IPv4-mapped addresses (see rt_ip6.h). */
typedef union rt_in6_addr {
	u8  b[16];
	u16 h[8];                /* network order halves */
	u32 w[4];                /* network order words */
} rt_in6_addr;

/* Sequence-space comparisons (RFC 9293 / Linux before()/after()). */
static inline int rt_seq_before(u32 a, u32 b) { return (s32)(a - b) < 0; }
static inline int rt_seq_after(u32 a, u32 b) { return (s32)(b - a) < 0; }
static inline int rt_seq_leq(u32 a, u32 b) { return (s32)(a - b) <= 0; }
static inline int rt_seq_geq(u32 a, u32 b) { return (s32)(a - b) >= 0; }
/* a <= b <= c in sequence space */
static inline int rt_seq_between(u32 b, u32 a, u32 c) { return c - a >= b - a; }

/* Freestanding formatted output (implemented in src/core/rt_printf.c).
 * Supports %d %i %u %x %X %o %c %s %p %% with l/ll/h/z modifiers,
 * width, '-', '0' and precision for %s. Always NUL terminates. */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int rt_snprintf(char *buf, size_t size, const char *fmt, ...) RT_PRINTF(3, 4);
size_t rt_strlcpy(char *dst, const char *src, size_t size);
unsigned long rt_strtoul(const char *s, const char **end, int base);
int rt_strcasecmp(const char *a, const char *b);
int rt_strncasecmp(const char *a, const char *b, size_t n);

#endif
