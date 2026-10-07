/*
 * test_fs_native.c — native filesystem backend against the real disk
 *
 * Every scenario works inside a fresh mkdtemp() directory and removes all
 * of it again through the asx fs API itself (asserting the directory is
 * really gone). Regular-file I/O is synchronous, so the suite runs in
 * every POSIX build (deterministic or live) with the NATIVE backend
 * selected explicitly; the live lane additionally checks that
 * asx_runtime_init() selects the native host backends.
 *
 * Parity scenarios run the same assertions against MEMORY and NATIVE.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/fs/fs.h>
#include <asx/net/http.h>
#include <asx/process/process.h>
#include <asx/runtime/rt.h>
#include <asx/signal/signal.h>
#include <string.h>

#if defined(ASX_PROFILE_POSIX)
#include <stdlib.h>

static asx_fs_path g_root; /* mkdtemp directory of the current scenario */

static int make_root(void) {
    char tmpl[] = "/tmp/asx-fs-native-XXXXXX";
    asx_fs_reset();
    if (asx_fs_set_backend(ASX_FS_BACKEND_NATIVE) != ASX_OK) return 0;
    if (mkdtemp(tmpl) == NULL) return 0;
    return asx_fs_path_from_cstr(&g_root, tmpl) == ASX_OK;
}

static asx_fs_path child(const asx_fs_path *base, const char *name) {
    asx_fs_path p;
    memset(&p, 0, sizeof(p));
    if (asx_fs_path_join(&p, base, name) != ASX_OK) p.len = 0u;
    return p;
}

/* Recursively delete `dir` using only the fs API. */
static asx_status remove_tree(const asx_fs_path *dir) {
    asx_fs_dir_handle d;
    asx_fs_dir_entry e;
    int has = 0;
    asx_status st = asx_fs_dir_open(&d, dir);
    if (st != ASX_OK) return st;
    for (;;) {
        asx_fs_path p;
        st = asx_fs_dir_next(d, &e, &has);
        if (st != ASX_OK || !has) break;
        p = child(dir, e.name);
        st = e.kind == ASX_FS_ENTRY_DIR ? remove_tree(&p) : asx_fs_remove_file(&p);
        if (st != ASX_OK) break;
    }
    if (asx_fs_dir_close(d) != ASX_OK && st == ASX_OK) st = ASX_E_INVALID_STATE;
    if (st != ASX_OK) return st;
    return asx_fs_remove_dir(dir);
}

static int drop_root(void) {
    asx_status st = remove_tree(&g_root);
    return st == ASX_OK && !asx_fs_exists(&g_root);
}

/* -------------------------------------------------------------------
 * Backend selection
 * ------------------------------------------------------------------- */

TEST(backend_selection_and_reset) {
    asx_fs_reset();
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
    ASSERT_EQ(asx_fs_set_backend((asx_fs_backend)7), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_NATIVE);
    asx_fs_reset();
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
}

/* -------------------------------------------------------------------
 * Files: create / write / read / seek / positional / stat / truncate
 * ------------------------------------------------------------------- */

TEST(file_create_write_read_stat_roundtrip) {
    asx_fs_path p;
    asx_file_handle f;
    asx_fs_metadata meta;
    asx_buf_mut dst;
    uint8_t buf[64];
    uint32_t n = 0;
    uint64_t pos = 0;

    ASSERT_TRUE(make_root());
    p = child(&g_root, "data.txt");
    ASSERT_TRUE(p.len > 0u);
    ASSERT_FALSE(asx_fs_exists(&p));
    ASSERT_EQ(asx_fs_file_open(&f, &p,
                               ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE |
                                   ASX_FS_OPEN_TRUNC),
              ASX_OK);
    ASSERT_TRUE(asx_fs_file_is_alive(f));
    ASSERT_EQ(asx_fs_file_write(f, (const uint8_t *)"hello world", 11u, &n), ASX_OK);
    ASSERT_EQ(n, 11u);

    /* Path metadata sees the real inode. */
    ASSERT_EQ(asx_fs_metadata_query(&p, &meta), ASX_OK);
    ASSERT_EQ(meta.kind, ASX_FS_ENTRY_FILE);
    ASSERT_EQ(meta.size, 11u);
    ASSERT_EQ(meta.exists, 1);
    ASSERT_TRUE(meta.modified_ns > 0u);
    ASSERT_TRUE((meta.mode & 0600u) == 0600u);
    ASSERT_EQ(meta.writable, 1);

    /* Streaming read from the start, EOF is OK + 0 bytes. */
    ASSERT_EQ(asx_fs_file_seek(f, 0, ASX_FS_SEEK_SET, &pos), ASX_OK);
    ASSERT_EQ(pos, 0u);
    ASSERT_EQ(asx_fs_file_read(f, buf, 5u, &n), ASX_OK);
    ASSERT_EQ(n, 5u);
    ASSERT_EQ(memcmp(buf, "hello", 5u), 0);
    ASSERT_EQ(asx_fs_file_read(f, buf, sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 6u);
    ASSERT_EQ(memcmp(buf, " world", 6u), 0);
    ASSERT_EQ(asx_fs_file_read(f, buf, sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 0u);

    /* Positional I/O leaves the cursor alone. */
    ASSERT_EQ(asx_fs_file_read_at(f, 6u, buf, sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 5u);
    ASSERT_EQ(memcmp(buf, "world", 5u), 0);
    ASSERT_EQ(asx_fs_file_write_at(f, 0u, (const uint8_t *)"HELLO", 5u, &n), ASX_OK);
    ASSERT_EQ(n, 5u);
    ASSERT_EQ(asx_fs_file_seek(f, 0, ASX_FS_SEEK_CUR, &pos), ASX_OK);
    ASSERT_EQ(pos, 11u);
    ASSERT_EQ(asx_fs_file_seek(f, -5, ASX_FS_SEEK_END, &pos), ASX_OK);
    ASSERT_EQ(pos, 6u);
    ASSERT_EQ(asx_fs_file_seek(f, -100, ASX_FS_SEEK_CUR, &pos), ASX_E_INVALID_ARGUMENT);

    /* The legacy poll API: data, then PENDING at end-of-file. */
    ASSERT_EQ(asx_fs_file_rewind(f), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_fs_file_poll_read(f, &dst, &n), ASX_OK);
    ASSERT_EQ(n, 11u);
    ASSERT_EQ(memcmp(dst.data, "HELLO world", 11u), 0);
    ASSERT_EQ(asx_fs_file_poll_read(f, &dst, &n), ASX_E_PENDING);

    /* Truncate, sync, fstat. */
    ASSERT_EQ(asx_fs_file_set_len(f, 5u), ASX_OK);
    ASSERT_EQ(asx_fs_file_sync(f), ASX_OK);
    ASSERT_EQ(asx_fs_file_metadata(f, &meta), ASX_OK);
    ASSERT_EQ(meta.size, 5u);
    ASSERT_EQ(asx_fs_file_set_len(f, 8u), ASX_OK); /* zero-filled growth */
    ASSERT_EQ(asx_fs_file_read_at(f, 0u, buf, sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 8u);
    ASSERT_EQ(memcmp(buf, "HELLO\0\0\0", 8u), 0);

    {
        asx_fs_path got;
        ASSERT_EQ(asx_fs_file_path(f, &got), ASX_OK);
        ASSERT_TRUE(asx_fs_path_eq(&got, &p));
    }
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);
    ASSERT_FALSE(asx_fs_file_is_alive(f));
    ASSERT_EQ(asx_fs_file_close(f), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_file_read(f, buf, sizeof(buf), &n), ASX_E_NOT_FOUND);
    ASSERT_TRUE(drop_root());
}

TEST(open_flags_append_exclusive_and_access) {
    asx_fs_path p;
    asx_fs_path missing;
    asx_file_handle f;
    asx_file_handle ro;
    asx_fs_metadata meta;
    uint8_t buf[16];
    uint32_t n = 0;

    ASSERT_TRUE(make_root());
    p = child(&g_root, "log");
    missing = child(&g_root, "missing");

    ASSERT_EQ(asx_fs_file_open(&f, &missing, ASX_FS_OPEN_READ), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_APPEND | ASX_FS_OPEN_CREATE), ASX_OK);
    ASSERT_EQ(asx_fs_file_write(f, (const uint8_t *)"ab", 2u, &n), ASX_OK);
    ASSERT_EQ(asx_fs_file_rewind(f), ASX_OK);
    ASSERT_EQ(asx_fs_file_write(f, (const uint8_t *)"cd", 2u, &n), ASX_OK); /* still appends */
    ASSERT_EQ(asx_fs_file_write_at(f, 0u, (const uint8_t *)"x", 1u, &n), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_fs_file_read(f, buf, sizeof(buf), &n), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);

    ASSERT_EQ(asx_fs_metadata_query(&p, &meta), ASX_OK);
    ASSERT_EQ(meta.size, 4u);
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_WRITE | ASX_FS_OPEN_EXCLUSIVE),
              ASX_E_ALREADY_EXISTS);
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_READ | ASX_FS_OPEN_TRUNC),
              ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_APPEND | ASX_FS_OPEN_TRUNC),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_fs_file_open(&f, &p, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_READ | (1u << 20)), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_fs_file_open(&f, &g_root, ASX_FS_OPEN_READ), ASX_E_INVALID_STATE);

    ASSERT_EQ(asx_fs_file_open(&ro, &p, ASX_FS_OPEN_READ), ASX_OK);
    ASSERT_EQ(asx_fs_file_read(ro, buf, sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(memcmp(buf, "abcd", 4u), 0);
    ASSERT_EQ(asx_fs_file_write(ro, (const uint8_t *)"z", 1u, &n), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_fs_file_set_len(ro, 0u), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_fs_file_close(ro), ASX_OK);

    /* Exclusive create of a new name works exactly once. */
    ASSERT_EQ(asx_fs_file_open(&f, &missing, ASX_FS_OPEN_WRITE | ASX_FS_OPEN_EXCLUSIVE), ASX_OK);
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);
    ASSERT_TRUE(asx_fs_exists(&missing));
    ASSERT_TRUE(drop_root());
}

/* -------------------------------------------------------------------
 * Directories: mkdir -p, readdir, rename, remove
 * ------------------------------------------------------------------- */

TEST(mkdir_p_readdir_rename_remove) {
    asx_fs_path a, ab, abc, x, y, z, c2, f_as_dir;
    asx_fs_dir_handle d;
    asx_fs_dir_entry e;
    asx_file_handle f;
    asx_fs_metadata meta;
    int has = 0;
    int seen_c = 0, seen_x = 0, seen_y = 0, total = 0;

    ASSERT_TRUE(make_root());
    a = child(&g_root, "a");
    ab = child(&a, "b");
    abc = child(&ab, "c");
    x = child(&ab, "x");
    y = child(&ab, "y");
    z = child(&ab, "z");
    c2 = child(&a, "c2");

    ASSERT_EQ(asx_fs_dir_create(&abc), ASX_E_NOT_FOUND); /* parent missing */
    ASSERT_EQ(asx_fs_dir_create_all(&abc), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create_all(&abc), ASX_OK); /* idempotent */
    ASSERT_EQ(asx_fs_dir_create(&ab), ASX_OK);      /* existing dir */
    ASSERT_EQ(asx_fs_metadata_query(&abc, &meta), ASX_OK);
    ASSERT_EQ(meta.kind, ASX_FS_ENTRY_DIR);
    ASSERT_EQ(meta.size, 0u);

    ASSERT_EQ(asx_fs_file_open(&f, &x, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&f, &y, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&x), ASX_E_ALREADY_EXISTS);
    f_as_dir = child(&x, "sub");
    ASSERT_EQ(asx_fs_dir_create_all(&f_as_dir), ASX_E_ALREADY_EXISTS);

    ASSERT_EQ(asx_fs_dir_open(&d, &ab), ASX_OK);
    for (;;) {
        ASSERT_EQ(asx_fs_dir_next(d, &e, &has), ASX_OK);
        if (!has) break;
        total++;
        if (strcmp(e.name, "c") == 0 && e.kind == ASX_FS_ENTRY_DIR) seen_c = 1;
        if (strcmp(e.name, "x") == 0 && e.kind == ASX_FS_ENTRY_FILE) seen_x = 1;
        if (strcmp(e.name, "y") == 0 && e.kind == ASX_FS_ENTRY_FILE) seen_y = 1;
        ASSERT_EQ(e.name_len, (uint32_t)strlen(e.name));
    }
    ASSERT_EQ(asx_fs_dir_next(d, &e, &has), ASX_OK); /* end is sticky */
    ASSERT_EQ(has, 0);
    ASSERT_EQ(asx_fs_dir_close(d), ASX_OK);
    ASSERT_EQ(asx_fs_dir_close(d), ASX_E_NOT_FOUND);
    ASSERT_EQ(total, 3);
    ASSERT_TRUE(seen_c && seen_x && seen_y);
    ASSERT_EQ(asx_fs_dir_open(&d, &x), ASX_E_INVALID_STATE);

    /* Rename a file (replacing nothing), then a directory. */
    ASSERT_EQ(asx_fs_rename(&x, &z), ASX_OK);
    ASSERT_FALSE(asx_fs_exists(&x));
    ASSERT_TRUE(asx_fs_exists(&z));
    ASSERT_EQ(asx_fs_rename(&y, &z), ASX_OK); /* atomically replaces z */
    ASSERT_FALSE(asx_fs_exists(&y));
    ASSERT_EQ(asx_fs_rename(&x, &y), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_rename(&z, &abc), ASX_E_INVALID_STATE); /* file onto dir */
    ASSERT_EQ(asx_fs_rename(&abc, &c2), ASX_OK);
    ASSERT_FALSE(asx_fs_exists(&abc));
    ASSERT_EQ(asx_fs_metadata_query(&c2, &meta), ASX_OK);
    ASSERT_EQ(meta.kind, ASX_FS_ENTRY_DIR);

    /* Removal rules. */
    ASSERT_EQ(asx_fs_remove_dir(&a), ASX_E_INVALID_STATE); /* not empty */
    ASSERT_EQ(asx_fs_remove_file(&ab), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_fs_remove_dir(&z), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_fs_remove_file(&z), ASX_OK);
    ASSERT_EQ(asx_fs_remove_file(&z), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_remove_dir(&c2), ASX_OK);
    ASSERT_TRUE(drop_root());
}

/* -------------------------------------------------------------------
 * Path validation (fail-closed on both backends)
 * ------------------------------------------------------------------- */

/* Scenario helpers run against either backend and log the first failing
 * condition (the harness ASSERTs only exist inside TEST bodies). */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "    CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void check_rejection_on_current_backend(const asx_fs_path *base, int *ok) {
    asx_fs_path evil;
    asx_fs_path probe;
    asx_file_handle f;
    asx_fs_metadata meta;
    char raw[ASX_FS_PATH_MAX + 1u];
    *ok = 0;

    /* "<base>/evil\0x": a hand-built path with an embedded NUL. */
    CHECK(asx_fs_path_join(&evil, base, "evilxx") == ASX_OK);
    evil.text[evil.len - 2u] = '\0';
    CHECK(asx_fs_file_open(&f, &evil, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE) ==
          ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_dir_create(&evil) == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_dir_create_all(&evil) == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_metadata_query(&evil, &meta) == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_remove_file(&evil) == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_rename(&evil, base) == ASX_E_INVALID_ARGUMENT);
    /* Nothing was created under the truncated name. */
    CHECK(asx_fs_path_join(&probe, base, "evil") == ASX_OK);
    CHECK(!asx_fs_exists(&probe));

    /* Length field disagreeing with the terminator, or over-long. */
    probe = *base;
    probe.text[probe.len] = 'x';
    CHECK(asx_fs_metadata_query(&probe, &meta) == ASX_E_INVALID_ARGUMENT);
    probe = *base;
    probe.len = ASX_FS_PATH_MAX;
    CHECK(asx_fs_metadata_query(&probe, &meta) == ASX_E_BUFFER_TOO_SMALL);
    probe.len = 0u;
    CHECK(asx_fs_dir_create(&probe) == ASX_E_INVALID_ARGUMENT);

    /* Constructors. */
    CHECK(asx_fs_path_from_bytes(&probe, "a\0b", 3u) == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_path_from_bytes(&probe, "", 0u) == ASX_E_INVALID_ARGUMENT);
    /* The longest valid path has ASX_FS_PATH_MAX - 1 bytes. */
    memset(raw, 'a', sizeof(raw));
    CHECK(asx_fs_path_from_bytes(&probe, raw, ASX_FS_PATH_MAX - 1u) == ASX_OK);
    CHECK(asx_fs_path_from_bytes(&probe, raw, ASX_FS_PATH_MAX) == ASX_E_BUFFER_TOO_SMALL);
    raw[ASX_FS_PATH_MAX] = '\0';
    CHECK(asx_fs_path_from_cstr(&probe, raw) == ASX_E_BUFFER_TOO_SMALL);
    raw[ASX_FS_PATH_MAX - 1u] = '\0';
    CHECK(asx_fs_path_from_cstr(&probe, raw) == ASX_OK);
    CHECK(asx_fs_path_from_bytes(&probe, "/ok", 3u) == ASX_OK);
    CHECK(probe.len == 3u);
    CHECK(asx_fs_path_join(&probe, base, "a/b") == ASX_E_INVALID_ARGUMENT);
    CHECK(asx_fs_path_join(&probe, base, "") == ASX_E_INVALID_ARGUMENT);
    raw[ASX_FS_PATH_MAX - 1u - base->len] = '\0'; /* base + "/" + raw: PATH_MAX bytes */
    CHECK(asx_fs_path_join(&probe, base, raw) == ASX_E_BUFFER_TOO_SMALL);
    raw[ASX_FS_PATH_MAX - 2u - base->len] = '\0'; /* one byte shorter: fits exactly */
    CHECK(asx_fs_path_join(&probe, base, raw) == ASX_OK);
    CHECK(probe.len == ASX_FS_PATH_MAX - 1u);
    *ok = 1;
}

TEST(path_rejection_is_fail_closed_on_both_backends) {
    asx_fs_path mem_base;
    int ok = 0;

    ASSERT_TRUE(make_root());
    check_rejection_on_current_backend(&g_root, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(drop_root());

    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&mem_base, "/mem"), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&mem_base), ASX_OK);
    check_rejection_on_current_backend(&mem_base, &ok);
    ASSERT_TRUE(ok);
    asx_fs_reset();
}

/* -------------------------------------------------------------------
 * Backend parity: unlink while open, rename replacing an open file
 * ------------------------------------------------------------------- */

static void unlink_while_open_scenario(const asx_fs_path *base, int *ok) {
    asx_fs_path p = child(base, "victim");
    asx_fs_path q = child(base, "other");
    asx_file_handle f;
    asx_file_handle g;
    uint8_t buf[8];
    uint32_t n = 0;
    *ok = 0;

    CHECK(asx_fs_file_open(&f, &p, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE) ==
          ASX_OK);
    CHECK(asx_fs_file_write(f, (const uint8_t *)"keep", 4u, &n) == ASX_OK);
    CHECK(n == 4u);
    CHECK(asx_fs_remove_file(&p) == ASX_OK);
    CHECK(!asx_fs_exists(&p));
    /* The open handle still reaches the unlinked data. */
    CHECK(asx_fs_file_read_at(f, 0u, buf, sizeof(buf), &n) == ASX_OK);
    CHECK(n == 4u);
    CHECK(memcmp(buf, "keep", 4u) == 0);
    CHECK(asx_fs_file_close(f) == ASX_OK);

    /* Rename over a file that is still open elsewhere. */
    CHECK(asx_fs_file_open(&f, &p, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE) == ASX_OK);
    CHECK(asx_fs_file_write(f, (const uint8_t *)"new", 3u, &n) == ASX_OK);
    CHECK(asx_fs_file_open(&g, &q, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE) ==
          ASX_OK);
    CHECK(asx_fs_file_write(g, (const uint8_t *)"old", 3u, &n) == ASX_OK);
    CHECK(asx_fs_rename(&p, &q) == ASX_OK);
    CHECK(asx_fs_file_read_at(g, 0u, buf, sizeof(buf), &n) == ASX_OK);
    CHECK(n == 3u);
    CHECK(memcmp(buf, "old", 3u) == 0); /* g still sees the replaced inode */
    CHECK(asx_fs_file_close(f) == ASX_OK);
    CHECK(asx_fs_file_close(g) == ASX_OK);
    CHECK(asx_fs_file_open(&g, &q, ASX_FS_OPEN_READ) == ASX_OK);
    CHECK(asx_fs_file_read(g, buf, sizeof(buf), &n) == ASX_OK);
    CHECK(n == 3u);
    CHECK(memcmp(buf, "new", 3u) == 0);
    CHECK(asx_fs_file_close(g) == ASX_OK);
    CHECK(asx_fs_remove_file(&q) == ASX_OK);
    *ok = 1;
}

TEST(unlink_and_replace_while_open_match_across_backends) {
    asx_fs_path mem_base;
    int ok = 0;

    ASSERT_TRUE(make_root());
    unlink_while_open_scenario(&g_root, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(drop_root());

    asx_fs_reset();
    ASSERT_EQ(asx_fs_path_from_cstr(&mem_base, "/mem"), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&mem_base), ASX_OK);
    unlink_while_open_scenario(&mem_base, &ok);
    ASSERT_TRUE(ok);
    asx_fs_reset();
}

/* -------------------------------------------------------------------
 * Handles: reset closes native handles; table exhaustion is atomic
 * ------------------------------------------------------------------- */

TEST(reset_closes_native_handles) {
    asx_fs_path p;
    asx_file_handle f;
    asx_fs_dir_handle d;
    asx_fs_dir_entry e;
    int has = 0;

    ASSERT_TRUE(make_root());
    p = child(&g_root, "h");
    ASSERT_EQ(asx_fs_file_open(&f, &p, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    ASSERT_EQ(asx_fs_dir_open(&d, &g_root), ASX_OK);
    asx_fs_reset();
    ASSERT_FALSE(asx_fs_file_is_alive(f));
    ASSERT_EQ(asx_fs_dir_next(d, &e, &has), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
    /* The disk is untouched by reset. */
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_OK);
    ASSERT_TRUE(asx_fs_exists(&p));
    ASSERT_TRUE(drop_root());
}

/* -------------------------------------------------------------------
 * Static file serving against real files
 * ------------------------------------------------------------------- */

TEST(http_serve_static_reads_real_files) {
    asx_fs_path index;
    asx_file_handle f;
    asx_http_response resp;
    uint32_t n = 0;
    const char *html = "<h1>native</h1>";

    ASSERT_TRUE(make_root());
    index = child(&g_root, "index.html");
    ASSERT_EQ(asx_fs_file_open(&f, &index, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE), ASX_OK);
    ASSERT_EQ(asx_fs_file_write(f, (const uint8_t *)html, (uint32_t)strlen(html), &n), ASX_OK);
    ASSERT_EQ(asx_fs_file_close(f), ASX_OK);

    ASSERT_EQ(asx_http_serve_static(&resp, g_root.text, "/"), ASX_OK);
    ASSERT_EQ(resp.status, ASX_HTTP_200_OK);
    ASSERT_STR_EQ(asx_http_headers_get(&resp.headers, "Content-Type"), "text/html");
    ASSERT_EQ(memcmp(resp.body.data, html, strlen(html)), 0);
    ASSERT_EQ(asx_http_serve_static(&resp, g_root.text, "/missing.html"), ASX_E_NOT_FOUND);
    ASSERT_EQ(resp.status, ASX_HTTP_404_NOT_FOUND);
    ASSERT_EQ(asx_http_serve_static(&resp, g_root.text, "/../etc/passwd"), ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(drop_root());
}

#if !ASX_DETERMINISTIC
/* Live POSIX runtimes select every native host backend at init. */
TEST(runtime_init_selects_native_host_backends) {
    asx_runtime rt;
    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_NATIVE);
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_NATIVE);
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_NATIVE);
    asx_runtime_shutdown(&rt);
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
    ASSERT_EQ(asx_process_get_backend(), ASX_PROCESS_BACKEND_MEMORY);
    ASSERT_EQ(asx_signal_get_backend(), ASX_SIGNAL_BACKEND_MEMORY);
}
#endif

static int run_native(void) {
    RUN_TEST(backend_selection_and_reset);
    RUN_TEST(file_create_write_read_stat_roundtrip);
    RUN_TEST(open_flags_append_exclusive_and_access);
    RUN_TEST(mkdir_p_readdir_rename_remove);
    RUN_TEST(path_rejection_is_fail_closed_on_both_backends);
    RUN_TEST(unlink_and_replace_while_open_match_across_backends);
    RUN_TEST(reset_closes_native_handles);
    RUN_TEST(http_serve_static_reads_real_files);
#if !ASX_DETERMINISTIC
    RUN_TEST(runtime_init_selects_native_host_backends);
#endif
    asx_fs_reset();
    return 0;
}

#elif ASX_HAS_NATIVE_RUNTIME_SURFACES

/* Non-POSIX builds keep the deterministic VFS; NATIVE is refused. */
TEST(native_backend_unavailable_without_posix) {
    asx_fs_reset();
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_fs_get_backend(), ASX_FS_BACKEND_MEMORY);
}

static int run_native(void) {
    fprintf(stderr, "  SKIP: native filesystem scenarios need a POSIX build\n");
    RUN_TEST(native_backend_unavailable_without_posix);
    return 0;
}

#else

TEST(fs_surface_hidden_in_browser) { ASSERT_EQ(ASX_HAS_NATIVE_RUNTIME_SURFACES, 0); }

static int run_native(void) {
    RUN_TEST(fs_surface_hidden_in_browser);
    return 0;
}

#endif

int main(void) {
    fprintf(stderr, "=== test_fs_native ===\n");
    (void)run_native();
    TEST_REPORT();
    return test_failures;
}
