/*
 * bm_main.c -- the entry of the bare-metal fixture proof (make
 * test-baremetal-qemu, bd-9kll.15.7).
 *
 * A freestanding image with no file system: the fixtures are const arrays
 * generated into bm_fixtures.c (tools/baremetal/embed_fixtures.sh), each one
 * an asx.fixture.v2 document. For every one this calls
 * asx_conformance_replay_line, the function `asx-conformance replay` prints
 * on the host for the same bytes and the wasm32 lane calls, and prints its
 * line through semihosting, followed by the count line the host prints. The
 * lines must equal the host's byte for byte (tools/baremetal/
 * compare_replay.sh); that is the proof the interpreter, the kernel and the
 * canonical digest give the same answer on a Cortex-M4 and a RISC-V 32 core.
 *
 * A line starting with "bm:" is this harness's own (the banner) and is not
 * compared. The exit status is the number of failed fixtures, capped.
 *
 * Linked into the proof image only, beside the library and the target's
 * startup file. Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bm_semihost.h"
#include "conformance/interpreter.h"

#include <stddef.h>
#include <stdio.h>

typedef struct {
    const unsigned char *data;
    size_t len;
} bm_fixture;

/* Generated: the embedded fixtures, in the order of the subset list. */
extern const bm_fixture g_bm_fixtures[];
extern const unsigned g_bm_fixture_count;

#ifndef BM_TARGET_NAME
#define BM_TARGET_NAME "?"
#endif

static char g_line[1024];

int main(void) {
    unsigned i;
    unsigned failed = 0;
    char banner[160];
    char tail[64];

    (void)snprintf(banner, sizeof(banner), "bm: asx bare-metal replay target=%s profile=%s fixtures=%u\n",
                   BM_TARGET_NAME, asx_profile_name(asx_profile_active()), g_bm_fixture_count);
    bm_puts(banner);
    for (i = 0; i < g_bm_fixture_count; i++) {
        if (asx_conformance_replay_line((const char *)g_bm_fixtures[i].data, g_bm_fixtures[i].len,
                                        g_line, sizeof(g_line)) != ASX_OK) {
            failed++;
        }
        bm_puts(g_line);
        bm_puts("\n");
    }
    (void)snprintf(tail, sizeof(tail), "replayed %u fixture(s), %u failed\n", g_bm_fixture_count,
                   failed);
    bm_puts(tail);
    bm_exit(failed == 0u ? 0 : (failed < 100u ? (int)failed : 100));
    return 0;
}
