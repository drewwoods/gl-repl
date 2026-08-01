/*
 * ui_variable_panel.c -- Floating slider panel for declared variables.
 *
 * Pure renderer + hit-test over a narrow UiVariablePanelView (the controller
 * projects the per-frame snapshot down to it via ui_app_variable_panel_view).
 * No ui/app, no live REPL/editor state: this TU links from {ui/core, config}
 * alone so it can stand up in the standalone variable_panel_demo. The actual
 * value mutation lives in variable_panel_drag.c; the editor's mouse handler
 * invokes the peer via variable_panel_handle_drag_*.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "ui/subsystems/variable_panel.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"
#include "config.h"                  /* FONT_SMALL, FONT_SMALL_W */

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

/* Variable-row data presentation + drag-state indicators. Deliberately
 * NOT theme tokens (theme.h "named constant" bucket): the green-name /
 * yellow-value two-tone, dim written-variable variants, and the
 * log/linear/idle handle states encode meaning and must stay fixed across
 * every UI scheme. */
static const float k_var_name[3]           = { 0.70f, 0.85f, 0.70f };
static const float k_var_value[3]          = { 0.90f, 0.90f, 0.60f };
static const float k_var_name_written[3]   = { 0.38f, 0.48f, 0.38f };
static const float k_var_value_written[3]  = { 0.48f, 0.48f, 0.34f };
static const float k_var_drag_coarse_bg[4] = { 0.30f, 0.20f, 0.05f, 0.60f };
static const float k_var_drag_linear_bg[4] = { 0.20f, 0.20f, 0.40f, 0.60f };
static const float k_var_handle_coarse[4]  = { 1.00f, 0.55f, 0.10f, 0.95f };
static const float k_var_handle_linear[4]  = { 1.00f, 0.80f, 0.20f, 0.95f };
static const float k_var_handle_idle[4]    = { 0.55f, 0.70f, 1.00f, 0.90f };
static const float k_var_handle_written[4] = { 0.34f, 0.42f, 0.60f, 0.70f };

static int clamp_var_count(int count) {
    if (count < 0) return 0;
    if (count > UI_VARIABLE_PANEL_MAX_ROWS) return UI_VARIABLE_PANEL_MAX_ROWS;
    return count;
}

static float ui_variable_value(const UiVariable *var) {
    return (var && var->value) ? *var->value : 0.0f;
}

/* Compute a shared logarithmic display scale from all variable absolute values.
 * All sliders use the same scale so their handles are normalized relative to
 * each other (a var at 100 shows near the extreme, one at 0.01 still visible). */
static float var_panel_log_scale(const UiVariable *vars, int count) {
    float max_abs = 0.1f;   /* minimum display range */
    for (int i = 0; i < count; i++) {
        float av = fabsf(ui_variable_value(&vars[i]));
        if (av > max_abs) max_abs = av;
    }
    return max_abs * 1.25f; /* 25% headroom so handle doesn't hug the edge */
}

static const char *truncate_var_name(const char *name, size_t max_len) {
    static char buf[64];
    /* Ensure max_len fits in buf with room for null terminator. */
    if (max_len >= sizeof(buf)) max_len = sizeof(buf) - 1;

    /* truncate name with ellipsis in the middle if necessary */
    size_t n = strlen(name);
    if (n > max_len) {
        /* Total length including "..." should be exactly max_len. */
        if (max_len < 4) {
            /* degenerate case: not even room for x...x */
            snprintf(buf, sizeof(buf), "...");
        } else {
            size_t avail = max_len - 3;
            size_t prefix = avail / 2;
            size_t suffix = avail - prefix;
            snprintf(buf, sizeof(buf), "%.*s...%.*s", (int)prefix, name, (int)suffix, name + n - suffix);
        }
    } else {
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }
    return buf;
}

/* Map a value to slider t in [0,1] using a symmetric asinh (log-like) scale.
 * The "knee" epsilon = 5% of scale is the linear region; beyond that the
 * response is logarithmic.  Zero always maps to 0.5 (centre). */
static float val_to_slider_t(float val, float scale) {
    float eps  = scale * 0.05f;               /* linear knee */
    float norm = asinhf(scale / eps);          /* ≈ asinh(20) ≈ 4.0 – fixed for scale */
    float t    = 0.5f + 0.5f * asinhf(val / eps) / norm;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

#define VAR_PANEL_W   250
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20
/* Layout constants for variable rows */
#define VAR_NAME_COL_WIDTH_DENOM 3   /* name column gets 1/N of panel width */
#define VAR_NAME_COL_PAD_CHARS 1     /* extra char-widths of padding for name column */
#define VAR_VALUE_FMT_WIDTH 7        /* format field width for value display */
#define VAR_VALUE_FMT_PREC 3         /* decimal precision for value display */
#define VAR_VALUE_COL_GAP 10         /* gap (px) between value text and track start */
#define VAR_HANDLE_W 10              /* width of slider handle */
#define VAR_VALUE_COL_PIXELS (VAR_VALUE_FMT_WIDTH*FONT_SMALL_W + VAR_VALUE_COL_GAP) /* pixels reserved for value column */
#define VAR_TRACK_PAD_RIGHT 8        /* right padding from track edge to panel edge */
#define VAR_TRACK_MIN_SLACK 4        /* minimum track width past the handle */
#define VAR_TRACK_PAD_TOP 6          /* top padding of track from row bounds */
#define VAR_TRACK_PAD_BOTTOM 6       /* bottom padding of track from row bounds */
#define VAR_TICK_PAD_TOP 5           /* top padding of center tick */
#define VAR_TICK_PAD_BOTTOM 5        /* bottom padding of center tick */
#define VAR_HANDLE_PAD_TOP 4         /* top padding of handle from row bounds */
#define VAR_HANDLE_PAD_BOTTOM 4      /* bottom padding of handle from row bounds */
#define VAR_NAME_MAX_PIXELS ((VAR_PANEL_W) / (VAR_NAME_COL_WIDTH_DENOM))
#define VAR_NAME_MAX_CHARS  ((VAR_NAME_MAX_PIXELS) / (FONT_SMALL_W))
/* Collapse chip: right-aligned "[-]"/"[+]" in the title bar, mirroring the
 * gl-state popup's details chip and the tour HUD's collapse affordance. */
#define VAR_COLLAPSE_CHIP_CHARS 3
/* Title text sits this far below the panel's padded top edge — it is the
 * *baseline* offset, since gl2d_draw_string rasters from the baseline. */
#define VAR_TITLE_BASELINE_PAD 4
/* Baseline-relative glyph box of FONT_SMALL (8x13): descent below, the rest
 * above. The chip hit cell is derived from this, not from VAR_TITLE_H — the
 * nominal title row extends well below the drawn glyphs, and sizing the cell
 * to the row put the clickable area under the visible "[-]". */
#define VAR_FONT_SMALL_DESCENT 2
/* Clickable slop around the chip's glyph box. */
#define VAR_COLLAPSE_CHIP_SLOP 3

/* Panel size for a row count (clamped) and collapse state. Pure; no view
 * needed. Collapsed drops the row area entirely, leaving just the title. */
void ui_variable_panel_size(int var_count, int collapsed, int *pw, int *ph) {
    int count = collapsed ? 0 : clamp_var_count(var_count);
    if (pw) *pw = VAR_PANEL_W;
    if (ph) *ph = VAR_TITLE_H + count * VAR_ROW_H + 2 * VAR_PANEL_PAD;
}

/* Geometry in render coords (y=0 at bottom). Position is the view's
 * resolved panel_x/panel_y (overlay layout engine / standalone driver). */
void ui_variable_panel_rect(const UiVariablePanelView *view,
                            int *px, int *py, int *pw, int *ph) {
    if (px) *px = view->panel_x;
    if (py) *py = view->panel_y;
    ui_variable_panel_size(view->var_count, view->collapsed, pw, ph);
}

/* Return 1 if window coord (gx, gy) is in the panel; sets *out_row.
 * Collapsed panels have no rows to hit. */
int ui_variable_panel_hit_row(const UiVariablePanelView *view,
                              int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    int count = view->collapsed ? 0 : clamp_var_count(view->var_count);
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);
    int ry = view->window_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= count) return 0;
    if (out_row) *out_row = row;
    return 1;
}

/* Title-text baseline (window coords, y-up) for a resolved panel rect.
 * Shared by the title string, the chip string, and the chip hit cell. */
static int var_panel_title_baseline(int py, int ph) {
    return py + ph - VAR_PANEL_PAD - VAR_TITLE_BASELINE_PAD;
}

/* Collapse/expand chip cell (window coords, y-up), right-aligned on the
 * title row and vertically bounded by the chip's glyph box plus slop.
 * Shared by the hit-test and the renderer so they always agree. */
static void var_panel_collapse_chip_rect(const UiVariablePanelView *view,
                                         int *x0, int *x1, int *y0, int *y1) {
    int px, py, pw, ph, baseline;
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);
    baseline = var_panel_title_baseline(py, ph);
    *x1 = px + pw - VAR_PANEL_PAD;
    *x0 = *x1 - VAR_COLLAPSE_CHIP_CHARS * FONT_SMALL_W;
    *y0 = baseline - VAR_FONT_SMALL_DESCENT - VAR_COLLAPSE_CHIP_SLOP;
    *y1 = baseline + (FONT_SMALL_H - VAR_FONT_SMALL_DESCENT) +
          VAR_COLLAPSE_CHIP_SLOP;
    /* Never let the cell reach past the panel border — clicks outside the
     * panel belong to whatever overlay is there, not to the chip. */
    if (*y1 > py + ph) *y1 = py + ph;
}

int ui_variable_panel_hit_test_collapse_toggle(const UiVariablePanelView *view,
                                               int mx, int my) {
    int x0, x1, y0, y1, y_up;
    if (!view->visible || view->window_h <= 0) return 0;
    var_panel_collapse_chip_rect(view, &x0, &x1, &y0, &y1);
    y_up = view->window_h - my;
    return mx >= x0 && mx <= x1 && y_up >= y0 && y_up <= y1;
}

UiHit ui_variable_panel_hit_test(const UiVariablePanelView *view, int mx, int my) {
    UiHit h = ui_hit_none();
    if (!view->visible) return h;
    if (view->window_h <= 0) return h;

    int px, py, pw, ph;
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);
    int gl_y = view->window_h - my;

    if (ui_variable_panel_hit_test_collapse_toggle(view, mx, my)) {
        h.kind = UI_HIT_VARIABLE_COLLAPSE_TOGGLE;
        h.local_x = (float)(mx - px);
        h.local_y = (float)(gl_y - py);
        return h;
    }

    int row = -1;
    if (!ui_variable_panel_hit_row(view, mx, my, &row)) return h;

    h.kind = UI_HIT_VARIABLE_SLIDER;
    h.item_idx = row;
    h.local_x = (float)(mx - px);
    h.local_y = (float)(gl_y - py);
    return h;
}

void ui_variable_panel_render(const UiVariablePanelView *view) {
    if (!view->visible) return;

    const UiVariable *vars = view->vars;
    int var_count = (vars && !view->collapsed) ? clamp_var_count(view->var_count) : 0;

    int px, py, pw, ph;
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);

    gl2d_begin(view->window_w, view->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background + Border */
    gl2d_panel_frame((float)px, (float)py, (float)pw, (float)ph,
                     UI_TOK_SUNKEN, 0.88f, UI_TOK_BORDER, 0.75f);

    /* Title */
    int title_baseline = var_panel_title_baseline(py, ph);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)(px + VAR_PANEL_PAD), (float)title_baseline,
                "Variables (declared)", FONT_SMALL);

    /* Collapse/expand chip: "[-]" collapses to just this title bar, "[+]"
     * restores the slider rows. Mouse-only — there is no keymap slot for it.
     *
     * An action chip by the grammar in ui/core/gl_2d.h — bracketed, muted, no
     * well. It names the move, not the state, which is the documented
     * exemption for single-glyph disclosure controls: +/- cannot be misread
     * as a value, and the panel's own collapsed shape already shows which way
     * it is. Drawn through the shared helper so the color cannot drift from
     * the other action chips. */
    {
        int chip_x0, chip_x1, chip_y0, chip_y1;
        var_panel_collapse_chip_rect(view, &chip_x0, &chip_x1, &chip_y0, &chip_y1);
        gl2d_chip_action((float)chip_x0, (float)title_baseline,
                         view->collapsed ? "[+]" : "[-]");
    }

    if (view->collapsed) {
        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }

    /* Column offsets within the panel - sized for multi-char var names */
    int max_name_len = 1;
    for (int i = 0; i < var_count; i++) {
        int len = (int)strlen(vars[i].name);
        if (len > max_name_len) max_name_len = len;
    }
    int label_w  = (max_name_len + VAR_NAME_COL_PAD_CHARS) * FONT_SMALL_W;
    label_w      = MIN(label_w, VAR_NAME_MAX_PIXELS);
    int label_x  = px + VAR_PANEL_PAD;
    int val_x    = px + VAR_PANEL_PAD + label_w;
    int track_x  = val_x + VAR_VALUE_COL_PIXELS;
    int track_w  = pw - (track_x - px) - VAR_TRACK_PAD_RIGHT;
    int handle_w = VAR_HANDLE_W;
    if (track_w < handle_w + VAR_TRACK_MIN_SLACK) track_w = handle_w + VAR_TRACK_MIN_SLACK;

    /* Shared logarithmic scale: all handles normalized relative to each other. */
    float log_scale = var_panel_log_scale(vars, var_count);
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < var_count; i++) {
        const UiVariable *var = &vars[i];
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = ui_variable_value(var);

        /* Drag highlight - amber tint for coarse mode, blue for normal */
        if (view->drag_active_var == i) {
            glColor4fv(view->drag_coarse ? k_var_drag_coarse_bg : k_var_drag_linear_bg);
            glRectf((float)(px + 1), (float)row_y, (float)(px + pw - 1), (float)(row_y + VAR_ROW_H));
        }

        /* @tune badge: accent bar on the row's left edge marks a tagged var
         * (one exported as a keyboard knob). */
        if (var->tuned) {
            ui_clr(UI_TOK_ACCENT);
            glRectf((float)(px + 1), (float)(row_y + 2),
                    (float)(px + 3), (float)(row_y + VAR_ROW_H - 2));
        }

        /* Label */
        glColor3fv(var->written ? k_var_name_written : k_var_name);
        gl2d_draw_string((float)label_x, (float)text_y, truncate_var_name(var->name, VAR_NAME_MAX_CHARS), FONT_SMALL);

        /* Value */
        char valstr[16];
        snprintf(valstr, sizeof(valstr), "%*.*f", VAR_VALUE_FMT_WIDTH, VAR_VALUE_FMT_PREC, (double)val);
        glColor3fv(var->written ? k_var_value_written : k_var_value);
        gl2d_draw_string((float)val_x, (float)text_y, valstr, FONT_SMALL);

        /* Slider track */
        ui_clr_a(UI_TOK_MENU_LABEL_ACTIVE_BG, 0.90f);
        glRectf((float)track_x, (float)(row_y + VAR_TRACK_PAD_TOP),
                (float)(track_x + track_w), (float)(row_y + VAR_ROW_H - VAR_TRACK_PAD_BOTTOM));

        /* Centre tick (marks zero on the log scale) */
        float cx = (float)track_x + (float)track_w * 0.5f;
        ui_clr_a(UI_TOK_DIVIDER, 0.70f);
        glBegin(GL_LINES);
        glVertex2f(cx, (float)(row_y + VAR_TICK_PAD_TOP));
        glVertex2f(cx, (float)(row_y + VAR_ROW_H - VAR_TICK_PAD_BOTTOM));
        glEnd();

        /* Handle - position computed via shared log-normalized scale.
         * Yellow = normal drag, orange = coarse drag, blue = idle. */
        float t  = val_to_slider_t(val, log_scale);
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (view->drag_active_var == i) {
            glColor4fv(view->drag_coarse ? k_var_handle_coarse : k_var_handle_linear);
        } else {
            glColor4fv(var->written ? k_var_handle_written : k_var_handle_idle);
        }
        glRectf(hx, (float)(row_y + VAR_HANDLE_PAD_TOP), hx + (float)handle_w,
                (float)(row_y + VAR_ROW_H - VAR_HANDLE_PAD_BOTTOM));
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
