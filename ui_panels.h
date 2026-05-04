/*
 * ui_panels.h - Code-panel rendering, scene status, and pointer hit-test.
 *
 * Contract (Phase J2 onward):
 *
 *   UI renders the code-panel / status banner and classifies pointer
 *   locations into a neutral `UiHit` via `ui_panels_hit_test`.
 *   `imrepl_ctrl` routes the hit to the owning subsystem (editor /
 *   variable_panel / replay / scene / color picker / menu) which
 *   implements the behavior. UI does not own input dispatch or
 *   mutation.
 *
 * Rendering:
 *   - ui_panels_render_code_panel(): Render the code panel with wrapped
 *     lines, syntax highlighting, overlays (cursor, selection, replay
 *     annotations).
 *   - ui_panels_render_scene_status(): Render the status banner below
 *     the scene (showing example name, status messages, etc.).
 *
 * Hit-test:
 *   - ui_panels_hit_test(): Pure classification of (mx, my) into a
 *     `UiHit`. Dispatches to the floating-overlay hit-testers in
 *     priority order (help > color picker > menu bar > variable
 *     panel > code panel > scene). Reads layout / state only. Sets
 *     line_idx / char_idx / cmd_idx / item_idx according to the
 *     per-kind contract documented in ui_hit.h.
 *
 * Right-click handler:
 *   - ui_panels_handle_right_press(): only thin wrapper that survives
 *     because right-click on the menu bar is treated as a Config-menu
 *     shortcut. Right-click hit-test classification is not yet on
 *     ui_panels_hit_test (deferred — the hit-test surface today is
 *     left-click oriented).
 *
 * Menu / config dispatch helpers:
 *   - ui_panels_open_config(), ui_panels_close_menus().
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

#include "ui_snapshot.h"
#include "ui_hit.h"

/* --- Rendering --- */

/* Render the code panel from the supplied snapshot. */
void ui_panels_render_code_panel(const UiRenderSnapshot *snap);

/* Render the scene status banner from the supplied snapshot. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap);

/* --- Menu/config dispatch --- */

/* Open the Config menu (visual dropdown showing toggles/cycles). */
void ui_panels_open_config(void);

/* Close all menus/overlays (Config, Example dropdown, color picker, etc.). */
void ui_panels_close_menus(void);

/* Handle right-click in non-code-panel areas: open Config menu if clicked on
 * menu bar region. Returns 1 if consumed, 0 otherwise. mx, my are window
 * coordinates. */
int  ui_panels_handle_right_press(int mx, int my);

/* Pure hit-test: classify the pointer at (mx, my) as a `UiHit`.
 * imrepl_ctrl_router_handle_code_panel_hit (declared in
 * imrepl_ctrl.h) is the canonical consumer — it dispatches by
 * UiHit.kind to the owning subsystem. */
UiHit ui_panels_hit_test(int mx, int my);

/* --- Test helpers --- */

/* Apply scroll-follow logic without rendering (for test verification). Computes
 * the scroll target for the follow line and outputs the follow_doc_line and
 * visible_lines. Used by tests to verify scroll-follow calculation. Returns 1
 * on success, 0 if follow target is out of bounds. */
int  ui_panels_code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                                       int *out_visible_lines);

#endif /* UI_PANELS_H */
