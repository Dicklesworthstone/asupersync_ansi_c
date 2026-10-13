/*
 * tools/wasm/include/string.h — declarations of the string functions the
 * wasm32 build's units call, and no others
 *
 * The freestanding wasm32 lane (make build-wasm32) has no C library and
 * no sysroot. This directory is not a libc: each header declares exactly
 * the functions that the compiled units reference, and
 * tools/wasm/wasm32_libc.c defines exactly those. A function a unit starts
 * to call must be added to both, deliberately, or the lane fails to
 * compile or to link.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_WASM32_STRING_H
#define ASX_WASM32_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);

#endif /* ASX_WASM32_STRING_H */
