/*
 * tour_presence.c - see tour_presence.h.
 */
#include "ui/subsystems/tour_presence.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

#include <stdio.h>
#include <string.h>

/* One edge band: solid along the window edge a--b, transparent along the
 * matching inner pair ia--ib. Written as an explicit quad rather than
 * gl2d_panel_frame because the whole point is the gradient — a flat rule alone
 * reads as a UI border someone forgot to remove, while a falloff reads as a
 * glow around the viewport. The inner pair must be given in the SAME order as
 * the edge pair (ia under a, ib under b) or the quad crosses itself. */
static void presence_band(float ax, float ay, float bx, float by,
                          float iax, float iay, float ibx, float iby,
                          const float *c, float alpha) {
    glBegin(GL_QUADS);
    glColor4f(c[0], c[1], c[2], alpha);
    glVertex2f(ax, ay);
    glVertex2f(bx, by);
    glColor4f(c[0], c[1], c[2], 0.0f);
    glVertex2f(ibx, iby);
    glVertex2f(iax, iay);
    glEnd();
}

/* Copy `src` into `dst`, elided with ".." past `max_chars` glyphs — the same
 * shape as the HUD's truncation, kept local because the card measures in a
 * different font and shares no layout with it. */
static void presence_truncate(char *dst, size_t dstsz, const char *src,
                              int max_chars) {
    size_t copy = (size_t)(max_chars < 0 ? 0 : max_chars);
    if (copy >= dstsz)
        copy = dstsz - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    if (copy >= 2 && strlen(src) > copy) {
        dst[copy - 1] = '.';
        dst[copy - 2] = '.';
    }
}

static void presence_draw_card(const GlrTourPresenceView *v,
                               int win_w, int win_h) {
    /* The hint is the whole reason the card exists past the first second: a
     * tour is a mode, and a mode has to say how to leave. */
    static const char *k_hint = "Esc exits  |  Space pauses";
    char name[GLR_TOUR_PRESENCE_NAME_CAP + 8];
    char title[sizeof(name) + 8];
    int title_w, hint_w, text_w;
    float card_x, card_y, card_w, card_h;
    float a = v->card_alpha;

    presence_truncate(name, sizeof(name),
                      (v->name && v->name[0]) ? v->name : "Untitled",
                      TOUR_PRESENCE_NAME_MAX_CHARS);
    snprintf(title, sizeof(title), "TOUR   %s", name);

    /* Both fonts are fixed-width, so a character count is the exact width. */
    title_w = (int)strlen(title) * FONT_W;
    hint_w  = (int)strlen(k_hint) * FONT_SMALL_W;
    text_w  = title_w > hint_w ? title_w : hint_w;

    card_w = (float)(text_w + 2 * TOUR_PRESENCE_CARD_PAD_X);
    card_h = (float)(FONT_H + TOUR_PRESENCE_CARD_GAP_Y + FONT_SMALL_H +
                     2 * TOUR_PRESENCE_CARD_PAD_Y);
    card_x = ((float)win_w - card_w) * 0.5f;
    card_y = (float)win_h * TOUR_PRESENCE_CARD_CENTER_Y - card_h * 0.5f;
    if (card_x < 0.0f) card_x = 0.0f;
    if (card_y < 0.0f) card_y = 0.0f;

    gl2d_panel_frame(card_x, card_y, card_w, card_h,
                     UI_TOK_RAISED, 0.94f * a, UI_TOK_ACCENT_ALT, 0.95f * a);
    /* Amber ribbon along the top edge, matching the tour HUD's silhouette so
     * the card and the HUD read as the same feature. */
    ui_clr_a(UI_TOK_ACCENT_ALT, 0.95f * a);
    glRectf(card_x, card_y + card_h - TOUR_PRESENCE_CARD_RIBBON_H,
            card_x + card_w, card_y + card_h);

    ui_clr_a(UI_TOK_ACCENT_ALT, a);
    gl2d_draw_string(card_x + (card_w - (float)title_w) * 0.5f,
                     card_y + (float)(TOUR_PRESENCE_CARD_PAD_Y +
                                      FONT_SMALL_H + TOUR_PRESENCE_CARD_GAP_Y),
                     title, FONT_MONO);

    ui_clr_a(UI_TOK_TEXT_MUTED, a);
    gl2d_draw_string(card_x + (card_w - (float)hint_w) * 0.5f,
                     card_y + (float)TOUR_PRESENCE_CARD_PAD_Y,
                     k_hint, FONT_SMALL);
}

void tour_ui_presence_render(const GlrTourPresenceView *v,
                             int win_w, int win_h) {
    const float *c;
    float w, h, band, rule, alpha;

    if (!v || v->phase == GLR_TOUR_PRESENCE_OFF)
        return;
    if (win_w <= 0 || win_h <= 0)
        return;

    w = (float)win_w;
    h = (float)win_h;
    band = (float)TOUR_PRESENCE_BAND_PX * v->band_scale;
    rule = (float)TOUR_PRESENCE_RULE_PX * v->band_scale;
    alpha = v->border_alpha;
    c = ui_rgba(UI_TOK_ACCENT_ALT);

    gl2d_begin(win_w, win_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (band > 0.0f && alpha > 0.0f) {
        /* Four bands, drawn full-length so they overlap at the corners. The
         * overlap is intentional: two blended passes make the corners the
         * brightest part of the frame, which is what makes this read as a
         * vignette rather than as four separate rules. */
        presence_band(0.0f, 0.0f, w, 0.0f,
                      0.0f, band, w, band, c, alpha);
        presence_band(0.0f, h, w, h,
                      0.0f, h - band, w, h - band, c, alpha);
        presence_band(0.0f, 0.0f, 0.0f, h,
                      band, 0.0f, band, h, c, alpha);
        presence_band(w, 0.0f, w, h,
                      w - band, 0.0f, w - band, h, c, alpha);

        /* Solid hairline at the very edge: the gradient alone has no crisp
         * boundary, and without one the effect can be mistaken for a lighting
         * artifact in the scene rather than chrome. */
        ui_clr_a(UI_TOK_ACCENT_ALT, alpha);
        glRectf(0.0f, 0.0f, w, rule);
        glRectf(0.0f, h - rule, w, h);
        glRectf(0.0f, 0.0f, rule, h);
        glRectf(w - rule, 0.0f, w, h);
    }

    if (v->card_alpha > 0.0f)
        presence_draw_card(v, win_w, win_h);

    glDisable(GL_BLEND);
    gl2d_end();
}
