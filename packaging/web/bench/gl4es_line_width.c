/* Browser coverage oracle for gl4es wide-line quads.
 *
 * Draws a few dozen horizontal segments at widths 1 / 1.5 / 3 / 6 plus a
 * zero-length segment. Width 1 must stay a native 1 px line. Width 6 must
 * cover on the order of 6x a 1 px line of the same screen length (not 6x
 * the canvas). The page title is FAIL below the coverage floor so an empty
 * frame cannot report a speedup.
 *
 * Also a 4-vertex GL_LINE_LOOP at width 4: coverage must include the
 * closing last-first edge (the one gen_stipple_tex_coords skips).
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
#define COVERAGE_FLOOR_W1  800
#define COVERAGE_FLOOR_W6  4000
#define COVERAGE_FLOOR_LOOP 400

static int g_frame;
static int g_cov_w1, g_cov_w15, g_cov_w3, g_cov_w6, g_cov_loop, g_cov_zero;

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

static void draw_loop(void)
{
    glBegin(GL_LINE_LOOP);
    glVertex3f(-0.3f, -0.3f, 0.f);
    glVertex3f( 0.3f, -0.3f, 0.f);
    glVertex3f( 0.3f,  0.3f, 0.f);
    glVertex3f(-0.3f,  0.3f, 0.f);
    glEnd();
}

static void scene(float width, int segs, int loop, int zero)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.f, 1.f, 1.f);
    glLineWidth(width);
    if (zero)
        draw_zero_length();
    else if (loop)
        draw_loop();
    else
        draw_segments(-0.8f, 1.6f / (float)segs, segs);
}

static int shot(float width, int segs, int loop, int zero)
{
    scene(width, segs, loop, zero);
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
        g_cov_loop = shot(4.f, 0, 1, 0);
        g_cov_zero = shot(4.f, 0, 0, 1);
        printf("line-width coverage: w1=%d w1.5=%d w3=%d w6=%d loop=%d zero=%d\n",
               g_cov_w1, g_cov_w15, g_cov_w3, g_cov_w6, g_cov_loop, g_cov_zero);

        int fail = 0;
        if (g_cov_w1 < COVERAGE_FLOOR_W1)
            fail = 1;
        if (g_cov_w6 < COVERAGE_FLOOR_W6)
            fail = 1;
        if (g_cov_w6 < g_cov_w1 * 3)
            fail = 1;
        if (g_cov_loop < COVERAGE_FLOOR_LOOP)
            fail = 1;
        if (g_cov_zero < 0)
            fail = 1;
        if (fail) {
            fprintf(stderr, "line-width benchmark: coverage guard failed\n");
            EM_ASM({ document.title = "FAIL coverage"; });
        } else {
            EM_ASM({
                document.title = "line-width w1 " + $0 + " w6 " + $1 +
                                 " loop " + $2;
            }, g_cov_w1, g_cov_w6, g_cov_loop);
        }
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
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return 0;
}
