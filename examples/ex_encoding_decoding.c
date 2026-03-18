/*
 * ex_encoding_decoding.c — Encoding/decoding pipeline walkthrough
 *
 * Demonstrates:
 *   1. Configuring encoding and decoding pipelines with codecs
 *   2. Encoding multiple frames through a length-delimited codec
 *   3. Decoding frames back with progress tracking
 *   4. Error handling: max frame limits, cancellation
 *   5. Statistics and artifact reporting
 *
 * Output: SCENARIO lines for smoke-test validation plus ARTIFACT lines
 * carrying pipeline statistics for log retention.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        printf("SCENARIO %s ...\n", (id));                                                         \
    } while (0)

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("  OK: %s\n", (msg));                                                           \
            g_pass++;                                                                              \
        } else {                                                                                   \
            printf("  FAIL: %s\n", (msg));                                                         \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ */
/* Scenario 1: Basic encode/decode round-trip                          */
/* ------------------------------------------------------------------ */

static void scenario_roundtrip(void) {
    asx_encoding_pipeline enc;
    asx_decoding_pipeline dec;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut wire;
    asx_mem_write_state ws;
    asx_mem_read_state rs;
    asx_write_adapter writer;
    asx_read_adapter reader;
    asx_encoded_symbol sym;
    asx_encoding_stats enc_stats;
    asx_decoding_progress dec_prog;
    asx_frame frame;
    asx_status st;

    SCENARIO_BEGIN("encode-decode-roundtrip");

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&wire);

    /* Encode */
    asx_mem_write_init(&ws, &wire);
    writer = asx_mem_write_adapter(&ws);
    IGNORE_RC(asx_encoding_pipeline_init(&enc, codec, writer, NULL));

    st = asx_encoding_pipeline_encode(&enc, "hello", 5, &sym);
    SCENARIO_CHECK(st == ASX_OK, "encode frame 1");

    st = asx_encoding_pipeline_encode(&enc, "world", 5, &sym);
    SCENARIO_CHECK(st == ASX_OK, "encode frame 2");

    asx_encoding_pipeline_stats(&enc, &enc_stats);
    SCENARIO_CHECK(enc_stats.frames_encoded == 2, "2 frames encoded");
    SCENARIO_CHECK(enc_stats.bytes_output == 18, "18 bytes on wire (4+5 + 4+5)");

    printf("ARTIFACT encoding_stats: frames=%u input=%u output=%u rejected=%u\n",
           enc_stats.frames_encoded, enc_stats.bytes_input, enc_stats.bytes_output,
           enc_stats.frames_rejected);

    /* Decode */
    asx_mem_read_init(&rs, &wire);
    reader = asx_mem_read_adapter(&rs);
    IGNORE_RC(asx_decoding_pipeline_init(&dec, codec, reader, NULL));

    st = asx_decoding_pipeline_decode(&dec, &frame);
    SCENARIO_CHECK(st == ASX_OK && frame.len == 5 && memcmp(frame.data, "hello", 5) == 0,
                   "decode frame 1 = 'hello'");

    st = asx_decoding_pipeline_decode(&dec, &frame);
    SCENARIO_CHECK(st == ASX_OK && frame.len == 5 && memcmp(frame.data, "world", 5) == 0,
                   "decode frame 2 = 'world'");

    st = asx_decoding_pipeline_decode(&dec, &frame);
    SCENARIO_CHECK(st == ASX_E_NOT_FOUND, "EOF after 2 frames");

    asx_decoding_pipeline_progress(&dec, &dec_prog);
    SCENARIO_CHECK(dec_prog.frames_decoded == 2, "2 frames decoded");
    SCENARIO_CHECK(dec_prog.complete != 0, "pipeline complete");

    printf("ARTIFACT decoding_progress: frames=%u bytes=%u eof=%d complete=%d\n",
           dec_prog.frames_decoded, dec_prog.bytes_consumed, dec_prog.eof_reached,
           dec_prog.complete);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: Max frame limit enforcement                             */
/* ------------------------------------------------------------------ */

static void scenario_max_frame(void) {
    asx_encoding_pipeline enc;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut wire;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_config cfg;
    asx_status st;

    SCENARIO_BEGIN("max-frame-limit");

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&wire);
    asx_mem_write_init(&ws, &wire);
    writer = asx_mem_write_adapter(&ws);

    asx_encoding_config_init(&cfg);
    cfg.max_frame_bytes = 10;

    IGNORE_RC(asx_encoding_pipeline_init(&enc, codec, writer, &cfg));

    st = asx_encoding_pipeline_encode(&enc, "short", 5, NULL);
    SCENARIO_CHECK(st == ASX_OK, "frame within limit accepted");

    st = asx_encoding_pipeline_encode(&enc, "this-is-too-long-for-limit", 26, NULL);
    SCENARIO_CHECK(st == ASX_E_RESOURCE_EXHAUSTED, "oversized frame rejected");
    SCENARIO_CHECK(asx_encoding_pipeline_last_error(&enc) == ASX_ENCODING_ERR_OVERFLOW,
                   "error classified as overflow");

    printf("ARTIFACT error_kind: %s\n", asx_encoding_error_kind_str(asx_encoding_pipeline_last_error(&enc)));
}

/* ------------------------------------------------------------------ */
/* Scenario 3: Pipeline cancellation                                   */
/* ------------------------------------------------------------------ */

static void scenario_cancellation(void) {
    asx_encoding_pipeline enc;
    asx_codec codec;
    asx_buf_mut wire;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_status st;

    SCENARIO_BEGIN("pipeline-cancellation");

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&wire);
    asx_mem_write_init(&ws, &wire);
    writer = asx_mem_write_adapter(&ws);

    IGNORE_RC(asx_encoding_pipeline_init(&enc, codec, writer, NULL));

    st = asx_encoding_pipeline_encode(&enc, "before", 6, NULL);
    SCENARIO_CHECK(st == ASX_OK, "encode before cancel");

    asx_encoding_pipeline_cancel(&enc);

    st = asx_encoding_pipeline_encode(&enc, "after", 5, NULL);
    SCENARIO_CHECK(st == ASX_E_CANCELLED, "encode after cancel rejected");

    asx_encoding_pipeline_reset(&enc);

    st = asx_encoding_pipeline_encode(&enc, "resumed", 7, NULL);
    SCENARIO_CHECK(st == ASX_OK, "encode after reset succeeds");
}

/* ------------------------------------------------------------------ */
/* Scenario 4: Lines codec with decoding pipeline                      */
/* ------------------------------------------------------------------ */

static void scenario_lines_codec(void) {
    asx_decoding_pipeline dec;
    asx_lines_codec lc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_status st;
    uint32_t count = 0;

    SCENARIO_BEGIN("lines-codec-decode");

    asx_lines_codec_init(&lc);
    codec = asx_lines_as_codec(&lc);
    asx_buf_mut_init(&source);
    IGNORE_RC(asx_buf_mut_put(&source, "alpha\nbeta\ngamma\n", 18));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);
    IGNORE_RC(asx_decoding_pipeline_init(&dec, codec, reader, NULL));

    while ((st = asx_decoding_pipeline_decode(&dec, &frame)) == ASX_OK) {
        printf("  line[%u]: \"", count);
        fwrite(frame.data, 1, frame.len, stdout);
        printf("\"\n");
        count++;
    }

    SCENARIO_CHECK(count == 3, "decoded 3 lines");
    SCENARIO_CHECK(st == ASX_E_NOT_FOUND, "terminated at EOF");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("=== ex_encoding_decoding ===\n\n");

    scenario_roundtrip();
    printf("\n");
    scenario_max_frame();
    printf("\n");
    scenario_cancellation();
    printf("\n");
    scenario_lines_codec();

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    printf("rerun: rch exec -- make build/tests/vignettes/vignette_encoding_decoding\n");
    return g_fail ? 1 : 0;
}
