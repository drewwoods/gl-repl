/*
 * ui_tabbed_overlay.c -- Modal tabbed text overlay renderer.
 *
 * Generic two-column tabbed reference card. See ui_tabbed_overlay.h
 * for the input struct and column-split convention.
 */
#include "tabbed_overlay.h"
#include "gl_2d.h"
#include "metrics.h"
#include "theme.h"

#include "config.h"

#include <stdio.h>
#include <string.h>

/* Layout geometry derived purely from viewport + active-tab line
 * count. The renderer and the input hit-test/clamp helpers both go
 * through overlay_compute_geom() so they can never disagree about
 * where the panel, tab bar, and scroll bounds actually are. */
typedef struct {
    int hx, hy, hw, hh;       /* panel rect (GL bottom-left origin) */
    int title_h, tab_bar_h;
    int pad_top, pad_bot;
    int content_h, visible_lines, max_scroll;
    int num_tabs, n_lines;
    int tab_y, tab_w;         /* tab-bar band + per-tab width */
} OverlayGeom;

static int overlay_active_tab_idx(const UiOverlayState *in) {
    int num_tabs = in->content->tab_count;
    int tab_idx = in->tab_idx;
    if (tab_idx < 0) tab_idx = 0;
    if (tab_idx >= num_tabs) tab_idx = num_tabs - 1;
    return tab_idx;
}

static int overlay_compute_geom(const UiOverlayState *in, OverlayGeom *g) {
    if (!in || !in->content || in->content->tab_count <= 0) return 0;
    int num_tabs = in->content->tab_count;
    const char *const *text =
        in->content->tabs[overlay_active_tab_idx(in)].lines;
    if (!text) return 0;

    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    int win_w = in->viewport_w;
    int win_h = in->viewport_h;
    g->hx = win_w / 6;     g->hy = win_h / 12;
    g->hw = win_w * 2 / 3; g->hh = win_h * 5 / 6;
    g->tab_bar_h = LINE_H + 2;
    g->title_h   = LINE_H + 4;
    g->pad_top   = g->title_h + g->tab_bar_h + 6;
    g->pad_bot   = 20;
    g->content_h = g->hh - g->pad_top - g->pad_bot;
    g->visible_lines = g->content_h / LINE_H;
    if (g->visible_lines < 1) g->visible_lines = 1;
    g->max_scroll = n_lines - g->visible_lines;
    if (g->max_scroll < 0) g->max_scroll = 0;
    g->num_tabs = num_tabs;
    g->n_lines  = n_lines;
    g->tab_y = g->hy + g->hh - g->title_h - g->tab_bar_h;
    g->tab_w = g->hw / num_tabs;
    return 1;
}

int ui_tabbed_overlay_max_scroll(const UiOverlayState *in) {
    OverlayGeom g;
    if (!overlay_compute_geom(in, &g)) return 0;
    return g.max_scroll;
}

UiOverlayHit ui_tabbed_overlay_hit_test(const UiOverlayState *in,
                                        int mx, int my) {
    UiOverlayHit hit = { UI_OVERLAY_HIT_OUTSIDE, -1, 0 };
    OverlayGeom g;
    if (!in || !in->visible) return hit;
    if (!overlay_compute_geom(in, &g)) return hit;
    hit.max_scroll = g.max_scroll;

    /* GLUT mouse is top-left origin; the panel rect is GL y-up. */
    int gy = in->viewport_h - my;
    if (mx < g.hx || mx > g.hx + g.hw || gy < g.hy || gy > g.hy + g.hh)
        return hit;

    if (gy >= g.tab_y && gy <= g.tab_y + g.tab_bar_h && g.tab_w > 0) {
        int t = (mx - g.hx) / g.tab_w;
        if (t >= 0 && t < g.num_tabs) {
            hit.kind = UI_OVERLAY_HIT_TAB;
            hit.tab  = t;
            return hit;
        }
    }
    hit.kind = UI_OVERLAY_HIT_BODY;
    return hit;
}

void ui_tabbed_overlay_render(const UiOverlayState *in) {
    if (!in || !in->visible) return;
    if (!in->content || in->content->tab_count <= 0) return;

    const UiOverlayContent *content = in->content;
    OverlayGeom g;
    if (!overlay_compute_geom(in, &g)) return;

    int tab_idx = overlay_active_tab_idx(in);
    int scroll  = in->scroll;

    const char *const *text = content->tabs[tab_idx].lines;
    if (!text) return;

    int num_tabs      = g.num_tabs;
    int n_lines       = g.n_lines;
    int hx = g.hx, hy = g.hy, hw = g.hw, hh = g.hh;
    int tab_bar_h     = g.tab_bar_h;
    int title_h       = g.title_h;
    int pad_top       = g.pad_top;
    int pad_bot       = g.pad_bot;
    int content_h     = g.content_h;
    int visible_lines = g.visible_lines;
    int max_scroll    = g.max_scroll;

    int win_w = in->viewport_w;
    int win_h = in->viewport_h;
    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Clamp scroll for display (the session is clamped on input). */
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;

    /* Background - shared raised-panel surface (config menu / dropdown) */
    ui_clr_a(UI_TOK_RAISED, 0.98f);
    glRectf((float)hx, (float)hy, (float)hx + (float)hw, (float)hy + (float)hh);

    /* Border - shared panel border */
    ui_clr(UI_TOK_BORDER);
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
        ui_clr(UI_TOK_DIVIDER);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)title_y);
        glVertex2f((float)(hx + hw), (float)title_y);
        glEnd();

        /* Title text - dim, left-aligned like config menu section headers */
        const char *title = content->title ? content->title : "";
        ui_clr(UI_TOK_TEXT_SECTION);
        gl2d_draw_string((float)(hx + MENU_TEXT_INSET_X), (float)(title_y + 4), title, FONT_SMALL);

        /* Tab switch hint right-aligned */
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * FONT_SMALL_W - 14;
        ui_clr_a(UI_TOK_TEXT_MUTED, 0.70f);
        gl2d_draw_string((float)nh_x, (float)(title_y + 4), nav_hint, FONT_SMALL);
    }

    /* --- Tab bar --- */
    {
        /* Use the geom's tab band/width (same formula as
         * overlay_compute_geom) so render and hit-test can't drift —
         * the single-source-of-truth invariant in the OverlayGeom doc. */
        int tab_y  = g.tab_y;
        int tab_w  = g.tab_w;

        /* Tab bar background */
        ui_clr(UI_TOK_SUNKEN);
        glRectf((float)hx, (float)tab_y, (float)hx + (float)hw, (float)tab_y + (float)tab_bar_h);

        for (int t = 0; t < num_tabs; t++) {
            int tx_tab = hx + t * tab_w;
            if (t == tab_idx) {
                /* Active tab: bottom accent bar + bright label */
                ui_clr_a(UI_TOK_ACCENT, 0.85f);
                glRectf((float)tx_tab, (float)tab_y, (float)tx_tab + (float)tab_w, (float)tab_y + 2.0f);
                ui_clr(UI_TOK_TEXT_PRIMARY);
            } else {
                ui_clr(UI_TOK_TEXT_MUTED);
            }
            const char *label = content->tabs[t].label ? content->tabs[t].label : "";
            int lbl_len = (int)strlen(label);
            int lbl_x   = tx_tab + (tab_w - lbl_len * FONT_SMALL_W) / 2;
            gl2d_draw_string((float)lbl_x, (float)(tab_y + 3), label, FONT_SMALL);
        }

        /* Separator line below tab bar */
        ui_clr(UI_TOK_DIVIDER);
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
            ui_clr(UI_TOK_TEXT_PRIMARY);
            gl2d_draw_string((float)tx, (float)ty, left, FONT_SMALL);

            /* Right column - aligned to shared tab stop */
            ui_clr(UI_TOK_TEXT_MUTED);
            gl2d_draw_string((float)(tx + tab_stop * FONT_SMALL_W), (float)ty,
                        tab + 1, FONT_SMALL);
        } else if (text[i][0] != ' ') {
            /* Section header - dim gray-blue like config menu */
            ui_clr(UI_TOK_TEXT_SECTION);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent - code example, accent */
            ui_clr_a(UI_TOK_ACCENT, 0.90f);
            gl2d_draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else {
            /* 2-space indent, no split - light label colour */
            ui_clr(UI_TOK_TEXT_PRIMARY);
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

        /* Track */
        ui_clr_a(UI_TOK_DIVIDER, 0.60f);
        glRectf((float)bar_x, (float)(bar_top - bar_h), (float)bar_x + 4.0f, (float)(bar_top - bar_h) + (float)bar_h);

        /* Thumb - #888 */
        ui_clr_a(UI_TOK_TEXT_MUTED, 0.80f);
        glRectf((float)bar_x, (float)thumb_y, (float)bar_x + 4.0f, (float)thumb_y + (float)thumb_h);

        /* Scroll hint at bottom */
        if (scroll < max_scroll) {
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_SMALL_W) / 2;
            ui_clr_a(UI_TOK_TEXT_MUTED, 0.50f);
            gl2d_draw_string((float)hint_x, (float)(hy + 4), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
