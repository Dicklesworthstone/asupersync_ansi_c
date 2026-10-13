/*
 * test_runtime.h — the runtime object of the library's static kernel
 *
 * A test that does not call asx_runtime_init still runs on a runtime: the
 * library's static kernel (tag 0), which needs no initialization. Its runtime
 * object is one zeroed asx_runtime, which test_runtime() returns, so a test
 * names the runtime it runs on as every caller does:
 *
 *   asx_runtime_reset(test_runtime());
 *   ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
 *
 * Nothing writes to it: asx_runtime_reset and asx_region_open only read the
 * tag. A test that initializes a runtime of its own passes that object, from
 * its successful init to its shutdown, and this one outside that span.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TEST_RUNTIME_H
#define ASX_TEST_RUNTIME_H

#include <asx/asx.h>

static inline asx_runtime *test_runtime(void) {
    static asx_runtime rt; /* zeroed: tag 0, never initialized */
    return &rt;
}

#endif /* ASX_TEST_RUNTIME_H */
