/*
 * ui_variable_panel.h - Floating variable slider panel renderer and hit-test.
 *
 * Draws the compact floating panel of declared predefined variables and classifies
 * pointer hits on its slider rows. The panel is a read-only view over current
 * variable values: visibility lives in the variable-panel peer subsystem, and
 * drag/value mutation lives in `subsystems/variable_panel/variable_panel_drag.h` plus controller
 * routing. UI only renders and returns `UiHit`.
 *
 * The renderer consumes a narrow `UiVariablePanelView` (the 2D analog of
 * SceneRenderConfig), NOT the whole-frame `UiRenderSnapshot`. That keeps this
 * subsystem linkable from {subsystems, support, ui/core} alone — no ui/app.
 * The controller projects the snapshot down via
 * `ui_app_variable_panel_view()` (src/ui/app/variable_panel_view.c).
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

#include "config.h"     /* MAX_PREDEF_VARS */
#include "ui/core/hit.h"

/* Hit kind this renderer emits. Owned here (not in ui/app/hit.h) as a fixed
 * offset off UI_HIT_CORE_COUNT, in a reserved subsystem range clear of
 * ui/app/hit.h's contiguous app kinds, so the renderer names it without
 * depending on ui/app. UiHit.kind is an int, so the ranges coexist. */
enum { UI_HIT_VARIABLE_SLIDER = UI_HIT_CORE_COUNT + 64 };

/* Max slider rows the panel will draw. Mirrors the predefined-variable
 * table size from the global REPL variable-table contract. */
enum { UI_VARIABLE_PANEL_MAX_ROWS = MAX_PREDEF_VARS };

enum { UI_VARIABLE_NAME_MAX = 16 };

/* One UI-facing variable row. `name` is copied; `value` points at the live
 * source value for the frame (const — the renderer never writes through it). */
typedef struct {
    char         name[UI_VARIABLE_NAME_MAX];
    const float *value;
    int          tuned;   /* 1 if @tune-tagged (exported as a keyboard knob) */
} UiVariable;

typedef struct {
    const UiVariable *vars;
    int               count;
} UiVariableList;

/* Narrow per-frame view. All pointer fields are const (check-views-flat).
 * The controller bakes the scene rect / statusbar inset / code-panel-at-top
 * flag so the renderer needs nothing from ui/app/layout or ui/app/state. */
typedef struct {
    int   visible;
    float replay_lift_px;
    int   window_w, window_h;
    int   scene_x, scene_y, scene_w, scene_h;
    int   statusbar_h;        /* bottom inset the panel floats above */
    int   code_panel_at_top;  /* overflow fallback parks the panel at the top */
    const UiVariable *vars;    /* may be NULL for hit-only views */
    int   var_count;
    int   drag_active_var;     /* row being dragged, -1 when idle */
    int   drag_log_mode;       /* 1 = log drag (only meaningful when dragging) */
} UiVariablePanelView;

/* Render the variable panel with all declared variables and current values.
 * Reads only from the supplied view. Rows are compacted (unused slots
 * skipped). Called once per frame if the variable panel is enabled. */
void ui_variable_panel_render(const UiVariablePanelView *view);

/* Query the variable panel's bounding rectangle (window/screen coordinates).
 * Outputs panel position (px, py) and size (pw, ph) for the view's var_count. */
void ui_variable_panel_rect(const UiVariablePanelView *view,
                            int *px, int *py, int *pw, int *ph);

/* Hit-test a click in the variable panel. gx, gy are window/screen coords.
 * Returns 1 and fills out_row with the variable row index (0 = first declared
 * variable) when a row is clicked; 0 if outside the panel or between rows. */
int  ui_variable_panel_hit_row(const UiVariablePanelView *view,
                               int gx, int gy, int *out_row);

/* Pure hit-test: classify (mx, my) as a UiHit for the variable panel. Returns
 * UI_HIT_VARIABLE_SLIDER (item_idx = row) on a slider row; UI_HIT_NONE if the
 * panel is hidden or the pointer is outside it. Reads only; never mutates. */
UiHit ui_variable_panel_hit_test(const UiVariablePanelView *view, int mx, int my);

#endif /* UI_VARIABLE_PANEL_H */
