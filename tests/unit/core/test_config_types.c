/*
 * test_config_types.c — unit tests for backoff, timeout, and transport config
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/config_types.h>

/* ================================================================== */
/* Backoff config init                                                 */
/* ================================================================== */

TEST(backoff_init_defaults) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    ASSERT_EQ((int)cfg.strategy, (int)ASX_BACKOFF_EXPONENTIAL);
    ASSERT_TRUE(cfg.initial_delay_ns > 0);
    ASSERT_TRUE(cfg.max_delay_ns > cfg.initial_delay_ns);
    ASSERT_TRUE(cfg.max_retries > 0);
}

TEST(backoff_init_null_safe) { asx_backoff_config_init(NULL); /* should not crash */ }

/* ================================================================== */
/* Backoff: NONE strategy                                              */
/* ================================================================== */

TEST(backoff_none_always_zero) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_NONE;
    cfg.max_retries = 10; /* ensure we're within retry limit */
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)0);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 5), (uint64_t)0);
}

/* ================================================================== */
/* Backoff: CONSTANT strategy                                          */
/* ================================================================== */

TEST(backoff_constant_same_every_attempt) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_CONSTANT;
    cfg.initial_delay_ns = 500;
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)500);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 3), (uint64_t)500);
}

/* ================================================================== */
/* Backoff: LINEAR strategy                                            */
/* ================================================================== */

TEST(backoff_linear_increases) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_LINEAR;
    cfg.initial_delay_ns = 100;
    cfg.max_delay_ns = 10000;
    cfg.max_retries = 10;

    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)100); /* 100 * 1 */
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 1), (uint64_t)200); /* 100 * 2 */
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 4), (uint64_t)500); /* 100 * 5 */
}

TEST(backoff_linear_caps_at_max) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_LINEAR;
    cfg.initial_delay_ns = 1000;
    cfg.max_delay_ns = 3000;
    cfg.max_retries = 10;

    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)1000);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 2), (uint64_t)3000); /* 1000*3 = 3000 = max */
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 9), (uint64_t)3000); /* capped */
}

TEST(backoff_linear_overflow_safe) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_LINEAR;
    cfg.initial_delay_ns = UINT64_MAX / 2;
    cfg.max_delay_ns = UINT64_MAX;
    cfg.max_retries = 10;

    /* Large initial_delay * factor would overflow; should clamp to max */
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 4), UINT64_MAX);
}

/* ================================================================== */
/* Backoff: EXPONENTIAL strategy                                       */
/* ================================================================== */

TEST(backoff_exponential_doubles) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_EXPONENTIAL;
    cfg.initial_delay_ns = 100;
    cfg.max_delay_ns = 100000;
    cfg.multiplier = 2.0;
    cfg.max_retries = 10;

    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)100);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 1), (uint64_t)200);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 2), (uint64_t)400);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 3), (uint64_t)800);
}

TEST(backoff_exponential_caps_at_max) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_EXPONENTIAL;
    cfg.initial_delay_ns = 100;
    cfg.max_delay_ns = 500;
    cfg.multiplier = 2.0;
    cfg.max_retries = 10;

    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 0), (uint64_t)100);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 1), (uint64_t)200);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 2), (uint64_t)400);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 3), (uint64_t)500); /* capped */
}

/* ================================================================== */
/* Backoff: JITTERED strategy                                          */
/* ================================================================== */

TEST(backoff_jittered_less_than_exponential) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_JITTERED;
    cfg.initial_delay_ns = 1000;
    cfg.max_delay_ns = 100000;
    cfg.multiplier = 2.0;
    cfg.max_retries = 10;

    /* Jittered delay should be less than pure exponential due to reduction */
    {
        uint64_t jittered = asx_backoff_delay_for_attempt(&cfg, 2);
        cfg.strategy = ASX_BACKOFF_EXPONENTIAL;
        {
            uint64_t exponential = asx_backoff_delay_for_attempt(&cfg, 2);
            ASSERT_TRUE(jittered < exponential);
        }
    }
}

/* ================================================================== */
/* Backoff: beyond max_retries returns max_delay                       */
/* ================================================================== */

TEST(backoff_beyond_max_retries) {
    asx_backoff_config cfg;
    asx_backoff_config_init(&cfg);
    cfg.strategy = ASX_BACKOFF_EXPONENTIAL;
    cfg.initial_delay_ns = 100;
    cfg.max_delay_ns = 5000;
    cfg.max_retries = 3;

    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 3), cfg.max_delay_ns);
    ASSERT_EQ(asx_backoff_delay_for_attempt(&cfg, 100), cfg.max_delay_ns);
}

/* ================================================================== */
/* Backoff: null config                                                */
/* ================================================================== */

TEST(backoff_null_returns_zero) { ASSERT_EQ(asx_backoff_delay_for_attempt(NULL, 0), (uint64_t)0); }

/* ================================================================== */
/* Timeout config init                                                 */
/* ================================================================== */

TEST(timeout_init_defaults) {
    asx_timeout_config cfg;
    asx_timeout_config_init(&cfg);
    ASSERT_TRUE(cfg.connect_timeout_ns > 0);
    ASSERT_TRUE(cfg.request_timeout_ns > 0);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_config_types ===\n");

    RUN_TEST(backoff_init_defaults);
    RUN_TEST(backoff_init_null_safe);

    RUN_TEST(backoff_none_always_zero);
    RUN_TEST(backoff_constant_same_every_attempt);

    RUN_TEST(backoff_linear_increases);
    RUN_TEST(backoff_linear_caps_at_max);
    RUN_TEST(backoff_linear_overflow_safe);

    RUN_TEST(backoff_exponential_doubles);
    RUN_TEST(backoff_exponential_caps_at_max);

    RUN_TEST(backoff_jittered_less_than_exponential);

    RUN_TEST(backoff_beyond_max_retries);
    RUN_TEST(backoff_null_returns_zero);

    RUN_TEST(timeout_init_defaults);

    TEST_REPORT();
    return test_failures;
}
