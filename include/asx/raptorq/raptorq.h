/*
 * asx/raptorq/raptorq.h — RaptorQ erasure coding surface
 *
 * Provides XOR-based erasure coding for symbol recovery. Encodes
 * source data into repair symbols that enable reconstruction when
 * some symbols are lost. Configuration controls symbol size, count,
 * and repair overhead.
 *
 * Upstream Rust parity: RaptorQConfig, encode, decode.
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
                                                   const void *source_data, uint32_t source_len,
                                                   void *out_symbols, uint32_t out_capacity,
                                                   uint32_t *out_symbol_count);

/* Decode received symbols back to source data. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decode(const asx_raptorq_config *cfg,
                                                   const void *symbols, uint32_t symbol_count,
                                                   void *out_data, uint32_t out_capacity,
                                                   uint32_t *out_data_len);

/* Decode received symbols with an explicit erasure bitmap.
 *
 * `symbol_present` is one byte per symbol: 1 if present, 0 if erased.
 * The first k entries correspond to source symbols and the remaining
 * entries correspond to repair symbols.
 *
 * The implementation performs XOR peeling over the emitted repair equations:
 *   - fast-path decode when all source symbols are present,
 *   - iterative single-unknown recovery from present repair symbols,
 *   - `ASX_E_RESOURCE_EXHAUSTED` when the erasure pattern is not solvable
 *     by the current repair set. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decode_with_erasures(
    const asx_raptorq_config *cfg, const void *symbols, uint32_t symbol_count,
    const uint8_t *symbol_present, void *out_data, uint32_t out_capacity, uint32_t *out_data_len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RAPTORQ_H */
