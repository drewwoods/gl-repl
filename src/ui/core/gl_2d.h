#ifndef GL_2D_H
#define GL_2D_H
#include <string.h>          /* strlen — chip label measurement */

#include "gl_includes.h"
#include "ui/core/theme.h"

/* Push a 2D ortho projection sized to (0,0)-(w,h) and disable depth +
 * lighting via glPushAttrib so the corresponding gl2d_end() restores
 * prior state without project-side lighting queries.
 *
 * GL_CURRENT_BIT is included so a 2D overlay's glColor* cannot leak
 * into GL's current color and bleed into the next frame's scene
 * geometry (uncolored user geometry otherwise inherited the status
 * banner's orange / profile panel's green). Overlays stay fully
 * transient: the scene/display color — or the GL default, or whatever
 * init() set — is left exactly as it was. */
static inline void gl2d_begin(int w, int h) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    /* glOrtho, not gluOrtho2D: they build the identical matrix, but GLU routes
     * its internal glOrtho through libGLU's own libGL dispatch. Under OSMesa
     * (linked against a separate libGL) that dispatch is NOT the current
     * rendering context, so gluOrtho2D silently no-ops and every 2D overlay
     * projects through identity. Calling glOrtho directly hits the live
     * context on every backend. */
    glOrtho(0.0, (double)w, 0.0, (double)h, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_CURRENT_BIT |
                 GL_LINE_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    /* GL_LINE_BIT is the same argument as GL_CURRENT_BIT run the other way:
     * not what leaks out, but what leaks in. 2D overlays are pixel work, and
     * a 1px panel border or axis tick must not change width or pick up
     * antialiasing because the scene left glLineWidth(5) or GL_LINE_SMOOTH
     * behind — the user's program can set both, and so can the Line smooth
     * config. Every overlay therefore starts from the same crisp 1px
     * baseline, and any width an overlay sets for itself is popped by
     * gl2d_end(). */
    glLineWidth(1.0f);
    glDisable(GL_LINE_SMOOTH);
}

static inline void gl2d_end(void) {
    glPopAttrib();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

/* Render a NUL-terminated string at (x,y) in the current GL color
 * using the given GLUT bitmap font. */
static inline void gl2d_draw_string(float x, float y, const char *s,
                                    void *font) {
    glRasterPos2f(x, y);
#ifdef USE_GLUT
    /* Apple GLUT does not expose the freeglut glutBitmapString extension. */
    for (; *s; s++) glutBitmapCharacter(font, (unsigned char)*s);
#else
    /* freeglut preserves pixel-store state once per string rather than once
     * per glyph, which is particularly important through gl4es/WebGL. */
    glutBitmapString(font, (const unsigned char *)s);
#endif
}

/* Pixel width of `s` in a GLUT bitmap font, summed per glyph.
 *
 * Fixed-width fonts (FONT_SMALL / FONT_MONO) can be measured as
 * strlen * FONT_*_W, but FONT_TINY is Helvetica 10 — proportional — so
 * chrome that centers or right-aligns tiny text has to ask GLUT. */
static inline int gl2d_text_width(void *font, const char *s) {
    int w = 0;
    if (!s)
        return 0;
    for (; *s; s++)
        w += glutBitmapWidth(font, (unsigned char)*s);
    return w;
}

/* Draw a floating-panel frame: filled background rect + 1px border line loop.
 * Expects blending already enabled; caller sets up gl2d_begin beforehand.
 * bg_tok / border_tok are UiThemeToken values; bg_alpha / border_alpha
 * are per-draw alpha multipliers fed through ui_clr_a(). */
static inline void gl2d_panel_frame(float x, float y, float w, float h,
                                     int bg_tok, float bg_alpha,
                                     int border_tok, float border_alpha) {
    ui_clr_a((UiThemeToken)bg_tok, bg_alpha);
    glRectf(x, y, x + w, y + h);
    ui_clr_a((UiThemeToken)border_tok, border_alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x,     y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x,     y + h);
    glEnd();
}

/* --- Mouse chips -------------------------------------------------------
 *
 * The panels carry two kinds of clickable text, and they are not the same
 * thing even though both used to be bracketed words:
 *
 *   A *state* chip owns a setting and shows that setting's current value —
 *   "1 Hz", "log", "2x". Clicking it changes the value (cycling, or toggling
 *   for a two-state one).
 *
 *   An *action* chip fires a one-shot — "[reset]", "[x]". It owns no state,
 *   so there is nothing for it to display but the verb.
 *
 * The rule for authors is therefore: **anything with persistent state shows
 * the state; a verb means clicking does that thing, now.** A chip labelled
 * with a verb must not have a mode. (Single-glyph disclosure controls —
 * "[+]" / "[-]" — are the one exemption: they are a universal idiom, they
 * cannot be misread as a value, and the panel's own collapsed shape already
 * shows the state.)
 *
 * The two are drawn differently so the distinction survives without knowing
 * the rule: a state chip sits in a sunken well like a form field showing a
 * value, an action chip is bare bracketed text. Brackets mean "press me";
 * a well means "this is the current setting".
 *
 * Both take the text *baseline* y, the same coordinate gl2d_draw_string
 * takes, so a chip drops into an existing text row without new arithmetic.
 * Expect blending enabled, as with gl2d_panel_frame. */
#define GL2D_CHIP_PAD_X 3   /* inset between a state well and its text */
/* Vertical fit. FONT_SMALL is the X11 8x13 cell: glyphs rise ~11px above the
 * baseline and descend ~2 below it, so a well anchored at the baseline has to
 * reach FONT_SMALL_H above it, not FONT_SMALL_H total, or ascenders sit
 * outside the box. One pixel of air on each side of the glyph box. */
#define GL2D_CHIP_DROP  3            /* well bottom, below the baseline */
#define GL2D_CHIP_H     (FONT_SMALL_H + 2)

/* Width of a state chip's well, for layout and hit-testing. `label` is the
 * bare value — no brackets. Fixed-width font, so this is a character count. */
static inline int gl2d_chip_state_w(const char *label) {
    return (label ? (int)strlen(label) : 0) * FONT_SMALL_W + 2 * GL2D_CHIP_PAD_X;
}

/* A setting showing its value. `tok` is the text token, so a caller can draw
 * an inert one (a request the data cannot honor) in the placeholder color
 * while keeping the well. */
static inline void gl2d_chip_state(float x, float baseline_y,
                                   const char *label, int tok) {
    float w = (float)gl2d_chip_state_w(label);
    gl2d_panel_frame(x, baseline_y - (float)GL2D_CHIP_DROP, w,
                     (float)GL2D_CHIP_H,
                     UI_TOK_SUNKEN, 0.85f, UI_TOK_BORDER, 0.7f);
    ui_clr((UiThemeToken)tok);
    gl2d_draw_string(x + (float)GL2D_CHIP_PAD_X, baseline_y, label, FONT_SMALL);
}

/* A one-shot. `label` carries its own brackets, since that is the affordance. */
static inline void gl2d_chip_action(float x, float baseline_y,
                                    const char *label) {
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string(x, baseline_y, label, FONT_SMALL);
}

#endif /* GL_2D_H */
