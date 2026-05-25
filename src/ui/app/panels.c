/*
 * ui_panels.c - Code-panel delegation, scene status banner, and top-level
 * overlay-priority hit routing.
 */
#include "ui/app/panels.h"

#include "config.h"
#include "ui/app/color_picker.h"
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/scene_tabs.h"
#include "ui/app/variable_panel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Two deliberately non-accent semantic surfaces, theme-stable in every
 * scheme (see theme.h's "named constant" bucket - fixed, non-theme
 * one-offs that must NOT follow the UI accent):
 *   - the inline-rename modal: a distinct blue so it reads as modal;
 *   - the bottom status banner: amber, the conventional status hue. */
static const float k_rename_bar_bg[4]   = { 0.078f, 0.122f, 0.298f, 0.95f };
static const float k_rename_bar_rule[4] = { 0.310f, 0.510f, 0.860f, 1.0f };
static const float k_rename_bar_text[4] = { 0.780f, 0.870f, 1.000f, 1.0f };
static const float k_status_bar_bg[3]   = { 0.227f, 0.165f, 0.063f };
static const float k_status_bar_edge[3] = { 0.102f, 0.071f, 0.031f };
static const float k_status_bar_fg[3]   = { 0.941f, 0.753f, 0.439f };
/* Parallel red palette for UI_STATUS_ERROR. Same luminance band as the
 * amber set above so the bar still reads against the scene background;
 * only the hue differs. */
static const float k_status_bar_bg_err[3]   = { 0.290f, 0.078f, 0.078f };
static const float k_status_bar_edge_err[3] = { 0.137f, 0.027f, 0.027f };
static const float k_status_bar_fg_err[3]   = { 1.000f, 0.557f, 0.494f };

void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out) {
    ui_repl_code_panel_render_with_chrome(snap, out);
}

/* Geometry the bottom-bar strip helpers compute once at open. Callers
 * keep it on the stack across the begin / paint / extra-draws / close
 * sequence. Defined here so the three strip clients (modal-prompt
 * bar, status banner) all reach for the same shape. */
typedef struct {
    int sc_x, sc_y, sc_w, sc_h;
    int bar_y, bar_h;
    int text_y;
} StatusStripFrame;

/* Compute the strip geometry, set up gl2d + blend. Returns 0 (no
 * draw) on a degenerate scene rect — callers should bail without
 * a matching close, matching `gl2d_begin` lifetime. */
static int status_strip_begin(const UiRenderSnapshot *snap,
                              StatusStripFrame *f) {
    ui_layout_scene_rect(&f->sc_x, &f->sc_y, &f->sc_w, &f->sc_h);
    if (f->sc_w <= 0 || f->sc_h <= 0)
        return 0;
    f->bar_h  = STATUSBAR_H;
    f->bar_y  = f->sc_y;
    f->text_y = f->bar_y + (f->bar_h - FONT_SMALL_H) / 2 + 1;
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return 1;
}

/* Paint the strip background rectangle plus its top rule. Both
 * colors carry their own alpha. Caller can layer text / badges /
 * etc. afterward before `status_strip_end`. */
static void status_strip_paint_bar(const StatusStripFrame *f,
                                   const float bg[4],
                                   const float rule[4]) {
    glColor4fv(bg);
    glRectf((float)f->sc_x, (float)f->bar_y,
            (float)(f->sc_x + f->sc_w), (float)(f->bar_y + f->bar_h));
    glColor4fv(rule);
    glBegin(GL_LINES);
    glVertex2f((float)f->sc_x, (float)(f->bar_y + f->bar_h));
    glVertex2f((float)(f->sc_x + f->sc_w), (float)(f->bar_y + f->bar_h));
    glEnd();
}

static void status_strip_end(void) {
    glDisable(GL_BLEND);
    gl2d_end();
}

static void draw_modal_strip(const UiRenderSnapshot *snap,
                             const char *msg, int msg_len, int max_buf) {
    StatusStripFrame f;
    int tx, max_px, max_chars;
    char trunc[320];

    if (!status_strip_begin(snap, &f))
        return;
    status_strip_paint_bar(&f, k_rename_bar_bg, k_rename_bar_rule);

    tx = f.sc_x + CODE_MARGIN_X;
    max_px = f.sc_w - 2 * CODE_MARGIN_X;
    max_chars = max_px / FONT_SMALL_W;
    if (max_chars < 8)
        max_chars = 8;
    if (max_chars > max_buf - 1)
        max_chars = max_buf - 1;
    if (msg_len > max_chars) {
        snprintf(trunc, sizeof(trunc), "%.*s", max_chars, msg);
        msg = trunc;
    }
    glColor4fv(k_rename_bar_text);
    gl2d_draw_string((float)tx, (float)f.text_y, msg, FONT_SMALL);

    status_strip_end();
}

void ui_panels_render_scene_status(const UiRenderSnapshot *snap) {
    if (snap->rename_active) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "Rename: %s_   [Enter] save   [Esc] cancel",
                         snap->rename_text);
        draw_modal_strip(snap, msg, n, (int)sizeof(msg));
        return;
    }

    if (snap->file_prompt_active) {
        char msg[320];
        int n;
        if (snap->file_prompt_error[0])
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   %s   [Esc] cancel",
                         snap->file_prompt_text, snap->file_prompt_error);
        else
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   [Enter] load   [Esc] cancel",
                         snap->file_prompt_text);
        draw_modal_strip(snap, msg, n, (int)sizeof(msg));
        return;
    }

    UiStatusState status = snap->status;
    if (status.ttl <= 0 || !status.text[0])
        return;

    {
        StatusStripFrame f;
        float alpha;
        int badge_d, badge_x, badge_y;
        int tx, max_px, max_chars;
        char msg[256];
        int n;
        const float *bg_rgb;
        const float *edge_rgb;
        const float *fg_rgb;
        float bg[4], rule[4], fg[4];

        if (!status_strip_begin(snap, &f))
            return;

        alpha = status.ttl > REPL_STATUS_FADE_FRAMES
                    ? 1.0f
                    : (float)status.ttl / (float)REPL_STATUS_FADE_FRAMES;
        if (status.kind == UI_STATUS_ERROR) {
            bg_rgb   = k_status_bar_bg_err;
            edge_rgb = k_status_bar_edge_err;
            fg_rgb   = k_status_bar_fg_err;
        } else {
            bg_rgb   = k_status_bar_bg;
            edge_rgb = k_status_bar_edge;
            fg_rgb   = k_status_bar_fg;
        }
        bg[0] = bg_rgb[0]; bg[1] = bg_rgb[1]; bg[2] = bg_rgb[2]; bg[3] = 0.92f * alpha;
        rule[0] = edge_rgb[0]; rule[1] = edge_rgb[1]; rule[2] = edge_rgb[2]; rule[3] = alpha;
        fg[0] = fg_rgb[0]; fg[1] = fg_rgb[1]; fg[2] = fg_rgb[2]; fg[3] = alpha;

        status_strip_paint_bar(&f, bg, rule);

        badge_d = 14;
        badge_x = f.sc_x + CODE_MARGIN_X;
        badge_y = f.bar_y + (f.bar_h - badge_d) / 2;
        glColor4f(fg[0], fg[1], fg[2], fg[3]);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 16; i++) {
            float angle = (float)i * (6.2831853f / 16.0f);
            glVertex2f(badge_x + badge_d * 0.5f + cosf(angle) * (badge_d * 0.5f),
                       badge_y + badge_d * 0.5f + sinf(angle) * (badge_d * 0.5f));
        }
        glEnd();
        gl2d_draw_string((float)(badge_x + badge_d * 0.5f - FONT_SMALL_W * 0.5f + 1.0f),
                         (float)f.text_y, "!", FONT_SMALL);

        tx = badge_x + badge_d + 8;
        max_px = f.sc_x + f.sc_w - CODE_MARGIN_X - tx;
        max_chars = max_px / FONT_SMALL_W;
        if (max_chars < 8)
            max_chars = 8;
        if (max_chars > 255)
            max_chars = 255;

        n = (int)strlen(status.text);
        if (n > max_chars)
            snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, status.text);
        else
            snprintf(msg, sizeof(msg), "%s", status.text);
        glColor4f(fg[0], fg[1], fg[2], fg[3]);
        gl2d_draw_string((float)tx, (float)f.text_y, msg, FONT_SMALL);

        status_strip_end();
    }
}

UiHit ui_panels_handle_right_press(int mx, int my) {
    return ui_menu_bar_handle_config_right_press(mx, my);
}

UiHit ui_panels_hit_test(const UiRenderSnapshot *snap,
                         int mx, int my, int variable_count) {
    UiHit hit = ui_hit_none();
    int win_w;
    int win_h;
    int gl_y;

    if (!snap)
        return hit;

    win_w = snap->viewport.window_w;
    win_h = snap->viewport.window_h;
    if (win_w <= 0 || win_h <= 0)
        return hit;

    gl_y = win_h - my;

    if (snap->help.visible) {
        /* Help is modal, but keep the statusbar "F1 help" keycap live
         * so a second click dismisses the overlay (mirrors pressing
         * F1 again). Everything else stays captured by the panel. */
        UiHit code_hit = ui_repl_code_panel_hit_test(snap, mx, my);
        if (code_hit.kind == UI_HIT_HELP_TOGGLE)
            return code_hit;
        hit.kind = UI_HIT_HELP_PANEL;
        hit.local_x = (float)mx;
        hit.local_y = (float)gl_y;
        return hit;
    }

    {
        UiHit picker_hit = ui_color_picker_hit_test(&snap->color_picker,
                                                    mx, my, win_h);
        if (picker_hit.kind != UI_HIT_NONE)
            return picker_hit;
    }

    {
        UiHit menu_hit = ui_menu_bar_hit_test(mx, my);
        if (menu_hit.kind != UI_HIT_NONE)
            return menu_hit;
    }

    /* Claim the scene tab strip before the code panel: the band consumes
     * its whole rect (TAB on a tab, CHROME in-band off-tab) so blank-strip
     * clicks never fall through to a code-text/gutter hit. After the
     * menu-bar block so an open dropdown over the band still wins. */
    {
        UiHit tab_hit = ui_scene_tabs_hit_test(snap, mx, my);
        if (tab_hit.kind != UI_HIT_NONE)
            return tab_hit;
    }

    {
        UiHit variable_hit = ui_variable_panel_hit_test(snap, mx, my, variable_count);
        if (variable_hit.kind != UI_HIT_NONE)
            return variable_hit;
    }

    {
        UiHit swatch_hit = ui_numeric_swatch_hit_test(snap, mx, my);
        if (swatch_hit.kind != UI_HIT_NONE)
            return swatch_hit;
    }

    {
        UiHit code_hit = ui_repl_code_panel_hit_test(snap, mx, my);
        if (code_hit.kind != UI_HIT_NONE)
            return code_hit;
    }

    hit.kind = UI_HIT_SCENE;
    hit.local_x = (float)mx;
    hit.local_y = (float)gl_y;
    return hit;
}