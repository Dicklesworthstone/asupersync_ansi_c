/*
 * tools/wasm/include/stdlib.h — the stdlib functions the wasm32 build's
 * units call (see string.h in this directory: not a libc)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_WASM32_STDLIB_H
#define ASX_WASM32_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

char *getenv(const char *name);
unsigned long strtoul(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);

#endif /* ASX_WASM32_STDLIB_H */
