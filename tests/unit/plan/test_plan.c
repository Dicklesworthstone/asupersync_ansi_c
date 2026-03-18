/*
 * test_plan.c — plan DAG, rewrite, certificate, and service layer tests
 *
 * Covers plan IR construction, validation, algebraic rewrite laws,
 * certificate evidence, and service layer composition.
 *
 * Bead: bd-1eqo.13.2
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <string.h>

/* ================================================================== */
/* Plan DAG construction tests                                         */
/* ================================================================== */

TEST(dag_init_empty) {
    asx_plan_dag dag;
    asx_plan_dag_init(&dag);
    ASSERT_EQ(dag.count, 0);
    ASSERT_EQ(dag.root, ASX_PLAN_ID_NONE);
}

TEST(dag_add_leaf) {
    asx_plan_dag dag;
    asx_plan_id id;
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    id = asx_plan_dag_leaf(&dag, "fetch");
    ASSERT_NE(id, ASX_PLAN_ID_NONE);
    ASSERT_EQ(dag.count, 1);

    n = asx_plan_dag_node(&dag, id);
    ASSERT_TRUE(n != NULL);
    ASSERT_EQ(n->kind, ASX_PLAN_LEAF);
    ASSERT_STR_EQ(n->data.leaf.label, "fetch");
}

TEST(dag_add_join) {
    asx_plan_dag dag;
    asx_plan_id a, b, j;
    asx_plan_id children[2];
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    children[0] = a;
    children[1] = b;
    j = asx_plan_dag_join(&dag, children, 2);

    ASSERT_NE(j, ASX_PLAN_ID_NONE);
    n = asx_plan_dag_node(&dag, j);
    ASSERT_EQ(n->kind, ASX_PLAN_JOIN);
    ASSERT_EQ(n->data.group.count, 2);
    ASSERT_EQ(n->data.group.children[0], a);
    ASSERT_EQ(n->data.group.children[1], b);
}

TEST(dag_add_race) {
    asx_plan_dag dag;
    asx_plan_id a, b, r;
    asx_plan_id children[2];

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "fast");
    b = asx_plan_dag_leaf(&dag, "slow");
    children[0] = a;
    children[1] = b;
    r = asx_plan_dag_race(&dag, children, 2);

    ASSERT_NE(r, ASX_PLAN_ID_NONE);
    ASSERT_EQ(asx_plan_dag_node(&dag, r)->kind, ASX_PLAN_RACE);
}

TEST(dag_add_timeout) {
    asx_plan_dag dag;
    asx_plan_id leaf, t;
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    leaf = asx_plan_dag_leaf(&dag, "work");
    t = asx_plan_dag_timeout(&dag, leaf, 5000);

    ASSERT_NE(t, ASX_PLAN_ID_NONE);
    n = asx_plan_dag_node(&dag, t);
    ASSERT_EQ(n->kind, ASX_PLAN_TIMEOUT);
    ASSERT_EQ(n->data.timeout.child, leaf);
    ASSERT_EQ(n->data.timeout.duration_us, 5000u);
}

TEST(dag_set_root) {
    asx_plan_dag dag;
    asx_plan_id leaf;

    asx_plan_dag_init(&dag);
    leaf = asx_plan_dag_leaf(&dag, "root");
    asx_plan_dag_set_root(&dag, leaf);
    ASSERT_EQ(dag.root, leaf);
}

TEST(dag_node_out_of_range) {
    asx_plan_dag dag;
    asx_plan_dag_init(&dag);
    ASSERT_TRUE(asx_plan_dag_node(&dag, 0) == NULL);
    ASSERT_TRUE(asx_plan_dag_node(&dag, 99) == NULL);
}

TEST(dag_empty_children_rejected) {
    asx_plan_dag dag;
    asx_plan_id j;

    asx_plan_dag_init(&dag);
    j = asx_plan_dag_join(&dag, NULL, 0);
    ASSERT_EQ(j, ASX_PLAN_ID_NONE);
}

/* ================================================================== */
/* Plan DAG validation tests                                           */
/* ================================================================== */

TEST(dag_validate_valid) {
    asx_plan_dag dag;
    asx_plan_id a, b, j;
    asx_plan_id children[2];

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    children[0] = a;
    children[1] = b;
    j = asx_plan_dag_join(&dag, children, 2);
    asx_plan_dag_set_root(&dag, j);

    ASSERT_EQ(asx_plan_dag_validate(&dag), ASX_OK);
}

TEST(dag_validate_empty) {
    asx_plan_dag dag;
    asx_plan_dag_init(&dag);
    ASSERT_EQ(asx_plan_dag_validate(&dag), ASX_OK);
}

/* ================================================================== */
/* Plan DAG stats tests                                                */
/* ================================================================== */

TEST(dag_stats) {
    asx_plan_dag dag;
    asx_plan_id a, b, j, t;
    asx_plan_id children[2];
    uint16_t leaves, joins, races, timeouts;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    children[0] = a;
    children[1] = b;
    j = asx_plan_dag_join(&dag, children, 2);
    t = asx_plan_dag_timeout(&dag, j, 1000);
    (void)t;

    asx_plan_dag_stats(&dag, &leaves, &joins, &races, &timeouts);
    ASSERT_EQ(leaves, 2);
    ASSERT_EQ(joins, 1);
    ASSERT_EQ(races, 0);
    ASSERT_EQ(timeouts, 1);
}

/* ================================================================== */
/* Plan hash tests                                                     */
/* ================================================================== */

TEST(hash_deterministic) {
    asx_plan_dag d1, d2;
    asx_plan_id a1, b1, a2, b2;
    asx_plan_id c1[2], c2[2];

    asx_plan_dag_init(&d1);
    a1 = asx_plan_dag_leaf(&d1, "a");
    b1 = asx_plan_dag_leaf(&d1, "b");
    c1[0] = a1;
    c1[1] = b1;
    asx_plan_dag_set_root(&d1, asx_plan_dag_join(&d1, c1, 2));

    asx_plan_dag_init(&d2);
    a2 = asx_plan_dag_leaf(&d2, "a");
    b2 = asx_plan_dag_leaf(&d2, "b");
    c2[0] = a2;
    c2[1] = b2;
    asx_plan_dag_set_root(&d2, asx_plan_dag_join(&d2, c2, 2));

    ASSERT_EQ(asx_plan_hash_compute(&d1).value, asx_plan_hash_compute(&d2).value);
}

TEST(hash_different_labels_differ) {
    asx_plan_dag d1, d2;

    asx_plan_dag_init(&d1);
    asx_plan_dag_leaf(&d1, "alpha");

    asx_plan_dag_init(&d2);
    asx_plan_dag_leaf(&d2, "beta");

    ASSERT_TRUE(asx_plan_hash_compute(&d1).value != asx_plan_hash_compute(&d2).value);
}

TEST(hash_different_structure_differ) {
    asx_plan_dag d1, d2;
    asx_plan_id a, b;
    asx_plan_id children[2];

    /* d1: just a leaf */
    asx_plan_dag_init(&d1);
    asx_plan_dag_leaf(&d1, "a");

    /* d2: join of two leaves */
    asx_plan_dag_init(&d2);
    a = asx_plan_dag_leaf(&d2, "a");
    b = asx_plan_dag_leaf(&d2, "b");
    children[0] = a;
    children[1] = b;
    asx_plan_dag_join(&d2, children, 2);

    ASSERT_TRUE(asx_plan_hash_compute(&d1).value != asx_plan_hash_compute(&d2).value);
}

/* ================================================================== */
/* Rewrite policy tests                                                */
/* ================================================================== */

TEST(policy_conservative_permits_assoc) {
    asx_rewrite_policy p;
    asx_rewrite_policy_conservative(&p);
    ASSERT_TRUE(asx_rewrite_policy_permits(&p, ASX_REWRITE_JOIN_ASSOC));
    ASSERT_TRUE(asx_rewrite_policy_permits(&p, ASX_REWRITE_RACE_ASSOC));
    ASSERT_TRUE(asx_rewrite_policy_permits(&p, ASX_REWRITE_TIMEOUT_MIN));
}

TEST(policy_none_denies_all) {
    asx_rewrite_policy p;
    asx_rewrite_policy_none(&p);
    ASSERT_TRUE(!asx_rewrite_policy_permits(&p, ASX_REWRITE_JOIN_ASSOC));
    ASSERT_TRUE(!asx_rewrite_policy_permits(&p, ASX_REWRITE_TIMEOUT_MIN));
}

/* ================================================================== */
/* Rewrite law tests                                                   */
/* ================================================================== */

TEST(rewrite_join_assoc) {
    /*
     * Join[Join[a,b], c] → Join[a,b,c]
     */
    asx_plan_dag dag;
    asx_plan_id a, b, c, inner_join, outer_join;
    asx_plan_id inner_ch[2], outer_ch[2];
    asx_rewrite_policy pol;
    asx_rewrite_certificate cert;
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    c = asx_plan_dag_leaf(&dag, "c");

    inner_ch[0] = a;
    inner_ch[1] = b;
    inner_join = asx_plan_dag_join(&dag, inner_ch, 2);

    outer_ch[0] = inner_join;
    outer_ch[1] = c;
    outer_join = asx_plan_dag_join(&dag, outer_ch, 2);
    asx_plan_dag_set_root(&dag, outer_join);

    asx_rewrite_policy_conservative(&pol);
    asx_rewrite_certificate_init(&cert);

    ASSERT_EQ(asx_plan_rewrite(&dag, &pol, &cert), ASX_OK);

    /* Outer join should now have 3 children: a, b, c */
    n = asx_plan_dag_node(&dag, outer_join);
    ASSERT_EQ(n->data.group.count, 3);
    ASSERT_EQ(n->data.group.children[0], a);
    ASSERT_EQ(n->data.group.children[1], b);
    ASSERT_EQ(n->data.group.children[2], c);

    /* Certificate should record the step */
    ASSERT_TRUE(!asx_rewrite_certificate_is_identity(&cert));
    ASSERT_TRUE(cert.step_count >= 1);
}

TEST(rewrite_race_assoc) {
    /*
     * Race[Race[a,b], c] → Race[a,b,c]
     */
    asx_plan_dag dag;
    asx_plan_id a, b, c, inner_race, outer_race;
    asx_plan_id inner_ch[2], outer_ch[2];
    asx_rewrite_policy pol;
    asx_rewrite_certificate cert;
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    c = asx_plan_dag_leaf(&dag, "c");

    inner_ch[0] = a;
    inner_ch[1] = b;
    inner_race = asx_plan_dag_race(&dag, inner_ch, 2);

    outer_ch[0] = inner_race;
    outer_ch[1] = c;
    outer_race = asx_plan_dag_race(&dag, outer_ch, 2);
    asx_plan_dag_set_root(&dag, outer_race);

    asx_rewrite_policy_conservative(&pol);
    asx_rewrite_certificate_init(&cert);

    ASSERT_EQ(asx_plan_rewrite(&dag, &pol, &cert), ASX_OK);

    n = asx_plan_dag_node(&dag, outer_race);
    ASSERT_EQ(n->data.group.count, 3);
    ASSERT_TRUE(!asx_rewrite_certificate_is_identity(&cert));
}

TEST(rewrite_timeout_min) {
    /*
     * Timeout[3000, Timeout[1000, leaf]] → Timeout[1000, leaf]
     */
    asx_plan_dag dag;
    asx_plan_id leaf, inner_t, outer_t;
    asx_rewrite_policy pol;
    asx_rewrite_certificate cert;
    const asx_plan_node *n;

    asx_plan_dag_init(&dag);
    leaf = asx_plan_dag_leaf(&dag, "work");
    inner_t = asx_plan_dag_timeout(&dag, leaf, 1000);
    outer_t = asx_plan_dag_timeout(&dag, inner_t, 3000);
    asx_plan_dag_set_root(&dag, outer_t);

    asx_rewrite_policy_conservative(&pol);
    asx_rewrite_certificate_init(&cert);

    ASSERT_EQ(asx_plan_rewrite(&dag, &pol, &cert), ASX_OK);

    /* Outer timeout now points directly to leaf with min duration */
    n = asx_plan_dag_node(&dag, outer_t);
    ASSERT_EQ(n->kind, ASX_PLAN_TIMEOUT);
    ASSERT_EQ(n->data.timeout.child, leaf);
    ASSERT_EQ(n->data.timeout.duration_us, 1000u);
    ASSERT_TRUE(!asx_rewrite_certificate_is_identity(&cert));
}

TEST(rewrite_no_change_when_disabled) {
    asx_plan_dag dag;
    asx_plan_id a, b, inner_join, outer_join;
    asx_plan_id inner_ch[2], outer_ch[2];
    asx_rewrite_policy pol;
    asx_rewrite_certificate cert;
    const asx_plan_node *n;
    asx_plan_id c;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    c = asx_plan_dag_leaf(&dag, "c");

    inner_ch[0] = a;
    inner_ch[1] = b;
    inner_join = asx_plan_dag_join(&dag, inner_ch, 2);

    outer_ch[0] = inner_join;
    outer_ch[1] = c;
    outer_join = asx_plan_dag_join(&dag, outer_ch, 2);

    asx_rewrite_policy_none(&pol);
    asx_rewrite_certificate_init(&cert);

    ASSERT_EQ(asx_plan_rewrite(&dag, &pol, &cert), ASX_OK);

    /* Should be unchanged: still 2 children in outer join */
    n = asx_plan_dag_node(&dag, outer_join);
    ASSERT_EQ(n->data.group.count, 2);
    ASSERT_TRUE(asx_rewrite_certificate_is_identity(&cert));
}

TEST(rewrite_preserves_hash_change) {
    asx_plan_dag dag;
    asx_plan_id a, b, c, inner, outer;
    asx_plan_id i_ch[2], o_ch[2];
    asx_rewrite_policy pol;
    asx_rewrite_certificate cert;

    asx_plan_dag_init(&dag);
    a = asx_plan_dag_leaf(&dag, "a");
    b = asx_plan_dag_leaf(&dag, "b");
    c = asx_plan_dag_leaf(&dag, "c");

    i_ch[0] = a;
    i_ch[1] = b;
    inner = asx_plan_dag_join(&dag, i_ch, 2);

    o_ch[0] = inner;
    o_ch[1] = c;
    outer = asx_plan_dag_join(&dag, o_ch, 2);
    asx_plan_dag_set_root(&dag, outer);

    asx_rewrite_policy_conservative(&pol);
    asx_rewrite_certificate_init(&cert);
    ASSERT_EQ(asx_plan_rewrite(&dag, &pol, &cert), ASX_OK);

    /* Before and after hashes should differ after rewrite */
    ASSERT_TRUE(cert.before_hash.value != cert.after_hash.value);
}

/* ================================================================== */
/* Certificate tests                                                   */
/* ================================================================== */

TEST(cert_init_is_identity) {
    asx_rewrite_certificate cert;
    asx_rewrite_certificate_init(&cert);
    ASSERT_TRUE(asx_rewrite_certificate_is_identity(&cert));
    ASSERT_EQ(cert.step_count, 0);
}

TEST(cert_record_step) {
    asx_rewrite_certificate cert;
    asx_rewrite_certificate_init(&cert);
    ASSERT_EQ(asx_rewrite_certificate_record(&cert, ASX_REWRITE_JOIN_ASSOC, 3, 3), ASX_OK);
    ASSERT_EQ(cert.step_count, 1);
    ASSERT_TRUE(!asx_rewrite_certificate_is_identity(&cert));
}

TEST(cert_fingerprint_deterministic) {
    asx_rewrite_certificate c1, c2;

    asx_rewrite_certificate_init(&c1);
    asx_rewrite_certificate_init(&c2);

    c1.before_hash.value = 42;
    c1.after_hash.value = 99;
    c2.before_hash.value = 42;
    c2.after_hash.value = 99;

    ASSERT_EQ(asx_rewrite_certificate_record(&c1, ASX_REWRITE_JOIN_ASSOC, 1, 1), ASX_OK);
    ASSERT_EQ(asx_rewrite_certificate_record(&c2, ASX_REWRITE_JOIN_ASSOC, 1, 1), ASX_OK);

    ASSERT_EQ(asx_rewrite_certificate_fingerprint(&c1), asx_rewrite_certificate_fingerprint(&c2));
}

TEST(cert_different_steps_differ) {
    asx_rewrite_certificate c1, c2;

    asx_rewrite_certificate_init(&c1);
    asx_rewrite_certificate_init(&c2);

    ASSERT_EQ(asx_rewrite_certificate_record(&c1, ASX_REWRITE_JOIN_ASSOC, 1, 1), ASX_OK);
    ASSERT_EQ(asx_rewrite_certificate_record(&c2, ASX_REWRITE_TIMEOUT_MIN, 1, 1), ASX_OK);

    ASSERT_TRUE(asx_rewrite_certificate_fingerprint(&c1) !=
                asx_rewrite_certificate_fingerprint(&c2));
}

/* ================================================================== */
/* Service layer tests                                                 */
/* ================================================================== */

/* Simple echo service for testing */
static int echo_ready_flag = 1;

static asx_service_readiness echo_poll_ready(void *state) {
    (void)state;
    return echo_ready_flag ? ASX_SERVICE_READY : ASX_SERVICE_NOT_READY;
}

static asx_status echo_call(void *state, const void *request, void *response) {
    (void)state;
    /* Echo: copy int from request to response */
    *(int *)response = *(const int *)request;
    return ASX_OK;
}

typedef struct {
    int ready_flag;
    int call_count;
    int bias;
} service_stack_state;

typedef struct {
    int failures_remaining;
    int call_count;
    asx_status fail_status;
} flaky_service_state;

static asx_service_readiness stack_poll_ready(void *state) {
    service_stack_state *stack = (service_stack_state *)state;
    return stack->ready_flag ? ASX_SERVICE_READY : ASX_SERVICE_NOT_READY;
}

static asx_status stack_call(void *state, const void *request, void *response) {
    const int *req = (const int *)request;
    int *resp = (int *)response;
    service_stack_state *stack = (service_stack_state *)state;

    stack->call_count++;
    *resp = *req + stack->bias;
    return ASX_OK;
}

static asx_service_readiness flaky_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status flaky_call(void *state, const void *request, void *response) {
    flaky_service_state *flaky = (flaky_service_state *)state;

    flaky->call_count++;
    if (flaky->failures_remaining > 0) {
        flaky->failures_remaining--;
        return flaky->fail_status;
    }

    *(int *)response = *(const int *)request + flaky->call_count;
    return ASX_OK;
}

TEST(service_basic_call) {
    asx_service svc;
    int req = 42, resp = 0;

    svc.poll_ready = echo_poll_ready;
    svc.call = echo_call;
    svc.state = NULL;

    echo_ready_flag = 1;
    ASSERT_EQ(asx_service_poll_ready(&svc), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&svc, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 42);
}

TEST(service_timeout_layer) {
    asx_service inner, wrapped;
    asx_timeout_service_state ts;
    int req = 7, resp = 0;

    inner.poll_ready = echo_poll_ready;
    inner.call = echo_call;
    inner.state = NULL;

    echo_ready_flag = 1;
    asx_timeout_layer_init(&wrapped, &ts, inner, 5000);

    ASSERT_EQ(asx_service_poll_ready(&wrapped), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 7);
}

TEST(service_timeout_rejects_without_ready) {
    asx_service inner, wrapped;
    asx_timeout_service_state ts;
    int req = 1, resp = 0;

    inner.poll_ready = echo_poll_ready;
    inner.call = echo_call;
    inner.state = NULL;

    echo_ready_flag = 1;
    asx_timeout_layer_init(&wrapped, &ts, inner, 5000);

    /* Call without polling ready first should fail */
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_INVALID_STATE);
}

TEST(service_load_shed_rejects_when_not_ready) {
    asx_service inner, wrapped;
    asx_load_shed_service_state ls;
    int req = 1, resp = 0;

    inner.poll_ready = echo_poll_ready;
    inner.call = echo_call;
    inner.state = NULL;

    echo_ready_flag = 0;
    asx_load_shed_layer_init(&wrapped, &ls, inner);

    /* Inner not ready → shed load */
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_WOULD_BLOCK);
}

TEST(service_load_shed_passes_when_ready) {
    asx_service inner, wrapped;
    asx_load_shed_service_state ls;
    int req = 99, resp = 0;

    inner.poll_ready = echo_poll_ready;
    inner.call = echo_call;
    inner.state = NULL;

    echo_ready_flag = 1;
    asx_load_shed_layer_init(&wrapped, &ls, inner);

    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 99);
}

TEST(service_composition_timeout_over_load_shed) {
    /*
     * Compose: Timeout(LoadShed(echo))
     * Verifies multi-layer stacking works.
     */
    asx_service echo_svc, shed_svc, timeout_svc;
    asx_load_shed_service_state ls;
    asx_timeout_service_state ts;
    int req = 55, resp = 0;

    echo_svc.poll_ready = echo_poll_ready;
    echo_svc.call = echo_call;
    echo_svc.state = NULL;
    echo_ready_flag = 1;

    asx_load_shed_layer_init(&shed_svc, &ls, echo_svc);
    asx_timeout_layer_init(&timeout_svc, &ts, shed_svc, 1000);

    ASSERT_EQ(asx_service_poll_ready(&timeout_svc), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&timeout_svc, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 55);
}

TEST(service_composition_load_shed_over_timeout_transitions) {
    asx_service base_svc, timeout_svc, wrapped_svc;
    asx_timeout_service_state ts;
    asx_load_shed_service_state ls;
    service_stack_state state;
    int req = 12, resp = 0;

    memset(&state, 0, sizeof(state));
    state.bias = 5;

    base_svc.poll_ready = stack_poll_ready;
    base_svc.call = stack_call;
    base_svc.state = &state;

    asx_timeout_layer_init(&timeout_svc, &ts, base_svc, 2500);
    asx_load_shed_layer_init(&wrapped_svc, &ls, timeout_svc);

    state.ready_flag = 0;
    ASSERT_EQ(asx_service_poll_ready(&wrapped_svc), ASX_SERVICE_NOT_READY);
    ASSERT_EQ(asx_service_call(&wrapped_svc, &req, &resp), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(state.call_count, 0);

    state.ready_flag = 1;
    ASSERT_EQ(asx_service_poll_ready(&wrapped_svc), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&wrapped_svc, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 17);
    ASSERT_EQ(state.call_count, 1);
}

TEST(service_retry_retries_retryable_status) {
    asx_service base, wrapped;
    asx_retry_service_state retry;
    flaky_service_state flaky;
    int req = 10, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 2;
    flaky.fail_status = ASX_E_WOULD_BLOCK;

    base.poll_ready = flaky_poll_ready;
    base.call = flaky_call;
    base.state = &flaky;

    asx_retry_layer_init(&wrapped, &retry, base, 4u);

    ASSERT_EQ(asx_service_poll_ready(&wrapped), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 13);
    ASSERT_EQ(flaky.call_count, 3);
    ASSERT_EQ(asx_retry_layer_last_attempts(&retry), 3);
    ASSERT_EQ(asx_retry_layer_last_status(&retry), ASX_OK);
}

TEST(service_retry_stops_on_permanent_status) {
    asx_service base, wrapped;
    asx_retry_service_state retry;
    flaky_service_state flaky;
    int req = 2, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 3;
    flaky.fail_status = ASX_E_PERMISSION_DENIED;

    base.poll_ready = flaky_poll_ready;
    base.call = flaky_call;
    base.state = &flaky;

    asx_retry_layer_init(&wrapped, &retry, base, 5u);

    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(flaky.call_count, 1);
    ASSERT_EQ(asx_retry_layer_last_attempts(&retry), 1);
    ASSERT_EQ(asx_retry_layer_last_status(&retry), ASX_E_PERMISSION_DENIED);
}

TEST(service_retry_respects_attempt_budget) {
    asx_service base, wrapped;
    asx_retry_service_state retry;
    flaky_service_state flaky;
    int req = 4, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 5;
    flaky.fail_status = ASX_E_WOULD_BLOCK;

    base.poll_ready = flaky_poll_ready;
    base.call = flaky_call;
    base.state = &flaky;

    asx_retry_layer_init(&wrapped, &retry, base, 3u);

    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(flaky.call_count, 3);
    ASSERT_EQ(asx_retry_layer_last_attempts(&retry), 3);
    ASSERT_EQ(asx_retry_layer_last_status(&retry), ASX_E_WOULD_BLOCK);
}

TEST(service_retry_composes_with_timeout_and_load_shed) {
    asx_service base_svc, retry_svc, timeout_svc, wrapped_svc;
    asx_retry_service_state retry;
    asx_timeout_service_state timeout;
    asx_load_shed_service_state load_shed;
    flaky_service_state flaky;
    int req = 20, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 1;
    flaky.fail_status = ASX_E_WOULD_BLOCK;

    base_svc.poll_ready = flaky_poll_ready;
    base_svc.call = flaky_call;
    base_svc.state = &flaky;

    asx_retry_layer_init(&retry_svc, &retry, base_svc, 2u);
    asx_timeout_layer_init(&timeout_svc, &timeout, retry_svc, 250u);
    asx_load_shed_layer_init(&wrapped_svc, &load_shed, timeout_svc);

    ASSERT_EQ(asx_service_poll_ready(&wrapped_svc), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&wrapped_svc, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 22);
    ASSERT_EQ(flaky.call_count, 2);
    ASSERT_EQ(asx_retry_layer_last_attempts(&retry), 2);
}

TEST(service_breaker_allows_successful_calls) {
    asx_service base, wrapped;
    asx_breaker_service_state breaker;
    int req = 8, resp = 0;

    base.poll_ready = echo_poll_ready;
    base.call = echo_call;
    base.state = NULL;

    echo_ready_flag = 1;
    ASSERT_EQ(asx_breaker_layer_init(&wrapped, &breaker, base, NULL), ASX_OK);
    ASSERT_EQ(asx_service_poll_ready(&wrapped), ASX_SERVICE_READY);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 8);
    ASSERT_EQ((int)asx_breaker_layer_state(&breaker), (int)ASX_BREAKER_CLOSED);
    ASSERT_EQ(asx_breaker_layer_last_status(&breaker), ASX_OK);
}

TEST(service_breaker_trips_and_rejects_after_failures) {
    asx_service base, wrapped;
    asx_breaker_service_state breaker;
    flaky_service_state flaky;
    asx_breaker_config cfg;
    int req = 5, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 2;
    flaky.fail_status = ASX_E_PERMISSION_DENIED;

    cfg.failure_threshold = 2u;
    cfg.success_threshold = 1u;
    cfg.half_open_max_calls = 1u;

    base.poll_ready = flaky_poll_ready;
    base.call = flaky_call;
    base.state = &flaky;

    ASSERT_EQ(asx_breaker_layer_init(&wrapped, &breaker, base, &cfg), ASX_OK);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ((int)asx_breaker_layer_state(&breaker), (int)ASX_BREAKER_OPEN);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_ADMISSION_CLOSED);
    ASSERT_EQ(asx_breaker_layer_last_status(&breaker), ASX_E_ADMISSION_CLOSED);
}

TEST(service_breaker_half_open_recovery_closes_circuit) {
    asx_service base, wrapped;
    asx_breaker_service_state breaker;
    flaky_service_state flaky;
    asx_breaker_config cfg;
    int req = 6, resp = 0;

    memset(&flaky, 0, sizeof(flaky));
    flaky.failures_remaining = 1;
    flaky.fail_status = ASX_E_PERMISSION_DENIED;

    cfg.failure_threshold = 1u;
    cfg.success_threshold = 1u;
    cfg.half_open_max_calls = 1u;

    base.poll_ready = flaky_poll_ready;
    base.call = flaky_call;
    base.state = &flaky;

    ASSERT_EQ(asx_breaker_layer_init(&wrapped, &breaker, base, &cfg), ASX_OK);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ((int)asx_breaker_layer_state(&breaker), (int)ASX_BREAKER_OPEN);
    ASSERT_EQ(asx_breaker_half_open(&breaker.breaker), ASX_OK);
    ASSERT_EQ(asx_service_call(&wrapped, &req, &resp), ASX_OK);
    ASSERT_EQ(resp, 8);
    ASSERT_EQ((int)asx_breaker_layer_state(&breaker), (int)ASX_BREAKER_CLOSED);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    /* DAG construction */
    RUN_TEST(dag_init_empty);
    RUN_TEST(dag_add_leaf);
    RUN_TEST(dag_add_join);
    RUN_TEST(dag_add_race);
    RUN_TEST(dag_add_timeout);
    RUN_TEST(dag_set_root);
    RUN_TEST(dag_node_out_of_range);
    RUN_TEST(dag_empty_children_rejected);

    /* DAG validation */
    RUN_TEST(dag_validate_valid);
    RUN_TEST(dag_validate_empty);

    /* DAG stats */
    RUN_TEST(dag_stats);

    /* Plan hash */
    RUN_TEST(hash_deterministic);
    RUN_TEST(hash_different_labels_differ);
    RUN_TEST(hash_different_structure_differ);

    /* Rewrite policy */
    RUN_TEST(policy_conservative_permits_assoc);
    RUN_TEST(policy_none_denies_all);

    /* Rewrite laws */
    RUN_TEST(rewrite_join_assoc);
    RUN_TEST(rewrite_race_assoc);
    RUN_TEST(rewrite_timeout_min);
    RUN_TEST(rewrite_no_change_when_disabled);
    RUN_TEST(rewrite_preserves_hash_change);

    /* Certificates */
    RUN_TEST(cert_init_is_identity);
    RUN_TEST(cert_record_step);
    RUN_TEST(cert_fingerprint_deterministic);
    RUN_TEST(cert_different_steps_differ);

    /* Service layers */
    RUN_TEST(service_basic_call);
    RUN_TEST(service_timeout_layer);
    RUN_TEST(service_timeout_rejects_without_ready);
    RUN_TEST(service_load_shed_rejects_when_not_ready);
    RUN_TEST(service_load_shed_passes_when_ready);
    RUN_TEST(service_composition_timeout_over_load_shed);
    RUN_TEST(service_composition_load_shed_over_timeout_transitions);
    RUN_TEST(service_retry_retries_retryable_status);
    RUN_TEST(service_retry_stops_on_permanent_status);
    RUN_TEST(service_retry_respects_attempt_budget);
    RUN_TEST(service_retry_composes_with_timeout_and_load_shed);
    RUN_TEST(service_breaker_allows_successful_calls);
    RUN_TEST(service_breaker_trips_and_rejects_after_failures);
    RUN_TEST(service_breaker_half_open_recovery_closes_circuit);

    TEST_REPORT();
    return test_failures;
}
