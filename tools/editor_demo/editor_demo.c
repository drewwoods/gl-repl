/*
 * tools/editor_demo/editor_demo.c -- Standalone editor demo binary.
 *
 * Drives src/editor (input, commit, clipboard, undo, ...) against a
 * fake REPL semantics (via tools/editor_demo/repl_shim.c) to prove
 * the editor module set can link without the full REPL pipeline.
 * Mirror of repl_demo (REPL pipeline without editor) and scene_demo
 * (scene without REPL).
 *
 * What links:
 *   - src/editor (input, commit, clipboard, undo, state, search,
 *     completion, reformat, help_session, inline_rename,
 *     inline_file_prompt) -- minus services.c, which the shim
 *     replaces with demo-local EditorServices bindings.
 *   - src/ui -- text panel, code panel adapter, menu bar, etc.
 *     The plan's "chrome is editor-inherent" decision means these
 *     link directly rather than being abstracted.
 *   - src/widgets -- tutorial, color picker state, variable panel,
 *     replay state. Their default state is inactive; in the demo
 *     they no-op.
 *   - prof.c -- profiling instrumentation.
 *   - tools/editor_demo/repl_shim.c -- fake EditorServices bindings
 *     plus direct stubs for the repl_/glr_ symbols input.c and
 *     commit.c still call by name.
 *
 * What does NOT link:
 *   - src/repl -- fully stubbed by repl_shim.c.
 *   - src/app -- controller / app shell; the demo runs its own
 *     minimal app loop. repl_shim.c stubs the handful of glr_
 *     symbols input.c calls directly (camera reset, presentation,
 *     router).
 *   - src/scene -- no 3D rendering.
 *
 * The demo is a plain text editor -- not a GL grammar editor. The
 * shim's compile() returns NO_CHANGE, so user input becomes
 * comment-style text in the editor buffer rather than being parsed
 * as GL commands.
 *
 * Run:
 *   make editor_demo USE_GL_STUBS=1   # link check only (no GL needed)
 *   make editor_demo                  # real GL build; opens a window
 *   ./editor_demo
 */

#include "editor/input.h"
#include "editor/state.h"

#include <gl_includes.h>

#include <stdio.h>
#include <string.h>

#define DEMO_WINDOW_W 800
#define DEMO_WINDOW_H 600

#ifndef GL_STUBS

static void demo_display_func(void) {
    glClearColor(0.10f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* TODO: drive ui_text_panel render against EditorState once shim
     * is far enough along to populate a snapshot. For now the
     * skeleton just shows the link works. */
    glutSwapBuffers();
}

static void demo_reshape_func(int w, int h) {
    glViewport(0, 0, w, h);
    glutPostRedisplay();
}

static void demo_keyboard_func(unsigned char key, int x, int y) {
    if (key == 27 || key == 'q')
        exit(0);
    /* Route through editor input dispatch -- the actual proof of
     * decoupling. */
    editor_handle_key(key, x, y);
    glutPostRedisplay();
}

static void demo_special_func(int key, int x, int y) {
    editor_handle_special(key, x, y);
    glutPostRedisplay();
}

static int run_demo(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(DEMO_WINDOW_W, DEMO_WINDOW_H);
    glutCreateWindow("editor_demo");

    editor_state_reset();

    glutDisplayFunc(demo_display_func);
    glutReshapeFunc(demo_reshape_func);
    glutKeyboardFunc(demo_keyboard_func);
    glutSpecialFunc(demo_special_func);

    printf("editor_demo: q / Esc to quit\n");
    glutMainLoop();
    return 0;
}

#else  /* GL_STUBS */

static int run_demo(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Stub build is link-check only. Initialize editor state so we
     * exercise at least one editor entry point at runtime. */
    editor_state_reset();
    printf("editor_demo: stub build (link check only). "
           "Rebuild without USE_GL_STUBS=1 to open a window.\n");
    return 0;
}

#endif /* GL_STUBS */

int main(int argc, char **argv) {
    return run_demo(argc, argv);
}
