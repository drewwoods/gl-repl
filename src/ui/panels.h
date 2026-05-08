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
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

#include "snapshot.h"
#include "hit.h"

/* --- Rendering --- */

/* Per-frame render output: values the renderer discovers while drawing
 * that the controller actualizes after the render call. Today this
 * carries the editor cursor's window-pixel position, which the
 * floating autocomplete popup needs to anchor itself under the cursor.
 * The cursor pixel is not durable state — it's recomputed every frame
 * from the same wrap/segment math the renderer is already running, so
 * surfacing it as a per-frame output (rather than a state field
 * mutated mid-render) keeps `ui_*` pure.
 *
 * `cursor_valid` is 0 when the active input row didn't render this
 * frame (code panel hidden, edit row scrolled offscreen). The
 * controller leaves any prior cursor coords undisturbed in that
 * case — same behaviour as the legacy mid-render publish, which
 * simply didn't fire in those frames. */
typedef struct UiCodePanelOutput {
    int cursor_px;
    int cursor_py;
    int cursor_valid;
} UiCodePanelOutput;

/* Render the code panel from the supplied snapshot. `out` is optional;
 * pass NULL when the caller doesn't need the cursor-pixel discovery
 * (test fixtures that aren't checking autocomplete anchoring). */
void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out);

/* Render the scene status banner from the supplied snapshot. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap);

/* Handle right-click in non-code-panel areas: open Config menu if clicked on
 * menu bar region. Returns 1 if consumed, 0 otherwise. mx, my are window
 * coordinates. */
int  ui_panels_handle_right_press(int mx, int my);

/* Pure hit-test: classify the pointer at (mx, my) as a `UiHit`.
 * glr_ctrl_router_handle_code_panel_hit is the canonical consumer — it
 * dispatches by UiHit.kind to the owning subsystem. `variable_count` is
 * supplied by the controller so UI hit-testing does not read the live
 * REPL variable table. */
UiHit ui_panels_hit_test(int mx, int my, int variable_count);

/* --- Test helpers --- */

/* Apply scroll-follow logic without rendering (for test verification). Computes
 * the scroll target for the follow line and outputs the follow_doc_line and
 * visible_lines. `show_vertex_indices` mirrors
 * `ReplPresentationState.show_vertex_indices` so the gutter width matches what
 * the renderer will use. Used by tests to verify scroll-follow calculation.
 * Returns 1 on success, 0 if follow target is out of bounds. */
int  ui_panels_code_panel_apply_scroll_follow_for_test(int show_vertex_indices,
                                                       int *out_follow_doc_line,
                                                       int *out_visible_lines);

#endif /* UI_PANELS_H */
