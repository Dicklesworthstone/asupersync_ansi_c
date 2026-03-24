/*
 * ex_pipe_io.c — Anonymous pipe and buffered I/O example
 *
 * Demonstrates pipe creation, buffered writing, and reading with
 * disconnection detection.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;
    asx_status st;

    printf("SCENARIO pipe_io_example\n");

    asx_pipe_reset();

    /* Create a pipe */
    st = asx_pipe_open(&rd, &wr);
    if (st != ASX_OK) {
        printf("SCENARIO pipe_open fail\n");
        return 1;
    }
    printf("SCENARIO pipe_open pass\n");

    /* Write through the pipe */
    src = asx_buf_from("Hello through the pipe!", 23);
    st = asx_pipe_poll_write(wr, &src, &written);
    printf("SCENARIO pipe_write %s (wrote %u bytes)\n", st == ASX_OK ? "pass" : "fail", written);

    /* Read from the pipe */
    asx_buf_mut_init(&dst);
    st = asx_pipe_poll_read(rd, &dst, &read_n);
    printf("SCENARIO pipe_read %s (read %u bytes)\n", st == ASX_OK ? "pass" : "fail", read_n);

    /* Verify data integrity */
    {
        int match = (read_n == 23 && memcmp(dst.data, "Hello through the pipe!", 23) == 0);
        printf("SCENARIO pipe_data_integrity %s\n", match ? "pass" : "fail");
    }

    /* Close writer and verify disconnection */
    asx_pipe_close_write(wr);
    asx_buf_mut_init(&dst);
    st = asx_pipe_poll_read(rd, &dst, &read_n);
    printf("SCENARIO pipe_disconnected %s\n", st == ASX_E_DISCONNECTED ? "pass" : "fail");

    asx_pipe_close_read(rd);
    printf("SCENARIO pipe_io_complete pass\n");

    return 0;
}
