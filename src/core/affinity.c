/*
 * affinity.c — thread-affinity domain guards and transfer certificates
 *
 * Compile-time gated: entire file is a no-op unless ASX_DEBUG_AFFINITY
 * is defined. When active, provides deterministic domain tracking with
 * zero heap allocation (fixed-size tracking table).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/affinity.h>

#ifdef ASX_DEBUG_AFFINITY

#include <string.h>

/* -------------------------------------------------------------------
 * Tracking table: maps entity IDs to their affinity domains
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t entity_id;
    asx_affinity_domain domain;
    int occupied;
} asx_affinity_entry;

static asx_affinity_entry g_affinity_table[ASX_AFFINITY_TABLE_CAPACITY];
static uint32_t g_affinity_count;
static asx_affinity_domain g_current_domain = ASX_AFFINITY_DOMAIN_ANY;

/* -------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------- */

static asx_affinity_entry *affinity_find(uint64_t entity_id) {
    uint32_t i;
    for (i = 0; i < ASX_AFFINITY_TABLE_CAPACITY; i++) {
        if (g_affinity_table[i].occupied && g_affinity_table[i].entity_id == entity_id) {
            return &g_affinity_table[i];
        }
    }
    return NULL;
}

static asx_affinity_entry *affinity_alloc(void) {
    uint32_t i;
    for (i = 0; i < ASX_AFFINITY_TABLE_CAPACITY; i++) {
        if (!g_affinity_table[i].occupied) { return &g_affinity_table[i]; }
    }
    return NULL;
}

/* -------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

void asx_affinity_reset(void) {
    memset(g_affinity_table, 0, sizeof(g_affinity_table));
    g_affinity_count = 0;
    g_current_domain = ASX_AFFINITY_DOMAIN_ANY;
}

void asx_affinity_set_domain(asx_affinity_domain domain) { g_current_domain = domain; }

asx_affinity_domain asx_affinity_current_domain(void) { return g_current_domain; }

asx_status asx_affinity_bind(uint64_t entity_id, asx_affinity_domain domain) {
    asx_affinity_entry *entry;

    if (entity_id == ASX_INVALID_ID) { return ASX_E_INVALID_ARGUMENT; }

    entry = affinity_find(entity_id);
    if (entry != NULL) {
        if (entry->domain != domain && entry->domain != ASX_AFFINITY_DOMAIN_ANY) {
            return ASX_E_AFFINITY_ALREADY_BOUND;
        }
        entry->domain = domain;
        return ASX_OK;
    }

    entry = affinity_alloc();
    if (entry == NULL) { return ASX_E_AFFINITY_TABLE_FULL; }

    entry->entity_id = entity_id;
    entry->domain = domain;
    entry->occupied = 1;
    g_affinity_count++;

    return ASX_OK;
}

asx_status asx_affinity_check(uint64_t entity_id) {
    asx_affinity_entry *entry;

    if (entity_id == ASX_INVALID_ID) { return ASX_E_INVALID_ARGUMENT; }

    entry = affinity_find(entity_id);
    if (entry == NULL) {
        /* Untracked entities pass: binding is opt-in */
        return ASX_OK;
    }

    /* Domain ANY always passes */
    if (entry->domain == ASX_AFFINITY_DOMAIN_ANY) { return ASX_OK; }

    /* Current domain ANY can access anything */
    if (g_current_domain == ASX_AFFINITY_DOMAIN_ANY) { return ASX_OK; }

    if (g_current_domain != entry->domain) { return ASX_E_AFFINITY_VIOLATION; }

    return ASX_OK;
}

asx_status asx_affinity_transfer(uint64_t entity_id, asx_affinity_domain to_domain) {
    asx_affinity_entry *entry;

    if (entity_id == ASX_INVALID_ID) { return ASX_E_INVALID_ARGUMENT; }

    entry = affinity_find(entity_id);
    if (entry == NULL) { return ASX_E_AFFINITY_NOT_BOUND; }

    /* Caller must be in the entity's current domain to transfer */
    if (entry->domain != ASX_AFFINITY_DOMAIN_ANY && g_current_domain != ASX_AFFINITY_DOMAIN_ANY &&
        g_current_domain != entry->domain) {
        return ASX_E_AFFINITY_VIOLATION;
    }

    entry->domain = to_domain;
    return ASX_OK;
}

asx_status asx_affinity_get_domain(uint64_t entity_id, asx_affinity_domain *out_domain) {
    asx_affinity_entry *entry;

    if (out_domain == NULL) { return ASX_E_INVALID_ARGUMENT; }

    if (entity_id == ASX_INVALID_ID) { return ASX_E_INVALID_ARGUMENT; }

    entry = affinity_find(entity_id);
    if (entry == NULL) { return ASX_E_AFFINITY_NOT_BOUND; }

    *out_domain = entry->domain;
    return ASX_OK;
}

void asx_affinity_unbind(uint64_t entity_id) {
    asx_affinity_entry *entry = affinity_find(entity_id);
    if (entry != NULL) {
        entry->occupied = 0;
        entry->entity_id = 0;
        entry->domain = ASX_AFFINITY_DOMAIN_NONE;
        if (g_affinity_count > 0) { g_affinity_count--; }
    }
}

uint32_t asx_affinity_tracked_count(void) { return g_affinity_count; }

#endif /* ASX_DEBUG_AFFINITY */
