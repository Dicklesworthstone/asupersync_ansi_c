/*
 * asx/runtime/hft_instrument.h — HFT profile instrumentation (bd-j4m.3)
 *
 * Provides latency histogram, jitter tracker, deterministic overload
 * policy, and metric gate hooks for the HFT profile. All instrumentation
 * is operational (affects observability, not semantics). The overload
 * policy produces deterministic outcomes given identical input state,
 * making overload behavior replayable and parity-safe.
 *
 * Key invariants:
 *   - Overload decisions are pure functions of (load, capacity, policy)
 *   - Histogram bins use fixed log2 boundaries — no floating point
 *   - Jitter is computed as mean absolute deviation (MAD)
 *   - Gate hooks evaluate pass/fail against configurable thresholds
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_HFT_INSTRUMENT_H
#define ASX_RUNTIME_HFT_INSTRUMENT_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Latency histogram — fixed-boundary log2 bins
 *
 * 16 bins covering [0, 2^16) nanoseconds in powers of two:
 *   bin 0:  [0,    1) ns    (sub-nanosecond)
 *   bin 1:  [1,    2) ns
 *   bin 2:  [2,    4) ns
 *   bin 3:  [4,    8) ns
 *   ...
 *   bin 15: [16384, 32768) ns
 *   overflow: >= 32768 ns
 *
 * No floating point: bin index = floor(log2(sample + 1))
 * ------------------------------------------------------------------- */

#define ASX_HFT_HISTOGRAM_BINS     16u
#define ASX_HFT_HISTOGRAM_MAX_NS   32768u  /* 2^15 */

typedef struct {
    uint32_t bins[ASX_HFT_HISTOGRAM_BINS];
    uint32_t overflow;     /* samples >= ASX_HFT_HISTOGRAM_MAX_NS */
    uint32_t total;        /* total samples recorded */
    uint64_t sum_ns;       /* running sum for mean calculation */
    uint64_t min_ns;       /* minimum sample observed */
    uint64_t max_ns;       /* maximum sample observed */
} asx_hft_histogram;

/* Initialize a histogram (zero all bins). */
ASX_API void asx_hft_histogram_init(asx_hft_histogram *h);

/* Record a latency sample in nanoseconds. */
ASX_API void asx_hft_histogram_record(asx_hft_histogram *h, uint64_t ns);

/* Read the approximate percentile value from the histogram.
 * pct is in [0, 100]. Returns the lower bound of the bin that
 * contains the percentile. */
ASX_API uint64_t asx_hft_histogram_percentile(const asx_hft_histogram *h,
                                               uint32_t pct);

/* Compute mean latency in nanoseconds. Returns 0 if no samples. */
ASX_API uint64_t asx_hft_histogram_mean(const asx_hft_histogram *h);

/* Reset histogram to initial state. */
ASX_API void asx_hft_histogram_reset(asx_hft_histogram *h);

/* -------------------------------------------------------------------
 * Jitter tracker — streaming mean absolute deviation
 *
 * Tracks jitter as the mean absolute deviation (MAD) of latency
 * samples around the running mean. Updated incrementally without
 * storing all samples.
 *
 * Two-pass streaming approximation:
 *   Pass 1: accumulate sum, count (via histogram)
 *   Pass 2: compute MAD from histogram bin midpoints
 *
 * For exact jitter from raw samples, use the sorted-samples
 * approach in bench_runtime.c. This tracker is for real-time
 * monitoring with bounded memory.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_hft_histogram  hist;            /* underlying histogram */
    uint64_t           mad_ns;          /* last computed MAD */
    uint32_t           recompute_every; /* recompute MAD every N samples */
    uint32_t           samples_since;   /* samples since last recompute */
} asx_hft_jitter_tracker;

/* Initialize a jitter tracker. recompute_interval controls how often
 * MAD is recalculated (0 = every sample, expensive). */
ASX_API void asx_hft_jitter_init(asx_hft_jitter_tracker *jt,
                                  uint32_t recompute_interval);

/* Record a latency sample and possibly recompute jitter. */
ASX_API void asx_hft_jitter_record(asx_hft_jitter_tracker *jt, uint64_t ns);

/* Force recompute of MAD from current histogram state. */
ASX_API void asx_hft_jitter_recompute(asx_hft_jitter_tracker *jt);

/* Read the current jitter (MAD) in nanoseconds. */
ASX_API uint64_t asx_hft_jitter_get(const asx_hft_jitter_tracker *jt);

/* Reset the jitter tracker. */
ASX_API void asx_hft_jitter_reset(asx_hft_jitter_tracker *jt);

/* -------------------------------------------------------------------
 * Overload policy — deterministic admission under pressure
 *
 * When task spawn rate exceeds capacity, the overload policy
 * determines behavior deterministically. Three modes:
 *
 *   REJECT     — excess spawns fail with ASX_E_ADMISSION_CLOSED
 *   SHED_OLDEST— cancel the oldest non-terminal task to make room
 *   BACKPRESSURE— block (return WOULD_BLOCK) until capacity frees
 *
 * The policy is a pure function of (mode, load, capacity), making
 * overload outcomes deterministic and replayable.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_OVERLOAD_REJECT       = 0,  /* hard reject excess work */
    ASX_OVERLOAD_SHED_OLDEST  = 1,  /* evict oldest task */
    ASX_OVERLOAD_BACKPRESSURE = 2   /* caller retries */
} asx_overload_mode;

typedef struct {
    asx_overload_mode mode;
    uint32_t          threshold_pct;  /* load% that triggers overload (0-100) */
    uint32_t          shed_max;       /* max tasks to shed per decision (SHED mode) */
} asx_overload_policy;

/* Overload decision result */
typedef struct {
    int               triggered;     /* 1 if overload was detected */
    asx_overload_mode mode;          /* policy that was applied */
    uint32_t          load_pct;      /* current load percentage */
    uint32_t          shed_count;    /* tasks shed (SHED mode only) */
    asx_status        admit_status;  /* ASX_OK or rejection status */
} asx_overload_decision;

/* Initialize a default overload policy (REJECT at 90%). */
ASX_API void asx_overload_policy_init(asx_overload_policy *pol);

/* Evaluate the overload policy given current load state.
 * used: current task count, capacity: max tasks.
 * Fills *decision with the deterministic outcome. */
ASX_API void asx_overload_evaluate(const asx_overload_policy *pol,
                                    uint32_t used,
                                    uint32_t capacity,
                                    asx_overload_decision *decision);

/* Return human-readable name for an overload mode. */
ASX_API const char *asx_overload_mode_str(asx_overload_mode mode);

/* -------------------------------------------------------------------
 * Metric gate — pass/fail thresholds for CI/monitoring
 *
 * A gate defines latency and jitter thresholds. The gate evaluates
 * pass/fail against a histogram + jitter tracker combo.
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t p99_ns;       /* p99 must be <= this value */
    uint64_t p99_9_ns;     /* p99.9 must be <= this value (0 = skip) */
    uint64_t p99_99_ns;    /* p99.99 must be <= this value (0 = skip) */
    uint64_t jitter_ns;    /* MAD must be <= this value (0 = skip) */
} asx_hft_gate;

typedef struct {
    int      pass;           /* 1 if all thresholds met */
    uint64_t actual_p99;     /* actual p99 value */
    uint64_t actual_p99_9;   /* actual p99.9 value */
    uint64_t actual_p99_99;  /* actual p99.99 value */
    uint64_t actual_jitter;  /* actual MAD */
    uint32_t violations;     /* bitmask of which thresholds failed */
} asx_hft_gate_result;

/* Violation bitmask bits */
#define ASX_GATE_VIOLATION_P99     (1u << 0)
#define ASX_GATE_VIOLATION_P99_9   (1u << 1)
#define ASX_GATE_VIOLATION_P99_99  (1u << 2)
#define ASX_GATE_VIOLATION_JITTER  (1u << 3)

/* Initialize a gate with default HFT thresholds. */
ASX_API void asx_hft_gate_init(asx_hft_gate *gate);

/* Evaluate a gate against a histogram and jitter tracker.
 * Either hist or jt may be NULL if the corresponding thresholds
 * are zeroed in the gate. */
ASX_API void asx_hft_gate_evaluate(const asx_hft_gate *gate,
                                    const asx_hft_histogram *hist,
                                    const asx_hft_jitter_tracker *jt,
                                    asx_hft_gate_result *result);

/* -------------------------------------------------------------------
 * Global HFT instrumentation state
 *
 * Singleton for per-process HFT metrics. Reset via
 * asx_hft_instrument_reset().
 * ------------------------------------------------------------------- */

/* Reset all global HFT instrumentation state. */
ASX_API void asx_hft_instrument_reset(void);

/* Get pointer to the global scheduler latency histogram. */
ASX_API asx_hft_histogram *asx_hft_sched_histogram(void);

/* Get pointer to the global jitter tracker. */
ASX_API asx_hft_jitter_tracker *asx_hft_sched_jitter(void);

/* Record a scheduler poll latency sample (convenience wrapper). */
ASX_API void asx_hft_record_poll_latency(uint64_t ns);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_HFT_INSTRUMENT_H */
