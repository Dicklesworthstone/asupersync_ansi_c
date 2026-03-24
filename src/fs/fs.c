/*
 * fs.c — deterministic filesystem host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/fs/fs.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

typedef struct {
    asx_fs_path path;
    asx_fs_entry_kind kind;
    uint32_t size;
    uint8_t data[ASX_FS_FILE_CAPACITY];
    int alive;
} asx_fs_entry_slot;

typedef struct {
    uint32_t entry_slot;
    uint32_t generation;
    uint32_t offset;
    uint32_t flags;
    int alive;
} asx_open_file_slot;

static asx_fs_entry_slot g_entries[ASX_MAX_FS_ENTRIES];
static asx_open_file_slot g_open_files[ASX_MAX_OPEN_FILES];

static uint32_t asx_fs_next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

static int asx_fs_find_entry(const asx_fs_path *path) {
    uint32_t i;
    if (path == NULL) return -1;
    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive) continue;
        if (asx_fs_path_eq(&g_entries[i].path, path)) return (int)i;
    }
    return -1;
}

static asx_fs_entry_slot *asx_fs_require_entry(int index) {
    if (index < 0) return NULL;
    if ((uint32_t)index >= ASX_MAX_FS_ENTRIES) return NULL;
    if (!g_entries[index].alive) return NULL;
    return &g_entries[index];
}

static asx_open_file_slot *asx_fs_lookup_open_file(asx_file_handle file) {
    asx_open_file_slot *slot;
    if (file.slot >= ASX_MAX_OPEN_FILES) return NULL;
    slot = &g_open_files[file.slot];
    if (!slot->alive) return NULL;
    if (slot->generation != file.generation) return NULL;
    return slot;
}

static asx_status asx_fs_create_entry(const asx_fs_path *path, asx_fs_entry_kind kind,
                                      int *out_index) {
    uint32_t i;

    if (path == NULL || out_index == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_MAX_FS_ENTRIES; i++) {
        if (!g_entries[i].alive) {
            memset(&g_entries[i], 0, sizeof(g_entries[i]));
            g_entries[i].alive = 1;
            g_entries[i].kind = kind;
            g_entries[i].path = *path;
            *out_index = (int)i;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

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

int asx_fs_path_eq(const asx_fs_path *a, const asx_fs_path *b) {
    if (a == NULL || b == NULL) return 0;
    if (a->len != b->len) return 0;
    return memcmp(a->text, b->text, a->len) == 0;
}

asx_status asx_fs_dir_create(const asx_fs_path *path) {
    int idx;
    asx_fs_entry_slot *entry;

    if (path == NULL) return ASX_E_INVALID_ARGUMENT;
    idx = asx_fs_find_entry(path);
    if (idx >= 0) {
        entry = asx_fs_require_entry(idx);
        if (entry == NULL) return ASX_E_NOT_FOUND;
        if (entry->kind != ASX_FS_ENTRY_DIR) return ASX_E_ALREADY_EXISTS;
        return ASX_OK;
    }
    return asx_fs_create_entry(path, ASX_FS_ENTRY_DIR, &idx);
}

asx_status asx_fs_metadata_query(const asx_fs_path *path, asx_fs_metadata *out) {
    int idx;
    asx_fs_entry_slot *entry;

    if (path == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    idx = asx_fs_find_entry(path);
    if (idx < 0) return ASX_E_NOT_FOUND;

    entry = asx_fs_require_entry(idx);
    if (entry == NULL) return ASX_E_NOT_FOUND;

    out->kind = entry->kind;
    out->size = entry->size;
    out->exists = 1;
    out->writable = 1;
    return ASX_OK;
}

asx_status asx_fs_file_open(asx_file_handle *out, const asx_fs_path *path, uint32_t flags) {
    uint32_t i;
    int idx;
    asx_fs_entry_slot *entry;
    asx_open_file_slot *slot;
    asx_status st;

    if (out == NULL || path == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((flags & (ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE)) == 0u) { return ASX_E_INVALID_ARGUMENT; }
    if ((flags & ASX_FS_OPEN_TRUNC) != 0u && (flags & ASX_FS_OPEN_WRITE) == 0u) {
        return ASX_E_PERMISSION_DENIED;
    }

    idx = asx_fs_find_entry(path);
    if (idx < 0) {
        if ((flags & ASX_FS_OPEN_CREATE) == 0u) return ASX_E_NOT_FOUND;
        st = asx_fs_create_entry(path, ASX_FS_ENTRY_FILE, &idx);
        if (st != ASX_OK) return st;
    }

    entry = asx_fs_require_entry(idx);
    if (entry == NULL) return ASX_E_NOT_FOUND;
    if (entry->kind != ASX_FS_ENTRY_FILE) return ASX_E_INVALID_STATE;
    if ((flags & ASX_FS_OPEN_TRUNC) != 0u) entry->size = 0u;

    for (i = 0; i < ASX_MAX_OPEN_FILES; i++) {
        if (!g_open_files[i].alive) {
            slot = &g_open_files[i];
            slot->alive = 1;
            slot->generation = asx_fs_next_generation(slot->generation);
            slot->entry_slot = (uint32_t)idx;
            slot->offset = 0u;
            slot->flags = flags;
            out->slot = i;
            out->generation = slot->generation;
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_fs_file_poll_read(asx_file_handle file, asx_buf_mut *dst, uint32_t *bytes_read) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *entry;
    uint32_t available;
    uint32_t writable;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_fs_lookup_open_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if ((slot->flags & ASX_FS_OPEN_READ) == 0u) return ASX_E_PERMISSION_DENIED;

    entry = asx_fs_require_entry((int)slot->entry_slot);
    if (entry == NULL) return ASX_E_NOT_FOUND;

    *bytes_read = 0u;
    if (slot->offset >= entry->size) return ASX_E_PENDING;

    available = entry->size - slot->offset;
    writable = asx_buf_mut_writable(dst);
    to_copy = available < writable ? available : writable;
    if (to_copy == 0u) return ASX_E_BUFFER_TOO_SMALL;

    st = asx_buf_mut_put(dst, &entry->data[slot->offset], to_copy);
    if (st != ASX_OK) return st;

    slot->offset += to_copy;
    *bytes_read = to_copy;
    return ASX_OK;
}

asx_status asx_fs_file_poll_write(asx_file_handle file, const asx_buf *src,
                                  uint32_t *bytes_written) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *entry;
    uint32_t remaining;
    uint32_t to_copy;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_fs_lookup_open_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    if ((slot->flags & ASX_FS_OPEN_WRITE) == 0u) return ASX_E_PERMISSION_DENIED;

    entry = asx_fs_require_entry((int)slot->entry_slot);
    if (entry == NULL) return ASX_E_NOT_FOUND;

    *bytes_written = 0u;
    if (src->len == 0u) return ASX_OK;
    if (src->ptr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (slot->offset > ASX_FS_FILE_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    remaining = ASX_FS_FILE_CAPACITY - slot->offset;
    to_copy = src->len < remaining ? src->len : remaining;
    if (to_copy == 0u) return ASX_E_RESOURCE_EXHAUSTED;

    memcpy(&entry->data[slot->offset], src->ptr, to_copy);
    slot->offset += to_copy;
    if (slot->offset > entry->size) entry->size = slot->offset;
    *bytes_written = to_copy;

    if (to_copy < src->len) return ASX_E_RESOURCE_EXHAUSTED;
    return ASX_OK;
}

asx_status asx_fs_file_rewind(asx_file_handle file) {
    asx_open_file_slot *slot = asx_fs_lookup_open_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->offset = 0u;
    return ASX_OK;
}

asx_status asx_fs_file_close(asx_file_handle file) {
    asx_open_file_slot *slot = asx_fs_lookup_open_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    slot->alive = 0;
    return ASX_OK;
}

asx_status asx_fs_file_path(asx_file_handle file, asx_fs_path *out) {
    asx_open_file_slot *slot;
    asx_fs_entry_slot *entry;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    slot = asx_fs_lookup_open_file(file);
    if (slot == NULL) return ASX_E_NOT_FOUND;
    entry = asx_fs_require_entry((int)slot->entry_slot);
    if (entry == NULL) return ASX_E_NOT_FOUND;
    *out = entry->path;
    return ASX_OK;
}

int asx_fs_file_is_alive(asx_file_handle file) { return asx_fs_lookup_open_file(file) != NULL; }

void asx_fs_reset(void) {
    uint32_t i;

    memset(g_entries, 0, sizeof(g_entries));
    for (i = 0; i < ASX_MAX_OPEN_FILES; i++) {
        g_open_files[i].generation = asx_fs_next_generation(g_open_files[i].generation);
        g_open_files[i].entry_slot = 0u;
        g_open_files[i].offset = 0u;
        g_open_files[i].flags = 0u;
        g_open_files[i].alive = 0;
    }
}

#endif /* ASX_HAS_NATIVE_RUNTIME_SURFACES */
