/*
 * ui_panels.h - Code-panel rendering, scene status, and UI input bridge.
 *
 * Renders the code-panel (source commands with syntax highlighting, overlays,
 * annotations), scene status banner, and handles user input (clicks, drags,
 * keyboard navigation). Bridges repl_editor.c input dispatch with color-picker
 * and panel-specific internals (scroll state, selection, etc.).
 *
 * Rendering:
 *   - ui_panels_render_code_panel(): Render the code panel with wrapped lines,
 *     syntax highlighting, overlays (cursor, selection, replay annotations).
 *   - ui_panels_render_scene_status(): Render the status banner below the scene
 *     (showing example name, status messages, etc.).
 *
 * Input handling (called by repl_editor.c):
 *   - Code-panel mouse: ui_panels_handle_code_panel_click/press/drag/release()
 *     handle editing, selection, cursor movement via mouse.
 *   - Menu/config: ui_panels_open_config() opens the Config menu;
 *     ui_panels_close_menus() closes all overlays.
 *   - Right-click: ui_panels_handle_right_press() shows context menu or color picker.
 *   - Global input: ui_panels_handle_escape/scene_press/motion/mouse_release()
 *     coordinate input across panels.
 *
 * Input bridge: Color-picker (floating overlay for inline color editing) state
 * is private to this module. Input handlers detect color-picker hits and manage
 * its input before forwarding to code-panel. ui_panels_handle_escape() cancels
 * color-picker without exposing its internals to repl_editor.c.
 *
 * Test helpers: ui_panels_code_panel_apply_scroll_follow_for_test() applies
 * scroll-follow logic for test verification (follow target → scroll position).
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

#include "ui_snapshot.h"

/* --- Rendering --- */

/* Render the code panel from the supplied snapshot. */
void ui_panels_render_code_panel(const UiRenderSnapshot *snap);

/* Render the scene status banner from the supplied snapshot. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap);

/* --- Menu/config dispatch (called by repl_editor.c) --- */

/* Open the Config menu (visual dropdown showing toggles/cycles). */
void ui_panels_open_config(void);

/* Close all menus/overlays (Config, Example dropdown, color picker, etc.). */
void ui_panels_close_menus(void);

/* --- Code-panel mouse input --- */

/* Handle left-click in code panel: move cursor, extend selection, or begin drag.
 * Returns the cursor position to apply, or -1 if the click did not move it.
 * mx, my are window coordinates. Called by repl_editor.c on GLUT mouse down. */
int ui_panels_handle_code_panel_click(int mx, int my);

/* Handle right-click in code panel: show context menu or color picker for inline
 * color editing. Returns a bitmask (UI_PANEL_PRESS_*): CONSUMED if click was
 * handled, OPENED_COLOR_PICKER if color picker was opened. If the press path
 * changes the cursor, stores the new cursor position in cursor_pos_out; -1 means
 * no cursor update. Called by repl_editor.c on right-mouse down. */
#define UI_PANEL_PRESS_NONE                0
#define UI_PANEL_PRESS_CONSUMED            (1 << 0)
#define UI_PANEL_PRESS_OPENED_COLOR_PICKER (1 << 1)
int  ui_panels_handle_code_panel_press(int mx, int my, int *cursor_pos_out);

/* Handle mouse drag in code panel: extend selection, drag scroll, or adjust color
 * picker (if active). mx, my are window coordinates. Called by repl_editor.c on
 * GLUT motion while button pressed. */
int  ui_panels_handle_code_panel_drag(int mx, int my);

/* Handle mouse release in code panel: finalize selection, end drag. Called by
 * repl_editor.c on GLUT mouse up. */
void ui_panels_handle_code_panel_release(void);

/* --- Global input bridge (routes input across panels) --- */

/* Handle Escape key: cancel/close color picker if active, otherwise close menus.
 * Returns 1 if color picker consumed the key, 0 otherwise. Bridges to color-picker
 * state without exposing internals to repl_editor.c. */
int  ui_panels_handle_escape(void);

/* Handle mouse press on scene (3D geometry area): select orbit target or pan
 * camera. Returns 1 if consumed, 0 if caller should forward to camera controls.
 * mx, my are window coordinates. */
int  ui_panels_handle_scene_press(int mx, int my);

/* Handle mouse motion: forward to color picker or camera/drag handler as needed.
 * Returns 1 if consumed, 0 otherwise. mx, my are window coordinates. */
int  ui_panels_handle_motion(int mx, int my);

/* Handle mouse release: finalize drag/selection across all panels. Called when
 * any mouse button is released. */
void ui_panels_handle_mouse_release(void);

/* Handle right-click in non-code-panel areas: open Config menu if clicked on
 * menu bar region. Returns 1 if consumed, 0 otherwise. mx, my are window
 * coordinates. */
int  ui_panels_handle_right_press(int mx, int my);

/* --- Test helpers --- */

/* Apply scroll-follow logic without rendering (for test verification). Computes
 * the scroll target for the follow line and outputs the follow_doc_line and
 * visible_lines. Used by tests to verify scroll-follow calculation. Returns 1
 * on success, 0 if follow target is out of bounds. */
int  ui_panels_code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                                       int *out_visible_lines);

#endif /* UI_PANELS_H */
