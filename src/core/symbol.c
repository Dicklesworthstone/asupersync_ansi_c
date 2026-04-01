/*
 * symbol.c — symbol registry, symbol sets, and typed symbols
 *
 * Static registry with no dynamic allocation. All storage is compile-time
 * fixed, suitable for embedded and freestanding profiles.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/symbol.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Symbol registry (static table)                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;        /* interned string (not copied) */
    asx_type_kind type_kind; /* typed symbol metadata */
    uint16_t type_size;
    uint16_t type_align;
    int has_type; /* nonzero if typed_symbol_register was used */
} symbol_entry;

static symbol_entry g_registry[ASX_SYMBOL_REGISTRY_CAPACITY];
static uint16_t g_count = 0; /* next ID to assign (1-based) */

void asx_symbol_registry_reset(void) {
    memset(g_registry, 0, sizeof(g_registry));
    g_count = 0;
}

asx_status asx_symbol_register(const char *name, asx_symbol_id *out_id) {
    uint16_t i;

    if (name == NULL || name[0] == '\0' || out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Check for existing registration (idempotent) */
    for (i = 0; i < g_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (g_registry[i].name != NULL && strcmp(g_registry[i].name, name) == 0) {
            *out_id = (asx_symbol_id)(i + 1);
            return ASX_OK;
        }
    }

    /* Register new */
    if (g_count >= ASX_SYMBOL_REGISTRY_CAPACITY) return ASX_E_RESOURCE_EXHAUSTED;

    g_registry[g_count].name = name;
    g_registry[g_count].has_type = 0;
    *out_id = (asx_symbol_id)(g_count + 1);
    g_count++;
    return ASX_OK;
}

asx_status asx_symbol_lookup(const char *name, asx_symbol_id *out_id) {
    uint16_t i;

    if (name == NULL || out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < g_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (g_registry[i].name != NULL && strcmp(g_registry[i].name, name) == 0) {
            *out_id = (asx_symbol_id)(i + 1);
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

const char *asx_symbol_name(asx_symbol_id id) {
    if (id == ASX_SYMBOL_INVALID || id > g_count) return NULL;
    return g_registry[id - 1].name;
}

uint16_t asx_symbol_count(void) { return g_count; }

int asx_symbol_is_valid(asx_symbol_id id) {
    return id != ASX_SYMBOL_INVALID && id <= g_count && g_registry[id - 1].name != NULL;
}

/* ------------------------------------------------------------------ */
/* Symbol set                                                          */
/* ------------------------------------------------------------------ */

static int symbol_set_bit_index(asx_symbol_id id, uint16_t *word_index, uint16_t *bit_index) {
    uint16_t zero_based;

    if (word_index == NULL || bit_index == NULL) return 0;
    if (id == ASX_SYMBOL_INVALID || id > ASX_SYMBOL_REGISTRY_CAPACITY) return 0;

    zero_based = (uint16_t)(id - 1u);
    *word_index = (uint16_t)(zero_based / 64u);
    *bit_index = (uint16_t)(zero_based % 64u);
    return 1;
}

void asx_symbol_set_init(asx_symbol_set *set) {
    if (set == NULL) return;
    memset(set, 0, sizeof(*set));
}

void asx_symbol_set_insert(asx_symbol_set *set, asx_symbol_id id) {
    uint16_t word_index;
    uint16_t bit_index;

    if (set == NULL) return;
    if (!symbol_set_bit_index(id, &word_index, &bit_index)) return;

    set->bits[word_index] |= ((uint64_t)1 << bit_index);
}

void asx_symbol_set_remove(asx_symbol_set *set, asx_symbol_id id) {
    uint16_t word_index;
    uint16_t bit_index;

    if (set == NULL) return;
    if (!symbol_set_bit_index(id, &word_index, &bit_index)) return;

    set->bits[word_index] &= ~((uint64_t)1 << bit_index);
}

int asx_symbol_set_contains(const asx_symbol_set *set, asx_symbol_id id) {
    uint16_t word_index;
    uint16_t bit_index;

    if (set == NULL) return 0;
    if (!symbol_set_bit_index(id, &word_index, &bit_index)) return 0;

    return (set->bits[word_index] & ((uint64_t)1 << bit_index)) != 0;
}

/* Portable popcount for uint64_t */
static uint16_t popcount64(uint64_t x) {
    uint16_t count = 0;
    while (x) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        count++;
        x &= x - 1; /* clear lowest set bit */
    }
    return count;
}

uint16_t asx_symbol_set_count(const asx_symbol_set *set) {
    uint16_t total = 0;
    int i;
    if (set == NULL) return 0;
    for (i = 0; i < 4; i++) total += popcount64(set->bits[i]);
    return total;
}

int asx_symbol_set_is_empty(const asx_symbol_set *set) {
    if (set == NULL) return 1;
    return (set->bits[0] | set->bits[1] | set->bits[2] | set->bits[3]) == 0;
}

void asx_symbol_set_union(asx_symbol_set *result, const asx_symbol_set *a,
                          const asx_symbol_set *b) {
    int i;
    if (result == NULL || a == NULL || b == NULL) return;
    for (i = 0; i < 4; i++) result->bits[i] = a->bits[i] | b->bits[i];
}

void asx_symbol_set_intersect(asx_symbol_set *result, const asx_symbol_set *a,
                              const asx_symbol_set *b) {
    int i;
    if (result == NULL || a == NULL || b == NULL) return;
    for (i = 0; i < 4; i++) result->bits[i] = a->bits[i] & b->bits[i];
}

int asx_symbol_set_is_subset(const asx_symbol_set *a, const asx_symbol_set *b) {
    int i;
    if (a == NULL || b == NULL) return 0;
    for (i = 0; i < 4; i++) {
        if ((a->bits[i] & ~b->bits[i]) != 0) return 0;
    }
    return 1;
}

int asx_symbol_set_equals(const asx_symbol_set *a, const asx_symbol_set *b) {
    if (a == NULL || b == NULL) return 0;
    return memcmp(a->bits, b->bits, sizeof(a->bits)) == 0;
}

/* ------------------------------------------------------------------ */
/* Typed symbols                                                       */
/* ------------------------------------------------------------------ */

asx_status asx_typed_symbol_register(const char *symbol_name, asx_type_kind kind, uint16_t size,
                                     uint16_t alignment, asx_typed_symbol *out) {
    asx_symbol_id id;
    asx_status s;
    symbol_entry *entry;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    s = asx_symbol_register(symbol_name, &id);
    if (s != ASX_OK) return s;

    entry = &g_registry[id - 1];

    /* If already typed, check for mismatch */
    if (entry->has_type) {
        if (entry->type_kind != kind || entry->type_size != size ||
            entry->type_align != alignment) {
            return ASX_E_INVALID_STATE; /* type mismatch */
        }
    } else {
        entry->type_kind = kind;
        entry->type_size = size;
        entry->type_align = alignment;
        entry->has_type = 1;
    }

    out->symbol = id;
    out->kind = entry->type_kind;
    out->size = entry->type_size;
    out->alignment = entry->type_align;
    return ASX_OK;
}

asx_status asx_typed_symbol_lookup(const char *symbol_name, asx_typed_symbol *out) {
    asx_symbol_id id;
    asx_status s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    s = asx_symbol_lookup(symbol_name, &id);
    if (s != ASX_OK) return s;

    return asx_typed_symbol_get(id, out);
}

asx_status asx_typed_symbol_get(asx_symbol_id id, asx_typed_symbol *out) {
    symbol_entry *entry;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id == ASX_SYMBOL_INVALID || id > g_count) return ASX_E_NOT_FOUND;

    entry = &g_registry[id - 1];
    if (!entry->has_type) return ASX_E_NOT_FOUND;

    out->symbol = id;
    out->kind = entry->type_kind;
    out->size = entry->type_size;
    out->alignment = entry->type_align;
    return ASX_OK;
}

const char *asx_type_kind_str(asx_type_kind kind) {
    switch (kind) {
    case ASX_TYPE_KIND_VOID: return "void";
    case ASX_TYPE_KIND_U8: return "u8";
    case ASX_TYPE_KIND_U16: return "u16";
    case ASX_TYPE_KIND_U32: return "u32";
    case ASX_TYPE_KIND_U64: return "u64";
    case ASX_TYPE_KIND_I32: return "i32";
    case ASX_TYPE_KIND_I64: return "i64";
    case ASX_TYPE_KIND_F32: return "f32";
    case ASX_TYPE_KIND_F64: return "f64";
    case ASX_TYPE_KIND_BOOL: return "bool";
    case ASX_TYPE_KIND_BYTES: return "bytes";
    case ASX_TYPE_KIND_STRING: return "string";
    case ASX_TYPE_KIND_STRUCT: return "struct";
    case ASX_TYPE_KIND_COUNT: return "count";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Typed value                                                         */
/* ------------------------------------------------------------------ */

asx_status asx_typed_value_init(asx_typed_value *val, asx_symbol_id symbol, asx_type_kind kind,
                                const void *payload, uint32_t payload_size) {
    if (val == NULL) return ASX_E_INVALID_ARGUMENT;
    val->symbol = symbol;
    val->kind = kind;
    val->payload = payload;
    val->payload_size = payload_size;
    return ASX_OK;
}

int asx_typed_value_equals(const asx_typed_value *a, const asx_typed_value *b) {
    if (a == NULL || b == NULL) return a == b;
    if (a->symbol != b->symbol) return 0;
    if (a->kind != b->kind) return 0;
    if (a->payload_size != b->payload_size) return 0;
    if (a->payload_size == 0) return 1;
    if (a->payload == NULL || b->payload == NULL) return a->payload == b->payload;
    return memcmp(a->payload, b->payload, a->payload_size) == 0;
}

/* ------------------------------------------------------------------ */
/* Symbol set iteration                                                */
/* ------------------------------------------------------------------ */

void asx_symbol_set_iterate(const asx_symbol_set *set, asx_symbol_set_iter_fn fn, void *ctx) {
    uint32_t word, bit;
    if (set == NULL || fn == NULL) return;
    for (word = 0; word < 4; word++) {
        uint64_t w = set->bits[word];
        if (w == 0) continue;
        for (bit = 0; bit < 64; bit++) {
            if (w & (1ULL << bit)) {
                /* Symbol IDs are 1-based: bit 0 in word 0 = symbol ID 1 */
                asx_symbol_id id = (asx_symbol_id)(word * 64 + bit + 1);
                if (fn(id, ctx) != 0) return;
            }
        }
    }
}
