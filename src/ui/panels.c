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
#include "ui/state.h"
#include "ui/variable_panel.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out) {
    ui_repl_code_panel_render(snap, out);
}

int ui_panels_code_panel_apply_scroll_follow_for_test(int show_vertex_indices,
                                                      int *out_follow_doc_line,
                                                      int *out_visible_lines) {
    return ui_repl_code_panel_apply_scroll_follow_for_test(
        show_vertex_indices, out_follow_doc_line, out_visible_lines);
}

void ui_panels_render_scene_status(const UiRenderSnapshot *snap) {
    ReplStatusState status = snap->status;
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

        ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
        if (sc_w <= 0 || sc_h <= 0)
            return;

        bar_h = STATUSBAR_H;
        bar_y = sc_y;
        alpha = status.ttl > 60 ? 1.0f : (float)status.ttl / 60.0f;

        gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glColor4f(0.227f, 0.165f, 0.063f, 0.92f * alpha);
        glRectf((float)sc_x, (float)bar_y,
                (float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glColor4f(0.102f, 0.071f, 0.031f, alpha);
        glBegin(GL_LINES);
        glVertex2f((float)sc_x, (float)(bar_y + bar_h));
        glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
        glEnd();

        text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;
        badge_d = 14;
        badge_x = sc_x + CODE_MARGIN_X;
        badge_y = bar_y + (bar_h - badge_d) / 2;
        glColor4f(0.941f, 0.753f, 0.439f, alpha);
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
        glColor4f(0.941f, 0.753f, 0.439f, alpha);
        gl2d_draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

        glDisable(GL_BLEND);
        gl2d_end();
    }
}

int ui_panels_handle_right_press(int mx, int my) {
    return ui_menu_bar_handle_config_right_press(mx, my);
}

UiHit ui_panels_hit_test(int mx, int my, int variable_count) {
    UiHit hit = ui_hit_none();
    int win_w = ui_state_viewport().window_w;
    int win_h = ui_state_viewport().window_h;
    int gl_y;

    if (win_w <= 0 || win_h <= 0)
        return hit;

    gl_y = win_h - my;

    if (ui_state_help().visible) {
        hit.kind = UI_HIT_HELP_PANEL;
        hit.local_x = (float)mx;
        hit.local_y = (float)gl_y;
        return hit;
    }

    {
        ColorPickerView picker_view = color_picker_view();
        UiHit picker_hit = ui_color_picker_hit_test(&picker_view, mx, my, win_h);
        if (picker_hit.kind != UI_HIT_NONE)
            return picker_hit;
    }

    {
        UiHit menu_hit = ui_menu_bar_hit_test(mx, my);
        if (menu_hit.kind != UI_HIT_NONE)
            return menu_hit;
    }

    {
        UiHit variable_hit = ui_variable_panel_hit_test(mx, my, variable_count);
        if (variable_hit.kind != UI_HIT_NONE)
            return variable_hit;
    }

    {
        UiHit code_hit = ui_repl_code_panel_hit_test(mx, my);
        if (code_hit.kind != UI_HIT_NONE)
            return code_hit;
    }

    hit.kind = UI_HIT_SCENE;
    hit.local_x = (float)mx;
    hit.local_y = (float)gl_y;
    return hit;
}