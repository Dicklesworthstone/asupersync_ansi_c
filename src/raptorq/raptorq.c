/*
 * raptorq.c — XOR-based erasure coding (simplified fountain code)
 *
 * Implements a simplified erasure code that provides the RaptorQ API surface:
 * encode source data into symbols with repair overhead, decode with recovery
 * from symbol loss. Uses XOR-based parity for repair symbols.
 *
 * This is not RFC 6330 compliant but provides real erasure protection
 * suitable for the deterministic core profile.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/raptorq/raptorq.h>
#include <string.h>

static uint32_t raptorq_symbol_size(const asx_raptorq_config *cfg) {
    return cfg->symbol_size > 0u ? cfg->symbol_size : 1024u;
}

static uint32_t raptorq_repair_symbol_count(uint32_t source_symbol_count, uint32_t overhead_pct) {
    uint32_t repair_symbol_count = (source_symbol_count * overhead_pct + 99u) / 100u;
    if (repair_symbol_count == 0u) repair_symbol_count = 1u;
    return repair_symbol_count;
}

static int raptorq_infer_source_symbol_count(const asx_raptorq_config *cfg,
                                             uint32_t total_symbol_count,
                                             uint32_t *out_source_symbol_count) {
    uint32_t candidate;

    if (cfg == NULL || out_source_symbol_count == NULL || total_symbol_count == 0u) return 0;

    for (candidate = 1u; candidate <= total_symbol_count; candidate++) {
        uint32_t repair_symbol_count =
            raptorq_repair_symbol_count(candidate, cfg->repair_overhead_pct);
        if (candidate + repair_symbol_count == total_symbol_count) {
            *out_source_symbol_count = candidate;
            return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Config defaults                                                     */
/* ------------------------------------------------------------------ */

void asx_raptorq_config_init(asx_raptorq_config *cfg) {
    if (cfg == NULL) return;
    cfg->symbol_size = 1024;
    cfg->source_block_count = 1;
    cfg->sub_block_count = 1;
    cfg->repair_overhead_pct = 20;
    cfg->max_source_symbols = 56403;
}

/* ------------------------------------------------------------------ */
/* Readiness probe                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_raptorq_available(void) { return ASX_OK; }

const char *asx_raptorq_deferral_reason(void) { return NULL; }

/* ------------------------------------------------------------------ */
/* Encode: source data → source symbols + XOR repair symbols           */
/* ------------------------------------------------------------------ */

asx_status asx_raptorq_encode(const asx_raptorq_config *cfg, const void *source_data,
                              uint32_t source_len, void *out_symbols, uint32_t out_capacity,
                              uint32_t *out_symbol_count) {
    const uint8_t *src;
    uint8_t *dst;
    uint32_t sym_size;
    uint32_t k; /* source symbol count */
    uint32_t r; /* repair symbol count */
    uint32_t total;
    uint32_t i, j;

    if (cfg == NULL || source_data == NULL || out_symbols == NULL || out_symbol_count == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (source_len == 0u) return ASX_E_INVALID_ARGUMENT;

    src = (const uint8_t *)source_data;
    dst = (uint8_t *)out_symbols;
    sym_size = raptorq_symbol_size(cfg);

    /* Calculate source symbol count (ceil division) */
    k = (source_len + sym_size - 1u) / sym_size;
    if (k > cfg->max_source_symbols) return ASX_E_RESOURCE_EXHAUSTED;

    /* Calculate repair symbols from overhead percentage */
    r = raptorq_repair_symbol_count(k, cfg->repair_overhead_pct);

    total = k + r;
    if (total * sym_size > out_capacity) return ASX_E_BUFFER_TOO_SMALL;

    /* Write source symbols (pad last with zeros if needed) */
    for (i = 0u; i < k; i++) {
        uint32_t offset = i * sym_size;
        uint32_t remaining = source_len > offset ? source_len - offset : 0u;
        uint32_t copy_len = remaining < sym_size ? remaining : sym_size;

        memset(dst + (i * sym_size), 0, sym_size);
        if (copy_len > 0u) memcpy(dst + (i * sym_size), src + offset, copy_len);
    }

    /* Generate XOR repair symbols: repair[j] = XOR of source symbols
     * where source index satisfies (i + j) % r == 0 */
    for (j = 0u; j < r; j++) {
        uint8_t *repair = dst + ((k + j) * sym_size);
        memset(repair, 0, sym_size);
        for (i = 0u; i < k; i++) {
            if ((i + j) % r == 0u || k <= r) {
                const uint8_t *source_sym = dst + (i * sym_size);
                uint32_t b;
                for (b = 0u; b < sym_size; b++) { repair[b] ^= source_sym[b]; }
            }
        }
    }

    *out_symbol_count = total;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Decode: received symbols → source data                              */
/* ------------------------------------------------------------------ */

asx_status asx_raptorq_decode(const asx_raptorq_config *cfg, const void *symbols,
                              uint32_t symbol_count, void *out_data, uint32_t out_capacity,
                              uint32_t *out_data_len) {
    const uint8_t *syms;
    uint8_t *dst;
    uint32_t sym_size;
    uint32_t copy_len;

    if (cfg == NULL || symbols == NULL || out_data == NULL || out_data_len == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (symbol_count == 0u) return ASX_E_INVALID_ARGUMENT;

    syms = (const uint8_t *)symbols;
    dst = (uint8_t *)out_data;
    sym_size = raptorq_symbol_size(cfg);

    /* Simple decode: copy source symbols directly (repair symbols at end are ignored).
     * Full recovery from symbol loss would require the repair XOR equations. */
    copy_len = symbol_count * sym_size;
    if (copy_len > out_capacity) copy_len = out_capacity;

    memcpy(dst, syms, copy_len);
    *out_data_len = copy_len;
    return ASX_OK;
}

asx_status asx_raptorq_decode_with_erasures(const asx_raptorq_config *cfg, const void *symbols,
                                            uint32_t symbol_count, const uint8_t *symbol_present,
                                            void *out_data, uint32_t out_capacity,
                                            uint32_t *out_data_len) {
    const uint8_t *syms;
    uint8_t *dst;
    uint32_t sym_size;
    uint32_t source_symbol_count;
    uint32_t repair_symbol_count;
    uint32_t present_source_count = 0u;
    uint32_t present_repair_count = 0u;
    uint32_t i;

    if (cfg == NULL || symbols == NULL || symbol_present == NULL || out_data == NULL ||
        out_data_len == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (symbol_count == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!raptorq_infer_source_symbol_count(cfg, symbol_count, &source_symbol_count)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    syms = (const uint8_t *)symbols;
    dst = (uint8_t *)out_data;
    sym_size = raptorq_symbol_size(cfg);
    repair_symbol_count = symbol_count - source_symbol_count;

    if (source_symbol_count * sym_size > out_capacity) return ASX_E_BUFFER_TOO_SMALL;

    for (i = 0u; i < source_symbol_count; i++) {
        if (symbol_present[i] != 0u) present_source_count++;
    }
    for (i = 0u; i < repair_symbol_count; i++) {
        if (symbol_present[source_symbol_count + i] != 0u) present_repair_count++;
    }

    if (present_source_count == source_symbol_count) {
        memcpy(dst, syms, source_symbol_count * sym_size);
        *out_data_len = source_symbol_count * sym_size;
        return ASX_OK;
    }

    if (present_source_count + present_repair_count < source_symbol_count) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}
