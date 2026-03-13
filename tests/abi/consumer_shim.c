/*
 * consumer_shim.c — ABI/API stability consumer shim test (bd-56t.4)
 *
 * Compiles against public headers only. Verifies that:
 * 1. All public types are accessible and have expected sizes
 * 2. ABI version macros are defined and consistent
 * 3. Key enum values have their frozen assignments
 * 4. Config-struct size-field pattern works
 * 5. Function symbols resolve at link time
 *
 * This test MUST NOT include any private/internal headers.
 * It simulates an external consumer linking against libasx.
 *
 * Build: cc -std=c99 -I include -o consumer_shim tests/abi/consumer_shim.c -L build/lib -lasx
 * Run:   ./consumer_shim
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Consumer shim — no runtime loops") */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only public headers — simulates an external consumer */
#include <asx/asx.h>

static int failures = 0;
static int passes = 0;

#define SHIM_CHECK(cond, msg)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            failures++;                                                                            \
        } else {                                                                                   \
            passes++;                                                                              \
        }                                                                                          \
    } while (0)

/* --- ABI version checks --- */

static void test_abi_version(void) {
    fprintf(stderr, "--- ABI version ---\n");

    /* Compile-time version macros exist */
    SHIM_CHECK(ASX_ABI_VERSION_MAJOR >= 1, "ABI major >= 1");
    SHIM_CHECK(ASX_ABI_VERSION_MINOR >= 0, "ABI minor >= 0");
    SHIM_CHECK(ASX_ABI_VERSION_PATCH >= 0, "ABI patch >= 0");

    /* Composite version */
    SHIM_CHECK(ASX_ABI_VERSION >= 1000000, "ABI composite version >= 1.0.0");

    /* Runtime version matches compile-time */
    SHIM_CHECK(asx_abi_version_major() == ASX_ABI_VERSION_MAJOR,
               "runtime ABI major matches header");
    SHIM_CHECK(asx_abi_version_minor() == ASX_ABI_VERSION_MINOR,
               "runtime ABI minor matches header");
    SHIM_CHECK(asx_abi_version_patch() == ASX_ABI_VERSION_PATCH,
               "runtime ABI patch matches header");

    /* API version macros exist */
    SHIM_CHECK(ASX_API_VERSION_MAJOR >= 0, "API major defined");
    SHIM_CHECK(ASX_API_VERSION_MINOR >= 0, "API minor defined");
    SHIM_CHECK(ASX_API_VERSION_PATCH >= 0, "API patch defined");

    /* ABI check macro compiles */
    ASX_ABI_CHECK(1);
}

/* --- Handle type sizes --- */

static void test_handle_sizes(void) {
    fprintf(stderr, "--- Handle type sizes ---\n");

    SHIM_CHECK(sizeof(asx_region_id) == ASX_ABI_SIZEOF_REGION_ID, "asx_region_id size matches ABI");
    SHIM_CHECK(sizeof(asx_task_id) == ASX_ABI_SIZEOF_TASK_ID, "asx_task_id size matches ABI");
    SHIM_CHECK(sizeof(asx_obligation_id) == ASX_ABI_SIZEOF_OBLIGATION_ID,
               "asx_obligation_id size matches ABI");
    SHIM_CHECK(sizeof(asx_timer_id) == ASX_ABI_SIZEOF_TIMER_ID, "asx_timer_id size matches ABI");
    SHIM_CHECK(sizeof(asx_channel_id) == ASX_ABI_SIZEOF_CHANNEL_ID,
               "asx_channel_id size matches ABI");
    SHIM_CHECK(sizeof(asx_status) == ASX_ABI_SIZEOF_STATUS, "asx_status size matches ABI");
    SHIM_CHECK(sizeof(asx_time) == ASX_ABI_SIZEOF_TIME, "asx_time size matches ABI");
}

/* --- Frozen enum values --- */

static void test_frozen_enums(void) {
    fprintf(stderr, "--- Frozen enum values ---\n");

    /* Region states */
    SHIM_CHECK(ASX_REGION_OPEN == 0, "ASX_REGION_OPEN == 0");
    SHIM_CHECK(ASX_REGION_CLOSED == 4, "ASX_REGION_CLOSED == 4");

    /* Task states */
    SHIM_CHECK(ASX_TASK_CREATED == 0, "ASX_TASK_CREATED == 0");
    SHIM_CHECK(ASX_TASK_COMPLETED == 5, "ASX_TASK_COMPLETED == 5");

    /* Outcome severity */
    SHIM_CHECK(ASX_OUTCOME_OK == 0, "ASX_OUTCOME_OK == 0");
    SHIM_CHECK(ASX_OUTCOME_PANICKED == 3, "ASX_OUTCOME_PANICKED == 3");

    /* Cancel kinds */
    SHIM_CHECK(ASX_CANCEL_USER == 0, "ASX_CANCEL_USER == 0");

    /* Cancel phases */
    SHIM_CHECK(ASX_CANCEL_PHASE_REQUESTED == 0, "ASX_CANCEL_PHASE_REQUESTED == 0");
    SHIM_CHECK(ASX_CANCEL_PHASE_COMPLETED == 3, "ASX_CANCEL_PHASE_COMPLETED == 3");

    /* Status codes — family structure */
    SHIM_CHECK(ASX_OK == 0, "ASX_OK == 0");
    SHIM_CHECK(ASX_E_PENDING == 1, "ASX_E_PENDING == 1");
    SHIM_CHECK(ASX_E_INVALID_ARGUMENT == 100, "ASX_E_INVALID_ARGUMENT == 100");
    SHIM_CHECK(ASX_E_INVALID_TRANSITION == 200, "ASX_E_INVALID_TRANSITION == 200");
    SHIM_CHECK(ASX_E_REGION_NOT_FOUND == 300, "ASX_E_REGION_NOT_FOUND == 300");
    SHIM_CHECK(ASX_E_TASK_NOT_FOUND == 400, "ASX_E_TASK_NOT_FOUND == 400");
    SHIM_CHECK(ASX_E_CANCELLED == 600, "ASX_E_CANCELLED == 600");
    SHIM_CHECK(ASX_E_DISCONNECTED == 700, "ASX_E_DISCONNECTED == 700");
    SHIM_CHECK(ASX_E_TIMER_NOT_FOUND == 800, "ASX_E_TIMER_NOT_FOUND == 800");
    SHIM_CHECK(ASX_E_RESOURCE_EXHAUSTED == 1000, "ASX_E_RESOURCE_EXHAUSTED == 1000");
    SHIM_CHECK(ASX_E_STALE_HANDLE == 1100, "ASX_E_STALE_HANDLE == 1100");
    SHIM_CHECK(ASX_E_HOOK_MISSING == 1200, "ASX_E_HOOK_MISSING == 1200");

    /* Invalid handle sentinel */
    SHIM_CHECK(ASX_INVALID_ID == 0, "ASX_INVALID_ID == 0");
}

/* --- Config-struct size-field pattern --- */

static void test_config_pattern(void) {
    asx_runtime_config cfg;

    fprintf(stderr, "--- Config size-field pattern ---\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.size = (uint32_t)sizeof(cfg);

    SHIM_CHECK(cfg.size == sizeof(asx_runtime_config), "config size-field roundtrip");
    SHIM_CHECK(cfg.size > 0, "config size nonzero");
}

/* --- Function symbol resolution --- */

static void test_symbol_resolution(void) {
    fprintf(stderr, "--- Symbol resolution ---\n");

    /* Core lifecycle functions resolve */
    SHIM_CHECK(asx_region_open != NULL, "asx_region_open resolves");
    SHIM_CHECK(asx_task_spawn != NULL, "asx_task_spawn resolves");
    SHIM_CHECK(asx_scheduler_run != NULL, "asx_scheduler_run resolves");
    SHIM_CHECK(asx_runtime_reset != NULL, "asx_runtime_reset resolves");

    /* Status helpers */
    SHIM_CHECK(asx_status_str != NULL, "asx_status_str resolves");

    /* ABI version */
    SHIM_CHECK(asx_abi_version_major != NULL, "asx_abi_version_major resolves");

    /* Handle helpers */
    SHIM_CHECK(asx_handle_is_valid(ASX_INVALID_ID) == 0, "invalid handle not valid");
}

/* --- Smoke: basic lifecycle works --- */

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static void test_lifecycle_smoke(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    fprintf(stderr, "--- Lifecycle smoke ---\n");

    asx_runtime_reset();

    st = asx_region_open(&rid);
    SHIM_CHECK(st == ASX_OK, "region_open succeeds");

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    SHIM_CHECK(st == ASX_OK, "task_spawn succeeds");

    budget = asx_budget_from_polls(64);
    st = asx_scheduler_run(rid, &budget);
    SHIM_CHECK(st == ASX_OK, "scheduler_run succeeds");
}

/* --- Main --- */

int main(void) {
    fprintf(stderr, "[asx-consumer-shim] ABI/API stability consumer shim test (bd-56t.4)\n\n");

    test_abi_version();
    test_handle_sizes();
    test_frozen_enums();
    test_config_pattern();
    test_symbol_resolution();
    test_lifecycle_smoke();

    fprintf(stderr, "\n[asx-consumer-shim] %d passed, %d failed\n", passes, failures);

    return failures > 0 ? 1 : 0;
}
