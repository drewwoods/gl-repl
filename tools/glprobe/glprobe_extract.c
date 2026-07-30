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

    /* glBegin is opened lazily and closed whenever the batch state changes:
     * glMaterialfv between glBegin and glEnd is invalid GL, so a material
     * switch has to break the primitive block rather than sit inside it. */
    int   in_begin;

    /* Last state actually written, so a batch that repeats the previous
     * material emits nothing. Worth the bookkeeping: a checkerboard ground
     * alternates two materials across dozens of batches. */
    int   have_state;
    GlProbeExtractBatch last_state;
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

static int vec4_same(const float *a, const float *b) {
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void emit_vec4_material(Emit *e, const char *pname, const float *v) {
    emit_line(e, "glMaterialfv(GL_FRONT_AND_BACK, %s, (GLfloat[]){%.*f, %.*f, "
                 "%.*f, %.*f});\n", pname,
              e->prec, v[0], e->prec, v[1], e->prec, v[2], e->prec, v[3]);
}

/* Close any open primitive block and write whatever changed between the last
 * emitted state and this batch's. */
static void emit_batch_state(Emit *e, const GlProbeExtractBatch *b) {
    const GlProbeExtractBatch *p = e->have_state ? &e->last_state : NULL;

    if (e->in_begin) {
        emit_line(e, "glEnd();\n");
        e->in_begin = 0;
    }

    if (!p || p->lit != b->lit)
        emit_line(e, "gl%sable(GL_LIGHTING);\n", b->lit ? "En" : "Dis");

    if (b->lit) {
        /* Diffuse goes out as glColor3f under GL_COLOR_MATERIAL rather than as
         * glMaterialfv(GL_DIFFUSE). Both parse, but color-material is the
         * idiom gl-repl's own scenes use and the one its default light rig is
         * tuned against -- an extraction that opts out with
         * glDisable(GL_COLOR_MATERIAL) loads correctly and renders nearly
         * black. It is also one command instead of two. */
        if (!p || !p->lit)
            emit_line(e, "glEnable(GL_COLOR_MATERIAL);\n");
        if (!p || !p->lit)
            emit_line(e, "glColorMaterial(GL_FRONT_AND_BACK, "
                         "GL_AMBIENT_AND_DIFFUSE);\n");
        if (b->has_material) {
            if (!p || !p->has_material || !vec4_same(p->specular, b->specular))
                emit_vec4_material(e, "GL_SPECULAR", b->specular);
            if (!p || !p->has_material || p->shininess != b->shininess)
                emit_line(e, "glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, "
                             "%.*f);\n", e->prec, b->shininess);
        }
        /* The batch's own color, unless the app was already driving diffuse
         * from glColor -- in which case the per-vertex stream is the truth. */
        if (!b->color_material) {
            if (!p || !p->has_material || !vec4_same(p->diffuse, b->diffuse) ||
                !p->lit)
                emit_line(e, "glColor3f(%.*f, %.*f, %.*f);\n",
                          e->prec, b->diffuse[0], e->prec, b->diffuse[1],
                          e->prec, b->diffuse[2]);
            /* That glColor3f is now the live color; make the next unlit batch
             * re-state its own rather than diffing against a stale value. */
            e->have_color = 0;
        }
    }

    e->last_state = *b;
    e->have_state = 1;
}

static int batch_uses_vertex_color(const GlProbeExtractBatch *b) {
    return !b || !b->lit || b->color_material;
}

static void emit_vert(Emit *e, const Vert *v, const GlProbeExtractBatch *b) {
    if (batch_uses_vertex_color(b) &&
        (!e->have_color || color_differs(v, &e->last_color))) {
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
    /* An extracted mesh has thousands of vertices; the per-vertex point and
     * outline overlays are tuned for hand-typed scenes with a dozen and turn
     * this into confetti. Off by default here, not because they are wrong but
     * because at this density they hide the thing being inspected. */
    emit_line(&e, "// @cfg vertex_points = 0\n");
    emit_line(&e, "// @cfg vertex_outlines = 0\n");

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

    int cur_batch = -1;
    int emitted_batch = -2;
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

        const GlProbeExtractBatch *bs =
            (o.batches && cur_batch >= 0 && cur_batch < o.batch_count)
                ? &o.batches[cur_batch] : NULL;

        for (int k = 1; k + 1 < nverts; k++) {
            Vert a = read_vert(vp + k * FPV, cap);
            Vert b = read_vert(vp + (k + 1) * FPV, cap);
            if (!vert_finite(&a) || !vert_finite(&b))
                continue;
            if (o.max_tris > 0 && st.tris_written >= o.max_tris) {
                st.tris_skipped++;
                continue;
            }
            /* State first, then open the block -- deferred to here so a batch
             * whose primitives were all filtered out emits nothing at all. */
            if (bs && (!e.have_state || emitted_batch != cur_batch)) {
                emit_batch_state(&e, bs);
                emitted_batch = cur_batch;
            }
            if (!e.in_begin) {
                emit_line(&e, "glBegin(GL_TRIANGLES);\n");
                e.in_begin = 1;
            }
            emit_vert(&e, &v0, bs);
            emit_vert(&e, &a, bs);
            emit_vert(&e, &b, bs);
            st.tris_written++;
            st.verts_written += 3;
        }
    }

    if (e.in_begin)
        emit_line(&e, "glEnd();\n");

    st.lines_written = e.lines;
    if (stats)
        *stats = st;
    return e.err ? -1 : 0;
}
