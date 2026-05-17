/*
 * postprocess_filter.c - see postprocess_filter.h.
 *
 * Iteration 1: chromatic aberration. Capture the resolved scene rect
 * into one POT texture, redraw it, then redraw red/blue channel-only
 * passes with a small ±x screen offset. Pure fixed-function GL.
 */
#include "postprocess_filter.h"

#include <gl_includes.h>

/* One owned texture, sized to the largest scene rect seen so far. */
static GLuint g_filter_tex = 0;
static int    g_tex_w      = 0;
static int    g_tex_h      = 0;
/* GL_MAX_TEXTURE_SIZE, queried once: 0 = not yet, -1 = unknown
 * (e.g. GL stubs) so the size guard is not enforced. */
static GLint  g_max_tex_size = 0;
/* Matrix mode saved across the 2D pass (not covered by glPushAttrib). */
static GLint  g_saved_matrix_mode = 0;

const char *scene_postprocess_filter_mode_name(int mode) {
    switch (mode) {
    case SCENE_POST_FILTER_CHROMATIC_ABERRATION: return "Chromatic aberration";
    case SCENE_POST_FILTER_OFF:
    default:                                     return "Off";
    }
}

void scene_postprocess_filter_reset(void) {
    if (g_filter_tex) {
        glDeleteTextures(1, &g_filter_tex);
        g_filter_tex = 0;
    }
    g_tex_w = 0;
    g_tex_h = 0;
    g_max_tex_size = 0; /* re-query against the (possibly new) context */
}

/* NPOT textures are an OpenGL 2.0 feature; the fixed-function GL 1.1
 * baseline only allows power-of-two GL_TEXTURE_2D dimensions. POT plus
 * the umax/vmax subregion below renders correctly on 1.1 and 2.0+. */
static int next_pow2(int v) {
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* Private 2D bracket. src/scene/ must not depend on ui/gl_2d.h, so the
 * minimal screen-space textured-quad state is set up here. */
static void postprocess_filter_begin_2d(int sx, int sy, int sw, int sh) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glGetIntegerv(GL_MATRIX_MODE, &g_saved_matrix_mode);

    glViewport(sx, sy, sw, sh);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)sw, 0.0, (double)sh, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

static void postprocess_filter_end_2d(void) {
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode((GLenum)g_saved_matrix_mode);
    glPopAttrib(); /* restores viewport, enables, color mask, depth mask */
}

/* Screen-aligned textured quad, vertex-shifted by dx pixels in X
 * (texcoords unchanged — an exact pixel offset independent of the POT
 * texture size). */
static void postprocess_filter_draw_quad(int sw, int sh,
                                         float umax, float vmax,
                                         float dx) {
    float w = (float)sw;
    float h = (float)sh;
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(dx,     0.0f);
        glTexCoord2f(umax, 0.0f); glVertex2f(w + dx, 0.0f);
        glTexCoord2f(umax, vmax); glVertex2f(w + dx, h);
        glTexCoord2f(0.0f, vmax); glVertex2f(dx,     h);
    glEnd();
}

void scene_postprocess_filter_render(int mode, int sx, int sy,
                                     int sw, int sh) {
    if (mode != SCENE_POST_FILTER_CHROMATIC_ABERRATION)
        return;
    if (sw <= 0 || sh <= 0)
        return;

    if (g_max_tex_size == 0) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_tex_size);
        if (g_max_tex_size <= 0)
            g_max_tex_size = -1; /* unknown (stubs): don't enforce */
    }

    int tex_w = next_pow2(sw);
    int tex_h = next_pow2(sh);
    if (g_max_tex_size > 0 &&
        (tex_w > g_max_tex_size || tex_h > g_max_tex_size))
        return; /* would exceed the GL texture limit — skip this frame */

    if (g_filter_tex == 0 || tex_w > g_tex_w || tex_h > g_tex_h) {
        if (g_filter_tex == 0)
            glGenTextures(1, &g_filter_tex);
        glBindTexture(GL_TEXTURE_2D, g_filter_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex_w, tex_h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL);
        g_tex_w = tex_w;
        g_tex_h = tex_h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_filter_tex);
    }

    /* Capture the resolved scene image. GL window coords (bottom-left
     * origin); (sx,sy,sw,sh) is the same rect the scene rendered into,
     * so no Y flip is needed. */
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx, sy, sw, sh);

    float umax = (float)sw / (float)g_tex_w;
    float vmax = (float)sh / (float)g_tex_h;

    postprocess_filter_begin_2d(sx, sy, sw, sh);

    /* Base image: full RGB, unshifted. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    postprocess_filter_draw_quad(sw, sh, umax, vmax, 0.0f);

    /* Chromatic aberration: red shifted +dx, blue shifted -dx, green
     * left from the base pass. Blending stays disabled; glColorMask
     * limits each pass to one channel. */
    const float dx = 1.75f;
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    postprocess_filter_draw_quad(sw, sh, umax, vmax, +dx);
    glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
    postprocess_filter_draw_quad(sw, sh, umax, vmax, -dx);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    postprocess_filter_end_2d();
}
