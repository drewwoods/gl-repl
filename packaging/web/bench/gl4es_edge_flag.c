/* Correctness cases for the gl4es edge-flag emulation.
 *
 * This is a coverage oracle, not a timing benchmark. gl4es stubbed
 * glEdgeFlag to nothing, so polygon-mode lines drew every edge; the patch
 * records the flag per vertex and builds the boundary edges from the original
 * primitive topology, in vertex-specification order.
 *
 * Each case calibrates itself: every edge under test is first drawn alone as
 * a GL_LINES segment, so the expected coverage is derived from this
 * rasterizer rather than hard-coded. The assertions are then absolute -
 * "clearing vertex v's flag leaves exactly the perimeter minus edge v" -
 * which is what distinguishes suppressing the right edge from suppressing a
 * differently-sized one. A build that silently drew nothing fails the
 * calibration guard rather than reporting a pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glut.h>
#include <emscripten.h>

#define BENCH_WIDTH 640
#define BENCH_HEIGHT 480

static int g_frame;
static int g_failures;
static char g_report[512];

static int coverage(void)
{
    size_t bytes = (size_t)BENCH_WIDTH * BENCH_HEIGHT * 4;
    unsigned char *pixels = (unsigned char *)malloc(bytes);
    int covered = 0;
    if (!pixels)
        return -1;
    glReadPixels(0, 0, BENCH_WIDTH, BENCH_HEIGHT, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 0; i < bytes; i += 4) {
        if ((int)pixels[i] + pixels[i + 1] + pixels[i + 2] >= 32)
            ++covered;
    }
    free(pixels);
    return covered;
}

static void clear_scene(void)
{
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LINE_STIPPLE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEdgeFlag(GL_TRUE);
    glLineWidth(1.f);
    glColor3f(1.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static int finish_coverage(void)
{
    glFinish();
    return coverage();
}

static void fail(const char *what, int a, int b)
{
    ++g_failures;
    fprintf(stderr, "edge-flag cases: %s (%d vs %d)\n", what, a, b);
}

static void report(const char *fmt, int a, int b)
{
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             fmt, a, b);
}

static void report3(const char *fmt, int a, int b, int c)
{
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             fmt, a, b, c);
}

/* Geometry is calibrated against itself: each edge is first drawn on its own
 * as a GL_LINES segment, so the oracle knows what one edge is worth on this
 * rasterizer without hard-coding a pixel count. */
static const float g_quad_x[4] = {-0.5f,  0.5f,  0.5f, -0.5f};
static const float g_quad_y[4] = {-0.5f, -0.5f,  0.5f,  0.5f};

static int g_edge_px[4];

static void calibrate_quad_edges(void)
{
    for (int e = 0; e < 4; ++e) {
        clear_scene();
        glBegin(GL_LINES);
        glVertex3f(g_quad_x[e], g_quad_y[e], 0.f);
        glVertex3f(g_quad_x[(e + 1) % 4], g_quad_y[(e + 1) % 4], 0.f);
        glEnd();
        g_edge_px[e] = finish_coverage();
    }
}

/* A single quad. `off_vertex` is the index whose edge flag is cleared, or -1
 * to leave every edge enabled. */
static void quad(int off_vertex)
{
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) {
        glEdgeFlag(i == off_vertex ? GL_FALSE : GL_TRUE);
        glVertex3f(g_quad_x[i], g_quad_y[i], 0.f);
    }
    glEnd();
}

static int quad_perimeter_px(void)
{
    return g_edge_px[0] + g_edge_px[1] + g_edge_px[2] + g_edge_px[3];
}

/* Clearing vertex v's flag must suppress exactly the edge that *begins* at v -
 * every other boundary edge stays. Checked against the calibrated segments, so
 * this pins the absolute result rather than a delta against a baseline, which
 * still carries gl4es's triangulation diagonal for an unflagged quad.
 *
 * The tolerance absorbs shared corner pixels between adjoining edges. */
static void test_quad_suppresses_one_edge(void)
{
    int perimeter = quad_perimeter_px();
    if (perimeter < 400) {
        fail("edge calibration too small", perimeter, 400);
        return;
    }
    for (int v = 0; v < 4; ++v) {
        clear_scene();
        quad(v);
        int got = finish_coverage();
        int want = perimeter - g_edge_px[v];
        int slack = 8 + want / 64;
        if (got < want - slack || got > want + slack)
            fail("wrong edge suppressed", got, want);
        report3(" q%d=%d/%d", v, got, want);
    }
}

/* Same rule on a triangle, whose edges gl4es already emitted in specification
 * order - so this is the case the generator must not regress. */
static void test_triangle_suppresses_one_edge(void)
{
    static const float x[3] = {-0.6f, 0.6f, 0.0f};
    static const float y[3] = {-0.6f, -0.6f, 0.6f};
    int seg[3];
    for (int e = 0; e < 3; ++e) {
        clear_scene();
        glBegin(GL_LINES);
        glVertex3f(x[e], y[e], 0.f);
        glVertex3f(x[(e + 1) % 3], y[(e + 1) % 3], 0.f);
        glEnd();
        seg[e] = finish_coverage();
    }
    int perimeter = seg[0] + seg[1] + seg[2];

    for (int v = 0; v < 3; ++v) {
        clear_scene();
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < 3; ++i) {
            glEdgeFlag(i == v ? GL_FALSE : GL_TRUE);
            glVertex3f(x[i], y[i], 0.f);
        }
        glEnd();
        int got = finish_coverage();
        int want = perimeter - seg[v];
        int slack = 8 + want / 32;
        if (got < want - slack || got > want + slack)
            fail("triangle: wrong edge suppressed", got, want);
        report3(" t%d=%d/%d", v, got, want);
    }
}

/* The default is GL_TRUE, and geometry that never clears a flag must render
 * exactly as it did before the patch - no array is even allocated. */
static void test_default_is_unchanged(void)
{
    clear_scene();
    quad(-1);
    int explicit_true = finish_coverage();

    clear_scene();
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i)
        glVertex3f(g_quad_x[i], g_quad_y[i], 0.f);
    glEnd();
    int untouched = finish_coverage();

    if (untouched != explicit_true)
        fail("explicit GL_TRUE differs from the default", untouched,
             explicit_true);
    report(" default=%d/%d", untouched, explicit_true);
}

/* Edge flags are per-vertex state: a flag cleared for one primitive must not
 * leak into the next one in the same glBegin/glEnd block. */
static void test_flag_is_per_vertex(void)
{
    static const float x[6] = {-0.9f, -0.1f, -0.5f,  0.1f,  0.9f,  0.5f};
    static const float y[6] = {-0.9f, -0.9f, -0.1f, -0.9f, -0.9f, -0.1f};

    clear_scene();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 6; ++i) {
        /* clear only the first triangle's first vertex */
        glEdgeFlag(i == 0 ? GL_FALSE : GL_TRUE);
        glVertex3f(x[i], y[i], 0.f);
    }
    glEnd();
    int mixed = finish_coverage();

    clear_scene();
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 6; ++i) {
        glEdgeFlag(GL_TRUE);
        glVertex3f(x[i], y[i], 0.f);
    }
    glEnd();
    int all = finish_coverage();

    /* the suppressed edge is one of six; losing much more means it leaked */
    int removed = all - mixed;
    if (removed < 50)
        fail("per-vertex flag had no effect", removed, 50);
    if (removed > all / 3)
        fail("per-vertex flag leaked past its primitive", removed, all / 3);
    report(" pervertex=%d/%d", all, mixed);
}

/* GL_FILL ignores edge flags entirely - they only select boundary edges when
 * the polygon mode draws lines. */
static void test_fill_mode_ignores_flags(void)
{
    clear_scene();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    quad(-1);
    int all = finish_coverage();

    clear_scene();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    quad(0);
    int one_off = finish_coverage();

    if (all != one_off)
        fail("GL_FILL honored an edge flag", all, one_off);
    report(" fill=%d/%d", all, one_off);
}

/* glPushAttrib(GL_CURRENT_BIT) must save and restore the flag. */
static void test_push_attrib_round_trip(void)
{
    clear_scene();
    glEdgeFlag(GL_TRUE);
    glPushAttrib(GL_CURRENT_BIT);
    glEdgeFlag(GL_FALSE);
    glPopAttrib();

    GLboolean flag = GL_FALSE;
    glGetBooleanv(GL_EDGE_FLAG, &flag);
    if (flag != GL_TRUE)
        fail("GL_CURRENT_BIT did not restore the edge flag", flag, GL_TRUE);
    report(" pushattrib=%d/%d", flag, GL_TRUE);
}

/* glGetBooleanv(GL_EDGE_FLAG) used to fall through to GLES and raise
 * GL_INVALID_ENUM without writing anything. */
static void test_getter(void)
{
    clear_scene();
    while (glGetError() != GL_NO_ERROR)
        ;

    GLboolean flag = GL_FALSE;
    glEdgeFlag(GL_TRUE);
    glGetBooleanv(GL_EDGE_FLAG, &flag);
    int ok_true = (flag == GL_TRUE && glGetError() == GL_NO_ERROR);

    flag = GL_TRUE;
    glEdgeFlag(GL_FALSE);
    glGetBooleanv(GL_EDGE_FLAG, &flag);
    int ok_false = (flag == GL_FALSE && glGetError() == GL_NO_ERROR);
    glEdgeFlag(GL_TRUE);

    if (!ok_true || !ok_false)
        fail("GL_EDGE_FLAG query", ok_true, ok_false);
    report(" getter=%d/%d", ok_true, ok_false);
}

/* gl4es merges consecutive compatible renderlists (extend_renderlist ->
 * append_renderlist). A merged batch must carry the flags of both halves,
 * including the half that never allocated an array, and each triangle's edges
 * must still be built from its own three vertices.
 *
 * Triangles, not quads: a quad's edges come from two different arms of
 * fill_lineIndices() depending on how many share a glBegin, so a mixed batch
 * of them would fold that variable into this case. A triangle's edge set is
 * the same on every path, which makes the expected total exact however the
 * merging falls out. */
static void test_merged_batches(void)
{
    const int tris = 8;
    int seg[3];
    int perim;
    int want;

    /* one triangle's three edges, calibrated alone */
    {
        float vx[3] = {-0.9f, -0.7f, -0.8f};
        float vy[3] = { 0.1f,  0.1f,  0.6f};
        for (int e = 0; e < 3; ++e) {
            clear_scene();
            glBegin(GL_LINES);
            glVertex3f(vx[e], vy[e], 0.f);
            glVertex3f(vx[(e + 1) % 3], vy[(e + 1) % 3], 0.f);
            glEnd();
            seg[e] = finish_coverage();
        }
        perim = seg[0] + seg[1] + seg[2];
    }

    clear_scene();
    for (int i = 0; i < tris; ++i) {
        float x = -0.9f + (float)i * 0.22f;
        float vx[3] = {x, x + 0.2f, x + 0.1f};
        float vy[3] = {0.1f, 0.1f, 0.6f};
        /* clear the flag on odd batches only, so the merge sees both an
         * array-carrying and an array-less list */
        int off = (i & 1) ? 0 : -1;
        glBegin(GL_TRIANGLES);
        for (int v = 0; v < 3; ++v) {
            glEdgeFlag(v == off ? GL_FALSE : GL_TRUE);
            glVertex3f(vx[v], vy[v], 0.f);
        }
        glEnd();
    }
    int got = finish_coverage();

    want = tris * perim - (tris / 2) * seg[0];
    int slack = 16 + want / 16;
    if (got < want - slack || got > want + slack)
        fail("merged batches lost or mixed up their edge flags", got, want);
    report(" merged=%d/%d", got, want);
}

static void display(void)
{
    if (g_frame == 0) {
        calibrate_quad_edges();
        test_quad_suppresses_one_edge();
        test_triangle_suppresses_one_edge();
        test_default_is_unchanged();
        test_flag_is_per_vertex();
        test_fill_mode_ignores_flags();
        test_push_attrib_round_trip();
        test_getter();
        test_merged_batches();
        printf("edge-flag cases:%s failures=%d\n", g_report, g_failures);
        EM_ASM({
            document.title = (UTF8ToString($0)) + " " + UTF8ToString($1);
        }, g_failures ? "FAIL edge-flag cases" : "edge-flag cases", g_report);
    }
    ++g_frame;
    glutSwapBuffers();
}

static void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(BENCH_WIDTH, BENCH_HEIGHT);
    glutCreateWindow("gl4es edge-flag cases");
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glViewport(0, 0, BENCH_WIDTH, BENCH_HEIGHT);
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutMainLoop();
    return 0;
}
