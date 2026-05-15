#ifndef GL_2D_H
#define GL_2D_H
#include <gl_includes.h>

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
    for (; *s; s++) glutBitmapCharacter(font, (unsigned char)*s);
}

#endif /* GL_2D_H */
