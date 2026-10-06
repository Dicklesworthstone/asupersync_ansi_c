/*
 * asx/raptorq/raptorq.h — RFC 6330 RaptorQ forward error correction
 *
 * Systematic RaptorQ encoder/decoder conforming to RFC 6330:
 *   - GF(256) octet arithmetic (RFC 6330 section 5.7),
 *   - systematic index table, Rand[], Deg[] and Tuple[] generators
 *     (sections 5.3.5, 5.5, 5.6),
 *   - constraint matrix (LDPC, HDPC and LT rows, section 5.3.3) and
 *     intermediate-symbol generation by inactivation decoding (section 5.4),
 *   - encoding symbol generation for any 24-bit ESI (section 5.3.4),
 *   - OTI wire format and source-block / sub-block partitioning
 *     (sections 3.3 and 4.4.1).
 *
 * Encoding symbols are bit-identical to other RFC 6330 implementations
 * (validated against golden vectors from an independent implementation in
 * tests/unit/raptorq/test_raptorq.c).
 *
 * Memory model: nothing in this module allocates. Encoders and decoders run
 * inside caller-owned workspace buffers sized by the *_workspace_size()
 * query functions. Undersized buffers fail closed with ASX_E_BUFFER_TOO_SMALL
 * before any state is modified. A workspace may come from any allocator or
 * static pool; it must not be shared between live encoders/decoders and must
 * outlive the object that uses it.
 *
 * Workspace sizes are guaranteed upper bounds: besides the L intermediate
 * symbols (and, for decoders, the stored received symbols) they reserve
 * L * ceil(L / 64) * 8 bytes of GF(2) scratch so that even a worst-case
 * inactivation set fits (about 158 KB at K = 1024 and 8.9 MB at K = 8192).
 * Typical inputs inactivate only P plus a few dozen columns.
 *
 * Determinism: outputs depend only on inputs (no clocks, no global mutable
 * state, no platform-dependent arithmetic). Wire formats are explicit
 * big-endian (network order) per RFC 6330 section 3.
 *
 * Upstream Rust parity: src/raptorq/{rfc6330,gf256,systematic,decoder}.rs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RAPTORQ_H
#define ASX_RAPTORQ_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Limits (RFC 6330)
 * ------------------------------------------------------------------- */

/* Maximum number of source symbols in one source block (K'max, Table 2). */
#define ASX_RAPTORQ_MAX_SOURCE_SYMBOLS 56403u

/* Largest encoding symbol ID (ESI is a 24-bit field, section 3.2). */
#define ASX_RAPTORQ_MAX_ESI 16777215u

/* Maximum number of source blocks Z (8-bit OTI field, section 3.3.3). */
#define ASX_RAPTORQ_MAX_SOURCE_BLOCKS 255u

/* Largest symbol size T in bytes (16-bit OTI field, section 3.3.2). */
#define ASX_RAPTORQ_MAX_SYMBOL_SIZE 65535u

/* Largest transfer length F in bytes (section 4.4.1.2, errata 5548). */
#define ASX_RAPTORQ_MAX_TRANSFER_LENGTH UINT64_C(942574504275)

/* Size of the encoded OTI: 8-byte common + 4-byte scheme-specific part. */
#define ASX_RAPTORQ_OTI_SIZE 12u

/* -------------------------------------------------------------------
 * Low-level RFC 6330 primitives
 * ------------------------------------------------------------------- */

/* Multiply two GF(256) octets (RFC 6330 section 5.7.2, polynomial 0x11D).
 * Pure function; thread-safe. */
ASX_API ASX_MUST_USE uint8_t asx_raptorq_gf256_mul(uint8_t a, uint8_t b);

/* Divide GF(256) octet `a` by `b` (RFC 6330 section 5.7.2). Division by
 * zero is undefined in the field; this function returns 0 when b == 0 so
 * callers fail deterministically. Pure function; thread-safe. */
ASX_API ASX_MUST_USE uint8_t asx_raptorq_gf256_div(uint8_t a, uint8_t b);

/* Return alpha^^i, where alpha = 2 is the GF(256) generator (OCT_EXP of
 * RFC 6330 section 5.7.3, exponent taken modulo 255). Pure; thread-safe. */
ASX_API ASX_MUST_USE uint8_t asx_raptorq_gf256_exp(uint32_t i);

/* RFC 6330 section 5.3.5.1 pseudo-random generator Rand[y, i, m].
 * Returns a value in [0, m); returns 0 when m == 0 (fail closed).
 * Pure function; thread-safe. */
ASX_API ASX_MUST_USE uint32_t asx_raptorq_rand(uint32_t y, uint32_t i, uint32_t m);

/* RFC 6330 section 5.3.5.2 degree generator Deg[v] for a block with W
 * LT symbols: the table degree for v (v < 2^20) capped at W - 2.
 * Returns 0 when v >= 2^20 or w < 3 (invalid inputs). Pure; thread-safe. */
ASX_API ASX_MUST_USE uint32_t asx_raptorq_deg(uint32_t v, uint32_t w);

/* Derived RFC 6330 parameters for a source block of K source symbols
 * (section 5.3.3.3 and Table 2). */
typedef struct asx_raptorq_params {
    uint32_t k;       /* K: source symbols in the block */
    uint32_t k_prime; /* K': extended source block size (Table 2) */
    uint32_t j;       /* J(K'): systematic index */
    uint32_t s;       /* S(K'): LDPC symbols */
    uint32_t h;       /* H(K'): HDPC symbols */
    uint32_t w;       /* W(K'): LT symbols */
    uint32_t l;       /* L = K' + S + H intermediate symbols */
    uint32_t p;       /* P = L - W permanently inactive (PI) symbols */
    uint32_t p1;      /* P1: smallest prime >= P */
    uint32_t u;       /* U = P - H */
    uint32_t b;       /* B = W - S */
} asx_raptorq_params;

/* Derive the RFC 6330 parameters for a source block of `k` symbols.
 * Preconditions: `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT when out is NULL or k is
 * outside [1, ASX_RAPTORQ_MAX_SOURCE_SYMBOLS] (out is then zeroed when
 * non-NULL). Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_params_init(uint32_t k, asx_raptorq_params *out);

/* LT/PI tuple (d, a, b, d1, a1, b1) of RFC 6330 section 5.3.5.4. */
typedef struct asx_raptorq_tuple {
    uint32_t d;  /* LT degree */
    uint32_t a;  /* LT step, 1 <= a < W */
    uint32_t b;  /* LT start, b < W */
    uint32_t d1; /* PI degree (2 or 3) */
    uint32_t a1; /* PI step, 1 <= a1 < P1 */
    uint32_t b1; /* PI start, b1 < P1 */
} asx_raptorq_tuple;

/* Compute Tuple[K', X] for internal symbol ID `isi` (RFC 6330 5.3.5.4).
 * Preconditions: `params` must be valid (from asx_raptorq_params_init) and
 * `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT on NULL/invalid parameters.
 * Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_tuple_compute(const asx_raptorq_params *params,
                                                          uint32_t isi, asx_raptorq_tuple *out);

/* -------------------------------------------------------------------
 * Source-block encoder
 * ------------------------------------------------------------------- */

/* Source-block encoder. Fields are private: initialize with
 * asx_raptorq_encoder_init() and treat the struct as opaque. */
typedef struct asx_raptorq_encoder {
    asx_raptorq_params params;
    uint32_t symbol_size;
    uint32_t ready;
    uint8_t *intermediate; /* L * symbol_size bytes inside the workspace */
} asx_raptorq_encoder;

/* Query the workspace bytes needed to encode a block of `k` source symbols
 * of `symbol_size` bytes. The workspace holds the L intermediate symbols
 * plus solver scratch.
 * Preconditions: `out_size` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL out_size, k outside
 * [1, ASX_RAPTORQ_MAX_SOURCE_SYMBOLS] or symbol_size outside
 * [1, ASX_RAPTORQ_MAX_SYMBOL_SIZE]; ASX_E_RESOURCE_EXHAUSTED if the size is
 * not representable in size_t on this target. Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_encoder_workspace_size(uint32_t k, uint32_t symbol_size,
                                                                   size_t *out_size);

/* Initialize an encoder for one source block and compute its intermediate
 * symbols (RFC 6330 section 5.3.3.4).
 *
 * `source` holds the block's source symbols back to back (symbol i at
 * offset i * symbol_size); `source_len` may be smaller than
 * k * symbol_size, in which case the missing trailing bytes are zero
 * (RFC 6330 padding of the final symbol).
 *
 * Preconditions: `enc`, `source` and `workspace` must not be NULL;
 * 1 <= source_len <= k * symbol_size. The workspace must stay valid and
 * unshared for the encoder's lifetime.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT on NULL pointers or out-of-range
 * k / symbol_size / source_len; ASX_E_BUFFER_TOO_SMALL when
 * workspace_size is below asx_raptorq_encoder_workspace_size(). On error
 * `enc` is left unready. Not thread-safe for a given encoder/workspace;
 * distinct encoders may be used from different threads. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_encoder_init(asx_raptorq_encoder *enc, uint32_t k,
                                                         uint32_t symbol_size,
                                                         const uint8_t *source, size_t source_len,
                                                         void *workspace, size_t workspace_size);

/* Generate the encoding symbol with encoding symbol ID `esi` (RFC 6330
 * section 5.3.4). ESIs 0..K-1 reproduce the source symbols (systematic);
 * ESI X >= K is a repair symbol with internal symbol ID X + K' - K.
 * Writes exactly symbol_size bytes to `out`.
 * Preconditions: `enc` must be initialized; `out` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL pointers or
 * esi > ASX_RAPTORQ_MAX_ESI; ASX_E_INVALID_STATE for an unready encoder;
 * ASX_E_BUFFER_TOO_SMALL when out_len < symbol_size.
 * Thread-safe for concurrent readers of one initialized encoder. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_encoder_symbol(const asx_raptorq_encoder *enc,
                                                           uint32_t esi, uint8_t *out,
                                                           size_t out_len);

/* -------------------------------------------------------------------
 * Source-block decoder
 * ------------------------------------------------------------------- */

/* Source-block decoder. Fields are private: initialize with
 * asx_raptorq_decoder_init() and treat the struct as opaque. */
typedef struct asx_raptorq_decoder {
    asx_raptorq_params params;
    uint32_t symbol_size;
    uint32_t capacity;     /* maximum stored encoding symbols */
    uint32_t count;        /* encoding symbols stored so far */
    uint32_t source_count; /* stored symbols with ESI < K */
    uint32_t hash_mask;
    uint32_t ready;
    uint32_t *esi;    /* [capacity] received ESIs */
    uint32_t *hash;   /* [hash_mask + 1] open-addressing ESI index */
    uint8_t *symbols; /* [capacity * symbol_size] received symbol data */
    uint8_t *solver;  /* solver scratch inside the workspace */
    size_t solver_size;
} asx_raptorq_decoder;

/* Query the workspace bytes needed to decode a block of `k` source symbols
 * of `symbol_size` bytes from up to `max_symbols` received encoding symbols
 * (max_symbols >= k). The returned size is a guaranteed upper bound for any
 * received set, including worst-case inactivation.
 * Preconditions: `out_size` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL out_size, out-of-range
 * k / symbol_size, or max_symbols outside [k, ASX_RAPTORQ_MAX_ESI + 1];
 * ASX_E_RESOURCE_EXHAUSTED if the size is not representable in size_t.
 * Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decoder_workspace_size(uint32_t k, uint32_t symbol_size,
                                                                   uint32_t max_symbols,
                                                                   size_t *out_size);

/* Initialize a decoder for one source block of `k` symbols that can store
 * up to `max_symbols` received encoding symbols.
 * Preconditions: `dec` and `workspace` must not be NULL. The workspace must
 * stay valid and unshared for the decoder's lifetime.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT on NULL pointers or out-of-range
 * parameters (see asx_raptorq_decoder_workspace_size); ASX_E_BUFFER_TOO_SMALL
 * when workspace_size is below the queried size. On error `dec` is left
 * unready. Not thread-safe for a given decoder/workspace. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decoder_init(asx_raptorq_decoder *dec, uint32_t k,
                                                         uint32_t symbol_size, uint32_t max_symbols,
                                                         void *workspace, size_t workspace_size);

/* Store one received encoding symbol (any ESI, source or repair, in any
 * order). The data is copied into the decoder workspace.
 * Preconditions: `dec` must be initialized; `symbol` must not be NULL and
 * hold exactly symbol_size bytes.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL pointers, esi >
 * ASX_RAPTORQ_MAX_ESI or symbol_len != symbol_size; ASX_E_INVALID_STATE for
 * an unready decoder; ASX_E_DUPLICATE_SYMBOL when the ESI was already
 * stored; ASX_E_BUFFER_TOO_SMALL when max_symbols symbols are already
 * stored. The decoder is unchanged on error. Not thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decoder_add_symbol(asx_raptorq_decoder *dec,
                                                               uint32_t esi, const uint8_t *symbol,
                                                               size_t symbol_len);

/* Attempt to recover the K source symbols from the stored encoding symbols
 * and write them back to back into `out` (K * symbol_size bytes).
 *
 * Returns ASX_OK on success. Returns ASX_E_RESOURCE_EXHAUSTED when the
 * stored set is insufficient (fewer than K symbols, or rank-deficient);
 * the stored symbols are kept, so the caller may add more symbols and call
 * again. Returns ASX_E_INVALID_ARGUMENT for NULL pointers,
 * ASX_E_INVALID_STATE for an unready decoder and ASX_E_BUFFER_TOO_SMALL when
 * out_len < K * symbol_size. `out` is only written on success.
 * Preconditions: `dec` must be initialized; `out` must not be NULL.
 * Not thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_decoder_decode(asx_raptorq_decoder *dec, uint8_t *out,
                                                           size_t out_len);

/* -------------------------------------------------------------------
 * Object-level helpers (RFC 6330 sections 3.3 and 4.4.1)
 * ------------------------------------------------------------------- */

/* Object Transmission Information. */
typedef struct asx_raptorq_oti {
    uint64_t transfer_length; /* F: object size in bytes (40-bit field) */
    uint16_t symbol_size;     /* T: bytes per symbol */
    uint16_t sub_blocks;      /* N: sub-blocks per source block */
    uint8_t source_blocks;    /* Z: number of source blocks */
    uint8_t alignment;        /* Al: symbol alignment in bytes */
} asx_raptorq_oti;

/* Result of RFC 6330 Partition[I, J] (section 4.4.1.2). */
typedef struct asx_raptorq_partition {
    uint32_t large_size;  /* IL = ceil(I / J) */
    uint32_t small_size;  /* IS = floor(I / J) */
    uint32_t large_count; /* JL = I - IS * J */
    uint32_t small_count; /* JS = J - JL */
} asx_raptorq_partition;

/* Compute Partition[I, J]: split I items into JL parts of IL items and JS
 * parts of IS items.
 * Preconditions: `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT when out is NULL or j == 0.
 * Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_partition_compute(uint32_t i, uint32_t j,
                                                              asx_raptorq_partition *out);

/* Validate an OTI: 1 <= F <= ASX_RAPTORQ_MAX_TRANSFER_LENGTH, T >= 1,
 * Al >= 1 with T % Al == 0, 1 <= N <= T / Al, 1 <= Z <= ceil(F / T) and
 * ceil(ceil(F / T) / Z) <= ASX_RAPTORQ_MAX_SOURCE_SYMBOLS.
 * Preconditions: `oti` must not be NULL.
 * Returns ASX_OK or ASX_E_INVALID_ARGUMENT. Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_oti_validate(const asx_raptorq_oti *oti);

/* Derive an OTI for an object of `transfer_length` bytes with symbol size
 * `symbol_size`, alignment `alignment` and N = 1 sub-block, choosing the
 * smallest Z such that no source block exceeds `max_block_symbols`
 * (1..ASX_RAPTORQ_MAX_SOURCE_SYMBOLS) symbols.
 * Preconditions: `oti` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for invalid inputs or when more
 * than ASX_RAPTORQ_MAX_SOURCE_BLOCKS blocks would be needed. Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_oti_init(asx_raptorq_oti *oti, uint64_t transfer_length,
                                                     uint16_t symbol_size, uint8_t alignment,
                                                     uint32_t max_block_symbols);

/* Encode a validated OTI into its 12-byte big-endian wire form: F (40 bits),
 * reserved zero octet, T (16 bits), Z (8 bits), N (16 bits), Al (8 bits).
 * Preconditions: `oti` and `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULL pointers or an
 * invalid OTI (out untouched). Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_oti_serialize(const asx_raptorq_oti *oti,
                                                          uint8_t out[ASX_RAPTORQ_OTI_SIZE]);

/* Decode and validate a 12-byte OTI. The reserved octet must be zero.
 * Preconditions: `in` and `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULL pointers, a non-zero
 * reserved octet or an invalid OTI (out untouched). Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_oti_deserialize(const uint8_t in[ASX_RAPTORQ_OTI_SIZE],
                                                            asx_raptorq_oti *out);

/* Placement of one source block inside the object. */
typedef struct asx_raptorq_block_layout {
    uint32_t source_symbols; /* K for this block */
    uint32_t symbol_size;    /* T */
    uint64_t offset;         /* first object byte covered by the block */
    uint64_t length;         /* object bytes covered (<= K * T; rest is padding) */
} asx_raptorq_block_layout;

/* Compute the layout of source block `sbn` (0 <= sbn < Z) of an object.
 * Preconditions: `oti` and `out` must not be NULL.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULL pointers, an invalid
 * OTI or sbn >= Z. Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_oti_block_layout(const asx_raptorq_oti *oti,
                                                             uint32_t sbn,
                                                             asx_raptorq_block_layout *out);

/* Gather the K source symbols of block `sbn` from `object` (F bytes) into
 * `block_out` (K * T bytes, symbol i at offset i * T), applying RFC 6330
 * sub-block interleaving (N > 1) and zero padding past the end of the
 * object.
 * Preconditions: `oti`, `object` and `block_out` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL pointers, an invalid OTI
 * or sbn >= Z; ASX_E_BUFFER_TOO_SMALL when object_len < F or
 * block_out_len < K * T. Thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_block_gather(const asx_raptorq_oti *oti, uint32_t sbn,
                                                         const uint8_t *object, size_t object_len,
                                                         uint8_t *block_out, size_t block_out_len);

/* Inverse of asx_raptorq_block_gather(): scatter the K decoded source
 * symbols of block `sbn` (K * T bytes) back into `object_out` (F bytes),
 * dropping padding.
 * Preconditions: `oti`, `block` and `object_out` must not be NULL.
 * Returns ASX_OK; ASX_E_INVALID_ARGUMENT for NULL pointers, an invalid OTI
 * or sbn >= Z; ASX_E_BUFFER_TOO_SMALL when block_len < K * T or
 * object_len < F. Thread-safe for disjoint blocks. */
ASX_API ASX_MUST_USE asx_status asx_raptorq_block_scatter(const asx_raptorq_oti *oti, uint32_t sbn,
                                                          const uint8_t *block, size_t block_len,
                                                          uint8_t *object_out, size_t object_len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RAPTORQ_H */
