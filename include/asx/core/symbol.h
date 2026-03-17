/*
 * asx/core/symbol.h — symbol registry, symbol sets, and typed symbols
 *
 * Provides interned identifiers for named concepts (metrics, events,
 * capabilities, payloads) that downstream diagnostics, services,
 * and interoperability surfaces use to identify typed values.
 *
 * Design:
 *   - Symbols are uint16_t IDs mapped to interned string names.
 *   - Registry is static (no dynamic allocation), suitable for embedded.
 *   - Symbol sets use a 256-bit bitfield for O(1) membership testing.
 *   - Typed symbols pair a symbol ID with type metadata (kind, size, alignment).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_SYMBOL_H
#define ASX_CORE_SYMBOL_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Symbol ID                                                           */
/* ------------------------------------------------------------------ */

typedef uint16_t asx_symbol_id;

#define ASX_SYMBOL_INVALID ((asx_symbol_id)0)
#define ASX_SYMBOL_REGISTRY_CAPACITY 256u

/* ------------------------------------------------------------------ */
/* Symbol registry                                                     */
/* ------------------------------------------------------------------ */

/* Reset the global symbol registry (clears all registrations). */
ASX_API void asx_symbol_registry_reset(void);

/* Register a named symbol. Returns ASX_OK and sets *out_id on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if the registry is full.
 * Returns ASX_E_INVALID_ARGUMENT if name is NULL or empty.
 * If the name is already registered, returns the existing ID (idempotent). */
ASX_API ASX_MUST_USE asx_status asx_symbol_register(const char *name, asx_symbol_id *out_id);

/* Look up a symbol by name. Returns ASX_OK if found, ASX_E_NOT_FOUND otherwise. */
ASX_API ASX_MUST_USE asx_status asx_symbol_lookup(const char *name, asx_symbol_id *out_id);

/* Get the name string for a registered symbol ID.
 * Returns NULL for invalid or unregistered IDs. */
ASX_API ASX_MUST_USE const char *asx_symbol_name(asx_symbol_id id);

/* Returns the number of currently registered symbols (excluding INVALID). */
ASX_API ASX_MUST_USE uint16_t asx_symbol_count(void);

/* Returns nonzero if the ID refers to a valid registered symbol. */
ASX_API ASX_MUST_USE int asx_symbol_is_valid(asx_symbol_id id);

/* ------------------------------------------------------------------ */
/* Symbol set — bitfield for efficient membership testing              */
/* ------------------------------------------------------------------ */

/* 256-bit bitfield: one bit per possible symbol ID */
typedef struct {
    uint64_t bits[4]; /* bits[i] covers IDs [64*i .. 64*i+63] */
} asx_symbol_set;

/* Initialize an empty set. */
ASX_API void asx_symbol_set_init(asx_symbol_set *set);

/* Insert a symbol ID into the set. */
ASX_API void asx_symbol_set_insert(asx_symbol_set *set, asx_symbol_id id);

/* Remove a symbol ID from the set. */
ASX_API void asx_symbol_set_remove(asx_symbol_set *set, asx_symbol_id id);

/* Test membership. Returns nonzero if id is in the set. */
ASX_API ASX_MUST_USE int asx_symbol_set_contains(const asx_symbol_set *set, asx_symbol_id id);

/* Returns the number of symbols in the set. */
ASX_API ASX_MUST_USE uint16_t asx_symbol_set_count(const asx_symbol_set *set);

/* Returns nonzero if the set is empty. */
ASX_API ASX_MUST_USE int asx_symbol_set_is_empty(const asx_symbol_set *set);

/* Set union: result = a ∪ b. */
ASX_API void asx_symbol_set_union(asx_symbol_set *result, const asx_symbol_set *a,
                                  const asx_symbol_set *b);

/* Set intersection: result = a ∩ b. */
ASX_API void asx_symbol_set_intersect(asx_symbol_set *result, const asx_symbol_set *a,
                                      const asx_symbol_set *b);

/* Returns nonzero if a is a subset of b. */
ASX_API ASX_MUST_USE int asx_symbol_set_is_subset(const asx_symbol_set *a, const asx_symbol_set *b);

/* Returns nonzero if a and b are equal. */
ASX_API ASX_MUST_USE int asx_symbol_set_equals(const asx_symbol_set *a, const asx_symbol_set *b);

/* ------------------------------------------------------------------ */
/* Typed symbol — symbol ID paired with type metadata                  */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_TYPE_KIND_VOID = 0, /* no payload */
    ASX_TYPE_KIND_U8 = 1,
    ASX_TYPE_KIND_U16 = 2,
    ASX_TYPE_KIND_U32 = 3,
    ASX_TYPE_KIND_U64 = 4,
    ASX_TYPE_KIND_I32 = 5,
    ASX_TYPE_KIND_I64 = 6,
    ASX_TYPE_KIND_F32 = 7,
    ASX_TYPE_KIND_F64 = 8,
    ASX_TYPE_KIND_BOOL = 9,
    ASX_TYPE_KIND_BYTES = 10,  /* variable-length byte buffer */
    ASX_TYPE_KIND_STRING = 11, /* NUL-terminated UTF-8 */
    ASX_TYPE_KIND_STRUCT = 12, /* opaque struct with known size/align */
    ASX_TYPE_KIND_COUNT = 13
} asx_type_kind;

typedef struct {
    asx_symbol_id symbol; /* which concept this describes */
    asx_type_kind kind;   /* payload type */
    uint16_t size;        /* payload size in bytes (0 for variable) */
    uint16_t alignment;   /* required alignment (0 = default) */
} asx_typed_symbol;

/* Register a typed symbol. Returns ASX_OK on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if registry full.
 * Returns ASX_E_INVALID_ARGUMENT if symbol_name is NULL/empty.
 * If name already registered with same type, idempotent.
 * If name registered with DIFFERENT type, returns ASX_E_INVALID_STATE (mismatch). */
ASX_API ASX_MUST_USE asx_status asx_typed_symbol_register(const char *symbol_name,
                                                          asx_type_kind kind, uint16_t size,
                                                          uint16_t alignment,
                                                          asx_typed_symbol *out);

/* Look up a typed symbol by name.
 * Returns ASX_OK if found, ASX_E_NOT_FOUND otherwise. */
ASX_API ASX_MUST_USE asx_status asx_typed_symbol_lookup(const char *symbol_name,
                                                        asx_typed_symbol *out);

/* Look up a typed symbol by ID.
 * Returns ASX_OK if found, ASX_E_NOT_FOUND otherwise. */
ASX_API ASX_MUST_USE asx_status asx_typed_symbol_get(asx_symbol_id id, asx_typed_symbol *out);

/* Human-readable name for a type kind. Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_type_kind_str(asx_type_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_SYMBOL_H */
