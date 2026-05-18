#include "app/glr_ctrl.h"
#include "widgets/variable_panel_drag.h"
#include "widgets/variable_panel_state.h"
#include "repl/state.h"
#include "repl/core.h"
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

static void test_inactive_queries(void) {
    glr_app_reset_all();

    ASSERT_INT("no drag active", variable_panel_drag_active(), 0);
    ASSERT_INT("active var is -1", variable_panel_drag_active_var(), -1);
    ASSERT_INT("log mode is 0", variable_panel_drag_log_mode(), 0);
    ASSERT_INT("undo snapshot flag clear", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_begin_captures_drag_metadata(void) {
    EditorVariableDragState drag;

    glr_app_reset_all();

    g_predef_vars[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 0, 100);
    drag = variable_panel_drag();

    ASSERT_INT("drag active", variable_panel_drag_active(), 1);
    ASSERT_INT("active var stored", variable_panel_drag_active_var(), 0);
    ASSERT_INT("linear mode stored", variable_panel_drag_log_mode(), 0);
    ASSERT_STR("drag name stored", drag.name, g_predef_vars[0].name);
    ASSERT_INT("undo flag reset on begin", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_begin_invalid_rows_leave_drag_inactive(void) {
    glr_app_reset_all();

    variable_panel_handle_drag_begin(-1, 0, 100);
    ASSERT_INT("negative row ignored", variable_panel_drag_active(), 0);

    variable_panel_handle_drag_begin(g_num_predef_vars + 10, 0, 100);
    ASSERT_INT("oob row ignored", variable_panel_drag_active(), 0);
}

static void test_linear_motion_emits_request_without_mutation(void) {
    VariablePanelValueChange change;

    glr_app_reset_all();
    g_predef_vars[0].value = 5.0f;

    variable_panel_handle_drag_begin(0, 0, 100);
    ASSERT_INT("linear motion emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    ASSERT_STR("linear request name", change.name, g_predef_vars[0].name);
    ASSERT_FLOAT("linear request value", change.value, 6.0f, 1e-5f);
    ASSERT_FLOAT("linear motion does not mutate live value",
                 g_predef_vars[0].value, 5.0f, 1e-5f);
}

static void test_log_motion_emits_request_without_mutation(void) {
    VariablePanelValueChange change;

    glr_app_reset_all();
    g_predef_vars[0].value = 10.0f;

    variable_panel_handle_drag_begin(0, 1, 300);
    ASSERT_INT("log motion emits request",
               variable_panel_handle_drag_motion(100, &change), 1);
    ASSERT_FLOAT("log request value", change.value, 1.0f, 0.01f);
    ASSERT_FLOAT("log motion does not mutate live value",
                 g_predef_vars[0].value, 10.0f, 1e-5f);
}

static void test_log_near_zero_bootstrap_emits_request(void) {
    VariablePanelValueChange change;

    glr_app_reset_all();
    g_predef_vars[0].value = 1e-7f;

    variable_panel_handle_drag_begin(0, 1, 100);
    ASSERT_INT("near-zero log motion emits request",
               variable_panel_handle_drag_motion(140, &change), 1);
    ASSERT_FLOAT("near-zero bootstrap value", change.value, 0.04f, 1e-5f);
    ASSERT_FLOAT("near-zero motion does not mutate live value",
                 g_predef_vars[0].value, 1e-7f, 1e-8f);
}

static void test_motion_without_active_drag_is_noop(void) {
    VariablePanelValueChange change;

    glr_app_reset_all();
    g_predef_vars[0].value = 5.0f;
    snprintf(change.name, sizeof(change.name), "%s", "stale");
    change.value = 99.0f;

    ASSERT_INT("motion without drag returns 0",
               variable_panel_handle_drag_motion(200, &change), 0);
    ASSERT_FLOAT("motion without drag keeps live value",
                 g_predef_vars[0].value, 5.0f, 1e-9f);
}

static void test_reset_clears_drag_state_and_undo_flag(void) {
    glr_app_reset_all();

    g_predef_vars[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 1, 100);
    variable_panel_drag_mark_undo_snapshot_pushed();
    ASSERT_INT("undo flag set before reset", variable_panel_drag_undo_snapshot_pushed(), 1);

    variable_panel_handle_drag_reset();

    ASSERT_INT("drag inactive after reset", variable_panel_drag_active(), 0);
    ASSERT_INT("active var cleared after reset", variable_panel_drag_active_var(), -1);
    ASSERT_INT("log mode cleared after reset", variable_panel_drag_log_mode(), 0);
    ASSERT_INT("undo flag cleared after reset", variable_panel_drag_undo_snapshot_pushed(), 0);
}

static void test_request_uses_dragged_variable_name(void) {
    VariablePanelValueChange change;
    int x_idx;

    glr_app_reset_all();
    editor_feed_line("float x;");
    x_idx = repl_eval_find_predef_var_idx("x");
    ASSERT_TRUE("x declared", x_idx >= 0);

    g_predef_vars[x_idx].value = 3.0f;
    variable_panel_handle_drag_begin(x_idx, 0, 100);
    ASSERT_INT("named drag emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    ASSERT_STR("named request uses x", change.name, "x");
    ASSERT_FLOAT("named request value", change.value, 4.0f, 1e-5f);
}

static void test_sequential_drags_reanchor_to_new_start_value(void) {
    VariablePanelValueChange change;

    glr_app_reset_all();

    g_predef_vars[0].value = 5.0f;
    variable_panel_handle_drag_begin(0, 0, 100);
    ASSERT_INT("first drag emits request",
               variable_panel_handle_drag_motion(120, &change), 1);
    ASSERT_FLOAT("first drag request", change.value, 6.0f, 1e-5f);

    variable_panel_handle_drag_reset();
    g_predef_vars[0].value = 10.0f;
    variable_panel_handle_drag_begin(0, 0, 50);
    ASSERT_INT("second drag emits request",
               variable_panel_handle_drag_motion(100, &change), 1);
    ASSERT_FLOAT("second drag reanchors to new start value",
                 change.value, 12.5f, 1e-5f);
}

int main(void) {
    test_inactive_queries();
    test_begin_captures_drag_metadata();
    test_begin_invalid_rows_leave_drag_inactive();
    test_linear_motion_emits_request_without_mutation();
    test_log_motion_emits_request_without_mutation();
    test_log_near_zero_bootstrap_emits_request();
    test_motion_without_active_drag_is_noop();
    test_reset_clears_drag_state_and_undo_flag();
    test_request_uses_dragged_variable_name();
    test_sequential_drags_reanchor_to_new_start_value();

    printf("test_repl_var_drag: %d/%d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
