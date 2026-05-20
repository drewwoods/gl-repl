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

#include "editor/state.h"
#include "ui/gl_2d.h"
#include "ui/text_panel.h"

#include "input.h"
#include "menu.h"

#include <gl_includes.h>

#include <stdio.h>
#include <string.h>

#define DEMO_WINDOW_W 800
#define DEMO_WINDOW_H 600
#define DEMO_MAX_ROWS  (MAX_COMMANDS + 1)

static int  g_demo_vp_w = DEMO_WINDOW_W;
static int  g_demo_vp_h = DEMO_WINDOW_H;
static int  g_demo_scroll = 0;

/* Build a UiTextPanelSnapshot from EditorState. One TEXT row per
 * buffer line plus one INPUT row at the active edit position.
 * Caller-owned rows[] storage stays valid for the duration of the
 * render/hit-test call.
 *
 * The demo does not (yet) own its own edit-line cursor; it reads
 * the editor's current edit-line through the EditorInputView, which
 * itself reads `repl_state_edit_line` via the shim. A follow-up
 * phase moves edit-line ownership into EditorState so the demo
 * doesn't need that shim stub. */
static int demo_build_snapshot(UiTextPanelRow *rows, int rows_cap,
                               UiTextPanelSnapshot *snap) {
    EditorBufferView buf = editor_buffer_view();
    EditorInputView  input = editor_state_input();

    int n = 0;
    int edit_line = input.edit_line_idx;
    if (edit_line < 0) edit_line = 0;

    for (int i = 0; i < buf.line_count && n < rows_cap; i++) {
        if (i == edit_line)
            continue;  /* INPUT row replaces the active line below */
        UiTextPanelRow row = {0};
        row.text              = editor_buffer_view_line(buf, i);
        if (!row.text) row.text = "";
        row.kind              = UI_TEXT_PANEL_ROW_TEXT;
        row.left_gutter_label = i + 1;
        row.source_line_idx   = i;
        row.hit_target_line_idx = -1;
        row.search_row_idx    = -1;
        row.color.r = 0.85f; row.color.g = 0.88f; row.color.b = 0.92f;
        row.color.a = 1.0f;  row.color.has_alpha = 0;
        row.hit_eligible      = 1;
        rows[n++] = row;
    }

    /* Active edit row (INPUT). Always present so the user has somewhere
     * to type even when the buffer is empty. */
    if (n < rows_cap) {
        UiTextPanelRow row = {0};
        row.text              = "";  /* INPUT rows ignore text */
        row.kind              = UI_TEXT_PANEL_ROW_INPUT;
        row.left_gutter_label = edit_line + 1;
        row.source_line_idx   = edit_line;
        row.hit_target_line_idx = -1;
        row.search_row_idx    = -1;
        row.color.r = 0.95f; row.color.g = 0.95f; row.color.b = 1.0f;
        row.color.a = 1.0f;  row.color.has_alpha = 0;
        row.hit_eligible      = 1;
        rows[n++] = row;
    }

    snap->vp_w           = g_demo_vp_w;
    snap->vp_h           = g_demo_vp_h;
    snap->cp_x           = 0;
    snap->cp_y           = 0;
    snap->cp_w           = g_demo_vp_w;
    snap->cp_h           = g_demo_vp_h - DEMO_MENU_BAR_H;
    snap->text_x         = 48;            /* leave room for line numbers */
    snap->wrap_at_comma  = 0;
    snap->top_chrome_h   = 0;
    snap->rows           = rows;
    snap->row_count      = n;
    snap->scroll         = g_demo_scroll;
    snap->chrome_flags   = UI_TEXT_PANEL_CHROME_LINE_NUMS;

    snap->input.input          = input.input ? input.input : "";
    snap->input.input_len      = input.input_len;
    snap->input.cursor         = input.cursor_pos;
    snap->input.anchor         = (input.anchor_pos >= 0) ? input.anchor_pos
                                                         : input.cursor_pos;
    snap->input.ghost          = "";
    snap->input.hint           = "";
    snap->input.cursor_visible = 1;

    snap->search.active    = 0;
    snap->search.query     = "";
    snap->search.query_len = 0;
    snap->search.hit_row   = -1;
    snap->search.hit_char  = -1;

    return n;
}

#ifndef GL_STUBS

static void demo_display_func(void) {
    glClearColor(0.10f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    UiTextPanelRow      rows[DEMO_MAX_ROWS];
    UiTextPanelSnapshot snap;
    UiTextPanelOutput   out = {0};
    demo_build_snapshot(rows, DEMO_MAX_ROWS, &snap);
    ui_text_panel_render(&snap, &out);

    /* The text panel pushes/pops its own 2D ortho via gl2d_begin/end.
     * Push a fresh one for menu chrome so the menu's draw state
     * doesn't depend on whatever the panel last set. */
    gl2d_begin(g_demo_vp_w, g_demo_vp_h);
    demo_menu_render(g_demo_vp_w, g_demo_vp_h);
    gl2d_end();

    glutSwapBuffers();
}

static void demo_mouse_func(int button, int state, int x, int y) {
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN)
        return;
    /* GLUT delivers top-left coords; convert to bottom-left. */
    int my = g_demo_vp_h - y;
    if (demo_menu_handle_click(x, my, g_demo_vp_w, g_demo_vp_h))
        glutPostRedisplay();
    /* No code-panel mouse handling in v1 — clicking outside the menu
     * just closes any open dropdown. */
}

static void demo_reshape_func(int w, int h) {
    g_demo_vp_w = w;
    g_demo_vp_h = h;
    glViewport(0, 0, w, h);
    glutPostRedisplay();
}

static void demo_keyboard_func(unsigned char key, int x, int y) {
    /* The demo's own generic dispatcher — does not route through
     * src/editor/input.c (the REPL editor's dispatcher). v1 covers
     * printable chars, backspace, and ESC; see
     * tools/editor_demo/input.c for the full key map. */
    demo_input_handle_key(key, x, y);
    glutPostRedisplay();
}

static void demo_special_func(int key, int x, int y) {
    demo_input_handle_special(key, x, y);
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
    glutMouseFunc(demo_mouse_func);

    printf("editor_demo: q / Esc to quit\n");
    glutMainLoop();
    return 0;
}

#else  /* GL_STUBS */

static int run_demo(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Stub build is link-check only, but we still exercise the
     * snapshot build path so it stays compiled and used (avoids
     * an unused-function warning and catches snapshot-shape
     * regressions in CI before the real-GL render touches them). */
    editor_state_reset();
    UiTextPanelRow      rows[DEMO_MAX_ROWS];
    UiTextPanelSnapshot snap;
    int n = demo_build_snapshot(rows, DEMO_MAX_ROWS, &snap);
    printf("editor_demo: stub build (link check only). "
           "Snapshot built with %d row(s). "
           "Rebuild without USE_GL_STUBS=1 to open a window.\n", n);
    return 0;
}

#endif /* GL_STUBS */

int main(int argc, char **argv) {
    return run_demo(argc, argv);
}
