/*
 * tools/wasm/include/ctype.h — the character functions the wasm32 build's
 * units call (see string.h in this directory: not a libc). ASCII only, as
 * the C locale.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_WASM32_CTYPE_H
#define ASX_WASM32_CTYPE_H

int tolower(int c);

#endif /* ASX_WASM32_CTYPE_H */
