#include "app/glr_ctrl.h"
/*
 * test_src/repl/compile.c - Phase C invariant tests.
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

#include "editor/commit.h"
#include "editor/reformat.h"
#include "editor/services.h"
#include "editor/state.h"
#include "repl/apply.h"
#include "repl/command_store.h"
#include "repl/compile.h"
#include "repl/core.h"
#include "repl/core_internal.h"  /* try_commit_*, editor_commit_func_decl_resume_peek */
#include "repl/eval.h"
#include "repl/load.h"           /* repl_load_apply_line for [P2] dup-check test */
#include "repl/state.h"
#include "ui/state.h"
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
    glr_app_reset_all();

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
    glr_app_reset_all();

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
    glr_app_reset_all();
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
    glr_app_reset_all();

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
    glr_app_reset_all();

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

/* Forced cmd-store capacity failure leaves predef-vars, editor
 * buffer, and command store all unchanged. The preflight inside
 * editor_commit_apply_compiled_change is the load-bearing
 * mechanism: without it the predef-op cascade would mutate while
 * the cmd-store insert silently fails. */
static void test_capacity_failure_is_atomic(void) {
    glr_app_reset_all();

    ReplCompiledChange change;
    ReplCompileContext ctx;
    char err[REPL_STATUS_TEXT_MAX];

    /* Establish a small pre-state: one declared variable, one
     * assignment line. */
    set_input("float anchor;");
    ctx = repl_compile_context_from_live();
    repl_compile_float_decl("float anchor;", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);
    set_input("anchor = 9");
    ctx = repl_compile_context_from_live();
    repl_compile_var_assign("anchor = 9", &ctx, &change, err, sizeof(err));
    editor_commit_apply_compiled_change(&change);

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
    change.cmds[0].var_decl_count = 1;
    strncpy(change.cmds[0].var_names[0], "ghost",
            sizeof(change.cmds[0].var_names[0]) - 1);
    change.cmds[0].var_names[0][sizeof(change.cmds[0].var_names[0]) - 1] = '\0';
    strncpy(change.text[0], "  float ghost;", sizeof(change.text[0]) - 1);
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

    int ok = editor_commit_apply_compiled_change(&change);
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
    ok = editor_commit_apply_compiled_change(&change);
    ASSERT_INT("apply returns 0 on insert_many capacity failure", ok, 0);
    ASSERT_INT("doc count still unchanged",
               repl_state_document_count(), pre_doc_count);
    ASSERT_INT("predef-var count still unchanged",
               g_num_predef_vars, pre_predef_count);
    ASSERT_INT("phantom not registered",
               repl_eval_find_predef_var_idx("phantom"), -1);
}

/* Reformat-the-document path: the legacy reformat loop calls
 * repl_command_store_replace_one + editor_buffer_replace_line for
 * every line. Verify that after a reformat (run via the existing
 * try_commit dispatcher path), buffer + store remain in sync. */
static void test_reformat_keeps_buffer_and_store_aligned(void) {
    glr_app_reset_all();

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

static void test_set_predef_value_prefers_last_literal_assign(void) {
    glr_app_reset_all();

    repl_feed_line_public("float x = 1, y;");
    repl_feed_line_public("x = 2;");
    repl_feed_line_public("x = y + 1;");
    repl_feed_line_public("x = 3;");

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 4.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef literal compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef literal replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_INT("set_predef literal replace pos", change.pos, 3);
    ASSERT_STR("set_predef literal text", change.text[0], "  x = 4.5;");
    ASSERT_INT("set_predef literal predef op count", change.predef_op_count, 1);
    ASSERT_INT("set_predef literal op kind", change.predef_ops[0].kind,
               REPL_PREDEF_OP_SET_VALUE);

    ASSERT_INT("set_predef literal apply OK",
               editor_commit_apply_compiled_change(&change), 1);
    ASSERT_STR("set_predef literal buffer line updated",
               editor_buffer_line(3), "  x = 4.5;");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef literal x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef literal live value",
                     g_predef_vars[x_idx].value, 4.5f, 1e-6f);
    }
}

static void test_set_predef_value_rewrites_declaration_initializer(void) {
    glr_app_reset_all();

    repl_feed_line_public("float a = 1, x = 2, y; // vars");

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 2.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef decl compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef decl replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_STR("set_predef decl text",
               change.text[0], "  float a = 1, x = 2.5, y; // vars");

    ASSERT_INT("set_predef decl apply OK",
               editor_commit_apply_compiled_change(&change), 1);
    ASSERT_STR("set_predef decl buffer line updated",
               editor_buffer_line(0), "  float a = 1, x = 2.5, y; // vars");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef decl x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef decl live value",
                     g_predef_vars[x_idx].value, 2.5f, 1e-6f);
    }
}

static void test_set_predef_value_adds_declaration_initializer(void) {
    glr_app_reset_all();

    repl_feed_line_public("float x;");

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 2.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef add init compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef add init replace kind", change.kind,
               REPL_COMPILED_REPLACE_ONE);
    ASSERT_STR("set_predef add init text", change.text[0], "  float x = 2.5;");

    ASSERT_INT("set_predef add init apply OK",
               editor_commit_apply_compiled_change(&change), 1);
    ASSERT_STR("set_predef add init buffer line updated",
               editor_buffer_line(0), "  float x = 2.5;");
}

static void test_set_predef_value_keeps_expression_sources(void) {
    glr_app_reset_all();

    repl_feed_line_public("float x;");
    repl_feed_line_public("float y;");
    repl_feed_line_public("x = y + 1;");

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "x", 8.0f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef expr compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef expr source untouched kind", change.kind,
               REPL_COMPILED_NO_CHANGE);

    ASSERT_INT("set_predef expr apply OK",
               editor_commit_apply_compiled_change(&change), 1);
    ASSERT_STR("set_predef expr formula preserved",
               editor_buffer_line(2), "  x = y + 1;");
    ASSERT_STR("set_predef expr decl preserved",
               editor_buffer_line(0), "  float x;");
    {
        int x_idx = repl_eval_find_predef_var_idx("x");
        ASSERT_TRUE("set_predef expr x exists", x_idx >= 0);
        ASSERT_FLOAT("set_predef expr live value",
                     g_predef_vars[x_idx].value, 8.0f, 1e-6f);
    }
}

static void test_set_predef_value_live_only_without_source(void) {
    glr_app_reset_all();

    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = repl_compile_set_predef_value(
        "t", 3.5f, &ctx, &change, err, sizeof(err));
    ASSERT_INT("set_predef live-only compile OK", r, REPL_COMPILE_OK);
    ASSERT_INT("set_predef live-only no source change", change.kind,
               REPL_COMPILED_NO_CHANGE);
    ASSERT_INT("set_predef live-only predef op count", change.predef_op_count, 1);

    ASSERT_INT("set_predef live-only apply OK",
               editor_commit_apply_compiled_change(&change), 1);
    ASSERT_INT("set_predef live-only buffer count unchanged",
               editor_buffer_count(), 0);
    {
        int t_idx = repl_eval_find_predef_var_idx("t");
        ASSERT_TRUE("set_predef live-only t exists", t_idx >= 0);
        ASSERT_FLOAT("set_predef live-only value",
                     g_predef_vars[t_idx].value, 3.5f, 1e-6f);
    }
}

/* editor_commit_current_input compile-failure path: returns
 * diagnostic via the result; no buffer/store/predef/status/undo
 * mutation. */
static void test_orchestration_compile_failure_returns_diagnostic(void) {
    glr_app_reset_all();

    /* Establish a non-trivial pre-state. */
    editor_input_set_text("float anchor;");
    EditorServices svc = editor_services_default();
    EditorCommitResult ok = editor_commit_current_input(&svc);
    ASSERT_INT("setup commit consumed", ok.consumed, 1);
    ASSERT_INT("setup commit mutated", ok.mutated, 1);
    ui_state_status_set("baseline status");

    ComputeFingerprint before = capture_fingerprint();

    /* Trigger compile failure: redeclare. */
    editor_input_set_text("float anchor;");
    EditorCommitResult r = editor_commit_current_input(&svc);
    ASSERT_INT("compile failure consumed", r.consumed, 1);
    ASSERT_INT("compile failure not mutated", r.mutated, 0);
    ASSERT_INT("compile failure not capacity_failed", r.capacity_failed, 0);
    ASSERT_INT("compile failure carries diagnostic", r.diagnostic_valid, 1);
    ASSERT_INT("compile failure no commit message",
               r.commit_message_valid, 0);
    ASSERT_TRUE("diagnostic non-empty", r.diagnostic[0] != '\0');

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("orchestration compile failure leaves state untouched",
                fingerprint_equal(&before, &after));
}

/* editor_commit_current_input NO_CHANGE path: input the dispatcher
 * doesn't recognize. consumed=0, no mutation. */
static void test_orchestration_no_change_falls_through(void) {
    glr_app_reset_all();
    editor_input_set_text("glVertex3f(0,0,0)");
    ui_state_status_set("baseline");
    ComputeFingerprint before = capture_fingerprint();

    EditorServices svc = editor_services_default();
    EditorCommitResult r = editor_commit_current_input(&svc);
    ASSERT_INT("non-handler input consumed=0", r.consumed, 0);
    ASSERT_INT("non-handler input mutated=0", r.mutated, 0);
    ASSERT_INT("non-handler input no diagnostic", r.diagnostic_valid, 0);

    ComputeFingerprint after = capture_fingerprint();
    ASSERT_TRUE("orchestration NO_CHANGE leaves state untouched",
                fingerprint_equal(&before, &after));
}

/* editor_commit_current_input success path: buffer + store + predef
 * mutate together, commit_message valid. */
static void test_orchestration_success_returns_message(void) {
    glr_app_reset_all();
    editor_input_set_text("float energy;");

    EditorServices svc = editor_services_default();
    EditorCommitResult r = editor_commit_current_input(&svc);
    ASSERT_INT("success consumed", r.consumed, 1);
    ASSERT_INT("success mutated", r.mutated, 1);
    ASSERT_INT("success not capacity_failed", r.capacity_failed, 0);
    ASSERT_INT("success not diagnostic", r.diagnostic_valid, 0);
    ASSERT_INT("success has commit_message", r.commit_message_valid, 1);
    ASSERT_TRUE("commit_message non-empty", r.commit_message[0] != '\0');

    ASSERT_INT("post-success cmd count", repl_state_document_count(), 1);
    ASSERT_INT("post-success buffer count", editor_buffer_count(), 1);
    ASSERT_TRUE("post-success predef registered",
                repl_eval_find_predef_var_idx("energy") >= 0);
}

/* func_def comment-relocation: leading comments above the cursor
 * get moved alongside the new func def block (delete + insert in
 * one transaction). */
static void test_func_def_comment_relocation(void) {
    glr_app_reset_all();

    /* Build doc:
     *   line 0: glVertex3f(1, 0, 0);    (geometry above future func)
     *   line 1: // descriptive comment   (top-level comment)
     *   line 2: // continuation          (top-level comment)
     *   cursor sits at line 3 (end of doc), about to define func0.
     */
    repl_feed_line_public("  glVertex3f(1, 0, 0);");
    repl_feed_line_public("// descriptive comment");
    repl_feed_line_public("// continuation");

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
    EditorServices svc = editor_services_default();
    EditorCommitResult ok = editor_commit_current_input(&svc);
    /* func_def isn't in repl_compile_dispatch yet (it's an
     * editor-side compile), so editor_commit_current_input returns
     * NO_CHANGE. Use the legacy try_commit dispatcher (which
     * forwards through editor_compile_func_def). */
    if (!ok.consumed) {
        /* Fall through to the legacy try_commit chain. */
        try_commit_block_structs();
    }

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
    /* Comment text retains the canonical indent from feed_line. */
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
    glr_app_reset_all();

    repl_feed_line_public("  glVertex3f(1, 0, 0);");
    repl_feed_line_public("");
    repl_feed_line_public("// descriptive comment");
    repl_feed_line_public("// continuation");

    ASSERT_INT("pre-funcdef blank relocation count",
               repl_state_document_count(), 4);
    ASSERT_INT("pre-funcdef blank row present",
               repl_state_document_cmds()[1].type, CMD_EMPTY);

    set_input("func0() {");
    try_commit_block_structs();

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

/* Resume publish: when a func_def's relocation moves the original
 * cursor position above the inserted block, the apply step
 * publishes the delta so the matching close-brace's compile
 * advances edit_line by that delta. */
static void test_func_def_resume_publish_consumed_by_close_brace(void) {
    glr_app_reset_all();

    /* Build a doc where leading comments precede the cursor:
     *   [0] // comment
     *   cursor at 1, define func.
     *
     * After func def insert: comment + fd + fe at position 0.
     * resume_pos = 1 - 1 = 0; insert_pos = 0; resume_delta = 0.
     * That's a degenerate case; let's add a vertex above so the
     * cursor sits past more state.
     */
    repl_feed_line_public("// header");
    repl_feed_line_public("  glVertex3f(1, 0, 0);");

    /* Move cursor between the vertex and a new comment. */
    repl_feed_line_public("// before func");
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
    glr_app_reset_all();
    repl_feed_line_public("// header");
    repl_feed_line_public("// pre-vertex");
    repl_feed_line_public("  glVertex3f(1, 0, 0);");
    repl_feed_line_public("// about to define func");

    set_input("func0() {");
    /* Drive the migration through try_commit_block_structs since
     * func_def isn't in repl_compile_dispatch yet. */
    try_commit_block_structs();

    /* After the migration, editor_commit_func_decl_resume_peek
     * should reflect the published delta. */
    int published = editor_commit_func_decl_resume_peek();
    ASSERT_INT("resume delta published by func_def", published, 1);

    /* Close the func block. The compile reads the delta (peek);
     * since end_type is CMD_FUNC_END, take + advance fires. */
    set_input("}");
    try_commit_close_brace();

    /* The global is consumed (cleared) on FUNC_END close. */
    int post_consume = editor_commit_func_decl_resume_peek();
    ASSERT_INT("resume delta cleared after func close-brace",
               post_consume, 0);
}

int main(void) {
    test_compile_float_decl_failure_is_pure();
    test_compile_var_assign_failure_is_pure();
    test_compile_no_change_leaves_state();
    test_compile_apply_updates_both();
    test_compile_apply_var_assign_updates_value();
    test_capacity_failure_is_atomic();
    test_reformat_keeps_buffer_and_store_aligned();
    test_set_predef_value_prefers_last_literal_assign();
    test_set_predef_value_rewrites_declaration_initializer();
    test_set_predef_value_adds_declaration_initializer();
    test_set_predef_value_keeps_expression_sources();
    test_set_predef_value_live_only_without_source();
    test_orchestration_compile_failure_returns_diagnostic();
    test_orchestration_no_change_falls_through();
    test_orchestration_success_returns_message();
    test_func_def_comment_relocation();
    test_func_def_blank_line_relocation();
    test_func_def_resume_publish_consumed_by_close_brace();

    /* [P1] regression: alias registration must roll back on parse
     * failure. Pre-fix, repl_compile_func_def called
     * repl_func_alias_set BEFORE parse_repl_func_signature, so a
     * malformed `name(args` (no `{`, no closing `)`) would leave
     * `name` registered as a func alias even though no CMD_FUNC_DEF
     * was created. Subsequent `name()` calls would erroneously
     * resolve. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        /* Call repl_compile_func_def directly with a header that
         * passes the quick-reject (both `(` and `{`) and whose alias
         * pre-step succeeds, but where parse_repl_func_signature
         * fails on the param list (`123` is not a valid identifier).
         * Pre-fix, the alias `badRoll` would leak to the global
         * alias table even though no CMD_FUNC_DEF was created. */
        ReplCompileContext ctx = repl_compile_context_from_live();
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
     * where feed_line() would have rejected the second. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        /* Establish slot 0: feed a first func0 def via the lean
         * loader so it lands as a real CMD_FUNC_DEF in the
         * document. */
        char err[128] = "";
        ASSERT_TRUE("[P2] first func0 def loads cleanly",
                    repl_load_apply_line("func0() {", err, sizeof(err)));
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
        ReplCompileContext ctx = repl_compile_context_from_live();
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

    /* [P2 editor] regression: editor_compile_func_def must roll back
     * alias registration on parse failure. The editor wrapper
     * (src/editor/commit.c ~line 645) registers a new alias before
     * parse_repl_func_signature runs, mirroring the
     * repl_compile_func_def pre-step. The corresponding parse-fail
     * branch (line ~659) returns NO_CHANGE without rolling back, so
     * a malformed `name(args` line in the user-facing path still
     * leaves the alias registered. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        ReplCompileContext ctx = repl_compile_context_from_live();
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
        glr_app_reset_all();
        repl_func_alias_clear_all();

        char err[128] = "";
        ASSERT_TRUE("[P1 editor] original aliased func loads",
                    repl_load_apply_line("drawCube() {", err, sizeof(err)));
        ASSERT_INT("[P1 editor] drawCube starts in slot 0",
                   repl_func_alias_lookup_slot("drawCube"), 0);

        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        ReplCompileContext ctx = repl_compile_context_from_live();
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

    /* [P2 editor] regression: editor_compile_func_def must roll back
     * alias registration on duplicate-funcN failure. The dup-check
     * branch (src/editor/commit.c ~line 678) returns ERROR without
     * rolling back, so a NEW alias whose pre-step assigns to slot N
     * — where slot N already has a CMD_FUNC_DEF — leaks the alias
     * registration. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        /* Insert a bare func0 (no alias registered) so slot 0 is
         * "free" by repl_func_alias_first_free_slot's definition
         * but already occupied at the document level. */
        char err[128] = "";
        ASSERT_TRUE("[P2 editor] bare func0 loads cleanly",
                    repl_load_apply_line("func0() {", err, sizeof(err)));

        /* Compile a NEW alias whose pre-step would target slot 0
         * (first free) but whose dup check fails because fn=0
         * already has a CMD_FUNC_DEF in the document. */
        ReplCompileContext ctx = repl_compile_context_from_live();
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

    /* [P1 loader] regression: repl_load_apply_line must preflight
     * via repl_apply_can_apply_compiled_change before mutating
     * predef vars / scratch ops / editor buffer. Pre-fix, a
     * capacity overflow at apply time would leave the predef var
     * already declared but no source CMD_VAR_DECLARE to back it. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        /* Force the cmd-store to capacity so any insert fails. */
        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        char err[128] = "";
        int ok = repl_load_apply_line("float fooLeak;", err, sizeof(err));

        /* Restore count BEFORE asserting so the asserts can clean up
         * gracefully and subsequent tests inherit a clean slate. */
        *store.count = saved_count;

        ASSERT_TRUE("[P1 loader] capacity-fail returns 0", !ok);
        ASSERT_TRUE("[P1 loader] capacity-fail does not register predef var",
                    repl_eval_find_predef_var_idx("fooLeak") < 0);
    }

    /* [P1 loader] regression: alias registered during compile must
     * be rolled back when the loader's apply step fails. Pre-fix,
     * compile_func_def's pre-step would register the alias, then
     * repl_apply_compiled_change would fail at capacity, but the
     * alias would remain in the global table with no matching
     * CMD_FUNC_DEF. */
    {
        glr_app_reset_all();
        repl_func_alias_clear_all();

        ReplCommandStore store = repl_command_store_live();
        int capacity = repl_command_store_capacity(&store);
        int saved_count = *store.count;
        *store.count = capacity;

        char err[128] = "";
        int ok = repl_load_apply_line("leakOnFail() {",
                                      err, sizeof(err));
        int slot = repl_func_alias_lookup_slot("leakOnFail");

        *store.count = saved_count;

        ASSERT_TRUE("[P1 loader] func capacity-fail returns 0", !ok);
        ASSERT_TRUE("[P1 loader] func capacity-fail does not leak alias",
                    slot < 0);
    }

    return test_harness_report(&g_harness, "test_repl_compile");
}
