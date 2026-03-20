/*
 * remote.c — remote execution, idempotency, saga, and lease implementation
 *
 * Zero dynamic allocation. All operations are deterministic.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/remote/remote.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Remote capability                                                   */
/* ------------------------------------------------------------------ */

void asx_remote_cap_init(asx_remote_cap *cap, uint64_t cap_id, uint32_t flags) {
    cap->cap_id = cap_id;
    cap->flags = flags;
    cap->epoch = 0;
}

int asx_remote_cap_allows(const asx_remote_cap *cap, uint32_t flag) {
    return (cap->flags & flag) == flag;
}

/* ------------------------------------------------------------------ */
/* Remote handle                                                       */
/* ------------------------------------------------------------------ */

void asx_remote_handle_init(asx_remote_handle *h, uint64_t handle_id) {
    h->handle_id = handle_id;
    h->status = ASX_REMOTE_PENDING;
    memset(&h->outcome, 0, sizeof(h->outcome));
    h->epoch = 0;
}

asx_status asx_remote_handle_transition(asx_remote_handle *h, asx_remote_status new_status) {
    /* Legal transitions:
     * PENDING -> RUNNING, CANCELLED
     * RUNNING -> COMPLETED, FAILED, CANCELLED
     * Terminal states cannot transition. */
    switch (h->status) {
    case ASX_REMOTE_PENDING:
        if (new_status == ASX_REMOTE_RUNNING || new_status == ASX_REMOTE_CANCELLED) {
            h->status = new_status;
            return ASX_OK;
        }
        return ASX_E_INVALID_TRANSITION;
    case ASX_REMOTE_RUNNING:
        if (new_status == ASX_REMOTE_COMPLETED || new_status == ASX_REMOTE_FAILED ||
            new_status == ASX_REMOTE_CANCELLED) {
            h->status = new_status;
            return ASX_OK;
        }
        return ASX_E_INVALID_TRANSITION;
    case ASX_REMOTE_COMPLETED:
    case ASX_REMOTE_FAILED:
    case ASX_REMOTE_CANCELLED: return ASX_E_INVALID_TRANSITION;
    }
    return ASX_E_INVALID_ARGUMENT;
}

asx_status asx_remote_handle_complete(asx_remote_handle *h, asx_outcome outcome) {
    asx_status st = asx_remote_handle_transition(h, ASX_REMOTE_COMPLETED);
    if (st == ASX_OK) { h->outcome = outcome; }
    return st;
}

int asx_remote_handle_is_terminal(const asx_remote_handle *h) {
    return h->status == ASX_REMOTE_COMPLETED || h->status == ASX_REMOTE_FAILED ||
           h->status == ASX_REMOTE_CANCELLED;
}

/* ------------------------------------------------------------------ */
/* Remote message                                                      */
/* ------------------------------------------------------------------ */

void asx_remote_message_init(asx_remote_message *msg, asx_remote_msg_kind kind,
                             uint64_t correlation_id) {
    memset(msg, 0, sizeof(*msg));
    msg->kind = kind;
    msg->correlation_id = correlation_id;
}

asx_status asx_remote_message_set_payload(asx_remote_message *msg, const void *data, uint32_t len) {
    if (len > ASX_REMOTE_MSG_PAYLOAD_SIZE) { return ASX_E_BUFFER_TOO_SMALL; }
    if (len > 0 && data == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (len > 0) { memcpy(msg->payload, data, len); }
    msg->payload_len = len;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Idempotency key and record                                          */
/* ------------------------------------------------------------------ */

void asx_idempotency_key_init(asx_idempotency_key *key, uint64_t hash, uint32_t ns) {
    key->key_hash = hash;
    key->namespace_ = ns;
}

int asx_idempotency_key_eq(const asx_idempotency_key *a, const asx_idempotency_key *b) {
    return a->key_hash == b->key_hash && a->namespace_ == b->namespace_;
}

void asx_idempotency_record_init(asx_idempotency_record *rec, asx_idempotency_key key,
                                 uint64_t ttl_ns) {
    rec->key = key;
    rec->state = ASX_IDEMPOTENCY_PENDING;
    rec->result = ASX_OK;
    rec->created_ns = 0;
    rec->ttl_ns = ttl_ns;
}

asx_status asx_idempotency_record_commit(asx_idempotency_record *rec, asx_status result) {
    if (rec->state != ASX_IDEMPOTENCY_PENDING) { return ASX_E_INVALID_TRANSITION; }
    rec->state = ASX_IDEMPOTENCY_COMMITTED;
    rec->result = result;
    return ASX_OK;
}

int asx_idempotency_record_expired(const asx_idempotency_record *rec, uint64_t now_ns) {
    if (rec->ttl_ns == 0) return 0; /* infinite TTL */
    return now_ns >= rec->created_ns + rec->ttl_ns;
}

/* ------------------------------------------------------------------ */
/* Idempotency store                                                   */
/* ------------------------------------------------------------------ */

void asx_idempotency_store_init(asx_idempotency_store *store) { memset(store, 0, sizeof(*store)); }

const asx_idempotency_record *asx_idempotency_store_lookup(const asx_idempotency_store *store,
                                                           const asx_idempotency_key *key) {
    uint32_t i;
    for (i = 0; i < store->count; i++) {
        if (asx_idempotency_key_eq(&store->records[i].key, key)) { return &store->records[i]; }
    }
    return NULL;
}

asx_status asx_idempotency_store_upsert(asx_idempotency_store *store,
                                        const asx_idempotency_record *rec) {
    uint32_t i;
    /* Update existing. */
    for (i = 0; i < store->count; i++) {
        if (asx_idempotency_key_eq(&store->records[i].key, &rec->key)) {
            store->records[i] = *rec;
            return ASX_OK;
        }
    }
    /* Insert new. */
    if (store->count >= ASX_IDEMPOTENCY_STORE_CAPACITY) { return ASX_E_RESOURCE_EXHAUSTED; }
    store->records[store->count++] = *rec;
    return ASX_OK;
}

uint32_t asx_idempotency_store_evict_expired(asx_idempotency_store *store, uint64_t now_ns) {
    uint32_t evicted = 0;
    uint32_t i = 0;
    while (i < store->count) {
        if (asx_idempotency_record_expired(&store->records[i], now_ns)) {
            /* Swap with last and shrink. */
            store->records[i] = store->records[store->count - 1];
            store->count--;
            evicted++;
        } else {
            i++;
        }
    }
    return evicted;
}

/* ------------------------------------------------------------------ */
/* Saga                                                                */
/* ------------------------------------------------------------------ */

void asx_saga_init(asx_saga *saga) {
    memset(saga, 0, sizeof(*saga));
    saga->state = ASX_SAGA_IDLE;
}

asx_status asx_saga_add_step(asx_saga *saga, asx_saga_step_fn execute,
                             asx_saga_compensate_fn compensate, void *user_data) {
    if (saga->step_count >= ASX_SAGA_MAX_STEPS) { return ASX_E_RESOURCE_EXHAUSTED; }
    if (saga->state != ASX_SAGA_IDLE) { return ASX_E_INVALID_STATE; }
    if (execute == NULL) { return ASX_E_INVALID_ARGUMENT; }

    {
        asx_saga_step *step = &saga->steps[saga->step_count++];
        step->execute = execute;
        step->compensate = compensate;
        step->user_data = user_data;
        step->state = ASX_SAGA_STEP_PENDING;
        step->result = ASX_OK;
    }
    return ASX_OK;
}

asx_status asx_saga_execute(asx_saga *saga) {
    if (saga->state == ASX_SAGA_COMPLETED || saga->state == ASX_SAGA_FAILED) {
        return saga->final_result;
    }

    saga->state = ASX_SAGA_RUNNING;

    while (saga->current_step < saga->step_count) {
        asx_saga_step *step = &saga->steps[saga->current_step];
        asx_status st = step->execute(step->user_data);

        step->result = st;
        if (st != ASX_OK) {
            step->state = ASX_SAGA_STEP_FAILED;
            saga->final_result = st;
            /* Trigger compensation. */
            return asx_saga_compensate(saga);
        }

        step->state = ASX_SAGA_STEP_COMPLETED;
        saga->current_step++;
    }

    saga->state = ASX_SAGA_COMPLETED;
    saga->final_result = ASX_OK;
    return ASX_OK;
}

asx_status asx_saga_compensate(asx_saga *saga) {
    uint32_t i;
    saga->state = ASX_SAGA_COMPENSATING;

    /* Compensate completed steps in reverse order. */
    for (i = saga->current_step; i > 0; i--) {
        asx_saga_step *step = &saga->steps[i - 1];
        if (step->state == ASX_SAGA_STEP_COMPLETED && step->compensate != NULL) {
            asx_status st = step->compensate(step->user_data);
            (void)st; /* compensation errors are logged but do not halt */
            step->state = ASX_SAGA_STEP_COMPENSATED;
        }
    }

    saga->state = ASX_SAGA_FAILED;
    return saga->final_result;
}

asx_saga_state asx_saga_get_state(const asx_saga *saga) { return saga->state; }

uint32_t asx_saga_step_count(const asx_saga *saga) { return saga->step_count; }

/* ------------------------------------------------------------------ */
/* Lease                                                               */
/* ------------------------------------------------------------------ */

void asx_lease_init(asx_lease *lease, uint64_t lease_id, uint64_t fence_token, uint64_t ttl_ns) {
    lease->lease_id = lease_id;
    lease->fence_token = fence_token;
    lease->granted_ns = 0;
    lease->ttl_ns = ttl_ns;
    lease->state = ASX_LEASE_ACTIVE;
}

int asx_lease_is_active(const asx_lease *lease, uint64_t now_ns) {
    if (lease->state != ASX_LEASE_ACTIVE) return 0;
    if (lease->ttl_ns == 0) return 1; /* infinite TTL */
    /* Overflow-safe comparison: instead of now < granted + ttl,
     * compute elapsed = now - granted and compare against ttl. */
    if (now_ns < lease->granted_ns) return 0; /* clock wrapped or invalid */
    return (now_ns - lease->granted_ns) < lease->ttl_ns;
}

asx_status asx_lease_renew(asx_lease *lease, uint64_t now_ns, uint64_t new_ttl_ns) {
    if (lease->state != ASX_LEASE_ACTIVE) { return ASX_E_INVALID_STATE; }
    if (lease->ttl_ns > 0 && now_ns >= lease->granted_ns &&
        (now_ns - lease->granted_ns) >= lease->ttl_ns) {
        lease->state = ASX_LEASE_EXPIRED;
        return ASX_E_TIMED_OUT;
    }
    lease->granted_ns = now_ns;
    lease->ttl_ns = new_ttl_ns;
    return ASX_OK;
}

void asx_lease_revoke(asx_lease *lease) { lease->state = ASX_LEASE_REVOKED; }

int asx_lease_validate_fence(const asx_lease *lease, uint64_t token) {
    return token >= lease->fence_token;
}

/* ------------------------------------------------------------------ */
/* Spawn request and acknowledgment                                    */
/* ------------------------------------------------------------------ */

static asx_status spawn_reject_to_status(asx_spawn_reject_reason reason) {
    switch (reason) {
    case ASX_SPAWN_REJECT_CAPACITY: return ASX_E_RESOURCE_EXHAUSTED;
    case ASX_SPAWN_REJECT_UNAUTHORIZED: return ASX_E_PERMISSION_DENIED;
    case ASX_SPAWN_REJECT_DUPLICATE: return ASX_E_ALREADY_EXISTS;
    case ASX_SPAWN_REJECT_INVALID: return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_E_INVALID_ARGUMENT;
}

static const char *spawn_ack_status_str(asx_spawn_ack_status status) {
    switch (status) {
    case ASX_SPAWN_ACK_ACCEPTED: return "accepted";
    case ASX_SPAWN_ACK_REJECTED: return "rejected";
    case ASX_SPAWN_ACK_QUEUED: return "queued";
    }
    return "unknown";
}

static const char *remote_status_str(asx_remote_status status) {
    switch (status) {
    case ASX_REMOTE_PENDING: return "pending";
    case ASX_REMOTE_RUNNING: return "running";
    case ASX_REMOTE_COMPLETED: return "completed";
    case ASX_REMOTE_FAILED: return "failed";
    case ASX_REMOTE_CANCELLED: return "cancelled";
    }
    return "unknown";
}

static const char *saga_state_str(asx_saga_state state) {
    switch (state) {
    case ASX_SAGA_IDLE: return "idle";
    case ASX_SAGA_RUNNING: return "running";
    case ASX_SAGA_COMPENSATING: return "compensating";
    case ASX_SAGA_COMPLETED: return "completed";
    case ASX_SAGA_FAILED: return "failed";
    }
    return "unknown";
}

static const char *lease_state_str(asx_lease_state state) {
    switch (state) {
    case ASX_LEASE_ACTIVE: return "active";
    case ASX_LEASE_EXPIRED: return "expired";
    case ASX_LEASE_REVOKED: return "revoked";
    }
    return "unknown";
}

void asx_spawn_request_init(asx_spawn_request *req, uint64_t request_id, asx_remote_cap cap,
                            asx_idempotency_key idempotency) {
    memset(req, 0, sizeof(*req));
    req->request_id = request_id;
    req->cap = cap;
    req->idempotency = idempotency;
}

void asx_spawn_ack_accepted(asx_spawn_ack *ack, uint64_t request_id, uint64_t handle_id) {
    if (ack == NULL) { return; }
    memset(ack, 0, sizeof(*ack));
    ack->request_id = request_id;
    ack->status = ASX_SPAWN_ACK_ACCEPTED;
    ack->reject_reason = ASX_SPAWN_REJECT_CAPACITY; /* unused */
    ack->handle_id = handle_id;
}

void asx_spawn_ack_rejected(asx_spawn_ack *ack, uint64_t request_id,
                            asx_spawn_reject_reason reason) {
    if (ack == NULL) { return; }
    memset(ack, 0, sizeof(*ack));
    ack->request_id = request_id;
    ack->status = ASX_SPAWN_ACK_REJECTED;
    ack->reject_reason = reason;
    ack->handle_id = 0;
}

void asx_spawn_ack_queued(asx_spawn_ack *ack, uint64_t request_id) {
    if (ack == NULL) { return; }
    memset(ack, 0, sizeof(*ack));
    ack->request_id = request_id;
    ack->status = ASX_SPAWN_ACK_QUEUED;
    ack->reject_reason = ASX_SPAWN_REJECT_CAPACITY; /* unused */
    ack->handle_id = 0;
}

void asx_remote_report_init(asx_remote_report *report, const asx_spawn_request *req,
                            const asx_spawn_ack *ack, const asx_remote_handle *handle,
                            const asx_idempotency_record *rec, const asx_lease *lease,
                            const asx_saga *saga, uint64_t now_ns) {
    if (report == NULL) { return; }
    memset(report, 0, sizeof(*report));
    report->final_status = ASX_E_PENDING;

    if (req != NULL) {
        report->request_id = req->request_id;
        report->idempotency_hash = req->idempotency.key_hash;
    }
    if (ack != NULL) {
        report->ack_present = 1u;
        report->ack_status = ack->status;
        report->reject_reason = ack->reject_reason;
        report->handle_id = ack->handle_id;
        if (ack->status == ASX_SPAWN_ACK_REJECTED) {
            report->final_status = spawn_reject_to_status(ack->reject_reason);
        }
    }
    if (handle != NULL) {
        report->handle_present = 1u;
        report->handle_id = handle->handle_id;
        report->remote_status = handle->status;
        report->handle_terminal = (uint8_t)asx_remote_handle_is_terminal(handle);
        if (handle->status == ASX_REMOTE_FAILED) {
            report->final_status = ASX_E_INVALID_STATE;
        } else if (handle->status == ASX_REMOTE_CANCELLED) {
            report->final_status = ASX_E_CANCELLED;
        } else if (handle->status == ASX_REMOTE_COMPLETED) {
            report->final_status = ASX_OK;
        }
    }
    if (rec != NULL) {
        report->idempotency_hash = rec->key.key_hash;
        report->idempotency_committed = (uint8_t)(rec->state == ASX_IDEMPOTENCY_COMMITTED);
        if (report->final_status == ASX_OK && rec->state == ASX_IDEMPOTENCY_COMMITTED) {
            report->final_status = rec->result;
        }
    }
    if (lease != NULL) {
        report->lease_present = 1u;
        report->lease_id = lease->lease_id;
        report->lease_state = lease->state;
        report->lease_active = (uint8_t)asx_lease_is_active(lease, now_ns);
    }
    if (saga != NULL) {
        uint32_t i;
        report->saga_present = 1u;
        report->saga_state = saga->state;
        report->total_steps = saga->step_count;
        report->reconnect_like_attempts = saga->current_step;
        for (i = 0; i < saga->step_count; i++) {
            if (saga->steps[i].state == ASX_SAGA_STEP_COMPLETED ||
                saga->steps[i].state == ASX_SAGA_STEP_COMPENSATED) {
                report->completed_steps++;
            }
        }
        if (saga->final_result != ASX_OK) {
            report->final_status = saga->final_result;
        } else if (saga->state == ASX_SAGA_COMPLETED && report->final_status == ASX_E_PENDING) {
            report->final_status = ASX_OK;
        }
    }
}

asx_status asx_remote_report_format(const asx_remote_report *report, char *buf, size_t buf_len) {
    int written;
    const char *ack_str;
    const char *remote_str;
    const char *saga_str;
    const char *lease_str;
    char handle_buf[32];

    if (report == NULL || buf == NULL || buf_len == 0u) { return ASX_E_INVALID_ARGUMENT; }

    ack_str = report->ack_present ? spawn_ack_status_str(report->ack_status) : "na";
    remote_str = report->handle_present ? remote_status_str(report->remote_status) : "na";
    saga_str = report->saga_present ? saga_state_str(report->saga_state) : "na";
    lease_str = report->lease_present ? lease_state_str(report->lease_state) : "na";
    if (report->handle_id != 0u) {
        (void)snprintf(handle_buf, sizeof(handle_buf), "%llu",
                       (unsigned long long)report->handle_id);
    } else {
        (void)snprintf(handle_buf, sizeof(handle_buf), "na");
    }

    written = snprintf(buf, buf_len,
                       "req=%llu handle=%s ack=%s remote=%s saga=%s lease=%s final=%d "
                       "steps=%u/%u idemp=%u terminal=%u active=%u attempts=%u",
                       (unsigned long long)report->request_id, handle_buf, ack_str, remote_str,
                       saga_str, lease_str, (int)report->final_status,
                       (unsigned)report->completed_steps, (unsigned)report->total_steps,
                       (unsigned)report->idempotency_committed, (unsigned)report->handle_terminal,
                       (unsigned)report->lease_active, (unsigned)report->reconnect_like_attempts);

    if (written < 0) { return ASX_E_INVALID_STATE; }
    if ((size_t)written >= buf_len) { return ASX_E_BUFFER_TOO_SMALL; }
    return ASX_OK;
}
