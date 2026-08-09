/*
 * Vertex-number overlay cost benchmark (Linux/macOS).
 *
 * WHY THIS EXISTS
 *
 * Turning vertex numbering on cost ~14 ms/frame on an NVIDIA RTX 5050
 * (driver 595.84): the profile panel's "vertex nums" row went from absent to
 * 14.3 ms and Frame Work went 2.2 -> 16.3 ms against a 16.67 ms budget, i.e.
 * all of the headroom. The pass does three things that could each plausibly
 * cost that much, and this benchmark prices all three:
 *
 *   1. one full-viewport glReadPixels(GL_DEPTH_COMPONENT, GL_FLOAT) per pass,
 *      feeding the label occlusion cull (edit_overlays.c,
 *      vertex_label_depth_snapshot),
 *   2. glRasterPos2f + glutBitmapCharacter per glyph for every placed label,
 *   3. glGetFloatv(GL_MODELVIEW_MATRIX) once per labelable vertex, in the walk
 *      callback (on_vertex_number_label).
 *
 * RESULT: it is (1), and almost none of it is the pixels.
 *
 *   case                                     kick  consume     work    frame
 *   no readback (baseline)                  0.000    0.000    0.150   16.665
 *   control: glFlush where read would go    0.002    0.000    0.152   16.666
 *   depth, mid-frame (pre-fix)              0.000   16.478   16.628   16.693
 *   depth, mid-frame after glFinish        13.732    2.736   16.618   16.668
 *   color GL_BACK, mid-frame                0.000   16.378   16.529   16.665
 *   color GL_FRONT, mid-frame               0.000   16.403   16.553   16.664
 *   depth, post-swap (last frame's)         0.000   16.060   16.209   16.664
 *   depth after frame's glFinish (implemented) 0.000  2.484    0.149   16.668
 *   control: 1x1 depth into PBO            16.334    0.000   16.484   16.666
 *   depth into PBO, map next frame         16.465    0.001   16.615   16.662
 *   depth into PBO + fence, polled         16.464    0.001   16.615   16.667
 *   control: full RGBA into PBO             0.008    0.001    0.159   16.665
 *
 * `work` is the app's Frame Work span - everything before the end-of-frame
 * glFinish that gl_repl.c:131 already performs. It is the headroom, and the
 * only column that decides anything.
 *
 * A synchronous read blocks until the driver's whole outstanding queue
 * retires. Controls that establish it, each killing a plausible alternative:
 *
 *   - Not the transfer. In the "mid-frame after glFinish" row the explicit
 *     drain takes 13.7 ms and the identical read then costs 2.7 ms - the wait
 *     simply moves into the glFinish, and Frame Work is unchanged at 16.6 ms.
 *     That row is the control, not a fix: it re-attributes, it does not save.
 *   - Not a dependency on the read target. GL_FRONT - already presented,
 *     nothing pending against it - blocks as long as GL_BACK. (glReadBuffer
 *     selects a *color* buffer, so it never applied to a depth read anyway.)
 *   - Not where in the frame the read sits, by itself. Moving it post-swap so
 *     it takes the previous frame's depth still costs 16.47 ms.
 *
 * IMPLEMENTED PATH: read after the frame's existing glFinish and consume it
 * next frame.
 * The implementation is documented in
 * docs/plans/done/vertex-label-depth-readback-stall.md.
 * gl_repl.c already drains at end of frame, outside glr_frame_work_end(), so
 * that wait is paid and attributed to Present regardless. A read issued there
 * costs only its transfer, in a span that has slack - and Frame Work returns
 * to baseline exactly (0.151 vs 0.148 ms). One frame of occlusion staleness,
 * invisible on labels.
 *
 * WHY NOT A PBO, WHICH IS THE TEXTBOOK ANSWER: glReadPixels of
 * GL_DEPTH_COMPONENT is synchronous on this driver no matter how it is
 * dressed. Into a PBO it still blocks (16.47 ms); at 1x1, with nothing to
 * transfer, it still blocks (16.37 ms); sourced from an FBO's
 * DEPTH_COMPONENT24 texture instead of the default framebuffer, it still
 * blocks (16.45 ms). A fence is architecturally right and cannot help: it
 * makes the *map* free, and the map was already free (0.001 ms) - the block
 * happens inside glReadPixels at issue time, before there is a fence to
 * insert. The PBO path itself is fine here, which is what the RGBA control
 * proves: the identical full-size read in color goes fully async at 0.015 ms.
 * So the constraint is specific to reading depth, not to PBOs, not to the
 * default framebuffer, and not to MSAA. Scope it to the driver, though: PBOs
 * permit pipelined/asynchronous readback, but OpenGL gives no wall-clock
 * guarantee that the issuing call is nonblocking. The RGBA control shows this
 * driver does enqueue colour asynchronously, so this is one vendor's behaviour
 * for depth, not an OpenGL guarantee. Reproduced on two NVIDIA drivers a major
 * version apart (595.84 and 610.43.02); re-run this before assuming it holds
 * anywhere else.
 *
 * Why NVIDIA in particular: the driver renders ahead by default, so there is a
 * deep queue for that synchronous read to drain. __GL_MaxFramesAllowed=1
 * removes the entire penalty (12.7 -> 1.08 ms measured in-app) without
 * touching the readback, and so does __GL_SYNC_TO_VBLANK=0 (12.8 -> 1.12 ms).
 *
 * READ THE FRAME COLUMN CAREFULLY. It is 16.66 ms in every row including the
 * baseline, because at 60 Hz the frame waits for the refresh regardless - the
 * readback does not add time here, it *relocates* the wait into the middle of
 * the frame. That is still the bug: a wait inside Frame Work is a wait nothing
 * can overlap, which is why the app's Frame Work went 2.2 -> 16.3 ms while
 * Present collapsed 10.2 -> 0.3 ms. This isolated harness has no other work to
 * lose, so only the app shows the headroom cost.
 *
 * HARNESS NOTE, learned the hard way: every case runs as real frames out of
 * glutMainLoop, one frame per display callback. Doing the timing loops inside
 * a single display callback - the obvious way to write this - produces a
 * window the compositor never throttles, which reports ~0.8 ms for the
 * mid-frame read and hides the entire effect. If the frame column is not
 * pinned to your refresh interval, the run is not throttled and the readback
 * rows mean nothing.
 *
 * IF IT HANGS WITH NO OUTPUT, CHECK THE MONITOR IS AWAKE: `xset -q | grep
 * Monitor`, and `xset dpms force on` if it says Off. A DPMS-blanked display
 * delivers no vblank, so glutSwapBuffers never returns and the first phase
 * never completes - the benchmark prints its header and then sits there
 * looking like a deadlock. This is the nastier sibling of the un-throttled
 * case above: that one prints wrong numbers, this one prints none.
 *
 * Usage:
 *   bench_vertex_label_readback [--msaa] [--size WxH] [--labels N]
 *                               [--vertices N] [--frames N]
 *
 * --msaa matches gl-repl, which requests GLUT_MULTISAMPLE; the MSAA depth
 * resolve roughly doubles the drained transfer cost. Compare a plain run with
 * one under __GL_SYNC_TO_VBLANK=0 to see the queue effect appear and vanish.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glut.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defaults match the observed case: the scene viewport of a default-size
 * gl-repl window, and the label count the atoll stress scene collects at
 * whole-scene scope. */
static int g_w        = 1200;
static int g_h        = 440;
static int g_labels   = 589;
static int g_vertices = 5000;
static int g_msaa     = 0;
static int g_frames   = 80;   /* counted frames per phase */
static int g_warm     = 20;   /* discarded frames before counting */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ scene */

/* Enough geometry to leave the GPU work queued. The scene's own cost is small
 * and is not the point; having something outstanding is. */
static void draw_scene(void) {
    int i;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)g_w / (double)g_h, 0.1, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -8.0f);
    for (i = 0; i < 24; i++) {
        glPushMatrix();
        glRotatef((float)i * 15.0f, 0.0f, 1.0f, 0.0f);
        glTranslatef(2.0f, 0.0f, 0.0f);
        glutSolidTorus(0.25, 0.6, 24, 32);
        glPopMatrix();
    }
}

/* ------------------------------------------------------------ (1) readback */

enum {
    PH_NONE,        /* baseline: what a frame costs with no read at all      */
    PH_FLUSH,       /* control: a no-op GL call where the read would go      */
    PH_MID,         /* historical pre-fix mid-frame read                    */
    PH_DRAINED,     /* same read, queue paid off first: the transfer cost    */
    PH_BACK,        /* color from the frame's live target                    */
    PH_FRONT,       /* color from an already-presented buffer                */
    PH_POSTSWAP,    /* last frame's depth, read at the top of this one       */
    PH_POSTFINISH,  /* implemented path: read after frame's own glFinish    */
    PH_PBO1,        /* control: 1x1 depth into a PBO - nothing to transfer   */
    PH_PBO,         /* depth into a PBO, mapped a frame later                */
    PH_FENCE,       /* depth into a PBO + fence, polled non-blocking         */
    PH_PBO_COLOR,   /* control: full RGBA into a PBO - the PBO path itself   */
    PH_COUNT
};

static const char *k_phase_name[PH_COUNT] = {
    "no readback (baseline)",
    "control: glFlush where read would go",
    "depth, mid-frame (pre-fix)",
    "depth, mid-frame after glFinish",
    "color GL_BACK, mid-frame",
    "color GL_FRONT, mid-frame",
    "depth, post-swap (last frame's)",
    "depth after frame's glFinish (implemented)",
    "control: 1x1 depth into PBO",
    "depth into PBO, map next frame",
    "depth into PBO + fence, polled",
    "control: full RGBA into PBO"
};

static int    g_phase = 0;
static int    g_frame = 0;
static double g_kick_ms[PH_COUNT];      /* time inside the issuing call     */
static double g_consume_ms[PH_COUNT];   /* time spent taking delivery       */
static double g_work_ms[PH_COUNT];      /* the app's Frame Work span        */
static double g_frame_ms[PH_COUNT];
static int    g_mapped[PH_COUNT];
static int    g_missed[PH_COUNT];
static void  *g_buf;
static GLuint g_pbo[3];
static GLsync g_fence[3];
static int    g_pbo_filled[3];
static int    g_phases_done = 0;

static double read_depth(void) {
    double t0 = now_ms();
    glReadPixels(0, 0, g_w, g_h, GL_DEPTH_COMPONENT, GL_FLOAT, g_buf);
    return now_ms() - t0;
}

static double read_color(GLenum which) {
    double t0;
    glReadBuffer(which);
    t0 = now_ms();
    glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, g_buf);
    return now_ms() - t0;
}

/* Issue a read into a PBO and take delivery of the one issued last frame.
 * `fenced` polls a fence non-blocking instead of mapping unconditionally, so a
 * buffer the GPU has not finished with is skipped rather than waited on.
 * `depth` picks the format - the color case is the control proving the PBO
 * path works here at all. The kick and the consume are timed separately
 * because that is the whole question: timing only the consume is what makes a
 * PBO look like it solved something. */
static void read_via_pbo(int depth, int one_pixel, int fenced,
                         double *kick_ms, double *consume_ms) {
    int cur = g_frame % 3;
    int prev = (g_frame + 2) % 3;
    int w = one_pixel ? 1 : g_w;
    int h = one_pixel ? 1 : g_h;
    double t0;

    t0 = now_ms();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[cur]);
    if (depth)
        glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    else
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (fenced) {
        if (g_fence[cur])
            glDeleteSync(g_fence[cur]);
        g_fence[cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    g_pbo_filled[cur] = 1;
    *kick_ms = now_ms() - t0;

    t0 = now_ms();
    if (g_pbo_filled[prev]) {
        int ready = 1;
        if (fenced && g_fence[prev]) {
            GLenum r = glClientWaitSync(g_fence[prev],
                                        GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            ready = (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
        }
        if (ready) {
            void *mapped;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[prev]);
            mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (mapped) {
                volatile unsigned char sink = ((unsigned char *)mapped)[0];
                (void)sink;
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            if (g_frame >= g_warm)
                g_mapped[g_phase]++;
        } else if (g_frame >= g_warm) {
            g_missed[g_phase]++;
        }
    }
    *consume_ms = now_ms() - t0;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void report_readback_phases(void) {
    int p;
    printf("\n-- depth readback, one case per phase, real frames --\n");
    printf("  %-40s %8s %8s %8s %8s  %s\n", "case", "kick", "consume",
           "work", "frame", "map/miss");
    for (p = 0; p < PH_COUNT; p++)
        printf("  %-40s %8.3f %8.3f %8.3f %8.3f  %d/%d\n", k_phase_name[p],
               g_kick_ms[p] / g_frames, g_consume_ms[p] / g_frames,
               g_work_ms[p] / g_frames, g_frame_ms[p] / g_frames,
               g_mapped[p], g_missed[p]);
    printf("\n  work = the app's Frame Work span (before the end-of-frame\n"
           "  glFinish gl_repl.c:131 already does). That is the headroom.\n"
           "  If the frame column is not pinned near your refresh interval,\n"
           "  the window is not vsync-throttled and these rows mean nothing.\n");
}
/* ------------------------------------------------ (2) the label glyph draw */

static void ortho_begin(void) {
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    gluOrtho2D(0.0, (double)g_w, 0.0, (double)g_h);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
}

static void ortho_end(void) {
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

/* Bracketed by glFinish on both sides, so this is submission+raster cost and
 * does not depend on the queue state the readback rows care about.
 * AT_VERTEX placement blends additively, DECLUTTERED does not; the blend is
 * the only state difference between them, so both are measured. */
static void bench_label_glyphs(int blend, int nlabels, const char *tag) {
    const int iters = 30;
    double t0, t1;
    int f, i;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
    glFinish();
    t0 = now_ms();
    for (f = 0; f < iters; f++) {
        ortho_begin();
        for (i = 0; i < nlabels; i++) {
            char text[16];
            const char *c;
            snprintf(text, sizeof text, "%d", i);
            glRasterPos2f((float)((i * 37) % (g_w - 40)),
                          (float)((i * 53) % (g_h - 20)));
            for (c = text; *c; c++)
                glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*c);
        }
        ortho_end();
    }
    glFinish();
    t1 = now_ms();
    printf("  %-38s %9.3f ms\n", tag, (t1 - t0) / iters);
}

/* ------------------------------------------- (3) the per-vertex glGetFloatv */

/* on_vertex_number_label reads GL_MODELVIEW_MATRIX for every labelable vertex,
 * interleaved with the walk's own push/transform/pop. Matrix readback in a hot
 * loop is a real cliff on some drivers, so it is measured rather than assumed.
 * On this one it is 0.017 us/vertex - not the problem. */
static void bench_get_modelview(int do_get, int do_xform, const char *tag) {
    const int iters = 20;
    double t0, t1;
    int f, i;
    float mv[16];
    volatile float sink = 0.0f;

    glMatrixMode(GL_MODELVIEW);
    glFinish();
    t0 = now_ms();
    for (f = 0; f < iters; f++) {
        glLoadIdentity();
        for (i = 0; i < g_vertices; i++) {
            if (do_xform) {
                glPushMatrix();
                glTranslatef(0.001f, 0.002f, 0.003f);
                glRotatef(0.5f, 0.0f, 1.0f, 0.0f);
            }
            if (do_get) {
                glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                sink += mv[0];
            }
            if (do_xform)
                glPopMatrix();
        }
    }
    glFinish();
    t1 = now_ms();
    (void)sink;
    printf("  %-38s %9.3f ms  (%.4f us/vertex)\n", tag, (t1 - t0) / iters,
           (t1 - t0) * 1000.0 / iters / g_vertices);
}

/* --------------------------------------------------- drained-transfer table */

typedef struct { const char *name; GLenum fmt, type; int bpp; } Fmt;

static const Fmt k_fmts[] = {
    { "DEPTH_COMPONENT / FLOAT (the app's)", GL_DEPTH_COMPONENT, GL_FLOAT,         4 },
    { "DEPTH_COMPONENT / UNSIGNED_INT",      GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,  4 },
    { "RGBA / UNSIGNED_BYTE",                GL_RGBA,            GL_UNSIGNED_BYTE, 4 },
    { "STENCIL_INDEX / UNSIGNED_BYTE",       GL_STENCIL_INDEX,   GL_UNSIGNED_BYTE, 1 }
};

/* What the pixels genuinely cost with no queue in the way: the floor any
 * "read less / read cheaper" idea would be optimizing toward. */
static void bench_transfer_floor(void) {
    const int iters = 20;
    size_t i;

    printf("\n-- transfer cost only (glFinish first, no queue) --\n");
    for (i = 0; i < sizeof k_fmts / sizeof k_fmts[0]; i++) {
        void *buf = malloc((size_t)g_w * (size_t)g_h * (size_t)k_fmts[i].bpp);
        double t0, t1;
        int n;
        if (!buf)
            continue;
        (void)glGetError();
        glReadPixels(0, 0, g_w, g_h, k_fmts[i].fmt, k_fmts[i].type, buf);
        glFinish();
        t0 = now_ms();
        for (n = 0; n < iters; n++)
            glReadPixels(0, 0, g_w, g_h, k_fmts[i].fmt, k_fmts[i].type, buf);
        glFinish();
        t1 = now_ms();
        printf("  %-38s %9.3f ms%s\n", k_fmts[i].name, (t1 - t0) / iters,
               glGetError() == GL_NO_ERROR ? "" : "  (unsupported)");
        free(buf);
    }
}

/* ------------------------------------------------------------------- main */

static void run_tail_benchmarks(void) {
    bench_transfer_floor();

    printf("\n-- label glyph submission --\n");
    bench_label_glyphs(0, g_labels, "labels, no blend (decluttered)");
    bench_label_glyphs(1, g_labels, "labels, additive blend (at-vertex)");
    bench_label_glyphs(1, g_labels * 4, "labels, additive blend, 4x count");

    printf("\n-- per-vertex modelview readback (walk callback) --\n");
    bench_get_modelview(0, 1, "push/transform/pop only");
    bench_get_modelview(1, 0, "glGetFloatv(GL_MODELVIEW_MATRIX) only");
    bench_get_modelview(1, 1, "transform + glGetFloatv (as the walk)");
}

static void display(void) {
    double frame_start = now_ms();
    double kick_ms = 0.0, consume_ms = 0.0, work_end;
    int counting;
    int i;

    if (g_phases_done)
        return;   /* phases finished; nothing further to present */
    counting = (g_frame >= g_warm);

    if (g_phase == PH_POSTSWAP)
        consume_ms = read_depth();   /* last frame's depth, before drawing */

    draw_scene();

    switch (g_phase) {
    case PH_NONE:      break;
    case PH_FLUSH: {
        double t0 = now_ms();
        glFlush();
        kick_ms = now_ms() - t0;
        break;
    }
    case PH_MID:       consume_ms = read_depth(); break;
    case PH_DRAINED: {
        double t0 = now_ms();
        glFinish();
        kick_ms = now_ms() - t0;      /* the drain, paid explicitly */
        consume_ms = read_depth();    /* what the pixels then cost  */
        break;
    }
    case PH_BACK:      consume_ms = read_color(GL_BACK); break;
    case PH_FRONT:     consume_ms = read_color(GL_FRONT);
                       glReadBuffer(GL_BACK);
                       break;
    case PH_POSTSWAP:  break;
    case PH_POSTFINISH: break;   /* handled after the glFinish below */
    case PH_PBO1:      read_via_pbo(1, 1, 0, &kick_ms, &consume_ms); break;
    case PH_PBO:       read_via_pbo(1, 0, 0, &kick_ms, &consume_ms); break;
    case PH_FENCE:     read_via_pbo(1, 0, 1, &kick_ms, &consume_ms); break;
    case PH_PBO_COLOR: read_via_pbo(0, 0, 0, &kick_ms, &consume_ms); break;
    default:           break;
    }

    /* Everything above is what the app calls Frame Work. */
    work_end = now_ms();

    /* gl_repl.c:131 - the drain the app already performs, outside Frame Work.
     * Present it in every phase so the comparison is against the real frame
     * shape rather than a hypothetical one. */
    glFinish();

    if (g_phase == PH_POSTFINISH)
        consume_ms = read_depth();   /* the queue is already drained here */

    glutSwapBuffers();

    if (counting) {
        g_kick_ms[g_phase]    += kick_ms;
        g_consume_ms[g_phase] += consume_ms;
        g_work_ms[g_phase]    += work_end - frame_start;
        g_frame_ms[g_phase]   += now_ms() - frame_start;
    }

    if (++g_frame >= g_warm + g_frames) {
        g_frame = 0;
        for (i = 0; i < 3; i++)
            g_pbo_filled[i] = 0;
        if (++g_phase >= PH_COUNT) {
            g_phases_done = 1;
            report_readback_phases();
            run_tail_benchmarks();
            fflush(stdout);
            exit(0);
        }
    }
    glutPostRedisplay();
}

static int parse_args(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--msaa") == 0) {
            g_msaa = 1;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g_w, &g_h) != 2 ||
                g_w <= 0 || g_h <= 0) {
                fprintf(stderr, "bad --size (want WxH)\n");
                return 0;
            }
        } else if (strcmp(argv[i], "--labels") == 0 && i + 1 < argc) {
            g_labels = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--vertices") == 0 && i + 1 < argc) {
            g_vertices = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_frames = atoi(argv[++i]);
            if (g_frames < 1)
                g_frames = 1;
        } else {
            fprintf(stderr,
                    "usage: %s [--msaa] [--size WxH] [--labels N]"
                    " [--vertices N] [--frames N]\n", argv[0]);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    unsigned mode = GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_STENCIL;
    GLint samples = 0, sample_buffers = 0, depth_bits = 0;
    int i;

    if (!parse_args(argc, argv))
        return 2;

    glutInit(&argc, argv);
    if (g_msaa)
        mode |= GLUT_MULTISAMPLE;
    glutInitDisplayMode(mode);
    glutInitWindowSize(g_w, g_h);
    glutCreateWindow("vertex-label overlay cost benchmark");

    g_buf = malloc((size_t)g_w * (size_t)g_h * 4);
    if (!g_buf) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    glGenBuffers(3, g_pbo);
    for (i = 0; i < 3; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER,
                     (GLsizeiptr)((size_t)g_w * (size_t)g_h * sizeof(float)),
                     NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sample_buffers);
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    printf("renderer: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("viewport %dx%d | MSAA requested %d -> SAMPLE_BUFFERS=%d SAMPLES=%d"
           " | DEPTH_BITS=%d\n",
           g_w, g_h, g_msaa, sample_buffers, samples, depth_bits);
    printf("labels %d | walk vertices %d | %d frames/phase (+%d warmup)\n",
           g_labels, g_vertices, g_frames, g_warm);

    glutDisplayFunc(display);
    glutMainLoop();
    return 0;
}
