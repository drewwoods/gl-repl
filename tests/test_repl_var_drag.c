#include "repl_var_drag.h"
#include "repl_state.h"
#include "repl_core.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", \
                label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_FLOAT(label, got, exp, tol) do { \
    g_run++; \
    float _diff = fabsf((got) - (exp)); \
    if (_diff <= (tol)) g_pass++; \
    else printf("FAIL [%s] got %g, expected %g (line %d)\n", \
                label, (double)(got), (double)(exp), __LINE__); \
} while (0)


/* Test: query when no drag is active */
static void test_inactive_queries(void) {
    repl_reset_state();

    ASSERT_INT("no drag active", repl_var_drag_active(), 0);
    ASSERT_INT("active var is -1", repl_var_drag_active_var(), -1);
    ASSERT_INT("log mode is 0", repl_var_drag_log_mode(), 0);
}

/* Test: begin drag on valid row in linear mode */
static void test_begin_linear_mode(void) {
    repl_reset_state();

    /* Ensure we have at least one predefined variable (t) */
    ASSERT_TRUE("predef vars exist", g_num_predef_vars > 0);

    float start_val = 5.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 0, 100);

    ASSERT_INT("drag is active", repl_var_drag_active(), 1);
    ASSERT_INT("active var is 0", repl_var_drag_active_var(), 0);
    ASSERT_INT("log mode is 0", repl_var_drag_log_mode(), 0);

    /* Verify internal state captured correctly (through accessor) */
    ASSERT_INT("drag is indeed active", repl_var_drag_active(), 1);
}

/* Test: begin drag on valid row in logarithmic mode */
static void test_begin_log_mode(void) {
    repl_reset_state();

    float start_val = 10.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 1, 50);

    ASSERT_INT("drag is active", repl_var_drag_active(), 1);
    ASSERT_INT("active var is 0", repl_var_drag_active_var(), 0);
    ASSERT_INT("log mode is 1", repl_var_drag_log_mode(), 1);
}

/* Test: begin with invalid row (negative) */
static void test_begin_invalid_negative_row(void) {
    repl_reset_state();

    repl_var_drag_begin(-1, 0, 100);

    ASSERT_INT("drag not active", repl_var_drag_active(), 0);
    ASSERT_INT("active var is -1", repl_var_drag_active_var(), -1);
}

/* Test: begin with invalid row (out of bounds) */
static void test_begin_invalid_row_oob(void) {
    repl_reset_state();

    int invalid_row = g_num_predef_vars + 10;
    repl_var_drag_begin(invalid_row, 0, 100);

    ASSERT_INT("drag not active", repl_var_drag_active(), 0);
    ASSERT_INT("active var is -1", repl_var_drag_active_var(), -1);
}

/* Test: linear drag motion with positive delta */
static void test_linear_motion_positive(void) {
    repl_reset_state();

    float start_val = 5.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 0, 100);
    repl_var_drag_motion(120);  /* +20 pixels */

    /* Linear: 20 pixels * 0.05 = 1.0 unit */
    float expected = start_val + 1.0f;
    ASSERT_FLOAT("linear +20px", g_predef_vars[0].value, expected, 1e-5f);
}

/* Test: linear drag motion with negative delta */
static void test_linear_motion_negative(void) {
    repl_reset_state();

    float start_val = 5.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 0, 100);
    repl_var_drag_motion(80);  /* -20 pixels */

    /* Linear: -20 pixels * 0.05 = -1.0 unit */
    float expected = start_val - 1.0f;
    ASSERT_FLOAT("linear -20px", g_predef_vars[0].value, expected, 1e-5f);
}

/* Test: linear drag motion from zero */
static void test_linear_motion_from_zero(void) {
    repl_reset_state();

    g_predef_vars[0].value = 0.0f;

    repl_var_drag_begin(0, 0, 100);
    repl_var_drag_motion(140);  /* +40 pixels */

    /* Linear: 40 * 0.05 = 2.0 */
    float expected = 2.0f;
    ASSERT_FLOAT("linear from zero", g_predef_vars[0].value, expected, 1e-5f);
}

/* Test: logarithmic drag motion with positive delta (multiply) */
static void test_log_motion_positive(void) {
    repl_reset_state();

    float start_val = 1.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 1, 100);
    repl_var_drag_motion(300);  /* +200 pixels = one decade */

    /* Log: 200px per decade means 1 * 10 = 10 */
    float expected = 10.0f;
    ASSERT_FLOAT("log +200px", g_predef_vars[0].value, expected, 0.01f);
}

/* Test: logarithmic drag motion with negative delta (divide) */
static void test_log_motion_negative(void) {
    repl_reset_state();

    float start_val = 10.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 1, 300);
    repl_var_drag_motion(100);  /* -200 pixels = one decade down */

    /* Log: 10 / 10 = 1 */
    float expected = 1.0f;
    ASSERT_FLOAT("log -200px", g_predef_vars[0].value, expected, 0.01f);
}

/* Test: logarithmic drag preserves sign (negative) */
static void test_log_motion_negative_value(void) {
    repl_reset_state();

    float start_val = -2.0f;
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 1, 100);
    repl_var_drag_motion(300);  /* +200 pixels */

    /* Log: -2 * 10 = -20 (sign preserved) */
    float expected = -20.0f;
    ASSERT_FLOAT("log negative sign", g_predef_vars[0].value, expected, 0.1f);
}

/* Test: logarithmic drag near-zero bootstrap */
static void test_log_motion_near_zero_bootstrap(void) {
    repl_reset_state();

    float start_val = 1e-7f;  /* Near zero */
    g_predef_vars[0].value = start_val;

    repl_var_drag_begin(0, 1, 100);
    repl_var_drag_motion(140);  /* +40 pixels */

    /* Near-zero bootstrap: dx * 0.001 = 40 * 0.001 = 0.04 */
    float expected = 0.04f;
    ASSERT_FLOAT("log near-zero bootstrap", g_predef_vars[0].value, expected, 1e-5f);
}

/* Test: motion without active drag is no-op */
static void test_motion_no_active_drag(void) {
    repl_reset_state();

    float before = 5.0f;
    g_predef_vars[0].value = before;

    repl_var_drag_motion(200);  /* No begin() called */

    /* Value should be unchanged */
    ASSERT_FLOAT("motion without drag no-op", g_predef_vars[0].value, before, 1e-9f);
}

/* Test: reset clears drag state */
static void test_reset(void) {
    repl_reset_state();

    g_predef_vars[0].value = 5.0f;
    repl_var_drag_begin(0, 1, 100);

    ASSERT_INT("drag active before reset", repl_var_drag_active(), 1);

    repl_var_drag_reset();

    ASSERT_INT("drag not active after reset", repl_var_drag_active(), 0);
    ASSERT_INT("active var -1 after reset", repl_var_drag_active_var(), -1);
    ASSERT_INT("log mode 0 after reset", repl_var_drag_log_mode(), 0);
}

/* Test: source code synchronization for literal CMD_VAR_ASSIGN */
static void test_source_sync_literal_assign(void) {
    repl_reset_state();

    /* Create a literal assignment: x = 3.14; */
    /* First need to declare x */
    repl_feed_line_public("float x;");

    /* Add assignment */
    repl_feed_line_public("x = 3.14;");

    int x_idx = repl_eval_find_predef_var_idx("x");
    ASSERT_TRUE("x declared", x_idx >= 0);

    /* Start drag on x */
    repl_var_drag_begin(x_idx, 0, 100);

    /* Move mouse */
    repl_var_drag_motion(120);  /* +20 pixels = +1.0 */

    /* Find the assignment command and verify source was updated */
    int found = 0;
    for (int i = 0; i < repl_state_document_count(); i++) {
        GLCmd *cmd = &repl_state_document_cmds_mut()[i];
        if (cmd->type == CMD_VAR_ASSIGN && cmd->num_args == x_idx && !cmd->has_vars) {
            found = 1;
            /* Source should contain the new value */
            ASSERT_FLOAT("assignment value updated", cmd->args[0], 4.14f, 0.01f);
            /* Source text should reflect new value */
            ASSERT_TRUE("source text updated", strstr(cmd->source, "4.14") != NULL);
        }
    }
    ASSERT_INT("assignment command found", found, 1);
}

/* Test: source sync skips expression-based assignments */
static void test_source_sync_skip_expressions(void) {
    repl_reset_state();

    /* Declare two variables */
    repl_feed_line_public("float x;");
    repl_feed_line_public("float y;");

    int x_idx = repl_eval_find_predef_var_idx("x");
    int y_idx = repl_eval_find_predef_var_idx("y");

    g_predef_vars[x_idx].value = 5.0f;
    g_predef_vars[y_idx].value = 2.0f;

    /* Add expression assignment: x = y * 2; */
    repl_feed_line_public("x = y * 2;");

    /* Store original source text */
    GLCmd *expr_cmd = NULL;
    for (int i = 0; i < repl_state_document_count(); i++) {
        GLCmd *cmd = &repl_state_document_cmds_mut()[i];
        if (cmd->type == CMD_VAR_ASSIGN && cmd->num_args == x_idx && cmd->has_vars) {
            expr_cmd = cmd;
            break;
        }
    }
    ASSERT_TRUE("expression assignment found", expr_cmd != NULL);

    char original_source[256];
    strcpy(original_source, expr_cmd->source);

    /* Drag x */
    repl_var_drag_begin(x_idx, 0, 100);
    repl_var_drag_motion(120);

    /* Expression-based assignment should NOT be updated */
    ASSERT_TRUE("source unchanged", strcmp(expr_cmd->source, original_source) == 0);
}

/* Test: sequential drags (begin -> motion -> reset -> begin) */
static void test_sequential_drags(void) {
    repl_reset_state();

    g_predef_vars[0].value = 5.0f;

    /* First drag */
    repl_var_drag_begin(0, 0, 100);
    repl_var_drag_motion(120);
    ASSERT_FLOAT("first drag result", g_predef_vars[0].value, 6.0f, 1e-5f);

    /* Reset */
    repl_var_drag_reset();
    ASSERT_INT("drag cleared", repl_var_drag_active(), 0);

    /* Second drag */
    g_predef_vars[0].value = 10.0f;
    repl_var_drag_begin(0, 0, 50);
    repl_var_drag_motion(100);  /* +50 pixels = +2.5 */
    ASSERT_FLOAT("second drag result", g_predef_vars[0].value, 12.5f, 1e-5f);
}

/* Test: switching between linear and log mode */
static void test_mode_consistency(void) {
    repl_reset_state();

    g_predef_vars[0].value = 1.0f;

    /* Begin in linear mode */
    repl_var_drag_begin(0, 0, 100);
    ASSERT_INT("linear mode set", repl_var_drag_log_mode(), 0);

    repl_var_drag_motion(120);
    float linear_result = g_predef_vars[0].value;

    /* Reset and try log mode */
    repl_var_drag_reset();
    g_predef_vars[0].value = 1.0f;

    repl_var_drag_begin(0, 1, 100);
    ASSERT_INT("log mode set", repl_var_drag_log_mode(), 1);

    repl_var_drag_motion(300);  /* +200 pixels */
    float log_result = g_predef_vars[0].value;

    /* Results should differ */
    ASSERT_TRUE("modes produce different results",
                fabsf(linear_result - log_result) > 0.1f);
}

/* Test: large positive motion */
static void test_large_positive_motion(void) {
    repl_reset_state();

    g_predef_vars[0].value = 1.0f;

    repl_var_drag_begin(0, 0, 0);
    repl_var_drag_motion(2000);  /* +2000 pixels = +100 units */

    float expected = 101.0f;
    ASSERT_FLOAT("large positive motion", g_predef_vars[0].value, expected, 1e-4f);
}

/* Test: large negative motion */
static void test_large_negative_motion(void) {
    repl_reset_state();

    g_predef_vars[0].value = 100.0f;

    repl_var_drag_begin(0, 0, 2000);
    repl_var_drag_motion(0);  /* -2000 pixels = -100 units */

    float expected = 0.0f;
    ASSERT_FLOAT("large negative motion", g_predef_vars[0].value, expected, 1e-4f);
}

/* Test: begin with log_mode != 0 and != 1 (normalization) */
static void test_log_mode_normalization(void) {
    repl_reset_state();

    g_predef_vars[0].value = 1.0f;

    repl_var_drag_begin(0, 99, 100);  /* Non-zero should be normalized to 1 */

    ASSERT_INT("log mode normalized to 1", repl_var_drag_log_mode(), 1);

    repl_var_drag_motion(300);  /* +200 pixels */
    /* Should behave as log mode */
    ASSERT_FLOAT("log behavior with 99", g_predef_vars[0].value, 10.0f, 0.1f);
}

/* Test: small motion in log mode */
static void test_log_small_motion(void) {
    repl_reset_state();

    g_predef_vars[0].value = 5.0f;

    repl_var_drag_begin(0, 1, 100);
    repl_var_drag_motion(110);  /* +10 pixels */

    /* Very small motion, result should be close to start but not exact */
    float result = g_predef_vars[0].value;
    ASSERT_TRUE("small log motion occurs", fabsf(result - 5.0f) > 1e-6f);
    ASSERT_TRUE("small log motion stays close", fabsf(result - 5.0f) < 1.0f);
}

/* Test: zero motion produces no change */
static void test_zero_motion(void) {
    repl_reset_state();

    g_predef_vars[0].value = 5.0f;

    repl_var_drag_begin(0, 0, 100);
    repl_var_drag_motion(100);  /* 0 pixels */

    ASSERT_FLOAT("zero motion no change", g_predef_vars[0].value, 5.0f, 1e-9f);
}

int main(void) {
    test_inactive_queries();
    test_begin_linear_mode();
    test_begin_log_mode();
    test_begin_invalid_negative_row();
    test_begin_invalid_row_oob();
    test_linear_motion_positive();
    test_linear_motion_negative();
    test_linear_motion_from_zero();
    test_log_motion_positive();
    test_log_motion_negative();
    test_log_motion_negative_value();
    test_log_motion_near_zero_bootstrap();
    test_motion_no_active_drag();
    test_reset();
    test_source_sync_literal_assign();
    test_source_sync_skip_expressions();
    test_sequential_drags();
    test_mode_consistency();
    test_large_positive_motion();
    test_large_negative_motion();
    test_log_mode_normalization();
    test_log_small_motion();
    test_zero_motion();

    printf("test_repl_var_drag: %d/%d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
