/*
 * ui_panels.c - Code-panel delegation, scene status banner, and top-level
 * overlay-priority hit routing.
 */
#include "ui/panels.h"

#include "ui/color_picker.h"
#include "ui/gl_2d.h"
#include "ui/layout.h"
#include "ui/menu_bar.h"
#include "ui/metrics.h"
#include "ui/repl_code_panel.h"
#include "ui/scene_tabs.h"
#include "ui/variable_panel.h"

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
    ui_repl_code_panel_render(snap, out);
}

void ui_panels_render_scene_status(const UiRenderSnapshot *snap) {
    /* Inline scene rename owns its own display state and takes
     * precedence over the transient status line. Drawn solid (no TTL
     * fade) in a distinct blue — visually unmistakable as a modal,
     * and unaffected by other repl_set_status() writers. */
    if (snap->rename_active) {
        int sc_x, sc_y, sc_w, sc_h;
        int bar_h, bar_y, text_y, tx, max_px, max_chars, n;
        char msg[256];

        ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
        if (sc_w <= 0 || sc_h <= 0)
            return;

        bar_h = STATUSBAR_H;
        bar_y = sc_y;

        gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glColor4fv(k_rename_bar_bg);  /* deep blue modal bar */
        glRectf((float)sc_x, (float)bar_y,
                (float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glColor4fv(k_rename_bar_rule);  /* modal rule */
        glBegin(GL_LINES);
        glVertex2f((float)sc_x, (float)(bar_y + bar_h));
        glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glEnd();

        text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;
        tx = sc_x + CODE_MARGIN_X;
        max_px = sc_w - 2 * CODE_MARGIN_X;
        max_chars = max_px / FONT_SMALL_W;
        if (max_chars < 8)
            max_chars = 8;
        if (max_chars > 255)
            max_chars = 255;
        n = snprintf(msg, sizeof(msg),
                     "Rename: %s_   [Enter] save   [Esc] cancel",
                     snap->rename_text);
        if (n > max_chars)
            msg[max_chars] = '\0';
        glColor4fv(k_rename_bar_text);  /* light blue modal text */
        gl2d_draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }

    /* Inline file prompt reuses the rename modal's bar geometry +
     * colors — same hard-modal contract, same display ownership; only
     * the prompt text differs. */
    if (snap->file_prompt_active) {
        int sc_x, sc_y, sc_w, sc_h;
        int bar_h, bar_y, text_y, tx, max_px, max_chars, n;
        char msg[320];

        ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
        if (sc_w <= 0 || sc_h <= 0)
            return;

        bar_h = STATUSBAR_H;
        bar_y = sc_y;

        gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glColor4fv(k_rename_bar_bg);
        glRectf((float)sc_x, (float)bar_y,
                (float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glColor4fv(k_rename_bar_rule);
        glBegin(GL_LINES);
        glVertex2f((float)sc_x, (float)(bar_y + bar_h));
        glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glEnd();

        text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;
        tx = sc_x + CODE_MARGIN_X;
        max_px = sc_w - 2 * CODE_MARGIN_X;
        max_chars = max_px / FONT_SMALL_W;
        if (max_chars < 8)
            max_chars = 8;
        if (max_chars > (int)sizeof(msg) - 1)
            max_chars = (int)sizeof(msg) - 1;
        /* When the most recent commit failed, swap the hint for the
         * error string. The prompt strip occludes the regular status
         * bar, so this is the only place a user can see the failure
         * reason without dismissing the prompt. */
        if (snap->file_prompt_error[0])
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   %s   [Esc] cancel",
                         snap->file_prompt_text, snap->file_prompt_error);
        else
            n = snprintf(msg, sizeof(msg),
                         "Load scene: %s_   [Enter] load   [Esc] cancel",
                         snap->file_prompt_text);
        if (n > max_chars)
            msg[max_chars] = '\0';
        glColor4fv(k_rename_bar_text);
        gl2d_draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }

    UiStatusState status = snap->status;
    if (status.ttl <= 0 || !status.text[0])
        return;

    {
        int sc_x;
        int sc_y;
        int sc_w;
        int sc_h;
        int bar_h;
        int bar_y;
        float alpha;
        int text_y;
        int badge_d;
        int badge_x;
        int badge_y;
        int tx;
        int max_px;
        int max_chars;
        char msg[256];
        int n;
        const float *bg;
        const float *edge;
        const float *fg;

        ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
        if (sc_w <= 0 || sc_h <= 0)
            return;

        bar_h = STATUSBAR_H;
        bar_y = sc_y;
        alpha = status.ttl > 60 ? 1.0f : (float)status.ttl / 60.0f;

        if (status.kind == UI_STATUS_ERROR) {
            bg   = k_status_bar_bg_err;
            edge = k_status_bar_edge_err;
            fg   = k_status_bar_fg_err;
        } else {
            bg   = k_status_bar_bg;
            edge = k_status_bar_edge;
            fg   = k_status_bar_fg;
        }

        gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glColor4f(bg[0], bg[1], bg[2], 0.92f * alpha);
        glRectf((float)sc_x, (float)bar_y,
                (float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glColor4f(edge[0], edge[1], edge[2], alpha);
        glBegin(GL_LINES);
        glVertex2f((float)sc_x, (float)(bar_y + bar_h));
        glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glEnd();

        text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;
        badge_d = 14;
        badge_x = sc_x + CODE_MARGIN_X;
        badge_y = bar_y + (bar_h - badge_d) / 2;
        glColor4f(fg[0], fg[1], fg[2], alpha);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 16; i++) {
            float angle = (float)i * (6.2831853f / 16.0f);
            glVertex2f(badge_x + badge_d * 0.5f + cosf(angle) * (badge_d * 0.5f),
                       badge_y + badge_d * 0.5f + sinf(angle) * (badge_d * 0.5f));
        }
        glEnd();
        gl2d_draw_string((float)(badge_x + badge_d * 0.5f - FONT_SMALL_W * 0.5f + 1.0f),
                         (float)text_y, "!", FONT_SMALL);

        tx = badge_x + badge_d + 8;
        max_px = sc_x + sc_w - CODE_MARGIN_X - tx;
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
        glColor4f(fg[0], fg[1], fg[2], alpha);
        gl2d_draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

        glDisable(GL_BLEND);
        gl2d_end();
    }
}

int ui_panels_handle_right_press(int mx, int my) {
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
        UiHit variable_hit = ui_variable_panel_hit_test(mx, my, variable_count);
        if (variable_hit.kind != UI_HIT_NONE)
            return variable_hit;
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