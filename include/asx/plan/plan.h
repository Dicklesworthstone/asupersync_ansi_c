/*
 * asx/plan/plan.h — plan DAG IR, algebraic rewrite, and certificates
 *
 * Provides the concurrency plan representation (Join/Race/Timeout/Leaf),
 * policy-controlled algebraic rewrites, and rewrite certificates that
 * link optimization decisions to deterministic inputs.
 *
 * Design:
 *   - Fixed-capacity DAG: no dynamic allocation, arena-style node storage.
 *   - Algebraic rewrites: associativity, commutativity, timeout collapse.
 *   - Certificates: FNV-1a hash of DAG structure + rewrite step ledger.
 *   - Service layer trait: poll_ready/call function pointer vtable.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PLAN_PLAN_H
#define ASX_PLAN_PLAN_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Capacity limits                                                     */
/* ------------------------------------------------------------------ */

#ifndef ASX_PLAN_MAX_NODES
#define ASX_PLAN_MAX_NODES 64u
#endif
#ifndef ASX_PLAN_MAX_CHILDREN
#define ASX_PLAN_MAX_CHILDREN 8u
#endif
#ifndef ASX_PLAN_MAX_LABEL
#define ASX_PLAN_MAX_LABEL 32u
#endif
#ifndef ASX_PLAN_MAX_STEPS
#define ASX_PLAN_MAX_STEPS 32u
#endif

/* ------------------------------------------------------------------ */
/* Plan node types                                                     */
/* ------------------------------------------------------------------ */

typedef uint16_t asx_plan_id;
#define ASX_PLAN_ID_NONE UINT16_MAX

typedef enum {
    ASX_PLAN_LEAF = 0,
    ASX_PLAN_JOIN = 1,   /* all children must complete */
    ASX_PLAN_RACE = 2,   /* first to complete wins */
    ASX_PLAN_TIMEOUT = 3 /* child with deadline */
} asx_plan_node_kind;

typedef struct {
    asx_plan_node_kind kind;
    union {
        struct {
            char label[ASX_PLAN_MAX_LABEL];
        } leaf;
        struct {
            asx_plan_id children[ASX_PLAN_MAX_CHILDREN];
            uint8_t count;
        } group; /* used for JOIN and RACE */
        struct {
            asx_plan_id child;
            uint64_t duration_us; /* microseconds */
        } timeout;
    } data;
} asx_plan_node;

/* ------------------------------------------------------------------ */
/* Plan DAG                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_plan_node nodes[ASX_PLAN_MAX_NODES];
    uint16_t count;
    asx_plan_id root;
} asx_plan_dag;

/* Initialize an empty plan DAG. */
ASX_API void asx_plan_dag_init(asx_plan_dag *dag);

/* Add a leaf node.  Returns the node ID or ASX_PLAN_ID_NONE on overflow. */
ASX_API asx_plan_id asx_plan_dag_leaf(asx_plan_dag *dag, const char *label);

/* Add a join node.  Returns node ID or ASX_PLAN_ID_NONE. */
ASX_API asx_plan_id asx_plan_dag_join(asx_plan_dag *dag, const asx_plan_id *children,
                                      uint8_t count);

/* Add a race node.  Returns node ID or ASX_PLAN_ID_NONE. */
ASX_API asx_plan_id asx_plan_dag_race(asx_plan_dag *dag, const asx_plan_id *children,
                                      uint8_t count);

/* Add a timeout node.  Returns node ID or ASX_PLAN_ID_NONE. */
ASX_API asx_plan_id asx_plan_dag_timeout(asx_plan_dag *dag, asx_plan_id child,
                                         uint64_t duration_us);

/* Set the root node. */
ASX_API void asx_plan_dag_set_root(asx_plan_dag *dag, asx_plan_id root);

/* Get a node by ID.  Returns NULL if out of range. */
ASX_API const asx_plan_node *asx_plan_dag_node(const asx_plan_dag *dag, asx_plan_id id);

/* Validate the DAG: checks for missing children, empty groups, and
 * simple cycle detection.  Returns ASX_OK or an error status. */
ASX_API ASX_MUST_USE asx_status asx_plan_dag_validate(const asx_plan_dag *dag);

/* Count nodes by kind. */
ASX_API void asx_plan_dag_stats(const asx_plan_dag *dag, uint16_t *out_leaves, uint16_t *out_joins,
                                uint16_t *out_races, uint16_t *out_timeouts);

/* ------------------------------------------------------------------ */
/* Plan hash — deterministic FNV-1a structural hash                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t value;
} asx_plan_hash;

/* Compute FNV-1a hash of the entire DAG structure. */
ASX_API asx_plan_hash asx_plan_hash_compute(const asx_plan_dag *dag);

/* ------------------------------------------------------------------ */
/* Rewrite rules                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_REWRITE_JOIN_ASSOC = 0, /* Join[Join[a,b], c] → Join[a,b,c] */
    ASX_REWRITE_RACE_ASSOC = 1, /* Race[Race[a,b], c] → Race[a,b,c] */
    ASX_REWRITE_TIMEOUT_MIN = 2 /* Timeout[d1, Timeout[d2, f]] → Timeout[min(d1,d2), f] */
} asx_rewrite_rule;

/* Rewrite policy flags */
typedef struct {
    int associativity;          /* flatten nested Join/Race */
    int timeout_simplification; /* collapse nested timeouts */
} asx_rewrite_policy;

/* Predefined policies */
ASX_API void asx_rewrite_policy_conservative(asx_rewrite_policy *p);
ASX_API void asx_rewrite_policy_none(asx_rewrite_policy *p);

/* Check if a policy permits a specific rule. */
ASX_API int asx_rewrite_policy_permits(const asx_rewrite_policy *p, asx_rewrite_rule rule);

/* ------------------------------------------------------------------ */
/* Rewrite step recording                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_rewrite_rule rule;
    asx_plan_id before; /* node ID before rewrite */
    asx_plan_id after;  /* node ID after rewrite */
} asx_rewrite_step;

/* ------------------------------------------------------------------ */
/* Rewrite certificate                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_rewrite_policy policy;
    asx_plan_hash before_hash;
    asx_plan_hash after_hash;
    uint16_t before_count;
    uint16_t after_count;
    asx_rewrite_step steps[ASX_PLAN_MAX_STEPS];
    uint16_t step_count;
} asx_rewrite_certificate;

/* Initialize a certificate. */
ASX_API void asx_rewrite_certificate_init(asx_rewrite_certificate *cert);

/* Record a rewrite step.  Returns ASX_OK or ASX_E_RESOURCE_EXHAUSTED. */
ASX_API ASX_MUST_USE asx_status asx_rewrite_certificate_record(asx_rewrite_certificate *cert,
                                                               asx_rewrite_rule rule,
                                                               asx_plan_id before,
                                                               asx_plan_id after);

/* Returns nonzero if no rewrites were applied. */
ASX_API int asx_rewrite_certificate_is_identity(const asx_rewrite_certificate *cert);

/* Compute a fingerprint of the certificate for dedup. */
ASX_API uint64_t asx_rewrite_certificate_fingerprint(const asx_rewrite_certificate *cert);

/* ------------------------------------------------------------------ */
/* Rewrite engine                                                      */
/* ------------------------------------------------------------------ */

/* Apply all permitted rewrites to the DAG in-place.
 * Records steps to the certificate.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_plan_rewrite(asx_plan_dag *dag,
                                                 const asx_rewrite_policy *policy,
                                                 asx_rewrite_certificate *cert);

/* ------------------------------------------------------------------ */
/* Service layer trait — poll_ready/call function pointer vtable        */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_SERVICE_READY = 0,
    ASX_SERVICE_NOT_READY = 1,
    ASX_SERVICE_ERROR = 2
} asx_service_readiness;

typedef asx_service_readiness (*asx_service_poll_ready_fn)(void *state);

typedef asx_status (*asx_service_call_fn)(void *state, const void *request, void *response);

typedef struct {
    asx_service_poll_ready_fn poll_ready;
    asx_service_call_fn call;
    void *state;
} asx_service;

/* Poll the service for readiness. */
ASX_API asx_service_readiness asx_service_poll_ready(asx_service *svc);

/* Call the service with a request, producing a response. */
ASX_API ASX_MUST_USE asx_status asx_service_call(asx_service *svc, const void *request,
                                                 void *response);

/* ------------------------------------------------------------------ */
/* Service layer trait — composable middleware                          */
/* ------------------------------------------------------------------ */

typedef void (*asx_layer_apply_fn)(void *layer_state, asx_service *inner, asx_service *out);

typedef struct {
    asx_layer_apply_fn apply;
    void *state;
} asx_layer;

/* Apply a layer to a service, producing a wrapped service. */
ASX_API void asx_layer_apply(const asx_layer *layer, asx_service *inner, asx_service *out);

/* ------------------------------------------------------------------ */
/* Timeout layer                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_service inner;
    uint64_t duration_us; /* timeout in microseconds */
    uint64_t deadline;    /* computed deadline (virtual time) */
    int ready;            /* readiness flag */
} asx_timeout_service_state;

/* Initialize a timeout layer. */
ASX_API void asx_timeout_layer_init(asx_service *out, asx_timeout_service_state *state,
                                    asx_service inner, uint64_t duration_us);

/* ------------------------------------------------------------------ */
/* Load-shed layer                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_service inner;
} asx_load_shed_service_state;

/* Initialize a load-shed layer (rejects when inner not ready). */
ASX_API void asx_load_shed_layer_init(asx_service *out, asx_load_shed_service_state *state,
                                      asx_service inner);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PLAN_PLAN_H */
