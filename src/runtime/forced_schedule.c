/*
 * forced_schedule.c — canonical bytes and certificate of Rust's
 * ForcedSchedule (lab/runtime.rs, bd-9kll.9.3)
 *
 * The schedule artifact twin_run records from Rust (and stores in each
 * fixture's schedule.forced_schedule): encode, strict decode, shape
 * validation and the ScheduleCertificate fold. Replaying one on the lab
 * dispatcher is in lab_dispatch.c.
 *
 * ASX_CHECKPOINT_WAIVER_FILE("forced-schedule: every loop is bounded by the "
 *   "dispatch count, itself bounded by the caller's limits and storage. "
 *   "Tooling and replay setup, never on the task poll path.")
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: new codec outside the kernel; no state transition")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/lab.h>
#include <asx/security/crypto.h>
#include <string.h>

/* ------------------------------------------------------------------- */
/* Little-endian fields                                                 */
/* ------------------------------------------------------------------- */

static void fs_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void fs_put64(uint8_t *p, uint64_t v) {
    fs_put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    fs_put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t fs_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t fs_get64(const uint8_t *p) {
    return (uint64_t)fs_get32(p) | ((uint64_t)fs_get32(p + 4) << 32);
}

/* ------------------------------------------------------------------- */
/* ScheduleCertificate (runtime/scheduler/priority.rs)                  */
/*                                                                      */
/* hash' = DetHasher::for_lab over: u64 hash, u32 arena index, u32      */
/* generation, isize lane discriminant (8 bytes), u64 step. DetHasher   */
/* (util/det_hash.rs) multiplies its state by 0x517cc1b727220a95 and    */
/* xors each little-endian byte in, from the seed 0x16f11fe89b0d677c;   */
/* finish() is the murmur3 finalizer. Verified against the              */
/* certificate_hash of the recorded fixtures (test_lab_dispatch.c).     */
/* ------------------------------------------------------------------- */

#define FS_DET_SEED 0x16f11fe89b0d677cULL
#define FS_DET_MULT 0x517cc1b727220a95ULL

static uint64_t fs_det_u32(uint64_t state, uint32_t v) {
    uint32_t i;
    for (i = 0u; i < 4u; i++) {
        state *= FS_DET_MULT;
        state ^= (uint64_t)((v >> (8u * i)) & 0xFFu);
    }
    return state;
}

static uint64_t fs_det_u64(uint64_t state, uint64_t v) {
    uint32_t i;
    for (i = 0u; i < 8u; i++) {
        state *= FS_DET_MULT;
        state ^= (v >> (8u * i)) & 0xFFu;
    }
    return state;
}

uint64_t asx_forced_schedule_certificate_step(uint64_t previous, uint64_t compact_task,
                                              uint8_t lane, uint64_t step) {
    uint64_t h = FS_DET_SEED;

    h = fs_det_u64(h, previous);
    h = fs_det_u32(h, (uint32_t)(compact_task >> 32));
    h = fs_det_u32(h, (uint32_t)(compact_task & 0xFFFFFFFFu));
    h = fs_det_u64(h, (uint64_t)lane);
    h = fs_det_u64(h, step);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

uint64_t asx_forced_schedule_certificate(const asx_forced_dispatch *dispatches, uint32_t count) {
    uint64_t hash = 0u;
    uint32_t i;

    if (dispatches == NULL) return 0u;
    for (i = 0u; i < count; i++) {
        hash = asx_forced_schedule_certificate_step(hash, dispatches[i].task, dispatches[i].lane,
                                                    dispatches[i].at_step);
    }
    return hash;
}

/* ------------------------------------------------------------------- */
/* Errors and sizes                                                     */
/* ------------------------------------------------------------------- */

static asx_status fs_fail(asx_forced_error *err, asx_forced_error_kind kind, uint32_t index,
                          uint64_t expected, uint64_t actual) {
    if (err != NULL) {
        err->kind = kind;
        err->index = index;
        err->step = 0u;
        err->expected = expected;
        err->actual = actual;
    }
    return ASX_E_INVALID_ARGUMENT;
}

size_t asx_forced_schedule_encoded_size(uint32_t dispatch_count) {
    const size_t fixed = (size_t)ASX_FORCED_SCHEDULE_HEADER_BYTES +
                         (size_t)ASX_FORCED_SCHEDULE_TERMINAL_BYTES +
                         (size_t)ASX_FORCED_SCHEDULE_CHECKSUM_BYTES;

    if ((size_t)dispatch_count > (SIZE_MAX - fixed) / (size_t)ASX_FORCED_SCHEDULE_DISPATCH_BYTES) {
        return 0u;
    }
    return fixed + (size_t)dispatch_count * (size_t)ASX_FORCED_SCHEDULE_DISPATCH_BYTES;
}

/* ------------------------------------------------------------------- */
/* Shape (validate_complete_forced_schedule_shape)                      */
/* ------------------------------------------------------------------- */

asx_status asx_forced_schedule_validate(const asx_forced_schedule *schedule,
                                        asx_forced_error *err) {
    uint64_t previous_step = 0u;
    uint64_t previous_nanos = 0u;
    uint32_t i;

    if (err != NULL) memset(err, 0, sizeof(*err));
    if (schedule == NULL || (schedule->dispatch_count > 0u && schedule->dispatches == NULL)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (schedule->version != ASX_FORCED_SCHEDULE_SCHEMA_VERSION) {
        return fs_fail(err, ASX_FORCED_E_SCHEMA_VERSION, 0u, ASX_FORCED_SCHEDULE_SCHEMA_VERSION,
                       schedule->version);
    }
    if (schedule->truncated != 0u || schedule->terminal_quiescent == 0u) {
        return fs_fail(err, ASX_FORCED_E_PARTIAL_SOURCE, 0u, 0u, 0u);
    }
    for (i = 0u; i < schedule->dispatch_count; i++) {
        const asx_forced_dispatch *d = &schedule->dispatches[i];

        if (d->at_step <= previous_step) {
            return fs_fail(err, ASX_FORCED_E_STEP_ORDER, i, previous_step, d->at_step);
        }
        if (d->at_nanos < previous_nanos) {
            return fs_fail(err, ASX_FORCED_E_TIME_ORDER, i, previous_nanos, d->at_nanos);
        }
        previous_step = d->at_step;
        previous_nanos = d->at_nanos;
    }
    if (schedule->dispatch_count > 0u) {
        const asx_forced_dispatch *last = &schedule->dispatches[schedule->dispatch_count - 1u];

        if (last->at_step > schedule->terminal_steps || last->at_nanos > schedule->terminal_nanos) {
            return fs_fail(err, ASX_FORCED_E_PARTIAL_SOURCE, schedule->dispatch_count - 1u,
                           schedule->terminal_steps, last->at_step);
        }
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------- */
/* Canonical bytes                                                      */
/* ------------------------------------------------------------------- */

static const uint8_t g_magic[8] = {'A', 'S', 'U', 'P', 'F', 'S', 'C', 0u};
/* sizeof includes the terminating NUL, which Rust hashes too. */
static const char g_checksum_domain[] = "asupersync.lab.forced-schedule.artifact.v1";

static void fs_checksum(const uint8_t *bytes, size_t len, uint8_t out[ASX_SHA256_DIGEST_SIZE]) {
    asx_sha256_ctx ctx;

    asx_sha256_init(&ctx);
    asx_sha256_update(&ctx, g_checksum_domain, sizeof(g_checksum_domain));
    asx_sha256_update(&ctx, bytes, len);
    asx_sha256_final(&ctx, out);
}

asx_status asx_forced_schedule_encode(const asx_forced_schedule *schedule, uint8_t *out, size_t cap,
                                      size_t *out_len, asx_forced_error *err) {
    asx_status st;
    size_t size;
    size_t body;
    uint8_t *p;
    uint32_t i;

    if (out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_forced_schedule_validate(schedule, err);
    if (st != ASX_OK) return st;
    for (i = 0u; i < schedule->dispatch_count; i++) {
        if (schedule->dispatches[i].lane > ASX_FORCED_LANE_STOLEN) {
            return fs_fail(err, ASX_FORCED_E_LANE_TAG, i, ASX_FORCED_LANE_STOLEN,
                           schedule->dispatches[i].lane);
        }
    }
    size = asx_forced_schedule_encoded_size(schedule->dispatch_count);
    if (size == 0u) {
        return fs_fail(err, ASX_FORCED_E_DISPATCH_LIMIT, 0u, 0u, schedule->dispatch_count);
    }
    *out_len = size;
    if (out == NULL || cap < size) return ASX_E_BUFFER_TOO_SMALL;

    p = out;
    memcpy(p, g_magic, sizeof(g_magic));
    fs_put32(p + 8, ASX_FORCED_SCHEDULE_ARTIFACT_VERSION);
    fs_put32(p + 12, schedule->version);
    fs_put64(p + 16, schedule->seed);
    fs_put64(p + 24, schedule->config_hash);
    fs_put64(p + 32, (uint64_t)schedule->dispatch_count);
    p += ASX_FORCED_SCHEDULE_HEADER_BYTES;
    for (i = 0u; i < schedule->dispatch_count; i++) {
        const asx_forced_dispatch *d = &schedule->dispatches[i];

        fs_put64(p, d->task);
        fs_put32(p + 8, d->worker);
        p[12] = d->lane;
        fs_put64(p + 13, d->at_step);
        fs_put64(p + 21, d->at_nanos);
        p += ASX_FORCED_SCHEDULE_DISPATCH_BYTES;
    }
    fs_put64(p, schedule->terminal_steps);
    fs_put64(p + 8, schedule->terminal_nanos);
    fs_put64(p + 16, schedule->terminal_schedule_hash);
    p[24] = 1u; /* validated: quiescent */
    p[25] = 0u; /* validated: not truncated */
    p += ASX_FORCED_SCHEDULE_TERMINAL_BYTES;
    body = (size_t)(p - out);
    fs_checksum(out, body, p);
    return ASX_OK;
}

asx_status asx_forced_schedule_decode(const uint8_t *bytes, size_t len,
                                      const asx_forced_decode_limits *limits,
                                      asx_forced_dispatch *storage, uint32_t storage_cap,
                                      asx_forced_schedule *out, asx_forced_error *err) {
    uint64_t count64;
    uint32_t count;
    size_t expected_len;
    size_t body_len;
    uint8_t digest[ASX_SHA256_DIGEST_SIZE];
    const uint8_t *p;
    uint32_t i;

    if (err != NULL) memset(err, 0, sizeof(*err));
    if (bytes == NULL || limits == NULL || out == NULL || (storage == NULL && storage_cap > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (len > limits->max_encoded_bytes) {
        return fs_fail(err, ASX_FORCED_E_BYTE_LIMIT, 0u, (uint64_t)limits->max_encoded_bytes,
                       (uint64_t)len);
    }
    if (len < ASX_FORCED_SCHEDULE_HEADER_BYTES) {
        return fs_fail(err, ASX_FORCED_E_TRUNCATED, 0u, ASX_FORCED_SCHEDULE_HEADER_BYTES,
                       (uint64_t)len);
    }
    if (memcmp(bytes, g_magic, sizeof(g_magic)) != 0) {
        return fs_fail(err, ASX_FORCED_E_MAGIC, 0u, 0u, 0u);
    }
    if (fs_get32(bytes + 8) != ASX_FORCED_SCHEDULE_ARTIFACT_VERSION) {
        return fs_fail(err, ASX_FORCED_E_ARTIFACT_VERSION, 0u, ASX_FORCED_SCHEDULE_ARTIFACT_VERSION,
                       fs_get32(bytes + 8));
    }
    if (fs_get32(bytes + 12) != ASX_FORCED_SCHEDULE_SCHEMA_VERSION) {
        return fs_fail(err, ASX_FORCED_E_SCHEMA_VERSION, 0u, ASX_FORCED_SCHEDULE_SCHEMA_VERSION,
                       fs_get32(bytes + 12));
    }
    count64 = fs_get64(bytes + 32);
    if (count64 > (uint64_t)limits->max_dispatches || count64 > (uint64_t)storage_cap ||
        count64 > (uint64_t)UINT32_MAX) {
        return fs_fail(err, ASX_FORCED_E_DISPATCH_LIMIT, 0u,
                       (uint64_t)(limits->max_dispatches < storage_cap ? limits->max_dispatches
                                                                       : storage_cap),
                       count64);
    }
    count = (uint32_t)count64;
    expected_len = asx_forced_schedule_encoded_size(count);
    if (expected_len == 0u) return fs_fail(err, ASX_FORCED_E_DISPATCH_LIMIT, 0u, 0u, count64);
    if (len < expected_len) {
        return fs_fail(err, ASX_FORCED_E_TRUNCATED, 0u, (uint64_t)expected_len, (uint64_t)len);
    }
    if (len > expected_len) {
        return fs_fail(err, ASX_FORCED_E_TRAILING_BYTES, 0u, (uint64_t)expected_len, (uint64_t)len);
    }
    body_len = expected_len - ASX_FORCED_SCHEDULE_CHECKSUM_BYTES;
    fs_checksum(bytes, body_len, digest);
    if (memcmp(digest, bytes + body_len, ASX_SHA256_DIGEST_SIZE) != 0) {
        return fs_fail(err, ASX_FORCED_E_CHECKSUM, 0u, 0u, 0u);
    }

    p = bytes + ASX_FORCED_SCHEDULE_HEADER_BYTES;
    for (i = 0u; i < count; i++) {
        storage[i].task = fs_get64(p);
        storage[i].worker = fs_get32(p + 8);
        storage[i].lane = p[12];
        storage[i].at_step = fs_get64(p + 13);
        storage[i].at_nanos = fs_get64(p + 21);
        if (storage[i].lane > ASX_FORCED_LANE_STOLEN) {
            return fs_fail(err, ASX_FORCED_E_LANE_TAG, i, ASX_FORCED_LANE_STOLEN, storage[i].lane);
        }
        p += ASX_FORCED_SCHEDULE_DISPATCH_BYTES;
    }
    if (p[24] > 1u) return fs_fail(err, ASX_FORCED_E_BOOL_TAG, 0u, 1u, p[24]);
    if (p[25] > 1u) return fs_fail(err, ASX_FORCED_E_BOOL_TAG, 1u, 1u, p[25]);

    out->version = fs_get32(bytes + 12);
    out->seed = fs_get64(bytes + 16);
    out->config_hash = fs_get64(bytes + 24);
    out->dispatches = storage;
    out->dispatch_count = count;
    out->terminal_steps = fs_get64(p);
    out->terminal_nanos = fs_get64(p + 8);
    out->terminal_schedule_hash = fs_get64(p + 16);
    out->terminal_quiescent = p[24];
    out->truncated = p[25];
    return asx_forced_schedule_validate(out, err);
}
