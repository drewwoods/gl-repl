/*
 * overlay_layout.c - Layout engine for the floating scene-overlay panels.
 *
 * See overlay_layout.h for the model. The solve pass is pure; the eased
 * positions live here as the module's only state, written exclusively by
 * ui_overlay_layout_tick() (controller, once per frame) and
 * ui_overlay_layout_reset().
 */
#include <math.h>
#include <string.h>

#include "ui/app/overlay_layout.h"
#include "ui/app/layout.h"                 /* ui_layout_scene_rect, STATUSBAR_H,
                                            * ui_layout_code_panel_layout_mode */
#include "ui/core/layout_utils.h"          /* ui_clamp_panel_y */
#include "ui/subsystems/variable_panel.h"  /* ui_variable_panel_size */
#include "ui/support/assign_plot.h"        /* ui_assign_plot_panel_size */
#include "ui/support/cpuprof.h"            /* ui_profile_panel_width/height */
#include "ui/support/memprof.h"            /* ui_memory_panel_width/height */

/* Stack geometry. BASE_Y / RIGHT_MARGIN / EDGE_PAD preserve the variable
 * panel's established bottom-right placement when no easing tick runs. */
enum {
    OVL_BASE_Y       = 8,  /* gap between the inset (or band) and the stack */
    OVL_RIGHT_MARGIN = 8,  /* gap from the scene's right edge */
    OVL_STACK_GAP    = 8,  /* vertical gap between stacked panels */
    OVL_COL_GAP      = 8,  /* horizontal gap between spill columns */
    OVL_EDGE_PAD     = 4,  /* min inset from scene edges after clamping */
};

/* Same glide feel as the variable-panel replay lift. */
#define OVL_EASE     0.22f
#define OVL_SNAP_PX  0.25f

typedef struct {
    int   valid[UI_OVERLAY_PANEL_COUNT];  /* eased position is live */
    float x[UI_OVERLAY_PANEL_COUNT];
    float y[UI_OVERLAY_PANEL_COUNT];
    int   band_h;                         /* band from the last tick */
} OverlayLayoutState;

static OverlayLayoutState g_ovl;  /* zero-init: never ticked, band 0 */

UiOverlayLayoutIn ui_overlay_layout_inputs(UiOverlayLayoutInputs args) {
    UiOverlayLayoutIn in;
    ui_layout_scene_rect(&in.render3d_x, &in.render3d_y, &in.render3d_w, &in.render3d_h);
    in.bottom_inset = STATUSBAR_H;
    in.band_h       = args.band_h;
    in.prefer_top_on_overflow =
        ui_layout_code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP;

    in.panels[UI_OVERLAY_PANEL_VARIABLE].visible = (args.var_visible != 0);
    ui_variable_panel_size(args.var_count, args.var_collapsed,
                           &in.panels[UI_OVERLAY_PANEL_VARIABLE].w,
                           &in.panels[UI_OVERLAY_PANEL_VARIABLE].h);

    /* The FPS plot rides every non-OFF Compute-profile level; the full
     * collapsible section listing joins from SECTIONS up. */
    in.panels[UI_OVERLAY_PANEL_FPS].visible =
        (args.profile_mode != PROFILE_PANEL_OFF);
    in.panels[UI_OVERLAY_PANEL_FPS].w = ui_fps_panel_width();
    in.panels[UI_OVERLAY_PANEL_FPS].h = ui_fps_panel_height();

    in.panels[UI_OVERLAY_PANEL_PROFILE].visible =
        (args.profile_mode == PROFILE_PANEL_SECTIONS ||
         args.profile_mode == PROFILE_PANEL_HISTOGRAM);
    in.panels[UI_OVERLAY_PANEL_PROFILE].w = ui_profile_panel_width();
    in.panels[UI_OVERLAY_PANEL_PROFILE].h =
        ui_profile_panel_height_collapsed(
            (UiProfilePanelMode)args.profile_mode,
            args.profile_collapsed_sections);

    /* Histogram is the final, deliberately expensive profile surface. */
    in.panels[UI_OVERLAY_PANEL_HISTOGRAM].visible =
        (args.profile_mode == PROFILE_PANEL_HISTOGRAM);
    in.panels[UI_OVERLAY_PANEL_HISTOGRAM].w = ui_histogram_panel_width();
    in.panels[UI_OVERLAY_PANEL_HISTOGRAM].h = ui_histogram_panel_height();

    in.panels[UI_OVERLAY_PANEL_MEMORY].visible =
        (args.memory_mode != MEMORY_PANEL_OFF);
    in.panels[UI_OVERLAY_PANEL_MEMORY].w = ui_memory_panel_width();
    in.panels[UI_OVERLAY_PANEL_MEMORY].h = ui_memory_panel_height();

    /* Not a profile surface: this one appears when the user right-clicks an
     * assignment row and disappears when they close it. */
    in.panels[UI_OVERLAY_PANEL_ASSIGN_PLOT].visible =
        (args.assign_plot_visible != 0);
    ui_assign_plot_panel_size(args.assign_plot_expanded,
                              args.assign_plot_series_count,
                              &in.panels[UI_OVERLAY_PANEL_ASSIGN_PLOT].w,
                              &in.panels[UI_OVERLAY_PANEL_ASSIGN_PLOT].h);
    return in;
}

int ui_overlay_layout_last_band_h(void) {
    return g_ovl.band_h;
}

void ui_overlay_layout_solve(const UiOverlayLayoutIn *in,
                             int out_x[UI_OVERLAY_PANEL_COUNT],
                             int out_y[UI_OVERLAY_PANEL_COUNT]) {
    int order[UI_OVERLAY_PANEL_COUNT];
    int order_count = 0;
    int base = (in->band_h > OVL_BASE_Y) ? in->band_h : OVL_BASE_Y;
    int y0 = in->render3d_y + in->bottom_inset + base;
    int top_limit = in->render3d_y + in->render3d_h - OVL_EDGE_PAD;
    int col_right = in->render3d_x + in->render3d_w - OVL_RIGHT_MARGIN;
    int col_left  = col_right;
    int y = y0;

    order[order_count++] = UI_OVERLAY_PANEL_VARIABLE;
    /* Directly above the variable panel: the assignment plot is read against
     * the code, so it stays in the low, always-visible part of the stack
     * rather than being pushed into a spill column by the profile surfaces. */
    order[order_count++] = UI_OVERLAY_PANEL_ASSIGN_PLOT;
    order[order_count++] = UI_OVERLAY_PANEL_FPS;
    /* Memory and FPS are one right-edge telemetry stack. Keep them together
     * before the larger compute panels spill into columns to the left. */
    if (in->panels[UI_OVERLAY_PANEL_FPS].visible &&
        in->panels[UI_OVERLAY_PANEL_MEMORY].visible)
        order[order_count++] = UI_OVERLAY_PANEL_MEMORY;
    order[order_count++] = UI_OVERLAY_PANEL_PROFILE;
    order[order_count++] = UI_OVERLAY_PANEL_HISTOGRAM;
    if (!(in->panels[UI_OVERLAY_PANEL_FPS].visible &&
          in->panels[UI_OVERLAY_PANEL_MEMORY].visible))
        order[order_count++] = UI_OVERLAY_PANEL_MEMORY;

    for (int order_idx = 0; order_idx < order_count; order_idx++) {
        int id = order[order_idx];
        const UiOverlayPanelReq *p = &in->panels[id];

        /* Stack ran out of vertical room: spill into a fresh column left of
         * everything placed so far. A panel at the column base never spills
         * (it can only clamp). */
        if (p->visible && y > y0 && y + p->h > top_limit) {
            col_right = col_left - OVL_COL_GAP;
            y = y0;
        }

        int x = col_right - p->w;
        if (x < in->render3d_x + OVL_EDGE_PAD) x = in->render3d_x + OVL_EDGE_PAD;
        int yc = ui_clamp_panel_y(in->render3d_y, in->render3d_h, p->h, y,
                                  in->prefer_top_on_overflow,
                                  in->bottom_inset, OVL_EDGE_PAD);
        out_x[id] = x;
        out_y[id] = yc;

        if (!p->visible) continue;  /* placed (for snap-in) but takes no slot */
        if (x < col_left) col_left = x;
        y = yc + p->h + OVL_STACK_GAP;
    }
}

/* One eased axis step toward the target. */
static float ovl_ease_toward(float cur, float target) {
    cur += (target - cur) * OVL_EASE;
    if (fabsf(target - cur) < OVL_SNAP_PX) cur = target;
    return cur;
}

void ui_overlay_layout_tick(const UiOverlayLayoutIn *in) {
    int tx[UI_OVERLAY_PANEL_COUNT], ty[UI_OVERLAY_PANEL_COUNT];
    ui_overlay_layout_solve(in, tx, ty);
    g_ovl.band_h = in->band_h;
    for (int id = 0; id < UI_OVERLAY_PANEL_COUNT; id++) {
        if (!in->panels[id].visible) {
            g_ovl.valid[id] = 0;  /* re-snap on reappearing */
            continue;
        }
        if (!g_ovl.valid[id]) {
            g_ovl.x[id] = (float)tx[id];
            g_ovl.y[id] = (float)ty[id];
            g_ovl.valid[id] = 1;
            continue;
        }
        g_ovl.x[id] = ovl_ease_toward(g_ovl.x[id], (float)tx[id]);
        g_ovl.y[id] = ovl_ease_toward(g_ovl.y[id], (float)ty[id]);
    }
}

void ui_overlay_layout_panel_pos(const UiOverlayLayoutIn *in,
                                 UiOverlayPanelId id, int *x, int *y) {
    int tx[UI_OVERLAY_PANEL_COUNT], ty[UI_OVERLAY_PANEL_COUNT];
    ui_overlay_layout_solve(in, tx, ty);
    if (g_ovl.valid[id]) {
        if (x) *x = (int)lroundf(g_ovl.x[id]);
        if (y) *y = (int)lroundf(g_ovl.y[id]);
    } else {
        if (x) *x = tx[id];
        if (y) *y = ty[id];
    }
}

void ui_overlay_layout_reset(void) {
    memset(&g_ovl, 0, sizeof(g_ovl));
}

void ui_overlay_layout_capture(UiOverlayLayoutSnapshot *out) {
    if (!out)
        return;
    for (int i = 0; i < UI_OVERLAY_PANEL_COUNT; i++) {
        out->valid[i] = g_ovl.valid[i];
        out->x[i]     = g_ovl.x[i];
        out->y[i]     = g_ovl.y[i];
    }
    out->band_h = g_ovl.band_h;
}

void ui_overlay_layout_restore(const UiOverlayLayoutSnapshot *snap) {
    if (!snap)
        return;
    for (int i = 0; i < UI_OVERLAY_PANEL_COUNT; i++) {
        g_ovl.valid[i] = snap->valid[i];
        g_ovl.x[i]     = snap->x[i];
        g_ovl.y[i]     = snap->y[i];
    }
    g_ovl.band_h = snap->band_h;
}
