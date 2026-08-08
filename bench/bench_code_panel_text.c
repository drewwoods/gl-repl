/*
 * Code-panel text-submission benchmark (Mesa / Linux freeglut).
 *
 * This drives the *real* renderer: it links src/ui/core/text_panel.c and calls
 * ui_text_panel_render() with a hand-built snapshot, exactly as the REPL
 * adapter (src/ui/app/repl_code_panel.c) does each frame. Nothing about the
 * drawing is reimplemented here, so the bench cannot drift from the code it
 * measures - a change to text_panel_draw_segment / _draw_colored_span shows up
 * in these numbers automatically. That is possible because text_panel.c is a
 * REPL-free ui/core TU (guard: check-ui-text-panel-pure), so it links against
 * {text_layout, text_search, theme, cpuprof} and nothing else - the same
 * dependency set the standalone editor_demo uses.
 *
 * bench_glut_bitmap.c answers "glutBitmapCharacter vs glutBitmapString for a
 * whole line" against a private copy of the draw loop. This one answers the
 * question the code panel actually poses: syntax highlighting does not change
 * the glyph count, it changes how many *draw calls* those glyphs are split
 * across. A highlighted row is drawn as a run of colored spans, and each span
 * costs one glColor + one glRasterPos2f + one bitmap submission.
 *
 * On Linux, freeglut brackets every glutBitmapCharacter and every
 * glutBitmapString call with glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT) /
 * glPopClientAttrib plus six glPixelStorei calls (fg_font.c; the cheaper
 * glGetIntegerv path is macOS-only). The measured result is that this fixed
 * prologue is *not* the dominant term - see the "no color" cases below.
 *
 * Cases, all rendering the identical rows and glyphs, varying only how many
 * color segments each row carries:
 *
 *   plain            - no color segments: one span per row, the panel's
 *                      unhighlighted path (one glutBitmapString per row)
 *   sparse-N         - the realistic shape: N-char colored token segments with
 *                      uncovered gaps between them that the renderer fills in
 *                      row->color, exactly what repl_code_panel.c emits
 *   dense-N          - contiguous segments, no gap spans: pure fragmentation
 *   dense-N mono     - the same span count, but every span carries the same
 *                      color, which isolates raster/pixel-store cost from GL
 *                      color-change cost
 *
 * The mono rows are the result: identical span counts cost the same as an
 * unsegmented row (~38ms vs ~36ms at 127 spans) when the color never changes,
 * against ~233ms when it does. Fragmentation is nearly free; the per-span
 * color changes are the whole effect.
 *
 * Worth recording because it was the first thing tried and it does not work:
 * skipping *redundant* glColor calls buys nothing, because the gap spans mean
 * consecutive spans never share a color. Driving the real classifier
 * (ui_repl_code_panel_classify_syntax) over a dozen representative rows gives
 * 98 spans and 0 redundant sets - a 0% hit rate. Any real fix has to reduce
 * the number of color *changes*, not filter repeats: e.g. batching spans by
 * color across a row, or drawing the row once in the base color and
 * overdrawing only the colored tokens.
 *
 * Lighting is not configured here on purpose: ui_text_panel_render() brackets
 * itself with gl2d_begin()/gl2d_end(), which glDisable(GL_LIGHTING) under a
 * GL_LIGHTING_BIT glPushAttrib. Measuring through the real entry point means
 * the bench inherits that setup rather than guessing at it.
 *
 * Buffer swaps are deliberately absent; each sample starts from an empty GL
 * queue and reports both CPU submission time and time through glFinish().
 */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C99, so a bare -std=c99 compile
 * hides them without this. The Makefile recipe also passes -D_GNU_SOURCE; this
 * keeps the TU compilable on its own. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gl_includes.h"
#include "ui/core/gl_2d.h"      /* gl2d_begin/_end - lighting-state check */
#include "ui/core/text_panel.h"

enum {
    BENCH_DEFAULT_SAMPLES = 9,
    BENCH_DEFAULT_REPEATS = 100,
    BENCH_WARMUP_REPEATS = 4,
    BENCH_WINDOW_WIDTH = 800,
    BENCH_WINDOW_HEIGHT = 600,
    BENCH_MAX_SAMPLES = 101,
    BENCH_PANEL_MARGIN = 8
};

typedef struct {
    double submit_seconds;
    double complete_seconds;
} BitmapSample;

/* A representative default-view code panel: the kind of rows the REPL shows
 * with highlighting on. */
static const char *g_lines[] = {
    "// --- Camera ----------------------------------------",
    "glClearColor(0.05, 0.06, 0.09, 1.0);",
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
    "glTranslatef(0.0000, 0.0000, -6.5000);",
    "glRotatef(35.2500, 1.0, 0.0, 0.0);",
    "float phase, radius;",
    "phase = sin(t * TAU) * 0.5 + 0.5;",
    "for (i, 0, 64) {",
    "  radius = lerp(0.5, 1.5, smoothstep(0.0, 1.0, phase));",
    "  glColor3f(phase, 1.0 - phase, 0.75);",
    "  glVertex3f(cos(i * 0.1) * radius, sin(i * 0.1) * radius, 0.0);",
    "}",
    "glutSolidTeapot(1.0);",
    "label(\"phase=%.3f\", phase);"
};

#define BENCH_LINE_COUNT ((int)(sizeof(g_lines) / sizeof(g_lines[0])))

static int g_samples = BENCH_DEFAULT_SAMPLES;
static int g_repeats = BENCH_DEFAULT_REPEATS;
static size_t g_glyphs_per_pass;

/* Rows rebuilt per case; the snapshot points at this array. */
static UiTextPanelRow g_rows[BENCH_LINE_COUNT];
static UiTextPanelSnapshot g_snap;

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

/* --- snapshot construction -------------------------------------------- */

static UiTextPanelColor bench_color(float r, float g, float b)
{
    UiTextPanelColor c;
    c.r = r; c.g = g; c.b = b; c.a = 1.0f; c.has_alpha = 0;
    return c;
}

/* Build the row array for one case.
 *
 * segment_chars == 0 leaves the rows unsegmented (the unhighlighted panel).
 *
 * Otherwise every row is cut into segment_chars-wide color segments. Real
 * syntax spans from repl_code_panel.c are *sparse and disjoint* - they cover
 * the tokens and leave the punctuation/whitespace between them uncovered, and
 * the renderer fills each of those gaps with row->color. So a highlighted row
 * actually submits segment, gap, segment, gap, ... and every one of those gap
 * spans re-sends the identical row color. `sparse` reproduces that by leaving
 * every other segment slot uncovered; without it the segments are contiguous
 * and no gap spans exist, which understates the repeat rate badly.
 *
 * vary_color picks whether consecutive *segments* differ, which separates GL
 * color-change cost from the per-span raster cost.
 *
 * Note what the sparse shape rules out: because a gap always sits between two
 * segments, consecutive spans never carry the same color, so filtering
 * redundant glColor calls has nothing to skip. Measured against the real
 * classifier over a dozen representative rows, the redundant-set rate is 0%
 * (see this file's header). A "skip the repeat" cache is therefore not a fix
 * here - the color changes are all genuine. */
static void build_rows(int segment_chars, int vary_color, int sparse)
{
    /* Token-ish colors, so the driver sees the same kind of color churn a
     * highlighted row produces. */
    static const float k_colors[4][3] = {
        { 0.85f, 0.85f, 0.90f }, { 0.55f, 0.78f, 1.00f },
        { 0.95f, 0.72f, 0.45f }, { 0.60f, 0.85f, 0.60f }
    };
    int i;

    memset(g_rows, 0, sizeof(g_rows));

    for (i = 0; i < BENCH_LINE_COUNT; i++) {
        UiTextPanelRow *row = &g_rows[i];
        int len = (int)strlen(g_lines[i]);
        int start;
        int color_index = 0;

        row->text = g_lines[i];
        row->kind = UI_TEXT_PANEL_ROW_TEXT;
        row->source_line_idx = i;
        row->hit_target_line_idx = -1;
        row->search_row_idx = i;
        row->hit_eligible = 1;
        row->color = bench_color(0.85f, 0.85f, 0.90f);

        if (segment_chars <= 0)
            continue;

        for (start = 0; start < len; start += segment_chars) {
            int n = len - start;
            const float *c;

            if (row->color_segment_count >= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
                break;
            if (n > segment_chars)
                n = segment_chars;

            /* Leave every other slot uncovered so the renderer emits a
             * row->color gap span between segments, as a real token stream
             * does. */
            if (sparse && ((start / segment_chars) & 1))
                continue;

            c = k_colors[vary_color ? (color_index++ & 3) : 1];
            row->color_segments[row->color_segment_count].char_start = start;
            row->color_segments[row->color_segment_count].char_count = n;
            row->color_segments[row->color_segment_count].color =
                bench_color(c[0], c[1], c[2]);
            row->color_segments[row->color_segment_count].shadow = 0;
            row->color_segment_count++;
        }
    }
}

/* Spans the renderer will actually submit per pass, so the report can price a
 * span as well as a glyph. This mirrors text_panel_draw_colored_text(): each
 * segment is one span, and each uncovered gap between (or after) segments is
 * another span drawn in row->color. An unsegmented row submits exactly one. */
static size_t spans_per_pass(void)
{
    size_t total = 0;
    int i;

    for (i = 0; i < BENCH_LINE_COUNT; i++) {
        const UiTextPanelRow *row = &g_rows[i];
        int len = (int)strlen(row->text);
        int cursor = 0;
        int s;

        if (row->color_segment_count <= 0) {
            total += 1u;
            continue;
        }

        for (s = 0; s < row->color_segment_count; s++) {
            int seg_start = row->color_segments[s].char_start;

            if (cursor < seg_start)
                total++;               /* gap span in row->color */
            total++;                   /* the segment itself */
            cursor = seg_start + row->color_segments[s].char_count;
        }
        if (cursor < len)
            total++;                   /* trailing gap span */
    }
    return total;
}

static void build_snapshot(void)
{
    memset(&g_snap, 0, sizeof(g_snap));

    g_snap.vp_w = BENCH_WINDOW_WIDTH;
    g_snap.vp_h = BENCH_WINDOW_HEIGHT;
    g_snap.cp_x = BENCH_PANEL_MARGIN;
    g_snap.cp_y = BENCH_PANEL_MARGIN;
    g_snap.cp_w = BENCH_WINDOW_WIDTH - 2 * BENCH_PANEL_MARGIN;
    g_snap.cp_h = BENCH_WINDOW_HEIGHT - 2 * BENCH_PANEL_MARGIN;
    g_snap.text_x = 40;
    g_snap.rows = g_rows;
    g_snap.row_count = BENCH_LINE_COUNT;
    g_snap.scroll = 0;
    /* No statusbar/scrollbar/gutter chrome: this bench is about row text, and
     * chrome would add a fixed per-frame cost to every case alike. */
    g_snap.chrome_flags = 0;
    g_snap.input.input = "";
    g_snap.input.input_len = 0;
    g_snap.input.cursor = 0;
}

/* --- timing ------------------------------------------------------------ */

static void draw_workload(int repeats)
{
    UiTextPanelOutput out;
    int repeat;

    for (repeat = 0; repeat < repeats; ++repeat) {
        memset(&out, 0, sizeof(out));
        ui_text_panel_render(&g_snap, &out);
    }
}

static BitmapSample take_sample(void)
{
    BitmapSample sample;
    double start;

    glFinish();
    start = now_seconds();
    draw_workload(g_repeats);
    sample.submit_seconds = now_seconds() - start;
    glFinish();
    sample.complete_seconds = now_seconds() - start;
    return sample;
}

static void report_case(const char *name, int segment_chars, int vary_color,
                        int sparse)
{
    BitmapSample samples[BENCH_MAX_SAMPLES];
    double submit[BENCH_MAX_SAMPLES];
    double complete[BENCH_MAX_SAMPLES];
    double glyph_count;
    double span_count;
    size_t spans;
    double submit_median;
    double complete_median;
    int i;

    build_rows(segment_chars, vary_color, sparse);
    spans = spans_per_pass();

    glyph_count = (double)g_glyphs_per_pass * (double)g_repeats;
    span_count = (double)spans * (double)g_repeats;

    draw_workload(BENCH_WARMUP_REPEATS);
    glFinish();
    for (i = 0; i < g_samples; ++i)
        samples[i] = take_sample();
    for (i = 0; i < g_samples; ++i) {
        submit[i] = samples[i].submit_seconds;
        complete[i] = samples[i].complete_seconds;
    }

    submit_median = median(submit, g_samples);
    complete_median = median(complete, g_samples);

    printf("%-22s %6lu %10.3f %10.1f %10.1f %12.3f\n", name,
           (unsigned long)spans,
           submit_median * 1000.0,
           submit_median * 1.0e9 / glyph_count,
           submit_median * 1.0e9 / span_count,
           complete_median * 1000.0);
}

/* Confirm lighting is off for the glyph draws and restored afterwards.
 *
 * The panel brackets itself with gl2d_begin()/gl2d_end() (src/ui/core/gl_2d.h),
 * which glDisable(GL_LIGHTING) under a GL_LIGHTING_BIT glPushAttrib. Sampling
 * GL_LIGHTING around ui_text_panel_render() only shows the restore, so the
 * "during" reading comes from gl2d_begin() itself: entering the same bracket
 * the renderer enters and reading the state the glyph submissions run under.
 * Both are reported with lighting deliberately enabled beforehand, so a
 * regression that dropped the disable would show during=1. */
static void report_lighting_state(void)
{
    GLboolean during = GL_TRUE, after = GL_FALSE;
    UiTextPanelOutput out;

    glEnable(GL_LIGHTING);

    /* Same bracket the renderer opens; read the state the row text draws in. */
    gl2d_begin(g_snap.vp_w, g_snap.vp_h);
    glGetBooleanv(GL_LIGHTING, &during);
    gl2d_end();

    memset(&out, 0, sizeof(out));
    ui_text_panel_render(&g_snap, &out);
    glGetBooleanv(GL_LIGHTING, &after);

    glDisable(GL_LIGHTING);

    printf("lighting: during panel pass=%s, caller state restored after=%s\n",
           during ? "ENABLED (unexpected)" : "disabled",
           after ? "yes" : "NO (unexpected)");
}

static void benchmark_display(void)
{
    const GLubyte *renderer;
    const GLubyte *version;
    int i;

    g_glyphs_per_pass = 0;
    for (i = 0; i < BENCH_LINE_COUNT; i++)
        g_glyphs_per_pass += strlen(g_lines[i]);

    build_rows(0, 0, 0);
    build_snapshot();

    renderer = glGetString(GL_RENDERER);
    version = glGetString(GL_VERSION);
    printf("renderer: %s\n", renderer ? (const char *)renderer : "unknown");
    printf("OpenGL: %s\n", version ? (const char *)version : "unknown");
    printf("path: ui_text_panel_render() (src/ui/core/text_panel.c)\n");
    printf("workload: %d rows, %lu glyphs/pass, %d repeats, %d samples\n",
           BENCH_LINE_COUNT, (unsigned long)g_glyphs_per_pass,
           g_repeats, g_samples);
    report_lighting_state();

    printf("\n%-22s %6s %10s %10s %10s %12s\n", "case", "spans",
           "submit ms", "ns/glyph", "ns/span", "complete ms");

    /* "sparse" is the realistic shape: tokens colored, gaps between them
     * falling back to row->color, which is what a real highlighted row
     * submits. The dense variants keep the segments contiguous (no gap
     * spans) to show the cost of pure fragmentation on its own. */
    report_case("plain (no segments)", 0, 0, 0);
    report_case("sparse-8 (realistic)", 8, 1, 1);
    report_case("sparse-4 (realistic)", 4, 1, 1);
    report_case("dense-8", 8, 1, 0);
    report_case("dense-4", 4, 1, 0);
    report_case("dense-8 mono", 8, 0, 0);
    report_case("dense-4 mono", 4, 0, 0);

    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv)
{
    parse_options(&argc, argv);
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_SINGLE | GLUT_RGB);
    glutInitWindowSize(BENCH_WINDOW_WIDTH, BENCH_WINDOW_HEIGHT);
    glutCreateWindow("Code panel text benchmark");
    glutDisplayFunc(benchmark_display);
    glutMainLoop();
    return 0;
}
