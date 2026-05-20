#include "editor/state.h"
#include "app/glr_ctrl.h"
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/state.h"
#include "repl/eval.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, actual, expected) \
    TEST_ASSERT_INT(&g_harness, label, actual, expected)

#define ASSERT_STR(label, actual, expected) \
    TEST_ASSERT_STR(&g_harness, label, actual, expected)

#define ASSERT_FALSE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, !(cond))

static GLCmd make_cmd(CmdType type, const char *source) {
    GLCmd cmd;
    (void)source;  /* text now lives in the editor buffer, not GLCmd */
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    return cmd;
}

static void test_repl_command_store_live(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_TRUE("live store has cmds pointer", store.cmds != NULL);
    ASSERT_TRUE("live store has count pointer", store.count != NULL);
    ASSERT_INT("live store has capacity", store.capacity, MAX_COMMANDS);
    /* No edit_line pointer post Phase 1 — cursor is caller-owned. */
}

static void test_repl_command_store_count(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("empty store count", repl_command_store_count(&store), 0);

    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");
    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    ASSERT_INT("store count after insert", repl_command_store_count(&store), 1);

    ASSERT_INT("count with NULL store", repl_command_store_count(NULL), 0);
}

static void test_repl_command_store_capacity(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("store capacity", repl_command_store_capacity(&store), MAX_COMMANDS);
    ASSERT_INT("capacity with NULL store", repl_command_store_capacity(NULL), 0);
}

static void test_repl_command_store_can_insert(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_TRUE("can insert 1 to empty store",
                repl_command_store_can_insert(&store, 1));
    ASSERT_TRUE("can insert many to empty store",
                repl_command_store_can_insert(&store, 100));
    ASSERT_TRUE("can insert 0 (no-op)",
                repl_command_store_can_insert(&store, 0));
    ASSERT_FALSE("cannot insert negative",
                 repl_command_store_can_insert(&store, -1));
    ASSERT_FALSE("cannot insert with NULL store",
                 repl_command_store_can_insert(NULL, 1));

    ReplCommandStore bad_store = {NULL, store.count, store.capacity};
    ASSERT_FALSE("cannot insert with NULL cmds",
                 repl_command_store_can_insert(&bad_store, 1));
}

static void test_repl_command_store_first_non_decl(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("empty store first_non_decl",
               repl_command_store_first_non_decl(&store), 0);

    GLCmd decl = make_cmd(CMD_VAR_DECLARE, "float x;");
    GLCmd vertex = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &decl, NULL);
    ASSERT_INT("first_non_decl with one decl",
               repl_command_store_first_non_decl(&store), 1);

    repl_command_store_insert_one(&store, 0, &decl, NULL);
    ASSERT_INT("first_non_decl with two decls",
               repl_command_store_first_non_decl(&store), 2);

    repl_command_store_insert_one(&store, 1, &vertex, NULL);
    ASSERT_INT("first_non_decl with decl then vertex",
               repl_command_store_first_non_decl(&store), 1);

    ASSERT_INT("first_non_decl with NULL store",
               repl_command_store_first_non_decl(NULL), 0);
}

static void test_repl_command_store_normalize_range(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");
    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);

    int out_start, out_count;
    ASSERT_INT("normalize valid range",
               repl_command_store_normalize_range(&store, 0, 2, &out_start, &out_count), 1);
    ASSERT_INT("normalized start", out_start, 0);
    ASSERT_INT("normalized count", out_count, 2);

    ASSERT_INT("normalize clamps overshoot",
               repl_command_store_normalize_range(&store, 1, 10, &out_start, &out_count), 1);
    ASSERT_INT("clamped count", out_count, 2);

    ASSERT_INT("normalize rejects negative start",
               repl_command_store_normalize_range(&store, -1, 1, &out_start, &out_count), 0);

    ASSERT_INT("normalize rejects start >= count",
               repl_command_store_normalize_range(&store, 3, 1, &out_start, &out_count), 0);

    ASSERT_INT("normalize rejects non-positive count",
               repl_command_store_normalize_range(&store, 0, 0, &out_start, &out_count), 0);

    ASSERT_INT("normalize rejects NULL store",
               repl_command_store_normalize_range(NULL, 0, 1, &out_start, &out_count), 0);
}

static void test_repl_command_store_insert_one(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");

    ASSERT_INT("insert to empty store",
               repl_command_store_insert_one(&store, 0, &cmd1, NULL), 1);
    ASSERT_INT("count after insert", repl_command_store_count(&store), 1);
    /* source text now lives in editor buffer; verify type only */
    ASSERT_INT("inserted cmd type", repl_state_document_cmd_at(0)->type, CMD_VERTEX3F);

    ASSERT_INT("insert at beginning with clamp",
               repl_command_store_insert_one(&store, -5, &cmd2, NULL), 1);
    ASSERT_INT("count after insert", repl_command_store_count(&store), 2);
    ASSERT_INT("first command type after insert",
               repl_state_document_cmd_at(0)->type, CMD_VERTEX3F);

    ASSERT_INT("insert rejects NULL store",
               repl_command_store_insert_one(NULL, 0, &cmd1, NULL), 0);

    ASSERT_INT("insert rejects NULL cmd",
               repl_command_store_insert_one(&store, 0, NULL, NULL), 0);
}

static void test_repl_command_store_insert_one_with_explicit_line(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "  glVertex3f(1, 0, 0);");
    const char *line = "glVertex3f(1, 0, 0)";

    ASSERT_INT("insert one with explicit line",
               repl_command_store_insert_one(&store, 0, &cmd, NULL), 1);
    editor_buffer_insert_line(0, line);
    ASSERT_STR("editor buffer uses explicit line",
               editor_buffer_line(0), "glVertex3f(1, 0, 0)");
}

static void test_repl_command_store_insert_many(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[3] = {
        make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);"),
        make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);"),
        make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 1);")
    };

    ASSERT_INT("insert many to empty",
               repl_command_store_insert_many(&store, 0, cmds, 3, NULL), 1);
    ASSERT_INT("count after insert many", repl_command_store_count(&store), 3);

    ASSERT_INT("insert many rejects NULL store",
               repl_command_store_insert_many(NULL, 0, cmds, 1, NULL), 0);

    ASSERT_INT("insert many rejects NULL cmds",
               repl_command_store_insert_many(&store, 0, NULL, 1, NULL), 0);

    ASSERT_INT("insert many rejects zero count",
               repl_command_store_insert_many(&store, 0, cmds, 0, NULL), 0);

    ASSERT_INT("insert many rejects negative count",
               repl_command_store_insert_many(&store, 0, cmds, -1, NULL), 0);
}

static void test_repl_command_store_insert_with_edit_line_adjustment(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");
    GLCmd cmd3 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 1);");

    repl_command_store_insert_one(&store, 0, &cmd1, NULL);

    /* Caller-owned cursor (Phase 1 of edit-line-ownership.md): the
     * store applies insert math to *opts.cursor_inout when the
     * ADJUST_EDIT_LINE flag is set. */
    int cur = 1;
    ReplStoreMutOpts adjust_opts = {
        .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
        .cursor_inout = &cur,
    };
    ASSERT_INT("insert at 0 with adjust flag",
               repl_command_store_insert_one(&store, 0, &cmd2, &adjust_opts), 1);
    ASSERT_INT("cursor adjusted after insert before", cur, 2);

    cur = 0;
    ReplStoreMutOpts no_adjust_opts = {
        .flags        = 0,
        .cursor_inout = &cur,
    };
    ASSERT_INT("insert before cursor without adjust flag",
               repl_command_store_insert_one(&store, 0, &cmd3, &no_adjust_opts), 1);
    ASSERT_INT("cursor not adjusted without flag", cur, 0);
}

static void test_repl_command_store_replace_one(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");

    repl_command_store_insert_one(&store, 0, &cmd1, NULL);
    editor_buffer_insert_line(0, "glVertex3f(1, 0, 0);");
    ASSERT_STR("before replace",
               editor_buffer_line(0), "glVertex3f(1, 0, 0);");

    ASSERT_INT("replace at valid index",
               repl_command_store_replace_one(&store, 0, &cmd2), 1);
    editor_buffer_replace_line(0, "glVertex3f(0, 1, 0);");
    ASSERT_STR("after replace",
               editor_buffer_line(0), "glVertex3f(0, 1, 0);");
    ASSERT_INT("count unchanged after replace", repl_command_store_count(&store), 1);

    ASSERT_INT("replace rejects NULL store",
               repl_command_store_replace_one(NULL, 0, &cmd1), 0);

    ASSERT_INT("replace rejects NULL cmd",
               repl_command_store_replace_one(&store, 0, NULL), 0);

    ASSERT_INT("replace rejects negative index",
               repl_command_store_replace_one(&store, -1, &cmd1), 0);

    ASSERT_INT("replace rejects out of bounds",
               repl_command_store_replace_one(&store, 1, &cmd1), 0);
}

static void test_repl_command_store_replace_one_with_explicit_line(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "  glVertex3f(0, 1, 0);");

    repl_command_store_insert_one(&store, 0, &cmd1, NULL);

    ASSERT_INT("replace with explicit line",
               repl_command_store_replace_one(&store, 0, &cmd2), 1);
    editor_buffer_replace_line(0, "glVertex3f(0, 1, 0)");
    ASSERT_STR("replace updates editor buffer line",
               editor_buffer_line(0), "glVertex3f(0, 1, 0)");
}

/* Regression: editor_buffer_replace_line must reject pos > line_count
 * rather than extending line_count over never-written zeroed gap lines
 * (the latent corruption the insert path already forecloses). */
static void test_editor_buffer_replace_line_rejects_gap(void) {
    glr_app_reset_all();

    editor_buffer_insert_line(0, "glVertex3f(1, 0, 0);");
    ASSERT_INT("one line present", editor_buffer_view().line_count, 1);

    ASSERT_INT("replace at pos > line_count rejected",
               editor_buffer_replace_line(5, "x"), 0);
    ASSERT_INT("rejected replace leaves line_count unchanged",
               editor_buffer_view().line_count, 1);

    ASSERT_INT("replace existing line ok",
               editor_buffer_replace_line(0, "glVertex3f(0, 1, 0);"), 1);
    ASSERT_STR("replace took effect",
               editor_buffer_line(0), "glVertex3f(0, 1, 0);");
    ASSERT_INT("append at pos == line_count ok",
               editor_buffer_replace_line(1, "glVertex3f(0, 0, 1);"), 1);
    ASSERT_INT("contiguous append grew line_count to 2",
               editor_buffer_view().line_count, 2);
}

static void test_repl_command_store_delete_range(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);

    ASSERT_INT("delete single command",
               repl_command_store_delete_range(&store, 0, 1, NULL), 1);
    ASSERT_INT("count after delete one", repl_command_store_count(&store), 2);

    ASSERT_INT("delete remaining range",
               repl_command_store_delete_range(&store, 0, 2, NULL), 1);
    ASSERT_INT("count after delete all", repl_command_store_count(&store), 0);

    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    ASSERT_INT("delete rejects invalid range",
               repl_command_store_delete_range(&store, 0, 0, NULL), 0);
}

static void test_repl_command_store_load(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[2] = {
        make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);"),
        make_cmd(CMD_COLOR3F, "glColor3f(1, 0, 0);")
    };
    const char *lines[2] = {
        "glVertex3f(1, 0, 0);",
        "glColor3f(1, 0, 0);"
    };

    /* _load no longer touches the cursor (Phase 1 of
     * plans/in-review/edit-line-ownership.md); cursor policy is
     * the caller's. */
    ASSERT_INT("load commands",
               repl_command_store_load(&store, cmds, 2), 1);
    editor_buffer_load_lines(lines, 2);
    ASSERT_INT("count after load", repl_command_store_count(&store), 2);
    ASSERT_STR("first loaded command",
               editor_buffer_line(0), "glVertex3f(1, 0, 0);");
    ASSERT_STR("second loaded command",
               editor_buffer_line(1), "glColor3f(1, 0, 0);");

    ASSERT_INT("second load succeeds",
               repl_command_store_load(&store, cmds, 2), 1);

    ASSERT_INT("load empty valid",
               repl_command_store_load(&store, NULL, 0), 1);
    ASSERT_INT("count after load empty", repl_command_store_count(&store), 0);

    ASSERT_INT("load rejects overflow",
               repl_command_store_load(&store, cmds, MAX_COMMANDS + 1), 0);

    ASSERT_INT("load rejects NULL cmds with count > 0",
               repl_command_store_load(&store, NULL, 1), 0);

    ASSERT_INT("load rejects NULL store",
               repl_command_store_load(NULL, cmds, 1), 0);
}

static void test_repl_command_store_load_with_explicit_lines(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[2] = {
        make_cmd(CMD_VERTEX3F, "  glVertex3f(1, 0, 0);"),
        make_cmd(CMD_COLOR3F, "  glColor3f(1, 0, 0);")
    };
    const char *lines[2] = {
        "glVertex3f(1, 0, 0)",
        "glColor3f(1, 0, 0)"
    };

    ASSERT_INT("load with explicit lines",
               repl_command_store_load(&store, cmds, 2), 1);
    editor_buffer_load_lines(lines, 2);
    ASSERT_STR("load line 0 preserved",
               editor_buffer_line(0), "glVertex3f(1, 0, 0)");
    ASSERT_STR("load line 1 preserved",
               editor_buffer_line(1), "glColor3f(1, 0, 0)");
}

static void test_repl_command_store_clear(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    repl_command_store_insert_one(&store, 1, &cmd, NULL);

    ASSERT_INT("count before clear", repl_command_store_count(&store), 2);
    repl_command_store_clear(&store);
    ASSERT_INT("count after clear", repl_command_store_count(&store), 0);

    repl_command_store_clear(NULL);
    ASSERT_INT("clear with NULL store is safe", 1, 1);
}

static void test_repl_command_store_delete_from_middle(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);

    ASSERT_INT("count before delete", repl_command_store_count(&store), 3);

    ASSERT_INT("delete from middle",
               repl_command_store_delete_range(&store, 1, 1, NULL), 1);
    ASSERT_INT("count after delete middle", repl_command_store_count(&store), 2);

    ASSERT_INT("delete from end",
               repl_command_store_delete_range(&store, 1, 1, NULL), 1);
    ASSERT_INT("count after delete end", repl_command_store_count(&store), 1);
}

static void test_repl_command_store_insert_at_end(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    ASSERT_INT("insert at position 0", repl_command_store_count(&store), 1);

    ASSERT_INT("insert at end (clamped from out of bounds)",
               repl_command_store_insert_one(&store, 100, &cmd, NULL), 1);
    ASSERT_INT("count after insert at end", repl_command_store_count(&store), 2);
}

/* Phase 1 of plans/in-review/edit-line-ownership.md: cursor-aware
 * delete is net-new store behavior. Verify the three cases in the
 * cursor-shift math (before / inside / past the deleted range). */
static void test_repl_command_store_delete_cursor_math(void) {
    glr_app_reset_all();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    /* Seed 5 commands: [0, 1, 2, 3, 4]. Delete [1..3) — count 2 —
     * leaves [0, 3, 4]. Cursors land per the three-case math. */
    repl_command_store_insert_one(&store, 0, &cmd, NULL);
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);
    repl_command_store_insert_one(&store, 3, &cmd, NULL);
    repl_command_store_insert_one(&store, 4, &cmd, NULL);

    /* Cursor before the deleted range — unchanged. */
    int cur_before = 0;
    ReplStoreMutOpts before_opts = { .flags = 0, .cursor_inout = &cur_before };
    ASSERT_INT("delete with cursor before range",
               repl_command_store_delete_range(&store, 1, 2, &before_opts), 1);
    ASSERT_INT("cursor before deleted range unchanged", cur_before, 0);
    ASSERT_INT("count after first delete", repl_command_store_count(&store), 3);

    /* Reset to 5 entries for the next case. */
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);
    ASSERT_INT("count back to 5", repl_command_store_count(&store), 5);

    /* Cursor inside the deleted range — snaps to range start. */
    int cur_inside = 2;
    ReplStoreMutOpts inside_opts = { .flags = 0, .cursor_inout = &cur_inside };
    ASSERT_INT("delete with cursor inside range",
               repl_command_store_delete_range(&store, 1, 2, &inside_opts), 1);
    ASSERT_INT("cursor inside snaps to range start", cur_inside, 1);

    /* Reset to 5 entries again. */
    repl_command_store_insert_one(&store, 1, &cmd, NULL);
    repl_command_store_insert_one(&store, 2, &cmd, NULL);
    ASSERT_INT("count back to 5 (again)", repl_command_store_count(&store), 5);

    /* Cursor past the deleted range — shifts left by count. */
    int cur_past = 4;
    ReplStoreMutOpts past_opts = { .flags = 0, .cursor_inout = &cur_past };
    ASSERT_INT("delete with cursor past range",
               repl_command_store_delete_range(&store, 1, 2, &past_opts), 1);
    ASSERT_INT("cursor past range shifted left by count", cur_past, 2);
}

int main(void) {
    repl_eval_init_predef_vars();

    test_repl_command_store_live();
    test_repl_command_store_count();
    test_repl_command_store_capacity();
    test_repl_command_store_can_insert();
    test_repl_command_store_first_non_decl();
    test_repl_command_store_normalize_range();
    test_repl_command_store_insert_one();
    test_repl_command_store_insert_one_with_explicit_line();
    test_repl_command_store_insert_many();
    test_repl_command_store_insert_with_edit_line_adjustment();
    test_repl_command_store_replace_one();
    test_repl_command_store_replace_one_with_explicit_line();
    test_editor_buffer_replace_line_rejects_gap();
    test_repl_command_store_delete_range();
    test_repl_command_store_load();
    test_repl_command_store_load_with_explicit_lines();
    test_repl_command_store_clear();
    test_repl_command_store_delete_from_middle();
    test_repl_command_store_insert_at_end();
    test_repl_command_store_delete_cursor_math();

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_command_store");
}
