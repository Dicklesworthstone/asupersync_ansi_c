/*
 * asx/net/grpc.h — gRPC codec, service, client/server, reflection, and health
 *
 * Provides the public gRPC surface: protobuf-style codec abstraction, service
 * definition with method dispatch, unary and streaming call types, client channel
 * and server listener, reflection registry, and health checking protocol.
 * All deterministic in-memory for the core profile.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_GRPC_H
#define ASX_NET_GRPC_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_GRPC_SURFACE
/* -------------------------------------------------------------------
 * gRPC status codes (mirrors grpc-status)
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_GRPC_OK = 0,
    ASX_GRPC_CANCELLED = 1,
    ASX_GRPC_UNKNOWN = 2,
    ASX_GRPC_INVALID_ARGUMENT = 3,
    ASX_GRPC_DEADLINE_EXCEEDED = 4,
    ASX_GRPC_NOT_FOUND = 5,
    ASX_GRPC_ALREADY_EXISTS = 6,
    ASX_GRPC_PERMISSION_DENIED = 7,
    ASX_GRPC_RESOURCE_EXHAUSTED = 8,
    ASX_GRPC_FAILED_PRECONDITION = 9,
    ASX_GRPC_ABORTED = 10,
    ASX_GRPC_OUT_OF_RANGE = 11,
    ASX_GRPC_UNIMPLEMENTED = 12,
    ASX_GRPC_INTERNAL = 13,
    ASX_GRPC_UNAVAILABLE = 14,
    ASX_GRPC_DATA_LOSS = 15,
    ASX_GRPC_UNAUTHENTICATED = 16
} asx_grpc_code;

/* Return the name of a gRPC status code. */
ASX_API const char *asx_grpc_code_str(asx_grpc_code code);

/* -------------------------------------------------------------------
 * gRPC codec (protobuf-style encode/decode abstraction)
 * ------------------------------------------------------------------- */

#ifndef ASX_GRPC_MSG_MAX
#define ASX_GRPC_MSG_MAX 4096u
#endif

typedef struct {
    uint8_t data[ASX_GRPC_MSG_MAX];
    uint32_t len;
} asx_grpc_message;

/* Initialize an empty message. */
ASX_API void asx_grpc_message_init(asx_grpc_message *msg);

/* Set message payload. */
ASX_API asx_status asx_grpc_message_set(asx_grpc_message *msg, const void *data, uint32_t len);

/* -------------------------------------------------------------------
 * gRPC metadata (key-value pairs on calls)
 * ------------------------------------------------------------------- */

#ifndef ASX_GRPC_MAX_METADATA
#define ASX_GRPC_MAX_METADATA 8u
#endif

#ifndef ASX_GRPC_METADATA_KEY_MAX
#define ASX_GRPC_METADATA_KEY_MAX 64u
#endif

#ifndef ASX_GRPC_METADATA_VALUE_MAX
#define ASX_GRPC_METADATA_VALUE_MAX 128u
#endif

typedef struct {
    char key[ASX_GRPC_METADATA_KEY_MAX];
    char value[ASX_GRPC_METADATA_VALUE_MAX];
} asx_grpc_metadata_entry;

typedef struct {
    asx_grpc_metadata_entry entries[ASX_GRPC_MAX_METADATA];
    uint32_t count;
} asx_grpc_metadata;

/* Initialize metadata. */
ASX_API void asx_grpc_metadata_init(asx_grpc_metadata *md);

/* Add a metadata entry. */
ASX_API asx_status asx_grpc_metadata_add(asx_grpc_metadata *md, const char *key, const char *value);

/* Get metadata by key. Returns NULL if not found. */
ASX_API const char *asx_grpc_metadata_get(const asx_grpc_metadata *md, const char *key);

/* -------------------------------------------------------------------
 * gRPC method and service definition
 * ------------------------------------------------------------------- */

#ifndef ASX_GRPC_MAX_METHODS
#define ASX_GRPC_MAX_METHODS 16u
#endif

#ifndef ASX_GRPC_METHOD_NAME_MAX
#define ASX_GRPC_METHOD_NAME_MAX 128u
#endif

#ifndef ASX_GRPC_SERVICE_NAME_MAX
#define ASX_GRPC_SERVICE_NAME_MAX 128u
#endif

typedef enum {
    ASX_GRPC_METHOD_UNARY = 0,
    ASX_GRPC_METHOD_SERVER_STREAM = 1,
    ASX_GRPC_METHOD_CLIENT_STREAM = 2,
    ASX_GRPC_METHOD_BIDI_STREAM = 3
} asx_grpc_method_type;

/* Unary handler: receives request message, produces response message. */
typedef asx_status (*asx_grpc_unary_handler_fn)(const asx_grpc_message *req, asx_grpc_message *resp,
                                                const asx_grpc_metadata *md,
                                                asx_grpc_code *out_code, void *user_data);

typedef struct {
    char full_name[ASX_GRPC_METHOD_NAME_MAX]; /* "/pkg.Service/Method" */
    asx_grpc_method_type method_type;
    asx_grpc_unary_handler_fn handler;
    void *user_data;
    uint8_t active;
} asx_grpc_method;

typedef struct {
    char name[ASX_GRPC_SERVICE_NAME_MAX];
    asx_grpc_method methods[ASX_GRPC_MAX_METHODS];
    uint32_t method_count;
} asx_grpc_service;

/* Initialize a service. */
ASX_API void asx_grpc_service_init(asx_grpc_service *svc, const char *name);

/* Add a unary method to a service. */
ASX_API asx_status asx_grpc_service_add_unary(asx_grpc_service *svc, const char *method_name,
                                              asx_grpc_unary_handler_fn handler, void *user_data);

/* -------------------------------------------------------------------
 * gRPC server
 * ------------------------------------------------------------------- */

#ifndef ASX_GRPC_MAX_SERVICES
#define ASX_GRPC_MAX_SERVICES 8u
#endif

typedef struct {
    asx_grpc_service services[ASX_GRPC_MAX_SERVICES];
    uint32_t service_count;
    uint32_t calls_handled;
} asx_grpc_server;

/* Initialize a gRPC server. */
ASX_API void asx_grpc_server_init(asx_grpc_server *srv);

/* Register a service with the server. */
ASX_API asx_status asx_grpc_server_add_service(asx_grpc_server *srv, const asx_grpc_service *svc);

/* Dispatch a unary call by full method name. */
ASX_API asx_status asx_grpc_server_call(asx_grpc_server *srv, const char *method_name,
                                        const asx_grpc_message *req, asx_grpc_message *resp,
                                        const asx_grpc_metadata *md, asx_grpc_code *out_code);

/* Get the number of calls handled. */
ASX_API uint32_t asx_grpc_server_calls_handled(const asx_grpc_server *srv);

/* -------------------------------------------------------------------
 * gRPC client channel
 * ------------------------------------------------------------------- */

typedef struct {
    char target[ASX_GRPC_SERVICE_NAME_MAX];
    asx_grpc_server *server; /* Direct in-memory link for deterministic mode */
} asx_grpc_channel;

/* Initialize a client channel connected to a server (in-memory). */
ASX_API void asx_grpc_channel_init(asx_grpc_channel *ch, const char *target,
                                   asx_grpc_server *server);

/* Make a unary call through the channel. */
ASX_API asx_status asx_grpc_channel_call(asx_grpc_channel *ch, const char *method_name,
                                         const asx_grpc_message *req, asx_grpc_message *resp,
                                         const asx_grpc_metadata *md, asx_grpc_code *out_code);

/* -------------------------------------------------------------------
 * gRPC health check (grpc.health.v1)
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_GRPC_HEALTH_UNKNOWN = 0,
    ASX_GRPC_HEALTH_SERVING = 1,
    ASX_GRPC_HEALTH_NOT_SERVING = 2,
    ASX_GRPC_HEALTH_SERVICE_UNKNOWN = 3
} asx_grpc_health_status;

#ifndef ASX_GRPC_MAX_HEALTH_ENTRIES
#define ASX_GRPC_MAX_HEALTH_ENTRIES 8u
#endif

typedef struct {
    char service[ASX_GRPC_SERVICE_NAME_MAX];
    asx_grpc_health_status status;
    uint8_t active;
} asx_grpc_health_entry;

typedef struct {
    asx_grpc_health_entry entries[ASX_GRPC_MAX_HEALTH_ENTRIES];
    uint32_t count;
} asx_grpc_health;

/* Initialize health checker. */
ASX_API void asx_grpc_health_init(asx_grpc_health *health);

/* Set the health status for a service. Empty name = overall server health. */
ASX_API asx_status asx_grpc_health_set(asx_grpc_health *health, const char *service,
                                       asx_grpc_health_status status);

/* Check the health status of a service. */
ASX_API asx_grpc_health_status asx_grpc_health_check(const asx_grpc_health *health,
                                                     const char *service);

/* -------------------------------------------------------------------
 * gRPC reflection (service listing)
 * ------------------------------------------------------------------- */

typedef struct {
    char services[ASX_GRPC_MAX_SERVICES][ASX_GRPC_SERVICE_NAME_MAX];
    uint32_t count;
} asx_grpc_reflection;

/* Build a reflection listing from a server. */
ASX_API asx_status asx_grpc_reflect(const asx_grpc_server *srv, asx_grpc_reflection *out);

/* Check if a service is listed in the reflection. */
ASX_API int asx_grpc_reflect_has_service(const asx_grpc_reflection *refl, const char *name);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_GRPC_H */
