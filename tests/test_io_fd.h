/*
 * test_io_fd.h — a file descriptor tests can register with the IO driver
 *
 * In live POSIX builds the IO driver arms registrations in epoll/poll, which
 * (correctly) rejects made-up descriptor numbers, so tests get the read end
 * of a real pipe. Everywhere else the ghost backend accepts any number and
 * the placeholder is returned unchanged.
 *
 * Include this header FIRST in a test file: it may need to request POSIX
 * declarations before any system header is seen.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TEST_IO_FD_H
#define ASX_TEST_IO_FD_H

#if defined(ASX_PROFILE_POSIX) && defined(ASX_DETERMINISTIC) && (ASX_DETERMINISTIC == 0)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <unistd.h>

static inline int test_io_fd(int placeholder) {
    static int fds[2] = {-1, -1};
    (void)placeholder;
    if (fds[0] < 0 && pipe(fds) != 0) return -1;
    return fds[0];
}
#else
static inline int test_io_fd(int placeholder) { return placeholder; }
#endif

#endif /* ASX_TEST_IO_FD_H */
