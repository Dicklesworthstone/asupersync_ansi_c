/*
 * fnv_fold.h — FNV-1a over fixed-width little-endian fields, cheaply
 * (bd-o5zh)
 *
 * The runtime's rolling digests (trace, hindsight, event log chain,
 * telemetry) are FNV-1a over the little-endian bytes of fixed-width integer
 * fields, in a fixed order. Each is a serial chain: a step is
 * h = (h ^ byte) * P, and the next step needs h. This header folds a field
 * without formatting it into bytes and without more serial steps than the
 * chain needs, and gives the same value as the byte loop, bit for bit.
 *
 * Two facts make it cheap:
 *   - A zero byte's step is a bare multiply by P, so a run of zero bytes
 *     (the high bytes of a small sequence, a kind, an id, an observed value
 *     of 0) is one multiply by P^k.
 *   - Multiplications commute with what follows only until the next xor, so
 *     they can be owed: a fold keeps h and the number of multiplications by
 *     P it still owes, and pays them with a single multiply by P^owed just
 *     before the next nonzero byte is xored in. The multiply of a field's
 *     last byte and the multiply of the zero bytes after it, and of whole
 *     zero fields after that, are one multiply, and the owed count carries
 *     over from one event to the next. The chain costs one xor and one
 *     multiply per nonzero byte, however many zero bytes lie between them.
 *
 * The value is h * P^owed (asx_fnv_fold_value). Folding fields and reading
 * the value in any pattern gives the byte loop's digest
 * (tests/unit/runtime/test_trace.c, test_hindsight.c, test_event_log.c and
 * test_telemetry.c compare each digest to an independent byte-wise
 * reference).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_FNV_FOLD_H
#define ASX_RUNTIME_FNV_FOLD_H

#include <asx/asx_config.h>

#define ASX_FNV_PRIME UINT64_C(0x00000100000001B3)

/* The most multiplications a fold carries owed; the table below holds
 * ASX_FNV_PRIME^k mod 2^64 for k = 0 .. this. The widest field is 8 bytes,
 * and a nonzero field leaves at most 8 owed, so 32 is never reached by a
 * run of fields; the bound is kept by normalizing a longer zero run. */
#define ASX_FNV_MAX_OWED 32u

/* Defined in trace.c. */
extern const uint64_t asx_fnv_prime_pow_internal[ASX_FNV_MAX_OWED + 1u];

typedef struct {
    uint64_t h;    /* the digest, less `owed` multiplications by the prime */
    uint32_t owed; /* multiplications by the prime still to apply (<= MAX) */
} asx_fnv_fold;

static inline void asx_fnv_fold_init(asx_fnv_fold *f, uint64_t seed) {
    f->h = seed;
    f->owed = 0u;
}

/* The digest of everything folded so far. Does not change the fold. */
static inline uint64_t asx_fnv_fold_value(const asx_fnv_fold *f) {
    return f->h * asx_fnv_prime_pow_internal[f->owed];
}

/* Fold the `width` (<= 8) little-endian bytes of `v`: FNV-1a's steps over
 * byte 0, byte 1, ... byte width-1. */
static inline void asx_fnv_fold_le(asx_fnv_fold *f, uint64_t v, uint32_t width) {
    uint64_t h;
    if (v == 0u) {
        /* All zero bytes: just more multiplications owed. */
        f->owed += width;
        if (f->owed > ASX_FNV_MAX_OWED) {
            f->h *= asx_fnv_prime_pow_internal[ASX_FNV_MAX_OWED];
            f->owed -= ASX_FNV_MAX_OWED;
        }
        return;
    }
    /* Byte 0: pay what is owed, then xor it in; its own multiply is owed. */
    h = (f->h * asx_fnv_prime_pow_internal[f->owed]) ^ (v & 0xFFu);
    v >>= 8;
    width--;
    while (v != 0u) {
        h = (h * ASX_FNV_PRIME) ^ (v & 0xFFu);
        v >>= 8;
        width--;
    }
    f->h = h;
    f->owed = 1u + width; /* the last nonzero byte's multiply, and the zero bytes above it */
}

#endif /* ASX_RUNTIME_FNV_FOLD_H */
