/*
 * ui_tabbed_overlay.c -- Modal tabbed text overlay renderer.
 *
 * Generic two-column tabbed reference card. See ui_tabbed_overlay.h
 * for the input struct and column-split convention.
 */
#include "tabbed_overlay.h"
#include "gl_2d.h"
#include "metrics.h"

#include "config.h"

#include <stdio.h>
#include <string.h>

void ui_tabbed_overlay_render(const UiOverlayState *in) {
    if (!in || !in->visible) return;
    if (!in->content || in->content->tab_count <= 0) return;

    const UiOverlayContent *content = in->content;
    int num_tabs = content->tab_count;

    int tab_idx = in->tab_idx;
    int scroll  = in->scroll;
    if (tab_idx < 0) tab_idx = 0;
    if (tab_idx >= num_tabs) tab_idx = num_tabs - 1;

    const char *const *text = content->tabs[tab_idx].lines;
    if (!text) return;

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    int win_w = in->viewport_w;
    int win_h = in->viewport_h;
    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hx = win_w / 6, hy = win_h / 12;
    int hw = win_w * 2 / 3, hh = win_h * 5 / 6;
    int tab_bar_h = LINE_H + 2;
    int title_h   = LINE_H + 4;
    int pad_top   = title_h + tab_bar_h + 6;
    int pad_bot   = 20;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;

    /* Background - matches config menu #222 */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f);
    glRectf((float)hx, (float)hy, (float)hx + (float)hw, (float)hy + (float)hh);

    /* Border - matches config menu #3a3a3a */
    glColor4f(0.227f, 0.227f, 0.227f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx,        (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx,        (float)(hy + hh));
    glEnd();

    /* --- Title bar --- */
    {
        int title_y = hy + hh - title_h;
        /* Title bar separator */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)title_y);
        glVertex2f((float)(hx + hw), (float)title_y);
        glEnd();

        /* Title text - dim, left-aligned like config menu section headers */
        const char *title = content->title ? content->title : "";
        glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
        gl2d_draw_string((float)(hx + 14), (float)(title_y + 4), title, FONT_SMALL);

        /* Tab switch hint right-aligned */
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * FONT_SMALL_W - 14;
        glColor4f(0.533f, 0.533f, 0.533f, 0.70f);
        gl2d_draw_string((float)nh_x, (float)(title_y + 4), nav_hint, FONT_SMALL);
    }

    /* --- Tab bar --- */
    {
        int tab_y  = hy + hh - title_h - tab_bar_h;
        int tab_w  = hw / num_tabs;

        /* Tab bar background */
        glColor4f(0.10f, 0.10f, 0.10f, 1.0f);
        glRectf((float)hx, (float)tab_y, (float)hx + (float)hw, (float)tab_y + (float)tab_bar_h);

        for (int t = 0; t < num_tabs; t++) {
            int tx_tab = hx + t * tab_w;
            if (t == tab_idx) {
                /* Active tab: bottom accent bar + bright label */
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.85f);
                glRectf((float)tx_tab, (float)tab_y, (float)tx_tab + (float)tab_w, (float)tab_y + 2.0f);
                glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            } else {
                glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            }
            const char *label = content->tabs[t].label ? content->tabs[t].label : "";
            int lbl_len = (int)strlen(label);
            int lbl_x   = tx_tab + (tab_w - lbl_len * FONT_SMALL_W) / 2;
            gl2d_draw_string((float)lbl_x, (float)(tab_y + 3), label, FONT_SMALL);
        }

        /* Separator line below tab bar */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)tab_y);
        glVertex2f((float)(hx + hw), (float)tab_y);
        glEnd();
    }

    /* --- Content --- */
    {
        int scissor_x = hx + 1;
        int scissor_y = hy + pad_bot;
        int scissor_w = hw - 2;
        int scissor_h = content_h;
        int have_scissor = (scissor_w > 0 && scissor_h > 0);

        if (scissor_w < 0) scissor_w = 0;
        if (scissor_h < 0) scissor_h = 0;

        if (have_scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    int tx       = hx + 14;
    int ty_start = hy + hh - pad_top - LINE_H + 3;

    /* Compute tab stop from widest left column so all right columns align */
    int tab_stop = 0;
    for (int i = 0; i < n_lines; i++) {
        const char *t = strchr(text[i], '\t');
        if (t) {
            int ln = (int)(t - text[i]);
            if (ln > tab_stop) tab_stop = ln;
        }
    }

    for (int i = scroll; i < n_lines && i < scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;
        if (text[i][0] == '\0') continue;

        /* '\t' marks the left/right column boundary */
        const char *tab = strchr(text[i], '\t');
        if (tab) {
            /* Left column - #d8d8d8 */
            char left[256];
            int ln = (int)(tab - text[i]);
            if (ln > 255) ln = 255;
            memcpy(left, text[i], ln);
            left[ln] = '\0';
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, left, FONT_SMALL);

            /* Right column - aligned to shared tab stop */
            glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            gl2d_draw_string((float)(tx + tab_stop * FONT_SMALL_W), (float)ty,
                        tab + 1, FONT_SMALL);
        } else if (text[i][0] != ' ') {
            /* Section header - dim gray-blue like config menu */
            glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent - code example, green accent */
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.90f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else {
            /* 2-space indent, no split - light label colour */
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x   = hx + hw - 8;
        int bar_top = hy + hh - pad_top;
        int bar_h   = content_h;
        float frac  = (float)visible_lines / (float)n_lines;
        float pos   = (float)scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track - #333 */
        glColor4f(0.20f, 0.20f, 0.20f, 0.60f);
        glRectf((float)bar_x, (float)(bar_top - bar_h), (float)bar_x + 4.0f, (float)(bar_top - bar_h) + (float)bar_h);

        /* Thumb - #888 */
        glColor4f(0.533f, 0.533f, 0.533f, 0.80f);
        glRectf((float)bar_x, (float)thumb_y, (float)bar_x + 4.0f, (float)thumb_y + (float)thumb_h);

        /* Scroll hint at bottom */
        if (scroll < max_scroll) {
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_SMALL_W) / 2;
            glColor4f(0.533f, 0.533f, 0.533f, 0.50f);
            gl2d_draw_string((float)hint_x, (float)(hy + 4), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
