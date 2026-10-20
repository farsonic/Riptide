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

#endif
