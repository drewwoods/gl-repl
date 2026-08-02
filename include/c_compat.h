/*
 * c_compat.h - Small C-standard portability shims.
 *
 * The sample is built C2x by default; `make c99` syntax-checks the
 * sample source set under `-std=c99 -pedantic-errors` so the code stays
 * valid C99. This header hides the one construct that differs between
 * the two standards.
 *
 * STATIC_ASSERT(expr, msg): real C11 `_Static_assert` when the
 * translation unit is compiled as C11 or newer; a C99 negative-array-
 * size typedef otherwise. The C99 fallback drops the message text (only
 * the negative-size diagnostic shows) - acceptable. A distinct __LINE__
 * keeps the fallback typedef names unique; all current call sites are on
 * separate lines. Valid at both file and block scope (it is a typedef).
 *
 * ARRAY_LEN(a): element count of a true C array (NOT a pointer). The
 * result is a compile-time constant, so it is valid in static
 * initializers and STATIC_ASSERT - use it instead of hand-counting a
 * length that a sibling array or table already fixes.
 */
#ifndef C_COMPAT_H
#define C_COMPAT_H

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#else
#  define STATIC_ASSERT__CAT2(a, b) a##b
#  define STATIC_ASSERT__CAT(a, b)  STATIC_ASSERT__CAT2(a, b)
#  define STATIC_ASSERT(expr, msg)                                       \
     typedef char STATIC_ASSERT__CAT(c_compat_static_assert_at_line_,     \
                                     __LINE__)[(expr) ? 1 : -1]           \
         __attribute__((unused))
#endif

#endif /* C_COMPAT_H */
