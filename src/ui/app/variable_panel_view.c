/*
 * ui_app_variable_panel_view.c - snapshot/live -> UiVariablePanelView projection.
 *
 * Placement comes from the overlay layout engine: both variants resolve the
 * panel's stack slot via ui_overlay_layout_panel_pos (eased once the
 * controller has ticked the engine, pure solve targets otherwise).
 */
#include "ui/app/variable_panel_view.h"

#include "ui/app/overlay_layout.h"  /* ui_overlay_layout_inputs/_panel_pos */
#include "ui/app/state.h"    /* ui_state_viewport, ui_state_profile/memory_panel */
#include "config.h"          /* MAX_PREDEF_VARS (for the row-cap assert) */
#include "c_compat.h"        /* STATIC_ASSERT */
#include "subsystems/variable_panel/variable_panel_state.h" /* variable_panel_view/_visible */

/* The renderer's row cap mirrors the predefined-variable table; pinned here
 * where both symbols are visible so they can't silently drift apart. */
STATIC_ASSERT(UI_VARIABLE_PANEL_MAX_ROWS == MAX_PREDEF_VARS,
              "UI_VARIABLE_PANEL_MAX_ROWS must equal MAX_PREDEF_VARS");

UiVariablePanelView ui_app_variable_panel_view(const UiRenderSnapshot *snap) {
    UiVariablePanelView v;
    v.visible        = snap->variable_panel.visible;
    v.window_w       = snap->viewport.window_w;
    v.window_h       = snap->viewport.window_h;
    {
        UiOverlayLayoutIn in = ui_overlay_layout_inputs(
            snap->variable_panel.visible,
            snap->variable_panel_vars.count,
            snap->profile_panel.mode,
            snap->memory_panel.mode,
            ui_overlay_layout_last_band_h());
        ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_VARIABLE,
                                    &v.panel_x, &v.panel_y);
    }
    v.vars            = snap->variable_panel_vars.vars;
    v.var_count       = snap->variable_panel_vars.count;
    v.drag_active_var = snap->variable_drag.active_var;
    v.drag_log_mode   = snap->variable_drag.log_mode;
    return v;
}

UiVariablePanelView ui_app_variable_panel_view_live(int var_count) {
    UiVariablePanelView v;
    UiViewportState vp = ui_state_viewport();
    v.visible        = variable_panel_visible();
    v.window_w       = vp.window_w;
    v.window_h       = vp.window_h;
    {
        UiOverlayLayoutIn in = ui_overlay_layout_inputs(
            variable_panel_visible(),
            var_count,
            ui_state_profile_panel().mode,
            ui_state_memory_panel().mode,
            ui_overlay_layout_last_band_h());
        ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_VARIABLE,
                                    &v.panel_x, &v.panel_y);
    }
    v.vars            = NULL;            /* geometry-only: no value rows */
    v.var_count       = var_count;
    v.drag_active_var = -1;
    v.drag_log_mode   = 0;
    return v;
}
