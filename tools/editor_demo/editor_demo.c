/*
 * tools/editor_demo/editor_demo.c -- Standalone generic text editor demo.
 *
 * Per the Phase 8 cleavage in plans/done/editor-demo.md, this demo
 * does NOT reuse the REPL editor's controller (src/editor/input.c,
 * commit.c, clipboard.c, undo.c, reformat.c, search.c, completion.c,
 * and the inline overlays — all REPL-flavored). It runs entirely on:
 *
 *   - src/editor/state.c     : text buffer + cursor + selection data model.
 *   - src/editor/edit_ops.c  : generic text-editing primitives shared with
 *                              src/editor/input.c (REPL dispatcher).
 *   - src/ui/text_panel.c    : generic wrapped text renderer.
 *   - src/ui/text_layout.c   : wrap math.
 *   - src/ui/text_search.c   : case-insensitive text find (linked for the
 *                              text_panel's search-row machinery — find is
 *                              not bound to a key in v1).
 *   - prof.c                 : profiling.
 *   - tools/editor_demo/input.c : the demo's own generic key dispatcher.
 *   - tools/editor_demo/menu.c  : the demo's own File menu.
 *   - tools/editor_demo/repl_shim.c : one-symbol ledger
 *                                     (repl_state_edit_line; the acknowledged
 *                                     state.c leak named in the plan's
 *                                     "Editor files that aren't yet generic"
 *                                     inventory).
 *
 * What does NOT link: anything under src/repl, src/app, src/scene, or
 * src/widgets, plus the REPL-flavored editor controller files listed
 * above. There is no fake EditorServices instance and no per-symbol
 * REPL / glr / ui / tutorial stub block in the shim.
 *
 * v1 behavior: type characters into the input row, backspace to delete,
 * arrow keys / Home / End to move within the row, click the File menu
 * for Load / Save (unimplemented handlers) / Quit. Cross-line nav, undo,
 * find, word jumps, selection clipboard, and File menu handlers are
 * deferred to follow-up phases — see plans/done/editor-demo.md "What's
 * still open".
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
static void demo_fill_text_row(UiTextPanelRow *row, const char *text,
                               int line_idx) {
    memset(row, 0, sizeof(*row));
    row->text                = text ? text : "";
    row->kind                = UI_TEXT_PANEL_ROW_TEXT;
    row->left_gutter_label   = line_idx + 1;
    row->source_line_idx     = line_idx;
    row->hit_target_line_idx = -1;
    row->search_row_idx      = -1;
    row->color.r = 0.85f; row->color.g = 0.88f; row->color.b = 0.92f;
    row->color.a = 1.0f;
    row->hit_eligible        = 1;
}

static void demo_fill_input_row(UiTextPanelRow *row, int line_idx) {
    memset(row, 0, sizeof(*row));
    row->text                = "";  /* INPUT rows ignore text */
    row->kind                = UI_TEXT_PANEL_ROW_INPUT;
    row->left_gutter_label   = line_idx + 1;
    row->source_line_idx     = line_idx;
    row->hit_target_line_idx = -1;
    row->search_row_idx      = -1;
    row->color.r = 0.95f; row->color.g = 0.95f; row->color.b = 1.0f;
    row->color.a = 1.0f;
    row->hit_eligible        = 1;
}

static int demo_build_snapshot(UiTextPanelRow *rows, int rows_cap,
                               UiTextPanelSnapshot *snap) {
    EditorBufferView buf = editor_buffer_view();
    EditorInputView  input = editor_state_input();

    int n = 0;
    int edit_line = editor_state_edit_line();
    if (edit_line < 0) edit_line = 0;
    int input_emitted = 0;

    /* Walk the buffer in document order, substituting an INPUT row
     * for the active edit line so it lands in the right vertical
     * position even when the buffer has committed lines after it. */
    for (int i = 0; i < buf.line_count && n < rows_cap; i++) {
        if (i == edit_line) {
            demo_fill_input_row(&rows[n++], i);
            input_emitted = 1;
            continue;
        }
        demo_fill_text_row(&rows[n++], editor_buffer_view_line(buf, i), i);
    }

    /* If the edit line is past the end of the buffer (empty buffer
     * or cursor parked on a virtual row beyond the last committed
     * line), emit the INPUT row last so the user always has
     * somewhere to type. */
    if (!input_emitted && n < rows_cap) {
        demo_fill_input_row(&rows[n++], edit_line);
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

/* Route a code-panel click to cursor placement. snap is built fresh
 * with the current EditorState so the hit-tester sees the same
 * geometry the renderer just used.
 *
 * ui_text_panel_hit_test expects raw GLUT window coordinates
 * (top-left origin) -- it does its own `vp_h - my` conversion to
 * bottom-left internally. Caller passes `y` straight through from
 * the GLUT mouse callback. */
static void demo_handle_code_panel_click(int mx, int glut_y) {
    UiTextPanelRow      rows[DEMO_MAX_ROWS];
    UiTextPanelSnapshot snap;
    demo_build_snapshot(rows, DEMO_MAX_ROWS, &snap);
    UiHit hit = ui_text_panel_hit_test(&snap, mx, glut_y);

    switch (hit.kind) {
    case UI_HIT_CODE_TEXT:
        /* Click on a committed text row -- navigate edit_line to
         * it, then place the cursor at the hit char. */
        if (hit.line_idx >= 0) {
            demo_input_navigate_to(hit.line_idx);
            if (hit.char_idx >= 0)
                editor_cursor_pos_set(hit.char_idx);
        }
        break;
    case UI_HIT_CODE_INSERT_LINE:
        /* Click on the active input row -- just place the cursor. */
        if (hit.char_idx >= 0)
            editor_cursor_pos_set(hit.char_idx);
        break;
    default:
        /* Gutter / chrome / outside-panel hits ignored. */
        break;
    }
}

static void demo_mouse_func(int button, int state, int x, int y) {
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN)
        return;
    /* The menu handler expects bottom-left coords (OpenGL); the
     * text-panel hit-tester expects raw top-left GLUT coords and
     * flips internally. Convert once for the menu, pass `y`
     * straight through for the panel. */
    int my_bl = g_demo_vp_h - y;
    if (demo_menu_handle_click(x, my_bl, g_demo_vp_w, g_demo_vp_h)) {
        glutPostRedisplay();
        return;
    }
    /* Click landed below the menu bar -- treat as code panel. */
    demo_handle_code_panel_click(x, y);
    glutPostRedisplay();
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

    printf("editor_demo: Esc to quit (q is a text character)\n");
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
