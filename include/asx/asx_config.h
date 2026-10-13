/*
 * asx_config.h — compile-time configuration and profile selection
 *
 * Profiles control resource defaults and platform adaptation without
 * altering semantic behavior. All profiles produce identical canonical
 * semantic digests for shared fixture sets.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONFIG_H
#define ASX_CONFIG_H

#include <asx/asx_abi.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Checkpoint-coverage waiver annotations (bd-66l.6)
 *
 * Used by the CI checkpoint-coverage lint gate to suppress false
 * positives for loops that intentionally do not call asx_checkpoint().
 *
 * ASX_CHECKPOINT_WAIVER("reason")      — per-loop waiver
 * ASX_CHECKPOINT_WAIVER_FILE("reason") — file-level waiver (all loops exempt)
 *
 * The reason string must explain why the loop is exempt (e.g.,
 * "kernel-scheduler: budget exhaustion provides bounded termination",
 * "codec-utility: input length bounded by buffer capacity contract").
 * ------------------------------------------------------------------- */
#ifndef ASX_CHECKPOINT_WAIVER
#define ASX_CHECKPOINT_WAIVER(reason) ((void)0)
#endif
#ifndef ASX_CHECKPOINT_WAIVER_FILE
#define ASX_CHECKPOINT_WAIVER_FILE(reason) ((void)0)
#endif

/*
 * Profile selection (exactly one must be defined at compile time).
 * If none is defined, ASX_PROFILE_CORE is assumed.
 */
#if (defined(ASX_PROFILE_CORE) + defined(ASX_PROFILE_POSIX) + defined(ASX_PROFILE_WIN32) +         \
     defined(ASX_PROFILE_FREESTANDING) + defined(ASX_PROFILE_EMBEDDED_ROUTER) +                    \
     defined(ASX_PROFILE_HFT) + defined(ASX_PROFILE_AUTOMOTIVE) + defined(ASX_PROFILE_PARALLEL) +  \
     defined(ASX_PROFILE_BROWSER)) > 1
#error "Exactly one ASX_PROFILE_* macro may be defined per build"
#elif !defined(ASX_PROFILE_CORE) && !defined(ASX_PROFILE_POSIX) && !defined(ASX_PROFILE_WIN32) &&  \
    !defined(ASX_PROFILE_FREESTANDING) && !defined(ASX_PROFILE_EMBEDDED_ROUTER) &&                 \
    !defined(ASX_PROFILE_HFT) && !defined(ASX_PROFILE_AUTOMOTIVE) &&                               \
    !defined(ASX_PROFILE_PARALLEL) && !defined(ASX_PROFILE_BROWSER)
#define ASX_PROFILE_CORE
#endif

/* -------------------------------------------------------------------
 * Compile-time profile capability contract
 *
 * These macros expose the public build contract to downstream code so
 * browser-incompatible surfaces can be gated in preprocessor space as
 * well as at runtime. Browser builds support a minimal-vs-extended
 * split so upstream wasm/browser feature contracts can be represented
 * without changing the default browser lane semantics.
 * ------------------------------------------------------------------- */

#if defined(ASX_PROFILE_BROWSER) &&                                                                \
    ((defined(ASX_BROWSER_PROFILE_MINIMAL) + defined(ASX_BROWSER_PROFILE_EXTENDED)) > 1)
#error "Exactly one ASX_BROWSER_PROFILE_* subprofile may be defined per browser build"
#elif defined(ASX_PROFILE_BROWSER) && !defined(ASX_BROWSER_PROFILE_MINIMAL) &&                     \
    !defined(ASX_BROWSER_PROFILE_EXTENDED)
#define ASX_BROWSER_PROFILE_EXTENDED
#endif

#define ASX_BROWSER_PROFILE_MODE_COUNT 2

#if defined(ASX_PROFILE_BROWSER)
#define ASX_HAS_NATIVE_RUNTIME_SURFACES 0
#define ASX_HAS_NATIVE_IO_DRIVER 0
#define ASX_HAS_BLOCKING_SURFACE 0
#define ASX_HAS_SERVER_SURFACE 0
#define ASX_HAS_GRPC_SURFACE 0
#define ASX_HAS_MESSAGING_SURFACE 0
#define ASX_HAS_TLS_SURFACE 0
#define ASX_HAS_DATABASE_SURFACE 0
#else
#define ASX_HAS_NATIVE_RUNTIME_SURFACES 1
#define ASX_HAS_NATIVE_IO_DRIVER 1
#define ASX_HAS_BLOCKING_SURFACE 1
#define ASX_HAS_SERVER_SURFACE 1
#define ASX_HAS_GRPC_SURFACE 1
#define ASX_HAS_MESSAGING_SURFACE 1
#define ASX_HAS_TLS_SURFACE 1
#define ASX_HAS_DATABASE_SURFACE 1
#endif

/* Browser I/O adapters/pipelines are only present in the extended browser
 * subprofile. Minimal browser builds omit the public adapter-based IO,
 * encoding, and decoding families from the umbrella surface. */
#if defined(ASX_PROFILE_BROWSER) && defined(ASX_BROWSER_PROFILE_MINIMAL)
#define ASX_HAS_BROWSER_IO 0
#else
#define ASX_HAS_BROWSER_IO 1
#endif
#define ASX_HAS_BROWSER_IO_SUBPROFILE_SPLIT 1

/* Backend-/variant-specific network subfeatures are not implemented yet.
 * Keep these explicit so the public build contract can represent the current
 * state without leaving feature absence implicit in docs alone. */
#define ASX_HAS_TLS_NATIVE_ROOTS 0
#define ASX_HAS_TLS_WEBPKI_ROOTS 0
#define ASX_HAS_TLS_ROOT_VARIANTS 0
#define ASX_HAS_DB_SQLITE_BACKEND 0
#define ASX_HAS_DB_POSTGRES_BACKEND 0
#define ASX_HAS_DB_MYSQL_BACKEND 0
#define ASX_HAS_DB_BACKEND_VARIANTS 0
#define ASX_HAS_MESSAGING_KAFKA_BACKEND 0
#define ASX_HAS_MESSAGING_BACKEND_VARIANTS 0
#define ASX_HAS_IO_URING 0

/* Browser trace is only present in the extended browser subprofile.
 * Minimal browser builds omit the public trace/lab/replay family from
 * the umbrella surface while preserving the existing default browser lane. */
#if defined(ASX_PROFILE_BROWSER) && defined(ASX_BROWSER_PROFILE_MINIMAL)
#define ASX_HAS_BROWSER_TRACE 0
#else
#define ASX_HAS_BROWSER_TRACE 1
#endif
#define ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT 1

/* Debug mode: enables ghost monitors (borrow ledger, protocol, linearity, determinism) */
#if !defined(ASX_DEBUG) && !defined(NDEBUG)
#define ASX_DEBUG 1
#endif

/* Ghost monitors: enabled by default when ASX_DEBUG is set.
 * Override with -DASX_DEBUG_GHOST=0 to disable ghost monitors in debug builds. */
#if defined(ASX_DEBUG) && ASX_DEBUG && !defined(ASX_DEBUG_GHOST)
#define ASX_DEBUG_GHOST 1
#endif

/* Deterministic mode: stable ordering, replay identity for fixed input/seed */
#ifndef ASX_DETERMINISTIC
#define ASX_DETERMINISTIC 1
#endif

/* Walking-skeleton fanout cap for child-region tracking. */
#ifndef ASX_MAX_REGION_CHILDREN
#define ASX_MAX_REGION_CHILDREN 8u
#endif
#if (ASX_MAX_REGION_CHILDREN) < 1
#error "ASX_MAX_REGION_CHILDREN must be at least 1"
#endif

/* ------------------------------------------------------------------ */
/* Resource classes                                                     */
/*                                                                     */
/* Resource classes are capability envelopes, not feature switches.     */
/* They scale operational limits (regions, tasks, timers, trace) while  */
/* preserving identical semantic behavior. Used primarily with          */
/* embedded/constrained profiles.                                      */
/*                                                                     */
/*   R1 — tight: aggressive caps for very constrained footprints       */
/*   R2 — balanced: typical router-class device defaults               */
/*   R3 — roomy: higher-capacity embedded/server crossover             */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CLASS_R1 = 0, /* tight: very constrained footprint */
    ASX_CLASS_R2 = 1, /* balanced: typical router-class device */
    ASX_CLASS_R3 = 2, /* roomy: higher-capacity embedded/server */
    ASX_CLASS_COUNT = 3
} asx_resource_class;

/* Return the human-readable name of a resource class. Never returns NULL. */
ASX_API const char *asx_resource_class_name(asx_resource_class cls);

/* Per-class resource limits — documents what R1/R2/R3 actually cap */
typedef struct {
    uint32_t max_regions;
    uint32_t max_tasks;
    uint32_t max_timers;
    uint32_t max_obligations;
    uint32_t max_channels;
    uint32_t max_trace_events;
} asx_resource_limits;

/* Return the default resource limits for a resource class. */
ASX_API asx_resource_limits asx_resource_limits_for_class(asx_resource_class cls);

/* The classes' limits (README "Resource Classes"), the one table that
 * asx_resource_limits_for_class() and a classed build both read. Each
 * literal has the type of the arena macro it can size. */
#define ASX_CLASS_R1_MAX_REGIONS 4
#define ASX_CLASS_R1_MAX_TASKS 16
#define ASX_CLASS_R1_MAX_TIMERS 32u
#define ASX_CLASS_R1_MAX_OBLIGATIONS 16
#define ASX_CLASS_R1_MAX_CHANNELS 8u
#define ASX_CLASS_R1_MAX_TRACE_EVENTS 64u
#define ASX_CLASS_R2_MAX_REGIONS 16
#define ASX_CLASS_R2_MAX_TASKS 64
#define ASX_CLASS_R2_MAX_TIMERS 128u
#define ASX_CLASS_R2_MAX_OBLIGATIONS 64
#define ASX_CLASS_R2_MAX_CHANNELS 32u
#define ASX_CLASS_R2_MAX_TRACE_EVENTS 256u
#define ASX_CLASS_R3_MAX_REGIONS 64
#define ASX_CLASS_R3_MAX_TASKS 256
#define ASX_CLASS_R3_MAX_TIMERS 512u
#define ASX_CLASS_R3_MAX_OBLIGATIONS 256
#define ASX_CLASS_R3_MAX_CHANNELS 128u
#define ASX_CLASS_R3_MAX_TRACE_EVENTS 1024u

/* Build-time resource class: -DASX_RESOURCE_CLASS=1, 2 or 3 (`make
 * RESOURCE_CLASS=...`) sizes the arenas from R1, R2 or R3. Each of
 * ASX_MAX_REGIONS, ASX_MAX_TASKS, ASX_MAX_TIMERS, ASX_MAX_OBLIGATIONS,
 * ASX_MAX_CHANNELS and ASX_TRACE_CAPACITY not set explicitly takes the
 * class's limit, so the class limit is both the footprint and the point of
 * deterministic exhaustion. Without it the arenas keep their unclassed
 * defaults (8 regions, 64 tasks, 128 timers, 128 obligations, 16 channels,
 * 1024 trace events). */
#ifdef ASX_RESOURCE_CLASS
#if ASX_RESOURCE_CLASS == 1
#define ASX_CLASS_MAX_REGIONS ASX_CLASS_R1_MAX_REGIONS
#define ASX_CLASS_MAX_TASKS ASX_CLASS_R1_MAX_TASKS
#define ASX_CLASS_MAX_TIMERS ASX_CLASS_R1_MAX_TIMERS
#define ASX_CLASS_MAX_OBLIGATIONS ASX_CLASS_R1_MAX_OBLIGATIONS
#define ASX_CLASS_MAX_CHANNELS ASX_CLASS_R1_MAX_CHANNELS
#define ASX_CLASS_MAX_TRACE_EVENTS ASX_CLASS_R1_MAX_TRACE_EVENTS
#elif ASX_RESOURCE_CLASS == 2
#define ASX_CLASS_MAX_REGIONS ASX_CLASS_R2_MAX_REGIONS
#define ASX_CLASS_MAX_TASKS ASX_CLASS_R2_MAX_TASKS
#define ASX_CLASS_MAX_TIMERS ASX_CLASS_R2_MAX_TIMERS
#define ASX_CLASS_MAX_OBLIGATIONS ASX_CLASS_R2_MAX_OBLIGATIONS
#define ASX_CLASS_MAX_CHANNELS ASX_CLASS_R2_MAX_CHANNELS
#define ASX_CLASS_MAX_TRACE_EVENTS ASX_CLASS_R2_MAX_TRACE_EVENTS
#elif ASX_RESOURCE_CLASS == 3
#define ASX_CLASS_MAX_REGIONS ASX_CLASS_R3_MAX_REGIONS
#define ASX_CLASS_MAX_TASKS ASX_CLASS_R3_MAX_TASKS
#define ASX_CLASS_MAX_TIMERS ASX_CLASS_R3_MAX_TIMERS
#define ASX_CLASS_MAX_OBLIGATIONS ASX_CLASS_R3_MAX_OBLIGATIONS
#define ASX_CLASS_MAX_CHANNELS ASX_CLASS_R3_MAX_CHANNELS
#define ASX_CLASS_MAX_TRACE_EVENTS ASX_CLASS_R3_MAX_TRACE_EVENTS
#else
#error "ASX_RESOURCE_CLASS must be 1 (R1), 2 (R2) or 3 (R3)"
#endif
#endif

/* Effective resource and public-layout defaults are resolved here, before
 * any individual module header is included. Applications including only
 * runtime/rt.h therefore use the same values as umbrella consumers. */
#ifndef ASX_MAX_REGIONS
#ifdef ASX_CLASS_MAX_REGIONS
#define ASX_MAX_REGIONS ASX_CLASS_MAX_REGIONS
#else
#define ASX_MAX_REGIONS 8
#endif
#endif
#ifndef ASX_MAX_TASKS
#ifdef ASX_CLASS_MAX_TASKS
#define ASX_MAX_TASKS ASX_CLASS_MAX_TASKS
#else
#define ASX_MAX_TASKS 64
#endif
#endif
#ifndef ASX_MAX_OBLIGATIONS
#ifdef ASX_CLASS_MAX_OBLIGATIONS
#define ASX_MAX_OBLIGATIONS ASX_CLASS_MAX_OBLIGATIONS
#else
#define ASX_MAX_OBLIGATIONS 128
#endif
#endif
#ifndef ASX_MAX_TIMERS
#ifdef ASX_CLASS_MAX_TIMERS
#define ASX_MAX_TIMERS ASX_CLASS_MAX_TIMERS
#else
#define ASX_MAX_TIMERS 128u
#endif
#endif
#ifndef ASX_MAX_CHANNELS
#ifdef ASX_CLASS_MAX_CHANNELS
#define ASX_MAX_CHANNELS ASX_CLASS_MAX_CHANNELS
#else
#define ASX_MAX_CHANNELS 16u
#endif
#endif
#ifndef ASX_TRACE_CAPACITY
#ifdef ASX_CLASS_MAX_TRACE_EVENTS
#define ASX_TRACE_CAPACITY ASX_CLASS_MAX_TRACE_EVENTS
#else
#define ASX_TRACE_CAPACITY 1024u
#endif
#endif
#ifndef ASX_REGION_CAPTURE_ARENA_BYTES
#define ASX_REGION_CAPTURE_ARENA_BYTES 16384u
#endif
#ifndef ASX_MAX_MASK_DEPTH
#define ASX_MAX_MASK_DEPTH 64u
#endif
#ifndef ASX_MAX_CANCEL_WITNESSES
#define ASX_MAX_CANCEL_WITNESSES 64u
#endif
#ifndef ASX_CLEANUP_STACK_CAPACITY
#define ASX_CLEANUP_STACK_CAPACITY 32
#endif
#ifndef ASX_MAX_WORKERS
#if defined(ASX_PROFILE_FREESTANDING) || defined(ASX_PROFILE_BROWSER)
#define ASX_MAX_WORKERS 1u
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
#define ASX_MAX_WORKERS 4u
#elif defined(ASX_PROFILE_HFT) || defined(ASX_PROFILE_AUTOMOTIVE)
#define ASX_MAX_WORKERS 16u
#else
#define ASX_MAX_WORKERS 64u
#endif
#endif
#define ASX_MAX_LANES 3u /* READY, CANCEL, TIMED */
#ifndef ASX_LANE_TASK_CAPACITY
#define ASX_LANE_TASK_CAPACITY 64u
#endif
#ifndef ASX_MAX_IO_TOKENS
#define ASX_MAX_IO_TOKENS 64u
#endif
#ifndef ASX_MAX_DEADLINE_MONITORS
#define ASX_MAX_DEADLINE_MONITORS 32u
#endif
#ifndef ASX_MAX_WAKERS
#define ASX_MAX_WAKERS 64u
#endif
#ifndef ASX_MAX_BLOCKING_TASKS
#define ASX_MAX_BLOCKING_TASKS 16u
#endif
#ifndef ASX_MAX_SUSPECTS
#define ASX_MAX_SUSPECTS 4u
#endif
#ifndef ASX_EVIDENCE_SINK_CAPACITY
#define ASX_EVIDENCE_SINK_CAPACITY 64u
#endif
#ifndef ASX_HINDSIGHT_CAPACITY
#define ASX_HINDSIGHT_CAPACITY 256u
#endif
#define ASX_HINDSIGHT_FLUSH_BUFFER_SIZE 8192u
#define ASX_SNAPSHOT_BUFFER_SIZE 4096u
#ifndef ASX_SNAPSHOT_MAX
#define ASX_SNAPSHOT_MAX 8u
#endif
#ifndef ASX_ORACLE_SUITE_MAX
#define ASX_ORACLE_SUITE_MAX 8u
#endif
#ifndef ASX_LAB_MAX_STEPS
#define ASX_LAB_MAX_STEPS 32u
#endif
#ifndef ASX_TASK_GROUP_MAX
#define ASX_TASK_GROUP_MAX 16u
#endif

/* Core containers, capability contexts and synchronization resources. */
#ifndef ASX_CHANNEL_MAX_CAPACITY
#define ASX_CHANNEL_MAX_CAPACITY 64u
#endif
#ifndef ASX_MAX_ONESHOTS
#define ASX_MAX_ONESHOTS 32u
#endif
#ifndef ASX_MAX_BROADCASTS
#define ASX_MAX_BROADCASTS 8u
#endif
#ifndef ASX_BROADCAST_MAX_CAPACITY
#define ASX_BROADCAST_MAX_CAPACITY 32u
#endif
#ifndef ASX_MAX_WATCHES
#define ASX_MAX_WATCHES 16u
#endif
#ifndef ASX_MAX_SESSIONS
#define ASX_MAX_SESSIONS 8u
#endif
#ifndef ASX_SESSION_MAX_CAPACITY
#define ASX_SESSION_MAX_CAPACITY 16u
#endif
#ifndef ASX_MAX_EPOCHS
#define ASX_MAX_EPOCHS 16u
#endif
#ifndef ASX_MAX_EPOCH_OBSERVERS
#define ASX_MAX_EPOCH_OBSERVERS 8u
#endif
#ifndef ASX_AFFINITY_TABLE_CAPACITY
#define ASX_AFFINITY_TABLE_CAPACITY 256u
#endif
#ifndef ASX_SYMBOL_REGISTRY_CAPACITY
#define ASX_SYMBOL_REGISTRY_CAPACITY 256u
#endif
#ifndef ASX_GHOST_RING_CAPACITY
#define ASX_GHOST_RING_CAPACITY 64u
#endif
#ifndef ASX_GHOST_BORROW_TABLE_CAPACITY
#define ASX_GHOST_BORROW_TABLE_CAPACITY 128u
#endif
#ifndef ASX_GHOST_DETERMINISM_CAPACITY
#define ASX_GHOST_DETERMINISM_CAPACITY 256u
#endif
#ifndef ASX_DET_HASH_MAX_ENTRIES
#define ASX_DET_HASH_MAX_ENTRIES 64u
#endif
#ifndef ASX_COMBINATOR_MAX_BRANCHES
#define ASX_COMBINATOR_MAX_BRANCHES 8u
#endif
#ifndef ASX_JOIN_SET_MAX_TASKS
#define ASX_JOIN_SET_MAX_TASKS 16u
#endif
#ifndef ASX_PIPELINE_MAX_STAGES
#define ASX_PIPELINE_MAX_STAGES 8u
#endif
#ifndef ASX_ADAPTIVE_HEDGE_WINDOW
#define ASX_ADAPTIVE_HEDGE_WINDOW 64u
#endif
#ifndef ASX_CX_MACAROON_MAX_CAVEATS
#define ASX_CX_MACAROON_MAX_CAVEATS 8u
#endif
#ifndef ASX_SEMAPHORE_MAX
#define ASX_SEMAPHORE_MAX 16u
#endif
#ifndef ASX_CONTENDED_MUTEX_MAX
#define ASX_CONTENDED_MUTEX_MAX 16u
#endif
#ifndef ASX_NOTIFY_MAX
#define ASX_NOTIFY_MAX 16u
#endif
#ifndef ASX_BARRIER_MAX
#define ASX_BARRIER_MAX 8u
#endif
#ifndef ASX_RWLOCK_MAX
#define ASX_RWLOCK_MAX 16u
#endif
#ifndef ASX_ONCE_MAX
#define ASX_ONCE_MAX 16u
#endif
#ifndef ASX_POOL_MAX
#define ASX_POOL_MAX 8u
#endif
#ifndef ASX_POOL_MAX_RESOURCES
#define ASX_POOL_MAX_RESOURCES 16u
#endif

/* Actor, service, stream, plan and application layouts. */
#ifndef ASX_MAX_ACTORS
#define ASX_MAX_ACTORS 16u
#endif
#ifndef ASX_ACTOR_MAILBOX_CAPACITY
#define ASX_ACTOR_MAILBOX_CAPACITY 64u
#endif
#ifndef ASX_MAX_SUPERVISORS
#define ASX_MAX_SUPERVISORS 8u
#endif
#ifndef ASX_SUPERVISOR_MAX_CHILDREN
#define ASX_SUPERVISOR_MAX_CHILDREN 8u
#endif
#ifndef ASX_SUPERVISOR_MAX_DEPS
#define ASX_SUPERVISOR_MAX_DEPS 4u
#endif
#ifndef ASX_SUPERVISOR_NAME_MAX
#define ASX_SUPERVISOR_NAME_MAX 32u
#endif
#ifndef ASX_SUPERVISOR_MAX_RESTARTS
#define ASX_SUPERVISOR_MAX_RESTARTS 32u
#endif
#ifndef ASX_GEN_SERVER_MAILBOX_SIZE
#define ASX_GEN_SERVER_MAILBOX_SIZE 16u
#endif
#ifndef ASX_GEN_SERVER_MAX_INSTANCES
#define ASX_GEN_SERVER_MAX_INSTANCES 8u
#endif
#ifndef ASX_MAX_SIGNAL_SUBSCRIPTIONS
#define ASX_MAX_SIGNAL_SUBSCRIPTIONS 16u
#endif
#ifndef ASX_BUF_CAPACITY
#define ASX_BUF_CAPACITY 4096u
#endif
#ifndef ASX_SERVICE_BUFFER_CAPACITY
#define ASX_SERVICE_BUFFER_CAPACITY 8u
#endif
#ifndef ASX_SERVICE_BUILDER_MAX_LAYERS
#define ASX_SERVICE_BUILDER_MAX_LAYERS 8u
#endif
#ifndef ASX_SERVICE_LB_MAX_BACKENDS
#define ASX_SERVICE_LB_MAX_BACKENDS 8u
#endif
#ifndef ASX_WORK_POOL_MAX_ITEMS
#define ASX_WORK_POOL_MAX_ITEMS 16u
#endif
#ifndef ASX_SCATTER_GATHER_MAX_ITEMS
#define ASX_SCATTER_GATHER_MAX_ITEMS 8u
#endif
#ifndef ASX_SPORK_PIPELINE_MAX_STAGES
#define ASX_SPORK_PIPELINE_MAX_STAGES 8u
#endif
#ifndef ASX_STREAM_CHUNK_MAX
#define ASX_STREAM_CHUNK_MAX 16u
#endif
#ifndef ASX_STREAM_WINDOW_MAX
#define ASX_STREAM_WINDOW_MAX 16u
#endif
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
#ifndef ASX_METRICS_MAX_ENTRIES
#define ASX_METRICS_MAX_ENTRIES 32u
#endif
#ifndef ASX_METRICS_NAME_MAX
#define ASX_METRICS_NAME_MAX 64u
#endif
#ifndef ASX_DOCTOR_MAX_CHECKS
#define ASX_DOCTOR_MAX_CHECKS 16u
#endif
#ifndef ASX_DOCTOR_PROBE_TIMEOUT_NS
#define ASX_DOCTOR_PROBE_TIMEOUT_NS ((asx_time)2000000000u)
#endif
#ifndef ASX_REPORT_BUF_SIZE
#define ASX_REPORT_BUF_SIZE 4096u
#endif
#ifndef ASX_APP_DEFAULT_POLL_BUDGET
#define ASX_APP_DEFAULT_POLL_BUDGET 10000u
#endif

/* Network, file-system and process resources and public buffers. */
#ifndef ASX_MAX_TCP_LISTENERS
#define ASX_MAX_TCP_LISTENERS 4u
#endif
#ifndef ASX_MAX_TCP_STREAMS
#define ASX_MAX_TCP_STREAMS 16u
#endif
#ifndef ASX_MAX_UDP_SOCKETS
#define ASX_MAX_UDP_SOCKETS 16u
#endif
#ifndef ASX_RESOLVE_MAX_RESULTS
#define ASX_RESOLVE_MAX_RESULTS 4u
#endif
#ifndef ASX_RESOLVER_CACHE_CAPACITY
#define ASX_RESOLVER_CACHE_CAPACITY 8u
#endif
#ifndef ASX_RESOLVER_HOST_CAPACITY
#define ASX_RESOLVER_HOST_CAPACITY 256u /* DNS names are at most 253 octets */
#endif
#ifndef ASX_RESOLVE_MAX_INFLIGHT
#define ASX_RESOLVE_MAX_INFLIGHT 8u /* concurrent native lookups */
#endif
#ifndef ASX_UNIX_PATH_MAX
#define ASX_UNIX_PATH_MAX 108u
#endif
#ifndef ASX_MAX_UNIX_LISTENERS
#define ASX_MAX_UNIX_LISTENERS 4u
#endif
#ifndef ASX_MAX_UNIX_STREAMS
#define ASX_MAX_UNIX_STREAMS 16u
#endif
#ifndef ASX_MAX_UNIX_DGRAM_SOCKETS
#define ASX_MAX_UNIX_DGRAM_SOCKETS 8u
#endif
#ifndef ASX_ANCILLARY_MAX_FDS
#define ASX_ANCILLARY_MAX_FDS 8u
#endif
#ifndef ASX_MAX_PIPES
#define ASX_MAX_PIPES 8u
#endif
#ifndef ASX_TLS_MAX_HOSTNAME
#define ASX_TLS_MAX_HOSTNAME 64u
#endif
#ifndef ASX_TLS_MAX_ALPN
#define ASX_TLS_MAX_ALPN 4u
#endif
#ifndef ASX_TLS_ALPN_MAX_LEN
#define ASX_TLS_ALPN_MAX_LEN 16u
#endif
#ifndef ASX_MAX_TLS_STREAMS
#define ASX_MAX_TLS_STREAMS 8u
#endif
#ifndef ASX_WS_MAX_MESSAGE
#define ASX_WS_MAX_MESSAGE 16384u
#endif
#ifndef ASX_WS_TX_CAPACITY
#define ASX_WS_TX_CAPACITY (ASX_WS_MAX_MESSAGE + 512u)
#endif
#ifndef ASX_WS_HANDSHAKE_MAX
#define ASX_WS_HANDSHAKE_MAX 4096u
#endif
#ifndef ASX_WS_PROTOCOLS_MAX
#define ASX_WS_PROTOCOLS_MAX 128u
#endif
#ifndef ASX_MAX_WS_CONNECTIONS
#define ASX_MAX_WS_CONNECTIONS 8u
#endif
#ifndef ASX_MAX_QUIC_CONNECTIONS
#define ASX_MAX_QUIC_CONNECTIONS 4u
#endif
#ifndef ASX_MAX_QUIC_STREAMS
#define ASX_MAX_QUIC_STREAMS 16u
#endif
#ifndef ASX_QUIC_MAX_DGRAM
#define ASX_QUIC_MAX_DGRAM 1200u
#endif
#ifndef ASX_HTTP_MAX_HEADERS
#define ASX_HTTP_MAX_HEADERS 16u
#endif
#ifndef ASX_HTTP_HEADER_NAME_MAX
#define ASX_HTTP_HEADER_NAME_MAX 64u
#endif
#ifndef ASX_HTTP_HEADER_VALUE_MAX
#define ASX_HTTP_HEADER_VALUE_MAX 256u
#endif
#ifndef ASX_HTTP_BODY_MAX
#define ASX_HTTP_BODY_MAX 4096u
#endif
#ifndef ASX_HTTP_URI_MAX
#define ASX_HTTP_URI_MAX 256u
#endif
#ifndef ASX_HTTP_MAX_ROUTES
#define ASX_HTTP_MAX_ROUTES 16u
#endif
#ifndef ASX_HTTP_MAX_ROUTE_MIDDLEWARE
#define ASX_HTTP_MAX_ROUTE_MIDDLEWARE 8u
#endif
#ifndef ASX_HTTP_MAX_PATH_PARAMS
#define ASX_HTTP_MAX_PATH_PARAMS 8u
#endif
#ifndef ASX_HTTP_PARAM_NAME_MAX
#define ASX_HTTP_PARAM_NAME_MAX 32u
#endif
#ifndef ASX_HTTP_PARAM_VALUE_MAX
#define ASX_HTTP_PARAM_VALUE_MAX 128u
#endif
#ifndef ASX_HTTP_COOKIE_VALUE_MAX
#define ASX_HTTP_COOKIE_VALUE_MAX 128u
#endif
#ifndef ASX_HTTP_SECURITY_PURPOSE_MAX
#define ASX_HTTP_SECURITY_PURPOSE_MAX 32u
#endif
#ifndef ASX_HTTP_MULTIPART_PARTS_MAX
#define ASX_HTTP_MULTIPART_PARTS_MAX 8u
#endif
#ifndef ASX_HTTP_MULTIPART_DATA_MAX
#define ASX_HTTP_MULTIPART_DATA_MAX 512u
#endif
#ifndef ASX_HTTP_MULTIPART_FILENAME_MAX
#define ASX_HTTP_MULTIPART_FILENAME_MAX 64u
#endif
#ifndef ASX_HTTP_PARSER_MAX_FIELDS
#define ASX_HTTP_PARSER_MAX_FIELDS 64u
#endif
#ifndef ASX_HTTP_DEFAULT_MAX_START_LINE
#define ASX_HTTP_DEFAULT_MAX_START_LINE 8192u
#endif
#ifndef ASX_HTTP_DEFAULT_MAX_HEADER_BYTES
#define ASX_HTTP_DEFAULT_MAX_HEADER_BYTES 16384u
#endif
#ifndef ASX_HTTP_DEFAULT_MAX_CHUNK_LINE
#define ASX_HTTP_DEFAULT_MAX_CHUNK_LINE 1024u
#endif
#ifndef ASX_HTTP_DEFAULT_MAX_BODY
#define ASX_HTTP_DEFAULT_MAX_BODY 16777216u /* 16 MiB */
#endif
#ifndef ASX_HTTP_MAX_LEADING_EMPTY_LINES
#define ASX_HTTP_MAX_LEADING_EMPTY_LINES 4u
#endif
#ifndef ASX_HTTP_CONN_HEAD_MAX
#define ASX_HTTP_CONN_HEAD_MAX 8192u
#endif
#ifndef ASX_HTTP_CONN_TX_MAX
#define ASX_HTTP_CONN_TX_MAX                                                                       \
    ((ASX_HTTP_MAX_HEADERS * (ASX_HTTP_HEADER_NAME_MAX + ASX_HTTP_HEADER_VALUE_MAX + 4u)) +        \
     ASX_HTTP_URI_MAX + 512u)
#endif
#ifndef ASX_HTTP_CLIENT_TX_MAX
#define ASX_HTTP_CLIENT_TX_MAX (ASX_HTTP_CONN_TX_MAX + ASX_HTTP_BODY_MAX + 64u)
#endif
#ifndef ASX_HTTP_CLIENT_MAX_INFORMATIONAL
#define ASX_HTTP_CLIENT_MAX_INFORMATIONAL 8u
#endif
#ifndef ASX_HTTP_SERVER_MAX_CONNS
#if defined(ASX_PROFILE_EMBEDDED_ROUTER) || defined(ASX_PROFILE_FREESTANDING)
#define ASX_HTTP_SERVER_MAX_CONNS 4u
#else
#define ASX_HTTP_SERVER_MAX_CONNS 16u
#endif
#endif
#ifndef ASX_HTTP_POOL_MAX_CONNECTIONS
#define ASX_HTTP_POOL_MAX_CONNECTIONS 8u
#endif
#ifndef ASX_GRPC_MSG_MAX
#define ASX_GRPC_MSG_MAX 4096u
#endif
#ifndef ASX_GRPC_MAX_METADATA
#define ASX_GRPC_MAX_METADATA 8u
#endif
#ifndef ASX_GRPC_METADATA_KEY_MAX
#define ASX_GRPC_METADATA_KEY_MAX 64u
#endif
#ifndef ASX_GRPC_METADATA_VALUE_MAX
#define ASX_GRPC_METADATA_VALUE_MAX 128u
#endif
#ifndef ASX_GRPC_MAX_METHODS
#define ASX_GRPC_MAX_METHODS 16u
#endif
#ifndef ASX_GRPC_METHOD_NAME_MAX
#define ASX_GRPC_METHOD_NAME_MAX 128u
#endif
#ifndef ASX_GRPC_SERVICE_NAME_MAX
#define ASX_GRPC_SERVICE_NAME_MAX 128u
#endif
#ifndef ASX_GRPC_MAX_SERVICES
#define ASX_GRPC_MAX_SERVICES 8u
#endif
#ifndef ASX_GRPC_MAX_HEALTH_ENTRIES
#define ASX_GRPC_MAX_HEALTH_ENTRIES 8u
#endif
#ifndef ASX_MSG_MAX_PAYLOAD
#define ASX_MSG_MAX_PAYLOAD 1024u
#endif
#ifndef ASX_MSG_MAX_TOPIC_LEN
#define ASX_MSG_MAX_TOPIC_LEN 64u
#endif
#ifndef ASX_MSG_MAX_TOPICS
#define ASX_MSG_MAX_TOPICS 8u
#endif
#ifndef ASX_MSG_QUEUE_DEPTH
#define ASX_MSG_QUEUE_DEPTH 16u
#endif
#ifndef ASX_MSG_MAX_SUBSCRIBERS
#define ASX_MSG_MAX_SUBSCRIBERS 8u
#endif
#ifndef ASX_DIST_MAX_NODES
#define ASX_DIST_MAX_NODES 8u
#endif
#ifndef ASX_DIST_MAX_PARTITIONS
#define ASX_DIST_MAX_PARTITIONS 16u
#endif
#ifndef ASX_DIST_NODE_NAME_MAX
#define ASX_DIST_NODE_NAME_MAX 32u
#endif
#ifndef ASX_DIST_SNAPSHOT_DATA_MAX
#define ASX_DIST_SNAPSHOT_DATA_MAX 1024u
#endif
#ifndef ASX_DIST_MAX_SNAPSHOTS
#define ASX_DIST_MAX_SNAPSHOTS 8u
#endif
#ifndef ASX_DIST_HASH_RING_SIZE
#define ASX_DIST_HASH_RING_SIZE 64u
#endif
#ifndef ASX_DB_MAX_CONNECTIONS
#define ASX_DB_MAX_CONNECTIONS 4u
#endif
#ifndef ASX_DB_MAX_COLUMNS
#define ASX_DB_MAX_COLUMNS 16u
#endif
#ifndef ASX_DB_MAX_ROWS
#define ASX_DB_MAX_ROWS 64u
#endif
#ifndef ASX_DB_COLUMN_NAME_MAX
#define ASX_DB_COLUMN_NAME_MAX 32u
#endif
#ifndef ASX_DB_CELL_MAX
#define ASX_DB_CELL_MAX 128u
#endif
#ifndef ASX_DB_QUERY_MAX
#define ASX_DB_QUERY_MAX 512u
#endif
#ifndef ASX_DB_DSN_MAX
#define ASX_DB_DSN_MAX 128u
#endif
#ifndef ASX_SERVER_MAX_CONNECTIONS
#define ASX_SERVER_MAX_CONNECTIONS 16u
#endif
#ifndef ASX_MEM_TRANSPORT_MAX_CONNS
#define ASX_MEM_TRANSPORT_MAX_CONNS 16u
#endif
#ifndef ASX_MEM_TRANSPORT_MAX_LISTENERS
#define ASX_MEM_TRANSPORT_MAX_LISTENERS 4u
#endif
#ifndef ASX_MEM_TRANSPORT_BUF_SIZE
#define ASX_MEM_TRANSPORT_BUF_SIZE 1024u
#endif
#ifndef ASX_RECONNECT_TRANSPORT_MAX_ADDR
#define ASX_RECONNECT_TRANSPORT_MAX_ADDR 64u
#endif
#ifndef ASX_WEB_MAX_ROUTES
#define ASX_WEB_MAX_ROUTES 32u
#endif
#ifndef ASX_WEB_PATH_MAX
#define ASX_WEB_PATH_MAX 128u
#endif
#ifndef ASX_WEB_MAX_MIDDLEWARE
#define ASX_WEB_MAX_MIDDLEWARE 8u
#endif
#ifndef ASX_WEB_MAX_QUERY_PARAMS
#define ASX_WEB_MAX_QUERY_PARAMS 16u
#endif
#ifndef ASX_WEB_PARAM_KEY_MAX
#define ASX_WEB_PARAM_KEY_MAX 64u
#endif
#ifndef ASX_WEB_PARAM_VALUE_MAX
#define ASX_WEB_PARAM_VALUE_MAX 256u
#endif
#ifndef ASX_WEB_MAX_SESSIONS
#define ASX_WEB_MAX_SESSIONS 16u
#endif
#ifndef ASX_WEB_SESSION_ID_LEN
#define ASX_WEB_SESSION_ID_LEN 32u
#endif
#ifndef ASX_WEB_SESSION_MAX_DATA
#define ASX_WEB_SESSION_MAX_DATA 512u
#endif
#ifndef ASX_WEB_SSE_MAX_EVENTS
#define ASX_WEB_SSE_MAX_EVENTS 16u
#endif
#ifndef ASX_WEB_SSE_DATA_MAX
#define ASX_WEB_SSE_DATA_MAX 256u
#endif
#ifndef ASX_WEB_SSE_EVENT_NAME_MAX
#define ASX_WEB_SSE_EVENT_NAME_MAX 32u
#endif
#ifndef ASX_WEB_MAX_MULTIPART_PARTS
#define ASX_WEB_MAX_MULTIPART_PARTS 8u
#endif
#ifndef ASX_WEB_MULTIPART_NAME_MAX
#define ASX_WEB_MULTIPART_NAME_MAX 64u
#endif
#ifndef ASX_WEB_MULTIPART_FILENAME_MAX
#define ASX_WEB_MULTIPART_FILENAME_MAX 128u
#endif
#ifndef ASX_WEB_MULTIPART_DATA_MAX
#define ASX_WEB_MULTIPART_DATA_MAX 4096u
#endif
#ifndef ASX_WEB_CORS_MAX_ORIGINS
#define ASX_WEB_CORS_MAX_ORIGINS 8u
#endif
#ifndef ASX_WEB_CORS_ORIGIN_MAX
#define ASX_WEB_CORS_ORIGIN_MAX 128u
#endif
#ifndef ASX_WEB_CSRF_TOKEN_LEN
#define ASX_WEB_CSRF_TOKEN_LEN 32u
#endif
#ifndef ASX_WEB_MAX_STATIC_FILES
#define ASX_WEB_MAX_STATIC_FILES 16u
#endif
#ifndef ASX_MAX_PROCESSES
#define ASX_MAX_PROCESSES 8u
#endif
#ifndef ASX_PROCESS_WAIT_MAX_POLLS
#define ASX_PROCESS_WAIT_MAX_POLLS 10000u
#endif
#ifndef ASX_PROCESS_PROGRAM_MAX
#define ASX_PROCESS_PROGRAM_MAX 96u
#endif
#ifndef ASX_PROCESS_MAX_ARGS
#define ASX_PROCESS_MAX_ARGS 16u
#endif
#ifndef ASX_PROCESS_ARG_MAX
#define ASX_PROCESS_ARG_MAX 128u
#endif
#ifndef ASX_PROCESS_MAX_ENV
#define ASX_PROCESS_MAX_ENV 16u
#endif
#ifndef ASX_PROCESS_ENV_MAX
#define ASX_PROCESS_ENV_MAX 128u
#endif
#ifndef ASX_PROCESS_OUTPUT_MAX
#define ASX_PROCESS_OUTPUT_MAX 4096u
#endif
#ifndef ASX_FS_PATH_MAX
#define ASX_FS_PATH_MAX 192u
#endif
#ifndef ASX_FS_NAME_MAX
#define ASX_FS_NAME_MAX 256u
#endif
#ifndef ASX_MAX_FS_ENTRIES
#define ASX_MAX_FS_ENTRIES 16u
#endif
#ifndef ASX_MAX_OPEN_FILES
#define ASX_MAX_OPEN_FILES 16u
#endif
#ifndef ASX_MAX_OPEN_DIRS
#define ASX_MAX_OPEN_DIRS 8u
#endif
#ifndef ASX_FS_FILE_CAPACITY
#define ASX_FS_FILE_CAPACITY 1024u
#endif

/* Remote message and transaction layouts. */
#ifndef ASX_REMOTE_MSG_PAYLOAD_SIZE
#define ASX_REMOTE_MSG_PAYLOAD_SIZE 256u
#endif
#ifndef ASX_IDEMPOTENCY_STORE_CAPACITY
#define ASX_IDEMPOTENCY_STORE_CAPACITY 16u
#endif
#ifndef ASX_SAGA_MAX_STEPS
#define ASX_SAGA_MAX_STEPS 8u
#endif
#ifndef ASX_REMOTE_REPORT_TEXT_SIZE
#define ASX_REMOTE_REPORT_TEXT_SIZE 256u
#endif

/* ------------------------------------------------------------------ */
/* Trace mode selection                                                */
/*                                                                     */
/* Controls how runtime trace events are retained:                     */
/*   RAM_RING         — bounded in-memory ring buffer (default)        */
/*   PERSISTENT_SPILL — spill to persistent storage path               */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_TRACE_MODE_RAM_RING = 0,        /* in-memory ring (default) */
    ASX_TRACE_MODE_PERSISTENT_SPILL = 1 /* flash/disk spill */
} asx_trace_mode;

/* Trace configuration for embedded/constrained profiles */
typedef struct {
    asx_trace_mode mode;        /* RAM_RING or PERSISTENT_SPILL */
    uint32_t ring_capacity;     /* event slots in RAM ring */
    int wear_safe;              /* 1: minimize flash write cycles */
    uint32_t flush_interval_ms; /* spill interval (0 = on-demand only) */
} asx_trace_config;

/* Initialize trace config with resource-class-appropriate defaults.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if cfg is NULL. */
ASX_API asx_status asx_trace_config_init(asx_trace_config *cfg, asx_resource_class cls);

/* ------------------------------------------------------------------ */
/* Safety profiles                                                     */
/*                                                                     */
/* Orthogonal to platform profiles (ASX_PROFILE_*). Safety profiles    */
/* control invariant-checking overhead and ghost monitor activation.    */
/*                                                                     */
/* ASX_SAFETY_DEBUG    — full ghost monitors, error ledger, linearity  */
/*                       tracking. Intended for development and test.   */
/* ASX_SAFETY_HARDENED — error ledger active, must-use enforced,       */
/*                       ghost monitors disabled, allocator sealable.  */
/*                       Intended for production with diagnostics.     */
/* ASX_SAFETY_RELEASE  — zero-cost stubs for all monitors, minimal    */
/*                       overhead. Intended for performance-critical.   */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_SAFETY_DEBUG = 0,
    ASX_SAFETY_HARDENED = 1,
    ASX_SAFETY_RELEASE = 2
} asx_safety_profile;

/* Auto-select safety profile from compile-time flags if not explicit */
#ifndef ASX_SAFETY_PROFILE_SELECTED
#if defined(ASX_DEBUG) && ASX_DEBUG
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_DEBUG
#elif defined(NDEBUG)
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_RELEASE
#else
#define ASX_SAFETY_PROFILE_SELECTED ASX_SAFETY_HARDENED
#endif
#endif

/* Query the active safety profile at runtime */
ASX_API asx_safety_profile asx_safety_profile_active(void);

/* Return the human-readable name of a safety profile. Never returns NULL. */
ASX_API const char *asx_safety_profile_str(asx_safety_profile profile);

/* ------------------------------------------------------------------ */
/* Fault containment policy                                            */
/*                                                                     */
/* Determines how the runtime responds to invariant violations:        */
/*   FAIL_FAST     — abort immediately (debug builds)                  */
/*   POISON_REGION — poison the owning region, continue others         */
/*   ERROR_ONLY    — return error code, no containment overhead        */
/*                                                                     */
/* The default policy is derived from the active safety profile.       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CONTAIN_FAIL_FAST = 0,     /* debug: halt on first violation */
    ASX_CONTAIN_POISON_REGION = 1, /* hardened: poison region, continue */
    ASX_CONTAIN_ERROR_ONLY = 2     /* release: return error, minimal overhead */
} asx_containment_policy;

/* Return the default containment policy for a safety profile. */
ASX_API asx_containment_policy asx_containment_policy_for_profile(asx_safety_profile profile);

/* Return the active containment policy (derived from active profile). */
ASX_API asx_containment_policy asx_containment_policy_active(void);

/* ------------------------------------------------------------------ */
/* Fault injection (deterministic-mode only)                           */
/*                                                                     */
/* Injects controlled faults into clock, entropy, and allocator paths  */
/* for testing exhaustion/anomaly handling. Active only when            */
/* ASX_DETERMINISTIC is set. No-op stubs in non-deterministic builds.  */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_FAULT_NONE = 0,
    ASX_FAULT_CLOCK_SKEW = 1,    /* adds param ns to each clock read */
    ASX_FAULT_CLOCK_REVERSE = 2, /* subtracts param ns (simulates reversal) */
    ASX_FAULT_ENTROPY_CONST = 3, /* returns param as constant entropy value */
    ASX_FAULT_ALLOC_FAIL = 4     /* allocation returns NULL after trigger */
} asx_fault_kind;

typedef struct {
    asx_fault_kind kind;
    uint64_t param;         /* kind-specific parameter */
    uint32_t trigger_after; /* inject after N calls (0 = immediate) */
    uint32_t trigger_count; /* deactivate after N injections (0 = permanent) */
} asx_fault_injection;

/* Inject a controlled fault for deterministic testing.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if fault is NULL. */
ASX_API asx_status asx_fault_inject(const asx_fault_injection *fault);

/* Clear all active fault injections. Returns ASX_OK. */
ASX_API asx_status asx_fault_clear(void);

/* Return the number of currently active fault injections. */
ASX_API uint32_t asx_fault_injection_count(void);

/* ------------------------------------------------------------------ */
/* Runtime hook contracts                                              */
/* ------------------------------------------------------------------ */

typedef void *(*asx_alloc_fn)(void *ctx, size_t size);
typedef void *(*asx_realloc_fn)(void *ctx, void *ptr, size_t size);
typedef void (*asx_free_fn)(void *ctx, void *ptr);

typedef asx_time (*asx_clock_now_ns_fn)(void *ctx);
typedef uint64_t (*asx_entropy_u64_fn)(void *ctx);

/* Fallible entropy source: writes one word to *out and returns ASX_OK, or
 * returns the status that says why the source could not produce one, with
 * *out untouched. A real (OS or hardware) source reports its failure here
 * instead of inventing a value. */
typedef asx_status (*asx_entropy_try_u64_fn)(void *ctx, uint64_t *out);

typedef asx_status (*asx_reactor_wait_fn)(void *ctx, uint32_t timeout_ms, uint32_t *ready_count);
typedef asx_status (*asx_ghost_reactor_wait_fn)(void *ctx, uint64_t logical_step,
                                                uint32_t *ready_count);

/* Readiness interest / ready bits shared by the reactor hooks and the IO
 * driver (same values as ASX_IO_READABLE / _WRITABLE / _ERROR). */
#define ASX_REACTOR_READABLE 0x01u
#define ASX_REACTOR_WRITABLE 0x02u
#define ASX_REACTOR_ERROR 0x04u

/* One readiness notification: the token supplied at registration plus
 * the ready bits (ERROR/hang-up also sets the interests that were armed so
 * the waiter wakes and observes the failure on its next I/O call). */
typedef struct {
    uint64_t token;
    uint32_t ready;
} asx_reactor_event;

/* Arm one-shot interest for fd: the next readiness for `interest` is
 * reported once with `token`, then the fd is disarmed until armed again.
 * Re-arming an already registered fd replaces its interest and token. */
typedef asx_status (*asx_reactor_register_fn)(void *ctx, int fd, uint32_t interest, uint64_t token);
/* Forget fd. Unknown descriptors are ignored. */
typedef void (*asx_reactor_deregister_fn)(void *ctx, int fd);
/* Wait up to timeout_ms for readiness and collect at most max_events
 * notifications (consuming their one-shot arming). */
typedef asx_status (*asx_reactor_poll_fn)(void *ctx, uint32_t timeout_ms, asx_reactor_event *events,
                                          uint32_t max_events, uint32_t *out_count);
/* Interrupt a wait_fn / poll_fn blocked on another thread (async-signal
 * and thread safe). A notify that races ahead of the wait must still make
 * that wait return promptly. */
typedef void (*asx_reactor_notify_fn)(void *ctx);

typedef void (*asx_log_sink_fn)(void *ctx, int level, const char *message);

typedef struct {
    void *ctx;
    asx_alloc_fn malloc_fn;
    asx_realloc_fn realloc_fn;
    asx_free_fn free_fn;
} asx_allocator_hooks;

typedef struct {
    void *ctx;
    asx_clock_now_ns_fn now_ns_fn;         /* wall or monotonic time */
    asx_clock_now_ns_fn logical_now_ns_fn; /* deterministic logical clock */
} asx_clock_hooks;

typedef struct {
    void *ctx;
    asx_entropy_u64_fn random_u64_fn;
    /* Optional fallible form of the source. When set it supplies every
     * runtime draw (asx_runtime_random_u64 and _secure_u64) and a source
     * failure reaches the caller; random_u64_fn then only serves code that
     * reads the hook table directly. The platform hooks install both. */
    asx_entropy_try_u64_fn try_u64_fn;
} asx_entropy_hooks;

typedef struct {
    void *ctx;
    asx_reactor_wait_fn wait_fn;
    asx_ghost_reactor_wait_fn ghost_wait_fn;
    /* Optional real fd readiness (all three or none). Without them the IO
     * driver keeps its ghost backend. */
    asx_reactor_register_fn register_fn;
    asx_reactor_deregister_fn deregister_fn;
    asx_reactor_poll_fn poll_fn;
    /* Optional cross-thread wakeup (blocking-pool completions). Without
     * it a blocked scheduler notices them at its next idle timeout. */
    asx_reactor_notify_fn notify_fn;
} asx_reactor_hooks;

typedef void (*asx_blocking_job_fn)(void *job_ctx);
typedef asx_status (*asx_blocking_submit_fn)(void *ctx, asx_blocking_job_fn job_fn, void *job_ctx);
typedef void (*asx_blocking_shutdown_fn)(void *ctx);
typedef uint32_t (*asx_blocking_capacity_fn)(void *ctx);

typedef struct {
    void *ctx;
    asx_blocking_submit_fn submit_fn;
    asx_blocking_shutdown_fn shutdown_fn;
    asx_blocking_capacity_fn capacity_fn;
} asx_blocking_hooks;

typedef struct {
    void *ctx;
    asx_log_sink_fn write_fn;
} asx_log_hooks;

typedef struct {
    asx_allocator_hooks allocator;
    asx_clock_hooks clock;
    asx_entropy_hooks entropy;
    asx_reactor_hooks reactor;
    asx_blocking_hooks blocking;
    asx_log_hooks log;
    uint8_t deterministic_seeded_prng; /* 1 when deterministic entropy stream is configured */
    uint8_t allocator_sealed;          /* 1 after asx_runtime_seal_allocator() */
} asx_runtime_hooks;

/* Static arena allocator backend configuration.
 *
 * The static backend is selected by binding allocator hooks to an
 * asx_static_arena instance; it does not change runtime semantics. The caller
 * owns the backing memory and the arena object for at least as long as the hook
 * table can dispatch allocation calls. */
typedef struct {
    uint32_t size;                     /* sizeof(asx_static_arena_config) */
    void *memory;                      /* caller-owned byte span */
    size_t memory_len;                 /* bytes in memory */
    size_t alignment;                  /* power-of-two, >= pointer alignment */
    asx_resource_class resource_class; /* R1/R2/R3 sizing defaults */
    asx_resource_limits limits;        /* default from resource_class */
    uint32_t capture_bytes_per_region;
    uint32_t cleanup_slots_per_region;
    uint32_t trace_ring_capacity;
    uint8_t seal_after_init; /* bind hooks as already sealed */
} asx_static_arena_config;

typedef struct {
    asx_static_arena_config config;
    unsigned char *base;
    size_t capacity;
    size_t used;
    uint32_t allocation_count;
    uint32_t failed_allocation_count;
    uint8_t initialized;
} asx_static_arena;

/* Initialize a static arena config with resource-class defaults. */
ASX_API ASX_MUST_USE asx_status asx_static_arena_config_init(asx_static_arena_config *cfg,
                                                             void *memory, size_t memory_len,
                                                             asx_resource_class resource_class);

/* Initialize arena state. Returns ASX_E_RESOURCE_EXHAUSTED for too-small
 * memory spans and leaves the arena uninitialized on failure. */
ASX_API ASX_MUST_USE asx_status asx_static_arena_init(asx_static_arena *arena,
                                                      const asx_static_arena_config *cfg);

/* Bind an initialized static arena into a hook table's allocator family. */
ASX_API ASX_MUST_USE asx_status asx_static_arena_bind_hooks(asx_static_arena *arena,
                                                            asx_runtime_hooks *hooks);

/* Return bytes consumed in an initialized static arena, or 0 for inactive state. */
ASX_API ASX_MUST_USE size_t asx_static_arena_used(const asx_static_arena *arena);

/* Return allocatable bytes remaining in an initialized static arena. */
ASX_API ASX_MUST_USE size_t asx_static_arena_remaining(const asx_static_arena *arena);

/* Wait policy (resource-plane only; does not affect semantics) */
typedef enum { ASX_WAIT_BUSY_SPIN = 0, ASX_WAIT_YIELD = 1, ASX_WAIT_SLEEP = 2 } asx_wait_policy;

/* Runtime IO backend selection.
 * GHOST is the walking-skeleton hook-driven backend.
 * IO_URING is recognized for contract parity but currently unsupported. */
typedef enum { ASX_IO_BACKEND_GHOST = 0, ASX_IO_BACKEND_IO_URING = 1 } asx_io_backend;

/* Obligation leak response policy */
typedef enum {
    ASX_LEAK_PANIC = 0,
    ASX_LEAK_LOG = 1,
    ASX_LEAK_SILENT = 2,
    ASX_LEAK_RECOVER = 3
} asx_leak_response;

/* Finalizer escalation policy */
typedef enum {
    ASX_FINALIZER_SOFT = 0,
    ASX_FINALIZER_BOUNDED_LOG = 1,
    ASX_FINALIZER_BOUNDED_PANIC = 2
} asx_finalizer_escalation;

/* Leak escalation threshold */
typedef struct {
    uint64_t threshold;
    asx_leak_response escalate_to;
} asx_leak_escalation_config;

/*
 * Runtime configuration.
 * Uses size-field pattern for forward compatibility:
 *   asx_runtime_config cfg;
 *   cfg.size = sizeof(cfg);
 */
typedef struct {
    uint32_t size;                                 /* sizeof(asx_runtime_config) */
    asx_wait_policy wait_policy;                   /* profile-dependent default */
    asx_io_backend io_backend;                     /* default: ghost */
    asx_leak_response leak_response;               /* default: ASX_LEAK_PANIC */
    asx_leak_escalation_config *leak_escalation;   /* optional */
    uint32_t finalizer_poll_budget;                /* default: 100 */
    uint64_t finalizer_time_budget_ns;             /* default: 5000000000 (5s) */
    asx_finalizer_escalation finalizer_escalation; /* default: BOUNDED_LOG */
    uint16_t max_cancel_chain_depth;               /* default: 16 */
    uint32_t max_cancel_chain_memory;              /* default: 4096 */
    /* Cleanup budgets as a hard bound (default 0, Rust's semantics). 0: a
     * cancelled task's cleanup budget becomes its budget when it
     * acknowledges the cancel, and a spent cleanup quota only strengthens
     * the reason to POLL_QUOTA; no task is force-completed, and a
     * cancelled task parked where nothing can wake it stays parked
     * (asx_scheduler_run returns ASX_E_WOULD_BLOCK). 1: a cancelled task
     * that has spent its cleanup polls, counted from the request, is
     * force-completed CANCELLED (ASX_SCHED_EVENT_CANCEL_FORCED), and a
     * stranded cancelled task is polled until it is. 1 is a semantic
     * deviation from Rust, excluded from parity. */
    uint32_t cleanup_hard_bound;
    /* Runtime-owned state storage. NULL (with storage_size 0): the
     * library's static block, as before. Otherwise the runtime's region,
     * task and obligation arenas live in this caller-supplied block, which
     * must be at least asx_runtime_storage_size() bytes (storage_size),
     * aligned to asx_runtime_storage_align(), and must outlive the runtime
     * and not be shared. Init zeroes it; shutdown leaves it as the runtime
     * left it. Changing it is a restart (config reload rejects it). */
    void *storage;
    size_t storage_size;
} asx_runtime_config;

/* Runtime and builder layouts are available to configuration-only consumers
 * so construction can compare the same app/library layout fingerprint. */
typedef struct {
    asx_runtime_config config;
    asx_runtime_hooks hooks;
    asx_leak_escalation_config leak_escalation_storage;
    uint32_t generation; /* nonzero when initialized */
    int initialized;     /* guard flag */
    int has_leak_escalation;
} asx_runtime;

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

/* Initialize config with profile-appropriate defaults. Mismatched builds
 * return ASX_E_CONFIG_MISMATCH without writing cfg; NULL returns
 * ASX_E_INVALID_ARGUMENT. Check the result before using the config. */
ASX_API asx_status asx_runtime_config_init_checked(asx_runtime_config *cfg,
                                                   uint64_t application_fingerprint);
#define asx_runtime_config_init(cfg) asx_runtime_config_init_checked((cfg), ASX_CONFIG_FINGERPRINT)

/* Get a human-readable IO backend name. */
ASX_API ASX_MUST_USE const char *asx_io_backend_str(asx_io_backend backend);

/* Select the IO backend used for runtime bootstrap.
 * Returns ASX_OK for supported selections, ASX_E_PERMISSION_DENIED for known
 * but currently unsupported backends such as io_uring, and
 * ASX_E_INVALID_ARGUMENT for NULL config or unknown enum values. */
ASX_API ASX_MUST_USE asx_status asx_runtime_config_set_io_backend(asx_runtime_config *cfg,
                                                                  asx_io_backend backend);

/* Initialize hook table with safe defaults.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API asx_status asx_runtime_hooks_init_checked(asx_runtime_hooks *hooks,
                                                  uint64_t application_fingerprint);
#define asx_runtime_hooks_init(hooks)                                                              \
    asx_runtime_hooks_init_checked((hooks), ASX_CONFIG_FINGERPRINT)

/* Validate hooks under deterministic/non-deterministic policy.
 * Returns ASX_OK if valid, ASX_E_HOOK_INVALID if constraints violated. */
ASX_API asx_status asx_runtime_hooks_validate(const asx_runtime_hooks *hooks,
                                              int deterministic_mode);

/* Install active runtime hooks.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if hooks is NULL. */
ASX_API asx_status asx_runtime_set_hooks(const asx_runtime_hooks *hooks);

/* Read currently active hooks (never NULL once initialized) */
ASX_API const asx_runtime_hooks *asx_runtime_get_hooks(void);

/* Seal allocator for steady-state no-allocation profiles.
 * Returns ASX_OK on success. After sealing, asx_runtime_alloc returns
 * ASX_E_ALLOCATOR_SEALED. */
ASX_API asx_status asx_runtime_seal_allocator(void);

/* Allocate memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator, ASX_E_ALLOCATOR_SEALED if sealed. */
ASX_API asx_status asx_runtime_alloc(size_t size, void **out_ptr);

/* Reallocate memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator, ASX_E_ALLOCATOR_SEALED if sealed. */
ASX_API asx_status asx_runtime_realloc(void *ptr, size_t size, void **out_ptr);

/* Free memory via hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no allocator. */
ASX_API asx_status asx_runtime_free(void *ptr);

/* Read current time via clock hook. Returns ASX_OK on success,
 * ASX_E_HOOK_MISSING if no clock hook installed. */
ASX_API asx_status asx_runtime_now_ns(asx_time *out_now);

/* Returns nonzero when the effective clock is the runtime's virtual clock
 * (no hooks installed, or the default clock stubs are active). Virtual
 * time only moves when the scheduler finds every runnable task parked and
 * jumps to the earliest armed task timer. */
ASX_API int asx_runtime_clock_is_virtual(void);

/* Current virtual time in nanoseconds (0 after runtime reset). */
ASX_API asx_time asx_runtime_virtual_now(void);

/* Advance virtual time to `to` (no-op if `to` is not in the future). */
ASX_API void asx_runtime_virtual_advance(asx_time to);

/* Read random u64 via the configured entropy stream.
 * Returns ASX_OK on success, ASX_E_HOOK_MISSING if hooks are not installed
 * (or, in a live build, no entropy hook is), ASX_E_INVALID_STATE when a
 * deterministic build's hook table does not provide a deterministic entropy
 * source, and the source's own status when a fallible source
 * (asx_entropy_hooks.try_u64_fn) fails, with *out_value untouched. */
ASX_API asx_status asx_runtime_random_u64(uint64_t *out_value);

/* Read a random u64 that protects something: a WebSocket client key or
 * frame mask, a session ID, a CSRF token. Live builds (ASX_DETERMINISTIC=0)
 * never answer from the default hook table's built-in fixed-seed PRNG
 * (asx_runtime_hooks_init): their draws fail closed, with a status and
 * *out_value untouched, instead of returning a value anyone can recompute:
 *   ASX_E_HOOK_MISSING  no entropy hook installed, or the source reports it
 *                       is unavailable (an OS failure surfaces here);
 *   ASX_E_HOOK_INVALID  the installed source is that built-in PRNG.
 * Profiles with no platform source (FREESTANDING, EMBEDDED_ROUTER, HFT,
 * AUTOMOTIVE) install one through the hook table or the builder. A source
 * the application installs is its own to vouch for. Deterministic builds
 * keep the seeded stream by design (replay needs it) and behave as
 * asx_runtime_random_u64. */
ASX_API ASX_MUST_USE asx_status asx_runtime_random_secure_u64(uint64_t *out_value);
/* Wait for reactor readiness. Returns ready count via out_ready_count.
 * Prefers the native reactor wait hook when one is installed; otherwise,
 * deterministic builds fall back to the ghost reactor hook.
 * Returns ASX_E_HOOK_MISSING if no compatible reactor hook is installed. */
ASX_API asx_status asx_runtime_reactor_wait(uint32_t timeout_ms, uint32_t *out_ready_count,
                                            uint64_t logical_step);

/* Returns nonzero when the installed reactor reports per-fd readiness
 * (register/deregister/poll hooks are all present). */
ASX_API int asx_runtime_reactor_has_readiness(void);

/* Arm one-shot readiness interest for fd (see asx_reactor_register_fn).
 * Returns ASX_E_HOOK_MISSING without a readiness-capable reactor. */
ASX_API asx_status asx_runtime_reactor_register(int fd, uint32_t interest, uint64_t token);

/* Forget fd in the readiness reactor (no-op without one). */
ASX_API void asx_runtime_reactor_deregister(int fd);

/* Collect readiness notifications, waiting up to timeout_ms.
 * Returns ASX_E_HOOK_MISSING without a readiness-capable reactor. */
ASX_API asx_status asx_runtime_reactor_poll(uint32_t timeout_ms, asx_reactor_event *events,
                                            uint32_t max_events, uint32_t *out_count);

/* Interrupt a reactor wait/poll blocked on another thread. Safe to call
 * from any thread; a no-op without a notify-capable reactor. */
ASX_API void asx_runtime_reactor_notify(void);

/* Write a log message at the given severity level.
 * Returns ASX_E_HOOK_MISSING if the runtime hook table is not installed.
 * Returns ASX_OK with no side effects when hooks are installed but no log sink is configured. */
ASX_API asx_status asx_runtime_log_write(int level, const char *message);

/* Application-side build identity. The ordered effective integer values
 * and bootstrap layout facts are folded without allocation or runtime
 * state. This detects accidental build mismatches; it is not authentication.
 * Keep all resource defaults above represented here when adding a knob. */
static inline uint64_t asx_header_config_fingerprint(void) {
    typedef struct {
        char pad;
        uint64_t value;
    } u64_alignment;
    typedef struct {
        char pad;
        void *value;
    } pointer_alignment;
    typedef struct {
        char pad;
        asx_alloc_fn value;
    } function_alignment;
    typedef struct {
        char pad;
        asx_runtime_config value;
    } config_alignment;
    typedef struct {
        char pad;
        asx_runtime_hooks value;
    } hooks_alignment;
    typedef struct {
        char pad;
        asx_runtime value;
    } runtime_alignment;
    typedef struct {
        char pad;
        asx_runtime_builder value;
    } builder_alignment;
    typedef struct {
        char pad;
        asx_leak_escalation_config value;
    } leak_alignment;
    typedef struct {
        char pad;
        asx_allocator_hooks value;
    } allocator_alignment;
    typedef struct {
        char pad;
        asx_clock_hooks value;
    } clock_alignment;
    typedef struct {
        char pad;
        asx_entropy_hooks value;
    } entropy_alignment;
    typedef struct {
        char pad;
        asx_reactor_hooks value;
    } reactor_alignment;
    typedef struct {
        char pad;
        asx_blocking_hooks value;
    } blocking_alignment;
    typedef struct {
        char pad;
        asx_log_hooks value;
    } log_alignment;
    static const uint64_t values[] = {
        ASX_ABI_VERSION,
        CHAR_BIT,
        sizeof(int),
        sizeof(unsigned int),
        sizeof(long),
        sizeof(size_t),
        sizeof(uint32_t),
        sizeof(uint64_t),
        sizeof(void *),
        sizeof(asx_alloc_fn),
        sizeof(asx_status),
        sizeof(asx_resource_class),
        sizeof(asx_runtime_preset),
        sizeof(asx_wait_policy),
        sizeof(asx_io_backend),
        sizeof(asx_leak_response),
        sizeof(asx_finalizer_escalation),
        offsetof(u64_alignment, value),
        offsetof(pointer_alignment, value),
        offsetof(function_alignment, value),
#if defined(ASX_PROFILE_CORE)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_POSIX)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_WIN32)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_FREESTANDING)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_EMBEDDED_ROUTER)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_HFT)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_AUTOMOTIVE)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_PARALLEL)
        1u,
#else
        0u,
#endif
#if defined(ASX_PROFILE_BROWSER)
        1u,
#else
        0u,
#endif
#if defined(ASX_CODEC_BIN)
        1u,
#else
        0u,
#endif
#if defined(ASX_CODEC_JSON)
        1u,
#else
        0u,
#endif
#if defined(ASX_DEBUG) && ASX_DEBUG
        1u,
#else
        0u,
#endif
#if defined(ASX_DEBUG_GHOST) && ASX_DEBUG_GHOST
        1u,
#else
        0u,
#endif
#ifdef ASX_RESOURCE_CLASS
        ASX_RESOURCE_CLASS,
#else
        0u,
#endif
        ASX_DETERMINISTIC,
        ASX_SAFETY_PROFILE_SELECTED,
        ASX_HAS_BROWSER_IO,
        ASX_HAS_BROWSER_TRACE,
        ASX_MAX_REGION_CHILDREN,
        ASX_MAX_REGIONS,
        ASX_MAX_TASKS,
        ASX_MAX_OBLIGATIONS,
        ASX_MAX_TIMERS,
        ASX_MAX_CHANNELS,
        ASX_TRACE_CAPACITY,
        ASX_REGION_CAPTURE_ARENA_BYTES,
        ASX_MAX_MASK_DEPTH,
        ASX_MAX_CANCEL_WITNESSES,
        ASX_CLEANUP_STACK_CAPACITY,
        ASX_MAX_WORKERS,
        ASX_MAX_LANES,
        ASX_LANE_TASK_CAPACITY,
        ASX_MAX_IO_TOKENS,
        ASX_MAX_DEADLINE_MONITORS,
        ASX_MAX_WAKERS,
        ASX_MAX_BLOCKING_TASKS,
        ASX_MAX_SUSPECTS,
        ASX_EVIDENCE_SINK_CAPACITY,
        ASX_HINDSIGHT_CAPACITY,
        ASX_HINDSIGHT_FLUSH_BUFFER_SIZE,
        ASX_SNAPSHOT_BUFFER_SIZE,
        ASX_SNAPSHOT_MAX,
        ASX_ORACLE_SUITE_MAX,
        ASX_LAB_MAX_STEPS,
        ASX_TASK_GROUP_MAX,
        ASX_CHANNEL_MAX_CAPACITY,
        ASX_MAX_ONESHOTS,
        ASX_MAX_BROADCASTS,
        ASX_BROADCAST_MAX_CAPACITY,
        ASX_MAX_WATCHES,
        ASX_MAX_SESSIONS,
        ASX_SESSION_MAX_CAPACITY,
        ASX_MAX_EPOCHS,
        ASX_MAX_EPOCH_OBSERVERS,
        ASX_AFFINITY_TABLE_CAPACITY,
        ASX_SYMBOL_REGISTRY_CAPACITY,
        ASX_GHOST_RING_CAPACITY,
        ASX_GHOST_BORROW_TABLE_CAPACITY,
        ASX_GHOST_DETERMINISM_CAPACITY,
        ASX_DET_HASH_MAX_ENTRIES,
        ASX_COMBINATOR_MAX_BRANCHES,
        ASX_JOIN_SET_MAX_TASKS,
        ASX_PIPELINE_MAX_STAGES,
        ASX_ADAPTIVE_HEDGE_WINDOW,
        ASX_CX_MACAROON_MAX_CAVEATS,
        ASX_SEMAPHORE_MAX,
        ASX_CONTENDED_MUTEX_MAX,
        ASX_NOTIFY_MAX,
        ASX_BARRIER_MAX,
        ASX_RWLOCK_MAX,
        ASX_ONCE_MAX,
        ASX_POOL_MAX,
        ASX_POOL_MAX_RESOURCES,
        ASX_MAX_ACTORS,
        ASX_ACTOR_MAILBOX_CAPACITY,
        ASX_MAX_SUPERVISORS,
        ASX_SUPERVISOR_MAX_CHILDREN,
        ASX_SUPERVISOR_MAX_DEPS,
        ASX_SUPERVISOR_NAME_MAX,
        ASX_SUPERVISOR_MAX_RESTARTS,
        ASX_GEN_SERVER_MAILBOX_SIZE,
        ASX_GEN_SERVER_MAX_INSTANCES,
        ASX_MAX_SIGNAL_SUBSCRIPTIONS,
        ASX_BUF_CAPACITY,
        ASX_SERVICE_BUFFER_CAPACITY,
        ASX_SERVICE_BUILDER_MAX_LAYERS,
        ASX_SERVICE_LB_MAX_BACKENDS,
        ASX_WORK_POOL_MAX_ITEMS,
        ASX_SCATTER_GATHER_MAX_ITEMS,
        ASX_SPORK_PIPELINE_MAX_STAGES,
        ASX_STREAM_CHUNK_MAX,
        ASX_STREAM_WINDOW_MAX,
        ASX_PLAN_MAX_NODES,
        ASX_PLAN_MAX_CHILDREN,
        ASX_PLAN_MAX_LABEL,
        ASX_PLAN_MAX_STEPS,
        ASX_METRICS_MAX_ENTRIES,
        ASX_METRICS_NAME_MAX,
        ASX_DOCTOR_MAX_CHECKS,
        ASX_DOCTOR_PROBE_TIMEOUT_NS,
        ASX_REPORT_BUF_SIZE,
        ASX_APP_DEFAULT_POLL_BUDGET,
        ASX_MAX_TCP_LISTENERS,
        ASX_MAX_TCP_STREAMS,
        ASX_MAX_UDP_SOCKETS,
        ASX_RESOLVE_MAX_RESULTS,
        ASX_RESOLVER_CACHE_CAPACITY,
        ASX_RESOLVER_HOST_CAPACITY,
        ASX_RESOLVE_MAX_INFLIGHT,
        ASX_UNIX_PATH_MAX,
        ASX_MAX_UNIX_LISTENERS,
        ASX_MAX_UNIX_STREAMS,
        ASX_MAX_UNIX_DGRAM_SOCKETS,
        ASX_ANCILLARY_MAX_FDS,
        ASX_MAX_PIPES,
        ASX_TLS_MAX_HOSTNAME,
        ASX_TLS_MAX_ALPN,
        ASX_TLS_ALPN_MAX_LEN,
        ASX_MAX_TLS_STREAMS,
        ASX_WS_MAX_MESSAGE,
        ASX_WS_TX_CAPACITY,
        ASX_WS_HANDSHAKE_MAX,
        ASX_WS_PROTOCOLS_MAX,
        ASX_MAX_WS_CONNECTIONS,
        ASX_MAX_QUIC_CONNECTIONS,
        ASX_MAX_QUIC_STREAMS,
        ASX_QUIC_MAX_DGRAM,
        ASX_HTTP_MAX_HEADERS,
        ASX_HTTP_HEADER_NAME_MAX,
        ASX_HTTP_HEADER_VALUE_MAX,
        ASX_HTTP_BODY_MAX,
        ASX_HTTP_URI_MAX,
        ASX_HTTP_MAX_ROUTES,
        ASX_HTTP_MAX_ROUTE_MIDDLEWARE,
        ASX_HTTP_MAX_PATH_PARAMS,
        ASX_HTTP_PARAM_NAME_MAX,
        ASX_HTTP_PARAM_VALUE_MAX,
        ASX_HTTP_COOKIE_VALUE_MAX,
        ASX_HTTP_SECURITY_PURPOSE_MAX,
        ASX_HTTP_MULTIPART_PARTS_MAX,
        ASX_HTTP_MULTIPART_DATA_MAX,
        ASX_HTTP_MULTIPART_FILENAME_MAX,
        ASX_HTTP_PARSER_MAX_FIELDS,
        ASX_HTTP_DEFAULT_MAX_START_LINE,
        ASX_HTTP_DEFAULT_MAX_HEADER_BYTES,
        ASX_HTTP_DEFAULT_MAX_CHUNK_LINE,
        ASX_HTTP_DEFAULT_MAX_BODY,
        ASX_HTTP_MAX_LEADING_EMPTY_LINES,
        ASX_HTTP_CONN_HEAD_MAX,
        ASX_HTTP_CONN_TX_MAX,
        ASX_HTTP_CLIENT_TX_MAX,
        ASX_HTTP_CLIENT_MAX_INFORMATIONAL,
        ASX_HTTP_SERVER_MAX_CONNS,
        ASX_HTTP_POOL_MAX_CONNECTIONS,
        ASX_GRPC_MSG_MAX,
        ASX_GRPC_MAX_METADATA,
        ASX_GRPC_METADATA_KEY_MAX,
        ASX_GRPC_METADATA_VALUE_MAX,
        ASX_GRPC_MAX_METHODS,
        ASX_GRPC_METHOD_NAME_MAX,
        ASX_GRPC_SERVICE_NAME_MAX,
        ASX_GRPC_MAX_SERVICES,
        ASX_GRPC_MAX_HEALTH_ENTRIES,
        ASX_MSG_MAX_PAYLOAD,
        ASX_MSG_MAX_TOPIC_LEN,
        ASX_MSG_MAX_TOPICS,
        ASX_MSG_QUEUE_DEPTH,
        ASX_MSG_MAX_SUBSCRIBERS,
        ASX_DIST_MAX_NODES,
        ASX_DIST_MAX_PARTITIONS,
        ASX_DIST_NODE_NAME_MAX,
        ASX_DIST_SNAPSHOT_DATA_MAX,
        ASX_DIST_MAX_SNAPSHOTS,
        ASX_DIST_HASH_RING_SIZE,
        ASX_DB_MAX_CONNECTIONS,
        ASX_DB_MAX_COLUMNS,
        ASX_DB_MAX_ROWS,
        ASX_DB_COLUMN_NAME_MAX,
        ASX_DB_CELL_MAX,
        ASX_DB_QUERY_MAX,
        ASX_DB_DSN_MAX,
        ASX_SERVER_MAX_CONNECTIONS,
        ASX_MEM_TRANSPORT_MAX_CONNS,
        ASX_MEM_TRANSPORT_MAX_LISTENERS,
        ASX_MEM_TRANSPORT_BUF_SIZE,
        ASX_RECONNECT_TRANSPORT_MAX_ADDR,
        ASX_WEB_MAX_ROUTES,
        ASX_WEB_PATH_MAX,
        ASX_WEB_MAX_MIDDLEWARE,
        ASX_WEB_MAX_QUERY_PARAMS,
        ASX_WEB_PARAM_KEY_MAX,
        ASX_WEB_PARAM_VALUE_MAX,
        ASX_WEB_MAX_SESSIONS,
        ASX_WEB_SESSION_ID_LEN,
        ASX_WEB_SESSION_MAX_DATA,
        ASX_WEB_SSE_MAX_EVENTS,
        ASX_WEB_SSE_DATA_MAX,
        ASX_WEB_SSE_EVENT_NAME_MAX,
        ASX_WEB_MAX_MULTIPART_PARTS,
        ASX_WEB_MULTIPART_NAME_MAX,
        ASX_WEB_MULTIPART_FILENAME_MAX,
        ASX_WEB_MULTIPART_DATA_MAX,
        ASX_WEB_CORS_MAX_ORIGINS,
        ASX_WEB_CORS_ORIGIN_MAX,
        ASX_WEB_CSRF_TOKEN_LEN,
        ASX_WEB_MAX_STATIC_FILES,
        ASX_MAX_PROCESSES,
        ASX_PROCESS_WAIT_MAX_POLLS,
        ASX_PROCESS_PROGRAM_MAX,
        ASX_PROCESS_MAX_ARGS,
        ASX_PROCESS_ARG_MAX,
        ASX_PROCESS_MAX_ENV,
        ASX_PROCESS_ENV_MAX,
        ASX_PROCESS_OUTPUT_MAX,
        ASX_FS_PATH_MAX,
        ASX_FS_NAME_MAX,
        ASX_MAX_FS_ENTRIES,
        ASX_MAX_OPEN_FILES,
        ASX_MAX_OPEN_DIRS,
        ASX_FS_FILE_CAPACITY,
        ASX_REMOTE_MSG_PAYLOAD_SIZE,
        ASX_IDEMPOTENCY_STORE_CAPACITY,
        ASX_SAGA_MAX_STEPS,
        ASX_REMOTE_REPORT_TEXT_SIZE,
        ASX_ERROR_LEDGER_DEPTH,
        ASX_ERROR_LEDGER_TASK_SLOTS,
        sizeof(asx_runtime_config),
        offsetof(config_alignment, value),
        offsetof(asx_runtime_config, size),
        offsetof(asx_runtime_config, wait_policy),
        offsetof(asx_runtime_config, io_backend),
        offsetof(asx_runtime_config, leak_response),
        offsetof(asx_runtime_config, leak_escalation),
        offsetof(asx_runtime_config, finalizer_poll_budget),
        offsetof(asx_runtime_config, finalizer_time_budget_ns),
        offsetof(asx_runtime_config, finalizer_escalation),
        offsetof(asx_runtime_config, max_cancel_chain_depth),
        offsetof(asx_runtime_config, max_cancel_chain_memory),
        offsetof(asx_runtime_config, cleanup_hard_bound),
        offsetof(asx_runtime_config, storage),
        offsetof(asx_runtime_config, storage_size),
        sizeof(asx_runtime_hooks),
        offsetof(hooks_alignment, value),
        offsetof(asx_runtime_hooks, allocator),
        offsetof(asx_runtime_hooks, clock),
        offsetof(asx_runtime_hooks, entropy),
        offsetof(asx_runtime_hooks, reactor),
        offsetof(asx_runtime_hooks, blocking),
        offsetof(asx_runtime_hooks, log),
        offsetof(asx_runtime_hooks, deterministic_seeded_prng),
        offsetof(asx_runtime_hooks, allocator_sealed),
        sizeof(asx_runtime),
        offsetof(runtime_alignment, value),
        offsetof(asx_runtime, config),
        offsetof(asx_runtime, hooks),
        offsetof(asx_runtime, leak_escalation_storage),
        offsetof(asx_runtime, generation),
        offsetof(asx_runtime, initialized),
        offsetof(asx_runtime, has_leak_escalation),
        sizeof(asx_runtime_builder),
        offsetof(builder_alignment, value),
        offsetof(asx_runtime_builder, config),
        offsetof(asx_runtime_builder, hooks),
        offsetof(asx_runtime_builder, preset),
        sizeof(asx_leak_escalation_config),
        offsetof(leak_alignment, value),
        offsetof(asx_leak_escalation_config, threshold),
        offsetof(asx_leak_escalation_config, escalate_to),
        sizeof(asx_allocator_hooks),
        offsetof(allocator_alignment, value),
        offsetof(asx_allocator_hooks, ctx),
        offsetof(asx_allocator_hooks, malloc_fn),
        offsetof(asx_allocator_hooks, realloc_fn),
        offsetof(asx_allocator_hooks, free_fn),
        sizeof(asx_clock_hooks),
        offsetof(clock_alignment, value),
        offsetof(asx_clock_hooks, ctx),
        offsetof(asx_clock_hooks, now_ns_fn),
        offsetof(asx_clock_hooks, logical_now_ns_fn),
        sizeof(asx_entropy_hooks),
        offsetof(entropy_alignment, value),
        offsetof(asx_entropy_hooks, ctx),
        offsetof(asx_entropy_hooks, random_u64_fn),
        offsetof(asx_entropy_hooks, try_u64_fn),
        sizeof(asx_reactor_hooks),
        offsetof(reactor_alignment, value),
        offsetof(asx_reactor_hooks, ctx),
        offsetof(asx_reactor_hooks, wait_fn),
        offsetof(asx_reactor_hooks, ghost_wait_fn),
        offsetof(asx_reactor_hooks, register_fn),
        offsetof(asx_reactor_hooks, deregister_fn),
        offsetof(asx_reactor_hooks, poll_fn),
        offsetof(asx_reactor_hooks, notify_fn),
        sizeof(asx_blocking_hooks),
        offsetof(blocking_alignment, value),
        offsetof(asx_blocking_hooks, ctx),
        offsetof(asx_blocking_hooks, submit_fn),
        offsetof(asx_blocking_hooks, shutdown_fn),
        offsetof(asx_blocking_hooks, capacity_fn),
        sizeof(asx_log_hooks),
        offsetof(log_alignment, value),
        offsetof(asx_log_hooks, ctx),
        offsetof(asx_log_hooks, write_fn),
    };
    uint64_t digest = UINT64_C(14695981039346656037);
    size_t i;

    for (i = 0u; i < sizeof(values) / sizeof(values[0]); i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded compile-time build identity values") */
        digest ^= values[i];
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

#define ASX_CONFIG_FINGERPRINT (asx_header_config_fingerprint())

/* The library's independently compiled build identity. Compare with the
 * header expression for diagnostics; checked constructors do this for you. */
ASX_API uint64_t asx_runtime_config_fingerprint(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CONFIG_H */
