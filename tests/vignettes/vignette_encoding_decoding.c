/*
 * vignette_encoding_decoding.c — logged encode/decode pipeline flow
 *
 * Demonstrates the full data path: raw data -> encoding pipeline ->
 * wire format -> decoding pipeline -> recovered frames, with progress
 * tracking and artifact summaries at each stage.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

static void print_hex(const uint8_t *data, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len && i < 32; i++) {
        printf("%02x", data[i]);
        if (i + 1 < len && i + 1 < 32) printf(" ");
    }
    if (len > 32) printf("...[%u more]", len - 32);
}

int main(void) {
    /* Encoding side */
    asx_encoding_pipeline enc;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut wire_buf;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_config enc_cfg;
    asx_encoding_stats enc_stats;
    asx_encoded_symbol sym;
    asx_status st;

    /* Decoding side */
    asx_decoding_pipeline dec;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_decoding_config dec_cfg;
    asx_decoding_progress dec_prog;
    asx_frame frame;

    const char *messages[] = {"hello world", "asx encoding pipeline", "deterministic replay"};
    uint32_t i;
    uint32_t msg_count = 3;

    printf("=== vignette: encoding/decoding pipeline ===\n\n");

    /* ---- Phase 1: Encode ---- */
    printf("[phase:encode] Encoding %u messages with length-delimited codec\n", msg_count);

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&wire_buf);
    asx_mem_write_init(&ws, &wire_buf);
    writer = asx_mem_write_adapter(&ws);

    asx_encoding_config_init(&enc_cfg);
    st = asx_encoding_pipeline_init(&enc, codec, writer, &enc_cfg);
    if (st != ASX_OK) {
        fprintf(stderr, "  ERROR: encoding pipeline init failed: %s\n", asx_status_str(st));
        return 1;
    }

    for (i = 0; i < msg_count; i++) {
        uint32_t len = (uint32_t)strlen(messages[i]);
        st = asx_encoding_pipeline_encode(&enc, messages[i], len, &sym);
        if (st != ASX_OK) {
            fprintf(stderr, "  ERROR: encode frame %u failed: %s\n", i, asx_status_str(st));
            return 1;
        }
        printf("  frame[%u]: input=%u bytes, wire=%u bytes, seq=%u\n", i, len, sym.len, sym.sequence);
    }

    asx_encoding_pipeline_stats(&enc, &enc_stats);
    printf("\n[artifact:encoding_stats]\n");
    printf("  frames_encoded=%u bytes_input=%u bytes_output=%u frames_rejected=%u\n",
           enc_stats.frames_encoded, enc_stats.bytes_input, enc_stats.bytes_output,
           enc_stats.frames_rejected);
    printf("  last_error=%s\n", asx_encoding_error_kind_str(asx_encoding_pipeline_last_error(&enc)));

    /* Show wire hex */
    printf("\n[artifact:wire_format] %u bytes on wire: ", asx_buf_mut_remaining(&wire_buf));
    print_hex(wire_buf.data + wire_buf.rd_pos, asx_buf_mut_remaining(&wire_buf));
    printf("\n");

    /* ---- Phase 2: Decode ---- */
    printf("\n[phase:decode] Decoding from wire buffer\n");

    asx_mem_read_init(&rs, &wire_buf);
    reader = asx_mem_read_adapter(&rs);

    asx_decoding_config_init(&dec_cfg);
    st = asx_decoding_pipeline_init(&dec, codec, reader, &dec_cfg);
    if (st != ASX_OK) {
        fprintf(stderr, "  ERROR: decoding pipeline init failed: %s\n", asx_status_str(st));
        return 1;
    }

    i = 0;
    while ((st = asx_decoding_pipeline_decode(&dec, &frame)) == ASX_OK) {
        printf("  frame[%u]: decoded %u bytes = \"", i, frame.len);
        fwrite(frame.data, 1, frame.len, stdout);
        printf("\"\n");

        /* Verify round-trip fidelity */
        if (i < msg_count) {
            uint32_t expected_len = (uint32_t)strlen(messages[i]);
            if (frame.len != expected_len || memcmp(frame.data, messages[i], expected_len) != 0) {
                fprintf(stderr, "  ERROR: round-trip mismatch at frame %u\n", i);
                return 1;
            }
        }
        i++;
    }

    if (st != ASX_E_NOT_FOUND) {
        fprintf(stderr, "  ERROR: unexpected decode termination: %s\n", asx_status_str(st));
        return 1;
    }

    asx_decoding_pipeline_progress(&dec, &dec_prog);
    printf("\n[artifact:decoding_progress]\n");
    printf("  frames_decoded=%u bytes_consumed=%u eof=%d complete=%d\n",
           dec_prog.frames_decoded, dec_prog.bytes_consumed, dec_prog.eof_reached,
           dec_prog.complete);
    printf("  last_error=%s\n",
           asx_decoding_error_kind_str(asx_decoding_pipeline_last_error(&dec)));

    /* ---- Summary ---- */
    printf("\n[verdict] encode=%u frames -> %u wire bytes -> decode=%u frames: %s\n",
           enc_stats.frames_encoded, enc_stats.bytes_output, dec_prog.frames_decoded,
           (enc_stats.frames_encoded == dec_prog.frames_decoded) ? "PASS" : "FAIL");

    printf("rerun_hint: rch exec -- make build/tests/vignettes/vignette_encoding_decoding\n");

    return (enc_stats.frames_encoded == dec_prog.frames_decoded) ? 0 : 1;
}
