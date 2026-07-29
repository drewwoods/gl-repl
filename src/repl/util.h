/*
 * src/repl/util.h - Small inline buffer helpers shared across REPL TUs.
 *
 * `repl_format_fits` and `repl_copy_string_fits` are size-checked
 * vsnprintf / copy wrappers. They return 0 on truncation so the caller can
 * surface a diagnostic instead of silently clipping text.
 * `repl_append_clamped` is the multi-call form, for text built up across
 * several appends. Keeping them in this narrow header lets consumers get safe
 * fixed-buffer helpers without pulling in a broader internal surface. Include
 * it directly where the helpers are used.
 */
#ifndef REPL_UTIL_H
#define REPL_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define REPL_PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define REPL_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

static inline int repl_format_fits(char *out, size_t out_sz,
                                   const char *fmt, ...) REPL_PRINTF_LIKE(3, 4);
static inline int repl_format_fits(char *out, size_t out_sz,
                                   const char *fmt, ...) {
    va_list ap;
    int written;

    if (!out || out_sz == 0)
        return 0;

    va_start(ap, fmt);
    written = vsnprintf(out, out_sz, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Append `fmt` at out[off]. Returns the new offset, always within
 * [0, out_sz - 1], and sets *truncated (when non-NULL) to 1 if the text did
 * not fit. The flag is never cleared, so one variable can cover a whole
 * sequence of appends and be checked once at the end.
 *
 * Use this instead of `off += snprintf(out + off, out_sz - off, ...)`.
 * snprintf returns the length it *would* have written, so a raw accumulation
 * can push `off` past out_sz; the next call then computes `out_sz - off` in
 * unsigned arithmetic, underflows to a huge size_t, and writes out of bounds
 * with no truncation left to stop it. Two stack-buffer overflows came from
 * exactly that idiom (format_decl_text in compile.c, parse_snippet_declare in
 * import.c), both reachable from ordinary input. */
static inline int repl_append_clamped(char *out, size_t out_sz, int off,
                                      int *truncated,
                                      const char *fmt, ...) REPL_PRINTF_LIKE(5, 6);
static inline int repl_append_clamped(char *out, size_t out_sz, int off,
                                      int *truncated,
                                      const char *fmt, ...) {
    va_list ap;
    int written;

    if (!out || out_sz == 0) {
        if (truncated) *truncated = 1;
        return 0;
    }
    if (off < 0)
        off = 0;
    if ((size_t)off >= out_sz - 1) {
        /* Already full: out[out_sz - 1] is the terminator a previous append
         * left behind, so there is nothing to write and nothing to fix. */
        if (truncated) *truncated = 1;
        return (int)out_sz - 1;
    }

    va_start(ap, fmt);
    written = vsnprintf(out + off, out_sz - (size_t)off, fmt, ap);
    va_end(ap);

    if (written < 0) {
        if (truncated) *truncated = 1;
        return off;
    }
    if ((size_t)off + (size_t)written >= out_sz) {
        if (truncated) *truncated = 1;
        return (int)out_sz - 1;
    }
    return off + written;
}

static inline int repl_copy_string_fits(char *dst, size_t dst_sz,
                                        const char *src) {
    size_t len;

    if (!dst || dst_sz == 0)
        return 0;
    if (!src) {
        dst[0] = '\0';
        return 1;
    }

    len = strlen(src);
    if (len >= dst_sz) {
        dst[0] = '\0';
        return 0;
    }

    memcpy(dst, src, len + 1);
    return 1;
}

#endif /* REPL_UTIL_H */
