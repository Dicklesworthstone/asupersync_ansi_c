/*
 * builder.c — explicit runtime builder presets and setters
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/builder.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int builder_surface_ready(const asx_runtime_builder *builder);

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

static asx_status builder_set_hook_family(asx_runtime_builder *builder, void *dst_family,
                                          size_t dst_family_size, const void *src_family,
                                          size_t src_family_size) {
    asx_runtime_hooks candidate;
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    candidate = builder->hooks;
    st = builder_copy_in(dst_family == NULL
                             ? NULL
                             : (void *)((char *)&candidate +
                                        ((const char *)dst_family - (const char *)&builder->hooks)),
                         dst_family_size, src_family, src_family_size);
    if (st != ASX_OK) return st;
    st = asx_runtime_hooks_validate(&candidate, ASX_DETERMINISTIC);
    if (st != ASX_OK) return st;
    builder->hooks = candidate;
    return ASX_OK;
}

static int ascii_streq_ignore_case(const char *lhs, const char *rhs) {
    if (lhs == NULL || rhs == NULL) return 0;
    while (*lhs != '\0' && *rhs != '\0') {
        if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) return 0;
        lhs++;
        rhs++;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static const char *builder_env_value(const char *prefix, const char *key, char *name_buf,
                                     size_t name_buf_size) {
    const char *effective_prefix = prefix;
    int written;

    if (key == NULL || name_buf == NULL || name_buf_size == 0u) return NULL;
    if (effective_prefix == NULL || effective_prefix[0] == '\0') effective_prefix = "ASX_RUNTIME_";

    written = snprintf(name_buf, name_buf_size, "%s%s", effective_prefix, key);
    if (written < 0 || (size_t)written >= name_buf_size) return NULL;
    return getenv(name_buf);
}

static asx_status parse_u32(const char *text, uint32_t *out_value) {
    unsigned long value;
    char *end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') return ASX_E_INVALID_ARGUMENT;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > (unsigned long)UINT32_MAX) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out_value = (uint32_t)value;
    return ASX_OK;
}

static asx_status parse_u16(const char *text, uint16_t *out_value) {
    uint32_t value;
    asx_status st;

    st = parse_u32(text, &value);
    if (st != ASX_OK || value > (uint32_t)UINT16_MAX) return ASX_E_INVALID_ARGUMENT;
    *out_value = (uint16_t)value;
    return ASX_OK;
}

static asx_status parse_u64(const char *text, uint64_t *out_value) {
    unsigned long long value;
    char *end = NULL;

    if (text == NULL || out_value == NULL || text[0] == '\0') return ASX_E_INVALID_ARGUMENT;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return ASX_E_INVALID_ARGUMENT;
    *out_value = (uint64_t)value;
    return ASX_OK;
}

static asx_status parse_preset(const char *text, asx_runtime_preset *out_preset) {
    if (text == NULL || out_preset == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ascii_streq_ignore_case(text, "default")) {
        *out_preset = ASX_RUNTIME_PRESET_DEFAULT;
    } else if (ascii_streq_ignore_case(text, "current-thread") ||
               ascii_streq_ignore_case(text, "current_thread")) {
        *out_preset = ASX_RUNTIME_PRESET_CURRENT_THREAD;
    } else if (ascii_streq_ignore_case(text, "low-latency") ||
               ascii_streq_ignore_case(text, "low_latency")) {
        *out_preset = ASX_RUNTIME_PRESET_LOW_LATENCY;
    } else if (ascii_streq_ignore_case(text, "high-throughput") ||
               ascii_streq_ignore_case(text, "high_throughput")) {
        *out_preset = ASX_RUNTIME_PRESET_HIGH_THROUGHPUT;
    } else {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status builder_apply_preset(asx_runtime_builder *builder, asx_runtime_preset preset) {
    switch (preset) {
    case ASX_RUNTIME_PRESET_DEFAULT: return asx_runtime_builder_init(builder);
    case ASX_RUNTIME_PRESET_CURRENT_THREAD: return asx_runtime_builder_init_current_thread(builder);
    case ASX_RUNTIME_PRESET_LOW_LATENCY: return asx_runtime_builder_init_low_latency(builder);
    case ASX_RUNTIME_PRESET_HIGH_THROUGHPUT:
        return asx_runtime_builder_init_high_throughput(builder);
    default: return ASX_E_INVALID_ARGUMENT;
    }
}

static asx_status builder_apply_preset_preserve_hooks(asx_runtime_builder *builder,
                                                      asx_runtime_preset preset) {
    asx_runtime_hooks hooks;
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    hooks = builder->hooks;
    st = builder_apply_preset(builder, preset);
    if (st != ASX_OK) return st;
    builder->hooks = hooks;
    return ASX_OK;
}

static asx_status parse_wait_policy(const char *text, asx_wait_policy *out_policy) {
    if (text == NULL || out_policy == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ascii_streq_ignore_case(text, "busy_spin") || ascii_streq_ignore_case(text, "busy-spin")) {
        *out_policy = ASX_WAIT_BUSY_SPIN;
    } else if (ascii_streq_ignore_case(text, "yield")) {
        *out_policy = ASX_WAIT_YIELD;
    } else if (ascii_streq_ignore_case(text, "sleep")) {
        *out_policy = ASX_WAIT_SLEEP;
    } else {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status parse_io_backend(const char *text, asx_io_backend *out_backend) {
    if (text == NULL || out_backend == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ascii_streq_ignore_case(text, "ghost")) {
        *out_backend = ASX_IO_BACKEND_GHOST;
    } else if (ascii_streq_ignore_case(text, "io_uring") ||
               ascii_streq_ignore_case(text, "io-uring")) {
        *out_backend = ASX_IO_BACKEND_IO_URING;
    } else {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status parse_leak_response(const char *text, asx_leak_response *out_response) {
    if (text == NULL || out_response == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ascii_streq_ignore_case(text, "panic")) {
        *out_response = ASX_LEAK_PANIC;
    } else if (ascii_streq_ignore_case(text, "log")) {
        *out_response = ASX_LEAK_LOG;
    } else if (ascii_streq_ignore_case(text, "silent")) {
        *out_response = ASX_LEAK_SILENT;
    } else if (ascii_streq_ignore_case(text, "recover")) {
        *out_response = ASX_LEAK_RECOVER;
    } else {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status parse_finalizer_escalation(const char *text,
                                             asx_finalizer_escalation *out_escalation) {
    if (text == NULL || out_escalation == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ascii_streq_ignore_case(text, "soft")) {
        *out_escalation = ASX_FINALIZER_SOFT;
    } else if (ascii_streq_ignore_case(text, "bounded_log") ||
               ascii_streq_ignore_case(text, "bounded-log")) {
        *out_escalation = ASX_FINALIZER_BOUNDED_LOG;
    } else if (ascii_streq_ignore_case(text, "bounded_panic") ||
               ascii_streq_ignore_case(text, "bounded-panic")) {
        *out_escalation = ASX_FINALIZER_BOUNDED_PANIC;
    } else {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static int builder_wait_policy_valid(asx_wait_policy policy) {
    switch (policy) {
    case ASX_WAIT_BUSY_SPIN:
    case ASX_WAIT_YIELD:
    case ASX_WAIT_SLEEP: return 1;
    default: return 0;
    }
}

static int builder_leak_response_valid(asx_leak_response response) {
    switch (response) {
    case ASX_LEAK_PANIC:
    case ASX_LEAK_LOG:
    case ASX_LEAK_SILENT:
    case ASX_LEAK_RECOVER: return 1;
    default: return 0;
    }
}

static int builder_io_backend_known(asx_io_backend backend) {
    switch (backend) {
    case ASX_IO_BACKEND_GHOST:
    case ASX_IO_BACKEND_IO_URING: return 1;
    default: return 0;
    }
}

static int builder_finalizer_escalation_valid(asx_finalizer_escalation escalation) {
    switch (escalation) {
    case ASX_FINALIZER_SOFT:
    case ASX_FINALIZER_BOUNDED_LOG:
    case ASX_FINALIZER_BOUNDED_PANIC: return 1;
    default: return 0;
    }
}

static int builder_preset_valid(asx_runtime_preset preset) {
    switch (preset) {
    case ASX_RUNTIME_PRESET_DEFAULT:
    case ASX_RUNTIME_PRESET_CURRENT_THREAD:
    case ASX_RUNTIME_PRESET_LOW_LATENCY:
    case ASX_RUNTIME_PRESET_HIGH_THROUGHPUT: return 1;
    default: return 0;
    }
}

static int builder_surface_ready(const asx_runtime_builder *builder) {
    if (builder == NULL) return 0;
    if (builder->config.size != (uint32_t)sizeof(builder->config)) return 0;
    if (!builder_preset_valid(builder->preset)) return 0;
    return asx_runtime_hooks_validate(&builder->hooks, ASX_DETERMINISTIC) == ASX_OK;
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
    if (!builder_surface_ready(builder)) return ASX_RUNTIME_PRESET_DEFAULT;
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
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    *out = builder->config;
    return ASX_OK;
}

asx_status asx_runtime_builder_get_hooks(const asx_runtime_builder *builder,
                                         asx_runtime_hooks *out) {
    if (builder == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    *out = builder->hooks;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_wait_policy(asx_runtime_builder *builder,
                                               asx_wait_policy policy) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    if (!builder_wait_policy_valid(policy)) return ASX_E_INVALID_ARGUMENT;
    builder->config.wait_policy = policy;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_io_backend(asx_runtime_builder *builder, asx_io_backend backend) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    if (!builder_io_backend_known(backend)) return ASX_E_INVALID_ARGUMENT;
    return asx_runtime_config_set_io_backend(&builder->config, backend);
}

asx_status asx_runtime_builder_set_leak_response(asx_runtime_builder *builder,
                                                 asx_leak_response response) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    if (!builder_leak_response_valid(response)) return ASX_E_INVALID_ARGUMENT;
    builder->config.leak_response = response;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_poll_budget(asx_runtime_builder *builder,
                                                         uint32_t polls) {
    if (builder == NULL || polls == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    builder->config.finalizer_poll_budget = polls;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_time_budget_ns(asx_runtime_builder *builder,
                                                            uint64_t budget_ns) {
    if (builder == NULL || budget_ns == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    builder->config.finalizer_time_budget_ns = budget_ns;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_finalizer_escalation(asx_runtime_builder *builder,
                                                        asx_finalizer_escalation escalation) {
    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    if (!builder_finalizer_escalation_valid(escalation)) return ASX_E_INVALID_ARGUMENT;
    builder->config.finalizer_escalation = escalation;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_max_cancel_chain_depth(asx_runtime_builder *builder,
                                                          uint16_t depth) {
    if (builder == NULL || depth == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    builder->config.max_cancel_chain_depth = depth;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_max_cancel_chain_memory(asx_runtime_builder *builder,
                                                           uint32_t bytes) {
    if (builder == NULL || bytes == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    builder->config.max_cancel_chain_memory = bytes;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_hooks(asx_runtime_builder *builder,
                                         const asx_runtime_hooks *hooks) {
    asx_status st;

    if (builder == NULL || hooks == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;
    st = asx_runtime_hooks_validate(hooks, ASX_DETERMINISTIC);
    if (st != ASX_OK) return st;
    builder->hooks = *hooks;
    return ASX_OK;
}

asx_status asx_runtime_builder_set_allocator_hooks(asx_runtime_builder *builder,
                                                   const asx_allocator_hooks *allocator) {
    return builder_set_hook_family(builder, &builder->hooks.allocator,
                                   sizeof(builder->hooks.allocator), allocator, sizeof(*allocator));
}

asx_status asx_runtime_builder_set_clock_hooks(asx_runtime_builder *builder,
                                               const asx_clock_hooks *clock) {
    return builder_set_hook_family(builder, &builder->hooks.clock, sizeof(builder->hooks.clock),
                                   clock, sizeof(*clock));
}

asx_status asx_runtime_builder_set_entropy_hooks(asx_runtime_builder *builder,
                                                 const asx_entropy_hooks *entropy) {
    return builder_set_hook_family(builder, &builder->hooks.entropy, sizeof(builder->hooks.entropy),
                                   entropy, sizeof(*entropy));
}

asx_status asx_runtime_builder_set_reactor_hooks(asx_runtime_builder *builder,
                                                 const asx_reactor_hooks *reactor) {
    return builder_set_hook_family(builder, &builder->hooks.reactor, sizeof(builder->hooks.reactor),
                                   reactor, sizeof(*reactor));
}

asx_status asx_runtime_builder_set_log_hooks(asx_runtime_builder *builder,
                                             const asx_log_hooks *log) {
    return builder_set_hook_family(builder, &builder->hooks.log, sizeof(builder->hooks.log), log,
                                   sizeof(*log));
}

asx_status asx_runtime_builder_validate(const asx_runtime_builder *builder) {
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (!builder_wait_policy_valid(builder->config.wait_policy)) return ASX_E_INVALID_ARGUMENT;
    if (!builder_leak_response_valid(builder->config.leak_response)) return ASX_E_INVALID_ARGUMENT;
    if (!builder_finalizer_escalation_valid(builder->config.finalizer_escalation)) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_runtime_config_validate(&builder->config);
    if (st != ASX_OK) return st;

    return asx_runtime_hooks_validate(&builder->hooks, ASX_DETERMINISTIC);
}

asx_status asx_runtime_builder_apply_env(asx_runtime_builder *builder, const char *prefix) {
    asx_runtime_builder candidate;
    const char *value;
    char env_name[96];
    asx_runtime_preset preset;
    asx_wait_policy wait_policy;
    asx_io_backend io_backend;
    asx_leak_response leak_response;
    asx_finalizer_escalation escalation;
    uint32_t u32_value;
    uint16_t u16_value;
    uint64_t u64_value;
    asx_status st;

    if (builder == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!builder_surface_ready(builder)) return ASX_E_INVALID_STATE;
    if (asx_runtime_config_validate(&builder->config) != ASX_OK) return ASX_E_INVALID_STATE;

    candidate = *builder;

    value = builder_env_value(prefix, "PRESET", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_preset(value, &preset);
        if (st != ASX_OK) return st;
        st = builder_apply_preset_preserve_hooks(&candidate, preset);
        if (st != ASX_OK) return st;
    }

    value = builder_env_value(prefix, "WAIT_POLICY", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_wait_policy(value, &wait_policy);
        if (st != ASX_OK) return st;
        candidate.config.wait_policy = wait_policy;
    }

    value = builder_env_value(prefix, "IO_BACKEND", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_io_backend(value, &io_backend);
        if (st != ASX_OK) return st;
        st = asx_runtime_config_set_io_backend(&candidate.config, io_backend);
        if (st != ASX_OK) return st;
    }

    value = builder_env_value(prefix, "LEAK_RESPONSE", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_leak_response(value, &leak_response);
        if (st != ASX_OK) return st;
        candidate.config.leak_response = leak_response;
    }

    value = builder_env_value(prefix, "FINALIZER_POLL_BUDGET", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_u32(value, &u32_value);
        if (st != ASX_OK || u32_value == 0u) return ASX_E_INVALID_ARGUMENT;
        candidate.config.finalizer_poll_budget = u32_value;
    }

    value = builder_env_value(prefix, "FINALIZER_TIME_BUDGET_NS", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_u64(value, &u64_value);
        if (st != ASX_OK || u64_value == 0u) return ASX_E_INVALID_ARGUMENT;
        candidate.config.finalizer_time_budget_ns = u64_value;
    }

    value = builder_env_value(prefix, "FINALIZER_ESCALATION", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_finalizer_escalation(value, &escalation);
        if (st != ASX_OK) return st;
        candidate.config.finalizer_escalation = escalation;
    }

    value = builder_env_value(prefix, "MAX_CANCEL_CHAIN_DEPTH", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_u16(value, &u16_value);
        if (st != ASX_OK || u16_value == 0u) return ASX_E_INVALID_ARGUMENT;
        candidate.config.max_cancel_chain_depth = u16_value;
    }

    value = builder_env_value(prefix, "MAX_CANCEL_CHAIN_MEMORY", env_name, sizeof(env_name));
    if (value != NULL) {
        st = parse_u32(value, &u32_value);
        if (st != ASX_OK || u32_value == 0u) return ASX_E_INVALID_ARGUMENT;
        candidate.config.max_cancel_chain_memory = u32_value;
    }

    st = asx_runtime_builder_validate(&candidate);
    if (st != ASX_OK) return st;

    *builder = candidate;
    return ASX_OK;
}

asx_status asx_runtime_builder_build(const asx_runtime_builder *builder, asx_runtime *out_runtime) {
    asx_status st;

    if (builder == NULL || out_runtime == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_builder_validate(builder);
    if (st != ASX_OK) return st;

    return asx_runtime_init(out_runtime, &builder->config, &builder->hooks);
}
