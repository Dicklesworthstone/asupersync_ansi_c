/*
 * plan.c — plan DAG, algebraic rewrite, certificates, and service layers
 *
 * Implements concurrency plan IR, policy-controlled rewrites with
 * certificate evidence, and a lightweight service layer trait.
 *
 * Bead: bd-1eqo.13.2
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/plan/plan.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* FNV-1a constants                                                    */
/* ------------------------------------------------------------------ */

#define FNV_OFFSET UINT64_C(0xcbf29ce484222325)
#define FNV_PRIME UINT64_C(0x00000100000001B3)

static uint64_t fnv_hash_byte(uint64_t hash, uint8_t byte) { return (hash ^ byte) * FNV_PRIME; }

static uint64_t fnv_hash_u16(uint64_t hash, uint16_t v) {
    hash = fnv_hash_byte(hash, (uint8_t)(v & 0xFF));
    hash = fnv_hash_byte(hash, (uint8_t)(v >> 8));
    return hash;
}

static uint64_t fnv_hash_u64(uint64_t hash, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8; i++) {
        hash = fnv_hash_byte(hash, (uint8_t)(v & 0xFF));
        v >>= 8;
    }
    return hash;
}

static uint64_t fnv_hash_str(uint64_t hash, const char *s) {
    while (*s) {
        hash = fnv_hash_byte(hash, (uint8_t)*s);
        s++;
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/* Plan DAG                                                            */
/* ------------------------------------------------------------------ */

void asx_plan_dag_init(asx_plan_dag *dag) {
    memset(dag, 0, sizeof(*dag));
    dag->root = ASX_PLAN_ID_NONE;
}

asx_plan_id asx_plan_dag_leaf(asx_plan_dag *dag, const char *label) {
    asx_plan_id id;
    asx_plan_node *n;
    size_t len;

    if (dag->count >= ASX_PLAN_MAX_NODES) return ASX_PLAN_ID_NONE;

    id = dag->count;
    dag->count++;
    n = &dag->nodes[id];
    n->kind = ASX_PLAN_LEAF;
    memset(n->data.leaf.label, 0, ASX_PLAN_MAX_LABEL);
    if (label) {
        len = strlen(label);
        if (len >= ASX_PLAN_MAX_LABEL) len = ASX_PLAN_MAX_LABEL - 1;
        memcpy(n->data.leaf.label, label, len);
    }
    return id;
}

static asx_plan_id add_group(asx_plan_dag *dag, asx_plan_node_kind kind,
                             const asx_plan_id *children, uint8_t count) {
    asx_plan_id id;
    asx_plan_node *n;
    uint8_t i;

    if (dag->count >= ASX_PLAN_MAX_NODES) return ASX_PLAN_ID_NONE;
    if (count == 0 || count > ASX_PLAN_MAX_CHILDREN) return ASX_PLAN_ID_NONE;

    id = dag->count;
    dag->count++;
    n = &dag->nodes[id];
    n->kind = kind;
    n->data.group.count = count;
    for (i = 0; i < count; i++) { n->data.group.children[i] = children[i]; }
    return id;
}

asx_plan_id asx_plan_dag_join(asx_plan_dag *dag, const asx_plan_id *children, uint8_t count) {
    return add_group(dag, ASX_PLAN_JOIN, children, count);
}

asx_plan_id asx_plan_dag_race(asx_plan_dag *dag, const asx_plan_id *children, uint8_t count) {
    return add_group(dag, ASX_PLAN_RACE, children, count);
}

asx_plan_id asx_plan_dag_timeout(asx_plan_dag *dag, asx_plan_id child, uint64_t duration_us) {
    asx_plan_id id;
    asx_plan_node *n;

    if (dag->count >= ASX_PLAN_MAX_NODES) return ASX_PLAN_ID_NONE;

    id = dag->count;
    dag->count++;
    n = &dag->nodes[id];
    n->kind = ASX_PLAN_TIMEOUT;
    n->data.timeout.child = child;
    n->data.timeout.duration_us = duration_us;
    return id;
}

void asx_plan_dag_set_root(asx_plan_dag *dag, asx_plan_id root) { dag->root = root; }

const asx_plan_node *asx_plan_dag_node(const asx_plan_dag *dag, asx_plan_id id) {
    if (id >= dag->count) return NULL;
    return &dag->nodes[id];
}

asx_status asx_plan_dag_validate(const asx_plan_dag *dag) {
    uint16_t i;
    uint8_t j;

    for (i = 0; i < dag->count; i++) {
        const asx_plan_node *n = &dag->nodes[i];
        switch (n->kind) {
        case ASX_PLAN_LEAF: break;
        case ASX_PLAN_JOIN:
        case ASX_PLAN_RACE:
            if (n->data.group.count == 0) return ASX_E_INVALID_ARGUMENT;
            for (j = 0; j < n->data.group.count; j++) {
                if (n->data.group.children[j] >= dag->count) return ASX_E_NOT_FOUND;
                /* Simple forward-reference-only check (no back edges) */
                if (n->data.group.children[j] >= i) return ASX_E_INVALID_STATE;
            }
            break;
        case ASX_PLAN_TIMEOUT:
            if (n->data.timeout.child >= dag->count) return ASX_E_NOT_FOUND;
            if (n->data.timeout.child >= i) return ASX_E_INVALID_STATE;
            break;
        }
    }
    return ASX_OK;
}

void asx_plan_dag_stats(const asx_plan_dag *dag, uint16_t *out_leaves, uint16_t *out_joins,
                        uint16_t *out_races, uint16_t *out_timeouts) {
    uint16_t leaves = 0, joins = 0, races = 0, timeouts = 0;
    uint16_t i;

    for (i = 0; i < dag->count; i++) {
        switch (dag->nodes[i].kind) {
        case ASX_PLAN_LEAF: leaves++; break;
        case ASX_PLAN_JOIN: joins++; break;
        case ASX_PLAN_RACE: races++; break;
        case ASX_PLAN_TIMEOUT: timeouts++; break;
        }
    }

    if (out_leaves) *out_leaves = leaves;
    if (out_joins) *out_joins = joins;
    if (out_races) *out_races = races;
    if (out_timeouts) *out_timeouts = timeouts;
}

/* ------------------------------------------------------------------ */
/* Plan hash                                                           */
/* ------------------------------------------------------------------ */

static uint64_t hash_node(uint64_t h, const asx_plan_node *n) {
    uint8_t j;

    h = fnv_hash_byte(h, (uint8_t)n->kind);
    switch (n->kind) {
    case ASX_PLAN_LEAF: h = fnv_hash_str(h, n->data.leaf.label); break;
    case ASX_PLAN_JOIN:
    case ASX_PLAN_RACE:
        h = fnv_hash_byte(h, n->data.group.count);
        for (j = 0; j < n->data.group.count; j++) {
            h = fnv_hash_u16(h, n->data.group.children[j]);
        }
        break;
    case ASX_PLAN_TIMEOUT:
        h = fnv_hash_u16(h, n->data.timeout.child);
        h = fnv_hash_u64(h, n->data.timeout.duration_us);
        break;
    }
    return h;
}

asx_plan_hash asx_plan_hash_compute(const asx_plan_dag *dag) {
    asx_plan_hash ph;
    uint64_t h = FNV_OFFSET;
    uint16_t i;

    h = fnv_hash_u16(h, dag->count);
    h = fnv_hash_u16(h, dag->root);

    for (i = 0; i < dag->count; i++) { h = hash_node(h, &dag->nodes[i]); }

    ph.value = h;
    return ph;
}

/* ------------------------------------------------------------------ */
/* Rewrite policy                                                      */
/* ------------------------------------------------------------------ */

void asx_rewrite_policy_conservative(asx_rewrite_policy *p) {
    p->associativity = 1;
    p->timeout_simplification = 1;
}

void asx_rewrite_policy_none(asx_rewrite_policy *p) {
    p->associativity = 0;
    p->timeout_simplification = 0;
}

int asx_rewrite_policy_permits(const asx_rewrite_policy *p, asx_rewrite_rule rule) {
    switch (rule) {
    case ASX_REWRITE_JOIN_ASSOC:
    case ASX_REWRITE_RACE_ASSOC: return p->associativity;
    case ASX_REWRITE_TIMEOUT_MIN: return p->timeout_simplification;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rewrite certificate                                                 */
/* ------------------------------------------------------------------ */

void asx_rewrite_certificate_init(asx_rewrite_certificate *cert) { memset(cert, 0, sizeof(*cert)); }

asx_status asx_rewrite_certificate_record(asx_rewrite_certificate *cert, asx_rewrite_rule rule,
                                          asx_plan_id before, asx_plan_id after) {
    if (cert->step_count >= ASX_PLAN_MAX_STEPS) return ASX_E_RESOURCE_EXHAUSTED;

    cert->steps[cert->step_count].rule = rule;
    cert->steps[cert->step_count].before = before;
    cert->steps[cert->step_count].after = after;
    cert->step_count++;
    return ASX_OK;
}

int asx_rewrite_certificate_is_identity(const asx_rewrite_certificate *cert) {
    return cert->step_count == 0;
}

uint64_t asx_rewrite_certificate_fingerprint(const asx_rewrite_certificate *cert) {
    uint64_t h = FNV_OFFSET;
    uint16_t i;

    h = fnv_hash_u64(h, cert->before_hash.value);
    h = fnv_hash_u64(h, cert->after_hash.value);
    h = fnv_hash_u16(h, cert->step_count);

    for (i = 0; i < cert->step_count; i++) {
        h = fnv_hash_byte(h, (uint8_t)cert->steps[i].rule);
        h = fnv_hash_u16(h, cert->steps[i].before);
        h = fnv_hash_u16(h, cert->steps[i].after);
    }

    return h;
}

/* ------------------------------------------------------------------ */
/* Rewrite engine                                                      */
/* ------------------------------------------------------------------ */

/*
 * Flatten nested Join/Race nodes:
 * Join[Join[a,b], c] → Join[a,b,c]
 * Race[Race[a,b], c] → Race[a,b,c]
 */
static int flatten_group(asx_plan_dag *dag, asx_plan_id id, asx_plan_node_kind kind,
                         asx_rewrite_rule rule, asx_rewrite_certificate *cert) {
    asx_plan_node *n = &dag->nodes[id];
    asx_plan_id new_children[ASX_PLAN_MAX_CHILDREN];
    uint8_t new_count = 0;
    uint8_t i;
    int changed = 0;

    if (n->kind != kind) return 0;

    for (i = 0; i < n->data.group.count; i++) {
        asx_plan_id child_id = n->data.group.children[i];
        const asx_plan_node *child;

        if (child_id >= dag->count) continue;
        child = &dag->nodes[child_id];

        if (child->kind == kind) {
            /* Flatten: pull grandchildren up */
            uint8_t j;
            for (j = 0; j < child->data.group.count; j++) {
                if (new_count >= ASX_PLAN_MAX_CHILDREN) return changed;
                new_children[new_count++] = child->data.group.children[j];
            }
            changed = 1;
        } else {
            if (new_count >= ASX_PLAN_MAX_CHILDREN) return changed;
            new_children[new_count++] = child_id;
        }
    }

    if (changed) {
        asx_status st = asx_rewrite_certificate_record(cert, rule, id, id);
        (void)st;
        n->data.group.count = new_count;
        for (i = 0; i < new_count; i++) { n->data.group.children[i] = new_children[i]; }
    }

    return changed;
}

/*
 * Collapse nested timeouts:
 * Timeout[d1, Timeout[d2, f]] → Timeout[min(d1,d2), f]
 */
static int collapse_timeout(asx_plan_dag *dag, asx_plan_id id, asx_rewrite_certificate *cert) {
    asx_plan_node *n = &dag->nodes[id];
    const asx_plan_node *child;
    uint64_t min_dur;
    asx_status st;

    if (n->kind != ASX_PLAN_TIMEOUT) return 0;
    if (n->data.timeout.child >= dag->count) return 0;

    child = &dag->nodes[n->data.timeout.child];
    if (child->kind != ASX_PLAN_TIMEOUT) return 0;

    /* Collapse: take the tighter deadline */
    min_dur = n->data.timeout.duration_us;
    if (child->data.timeout.duration_us < min_dur) { min_dur = child->data.timeout.duration_us; }

    st = asx_rewrite_certificate_record(cert, ASX_REWRITE_TIMEOUT_MIN, id, id);
    (void)st;

    n->data.timeout.child = child->data.timeout.child;
    n->data.timeout.duration_us = min_dur;

    return 1;
}

asx_status asx_plan_rewrite(asx_plan_dag *dag, const asx_rewrite_policy *policy,
                            asx_rewrite_certificate *cert) {
    int changed;
    uint16_t i;
    int pass;

    cert->policy = *policy;
    cert->before_hash = asx_plan_hash_compute(dag);
    cert->before_count = dag->count;

    /* Fixed-point iteration: apply rules until no changes */
    for (pass = 0; pass < 16; pass++) {
        changed = 0;

        for (i = 0; i < dag->count; i++) {
            if (policy->associativity) {
                changed |= flatten_group(dag, i, ASX_PLAN_JOIN, ASX_REWRITE_JOIN_ASSOC, cert);
                changed |= flatten_group(dag, i, ASX_PLAN_RACE, ASX_REWRITE_RACE_ASSOC, cert);
            }
            if (policy->timeout_simplification) { changed |= collapse_timeout(dag, i, cert); }
        }

        if (!changed) break;
    }

    cert->after_hash = asx_plan_hash_compute(dag);
    cert->after_count = dag->count;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Service trait                                                        */
/* ------------------------------------------------------------------ */

asx_service_readiness asx_service_poll_ready(asx_service *svc) {
    return svc->poll_ready(svc->state);
}

asx_status asx_service_call(asx_service *svc, const void *request, void *response) {
    return svc->call(svc->state, request, response);
}

void asx_layer_apply(const asx_layer *layer, asx_service *inner, asx_service *out) {
    layer->apply(layer->state, inner, out);
}

/* ------------------------------------------------------------------ */
/* Timeout layer                                                       */
/* ------------------------------------------------------------------ */

static asx_service_readiness timeout_poll_ready(void *state) {
    asx_timeout_service_state *ts = (asx_timeout_service_state *)state;
    asx_service_readiness r = asx_service_poll_ready(&ts->inner);
    if (r == ASX_SERVICE_READY) { ts->ready = 1; }
    return r;
}

static asx_status timeout_call(void *state, const void *request, void *response) {
    asx_timeout_service_state *ts = (asx_timeout_service_state *)state;

    if (!ts->ready) return ASX_E_INVALID_STATE;
    ts->ready = 0;

    /* In a real async runtime, this would set up a deadline.
     * For the synchronous C port, we delegate directly to inner. */
    return asx_service_call(&ts->inner, request, response);
}

void asx_timeout_layer_init(asx_service *out, asx_timeout_service_state *state, asx_service inner,
                            uint64_t duration_us) {
    state->inner = inner;
    state->duration_us = duration_us;
    state->deadline = 0;
    state->ready = 0;
    out->poll_ready = timeout_poll_ready;
    out->call = timeout_call;
    out->state = state;
}

/* ------------------------------------------------------------------ */
/* Load-shed layer                                                     */
/* ------------------------------------------------------------------ */

static asx_service_readiness load_shed_poll_ready(void *state) {
    asx_load_shed_service_state *ls = (asx_load_shed_service_state *)state;
    return asx_service_poll_ready(&ls->inner);
}

static asx_status load_shed_call(void *state, const void *request, void *response) {
    asx_load_shed_service_state *ls = (asx_load_shed_service_state *)state;
    asx_service_readiness r = asx_service_poll_ready(&ls->inner);

    if (r != ASX_SERVICE_READY) { return ASX_E_WOULD_BLOCK; /* Shed load: reject immediately */ }

    return asx_service_call(&ls->inner, request, response);
}

void asx_load_shed_layer_init(asx_service *out, asx_load_shed_service_state *state,
                              asx_service inner) {
    state->inner = inner;
    out->poll_ready = load_shed_poll_ready;
    out->call = load_shed_call;
    out->state = state;
}

/* ------------------------------------------------------------------ */
/* Retry layer                                                         */
/* ------------------------------------------------------------------ */

static asx_service_readiness retry_poll_ready(void *state) {
    asx_retry_service_state *rs = (asx_retry_service_state *)state;
    return asx_service_poll_ready(&rs->inner);
}

static asx_status retry_call(void *state, const void *request, void *response) {
    asx_retry_service_state *rs = (asx_retry_service_state *)state;
    uint8_t attempts;
    asx_status st;

    attempts = 0;
    st = ASX_E_INVALID_STATE;

    while (attempts < rs->max_attempts) {
        attempts++;
        st = asx_service_call(&rs->inner, request, response);
        if (st == ASX_OK || !asx_status_is_retryable(st)) { break; }
    }

    rs->last_attempts = attempts;
    rs->last_status = st;
    return st;
}

void asx_retry_layer_init(asx_service *out, asx_retry_service_state *state, asx_service inner,
                          uint8_t max_attempts) {
    state->inner = inner;
    state->max_attempts = (max_attempts == 0u) ? 1u : max_attempts;
    state->last_attempts = 0u;
    state->last_status = ASX_OK;
    out->poll_ready = retry_poll_ready;
    out->call = retry_call;
    out->state = state;
}

uint8_t asx_retry_layer_last_attempts(const asx_retry_service_state *state) {
    return state->last_attempts;
}

asx_status asx_retry_layer_last_status(const asx_retry_service_state *state) {
    return state->last_status;
}

/* ------------------------------------------------------------------ */
/* Circuit-breaker layer                                               */
/* ------------------------------------------------------------------ */

static asx_service_readiness breaker_poll_ready(void *state) {
    asx_breaker_service_state *bs = (asx_breaker_service_state *)state;
    asx_status allow;

    allow = asx_breaker_allow(&bs->breaker);
    if (allow != ASX_OK) { return ASX_SERVICE_NOT_READY; }

    return asx_service_poll_ready(&bs->inner);
}

static asx_status breaker_call(void *state, const void *request, void *response) {
    asx_breaker_service_state *bs = (asx_breaker_service_state *)state;
    asx_status st;

    st = asx_breaker_allow(&bs->breaker);
    if (st != ASX_OK) {
        bs->last_status = st;
        return st;
    }

    st = asx_service_call(&bs->inner, request, response);
    if (st == ASX_OK) {
        asx_breaker_record_success(&bs->breaker);
    } else {
        asx_breaker_record_failure(&bs->breaker);
    }

    bs->last_status = st;
    return st;
}

asx_status asx_breaker_layer_init(asx_service *out, asx_breaker_service_state *state,
                                  asx_service inner, const asx_breaker_config *config) {
    asx_status st;

    if (out == NULL || state == NULL) { return ASX_E_INVALID_ARGUMENT; }

    if (config == NULL) {
        asx_breaker_init(&state->breaker);
    } else {
        st = asx_breaker_init_with(&state->breaker, config);
        if (st != ASX_OK) { return st; }
    }

    state->inner = inner;
    state->last_status = ASX_OK;
    out->poll_ready = breaker_poll_ready;
    out->call = breaker_call;
    out->state = state;
    return ASX_OK;
}

asx_status asx_breaker_layer_last_status(const asx_breaker_service_state *state) {
    return state->last_status;
}

asx_breaker_state asx_breaker_layer_state(const asx_breaker_service_state *state) {
    return asx_breaker_get_state(&state->breaker);
}
