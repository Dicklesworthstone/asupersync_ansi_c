/*
 * asx/fs/fs.h — deterministic filesystem host surface
 *
 * Provides a runtime-friendly, fixed-capacity filesystem surface for
 * native integration and deterministic tests. This is a ghost host
 * implementation: state is kept in memory and all effects are explicit.
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

#ifndef ASX_MAX_FS_ENTRIES
#define ASX_MAX_FS_ENTRIES 16u
#endif

#ifndef ASX_MAX_OPEN_FILES
#define ASX_MAX_OPEN_FILES 16u
#endif

#ifndef ASX_FS_FILE_CAPACITY
#define ASX_FS_FILE_CAPACITY 1024u
#endif

typedef struct {
    char text[ASX_FS_PATH_MAX];
    uint32_t len;
} asx_fs_path;

typedef enum { ASX_FS_ENTRY_FILE = 0, ASX_FS_ENTRY_DIR = 1 } asx_fs_entry_kind;

typedef struct {
    asx_fs_entry_kind kind;
    uint32_t size;
    int exists;
    int writable;
} asx_fs_metadata;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_file_handle;

typedef enum {
    ASX_FS_OPEN_READ = 1u << 0,
    ASX_FS_OPEN_WRITE = 1u << 1,
    ASX_FS_OPEN_CREATE = 1u << 2,
    ASX_FS_OPEN_TRUNC = 1u << 3
} asx_fs_open_flags;

/* Construct an fs path from a C string. */
ASX_API ASX_MUST_USE asx_status asx_fs_path_from_cstr(asx_fs_path *out, const char *path);
/* Compare two fs paths for equality. */
ASX_API int asx_fs_path_eq(const asx_fs_path *a, const asx_fs_path *b);

/* Create a directory at the given path. */
ASX_API ASX_MUST_USE asx_status asx_fs_dir_create(const asx_fs_path *path);
/* Query metadata (kind, size, existence) for a path. */
ASX_API ASX_MUST_USE asx_status asx_fs_metadata_query(const asx_fs_path *path,
                                                      asx_fs_metadata *out);

/* Open a file with the given flags, returning a file handle. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_open(asx_file_handle *out, const asx_fs_path *path,
                                                 uint32_t flags);
/* Poll-read from an open file into a destination buffer. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_poll_read(asx_file_handle file, asx_buf_mut *dst,
                                                      uint32_t *bytes_read);
/* Poll-write to an open file from a source buffer. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_poll_write(asx_file_handle file, const asx_buf *src,
                                                       uint32_t *bytes_written);
/* Rewind the file cursor to the beginning. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_rewind(asx_file_handle file);
/* Close an open file handle. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_close(asx_file_handle file);
/* Retrieve the path associated with an open file handle. */
ASX_API ASX_MUST_USE asx_status asx_fs_file_path(asx_file_handle file, asx_fs_path *out);
/* Check if a file handle is still alive. */
ASX_API int asx_fs_file_is_alive(asx_file_handle file);

/* Reset all filesystem state (test support). */
ASX_API void asx_fs_reset(void);

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */

#ifdef __cplusplus
}
#endif

#endif /* ASX_FS_FS_H */
