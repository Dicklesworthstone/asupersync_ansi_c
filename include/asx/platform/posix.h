/*
 * asx/platform/posix.h — POSIX platform adapter public API
 *
 * Provides asx_posix_hooks_install() which populates asx_runtime_hooks
 * with POSIX-native implementations: monotonic clock (clock_gettime),
 * entropy (getrandom/urandom), timed reactor wait, stderr logging, and a
 * bounded blocking-submit queue.
 *
 * Usage:
 *   asx_runtime_hooks hooks;
 *   asx_posix_hooks_install(&hooks);
 *   asx_runtime_set_hooks(&hooks);
 *
 * Requires: -DASX_PROFILE_POSIX at compile time.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PLATFORM_POSIX_H
#define ASX_PLATFORM_POSIX_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/journal.h>

#define ASX_POSIX_REACTOR_READABLE 0x01u
#define ASX_POSIX_REACTOR_WRITABLE 0x02u
#define ASX_POSIX_REACTOR_ERROR 0x04u

#define ASX_POSIX_HAS_TIMED_REACTOR_WAIT 1u
/* fd readiness registration: epoll on Linux, poll(2) on other POSIX. */
#define ASX_POSIX_HAS_REACTOR_FD_REGISTRATION 1u

#ifdef __cplusplus
extern "C" {
#endif

/* Install POSIX-native hooks into the provided hook table.
 *
 * Calls asx_runtime_hooks_init() for safe defaults, then overlays:
 *   - clock: clock_gettime(CLOCK_MONOTONIC) for wall time
 *   - entropy: getrandom(2) with /dev/urandom fallback
 *   - reactor: epoll (Linux) or poll(2) fallback
 *   - log: stderr fprintf sink
 *   - blocking: bounded pthread worker queue for live-mode blocking submit
 *
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API ASX_MUST_USE asx_status asx_posix_hooks_install_checked(asx_runtime_hooks *hooks,
                                                                uint64_t application_fingerprint);
#define asx_posix_hooks_install(hooks)                                                             \
    asx_posix_hooks_install_checked((hooks), ASX_CONFIG_FINGERPRINT)

/* The platform's entropy source alone, as an entropy hook family: the same
 * OS source asx_posix_hooks_install installs (getrandom(2) with a
 * /dev/urandom fallback, fallible through try_u64_fn), without its
 * reactor, worker pool or clocks. For a hook table assembled by hand or by
 * the runtime builder, which takes it in live builds.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT if out is NULL. */
ASX_API ASX_MUST_USE asx_status asx_posix_entropy_hooks(asx_entropy_hooks *out);

/* Register (arm) a POSIX file descriptor with the reactor context installed
 * in hooks.reactor.ctx. interest is a nonzero mask of ASX_POSIX_REACTOR_*
 * bits. Readiness is one-shot: the fd reports once, then must be armed
 * again. The readiness token is the fd itself.
 *
 * Linux builds use epoll; other POSIX builds use poll(2). Returns ASX_OK,
 * ASX_E_INVALID_STATE for a NULL context, ASX_E_INVALID_ARGUMENT for bad
 * fds/interest, or ASX_E_RESOURCE_EXHAUSTED when the fd table is full. */
ASX_API ASX_MUST_USE asx_status asx_posix_reactor_register_fd(void *reactor_ctx, int fd,
                                                              uint32_t interest);

/* Deregister a file descriptor from the POSIX reactor context. Unknown or
 * already-removed descriptors are ignored so cleanup paths stay idempotent. */
ASX_API void asx_posix_reactor_deregister_fd(void *reactor_ctx, int fd);

/* Reset the POSIX reactor context, closing any platform handle it owns. */
ASX_API void asx_posix_reactor_reset(void *reactor_ctx);

/* -------------------------------------------------------------------
 * Journal file store (the POSIX profile's persistence policy,
 * asx/runtime/journal.h ASX_JOURNAL_STORE_FILE)
 *
 * The journal's sink appends the stream to a file; its commit function is
 * fsync(2), run at every commit point of ASX_JOURNAL_PERSIST_CHAIN. Wire it
 * with the store as both contexts:
 *
 *   asx_posix_journal_file file;
 *   (void)asx_posix_journal_file_open(&file, path);
 *   cfg.sink_fn = asx_posix_journal_file_sink;
 *   cfg.sink_ctx = &file;
 *   cfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
 *   cfg.commit_fn = asx_posix_journal_file_commit;
 *   ...
 *   (void)asx_journal_finish(&journal);
 *   (void)asx_posix_journal_file_close(&file);
 * ------------------------------------------------------------------- */
typedef struct {
    int fd;                   /* -1 when closed */
    uint64_t bytes;           /* stream bytes written */
    uint64_t syncs;           /* fsync(2) calls that succeeded */
    uint64_t committed_bytes; /* stream bytes at the last fsync: durable */
} asx_posix_journal_file;

/* Create `path` exclusively (mode 0600, close-on-exec): an existing file is
 * ASX_E_ALREADY_EXISTS and is never overwritten. ASX_OK, or
 * ASX_E_INVALID_ARGUMENT for NULLs or an empty path, or the status of the
 * failed open (NOT_FOUND, PERMISSION_DENIED, RESOURCE_EXHAUSTED). */
ASX_API ASX_MUST_USE asx_status asx_posix_journal_file_open(asx_posix_journal_file *f,
                                                            const char *path);

/* An asx_journal_sink_fn and an asx_journal_commit_fn; `ctx` is the
 * asx_posix_journal_file. The sink writes all `len` bytes or fails; the
 * commit function fsyncs. */
ASX_API asx_status asx_posix_journal_file_sink(void *ctx, const uint8_t *bytes, uint32_t len);
ASX_API asx_status asx_posix_journal_file_commit(void *ctx, const asx_journal_commit *commit);

/* Close the file (idempotent). ASX_OK, or the status of a failed close. */
ASX_API asx_status asx_posix_journal_file_close(asx_posix_journal_file *f);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PLATFORM_POSIX_H */
