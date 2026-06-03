/*
 * ui_app_variable_panel_view.c - snapshot/live -> UiVariablePanelView projection.
 */
#include "ui/app/variable_panel_view.h"

#include "ui/app/layout.h"   /* ui_layout_scene_rect, STATUSBAR_H, CODE_PANEL_LAYOUT_TOP */
#include "ui/app/state.h"    /* ui_state_viewport */
#include "config.h"          /* MAX_PREDEF_VARS (for the row-cap assert) */
#include "c_compat.h"        /* STATIC_ASSERT */
#include "subsystems/variable_panel/variable_panel_state.h" /* variable_panel_view/_visible */

/* The renderer's row cap mirrors the predefined-variable table; pinned here
 * where both symbols are visible so they can't silently drift apart. */
STATIC_ASSERT(UI_VARIABLE_PANEL_MAX_ROWS == MAX_PREDEF_VARS,
              "UI_VARIABLE_PANEL_MAX_ROWS must equal MAX_PREDEF_VARS");

static int code_panel_is_top(void) {
    return ui_layout_code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP;
}

UiVariablePanelView ui_app_variable_panel_view(const UiRenderSnapshot *snap) {
    UiVariablePanelView v;
    v.visible        = snap->variable_panel.visible;
    v.replay_lift_px = snap->variable_panel.replay_lift_px;
    v.window_w       = snap->viewport.window_w;
    v.window_h       = snap->viewport.window_h;
    ui_layout_scene_rect(&v.scene_x, &v.scene_y, &v.scene_w, &v.scene_h);
    v.statusbar_h       = STATUSBAR_H;
    v.code_panel_at_top = code_panel_is_top();
    v.vars            = snap->variable_panel_vars.vars;
    v.var_count       = snap->variable_panel_vars.count;
    v.drag_active_var = snap->variable_drag.active_var;
    v.drag_log_mode   = snap->variable_drag.log_mode;
    return v;
}

UiVariablePanelView ui_app_variable_panel_view_live(int var_count) {
    UiVariablePanelView v;
    VariablePanelViewState st = variable_panel_view();
    UiViewportState        vp = ui_state_viewport();
    v.visible        = variable_panel_visible();
    v.replay_lift_px = st.replay_lift_px;
    v.window_w       = vp.window_w;
    v.window_h       = vp.window_h;
    ui_layout_scene_rect(&v.scene_x, &v.scene_y, &v.scene_w, &v.scene_h);
    v.statusbar_h       = STATUSBAR_H;
    v.code_panel_at_top = code_panel_is_top();
    v.vars            = NULL;            /* geometry-only: no value rows */
    v.var_count       = var_count;
    v.drag_active_var = -1;
    v.drag_log_mode   = 0;
    return v;
}
