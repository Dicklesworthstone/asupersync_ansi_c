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

#include "../runtime/runtime_internal.h"
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
    uint8_t released;  /* its handle is gone: fire without storing */
    asx_monitor_ref ref;
    asx_task_id watcher;
    asx_task_id monitored;
    asx_task_id waiter; /* a task parked in asx_monitor_down_poll, else invalid */
    /* The priority of the waker it registered (lab dispatch). */
    uint8_t waiter_prio;
    asx_actor_handle server;
    asx_down notice;
} mon_entry;

static mon_entry g_mon[ASX_MONITOR_CAPACITY];
/* One past the highest entry used since the last reset: an entry at or above
 * it is zero, so a scan stops there. The completion path scans at every task
 * completion, and with no monitor ever registered it is empty (bd-bzt3). */
static uint32_t g_mon_hwm;
static asx_monitor_ref g_next_ref = 1u;

/* The last ASX_MONITOR_CAPACITY deliveries, in the order the monitors
 * fired, oldest overwritten first: which monitor, for which exit, at what
 * completion time. The down_order oracle (oracle.c, bd-9kll.9.2) reads it;
 * nothing else does. */
static asx_monitor_delivery g_deliveries[ASX_MONITOR_CAPACITY];
static uint64_t g_delivery_total;

/* A task handle carries the task's state in its tag, so the handle the
 * runtime holds at completion is not bit-equal to the one a monitor was made
 * with: tasks are the same when runtime, slot and generation are. */
static int same_task(asx_task_id a, asx_task_id b) { return asx_handle_same_entity(a, b); }

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
    for (i = 0; i < g_mon_hwm; i++) {
        if (g_mon[i].state != MON_FREE && g_mon[i].ref == ref) return &g_mon[i];
    }
    return NULL;
}

static asx_status add_monitor(asx_task_id watcher, asx_task_id target,
                              const asx_actor_handle *server, asx_monitor_ref *out_ref) {
    uint32_t i;
    if (out_ref == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_runtime_shared_state_refused_internal("monitors")) return ASX_E_INVALID_STATE;
    /* TaskWatches::apply resolves the holder, then the target (:1333). */
    if (!task_is_live(watcher)) return ASX_E_STALE_HANDLE;
    if (!task_is_live(target)) return ASX_E_NOT_FOUND;
    for (i = 0; i < ASX_MONITOR_CAPACITY; i++) {
        mon_entry *e = &g_mon[i];
        if (e->state != MON_FREE) continue;
        memset(e, 0, sizeof(*e));
        if (i >= g_mon_hwm) g_mon_hwm = i + 1u;
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

asx_status asx_monitor_apply_internal(uint64_t watcher, uint64_t target, int to_server,
                                      uint32_t server_slot, uint32_t server_gen,
                                      uint64_t *out_ref) {
    if (to_server) {
        asx_actor_handle server;
        server.slot = server_slot;
        server.generation = server_gen;
        return asx_monitor_establish_server(server, target, out_ref);
    }
    return asx_monitor_establish(watcher, target, out_ref);
}

/* A fired monitor is no longer in Rust's set: removal ignores it, and the
 * handle's drop frees what it holds (asx_monitor_release). */
void asx_monitor_remove_internal(uint64_t ref) {
    mon_entry *e = find_ref(ref);
    if (e != NULL && e->state == MON_ACTIVE) memset(e, 0, sizeof(*e));
}

/* Rust applies a removal as a Demonitor command of the region-command lane
 * (Monitor's Drop, GenServerHandle::demonitor). Without the lab's lane it
 * is immediate, and so it is when the queue is full: nothing is lost. */
static void request_removal(asx_monitor_ref ref) {
    if (asx_lab_dispatch_active() && asx_lab_watch_demonitor_command(ref) == ASX_OK) return;
    asx_monitor_remove_internal(ref);
}

void asx_demonitor(asx_monitor_ref ref) { request_removal(ref); }

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
    if (e == NULL || e->to_server) return;
    if (e->state == MON_FIRED) {
        memset(e, 0, sizeof(*e)); /* the notice was delivered: nothing to remove */
        return;
    }
    /* Still waiting: it must not store a notice nobody will read, whether
     * the removal is applied before the exit or after. */
    e->released = 1u;
    e->waiter = ASX_INVALID_ID;
    request_removal(ref);
}

/* MonitorOpening / ServerMonitorOpening (monitor.rs:880, :1090): with the
 * lab's lane the request is a command, applied by the next step's drain,
 * and the task parks until its result is there; without it the request is
 * applied at once. */
static asx_status open_poll(asx_task_id self, asx_task_id target, const asx_actor_handle *server,
                            asx_monitor_ref *out_ref) {
    asx_task_slot *t;
    asx_status st;

    if (out_ref == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_lab_dispatch_active()) {
        return server != NULL ? asx_monitor_establish_server(*server, target, out_ref)
                              : asx_monitor_establish(self, target, out_ref);
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
        st = asx_lab_watch_monitor_command(t, target, server != NULL,
                                           server != NULL ? server->slot : 0u,
                                           server != NULL ? server->generation : 0u);
        if (st != ASX_OK) return st;
        t->region_wait = ASX_REGION_WAIT_WATCH;
        break;
    default: return ASX_E_INVALID_STATE; /* awaiting a region command of another kind */
    }
    if (t->in_poll) t->park_requested = 1;
    return ASX_E_PENDING;
}

asx_status asx_monitor_open_poll(asx_task_id self, asx_task_id target, asx_monitor_ref *out_ref) {
    return open_poll(self, target, NULL, out_ref);
}

asx_status asx_monitor_open_server_poll(asx_task_id self, asx_actor_handle server,
                                        asx_task_id target, asx_monitor_ref *out_ref) {
    return open_poll(self, target, &server, out_ref);
}

/* Monitor::down (monitor.rs:1170): the stored notice, else cancellation,
 * else wait to be woken by the exit (poll_notice, :846). */
asx_status asx_monitor_down_poll(asx_task_id self, asx_cx *cx, asx_monitor_ref ref,
                                 asx_down *out_down) {
    mon_entry *e = find_ref(ref);
    asx_task_slot *t;
    if (out_down == NULL) return ASX_E_INVALID_ARGUMENT;
    if (e == NULL || e->to_server || e->released) return ASX_E_NOT_FOUND;
    if (e->state == MON_FIRED) {
        *out_down = e->notice;
        return ASX_OK;
    }
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) return ASX_E_CANCELLED;
    if (asx_task_slot_lookup(self, &t) != ASX_OK) return ASX_E_STALE_HANDLE;
    e->waiter = self;
    e->waiter_prio = t->lab_waker_prio;
    if (t->in_poll) t->park_requested = 1;
    return ASX_E_PENDING;
}

/* The next monitor on `task` to fire: the lowest reference left. Every
 * notification of one exit has its completion time and monitored task, so
 * DownBatch::into_sorted (:509) orders them by reference alone; picking the
 * minimum repeatedly gives that order without a buffer, which matters
 * because this runs at every task completion. */
static mon_entry *next_firing(asx_task_id task) {
    mon_entry *next = NULL;
    uint32_t i;
    for (i = 0; i < g_mon_hwm; i++) {
        mon_entry *e = &g_mon[i];
        if (e->state != MON_ACTIVE || !same_task(e->monitored, task)) continue;
        if (next == NULL || e->ref < next->ref) next = e;
    }
    return next;
}

void asx_monitor_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt) {
    mon_entry *e;
    uint32_t i;

    if (g_mon_hwm == 0u) return; /* no monitor since the last reset: none watches this task */

    /* Each monitor on the task leaves the set as it fires (remove_monitored
     * removes them all first, :1404; nothing delivered here can add or
     * remove a monitor, so the order of the two is not observable). */
    while ((e = next_firing(task)) != NULL) {
        asx_down down;
        down.monitored = task;
        down.reason = outcome != NULL ? *outcome : asx_outcome_make(ASX_OUTCOME_ERR);
        down.vt = vt;
        down.ref = e->ref;
        {
            asx_monitor_delivery *d = &g_deliveries[g_delivery_total % ASX_MONITOR_CAPACITY];
            d->ref = (uint64_t)e->ref;
            d->monitored = (uint64_t)task;
            d->vt = (uint64_t)vt;
            g_delivery_total++;
        }
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
        } else if (e->released) {
            memset(e, 0, sizeof(*e)); /* nobody holds the handle: nothing to store */
        } else {
            /* The notice is stored, and the task waiting for it wakes at
             * this dispatch (WatchSlot::deliver, :803). */
            asx_task_id waiter = e->waiter;
            uint8_t prio = e->waiter_prio;
            asx_task_slot *w;
            e->state = MON_FIRED;
            e->notice = down;
            e->waiter = ASX_INVALID_ID;
            if (waiter != ASX_INVALID_ID && asx_task_slot_lookup(waiter, &w) == ASX_OK) {
                /* The stored waker keeps the priority of the poll that registered it, which
                 * the task may have left since (a cancel's cleanup budget). */
                if (asx_lab_dispatch_active())
                    asx_lab_schedule(w, prio);
                else
                    asx_task_wake_slot_internal(w);
            }
        }
    }

    /* The task's own monitors end with it, silently (:1425), and so do the
     * notices they stored: the Monitor handles that would read them are
     * dropped with the task, and nothing else can release them. A handle
     * dropped before its DOWN was delivered queues a Demonitor command
     * (Monitor::drop, :1133): a no-op when it is applied, but a command of
     * the lane, and a step that drains it is one of the lab's steps. A
     * server's monitors have no handle to drop. */
    for (i = 0; i < g_mon_hwm; i++) {
        if (g_mon[i].state != MON_FREE && same_task(g_mon[i].watcher, task)) {
            if (g_mon[i].state == MON_ACTIVE && !g_mon[i].to_server && !g_mon[i].released &&
                asx_lab_dispatch_active()) {
                (void)asx_lab_watch_demonitor_command(g_mon[i].ref);
            }
            memset(&g_mon[i], 0, sizeof(g_mon[i]));
        }
    }
}

uint32_t asx_monitor_count(void) {
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < g_mon_hwm; i++) {
        if (g_mon[i].state != MON_FREE) n++;
    }
    return n;
}

void asx_monitor_reset(void) {
    memset(g_mon, 0, (size_t)g_mon_hwm * sizeof(g_mon[0]));
    g_mon_hwm = 0u;
    memset(g_deliveries, 0, sizeof(g_deliveries));
    g_delivery_total = 0u;
    g_next_ref = 1u;
}

uint32_t asx_monitor_delivery_count_internal(void) {
    return g_delivery_total < (uint64_t)ASX_MONITOR_CAPACITY ? (uint32_t)g_delivery_total
                                                             : (uint32_t)ASX_MONITOR_CAPACITY;
}

/* Index 0 is the oldest retained delivery. */
int asx_monitor_delivery_get_internal(uint32_t index, asx_monitor_delivery *out) {
    uint32_t n = asx_monitor_delivery_count_internal();
    uint64_t first;
    if (out == NULL || index >= n) return 0;
    first = g_delivery_total - (uint64_t)n;
    *out = g_deliveries[(first + (uint64_t)index) % (uint64_t)ASX_MONITOR_CAPACITY];
    return 1;
}
