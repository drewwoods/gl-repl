/*
 * Stencil-routed code-panel text benchmark (Linux/macOS vendored freeglut).
 *
 * bench_code_panel_text.c established the problem: a syntax-highlighted row is
 * submitted as a run of colored spans, and the per-span glColor is what costs -
 * 127 spans cost ~40ms in a single color against ~236ms in four, while the span
 * count itself is nearly free. The obvious fixes reduce the number of color
 * changes. This benchmark tests one of them.
 *
 * The idea under test: instead of interleaving glColor with the glyph draws,
 * rasterize *all* the glyphs once with color writes masked off, using the
 * stencil buffer to tag each glyph's pixels with a small integer identifying
 * its color. Then "back fill" - for each distinct color, set that color once
 * and draw a quad over the panel with the stencil test gated to that tag. The
 * glyph pass issues no color changes at all, and the fill pass issues exactly
 * one per distinct color rather than one per span.
 *
 * The trade is explicit and worth stating up front: this replaces N per-span
 * color changes with C full-quad fills, where C is the number of *distinct*
 * colors on screen. It is a win only if a stencil-gated fill is cheaper than
 * the color changes it removes - which is a question about overdraw and
 * stencil-test throughput, so it has to be measured, not reasoned about.
 *
 * RESULT: the technique loses badly on this driver, and the reason is a
 * correctness trap that a naive timing run reports as a 3.3x speedup.
 *
 * Mesa does not latch glStencilFunc per glBitmap. A run of
 *
 *     glStencilFunc(GL_ALWAYS, tag, 0xFF);  glRasterPos2i(...);
 *     glutBitmapString(...);
 *
 * with no flush between iterations ends with EVERY span tagged with the LAST
 * ref value - verified by reading the stencil buffer back: four adjacent spans
 * asking for tags 1,2,3,4 all come back as 4. The glyphs are therefore all
 * filled in one color, which is both wrong and much less work, and that is
 * where the apparent win came from. Forcing the per-span state to take effect
 * (glFlush after each span) restores correct output and costs 83.68 ms/frame
 * against 1.42 ms for the direct per-span glColor path - about 59x slower.
 *
 * The per-span flush is not the only way to make the tags land, though. If the
 * stencil ref is pinned at 0xFF with GL_REPLACE and each color owns one
 * stencil BITPLANE, the per-span value moves into glStencilMask - and spans
 * sharing a color then share a mask, so they can be grouped by color and the
 * stencil state changed (and flushed) once per color instead of once per span.
 * That is stencil/grouped, and it is correct: 0/67 mis-tagged, at 2.4x direct
 * rather than 23x. A real fix to the mechanism, but still a loss, and it caps
 * at GL_STENCIL_BITS distinct colors (8 here) where a real panel needs 12 - so
 * the technique cannot express the actual workload even at that price.
 *
 * The useful result is the control that grouping suggested: direct/grouped -
 * same span order regrouped by color, one glColor per color, no stencil at all
 * - runs at 0.78x with no caveats. That is where the win is.
 *
 * Why grouping helps far less than the span counts suggest: holding draw order
 * fixed and varying only the glColor pattern, the cost is a cliff between one
 * distinct color (0.40ms) and two (1.32ms), then flat out to 67 distinct
 * colors (1.41ms). A single color change mid-batch is free (0.37ms). So Mesa
 * is not charging per color change; a frame that changes color repeatedly
 * takes a slow path and the number of changes barely matters after that.
 * Batching therefore cannot reach the single-color floor - which is why
 * direct/grouped saves ~22% and not the ~3x the floor row hints at.
 *
 * This benchmark VERIFIES ITS OWN OUTPUT before reporting any timing - see
 * verify_stencil_tagging(). A performance number from a path that draws the
 * wrong pixels is worse than no number at all.
 *
 * Three cases, all producing identical pixels:
 *
 *   direct            - the current model: glColor per span, then the glyphs
 *   stencil/panel     - stencil-tag pass, then one panel-wide quad per color
 *   stencil/bbox      - stencil-tag pass, then one quad per color clipped to
 *                       that color's bounding box (most colors occupy a few
 *                       rows, so the panel-wide quad is mostly wasted fill)
 *   stencil/flushed   - stencil/bbox with the glFlush per span that Mesa needs
 *                       to actually apply the per-span tag. This is the only
 *                       stencil case that draws correct pixels, and it is the
 *                       one that matters for the verdict.
 *
 * The bench sweeps the distinct-color count, because that is the term the
 * stencil scheme scales in and the direct path does not. For calibration
 * against the real thing: driving the actual classifier over a representative
 * 12-row panel gives 44 token spans in 12 distinct colors, since a token's
 * color is a function of (syntax kind, command category) and the category is
 * per-row - so colors do not collapse across rows the way one might hope.
 *
 * This deliberately does NOT link ui_text_panel_render(): the whole point is
 * to draw the same glyphs a different way, which the current renderer has no
 * path for. It shares bench_code_panel_text.c's row corpus and timing harness
 * so the two are comparable, and its "direct" case is the control that keeps
 * it honest - if "direct" here does not track the real renderer's numbers, the
 * model is wrong and the comparison is meaningless.
 *
 * Buffer swaps are deliberately absent; each sample starts from an empty GL
 * queue and reports both CPU submission time and time through glFinish().
 */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C99. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <GL/freeglut.h>

enum {
    BENCH_DEFAULT_SAMPLES = 9,
    BENCH_DEFAULT_REPEATS = 100,
    BENCH_WARMUP_REPEATS = 4,
    BENCH_WINDOW_WIDTH = 800,
    BENCH_WINDOW_HEIGHT = 600,
    BENCH_MAX_SAMPLES = 101,
    BENCH_FONT_W = 9,
    BENCH_FONT_H = 15,
    BENCH_LINE_H = 18,
    BENCH_PANEL_X = 8,
    BENCH_PANEL_Y = 8,
    BENCH_SEGMENT_CHARS = 8,   /* token width; gaps are the same width */
    BENCH_MAX_COLORS = 32,
    BENCH_MAX_LINE = 256
};

#define BENCH_FONT GLUT_BITMAP_9_BY_15

typedef struct {
    double submit_seconds;
    double complete_seconds;
} BitmapSample;

/* Same corpus as bench_code_panel_text.c, so the two benchmarks are directly
 * comparable. */
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

/* One span of text drawn in a single color. The corpus is pre-flattened into
 * this form so every case walks the identical span list and only the drawing
 * strategy differs. */
typedef struct {
    int line;        /* index into g_lines */
    int start;       /* char offset of the span within the line */
    int len;         /* span length in chars */
    int color_id;    /* index into g_colors; also the stencil tag (1-based) */
} BenchSpan;

static int g_samples = BENCH_DEFAULT_SAMPLES;
static int g_repeats = BENCH_DEFAULT_REPEATS;
static int g_run_flushed = 0;  /* --flushed: include the slow glFlush-per-span case */

static BenchSpan g_spans[BENCH_LINE_COUNT * (BENCH_MAX_LINE / BENCH_SEGMENT_CHARS + 2)];
static int g_span_count;
static size_t g_glyphs_per_pass;

static float g_colors[BENCH_MAX_COLORS][3];
static int g_color_count;

/* Per-color bounding box in window pixels, for the bbox backfill case. */
typedef struct {
    int x0, y0, x1, y1;
    int used;
} BenchColorBox;
static BenchColorBox g_boxes[BENCH_MAX_COLORS];

static GLint g_stencil_bits;

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
        } else if (strcmp(argv[read_index], "--flushed") == 0) {
            g_run_flushed = 1;
        } else {
            argv[write_index++] = argv[read_index];
        }
    }
    *argc = write_index;
}

/* --- corpus construction ---------------------------------------------- */

static int span_x(const BenchSpan *s)
{
    return BENCH_PANEL_X + 20 + s->start * BENCH_FONT_W;
}

static int span_y(const BenchSpan *s)
{
    /* Rows run top-down from the panel's top edge. */
    return BENCH_WINDOW_HEIGHT - BENCH_PANEL_Y - BENCH_LINE_H
           - s->line * BENCH_LINE_H;
}

/* Build the span list and the palette for a given distinct-color count.
 *
 * Spans alternate token/gap the way a real highlighted row does: odd slots are
 * "gaps" and always take color 0 (the row color), even slots cycle through the
 * remaining colors. That keeps the span count fixed while color_count varies,
 * which is exactly the axis under test. */
static void build_corpus(int color_count)
{
    int i;

    if (color_count < 1)
        color_count = 1;
    if (color_count > BENCH_MAX_COLORS)
        color_count = BENCH_MAX_COLORS;
    g_color_count = color_count;

    for (i = 0; i < g_color_count; i++) {
        /* Spread hues so no two tags share a color; exact values are
         * irrelevant to cost, only the count is. */
        float f = (float)i / (float)g_color_count;
        g_colors[i][0] = 0.45f + 0.5f * f;
        g_colors[i][1] = 0.85f - 0.4f * f;
        g_colors[i][2] = 0.55f + 0.4f * (1.0f - f);
    }

    g_span_count = 0;
    g_glyphs_per_pass = 0;
    for (i = 0; i < BENCH_LINE_COUNT; i++) {
        int len = (int)strlen(g_lines[i]);
        int start;
        int slot = 0;

        g_glyphs_per_pass += (size_t)len;
        for (start = 0; start < len; start += BENCH_SEGMENT_CHARS, slot++) {
            BenchSpan *s = &g_spans[g_span_count++];
            int n = len - start;

            if (n > BENCH_SEGMENT_CHARS)
                n = BENCH_SEGMENT_CHARS;
            s->line = i;
            s->start = start;
            s->len = n;
            /* Gaps take the row color; tokens cycle the rest. */
            s->color_id = (slot & 1) || g_color_count == 1
                              ? 0
                              : 1 + (slot / 2) % (g_color_count - 1);
        }
    }

    /* Per-color bounding boxes for the bbox backfill. */
    for (i = 0; i < g_color_count; i++) {
        g_boxes[i].used = 0;
        g_boxes[i].x0 = g_boxes[i].y0 = 1 << 28;
        g_boxes[i].x1 = g_boxes[i].y1 = -(1 << 28);
    }
    for (i = 0; i < g_span_count; i++) {
        const BenchSpan *s = &g_spans[i];
        BenchColorBox *b = &g_boxes[s->color_id];
        int x0 = span_x(s);
        int x1 = x0 + s->len * BENCH_FONT_W;
        int y0 = span_y(s) - 3;
        int y1 = y0 + BENCH_FONT_H + 3;

        if (x0 < b->x0) b->x0 = x0;
        if (y0 < b->y0) b->y0 = y0;
        if (x1 > b->x1) b->x1 = x1;
        if (y1 > b->y1) b->y1 = y1;
        b->used = 1;
    }
}

static void draw_span_text(const BenchSpan *s)
{
    unsigned char buf[BENCH_MAX_LINE + 1];

    memcpy(buf, g_lines[s->line] + s->start, (size_t)s->len);
    buf[s->len] = '\0';
    glRasterPos2i(span_x(s), span_y(s));
    glutBitmapString(BENCH_FONT, buf);
}

/* --- case 1: direct (the current renderer's model) --------------------- */

static void draw_direct(void)
{
    int i;

    for (i = 0; i < g_span_count; i++) {
        const float *c = g_colors[g_spans[i].color_id];

        glColor3f(c[0], c[1], c[2]);
        draw_span_text(&g_spans[i]);
    }
}

/* --- cases 2/3: stencil-routed ---------------------------------------- */

/* Pass 1: rasterize every glyph with color writes off, stamping each span's
 * color id into the stencil buffer. One glColor is never issued.
 *
 * flush_per_span exists because Mesa does not apply a glStencilFunc change
 * between successive glBitmap calls unless the pipeline is flushed - without
 * it every span ends up carrying the last ref (see the header). Only the
 * flushed form produces correct pixels; the unflushed form is kept solely to
 * show what the incorrect-but-fast path measures. */
static void stencil_tag_pass(int flush_per_span)
{
    int i;

    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    for (i = 0; i < g_span_count; i++) {
        /* The tag is the color id + 1 so that 0 stays "no glyph". */
        glStencilFunc(GL_ALWAYS, g_spans[i].color_id + 1, 0xFF);
        draw_span_text(&g_spans[i]);
        if (flush_per_span)
            glFlush();
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* Pass 2: one quad per distinct color, gated to that color's stencil tag. */
static void stencil_fill_pass(int use_bbox)
{
    int i;

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    for (i = 0; i < g_color_count; i++) {
        int x0, y0, x1, y1;

        if (use_bbox) {
            if (!g_boxes[i].used)
                continue;
            x0 = g_boxes[i].x0; y0 = g_boxes[i].y0;
            x1 = g_boxes[i].x1; y1 = g_boxes[i].y1;
        } else {
            x0 = BENCH_PANEL_X;
            y0 = BENCH_PANEL_Y;
            x1 = BENCH_WINDOW_WIDTH - BENCH_PANEL_X;
            y1 = BENCH_WINDOW_HEIGHT - BENCH_PANEL_Y;
        }

        glStencilFunc(GL_EQUAL, i + 1, 0xFF);
        glColor3f(g_colors[i][0], g_colors[i][1], g_colors[i][2]);
        glBegin(GL_QUADS);
        glVertex2i(x0, y0);
        glVertex2i(x1, y0);
        glVertex2i(x1, y1);
        glVertex2i(x0, y1);
        glEnd();
    }
    glDisable(GL_STENCIL_TEST);
}

/* Grouped-bitplane variant.
 *
 * Instead of a per-span glStencilFunc ref, the ref is pinned at 0xFF with
 * GL_REPLACE and the per-span value comes from glStencilMask - each color owns
 * one stencil bitplane, so writing through mask (1 << c) sets only that
 * color's bit. The point is that spans sharing a color share a mask, so the
 * spans can be grouped by color and the stencil state changed once per color
 * rather than once per span. One flush per color then costs C flushes instead
 * of N, which is what makes this correct without the 25x per-span flush.
 *
 * Costs 8 bitplanes for 8 colors, so it caps at GL_STENCIL_BITS distinct
 * colors (8 here) - well under the 12 a real panel needs, which is a hard
 * limit on the technique quite apart from the timings. */
static void stencil_tag_pass_grouped(void)
{
    int c, i;

    glEnable(GL_STENCIL_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);   /* fixed; never changes */
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    for (c = 0; c < g_color_count && c < (int)g_stencil_bits; c++) {
        glStencilMask(1u << c);
        for (i = 0; i < g_span_count; i++)
            if (g_spans[i].color_id == c)
                draw_span_text(&g_spans[i]);
        /* Per color, not per span: this is the whole point of grouping. */
        glFlush();
    }

    glStencilMask(0xFF);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* Fill pass for the bitplane tagging: test the color's own bit. */
static void stencil_fill_pass_grouped(void)
{
    int c;

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    for (c = 0; c < g_color_count && c < (int)g_stencil_bits; c++) {
        if (!g_boxes[c].used)
            continue;
        glStencilFunc(GL_EQUAL, 1u << c, 1u << c);
        glColor3f(g_colors[c][0], g_colors[c][1], g_colors[c][2]);
        glBegin(GL_QUADS);
        glVertex2i(g_boxes[c].x0, g_boxes[c].y0);
        glVertex2i(g_boxes[c].x1, g_boxes[c].y0);
        glVertex2i(g_boxes[c].x1, g_boxes[c].y1);
        glVertex2i(g_boxes[c].x0, g_boxes[c].y1);
        glEnd();
    }
    glDisable(GL_STENCIL_TEST);
}

static void draw_stencil_grouped(void)
{
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    stencil_tag_pass_grouped();
    stencil_fill_pass_grouped();
}

/* Group the spans by color and issue one glColor per color - no stencil at
 * all. The obvious alternative to the whole stencil idea, included so the two
 * are measured side by side. */
static void draw_direct_grouped(void)
{
    int c, i;

    for (c = 0; c < g_color_count; c++) {
        glColor3f(g_colors[c][0], g_colors[c][1], g_colors[c][2]);
        for (i = 0; i < g_span_count; i++)
            if (g_spans[i].color_id == c)
                draw_span_text(&g_spans[i]);
    }
}

/* Floor: one color for the whole panel. Not a candidate implementation (it
 * throws the highlighting away) - it is the lower bound any color-batching
 * scheme is aiming at, and the gap to it is the prize on offer. */
static void draw_single_color(void)
{
    int i;

    glColor3f(g_colors[0][0], g_colors[0][1], g_colors[0][2]);
    for (i = 0; i < g_span_count; i++)
        draw_span_text(&g_spans[i]);
}

static void draw_stencil(int use_bbox, int flush_per_span)
{
    /* The tag pass needs a clean slate; this is part of the technique's cost
     * and is timed with it. */
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    stencil_tag_pass(flush_per_span);
    stencil_fill_pass(use_bbox);
}

/* Read the stencil buffer back and check each span actually carries its own
 * tag. Returns the number of spans whose tag is wrong.
 *
 * This is the guard that turns a misleading 3.3x into the real result: the
 * unflushed tag pass leaves every span holding the last ref written, which
 * draws the whole panel in one color. Sampling one interior pixel per span is
 * enough to catch that - a mis-tagged span is mis-tagged everywhere. */
static int verify_stencil_tagging(int flush_per_span)
{
    int bad = 0;
    int i;

    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    stencil_tag_pass(flush_per_span);
    glDisable(GL_STENCIL_TEST);
    glFinish();

    /* Default GL_PACK_ALIGNMENT is 4, which pads each returned row out to a
     * 4-byte boundary - a 1-pixel-wide read would then write 4 bytes per row
     * into a 1-byte-per-row buffer and overrun it. */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (i = 0; i < g_span_count; i++) {
        const BenchSpan *s = &g_spans[i];
        /* One row per glyph scanline. GL_PACK_ALIGNMENT is set to 1 below, so
         * rows are not padded and this is exactly BENCH_FONT_H bytes. */
        GLubyte column[BENCH_FONT_H];
        int want = s->color_id + 1;
        int x = span_x(s) + 1;          /* inside the first glyph cell */
        int hit = 0;
        int row;

        /* Scan the glyph's height for a tagged pixel: which rows a glyph
         * actually covers depends on the character, so take the first
         * non-zero and compare that. */
        glReadPixels(x, span_y(s), 1, BENCH_FONT_H, GL_STENCIL_INDEX,
                     GL_UNSIGNED_BYTE, column);
        for (row = 0; row < BENCH_FONT_H; row++) {
            if (column[row] != 0) {
                hit = column[row];
                break;
            }
        }
        if (hit != 0 && hit != want)
            bad++;
    }
    return bad;
}

/* Same check for the grouped bitplane tagging: each span's pixels must carry
 * its color's bit and no other. */
static int verify_stencil_tagging_grouped(void)
{
    int bad = 0;
    int i;

    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    stencil_tag_pass_grouped();
    glDisable(GL_STENCIL_TEST);
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (i = 0; i < g_span_count; i++) {
        const BenchSpan *s = &g_spans[i];
        GLubyte column[BENCH_FONT_H];
        int want;
        int x = span_x(s) + 1;
        int hit = 0;
        int row;

        if (s->color_id >= (int)g_stencil_bits)
            continue;               /* no bitplane for this color */
        want = 1 << s->color_id;

        glReadPixels(x, span_y(s), 1, BENCH_FONT_H, GL_STENCIL_INDEX,
                     GL_UNSIGNED_BYTE, column);
        for (row = 0; row < BENCH_FONT_H; row++) {
            if (column[row] != 0) {
                hit = column[row];
                break;
            }
        }
        if (hit != 0 && hit != want)
            bad++;
    }
    return bad;
}

/* --- timing ------------------------------------------------------------ */

typedef enum {
    CASE_DIRECT = 0,
    CASE_STENCIL_PANEL,
    CASE_STENCIL_BBOX,
    CASE_STENCIL_FLUSHED,
    CASE_STENCIL_GROUPED,
    CASE_DIRECT_GROUPED,
    CASE_SINGLE_COLOR
} BenchCase;

static void draw_workload(BenchCase which, int repeats)
{
    int r;

    for (r = 0; r < repeats; r++) {
        switch (which) {
        case CASE_DIRECT:          draw_direct();          break;
        case CASE_STENCIL_PANEL:   draw_stencil(0, 0);     break;
        case CASE_STENCIL_BBOX:    draw_stencil(1, 0);     break;
        case CASE_STENCIL_FLUSHED: draw_stencil(1, 1);     break;
        case CASE_STENCIL_GROUPED: draw_stencil_grouped(); break;
        case CASE_DIRECT_GROUPED:  draw_direct_grouped();  break;
        case CASE_SINGLE_COLOR:    draw_single_color();    break;
        }
    }
}

static BitmapSample take_sample(BenchCase which)
{
    BitmapSample sample;
    double start;

    glFinish();
    start = now_seconds();
    draw_workload(which, g_repeats);
    sample.submit_seconds = now_seconds() - start;
    glFinish();
    sample.complete_seconds = now_seconds() - start;
    return sample;
}

static double report_case(const char *name, BenchCase which, double baseline_ms,
                          const char *note)
{
    BitmapSample samples[BENCH_MAX_SAMPLES];
    double submit[BENCH_MAX_SAMPLES];
    double complete[BENCH_MAX_SAMPLES];
    double submit_median, complete_median;
    int i;

    draw_workload(which, BENCH_WARMUP_REPEATS);
    glFinish();
    for (i = 0; i < g_samples; ++i)
        samples[i] = take_sample(which);
    for (i = 0; i < g_samples; ++i) {
        submit[i] = samples[i].submit_seconds;
        complete[i] = samples[i].complete_seconds;
    }
    submit_median = median(submit, g_samples);
    complete_median = median(complete, g_samples);

    printf("  %-18s %10.3f %12.3f", name, submit_median * 1000.0,
           complete_median * 1000.0);
    if (baseline_ms > 0.0)
        printf("  %9.2fx", complete_median * 1000.0 / baseline_ms);
    else
        printf("  %10s", "-");
    printf("  %s\n", note ? note : "");
    return complete_median * 1000.0;
}

static void run_sweep(int color_count)
{
    double base;
    int bad_unflushed;
    int bad_flushed;
    int bad_grouped;
    int planes_ok;

    build_corpus(color_count);

    /* Correctness first: a timing number from a path that draws the wrong
     * pixels is not a result. */
    bad_unflushed = verify_stencil_tagging(0);
    if (g_run_flushed)
        bad_flushed = verify_stencil_tagging(1);
    else
        bad_flushed = 0;
    bad_grouped = verify_stencil_tagging_grouped();
    planes_ok = g_color_count <= (int)g_stencil_bits;

    char flushed_status[32];
    if (g_run_flushed)
        sprintf(flushed_status, "%d/%d", bad_flushed, g_span_count);
    else
        sprintf(flushed_status, "skipped");

    printf("\ncolors=%d  spans=%d  glyphs=%lu\n", g_color_count, g_span_count,
           (unsigned long)g_glyphs_per_pass);
    printf("  tag check: unflushed %d/%d mis-tagged, flushed %s,"
           " grouped %d/%d\n",
           bad_unflushed, g_span_count, flushed_status,
           bad_grouped, g_span_count);
    printf("  %-18s %10s %12s %10s  %s\n", "case", "submit ms", "complete ms",
           "vs direct", "output");
    base = report_case("direct", CASE_DIRECT, 0.0, "correct (today)");
    report_case("stencil/panel", CASE_STENCIL_PANEL, base,
                bad_unflushed ? "WRONG PIXELS" : "correct");
    report_case("stencil/bbox", CASE_STENCIL_BBOX, base,
                bad_unflushed ? "WRONG PIXELS" : "correct");
    if (g_run_flushed)
        report_case("stencil/flushed", CASE_STENCIL_FLUSHED, base,
                    bad_flushed ? "WRONG PIXELS" : "correct");
    report_case("stencil/grouped", CASE_STENCIL_GROUPED, base,
                !planes_ok ? "NEEDS > STENCIL_BITS PLANES"
                           : (bad_grouped ? "WRONG PIXELS" : "correct"));
    report_case("direct/grouped", CASE_DIRECT_GROUPED, base, "correct");
    report_case("single color (floor)", CASE_SINGLE_COLOR, base,
                "not a candidate - no highlighting");
}

static void benchmark_display(void)
{
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);

    glGetIntegerv(GL_STENCIL_BITS, &g_stencil_bits);

    printf("renderer: %s\n", renderer ? (const char *)renderer : "unknown");
    printf("OpenGL: %s\n", version ? (const char *)version : "unknown");
    printf("stencil bits: %d\n", (int)g_stencil_bits);
    if (g_stencil_bits <= 0) {
        printf("\nNo stencil planes in this visual - the stencil cases cannot "
               "run.\n");
        fflush(stdout);
        exit(1);
    }
    printf("font: GLUT_BITMAP_9_BY_15, %d rows, %d repeats, %d samples\n",
           BENCH_LINE_COUNT, g_repeats, g_samples);
    printf("note: a real 12-row panel measures 44 token spans in 12 distinct"
           " colors\n");

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (double)BENCH_WINDOW_WIDTH, 0.0,
            (double)BENCH_WINDOW_HEIGHT, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Sweep the distinct-color count: the term the stencil scheme pays per
     * frame and the direct path is indifferent to. */
    run_sweep(2);
    run_sweep(4);
    run_sweep(8);
    run_sweep(12);
    run_sweep(24);

    fflush(stdout);
    exit(0);
}

int main(int argc, char **argv)
{
    parse_options(&argc, argv);
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_SINGLE | GLUT_RGB | GLUT_STENCIL);
    glutInitWindowSize(BENCH_WINDOW_WIDTH, BENCH_WINDOW_HEIGHT);
    glutCreateWindow("Code panel stencil benchmark");
    glutDisplayFunc(benchmark_display);
    glutMainLoop();
    return 0;
}
