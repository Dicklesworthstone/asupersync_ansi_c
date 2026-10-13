/*
 * tools/wasm/include/errno.h — errno for the wasm32 build (see string.h in
 * this directory: not a libc). A wasm32 module runs one thread, so errno
 * is one int.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_WASM32_ERRNO_H
#define ASX_WASM32_ERRNO_H

extern int errno;

#define ERANGE 34

#endif /* ASX_WASM32_ERRNO_H */
