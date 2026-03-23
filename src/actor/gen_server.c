/*
 * gen_server.c — OTP-style generic request/reply server
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/gen_server.h>
#include <string.h>

typedef struct {
    asx_gen_server_callbacks callbacks;
    void *state;
    asx_gen_server_state server_state;
    asx_gen_server_msg mailbox[ASX_GEN_SERVER_MAILBOX_SIZE];
    uint32_t mb_head;
    uint32_t mb_count;
    uint32_t generation;
    uint32_t calls_handled;
    uint32_t casts_handled;
    char name[32];
    uint8_t alive;
} gen_server_slot;

static gen_server_slot g_servers[ASX_GEN_SERVER_MAX_INSTANCES];
static uint32_t g_next_gen = 1u;

static gen_server_slot *gs_lookup(asx_gen_server_ref ref) {
    gen_server_slot *s;

    if (ref.slot >= ASX_GEN_SERVER_MAX_INSTANCES) return NULL;
    s = &g_servers[ref.slot];
    if (!s->alive) return NULL;
    if (s->generation != ref.generation) return NULL;
    return s;
}

static asx_status gs_enqueue(gen_server_slot *s, const asx_gen_server_msg *msg) {
    uint32_t tail;

    if (s->mb_count >= ASX_GEN_SERVER_MAILBOX_SIZE) return ASX_E_WOULD_BLOCK;
    tail = (s->mb_head + s->mb_count) % ASX_GEN_SERVER_MAILBOX_SIZE;
    s->mailbox[tail] = *msg;
    s->mb_count++;
    return ASX_OK;
}

static asx_gen_server_msg *gs_peek(gen_server_slot *s) {
    if (s->mb_count == 0u) return NULL;
    return &s->mailbox[s->mb_head];
}

static void gs_dequeue(gen_server_slot *s) {
    if (s->mb_count == 0u) return;
    s->mb_head = (s->mb_head + 1u) % ASX_GEN_SERVER_MAILBOX_SIZE;
    s->mb_count--;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_gen_server_start(asx_gen_server_ref *out,
                                 const asx_gen_server_callbacks *callbacks, void *init_args) {
    uint32_t i;
    gen_server_slot *s;
    asx_status st;

    if (out == NULL || callbacks == NULL) return ASX_E_INVALID_ARGUMENT;
    if (callbacks->init == NULL || callbacks->handle_call == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0u; i < ASX_GEN_SERVER_MAX_INSTANCES; i++) {
        if (!g_servers[i].alive) break;
    }
    if (i >= ASX_GEN_SERVER_MAX_INSTANCES) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_servers[i];
    memset(s, 0, sizeof(*s));
    s->callbacks = *callbacks;
    s->generation = g_next_gen++;
    s->alive = 1u;

    st = s->callbacks.init(init_args, &s->state);
    if (st != ASX_OK) {
        s->alive = 0u;
        return st;
    }

    s->server_state = ASX_GEN_SERVER_RUNNING;
    out->slot = i;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_gen_server_call(asx_gen_server_ref ref, uint64_t request, uint64_t *reply) {
    gen_server_slot *s;
    asx_gen_server_msg msg;
    asx_status st;

    if (reply == NULL) return ASX_E_INVALID_ARGUMENT;
    s = gs_lookup(ref);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->server_state != ASX_GEN_SERVER_RUNNING) return ASX_E_INVALID_STATE;

    /* In deterministic mode, call is synchronous — dispatch immediately */
    if (s->callbacks.handle_call != NULL) {
        st = s->callbacks.handle_call(s->state, request, reply);
        s->calls_handled++;
        return st;
    }

    /* Fallback: enqueue and wait (for async mode, future extension) */
    memset(&msg, 0, sizeof(msg));
    msg.kind = ASX_GEN_SERVER_MSG_CALL;
    msg.payload = request;
    return gs_enqueue(s, &msg);
}

asx_status asx_gen_server_cast(asx_gen_server_ref ref, uint64_t message) {
    gen_server_slot *s;

    s = gs_lookup(ref);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->server_state != ASX_GEN_SERVER_RUNNING) return ASX_E_INVALID_STATE;

    /* In deterministic mode, cast dispatches immediately */
    if (s->callbacks.handle_cast != NULL) {
        asx_status st = s->callbacks.handle_cast(s->state, message);
        s->casts_handled++;
        return st;
    }
    return ASX_OK;
}

asx_status asx_gen_server_stop(asx_gen_server_ref ref) {
    gen_server_slot *s;

    s = gs_lookup(ref);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    if (s->callbacks.terminate != NULL) {
        s->callbacks.terminate(s->state, ASX_OK);
    }
    s->server_state = ASX_GEN_SERVER_STOPPED;
    s->alive = 0u;
    return ASX_OK;
}

asx_gen_server_state asx_gen_server_get_state(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return ASX_GEN_SERVER_STOPPED;
    return s->server_state;
}

asx_status asx_gen_server_poll(asx_gen_server_ref ref) {
    gen_server_slot *s;
    asx_gen_server_msg *msg;

    s = gs_lookup(ref);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->server_state != ASX_GEN_SERVER_RUNNING) return ASX_E_INVALID_STATE;

    msg = gs_peek(s);
    if (msg == NULL) return ASX_E_PENDING;

    switch (msg->kind) {
    case ASX_GEN_SERVER_MSG_CALL:
        if (s->callbacks.handle_call != NULL) {
            asx_status st = s->callbacks.handle_call(s->state, msg->payload, &msg->reply);
            msg->replied = 1u;
            s->calls_handled++;
            gs_dequeue(s);
            return st;
        }
        break;
    case ASX_GEN_SERVER_MSG_CAST:
        if (s->callbacks.handle_cast != NULL) {
            asx_status st = s->callbacks.handle_cast(s->state, msg->payload);
            s->casts_handled++;
            gs_dequeue(s);
            return st;
        }
        break;
    case ASX_GEN_SERVER_MSG_STOP:
        if (s->callbacks.terminate != NULL) {
            s->callbacks.terminate(s->state, ASX_OK);
        }
        s->server_state = ASX_GEN_SERVER_STOPPED;
        s->alive = 0u;
        gs_dequeue(s);
        return ASX_OK;
    }

    gs_dequeue(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Extended API                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_gen_server_try_cast(asx_gen_server_ref ref, uint64_t message) {
    gen_server_slot *s;
    asx_gen_server_msg msg;

    s = gs_lookup(ref);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->server_state != ASX_GEN_SERVER_RUNNING) return ASX_E_INVALID_STATE;
    if (s->mb_count >= ASX_GEN_SERVER_MAILBOX_SIZE) return ASX_E_WOULD_BLOCK;

    memset(&msg, 0, sizeof(msg));
    msg.kind = ASX_GEN_SERVER_MSG_CAST;
    msg.payload = message;
    return gs_enqueue(s, &msg);
}

asx_status asx_gen_server_info(asx_gen_server_ref ref, uint64_t message) {
    /* Info messages are handled identically to casts in this implementation */
    return asx_gen_server_cast(ref, message);
}

int asx_gen_server_is_finished(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return 1;
    return s->server_state == ASX_GEN_SERVER_STOPPED;
}

int asx_gen_server_is_closed(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return 1;
    return !s->alive;
}

const char *asx_gen_server_name(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return "";
    return s->name;
}

uint32_t asx_gen_server_calls_handled(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return 0u;
    return s->calls_handled;
}

uint32_t asx_gen_server_casts_handled(asx_gen_server_ref ref) {
    gen_server_slot *s = gs_lookup(ref);
    if (s == NULL) return 0u;
    return s->casts_handled;
}

asx_status asx_gen_server_start_named(asx_gen_server_ref *out, const char *name,
                                        const asx_gen_server_callbacks *callbacks, void *init_args) {
    asx_status st;
    gen_server_slot *s;

    st = asx_gen_server_start(out, callbacks, init_args);
    if (st != ASX_OK) return st;

    s = gs_lookup(*out);
    if (s != NULL && name != NULL) {
        size_t len = strlen(name);
        if (len >= sizeof(s->name)) len = sizeof(s->name) - 1u;
        memcpy(s->name, name, len);
        s->name[len] = '\0';
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_gen_server_reset(void) {
    memset(g_servers, 0, sizeof(g_servers));
    g_next_gen = 1u;
}
