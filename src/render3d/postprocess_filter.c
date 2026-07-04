/*
 * postprocess_filter.c - see postprocess_filter.h.
 *
 * Iteration 1: chromatic aberration. Capture the resolved scene rect
 * into one POT texture, redraw it, then redraw red/blue channel-only
 * passes with a small ±x screen offset.
 *
 * Iteration 2: vignette. A blended elliptical radial gradient drawn
 * over the rect — transparent at the centre, darkening toward the
 * corners. No capture needed (it only darkens), so it skips the
 * texture machinery entirely.
 *
 * Iteration 3: scanlines (CRT mask). Dark 1px horizontal GL_LINES
 * every 3px, multiplicatively blended (GL_DST_COLOR, GL_ZERO) to
 * darken existing pixels without adding light. Capture-free overlay.
 *
 * All are pure fixed-function GL (no shaders, no FBOs). The same rect
 * the scene renders into is reused over the whole window by the
 * app-level glr_compositor, so each effect works at scene-viewport and
 * full-frame scope alike. render3d_postprocess_filter_render() dispatches
 * on the mode.
 */
#include "postprocess_filter.h"

#include "gl_includes.h"

#include <math.h>   /* cosf / sinf / sqrtf for the vignette ring */

/* One owned texture, sized to the largest scene rect seen so far. */
static GLuint g_filter_tex = 0;
static int    g_tex_w      = 0;
static int    g_tex_h      = 0;
/* GL_MAX_TEXTURE_SIZE, queried once: 0 = not yet, -1 = unknown
 * (e.g. GL stubs) so the size guard is not enforced. */
static GLint  g_max_tex_size = 0;

const char *render3d_postprocess_filter_mode_name(Render3dPostFilterMode mode) {
    switch (mode) {
    case RENDER3D_POST_FILTER_CHROMATIC_ABERRATION: return "Chromatic aberration";
    case RENDER3D_POST_FILTER_VIGNETTE:             return "Vignette";
    case RENDER3D_POST_FILTER_SCANLINES:            return "Scanlines";
    case RENDER3D_POST_FILTER_OFF:
    case RENDER3D_POST_FILTER_COUNT:
    default:                                     return "Off";
    }
}

void render3d_postprocess_filter_reset(void) {
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
 * minimal screen-space textured-quad state is set up here. begin_2d
 * snapshots the matrix-mode (not covered by glPushAttrib); end_2d
 * accepts it back as a parameter, so the pair isn't coupled by a
 * file-static and an unbalanced second caller can't desync. */
static void postprocess_filter_begin_2d(int sx, int sy, int sw, int sh,
                                        GLint *saved_matrix_mode_out) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glGetIntegerv(GL_MATRIX_MODE, saved_matrix_mode_out);

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

static void postprocess_filter_end_2d(GLint saved_matrix_mode) {
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode((GLenum)saved_matrix_mode);
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

static void postprocess_filter_render_chromatic(int sx, int sy,
                                                int sw, int sh) {
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

    /* The state guard MUST start before the first glBindTexture.
     * glPushAttrib's GL_TEXTURE_BIT group (pulled in by
     * GL_ALL_ATTRIB_BITS) saves "the current texture bindings (for
     * example, GL_TEXTURE_BINDING_2D)" — OpenGL 2.1 spec §6.1.16
     * (state tables) / glPushAttrib reference page,
     * https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/glPushAttrib.xml
     * It snapshots that binding at push time, so glPopAttrib restores
     * the caller's GL_TEXTURE_BINDING_2D only if the push precedes our
     * bind. Allocation/copy are matrix/viewport-independent, so running
     * them inside the 2D bracket is safe. Do not reorder a
     * glBindTexture before this call. */
    GLint saved_matrix_mode = 0;
    postprocess_filter_begin_2d(sx, sy, sw, sh, &saved_matrix_mode);

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

    postprocess_filter_end_2d(saved_matrix_mode);
}

/* Vignette: a blended elliptical annulus over the rect — transparent at
 * the inner ellipse, darkening to `edge_alpha` black at the outer one.
 * The outer ellipse is aspect-matched (radii scaled by sqrt(2)) so it
 * passes exactly through the four corners; every rect pixel is therefore
 * inside-or-on it. The inner ellipse leaves the centre untouched. No
 * frame capture needed — this only darkens what is already on screen. */
static void postprocess_filter_render_vignette(int sx, int sy,
                                               int sw, int sh) {
    GLint saved_matrix_mode = 0;
    postprocess_filter_begin_2d(sx, sy, sw, sh, &saved_matrix_mode);

    /* begin_2d enables GL_TEXTURE_2D + REPLACE for the chromatic path;
     * the vignette is a flat-shaded color overlay, so drop texturing and
     * switch to standard alpha blending. Both are inside the
     * GL_ALL_ATTRIB_BITS push, so end_2d's glPopAttrib restores them. */
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float cx = (float)sw * 0.5f;
    const float cy = (float)sh * 0.5f;
    const float corner_k = 1.41421356f;   /* sqrt(2): outer ellipse hits corners */
    const float rx_out = cx * corner_k;
    const float ry_out = cy * corner_k;
    const float rx_in  = rx_out * 0.5f;   /* radius of the untouched centre */
    const float ry_in  = ry_out * 0.5f;
    const float edge_alpha = 0.55f;
    const float two_pi = 6.28318530717958648f;
    const int   segments = 64;

    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= segments; i++) {
        float a = two_pi * (float)i / (float)segments;
        float c = cosf(a);
        float s = sinf(a);
        glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
        glVertex2f(cx + rx_in * c, cy + ry_in * s);
        glColor4f(0.0f, 0.0f, 0.0f, edge_alpha);
        glVertex2f(cx + rx_out * c, cy + ry_out * s);
    }
    glEnd();

    postprocess_filter_end_2d(saved_matrix_mode);
}

/* Scanlines / CRT mask: dark 1px horizontal lines every 3 scanlines,
 * multiplicatively blended over the frame via GL_DST_COLOR × GL_ZERO
 * (darkens existing pixels, never adds light). The line color is a dim
 * gray (0.65) — when multiplied against the framebuffer, bright areas
 * dim by 35% on the scanline rows while the gaps stay untouched,
 * giving the classic CRT phosphor-mask look. Capture-free. */
static void postprocess_filter_render_scanlines(int sx, int sy,
                                                int sw, int sh) {
    GLint saved_matrix_mode = 0;
    postprocess_filter_begin_2d(sx, sy, sw, sh, &saved_matrix_mode);

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO); /* multiply: fb *= line_color */
    glLineWidth(1.0f);

    const float gray = 0.75f;
    glColor4f(gray, gray, gray, 1.0f);

    const int spacing = 3; /* one dark scanline every 3 rows */
    glBegin(GL_LINES);
    for (int y = 0; y < sh; y += spacing) {
        float fy = (float)y + 0.5f; /* centre of the pixel row */
        glVertex2f(0.0f,      fy);
        glVertex2f((float)sw, fy);
    }
    glEnd();

    postprocess_filter_end_2d(saved_matrix_mode);
}

void render3d_postprocess_filter_render(Render3dPostFilterMode mode, int sx, int sy,
                                     int sw, int sh) {
    if (sw <= 0 || sh <= 0)
        return;

    switch (mode) {
    case RENDER3D_POST_FILTER_CHROMATIC_ABERRATION:
        postprocess_filter_render_chromatic(sx, sy, sw, sh);
        break;
    case RENDER3D_POST_FILTER_VIGNETTE:
        postprocess_filter_render_vignette(sx, sy, sw, sh);
        break;
    case RENDER3D_POST_FILTER_SCANLINES:
        postprocess_filter_render_scanlines(sx, sy, sw, sh);
        break;
    case RENDER3D_POST_FILTER_OFF:
    case RENDER3D_POST_FILTER_COUNT:
    default:
        break;
    }
}
