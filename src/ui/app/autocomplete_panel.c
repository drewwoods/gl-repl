/*
 * ui_autocomplete_panel.c -- Floating autocomplete popup.
 *
 * Pure renderer: reads match_count / matches / selected_idx from the
 * autocomplete model and takes the cursor pixel position from
 * ui_panels_render_code_panel's per-frame UiCodePanelOutput, then
 * draws. Selection mutation, match building, and hint text all live
 * outside.
 *
 * The ghost-text and parameter-hint overlays drawn inline next to
 * the input line stay in ui_panels.c where the code panel's row
 * layout has the surrounding context.
 */
#include <string.h>

#include "ui/app/autocomplete_panel.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include "ui/core/gl_2d.h"

void ui_autocomplete_panel_rect(const EditorAutocompleteState *ac,
                                int cursor_px, int cursor_py,
                                int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!ac || ac->match_count < 1) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    int count = ac->match_count;
    if (count > MAX_AC_MATCHES) count = MAX_AC_MATCHES;

    int popup_x = cursor_px;
    int popup_y = cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match across all candidates */
    int max_w = 0;
    for (int i = 0; i < count; i++) {
        if (ac->matches[i]) {
            int w = (int)strlen(ac->matches[i]) * FONT_W;
            if (w > max_w) max_w = w;
        }
    }
    int scrollbar_pad = (count > MAX_AC_VISIBLE) ? 8 : 0;
    int popup_w = max_w + 16 + scrollbar_pad;
    int visible_count = (count > MAX_AC_VISIBLE) ? MAX_AC_VISIBLE : count;
    int popup_h = visible_count * LINE_H + 6;

    /* Clamp to code panel width */
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (popup_x + popup_w > cp_x + cp_w - 4)
        popup_x = cp_x + cp_w - popup_w - 4;
    if (popup_x < cp_x + 4) popup_x = cp_x + 4;

    int total_h = popup_h + FONT_H + 4;
    int bottom_y = popup_y - total_h;

    if (out_x) *out_x = popup_x;
    if (out_y) *out_y = bottom_y;
    if (out_w) *out_w = popup_w;
    if (out_h) *out_h = total_h;
}

int ui_autocomplete_panel_hit_test(const EditorAutocompleteState *ac,
                                   int cursor_px, int cursor_py,
                                   int gl_x, int gl_y) {
    int rx, ry, rw, rh;
    ui_autocomplete_panel_rect(ac, cursor_px, cursor_py, &rx, &ry, &rw, &rh);
    return (rw > 0 && rh > 0 &&
            gl_x >= rx && gl_x < rx + rw &&
            gl_y >= ry && gl_y < ry + rh);
}

void ui_autocomplete_panel_render(const UiRenderSnapshot *snap,
                                  int cursor_px, int cursor_py) {
    EditorAutocompleteState ac = snap->autocomplete;

    if (ac.match_count < 1) return;

    int popup_x, bottom_y, popup_w, total_h;
    ui_autocomplete_panel_rect(&ac, cursor_px, cursor_py,
                               &popup_x, &bottom_y, &popup_w, &total_h);
    if (popup_w <= 0 || total_h <= 0) return;

    int hint_band_h = FONT_H + 4;
    int popup_h = total_h - hint_band_h;
    int popup_y = bottom_y + total_h;

    int count = ac.match_count > MAX_AC_MATCHES ? MAX_AC_MATCHES : ac.match_count;
    int visible_count = (count > MAX_AC_VISIBLE) ? MAX_AC_VISIBLE : count;

    int max_scroll = count - visible_count;
    int scroll = ac.scroll_top;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background + Border */
    gl2d_panel_frame((float)popup_x, (float)(popup_y - popup_h),
                     (float)popup_w, (float)popup_h,
                     UI_TOK_SUNKEN, 0.95f, UI_TOK_BORDER, 0.80f);

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = scroll; i < scroll + visible_count; i++) {
        if (i == ac.selected_idx) {
            /* Highlight selected (accent selection band) */
            ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, 0.90f);
            glRectf((float)(popup_x + 1), (float)(ey - 2),
                    (float)(popup_x + popup_w - 2), (float)(ey - 2 + LINE_H));
            ui_clr(UI_TOK_TEXT_ON_HILITE);
        } else {
            ui_clr(UI_TOK_TEXT_MUTED);
        }
        if (ac.matches[i]) {
            gl2d_draw_string((float)(popup_x + 8), (float)ey,
                             ac.matches[i], FONT_MONO);
        }
        ey -= LINE_H;
    }

    /* Overflow scrollbar hint matching menu_bar.c flyout styling */
    if (count > visible_count) {
        float bx1       = (float)(popup_x + popup_w - 2);
        float bx0       = bx1 - 3.0f;
        float track_top = (float)(popup_y - 2);
        float track_bot = (float)(popup_y - popup_h + 2);
        float track_h   = track_top - track_bot;
        float thumb_h   = track_h * (float)visible_count / (float)count;
        float max_off, thumb_top;
        if (thumb_h < 10.0f)   thumb_h = 10.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        max_off   = track_h - thumb_h;
        thumb_top = track_top -
                    max_off * (float)scroll / (float)(count - visible_count);
        ui_clr_a(UI_TOK_DIVIDER, 0.90f);
        glRectf(bx0, track_bot, bx1, track_top);
        ui_clr_a(UI_TOK_TEXT_MUTED, 0.90f);
        glRectf(bx0, thumb_top - thumb_h, bx1, thumb_top);
    }

    /* Hint text */
    ui_clr_a(UI_TOK_TEXT_MUTED, 0.70f);
    gl2d_draw_string((float)(popup_x + 4),
                     (float)(popup_y - popup_h - FONT_H - 2),
                     "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}
