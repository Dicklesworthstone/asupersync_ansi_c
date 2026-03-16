/*
 * test_fs.c — unit tests for deterministic filesystem host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/fs/fs.h>
#include <string.h>

TEST(path_from_cstr_roundtrip) {
    asx_fs_path path;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/tmp/config.toml"), ASX_OK);
    ASSERT_EQ(path.len, 16u);
    ASSERT_TRUE(strcmp(path.text, "/tmp/config.toml") == 0);
}

TEST(path_eq_rejects_mismatch) {
    asx_fs_path a, b;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&a, "/a"), ASX_OK);
    ASSERT_EQ(asx_fs_path_from_cstr(&b, "/b"), ASX_OK);
    ASSERT_FALSE(asx_fs_path_eq(&a, &b));
}

TEST(dir_create_and_metadata_query) {
    asx_fs_path path;
    asx_fs_metadata meta;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/etc/asx"), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&path), ASX_OK);
    ASSERT_EQ(asx_fs_metadata_query(&path, &meta), ASX_OK);
    ASSERT_EQ(meta.kind, ASX_FS_ENTRY_DIR);
    ASSERT_EQ(meta.exists, 1);
    ASSERT_EQ(meta.size, 0u);
}

TEST(file_open_write_read_rewind) {
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    asx_buf_mut dst;
    uint32_t n;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/etc/asx/config.json"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path,
                               ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE),
              ASX_OK);
    payload = asx_buf_from_cstr("{\"ok\":true}");
    ASSERT_EQ(asx_fs_file_poll_write(file, &payload, &n), ASX_OK);
    ASSERT_EQ(n, payload.len);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_fs_file_rewind(file), ASX_OK);
    ASSERT_EQ(asx_fs_file_poll_read(file, &dst, &n), ASX_OK);
    ASSERT_EQ(n, payload.len);
    ASSERT_TRUE(asx_buf_eq(asx_buf_mut_freeze(&dst), payload));
    ASSERT_EQ(asx_fs_file_close(file), ASX_OK);
}

TEST(file_metadata_tracks_size) {
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    asx_fs_metadata meta;
    uint32_t n;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/data/run.log"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    payload = asx_buf_from_cstr("abc123");
    ASSERT_EQ(asx_fs_file_poll_write(file, &payload, &n), ASX_OK);
    ASSERT_EQ(asx_fs_metadata_query(&path, &meta), ASX_OK);
    ASSERT_EQ(meta.kind, ASX_FS_ENTRY_FILE);
    ASSERT_EQ(meta.size, 6u);
}

TEST(file_read_at_eof_is_pending) {
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    asx_buf_mut dst;
    uint32_t n;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/tmp/eof"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path,
                               ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE),
              ASX_OK);
    payload = asx_buf_from_cstr("x");
    ASSERT_EQ(asx_fs_file_poll_write(file, &payload, &n), ASX_OK);
    ASSERT_EQ(asx_fs_file_rewind(file), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_fs_file_poll_read(file, &dst, &n), ASX_OK);
    ASSERT_EQ(asx_fs_file_poll_read(file, &dst, &n), ASX_E_PENDING);
}

TEST(file_capacity_is_enforced) {
    asx_fs_path path;
    asx_file_handle file;
    uint8_t bytes[ASX_FS_FILE_CAPACITY + 8u];
    asx_buf payload;
    uint32_t n;
    asx_fs_reset();
    memset(bytes, 0xAA, sizeof(bytes));
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/tmp/big"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    payload = asx_buf_from(bytes, (uint32_t)sizeof(bytes));
    ASSERT_EQ(asx_fs_file_poll_write(file, &payload, &n), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(n, ASX_FS_FILE_CAPACITY);
}

TEST(fs_reset_invalidates_handles) {
    asx_fs_path path;
    asx_file_handle file;
    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&path, "/tmp/reset"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&file, &path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ), ASX_OK);
    ASSERT_TRUE(asx_fs_file_is_alive(file));
    asx_fs_reset();
    ASSERT_FALSE(asx_fs_file_is_alive(file));
}

TEST(fs_reset_reuse_bumps_generation) {
    asx_fs_path first_path, second_path;
    asx_file_handle stale_file, fresh_file;
    asx_buf payload;
    uint32_t written;

    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&first_path, "/tmp/first"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&stale_file, &first_path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ),
              ASX_OK);

    asx_fs_reset();

    ASSERT_EQ(asx_fs_path_from_cstr(&second_path, "/tmp/second"), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&fresh_file, &second_path,
                               ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE),
              ASX_OK);
    ASSERT_EQ(stale_file.slot, fresh_file.slot);
    ASSERT_NE(stale_file.generation, fresh_file.generation);
    ASSERT_FALSE(asx_fs_file_is_alive(stale_file));

    payload = asx_buf_from_cstr("ok");
    ASSERT_EQ(asx_fs_file_poll_write(stale_file, &payload, &written), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_file_close(fresh_file), ASX_OK);
}

int main(void) {
    RUN_TEST(path_from_cstr_roundtrip);
    RUN_TEST(path_eq_rejects_mismatch);
    RUN_TEST(dir_create_and_metadata_query);
    RUN_TEST(file_open_write_read_rewind);
    RUN_TEST(file_metadata_tracks_size);
    RUN_TEST(file_read_at_eof_is_pending);
    RUN_TEST(file_capacity_is_enforced);
    RUN_TEST(fs_reset_invalidates_handles);
    RUN_TEST(fs_reset_reuse_bumps_generation);
    TEST_REPORT();
    return test_failures;
}
