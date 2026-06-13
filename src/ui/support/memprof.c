/*
 * ui_memprof.c - process memory profiling overlay panel.
 */
#include "ui/support/memprof.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"
#include "support/memprof.h"
#include "config.h"                  /* FONT_SMALL, FONT_SMALL_W */

#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Panel geometry                                                             */
/* ========================================================================= */

/* Matches the variable panel width so the right-column overlay stack reads
 * as one uniform column. The shortcut is in the Config menu + F1 help, so no
 * inline hint is needed in the header. */
#define MEM_PANEL_W         250
#define MEM_PANEL_MARGIN     12
#define MEM_ROW_H            14
#define MEM_HEADER_H         18
#define MEM_TEXT_BLOCK_H     (3 * MEM_ROW_H + 4)  /* 3 rows + divider */
#define MEM_GRAPH_H          90
#define MEM_Y_GUTTER         56   /* room for "999.9 MB" / "9.99 GB" tiny labels */
#define MEM_BOTTOM_PAD       16   /* X labels + margin */

/* Variable-width FONT_TINY (Helvetica 10) string-width helper. Uses the
 * GLUT per-character width API rather than a flat approximation so the
 * Y / X tick labels right-align cleanly without spilling out of the
 * gutter. */
static int tiny_text_w(const char *s) {
    int w = 0;
    for (; *s; s++)
        w += glutBitmapWidth(FONT_TINY, (unsigned char)*s);
    return w;
}

/* Total panel height from the geometry constants above. The controller
 * needs this (via ui_memory_panel_height) to resolve the stacked anchor;
 * the renderer uses it to size the frame. */
static int mem_panel_height(void) {
    return MEM_HEADER_H + MEM_TEXT_BLOCK_H + MEM_GRAPH_H
         + MEM_BOTTOM_PAD + MEM_PANEL_MARGIN;
}

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

/* Format a signed delta using memprof's formatter for the magnitude.
 * memprof_format_bytes right-aligns the digit portion to
 * MEMPROF_FMT_WIDTH chars (leading-space padded), so the unit suffix
 * lands in the same column for RSS / init / delta rows. To keep that
 * column alignment, the sign character overwrites the rightmost
 * leading space rather than prepending — prepending would push the
 * digits one column right of the RSS / init rows.
 *
 * Either side being zero (reader-failure sentinel — current/baseline
 * rows render "--" for that case) propagates as "--" so the delta row
 * does not show a fictitious large negative against zero. */
static void fmt_delta(char *buf, int buf_sz,
                      unsigned long long cur, unsigned long long base) {
    if (cur == 0 || base == 0) {
        snprintf(buf, (size_t)buf_sz, "--");
        return;
    }
    if (cur == base) {
        /* Zero delta: render as "0.0 MB" with the same width formatting
         * as RSS / init so the unit column lines up. */
        snprintf(buf, (size_t)buf_sz, "%*.1f MB", MEMPROF_FMT_WIDTH, 0.0);
        return;
    }
    char tmp[128];
    unsigned long long mag = (cur > base) ? cur - base : base - cur;
    char sign = (cur > base) ? '+' : '-';
    memprof_format_bytes(tmp, (int)sizeof(tmp), mag);
    char *digit_start = tmp;
    while (*digit_start == ' ') digit_start++;
    if (digit_start > tmp) {
        *(digit_start - 1) = sign;
        snprintf(buf, (size_t)buf_sz, "%s", tmp);
    } else {
        /* No leading space (e.g. KB case which memprof_format_bytes does
         * not pad). Prepend sign; this row may sit one column left of the
         * MB/GB rows but is rare for a long-running REPL. */
        snprintf(buf, (size_t)buf_sz, "%c%s", sign, tmp);
    }
}

/* Round (range / 3) up to {1, 2, 5} * 10^k bytes. Yields ~3-5 ticks. */
static unsigned long long pick_nice_step(unsigned long long range) {
    if (range == 0) return 1024ULL;
    unsigned long long target = range / 3;
    if (target < 1024ULL) target = 1024ULL;
    unsigned long long base = 1024ULL;  /* start at 1 KB */
    while (base * 50ULL < target) {
        if (base > (unsigned long long)-1 / 10ULL) break;
        base *= 10ULL;
    }
    for (;;) {
        if (base       >= target) return base;
        if (base * 2ULL >= target) return base * 2ULL;
        if (base * 5ULL >= target) return base * 5ULL;
        if (base > (unsigned long long)-1 / 10ULL) return base * 5ULL;
        base *= 10ULL;
    }
}

/* "-85m" / "-42m" / "now" for a given offset (seconds before "now"). */
static void fmt_time_offset(char *buf, int buf_sz, double seconds_ago) {
    if (seconds_ago < 1.0) {
        snprintf(buf, (size_t)buf_sz, "now");
    } else if (seconds_ago < 90.0) {
        snprintf(buf, (size_t)buf_sz, "-%.0fs", seconds_ago);
    } else {
        snprintf(buf, (size_t)buf_sz, "-%.0fm", seconds_ago / 60.0);
    }
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

void ui_memory_panel_render(const UiMemoryPanelView *view) {
    int mode = view->mode;
    if (mode == MEMORY_PANEL_OFF) return;

    int panel_h = mem_panel_height();

    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)MEM_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - MEM_HEADER_H + 2;

    /* No inline hint — the Config menu row carries the Ctrl+Shift+W
     * shortcut and F1 help duplicates it. Header has just the title. */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "Memory Profile", FONT_SMALL);

    ty -= MEM_HEADER_H;

    /* Text block: three single-column rows showing current / init / delta
     * RSS. VSZ deliberately dropped — on macOS it counts the whole virtual
     * address reservation including unmapped pages and renders as GB-scale
     * noise that swamps the graph; RSS is the portable leak signal. */
    MemSample cur  = memprof_current();
    MemSample base = memprof_baseline();

    int col_label = tx;
    int col_value = tx + 64;
    char val_buf[256];

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "RSS",   FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    memprof_format_bytes(val_buf, (int)sizeof(val_buf), cur.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "init",  FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    memprof_format_bytes(val_buf, (int)sizeof(val_buf), base.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)col_label, (float)ty, "delta", FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    fmt_delta(val_buf, (int)sizeof(val_buf), cur.rss_bytes, base.rss_bytes);
    gl2d_draw_string((float)col_value, (float)ty, val_buf, FONT_SMALL);
    ty -= MEM_ROW_H;

    /* Divider before graph */
    ui_clr_a(UI_TOK_DIVIDER, 0.80f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    glVertex2f((float)(panel_x + 4),                (float)(ty + 3));
    glVertex2f((float)(panel_x + MEM_PANEL_W - 4),  (float)(ty + 3));
    glEnd();
    glDisable(GL_BLEND);

    /* Graph area */
    int plot_x = panel_x + MEM_Y_GUTTER + 4;
    int plot_w = MEM_PANEL_W - MEM_Y_GUTTER - 8;
    int plot_y = panel_y + MEM_BOTTOM_PAD;

    /* Auto-fit Y range from RSS history. */
    unsigned long long hi = memprof_history_max_rss();

    if (hi == 0) {
        /* No data yet - draw an empty plot frame and a placeholder. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl2d_panel_frame((float)plot_x, (float)plot_y,
                         (float)plot_w, (float)MEM_GRAPH_H,
                         UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);
        glDisable(GL_BLEND);
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        const char *empty = "(collecting samples)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        gl2d_draw_string((float)plot_x + ((float)plot_w - (float)ew) * 0.5f,
                 (float)plot_y + (float)MEM_GRAPH_H * 0.5f,
                         empty, FONT_SMALL);
        gl2d_end();
        return;
    }

    unsigned long long lo = memprof_history_min_rss();
    unsigned long long range = (hi > lo) ? hi - lo : 0;
    if (range < 1024ULL * 1024ULL) range = 1024ULL * 1024ULL;
    unsigned long long pad   = range / 10;
    unsigned long long y_lo  = (lo > pad / 2) ? lo - pad / 2 : 0;
    unsigned long long y_hi  = hi + pad;
    unsigned long long step  = pick_nice_step(y_hi - y_lo);

    /* Snap to step multiples so labels land on round values. */
    if (step > 0) {
        y_lo = (y_lo / step) * step;
        y_hi = ((y_hi + step - 1) / step) * step;
    }
    if (y_hi <= y_lo) y_hi = y_lo + step;

    /* Plot frame + faint horizontal gridlines + Y labels */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)MEM_GRAPH_H,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);

    ui_clr_a(UI_TOK_DIVIDER, 0.30f);
    glBegin(GL_LINES);
    for (unsigned long long v = y_lo; v <= y_hi; v += step) {
        if (y_hi <= y_lo) break;
        double frac = (double)(v - y_lo) / (double)(y_hi - y_lo);
        float gy = (float)(plot_y + frac * MEM_GRAPH_H);
        glVertex2f((float)plot_x,            gy);
        glVertex2f((float)(plot_x + plot_w), gy);
        if (step == 0) break;
    }
    glEnd();
    glDisable(GL_BLEND);

    /* Y labels right-aligned in the gutter (FONT_TINY, Helvetica 10). */
    ui_clr(UI_TOK_TEXT_MUTED);
    for (unsigned long long v = y_lo; v <= y_hi; v += step) {
        if (y_hi <= y_lo) break;
        double frac = (double)(v - y_lo) / (double)(y_hi - y_lo);
        int gy = plot_y + (int)(frac * MEM_GRAPH_H);
        char lab[24];
        memprof_format_bytes(lab, (int)sizeof(lab), v);
        int lw = tiny_text_w(lab);
        gl2d_draw_string((float)(panel_x + MEM_Y_GUTTER - 2 - lw),
                         (float)(gy - 5),
                         lab, FONT_TINY);
        if (step == 0) break;
    }

    /* Plot the RSS history. The X axis spans the ACTUAL stored window —
     * the oldest sample's timestamp through the newest's — so it adapts to
     * whatever cadence the samples arrived at instead of assuming a fixed
     * MEMPROF_PUSH_INTERVAL_S per slot. The newest sample pins to the right
     * edge ("now"); the oldest pins to the left. */
    int n = memprof_history_count();
    double t_latest = memprof_history_latest_t();
    double span_s = 0.0;
    if (n > 1) {
        MemSample s_oldest;
        double    t_oldest;
        memprof_history_get(0, &s_oldest, &t_oldest);
        span_s = t_latest - t_oldest;
    }
    if (n > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ui_clr(UI_TOK_ACCENT);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < n; i++) {
            MemSample s;
            double    t_rel;
            memprof_history_get(i, &s, &t_rel);
            double age = t_latest - t_rel;
            /* span_s <= 0 means a single sample: pin it to the right edge. */
            float xx = (span_s > 0.0)
                     ? (float)(plot_x + plot_w - (age / span_s) * plot_w)
                     : (float)(plot_x + plot_w);
            double frac = (double)(s.rss_bytes - y_lo)
                        / (double)(y_hi - y_lo);
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            float yy = (float)(plot_y + frac * MEM_GRAPH_H);
            glVertex2f(xx, yy);
        }
        glEnd();
        glDisable(GL_BLEND);
    }

    /* X-axis labels reflect the actual stored window: left = oldest sample
     * age, middle = half, right = now. With under a second of data (a single
     * sample) only "now" is meaningful, so the left/middle ticks are
     * suppressed rather than printing a bogus "-0s". */
    ui_clr(UI_TOK_TEXT_MUTED);
    char right_lab[16];
    fmt_time_offset(right_lab, (int)sizeof(right_lab), 0.0);
    int label_y = plot_y - 12;
    int rl_w = tiny_text_w(right_lab);
    if (span_s >= 1.0) {
        char left_lab[16], mid_lab[16];
        fmt_time_offset(left_lab, (int)sizeof(left_lab), span_s);
        fmt_time_offset(mid_lab,  (int)sizeof(mid_lab),  span_s / 2.0);
        int ml_w = tiny_text_w(mid_lab);
        gl2d_draw_string((float)plot_x, (float)label_y, left_lab, FONT_TINY);
        gl2d_draw_string((float)plot_x + (float)plot_w * 0.5f - (float)ml_w * 0.5f,
                         (float)label_y, mid_lab, FONT_TINY);
    }
    gl2d_draw_string((float)(plot_x + plot_w - rl_w),
                     (float)label_y, right_lab, FONT_TINY);

    gl2d_end();
}

/* ========================================================================= */
/* Footprint accessors (for controller-side sibling-panel stacking)          */
/* ========================================================================= */

int ui_memory_panel_width(void) {
    return MEM_PANEL_W;
}

int ui_memory_panel_height(void) {
    return mem_panel_height();
}