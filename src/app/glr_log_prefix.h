/*
 * glr_log_prefix.h - one place that decides what gl-repl's own log lines
 * look like, because the answer differs between native and the web build.
 *
 * Native keeps the historic bracketed stamps:
 *
 *     [init + 0.026s] GL init done
 *     [gl-repl] GPU profile timing disabled (...)
 *
 * The web build sits in a browser console next to gl4es, which tags every
 * line `LIBGL: `. Three competing prefix styles in one scrolling log is
 * noise, so there we match that shape - one tag, message after the colon,
 * elapsed stamp as a compact suffix rather than a padded column:
 *
 *     LIBGL: Accum buffer emulation active (16 bits per channel)
 *     GLREPL: +0.026s accum buffer: 16 bits
 *     GLREPL: GPU profile timing disabled (...)
 *
 * The tag is load-bearing beyond looks: packaging/web/shell.html routes
 * stderr by it (GLREPL-tagged lines are the informational startup trace and
 * go to console.log; anything else on stderr is a real diagnostic and keeps
 * console.error). Change the spelling here and that regex moves with it.
 *
 * Carrying the stamp is NOT a claim of being informational. glr_audio.c
 * stamps its miniaudio-log and worker-hitch lines so they can be correlated
 * against the startup timeline, and those are diagnostics; they keep their
 * own `repl_audio: ` sub-tag immediately after the prefix, which is what the
 * shell routes on. So: a message passed to this prefix may begin with a
 * lowercase `module: ` sub-tag *only* if it really is a diagnostic.
 *
 * Header-only on purpose. The three callers span bands that have no shared
 * .c to link against - src/app/boot (glr_init_trace.c), src/app
 * (glr_ctrl_init_log) and packaging/web/gl4es_bootstrap.c, which is a
 * link-time input rather than a member of $(SRCS) - and this is pure
 * formatting with no state to own.
 */
#ifndef GLR_LOG_PREFIX_H
#define GLR_LOG_PREFIX_H

#include <stddef.h>
#include <stdio.h>

/* Longest prefix any branch below can produce, plus room for an absurd
 * elapsed value. Callers size their stack buffer with this. */
#define GLR_LOG_PREFIX_MAX 32

#if defined(__EMSCRIPTEN__)
/* Untagged-by-time lines, and the literal for callers that only need the
 * tag (gl4es_bootstrap.c prints straight through it). */
#define GLR_LOG_TAG "GLREPL: "
#else
#define GLR_LOG_TAG "[gl-repl] "
#endif

/* Build the prefix for one log line into `buf` and return it, so a caller
 * can write `fprintf(stderr, "%s...", glr_log_prefix(buf, sizeof buf, &t))`.
 *
 * `elapsed_seconds` NULL means "no time source installed" - the line gets
 * the bare tag. Native deliberately spells the timed and untimed forms with
 * different tags (`[init +...]` vs `[gl-repl]`); that predates this header
 * and is preserved.
 */
static inline const char *glr_log_prefix(char *buf, size_t cap,
                                         const double *elapsed_seconds) {
    if (elapsed_seconds == NULL) {
        snprintf(buf, cap, "%s", GLR_LOG_TAG);
    } else {
#if defined(__EMSCRIPTEN__)
        snprintf(buf, cap, GLR_LOG_TAG "+%.3fs ", *elapsed_seconds);
#else
        snprintf(buf, cap, "[init +%6.3fs] ", *elapsed_seconds);
#endif
    }
    return buf;
}

#endif /* GLR_LOG_PREFIX_H */
