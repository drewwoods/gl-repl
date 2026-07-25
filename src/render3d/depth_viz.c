/*
 * depth_viz.c - see depth_viz.h.
 *
 * Split into a pure conversion core (render3d_depth_viz_map — no GL,
 * unit-tested with synthetic buffers) and a thin GL shell: capture
 * (glReadPixels GL_DEPTH_COMPONENT into a persistent float buffer,
 * grown on resize) and render (byte conversion + POT GL_LUMINANCE
 * texture sub-upload + screen-space quad, reusing postprocess_filter's
 * 2D bracket). GL_NEAREST filtering — the quad is a 1:1 pixel overlay.
 */
#include "depth_viz.h"

#include "postprocess_filter.h"   /* render3d_post_2d_begin/_end */
#include "projection_mode.h"
#include "gl_includes.h"
#include "support/cpuprof.h"

#include <stdlib.h>

/* Background test: clear depth is exactly 1.0; anything at or beyond
 * 1 - epsilon is "no geometry here" and renders black. */
#define DEPTH_VIZ_BG_EPS       1e-6f
/* Below this linear-depth span the SCENE normalization would divide by
 * ~0 (constant-depth scene); in-range pixels map to mid-gray instead. */
#define DEPTH_VIZ_SPAN_EPS     1e-6f
/* EMA smoothing for the SCENE range: s += alpha * (raw - s). */
#define DEPTH_VIZ_EMA_ALPHA    0.25f
/* Snap (no smoothing) when the raw span jumps past this ratio of the
 * smoothed span in either direction — example switches must not lag. */
#define DEPTH_VIZ_SNAP_RATIO   2.0f
/* SCENE mode maps its farthest pixel to this luminance floor so it
 * stays distinguishable from the black background. */
#define DEPTH_VIZ_SCENE_FAR_LUM 0.1f

/* Persistent capture + conversion buffers (one allocation per resize). */
static float         *g_depth   = NULL;
static unsigned char *g_lum     = NULL;
static size_t         g_buf_px  = 0;
static int            g_cap_w   = 0;
static int            g_cap_h   = 0;
static int            g_cap_valid = 0;

/* One owned POT luminance texture, sized to the largest rect seen. */
static GLuint g_tex   = 0;
static int    g_tex_w = 0;
static int    g_tex_h = 0;
/* GL_MAX_TEXTURE_SIZE, queried once: 0 = not yet, -1 = unknown (GL
 * stubs) so the size guard is not enforced. */
static GLint  g_max_tex_size = 0;

static Render3dDepthVizRange g_range = { 0.0f, 0.0f, 0 };

void render3d_depth_viz_reset(void) {
    free(g_depth);
    g_depth = NULL;
    free(g_lum);
    g_lum = NULL;
    g_buf_px = 0;
    g_cap_w = 0;
    g_cap_h = 0;
    g_cap_valid = 0;
    if (g_tex) {
        glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    g_tex_w = 0;
    g_tex_h = 0;
    g_max_tex_size = 0; /* re-query against the (possibly new) context */
    g_range.lo = 0.0f;
    g_range.hi = 0.0f;
    g_range.valid = 0;
}

/* Window z -> linear depth. Perspective inverts the hyperbolic depth
 * mapping back to eye distance; ortho window z is already linear in
 * eye z, so it passes through. */
static float depth_viz_linearize(float z, float near_z, float far_z,
                                 int perspective) {
    if (!perspective)
        return z;
    {
        float denom = far_z - z * (far_z - near_z);
        if (denom < 1e-12f)
            denom = 1e-12f;
        return (near_z * far_z) / denom;
    }
}

static float depth_viz_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* LINEAR mapping for one pixel: d01 across the full near..far range. */
static unsigned char depth_viz_linear_byte(float z, float near_z,
                                           float far_z, int perspective) {
    float d01;
    if (perspective) {
        float lin = depth_viz_linearize(z, near_z, far_z, 1);
        d01 = (lin - near_z) / (far_z - near_z);
    } else {
        d01 = z;
    }
    d01 = depth_viz_clamp01(d01);
    return (unsigned char)((1.0f - d01) * 255.0f + 0.5f);
}

void render3d_depth_viz_map(const float *depth, int count,
                            Render3dDepthVizMode mode,
                            const Render3dProjectionDesc *proj,
                            Render3dDepthVizRange *range,
                            unsigned char *lum_out) {
    int perspective;
    float near_z, far_z;
    int range_normalize;

    if (!depth || !proj || !lum_out || count <= 0)
        return;

    perspective = (proj->projection == PROJ_PERSPECTIVE);
    near_z = (float)proj->near_z;
    far_z  = (float)proj->far_z;
    range_normalize = (mode == RENDER3D_DEPTH_VIZ_SCENE ||
                  mode == RENDER3D_DEPTH_VIZ_SPLIT);

    if (range_normalize && range) {
        /* Scan pass: raw linear-depth extent of the in-range pixels. */
        float raw_lo = 0.0f, raw_hi = 0.0f;
        int found = 0;
        for (int i = 0; i < count; i++) {
            float z = depth[i];
            float lin;
            if (z >= 1.0f - DEPTH_VIZ_BG_EPS)
                continue;
            lin = depth_viz_linearize(z, near_z, far_z, perspective);
            if (!found) {
                raw_lo = lin;
                raw_hi = lin;
                found = 1;
            } else {
                if (lin < raw_lo) raw_lo = lin;
                if (lin > raw_hi) raw_hi = lin;
            }
        }
        if (!found) {
            /* Empty capture: nothing to normalize against — fall back
             * to the LINEAR map (every pixel is background -> black
             * anyway) and leave the EMA range untouched. */
            range_normalize = 0;
        } else {
            float raw_span = raw_hi - raw_lo;
            float ema_span = range->hi - range->lo;
            int snap = !range->valid ||
                       raw_span > ema_span * DEPTH_VIZ_SNAP_RATIO ||
                       ema_span > raw_span * DEPTH_VIZ_SNAP_RATIO;
            if (snap) {
                range->lo = raw_lo;
                range->hi = raw_hi;
                range->valid = 1;
            } else {
                range->lo += DEPTH_VIZ_EMA_ALPHA * (raw_lo - range->lo);
                range->hi += DEPTH_VIZ_EMA_ALPHA * (raw_hi - range->hi);
            }
        }
    } else if (range_normalize) {
        range_normalize = 0; /* no range state supplied: LINEAR fallback */
    }

    if (range_normalize) {
        float lo = range->lo;
        float span = range->hi - range->lo;
        int degenerate = (span < DEPTH_VIZ_SPAN_EPS);
        for (int i = 0; i < count; i++) {
            float z = depth[i];
            if (z >= 1.0f - DEPTH_VIZ_BG_EPS) {
                lum_out[i] = 0;
                continue;
            }
            {
                float d01;
                if (degenerate) {
                    d01 = 0.5f;
                } else {
                    float lin = depth_viz_linearize(z, near_z, far_z,
                                                    perspective);
                    /* Clamp AFTER the division: EMA lag can put this
                     * frame's samples outside the smoothed range. */
                    d01 = depth_viz_clamp01((lin - lo) / span);
                }
                lum_out[i] = (unsigned char)(
                    (1.0f - (1.0f - DEPTH_VIZ_SCENE_FAR_LUM) * d01)
                    * 255.0f + 0.5f);
            }
        }
    } else {
        for (int i = 0; i < count; i++) {
            float z = depth[i];
            if (z >= 1.0f - DEPTH_VIZ_BG_EPS) {
                lum_out[i] = 0;
                continue;
            }
            lum_out[i] = depth_viz_linear_byte(z, near_z, far_z,
                                               perspective);
        }
    }
}

/* The profile bracket lives here rather than around the render3d hook
 * call: the hook is deliberately neutral, so render3d must not name a
 * viz section, and bracketing on this side also stops charging the
 * section on frames where the mode is Off. */
void render3d_depth_viz_capture(int sx, int sy, int sw, int sh) {
    size_t px;

    g_cap_valid = 0;
    if (sw <= 0 || sh <= 0)
        return;
    prof_begin(PROF_RENDER3D_DEPTH_VIZ);
    px = (size_t)sw * (size_t)sh;
    if (px > g_buf_px) {
        float *new_depth = (float *)malloc(px * sizeof(float));
        unsigned char *new_lum = (unsigned char *)malloc(px);
        if (!new_depth || !new_lum) {
            free(new_depth);
            free(new_lum);
            prof_accum_end(PROF_RENDER3D_DEPTH_VIZ);
            return; /* degrade to no capture this frame */
        }
        free(g_depth);
        free(g_lum);
        g_depth = new_depth;
        g_lum = new_lum;
        g_buf_px = px;
    }
    /* Float rows are always 4-byte aligned, so no GL_PACK_ALIGNMENT
     * bracketing is needed (same as the edit-overlays occlusion read). */
    glReadPixels(sx, sy, sw, sh, GL_DEPTH_COMPONENT, GL_FLOAT, g_depth);
    g_cap_w = sw;
    g_cap_h = sh;
    g_cap_valid = 1;
    prof_accum_end(PROF_RENDER3D_DEPTH_VIZ);
}

/* See postprocess_filter.c: GL 1.1 baseline needs power-of-two texture
 * dimensions; the umax/vmax subregion renders correctly on 1.1 and 2.0+. */
static int depth_viz_next_pow2(int v) {
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* Ensure the POT luminance texture exists at >= sw x sh, bind it, and
 * sub-upload the converted bytes. Runs inside the 2D bracket so the
 * pushed GL_TEXTURE_BIT restores the caller's binding on pop. Returns 0
 * when the required POT size would exceed GL_MAX_TEXTURE_SIZE. */
static int depth_viz_upload_texture(int sw, int sh) {
    int tex_w, tex_h;
    GLint saved_align = 0;

    if (g_max_tex_size == 0) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_tex_size);
        if (g_max_tex_size <= 0)
            g_max_tex_size = -1; /* unknown (stubs): don't enforce */
    }
    tex_w = depth_viz_next_pow2(sw);
    tex_h = depth_viz_next_pow2(sh);
    if (g_max_tex_size > 0 &&
        (tex_w > g_max_tex_size || tex_h > g_max_tex_size))
        return 0;

    if (g_tex == 0 || tex_w > g_tex_w || tex_h > g_tex_h) {
        if (g_tex == 0)
            glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, tex_w, tex_h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        g_tex_w = tex_w;
        g_tex_h = tex_h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex);
    }

    /* Byte rows need 1-byte unpack alignment; pixel-store is client
     * state outside glPushAttrib, so save/restore it explicitly. */
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, g_lum);
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved_align > 0 ? saved_align : 4);
    return 1;
}

void render3d_depth_viz_render(Render3dDepthVizMode mode,
                               const Render3dProjectionDesc *proj,
                               int sx, int sy, int sw, int sh) {
    GLint saved_matrix_mode = 0;
    float umax, vmax, u0;
    int x0;

    if (mode <= RENDER3D_DEPTH_VIZ_OFF || mode >= RENDER3D_DEPTH_VIZ_COUNT)
        return;
    if (!proj || sw <= 0 || sh <= 0)
        return;
    if (!g_cap_valid || g_cap_w != sw || g_cap_h != sh)
        return;
    g_cap_valid = 0; /* consume: never redraw a stale capture */

    prof_begin(PROF_RENDER3D_DEPTH_VIZ);
    render3d_depth_viz_map(g_depth, sw * sh, mode, proj, &g_range, g_lum);

    render3d_post_2d_begin(sx, sy, sw, sh, &saved_matrix_mode);
    if (!depth_viz_upload_texture(sw, sh)) {
        render3d_post_2d_end(saved_matrix_mode);
        prof_accum_end(PROF_RENDER3D_DEPTH_VIZ);
        return; /* texture would exceed the GL limit — skip this frame */
    }

    umax = (float)sw / (float)g_tex_w;
    vmax = (float)sh / (float)g_tex_h;
    /* SPLIT: one integer split coordinate drives both the quad geometry
     * and the texture crop, so odd widths stay pixel-aligned (vertex
     * boundary and texel origin land on the same column). */
    x0 = 0;
    u0 = 0.0f;
    if (mode == RENDER3D_DEPTH_VIZ_SPLIT) {
        int split = sw / 2;
        x0 = split;
        u0 = (float)split / (float)g_tex_w;
    }

    glBegin(GL_QUADS);
        glTexCoord2f(u0,   0.0f); glVertex2f((float)x0, 0.0f);
        glTexCoord2f(umax, 0.0f); glVertex2f((float)sw, 0.0f);
        glTexCoord2f(umax, vmax); glVertex2f((float)sw, (float)sh);
        glTexCoord2f(u0,   vmax); glVertex2f((float)x0, (float)sh);
    glEnd();

    if (mode == RENDER3D_DEPTH_VIZ_SPLIT) {
        glDisable(GL_TEXTURE_2D);
        glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
            glVertex2f((float)x0 + 0.5f, 0.0f);
            glVertex2f((float)x0 + 0.5f, (float)sh);
        glEnd();
    }

    render3d_post_2d_end(saved_matrix_mode);
    prof_accum_end(PROF_RENDER3D_DEPTH_VIZ);
}
