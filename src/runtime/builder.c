/*
 * builder.c — explicit runtime builder presets and setters
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/builder.h>
#include <string.h>

static asx_status builder_init_common(asx_runtime_builder *builder, asx_runtime_preset preset) {
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(builder, 0, sizeof(*builder));
    asx_runtime_config_init(&builder->config);
    st = asx_runtime_hooks_init(&builder->hooks);
    if (st != ASX_OK) return st;
    builder->preset = preset;
    return ASX_OK;
}

static asx_status builder_copy_in(void *dst, size_t dst_size, const void *src, size_t src_size) {
    if (dst == NULL || src == NULL || dst_size != src_size) return ASX_E_INVALID_ARGUMENT;
    memcpy(dst, src, src_size);
    return ASX_OK;
}

asx_status asx_runtime_builder_init(asx_runtime_builder *builder) {
    return builder_init_common(builder, ASX_RUNTIME_PRESET_DEFAULT);
}

asx_status asx_runtime_builder_init_current_thread(asx_runtime_builder *builder) {
    asx_status st;

    st = builder_init_common(builder, ASX_RUNTIME_PRESET_CURRENT_THREAD);
    if (st != ASX_OK) return st;

    builder->config.wait_policy = ASX_WAIT_BUSY_SPIN;
    builder->config.finalizer_poll_budget = 64u;
    builder->config.finalizer_time_budget_ns = (uint64_t)1000000000ULL;
    return ASX_OK;
}

asx_status asx_runtime_builder_init_low_latency(asx_runtime_builder *builder) {
    asx_status st;

    st = builder_init_common(builder, ASX_RUNTIME_PRESET_LOW_LATENCY);
    if (st != ASX_OK) return st;

    builder->config.wait_policy = ASX_WAIT_BUSY_SPIN;
    builder->config.finalizer_poll_budget = 32u;
    builder->config.finalizer_time_budget_ns = (uint64_t)500000000ULL;
    builder->config.max_cancel_chain_depth = 8u;
    builder->config.max_cancel_chain_memory = 2048u;
    builder->config.finalizer_escalation = ASX_FINALIZER_SOFT;
    return ASX_OK;
}

asx_status asx_runtime_builder_init_high_throughput(asx_runtime_builder *builder) {
    asx_status st;

    st = builder_init_common(builder, ASX_RUNTIME_PRESET_HIGH_THROUGHPUT);
    if (st != ASX_OK) return st;

    builder->config.wait_policy = ASX_WAIT_SLEEP;
    builder->config.finalizer_poll_budget = 256u;
    builder->config.finalizer_time_budget_ns = (uint64_t)10000000000ULL;
    builder->config.max_cancel_chain_depth = 32u;
    builder->config.max_cancel_chain_memory = 8192u;
    builder->config.finalizer_escalation = ASX_FINALIZER_BOUNDED_LOG;
    return ASX_OK;
}

asx_runtime_preset asx_runtime_builder_preset(const asx_runtime_builder *builder) {
    if (builder == NULL) return ASX_RUNTIME_PRESET_DEFAULT;
    return builder->preset;
}

const char *asx_runtime_preset_str(asx_runtime_preset preset) {
    switch (preset) {
    case ASX_RUNTIME_PRESET_DEFAULT: return "default";
    case ASX_RUNTIME_PRESET_CURRENT_THREAD: return "current-thread";
    case ASX_RUNTIME_PRESET_LOW_LATENCY: return "low-latency";
    case ASX_RUNTIME_PRESET_HIGH_THROUGHPUT: return "high-throughput";
    default: return "unknown";
    }
}

asx_status asx_runtime_builder_get_config(const asx_runtime_builder *builder,
                                          asx_runtime_config *out) {
    if (builder == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = builder->config;
    return ASX_OK;
}

asx_status asx_runtime_builder_get_hooks(const asx_runtime_builder *builder, asx_runtime_hooks *out) {
    if (builder == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = builder->hooks;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_wait_policy(asx_runtime_builder *builder, asx_wait_policy policy) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    builder->config.wait_policy = policy;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_leak_response(asx_runtime_builder *builder,
                                                 asx_leak_response response) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    builder->config.leak_response = response;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_poll_budget(asx_runtime_builder *builder,
                                                         uint32_t polls) {
    if (builder == NULL || polls == 0u) return ASX_E_INVALID_ARGUMENT;
    builder->config.finalizer_poll_budget = polls;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_time_budget_ns(asx_runtime_builder *builder,
                                                            uint64_t budget_ns) {
    if (builder == NULL || budget_ns == 0u) return ASX_E_INVALID_ARGUMENT;
    builder->config.finalizer_time_budget_ns = budget_ns;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_escalation(asx_runtime_builder *builder,
                                                        asx_finalizer_escalation escalation) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    builder->config.finalizer_escalation = escalation;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_max_cancel_chain_depth(asx_runtime_builder *builder,
                                                          uint16_t depth) {
    if (builder == NULL || depth == 0u) return ASX_E_INVALID_ARGUMENT;
    builder->config.max_cancel_chain_depth = depth;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_max_cancel_chain_memory(asx_runtime_builder *builder,
                                                           uint32_t bytes) {
    if (builder == NULL || bytes == 0u) return ASX_E_INVALID_ARGUMENT;
    builder->config.max_cancel_chain_memory = bytes;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_hooks(asx_runtime_builder *builder, const asx_runtime_hooks *hooks) {
    if (builder == NULL || hooks == NULL) return ASX_E_INVALID_ARGUMENT;
    builder->hooks = *hooks;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_allocator_hooks(asx_runtime_builder *builder,
                                                   const asx_allocator_hooks *allocator) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    return builder_copy_in(&builder->hooks.allocator, sizeof(builder->hooks.allocator), allocator,
                           sizeof(*allocator));
}

asx_status asx_runtime_builder_set_clock_hooks(asx_runtime_builder *builder,
                                               const asx_clock_hooks *clock) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    return builder_copy_in(&builder->hooks.clock, sizeof(builder->hooks.clock), clock,
                           sizeof(*clock));
}

asx_status asx_runtime_builder_set_entropy_hooks(asx_runtime_builder *builder,
                                                 const asx_entropy_hooks *entropy) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    return builder_copy_in(&builder->hooks.entropy, sizeof(builder->hooks.entropy), entropy,
                           sizeof(*entropy));
}

asx_status asx_runtime_builder_set_reactor_hooks(asx_runtime_builder *builder,
                                                 const asx_reactor_hooks *reactor) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    return builder_copy_in(&builder->hooks.reactor, sizeof(builder->hooks.reactor), reactor,
                           sizeof(*reactor));
}

asx_status asx_runtime_builder_set_log_hooks(asx_runtime_builder *builder, const asx_log_hooks *log) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    return builder_copy_in(&builder->hooks.log, sizeof(builder->hooks.log), log, sizeof(*log));
}

asx_status asx_runtime_builder_validate(const asx_runtime_builder *builder) {
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_config_validate(&builder->config);
    if (st != ASX_OK) return st;

    return asx_runtime_hooks_validate(&builder->hooks, ASX_DETERMINISTIC);
}

asx_status asx_runtime_builder_build(const asx_runtime_builder *builder, asx_runtime *out_runtime) {
    asx_status st;

    if (builder == NULL || out_runtime == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_builder_validate(builder);
    if (st != ASX_OK) return st;

    return asx_runtime_init(out_runtime, &builder->config, &builder->hooks);
}
