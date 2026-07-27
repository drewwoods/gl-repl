#include "app/glr_ctrl.h"
/*
 * test_src/repl/compile.c - Phase C invariant tests.
 *
 * Verifies the compile/apply boundary established in Phase C
 * commits 19-21:
 *
 *   - repl_compile_*() never mutates editor buffer, command store,
 *     status, or undo on either success or failure.
 *   - apply (driven by editor_commit_apply_external_change) updates
 *     editor text and command store together.
 *   - On compile failure, set_status from the wrapper IS allowed
 *     (Phase C transition); the strong invariant is checked at the
 *     compile-function boundary itself.
 */

#include "editor/commit.h"
#include "editor/reformat.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/apply.h"
#include "repl/command_store.h"
#include "repl/compile.h"
#include <stdbool.h>
#include "editor/input.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/load.h"           /* repl_load_apply_line for [P2] dup-check test */
#include "repl/state_owners.h"
#include "source_document.h"
#include "ui/app/state.h"
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
#define ASSERT_FLOAT(label, actual, expected, tolerance) \
    TEST_ASSERT_FLOAT(&g_harness, label, actual, expected, tolerance)

/* Fill the input buffer the compile entry points read from. */
static void set_input(const char *s) {
    editor_input_set_text(s ? s : "");
}

/* Snapshot of the four mutation surfaces compile must NOT touch. */
typedef struct {
    int  cmd_count;
    int  edit_line;
    int  insert_mode;
    int  num_predef_vars;
    char status_text[REPL_STATUS_TEXT_MAX];
    int  status_ttl;
    int  buffer_count;
    char first_line[MAX_LINE_LEN];
} ComputeFingerprint;

static ComputeFingerprint capture_fingerprint(void) {
    ComputeFingerprint fp = {0};
    fp.cmd_count       = repl_state_document_count();
    fp.edit_line       = editor_state_edit_line();
    fp.insert_mode     = editor_insert_mode();
    fp.num_predef_vars = g_num_predef_vars;

    UiStatusState st = ui_state_status();
    strncpy(fp.status_text, st.text, sizeof(fp.status_text) - 1);
    fp.status_text[sizeof(fp.status_text) - 1] = '\0';
    fp.status_ttl = st.ttl;

    fp.buffer_count = editor_buffer_count();
    const char *line0 = editor_buffer_line(0);
    strncpy(fp.first_line, line0 ? line0 : "", sizeof(fp.first_line) - 1);
    fp.first_line[sizeof(fp.first_line) - 1] = '\0';

    return fp;
}

static int fingerprint_equal(const ComputeFingerprint *a,
                             const ComputeFingerprint *b) {
    return a->cmd_count       == b->cmd_count       &&
           a->edit_line       == b->edit_line       &&
           a->insert_mode     == b->insert_mode     &&
           a->num_predef_vars == b->num_predef_vars &&
           a->buffer_count    == b->buffer_count    &&
           a->status_ttl      == b->status_ttl      &&
           strcmp(a->status_text, b->status_text) == 0 &&
           strcmp(a->first_line, b->first_line) == 0;
}

/* ---- Tests --------------------------------------------------------- */

/* repl_compile_float_decl is pure on the failure path. */
static void test_compile_float_decl_failure_is_pure(void) {
    glr_ctrl_reset_all();

    /* Establish a non-trivial pre-state: declare a variable, set
     * status, push a buffer line. */
    set_input("float existing;");
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float existing;", &ctx, &change, err, sizeof(err));
    /* Apply the success change so the state is non-trivial. */
    editor_commit_apply_external_change(&change, 0, 0);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Trigger compile failure: redeclaring an existing name. */
    set_input("float existing;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompileResult r = repl_compile_float_decl(
        "float existing;", &ctx, &change, err, sizeof(err));
    ASSERT_INT("redeclare returns ERROR", r, REPL_COMPILE_ERROR);
    ASSERT_TRUE("redeclare fills err", err[0] != '\0');

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("compile failure leaves state untouched (cmd_count, "
                "buffer, status, predef_vars)",
                fingerprint_equal(&before, &after));
}

/* A float decl can carry a trailing `// comment` even when the input
 * has no semicolon — the interactive case, since the `;` key commits
 * before a comment can be typed. Mirrors the var-assign `//` handling.
 * The comment is the vehicle for the `@tune` knob tag, so this is what
 * lets an interactively-declared variable be tagged tunable. */
static void test_compile_float_decl_trailing_comment_no_semicolon(void) {
    glr_ctrl_reset_all();

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    /* init + comment, no `;` */
    ReplCompileResult r = repl_compile_float_decl(
        "float n = 1 // @tune", &ctx, &change, err, sizeof(err));
    ASSERT_INT("decl init+comment compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("decl init+comment is INSERT_ONE", change.kind,
               REPL_COMPILED_INSERT_ONE);
    ASSERT_STR("decl init+comment text",
               change.text[0], "  static float n = 1; // @tune");
    ASSERT_TRUE("decl init+comment text is @tune-tagged",
                repl_eval_line_has_tune_tag(change.text[0]));

    /* no init, comment only, no `;` */
    glr_ctrl_reset_all();
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    r = repl_compile_float_decl(
        "float m // @tune knob", &ctx, &change, err, sizeof(err));
    ASSERT_INT("decl no-init comment compile OK", r, REPL_COMPILE_OK);
    ASSERT_STR("decl no-init comment text",
               change.text[0], "  static float m; // @tune knob");

    /* multi-name with init + comment, no `;` */
    glr_ctrl_reset_all();
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    r = repl_compile_float_decl(
        "float a = 1, b // @tune", &ctx, &change, err, sizeof(err));
    ASSERT_INT("decl multi+comment compile OK", r, REPL_COMPILE_OK);
    ASSERT_STR("decl multi+comment text",
               change.text[0], "  static float a = 1, b; // @tune");

    /* A single `/` (division) in the initializer is NOT a comment. */
    glr_ctrl_reset_all();
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    r = repl_compile_float_decl(
        "float h = 1/2 // @tune", &ctx, &change, err, sizeof(err));
    ASSERT_INT("decl division+comment compile OK", r, REPL_COMPILE_OK);
    ASSERT_STR("decl division initializer kept, comment split",
               change.text[0], "  static float h = 0.5; // @tune");
}

/* Adding a comment to an existing decl that had none round-trips
 * through the editor. After editor_load_line_to_input the loaded
 * buffer has the trailing `;` stripped, so appending ` // @tune`
 * yields a no-semicolon input that overwrites the decl in place. */
static void test_float_decl_add_comment_to_existing(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float n = 1;");
    ASSERT_STR("baseline decl has no comment",
               editor_buffer_line(0), "  static float n = 1;");

    /* Re-commit the same decl with a comment appended, as the editor
     * would after loading the line (no trailing `;` in the input). */
    set_input("static float n = 1 // @tune");
    ReplCompileContext ctx = repl_compile_context_from_live(0);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileResult r = repl_compile_float_decl(
        "static float n = 1 // @tune", &ctx, &change, err, sizeof(err));
    ASSERT_INT("add-comment compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("add-comment replaces in place", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("add-comment apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("existing decl now carries the comment",
               editor_buffer_line(0), "  static float n = 1; // @tune");
    ASSERT_TRUE("existing decl is now @tune-tagged",
                repl_eval_line_has_tune_tag(editor_buffer_line(0)));
}

/* repl_compile_split_decl turns `float a, b, c;` into one decl per line,
 * in place, without touching the predef table (the vars stay declared). */
static void test_split_decl_basic(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float grid, extent, x;");
    ASSERT_STR("baseline multi-name decl",
               editor_buffer_line(0), "  static float grid, extent, x;");
    int predefs_before = g_num_predef_vars;

    ReplCompileContext ctx = repl_compile_context_from_live(0);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileResult r = repl_compile_split_decl(&ctx, 0, &change, err, sizeof(err));
    ASSERT_INT("split compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("split is INSERT_MANY", change.kind, REPL_COMPILED_INSERT_MANY);
    ASSERT_INT("split inserts 3", change.count, 3);
    ASSERT_INT("split deletes the old line", change.delete_count, 1);
    ASSERT_INT("split delete pos", change.delete_pos, 0);
    ASSERT_INT("split emits no predef ops", change.predef_op_count, 0);
    ASSERT_STR("split line 0", change.text[0], "  static float grid;");
    ASSERT_STR("split line 1", change.text[1], "  static float extent;");
    ASSERT_STR("split line 2", change.text[2], "  static float x;");

    ASSERT_INT("split apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("document now has 3 decl lines", repl_state_document_count(), 3);
    ASSERT_STR("applied line 0", editor_buffer_line(0), "  static float grid;");
    ASSERT_STR("applied line 2", editor_buffer_line(2), "  static float x;");
    ASSERT_INT("predef count unchanged by split", g_num_predef_vars, predefs_before);
    ASSERT_TRUE("grid still declared", repl_eval_find_predef_var_idx("grid") >= 0);
    ASSERT_TRUE("extent still declared", repl_eval_find_predef_var_idx("extent") >= 0);
}

/* Initializers are preserved per-name; the line's trailing comment rides
 * the first split line only. */
static void test_split_decl_inits_and_comment(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float a = 1, b = 2; // hello");

    ReplCompileContext ctx = repl_compile_context_from_live(0);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileResult r = repl_compile_split_decl(&ctx, 0, &change, err, sizeof(err));
    ASSERT_INT("split inits compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("split inits count", change.count, 2);
    ASSERT_STR("init + comment on first line",
               change.text[0], "  static float a = 1; // hello");
    ASSERT_STR("init kept, comment dropped on rest",
               change.text[1], "  static float b = 2;");
}

/* A single-name decl has nothing to split. */
static void test_split_decl_single_name_no_change(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float solo;");

    ReplCompileContext ctx = repl_compile_context_from_live(0);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileResult r = repl_compile_split_decl(&ctx, 0, &change, err, sizeof(err));
    ASSERT_INT("single-name split compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("single-name split is NO_CHANGE", change.kind, REPL_COMPILED_NO_CHANGE);
}

/* A non-declaration line is not a split target. */
static void test_split_decl_non_decl_no_change(void) {
    glr_ctrl_reset_all();
    editor_feed_line("glVertex3f(0, 0, 0);");

    ReplCompileContext ctx = repl_compile_context_from_live(0);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileResult r = repl_compile_split_decl(&ctx, 0, &change, err, sizeof(err));
    ASSERT_INT("non-decl split compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("non-decl split is NO_CHANGE", change.kind, REPL_COMPILED_NO_CHANGE);
}

/* The editor entry point splits at the cursor as one undoable step. */
static void test_split_decl_via_editor_entry(void) {
    glr_ctrl_reset_all();
    editor_feed_line("float p, q;");
    editor_feed_line("glVertex3f(p, q, 0);");
    editor_state_edit_line_set(0);   /* cursor on the decl */

    ASSERT_INT("editor split consumed", editor_split_decl_at_cursor(), 1);
    ASSERT_INT("doc grew by one line", repl_state_document_count(), 3);
    ASSERT_STR("split line 0", editor_buffer_line(0), "  static float p;");
    ASSERT_STR("split line 1", editor_buffer_line(1), "  static float q;");
    ASSERT_TRUE("trailing command preserved",
                strstr(editor_buffer_line(2), "glVertex3f") != NULL);

    /* One undo restores the single multi-name decl line. */
    editor_undo_pop_snapshot();
    ASSERT_INT("undo restores single decl line", repl_state_document_count(), 2);
    ASSERT_STR("undo restores multi-name decl",
               editor_buffer_line(0), "  static float p, q;");
}

/* repl_compile_var_assign is pure on the failure path. */
static void test_compile_var_assign_failure_is_pure(void) {
    glr_ctrl_reset_all();

    set_input("float a;");
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float a;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Compile failure: assigning to undeclared name. */
    set_input("nonexistent = 1");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompileResult r = repl_compile_var_assign(
        "nonexistent = 1", &ctx, &change, err, sizeof(err));
    ASSERT_INT("undeclared assign returns ERROR", r, REPL_COMPILE_ERROR);
    ASSERT_TRUE("undeclared assign fills err", err[0] != '\0');

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("compile failure leaves state untouched (var_assign)",
                fingerprint_equal(&before, &after));
}

/* Compile NO_CHANGE (input that doesn't match the handler) leaves
 * everything untouched and never fills err. */
static void test_compile_no_change_leaves_state(void) {
    glr_ctrl_reset_all();
    ui_state_status_set("baseline status");
    ComputeFingerprint before = capture_fingerprint();

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    err[0] = 'x';  /* sentinel: ensure compile doesn't write err on NO_CHANGE */

    /* Input that's neither a float decl nor an assignment. */
    ReplCompileResult r = repl_compile_float_decl(
        "glVertex3f(0,0,0)", &ctx, &change, err, sizeof(err));
    ASSERT_INT("non-decl returns OK", r, REPL_COMPILE_OK);
    ASSERT_INT("non-decl is NO_CHANGE", change.kind, REPL_COMPILED_NO_CHANGE);

    r = repl_compile_var_assign("glColor3f(1,0,0)", &ctx, &change,
                                err, sizeof(err));
    ASSERT_INT("non-assign returns OK", r, REPL_COMPILE_OK);
    ASSERT_INT("non-assign is NO_CHANGE", change.kind, REPL_COMPILED_NO_CHANGE);

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("compile NO_CHANGE leaves state untouched",
                fingerprint_equal(&before, &after));
}

/* Successful compile + apply updates editor buffer and command store
 * atomically. */
static void test_compile_apply_updates_both(void) {
    glr_ctrl_reset_all();

    set_input("float energy;");
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_float_decl(
        "float energy;", &ctx, &change, err, sizeof(err));
    ASSERT_INT("float decl compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("float decl insert kind", change.kind, REPL_COMPILED_INSERT_ONE);
    ASSERT_INT("float decl is registered as predef op",
               change.predef_op_count >= 1, 1);

    /* Pre-apply: cmd store + buffer empty. */
    ASSERT_INT("pre-apply cmd count", repl_state_document_count(), 0);
    ASSERT_INT("pre-apply buffer count", editor_buffer_count(), 0);
    ASSERT_INT("pre-apply predef registered",
               repl_eval_find_predef_var_idx("energy"), -1);

    int ok = editor_commit_apply_external_change(&change, 0, 0);
    ASSERT_INT("apply returns 1 on success", ok, 1);

    /* Post-apply: both halves match. */
    ASSERT_INT("post-apply cmd count", repl_state_document_count(), 1);
    ASSERT_INT("post-apply buffer count", editor_buffer_count(), 1);
    ASSERT_INT("post-apply cmd type",
               repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
    ASSERT_TRUE("post-apply predef registered",
                repl_eval_find_predef_var_idx("energy") >= 0);
    ASSERT_STR("post-apply buffer line text",
               editor_buffer_line(0), "  static float energy;");
}

/* Var-assign compile + apply updates the predef value alongside the
 * source command. */
static void test_compile_apply_var_assign_updates_value(void) {
    glr_ctrl_reset_all();

    /* Set up a declared variable. */
    set_input("float k;");
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float k;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);

    /* Snapshot: k is registered with value 0. */
    int slot = repl_eval_find_predef_var_idx("k");
    ASSERT_TRUE("k is registered", slot >= 0);
    ASSERT_TRUE("k value starts at 0", g_predef_vars[slot].value == 0.0f);

    /* Compile + apply assignment. */
    set_input("k = 7");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompileResult r = repl_compile_var_assign(
        "k = 7", &ctx, &change, err, sizeof(err));
    ASSERT_INT("k = 7 compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("k = 7 INSERT_ONE", change.kind, REPL_COMPILED_INSERT_ONE);

    int ok = editor_commit_apply_external_change(&change, 0, 0);
    ASSERT_INT("apply returns 1", ok, 1);
    ASSERT_TRUE("k value is 7", g_predef_vars[slot].value == 7.0f);
    ASSERT_INT("doc has decl + assign", repl_state_document_count(), 2);
}

/* Regression: when an `X = expr;` assignment overwrites a
 * `float Y;` decl row (overwrite mode, Y unused elsewhere), the
 * SET_VALUE op for X must survive the UNDECLARE op insertion for
 * Y. Pre-fix, src/repl/compile.c:983 documents that the UNDECLARE
 * loop clobbers slot 0 (where the scalar branch parked SET_VALUE
 * for X) and the salvage block copies the already-clobbered slot,
 * producing [UNDECLARE_Y, UNDECLARE_Y] and silently dropping the
 * SET_VALUE for X. X's predef value stays at its old value. */
static void test_overwrite_decl_with_assign_preserves_set_value(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;
    ReplCompileContext ctx;

    /* Declare X with an initializer of 5. */
    set_input("float X = 5;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("seed float X = 5 compile OK",
               repl_compile_float_decl("float X = 5;", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("seed float X = 5 apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    /* Declare Y (no initializer, no users — eligible for overwrite). */
    set_input("float Y;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("seed float Y compile OK",
               repl_compile_float_decl("float Y;", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("seed float Y apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    int x_slot = repl_eval_find_predef_var_idx("X");
    int y_slot = repl_eval_find_predef_var_idx("Y");
    ASSERT_TRUE("X declared", x_slot >= 0);
    ASSERT_TRUE("Y declared", y_slot >= 0);
    ASSERT_TRUE("X starts at 5", g_predef_vars[x_slot].value == 5.0f);

    /* Find Y's row index and point the editor at it in overwrite mode. */
    int y_row = -1;
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *c = &repl_state_document_cmds_mut()[i];
        if (c->type == CMD_VAR_DECLARE && c->payload.decl.count == 1 &&
            strcmp(c->payload.decl.names[0], "Y") == 0) {
            y_row = i;
            break;
        }
    }
    ASSERT_TRUE("located float Y; row", y_row >= 0);

    editor_state_edit_line_set(y_row);
    editor_insert_mode_set(0);

    /* Compile + apply `X = 42` overwriting the `float Y;` row. */
    set_input("X = 42");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompileResult r = repl_compile_var_assign(
        "X = 42", &ctx, &change, err, sizeof(err));
    ASSERT_INT("X = 42 over decl-row compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("X = 42 over decl-row plans REPLACE_ONE",
               change.kind, REPL_COMPILED_REPLACE_ONE);

    int ok = editor_commit_apply_external_change(&change, 0, 0);
    ASSERT_INT("X = 42 over decl-row apply OK", ok, 1);

    /* Sanity: Y was undeclared (its UNDECLARE op did run). */
    ASSERT_INT("Y undeclared after overwrite",
               repl_eval_find_predef_var_idx("Y"), -1);

    /* The bug: X's predef value should now be 42, but the dropped
     * SET_VALUE op leaves it at 5. Re-find X's slot in case Y's
     * removal shifted slot indices. */
    x_slot = repl_eval_find_predef_var_idx("X");
    ASSERT_TRUE("X still declared after overwrite", x_slot >= 0);
    ASSERT_FLOAT("X = 42 SET_VALUE survives UNDECLARE cascade",
                 g_predef_vars[x_slot].value, 42.0f, 1e-6f);
}

/* Regression: when a later variable assignment overwrites an earlier
 * decl row, the staged CMD_VAR_ASSIGN must be rebased to the post-
 * undeclare slot index before the replacement lands. Pre-fix the
 * compiled change kept Y's old slot while the overwrite removed X,
 * so the inserted assignment pointed at the wrong predef variable. */
static void test_overwrite_earlier_decl_with_later_assign_rebases_slot(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;
    ReplCompileContext ctx;

    set_input("float X;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("seed float X compile OK",
               repl_compile_float_decl("float X;", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("seed float X apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    set_input("float Y = 5;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("seed float Y compile OK",
               repl_compile_float_decl("float Y = 5;", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("seed float Y apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    int x_slot = repl_eval_find_predef_var_idx("X");
    int y_slot = repl_eval_find_predef_var_idx("Y");
    ASSERT_TRUE("X declared", x_slot >= 0);
    ASSERT_TRUE("Y declared", y_slot >= 0);
    ASSERT_TRUE("Y starts after X", y_slot > x_slot);

    int x_row = -1;
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *c = &repl_state_document_cmds_mut()[i];
        if (c->type == CMD_VAR_DECLARE && c->payload.decl.count == 1 &&
            strcmp(c->payload.decl.names[0], "X") == 0) {
            x_row = i;
            break;
        }
    }
    ASSERT_TRUE("located float X; row", x_row >= 0);

    editor_state_edit_line_set(x_row);
    editor_insert_mode_set(0);

    set_input("Y = 42");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("Y = 42 over earlier decl compile OK",
               repl_compile_var_assign("Y = 42", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("Y = 42 over earlier decl plans REPLACE_ONE",
               change.kind, REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("compiled slot rebased before apply",
               change.cmds[0].var_idx, y_slot - 1);

    ASSERT_INT("Y = 42 over earlier decl apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    ASSERT_INT("X undeclared after overwrite",
               repl_eval_find_predef_var_idx("X"), -1);

    y_slot = repl_eval_find_predef_var_idx("Y");
    ASSERT_TRUE("Y still declared after overwrite", y_slot >= 0);
    ASSERT_FLOAT("Y value survives overwrite",
                 g_predef_vars[y_slot].value, 42.0f, 1e-6f);
    ASSERT_INT("replacement row is var assign",
               repl_state_document_cmds_mut()[x_row].type, CMD_VAR_ASSIGN);
    ASSERT_INT("replacement row slot matches live Y slot",
               repl_state_document_cmds_mut()[x_row].var_idx, y_slot);
}

static void test_overwrite_decl_ignores_shadowed_param_refs(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;
    ReplCompileContext ctx;

    editor_feed_line("float x;");
    editor_feed_line("func0(x) {");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");

    editor_state_edit_line_set(0);
    editor_insert_mode_set(0);
    set_input("float y;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());

    ASSERT_INT("shadowed param decl overwrite compile OK",
               repl_compile_float_decl("float y;", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("shadowed param decl overwrite is REPLACE_ONE",
               change.kind, REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("shadowed param decl overwrite apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("shadowed param decl overwrite removed x",
               repl_eval_find_predef_var_idx("x"), -1);
    ASSERT_TRUE("shadowed param decl overwrite declared y",
                repl_eval_find_predef_var_idx("y") >= 0);
    ASSERT_INT("shadowed param decl overwrite kept func def",
               repl_state_document_cmds_mut()[1].type, CMD_FUNC_DEF);
}

static void test_overwrite_assign_ignores_shadowed_param_refs(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;
    ReplCompileContext ctx;
    int y_slot;

    editor_feed_line("float y;");
    editor_feed_line("float x;");
    editor_feed_line("func0(x) {");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");

    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);
    set_input("y = 3");
    ctx = repl_compile_context_from_live(editor_state_edit_line());

    ASSERT_INT("shadowed param assign overwrite compile OK",
               repl_compile_var_assign("y = 3", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("shadowed param assign overwrite is REPLACE_ONE",
               change.kind, REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("shadowed param assign overwrite apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("shadowed param assign overwrite removed x",
               repl_eval_find_predef_var_idx("x"), -1);
    y_slot = repl_eval_find_predef_var_idx("y");
    ASSERT_TRUE("shadowed param assign overwrite kept y", y_slot >= 0);
    ASSERT_FLOAT("shadowed param assign overwrite set y",
                 g_predef_vars[y_slot].value, 3.0f, 1e-6f);
}

static void test_var_assign_rejects_function_param_target(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;

    editor_feed_line("func0(radius) {");
    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("func param assign compile ERROR",
               repl_compile_var_assign("radius = 1", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("func param assign message names parameter",
                strstr(err, "function parameter 'radius'") != NULL);
    ASSERT_TRUE("func param assign message mentions constant",
                strstr(err, "function parameters are constant") != NULL);
}

static void test_var_assign_rejects_shadowed_function_param_target(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;

    editor_feed_line("float radius;");
    editor_feed_line("func0(radius) {");
    editor_state_edit_line_set(2);
    editor_insert_mode_set(0);

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("shadowed func param assign compile ERROR",
               repl_compile_var_assign("radius = 1", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("shadowed func param assign message names parameter",
                strstr(err, "function parameter 'radius'") != NULL);
    ASSERT_TRUE("shadowed func param assign message mentions constant",
                strstr(err, "function parameters are constant") != NULL);
}

static void test_delete_range_ignores_shadowed_param_refs(void) {
    glr_ctrl_reset_all();

    char err[REPL_STATUS_TEXT_MAX];
    ReplCompiledChange change;
    ReplCompileContext ctx;

    editor_feed_line("float x;");
    editor_feed_line("func0(x) {");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");

    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("shadowed param delete compile OK",
               repl_compile_delete_range(0, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("shadowed param delete is DELETE_RANGE",
               change.kind, REPL_COMPILED_DELETE_RANGE);
    ASSERT_INT("shadowed param delete apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("shadowed param delete removed x",
               repl_eval_find_predef_var_idx("x"), -1);
    ASSERT_INT("shadowed param delete kept func def at top",
               repl_state_document_cmds_mut()[0].type, CMD_FUNC_DEF);
}

/* Forced cmd-store capacity failure leaves predef-vars, editor
 * buffer, and command store all unchanged. The preflight inside
 * editor_commit_apply_external_change is the load-bearing
 * mechanism: without it the predef-op cascade would mutate while
 * the cmd-store insert silently fails. */
static void test_capacity_failure_is_atomic(void) {
    glr_ctrl_reset_all();

    ReplCompiledChange change;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX];

    /* Establish a small pre-state: one declared variable, one
     * assignment line. */
    set_input("float anchor;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    repl_compile_float_decl("float anchor;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);
    set_input("anchor = 9");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    repl_compile_var_assign("anchor = 9", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);

    /* Snapshot pre-state. */
    int pre_doc_count    = repl_state_document_count();
    int pre_buf_count    = editor_buffer_count();
    int pre_predef_count = g_num_predef_vars;
    int anchor_slot      = repl_eval_find_predef_var_idx("anchor");
    float pre_anchor_val = anchor_slot >= 0 ? g_predef_vars[anchor_slot].value : 0.0f;
    char pre_line0[MAX_LINE_LEN];
    strncpy(pre_line0, editor_buffer_line(0) ? editor_buffer_line(0) : "",
            sizeof(pre_line0) - 1);
    pre_line0[sizeof(pre_line0) - 1] = '\0';

    /* Forge a compiled change that the preflight must reject:
     * INSERT_ONE at pos == doc_count + 5 is past the legal insert
     * range. The preflight returns 0 and no mutation runs. */
    repl_compiled_change_init(&change);
    change.kind = REPL_COMPILED_INSERT_ONE;
    change.pos = repl_state_document_count() + 5;  /* out of range */
    change.count = 1;
    change.adjust_edit_line = 1;
    change.cmds[0].type = CMD_VAR_DECLARE;
    change.cmds[0].valid = 1;
    change.cmds[0].payload.decl.count = 1;
    strncpy(change.cmds[0].payload.decl.names[0], "ghost",
            sizeof(change.cmds[0].payload.decl.names[0]) - 1);
    change.cmds[0].payload.decl.names[0][sizeof(change.cmds[0].payload.decl.names[0]) - 1] = '\0';
    strncpy(change.text[0], "  static float ghost;", sizeof(change.text[0]) - 1);
    change.text[0][sizeof(change.text[0]) - 1] = '\0';
    /* Add a predef-op that, if replayed, would register a new
     * predef and grow num_predef_vars by 1. */
    change.predef_ops[0].kind = REPL_PREDEF_OP_DECLARE;
    strncpy(change.predef_ops[0].name, "ghost",
            sizeof(change.predef_ops[0].name) - 1);
    change.predef_ops[0].name[sizeof(change.predef_ops[0].name) - 1] = '\0';
    change.predef_op_count = 1;

    /* Preflight rejects. */
    int can = repl_apply_can_apply_compiled_change(&change);
    ASSERT_INT("preflight rejects out-of-range insert", can, 0);

    int ok = editor_commit_apply_external_change(&change, 0, 0);
    ASSERT_INT("apply returns 0 on preflight failure", ok, 0);

    /* All three surfaces unchanged. */
    ASSERT_INT("doc count unchanged after failed apply",
               repl_state_document_count(), pre_doc_count);
    ASSERT_INT("buffer count unchanged after failed apply",
               editor_buffer_count(), pre_buf_count);
    ASSERT_INT("predef-var count unchanged after failed apply",
               g_num_predef_vars, pre_predef_count);
    ASSERT_INT("ghost not registered",
               repl_eval_find_predef_var_idx("ghost"), -1);
    if (anchor_slot >= 0) {
        ASSERT_TRUE("anchor value unchanged after failed apply",
                    g_predef_vars[anchor_slot].value == pre_anchor_val);
    }
    ASSERT_STR("buffer first line unchanged after failed apply",
               editor_buffer_line(0), pre_line0);

    /* Same atomicity for an over-capacity INSERT_MANY: forge one
     * past the cmd-store capacity so insert_many's preflight fires. */
    repl_compiled_change_init(&change);
    change.kind = REPL_COMPILED_INSERT_MANY;
    change.pos = 0;
    change.count = MAX_COMMIT_CMDS + 1;  /* exceeds change buffer */
    /* leave cmds/text uninitialized — preflight should reject before reading */
    change.predef_ops[0].kind = REPL_PREDEF_OP_DECLARE;
    strncpy(change.predef_ops[0].name, "phantom",
            sizeof(change.predef_ops[0].name) - 1);
    change.predef_ops[0].name[sizeof(change.predef_ops[0].name) - 1] = '\0';
    change.predef_op_count = 1;

    can = repl_apply_can_apply_compiled_change(&change);
    ASSERT_INT("preflight rejects over-capacity insert_many", can, 0);
    ok = editor_commit_apply_external_change(&change, 0, 0);
    ASSERT_INT("apply returns 0 on insert_many capacity failure", ok, 0);
    ASSERT_INT("doc count still unchanged",
               repl_state_document_count(), pre_doc_count);
    ASSERT_INT("predef-var count still unchanged",
               g_num_predef_vars, pre_predef_count);
    ASSERT_INT("phantom not registered",
               repl_eval_find_predef_var_idx("phantom"), -1);
}

/* Reformat-the-document path: the live reformat loop calls
 * repl_command_store_replace_one + editor_buffer_replace_line for
 * every line. Verify that after a reformat (run via the existing
 * try_commit dispatcher path), buffer + store remain in sync. */
static void test_reformat_keeps_buffer_and_store_aligned(void) {
    glr_ctrl_reset_all();

    /* Build a small program. */
    set_input("float a;");
    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float a;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);
    set_input("a = 1");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    repl_compile_var_assign("a = 1", &ctx, &change, err, sizeof(err));
    editor_commit_apply_external_change(&change, 0, 0);

    int doc_count = repl_state_document_count();
    int buf_count = editor_buffer_count();
    ASSERT_INT("post-build doc + buffer agree on count",
               doc_count, buf_count);

    /* Trigger reformat via editor_reformat_commands(). */
    editor_reformat_commands();

    ASSERT_INT("post-reformat doc count", repl_state_document_count(), doc_count);
    ASSERT_INT("post-reformat buffer count", editor_buffer_count(), buf_count);
    /* Each cmd has matching text in the buffer. */
    for (int i = 0; i < doc_count; i++) {
        ASSERT_TRUE("post-reformat buffer line non-empty",
                    editor_buffer_line(i) != NULL &&
                    editor_buffer_line(i)[0] != '\0');
    }
}

static void test_set_predef_value_rewrites_decl_not_assignments(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float x = 1, y;");
    editor_feed_line("x = 2;");
    editor_feed_line("x = y + 1;");
    editor_feed_line("x = 3;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 4.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef decl-first compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef decl-first replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("set_predef decl-first replace pos", change.pos, 0);
    ASSERT_STR("set_predef decl-first text",
               change.text[0], "  static float x = 4.5, y;");
    ASSERT_INT("set_predef decl-first predef op count", change.predef_op_count, 1);
    ASSERT_INT("set_predef decl-first op kind", change.predef_ops[0].kind,
               REPL_PREDEF_OP_SET_VALUE);

    ASSERT_INT("set_predef decl-first apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("set_predef decl-first buffer line updated",
               editor_buffer_line(0), "  static float x = 4.5, y;");
    ASSERT_STR("set_predef literal assignment preserved",
               editor_buffer_line(1), "  x = 2;");
    ASSERT_STR("set_predef expression assignment preserved",
               editor_buffer_line(2), "  x = y + 1;");
    ASSERT_STR("set_predef reset assignment preserved",
               editor_buffer_line(3), "  x = 3;");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef decl-first x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef decl-first live value",
                     g_predef_vars[x_idx].value, 4.5f, 1e-6f);
    }
}

static void test_set_predef_value_rewrites_declaration_initializer(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float a = 1, x = 2, y; // vars");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 2.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef decl compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef decl replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_STR("set_predef decl text",
               change.text[0], "  static float a = 1, x = 2.5, y; // vars");

    ASSERT_INT("set_predef decl apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("set_predef decl buffer line updated",
               editor_buffer_line(0), "  static float a = 1, x = 2.5, y; // vars");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef decl x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef decl live value",
                     g_predef_vars[x_idx].value, 2.5f, 1e-6f);
    }
}

static void test_set_predef_value_adds_declaration_initializer(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float x;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 2.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef add init compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef add init replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_STR("set_predef add init text", change.text[0], "  static float x = 2.5;");

    ASSERT_INT("set_predef add init apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("set_predef add init buffer line updated",
               editor_buffer_line(0), "  static float x = 2.5;");
}

static void test_set_predef_value_rewrites_declaration_and_keeps_expression_sources(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float x;");
    editor_feed_line("float y;");
    editor_feed_line("x = y + 1;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 8.0f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef expr compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef expr rewrites decl kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_STR("set_predef expr decl text",
               change.text[0], "  static float x = 8;");

    ASSERT_INT("set_predef expr apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("set_predef expr formula preserved",
               editor_buffer_line(2), "  x = y + 1;");
    ASSERT_STR("set_predef expr decl updated",
               editor_buffer_line(0), "  static float x = 8;");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef expr x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef expr live value",
                     g_predef_vars[x_idx].value, 8.0f, 1e-6f);
    }
}

static void test_set_predef_value_does_not_rewrite_assignment_without_decl(void) {
    glr_ctrl_reset_all();

    editor_feed_line("t = 0;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "t", 3.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef assignment-only compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef assignment-only source untouched kind", change.kind,
               REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("set_predef assignment-only predef op count", change.predef_op_count, 1);

    ASSERT_INT("set_predef assignment-only apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("set_predef assignment-only reset preserved",
               editor_buffer_line(0), "  t = 0;");
    {
        int t_idx = repl_eval_find_predef_var_idx("t");
        ASSERT_TRUE("set_predef assignment-only t exists", t_idx >= 0);
        ASSERT_FLOAT("set_predef assignment-only live value",
                     g_predef_vars[t_idx].value, 3.5f, 1e-6f);
    }
}

static void test_set_predef_value_live_only_without_source(void) {
    glr_ctrl_reset_all();

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "t", 3.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef live-only compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef live-only no source change", change.kind,
               REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("set_predef live-only predef op count", change.predef_op_count, 1);

    ASSERT_INT("set_predef live-only apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("set_predef live-only buffer count unchanged",
               editor_buffer_count(), 0);
    {
        int t_idx = repl_eval_find_predef_var_idx("t");
        ASSERT_TRUE("set_predef live-only t exists", t_idx >= 0);
        ASSERT_FLOAT("set_predef live-only value",
                     g_predef_vars[t_idx].value, 3.5f, 1e-6f);
    }
}

/* The live half: predef op, never a source rewrite — even when a declaration
 * row exists. This is what every variable-panel motion event compiles. */
static void test_compile_set_predef_value_live_leaves_declaration(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float x = 2;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value_live(
        "x", 7.25f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("live set compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("live set makes no source change", change.kind,
               REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("live set carries the predef op", change.predef_op_count, 1);

    ASSERT_INT("live set apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("live set leaves the declaration text alone",
               editor_buffer_line(0), "  static float x = 2;");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("live set x exists", x_idx >= 0);
        ASSERT_FLOAT("live set updates the live value",
                     g_predef_vars[x_idx].value, 7.25f, 1e-6f);
    }
}

/* The persistence half: source rewrite, no predef op (the live value is
 * already final, and a predef op would fire a second tutorial notify). This is
 * what the variable-panel drag compiles once, on mouse-up. */
static void test_compile_persist_predef_value_rewrites_declaration(void) {
    glr_ctrl_reset_all();

    editor_feed_line("float a = 1, x = 2, y; // vars");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_persist_predef_value(
        "x", 2.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("persist compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("persist replaces the decl row", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("persist carries no predef op", change.predef_op_count, 0);
    /* Byte-identical to the combined entry's rewrite: same kernel. */
    ASSERT_STR("persist decl text",
               change.text[0], "  static float a = 1, x = 2.5, y; // vars");

    ASSERT_INT("persist apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_STR("persist decl buffer line updated",
               editor_buffer_line(0), "  static float a = 1, x = 2.5, y; // vars");
}

/* No declaration row to rewrite: persistence is a no-op the caller can skip. */
static void test_compile_persist_predef_value_without_declaration(void) {
    glr_ctrl_reset_all();

    editor_feed_line("t = 0;");

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_persist_predef_value(
        "t", 3.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("persist-no-decl compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("persist-no-decl is a no-change", change.kind,
               REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("persist-no-decl carries no predef op", change.predef_op_count, 0);
    ASSERT_STR("persist-no-decl leaves the assignment alone",
               editor_buffer_line(0), "  t = 0;");
}

static void test_compile_predef_value_split_rejects_undeclared(void) {
    glr_ctrl_reset_all();

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ASSERT_INT("live set rejects undeclared name",
               repl_compile_set_predef_value_live("nope", 1.0f, &ctx, &change,
                                                  err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_INT("persist rejects undeclared name",
               repl_compile_persist_predef_value("nope", 1.0f, &ctx, &change,
                                                 err, sizeof(err)),
               REPL_COMPILE_ERROR);
}

static void test_set_predef_value_marks_flat_dirty_only_on_change(void) {
    glr_ctrl_reset_all();

    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("dirty-gate t exists", t_idx >= 0);
    g_predef_vars_mut[t_idx].value = 3.5f;
    repl_state_flat_program_clear_dirty();
    /* Value changes route by the flat program's dep masks; seed a state
     * where t is a value-only root so a real change is observable (as t's
     * args-dirty bit) while a same-value SET_VALUE stays a no-op. */
    repl_state_flat_program_set_dep_state(0, (ReplExprDepMask)1u << t_idx, 1);

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "t", 3.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("same-value set_predef compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("same-value set_predef no source change",
               change.kind, REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("same-value set_predef apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_FLOAT("same-value set_predef keeps value",
                 g_predef_vars[t_idx].value, 3.5f, 1e-6f);
    ASSERT_INT("same-value set_predef keeps flat clean",
               repl_state_flat_program_dirty(), 0);
    ASSERT_INT("same-value set_predef keeps args-dirty clean",
               (int)repl_state_flat_program_args_dirty_mask(), 0);

    r = repl_compile_set_predef_value(
        "t", 4.0f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("changed set_predef compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("changed set_predef apply OK",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_FLOAT("changed set_predef updates value",
                 g_predef_vars[t_idx].value, 4.0f, 1e-6f);
    ASSERT_INT("changed set_predef sets t's args-dirty bit",
               (int)((repl_state_flat_program_args_dirty_mask()
                      >> t_idx) & 1u), 1);
    ASSERT_INT("changed set_predef leaves full flag clean",
               repl_state_flat_program_dirty(), 0);
}

/* Live dispatch compile-failure path: redeclaring an existing predef
 * var produces a status error and leaves buffer/store/predef/undo
 * untouched. */
static void test_orchestration_compile_failure_returns_diagnostic(void) {
    glr_ctrl_reset_all();

    /* Establish a non-trivial pre-state. */
    editor_input_set_text("float anchor;");
    int ok = editor_try_commit_float_decl();
    ASSERT_INT("setup commit consumed", ok, 1);
    ASSERT_TRUE("setup commit registered predef",
                repl_eval_find_predef_var_idx("anchor") >= 0);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Trigger compile failure: redeclare. */
    editor_input_set_text("float anchor;");
    int r = editor_try_commit_float_decl();
    ASSERT_INT("compile failure consumed", r, 1);
    {
        UiStatusState st = ui_state_status();
        ASSERT_TRUE("compile failure raised status", st.text[0] != '\0');
        ASSERT_TRUE("compile failure status differs from baseline",
                    strcmp(st.text, "baseline status") != 0);
    }

    /* The status fingerprint shifts (new error text), but document /
     * predef / buffer state must be byte-identical to pre-attempt. */
    ComputeFingerprint after = capture_fingerprint();
    ASSERT_INT("doc count unchanged", after.cmd_count, before.cmd_count);
    ASSERT_INT("predef count unchanged",
               after.num_predef_vars, before.num_predef_vars);
    ASSERT_INT("buffer count unchanged",
               after.buffer_count, before.buffer_count);
}

/* Live dispatch NO_CHANGE path: input that no editor_try_commit_*
 * handler accepts — the chain falls through with no mutation. */
static void test_orchestration_no_change_falls_through(void) {
    glr_ctrl_reset_all();
    editor_input_set_text("glVertex3f(0,0,0)");
    ui_state_status_set("baseline");
    ComputeFingerprint before = capture_fingerprint();

    /* var_statements covers float_decl + var_assign; neither matches
     * a gl-call input, so it returns 0 (chain falls through). */
    int r = editor_try_commit_var_statements();
    ASSERT_INT("var-statement chain non-match returns 0", r, 0);

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("var-statement NO_CHANGE leaves state untouched",
                fingerprint_equal(&before, &after));
}

/* Live dispatch success path: buffer + store + predef mutate together
 * and status carries the commit message. */
static void test_orchestration_success_returns_message(void) {
    glr_ctrl_reset_all();
    editor_input_set_text("float energy;");
    ui_state_status_set("");

    int r = editor_try_commit_float_decl();
    ASSERT_INT("success consumed", r, 1);
    {
        UiStatusState st = ui_state_status();
        ASSERT_TRUE("success status non-empty", st.text[0] != '\0');
    }

    ASSERT_INT("post-success cmd count", repl_state_document_count(), 1);
    ASSERT_INT("post-success buffer count", editor_buffer_count(), 1);
    ASSERT_TRUE("post-success predef registered",
                repl_eval_find_predef_var_idx("energy") >= 0);
}

/* func_def comment-relocation: leading comments above the cursor
 * get moved alongside the new func def block (delete + insert in
 * one transaction). */
static void test_func_def_comment_relocation(void) {
    glr_ctrl_reset_all();

    /* Build doc:
     *   line 0: glVertex3f(1, 0, 0);    (geometry above future func)
     *   line 1: // descriptive comment   (top-level comment)
     *   line 2: // continuation          (top-level comment)
     *   cursor sits at line 3 (end of doc), about to define func0.
     */
    editor_feed_line("  glVertex3f(1, 0, 0);");
    editor_feed_line("// descriptive comment");
    editor_feed_line("// continuation");

    int pre_doc_count = repl_state_document_count();
    ASSERT_INT("pre-funcdef doc count", pre_doc_count, 3);

    /* Commit a func_def. The compile path should:
     *   - delete comments at [1, 2]
     *   - insert {comment, comment, FUNC_DEF, FUNC_END} at the
     *     post-delete function_decl_insert_pos (= 1 since vertex is
     *     not a var-decl/comment/funcdef and stops the walk
     *     immediately after var-decls).
     * Wait: function_decl_insert_pos walks past var-decls then
     * accepts CMD_COMMENT or CMD_FUNC_DEF as continuation. The
     * vertex command is none of those, so it's a stopper. With no
     * var-decls in our doc, the walk stops at index 0. */
    set_input("func0() {");
    /* func_def is an editor-side compile; route through the live
     * try_commit dispatcher which forwards through
     * editor_compile_func_def. */
    editor_try_commit_block_structs();

    /* Post-commit doc shape:
     *   - The two comments + fd + fe land at function_decl_insert_pos
     *     = 0 (start of doc).
     *   - The vertex shifts to index 4 (after the inserted block).
     *   Total: 5 cmds.
     */
    int post_doc_count = repl_state_document_count();
    ASSERT_INT("post-funcdef doc count", post_doc_count, 5);
    ASSERT_INT("relocated comment 0",
               repl_state_document_cmds()[0].type, CMD_COMMENT);
    ASSERT_INT("relocated comment 1",
               repl_state_document_cmds()[1].type, CMD_COMMENT);
    ASSERT_INT("inserted func def",
               repl_state_document_cmds()[2].type, CMD_FUNC_DEF);
    ASSERT_INT("inserted func end",
               repl_state_document_cmds()[3].type, CMD_FUNC_END);
    ASSERT_INT("vertex shifted to end",
               repl_state_document_cmds()[4].type, CMD_VERTEX3F);

    /* The editor buffer mirrors the cmd-store with the same
     * content. */
    /* Comment text retains the canonical indent from editor_feed_line. */
    ASSERT_TRUE("buffer line 0 contains 'descriptive'",
                strstr(editor_buffer_line(0), "descriptive") != NULL);
    ASSERT_TRUE("buffer line 1 contains 'continuation'",
                strstr(editor_buffer_line(1), "continuation") != NULL);
    ASSERT_TRUE("buffer line 2 contains 'func0'",
                strstr(editor_buffer_line(2), "func0") != NULL);
    ASSERT_STR("buffer line 3 is closing brace",
               editor_buffer_line(3), "  }");
    ASSERT_STR("buffer line 4 is vertex",
               editor_buffer_line(4), "  glVertex3f(1, 0, 0);");
}

static void test_func_def_blank_line_relocation(void) {
    glr_ctrl_reset_all();

    editor_feed_line("  glVertex3f(1, 0, 0);");
    editor_feed_line("");
    editor_feed_line("// descriptive comment");
    editor_feed_line("// continuation");

    ASSERT_INT("pre-funcdef blank relocation count",
               repl_state_document_count(), 4);
    ASSERT_INT("pre-funcdef blank row present",
               repl_state_document_cmds()[1].type, CMD_EMPTY);

    set_input("func0() {");
    editor_try_commit_block_structs();

    ASSERT_INT("post-funcdef blank relocation count",
               repl_state_document_count(), 6);
    ASSERT_INT("relocated blank row",
               repl_state_document_cmds()[0].type, CMD_EMPTY);
    ASSERT_INT("relocated comment 0 after blank",
               repl_state_document_cmds()[1].type, CMD_COMMENT);
    ASSERT_INT("relocated comment 1 after blank",
               repl_state_document_cmds()[2].type, CMD_COMMENT);
    ASSERT_INT("inserted func def after blank/comment run",
               repl_state_document_cmds()[3].type, CMD_FUNC_DEF);
    ASSERT_INT("inserted func end after blank/comment run",
               repl_state_document_cmds()[4].type, CMD_FUNC_END);
    ASSERT_INT("vertex shifted to end after relocation",
               repl_state_document_cmds()[5].type, CMD_VERTEX3F);

    ASSERT_STR("buffer line 0 preserved blank", editor_buffer_line(0), "");
    ASSERT_TRUE("buffer line 1 contains descriptive",
                strstr(editor_buffer_line(1), "descriptive") != NULL);
    ASSERT_TRUE("buffer line 2 contains continuation",
                strstr(editor_buffer_line(2), "continuation") != NULL);
}

/* Regression: a blank line separating two var-decl groups must not
 * stop the func-decl insert walk inside the prologue. Pre-fix, the
 * walk skipped the leading decls, stepped over the blank, then hit the
 * second decl group and broke — inserting the new func BETWEEN the
 * float groups instead of after all decls. */
static void test_func_def_after_blank_separated_decls(void) {
    glr_ctrl_reset_all();

    /* Build a doc with two var-decl groups split by a blank line:
     *   [0] float a;
     *   [1] float b;
     *   [2] <blank>
     *   [3] float c;
     *   [4] float d;
     * The blank is inserted mid-document (Enter at column 0 above
     * `float c`) so it genuinely separates two decl groups — feeding a
     * blank directly would auto-relocate the later decls above it. */
    editor_feed_line("float a;");
    editor_feed_line("float b;");
    editor_feed_line("float c;");
    editor_feed_line("float d;");
    editor_navigate_to_line(2);          /* float c */
    editor_cursor_pos_set(0);
    editor_handle_key('\r', 0, 0);       /* blank line above float c */

    ASSERT_INT("pre-funcdef decl-group doc count",
               repl_state_document_count(), 5);
    ASSERT_INT("pre-funcdef blank between decl groups",
               repl_state_document_cmds()[2].type, CMD_EMPTY);
    ASSERT_INT("pre-funcdef second decl group present",
               repl_state_document_cmds()[3].type, CMD_VAR_DECLARE);

    /* Move the cursor below the decls and define a func. Pre-fix, the
     * blank at index 2 stopped the insert-pos walk and the func landed
     * BETWEEN the float groups; the func must land after all decls. */
    editor_navigate_to_line(repl_state_document_count());
    set_input("func0() {");
    editor_try_commit_block_structs();

    ASSERT_INT("post-funcdef decl-group doc count",
               repl_state_document_count(), 7);
    ASSERT_INT("decl 0 stays put",
               repl_state_document_cmds()[0].type, CMD_VAR_DECLARE);
    ASSERT_INT("decl 1 stays put",
               repl_state_document_cmds()[1].type, CMD_VAR_DECLARE);
    ASSERT_INT("blank separator stays put",
               repl_state_document_cmds()[2].type, CMD_EMPTY);
    ASSERT_INT("decl 2 stays put (not displaced by func)",
               repl_state_document_cmds()[3].type, CMD_VAR_DECLARE);
    ASSERT_INT("decl 3 stays put",
               repl_state_document_cmds()[4].type, CMD_VAR_DECLARE);
    ASSERT_INT("func def lands after all decls",
               repl_state_document_cmds()[5].type, CMD_FUNC_DEF);
    ASSERT_INT("func end lands after all decls",
               repl_state_document_cmds()[6].type, CMD_FUNC_END);
}

/* Resume publish: when a func_def's relocation moves the original
 * cursor position above the inserted block, the apply step
 * publishes the delta so the matching close-brace's compile
 * advances edit_line by that delta. */
static void test_func_def_resume_publish_consumed_by_close_brace(void) {
    glr_ctrl_reset_all();

    /* Build a doc where leading comments precede the cursor:
     *   [0] // comment
     *   cursor at 1, define func.
     *
     * After func def insert: comment + fd + fe at position 0.
     * resume_pos = 1 - 1 = 0; insert_pos = 0; resume_delta = 0.
     * That's a degenerate case; let's add a vertex above so the
     * cursor sits past more state.
     */
    editor_feed_line("// header");
    editor_feed_line("  glVertex3f(1, 0, 0);");

    /* Move cursor between the vertex and a new comment. */
    editor_feed_line("// before func");
    int pre_count = repl_state_document_count();
    ASSERT_INT("pre-funcdef count", pre_count, 3);

    /* Now define func0. The leading comment "// before func" is at
     * index 2 (depth 0). After delete: doc = [0:// header,
     * 1:vertex]. function_decl_insert_pos walks: not a var_decl
     * (header is a comment, allowed), step over comment, vertex is
     * not a comment/funcdef → stops at 1 (post-delete index).
     * Wait: function_decl_insert_pos starts after var-decls
     * (none), then accepts CMD_COMMENT or CMD_FUNC_DEF. Comment at
     * index 0 is stepped over → pos = 1. Vertex at 1 is the
     * stopper → returns 1.
     *
     * So insert_pos = 1. resume_pos = 2 (edit_line) - 1 (count) =
     * 1. resume_delta = max(0, 1 - 1) = 0. Hmm, still 0.
     *
     * Try a layout where insert_pos < resume_pos:
     *   [0] // header
     *   [1] // pre-vertex
     *   [2] vertex
     *   [3] // about to define func
     *   cursor at 4.
     * After delete of comment at [3,3]: insert_pos = 2
     * (header, pre-vertex stepped over, vertex stops). resume_pos
     * = 4 - 1 = 3. resume_delta = 3 - 2 = 1.
     */
    glr_ctrl_reset_all();
    editor_feed_line("// header");
    editor_feed_line("// pre-vertex");
    editor_feed_line("  glVertex3f(1, 0, 0);");
    editor_feed_line("// about to define func");

    set_input("func0() {");
    /* Drive the migration through editor_try_commit_block_structs since
     * func_def isn't in repl_compile_dispatch yet. */
    editor_try_commit_block_structs();

    /* After the migration, editor_commit_func_decl_resume_peek
     * should reflect the published delta. */
    int published = editor_commit_func_decl_resume_peek();
    ASSERT_INT("resume delta published by func_def", published, 1);

    /* Close the func block. The compile reads the delta (peek);
     * since end_type is CMD_FUNC_END, take + advance fires. */
    set_input("}");
    editor_try_commit_close_brace();

    /* The global is consumed (cleared) on FUNC_END close. */
    int post_consume = editor_commit_func_decl_resume_peek();
    ASSERT_INT("resume delta cleared after func close-brace",
               post_consume, 0);
}

static void test_if_block_condition_eval_uses_context_predef(void) {
    glr_ctrl_reset_all();

    int live_t = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("live t exists", live_t >= 0);
    if (live_t >= 0)
        g_predef_vars_mut[live_t].value = 0.0f;

    ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
    ExprVar synthetic_predef[1] = { { "t", 3.0f } };
    ctx.predef.vars = synthetic_predef;
    ctx.predef.count = 1;

    ReplIfBlockKernel kernel;
    char err[128] = "";
    ReplCompileResult r = repl_compile_if_block_kernel(
        "if(t) {", &ctx, &kernel, err, sizeof(err));

    ASSERT_INT("if-block synthetic predef compile OK", r, REPL_COMPILE_OK);
    ASSERT_TRUE("if-block synthetic predef is valid", kernel.valid);
    ASSERT_FLOAT("if-block condition uses ctx predef",
                 kernel.ib.args[0], 3.0f, 1e-6f);
}

/* ---- Function-scoped locals (scoped-local-variables, phase 1) ------- */

/* Commit `text` as a declaration with the cursor on source row
 * `edit_line`, overwrite mode. Returns 1 if compile + apply both
 * succeeded, 0 if compile rejected it (err receives the diagnostic). */
static int commit_decl_at(const char *text, int edit_line,
                          char *err, int err_size) {
    ReplCompileContext ctx;
    ReplCompiledChange change;

    editor_state_edit_line_set(edit_line);
    editor_insert_mode_set(0);
    set_input(text);
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    if (err && err_size > 0) err[0] = '\0';
    if (repl_compile_float_decl(text, &ctx, &change, err, err_size)
            != REPL_COMPILE_OK)
        return 0;
    return editor_commit_apply_external_change(&change, 0, 0);
}

/* func0(r) { glVertex3f(r, 0, 0); } — rows 0..2, cursor left on row 1. */
static void seed_one_func(void) {
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
}

/* A plain `float` inside a function body declares a local: it takes no
 * predef slot, carries the REPL_VAR_IDX_LOCAL marker, drops `static`
 * from its canonical text, and hoists to the top of that body. */
static void test_local_decl_inside_func(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_one_func();

    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);
    set_input("float u;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("local decl compiles",
               repl_compile_float_decl("float u;", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("local decl inserts", change.kind, REPL_COMPILED_INSERT_ONE);
    ASSERT_INT("local decl lands at the body top", change.pos, 1);
    ASSERT_INT("local decl emits no predef ops", change.predef_op_count, 0);
    ASSERT_INT("local decl row is marked local",
               change.cmds[0].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_STR("local decl text drops static and indents to the body",
               change.text[0], "    float u;");
    ASSERT_STR("local decl commit message names the storage and function",
               change.commit_message, "declared local u in func0");

    ASSERT_INT("local decl applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("local takes no predef slot", repl_eval_find_predef_var_idx("u"), -1);
    ASSERT_INT("local decl row is a CMD_VAR_DECLARE",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
}

/* `static` selects storage and beats cursor position: typed from inside a
 * function body it still produces a document-top global. */
static void test_static_keyword_selects_global_from_inside_func(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_one_func();

    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);
    set_input("static float g;");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("static decl from inside a func compiles",
               repl_compile_float_decl("static float g;", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("static decl lands at the document top", change.pos, 0);
    ASSERT_INT("static decl row is global", change.cmds[0].var_idx, 0);
    ASSERT_STR("static decl keeps the keyword", change.text[0], "  static float g;");
    ASSERT_INT("static decl applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_TRUE("static decl from inside a func takes a predef slot",
                repl_eval_find_predef_var_idx("g") >= 0);
}

/* Locals take no initializer in V1. Both spellings must produce the
 * initializer diagnostic — `= param` used to die on unknown-identifier
 * validation before any local diagnostic could run. */
static void test_local_decl_rejects_initializer(void) {
    char err[REPL_STATUS_TEXT_MAX];

    seed_one_func();
    ASSERT_INT("local decl with literal initializer is rejected",
               commit_decl_at("float u = 1;", 1, err, sizeof(err)), 0);
    ASSERT_TRUE("literal initializer diagnostic names the rule",
                strstr(err, "cannot have an initializer") != NULL);

    seed_one_func();
    ASSERT_INT("local decl initialized from a parameter is rejected",
               commit_decl_at("float u = r;", 1, err, sizeof(err)), 0);
    ASSERT_TRUE("parameter initializer gets the initializer diagnostic, "
                "not unknown-identifier",
                strstr(err, "cannot have an initializer") != NULL);
}

/* @tune / @config need a variable-panel slot, which a local does not have. */
static void test_local_decl_rejects_tune_tag(void) {
    char err[REPL_STATUS_TEXT_MAX];

    seed_one_func();
    ASSERT_INT("@tune on a local is rejected",
               commit_decl_at("float u; // @tune", 1, err, sizeof(err)), 0);
    ASSERT_TRUE("@tune diagnostic points at static float",
                strstr(err, "static float") != NULL);
}

/* Same scope in C, so these are redefinitions rather than shadowing. */
static void test_local_decl_rejects_same_scope_redefinition(void) {
    char err[REPL_STATUS_TEXT_MAX];

    seed_one_func();
    ASSERT_INT("a local colliding with a parameter is rejected",
               commit_decl_at("float r;", 1, err, sizeof(err)), 0);
    ASSERT_TRUE("parameter collision is reported as such",
                strstr(err, "already a parameter") != NULL);

    seed_one_func();
    ASSERT_INT("first local commits", commit_decl_at("float u;", 1, err, sizeof(err)), 1);
    ASSERT_INT("a second local of the same name is rejected",
               commit_decl_at("float u;", 2, err, sizeof(err)), 0);
    ASSERT_TRUE("duplicate local is reported as such",
                strstr(err, "already declared in this function") != NULL);
}

/* Outer scopes may be shadowed — that is ordinary C, and eval_primary
 * already resolves innermost-first. */
static void test_local_decl_allows_shadowing_outer_scopes(void) {
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("static float x;");
    editor_feed_line("func0(r) {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("a local may shadow a global",
               commit_decl_at("float x;", 2, err, sizeof(err)), 1);
    ASSERT_TRUE("the shadowed global keeps its slot",
                repl_eval_find_predef_var_idx("x") >= 0);

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    ASSERT_INT("a local may share a name with a loop iterator",
               commit_decl_at("float i;", 2, err, sizeof(err)), 1);
    ASSERT_INT("the local hoists to the function-body top, above the loop",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
    ASSERT_INT("the hoisted row is a local",
               repl_state_document_cmds_mut()[1].var_idx, REPL_VAR_IDX_LOCAL);
}

/* A declaration typed at depth relocates to the owning function's
 * prologue, exactly as a top-level decl relocates to the document top. */
static void test_local_decl_hoists_from_nested_block(void) {
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");

    ASSERT_INT("a decl typed inside a nested for commits",
               commit_decl_at("float u;", 2, err, sizeof(err)), 1);
    ASSERT_INT("it hoists to the function prologue, not the loop body",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
    ASSERT_STR("and takes the body's indent",
               editor_buffer_line(1), "    float u;");
    ASSERT_INT("the loop header follows it",
               repl_state_document_cmds_mut()[2].type, CMD_FOR_BEGIN);
}

/* Assignment resolves lexically: a local target carries the sentinel and
 * stages no predef op; PARAM and LOOP stay unwritable even when an outer
 * binding of the same name exists. */
static void test_var_assign_resolves_scoped_targets(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_one_func();
    ASSERT_INT("local decl commits", commit_decl_at("float u;", 1, err, sizeof(err)), 1);

    editor_state_edit_line_set(2);
    editor_insert_mode_set(1);
    set_input("u = 2");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("assignment to a local compiles",
               repl_compile_var_assign("u = 2", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("assignment to a local carries the local sentinel",
               change.cmds[0].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_INT("assignment to a local stages no predef op",
               change.predef_op_count, 0);

    /* A loop iterator is not writable, even with an outer local of the
     * same name — the innermost binding decides. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(x, 0, 3) {");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    ASSERT_INT("outer local x commits", commit_decl_at("float x;", 4, err, sizeof(err)), 1);

    editor_state_edit_line_set(3);
    editor_insert_mode_set(1);
    set_input("x = 2");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("assigning the shadowing loop iterator is rejected",
               repl_compile_var_assign("x = 2", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("loop-iterator assignment names the rule",
                strstr(err, "loop variables are constant") != NULL);
}

/* Retyping a local as `static float` is a storage conversion: refused
 * while the name is read, and a move (not a replace-in-place) when it
 * is not. */
static void test_local_to_global_conversion(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    /* Referenced: rejected. */
    seed_one_func();
    ASSERT_INT("local decl commits", commit_decl_at("float u;", 1, err, sizeof(err)), 1);
    editor_state_edit_line_set(2);
    editor_insert_mode_set(1);
    set_input("u = 2");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("assignment to the local compiles",
               repl_compile_var_assign("u = 2", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("assignment applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    ASSERT_INT("converting a referenced local is rejected",
               commit_decl_at("static float u;", 1, err, sizeof(err)), 0);
    ASSERT_TRUE("conversion rejection reuses the in-use wording",
                strstr(err, "is in use, cannot overwrite") != NULL);
    ASSERT_INT("the rejected conversion left no predef slot",
               repl_eval_find_predef_var_idx("u"), -1);

    /* Unreferenced: allowed, and the row moves to the document top. */
    seed_one_func();
    ASSERT_INT("local decl commits", commit_decl_at("float u;", 1, err, sizeof(err)), 1);
    ASSERT_INT("converting an unreferenced local succeeds",
               commit_decl_at("static float u;", 1, err, sizeof(err)), 1);
    ASSERT_INT("the converted row moved to the document top",
               repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
    ASSERT_INT("the converted row is global",
               repl_state_document_cmds_mut()[0].var_idx, 0);
    ASSERT_STR("the converted row is re-emitted in global form",
               editor_buffer_line(0), "  static float u;");
    ASSERT_TRUE("the converted name now holds a predef slot",
                repl_eval_find_predef_var_idx("u") >= 0);
    ASSERT_INT("the function header follows it",
               repl_state_document_cmds_mut()[1].type, CMD_FUNC_DEF);

    /* A same-name global is a duplicate in the same namespace, so it
     * still blocks the conversion. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("static float u;");
    editor_feed_line("func0(r) {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("shadowing local commits", commit_decl_at("float u;", 2, err, sizeof(err)), 1);
    ASSERT_INT("converting onto an existing global is rejected",
               commit_decl_at("static float u;", 2, err, sizeof(err)), 0);
    ASSERT_TRUE("duplicate-global wording",
                strstr(err, "already declared") != NULL);
}

/* Splitting a local must not silently promote it to a document-top
 * global: repl_compile_split_decl re-parses the row and rebuilds every
 * emitted line. */
static void test_split_decl_on_local_keeps_storage(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_one_func();
    ASSERT_INT("two-name local commits",
               commit_decl_at("float u, v;", 1, err, sizeof(err)), 1);

    ctx = repl_compile_context_from_live(1);
    ASSERT_INT("split compiles",
               repl_compile_split_decl(&ctx, 1, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("split emits two rows", change.count, 2);
    ASSERT_INT("split keeps row 0 local", change.cmds[0].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_INT("split keeps row 1 local", change.cmds[1].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_STR("split re-emits the local form", change.text[0], "    float u;");
    ASSERT_STR("split re-emits the local form", change.text[1], "    float v;");
}

/* Deleting a local decl releases no predef slot — undeclaring by name
 * would have taken a same-named global with it. */
static void test_delete_local_decl_spares_same_named_global(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("static float u;");
    editor_feed_line("func0(r) {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("shadowing local commits", commit_decl_at("float u;", 2, err, sizeof(err)), 1);

    ctx = repl_compile_context_from_live(2);
    ASSERT_INT("deleting the local row compiles",
               repl_compile_delete_range(2, 1, &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("deleting a local emits no UNDECLARE", change.predef_op_count, 0);
    ASSERT_INT("delete applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_TRUE("the same-named global survives",
                repl_eval_find_predef_var_idx("u") >= 0);
}

/* A local that is still read cannot be deleted; one whose only textual
 * occurrence sits under a shadowing iterator is not a reference at all. */
static void test_local_delete_guard_is_scope_aware(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_one_func();
    ASSERT_INT("local decl commits", commit_decl_at("float u;", 1, err, sizeof(err)), 1);
    editor_state_edit_line_set(2);
    editor_insert_mode_set(1);
    set_input("u = 2");
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    ASSERT_INT("assignment to the local compiles",
               repl_compile_var_assign("u = 2", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("assignment applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);

    ctx = repl_compile_context_from_live(1);
    ASSERT_INT("deleting a referenced local is rejected",
               repl_compile_delete_range(1, 1, &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("delete rejection says why",
                strstr(err, "still referenced") != NULL);

    /* Sole textual `x` lives under `for(x, ...)`, which shadows the
     * local — so the local is unreferenced and deletes cleanly. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(x, 0, 3) {");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    ASSERT_INT("shadowed local commits", commit_decl_at("float x;", 4, err, sizeof(err)), 1);

    ctx = repl_compile_context_from_live(1);
    ASSERT_INT("a local read only under a shadowing iterator deletes",
               repl_compile_delete_range(1, 1, &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
}

/* Capacity is a whole-function property: parameters + locals + the
 * deepest loop nesting must fit MAX_EXPR_VARS, because flatten_for_loop
 * prepends an iterator to a fresh array and drops the last outer binding
 * at the cap rather than erroring. */
static void test_local_decl_capacity_is_whole_function(void) {
    char err[REPL_STATUS_TEXT_MAX];
    int tail;

    /* One parameter, one loop level: 30 locals fit exactly (1 + 30 + 1). */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");

    tail = repl_state_document_count() - 1;
    ASSERT_INT("locals 1-8 commit",
               commit_decl_at("float a1, a2, a3, a4, a5, a6, a7, a8;",
                              tail, err, sizeof(err)), 1);
    tail = repl_state_document_count() - 1;
    ASSERT_INT("locals 9-16 commit",
               commit_decl_at("float b1, b2, b3, b4, b5, b6, b7, b8;",
                              tail, err, sizeof(err)), 1);
    tail = repl_state_document_count() - 1;
    ASSERT_INT("locals 17-24 commit",
               commit_decl_at("float c1, c2, c3, c4, c5, c6, c7, c8;",
                              tail, err, sizeof(err)), 1);
    tail = repl_state_document_count() - 1;
    ASSERT_INT("locals 25-30 commit",
               commit_decl_at("float d1, d2, d3, d4, d5, d6;",
                              tail, err, sizeof(err)), 1);

    tail = repl_state_document_count() - 1;
    ASSERT_INT("the local that would overflow the loop-inflated scope "
               "is rejected",
               commit_decl_at("float d7;", tail, err, sizeof(err)), 0);
    ASSERT_TRUE("capacity rejection names the scope budget",
                strstr(err, "function scope full") != NULL);
}

/* ---- Reverse binder guards + overwrite routes (phase 3) -------------- */

/* Rewrite the function header at `row` the way the editor does: through
 * the kernel, with allow_overwrite_at_pos set. A check placed on the
 * repl_compile_func_def wrapper alone would be bypassed here, because
 * src/editor/commit.c calls the kernel directly. */
static ReplCompileResult recompile_func_header(const char *header, int row,
                                               char *err, int err_size) {
    ReplFuncDefKernel kernel;
    ReplCompileContext ctx;

    editor_state_edit_line_set(row);
    editor_insert_mode_set(0);
    set_input(header);
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    err[0] = '\0';
    return repl_compile_func_def_kernel(header, &ctx, row, &kernel,
                                        err, err_size);
}

/* Compile a for-loop header at `row`, likewise through the kernel.
 * insert_mode 0 rewrites the header there; 1 opens a new loop. */
static ReplCompileResult compile_for_header_at(const char *header, int row,
                                               int insert_mode,
                                               char *err, int err_size) {
    ReplForLoopKernel kernel;
    ReplCompileContext ctx;

    editor_state_edit_line_set(row);
    editor_insert_mode_set(insert_mode);
    set_input(header);
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    err[0] = '\0';
    return repl_compile_for_loop_kernel(header, &ctx, &kernel, err, err_size);
}

/* A parameter and a local of the same body share one scope, so a header
 * edit colliding with an existing local is a redefinition — the mirror of
 * the rule Phase 1 enforces when the local is declared. */
static void test_param_rename_onto_local_is_rejected(void) {
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("float u;");
    editor_feed_line("glVertex3f(u, 0, 0);");
    editor_feed_line("}");

    ASSERT_INT("renaming a parameter onto an existing local is rejected",
               recompile_func_header("func0(u) {", 0, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("reported as a redefinition in this function",
                strstr(err, "already declared in this function") != NULL);
}

/* Shadowing does not make a parameter writable. Adding one over a body
 * assignment that currently writes a global would silently turn that row
 * into a write to a constant binding, so the header edit is refused. */
static void test_param_capturing_an_assignment_is_rejected(void) {
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("static float x;");
    editor_feed_line("func0(r) {");
    editor_feed_line("x = 5;");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");

    ASSERT_INT("row 1 is the function header",
               repl_state_document_cmds_mut()[1].type, CMD_FUNC_DEF);
    ASSERT_INT("a parameter that would capture a body assignment is rejected",
               recompile_func_header("func0(x) {", 1, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("the diagnostic names the capture and the rule",
                strstr(err, "would capture an assignment") != NULL &&
                strstr(err, "function parameters are constant") != NULL);
    ASSERT_INT("the header is unchanged",
               repl_state_document_cmds_mut()[1].type, CMD_FUNC_DEF);
}

/* The loop-header twin: renaming `for(i, ...)` to `for(x, ...)` over a
 * body that accumulates into an outer local `x` must not turn that row
 * into a write to the iterator. */
static void test_loop_rename_capturing_an_assignment_is_rejected(void) {
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("float x;");
    editor_feed_line("x = 1;");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("x = x + 1;");
    editor_feed_line("}");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");

    ASSERT_INT("row 3 is the loop header",
               repl_state_document_cmds_mut()[3].type, CMD_FOR_BEGIN);
    ASSERT_INT("renaming the iterator over a captured assignment is rejected",
               compile_for_header_at("for(x, 0, 3) {", 3, 0, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("the diagnostic names the capture and the rule",
                strstr(err, "would capture an assignment") != NULL &&
                strstr(err, "loop variables are constant") != NULL);
    ASSERT_INT("the loop header is unchanged",
               repl_state_document_cmds_mut()[3].type, CMD_FOR_BEGIN);
}

/* Ordinary shadowing stays legal. This feature must not quietly ban what
 * the language already allowed, nor what C allows. */
static void test_shadowing_without_capture_is_accepted(void) {
    char err[REPL_STATUS_TEXT_MAX];

    /* A parameter may shadow a global — pre-existing behavior. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("static float i;");
    editor_feed_line("func0(r) {");
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("a parameter may still shadow a global",
               recompile_func_header("func0(i) {", 1, err, sizeof(err)),
               REPL_COMPILE_OK);

    /* A global may be declared under an existing loop iterator. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("a global may still be declared under a loop iterator",
               commit_decl_at("static float i;", 1, err, sizeof(err)), 1);

    /* A loop iterator may shadow a function local it does not write. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("float x;");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    ASSERT_INT("row 2 is the loop header",
               repl_state_document_cmds_mut()[2].type, CMD_FOR_BEGIN);
    ASSERT_INT("an iterator may shadow a local it does not write",
               compile_for_header_at("for(x, 0, 3) {", 2, 0, err, sizeof(err)),
               REPL_COMPILE_OK);
}

/* One parameter, one loop, thirty locals: peak scope occupancy is exactly
 * MAX_EXPR_VARS (1 + 30 + 1). Leaves the document at that saturation
 * point for the two capacity cases below. */
static void seed_saturated_func(void) {
    static const char *const decls[] = {
        "float a1, a2, a3, a4, a5, a6, a7, a8;",
        "float b1, b2, b3, b4, b5, b6, b7, b8;",
        "float c1, c2, c3, c4, c5, c6, c7, c8;",
        "float d1, d2, d3, d4, d5, d6;",
    };
    char err[REPL_STATUS_TEXT_MAX];

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(r) {");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    for (int i = 0; i < 4; i++)
        ASSERT_INT("saturating decl commits",
                   commit_decl_at(decls[i], repl_state_document_count() - 1,
                                  err, sizeof(err)), 1);
}

/* Capacity is a whole-function property, so every binder edit is measured
 * against the same expression — not just the one that declares a local. */
static void test_capacity_blocks_later_binder_edits(void) {
    char err[REPL_STATUS_TEXT_MAX];
    int loop_row = -1;

    seed_saturated_func();
    ASSERT_INT("one more parameter overflows the frame",
               recompile_func_header("func0(r, s) {", 0, err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("capacity rejection names the scope budget",
                strstr(err, "function scope full") != NULL);
    ASSERT_INT("renaming a parameter without adding one still fits",
               recompile_func_header("func0(q) {", 0, err, sizeof(err)),
               REPL_COMPILE_OK);

    /* A nested loop adds a level the outer bindings must survive:
     * flatten_for_loop would otherwise drop the last one at the cap and a
     * live local would read 0 mid-body. */
    seed_saturated_func();
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].type == CMD_FOR_BEGIN) {
            loop_row = i;
            break;
        }
    }
    ASSERT_TRUE("found the existing loop", loop_row >= 0);
    ASSERT_INT("a nested loop at the cap is rejected at compile time",
               compile_for_header_at("for(j, 0, 2) {", loop_row + 1, 1,
                                     err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("capacity rejection names the scope budget",
                strstr(err, "function scope full") != NULL);
}

/* func0(a) { float u; u = a; glVertex3f(u, 0, 0); } — rows 0 head,
 * 1 decl, 2 assign, 3 vertex, 4 close. `u` is read, so row 1 cannot be
 * replaced. */
static void seed_func_with_used_local(void) {
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(a) {");
    editor_feed_line("float u;");
    editor_feed_line("u = a;");
    editor_feed_line("glVertex3f(u, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("row 1 is the local declaration",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
}

/* All five declaration-replacement routes share one guard. These are the
 * two that had no check at all: a local's binding *is* its prologue row,
 * so a raw replace would leave its readers bound to nothing. */
static void test_raw_replace_routes_respect_the_guard(void) {
    /* Route: retype the decl row as a GL command. */
    seed_func_with_used_local();
    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);
    ASSERT_INT("overwriting a used local decl with a GL command is refused",
               editor_feed_line("glVertex3f(9, 0, 0);"), 0);
    ASSERT_INT("the declaration row survives",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);

    /* Route: Enter over the decl row. */
    seed_func_with_used_local();
    editor_state_edit_line_set(1);
    editor_insert_mode_set(0);
    set_input("glVertex3f(9, 0, 0);");
    editor_handle_key('\r', 0, 0);
    ASSERT_INT("Enter over a used local decl leaves it in place",
               repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
    ASSERT_INT("and it is still the same local declaration",
               repl_state_document_cmds_mut()[1].var_idx, REPL_VAR_IDX_LOCAL);
}

/* The var-assign cascade: refuse when a dropped name is read, allow when
 * it is not. The success half is the one that would regress silently —
 * the slot rebase reads REPL_VAR_IDX_LOCAL as failure, so running it
 * ungated rejects a perfectly legal edit. */
static void test_overwrite_local_decl_with_assignment(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    /* Refused: row 2 declares `used`, which rows 3 and 4 read. The
     * assignment's own target `lead` is declared on row 1, so it resolves
     * — the rejection is the overwrite guard's, not a lookup failure. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(a) {");
    editor_feed_line("float lead;");
    editor_feed_line("float used;");
    editor_feed_line("used = a;");
    editor_feed_line("glVertex3f(used, 0, 0);");
    editor_feed_line("}");
    ASSERT_INT("row 2 is the second local declaration",
               repl_state_document_cmds_mut()[2].type, CMD_VAR_DECLARE);

    editor_state_edit_line_set(2);
    editor_insert_mode_set(0);
    set_input("lead = 9");
    ctx = repl_compile_context_from_live(2);
    ASSERT_INT("overwriting a used local decl with an assignment is refused",
               repl_compile_var_assign("lead = 9", &ctx, &change,
                                       err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("refusal names the in-use variable",
                strstr(err, "'used' is in use") != NULL);

    /* Allowed: row 2 declares `spare`, which nothing reads. */
    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(a) {");
    editor_feed_line("float u;");
    editor_feed_line("float spare;");
    editor_feed_line("u = a;");
    editor_feed_line("glVertex3f(u, 0, 0);");
    editor_feed_line("}");

    editor_state_edit_line_set(2);
    editor_insert_mode_set(0);
    set_input("u = 9");
    ctx = repl_compile_context_from_live(2);
    ASSERT_INT("overwriting an unused local decl with an assignment is allowed",
               repl_compile_var_assign("u = 9", &ctx, &change, err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("the assignment targets the local, not a predef slot",
               change.cmds[0].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_INT("and stages no predef op", change.predef_op_count, 0);
    ASSERT_INT("it applies", editor_commit_apply_external_change(&change, 0, 0), 1);
}

/* A CMD_VAR_DECLARE inside a block could not arise through normal user
 * input before this feature, so the block-batch comment-toggle path had
 * no coverage for it. It can now. */
static void test_block_comment_toggle_over_a_local_decl(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;

    seed_func_with_used_local();

    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("commenting out the whole function compiles",
               repl_compile_toggle_comment(0, "// ", &ctx, &change,
                                           err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_TRUE("it is a block batch, not a single row", change.count > 1);
    ASSERT_INT("no UNDECLARE is emitted for the local in the body",
               change.predef_op_count, 0);
    ASSERT_INT("it applies", editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("the declaration row is now a comment",
               repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
}

/* The local delete guard is bounded to its own function body: a same-name
 * local in another function is a different variable and must not block
 * the delete. */
static void test_local_delete_guard_is_bounded_to_its_body(void) {
    char err[REPL_STATUS_TEXT_MAX];
    ReplCompileContext ctx;
    ReplCompiledChange change;
    int func1_row = -1;

    glr_ctrl_reset_all();
    repl_func_alias_clear_all();
    editor_feed_line("func0(a) {");
    editor_feed_line("float u;");
    editor_feed_line("u = a;");
    editor_feed_line("glVertex3f(u, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("func1(b) {");
    editor_feed_line("float u;");
    editor_feed_line("glVertex3f(2, 0, 0);");
    editor_feed_line("}");

    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *c = &repl_state_document_cmds_mut()[i];
        if (c->type == CMD_FUNC_DEF && (int)c->args[0] == 1) {
            func1_row = i;
            break;
        }
    }
    ASSERT_TRUE("found func1", func1_row >= 0);
    ASSERT_INT("func1's local sits right after its header",
               repl_state_document_cmds_mut()[func1_row + 1].type,
               CMD_VAR_DECLARE);

    ctx = repl_compile_context_from_live(func1_row + 1);
    ASSERT_INT("deleting func1's unreferenced local is allowed even though "
               "func0 reads a local of the same name",
               repl_compile_delete_range(func1_row + 1, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("and emits no UNDECLARE", change.predef_op_count, 0);
}

int main(void) {
    test_compile_float_decl_failure_is_pure();
    test_compile_float_decl_trailing_comment_no_semicolon();
    test_float_decl_add_comment_to_existing();
    test_split_decl_basic();
    test_split_decl_inits_and_comment();
    test_split_decl_single_name_no_change();
    test_split_decl_non_decl_no_change();
    test_split_decl_via_editor_entry();
    test_compile_var_assign_failure_is_pure();
    test_compile_no_change_leaves_state();
    test_compile_apply_updates_both();
    test_compile_apply_var_assign_updates_value();
    test_overwrite_decl_with_assign_preserves_set_value();
    test_overwrite_earlier_decl_with_later_assign_rebases_slot();
    test_overwrite_decl_ignores_shadowed_param_refs();
    test_overwrite_assign_ignores_shadowed_param_refs();
    test_var_assign_rejects_function_param_target();
    test_var_assign_rejects_shadowed_function_param_target();
    test_delete_range_ignores_shadowed_param_refs();
    test_capacity_failure_is_atomic();
    test_reformat_keeps_buffer_and_store_aligned();
    test_set_predef_value_rewrites_decl_not_assignments();
    test_set_predef_value_rewrites_declaration_initializer();
    test_set_predef_value_adds_declaration_initializer();
    test_set_predef_value_rewrites_declaration_and_keeps_expression_sources();
    test_set_predef_value_does_not_rewrite_assignment_without_decl();
    test_set_predef_value_live_only_without_source();
    test_compile_set_predef_value_live_leaves_declaration();
    test_compile_persist_predef_value_rewrites_declaration();
    test_compile_persist_predef_value_without_declaration();
    test_compile_predef_value_split_rejects_undeclared();
    test_set_predef_value_marks_flat_dirty_only_on_change();
    test_orchestration_compile_failure_returns_diagnostic();
    test_orchestration_no_change_falls_through();
    test_orchestration_success_returns_message();
    test_func_def_comment_relocation();
    test_func_def_blank_line_relocation();
    test_func_def_after_blank_separated_decls();
    test_func_def_resume_publish_consumed_by_close_brace();
    test_if_block_condition_eval_uses_context_predef();
    test_local_decl_inside_func();
    test_static_keyword_selects_global_from_inside_func();
    test_local_decl_rejects_initializer();
    test_local_decl_rejects_tune_tag();
    test_local_decl_rejects_same_scope_redefinition();
    test_local_decl_allows_shadowing_outer_scopes();
    test_local_decl_hoists_from_nested_block();
    test_var_assign_resolves_scoped_targets();
    test_local_to_global_conversion();
    test_split_decl_on_local_keeps_storage();
    test_delete_local_decl_spares_same_named_global();
    test_local_delete_guard_is_scope_aware();
    test_local_decl_capacity_is_whole_function();
    test_param_rename_onto_local_is_rejected();
    test_param_capturing_an_assignment_is_rejected();
    test_loop_rename_capturing_an_assignment_is_rejected();
    test_shadowing_without_capture_is_accepted();
    test_capacity_blocks_later_binder_edits();
    test_raw_replace_routes_respect_the_guard();
    test_overwrite_local_decl_with_assignment();
    test_block_comment_toggle_over_a_local_decl();
    test_local_delete_guard_is_bounded_to_its_body();

    /* [P1] regression: alias registration must roll back on parse
     * failure. Pre-fix, repl_compile_func_def called
     * repl_func_alias_set BEFORE parse_repl_func_signature, so a
     * malformed `name(args` (no `{`, no closing `)`) would leave
     * `name` registered as a func alias even though no CMD_FUNC_DEF
     * was created. Subsequent `name()` calls would erroneously
     * resolve. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Call repl_compile_func_def directly with a header that
         * passes the quick-reject (both `(` and `{`) and whose pending
         * alias resolves, but where parse_repl_func_signature fails on
         * the param list (`123` is not a valid identifier). Pre-fix,
         * the alias `badRoll` would leak to the global alias table even
         * though no CMD_FUNC_DEF was created. */
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        ReplCompiledChange change;
        char err[128] = "";
        ReplCompileResult r = repl_compile_func_def(
            "badRoll(123) {",
            &ctx, &change, err, sizeof(err));

        ASSERT_TRUE("[P1] func_def parse failure returns OK + NO_CHANGE",
                    r == REPL_COMPILE_OK &&
                    change.kind == REPL_COMPILED_NO_CHANGE);

        /* The fix: alias must NOT be registered after parse failure. */
        int slot = repl_func_alias_lookup_slot("badRoll");
        ASSERT_TRUE("[P1] failed func_def parse does not leak alias",
                    slot < 0);
    }

    /* [P2] regression: repl_compile_func_def must reject duplicate
     * funcN definitions. The editor's editor_compile_func_def has
     * this check (src/editor/commit.c lines 670-681); the lean
     * validator was missing it, so an imported file with two
     * `func0() { ... }` blocks would have been accepted silently
     * where editor_feed_line() would have rejected the second. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Establish slot 0: feed a first func0 def via the lean
         * loader so it lands as a real CMD_FUNC_DEF in the
         * document. */
        char err[128] = "";
        ASSERT_TRUE("[P2] first func0 def loads cleanly",
                    repl_load_apply_line("func0() {", err, sizeof(err), NULL));
        ASSERT_TRUE("[P2] first func0 def populated err is empty",
                    err[0] == '\0');

        /* Verify slot 0 is a CMD_FUNC_DEF for fn=0. */
        int found_first = 0;
        for (int i = 0; i < repl_state_document_count(); i++) {
            const GLCmd *c = &repl_state_document_cmds_mut()[i];
            if (c->valid && c->type == CMD_FUNC_DEF && (int)c->args[0] == 0) {
                found_first = 1;
                break;
            }
        }
        ASSERT_TRUE("[P2] first func0 def visible in document",
                    found_first);

        /* Now try a second func0 def. repl_compile_func_def directly
         * (not through the loader, since the loader's append-at-end
         * semantics would still produce the duplicate). */
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        ReplCompiledChange change;
        err[0] = '\0';
        ReplCompileResult r = repl_compile_func_def(
            "func0() {", &ctx, &change, err, sizeof(err));

        ASSERT_TRUE("[P2] duplicate func0 def returns ERROR",
                    r == REPL_COMPILE_ERROR);
        ASSERT_TRUE("[P2] duplicate func0 def diagnostic mentions func0",
                    strstr(err, "func0") != NULL &&
                    strstr(err, "already defined") != NULL);
    }

    /* [P2 editor] regression: editor_compile_func_def must not publish
     * aliases on parse failure. The editor wrapper used to register a
     * new alias before parse_repl_func_signature ran, so a malformed
     * `name(args` line in the user-facing path could leave the alias
     * registered. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        EditorCommitPlan plan;
        ReplCompileResult r;
        char err[16] = "stale";

        editor_commit_plan_init(&plan);
        r = editor_compile_close_brace("glVertex3f(1, 1, 1);",
                                       &ctx, &plan, err, sizeof(err));
        ASSERT_TRUE("[P2 editor] close-brace NO_CHANGE clears err",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE &&
                    err[0] == '\0');

        strcpy(err, "stale");
        editor_commit_plan_init(&plan);
        r = editor_compile_if_block("glVertex3f(1, 1, 1);",
                                    &ctx, &plan, err, sizeof(err));
        ASSERT_TRUE("[P2 editor] if-block NO_CHANGE clears err",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE &&
                    err[0] == '\0');

        strcpy(err, "stale");
        editor_commit_plan_init(&plan);
        r = editor_compile_func_def("glVertex3f(1, 1, 1);",
                                    &ctx, &plan, err, sizeof(err));
        ASSERT_TRUE("[P2 editor] func-def NO_CHANGE clears err",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE &&
                    err[0] == '\0');

        strcpy(err, "stale");
        editor_commit_plan_init(&plan);
        r = editor_compile_for_loop("glVertex3f(1, 1, 1);",
                                    &ctx, &plan, err, sizeof(err));
        ASSERT_TRUE("[P2 editor] for-loop NO_CHANGE clears err",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE &&
                    err[0] == '\0');
    }

    /* [P2 editor] wrapper diagnostics are optional: failing
     * editor_compile_* calls must tolerate omitted or zero-sized err
     * buffers instead of unconditionally writing through them. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        EditorCommitPlan plan;
        ReplCompileResult r;
        char zero_err[1] = { 'x' };

        editor_commit_plan_init(&plan);
        r = editor_compile_close_brace("}", &ctx, &plan, NULL, 0);
        ASSERT_TRUE("[P2 editor] close-brace error allows NULL err",
                    r == REPL_COMPILE_ERROR);

        editor_commit_plan_init(&plan);
        r = editor_compile_if_block("if(", &ctx, &plan, zero_err, 0);
        ASSERT_TRUE("[P2 editor] if-block error allows zero-size err",
                    r == REPL_COMPILE_ERROR && zero_err[0] == 'x');

        char load_err[128] = "";
        ASSERT_TRUE("[P2 editor] setup bare func0 for optional err test",
                    repl_load_apply_line("func0() {", load_err, sizeof(load_err), NULL));
        ctx = repl_compile_context_from_live(editor_state_edit_line());

        editor_commit_plan_init(&plan);
        r = editor_compile_func_def("edDupAlias() {", &ctx, &plan, NULL, 0);
        ASSERT_TRUE("[P2 editor] func-def error allows NULL err",
                    r == REPL_COMPILE_ERROR);
        ASSERT_TRUE("[P2 editor] func-def error still rolls back alias",
                    repl_func_alias_lookup_slot("edDupAlias") < 0);

        editor_commit_plan_init(&plan);
        r = editor_compile_for_loop("for(", &ctx, &plan, zero_err, 0);
        ASSERT_TRUE("[P2 editor] for-loop error allows zero-size err",
                    r == REPL_COMPILE_ERROR && zero_err[0] == 'x');
    }

    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        EditorCommitPlan plan;
        editor_commit_plan_init(&plan);
        char err[128] = "";
        ReplCompileResult r = editor_compile_func_def(
            "edBadRoll(123) {",
            &ctx, &plan, err, sizeof(err));

        ASSERT_TRUE("[P2 editor] parse fail returns OK + NO_CHANGE",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE);
        ASSERT_TRUE("[P2 editor] parse fail does not leak alias",
                    repl_func_alias_lookup_slot("edBadRoll") < 0);
    }

    /* [P1 editor] regression: a failed overwrite/rename must restore
     * the previous alias, not merely clear the touched slot. The
     * editor path targets the current CMD_FUNC_DEF slot when
     * overwriting a header, so a malformed rename can replace an
     * existing alias before parse validation fails. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        char err[128] = "";
        ASSERT_TRUE("[P1 editor] original aliased func loads",
                    repl_load_apply_line("drawCube() {", err, sizeof(err), NULL));
        ASSERT_INT("[P1 editor] drawCube starts in slot 0",
                   repl_func_alias_lookup_slot("drawCube"), 0);

        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        EditorCommitPlan plan;
        editor_commit_plan_init(&plan);
        err[0] = '\0';
        ReplCompileResult r = editor_compile_func_def(
            "drawSphere(123) {", &ctx, &plan, err, sizeof(err));

        ASSERT_TRUE("[P1 editor] malformed rename returns OK + NO_CHANGE",
                    r == REPL_COMPILE_OK &&
                    plan.change.kind == REPL_COMPILED_NO_CHANGE);
        ASSERT_INT("[P1 editor] failed rename preserves old alias",
                   repl_func_alias_lookup_slot("drawCube"), 0);
        ASSERT_TRUE("[P1 editor] failed rename removes new alias",
                    repl_func_alias_lookup_slot("drawSphere") < 0);
    }

    /* [P2 editor] regression: editor_compile_func_def must not publish
     * aliases on duplicate-funcN failure. The old dup-check returned
     * ERROR after registering the alias, so a NEW alias for slot N —
     * where slot N already had a CMD_FUNC_DEF — leaked the alias. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Insert a bare func0 (no alias registered) so slot 0 is
         * "free" by repl_func_alias_first_free_slot's definition
         * but already occupied at the document level. */
        char err[128] = "";
        ASSERT_TRUE("[P2 editor] bare func0 loads cleanly",
                    repl_load_apply_line("func0() {", err, sizeof(err), NULL));

        /* Compile a NEW alias whose pending op would target slot 0
         * (first free) but whose dup check fails because fn=0 already
         * has a CMD_FUNC_DEF in the document. */
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        EditorCommitPlan plan;
        editor_commit_plan_init(&plan);
        err[0] = '\0';
        ReplCompileResult r = editor_compile_func_def(
            "edDupAlias() {",
            &ctx, &plan, err, sizeof(err));

        ASSERT_TRUE("[P2 editor] dup fail returns ERROR",
                    r == REPL_COMPILE_ERROR);
        ASSERT_TRUE("[P2 editor] dup fail diagnostic mentions func0",
                    strstr(err, "func0") != NULL &&
                    strstr(err, "already defined") != NULL);
        ASSERT_TRUE("[P2 editor] dup fail does not leak alias",
                    repl_func_alias_lookup_slot("edDupAlias") < 0);
    }

    /* [Phase 0] Pin the widened src/repl/load.h contract: setting
     * repl_state_edit_line to a mid-document index must insert the
     * line at that index (rather than appending to document_count).
     * This is what the tutorial runner relies on for label-targeted
     * steps that need to splice an instruction comment in front of
     * an earlier committed command. The behavior already matched;
     * Phase 0 only widened the documented contract and pins it. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();
        editor_state_input_reset();

        char err[128] = "";
        int el = repl_state_document_count();
        editor_insert_mode_set(0);
        ASSERT_TRUE("[Phase 0] seed line a loads",
                    repl_load_apply_line("glColor3f(1, 0, 0)", err, sizeof(err), &el));
        editor_insert_mode_set(0);
        ASSERT_TRUE("[Phase 0] seed line b loads",
                    repl_load_apply_line("glColor3f(0, 1, 0)", err, sizeof(err), &el));
        ASSERT_INT("[Phase 0] seeded doc has 2 lines",
                   repl_state_document_count(), 2);

        /* Mid-document insert at index 1: between the two color
         * commands. The widened contract allows edit_line in
         * [0, document_count]. */
        el = 1;
        editor_insert_mode_set(0);
        err[0] = '\0';
        int ok = repl_load_apply_line("// inserted in the middle",
                                      err, sizeof(err), &el);
        ASSERT_TRUE("[Phase 0] mid-document comment insert succeeds", ok);
        ASSERT_INT("[Phase 0] doc grew by exactly one row",
                   repl_state_document_count(), 3);

        EditorBufferView view = editor_buffer_view();
        const char *line0 = editor_buffer_view_line(view, 0);
        const char *line1 = editor_buffer_view_line(view, 1);
        const char *line2 = editor_buffer_view_line(view, 2);
        ASSERT_TRUE("[Phase 0] row 0 unchanged",
                    line0 && strstr(line0, "glColor3f(1") != NULL);
        ASSERT_TRUE("[Phase 0] inserted comment landed at row 1",
                    line1 && strstr(line1, "inserted in the middle") != NULL);
        ASSERT_TRUE("[Phase 0] original row 1 pushed down to row 2",
                    line2 && strstr(line2, "glColor3f(0") != NULL);

        /* And a plain GL command mid-document at index 0 — the
         * widened contract allows insertion at the very top. */
        el = 0;
        editor_insert_mode_set(0);
        err[0] = '\0';
        ASSERT_TRUE("[Phase 0] top-of-doc insert succeeds",
                    repl_load_apply_line("glColor3f(0, 0, 1)",
                                         err, sizeof(err), &el));
        ASSERT_INT("[Phase 0] doc grew to 4 rows",
                   repl_state_document_count(), 4);
        view = editor_buffer_view();
        const char *new_top = editor_buffer_view_line(view, 0);
        ASSERT_TRUE("[Phase 0] new top row is the blue color",
                    new_top && strstr(new_top, "glColor3f(0, 0, 1") != NULL);
    }

    /* [P1 loader] regression: repl_load_apply_line must preflight
     * via repl_apply_can_apply_compiled_change before mutating
     * predef vars / scratch ops / editor buffer. Pre-fix, a
     * capacity overflow at apply time would leave the predef var
     * already declared but no source CMD_VAR_DECLARE to back it. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Force the cmd-store to capacity so any insert fails. */
        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        char err[128] = "";
        int ok = repl_load_apply_line("float fooLeak;", err, sizeof(err), NULL);

        /* Restore count BEFORE asserting so the asserts can clean up
         * gracefully and subsequent tests inherit a clean slate. */
        *store.count = saved_count;

        ASSERT_TRUE("[P1 loader] capacity-fail returns 0", !ok);
        ASSERT_TRUE("[P1 loader] capacity-fail does not register predef var",
                    repl_eval_find_predef_var_idx("fooLeak") < 0);
    }

    /* [P1 loader] regression: alias registration is an apply-side op
     * and must not run when the loader's command-store apply fails.
     * Pre-fix, compile_func_def registered the alias before apply, then
     * repl_apply_compiled_change could fail at capacity and leave the
     * alias in the global table with no matching CMD_FUNC_DEF. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        char err[128] = "";
        int ok = repl_load_apply_line("leakOnFail() {",
                                      err, sizeof(err), NULL);
        int slot = repl_func_alias_lookup_slot("leakOnFail");

        *store.count = saved_count;

        ASSERT_TRUE("[P1 loader] func capacity-fail returns 0", !ok);
        ASSERT_TRUE("[P1 loader] func capacity-fail does not leak alias",
                    slot < 0);
    }

    /* [Finding 12] The non-editor loader exposes its structured-change
     * transaction boundary. The helper reports the post-apply cursor on
     * success and rejects at preflight before source/predef mutation. */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        ReplCompiledChange change;
        repl_compiled_change_init(&change);
        ReplCompileContext ctx = repl_compile_context_from_live(0);
        ctx.insert_mode = 1;
        char err[128] = "";
        ReplCompileResult r = repl_compile_dispatch("float txValue = 2;",
                                                     &ctx, &change,
                                                     err, sizeof(err));
        ASSERT_INT("[F12 transaction] decl compiles", r, REPL_COMPILE_OK);
        ASSERT_INT("[F12 transaction] compile produced insert",
                   change.kind, REPL_COMPILED_INSERT_ONE);
        change.adjust_edit_line = 1;

        ReplLoadTransactionResult tx;
        ASSERT_TRUE("[F12 transaction] apply succeeds",
                    repl_load_apply_compiled_change_transaction(&change,
                                                               0, &tx));
        ASSERT_INT("[F12 transaction] applied flag", tx.applied, 1);
        ASSERT_INT("[F12 transaction] wrote source", tx.wrote_local, 1);
        ASSERT_INT("[F12 transaction] cursor advanced", tx.next_cursor, 1);
        ASSERT_INT("[F12 transaction] document count",
                   repl_state_document_count(), 1);
        ASSERT_INT("[F12 transaction] source line count",
                   source_document_view().line_count, 1);
        ASSERT_TRUE("[F12 transaction] predef registered",
                    repl_eval_find_predef_var_idx("txValue") >= 0);
    }

    {
        glr_ctrl_reset_all();

        ReplCompiledChange change;
        repl_compiled_change_init(&change);
        ReplCompileContext ctx = repl_compile_context_from_live(0);
        ctx.insert_mode = 1;
        char err[128] = "";
        ReplCompileResult r = repl_compile_dispatch("float txNoLeak;",
                                                     &ctx, &change,
                                                     err, sizeof(err));
        ASSERT_INT("[F12 transaction] no-leak decl compiles",
                   r, REPL_COMPILE_OK);
        change.adjust_edit_line = 1;

        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        ReplLoadTransactionResult tx;
        int ok = repl_load_apply_compiled_change_transaction(&change, 0, &tx);

        *store.count = saved_count;

        ASSERT_TRUE("[F12 transaction] preflight rejects", !ok);
        ASSERT_INT("[F12 transaction] reject applied flag", tx.applied, 0);
        ASSERT_INT("[F12 transaction] reject wrote no source",
                   tx.wrote_local, 0);
        ASSERT_INT("[F12 transaction] reject kept cursor",
                   tx.next_cursor, 0);
        ASSERT_INT("[F12 transaction] reject source line count",
                   source_document_view().line_count, 0);
        ASSERT_TRUE("[F12 transaction] reject predef not registered",
                    repl_eval_find_predef_var_idx("txNoLeak") < 0);
    }

    /* #49 regression: repl_apply_compiled_change runs the preflight
     * internally so a malformed change cannot half-apply. Pre-fix the
     * pre-insert delete would fire, then INSERT_MANY would either
     * silently clamp the pos or fail — leaving the cmd-store mutated
     * but not in the requested shape. These tests bypass the wrapper
     * (editor_commit_apply_external_change) and call the bare
     * repl_apply_compiled_change so they pin the apply-level contract,
     * not the wrapper's preflight. */
    {
        glr_ctrl_reset_all();
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("glColor3f(0, 1, 0);");
        editor_feed_line("glColor3f(0, 0, 1);");

        int pre_count = repl_state_document_count();
        ASSERT_INT("[#49] setup: 3 rows", pre_count, 3);

        ReplCompiledChange change;

        /* (a) INSERT_MANY with count > MAX_COMMIT_CMDS — apply must
         * reject before mutating the cmd-store. */
        repl_compiled_change_init(&change);
        change.kind = REPL_COMPILED_INSERT_MANY;
        change.pos = 0;
        change.count = MAX_COMMIT_CMDS + 1;
        ASSERT_INT("[#49] over-MAX_COMMIT_CMDS rejected by apply",
                   repl_apply_compiled_change(&change, NULL), 0);
        ASSERT_INT("[#49] over-MAX_COMMIT_CMDS no mutation",
                   repl_state_document_count(), pre_count);

        /* (b) REPLACE_ONE with out-of-bounds pos — apply rejects
         * cleanly rather than relying on the store's range check. */
        repl_compiled_change_init(&change);
        change.kind = REPL_COMPILED_REPLACE_ONE;
        change.pos = pre_count + 99;
        change.cmds[0].type = CMD_COLOR3F;
        change.cmds[0].valid = 1;
        change.cmds[0].args[0] = 0.5f;
        change.cmds[0].args[1] = 0.5f;
        change.cmds[0].args[2] = 0.5f;
        change.cmds[0].num_args = 3;
        ASSERT_INT("[#49] OOB replace rejected by apply",
                   repl_apply_compiled_change(&change, NULL), 0);
        ASSERT_INT("[#49] OOB replace no mutation",
                   repl_state_document_count(), pre_count);

        /* (c) Compound pre-delete + REPLACE_ONE: the preflight
         * disallows mixing pre-delete with REPLACE_ONE. Pre-fix the
         * delete would happen and then the replace would fall through
         * the switch with no follow-up, leaving rows missing. */
        repl_compiled_change_init(&change);
        change.kind = REPL_COMPILED_REPLACE_ONE;
        change.pos = 0;
        change.delete_pos = 1;
        change.delete_count = 1;
        change.cmds[0].type = CMD_COLOR3F;
        change.cmds[0].valid = 1;
        change.cmds[0].num_args = 3;
        ASSERT_INT("[#49] pre-delete + replace rejected by apply",
                   repl_apply_compiled_change(&change, NULL), 0);
        ASSERT_INT("[#49] pre-delete + replace no mutation",
                   repl_state_document_count(), pre_count);

        /* (d) Pre-delete with out-of-bounds range — apply rejects
         * before performing the partial delete. */
        repl_compiled_change_init(&change);
        change.kind = REPL_COMPILED_INSERT_ONE;
        change.pos = 0;
        change.delete_pos = pre_count + 1;
        change.delete_count = 1;
        change.cmds[0].type = CMD_COLOR3F;
        change.cmds[0].valid = 1;
        change.cmds[0].num_args = 3;
        ASSERT_INT("[#49] OOB pre-delete rejected by apply",
                   repl_apply_compiled_change(&change, NULL), 0);
        ASSERT_INT("[#49] OOB pre-delete no mutation",
                   repl_state_document_count(), pre_count);

        /* (e) Sanity: a well-formed change still applies. */
        repl_compiled_change_init(&change);
        change.kind = REPL_COMPILED_INSERT_ONE;
        change.pos = pre_count;
        change.cmds[0].type = CMD_COLOR3F;
        change.cmds[0].valid = 1;
        change.cmds[0].args[0] = 1.0f;
        change.cmds[0].args[1] = 1.0f;
        change.cmds[0].args[2] = 1.0f;
        change.cmds[0].num_args = 3;
        ASSERT_INT("[#49] well-formed insert succeeds",
                   repl_apply_compiled_change(&change, NULL), 1);
        ASSERT_INT("[#49] well-formed insert grew count",
                   repl_state_document_count(), pre_count + 1);
    }

    /* Test alias resolution and rejected_keyword signal */
    {
        glr_ctrl_reset_all();
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        ReplCompiledChange change;
        int rejected_keyword = 0;
        char err[256];

        /* Case A: Bare predefined function like func0() { — not a custom alias, should return REPL_COMPILE_OK */
        repl_compiled_change_init(&change);
        rejected_keyword = 0;
        ReplCompileResult res = repl_compile_func_def_resolve_alias(&ctx, "func0() {", &change, &rejected_keyword, err, sizeof(err));
        ASSERT_INT("resolve_alias func0: returns OK", res, REPL_COMPILE_OK);
        ASSERT_INT("resolve_alias func0: rejected_keyword is false", rejected_keyword, 0);

        /* Case B: Reserved control keyword like if() { — rejected_keyword should be set to 1, out->kind set to NO_CHANGE, returns OK */
        repl_compiled_change_init(&change);
        rejected_keyword = 0;
        res = repl_compile_func_def_resolve_alias(&ctx, "if() {", &change, &rejected_keyword, err, sizeof(err));
        ASSERT_INT("resolve_alias if: returns OK", res, REPL_COMPILE_OK);
        ASSERT_INT("resolve_alias if: rejected_keyword is true", rejected_keyword, 1);
        ASSERT_INT("resolve_alias if: change kind is NO_CHANGE", change.kind, REPL_COMPILED_NO_CHANGE);

        /* Case C: Valid custom function like myfunc() { — should compile and pick a slot successfully */
        repl_compiled_change_init(&change);
        rejected_keyword = 0;
        res = repl_compile_func_def_resolve_alias(&ctx, "myfunc() {", &change, &rejected_keyword, err, sizeof(err));
        ASSERT_INT("resolve_alias myfunc: returns OK", res, REPL_COMPILE_OK);
        ASSERT_INT("resolve_alias myfunc: rejected_keyword is false", rejected_keyword, 0);
        ASSERT_TRUE("resolve_alias myfunc: picked slot", change.alias_op.slot >= 0);
        ASSERT_STR("resolve_alias myfunc: captured name", change.alias_op.name, "myfunc");
        ASSERT_TRUE("resolve_alias myfunc: compile kept alias table unchanged",
                    repl_func_alias_lookup_slot("myfunc") < 0);
    }

    /* Func alias slot exhaustion test */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Register 10 distinct custom aliases to fill up all slots. */
        for (int i = 0; i < REPL_FUNC_SLOT_COUNT; i++) {
            char name[32];
            snprintf(name, sizeof(name), "myfunc%d", i);
            int set_ok = repl_func_alias_set(i, name);
            ASSERT_TRUE("fill alias slot", set_ok == 1);
        }

        /* Verify all 10 slots are occupied. */
        ASSERT_INT("first free slot after fill", repl_func_alias_first_free_slot(), -1);

        /* Attempt to compile an 11th custom function def. */
        ReplCompileContext ctx = repl_compile_context_from_live(editor_state_edit_line());
        ReplCompiledChange change;
        repl_compiled_change_init(&change);
        char err[256] = "";
        ReplCompileResult r = repl_compile_func_def("myfunc10() {", &ctx, &change, err, sizeof(err));

        /* Verify that it was rejected with the slot exhaustion error. */
        ASSERT_INT("exhaustion compile result is ERROR", r, REPL_COMPILE_ERROR);
        ASSERT_TRUE("exhaustion error mentions free slots", strstr(err, "no free function slots") != NULL);

        /* Make sure it didn't leak. */
        ASSERT_TRUE("myfunc10 slot not registered", repl_func_alias_lookup_slot("myfunc10") < 0);
    }

    /* Func alias name collision test */
    {
        glr_ctrl_reset_all();
        repl_func_alias_clear_all();

        /* Set an alias in slot 0. */
        int set_ok = repl_func_alias_set(0, "myfunc0");
        ASSERT_TRUE("set slot 0 alias", set_ok == 1);

        /* Assign the same alias to a different slot (slot 1).
         * This should be rejected by repl_func_alias_set. */
        int collide_ok = repl_func_alias_set(1, "myfunc0");
        ASSERT_TRUE("collision set is rejected", collide_ok == 0);

        /* Verify that lookup still returns slot 0. */
        ASSERT_INT("lookup returns slot 0", repl_func_alias_lookup_slot("myfunc0"), 0);
    }

    /* Standalone else rejection test */
    {
        glr_ctrl_reset_all();
        ReplCompileContext ctx = repl_compile_context_from_live(0);
        ReplCompiledChange change;
        char err[256];

        /* Bare "else" should fail compilation with the specific error message */
        repl_compiled_change_init(&change);
        err[0] = '\0';
        ReplCompileResult r = repl_compile_dispatch("else", &ctx, &change, err, sizeof(err));
        ASSERT_INT("standalone else compile returns ERROR", r, REPL_COMPILE_ERROR);
        ASSERT_TRUE("standalone else error message matches",
                    strstr(err, "else must be on the same line as }: } else {") != NULL);

        /* "else {" should also fail compile */
        repl_compiled_change_init(&change);
        err[0] = '\0';
        r = repl_compile_dispatch("else {", &ctx, &change, err, sizeof(err));
        ASSERT_INT("standalone else with brace compile returns ERROR", r, REPL_COMPILE_ERROR);
        ASSERT_TRUE("standalone else with brace error message matches",
                    strstr(err, "else must be on the same line as }: } else {") != NULL);

        /* "else if (x < 5) {" should also fail compile */
        repl_compiled_change_init(&change);
        err[0] = '\0';
        r = repl_compile_dispatch("else if (x < 5) {", &ctx, &change, err, sizeof(err));
        ASSERT_INT("standalone else if compile returns ERROR", r, REPL_COMPILE_ERROR);
        ASSERT_TRUE("standalone else if error message matches",
                    strstr(err, "else must be on the same line as }: } else {") != NULL);

        /* "elsewhere {" should NOT fail with the else error because it has a different word */
        repl_compiled_change_init(&change);
        err[0] = '\0';
        r = repl_compile_dispatch("elsewhere {", &ctx, &change, err, sizeof(err));
        /* elsewhere is a custom function name, since we have free slots it should compile OK (returning REPL_COMPILE_OK) */
        ASSERT_INT("elsewhere compile returns OK", r, REPL_COMPILE_OK);
        ASSERT_TRUE("elsewhere does not set else error",
                    strstr(err, "else must be on the same line as }") == NULL);
    }

    return test_harness_report(&g_harness, "test_repl_compile");
}
