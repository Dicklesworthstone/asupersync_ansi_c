/*
 * test_distributed.c — unit tests for distributed runtime surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/distributed.h>
#include <string.h>

/* Cluster tests */

TEST(cluster_add_and_find) {
    asx_dist_cluster cluster;
    asx_dist_node *n;

    asx_dist_cluster_init(&cluster);
    ASSERT_EQ(asx_dist_cluster_add_node(&cluster, "node-a", &n), ASX_OK);
    ASSERT_TRUE(n != NULL);
    ASSERT_STR_EQ(n->name, "node-a");
    ASSERT_EQ(n->state, ASX_DIST_NODE_JOINING);

    n = asx_dist_cluster_find(&cluster, "node-a");
    ASSERT_TRUE(n != NULL);
    ASSERT_TRUE(asx_dist_cluster_find(&cluster, "missing") == NULL);
}

TEST(cluster_remove) {
    asx_dist_cluster cluster;
    asx_dist_node *n;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "node-b", &n);
    ASSERT_EQ(asx_dist_cluster_remove_node(&cluster, "node-b"), ASX_OK);
    ASSERT_TRUE(asx_dist_cluster_find(&cluster, "node-b") == NULL);
    ASSERT_EQ(asx_dist_cluster_remove_node(&cluster, "node-b"), ASX_E_NOT_FOUND);
}

TEST(cluster_duplicate_rejected) {
    asx_dist_cluster cluster;
    asx_dist_node *n;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "dup", &n);
    ASSERT_EQ(asx_dist_cluster_add_node(&cluster, "dup", &n), ASX_E_ALREADY_EXISTS);
}

TEST(cluster_active_count) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "x", &n1);
    asx_dist_cluster_add_node(&cluster, "y", &n2);
    ASSERT_EQ(asx_dist_cluster_active_count(&cluster), 0u); /* joining, not active */

    asx_dist_node_set_state(n1, ASX_DIST_NODE_ACTIVE);
    asx_dist_node_set_state(n2, ASX_DIST_NODE_ACTIVE);
    ASSERT_EQ(asx_dist_cluster_active_count(&cluster), 2u);
}

/* Hash ring tests */

TEST(hash_deterministic) {
    uint32_t h1 = asx_dist_hash("key", 3);
    uint32_t h2 = asx_dist_hash("key", 3);
    ASSERT_EQ(h1, h2);
}

TEST(hash_different_keys_differ) {
    uint32_t h1 = asx_dist_hash("aaa", 3);
    uint32_t h2 = asx_dist_hash("bbb", 3);
    ASSERT_NE(h1, h2);
}

TEST(ring_build_and_lookup) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;
    asx_dist_hash_ring ring;
    uint32_t owner;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "alpha", &n1);
    asx_dist_cluster_add_node(&cluster, "beta", &n2);
    asx_dist_node_set_state(n1, ASX_DIST_NODE_ACTIVE);
    asx_dist_node_set_state(n2, ASX_DIST_NODE_ACTIVE);

    asx_dist_ring_init(&ring);
    ASSERT_EQ(asx_dist_ring_build(&ring, &cluster), ASX_OK);
    ASSERT_TRUE(ring.count > 0u);

    owner = asx_dist_ring_lookup(&ring, asx_dist_hash("my-key", 6));
    ASSERT_TRUE(owner == n1->id || owner == n2->id);
}

/* Assignment tests */

TEST(assignment_rebalance) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;
    asx_dist_assignment assign;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "a", &n1);
    asx_dist_cluster_add_node(&cluster, "b", &n2);
    asx_dist_node_set_state(n1, ASX_DIST_NODE_ACTIVE);
    asx_dist_node_set_state(n2, ASX_DIST_NODE_ACTIVE);

    asx_dist_assignment_init(&assign, 4);
    ASSERT_EQ(asx_dist_assignment_rebalance(&assign, &cluster), ASX_OK);

    /* All partitions should be assigned */
    ASSERT_TRUE(asx_dist_assignment_owner(&assign, 0) != 0u);
    ASSERT_TRUE(asx_dist_assignment_owner(&assign, 1) != 0u);
    ASSERT_TRUE(asx_dist_assignment_owner(&assign, 2) != 0u);
    ASSERT_TRUE(asx_dist_assignment_owner(&assign, 3) != 0u);
}

TEST(assignment_no_active_nodes) {
    asx_dist_cluster cluster;
    asx_dist_assignment assign;

    asx_dist_cluster_init(&cluster);
    asx_dist_assignment_init(&assign, 4);
    ASSERT_EQ(asx_dist_assignment_rebalance(&assign, &cluster), ASX_E_NOT_FOUND);
}

/* Snapshot tests */

TEST(snapshot_create_and_restore) {
    asx_dist_snapshot_store store;
    asx_dist_snapshot *snap;
    uint8_t restored[64];
    uint32_t restored_len;

    asx_dist_snapshot_store_init(&store);
    ASSERT_EQ(asx_dist_snapshot_create(&store, 1, "state-data", 10, &snap), ASX_OK);
    ASSERT_TRUE(snap != NULL);
    ASSERT_TRUE(snap->valid);
    ASSERT_EQ(snap->node_id, 1u);

    ASSERT_EQ(asx_dist_snapshot_restore(snap, restored, sizeof(restored), &restored_len), ASX_OK);
    ASSERT_EQ(restored_len, 10u);
    ASSERT_TRUE(memcmp(restored, "state-data", 10) == 0);
}

TEST(snapshot_latest) {
    asx_dist_snapshot_store store;
    asx_dist_snapshot *s1, *s2;
    const asx_dist_snapshot *latest;

    asx_dist_snapshot_store_init(&store);
    asx_dist_snapshot_create(&store, 1, "old", 3, &s1);
    asx_dist_snapshot_create(&store, 1, "new", 3, &s2);

    latest = asx_dist_snapshot_latest(&store, 1);
    ASSERT_TRUE(latest != NULL);
    ASSERT_EQ(latest->id, s2->id);
}

TEST(snapshot_missing_node) {
    asx_dist_snapshot_store store;
    asx_dist_snapshot_store_init(&store);
    ASSERT_TRUE(asx_dist_snapshot_latest(&store, 99) == NULL);
}

/* Recovery tests */

TEST(recovery_full_cycle) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;
    asx_dist_assignment assign;
    asx_dist_snapshot_store snapshots;
    asx_dist_snapshot *snap;
    asx_dist_recovery recovery;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "primary", &n1);
    asx_dist_cluster_add_node(&cluster, "backup", &n2);
    asx_dist_node_set_state(n1, ASX_DIST_NODE_ACTIVE);
    asx_dist_node_set_state(n2, ASX_DIST_NODE_ACTIVE);

    asx_dist_assignment_init(&assign, 4);
    asx_dist_assignment_rebalance(&assign, &cluster);

    asx_dist_snapshot_store_init(&snapshots);
    asx_dist_snapshot_create(&snapshots, n1->id, "checkpoint", 10, &snap);

    /* Simulate failure of primary */
    asx_dist_recovery_init(&recovery);
    ASSERT_EQ(asx_dist_recovery_start(&recovery, n1->id), ASX_OK);
    ASSERT_EQ(asx_dist_recovery_get_state(&recovery), ASX_DIST_RECOVERY_DETECTING);

    /* Step through recovery */
    ASSERT_EQ(asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots), ASX_OK);
    ASSERT_EQ(asx_dist_recovery_get_state(&recovery), ASX_DIST_RECOVERY_RESTORING);

    ASSERT_EQ(asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots), ASX_OK);
    ASSERT_EQ(asx_dist_recovery_get_state(&recovery), ASX_DIST_RECOVERY_REASSIGNING);
    ASSERT_EQ(recovery.snapshots_restored, 1u);

    ASSERT_EQ(asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots), ASX_OK);
    ASSERT_EQ(asx_dist_recovery_get_state(&recovery), ASX_DIST_RECOVERY_COMPLETE);
    ASSERT_TRUE(asx_dist_recovery_is_complete(&recovery));
    ASSERT_TRUE(recovery.partitions_reassigned > 0u);

    /* Primary should be down */
    ASSERT_TRUE(asx_dist_cluster_find(&cluster, "primary") == NULL);
    /* Backup should now own all partitions */
    ASSERT_EQ(asx_dist_cluster_active_count(&cluster), 1u);
}

TEST(recovery_double_start_rejected) {
    asx_dist_recovery recovery;
    asx_dist_recovery_init(&recovery);
    ASSERT_EQ(asx_dist_recovery_start(&recovery, 1), ASX_OK);
    ASSERT_EQ(asx_dist_recovery_start(&recovery, 2), ASX_E_INVALID_STATE);
}

TEST(recovery_null_args) {
    ASSERT_EQ(asx_dist_recovery_start(NULL, 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_dist_recovery_step(NULL, NULL, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* Edge case: ring lookup with single node returns that node for any key */
TEST(ring_single_node_covers_all_keys) {
    asx_dist_cluster cluster;
    asx_dist_node *n;
    asx_dist_hash_ring ring;
    uint32_t i;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "solo", &n);
    asx_dist_node_set_state(n, ASX_DIST_NODE_ACTIVE);

    asx_dist_ring_init(&ring);
    ASSERT_EQ(asx_dist_ring_build(&ring, &cluster), ASX_OK);

    for (i = 0u; i < 100u; i++) { ASSERT_EQ(asx_dist_ring_lookup(&ring, i * 12345u), n->id); }
}

/* Edge case: node re-add after removal gets new ID */
TEST(cluster_readd_after_remove) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;
    uint32_t id1;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "node", &n1);
    id1 = n1->id;
    asx_dist_cluster_remove_node(&cluster, "node");
    ASSERT_EQ(asx_dist_cluster_add_node(&cluster, "node", &n2), ASX_OK);
    ASSERT_NE(n2->id, id1);
}

/* Edge case: rebalance after multiple failures leaves surviving nodes */
TEST(assignment_multi_failure_rebalance) {
    asx_dist_cluster cluster;
    asx_dist_node *nodes[4];
    asx_dist_assignment assign;
    uint32_t i;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "a", &nodes[0]);
    asx_dist_cluster_add_node(&cluster, "b", &nodes[1]);
    asx_dist_cluster_add_node(&cluster, "c", &nodes[2]);
    asx_dist_cluster_add_node(&cluster, "d", &nodes[3]);
    for (i = 0; i < 4; i++) asx_dist_node_set_state(nodes[i], ASX_DIST_NODE_ACTIVE);

    asx_dist_assignment_init(&assign, 8);
    ASSERT_EQ(asx_dist_assignment_rebalance(&assign, &cluster), ASX_OK);

    /* Kill two nodes */
    asx_dist_cluster_remove_node(&cluster, "a");
    asx_dist_cluster_remove_node(&cluster, "c");
    ASSERT_EQ(asx_dist_cluster_active_count(&cluster), 2u);

    /* Rebalance with survivors */
    ASSERT_EQ(asx_dist_assignment_rebalance(&assign, &cluster), ASX_OK);
    for (i = 0; i < 8; i++) {
        uint32_t owner = asx_dist_assignment_owner(&assign, i);
        ASSERT_TRUE(owner == nodes[1]->id || owner == nodes[3]->id);
    }
}

/* Edge case: snapshot store exhaustion */
TEST(snapshot_exhaustion) {
    asx_dist_snapshot_store store;
    asx_dist_snapshot *snap;
    uint32_t i;

    asx_dist_snapshot_store_init(&store);
    for (i = 0; i < ASX_DIST_MAX_SNAPSHOTS; i++) {
        ASSERT_EQ(asx_dist_snapshot_create(&store, 1, "data", 4, &snap), ASX_OK);
    }
    ASSERT_EQ(asx_dist_snapshot_create(&store, 1, "overflow", 8, &snap), ASX_E_RESOURCE_EXHAUSTED);
}

/* Edge case: recovery with no snapshots still completes */
TEST(recovery_no_snapshots) {
    asx_dist_cluster cluster;
    asx_dist_node *n1, *n2;
    asx_dist_assignment assign;
    asx_dist_snapshot_store snapshots;
    asx_dist_recovery recovery;

    asx_dist_cluster_init(&cluster);
    asx_dist_cluster_add_node(&cluster, "p", &n1);
    asx_dist_cluster_add_node(&cluster, "b", &n2);
    asx_dist_node_set_state(n1, ASX_DIST_NODE_ACTIVE);
    asx_dist_node_set_state(n2, ASX_DIST_NODE_ACTIVE);
    asx_dist_assignment_init(&assign, 4);
    asx_dist_assignment_rebalance(&assign, &cluster);
    asx_dist_snapshot_store_init(&snapshots); /* empty — no snapshots */

    asx_dist_recovery_init(&recovery);
    asx_dist_recovery_start(&recovery, n1->id);

    /* Step through all phases */
    asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots);
    asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots);
    asx_dist_recovery_step(&recovery, &cluster, &assign, &snapshots);

    ASSERT_TRUE(asx_dist_recovery_is_complete(&recovery));
    ASSERT_EQ(recovery.snapshots_restored, 0u);
    ASSERT_EQ(asx_dist_recovery_get_state(&recovery), ASX_DIST_RECOVERY_COMPLETE);
}

/* Edge case: cluster node exhaustion */
TEST(cluster_exhaustion) {
    asx_dist_cluster cluster;
    asx_dist_node *n;
    uint32_t i;
    char name[8];

    asx_dist_cluster_init(&cluster);
    for (i = 0; i < ASX_DIST_MAX_NODES; i++) {
        name[0] = 'A' + (char)(i % 26);
        name[1] = '0' + (char)(i / 26);
        name[2] = '\0';
        ASSERT_EQ(asx_dist_cluster_add_node(&cluster, name, &n), ASX_OK);
    }
    ASSERT_EQ(asx_dist_cluster_add_node(&cluster, "overflow", &n), ASX_E_RESOURCE_EXHAUSTED);
}

int main(void) {
    fprintf(stderr, "=== distributed tests ===\n");

    /* Cluster */
    RUN_TEST(cluster_add_and_find);
    RUN_TEST(cluster_remove);
    RUN_TEST(cluster_duplicate_rejected);
    RUN_TEST(cluster_active_count);

    /* Hash */
    RUN_TEST(hash_deterministic);
    RUN_TEST(hash_different_keys_differ);
    RUN_TEST(ring_build_and_lookup);
    RUN_TEST(ring_single_node_covers_all_keys);

    /* Cluster edge cases */
    RUN_TEST(cluster_readd_after_remove);
    RUN_TEST(cluster_exhaustion);

    /* Assignment */
    RUN_TEST(assignment_rebalance);
    RUN_TEST(assignment_no_active_nodes);
    RUN_TEST(assignment_multi_failure_rebalance);

    /* Snapshot */
    RUN_TEST(snapshot_create_and_restore);
    RUN_TEST(snapshot_latest);
    RUN_TEST(snapshot_missing_node);
    RUN_TEST(snapshot_exhaustion);

    /* Recovery */
    RUN_TEST(recovery_full_cycle);
    RUN_TEST(recovery_no_snapshots);
    RUN_TEST(recovery_double_start_rejected);
    RUN_TEST(recovery_null_args);

    TEST_REPORT();
    return test_failures;
}
