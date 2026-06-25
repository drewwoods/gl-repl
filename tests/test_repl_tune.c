#include "app/glr_ctrl.h"
/*
 * test_repl_tune.c - Tunable-variable (@tune) export + round-trip tests.
 *
 * Covers: the pure tag predicate, the document collector (incl. the knob cap),
 * the generated exported-C content (HUD + keyboard knobs + window-size
 * globals) vs. the untagged baseline, export/import round-trip of the tag,
 * key assignment, swatch-step parity, and a compile gate over the generated
 * file against the GL stub headers.
 */
#include "repl/program_query.h"
#include "repl/eval.h"
#include "repl/export.h"
#include "editor/input.h"
#include "source_document.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)

/* Slurp a file into a heap string (NUL-terminated). NULL on error. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static int contains(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

static int compile_generated_file(const char *path, const char *log) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "cc -std=c99 -fsyntax-only -Werror=implicit-function-declaration "
        "-DGL_STUBS -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations "
        "-Wno-unused-function -Wno-unused-variable "
        "-Itests/gl-stubs/include -Iinclude -I. -Isrc "
        "'%s' >'%s' 2>&1",
        path, log);
    int rc = system(cmd);
    if (rc != 0) {
        printf("  compile-gate output (%s):\n", log);
        char *l = read_file(log);
        if (l) { fputs(l, stdout); free(l); }
    }
    return rc;
}

static void reset_repl(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
}

/* ----------------------------------------------------------------------- */

static void test_tag_predicate(void) {
    ASSERT_TRUE("bare @tune matches",
                repl_eval_line_has_tune_tag("float a = 1; // @tune"));
    ASSERT_TRUE("@tune with trailing text matches",
                repl_eval_line_has_tune_tag("float a = 1; // @tune knob"));
    ASSERT_TRUE("no comment -> no match",
                !repl_eval_line_has_tune_tag("float a = 1;"));
    ASSERT_TRUE("unrelated comment -> no match",
                !repl_eval_line_has_tune_tag("float a = 1; // hello"));
    ASSERT_TRUE("@tuned does NOT match (whole token)",
                !repl_eval_line_has_tune_tag("float a = 1; // @tuned=5"));
}

static void test_swatch_parity(void) {
    /* Pins the in-app formula the exported tuning_step() mirrors. If
     * these change, the export generator (write_tune_helpers) must follow. */
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "swatch 0.5", repl_eval_swatch_step(0.5f), 0.05f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "swatch 5",   repl_eval_swatch_step(5.0f), 0.05f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "swatch 50",  repl_eval_swatch_step(50.0f), 0.5f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "swatch 500", repl_eval_swatch_step(500.0f), 5.0f);
}

static void test_collector_and_export(void) {
    const char *path = "/tmp/repl_tune_export.c";
    reset_repl();

    editor_feed_line("float amp = 1.5; // @tune");
    editor_feed_line("float freq = 2; // @tune");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(amp, 0, 0);");
    editor_feed_line("glVertex3f(0, freq, 0);");
    editor_feed_line("glVertex3f(0, 0, amp);");
    editor_feed_line("glEnd();");

    const char *names[REPL_TUNE_MAX_KNOBS];
    int total = -1;
    int n = repl_collect_tuned_vars(repl_state_document_cmds(),
                                    repl_state_document_count(),
                                    source_document_view(), names,
                                    REPL_TUNE_MAX_KNOBS, &total);
    ASSERT_INT("collector count", n, 2);
    ASSERT_INT("collector total", total, 2);
    if (n == 2) {
        ASSERT_TRUE("first knob amp", strcmp(names[0], "amp") == 0);
        ASSERT_TRUE("second knob freq", strcmp(names[1], "freq") == 0);
    }

    repl_export_save_output(path, source_document_view(), NULL);
    char *c = read_file(path);
    ASSERT_TRUE("export readable", c != NULL);

    ASSERT_TRUE("emits tuning_step", contains(c, "tuning_step"));
    ASSERT_TRUE("emits draw_tuning_overlay def", contains(c, "void draw_tuning_overlay"));
    ASSERT_TRUE("emits hud_text helper",
                contains(c, "static void hud_text(float x, float y, const char *fmt, ...)"));
    ASSERT_TRUE("calls draw_tuning_overlay", contains(c, "draw_tuning_overlay();"));
    ASSERT_TRUE("hud_text formats via vsnprintf",
                contains(c, "vsnprintf(line_text, sizeof line_text, fmt, args);"));
    ASSERT_TRUE("hud_text hoists char pointer",
                contains(c, "const char *ch;\n  char line_text[96];"));
    ASSERT_TRUE("hud_text loop reuses declared char pointer",
                contains(c, "for (ch = line_text; *ch; ch++)"));
    ASSERT_TRUE("no C99 helper loop declaration",
                !contains(c, "for (const char *ch ="));
    ASSERT_TRUE("overlay uses hud_text helper",
                contains(c, "hud_text(8.0f, text_y, \"q/a  amp = %.4g\", (double)amp);"));
    ASSERT_TRUE("overlay text_y declared before GL statements",
                contains(c, "float text_y = (float)overlay_height - 18.0f;\n\n  glMatrixMode(GL_PROJECTION);"));
    ASSERT_TRUE("emits readable window-size globals", contains(c, "static int window_width"));
    ASSERT_TRUE("captures window size in reshape",
                contains(c, "window_width = w;\n  window_height = h;"));
    ASSERT_TRUE("fabsf (not manual abs) in step", contains(c, "fabsf(value)"));
    ASSERT_TRUE("keyboard locals precede unused param markers",
                contains(c, "int modifiers = glutGetModifiers();\n"
                            "  unsigned char normalized_key = key;\n"
                            "  float step_scale = 1.0f;\n\n"
                            "  (void)mouse_x;\n"
                            "  (void)mouse_y;"));
    ASSERT_TRUE("knob q raises amp",
                contains(c, "if (normalized_key == 'q') amp += tuning_step(amp) * step_scale;"));
    ASSERT_TRUE("knob a lowers amp",
                contains(c, "if (normalized_key == 'a') amp -= tuning_step(amp) * step_scale;"));
    ASSERT_TRUE("knob w raises freq",
                contains(c, "if (normalized_key == 'w') freq += tuning_step(freq) * step_scale;"));
    ASSERT_TRUE("Shift fine uses shared scale",
                contains(c, "step_scale *= "
                         REPL_EXPORT_STRINGIFY(GLR_ADJUST_FINE_SCALE)));
    ASSERT_TRUE("Ctrl coarse uses shared scale",
                contains(c, "step_scale *= "
                         REPL_EXPORT_STRINGIFY(GLR_ADJUST_COARSE_SCALE)));
    ASSERT_TRUE("blank line before key handlers",
                contains(c, "step_scale *= "
                         REPL_EXPORT_STRINGIFY(GLR_ADJUST_COARSE_SCALE)
                         ";\n\n  if (normalized_key == 'q')"));
    ASSERT_TRUE("round-trip marker amp", contains(c, "@declare amp @tune"));
    ASSERT_TRUE("round-trip marker freq", contains(c, "@declare freq @tune"));
    /* baseline keyboard handlers stay */
    ASSERT_TRUE("keeps space-toggle",
                contains(c, "if (key == ' ')"));
    free(c);
}

static void test_untagged_baseline(void) {
    const char *path = "/tmp/repl_tune_none.c";
    reset_repl();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");

    int total = -1;
    int n = repl_collect_tuned_vars(repl_state_document_cmds(),
                                    repl_state_document_count(),
                                    source_document_view(), NULL, 0, &total);
    ASSERT_INT("no tagged vars", n, 0);
    ASSERT_INT("no tagged total", total, 0);

    repl_export_save_output(path, source_document_view(), NULL);
    char *c = read_file(path);
    ASSERT_TRUE("export readable (none)", c != NULL);
    ASSERT_TRUE("no HUD when untagged", !contains(c, "draw_tuning_overlay"));
    ASSERT_TRUE("no hud_text when untagged", !contains(c, "hud_text("));
    ASSERT_TRUE("no tuning_step when untagged", !contains(c, "tuning_step"));
    ASSERT_TRUE("no knob decode when untagged",
                !contains(c, "normalized_key"));
    free(c);
}

static void test_roundtrip(void) {
    const char *path = "/tmp/repl_tune_rt.c";
    reset_repl();
    editor_feed_line("float amp = 1.5; // @tune");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(amp, 0, 0);");
    editor_feed_line("glEnd();");
    repl_export_save_output(path, source_document_view(), NULL);

    /* Reimport and confirm the tag survived. */
    reset_repl();
    int loaded = repl_export_load_from_file(path, NULL);
    ASSERT_INT("import ok", loaded, 1);

    const char *names[REPL_TUNE_MAX_KNOBS];
    int total = -1;
    int n = repl_collect_tuned_vars(repl_state_document_cmds(),
                                    repl_state_document_count(),
                                    source_document_view(), names,
                                    REPL_TUNE_MAX_KNOBS, &total);
    ASSERT_INT("tag survives round-trip", n, 1);
    if (n == 1)
        ASSERT_TRUE("round-trip name amp", strcmp(names[0], "amp") == 0);

    /* The reconstructed decl line itself carries `// @tune`. */
    int found_tagged_decl = 0;
    SourceTextView v = source_document_view();
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds()[i].type == CMD_VAR_DECLARE &&
            repl_eval_line_has_tune_tag(source_text_line(v, i)))
            found_tagged_decl = 1;
    }
    ASSERT_TRUE("decl line carries // @tune after import", found_tagged_decl);
}

static void test_key_assignment_and_cap(void) {
    const char *path = "/tmp/repl_tune_cap.c";
    reset_repl();
    /* 10 tagged vars: knob assignment goes q/a, w/s, e/d, ... i/k, o/l;
     * the 10th is dropped (cap = REPL_TUNE_MAX_KNOBS = 9). */
    static const char *vnames[10] = {
        "knob0","knob1","knob2","knob3","knob4",
        "knob5","knob6","knob7","knob8","knob9"
    };
    for (int i = 0; i < 10; i++) {
        char line[64];
        snprintf(line, sizeof line, "float %s = %d; // @tune", vnames[i], i + 1);
        editor_feed_line(line);
    }

    int total = -1;
    int n = repl_collect_tuned_vars(repl_state_document_cmds(),
                                    repl_state_document_count(),
                                    source_document_view(), NULL, 0, &total);
    (void)n;
    ASSERT_INT("cap: total counted", total, 10);

    repl_export_save_output(path, source_document_view(), NULL);
    char *c = read_file(path);
    ASSERT_TRUE("export readable (cap)", c != NULL);
    /* Key assignment q/a, w/s, e/d for the first three. */
    ASSERT_TRUE("knob0 -> q/a",
                contains(c, "if (normalized_key == 'q') knob0 +="));
    ASSERT_TRUE("knob1 -> w/s",
                contains(c, "if (normalized_key == 'w') knob1 +="));
    ASSERT_TRUE("knob2 -> e/d",
                contains(c, "if (normalized_key == 'e') knob2 +="));
    /* 9th knob is o/l; the 10th must be dropped (no p key, knob9 absent). */
    ASSERT_TRUE("knob8 -> o/l",
                contains(c, "if (normalized_key == 'o') knob8 +="));
    ASSERT_TRUE("knob9 dropped (capped)", !contains(c, "knob9 +="));
    ASSERT_TRUE("cap note emitted", contains(c, "capped at 9 keyboard knobs"));
    free(c);
}

static void test_generated_names_do_not_shadow_tuned_vars(void) {
    const char *path = "/tmp/repl_tune_shadow.c";
    const char *log  = "/tmp/repl_tune_shadow.log";
    reset_repl();

    editor_feed_line("float key = 1; // @tune");
    editor_feed_line("float x = 2; // @tune");
    editor_feed_line("float y = 3; // @tune");
    editor_feed_line("float w = 4; // @tune");
    editor_feed_line("float h = 5; // @tune");
    editor_feed_line("float hud_text = 6; // @tune");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(key, x, y);");
    editor_feed_line("glVertex3f(w, h, 0);");
    editor_feed_line("glEnd();");
    repl_export_save_output(path, source_document_view(), NULL);

    char *c = read_file(path);
    ASSERT_TRUE("shadow export readable", c != NULL);
    ASSERT_TRUE("keyboard callback key param falls back around global key",
                contains(c, "void keyboard(unsigned char pressed_key,"));
    ASSERT_TRUE("preferred key param absent when key is a user var",
                !contains(c, "void keyboard(unsigned char key,"));
    ASSERT_TRUE("old overlay w/h locals absent", !contains(c, "int w ="));
    ASSERT_TRUE("old overlay y local absent", !contains(c, "float y = (float)"));
    ASSERT_TRUE("old overlay ln local absent", !contains(c, "char ln[96]"));
    ASSERT_TRUE("hud_text helper name falls back around user var",
                !contains(c, "static void hud_text(float x, float y, const char *fmt, ...)"));
    ASSERT_TRUE("tuned key still adjusted",
                contains(c, "key += tuning_step(key) * step_scale"));
    ASSERT_TRUE("tuned h still adjusted",
                contains(c, "h += tuning_step(h) * step_scale"));
    ASSERT_TRUE("tuned hud_text still adjusted",
                contains(c, "hud_text += tuning_step(hud_text) * step_scale"));
    free(c);

    ASSERT_INT("shadow-collision export compiles against stubs",
               compile_generated_file(path, log), 0);
}

/* Compile the generated knob file against the GL stub headers with the
 * project's load-bearing failure mode so a missing generated include /
 * implicit declaration fails loudly. */
static void test_compile_gate(void) {
    const char *path = "/tmp/repl_tune_gate.c";
    const char *log  = "/tmp/repl_tune_gate.log";
    reset_repl();
    editor_feed_line("float amp = 1.5; // @tune");
    editor_feed_line("float freq = 2; // @tune");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(amp, freq, 0);");
    editor_feed_line("glEnd();");
    repl_export_save_output(path, source_document_view(), NULL);

    int rc = compile_generated_file(path, log);
    ASSERT_INT("generated knob file compiles against stubs", rc, 0);
}

int main(void) {
    test_tag_predicate();
    test_swatch_parity();
    test_collector_and_export();
    test_untagged_baseline();
    test_roundtrip();
    test_key_assignment_and_cap();
    test_generated_names_do_not_shadow_tuned_vars();
    test_compile_gate();
    return test_harness_report(&g_harness, "test_repl_tune");
}
