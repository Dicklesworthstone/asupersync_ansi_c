/*
 * doctor.c — runtime diagnostic checks and health reports
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/net/net.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Check helpers                                                       */
/* ------------------------------------------------------------------ */

static void add_check(asx_doctor_report *r, const char *name, asx_doctor_severity sev,
                      const char *msg, uint32_t value, uint32_t capacity) {
    if (r->check_count >= ASX_DOCTOR_MAX_CHECKS) return;

    r->checks[r->check_count].name = name;
    r->checks[r->check_count].severity = sev;
    r->checks[r->check_count].message = msg;
    r->checks[r->check_count].value = value;
    r->checks[r->check_count].capacity = capacity;
    r->check_count++;

    switch (sev) {
    case ASX_DOCTOR_OK: r->pass_count++; break;
    case ASX_DOCTOR_WARN: r->warn_count++; break;
    case ASX_DOCTOR_FAIL: r->fail_count++; break;
    case ASX_DOCTOR_SKIP: r->skip_count++; break;
    }
}

/* ------------------------------------------------------------------ */
/* Diagnostic checks                                                   */
/* ------------------------------------------------------------------ */

static void check_runtime_initialized(const asx_runtime *rt, asx_doctor_report *r) {
    if (asx_runtime_is_initialized(rt)) {
        add_check(r, "runtime", ASX_DOCTOR_OK, "runtime initialized", 1, 1);
    } else {
        add_check(r, "runtime", ASX_DOCTOR_FAIL, "runtime not initialized", 0, 1);
    }
}

static void check_region_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_region_count(rt);
    uint32_t cap = asx_runtime_region_capacity();
    asx_doctor_severity sev;

    if (count == 0) {
        sev = ASX_DOCTOR_OK;
    } else if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "regions", sev,
              count >= cap          ? "region arena exhausted"
              : count > cap * 3 / 4 ? "region arena >75% utilized"
                                    : "region utilization normal",
              count, cap);
}

static void check_task_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_task_count(rt);
    uint32_t cap = asx_runtime_task_capacity();
    asx_doctor_severity sev;

    if (count == 0) {
        sev = ASX_DOCTOR_OK;
    } else if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "tasks", sev,
              count >= cap          ? "task arena exhausted"
              : count > cap * 3 / 4 ? "task arena >75% utilized"
                                    : "task utilization normal",
              count, cap);
}

static void check_obligation_utilization(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_obligation_count(rt);
    uint32_t cap = asx_runtime_obligation_capacity();
    asx_doctor_severity sev;

    if (count >= cap) {
        sev = ASX_DOCTOR_FAIL;
    } else if (count > cap * 3 / 4) {
        sev = ASX_DOCTOR_WARN;
    } else {
        sev = ASX_DOCTOR_OK;
    }

    add_check(r, "obligations", sev,
              count >= cap          ? "obligation arena exhausted"
              : count > cap * 3 / 4 ? "obligation arena >75% utilized"
                                    : "obligation utilization normal",
              count, cap);
}

static void check_io_driver_state(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_io_registration_count(rt);
#if ASX_HAS_NATIVE_IO_DRIVER
    uint32_t cap = ASX_MAX_IO_TOKENS;
#else
    uint32_t cap = 0u;
#endif
    asx_doctor_severity sev;
    const char *msg;

    if (!asx_runtime_is_initialized(rt)) {
        sev = ASX_DOCTOR_FAIL;
        msg = "runtime not initialized";
        count = 0u;
        cap = 1u;
    } else if (!asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        sev = ASX_DOCTOR_SKIP;
        msg = "io driver inactive or unsupported";
    } else if (!asx_runtime_io_driver_initialized(rt)) {
        sev = ASX_DOCTOR_FAIL;
        msg = "io driver not initialized";
    } else if (count >= cap) {
        /* ASX_ANALYZER_WAIVER("config-dependent: cap is 0u without the native IO driver") */
        sev = ASX_DOCTOR_FAIL;
        msg = "io registration arena exhausted";
    } else if (count > cap * 3u / 4u) {
        sev = ASX_DOCTOR_WARN;
        msg = "io registration arena >75% utilized";
    } else {
        sev = ASX_DOCTOR_OK;
#if ASX_HAS_NATIVE_IO_DRIVER
        msg = asx_io_driver_is_live() ? "io driver initialized (live reactor)"
                                      : "io driver initialized (ghost backend: no OS readiness)";
#else
        msg = "io driver initialized";
#endif
    }

    add_check(r, "io_driver", sev, msg, count, cap);
}

static void check_blocking_pool_state(const asx_runtime *rt, asx_doctor_report *r) {
    uint32_t count = asx_runtime_blocking_active_count(rt);
#if ASX_HAS_BLOCKING_SURFACE
    uint32_t cap = ASX_MAX_BLOCKING_TASKS;
#else
    uint32_t cap = 0u;
#endif
    asx_doctor_severity sev;
    const char *msg;

    if (!asx_runtime_is_initialized(rt)) {
        sev = ASX_DOCTOR_FAIL;
        msg = "runtime not initialized";
        count = 0u;
        cap = 1u;
    } else if (!asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        sev = ASX_DOCTOR_SKIP;
        msg = "blocking pool inactive or unsupported";
    } else if (!asx_runtime_blocking_pool_initialized(rt)) {
        sev = ASX_DOCTOR_FAIL;
        msg = "blocking pool not initialized";
    } else if (count >= cap) {
        /* ASX_ANALYZER_WAIVER("config-dependent: cap is 0u without the blocking surface") */
        sev = ASX_DOCTOR_FAIL;
        msg = "blocking task arena exhausted";
    } else if (count > cap * 3u / 4u) {
        sev = ASX_DOCTOR_WARN;
        msg = "blocking task arena >75% utilized";
    } else {
        sev = ASX_DOCTOR_OK;
        msg = "blocking pool initialized";
    }

    add_check(r, "blocking", sev, msg, count, cap);
}

static void check_safety_profile(const asx_runtime *rt, asx_doctor_report *r) {
    asx_safety_profile prof = asx_runtime_safety_profile(rt);

    add_check(r, "safety_profile", ASX_DOCTOR_OK,
              prof == ASX_SAFETY_DEBUG      ? "debug (full ghost monitors)"
              : prof == ASX_SAFETY_HARDENED ? "hardened (error ledger)"
                                            : "release (zero-cost stubs)",
              (uint32_t)prof, 2);
}

static void check_containment_policy(const asx_runtime *rt, asx_doctor_report *r) {
    asx_containment_policy pol = asx_runtime_containment_policy(rt);

    add_check(r, "containment", ASX_DOCTOR_OK,
              pol == ASX_CONTAIN_FAIL_FAST       ? "fail-fast"
              : pol == ASX_CONTAIN_POISON_REGION ? "poison-region"
                                                 : "error-only",
              (uint32_t)pol, 2);
}

/* Which transport serves the net surface. A default (deterministic) build
 * listens in memory, not on the OS: an operator sees it here
 * (bd-9kll.10.2). Both are healthy; a live POSIX build is NATIVE. */
static void check_net_backend(asx_doctor_report *r) {
    asx_net_backend b = asx_net_get_backend();

    add_check(r, "net", ASX_DOCTOR_OK,
              b == ASX_NET_BACKEND_NATIVE ? "native OS sockets"
                                          : "in-memory transport (no OS sockets)",
              (uint32_t)b, 1);
}

/* ------------------------------------------------------------------ */
/* Platform probes (asx_doctor_probe)                                  */
/* ------------------------------------------------------------------ */

#define DOCTOR_PROBE_CHECKS 5u
/* Echo server, echo client, blocking waiter. */
#define DOCTOR_PROBE_MAX_TASKS 3u
/* Poll quota of the run that drives the probe tasks (each needs a few). */
#define DOCTOR_PROBE_POLLS 256u
/* Reads a real clock may return unchanged before the probe calls it
 * stuck (a nanosecond clock advances within a handful). */
#define DOCTOR_CLOCK_READ_LIMIT 1000000u

static void probe_clock(const asx_runtime *rt, asx_doctor_report *r) {
    asx_time first = 0;
    asx_time now = 0;
    uint32_t i;

    if (asx_runtime_now_ns(rt, &first) != ASX_OK) {
        add_check(r, "clock", ASX_DOCTOR_FAIL, "clock hook failed", 0, 0);
        return;
    }
    for (i = 0; i < DOCTOR_CLOCK_READ_LIMIT; i++) {
        if (asx_runtime_now_ns(rt, &now) != ASX_OK) {
            add_check(r, "clock", ASX_DOCTOR_FAIL, "clock hook failed", 0, 0);
            return;
        }
        if (now < first) {
            add_check(r, "clock", ASX_DOCTOR_FAIL, "clock went backwards", 0, 0);
            return;
        }
        /* The virtual clock moves only when the scheduler advances it. */
        if (now > first || asx_runtime_clock_is_virtual()) break;
    }
    if (now > first) {
        add_check(r, "clock", ASX_DOCTOR_OK, "monotonic clock advancing", 0, 0);
    } else if (asx_runtime_clock_is_virtual()) {
        add_check(r, "clock", ASX_DOCTOR_OK, "virtual clock (moves only with the scheduler)", 0, 0);
    } else {
        add_check(r, "clock", ASX_DOCTOR_FAIL, "clock does not advance", 0, 0);
    }
}

static void probe_entropy(const asx_runtime *rt, asx_doctor_report *r) {
    uint64_t a = 0;
    uint64_t b = 0;

    if (asx_runtime_random_u64(rt, &a) != ASX_OK || asx_runtime_random_u64(rt, &b) != ASX_OK) {
        add_check(r, "entropy", ASX_DOCTOR_FAIL, "entropy source unavailable", 0, 0);
    } else if (a == b) {
        add_check(r, "entropy", ASX_DOCTOR_FAIL, "entropy source repeats its value", 0, 0);
    } else {
        add_check(r, "entropy", ASX_DOCTOR_OK, "entropy stream yields distinct values", 0, 0);
    }
}

#if ASX_HAS_NATIVE_IO_DRIVER || ASX_HAS_BLOCKING_SURFACE
/* A probe task's checkpoint: nonzero once its region is cancelled. */
static int probe_cancelled(asx_task_id self) {
    asx_checkpoint_result cp;
    return asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled;
}
#endif

#if ASX_HAS_NATIVE_IO_DRIVER
#define DOCTOR_ECHO_BYTE 0xA5u

/* One end of the loopback echo. The server accepts, reads a byte and
 * writes it back; the client connects, writes the byte and reads the
 * echo. `waits` counts the polls that found the socket not ready, each
 * of which parked the task on the reactor. */
typedef struct {
    asx_co_state co;
    asx_tcp_listener listener; /* server: owned by asx_doctor_probe */
    asx_socket_addr addr;      /* client: where the listener is bound */
    asx_tcp_stream conn;
    int have_conn;
    uint8_t byte;
    uint32_t waits;
    asx_status result; /* ASX_E_PENDING until the task ends */
} doctor_echo_end;

/* Ends an echo task: closes its connection and records the result. A
 * failed echo still ends the task with ASX_OK (the verdict is the
 * report's); a cancel is acknowledged with ASX_E_CANCELLED. */
static asx_status echo_finish(doctor_echo_end *e, asx_status result) {
    if (e->have_conn) {
        (void)asx_tcp_stream_close(e->conn);
        e->have_conn = 0;
    }
    e->result = result;
    return result == ASX_E_CANCELLED ? ASX_E_CANCELLED : ASX_OK;
}

static asx_status echo_server_poll(void *ud, asx_task_id self) {
    doctor_echo_end *e = (doctor_echo_end *)ud;
    asx_status st = ASX_OK;
    uint32_t n = 0;

    if (probe_cancelled(self)) return echo_finish(e, ASX_E_CANCELLED);
    /* ASX_ANALYZER_WAIVER("coroutine macro: the state word takes only the labels it emits") */
    ASX_CO_BEGIN(&e->co);
    for (;;) {
        st = asx_tcp_listener_poll_accept(e->listener, &e->conn, NULL);
        if (st != ASX_E_PENDING) break;
        e->waits++;
        ASX_CO_YIELD(&e->co);
    }
    if (st != ASX_OK) return echo_finish(e, st);
    e->have_conn = 1;
    for (;;) {
        st = asx_tcp_stream_read(e->conn, &e->byte, 1u, &n);
        if (st != ASX_E_PENDING) break;
        e->waits++;
        ASX_CO_YIELD(&e->co);
    }
    if (st == ASX_OK && n != 1u) st = ASX_E_DISCONNECTED;
    if (st != ASX_OK) return echo_finish(e, st);
    for (;;) {
        st = asx_tcp_stream_write(e->conn, &e->byte, 1u, &n);
        if (st != ASX_E_PENDING) break;
        e->waits++;
        ASX_CO_YIELD(&e->co);
    }
    if (st == ASX_OK && n != 1u) st = ASX_E_DISCONNECTED;
    (void)echo_finish(e, st);
    ASX_CO_END(&e->co);
}

static asx_status echo_client_poll(void *ud, asx_task_id self) {
    doctor_echo_end *e = (doctor_echo_end *)ud;
    asx_status st = ASX_OK;
    uint32_t n = 0;

    if (probe_cancelled(self)) return echo_finish(e, ASX_E_CANCELLED);
    /* ASX_ANALYZER_WAIVER("coroutine macro: the state word takes only the labels it emits") */
    ASX_CO_BEGIN(&e->co);
    st = asx_tcp_connect(&e->conn, &e->addr);
    if (st != ASX_OK) return echo_finish(e, st);
    e->have_conn = 1;
    e->byte = (uint8_t)DOCTOR_ECHO_BYTE;
    for (;;) {
        st = asx_tcp_stream_write(e->conn, &e->byte, 1u, &n);
        if (st != ASX_E_PENDING) break;
        e->waits++;
        ASX_CO_YIELD(&e->co);
    }
    if (st == ASX_OK && n != 1u) st = ASX_E_DISCONNECTED;
    if (st != ASX_OK) return echo_finish(e, st);
    e->byte = 0u;
    for (;;) {
        st = asx_tcp_stream_read(e->conn, &e->byte, 1u, &n);
        if (st != ASX_E_PENDING) break;
        e->waits++;
        ASX_CO_YIELD(&e->co);
    }
    if (st == ASX_OK && n != 1u) st = ASX_E_DISCONNECTED;
    if (st == ASX_OK && e->byte != (uint8_t)DOCTOR_ECHO_BYTE) st = ASX_E_INVALID_STATE;
    (void)echo_finish(e, st);
    ASX_CO_END(&e->co);
}
#endif /* ASX_HAS_NATIVE_IO_DRIVER */

#if ASX_HAS_BLOCKING_SURFACE
#define DOCTOR_BLOCKING_RESULT 0x617378u

/* Submits one pool job with its own waker, then parks until it is done. */
typedef struct {
    asx_waker waker;
    asx_blocking_handle job;
    int have_waker;
    int submitted;
    uint64_t value;
    uint32_t waits;
    asx_status result; /* ASX_E_PENDING until the task ends */
} doctor_blocking_waiter;

static uint64_t doctor_blocking_job(void *user_data) {
    (void)user_data;
    return DOCTOR_BLOCKING_RESULT;
}

static asx_status blocking_finish(doctor_blocking_waiter *w, asx_status result) {
    if (w->have_waker) {
        asx_waker_deregister(&w->waker);
        w->have_waker = 0;
    }
    w->result = result;
    return result == ASX_E_CANCELLED ? ASX_E_CANCELLED : ASX_OK;
}

static asx_status blocking_waiter_poll(void *ud, asx_task_id self) {
    doctor_blocking_waiter *w = (doctor_blocking_waiter *)ud;
    asx_status st;

    if (probe_cancelled(self)) return blocking_finish(w, ASX_E_CANCELLED);
    if (!w->submitted) {
        st = asx_waker_register(self, &w->waker);
        if (st != ASX_OK) return blocking_finish(w, st);
        w->have_waker = 1;
        st = asx_spawn_blocking(doctor_blocking_job, NULL, &w->waker, &w->job);
        if (st != ASX_OK) return blocking_finish(w, st);
        w->submitted = 1;
    }
    st = asx_blocking_get_result(&w->job, &w->value);
    if (st == ASX_E_PENDING) {
        w->waits++;
        st = asx_task_park(self);
        if (st != ASX_OK) return blocking_finish(w, st);
        return ASX_E_PENDING;
    }
    if (st == ASX_OK && w->value != DOCTOR_BLOCKING_RESULT) st = ASX_E_INVALID_STATE;
    return blocking_finish(w, st);
}

/* Live builds hand blocking jobs to the platform's worker threads;
 * deterministic builds run them inline. */
static int blocking_uses_worker_threads(void) {
#if ASX_DETERMINISTIC
    return 0;
#else
    const asx_runtime_hooks *hooks = asx_runtime_get_hooks();
    return hooks != NULL && hooks->blocking.submit_fn != NULL;
#endif
}
#endif /* ASX_HAS_BLOCKING_SURFACE */

/* The task probes share one region and one bounded scheduler run. A
 * probe that spawns no task carries its verdict from the start. */
typedef struct {
    const asx_runtime *rt; /* the runtime under test: its clock, its stream */
    asx_region_id region;
    int have_region;
    asx_task_id spawned[DOCTOR_PROBE_MAX_TASKS];
    uint32_t tasks;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_tcp_listener listener;
    int have_listener;
    doctor_echo_end *server;
    doctor_echo_end *client;
    asx_doctor_severity reactor_sev;
    const char *reactor_msg;
#endif
#if ASX_HAS_BLOCKING_SURFACE
    doctor_blocking_waiter *waiter;
    asx_doctor_severity blocking_sev;
    const char *blocking_msg;
#endif
} doctor_probe_ctx;

/* Budget of a probe run: a poll quota and ASX_DOCTOR_PROBE_TIMEOUT_NS. */
static asx_budget probe_budget(const asx_runtime *rt) {
    asx_budget b = asx_budget_from_polls(DOCTOR_PROBE_POLLS);
    asx_time now = 0;

    if (asx_runtime_now_ns(rt, &now) == ASX_OK) b.deadline = now + ASX_DOCTOR_PROBE_TIMEOUT_NS;
    return b;
}

/* socket: bind a loopback listener; with a live reactor, spawn the echo. */
static void probe_socket_start(doctor_probe_ctx *p, asx_doctor_report *r) {
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_socket_addr loopback = asx_socket_addr_loopback(0);
    asx_socket_addr bound = loopback;
    asx_task_id id;
    int bound_ok = 0;

    if (asx_net_get_backend() != ASX_NET_BACKEND_NATIVE) {
        add_check(r, "socket", ASX_DOCTOR_SKIP, "in-memory transport: no OS sockets", 0, 0);
    } else if (asx_tcp_listener_bind(&p->listener, &loopback) != ASX_OK) {
        add_check(r, "socket", ASX_DOCTOR_FAIL, "loopback bind failed", 0, 0);
    } else {
        p->have_listener = 1;
        if (asx_tcp_listener_local_addr(p->listener, &bound) != ASX_OK || bound.port == 0u) {
            add_check(r, "socket", ASX_DOCTOR_FAIL, "loopback listener has no port", 0, 0);
        } else {
            add_check(r, "socket", ASX_DOCTOR_OK, "TCP listener bound on loopback", 0, 0);
            bound_ok = 1;
        }
    }

    if (!asx_io_driver_is_live()) {
        p->reactor_sev = ASX_DOCTOR_SKIP;
        p->reactor_msg = "ghost reactor: no OS readiness";
        return;
    }
    if (!bound_ok) {
        p->reactor_sev = ASX_DOCTOR_SKIP;
        p->reactor_msg = "no loopback listener to echo through";
        return;
    }
    if (!p->have_region ||
        asx_task_spawn_captured(p->region, echo_server_poll, (uint32_t)sizeof(doctor_echo_end),
                                NULL, &id, (void **)&p->server) != ASX_OK) {
        p->reactor_sev = ASX_DOCTOR_FAIL;
        p->reactor_msg = "echo tasks could not start";
        return;
    }
    memset(p->server, 0, sizeof(*p->server));
    p->server->listener = p->listener;
    p->server->result = ASX_E_PENDING;
    p->spawned[p->tasks++] = id;
    if (asx_task_spawn_captured(p->region, echo_client_poll, (uint32_t)sizeof(doctor_echo_end),
                                NULL, &id, (void **)&p->client) != ASX_OK) {
        p->client = NULL;
        p->reactor_sev = ASX_DOCTOR_FAIL;
        p->reactor_msg = "echo tasks could not start";
        return;
    }
    memset(p->client, 0, sizeof(*p->client));
    p->client->addr = bound;
    p->client->result = ASX_E_PENDING;
    p->spawned[p->tasks++] = id;
#else
    (void)p;
    add_check(r, "socket", ASX_DOCTOR_SKIP, "no OS sockets in this profile", 0, 0);
#endif
}

static void probe_blocking_start(doctor_probe_ctx *p) {
#if ASX_HAS_BLOCKING_SURFACE
    asx_task_id id;

    if (!asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        p->blocking_sev = ASX_DOCTOR_SKIP;
        p->blocking_msg = "blocking pool inactive or unsupported";
        return;
    }
    if (!p->have_region || asx_task_spawn_captured(p->region, blocking_waiter_poll,
                                                   (uint32_t)sizeof(doctor_blocking_waiter), NULL,
                                                   &id, (void **)&p->waiter) != ASX_OK) {
        p->waiter = NULL;
        p->blocking_sev = ASX_DOCTOR_FAIL;
        p->blocking_msg = "blocking probe task could not start";
        return;
    }
    memset(p->waiter, 0, sizeof(*p->waiter));
    p->waiter->result = ASX_E_PENDING;
    p->spawned[p->tasks++] = id;
#else
    (void)p;
#endif
}

static void probe_reactor_verdict(const doctor_probe_ctx *p, asx_doctor_report *r) {
#if ASX_HAS_NATIVE_IO_DRIVER
    uint32_t waits;

    if (p->server == NULL || p->client == NULL) {
        add_check(r, "reactor", p->reactor_sev, p->reactor_msg, 0, 0);
        return;
    }
    waits = p->server->waits + p->client->waits;
    if (p->server->result == ASX_E_PENDING || p->client->result == ASX_E_PENDING) {
        add_check(r, "reactor", ASX_DOCTOR_FAIL, "loopback echo timed out", waits, 0);
    } else if (p->server->result != ASX_OK || p->client->result != ASX_OK) {
        add_check(r, "reactor", ASX_DOCTOR_FAIL, "loopback echo failed", waits, 0);
    } else if (waits == 0u) {
        add_check(r, "reactor", ASX_DOCTOR_WARN, "echo completed without waiting on the reactor", 0,
                  0);
    } else {
        add_check(r, "reactor", ASX_DOCTOR_OK, "loopback echo woke its parked tasks", waits, 0);
    }
#else
    (void)p;
    add_check(r, "reactor", ASX_DOCTOR_SKIP, "no OS reactor in this profile", 0, 0);
#endif
}

static void probe_blocking_verdict(const doctor_probe_ctx *p, asx_doctor_report *r) {
#if ASX_HAS_BLOCKING_SURFACE
    if (p->waiter == NULL) {
        add_check(r, "blocking_job", p->blocking_sev, p->blocking_msg, 0, 0);
    } else if (p->waiter->result == ASX_E_PENDING) {
        add_check(r, "blocking_job", ASX_DOCTOR_FAIL, "blocking job did not complete",
                  p->waiter->waits, 0);
    } else if (p->waiter->result != ASX_OK) {
        add_check(r, "blocking_job", ASX_DOCTOR_FAIL, "blocking job failed", p->waiter->waits, 0);
    } else {
        /* ASX_ANALYZER_WAIVER("config-dependent: constant 0 under ASX_DETERMINISTIC") */
        add_check(r, "blocking_job", ASX_DOCTOR_OK,
                  blocking_uses_worker_threads() ? "worker-thread job returned its result"
                                                 : "job ran inline (deterministic build)",
                  p->waiter->waits, 0);
    }
#else
    (void)p;
    add_check(r, "blocking_job", ASX_DOCTOR_SKIP, "no blocking pool in this profile", 0, 0);
#endif
}

static int probe_tasks_pending(const doctor_probe_ctx *p) {
    int pending = 0;
#if ASX_HAS_NATIVE_IO_DRIVER
    if (p->server != NULL && p->server->result == ASX_E_PENDING) pending = 1;
    if (p->client != NULL && p->client->result == ASX_E_PENDING) pending = 1;
#endif
#if ASX_HAS_BLOCKING_SURFACE
    if (p->waiter != NULL && p->waiter->result == ASX_E_PENDING) pending = 1;
#endif
    (void)p;
    return pending;
}

/* Cancel unfinished probe tasks, drain the region, join the tasks (which
 * frees their slots and captured state), close the listener. */
static void probe_cleanup(doctor_probe_ctx *p) {
    asx_cancel_reason reason;
    asx_budget budget;
    asx_status st;
    uint32_t i;

    if (p->have_region) {
        /* ASX_ANALYZER_WAIVER("config-dependent: nothing pends without IO driver or blocking") */
        if (probe_tasks_pending(p)) {
            reason =
                asx_cancel_reason_default(p->rt, ASX_CANCEL_SHUTDOWN, "asx doctor probe timed out");
            st = asx_region_cancel(p->region, &reason, NULL);
            (void)st;
        }
        budget = probe_budget(p->rt);
        st = asx_region_drain(p->region, &budget);
        (void)st;
        for (i = 0; i < p->tasks; i++) {
            st = asx_task_join(p->spawned[i], NULL);
            (void)st;
        }
    }
#if ASX_HAS_NATIVE_IO_DRIVER
    if (p->have_listener) (void)asx_tcp_listener_close(p->listener);
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

asx_status asx_doctor_run(const asx_runtime *rt, asx_doctor_report *report) {
    if (rt == NULL || report == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(report, 0, sizeof(*report));

    check_runtime_initialized(rt, report);
    check_region_utilization(rt, report);
    check_task_utilization(rt, report);
    check_obligation_utilization(rt, report);
    check_io_driver_state(rt, report);
    check_blocking_pool_state(rt, report);
    check_safety_profile(rt, report);
    check_containment_policy(rt, report);
    check_net_backend(report);

    return ASX_OK;
}

asx_status asx_doctor_probe(asx_runtime *rt, asx_doctor_report *report) {
    doctor_probe_ctx p;
    asx_budget budget;
    asx_status st;

    if (rt == NULL || report == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_runtime_is_initialized(rt)) return ASX_E_INVALID_STATE;
    if (report->check_count > ASX_DOCTOR_MAX_CHECKS - DOCTOR_PROBE_CHECKS) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    memset(&p, 0, sizeof(p));
    p.rt = rt;
    probe_clock(rt, report);
    probe_entropy(rt, report);
    p.have_region = asx_region_open(rt, &p.region) == ASX_OK;
    probe_socket_start(&p, report);
    probe_blocking_start(&p);

    /* The echo tasks park on their sockets and the blocking waiter on its
     * job: only the reactor and the pool's completion can wake them. The
     * run ends when they are done or at the probe deadline. */
    if (p.tasks > 0u) {
        budget = probe_budget(rt);
        st = asx_scheduler_run(p.region, &budget);
        (void)st; /* the verdicts are in the tasks' results */
    }
    probe_reactor_verdict(&p, report);
    probe_blocking_verdict(&p, report);
    probe_cleanup(&p);
    return ASX_OK;
}

int asx_doctor_is_healthy(const asx_doctor_report *report) {
    if (report == NULL) return 0;
    return report->fail_count == 0;
}

asx_doctor_severity asx_doctor_overall(const asx_doctor_report *report) {
    if (report == NULL) return ASX_DOCTOR_FAIL;
    if (report->fail_count > 0) return ASX_DOCTOR_FAIL;
    if (report->warn_count > 0) return ASX_DOCTOR_WARN;
    return ASX_DOCTOR_OK;
}
