/*
 * ui_layout.c - pure window layout geometry.
 */
#include "ui/app/layout.h"
#include "ui/app/state_types.h"
#include "ui/core/metrics.h"

/* ui_state_viewport / ui_state_code_panel are forward-declared here
 * because repl_*.c is not allowed to include ui_state.h per
 * check-controller-boundaries. Layout reads viewport size, panel
 * fraction, and the controller-mirrored layout_mode; all are UiState
 * chrome. */
UiViewportState         ui_state_viewport(void);
UiCodePanelRuntimeState ui_state_code_panel(void);

int ui_layout_code_panel_layout_mode(void) {
    int layout = ui_state_code_panel().layout_mode;
    if (layout < 0 || layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return layout;
}

static int ui_layout_panel_span_px(int total_px) {
    int span = (int)((float)total_px * ui_state_code_panel().panel_frac);
    if (span < 1) span = 1;
    if (span > total_px) span = total_px;
    return span;
}

void ui_layout_code_panel_rect(int *x, int *y, int *w, int *h) {
    int layout = ui_layout_code_panel_layout_mode();
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    /* Callers can pass any subset of outputs. Every visible code-panel
     * layout anchors at x = 0; y/h vary by mode, and the hidden layout
     * zeros the whole rect. */
    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = ui_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = win_h - panel_h;
        if (w) *w = win_w;
        if (h) *h = panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = ui_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = panel_h;
    } else {
        int panel_w = ui_layout_panel_span_px(win_w);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = panel_w;
        if (h) *h = win_h;
    }
}

void ui_layout_scene_rect(int *x, int *y, int *w, int *h) {
    int layout = ui_layout_code_panel_layout_mode();
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = win_h;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = ui_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = win_h - panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = ui_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = panel_h;
        if (w) *w = win_w;
        if (h) *h = win_h - panel_h;
    } else {
        int panel_w = ui_layout_panel_span_px(win_w);
        if (x) *x = panel_w;
        if (y) *y = 0;
        if (w) *w = win_w - panel_w;
        if (h) *h = win_h;
    }
}

void ui_layout_menu_bar_rect(int *x, int *y, int *w, int *h) {
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (x) *x = cp_x;
    if (y) *y = cp_y + cp_h - CODE_MARGIN_Y - LINE_H;
    if (w) *w = cp_w;
    if (h) *h = LINE_H;
}

int ui_clamp_panel_y(int scene_y, int scene_h, int panel_h, int requested_y, int layout_mode, int statusbar_h) {
    int min_y = scene_y + statusbar_h + 4;
    int max_y = scene_y + scene_h - panel_h - 4;
    if (max_y >= min_y) {
        if (requested_y < min_y) return min_y;
        if (requested_y > max_y) return max_y;
        return requested_y;
    } else {
        return layout_mode == CODE_PANEL_LAYOUT_TOP ? (scene_y + scene_h - panel_h - 4) : min_y;
    }
}
