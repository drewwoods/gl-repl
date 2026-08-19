/* Coverage oracle for gl4es wide-line quads (not a timing bench).
 *
 * 40 horizontal segments at widths 1 / 1.5 / 3 / 6, a 4-vertex loop
 * vs the same verts as a strip, and a zero-length segment. Width 1
 * stays on the native GL_LINES path and must have substantial
 * coverage; width 6 must cover ~N_SEGS * 512 px * 6 (0.8 slack), so
 * a half-width implementation fails. Loop minus strip must be about
 * one edge. Title is FAIL below a floor so an empty frame cannot
 * look successful.
 */
#include <stdio.h>
#include <stdlib.h>

#include <GL/gl.h>
#include <GL/glut.h>
#include <emscripten.h>

#define BENCH_WIDTH 640
#define BENCH_HEIGHT 480
#define N_SEGS 40
#define LINE_X0 (-0.8f)
#define LINE_X1 ( 0.8f)
#define LINE_PX  ((LINE_X1 - LINE_X0) * (BENCH_WIDTH * 0.5f)) /* 512 */
#define COVERAGE_FLOOR_W1  800
#define COVERAGE_FLOOR_W6  ((int)(N_SEGS * LINE_PX * 6 * 0.8f)) /* 98304 */
#define LOOP_EDGE_PX  (0.6f * (BENCH_WIDTH * 0.5f)) /* 192 */
#define COVERAGE_FLOOR_LOOP_DELTA  ((int)(LOOP_EDGE_PX * 4 * 0.5f)) /* 384 */
#define COVERAGE_CEIL_ZERO 200

static int g_frame;
static int g_cov_w1, g_cov_w15, g_cov_w3, g_cov_w6;
static int g_cov_loop, g_cov_strip, g_cov_zero;

static int measure_coverage(void)
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

static void draw_segments(float y0, float ystep, int n)
{
    glBegin(GL_LINES);
    for (int i = 0; i < n; ++i) {
        float y = y0 + (float)i * ystep;
        glVertex3f(LINE_X0, y, 0.f);
        glVertex3f(LINE_X1, y, 0.f);
    }
    glEnd();
}

static void draw_zero_length(void)
{
    glBegin(GL_LINES);
    glVertex3f(0.f, 0.f, 0.f);
    glVertex3f(0.f, 0.f, 0.f);
    glEnd();
}

static void draw_square(GLenum mode)
{
    glBegin(mode);
    glVertex3f(-0.3f, -0.3f, 0.f);
    glVertex3f( 0.3f, -0.3f, 0.f);
    glVertex3f( 0.3f,  0.3f, 0.f);
    glVertex3f(-0.3f,  0.3f, 0.f);
    glEnd();
}

static void reshape(int w, int h)
{
    glViewport(0, 0, w, h);
}

static void scene(float width, int segs, GLenum extra, int zero)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.f, 1.f, 1.f);
    glLineWidth(width);
    if (zero)
        draw_zero_length();
    else if (extra)
        draw_square(extra);
    else
        draw_segments(-0.8f, 1.6f / (float)segs, segs);
}

static int shot(float width, int segs, GLenum extra, int zero)
{
    scene(width, segs, extra, zero);
    glFinish();
    return measure_coverage();
}

static void display(void)
{
    if (g_frame == 0) {
        GLfloat range[2] = {0.f, 0.f};
        glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
        printf("ALIASED_LINE_WIDTH_RANGE [%.1f, %.1f]\n",
               range[0], range[1]);
        g_cov_w1 = shot(1.f, N_SEGS, 0, 0);
        g_cov_w15 = shot(1.5f, N_SEGS, 0, 0);
        g_cov_w3 = shot(3.f, N_SEGS, 0, 0);
        g_cov_w6 = shot(6.f, N_SEGS, 0, 0);
        g_cov_strip = shot(4.f, 0, GL_LINE_STRIP, 0);
        g_cov_loop = shot(4.f, 0, GL_LINE_LOOP, 0);
        g_cov_zero = shot(4.f, 0, 0, 1);
        printf("line-width coverage: w1=%d w1.5=%d w3=%d w6=%d loop=%d strip=%d zero=%d\n",
               g_cov_w1, g_cov_w15, g_cov_w3, g_cov_w6,
               g_cov_loop, g_cov_strip, g_cov_zero);

        int fail = 0;
        if (g_cov_w1 < COVERAGE_FLOOR_W1)
            fail = 1;
        if (g_cov_w6 < COVERAGE_FLOOR_W6)
            fail = 1;
        if (g_cov_loop - g_cov_strip < COVERAGE_FLOOR_LOOP_DELTA)
            fail = 1;
        if (g_cov_zero < 0 || g_cov_zero > COVERAGE_CEIL_ZERO)
            fail = 1;
        EM_ASM({
            document.title = (UTF8ToString($0)) +
                             " w1 " + $1 + " w1.5 " + $2 + " w3 " + $3 +
                             " w6 " + $4 + " loop " + $5 + " strip " + $6 +
                             " zero " + $7;
        }, fail ? "FAIL coverage" : "line-width",
           g_cov_w1, g_cov_w15, g_cov_w3, g_cov_w6,
           g_cov_loop, g_cov_strip, g_cov_zero);
        if (fail)
            fprintf(stderr, "line-width benchmark: coverage guard failed\n");
    } else {
        scene(6.f, N_SEGS, 0, 0);
    }
    ++g_frame;
    glutSwapBuffers();
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(BENCH_WIDTH, BENCH_HEIGHT);
    glutCreateWindow("gl4es line-width benchmark");
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glViewport(0, 0, BENCH_WIDTH, BENCH_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutMainLoop();
    return 0;
}
