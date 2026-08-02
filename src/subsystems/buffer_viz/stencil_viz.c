/*
 * stencil_viz.c - see stencil_viz.h.
 *
 * Conversion core (buffer_viz_stencil_scan / _map - no GL, unit-tested
 * with synthetic buffers) plus a thin GL shell: capture (glReadPixels
 * GL_STENCIL_INDEX into a persistent byte buffer, grown on resize) and
 * render (RGBA conversion + POT texture sub-upload + a blended
 * screen-space quad, reusing postprocess_filter's 2D bracket).
 * GL_NEAREST filtering - the quad is a 1:1 pixel overlay.
 */
#include "subsystems/buffer_viz/stencil_viz.h"

#include "render3d/postprocess_filter.h"   /* render3d_post_2d_begin/_end */
#include "gl_includes.h"
#include "support/cpuprof.h"

#include <stdlib.h>
#include <string.h>

/* Below this span the RAMP normalization would divide by ~0 (every
 * non-zero pixel carries the same value); those pixels map to the middle
 * of the ramp instead. Stencil values are integers, so anything under 1
 * is a single-value buffer. */
#define STENCIL_VIZ_SPAN_EPS 0.5f

/* Fixed 16-entry palette, indexed `value & 15`. Chosen for mutual
 * separability at BUFFER_VIZ_STENCIL_ALPHA over an arbitrary scene, and
 * kept HERE rather than in ui_theme: docs/MODULES.md keeps computed and
 * data palettes out of the theme module, and these are data the
 * conversion core indexes, not chrome colours a theme could restyle.
 * Index 0 is the colour of value 16, 32, … - value 0 itself never draws. */
static const unsigned char k_stencil_palette[16][3] = {
    { 120, 120, 128 },  /*  0 (only reached by 16, 32, ...) */
    {  84, 196, 255 },  /*  1  cyan-blue    */
    { 255, 138,  72 },  /*  2  orange       */
    { 126, 224, 130 },  /*  3  green        */
    { 236, 122, 232 },  /*  4  magenta      */
    { 246, 222,  96 },  /*  5  yellow       */
    { 118, 140, 255 },  /*  6  indigo       */
    { 255, 108, 122 },  /*  7  salmon       */
    {  96, 214, 206 },  /*  8  teal         */
    { 202, 158, 255 },  /*  9  violet       */
    { 176, 214,  92 },  /* 10  lime         */
    { 255, 176, 196 },  /* 11  pink         */
    {  92, 168, 148 },  /* 12  deep teal    */
    { 214, 148,  86 },  /* 13  bronze       */
    { 150, 190, 232 },  /* 14  steel        */
    { 232, 232, 240 },  /* 15  near-white   */
};

/* RAMP endpoints: cool/dim at the low value, warm/bright at the high one,
 * so the ordering reads as brightness even in a screenshot. */
static const float k_ramp_lo[3] = { 0.22f, 0.36f, 0.92f };
static const float k_ramp_hi[3] = { 1.00f, 0.84f, 0.28f };

/* Persistent capture + conversion buffers (one allocation per resize). */
static unsigned char *g_stencil = NULL;
static unsigned char *g_rgba    = NULL;
static size_t         g_buf_px  = 0;
static int            g_cap_w   = 0;
static int            g_cap_h   = 0;
static int            g_cap_valid = 0;

/* One owned POT RGBA texture, sized to the largest rect seen. */
static GLuint g_tex   = 0;
static int    g_tex_w = 0;
static int    g_tex_h = 0;
/* GL_MAX_TEXTURE_SIZE, queried once: 0 = not yet, -1 = unknown (GL
 * stubs) so the size guard is not enforced. */
static GLint  g_max_tex_size = 0;

/* The smoothed range, and the frame-local copy every pass of one frame
 * shares (see the is_final_pass note in the header). */
static BufferVizRange g_range = { 0.0f, 0.0f, 0 };
static BufferVizRange g_frame_range = { 0.0f, 0.0f, 0 };
static int            g_frame_range_valid = 0;

/* Scratch is written by every pass; g_hist is what the legend reads and
 * is only republished on the final pass. */
static BufferVizStencilHistogram g_hist;
static BufferVizStencilHistogram g_hist_scratch;

void buffer_viz_stencil_reset(void) {
    free(g_stencil);
    g_stencil = NULL;
    free(g_rgba);
    g_rgba = NULL;
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
    g_frame_range = g_range;
    g_frame_range_valid = 0;
    memset(&g_hist, 0, sizeof g_hist);
    memset(&g_hist_scratch, 0, sizeof g_hist_scratch);
}

void buffer_viz_stencil_palette_rgb(int value, unsigned char *rgb_out) {
    const unsigned char *entry;
    if (!rgb_out)
        return;
    entry = k_stencil_palette[(unsigned)value & 15u];
    rgb_out[0] = entry[0];
    rgb_out[1] = entry[1];
    rgb_out[2] = entry[2];
}

void buffer_viz_stencil_swatch_rgb(int value, BufferVizStencilMode mode,
                                   unsigned char *rgb_out) {
    unsigned char v = (unsigned char)(value & 0xFF);
    unsigned char rgba[4];

    if (!rgb_out)
        return;
    /* Route through the same _map() the overlay uses rather than
     * re-deriving the ramp here: one mapping, so the swatch cannot drift
     * from the pixels. g_frame_range is the range the last rendered pass
     * mapped with, which is exactly the frame the published histogram
     * came from. */
    if (v == 0 || mode <= BUFFER_VIZ_STENCIL_OFF ||
        mode >= BUFFER_VIZ_STENCIL_COUNT) {
        buffer_viz_stencil_palette_rgb(value, rgb_out);
        return;
    }
    buffer_viz_stencil_map(&v, 1, mode, &g_frame_range, rgba);
    rgb_out[0] = rgba[0];
    rgb_out[1] = rgba[1];
    rgb_out[2] = rgba[2];
}

int buffer_viz_stencil_scan(const unsigned char *stencil, int count,
                            BufferVizStencilHistogram *hist_out,
                            float *lo_out, float *hi_out) {
    unsigned int counts[256];
    int distinct = 0;
    int nonzero_px = 0;
    int lo = -1, hi = -1;
    int i;

    memset(counts, 0, sizeof counts);
    if (stencil && count > 0) {
        for (i = 0; i < count; i++)
            counts[stencil[i]]++;
    }
    /* Bin 0 is deliberately skipped for BOTH the extent and `distinct`:
     * zero is the clear value, it is transparent in every mode, and
     * letting it into the range would pin lo at 0 forever. */
    for (i = 1; i < 256; i++) {
        if (!counts[i])
            continue;
        distinct++;
        nonzero_px += (int)counts[i];
        if (lo < 0)
            lo = i;
        hi = i;
    }
    if (hist_out) {
        memcpy(hist_out->counts, counts, sizeof counts);
        hist_out->distinct = distinct;
        hist_out->total_px = (stencil && count > 0) ? count : 0;
        hist_out->nonzero_px = nonzero_px;
        hist_out->valid = 1;
    }
    if (lo < 0)
        return 0;
    if (lo_out) *lo_out = (float)lo;
    if (hi_out) *hi_out = (float)hi;
    return 1;
}

static unsigned char stencil_byte(float v01) {
    if (v01 <= 0.0f) return 0;
    if (v01 >= 1.0f) return 255;
    return (unsigned char)(v01 * 255.0f + 0.5f);
}

void buffer_viz_stencil_map(const unsigned char *stencil, int count,
                            BufferVizStencilMode mode,
                            const BufferVizRange *range,
                            unsigned char *rgba_out) {
    int ramp;
    float lo = 0.0f, span = 0.0f;
    int degenerate = 1;
    int i;

    if (!stencil || !rgba_out || count <= 0)
        return;
    if (mode <= BUFFER_VIZ_STENCIL_OFF || mode >= BUFFER_VIZ_STENCIL_COUNT)
        return;

    ramp = (mode == BUFFER_VIZ_STENCIL_RAMP);
    if (ramp && range && range->valid) {
        lo = range->lo;
        span = range->hi - range->lo;
        degenerate = (span < STENCIL_VIZ_SPAN_EPS);
    }

    for (i = 0; i < count; i++) {
        int v = stencil[i];
        unsigned char *out = rgba_out + (size_t)i * 4;
        if (v == 0) {
            /* Untouched (or explicitly zeroed - indistinguishable, see the
             * header): fully transparent, so the scene shows through. */
            out[0] = out[1] = out[2] = 0;
            out[3] = 0;
            continue;
        }
        if (ramp) {
            float t = degenerate ? 0.5f : (((float)v - lo) / span);
            if (t < 0.0f) t = 0.0f;   /* EMA lag can put this frame's */
            if (t > 1.0f) t = 1.0f;   /* values outside the smoothed range */
            out[0] = stencil_byte(k_ramp_lo[0] + (k_ramp_hi[0] - k_ramp_lo[0]) * t);
            out[1] = stencil_byte(k_ramp_lo[1] + (k_ramp_hi[1] - k_ramp_lo[1]) * t);
            out[2] = stencil_byte(k_ramp_lo[2] + (k_ramp_hi[2] - k_ramp_lo[2]) * t);
        } else {
            buffer_viz_stencil_palette_rgb(v, out);
        }
        out[3] = BUFFER_VIZ_STENCIL_ALPHA;
    }
}

void buffer_viz_stencil_capture(int sx, int sy, int sw, int sh) {
    size_t px;
    GLint saved_align = 0;

    g_cap_valid = 0;
    if (sw <= 0 || sh <= 0)
        return;
    prof_begin(PROF_BUFFER_VIZ_STENCIL);
    px = (size_t)sw * (size_t)sh;
    if (px > g_buf_px) {
        unsigned char *new_stencil = (unsigned char *)malloc(px);
        unsigned char *new_rgba = (unsigned char *)malloc(px * 4);
        if (!new_stencil || !new_rgba) {
            free(new_stencil);
            free(new_rgba);
            prof_accum_end(PROF_BUFFER_VIZ_STENCIL);
            return; /* degrade to no capture this pass */
        }
        free(g_stencil);
        free(g_rgba);
        g_stencil = new_stencil;
        g_rgba = new_rgba;
        g_buf_px = px;
    }
    /* Unlike depth's float rows, single-byte rows are NOT naturally
     * 4-byte aligned, so an odd-width rect would be read with row
     * padding the conversion never accounts for. Pixel-store is client
     * state outside glPushAttrib, so save/restore it explicitly. */
    glGetIntegerv(GL_PACK_ALIGNMENT, &saved_align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(sx, sy, sw, sh, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, g_stencil);
    glPixelStorei(GL_PACK_ALIGNMENT, saved_align > 0 ? saved_align : 4);
    g_cap_w = sw;
    g_cap_h = sh;
    g_cap_valid = 1;
    prof_accum_end(PROF_BUFFER_VIZ_STENCIL);
}

/* See postprocess_filter.c: GL 1.1 baseline needs power-of-two texture
 * dimensions; the umax/vmax subregion renders correctly on 1.1 and 2.0+. */
static int stencil_viz_next_pow2(int v) {
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* Ensure the POT RGBA texture exists at >= sw x sh, bind it, and
 * sub-upload the converted bytes. Runs inside the 2D bracket so the
 * pushed GL_TEXTURE_BIT restores the caller's binding on pop. Returns 0
 * when the required POT size would exceed GL_MAX_TEXTURE_SIZE. */
static int stencil_viz_upload_texture(int sw, int sh) {
    int tex_w, tex_h;
    GLint saved_align = 0;

    if (g_max_tex_size == 0) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_tex_size);
        if (g_max_tex_size <= 0)
            g_max_tex_size = -1; /* unknown (stubs): don't enforce */
    }
    tex_w = stencil_viz_next_pow2(sw);
    tex_h = stencil_viz_next_pow2(sh);
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_tex_w = tex_w;
        g_tex_h = tex_h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex);
    }

    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved_align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved_align > 0 ? saved_align : 4);
    return 1;
}

static void stencil_viz_render_pass(BufferVizStencilMode mode,
                                    int sx, int sy, int sw, int sh) {
    GLint saved_matrix_mode = 0;
    float umax, vmax, u0;
    int x0;

    if (sw <= 0 || sh <= 0)
        return;
    if (!g_cap_valid || g_cap_w != sw || g_cap_h != sh)
        return;
    g_cap_valid = 0; /* consume: never redraw a stale capture */

    prof_begin(PROF_BUFFER_VIZ_STENCIL);
    {
        float raw_lo = 0.0f, raw_hi = 0.0f;
        int found = buffer_viz_stencil_scan(g_stencil, sw * sh, &g_hist_scratch,
                                            &raw_lo, &raw_hi);
        /* Fold the smoothed range ONCE per frame; every remaining
         * accumulation pass reuses the result verbatim so the colours
         * cannot drift between blur samples of the same frame. */
        if (!g_frame_range_valid) {
            if (found)
                buffer_viz_range_update(&g_range, raw_lo, raw_hi);
            g_frame_range = g_range;
            g_frame_range_valid = 1;
        }
    }
    buffer_viz_stencil_map(g_stencil, sw * sh, mode, &g_frame_range, g_rgba);

    render3d_post_2d_begin(sx, sy, sw, sh, &saved_matrix_mode);
    /* The bracket disables GL_BLEND and selects GL_REPLACE, which is right
     * for depth's full-rect replacement and fatal for a sparse overlay -
     * every zero-valued (transparent) pixel would still be written. Turn
     * blending back on here: the bracket opened with
     * glPushAttrib(GL_ALL_ATTRIB_BITS), so its glPopAttrib undoes this.
     * GL_REPLACE stays correct - it takes RGB *and* alpha from the
     * texture, which is exactly what drives the composite.
     *
     * The bracket also disables GL_STENCIL_TEST, which matters more than
     * it looks: otherwise this quad would be clipped by the very stencil
     * state it exists to show. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!stencil_viz_upload_texture(sw, sh)) {
        render3d_post_2d_end(saved_matrix_mode);
        prof_accum_end(PROF_BUFFER_VIZ_STENCIL);
        return; /* texture would exceed the GL limit - skip this pass */
    }

    umax = (float)sw / (float)g_tex_w;
    vmax = (float)sh / (float)g_tex_h;
    /* SPLIT: one integer split coordinate drives both the quad geometry
     * and the texture crop, so odd widths stay pixel-aligned. */
    x0 = 0;
    u0 = 0.0f;
    if (mode == BUFFER_VIZ_STENCIL_SPLIT) {
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

    if (mode == BUFFER_VIZ_STENCIL_SPLIT) {
        glDisable(GL_TEXTURE_2D);
        glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
            glVertex2f((float)x0 + 0.5f, 0.0f);
            glVertex2f((float)x0 + 0.5f, (float)sh);
        glEnd();
    }

    render3d_post_2d_end(saved_matrix_mode);
    prof_accum_end(PROF_BUFFER_VIZ_STENCIL);
}

void buffer_viz_stencil_render(BufferVizStencilMode mode, int is_final_pass,
                               int sx, int sy, int sw, int sh) {
    if (mode <= BUFFER_VIZ_STENCIL_OFF || mode >= BUFFER_VIZ_STENCIL_COUNT) {
        /* Off or out of range: drop the published histogram so a legend
         * built from it disappears together with the overlay, and clear
         * the frame latch so the next frame that turns the viz on starts
         * from a fresh fold rather than a stale one. */
        g_hist.valid = 0;
        g_frame_range_valid = 0;
        return;
    }
    stencil_viz_render_pass(mode, sx, sy, sw, sh);
    /* Frame boundary. Done here rather than inside the pass helper so it
     * still runs when the pass bailed early (no capture, texture over the
     * GL limit) - otherwise one skipped final pass would leave the latch
     * set and the next frame would reuse a stale range forever. */
    if (is_final_pass) {
        g_hist = g_hist_scratch;
        g_frame_range_valid = 0;
    }
}

const BufferVizStencilHistogram *buffer_viz_stencil_histogram(void) {
    return &g_hist;
}
