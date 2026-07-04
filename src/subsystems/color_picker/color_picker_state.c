/*
 * color_picker_state.c - Floating color-picker peer (state + lifecycle + writeback).
 * Note: File-private helper functions and variables use the cp_* shorthand prefix.
 */
#include "subsystems/color_picker/color_picker_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Slider geometry. Kept here so the renderer reads the resulting rects
 * via ColorPickerView instead of recomputing geometry. */
#define CP_SV_SZ    150   /* SV square side */
#define CP_HUE_W     18   /* hue bar width  */
#define CP_ALPHA_W   18   /* alpha bar width (RGBA-shaped only) */
#define CP_GAP        6   /* gap between elements */
#define CP_PREV_H    16   /* preview strip height */
#define CP_PANEL_OFFSET  8   /* px gap between the code panel and the picker */
#define CP_SCREEN_MARGIN 4   /* px keep-on-screen margin at window edges */
#define CP_HUE_MAX   0.999f  /* clamp hue < 1.0 so hsv_to_rgb's sextant
                              * index (int)(h*6) never lands on the wrap */
#define CP_TAB_H     16      /* palette tab-strip height */
#define CP_SWATCH_GAP 3      /* gap between palette swatches */
#define CP_FULL_COLS  8      /* Full palette: hue columns */
#define CP_FULL_HUE_ROWS 6   /* Full palette: tint->shade rows (+1 grey row) */
#define CP_FULL_COUNT (CP_FULL_COLS * (CP_FULL_HUE_ROWS + 1))  /* 56 */

typedef enum {
    CP_DRAG_NONE = 0,
    CP_DRAG_SV   = 1,
    CP_DRAG_HUE  = 2,
    CP_DRAG_ALPHA = 3
} CpDragTarget;

/* Active palette tab. Session state: persists across opens (the user's last
 * tab choice sticks), cleared only by color_picker_state_reset. */
static CpPaletteTab g_cp_tab = CP_TAB_BASIC;

/* Harmony key: a persistent base color (HSV) the tetrad derives from. Unlike
 * the active command's color, it survives close/reopen — so a coordinated set
 * can be applied across several color commands. Seeded from the current color
 * the first time the Harmony tab is entered (or via the Set-key button), and
 * NOT moved when a derived swatch is applied. Cleared by state_reset. */
static int   g_cp_key_set = 0;
static float g_cp_key_hue = 0.0f, g_cp_key_sat = 1.0f, g_cp_key_val = 1.0f;

/* Basic palette: ten common colors, one clickable row. */
static const float CP_BASIC[10][3] = {
    { 0.00f, 0.00f, 0.00f },   /* black   */
    { 1.00f, 1.00f, 1.00f },   /* white   */
    { 0.50f, 0.50f, 0.50f },   /* grey    */
    { 0.85f, 0.20f, 0.20f },   /* red     */
    { 0.95f, 0.55f, 0.15f },   /* orange  */
    { 0.95f, 0.85f, 0.20f },   /* yellow  */
    { 0.30f, 0.70f, 0.30f },   /* green   */
    { 0.25f, 0.75f, 0.80f },   /* cyan    */
    { 0.25f, 0.45f, 0.85f },   /* blue    */
    { 0.75f, 0.30f, 0.80f },   /* magenta */
};

/* Full palette: built lazily from HSV the first time it's needed (avoids a
 * 56-entry literal and keeps the colors in sync with hsv_to_rgb). Rows go
 * light tint -> pure -> dark shade per hue column, plus a trailing greyscale
 * row. Index is row-major: idx = row * CP_FULL_COLS + col.
 *
 * Column hues are curated named color-wheel stops, NOT an even c/COLS slice:
 * HSV hue is perceptually non-uniform (yellow sits in a narrow band near
 * 0.167, green spans a wide one near 0.333), so an even split skips yellow
 * and lands two adjacent columns in the green band. These eight stops keep
 * one column each on red/orange/yellow/green/cyan/blue/violet/magenta. */
static const float CP_FULL_HUES[CP_FULL_COLS] = {
    0.000f,   /* red     (  0 deg) */
    0.083f,   /* orange  ( 30 deg) */
    0.167f,   /* yellow  ( 60 deg) */
    0.333f,   /* green   (120 deg) */
    0.500f,   /* cyan    (180 deg) */
    0.667f,   /* blue    (240 deg) */
    0.750f,   /* violet  (270 deg) */
    0.833f,   /* magenta (300 deg) */
};
static float g_cp_full[CP_FULL_COUNT][3];
static int   g_cp_full_ready = 0;

static void cp_ensure_full(void) {
    if (g_cp_full_ready) return;
    for (int c = 0; c < CP_FULL_COLS; c++) {
        float h = CP_FULL_HUES[c];
        if (h >= 1.0f) h = CP_HUE_MAX;
        for (int r = 0; r < CP_FULL_HUE_ROWS; r++) {
            float sat, val;
            if (r <= 2) {            /* light tint -> pure */
                sat = 0.30f + 0.35f * (float)r;   /* 0.30, 0.65, 1.00 */
                val = 1.0f;
            } else {                 /* pure -> dark shade */
                sat = 1.0f;
                val = 1.0f - 0.20f * (float)(r - 2);  /* 0.80, 0.60, 0.40 */
            }
            int idx = r * CP_FULL_COLS + c;
            color_picker_hsv_to_rgb(h, sat, val,
                                    &g_cp_full[idx][0], &g_cp_full[idx][1],
                                    &g_cp_full[idx][2]);
        }
    }
    /* Trailing greyscale row: white -> black across the columns. */
    for (int c = 0; c < CP_FULL_COLS; c++) {
        float g = 1.0f - (float)c / (float)(CP_FULL_COLS - 1);
        int idx = CP_FULL_HUE_ROWS * CP_FULL_COLS + c;
        g_cp_full[idx][0] = g; g_cp_full[idx][1] = g; g_cp_full[idx][2] = g;
    }
    g_cp_full_ready = 1;
}

/* g_cp_line >= 0: picker is open for that source-cmd index */
static int   g_cp_line       = -1;
static float g_cp_hue        = 0.0f, g_cp_sat = 1.0f, g_cp_val = 1.0f;
static float g_cp_alpha      = 1.0f;
static int   g_cp_px         = 0, g_cp_py = 0;  /* top-left (OpenGL y-up) */
static CpDragTarget g_cp_drag = CP_DRAG_NONE;
static int   g_cp_has_alpha  = 0;    /* RGBA-shaped command */
static float g_cp_value_max  = 1.0f;
/* Picker session undo bookkeeping: the very first writeback after open
 * (or after switching the active line) captures one undo snapshot via
 * editor_commit_apply_external_change(capture_undo=1). Subsequent
 * slider drags within the same session pass capture_undo=0 so the
 * undo ring records "before this picker session" state, not every
 * intermediate slider tick. */
static int   g_cp_undo_captured = 0;

/* Base HSV the harmony tetrad fans out from: the key if captured, else the
 * live color (so the tab is never empty before a key is set). */
static void cp_harmony_base(float *h, float *s, float *v) {
    if (g_cp_key_set) { *h = g_cp_key_hue; *s = g_cp_key_sat; *v = g_cp_key_val; }
    else              { *h = g_cp_hue;     *s = g_cp_sat;     *v = g_cp_val; }
}

/* Capture the current color as the harmony key. */
static void cp_set_key_from_current(void) {
    g_cp_key_hue = g_cp_hue;
    g_cp_key_sat = g_cp_sat;
    g_cp_key_val = g_cp_val;
    g_cp_key_set = 1;
}

/* Installed host services (document read/write + screen geometry). */
static const ColorPickerHostBridge *g_host = NULL;

void color_picker_install_host(const ColorPickerHostBridge *host) {
    g_host = host;
}

/* Window height via the host (0 if unset) — used for screen-y flips. */
static int cp_viewport_h(void) {
    int w = 0, h = 0;
    if (g_host && g_host->viewport) g_host->viewport(&w, &h);
    return h;
}

/* 1 if the active line is still an editable color command per the host. */
static int cp_line_editable(void) {
    if (g_cp_line < 0 || !g_host || !g_host->read_color) return 0;
    float r, g, b, a, vmax; int ha;
    return g_host->read_color(g_cp_line, &r, &g, &b, &a, &ha, &vmax);
}

static void cp_compute_rects(int px, int py, ColorPickerRects *r) {
    int sz = CP_SV_SZ;
    int hx = px + sz + CP_GAP;

    r->sv_x = px;          r->sv_y = py - sz; r->sv_sz = sz;
    r->hue_x = hx;         r->hue_y = py - sz; r->hue_w = CP_HUE_W; r->hue_h = sz;
    r->alp_x = hx + CP_HUE_W + CP_GAP;
    r->alp_y = py - sz;
    r->alp_w = CP_ALPHA_W;
    r->alp_h = sz;
}

/* Defined further down; the palette-apply helper needs them here. */
static void cp_rgb_to_hsv(float r, float g, float b,
                          float *h, float *s, float *v);
static int  color_picker_write_cmd(void);

/* Width spanned by the SV square + hue bar (+ alpha bar) — the palette and
 * preview strip stretch across this. */
static int cp_total_w(void) {
    return CP_SV_SZ + CP_GAP + CP_HUE_W
         + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0);
}

static int cp_tab_cols(CpPaletteTab t) {
    if (t == CP_TAB_BASIC)   return 10;
    if (t == CP_TAB_FULL)    return CP_FULL_COLS;
    return 4;                            /* harmony: chosen + 3 derived */
}
static int cp_tab_rows(CpPaletteTab t) {
    return (t == CP_TAB_FULL) ? (CP_FULL_HUE_ROWS + 1) : 1;
}
static int cp_swatch_count(CpPaletteTab t) {
    if (t == CP_TAB_BASIC)   return 10;
    if (t == CP_TAB_FULL)    return CP_FULL_COUNT;
    return 4;
}
/* Square cell side: fit `cols` cells (with gaps) across cp_total_w(). */
static int cp_cell(CpPaletteTab t) {
    int cols = cp_tab_cols(t);
    int c = (cp_total_w() - (cols - 1) * CP_SWATCH_GAP) / cols;
    return (c < 6) ? 6 : c;
}
static int cp_grid_h(CpPaletteTab t) {
    int rows = cp_tab_rows(t);
    return rows * cp_cell(t) + (rows - 1) * CP_SWATCH_GAP;
}

/* Extra height for the Harmony tab's "Set key" button (button + gap). */
static int cp_keybtn_block(CpPaletteTab t) {
    return (t == CP_TAB_HARMONY) ? (CP_TAB_H + CP_GAP) : 0;
}

/* Whole-popup extent. popup_w mirrors the legacy `pw` (element span + gap);
 * popup_h is the original (SV + preview) height plus the tab strip, the
 * active palette grid, and the Harmony "Set key" button when shown. */
static int cp_popup_w(void) { return cp_total_w() + CP_GAP; }
static int cp_popup_h(void) {
    return CP_SV_SZ + CP_GAP + CP_PREV_H + CP_GAP
         + CP_TAB_H + CP_GAP + cp_grid_h(g_cp_tab)
         + cp_keybtn_block(g_cp_tab) + CP_GAP;
}

/* Resolve the RGBA the swatch at index `i` of tab `t` writes/draws. Alpha is
 * cosmetic here (1.0); the actual writeback keeps the session alpha. */
static void cp_swatch_rgba(CpPaletteTab t, int i, float out[4]) {
    out[3] = 1.0f;
    if (t == CP_TAB_HARMONY) {
        float bh, bs, bv;
        cp_harmony_base(&bh, &bs, &bv);
        float h = fmodf(bh + 0.25f * (float)i, 1.0f);
        color_picker_hsv_to_rgb(h, bs, bv, &out[0], &out[1], &out[2]);
    } else if (t == CP_TAB_FULL) {
        cp_ensure_full();
        out[0] = g_cp_full[i][0]; out[1] = g_cp_full[i][1]; out[2] = g_cp_full[i][2];
    } else {
        out[0] = CP_BASIC[i][0]; out[1] = CP_BASIC[i][1]; out[2] = CP_BASIC[i][2];
    }
}

/* Tab-strip top edge (y-up) for popup top py. */
static int cp_tab_top(int py) {
    return (py - CP_SV_SZ - CP_GAP) - CP_PREV_H - CP_GAP;
}
/* Palette-grid top edge (y-up), one tab strip + gap below the tab top. */
static int cp_pal_top(int py) {
    return cp_tab_top(py) - CP_TAB_H - CP_GAP;
}
/* Harmony "Set key" button top edge (y-up), one gap below the swatch grid. */
static int cp_keybtn_top(int py) {
    return cp_pal_top(py) - cp_grid_h(CP_TAB_HARMONY) - CP_GAP;
}

/* Quantize a [0,1] channel to a clamped 0..255 byte. */
static int cp_byte(float c) {
    int b = (int)(c * 255.0f + 0.5f);
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return b;
}

/* Fill the view's tab-strip + palette-grid geometry and resolved swatches,
 * stacked below the preview swatch at popup top-left (px, py). */
static void cp_compute_palette(int px, int py, ColorPickerView *v) {
    CpPaletteTab t = g_cp_tab;
    int tab_top = cp_tab_top(py);

    v->palette_tab = (int)t;
    v->tab_x = px;  v->tab_y = tab_top;
    v->tab_w = cp_total_w();  v->tab_h = CP_TAB_H;

    v->pal_x    = px;
    v->pal_y    = cp_pal_top(py);
    v->pal_cols = cp_tab_cols(t);
    v->pal_rows = cp_tab_rows(t);
    v->pal_cell = cp_cell(t);
    v->pal_gap  = CP_SWATCH_GAP;

    int n = cp_swatch_count(t);
    if (n > CP_MAX_SWATCHES) n = CP_MAX_SWATCHES;
    v->swatch_count = n;
    for (int i = 0; i < n; i++)
        cp_swatch_rgba(t, i, v->swatches[i]);

    /* Harmony "Set key" button (only on the Harmony tab). */
    v->key_set = g_cp_key_set;
    if (t == CP_TAB_HARMONY) {
        int btop = cp_keybtn_top(py);
        v->key_btn_x = px;  v->key_btn_y = btop;
        v->key_btn_w = cp_total_w();  v->key_btn_h = CP_TAB_H;
    } else {
        v->key_btn_x = v->key_btn_y = v->key_btn_w = v->key_btn_h = 0;
    }

    /* 32-bit #RRGGBBAA readout of the current color (alpha FF when the
     * command has no alpha channel). */
    float hr, hg, hb;
    color_picker_hsv_to_rgb(g_cp_hue, g_cp_sat, g_cp_val, &hr, &hg, &hb);
    int A = g_cp_has_alpha ? cp_byte(g_cp_alpha) : 255;
    snprintf(v->hex, sizeof(v->hex), "#%02X%02X%02X%02X",
             cp_byte(hr), cp_byte(hg), cp_byte(hb), A);
}

/* Swatch index under (mx, gl_y) for the active palette, or -1. */
static int cp_palette_hit(int px, int py, int mx, int gl_y) {
    CpPaletteTab t = g_cp_tab;
    int cols = cp_tab_cols(t), rows = cp_tab_rows(t);
    int cell = cp_cell(t), gap = CP_SWATCH_GAP;
    int pal_x = px;
    int pal_y = cp_pal_top(py);
    int count = cp_swatch_count(t);
    for (int r = 0; r < rows; r++) {
        int cell_top = pal_y - r * (cell + gap);
        if (gl_y > cell_top || gl_y < cell_top - cell) continue;
        for (int c = 0; c < cols; c++) {
            int cell_x = pal_x + c * (cell + gap);
            if (mx < cell_x || mx >= cell_x + cell) continue;
            int idx = r * cols + c;
            return (idx < count) ? idx : -1;
        }
    }
    return -1;
}

/* Tab segment under mx within the tab strip, or -1 if mx is off-strip. */
static int cp_tab_hit(int px, int mx) {
    int total_w = cp_total_w();
    if (mx < px || mx >= px + total_w) return -1;
    int seg = (mx - px) * CP_TAB_COUNT / total_w;
    if (seg < 0) seg = 0;
    if (seg >= CP_TAB_COUNT) seg = CP_TAB_COUNT - 1;
    return seg;
}

/* Re-clamp the popup's vertical anchor after its height changes (tab switch
 * to a taller grid) so it stays fully on-screen. */
static void cp_reclamp_vertical(void) {
    int win_w = 0, win_h = 0;
    if (g_host && g_host->viewport) g_host->viewport(&win_w, &win_h);
    int ph = cp_popup_h();
    if (g_cp_py > win_h - CP_SCREEN_MARGIN) g_cp_py = win_h - CP_SCREEN_MARGIN;
    if (g_cp_py - ph < CP_SCREEN_MARGIN)    g_cp_py = ph + CP_SCREEN_MARGIN;
}

/* Apply a palette swatch click: sync HSV from its RGB, clamp V to the active
 * command's max, keep the session alpha, and write back. Returns the
 * writeback result (1 if state mutated). */
static int cp_apply_swatch(int idx) {
    float rgba[4];
    cp_swatch_rgba(g_cp_tab, idx, rgba);
    cp_rgb_to_hsv(rgba[0], rgba[1], rgba[2], &g_cp_hue, &g_cp_sat, &g_cp_val);
    if (g_cp_hue >= 1.0f) g_cp_hue = CP_HUE_MAX;
    if (g_cp_val > g_cp_value_max) g_cp_val = g_cp_value_max;
    return color_picker_write_cmd();
}

void color_picker_hsv_to_rgb(float h, float s, float v,
                             float *r, float *g, float *b) {
    int i = (int)(h * 6.0f);
    float f = h * 6.0f - i, p = v * (1 - s), q = v * (1 - f * s),
          t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void cp_rgb_to_hsv(float r, float g, float b,
                          float *h, float *s, float *v) {
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *v = mx;
    *s = (mx != 0.0f) ? d / mx : 0.0f;
    if (d == 0.0f) { *h = 0.0f; return; }
    if      (mx == r) *h = fmodf((g - b) / d, 6.0f) / 6.0f;
    else if (mx == g) *h = ((b - r) / d + 2.0f) / 6.0f;
    else              *h = ((r - g) / d + 4.0f) / 6.0f;
    if (*h < 0.0f) *h += 1.0f;
}

/* Push the current slider state through the host's writeback. The host owns
 * the source-line synthesis + parse + commit pipeline; the picker just hands
 * over the resolved RGBA and the session undo flag. Returns 1 if the
 * writeback fired (state actually mutated), 0 if it short-circuited. */
static int color_picker_write_cmd(void) {
    if (g_cp_line < 0 || !g_host || !g_host->write_color)
        return 0;
    float r, g, b;
    color_picker_hsv_to_rgb(g_cp_hue, g_cp_sat, g_cp_val, &r, &g, &b);
    /* The first writeback of a session captures undo; subsequent drags pass
     * capture_undo=0 so each session lands as one undo entry. */
    if (g_host->write_color(g_cp_line, r, g, b, g_cp_alpha, !g_cp_undo_captured)) {
        g_cp_undo_captured = 1;
        return 1;
    }
    return 0;
}

static int clamp_popup_x(int prefer_right_x, int prefer_left_x,
                         int popup_w, int win_w, int margin) {
    int x = prefer_right_x;
    /* Flip left if right runs off screen */
    if (x + popup_w > win_w - margin) {
        x = prefer_left_x;
    }
    /* Flushed-right clamp if left also runs off screen */
    if (x < margin) {
        x = win_w - popup_w - margin;
    }
    /* Hard left-edge clamp fallback */
    if (x < margin) {
        x = margin;
    }
    return x;
}

void color_picker_start(int cmd_idx, int my) {
    if (!g_host || !g_host->read_color)
        return;

    float r, g, b, a, value_max;
    int has_alpha;
    if (!g_host->read_color(cmd_idx, &r, &g, &b, &a, &has_alpha, &value_max))
        return;  /* not an editable color command */

    /* New session (first open, or switching to a different line):
     * arm the next writeback to capture undo. Editing the same line
     * after a close-then-reopen is treated as a new session — matches
     * the legacy behaviour where every open-on-different-line pushed
     * undo, except the snapshot now records the moment of the first
     * actual mutation rather than the moment of open. */
    if (g_cp_line != cmd_idx)
        g_cp_undo_captured = 0;

    int cp_x = 0, cp_w = 0;
    if (g_host->code_panel_rect) g_host->code_panel_rect(&cp_x, &cp_w);
    g_cp_line = cmd_idx;
    g_cp_has_alpha = has_alpha;
    g_cp_alpha     = has_alpha ? a : 1.0f;
    g_cp_value_max = value_max;
    cp_rgb_to_hsv(r, g, b, &g_cp_hue, &g_cp_sat, &g_cp_val);
    if (g_cp_val > g_cp_value_max)
        g_cp_val = g_cp_value_max;

    /* Prefer to the right of the code panel, then flip left if that
     * would run off-screen; the final clamp keeps a too-wide popup
     * usable even in narrow windows. */
    int pw = cp_popup_w();
    int ph = cp_popup_h();
    int win_w = 0, win_h = 0;
    if (g_host->viewport) g_host->viewport(&win_w, &win_h);
    int ppx = clamp_popup_x(cp_x + cp_w + CP_PANEL_OFFSET, cp_x - pw - CP_SCREEN_MARGIN, pw, win_w, CP_SCREEN_MARGIN);
    /* Center vertically around the trigger point, then clamp so the
     * popup stays fully on-screen in OpenGL's y-up coordinates. */
    int ppy = (win_h - my) + ph / 2;
    if (ppy > win_h - CP_SCREEN_MARGIN) ppy = win_h - CP_SCREEN_MARGIN;
    if (ppy - ph < CP_SCREEN_MARGIN)    ppy = ph + CP_SCREEN_MARGIN;
    g_cp_px = ppx;
    g_cp_py = ppy;
}

int color_picker_stop(void) {
    if (g_cp_line < 0) return 0;
    g_cp_line = -1;
    g_cp_drag = CP_DRAG_NONE;
    g_cp_undo_captured = 0;
    return 1;
}

void color_picker_state_reset(void) {
    color_picker_stop();
    g_cp_tab = CP_TAB_BASIC;
    g_cp_key_set = 0;
}

int color_picker_active_line(void) {
    return g_cp_line;
}

int color_picker_can_edit_cmd(int cmd_idx) {
    if (!g_host || !g_host->read_color) return 0;
    float r, g, b, a, vmax; int ha;
    return g_host->read_color(cmd_idx, &r, &g, &b, &a, &ha, &vmax);
}

ColorPickerView color_picker_view(void) {
    ColorPickerView v;
    memset(&v, 0, sizeof(v));
    if (g_cp_line < 0) {
        v.active_line = -1;
        return v;
    }
    v.open        = 1;
    v.active_line = g_cp_line;
    v.has_alpha   = g_cp_has_alpha;
    v.hue         = g_cp_hue;
    v.sat         = g_cp_sat;
    v.val         = g_cp_val;
    v.alpha       = g_cp_alpha;
    v.anchor_px   = g_cp_px;
    v.anchor_py   = g_cp_py;
    v.value_max   = g_cp_value_max;
    v.gap         = CP_GAP;
    v.prev_h      = CP_PREV_H;
    v.popup_w     = cp_popup_w();
    v.popup_h     = cp_popup_h();
    cp_compute_rects(g_cp_px, g_cp_py, &v.rects);
    cp_compute_palette(g_cp_px, g_cp_py, &v);
    return v;
}

/* Clamp v to [0, 1]. Replaces the inline `if (x<0) x=0; if (x>1) x=1;`
 * pattern that kept tripping -Wmisleading-indentation (two statements
 * per line). */
static float cp_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Apply the slider value for drag target `which` (1=SV, 2=hue,
 * 3=alpha) from pointer (mx, gl_y) against rects r. Shared by press
 * (after a region hit-test arms the target) and motion so the
 * ratio/clamp/wrap math has one home. */
static void cp_apply_drag_at(CpDragTarget which, int mx, int gl_y,
                             const ColorPickerRects *r) {
    if (which == CP_DRAG_SV) {
        g_cp_sat = cp_clamp01((float)(mx - r->sv_x) / (float)r->sv_sz);
        g_cp_val = cp_clamp01((float)(gl_y - r->sv_y) / (float)r->sv_sz);
        if (g_cp_val > g_cp_value_max) g_cp_val = g_cp_value_max;
    } else if (which == CP_DRAG_HUE) {
        g_cp_hue = cp_clamp01(1.0f -
                              (float)(gl_y - r->hue_y) / (float)r->hue_h);
        if (g_cp_hue >= 1) g_cp_hue = CP_HUE_MAX;
    } else if (which == CP_DRAG_ALPHA) {
        g_cp_alpha = cp_clamp01((float)(gl_y - r->alp_y) / (float)r->alp_h);
    }
}

ColorPickerInputResult color_picker_handle_press(int mx, int my) {
    ColorPickerInputResult res = { 0, 0, 0 };
    ColorPickerRects r;

    if (!cp_line_editable()) return res;
    int gl_y = cp_viewport_h() - my;
    cp_compute_rects(g_cp_px, g_cp_py, &r);

    /* SV square */
    if (mx >= r.sv_x && mx < r.sv_x + r.sv_sz &&
        gl_y >= r.sv_y && gl_y < r.sv_y + r.sv_sz) {
        g_cp_drag = CP_DRAG_SV;
        cp_apply_drag_at(CP_DRAG_SV, mx, gl_y, &r);
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }
    /* Hue bar */
    if (mx >= r.hue_x && mx < r.hue_x + r.hue_w &&
        gl_y >= r.hue_y && gl_y < r.hue_y + r.hue_h) {
        g_cp_drag = CP_DRAG_HUE;
        cp_apply_drag_at(CP_DRAG_HUE, mx, gl_y, &r);
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }
    /* Alpha bar (RGBA-shaped only) */
    if (g_cp_has_alpha &&
        mx >= r.alp_x && mx < r.alp_x + r.alp_w &&
        gl_y >= r.alp_y && gl_y < r.alp_y + r.alp_h) {
        g_cp_drag = CP_DRAG_ALPHA;
        cp_apply_drag_at(CP_DRAG_ALPHA, mx, gl_y, &r);
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }

    /* Palette tab strip */
    {
        int seg = cp_tab_hit(g_cp_px, mx);
        int tab_top = cp_tab_top(g_cp_py);
        if (seg >= 0 && gl_y <= tab_top && gl_y >= tab_top - CP_TAB_H) {
            g_cp_tab = (CpPaletteTab)seg;
            /* Seed the harmony key from the current color the first time the
             * tab is entered, so the tetrad is never empty; once set it
             * persists across instances until re-set via the button. */
            if (g_cp_tab == CP_TAB_HARMONY && !g_cp_key_set)
                cp_set_key_from_current();
            cp_reclamp_vertical();
            res.consumed = 1;
            return res;
        }
    }
    /* Harmony "Set key" button: capture the current color as the key. */
    if (g_cp_tab == CP_TAB_HARMONY) {
        int btop = cp_keybtn_top(g_cp_py);
        if (mx >= g_cp_px && mx < g_cp_px + cp_total_w() &&
            gl_y <= btop && gl_y > btop - CP_TAB_H) {
            cp_set_key_from_current();
            res.consumed = 1;
            return res;
        }
    }
    /* Palette swatch */
    {
        int idx = cp_palette_hit(g_cp_px, g_cp_py, mx, gl_y);
        if (idx >= 0) {
            res.changed = cp_apply_swatch(idx);
            res.consumed = 1;
            return res;
        }
    }

    /* Inside the popup bounds but outside any interactive rect: consumed-no-op */
    int pw = cp_popup_w();
    int ph = cp_popup_h();
    if (mx >= g_cp_px && mx < g_cp_px + pw &&
        gl_y >= g_cp_py - ph && gl_y < g_cp_py) {
        res.consumed = 1;
        res.closed = 0;
        res.changed = 0;
        return res;
    }

    /* Click outside the picker: dismiss and let the event flow through. */
    color_picker_stop();
    res.closed = 1;
    return res;
}

ColorPickerInputResult color_picker_handle_motion(int mx, int my) {
    ColorPickerInputResult res = { 0, 0, 0 };
    ColorPickerRects r;

    if (g_cp_drag == CP_DRAG_NONE || !cp_line_editable()) return res;
    int gl_y = cp_viewport_h() - my;
    cp_compute_rects(g_cp_px, g_cp_py, &r);
    cp_apply_drag_at(g_cp_drag, mx, gl_y, &r);
    res.changed = color_picker_write_cmd();
    res.consumed = 1;
    return res;
}

void color_picker_handle_release(void) {
    g_cp_drag = CP_DRAG_NONE;
}
