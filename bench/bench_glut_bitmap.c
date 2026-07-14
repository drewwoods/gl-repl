/*
 * Isolated GLUT bitmap-font benchmark.
 *
 * Build this translation unit twice: once against Apple GLUT and once against
 * freeglut.  Buffer swaps are deliberately absent; each sample starts with an
 * empty GL queue and reports both CPU submission time and time through the
 * final glFinish().
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(GLUT_BITMAP_BENCH_APPLE)
#include <OpenGL/gl.h>
#include <GLUT/glut.h>
#define BENCH_BACKEND "Apple GLUT"
#else
#include <GL/freeglut.h>
#define BENCH_BACKEND "freeglut"
#endif

enum {
    BENCH_DEFAULT_SAMPLES = 9,
    BENCH_DEFAULT_REPEATS = 100,
    BENCH_WARMUP_REPEATS = 4,
    BENCH_WINDOW_WIDTH = 800,
    BENCH_WINDOW_HEIGHT = 600,
    BENCH_MAX_SAMPLES = 101
};

typedef struct {
    double submit_seconds;
    double complete_seconds;
} BitmapSample;

typedef void (*DrawLineFn)(const char *text);

static const char *g_lines[] = {
    "void display(void) {",
    "  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "  glLoadIdentity();",
    "  glPushAttrib(GL_ALL_ATTRIB_BITS);",
    "  glEnable(GL_MULTISAMPLE);",
    "  // --- Camera ----------------------------------------",
    "  glTranslatef(0.0000f, 0.0000f, -6.5000f);",
    "  glRotatef(35.2500f, 1.0f, 0.0f, 0.0f);",
    "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);",
    "  glLightfv(GL_LIGHT1, GL_POSITION, light_position);",
    "  glutSolidTeapot(1.0);",
    "}"
};

static int g_samples = BENCH_DEFAULT_SAMPLES;
static int g_repeats = BENCH_DEFAULT_REPEATS;
static size_t g_glyphs_per_pass;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}

static int compare_double(const void *a, const void *b)
{
    const double da = *(const double *)a;
    const double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median(double *values, int count)
{
    qsort(values, (size_t)count, sizeof(values[0]), compare_double);
    return values[count / 2];
}

static int parse_positive_int(const char *option, const char *text, int limit)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < 1 || value > limit) {
        fprintf(stderr, "%s expects an integer from 1 through %d\n", option,
                limit);
        exit(2);
    }
    return (int)value;
}

static void parse_options(int *argc, char **argv)
{
    int read_index;
    int write_index = 1;

    for (read_index = 1; read_index < *argc; ++read_index) {
        if (strcmp(argv[read_index], "--samples") == 0) {
            if (read_index + 1 >= *argc) {
                fprintf(stderr, "--samples requires a value\n");
                exit(2);
            }
            g_samples = parse_positive_int("--samples", argv[++read_index],
                                           BENCH_MAX_SAMPLES);
        } else if (strcmp(argv[read_index], "--repeats") == 0) {
            if (read_index + 1 >= *argc) {
                fprintf(stderr, "--repeats requires a value\n");
                exit(2);
            }
            g_repeats = parse_positive_int("--repeats", argv[++read_index],
                                           10000);
        } else {
            argv[write_index++] = argv[read_index];
        }
    }
    *argc = write_index;
}

static void draw_line_characters(const char *text)
{
    while (*text)
        glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*text++);
}

#if !defined(GLUT_BITMAP_BENCH_APPLE)
static void draw_line_string(const char *text)
{
    glutBitmapString(GLUT_BITMAP_9_BY_15, (const unsigned char *)text);
}
#endif

static void draw_workload(DrawLineFn draw_line, int repeats)
{
    const size_t line_count = sizeof(g_lines) / sizeof(g_lines[0]);
    int repeat;
    size_t line;

    for (repeat = 0; repeat < repeats; ++repeat) {
        for (line = 0; line < line_count; ++line) {
            glRasterPos2i(8, 18 + (int)line * 18);
            draw_line(g_lines[line]);
        }
    }
}

static BitmapSample take_sample(DrawLineFn draw_line)
{
    BitmapSample sample;
    double start;

    glFinish();
    start = now_seconds();
    draw_workload(draw_line, g_repeats);
    sample.submit_seconds = now_seconds() - start;
    glFinish();
    sample.complete_seconds = now_seconds() - start;
    return sample;
}

static void report_case(const char *name, DrawLineFn draw_line)
{
    BitmapSample samples[BENCH_MAX_SAMPLES];
    double submit[BENCH_MAX_SAMPLES];
    double complete[BENCH_MAX_SAMPLES];
    const double glyph_count = (double)g_glyphs_per_pass * (double)g_repeats;
    double submit_median;
    double complete_median;
    int i;

    draw_workload(draw_line, BENCH_WARMUP_REPEATS);
    glFinish();
    for (i = 0; i < g_samples; ++i)
        samples[i] = take_sample(draw_line);
    for (i = 0; i < g_samples; ++i) {
        submit[i] = samples[i].submit_seconds;
        complete[i] = samples[i].complete_seconds;
    }

    submit_median = median(submit, g_samples);
    complete_median = median(complete, g_samples);
    printf("%-20s %10.3f %12.1f %12.3f %12.1f\n", name,
           submit_median * 1000.0, submit_median * 1.0e9 / glyph_count,
           complete_median * 1000.0, complete_median * 1.0e9 / glyph_count);
}

static void benchmark_display(void)
{
    const GLubyte *renderer;
    const GLubyte *version;
    const size_t line_count = sizeof(g_lines) / sizeof(g_lines[0]);
    size_t line;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (double)BENCH_WINDOW_WIDTH, 0.0,
            (double)BENCH_WINDOW_HEIGHT, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(1.0f, 1.0f, 1.0f);

    g_glyphs_per_pass = 0;
    for (line = 0; line < line_count; ++line)
        g_glyphs_per_pass += strlen(g_lines[line]);

    renderer = glGetString(GL_RENDERER);
    version = glGetString(GL_VERSION);
    printf("backend: %s\n", BENCH_BACKEND);
    printf("renderer: %s\n", renderer ? (const char *)renderer : "unknown");
    printf("OpenGL: %s\n", version ? (const char *)version : "unknown");
    printf("font: GLUT_BITMAP_9_BY_15\n");
    printf("workload: %lu lines, %lu glyphs/pass, %d repeats, %d samples\n",
           (unsigned long)line_count, (unsigned long)g_glyphs_per_pass,
           g_repeats, g_samples);
    printf("\n%-20s %10s %12s %12s %12s\n", "API", "submit ms",
           "submit ns/g", "complete ms", "complete ns/g");
    report_case("character loop", draw_line_characters);
#if defined(GLUT_BITMAP_BENCH_APPLE)
    printf("%-20s %s\n", "glutBitmapString",
           "unavailable (not exported by Apple GLUT)");
#else
    report_case("glutBitmapString", draw_line_string);
#endif
    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv)
{
    parse_options(&argc, argv);
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_SINGLE | GLUT_RGB);
    glutInitWindowSize(BENCH_WINDOW_WIDTH, BENCH_WINDOW_HEIGHT);
    glutCreateWindow("GLUT bitmap benchmark");
    glutDisplayFunc(benchmark_display);
    glutMainLoop();
    return 0;
}
