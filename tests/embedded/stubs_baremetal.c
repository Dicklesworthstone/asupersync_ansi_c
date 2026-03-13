/*
 * stubs_baremetal.c — Stubs for not-yet-implemented subsystems
 *
 * Provides no-op implementations of reset/init functions referenced
 * by the runtime lifecycle but not yet implemented in the library.
 * These allow bare-metal linking without pulling in unfinished modules.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

/* Reset stubs for subsystems not yet implemented */
void asx_oneshot_reset(void) {}
void asx_watch_reset(void) {}
void asx_broadcast_reset(void) {}
void asx_session_reset(void) {}
void asx_waker_reset(void) {}
void asx_io_driver_reset(void) {}
void asx_blocking_pool_reset(void) {}
void asx_notify_reset(void) {}
void asx_semaphore_reset(void) {}
void asx_barrier_reset(void) {}
void asx_once_reset(void) {}
void asx_actor_reset(void) {}
void asx_supervisor_reset(void) {}
void asx_net_reset(void) {}
void asx_diagnostic_reset(void) {}
void asx_adapter_reset_all(void) {}

/* Channel stubs (not linked in FREESTANDING archive) */
void asx_channel_reset(void) {}
int asx_broadcast_try_recv(void) { return -1; }
int asx_channel_try_recv(void) { return -1; }
int asx_watch_has_changed(void) { return 0; }
int asx_watch_recv(void) { return -1; }

/* Buf stubs */
size_t asx_buf_mut_writable(void *buf) {
    (void)buf;
    return 0;
}
asx_status asx_buf_mut_put(void *buf, const void *data, size_t len) {
    (void)buf;
    (void)data;
    (void)len;
    return ASX_E_INVALID_STATE;
}

/* Sbrk stub for newlib heap */
void *_sbrk(int incr);

static char g_heap[8192];
static char *g_heap_ptr = g_heap;

void *_sbrk(int incr) {
    char *prev = g_heap_ptr;
    if (g_heap_ptr + incr > g_heap + sizeof(g_heap)) { return (void *)-1; }
    g_heap_ptr += incr;
    return prev;
}
