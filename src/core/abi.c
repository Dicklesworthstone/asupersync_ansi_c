/*
 * abi.c — ABI version identity functions (bd-56t.4)
 *
 * Provides runtime-queryable ABI version for shared library consumers.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Pure version constants, no loops") */

#include <asx/asx_abi.h>

unsigned int asx_abi_version_major(void) { return ASX_ABI_VERSION_MAJOR; }

unsigned int asx_abi_version_minor(void) { return ASX_ABI_VERSION_MINOR; }

unsigned int asx_abi_version_patch(void) { return ASX_ABI_VERSION_PATCH; }
