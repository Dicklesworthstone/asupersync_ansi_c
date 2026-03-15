/*
 * asx/runtime/builder.h — explicit runtime builder presets and setters
 *
 * Provides a public builder-style surface for constructing an asx_runtime
 * without mutating a live runtime object in place. The builder owns an
 * asx_runtime_config plus asx_runtime_hooks and exposes preset initializers
 * for common deployment intents.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_BUILDER_H
#define ASX_RUNTIME_BUILDER_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/rt.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ASX_RUNTIME_PRESET_DEFAULT = 0,
    ASX_RUNTIME_PRESET_CURRENT_THREAD = 1,
    ASX_RUNTIME_PRESET_LOW_LATENCY = 2,
    ASX_RUNTIME_PRESET_HIGH_THROUGHPUT = 3
} asx_runtime_preset;

typedef struct {
    asx_runtime_config config;
    asx_runtime_hooks hooks;
    asx_runtime_preset preset;
} asx_runtime_builder;

/* Initialize the builder with default config + deterministic-safe hooks. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_init(asx_runtime_builder *builder);

/* Preset initializers. These all call asx_runtime_builder_init first. */
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_init_current_thread(asx_runtime_builder *builder);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_init_low_latency(asx_runtime_builder *builder);
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_init_high_throughput(asx_runtime_builder *builder);

/* Query the builder's active preset. */
ASX_API ASX_MUST_USE asx_runtime_preset
asx_runtime_builder_preset(const asx_runtime_builder *builder);
ASX_API ASX_MUST_USE const char *asx_runtime_preset_str(asx_runtime_preset preset);

/* Copy out the builder's working config/hooks. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_get_config(const asx_runtime_builder *builder,
                                                               asx_runtime_config *out);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_get_hooks(const asx_runtime_builder *builder,
                                                              asx_runtime_hooks *out);

/* Config setters. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_wait_policy(asx_runtime_builder *builder,
                                                                    asx_wait_policy policy);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_leak_response(asx_runtime_builder *builder,
                                                                      asx_leak_response response);
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_set_finalizer_poll_budget(asx_runtime_builder *builder, uint32_t polls);
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_set_finalizer_time_budget_ns(asx_runtime_builder *builder, uint64_t budget_ns);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_finalizer_escalation(
    asx_runtime_builder *builder, asx_finalizer_escalation escalation);
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_set_max_cancel_chain_depth(asx_runtime_builder *builder, uint16_t depth);
ASX_API ASX_MUST_USE asx_status
asx_runtime_builder_set_max_cancel_chain_memory(asx_runtime_builder *builder, uint32_t bytes);

/* Hook setters. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_hooks(asx_runtime_builder *builder,
                                                              const asx_runtime_hooks *hooks);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_allocator_hooks(
    asx_runtime_builder *builder, const asx_allocator_hooks *allocator);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_clock_hooks(asx_runtime_builder *builder,
                                                                    const asx_clock_hooks *clock);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_entropy_hooks(
    asx_runtime_builder *builder, const asx_entropy_hooks *entropy);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_reactor_hooks(
    asx_runtime_builder *builder, const asx_reactor_hooks *reactor);
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_set_log_hooks(asx_runtime_builder *builder,
                                                                  const asx_log_hooks *log);

/* Validate the working builder payload without building a runtime. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_validate(const asx_runtime_builder *builder);

/* Apply environment-driven builder configuration overrides.
 *
 * Recognized keys are read from the given prefix (or "ASX_RUNTIME_" when
 * prefix is NULL/empty):
 *   PRESET=current-thread|low-latency|high-throughput|default
 *   WAIT_POLICY=busy_spin|yield|sleep
 *   LEAK_RESPONSE=panic|log|silent|recover
 *   FINALIZER_POLL_BUDGET=<u32>
 *   FINALIZER_TIME_BUDGET_NS=<u64>
 *   FINALIZER_ESCALATION=soft|bounded_log|bounded_panic
 *   MAX_CANCEL_CHAIN_DEPTH=<u16>
 *   MAX_CANCEL_CHAIN_MEMORY=<u32>
 *
 * Unset keys leave the builder unchanged. Parsing is atomic: malformed values
 * return ASX_E_INVALID_ARGUMENT and preserve the original builder state. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_apply_env(asx_runtime_builder *builder,
                                                              const char *prefix);

/* Build a runtime from the builder's stored config/hooks. */
ASX_API ASX_MUST_USE asx_status asx_runtime_builder_build(const asx_runtime_builder *builder,
                                                          asx_runtime *out_runtime);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_BUILDER_H */
