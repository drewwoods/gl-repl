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
#include "app/glr_config.h"
#include "source_document.h"
#include "repl/export.h"
#include "scene/themes.h"      /* LIGHT_THEME_HEADLIGHT / _DEFAULT for the
                                  headlight round-trip case */

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

/* Locate the start of the `void init(void) {` body in the exported file
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

static void expected_export_line(char *out, size_t out_sz, const char *line) {
    const char *comment = strstr(line, "//");
    size_t prefix_len;

    if (!comment) {
        snprintf(out, out_sz, "%s", line);
        return;
    }
    prefix_len = (size_t)(comment - line);
    if (prefix_len >= out_sz)
        prefix_len = out_sz - 1;
    memcpy(out, line, prefix_len);
    snprintf(out + prefix_len, out_sz - prefix_len, "/*%s */", comment + 2);
}

int main(void) {
    const char *path = "/tmp/repl_export_lights_test.c";

    glr_ctrl_reset_all();

    /* Trigger an export of the default scene. */
    repl_export_save_output(path, source_document_view(), NULL);

    char *text = slurp(path);
    ASSERT_TRUE("exported file readable", text != NULL);
    if (!text) {
        return test_harness_report(&g_harness, "repl_export_lights");
    }

    /* Init-section lines should land between `void init(void) {` and the
     * matching close brace just before `int main`. */
    int n_init = repl_export_lights_init_line_count();
    ASSERT_TRUE("init line count > 0", n_init > 0);
    for (int i = 0; i < n_init; i++) {
        char line[256];
        char expected[256];
        repl_export_lights_init_line(i, line, sizeof(line));
        expected_export_line(expected, sizeof(expected), line);
        /* The generator emits 2-space-indented lines; strip the leading
         * whitespace because the exported file may add its own
         * indentation in some sections. We match the bare text. */
        const char *needle = expected;
        while (*needle == ' ') needle++;
        char label[512];
        snprintf(label, sizeof(label),
                 "init line %d appears in init() body: %s", i, needle);
        int found = substring_in_window(text, "void init(void) {",
                                        "}\n\nint main", needle);
        ASSERT_TRUE(label, found);
    }

    /* Display-section lines should land between `void display(void) {` and
     * the closing `glPopAttrib`. */
    int n_disp = repl_export_lights_display_line_count();
    ASSERT_TRUE("display line count > 0", n_disp > 0);
    for (int i = 0; i < n_disp; i++) {
        char line[256];
        char expected[256];
        repl_export_lights_display_line(i, line, sizeof(line));
        expected_export_line(expected, sizeof(expected), line);
        const char *needle = expected;
        while (*needle == ' ') needle++;
        char label[512];
        snprintf(label, sizeof(label),
                 "display line %d appears in display() body: %s", i, needle);
        int found = substring_in_window(text, "void display(void) {",
                                        "glPopAttrib();", needle);
        ASSERT_TRUE(label, found);
    }

    free(text);

    /* Headlight round-trip: switch theme to HEADLIGHT (mirrors what a
     * saved `@cfg light_theme = HEADLIGHT` load triggers), re-export,
     * and check that slot-0 POSITION moves from display() to init()
     * while slots 1..3 stay in display(). */
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_HEADLIGHT);
    const char *path_hl = "/tmp/repl_export_lights_headlight.c";
    repl_export_save_output(path_hl, source_document_view(), NULL);
    char *text_hl = slurp(path_hl);
    ASSERT_TRUE("headlight file readable", text_hl != NULL);
    if (text_hl) {
        int slot0_in_init = substring_in_window(
            text_hl, "void init(void) {", "}\n\nint main",
            "glLightfv(GL_LIGHT0, GL_POSITION");
        int slot0_in_display = substring_in_window(
            text_hl, "void display(void) {", "glPopAttrib();",
            "glLightfv(GL_LIGHT0, GL_POSITION");
        int slot1_in_display = substring_in_window(
            text_hl, "void display(void) {", "glPopAttrib();",
            "glLightfv(GL_LIGHT1, GL_POSITION");
        ASSERT_TRUE("headlight: GL_LIGHT0 POSITION lives in init()",
                    slot0_in_init);
        ASSERT_TRUE("headlight: GL_LIGHT0 POSITION absent from display()",
                    !slot0_in_display);
        ASSERT_TRUE("headlight: GL_LIGHT1 POSITION still in display()",
                    slot1_in_display);
        free(text_hl);
    }

    /* Reload the headlight-themed file into a fresh REPL state, then
     * re-export — the same slot-0-in-init invariant must hold, proving
     * that @cfg drives the cfg bridge → glr_config_set hook →
     * scene_lights_apply_theme path on load. */
    glr_ctrl_reset_all();
    ReplImportResult import_result;
    memset(&import_result, 0, sizeof(import_result));
    repl_export_load_from_file(path_hl, &import_result);
    const char *path_rt = "/tmp/repl_export_lights_headlight_rt.c";
    repl_export_save_output(path_rt, source_document_view(), NULL);
    char *text_rt = slurp(path_rt);
    ASSERT_TRUE("headlight round-trip file readable", text_rt != NULL);
    if (text_rt) {
        int slot0_in_init = substring_in_window(
            text_rt, "void init(void) {", "}\n\nint main",
            "glLightfv(GL_LIGHT0, GL_POSITION");
        int slot0_in_display = substring_in_window(
            text_rt, "void display(void) {", "glPopAttrib();",
            "glLightfv(GL_LIGHT0, GL_POSITION");
        ASSERT_TRUE("round-trip: GL_LIGHT0 POSITION still in init()",
                    slot0_in_init);
        ASSERT_TRUE("round-trip: GL_LIGHT0 POSITION still absent from display()",
                    !slot0_in_display);
        free(text_rt);
    }

    /* Reset back to default so subsequent test binaries see a clean
     * theme. (Each test main() owns its global state.) */
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_DEFAULT);

    return test_harness_report(&g_harness, "repl_export_lights");
}
