/*
 * tests/test_repl_locals.c -- function-scoped local variables, runtime
 * semantics (scoped-local-variables, phase 2).
 *
 * Phase 1's tests live in tests/test_repl_compile.c and cover what the
 * *compiler* accepts and rejects. This suite covers what the flattened
 * program actually computes: that a local exists per invocation, that name
 * resolution is innermost-first and lexical (never dynamic), that a value
 * accumulated inside a for-loop survives the loop, and that a global
 * feeding a local routes as a structural dependency rather than a
 * value-only rebake.
 *
 * Every case loads a scene through the live commit pipeline, runs a full
 * flatten, and reads the emitted flat stream — so an assertion failure
 * means the program a user would see rendered is wrong, not merely that an
 * internal field drifted.
 */
#include <stdio.h>
#include <string.h>

#include "app/glr_ctrl.h"
#include "editor/commit.h"
#include "editor/input.h"
#include "editor/state.h"
#include "repl/command.h"
#include "repl/compile.h"
#include "repl/eval.h"
#include "repl/executor.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "source_document.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-4f)

/* Feed `lines` into a fresh scene through the commit pipeline, then run a
 * full flatten so the flat stream describes exactly this program. */
static void load_scene(const char *const *lines, int count) {
    glr_ctrl_reset_all();
    for (int i = 0; i < count; i++)
        editor_feed_line(lines[i]);
    repl_flatten_commands(0);
}

/* args[0] of the `n`-th (0-based) emitted command of `type`, or NAN-ish
 * sentinel when there aren't that many. Vertices are the probe of choice:
 * they are the one command whose argument a reader can trace straight back
 * to the scene text. */
static float nth_cmd_arg0(CmdType type, int n) {
    FlatProgramView v = repl_flat_program_view_live();
    int seen = 0;
    for (int i = 0; i < v.cmd_count; i++) {
        if (v.cmds[i].type != type)
            continue;
        if (seen++ == n)
            return v.cmds[i].args[0];
    }
    return -99999.0f;
}

static int count_cmds(CmdType type) {
    FlatProgramView v = repl_flat_program_view_live();
    int n = 0;
    for (int i = 0; i < v.cmd_count; i++)
        if (v.cmds[i].type == type)
            n++;
    return n;
}

static int nth_cmd_var_idx(CmdType type, int n) {
    FlatProgramView v = repl_flat_program_view_live();
    int seen = 0;
    for (int i = 0; i < v.cmd_count; i++) {
        if (v.cmds[i].type != type)
            continue;
        if (seen++ == n)
            return v.cmds[i].var_idx;
    }
    return -99999;
}

static float predef_value(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    return idx >= 0 ? g_predef_vars[idx].value : -99999.0f;
}

static ReplExprDepMask predef_bit(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    return idx >= 0 ? (ReplExprDepMask)1u << idx : 0;
}

/* Commit `text` with the cursor on source row `edit_line`, overwrite mode,
 * and apply it. Returns 1 on success. Used by the cases that need a second
 * edit *after* a scene has already been flattened once. */
static int commit_decl_at(const char *text, int edit_line) {
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    editor_state_edit_line_set(edit_line);
    editor_insert_mode_set(0);
    editor_input_set_text(text);
    ctx = repl_compile_context_from_live(editor_state_edit_line());
    if (repl_compile_float_decl(text, &ctx, &change, err, sizeof(err))
            != REPL_COMPILE_OK)
        return 0;
    return editor_commit_apply_external_change(&change, 0, 0);
}

/* ---- Cases ---------------------------------------------------------- */

/* A local exists once per invocation and carries a real computed value
 * into the geometry, while taking no predef slot. */
static void test_local_is_per_invocation(void) {
    printf("--- local is per invocation ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float u;",
        "u = r * 2;",
        "glVertex3f(u, 0, 0);",
        "}",
        "func0(3);",
        "func0(5);",
        "glEnd();",
    };
    load_scene(lines, 9);

    ASSERT_INT("both calls emitted a vertex", count_cmds(CMD_VERTEX3F), 2);
    ASSERT_FLOAT("first call binds u = 6", nth_cmd_arg0(CMD_VERTEX3F, 0), 6.0f);
    ASSERT_FLOAT("second call binds u = 10", nth_cmd_arg0(CMD_VERTEX3F, 1), 10.0f);
    ASSERT_INT("the local took no predef slot",
               repl_eval_find_predef_var_idx("u"), -1);
    ASSERT_INT("the emitted assignment carries the local sentinel",
               nth_cmd_var_idx(CMD_VAR_ASSIGN, 0), REPL_VAR_IDX_LOCAL);
}

/* A local reads 0.0f on entry — the conversion-safety criterion depends on
 * this, so it needs a regression behind it. */
static void test_local_reads_zero_before_write(void) {
    printf("--- local reads zero before write ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float u;",
        "glVertex3f(u, 0, 0);",
        "u = r;",
        "}",
        "func0(7);",
        "glEnd();",
    };
    load_scene(lines, 8);

    ASSERT_FLOAT("an unwritten local reads 0 on entry",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 0.0f);
}

/* Shadowing a global is legal and resolves innermost-first; the global
 * itself is untouched by the local's writes. */
static void test_local_shadows_global(void) {
    printf("--- local shadows global ---\n");
    const char *lines[] = {
        "static float x;",
        "glBegin(GL_POINTS);",
        "x = 7;",
        "func0(r) {",
        "float x;",
        "x = r;",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(2);",
        "glVertex3f(x, 0, 0);",
        "glEnd();",
    };
    load_scene(lines, 11);

    ASSERT_FLOAT("inside the function, the local wins",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 2.0f);
    ASSERT_FLOAT("outside it, the global is unchanged",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 7.0f);
    ASSERT_FLOAT("the global's live value is unchanged", predef_value("x"), 7.0f);
}

/* A loop iterator is a nested scope, so it shadows a function local inside
 * the loop and the local reappears after it — C's rule, and the reason the
 * name collision is accepted rather than rejected. */
static void test_loop_iterator_shadows_local(void) {
    printf("--- loop iterator shadows local ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float i;",
        "i = 9;",
        "for(i, 0, 2) {",
        "glVertex3f(i, 0, 0);",
        "}",
        "glVertex3f(i, 0, 0);",
        "}",
        "func0(0);",
        "glEnd();",
    };
    load_scene(lines, 11);

    ASSERT_INT("two loop vertices plus the trailing one",
               count_cmds(CMD_VERTEX3F), 3);
    ASSERT_FLOAT("iteration 0 sees the iterator",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 0.0f);
    ASSERT_FLOAT("iteration 1 sees the iterator",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 1.0f);
    ASSERT_FLOAT("after the loop the local is visible again, unclobbered",
                 nth_cmd_arg0(CMD_VERTEX3F, 2), 9.0f);
}

/* Scope is lexical, not dynamic. A caller's local must not follow the call
 * into a callee that has no such name — the callee reads the global, which
 * is what the exported C does. And a callee's *own* local beats the
 * caller's same-named one without disturbing it. */
static void test_call_frames_are_lexical(void) {
    printf("--- call frames are lexical ---\n");
    const char *lines[] = {
        "static float x;",
        "glBegin(GL_POINTS);",
        "x = 10;",
        "func1(q) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(p) {",
        "float x;",
        "x = 2;",
        "func1(p);",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(1);",
        "glEnd();",
    };
    load_scene(lines, 14);

    ASSERT_INT("both vertices emitted", count_cmds(CMD_VERTEX3F), 2);
    ASSERT_FLOAT("the callee reads the global, not the caller's local",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 10.0f);
    ASSERT_FLOAT("the caller's local survives the call",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 2.0f);

    printf("--- callee local beats caller local ---\n");
    const char *lines2[] = {
        "static float x;",
        "glBegin(GL_POINTS);",
        "x = 10;",
        "func1(q) {",
        "float x;",
        "x = 3;",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(p) {",
        "float x;",
        "x = 2;",
        "func1(p);",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(1);",
        "glEnd();",
    };
    load_scene(lines2, 16);

    ASSERT_FLOAT("the callee's own local wins inside the callee",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 3.0f);
    ASSERT_FLOAT("and does not leak back into the caller's frame",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 2.0f);
    ASSERT_FLOAT("the global is untouched by either", predef_value("x"), 10.0f);
}

/* Recursion is the case a global scratch variable cannot serve: each frame
 * needs its own copy, or the recursive call clobbers the parent's value
 * before the parent is done with it. */
static void test_recursion_gets_a_fresh_frame(void) {
    printf("--- recursion gets a fresh frame ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(n) {",
        "float u;",
        "u = n;",
        "if(n > 0) {",
        "func0(n - 1);",
        "}",
        "glVertex3f(u, 0, 0);",
        "}",
        "func0(2);",
        "glEnd();",
    };
    load_scene(lines, 11);

    ASSERT_INT("three frames, three vertices", count_cmds(CMD_VERTEX3F), 3);
    /* Emission order is innermost-first: each frame's vertex follows its
     * recursive call. A shared local would make all three read 0. */
    ASSERT_FLOAT("innermost frame kept u = 0", nth_cmd_arg0(CMD_VERTEX3F, 0), 0.0f);
    ASSERT_FLOAT("middle frame kept u = 1", nth_cmd_arg0(CMD_VERTEX3F, 1), 1.0f);
    ASSERT_FLOAT("outer frame kept u = 2", nth_cmd_arg0(CMD_VERTEX3F, 2), 2.0f);
}

/* flatten_for_loop rebuilds its scope array per iteration, so a local
 * written inside the loop only survives because the outer entries are
 * copied back out. Without that, acc resets every pass and ends at 0. */
static void test_accumulate_across_for_inside_func(void) {
    printf("--- accumulate across a for inside a func ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(n) {",
        "float acc;",
        "acc = 0;",
        "for(i, 0, n) {",
        "acc = acc + i;",
        "}",
        "glVertex3f(acc, 0, 0);",
        "}",
        "func0(4);",
        "glEnd();",
    };
    load_scene(lines, 11);

    ASSERT_FLOAT("acc accumulated 0+1+2+3 across the loop",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 6.0f);
}

/* The declaration prologue tolerates comments and blank rows. A strictly
 * contiguous run would silently unbind every local after the first one an
 * author commented out. */
static void test_prologue_tolerates_a_commented_decl(void) {
    printf("--- prologue tolerates a commented decl ---\n");
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float unused;",
        "float b;",
        "b = r;",
        "glVertex3f(b, 0, 0);",
        "}",
        "func0(4);",
        "glEnd();",
    };
    load_scene(lines, 9);
    ASSERT_FLOAT("baseline binds b", nth_cmd_arg0(CMD_VERTEX3F, 0), 4.0f);

    /* Row 1 is `float unused;` — the first of the two decls. */
    ASSERT_INT("row 1 is the unused decl",
               repl_state_document_cmds()[1].type, CMD_VAR_DECLARE);
    ctx = repl_compile_context_from_live(1);
    ASSERT_INT("commenting out the unused decl compiles",
               repl_compile_toggle_comment(1, "// ", &ctx, &change,
                                           err, sizeof(err)),
               REPL_COMPILE_OK);
    ASSERT_INT("comment-toggle applies",
               editor_commit_apply_external_change(&change, 0, 0), 1);
    ASSERT_INT("row 1 is now a comment",
               repl_state_document_cmds()[1].type, CMD_COMMENT);

    repl_flatten_commands(0);
    ASSERT_FLOAT("the local after the commented row still binds",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 4.0f);
}

/* Inserting a legal local over an existing global retargets older
 * assignment rows lexically, on the next flatten, without rewriting their
 * persisted metadata. */
static void test_new_local_retargets_older_global_assignment(void) {
    printf("--- a new local retargets an older global assignment ---\n");
    float vals[MAX_PREDEF_VARS];
    int x_slot;
    int assign_row = -1;
    const char *lines[] = {
        "static float x;",
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "x = 5;",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0(1);",
        "glEnd();",
    };
    load_scene(lines, 8);

    x_slot = repl_eval_find_predef_var_idx("x");
    ASSERT_TRUE("x is a global", x_slot >= 0);
    ASSERT_FLOAT("the global assignment ran", predef_value("x"), 5.0f);

    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds()[i].type == CMD_VAR_ASSIGN) {
            assign_row = i;
            break;
        }
    }
    ASSERT_TRUE("found the assignment row", assign_row >= 0);
    ASSERT_INT("it was compiled against the global slot",
               repl_state_document_cmds()[assign_row].var_idx, x_slot);

    /* Insert `float x;` into the body, over the assignment's row. */
    ASSERT_INT("inserting a shadowing local commits",
               commit_decl_at("float x;", assign_row), 1);

    /* Park a sentinel in the global so a stray write to it is visible. */
    repl_copy_predef_values(vals, MAX_PREDEF_VARS);
    vals[x_slot] = 9.0f;
    repl_restore_predef_values(vals, MAX_PREDEF_VARS);

    repl_flatten_commands(0);
    ASSERT_FLOAT("the vertex now reads the local", nth_cmd_arg0(CMD_VERTEX3F, 0), 5.0f);
    ASSERT_FLOAT("the global was not written", predef_value("x"), 9.0f);
    ASSERT_INT("the emitted assignment carries the local sentinel",
               nth_cmd_var_idx(CMD_VAR_ASSIGN, 0), REPL_VAR_IDX_LOCAL);
    ASSERT_INT("the persisted source row still holds its old global slot",
               repl_state_document_cmds()[assign_row + 1].var_idx, x_slot);
}

/* A global feeding a local must be reported structural. rebake_one_cmd
 * evaluates against a frozen per-command snapshot and writes back only
 * through predef slots, so it cannot thread a local's new value into later
 * commands — a value-only rebake would read stale locals. */
static void test_global_feeding_a_local_is_structural(void) {
    printf("--- a global feeding a local routes structurally ---\n");
    float vals[MAX_PREDEF_VARS];
    int radius_slot;
    /* Declared-with-initializer, never reassigned in source: the scrubbable
     * knob shape. A `radius = 2;` statement instead would re-bake over the
     * slider on every reflatten and correctly drop radius from the masks
     * entirely, which is a different rule and not what this tests. */
    const char *lines[] = {
        "static float radius = 2;",
        "glBegin(GL_POINTS);",
        "func0() {",
        "float x;",
        "x = radius;",
        "glVertex3f(x, 0, 0);",
        "}",
        "func0();",
        "glEnd();",
    };
    load_scene(lines, 9);
    repl_state_flat_program_clear_dirty();

    ReplExprDepMask radius_bit = predef_bit("radius");
    radius_slot = repl_eval_find_predef_var_idx("radius");
    ASSERT_TRUE("radius predef exists", radius_bit != 0);
    ASSERT_FLOAT("baseline vertex tracks radius",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 2.0f);
    ASSERT_TRUE("a global feeding a local lands in the structural mask",
                (repl_state_flat_program_structural_dep_mask() & radius_bit)
                    == radius_bit);
    /* It is in the value mask as well — the vertex reads the local, whose
     * mask is the assignment's RHS. That is fine and not a licence to
     * rebake: routing checks the structural mask first, and the assertions
     * below are the ones that matter. */
    ASSERT_TRUE("it is in the value mask too, via the local it feeds",
                (repl_state_flat_program_value_dep_mask() & radius_bit)
                    == radius_bit);

    /* Move the "slider": a live predef value change, not a source edit. */
    repl_copy_predef_values(vals, MAX_PREDEF_VARS);
    vals[radius_slot] = 6.0f;
    repl_restore_predef_values(vals, MAX_PREDEF_VARS);
    repl_state_notify_predef_value_changed(radius_slot);

    ASSERT_INT("the change demands a full reflatten",
               repl_state_flat_program_dirty(), 1);
    ASSERT_INT("and not a value-only rebake",
               (int)repl_state_flat_program_args_dirty_mask(), 0);

    repl_flatten_commands(0);
    ASSERT_FLOAT("the emitted vertex tracks the moved slider",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 6.0f);
}

int main(void) {
    test_local_is_per_invocation();
    test_local_reads_zero_before_write();
    test_local_shadows_global();
    test_loop_iterator_shadows_local();
    test_call_frames_are_lexical();
    test_recursion_gets_a_fresh_frame();
    test_accumulate_across_for_inside_func();
    test_prologue_tolerates_a_commented_decl();
    test_new_local_retargets_older_global_assignment();
    test_global_feeding_a_local_is_structural();

    return test_harness_report(&g_harness, "test_repl_locals");
}
