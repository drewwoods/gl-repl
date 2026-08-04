/*
 * tools/assign_plot_demo/assign_plot_demo.c - standalone driver for the
 * assignment-plot subsystem.
 *
 * Isolation proof: links ONLY the src/subsystems/assign_plot peer (capture +
 * statistics) + src/ui/support/assign_plot.c (the panel renderer) + runstats +
 * ui/core theme. No src/ui/app, no src/app, no src/repl, no src/editor - see
 * ASSIGN_PLOT_DEMO_DEP_SRCS in the Makefile and
 * check-assign-plot-demo-isolation.
 *
 * What it shows: a three-row "program" whose loop draws a wave, with the plot
 * open on its assignments. The demo runs that loop every frame and records
 * each assignment's value into an execution trace, then hands the trace to the
 * peer through an AssignPlotHostBridge - which is the seam that lets the
 * subsystem link with no flat program, no GLCmd and no src/repl behind it. The
 * curve on screen and the trace in the panel are literally the same numbers.
 *
 * The trace is deliberately *unfiltered*, the way the app's flat program is: a
 * fourth entry per iteration stands in for the `glVertex3f` between the
 * assignments, and demo_trace_at() returns 0 for it. That keeps trace indices
 * in the same space as the program counter below, which is what the real host
 * needs for its replay exec limit.
 *
 * The replay coupling has no replay subsystem to come from here, and does not
 * need one: assign_plot_exec_progress() takes a plain trace index. Pressing r
 * walks a program counter through the frame, clamps the drawn curve to it, and
 * feeds the same index to the peer - so the PC rules, the value dots and the
 * readouts run exactly as they do under Ctrl+R in the app.
 *
 * Keys: r scrub  [ ] step the PC  space pause  1..3 toggle a row  q quit.
 * Right-click a source row to retarget the plot, Shift+right-click to add or
 * remove it; every chip in the panel is live under the mouse.
 */
#include "gl_includes.h"

#include "subsystems/assign_plot/assign_plot.h"  /* peer: host bridge, capture */
#include "ui/support/assign_plot.h"              /* panel render + hit test    */
#include "ui/core/gl_2d.h"                       /* 2D text for the source rows */
#include "ui/core/theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Window state -------------------------------------------------------- */
static int   g_window_w = 900;
static int   g_window_h = 620;
static int   g_pointer_x = -1, g_pointer_y = -1;

/* --- The "program" ------------------------------------------------------- *
 *
 * Three assignment rows inside one loop, so all three share an execution count
 * and can therefore share the plot's X axis (a row with a different count is
 * refused at add time - that rule is the peer's, and this scene satisfies it).
 * DEMO_ROW_WAVE is the sum of the other two, which makes an overlay of all
 * three readable at a glance. */
enum {
    DEMO_ROW_BASE = 0,
    DEMO_ROW_RIPPLE,
    DEMO_ROW_WAVE,
    DEMO_ROW_COUNT
};

static const char *const k_row_names[DEMO_ROW_COUNT] = {
    "base", "ripple", "wave"
};

/* Shown in the demo's own source listing - the text a code panel would hold.
 * The peer never sees these: it addresses rows by index, and the titles are
 * the host's to supply. */
static const char *const k_row_source[DEMO_ROW_COUNT] = {
    "base   = sin(i * 0.05 + t) * 0.5;",
    "ripple = cos(i * 0.17 - t * 1.3) * 0.25;",
    "wave   = base + ripple;",
};

#define DEMO_ITERS 128   /* loop trip count - the plot's per-frame exec count */

/* One trace slot per assignment plus one for the vertex that follows them. */
enum { DEMO_TRACE_STRIDE = DEMO_ROW_COUNT + 1 };
enum { DEMO_TRACE_MAX = DEMO_ITERS * DEMO_TRACE_STRIDE };

/* Execution record of the last frame, in execution order. `row` is -1 for the
 * vertex entries, which are not assignments and are skipped by the bridge. */
typedef struct { int row; float value; } DemoTraceEntry;
static DemoTraceEntry g_trace[DEMO_TRACE_MAX];
static int   g_trace_len;
static float g_time;          /* the program's `t`                          */
static int   g_paused;

/* Run the program for one frame: evaluate every row of every iteration and
 * record it. This is the demo's whole "flatten + execute" - the values are
 * baked into the trace here, exactly as the app's flatten bakes them into the
 * flat command, so the peer never has to evaluate anything. */
static void demo_run_program(void) {
    g_trace_len = 0;
    for (int i = 0; i < DEMO_ITERS; i++) {
        float base   = sinf((float)i * 0.05f + g_time) * 0.5f;
        float ripple = cosf((float)i * 0.17f - g_time * 1.3f) * 0.25f;
        float wave   = base + ripple;

        g_trace[g_trace_len].row = DEMO_ROW_BASE;
        g_trace[g_trace_len++].value = base;
        g_trace[g_trace_len].row = DEMO_ROW_RIPPLE;
        g_trace[g_trace_len++].value = ripple;
        g_trace[g_trace_len].row = DEMO_ROW_WAVE;
        g_trace[g_trace_len++].value = wave;
        /* The vertex the loop body draws with those values. Not an assignment;
         * present so trace indices stay execution indices. */
        g_trace[g_trace_len].row = -1;
        g_trace[g_trace_len++].value = wave;
    }
}

/* --- Host bridge --------------------------------------------------------- *
 *
 * The three questions the peer asks of any host. In the app these are answered
 * against the flat program by src/app/glr_assign_plot_bridge.c; here they are
 * answered against the array above, and nothing else changes. */
static int demo_trace_len(void) {
    return g_trace_len;
}

static int demo_trace_at(int idx, AssignPlotSample *out) {
    if (idx < 0 || idx >= g_trace_len) return 0;
    if (g_trace[idx].row < 0) return 0;      /* the vertex entry */
    out->source_line_idx = g_trace[idx].row;
    out->value           = g_trace[idx].value;
    return 1;
}

static int demo_row_is_plottable(int row) {
    return row >= 0 && row < DEMO_ROW_COUNT;
}

static const AssignPlotHostBridge g_demo_host = {
    demo_trace_len, demo_trace_at, demo_row_is_plottable
};

/* --- Program counter ----------------------------------------------------- *
 *
 * Stands in for replay's exec limit: a trace index the frame is rendering up
 * to, negative for "the whole frame". The peer takes exactly that, so no
 * replay code is linked - and the curve is clamped to the same index, so what
 * is drawn and what the marker claims cannot disagree. */
static int g_scrub;             /* the PC is walking the program */
static int g_pc = DEMO_TRACE_STRIDE;

static int demo_exec_limit(void) {
    return g_scrub ? g_pc : -1;
}

static void demo_pc_step(int delta) {
    g_pc += delta * DEMO_TRACE_STRIDE;
    if (g_pc > DEMO_TRACE_MAX) g_pc = DEMO_TRACE_STRIDE;
    if (g_pc < DEMO_TRACE_STRIDE) g_pc = DEMO_TRACE_MAX;
}

/* --- The panel view ------------------------------------------------------ */
static void demo_panel_pos(int *out_x, int *out_y) {
    int pw, ph;
    ui_assign_plot_panel_size(assign_plot_is_expanded(),
                              assign_plot_series_count(), &pw, &ph);
    /* Standalone placement: pinned bottom-right. The app resolves this through
     * its overlay layout engine (src/ui/app/overlay_layout.c); the renderer
     * draws at whatever position the view carries either way. */
    *out_x = g_window_w - pw - 12;
    *out_y = 12;
}

static UiAssignPlotPanelView demo_build_view(void) {
    UiAssignPlotPanelView v;
    float frac[MAX_ASSIGN_PLOT_SERIES], value[MAX_ASSIGN_PLOT_SERIES];

    memset(&v, 0, sizeof(v));
    v.window_w = g_window_w;
    v.window_h = g_window_h;
    v.visible  = assign_plot_is_open();
    v.plot     = assign_plot_view();
    demo_panel_pos(&v.panel_x, &v.panel_y);
    v.pointer_x = g_pointer_x;
    v.pointer_y = g_pointer_y;

    /* Titles are the host's: the peer holds row indices, and the app rebuilds
     * these from the live source rows every frame so an edited row retitles
     * itself. Here the rows never change, so the names are constant. */
    for (int s = 0; s < v.plot.series_count; s++) {
        int row = v.plot.series[s].source_line_idx;
        v.titles[s] = demo_row_is_plottable(row) ? k_row_names[row] : "?";
    }

    /* Where the PC falls in each series, and what each computed there. */
    v.replay_active = g_scrub;
    if (g_scrub && assign_plot_exec_progress(demo_exec_limit(), frac, value)) {
        for (int s = 0; s < MAX_ASSIGN_PLOT_SERIES; s++) {
            v.replay_frac[s]  = frac[s];
            v.replay_value[s] = value[s];
        }
    } else {
        for (int s = 0; s < MAX_ASSIGN_PLOT_SERIES; s++)
            v.replay_frac[s] = -1.0f;
    }
    return v;
}

/* --- The source listing -------------------------------------------------- *
 *
 * The demo's stand-in for the code panel: three rows, right-clickable. Row 0
 * is at the top, so the geometry is shared by the draw and the hit test the
 * same way the panel's own control row is. */
#define DEMO_SRC_X      16
#define DEMO_SRC_TOP    36    /* baseline of row 0, from the window top */
#define DEMO_SRC_ROW_H  (FONT_SMALL_H + 5)
/* Window band the listing owns: the source rows plus the status line under
 * them. The wave gets what is left, so the two never overlap. */
#define DEMO_LISTING_H  (DEMO_SRC_TOP + (DEMO_ROW_COUNT + 1) * DEMO_SRC_ROW_H)

static int demo_row_baseline(int row) {
    return g_window_h - DEMO_SRC_TOP - row * DEMO_SRC_ROW_H;
}

/* Which source row is at window (x, y) - GLUT coords, y down - or -1. */
static int demo_row_at(int x, int y) {
    int gl_y = g_window_h - y;
    for (int row = 0; row < DEMO_ROW_COUNT; row++) {
        int base = demo_row_baseline(row);
        if (x >= DEMO_SRC_X && gl_y >= base - 3 && gl_y < base + FONT_SMALL_H - 2)
            return row;
    }
    return -1;
}

/* Which series `row` is plotted as, or -1. The listing marks a plotted row in
 * that series' color, so it and the legend name the same thing in the same
 * hue. */
static int demo_series_of_row(const AssignPlotView *v, int row) {
    for (int s = 0; s < v->series_count; s++)
        if (v->series[s].source_line_idx == row) return s;
    return -1;
}

static void demo_draw_source(void) {
    AssignPlotView v = assign_plot_view();
    char line[96];

    gl2d_begin(g_window_w, g_window_h);
    for (int row = 0; row < DEMO_ROW_COUNT; row++) {
        int series = demo_series_of_row(&v, row);
        float base_y = (float)demo_row_baseline(row);

        if (series >= 0) {
            float rgb[3];
            ui_assign_plot_series_color(series, rgb);
            glColor4f(rgb[0], rgb[1], rgb[2], 1.0f);
            glRectf((float)(DEMO_SRC_X - 10), base_y,
                    (float)(DEMO_SRC_X - 5), base_y + 6.0f);
        }
        ui_clr(series >= 0 ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)DEMO_SRC_X, base_y, k_row_source[row],
                         FONT_SMALL);
    }

    /* Status line: what the PC is doing, since the plot's rules only mean
     * anything against it. */
    if (g_scrub)
        snprintf(line, sizeof(line),
                 "scrub: exec %d/%d   [ ] step   r stop   space %s",
                 g_pc / DEMO_TRACE_STRIDE, DEMO_ITERS,
                 g_paused ? "run" : "pause");
    else
        snprintf(line, sizeof(line),
                 "r scrub   space %s   right-click a row to plot it"
                 "   shift+right-click add to plot",
                 g_paused ? "run" : "pause");
    ui_clr(UI_TOK_TEXT_PLACEHOLDER);
    gl2d_draw_string((float)DEMO_SRC_X,
                     (float)demo_row_baseline(DEMO_ROW_COUNT) - 6.0f,
                     line, FONT_SMALL);
    gl2d_end();
}

/* --- The wave ------------------------------------------------------------ *
 *
 * Drawn straight out of the trace rather than recomputed, so the curve cannot
 * drift from what the plot is showing. Under the PC it stops where the PC is,
 * which is what replay does to the program in the app.
 *
 * It gets its own viewport band under the listing rather than a fudge factor
 * on the values: the whole point is that these are the numbers in the panel,
 * so the curve is drawn at its true amplitude and the window is divided
 * instead. */
static void demo_draw_wave(void) {
    int limit = demo_exec_limit();
    int band_h = g_window_h - DEMO_LISTING_H;

    if (limit < 0) limit = g_trace_len;
    if (band_h < 60) band_h = g_window_h;    /* a window too short to divide */

    glViewport(0, 0, g_window_w, band_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.15, 1.15, -0.95, 0.95, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Zero line: the curve is signed, and the plot's own axis has one. */
    ui_clr_a(UI_TOK_DIVIDER, 0.5f);
    glBegin(GL_LINES);
    glVertex2f(-1.05f, 0.0f);
    glVertex2f(1.05f, 0.0f);
    glEnd();

    glLineWidth(2.0f);
    glColor3f(0.35f, 0.80f, 1.0f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < g_trace_len && i < limit; i++) {
        int iter;
        float x;
        if (g_trace[i].row != -1) continue;   /* vertex entries only */
        iter = i / DEMO_TRACE_STRIDE;         /* integer: the loop index */
        x = (float)iter / (float)(DEMO_ITERS - 1);
        glVertex2f(x * 2.0f - 1.0f, g_trace[i].value);
    }
    glEnd();
    glLineWidth(1.0f);

    /* The 2D overlays own the whole window. */
    glViewport(0, 0, g_window_w, g_window_h);
}

/* --- GLUT callbacks ------------------------------------------------------ */
static void display_func(void) {
    UiAssignPlotPanelView view;

    if (!g_paused) g_time += 1.0f / 60.0f;
    demo_run_program();

    /* Scrubbing re-runs the program under a moving PC, so the trace the marker
     * is drawn over has to be this frame's - the same reason the app turns on
     * live capture for the duration of a replay. */
    assign_plot_set_live_capture(g_scrub);
    assign_plot_capture((double)glutGet(GLUT_ELAPSED_TIME) * 1000.0);

    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    demo_draw_wave();
    demo_draw_source();

    view = demo_build_view();
    ui_assign_plot_panel_render(&view);

    glutSwapBuffers();
}

/* Window size only: both the wave band and the 2D overlays set their own
 * viewport and projection at draw time, because they do not share one. */
static void reshape_func(int w, int h) {
    if (h < 1) h = 1;
    g_window_w = w;
    g_window_h = h;
    glViewport(0, 0, w, h);
}

static void idle_func(void) {
    if (g_scrub && !g_paused) demo_pc_step(1);
    glutPostRedisplay();
}

/* Panel chips first, then the source listing - the panel floats over the
 * window, so a click inside it is never a click on what is behind it. */
static void mouse_func(int button, int state, int x, int y) {
    UiAssignPlotPanelView view;
    int hit, row, shift;

    if (state != GLUT_DOWN) return;
    g_pointer_x = x;
    g_pointer_y = y;

    view = demo_build_view();
    hit = ui_assign_plot_panel_hit_test(&view, x, y);
    if (hit != UI_ASSIGN_PLOT_HIT_NONE) {
        switch (hit) {
        case UI_ASSIGN_PLOT_HIT_CLOSE:  assign_plot_close();            break;
        /* Right-press cycles a state chip backward, as in the app. */
        case UI_ASSIGN_PLOT_HIT_RATE:
            assign_plot_cycle_rate(button == GLUT_RIGHT_BUTTON ? -1 : 1);
            break;
        case UI_ASSIGN_PLOT_HIT_YSCALE: assign_plot_toggle_y_log();     break;
        case UI_ASSIGN_PLOT_HIT_EXPAND: assign_plot_toggle_expanded();  break;
        case UI_ASSIGN_PLOT_HIT_RESET:  assign_plot_reset();            break;
        default: break;   /* a legend entry: hover-only, inert on click */
        }
        glutPostRedisplay();
        return;
    }

    row = demo_row_at(x, y);
    if (row >= 0 && button == GLUT_RIGHT_BUTTON) {
        shift = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0;
        if (shift) {
            int result = assign_plot_toggle_series(row);
            if (result == ASSIGN_PLOT_SERIES_FULL)
                printf("assign_plot_demo: %d series is the limit\n",
                       MAX_ASSIGN_PLOT_SERIES);
            else if (result == ASSIGN_PLOT_SERIES_INCOMPATIBLE)
                printf("assign_plot_demo: '%s' has a different execution "
                       "count - no shared X axis\n", k_row_names[row]);
        } else {
            assign_plot_toggle(row);
        }
    }
    glutPostRedisplay();
}

/* The legend's hovered entry decides whose statistics the panel shows, so the
 * pointer has to be tracked even when nothing is pressed. */
static void passive_motion_func(int x, int y) {
    g_pointer_x = x;
    g_pointer_y = y;
    glutPostRedisplay();
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x; (void)y;
    switch (key) {
    case 'r': case 'R':
        g_scrub = !g_scrub;
        if (g_scrub) g_pc = DEMO_TRACE_STRIDE;
        break;
    case '[': g_scrub = 1; demo_pc_step(-1); break;
    case ']': g_scrub = 1; demo_pc_step(1);  break;
    case ' ': g_paused = !g_paused;          break;
    case '1': assign_plot_toggle_series(DEMO_ROW_BASE);   break;
    case '2': assign_plot_toggle_series(DEMO_ROW_RIPPLE); break;
    case '3': assign_plot_toggle_series(DEMO_ROW_WAVE);   break;
    case 27: case 'q': case 'Q': exit(0);
    default: break;
    }
    glutPostRedisplay();
}

/* Capture hooks, mirroring color_picker_demo's GLR_DEMO_OPEN_PICKER: pose the
 * demo from the environment so a headless frame grab needs no synthetic input.
 * GLR_DEMO_PLOT_ROWS=<r,r,...> opens the plot on those rows (the first is the
 * primary), GLR_DEMO_PC=<iteration> parks the program counter there. Symbol
 * names stay demo_*: check-assign-plot-demo-isolation greps nm for
 * repl_/editor_/glr_. */
static void demo_stage_from_env(void) {
    const char *rows = getenv("GLR_DEMO_PLOT_ROWS");
    const char *pc   = getenv("GLR_DEMO_PC");

    if (rows && *rows) {
        /* Replace whatever main() opened rather than toggling against it: the
         * first row listed has to end up the primary, since it is the one that
         * fixes the X axis. */
        assign_plot_close();
        for (const char *s = rows; *s; ) {
            int row = atoi(s);
            if (demo_row_is_plottable(row)) assign_plot_toggle_series(row);
            while (*s && *s != ',') s++;
            if (*s == ',') s++;
        }
    }
    if (pc && *pc) {
        int iter = atoi(pc);
        if (iter < 1) iter = 1;
        if (iter > DEMO_ITERS) iter = DEMO_ITERS;
        g_scrub = 1;
        g_pc = iter * DEMO_TRACE_STRIDE;
        /* Frozen where it was asked for: an advancing PC would land the grab
         * somewhere else. */
        g_paused = 1;
    }
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(g_window_w, g_window_h);
    glutCreateWindow("assign_plot_demo");

    assign_plot_reset_all();
    assign_plot_install_host(&g_demo_host);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    assign_plot_open(DEMO_ROW_WAVE);
    demo_run_program();
    demo_stage_from_env();

    glutDisplayFunc(display_func);
    glutReshapeFunc(reshape_func);
    glutMouseFunc(mouse_func);
    glutPassiveMotionFunc(passive_motion_func);
    glutKeyboardFunc(keyboard_func);
    glutIdleFunc(idle_func);

    printf("assign_plot_demo: right-click a source row to plot it "
           "(Shift adds a series).\n");
    printf("  r=scrub the program counter  [ ]=step  space=pause  "
           "1..3=toggle a row  q=quit\n");
    glutMainLoop();
    return 0;
}
