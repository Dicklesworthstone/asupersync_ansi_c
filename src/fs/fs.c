/*
 * fs.c — filesystem host surface
 *
 * Two backends behind one API:
 *   MEMORY — deterministic fixed-capacity VFS: a flat table of path-keyed
 *            entries (files hold up to ASX_FS_FILE_CAPACITY bytes). Entries
 *            are inodes: unlinking or replacing a path keeps the data alive
 *            for handles that still have it open (POSIX semantics).
 *   NATIVE — the real filesystem (src/platform/posix/fs_posix.c). Path
 *            operations dispatch on the backend selected at call time;
 *            native handles carry ASX_FS_NATIVE_SLOT_BIT in their slot.
 *
 * Argument validation (paths, flags, pointers) happens here, before any
 * backend effect, so both backends fail closed identically.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/fs/fs.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

#if defined(ASX_PROFILE_POSIX)
#include "fs_native.h"
#define ASX_FS_HAS_NATIVE 1
#else
#define ASX_FS_HAS_NATIVE 0
#endif

/* asx_fs_reset() always restores the deterministic VFS; asx_runtime_init()
 * switches to NATIVE when it installs a live reactor (see rt.c). */
#define ASX_FS_DEFAULT_BACKEND ASX_FS_BACKEND_MEMORY

static asx_fs_backend g_fs_backend = ASX_FS_DEFAULT_BACKEND;

#if ASX_FS_HAS_NATIVE
#define FS_NATIVE(h) asx_fs_slot_is_native((h).slot)
#define FS_USE_NATIVE() (g_fs_backend == ASX_FS_BACKEND_NATIVE)
#endif

#define ASX_FS_ALL_OPEN_FLAGS                                                                      \
    ((uint32_t)ASX_FS_OPEN_READ | (uint32_t)ASX_FS_OPEN_WRITE | (uint32_t)ASX_FS_OPEN_CREATE |     \
     (uint32_t)ASX_FS_OPEN_TRUNC | (uint32_t)ASX_FS_OPEN_APPEND | (uint32_t)ASX_FS_OPEN_EXCLUSIVE)
#define ASX_FS_WRITE_ACCESS ((uint32_t)ASX_FS_OPEN_WRITE | (uint32_t)ASX_FS_OPEN_APPEND)

/* ------------------------------------------------------------------ */
/* Memory backend state                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_fs_path path;
    asx_fs_entry_kind kind;
    uint32_t size;
    uint64_t modified;   /* deterministic modification counter */
    uint32_t open_count; /* open file handles referencing this inode */
    uint8_t data[ASX_FS_FILE_CAPACITY];
    int alive;  /* slot holds an inode */
    int linked; /* reachable through its path */
} asx_fs_entry_slot;

typedef struct {
    uint32_t entry_slot;
    uint32_t generation;
    uint32_t offset;
    uint32_t flags;
    asx_fs_path path; /* path as opened */
    int alive;
} asx_open_file_slot;

typedef struct {
    asx_fs_path path;
    uint32_t generation;
    uint32_t cursor; /* next entry index to examine */
    int alive;
} asx_open_dir_slot;

static asx_fs_entry_slot g_entries[ASX_MAX_FS_ENTRIES];
static asx_open_file_slot g_open_files[ASX_MAX_OPEN_FILES];
static asx_open_dir_slot g_open_dirs[ASX_MAX_OPEN_DIRS];
static uint64_t g_fs_mod_counter;

static uint32_t asx_fs_next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

/* ------------------------------------------------------------------ */
/* Path validation and helpers                                         */
/* ------------------------------------------------------------------ */

/* Fail-closed path check shared by both backends: non-empty, shorter than
 * ASX_FS_PATH_MAX, NUL-terminated at len, no embedded NUL. */
static asx_status fs_path_check(const asx_fs_path *p) {
    if (p == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->len == 0u) return ASX_E_INVALID_ARGUMENT;
    if (p->len >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
    if (p->text[p->len] != '\0') return ASX_E_INVALID_ARGUMENT;
    if (memchr(p->text, '\0', p->len) != NULL) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

/* Length ignoring trailing separators ("/a/b/" == "/a/b"; "/" stays). */
static uint32_t fs_norm_len(const char *text, uint32_t len) {
    while (len > 1u && text[len - 1u] == '/') len--;
    return len;
}

static int fs_text_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    alen = fs_norm_len(a, alen);
    blen = fs_norm_len(b, blen);
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* Length of the parent directory of text[0..len). Returns 0 when the path
 * has no separator (no listable parent). */
static uint32_t fs_parent_len(const char *text, uint32_t len) {
    len = fs_norm_len(text, len);
    while (len > 0u && text[len - 1u] != '/') len--;
    if (len == 0u) return 0u;
    /* len now includes the separator; keep "/" for top-level entries. */
    return len == 1u ? 1u : len - 1u;
}

static int fs_is_child_of(const asx_fs_path *entry, const asx_fs_path *dir) {
    uint32_t plen = fs_parent_len(entry->text, entry->len);
    if (plen == 0u) return 0;
    if (fs_text_eq(entry->text, entry->len, dir->text, dir->len)) return 0; /* "/" itself */
    return fs_text_eq(entry->text, plen, dir->text, dir->len);
}

/* True when `inner` lies strictly below `outer` ("/a/b" below "/a"). */
static int fs_is_descendant(const char *inner, uint32_t ilen, const char *outer, uint32_t olen) {
    ilen = fs_norm_len(inner, ilen);
    olen = fs_norm_len(outer, olen);
    if (olen == 1u && outer[0] == '/') return ilen > 1u && inner[0] == '/';
    if (ilen <= olen + 1u) return 0;
    return memcmp(inner, outer, olen) == 0 && inner[olen] == '/';
}

/* ------------------------------------------------------------------ */
/* Memory backend: entries                                             */
/* ------------------------------------------------------------------ */

static int fs_mem_find(const asx_fs_path *path) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive || !g_entries[i].linked) continue;
        if (fs_text_eq(g_entries[i].path.text, g_entries[i].path.len, path->text, path->len)) {
            return (int)i;
        }
    }
    return -1;
}

static int fs_mem_free_entry_slot(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive) return (int)i;
    }
    return -1;
}

static uint32_t fs_mem_free_entry_count(void) {
    uint32_t i;
    uint32_t n = 0u;
    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive) n++;
    }
    return n;
}

static asx_status fs_mem_create_entry(const asx_fs_path *path, asx_fs_entry_kind kind,
                                      int *out_index) {
    int idx = fs_mem_free_entry_slot();
    if (idx < 0) return ASX_E_RESOURCE_EXHAUSTED;
    memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
    g_entries[idx].alive = 1;
    g_entries[idx].linked = 1;
    g_entries[idx].kind = kind;
    g_entries[idx].path = *path;
    g_entries[idx].modified = ++g_fs_mod_counter;
    *out_index = idx;
    return ASX_OK;
}

/* Remove a path from the namespace; the inode lives on while open. */
static void fs_mem_unlink(uint32_t idx) {
    g_entries[idx].linked = 0;
    if (g_entries[idx].open_count == 0u) g_entries[idx].alive = 0;
}

static int fs_mem_has_children(const asx_fs_path *dir) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive || !g_entries[i].linked) continue;
        if (fs_is_child_of(&g_entries[i].path, dir)) return 1;
    }
    return 0;
}

static void fs_mem_fill_metadata(const asx_fs_entry_slot *e, asx_fs_metadata *out) {
    memset(out, 0, sizeof(*out));
    out->kind = e->kind;
    out->size = e->kind == ASX_FS_ENTRY_FILE ? (uint64_t)e->size : 0u;
    out->exists = 1;
    out->writable = 1;
    out->modified_ns = e->modified;
    out->mode = e->kind == ASX_FS_ENTRY_DIR ? 0755u : 0644u;
}

/* Grow a file to `end` bytes, zero-filling the gap. */
static void fs_mem_extend(asx_fs_entry_slot *e, uint32_t end) {
    if (end > e->size) {
        memset(&e->data[e->size], 0, (size_t)(end - e->size));
        e->size = end;
    }
}

/* ------------------------------------------------------------------ */
/* Memory backend: handles                                             */
/* ------------------------------------------------------------------ */

static asx_open_file_slot *fs_mem_lookup_file(asx_file_handle file) {
    asx_open_file_slot *slot;
    if (file.slot >= ASX_MAX_OPEN_FILES) return NULL;
    slot = &g_open_files[file.slot];
    if (!slot->alive) return NULL;
    if (slot->generation != file.generation) return NULL;
    if (slot->entry_slot >= ASX_MAX_FS_ENTRIES || !g_entries[slot->entry_slot].alive) return NULL;
    return slot;
}

static asx_open_dir_slot *fs_mem_lookup_dir(asx_fs_dir_handle dir) {
    asx_open_dir_slot *slot;
    if (dir.slot >= ASX_MAX_OPEN_DIRS) return NULL;
    slot = &g_open_dirs[dir.slot];
    if (!slot->alive) return NULL;
    if (slot->generation != dir.generation) return NULL;
    return slot;
}

static asx_status fs_mem_check(asx_file_handle file, int want_read, int want_write,
                               asx_open_file_slot **out) {
    asx_open_file_slot *slot = fs_mem_lookup_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if (want_read && (slot->flags & (uint32_t)ASX_FS_OPEN_READ) == 0u) {
        return ASX_E_PERMISSION_DENIED;
    }
    if (want_write && (slot->flags & ASX_FS_WRITE_ACCESS) == 0u) return ASX_E_PERMISSION_DENIED;
    if (out != NULL) *out = slot;
    return ASX_OK;
}

static uint32_t fs_mem_read_at(asx_fs_entry_slot *e, uint64_t offset, uint8_t *dst, uint32_t cap) {
    uint32_t avail;
    uint32_t n;
    if (offset >= (uint64_t)e->size) return 0u;
    avail = e->size - (uint32_t)offset;
    n = avail < cap ? avail : cap;
    if (n > 0u) memcpy(dst, &e->data[offset], (size_t)n);
    return n;
}

/* Write at `offset`; partial when capacity runs out. */
static asx_status fs_mem_write_at(asx_fs_entry_slot *e, uint64_t offset, const uint8_t *src,
                                  uint32_t len, uint32_t *out_written) {
    uint32_t room;
    uint32_t n;
    *out_written = 0u;
    if (len == 0u) return ASX_OK;
    if (offset >= (uint64_t)ASX_FS_FILE_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;
    room = ASX_FS_FILE_CAPACITY - (uint32_t)offset;
    n = len < room ? len : room;
    fs_mem_extend(e, (uint32_t)offset);
    memcpy(&e->data[offset], src, (size_t)n);
    if ((uint32_t)offset + n > e->size) e->size = (uint32_t)offset + n;
    e->modified = ++g_fs_mod_counter;
    *out_written = n;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch helpers                                                    */
/* ------------------------------------------------------------------ */

static asx_status fs_check(asx_file_handle file, int want_read, int want_write) {
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_check(file, want_read, want_write);
#endif
    return fs_mem_check(file, want_read, want_write, NULL);
}

/* Raw read at the cursor; cap may be 0 (validation only). */
static asx_status fs_read_raw(asx_file_handle file, uint8_t *dst, uint32_t cap,
                              uint32_t *out_read) {
    asx_open_file_slot *slot;
    asx_status st;

    *out_read = 0u;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) {
        st = asx_native_fs_check(file, 1, 0);
        if (st != ASX_OK || cap == 0u) return st;
        return asx_native_fs_read(file, dst, cap, out_read);
    }
#endif
    st = fs_mem_check(file, 1, 0, &slot);
    if (st != ASX_OK || cap == 0u) return st;
    *out_read = fs_mem_read_at(&g_entries[slot->entry_slot], slot->offset, dst, cap);
    slot->offset += *out_read;
    return ASX_OK;
}

/* Raw write at the cursor (or end-of-file for APPEND); len may be 0. */
static asx_status fs_write_raw(asx_file_handle file, const uint8_t *src, uint32_t len,
                               uint32_t *out_written) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *e;
    asx_status st;

    *out_written = 0u;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) {
        st = asx_native_fs_check(file, 0, 1);
        if (st != ASX_OK || len == 0u) return st;
        return asx_native_fs_write(file, src, len, out_written);
    }
#endif
    st = fs_mem_check(file, 0, 1, &slot);
    if (st != ASX_OK || len == 0u) return st;
    e = &g_entries[slot->entry_slot];
    if ((slot->flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) slot->offset = e->size;
    st = fs_mem_write_at(e, slot->offset, src, len, out_written);
    slot->offset += *out_written;
    return st;
}

/* ------------------------------------------------------------------ */
/* Backend selection                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_fs_set_backend(asx_fs_backend backend) {
    if (backend != ASX_FS_BACKEND_MEMORY && backend != ASX_FS_BACKEND_NATIVE) {
        return ASX_E_INVALID_ARGUMENT;
    }
#if !ASX_FS_HAS_NATIVE
    if (backend == ASX_FS_BACKEND_NATIVE) return ASX_E_PERMISSION_DENIED;
#endif
    g_fs_backend = backend;
    return ASX_OK;
}

asx_fs_backend asx_fs_get_backend(void) { return g_fs_backend; }

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_fs_path_from_cstr(asx_fs_path *out, const char *path) {
    uint32_t len;
    if (out == NULL || path == NULL) return ASX_E_INVALID_ARGUMENT;

    len = 0u;
    while (path[len] != '\0') {
        if (len + 1u >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
        out->text[len] = path[len];
        len++;
    }
    out->text[len] = '\0';
    out->len = len;
    return ASX_OK;
}

asx_status asx_fs_path_from_bytes(asx_fs_path *out, const char *bytes, uint32_t len) {
    if (out == NULL || bytes == NULL || len == 0u) return ASX_E_INVALID_ARGUMENT;
    if (len >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
    if (memchr(bytes, '\0', (size_t)len) != NULL) return ASX_E_INVALID_ARGUMENT;
    memcpy(out->text, bytes, (size_t)len);
    out->text[len] = '\0';
    out->len = len;
    return ASX_OK;
}

asx_status asx_fs_path_join(asx_fs_path *out, const asx_fs_path *base, const char *component) {
    asx_status st;
    uint32_t clen;
    uint32_t sep;
    uint32_t total;

    if (out == NULL || component == NULL) return ASX_E_INVALID_ARGUMENT;
    st = fs_path_check(base);
    if (st != ASX_OK) return st;
    clen = 0u;
    while (component[clen] != '\0') {
        if (component[clen] == '/') return ASX_E_INVALID_ARGUMENT;
        if (clen >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
        clen++;
    }
    if (clen == 0u) return ASX_E_INVALID_ARGUMENT;
    sep = base->text[base->len - 1u] == '/' ? 0u : 1u;
    total = base->len + sep + clen;
    if (total >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
    if (out != base) memcpy(out->text, base->text, (size_t)base->len);
    if (sep != 0u) out->text[base->len] = '/';
    memcpy(&out->text[base->len + sep], component, (size_t)clen);
    out->text[total] = '\0';
    out->len = total;
    return ASX_OK;
}

int asx_fs_path_eq(const asx_fs_path *a, const asx_fs_path *b) {
    if (a == NULL || b == NULL) return 0;
    if (a->len != b->len) return 0;
    return memcmp(a->text, b->text, a->len) == 0;
}

/* ------------------------------------------------------------------ */
/* Path operations                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_fs_dir_create(const asx_fs_path *path) {
    int idx;
    asx_status st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_dir_create(path);
#endif
    idx = fs_mem_find(path);
    if (idx >= 0) {
        if (g_entries[idx].kind != ASX_FS_ENTRY_DIR) return ASX_E_ALREADY_EXISTS;
        return ASX_OK;
    }
    return fs_mem_create_entry(path, ASX_FS_ENTRY_DIR, &idx);
}

asx_status asx_fs_dir_create_all(const asx_fs_path *path) {
    asx_fs_path prefix;
    uint32_t len;
    uint32_t i;
    uint32_t missing = 0u;
    int idx;
    int pass;
    asx_status st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_dir_create_all(path);
#endif
    len = fs_norm_len(path->text, path->len);
    /* Pass 0 validates every component and counts the missing ones (so
     * exhaustion fails before any effect); pass 1 creates them. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 1u; i <= len; i++) {
            if (i < len && path->text[i] != '/') continue;
            if (i < len && path->text[i - 1u] == '/') continue; /* "//" */
            memcpy(prefix.text, path->text, (size_t)i);
            prefix.text[i] = '\0';
            prefix.len = i;
            idx = fs_mem_find(&prefix);
            if (idx >= 0) {
                if (g_entries[idx].kind != ASX_FS_ENTRY_DIR) return ASX_E_ALREADY_EXISTS;
                continue;
            }
            if (pass == 0) {
                missing++;
            } else {
                st = fs_mem_create_entry(&prefix, ASX_FS_ENTRY_DIR, &idx);
                if (st != ASX_OK) return st;
            }
        }
        if (pass == 0 && missing > fs_mem_free_entry_count()) return ASX_E_RESOURCE_EXHAUSTED;
    }
    return ASX_OK;
}

asx_status asx_fs_metadata_query(const asx_fs_path *path, asx_fs_metadata *out) {
    int idx;
    asx_status st;

    if (path == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_metadata(path, out);
#endif
    idx = fs_mem_find(path);
    if (idx < 0) return ASX_E_NOT_FOUND;
    fs_mem_fill_metadata(&g_entries[idx], out);
    return ASX_OK;
}

int asx_fs_exists(const asx_fs_path *path) {
    asx_fs_metadata meta;
    return asx_fs_metadata_query(path, &meta) == ASX_OK;
}

asx_status asx_fs_remove_file(const asx_fs_path *path) {
    int idx;
    asx_status st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_remove_file(path);
#endif
    idx = fs_mem_find(path);
    if (idx < 0) return ASX_E_NOT_FOUND;
    if (g_entries[idx].kind == ASX_FS_ENTRY_DIR) return ASX_E_INVALID_STATE;
    fs_mem_unlink((uint32_t)idx);
    return ASX_OK;
}

asx_status asx_fs_remove_dir(const asx_fs_path *path) {
    int idx;
    asx_status st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_remove_dir(path);
#endif
    idx = fs_mem_find(path);
    if (idx < 0) return ASX_E_NOT_FOUND;
    if (g_entries[idx].kind != ASX_FS_ENTRY_DIR) return ASX_E_INVALID_STATE;
    if (fs_mem_has_children(&g_entries[idx].path)) return ASX_E_INVALID_STATE;
    fs_mem_unlink((uint32_t)idx);
    return ASX_OK;
}

asx_status asx_fs_rename(const asx_fs_path *from, const asx_fs_path *to) {
    int src;
    int dst;
    uint32_t i;
    uint32_t from_len;
    uint32_t to_len;
    asx_status st;

    st = fs_path_check(from);
    if (st != ASX_OK) return st;
    st = fs_path_check(to);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_rename(from, to);
#endif
    src = fs_mem_find(from);
    if (src < 0) return ASX_E_NOT_FOUND;
    if (fs_text_eq(from->text, from->len, to->text, to->len)) return ASX_OK;
    if (fs_is_descendant(to->text, to->len, from->text, from->len)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    dst = fs_mem_find(to);
    if (dst >= 0) {
        if (g_entries[src].kind == ASX_FS_ENTRY_DIR) {
            if (g_entries[dst].kind != ASX_FS_ENTRY_DIR) return ASX_E_INVALID_STATE;
            if (fs_mem_has_children(&g_entries[dst].path)) return ASX_E_INVALID_STATE;
        } else if (g_entries[dst].kind == ASX_FS_ENTRY_DIR) {
            return ASX_E_INVALID_STATE;
        }
    }

    /* Every renamed path must fit before anything changes. */
    from_len = fs_norm_len(from->text, from->len);
    to_len = fs_norm_len(to->text, to->len);
    if (g_entries[src].kind == ASX_FS_ENTRY_DIR) {
        for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
            const asx_fs_path *p = &g_entries[i].path;
            if (!g_entries[i].alive || !g_entries[i].linked) continue;
            if (!fs_is_descendant(p->text, p->len, from->text, from->len)) continue;
            if (to_len + (p->len - from_len) >= ASX_FS_PATH_MAX) return ASX_E_BUFFER_TOO_SMALL;
        }
        for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
            asx_fs_path *p = &g_entries[i].path;
            uint32_t tail;
            if (!g_entries[i].alive || !g_entries[i].linked) continue;
            if (!fs_is_descendant(p->text, p->len, from->text, from->len)) continue;
            tail = p->len - from_len;
            memmove(&p->text[to_len], &p->text[from_len], (size_t)tail);
            memcpy(p->text, to->text, (size_t)to_len);
            p->len = to_len + tail;
            p->text[p->len] = '\0';
        }
    }
    if (dst >= 0) fs_mem_unlink((uint32_t)dst);
    memcpy(g_entries[src].path.text, to->text, (size_t)to_len);
    g_entries[src].path.text[to_len] = '\0';
    g_entries[src].path.len = to_len;
    g_entries[src].modified = ++g_fs_mod_counter;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_fs_dir_open(asx_fs_dir_handle *out, const asx_fs_path *path) {
    uint32_t i;
    int idx;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = fs_path_check(path);
    if (st != ASX_OK) return st;
#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_dir_open(path, out);
#endif
    idx = fs_mem_find(path);
    if (idx < 0) return ASX_E_NOT_FOUND;
    if (g_entries[idx].kind != ASX_FS_ENTRY_DIR) return ASX_E_INVALID_STATE;
    for (i = 0; i < ASX_MAX_OPEN_DIRS; i++) {
        asx_open_dir_slot *d = &g_open_dirs[i];
        if (d->alive) continue;
        d->alive = 1;
        d->generation = asx_fs_next_generation(d->generation);
        d->path = g_entries[idx].path;
        d->cursor = 0u;
        out->slot = i;
        out->generation = d->generation;
        return ASX_OK;
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_fs_dir_next(asx_fs_dir_handle dir, asx_fs_dir_entry *out, int *out_has_entry) {
    asx_open_dir_slot *d;

    if (out == NULL || out_has_entry == NULL) return ASX_E_INVALID_ARGUMENT;
    *out_has_entry = 0;
#if ASX_FS_HAS_NATIVE
    if (asx_fs_slot_is_native(dir.slot)) return asx_native_fs_dir_next(dir, out, out_has_entry);
#endif
    d = fs_mem_lookup_dir(dir);
    if (d == NULL) return ASX_E_NOT_FOUND;
    while (d->cursor < ASX_MAX_FS_ENTRIES) {
        const asx_fs_entry_slot *e = &g_entries[d->cursor];
        uint32_t len;
        uint32_t start;
        d->cursor++;
        if (!e->alive || !e->linked || !fs_is_child_of(&e->path, &d->path)) continue;
        len = fs_norm_len(e->path.text, e->path.len);
        start = len;
        while (start > 0u && e->path.text[start - 1u] != '/') start--;
        if (len - start >= ASX_FS_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;
        memset(out, 0, sizeof(*out));
        memcpy(out->name, &e->path.text[start], (size_t)(len - start));
        out->name_len = len - start;
        out->kind = e->kind;
        *out_has_entry = 1;
        return ASX_OK;
    }
    return ASX_OK;
}

asx_status asx_fs_dir_close(asx_fs_dir_handle dir) {
    asx_open_dir_slot *d;
#if ASX_FS_HAS_NATIVE
    if (asx_fs_slot_is_native(dir.slot)) return asx_native_fs_dir_close(dir);
#endif
    d = fs_mem_lookup_dir(dir);
    if (d == NULL) return ASX_E_NOT_FOUND;
    d->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_fs_file_open(asx_file_handle *out, const asx_fs_path *path, uint32_t flags) {
    uint32_t i;
    uint32_t free_slot = ASX_MAX_OPEN_FILES;
    int idx;
    asx_fs_entry_slot *entry;
    asx_open_file_slot *slot;
    asx_status st;

    if (out == NULL || path == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((flags & ~ASX_FS_ALL_OPEN_FLAGS) != 0u) return ASX_E_INVALID_ARGUMENT;
    if ((flags & ((uint32_t)ASX_FS_OPEN_READ | ASX_FS_WRITE_ACCESS)) == 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if ((flags & (uint32_t)ASX_FS_OPEN_TRUNC) != 0u && (flags & ASX_FS_WRITE_ACCESS) == 0u) {
        return ASX_E_PERMISSION_DENIED;
    }
    if ((flags & (uint32_t)ASX_FS_OPEN_TRUNC) != 0u &&
        (flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = fs_path_check(path);
    if (st != ASX_OK) return st;

#if ASX_FS_HAS_NATIVE
    if (FS_USE_NATIVE()) return asx_native_fs_open(path, flags, out);
#endif

    /* Claim the handle slot first so exhaustion never leaves a fresh file
     * behind (failure atomicity). */
    for (i = 0; i < ASX_MAX_OPEN_FILES; i++) {
        if (!g_open_files[i].alive) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == ASX_MAX_OPEN_FILES) return ASX_E_RESOURCE_EXHAUSTED;

    idx = fs_mem_find(path);
    if (idx >= 0 && (flags & (uint32_t)ASX_FS_OPEN_EXCLUSIVE) != 0u) {
        return ASX_E_ALREADY_EXISTS;
    }
    if (idx >= 0 && g_entries[idx].kind != ASX_FS_ENTRY_FILE) return ASX_E_INVALID_STATE;
    if (idx < 0) {
        if ((flags & ((uint32_t)ASX_FS_OPEN_CREATE | (uint32_t)ASX_FS_OPEN_EXCLUSIVE)) == 0u) {
            return ASX_E_NOT_FOUND;
        }
        st = fs_mem_create_entry(path, ASX_FS_ENTRY_FILE, &idx);
        if (st != ASX_OK) return st;
    }

    entry = &g_entries[idx];
    if ((flags & (uint32_t)ASX_FS_OPEN_TRUNC) != 0u && entry->size != 0u) {
        entry->size = 0u;
        entry->modified = ++g_fs_mod_counter;
    }
    entry->open_count++;

    slot = &g_open_files[free_slot];
    slot->alive = 1;
    slot->generation = asx_fs_next_generation(slot->generation);
    slot->entry_slot = (uint32_t)idx;
    slot->offset = 0u;
    slot->flags = flags;
    slot->path = *path;
    out->slot = free_slot;
    out->generation = slot->generation;
    return ASX_OK;
}

asx_status asx_fs_file_poll_read(asx_file_handle file, asx_buf_mut *dst, uint32_t *bytes_read) {
    uint32_t cap;
    uint32_t n = 0u;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    *bytes_read = 0u;
    st = fs_check(file, 1, 0);
    if (st != ASX_OK) return st;

    cap = asx_buf_mut_writable(dst);
    if (cap == 0u) {
        /* A full buffer is only an error when data is actually available. */
        uint64_t pos = 0u;
        uint8_t probe;
        st = asx_fs_file_seek(file, 0, ASX_FS_SEEK_CUR, &pos);
        if (st != ASX_OK) return st;
        st = asx_fs_file_read_at(file, pos, &probe, 1u, &n);
        if (st != ASX_OK) return st;
        return n == 0u ? ASX_E_PENDING : ASX_E_BUFFER_TOO_SMALL;
    }

    st = fs_read_raw(file, &dst->data[dst->wr_pos], cap, &n);
    if (st != ASX_OK) return st;
    if (n == 0u) return ASX_E_PENDING; /* at end-of-file: no data yet */
    dst->wr_pos += n;
    *bytes_read = n;
    return ASX_OK;
}

asx_status asx_fs_file_poll_write(asx_file_handle file, const asx_buf *src,
                                  uint32_t *bytes_written) {
    uint32_t total = 0u;
    uint32_t n;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    *bytes_written = 0u;
    st = fs_check(file, 0, 1);
    if (st != ASX_OK) return st;
    if (src->len == 0u) return ASX_OK;
    if (src->ptr == NULL) return ASX_E_INVALID_ARGUMENT;

    while (total < src->len) {
        st = fs_write_raw(file, src->ptr + total, src->len - total, &n);
        total += n;
        if (st != ASX_OK) break;
        if (n == 0u) {
            st = ASX_E_RESOURCE_EXHAUSTED;
            break;
        }
    }
    *bytes_written = total;
    return st;
}

asx_status asx_fs_file_read(asx_file_handle file, uint8_t *dst, uint32_t cap, uint32_t *out_read) {
    asx_status st;
    if (out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0u;
    st = fs_check(file, 1, 0);
    if (st != ASX_OK) return st;
    if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
    return fs_read_raw(file, dst, cap, out_read);
}

asx_status asx_fs_file_write(asx_file_handle file, const uint8_t *src, uint32_t len,
                             uint32_t *out_written) {
    asx_status st;
    if (out_written == NULL || (src == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_written = 0u;
    st = fs_write_raw(file, src, len, out_written);
    if (st == ASX_OK && len > 0u && *out_written == 0u) return ASX_E_RESOURCE_EXHAUSTED;
    return st;
}

asx_status asx_fs_file_read_at(asx_file_handle file, uint64_t offset, uint8_t *dst, uint32_t cap,
                               uint32_t *out_read) {
    asx_open_file_slot *slot;
    asx_status st;

    if (out_read == NULL || (dst == NULL && cap > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_read = 0u;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) {
        st = asx_native_fs_check(file, 1, 0);
        if (st != ASX_OK) return st;
        if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
        return asx_native_fs_read_at(file, offset, dst, cap, out_read);
    }
#endif
    st = fs_mem_check(file, 1, 0, &slot);
    if (st != ASX_OK) return st;
    if (cap == 0u) return ASX_E_BUFFER_TOO_SMALL;
    *out_read = fs_mem_read_at(&g_entries[slot->entry_slot], offset, dst, cap);
    return ASX_OK;
}

asx_status asx_fs_file_write_at(asx_file_handle file, uint64_t offset, const uint8_t *src,
                                uint32_t len, uint32_t *out_written) {
    asx_open_file_slot *slot;
    asx_status st;

    if (out_written == NULL || (src == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;
    *out_written = 0u;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) {
        st = asx_native_fs_check(file, 0, 1);
        if (st != ASX_OK || len == 0u) return st;
        return asx_native_fs_write_at(file, offset, src, len, out_written);
    }
#endif
    st = fs_mem_check(file, 0, 1, &slot);
    if (st != ASX_OK) return st;
    /* Positional writes ignore O_APPEND on some kernels: refuse them. */
    if ((slot->flags & (uint32_t)ASX_FS_OPEN_APPEND) != 0u) return ASX_E_INVALID_STATE;
    if (len == 0u) return ASX_OK;
    return fs_mem_write_at(&g_entries[slot->entry_slot], offset, src, len, out_written);
}

asx_status asx_fs_file_seek(asx_file_handle file, int64_t offset, asx_fs_seek_whence whence,
                            uint64_t *out_pos) {
    asx_open_file_slot *slot;
    int64_t base;
    int64_t target;

    if (whence != ASX_FS_SEEK_SET && whence != ASX_FS_SEEK_CUR && whence != ASX_FS_SEEK_END) {
        return ASX_E_INVALID_ARGUMENT;
    }
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_seek(file, offset, whence, out_pos);
#endif
    slot = fs_mem_lookup_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    switch (whence) {
    case ASX_FS_SEEK_CUR: base = (int64_t)slot->offset; break;
    case ASX_FS_SEEK_END: base = (int64_t)g_entries[slot->entry_slot].size; break;
    case ASX_FS_SEEK_SET:
    default: base = 0; break;
    }
    /* base is within [0, UINT32_MAX], so neither bound can overflow. */
    if (offset < -base) return ASX_E_INVALID_ARGUMENT;
    if (offset > (int64_t)UINT32_MAX - base) return ASX_E_INVALID_ARGUMENT;
    target = base + offset;
    slot->offset = (uint32_t)target;
    if (out_pos != NULL) *out_pos = (uint64_t)target;
    return ASX_OK;
}

asx_status asx_fs_file_rewind(asx_file_handle file) {
    return asx_fs_file_seek(file, 0, ASX_FS_SEEK_SET, NULL);
}

asx_status asx_fs_file_sync(asx_file_handle file) {
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_sync(file);
#endif
    return fs_mem_lookup_file(file) != NULL ? ASX_OK : ASX_E_NOT_FOUND;
}

asx_status asx_fs_file_set_len(asx_file_handle file, uint64_t len) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *e;
    asx_status st;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) {
        st = asx_native_fs_check(file, 0, 1);
        if (st != ASX_OK) return st;
        return asx_native_fs_set_len(file, len);
    }
#endif
    st = fs_mem_check(file, 0, 1, &slot);
    if (st != ASX_OK) return st;
    if (len > (uint64_t)ASX_FS_FILE_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;
    e = &g_entries[slot->entry_slot];
    if ((uint32_t)len > e->size) {
        fs_mem_extend(e, (uint32_t)len);
    } else {
        e->size = (uint32_t)len;
    }
    e->modified = ++g_fs_mod_counter;
    return ASX_OK;
}

asx_status asx_fs_file_metadata(asx_file_handle file, asx_fs_metadata *out) {
    asx_open_file_slot *slot;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_file_metadata(file, out);
#endif
    slot = fs_mem_lookup_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    fs_mem_fill_metadata(&g_entries[slot->entry_slot], out);
    return ASX_OK;
}

asx_status asx_fs_file_close(asx_file_handle file) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *e;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_close(file);
#endif
    slot = fs_mem_lookup_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->alive = 0;
    e = &g_entries[slot->entry_slot];
    if (e->open_count > 0u) e->open_count--;
    if (!e->linked && e->open_count == 0u) e->alive = 0; /* last reference to an unlinked inode */
    return ASX_OK;
}

asx_status asx_fs_file_path(asx_file_handle file, asx_fs_path *out) {
    asx_open_file_slot *slot;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_file_path(file, out);
#endif
    slot = fs_mem_lookup_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    *out = slot->path;
    return ASX_OK;
}

int asx_fs_file_is_alive(asx_file_handle file) {
#if ASX_FS_HAS_NATIVE
    if (FS_NATIVE(file)) return asx_native_fs_is_alive(file);
#endif
    return fs_mem_lookup_file(file) != NULL;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_fs_reset(void) {
    uint32_t i;

#if ASX_FS_HAS_NATIVE
    asx_native_fs_reset();
#endif
    g_fs_backend = ASX_FS_DEFAULT_BACKEND;
    memset(g_entries, 0, sizeof(g_entries));
    g_fs_mod_counter = 0u;
    for (i = 0; i < ASX_MAX_OPEN_FILES; i++) {
        uint32_t generation = asx_fs_next_generation(g_open_files[i].generation);
        memset(&g_open_files[i], 0, sizeof(g_open_files[i]));
        g_open_files[i].generation = generation;
    }
    for (i = 0; i < ASX_MAX_OPEN_DIRS; i++) {
        uint32_t generation = asx_fs_next_generation(g_open_dirs[i].generation);
        memset(&g_open_dirs[i], 0, sizeof(g_open_dirs[i]));
        g_open_dirs[i].generation = generation;
    }
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */
