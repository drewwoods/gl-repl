#ifndef GL_2D_H
#define GL_2D_H
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
    gluOrtho2D(0, w, 0, h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_CURRENT_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
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

#endif /* GL_2D_H */
