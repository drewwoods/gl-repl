/*
 * test_console.c - Unit tests for console trace line subsystem.
 */
#include "subsystems/console/console.h"
#include "repl/text_helpers.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp)   TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_STR(label, got, exp)   TEST_ASSERT_STR(&g_harness, label, got, exp)

static void test_initial_state(void) {
    console_reset();
    ASSERT_INT("initially closed", console_is_open(), 0);
    ConsoleView view = console_view();
    ASSERT_INT("init count is 0", view.count, 0);
    ASSERT_INT("init total_count is 0", view.total_count, 0);
    ASSERT_INT("init overflow_count is 0", view.overflow_count, 0);
}

static void test_open_close_toggle(void) {
    console_reset();
    console_open();
    ASSERT_INT("open", console_is_open(), 1);
    console_close();
    ASSERT_INT("close", console_is_open(), 0);
    console_toggle();
    ASSERT_INT("toggle 1", console_is_open(), 1);
    console_toggle();
    ASSERT_INT("toggle 2", console_is_open(), 0);
    console_set_open(1);
    ASSERT_INT("set_open 1", console_is_open(), 1);
    console_set_open(0);
    ASSERT_INT("set_open 0", console_is_open(), 0);
}

static void test_capture_when_closed(void) {
    console_reset();
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_CONSOLE;
    cmd.valid = 1;
    cmd.num_args = 1;
    cmd.args[0] = 42.0f;
    snprintf(cmd.payload.label.fmt, sizeof(cmd.payload.label.fmt), "%s", "value=%f");

    console_capture(&cmd, 1, 1);
    ConsoleView view = console_view();
    ASSERT_INT("closed capture ignores", view.count, 0);
}

static void test_capture_formatting_and_indentation(void) {
    console_reset();
    console_open();

    GLCmd cmds[4];
    memset(cmds, 0, sizeof(cmds));

    /* cmd 0: depth 0 */
    cmds[0].type = CMD_CONSOLE;
    cmds[0].valid = 1;
    cmds[0].call_depth = 0;
    cmds[0].num_args = 2;
    cmds[0].args[0] = 1.5f;
    cmds[0].args[1] = 2.5f;
    snprintf(cmds[0].payload.label.fmt, sizeof(cmds[0].payload.label.fmt), "%s", "x=%f, y=%f");

    /* cmd 1: depth 1 (2 spaces indent) */
    cmds[1].type = CMD_CONSOLE;
    cmds[1].valid = 1;
    cmds[1].call_depth = 1;
    cmds[1].num_args = 1;
    cmds[1].args[0] = 100.0f;
    snprintf(cmds[1].payload.label.fmt, sizeof(cmds[1].payload.label.fmt), "%s", "sub %% 100=%f%%");

    /* cmd 2: depth 3 (6 spaces indent) with 8 args */
    cmds[2].type = CMD_CONSOLE;
    cmds[2].valid = 1;
    cmds[2].call_depth = 3;
    cmds[2].num_args = 8;
    for (int i = 0; i < 8; i++) cmds[2].args[i] = (float)(i + 1);
    snprintf(cmds[2].payload.label.fmt, sizeof(cmds[2].payload.label.fmt), "%s",
             "%f %f %f %f %f %f %f %f");

    /* cmd 3: non-console command (should be skipped) */
    cmds[3].type = CMD_VERTEX3F;
    cmds[3].valid = 1;

    console_capture(cmds, 4, 4);
    ConsoleView view = console_view();
    ASSERT_INT("captured 3 console lines", view.count, 3);
    ASSERT_INT("total count is 3", view.total_count, 3);
    ASSERT_INT("overflow count is 0", view.overflow_count, 0);

    ASSERT_STR("line 0 text", view.lines[0].text, "x=1.5, y=2.5");
    ASSERT_INT("line 0 depth", view.lines[0].call_depth, 0);

    ASSERT_STR("line 1 text", view.lines[1].text, "  sub % 100=100%");
    ASSERT_INT("line 1 depth", view.lines[1].call_depth, 1);

    ASSERT_STR("line 2 text", view.lines[2].text, "      1 2 3 4 5 6 7 8");
    ASSERT_INT("line 2 depth", view.lines[2].call_depth, 3);
}

static void test_exec_limit_clamping(void) {
    console_reset();
    console_open();

    GLCmd cmds[3];
    memset(cmds, 0, sizeof(cmds));
    for (int i = 0; i < 3; i++) {
        cmds[i].type = CMD_CONSOLE;
        cmds[i].valid = 1;
        cmds[i].call_depth = 0;
        cmds[i].num_args = 1;
        cmds[i].args[0] = (float)(i + 1);
        snprintf(cmds[i].payload.label.fmt, sizeof(cmds[i].payload.label.fmt), "%s", "step %f");
    }

    /* Clamped to first 2 */
    console_capture(cmds, 3, 2);
    ConsoleView view = console_view();
    ASSERT_INT("exec limit clamped count to 2", view.count, 2);
    ASSERT_STR("line 0", view.lines[0].text, "step 1");
    ASSERT_STR("line 1", view.lines[1].text, "step 2");

    /* Clamped to 0 */
    console_capture(cmds, 3, 0);
    view = console_view();
    ASSERT_INT("exec limit 0 gives 0 lines", view.count, 0);
}

static void test_overflow_handling(void) {
    console_reset();
    console_open();

    GLCmd cmds[MAX_CONSOLE_LINES + 50];
    memset(cmds, 0, sizeof(cmds));
    for (int i = 0; i < MAX_CONSOLE_LINES + 50; i++) {
        cmds[i].type = CMD_CONSOLE;
        cmds[i].valid = 1;
        cmds[i].call_depth = 0;
        cmds[i].num_args = 1;
        cmds[i].args[0] = (float)i;
        snprintf(cmds[i].payload.label.fmt, sizeof(cmds[i].payload.label.fmt), "%s", "line %f");
    }

    console_capture(cmds, MAX_CONSOLE_LINES + 50, MAX_CONSOLE_LINES + 50);
    ConsoleView view = console_view();
    ASSERT_INT("count capped at MAX_CONSOLE_LINES", view.count, MAX_CONSOLE_LINES);
    ASSERT_INT("total count recorded", view.total_count, MAX_CONSOLE_LINES + 50);
    ASSERT_INT("overflow count is 50", view.overflow_count, 50);
}

int main(void) {
    test_initial_state();
    test_open_close_toggle();
    test_capture_when_closed();
    test_capture_formatting_and_indentation();
    test_exec_limit_clamping();
    test_overflow_handling();

    return test_harness_report(&g_harness, "test_console");
}
