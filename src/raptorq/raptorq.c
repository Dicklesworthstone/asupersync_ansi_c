/*
 * raptorq.c — RaptorQ erasure coding (fail-closed stub)
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/raptorq/raptorq.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Config defaults (per RFC 6330 recommendations)                      */
/* ------------------------------------------------------------------ */

void asx_raptorq_config_init(asx_raptorq_config *cfg) {
    if (cfg == NULL) return;
    cfg->symbol_size = 1024;
    cfg->source_block_count = 1;
    cfg->sub_block_count = 1;
    cfg->repair_overhead_pct = 20;
    cfg->max_source_symbols = 56403; /* K'_max per RFC 6330 */
}

/* ------------------------------------------------------------------ */
/* Readiness probe                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_raptorq_available(void) {
    return ASX_E_NOT_FOUND; /* Deferred per DEF-009 (Wave D) */
}

const char *asx_raptorq_deferral_reason(void) {
    return "RaptorQ erasure coding deferred per DEF-009 (Wave D): "
           "specialized surface (~18k LOC), not required for kernel parity. "
           "Unblock criteria: channel semantics stable, binary codec proven, "
           "demand from specific vertical.";
}

/* ------------------------------------------------------------------ */
/* Stub encode/decode (fail-closed)                                    */
/* ------------------------------------------------------------------ */

asx_status asx_raptorq_encode(const asx_raptorq_config *cfg, const void *source_data,
                               uint32_t source_len, void *out_symbols, uint32_t out_capacity,
                               uint32_t *out_symbol_count) {
    (void)cfg;
    (void)source_data;
    (void)source_len;
    (void)out_symbols;
    (void)out_capacity;
    if (out_symbol_count != NULL) *out_symbol_count = 0;
    return ASX_E_NOT_FOUND;
}

asx_status asx_raptorq_decode(const asx_raptorq_config *cfg, const void *symbols,
                               uint32_t symbol_count, void *out_data, uint32_t out_capacity,
                               uint32_t *out_data_len) {
    (void)cfg;
    (void)symbols;
    (void)symbol_count;
    (void)out_data;
    (void)out_capacity;
    if (out_data_len != NULL) *out_data_len = 0;
    return ASX_E_NOT_FOUND;
}
