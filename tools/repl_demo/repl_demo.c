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

#ifndef OPENGL_VIBE_USE_GL_STUBS

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

    /* Sample 2 / 3 emit isolated vertices (no glBegin/glEnd in the
     * source). Wrap them so the points actually rasterize. Sample 1
     * has its own glBegin/glEnd. */
    if (g_render_sample != 1) {
        glPointSize(8.0f);
        glColor3f(0.95f, 0.85f, 0.20f);
        glBegin(GL_POINTS);
    }

    /* For sample 3, push the live `t` into the predef table so the
     * has_vars re-evaluation sees the animated value. */
    if (g_render_sample == 3) {
        int t_idx = repl_eval_find_predef_var_idx("t");
        if (t_idx >= 0)
            g_predef_vars[t_idx].value = g_render_t;
        repl_state_mark_flat_dirty();
    }
    repl_flatten_commands();

    ReplExecutionOptions opts = {
        .flat_cmd_count = repl_state_flat_program_count(),
        .program        = repl_state_flat_program_view(),
        .text           = editor_buffer_view(),
    };
    repl_execute_program(&opts);

    if (g_render_sample != 1)
        glEnd();

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

    render_load_current_sample();

    glutDisplayFunc(render_display_func);
    glutReshapeFunc(render_reshape_func);
    glutIdleFunc(render_idle_func);
    glutKeyboardFunc(render_keyboard_func);

    printf("[render] keys: 1/2/3 switch sample, space pause, q/Esc quit\n");
    glutMainLoop();
    return 0;
}

#else  /* OPENGL_VIBE_USE_GL_STUBS */

static int run_render_mode(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
        "repl_demo: --render needs a real GL build. Rebuild without\n"
        "  USE_GL_STUBS=1 (i.e. `make repl_demo`) and try again.\n");
    return 1;
}

#endif /* OPENGL_VIBE_USE_GL_STUBS */

/* --- Main ------------------------------------------------------------- */

int main(int argc, char **argv) {
    int run_execute = 0;
    int run_render  = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--execute") == 0)
            run_execute = 1;
        else if (strcmp(argv[i], "--render") == 0)
            run_render = 1;
    }

    if (run_render)
        return run_render_mode(argc, argv);

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
