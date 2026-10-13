/*
 * main.c — asx CLI convenience tool
 *
 * Thin wrapper around libasx for quick diagnostics, profile inspection,
 * and version queries. Links against libasx.a.
 *
 * Subcommands:
 *   version   Print version and build info
 *   info      Print active profile, capabilities, resource limits
 *   doctor    Run runtime diagnostic checks (--journal=<path> records the
 *             run's native boundary results to a journal file)
 *   replay    Validate a journal file and say whether a replay of it would
 *             be accepted by this build
 *   help      Print usage
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/asx.h>
#include <asx/cli/cli.h>
#include <asx/migration/migration.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/journal_replay.h>
#include <asx/runtime/profile_compat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ASX_PROFILE_POSIX)
#include <asx/platform/posix.h>
#endif

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
                 "  replay <journal> Validate a journal and report whether a replay of it\n"
                 "                   would be accepted (exit 0 accepted, 1 refused, 2 error)\n"
                 "  help             Print this help message\n"
                 "\n"
                 "Options:\n"
                 "  --format=json    Output in JSON format (doctor, replay; or --json)\n"
                 "  --format=text    Output as text (the default)\n"
                 "  --journal=<path> doctor: record the run's native boundary results\n"
                 "  --rust-rev=<s>   replay: the asupersync revision the journal must name\n"
                 "  --build=<s>      replay: the C build the journal must name\n"
                 "  -h, --help       Print help\n");
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    fprintf(out, "\n"
                 "Environment (a flag overrides it):\n"
                 "  ASX_FORMAT=json|text    Output format (default text)\n"
                 "  ASX_COLOR=always|never  Color the doctor verdicts (default never)\n");
#endif
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* The asx_cli_* helper library is compile-time hidden in the browser
 * profile (asx/cli/cli.h), where this tool keeps the plain behaviour. */

/* A failed status, reported on stderr. */
static void report_status(const char *context, asx_status st) {
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    asx_cli_report_error(context, st);
#else
    fprintf(stderr, "error: %s: %s\n", context, asx_status_str(st));
#endif
}

/* Print `word` on stdout in `fg`: the console styles it as ASX_COLOR asked
 * (main sets the mode) and adds nothing in the default, so the text is
 * byte-identical unless color was requested. */
static void put_word(asx_ansi_color fg, const char *word) {
    asx_report_buf buf;
    asx_report_buf_init(&buf);
    if (asx_console_colored(&buf, fg, word) == ASX_OK) {
        fputs(buf.data, stdout);
    } else {
        fputs(word, stdout);
    }
}

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
/* ctrl-C / SIGTERM: the first asks the running command to stop at its next
 * cancellation point, the second stops the process at once. */
static void on_interrupt(int sig) {
    /* ASX_ANALYZER_WAIVER("cli.c keeps the signal state in one volatile counter") */
    asx_cli_record_signal(sig);
    if (asx_cli_should_force_quit()) _Exit(130);
}
#endif

/* Print `s` as a JSON string body (quotes and backslashes escaped,
 * control bytes dropped). */
static void json_text(const char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar((int)c);
        } else if (c >= 0x20u) {
            putchar((int)c);
        }
    }
}

static const char *kind_name(unsigned kind) {
    switch (kind) {
    case ASX_JOURNAL_KIND_CLOCK: return "CLOCK";
    case ASX_JOURNAL_KIND_ENTROPY: return "ENTROPY";
    case ASX_JOURNAL_KIND_REACTOR_WAIT: return "REACTOR_WAIT";
    case ASX_JOURNAL_KIND_REACTOR_POLL: return "REACTOR_POLL";
    case ASX_JOURNAL_KIND_NET_ACCEPT: return "NET_ACCEPT";
    case ASX_JOURNAL_KIND_NET_CONNECT: return "NET_CONNECT";
    case ASX_JOURNAL_KIND_NET_READ: return "NET_READ";
    case ASX_JOURNAL_KIND_NET_WRITE: return "NET_WRITE";
    case ASX_JOURNAL_KIND_UDP_RECV: return "UDP_RECV";
    case ASX_JOURNAL_KIND_UDP_SEND: return "UDP_SEND";
    case ASX_JOURNAL_KIND_RESOLVE: return "RESOLVE";
    case ASX_JOURNAL_KIND_PROCESS_SPAWN: return "PROCESS_SPAWN";
    case ASX_JOURNAL_KIND_PROCESS_EXIT: return "PROCESS_EXIT";
    case ASX_JOURNAL_KIND_SIGNAL: return "SIGNAL";
    case ASX_JOURNAL_KIND_BLOCKING_DONE: return "BLOCKING_DONE";
    case ASX_JOURNAL_KIND_NET_LISTEN: return "NET_LISTEN";
    case ASX_JOURNAL_KIND_UDP_BIND: return "UDP_BIND";
    case ASX_JOURNAL_KIND_NET_CTL: return "NET_CTL";
    case ASX_JOURNAL_KIND_PROCESS_WRITE: return "PROCESS_WRITE";
    case ASX_JOURNAL_KIND_PROCESS_READ: return "PROCESS_READ";
    case ASX_JOURNAL_KIND_CHAIN: return "CHAIN";
    case ASX_JOURNAL_KIND_END: return "END";
    default: return "?";
    }
}

static const char *replay_error_name(asx_replay_error e) {
    switch (e) {
    case ASX_REPLAY_OK: return "ACCEPTED";
    case ASX_REPLAY_E_BUILD_MISMATCH: return "BUILD_MISMATCH";
    case ASX_REPLAY_E_INCOMPLETE: return "INCOMPLETE";
    case ASX_REPLAY_E_CORRUPT: return "CORRUPT";
    case ASX_REPLAY_E_UNSUPPORTED: return "UNSUPPORTED";
    case ASX_REPLAY_E_DIVERGED: return "DIVERGED";
    case ASX_REPLAY_E_EXHAUSTED: return "EXHAUSTED";
    case ASX_REPLAY_E_RUNTIME: return "RUNTIME";
    case ASX_REPLAY_E_CUT: return "CUT";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* replay subcommand                                                   */
/* ------------------------------------------------------------------ */

#define REPLAY_MAX_BYTES ((size_t)1 << 30)

/* Read the whole file into a malloc'd buffer. NULL, with *err set, on
 * failure. */
static uint8_t *read_file(const char *path, size_t *out_len, const char **err) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    size_t got;
    if (f == NULL) {
        *err = "cannot open the file";
        return NULL;
    }
    size = fseek(f, 0L, SEEK_END) == 0 ? ftell(f) : -1L;
    if (size < 0 || fseek(f, 0L, SEEK_SET) != 0) {
        (void)fclose(f);
        *err = "cannot size the file";
        return NULL;
    }
    if ((size_t)size > REPLAY_MAX_BYTES) {
        (void)fclose(f);
        *err = "file is too large";
        return NULL;
    }
    buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (buf == NULL) {
        (void)fclose(f);
        *err = "out of memory";
        return NULL;
    }
    got = fread(buf, 1u, (size_t)size, f);
    (void)fclose(f);
    if (got != (size_t)size) {
        free(buf);
        *err = "short read";
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

static int cmd_replay(const char *path, int json_output, const char *rust_rev, const char *build) {
    const char *err = "";
    size_t len = 0u;
    uint8_t *bytes = read_file(path, &len, &err);
    asx_journal_reader rd;
    asx_journal_record rec;
    asx_journal_read_result rr;
    asx_journal_read_result walk = ASX_JOURNAL_READ_BAD_HEADER;
    asx_replay r;
    asx_replay_report rep;
    asx_replay_identity id;
    asx_replay_error verdict;
    uint32_t counts[256];
    uint32_t k;
    int first;
    int has_prefix;

    if (bytes == NULL) {
        fprintf(stderr, "asx replay: %s: %s\n", path, err);
        return 2;
    }
    memset(counts, 0, sizeof(counts));
    id.rust_rev_hash = rust_rev != NULL ? asx_journal_text_hash(rust_rev) : 0u;
    id.build_hash = build != NULL ? asx_journal_text_hash(build) : 0u;

    /* The header and the kinds the stream holds: what is readable of it,
     * reported even when the stream is then refused. */
    memset(&rd, 0, sizeof(rd));
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    /* The walk is the long part of a large journal and its cancellation
     * point: the first interrupt ends the command there. */
    (void)signal(SIGINT, on_interrupt);
    (void)signal(SIGTERM, on_interrupt);
#endif
    rr = asx_journal_reader_init(&rd, bytes, len);
    if (rr == ASX_JOURNAL_READ_RECORD) {
        for (;;) {
            walk = asx_journal_reader_next(&rd, &rec);
            if (walk != ASX_JOURNAL_READ_RECORD) break;
            counts[rec.kind]++;
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
            if (asx_cli_is_cancelled()) break;
#endif
        }
    }
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    if (asx_cli_is_cancelled()) {
        free(bytes);
        fprintf(stderr, "asx replay: %s: interrupted\n", path);
        return 130;
    }
#endif
    /* A stream that did not reach a valid END (a run that was killed, a
     * damaged tail) still has a committed prefix: what verified through its
     * last chain. A recovery replay serves exactly that. */
    has_prefix = rr == ASX_JOURNAL_READ_RECORD && walk != ASX_JOURNAL_READ_END;
    verdict = asx_replay_open(&r, bytes, len, &id, &rep);

    if (json_output) {
        printf("{\"path\":\"");
        json_text(path);
        printf("\",\"bytes\":%lu,\"header\":", (unsigned long)len);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            printf("{\"flags\":%u,\"config_fingerprint\":\"0x%llx\",\"rust_rev_hash\":\"0x%llx\","
                   "\"build_hash\":\"0x%llx\"}",
                   (unsigned)rd.flags, (unsigned long long)rd.config_fingerprint,
                   (unsigned long long)rd.rust_rev_hash, (unsigned long long)rd.build_hash);
        } else {
            printf("null");
        }
        printf(",\"data_records\":%llu,\"chains\":%llu,\"kinds\":{",
               (unsigned long long)rd.data_records, (unsigned long long)rd.chains);
        first = 1;
        for (k = 0u; k < 256u; k++) {
            if (counts[k] == 0u) continue;
            printf("%s\"%s\":%u", first ? "" : ",", kind_name(k), (unsigned)counts[k]);
            first = 0;
        }
        printf("},\"prefix\":");
        if (has_prefix) {
            printf("{\"bytes\":%lu,\"data_records\":%llu,\"cut_seq\":%llu,\"reason\":\"%s\"}",
                   (unsigned long)rd.chain_end_pos, (unsigned long long)rd.chain_end_records,
                   (unsigned long long)rd.chain_end_seq,
                   walk == ASX_JOURNAL_READ_TRUNCATED ? "INCOMPLETE" : "CORRUPT");
        } else {
            printf("null");
        }
        printf(",\"accepted\":%s,\"verdict\":\"%s\"", verdict == ASX_REPLAY_OK ? "true" : "false",
               replay_error_name(verdict));
        if (verdict != ASX_REPLAY_OK) {
            printf(",\"seq\":%llu,\"first_seq\":%llu,\"kind\":\"%s\",\"what\":\"",
                   (unsigned long long)rep.seq, (unsigned long long)rep.first_seq,
                   kind_name(rep.expected_kind));
            json_text(rep.what != NULL ? rep.what : "");
            printf("\"");
        }
        printf("}\n");
    } else {
        printf("asx replay: %s (%lu bytes)\n", path, (unsigned long)len);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            printf("  header:  flags 0x%x  config 0x%llx  rust_rev 0x%llx  build 0x%llx\n",
                   (unsigned)rd.flags, (unsigned long long)rd.config_fingerprint,
                   (unsigned long long)rd.rust_rev_hash, (unsigned long long)rd.build_hash);
        } else {
            printf("  header:  not readable\n");
        }
        printf("  records: %llu data, %llu chains\n", (unsigned long long)rd.data_records,
               (unsigned long long)rd.chains);
        first = 1;
        for (k = 0u; k < 256u; k++) {
            if (counts[k] == 0u) continue;
            printf("%s %s %u", first ? "  kinds:  " : ",", kind_name(k), (unsigned)counts[k]);
            first = 0;
        }
        if (!first) printf("\n");
        if (has_prefix) {
            const char *why = walk == ASX_JOURNAL_READ_TRUNCATED
                                  ? "the stream ends there"
                                  : "a record or chain does not verify";
            printf("  prefix:  %llu data records in the first %lu bytes verify; the next record, "
                   "seq %llu, was not committed (%s)\n",
                   (unsigned long long)rd.chain_end_records, (unsigned long)rd.chain_end_pos,
                   (unsigned long long)rd.chain_end_seq, why);
        }
        if (verdict == ASX_REPLAY_OK) {
            printf("  verdict: ACCEPTED (identity, integrity and every record are served by "
                   "this build)\n");
        } else {
            printf("  verdict: REFUSED %s at seq %llu", replay_error_name(verdict),
                   (unsigned long long)rep.seq);
            if (verdict == ASX_REPLAY_E_CORRUPT) {
                printf(" (a break lies in seq %llu..%llu)", (unsigned long long)rep.first_seq,
                       (unsigned long long)rep.seq);
            }
            if (rep.expected_kind != 0u) printf(" (%s)", kind_name(rep.expected_kind));
            printf(": %s\n", rep.what != NULL ? rep.what : "");
        }
    }
    free(bytes);
    return verdict == ASX_REPLAY_OK ? 0 : 1;
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

/* The journal sink of `doctor --journal`: the stream goes to a file. */
static asx_status file_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    return fwrite(bytes, 1u, (size_t)len, (FILE *)ctx) == (size_t)len ? ASX_OK
                                                                      : ASX_E_RESOURCE_EXHAUSTED;
}

#define DOCTOR_JOURNAL_STAGE 65536u

/* The pinned asupersync revision this build targets, named in the journal
 * header so a replay can refuse a stream recorded against another. */
#define DOCTOR_RUST_REV "5e60b1c4c"

static int cmd_doctor(int json_output, const char *journal_path) {
    asx_runtime rt;
    asx_doctor_report report;
    asx_status st;
    uint32_t i;
    FILE *jf = NULL;
    asx_journal journal;
    static uint8_t stage[DOCTOR_JOURNAL_STAGE];
    int journaling = 0;

    if (journal_path != NULL) {
        asx_journal_config jcfg;
        asx_runtime_config cfg;
        asx_runtime_hooks hooks;

        jf = fopen(journal_path, "wb");
        if (jf == NULL) {
            fprintf(stderr, "asx doctor: cannot open journal '%s'\n", journal_path);
            return 2;
        }
        memset(&jcfg, 0, sizeof(jcfg));
        jcfg.sink_fn = file_sink;
        jcfg.sink_ctx = jf;
        jcfg.buffer = stage;
        jcfg.buffer_cap = DOCTOR_JOURNAL_STAGE;
        jcfg.config_fingerprint = ASX_CONFIG_FINGERPRINT;
        jcfg.rust_rev_hash = asx_journal_text_hash(DOCTOR_RUST_REV);
        jcfg.build_hash = asx_journal_text_hash(asx_version_str());
        /* The same table the default runtime is built over, wrapped so
         * every boundary result is recorded. */
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
        st = asx_posix_hooks_install(&hooks);
#else
        st = asx_runtime_hooks_init(&hooks);
#endif
        if (st == ASX_OK) st = asx_runtime_config_init(&cfg);
        if (st == ASX_OK) st = asx_journal_init(&journal, &jcfg);
        if (st == ASX_OK) st = asx_journal_wrap_hooks(&journal, &hooks);
        if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
        if (st != ASX_OK) {
            asx_journal_uninstall();
            (void)fclose(jf);
            report_status("asx doctor: journal setup failed", st);
            return 1;
        }
        journaling = 1;
    } else {
        /* The build's default runtime: on a live POSIX build that is the
         * real platform (monotonic clock, OS entropy, epoll/poll reactor,
         * worker threads), which the probes then exercise. */
        st = asx_runtime_init_default(&rt);
        if (st != ASX_OK) {
            report_status("asx doctor: runtime init failed", st);
            return 1;
        }
    }

    /* Run diagnostics */
    memset(&report, 0, sizeof(report));
    st = asx_doctor_run(&rt, &report);
    if (st == ASX_OK) st = asx_doctor_probe(&rt, &report);
    asx_runtime_shutdown(&rt);

    if (journaling) {
        asx_status js;
        asx_journal_uninstall();
        js = asx_journal_finish(&journal);
        if (fclose(jf) != 0 && js == ASX_OK) js = ASX_E_RESOURCE_EXHAUSTED;
        if (js != ASX_OK) {
            char what[512];
            (void)snprintf(what, sizeof(what), "asx doctor: journal '%s' is incomplete",
                           journal_path);
            report_status(what, js);
            return 1;
        }
    }

    if (st != ASX_OK) {
        report_status("asx doctor: diagnostic failed", st);
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
            asx_ansi_color tint = c->severity == ASX_DOCTOR_OK     ? ASX_COLOR_GREEN
                                  : c->severity == ASX_DOCTOR_WARN ? ASX_COLOR_YELLOW
                                  : c->severity == ASX_DOCTOR_SKIP ? ASX_COLOR_CYAN
                                                                   : ASX_COLOR_RED;
            printf("  ");
            put_word(tint, icon);
            printf(": %s", c->name ? c->name : "?");
            if (c->message && c->message[0]) printf(" — %s", c->message);
            if (c->capacity > 0) printf(" (%u/%u)", (unsigned)c->value, (unsigned)c->capacity);
            printf("\n");
        }
        printf("\nOverall: ");
        if (asx_doctor_is_healthy(&report)) {
            put_word(ASX_COLOR_GREEN, "HEALTHY");
        } else {
            put_word(ASX_COLOR_RED, "UNHEALTHY");
        }
        printf("\n");
    }

    return asx_doctor_is_healthy(&report) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    int json_fmt = 0;
    int i;
    const char *journal_path = NULL;
    const char *replay_path = NULL;
    const char *rust_rev = NULL;
    const char *build = NULL;

    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    {
        /* ASX_FORMAT and ASX_COLOR set the defaults; the flags override them. */
        asx_cli_config cfg;
        const char *given = getenv("ASX_FORMAT");
        asx_cli_config_from_env(&cfg);
        json_fmt = cfg.output_format != ASX_OUTPUT_TEXT;
        asx_console_set_color_mode(cfg.color_mode);
        if (given != NULL && strcmp(asx_output_format_str(cfg.output_format), given) != 0) {
            asx_cli_report_warning("ASX_FORMAT is neither text nor json; using text");
        }
    }
#endif

    /* Flags; anything else, an unknown --format=<x> included, is an error
     * rather than silently ignored. `replay` takes one file operand. */
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
        } else if (strncmp(argv[i], "--journal=", 10) == 0 && argv[i][10] != '\0') {
            journal_path = argv[i] + 10;
        } else if (strncmp(argv[i], "--rust-rev=", 11) == 0) {
            rust_rev = argv[i] + 11;
        } else if (strncmp(argv[i], "--build=", 8) == 0) {
            build = argv[i] + 8;
        } else if (argv[i][0] != '-' && strcmp(argv[1], "replay") == 0 && replay_path == NULL) {
            replay_path = argv[i];
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
        return cmd_doctor(json_fmt, journal_path);
    } else if (strcmp(argv[1], "replay") == 0) {
        if (replay_path == NULL) {
            fprintf(stderr, "asx replay: a journal file is required\n\n");
            print_usage(stderr);
            return 2;
        }
        return cmd_replay(replay_path, json_fmt, rust_rev, build);
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
