/*
 * kernel_min.c -- the smallest program that uses the kernel: one region,
 * one task, one channel (make test-musl-qemu, bd-9kll.15.8).
 *
 * The task reserves a send permit on the channel and sends 42; main
 * receives it, drains the region and checks quiescence. It is linked
 * against the kernel-only archive alone (no conformance interpreter, CLI or
 * protocol models) with musl, statically, so the section sizes of the binary
 * are what an application using the kernel pays on the target, not what a
 * test harness does. It prints one fixed line, which must be the same under
 * qemu-user on every target.
 *
 * Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>

static asx_runtime g_rt;

typedef struct {
    asx_channel_id channel;
    asx_status result;
} sender_state;

/* One poll: reserve, send, done. */
static asx_status poll_sender(void *ud, asx_task_id self) {
    sender_state *s = (sender_state *)ud;
    asx_send_permit permit;
    (void)self;
    s->result = asx_channel_try_reserve(s->channel, &permit);
    if (s->result != ASX_OK) return s->result;
    s->result = asx_send_permit_send(&permit, 42u);
    return s->result;
}

static int fail(const char *what) {
    printf("kernel-min: FAIL %s\n", what);
    return 1;
}

int main(void) {
    asx_region_id region;
    asx_channel_id channel;
    asx_task_id task;
    asx_task_state state;
    asx_budget budget;
    sender_state *s = NULL;
    void *raw = NULL;
    uint64_t value = 0;

    asx_runtime_reset(&g_rt);
    asx_channel_reset();
    if (asx_region_open(&g_rt, &region) != ASX_OK) return fail("region_open");
    if (asx_channel_create(region, 4u, &channel) != ASX_OK) return fail("channel_create");
    if (asx_task_spawn_captured(region, poll_sender, (uint32_t)sizeof(sender_state), NULL, &task,
                                &raw) != ASX_OK) {
        return fail("task_spawn");
    }
    s = (sender_state *)raw;
    s->channel = channel;
    s->result = ASX_E_INVALID_STATE;
    budget = asx_budget_from_polls(16);
    (void)asx_scheduler_run(region, &budget);
    if (asx_task_get_state(task, &state) != ASX_OK || state != ASX_TASK_COMPLETED) {
        return fail("task_completed");
    }
    if (s->result != ASX_OK) return fail("task_sent");
    if (asx_channel_try_recv(channel, &value) != ASX_OK || value != 42u) return fail("recv_42");
    budget = asx_budget_from_polls(16);
    if (asx_region_drain(region, &budget) != ASX_OK) return fail("drain");
    if (asx_quiescence_check(region) != ASX_OK) return fail("quiescent");
    printf("kernel-min: region ok, task ok, channel ok (42), quiescent\n");
    return 0;
}
