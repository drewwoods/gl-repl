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
 * Render3dRenderConfig), NOT the whole-frame `UiRenderSnapshot`. That keeps this
 * subsystem linkable from {subsystems, support, ui/core} alone - no ui/app.
 * The controller projects the snapshot down via
 * `ui_app_variable_panel_view()` (src/ui/app/variable_panel_view.c).
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

#include "config.h"     /* MAX_PREDEF_VARS */
#include "ui/core/hit.h"

/* Hit kinds this renderer emits. Owned here (not in ui/app/hit.h) as fixed
 * offsets off UI_HIT_CORE_COUNT, in a reserved subsystem range clear of
 * ui/app/hit.h's contiguous app kinds, so the renderer names them without
 * depending on ui/app. UiHit.kind is an int, so the ranges coexist. */
enum { UI_HIT_VARIABLE_SLIDER = UI_HIT_CORE_COUNT + 64 };
enum { UI_HIT_VARIABLE_COLLAPSE_TOGGLE = UI_HIT_CORE_COUNT + 67 };

/* Max slider rows the panel will draw. Mirrors the predefined-variable
 * table size from the global REPL variable-table contract. */
enum { UI_VARIABLE_PANEL_MAX_ROWS = MAX_PREDEF_VARS };

enum { UI_VARIABLE_NAME_MAX = 16 };

/* One UI-facing variable row. `name` is copied; `value` points at the live
 * source value for the frame (const - the renderer never writes through it). */
typedef struct {
    char         name[UI_VARIABLE_NAME_MAX];
    const float *value;
    int          tuned;   /* 1 if @tune-tagged (exported as a keyboard knob) */
    int          written; /* 1 if source contains CMD_VAR_ASSIGN writing it
                           * (rendered dimmed as program state); suppressed
                           * for vars on a `// @config` decl line */
} UiVariable;

typedef struct {
    const UiVariable *vars;
    int               count;
} UiVariableList;

/* Narrow per-frame view. All pointer fields are const (check-views-flat).
 * panel_x/panel_y is the resolved bottom-left position (GL window coords):
 * the app side bakes it via the overlay layout engine
 * (src/ui/app/overlay_layout.c), standalone drivers set it directly - the
 * renderer holds no placement policy of its own. */
typedef struct {
    int   visible;
    int   window_w, window_h;
    int   panel_x, panel_y;    /* resolved position (bottom-left, GL coords) */
    const UiVariable *vars;    /* may be NULL for hit-only views */
    int   var_count;
    int   drag_active_var;     /* row being dragged, -1 when idle */
    int   drag_coarse;         /* 1 = coarse (right-click) drag (only meaningful
                                * when dragging) */
    int   collapsed;           /* 1 = only the title bar is shown; slider rows
                                * and the value/track columns are hidden */
} UiVariablePanelView;

/* Render the variable panel with all declared variables and current values.
 * Reads only from the supplied view. Rows are compacted (unused slots
 * skipped). Called once per frame if the variable panel is enabled. */
void ui_variable_panel_render(const UiVariablePanelView *view);

/* Panel size for a declared-variable count (clamped to the row cap) and
 * collapse state. Pure; the app's overlay layout engine uses it to size the
 * panel's stack slot. Collapsed drops the row area entirely, leaving just
 * the title bar. */
void ui_variable_panel_size(int var_count, int collapsed, int *pw, int *ph);

/* Query the variable panel's bounding rectangle (window/screen coordinates).
 * Position comes from the view's resolved panel_x/panel_y; size from the
 * view's var_count via ui_variable_panel_size. */
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

/* Pure hit-test for the title bar's collapse/expand chip. Returns 1 (and
 * classifies the click as UI_HIT_VARIABLE_COLLAPSE_TOGGLE through
 * ui_variable_panel_hit_test) when (mx, my) lands on the chip; 0 otherwise.
 * Exposed separately so callers that already resolved a view can probe the
 * chip without redoing ui_variable_panel_hit_test's full dispatch. */
int  ui_variable_panel_hit_test_collapse_toggle(const UiVariablePanelView *view,
                                                int mx, int my);

#endif /* UI_VARIABLE_PANEL_H */
