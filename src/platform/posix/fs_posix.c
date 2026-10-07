/*
 * posix/fs_posix.c — native filesystem backend for the fs API
 *
 * Real POSIX files and directories behind the asx fs surface. Every
 * descriptor is close-on-exec. Regular-file I/O never waits on readiness
 * (epoll/poll cannot watch regular files), so operations run directly on
 * the calling thread and never park the task; EINTR is retried.
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
#define _GNU_SOURCE
#endif
#if !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "../../fs/fs_native.h"
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
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_native_fs_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_FS_NATIVE_MAX_FILES; i++) {
        if (g_files[i].in_use) (void)fs_close_fd(g_files[i].fd);
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

asx_status asx_native_fs_dir_create(const asx_fs_path *path) { return fs_mkdir_one(path->text); }

asx_status asx_native_fs_dir_create_all(const asx_fs_path *path) {
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

asx_status asx_native_fs_metadata(const asx_fs_path *path, asx_fs_metadata *out) {
    struct stat st;
    if (stat(path->text, &st) != 0) return fs_errno_status(errno);
    fs_fill_meta(&st, out);
    return ASX_OK;
}

asx_status asx_native_fs_remove_file(const asx_fs_path *path) {
    struct stat st;
    if (unlink(path->text) == 0) return ASX_OK;
    if ((errno == EISDIR || errno == EPERM) && lstat(path->text, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ASX_E_INVALID_STATE;
    }
    return fs_errno_status(errno);
}

asx_status asx_native_fs_remove_dir(const asx_fs_path *path) {
    if (rmdir(path->text) == 0) return ASX_OK;
    if (errno == EEXIST) return ASX_E_INVALID_STATE; /* non-empty on some systems */
    return fs_errno_status(errno);
}

asx_status asx_native_fs_rename(const asx_fs_path *from, const asx_fs_path *to) {
    if (rename(from->text, to->text) == 0) return ASX_OK;
    if (errno == EEXIST) return ASX_E_INVALID_STATE; /* non-empty target directory */
    return fs_errno_status(errno);
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_native_fs_dir_open(const asx_fs_path *path, asx_fs_dir_handle *out) {
    uint32_t idx;
    int fd;
    DIR *d;

    for (idx = 0; idx < ASX_FS_NATIVE_MAX_DIRS; idx++) {
        if (!g_dirs[idx].in_use) break;
    }
    if (idx >= ASX_FS_NATIVE_MAX_DIRS) return ASX_E_RESOURCE_EXHAUSTED;

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

asx_status asx_native_fs_dir_next(asx_fs_dir_handle dir, asx_fs_dir_entry *out,
                                  int *out_has_entry) {
    native_dir *d = fs_dir_lookup(dir);
    struct dirent *ent;

    if (d == NULL) return ASX_E_NOT_FOUND;
    for (;;) {
        size_t len;
        asx_fs_entry_kind kind = ASX_FS_ENTRY_OTHER;
        int known = 0;

        errno = 0;
        ent = readdir(d->dir);
        if (ent == NULL) return errno == 0 ? ASX_OK : fs_errno_status(errno);
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        len = strlen(ent->d_name);
        if (len >= ASX_FS_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(DT_UNKNOWN)
        switch (ent->d_type) {
        case DT_REG:
            kind = ASX_FS_ENTRY_FILE;
            known = 1;
            break;
        case DT_DIR:
            kind = ASX_FS_ENTRY_DIR;
            known = 1;
            break;
        case DT_LNK:
            kind = ASX_FS_ENTRY_SYMLINK;
            known = 1;
            break;
        default: break;
        }
#endif
        if (!known) {
            struct stat st;
            if (fstatat(dirfd(d->dir), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                kind = fs_kind_from_mode(st.st_mode);
            } else if (errno == ENOENT) {
                continue; /* removed while iterating */
            }
        }
        memset(out, 0, sizeof(*out));
        memcpy(out->name, ent->d_name, len);
        out->name_len = (uint32_t)len;
        out->kind = kind;
        *out_has_entry = 1;
        return ASX_OK;
    }
}

asx_status asx_native_fs_dir_close(asx_fs_dir_handle dir) {
    native_dir *d = fs_dir_lookup(dir);
    if (d == NULL) return ASX_E_NOT_FOUND;
    (void)closedir(d->dir);
    d->dir = NULL;
    d->in_use = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_native_fs_open(const asx_fs_path *path, uint32_t flags, asx_file_handle *out) {
    uint32_t idx;
    int oflags = O_CLOEXEC;
    int want_read = (flags & (uint32_t)ASX_FS_OPEN_READ) != 0u;
    int want_write = (flags & ((uint32_t)ASX_FS_OPEN_WRITE | (uint32_t)ASX_FS_OPEN_APPEND)) != 0u;
    struct stat st;
    int fd;

    /* Claim a slot first so exhaustion never leaves a created file. */
    for (idx = 0; idx < ASX_FS_NATIVE_MAX_FILES; idx++) {
        if (!g_files[idx].in_use) break;
    }
    if (idx >= ASX_FS_NATIVE_MAX_FILES) return ASX_E_RESOURCE_EXHAUSTED;

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
    ssize_t n;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (cap > ASX_FS_NATIVE_IO_MAX) cap = ASX_FS_NATIVE_IO_MAX;
    do { n = read(f->fd, dst, (size_t)cap); } while (n < 0 && errno == EINTR);
    if (n < 0) return fs_errno_status(errno);
    *out_read = (uint32_t)n;
    return ASX_OK;
}

asx_status asx_native_fs_write(asx_file_handle file, const uint8_t *src, uint32_t len,
                               uint32_t *out_written) {
    native_file *f = fs_file_lookup(file);
    ssize_t n;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (len > ASX_FS_NATIVE_IO_MAX) len = ASX_FS_NATIVE_IO_MAX;
    do { n = write(f->fd, src, (size_t)len); } while (n < 0 && errno == EINTR);
    if (n < 0) return fs_errno_status(errno);
    *out_written = (uint32_t)n;
    return ASX_OK;
}

asx_status asx_native_fs_read_at(asx_file_handle file, uint64_t offset, uint8_t *dst, uint32_t cap,
                                 uint32_t *out_read) {
    native_file *f = fs_file_lookup(file);
    ssize_t n;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (!fs_offset_fits(offset)) return ASX_OK; /* beyond any possible end-of-file */
    if (cap > ASX_FS_NATIVE_IO_MAX) cap = ASX_FS_NATIVE_IO_MAX;
    do { n = pread(f->fd, dst, (size_t)cap, (off_t)offset); } while (n < 0 && errno == EINTR);
    if (n < 0) return fs_errno_status(errno);
    *out_read = (uint32_t)n;
    return ASX_OK;
}

asx_status asx_native_fs_write_at(asx_file_handle file, uint64_t offset, const uint8_t *src,
                                  uint32_t len, uint32_t *out_written) {
    native_file *f = fs_file_lookup(file);
    ssize_t n;
    if (f == NULL) return ASX_E_NOT_FOUND;
    /* Linux pwrite() appends on O_APPEND descriptors; refuse, like MEMORY. */
    if ((f->flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) return ASX_E_INVALID_STATE;
    if (!fs_offset_fits(offset)) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_FS_NATIVE_IO_MAX) len = ASX_FS_NATIVE_IO_MAX;
    do { n = pwrite(f->fd, src, (size_t)len, (off_t)offset); } while (n < 0 && errno == EINTR);
    if (n < 0) return fs_errno_status(errno);
    *out_written = (uint32_t)n;
    return ASX_OK;
}

asx_status asx_native_fs_seek(asx_file_handle file, int64_t offset, asx_fs_seek_whence whence,
                              uint64_t *out_pos) {
    native_file *f = fs_file_lookup(file);
    int how;
    off_t pos;
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
    pos = lseek(f->fd, (off_t)offset, how);
    if (pos < 0) return fs_errno_status(errno);
    if (out_pos != NULL) *out_pos = (uint64_t)pos;
    return ASX_OK;
}

asx_status asx_native_fs_sync(asx_file_handle file) {
    native_file *f = fs_file_lookup(file);
    int rv;
    if (f == NULL) return ASX_E_NOT_FOUND;
    do { rv = fsync(f->fd); } while (rv != 0 && errno == EINTR);
    return rv == 0 ? ASX_OK : fs_errno_status(errno);
}

asx_status asx_native_fs_set_len(asx_file_handle file, uint64_t len) {
    native_file *f = fs_file_lookup(file);
    int rv;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (!fs_offset_fits(len)) return ASX_E_RESOURCE_EXHAUSTED;
    do { rv = ftruncate(f->fd, (off_t)len); } while (rv != 0 && errno == EINTR);
    return rv == 0 ? ASX_OK : fs_errno_status(errno);
}

asx_status asx_native_fs_file_metadata(asx_file_handle file, asx_fs_metadata *out) {
    native_file *f = fs_file_lookup(file);
    struct stat st;
    if (f == NULL) return ASX_E_NOT_FOUND;
    if (fstat(f->fd, &st) != 0) return fs_errno_status(errno);
    fs_fill_meta(&st, out);
    return ASX_OK;
}

asx_status asx_native_fs_close(asx_file_handle file) {
    native_file *f = fs_file_lookup(file);
    int fd;
    if (f == NULL) return ASX_E_NOT_FOUND;
    fd = f->fd;
    f->in_use = 0;
    f->fd = -1;
    return fs_close_fd(fd);
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
