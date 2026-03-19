/*
 * asx/net/distributed.h — distributed runtime, snapshot, assignment, and recovery
 *
 * Provides the distributed systems surface: consistent hashing for partition
 * assignment, state snapshots with versioning, node membership and failure
 * detection, task assignment/reassignment, and recovery protocol with
 * checkpoint/restore semantics. All deterministic in-memory for core profile.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_DISTRIBUTED_H
#define ASX_NET_DISTRIBUTED_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------
 * Node membership
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DIST_NODE_JOINING  = 0,
    ASX_DIST_NODE_ACTIVE   = 1,
    ASX_DIST_NODE_SUSPECT  = 2,
    ASX_DIST_NODE_LEAVING  = 3,
    ASX_DIST_NODE_DOWN     = 4
} asx_dist_node_state;

typedef struct {
    char name[ASX_DIST_NODE_NAME_MAX];
    uint32_t id;
    asx_dist_node_state state;
    uint64_t last_heartbeat_ns;
    uint32_t generation;
    uint8_t active;
} asx_dist_node;

typedef struct {
    asx_dist_node nodes[ASX_DIST_MAX_NODES];
    uint32_t count;
    uint32_t next_id;
} asx_dist_cluster;

/* Initialize a cluster. */
ASX_API void asx_dist_cluster_init(asx_dist_cluster *cluster);

/* Add a node. */
ASX_API asx_status asx_dist_cluster_add_node(asx_dist_cluster *cluster, const char *name,
                                               asx_dist_node **out);

/* Remove a node by name. */
ASX_API asx_status asx_dist_cluster_remove_node(asx_dist_cluster *cluster, const char *name);

/* Find a node by name. Returns NULL if not found. */
ASX_API asx_dist_node *asx_dist_cluster_find(asx_dist_cluster *cluster, const char *name);

/* Update node state. */
ASX_API asx_status asx_dist_node_set_state(asx_dist_node *node, asx_dist_node_state state);

/* Get count of active nodes. */
ASX_API uint32_t asx_dist_cluster_active_count(const asx_dist_cluster *cluster);

/* -------------------------------------------------------------------
 * Consistent hash ring
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t hash;
    uint32_t node_id;
} asx_dist_ring_entry;

typedef struct {
    asx_dist_ring_entry entries[ASX_DIST_HASH_RING_SIZE];
    uint32_t count;
} asx_dist_hash_ring;

/* Initialize hash ring. */
ASX_API void asx_dist_ring_init(asx_dist_hash_ring *ring);

/* Build ring from cluster's active nodes. */
ASX_API asx_status asx_dist_ring_build(asx_dist_hash_ring *ring,
                                        const asx_dist_cluster *cluster);

/* Look up the node responsible for a key. Returns node_id. */
ASX_API uint32_t asx_dist_ring_lookup(const asx_dist_hash_ring *ring, uint32_t key_hash);

/* Deterministic hash function for keys. */
ASX_API uint32_t asx_dist_hash(const void *data, uint32_t len);

/* -------------------------------------------------------------------
 * Partition assignment
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t partition_id;
    uint32_t owner_node_id;
    uint32_t version;
    uint8_t assigned;
} asx_dist_partition;

typedef struct {
    asx_dist_partition partitions[ASX_DIST_MAX_PARTITIONS];
    uint32_t count;
} asx_dist_assignment;

/* Initialize partition assignment. */
ASX_API void asx_dist_assignment_init(asx_dist_assignment *assign, uint32_t partition_count);

/* Rebalance partitions across active nodes in a cluster. */
ASX_API asx_status asx_dist_assignment_rebalance(asx_dist_assignment *assign,
                                                   const asx_dist_cluster *cluster);

/* Get the owner node for a partition. Returns 0 if unassigned. */
ASX_API uint32_t asx_dist_assignment_owner(const asx_dist_assignment *assign, uint32_t partition_id);

/* -------------------------------------------------------------------
 * Snapshot
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint32_t version;
    uint32_t node_id;
    uint8_t data[ASX_DIST_SNAPSHOT_DATA_MAX];
    uint32_t data_len;
    uint64_t created_at_ns;
    uint8_t valid;
} asx_dist_snapshot;

typedef struct {
    asx_dist_snapshot snapshots[ASX_DIST_MAX_SNAPSHOTS];
    uint32_t count;
    uint32_t next_id;
} asx_dist_snapshot_store;

/* Initialize snapshot store. */
ASX_API void asx_dist_snapshot_store_init(asx_dist_snapshot_store *store);

/* Create a snapshot. */
ASX_API asx_status asx_dist_snapshot_create(asx_dist_snapshot_store *store, uint32_t node_id,
                                              const void *data, uint32_t data_len,
                                              asx_dist_snapshot **out);

/* Find the latest snapshot for a node. Returns NULL if none. */
ASX_API const asx_dist_snapshot *asx_dist_snapshot_latest(const asx_dist_snapshot_store *store,
                                                            uint32_t node_id);

/* Restore from a snapshot (copy data out). */
ASX_API asx_status asx_dist_snapshot_restore(const asx_dist_snapshot *snap, void *out_data,
                                               uint32_t out_capacity, uint32_t *out_len);

/* -------------------------------------------------------------------
 * Recovery protocol
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DIST_RECOVERY_IDLE       = 0,
    ASX_DIST_RECOVERY_DETECTING  = 1,
    ASX_DIST_RECOVERY_RESTORING  = 2,
    ASX_DIST_RECOVERY_REASSIGNING = 3,
    ASX_DIST_RECOVERY_COMPLETE   = 4,
    ASX_DIST_RECOVERY_FAILED     = 5
} asx_dist_recovery_state;

typedef struct {
    asx_dist_recovery_state state;
    uint32_t failed_node_id;
    uint32_t replacement_node_id;
    uint32_t partitions_reassigned;
    uint32_t snapshots_restored;
    asx_status last_error;
} asx_dist_recovery;

/* Initialize recovery state. */
ASX_API void asx_dist_recovery_init(asx_dist_recovery *recovery);

/* Start recovery for a failed node. */
ASX_API asx_status asx_dist_recovery_start(asx_dist_recovery *recovery, uint32_t failed_node_id);

/* Execute one recovery step (deterministic state machine). */
ASX_API asx_status asx_dist_recovery_step(asx_dist_recovery *recovery,
                                            asx_dist_cluster *cluster,
                                            asx_dist_assignment *assign,
                                            asx_dist_snapshot_store *snapshots);

/* Check if recovery is complete. */
ASX_API int asx_dist_recovery_is_complete(const asx_dist_recovery *recovery);

/* Get recovery state. */
ASX_API asx_dist_recovery_state asx_dist_recovery_get_state(const asx_dist_recovery *recovery);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_DISTRIBUTED_H */
