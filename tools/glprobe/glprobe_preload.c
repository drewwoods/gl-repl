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
#include "glprobe_glut.h"

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
REAL_DECL_VOID(glBindTexture, (GLenum target, GLuint texture),
                              (target, texture))
#endif

/* ------------------------------------------------------------------- state */

#define GLPROBE_MAX_BATCHES 48

static void (*g_app_display)(void);   /* the app's real display callback */

static int   g_enabled;               /* GLPROBE set in the environment      */
static int   g_target_frame = 1;      /* GLPROBE_FRAME                       */
static int   g_frame;                 /* frames seen so far                  */
static char  g_ply_path[512];         /* GLPROBE_PLY                         */

static int   g_in_pass;               /* inside a feedback pass              */
static int   g_force_neutral;         /* the NEUTRAL pass is running         */

/* View matrix snooped from gluLookAt, so window coords can be unprojected
 * back into world space. Valid only once the app has actually called it. */
static double g_view[16];
static int    g_have_view;

/* Batch markers. Each state change the app makes during a pass emits a
 * glPassThrough with the next id, and the label table records what the change
 * was, so the report can say "the batch after diffuse (0.40,0.22,0.08)"
 * without ever knowing that function was called drawTorch. */
static char g_batch_label[GLPROBE_MAX_BATCHES][72];
static int  g_batch_next;

static void batch_mark(const char *fmt, ...) {
    if (!g_in_pass || g_batch_next >= GLPROBE_MAX_BATCHES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_batch_label[g_batch_next], sizeof g_batch_label[0], fmt, ap);
    va_end(ap);
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
    glGetDoublev(GL_MODELVIEW_MATRIX, g_view);
    g_have_view = 1;
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
    if (pname == GL_DIFFUSE && params)
        batch_mark("after diffuse (%.2f, %.2f, %.2f)", params[0], params[1],
                   params[2]);
    REAL(glMaterialfv)(face, pname, params);
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
    if (g_have_view)
        memcpy(xf->mv, g_view, sizeof xf->mv);
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
    fprintf(f, "    coordinates: %s\n", g_have_view
            ? "world space (view matrix snooped from gluLookAt)"
            : "unprojected through the modelview the frame left behind "
              "(no gluLookAt seen -- treat positions as approximate)");
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
        /* PLY needs the known ortho inverse that only the in-process geometry
         * lens installs, so say so rather than writing a file whose vertex
         * positions would be silently wrong. */
        fprintf(f, "   note: GLPROBE_PLY is ignored in preload mode -- a PLY "
                   "dump needs the geometry lens (see README).\n\n");
    }
    free(buf);
}

/* ------------------------------------------------------- the frame hook */

static void probe_display(void) {
    g_frame++;
    if (g_enabled && g_frame == g_target_frame)
        probe_frame();
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

    if (g_enabled)
        fprintf(stderr, "glprobe: armed for frame %d\n", g_target_frame);
}

INTERPOSE(glutDisplayFunc)
INTERPOSE(glutSwapBuffers)
INTERPOSE(gluLookAt)
INTERPOSE(glEnable)
INTERPOSE(glDisable)
INTERPOSE(glMaterialfv)
INTERPOSE(glBindTexture)
