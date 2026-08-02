/* Browser benchmark for the gl4es polygon-line patch.
 *
 * Build twice: GL4ES_BENCH_DISPLAY_LIST=0 emits 128 short-lived immediate
 * renderlists per frame; =1 measures a compiled display list, including the
 * append_calllist shallow-copy and source-deletion lifecycle. The first frame
 * also reads back coverage so a blank frame cannot report a speedup.
 */
#include <stdio.h>
#include <stdlib.h>

#include <GL/gl.h>
#include <GL/glut.h>
#include <emscripten.h>

#ifndef GL4ES_BENCH_DISPLAY_LIST
#define GL4ES_BENCH_DISPLAY_LIST 0
#endif

#define BENCH_WIDTH 640
#define BENCH_HEIGHT 480
#define BENCH_TRIANGLES 16000
#define BENCH_BATCHES 128
#define BENCH_SAMPLE_FIRST 21
#define BENCH_SAMPLE_LAST 120

static GLuint g_list;
static int g_frame;
static double g_setup_ms;
static double g_first_ms;
static double g_later_ms;
static int g_coverage;

static void emit_triangle_range(int begin, int end)
{
    glBegin(GL_TRIANGLES);
    for (int i = begin; i < end; ++i) {
        float x = (float)(i % 160) / 80.0f - 1.0f;
        float y = (float)(i / 160) / 50.0f - 1.0f;
        glColor3f(0.2f + (float)(i & 1) * 0.6f, 0.8f, 1.0f);
        glVertex3f(x, y, 0.0f);
        glVertex3f(x + 0.01f, y, 0.0f);
        glVertex3f(x, y + 0.01f, 0.0f);
    }
    glEnd();
}

static void emit_immediate_geometry(void)
{
    for (int batch = 0; batch < BENCH_BATCHES; ++batch) {
        int begin = batch * BENCH_TRIANGLES / BENCH_BATCHES;
        int end = (batch + 1) * BENCH_TRIANGLES / BENCH_BATCHES;
        emit_triangle_range(begin, end);
    }
}

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

static void draw_benchmark_geometry(void)
{
#if GL4ES_BENCH_DISPLAY_LIST
    glCallList(g_list);
#else
    emit_immediate_geometry();
#endif
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    double start = emscripten_get_now();
    draw_benchmark_geometry();
    glFinish();
    double elapsed = emscripten_get_now() - start;

    if (g_frame == 0) {
        g_first_ms = elapsed;
        g_coverage = measure_coverage();
    } else if (g_frame >= BENCH_SAMPLE_FIRST &&
               g_frame <= BENCH_SAMPLE_LAST) {
        g_later_ms += elapsed;
    }

    if (g_frame == BENCH_SAMPLE_LAST) {
        double average = g_later_ms /
                         (BENCH_SAMPLE_LAST - BENCH_SAMPLE_FIRST + 1);
        const char *kind = GL4ES_BENCH_DISPLAY_LIST ? "display-list" :
                                                        "immediate";
        printf("polygon-line %s: setup %.3f ms, first %.3f ms, "
               "steady %.3f ms, coverage %d px\n",
               kind, g_setup_ms, g_first_ms, average, g_coverage);
        EM_ASM({
            document.title = UTF8ToString($0) + " first " +
                             $1.toFixed(2) + " ms steady " +
                             $2.toFixed(2) + " ms coverage " + $3;
        }, kind, g_first_ms, average, g_coverage);
        if (g_coverage < 1000) {
            fprintf(stderr, "polygon-line benchmark: coverage guard failed\n");
            EM_ASM({ document.title = "FAIL coverage " + $0; }, g_coverage);
        }
    }

    ++g_frame;
    glutSwapBuffers();
}

static void build_display_list_benchmark(void)
{
#if GL4ES_BENCH_DISPLAY_LIST
    GLuint source = glGenLists(1);
    glNewList(source, GL_COMPILE);
    emit_triangle_range(0, BENCH_TRIANGLES);
    glEndList();

    double start = emscripten_get_now();
    glCallList(source);
    glFinish();
    g_setup_ms = emscripten_get_now() - start;

    g_list = glGenLists(1);
    glNewList(g_list, GL_COMPILE);
    glCallList(source);
    glEndList();
    glDeleteLists(source, 1);
#endif
}

int main(int argc, char **argv)
{
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(BENCH_WIDTH, BENCH_HEIGHT);
    glutCreateWindow("gl4es polygon-line benchmark");

    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    build_display_list_benchmark();

    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutMainLoop();
    return 0;
}
