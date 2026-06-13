/*
 * ui_cpuprof.c - per-section profiling overlay panel.
 *
 * Three time columns per section, all EMAs: CPU (wall time from
 * support/cpuprof), GPU (timer-query elapsed time from support/gpuprof;
 * "--" for sections that are CPU-only or when GPU timing is unavailable),
 * and Max (the worse of the two — the binding cost for the section).
 */
#include "ui/support/cpuprof.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"
#include "support/cpuprof.h"
#include "support/gpuprof.h"

#include <stdio.h>
#include <string.h>

/* Dim/stale text tiers: deliberately darker than TEXT_MUTED so stale
 * rows recede - no clean token twin, kept as theme-stable named consts
 * (theme.h "named constant" bucket). The FPS gauge in set_time_color
 * is the separate documented data-viz exclusion (red must stay red). */
static const float k_prof_stale[3] = { 0.35f, 0.35f, 0.42f };
static const float k_prof_dim[3]   = { 0.30f, 0.30f, 0.38f };

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* Panel geometry (pixels). PROFILE_PANEL_W lives in cpuprof.h so
 * sibling panels (ui_memory_panel) can shift left for side-by-side
 * layout; the local PROF_PANEL_W alias preserves the historical name. */
#define PROF_PANEL_W        PROFILE_PANEL_W
#define PROF_PANEL_MARGIN    12
#define PROF_ROW_H           16
#define PROF_HEADER_H        20
#define PROF_BOTTOM_PAD      14
#define PROF_COL_LABEL_W    150
#define PROF_COL_VAL_W       72

/* Visual indentation per nesting level. Labels arrive un-indented; the panel
 * offsets each row by depth * this, so prof_section_info()'s explicit depth is
 * the single source of truth (restyling the indent never re-classifies a row).
 * FONT_SMALL is fixed-width, so 2 cell widths reads as the prior 2-space step. */
#define PROF_INDENT_W       (FONT_SMALL_W * 2)

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

/* Format µs as "1234 us" or "12.3 ms", whichever is more readable. */
static void fmt_us(char *buf, int buf_sz, double us) {
    if (us < 1000.0)
        snprintf(buf, (size_t)buf_sz, "%.0f us", us);
    else
        snprintf(buf, (size_t)buf_sz, "%.2f ms", us / 1000.0);
}

/* Apply a green/yellow/red color based on section timing thresholds.
 * The whole-frame total row (is_total) uses 1/120s (8.3ms) and 1/60s
 * (16.7ms) breakpoints; every other section uses half those thresholds
 * (4.15ms / 8.3ms). */
/* FPS gauge: green/yellow/red is a fixed data-viz semantic, NOT theme
 * tokens (theme.h bucket 3 - red must read as "over budget" in every
 * scheme; it must not follow the UI accent). */
static void set_time_color(int is_total, double us) {
    if (us < 2.0) {
        glColor3f(0.30f, 0.30f, 0.38f);       /* near-zero – same as stale */
        return;
    }
    if (is_total) {
        if (us < 8333.0)
            glColor3f(0.50f, 0.88f, 0.45f);   /* green  – fits in 120 fps */
        else if (us < 16667.0)
            glColor3f(0.95f, 0.82f, 0.25f);   /* yellow – fits in 60 fps */
        else
            glColor3f(0.95f, 0.38f, 0.32f);   /* red    – below 60 fps */
    } else {
        if (us < 4167.0)
            glColor3f(0.50f, 0.88f, 0.45f);   /* green  – half-budget OK */
        else if (us < 8333.0)
            glColor3f(0.95f, 0.82f, 0.25f);   /* yellow – half-budget tight */
        else
            glColor3f(0.95f, 0.38f, 0.32f);   /* red    – over half-budget */
    }
}

/* A row's visibility is the app's call, expressed through prof_section_info():
 *  - a section with no label (NULL/empty) is not a row at all — it's a catalog
 *    slot this binary doesn't use (a demo instrumenting a subset returns this
 *    for the rest), so it's omitted in every mode;
 *  - a "detail" row is a nested child (depth > 0), hidden outside DETAILS mode.
 * The app supplies label/depth, so a new sub-section needs no edit here. */
static int section_visible(int profile_mode, ProfSection s) {
    ProfSectionInfo info = prof_section_info(s);
    if (info.label == NULL || info.label[0] == '\0')
        return 0;
    if (profile_mode != PROFILE_PANEL_DETAILS && info.depth > 0)
        return 0;
    return 1;
}

static int visible_section_count(int profile_mode) {
    int section_count = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (section_visible(profile_mode, (ProfSection)section_idx))
            section_count++;
    }
    return section_count;
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

/* Panel total height for a given mode: header + column headings + one row per
 * visible section + divider before FRAME_TOTAL + bottom padding. The
 * controller needs this (via ui_profile_panel_height) to resolve the stacked
 * anchor; the renderer uses it to size the frame. */
int ui_profile_panel_height(UiProfilePanelMode mode) {
    int visible_count = visible_section_count((int)mode);
    return PROF_HEADER_H
         + 18                           /* column heading row */
         + visible_count * PROF_ROW_H
         + 4                            /* divider before FRAME_TOTAL */
         + PROF_BOTTOM_PAD
         + PROF_PANEL_MARGIN;
}

int ui_profile_panel_width(void) {
    return PROF_PANEL_W;
}

void ui_profile_panel_render(const UiProfilePanelView *view) {
    int profile_mode = view->mode;
    /* The section listing only exists from SECTIONS up; PLOT shows just the
     * separate FPS plot panel (ui_fps_panel_render). */
    if (profile_mode != PROFILE_PANEL_SECTIONS &&
        profile_mode != PROFILE_PANEL_DETAILS)
        return;

    int panel_h = ui_profile_panel_height((UiProfilePanelMode)profile_mode);

    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    /* Background + Border */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)PROF_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - PROF_HEADER_H + 2;
    const char *HINT = "Ctrl+W:hide";
    const int hint_width = FONT_SMALL_W * (int)strlen(HINT) + 2;

    /* Title */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "Compute Profile", FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + PROF_PANEL_W - hint_width), (float)ty, HINT, FONT_SMALL);

    ty -= PROF_HEADER_H;

    /* Column headings */
    int col_cpu = tx + PROF_COL_LABEL_W;
    int col_gpu = col_cpu + PROF_COL_VAL_W;
    int col_max = col_gpu + PROF_COL_VAL_W;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)tx,       (float)ty, "Section",  FONT_SMALL);
    gl2d_draw_string((float)col_cpu,  (float)ty, "CPU",      FONT_SMALL);
    gl2d_draw_string((float)col_gpu,  (float)ty, "GPU",      FONT_SMALL);
    gl2d_draw_string((float)col_max,  (float)ty, "Max",      FONT_SMALL);
    ty -= 2;

    /* Thin rule under headings */
    ui_clr_a(UI_TOK_DIVIDER, 0.80f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    glVertex2f((float)(panel_x + 4),              (float)ty);
    glVertex2f((float)(panel_x + PROF_PANEL_W - 4), (float)ty);
    glEnd();
    glDisable(GL_BLEND);

    ty -= PROF_ROW_H - 2;

    /* One row per section.  Insert a separator line before FRAME_TOTAL. */
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!section_visible(profile_mode, s)) continue;

        ProfSectionInfo info = prof_section_info(s);

        if (info.is_total) {
            /* Divider above totals row */
            ui_clr_a(UI_TOK_DIVIDER, 0.80f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBegin(GL_LINES);
            glVertex2f((float)(panel_x + 4),               (float)(ty + PROF_ROW_H - 3));
            glVertex2f((float)(panel_x + PROF_PANEL_W - 4),(float)(ty + PROF_ROW_H - 3));
            glEnd();
            glDisable(GL_BLEND);
        }

        int cpu_ok = !prof_section_is_stale(s);
        int gpu_ok = gpu_prof_section_has_data(s);

        /* Label */
        if (info.is_total)
            ui_clr(UI_TOK_TEXT_PRIMARY);
        else if (!cpu_ok && !gpu_ok)
            glColor3fv(k_prof_stale);
        else if (info.depth > 0)
            ui_clr(UI_TOK_TEXT_SECTION);
        else
            ui_clr(UI_TOK_TEXT_PRIMARY);
        /* Indentation is derived from depth, not baked into the label. */
        gl2d_draw_string((float)(tx + info.depth * PROF_INDENT_W),
                         (float)ty, info.label, FONT_SMALL);

        /* CPU / GPU / Max values — all smoothed averages. */
        double cpu_us = prof_section_avg_us(s);
        double gpu_us = gpu_prof_section_avg_us(s);
        char val_buf[24];

        if (cpu_ok) {
            fmt_us(val_buf, (int)sizeof(val_buf), cpu_us);
            set_time_color(info.is_total, cpu_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_cpu, (float)ty, val_buf, FONT_SMALL);

        if (gpu_ok) {
            fmt_us(val_buf, (int)sizeof(val_buf), gpu_us);
            set_time_color(info.is_total, gpu_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_gpu, (float)ty, val_buf, FONT_SMALL);

        /* NOTE: Max takes the worse of the two independently-smoothed EMAs
         * (and the GPU EMA runs 1-3 frames behind the CPU one, since query
         * results are harvested asynchronously), not the average of paired
         * per-frame worst cases — pairing each CPU sample with its GPU twin
         * would mean holding samples back until the query resolves, for a
         * distinction that only shows when CPU and GPU spikes anti-correlate
         * frame-to-frame. Not strictly correct, close enough for a HUD. */
        if (cpu_ok || gpu_ok) {
            double max_us = cpu_ok ? cpu_us : 0.0;
            if (gpu_ok && gpu_us > max_us) max_us = gpu_us;
            fmt_us(val_buf, (int)sizeof(val_buf), max_us);
            set_time_color(info.is_total, max_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_max, (float)ty, val_buf, FONT_SMALL);

        ty -= PROF_ROW_H;
    }

    gl2d_end();
}

/* ========================================================================= */
/* FPS plot panel                                                             */
/* ========================================================================= */

#define FPS_PANEL_W       300
#define FPS_HEADER_H      20
#define FPS_LEGEND_H      14
#define FPS_PLOT_H        90
#define FPS_PLOT_GUTTER   28   /* y-axis label gutter ("240") */
#define FPS_BOTTOM_PAD    10

/* Series identity colors: fixed data-viz semantics (which time window a
 * line is), not theme tokens — they must stay distinct in every scheme. */
static const float k_fps_col_10s[3] = { 0.55f, 0.95f, 0.55f };  /* green */
static const float k_fps_col_1m[3]  = { 0.95f, 0.85f, 0.35f };  /* amber */
static const float k_fps_col_10m[3] = { 0.45f, 0.75f, 1.00f };  /* blue  */

int ui_fps_panel_width(void) {
    return FPS_PANEL_W;
}

int ui_fps_panel_height(void) {
    return FPS_HEADER_H + FPS_LEGEND_H + FPS_PLOT_H + FPS_BOTTOM_PAD + 8;
}

/* One series as a line strip. The newest bucket pins to the right edge and
 * each ring slot is one x step, so a partially-filled window draws only its
 * recent right-hand stretch and grows leftward as history accrues. */
static void fps_draw_series(int window, int plot_x, int plot_y,
                            int plot_w, int plot_h, float y_hi,
                            const float col[3]) {
    float samples[PROF_FPS_HISTORY_CAP];
    int n = prof_fps_history(window, samples, PROF_FPS_HISTORY_CAP);
    if (n < 2 || y_hi <= 0.0f) return;

    glColor4f(col[0], col[1], col[2], 0.95f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < n; i++) {
        float fx = (float)plot_x + (float)plot_w
                 * (float)(PROF_FPS_HISTORY_CAP - n + i)
                 / (float)(PROF_FPS_HISTORY_CAP - 1);
        float v = samples[i];
        if (v < 0.0f) v = 0.0f;
        if (v > y_hi) v = y_hi;
        glVertex2f(fx, (float)plot_y + (v / y_hi) * (float)plot_h);
    }
    glEnd();
}

void ui_fps_panel_render(const UiFpsPanelView *view) {
    if (!view || !view->visible) return;

    int panel_h = ui_fps_panel_height();
    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)FPS_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - FPS_HEADER_H + 2;

    /* Title + smoothed current FPS readout. */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "FPS", FONT_SMALL);
    {
        char now_buf[24];
        snprintf(now_buf, sizeof(now_buf), "%.1f now", prof_fps_current());
        int nw = (int)strlen(now_buf) * FONT_SMALL_W + 2;
        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)(panel_x + FPS_PANEL_W - 8 - nw), (float)ty,
                         now_buf, FONT_SMALL);
    }
    ty -= FPS_LEGEND_H;

    /* Legend: one colored label per window. */
    {
        int lx = tx;
        glColor3fv(k_fps_col_10s);
        gl2d_draw_string((float)lx, (float)ty, "10s", FONT_SMALL);
        lx += 4 * FONT_SMALL_W + 8;
        glColor3fv(k_fps_col_1m);
        gl2d_draw_string((float)lx, (float)ty, "1m", FONT_SMALL);
        lx += 3 * FONT_SMALL_W + 8;
        glColor3fv(k_fps_col_10m);
        gl2d_draw_string((float)lx, (float)ty, "10m", FONT_SMALL);
    }
    ty -= FPS_LEGEND_H;

    /* Plot rect (gutter on the left for y labels). */
    int plot_x = panel_x + FPS_PLOT_GUTTER + 4;
    int plot_w = FPS_PANEL_W - FPS_PLOT_GUTTER - 12;
    int plot_y = panel_y + FPS_BOTTOM_PAD;
    int plot_h = ty - plot_y - 2;
    if (plot_h < 20) plot_h = 20;

    /* Y range: 0 .. a nice ceiling over the worst series sample (at least
     * 60 so an idle plot still reads in familiar FPS terms). */
    float hi = 60.0f;
    float cur = (float)prof_fps_current();
    if (cur > hi) hi = cur;
    int have_data = 0;
    for (int w = 0; w < PROF_FPS_WIN_COUNT; w++) {
        float samples[PROF_FPS_HISTORY_CAP];
        int n = prof_fps_history(w, samples, PROF_FPS_HISTORY_CAP);
        if (n >= 2) have_data = 1;
        for (int i = 0; i < n; i++)
            if (samples[i] > hi) hi = samples[i];
    }
    static const float k_steps[] = { 15.0f, 30.0f, 60.0f, 120.0f, 240.0f, 480.0f };
    float step = k_steps[0];
    for (size_t si = 0; si < sizeof(k_steps) / sizeof(k_steps[0]); si++) {
        step = k_steps[si];
        if (hi * 1.05f / step <= 4.0f) break;
    }
    float y_hi = step;
    while (y_hi < hi * 1.05f) y_hi += step;

    /* Plot frame + gridlines + y labels. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)plot_h,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);
    ui_clr_a(UI_TOK_DIVIDER, 0.30f);
    glBegin(GL_LINES);
    for (float v = step; v < y_hi + step * 0.5f; v += step) {
        float gy = (float)plot_y + (v / y_hi) * (float)plot_h;
        glVertex2f((float)plot_x,            gy);
        glVertex2f((float)(plot_x + plot_w), gy);
    }
    glEnd();

    if (!have_data) {
        glDisable(GL_BLEND);
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        const char *empty = "(collecting)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        gl2d_draw_string((float)plot_x + ((float)plot_w - (float)ew) * 0.5f,
                         (float)plot_y + (float)plot_h * 0.5f,
                         empty, FONT_SMALL);
        gl2d_end();
        return;
    }

    /* Series, slowest window first so the fast 10s line draws on top. */
    fps_draw_series(PROF_FPS_WIN_10M, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_10m);
    fps_draw_series(PROF_FPS_WIN_1M, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_1m);
    fps_draw_series(PROF_FPS_WIN_10S, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_10s);
    glDisable(GL_BLEND);

    /* Y labels right-aligned in the gutter. */
    ui_clr(UI_TOK_TEXT_MUTED);
    for (float v = 0.0f; v < y_hi + step * 0.5f; v += step) {
        char lab[12];
        snprintf(lab, sizeof(lab), "%.0f", (double)v);
        int lw = (int)strlen(lab) * FONT_SMALL_W;
        int gy = plot_y + (int)((v / y_hi) * (float)plot_h);
        gl2d_draw_string((float)(panel_x + FPS_PLOT_GUTTER + 2 - lw),
                         (float)(gy - 4), lab, FONT_SMALL);
    }

    gl2d_end();
}