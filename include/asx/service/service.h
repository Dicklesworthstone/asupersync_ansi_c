/*
 * asx/service/service.h — extended service middleware combinators
 *
 * Builds on the service trait defined in asx/plan/plan.h (asx_service,
 * asx_service_readiness, asx_service_poll_ready_fn, asx_service_call_fn)
 * and adds: identity service, map-request, map-response, rate-limit,
 * buffer, and a service-builder for fluent middleware composition.
 *
 * The base trait (poll_ready/call/layer) plus timeout, load-shed, and
 * retry layers live in plan.h. This header extends with additional
 * middleware for production service stacks.
 *
 * Design:
 *   - Each middleware wraps an inner asx_service, adding behavior.
 *   - Zero dynamic allocation — all types are stack-allocable value types.
 *   - Requests and responses are opaque pointers with caller-managed lifetimes.
 *   - Integrates with asx_status for error propagation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SERVICE_SERVICE_H
#define ASX_SERVICE_SERVICE_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/plan/plan.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Identity service — passes requests through unchanged               */
/* ------------------------------------------------------------------ */

typedef struct {
    int placeholder; /* zero-state identity */
} asx_service_identity_state;

/* Initialize an identity service that copies request to response.
 * Caller must ensure the response buffer is large enough. */
ASX_API void asx_service_identity_init(asx_service *svc, asx_service_identity_state *state);

/* ------------------------------------------------------------------ */
/* Map-request middleware — transforms requests before inner call      */
/* ------------------------------------------------------------------ */

/* Map function: transforms request, returning a pointer to the
 * transformed request. The returned pointer must remain valid until
 * the inner call completes. */
typedef const void *(*asx_service_map_request_fn)(const void *request, void *user_data);

typedef struct {
    asx_service inner;
    asx_service_map_request_fn map_fn;
    void *user_data;
} asx_service_map_request_state;

/* Wrap a service with a request-mapping function. */
ASX_API void asx_service_map_request_init(asx_service *svc, asx_service_map_request_state *state,
                                          asx_service inner, asx_service_map_request_fn map_fn,
                                          void *user_data);

/* ------------------------------------------------------------------ */
/* Map-response middleware — transforms responses after inner call     */
/* ------------------------------------------------------------------ */

/* Map function: transforms response in-place or copies to a new buffer.
 * response points to the buffer that the inner call wrote to.
 * user_data is the closure context. */
typedef void (*asx_service_map_response_fn)(void *response, void *user_data);

typedef struct {
    asx_service inner;
    asx_service_map_response_fn map_fn;
    void *user_data;
} asx_service_map_response_state;

/* Wrap a service with a response-mapping function. */
ASX_API void asx_service_map_response_init(asx_service *svc, asx_service_map_response_state *state,
                                           asx_service inner, asx_service_map_response_fn map_fn,
                                           void *user_data);

/* ------------------------------------------------------------------ */
/* Rate-limit middleware — limits call throughput                      */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_service inner;
    uint32_t max_permits;  /* maximum calls per window */
    uint32_t remaining;    /* permits remaining in current window */
    uint64_t window_ns;    /* window duration in nanoseconds */
    uint64_t window_start; /* monotonic timestamp of window start */
} asx_service_rate_limit_state;

/* Wrap a service with rate limiting. Returns ASX_E_RESOURCE_EXHAUSTED
 * when the permit budget is exhausted within the current window. */
ASX_API void asx_service_rate_limit_init(asx_service *svc, asx_service_rate_limit_state *state,
                                         asx_service inner, uint32_t max_permits,
                                         uint64_t window_ns);

/* Return the number of permits remaining in the current window. */
ASX_API uint32_t asx_service_rate_limit_remaining(const asx_service_rate_limit_state *state);

/* ------------------------------------------------------------------ */
/* Buffer middleware — buffers requests when inner is not ready        */
/* ------------------------------------------------------------------ */

#ifndef ASX_SERVICE_BUFFER_CAPACITY
#define ASX_SERVICE_BUFFER_CAPACITY 8u
#endif

typedef struct {
    const void *request;
    void *response;
    int occupied;
} asx_service_buffer_slot;

typedef struct {
    asx_service inner;
    asx_service_buffer_slot slots[ASX_SERVICE_BUFFER_CAPACITY];
    uint32_t head;  /* next slot to dequeue */
    uint32_t tail;  /* next slot to enqueue */
    uint32_t count; /* number of buffered requests */
    uint32_t capacity;
} asx_service_buffer_state;

/* Wrap a service with a bounded request buffer. Enqueued requests are
 * forwarded when inner becomes ready. Returns ASX_E_RESOURCE_EXHAUSTED
 * if the buffer is full. */
ASX_API void asx_service_buffer_init(asx_service *svc, asx_service_buffer_state *state,
                                     asx_service inner);

/* Return the number of buffered (pending) requests. */
ASX_API uint32_t asx_service_buffer_pending(const asx_service_buffer_state *state);

/* Forward declarations for builder use */
typedef int (*asx_service_filter_fn)(const void *request, void *user_data);

typedef struct {
    asx_service inner;
    uint32_t max_inflight;
    uint32_t current;
} asx_service_concurrency_limit_state;

typedef struct {
    asx_service inner;
    asx_service_filter_fn filter_fn;
    void *user_data;
} asx_service_filter_state;

/* ------------------------------------------------------------------ */
/* Service builder — fluent middleware composition                     */
/* ------------------------------------------------------------------ */

/* Maximum middleware layers in a builder chain. */
#ifndef ASX_SERVICE_BUILDER_MAX_LAYERS
#define ASX_SERVICE_BUILDER_MAX_LAYERS 8u
#endif

typedef enum {
    ASX_SERVICE_LAYER_TIMEOUT = 0,
    ASX_SERVICE_LAYER_RETRY = 1,
    ASX_SERVICE_LAYER_RATE_LIMIT = 2,
    ASX_SERVICE_LAYER_LOAD_SHED = 3,
    ASX_SERVICE_LAYER_BUFFER = 4,
    ASX_SERVICE_LAYER_MAP_REQUEST = 5,
    ASX_SERVICE_LAYER_MAP_RESPONSE = 6,
    ASX_SERVICE_LAYER_CONCURRENCY_LIMIT = 7,
    ASX_SERVICE_LAYER_FILTER = 8
} asx_service_layer_kind;

typedef struct {
    asx_service_layer_kind kind;
    union {
        struct {
            uint64_t timeout_ns;
        } timeout;
        struct {
            uint32_t max_retries;
        } retry;
        struct {
            uint32_t max_permits;
            uint64_t window_ns;
        } rate_limit;
        struct {
            asx_service_map_request_fn fn;
            void *user_data;
        } map_request;
        struct {
            asx_service_map_response_fn fn;
            void *user_data;
        } map_response;
        struct {
            uint32_t max_inflight;
        } concurrency_limit;
        struct {
            asx_service_filter_fn fn;
            void *user_data;
        } filter;
    } config;
} asx_service_layer_spec;

typedef struct {
    asx_service_layer_spec layers[ASX_SERVICE_BUILDER_MAX_LAYERS];
    uint32_t layer_count;
} asx_service_builder;

typedef struct {
    asx_timeout_service_state timeout[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_retry_service_state retry[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_rate_limit_state rate_limit[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_load_shed_service_state load_shed[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_buffer_state buffer[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_map_request_state map_request[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_map_response_state map_response[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_concurrency_limit_state concurrency_limit[ASX_SERVICE_BUILDER_MAX_LAYERS];
    asx_service_filter_state filter[ASX_SERVICE_BUILDER_MAX_LAYERS];
} asx_service_builder_runtime;

/* Initialize a service builder. */
ASX_API void asx_service_builder_init(asx_service_builder *builder);

/* Add a timeout layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_timeout(asx_service_builder *builder,
                                                            uint64_t timeout_ns);

/* Add a retry layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_retry(asx_service_builder *builder,
                                                          uint32_t max_retries);

/* Add a rate-limit layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_rate_limit(asx_service_builder *builder,
                                                               uint32_t max_permits,
                                                               uint64_t window_ns);

/* Add a request-mapping layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_map_request(asx_service_builder *builder,
                                                                asx_service_map_request_fn fn,
                                                                void *user_data);

/* Add a response-mapping layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_map_response(asx_service_builder *builder,
                                                                 asx_service_map_response_fn fn,
                                                                 void *user_data);

/* Add a load-shed layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_load_shed(asx_service_builder *builder);

/* Add a buffer layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_buffer(asx_service_builder *builder);

/* Add a concurrency-limit layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_concurrency_limit(asx_service_builder *builder,
                                                                      uint32_t max_inflight);

/* Add a filter layer. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_filter(asx_service_builder *builder,
                                                           asx_service_filter_fn fn,
                                                           void *user_data);

/* Build a concrete composed service from the recorded layer specs.
 * Layers are applied in insertion order, so later-added layers become
 * the outermost wrappers around the base service. */
ASX_API ASX_MUST_USE asx_status asx_service_builder_build(asx_service *out,
                                                          asx_service_builder_runtime *runtime,
                                                          const asx_service_builder *builder,
                                                          asx_service base);

/* Return the number of layers added so far. */
ASX_API uint32_t asx_service_builder_layer_count(const asx_service_builder *builder);

/* ------------------------------------------------------------------ */
/* Concurrency-limit middleware — bounds in-flight requests            */
/* ------------------------------------------------------------------ */

/* (asx_service_concurrency_limit_state defined above for builder use) */

/* Initialize a concurrency-limiting wrapper. Returns ASX_E_OVERLOADED
 * when the limit is reached instead of forwarding to the inner service. */
ASX_API void asx_service_concurrency_limit_init(asx_service *svc,
                                                asx_service_concurrency_limit_state *state,
                                                asx_service inner, uint32_t max_inflight);

/* Must be called after each request completes (success or failure)
 * to release the in-flight slot. */
ASX_API void asx_service_concurrency_limit_release(asx_service_concurrency_limit_state *state);

/* Get current in-flight count. */
ASX_API uint32_t
asx_service_concurrency_limit_inflight(const asx_service_concurrency_limit_state *state);

/* ------------------------------------------------------------------ */
/* Filter middleware — rejects requests that fail a predicate          */
/* ------------------------------------------------------------------ */

/* (asx_service_filter_state defined above for builder use) */

/* Initialize a filter wrapper. Rejected requests get ASX_E_PERMISSION_DENIED. */
ASX_API void asx_service_filter_init(asx_service *svc, asx_service_filter_state *state,
                                     asx_service inner, asx_service_filter_fn filter_fn,
                                     void *user_data);

/* ------------------------------------------------------------------ */
/* Load-balance middleware — round-robin across multiple backends      */
/* ------------------------------------------------------------------ */

#ifndef ASX_SERVICE_LB_MAX_BACKENDS
#define ASX_SERVICE_LB_MAX_BACKENDS 8u
#endif

typedef struct {
    asx_service backends[ASX_SERVICE_LB_MAX_BACKENDS];
    uint32_t count;
    uint32_t next; /* round-robin index */
} asx_service_load_balance_state;

/* Initialize a load balancer. Backends must be added via _add_backend. */
ASX_API void asx_service_load_balance_init(asx_service *svc, asx_service_load_balance_state *state);

/* Add a backend service. Returns ASX_E_RESOURCE_EXHAUSTED at capacity. */
ASX_API ASX_MUST_USE asx_status
asx_service_load_balance_add_backend(asx_service_load_balance_state *state, asx_service backend);

/* Get the number of registered backends. */
ASX_API uint32_t
asx_service_load_balance_backend_count(const asx_service_load_balance_state *state);

/* ------------------------------------------------------------------ */
/* Reconnect middleware — auto-recreates service on failure            */
/* ------------------------------------------------------------------ */

/* Factory function: creates a new service instance.
 * Called when the inner service fails and needs replacement. */
typedef asx_status (*asx_service_make_fn)(void *factory_data, asx_service *out);

typedef struct {
    asx_service inner;
    asx_service_make_fn make_fn;
    void *factory_data;
    uint32_t reconnect_count;
    uint32_t max_reconnects; /* 0 = unlimited */
} asx_service_reconnect_state;

/* Initialize a reconnect wrapper. On inner call failure, the service
 * is recreated from make_fn. Limits total reconnects to max_reconnects
 * (0 = unlimited). */
ASX_API void asx_service_reconnect_init(asx_service *svc, asx_service_reconnect_state *state,
                                        asx_service inner, asx_service_make_fn make_fn,
                                        void *factory_data, uint32_t max_reconnects);

/* Get the number of reconnections that have occurred. */
ASX_API uint32_t asx_service_reconnect_count(const asx_service_reconnect_state *state);

/* ------------------------------------------------------------------ */
/* Steer middleware — route requests to backends by key                */
/* ------------------------------------------------------------------ */

/* Steer function: returns backend index for a given request.
 * Must return a value < backend_count. */
typedef uint32_t (*asx_service_steer_fn)(const void *request, uint32_t backend_count,
                                         void *user_data);

typedef struct {
    asx_service backends[ASX_SERVICE_LB_MAX_BACKENDS];
    uint32_t count;
    asx_service_steer_fn steer_fn;
    void *user_data;
} asx_service_steer_state;

/* Initialize a steer router. Backends added via _add_backend. */
ASX_API void asx_service_steer_init(asx_service *svc, asx_service_steer_state *state,
                                    asx_service_steer_fn steer_fn, void *user_data);

/* Add a backend. Returns ASX_E_RESOURCE_EXHAUSTED at capacity. */
ASX_API ASX_MUST_USE asx_status asx_service_steer_add_backend(asx_service_steer_state *state,
                                                              asx_service backend);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SERVICE_SERVICE_H */
