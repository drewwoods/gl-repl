/*
 * glprobe.c -- GL_FEEDBACK geometry probe. See glprobe.h for the API and the
 * geometry-vs-shading split that the tool is built around.
 *
 * Only GL 1.1 + gluUnProject are used, so this drops into any fixed-function
 * sample. The PLY path reuses the project's pure writer (src/support/mesh_ply.c),
 * which touches no GL at all.
 */

#include "glprobe.h"

/* Self-contained GL include block: glprobe is meant to compile into standalone
 * samples that do not use the project's gl_includes.h shim. FREEGLUT_OSMESA
 * builds take the Mesa headers even on Apple. */
#if defined(FREEGLUT_OSMESA)
    #include <GL/gl.h>
    #include <GL/glu.h>
#elif defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glu.h>
#else
    #include <GL/gl.h>
    #include <GL/glu.h>
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "support/mesh_ply.h"

#define GLPROBE_FB_DEFAULT (1 << 20)   /* 1M floats ~ 150k vertices */
#define GLPROBE_FB_MAX     (64 << 20)
#define GLPROBE_ORTHO_R    1000.0f     /* first-pass containing cube */
#define GLPROBE_VP         1024        /* capture viewport, both axes */

/* GL_3D_COLOR: x y z r g b a. Kept in sync with the pure writer's constant so
 * the PLY path and the analysis path cannot disagree about the stride. */
#define FPV MESH_PLY_FLOATS_PER_VERTEX

/* GlProbeXform (glprobe.h) is spelled in plain double/int so the header needs
 * no GL types; pin that to the GL spelling here rather than casting at every
 * glGet call site. */
typedef char glprobe_xform_double_check[sizeof(GLdouble) == sizeof(double) ? 1 : -1];
typedef char glprobe_xform_int_check[sizeof(GLint) == sizeof(int) ? 1 : -1];

/* ---------------------------------------------------------------- capture */

static void probe_state_neutral(void) {
    /* Feedback honors lighting, polygon mode and culling: a back-facing or
     * GL_LINE-mode primitive changes the token stream. Force the state that
     * makes the stream describe the mesh itself, not the current look. */
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

/* Run one capture into buf[0..cap_floats). Returns the float count written
 * (>= 0) or < 0 on overflow, per glRenderMode(GL_RENDER). Records the
 * transform actually in force into *xf. */
static int probe_capture(int mode, GlProbeDrawFn draw, void *user,
                         float *buf, int cap_floats, float ortho_r,
                         GlProbeXform *xf) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    if (mode == GLPROBE_MODE_GEOMETRY) {
        /* Known transform: the draw callback's own emitted coordinates come
         * straight back, with a cube big enough that nothing clips. */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-ortho_r, ortho_r, -ortho_r, ortho_r, -ortho_r, ortho_r);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glViewport(0, 0, GLPROBE_VP, GLPROBE_VP);
        glDepthRange(0.0, 1.0);
        probe_state_neutral();
    }
    /* Shading mode deliberately changes NOTHING above: the whole point is that
     * the capture sees the same matrices, lights and materials the visible
     * frame does, so the colors coming back are the ones on screen. */

    glGetDoublev(GL_MODELVIEW_MATRIX, xf->mv);
    glGetDoublev(GL_PROJECTION_MATRIX, xf->proj);
    glGetIntegerv(GL_VIEWPORT, xf->vp);

    glFeedbackBuffer((GLsizei)cap_floats, GL_3D_COLOR, buf);
    glRenderMode(GL_FEEDBACK);
    if (draw)
        draw(user);
    int written = glRenderMode(GL_RENDER);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
    return written;
}

/* Capture into a buffer that grows x2 on overflow. On success *out_buf owns a
 * malloc'd buffer the caller frees and the return value is the float count.
 * Returns < 0 only when even GLPROBE_FB_MAX floats overflowed or malloc
 * failed; *out_overflow distinguishes the two. */
static int probe_capture_grow(int mode, GlProbeDrawFn draw, void *user,
                              int cap_floats, float ortho_r,
                              float **out_buf, GlProbeXform *xf,
                              int *out_overflow) {
    float *buf = NULL;
    *out_overflow = 0;
    for (;;) {
        float *nb = realloc(buf, (size_t)cap_floats * sizeof *nb);
        if (!nb) {
            free(buf);
            return -1;
        }
        buf = nb;

        int written = probe_capture(mode, draw, user, buf, cap_floats,
                                    ortho_r, xf);
        if (written >= 0) {
            *out_buf = buf;
            return written;
        }
        if (cap_floats >= GLPROBE_FB_MAX) {
            /* Report the truncated capture rather than nothing: a lower bound
             * on the counts still answers "did anything come out?". */
            *out_buf = buf;
            *out_overflow = 1;
            return cap_floats;
        }
        cap_floats <<= 1;
        if (cap_floats > GLPROBE_FB_MAX)
            cap_floats = GLPROBE_FB_MAX;
    }
}

/* --------------------------------------------------------------- analysis */

typedef struct {
    float x, y, z;        /* unprojected into the probe's coordinate space */
    float wx, wy, wz;     /* raw window coords, as feedback returned them   */
    float r, g, b, a;
    int   finite;
} ProbeVert;

static int vec_finite(float a, float b, float c) {
    return isfinite(a) && isfinite(b) && isfinite(c);
}

static void unproject(const GlProbeXform *xf, ProbeVert *v) {
    GLdouble ox = 0, oy = 0, oz = 0;
    if (gluUnProject((GLdouble)v->wx, (GLdouble)v->wy, (GLdouble)v->wz,
                     xf->mv, xf->proj, xf->vp, &ox, &oy, &oz) == GL_TRUE) {
        v->x = (float)ox;
        v->y = (float)oy;
        v->z = (float)oz;
    } else {
        /* Singular matrix (a zero-scale modelview, an unset projection):
         * keep the window coords so the report still shows something real. */
        v->x = v->wx;
        v->y = v->wy;
        v->z = v->wz;
    }
}

static ProbeVert read_vert(const float *f, const GlProbeXform *xf) {
    ProbeVert v;
    v.wx = f[0]; v.wy = f[1]; v.wz = f[2];
    v.r  = f[3]; v.g  = f[4]; v.b  = f[5]; v.a = f[6];
    v.finite = vec_finite(v.wx, v.wy, v.wz) && vec_finite(v.r, v.g, v.b) &&
               isfinite(v.a);
    if (v.finite)
        unproject(xf, &v);
    else
        v.x = v.y = v.z = 0.0f;
    return v;
}

static float luminance(const ProbeVert *v) {
    return 0.2126f * v->r + 0.7152f * v->g + 0.0722f * v->b;
}

/* Fan-triangulated area of a polygon, plus its longest edge, so a sliver can
 * be judged against its own scale rather than an absolute epsilon. */
static void poly_area(const ProbeVert *vs, int n, float *out_area,
                      float *out_max_edge) {
    float area = 0.0f, max_edge = 0.0f;
    for (int i = 1; i + 1 < n; i++) {
        float ax = vs[i].x - vs[0].x, ay = vs[i].y - vs[0].y, az = vs[i].z - vs[0].z;
        float bx = vs[i + 1].x - vs[0].x, by = vs[i + 1].y - vs[0].y,
              bz = vs[i + 1].z - vs[0].z;
        float cx = ay * bz - az * by;
        float cy = az * bx - ax * bz;
        float cz = ax * by - ay * bx;
        area += 0.5f * sqrtf(cx * cx + cy * cy + cz * cz);
    }
    for (int i = 0; i < n; i++) {
        const ProbeVert *p = &vs[i], *q = &vs[(i + 1) % n];
        float dx = q->x - p->x, dy = q->y - p->y, dz = q->z - p->z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > max_edge)
            max_edge = d;
    }
    *out_area = area;
    *out_max_edge = max_edge;
}

/* Per-primitive dump state, threaded through the parse so the printer and the
 * statistics walk the stream exactly once. */
typedef struct {
    FILE *f;
    int   remaining;   /* primitives still to print; < 0 = unlimited */
} ProbeDump;

static void dump_prim(ProbeDump *d, const char *kind, const ProbeVert *vs,
                      int n) {
    if (!d || !d->f || d->remaining == 0)
        return;
    fprintf(d->f, "  %-8s n=%d\n", kind, n);
    for (int i = 0; i < n; i++)
        fprintf(d->f,
                "    [%2d] pos (% .4f, % .4f, % .4f)  win (%7.2f,%7.2f,%6.4f)"
                "  rgba (%.3f, %.3f, %.3f, %.3f)\n",
                i, vs[i].x, vs[i].y, vs[i].z, vs[i].wx, vs[i].wy, vs[i].wz,
                vs[i].r, vs[i].g, vs[i].b, vs[i].a);
    if (d->remaining > 0)
        d->remaining--;
}

/* A report under construction. The running luminance sum has nowhere to live
 * in the public struct, so accumulation goes through this instead -- which also
 * lets one pass over the stream feed two reports at once (the whole-capture
 * total and the current glPassThrough batch). */
typedef struct {
    GlProbeReport *r;
    double lum_sum;
    int    lum_n;
} Acc;

static void acc_begin(Acc *a, GlProbeReport *r, int mode) {
    memset(r, 0, sizeof *r);
    r->mode = mode;
    r->lum_min = r->alpha_min = 1e30f;
    r->lum_max = r->alpha_max = -1e30f;
    r->bbox_min[0] = r->bbox_min[1] = r->bbox_min[2] = 1e30f;
    r->bbox_max[0] = r->bbox_max[1] = r->bbox_max[2] = -1e30f;
    a->r = r;
    a->lum_sum = 0.0;
    a->lum_n = 0;
}

static void acc_vert(Acc *a, const ProbeVert *v, const GlProbeXform *xf) {
    GlProbeReport *r = a->r;
    r->verts++;
    if (!v->finite) {
        r->nonfinite++;
        return;
    }
    if (v->x < r->bbox_min[0]) r->bbox_min[0] = v->x;
    if (v->y < r->bbox_min[1]) r->bbox_min[1] = v->y;
    if (v->z < r->bbox_min[2]) r->bbox_min[2] = v->z;
    if (v->x > r->bbox_max[0]) r->bbox_max[0] = v->x;
    if (v->y > r->bbox_max[1]) r->bbox_max[1] = v->y;
    if (v->z > r->bbox_max[2]) r->bbox_max[2] = v->z;

    float lum = luminance(v);
    if (lum < r->lum_min) r->lum_min = lum;
    if (lum > r->lum_max) r->lum_max = lum;
    if (v->a < r->alpha_min) r->alpha_min = v->a;
    if (v->a > r->alpha_max) r->alpha_max = v->a;
    a->lum_sum += lum;
    a->lum_n++;
    if (lum < GLPROBE_DARK_LUM)
        r->dark_verts++;

    /* Geometry mode installs a cube that contains everything, so it can never
     * report a clip -- the counters would only ever be noise there. */
    if (r->mode != GLPROBE_MODE_GEOMETRY) {
        if (v->wx < (float)xf->vp[0] ||
            v->wx > (float)(xf->vp[0] + xf->vp[2]) ||
            v->wy < (float)xf->vp[1] ||
            v->wy > (float)(xf->vp[1] + xf->vp[3]))
            r->offscreen++;
        if (v->wz < 0.0f || v->wz > 1.0f)
            r->depth_clipped++;
    }
}

static void acc_prim(Acc *a, int tok, int nverts, int degenerate) {
    GlProbeReport *r = a->r;
    if (tok == MESH_PLY_TOK_POINT)
        r->points++;
    else if (tok == MESH_PLY_TOK_POLYGON) {
        r->polygons++;
        r->tris += nverts - 2;
        r->degenerate += degenerate;
    } else
        r->lines++;
}

static void acc_end(Acc *a) {
    GlProbeReport *r = a->r;
    if (a->lum_n > 0)
        r->lum_mean = (float)(a->lum_sum / a->lum_n);
    if (r->verts == 0 || a->lum_n == 0) {
        r->lum_min = r->lum_max = r->lum_mean = 0.0f;
        r->alpha_min = r->alpha_max = 0.0f;
        r->bbox_min[0] = r->bbox_min[1] = r->bbox_min[2] = 0.0f;
        r->bbox_max[0] = r->bbox_max[1] = r->bbox_max[2] = 0.0f;
    }
}

/* Parse the feedback stream into *r, optionally splitting it into sub-reports
 * at each glPassThrough marker. Returns 0 on success, -1 on a malformed or
 * truncated stream (which, absent an overflow, means a GL bug or a stride
 * mismatch -- worth surfacing rather than silently under-reporting). */
static int analyze(const float *fb, int n, const GlProbeXform *xf, int mode,
                   GlProbeReport *r, GlProbeReport *batches, int max_batches,
                   ProbeDump *dump) {
    ProbeVert poly[256];
    Acc total, batch;
    int have_batch = 0;

    acc_begin(&total, r, mode);
    if (batches && max_batches > 0) {
        r->batches = batches;
        r->batch_count = 0;
    }

    int i = 0;
    while (i < n) {
        int tok = (int)fb[i++];
        int nverts = 0;
        const char *kind = NULL;

        switch (tok) {
        case MESH_PLY_TOK_POINT:
            nverts = 1; kind = "point"; break;
        case MESH_PLY_TOK_LINE:
        case MESH_PLY_TOK_LINE_RESET:
            nverts = 2; kind = "line"; break;
        case MESH_PLY_TOK_POLYGON:
            if (i >= n)
                return -1;
            nverts = (int)fb[i++];
            kind = "polygon";
            if (nverts < 3)
                return -1;
            break;
        case MESH_PLY_TOK_PASS_THROUGH:
            if (i >= n)
                return -1;
            /* A marker closes the batch in progress and opens the next. The
             * caller injected it from a state-change hook, so the split lands
             * on an object boundary without anyone naming the objects. */
            if (r->batches && r->batch_count < max_batches) {
                if (have_batch)
                    acc_end(&batch);
                float id = fb[i];
                acc_begin(&batch, &batches[r->batch_count], mode);
                batches[r->batch_count].batch_id = id;
                r->batch_count++;
                have_batch = 1;
            }
            i++;
            continue;
        case MESH_PLY_TOK_BITMAP:
        case MESH_PLY_TOK_DRAW_PIXEL:
        case MESH_PLY_TOK_COPY_PIXEL:
            nverts = 1; kind = NULL; break;   /* consumed, not counted */
        default:
            return -1;
        }

        if (i + nverts * FPV > n)
            return -1;

        /* Oversized polygons still have to be consumed and measured; only the
         * per-primitive geometry tests need the corners buffered. */
        int buffered = nverts <= (int)(sizeof poly / sizeof poly[0]) ? nverts : 0;

        for (int k = 0; k < nverts; k++) {
            ProbeVert v = read_vert(fb + i + k * FPV, xf);
            if (buffered)
                poly[k] = v;
            if (!kind)
                continue;        /* pixel/bitmap token: consumed only */
            acc_vert(&total, &v, xf);
            if (have_batch)
                acc_vert(&batch, &v, xf);
        }
        i += nverts * FPV;

        if (!kind)
            continue;

        int degenerate = 0;
        if (tok == MESH_PLY_TOK_POLYGON && buffered) {
            float area, edge;
            poly_area(poly, nverts, &area, &edge);
            degenerate = (edge <= 0.0f || area <= 1e-6f * edge * edge);
        }
        acc_prim(&total, tok, nverts, degenerate);
        if (have_batch)
            acc_prim(&batch, tok, nverts, degenerate);

        if (buffered)
            dump_prim(dump, kind, poly, nverts);
    }

    if (have_batch)
        acc_end(&batch);
    acc_end(&total);
    return 0;
}

int glprobe_analyze(const float *feedback, int float_count,
                    const GlProbeXform *xf, int mode, GlProbeReport *out,
                    GlProbeReport *batches, int max_batches) {
    if (!feedback || !xf || !out)
        return -1;
    return analyze(feedback, float_count, xf, mode, out, batches, max_batches,
                   NULL);
}

/* ------------------------------------------------------------ public API */

static void opts_defaults(const GlProbeOptions *in, GlProbeOptions *out) {
    static const GlProbeOptions kDefault = { 0, 0.0f, 0 };
    *out = in ? *in : kDefault;
    if (out->buffer_floats <= 0)
        out->buffer_floats = GLPROBE_FB_DEFAULT;
}

int glprobe_geometry(GlProbeDrawFn draw, void *user, const GlProbeOptions *opts,
                     GlProbeReport *out, const char *ply_path) {
    GlProbeOptions o;
    opts_defaults(opts, &o);

    int auto_fit = (o.ortho_r <= 0.0f);
    float ortho_r = auto_fit ? GLPROBE_ORTHO_R : o.ortho_r;

    float *buf = NULL;
    GlProbeXform xf;
    int overflow = 0;
    int n = probe_capture_grow(GLPROBE_MODE_GEOMETRY, draw, user,
                               o.buffer_floats, ortho_r, &buf, &xf, &overflow);
    if (n < 0)
        return -1;

    GlProbeReport r;
    if (analyze(buf, n, &xf, GLPROBE_MODE_GEOMETRY, &r, NULL, 0, NULL) != 0) {
        free(buf);
        return -1;
    }

    /* Second pass at a cube sized to the mesh. The first pass exists only to
     * find the extent; capturing a 0.1-unit torch inside a 1000-unit cube
     * quantizes window coords hard enough to blur a report meant to be read
     * to four decimals. */
    if (auto_fit && !overflow && r.verts > 0) {
        float ext = 0.0f;
        for (int k = 0; k < 3; k++) {
            float lo = fabsf(r.bbox_min[k]), hi = fabsf(r.bbox_max[k]);
            if (lo > ext) ext = lo;
            if (hi > ext) ext = hi;
        }
        float fit = ext * 1.25f;
        if (fit < 1e-4f)
            fit = 1e-4f;
        if (fit < ortho_r) {
            free(buf);
            buf = NULL;
            n = probe_capture_grow(GLPROBE_MODE_GEOMETRY, draw, user,
                                   o.buffer_floats, fit, &buf, &xf, &overflow);
            if (n < 0)
                return -1;
            if (analyze(buf, n, &xf, GLPROBE_MODE_GEOMETRY, &r, NULL, 0,
                        NULL) != 0) {
                free(buf);
                return -1;
            }
            ortho_r = fit;
        }
    }

    r.overflow = overflow;
    if (out)
        *out = r;

    int rc = 0;
    if (ply_path) {
        FILE *fp = fopen(ply_path, "w");
        if (!fp) {
            rc = -1;
        } else {
            MeshPlyCapture cap;
            cap.ortho_r = ortho_r;
            cap.vp_x = 0; cap.vp_y = 0;
            cap.vp_w = GLPROBE_VP; cap.vp_h = GLPROBE_VP;
            cap.depth_near = 0.0f; cap.depth_far = 1.0f;
            cap.floats_per_vertex = MESH_PLY_FLOATS_PER_VERTEX;

            MeshPlyOptions po;
            memset(&po, 0, sizeof po);
            po.weld = 1;
            po.weld_eps = 1e-4f;
            po.smooth_normals = 1;
            po.triangulate = 1;
            /* Points and lines get triangle proxies: a probe dump is usually
             * opened in Quick Look or Blender, and both ignore PLY loose
             * vertices and edge records. */
            po.primitive_radius_scale = 0.0025f;

            if (mesh_ply_write(fp, buf, n, &cap, &po, NULL) < 0)
                rc = -1;
            if (fclose(fp) != 0)
                rc = -1;
        }
    }

    free(buf);
    return rc;
}

int glprobe_shading(GlProbeDrawFn draw, void *user, const GlProbeOptions *opts,
                    GlProbeReport *out) {
    GlProbeOptions o;
    opts_defaults(opts, &o);

    float *buf = NULL;
    GlProbeXform xf;
    int overflow = 0;
    int n = probe_capture_grow(GLPROBE_MODE_SHADING, draw, user,
                               o.buffer_floats, 0.0f, &buf, &xf, &overflow);
    if (n < 0)
        return -1;

    GlProbeReport r;
    int rc = analyze(buf, n, &xf, GLPROBE_MODE_SHADING, &r, NULL, 0, NULL);
    r.overflow = overflow;
    free(buf);
    if (rc != 0)
        return -1;
    if (out)
        *out = r;
    return 0;
}

void glprobe_report_print(const GlProbeReport *r, const char *label,
                          const GlProbeOptions *opts, FILE *f) {
    if (!r)
        return;
    if (!f)
        f = stderr;
    (void)opts;   /* per-primitive dump is emitted by glprobe_diagnose */

    const char *mode;
    switch (r->mode) {
    case GLPROBE_MODE_GEOMETRY: mode = "geometry (known ortho, lighting off)"; break;
    case GLPROBE_MODE_AS_DRAWN: mode = "as-drawn (whole frame, app state untouched)"; break;
    case GLPROBE_MODE_NEUTRAL:  mode = "neutral  (whole frame, culling/clip/lighting off)"; break;
    default:                    mode = "shading  (live camera + lights)"; break;
    }
    fprintf(f, "\n== glprobe: %s ==\n   mode: %s\n", label ? label : "(unnamed)", mode);

    if (r->overflow)
        fprintf(f, "!! feedback buffer OVERFLOWED -- counts below are lower bounds\n");

    fprintf(f, "   prims: %d polygons (%d tris), %d lines, %d points; %d verts\n",
            r->polygons, r->tris, r->lines, r->points, r->verts);

    if (r->verts == 0) {
        fprintf(f, "!! NOTHING CAPTURED -- the draw callback emitted no primitives.\n"
                   "!! Look for: an early return, a zero loop bound, geometry outside\n"
                   "!! the capture cube, or glBegin/glEnd never actually reached.\n");
        return;
    }

    fprintf(f, "   bbox:  x [% .4f, % .4f]  y [% .4f, % .4f]  z [% .4f, % .4f]\n",
            r->bbox_min[0], r->bbox_max[0], r->bbox_min[1], r->bbox_max[1],
            r->bbox_min[2], r->bbox_max[2]);
    fprintf(f, "   size:  % .4f x % .4f x % .4f\n",
            r->bbox_max[0] - r->bbox_min[0], r->bbox_max[1] - r->bbox_min[1],
            r->bbox_max[2] - r->bbox_min[2]);
    fprintf(f, "   color: luminance min %.4f  mean %.4f  max %.4f;"
               " alpha [%.3f, %.3f]\n",
            r->lum_min, r->lum_mean, r->lum_max, r->alpha_min, r->alpha_max);

    if (r->nonfinite)
        fprintf(f, "!! %d vertices are NaN/Inf -- a divide by zero or an "
                   "uninitialized value reached glVertex/glColor.\n", r->nonfinite);
    if (r->degenerate)
        fprintf(f, "!! %d of %d polygons are degenerate (zero area) -- duplicated "
                   "corners or a collapsed ring.\n", r->degenerate, r->polygons);
    if (r->offscreen)
        fprintf(f, "!! %d of %d vertices land OUTSIDE the viewport -- the camera is "
                   "not pointed at this geometry.\n", r->offscreen, r->verts);
    if (r->depth_clipped)
        fprintf(f, "!! %d of %d vertices are outside the depth range -- the near/far "
                   "planes are cutting this geometry.\n", r->depth_clipped, r->verts);

    if (r->mode == GLPROBE_MODE_SHADING || r->mode == GLPROBE_MODE_AS_DRAWN) {
        int pct = r->verts ? (100 * r->dark_verts) / r->verts : 0;
        if (r->dark_verts == r->verts)
            fprintf(f, "!! EVERY vertex is darker than %.2f luminance -- the geometry "
                       "is drawn but unlit.\n"
                       "!! Check: N.L sign (is the light inside/behind the surface?),\n"
                       "!! GL_LIGHTING/GL_LIGHTn enabled, material diffuse non-zero,\n"
                       "!! light position set AFTER the camera matrix.\n",
                    GLPROBE_DARK_LUM);
        else if (pct >= 50)
            fprintf(f, "!! %d%% of vertices are darker than %.2f luminance -- large "
                       "parts of this mesh face away from every light.\n",
                    pct, GLPROBE_DARK_LUM);
        if (r->alpha_max <= 0.0f)
            fprintf(f, "!! every vertex has alpha 0 -- fully transparent under a "
                       "blended draw.\n");
    }

    /* Per-batch breakdown. Each row is the geometry drawn between two state
     * changes, which is as close to "one object" as a capture can get without
     * being told where the objects are. Empty batches are skipped: a state
     * change that draws nothing is noise, not a finding. */
    int printed = 0, suppressed = 0;
    for (int b = 0; b < r->batch_count; b++) {
        const GlProbeReport *bt = &r->batches[b];
        if (bt->verts == 0)
            continue;
        if (printed >= GLPROBE_REPORT_BATCH_ROWS) {
            suppressed++;
            continue;
        }
        printed++;
        fprintf(f, "   %-38s %5d tris  lum max %.4f%s%s\n",
                bt->batch_label[0] ? bt->batch_label : "(unlabeled batch)",
                bt->tris, bt->lum_max,
                (bt->verts && bt->dark_verts == bt->verts) ? "  !! all dark" : "",
                (bt->verts && bt->offscreen == bt->verts) ? "  !! all off-screen" : "");
    }
    if (suppressed)
        fprintf(f, "   ... and %d more batches (per-tile material changes make "
                   "long lists; see the extractor for the full set)\n",
                suppressed);
}

int glprobe_diagnose(GlProbeDrawFn draw, void *user, const char *label,
                     const GlProbeOptions *opts, const char *ply_path,
                     FILE *f) {
    if (!f)
        f = stderr;
    GlProbeOptions o;
    opts_defaults(opts, &o);

    GlProbeReport geom, shade;
    int rc = 0;

    if (glprobe_geometry(draw, user, &o, &geom, ply_path) != 0) {
        fprintf(f, "!! glprobe: geometry capture FAILED for %s\n",
                label ? label : "(unnamed)");
        rc = -1;
    } else {
        glprobe_report_print(&geom, label, &o, f);
        if (ply_path)
            fprintf(f, "   wrote: %s\n", ply_path);
        if (o.dump_primitives != 0) {
            /* Re-capture purely to print; a dump is an interactive action, so
             * the second pass costs nothing that matters. */
            float *buf = NULL;
            GlProbeXform xf;
            int overflow = 0;
            int n = probe_capture_grow(GLPROBE_MODE_GEOMETRY, draw, user,
                                       o.buffer_floats, GLPROBE_ORTHO_R,
                                       &buf, &xf, &overflow);
            if (n >= 0) {
                GlProbeReport tmp;
                ProbeDump d;
                d.f = f;
                d.remaining = o.dump_primitives;
                fprintf(f, "   primitives:\n");
                analyze(buf, n, &xf, GLPROBE_MODE_GEOMETRY, &tmp, NULL, 0,
                        &d);
                free(buf);
            }
        }
    }

    if (glprobe_shading(draw, user, &o, &shade) != 0) {
        fprintf(f, "!! glprobe: shading capture FAILED for %s\n",
                label ? label : "(unnamed)");
        rc = -1;
    } else {
        glprobe_report_print(&shade, label, &o, f);
    }

    /* The comparison is the whole product: same mesh, two lenses. */
    if (rc == 0) {
        fprintf(f, "   verdict: ");
        if (geom.verts == 0)
            fprintf(f, "no geometry at all -- the bug is upstream of GL.\n");
        else if (shade.verts == 0)
            fprintf(f, "geometry exists but nothing survives the live pipeline "
                       "-- culling, clipping or polygon mode is removing it.\n");
        else if (shade.offscreen == shade.verts)
            fprintf(f, "geometry is fine and entirely off-screen -- a camera "
                       "problem, not a mesh problem.\n");
        else if (shade.dark_verts == shade.verts && geom.verts > 0)
            fprintf(f, "geometry is FINE (%d tris) and the lighting is the bug "
                       "-- every lit vertex came back black.\n", geom.tris);
        else
            fprintf(f, "geometry present and lit; look at blending, depth or "
                       "draw order next.\n");
    }
    fprintf(f, "\n");
    return rc;
}
