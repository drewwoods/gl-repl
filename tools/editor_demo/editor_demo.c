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
 *   - src/ui/core/text_panel.c    : generic wrapped text renderer.
 *   - src/ui/core/text_layout.c   : wrap math.
 *   - src/ui/core/text_search.c   : case-insensitive text find (linked for the
 *                              text_panel's search-row machinery — find is
 *                              not bound to a key in v1).
 *   - src/support/prof.c     : profiling.
 *   - tools/editor_demo/input.c : the demo's own generic key dispatcher.
 *   - tools/editor_demo/menu.c  : the demo's own File menu.
 *
 * What does NOT link: anything under src/repl, src/app, src/scene, or
 * src/subsystems, plus the REPL-flavored editor controller files listed
 * above. There is no fake EditorServices instance, no per-symbol
 * REPL / glr / ui / tutorial stub block, and (since Phase 4 of
 * plans/done/edit-line-ownership.md flipped storage and Phase 5
 * deleted the shim file) no repl_shim.c either — the editor now
 * owns its edit-line cursor on EditorState.document.edit_line_idx.
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
#include "ui/core/gl_2d.h"
#include "ui/core/text_panel.h"

#include "input.h"
#include "menu.h"

#include "gl_includes.h"

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
 * render/hit-test call. Edit-line lives on EditorState.document
 * (Phase 4 of plans/done/edit-line-ownership.md); read it via
 * editor_state_edit_line(). */
static void demo_fill_text_row(UiTextPanelRow *row, const char *text,
                               int line_idx) {
    memset(row, 0, sizeof(*row));
    row->text                = text ? text : "";
    row->kind                = UI_TEXT_PANEL_ROW_TEXT;
    row->left_gutter_label   = line_idx + 1;
    row->source_line_idx     = line_idx;
    row->hit_target_line_idx = -1;
    /* search_row_idx == line index in the demo's flat model. The
     * text_panel highlights any row whose search_row_idx >= 0
     * when snap->search.active and query matches; the row whose
     * (search_row_idx, char) equals (snap->search.hit_row, hit_char)
     * gets the brighter "current hit" accent. */
    row->search_row_idx      = line_idx;
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

    /* Find/search state flows in from the demo's input dispatcher.
     * When active, text_panel highlights every match in rows whose
     * search_row_idx >= 0; the row matching (hit_row, hit_char)
     * gets the brighter "current hit" accent. The find-bar itself
     * is drawn separately as a top overlay (text_panel doesn't
     * render the bar — only the highlights). */
    snap->search.active    = demo_search_active();
    snap->search.query     = demo_search_query();
    snap->search.query_len = demo_search_query_len();
    snap->search.hit_row   = demo_search_hit_line();
    snap->search.hit_char  = demo_search_hit_char();

    return n;
}

#ifndef GL_STUBS

/* Tiny find-bar overlay drawn just below the menu bar when search
 * is active. text_panel handles the in-line match highlights, but
 * not the bar showing the live query — that's a demo concern. */
static void demo_render_find_bar(void) {
    if (!demo_search_active()) return;

    float bar_y_top = (float)(g_demo_vp_h - DEMO_MENU_BAR_H);
    float bar_h     = 22.0f;
    float bar_y_bot = bar_y_top - bar_h;

    glColor3f(0.16f, 0.16f, 0.20f);
    glRectf(0, bar_y_bot, (float)g_demo_vp_w, bar_y_top);

    /* "Find: <query>" — Esc to cancel, Enter to accept hint on
     * the right. Hit-count "no match" indicator when query is
     * non-empty but no match found. */
    char buf[DEMO_SEARCH_QUERY_MAX + 32];
    const char *suffix =
        demo_search_query_len() == 0       ? "" :
        demo_search_hit_line() < 0         ? "  (no match)" : "";
    snprintf(buf, sizeof(buf), "Find: %s%s",
             demo_search_query(), suffix);
    glColor3f(0.92f, 0.94f, 0.96f);
    gl2d_draw_string(10.0f, bar_y_bot + 6.0f, buf, GLUT_BITMAP_9_BY_15);

    glColor3f(0.55f, 0.60f, 0.70f);
    const char *hint = "Enter: go to match   Esc: cancel   F3 / Cmd-G: next";
    int hint_x = g_demo_vp_w - (int)strlen(hint) * 9 - 10;
    gl2d_draw_string((float)hint_x, bar_y_bot + 6.0f, hint,
                     GLUT_BITMAP_9_BY_15);
}

static void demo_display_func(void) {
    glClearColor(0.10f, 0.10f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    UiTextPanelRow      rows[DEMO_MAX_ROWS];
    UiTextPanelSnapshot snap;
    UiTextPanelOutput   out = {0};
    demo_build_snapshot(rows, DEMO_MAX_ROWS, &snap);
    ui_text_panel_render(&snap, &out);

    /* The text panel pushes/pops its own 2D ortho via gl2d_begin/end.
     * Push a fresh one for menu chrome + find bar so their draw state
     * doesn't depend on whatever the panel last set. */
    gl2d_begin(g_demo_vp_w, g_demo_vp_h);
    demo_menu_render(g_demo_vp_w, g_demo_vp_h);
    demo_render_find_bar();
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

/* Single-line drag-selection bookkeeping. The active line is read
 * from editor_state_edit_line at the moment of the initial mouse
 * down (after demo_handle_code_panel_click navigates to it), so
 * the drag stays clamped to the line the user clicked on — cross-
 * line drag would need either snap-to-start-line or a buffer-line-
 * range selection mode, both deferred. */
static int g_demo_drag_active = 0;

/* Extend the input-row character-range selection to the hit
 * position under (mx, glut_y), clamped to the active line. The
 * extend primitive pins the pre-move cursor as the anchor on first
 * call, then grows / shrinks in place. */
static void demo_handle_code_panel_drag(int mx, int glut_y) {
    UiTextPanelRow      rows[DEMO_MAX_ROWS];
    UiTextPanelSnapshot snap;
    demo_build_snapshot(rows, DEMO_MAX_ROWS, &snap);
    UiHit hit = ui_text_panel_hit_test(&snap, mx, glut_y);

    int active_line = editor_state_edit_line();
    int target;

    /* The active edit line is rendered as a single INPUT row.
     * Dragging within that row gives a clean char hit on the
     * INPUT_LINE kind. Dragging above / below the row clamps to
     * the row's bounds — the visible selection stays on the line
     * where the drag started. */
    if (hit.kind == UI_HIT_CODE_INSERT_LINE && hit.char_idx >= 0) {
        target = hit.char_idx;
    } else if (hit.kind == UI_HIT_CODE_TEXT &&
               hit.line_idx >= 0 && hit.char_idx >= 0 &&
               hit.line_idx == active_line) {
        target = hit.char_idx;
    } else if (hit.kind == UI_HIT_CODE_TEXT &&
               hit.line_idx >= 0 && hit.line_idx < active_line) {
        target = 0;
    } else {
        target = editor_input_len();
    }

    editor_cursor_pos_extend_selection(target);
}

static void demo_mouse_func(int button, int state, int x, int y) {
    if (button != GLUT_LEFT_BUTTON) return;

    if (state == GLUT_UP) {
        g_demo_drag_active = 0;
        return;
    }

    /* state == GLUT_DOWN below. The menu handler expects bottom-
     * left coords (OpenGL); the text-panel hit-tester expects raw
     * top-left GLUT coords and flips internally. Convert once for
     * the menu, pass `y` straight through for the panel. */
    int my_bl = g_demo_vp_h - y;
    if (demo_menu_handle_click(x, my_bl, g_demo_vp_w, g_demo_vp_h)) {
        glutPostRedisplay();
        return;
    }
    /* Click landed below the menu bar -- treat as code panel.
     * demo_handle_code_panel_click sets the cursor and clears any
     * existing anchor; arming drag-active here means the first
     * subsequent motion event extends a fresh selection from the
     * click position. */
    demo_handle_code_panel_click(x, y);
    g_demo_drag_active = 1;
    glutPostRedisplay();
}

static void demo_motion_func(int x, int y) {
    if (!g_demo_drag_active) return;
    demo_handle_code_panel_drag(x, y);
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
    /* glutMotionFunc fires only while a button is held — exactly
     * the "drag" event. demo_motion_func gates on g_demo_drag_active
     * (set by mouse-down on the code panel, cleared on mouse-up)
     * so non-code-panel drags (a click that landed on the menu
     * bar) don't accidentally extend a selection. */
    glutMotionFunc(demo_motion_func);

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
