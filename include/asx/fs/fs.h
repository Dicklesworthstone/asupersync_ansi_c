/*
 * asx/fs/fs.h — filesystem host surface
 *
 * Two backends behind one API:
 *   MEMORY — deterministic fixed-capacity in-memory VFS (lab runtime,
 *            replay, portable core). State is explicit and resettable;
 *            files hold at most ASX_FS_FILE_CAPACITY bytes.
 *   NATIVE — the real POSIX filesystem (POSIX builds only).
 *
 * Backend selection: asx_fs_reset() restores MEMORY; asx_runtime_init()
 * switches to NATIVE when it installs a live readiness reactor (live POSIX
 * builds), and asx_fs_set_backend() selects one explicitly. Path
 * operations use the backend selected at call time; file and directory
 * handles keep the backend they were opened with.
 *
 * Native regular-file I/O is performed directly on the calling thread:
 * regular files are always "ready" (epoll/poll cannot wait on them), so
 * reads and writes never park the task. Very large transfers block the
 * scheduler thread for their duration; chunk them across polls if that
 * matters.
 *
 * Paths are validated fail-closed on both backends: empty paths, paths of
 * ASX_FS_PATH_MAX bytes or more, and paths containing an embedded NUL are
 * rejected before any effect.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_FS_FS_H
#define ASX_FS_FS_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#ifndef ASX_FS_PATH_MAX
#define ASX_FS_PATH_MAX 192u
#endif

#ifndef ASX_FS_NAME_MAX
#define ASX_FS_NAME_MAX 256u
#endif

#ifndef ASX_MAX_FS_ENTRIES
#define ASX_MAX_FS_ENTRIES 16u
#endif

#ifndef ASX_MAX_OPEN_FILES
#define ASX_MAX_OPEN_FILES 16u
#endif

#ifndef ASX_MAX_OPEN_DIRS
#define ASX_MAX_OPEN_DIRS 8u
#endif

#ifndef ASX_FS_FILE_CAPACITY
#define ASX_FS_FILE_CAPACITY 1024u
#endif

typedef struct {
    char text[ASX_FS_PATH_MAX];
    uint32_t len;
} asx_fs_path;

typedef enum {
    ASX_FS_ENTRY_FILE = 0,
    ASX_FS_ENTRY_DIR = 1,
    ASX_FS_ENTRY_SYMLINK = 2, /* only reported by directory iteration */
    ASX_FS_ENTRY_OTHER = 3    /* fifo, socket, device (native only) */
} asx_fs_entry_kind;

typedef struct {
    asx_fs_entry_kind kind;
    uint64_t size;        /* content length in bytes (0 for non-regular files) */
    int exists;           /* 1 when the query succeeded */
    int writable;         /* any write permission bit is set */
    uint64_t modified_ns; /* NATIVE: mtime, ns since the Unix epoch;
                           * MEMORY: deterministic modification counter */
    uint32_t mode;        /* permission bits (07777) */
} asx_fs_metadata;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_file_handle;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_fs_dir_handle;

typedef struct {
    char name[ASX_FS_NAME_MAX]; /* NUL-terminated entry name (no path) */
    uint32_t name_len;
    asx_fs_entry_kind kind; /* symlinks are reported as SYMLINK */
} asx_fs_dir_entry;

typedef enum {
    ASX_FS_OPEN_READ = 1u << 0,
    ASX_FS_OPEN_WRITE = 1u << 1,
    ASX_FS_OPEN_CREATE = 1u << 2,
    ASX_FS_OPEN_TRUNC = 1u << 3,
    ASX_FS_OPEN_APPEND = 1u << 4,   /* every write goes to end-of-file; implies write */
    ASX_FS_OPEN_EXCLUSIVE = 1u << 5 /* create_new: fail if the path exists; implies create */
} asx_fs_open_flags;

typedef enum { ASX_FS_SEEK_SET = 0, ASX_FS_SEEK_CUR = 1, ASX_FS_SEEK_END = 2 } asx_fs_seek_whence;

typedef enum { ASX_FS_BACKEND_MEMORY = 0, ASX_FS_BACKEND_NATIVE = 1 } asx_fs_backend;

/* -------------------------------------------------------------------
 * Backend selection
 * ------------------------------------------------------------------- */

/* Select the backend for path operations and handles opened from now on.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for unknown values, or
 * ASX_E_PERMISSION_DENIED when NATIVE is unavailable in this build. */
ASX_API ASX_MUST_USE asx_status asx_fs_set_backend(asx_fs_backend backend);

/* Report the backend used by path operations and newly opened handles. */
ASX_API asx_fs_backend asx_fs_get_backend(void);

/* -------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------- */

/* Construct an fs path from a C string.
 * Returns ASX_E_INVALID_ARGUMENT for NULL, ASX_E_BUFFER_TOO_SMALL when the
 * path does not fit in ASX_FS_PATH_MAX - 1 bytes. */
ASX_API ASX_MUST_USE asx_status asx_fs_path_from_cstr(asx_fs_path *out, const char *path);

/* Construct an fs path from `len` raw bytes (not necessarily
 * NUL-terminated). Fails closed: ASX_E_INVALID_ARGUMENT for NULL, an empty
 * path, or an embedded NUL byte; ASX_E_BUFFER_TOO_SMALL when too long. */
ASX_API ASX_MUST_USE asx_status asx_fs_path_from_bytes(asx_fs_path *out, const char *bytes,
                                                       uint32_t len);

/* Append one component: out = base + "/" + component (no separator is
 * added when base already ends in '/'). The component must be non-empty
 * and must not contain '/'. Returns ASX_E_INVALID_ARGUMENT or
 * ASX_E_BUFFER_TOO_SMALL without writing a partial result. */
ASX_API ASX_MUST_USE asx_status asx_fs_path_join(asx_fs_path *out, const asx_fs_path *base,
                                                 const char *component);

/* Compare two fs paths for equality. */
ASX_API int asx_fs_path_eq(const asx_fs_path *a, const asx_fs_path *b);

/* -------------------------------------------------------------------
 * Path operations (backend selected at call time)
 * ------------------------------------------------------------------- */

/* Create a directory. Succeeds if a directory already exists there;
 * ASX_E_ALREADY_EXISTS if a non-directory does; ASX_E_NOT_FOUND (NATIVE)
 * when the parent is missing. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_create(const asx_fs_path *path);

/* Create a directory and every missing parent (mkdir -p). Succeeds when
 * the directory already exists; ASX_E_ALREADY_EXISTS if any component is
 * a non-directory. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_create_all(const asx_fs_path *path);

/* Query metadata (kind, size, mtime, mode) for a path, following symlinks.
 * Returns ASX_OK, ASX_E_NOT_FOUND, or ASX_E_INVALID_ARGUMENT. */
ASX_API ASX_MUST_USE asx_status asx_fs_metadata_query(const asx_fs_path *path,
                                                      asx_fs_metadata *out);

/* Return nonzero when something exists at the path (symlinks followed). */
ASX_API int asx_fs_exists(const asx_fs_path *path);

/* Remove a file (or symlink). ASX_E_NOT_FOUND if missing,
 * ASX_E_INVALID_STATE if the path is a directory. Open handles to the
 * file stay usable until closed (POSIX unlink semantics, both backends). */
ASX_API ASX_MUST_USE asx_status asx_fs_remove_file(const asx_fs_path *path);

/* Remove an empty directory. ASX_E_NOT_FOUND if missing,
 * ASX_E_INVALID_STATE if it is not a directory or not empty. */
ASX_API ASX_MUST_USE asx_status asx_fs_remove_dir(const asx_fs_path *path);

/* Rename (move) a file or directory, atomically replacing an existing
 * file at `to` (POSIX rename semantics). ASX_E_NOT_FOUND if `from` is
 * missing; ASX_E_INVALID_STATE for kind mismatches or a non-empty
 * destination directory. */
ASX_API ASX_MUST_USE asx_status asx_fs_rename(const asx_fs_path *from, const asx_fs_path *to);

/* -------------------------------------------------------------------
 * Directory iteration
 * ------------------------------------------------------------------- */

/* Open a directory for iteration. ASX_E_NOT_FOUND if missing,
 * ASX_E_INVALID_STATE if not a directory, ASX_E_RESOURCE_EXHAUSTED when
 * the handle table is full. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_open(asx_fs_dir_handle *out, const asx_fs_path *path);

/* Read the next entry ("." and ".." are skipped). Returns ASX_OK with
 * *out_has_entry = 1 and *out filled, or ASX_OK with *out_has_entry = 0 at
 * the end. Order: NATIVE follows the OS; MEMORY is creation order. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_next(asx_fs_dir_handle dir, asx_fs_dir_entry *out,
                                                int *out_has_entry);

/* Close a directory handle. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_close(asx_fs_dir_handle dir);

/* -------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------- */

/* Open a file with ASX_FS_OPEN_* flags, returning a handle with a cursor
 * at offset 0. Validation (both backends): at least one of
 * READ/WRITE/APPEND; TRUNC needs write access (ASX_E_PERMISSION_DENIED);
 * TRUNC|APPEND and unknown bits are ASX_E_INVALID_ARGUMENT. Opening a
 * directory is ASX_E_INVALID_STATE; a missing path without CREATE is
 * ASX_E_NOT_FOUND; EXCLUSIVE on an existing path is ASX_E_ALREADY_EXISTS. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_open(asx_file_handle *out, const asx_fs_path *path,
                                                 uint32_t flags);

/* Poll-read from the cursor into a destination buffer (advances the
 * cursor). Returns ASX_OK with bytes read, ASX_E_PENDING when the cursor is
 * at end-of-file (no data yet; nothing is parked), ASX_E_BUFFER_TOO_SMALL
 * when dst is full, or ASX_E_PERMISSION_DENIED without read access. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_poll_read(asx_file_handle file, asx_buf_mut *dst,
                                                      uint32_t *bytes_read);

/* Poll-write a source buffer at the cursor (or at end-of-file in APPEND
 * mode). Writes everything or reports why not: ASX_E_RESOURCE_EXHAUSTED
 * with a partial count when space runs out. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_poll_write(asx_file_handle file, const asx_buf *src,
                                                       uint32_t *bytes_written);

/* Read up to `cap` bytes at the cursor. Returns ASX_OK with *out_read > 0,
 * or ASX_OK with *out_read == 0 at end-of-file. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_read(asx_file_handle file, uint8_t *dst, uint32_t cap,
                                                 uint32_t *out_read);

/* Write up to `len` bytes at the cursor (end-of-file in APPEND mode).
 * Returns ASX_OK with the count written (may be partial), or
 * ASX_E_RESOURCE_EXHAUSTED when no space is left. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_write(asx_file_handle file, const uint8_t *src,
                                                  uint32_t len, uint32_t *out_written);

/* Positional read at `offset` without moving the cursor (pread).
 * ASX_OK with 0 bytes at or past end-of-file. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_read_at(asx_file_handle file, uint64_t offset,
                                                    uint8_t *dst, uint32_t cap, uint32_t *out_read);

/* Positional write at `offset` without moving the cursor (pwrite).
 * Writing past end-of-file zero-fills the gap. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_write_at(asx_file_handle file, uint64_t offset,
                                                     const uint8_t *src, uint32_t len,
                                                     uint32_t *out_written);

/* Move the cursor (SET/CUR/END + offset) and report the new position.
 * A negative resulting position is ASX_E_INVALID_ARGUMENT. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_seek(asx_file_handle file, int64_t offset,
                                                 asx_fs_seek_whence whence, uint64_t *out_pos);

/* Rewind the file cursor to the beginning. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_rewind(asx_file_handle file);

/* Flush file data and metadata to stable storage (fsync; MEMORY: no-op). */
ASX_API ASX_MUST_USE asx_status asx_fs_file_sync(asx_file_handle file);

/* Truncate or extend (zero-filled) the file to `len` bytes. Requires
 * write access. MEMORY: ASX_E_RESOURCE_EXHAUSTED beyond capacity. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_set_len(asx_file_handle file, uint64_t len);

/* Query metadata of an open file (fstat). */
ASX_API ASX_MUST_USE asx_status asx_fs_file_metadata(asx_file_handle file, asx_fs_metadata *out);

/* Close an open file handle. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_close(asx_file_handle file);

/* Retrieve the path an open file handle was opened with. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_path(asx_file_handle file, asx_fs_path *out);

/* Check if a file handle is still alive. */
ASX_API int asx_fs_file_is_alive(asx_file_handle file);

/* Reset all filesystem state (test support): closes every handle of both
 * backends (the on-disk filesystem is untouched), clears the MEMORY VFS and
 * restores the MEMORY backend. */
ASX_API void asx_fs_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_FS_FS_H */
