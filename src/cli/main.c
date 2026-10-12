/*
 * main.c — asx CLI convenience tool
 *
 * Thin wrapper around libasx for quick diagnostics, profile inspection,
 * and version queries. Links against libasx.a.
 *
 * Subcommands:
 *   version   Print version and build info
 *   info      Print active profile, capabilities, resource limits
 *   doctor    Run runtime diagnostic checks
 *   help      Print usage
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/asx.h>
#include <asx/migration/migration.h>
#include <asx/runtime/profile_compat.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void print_usage(FILE *out) {
    fprintf(out, "asx — asupersync ANSI C runtime tool\n"
                 "\n"
                 "Usage: asx <command> [options]\n"
                 "\n"
                 "Commands:\n"
                 "  version          Print version and build configuration\n"
                 "  info             Print active profile and resource limits\n"
                 "  doctor           Run runtime diagnostic checks\n"
                 "  help             Print this help message\n"
                 "\n"
                 "Options:\n"
                 "  --format=json    Output in JSON format (doctor only; or --json)\n"
                 "  --format=text    Output as text (the default)\n"
                 "  -h, --help       Print help\n");
}

/* ------------------------------------------------------------------ */
/* version subcommand                                                  */
/* ------------------------------------------------------------------ */

static int cmd_version(void) {
    const char *ver = asx_version_str();
    asx_profile_id pid = asx_profile_active();
    const char *pname = asx_profile_name(pid);

    printf("asx %s (profile=%s codec=%s det=%d)\n", ver ? ver : "unknown",
           pname ? pname : "unknown",
#if defined(ASX_CODEC_BIN)
           "BIN",
#else
           "JSON",
#endif
#if ASX_DETERMINISTIC
           1
#else
           0
#endif
    );
    return 0;
}

/* ------------------------------------------------------------------ */
/* info subcommand                                                     */
/* ------------------------------------------------------------------ */

static int cmd_info(void) {
    asx_profile_id pid = asx_profile_active();
    const char *pname = asx_profile_name(pid);
    asx_profile_descriptor desc;
    asx_status st;

    printf("Profile:     %s (id=%d)\n", pname ? pname : "unknown", (int)pid);

#if defined(ASX_CODEC_BIN)
    printf("Codec:       BIN\n");
#else
    printf("Codec:       JSON\n");
#endif

#if ASX_DETERMINISTIC
    printf("Deterministic: yes\n");
#else
    printf("Deterministic: no\n");
#endif

    st = asx_profile_get_descriptor(pid, &desc);
    if (st == ASX_OK) {
        printf("Resource class: R%d\n", (int)desc.resource_class + 1);
        printf("Max regions:     %u\n", (unsigned)desc.max_regions);
        printf("Max tasks:       %u\n", (unsigned)desc.max_tasks);
        printf("Max timers:      %u\n", (unsigned)desc.max_timers);
        printf("Max obligations: %u\n", (unsigned)desc.max_obligations);
        printf("Trace capacity:  %u\n", (unsigned)desc.trace_capacity);
#if defined(ASX_DEBUG_GHOST) && ASX_DEBUG_GHOST
        printf("Ghost monitors:  yes\n");
#else
        printf("Ghost monitors:  no\n");
#endif
        printf("Alloc sealable:  %s\n", desc.allocator_sealable ? "yes" : "no");
    }

    printf("\nCompile flags:\n");
#if defined(ASX_DEBUG)
    printf("  ASX_DEBUG=%d\n", (int)ASX_DEBUG);
#endif
#if defined(ASX_DEBUG_GHOST)
    printf("  ASX_DEBUG_GHOST=%d\n", (int)ASX_DEBUG_GHOST);
#else
    printf("  ASX_DEBUG_GHOST=0\n");
#endif

    return 0;
}

/* ------------------------------------------------------------------ */
/* doctor subcommand                                                   */
/* ------------------------------------------------------------------ */

static int cmd_doctor(int json_output) {
    asx_runtime rt;
    asx_doctor_report report;
    asx_status st;
    uint32_t i;

    /* The build's default runtime: on a live POSIX build that is the real
     * platform (monotonic clock, OS entropy, epoll/poll reactor, worker
     * threads), which the probes then exercise. */
    st = asx_runtime_init_default(&rt);
    if (st != ASX_OK) {
        fprintf(stderr, "asx doctor: runtime init failed: %s\n", asx_status_str(st));
        return 1;
    }

    /* Run diagnostics */
    memset(&report, 0, sizeof(report));
    st = asx_doctor_run(&rt, &report);
    if (st == ASX_OK) st = asx_doctor_probe(&rt, &report);
    asx_runtime_shutdown(&rt);

    if (st != ASX_OK) {
        fprintf(stderr, "asx doctor: diagnostic failed: %s\n", asx_status_str(st));
        return 1;
    }

    if (json_output) {
        /* JSON output */
        printf("{\"checks\":[");
        for (i = 0; i < report.check_count; i++) {
            const asx_doctor_check_result *c = &report.checks[i];
            const char *sev = c->severity == ASX_DOCTOR_OK     ? "ok"
                              : c->severity == ASX_DOCTOR_WARN ? "warn"
                              : c->severity == ASX_DOCTOR_SKIP ? "skip"
                                                               : "fail";
            if (i > 0) printf(",");
            printf("{\"name\":\"%s\",\"severity\":\"%s\",\"message\":\"%s\","
                   "\"value\":%u,\"capacity\":%u}",
                   c->name ? c->name : "", sev, c->message ? c->message : "", (unsigned)c->value,
                   (unsigned)c->capacity);
        }
        printf("],\"pass\":%u,\"warn\":%u,\"fail\":%u,\"skip\":%u,\"healthy\":%s}\n",
               (unsigned)report.pass_count, (unsigned)report.warn_count,
               (unsigned)report.fail_count, (unsigned)report.skip_count,
               asx_doctor_is_healthy(&report) ? "true" : "false");
    } else {
        /* Text output */
        printf("asx doctor: %u checks (%u pass, %u warn, %u fail, %u skip)\n",
               (unsigned)report.check_count, (unsigned)report.pass_count,
               (unsigned)report.warn_count, (unsigned)report.fail_count,
               (unsigned)report.skip_count);
        for (i = 0; i < report.check_count; i++) {
            const asx_doctor_check_result *c = &report.checks[i];
            const char *icon = c->severity == ASX_DOCTOR_OK     ? "PASS"
                               : c->severity == ASX_DOCTOR_WARN ? "WARN"
                               : c->severity == ASX_DOCTOR_SKIP ? "SKIP"
                                                                : "FAIL";
            printf("  %s: %s", icon, c->name ? c->name : "?");
            if (c->message && c->message[0]) printf(" — %s", c->message);
            if (c->capacity > 0) printf(" (%u/%u)", (unsigned)c->value, (unsigned)c->capacity);
            printf("\n");
        }
        printf("\nOverall: %s\n", asx_doctor_is_healthy(&report) ? "HEALTHY" : "UNHEALTHY");
    }

    return asx_doctor_is_healthy(&report) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int json_fmt = 0;
    int i;

    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }

    /* Global flags; anything else, an unknown --format=<x> included, is an
     * error rather than silently ignored. */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format=json") == 0 || strcmp(argv[i], "--json") == 0) {
            json_fmt = 1;
        } else if (strcmp(argv[i], "--format=text") == 0) {
            json_fmt = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strncmp(argv[i], "--format=", 9) == 0) {
            fprintf(stderr, "asx: unknown format '%s' (text or json)\n", argv[i] + 9);
            return 1;
        } else {
            fprintf(stderr, "asx: unknown option '%s'\n\n", argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    if (strcmp(argv[1], "version") == 0) {
        return cmd_version();
    } else if (strcmp(argv[1], "info") == 0) {
        return cmd_info();
    } else if (strcmp(argv[1], "doctor") == 0) {
        return cmd_doctor(json_fmt);
    } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
               strcmp(argv[1], "-h") == 0) {
        print_usage(stdout);
        return 0;
    } else {
        fprintf(stderr, "asx: unknown command '%s'\n\n", argv[1]);
        print_usage(stderr);
        return 1;
    }
}
