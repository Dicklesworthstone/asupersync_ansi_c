/*
 * asx/raptorq/raptorq.h — RaptorQ erasure coding surface (fail-closed stub)
 *
 * RaptorQ (RFC 6330) fountain erasure coding is exported by the upstream
 * Rust crate but is explicitly deferred in the ANSI C port per DEF-009
 * (Wave D, ~18k LOC equivalent, specialized audience).
 *
 * This header provides:
 *   - Configuration types that document the upstream contract
 *   - A readiness probe that returns ASX_E_NOT_FOUND (fail-closed)
 *   - String constants for diagnostic/interop messages
 *
 * When the full implementation lands, these stubs will be replaced by
 * real encode/decode/recover APIs. Until then, callers get a clear,
 * deterministic rejection rather than silent omission.
 *
 * Upstream Rust parity: RaptorQConfig, raptorq module re-exports.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RAPTORQ_H
#define ASX_RAPTORQ_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * RaptorQ configuration (documents upstream contract)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t symbol_size;         /* bytes per encoding symbol */
    uint32_t source_block_count;  /* number of source blocks */
    uint32_t sub_block_count;     /* sub-blocks per source block */
    uint32_t repair_overhead_pct; /* repair symbol overhead percentage */
    uint32_t max_source_symbols;  /* max source symbols per block */
} asx_raptorq_config;

/* Initialize config with defaults per RFC 6330 recommendations. */
ASX_API void asx_raptorq_config_init(asx_raptorq_config *cfg);

/* -------------------------------------------------------------------
 * Readiness probe (fail-closed)
 * ------------------------------------------------------------------- */

/* Returns ASX_OK if RaptorQ encoding/decoding is available.
 * Callers should check this before attempting erasure operations. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_available(void);

/* Returns a human-readable reason string for the deferral.
 * Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_raptorq_deferral_reason(void);

/* -------------------------------------------------------------------
 * Stub encode/decode entry points (fail-closed)
 *
 * These exist so callers can write code against the API surface
 * today and get deterministic errors rather than link failures.
 * ------------------------------------------------------------------- */

/* Encode source data into source + repair symbols with XOR-based
 * erasure protection. Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_encode(const asx_raptorq_config *cfg,
                                                     const void *source_data,
                                                     uint32_t source_len,
                                                     void *out_symbols,
                                                     uint32_t out_capacity,
                                                     uint32_t *out_symbol_count);

/* Decode received symbols back to source data. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decode(const asx_raptorq_config *cfg,
                                                     const void *symbols,
                                                     uint32_t symbol_count,
                                                     void *out_data,
                                                     uint32_t out_capacity,
                                                     uint32_t *out_data_len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RAPTORQ_H */
