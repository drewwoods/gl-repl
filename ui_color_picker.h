/*
 * ui_color_picker.h - Floating inline color editor (RGB sliders).
 *
 * Floating modal overlay for editing glColor3f/glColor4f command arguments
 * inline. Opened by right-clicking a color command in the code panel; renders
 * R/G/B (and optionally A) sliders. Dragging sliders updates the command's
 * source text in real-time, allowing interactive color tweaking with live
 * preview of geometry color changes.
 *
 * Lifecycle: ui_color_picker_open() initializes a picker on a specific command
 * (must be a color command, checked via can_edit_cmd). ui_color_picker_render()
 * draws the picker overlay once per frame. Input is routed by ui_panels.c:
 * press/motion/release handle slider dragging. ui_color_picker_close() or
 * pressing Escape dismisses the picker and commits the final value.
 *
 * Geometry: Position anchored near the clicked code-panel line (my parameter);
 * color swatches (small boxes) appear inline in the code panel to show color
 * values at-a-glance. ui_color_picker_render_swatch() draws these boxes.
 *
 * Integration: ui_panels.c bridges input between the editor and color picker,
 * ensuring the picker stays in focus while active. ui_panels_handle_escape()
 * closes the picker; ui_panels_handle_motion/press/release() forward mouse
 * events to color picker if active.
 *
 * Constraints: Only glColor3f/glColor4f commands can be edited (checked by
 * can_edit_cmd). Arguments must be constant expressions (no variables), so
 * slider values are well-defined. If a command has variables, the picker
 * can't open on it.
 */
#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

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

/* Render an inline color swatch (small colored box) for a command in the code
 * panel. cmd_idx is the source command index; sx, sy are pixel coordinates in
 * the code panel. Draws a small box showing the command's color value. Called
 * by the code-panel renderer for each color command to provide at-a-glance
 * color feedback. */
void ui_color_picker_render_swatch(int cmd_idx, int sx, int sy);

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

#endif /* UI_COLOR_PICKER_H */
