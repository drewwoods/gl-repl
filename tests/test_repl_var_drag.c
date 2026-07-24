#include "app/glr_ctrl.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"
#include "repl/state.h"

#include "editor/input.h"
#include "repl/eval.h"
#include "support/test_harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_FLOAT(label, got, exp, tol) do { \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, tol); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

/* The drag handlers read the dragged variable's name+value through an
 * installed value source (so the peer needn't reach the eval table directly).
 * Back it with the REPL eval table for these tests. */
static int test_var_read_row(int row, char *name_out, int name_cap, float *value_out) {
    ReplPredefView predef = repl_eval_predef_view();
    if (row < 0 || row >= predef.count) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", predef.vars[row].name);
    *value_out = predef.vars[row].value;
    return 1;
}
static const VariablePanelValueSource g_test_value_source = { test_var_read_row };

static void test_inactive_queries(void) {
    glr_ctrl_reset_all();

    ASSERT_INT("no drag active", variable_panel_drag_active(), 0);
    ASSERT_INT("active var is -1", variable_panel_drag_active_var(), -1);
    ASSERT_INT("coarse mode is 0", variable_panel_drag_coarse(), 0);
    ASSERT_INT("undo snapshot flag clear", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_begin_captures_drag_metadata(void) {
    VariablePanelDragState drag;

    glr_ctrl_reset_all();

    g_predef_vars_mut[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 0, 100);
    drag = variable_panel_drag();

    ASSERT_INT("drag active", variable_panel_drag_active(), 1);
    ASSERT_INT("active var stored", variable_panel_drag_active_var(), 0);
    ASSERT_INT("normal (non-coarse) mode stored", variable_panel_drag_coarse(), 0);
    ASSERT_STR("drag name stored", drag.name, g_predef_vars[0].name);
    ASSERT_INT("undo flag reset on begin", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_begin_invalid_rows_leave_drag_inactive(void) {
    glr_ctrl_reset_all();

    variable_panel_handle_drag_begin(-1, 0, 100);
    ASSERT_INT("negative row ignored", variable_panel_drag_active(), 0);

    variable_panel_handle_drag_begin(g_num_predef_vars + 10, 0, 100);
    ASSERT_INT("oob row ignored", variable_panel_drag_active(), 0);
}

static void test_linear_motion_emits_request_without_mutation(void) {
    VariablePanelValueChange change = {0};

    glr_ctrl_reset_all();
    g_predef_vars_mut[0].value = 5.0f;

    variable_panel_handle_drag_begin(0, 0, 100);
    ASSERT_INT("linear motion emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    ASSERT_STR("linear request name", change.name, g_predef_vars[0].name);
    /* dx = 20 px at 0.1 units/px = +2.0. */
    ASSERT_FLOAT("linear request value", change.value, 7.0f, 1e-5f);
    ASSERT_FLOAT("linear motion does not mutate live value",
                 g_predef_vars[0].value, 5.0f, 1e-5f);
}

/* The coarse (right-click) scrub is the same linear scrub at
 * GLR_ADJUST_COARSE_SCALE (10x) the units-per-pixel. */
static void test_coarse_motion_is_ten_times_linear(void) {
    VariablePanelValueChange change = {0};

    glr_ctrl_reset_all();
    g_predef_vars_mut[0].value = 5.0f;

    variable_panel_handle_drag_begin(0, /*coarse=*/1, 100);
    ASSERT_INT("coarse motion emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    /* dx = 20 px at 0.1 * 10 = 1.0 units/px = +20.0 (10x the linear +2.0). */
    ASSERT_FLOAT("coarse request value", change.value, 25.0f, 1e-5f);
    ASSERT_FLOAT("coarse motion does not mutate live value",
                 g_predef_vars[0].value, 5.0f, 1e-5f);
}

/* Pure linear means the coarse scrub walks off zero with no special-casing. */
static void test_coarse_from_zero_emits_request(void) {
    VariablePanelValueChange change = {0};

    glr_ctrl_reset_all();
    g_predef_vars_mut[0].value = 0.0f;

    variable_panel_handle_drag_begin(0, /*coarse=*/1, 100);
    ASSERT_INT("coarse-from-zero motion emits request",
               variable_panel_handle_drag_motion(140, &change), 1);
    /* dx = 40 px at 1.0 units/px = +40.0. */
    ASSERT_FLOAT("coarse-from-zero value", change.value, 40.0f, 1e-5f);
    ASSERT_FLOAT("coarse-from-zero motion does not mutate live value",
                 g_predef_vars[0].value, 0.0f, 1e-5f);
}

static void test_motion_without_active_drag_is_noop(void) {
    VariablePanelValueChange change = {0};

    glr_ctrl_reset_all();
    g_predef_vars_mut[0].value = 5.0f;
    snprintf(change.name, sizeof(change.name), "%s", "stale");
    change.value = 99.0f;

    ASSERT_INT("motion without drag returns 0",
               variable_panel_handle_drag_motion(200, &change), 0);
    ASSERT_FLOAT("motion without drag keeps live value",
                 g_predef_vars[0].value, 5.0f, 1e-9f);
}

static void test_reset_clears_drag_state_and_undo_flag(void) {
    glr_ctrl_reset_all();

    g_predef_vars_mut[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 1, 100);
    variable_panel_drag_mark_undo_snapshot_pushed();
    ASSERT_INT("undo flag set before reset", variable_panel_drag_undo_snapshot_pushed(), 1);

    variable_panel_handle_drag_reset();

    ASSERT_INT("drag inactive after reset", variable_panel_drag_active(), 0);
    ASSERT_INT("active var cleared after reset", variable_panel_drag_active_var(), -1);
    ASSERT_INT("coarse mode cleared after reset", variable_panel_drag_coarse(), 0);
    ASSERT_INT("undo flag cleared after reset", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_request_uses_dragged_variable_name(void) {
    VariablePanelValueChange change = {0};
    int x_idx;

    glr_ctrl_reset_all();
    editor_feed_line("float x;");
    x_idx = repl_eval_find_predef_var_idx("x");
    ASSERT_TRUE("x declared", x_idx >= 0);

    g_predef_vars_mut[x_idx].value = 3.0f;
    variable_panel_handle_drag_begin(x_idx, 0, 100);
    ASSERT_INT("named drag emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    ASSERT_STR("named request uses x", change.name, "x");
    /* dx = 20 px at 0.1 units/px = +2.0. */
    ASSERT_FLOAT("named request value", change.value, 5.0f, 1e-5f);
}

static void test_sequential_drags_reanchor_to_new_start_value(void) {
    VariablePanelValueChange change = {0};

    glr_ctrl_reset_all();

    g_predef_vars_mut[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 0, 100);
    ASSERT_INT("first drag emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    /* dx = 20 px at 0.1 units/px = +2.0. */
    ASSERT_FLOAT("first drag request", change.value, 7.0f, 1e-5f);

    variable_panel_handle_drag_reset();
    g_predef_vars_mut[0].value = 10.0f;
    variable_panel_handle_drag_begin(0, 0, 50);
    ASSERT_INT("second drag emits request",
               variable_panel_handle_drag_motion(100, &change), 1);
    /* dx = 50 px at 0.1 units/px = +5.0, anchored to the new start. */
    ASSERT_FLOAT("second drag reanchors to new start value",
                 change.value, 15.0f, 1e-5f);
}

int main(void) {
    variable_panel_install_value_source(&g_test_value_source);
    test_inactive_queries();
    test_begin_captures_drag_metadata();
    test_begin_invalid_rows_leave_drag_inactive();
    test_linear_motion_emits_request_without_mutation();
    test_coarse_motion_is_ten_times_linear();
    test_coarse_from_zero_emits_request();
    test_motion_without_active_drag_is_noop();
    test_reset_clears_drag_state_and_undo_flag();
    test_request_uses_dragged_variable_name();
    test_sequential_drags_reanchor_to_new_start_value();

    printf("test_repl_var_drag: %d/%d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
