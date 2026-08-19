/* Regression cases for the gl4es line-width emulator.
 *
 * This is intentionally a coverage/state oracle, not a timing benchmark.
 * It exercises the cases that the small geometry oracle cannot reach:
 * trailing width resets, culling, near-plane clipping, stipple, client
 * arrays, compiled-list width changes, source-list deletion, and the
 * polygon-mode line-array/VBO path.
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

static int bright_coverage(void)
{
    size_t bytes = (size_t)BENCH_WIDTH * BENCH_HEIGHT * 4;
    unsigned char *pixels = (unsigned char *)malloc(bytes);
    int covered = 0;
    if (!pixels)
        return -1;
    glReadPixels(0, 0, BENCH_WIDTH, BENCH_HEIGHT, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 0; i < bytes; i += 4) {
        if (pixels[i] > 220 && pixels[i + 1] > 220 && pixels[i + 2] > 220)
            ++covered;
    }
    free(pixels);
    return covered;
}

static void clear_scene(void)
{
    glDisable(GL_LINE_STIPPLE);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_CLIP_PLANE0);
    glDisable(GL_FOG);
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glLineWidth(1.f);
    glColor3f(1.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static int finish_coverage(void)
{
    glFinish();
    return coverage();
}

static void horizontal_line(float y, float x0, float x1)
{
    glBegin(GL_LINES);
    glVertex3f(x0, y, 0.f);
    glVertex3f(x1, y, 0.f);
    glEnd();
}

static void test_trailing_reset(void)
{
    clear_scene();
    glLineWidth(8.f);
    horizontal_line(0.f, -0.8f, 0.8f);
    glLineWidth(1.f);
    int got = finish_coverage();
    if (got < 3000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: trailing-reset coverage %d\n", got);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " reset=%d", got);
}

static void test_culling(void)
{
    clear_scene();
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glLineWidth(6.f);
    glBegin(GL_LINES);
    glVertex3f(-0.8f, -0.7f, 0.f);
    glVertex3f( 0.8f,  0.7f, 0.f);
    glVertex3f(-0.8f,  0.7f, 0.f);
    glVertex3f( 0.8f, -0.7f, 0.f);
    glEnd();
    glFrontFace(GL_CW);
    horizontal_line(0.f, -0.8f, 0.8f);
    int got = finish_coverage();
    if (got < 5000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: culling coverage %d\n", got);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " cull=%d", got);
}

static void set_perspective(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.f, 1.f, -1.f, 1.f, 0.1f, 10.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void restore_flat_projection(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void test_near_plane(void)
{
    clear_scene();
    set_perspective();
    glLineWidth(8.f);
    glBegin(GL_LINES);
    /* One endpoint is behind the eye; the other is well in front of the
     * near plane. The surviving half must not become a giant sliver. */
    glVertex3f(-0.4f, 0.f, 0.5f);
    glVertex3f( 0.4f, 0.f, -2.f);
    glEnd();
    int got = finish_coverage();
    restore_flat_projection();
    if (got < 100 || got > 50000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: near-plane coverage %d\n", got);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " near=%d", got);
}

static void test_stipple(void)
{
    clear_scene();
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, (GLushort)0x00ff);
    glLineWidth(4.f);
    horizontal_line(0.f, -0.8f, 0.8f);
    int got = finish_coverage();
    glDisable(GL_LINE_STIPPLE);
    if (got < 100 || got > 10000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: stipple coverage %d\n", got);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " stipple=%d", got);
}

static void test_client_loop(void)
{
    static const GLfloat verts[] = {
        -0.65f, -0.45f, 0.f, 1.f,
         0.65f, -0.45f, 0.f, 1.f,
         0.65f,  0.45f, 0.f, 1.f,
        -0.65f,  0.45f, 0.f, 1.f
    };
    clear_scene();
    glLineWidth(4.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, verts);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    int loop = finish_coverage();

    clear_scene();
    glLineWidth(4.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, verts);
    glDrawArrays(GL_LINE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    int strip = finish_coverage();
    int got = loop;
    if (got < 1000 || loop - strip < 300) {
        ++g_failures;
        fprintf(stderr, "line-width cases: client loop=%d strip=%d\n",
                loop, strip);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " client-loop=%d/%d", loop, strip);
}

static void test_instancing(void)
{
    static const GLfloat verts[] = {
        -0.8f, 0.f, 0.f, 1.f,
         0.8f, 0.f, 0.f, 1.f
    };
    unsigned char pixel[4] = {0, 0, 0, 0};
    clear_scene();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor3f(0.2f, 0.2f, 0.2f);
    glLineWidth(4.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, verts);
    glDrawArraysInstanced(GL_LINES, 0, 2, 2);
    glDisableClientState(GL_VERTEX_ARRAY);
    glFinish();
    glReadPixels(BENCH_WIDTH / 2, BENCH_HEIGHT / 2, 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixel);
    int twice = pixel[0];

    clear_scene();
    glLineWidth(4.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, verts);
    glDrawArraysInstanced(GL_LINES, 0, 2, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    int zero = finish_coverage();
    glDisable(GL_BLEND);
    if (twice < 70 || zero != 0) {
        ++g_failures;
        fprintf(stderr, "line-width cases: instances twice=%d zero=%d\n",
                twice, zero);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " instances=%d/%d", twice, zero);
}

static void emit_triangle(void)
{
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.7f, -0.6f, 0.f);
    glVertex3f( 0.7f, -0.6f, 0.f);
    glVertex3f( 0.f,   0.7f, 0.f);
    glEnd();
}

static void emit_list_line(void)
{
    horizontal_line(0.f, -0.8f, 0.8f);
}

static int call_wide_list(GLuint list)
{
    clear_scene();
    glLineWidth(1.f);
    glCallList(list);
    return finish_coverage();
}

static void test_named_lists(void)
{
    GLuint same = glGenLists(1);
    glLineWidth(4.f);
    glNewList(same, GL_COMPILE);
    glLineWidth(4.f);
    emit_list_line();
    glEndList();
    int same_coverage = call_wide_list(same);
    glDeleteLists(same, 1);

    GLuint source = glGenLists(1);
    glNewList(source, GL_COMPILE);
    glLineWidth(4.f);
    emit_list_line();
    glEndList();
    GLuint caller = glGenLists(1);
    glNewList(caller, GL_COMPILE);
    glCallList(source);
    glEndList();
    glDeleteLists(source, 1);
    int copied_coverage = call_wide_list(caller);
    glDeleteLists(caller, 1);

    GLuint unstamped = glGenLists(1);
    glNewList(unstamped, GL_COMPILE);
    emit_list_line();
    glEndList();
    clear_scene();
    glLineWidth(4.f);
    glCallList(unstamped);
    int unstamped_coverage = finish_coverage();
    glDeleteLists(unstamped, 1);

    GLuint mixed = glGenLists(1);
    glNewList(mixed, GL_COMPILE);
    glLineWidth(1.f);
    glBegin(GL_LINES);
    glVertex3f(-0.8f, -0.5f, 0.f);
    glVertex3f( 0.8f, -0.5f, 0.f);
    glEnd();
    glLineWidth(6.f);
    glBegin(GL_LINES);
    glVertex3f(-0.8f, 0.5f, 0.f);
    glVertex3f( 0.8f, 0.5f, 0.f);
    glEnd();
    glEndList();
    int mixed_coverage = call_wide_list(mixed);
    glDeleteLists(mixed, 1);

    if (same_coverage < 1200 || copied_coverage < 1200 ||
            mixed_coverage < 3000 || unstamped_coverage < 1200) {
        ++g_failures;
        fprintf(stderr, "line-width cases: lists same=%d copied=%d mixed=%d "
                "unstamped=%d\n", same_coverage, copied_coverage,
                mixed_coverage, unstamped_coverage);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " lists=%d/%d/%d/%d", same_coverage, copied_coverage,
             mixed_coverage, unstamped_coverage);
}

static int polygon_list_coverage(GLuint list)
{
    clear_scene();
    glLineWidth(1.f);
    glCallList(list);
    int got = finish_coverage();
    clear_scene();
    glCallList(list);
    int again = finish_coverage();
    return got < again ? got : again;
}

static void test_polygon_line_list(void)
{
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(4.f);
    emit_triangle();
    glEndList();
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    int got = polygon_list_coverage(list);
    glDeleteLists(list, 1);
    if (got < 1000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: polygon-list coverage %d\n", got);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " poly-list=%d", got);
}

static void test_fill_offset_isolation(void)
{
    clear_scene();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glPolygonOffset(0.f, -100000.f);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glColor3f(0.2f, 0.2f, 0.2f);
    glBegin(GL_QUADS);
    glVertex3f(-0.8f, -0.2f, 0.f);
    glVertex3f( 0.8f, -0.2f, 0.f);
    glVertex3f( 0.8f,  0.2f, 0.f);
    glVertex3f(-0.8f,  0.2f, 0.f);
    glEnd();
    glColor3f(1.f, 1.f, 1.f);
    glLineWidth(4.f);
    horizontal_line(0.f, -0.8f, 0.8f);
    int got = finish_coverage();
    int bright = bright_coverage();
    if (bright > 100) {
        ++g_failures;
        fprintf(stderr, "line-width cases: fill-offset leaked, bright %d\n",
                bright);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " fill=%d/%d", got, bright);
}

static void test_polygon_offset_line(void)
{
    clear_scene();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColor3f(0.2f, 0.2f, 0.2f);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.7f, -0.6f, 0.f);
    glVertex3f( 0.7f, -0.6f, 0.f);
    glVertex3f( 0.f,   0.7f, 0.f);
    glEnd();
    glColor3f(1.f, 1.f, 1.f);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(0.1f, -100000.f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(1.f);
    emit_triangle();
    int got = finish_coverage();
    int bright = bright_coverage();
    if (bright < 100) {
        ++g_failures;
        fprintf(stderr, "line-width cases: polygon-offset-line bright %d\n",
                bright);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " offset-line=%d/%d", got, bright);
}

static void test_known_deviations(void)
{
    clear_scene();
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogf(GL_FOG_DENSITY, 20.f);
    glColor3f(1.f, 1.f, 1.f);
    glLineWidth(6.f);
    horizontal_line(0.f, -0.8f, 0.8f);
    int fog_coverage = finish_coverage();

    clear_scene();
    glEnable(GL_CLIP_PLANE0);
    GLdouble plane[] = {1.0, 0.0, 0.0, 0.0};
    glClipPlane(GL_CLIP_PLANE0, plane);
    glLineWidth(6.f);
    horizontal_line(0.f, -0.8f, 0.8f);
    int clip_coverage = finish_coverage();
    glDisable(GL_CLIP_PLANE0);
    if (fog_coverage < 100 || clip_coverage < 1 || clip_coverage > 2000) {
        ++g_failures;
        fprintf(stderr, "line-width cases: deviations fog=%d clip=%d\n",
                fog_coverage, clip_coverage);
    }
    snprintf(g_report + strlen(g_report), sizeof(g_report) - strlen(g_report),
             " fog=%d clip=%d", fog_coverage, clip_coverage);
}

static void display(void)
{
    if (g_frame == 0) {
        test_trailing_reset();
        test_culling();
        test_near_plane();
        test_stipple();
        test_client_loop();
        test_instancing();
        test_named_lists();
        test_polygon_line_list();
        test_fill_offset_isolation();
        test_polygon_offset_line();
        test_known_deviations();
        printf("line-width cases:%s failures=%d\n", g_report, g_failures);
        EM_ASM({
            document.title = (UTF8ToString($0)) + " " + $1;
        }, g_failures ? "FAIL line-width cases" : "line-width cases",
           g_report);
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
    glutCreateWindow("gl4es line-width cases");
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glViewport(0, 0, BENCH_WIDTH, BENCH_HEIGHT);
    restore_flat_projection();
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutMainLoop();
    return 0;
}
