/*
 * glprobe_preload.c -- the glprobe geometry probe as an injectable library, so
 * a sample can be probed WITHOUT touching its source.
 *
 *   DYLD_INSERT_LIBRARIES=libglprobe_preload.dylib GLPROBE=1 ./sample   (macOS)
 *   LD_PRELOAD=libglprobe_preload.so GLPROBE=1 ./sample                 (Linux)
 *
 * How it gets a frame hook without source access: it interposes
 * glutDisplayFunc(), keeps the app's callback for itself, and installs its own.
 * On the armed frame that wrapper runs the app's display function two extra
 * times inside glRenderMode(GL_FEEDBACK) -- which rasterizes nothing -- before
 * running it for real. glutSwapBuffers() is interposed too, and swallowed
 * during those passes, so the extra runs are invisible.
 *
 * The two passes are the same idea as the in-process API's two lenses, but
 * chosen for what an outsider can actually change about a frame it does not
 * understand:
 *
 *   AS_DRAWN -- nothing touched. Exactly the frame on screen.
 *   NEUTRAL  -- culling, clip planes, scissor, polygon mode and lighting forced
 *               off for the whole pass (the app's own glEnable calls for those
 *               are swallowed while it runs). More primitives here than in
 *               AS_DRAWN means state is eating geometry; the same primitives
 *               but black in AS_DRAWN means lighting is.
 *
 * WHAT THIS CANNOT DO, and why the in-process API still exists: the geometry
 * lens. Overriding the modelview to get camera-independent model-space
 * coordinates is impossible from out here -- the app's display callback calls
 * glLoadIdentity() and installs its own camera, which is precisely the thing
 * that would have to be suppressed. Coordinates are recovered by unprojecting
 * through the view matrix snooped from gluLookAt(), so they are world-space
 * when the app uses gluLookAt and window-space otherwise (the report says
 * which). Per-function attribution is likewise gone -- there are no function
 * boundaries out here, only state changes, which is what the batch markers
 * below approximate.
 *
 * Interposition requires the GLUT the sample links to be a DYNAMIC library.
 * A sample statically linked against the vendored freeglut archive has no
 * glutDisplayFunc symbol left to interpose; see tools/glprobe/README.md.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* RTLD_NEXT on glibc; harmless elsewhere */
#endif

#include "glprobe.h"
#include "glprobe_extract.h"
#include "glprobe_glut.h"

#include "support/mesh_ply.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------ interposition glue */

#if defined(__APPLE__)
/* dyld's documented mechanism (dyld-interposing.h). The __interpose section
 * rebinds every OTHER image's calls to `replacee`, but deliberately not this
 * image's own -- which is what lets a replacement call the real function by
 * its plain name below. */
#define DYLD_INTERPOSE(_replacement, _replacee)                                \
    __attribute__((used)) static struct {                                      \
        const void *replacement;                                               \
        const void *replacee;                                                  \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement,                            \
        (const void *)(unsigned long)&_replacee                                \
    };
#define GLPROBE_REPLACEMENT(name) glprobe_##name
#define REAL(name) name
#define INTERPOSE(name) DYLD_INTERPOSE(glprobe_##name, name)

#else
/* ELF: the preloaded definition shadows the real symbol outright, and the
 * original is fetched from the next object in the search order. */
#include <dlfcn.h>
#define GLPROBE_REPLACEMENT(name) name
#define INTERPOSE(name) /* shadowing is the mechanism; nothing to declare */

static void *real_sym(const char *name) {
    void *p = dlsym(RTLD_NEXT, name);
    if (!p) {
        /* dlerror() clears the error as a side effect, so it gets read once. */
        const char *err = dlerror();
        fprintf(stderr, "glprobe: cannot resolve real %s (%s)\n", name,
                err ? err : "no error reported");
        abort();
    }
    return p;
}
#define REAL_DECL(ret, name, params, args)                                     \
    static ret real_##name params {                                            \
        typedef ret (*fn_t) params;                                            \
        static fn_t fn;                                                        \
        if (!fn) fn = (fn_t)real_sym(#name);                                   \
        return fn args;                                                        \
    }
#define REAL_DECL_VOID(name, params, args)                                     \
    static void real_##name params {                                           \
        typedef void (*fn_t) params;                                           \
        static fn_t fn;                                                        \
        if (!fn) fn = (fn_t)real_sym(#name);                                   \
        fn args;                                                               \
    }
#define REAL(name) real_##name

REAL_DECL_VOID(glutDisplayFunc, (void (*func)(void)), (func))
REAL_DECL_VOID(glutSwapBuffers, (void), ())
REAL_DECL_VOID(gluLookAt, (GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx,
                           GLdouble cy, GLdouble cz, GLdouble ux, GLdouble uy,
                           GLdouble uz),
                          (ex, ey, ez, cx, cy, cz, ux, uy, uz))
REAL_DECL_VOID(glEnable, (GLenum cap), (cap))
REAL_DECL_VOID(glDisable, (GLenum cap), (cap))
REAL_DECL_VOID(glMaterialfv, (GLenum face, GLenum pname, const GLfloat *params),
                             (face, pname, params))
REAL_DECL_VOID(glMaterialf, (GLenum face, GLenum pname, GLfloat param),
                            (face, pname, param))
REAL_DECL_VOID(glBindTexture, (GLenum target, GLuint texture),
                              (target, texture))
REAL_DECL_VOID(glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a),
                             (r, g, b, a))
REAL_DECL_VOID(glBegin, (GLenum mode), (mode))
REAL_DECL_VOID(glPushMatrix, (void), ())
#endif

/* ------------------------------------------------------------------- state */

#define GLPROBE_MAX_BATCHES 48

static void (*g_app_display)(void);   /* the app's real display callback */

static int   g_enabled;               /* GLPROBE set in the environment      */
static int   g_target_frame = 1;      /* GLPROBE_FRAME                       */
static int   g_frame;                 /* frames seen so far                  */
static char  g_ply_path[512];         /* GLPROBE_PLY                         */
static char  g_extract_path[512];     /* GLPROBE_EXTRACT                     */
static int   g_extract_batch = -1;    /* GLPROBE_BATCH                       */
static int   g_extract_max_tris;      /* GLPROBE_MAX_TRIS                    */
static const char *g_argv0 = "the probed binary";

static int   g_in_pass;               /* inside a feedback pass              */
static int   g_force_neutral;         /* the NEUTRAL pass is running         */

/* The app's VIEW matrix, snapshotted at the first geometry of a probe pass.
 *
 * Deliberately not taken from gluLookAt: plenty of programs build their view
 * out of raw glTranslatef/glRotatef and never call it, and the modelview at
 * the moment the frame's first primitive is issued is the view under either
 * style. Snapshotting also fires on the first glPushMatrix, since an app whose
 * very first object is drawn inside a push would otherwise hand back a matrix
 * with that object's model transform folded in. */
static double g_view_mat[16];
static int    g_have_view_mat;
static int    g_view_snapshot_pending;

/* Snooped so an extracted scene keeps the app's background. */
static float  g_clear_color[4];
static int    g_have_clear_color;

/* Batch markers. Each state change the app makes during a pass emits a
 * glPassThrough with the next id, and the label table records what the change
 * was, so the report can say "the batch after diffuse (0.40,0.22,0.08)"
 * without ever knowing that function was called drawTorch. */
static char g_batch_label[GLPROBE_MAX_BATCHES][72];
static int  g_batch_next;

/* Shadowed lighting/material state. The extract pass turns lighting off so it
 * does not distort the capture, which means a lit surface's color never
 * reaches the feedback stream -- it has to be reconstructed from the material
 * the app installed. These track the app's INTENT, so they are updated even
 * when the pass suppresses the call. */
static int   g_app_lighting;
static int   g_app_color_material;
static int   g_app_has_material;
static float g_app_diffuse[4]  = { 0.8f, 0.8f, 0.8f, 1.0f };
static float g_app_ambient[4]  = { 0.2f, 0.2f, 0.2f, 1.0f };
static float g_app_specular[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static float g_app_shininess;

/* Snapshot of the above at each batch boundary, handed to the emitter. */
static GlProbeExtractBatch g_batch_state[GLPROBE_MAX_BATCHES];

static void batch_capture_state(int idx) {
    if (idx < 0 || idx >= GLPROBE_MAX_BATCHES)
        return;
    GlProbeExtractBatch *b = &g_batch_state[idx];
    b->lit = g_app_lighting;
    b->color_material = g_app_color_material;
    b->has_material = g_app_has_material;
    memcpy(b->diffuse, g_app_diffuse, sizeof b->diffuse);
    memcpy(b->ambient, g_app_ambient, sizeof b->ambient);
    memcpy(b->specular, g_app_specular, sizeof b->specular);
    b->shininess = g_app_shininess;
}

static void batch_mark(const char *fmt, ...) {
    if (!g_in_pass || g_batch_next >= GLPROBE_MAX_BATCHES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_batch_label[g_batch_next], sizeof g_batch_label[0], fmt, ap);
    va_end(ap);
    batch_capture_state(g_batch_next);
    glPassThrough((GLfloat)g_batch_next);
    g_batch_next++;
}

/* ------------------------------------------------- interposed GL/GLU/GLUT */

/* gluLookAt: let it run, then keep the modelview it produced. That matrix is
 * the app's view transform, and unprojecting through it turns the whole-frame
 * window coordinates into world coordinates -- per-object model transforms
 * stay folded in, which is exactly what a world-space report wants. */
void GLPROBE_REPLACEMENT(gluLookAt)(GLdouble ex, GLdouble ey, GLdouble ez,
                                    GLdouble cx, GLdouble cy, GLdouble cz,
                                    GLdouble ux, GLdouble uy, GLdouble uz) {
    REAL(gluLookAt)(ex, ey, ez, cx, cy, cz, ux, uy, uz);
}

static void view_snapshot_maybe(void) {
    if (!g_view_snapshot_pending)
        return;
    g_view_snapshot_pending = 0;
    glGetDoublev(GL_MODELVIEW_MATRIX, g_view_mat);
    g_have_view_mat = 1;
}

void GLPROBE_REPLACEMENT(glBegin)(GLenum mode) {
    view_snapshot_maybe();
    REAL(glBegin)(mode);
}

void GLPROBE_REPLACEMENT(glPushMatrix)(void) {
    view_snapshot_maybe();
    REAL(glPushMatrix)();
}

void GLPROBE_REPLACEMENT(glClearColor)(GLclampf r, GLclampf g, GLclampf b,
                                       GLclampf a) {
    g_clear_color[0] = (float)r; g_clear_color[1] = (float)g;
    g_clear_color[2] = (float)b; g_clear_color[3] = (float)a;
    g_have_clear_color = 1;
    REAL(glClearColor)(r, g, b, a);
}

/* Swallowed during a pass so the extra runs of the app's display callback
 * never reach the screen. */
void GLPROBE_REPLACEMENT(glutSwapBuffers)(void) {
    if (g_in_pass)
        return;
    REAL(glutSwapBuffers)();
}

/* The NEUTRAL pass has to win against the app re-enabling the very state the
 * pass exists to remove, so those caps are swallowed while it runs. */
static int neutral_suppressed(GLenum cap) {
    if (!g_force_neutral)
        return 0;
    switch (cap) {
    case GL_LIGHTING:
    case GL_CULL_FACE:
    case GL_SCISSOR_TEST:
    case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
    case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
        return 1;
    default:
        return 0;
    }
}

/* Marking happens BEFORE the suppression check, and unconditionally, so both
 * passes emit the identical marker sequence. Otherwise the neutral pass drops
 * a marker exactly where it swallowed a glEnable(GL_LIGHTING), the two batch
 * lists shift out of step, and the per-batch comparison that decides the
 * lighting verdict silently gives up. */
void GLPROBE_REPLACEMENT(glEnable)(GLenum cap) {
    if (cap == GL_LIGHTING) g_app_lighting = 1;
    if (cap == GL_COLOR_MATERIAL) g_app_color_material = 1;
    /* A lighting or blending switch mid-frame is a pass boundary in every
     * fixed-function renderer worth probing: opaque geometry, then the
     * blended overlay. Worth a batch split even though it is not a material. */
    if (cap == GL_BLEND)
        batch_mark("after glEnable(GL_BLEND)");
    else if (cap == GL_LIGHTING)
        batch_mark("after glEnable(GL_LIGHTING)");
    if (neutral_suppressed(cap))
        return;
    REAL(glEnable)(cap);
}

void GLPROBE_REPLACEMENT(glDisable)(GLenum cap) {
    if (cap == GL_LIGHTING) g_app_lighting = 0;
    if (cap == GL_COLOR_MATERIAL) g_app_color_material = 0;
    if (cap == GL_LIGHTING)
        batch_mark("after glDisable(GL_LIGHTING)");
    else if (cap == GL_BLEND)
        batch_mark("after glDisable(GL_BLEND)");
    if (neutral_suppressed(cap))
        return;
    REAL(glDisable)(cap);
}

/* Deliberately NOT hooked as a batch boundary: glColor. It changes per
 * particle in any particle system, which would shred the report into hundreds
 * of one-quad batches. Material and texture changes are the coarse-grained
 * signals that track objects. */
void GLPROBE_REPLACEMENT(glMaterialfv)(GLenum face, GLenum pname,
                                       const GLfloat *params) {
    if (params) {
        /* GL_AMBIENT_AND_DIFFUSE writes both, so it is expanded rather than
         * stored as a third case the emitter would have to understand. */
        if (pname == GL_DIFFUSE || pname == GL_AMBIENT_AND_DIFFUSE) {
            memcpy(g_app_diffuse, params, sizeof g_app_diffuse);
            g_app_has_material = 1;
        }
        if (pname == GL_AMBIENT || pname == GL_AMBIENT_AND_DIFFUSE) {
            memcpy(g_app_ambient, params, sizeof g_app_ambient);
            g_app_has_material = 1;
        }
        if (pname == GL_SPECULAR) {
            memcpy(g_app_specular, params, sizeof g_app_specular);
            g_app_has_material = 1;
        }
        if (pname == GL_SHININESS) {
            g_app_shininess = params[0];
            g_app_has_material = 1;
        }
        if (pname == GL_DIFFUSE || pname == GL_AMBIENT_AND_DIFFUSE)
            batch_mark("after diffuse (%.2f, %.2f, %.2f)", params[0], params[1],
                       params[2]);
    }
    REAL(glMaterialfv)(face, pname, params);
}

void GLPROBE_REPLACEMENT(glMaterialf)(GLenum face, GLenum pname, GLfloat param) {
    if (pname == GL_SHININESS) {
        g_app_shininess = param;
        g_app_has_material = 1;
    }
    REAL(glMaterialf)(face, pname, param);
}

void GLPROBE_REPLACEMENT(glBindTexture)(GLenum target, GLuint texture) {
    batch_mark("after glBindTexture(%u)", (unsigned)texture);
    REAL(glBindTexture)(target, texture);
}

/* ------------------------------------------------------------- the passes */

#define FB_INITIAL (1 << 20)
#define FB_MAX     (64 << 20)

/* Run the app's display callback once under feedback. Returns the float count,
 * or < 0 on overflow. */
static int run_pass(int neutral, float *buf, int cap_floats, GlProbeXform *xf) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    g_batch_next = 0;
    g_force_neutral = neutral;
    g_in_pass = 1;

    if (neutral) {
        REAL(glDisable)(GL_LIGHTING);
        REAL(glDisable)(GL_CULL_FACE);
        REAL(glDisable)(GL_SCISSOR_TEST);
        for (int i = 0; i < 6; i++)
            REAL(glDisable)((GLenum)(GL_CLIP_PLANE0 + i));
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    glFeedbackBuffer((GLsizei)cap_floats, GL_3D_COLOR, buf);
    glRenderMode(GL_FEEDBACK);
    if (g_app_display)
        g_app_display();
    int written = glRenderMode(GL_RENDER);

    /* The projection is whatever reshape installed; the modelview at this
     * point is whatever the callback left behind, so use the snooped view
     * matrix when there is one. */
    glGetDoublev(GL_PROJECTION_MATRIX, xf->proj);
    glGetIntegerv(GL_VIEWPORT, xf->vp);
    if (g_have_view_mat)
        memcpy(xf->mv, g_view_mat, sizeof xf->mv);
    else
        glGetDoublev(GL_MODELVIEW_MATRIX, xf->mv);

    g_in_pass = 0;
    g_force_neutral = 0;
    glPopAttrib();
    return written;
}

static int run_pass_grow(int neutral, float **out_buf, GlProbeXform *xf,
                         int *out_overflow) {
    int cap_floats = FB_INITIAL;
    float *buf = NULL;
    *out_overflow = 0;
    for (;;) {
        float *nb = realloc(buf, (size_t)cap_floats * sizeof *nb);
        if (!nb) {
            free(buf);
            return -1;
        }
        buf = nb;
        int written = run_pass(neutral, buf, cap_floats, xf);
        if (written >= 0) {
            *out_buf = buf;
            return written;
        }
        if (cap_floats >= FB_MAX) {
            *out_buf = buf;
            *out_overflow = 1;
            return cap_floats;
        }
        cap_floats <<= 1;
        if (cap_floats > FB_MAX)
            cap_floats = FB_MAX;
    }
}

static void label_batches(GlProbeReport *r) {
    for (int i = 0; i < r->batch_count; i++) {
        int id = (int)r->batches[i].batch_id;
        if (id >= 0 && id < GLPROBE_MAX_BATCHES)
            snprintf(r->batches[i].batch_label, sizeof r->batches[i].batch_label,
                     "%s", g_batch_label[id]);
    }
}

static void probe_frame(void) {
    FILE *f = stderr;
    GlProbeReport as_drawn, neutral;
    static GlProbeReport batches_a[GLPROBE_MAX_BATCHES];
    static GlProbeReport batches_n[GLPROBE_MAX_BATCHES];
    float *buf = NULL;
    GlProbeXform xf;
    int overflow = 0, n;

    fprintf(f, "\n=== glprobe (preload) frame %d ===\n", g_frame);

    n = run_pass_grow(0, &buf, &xf, &overflow);
    if (n < 0) {
        fprintf(f, "!! glprobe: as-drawn pass failed\n");
        return;
    }
    /* Only meaningful after a pass has run: the app calls gluLookAt from
     * inside its display callback, so before the first pass there is nothing
     * to have snooped yet. */
    fprintf(f, "    coordinates: %s\n", g_have_view_mat
            ? "world space (view matrix taken at the frame's first primitive)"
            : "unprojected through the modelview the frame left behind "
              "(no view matrix captured -- treat positions as approximate)");
    if (glprobe_analyze(buf, n, &xf, GLPROBE_MODE_AS_DRAWN, &as_drawn,
                        batches_a, GLPROBE_MAX_BATCHES) == 0) {
        as_drawn.overflow = overflow;
        label_batches(&as_drawn);
        glprobe_report_print(&as_drawn, "whole frame", NULL, f);
    } else {
        fprintf(f, "!! glprobe: as-drawn stream did not parse\n");
    }
    free(buf);
    buf = NULL;

    n = run_pass_grow(1, &buf, &xf, &overflow);
    if (n < 0) {
        fprintf(f, "!! glprobe: neutral pass failed\n");
        return;
    }
    if (glprobe_analyze(buf, n, &xf, GLPROBE_MODE_NEUTRAL, &neutral,
                        batches_n, GLPROBE_MAX_BATCHES) == 0) {
        neutral.overflow = overflow;
        label_batches(&neutral);
        glprobe_report_print(&neutral, "whole frame", NULL, f);
    } else {
        fprintf(f, "!! glprobe: neutral stream did not parse\n");
        free(buf);
        return;
    }

    /* The comparison, same as the in-process front-end: two lenses on one
     * frame, and the difference between them names the culprit.
     *
     * Whole-frame totals are the wrong altitude for the lighting question --
     * one bright additive particle system drags the frame's dark-vertex count
     * below 100% and hides a completely black object. So the lighting verdict
     * is decided per batch: a batch that is entirely black as drawn but has
     * real color with lighting off is geometry the lighting is losing. */
    int unlit_batches = 0, unlit_tris = 0, drawn_batches = 0;
    int aligned = (as_drawn.batch_count == neutral.batch_count);
    for (int b = 0; aligned && b < as_drawn.batch_count; b++) {
        const GlProbeReport *a = &as_drawn.batches[b];
        const GlProbeReport *nb = &neutral.batches[b];
        if (a->verts == 0 || a->verts != nb->verts)
            continue;                       /* not the same geometry twice */
        /* Only batches that drew something are worth counting: a state change
         * followed by no geometry is a denominator nobody can interpret. */
        drawn_batches++;
        if (a->dark_verts == a->verts && nb->lum_max > GLPROBE_DARK_LUM) {
            unlit_batches++;
            unlit_tris += a->tris;
        }
    }

    fprintf(f, "   verdict: ");
    if (neutral.verts == 0)
        fprintf(f, "the frame emits no geometry at all -- the bug is upstream "
                   "of GL.\n");
    else if (as_drawn.tris < neutral.tris)
        fprintf(f, "%d of %d triangles vanish with culling/clipping off -- "
                   "winding, GL_CULL_FACE or a clip plane is eating them.\n",
                neutral.tris - as_drawn.tris, neutral.tris);
    else if (as_drawn.verts && as_drawn.offscreen == as_drawn.verts)
        fprintf(f, "the frame is entirely off-screen -- a camera problem, not "
                   "a mesh problem.\n");
    else if (unlit_batches > 0) {
        fprintf(f, "%d of %d drawing batches (%d tris) are DRAWN BUT UNLIT "
                   "-- the geometry is fine and the lighting is the bug:\n",
                unlit_batches, drawn_batches, unlit_tris);
        for (int b = 0; b < as_drawn.batch_count; b++) {
            const GlProbeReport *a = &as_drawn.batches[b];
            const GlProbeReport *nb = &neutral.batches[b];
            if (a->verts && a->verts == nb->verts &&
                a->dark_verts == a->verts && nb->lum_max > GLPROBE_DARK_LUM)
                fprintf(f, "            %s (%d tris, lum max %.4f)\n",
                        a->batch_label[0] ? a->batch_label : "(unlabeled)",
                        a->tris, a->lum_max);
        }
        fprintf(f, "!!          Check the N.L sign (is the light inside or "
                   "behind these surfaces?),\n"
                   "!!          GL_LIGHTING/GL_LIGHTn, a non-zero material "
                   "diffuse, and that the\n"
                   "!!          light position is set AFTER the camera "
                   "matrix.\n");
    } else if (as_drawn.verts && as_drawn.dark_verts == as_drawn.verts)
        fprintf(f, "geometry is FINE (%d tris) and the lighting is the bug -- "
                   "every vertex came back black.\n", as_drawn.tris);
    else
        fprintf(f, "geometry present and lit.\n");
    fprintf(f, "\n");

    if (g_ply_path[0]) {
        /* The diagnostic passes run under the app's own perspective, whose
         * inverse the PLY writer cannot apply. Extraction installs the ortho
         * the writer needs, so point there rather than writing a file whose
         * vertex positions would be silently wrong. */
        fprintf(f, "   note: GLPROBE_PLY is not a diagnostic output -- use "
                   "GLPROBE_EXTRACT=<file>.ply instead.\n\n");
    }
    free(buf);
}

/* ---------------------------------------------------------- extraction */

/* Capture cube half-extent for the first (sizing) extract pass, and the
 * viewport the inverse is computed against. */
#define EXTRACT_ORTHO_R0 1000.0f
#define EXTRACT_VP       4096

/* One extraction pass. Unlike the diagnostic passes this REPLACES the
 * projection -- feedback clips to the view volume, so a capture taken under
 * the app's own perspective loses everything off-screen, which is fine for a
 * report about the visible frame and fatal for an extractor. The app sets its
 * projection in reshape(), not per frame, so overriding it here sticks for the
 * duration of the callback. */
static int extract_pass(float *buf, int cap_floats, float ortho_r) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-ortho_r, ortho_r, -ortho_r, ortho_r, -ortho_r, ortho_r);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glViewport(0, 0, EXTRACT_VP, EXTRACT_VP);
    glDepthRange(0.0, 1.0);

    g_batch_next = 0;
    g_in_pass = 1;
    g_force_neutral = 1;      /* culling/clipping must not eat the mesh */
    REAL(glDisable)(GL_LIGHTING);
    REAL(glDisable)(GL_CULL_FACE);
    REAL(glDisable)(GL_SCISSOR_TEST);
    for (int i = 0; i < 6; i++)
        REAL(glDisable)((GLenum)(GL_CLIP_PLANE0 + i));
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glFeedbackBuffer((GLsizei)cap_floats, GL_3D_COLOR, buf);
    glRenderMode(GL_FEEDBACK);
    if (g_app_display)
        g_app_display();
    int written = glRenderMode(GL_RENDER);

    g_force_neutral = 0;
    g_in_pass = 0;
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
    return written;
}

static int extract_pass_grow(float ortho_r, float **out_buf) {
    int cap_floats = FB_INITIAL;
    float *buf = NULL;
    for (;;) {
        float *nb = realloc(buf, (size_t)cap_floats * sizeof *nb);
        if (!nb) {
            free(buf);
            return -1;
        }
        buf = nb;
        int written = extract_pass(buf, cap_floats, ortho_r);
        if (written >= 0) {
            *out_buf = buf;
            return written;
        }
        if (cap_floats >= FB_MAX) {
            free(buf);
            return -1;
        }
        cap_floats <<= 1;
        if (cap_floats > FB_MAX)
            cap_floats = FB_MAX;
    }
}

/* Invert a 4x4 affine modelview (column-major, m[col*4 + row]).
 *
 * General 3x3 inverse rather than a transpose: a view matrix is usually rigid,
 * where R^-1 == R^T, but an app is free to have scaled or mirrored on its way
 * to the pose and a transpose would silently corrupt those. Returns 0 when the
 * upper 3x3 is singular, in which case the caller leaves coordinates in eye
 * space rather than producing garbage. */
static int mat4_affine_inverse(const double *m, double *out) {
    double a = m[0], b = m[4], c = m[8];
    double d = m[1], e = m[5], f = m[9];
    double g = m[2], h = m[6], i = m[10];

    double A =  (e * i - f * h);
    double B = -(d * i - f * g);
    double C =  (d * h - e * g);
    double det = a * A + b * B + c * C;
    if (det > -1e-12 && det < 1e-12)
        return 0;
    double id = 1.0 / det;

    double r[9];
    r[0] = A * id;                    r[3] = -(b * i - c * h) * id;  r[6] =  (b * f - c * e) * id;
    r[1] = B * id;                    r[4] =  (a * i - c * g) * id;  r[7] = -(a * f - c * d) * id;
    r[2] = C * id;                    r[5] = -(a * h - b * g) * id;  r[8] =  (a * e - b * d) * id;

    double tx = m[12], ty = m[13], tz = m[14];
    for (int k = 0; k < 3; k++) {
        out[k * 4 + 0] = r[k * 3 + 0];
        out[k * 4 + 1] = r[k * 3 + 1];
        out[k * 4 + 2] = r[k * 3 + 2];
        out[k * 4 + 3] = 0.0;
    }
    out[12] = -(r[0] * tx + r[3] * ty + r[6] * tz);
    out[13] = -(r[1] * tx + r[4] * ty + r[7] * tz);
    out[14] = -(r[2] * tx + r[5] * ty + r[8] * tz);
    out[15] = 1.0;
    return 1;
}

/* Rewrite a capture from EYE space into WORLD space, in place.
 *
 * The extract pass overrides the projection but deliberately leaves the
 * modelview alone, so the app applies its own camera however it likes --
 * gluLookAt, raw glTranslatef/glRotatef, a hand-built matrix. What comes back
 * is therefore eye space, and multiplying through by the inverse of the view
 * matrix captured at the frame's first primitive is what makes the result
 * general instead of working only for the gluLookAt case.
 *
 * Rewriting the buffer rather than the emitted vertices keeps mesh_ply.c
 * (which has no idea a camera exists) working unchanged, and keeps the two
 * output formats from disagreeing about where the geometry is. */
static void feedback_to_world(float *fb, int n, float ortho_r,
                              const double *view) {
    double inv[16];
    if (!mat4_affine_inverse(view, inv))
        return;

    int i = 0;
    while (i < n) {
        int tok = (int)fb[i++];
        int nverts;
        switch (tok) {
        case MESH_PLY_TOK_POINT: nverts = 1; break;
        case MESH_PLY_TOK_LINE:
        case MESH_PLY_TOK_LINE_RESET: nverts = 2; break;
        case MESH_PLY_TOK_POLYGON:
            if (i >= n) return;
            nverts = (int)fb[i++];
            if (nverts < 3) return;
            break;
        case MESH_PLY_TOK_PASS_THROUGH:
            if (i >= n) return;
            i++;
            continue;
        case MESH_PLY_TOK_BITMAP:
        case MESH_PLY_TOK_DRAW_PIXEL:
        case MESH_PLY_TOK_COPY_PIXEL: nverts = 1; break;
        default: return;
        }
        if (i + nverts * 7 > n)
            return;
        for (int k = 0; k < nverts; k++) {
            float *f = fb + i + k * 7;
            /* window -> ndc -> eye */
            double ex = (2.0 * f[0] / (double)EXTRACT_VP - 1.0) * ortho_r;
            double ey = (2.0 * f[1] / (double)EXTRACT_VP - 1.0) * ortho_r;
            double ez = -(2.0 * f[2] - 1.0) * ortho_r;
            /* eye -> world */
            double wx = inv[0] * ex + inv[4] * ey + inv[8]  * ez + inv[12];
            double wy = inv[1] * ex + inv[5] * ey + inv[9]  * ez + inv[13];
            double wz = inv[2] * ex + inv[6] * ey + inv[10] * ez + inv[14];
            /* world -> ndc -> window, same cube, so every downstream reader
             * applies the one inverse it already knows about. */
            f[0] = (float)((wx / ortho_r + 1.0) * 0.5 * EXTRACT_VP);
            f[1] = (float)((wy / ortho_r + 1.0) * 0.5 * EXTRACT_VP);
            f[2] = (float)((-wz / ortho_r + 1.0) * 0.5);
        }
        i += nverts * 7;
    }
}

/* Widest |coordinate| in a capture, used to size the second pass's cube.
 * Window coordinates are floats, so a 2-unit scene inside a 1000-unit cube
 * throws away most of the mantissa; refitting is what makes an extracted mesh
 * accurate rather than merely plausible. */
static float capture_extent(const float *fb, int n, float ortho_r) {
    GlProbeExtractCapture cap;
    cap.ortho_r = ortho_r;
    cap.vp_x = 0; cap.vp_y = 0; cap.vp_w = EXTRACT_VP; cap.vp_h = EXTRACT_VP;
    cap.depth_near = 0.0f; cap.depth_far = 1.0f;

    float ext = 0.0f;
    int i = 0;
    while (i < n) {
        int tok = (int)fb[i++];
        int nverts;
        switch (tok) {
        case MESH_PLY_TOK_POINT: nverts = 1; break;
        case MESH_PLY_TOK_LINE:
        case MESH_PLY_TOK_LINE_RESET: nverts = 2; break;
        case MESH_PLY_TOK_POLYGON:
            if (i >= n) return ext;
            nverts = (int)fb[i++];
            if (nverts < 3) return ext;
            break;
        case MESH_PLY_TOK_PASS_THROUGH:
            if (i >= n) return ext;
            i++;
            continue;
        case MESH_PLY_TOK_BITMAP:
        case MESH_PLY_TOK_DRAW_PIXEL:
        case MESH_PLY_TOK_COPY_PIXEL: nverts = 1; break;
        default: return ext;
        }
        if (i + nverts * 7 > n)
            return ext;
        for (int k = 0; k < nverts; k++) {
            const float *f = fb + i + k * 7;
            float c[3];
            c[0] = (2.0f * f[0] / (float)EXTRACT_VP - 1.0f) * cap.ortho_r;
            c[1] = (2.0f * f[1] / (float)EXTRACT_VP - 1.0f) * cap.ortho_r;
            c[2] = -(2.0f * f[2] - 1.0f) * cap.ortho_r;
            for (int a = 0; a < 3; a++) {
                float m = c[a] < 0.0f ? -c[a] : c[a];
                if (m > ext && m < 1e30f)
                    ext = m;
            }
        }
        i += nverts * 7;
    }
    return ext;
}

/* Run the app's display callback with nothing overridden, purely so the
 * interposed glBegin/glPushMatrix can snapshot the view matrix. Feedback is on
 * with a tiny buffer: the geometry is thrown away, the point is that nothing
 * rasterizes and the screen is untouched. */
static void view_probe_pass(void) {
    static float scratch[64];
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    g_in_pass = 1;
    g_view_snapshot_pending = 1;
    glFeedbackBuffer((GLsizei)(sizeof scratch / sizeof scratch[0]), GL_3D,
                     scratch);
    glRenderMode(GL_FEEDBACK);
    if (g_app_display)
        g_app_display();
    glRenderMode(GL_RENDER);
    g_view_snapshot_pending = 0;
    g_in_pass = 0;
    glPopAttrib();
}

/* Decompose a view matrix into gl-repl's orbit camera block,
 *
 *     M = T(0,0,-dist) . Rx(rx) . Ry(ry) . T(-target)
 *
 * which is general enough to cover both a gluLookAt view and a hand-rolled
 * glTranslatef/glRotatef one, because it only reads the resulting matrix.
 *
 * Rx(a).Ry(b) expands to
 *     [  cos b        0       sin b  ]
 *     [  sin a sin b  cos a  -sin a cos b ]
 *     [ -cos a sin b  sin a   cos a cos b ]
 * so the pitch falls out of one element and the yaw out of the top row.
 *
 * The translation is underdetermined -- three equations for dist plus a
 * three-vector target -- so the split is fixed by taking dist from the view-z
 * offset and letting the target absorb whatever is left. That is the choice
 * that keeps an app's off-center framing (a camera raised to chest height, say)
 * instead of silently recentering the scene.
 */
static GlProbeExtractCamera derive_camera(void) {
    GlProbeExtractCamera c;
    memset(&c, 0, sizeof c);
    if (!g_have_view_mat)
        return c;

    /* Column-major: m[col*4 + row]. */
    const double *m = g_view_mat;
    double r00 = m[0], r02 = m[8];
    double r21 = m[6];            /* col 1, row 2 */
    double tx = m[12], ty = m[13], tz = m[14];

    double sa = r21;
    if (sa > 1.0) sa = 1.0;
    if (sa < -1.0) sa = -1.0;

    const double kRad2Deg = 57.29577951308232;
    double rx = asin(sa);
    double ry = atan2(r02, r00);

    double dist = -tz;
    if (dist < 0.0) {
        /* A view that puts the scene behind the eye is not an orbit pose; take
         * the magnitude so the block is at least loadable. */
        dist = -dist;
    }

    /* residual = M_translation - (0, 0, -dist), then target = -R^T . residual. */
    double res[3];
    res[0] = tx;
    res[1] = ty;
    res[2] = tz + dist;

    double ca = cos(rx), sa2 = sin(rx);
    double cb = cos(ry), sb = sin(ry);
    /* R = Rx(rx).Ry(ry); R^T rows are R's columns. */
    double rT[3][3];
    rT[0][0] = cb;        rT[0][1] = sa2 * sb;  rT[0][2] = -ca * sb;
    rT[1][0] = 0.0;       rT[1][1] = ca;        rT[1][2] = sa2;
    rT[2][0] = sb;        rT[2][1] = -sa2 * cb; rT[2][2] = ca * cb;

    c.present = 1;
    c.dist = (float)dist;
    c.rx = (float)(rx * kRad2Deg);
    c.ry = (float)(ry * kRad2Deg);
    c.tx = (float)-(rT[0][0] * res[0] + rT[0][1] * res[1] + rT[0][2] * res[2]);
    c.ty = (float)-(rT[1][0] * res[0] + rT[1][1] * res[1] + rT[1][2] * res[2]);
    c.tz = (float)-(rT[2][0] * res[0] + rT[2][1] * res[1] + rT[2][2] * res[2]);
    return c;
}

static void extract_write(FILE *log, const char *path, const float *buf, int n,
                          float fit, int as_ply);

static int path_ends_with(const char *p, const char *suffix) {
    size_t lp = strlen(p), ls = strlen(suffix);
    return lp >= ls && strcmp(p + lp - ls, suffix) == 0;
}

static void extract_frame(void) {
    FILE *log = stderr;
    float *buf = NULL;

    view_probe_pass();

    int n = extract_pass_grow(EXTRACT_ORTHO_R0, &buf);
    if (n < 0) {
        fprintf(log, "!! glprobe: extraction capture failed\n");
        return;
    }

    /* Refit and re-capture at the scene's own scale.
     *
     * The extent is measured in EYE space -- before the world conversion
     * below -- because that is the space the capture happens in: the cube has
     * to contain the geometry as the app submits it, camera offset and all.
     * Size it to the world extent instead and a scene the camera has pushed
     * out to z = -14 falls straight out of a cube fitted to its 6-unit world
     * bounds, which silently drops most of the mesh. */
    float ext = capture_extent(buf, n, EXTRACT_ORTHO_R0);
    float fit = ext * 1.25f;
    if (fit < 1e-4f)
        fit = 1e-4f;
    if (ext > 0.0f && fit < EXTRACT_ORTHO_R0) {
        free(buf);
        buf = NULL;
        n = extract_pass_grow(fit, &buf);
        if (n < 0) {
            fprintf(log, "!! glprobe: extraction re-capture failed\n");
            return;
        }
    } else {
        fit = EXTRACT_ORTHO_R0;
    }

    /* Eye -> world, now that the capture is final. Re-encoding into the same
     * cube is safe even when a world coordinate lands outside it: nothing
     * clips after the capture, so the round trip is pure arithmetic. */
    if (g_have_view_mat)
        feedback_to_world(buf, n, fit, g_view_mat);

    /* A path with no recognized extension means "both", which is what the
     * `make extract` target uses: one capture, two files, guaranteed to
     * describe the same frame. Running the app twice would not be -- anything
     * animated moves between them. */
    int want_ply = path_ends_with(g_extract_path, ".ply");
    int want_glr = path_ends_with(g_extract_path, ".glr");
    if (!want_ply && !want_glr) {
        char p[sizeof g_extract_path];
        snprintf(p, sizeof p, "%s.ply", g_extract_path);
        extract_write(log, p, buf, n, fit, 1);
        snprintf(p, sizeof p, "%s.glr", g_extract_path);
        extract_write(log, p, buf, n, fit, 0);
    } else {
        extract_write(log, g_extract_path, buf, n, fit, want_ply);
    }
    free(buf);
}

static void extract_write(FILE *log, const char *path, const float *buf, int n,
                          float fit, int as_ply) {
    GlProbeExtractCapture cap;
    cap.ortho_r = fit;
    cap.vp_x = 0; cap.vp_y = 0; cap.vp_w = EXTRACT_VP; cap.vp_h = EXTRACT_VP;
    cap.depth_near = 0.0f; cap.depth_far = 1.0f;

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(log, "!! glprobe: cannot open %s for writing\n", path);
        return;
    }

    if (as_ply) {
        MeshPlyCapture mc;
        mc.ortho_r = fit;
        mc.vp_x = 0; mc.vp_y = 0; mc.vp_w = EXTRACT_VP; mc.vp_h = EXTRACT_VP;
        mc.depth_near = 0.0f; mc.depth_far = 1.0f;
        mc.floats_per_vertex = MESH_PLY_FLOATS_PER_VERTEX;

        MeshPlyOptions po;
        memset(&po, 0, sizeof po);
        po.weld = 1;
        po.weld_eps = 1e-4f;
        po.smooth_normals = 1;
        po.triangulate = 1;
        po.primitive_radius_scale = 0.0025f;

        MeshPlyStats ms;
        int ntris = mesh_ply_write(f, buf, n, &mc, &po, &ms);
        if (ntris < 0)
            fprintf(log, "!! glprobe: PLY write failed\n");
        else
            fprintf(log, "glprobe: extracted %d triangles (%d verts, %d edges) "
                         "to %s\n", ms.tris, ms.verts, ms.edges, path);
    } else {
        GlProbeExtractOptions eo;
        memset(&eo, 0, sizeof eo);
        eo.batch = g_extract_batch;
        eo.max_tris = g_extract_max_tris;
        eo.source_note = g_argv0;
        eo.camera = derive_camera();
        eo.has_clear_color = g_have_clear_color;
        memcpy(eo.clear_color, g_clear_color, sizeof eo.clear_color);
        eo.batches = g_batch_state;
        eo.batch_count = GLPROBE_MAX_BATCHES;

        GlProbeExtractStats es;
        if (glprobe_extract_write_repl_c(f, buf, n, &cap, &eo, &es) != 0) {
            fprintf(log, "!! glprobe: snippet write failed\n");
        } else {
            fprintf(log, "glprobe: extracted %d triangles to %s "
                         "(%d source commands)\n",
                    es.tris_written, path, es.lines_written);
            if (es.tris_skipped)
                fprintf(log, "glprobe: %d triangles dropped by "
                             "GLPROBE_MAX_TRIS\n", es.tris_skipped);
            /* gl-repl's editor buffer is a fixed array, so this is a hard
             * ceiling, not a soft one. Warn with headroom rather than at the
             * limit: `auto_normals` materializes derived normals as real
             * document rows once the scene runs, so the count written here is
             * a lower bound on what the editor will actually hold (312
             * triangles measured 941 written -> 1022 live). */
            if (es.lines_written > 850)
                fprintf(log,
                        "!! %d commands is over gl-repl's default "
                        "MAX_EDITOR_COMMANDS (1024), and auto_normals\n"
                        "!! expands it further at load (measured 1.1x-2.0x, "
                        "worse for independent quads).\n"
                        "!! Either build `make gl-repl-unchained` (32768), or "
                        "narrow the capture with\n"
                        "!! GLPROBE_BATCH=<n> (one object) / "
                        "GLPROBE_MAX_TRIS=<n>.\n", es.lines_written);
        }
    }

    fclose(f);
}

/* ------------------------------------------------------- the frame hook */

static void probe_display(void) {
    g_frame++;
    if (g_frame == g_target_frame) {
        if (g_extract_path[0])
            extract_frame();
        if (g_enabled)
            probe_frame();
    }
    if (g_app_display)
        g_app_display();
}

void GLPROBE_REPLACEMENT(glutDisplayFunc)(void (*func)(void)) {
    g_app_display = func;
    REAL(glutDisplayFunc)(probe_display);
}

/* ------------------------------------------------------------------ setup */

__attribute__((constructor)) static void glprobe_preload_init(void) {
    const char *e = getenv("GLPROBE");
    g_enabled = (e && *e && strcmp(e, "0") != 0);

    const char *frame = getenv("GLPROBE_FRAME");
    if (frame) {
        int v = atoi(frame);
        if (v > 0)
            g_target_frame = v;
    }
    const char *ply = getenv("GLPROBE_PLY");
    if (ply)
        snprintf(g_ply_path, sizeof g_ply_path, "%s", ply);

    const char *extract = getenv("GLPROBE_EXTRACT");
    if (extract)
        snprintf(g_extract_path, sizeof g_extract_path, "%s", extract);
    const char *batch = getenv("GLPROBE_BATCH");
    if (batch)
        g_extract_batch = atoi(batch);
    const char *maxt = getenv("GLPROBE_MAX_TRIS");
    if (maxt)
        g_extract_max_tris = atoi(maxt);

    if (g_enabled || g_extract_path[0])
        fprintf(stderr, "glprobe: armed for frame %d%s%s\n", g_target_frame,
                g_extract_path[0] ? ", extracting to " : "",
                g_extract_path[0] ? g_extract_path : "");
}

INTERPOSE(glutDisplayFunc)
INTERPOSE(glutSwapBuffers)
INTERPOSE(gluLookAt)
INTERPOSE(glEnable)
INTERPOSE(glDisable)
INTERPOSE(glMaterialfv)
INTERPOSE(glMaterialf)
INTERPOSE(glBindTexture)
INTERPOSE(glClearColor)
INTERPOSE(glBegin)
INTERPOSE(glPushMatrix)
