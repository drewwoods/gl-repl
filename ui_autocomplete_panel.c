/*
 * repl_autocomplete_panel.c -- Floating autocomplete popup.
 *
 * Pure renderer: reads match_count / matches / selected_idx from the
 * autocomplete model (repl_autocomplete.c) and the cursor pixel
 * position (cursor_px/cursor_py), and draws.  Selection
 * mutation, match building, and hint text all live outside.
 *
 * The ghost-text and parameter-hint overlays drawn inline next to
 * the input line stay in ui_panels.c where the code panel's row
 * layout has the surrounding context.
 */
#include "sample.h"
#include "repl_state.h"
#include "ui_autocomplete_panel.h"
#include "ui_panels.h"

void ui_autocomplete_panel_render(void) {
    const ReplAutocompleteState      *ac  = repl_state_autocomplete();
    const ReplCodePanelRuntimeState  *cp  = repl_state_code_panel();

    if (*ac->match_count < 1) return;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int popup_x = *cp->cursor_px;
    int popup_y = *cp->cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match */
    int max_w = 0;
    for (int i = 0; i < *ac->match_count; i++) {
        int w = (int)strlen(ac->matches[i]) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int popup_w = max_w + 16;
    int popup_h = *ac->match_count * LINE_H + 6;

    /* Clamp to code panel width */
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (popup_x + popup_w > cp_x + cp_w - 4)
        popup_x = cp_x + cp_w - popup_w - 4;
    if (popup_x < cp_x + 4) popup_x = cp_x + 4;

    /* Background */
    glColor4f(0.08f, 0.08f, 0.15f, 0.95f);
    draw_quad((float)popup_x, (float)(popup_y - popup_h),
              (float)popup_w, (float)popup_h);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.65f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)popup_x, (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)popup_y);
    glVertex2f((float)popup_x, (float)popup_y);
    glEnd();

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = 0; i < *ac->match_count; i++) {
        if (i == *ac->selected_idx) {
            /* Highlight selected */
            glColor4f(0.20f, 0.25f, 0.42f, 0.90f);
            draw_quad((float)(popup_x + 1), (float)(ey - 2),
                      (float)(popup_w - 2), (float)LINE_H);
            glColor3f(1.0f, 1.0f, 0.90f);
        } else {
            glColor3f(0.65f, 0.65f, 0.72f);
        }
        draw_string((float)(popup_x + 8), (float)ey,
                    ac->matches[i], FONT_MONO);
        ey -= LINE_H;
    }

    /* Hint text */
    glColor4f(0.45f, 0.45f, 0.55f, 0.70f);
    draw_string((float)(popup_x + 4),
                (float)(popup_y - popup_h - FONT_H - 2),
                "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}
