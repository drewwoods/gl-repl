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
const char *repl_tutorial_step_comment(int idx, int step_idx) { (void)idx; (void)step_idx; return NULL; }

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

/* Block-shaped expected text (for/if headers, close braces) matches
 * through the same whitespace-stripped compare as ordinary commands -
 * the matcher needs no block awareness. */
static void test_match_block_shapes(void) {
    TutorialMatchResult result;

    result = tutorial_match("for(i, 0, 8) {", "for(i,0,8){");
    ASSERT_INT("block open whitespace tolerant", result.kind, TUT_MATCH_OK);

    result = tutorial_match("}", "  } ; ");
    ASSERT_INT("close brace + trailing semicolon", result.kind, TUT_MATCH_OK);

    result = tutorial_match("} else {", "}else{");
    ASSERT_INT("else branch whitespace tolerant", result.kind, TUT_MATCH_OK);

    result = tutorial_match("for(i, 0, 8) {", "for(i, 0, 9) {");
    ASSERT_INT("block open arg mismatch", result.kind, TUT_MISMATCH_SHAPE);

    result = tutorial_match("for(i, 0, 8) {", "for(i, 0, 8)");
    ASSERT_INT("missing open brace mismatch", result.kind,
               TUT_MISMATCH_SHAPE);
}

int main(void) {
    test_match_shapes();
    test_match_block_shapes();
    return test_harness_report(&g_harness, "test_tutorial_match");
}
