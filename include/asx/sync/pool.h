/*
 * asx/sync/pool.h — generic resource pool with health checks and stats
 *
 * Manages a fixed-size pool of opaque resources (void*). Resources are
 * created via a factory function, health-checked before reuse, and
 * automatically returned on release. Cancel-safe: if acquisition is
 * cancelled, no resource is leaked.
 *
 * Zero dynamic allocation — uses fixed-size slot arenas.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_POOL_H
#define ASX_SYNC_POOL_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_POOL_MAX
#define ASX_POOL_MAX 8u
#endif

#ifndef ASX_POOL_MAX_RESOURCES
#define ASX_POOL_MAX_RESOURCES 16u
#endif

#ifndef ASX_POOL_MAX_WAITERS
#define ASX_POOL_MAX_WAITERS 8u
#endif

/* -------------------------------------------------------------------
 * Factory and health check function types
 * ------------------------------------------------------------------- */

/* Create a new resource. Returns ASX_OK and sets *out on success. */
typedef asx_status (*asx_pool_create_fn)(void *factory_data, void **out);

/* Destroy a resource (cleanup). */
typedef void (*asx_pool_destroy_fn)(void *resource, void *factory_data);

/* Health check: returns nonzero if resource is still usable. */
typedef int (*asx_pool_health_fn)(void *resource, void *factory_data);

/* -------------------------------------------------------------------
 * Pool configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_size;               /* maximum number of resources */
    uint32_t min_idle;               /* minimum idle resources to maintain */
    asx_pool_create_fn create_fn;    /* factory: create new resource */
    asx_pool_destroy_fn destroy_fn;  /* factory: destroy resource (may be NULL) */
    asx_pool_health_fn health_fn;    /* health check (may be NULL = always healthy) */
    void *factory_data;              /* user data passed to factory functions */
} asx_pool_config;

/* -------------------------------------------------------------------
 * Pool statistics
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t active;             /* resources currently checked out */
    uint32_t idle;               /* resources available in pool */
    uint32_t total;              /* active + idle */
    uint32_t max_size;           /* maximum pool size */
    uint32_t waiters;            /* threads/tasks waiting for a resource */
    uint64_t total_acquisitions; /* lifetime acquisition count */
    uint64_t total_creates;      /* lifetime resource creation count */
    uint64_t health_failures;    /* resources discarded by health check */
} asx_pool_stats;

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_pool_handle;

/* A checked-out resource. Must be returned via asx_pool_return(). */
typedef struct {
    uint32_t pool_slot;
    uint32_t resource_slot;
    uint16_t generation;
    void *resource; /* the actual resource pointer */
} asx_pooled_resource;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a pool with the given configuration. */
ASX_API ASX_MUST_USE asx_status asx_pool_create(const asx_pool_config *config,
                                                  asx_pool_handle *out);

/* Close a pool. All idle resources are destroyed.
 * Active resources will be destroyed when returned. */
ASX_API asx_status asx_pool_close(asx_pool_handle handle);

/* -------------------------------------------------------------------
 * Acquire / Return
 * ------------------------------------------------------------------- */

/* Try to acquire a resource immediately.
 * Returns ASX_OK and fills *out on success.
 * Returns ASX_E_WOULD_BLOCK if no resource available.
 * Returns ASX_E_DISCONNECTED if pool is closed.
 * Health-checks idle resources before returning them. */
ASX_API asx_status asx_pool_try_acquire(asx_pool_handle handle, asx_pooled_resource *out);

/* Return a resource to the pool. If the pool is closed, the resource
 * is destroyed. If health check fails on return, it's destroyed. */
ASX_API asx_status asx_pool_return(asx_pooled_resource *pr);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get current pool statistics. */
ASX_API asx_status asx_pool_get_stats(asx_pool_handle handle, asx_pool_stats *out);

/* Check if the pool is closed. */
ASX_API int asx_pool_is_closed(asx_pool_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

ASX_API void asx_pool_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_POOL_H */
