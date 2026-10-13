/*
 * posix/journal_posix.c — the POSIX persistence store of the native journal
 * (bd-9kll.11.4)
 *
 * The journal's sink appends the stream to a file with write(2); its
 * commit function, run at every commit point (journal.h,
 * ASX_JOURNAL_PERSIST_CHAIN), is fsync(2). What a killed process
 * (SIGKILL) leaves in the file is everything the sink was given, which the
 * page cache keeps; what an OS crash or power loss leaves is at least the
 * prefix up to the last commit point, which is what the fsync is for. The
 * file is created exclusively (an existing journal is never overwritten)
 * and close-on-exec. Making the new directory entry durable is the
 * caller's: fsync the directory once after open.
 *
 * errno values map onto statuses as the fs backend's do:
 *   ENOENT -> NOT_FOUND, EEXIST -> ALREADY_EXISTS,
 *   EACCES/EPERM/EROFS -> PERMISSION_DENIED,
 *   anything else (ENOSPC, EDQUOT, EIO ...) -> RESOURCE_EXHAUSTED.
 *
 * Guarantee Impact: this file only persists bytes the journal already
 * produced; it reads no runtime state and a failure here is the journal's
 * sticky status, never a change to what the runtime does.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* ASX_ANALYZER_WAIVER("feature-test macro the platform headers require") */
#endif

#include <asx/platform/posix.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static asx_status journal_errno_status(int err) {
    switch (err) {
    case ENOENT: return ASX_E_NOT_FOUND;
    case EEXIST: return ASX_E_ALREADY_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS: return ASX_E_PERMISSION_DENIED;
    default: return ASX_E_RESOURCE_EXHAUSTED;
    }
}

asx_status asx_posix_journal_file_open(asx_posix_journal_file *f, const char *path) {
    int fd;
    if (f == NULL || path == NULL || path[0] == '\0') return ASX_E_INVALID_ARGUMENT;
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    do {
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return journal_errno_status(errno);
    f->fd = fd;
    return ASX_OK;
}

asx_status asx_posix_journal_file_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    asx_posix_journal_file *f = (asx_posix_journal_file *)ctx;
    uint32_t done = 0u;
    if (f == NULL || f->fd < 0 || (bytes == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    while (done < len) {
        ASX_CHECKPOINT_WAIVER("bounded: every pass writes at least one byte or leaves");
        ssize_t n = write(f->fd, bytes + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return journal_errno_status(errno);
        }
        if (n == 0) return ASX_E_RESOURCE_EXHAUSTED;
        done += (uint32_t)n;
    }
    f->bytes += (uint64_t)len;
    return ASX_OK;
}

asx_status asx_posix_journal_file_commit(void *ctx, const asx_journal_commit *commit) {
    asx_posix_journal_file *f = (asx_posix_journal_file *)ctx;
    int rv;
    if (f == NULL || f->fd < 0 || commit == NULL) return ASX_E_INVALID_ARGUMENT;
    do { rv = fsync(f->fd); } while (rv != 0 && errno == EINTR);
    if (rv != 0) return journal_errno_status(errno);
    f->syncs++;
    f->committed_bytes = commit->stream_bytes;
    return ASX_OK;
}

asx_status asx_posix_journal_file_close(asx_posix_journal_file *f) {
    int rv;
    if (f == NULL) return ASX_E_INVALID_ARGUMENT;
    if (f->fd < 0) return ASX_OK;
    rv = close(f->fd);
    f->fd = -1;
    return rv == 0 ? ASX_OK : journal_errno_status(errno);
}

#endif /* ASX_PROFILE_POSIX */
