/*
 * ui_memory_panel.c - process memory profiling overlay panel.
 */
#include "ui/app/memory_panel.h"
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/app/profile_panel.h"   /* PROFILE_PANEL_W, PROFILE_PANEL_OFF */
#include "ui/app/variable_panel.h"  /* ui_variable_panel_rect_for_count */
#include "ui/core/metrics.h"
#include "ui/core/theme.h"
#include "support/memprof.h"
#include "config.h"                  /* FONT_SMALL, FONT_SMALL_W */

#include <stdio.h>
#include <string.h>

/* Derived total time span the X axis covers, from cadence constants
 * exposed in memprof.h. */
#define MEM_TOTAL_SPAN_S    ((double)MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S)

/* ========================================================================= */
/* Panel geometry                                                             */
/* ========================================================================= */

/* Narrower than the CPU profile panel because the table is one column
 * and the graph uses the tiny font for tick labels. The shortcut is in
 * the Config menu + F1 help, so no inline hint is needed in the header. */
#define MEM_PANEL_W         220
#define MEM_PANEL_MARGIN     12
#define MEM_ROW_H            14
#define MEM_HEADER_H         18
#define MEM_TEXT_BLOCK_H     (3 * MEM_ROW_H + 4)  /* 3 rows + divider */
#define MEM_GRAPH_H          90
#define MEM_Y_GUTTER         44   /* room for "999.9 MB" tiny labels */
#define MEM_BOTTOM_PAD       16   /* X labels + margin */

/* FONT_TINY is variable-width Helvetica 10. The codebase has no
 * glutBitmapLength helper, so width is approximated at 6 px per char
 * for right-alignment. Accurate enough for short numeric labels. */
#define MEM_TINY_W_APPROX    6

static int clamp_int(int v, int lo, int hi) {
    if (hi < lo) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

/* Format a signed delta using memprof's formatter for the magnitude.
 * Either side being zero (the reader-failure sentinel — current/baseline
 * rows render "--" for that case) propagates as "--" so the delta row
 * does not show a fictitious large negative against zero. */
static void fmt_delta(char *buf, int buf_sz,
                      unsigned long long cur, unsigned long long base) {
    if (cur == 0 || base == 0) {
        snprintf(buf, (size_t)buf_sz, "--");
        return;
    }
    if (cur == base) {
        snprintf(buf, (size_t)buf_sz, "  0 B");
        return;
    }
    char tmp[24];
    if (cur > base) {
        memprof_format_bytes(tmp, (int)sizeof(tmp), cur - base);
        snprintf(buf, (size_t)buf_sz, "+%s", tmp);
    } else {
        memprof_format_bytes(tmp, (int)sizeof(tmp), base - cur);
        snprintf(buf, (size_t)buf_sz, "-%s", tmp);
    }
}

/* Round (range / 3) up to {1, 2, 5} * 10^k bytes. Yields ~3-5 ticks. */
static unsigned long long pick_nice_step(unsigned long long range) {
    if (range == 0) return 1024ULL;
    unsigned long long target = range / 3;
    if (target < 1024ULL) target = 1024ULL;
    unsigned long long base = 1024ULL;  /* start at 1 KB */
    while (base * 50ULL < target) {
        if (base > (unsigned long long)-1 / 10ULL) break;
        base *= 10ULL;
    }
    for (;;) {
        if (base       >= target) return base;
        if (base * 2ULL >= target) return base * 2ULL;
        if (base * 5ULL >= target) return base * 5ULL;
        if (base > (unsigned long long)-1 / 10ULL) return base * 5ULL;
        base *= 10ULL;
    }
}

/* "-85m" / "-42m" / "now" for a given offset (seconds before "now"). */
static void fmt_time_offset(char *buf, int buf_sz, double seconds_ago) {
    if (seconds_ago < 1.0) {
        snprintf(buf, (size_t)buf_sz, "now");
    } else if (seconds_ago < 90.0) {
        snprintf(buf, (size_t)buf_sz, "-%.0fs", seconds_ago);
    } else {
        snprintf(buf, (size_t)buf_sz, "-%.0fm", seconds_ago / 60.0);
    }
}

/* Mirror profile_panel_rect_for_height exactly so the panel inherits
 * the CPU profile's anchor logic — including the variable-panel-hidden
 * branch, which routes through ui_variable_panel_rect_for_count() and
 * therefore picks up the replay_lift baked into the variable panel's
 * y when the replay HUD is active. Then apply the side-by-side shift
 * left when the CPU profile panel is visible. */
static void memory_panel_rect_for_height(const UiRenderSnapshot *snap,
                                         int panel_h, int *out_x, int *out_y) {
    int scene_x, scene_y, scene_w, scene_h;
    int panel_x, panel_y;
    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    if (snap->variable_panel.visible) {
        panel_x = scene_x + scene_w - MEM_PANEL_W - MEM_PANEL_MARGIN;
        panel_y = scene_y + scene_h - panel_h    - MEM_PANEL_MARGIN;
    } else {
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect_for_count(snap, snap->variable_panel_vars.count,
                                         &var_x, &var_y, &var_w, &var_h);
        panel_x = var_x + var_w - MEM_PANEL_W;
        panel_y = var_y;
    }

    if (snap->profile_panel.mode != PROFILE_PANEL_OFF) {
        panel_x -= (PROFILE_PANEL_W + 8);
    }

    panel_x = clamp_int(panel_x, scene_x + 4, scene_x + scene_w - MEM_PANEL_W - 4);

    int min_y = scene_y + STATUSBAR_H + 4;
    int max_y = scene_y + scene_h     - panel_h - 4;
    if (max_y >= min_y)
        panel_y = clamp_int(panel_y, min_y, max_y);
    else
        panel_y = min_y;

    if (out_x) *out_x = panel_x;
    if (out_y) *out_y = panel_y;
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

void ui_memory_panel_render(const UiRenderSnapshot *snap) {
    int mode = snap->memory_panel.mode;
    if (mode == MEMORY_PANEL_OFF) return;

    int panel_h = MEM_HEADER_H + MEM_TEXT_BLOCK_H + MEM_GRAPH_H
                + MEM_BOTTOM_PAD + MEM_PANEL_MARGIN;

    int panel_x, panel_y;
    memory_panel_rect_for_height(snap, panel_h, &panel_x, &panel_y);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)MEM_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - MEM_HEADER_H + 2;

    /* No inline hint — the Config menu row carries the Ctrl+Shift+W
     * shortcut and F1 help duplicates it. Header has just the title. */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "Memory Profile", FONT_SMALL);

    ty -= MEM_HEADER_H;

    /* Text block: three single-column rows showing current / init / delta
     * RSS. VSZ deliberately dropped — on macOS it counts the whole virtual
     * address reservation including unmapped pages and renders as GB-scale
     * noise that swamps the graph; RSS is the portable leak signal. */
    MemSample cur  = memprof_current();
    MemSample base = memprof_baseline();

    int col_label = tx;
    int col_value = tx + 64;
    char val_buf[24];

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "RSS",   FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    memprof_format_bytes(val_buf, (int)sizeof(val_buf), cur.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "init",  FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    memprof_format_bytes(val_buf, (int)sizeof(val_buf), base.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "delta", FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    fmt_delta(val_buf, (int)sizeof(val_buf), cur.rss_bytes, base.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    /* Divider before graph */
    ui_clr_a(UI_TOK_DIVIDER, 0.80f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    glVertex2f((float)(panel_x + 4),                (float)(ty + 3));
    glVertex2f((float)(panel_x + MEM_PANEL_W - 4),  (float)(ty + 3));
    glEnd();
    glDisable(GL_BLEND);

    /* Graph area */
    int plot_x = panel_x + MEM_Y_GUTTER + 4;
    int plot_w = MEM_PANEL_W - MEM_Y_GUTTER - 8;
    int plot_y = panel_y + MEM_BOTTOM_PAD;

    /* Auto-fit Y range from RSS history. */
    unsigned long long hi = memprof_history_max_rss();

    if (hi == 0) {
        /* No data yet - draw an empty plot frame and a placeholder. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl2d_panel_frame((float)plot_x, (float)plot_y,
                         (float)plot_w, (float)MEM_GRAPH_H,
                         UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);
        glDisable(GL_BLEND);
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        const char *empty = "(collecting samples)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        gl2d_draw_string((float)(plot_x + (plot_w - ew) / 2),
                         (float)(plot_y + MEM_GRAPH_H / 2),
                         empty, FONT_SMALL);
        gl2d_end();
        return;
    }

    unsigned long long lo = memprof_history_min_rss();
    unsigned long long range = (hi > lo) ? hi - lo : 0;
    if (range < 1024ULL * 1024ULL) range = 1024ULL * 1024ULL;
    unsigned long long pad   = range / 10;
    unsigned long long y_lo  = (lo > pad / 2) ? lo - pad / 2 : 0;
    unsigned long long y_hi  = hi + pad;
    unsigned long long step  = pick_nice_step(y_hi - y_lo);

    /* Snap to step multiples so labels land on round values. */
    if (step > 0) {
        y_lo = (y_lo / step) * step;
        y_hi = ((y_hi + step - 1) / step) * step;
    }
    if (y_hi <= y_lo) y_hi = y_lo + step;

    /* Plot frame + faint horizontal gridlines + Y labels */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)MEM_GRAPH_H,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);

    ui_clr_a(UI_TOK_DIVIDER, 0.30f);
    glBegin(GL_LINES);
    for (unsigned long long v = y_lo; v <= y_hi; v += step) {
        if (y_hi <= y_lo) break;
        double frac = (double)(v - y_lo) / (double)(y_hi - y_lo);
        float gy = (float)(plot_y + frac * MEM_GRAPH_H);
        glVertex2f((float)plot_x,            gy);
        glVertex2f((float)(plot_x + plot_w), gy);
        if (step == 0) break;
    }
    glEnd();
    glDisable(GL_BLEND);

    /* Y labels right-aligned in the gutter. FONT_TINY (Helvetica 10)
     * — proportional width; approximated via MEM_TINY_W_APPROX since
     * the codebase has no glutBitmapLength wrapper. */
    ui_clr(UI_TOK_TEXT_MUTED);
    for (unsigned long long v = y_lo; v <= y_hi; v += step) {
        if (y_hi <= y_lo) break;
        double frac = (double)(v - y_lo) / (double)(y_hi - y_lo);
        int gy = plot_y + (int)(frac * MEM_GRAPH_H);
        char lab[24];
        memprof_format_bytes(lab, (int)sizeof(lab), v);
        int lw = (int)strlen(lab) * MEM_TINY_W_APPROX;
        gl2d_draw_string((float)(panel_x + MEM_Y_GUTTER - 2 - lw),
                         (float)(gy - 5),
                         lab, FONT_TINY);
        if (step == 0) break;
    }

    /* Plot the RSS history. Anchor to the newest sample's t_rel so the
     * rightmost point stays pinned to "now" between pushes. */
    int n = memprof_history_count();
    if (n > 0) {
        double t_latest = memprof_history_latest_t();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ui_clr(UI_TOK_ACCENT);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < n; i++) {
            MemSample s;
            double    t_rel;
            memprof_history_get(i, &s, &t_rel);
            double age = t_latest - t_rel;
            float xx = (float)(plot_x + plot_w - (age / MEM_TOTAL_SPAN_S) * plot_w);
            double frac = (double)(s.rss_bytes - y_lo)
                        / (double)(y_hi - y_lo);
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            float yy = (float)(plot_y + frac * MEM_GRAPH_H);
            glVertex2f(xx, yy);
        }
        glEnd();
        glDisable(GL_BLEND);
    }

    /* X-axis labels: left = -85m, middle = -42m, right = now. */
    ui_clr(UI_TOK_TEXT_MUTED);
    char left_lab[16], mid_lab[16], right_lab[16];
    fmt_time_offset(left_lab,  (int)sizeof(left_lab),  MEM_TOTAL_SPAN_S);
    fmt_time_offset(mid_lab,   (int)sizeof(mid_lab),   MEM_TOTAL_SPAN_S / 2.0);
    fmt_time_offset(right_lab, (int)sizeof(right_lab), 0.0);
    int label_y = plot_y - 12;
    int rl_w = (int)strlen(right_lab) * MEM_TINY_W_APPROX;
    int ml_w = (int)strlen(mid_lab)   * MEM_TINY_W_APPROX;
    gl2d_draw_string((float)plot_x, (float)label_y, left_lab, FONT_TINY);
    gl2d_draw_string((float)(plot_x + plot_w / 2 - ml_w / 2),
                     (float)label_y, mid_lab, FONT_TINY);
    gl2d_draw_string((float)(plot_x + plot_w - rl_w),
                     (float)label_y, right_lab, FONT_TINY);

    gl2d_end();
}
