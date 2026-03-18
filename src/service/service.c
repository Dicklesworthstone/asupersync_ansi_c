/*
 * service.c — extended service middleware implementation
 *
 * Provides identity service, map-request, map-response, rate-limit,
 * buffer middleware, and a service builder. Built on the service trait
 * defined in plan.h (asx_service, asx_service_readiness).
 *
 * Zero dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/service/service.h>
#include <string.h>

/* Helper: read current monotonic time via the runtime clock. Returns
 * 0 if the runtime clock is not yet initialized. */
static uint64_t service_now_ns(void) {
    asx_time now = 0;
    asx_runtime_now_ns(&now);
    return now;
}

/* ------------------------------------------------------------------ */
/* Identity service                                                    */
/* ------------------------------------------------------------------ */

static asx_service_readiness identity_poll_ready(void *state) {
    (void)state;
    return ASX_SERVICE_READY;
}

static asx_status identity_call(void *state, const void *request, void *response) {
    (void)state;
    /* Identity: caller is responsible for request/response buffer semantics.
     * We store the request pointer in the response for trivial pass-through. */
    *(const void **)response = request;
    return ASX_OK;
}

void asx_service_identity_init(asx_service *svc, asx_service_identity_state *state) {
    state->placeholder = 0;
    svc->poll_ready = identity_poll_ready;
    svc->call = identity_call;
    svc->state = state;
}

/* ------------------------------------------------------------------ */
/* Map-request middleware                                               */
/* ------------------------------------------------------------------ */

static asx_service_readiness map_request_poll_ready(void *state) {
    asx_service_map_request_state *ms = (asx_service_map_request_state *)state;
    return asx_service_poll_ready(&ms->inner);
}

static asx_status map_request_call(void *state, const void *request, void *response) {
    asx_service_map_request_state *ms = (asx_service_map_request_state *)state;
    const void *mapped = ms->map_fn(request, ms->user_data);
    return asx_service_call(&ms->inner, mapped, response);
}

void asx_service_map_request_init(asx_service *svc, asx_service_map_request_state *state,
                                  asx_service inner, asx_service_map_request_fn map_fn,
                                  void *user_data) {
    state->inner = inner;
    state->map_fn = map_fn;
    state->user_data = user_data;
    svc->poll_ready = map_request_poll_ready;
    svc->call = map_request_call;
    svc->state = state;
}

/* ------------------------------------------------------------------ */
/* Map-response middleware                                              */
/* ------------------------------------------------------------------ */

static asx_service_readiness map_response_poll_ready(void *state) {
    asx_service_map_response_state *ms = (asx_service_map_response_state *)state;
    return asx_service_poll_ready(&ms->inner);
}

static asx_status map_response_call(void *state, const void *request, void *response) {
    asx_service_map_response_state *ms = (asx_service_map_response_state *)state;
    asx_status st = asx_service_call(&ms->inner, request, response);

    if (st == ASX_OK) { ms->map_fn(response, ms->user_data); }
    return st;
}

void asx_service_map_response_init(asx_service *svc, asx_service_map_response_state *state,
                                   asx_service inner, asx_service_map_response_fn map_fn,
                                   void *user_data) {
    state->inner = inner;
    state->map_fn = map_fn;
    state->user_data = user_data;
    svc->poll_ready = map_response_poll_ready;
    svc->call = map_response_call;
    svc->state = state;
}

/* ------------------------------------------------------------------ */
/* Rate-limit middleware                                                */
/* ------------------------------------------------------------------ */

static asx_service_readiness rate_limit_poll_ready(void *state) {
    asx_service_rate_limit_state *rl = (asx_service_rate_limit_state *)state;
    uint64_t now = service_now_ns();

    /* Reset window if expired. */
    if (rl->window_ns > 0 && now - rl->window_start >= rl->window_ns) {
        rl->window_start = now;
        rl->remaining = rl->max_permits;
    }

    if (rl->remaining == 0) { return ASX_SERVICE_NOT_READY; }

    return asx_service_poll_ready(&rl->inner);
}

static asx_status rate_limit_call(void *state, const void *request, void *response) {
    asx_service_rate_limit_state *rl = (asx_service_rate_limit_state *)state;
    uint64_t now = service_now_ns();

    /* Reset window if expired. */
    if (rl->window_ns > 0 && now - rl->window_start >= rl->window_ns) {
        rl->window_start = now;
        rl->remaining = rl->max_permits;
    }

    if (rl->remaining == 0) { return ASX_E_RESOURCE_EXHAUSTED; }

    rl->remaining--;
    return asx_service_call(&rl->inner, request, response);
}

void asx_service_rate_limit_init(asx_service *svc, asx_service_rate_limit_state *state,
                                 asx_service inner, uint32_t max_permits, uint64_t window_ns) {
    state->inner = inner;
    state->max_permits = max_permits;
    state->remaining = max_permits;
    state->window_ns = window_ns;
    state->window_start = service_now_ns();
    svc->poll_ready = rate_limit_poll_ready;
    svc->call = rate_limit_call;
    svc->state = state;
}

uint32_t asx_service_rate_limit_remaining(const asx_service_rate_limit_state *state) {
    return state->remaining;
}

/* ------------------------------------------------------------------ */
/* Buffer middleware                                                    */
/* ------------------------------------------------------------------ */

static asx_service_readiness buffer_poll_ready(void *state) {
    asx_service_buffer_state *bs = (asx_service_buffer_state *)state;
    if (bs->count >= bs->capacity) { return ASX_SERVICE_NOT_READY; }
    return ASX_SERVICE_READY;
}

static asx_status buffer_call(void *state, const void *request, void *response) {
    asx_service_buffer_state *bs = (asx_service_buffer_state *)state;

    /* Try to flush buffered requests first. */
    while (bs->count > 0) {
        asx_service_readiness inner_ready = asx_service_poll_ready(&bs->inner);
        if (inner_ready != ASX_SERVICE_READY) break;

        {
            const void *buffered_req = bs->slots[bs->head].request;
            void *buffered_resp = bs->slots[bs->head].response;
            asx_status st = asx_service_call(&bs->inner, buffered_req, buffered_resp);
            (void)st; /* buffered calls are fire-and-forget in this model */
            bs->slots[bs->head].occupied = 0;
            bs->head = (bs->head + 1) % bs->capacity;
            bs->count--;
        }
    }

    /* Try direct call if inner is ready. */
    {
        asx_service_readiness inner_ready = asx_service_poll_ready(&bs->inner);
        if (inner_ready == ASX_SERVICE_READY) {
            return asx_service_call(&bs->inner, request, response);
        }
    }

    /* Buffer the request. */
    if (bs->count >= bs->capacity) { return ASX_E_RESOURCE_EXHAUSTED; }

    bs->slots[bs->tail].request = request;
    bs->slots[bs->tail].response = response;
    bs->slots[bs->tail].occupied = 1;
    bs->tail = (bs->tail + 1) % bs->capacity;
    bs->count++;
    return ASX_E_PENDING;
}

void asx_service_buffer_init(asx_service *svc, asx_service_buffer_state *state, asx_service inner) {
    uint32_t i;
    state->inner = inner;
    state->head = 0;
    state->tail = 0;
    state->count = 0;
    state->capacity = ASX_SERVICE_BUFFER_CAPACITY;
    for (i = 0; i < ASX_SERVICE_BUFFER_CAPACITY; i++) {
        state->slots[i].request = NULL;
        state->slots[i].response = NULL;
        state->slots[i].occupied = 0;
    }
    svc->poll_ready = buffer_poll_ready;
    svc->call = buffer_call;
    svc->state = state;
}

uint32_t asx_service_buffer_pending(const asx_service_buffer_state *state) { return state->count; }

/* ------------------------------------------------------------------ */
/* Service builder                                                     */
/* ------------------------------------------------------------------ */

void asx_service_builder_init(asx_service_builder *builder) {
    builder->layer_count = 0;
    memset(builder->layers, 0, sizeof(builder->layers));
}

static asx_status builder_add_layer(asx_service_builder *builder, asx_service_layer_spec spec) {
    if (builder->layer_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) { return ASX_E_RESOURCE_EXHAUSTED; }
    builder->layers[builder->layer_count++] = spec;
    return ASX_OK;
}

asx_status asx_service_builder_timeout(asx_service_builder *builder, uint64_t timeout_ns) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_TIMEOUT;
    spec.config.timeout.timeout_ns = timeout_ns;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_retry(asx_service_builder *builder, uint32_t max_retries) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_RETRY;
    spec.config.retry.max_retries = max_retries;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_rate_limit(asx_service_builder *builder, uint32_t max_permits,
                                          uint64_t window_ns) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_RATE_LIMIT;
    spec.config.rate_limit.max_permits = max_permits;
    spec.config.rate_limit.window_ns = window_ns;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_map_request(asx_service_builder *builder,
                                           asx_service_map_request_fn fn, void *user_data) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_MAP_REQUEST;
    spec.config.map_request.fn = fn;
    spec.config.map_request.user_data = user_data;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_map_response(asx_service_builder *builder,
                                            asx_service_map_response_fn fn, void *user_data) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_MAP_RESPONSE;
    spec.config.map_response.fn = fn;
    spec.config.map_response.user_data = user_data;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_load_shed(asx_service_builder *builder) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_LOAD_SHED;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_buffer(asx_service_builder *builder) {
    asx_service_layer_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = ASX_SERVICE_LAYER_BUFFER;
    return builder_add_layer(builder, spec);
}

asx_status asx_service_builder_build(asx_service *out, asx_service_builder_runtime *runtime,
                                     const asx_service_builder *builder, asx_service base) {
    asx_service current;
    uint32_t timeout_count = 0;
    uint32_t retry_count = 0;
    uint32_t rate_limit_count = 0;
    uint32_t load_shed_count = 0;
    uint32_t buffer_count = 0;
    uint32_t map_request_count = 0;
    uint32_t map_response_count = 0;
    uint32_t i;

    if (out == NULL || runtime == NULL || builder == NULL) { return ASX_E_INVALID_ARGUMENT; }

    memset(runtime, 0, sizeof(*runtime));
    current = base;

    for (i = 0; i < builder->layer_count; i++) {
        const asx_service_layer_spec *spec = &builder->layers[i];

        switch (spec->kind) {
        case ASX_SERVICE_LAYER_TIMEOUT:
            if (timeout_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) {
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            asx_timeout_layer_init(&current, &runtime->timeout[timeout_count], current,
                                   spec->config.timeout.timeout_ns);
            timeout_count++;
            break;
        case ASX_SERVICE_LAYER_RETRY:
            if (retry_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) { return ASX_E_RESOURCE_EXHAUSTED; }
            if (spec->config.retry.max_retries > UINT8_MAX) { return ASX_E_INVALID_ARGUMENT; }
            asx_retry_layer_init(&current, &runtime->retry[retry_count], current,
                                 (uint8_t)spec->config.retry.max_retries);
            retry_count++;
            break;
        case ASX_SERVICE_LAYER_RATE_LIMIT:
            if (rate_limit_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) {
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            asx_service_rate_limit_init(&current, &runtime->rate_limit[rate_limit_count], current,
                                        spec->config.rate_limit.max_permits,
                                        spec->config.rate_limit.window_ns);
            rate_limit_count++;
            break;
        case ASX_SERVICE_LAYER_LOAD_SHED:
            if (load_shed_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) {
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            asx_load_shed_layer_init(&current, &runtime->load_shed[load_shed_count], current);
            load_shed_count++;
            break;
        case ASX_SERVICE_LAYER_BUFFER:
            if (buffer_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) { return ASX_E_RESOURCE_EXHAUSTED; }
            asx_service_buffer_init(&current, &runtime->buffer[buffer_count], current);
            buffer_count++;
            break;
        case ASX_SERVICE_LAYER_MAP_REQUEST:
            if (map_request_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) {
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            if (spec->config.map_request.fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
            asx_service_map_request_init(&current, &runtime->map_request[map_request_count],
                                         current, spec->config.map_request.fn,
                                         spec->config.map_request.user_data);
            map_request_count++;
            break;
        case ASX_SERVICE_LAYER_MAP_RESPONSE:
            if (map_response_count >= ASX_SERVICE_BUILDER_MAX_LAYERS) {
                return ASX_E_RESOURCE_EXHAUSTED;
            }
            if (spec->config.map_response.fn == NULL) { return ASX_E_INVALID_ARGUMENT; }
            asx_service_map_response_init(&current, &runtime->map_response[map_response_count],
                                          current, spec->config.map_response.fn,
                                          spec->config.map_response.user_data);
            map_response_count++;
            break;
        default: return ASX_E_INVALID_ARGUMENT;
        }
    }

    *out = current;
    return ASX_OK;
}

uint32_t asx_service_builder_layer_count(const asx_service_builder *builder) {
    return builder->layer_count;
}
