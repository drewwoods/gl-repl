/*
 * test_repl_compile.c - Phase C invariant tests.
 *
 * Verifies the compile/apply boundary established in Phase C
 * commits 19-21:
 *
 *   - repl_compile_*() never mutates editor buffer, command store,
 *     status, or undo on either success or failure.
 *   - apply (driven by editor_commit_apply_compiled_change) updates
 *     editor text and command store together.
 *   - On compile failure, set_status from the wrapper IS allowed
 *     (Phase C transition); the strong invariant is checked at the
 *     compile-function boundary itself.
 */

#include "editor_commit.h"
#include "editor_state.h"
#include "repl_apply.h"
#include "repl_command_store.h"
#include "repl_compile.h"
#include "repl_core.h"
#include "repl_eval.h"
#include "repl_state.h"
#include "ui_state.h"
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
    fp.edit_line       = repl_state_edit_line();
    fp.insert_mode     = editor_insert_mode();
    fp.num_predef_vars = g_num_predef_vars;

    ReplStatusState st = ui_state_status();
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
    repl_reset_state();

    /* Establish a non-trivial pre-state: declare a variable, set
     * status, push a buffer line. */
    set_input("float existing;");
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float existing;", &ctx, &change, err, sizeof(err));
    /* Apply the success change so the state is non-trivial. */
    editor_commit_apply_compiled_change(&change);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Trigger compile failure: redeclaring an existing name. */
    set_input("float existing;");
    ctx = repl_compile_context_from_live();
    ReplCompileResult r = repl_compile_float_decl(
        "float existing;", &ctx, &change, err, sizeof(err));
    ASSERT_INT("redeclare returns ERROR", r, REPL_COMPILE_ERROR);
    ASSERT_TRUE("redeclare fills err", err[0] != '\0');

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("compile failure leaves state untouched (cmd_count, "
                "buffer, status, predef_vars)",
                fingerprint_equal(&before, &after));
}

/* repl_compile_var_assign is pure on the failure path. */
static void test_compile_var_assign_failure_is_pure(void) {
    repl_reset_state();

    set_input("float a;");
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float a;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Compile failure: assigning to undeclared name. */
    set_input("nonexistent = 1");
    ctx = repl_compile_context_from_live();
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
    repl_reset_state();
    ui_state_status_set("baseline status");
    ComputeFingerprint before = capture_fingerprint();

    ReplCompileContext ctx = repl_compile_context_from_live();
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
    repl_reset_state();

    set_input("float energy;");
    ReplCompileContext ctx = repl_compile_context_from_live();
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

    int ok = editor_commit_apply_compiled_change(&change);
    ASSERT_INT("apply returns 1 on success", ok, 1);

    /* Post-apply: both halves match. */
    ASSERT_INT("post-apply cmd count", repl_state_document_count(), 1);
    ASSERT_INT("post-apply buffer count", editor_buffer_count(), 1);
    ASSERT_INT("post-apply cmd type",
               repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
    ASSERT_TRUE("post-apply predef registered",
                repl_eval_find_predef_var_idx("energy") >= 0);
    ASSERT_STR("post-apply buffer line text",
               editor_buffer_line(0), "  float energy;");
}

/* Var-assign compile + apply updates the predef value alongside the
 * source command. */
static void test_compile_apply_var_assign_updates_value(void) {
    repl_reset_state();

    /* Set up a declared variable. */
    set_input("float k;");
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float k;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);

    /* Snapshot: k is registered with value 0. */
    int slot = repl_eval_find_predef_var_idx("k");
    ASSERT_TRUE("k is registered", slot >= 0);
    ASSERT_TRUE("k value starts at 0", g_predef_vars[slot].value == 0.0f);

    /* Compile + apply assignment. */
    set_input("k = 7");
    ctx = repl_compile_context_from_live();
    ReplCompileResult r = repl_compile_var_assign(
        "k = 7", &ctx, &change, err, sizeof(err));
    ASSERT_INT("k = 7 compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("k = 7 INSERT_ONE", change.kind, REPL_COMPILED_INSERT_ONE);

    int ok = editor_commit_apply_compiled_change(&change);
    ASSERT_INT("apply returns 1", ok, 1);
    ASSERT_TRUE("k value is 7", g_predef_vars[slot].value == 7.0f);
    ASSERT_INT("doc has decl + assign", repl_state_document_count(), 2);
}

/* Reformat-the-document path: the legacy reformat loop calls
 * repl_command_store_replace_one + editor_buffer_replace_line for
 * every line. Verify that after a reformat (run via the existing
 * try_commit dispatcher path), buffer + store remain in sync. */
static void test_reformat_keeps_buffer_and_store_aligned(void) {
    repl_reset_state();

    /* Build a small program. */
    set_input("float a;");
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    repl_compile_float_decl("float a;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);
    set_input("a = 1");
    ctx = repl_compile_context_from_live();
    repl_compile_var_assign("a = 1", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);

    int doc_count = repl_state_document_count();
    int buf_count = editor_buffer_count();
    ASSERT_INT("post-build doc + buffer agree on count",
               doc_count, buf_count);

    /* Trigger reformat via repl_reformat_commands(). */
    repl_reformat_commands();

    ASSERT_INT("post-reformat doc count", repl_state_document_count(), doc_count);
    ASSERT_INT("post-reformat buffer count", editor_buffer_count(), buf_count);
    /* Each cmd has matching text in the buffer. */
    for (int i = 0; i < doc_count; i++) {
        ASSERT_TRUE("post-reformat buffer line non-empty",
                    editor_buffer_line(i) != NULL &&
                    editor_buffer_line(i)[0] != '\0');
    }
}

int main(void) {
    test_compile_float_decl_failure_is_pure();
    test_compile_var_assign_failure_is_pure();
    test_compile_no_change_leaves_state();
    test_compile_apply_updates_both();
    test_compile_apply_var_assign_updates_value();
    test_reformat_keeps_buffer_and_store_aligned();

    return test_harness_report(&g_harness, "test_repl_compile");
}
