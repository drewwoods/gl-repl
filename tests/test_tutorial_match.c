#include "subsystems/tutorial/tutorial.h"
#include "support/test_harness.h"

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
