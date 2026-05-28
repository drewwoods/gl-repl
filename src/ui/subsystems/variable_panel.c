/*
 * ui_variable_panel.c -- Floating slider panel for declared variables.
 *
 * Pure renderer + hit-test. Render reads UI-facing variable rows from the
 * frame snapshot, scene rect, and the variable_panel peer's drag accessors.
 * The actual
 * value mutation lives in variable_panel_drag.c (the peer's drag
 * implementation); the editor's mouse handler invokes the peer via
 * variable_panel_handle_drag_*.
 *
 * The replay-lift easing state is panel-local animation (not
 * variable mutation) and stays here.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "ui/subsystems/variable_panel.h"
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"
#include "subsystems/replay/replay_state.h"
#include "ui/app/state.h"
#include "subsystems/variable_panel/variable_panel_state.h"

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

/* Compute a shared logarithmic display scale from all variable absolute values.
 * All sliders use the same scale so their handles are normalized relative to
 * each other (a var at 100 shows near the extreme, one at 0.01 still visible). */
static int ui_variable_count(const UiVariableList *vars) {
    if (!vars || !vars->vars || vars->count <= 0)
        return 0;
    if (vars->count > MAX_PREDEF_VARS)
        return MAX_PREDEF_VARS;
    return vars->count;
}

static float ui_variable_value(const UiVariable *var) {
    return (var && var->value) ? *var->value : 0.0f;
}

static float var_panel_log_scale(const UiVariableList *vars) {
    float max_abs = 0.1f;   /* minimum display range */
    int count = ui_variable_count(vars);
    for (int i = 0; i < count; i++) {
        float av = fabsf(ui_variable_value(&vars->vars[i]));
        if (av > max_abs) max_abs = av;
    }
    return max_abs * 1.25f; /* 25% headroom so handle doesn't hug the edge */
}

static const char *truncate_var_name(const char *name, size_t max_len) {
    static char buf[64];
    max_len = MIN(max_len, sizeof(buf) - 1);

    /* truncate name with ellipsis in the middle if necessary */
    size_t n = strlen(name);
    if (n > max_len) {
        size_t half = max_len / 2;
        snprintf(buf, max_len, "%.*s...%.*s", (int)half - 1, name, (int)(max_len - half - 2), name + n - (max_len - half - 2));
    } else {
        strncpy(buf, name, max_len);
        buf[max_len] = '\0';
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

#define VAR_PANEL_W   275
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20
#define VAR_PANEL_BASE_Y 8
/* Extra gap above REPLAY_HUD_BOTTOM_Y while replay HUD is active. */
#define VAR_REPLAY_CLEARANCE 10
/* Per-frame easing fraction the replay-lift converges by, and the pixel
 * gap below which it snaps to the target. Feel tunables for the lift
 * animation (cf. config.h's GLR_CAMERA_TARGET_DECAY). */
#define VAR_PANEL_LIFT_EASE 0.22f
#define VAR_PANEL_LIFT_SNAP_PX 0.25f
/* Layout constants for variable rows */
#define VAR_NAME_COL_WIDTH_FRAC 3  /* name column is 1/3 of panel width */
#define VAR_NAME_COL_PADDING 1     /* extra char width padding for name column */
#define VAR_VALUE_FMT_WIDTH 7      /* format field width for value display */
#define VAR_VALUE_FMT_PREC 3       /* decimal precision for value display */
#define VAR_HANDLE_W 10            /* width of slider handle */
#define VAR_VALUE_COL_PIXELS (VAR_VALUE_FMT_WIDTH*FONT_SMALL_W + 10) /* pixels reserved for value (empirical spacing) */
#define VAR_TRACK_RIGHT_MARGIN 8   /* right margin before track edge */
#define VAR_HANDLE_MIN_TRACK 4     /* minimum track width including handle */
#define VAR_TRACK_PAD_TOP 6        /* top padding of track from row bounds */
#define VAR_TRACK_PAD_BOTTOM 6     /* bottom padding of track from row bounds */
#define VAR_TICK_PAD_TOP 5         /* top padding of center tick */
#define VAR_TICK_PAD_BOTTOM 5      /* bottom padding of center tick */
#define VAR_HANDLE_PAD_TOP 4       /* top padding of handle from row bounds */
#define VAR_HANDLE_PAD_BOTTOM 4    /* bottom padding of handle from row bounds */
#define MAX_VAR_NAME_PIXELS VAR_PANEL_W / VAR_NAME_COL_WIDTH_FRAC
#define MAX_VAR_NAME_CHARS MAX_VAR_NAME_PIXELS / FONT_SMALL_W

static float var_panel_replay_lift(const UiRenderSnapshot *snap) {
    return snap ? snap->variable_panel.replay_lift_px : variable_panel_view().replay_lift_px;
}

static int ui_variable_panel_clamp_count(int var_count) {
    if (var_count < 0)
        return 0;
    if (var_count > MAX_PREDEF_VARS)
        return MAX_PREDEF_VARS;
    return var_count;
}

/* Geometry in render coords (y=0 at bottom). */
void ui_variable_panel_rect_for_count(const UiRenderSnapshot *snap,
                                      int variable_count,
                                      int *px, int *py,
                                      int *pw, int *ph) {
    int sc_x, sc_y, sc_w, sc_h;
    int panel_w, panel_h, panel_x, panel_y;
    int min_y, max_y;

    variable_count = ui_variable_panel_clamp_count(variable_count);
    ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    panel_w = VAR_PANEL_W;
    panel_h = VAR_TITLE_H + variable_count * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    panel_x = sc_x + sc_w - panel_w - 8;
    if (panel_x < sc_x + 4) panel_x = sc_x + 4;

    panel_y = sc_y + VAR_PANEL_BASE_Y + STATUSBAR_H
            + (int)lroundf(var_panel_replay_lift(snap));
    min_y = sc_y + STATUSBAR_H + 4;
    max_y = sc_y + sc_h - panel_h - 4;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = ui_layout_code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP
                ? sc_y + sc_h - panel_h - 4
                : min_y;
    }

    if (px) *px = panel_x;
    if (py) *py = panel_y;
    if (pw) *pw = panel_w;
    if (ph) *ph = panel_h;
}

/* Return 1 if GLUT screen coord (gx, gy) is in the panel; sets *out_row. */
int ui_variable_panel_hit_for_count(const UiRenderSnapshot *snap,
                                    int gx, int gy, int variable_count,
                                    int *out_row) {
    int px, py, pw, ph;
    variable_count = ui_variable_panel_clamp_count(variable_count);
    ui_variable_panel_rect_for_count(snap, variable_count, &px, &py, &pw, &ph);
    int ry = ui_state_viewport().window_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= variable_count) return 0;
    if (out_row) *out_row = row;
    return 1;
}

UiHit ui_variable_panel_hit_test(const UiRenderSnapshot *snap, int mx, int my, int variable_count) {
    UiHit h = ui_hit_none();
    if (!variable_panel_visible()) return h;
    int win_h = ui_state_viewport().window_h;
    if (win_h <= 0) return h;
    int row = -1;
    if (!ui_variable_panel_hit_for_count(snap, mx, my, variable_count, &row)) return h;

    int px, py, pw, ph;
    ui_variable_panel_rect_for_count(snap, variable_count, &px, &py, &pw, &ph);
    int gl_y = win_h - my;

    h.kind = UI_HIT_VARIABLE_SLIDER;
    h.item_idx = row;
    h.local_x = (float)(mx - px);
    h.local_y = (float)(gl_y - py);
    return h;
}

void ui_variable_panel_render(const UiRenderSnapshot *snap) {
    if (!snap->variable_panel.visible) return;

    const UiVariableList *vars = &snap->variable_panel_vars;
    int var_count = ui_variable_count(vars);

    int px, py, pw, ph;
    ui_variable_panel_rect_for_count(snap, var_count, &px, &py, &pw, &ph);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
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
        int len = (int)strlen(vars->vars[i].name);
        if (len > max_name_len) max_name_len = len;
    }
    int label_w  = (max_name_len + VAR_NAME_COL_PADDING) * FONT_SMALL_W;
    label_w      = MIN(label_w, MAX_VAR_NAME_PIXELS);
    int label_x  = px + VAR_PANEL_PAD;
    int val_x    = px + VAR_PANEL_PAD + label_w;
    int track_x  = val_x + VAR_VALUE_COL_PIXELS;
    int track_w  = pw - (track_x - px) - VAR_TRACK_RIGHT_MARGIN;
    int handle_w = VAR_HANDLE_W;
    if (track_w < handle_w + VAR_HANDLE_MIN_TRACK) track_w = handle_w + VAR_HANDLE_MIN_TRACK;

    /* Shared logarithmic scale: all handles normalized relative to each other. */
    float log_scale = var_panel_log_scale(vars);
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < var_count; i++) {
        const UiVariable *var = &vars->vars[i];
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = ui_variable_value(var);

        /* Drag highlight - amber tint for log mode, blue for linear */
        if (snap->variable_drag.active_var == i) {
            glColor4fv(snap->variable_drag.log_mode ? k_var_drag_log_bg : k_var_drag_linear_bg);
            glRectf((float)(px + 1), (float)row_y, (float)(px + pw - 1), (float)(row_y + VAR_ROW_H));
        }

        /* Label */
        glColor3fv(k_var_name);
        gl2d_draw_string((float)label_x, (float)text_y, truncate_var_name(var->name, MAX_VAR_NAME_CHARS), FONT_SMALL);

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
        if (snap->variable_drag.active_var == i) {
            glColor4fv(snap->variable_drag.log_mode ? k_var_handle_log : k_var_handle_linear);
        } else {
            glColor4fv(k_var_handle_idle);
        }
        glRectf(hx, (float)(row_y + VAR_HANDLE_PAD_TOP), hx + (float)handle_w,
                (float)(row_y + VAR_ROW_H - VAR_HANDLE_PAD_BOTTOM));
    }

    glDisable(GL_BLEND);
    gl2d_end();
}