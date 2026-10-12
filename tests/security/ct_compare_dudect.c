/*
 * ct_compare_dudect.c — timing-leak check of asx_crypto_ct_equal and
 * asx_crypto_ct_is_zero (bd-9kll.16.5)
 *
 * A dudect-style test (Reparaz, Balasch, Verbauwhede, "Dude, is my code
 * constant time?", 2017): time the function on two classes of input, drop
 * the slowest outliers, and apply Welch's t-test to the two timing
 * distributions. A comparison whose running time depends on where two
 * secrets differ gives the classes different means, and the t statistic
 * grows with the number of samples; a constant-time one does not.
 *
 *   class 0  the two buffers are equal (a full pass is unavoidable)
 *   class 1  they differ in the first byte (an early exit is possible)
 *   class 2  they differ in every byte, against class 0
 *
 * A passing run is only worth something if the harness can see a leak, so
 * each experiment also times a deliberately leaky comparison (early exit on
 * the first difference) and requires that it is flagged. If the control is
 * not flagged the harness has no power on this machine (the optimizer
 * removed the work, a clock too coarse to resolve a pass): that is a harness
 * failure, never a pass.
 *
 * This is not part of `make test`: timing statistics are noisy on shared or
 * virtualized machines. Run it as `make test-ct-timing` on a quiet one.
 *
 * Exit status: 0 no leak found and the control was flagged; 1 a leak was
 * flagged in the library function in every attempt; 2 the harness could not
 * flag its own control (inconclusive).
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <asx/security/crypto.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* dudect's own "definitely not constant time" threshold on |t|. */
#define CT_T_LEAK 10.0
#define CT_BUF_LEN 1024u
/* Calls per timed sample: amortizes the clock read (tens of ns) over a
 * pass that takes about a microsecond. */
#define CT_BATCH 16u
#define CT_DEFAULT_SAMPLES 40000u
#define CT_DEFAULT_ATTEMPTS 3u
/* Samples above this percentile of the pooled distribution are dropped,
 * as dudect's cropping does: scheduler preemption and interrupts, not the
 * code under test. */
#define CT_CROP_PERCENTILE 0.90

typedef int (*equal_fn)(const void *a, const void *b, size_t len);
typedef int (*zero_fn)(const void *buf, size_t len);

static volatile int g_sink;

/* ------------------------------------------------------------------ */
/* Deliberately leaky controls                                         */
/* ------------------------------------------------------------------ */

/* The early-exit comparison a constant-time one replaces. Volatile reads
 * keep the optimizer from vectorizing or folding it, so its time really
 * depends on the first difference. */
static int leaky_equal(const void *a, const void *b, size_t len) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    size_t i;
    for (i = 0u; i < len; i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static int leaky_is_zero(const void *buf, size_t len) {
    const volatile uint8_t *p = (const volatile uint8_t *)buf;
    size_t i;
    for (i = 0u; i < len; i++) {
        if (p[i] != 0u) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Measurement                                                         */
/* ------------------------------------------------------------------ */

static double now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)ts.tv_sec * 1e9) + (double)ts.tv_nsec;
}

/* xorshift64: the class sequence must not correlate with anything the
 * machine does, and must be reproducible from the seed. */
static uint64_t g_rng = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x;
    double b = *(const double *)y;
    return (a > b) - (a < b);
}

typedef struct {
    double n;
    double mean;
    double m2; /* sum of squared deviations (Welford) */
} moments;

static void moments_push(moments *m, double x) {
    double delta = x - m->mean;
    m->n += 1.0;
    m->mean += delta / m->n;
    m->m2 += delta * (x - m->mean);
}

/* Welch's t statistic of class 0 against class 1. */
static double welch_t(const moments *a, const moments *b) {
    double va;
    double vb;
    double denom;
    if (a->n < 2.0 || b->n < 2.0) return 0.0;
    va = a->m2 / (a->n - 1.0);
    vb = b->m2 / (b->n - 1.0);
    denom = sqrt((va / a->n) + (vb / b->n));
    if (denom == 0.0) return 0.0;
    return (a->mean - b->mean) / denom;
}

/* One experiment: `samples` timed batches, each randomly in class 0 or 1.
 * `call` runs the function under test on (x, y). Returns the t statistic
 * after cropping. */
typedef struct {
    equal_fn equal;
    zero_fn zero;
    const uint8_t *x;
    const uint8_t *y0;
    const uint8_t *y1;
} experiment;

static double run_experiment(const experiment *e, uint32_t samples, uint32_t *out_kept) {
    double *times = (double *)malloc((size_t)samples * sizeof(double));
    uint8_t *cls = (uint8_t *)malloc((size_t)samples);
    double *sorted = (double *)malloc((size_t)samples * sizeof(double));
    moments m[2];
    double cutoff;
    uint32_t i;
    uint32_t j;
    int acc = 0;
    double t;

    if (times == NULL || cls == NULL || sorted == NULL) {
        free(times);
        free(cls);
        free(sorted);
        fprintf(stderr, "ct_compare_dudect: out of memory\n");
        exit(2);
    }

    /* Warm the caches and the branch predictors with both classes. */
    for (i = 0u; i < 512u; i++) {
        if (e->equal != NULL) {
            acc += e->equal(e->x, (i & 1u) ? e->y1 : e->y0, CT_BUF_LEN);
        } else {
            acc += e->zero((i & 1u) ? e->y1 : e->y0, CT_BUF_LEN);
        }
    }

    for (i = 0u; i < samples; i++) {
        uint8_t c = (uint8_t)(rng_next() & 1u);
        const uint8_t *y = c ? e->y1 : e->y0;
        double t0;
        double t1;
        cls[i] = c;
        t0 = now_ns();
        for (j = 0u; j < CT_BATCH; j++) {
            if (e->equal != NULL) {
                acc += e->equal(e->x, y, CT_BUF_LEN);
            } else {
                acc += e->zero(y, CT_BUF_LEN);
            }
        }
        t1 = now_ns();
        times[i] = (t1 - t0) / (double)CT_BATCH;
    }
    g_sink = acc;

    memcpy(sorted, times, (size_t)samples * sizeof(double));
    qsort(sorted, (size_t)samples, sizeof(double), cmp_double);
    cutoff = sorted[(size_t)((double)(samples - 1u) * CT_CROP_PERCENTILE)];

    memset(m, 0, sizeof(m));
    for (i = 0u; i < samples; i++) {
        if (times[i] <= cutoff) moments_push(&m[cls[i]], times[i]);
    }
    t = welch_t(&m[0], &m[1]);
    if (out_kept != NULL) *out_kept = (uint32_t)(m[0].n + m[1].n);

    free(times);
    free(cls);
    free(sorted);
    return t;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int (*fn_equal)(const void *, const void *, size_t);
    int (*fn_zero)(const void *, size_t);
    int use_all_different; /* class 1 differs everywhere, not in byte 0 */
    int is_control;
} subject;

/* Returns the strongest |t| of the attempts for a control, and the weakest
 * (best case for the library) for a function under test: a real leak shows
 * in every attempt, a noisy neighbor in some. */
static double run_subject(const subject *s, uint32_t samples, uint32_t attempts) {
    static uint8_t x[CT_BUF_LEN];
    static uint8_t y0[CT_BUF_LEN];
    static uint8_t y1[CT_BUF_LEN];
    experiment e;
    uint32_t a;
    uint32_t i;
    double result = 0.0;

    for (i = 0u; i < CT_BUF_LEN; i++) {
        x[i] = (uint8_t)(rng_next() & 0xFFu);
        y0[i] = x[i];
        y1[i] = (uint8_t)(x[i] ^ 0xFFu);
    }
    if (!s->use_all_different) {
        /* differ in the first byte only */
        memcpy(y1, x, CT_BUF_LEN);
        y1[0] = (uint8_t)(x[0] ^ 0xFFu);
    }
    if (s->fn_zero != NULL) {
        /* the zero predicate: class 0 all zero, class 1 nonzero first byte */
        memset(x, 0, CT_BUF_LEN);
        memset(y0, 0, CT_BUF_LEN);
        memset(y1, 0, CT_BUF_LEN);
        y1[0] = 0xA5u;
        if (s->use_all_different) memset(y1, 0xA5, CT_BUF_LEN);
    }
    e.equal = s->fn_equal;
    e.zero = s->fn_zero;
    e.x = x;
    e.y0 = y0;
    e.y1 = y1;

    for (a = 0u; a < attempts; a++) {
        uint32_t kept = 0u;
        double t = run_experiment(&e, samples, &kept);
        double abs_t = fabs(t);
        printf("  attempt %u: t = %+9.2f  (|t| %s %.0f; %u of %u samples kept)\n", a + 1u, t,
               abs_t > CT_T_LEAK ? ">" : "<=", CT_T_LEAK, kept, samples);
        if (a == 0u) {
            result = abs_t;
        } else if (s->is_control) {
            if (abs_t > result) result = abs_t;
        } else if (abs_t < result) {
            result = abs_t;
        }
        /* A function under test that passes once needs no more attempts; a
         * control that is flagged once is flagged. */
        if (s->is_control ? abs_t > CT_T_LEAK : abs_t <= CT_T_LEAK) break;
    }
    return result;
}

int main(int argc, char **argv) {
    static const subject subjects[] = {
        {"leaky_equal (control)", leaky_equal, NULL, 0, 1},
        {"leaky_is_zero (control)", NULL, leaky_is_zero, 0, 1},
        {"asx_crypto_ct_equal, first byte differs", asx_crypto_ct_equal, NULL, 0, 0},
        {"asx_crypto_ct_equal, every byte differs", asx_crypto_ct_equal, NULL, 1, 0},
        {"asx_crypto_ct_is_zero, first byte nonzero", NULL, asx_crypto_ct_is_zero, 0, 0},
        {"asx_crypto_ct_is_zero, every byte nonzero", NULL, asx_crypto_ct_is_zero, 1, 0},
    };
    uint32_t samples = CT_DEFAULT_SAMPLES;
    uint32_t attempts = CT_DEFAULT_ATTEMPTS;
    int controls_flagged = 1;
    int leak_found = 0;
    size_t i;

    for (i = 1u; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--samples") == 0 && i + 1u < (size_t)argc) {
            samples = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--attempts") == 0 && i + 1u < (size_t)argc) {
            attempts = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1u < (size_t)argc) {
            g_rng = (uint64_t)strtoull(argv[++i], NULL, 10);
            if (g_rng == 0u) g_rng = 1u;
        } else {
            fprintf(stderr, "usage: %s [--samples N] [--attempts N] [--seed N]\n", argv[0]);
            return 2;
        }
    }
    if (samples < 1000u || attempts < 1u) {
        fprintf(stderr, "ct_compare_dudect: need --samples >= 1000 and --attempts >= 1\n");
        return 2;
    }

    printf("ct_compare_dudect: %u samples x %u calls of %u bytes, up to %u attempts, "
           "leak threshold |t| > %.0f\n",
           samples, (unsigned)CT_BATCH, (unsigned)CT_BUF_LEN, attempts, CT_T_LEAK);

    /* Controls first: the harness must flag a leak before it may call
     * anything clean. */
    for (i = 0u; i < sizeof(subjects) / sizeof(subjects[0]); i++) {
        const subject *s = &subjects[i];
        double abs_t;
        printf("%s\n", s->name);
        abs_t = run_subject(s, samples, attempts);
        if (s->is_control) {
            int flagged = abs_t > CT_T_LEAK;
            printf("  -> control %s (|t| = %.2f)\n",
                   flagged ? "FLAGGED, as it must be" : "NOT FLAGGED", abs_t);
            if (!flagged) controls_flagged = 0;
        } else {
            int clean = abs_t <= CT_T_LEAK;
            printf("  -> %s (best |t| = %.2f)\n", clean ? "no leak detected" : "LEAK DETECTED",
                   abs_t);
            if (!clean) leak_found = 1;
        }
    }

    if (!controls_flagged) {
        printf("ct_compare_dudect: INCONCLUSIVE: the harness did not flag its own leaky control; "
               "its verdicts on the library mean nothing on this machine.\n");
        return 2;
    }
    if (leak_found) {
        printf("ct_compare_dudect: FAIL: a timing dependence on the input was detected.\n");
        return 1;
    }
    printf("ct_compare_dudect: PASS: no timing dependence detected; controls flagged.\n");
    return 0;
}
