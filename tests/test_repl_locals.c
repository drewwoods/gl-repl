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
#include "repl/export.h"
#include "repl/util.h"
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
#define ASSERT_STR(label, got, exp) TEST_ASSERT_STR(&g_harness, label, got, exp)

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

/* args[arg] of the `n`-th emitted command of `type`. */
static float nth_cmd_arg_n(CmdType type, int n, int arg) {
    FlatProgramView v = repl_flat_program_view_live();
    int seen = 0;
    for (int i = 0; i < v.cmd_count; i++) {
        if (v.cmds[i].type != type)
            continue;
        if (seen++ == n)
            return v.cmds[i].args[arg];
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

/* ---- Export / import (phase 4) --------------------------------------- */

/* Read a whole file into `buf`. Returns 1 on success. */
static int slurp(const char *path, char *buf, size_t buf_sz) {
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return 0;
    n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

/* Index of the first function-scoped declaration row, or -1. */
static int first_local_decl_row(void) {
    for (int i = 0; i < repl_state_document_count(); i++) {
        const GLCmd *c = &repl_state_document_cmds()[i];
        if (c->type == CMD_VAR_DECLARE && c->var_idx == REPL_VAR_IDX_LOCAL)
            return i;
    }
    return -1;
}

/* Its canonical source line, or "". */
static const char *first_local_decl_line(void) {
    int row = first_local_decl_row();
    const char *line = row >= 0
                     ? source_text_line(source_document_view(), row) : NULL;
    return line ? line : "";
}

static int count_decl_rows(void) {
    int n = 0;
    for (int i = 0; i < repl_state_document_count(); i++)
        if (repl_state_document_cmds()[i].type == CMD_VAR_DECLARE)
            n++;
    return n;
}

static const char *const k_local_scene[] = {
    "static float amp = 2;",
    "glBegin(GL_POINTS);",
    "func0(k) {",
    "float ang; // swing phase",
    "float rad;",
    "ang = k;",
    "rad = amp;",
    "glVertex3f(rad, ang, 0);",
    "}",
    "func0(3);",
    "glEnd();",
};

/* A local exports as a real C automatic at its body position, carrying an
 * explicit zero initializer, and reimports as canonical REPL text. The
 * initializer is the behavior-parity fix: the REPL binds every local to
 * 0.0f on call entry, so a bare `float a;` in the generated file would be
 * undefined where the REPL reads 0. */
static void test_local_export_import_round_trip(void) {
    printf("--- local export/import round trip ---\n");
    const char *path1 = "/tmp/repl_locals_roundtrip_1.c";
    const char *path2 = "/tmp/repl_locals_roundtrip_2.c";
    static char text[1 << 16];
    char first_pass[MAX_LINE_LEN];

    load_scene(k_local_scene, (int)(sizeof(k_local_scene) / sizeof(k_local_scene[0])));
    ASSERT_INT("two locals before export", count_decl_rows(), 3);

    ASSERT_INT("export succeeds",
               repl_export_save_output(path1, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path1, text, sizeof(text)));
    ASSERT_TRUE("the local is a real C declaration with a zero initializer",
                strstr(text, "float ang = 0.0f") != NULL);
    ASSERT_TRUE("so is the second one",
                strstr(text, "float rad = 0.0f") != NULL);
    ASSERT_TRUE("its trailing comment survives as a C89 block comment",
                strstr(text, "/* swing phase */") != NULL);
    ASSERT_TRUE("the global still uses the @declare marker",
                strstr(text, "@declare amp") != NULL);
    ASSERT_TRUE("and the global is still a file-scope static",
                strstr(text, "static float amp") != NULL);

    glr_ctrl_reset_all();
    ASSERT_INT("reimport succeeds", repl_export_load_from_file(path1, NULL), 1);
    ASSERT_INT("the local count is unchanged", count_decl_rows(), 3);
    ASSERT_TRUE("a function-scoped declaration came back",
                first_local_decl_row() >= 0);
    repl_copy_string_fits(first_pass, sizeof(first_pass), first_local_decl_line());
    ASSERT_TRUE("the reimported local dropped the generated initializer",
                strstr(first_pass, "= 0") == NULL);
    ASSERT_TRUE("and is plain `float`, not `static float`",
                strstr(first_pass, "float ang") != NULL &&
                strstr(first_pass, "static") == NULL);
    ASSERT_TRUE("its trailing comment survived the round trip",
                strstr(first_pass, "swing phase") != NULL);

    /* Second round-trip is a fixed point: the body must not grow by a row
     * per local, which is what a synthesized `a = 0.0f;` statement would
     * have cost. */
    int rows_after_first = repl_state_document_count();
    ASSERT_INT("re-export succeeds",
               repl_export_save_output(path2, source_document_view(), NULL), 1);
    glr_ctrl_reset_all();
    ASSERT_INT("re-import succeeds", repl_export_load_from_file(path2, NULL), 1);
    ASSERT_INT("the document did not grow", repl_state_document_count(),
               rows_after_first);
    ASSERT_STR("and the declaration text is a fixed point",
               first_local_decl_line(), first_pass);
}

/* Behavior parity, not just syntactic round-trip: a function that reads a
 * local before writing it must compute the same thing after the
 * round-trip as it did before. */
static void test_export_parity_read_before_write(void) {
    printf("--- export parity: read before write ---\n");
    const char *path = "/tmp/repl_locals_parity.c";
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float u;",
        "glVertex3f(u, 0, 0);",
        "u = r;",
        "glVertex3f(u, 0, 0);",
        "}",
        "func0(7);",
        "glEnd();",
    };
    load_scene(lines, 9);

    ASSERT_FLOAT("before export: the unwritten read is 0",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 0.0f);
    ASSERT_FLOAT("before export: the written read is 7",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 7.0f);

    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    glr_ctrl_reset_all();
    ASSERT_INT("reimport succeeds", repl_export_load_from_file(path, NULL), 1);
    repl_flatten_commands(0);

    ASSERT_FLOAT("after round trip: the unwritten read is still 0",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 0.0f);
    ASSERT_FLOAT("after round trip: the written read is still 7",
                 nth_cmd_arg0(CMD_VERTEX3F, 1), 7.0f);
}

/* Import lowers only the exporter's own literal-zero form. A hand-written
 * non-zero initializer inside a function body must still hit the Phase 1
 * preflight rejection, so REPL and file semantics stay identical rather
 * than merely compatible. */
static void test_import_lowers_only_the_generated_zero(void) {
    printf("--- import lowers only the generated zero ---\n");
    const char *path = "/tmp/repl_locals_handwritten.c";
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float u;",
        "u = r;",
        "glVertex3f(u, 0, 0);",
        "}",
        "func0(3);",
        "glEnd();",
    };
    static char text[1 << 16];
    FILE *f;

    load_scene(lines, 8);
    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path, text, sizeof(text)));

    /* Hand-edit the generated file: a non-zero initializer on the local. */
    {
        char *at = strstr(text, "float u = 0.0f");
        ASSERT_TRUE("found the generated declaration", at != NULL);
        if (at)
            memcpy(at, "float u = 5.0f", strlen("float u = 5.0f"));
    }
    f = fopen(path, "wb");
    ASSERT_TRUE("rewrote the file", f != NULL);
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    glr_ctrl_reset_all();
    repl_export_load_from_file(path, NULL);
    ASSERT_INT("the hand-written initializer produced no local declaration",
               count_decl_rows(), 0);
}

/* ---- Review regressions --------------------------------------------- */

/* Commit `text` with the cursor on source row `edit_line`. `insert` picks
 * insert mode (place before the row) or overwrite mode (replace it). */
static int commit_line_at(const char *text, int edit_line, int insert) {
    editor_state_edit_line_set(edit_line);
    editor_insert_mode_set(insert);
    return editor_feed_line(text);
}

/* Commit hoists declarations to the body top, but nothing keeps them
 * there. Two ordinary edits push a statement ahead of a local, and a
 * binding that depended on the declarations forming a leading run would
 * silently stop resolving — the reads would fall through to a global of
 * the same name, or to nothing, with no diagnostic. */
static void test_locals_bind_regardless_of_row_order(void) {
    printf("--- locals bind regardless of row order ---\n");
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float a;",
        "float b;",
        "a = r;",
        "b = r * 2;",
        "glVertex3f(a, b, 0);",
        "}",
        "func0(3);",
        "glEnd();",
    };

    /* Function definitions hoist to the document top, so the body is
     * rows 1..: 0 head, 1 float a, 2 float b, 3 a = r, ... */
    load_scene(lines, 10);
    ASSERT_INT("row 1 is the first local", first_local_decl_row(), 1);
    ASSERT_FLOAT("baseline binds a", nth_cmd_arg0(CMD_VERTEX3F, 0), 3.0f);

    /* Insert a statement between the two declarations. */
    ASSERT_INT("inserting a statement mid-prologue commits",
               commit_line_at("glColor3f(1, 0, 0);", 2, 1), 1);
    ASSERT_INT("the statement landed between the declarations",
               repl_state_document_cmds()[2].type, CMD_COLOR3F);
    ASSERT_INT("and the second declaration follows it",
               repl_state_document_cmds()[3].type, CMD_VAR_DECLARE);
    repl_flatten_commands(0);
    ASSERT_INT("both vertices survive", count_cmds(CMD_VERTEX3F), 1);
    ASSERT_FLOAT("the local before the statement still binds",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 3.0f);
    ASSERT_TRUE("the local after the statement still binds",
                nth_cmd_arg_n(CMD_VERTEX3F, 0, 1) == 6.0f);

    /* Replace an unused leading declaration with a statement — allowed,
     * since nothing reads it, and it leaves the survivor out of the
     * prologue. */
    const char *lines2[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float unused;",
        "float b;",
        "b = r * 2;",
        "glVertex3f(b, 0, 0);",
        "}",
        "func0(3);",
        "glEnd();",
    };
    load_scene(lines2, 9);
    ASSERT_INT("row 1 is the unused declaration",
               repl_state_document_cmds()[1].type, CMD_VAR_DECLARE);
    ASSERT_INT("replacing the unused leading declaration commits",
               commit_line_at("glColor3f(1, 0, 0);", 1, 0), 1);
    ASSERT_INT("row 1 is now a statement",
               repl_state_document_cmds()[1].type, CMD_COLOR3F);
    ASSERT_INT("row 2 is still the surviving declaration",
               repl_state_document_cmds()[2].type, CMD_VAR_DECLARE);
    repl_flatten_commands(0);
    ASSERT_FLOAT("the surviving local still binds",
                 nth_cmd_arg0(CMD_VERTEX3F, 0), 6.0f);
}

/* A for-header's bounds are evaluated in the enclosing scope, before the
 * iterator exists — `for(x, 0, x + 1)` reads the *outer* x. A delete
 * guard that treats the iterator as bound across the whole header line
 * calls that outer local unreferenced and lets it be deleted; the bound
 * then resolves to the shadowed global instead and the loop silently
 * runs a different number of times. */
static void test_delete_guard_sees_locals_in_loop_bounds(void) {
    printf("--- delete guard sees locals in loop bounds ---\n");
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    const char *lines[] = {
        "static float x = 5;",
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float x;",
        "for(x, 0, x + 1) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "func0(1);",
        "glEnd();",
    };
    load_scene(lines, 10);

    ASSERT_INT("row 2 is the local declaration",
               repl_state_document_cmds()[2].type, CMD_VAR_DECLARE);
    ASSERT_INT("it is the local, not the global",
               repl_state_document_cmds()[2].var_idx, REPL_VAR_IDX_LOCAL);
    ASSERT_INT("row 3 is the loop header",
               repl_state_document_cmds()[3].type, CMD_FOR_BEGIN);
    /* The bound reads the local (0), not the global (5): one iteration.
     * That is the whole point — the local is live, and its only reader is
     * the loop bound. */
    ASSERT_INT("the bound reads the local: one iteration",
               count_cmds(CMD_VERTEX3F), 1);

    ctx = repl_compile_context_from_live(2);
    ASSERT_INT("deleting a local read only by a loop bound is rejected",
               repl_compile_delete_range(2, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("delete rejection says why",
                strstr(err, "still referenced") != NULL);

    /* The complementary case must stay deletable: a local whose only
     * textual occurrence is the iterator's own declaring token, with a
     * bound that reads nothing. */
    const char *lines2[] = {
        "func0(r) {",
        "float x;",
        "for(x, 0, 3) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "func0(1);",
    };
    load_scene(lines2, 7);
    ASSERT_INT("row 1 is the shadowed local",
               repl_state_document_cmds()[1].type, CMD_VAR_DECLARE);
    ctx = repl_compile_context_from_live(1);
    ASSERT_INT("a local shadowed by an iterator that does not read it "
               "still deletes",
               repl_compile_delete_range(1, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_OK);
}

/* The same binder-line rule applies to globals. A loop iterator shadows a
 * same-named global only in the loop body; its start/end/step expressions
 * still read the enclosing scope. The global delete walk must therefore
 * inspect the bounds before deciding that the iterator hides the name. */
static void test_delete_guard_sees_globals_in_loop_bounds(void) {
    printf("--- delete guard sees globals in loop bounds ---\n");
    ReplCompileContext ctx;
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    const char *lines[] = {
        "static float x = 5;",
        "func0(r) {",
        "for(x, 0, x + 1) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "func0(1);",
    };
    load_scene(lines, 7);

    ASSERT_INT("row 0 is the global declaration",
               repl_state_document_cmds()[0].type, CMD_VAR_DECLARE);
    ASSERT_TRUE("it owns a predef slot",
                repl_state_document_cmds()[0].var_idx >= 0);
    ASSERT_INT("the bound reads the global: six iterations",
               count_cmds(CMD_VERTEX3F), 6);

    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("deleting a global read only by a loop bound is rejected",
               repl_compile_delete_range(0, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_ERROR);
    ASSERT_TRUE("global delete rejection says why",
                strstr(err, "still referenced") != NULL);

    /* The iterator token and its body uses are not global reads. */
    const char *lines2[] = {
        "static float x = 5;",
        "func0(r) {",
        "for(x, 0, 3) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "func0(1);",
    };
    load_scene(lines2, 7);
    ctx = repl_compile_context_from_live(0);
    ASSERT_INT("a global only shadowed by the iterator still deletes",
               repl_compile_delete_range(0, 1, &ctx, &change,
                                         err, sizeof(err)),
               REPL_COMPILE_OK);
}

/* Import lowers only the exporter's own form. `float a = 0.0f, b;` is
 * never generated: in C, `b` is indeterminate, so silently lowering it
 * would turn undefined behavior into a deterministic REPL zero. */
static void test_import_rejects_partially_initialized_decl(void) {
    printf("--- import rejects a partially initialized declaration ---\n");
    const char *path = "/tmp/repl_locals_partial_init.c";
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float a;",
        "float b;",
        "a = r;",
        "glVertex3f(a, b, 0);",
        "}",
        "func0(3);",
        "glEnd();",
    };
    static char text[1 << 16];
    char *at;
    FILE *f;

    load_scene(lines, 9);
    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path, text, sizeof(text)));

    /* Hand-edit the first declaration into a form the exporter never
     * writes: one declarator initialized, one not. The replacement is
     * length-preserving so the surrounding file is untouched. */
    at = strstr(text, "float a = 0.0f;");
    ASSERT_TRUE("found the first declaration", at != NULL);
    if (at)
        memcpy(at, "float a=0.0f,q;", strlen("float a=0.0f,q;"));
    f = fopen(path, "wb");
    ASSERT_TRUE("rewrote the file", f != NULL);
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    glr_ctrl_reset_all();
    repl_export_load_from_file(path, NULL);
    /* `float b = 0.0f;` still lowers; the mixed line must not, so it
     * reaches the declaration preflight and is rejected there. */
    ASSERT_INT("a partially initialized declaration is not lowered",
               count_decl_rows(), 1);
}

/* A comma promises another declarator. The exporter's generated form never
 * ends its list with one, and invalid C such as `float a = 0.0f,;` must pass
 * through untouched so the ordinary declaration preflight rejects it. */
static void test_import_rejects_trailing_comma_decl(void) {
    printf("--- import rejects a trailing-comma declaration ---\n");
    const char *path = "/tmp/repl_locals_trailing_comma.c";
    const char *lines[] = {
        "func0(r) {",
        "float a;",
        "a = r;",
        "glVertex3f(a, 0, 0);",
        "}",
        "func0(3);",
    };
    static char text[1 << 16];
    char *at;
    FILE *f;

    load_scene(lines, 6);
    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path, text, sizeof(text)));

    /* Length-preserving replacement: leave the rest of the generated file
     * at exactly the same offsets while creating the malformed list. */
    at = strstr(text, "float a = 0.0f;");
    ASSERT_TRUE("found the generated declaration", at != NULL);
    if (at)
        memcpy(at, "float a=0.0f, ;", strlen("float a=0.0f, ;"));
    f = fopen(path, "wb");
    ASSERT_TRUE("rewrote the file", f != NULL);
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    glr_ctrl_reset_all();
    repl_export_load_from_file(path, NULL);
    ASSERT_INT("a trailing-comma declaration is not lowered",
               count_decl_rows(), 0);
}

/* A declaration line can already sit near MAX_LINE_LEN. Export widens it
 * by strlen(" = 0.0f") per name, so building into a same-sized buffer
 * silently truncates — and what falls off the end is the trailing
 * comment. The fixture asserts its own premise: the committed row fits,
 * and the widened row would not have. */
static void test_export_does_not_truncate_long_decl_comment(void) {
    printf("--- export does not truncate a long declaration comment ---\n");
    const char *path = "/tmp/repl_locals_long_decl.c";
    static char text[1 << 16];
    char decl[MAX_LINE_LEN];
    const char *marker = "TAILMARKER";
    const int widen_per_name = (int)strlen(" = 0.0f");
    /* Long enough that widening overflows MAX_LINE_LEN, short enough that
     * format_decl_text's own MAX_LINE_LEN buffer does not truncate the
     * committed row. Both halves are asserted below. */
    const int target_len = MAX_LINE_LEN - 24;
    int committed_len;
    int off;

    off = snprintf(decl, sizeof(decl), "float");
    for (int i = 0; i < MAX_NAMES_PER_DECL; i++)
        off += snprintf(decl + off, sizeof(decl) - (size_t)off,
                        "%s nnnnnnnnnnnn%d", i ? "," : "", i);
    off += snprintf(decl + off, sizeof(decl) - (size_t)off, "; // ");
    while (off < target_len - (int)strlen(marker))
        decl[off++] = 'x';
    snprintf(decl + off, sizeof(decl) - (size_t)off, "%s", marker);

    glr_ctrl_reset_all();
    editor_feed_line("func0(r) {");
    editor_feed_line(decl);
    editor_feed_line("glVertex3f(r, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(1);");
    ASSERT_INT("the long declaration committed", count_decl_rows(), 1);

    committed_len = (int)strlen(first_local_decl_line());
    ASSERT_TRUE("premise: the committed row was not itself truncated",
                committed_len < MAX_LINE_LEN - 1 &&
                strstr(first_local_decl_line(), marker) != NULL);
    ASSERT_TRUE("premise: widening it would overflow a MAX_LINE_LEN buffer",
                committed_len + MAX_NAMES_PER_DECL * widen_per_name >
                    MAX_LINE_LEN - 1);

    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path, text, sizeof(text)));
    ASSERT_TRUE("every name kept its zero initializer",
                strstr(text, "nnnnnnnnnnnn7 = 0.0f") != NULL);
    ASSERT_TRUE("the trailing comment was not truncated away",
                strstr(text, marker) != NULL);
}

/* The exporter targets C89 — it hoists for-loop variables into a scope
 * brace for exactly this reason. A local declaration can legitimately sit
 * after a statement in the body (commit hoists, but ordinary insert-mode
 * editing can push a statement ahead of one), so the exported function
 * body has to hoist them too or the generated file is C99-only. */
static void test_export_hoists_locals_for_c89(void) {
    printf("--- export hoists locals to the top of the function body ---\n");
    const char *path = "/tmp/repl_locals_c89.c";
    static char text[1 << 16];
    const char *body;
    const char *decl_b;
    const char *stmt;
    const char *lines[] = {
        "glBegin(GL_POINTS);",
        "func0(r) {",
        "float a;",
        "float b;",
        "a = r;",
        "b = r * 2;",
        "glVertex3f(a, b, 0);",
        "}",
        "func0(3);",
        "glEnd();",
    };
    load_scene(lines, 10);

    /* Push a statement between the two declarations. */
    ASSERT_INT("inserting a statement mid-prologue commits",
               commit_line_at("glColor3f(1, 0, 0);", 2, 1), 1);
    ASSERT_INT("the source row order really is decl, statement, decl",
               repl_state_document_cmds()[2].type, CMD_COLOR3F);
    ASSERT_INT("with a declaration after it",
               repl_state_document_cmds()[3].type, CMD_VAR_DECLARE);

    ASSERT_INT("export succeeds",
               repl_export_save_output(path, source_document_view(), NULL), 1);
    ASSERT_TRUE("exported file is readable", slurp(path, text, sizeof(text)));

    body = strstr(text, "static void func0(");
    ASSERT_TRUE("found the exported function", body != NULL);
    if (!body)
        return;
    decl_b = strstr(body, "float b = 0.0f;");
    stmt = strstr(body, "glColor3f");
    ASSERT_TRUE("both the declaration and the statement were emitted",
                decl_b != NULL && stmt != NULL);
    ASSERT_TRUE("the declaration precedes the statement in the emitted body",
                decl_b != NULL && stmt != NULL && decl_b < stmt);
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
    test_local_export_import_round_trip();
    test_export_parity_read_before_write();
    test_import_lowers_only_the_generated_zero();
    test_locals_bind_regardless_of_row_order();
    test_delete_guard_sees_locals_in_loop_bounds();
    test_delete_guard_sees_globals_in_loop_bounds();
    test_import_rejects_partially_initialized_decl();
    test_import_rejects_trailing_comma_decl();
    test_export_does_not_truncate_long_decl_comment();
    test_export_hoists_locals_for_c89();

    return test_harness_report(&g_harness, "test_repl_locals");
}
