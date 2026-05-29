/*
 * ui_cpuprof.c - per-section wall-time profiling overlay panel.
 */
#include "ui/support/cpuprof.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"
#include "support/cpuprof.h"

#include <stdio.h>
#include <string.h>

/* Dim/stale text tiers: deliberately darker than TEXT_MUTED so stale
 * rows recede - no clean token twin, kept as theme-stable named consts
 * (theme.h "named constant" bucket). The FPS gauge in set_time_color
 * is the separate documented data-viz exclusion (red must stay red). */
static const float k_prof_stale[3] = { 0.35f, 0.35f, 0.42f };
static const float k_prof_dim[3]   = { 0.30f, 0.30f, 0.38f };

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* Panel geometry (pixels). PROFILE_PANEL_W lives in cpuprof.h so
 * sibling panels (ui_memory_panel) can shift left for side-by-side
 * layout; the local PROF_PANEL_W alias preserves the historical name. */
#define PROF_PANEL_W        PROFILE_PANEL_W
#define PROF_PANEL_MARGIN    12
#define PROF_ROW_H           16
#define PROF_HEADER_H        20
#define PROF_BOTTOM_PAD      14
#define PROF_COL_LABEL_W    150
#define PROF_COL_LAST_W      72

/* Visual indentation per nesting level. Labels arrive un-indented; the panel
 * offsets each row by depth * this, so prof_section_info()'s explicit depth is
 * the single source of truth (restyling the indent never re-classifies a row).
 * FONT_SMALL is fixed-width, so 2 cell widths reads as the prior 2-space step. */
#define PROF_INDENT_W       (FONT_SMALL_W * 2)

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

/* Format µs as "1234 us" or "12.3 ms", whichever is more readable. */
static void fmt_us(char *buf, int buf_sz, double us) {
    if (us < 1000.0)
        snprintf(buf, (size_t)buf_sz, "%.0f us", us);
    else
        snprintf(buf, (size_t)buf_sz, "%.2f ms", us / 1000.0);
}

/* Apply a green/yellow/red color based on section timing thresholds.
 * The whole-frame total row (is_total) uses 1/120s (8.3ms) and 1/60s
 * (16.7ms) breakpoints; every other section uses half those thresholds
 * (4.15ms / 8.3ms). */
/* FPS gauge: green/yellow/red is a fixed data-viz semantic, NOT theme
 * tokens (theme.h bucket 3 - red must read as "over budget" in every
 * scheme; it must not follow the UI accent). */
static void set_time_color(int is_total, double us) {
    if (us < 2.0) {
        glColor3f(0.30f, 0.30f, 0.38f);       /* near-zero – same as stale */
        return;
    }
    if (is_total) {
        if (us < 8333.0)
            glColor3f(0.50f, 0.88f, 0.45f);   /* green  – fits in 120 fps */
        else if (us < 16667.0)
            glColor3f(0.95f, 0.82f, 0.25f);   /* yellow – fits in 60 fps */
        else
            glColor3f(0.95f, 0.38f, 0.32f);   /* red    – below 60 fps */
    } else {
        if (us < 4167.0)
            glColor3f(0.50f, 0.88f, 0.45f);   /* green  – half-budget OK */
        else if (us < 8333.0)
            glColor3f(0.95f, 0.82f, 0.25f);   /* yellow – half-budget tight */
        else
            glColor3f(0.95f, 0.38f, 0.32f);   /* red    – over half-budget */
    }
}

/* A "detail" row is a nested child (prof_section_info().depth > 0); the app
 * supplies the nesting so a new sub-section needs no edit here. Detail rows
 * are hidden outside DETAILS mode. */
static int section_visible(int profile_mode, ProfSection s) {
    if (profile_mode != PROFILE_PANEL_DETAILS &&
        prof_section_info(s).depth > 0)
        return 0;
    return 1;
}

static int visible_section_count(int profile_mode) {
    int section_count = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (section_visible(profile_mode, (ProfSection)section_idx))
            section_count++;
    }
    return section_count;
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

/* Panel total height for a given mode: header + column headings + one row per
 * visible section + divider before FRAME_TOTAL + bottom padding. The
 * controller needs this (via ui_profile_panel_height) to resolve the stacked
 * anchor; the renderer uses it to size the frame. */
int ui_profile_panel_height(UiProfilePanelMode mode) {
    int visible_count = visible_section_count((int)mode);
    return PROF_HEADER_H
         + 18                           /* column heading row */
         + visible_count * PROF_ROW_H
         + 4                            /* divider before FRAME_TOTAL */
         + PROF_BOTTOM_PAD
         + PROF_PANEL_MARGIN;
}

int ui_profile_panel_width(void) {
    return PROF_PANEL_W;
}

void ui_profile_panel_render(const UiProfilePanelView *view) {
    int profile_mode = view->mode;
    if (profile_mode == PROFILE_PANEL_OFF) return;

    int panel_h = ui_profile_panel_height((UiProfilePanelMode)profile_mode);

    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    /* Background + Border */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)PROF_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - PROF_HEADER_H + 2;
    const char *HINT = "Ctrl+W:hide";
    const int hint_width = FONT_SMALL_W * (int)strlen(HINT) + 2;

    /* Title */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "CPU Profile", FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + PROF_PANEL_W - hint_width), (float)ty, HINT, FONT_SMALL);

    ty -= PROF_HEADER_H;

    /* Column headings */
    int col_last = tx + PROF_COL_LABEL_W;
    int col_avg  = col_last + PROF_COL_LAST_W;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)tx,        (float)ty, "Section",  FONT_SMALL);
    gl2d_draw_string((float)col_last,  (float)ty, "Last",     FONT_SMALL);
    gl2d_draw_string((float)col_avg,   (float)ty, "Avg",      FONT_SMALL);
    ty -= 2;

    /* Thin rule under headings */
    ui_clr_a(UI_TOK_DIVIDER, 0.80f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_LINES);
    glVertex2f((float)(panel_x + 4),              (float)ty);
    glVertex2f((float)(panel_x + PROF_PANEL_W - 4), (float)ty);
    glEnd();
    glDisable(GL_BLEND);

    ty -= PROF_ROW_H - 2;

    /* One row per section.  Insert a separator line before FRAME_TOTAL. */
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!section_visible(profile_mode, s)) continue;

        ProfSectionInfo info = prof_section_info(s);

        if (info.is_total) {
            /* Divider above totals row */
            ui_clr_a(UI_TOK_DIVIDER, 0.80f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBegin(GL_LINES);
            glVertex2f((float)(panel_x + 4),               (float)(ty + PROF_ROW_H - 3));
            glVertex2f((float)(panel_x + PROF_PANEL_W - 4),(float)(ty + PROF_ROW_H - 3));
            glEnd();
            glDisable(GL_BLEND);
        }

        int stale = prof_section_is_stale(s);

        /* Label */
        if (info.is_total)
            ui_clr(UI_TOK_TEXT_PRIMARY);
        else if (stale)
            glColor3fv(k_prof_stale);
        else if (info.depth > 0)
            ui_clr(UI_TOK_TEXT_SECTION);
        else
            ui_clr(UI_TOK_TEXT_PRIMARY);
        /* Indentation is derived from depth, not baked into the label. */
        gl2d_draw_string((float)(tx + info.depth * PROF_INDENT_W),
                         (float)ty, info.label, FONT_SMALL);

        /* Last / avg values */
        char last_buf[24], avg_buf[24];
        if (stale) {
            snprintf(last_buf, sizeof(last_buf), "--");
            snprintf(avg_buf,  sizeof(avg_buf),  "--");
            glColor3fv(k_prof_dim);
            gl2d_draw_string((float)col_last, (float)ty, last_buf, FONT_SMALL);
            gl2d_draw_string((float)col_avg,  (float)ty, avg_buf,  FONT_SMALL);
        } else {
            fmt_us(last_buf, (int)sizeof(last_buf), prof_section_last_us(s));
            fmt_us(avg_buf,  (int)sizeof(avg_buf),  prof_section_avg_us(s));
            set_time_color(info.is_total, prof_section_last_us(s));
            gl2d_draw_string((float)col_last, (float)ty, last_buf, FONT_SMALL);
            set_time_color(info.is_total, prof_section_avg_us(s));
            gl2d_draw_string((float)col_avg,  (float)ty, avg_buf,  FONT_SMALL);
        }

        ty -= PROF_ROW_H;
    }

    gl2d_end();
}