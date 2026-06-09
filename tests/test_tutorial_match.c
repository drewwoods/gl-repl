#include "subsystems/tutorial/tutorial.h"
#include "support/test_harness.h"
#include "repl/eval.h"
#include "repl/tutorials.h"

/* Stubs to satisfy tutorial_shadow_suffix references when compiling tutorial_match.c */
int tutorial_active(void) { return 0; }
const char *tutorial_current_expected_text(void) { return NULL; }
TutorialRuntimeState tutorial_state_view(void) { return (TutorialRuntimeState){0}; }
int repl_eval_find_predef_var_idx(const char *name) { (void)name; return -1; }
const TutorialStep *repl_tutorial_step_get(int idx, int step_idx) { (void)idx; (void)step_idx; return NULL; }

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static void test_match_shapes(void) {
    TutorialMatchResult result;

    result = tutorial_match("glBegin(GL_TRIANGLES)", "glBegin(GL_TRIANGLES)");
    ASSERT_INT("exact match kind", result.kind, TUT_MATCH_OK);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "  glBegin( GL_TRIANGLES )  ");
    ASSERT_INT("whitespace tolerant kind", result.kind, TUT_MATCH_OK);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "\tglBegin(GL_TRIANGLES);  ");
    ASSERT_INT("trailing semicolon tolerant", result.kind, TUT_MATCH_OK);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "   \t  ");
    ASSERT_INT("empty input mismatch", result.kind, TUT_MISMATCH_EMPTY);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "glEnd()");
    ASSERT_INT("shape mismatch kind", result.kind, TUT_MISMATCH_SHAPE);
}

int main(void) {
    test_match_shapes();
    return test_harness_report(&g_harness, "test_tutorial_match");
}
