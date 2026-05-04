#ifndef GL_2D_H
#define GL_2D_H
#include <gl_includes.h>

#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13

/* Push a 2D ortho projection sized to (0,0)-(w,h) and disable depth +
 * lighting via glPushAttrib so the corresponding gl2d_end() restores
 * prior state without project-side lighting queries. */
static inline void gl2d_begin(int w, int h) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, w, 0, h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT);
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
