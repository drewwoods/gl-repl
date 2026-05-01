#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_state.h"
#include "repl_eval.h"
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
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    strncpy(cmd.source, source, sizeof(cmd.source) - 1);
    cmd.source[sizeof(cmd.source) - 1] = '\0';
    return cmd;
}

static void test_repl_command_store_live(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_TRUE("live store has cmds pointer", store.cmds != NULL);
    ASSERT_TRUE("live store has count pointer", store.count != NULL);
    ASSERT_INT("live store has capacity", store.capacity, MAX_COMMANDS);
    ASSERT_TRUE("live store has edit_line pointer", store.edit_line != NULL);
}

static void test_repl_command_store_count(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("empty store count", repl_command_store_count(&store), 0);

    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");
    repl_command_store_insert_one(&store, 0, &cmd, 0);
    ASSERT_INT("store count after insert", repl_command_store_count(&store), 1);

    ASSERT_INT("count with NULL store", repl_command_store_count(NULL), 0);
}

static void test_repl_command_store_capacity(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("store capacity", repl_command_store_capacity(&store), MAX_COMMANDS);
    ASSERT_INT("capacity with NULL store", repl_command_store_capacity(NULL), 0);
}

static void test_repl_command_store_can_insert(void) {
    repl_reset_state();

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

    ReplCommandStore bad_store = {NULL, store.count, store.capacity, store.edit_line};
    ASSERT_FALSE("cannot insert with NULL cmds",
                 repl_command_store_can_insert(&bad_store, 1));
}

static void test_repl_command_store_first_non_decl(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    ASSERT_INT("empty store first_non_decl",
               repl_command_store_first_non_decl(&store), 0);

    GLCmd decl = make_cmd(CMD_VAR_DECLARE, "float x;");
    GLCmd vertex = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &decl, 0);
    ASSERT_INT("first_non_decl with one decl",
               repl_command_store_first_non_decl(&store), 1);

    repl_command_store_insert_one(&store, 0, &decl, 0);
    ASSERT_INT("first_non_decl with two decls",
               repl_command_store_first_non_decl(&store), 2);

    repl_command_store_insert_one(&store, 1, &vertex, 0);
    ASSERT_INT("first_non_decl with decl then vertex",
               repl_command_store_first_non_decl(&store), 1);

    ASSERT_INT("first_non_decl with NULL store",
               repl_command_store_first_non_decl(NULL), 0);
}

static void test_repl_command_store_normalize_range(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");
    repl_command_store_insert_one(&store, 0, &cmd, 0);
    repl_command_store_insert_one(&store, 1, &cmd, 0);
    repl_command_store_insert_one(&store, 2, &cmd, 0);

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
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");

    ASSERT_INT("insert to empty store",
               repl_command_store_insert_one(&store, 0, &cmd1, 0), 1);
    ASSERT_INT("count after insert", repl_command_store_count(&store), 1);
    ASSERT_STR("inserted source",
               repl_state_document_cmd_at(0)->source, "glVertex3f(1, 0, 0);");

    ASSERT_INT("insert at beginning with clamp",
               repl_command_store_insert_one(&store, -5, &cmd2, 0), 1);
    ASSERT_INT("count after insert", repl_command_store_count(&store), 2);
    ASSERT_STR("first command after insert",
               repl_state_document_cmd_at(0)->source, "glVertex3f(0, 1, 0);");

    ASSERT_INT("insert rejects NULL store",
               repl_command_store_insert_one(NULL, 0, &cmd1, 0), 0);

    ASSERT_INT("insert rejects NULL cmd",
               repl_command_store_insert_one(&store, 0, NULL, 0), 0);
}

static void test_repl_command_store_insert_many(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[3] = {
        make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);"),
        make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);"),
        make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 1);")
    };

    ASSERT_INT("insert many to empty",
               repl_command_store_insert_many(&store, 0, cmds, 3, 0), 1);
    ASSERT_INT("count after insert many", repl_command_store_count(&store), 3);

    ASSERT_INT("insert many rejects NULL store",
               repl_command_store_insert_many(NULL, 0, cmds, 1, 0), 0);

    ASSERT_INT("insert many rejects NULL cmds",
               repl_command_store_insert_many(&store, 0, NULL, 1, 0), 0);

    ASSERT_INT("insert many rejects zero count",
               repl_command_store_insert_many(&store, 0, cmds, 0, 0), 0);

    ASSERT_INT("insert many rejects negative count",
               repl_command_store_insert_many(&store, 0, cmds, -1, 0), 0);
}

static void test_repl_command_store_insert_with_edit_line_adjustment(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");
    GLCmd cmd3 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 1);");

    repl_command_store_insert_one(&store, 0, &cmd1, 0);
    *store.edit_line = 1;

    ASSERT_INT("edit line before insert with flag", *store.edit_line, 1);

    ASSERT_INT("insert at 0 with adjust flag",
               repl_command_store_insert_one(&store, 0, &cmd2,
                                             REPL_COMMAND_STORE_ADJUST_EDIT_LINE), 1);
    ASSERT_INT("edit line adjusted after insert before", *store.edit_line, 2);

    *store.edit_line = 0;
    ASSERT_INT("insert before edit_line without adjust",
               repl_command_store_insert_one(&store, 0, &cmd3, 0), 1);
    ASSERT_INT("edit line not adjusted without flag", *store.edit_line, 0);
}

static void test_repl_command_store_replace_one(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd1 = make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);");
    GLCmd cmd2 = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 1, 0);");

    repl_command_store_insert_one(&store, 0, &cmd1, 0);
    ASSERT_STR("before replace",
               repl_state_document_cmd_at(0)->source, "glVertex3f(1, 0, 0);");

    ASSERT_INT("replace at valid index",
               repl_command_store_replace_one(&store, 0, &cmd2), 1);
    ASSERT_STR("after replace",
               repl_state_document_cmd_at(0)->source, "glVertex3f(0, 1, 0);");
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

static void test_repl_command_store_delete_range(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, 0);
    repl_command_store_insert_one(&store, 1, &cmd, 0);
    repl_command_store_insert_one(&store, 2, &cmd, 0);

    ASSERT_INT("delete single command",
               repl_command_store_delete_range(&store, 0, 1), 1);
    ASSERT_INT("count after delete one", repl_command_store_count(&store), 2);

    ASSERT_INT("delete remaining range",
               repl_command_store_delete_range(&store, 0, 2), 1);
    ASSERT_INT("count after delete all", repl_command_store_count(&store), 0);

    repl_command_store_insert_one(&store, 0, &cmd, 0);
    ASSERT_INT("delete rejects invalid range",
               repl_command_store_delete_range(&store, 0, 0), 0);
}

static void test_repl_command_store_load(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[2] = {
        make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);"),
        make_cmd(CMD_COLOR3F, "glColor3f(1, 0, 0);")
    };

    ASSERT_INT("load commands",
               repl_command_store_load(&store, cmds, 2, 0), 1);
    ASSERT_INT("count after load", repl_command_store_count(&store), 2);
    ASSERT_STR("first loaded command",
               repl_state_document_cmd_at(0)->source, "glVertex3f(1, 0, 0);");
    ASSERT_STR("second loaded command",
               repl_state_document_cmd_at(1)->source, "glColor3f(1, 0, 0);");

    ASSERT_INT("load clamps edit_line to count",
               repl_state_edit_line(), 0);

    ASSERT_INT("load with edit_line > count",
               repl_command_store_load(&store, cmds, 2, 99), 1);
    ASSERT_INT("edit_line clamped", repl_state_edit_line(), 2);

    ASSERT_INT("load empty valid",
               repl_command_store_load(&store, NULL, 0, -5), 1);
    ASSERT_INT("count after load empty", repl_command_store_count(&store), 0);

    ASSERT_INT("load rejects overflow",
               repl_command_store_load(&store, cmds, MAX_COMMANDS + 1, 0), 0);

    ASSERT_INT("load rejects NULL cmds with count > 0",
               repl_command_store_load(&store, NULL, 1, 0), 0);

    ASSERT_INT("load rejects NULL store",
               repl_command_store_load(NULL, cmds, 1, 0), 0);
}

static void test_repl_command_store_clear(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, 0);
    repl_command_store_insert_one(&store, 1, &cmd, 0);

    ASSERT_INT("count before clear", repl_command_store_count(&store), 2);
    repl_command_store_clear(&store);
    ASSERT_INT("count after clear", repl_command_store_count(&store), 0);

    repl_command_store_clear(NULL);
    ASSERT_INT("clear with NULL store is safe", 1, 1);
}

static void test_repl_command_store_set_color(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd color_cmd = make_cmd(CMD_COLOR3F, "glColor3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &color_cmd, 0);

    ASSERT_INT("set color on CMD_COLOR3F",
               repl_command_store_set_color(0, 1, 0, 0, 0, 0), 1);

    GLCmd cmd4f = make_cmd(CMD_COLOR4F, "glColor4f(0, 0, 0, 0);");
    repl_command_store_insert_one(&store, 1, &cmd4f, 0);

    ASSERT_INT("set color on CMD_COLOR4F",
               repl_command_store_set_color(1, 0, 1, 0, 1, 1), 1);

    ASSERT_INT("set color rejects invalid command type",
               repl_command_store_set_color(0, 1, 1, 1, 1, 0), 1);

    ASSERT_INT("set color rejects out of bounds",
               repl_command_store_set_color(10, 1, 0, 0, 0, 0), 0);
}

static void test_repl_command_store_set_clear_color(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd clear_cmd = make_cmd(CMD_CLEAR_COLOR, "glClearColor(0, 0, 0, 1);");

    repl_command_store_insert_one(&store, 0, &clear_cmd, 0);

    ASSERT_INT("set clear color on CMD_CLEAR_COLOR",
               repl_command_store_set_clear_color(0, 0.5f, 0.5f, 0.5f, 1), 1);

    ASSERT_INT("set clear color rejects wrong command type",
               repl_command_store_set_clear_color(1, 0, 0, 0, 1), 0);

    ASSERT_INT("set clear color rejects out of bounds",
               repl_command_store_set_clear_color(10, 0, 0, 0, 1), 0);
}

static void test_repl_command_store_set_color_with_alpha(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd color4f = make_cmd(CMD_COLOR4F, "glColor4f(0, 0, 0, 1);");
    GLCmd color3f = make_cmd(CMD_COLOR3F, "glColor3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &color4f, 0);
    repl_command_store_insert_one(&store, 1, &color3f, 0);

    ASSERT_INT("set color4f with alpha",
               repl_command_store_set_color(0, 0.5f, 0.5f, 0.5f, 0.8f, 1), 1);
    ASSERT_INT("set color3f without alpha",
               repl_command_store_set_color(1, 0.5f, 0.5f, 0.5f, 0, 0), 1);
}

static void test_repl_command_store_tess_color(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd tess_color = make_cmd(CMD_TESS_COLOR, "gluColor(1, 1, 1);");

    repl_command_store_insert_one(&store, 0, &tess_color, 0);

    ASSERT_INT("set tess color with alpha",
               repl_command_store_set_color(0, 0.5f, 0.5f, 0.5f, 0.8f, 1), 1);
    ASSERT_INT("set tess color without alpha",
               repl_command_store_set_color(0, 0.3f, 0.3f, 0.3f, 0, 0), 1);
}

static void test_repl_command_store_delete_from_middle(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, 0);
    repl_command_store_insert_one(&store, 1, &cmd, 0);
    repl_command_store_insert_one(&store, 2, &cmd, 0);

    ASSERT_INT("count before delete", repl_command_store_count(&store), 3);

    ASSERT_INT("delete from middle",
               repl_command_store_delete_range(&store, 1, 1), 1);
    ASSERT_INT("count after delete middle", repl_command_store_count(&store), 2);

    ASSERT_INT("delete from end",
               repl_command_store_delete_range(&store, 1, 1), 1);
    ASSERT_INT("count after delete end", repl_command_store_count(&store), 1);
}

static void test_repl_command_store_insert_at_end(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmd = make_cmd(CMD_VERTEX3F, "glVertex3f(0, 0, 0);");

    repl_command_store_insert_one(&store, 0, &cmd, 0);
    ASSERT_INT("insert at position 0", repl_command_store_count(&store), 1);

    ASSERT_INT("insert at end (clamped from out of bounds)",
               repl_command_store_insert_one(&store, 100, &cmd, 0), 1);
    ASSERT_INT("count after insert at end", repl_command_store_count(&store), 2);
}

static void test_repl_command_store_load_with_negative_edit_line(void) {
    repl_reset_state();

    ReplCommandStore store = repl_command_store_live();
    GLCmd cmds[2] = {
        make_cmd(CMD_VERTEX3F, "glVertex3f(1, 0, 0);"),
        make_cmd(CMD_COLOR3F, "glColor3f(1, 0, 0);")
    };

    ASSERT_INT("load with negative edit_line",
               repl_command_store_load(&store, cmds, 2, -10), 1);
    ASSERT_INT("edit_line clamped to 0", repl_state_edit_line(), 0);
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
    test_repl_command_store_insert_many();
    test_repl_command_store_insert_with_edit_line_adjustment();
    test_repl_command_store_replace_one();
    test_repl_command_store_delete_range();
    test_repl_command_store_load();
    test_repl_command_store_clear();
    test_repl_command_store_set_color();
    test_repl_command_store_set_clear_color();
    test_repl_command_store_set_color_with_alpha();
    test_repl_command_store_tess_color();
    test_repl_command_store_delete_from_middle();
    test_repl_command_store_insert_at_end();
    test_repl_command_store_load_with_negative_edit_line();

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_command_store");
}
