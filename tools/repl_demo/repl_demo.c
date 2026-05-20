/*
 * tools/repl_demo/repl_demo.c -- Standalone REPL pipeline demo.
 *
 * Drives the REPL pipeline (parse -> command store -> flatten -> execute)
 * from a hard-coded static-text program. Proves the pipeline modules link
 * cleanly without the editor input dispatch (src/editor/input.c), the
 * controller (glr_ctrl.c and the glr_ctrl_router_* family), or the UI
 * (src/ui/, replay_ui_hud.c).
 *
 * The mirror of tools/scene_demo/scene_demo.c: that one proves src/scene/
 * has no hard dependency on REPL editor / controller / UI; this one
 * proves the REPL pipeline has no hard dependency on the editor input
 * dispatch / controller / UI.
 *
 * What's in the link set (see Makefile REPL_DEMO_DEP_SRCS):
 *   - REPL pipeline TUs: parser, command store, compile, apply, flatten,
 *     executor, eval, source-scope, command spec, autonormal, scenes,
 *     export, examples, replay annotations, replay, load.
 *   - State singleton: src/repl/state.c.
 *   - tools/repl_demo/source_document.c: standalone static line store
 *     implementing the source_document_* contract (Phase 6 of
 *     feature/source-document-port.md). The demo does NOT link
 *     src/editor/state.c, glr_source_document.c, or any other editor
 *     translation unit.
 *   - tools/repl_demo/stubs.c: deliberately empty. After every
 *     REPL-pipeline → editor/UI/controller edge was routed through a
 *     controller-installed sink/bridge or an opaque parameter, there
 *     are zero stubs to backfill. Pipeline diagnostics and the editor
 *     host effects flow through the one ReplHostEffects bridge in
 *     src/repl/core.h; the demo leaves the bridge unset, so every
 *     dispatch silently no-ops. The dependency ledger and removal plan live in
 *     feature/decouple-repl-from-gl-repl-alt.md and
 *     feature/source-document-port.md.
 *
 * Run:
 *   make repl_demo USE_GL_STUBS=1
 *   ./repl_demo                  # default: print summary for both samples
 *   ./repl_demo --execute        # also call repl_execute_program()
 */

#include "repl/command.h"
#include "repl/command_spec.h"    /* cmd_type_name */
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"   /* repl_parse_and_normalize */
#include "repl/eval.h"            /* repl_eval_declare_predef_var, g_predef_vars */
#include "repl/executor.h"
#include "repl/parser.h"
#include "repl/state_owners.h"
#include "source_document.h"      /* source_document_load_lines / _insert_line / _clear / _view */

#include <gl_includes.h>          /* GLUT bootstrap for --render mode */

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

/* --- Demo-local edit-line cursor ------------------------------------- */

/* Phase 4 of plans/in-review/edit-line-ownership.md moved edit-line
 * storage out of ReplState. The REPL pipeline reads/writes the
 * cursor through a controller-installed host-effects sink; the
 * demo backs the sink with a file-local int since it doesn't link
 * src/editor/state.c (where the canonical EditorState storage lives). */
static int g_demo_edit_line = 0;

static int demo_edit_line_get(void) { return g_demo_edit_line; }
static void demo_edit_line_set(int line) { g_demo_edit_line = line < 0 ? 0 : line; }

static const ReplHostEffects g_demo_host_effects = {
    .edit_line_get = demo_edit_line_get,
    .edit_line_set = demo_edit_line_set,
};

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
 * flow goes through `editor_try_commit_for_loop` in src/editor/commit.c, which
 * we don't want to link. Instead we hand-construct the CMD_FOR_BEGIN /
 * body / CMD_FOR_END triplet directly so the demo can prove that
 * repl_flatten_program unrolls loops. */
static void seed_for_loop_program(void) {
    /* Build the source program as canonical text and hand-construct
     * matching GLCmds. The flattener reads expressions from the editor
     * buffer, so the text and cmds must agree. */
    static const char *const lines[] = {
        "glPointSize(8)",
        "glColor3f(0.95, 0.85, 0.20)",
        "glBegin(GL_POINTS)",
        "for(i, 0, 4)",
        "  glVertex3f(i, 0, 0)",
        "}",
        "glEnd()",
    };
    enum { LINE_COUNT = sizeof(lines) / sizeof(lines[0]) };

    /* Source-text first -- repl_flatten reads the body's text via
     * source_document_view(). The demo links its own static-store
     * implementation in tools/repl_demo/source_document.c, so no
     * editor TU is pulled in. */
    source_document_load_lines(lines, LINE_COUNT);

    GLCmd cmds[LINE_COUNT];
    memset(cmds, 0, sizeof(cmds));

    cmds[0].type = CMD_POINT_SIZE;
    cmds[0].valid = 1;
    cmds[0].args[0] = 8.0f;
    cmds[0].num_args = 1;

    cmds[1].type = CMD_COLOR3F;
    cmds[1].valid = 1;
    cmds[1].args[0] = 0.95f;
    cmds[1].args[1] = 0.85f;
    cmds[1].args[2] = 0.20f;
    cmds[1].num_args = 3;

    cmds[2].type = CMD_BEGIN;
    cmds[2].valid = 1;
    cmds[2].args[0] = GL_POINTS;
    cmds[2].num_args = 1;

    /* CMD_FOR_BEGIN: var name 'i', start 0, end 4, step 1 (default). */
    cmds[3].type = CMD_FOR_BEGIN;
    cmds[3].valid = 1;
    cmds[3].args[0] = 0.0f;     /* start */
    cmds[3].args[1] = 4.0f;     /* end (exclusive in the canonical form) */
    cmds[3].args[2] = 1.0f;     /* step */
    cmds[3].num_args = 3;
    /* Body command: glVertex3f with `i` as an expression. has_vars=1 so
     * the flattener re-evaluates the expression with the loop's local i
     * for each iteration. */
    cmds[4].type = CMD_VERTEX3F;
    cmds[4].valid = 1;
    cmds[4].has_vars = 1;
    cmds[4].args[0] = 0.0f;     /* placeholder; flatten reparses from text */
    cmds[4].args[1] = 0.0f;
    cmds[4].args[2] = 0.0f;
    cmds[4].num_args = 3;
    cmds[5].type = CMD_FOR_END;
    cmds[5].valid = 1;

    cmds[6].type = CMD_END;
    cmds[6].valid = 1;

    ReplCommandStore store = repl_command_store_live();
    repl_command_store_load(&store, cmds, LINE_COUNT);
    /* Demo-local cursor policy: zero after a bulk load. */
    repl_dispatch_edit_line_set(0);
}

/* --- Program loaders -------------------------------------------------- */

static int load_text_lines(const char *const *lines) {
    /* Reset state before each program so the two samples don't smear. */
    repl_state_init_defaults();
    source_document_clear();

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
        /* Demo-local cursor: thread the edit-line through the
         * insert so the auto-advance behavior the demo's loader
         * depends on is preserved (each line appends after the
         * previous insertion). */
        int edit_line = repl_dispatch_edit_line_get();
        ReplStoreMutOpts opts = {
            .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
            .cursor_inout = &edit_line,
        };
        if (!repl_command_store_insert_one(&store, loaded, &pl.cmd, &opts)) {
            fprintf(stderr, "  command store full at line %d\n", loaded);
            return -1;
        }
        repl_dispatch_edit_line_set(edit_line);
        source_document_insert_line(loaded, pl.text);
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
    source_document_clear();

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
    static const char *const lines[] = {
        "glPointSize(8)",
        "glColor3f(0.95, 0.85, 0.20)",
        "glBegin(GL_POINTS)",
        "glVertex3f(r * sin(t), r * cos(t), 0)",
        "glEnd()",
    };
    enum { N_LINES = sizeof(lines) / sizeof(lines[0]) };
    int pos = 0;
    for (int i = 0; i < N_LINES; i++) {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        char text[MAX_LINE_LEN] = "";
        int preserve = (i == 3) ? 1 : 0;
        if (!repl_parse_and_normalize(lines[i], 0, NULL, 0,
                                      preserve,
                                      &cmd, text, (int)sizeof(text))) {
            fprintf(stderr, "  parse_and_normalize failed: %s\n", lines[i]);
            return;
        }
        ReplCommandStore store = repl_command_store_live();
        int edit_line = repl_dispatch_edit_line_get();
        ReplStoreMutOpts opts = {
            .flags        = REPL_COMMAND_STORE_ADJUST_EDIT_LINE,
            .cursor_inout = &edit_line,
        };
        repl_command_store_insert_one(&store, pos, &cmd, &opts);
        repl_dispatch_edit_line_set(edit_line);
        source_document_insert_line(pos, text);
        pos++;
    }
}

/* Real-GL builds (GL_STUBS not defined) emit live OpenGL calls from
 * repl_execute_program. The headless samples below never bootstrap a
 * GL context — that's the --render mode's job — so invoking the
 * executor here would segfault. The stubs build (USE_GL_STUBS=1)
 * defines GL_STUBS and turns every GL call into a no-op, so the
 * headless path is safe there. Print a one-shot warning and skip the
 * executor call in real-GL builds; the print summaries still run. */
static int headless_executor_safe(void) {
#ifdef GL_STUBS
    return 1;
#else
    static int warned = 0;
    if (!warned) {
        fprintf(stderr,
                "repl_demo: skipping repl_execute_program — no GL context.\n"
                "  Use --render to bootstrap GLUT, or rebuild with"
                " USE_GL_STUBS=1 for the headless executor path.\n");
        warned = 1;
    }
    return 0;
#endif
}

static void tick_and_execute(float t_value) {
    int t_idx = repl_eval_find_predef_var_idx("t");
    g_predef_vars[t_idx].value = t_value;
    repl_state_mark_flat_dirty();
    repl_flatten_commands(repl_dispatch_edit_line_get());
    if (!headless_executor_safe())
        return;
    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
    };
    repl_execute_program(&opts);
    const GLCmd *flat = opts.program.cmds;
    for (int i = 0; i < opts.flat_cmd_count; i++) {
        if (flat[i].type == CMD_VERTEX3F) {
            printf("  t=%4.2f -> glVertex3f(%.3f, %.3f, %.3f)\n", t_value,
                   flat[i].args[0], flat[i].args[1], flat[i].args[2]);
            break;
        }
    }
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
    repl_flatten_commands(repl_dispatch_edit_line_get());
    int n = repl_state_flat_program_count();
    FlatProgramView view = repl_state_flat_program_view();
    printf("  flat program: %d cmd(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("    flat[%d] %-22s src_idx=%d\n",
               i, cmd_type_name(view.cmds[i].type), view.cmds[i].src_cmd_idx);
}

static void execute_against_stubs(void) {
    if (!headless_executor_safe())
        return;
    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
    };
    repl_execute_program(&opts);
    printf("  executed %d flat cmd(s) against GL stubs\n", opts.flat_cmd_count);
}

/* --- Render mode (--render flag) -------------------------------------- *
 *
 * Drives the REPL pipeline against a real GL context. Same parse +
 * flatten + execute path as the headless mode; the only added surface
 * is the GLUT bootstrap (window, callbacks) and a fixed orbit camera.
 * The negative-isolation contract still holds: `nm repl_demo` shows
 * zero editor / glr_ctrl / ui_* / replay_ui_ symbols.
 *
 * Keys:  1 / 2 / 3  switch sample
 *        space      pause/resume animation (sample 3)
 *        Esc / q    quit
 *
 * Build:  make repl_demo                  (real GL, --render works)
 *         make repl_demo USE_GL_STUBS=1   (headless, --render is a no-op)
 */

#ifndef GL_STUBS

#define DEMO_WINDOW_W 800
#define DEMO_WINDOW_H 600

static int   g_render_sample = 1;     /* 1, 2, 3 */
static float g_render_t      = 0.0f;
static int   g_render_paused = 0;

/* Load whichever sample is currently selected into REPL state. */
static void render_load_current_sample(void) {
    repl_state_init_defaults();
    switch (g_render_sample) {
    case 1:
        load_text_lines(SAMPLE_TRIANGLE);
        break;
    case 2:
        seed_for_loop_program();
        break;
    case 3:
        seed_variable_driven_program();
        break;
    default:
        break;
    }
    repl_state_mark_flat_dirty();
}

static void render_apply_camera(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)DEMO_WINDOW_W / (double)DEMO_WINDOW_H, 0.1, 50.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    /* Fixed orbit camera: eye 4 units back and 3 up, looking at origin. */
    gluLookAt(0.0, 3.0, 4.0,   /* eye */
              0.0, 0.0, 0.0,   /* center */
              0.0, 1.0, 0.0);  /* up */
}

static void render_display_func(void) {
    glClearColor(0.10f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    render_apply_camera();

    /* For sample 3, push the live `t` into the predef table so the
     * has_vars re-evaluation sees the animated value. */
    if (g_render_sample == 3) {
        int t_idx = repl_eval_find_predef_var_idx("t");
        if (t_idx >= 0)
            g_predef_vars[t_idx].value = g_render_t;
        repl_state_mark_flat_dirty();
    }
    repl_flatten_commands(repl_dispatch_edit_line_get());

    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = source_document_view(),
    };
    repl_execute_program(&opts);

    glutSwapBuffers();
}

static void render_reshape_func(int w, int h) {
    glViewport(0, 0, w, h);
    /* Keep the perspective math simple: ignore the new size and use the
     * compile-time aspect. The minimal demo doesn't need responsive
     * layout. */
    glutPostRedisplay();
}

static void render_idle_func(void) {
    if (!g_render_paused && g_render_sample == 3)
        g_render_t += 0.016f;
    glutPostRedisplay();
}

static void render_keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 27: case 'q': case 'Q':
        exit(0);
    case '1': case '2': case '3':
        g_render_sample = key - '0';
        render_load_current_sample();
        printf("[render] sample = %d\n", g_render_sample);
        break;
    case ' ':
        g_render_paused = !g_render_paused;
        printf("[render] %s\n", g_render_paused ? "paused" : "resumed");
        break;
    default:
        return;
    }
    glutPostRedisplay();
}

static int run_render_mode(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(DEMO_WINDOW_W, DEMO_WINDOW_H);
    glutCreateWindow("repl_demo (real GL)");

    glEnable(GL_DEPTH_TEST);

    /* Install the host-effects sink (same reason as main(), but
     * render mode bypasses main()'s install above). */
    repl_install_host_effects(&g_demo_host_effects);

    render_load_current_sample();

    glutDisplayFunc(render_display_func);
    glutReshapeFunc(render_reshape_func);
    glutIdleFunc(render_idle_func);
    glutKeyboardFunc(render_keyboard_func);

    printf("[render] keys: 1/2/3 switch sample, space pause, q/Esc quit\n");
    glutMainLoop();
    return 0;
}

#else  /* GL_STUBS */

static int run_render_mode(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
        "repl_demo: --render needs a real GL build. Rebuild without\n"
        "  USE_GL_STUBS=1 (i.e. `make repl_demo`) and try again.\n");
    return 1;
}

#endif /* GL_STUBS */

/* --- Main ------------------------------------------------------------- */

static void print_help(const char *prog) {
    printf(
"Usage: %s [--execute | --render] [-h|--help]\n"
"\n"
"Standalone REPL pipeline demo. Drives parse -> command store -> flatten\n"
"-> execute on hard-coded static-text samples, without linking the editor\n"
"input dispatch, the controller, or the UI.\n"
"\n"
"Modes:\n"
"  (no flag)   Print parse/flatten summaries for all three samples and\n"
"              show the variable-driven re-evaluation table for sample 3.\n"
"  --execute   Same as above, plus run repl_execute_program() against the\n"
"              GL stubs after each sample's flatten.\n"
"  --render    Real GL: open a GLUT window and render the active sample.\n"
"              Requires a non-stubs build (see Build below).\n"
"  -h, --help  Print this help and exit.\n"
"\n"
"Render-mode keys:\n"
"  1 / 2 / 3   Switch sample (1 = triangle, 2 = for-loop, 3 = animated)\n"
"  space       Pause/resume sample-3 animation\n"
"  q, Esc      Quit\n"
"\n"
"Samples:\n"
"  1  Plain commands: parses a hand-typed glBegin/glColor/glVertex/glEnd\n"
"     triangle through repl_parser_parse_command_ctx + the command store.\n"
"  2  Hand-built for-loop: constructs a CMD_FOR_BEGIN/body/CMD_FOR_END\n"
"     triplet directly so flatten unrolls 4 vertices without going\n"
"     through src/editor/commit.c's editor_try_commit_for_loop.\n"
"  3  Variable-driven re-evaluation: declares `r` via\n"
"     repl_eval_declare_predef_var(), parses an expression that\n"
"     references `r` and `t` with preserve_expr=1, then bumps `t` and\n"
"     re-flattens to show has_vars expressions re-evaluating against\n"
"     the live g_predef_vars table.\n"
"\n"
"Build:\n"
"  make repl_demo                  Real GL; --render works.\n"
"  make repl_demo USE_GL_STUBS=1   Headless; --render prints a helpful\n"
"                                  error and exits.\n",
        prog);
}

int main(int argc, char **argv) {
    int run_execute = 0;
    int run_render  = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--execute") == 0)
            run_execute = 1;
        else if (strcmp(argv[i], "--render") == 0)
            run_render = 1;
        else if (strcmp(argv[i], "-h") == 0
              || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown flag '%s' (try -h)\n",
                    argv[0], argv[i]);
            return 2;
        }
    }

    if (run_render)
        return run_render_mode(argc, argv);

    /* Install the host-effects sink so the REPL pipeline's
     * repl_dispatch_edit_line_* calls land on the demo-local int.
     * Without this, the loader's cursor reads return 0 and writes
     * are dropped, which breaks the append-at-end semantics. */
    repl_install_host_effects(&g_demo_host_effects);

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
