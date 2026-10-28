/*
 * Riptide - string.h of the freestanding Amiga build (src/amiga/libc.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_STRING_H
#define RT_AMIGA_STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);

/* -ffreestanding turns GCC's built-in memcpy/memset off, so every header
 * field copy was a call; small fixed-size ones are inlined again (others
 * still call the functions in libc.c). vbcc inlines them by itself. */
#if defined(__GNUC__) && !defined(RT_LIBC_IMPL)
#define memcpy(d, s, n) __builtin_memcpy(d, s, n)
#define memset(d, c, n) __builtin_memset(d, c, n)
#endif

#endif
