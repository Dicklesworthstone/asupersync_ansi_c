/*
 * asx/abi/wasm_abi.h — wasm ABI type contracts and transition helpers
 *
 * Defines the exported type surface for wasm/browser boundary integration:
 *   - ABI version and signature for compatibility checking
 *   - Compatibility classification (compatible, degraded, incompatible)
 *   - Bootstrap, provider, and hook transition records
 *   - Failure/recoverability envelopes
 *   - Request/response/task/scope payload boundary types
 *
 * This header defines type contracts only. It does not implement the
 * full browser or framework runtime — that is deferred to bd-1eqo.16.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ABI_WASM_ABI_H
#define ASX_ABI_WASM_ABI_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ABI version and signature                                           */
/* ------------------------------------------------------------------ */

/* Semantic version for the wasm ABI contract surface.
 * Bumped when any boundary type layout, enum value, or protocol
 * changes in a way that affects cross-boundary compatibility. */
#define ASX_WASM_ABI_VERSION_MAJOR 0
#define ASX_WASM_ABI_VERSION_MINOR 1
#define ASX_WASM_ABI_VERSION_PATCH 0

/* 32-bit packed version: major<<24 | minor<<16 | patch */
#define ASX_WASM_ABI_VERSION                                                                       \
    (((uint32_t)ASX_WASM_ABI_VERSION_MAJOR << 24) | ((uint32_t)ASX_WASM_ABI_VERSION_MINOR << 16) | \
     ((uint32_t)ASX_WASM_ABI_VERSION_PATCH))

/* 64-bit ABI signature: encodes version + feature flags.
 * Used for fast mismatch detection at the wasm boundary. */
typedef uint64_t asx_abi_signature;

/* ABI version descriptor (for runtime introspection) */
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint16_t patch;
    uint32_t feature_flags; /* bitmask of enabled features */
} asx_abi_version;

/* Feature flag bits */
#define ASX_ABI_FEATURE_DETERMINISTIC (1u << 0)
#define ASX_ABI_FEATURE_CHANNELS (1u << 1)
#define ASX_ABI_FEATURE_TIMERS (1u << 2)
#define ASX_ABI_FEATURE_OBLIGATIONS (1u << 3)
#define ASX_ABI_FEATURE_SYMBOLS (1u << 4)
#define ASX_ABI_FEATURE_CODEC_JSON (1u << 5)
#define ASX_ABI_FEATURE_CODEC_BIN (1u << 6)
#define ASX_ABI_FEATURE_GHOST_MONITORS (1u << 7)

/* ------------------------------------------------------------------ */
/* Compatibility classification                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_ABI_COMPATIBLE = 0,  /* full feature parity, safe to use */
    ASX_ABI_DEGRADED = 1,    /* partial features, some ops will return ASX_E_NOT_SUPPORTED */
    ASX_ABI_INCOMPATIBLE = 2 /* major version mismatch, must not proceed */
} asx_abi_compat;

/* Result of an ABI compatibility check */
typedef struct {
    asx_abi_compat compat;
    asx_abi_version local;
    asx_abi_version remote;
    uint32_t missing_features; /* features remote wants but local lacks */
    uint32_t extra_features;   /* features local has but remote doesn't */
} asx_abi_compat_result;

/* ------------------------------------------------------------------ */
/* Bootstrap transition records                                        */
/*                                                                     */
/* Tracks the phases of wasm module initialization, provider binding,  */
/* and hook installation. Each transition is a deterministic record     */
/* that can be replayed for diagnostics.                               */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_BOOT_PHASE_UNINITIALIZED = 0,
    ASX_BOOT_PHASE_ABI_CHECK = 1,     /* checking ABI compatibility */
    ASX_BOOT_PHASE_HOOKS_BIND = 2,    /* binding platform hooks */
    ASX_BOOT_PHASE_PROVIDER_INIT = 3, /* initializing providers */
    ASX_BOOT_PHASE_RUNTIME_INIT = 4,  /* initializing runtime state */
    ASX_BOOT_PHASE_READY = 5,         /* fully operational */
    ASX_BOOT_PHASE_SHUTDOWN = 6,      /* graceful shutdown initiated */
    ASX_BOOT_PHASE_TERMINATED = 7,    /* shutdown complete */
    ASX_BOOT_PHASE_FAILED = 8,        /* fatal error during bootstrap */
    ASX_BOOT_PHASE_COUNT = 9
} asx_boot_phase;

/* Legal bootstrap transitions (encoded as from→to legality) */
typedef struct {
    asx_boot_phase from;
    asx_boot_phase to;
    asx_status result; /* ASX_OK or ASX_E_INVALID_TRANSITION */
} asx_boot_transition_result;

/* ------------------------------------------------------------------ */
/* Provider transition records                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_PROVIDER_UNREGISTERED = 0,
    ASX_PROVIDER_REGISTERING = 1,
    ASX_PROVIDER_ACTIVE = 2,
    ASX_PROVIDER_SUSPENDING = 3,
    ASX_PROVIDER_SUSPENDED = 4,
    ASX_PROVIDER_REMOVING = 5,
    ASX_PROVIDER_REMOVED = 6,
    ASX_PROVIDER_FAILED = 7,
    ASX_PROVIDER_STATE_COUNT = 8
} asx_provider_state;

/* ------------------------------------------------------------------ */
/* Hook transition records                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_HOOK_UNBOUND = 0,
    ASX_HOOK_BINDING = 1,
    ASX_HOOK_BOUND = 2,
    ASX_HOOK_REPLACING = 3,
    ASX_HOOK_UNBOUND_FAILED = 4,
    ASX_HOOK_STATE_COUNT = 5
} asx_hook_state;

/* ------------------------------------------------------------------ */
/* Failure / recoverability envelopes                                  */
/*                                                                     */
/* Wraps any ABI boundary error with enough context for the caller to  */
/* decide whether to retry, degrade, or abort.                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_RECOVER_NONE = 0,     /* not recoverable — abort */
    ASX_RECOVER_RETRY = 1,    /* transient — safe to retry */
    ASX_RECOVER_DEGRADE = 2,  /* feature unavailable — degrade gracefully */
    ASX_RECOVER_RECONNECT = 3 /* boundary lost — re-bootstrap required */
} asx_recoverability;

typedef struct {
    asx_status status; /* underlying error code */
    asx_recoverability recoverability;
    asx_boot_phase boot_phase; /* phase when error occurred */
    uint32_t error_code;       /* provider-specific error code */
    const char *message;       /* human-readable diagnostic (may be NULL) */
} asx_abi_error_envelope;

/* ------------------------------------------------------------------ */
/* Request/Response boundary types                                     */
/*                                                                     */
/* Minimal boundary types for passing structured data across the wasm   */
/* boundary. These use flat byte buffers with length prefixes,          */
/* suitable for wasm linear memory.                                     */
/* ------------------------------------------------------------------ */

/* Opaque byte span for boundary data transfer */
typedef struct {
    const uint8_t *data;
    uint32_t len;
} asx_abi_bytes;

/* Request envelope for cross-boundary calls */
typedef struct {
    uint32_t request_id;   /* caller-assigned correlation ID */
    uint32_t method;       /* operation code */
    asx_abi_bytes payload; /* serialized request body */
    uint32_t timeout_ms;   /* 0 = no timeout */
} asx_abi_request;

/* Response envelope for cross-boundary returns */
typedef struct {
    uint32_t request_id;               /* correlates with request */
    asx_status status;                 /* ASX_OK or error */
    asx_abi_bytes payload;             /* serialized response body */
    asx_recoverability recoverability; /* hint for error handling */
} asx_abi_response;

/* Task-scope boundary context (passed into wasm-spawned tasks) */
typedef struct {
    uint32_t task_id;     /* local task handle */
    uint32_t region_id;   /* owning region handle */
    uint32_t budget_poll; /* poll quota remaining */
    uint32_t budget_cost; /* cost quota remaining (lo32) */
    uint32_t abi_version; /* for version skew detection */
} asx_abi_task_context;

/* ------------------------------------------------------------------ */
/* API: ABI version and compatibility                                  */
/* ------------------------------------------------------------------ */

/* Get the current ABI version descriptor. */
ASX_API asx_abi_version asx_abi_version_current(void);

/* Compute the ABI signature for the current build. */
ASX_API asx_abi_signature asx_abi_signature_compute(void);

/* Check compatibility between local and remote ABI versions. */
ASX_API asx_status asx_abi_check_compat(const asx_abi_version *local, const asx_abi_version *remote,
                                        asx_abi_compat_result *out);

/* Human-readable name for a compatibility level. Never returns NULL. */
ASX_API const char *asx_abi_compat_str(asx_abi_compat compat);

/* ------------------------------------------------------------------ */
/* API: Bootstrap transitions                                          */
/* ------------------------------------------------------------------ */

/* Check if a bootstrap phase transition is legal.
 * Returns ASX_OK if legal, ASX_E_INVALID_TRANSITION otherwise. */
ASX_API asx_status asx_boot_transition_check(asx_boot_phase from, asx_boot_phase to);

/* Human-readable name for a bootstrap phase. Never returns NULL. */
ASX_API const char *asx_boot_phase_str(asx_boot_phase phase);

/* ------------------------------------------------------------------ */
/* API: Provider transitions                                           */
/* ------------------------------------------------------------------ */

/* Check if a provider state transition is legal. */
ASX_API asx_status asx_provider_transition_check(asx_provider_state from, asx_provider_state to);

/* Human-readable name for a provider state. Never returns NULL. */
ASX_API const char *asx_provider_state_str(asx_provider_state state);

/* ------------------------------------------------------------------ */
/* API: Hook transitions                                               */
/* ------------------------------------------------------------------ */

/* Check if a hook state transition is legal. */
ASX_API asx_status asx_hook_transition_check(asx_hook_state from, asx_hook_state to);

/* Human-readable name for a hook state. Never returns NULL. */
ASX_API const char *asx_hook_state_str(asx_hook_state state);

/* ------------------------------------------------------------------ */
/* API: Error envelope                                                 */
/* ------------------------------------------------------------------ */

/* Human-readable name for a recoverability level. Never returns NULL. */
ASX_API const char *asx_recoverability_str(asx_recoverability r);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ABI_WASM_ABI_H */
