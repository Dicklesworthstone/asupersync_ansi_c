/*
 * det_hash.c — deterministic fixed-size hash map
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/det_hash.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* FNV-1a constants                                                    */
/* ------------------------------------------------------------------ */

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

/* ------------------------------------------------------------------ */
/* Hash functions                                                      */
/* ------------------------------------------------------------------ */

uint64_t asx_det_hash_u64(uint64_t key) {
    uint64_t h = FNV_OFFSET;
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (key & 0xFFu);
        h *= FNV_PRIME;
        key >>= 8;
    }
    return h;
}

uint64_t asx_det_hash_str(const char *s) {
    uint64_t h = FNV_OFFSET;
    if (s == NULL) return h;
    while (*s != '\0') {
        /* ASX_CHECKPOINT_WAIVER("bounded hash computation") */
        h ^= (uint64_t)(uint8_t)*s;
        h *= FNV_PRIME;
        s++;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t bucket_index(uint64_t hash) {
    return (uint32_t)(hash & (ASX_DET_HASH_BUCKETS - 1u));
}

/* Find the bucket containing key, or the first empty bucket for insertion.
 * Sets *out_bucket to the bucket index. Returns the entries[] index if found,
 * or UINT32_MAX if key is not present. */
static uint32_t probe(const asx_det_hash_map *map, uint64_t key, uint32_t *out_bucket) {
    uint64_t h = asx_det_hash_u64(key);
    uint32_t b = bucket_index(h);
    uint32_t i;

    for (i = 0; i < ASX_DET_HASH_BUCKETS; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded hash computation") */
        uint32_t idx = map->buckets[b];
        if (idx == UINT32_MAX) {
            /* Empty bucket — key not found */
            *out_bucket = b;
            return UINT32_MAX;
        }
        if (idx < ASX_DET_HASH_MAX_ENTRIES && map->entries[idx].occupied &&
            map->entries[idx].key == key) {
            *out_bucket = b;
            return idx;
        }
        b = (b + 1u) & (ASX_DET_HASH_BUCKETS - 1u);
    }

    *out_bucket = b;
    return UINT32_MAX;
}

/* Rebuild the bucket index from the entries array. */
static void rebuild_buckets(asx_det_hash_map *map) {
    uint32_t i;
    memset(map->buckets, 0xFF, sizeof(map->buckets)); /* fill with UINT32_MAX */

    for (i = 0; i < map->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (map->entries[i].occupied) {
            uint64_t h = asx_det_hash_u64(map->entries[i].key);
            uint32_t b = bucket_index(h);
            uint32_t j;
            for (j = 0; j < ASX_DET_HASH_BUCKETS; j++) {
                /* ASX_CHECKPOINT_WAIVER("bounded hash computation") */
                if (map->buckets[b] == UINT32_MAX) {
                    map->buckets[b] = i;
                    break;
                }
                b = (b + 1u) & (ASX_DET_HASH_BUCKETS - 1u);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void asx_det_hash_init(asx_det_hash_map *map) {
    if (map == NULL) return;
    map->count = 0;
    memset(map->entries, 0, sizeof(map->entries));
    memset(map->buckets, 0xFF, sizeof(map->buckets));
}

void asx_det_hash_clear(asx_det_hash_map *map) { asx_det_hash_init(map); }

/* ------------------------------------------------------------------ */
/* Insert                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_det_hash_insert(asx_det_hash_map *map, uint64_t key, uint64_t value) {
    uint32_t bucket;
    uint32_t idx;

    if (map == NULL) return ASX_E_INVALID_ARGUMENT;

    idx = probe(map, key, &bucket);

    if (idx != UINT32_MAX) {
        /* Key exists — update value */
        map->entries[idx].value = value;
        return ASX_OK;
    }

    /* New key — check capacity */
    if (map->count >= ASX_DET_HASH_MAX_ENTRIES) return ASX_E_RESOURCE_EXHAUSTED;

    /* Append to entries array */
    idx = map->count;
    map->entries[idx].key = key;
    map->entries[idx].value = value;
    map->entries[idx].occupied = 1;
    map->count++;

    /* Place in bucket */
    map->buckets[bucket] = idx;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Get                                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_det_hash_get(const asx_det_hash_map *map, uint64_t key, uint64_t *out_value) {
    uint32_t bucket;
    uint32_t idx;

    if (map == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;

    idx = probe(map, key, &bucket);
    if (idx == UINT32_MAX) return ASX_E_NOT_FOUND;

    *out_value = map->entries[idx].value;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Contains                                                            */
/* ------------------------------------------------------------------ */

int asx_det_hash_contains(const asx_det_hash_map *map, uint64_t key) {
    uint32_t bucket;
    if (map == NULL) return 0;
    return probe(map, key, &bucket) != UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/* Remove                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_det_hash_remove(asx_det_hash_map *map, uint64_t key) {
    uint32_t bucket;
    uint32_t idx;

    if (map == NULL) return ASX_E_INVALID_ARGUMENT;

    idx = probe(map, key, &bucket);
    if (idx == UINT32_MAX) return ASX_E_NOT_FOUND;

    /* Mark entry as removed (tombstone) */
    map->entries[idx].occupied = 0;

    /* Rebuild bucket index to handle probe chain breaks */
    rebuild_buckets(map);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_det_hash_count(const asx_det_hash_map *map) {
    uint32_t i, count = 0;
    if (map == NULL) return 0;
    for (i = 0; i < map->count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        if (map->entries[i].occupied) count++;
    }
    return count;
}

int asx_det_hash_is_empty(const asx_det_hash_map *map) { return asx_det_hash_count(map) == 0; }

/* ------------------------------------------------------------------ */
/* Deterministic iteration                                             */
/* ------------------------------------------------------------------ */

int asx_det_hash_entry_at(const asx_det_hash_map *map, uint32_t i, uint64_t *out_key,
                          uint64_t *out_value) {
    if (map == NULL || i >= map->count) return 0;
    if (!map->entries[i].occupied) return 0;
    if (out_key != NULL) *out_key = map->entries[i].key;
    if (out_value != NULL) *out_value = map->entries[i].value;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Deterministic hash set (thin wrapper)                               */
/* ------------------------------------------------------------------ */

void asx_det_hash_set_init(asx_det_hash_set *set) { asx_det_hash_init(set); }

void asx_det_hash_set_clear(asx_det_hash_set *set) { asx_det_hash_clear(set); }

asx_status asx_det_hash_set_insert(asx_det_hash_set *set, uint64_t key) {
    return asx_det_hash_insert(set, key, 0);
}

int asx_det_hash_set_contains(const asx_det_hash_set *set, uint64_t key) {
    return asx_det_hash_contains(set, key);
}

asx_status asx_det_hash_set_remove(asx_det_hash_set *set, uint64_t key) {
    return asx_det_hash_remove(set, key);
}

uint32_t asx_det_hash_set_count(const asx_det_hash_set *set) { return asx_det_hash_count(set); }

int asx_det_hash_set_is_empty(const asx_det_hash_set *set) { return asx_det_hash_is_empty(set); }

int asx_det_hash_set_member_at(const asx_det_hash_set *set, uint32_t i, uint64_t *out_key) {
    return asx_det_hash_entry_at(set, i, out_key, NULL);
}
