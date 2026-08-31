/*
 * glr_telemetry.c - Optional, opt-in usage beacon (web build only).
 *
 * See glr_telemetry.h for the policy. Two parts: a name sanitizer compiled
 * everywhere (so it is testable off the web, where the rest of this file does
 * not exist), and a single EM_ASM hand-off to the page's shim.
 */
#include "app/glr_telemetry.h"

#include <stddef.h>
#include <stdio.h>

/* Keep to a conservative set - letters, digits, and the three separators the
 * event vocabulary uses. Everything else (spaces and parens in example names
 * included) becomes '-', so "Wave surface (analytic normals)" reports as a
 * stable slug rather than a URL-escaping question. Runs of rejected
 * characters collapse to one dash and a name never opens or closes on one,
 * so a rename that only changes punctuation does not fork the series.
 *
 * Truncation, not rejection: a name too long for the buffer is cut short.
 * A slightly-clipped label is a usable dimension value; a dropped event is
 * a hole in the data. */
void glr_telemetry_slugify(const char *src, char *dst, size_t cap) {
    size_t out = 0;
    int pending_dash = 0;

    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;

    for (size_t i = 0; src[i] != '\0' && out + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_';
        if (!keep) {
            /* Never open with a dash, and never emit a run of them. Deferred
             * rather than written now, so a name ending in punctuation does
             * not pick up a trailing dash either. */
            pending_dash = (out > 0);
            continue;
        }
        if (pending_dash) {
            /* Room for the dash AND the character it separates, or stop
             * here: writing the dash first and discovering there is no room
             * for the character would leave the name ending in separator
             * noise, which is exactly what deferring the dash exists to
             * prevent. */
            if (out + 2 >= cap)
                break;
            dst[out++] = '-';
            pending_dash = 0;
        }
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

int glr_telemetry_join(const char *prefix, const char *detail,
                       char *dst, size_t cap) {
    char slug[GLR_TELEMETRY_NAME_MAX];
    char joined[GLR_TELEMETRY_NAME_MAX * 2];

    if (!dst || cap == 0)
        return 0;
    dst[0] = '\0';
    if (!prefix || !prefix[0] || !detail || !detail[0])
        return 0;

    snprintf(joined, sizeof(joined), "%s/%s", prefix, detail);
    glr_telemetry_slugify(joined, slug, sizeof(slug));
    if (!slug[0])
        return 0;

    snprintf(dst, cap, "%s", slug);
    return dst[0] != '\0';
}

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>

void glr_telemetry_event(const char *name) {
    char slug[GLR_TELEMETRY_NAME_MAX];

    if (!name || !name[0])
        return;

    glr_telemetry_slugify(name, slug, sizeof(slug));
    if (!slug[0])
        return;

    /* The shim owns every policy decision - whether a collector was baked in
     * at deploy time, whether the visitor opted out, the repeat and budget
     * limits, and the transport. This side only names the thing that
     * happened. A missing shim (someone serving index.js against a page of
     * their own) is not an error. */
    EM_ASM({
        try {
            if (window.glrTelemetry && window.glrTelemetry.event)
                window.glrTelemetry.event(UTF8ToString($0));
        } catch (e) {
            /* Telemetry must never take the app down with it. */
        }
    }, slug);
}

#else /* Native: no telemetry path exists at all. */

/* An empty body, not a runtime-disabled one: a desktop gl-repl has no beacon
 * code and no flag that could turn one on. Call sites stay unconditional so
 * the native build still type-checks them. */
void glr_telemetry_event(const char *name) {
    (void)name;
}

#endif /* __EMSCRIPTEN__ */

void glr_telemetry_event_detail(const char *prefix, const char *detail) {
    char joined[GLR_TELEMETRY_NAME_MAX];

    if (glr_telemetry_join(prefix, detail, joined, sizeof(joined)))
        glr_telemetry_event(joined);
}
