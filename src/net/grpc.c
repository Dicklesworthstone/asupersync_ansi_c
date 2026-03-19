/*
 * grpc.c — gRPC codec, service, client/server, reflection, and health
 *
 * Deterministic in-memory gRPC surface. Services are registered with
 * method handlers and dispatched by full method name. Client channels
 * link directly to servers for deterministic testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/grpc.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounded string helpers                                              */
/* ------------------------------------------------------------------ */

static size_t grpc_bounded_strlen(const char *str, size_t max) {
    size_t len;

    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

/* ------------------------------------------------------------------ */
/* gRPC status codes                                                   */
/* ------------------------------------------------------------------ */

static const char *const g_grpc_code_names[] = {
    "OK",                  "CANCELLED",           "UNKNOWN",
    "INVALID_ARGUMENT",    "DEADLINE_EXCEEDED",    "NOT_FOUND",
    "ALREADY_EXISTS",      "PERMISSION_DENIED",    "RESOURCE_EXHAUSTED",
    "FAILED_PRECONDITION", "ABORTED",              "OUT_OF_RANGE",
    "UNIMPLEMENTED",       "INTERNAL",             "UNAVAILABLE",
    "DATA_LOSS",           "UNAUTHENTICATED"
};

const char *asx_grpc_code_str(asx_grpc_code code) {
    if ((unsigned)code > 16u) return "UNKNOWN";
    return g_grpc_code_names[(unsigned)code];
}

/* ------------------------------------------------------------------ */
/* gRPC message                                                        */
/* ------------------------------------------------------------------ */

void asx_grpc_message_init(asx_grpc_message *msg) {
    if (msg == NULL) return;
    memset(msg, 0, sizeof(*msg));
}

asx_status asx_grpc_message_set(asx_grpc_message *msg, const void *data, uint32_t len) {
    if (msg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_GRPC_MSG_MAX) return ASX_E_BUFFER_TOO_SMALL;
    msg->len = len;
    if (len > 0u && data != NULL) memcpy(msg->data, data, len);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* gRPC metadata                                                       */
/* ------------------------------------------------------------------ */

void asx_grpc_metadata_init(asx_grpc_metadata *md) {
    if (md == NULL) return;
    memset(md, 0, sizeof(*md));
}

asx_status asx_grpc_metadata_add(asx_grpc_metadata *md, const char *key, const char *value) {
    size_t klen, vlen;
    asx_grpc_metadata_entry *e;

    if (md == NULL || key == NULL || value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (md->count >= ASX_GRPC_MAX_METADATA) return ASX_E_RESOURCE_EXHAUSTED;
    klen = grpc_bounded_strlen(key, ASX_GRPC_METADATA_KEY_MAX);
    vlen = grpc_bounded_strlen(value, ASX_GRPC_METADATA_VALUE_MAX);
    if (klen == 0u || klen >= ASX_GRPC_METADATA_KEY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (vlen >= ASX_GRPC_METADATA_VALUE_MAX) return ASX_E_BUFFER_TOO_SMALL;

    e = &md->entries[md->count];
    memcpy(e->key, key, klen + 1u);
    memcpy(e->value, value, vlen + 1u);
    md->count++;
    return ASX_OK;
}

const char *asx_grpc_metadata_get(const asx_grpc_metadata *md, const char *key) {
    uint32_t i;

    if (md == NULL || key == NULL) return NULL;
    for (i = 0u; i < md->count; i++) {
        if (strcmp(md->entries[i].key, key) == 0) return md->entries[i].value;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* gRPC service                                                        */
/* ------------------------------------------------------------------ */

void asx_grpc_service_init(asx_grpc_service *svc, const char *name) {
    size_t len;

    if (svc == NULL) return;
    memset(svc, 0, sizeof(*svc));
    if (name != NULL) {
        len = grpc_bounded_strlen(name, ASX_GRPC_SERVICE_NAME_MAX);
        if (len < ASX_GRPC_SERVICE_NAME_MAX) memcpy(svc->name, name, len + 1u);
    }
}

asx_status asx_grpc_service_add_unary(asx_grpc_service *svc, const char *method_name,
                                       asx_grpc_unary_handler_fn handler, void *user_data) {
    size_t slen, mlen;
    asx_grpc_method *m;
    int written;

    if (svc == NULL || method_name == NULL || handler == NULL) return ASX_E_INVALID_ARGUMENT;
    if (svc->method_count >= ASX_GRPC_MAX_METHODS) return ASX_E_RESOURCE_EXHAUSTED;

    slen = grpc_bounded_strlen(svc->name, ASX_GRPC_SERVICE_NAME_MAX);
    mlen = grpc_bounded_strlen(method_name, ASX_GRPC_METHOD_NAME_MAX);
    if (mlen == 0u) return ASX_E_INVALID_ARGUMENT;

    m = &svc->methods[svc->method_count];
    memset(m, 0, sizeof(*m));

    /* Build full name: "/ServiceName/MethodName" */
    written = snprintf(m->full_name, ASX_GRPC_METHOD_NAME_MAX, "/%.*s/%.*s", (int)slen, svc->name,
                       (int)mlen, method_name);
    if (written < 0 || (uint32_t)written >= ASX_GRPC_METHOD_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;

    m->method_type = ASX_GRPC_METHOD_UNARY;
    m->handler = handler;
    m->user_data = user_data;
    m->active = 1u;
    svc->method_count++;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* gRPC server                                                         */
/* ------------------------------------------------------------------ */

void asx_grpc_server_init(asx_grpc_server *srv) {
    if (srv == NULL) return;
    memset(srv, 0, sizeof(*srv));
}

asx_status asx_grpc_server_add_service(asx_grpc_server *srv, const asx_grpc_service *svc) {
    if (srv == NULL || svc == NULL) return ASX_E_INVALID_ARGUMENT;
    if (srv->service_count >= ASX_GRPC_MAX_SERVICES) return ASX_E_RESOURCE_EXHAUSTED;
    srv->services[srv->service_count] = *svc;
    srv->service_count++;
    return ASX_OK;
}

asx_status asx_grpc_server_call(asx_grpc_server *srv, const char *method_name,
                                 const asx_grpc_message *req, asx_grpc_message *resp,
                                 const asx_grpc_metadata *md, asx_grpc_code *out_code) {
    uint32_t si, mi;

    if (srv == NULL || method_name == NULL || req == NULL || resp == NULL || out_code == NULL)
        return ASX_E_INVALID_ARGUMENT;

    for (si = 0u; si < srv->service_count; si++) {
        for (mi = 0u; mi < srv->services[si].method_count; mi++) {
            asx_grpc_method *m = &srv->services[si].methods[mi];
            if (m->active && strcmp(m->full_name, method_name) == 0) {
                asx_status st;
                srv->calls_handled++;
                asx_grpc_message_init(resp);
                *out_code = ASX_GRPC_OK;
                st = m->handler(req, resp, md, out_code, m->user_data);
                return st;
            }
        }
    }

    *out_code = ASX_GRPC_UNIMPLEMENTED;
    return ASX_OK;
}

uint32_t asx_grpc_server_calls_handled(const asx_grpc_server *srv) {
    if (srv == NULL) return 0u;
    return srv->calls_handled;
}

/* ------------------------------------------------------------------ */
/* gRPC client channel                                                 */
/* ------------------------------------------------------------------ */

void asx_grpc_channel_init(asx_grpc_channel *ch, const char *target, asx_grpc_server *server) {
    size_t len;

    if (ch == NULL) return;
    memset(ch, 0, sizeof(*ch));
    ch->server = server;
    if (target != NULL) {
        len = grpc_bounded_strlen(target, ASX_GRPC_SERVICE_NAME_MAX);
        if (len < ASX_GRPC_SERVICE_NAME_MAX) memcpy(ch->target, target, len + 1u);
    }
}

asx_status asx_grpc_channel_call(asx_grpc_channel *ch, const char *method_name,
                                  const asx_grpc_message *req, asx_grpc_message *resp,
                                  const asx_grpc_metadata *md, asx_grpc_code *out_code) {
    if (ch == NULL || ch->server == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_grpc_server_call(ch->server, method_name, req, resp, md, out_code);
}

/* ------------------------------------------------------------------ */
/* gRPC health check                                                   */
/* ------------------------------------------------------------------ */

void asx_grpc_health_init(asx_grpc_health *health) {
    if (health == NULL) return;
    memset(health, 0, sizeof(*health));
}

asx_status asx_grpc_health_set(asx_grpc_health *health, const char *service,
                                asx_grpc_health_status status) {
    uint32_t i;
    size_t len;
    const char *svc_name;

    if (health == NULL) return ASX_E_INVALID_ARGUMENT;
    svc_name = service != NULL ? service : "";

    /* Update existing entry */
    for (i = 0u; i < health->count; i++) {
        if (health->entries[i].active && strcmp(health->entries[i].service, svc_name) == 0) {
            health->entries[i].status = status;
            return ASX_OK;
        }
    }

    /* Add new entry */
    if (health->count >= ASX_GRPC_MAX_HEALTH_ENTRIES) return ASX_E_RESOURCE_EXHAUSTED;
    len = grpc_bounded_strlen(svc_name, ASX_GRPC_SERVICE_NAME_MAX);
    if (len >= ASX_GRPC_SERVICE_NAME_MAX) return ASX_E_BUFFER_TOO_SMALL;

    health->entries[health->count].active = 1u;
    memcpy(health->entries[health->count].service, svc_name, len + 1u);
    health->entries[health->count].status = status;
    health->count++;
    return ASX_OK;
}

asx_grpc_health_status asx_grpc_health_check(const asx_grpc_health *health, const char *service) {
    uint32_t i;
    const char *svc_name;

    if (health == NULL) return ASX_GRPC_HEALTH_UNKNOWN;
    svc_name = service != NULL ? service : "";

    for (i = 0u; i < health->count; i++) {
        if (health->entries[i].active && strcmp(health->entries[i].service, svc_name) == 0) {
            return health->entries[i].status;
        }
    }
    return ASX_GRPC_HEALTH_SERVICE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* gRPC reflection                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_grpc_reflect(const asx_grpc_server *srv, asx_grpc_reflection *out) {
    uint32_t i;
    size_t len;

    if (srv == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    for (i = 0u; i < srv->service_count && out->count < ASX_GRPC_MAX_SERVICES; i++) {
        len = grpc_bounded_strlen(srv->services[i].name, ASX_GRPC_SERVICE_NAME_MAX);
        if (len < ASX_GRPC_SERVICE_NAME_MAX) {
            memcpy(out->services[out->count], srv->services[i].name, len + 1u);
            out->count++;
        }
    }
    return ASX_OK;
}

int asx_grpc_reflect_has_service(const asx_grpc_reflection *refl, const char *name) {
    uint32_t i;

    if (refl == NULL || name == NULL) return 0;
    for (i = 0u; i < refl->count; i++) {
        if (strcmp(refl->services[i], name) == 0) return 1;
    }
    return 0;
}
