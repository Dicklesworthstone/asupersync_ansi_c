/*
 * report.c — structured report formatting and logging
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/app/report.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/rt.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Report buffer                                                       */
/* ------------------------------------------------------------------ */

void asx_report_buf_init(asx_report_buf *buf) {
    if (buf == NULL) return;
    memset(buf->data, 0, ASX_REPORT_BUF_SIZE);
    buf->len = 0;
}

void asx_report_buf_append(asx_report_buf *buf, const char *str) {
    uint32_t slen, avail;
    if (buf == NULL || str == NULL) return;

    if (buf->len >= ASX_REPORT_BUF_SIZE) {
        buf->len = ASX_REPORT_BUF_SIZE - 1u;
        buf->data[buf->len] = '\0';
    }

    slen = (uint32_t)strlen(str);
    avail = ASX_REPORT_BUF_SIZE - buf->len - 1; /* reserve NUL */
    if (slen > avail) slen = avail;
    if (slen == 0) return;

    memcpy(buf->data + buf->len, str, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
}

void asx_report_buf_append_u32(asx_report_buf *buf, uint32_t val) {
    char tmp[12];
    uint32_t i = 0;
    uint32_t v = val;

    if (buf == NULL) return;
    if (v == 0) {
        asx_report_buf_append(buf, "0");
        return;
    }

    while (v > 0 && i < 11) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    /* Reverse */
    {
        char rev[12];
        uint32_t j;
        for (j = 0; j < i; j++) { rev[j] = tmp[i - 1 - j]; }
        rev[i] = '\0';
        asx_report_buf_append(buf, rev);
    }
}

void asx_report_buf_append_hex64(asx_report_buf *buf, uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    int i;

    if (buf == NULL) return;

    for (i = 15; i >= 0; i--) {
        tmp[i] = hex[val & 0xf];
        val >>= 4;
    }
    tmp[16] = '\0';

    asx_report_buf_append(buf, "0x");
    asx_report_buf_append(buf, tmp);
}

const char *asx_report_buf_cstr(const asx_report_buf *buf) {
    if (buf == NULL) return "";
    return buf->data;
}

/* ------------------------------------------------------------------ */
/* Severity/level strings                                              */
/* ------------------------------------------------------------------ */

static const char *severity_str(asx_doctor_severity sev) {
    switch (sev) {
    case ASX_DOCTOR_OK: return "OK";
    case ASX_DOCTOR_WARN: return "WARN";
    case ASX_DOCTOR_FAIL: return "FAIL";
    }
    return "??";
}

static asx_status validate_doctor_report(const asx_doctor_report *report) {
    if (report == NULL) return ASX_E_INVALID_ARGUMENT;
    if (report->check_count > ASX_DOCTOR_MAX_CHECKS) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

static asx_status validate_evidence_sink(const asx_evidence_sink *sink) {
    if (sink == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sink->count > ASX_EVIDENCE_SINK_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

static const char *evidence_level_str(asx_evidence_level lev) {
    switch (lev) {
    case ASX_EVIDENCE_INFO: return "INFO";
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    }
    return "??";
}

static void append_json_escaped(asx_report_buf *buf, const char *str) {
    const unsigned char *p;

    if (buf == NULL) return;
    if (str == NULL) return;

    p = (const unsigned char *)str;
    while (*p != '\0') {
        switch (*p) {
        case '\"': asx_report_buf_append(buf, "\\\""); break;
        case '\\': asx_report_buf_append(buf, "\\\\"); break;
        case '\b': asx_report_buf_append(buf, "\\b"); break;
        case '\f': asx_report_buf_append(buf, "\\f"); break;
        case '\n': asx_report_buf_append(buf, "\\n"); break;
        case '\r': asx_report_buf_append(buf, "\\r"); break;
        case '\t': asx_report_buf_append(buf, "\\t"); break;
        default:
            if (*p < 0x20u) {
                static const char hex[] = "0123456789abcdef";
                char esc[7];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(*p >> 4) & 0x0fu];
                esc[5] = hex[*p & 0x0fu];
                esc[6] = '\0';
                asx_report_buf_append(buf, esc);
            } else {
                char ch[2];
                ch[0] = (char)*p;
                ch[1] = '\0';
                asx_report_buf_append(buf, ch);
            }
            break;
        }
        p++;
    }
}

/* ------------------------------------------------------------------ */
/* Doctor report rendering                                             */
/* ------------------------------------------------------------------ */

asx_status asx_report_doctor_text(const asx_doctor_report *report, asx_report_buf *out) {
    uint32_t i;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = validate_doctor_report(report);
    if (st != ASX_OK) return st;

    for (i = 0; i < report->check_count; i++) {
        const asx_doctor_check_result *c = &report->checks[i];
        asx_report_buf_append(out, "  [");
        asx_report_buf_append(out, severity_str(c->severity));
        asx_report_buf_append(out, "] ");
        asx_report_buf_append(out, c->name);
        asx_report_buf_append(out, ": ");
        asx_report_buf_append(out, c->message);
        asx_report_buf_append(out, " (");
        asx_report_buf_append_u32(out, c->value);
        asx_report_buf_append(out, "/");
        asx_report_buf_append_u32(out, c->capacity);
        asx_report_buf_append(out, ")\n");
    }

    asx_report_buf_append(out, "  ---\n  Overall: ");
    asx_report_buf_append(out, severity_str(asx_doctor_overall(report)));
    asx_report_buf_append(out, " (");
    asx_report_buf_append_u32(out, report->pass_count);
    asx_report_buf_append(out, " pass, ");
    asx_report_buf_append_u32(out, report->warn_count);
    asx_report_buf_append(out, " warn, ");
    asx_report_buf_append_u32(out, report->fail_count);
    asx_report_buf_append(out, " fail)\n");

    return ASX_OK;
}

asx_status asx_report_doctor_json(const asx_doctor_report *report, asx_report_buf *out) {
    uint32_t i;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = validate_doctor_report(report);
    if (st != ASX_OK) return st;

    asx_report_buf_append(out, "{\"checks\":[");

    for (i = 0; i < report->check_count; i++) {
        const asx_doctor_check_result *c = &report->checks[i];
        if (i > 0) asx_report_buf_append(out, ",");
        asx_report_buf_append(out, "{\"name\":\"");
        append_json_escaped(out, c->name);
        asx_report_buf_append(out, "\",\"severity\":\"");
        asx_report_buf_append(out, severity_str(c->severity));
        asx_report_buf_append(out, "\",\"message\":\"");
        append_json_escaped(out, c->message);
        asx_report_buf_append(out, "\",\"value\":");
        asx_report_buf_append_u32(out, c->value);
        asx_report_buf_append(out, ",\"capacity\":");
        asx_report_buf_append_u32(out, c->capacity);
        asx_report_buf_append(out, "}");
    }

    asx_report_buf_append(out, "],\"overall\":\"");
    asx_report_buf_append(out, severity_str(asx_doctor_overall(report)));
    asx_report_buf_append(out, "\",\"pass\":");
    asx_report_buf_append_u32(out, report->pass_count);
    asx_report_buf_append(out, ",\"warn\":");
    asx_report_buf_append_u32(out, report->warn_count);
    asx_report_buf_append(out, ",\"fail\":");
    asx_report_buf_append_u32(out, report->fail_count);
    asx_report_buf_append(out, "}");

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Evidence sink rendering                                             */
/* ------------------------------------------------------------------ */

asx_status asx_report_evidence_text(const asx_evidence_sink *sink, asx_report_buf *out) {
    uint32_t i;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = validate_evidence_sink(sink);
    if (st != ASX_OK) return st;

    for (i = 0; i < sink->count; i++) {
        const asx_evidence_entry *e = &sink->entries[i];
        asx_report_buf_append(out, "  [");
        asx_report_buf_append(out, evidence_level_str(e->level));
        asx_report_buf_append(out, "] ");
        asx_report_buf_append(out, e->source ? e->source : "?");
        asx_report_buf_append(out, " — ");
        asx_report_buf_append(out, e->message ? e->message : "");
        asx_report_buf_append(out, "\n");
    }

    asx_report_buf_append(out, "  ---\n  Verdict: ");
    asx_report_buf_append(out, evidence_level_str(asx_evidence_verdict(sink)));
    asx_report_buf_append(out, " (");
    asx_report_buf_append_u32(out, sink->pass_count);
    asx_report_buf_append(out, " pass, ");
    asx_report_buf_append_u32(out, sink->warn_count);
    asx_report_buf_append(out, " warn, ");
    asx_report_buf_append_u32(out, sink->fail_count);
    asx_report_buf_append(out, " fail, ");
    asx_report_buf_append_u32(out, sink->info_count);
    asx_report_buf_append(out, " info)\n");

    return ASX_OK;
}

asx_status asx_report_evidence_json(const asx_evidence_sink *sink, asx_report_buf *out) {
    uint32_t i;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = validate_evidence_sink(sink);
    if (st != ASX_OK) return st;

    asx_report_buf_append(out, "{\"entries\":[");

    for (i = 0; i < sink->count; i++) {
        const asx_evidence_entry *e = &sink->entries[i];
        if (i > 0) asx_report_buf_append(out, ",");
        asx_report_buf_append(out, "{\"source\":\"");
        append_json_escaped(out, e->source ? e->source : "");
        asx_report_buf_append(out, "\",\"level\":\"");
        asx_report_buf_append(out, evidence_level_str(e->level));
        asx_report_buf_append(out, "\",\"message\":\"");
        append_json_escaped(out, e->message ? e->message : "");
        asx_report_buf_append(out, "\",\"seq\":");
        asx_report_buf_append_u32(out, e->sequence);
        asx_report_buf_append(out, "}");
    }

    asx_report_buf_append(out, "],\"verdict\":\"");
    asx_report_buf_append(out, evidence_level_str(asx_evidence_verdict(sink)));
    asx_report_buf_append(out, "\",\"pass\":");
    asx_report_buf_append_u32(out, sink->pass_count);
    asx_report_buf_append(out, ",\"warn\":");
    asx_report_buf_append_u32(out, sink->warn_count);
    asx_report_buf_append(out, ",\"fail\":");
    asx_report_buf_append_u32(out, sink->fail_count);
    asx_report_buf_append(out, ",\"info\":");
    asx_report_buf_append_u32(out, sink->info_count);
    asx_report_buf_append(out, "}");

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Inspection report rendering                                         */
/* ------------------------------------------------------------------ */

asx_status asx_report_inspection_text(const asx_inspection_report *report, asx_report_buf *out) {
    if (report == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_append(out, "Runtime Inspection:\n");
    asx_report_buf_append(out, "  initialized: ");
    asx_report_buf_append(out, report->initialized ? "yes" : "no");
    asx_report_buf_append(out, "\n  poisoned:    ");
    asx_report_buf_append(out, report->any_poisoned ? "yes" : "no");

    asx_report_buf_append(out, "\n  regions:     ");
    asx_report_buf_append_u32(out, report->regions.active_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->regions.capacity);
    asx_report_buf_append(out, " (peak ");
    asx_report_buf_append_u32(out, report->regions.peak_count);
    asx_report_buf_append(out, ")");

    asx_report_buf_append(out, "\n  tasks:       ");
    asx_report_buf_append_u32(out, report->tasks.active_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->tasks.capacity);
    asx_report_buf_append(out, " (peak ");
    asx_report_buf_append_u32(out, report->tasks.peak_count);
    asx_report_buf_append(out, ")");

    asx_report_buf_append(out, "\n  obligations: ");
    asx_report_buf_append_u32(out, report->obligations.active_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->obligations.capacity);
    asx_report_buf_append(out, " (peak ");
    asx_report_buf_append_u32(out, report->obligations.peak_count);
    asx_report_buf_append(out, ")");

    asx_report_buf_append(out, "\n  io_driver:   ");
    asx_report_buf_append_u32(out, report->io_driver.active_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->io_driver.capacity);
    asx_report_buf_append(out, " (peak ");
    asx_report_buf_append_u32(out, report->io_driver.peak_count);
    asx_report_buf_append(out, ")");

    asx_report_buf_append(out, "\n  blocking:    ");
    asx_report_buf_append_u32(out, report->blocking.active_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->blocking.capacity);
    asx_report_buf_append(out, " (peak ");
    asx_report_buf_append_u32(out, report->blocking.peak_count);
    asx_report_buf_append(out, ")");

    asx_report_buf_append(out, "\n  trace:       ");
    asx_report_buf_append_u32(out, report->trace.event_count);
    asx_report_buf_append(out, "/");
    asx_report_buf_append_u32(out, report->trace.capacity);
    asx_report_buf_append(out, " digest=");
    asx_report_buf_append_hex64(out, report->trace.digest);

    asx_report_buf_append(out, "\n  ghosts:      ");
    asx_report_buf_append_u32(out, report->ghosts.violation_count);
    asx_report_buf_append(out, " violations");
    if (report->ghosts.overflowed) { asx_report_buf_append(out, " (overflowed)"); }

    asx_report_buf_append(out, "\n  telemetry:   ");
    asx_report_buf_append_u32(out, report->telemetry_emitted);
    asx_report_buf_append(out, " emitted, ");
    asx_report_buf_append_u32(out, report->telemetry_filtered);
    asx_report_buf_append(out, " filtered");

    asx_report_buf_append(out, "\n  event_log:   ");
    asx_report_buf_append_u32(out, report->event_log_count);
    asx_report_buf_append(out, " events, digest=");
    asx_report_buf_append_hex64(out, report->event_log_digest);
    asx_report_buf_append(out, "\n");

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Structured logging                                                  */
/* ------------------------------------------------------------------ */

static asx_log_fn g_log_fn;
static void *g_log_user_data;

void asx_log_set_callback(asx_log_fn fn, void *user_data) {
    g_log_fn = fn;
    g_log_user_data = user_data;
}

void asx_log_emit(asx_log_level level, const char *source, const char *message) {
    if (g_log_fn == NULL) return;
    g_log_fn(level, source, message, g_log_user_data);
}

const char *asx_log_level_str(asx_log_level level) {
    switch (level) {
    case ASX_LOG_DEBUG: return "DEBUG";
    case ASX_LOG_INFO: return "INFO";
    case ASX_LOG_WARN: return "WARN";
    case ASX_LOG_ERROR: return "ERROR";
    }
    return "??";
}

/* ------------------------------------------------------------------ */
/* Failure artifact                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_report_failure_artifact(const asx_runtime *rt, asx_report_buf *out) {
    asx_doctor_report doctor;
    asx_inspection_report inspection;
    asx_evidence_sink sink;
    asx_status st;

    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_append(out, "=== FAILURE ARTIFACT ===\n\n");

    /* Doctor report */
    st = asx_doctor_run(rt, &doctor);
    if (st == ASX_OK) {
        asx_report_buf_append(out, "Doctor Report:\n");
        st = asx_report_doctor_text(&doctor, out);
        if (st != ASX_OK) return st;
        asx_report_buf_append(out, "\n");
    }

    /* Inspection report */
    st = asx_inspect(rt, &inspection);
    if (st == ASX_OK) {
        st = asx_report_inspection_text(&inspection, out);
        if (st != ASX_OK) return st;
        asx_report_buf_append(out, "\n");
    }

    /* Evidence from inspection */
    asx_evidence_sink_init(&sink);
    st = asx_inspect_to_evidence(rt, &sink);
    if (st == ASX_OK) {
        asx_report_buf_append(out, "Evidence:\n");
        st = asx_report_evidence_text(&sink, out);
        if (st != ASX_OK) return st;
    }

    /* Diagnostic context if active */
    {
        const asx_diagnostic_ctx *ctx = asx_diagnostic_current();
        if (ctx != NULL) {
            asx_report_buf_append(out, "\nDiagnostic Context:\n");
            asx_report_buf_append(out, "  operation: ");
            asx_report_buf_append(out, ctx->operation ? ctx->operation : "?");
            asx_report_buf_append(out, "\n  depth: ");
            asx_report_buf_append_u32(out, ctx->depth);
            asx_report_buf_append(out, "\n");
        }
    }

    asx_report_buf_append(out, "\n=== END ARTIFACT ===\n");
    return ASX_OK;
}
