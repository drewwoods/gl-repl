/*
 * Shared native/browser rendering benchmark.
 *
 * The native build is useful for a local baseline and for checking that a
 * change did not make the fixed-function workload regress everywhere. The
 * Emscripten build is the important one: it runs the same draw calls through
 * gl4es -> WebGL2 and publishes a small browser-readable result object.
 *
 * This is deliberately a metric benchmark rather than an exact screenshot
 * comparison. Antialiasing, color management, and the native driver vary
 * between machines, so each case reports timing plus stable pixel invariants
 * (coverage, channel energy, probes, and an informational FNV hash). The
 * browser build turns the feature-specific invariants into pass/fail oracles;
 * native builds keep reporting them even when the host does not implement an
 * emulated feature such as wide lines.
 */

#define _POSIX_C_SOURCE 200809L

#include "gl_includes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

enum {
    BENCH_WIDTH = 640,
    BENCH_HEIGHT = 480,
    DEFAULT_REPS = 8,
    TRI_COLS = 48,
    TRI_ROWS = 48,
    POINT_COUNT = 96,
    LINE_COUNT = 40,
};

typedef struct {
    const char *name;
    void (*draw)(void);
} RenderCase;

typedef struct {
    double ms_per_frame;
    int coverage;
    int bright;
    unsigned int hash;
    unsigned long long energy;
    unsigned char center[3];
    unsigned char left[3];
    unsigned char right[3];
    int oracle_failed;
} RenderMetrics;

static int g_reps = DEFAULT_REPS;
static int g_csv;
static int g_strict;
static int g_case_index;
static int g_case_count;
static int g_failures;

static double now_ms(void)
{
#if defined(__EMSCRIPTEN__)
    return emscripten_get_now();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1.0e-6;
#endif
}

static void flat_projection(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void reset_frame(void)
{
    glDisable(GL_BLEND);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_LINE_STIPPLE);
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_TEXTURE_2D);
    glLineWidth(1.0f);
    glPointSize(1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glViewport(0, 0, BENCH_WIDTH, BENCH_HEIGHT);
    flat_projection();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void draw_triangles(void)
{
    int row;
    int col;

    glBegin(GL_TRIANGLES);
    for (row = 0; row < TRI_ROWS; ++row) {
        for (col = 0; col < TRI_COLS; ++col) {
            float x0 = -0.96f + 1.92f * (float)col / (float)TRI_COLS;
            float x1 = -0.96f + 1.92f * (float)(col + 1) / (float)TRI_COLS;
            float y0 = -0.96f + 1.92f * (float)row / (float)TRI_ROWS;
            float y1 = -0.96f + 1.92f * (float)(row + 1) / (float)TRI_ROWS;
            float hue = (float)((row * TRI_COLS + col) % 17) / 16.0f;

            glColor3f(0.15f + 0.55f * hue, 0.25f + 0.65f * (1.0f - hue),
                      0.9f);
            glVertex2f(x0, y0);
            glVertex2f(x1, y0);
            glVertex2f(x1, y1);
            glVertex2f(x0, y0);
            glVertex2f(x1, y1);
            glVertex2f(x0, y1);
        }
    }
    glEnd();
}

static void draw_points(void)
{
    int i;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glPointSize(18.0f);
    glBegin(GL_POINTS);
    for (i = 0; i < POINT_COUNT; ++i) {
        int col = i % 12;
        int row = i / 12;
        float x = -0.86f + 1.72f * (float)col / 11.0f;
        float y = -0.60f + 1.20f * (float)row / 7.0f;
        glColor4f(0.25f + 0.65f * (float)(i & 1),
                  0.35f + 0.50f * (float)((i / 2) & 1), 1.0f, 0.82f);
        glVertex2f(x, y);
    }
    glEnd();

    /* The reset is intentional. gl4es must keep the size on the batch that
     * was already submitted; using the final live state would collapse the
     * large points to one pixel. */
    glPointSize(1.0f);
}

static void draw_wide_lines(void)
{
    int i;

    glColor3f(0.95f, 0.85f, 0.20f);
    glLineWidth(7.0f);
    glBegin(GL_LINES);
    for (i = 0; i < LINE_COUNT; ++i) {
        float y = -0.90f + 1.80f * (float)i / (float)(LINE_COUNT - 1);
        glVertex2f(-0.92f, y);
        glVertex2f(0.92f, y);
    }
    glEnd();

    glColor3f(0.20f, 0.90f, 1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(5.0f);
    glBegin(GL_TRIANGLES);
    for (i = 0; i < 6; ++i) {
        float x = -0.75f + 0.30f * (float)i;
        glVertex2f(x, -0.18f);
        glVertex2f(x + 0.22f, -0.18f);
        glVertex2f(x + 0.11f, 0.22f);
    }
    glEnd();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.0f);
}

static void draw_attrib_stack(void)
{
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor3f(1.0f, 0.0f, 0.0f);
    glLineWidth(8.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glPopAttrib();

    /* After the pop this must be a filled green quad, with the blend and
     * polygon state restored. It is a compact state-leak oracle. */
    glColor3f(0.10f, 0.90f, 0.25f);
    glBegin(GL_QUADS);
    glVertex2f(-0.72f, -0.62f);
    glVertex2f(0.72f, -0.62f);
    glVertex2f(0.72f, 0.62f);
    glVertex2f(-0.72f, 0.62f);
    glEnd();
}

static void draw_accumulation(void)
{
    const GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    glClearAccum(zero[0], zero[1], zero[2], zero[3]);
    glClear(GL_ACCUM_BUFFER_BIT);

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glAccum(GL_LOAD, 1.0f);

    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glAccum(GL_ACCUM, 1.0f);
    glAccum(GL_RETURN, 0.5f);
}

static void draw_bitmap_perspective(void)
{
    static const char text[] = "GL4ES RENDER";
    size_t i;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -0.75, 0.75, 0.1, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(0.95f, 0.95f, 0.95f);
    glRasterPos3f(-0.72f, -0.10f, -2.0f);
    for (i = 0; i + 1 < sizeof(text); ++i)
        glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)text[i]);
}

static void draw_material_faces(void)
{
    static const GLfloat ambient[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    static const GLfloat no_diffuse[] = { 0.0f, 0.0f, 0.0f, 1.0f };

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, no_diffuse);
    glEnable(GL_COLOR_MATERIAL);
    glNormal3f(0.0f, 0.0f, 1.0f);

    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glColor3f(1.0f, 0.08f, 0.08f);
    glBegin(GL_QUADS);
    glVertex2f(-0.82f, -0.55f);
    glVertex2f(-0.08f, -0.55f);
    glVertex2f(-0.08f, 0.55f);
    glVertex2f(-0.82f, 0.55f);
    glEnd();

    glColorMaterial(GL_BACK, GL_AMBIENT_AND_DIFFUSE);
    glColor3f(0.08f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0.08f, -0.55f);
    glVertex2f(0.08f, 0.55f);
    glVertex2f(0.82f, 0.55f);
    glVertex2f(0.82f, -0.55f);
    glEnd();
}

static const RenderCase g_cases[] = {
    { "triangles", draw_triangles },
    { "points", draw_points },
    { "wide-lines", draw_wide_lines },
    { "attrib-stack", draw_attrib_stack },
    { "accumulation", draw_accumulation },
    { "bitmap-perspective", draw_bitmap_perspective },
    { "material-faces", draw_material_faces },
};

static int case_count(void)
{
    return (int)(sizeof(g_cases) / sizeof(g_cases[0]));
}

static int pixel_is_bright(const unsigned char *pixel)
{
    return pixel[0] > 220 && pixel[1] > 220 && pixel[2] > 220;
}

static void copy_pixel(const unsigned char *pixels, int x, int y,
                       unsigned char out[3])
{
    size_t offset = ((size_t)y * BENCH_WIDTH + (size_t)x) * 4u;
    out[0] = pixels[offset + 0];
    out[1] = pixels[offset + 1];
    out[2] = pixels[offset + 2];
}

static int measure_pixels(RenderMetrics *metrics)
{
    size_t bytes = (size_t)BENCH_WIDTH * BENCH_HEIGHT * 4u;
    unsigned char *pixels = (unsigned char *)malloc(bytes);
    size_t i;
    unsigned int hash = 2166136261u;

    if (!pixels)
        return 0;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, BENCH_WIDTH, BENCH_HEIGHT, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    metrics->coverage = 0;
    metrics->bright = 0;
    metrics->energy = 0;
    for (i = 0; i < bytes; i += 4u) {
        unsigned int rgb = (unsigned int)pixels[i] +
                           (unsigned int)pixels[i + 1] +
                           (unsigned int)pixels[i + 2];
        if (rgb >= 32u)
            ++metrics->coverage;
        if (pixel_is_bright(pixels + i))
            ++metrics->bright;
        metrics->energy += rgb;
        hash ^= pixels[i + 0]; hash *= 16777619u;
        hash ^= pixels[i + 1]; hash *= 16777619u;
        hash ^= pixels[i + 2]; hash *= 16777619u;
        hash ^= pixels[i + 3]; hash *= 16777619u;
    }
    metrics->hash = hash;
    copy_pixel(pixels, BENCH_WIDTH / 2, BENCH_HEIGHT / 2, metrics->center);
    copy_pixel(pixels, BENCH_WIDTH * 3 / 10, BENCH_HEIGHT / 2,
               metrics->left);
    copy_pixel(pixels, BENCH_WIDTH * 7 / 10, BENCH_HEIGHT / 2,
               metrics->right);
    free(pixels);
    return 1;
}

static int material_oracle(const RenderMetrics *m)
{
    return m->left[0] <= m->left[1] * 2u ||
           m->right[1] <= m->right[0] * 2u;
}

static int oracle_for_case(int index, const RenderMetrics *m)
{
    switch (index) {
    case 0: return m->coverage < 100000;
    case 1: return m->coverage < 5000 || m->coverage > 29000;
    case 2: return m->coverage < 80000;
    case 3: return m->center[1] < 120 ||
                   m->center[1] <= m->center[0] * 2u;
    case 4: return m->center[0] < 32 || m->center[2] < 32 ||
                   abs((int)m->center[0] - (int)m->center[2]) > 100;
    case 5: return m->coverage < 100;
    case 6: return material_oracle(m);
    default: return 1;
    }
}

static void publish_case(const RenderCase *c, const RenderMetrics *m)
{
    const char *oracle_status = m->oracle_failed
        ? (g_strict ? "FAIL" : "WARN") : "PASS";

    if (g_csv) {
        printf("render_case,%s,%d,%.4f,%d,%d,%u,%llu,%d,%d,%d,%d,%d,%d,%d,%d\n",
               c->name, g_reps, m->ms_per_frame, m->coverage, m->bright,
               m->hash, m->energy, m->center[0], m->center[1], m->center[2],
               m->left[0], m->left[1], m->right[0], m->right[1],
               m->oracle_failed);
    } else {
        printf("render %-18s reps=%d ms/frame=%.4f coverage=%d bright=%d "
               "hash=%u center=%u/%u/%u probes=%u/%u:%u/%u oracle=%s\n",
               c->name, g_reps, m->ms_per_frame, m->coverage, m->bright,
               m->hash, m->center[0], m->center[1], m->center[2],
               m->left[0], m->left[1], m->right[0], m->right[1],
               oracle_status);
    }

#if defined(__EMSCRIPTEN__)
    EM_ASM({
        var rows = window.gl4esRenderBenchResults || [];
        rows.push({name: UTF8ToString($0), reps: $1, msPerFrame: $2,
                   coverage: $3, bright: $4, hash: $5, energy: $6,
                   center: [$7, $8, $9], left: [$10, $11],
                   right: [$12, $13], oracle: $14 ? "FAIL" : "PASS"});
        window.gl4esRenderBenchResults = rows;
    }, c->name, g_reps, m->ms_per_frame, m->coverage, m->bright,
       m->hash, (double)m->energy, m->center[0], m->center[1], m->center[2],
       m->left[0], m->left[1], m->right[0], m->right[1],
       m->oracle_failed);
#endif
}

static RenderMetrics run_case(int index)
{
    RenderMetrics metrics;
    int i;
    double start;

    memset(&metrics, 0, sizeof(metrics));
    reset_frame();
    start = now_ms();
    for (i = 0; i < g_reps; ++i) {
        reset_frame();
        g_cases[index].draw();
    }
    glFinish();
    metrics.ms_per_frame = (now_ms() - start) / (double)g_reps;

    reset_frame();
    g_cases[index].draw();
    glFinish();
    if (!measure_pixels(&metrics))
        metrics.oracle_failed = 1;
    else
        metrics.oracle_failed = oracle_for_case(index, &metrics);
    return metrics;
}

static void publish_summary(void)
{
#if defined(__EMSCRIPTEN__)
    EM_ASM({
        var failed = $0 !== 0;
        var status = failed ? "FAIL" : "PASS";
        window.gl4esRenderBench = {
            status: status, cases: $1, failures: $0,
            strict: $2 !== 0, results: window.gl4esRenderBenchResults || []
        };
        document.documentElement.setAttribute("data-gl4es-render-bench", status);
        document.title = status + " gl4es render bench";
    }, g_failures, g_case_count, g_strict);
#endif
}

static void display(void)
{
    if (g_case_index < g_case_count) {
        RenderMetrics metrics = run_case(g_case_index);
        if (metrics.oracle_failed && g_strict)
            ++g_failures;
        publish_case(&g_cases[g_case_index], &metrics);
        ++g_case_index;
        glutPostRedisplay();
    } else {
        publish_summary();
        glutIdleFunc(NULL);
#if !defined(__EMSCRIPTEN__)
        exit(g_failures ? 1 : 0);
#endif
    }
    glutSwapBuffers();
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--reps N] [--csv] [--strict]\n"
            "  --strict  turn pixel-invariant failures into a non-zero exit\n"
            "            (enabled automatically by the browser build)\n",
            program);
}

static int parse_args(int argc, char **argv)
{
    int i;

#if defined(__EMSCRIPTEN__)
    g_strict = 1;
#endif
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            g_reps = atoi(argv[++i]);
            if (g_reps < 1 || g_reps > 10000)
                return 0;
        } else if (strcmp(argv[i], "--csv") == 0) {
            g_csv = 1;
        } else if (strcmp(argv[i], "--strict") == 0) {
            g_strict = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (!parse_args(argc, argv))
        return 2;

    g_case_count = case_count();
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH | GLUT_STENCIL |
                        GLUT_ACCUM);
    glutInitWindowSize(BENCH_WIDTH, BENCH_HEIGHT);
    glutCreateWindow("gl-repl rendering benchmark");
    glDisable(GL_DITHER);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (g_csv)
        printf("case,reps,ms_per_frame,coverage,bright,hash,energy,center_r,"
               "center_g,center_b,left_r,left_g,right_r,right_g,oracle\n");
    else
        printf("render benchmark: cases=%d reps=%d strict=%d\n",
               g_case_count, g_reps, g_strict);

    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return g_failures ? 1 : 0;
}
