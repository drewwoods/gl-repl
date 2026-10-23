#ifndef TESTS_SUPPORT_GL_TRACE_H
#define TESTS_SUPPORT_GL_TRACE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/* Naming GL values in stub-trace expectations.
 *
 * The stub GL headers (tests/gl-stubs/include/GL/) write every enum argument
 * to the trace as its integer code, so an expectation such as
 * "glBegin 9" or "glEnable 2884" only reads with a lookup table beside it.
 * trace_fmt() writes the same line from the named constants and leaves the
 * stub's numeric output untouched:
 *
 *     char buf[96];
 *     trace_count_line(&log, trace_fmt(buf, sizeof buf, "glBegin %u", GL_POLYGON));
 *
 * Enum arguments are traced as unsigned codes, so GLenum goes with %u; floats
 * go with %g exactly as the stub writes them. TRACE_FMT() is the same call on
 * a temporary buffer, for an expectation used once in an expression. */

#if defined(__GNUC__) || defined(__clang__)
#define GL_TRACE_PRINTF_LIKE(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define GL_TRACE_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

static inline const char *trace_fmt(char *buf, size_t size, const char *fmt, ...)
    GL_TRACE_PRINTF_LIKE(3, 4);

static inline const char *trace_fmt(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return buf;
}

/* One trace line is never longer than this in the suites that read them. */
#define TRACE_FMT_MAX 128

/* The temporary lives to the end of the enclosing block (C99 compound literal). */
#define TRACE_FMT(...) trace_fmt((char[TRACE_FMT_MAX]){ 0 }, TRACE_FMT_MAX, __VA_ARGS__)

#endif /* TESTS_SUPPORT_GL_TRACE_H */
