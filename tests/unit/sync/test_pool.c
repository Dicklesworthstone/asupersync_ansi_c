/*
 * test_pool.c — unit tests for generic resource pool
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/sync/pool.h>

/* ------------------------------------------------------------------ */
/* Mock resource factory                                               */
/* ------------------------------------------------------------------ */

static int create_count;
static int destroy_count;
static int health_count;
static int health_result; /* 1 = healthy, 0 = unhealthy */

static int resources[32]; /* backing store for mock resources */

static asx_status mock_create(void *factory_data, void **out) {
    (void)factory_data;
    resources[create_count] = create_count + 1;
    *out = &resources[create_count];
    create_count++;
    return ASX_OK;
}

static void mock_destroy(void *resource, void *factory_data) {
    (void)resource;
    (void)factory_data;
    destroy_count++;
}

static int mock_health(void *resource, void *factory_data) {
    (void)resource;
    (void)factory_data;
    health_count++;
    return health_result;
}

static void setup(void) {
    asx_pool_reset();
    create_count = 0;
    destroy_count = 0;
    health_count = 0;
    health_result = 1;
}

static asx_pool_config basic_config(uint32_t max_size) {
    asx_pool_config c;
    c.max_size = max_size;
    c.min_idle = 0;
    c.create_fn = mock_create;
    c.destroy_fn = mock_destroy;
    c.health_fn = NULL;
    c.factory_data = NULL;
    return c;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(create_and_close) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    ASSERT_FALSE(asx_pool_is_closed(h));
    ASSERT_EQ(asx_pool_close(h), ASX_OK);
    ASSERT_TRUE(asx_pool_is_closed(h));
}

TEST(create_null_fails) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    setup();
    ASSERT_EQ(asx_pool_create(NULL, &h), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_pool_create(&cfg, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_zero_size_fails) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(0);
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Acquire / Return                                                    */
/* ------------------------------------------------------------------ */

TEST(acquire_creates_resource) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr), ASX_OK);
    ASSERT_TRUE(pr.resource != NULL);
    ASSERT_EQ(create_count, 1);
    ASSERT_EQ(asx_pool_return(&pr), ASX_OK);
    ASSERT_TRUE(pr.resource == NULL); /* cleared on return */
    asx_pool_close(h);
}

TEST(acquire_reuses_returned_resource) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr1, pr2;
    void *first_resource;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr1), ASX_OK);
    first_resource = pr1.resource;
    ASSERT_EQ(asx_pool_return(&pr1), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr2), ASX_OK);
    ASSERT_EQ(pr2.resource, first_resource); /* same resource reused */
    ASSERT_EQ(create_count, 1);              /* only created once */

    ASSERT_EQ(asx_pool_return(&pr2), ASX_OK);
    asx_pool_close(h);
}

TEST(acquire_blocked_when_at_max) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(2);
    asx_pooled_resource pr1, pr2, pr3;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr1), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr2), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr3), ASX_E_WOULD_BLOCK);

    asx_pool_return(&pr1);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr3), ASX_OK);

    asx_pool_return(&pr2);
    asx_pool_return(&pr3);
    asx_pool_close(h);
}

TEST(acquire_after_close_fails) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    ASSERT_EQ(asx_pool_close(h), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr), ASX_E_DISCONNECTED);
}

/* ------------------------------------------------------------------ */
/* Health checks                                                       */
/* ------------------------------------------------------------------ */

TEST(health_check_rejects_unhealthy) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr1, pr2;
    asx_pool_stats stats;
    setup();
    cfg.health_fn = mock_health;
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    /* Acquire, return, then make unhealthy */
    ASSERT_EQ(asx_pool_try_acquire(h, &pr1), ASX_OK);
    asx_pool_return(&pr1);
    health_result = 0; /* mark unhealthy */

    /* Next acquire should fail health check, create new resource */
    ASSERT_EQ(asx_pool_try_acquire(h, &pr2), ASX_OK);
    ASSERT_EQ(health_count, 1);  /* health check was called */
    ASSERT_EQ(create_count, 2);  /* new resource was created */
    ASSERT_EQ(destroy_count, 1); /* unhealthy one was destroyed */

    asx_pool_get_stats(h, &stats);
    ASSERT_EQ(stats.health_failures, (uint64_t)1);

    asx_pool_return(&pr2);
    asx_pool_close(h);
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

TEST(stats_track_usage) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr1, pr2;
    asx_pool_stats stats;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr1), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &pr2), ASX_OK);

    ASSERT_EQ(asx_pool_get_stats(h, &stats), ASX_OK);
    ASSERT_EQ(stats.active, 2u);
    ASSERT_EQ(stats.idle, 0u);
    ASSERT_EQ(stats.total, 2u);
    ASSERT_EQ(stats.max_size, 4u);
    ASSERT_EQ(stats.total_acquisitions, (uint64_t)2);
    ASSERT_EQ(stats.total_creates, (uint64_t)2);

    asx_pool_return(&pr1);
    ASSERT_EQ(asx_pool_get_stats(h, &stats), ASX_OK);
    ASSERT_EQ(stats.active, 1u);
    ASSERT_EQ(stats.idle, 1u);

    asx_pool_return(&pr2);
    asx_pool_close(h);
}

/* ------------------------------------------------------------------ */
/* Close destroys idle resources                                       */
/* ------------------------------------------------------------------ */

TEST(close_destroys_idle) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr), ASX_OK);
    asx_pool_return(&pr);
    ASSERT_EQ(destroy_count, 0);

    ASSERT_EQ(asx_pool_close(h), ASX_OK);
    ASSERT_EQ(destroy_count, 1); /* idle resource destroyed on close */
}

TEST(return_to_closed_pool_destroys) {
    asx_pool_handle h;
    asx_pool_config cfg = basic_config(4);
    asx_pooled_resource pr;
    setup();
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);

    ASSERT_EQ(asx_pool_try_acquire(h, &pr), ASX_OK);
    ASSERT_EQ(asx_pool_close(h), ASX_OK);
    ASSERT_EQ(destroy_count, 0); /* active resource not destroyed yet */

    ASSERT_EQ(asx_pool_return(&pr), ASX_OK);
    ASSERT_EQ(destroy_count, 1); /* destroyed on return */
}

/* ------------------------------------------------------------------ */
/* Arena exhaustion                                                    */
/* ------------------------------------------------------------------ */

TEST(pool_arena_exhaustion) {
    asx_pool_handle handles[ASX_POOL_MAX];
    asx_pool_handle overflow;
    asx_pool_config cfg = basic_config(1);
    uint32_t i;
    setup();
    for (i = 0; i < ASX_POOL_MAX; i++) { ASSERT_EQ(asx_pool_create(&cfg, &handles[i]), ASX_OK); }
    ASSERT_EQ(asx_pool_create(&cfg, &overflow), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0; i < ASX_POOL_MAX; i++) { asx_pool_close(handles[i]); }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_pool ===\n");

    RUN_TEST(create_and_close);
    RUN_TEST(create_null_fails);
    RUN_TEST(create_zero_size_fails);

    RUN_TEST(acquire_creates_resource);
    RUN_TEST(acquire_reuses_returned_resource);
    RUN_TEST(acquire_blocked_when_at_max);
    RUN_TEST(acquire_after_close_fails);

    RUN_TEST(health_check_rejects_unhealthy);

    RUN_TEST(stats_track_usage);
    RUN_TEST(close_destroys_idle);
    RUN_TEST(return_to_closed_pool_destroys);
    RUN_TEST(pool_arena_exhaustion);

    TEST_REPORT();
    return test_failures;
}
