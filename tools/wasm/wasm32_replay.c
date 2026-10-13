/*
 * wasm32_replay.c -- the entry of the wasm32 fixture-replay module
 * (make test-wasm32-replay, bd-9kll.15.4).
 *
 * The lane's modules are freestanding: no file system, no WASI. The host
 * (tools/wasm/replay.js) copies one fixture's bytes into the static input
 * buffer, calls asx_wasm32_replay(len) and reads the result line from the
 * output buffer. The line is asx_conformance_replay_line's, the function
 * `asx-conformance replay` prints on x86_64 for the same bytes; the two
 * outputs must be identical.
 *
 * Linked into the replay module only, beside the library and
 * tools/wasm/wasm32_libc.c. Not part of the library.
 *
 * SPDX-License-Identifier: MIT
 */

#include "conformance/interpreter.h"

#include <stddef.h>
#include <stdint.h>

#define ASX_WASM32_INPUT_CAP (1024u * 1024u)
#define ASX_WASM32_OUTPUT_CAP 4096u

static unsigned char g_input[ASX_WASM32_INPUT_CAP];
static char g_output[ASX_WASM32_OUTPUT_CAP];

unsigned char *asx_wasm32_input(void);
size_t asx_wasm32_input_capacity(void);
char *asx_wasm32_output(void);
size_t asx_wasm32_output_capacity(void);
int32_t asx_wasm32_replay(size_t len);

/* Where the host writes the fixture's bytes. */
unsigned char *asx_wasm32_input(void) { return g_input; }

size_t asx_wasm32_input_capacity(void) { return sizeof(g_input); }

/* Where the result line is, NUL-terminated, after a replay. */
char *asx_wasm32_output(void) { return g_output; }

size_t asx_wasm32_output_capacity(void) { return sizeof(g_output); }

/* Replay the fixture in the first len bytes of the input buffer. Returns 0
 * when the output holds a result line, or the asx_status of the failure the
 * output's "ERROR ..." line reports. A len over the input capacity is
 * ASX_E_INVALID_ARGUMENT with nothing run. */
int32_t asx_wasm32_replay(size_t len) {
    if (len > sizeof(g_input)) return (int32_t)ASX_E_INVALID_ARGUMENT;
    return (int32_t)asx_conformance_replay_line((const char *)g_input, len, g_output,
                                                sizeof(g_output));
}
