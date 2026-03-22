/*
 * distributed.c — distributed runtime, snapshot, assignment, and recovery
 *
 * Deterministic in-memory distributed systems surface.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/distributed.h>
#include <string.h>

static size_t dist_bounded_strlen(const char *str, size_t max) {
    size_t len;
    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

/* ------------------------------------------------------------------ */
/* Cluster membership                                                  */
/* ------------------------------------------------------------------ */

void asx_dist_cluster_init(asx_dist_cluster *cluster) {
    if (cluster == NULL) return;
    memset(cluster, 0, sizeof(*cluster));
    cluster->next_id = 1u;
}

asx_status asx_dist_cluster_add_node(asx_dist_cluster *cluster, const char *name,
                                     asx_dist_node **out) {
    uint32_t i;
    size_t len;
    asx_dist_node *n;

    if (cluster == NULL || name == NULL) return ASX_E_INVALID_ARGUMENT;
    if (out != NULL) *out = NULL;

    len = dist_bounded_strlen(name, ASX_DIST_NODE_NAME_MAX);
    if (len == 0u || len >= ASX_DIST_NODE_NAME_MAX) return ASX_E_INVALID_ARGUMENT;

    /* Check for duplicate */
    for (i = 0u; i < cluster->count; i++) {
        if (cluster->nodes[i].active && strcmp(cluster->nodes[i].name, name) == 0) {
            return ASX_E_ALREADY_EXISTS;
        }
    }

    /* Find free slot */
    for (i = 0u; i < ASX_DIST_MAX_NODES; i++) {
        if (!cluster->nodes[i].active) break;
    }
    if (i >= ASX_DIST_MAX_NODES) return ASX_E_RESOURCE_EXHAUSTED;

    n = &cluster->nodes[i];
    memset(n, 0, sizeof(*n));
    memcpy(n->name, name, len + 1u);
    n->id = cluster->next_id++;
    n->state = ASX_DIST_NODE_JOINING;
    n->generation = 1u;
    n->active = 1u;
    if (i >= cluster->count) cluster->count = i + 1u;
    if (out != NULL) *out = n;
    return ASX_OK;
}

asx_status asx_dist_cluster_remove_node(asx_dist_cluster *cluster, const char *name) {
    uint32_t i;

    if (cluster == NULL || name == NULL) return ASX_E_INVALID_ARGUMENT;
    for (i = 0u; i < cluster->count; i++) {
        if (cluster->nodes[i].active && strcmp(cluster->nodes[i].name, name) == 0) {
            cluster->nodes[i].state = ASX_DIST_NODE_DOWN;
            cluster->nodes[i].active = 0u;
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

asx_dist_node *asx_dist_cluster_find(asx_dist_cluster *cluster, const char *name) {
    uint32_t i;

    if (cluster == NULL || name == NULL) return NULL;
    for (i = 0u; i < cluster->count; i++) {
        if (cluster->nodes[i].active && strcmp(cluster->nodes[i].name, name) == 0) {
            return &cluster->nodes[i];
        }
    }
    return NULL;
}

asx_status asx_dist_node_set_state(asx_dist_node *node, asx_dist_node_state state) {
    if (node == NULL) return ASX_E_INVALID_ARGUMENT;
    node->state = state;
    return ASX_OK;
}

uint32_t asx_dist_cluster_active_count(const asx_dist_cluster *cluster) {
    uint32_t i, count;

    if (cluster == NULL) return 0u;
    count = 0u;
    for (i = 0u; i < cluster->count; i++) {
        if (cluster->nodes[i].active && cluster->nodes[i].state == ASX_DIST_NODE_ACTIVE) {
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Consistent hash ring                                                */
/* ------------------------------------------------------------------ */

uint32_t asx_dist_hash(const void *data, uint32_t len) {
    /* FNV-1a 32-bit */
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    uint32_t i;

    if (p == NULL) return h;
    for (i = 0u; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

void asx_dist_ring_init(asx_dist_hash_ring *ring) {
    if (ring == NULL) return;
    memset(ring, 0, sizeof(*ring));
}

static void ring_sort(asx_dist_ring_entry *entries, uint32_t count) {
    uint32_t i, j;
    asx_dist_ring_entry tmp;

    /* Simple insertion sort (small array) */
    for (i = 1u; i < count; i++) {
        tmp = entries[i];
        j = i;
        while (j > 0u && entries[j - 1u].hash > tmp.hash) {
            entries[j] = entries[j - 1u];
            j--;
        }
        entries[j] = tmp;
    }
}

asx_status asx_dist_ring_build(asx_dist_hash_ring *ring, const asx_dist_cluster *cluster) {
    uint32_t i, vnode;

    if (ring == NULL || cluster == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(ring, 0, sizeof(*ring));

    for (i = 0u; i < cluster->count; i++) {
        if (!cluster->nodes[i].active || cluster->nodes[i].state != ASX_DIST_NODE_ACTIVE) continue;

        /* Add virtual nodes for better distribution */
        for (vnode = 0u; vnode < 4u && ring->count < ASX_DIST_HASH_RING_SIZE; vnode++) {
            uint8_t key_buf[ASX_DIST_NODE_NAME_MAX + 4];
            uint32_t key_len;
            size_t name_len;

            name_len = dist_bounded_strlen(cluster->nodes[i].name, ASX_DIST_NODE_NAME_MAX);
            memcpy(key_buf, cluster->nodes[i].name, name_len);
            key_buf[name_len] = (uint8_t)vnode;
            key_len = (uint32_t)(name_len + 1u);

            ring->entries[ring->count].hash = asx_dist_hash(key_buf, key_len);
            ring->entries[ring->count].node_id = cluster->nodes[i].id;
            ring->count++;
        }
    }

    ring_sort(ring->entries, ring->count);
    return ASX_OK;
}

uint32_t asx_dist_ring_lookup(const asx_dist_hash_ring *ring, uint32_t key_hash) {
    uint32_t i;

    if (ring == NULL || ring->count == 0u) return 0u;

    /* Binary search for first entry >= key_hash */
    {
        uint32_t lo = 0u;
        uint32_t hi = ring->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (ring->entries[mid].hash < key_hash) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        i = lo;
    }

    /* Wrap around */
    if (i >= ring->count) i = 0u;
    return ring->entries[i].node_id;
}

/* ------------------------------------------------------------------ */
/* Partition assignment                                                */
/* ------------------------------------------------------------------ */

void asx_dist_assignment_init(asx_dist_assignment *assign, uint32_t partition_count) {
    uint32_t i;

    if (assign == NULL) return;
    memset(assign, 0, sizeof(*assign));
    if (partition_count > ASX_DIST_MAX_PARTITIONS) partition_count = ASX_DIST_MAX_PARTITIONS;
    assign->count = partition_count;
    for (i = 0u; i < partition_count; i++) { assign->partitions[i].partition_id = i; }
}

asx_status asx_dist_assignment_rebalance(asx_dist_assignment *assign,
                                         const asx_dist_cluster *cluster) {
    uint32_t active_ids[ASX_DIST_MAX_NODES];
    uint32_t active_count;
    uint32_t i;

    if (assign == NULL || cluster == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Collect active node IDs */
    active_count = 0u;
    for (i = 0u; i < cluster->count; i++) {
        if (cluster->nodes[i].active && cluster->nodes[i].state == ASX_DIST_NODE_ACTIVE) {
            if (active_count < ASX_DIST_MAX_NODES) {
                active_ids[active_count++] = cluster->nodes[i].id;
            }
        }
    }
    if (active_count == 0u) return ASX_E_NOT_FOUND;

    /* Round-robin assignment */
    for (i = 0u; i < assign->count; i++) {
        assign->partitions[i].owner_node_id = active_ids[i % active_count];
        assign->partitions[i].assigned = 1u;
        assign->partitions[i].version++;
    }
    return ASX_OK;
}

uint32_t asx_dist_assignment_owner(const asx_dist_assignment *assign, uint32_t partition_id) {
    if (assign == NULL || partition_id >= assign->count) return 0u;
    if (!assign->partitions[partition_id].assigned) return 0u;
    return assign->partitions[partition_id].owner_node_id;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

void asx_dist_snapshot_store_init(asx_dist_snapshot_store *store) {
    if (store == NULL) return;
    memset(store, 0, sizeof(*store));
    store->next_id = 1u;
}

asx_status asx_dist_snapshot_create(asx_dist_snapshot_store *store, uint32_t node_id,
                                    const void *data, uint32_t data_len, asx_dist_snapshot **out) {
    asx_dist_snapshot *s;

    if (store == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (data_len > ASX_DIST_SNAPSHOT_DATA_MAX) return ASX_E_BUFFER_TOO_SMALL;
    if (store->count >= ASX_DIST_MAX_SNAPSHOTS) return ASX_E_RESOURCE_EXHAUSTED;
    *out = NULL;

    s = &store->snapshots[store->count];
    memset(s, 0, sizeof(*s));
    s->id = store->next_id++;
    s->version = 1u;
    s->node_id = node_id;
    s->data_len = data_len;
    if (data_len > 0u && data != NULL) memcpy(s->data, data, data_len);
    s->valid = 1u;
    store->count++;
    *out = s;
    return ASX_OK;
}

const asx_dist_snapshot *asx_dist_snapshot_latest(const asx_dist_snapshot_store *store,
                                                  uint32_t node_id) {
    const asx_dist_snapshot *latest;
    uint32_t i;

    if (store == NULL) return NULL;
    latest = NULL;
    for (i = 0u; i < store->count; i++) {
        if (store->snapshots[i].valid && store->snapshots[i].node_id == node_id) {
            if (latest == NULL || store->snapshots[i].id > latest->id) {
                latest = &store->snapshots[i];
            }
        }
    }
    return latest;
}

asx_status asx_dist_snapshot_restore(const asx_dist_snapshot *snap, void *out_data,
                                     uint32_t out_capacity, uint32_t *out_len) {
    if (snap == NULL || out_data == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!snap->valid) return ASX_E_INVALID_STATE;
    if (snap->data_len > out_capacity) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(out_data, snap->data, snap->data_len);
    *out_len = snap->data_len;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Recovery protocol                                                   */
/* ------------------------------------------------------------------ */

void asx_dist_recovery_init(asx_dist_recovery *recovery) {
    if (recovery == NULL) return;
    memset(recovery, 0, sizeof(*recovery));
    recovery->state = ASX_DIST_RECOVERY_IDLE;
}

asx_status asx_dist_recovery_start(asx_dist_recovery *recovery, uint32_t failed_node_id) {
    if (recovery == NULL) return ASX_E_INVALID_ARGUMENT;
    if (recovery->state != ASX_DIST_RECOVERY_IDLE) return ASX_E_INVALID_STATE;
    recovery->failed_node_id = failed_node_id;
    recovery->state = ASX_DIST_RECOVERY_DETECTING;
    return ASX_OK;
}

asx_status asx_dist_recovery_step(asx_dist_recovery *recovery, asx_dist_cluster *cluster,
                                  asx_dist_assignment *assign, asx_dist_snapshot_store *snapshots) {
    if (recovery == NULL) return ASX_E_INVALID_ARGUMENT;

    switch (recovery->state) {
    case ASX_DIST_RECOVERY_DETECTING: {
        /* Mark the failed node as down */
        uint32_t i;
        if (cluster != NULL) {
            for (i = 0u; i < cluster->count; i++) {
                if (cluster->nodes[i].active && cluster->nodes[i].id == recovery->failed_node_id) {
                    cluster->nodes[i].state = ASX_DIST_NODE_DOWN;
                    cluster->nodes[i].active = 0u;
                    break;
                }
            }
        }
        recovery->state = ASX_DIST_RECOVERY_RESTORING;
        return ASX_OK;
    }
    case ASX_DIST_RECOVERY_RESTORING: {
        /* Check if we have snapshots to restore */
        if (snapshots != NULL) {
            const asx_dist_snapshot *snap =
                asx_dist_snapshot_latest(snapshots, recovery->failed_node_id);
            if (snap != NULL) recovery->snapshots_restored++;
        }
        recovery->state = ASX_DIST_RECOVERY_REASSIGNING;
        return ASX_OK;
    }
    case ASX_DIST_RECOVERY_REASSIGNING: {
        /* Rebalance partitions */
        if (assign != NULL && cluster != NULL) {
            uint32_t i;
            asx_status st = asx_dist_assignment_rebalance(assign, cluster);
            if (st != ASX_OK) {
                recovery->last_error = st;
                recovery->state = ASX_DIST_RECOVERY_FAILED;
                return st;
            }
            /* Count reassigned partitions */
            for (i = 0u; i < assign->count; i++) {
                if (assign->partitions[i].assigned) recovery->partitions_reassigned++;
            }
        }
        recovery->state = ASX_DIST_RECOVERY_COMPLETE;
        return ASX_OK;
    }
    case ASX_DIST_RECOVERY_COMPLETE:
    case ASX_DIST_RECOVERY_FAILED: return ASX_OK;
    case ASX_DIST_RECOVERY_IDLE: return ASX_E_INVALID_STATE;
    }
    return ASX_E_INVALID_STATE;
}

int asx_dist_recovery_is_complete(const asx_dist_recovery *recovery) {
    if (recovery == NULL) return 0;
    return recovery->state == ASX_DIST_RECOVERY_COMPLETE ||
           recovery->state == ASX_DIST_RECOVERY_FAILED;
}

asx_dist_recovery_state asx_dist_recovery_get_state(const asx_dist_recovery *recovery) {
    if (recovery == NULL) return ASX_DIST_RECOVERY_IDLE;
    return recovery->state;
}
