/*
 * tools/wasm/include/stdio.h — the formatted-text functions the wasm32
 * build's units call (see string.h in this directory: not a libc)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_WASM32_STDIO_H
#define ASX_WASM32_STDIO_H

#include <stddef.h>

int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sscanf(const char *text, const char *fmt, ...);

#endif /* ASX_WASM32_STDIO_H */
