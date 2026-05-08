# API Misuse Catalog

Public API misuse catalog for the ASX ANSI C runtime kernel.
Covers invalid-usage examples and expected outcomes for every public API surface.

**Purpose:** Compensate for missing Rust compile-time misuse rejection (plan 7.3.4).
Each entry is regression-tested via unit/invariant/fixture artifacts.

## Conformance Gate

`tests/conformance/public_api_misuse_conformance_test.c` is the release-facing
negative conformance gate for this catalog. It freezes exact `asx_status`
outcomes for stale handles, unknown IDs, double close/finalize, illegal
obligation transitions, invalid channel operations, and deadline/cancellation
misuse. It also snapshots the runtime before rejected calls and fails if those
calls mutate the runtime snapshot or emit semantic event, scheduler, or trace
records.

## Error Classification

| Code Family | Range | Meaning |
|------------|-------|---------|
| General | 1xx | NULL pointers, bad arguments, invalid state |
| Transition | 2xx | Illegal state machine transitions |
| Region | 3xx | Region not found/open/poisoned/at capacity |
| Task | 4xx | Task not found/completed, budget exhausted |
| Obligation | 5xx | Already resolved, unresolved leak |
| Cancel | 6xx | Cancelled, witness violations |
| Channel | 7xx | Disconnected, would block, full |
| Timer | 8xx | Not found, pending, duration exceeded |
| Quiescence | 9xx | Tasks active, obligations unresolved |
| Resource | 10xx | Resource exhausted |
| Handle | 11xx | Stale handle (generation mismatch) |
| Hook | 12xx | Missing/invalid hook, determinism, sealed allocator |
| Affinity | 13xx | Domain violation, not bound, already bound |
| Codec | 14xx | Equivalence mismatch |
| Replay | 15xx | Replay mismatch |

## Region Lifecycle

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_region_open(NULL)` | NULL output pointer | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |
| `asx_region_close(INVALID_ID)` | Invalid handle | ASX_E_NOT_FOUND | test_safety_posture:zero_handle_rejected_everywhere |
| `asx_region_close(stale)` | Stale handle | ASX_E_STALE_HANDLE | test_safety_posture:stale_handle_close_after_recycle |
| `asx_region_close(rid)` x2 | Double close | ASX_E_INVALID_TRANSITION | test_safety_posture:double_close_rejected |
| `asx_region_close(poisoned)` | Poisoned region | ASX_E_REGION_POISONED | test_safety_posture:poison_blocks_close |
| `asx_region_get_state(INVALID_ID, &s)` | Invalid handle | ASX_E_NOT_FOUND | test_safety_posture:zero_handle_rejected_everywhere |
| `asx_region_get_state(rid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |
| `asx_region_get_state(wrong_type, &s)` | Type tag mismatch | ASX_E_NOT_FOUND | test_safety_posture:crafted_handle_wrong_type_tag_region |
| `asx_region_get_state(stale, &s)` | Generation mismatch | ASX_E_STALE_HANDLE | handle_safety_test:stale_region_handle_after_recycle |
| `asx_region_poison(INVALID_ID)` | Invalid handle | ASX_E_NOT_FOUND | test_safety_posture:poison_on_invalid_handle_fails |
| `asx_region_poison(poisoned)` | Already poisoned | ASX_OK (idempotent) | test_safety_posture:poison_is_idempotent |
| `asx_region_is_poisoned(rid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:poison_is_poisoned_null_out |
| `asx_region_drain(INVALID_ID, &b)` | Invalid handle | ASX_E_NOT_FOUND | test_api_misuse:drain_invalid_region |
| `asx_region_drain(rid, NULL)` | NULL budget | ASX_E_INVALID_ARGUMENT | test_api_misuse:drain_null_budget |

## Task Lifecycle

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_task_spawn(INVALID_ID, fn, d, &t)` | Invalid region | ASX_E_NOT_FOUND | test_safety_posture:zero_handle_rejected_everywhere |
| `asx_task_spawn(stale, fn, d, &t)` | Stale region | ASX_E_STALE_HANDLE | test_safety_posture:stale_handle_spawn_after_recycle |
| `asx_task_spawn(rid, NULL, d, &t)` | NULL poll function | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_poll_fn_rejected |
| `asx_task_spawn(rid, fn, d, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |
| `asx_task_spawn(closed, fn, d, &t)` | Closed region | ASX_E_REGION_NOT_OPEN | test_safety_posture:spawn_on_closed_region_rejected |
| `asx_task_spawn(poisoned, fn, d, &t)` | Poisoned region | ASX_E_REGION_POISONED | test_safety_posture:poison_blocks_spawn |
| `asx_task_get_state(INVALID_ID, &s)` | Invalid handle | ASX_E_NOT_FOUND | test_safety_posture:zero_handle_rejected_everywhere |
| `asx_task_get_state(tid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |
| `asx_task_get_state(wrong_type, &s)` | Type mismatch | ASX_E_NOT_FOUND | test_safety_posture:crafted_handle_wrong_type_tag_task |
| `asx_task_get_outcome(pending, &o)` | Before completion | ASX_E_TASK_NOT_COMPLETED | test_safety_posture:task_outcome_before_completion_rejected |
| `asx_task_cancel(INVALID_ID, kind)` | Invalid handle | ASX_E_NOT_FOUND | test_api_misuse:cancel_invalid_handle |
| `asx_task_finalize(running_tid)` | Wrong state | ASX_E_INVALID_STATE | test_cancellation:finalize_rejects_wrong_state |
| `asx_checkpoint(tid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_cancellation:checkpoint_null_result_rejected |
| `asx_task_get_cancel_phase(tid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_cancellation:cancel_phase_null_output_rejected |

## Obligation Lifecycle

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_obligation_reserve(INVALID_ID, &o)` | Invalid region | ASX_E_NOT_FOUND | test_safety_posture:zero_handle_rejected_everywhere |
| `asx_obligation_reserve(stale, &o)` | Stale region | ASX_E_STALE_HANDLE | test_safety_posture:stale_handle_obligation_reserve_after_recycle |
| `asx_obligation_reserve(poisoned, &o)` | Poisoned region | ASX_E_REGION_POISONED | test_safety_posture:poison_blocks_obligation_reserve |
| `asx_obligation_reserve(rid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |
| `asx_obligation_commit(oid)` x2 | Double commit | ASX_E_INVALID_TRANSITION | test_safety_posture:obligation_double_commit_rejected |
| `asx_obligation_abort(committed)` | Abort after commit | ASX_E_INVALID_TRANSITION | test_safety_posture:obligation_commit_then_abort_rejected |
| `asx_obligation_get_state(oid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_out_pointers_rejected |

## Scheduler

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_scheduler_run(INVALID_ID, &b)` | Invalid region | ASX_E_NOT_FOUND | test_api_misuse:scheduler_invalid_region |
| `asx_scheduler_run(stale, &b)` | Stale region | ASX_E_STALE_HANDLE | test_safety_posture:stale_handle_scheduler_after_recycle |
| `asx_scheduler_run(rid, NULL)` | NULL budget | ASX_E_INVALID_ARGUMENT | test_safety_posture:null_budget_rejected |
| `asx_scheduler_event_get(OOB, &e)` | Out of bounds index | returns 0 | test_scheduler:scheduler_event_get_out_of_bounds |

## Channel

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_channel_create(INVALID_ID, 16, &c)` | Invalid region | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_create_invalid_region |
| `asx_channel_create(rid, 0, &c)` | Zero capacity | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_create_zero_capacity |
| `asx_channel_create(rid, 16, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_create_null_output |
| `asx_channel_close_sender(cid)` x2 | Double close sender | ASX_E_INVALID_STATE | test_api_misuse:channel_double_close_sender |
| `asx_channel_close_receiver(cid)` x2 | Double close receiver | ASX_E_INVALID_STATE | test_api_misuse:channel_double_close_receiver |
| `asx_channel_try_reserve(closed, &p)` | Reserve on closed | ASX_E_DISCONNECTED | test_api_misuse:channel_reserve_after_close |
| `asx_channel_try_recv(INVALID_ID, &v)` | Invalid handle | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_recv_invalid_handle |
| `asx_channel_get_state(cid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_get_state_null |
| `asx_channel_queue_len(cid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_api_misuse:channel_queue_len_null |

## Cleanup Stack

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_cleanup_push()` x33 | Stack overflow | ASX_E_RESOURCE_EXHAUSTED | test_cleanup:cleanup_capacity_exhaustion |
| `asx_cleanup_pop(stack, bad)` | Invalid handle | ASX_E_NOT_FOUND | test_cleanup:cleanup_pop_invalid_handle |
| `asx_cleanup_pop(stack, h)` x2 | Double pop | ASX_E_NOT_FOUND | test_cleanup:cleanup_pop_double_pop |

## Resource Management

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_resource_snapshot_get(kind, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_resource:resource_snapshot_null_output |
| `asx_resource_snapshot_get(invalid, &s)` | Invalid kind | ASX_E_INVALID_ARGUMENT | test_resource:resource_snapshot_invalid_kind |
| `asx_resource_admit(kind, N)` at cap | Exhausted | ASX_E_RESOURCE_EXHAUSTED | test_resource:resource_admit_fails_when_exhausted |
| `asx_resource_admit(kind, 0)` | Zero count | ASX_E_INVALID_ARGUMENT | test_resource:resource_admit_zero_count |
| `asx_resource_admit(invalid, 1)` | Invalid kind | ASX_E_INVALID_ARGUMENT | test_resource:resource_admit_invalid_kind |
| `asx_resource_region_capture_remaining(INVALID_ID, &b)` | Invalid region | ASX_E_NOT_FOUND | test_resource:resource_region_capture_remaining_invalid_region |
| `asx_resource_region_capture_remaining(rid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_resource:resource_region_capture_remaining_null_output |

## Hook Configuration

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_runtime_hooks_init(NULL)` | NULL hooks | ASX_E_INVALID_ARGUMENT | test_hooks:hooks_init_null_rejected |
| `asx_runtime_hooks_validate(&h, 1)` no clock | Missing logical clock | ASX_E_HOOK_INVALID | test_hooks:hooks_validate_deterministic_needs_logical_clock |
| `asx_runtime_hooks_validate(&h, 1)` ambient | Ambient entropy in deterministic | ASX_E_HOOK_INVALID | test_hooks:hooks_validate_deterministic_forbids_ambient_entropy |
| `asx_runtime_alloc()` after seal | Sealed allocator | ASX_E_ALLOCATOR_SEALED | test_fault_injection:fault_allocator_seal_blocks_alloc |

## Affinity (Debug builds)

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_affinity_bind(ASX_INVALID_ID, dom)` | Invalid entity ID | ASX_E_INVALID_ARGUMENT | test_affinity:affinity_bind_invalid_id |
| `asx_affinity_bind(eid, dom2)` | Already bound to different domain | ASX_E_AFFINITY_ALREADY_BOUND | test_affinity:affinity_bind_already_bound_different_domain |
| `asx_affinity_check(eid)` wrong domain | Access from wrong domain | ASX_E_AFFINITY_VIOLATION | test_affinity:affinity_check_wrong_domain_fails |
| `asx_affinity_check(ASX_INVALID_ID)` | Invalid entity ID | ASX_E_INVALID_ARGUMENT | test_affinity:affinity_check_invalid_id |
| `asx_affinity_transfer(eid, dom)` wrong src | Transfer from wrong domain | ASX_E_AFFINITY_VIOLATION | test_affinity:affinity_transfer_from_wrong_domain_fails |
| `asx_affinity_transfer(untracked, dom)` | Untracked entity | ASX_E_AFFINITY_NOT_BOUND | test_affinity:affinity_transfer_untracked_entity_fails |
| `asx_affinity_transfer(ASX_INVALID_ID, dom)` | Invalid entity ID | ASX_E_INVALID_ARGUMENT | test_affinity:affinity_transfer_invalid_id |
| `asx_affinity_get_domain(eid, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_affinity:affinity_get_domain_null_output |
| `asx_affinity_get_domain(ASX_INVALID_ID, &d)` | Invalid entity ID | ASX_E_INVALID_ARGUMENT | test_affinity:affinity_get_domain_invalid_id |
| `asx_affinity_bind()` at capacity | Table full (256 entries) | ASX_E_AFFINITY_TABLE_FULL | test_affinity:affinity_table_capacity |

## Timer

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_timer_register(NULL, d, ctx, &h)` | NULL wheel | ASX_E_INVALID_ARGUMENT | test_timer_wheel:timer_null_argument_rejection |
| `asx_timer_register(w, d, ctx, NULL)` | NULL output | ASX_E_INVALID_ARGUMENT | test_timer_wheel:timer_null_argument_rejection |
| `asx_timer_register()` at capacity | All slots full | ASX_E_RESOURCE_EXHAUSTED | test_timer_wheel:timer_resource_exhaustion |
| `asx_timer_register(w, MAX, ctx, &h)` | Duration exceeded | ASX_E_TIMER_DURATION_EXCEEDED | test_timer_wheel:timer_duration_exceeded |
| `asx_timer_cancel(NULL, &h)` | NULL wheel | returns false | test_timer_wheel:timer_null_argument_rejection |
| `asx_timer_cancel(w, NULL)` | NULL handle | returns false | test_timer_wheel:timer_null_argument_rejection |
| `asx_timer_cancel(w, &stale)` | Stale handle | returns false | test_timer_wheel:timer_stale_handle_cancel_returns_false |
| `asx_timer_cancel(w, &fired)` | Already fired | returns false | test_timer_wheel:timer_cancel_after_fire_returns_false |
| `asx_timer_cancel(w, &h)` x2 | Double cancel | returns false | test_timer_wheel:timer_double_cancel_returns_false |

## Trace / Replay Continuity

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| `asx_trace_export_binary(NULL, sz, &len)` | NULL buffer | ASX_E_INVALID_ARGUMENT | test_continuity:export_null_buf_returns_invalid |
| `asx_trace_export_binary(buf, sz, NULL)` | NULL output length | ASX_E_INVALID_ARGUMENT | test_continuity:export_null_out_len_returns_invalid |
| `asx_trace_export_binary(buf, small, &len)` | Buffer too small | ASX_E_BUFFER_TOO_SMALL | test_continuity:export_buffer_too_small |
| `asx_trace_import_binary(NULL, sz)` | NULL buffer | ASX_E_INVALID_ARGUMENT | test_continuity:import_null_buf_returns_invalid |
| `asx_trace_import_binary(buf, 10)` | Truncated header | ASX_E_INVALID_ARGUMENT | test_continuity:import_truncated_header_returns_invalid |
| `asx_trace_import_binary(bad_magic)` | Corrupted magic | ASX_E_INVALID_ARGUMENT | test_continuity:import_bad_magic_returns_invalid |
| `asx_trace_import_binary(bad_ver)` | Wrong version | ASX_E_INVALID_ARGUMENT | test_continuity:import_bad_version_returns_invalid |
| `asx_trace_import_binary(trunc)` | Truncated events | ASX_E_INVALID_ARGUMENT | test_continuity:import_truncated_events_returns_invalid |
| `asx_trace_import_binary(bad_digest)` | Corrupted digest | ASX_E_INVALID_ARGUMENT | test_continuity:import_corrupted_digest_returns_invalid |
| `asx_trace_continuity_check(mismatch)` | Divergent replay | ASX_E_REPLAY_MISMATCH | test_continuity:continuity_check_mismatched_trace |

## Handle Safety

| API | Misuse Mode | Expected Error | Test |
|-----|------------|----------------|------|
| Any API with wrong type tag | Region handle as task | ASX_E_NOT_FOUND | test_safety_posture:crafted_handle_wrong_type_tag_* |
| Any API with out-of-range slot | Index > arena capacity | ASX_E_NOT_FOUND | test_safety_posture:crafted_handle_out_of_range_slot |
| Any API with dead slot | Empty/reclaimed slot | ASX_E_NOT_FOUND | test_safety_posture:crafted_handle_dead_slot |
| Any API with stale generation | Old generation number | ASX_E_STALE_HANDLE | test_safety_posture:crafted_handle_wrong_generation |

## Ghost Safety Monitors (Debug builds)

| Misuse Mode | Detection | Test |
|------------|-----------|------|
| Illegal region transition | Ghost violation recorded | test_safety_posture:ghost_records_illegal_region_transition |
| Double obligation resolution | Ghost violation recorded | test_safety_posture:ghost_records_obligation_double_resolution |
| Obligation leak (unresolve) | `asx_ghost_check_obligation_leaks()` | test_ghost:ghost_linearity_leaked_obligation |
| Borrow exclusive while shared | Exclusive rejected | test_ghost_borrow:borrow_exclusive_while_shared_active |
| Borrow shared while exclusive | Shared rejected | test_ghost_borrow:borrow_shared_while_exclusive_active |
| Determinism seal drift | Replay check fails | test_ghost_borrow:determinism_seal_and_replay_drift |

## Containment After Misuse

When a safety violation is detected:
1. **FAIL_FAST (debug):** Returns fault status; caller should abort.
2. **POISON_REGION (hardened):** Poisons the region; spawn/close/schedule blocked; queries survive.
3. **ERROR_ONLY (release):** Returns error without side effects.

| Scenario | Expected Behavior | Test |
|----------|-------------------|------|
| Task error → region containment | Policy-dependent | test_fault_containment:contain_fault_* |
| Poisoned region blocks spawn | ASX_E_REGION_POISONED | test_safety_posture:poison_blocks_spawn |
| Poisoned region allows query | ASX_OK | test_safety_posture:poison_allows_state_query |
| Multi-region isolation | Poison is region-local | test_fault_containment:multi_region_isolation_after_poison |
