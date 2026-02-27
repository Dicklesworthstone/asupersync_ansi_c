/*
 * asx_ids.h — generation-tagged handle types and packing helpers
 *
 * All externally visible runtime entities use opaque generation-safe handles.
 * Handle format: [16-bit type_tag | 16-bit state_mask | 32-bit arena_index]
 *
 * Handle validation requires:
 *   - type tag match
 *   - slot index bounds
 *   - generation match (stored in arena slot, not in handle)
 *   - liveness/state legality
 *
 * State masks enable O(1) admission gating: handle.state_mask & expected_mask.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_IDS_H
#define ASX_IDS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Opaque handle types                                                */
/* ------------------------------------------------------------------ */

/*
 * Internal structure: [type_tag:16 | state_mask:16 | arena_index:32].
 * Users must not inspect or construct these directly.
 * Use asx_handle_* helpers for all operations.
 */
typedef uint64_t asx_region_id;
typedef uint64_t asx_task_id;
typedef uint64_t asx_obligation_id;
typedef uint64_t asx_timer_id;
typedef uint64_t asx_channel_id;

/* Sentinel value for invalid/uninitialized handles */
#define ASX_INVALID_ID ((uint64_t)0)

/* ------------------------------------------------------------------ */
/* Type tags (high 16 bits of handle)                                 */
/* Per Appendix B of LIFECYCLE_TRANSITION_TABLES.md                   */
/* ------------------------------------------------------------------ */

#define ASX_TYPE_REGION          ((uint16_t)0x0001)
#define ASX_TYPE_TASK            ((uint16_t)0x0002)
#define ASX_TYPE_OBLIGATION      ((uint16_t)0x0003)
#define ASX_TYPE_CANCEL_WITNESS  ((uint16_t)0x0004)
#define ASX_TYPE_TIMER           ((uint16_t)0x0005)
#define ASX_TYPE_CHANNEL         ((uint16_t)0x0006)

/* ------------------------------------------------------------------ */
/* Handle packing/unpacking helpers                                   */
/* ------------------------------------------------------------------ */

static inline uint64_t asx_handle_pack(uint16_t type_tag,
                                       uint16_t state_mask,
                                       uint32_t index)
{
    return ((uint64_t)type_tag << 48)
         | ((uint64_t)state_mask << 32)
         | (uint64_t)index;
}

static inline uint16_t asx_handle_type_tag(uint64_t h)
{
    return (uint16_t)(h >> 48);
}

static inline uint16_t asx_handle_state_mask(uint64_t h)
{
    return (uint16_t)((h >> 32) & 0xFFFF);
}

static inline uint32_t asx_handle_index(uint64_t h)
{
    return (uint32_t)(h & 0xFFFFFFFF);
}

static inline int asx_handle_is_valid(uint64_t h)
{
    return h != ASX_INVALID_ID;
}

/* O(1) state admission check: returns nonzero if current state is allowed */
static inline int asx_handle_state_allowed(uint64_t h, uint16_t allowed_mask)
{
    return (asx_handle_state_mask(h) & allowed_mask) != 0;
}

#endif /* ASX_IDS_H */
