/*
 * repl_layout.c - pure window layout geometry.
 */
#include "sample.h"
#include "repl_layout.h"
#include "repl_state.h"

/* ui_state_viewport / ui_state_code_panel are forward-declared here
 * because repl_*.c is not allowed to include ui_state.h per
 * check-controller-boundaries. Layout reads viewport size and panel
 * fraction; both slices are UiState chrome. */
ReplViewportState         ui_state_viewport(void);
ReplCodePanelRuntimeState ui_state_code_panel(void);

static int repl_layout_code_panel_layout_mode(void) {
    if (repl_state_presentation().code_panel_layout < 0 ||
        repl_state_presentation().code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return repl_state_presentation().code_panel_layout;
}

static int repl_layout_panel_span_px(int total_px) {
    int span = (int)((float)total_px * ui_state_code_panel().panel_frac);
    if (span < 1) span = 1;
    if (span > total_px) span = total_px;
    return span;
}

void repl_layout_code_panel_rect(int *x, int *y, int *w, int *h) {
    int layout = repl_layout_code_panel_layout_mode();
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = repl_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = win_h - panel_h;
        if (w) *w = win_w;
        if (h) *h = panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = repl_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = panel_h;
    } else {
        int panel_w = repl_layout_panel_span_px(win_w);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = panel_w;
        if (h) *h = win_h;
    }
}

void repl_layout_scene_rect(int *x, int *y, int *w, int *h) {
    int layout = repl_layout_code_panel_layout_mode();
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = win_h;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = repl_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = win_w;
        if (h) *h = win_h - panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = repl_layout_panel_span_px(win_h);
        if (x) *x = 0;
        if (y) *y = panel_h;
        if (w) *w = win_w;
        if (h) *h = win_h - panel_h;
    } else {
        int panel_w = repl_layout_panel_span_px(win_w);
        if (x) *x = panel_w;
        if (y) *y = 0;
        if (w) *w = win_w - panel_w;
        if (h) *h = win_h;
    }
}
