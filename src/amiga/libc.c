/*
 * Riptide - the little libc of the freestanding AmigaOS build: the string
 * functions the core may use (docs/DESIGN.md) and whatever GCC emits calls
 * to (memcpy/memset/memmove/memcmp for structure copies and clears).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define RT_LIBC_IMPL    /* the functions themselves, not the inline forms */
#include <stdint.h>
#include <string.h>

/* GCC must not turn these loops back into calls to themselves. */
#pragma GCC optimize ("no-tree-loop-distribute-patterns")

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/* The 68020+ handles misaligned long accesses, but aligned ones are
	 * faster; copy longs when both pointers share alignment. */
	if (n >= 16 && (((uintptr_t)d ^ (uintptr_t)s) & 1) == 0) {
		if ((uintptr_t)d & 1) {
			*d++ = *s++;
			n--;
		}
		if (((uintptr_t)d & 2) && n >= 2) {
			*(uint16_t *)d = *(const uint16_t *)s;
			d += 2; s += 2; n -= 2;
		}
		{
			uint32_t *dl = (uint32_t *)d;
			const uint32_t *sl = (const uint32_t *)s;
			size_t blocks = n >> 4;
			while (blocks--) {
				dl[0] = sl[0]; dl[1] = sl[1];
				dl[2] = sl[2]; dl[3] = sl[3];
				dl += 4; sl += 4;
			}
			n &= 15;
			while (n >= 4) {
				*dl++ = *sl++;
				n -= 4;
			}
			d = (uint8_t *)dl;
			s = (const uint8_t *)sl;
		}
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d <= s || d >= s + n)
		return memcpy(dst, src, n);
	d += n;
	s += n;
	while (n--)
		*--d = *--s;
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	uint8_t b = (uint8_t)c;

	if (n >= 16) {
		uint32_t v = b * 0x01010101u;
		uint32_t *dl;
		while ((uintptr_t)d & 3) {
			*d++ = b;
			n--;
		}
		dl = (uint32_t *)d;
		while (n >= 16) {
			dl[0] = v; dl[1] = v; dl[2] = v; dl[3] = v;
			dl += 4;
			n -= 16;
		}
		while (n >= 4) {
			*dl++ = v;
			n -= 4;
		}
		d = (uint8_t *)dl;
	}
	while (n--)
		*d++ = b;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (uint8_t)*a - (uint8_t)*b;
		if (!*a)
			break;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = NULL;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++) != 0)
		;
	return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
	char *d = dst;

	while (n && *src) {
		*d++ = *src++;
		n--;
	}
	while (n--)
		*d++ = 0;
	return dst;
}

char *strcat(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}
