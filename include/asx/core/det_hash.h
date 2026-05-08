/*
 * asx/core/det_hash.h — deterministic fixed-size hash map
 *
 * Open-addressing hash map with FNV-1a hashing. Deterministic iteration
 * order (insertion order). Fixed capacity, zero dynamic allocation.
 * Keys are uint64_t, values are uint64_t.
 *
 * For string-keyed maps, hash the string to uint64_t first using
 * asx_det_hash_str().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_DET_HASH_H
#define ASX_CORE_DET_HASH_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Capacity
 * ------------------------------------------------------------------- */

#ifndef ASX_DET_HASH_MAX_ENTRIES
#define ASX_DET_HASH_MAX_ENTRIES 64u
#endif

/* Load factor threshold (numerator/16). Rehash when load exceeds 12/16 = 75%. */
#define ASX_DET_HASH_LOAD_NUM 12u

/* Internal bucket count — next power of 2 >= entries * 16/12 */
#define ASX_DET_HASH_BUCKETS 128u

/* -------------------------------------------------------------------
 * Hash map types
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t key;
    uint64_t value;
    int occupied;
} asx_det_hash_entry;

typedef struct {
    /* Insertion-order array for deterministic iteration */
    asx_det_hash_entry entries[ASX_DET_HASH_MAX_ENTRIES];
    uint32_t count;

    /* Hash index for O(1) lookup: maps hash bucket -> entries[] index.
     * UINT32_MAX means empty bucket. */
    uint32_t buckets[ASX_DET_HASH_BUCKETS];
} asx_det_hash_map;

/* -------------------------------------------------------------------
 * Hash function
 * ------------------------------------------------------------------- */

/* FNV-1a hash of a uint64_t key. */
ASX_API uint64_t asx_det_hash_u64(uint64_t key);

/* FNV-1a hash of a NUL-terminated string. */
ASX_API uint64_t asx_det_hash_str(const char *s);

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Initialize an empty hash map. */
ASX_API void asx_det_hash_init(asx_det_hash_map *map);

/* Clear all entries. */
ASX_API void asx_det_hash_clear(asx_det_hash_map *map);

/* -------------------------------------------------------------------
 * Operations
 * ------------------------------------------------------------------- */

/* Insert or update a key-value pair.
 * Returns ASX_OK on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if map is full and key is new. */
ASX_API ASX_MUST_USE asx_status asx_det_hash_insert(asx_det_hash_map *map, uint64_t key,
                                                    uint64_t value);

/* Look up a key. Returns ASX_OK and sets *out_value if found.
 * Returns ASX_E_NOT_FOUND if key is absent. */
ASX_API ASX_MUST_USE asx_status asx_det_hash_get(const asx_det_hash_map *map, uint64_t key,
                                                 uint64_t *out_value);

/* Check if a key exists. Returns nonzero if present. */
ASX_API int asx_det_hash_contains(const asx_det_hash_map *map, uint64_t key);

/* Remove a key. Returns ASX_OK if removed, ASX_E_NOT_FOUND if absent. */
ASX_API ASX_MUST_USE asx_status asx_det_hash_remove(asx_det_hash_map *map, uint64_t key);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Number of entries in the map. */
ASX_API uint32_t asx_det_hash_count(const asx_det_hash_map *map);

/* Check if map is empty. */
ASX_API int asx_det_hash_is_empty(const asx_det_hash_map *map);

/* -------------------------------------------------------------------
 * Deterministic iteration (insertion order)
 * ------------------------------------------------------------------- */

/* Get entry at insertion-order index i.
 * Returns nonzero and fills out_key and out_value if valid.
 * Returns 0 if index is out of range or entry was removed. */
ASX_API int asx_det_hash_entry_at(const asx_det_hash_map *map, uint32_t i, uint64_t *out_key,
                                  uint64_t *out_value);

/* -------------------------------------------------------------------
 * Deterministic hash set (thin wrapper over hash map)
 * ------------------------------------------------------------------- */

typedef asx_det_hash_map asx_det_hash_set;

/* Initialize an empty hash set. */
ASX_API void asx_det_hash_set_init(asx_det_hash_set *set);

/* Clear all members. */
ASX_API void asx_det_hash_set_clear(asx_det_hash_set *set);

/* Insert a member. Returns ASX_OK or ASX_E_RESOURCE_EXHAUSTED. */
ASX_API ASX_MUST_USE asx_status asx_det_hash_set_insert(asx_det_hash_set *set, uint64_t key);

/* Check membership. Returns nonzero if present. */
ASX_API int asx_det_hash_set_contains(const asx_det_hash_set *set, uint64_t key);

/* Remove a member. Returns ASX_OK or ASX_E_NOT_FOUND. */
ASX_API ASX_MUST_USE asx_status asx_det_hash_set_remove(asx_det_hash_set *set, uint64_t key);

/* Number of members. */
ASX_API uint32_t asx_det_hash_set_count(const asx_det_hash_set *set);

/* Check if empty. */
ASX_API int asx_det_hash_set_is_empty(const asx_det_hash_set *set);

/* Get member at insertion-order index. Returns nonzero and fills out_key. */
ASX_API int asx_det_hash_set_member_at(const asx_det_hash_set *set, uint32_t i, uint64_t *out_key);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_DET_HASH_H */
