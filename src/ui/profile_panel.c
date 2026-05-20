/*
 * ui_profile_panel.c - per-section wall-time profiling overlay panel.
 */
#include "profile_panel.h"
#include "gl_2d.h"
#include "layout.h"
#include "metrics.h"
#include "theme.h"
#include "variable_panel.h"
#include "prof.h"

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

static void profile_panel_rect_for_height(const UiRenderSnapshot *snap, int panel_h, int *out_x, int *out_y) {
    int scene_x, scene_y, scene_w, scene_h;
    int panel_x, panel_y;

    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    if (snap->variable_panel.visible) {
        panel_x = scene_x + scene_w - PROF_PANEL_W - PROF_PANEL_MARGIN;
        panel_y = scene_y + scene_h - panel_h - PROF_PANEL_MARGIN;
    } else {
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect_for_count(snap->variable_panel_vars.count,
                                         &var_x, &var_y, &var_w, &var_h);
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
    case PROF_SCENE_3D_FADE_BATCH_PREP: return "    batch prep";
    case PROF_SCENE_3D_FADE_BATCH_EXEC: return "    batch exec";
    case PROF_SCENE_3D_FADE_BATCH_POST: return "    batch post";
    case PROF_SCENE_3D_HELPERS:      return "  helpers";
    case PROF_SCENE_3D_BACKDROP:     return "    backdrop";
    case PROF_SCENE_3D_GRID:         return "    grid";
    case PROF_SCENE_3D_AXES:         return "    axes";
    case PROF_SCENE_3D_ORBIT_TARGET: return "    orbit target";
    case PROF_SCENE_3D_OVERLAYS: return "  overlays";
    case PROF_SCENE_3D_OVERLAY_OUTLINES: return "    outlines";
    case PROF_SCENE_3D_OVERLAY_BUILD_GUIDES: return "    build guides";
    case PROF_SCENE_3D_OVERLAY_TRANSFORM_GUIDES: return "    xform guides";
    case PROF_SCENE_3D_OVERLAY_NORMALS: return "    normals";
    case PROF_SCENE_3D_OVERLAY_VERTEX_NUMBERS: return "    vertex nums";
    case PROF_SCENE_3D_POST_PROCESS:     return "  postprocess FX";
    case PROF_CODE_PANEL:  return "Code Panel";
    case PROF_CODE_PANEL_LAYOUT:   return "  layout";
    case PROF_CODE_PANEL_CHROME:   return "  chrome";
    case PROF_CODE_PANEL_LINES:    return "  lines";
    case PROF_CODE_PANEL_LINES_STATIC: return "    static";
    case PROF_CODE_PANEL_LINES_BODY:   return "    body";
    case PROF_CODE_PANEL_LINES_BODY_CMDS:    return "      commands";
    case PROF_CODE_PANEL_LINES_BODY_NEWLINE: return "      newline";
    case PROF_CODE_PANEL_LINES_FOOTER: return "    footer";
    case PROF_CODE_PANEL_OVERLAYS: return "  overlays";
    case PROF_UI_PANELS:   return "UI Panels";
    case PROF_SNAPSHOT:                return "Snapshot";
    case PROF_SNAPSHOT_TRANSFORMERS:   return "  transformers";
    case PROF_SNAPSHOT_HIGHLIGHTS:     return "  highlights";
    case PROF_SNAPSHOT_VIRTUAL_LINES:  return "  virtual lines";
    case PROF_SNAPSHOT_PREP:           return "  prep";
    case PROF_SNAPSHOT_SCENE_CONFIG:   return "  scene config";
    case PROF_SNAPSHOT_UI:             return "  ui snapshot";
    case PROF_FLATTEN:        return "Flatten";
    case PROF_REFORMAT:       return "Reformat";
    case PROF_AUTONORMAL:     return "Autonormal";
    case PROF_REPLAY_HUD:     return "Replay HUD";
    case PROF_PROFILE_PANEL:  return "Profile Panel";
    case PROF_FRAME_RESTORE:  return "Frame Restore";
    case PROF_FRAME_TOTAL:    return "Frame Total";
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

/* A "detail" row is an indented child in section_label(): the label
 * indentation reflects true prof-scope enclosure (e.g. PROF_SNAPSHOT_*
 * nest inside PROF_SNAPSHOT, PROF_SCENE_3D_OVERLAY_* inside _OVERLAYS),
 * so deriving detail from the label keeps the two from drifting and a
 * new nested section is a one-line section_label() change.
 *
 * Behavior change vs the previous hand-maintained || list: that list
 * omitted PROF_SNAPSHOT_PREP and PROF_SCENE_3D_OVERLAY_NORMALS, which
 * ARE enclosed children (verified against prof_begin/prof_end
 * nesting). They now fold as detail like their siblings — the
 * intended classification. */
static int is_detail_section(ProfSection s) {
    return section_label(s)[0] == ' ';
}

/* Apply a green/yellow/red color based on section timing thresholds.
 * FRAME_TOTAL uses 1/120s (8.3ms) and 1/60s (16.7ms) breakpoints.
 * All other sections use half those thresholds (4.15ms / 8.3ms). */
/* FPS gauge: green/yellow/red is a fixed data-viz semantic, NOT theme
 * tokens (theme.h bucket 3 - red must read as "over budget" in every
 * scheme; it must not follow the UI accent). */
static void set_time_color(ProfSection s, double us) {
    if (us < 2.0) {
        glColor3f(0.30f, 0.30f, 0.38f);       /* near-zero – same as stale */
        return;
    }
    if (s == PROF_FRAME_TOTAL) {
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

static int section_visible(int profile_mode, ProfSection s) {
    if (profile_mode != PROFILE_PANEL_DETAILS &&
        is_detail_section(s))
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

void ui_profile_panel_render(const UiRenderSnapshot *snap) {
    int profile_mode = snap->profile_panel.mode;
    if (profile_mode == PROFILE_PANEL_OFF) return;

    int visible_count = visible_section_count(profile_mode);

    /* Panel total height: header + column headings + one row per section +
     * divider before FRAME_TOTAL + bottom padding */
    int panel_h  = PROF_HEADER_H
                 + 18                           /* column heading row */
                 + visible_count * PROF_ROW_H
                 + 4                            /* divider before FRAME_TOTAL */
                 + PROF_BOTTOM_PAD
                 + PROF_PANEL_MARGIN;

    int panel_x, panel_y;
    profile_panel_rect_for_height(snap, panel_h, &panel_x, &panel_y);

    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ui_clr_a(UI_TOK_SUNKEN, 0.91f);
    glRectf((float)panel_x, (float)panel_y, (float)panel_x + (float)PROF_PANEL_W, (float)panel_y + (float)panel_h);

    /* Border */
    ui_clr_a(UI_TOK_BORDER, 0.85f);
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

        if (s == PROF_FRAME_TOTAL) {
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
        if (s == PROF_FRAME_TOTAL)
            ui_clr(UI_TOK_TEXT_PRIMARY);
        else if (stale)
            glColor3fv(k_prof_stale);
        else if (is_detail_section(s))
            ui_clr(UI_TOK_TEXT_SECTION);
        else
            ui_clr(UI_TOK_TEXT_PRIMARY);
        gl2d_draw_string((float)tx, (float)ty, section_label(s), FONT_SMALL);

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
            set_time_color(s, prof_section_last_us(s));
            gl2d_draw_string((float)col_last, (float)ty, last_buf, FONT_SMALL);
            set_time_color(s, prof_section_avg_us(s));
            gl2d_draw_string((float)col_avg,  (float)ty, avg_buf,  FONT_SMALL);
        }

        ty -= PROF_ROW_H;
    }

    gl2d_end();
}
