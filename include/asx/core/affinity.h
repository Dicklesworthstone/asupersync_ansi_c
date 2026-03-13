/*
 * asx/core/affinity.h — thread-affinity domain guards and transfer certificates
 *
 * Replaces Rust Send/Sync guarantees with explicit affinity domain
 * tracking. Every mutable runtime entity (region, task, obligation)
 * may be bound to an affinity domain. Access from a different domain
 * without an explicit transfer triggers a deterministic error.
 *
 * Compile-time gated: when ASX_DEBUG_AFFINITY is not defined, all
 * check functions compile to zero-overhead macros that always succeed.
 * In debug/hardened builds, affinity checks run on every access path.
 *
 * Design:
 *   - Domain IDs are opaque uint32_t values (0 = any, UINT32_MAX = none)
 *   - Current domain set via asx_affinity_set_domain() at thread init
 *   - Entities bound via asx_affinity_bind() at creation
 *   - Cross-domain handoff via asx_affinity_transfer()
 *   - Violations recorded deterministically for diagnostics
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_AFFINITY_H
#define ASX_CORE_AFFINITY_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

/* Auto-enable affinity guards in debug builds unless explicitly disabled. */
#if defined(ASX_DEBUG) && ASX_DEBUG && !defined(ASX_DEBUG_AFFINITY) &&                             \
    !defined(ASX_DEBUG_AFFINITY_DISABLE)
#define ASX_DEBUG_AFFINITY
#endif

/* ------------------------------------------------------------------ */
/* Affinity domain type                                               */
/* ------------------------------------------------------------------ */

typedef uint32_t asx_affinity_domain;

/* Domain that permits access from any thread (no restriction). */
#define ASX_AFFINITY_DOMAIN_ANY ((asx_affinity_domain)0u)

/* Sentinel for entities with no domain binding. */
#define ASX_AFFINITY_DOMAIN_NONE ((asx_affinity_domain)0xFFFFFFFFu)

/* Tracking table capacity */
#define ASX_AFFINITY_TABLE_CAPACITY 256u

/* ------------------------------------------------------------------ */
/* API (real implementations when ASX_DEBUG_AFFINITY is defined)       */
/* ------------------------------------------------------------------ */

#ifdef ASX_DEBUG_AFFINITY

/* Reset all affinity state (tracking table and current domain). */
ASX_API void asx_affinity_reset(void);

/* Set the current execution domain. Call at thread/context init. */
ASX_API void asx_affinity_set_domain(asx_affinity_domain domain);

/* Query the current execution domain. */
ASX_API ASX_MUST_USE asx_affinity_domain asx_affinity_current_domain(void);

/* Bind an entity to a domain. Returns ASX_E_AFFINITY_ALREADY_BOUND
 * if entity was already bound to a different domain. */
ASX_API ASX_MUST_USE asx_status asx_affinity_bind(uint64_t entity_id, asx_affinity_domain domain);

/* Check that the current domain matches the entity's bound domain.
 * Returns ASX_OK if access is legal, ASX_E_AFFINITY_VIOLATION otherwise.
 * Entities bound to ASX_AFFINITY_DOMAIN_ANY always pass. */
ASX_API ASX_MUST_USE asx_status asx_affinity_check(uint64_t entity_id);

/* Transfer an entity's domain binding to a new domain.
 * The caller must be in the entity's current domain (or entity must
 * be domain ANY). Returns ASX_E_AFFINITY_VIOLATION if caller is in
 * wrong domain, ASX_E_AFFINITY_NOT_BOUND if entity is untracked. */
ASX_API ASX_MUST_USE asx_status asx_affinity_transfer(uint64_t entity_id,
                                                      asx_affinity_domain to_domain);

/* Query an entity's current domain binding.
 * Returns ASX_E_AFFINITY_NOT_BOUND if entity is not tracked. */
ASX_API ASX_MUST_USE asx_status asx_affinity_get_domain(uint64_t entity_id,
                                                        asx_affinity_domain *out_domain);

/* Unbind an entity from affinity tracking (e.g. on destruction). */
ASX_API void asx_affinity_unbind(uint64_t entity_id);

/* Query how many entities are currently tracked. */
ASX_API ASX_MUST_USE uint32_t asx_affinity_tracked_count(void);

#else /* ASX_DEBUG_AFFINITY not defined: zero-overhead stubs */

#define asx_affinity_reset() ((void)0)
#define asx_affinity_set_domain(d) ((void)(d))
#define asx_affinity_current_domain() ASX_AFFINITY_DOMAIN_ANY
#define asx_affinity_bind(id, d) ((void)(id), (void)(d), ASX_OK)
#define asx_affinity_check(id) ((void)(id), ASX_OK)
#define asx_affinity_transfer(id, d) ((void)(id), (void)(d), ASX_OK)
#define asx_affinity_get_domain(id, out) ((void)(id), (void)(out), ASX_OK)
#define asx_affinity_unbind(id) ((void)(id))
#define asx_affinity_tracked_count() (0u)

#endif /* ASX_DEBUG_AFFINITY */

#endif /* ASX_CORE_AFFINITY_H */
