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
#include <asx/cli/cli.h>
#include <asx/migration/migration.h>
#include <asx/runtime/profile_compat.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */

static void print_usage(void) {
    fprintf(stderr, "asx — asupersync ANSI C runtime tool\n"
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
                    "  --format=json    Output in JSON format (doctor only)\n"
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
        printf("Ghost monitors:  %s\n", desc.ghost_monitors ? "yes" : "no");
        printf("Alloc sealable:  %s\n", desc.allocator_sealable ? "yes" : "no");
    }

    printf("\nCompile flags:\n");
#if defined(ASX_DEBUG)
    printf("  ASX_DEBUG=1\n");
#endif
#if defined(ASX_DEBUG_GHOST)
    printf("  ASX_DEBUG_GHOST=1\n");
#endif

    return 0;
}

/* ------------------------------------------------------------------ */
/* doctor subcommand                                                   */
/* ------------------------------------------------------------------ */

static int cmd_doctor(int json_output) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_doctor_report report;
    asx_status st;
    uint32_t i;

    /* Initialize runtime for diagnostic */
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) {
        fprintf(stderr, "asx doctor: hooks init failed: %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) {
        fprintf(stderr, "asx doctor: runtime init failed: %s\n", asx_status_str(st));
        return 1;
    }

    /* Run diagnostics */
    memset(&report, 0, sizeof(report));
    st = asx_doctor_run(&rt, &report);
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
                                                               : "fail";
            if (i > 0) printf(",");
            printf("{\"name\":\"%s\",\"severity\":\"%s\",\"message\":\"%s\","
                   "\"value\":%u,\"capacity\":%u}",
                   c->name ? c->name : "", sev, c->message ? c->message : "", (unsigned)c->value,
                   (unsigned)c->capacity);
        }
        printf("],\"pass\":%u,\"warn\":%u,\"fail\":%u,\"healthy\":%s}\n",
               (unsigned)report.pass_count, (unsigned)report.warn_count,
               (unsigned)report.fail_count, asx_doctor_is_healthy(&report) ? "true" : "false");
    } else {
        /* Text output */
        printf("asx doctor: %u checks (%u pass, %u warn, %u fail)\n", (unsigned)report.check_count,
               (unsigned)report.pass_count, (unsigned)report.warn_count,
               (unsigned)report.fail_count);
        for (i = 0; i < report.check_count; i++) {
            const asx_doctor_check_result *c = &report.checks[i];
            const char *icon = c->severity == ASX_DOCTOR_OK     ? "PASS"
                               : c->severity == ASX_DOCTOR_WARN ? "WARN"
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
        print_usage();
        return 1;
    }

    /* Check for global flags */
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format=json") == 0 || strcmp(argv[i], "--json") == 0) {
            json_fmt = 1;
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
        print_usage();
        return 0;
    } else {
        fprintf(stderr, "asx: unknown command '%s'\n\n", argv[1]);
        print_usage();
        return 1;
    }
}
