/*
 * link.c — task links and exit signals (Rust src/link.rs)
 *
 * Rust's LinkSet keeps a BTreeMap of records and indexes by task and by
 * region, and TaskWatches pairs each link with the slot that receives its
 * exit signal. Here one bounded table holds both; a lookup is a scan of at
 * most ASX_LINK_CAPACITY entries. A record names its holder (task_a, whose
 * policy is `policy_a`) and its peer (task_b, which always propagates).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/link/link.h>
#include <asx/runtime/runtime.h>
#include <string.h>

typedef enum {
    LK_FREE = 0,
    LK_ACTIVE,   /* in the set: an exit of either task acts on it */
    LK_SIGNALLED /* out of the set; a polled holder's exit signal is stored */
} lk_state;

typedef struct {
    uint8_t state;
    uint8_t policy_a;  /* the holder's side; the peer's is Propagate */
    uint8_t to_server; /* the holder is a server: signals go to its lane */
    uint8_t has_signal;
    uint8_t released;
    asx_link_ref ref;
    asx_task_id task_a; /* the holder */
    asx_task_id task_b; /* the peer */
    asx_task_id waiter; /* a task parked in asx_link_exit_poll, else invalid */
    /* The priority of the waker it registered (lab dispatch). */
    uint8_t waiter_prio;
    asx_actor_handle server;
    asx_exit_signal signal;
} lk_entry;

static lk_entry g_link[ASX_LINK_CAPACITY];
/* One past the highest entry used since the last reset: an entry at or above
 * it is zero, so a scan stops there (bd-bzt3; the completion path scans at
 * every task completion, and with no link registered it is empty). */
static uint32_t g_link_hwm;
static asx_link_ref g_next_ref = 1u;

/* The reason a linked peer is cancelled with when the exiting task had been
 * cancelled: it points at a copy of that task's reason, one per task slot,
 * because the exiting task's own slot may be released before the peer
 * applies the cancel. */
static asx_cancel_reason g_cause[ASX_MAX_TASKS];
static uint32_t g_cause_hwm; /* one past the highest slot written since the last reset */

/* A task handle carries the task's state in its tag: tasks are the same
 * when runtime, slot and generation are (as asx/monitor/monitor.c). */
static int same_task(asx_task_id a, asx_task_id b) { return asx_handle_same_entity(a, b); }

static int task_is_live(asx_task_id task) {
    asx_task_state state;
    if (task == ASX_INVALID_ID) return 0;
    if (asx_task_get_state(task, &state) != ASX_OK) return 0;
    return state != ASX_TASK_COMPLETED;
}

static lk_entry *find_ref(asx_link_ref ref) {
    uint32_t i;
    if (ref == 0u) return NULL;
    for (i = 0; i < g_link_hwm; i++) {
        if (g_link[i].state != LK_FREE && g_link[i].ref == ref) return &g_link[i];
    }
    return NULL;
}

static asx_status add_link(asx_task_id holder, asx_task_id peer, asx_exit_policy policy,
                           const asx_actor_handle *server, asx_link_ref *out_ref) {
    uint32_t i;
    if (out_ref == NULL || (policy != ASX_EXIT_PROPAGATE && policy != ASX_EXIT_TRAP)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (asx_runtime_shared_state_refused_internal("links")) return ASX_E_INVALID_STATE;
    /* TaskWatches::apply resolves the holder, then the peer (:1363). */
    if (!task_is_live(holder)) return ASX_E_STALE_HANDLE;
    if (!task_is_live(peer)) return ASX_E_NOT_FOUND;
    for (i = 0; i < ASX_LINK_CAPACITY; i++) {
        lk_entry *e = &g_link[i];
        if (e->state != LK_FREE) continue;
        memset(e, 0, sizeof(*e));
        if (i >= g_link_hwm) g_link_hwm = i + 1u;
        e->state = LK_ACTIVE;
        e->ref = g_next_ref++;
        e->task_a = holder;
        e->task_b = peer;
        e->policy_a = (uint8_t)policy;
        if (server != NULL) {
            e->to_server = 1u;
            e->server = *server;
        }
        *out_ref = e->ref;
        return ASX_OK;
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_link_establish(asx_task_id task, asx_task_id peer, asx_exit_policy policy,
                              asx_link_ref *out_ref) {
    return add_link(task, peer, policy, NULL, out_ref);
}

asx_status asx_link_establish_server(asx_actor_handle server, asx_task_id peer,
                                     asx_exit_policy policy, asx_link_ref *out_ref) {
    asx_task_id holder = ASX_INVALID_ID;
    if (asx_actor_task_id(server, &holder) != ASX_OK) return ASX_E_STALE_HANDLE;
    return add_link(holder, peer, policy, &server, out_ref);
}

asx_status asx_link_apply_internal(uint64_t holder, uint64_t peer, int to_server,
                                   uint32_t server_slot, uint32_t server_gen, uint32_t policy,
                                   uint64_t *out_ref) {
    if (to_server) {
        asx_actor_handle server;
        server.slot = server_slot;
        server.generation = server_gen;
        return asx_link_establish_server(server, peer, (asx_exit_policy)policy, out_ref);
    }
    return asx_link_establish(holder, peer, (asx_exit_policy)policy, out_ref);
}

void asx_link_remove_internal(uint64_t ref) {
    lk_entry *e = find_ref(ref);
    if (e != NULL && e->state == LK_ACTIVE) memset(e, 0, sizeof(*e));
}

/* A removal is a Demonitor-like command of the region-command lane under
 * lab dispatch (WatchCommand::Unlink); immediate otherwise or when the
 * queue is full. */
void asx_unlink(asx_link_ref ref) {
    if (asx_lab_dispatch_active() && asx_lab_watch_unlink_command(ref) == ASX_OK) return;
    asx_link_remove_internal(ref);
}

void asx_link_release(asx_link_ref ref) {
    lk_entry *e = find_ref(ref);
    if (e == NULL || e->to_server) return;
    if (e->state == LK_SIGNALLED) {
        memset(e, 0, sizeof(*e));
        return;
    }
    /* Still in the set: the link goes on acting; only a signal nobody will
     * read must not be stored. */
    e->released = 1u;
    e->waiter = ASX_INVALID_ID;
}

/* LinkOpening / ServerWatchOpening for a link (link.rs:824): the same
 * lane handshake as a monitor request (asx/monitor/monitor.c), with the
 * exit policy riding in the command. */
static asx_status open_poll(asx_task_id self, asx_task_id peer, asx_exit_policy policy,
                            const asx_actor_handle *server, asx_link_ref *out_ref) {
    asx_task_slot *t;
    asx_status st;

    if (out_ref == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_lab_dispatch_active()) {
        return server != NULL ? asx_link_establish_server(*server, peer, policy, out_ref)
                              : asx_link_establish(self, peer, policy, out_ref);
    }
    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    switch (t->region_wait) {
    case ASX_REGION_WAIT_WATCHED:
        t->region_wait = ASX_REGION_WAIT_NONE;
        if (t->region_wait_status == ASX_OK) *out_ref = t->region_wait_region;
        return t->region_wait_status;
    case ASX_REGION_WAIT_WATCH: break;
    case ASX_REGION_WAIT_NONE:
        st = asx_lab_watch_link_command(t, peer, server != NULL, server != NULL ? server->slot : 0u,
                                        server != NULL ? server->generation : 0u, (uint32_t)policy);
        if (st != ASX_OK) return st;
        t->region_wait = ASX_REGION_WAIT_WATCH;
        break;
    default: return ASX_E_INVALID_STATE;
    }
    if (t->in_poll) t->park_requested = 1;
    return ASX_E_PENDING;
}

asx_status asx_link_open_poll(asx_task_id self, asx_task_id peer, asx_exit_policy policy,
                              asx_link_ref *out_ref) {
    return open_poll(self, peer, policy, NULL, out_ref);
}

asx_status asx_link_open_server_poll(asx_task_id self, asx_actor_handle server, asx_task_id peer,
                                     asx_exit_policy policy, asx_link_ref *out_ref) {
    return open_poll(self, peer, policy, &server, out_ref);
}

asx_status asx_link_exit_poll(asx_task_id self, asx_cx *cx, asx_link_ref ref,
                              asx_exit_signal *out_signal) {
    lk_entry *e = find_ref(ref);
    asx_task_slot *t;
    if (out_signal == NULL) return ASX_E_INVALID_ARGUMENT;
    if (e == NULL || e->to_server || e->released || e->policy_a != (uint8_t)ASX_EXIT_TRAP) {
        return ASX_E_NOT_FOUND;
    }
    if (e->has_signal) {
        *out_signal = e->signal;
        return ASX_OK;
    }
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) return ASX_E_CANCELLED;
    if (asx_task_slot_lookup(self, &t) != ASX_OK) return ASX_E_STALE_HANDLE;
    e->waiter = self;
    e->waiter_prio = t->lab_waker_prio;
    if (t->in_poll) t->park_requested = 1;
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* An exit                                                             */
/* ------------------------------------------------------------------ */

/* The task on the other side of `e` from `task`, and the exit policy of
 * that side. A self-link is one edge: its policy is the holder's on both. */
static asx_task_id peer_of(const lk_entry *e, asx_task_id task) {
    return same_task(e->task_a, task) ? e->task_b : e->task_a;
}

static uint8_t policy_of_peer(const lk_entry *e, asx_task_id task) {
    if (same_task(e->task_a, e->task_b)) return e->policy_a;
    /* The crashed side is `task`; the policy that decides is the other's. */
    return same_task(e->task_a, task) ? (uint8_t)ASX_EXIT_PROPAGATE : e->policy_a;
}

static int involves(const lk_entry *e, asx_task_id task) {
    return e->state == LK_ACTIVE && (same_task(e->task_a, task) || same_task(e->task_b, task));
}

/* The order EXIT-ORDER gives the actions of one exit: the peer's TaskId
 * (the lab's Rust arena index and generation), then the link reference. */
static uint64_t peer_key(asx_task_id peer) { return asx_lab_task_order_key_internal(peer); }

/* The next link of `task` after (`last_key`, `last_ref`) in that order, or
 * NULL. `by_peer` 0 orders by reference alone (peers_of's order, used for a
 * normal exit). */
static lk_entry *next_link(asx_task_id task, int by_peer, int started, uint64_t last_key,
                           asx_link_ref last_ref) {
    lk_entry *best = NULL;
    uint64_t best_key = 0;
    uint32_t i;
    for (i = 0; i < g_link_hwm; i++) {
        lk_entry *e = &g_link[i];
        uint64_t key;
        if (!involves(e, task)) continue;
        key = by_peer ? peer_key(peer_of(e, task)) : 0u;
        if (started && (key < last_key || (key == last_key && e->ref <= last_ref))) continue;
        if (best == NULL || key < best_key || (key == best_key && e->ref < best->ref)) {
            best = e;
            best_key = key;
        }
    }
    return best;
}

/* push_trapped_exit (monitor.rs:1481): only the link's holder is sent a
 * signal. A server's lane takes an EXIT message (a stopped server drops
 * it); a polled holder stores it and its waiter wakes (WatchSlot::deliver).
 * The entry stays in the table until the exit has been applied. */
static void deliver_exit(lk_entry *e, asx_task_id from, const asx_outcome *reason, asx_time vt) {
    if (e->to_server) {
        asx_actor_info_msg msg;
        asx_status st;
        memset(&msg, 0, sizeof(msg));
        msg.kind = ASX_ACTOR_INFO_EXIT;
        msg.task = from;
        msg.vt = vt;
        msg.reason = *reason;
        msg.ref = e->ref;
        st = asx_actor_system_push(e->server, &msg);
        (void)st; /* DISCONNECTED: the server stopped, the message is dropped */
    } else if (!e->released) {
        asx_task_slot *w;
        e->signal.from = from;
        e->signal.reason = *reason;
        e->signal.vt = vt;
        e->signal.ref = e->ref;
        e->has_signal = 1u;
        if (e->waiter != ASX_INVALID_ID && asx_task_slot_lookup(e->waiter, &w) == ASX_OK) {
            e->waiter = ASX_INVALID_ID;
            /* The stored waker keeps the priority of the poll that registered it. */
            if (asx_lab_dispatch_active())
                asx_lab_schedule(w, e->waiter_prio);
            else
                asx_task_wake_slot_internal(w);
        }
    }
}

/* linked_exit_cancel_reason (link.rs:610): CancelReason::new(LinkedExit)
 * with the message "link exit"; a cancelled exit is its cause. */
static void linked_exit_reason(asx_cancel_reason *out, const asx_outcome *exit, asx_task_id peer) {
    *out = asx_cancel_reason_default(ASX_CANCEL_LINKED_EXIT, "link exit");
    if (exit->severity == ASX_OUTCOME_CANCELLED) {
        uint32_t slot = (uint32_t)asx_handle_slot(peer);
        if (slot < ASX_MAX_TASKS) {
            if (slot >= g_cause_hwm) g_cause_hwm = slot + 1u;
            g_cause[slot] = exit->cancel_reason;
            asx_cancel_reason_with_cause_limited(out, &g_cause[slot]);
        }
    }
}

void asx_link_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt) {
    asx_outcome reason;
    int normal;
    lk_entry *e;
    uint64_t last_key = 0;
    asx_link_ref last_ref = 0;
    int started = 0;
    uint32_t i;

    if (g_link_hwm == 0u) return; /* no link since the last reset: nothing ends with this task */
    reason = outcome != NULL ? *outcome : asx_outcome_make(ASX_OUTCOME_ERR);
    normal = reason.severity == ASX_OUTCOME_OK;

    /* The exit's signals: a normal exit tells the trapping holders among
     * the peers (peers_of order, by reference); an abnormal one resolves
     * its actions in EXIT-ORDER and delivers the trapped ones. */
    while ((e = next_link(task, !normal, started, last_key, last_ref)) != NULL) {
        asx_task_id peer = peer_of(e, task);
        started = 1;
        last_key = !normal ? peer_key(peer) : 0u;
        last_ref = e->ref;
        if (policy_of_peer(e, task) == (uint8_t)ASX_EXIT_TRAP && same_task(e->task_a, peer)) {
            deliver_exit(e, task, &reason, vt);
        }
    }

    /* Then the cancels, in the same order: each propagating peer is
     * cancelled through the handle-cancel lane (enqueue_handle_cancel). */
    if (!normal) {
        started = 0;
        last_key = 0;
        last_ref = 0;
        while ((e = next_link(task, 1, started, last_key, last_ref)) != NULL) {
            asx_task_id peer = peer_of(e, task);
            started = 1;
            last_key = peer_key(peer);
            last_ref = e->ref;
            if (policy_of_peer(e, task) == (uint8_t)ASX_EXIT_PROPAGATE) {
                asx_cancel_reason why;
                asx_status st;
                linked_exit_reason(&why, &reason, peer);
                st = asx_task_abort_request(peer, &why);
                (void)st; /* a peer that is gone has nothing to cancel */
            }
        }
    }

    /* Every link the task had ends (remove_task). A trapping polled holder
     * that is still running keeps the signal this exit stored, for the Link
     * handle it holds; one that is the exiting task itself (a self-link), or
     * that exits later, drops the handle with the task, so a stored signal
     * goes with its holder. */
    for (i = 0; i < g_link_hwm; i++) {
        lk_entry *l = &g_link[i];
        if (l->state == LK_SIGNALLED && same_task(l->task_a, task)) {
            memset(l, 0, sizeof(*l));
        } else if (involves(l, task)) {
            if (l->has_signal && !l->released && !same_task(l->task_a, task)) {
                l->state = LK_SIGNALLED;
            } else {
                memset(l, 0, sizeof(*l));
            }
        }
    }
}

/* TaskWatches::on_task_completed (monitor.rs:1386): the DOWNs go first, then
 * the exit signals and the cancels (WatchEffects::dispatch, :1523). */
void asx_watches_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt) {
    asx_monitor_task_finished(task, outcome, vt);
    asx_link_task_finished(task, outcome, vt);
}

uint32_t asx_link_count(void) {
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < g_link_hwm; i++) {
        if (g_link[i].state != LK_FREE) n++;
    }
    return n;
}

void asx_link_reset(void) {
    memset(g_link, 0, (size_t)g_link_hwm * sizeof(g_link[0]));
    g_link_hwm = 0u;
    memset(g_cause, 0, (size_t)g_cause_hwm * sizeof(g_cause[0]));
    g_cause_hwm = 0u;
    g_next_ref = 1u;
}
