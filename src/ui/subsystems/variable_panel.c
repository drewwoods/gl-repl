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
 * yellow-value two-tone and the log/linear/idle handle states encode
 * meaning and must stay fixed across every UI scheme. */
static const float k_var_name[3]           = { 0.70f, 0.85f, 0.70f };
static const float k_var_value[3]          = { 0.90f, 0.90f, 0.60f };
static const float k_var_drag_log_bg[4]    = { 0.30f, 0.20f, 0.05f, 0.60f };
static const float k_var_drag_linear_bg[4] = { 0.20f, 0.20f, 0.40f, 0.60f };
static const float k_var_handle_log[4]     = { 1.00f, 0.55f, 0.10f, 0.95f };
static const float k_var_handle_linear[4]  = { 1.00f, 0.80f, 0.20f, 0.95f };
static const float k_var_handle_idle[4]    = { 0.55f, 0.70f, 1.00f, 0.90f };

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
#define VAR_PANEL_BASE_Y 8
#define VAR_PANEL_RIGHT_MARGIN 8     /* gap from the scene's right edge */
#define VAR_PANEL_EDGE_PAD     4     /* min inset from scene edges after clamping */
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

/* Geometry in render coords (y=0 at bottom). */
void ui_variable_panel_rect(const UiVariablePanelView *view,
                            int *px, int *py, int *pw, int *ph) {
    int sc_x = view->scene_x, sc_y = view->scene_y;
    int sc_w = view->scene_w, sc_h = view->scene_h;
    int count = clamp_var_count(view->var_count);
    int panel_w = VAR_PANEL_W;
    int panel_h = VAR_TITLE_H + count * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    int panel_x = sc_x + sc_w - panel_w - VAR_PANEL_RIGHT_MARGIN;
    if (panel_x < sc_x + VAR_PANEL_EDGE_PAD) panel_x = sc_x + VAR_PANEL_EDGE_PAD;

    int panel_y = sc_y + VAR_PANEL_BASE_Y + view->statusbar_h
                + (int)lroundf(view->replay_lift_px);
    int min_y = sc_y + view->statusbar_h + VAR_PANEL_EDGE_PAD;
    int max_y = sc_y + sc_h - panel_h - VAR_PANEL_EDGE_PAD;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = view->code_panel_at_top
                ? sc_y + sc_h - panel_h - VAR_PANEL_EDGE_PAD
                : min_y;
    }

    if (px) *px = panel_x;
    if (py) *py = panel_y;
    if (pw) *pw = panel_w;
    if (ph) *ph = panel_h;
}

/* Return 1 if window coord (gx, gy) is in the panel; sets *out_row. */
int ui_variable_panel_hit_row(const UiVariablePanelView *view,
                              int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    int count = clamp_var_count(view->var_count);
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);
    int ry = view->window_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= count) return 0;
    if (out_row) *out_row = row;
    return 1;
}

UiHit ui_variable_panel_hit_test(const UiVariablePanelView *view, int mx, int my) {
    UiHit h = ui_hit_none();
    if (!view->visible) return h;
    if (view->window_h <= 0) return h;
    int row = -1;
    if (!ui_variable_panel_hit_row(view, mx, my, &row)) return h;

    int px, py, pw, ph;
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);
    int gl_y = view->window_h - my;

    h.kind = UI_HIT_VARIABLE_SLIDER;
    h.item_idx = row;
    h.local_x = (float)(mx - px);
    h.local_y = (float)(gl_y - py);
    return h;
}

void ui_variable_panel_render(const UiVariablePanelView *view) {
    if (!view->visible) return;

    const UiVariable *vars = view->vars;
    int var_count = vars ? clamp_var_count(view->var_count) : 0;

    int px, py, pw, ph;
    ui_variable_panel_rect(view, &px, &py, &pw, &ph);

    gl2d_begin(view->window_w, view->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background + Border */
    gl2d_panel_frame((float)px, (float)py, (float)pw, (float)ph,
                     UI_TOK_SUNKEN, 0.88f, UI_TOK_BORDER, 0.75f);

    /* Title */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)(px + VAR_PANEL_PAD),
                (float)(py + ph - VAR_PANEL_PAD - 4),
                "Variables (declared)", FONT_SMALL);

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

        /* Drag highlight - amber tint for log mode, blue for linear */
        if (view->drag_active_var == i) {
            glColor4fv(view->drag_log_mode ? k_var_drag_log_bg : k_var_drag_linear_bg);
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
        glColor3fv(k_var_name);
        gl2d_draw_string((float)label_x, (float)text_y, truncate_var_name(var->name, VAR_NAME_MAX_CHARS), FONT_SMALL);

        /* Value */
        char valstr[16];
        snprintf(valstr, sizeof(valstr), "%*.*f", VAR_VALUE_FMT_WIDTH, VAR_VALUE_FMT_PREC, (double)val);
        glColor3fv(k_var_value);
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
         * Yellow = linear drag, orange = log drag, blue = idle. */
        float t  = val_to_slider_t(val, log_scale);
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (view->drag_active_var == i) {
            glColor4fv(view->drag_log_mode ? k_var_handle_log : k_var_handle_linear);
        } else {
            glColor4fv(k_var_handle_idle);
        }
        glRectf(hx, (float)(row_y + VAR_HANDLE_PAD_TOP), hx + (float)handle_w,
                (float)(row_y + VAR_ROW_H - VAR_HANDLE_PAD_BOTTOM));
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
