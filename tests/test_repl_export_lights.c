/*
 * test_repl_export_lights.c - Editor / exported-C lights-text consistency.
 *
 * The light setup text shown in the editor's code panel and the text
 * written into the exported .c file come from the same generator
 * (repl_export_lights_*). The two surfaces share by construction;
 * this test guards against future divergence where someone hand-rolls
 * either surface or rewrites the file emitter to bypass the generator.
 */
#include "app/glr_ctrl.h"
#include "repl/core.h"
#include "repl/export.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Locate the start of the `void init() {` body in the exported file
 * and verify each generator-produced init line appears before the
 * closing `}`. The window check rejects accidental matches in
 * display() body that share the same substring. */
static int substring_in_window(const char *text, const char *window_start_marker,
                               const char *window_end_marker, const char *needle) {
    const char *start = strstr(text, window_start_marker);
    if (!start) return 0;
    const char *end = strstr(start, window_end_marker);
    if (!end) return 0;
    /* memmem isn't portable; do a bounded strstr by null-temping the buffer. */
    /* Caller passes mutable buffer so we can NUL-terminate the window. */
    char saved = *end;
    *((char *)end) = '\0';
    int found = strstr(start, needle) != NULL;
    *((char *)end) = saved;
    return found;
}

int main(void) {
    const char *path = "/tmp/repl_export_lights_test.c";

    glr_app_reset_all();

    /* Trigger an export of the default scene. */
    repl_export_save_output(path, source_document_view(), NULL);

    char *text = slurp(path);
    ASSERT_TRUE("exported file readable", text != NULL);
    if (!text) {
        return test_harness_report(&g_harness, "repl_export_lights");
    }

    /* Init-section lines should land between `void init() {` and the
     * matching close brace just before `int main`. */
    int n_init = repl_export_lights_init_line_count();
    ASSERT_TRUE("init line count > 0", n_init > 0);
    for (int i = 0; i < n_init; i++) {
        char line[256];
        repl_export_lights_init_line(i, line, sizeof(line));
        /* The generator emits 2-space-indented lines; strip the leading
         * whitespace because the exported file may add its own
         * indentation in some sections. We match the bare text. */
        const char *needle = line;
        while (*needle == ' ') needle++;
        char label[128];
        snprintf(label, sizeof(label),
                 "init line %d appears in init() body: %s", i, needle);
        int found = substring_in_window(text, "void init() {",
                                        "}\n\nint main", needle);
        ASSERT_TRUE(label, found);
    }

    /* Display-section lines should land between `void display() {` and
     * the closing `glPopAttrib`. */
    int n_disp = repl_export_lights_display_line_count();
    ASSERT_TRUE("display line count > 0", n_disp > 0);
    for (int i = 0; i < n_disp; i++) {
        char line[256];
        repl_export_lights_display_line(i, line, sizeof(line));
        const char *needle = line;
        while (*needle == ' ') needle++;
        char label[128];
        snprintf(label, sizeof(label),
                 "display line %d appears in display() body: %s", i, needle);
        int found = substring_in_window(text, "void display() {",
                                        "glPopAttrib();", needle);
        ASSERT_TRUE(label, found);
    }

    free(text);

    return test_harness_report(&g_harness, "repl_export_lights");
}
