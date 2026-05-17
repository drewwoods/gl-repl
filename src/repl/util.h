/*
 * src/repl/util.h - Small inline buffer helpers shared across REPL TUs.
 *
 * `repl_format_fits` and `repl_copy_string_fits` are size-checked
 * vsnprintf / copy wrappers. They return 0 on truncation so the caller can
 * surface a diagnostic instead of silently clipping text. Keeping them in this
 * narrow header lets consumers get safe fixed-buffer helpers without pulling in
 * the broader src/repl/core_internal.h surface.
 *
 * Phase 5 of feature/source-document-port.md split these out of
 * src/repl/core_internal.h. The latter still includes this header for
 * back-compat, so existing consumers see no behavioural change.
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
