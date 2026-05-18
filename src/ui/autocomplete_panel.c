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
#include "autocomplete_panel.h"
#include "layout.h"
#include "metrics.h"
#include "theme.h"

#include "gl_2d.h"

void ui_autocomplete_panel_render(const UiRenderSnapshot *snap,
                                  int cursor_px, int cursor_py) {
    EditorAutocompleteState           ac  = snap->autocomplete;

    if (ac.match_count < 1) return;

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int popup_x = cursor_px;
    int popup_y = cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match */
    int max_w = 0;
    for (int i = 0; i < ac.match_count; i++) {
        int w = (int)strlen(ac.matches[i]) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int popup_w = max_w + 16;
    int popup_h = ac.match_count * LINE_H + 6;

    /* Clamp to code panel width */
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (popup_x + popup_w > cp_x + cp_w - 4)
        popup_x = cp_x + cp_w - popup_w - 4;
    if (popup_x < cp_x + 4) popup_x = cp_x + 4;

    /* Background */
    ui_clr_a(UI_TOK_SUNKEN, 0.95f);
    glRectf((float)((float)popup_x), (float)((float)(popup_y - popup_h)), (float)((float)popup_x)+(float)((float)popup_w), (float)((float)(popup_y - popup_h))+(float)((float)popup_h));

    /* Border */
    ui_clr_a(UI_TOK_BORDER, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)popup_x, (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)popup_y);
    glVertex2f((float)popup_x, (float)popup_y);
    glEnd();

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = 0; i < ac.match_count; i++) {
        if (i == ac.selected_idx) {
            /* Highlight selected (accent selection band) */
            ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, 0.90f);
            glRectf((float)((float)(popup_x + 1)), (float)((float)(ey - 2)), (float)((float)(popup_x + 1))+(float)((float)(popup_w - 2)), (float)((float)(ey - 2))+(float)((float)LINE_H));
            ui_clr(UI_TOK_TEXT_ON_HILITE);
        } else {
            ui_clr(UI_TOK_TEXT_MUTED);
        }
        gl2d_draw_string((float)(popup_x + 8), (float)ey,
                    ac.matches[i], FONT_MONO);
        ey -= LINE_H;
    }

    /* Hint text */
    ui_clr_a(UI_TOK_TEXT_MUTED, 0.70f);
    gl2d_draw_string((float)(popup_x + 4),
                (float)(popup_y - popup_h - FONT_H - 2),
                "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
}
