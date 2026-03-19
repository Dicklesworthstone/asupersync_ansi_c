/*
 * asx/remote/remote.h — remote execution, idempotency, saga, and lease primitives
 *
 * Provides types and APIs for cross-boundary task execution:
 *   - Remote capabilities and handles for cross-region task management
 *   - Idempotency keys and records for at-most-once execution guarantees
 *   - Saga coordination with step-level error handling and compensation
 *   - Lease management with renewal, expiry, and fencing
 *   - Spawn requests with acknowledgment protocol
 *
 * Design:
 *   - All types are value types with inline storage (no heap allocation).
 *   - Remote operations use the asx_status error taxonomy.
 *   - Idempotency records are keyed by 64-bit deterministic hashes.
 *   - Saga steps run as poll-based state machines.
 *   - Leases use monotonic timestamps from the runtime clock.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_REMOTE_REMOTE_H
#define ASX_REMOTE_REMOTE_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Remote capability — permission token for cross-region operations    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t cap_id;
    uint32_t flags; /* bitmask of permitted operations */
    uint32_t epoch; /* issuing epoch for revocation checks */
} asx_remote_cap;

#define ASX_REMOTE_CAP_SPAWN 0x01u
#define ASX_REMOTE_CAP_CANCEL 0x02u
#define ASX_REMOTE_CAP_QUERY 0x04u
#define ASX_REMOTE_CAP_LEASE 0x08u
#define ASX_REMOTE_CAP_ALL 0x0Fu

/* Initialize a remote capability with the given flags. */
ASX_API void asx_remote_cap_init(asx_remote_cap *cap, uint64_t cap_id, uint32_t flags);

/* Check if a capability permits a given operation flag. */
ASX_API int asx_remote_cap_allows(const asx_remote_cap *cap, uint32_t flag);

/* ------------------------------------------------------------------ */
/* Remote handle — reference to a remotely executing task              */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_REMOTE_PENDING = 0,
    ASX_REMOTE_RUNNING = 1,
    ASX_REMOTE_COMPLETED = 2,
    ASX_REMOTE_FAILED = 3,
    ASX_REMOTE_CANCELLED = 4
} asx_remote_status;

typedef struct {
    uint64_t handle_id;
    asx_remote_status status;
    asx_outcome outcome;
    uint32_t epoch;
} asx_remote_handle;

/* Initialize a remote handle in pending state. */
ASX_API void asx_remote_handle_init(asx_remote_handle *h, uint64_t handle_id);

/* Transition a remote handle to a new status. Returns ASX_E_INVALID_TRANSITION
 * if the transition is not legal. */
ASX_API ASX_MUST_USE asx_status asx_remote_handle_transition(asx_remote_handle *h,
                                                             asx_remote_status new_status);

/* Complete a remote handle with an outcome. */
ASX_API ASX_MUST_USE asx_status asx_remote_handle_complete(asx_remote_handle *h,
                                                           asx_outcome outcome);

/* Check if a remote handle has reached a terminal state. */
ASX_API int asx_remote_handle_is_terminal(const asx_remote_handle *h);

/* ------------------------------------------------------------------ */
/* Remote message — serializable request/response envelope             */
/* ------------------------------------------------------------------ */

#ifndef ASX_REMOTE_MSG_PAYLOAD_SIZE
#define ASX_REMOTE_MSG_PAYLOAD_SIZE 256u
#endif

typedef enum {
    ASX_REMOTE_MSG_SPAWN = 0,
    ASX_REMOTE_MSG_CANCEL = 1,
    ASX_REMOTE_MSG_QUERY = 2,
    ASX_REMOTE_MSG_RESULT = 3,
    ASX_REMOTE_MSG_ACK = 4
} asx_remote_msg_kind;

typedef struct {
    asx_remote_msg_kind kind;
    uint64_t correlation_id;
    uint64_t sender_cap_id;
    uint8_t payload[ASX_REMOTE_MSG_PAYLOAD_SIZE];
    uint32_t payload_len;
} asx_remote_message;

/* Initialize a remote message. */
ASX_API void asx_remote_message_init(asx_remote_message *msg, asx_remote_msg_kind kind,
                                     uint64_t correlation_id);

/* Set message payload. Returns ASX_E_BUFFER_TOO_SMALL if data exceeds capacity. */
ASX_API ASX_MUST_USE asx_status asx_remote_message_set_payload(asx_remote_message *msg,
                                                               const void *data, uint32_t len);

/* ------------------------------------------------------------------ */
/* Idempotency key and record — at-most-once execution guarantee       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t key_hash;   /* deterministic hash of the idempotency key */
    uint32_t namespace_; /* key namespace for collision avoidance */
} asx_idempotency_key;

typedef enum {
    ASX_IDEMPOTENCY_PENDING = 0,
    ASX_IDEMPOTENCY_COMMITTED = 1,
    ASX_IDEMPOTENCY_EXPIRED = 2
} asx_idempotency_state;

typedef struct {
    asx_idempotency_key key;
    asx_idempotency_state state;
    asx_status result;   /* cached result from first execution */
    uint64_t created_ns; /* monotonic timestamp */
    uint64_t ttl_ns;     /* time-to-live for the record */
} asx_idempotency_record;

/* Initialize an idempotency key from a hash and namespace. */
ASX_API void asx_idempotency_key_init(asx_idempotency_key *key, uint64_t hash, uint32_t ns);

/* Check if two idempotency keys are equal. */
ASX_API int asx_idempotency_key_eq(const asx_idempotency_key *a, const asx_idempotency_key *b);

/* Initialize an idempotency record in pending state. */
ASX_API void asx_idempotency_record_init(asx_idempotency_record *rec, asx_idempotency_key key,
                                         uint64_t ttl_ns);

/* Commit an idempotency record with a result. */
ASX_API ASX_MUST_USE asx_status asx_idempotency_record_commit(asx_idempotency_record *rec,
                                                              asx_status result);

/* Check if a record has expired given the current time. */
ASX_API int asx_idempotency_record_expired(const asx_idempotency_record *rec, uint64_t now_ns);

/* ------------------------------------------------------------------ */
/* Idempotency store — bounded cache of idempotency records            */
/* ------------------------------------------------------------------ */

#ifndef ASX_IDEMPOTENCY_STORE_CAPACITY
#define ASX_IDEMPOTENCY_STORE_CAPACITY 16u
#endif

typedef struct {
    asx_idempotency_record records[ASX_IDEMPOTENCY_STORE_CAPACITY];
    uint32_t count;
} asx_idempotency_store;

/* Initialize an empty idempotency store. */
ASX_API void asx_idempotency_store_init(asx_idempotency_store *store);

/* Look up a record by key. Returns NULL if not found. */
ASX_API const asx_idempotency_record *asx_idempotency_store_lookup(
    const asx_idempotency_store *store, const asx_idempotency_key *key);

/* Insert or update a record. Returns ASX_E_RESOURCE_EXHAUSTED if full. */
ASX_API ASX_MUST_USE asx_status asx_idempotency_store_upsert(asx_idempotency_store *store,
                                                             const asx_idempotency_record *rec);

/* Evict expired records given the current time. Returns count evicted. */
ASX_API uint32_t asx_idempotency_store_evict_expired(asx_idempotency_store *store, uint64_t now_ns);

/* ------------------------------------------------------------------ */
/* Saga — multi-step compensating transaction coordination             */
/* ------------------------------------------------------------------ */

#ifndef ASX_SAGA_MAX_STEPS
#define ASX_SAGA_MAX_STEPS 8u
#endif

typedef asx_status (*asx_saga_step_fn)(void *user_data);
typedef asx_status (*asx_saga_compensate_fn)(void *user_data);

typedef enum {
    ASX_SAGA_STEP_PENDING = 0,
    ASX_SAGA_STEP_COMPLETED = 1,
    ASX_SAGA_STEP_FAILED = 2,
    ASX_SAGA_STEP_COMPENSATED = 3
} asx_saga_step_state;

typedef struct {
    asx_saga_step_fn execute;
    asx_saga_compensate_fn compensate;
    void *user_data;
    asx_saga_step_state state;
    asx_status result;
} asx_saga_step;

typedef enum {
    ASX_SAGA_IDLE = 0,
    ASX_SAGA_RUNNING = 1,
    ASX_SAGA_COMPENSATING = 2,
    ASX_SAGA_COMPLETED = 3,
    ASX_SAGA_FAILED = 4
} asx_saga_state;

typedef struct {
    asx_saga_step steps[ASX_SAGA_MAX_STEPS];
    uint32_t step_count;
    uint32_t current_step;
    asx_saga_state state;
    asx_status final_result;
} asx_saga;

/* Initialize an empty saga. */
ASX_API void asx_saga_init(asx_saga *saga);

/* Add a step with execute and compensate functions. */
ASX_API ASX_MUST_USE asx_status asx_saga_add_step(asx_saga *saga, asx_saga_step_fn execute,
                                                  asx_saga_compensate_fn compensate,
                                                  void *user_data);

/* Execute the saga forward. Returns ASX_OK when all steps complete,
 * ASX_E_PENDING when more steps remain, or an error if a step fails
 * (which triggers compensation). */
ASX_API ASX_MUST_USE asx_status asx_saga_execute(asx_saga *saga);

/* Run compensation for completed steps in reverse order.
 * Returns ASX_OK when all compensations complete. */
ASX_API ASX_MUST_USE asx_status asx_saga_compensate(asx_saga *saga);

/* Return the current saga state. */
ASX_API asx_saga_state asx_saga_get_state(const asx_saga *saga);

/* Return the number of steps registered. */
ASX_API uint32_t asx_saga_step_count(const asx_saga *saga);

/* ------------------------------------------------------------------ */
/* Lease — time-bounded resource reservation with fencing              */
/* ------------------------------------------------------------------ */

typedef enum { ASX_LEASE_ACTIVE = 0, ASX_LEASE_EXPIRED = 1, ASX_LEASE_REVOKED = 2 } asx_lease_state;

typedef struct {
    uint64_t lease_id;
    uint64_t fence_token; /* monotonically increasing fencing token */
    uint64_t granted_ns;  /* monotonic timestamp of grant */
    uint64_t ttl_ns;      /* time-to-live */
    asx_lease_state state;
} asx_lease;

/* Initialize a lease with the given TTL. */
ASX_API void asx_lease_init(asx_lease *lease, uint64_t lease_id, uint64_t fence_token,
                            uint64_t ttl_ns);

/* Check if a lease is still active given the current time. */
ASX_API int asx_lease_is_active(const asx_lease *lease, uint64_t now_ns);

/* Renew a lease, extending its TTL from the current time. */
ASX_API ASX_MUST_USE asx_status asx_lease_renew(asx_lease *lease, uint64_t now_ns,
                                                uint64_t new_ttl_ns);

/* Revoke a lease immediately. */
ASX_API void asx_lease_revoke(asx_lease *lease);

/* Validate a fencing token (must be >= lease's fence_token). */
ASX_API int asx_lease_validate_fence(const asx_lease *lease, uint64_t token);

/* ------------------------------------------------------------------ */
/* Spawn request and acknowledgment protocol                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_SPAWN_ACK_ACCEPTED = 0,
    ASX_SPAWN_ACK_REJECTED = 1,
    ASX_SPAWN_ACK_QUEUED = 2
} asx_spawn_ack_status;

typedef enum {
    ASX_SPAWN_REJECT_CAPACITY = 0,
    ASX_SPAWN_REJECT_UNAUTHORIZED = 1,
    ASX_SPAWN_REJECT_DUPLICATE = 2,
    ASX_SPAWN_REJECT_INVALID = 3
} asx_spawn_reject_reason;

typedef struct {
    uint64_t request_id;
    asx_remote_cap cap;
    asx_idempotency_key idempotency;
    uint8_t payload[ASX_REMOTE_MSG_PAYLOAD_SIZE];
    uint32_t payload_len;
} asx_spawn_request;

typedef struct {
    uint64_t request_id;
    asx_spawn_ack_status status;
    asx_spawn_reject_reason reject_reason; /* valid only when status == REJECTED */
    uint64_t handle_id;                    /* valid only when status == ACCEPTED */
} asx_spawn_ack;

/* ------------------------------------------------------------------ */
/* Remote execution report — operator-facing diagnostic artifact       */
/* ------------------------------------------------------------------ */

#ifndef ASX_REMOTE_REPORT_TEXT_SIZE
#define ASX_REMOTE_REPORT_TEXT_SIZE 256u
#endif

typedef struct {
    uint64_t request_id;
    uint64_t handle_id;
    uint64_t lease_id;
    uint64_t idempotency_hash;
    asx_spawn_ack_status ack_status;
    asx_spawn_reject_reason reject_reason;
    asx_remote_status remote_status;
    asx_saga_state saga_state;
    asx_lease_state lease_state;
    asx_status final_status;
    uint32_t completed_steps;
    uint32_t total_steps;
    uint32_t reconnect_like_attempts;
    uint8_t ack_present;
    uint8_t handle_present;
    uint8_t saga_present;
    uint8_t lease_present;
    uint8_t idempotency_committed;
    uint8_t handle_terminal;
    uint8_t lease_active;
} asx_remote_report;

/* Initialize a spawn request. */
ASX_API void asx_spawn_request_init(asx_spawn_request *req, uint64_t request_id, asx_remote_cap cap,
                                    asx_idempotency_key idempotency);

/* Create an acceptance acknowledgment. */
ASX_API void asx_spawn_ack_accepted(asx_spawn_ack *ack, uint64_t request_id, uint64_t handle_id);

/* Create a rejection acknowledgment. */
ASX_API void asx_spawn_ack_rejected(asx_spawn_ack *ack, uint64_t request_id,
                                    asx_spawn_reject_reason reason);

/* Create a queued acknowledgment when execution has been accepted for later start. */
ASX_API void asx_spawn_ack_queued(asx_spawn_ack *ack, uint64_t request_id);

/* Build an operator-facing remote execution report from current primitive state. */
ASX_API void asx_remote_report_init(asx_remote_report *report, const asx_spawn_request *req,
                                    const asx_spawn_ack *ack, const asx_remote_handle *handle,
                                    const asx_idempotency_record *rec, const asx_lease *lease,
                                    const asx_saga *saga, uint64_t now_ns);

/* Render a deterministic single-line diagnostic summary into caller storage. */
ASX_API ASX_MUST_USE asx_status asx_remote_report_format(const asx_remote_report *report, char *buf,
                                                         size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_REMOTE_REMOTE_H */
