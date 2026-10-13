/*
 * wasm32_libc.c -- the C library symbols the freestanding wasm32 lane links.
 *
 * clang --target=wasm32 -ffreestanding has no sysroot, no libc and no
 * compiler-rt (worker B ships clang and wasm-ld only). The library compiled
 * for this lane references a small, measured set of C library functions;
 * the shim headers beside this file (tools/wasm/include) declare exactly
 * those and this file defines exactly those. It is not a libc:
 *
 *   - nothing the library does not call is here, so a new reference is a
 *     link error (wasm-ld runs without --allow-undefined), not a silent
 *     stub;
 *   - a printf conversion, flag or length the library does not use is
 *     rejected (snprintf returns -1), and a scanf directive it does not use
 *     ends the scan, so a new use cannot be quietly misformatted;
 *   - getenv has no environment to read and says so.
 *
 * Memory: malloc, realloc and free are the existing static arena
 * (asx_static_arena_*, src/runtime/hooks.c) over one static buffer, the
 * allocator a FREESTANDING deployment binds with asx_static_arena_bind_hooks.
 * It never returns memory (free only validates the pointer). A host that
 * runs real workloads binds its own arena through the runtime hook table;
 * this one serves the few library callers that reach malloc directly (the
 * default hook table, the config text parser).
 *
 * __multi3 is a compiler builtin, not libc: LLVM lowers the library's
 * saturating 64-bit multiply (x > UINT64_MAX / m ? UINT64_MAX : x * m) to an
 * overflow-checked 64x64 multiply, which wasm32 has no instruction for.
 *
 * Compile with -fno-builtin: the loops below are exactly the shape LLVM
 * turns back into calls to memcpy, memset and memmove, which here would be
 * calls to themselves.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Compiler builtin                                                    */
/* ------------------------------------------------------------------ */

__extension__ typedef __int128 asx_wasm32_i128;
__extension__ typedef unsigned __int128 asx_wasm32_u128;

asx_wasm32_i128 __multi3(asx_wasm32_i128 a, asx_wasm32_i128 b);

/* Low 128 bits of a * b, from 32-bit limbs so no step is itself a 128-bit
 * multiply. */
asx_wasm32_i128 __multi3(asx_wasm32_i128 a, asx_wasm32_i128 b) {
    uint64_t alo = (uint64_t)a;
    uint64_t ahi = (uint64_t)(a >> 64);
    uint64_t blo = (uint64_t)b;
    uint64_t bhi = (uint64_t)(b >> 64);
    uint64_t a0 = alo & 0xffffffffu;
    uint64_t a1 = alo >> 32;
    uint64_t b0 = blo & 0xffffffffu;
    uint64_t b1 = blo >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    uint64_t lo = (p00 & 0xffffffffu) | (mid << 32);
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) + alo * bhi + ahi * blo;

    return (asx_wasm32_i128)(((asx_wasm32_u128)hi << 64) | lo);
}

/* ------------------------------------------------------------------ */
/* <string.h>                                                          */
/* ------------------------------------------------------------------ */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;

    for (i = 0u; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;

    if (d == s) return dst;
    if ((uintptr_t)d < (uintptr_t)s) {
        for (i = 0u; i < n; i++) d[i] = s[i];
    } else {
        for (i = n; i > 0u; i--) d[i - 1u] = s[i - 1u];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    size_t i;

    for (i = 0u; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i;

    for (i = 0u; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i;

    for (i = 0u; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    }
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0u;

    while (s[n] != '\0') n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (*x != '\0' && *x == *y) {
        x++;
        y++;
    }
    return (int)*x - (int)*y;
}

int strncmp(const char *a, const char *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i;

    for (i = 0u; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
        if (x[i] == '\0') return 0;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    char want = (char)c;

    for (;; s++) {
        if (*s == want) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char want = (char)c;
    const char *last = NULL;

    for (;; s++) {
        if (*s == want) last = s;
        if (*s == '\0') return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);

    if (n == 0u) return (char *)hay;
    for (; *hay != '\0'; hay++) {
        if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char *)hay;
    }
    return NULL;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0u;

    while (s[n] != '\0' && strchr(reject, s[n]) == NULL) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s != '\0'; s++) {
        if (strchr(accept, *s) != NULL) return (char *)s;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* <ctype.h>, C locale                                                 */
/* ------------------------------------------------------------------ */

int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* ------------------------------------------------------------------ */
/* <errno.h>, <stdlib.h> environment and number parsing                */
/* ------------------------------------------------------------------ */

int errno;

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* The magnitude of the integer at s, as strtoull reads it: optional white
 * space and sign, then digits in base 2..36 (0 picks 8, 10 or 16 from the
 * prefix; 16 accepts 0x). *endp is s when no digits were read. *range is set
 * when the magnitude does not fit 64 bits (the value is then ULLONG_MAX). */
static unsigned long long scan_magnitude(const char *s, int base, const char **endp, int *neg,
                                         int *range) {
    const char *p = s;
    unsigned long long v = 0u;
    unsigned long long limit = (unsigned long long)-1;
    int any = 0;

    *neg = 0;
    *range = 0;
    while (is_space((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') {
        *neg = *p == '-';
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit_value((unsigned char)p[2]) >= 0 && digit_value((unsigned char)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    for (;; p++) {
        int d = digit_value((unsigned char)*p);
        unsigned long long ub = (unsigned long long)base;

        if (d < 0 || d >= base) break;
        any = 1;
        if (v > (limit - (unsigned long long)d) / ub) {
            *range = 1;
        } else {
            v = v * ub + (unsigned long long)d;
        }
    }
    *endp = any ? p : s;
    return *range ? limit : v;
}

unsigned long long strtoull(const char *s, char **endp, int base) {
    const char *end;
    int neg;
    int range;
    unsigned long long v = scan_magnitude(s, base, &end, &neg, &range);

    if (endp != NULL) *endp = (char *)end;
    if (range) {
        errno = ERANGE;
        return (unsigned long long)-1;
    }
    return neg ? 0u - v : v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    const char *end;
    int neg;
    int range;
    unsigned long long v = scan_magnitude(s, base, &end, &neg, &range);
    unsigned long max = (unsigned long)-1;

    if (endp != NULL) *endp = (char *)end;
    if (range || v > (unsigned long long)max) {
        errno = ERANGE;
        return max;
    }
    return neg ? 0ul - (unsigned long)v : (unsigned long)v;
}

/* ------------------------------------------------------------------ */
/* <stdlib.h> allocation: the static arena                             */
/* ------------------------------------------------------------------ */

#ifndef ASX_WASM32_HEAP_BYTES
#define ASX_WASM32_HEAP_BYTES 524288u
#endif

static uint64_t g_heap[ASX_WASM32_HEAP_BYTES / sizeof(uint64_t)];
static asx_static_arena g_heap_arena;
static asx_runtime_hooks g_heap_hooks;

/* The arena is bound on first use: 1 ready, -1 the bind failed. */
static int heap_ready(void) {
    static int state;

    if (state == 0) {
        asx_static_arena_config cfg;

        state = -1;
        if (asx_static_arena_config_init(&cfg, g_heap, sizeof(g_heap), ASX_CLASS_R1) == ASX_OK &&
            asx_static_arena_init(&g_heap_arena, &cfg) == ASX_OK &&
            asx_static_arena_bind_hooks(&g_heap_arena, &g_heap_hooks) == ASX_OK) {
            state = 1;
        }
    }
    return state > 0;
}

void *malloc(size_t n) {
    if (!heap_ready()) return NULL;
    return g_heap_hooks.allocator.malloc_fn(g_heap_hooks.allocator.ctx, n);
}

void *realloc(void *p, size_t n) {
    if (!heap_ready()) return NULL;
    return g_heap_hooks.allocator.realloc_fn(g_heap_hooks.allocator.ctx, p, n);
}

void free(void *p) {
    if (p == NULL || !heap_ready()) return;
    g_heap_hooks.allocator.free_fn(g_heap_hooks.allocator.ctx, p);
}

/* ------------------------------------------------------------------ */
/* <stdio.h>: snprintf                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap; /* bytes of buf, terminator included; 0 counts only */
    size_t len; /* characters the complete output has so far */
} fmt_sink;

static void sink_putc(fmt_sink *k, char c) {
    if (k->cap > 0u && k->len + 1u < k->cap) k->buf[k->len] = c;
    k->len++;
}

static void sink_pad(fmt_sink *k, char c, size_t n) {
    size_t i;

    for (i = 0u; i < n; i++) sink_putc(k, c);
}

typedef enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z } fmt_len;

static void emit_uint(fmt_sink *k, uint64_t v, unsigned base, int upper, int neg, int zero_pad,
                      size_t width) {
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[24];
    size_t nd = 0u;
    size_t total;

    do {
        digits[nd++] = set[v % base];
        v /= base;
    } while (v != 0u);
    total = nd + (neg ? 1u : 0u);
    if (zero_pad) {
        if (neg) sink_putc(k, '-');
        if (width > total) sink_pad(k, '0', width - total);
    } else {
        if (width > total) sink_pad(k, ' ', width - total);
        if (neg) sink_putc(k, '-');
    }
    while (nd > 0u) sink_putc(k, digits[--nd]);
}

/* %[0][width][.prec|.*][hh|h|l|ll|z]{% c s d i u x X}. Everything else is
 * refused: -1. */
static int format_into(fmt_sink *k, const char *fmt, va_list ap) {
    const char *p = fmt;

    while (*p != '\0') {
        int zero_pad = 0;
        int has_prec = 0;
        size_t width = 0u;
        size_t prec = 0u;
        fmt_len len = LEN_NONE;
        char conv;

        if (*p != '%') {
            sink_putc(k, *p++);
            continue;
        }
        p++;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10u + (size_t)(*p - '0');
            p++;
        }
        if (*p == '.') {
            p++;
            has_prec = 1;
            if (*p == '*') {
                int pv = va_arg(ap, int);

                if (pv < 0) {
                    has_prec = 0;
                } else {
                    prec = (size_t)pv;
                }
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10u + (size_t)(*p - '0');
                    p++;
                }
            }
        }
        if (*p == 'h') {
            len = LEN_H;
            p++;
            if (*p == 'h') {
                len = LEN_HH;
                p++;
            }
        } else if (*p == 'l') {
            len = LEN_L;
            p++;
            if (*p == 'l') {
                len = LEN_LL;
                p++;
            }
        } else if (*p == 'z') {
            len = LEN_Z;
            p++;
        }
        conv = *p;
        if (conv == '\0') return -1;
        p++;

        switch (conv) {
        case '%': sink_putc(k, '%'); break;
        case 'c':
            if (width > 1u) sink_pad(k, ' ', width - 1u);
            sink_putc(k, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = 0u;

            if (s == NULL) s = "(null)";
            while (s[n] != '\0' && (!has_prec || n < prec)) n++;
            if (width > n) sink_pad(k, ' ', width - n);
            while (n > 0u) {
                sink_putc(k, *s++);
                n--;
            }
            break;
        }
        case 'd':
        case 'i': {
            long long sv;

            if (has_prec) return -1;
            switch (len) {
            case LEN_LL: sv = va_arg(ap, long long); break;
            case LEN_L: sv = va_arg(ap, long); break;
            case LEN_H: sv = (short)va_arg(ap, int); break;
            case LEN_HH: sv = (signed char)va_arg(ap, int); break;
            case LEN_NONE: sv = va_arg(ap, int); break;
            case LEN_Z: return -1;
            }
            emit_uint(k, sv < 0 ? 0u - (uint64_t)sv : (uint64_t)sv, 10u, 0, sv < 0, zero_pad,
                      width);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long uv;

            if (has_prec) return -1;
            switch (len) {
            case LEN_LL: uv = va_arg(ap, unsigned long long); break;
            case LEN_L: uv = va_arg(ap, unsigned long); break;
            case LEN_Z: uv = va_arg(ap, size_t); break;
            case LEN_H: uv = (unsigned short)va_arg(ap, unsigned int); break;
            case LEN_HH: uv = (unsigned char)va_arg(ap, unsigned int); break;
            case LEN_NONE: uv = va_arg(ap, unsigned int); break;
            }
            emit_uint(k, uv, conv == 'u' ? 10u : 16u, conv == 'X', 0, zero_pad, width);
            break;
        }
        default: return -1;
        }
    }
    return 0;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    fmt_sink k;
    va_list ap;
    int rc;

    k.buf = buf;
    k.cap = buf == NULL ? 0u : cap;
    k.len = 0u;
    va_start(ap, fmt);
    rc = format_into(&k, fmt, ap);
    va_end(ap);
    if (k.cap > 0u) buf[k.len < k.cap - 1u ? k.len : k.cap - 1u] = '\0';
    if (rc != 0 || k.len > 0x7fffffffu) return -1;
    return (int)k.len;
}

/* ------------------------------------------------------------------ */
/* <stdio.h>: sscanf                                                   */
/* ------------------------------------------------------------------ */

#define SCAN_OK 1
#define SCAN_MISMATCH 0
#define SCAN_END (-1)

/* One %[width][hh|h|l|ll]{s u d x} directive; *f is just past the '%'.
 * Returns SCAN_OK after an assignment, SCAN_END when the input ran out
 * before the conversion, SCAN_MISMATCH otherwise (a directive the library
 * does not use is a mismatch). */
static int scan_directive(const char **in, const char **f, va_list *ap) {
    size_t width = 0u;
    int has_width = 0;
    fmt_len len = LEN_NONE;
    char conv;

    while (**f >= '0' && **f <= '9') {
        width = width * 10u + (size_t)(**f - '0');
        has_width = 1;
        (*f)++;
    }
    if (**f == 'h') {
        len = LEN_H;
        (*f)++;
        if (**f == 'h') {
            len = LEN_HH;
            (*f)++;
        }
    } else if (**f == 'l') {
        len = LEN_L;
        (*f)++;
        if (**f == 'l') {
            len = LEN_LL;
            (*f)++;
        }
    }
    conv = **f;
    if (conv == '\0') return SCAN_MISMATCH;
    (*f)++;

    if (conv == '%') {
        while (is_space((unsigned char)**in)) (*in)++;
        if (**in == '\0') return SCAN_END;
        if (**in != '%') return SCAN_MISMATCH;
        (*in)++;
        return SCAN_OK;
    }
    while (is_space((unsigned char)**in)) (*in)++;
    if (**in == '\0') return SCAN_END;

    if (conv == 's') {
        char *out = va_arg(*ap, char *);
        size_t n = 0u;

        while (**in != '\0' && !is_space((unsigned char)**in) && (!has_width || n < width)) {
            out[n++] = **in;
            (*in)++;
        }
        out[n] = '\0';
        return SCAN_OK;
    }
    if (conv == 'u' || conv == 'd' || conv == 'x') {
        const char *end;
        int neg;
        int range;
        unsigned long long mag;

        if (has_width) return SCAN_MISMATCH;
        mag = scan_magnitude(*in, conv == 'x' ? 16 : 10, &end, &neg, &range);
        if (end == *in) return SCAN_MISMATCH;
        *in = end;
        if (neg) mag = 0u - mag;
        switch (len) {
        case LEN_LL:
            if (conv == 'd') {
                *va_arg(*ap, long long *) = (long long)mag;
            } else {
                *va_arg(*ap, unsigned long long *) = mag;
            }
            break;
        case LEN_L:
            if (conv == 'd') {
                *va_arg(*ap, long *) = (long)mag;
            } else {
                *va_arg(*ap, unsigned long *) = (unsigned long)mag;
            }
            break;
        case LEN_H:
            if (conv == 'd') {
                *va_arg(*ap, short *) = (short)mag;
            } else {
                *va_arg(*ap, unsigned short *) = (unsigned short)mag;
            }
            break;
        case LEN_HH:
            if (conv == 'd') {
                *va_arg(*ap, signed char *) = (signed char)mag;
            } else {
                *va_arg(*ap, unsigned char *) = (unsigned char)mag;
            }
            break;
        case LEN_NONE:
            if (conv == 'd') {
                *va_arg(*ap, int *) = (int)mag;
            } else {
                *va_arg(*ap, unsigned int *) = (unsigned int)mag;
            }
            break;
        case LEN_Z: return SCAN_MISMATCH;
        }
        return SCAN_OK;
    }
    return SCAN_MISMATCH;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    const char *in = str;
    const char *f = fmt;
    int assigned = 0;
    int status = SCAN_OK;

    va_start(ap, fmt);
    while (status == SCAN_OK && *f != '\0') {
        if (is_space((unsigned char)*f)) {
            while (is_space((unsigned char)*f)) f++;
            while (is_space((unsigned char)*in)) in++;
        } else if (*f != '%') {
            if (*in == '\0') {
                status = SCAN_END;
            } else if (*in != *f) {
                status = SCAN_MISMATCH;
            } else {
                in++;
                f++;
            }
        } else {
            f++;
            status = scan_directive(&in, &f, &ap);
            if (status == SCAN_OK && f[-1] != '%') assigned++;
        }
    }
    va_end(ap);
    if (status == SCAN_END && assigned == 0) return -1;
    return assigned;
}
