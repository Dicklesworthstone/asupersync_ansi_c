/*
 * child_region_harness.c — CBMC harness for parent-child region invariants
 *
 * Verifies:
 * 1. Attached children are always reflected in the parent's child_count/list.
 * 2. Closing a child unlinks it exactly once (no double-decrement).
 * 3. Parent child_count stays within ASX_MAX_REGION_CHILDREN.
 * 4. Every listed child points back to the parent via parent_id.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "../../../src/runtime/runtime_internal.h"
#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>

#define HARNESS_CHILDREN 3u

static int passes = 0;
static int failures = 0;

static int parent_lists_child(const asx_region_slot *parent_slot, asx_region_id child) {
    uint32_t i;

    for (i = 0; i < parent_slot->child_count; i++) {
        if (parent_slot->children[i] == child) return 1;
    }

    return 0;
}

static int count_attached_children(asx_region_id parent, asx_region_id *children, uint32_t count) {
    uint32_t i;
    int attached = 0;

    for (i = 0; i < count; i++) {
        asx_region_slot *child_slot = NULL;
        if (asx_region_slot_lookup(children[i], &child_slot) != ASX_OK) continue;
        if (child_slot->parent_id == parent) attached++;
    }

    return attached;
}

static int verify_parent_child_consistency(asx_region_id parent, asx_region_slot *parent_slot,
                                           asx_region_id *children, uint32_t count) {
    uint32_t i;

    if (parent_slot->child_count > ASX_MAX_REGION_CHILDREN) return 0;
    if ((int)parent_slot->child_count != count_attached_children(parent, children, count)) return 0;

    for (i = 0; i < parent_slot->child_count; i++) {
        asx_region_slot *listed_child = NULL;
        if (asx_region_slot_lookup(parent_slot->children[i], &listed_child) != ASX_OK) return 0;
        if (listed_child->parent_id != parent) return 0;
    }

    for (i = 0; i < count; i++) {
        asx_region_slot *child_slot = NULL;
        if (asx_region_slot_lookup(children[i], &child_slot) != ASX_OK) return 0;

        if (child_slot->parent_id == parent) {
            if (parent_slot->child_count == 0u) return 0;
            if (!parent_lists_child(parent_slot, children[i])) return 0;
        } else if (parent_lists_child(parent_slot, children[i])) {
            return 0;
        }
    }

    return 1;
}

#ifdef CBMC

int main(void) {
    asx_region_id parent;
    asx_region_id children[HARNESS_CHILDREN];
    asx_region_slot *parent_slot = NULL;
    unsigned open_count = NONDET_UINT();
    unsigned close_count = NONDET_UINT();
    unsigned step;

    ASSUME(open_count <= HARNESS_CHILDREN);
    ASSUME(close_count <= open_count);

    asx_runtime_reset();
    VERIFY(asx_region_open(&parent) == ASX_OK);
    VERIFY(asx_region_slot_lookup(parent, &parent_slot) == ASX_OK);

    for (step = 0; step < open_count; step++) {
        asx_region_slot *child_slot = NULL;

        VERIFY(asx_region_open_child(parent, &children[step]) == ASX_OK);
        VERIFY(asx_region_slot_lookup(children[step], &child_slot) == ASX_OK);
        VERIFY(child_slot->parent_id == parent);
        VERIFY(parent_slot->child_count == (uint32_t)(step + 1u));
        VERIFY(parent_lists_child(parent_slot, children[step]));
        VERIFY(
            verify_parent_child_consistency(parent, parent_slot, children, (uint32_t)(step + 1u)));
    }

    if (open_count > 0u) {
        asx_budget budget = asx_budget_from_polls(1);
        asx_status st = asx_region_drain(parent, &budget);
        VERIFY(st == ASX_E_PENDING);
        VERIFY(parent_slot->state == ASX_REGION_CLOSING);
        VERIFY(verify_parent_child_consistency(parent, parent_slot, children, open_count));
    }

    for (step = 0; step < close_count; step++) {
        asx_region_slot *child_slot = NULL;
        uint32_t before;
        asx_budget budget = asx_budget_from_polls(1);

        VERIFY(asx_region_slot_lookup(children[step], &child_slot) == ASX_OK);
        child_slot->state = ASX_REGION_FINALIZING;
        before = parent_slot->child_count;
        VERIFY(asx_region_drain(children[step], &budget) == ASX_OK);
        VERIFY(parent_slot->child_count + 1u == before);
        VERIFY(child_slot->parent_id == ASX_INVALID_ID);
        VERIFY(!parent_lists_child(parent_slot, children[step]));
        VERIFY(verify_parent_child_consistency(parent, parent_slot, children, open_count));

        budget = asx_budget_from_polls(1);
        VERIFY(asx_region_drain(children[step], &budget) == ASX_OK);
        VERIFY(parent_slot->child_count + 1u == before);
        VERIFY(verify_parent_child_consistency(parent, parent_slot, children, open_count));
    }

    return 0;
}

#else

static void check(int cond, const char *msg, unsigned a, unsigned b) {
    if (!cond) {
        fprintf(stderr, "  FAIL: %s (%u, %u)\n", msg, a, b);
        failures++;
    } else {
        passes++;
    }
}

int main(void) {
    unsigned open_count;

    fprintf(stderr, "[child-region-harness] CBMC-compatible verification\n");

    for (open_count = 0; open_count <= HARNESS_CHILDREN; open_count++) {
        asx_region_id parent;
        asx_region_id children[HARNESS_CHILDREN];
        asx_region_slot *parent_slot = NULL;
        unsigned i;

        asx_runtime_reset();
        check(asx_region_open(&parent) == ASX_OK, "open parent", open_count, 0);
        check(asx_region_slot_lookup(parent, &parent_slot) == ASX_OK, "lookup parent", open_count,
              0);

        for (i = 0; i < open_count; i++) {
            asx_region_slot *child_slot = NULL;

            check(asx_region_open_child(parent, &children[i]) == ASX_OK, "open child", open_count,
                  i);
            check(asx_region_slot_lookup(children[i], &child_slot) == ASX_OK, "lookup child",
                  open_count, i);
            check(child_slot->parent_id == parent, "child points to parent", open_count, i);
            check(parent_slot->child_count == (uint32_t)(i + 1u), "child_count increments",
                  open_count, i);
            check(parent_lists_child(parent_slot, children[i]), "parent lists child", open_count,
                  i);
            check(verify_parent_child_consistency(parent, parent_slot, children,
                                                  (uint32_t)(i + 1u)),
                  "open-state consistency", open_count, i);
        }

        check(parent_slot->child_count <= ASX_MAX_REGION_CHILDREN, "child_count bounded",
              open_count, 0);

        if (open_count > 0u) {
            asx_budget budget = asx_budget_from_polls(1);
            check(asx_region_drain(parent, &budget) == ASX_E_PENDING, "parent waits for children",
                  open_count, 0);
            check(parent_slot->state == ASX_REGION_CLOSING, "parent remains closing", open_count,
                  0);
            check(verify_parent_child_consistency(parent, parent_slot, children, open_count),
                  "pending-drain consistency", open_count, 0);
        }

        for (i = 0; i < open_count; i++) {
            asx_region_slot *child_slot = NULL;
            uint32_t before;
            asx_budget budget = asx_budget_from_polls(1);

            check(asx_region_slot_lookup(children[i], &child_slot) == ASX_OK,
                  "lookup child pre-close", open_count, i);
            child_slot->state = ASX_REGION_FINALIZING;
            before = parent_slot->child_count;
            check(asx_region_drain(children[i], &budget) == ASX_OK, "close child", open_count, i);
            check(parent_slot->child_count + 1u == before, "single decrement", open_count, i);
            check(child_slot->parent_id == ASX_INVALID_ID, "child unlinked", open_count, i);
            check(!parent_lists_child(parent_slot, children[i]), "parent removed child", open_count,
                  i);
            check(verify_parent_child_consistency(parent, parent_slot, children, open_count),
                  "post-close consistency", open_count, i);

            budget = asx_budget_from_polls(1);
            check(asx_region_drain(children[i], &budget) == ASX_OK, "double close tolerated",
                  open_count, i);
            check(parent_slot->child_count + 1u == before, "double close no underflow", open_count,
                  i);
            check(verify_parent_child_consistency(parent, parent_slot, children, open_count),
                  "double-close consistency", open_count, i);
        }

        {
            asx_budget budget = asx_budget_from_polls(1);
            check(asx_region_drain(parent, &budget) == ASX_OK, "parent closes after children",
                  open_count, 0);
            check(parent_slot->state == ASX_REGION_CLOSED, "parent reaches closed", open_count, 0);
            check(parent_slot->child_count == 0u, "no children remain", open_count, 0);
        }
    }

    fprintf(stderr, "[child-region-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
