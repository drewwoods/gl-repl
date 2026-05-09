/*
 * tools/repl_demo/repl_demo.c -- Standalone REPL pipeline demo.
 *
 * Drives the REPL pipeline (parse -> command store -> flatten -> execute)
 * from a hard-coded static-text program. Proves the pipeline modules link
 * cleanly without the editor input dispatch (src/editor/input.c), the
 * controller (glr_ctrl.c and the glr_ctrl_router_* family), or the UI
 * (src/ui/, replay_ui_hud.c).
 *
 * The mirror of tools/teapot_demo/teapot.c: that one proves src/scene/
 * has no hard dependency on REPL editor / controller / UI; this one
 * proves the REPL pipeline has no hard dependency on the editor input
 * dispatch / controller / UI.
 *
 * What's in the link set (see Makefile REPL_DEMO_DEP_SRCS):
 *   - REPL pipeline TUs: parser, command store, compile, apply, flatten,
 *     executor, eval, source-scope, command spec, autonormal, scenes,
 *     export, examples, replay annotations, help text, autocomplete.
 *   - State singleton: repl_state.c, plus glr_camera.c (referenced by
 *     repl_state.c) and src/editor/state.c (the editor *buffer* that holds
 *     per-line canonical text -- by design, not editor input dispatch).
 *   - GL stub-ready scene/peer surface for whatever repl_state.c reaches.
 *   - tools/repl_demo/stubs.c: no-op replacements for the editor / UI /
 *     controller entry points that repl_state_reset_all() and the executor
 *     reach for.
 *
 * Run:
 *   make repl_demo USE_GL_STUBS=1
 *   ./repl_demo                  # default: print summary for both samples
 *   ./repl_demo --execute        # also call repl_execute_program()
 */

#include "repl_command.h"
#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_core_internal.h"   /* cmd_type_name */
#include "repl_eval.h"            /* repl_eval_declare_predef_var, g_predef_vars */
#include "repl_executor.h"
#include "repl_parser.h"
#include "repl_state_owners.h"
#include "editor/state.h"         /* editor_buffer_view, editor_buffer_set_line */

#include <stdio.h>
#include <string.h>

/* Why no `float x;` / `x = expr;` / typed-as-text `for(...) {`?
 *
 * Those flows go through the `try_commit_*` chain in src/editor/commit.c,
 * which is editor-owned by design (see src/editor/commit.h's preamble).
 * The split:
 *
 *   - `repl_compile_*` returns `ReplCompiledChange` -- a pure source-
 *     command-array mutation. It has no notion of cursor, insert mode,
 *     or input buffer. Callable from outside the editor.
 *   - `editor_compile_*` (close-brace, if-block, func-def, for-loop)
 *     returns `EditorCommitPlan` = ReplCompiledChange + cursor target +
 *     insert-mode toggle + clear-input + clear-pending-newline +
 *     load-line-after-apply. Those side-effects are editor concerns;
 *     putting them on ReplCompiledChange would let REPL pipeline tests
 *     learn cursor mechanics and re-mix the compile/apply split.
 *   - `try_commit_*` is the dispatch chain that picks the right handler
 *     and drives the canonical transaction:
 *         preflight -> undo capture -> REPL apply -> editor-buffer apply
 *         -> editor post-effects -> status.
 *     This transaction is editor business because it mutates editor
 *     state AND REPL state atomically with one undo snapshot.
 *
 * The demo wants to prove the REPL pipeline (parse + store + flatten +
 * execute) doesn't need any of that. So we use `repl_parser_parse_command_ctx`
 * directly for plain commands and hand-construct GLCmds for the loop
 * sample, bypassing the editor commit chain entirely.
 */

/* --- Static sample programs ------------------------------------------ */

/* Plain commands: no for-loops, no function defs, no var decls. Exercises
 * parse -> store -> flatten -> execute. */
static const char *const SAMPLE_TRIANGLE[] = {
    "// triangle in the XY plane",
    "glBegin(GL_TRIANGLES)",
    "glColor3f(1, 0, 0)",
    "glVertex3f(0, 0, 0)",
    "glColor3f(0, 1, 0)",
    "glVertex3f(1, 0, 0)",
    "glColor3f(0, 0, 1)",
    "glVertex3f(0, 1, 0)",
    "glEnd()",
    NULL,
};

/* For-loop unrolls 4 vertices. The text-as-typed `for(i, 0, 4) { ... }`
 * flow goes through `try_commit_for_loop` in src/editor/commit.c, which
 * we don't want to link. Instead we hand-construct the CMD_FOR_BEGIN /
 * body / CMD_FOR_END triplet directly so the demo can prove that
 * repl_flatten_program unrolls loops. */
static void seed_for_loop_program(void) {
    /* Build the source program as canonical text and hand-construct
     * matching GLCmds. The flattener reads expressions from the editor
     * buffer, so the text and cmds must agree. */
    static const char *const lines[] = {
        "for(i, 0, 4)",
        "  glVertex3f(i, 0, 0)",
        "}",
    };
    enum { LINE_COUNT = sizeof(lines) / sizeof(lines[0]) };

    /* Editor buffer first -- repl_flatten reads the body's text from it. */
    for (int i = 0; i < LINE_COUNT; i++)
        editor_buffer_set_line(i, lines[i]);

    /* CMD_FOR_BEGIN: var name 'i', start 0, end 4, step 1 (default). */
    GLCmd cmds[3];
    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_FOR_BEGIN;
    cmds[0].valid = 1;
    cmds[0].args[0] = 0.0f;     /* start */
    cmds[0].args[1] = 4.0f;     /* end (exclusive in the canonical form) */
    cmds[0].args[2] = 1.0f;     /* step */
    cmds[0].num_args = 3;
    /* Body command: glVertex3f with `i` as an expression. has_vars=1 so
     * the flattener re-evaluates the expression with the loop's local i
     * for each iteration. */
    cmds[1].type = CMD_VERTEX3F;
    cmds[1].valid = 1;
    cmds[1].has_vars = 1;
    cmds[1].args[0] = 0.0f;     /* placeholder; flatten reparses from text */
    cmds[1].args[1] = 0.0f;
    cmds[1].args[2] = 0.0f;
    cmds[1].num_args = 3;
    cmds[2].type = CMD_FOR_END;
    cmds[2].valid = 1;

    ReplCommandStore store = repl_command_store_live();
    /* Bulk-load the three commands so count/edit_line is set in one shot. */
    repl_command_store_load(&store, cmds, 3, 0);
}

/* --- Program loaders -------------------------------------------------- */

static int load_text_lines(const char *const *lines) {
    /* Reset state before each program so the two samples don't smear. */
    repl_state_init_defaults();

    int loaded = 0;
    for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        char err[REPL_STATUS_TEXT_MAX] = "";
        ReplParsedLine pl;
        memset(&pl, 0, sizeof(pl));
        ReplParseContext ctx = {
            .source_line_idx = loaded,
            .vars            = NULL,
            .num_vars        = 0,
            .strict_refs     = 0,
            .err_buf         = err,
            .err_sz          = (int)sizeof(err),
        };
        if (!repl_parser_parse_command_ctx(line, &pl, &ctx)) {
            fprintf(stderr,
                    "  parse error on line %d: %s\n  source: %s\n",
                    loaded, err[0] ? err : "(no diagnostic)", line);
            return -1;
        }
        ReplCommandStore store = repl_command_store_live();
        if (!repl_command_store_insert_one(&store, loaded, &pl.cmd,
                                           REPL_COMMAND_STORE_ADJUST_EDIT_LINE)) {
            fprintf(stderr, "  command store full at line %d\n", loaded);
            return -1;
        }
        editor_buffer_set_line(loaded, pl.text);
        loaded++;
    }
    return loaded;
}

/* Sample 3: variable-driven re-evaluation.
 *
 * Variables are REPL-owned, not editor-owned. The text syntax `float x;`
 * (and `x = expr;`, `A[0] = ...`) is editor-level sugar that calls
 * repl_eval_declare_predef_var() / sets g_predef_vars[idx].value. The
 * model itself is REPL: g_predef_vars[MAX_PREDEF_VARS] holds (name,
 * value) pairs, the parser validates expressions against the table, the
 * executor re-evaluates expressions with `has_vars` each call.
 *
 * The demo registers `r` directly via the eval API, parses an expression
 * that references `r` and `t` (`t` already exists -- repl_eval_init_predef_vars
 * creates it), then "ticks the frame" by bumping `t` and re-executing.
 * No editor commit path involved. */
static void seed_variable_driven_program(void) {
    /* `t` is created by repl_state_init_defaults() -> ... ->
     * repl_eval_init_predef_vars(). Add `r`. */
    char err[64] = "";
    if (repl_eval_declare_predef_var("r", err, sizeof(err)) < 0) {
        fprintf(stderr, "  declare_predef_var(r) failed: %s\n", err);
        return;
    }
    int r_idx = repl_eval_find_predef_var_idx("r");
    g_predef_vars[r_idx].value = 1.5f;

    /* Parse with preserve_expr=1 so the canonical text keeps the
     * original `r * sin(t)` form rather than the evaluated literal.
     * Flatten re-reads this text every tick and re-evaluates the
     * expressions against current g_predef_vars values. The plain
     * `repl_parser_parse_command_ctx` path bakes the literal values
     * in -- correct for static commands, wrong for var-driven ones. */
    static const char *line =
        "glVertex3f(r * sin(t), r * cos(t), 0)";
    GLCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    char text[MAX_LINE_LEN] = "";
    if (!repl_parse_and_normalize(line, 0, NULL, 0,
                                  /*preserve_expr=*/1,
                                  &cmd, text, (int)sizeof(text))) {
        fprintf(stderr, "  parse_and_normalize failed\n");
        return;
    }

    ReplCommandStore store = repl_command_store_live();
    repl_command_store_insert_one(&store, 0, &cmd,
                                  REPL_COMMAND_STORE_ADJUST_EDIT_LINE);
    editor_buffer_set_line(0, text);
}

static void tick_and_execute(float t_value) {
    int t_idx = repl_eval_find_predef_var_idx("t");
    g_predef_vars[t_idx].value = t_value;
    repl_state_mark_flat_dirty();
    repl_flatten_commands();
    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = editor_buffer_view(),
    };
    repl_execute_program(&opts);
    const GLCmd *flat = opts.program.cmds;
    if (opts.flat_cmd_count > 0)
        printf("  t=%4.2f -> glVertex3f(%.3f, %.3f, %.3f)\n", t_value,
               flat[0].args[0], flat[0].args[1], flat[0].args[2]);
}

/* --- Reporters -------------------------------------------------------- */

static void print_source_summary(const char *label, int loaded) {
    printf("%s: loaded %d source command(s)\n", label, loaded);
    int doc_count = repl_state_document_count();
    const GLCmd *cmds = repl_state_document_cmds_mut();
    for (int i = 0; i < doc_count; i++)
        printf("  src[%d] %-22s\n", i, cmd_type_name(cmds[i].type));
}

static void print_flat_summary(void) {
    repl_flatten_commands();
    int n = repl_state_flat_program_count();
    FlatProgramView view = repl_state_flat_program_view();
    printf("  flat program: %d cmd(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("    flat[%d] %-22s src_idx=%d\n",
               i, cmd_type_name(view.cmds[i].type), view.cmds[i].src_cmd_idx);
}

static void execute_against_stubs(void) {
    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = editor_buffer_view(),
    };
    repl_execute_program(&opts);
    printf("  executed %d flat cmd(s) against GL stubs\n", opts.flat_cmd_count);
}

/* --- Main ------------------------------------------------------------- */

int main(int argc, char **argv) {
    int run_execute = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--execute") == 0)
            run_execute = 1;
    }

    printf("=== sample 1: plain commands ===\n");
    int loaded = load_text_lines(SAMPLE_TRIANGLE);
    if (loaded < 0)
        return 1;
    print_source_summary("triangle", loaded);
    print_flat_summary();
    if (run_execute)
        execute_against_stubs();

    printf("\n=== sample 2: hand-built for-loop ===\n");
    repl_state_init_defaults();
    seed_for_loop_program();
    print_source_summary("for-loop", repl_state_document_count());
    print_flat_summary();
    if (run_execute)
        execute_against_stubs();

    /* Sample 3 always runs against the executor: the point is to show
     * has_vars expressions re-evaluating with REPL-owned variables, and
     * the only way to observe that is to execute and read back args[]. */
    printf("\n=== sample 3: variable-driven re-evaluation ===\n");
    repl_state_init_defaults();
    seed_variable_driven_program();
    print_source_summary("var-driven", repl_state_document_count());
    /* Set r once via the eval API; bump t and re-execute four times to
     * watch the args change. No editor commit path involved -- variables
     * are REPL state. */
    tick_and_execute(0.00f);
    tick_and_execute(0.50f);
    tick_and_execute(1.00f);
    tick_and_execute(1.57f);  /* ~pi/2 */

    return 0;
}
