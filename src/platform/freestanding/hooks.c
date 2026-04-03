/*
 * freestanding/hooks.c — freestanding platform adapter (minimal hooks only)
 *
 * Deferred stub status: user-supplied adapter contract.
 * The freestanding profile intentionally does not ship OS hooks here;
 * embedders install allocator/clock/entropy/reactor/log hooks through
 * runtime init or builder-provided configuration at integration time.
 * See docs/DEFERRED_STUBS_REGISTER.md.
 *
 * SPDX-License-Identifier: MIT
 */

/* Always provide at least one definition to avoid empty TU */
typedef int asx_freestanding_hooks_stub;

#ifdef ASX_PROFILE_FREESTANDING
/* Freestanding platform hooks: supplied by the embedder during runtime init. */
#endif
