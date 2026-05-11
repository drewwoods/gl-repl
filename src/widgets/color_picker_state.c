/*
 * color_picker_state.c - Floating color-picker peer (state + lifecycle + writeback).
 */
#include "widgets/color_picker_state.h"

#include "config.h"               /* CP_CLEAR_MAX_V */
#include "editor/commit.h"
#include "repl/command.h"
#include "repl/compile.h"
#include "repl/core.h"             /* set_status, MAX_LINE_LEN */
#include "repl/parser.h"
#include "ui/layout.h"
#include "ui/state.h"

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

/* g_cp_line >= 0: picker is open for that source-cmd index */
static int   g_cp_line       = -1;
static float g_cp_hue        = 0.0f, g_cp_sat = 1.0f, g_cp_val = 1.0f;
static float g_cp_alpha      = 1.0f;
static int   g_cp_px         = 0, g_cp_py = 0;  /* top-left (OpenGL y-up) */
static int   g_cp_drag       = 0;    /* 0=none 1=SV 2=hue 3=alpha */
static int   g_cp_has_alpha  = 0;    /* RGBA-shaped command */
static float g_cp_value_max  = 1.0f;
/* Picker session undo bookkeeping: the very first writeback after open
 * (or after switching the active line) captures one undo snapshot via
 * editor_commit_apply_external_change(capture_undo=1). Subsequent
 * slider drags within the same session pass capture_undo=0 so the
 * undo ring records "before this picker session" state, not every
 * intermediate slider tick. */
static int   g_cp_undo_captured = 0;

static void cp_compute_rects(ColorPickerRects *r) {
    int px = g_cp_px;
    int py = g_cp_py;
    int sz = CP_SV_SZ;
    int hx = px + sz + CP_GAP;

    r->sv_x = px;          r->sv_y = py - sz; r->sv_sz = sz;
    r->hue_x = hx;         r->hue_y = py - sz; r->hue_w = CP_HUE_W; r->hue_h = sz;
    r->alp_x = hx + CP_HUE_W + CP_GAP;
    r->alp_y = py - sz;
    r->alp_w = CP_ALPHA_W;
    r->alp_h = sz;
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

static const GLCmd *cp_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return NULL;
    return repl_state_document_cmd_at(cmd_idx);
}

/* Push the current slider state through the editor commit pipeline,
 * synthesizing the appropriate glColor* / glClearColor / gluColor
 * source line. Returns 1 if the writeback fired (state actually
 * mutated), 0 if it short-circuited. */
static int color_picker_write_cmd(void) {
    const GLCmd *cmd = cp_cmd_at(g_cp_line);
    float r, g, b;
    char new_line[MAX_LINE_LEN];
    int written;

    if (!cmd)
        return 0;

    color_picker_hsv_to_rgb(g_cp_hue, g_cp_sat, g_cp_val, &r, &g, &b);
    if (cmd->type == CMD_CLEAR_COLOR) {
        if (r > CP_CLEAR_MAX_V) r = CP_CLEAR_MAX_V;
        if (g > CP_CLEAR_MAX_V) g = CP_CLEAR_MAX_V;
        if (b > CP_CLEAR_MAX_V) b = CP_CLEAR_MAX_V;
        written = snprintf(new_line, sizeof(new_line),
                           "glClearColor(%g, %g, %g, %g);",
                           r, g, b, g_cp_alpha);
    } else if (cmd->type == CMD_COLOR3F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor3f(%g, %g, %g);", r, g, b);
    } else if (cmd->type == CMD_COLOR4F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor4f(%g, %g, %g, %g);",
                           r, g, b, g_cp_alpha);
    } else if (cmd->type == CMD_TESS_COLOR) {
        if (g_cp_has_alpha)
            written = snprintf(new_line, sizeof(new_line),
                               "gluColor(%g, %g, %g, %g);",
                               r, g, b, g_cp_alpha);
        else
            written = snprintf(new_line, sizeof(new_line),
                               "gluColor(%g, %g, %g);", r, g, b);
    } else {
        return 0;
    }

    if (written < 0 || written >= (int)sizeof(new_line)) {
        repl_set_status("Command too long");
        return 0;
    }

    /* Color picker writeback: surface parser errors so a malformed
     * rewrite (shouldn't happen for synthesized glColor commands but
     * defends the path) shows in the status bar instead of failing
     * silently. */
    char picker_parse_err[REPL_STATUS_TEXT_MAX];
    picker_parse_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = g_cp_line,
        .err_buf = picker_parse_err,
        .err_sz  = (int)sizeof(picker_parse_err),
    };
    ReplParsedLine pl;
    if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx)) {
        if (picker_parse_err[0])
            repl_set_status(picker_parse_err);
        return 0;
    }

    /* Route the cmd-store + editor-buffer writes through the editor
     * commit pipeline so the picker is a pure value-emitter and stays
     * out of the UI input boundary. The first writeback of a session
     * captures undo; subsequent drags pass capture_undo=0 so each
     * session lands as one undo entry. */
    ReplCompiledChange change;
    repl_compiled_change_init(&change);
    change.kind  = REPL_COMPILED_REPLACE_ONE;
    change.pos   = g_cp_line;
    change.count = 1;
    change.cmds[0] = pl.cmd;
    int text_len = (int)strlen(pl.text);
    if (text_len >= MAX_LINE_LEN) text_len = MAX_LINE_LEN - 1;
    memcpy(change.text[0], pl.text, (size_t)text_len);
    change.text[0][text_len] = '\0';

    if (editor_commit_apply_external_change(&change, !g_cp_undo_captured)) {
        g_cp_undo_captured = 1;
        return 1;
    }
    return 0;
}

void color_picker_open(int cmd_idx, int my) {
    int cp_x, cp_w;
    const GLCmd *cmd;

    if (!color_picker_can_edit_cmd(cmd_idx))
        return;

    /* New session (first open, or switching to a different line):
     * arm the next writeback to capture undo. Editing the same line
     * after a close-then-reopen is treated as a new session — matches
     * the legacy behaviour where every open-on-different-line pushed
     * undo, except the snapshot now records the moment of the first
     * actual mutation rather than the moment of open. */
    if (g_cp_line != cmd_idx)
        g_cp_undo_captured = 0;

    ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    g_cp_line = cmd_idx;
    cmd = cp_cmd_at(cmd_idx);
    if (!cmd)
        return;
    g_cp_has_alpha = (cmd->type == CMD_COLOR4F ||
                      cmd->type == CMD_TESS_COLOR ||
                      cmd->type == CMD_CLEAR_COLOR);
    g_cp_alpha     = g_cp_has_alpha ? cmd->args[3] : 1.0f;
    g_cp_value_max = (cmd->type == CMD_CLEAR_COLOR) ? CP_CLEAR_MAX_V : 1.0f;
    cp_rgb_to_hsv(cmd->args[0], cmd->args[1], cmd->args[2],
                  &g_cp_hue, &g_cp_sat, &g_cp_val);
    if (g_cp_val > g_cp_value_max)
        g_cp_val = g_cp_value_max;

    /* Position to the right of the panel, near the click y. */
    int pw = CP_SV_SZ + CP_GAP + CP_HUE_W
           + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0) + CP_GAP;
    int ph = CP_SV_SZ + CP_GAP + CP_PREV_H + CP_GAP;
    int ppx = cp_x + cp_w + 8;
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;
    if (ppx + pw > win_w - 4) ppx = cp_x - pw - 4;
    if (ppx < 4) ppx = 4;
    if (ppx + pw > win_w - 4) ppx = win_w - pw - 4;
    if (ppx < 4) ppx = 4;
    int ppy = (win_h - my) + ph / 2;
    if (ppy > win_h - 4) ppy = win_h - 4;
    if (ppy - ph < 4)    ppy = ph + 4;
    g_cp_px = ppx;
    g_cp_py = ppy;
}

int color_picker_close(void) {
    if (g_cp_line < 0) return 0;
    g_cp_line = -1;
    g_cp_drag = 0;
    g_cp_undo_captured = 0;
    return 1;
}

int color_picker_active_line(void) {
    return g_cp_line;
}

int color_picker_can_edit_cmd(int cmd_idx) {
    const GLCmd *cmd = cp_cmd_at(cmd_idx);
    if (!cmd || !cmd->valid || cmd->has_vars)
        return 0;
    return cmd->type == CMD_COLOR3F ||
           cmd->type == CMD_COLOR4F ||
           cmd->type == CMD_TESS_COLOR ||
           cmd->type == CMD_CLEAR_COLOR;
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
    cp_compute_rects(&v.rects);
    return v;
}

ColorPickerInputResult color_picker_handle_press(int mx, int my) {
    ColorPickerInputResult res = { 0, 0, 0 };
    const GLCmd *cmd;
    ColorPickerRects r;

    if (g_cp_line < 0) return res;
    cmd = cp_cmd_at(g_cp_line);
    if (!cmd) return res;
    int gl_y = ui_state_viewport().window_h - my;
    cp_compute_rects(&r);

    /* SV square */
    if (mx >= r.sv_x && mx < r.sv_x + r.sv_sz &&
        gl_y >= r.sv_y && gl_y < r.sv_y + r.sv_sz) {
        g_cp_drag = 1;
        g_cp_sat = (float)(mx - r.sv_x) / (float)r.sv_sz;
        g_cp_val = (float)(gl_y - r.sv_y) / (float)r.sv_sz;
        if (g_cp_sat < 0) g_cp_sat = 0; if (g_cp_sat > 1) g_cp_sat = 1;
        if (g_cp_val < 0) g_cp_val = 0; if (g_cp_val > 1) g_cp_val = 1;
        if (g_cp_val > g_cp_value_max) g_cp_val = g_cp_value_max;
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }
    /* Hue bar */
    if (mx >= r.hue_x && mx < r.hue_x + r.hue_w &&
        gl_y >= r.hue_y && gl_y < r.hue_y + r.hue_h) {
        g_cp_drag = 2;
        g_cp_hue = 1.0f - (float)(gl_y - r.hue_y) / (float)r.hue_h;
        if (g_cp_hue < 0) g_cp_hue = 0;
        if (g_cp_hue >= 1) g_cp_hue = 0.999f;
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }
    /* Alpha bar (RGBA-shaped only) */
    if (g_cp_has_alpha &&
        mx >= r.alp_x && mx < r.alp_x + r.alp_w &&
        gl_y >= r.alp_y && gl_y < r.alp_y + r.alp_h) {
        g_cp_drag = 3;
        g_cp_alpha = (float)(gl_y - r.alp_y) / (float)r.alp_h;
        if (g_cp_alpha < 0) g_cp_alpha = 0;
        if (g_cp_alpha > 1) g_cp_alpha = 1;
        res.changed = color_picker_write_cmd();
        res.consumed = 1;
        return res;
    }

    /* Click outside the picker: dismiss and let the event flow through. */
    g_cp_line = -1;
    g_cp_drag = 0;
    g_cp_undo_captured = 0;
    res.closed = 1;
    return res;
}

ColorPickerInputResult color_picker_handle_motion(int mx, int my) {
    ColorPickerInputResult res = { 0, 0, 0 };
    const GLCmd *cmd;
    ColorPickerRects r;

    if (g_cp_drag == 0) return res;
    cmd = cp_cmd_at(g_cp_line);
    if (!cmd) return res;
    int gl_y = ui_state_viewport().window_h - my;
    cp_compute_rects(&r);
    if (g_cp_drag == 1) {
        g_cp_sat = (float)(mx - r.sv_x) / (float)r.sv_sz;
        g_cp_val = (float)(gl_y - r.sv_y) / (float)r.sv_sz;
        if (g_cp_sat < 0) g_cp_sat = 0; if (g_cp_sat > 1) g_cp_sat = 1;
        if (g_cp_val < 0) g_cp_val = 0; if (g_cp_val > 1) g_cp_val = 1;
        if (g_cp_val > g_cp_value_max) g_cp_val = g_cp_value_max;
    } else if (g_cp_drag == 2) {
        g_cp_hue = 1.0f - (float)(gl_y - r.hue_y) / (float)r.hue_h;
        if (g_cp_hue < 0) g_cp_hue = 0;
        if (g_cp_hue >= 1) g_cp_hue = 0.999f;
    } else if (g_cp_drag == 3) {
        g_cp_alpha = (float)(gl_y - r.alp_y) / (float)r.alp_h;
        if (g_cp_alpha < 0) g_cp_alpha = 0;
        if (g_cp_alpha > 1) g_cp_alpha = 1;
    }
    res.changed = color_picker_write_cmd();
    res.consumed = 1;
    return res;
}

void color_picker_handle_release(void) {
    g_cp_drag = 0;
}
