/*
 * ui_variable_panel.c -- Floating slider panel for declared variables.
 *
 * Pure renderer + hit-test. Reads g_predef_vars, scene rect, and
 * the variable_panel peer's drag accessors, and draws. The actual
 * value mutation lives in variable_panel_drag.c (the peer's drag
 * implementation); the editor's mouse handler invokes the peer via
 * variable_panel_handle_drag_*.
 *
 * The replay-lift easing state is panel-local animation (not
 * variable mutation) and stays here.
 */
#include "ui_variable_panel.h"
#include "repl_state_views.h"
#include "ui_state.h"
#include "variable_panel_drag.h"
#include "ui_layout.h"
#include "variable_panel.h"
#include "replay_state.h"
#include "./include/gl_2d.h"
#include "ui_metrics.h"
#include "ui_replay_hud.h"

/* Local copy of the layout-mode clamp.  Duplicated by repl_editor.c and
 * ui_panels.c; promoting to a shared header is a separate cleanup. */
static int rvp_code_panel_layout_mode(void) {
    if (repl_state_presentation().code_panel_layout < 0 || repl_state_presentation().code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return repl_state_presentation().code_panel_layout;
}

/* Compute a shared logarithmic display scale from all variable absolute values.
 * All sliders use the same scale so their handles are normalized relative to
 * each other (a var at 100 shows near the extreme, one at 0.01 still visible). */
static float var_panel_log_scale(void) {
    float max_abs = 0.1f;   /* minimum display range */
    for (int i = 0; i < g_num_predef_vars; i++) {
        float av = fabsf(g_predef_vars[i].value);
        if (av > max_abs) max_abs = av;
    }
    return max_abs * 1.25f; /* 25% headroom so handle doesn't hug the edge */
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

#define VAR_PANEL_W   200
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20
#define VAR_PANEL_BASE_Y 8
/* Extra gap above REPLAY_HUD_BOTTOM_Y while replay HUD is active. */
#define VAR_REPLAY_CLEARANCE 10

static float g_var_panel_replay_lift_px = 0.0f;
static float g_var_panel_lift_update_time = -1.0f;
static float g_var_panel_lift_update_target = -1.0f;

static float var_panel_replay_target_lift_px(void) {
    float target = (float)((REPLAY_HUD_BOTTOM_Y + VAR_REPLAY_CLEARANCE)
                         - VAR_PANEL_BASE_Y);
    if (target < 0.0f) target = 0.0f;
    return target;
}

static float var_panel_replay_lift(void) {
    float target = 0.0f;
    if (replay_active())
        target = var_panel_replay_target_lift_px();

    float anim_time = repl_state_variables().anim_time;
    if (g_var_panel_lift_update_time == anim_time &&
        g_var_panel_lift_update_target == target)
        return g_var_panel_replay_lift_px;
    g_var_panel_lift_update_time = anim_time;
    g_var_panel_lift_update_target = target;

    /* Exponential-decay style easing toward target (and back to 0 when replay ends). */
    g_var_panel_replay_lift_px += (target - g_var_panel_replay_lift_px) * 0.22f;
    if (fabsf(target - g_var_panel_replay_lift_px) < 0.25f)
        g_var_panel_replay_lift_px = target;

    return g_var_panel_replay_lift_px;
}

/* Geometry in render coords (y=0 at bottom). */
void ui_variable_panel_rect(int *px, int *py, int *pw, int *ph) {
    int sc_x, sc_y, sc_w, sc_h;
    int panel_w, panel_h, panel_x, panel_y;
    int min_y, max_y;

    repl_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    panel_w = VAR_PANEL_W;
    panel_h = VAR_TITLE_H + g_num_predef_vars * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    panel_x = sc_x + sc_w - panel_w - 8;
    if (panel_x < sc_x + 4) panel_x = sc_x + 4;

    panel_y = sc_y + VAR_PANEL_BASE_Y + STATUSBAR_H
            + (int)lroundf(var_panel_replay_lift());
    min_y = sc_y + STATUSBAR_H + 4;
    max_y = sc_y + sc_h - panel_h - 4;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = rvp_code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP
                ? sc_y + sc_h - panel_h - 4
                : min_y;
    }

    if (px) *px = panel_x;
    if (py) *py = panel_y;
    if (pw) *pw = panel_w;
    if (ph) *ph = panel_h;
}

/* Return 1 if GLUT screen coord (gx, gy) is in the panel; sets *out_row. */
int ui_variable_panel_hit(int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    int ry = ui_state_viewport().window_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= g_num_predef_vars) return 0;
    if (out_row) *out_row = row;
    return 1;
}

UiHit ui_variable_panel_hit_test(int mx, int my) {
    UiHit h = ui_hit_none();
    if (!variable_panel_visible()) return h;
    int win_h = ui_state_viewport().window_h;
    if (win_h <= 0) return h;
    int row = -1;
    if (!ui_variable_panel_hit(mx, my, &row)) return h;

    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    int gl_y = win_h - my;

    h.kind = UI_HIT_VARIABLE_SLIDER;
    h.item_idx = row;
    h.local_x = (float)(mx - px);
    h.local_y = (float)(gl_y - py);
    return h;
}

void ui_variable_panel_render(const UiRenderSnapshot *snap) {
    if (!snap->variable_panel.visible) return;

    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background */
    glColor4f(0.05f, 0.05f, 0.10f, 0.88f);
    glRectf((float)((float)px), (float)((float)py), (float)((float)px)+(float)((float)pw), (float)((float)py)+(float)ph);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.70f, 0.75f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)px,        (float)py);
    glVertex2f((float)(px + pw), (float)py);
    glVertex2f((float)(px + pw), (float)(py + ph));
    glVertex2f((float)px,        (float)(py + ph));
    glEnd();

    /* Title */
    glColor3f(0.75f, 0.75f, 1.00f);
    gl2d_draw_string((float)(px + 6),
                (float)(py + ph - VAR_PANEL_PAD - 4),
                "Variables (declared)", FONT_SMALL);

    /* Column offsets within the panel - sized for multi-char var names */
    int max_name_len = 1;
    for (int i = 0; i < g_num_predef_vars; i++) {
        int len = (int)strlen(g_predef_vars[i].name);
        if (len > max_name_len) max_name_len = len;
    }
    int label_w  = max_name_len * FONT_SMALL_W + FONT_SMALL_W;
    if (label_w > pw / 3) label_w = pw / 3;
    int label_x  = px + 6;
    int val_x    = px + 6 + label_w;
    int track_x  = val_x + 66;
    int track_w  = pw - (track_x - px) - 8;
    int handle_w = 10;
    if (track_w < handle_w + 4) track_w = handle_w + 4;

    /* Shared logarithmic scale: all handles normalized relative to each other. */
    float log_scale = var_panel_log_scale();

    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < g_num_predef_vars; i++) {
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = g_predef_vars[i].value;

        /* Drag highlight - amber tint for log mode, blue for linear */
        if (variable_panel_drag_active_var() == i) {
            if (variable_panel_drag_log_mode())
                glColor4f(0.30f, 0.20f, 0.05f, 0.60f);
            else
                glColor4f(0.20f, 0.20f, 0.40f, 0.60f);
            glRectf((float)(px + 1), (float)row_y, (float)(px + 1) + (float)(pw - 2), (float)row_y + (float)VAR_ROW_H);
        }

        /* Label */
        glColor3f(0.70f, 0.85f, 0.70f);
        gl2d_draw_string((float)label_x, (float)text_y,
                    g_predef_vars[i].name, FONT_SMALL);

        /* Value */
        char valstr[16]; snprintf(valstr, sizeof(valstr), "%7.3f", (double)val);
        glColor3f(0.90f, 0.90f, 0.60f);
        gl2d_draw_string((float)val_x, (float)text_y, valstr, FONT_SMALL);

        /* Slider track */
        glColor4f(0.18f, 0.18f, 0.28f, 0.90f);
        glRectf((float)track_x, (float)(row_y + 6), (float)track_x + (float)track_w, (float)(row_y + 6) + (float)(VAR_ROW_H - 12));

        /* Centre tick (marks zero on the log scale) */
        float cx = (float)track_x + (float)track_w * 0.5f;
        glColor4f(0.35f, 0.35f, 0.50f, 0.70f);
        glBegin(GL_LINES);
        glVertex2f(cx, (float)(row_y + 5));
        glVertex2f(cx, (float)(row_y + VAR_ROW_H - 5));
        glEnd();

        /* Handle - position computed via shared log-normalized scale.
         * Yellow = linear drag, orange = log drag, blue = idle. */
        float t  = val_to_slider_t(val, log_scale);
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (variable_panel_drag_active_var() == i) {
            if (variable_panel_drag_log_mode())
                glColor4f(1.00f, 0.55f, 0.10f, 0.95f);  /* orange: log mode */
            else
                glColor4f(1.00f, 0.80f, 0.20f, 0.95f);  /* yellow: linear mode */
        } else {
            glColor4f(0.55f, 0.70f, 1.00f, 0.90f);      /* blue: idle */
        }
        glRectf(hx, (float)(row_y + 4), hx + (float)handle_w, (float)(row_y + 4) + (float)(VAR_ROW_H - 8));
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
