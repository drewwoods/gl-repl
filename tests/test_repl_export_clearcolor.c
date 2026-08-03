#include "app/glr_ctrl.h"
/* test_repl_export_clearcolor.c - glClearColor remains in the scene body and
 * executes in source order relative to glClear, matching immediate-mode GL. */
#include "repl/export.h"
#include "repl/eval.h"
#include "editor/input.h"
#include "source_document.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

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

/* Return the text of the exported init() body (between "void init(void) {" and
 * the next line-start "}"). Heap-allocated; NULL if not found. */
static char *extract_init_body(const char *c) {
    if (!c) return NULL;
    const char *start = strstr(c, "void init(void) {");
    if (!start) return NULL;
    const char *end = strstr(start, "\n}\n");
    if (!end) return NULL;
    size_t len = (size_t)(end - start);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int init_has(const char *c, const char *needle) {
    if (!c) return 0;
    char *body = extract_init_body(c);
    int r = body && strstr(body, needle) != NULL;
    free(body);
    return r;
}

static void reset_repl(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
}

static void test_body_clearcolor_is_not_hoisted(void) {
    const char *path = "/tmp/repl_cc_order.c";
    reset_repl();
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glClearColor(0.05, 0.06, 0.07, 1);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    repl_export_save_output(path, source_document_view(), NULL);

    /* RGB channels are clamped to REPL_CLEAR_COLOR_MAX_V = 0.1, so the test
     * values stay below it (real examples are dark too). */
    char *c = read_file(path);
    ASSERT_TRUE("export readable", c != NULL);
    ASSERT_TRUE("init() keeps the bootstrap default",
                init_has(c, "glClearColor(0.1, 0.1, 0.1, 1)"));
    const char *clear = strstr(c, "glClear(GL_COLOR_BUFFER_BIT);");
    const char *color = strstr(c, "glClearColor(0.05, 0.06, 0.07, 1);");
    ASSERT_TRUE("body clear is exported", clear != NULL);
    ASSERT_TRUE("body clear color is exported", color != NULL);
    ASSERT_TRUE("clear precedes later clear color", clear && color && clear < color);
    free(c);
}

static void test_no_clearcolor_keeps_default(void) {
    const char *path = "/tmp/repl_cc_default.c";
    reset_repl();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    repl_export_save_output(path, source_document_view(), NULL);

    char *c = read_file(path);
    ASSERT_TRUE("export readable (default)", c != NULL);
    ASSERT_TRUE("init() keeps the bootstrap default",
                init_has(c, "glClearColor(0.1, 0.1, 0.1, 1)"));
    free(c);
}

int main(void) {
    test_body_clearcolor_is_not_hoisted();
    test_no_clearcolor_keeps_default();
    return test_harness_report(&g_harness, "test_repl_export_clearcolor");
}
