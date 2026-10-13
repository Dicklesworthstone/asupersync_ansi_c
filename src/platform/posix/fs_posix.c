/*
 * posix/fs_posix.c — native filesystem backend for the fs API
 *
 * Real POSIX files and directories behind the asx fs surface. Every
 * descriptor is close-on-exec. Regular-file I/O never waits on readiness
 * (epoll/poll cannot watch regular files), so operations run directly on
 * the calling thread and never park the task; EINTR is retried.
 *
 * Recording and replay (bd-9kll.11.2): every call that reaches the
 * operating system is noted in the boundary journal when one is active (the
 * FS record: its inputs and its outcome, the bytes a read returned), and
 * while a replay is active each such call is answered from the journal
 * instead, touching nothing. The handle tables stay what they are, with
 * entries that hold no descriptor ("virtual": fd -1, no DIR) so a replayed
 * handle answers to the name the recording gave it. A call the table itself
 * answers (a stale handle, an access mode the open did not grant, a
 * refused offset) is the same in both and makes no record.
 *
 * Arguments are validated by fs.c before they reach this file; errno
 * values map onto the same statuses the MEMORY backend reports:
 *   ENOENT -> NOT_FOUND, EEXIST -> ALREADY_EXISTS,
 *   EACCES/EPERM/EROFS -> PERMISSION_DENIED,
 *   EISDIR/ENOTDIR/ENOTEMPTY/EBUSY/EXDEV -> INVALID_STATE,
 *   ENAMETOOLONG -> BUFFER_TOO_SMALL,
 *   ENOSPC/EDQUOT/EFBIG/EMFILE/ENFILE/ENOMEM -> RESOURCE_EXHAUSTED.
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef ASX_PROFILE_POSIX

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* ASX_ANALYZER_WAIVER("feature-test macro the platform headers require") */
#endif
#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64 /* ASX_ANALYZER_WAIVER("feature-test macro: 64-bit off_t") */
#endif

#include "../../fs/fs_native.h"
#include <asx/platform/atomics.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/journal_replay.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ASX_FS_NATIVE_MAX_FILES
#define ASX_FS_NATIVE_MAX_FILES 64u
#endif

#ifndef ASX_FS_NATIVE_MAX_DIRS
#define ASX_FS_NATIVE_MAX_DIRS 16u
#endif

/* Largest single read/write request handed to the kernel. */
#define ASX_FS_NATIVE_IO_MAX 0x40000000u

typedef struct {
    int fd;
    uint32_t generation;
    uint32_t flags;
    int in_use;
    asx_fs_path path;
} native_file;

typedef struct {
    DIR *dir;
    uint32_t generation;
    int in_use;
} native_dir;

static native_file g_files[ASX_FS_NATIVE_MAX_FILES];
static native_dir g_dirs[ASX_FS_NATIVE_MAX_DIRS];

/* Calls that ran their real implementation. A blocking job may call the
 * fs surface beside the runtime thread, so the count is atomic. */
static asx_atomic_u32 g_real_calls;

static void real_call(void) { (void)asx_atomic_u32_fetch_add(&g_real_calls, 1u); }

uint32_t asx_native_fs_real_calls(void) { return asx_atomic_u32_load(&g_real_calls); }

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t fs_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static asx_status fs_errno_status(int err) {
    switch (err) {
    case ENOENT: return ASX_E_NOT_FOUND;
    case EEXIST: return ASX_E_ALREADY_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS: return ASX_E_PERMISSION_DENIED;
    case EISDIR:
    case ENOTDIR:
    case EBUSY:
    case EXDEV: return ASX_E_INVALID_STATE;
#if defined(ENOTEMPTY) && ENOTEMPTY != EEXIST
    case ENOTEMPTY: return ASX_E_INVALID_STATE;
#endif
    case ENAMETOOLONG: return ASX_E_BUFFER_TOO_SMALL;
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
    case EFBIG:
    case EMFILE:
    case ENFILE:
    case ENOMEM: return ASX_E_RESOURCE_EXHAUSTED;
    case EINVAL:
    case EBADF:
    case ELOOP: return ASX_E_INVALID_ARGUMENT;
    default: return ASX_E_INVALID_STATE;
    }
}

/* Largest offset representable in off_t. */
static int fs_offset_fits(uint64_t offset) {
    uint64_t max = ((uint64_t)1 << (sizeof(off_t) * 8u - 1u)) - 1u;
    return offset <= max;
}

static native_file *fs_file_lookup(asx_file_handle h) {
    uint32_t idx;
    if (!asx_fs_slot_is_native(h.slot)) return NULL;
    idx = h.slot & ~ASX_FS_NATIVE_SLOT_BIT;
    if (idx >= ASX_FS_NATIVE_MAX_FILES) return NULL;
    if (!g_files[idx].in_use || g_files[idx].generation != h.generation) return NULL;
    return &g_files[idx];
}

static native_dir *fs_dir_lookup(asx_fs_dir_handle h) {
    uint32_t idx;
    if (!asx_fs_slot_is_native(h.slot)) return NULL;
    idx = h.slot & ~ASX_FS_NATIVE_SLOT_BIT;
    if (idx >= ASX_FS_NATIVE_MAX_DIRS) return NULL;
    if (!g_dirs[idx].in_use || g_dirs[idx].generation != h.generation) return NULL;
    return &g_dirs[idx];
}

static asx_fs_entry_kind fs_kind_from_mode(mode_t mode) {
    if (S_ISREG(mode)) return ASX_FS_ENTRY_FILE;
    if (S_ISDIR(mode)) return ASX_FS_ENTRY_DIR;
    if (S_ISLNK(mode)) return ASX_FS_ENTRY_SYMLINK;
    return ASX_FS_ENTRY_OTHER;
}

static void fs_fill_meta(const struct stat *st, asx_fs_metadata *out) {
    const struct timespec *mt;
    memset(out, 0, sizeof(*out));
    out->kind = fs_kind_from_mode(st->st_mode);
    /* Content length: regular files only (directories report 0, like
     * the MEMORY backend). */
    out->size = (S_ISREG(st->st_mode) && st->st_size > 0) ? (uint64_t)st->st_size : 0u;
    out->exists = 1;
    out->mode = (uint32_t)(st->st_mode & 07777);
    out->writable = (st->st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
#if defined(__APPLE__)
    mt = &st->st_mtimespec;
#else
    mt = &st->st_mtim;
#endif
    if (mt->tv_sec >= 0) {
        out->modified_ns =
            (uint64_t)mt->tv_sec * 1000000000ULL + (uint64_t)(mt->tv_nsec > 0 ? mt->tv_nsec : 0);
    }
}

static asx_status fs_close_fd(int fd) {
    /* Never retry close() on EINTR: the descriptor is already released. */
    if (close(fd) != 0 && errno != EINTR) return fs_errno_status(errno);
    return ASX_OK;
}

/* mkdir that treats an existing directory as success. */
static asx_status fs_mkdir_one(const char *path) {
    struct stat st;
    if (mkdir(path, 0777) == 0) return ASX_OK;
    if (errno != EEXIST) return fs_errno_status(errno);
    if (stat(path, &st) != 0) return fs_errno_status(errno);
    return S_ISDIR(st.st_mode) ? ASX_OK : ASX_E_ALREADY_EXISTS;
}

/* ------------------------------------------------------------------ */
/* Journal and replay                                                  */
/* ------------------------------------------------------------------ */

/* The journal's name for a handle: slot (native bit included), generation. */
static uint64_t fs_handle_id(uint32_t slot, uint32_t generation) {
    return ((uint64_t)slot << 32) | (uint64_t)generation;
}

static uint64_t fs_path_key(const asx_fs_path *path) { return asx_journal_text_hash(path->text); }

/* A metadata result as the FS record carries it: kind, exists, writable, 0,
 * size, modified_ns, mode, little-endian. */
#define FS_META_BYTES 24u

static void fs_put_le(uint8_t *p, uint64_t v, uint32_t bytes) {
    uint32_t i;
    for (i = 0; i < bytes; i++) { p[i] = (uint8_t)(v >> (8u * i)); }
}

static uint64_t fs_get_le(const uint8_t *p, uint32_t bytes) {
    uint64_t v = 0u;
    uint32_t i;
    for (i = 0; i < bytes; i++) { v |= (uint64_t)p[i] << (8u * i); }
    return v;
}

static void fs_meta_pack(const asx_fs_metadata *m, uint8_t *b) {
    b[0] = (uint8_t)m->kind;
    b[1] = (uint8_t)(m->exists != 0);
    b[2] = (uint8_t)(m->writable != 0);
    b[3] = 0u;
    fs_put_le(b + 4, m->size, 8u);
    fs_put_le(b + 12, m->modified_ns, 8u);
    fs_put_le(b + 20, m->mode, 4u);
}

static void fs_meta_unpack(const uint8_t *b, asx_fs_metadata *m) {
    memset(m, 0, sizeof(*m));
    m->kind = (asx_fs_entry_kind)b[0];
    m->exists = b[1] != 0u;
    m->writable = b[2] != 0u;
    m->size = fs_get_le(b + 4, 8u);
    m->modified_ns = fs_get_le(b + 12, 8u);
    m->mode = (uint32_t)fs_get_le(b + 20, 4u);
}

/* While a replay is active a call that reaches the OS is answered from the
 * journal: 1 when it was (*st is the recorded status, *result its result,
 * `dst` its data and *n their count), 0 when the call is real. */
static int fs_serve(uint8_t op, uint64_t key, uint64_t arg, uint32_t len, asx_status *st,
                    uint64_t *result, uint8_t *dst, uint32_t cap, uint32_t *n) {
    int32_t status = 0;
    *st = ASX_OK;
    *result = 0u;
    *n = 0u;
    if (!asx_replay_active()) return 0;
    (void)asx_replay_serve_fs(op, key, arg, len, &status, result, dst, cap, n);
    *st = (asx_status)status;
    return 1;
}

/* A call with a status and nothing else. */
static asx_status fs_status_call(uint8_t op, uint64_t key, uint64_t arg, int *served) {
    asx_status st = ASX_OK;
    uint64_t result = 0u;
    uint32_t n = 0u;
    *served = fs_serve(op, key, arg, 0u, &st, &result, NULL, 0u, &n);
    return st;
}

static void fs_note_status(uint8_t op, uint64_t key, uint64_t arg, asx_status st) {
    asx_journal_note_fs(op, key, arg, 0u, (int32_t)st, 0u, NULL, 0u);
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_native_fs_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_FS_NATIVE_MAX_FILES; i++) {
        if (g_files[i].in_use && g_files[i].fd >= 0) (void)fs_close_fd(g_files[i].fd);
        g_files[i].in_use = 0;
        g_files[i].fd = -1;
        g_files[i].generation = fs_next_gen(g_files[i].generation);
    }
    for (i = 0; i < ASX_FS_NATIVE_MAX_DIRS; i++) {
        if (g_dirs[i].in_use && g_dirs[i].dir != NULL) (void)closedir(g_dirs[i].dir);
        g_dirs[i].in_use = 0;
        g_dirs[i].dir = NULL;
        g_dirs[i].generation = fs_next_gen(g_dirs[i].generation);
    }
}

/* ------------------------------------------------------------------ */
/* Path operations                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_native_fs_dir_create(const asx_fs_path *path) {
    uint64_t key = fs_path_key(path);
    int served;
    asx_status st = fs_status_call(ASX_JOURNAL_FS_DIR_CREATE, key, 0u, &served);
    if (served) return st;
    real_call();
    st = fs_mkdir_one(path->text);
    fs_note_status(ASX_JOURNAL_FS_DIR_CREATE, key, 0u, st);
    return st;
}

static asx_status fs_mkdir_all(const asx_fs_path *path) {
    char buf[ASX_FS_PATH_MAX];
    uint32_t len = path->len;
    uint32_t i;
    asx_status st;

    memcpy(buf, path->text, (size_t)len + 1u);
    while (len > 1u && buf[len - 1u] == '/') buf[--len] = '\0';
    for (i = 1u; i < len; i++) {
        if (buf[i] != '/' || buf[i - 1u] == '/') continue;
        buf[i] = '\0';
        st = fs_mkdir_one(buf);
        buf[i] = '/';
        if (st != ASX_OK) return st;
    }
    return fs_mkdir_one(buf);
}

asx_status asx_native_fs_dir_create_all(const asx_fs_path *path) {
    uint64_t key = fs_path_key(path);
    int served;
    asx_status st = fs_status_call(ASX_JOURNAL_FS_DIR_CREATE_ALL, key, 0u, &served);
    if (served) return st;
    real_call();
    st = fs_mkdir_all(path);
    fs_note_status(ASX_JOURNAL_FS_DIR_CREATE_ALL, key, 0u, st);
    return st;
}

asx_status asx_native_fs_metadata(const asx_fs_path *path, asx_fs_metadata *out) {
    uint64_t key = fs_path_key(path);
    uint8_t packed[FS_META_BYTES];
    uint64_t result = 0u;
    uint32_t n = 0u;
    asx_status rst = ASX_OK;
    struct stat st;

    if (fs_serve(ASX_JOURNAL_FS_METADATA, key, 0u, 0u, &rst, &result, packed, sizeof(packed), &n)) {
        if (rst != ASX_OK) return rst;
        if (n != FS_META_BYTES) return ASX_E_REPLAY_MISMATCH;
        fs_meta_unpack(packed, out);
        return ASX_OK;
    }
    real_call();
    if (stat(path->text, &st) != 0) {
        rst = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_METADATA, key, 0u, 0u, (int32_t)rst, 0u, NULL, 0u);
        return rst;
    }
    fs_fill_meta(&st, out);
    fs_meta_pack(out, packed);
    asx_journal_note_fs(ASX_JOURNAL_FS_METADATA, key, 0u, 0u, (int32_t)ASX_OK, 0u, packed,
                        FS_META_BYTES);
    return ASX_OK;
}

asx_status asx_native_fs_remove_file(const asx_fs_path *path) {
    uint64_t key = fs_path_key(path);
    struct stat sb;
    int served;
    asx_status st = fs_status_call(ASX_JOURNAL_FS_REMOVE_FILE, key, 0u, &served);
    if (served) return st;
    real_call();
    if (unlink(path->text) == 0) {
        st = ASX_OK;
    } else if ((errno == EISDIR || errno == EPERM) && lstat(path->text, &sb) == 0 &&
               S_ISDIR(sb.st_mode)) {
        st = ASX_E_INVALID_STATE;
    } else {
        st = fs_errno_status(errno);
    }
    fs_note_status(ASX_JOURNAL_FS_REMOVE_FILE, key, 0u, st);
    return st;
}

asx_status asx_native_fs_remove_dir(const asx_fs_path *path) {
    uint64_t key = fs_path_key(path);
    int served;
    asx_status st = fs_status_call(ASX_JOURNAL_FS_REMOVE_DIR, key, 0u, &served);
    if (served) return st;
    real_call();
    if (rmdir(path->text) == 0) {
        st = ASX_OK;
    } else if (errno == EEXIST) {
        st = ASX_E_INVALID_STATE; /* non-empty on some systems */
    } else {
        st = fs_errno_status(errno);
    }
    fs_note_status(ASX_JOURNAL_FS_REMOVE_DIR, key, 0u, st);
    return st;
}

asx_status asx_native_fs_rename(const asx_fs_path *from, const asx_fs_path *to) {
    uint64_t key = fs_path_key(from);
    uint64_t to_key = fs_path_key(to);
    int served;
    asx_status st = fs_status_call(ASX_JOURNAL_FS_RENAME, key, to_key, &served);
    if (served) return st;
    real_call();
    if (rename(from->text, to->text) == 0) {
        st = ASX_OK;
    } else if (errno == EEXIST) {
        st = ASX_E_INVALID_STATE; /* non-empty target directory */
    } else {
        st = fs_errno_status(errno);
    }
    fs_note_status(ASX_JOURNAL_FS_RENAME, key, to_key, st);
    return st;
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                 */
/* ------------------------------------------------------------------ */

static asx_status fs_dir_open_real(uint32_t idx, const asx_fs_path *path, asx_fs_dir_handle *out) {
    int fd;
    DIR *d;

    do {
        fd = open(path->text, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return fs_errno_status(errno);
    d = fdopendir(fd);
    if (d == NULL) {
        asx_status st = fs_errno_status(errno);
        (void)close(fd);
        return st;
    }
    g_dirs[idx].dir = d;
    g_dirs[idx].in_use = 1;
    g_dirs[idx].generation = fs_next_gen(g_dirs[idx].generation);
    out->slot = idx | ASX_FS_NATIVE_SLOT_BIT;
    out->generation = g_dirs[idx].generation;
    return ASX_OK;
}

asx_status asx_native_fs_dir_open(const asx_fs_path *path, asx_fs_dir_handle *out) {
    uint64_t key = fs_path_key(path);
    uint32_t idx;
    asx_status st;

    for (idx = 0; idx < ASX_FS_NATIVE_MAX_DIRS; idx++) {
        if (!g_dirs[idx].in_use) break;
    }
    if (idx >= ASX_FS_NATIVE_MAX_DIRS) return ASX_E_RESOURCE_EXHAUSTED;

    if (asx_replay_active()) {
        /* The handle is the recording's, in a table that mirrors it. */
        uint64_t result = 0u;
        uint32_t n = 0u;
        uint32_t slot;
        (void)fs_serve(ASX_JOURNAL_FS_DIR_OPEN, key, 0u, 0u, &st, &result, NULL, 0u, &n);
        if (st != ASX_OK) return st;
        slot = (uint32_t)(result >> 32);
        idx = slot & ~ASX_FS_NATIVE_SLOT_BIT;
        if (!asx_fs_slot_is_native(slot) || idx >= ASX_FS_NATIVE_MAX_DIRS || g_dirs[idx].in_use) {
            return ASX_E_REPLAY_MISMATCH;
        }
        g_dirs[idx].dir = NULL;
        g_dirs[idx].in_use = 1;
        g_dirs[idx].generation = (uint32_t)result;
        out->slot = slot;
        out->generation = (uint32_t)result;
        return ASX_OK;
    }
    real_call();
    st = fs_dir_open_real(idx, path, out);
    asx_journal_note_fs(ASX_JOURNAL_FS_DIR_OPEN, key, 0u, 0u, (int32_t)st,
                        st == ASX_OK ? fs_handle_id(out->slot, out->generation) : 0u, NULL, 0u);
    return st;
}

/* The kind of an entry whose d_type does not say, from its status. 0 when
 * the entry was removed while iterating (the caller skips it); a status
 * that fails otherwise leaves the kind as it was (OTHER). */
static int fs_entry_kind_by_stat(native_dir *d, const struct dirent *ent, asx_fs_entry_kind *kind) {
    struct stat st;
    if (fstatat(dirfd(d->dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        *kind = fs_kind_from_mode(st.st_mode);
        return 1;
    }
    return errno != ENOENT;
}

static asx_status fs_dir_next_real(native_dir *d, asx_fs_dir_entry *out, int *out_has_entry) {
    struct dirent *ent;

    for (;;) {
        size_t len;
        asx_fs_entry_kind kind = ASX_FS_ENTRY_OTHER;

        errno = 0;
        ent = readdir(d->dir);
        if (ent == NULL) return errno == 0 ? ASX_OK : fs_errno_status(errno);
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        len = strlen(ent->d_name);
        if (len >= ASX_FS_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(DT_UNKNOWN)
        switch (ent->d_type) {
        case DT_REG: kind = ASX_FS_ENTRY_FILE; break;
        case DT_DIR: kind = ASX_FS_ENTRY_DIR; break;
        case DT_LNK: kind = ASX_FS_ENTRY_SYMLINK; break;
        default:
            if (!fs_entry_kind_by_stat(d, ent, &kind)) continue;
            break;
        }
#else
        if (!fs_entry_kind_by_stat(d, ent, &kind)) continue;
#endif
        memset(out, 0, sizeof(*out));
        memcpy(out->name, ent->d_name, len);
        out->name_len = (uint32_t)len;
        out->kind = kind;
        *out_has_entry = 1;
        return ASX_OK;
    }
}

asx_status asx_native_fs_dir_next(asx_fs_dir_handle dir, asx_fs_dir_entry *out,
                                  int *out_has_entry) {
    native_dir *d = fs_dir_lookup(dir);
    uint64_t key;
    asx_status st;

    if (d == NULL) return ASX_E_NOT_FOUND;
    key = fs_handle_id(dir.slot, dir.generation);
    if (asx_replay_active()) {
        uint8_t name[ASX_FS_NAME_MAX];
        uint64_t result = 0u;
        uint32_t n = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_DIR_NEXT, key, 0u, 0u, &st, &result, name, sizeof(name), &n);
        if (st != ASX_OK) return st;
        if ((result & 1u) != 0u) {
            if (n >= ASX_FS_NAME_MAX) return ASX_E_REPLAY_MISMATCH;
            memset(out, 0, sizeof(*out));
            memcpy(out->name, name, n);
            out->name_len = n;
            out->kind = (asx_fs_entry_kind)((result >> 8) & 0xFFu);
            *out_has_entry = 1;
        }
        return ASX_OK;
    }
    real_call();
    {
        int has = 0;
        asx_fs_dir_entry entry;
        st = fs_dir_next_real(d, &entry, &has);
        if (st == ASX_OK && has) {
            *out = entry;
            *out_has_entry = 1;
            asx_journal_note_fs(ASX_JOURNAL_FS_DIR_NEXT, key, 0u, 0u, (int32_t)ASX_OK,
                                1u | ((uint64_t)entry.kind << 8), (const uint8_t *)entry.name,
                                entry.name_len);
        } else {
            asx_journal_note_fs(ASX_JOURNAL_FS_DIR_NEXT, key, 0u, 0u, (int32_t)st, 0u, NULL, 0u);
        }
    }
    return st;
}

asx_status asx_native_fs_dir_close(asx_fs_dir_handle dir) {
    native_dir *d = fs_dir_lookup(dir);
    uint64_t key;
    int served;
    asx_status st;
    if (d == NULL) return ASX_E_NOT_FOUND;
    key = fs_handle_id(dir.slot, dir.generation);
    st = fs_status_call(ASX_JOURNAL_FS_DIR_CLOSE, key, 0u, &served);
    if (!served) {
        real_call();
        if (d->dir != NULL) (void)closedir(d->dir);
        st = ASX_OK;
        fs_note_status(ASX_JOURNAL_FS_DIR_CLOSE, key, 0u, st);
    }
    d->dir = NULL;
    d->in_use = 0;
    return st;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static asx_status fs_open_real(uint32_t idx, const asx_fs_path *path, uint32_t flags,
                               asx_file_handle *out) {
    int oflags = O_CLOEXEC;
    int want_read = (flags & (uint32_t)ASX_FS_OPEN_READ) != 0u;
    int want_write = (flags & ((uint32_t)ASX_FS_OPEN_WRITE | (uint32_t)ASX_FS_OPEN_APPEND)) != 0u;
    struct stat st;
    int fd;

    if (want_read && want_write) {
        oflags |= O_RDWR;
    } else if (want_write) {
        oflags |= O_WRONLY;
    } else {
        oflags |= O_RDONLY;
    }
    if ((flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) oflags |= O_APPEND;
    if ((flags & (uint32_t)ASX_FS_OPEN_CREATE) != 0u) oflags |= O_CREAT;
    if ((flags & (uint32_t)ASX_FS_OPEN_EXCLUSIVE) != 0u) oflags |= O_CREAT | O_EXCL;
    if ((flags & (uint32_t)ASX_FS_OPEN_TRUNC) != 0u) oflags |= O_TRUNC;

    do { fd = open(path->text, oflags, 0666); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return fs_errno_status(errno);
    /* O_RDONLY succeeds on directories; files only, like MEMORY. */
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return ASX_E_INVALID_STATE;
    }

    g_files[idx].fd = fd;
    g_files[idx].flags = flags;
    g_files[idx].path = *path;
    g_files[idx].in_use = 1;
    g_files[idx].generation = fs_next_gen(g_files[idx].generation);
    out->slot = idx | ASX_FS_NATIVE_SLOT_BIT;
    out->generation = g_files[idx].generation;
    return ASX_OK;
}

asx_status asx_native_fs_open(const asx_fs_path *path, uint32_t flags, asx_file_handle *out) {
    uint64_t key = fs_path_key(path);
    uint32_t idx;
    asx_status st;

    /* Claim a slot first so exhaustion never leaves a created file. */
    for (idx = 0; idx < ASX_FS_NATIVE_MAX_FILES; idx++) {
        if (!g_files[idx].in_use) break;
    }
    if (idx >= ASX_FS_NATIVE_MAX_FILES) return ASX_E_RESOURCE_EXHAUSTED;

    if (asx_replay_active()) {
        /* The handle is the recording's, in a table that mirrors it; the
         * entry holds no descriptor. */
        uint64_t result = 0u;
        uint32_t n = 0u;
        uint32_t slot;
        (void)fs_serve(ASX_JOURNAL_FS_OPEN, key, (uint64_t)flags, 0u, &st, &result, NULL, 0u, &n);
        if (st != ASX_OK) return st;
        slot = (uint32_t)(result >> 32);
        idx = slot & ~ASX_FS_NATIVE_SLOT_BIT;
        if (!asx_fs_slot_is_native(slot) || idx >= ASX_FS_NATIVE_MAX_FILES || g_files[idx].in_use) {
            return ASX_E_REPLAY_MISMATCH;
        }
        g_files[idx].fd = -1;
        g_files[idx].flags = flags;
        g_files[idx].path = *path;
        g_files[idx].in_use = 1;
        g_files[idx].generation = (uint32_t)result;
        out->slot = slot;
        out->generation = (uint32_t)result;
        return ASX_OK;
    }
    real_call();
    st = fs_open_real(idx, path, flags, out);
    asx_journal_note_fs(ASX_JOURNAL_FS_OPEN, key, (uint64_t)flags, 0u, (int32_t)st,
                        st == ASX_OK ? fs_handle_id(out->slot, out->generation) : 0u, NULL, 0u);
    return st;
}

asx_status asx_native_fs_check(asx_file_handle file, int want_read, int want_write) {
    native_file *f = fs_file_lookup(file);
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (want_read && (f->flags & (uint32_t)ASX_FS_OPEN_READ) == 0u) return ASX_E_PERMISSION_DENIED;
    if (want_write &&
        (f->flags & ((uint32_t)ASX_FS_OPEN_WRITE | (uint32_t)ASX_FS_OPEN_APPEND)) == 0u) {
        return ASX_E_PERMISSION_DENIED;
    }
    return ASX_OK;
}

asx_status asx_native_fs_read(asx_file_handle file, uint8_t *dst, uint32_t cap,
                              uint32_t *out_read) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    ssize_t n;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (cap > ASX_FS_NATIVE_IO_MAX) cap = ASX_FS_NATIVE_IO_MAX;
    key = fs_handle_id(file.slot, file.generation);
    if (asx_replay_active()) {
        uint64_t result = 0u;
        uint32_t got = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_READ, key, 0u, cap, &st, &result, dst, cap, &got);
        if (st == ASX_OK) *out_read = got;
        return st;
    }
    real_call();
    do { n = read(f->fd, dst, (size_t)cap); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        st = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_READ, key, 0u, cap, (int32_t)st, 0u, NULL, 0u);
        return st;
    }
    *out_read = (uint32_t)n;
    asx_journal_note_fs(ASX_JOURNAL_FS_READ, key, 0u, cap, (int32_t)ASX_OK, 0u, dst, (uint32_t)n);
    return ASX_OK;
}

asx_status asx_native_fs_write(asx_file_handle file, const uint8_t *src, uint32_t len,
                               uint32_t *out_written) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    ssize_t n;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (len > ASX_FS_NATIVE_IO_MAX) len = ASX_FS_NATIVE_IO_MAX;
    key = fs_handle_id(file.slot, file.generation);
    if (asx_replay_active()) {
        uint64_t result = 0u;
        uint32_t none = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_WRITE, key, 0u, len, &st, &result, NULL, 0u, &none);
        if (st == ASX_OK) *out_written = (uint32_t)result;
        return st;
    }
    real_call();
    do { n = write(f->fd, src, (size_t)len); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        st = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_WRITE, key, 0u, len, (int32_t)st, 0u, NULL, 0u);
        return st;
    }
    *out_written = (uint32_t)n;
    asx_journal_note_fs(ASX_JOURNAL_FS_WRITE, key, 0u, len, (int32_t)ASX_OK, (uint64_t)n, NULL, 0u);
    return ASX_OK;
}

asx_status asx_native_fs_read_at(asx_file_handle file, uint64_t offset, uint8_t *dst, uint32_t cap,
                                 uint32_t *out_read) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    ssize_t n;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (!fs_offset_fits(offset)) return ASX_OK; /* beyond any possible end-of-file */
    if (cap > ASX_FS_NATIVE_IO_MAX) cap = ASX_FS_NATIVE_IO_MAX;
    key = fs_handle_id(file.slot, file.generation);
    if (asx_replay_active()) {
        uint64_t result = 0u;
        uint32_t got = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_READ_AT, key, offset, cap, &st, &result, dst, cap, &got);
        if (st == ASX_OK) *out_read = got;
        return st;
    }
    real_call();
    do { n = pread(f->fd, dst, (size_t)cap, (off_t)offset); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        st = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_READ_AT, key, offset, cap, (int32_t)st, 0u, NULL, 0u);
        return st;
    }
    *out_read = (uint32_t)n;
    asx_journal_note_fs(ASX_JOURNAL_FS_READ_AT, key, offset, cap, (int32_t)ASX_OK, 0u, dst,
                        (uint32_t)n);
    return ASX_OK;
}

asx_status asx_native_fs_write_at(asx_file_handle file, uint64_t offset, const uint8_t *src,
                                  uint32_t len, uint32_t *out_written) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    ssize_t n;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    /* Linux pwrite() appends on O_APPEND descriptors; refuse, like MEMORY. */
    if ((f->flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) return ASX_E_INVALID_STATE;
    if (!fs_offset_fits(offset)) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_FS_NATIVE_IO_MAX) len = ASX_FS_NATIVE_IO_MAX;
    key = fs_handle_id(file.slot, file.generation);
    if (asx_replay_active()) {
        uint64_t result = 0u;
        uint32_t none = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_WRITE_AT, key, offset, len, &st, &result, NULL, 0u, &none);
        if (st == ASX_OK) *out_written = (uint32_t)result;
        return st;
    }
    real_call();
    do { n = pwrite(f->fd, src, (size_t)len, (off_t)offset); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        st = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_WRITE_AT, key, offset, len, (int32_t)st, 0u, NULL, 0u);
        return st;
    }
    *out_written = (uint32_t)n;
    asx_journal_note_fs(ASX_JOURNAL_FS_WRITE_AT, key, offset, len, (int32_t)ASX_OK, (uint64_t)n,
                        NULL, 0u);
    return ASX_OK;
}

asx_status asx_native_fs_seek(asx_file_handle file, int64_t offset, asx_fs_seek_whence whence,
                              uint64_t *out_pos) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    int how;
    off_t pos;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    switch (whence) {
    case ASX_FS_SEEK_CUR: how = SEEK_CUR; break;
    case ASX_FS_SEEK_END: how = SEEK_END; break;
    case ASX_FS_SEEK_SET:
    default: how = SEEK_SET; break;
    }
    /* -(offset + 1) cannot overflow and fits iff offset >= OFF_T_MIN. */
    if (!fs_offset_fits(offset < 0 ? (uint64_t)(-(offset + 1)) : (uint64_t)offset)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    key = fs_handle_id(file.slot, file.generation);
    if (asx_replay_active()) {
        uint64_t result = 0u;
        uint32_t none = 0u;
        (void)fs_serve(ASX_JOURNAL_FS_SEEK, key, (uint64_t)offset, (uint32_t)how, &st, &result,
                       NULL, 0u, &none);
        if (st == ASX_OK && out_pos != NULL) *out_pos = result;
        return st;
    }
    real_call();
    pos = lseek(f->fd, (off_t)offset, how);
    if (pos < 0) {
        st = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_SEEK, key, (uint64_t)offset, (uint32_t)how, (int32_t)st,
                            0u, NULL, 0u);
        return st;
    }
    if (out_pos != NULL) *out_pos = (uint64_t)pos;
    asx_journal_note_fs(ASX_JOURNAL_FS_SEEK, key, (uint64_t)offset, (uint32_t)how, (int32_t)ASX_OK,
                        (uint64_t)pos, NULL, 0u);
    return ASX_OK;
}

asx_status asx_native_fs_sync(asx_file_handle file) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    int served;
    int rv;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    key = fs_handle_id(file.slot, file.generation);
    st = fs_status_call(ASX_JOURNAL_FS_SYNC, key, 0u, &served);
    if (served) return st;
    real_call();
    do { rv = fsync(f->fd); } while (rv != 0 && errno == EINTR);
    st = rv == 0 ? ASX_OK : fs_errno_status(errno);
    fs_note_status(ASX_JOURNAL_FS_SYNC, key, 0u, st);
    return st;
}

asx_status asx_native_fs_set_len(asx_file_handle file, uint64_t len) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    int served;
    int rv;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (!fs_offset_fits(len)) return ASX_E_RESOURCE_EXHAUSTED;
    key = fs_handle_id(file.slot, file.generation);
    st = fs_status_call(ASX_JOURNAL_FS_SET_LEN, key, len, &served);
    if (served) return st;
    real_call();
    do { rv = ftruncate(f->fd, (off_t)len); } while (rv != 0 && errno == EINTR);
    st = rv == 0 ? ASX_OK : fs_errno_status(errno);
    fs_note_status(ASX_JOURNAL_FS_SET_LEN, key, len, st);
    return st;
}

asx_status asx_native_fs_file_metadata(asx_file_handle file, asx_fs_metadata *out) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    uint8_t packed[FS_META_BYTES];
    uint64_t result = 0u;
    uint32_t n = 0u;
    asx_status rst = ASX_OK;
    struct stat st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    key = fs_handle_id(file.slot, file.generation);
    if (fs_serve(ASX_JOURNAL_FS_FILE_METADATA, key, 0u, 0u, &rst, &result, packed, sizeof(packed),
                 &n)) {
        if (rst != ASX_OK) return rst;
        if (n != FS_META_BYTES) return ASX_E_REPLAY_MISMATCH;
        fs_meta_unpack(packed, out);
        return ASX_OK;
    }
    real_call();
    if (fstat(f->fd, &st) != 0) {
        rst = fs_errno_status(errno);
        asx_journal_note_fs(ASX_JOURNAL_FS_FILE_METADATA, key, 0u, 0u, (int32_t)rst, 0u, NULL, 0u);
        return rst;
    }
    fs_fill_meta(&st, out);
    fs_meta_pack(out, packed);
    asx_journal_note_fs(ASX_JOURNAL_FS_FILE_METADATA, key, 0u, 0u, (int32_t)ASX_OK, 0u, packed,
                        FS_META_BYTES);
    return ASX_OK;
}

asx_status asx_native_fs_close(asx_file_handle file) {
    native_file *f = fs_file_lookup(file);
    uint64_t key;
    int served;
    int fd;
    asx_status st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    key = fs_handle_id(file.slot, file.generation);
    st = fs_status_call(ASX_JOURNAL_FS_CLOSE, key, 0u, &served);
    fd = f->fd;
    f->in_use = 0;
    f->fd = -1;
    if (served) return st;
    real_call();
    st = fs_close_fd(fd);
    fs_note_status(ASX_JOURNAL_FS_CLOSE, key, 0u, st);
    return st;
}

asx_status asx_native_fs_file_path(asx_file_handle file, asx_fs_path *out) {
    native_file *f = fs_file_lookup(file);
    if (f == NULL) return ASX_E_NOT_FOUND;
    *out = f->path;
    return ASX_OK;
}

int asx_native_fs_is_alive(asx_file_handle file) { return fs_file_lookup(file) != NULL; }

#else
typedef int asx_fs_posix_empty_translation_unit;
#endif /* ASX_PROFILE_POSIX */
