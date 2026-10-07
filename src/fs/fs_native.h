/*
 * fs_native.h — internal bridge between the fs API and the native filesystem
 *
 * NOT part of the public API. fs.c validates arguments (paths, flags,
 * NULL pointers) and dispatches here for the NATIVE backend: path
 * operations while NATIVE is selected, and file/dir handles whose slot
 * carries ASX_FS_NATIVE_SLOT_BIT. The POSIX implementation lives in
 * src/platform/posix/fs_posix.c and is only linked in POSIX builds.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_FS_NATIVE_H
#define ASX_FS_NATIVE_H

#include <asx/asx_status.h>
#include <asx/fs/fs.h>
#include <stdint.h>

/* Handles with this bit set in `slot` refer to native files/directories. */
#define ASX_FS_NATIVE_SLOT_BIT 0x80000000u

static inline int asx_fs_slot_is_native(uint32_t slot) {
    return (slot & ASX_FS_NATIVE_SLOT_BIT) != 0u;
}

/* Reset: close every native file and directory handle. */
void asx_native_fs_reset(void);

/* Path operations (paths are pre-validated by fs.c). */
asx_status asx_native_fs_dir_create(const asx_fs_path *path);
asx_status asx_native_fs_dir_create_all(const asx_fs_path *path);
asx_status asx_native_fs_metadata(const asx_fs_path *path, asx_fs_metadata *out);
asx_status asx_native_fs_remove_file(const asx_fs_path *path);
asx_status asx_native_fs_remove_dir(const asx_fs_path *path);
asx_status asx_native_fs_rename(const asx_fs_path *from, const asx_fs_path *to);

/* Directory iteration. */
asx_status asx_native_fs_dir_open(const asx_fs_path *path, asx_fs_dir_handle *out);
asx_status asx_native_fs_dir_next(asx_fs_dir_handle dir, asx_fs_dir_entry *out, int *out_has_entry);
asx_status asx_native_fs_dir_close(asx_fs_dir_handle dir);

/* Files (flags are pre-validated by fs.c). */
asx_status asx_native_fs_open(const asx_fs_path *path, uint32_t flags, asx_file_handle *out);
asx_status asx_native_fs_read(asx_file_handle file, uint8_t *dst, uint32_t cap, uint32_t *out_read);
asx_status asx_native_fs_write(asx_file_handle file, const uint8_t *src, uint32_t len,
                               uint32_t *out_written);
asx_status asx_native_fs_read_at(asx_file_handle file, uint64_t offset, uint8_t *dst, uint32_t cap,
                                 uint32_t *out_read);
asx_status asx_native_fs_write_at(asx_file_handle file, uint64_t offset, const uint8_t *src,
                                  uint32_t len, uint32_t *out_written);
asx_status asx_native_fs_seek(asx_file_handle file, int64_t offset, asx_fs_seek_whence whence,
                              uint64_t *out_pos);
asx_status asx_native_fs_sync(asx_file_handle file);
asx_status asx_native_fs_set_len(asx_file_handle file, uint64_t len);
asx_status asx_native_fs_file_metadata(asx_file_handle file, asx_fs_metadata *out);
asx_status asx_native_fs_close(asx_file_handle file);
asx_status asx_native_fs_file_path(asx_file_handle file, asx_fs_path *out);
int asx_native_fs_is_alive(asx_file_handle file);
/* Validate a handle and its access mode: ASX_OK, ASX_E_NOT_FOUND for stale
 * handles, ASX_E_PERMISSION_DENIED when the requested access is missing. */
asx_status asx_native_fs_check(asx_file_handle file, int want_read, int want_write);

#endif /* ASX_FS_NATIVE_H */
