/*
 * ui_color_picker.h - Floating inline color editor (RGB sliders).
 *
 * Floating overlay for editing glColor3f/glColor4f command arguments
 * inline. Opened by right-clicking a color command; renders R/G/B
 * (and optionally A) sliders.
 *
 * Target contract (Phase E onward):
 *
 *   UI renders the picker overlay and reports `UiHit` results from
 *   ui_color_picker_hit_test() (UI_HIT_COLOR_SWATCH on the slider
 *   rects). `imrepl_ctrl` routes the hit; the owning subsystem
 *   (editor commit pipeline) mutates source text. The color picker
 *   is the one peer that crosses back into the editor commit path —
 *   eventually its writeback uses repl_compile / editor_commit_apply.
 *
 * Hit-test: ui_color_picker_hit_test() returns UI_HIT_COLOR_SWATCH
 * when the pointer lands on the SV / hue / alpha rect of an open
 * picker; cmd_idx carries the active picker line, item_idx encodes
 * the slider region (1=SV, 2=hue, 3=alpha).
 *
 * Legacy imperative handlers (transitional): ui_color_picker_press /
 * _motion / _release still mutate the source command directly via
 * repl_command_store_replace_one + editor_buffer_replace_line. These
 * call sites are tracked by `check-ui-returns-hits-only` and are
 * scheduled to move through editor compile/apply in Phase F+.
 *
 * Geometry: Position anchored near the clicked code-panel line; color
 * swatches (small boxes) appear inline in the code panel to show
 * color values at a glance. ui_color_picker_render_swatch() draws
 * those swatches.
 *
 * Constraints: Only glColor3f/glColor4f commands can be edited
 * (checked by can_edit_cmd). Arguments must be constant expressions
 * — if a command has variables, the picker can't open on it.
 */
#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

#include "ui_editor.h"
#include "ui_hit.h"
#include "ui_snapshot.h"

/* Width of inline color swatch boxes (shown in code panel). */
#define UI_COLOR_SWATCH_W 12

/* Query the source command index of the currently open color picker. Returns
 * the command index, or -1 if no picker is open. Used to determine which
 * command is being edited. */
int  ui_color_picker_active_line(void);

/* Check whether a command is editable by the color picker. Returns 1 if the
 * command is a glColor3f/glColor4f command with constant (non-variable)
 * arguments, 0 otherwise. Used before opening the picker to validate that
 * the command can be edited. */
int  ui_color_picker_can_edit_cmd(int cmd_idx);

/* Open the color picker on a specific command. cmd_idx is the source command
 * index (must pass can_edit_cmd check first). my is the mouse y-coordinate
 * where the picker was opened (for positioning). Called by ui_panels.c on
 * right-click of a color command. */
void ui_color_picker_open(int cmd_idx, int my);

/* Render the color picker overlay (RGB/RGBA sliders) once per frame. Called
 * by ui_panels.c if active_line() != -1. Slider values are synced to the
 * current command's arguments. Reads the supplied snapshot for layout
 * data; performs no live REPL state reads inside the render path. */
void ui_color_picker_render(const UiRenderSnapshot *snap);

/* Render an inline color swatch from a controller-pushed transformer entry.
 * sx, sy are pixel coordinates in the code panel. The renderer reads color
 * directly from t->state.color, so no live document lookup is required. */
void ui_color_picker_render_swatch(const EditorTransformer *t, int sx, int sy);

/* Handle mouse press in the color picker. mx, my are window coordinates.
 * Detects which slider was clicked and begins dragging. Returns 1 if the
 * picker consumed the click, 0 if not. Called by ui_panels.c on left-mouse
 * down while picker is active. */
int  ui_color_picker_press(int mx, int my);

/* Handle mouse motion during color picker drag. mx, my are window coordinates.
 * Updates the dragged slider value and syncs the command source in real-time.
 * Returns 1 if motion was consumed, 0 if not. Called by ui_panels.c on
 * GLUT motion while a slider is being dragged. */
int  ui_color_picker_motion(int mx, int my);

/* Handle mouse release during color picker drag. Finalizes the slider drag
 * (value is already synced during motion). Called by ui_panels.c on
 * left-mouse up. */
void ui_color_picker_release(void);

/* Close the color picker and dismiss the overlay. Returns 1 if a picker was
 * open and closed, 0 if no picker was active. Called by Escape key handler
 * or when user clicks outside the picker. */
int  ui_color_picker_close(void);

/* Pure hit-test: classify (mx, my) as a UiHit for the floating color picker.
 *
 * Phase E commit 29 entry. Returns UI_HIT_COLOR_SWATCH if the pointer lands
 * on a slider rectangle of the open picker; cmd_idx carries the source-command
 * index whose color is being edited, item_idx encodes the slider region
 * (1 = SV square, 2 = hue bar, 3 = alpha bar). Returns UI_HIT_NONE if the
 * picker is closed or the pointer is outside its rects. Reads picker state
 * only; never mutates. */
UiHit ui_color_picker_hit_test(int mx, int my);

#endif /* UI_COLOR_PICKER_H */
