/*
 * vignette_plan.c — operator-path walkthrough for plan rewrites and service stacks
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/plan/plan.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int ready_flag;
    int call_count;
    int last_request;
} demo_service_state;

static int require_status(asx_status st, const char *label) {
    if (st != ASX_OK) {
        fprintf(stderr, "%s failed: %s\n", label, asx_status_str(st));
        return 0;
    }
    return 1;
}

static asx_service_readiness demo_poll_ready(void *state) {
    demo_service_state *svc = (demo_service_state *)state;
    return svc->ready_flag ? ASX_SERVICE_READY : ASX_SERVICE_NOT_READY;
}

static asx_status demo_call(void *state, const void *request, void *response) {
    const int *req = (const int *)request;
    int *resp = (int *)response;
    demo_service_state *svc = (demo_service_state *)state;

    svc->call_count++;
    svc->last_request = *req;
    *resp = *req + 100;
    return ASX_OK;
}

static int scenario_plan_rewrite_walkthrough(void) {
    asx_plan_dag dag;
    asx_rewrite_policy policy;
    asx_rewrite_certificate cert;
    asx_plan_hash before_hash;
    asx_plan_hash after_hash;
    asx_plan_id parse;
    asx_plan_id route;
    asx_plan_id encode;
    asx_plan_id inner_children[2];
    asx_plan_id outer_children[2];
    uint16_t leaves = 0;
    uint16_t joins = 0;
    uint16_t races = 0;
    uint16_t timeouts = 0;

    printf("--- scenario: plan rewrite walkthrough ---\n");

    asx_plan_dag_init(&dag);
    parse = asx_plan_dag_leaf(&dag, "parse");
    route = asx_plan_dag_leaf(&dag, "route");
    encode = asx_plan_dag_leaf(&dag, "encode");
    inner_children[0] = parse;
    inner_children[1] = route;
    outer_children[0] = asx_plan_dag_join(&dag, inner_children, 2);
    outer_children[1] = encode;
    asx_plan_dag_set_root(&dag, asx_plan_dag_join(&dag, outer_children, 2));

    if (dag.root == ASX_PLAN_ID_NONE ||
        !require_status(asx_plan_dag_validate(&dag), "plan validate")) {
        return 1;
    }

    before_hash = asx_plan_hash_compute(&dag);
    asx_rewrite_policy_conservative(&policy);
    asx_rewrite_certificate_init(&cert);
    if (!require_status(asx_plan_rewrite(&dag, &policy, &cert), "plan rewrite")) { return 1; }
    after_hash = asx_plan_hash_compute(&dag);
    asx_plan_dag_stats(&dag, &leaves, &joins, &races, &timeouts);

    if (cert.step_count == 0u || joins != 2u || leaves != 3u || races != 0u || timeouts != 0u) {
        fprintf(stderr, "unexpected rewrite stats\n");
        return 1;
    }

    printf("plan.hash.before=0x%llx\n", (unsigned long long)before_hash.value);
    printf("plan.hash.after=0x%llx\n", (unsigned long long)after_hash.value);
    printf("plan.rewrite.steps=%u\n", (unsigned)cert.step_count);
    printf("plan.stats.leaves=%u joins=%u races=%u timeouts=%u\n", (unsigned)leaves,
           (unsigned)joins, (unsigned)races, (unsigned)timeouts);
    return 0;
}

static int scenario_service_stack_walkthrough(void) {
    asx_service base_svc;
    asx_service timeout_svc;
    asx_service wrapped_svc;
    asx_timeout_service_state timeout_state;
    asx_load_shed_service_state load_shed_state;
    demo_service_state state;
    int request = 23;
    int response = 0;
    asx_service_readiness readiness;

    printf("--- scenario: service stack walkthrough ---\n");

    memset(&state, 0, sizeof(state));
    base_svc.poll_ready = demo_poll_ready;
    base_svc.call = demo_call;
    base_svc.state = &state;

    asx_timeout_layer_init(&timeout_svc, &timeout_state, base_svc, 4000);
    asx_load_shed_layer_init(&wrapped_svc, &load_shed_state, timeout_svc);

    readiness = asx_service_poll_ready(&wrapped_svc);
    if (readiness != ASX_SERVICE_NOT_READY ||
        asx_service_call(&wrapped_svc, &request, &response) != ASX_E_WOULD_BLOCK ||
        state.call_count != 0) {
        fprintf(stderr, "service stack should shed while not ready\n");
        return 1;
    }

    state.ready_flag = 1;
    readiness = asx_service_poll_ready(&wrapped_svc);
    if (readiness != ASX_SERVICE_READY ||
        !require_status(asx_service_call(&wrapped_svc, &request, &response), "service call") ||
        response != 123 || state.call_count != 1 || state.last_request != request) {
        fprintf(stderr, "service stack should pass once ready\n");
        return 1;
    }

    printf("service.first_readiness=%d\n", (int)ASX_SERVICE_NOT_READY);
    printf("service.second_readiness=%d\n", (int)readiness);
    printf("service.calls=%d last_request=%d response=%d\n", state.call_count, state.last_request,
           response);
    printf("rerun_hint: rch exec -- make %s\n", "build/tests/vignettes/vignette_plan");
    return 0;
}

int main(void) {
    int failures = 0;

    printf("=== vignette: plan ===\n\n");

    failures += scenario_plan_rewrite_walkthrough();
    failures += scenario_service_stack_walkthrough();

    printf("\n=== plan: %d failures ===\n", failures);
    return failures == 0 ? 0 : 1;
}
