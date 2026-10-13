/*
 * monitor.c — task monitors and DOWN notifications (Rust src/monitor.rs)
 *
 * Rust's MonitorSet keeps four BTreeMap indexes (by reference, monitored
 * task, watcher, watcher region) and TaskWatches pairs each monitor with the
 * slot that receives its notice. Here one bounded table holds both; every
 * lookup is a scan of at most ASX_MONITOR_CAPACITY entries, and the only
 * order the contract asks for is ascending reference, which a sort of the
 * firing monitors gives.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/monitor/monitor.h>
#include <asx/runtime/runtime.h>
#include <string.h>

typedef enum {
    MON_FREE = 0,
    MON_ACTIVE, /* registered, its task has not finished */
    MON_FIRED   /* a polled monitor's notification is stored, unreleased */
} mon_state;

typedef struct {
    uint8_t state;
    uint8_t to_server; /* sink: a server's system lane, else the stored notice */
    asx_monitor_ref ref;
    asx_task_id watcher;
    asx_task_id monitored;
    asx_actor_handle server;
    asx_down notice;
} mon_entry;

static mon_entry g_mon[ASX_MONITOR_CAPACITY];
static asx_monitor_ref g_next_ref = 1u;

/* A task handle carries the task's state in its tag, so the handle the
 * runtime holds at completion is not bit-equal to the one a monitor was made
 * with: tasks are the same when slot and generation are. */
static int same_task(asx_task_id a, asx_task_id b) {
    return asx_handle_index(a) == asx_handle_index(b);
}

/* Rust's live_owner (state.rs:4973): a task is live while its record exists
 * and is not terminal. */
static int task_is_live(asx_task_id task) {
    asx_task_state state;
    if (task == ASX_INVALID_ID) return 0;
    if (asx_task_get_state(task, &state) != ASX_OK) return 0;
    return state != ASX_TASK_COMPLETED;
}

static mon_entry *find_ref(asx_monitor_ref ref) {
    uint32_t i;
    if (ref == 0u) return NULL;
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        if (g_mon[i].state != MON_FREE && g_mon[i].ref == ref) return &g_mon[i];
    }
    return NULL;
}

static asx_status add_monitor(asx_task_id watcher, asx_task_id target,
                              const asx_actor_handle *server, asx_monitor_ref *out_ref) {
    uint32_t i;
    if (out_ref == NULL) return ASX_E_INVALID_ARGUMENT;
    /* TaskWatches::apply resolves the holder, then the target (:1333). */
    if (!task_is_live(watcher)) return ASX_E_STALE_HANDLE;
    if (!task_is_live(target)) return ASX_E_NOT_FOUND;
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        mon_entry *e = &g_mon[i];
        if (e->state != MON_FREE) continue;
        memset(e, 0, sizeof(*e));
        e->state = MON_ACTIVE;
        e->ref = g_next_ref++;
        e->watcher = watcher;
        e->monitored = target;
        if (server != NULL) {
            e->to_server = 1u;
            e->server = *server;
        }
        *out_ref = e->ref;
        return ASX_OK;
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_monitor_establish(asx_task_id watcher, asx_task_id target,
                                 asx_monitor_ref *out_ref) {
    return add_monitor(watcher, target, NULL, out_ref);
}

asx_status asx_monitor_establish_server(asx_actor_handle server, asx_task_id target,
                                        asx_monitor_ref *out_ref) {
    asx_task_id holder = ASX_INVALID_ID;
    if (asx_actor_task_id(server, &holder) != ASX_OK) return ASX_E_STALE_HANDLE;
    return add_monitor(holder, target, &server, out_ref);
}

void asx_demonitor(asx_monitor_ref ref) {
    mon_entry *e = find_ref(ref);
    /* A fired monitor is no longer in Rust's set: demonitor ignores it and
     * the handle's drop frees what it holds (asx_monitor_release). */
    if (e != NULL && e->state == MON_ACTIVE) memset(e, 0, sizeof(*e));
}

asx_status asx_monitor_down(asx_monitor_ref ref, asx_down *out_down) {
    const mon_entry *e = find_ref(ref);
    if (out_down == NULL) return ASX_E_INVALID_ARGUMENT;
    if (e == NULL || e->to_server) return ASX_E_NOT_FOUND;
    if (e->state != MON_FIRED) return ASX_E_PENDING;
    *out_down = e->notice;
    return ASX_OK;
}

void asx_monitor_release(asx_monitor_ref ref) {
    mon_entry *e = find_ref(ref);
    if (e != NULL && !e->to_server) memset(e, 0, sizeof(*e));
}

/* The next monitor on `task` to fire: the lowest reference left. Every
 * notification of one exit has its completion time and monitored task, so
 * DownBatch::into_sorted (:509) orders them by reference alone; picking the
 * minimum repeatedly gives that order without a buffer, which matters
 * because this runs at every task completion. */
static mon_entry *next_firing(asx_task_id task) {
    mon_entry *next = NULL;
    uint32_t i;
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        mon_entry *e = &g_mon[i];
        if (e->state != MON_ACTIVE || !same_task(e->monitored, task)) continue;
        if (next == NULL || e->ref < next->ref) next = e;
    }
    return next;
}

void asx_monitor_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt) {
    mon_entry *e;
    uint32_t i;

    /* Each monitor on the task leaves the set as it fires (remove_monitored
     * removes them all first, :1404; nothing delivered here can add or
     * remove a monitor, so the order of the two is not observable). */
    while ((e = next_firing(task)) != NULL) {
        asx_down down;
        down.monitored = task;
        down.reason = outcome != NULL ? *outcome : asx_outcome_make(ASX_OUTCOME_ERR);
        down.vt = vt;
        down.ref = e->ref;
        if (e->to_server) {
            /* Delivery (WatchEffects::dispatch, :1523): the server's lane
             * takes a DOWN message; a stopped server drops it, as Rust's
             * closed lane does, and the lane cannot be full
             * (ASX_MONITOR_CAPACITY <= its capacity). */
            asx_actor_info_msg msg;
            asx_status st;
            memset(&msg, 0, sizeof(msg));
            msg.kind = ASX_ACTOR_INFO_DOWN;
            msg.task = down.monitored;
            msg.vt = down.vt;
            msg.reason = down.reason;
            msg.ref = down.ref;
            st = asx_actor_system_push(e->server, &msg);
            (void)st; /* DISCONNECTED: the server stopped, the message is dropped */
            memset(e, 0, sizeof(*e));
        } else {
            e->state = MON_FIRED;
            e->notice = down;
        }
    }

    /* The task's own monitors end with it, silently (:1425). */
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        if (g_mon[i].state == MON_ACTIVE && same_task(g_mon[i].watcher, task)) {
            memset(&g_mon[i], 0, sizeof(g_mon[i]));
        }
    }
}

uint32_t asx_monitor_count(void) {
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        if (g_mon[i].state != MON_FREE) n++;
    }
    return n;
}

void asx_monitor_reset(void) {
    memset(g_mon, 0, sizeof(g_mon));
    g_next_ref = 1u;
}
