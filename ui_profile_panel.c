/*
 * ui_profile_panel.c - per-section wall-time profiling overlay panel.
 */
#include "sample.h"
#include "repl_state.h"
#include "ui_profile_panel.h"
#include "./include/gl_2d.h"
#include "ui_panels.h"
#include "ui_variable_panel.h"
#include "prof.h"

#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* Panel geometry (pixels). */
#define PROF_PANEL_W        320
#define PROF_PANEL_MARGIN    12
#define PROF_ROW_H           16
#define PROF_HEADER_H        20
#define PROF_BOTTOM_PAD      14
#define PROF_COL_LABEL_W    150
#define PROF_COL_LAST_W      72

static int clamp_int(int v, int lo, int hi) {
    if (hi < lo) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clamp_profile_y(int y, int scene_y, int scene_h, int panel_h) {
    int min_y = scene_y + STATUSBAR_H + 4;
    int max_y = scene_y + scene_h - panel_h - 4;

    if (max_y >= min_y)
        return clamp_int(y, min_y, max_y);
    return min_y;
}

static void profile_panel_rect_for_height(int panel_h, int *out_x, int *out_y) {
    int scene_x, scene_y, scene_w, scene_h;
    int panel_x, panel_y;

    ui_panels_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    if (*repl_state_variable_panel()->visible) {
        panel_x = scene_x + scene_w - PROF_PANEL_W - PROF_PANEL_MARGIN;
        panel_y = scene_y + scene_h - panel_h - PROF_PANEL_MARGIN;
    } else {
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect(&var_x, &var_y, &var_w, &var_h);
        panel_x = var_x + var_w - PROF_PANEL_W;
        panel_y = var_y;
    }

    panel_x = clamp_int(panel_x, scene_x + 4, scene_x + scene_w - PROF_PANEL_W - 4);
    panel_y = clamp_profile_y(panel_y, scene_y, scene_h, panel_h);

    if (out_x) *out_x = panel_x;
    if (out_y) *out_y = panel_y;
}

/* ========================================================================= */
/* Helpers                                                                    */
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
    int section_count = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (section_visible((ProfSection)section_idx))
            section_count++;
    }
    return section_count;
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

void ui_profile_panel_render(void) {
    if (*repl_state_profile_panel()->mode == PROFILE_PANEL_OFF) return;

    int visible_count = visible_section_count();

    /* Panel total height: header + column headings + one row per section +
     * divider before FRAME_TOTAL + bottom padding */
    int panel_h  = PROF_HEADER_H
                 + 18                           /* column heading row */
                 + visible_count * PROF_ROW_H
                 + 4                            /* divider before FRAME_TOTAL */
                 + PROF_BOTTOM_PAD
                 + PROF_PANEL_MARGIN;

    int panel_x, panel_y;
    profile_panel_rect_for_height(panel_h, &panel_x, &panel_y);

    gl2d_begin(*repl_state_viewport()->window_w, *repl_state_viewport()->window_h);

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.05f, 0.05f, 0.08f, 0.91f);
    glRectf((float)((float)panel_x), (float)((float)panel_y), (float)((float)panel_x)+(float)((float)PROF_PANEL_W), (float)((float)panel_y)+(float)panel_h);

    /* Border */
    glColor4f(0.35f, 0.35f, 0.55f, 0.85f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)panel_x,                   (float)panel_y);
    glVertex2f((float)(panel_x + PROF_PANEL_W),  (float)panel_y);
    glVertex2f((float)(panel_x + PROF_PANEL_W),  (float)(panel_y + panel_h));
    glVertex2f((float)panel_x,                   (float)(panel_y + panel_h));
    glEnd();
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - PROF_HEADER_H + 2;
    const char *HINT = "Ctrl+W:hide";
    const int hint_width = FONT_SMALL_W * (int)strlen(HINT) + 2;

    /* Title */
    glColor3f(0.85f, 0.90f, 1.00f);
    gl2d_draw_string((float)tx, (float)ty, "CPU Profile", FONT_SMALL);
    glColor3f(0.40f, 0.42f, 0.50f);
    gl2d_draw_string((float)(panel_x + PROF_PANEL_W - hint_width), (float)ty, HINT, FONT_SMALL);

    ty -= PROF_HEADER_H;

    /* Column headings */
    int col_last = tx + PROF_COL_LABEL_W;
    int col_avg  = col_last + PROF_COL_LAST_W;

    glColor3f(0.45f, 0.50f, 0.62f);
    gl2d_draw_string((float)tx,        (float)ty, "Section",  FONT_SMALL);
    gl2d_draw_string((float)col_last,  (float)ty, "Last",     FONT_SMALL);
    gl2d_draw_string((float)col_avg,   (float)ty, "Avg",      FONT_SMALL);
    ty -= 2;

    /* Thin rule under headings */
    glColor4f(0.25f, 0.25f, 0.40f, 0.80f);
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
        if (!section_visible(s)) continue;

        if (s == PROF_FRAME_TOTAL) {
            /* Divider above totals row */
            glColor4f(0.25f, 0.25f, 0.40f, 0.80f);
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
        if (s == PROF_FRAME_TOTAL)
            glColor3f(0.80f, 0.85f, 1.00f);
        else if (stale)
            glColor3f(0.35f, 0.35f, 0.42f);
        else if (is_detail_section(s))
            glColor3f(0.62f, 0.68f, 0.80f);
        else
            glColor3f(0.72f, 0.78f, 0.90f);
        gl2d_draw_string((float)tx, (float)ty, section_label(s), FONT_SMALL);

        /* Last / avg values */
        char last_buf[24], avg_buf[24];
        if (stale) {
            snprintf(last_buf, sizeof(last_buf), "--");
            snprintf(avg_buf,  sizeof(avg_buf),  "--");
            glColor3f(0.30f, 0.30f, 0.38f);
        } else {
            fmt_us(last_buf, (int)sizeof(last_buf), prof_section_last_us(s));
            fmt_us(avg_buf,  (int)sizeof(avg_buf),  prof_section_avg_us(s));
            if (s == PROF_FRAME_TOTAL)
                glColor3f(0.90f, 0.95f, 0.70f);
            else
                glColor3f(0.60f, 0.88f, 0.60f);
        }
        gl2d_draw_string((float)col_last, (float)ty, last_buf, FONT_SMALL);
        gl2d_draw_string((float)col_avg,  (float)ty, avg_buf,  FONT_SMALL);

        ty -= PROF_ROW_H;
    }

    gl2d_end();
}
