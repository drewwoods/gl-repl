#include "widgets/tutorial.h"
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
    ASSERT_INT("exact match arg index", result.arg_index, -1);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "  glBegin( GL_TRIANGLES )  ");
    ASSERT_INT("whitespace tolerant kind", result.kind, TUT_MATCH_OK);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "\tglBegin(GL_TRIANGLES);  ");
    ASSERT_INT("trailing semicolon tolerant", result.kind, TUT_MATCH_OK);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "   \t  ");
    ASSERT_INT("empty input mismatch", result.kind, TUT_MISMATCH_EMPTY);
    ASSERT_INT("empty input arg index", result.arg_index, -1);

    result = tutorial_match("glBegin(GL_TRIANGLES)", "glEnd()");
    ASSERT_INT("shape mismatch kind", result.kind, TUT_MISMATCH_SHAPE);
    ASSERT_INT("shape mismatch arg index", result.arg_index, -1);
}

static void test_future_enum_slots_exist(void) {
    ASSERT_TRUE("command mismatch enum distinct",
                TUT_MISMATCH_COMMAND > TUT_MISMATCH_SHAPE);
    ASSERT_TRUE("arg mismatch enum distinct",
                TUT_MISMATCH_ARG > TUT_MISMATCH_COMMAND);
}

int main(void) {
    test_match_shapes();
    test_future_enum_slots_exist();
    return test_harness_report(&g_harness, "test_tutorial_match");
}