/*
 * ui_app_variable_panel_view.h - Project app state into the variable panel's
 * narrow render view.
 *
 * The variable-panel renderer (src/ui/subsystems/variable_panel.c) consumes a
 * dependency-light UiVariablePanelView, never the whole-frame UiRenderSnapshot
 * or ui/app/layout / ui/app/state directly. This app-layer helper bakes the
 * scene rect, statusbar inset, and code-panel-at-top flag into that view so
 * the subsystem stays linkable from {ui/core, config} alone. It is the
 * 2D-panel analogue of glr_ctrl building Render3dRenderConfig.
 */
#ifndef UI_APP_VARIABLE_PANEL_VIEW_H
#define UI_APP_VARIABLE_PANEL_VIEW_H

#include "ui/app/snapshot.h"
#include "ui/subsystems/variable_panel.h"

/* Build the view from a per-frame snapshot (the render + snapshot-hit path). */
UiVariablePanelView ui_app_variable_panel_view(const UiRenderSnapshot *snap);

/* Build a geometry-only view from LIVE app state, for the mouse-down hit path
 * that runs outside a frame snapshot. vars is left NULL (no value rows), so
 * the result serves rect / hit-row queries only. The caller supplies the
 * current declared-variable count (e.g. repl_eval_predef_view().count). */
UiVariablePanelView ui_app_variable_panel_view_live(int var_count,
                                                    int time_row,
                                                    int time_playing);

#endif /* UI_APP_VARIABLE_PANEL_VIEW_H */
