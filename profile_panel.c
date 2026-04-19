/*
 * profile_panel.c — per-section wall-time profiling overlay panel.
 *
 * Uses a fast monotonic clock so sampling doesn't itself dominate the
 * sections it measures:
 *   - Linux:  clock_gettime(CLOCK_MONOTONIC) — served from the vDSO, no
 *             syscall on modern kernels.
 *   - macOS:  mach_absolute_time() — direct timer read, no syscall.
 * This is wall clock (elapsed time), not CPU time; in a single-threaded
 * render loop the two differ only when the process is preempted, which
 * is exactly what a frame-budget profiler should surface anyway.
 *
 * Each section is timed with prof_begin/prof_end.  The last measured value
 * and an exponential moving average (EMA, alpha = 0.08) are maintained.
 * Sections that didn't run this frame are shown with their most recent value
 * dimmed; after PROF_STALE_FRAMES frames without a sample they show "--".
 */
#define _POSIX_C_SOURCE 200809L
#include "sample.h"
#include "profile_panel.h"

#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* EMA smoothing factor.  Higher = more responsive, lower = smoother. */
#define PROF_EMA_ALPHA      0.08

/* After this many frames without a new sample, display "--" instead of the
 * stale value.  At 60 fps this is 3 seconds. */
#define PROF_STALE_FRAMES   180

/* Panel geometry (pixels). */
#define PROF_PANEL_W        320
#define PROF_PANEL_MARGIN    12
#define PROF_ROW_H           16
#define PROF_HEADER_H        20
#define PROF_BOTTOM_PAD      14
#define PROF_COL_LABEL_W    150
#define PROF_COL_LAST_W      72

/* ========================================================================= */
/* State                                                                      */
/* ========================================================================= */

static double g_prof_start[PROF_SECTION_COUNT];
static double g_prof_last_us[PROF_SECTION_COUNT];  /* last measured wall µs */
static double g_prof_avg_us[PROF_SECTION_COUNT];   /* EMA in µs             */
static int    g_prof_stale[PROF_SECTION_COUNT];    /* frames since last sample */
static double g_prof_accum_pending[PROF_SECTION_COUNT]; /* running total for accum-commit */
static int    g_prof_initialized = 0;

int g_show_profile_panel = PROFILE_PANEL_OFF;

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

#ifdef __APPLE__
static double prof_now_us(void) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    /* mach_absolute_time returns ticks; convert to nanoseconds via the
     * timebase, then to microseconds. Division by 1000.0 is in double to
     * keep sub-microsecond resolution for the EMA. */
    uint64_t ticks = mach_absolute_time();
    double nsec = (double)ticks * (double)tb.numer / (double)tb.denom;
    return nsec / 1000.0;
}
#else
static double prof_now_us(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC is served by the vDSO on Linux, so this is a
     * single register-load worth of overhead rather than a full syscall. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1000.0;
}
#endif

static void init_if_needed(void) {
    if (g_prof_initialized) return;
    for (int i = 0; i < PROF_SECTION_COUNT; i++) {
        g_prof_start[i]         = 0.0;
        g_prof_last_us[i]       = 0.0;
        g_prof_avg_us[i]        = 0.0;
        g_prof_stale[i]         = PROF_STALE_FRAMES; /* treat as stale until first sample */
        g_prof_accum_pending[i] = 0.0;
    }
    g_prof_initialized = 1;
}

/* ========================================================================= */
/* Public API                                                                 */
/* ========================================================================= */

void prof_begin(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    g_prof_start[s] = prof_now_us();
}

void prof_end(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();

    double elapsed = prof_now_us() - g_prof_start[s];
    if (elapsed < 0.0) elapsed = 0.0;

    g_prof_last_us[s] = elapsed;
    g_prof_stale[s]   = 0;

    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = elapsed;  /* seed with first sample */
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * elapsed
                         + (1.0 - PROF_EMA_ALPHA) * g_prof_avg_us[s];
}

void prof_accum_reset(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    g_prof_accum_pending[s] = 0.0;
}

void prof_accum_end(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double elapsed = prof_now_us() - g_prof_start[s];
    if (elapsed < 0.0) elapsed = 0.0;
    g_prof_accum_pending[s] += elapsed;
    g_prof_stale[s] = 0;
}

void prof_accum_commit(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double total = g_prof_accum_pending[s];
    g_prof_last_us[s] = total;
    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = total;
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * total
                         + (1.0 - PROF_EMA_ALPHA) * g_prof_avg_us[s];
}

void prof_frame_tick(void) {
    init_if_needed();
    /* Increment staleness counters for all sections.  For sections that run
     * during this frame prof_end() will reset their counter back to 0, so the
     * transient increment here is invisible by the time the panel renders. */
    for (int i = 0; i < PROF_SECTION_COUNT; i++) {
        if (g_prof_stale[i] < PROF_STALE_FRAMES)
            g_prof_stale[i]++;
    }
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

static const char *section_label(ProfSection s) {
    switch (s) {
    case PROF_SCENE_3D:    return "Scene 3D";
    case PROF_SCENE_3D_SETUP:    return "  setup";
    case PROF_SCENE_3D_FILL:     return "  fill";
    case PROF_SCENE_3D_FADE:     return "  fade batches";
    case PROF_SCENE_3D_FADE_PROLOGUE:   return "    prologue";
    case PROF_SCENE_3D_FADE_BATCH_PREP: return "    batch prep";
    case PROF_SCENE_3D_FADE_BATCH_EXEC: return "    batch exec";
    case PROF_SCENE_3D_FADE_BATCH_POST: return "    batch post";
    case PROF_SCENE_3D_HELPERS:  return "  helpers";
    case PROF_SCENE_3D_OUTLINES: return "  outlines";
    case PROF_SCENE_3D_OVERLAYS: return "  overlays";
    case PROF_SCENE_3D_HUD:      return "  hud";
    case PROF_CODE_PANEL:  return "Code Panel";
    case PROF_CODE_PANEL_LAYOUT:   return "  layout";
    case PROF_CODE_PANEL_LAYOUT_GEOM:   return "    geom+rows";
    case PROF_CODE_PANEL_LAYOUT_GEOM_SETUP:      return "      setup";
    case PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE: return "      precompute";
    case PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS:     return "      totals";
    case PROF_CODE_PANEL_LAYOUT_CURSOR: return "    cursor map";
    case PROF_CODE_PANEL_LAYOUT_SCROLL: return "    scroll/follow";
    case PROF_CODE_PANEL_CHROME:   return "  chrome";
    case PROF_CODE_PANEL_LINES:    return "  lines";
    case PROF_CODE_PANEL_LINES_STATIC: return "    static";
    case PROF_CODE_PANEL_LINES_BODY:   return "    body";
    case PROF_CODE_PANEL_LINES_BODY_CMDS:    return "      commands";
    case PROF_CODE_PANEL_LINES_BODY_NEWLINE: return "      newline";
    case PROF_CODE_PANEL_LINES_FOOTER: return "    footer";
    case PROF_CODE_PANEL_OVERLAYS: return "  overlays";
    case PROF_UI_PANELS:   return "UI Panels";
    case PROF_FLATTEN:     return "Flatten";
    case PROF_REFORMAT:    return "Reformat";
    case PROF_FRAME_TOTAL: return "Frame Total";
    default:               return "?";
    }
}

/* Format µs as "1234 us" or "12.3 ms", whichever is more readable. */
static void fmt_us(char *buf, int buf_sz, double us) {
    if (us < 1000.0)
        snprintf(buf, (size_t)buf_sz, "%.0f us", us);
    else
        snprintf(buf, (size_t)buf_sz, "%.2f ms", us / 1000.0);
}

int prof_code_panel_details_enabled(void) {
    return g_show_profile_panel == PROFILE_PANEL_DETAILS;
}

static int is_detail_section(ProfSection s) {
    return (s == PROF_SCENE_3D_SETUP ||
            s == PROF_SCENE_3D_FILL ||
            s == PROF_SCENE_3D_FADE ||
            s == PROF_SCENE_3D_FADE_PROLOGUE ||
            s == PROF_SCENE_3D_FADE_BATCH_PREP ||
            s == PROF_SCENE_3D_FADE_BATCH_EXEC ||
            s == PROF_SCENE_3D_FADE_BATCH_POST ||
            s == PROF_SCENE_3D_HELPERS ||
            s == PROF_SCENE_3D_OUTLINES ||
            s == PROF_SCENE_3D_OVERLAYS ||
            s == PROF_SCENE_3D_HUD ||
            s == PROF_CODE_PANEL_LAYOUT ||
            s == PROF_CODE_PANEL_LAYOUT_GEOM ||
            s == PROF_CODE_PANEL_LAYOUT_GEOM_SETUP ||
            s == PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE ||
            s == PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS ||
            s == PROF_CODE_PANEL_LAYOUT_CURSOR ||
            s == PROF_CODE_PANEL_LAYOUT_SCROLL ||
            s == PROF_CODE_PANEL_CHROME ||
            s == PROF_CODE_PANEL_LINES ||
            s == PROF_CODE_PANEL_LINES_STATIC ||
            s == PROF_CODE_PANEL_LINES_BODY ||
            s == PROF_CODE_PANEL_LINES_BODY_CMDS ||
            s == PROF_CODE_PANEL_LINES_BODY_NEWLINE ||
            s == PROF_CODE_PANEL_LINES_FOOTER ||
            s == PROF_CODE_PANEL_OVERLAYS);
}

static int section_visible(ProfSection s) {
    if (!prof_code_panel_details_enabled() && is_detail_section(s))
        return 0;
    return 1;
}

static int visible_section_count(void) {
    int count = 0;
    for (int i = 0; i < PROF_SECTION_COUNT; i++) {
        if (section_visible((ProfSection)i))
            count++;
    }
    return count;
}

void render_profile_panel(void) {
    if (g_show_profile_panel == PROFILE_PANEL_OFF) return;
    init_if_needed();

    int row_count = visible_section_count();

    /* Panel total height: header + column headings + one row per section +
     * divider before FRAME_TOTAL + bottom padding */
    int panel_h  = PROF_HEADER_H
                 + 18                           /* column heading row */
                 + row_count * PROF_ROW_H
                 + 4                            /* divider before FRAME_TOTAL */
                 + PROF_BOTTOM_PAD
                 + PROF_PANEL_MARGIN;

    int px = g_win_w - PROF_PANEL_W - PROF_PANEL_MARGIN;
    int py = g_win_h - panel_h - PROF_PANEL_MARGIN;

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.05f, 0.05f, 0.08f, 0.91f);
    draw_quad((float)px, (float)py, (float)PROF_PANEL_W, (float)panel_h);

    /* Border */
    glColor4f(0.35f, 0.35f, 0.55f, 0.85f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)px,                   (float)py);
    glVertex2f((float)(px + PROF_PANEL_W),  (float)py);
    glVertex2f((float)(px + PROF_PANEL_W),  (float)(py + panel_h));
    glVertex2f((float)px,                   (float)(py + panel_h));
    glEnd();
    glDisable(GL_BLEND);

    int tx = px + 8;
    int ty = py + panel_h - PROF_HEADER_H + 2;
    const char *HINT = "Ctrl+W:hide";
    const int hint_width = FONT_SMALL_W * (int)strlen(HINT) + 2;

    /* Title */
    glColor3f(0.85f, 0.90f, 1.00f);
    draw_string((float)tx, (float)ty, "CPU Profile", FONT_SMALL);
    glColor3f(0.40f, 0.42f, 0.50f);
    draw_string((float)(px + PROF_PANEL_W - hint_width), (float)ty, HINT, FONT_SMALL);

    ty -= PROF_HEADER_H;

    /* Column headings */
    int col_last = tx + PROF_COL_LABEL_W;
    int col_avg  = col_last + PROF_COL_LAST_W;

    glColor3f(0.45f, 0.50f, 0.62f);
    draw_string((float)tx,        (float)ty, "Section",  FONT_SMALL);
    draw_string((float)col_last,  (float)ty, "Last",     FONT_SMALL);
    draw_string((float)col_avg,   (float)ty, "Avg",      FONT_SMALL);
    ty -= 2;

    /* Thin rule under headings */
    glColor4f(0.25f, 0.25f, 0.40f, 0.80f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    glVertex2f((float)(px + 4),              (float)ty);
    glVertex2f((float)(px + PROF_PANEL_W - 4), (float)ty);
    glEnd();
    glDisable(GL_BLEND);

    ty -= PROF_ROW_H - 2;

    /* One row per section.  Insert a separator line before FRAME_TOTAL. */
    for (int i = 0; i < PROF_SECTION_COUNT; i++) {
        ProfSection s = (ProfSection)i;
        if (!section_visible(s)) continue;

        if (s == PROF_FRAME_TOTAL) {
            /* Divider above totals row */
            glColor4f(0.25f, 0.25f, 0.40f, 0.80f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBegin(GL_LINES);
            glVertex2f((float)(px + 4),               (float)(ty + PROF_ROW_H - 3));
            glVertex2f((float)(px + PROF_PANEL_W - 4),(float)(ty + PROF_ROW_H - 3));
            glEnd();
            glDisable(GL_BLEND);
        }

        int stale = g_prof_stale[s];

        /* Label */
        if (s == PROF_FRAME_TOTAL)
            glColor3f(0.80f, 0.85f, 1.00f);
        else if (stale >= PROF_STALE_FRAMES)
            glColor3f(0.35f, 0.35f, 0.42f);
        else if (is_detail_section(s))
            glColor3f(0.62f, 0.68f, 0.80f);
        else
            glColor3f(0.72f, 0.78f, 0.90f);
        draw_string((float)tx, (float)ty, section_label(s), FONT_SMALL);

        /* Last / avg values */
        char last_buf[24], avg_buf[24];
        if (stale >= PROF_STALE_FRAMES) {
            snprintf(last_buf, sizeof(last_buf), "--");
            snprintf(avg_buf,  sizeof(avg_buf),  "--");
            glColor3f(0.30f, 0.30f, 0.38f);
        } else {
            fmt_us(last_buf, (int)sizeof(last_buf), g_prof_last_us[s]);
            fmt_us(avg_buf,  (int)sizeof(avg_buf),  g_prof_avg_us[s]);
            if (s == PROF_FRAME_TOTAL)
                glColor3f(0.90f, 0.95f, 0.70f);
            else
                glColor3f(0.60f, 0.88f, 0.60f);
        }
        draw_string((float)col_last, (float)ty, last_buf, FONT_SMALL);
        draw_string((float)col_avg,  (float)ty, avg_buf,  FONT_SMALL);

        ty -= PROF_ROW_H;
    }

    end_2d();
}
