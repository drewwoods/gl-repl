/*
 * glr_telemetry.h - Optional, opt-in usage beacon (web build only).
 *
 * GitHub Pages serves the wasm build with no access logs of any kind, so the
 * only way to learn whether the browser build actually *works* for people is
 * for the page to say so itself. This is the C-side entry point into that:
 * app code names an event, and on the web the shell's telemetry shim decides
 * whether anything is sent.
 *
 * Two properties are deliberate and load-bearing:
 *
 *   - **Native builds are a hard no-op.** glr_telemetry_event()'s body is
 *     empty off the web - not "disabled at runtime", not "off by default".
 *     A desktop gl-repl has no telemetry code path, and no flag turns one on.
 *
 *   - **The web build is inert unless a collector was baked in at deploy
 *     time.** scripts/web-stamp-build.sh substitutes an empty endpoint by
 *     default, and the shim then drops every event without a network
 *     request. A local `make web`, a fork's build, and anyone serving the
 *     artifact themselves therefore phone home to nobody.
 *
 * Event names are coarse, low-cardinality strings ("boot", "example/Torus").
 * Nothing user-authored is ever passed: scene text, file names, expressions
 * and variable values stay in the browser. The names that exist are the ones
 * that answer "did the app reach a running state, and what did people look
 * at" - see packaging/web/README.md for the catalog and the deploy switch.
 */
#ifndef GLR_TELEMETRY_H
#define GLR_TELEMETRY_H

#include <stddef.h>

/* Event names are dimension values in whatever dashboard receives them, so
 * they must stay low-cardinality and bounded. The cap is well past every name
 * this tree emits; it exists so a future caller cannot turn an arbitrary
 * scene name into an unbounded string. */
#define GLR_TELEMETRY_NAME_MAX 96

/* Record one named event. Safe with NULL/empty (dropped), safe before the
 * page's shim exists (dropped), and never blocks the caller: the web path
 * hands off to the shim, which itself only ever fires a fire-and-forget
 * beacon. There is no return value because there is nothing useful a caller
 * could do about a dropped event. */
void glr_telemetry_event(const char *name);

/* Convenience for the "<prefix>/<detail>" event shape, so call sites do not
 * each grow their own snprintf buffer. Either argument NULL/empty drops the
 * event. */
void glr_telemetry_event_detail(const char *prefix, const char *detail);

/* Sanitizers, exposed for tests (and compiled on every platform, unlike the
 * beacon itself - the rule about what may become an event name is worth
 * asserting on the machine doing the development).
 *
 * glr_telemetry_slugify() writes at most `cap` bytes including the
 * terminator, always NUL-terminating a non-zero `cap`. glr_telemetry_join()
 * formats "<prefix>/<detail>", slugifies it, and returns 1 on success or 0
 * when the result would be empty. */
void glr_telemetry_slugify(const char *src, char *dst, size_t cap);
int  glr_telemetry_join(const char *prefix, const char *detail,
                        char *dst, size_t cap);

#endif /* GLR_TELEMETRY_H */
