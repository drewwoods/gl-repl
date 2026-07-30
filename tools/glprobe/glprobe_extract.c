/*
 * glprobe_extract.c -- GL_FEEDBACK capture to a gl-repl-importable C snippet.
 * See glprobe_extract.h. Calls no GL and includes no GL header: it reads a
 * plain float buffer, exactly like src/support/mesh_ply.c (which handles the
 * PLY half of extraction).
 */

#include "glprobe_extract.h"

#include <stdarg.h>
#include <math.h>
#include <string.h>

#include "support/mesh_ply.h"   /* MESH_PLY_TOK_*, floats-per-vertex */

#define FPV MESH_PLY_FLOATS_PER_VERTEX

typedef struct {
    float x, y, z;
    float r, g, b;
} Vert;

/* Invert the extract pass's known transform: identity modelview, a
 * glOrtho(-R, R, -R, R, -R, R) projection, a known viewport and depth range.
 * Kept identical to the inverse mesh_ply.c applies, since both read captures
 * produced by the same pass. */
static Vert read_vert(const float *f, const GlProbeExtractCapture *cap) {
    Vert v;
    float R = cap->ortho_r;
    float ndc_x = (cap->vp_w > 0)
        ? 2.0f * (f[0] - (float)cap->vp_x) / (float)cap->vp_w - 1.0f : 0.0f;
    float ndc_y = (cap->vp_h > 0)
        ? 2.0f * (f[1] - (float)cap->vp_y) / (float)cap->vp_h - 1.0f : 0.0f;
    float dz = cap->depth_far - cap->depth_near;
    float ndc_z = (dz != 0.0f)
        ? 2.0f * (f[2] - cap->depth_near) / dz - 1.0f : 0.0f;

    v.x = ndc_x * R;
    v.y = ndc_y * R;
    v.z = -ndc_z * R;          /* glOrtho negates z */
    v.r = f[3]; v.g = f[4]; v.b = f[5];
    return v;
}

static int vert_finite(const Vert *v) {
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) &&
           isfinite(v->r) && isfinite(v->g) && isfinite(v->b);
}

/* 8-bit quantization is the granularity a color is worth emitting at: below
 * that the difference cannot survive the %.*f round-trip anyway, and treating
 * near-identical colors as equal is what collapses a per-vertex color stream
 * into a handful of glColor3f lines. */
static int color_differs(const Vert *a, const Vert *b) {
    return (int)(a->r * 255.0f + 0.5f) != (int)(b->r * 255.0f + 0.5f) ||
           (int)(a->g * 255.0f + 0.5f) != (int)(b->g * 255.0f + 0.5f) ||
           (int)(a->b * 255.0f + 0.5f) != (int)(b->b * 255.0f + 0.5f);
}

typedef struct {
    FILE *out;
    int   prec;
    int   have_color;
    Vert  last_color;
    int   lines;
    int   err;
} Emit;

static void emit_line(Emit *e, const char *fmt, ...) {
    if (e->err)
        return;
    va_list ap;
    va_start(ap, fmt);
    if (vfprintf(e->out, fmt, ap) < 0)
        e->err = 1;
    va_end(ap);
    e->lines++;
}

static void emit_vert(Emit *e, const Vert *v) {
    if (!e->have_color || color_differs(v, &e->last_color)) {
        emit_line(e, "glColor3f(%.*f, %.*f, %.*f);\n",
                  e->prec, v->r, e->prec, v->g, e->prec, v->b);
        e->last_color = *v;
        e->have_color = 1;
    }
    emit_line(e, "glVertex3f(%.*f, %.*f, %.*f);\n",
              e->prec, v->x, e->prec, v->y, e->prec, v->z);
}

int glprobe_extract_write_repl_c(FILE *out, const float *feedback,
                                 int float_count,
                                 const GlProbeExtractCapture *cap,
                                 const GlProbeExtractOptions *opts,
                                 GlProbeExtractStats *stats) {
    GlProbeExtractOptions o;
    if (opts) {
        o = *opts;
    } else {
        memset(&o, 0, sizeof o);
        o.batch = -1;
    }
    if (o.precision <= 0)
        o.precision = 4;

    GlProbeExtractStats st;
    memset(&st, 0, sizeof st);
    if (stats)
        memset(stats, 0, sizeof *stats);
    if (!out || !feedback || !cap)
        return -1;

    Emit e;
    memset(&e, 0, sizeof e);
    e.out = out;
    e.prec = o.precision;

    /* Header. auto_normals is the point of the whole emit strategy: feedback
     * carries no normals, and having gl-repl derive them per face costs zero
     * source lines where an explicit glNormal3f per vertex would double the
     * document. */
    emit_line(&e, "// Extracted by glprobe from %s.\n",
              o.source_note ? o.source_note : "a GL_FEEDBACK capture");
    emit_line(&e, "// Geometry is world space; normals are derived by gl-repl "
                  "(feedback carries none).\n");
    emit_line(&e, "// @cfg auto_normals = 1\n");

    /* The camera block is consumed by gl-repl's camera bridge rather than
     * becoming document commands, so it costs nothing against the budget --
     * and without it an extracted scene opens at gl-repl's default pose, which
     * for anything not roughly 5 units from the origin looks like the geometry
     * failed to import. */
    if (o.camera.present) {
        emit_line(&e, "// camera\n");
        emit_line(&e, "glTranslatef(0.0f, 0.0f, %.4ff);\n", -o.camera.dist);
        emit_line(&e, "glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);\n", o.camera.rx);
        emit_line(&e, "glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);\n", o.camera.ry);
        emit_line(&e, "glTranslatef(%.4ff, %.4ff, %.4ff);\n",
                  -o.camera.tx, -o.camera.ty, -o.camera.tz);
    }

    /* Everything above is comments, directives and camera lines. Reset the
     * count so `lines_written` measures what actually lands in the editor
     * buffer, which is the number the 1024-command ceiling applies to. */
    e.lines = 0;

    if (o.has_clear_color)
        emit_line(&e, "glClearColor(%.*f, %.*f, %.*f, %.*f);\n",
                  e.prec, o.clear_color[0], e.prec, o.clear_color[1],
                  e.prec, o.clear_color[2], e.prec, o.clear_color[3]);
    emit_line(&e, "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);\n");
    emit_line(&e, "glBegin(GL_TRIANGLES);\n");

    int cur_batch = -1;
    int i = 0;
    while (i < float_count) {
        int tok = (int)feedback[i++];
        int nverts;

        switch (tok) {
        case MESH_PLY_TOK_POINT:
            nverts = 1; break;
        case MESH_PLY_TOK_LINE:
        case MESH_PLY_TOK_LINE_RESET:
            nverts = 2; break;
        case MESH_PLY_TOK_POLYGON:
            if (i >= float_count)
                return -1;
            nverts = (int)feedback[i++];
            if (nverts < 3)
                return -1;
            break;
        case MESH_PLY_TOK_PASS_THROUGH:
            if (i >= float_count)
                return -1;
            cur_batch = (int)feedback[i++];
            continue;
        case MESH_PLY_TOK_BITMAP:
        case MESH_PLY_TOK_DRAW_PIXEL:
        case MESH_PLY_TOK_COPY_PIXEL:
            nverts = 1; break;
        default:
            return -1;
        }

        if (i + nverts * FPV > float_count)
            return -1;

        const float *vp = feedback + i;
        i += nverts * FPV;

        /* Only polygons become geometry: gl-repl's importer would take
         * GL_POINTS/GL_LINES too, but mixing primitive modes inside one
         * glBegin is not expressible, and a mesh extractor that silently
         * turned an app's HUD lines into triangles would be worse than one
         * that leaves them out. */
        if (tok != MESH_PLY_TOK_POLYGON)
            continue;
        if (o.batch >= 0 && cur_batch != o.batch)
            continue;

        /* Fan-triangulate in place. */
        Vert v0 = read_vert(vp, cap);
        if (!vert_finite(&v0))
            continue;
        for (int k = 1; k + 1 < nverts; k++) {
            Vert a = read_vert(vp + k * FPV, cap);
            Vert b = read_vert(vp + (k + 1) * FPV, cap);
            if (!vert_finite(&a) || !vert_finite(&b))
                continue;
            if (o.max_tris > 0 && st.tris_written >= o.max_tris) {
                st.tris_skipped++;
                continue;
            }
            emit_vert(&e, &v0);
            emit_vert(&e, &a);
            emit_vert(&e, &b);
            st.tris_written++;
            st.verts_written += 3;
        }
    }

    emit_line(&e, "glEnd();\n");

    st.lines_written = e.lines;
    if (stats)
        *stats = st;
    return e.err ? -1 : 0;
}
