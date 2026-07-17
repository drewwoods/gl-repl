/*
 * ui_cpuprof.c - the compute-profile overlay panels.
 *
 * Three independent floating panels live here, each with its own
 * overlay-layout slot:
 *
 *  - the section listing, three time columns per section, all EMAs: CPU (wall
 *    time from support/cpuprof), GPU (timer-query elapsed time from
 *    support/gpuprof; "--" for sections that are CPU-only or when GPU timing
 *    is unavailable), and Max (the worse of the two — the binding cost for
 *    the section);
 *  - the FPS plot, three overlaid time windows off the prof_fps_* history;
 *  - the section histograms, every top-level section's fixed timing histogram
 *    overlaid on one graph. Where the listing collapses a section to one EMA,
 *    this shows the whole distribution: a section that is usually fast but
 *    occasionally stalls looks identical to a uniformly mediocre one in the
 *    Max column, and quite different here.
 */
#include "ui/support/cpuprof.h"
#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"
#include "support/cpuprof.h"
#include "support/gpuprof.h"
#include "c_compat.h"      /* STATIC_ASSERT */

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Dim/stale text tiers: deliberately darker than TEXT_MUTED so stale
 * rows recede - no clean token twin, kept as theme-stable named consts
 * (theme.h "named constant" bucket). The FPS gauge in set_time_color
 * is the separate documented data-viz exclusion (red must stay red). */
static const float k_prof_stale[3] = { 0.35f, 0.35f, 0.42f };
static const float k_prof_dim[3]   = { 0.30f, 0.30f, 0.38f };

/* Filled disclosure markers sized for GLUT_BITMAP_8_BY_13. glBitmap rows are
 * bottom-to-top and MSB-first: the right marker is 5x7, while the down marker
 * is a slightly wider 7x4 triangle raised two pixels from the baseline. */
static const GLubyte k_prof_disclosure_right[7] = {
    0x80, // #.......
    0xc0, // ##......
    0xe0, // ###.....
    0xf0, // ####....
    0xe0, // ###.....
    0xc0, // ##......
    0x80  // #.......
};
static const GLubyte k_prof_disclosure_down[4] = {
    0x10, 0x38, 0x7c, 0xfe
};

typedef struct {
    GLint swap_bytes;
    GLint lsb_first;
    GLint row_length;
    GLint skip_rows;
    GLint skip_pixels;
    GLint alignment;
} ProfBitmapUnpackState;

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
#define PROF_COL_VAL_W       72

/* Visual indentation per nesting level. Labels arrive un-indented; the panel
 * offsets each row by depth * this, so prof_section_info()'s explicit depth is
 * the single source of truth (restyling the indent never re-classifies a row).
 * FONT_SMALL is fixed-width, so 2 cell widths reads as the prior 2-space step. */
#define PROF_INDENT_W       (FONT_SMALL_W * 2)

STATIC_ASSERT(PROF_SECTION_COUNT <= 64,
              "profile collapse mask must fit 64 bits");

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

/* Establish tightly-packed MSB-first rows once for every disclosure bitmap in
 * the panel. The GLUT string renderer temporarily changes the same state and
 * restores it after each string, so our surrounding state remains active.
 * Keeping this outside the row loop also avoids per-marker state traffic. */
static void disclosure_unpack_begin(ProfBitmapUnpackState *state) {
    glGetIntegerv(GL_UNPACK_SWAP_BYTES, &state->swap_bytes);
    glGetIntegerv(GL_UNPACK_LSB_FIRST, &state->lsb_first);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &state->row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &state->skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &state->skip_pixels);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &state->alignment);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void disclosure_unpack_end(const ProfBitmapUnpackState *state) {
    glPixelStorei(GL_UNPACK_SWAP_BYTES, state->swap_bytes);
    glPixelStorei(GL_UNPACK_LSB_FIRST, state->lsb_first);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, state->row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, state->skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, state->skip_pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, state->alignment);
}

static void draw_disclosure_bitmap(float x, float y, int collapsed) {
    glRasterPos2f(x, y);
    if (collapsed) {
        glBitmap(5, 7, 0.0f, 0.0f, 0.0f, 0.0f,
                 k_prof_disclosure_right);
    } else {
        glBitmap(7, 4, 0.0f, -2.0f, 0.0f, 0.0f,
                 k_prof_disclosure_down);
    }
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

/* A row's visibility is the app's call, expressed through prof_section_info():
 *  - a section with no label (NULL/empty) is not a row at all — it's a catalog
 *    slot this binary doesn't use (a demo instrumenting a subset returns this
 *    for the rest), so it's omitted in every mode;
 *  - a nested row has depth > 0 and can be hidden with its parent branch.
 * The app supplies label/depth, so a new sub-section needs no edit here. */
static int section_has_label(ProfSection s) {
    ProfSectionInfo info = prof_section_info(s);
    return info.label != NULL && info.label[0] != '\0';
}

static UiProfileCollapseMask section_bit(ProfSection s) {
    return ((UiProfileCollapseMask)1) << (unsigned int)s;
}

/* Catalog order plus depth defines the tree: a branch owns the contiguous
 * following run of rows with greater depth. */
static int section_has_children(ProfSection s) {
    ProfSectionInfo parent;
    int section_idx;

    if (!section_has_label(s))
        return 0;
    parent = prof_section_info(s);
    for (section_idx = (int)s + 1; section_idx < PROF_SECTION_COUNT;
         section_idx++) {
        ProfSection child = (ProfSection)section_idx;
        ProfSectionInfo info;
        if (!section_has_label(child))
            continue;
        info = prof_section_info(child);
        if (info.depth <= parent.depth)
            return 0;
        return 1;
    }
    return 0;
}

/* A single visibility walk is shared by sizing, rendering, and hit-testing.
 * collapsed_depth tracks the nearest hidden ancestor while traversing the
 * catalog; the collapsed branch itself remains visible. */
static int section_visibility(UiProfileCollapseMask collapsed_sections,
                              unsigned char visible[PROF_SECTION_COUNT]) {
    int section_count = 0;
    int collapsed_depth = -1;
    int section_idx;

    for (section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        ProfSectionInfo info;
        int row_visible;

        if (!section_has_label(s)) {
            visible[section_idx] = 0;
            continue;
        }
        info = prof_section_info(s);
        if (collapsed_depth >= 0 && info.depth <= collapsed_depth)
            collapsed_depth = -1;

        row_visible = (collapsed_depth < 0);
        if (row_visible && section_has_children(s) &&
            (collapsed_sections & section_bit(s)) != 0)
            collapsed_depth = info.depth;

        visible[section_idx] = (unsigned char)row_visible;
        if (row_visible)
            section_count++;
    }
    return section_count;
}

static UiProfileCollapseMask branch_mask(void) {
    UiProfileCollapseMask mask = 0;
    int section_idx;
    for (section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (section_has_children(s))
            mask |= section_bit(s);
    }
    return mask;
}

static UiProfileCollapseMask root_branch_mask(void) {
    UiProfileCollapseMask mask = 0;
    int section_idx;
    for (section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (section_has_children(s) && prof_section_info(s).depth == 0)
            mask |= section_bit(s);
    }
    return mask;
}

static int tree_is_collapsed(UiProfileCollapseMask collapsed_sections) {
    UiProfileCollapseMask roots = root_branch_mask();
    return roots != 0 && (collapsed_sections & roots) == roots;
}

UiProfileCollapseMask ui_profile_panel_toggle_mask(
        UiProfileCollapseMask collapsed_sections, int toggle_target) {
    if (toggle_target == UI_PROFILE_PANEL_TOGGLE_ALL) {
        UiProfileCollapseMask all = branch_mask();
        if (tree_is_collapsed(collapsed_sections))
            return collapsed_sections & ~all;
        return collapsed_sections | all;
    }
    if (toggle_target >= 0 && toggle_target < PROF_SECTION_COUNT &&
        section_has_children((ProfSection)toggle_target))
        return collapsed_sections ^ section_bit((ProfSection)toggle_target);
    return collapsed_sections;
}

/* ========================================================================= */
/* Rendering                                                                  */
/* ========================================================================= */

/* Panel total height for a given mode: header + column headings + one row per
 * visible section + divider before FRAME_TOTAL + bottom padding. The
 * controller needs this (via ui_profile_panel_height) to resolve the stacked
 * anchor; the renderer uses it to size the frame. */
int ui_profile_panel_height(UiProfilePanelMode mode) {
    return ui_profile_panel_height_collapsed(mode, 0);
}

int ui_profile_panel_height_collapsed(
        UiProfilePanelMode mode,
        UiProfileCollapseMask collapsed_sections) {
    unsigned char visible[PROF_SECTION_COUNT];
    int visible_count;
    if (mode != PROFILE_PANEL_SECTIONS && mode != PROFILE_PANEL_HISTOGRAM)
        return 0;
    visible_count = section_visibility(collapsed_sections, visible);
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
    unsigned char visible[PROF_SECTION_COUNT];
    ProfBitmapUnpackState disclosure_unpack;
    /* The section listing only exists from SECTIONS up; FPS shows just the
     * separate FPS plot panel (ui_fps_panel_render). */
    if (profile_mode != PROFILE_PANEL_SECTIONS &&
        profile_mode != PROFILE_PANEL_HISTOGRAM)
        return;

    int panel_h = ui_profile_panel_height_collapsed(
        (UiProfilePanelMode)profile_mode, view->collapsed_sections);

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
    const char *hint;
    int hint_width;

    hint = tree_is_collapsed(view->collapsed_sections)
         ? "Ctrl+W  [+] all" : "Ctrl+W  [-] all";
    hint_width = FONT_SMALL_W * (int)strlen(hint) + 2;

    /* Title */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "Compute Profile", FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + PROF_PANEL_W - hint_width), (float)ty,
                     hint, FONT_SMALL);

    ty -= PROF_HEADER_H;

    /* Column headings */
    int col_cpu = tx + PROF_COL_LABEL_W;
    int col_gpu = col_cpu + PROF_COL_VAL_W;
    int col_max = col_gpu + PROF_COL_VAL_W;

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)tx,       (float)ty, "Section",  FONT_SMALL);
    gl2d_draw_string((float)col_cpu,  (float)ty, "CPU",      FONT_SMALL);
    gl2d_draw_string((float)col_gpu,  (float)ty, "GPU",      FONT_SMALL);
    gl2d_draw_string((float)col_max,  (float)ty, "Max",      FONT_SMALL);
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

    section_visibility(view->collapsed_sections, visible);
    disclosure_unpack_begin(&disclosure_unpack);

    /* One row per section.  Insert a separator line before FRAME_TOTAL. */
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!visible[section_idx]) continue;

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

        int cpu_ok = !prof_section_is_stale(s);
        int gpu_ok = gpu_prof_section_has_data(s);

        /* Label */
        if (info.is_total)
            ui_clr(UI_TOK_TEXT_PRIMARY);
        else if (!cpu_ok && !gpu_ok)
            glColor3fv(k_prof_stale);
        else if (info.depth > 0)
            ui_clr(UI_TOK_TEXT_SECTION);
        else
            ui_clr(UI_TOK_TEXT_PRIMARY);
        /* Indentation is derived from depth, not baked into the label. The
         * tree reserves one marker cell per row so leaf labels stay aligned
         * with sibling branches. The custom filled markers work across every
         * fixed-function backend, including gl4es/WebGL. */
        {
            int label_x = tx + info.depth * PROF_INDENT_W;
            if (section_has_children(s)) {
                draw_disclosure_bitmap(
                    (float)label_x, (float)ty,
                    (view->collapsed_sections & section_bit(s)) != 0);
            }
            label_x += FONT_SMALL_W;
            gl2d_draw_string((float)label_x, (float)ty, info.label, FONT_SMALL);
        }

        /* CPU / GPU / Max values — all smoothed averages. */
        double cpu_us = prof_section_avg_us(s);
        double gpu_us = gpu_prof_section_avg_us(s);
        char val_buf[24];

        if (cpu_ok) {
            fmt_us(val_buf, (int)sizeof(val_buf), cpu_us);
            set_time_color(info.is_total, cpu_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_cpu, (float)ty, val_buf, FONT_SMALL);

        if (gpu_ok) {
            fmt_us(val_buf, (int)sizeof(val_buf), gpu_us);
            set_time_color(info.is_total, gpu_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_gpu, (float)ty, val_buf, FONT_SMALL);

        /* NOTE: Max takes the worse of the two independently-smoothed EMAs
         * (and the GPU EMA runs 1-3 frames behind the CPU one, since query
         * results are harvested asynchronously), not the average of paired
         * per-frame worst cases — pairing each CPU sample with its GPU twin
         * would mean holding samples back until the query resolves, for a
         * distinction that only shows when CPU and GPU spikes anti-correlate
         * frame-to-frame. Not strictly correct, close enough for a HUD. */
        if (cpu_ok || gpu_ok) {
            double max_us = cpu_ok ? cpu_us : 0.0;
            if (gpu_ok && gpu_us > max_us) max_us = gpu_us;
            fmt_us(val_buf, (int)sizeof(val_buf), max_us);
            set_time_color(info.is_total, max_us);
        } else {
            snprintf(val_buf, sizeof(val_buf), "--");
            glColor3fv(k_prof_dim);
        }
        gl2d_draw_string((float)col_max, (float)ty, val_buf, FONT_SMALL);

        ty -= PROF_ROW_H;
    }

    disclosure_unpack_end(&disclosure_unpack);

    gl2d_end();
}

int ui_profile_panel_hit_test(const UiProfilePanelView *view, int mx, int my) {
    unsigned char visible[PROF_SECTION_COUNT];
    int panel_h;
    int gl_y;
    int tx;
    int ty;
    int section_idx;

    if (view == NULL ||
        (view->mode != PROFILE_PANEL_SECTIONS &&
         view->mode != PROFILE_PANEL_HISTOGRAM) ||
        view->window_w <= 0 || view->window_h <= 0)
        return UI_PROFILE_PANEL_HIT_NONE;

    panel_h = ui_profile_panel_height_collapsed(
        view->mode, view->collapsed_sections);
    gl_y = view->window_h - my;
    if (mx < view->panel_x || mx >= view->panel_x + PROF_PANEL_W ||
        gl_y < view->panel_y || gl_y >= view->panel_y + panel_h)
        return UI_PROFILE_PANEL_HIT_NONE;

    /* The right side of the title row owns the collapse/expand-all control. */
    {
        const char *control = tree_is_collapsed(view->collapsed_sections)
                            ? "[+] all" : "[-] all";
        int control_width = FONT_SMALL_W * (int)strlen(control) + 2;
        if (mx >= view->panel_x + PROF_PANEL_W - control_width &&
            gl_y >= view->panel_y + panel_h - PROF_HEADER_H)
            return UI_PROFILE_PANEL_TOGGLE_ALL;
    }

    tx = view->panel_x + 8;
    if (mx < tx || mx >= tx + PROF_COL_LABEL_W)
        return UI_PROFILE_PANEL_HIT_NONE;

    /* Same baseline progression as ui_profile_panel_render(): title row,
     * headings, rule offset, then one fixed-height band per visible row. */
    ty = view->panel_y + panel_h - PROF_HEADER_H + 2;
    ty -= PROF_HEADER_H;
    ty -= 2;
    ty -= PROF_ROW_H - 2;
    section_visibility(view->collapsed_sections, visible);

    for (section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!visible[section_idx])
            continue;
        if (gl_y >= ty - 3 && gl_y < ty + PROF_ROW_H - 3) {
            if (section_has_children(s))
                return section_idx;
            return UI_PROFILE_PANEL_HIT_NONE;
        }
        ty -= PROF_ROW_H;
    }
    return UI_PROFILE_PANEL_HIT_NONE;
}

/* ========================================================================= */
/* FPS plot panel                                                             */
/* ========================================================================= */

#define FPS_PANEL_W       250   /* matches the variable panel width */
#define FPS_HEADER_H      20
#define FPS_LEGEND_H      14
#define FPS_PLOT_H        90
#define FPS_PLOT_GUTTER   28   /* y-axis label gutter ("240") */
#define FPS_BOTTOM_PAD    10

/* Series identity colors: fixed data-viz semantics (which time window a
 * line is), not theme tokens — they must stay distinct in every scheme. */
static const float k_fps_col_10s[3] = { 0.55f, 0.95f, 0.55f };  /* green */
static const float k_fps_col_1m[3]  = { 0.95f, 0.85f, 0.35f };  /* amber */
static const float k_fps_col_10m[3] = { 0.45f, 0.75f, 1.00f };  /* blue  */

int ui_fps_panel_width(void) {
    return FPS_PANEL_W;
}

int ui_fps_panel_height(void) {
    return FPS_HEADER_H + FPS_LEGEND_H + FPS_PLOT_H + FPS_BOTTOM_PAD + 8;
}

/* One series as a line strip. The newest bucket pins to the right edge and
 * each ring slot is one x step, so a partially-filled window draws only its
 * recent right-hand stretch and grows leftward as history accrues. */
static void fps_draw_series(int window, int plot_x, int plot_y,
                            int plot_w, int plot_h, float y_hi,
                            const float col[3]) {
    float samples[PROF_FPS_HISTORY_CAP];
    int n = prof_fps_history(window, samples, PROF_FPS_HISTORY_CAP);
    if (n < 2 || y_hi <= 0.0f) return;

    glColor4f(col[0], col[1], col[2], 0.95f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < n; i++) {
        float fx = (float)plot_x + (float)plot_w
                 * (float)(PROF_FPS_HISTORY_CAP - n + i)
                 / (float)(PROF_FPS_HISTORY_CAP - 1);
        float v = samples[i];
        if (v < 0.0f) v = 0.0f;
        if (v > y_hi) v = y_hi;
        glVertex2f(fx, (float)plot_y + (v / y_hi) * (float)plot_h);
    }
    glEnd();
}

void ui_fps_panel_render(const UiFpsPanelView *view) {
    if (!view || !view->visible) return;

    int panel_h = ui_fps_panel_height();
    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)FPS_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - FPS_HEADER_H + 2;

    /* Title + smoothed current FPS readout. */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "FPS", FONT_SMALL);
    {
        char now_buf[24];
        snprintf(now_buf, sizeof(now_buf), "%.1f now", prof_fps_current());
        int nw = (int)strlen(now_buf) * FONT_SMALL_W + 2;
        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)(panel_x + FPS_PANEL_W - 8 - nw), (float)ty,
                         now_buf, FONT_SMALL);
    }
    ty -= FPS_LEGEND_H;

    /* Legend: one colored label per window. */
    {
        int lx = tx;
        glColor3fv(k_fps_col_10s);
        gl2d_draw_string((float)lx, (float)ty, "10s", FONT_SMALL);
        lx += 4 * FONT_SMALL_W + 8;
        glColor3fv(k_fps_col_1m);
        gl2d_draw_string((float)lx, (float)ty, "1m", FONT_SMALL);
        lx += 3 * FONT_SMALL_W + 8;
        glColor3fv(k_fps_col_10m);
        gl2d_draw_string((float)lx, (float)ty, "10m", FONT_SMALL);
    }
    ty -= FPS_LEGEND_H;

    /* Plot rect (gutter on the left for y labels). */
    int plot_x = panel_x + FPS_PLOT_GUTTER + 4;
    int plot_w = FPS_PANEL_W - FPS_PLOT_GUTTER - 12;
    int plot_y = panel_y + FPS_BOTTOM_PAD;
    int plot_h = ty - plot_y - 2;
    if (plot_h < 20) plot_h = 20;

    /* Y range: 0 .. a nice ceiling over the worst series sample (at least
     * 60 so an idle plot still reads in familiar FPS terms). */
    float hi = 60.0f;
    float cur = (float)prof_fps_current();
    if (cur > hi) hi = cur;
    int have_data = 0;
    for (int w = 0; w < PROF_FPS_WIN_COUNT; w++) {
        float samples[PROF_FPS_HISTORY_CAP];
        int n = prof_fps_history(w, samples, PROF_FPS_HISTORY_CAP);
        if (n >= 2) have_data = 1;
        for (int i = 0; i < n; i++)
            if (samples[i] > hi) hi = samples[i];
    }
    static const float k_steps[] = { 15.0f, 30.0f, 60.0f, 120.0f, 240.0f, 480.0f };
    float step = k_steps[0];
    for (size_t si = 0; si < sizeof(k_steps) / sizeof(k_steps[0]); si++) {
        step = k_steps[si];
        if (hi * 1.05f / step <= 4.0f) break;
    }
    float y_hi = step;
    while (y_hi < hi * 1.05f) y_hi += step;

    /* Plot frame + gridlines + y labels. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)plot_h,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);
    ui_clr_a(UI_TOK_DIVIDER, 0.30f);
    glBegin(GL_LINES);
    for (float v = step; v < y_hi + step * 0.5f; v += step) {
        float gy = (float)plot_y + (v / y_hi) * (float)plot_h;
        glVertex2f((float)plot_x,            gy);
        glVertex2f((float)(plot_x + plot_w), gy);
    }
    glEnd();

    if (!have_data) {
        glDisable(GL_BLEND);
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        const char *empty = "(collecting)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        gl2d_draw_string((float)plot_x + ((float)plot_w - (float)ew) * 0.5f,
                         (float)plot_y + (float)plot_h * 0.5f,
                         empty, FONT_SMALL);
        gl2d_end();
        return;
    }

    /* Series, slowest window first so the fast 10s line draws on top. */
    fps_draw_series(PROF_FPS_WIN_10M, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_10m);
    fps_draw_series(PROF_FPS_WIN_1M, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_1m);
    fps_draw_series(PROF_FPS_WIN_10S, plot_x, plot_y, plot_w, plot_h,
                    y_hi, k_fps_col_10s);
    glDisable(GL_BLEND);

    /* Y labels right-aligned in the gutter. */
    ui_clr(UI_TOK_TEXT_MUTED);
    for (float v = 0.0f; v < y_hi + step * 0.5f; v += step) {
        char lab[12];
        snprintf(lab, sizeof(lab), "%.0f", (double)v);
        int lw = (int)strlen(lab) * FONT_SMALL_W;
        int gy = plot_y + (int)((v / y_hi) * (float)plot_h);
        gl2d_draw_string((float)(panel_x + FPS_PLOT_GUTTER + 2 - lw),
                         (float)(gy - 4), lab, FONT_SMALL);
    }

    gl2d_end();
}

/* ========================================================================= */
/* Section histogram panel                                                    */
/* ========================================================================= */

#define HIST_PANEL_W       PROFILE_PANEL_W
#define HIST_HEADER_H      20
#define HIST_PLOT_H       104
#define HIST_PLOT_GUTTER   34   /* y-axis label gutter ("10k") */
#define HIST_XAXIS_H       14
#define HIST_LEGEND_ROW_H  12
#define HIST_LEGEND_COLS    2
#define HIST_BOTTOM_PAD     8
#define HIST_MARGIN        12

/* The header's clear-samples control (the histograms are cumulative, unlike
 * the section listing's decaying EMAs, so they need an explicit reset). Text
 * and hit region share this label the same way the profile panel's
 * "[+/-] all" control does. */
#define HIST_RESET_LABEL   "[reset]"

/* The x axis spans the occupied bins, with a floor so a single-bin
 * distribution still gets a drawable span. There is no percentile trim: the
 * bins are log-spaced (see support/cpuprof.h), so the axis is linear in bin
 * index and therefore logarithmic in time. A 100 ms stall then sits a bounded
 * number of bins to the right of a 3 ms frame instead of 30x further along a
 * linear scale — it costs the axis a slice of width rather than flattening
 * every real distribution into the leftmost pixel. That is what lets the panel
 * show the whole distribution, tail included, instead of trimming it away. */
#define HIST_MIN_AXIS_BINS  64

/* Floor for the log-y ceiling. Early in a run the tallest bin holds one or
 * two samples, and scaling to that draws every occupied bin at full height —
 * a presence band rather than a histogram. Pinning the ceiling at 10 lets the
 * bars grow up out of the baseline as samples accumulate, and is inert once
 * any bin passes 10. */
#define HIST_MIN_PEAK  10UL

/* Upper bound on the plot's pixel width, so the per-column envelopes below can
 * be plain stack arrays. */
#define HIST_MAX_COLS  512
STATIC_ASSERT(HIST_PANEL_W - HIST_PLOT_GUTTER - 12 <= HIST_MAX_COLS,
              "histogram plot is wider than the per-column envelope buffer");

/* The plotted set: top-level sections only (see the header). Mirrors
 * section_visible()'s "no label means this binary doesn't use the slot". */
static int hist_series_visible(ProfSection s) {
    ProfSectionInfo info = prof_section_info(s);
    if (info.label == NULL || info.label[0] == '\0')
        return 0;
    return info.depth == 0;
}

static int hist_series_count(void) {
    int series_count = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (hist_series_visible((ProfSection)section_idx))
            series_count++;
    }
    return series_count;
}

/* Series identity colors — the same fixed data-viz exclusion as the FPS
 * plot's three window colors, but generated rather than hand-picked: hue
 * rotates by the golden ratio per series, which keeps any number of catalog
 * sections mutually distinguishable without a palette that has to grow every
 * time a section is added. Value is floored so pure blue still reads on the
 * sunken panel background. */
static void hist_series_color(int ordinal, float rgb[3]) {
    float hue = fmodf((float)ordinal * 0.6180339887f, 1.0f) * 6.0f;
    float x   = 1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (hue < 1.0f) { r = 1.0f; g = x;    }
    else if (hue < 2.0f) { r = x;    g = 1.0f; }
    else if (hue < 3.0f) { g = 1.0f; b = x;    }
    else if (hue < 4.0f) { g = x;    b = 1.0f; }
    else if (hue < 5.0f) { r = x;    b = 1.0f; }
    else                 { r = 1.0f; b = x;    }
    rgb[0] = 0.30f + 0.70f * r;
    rgb[1] = 0.30f + 0.70f * g;
    rgb[2] = 0.30f + 0.70f * b;
}

/* Log-scaled bar height fraction. count 0 -> 0; count == peak -> 1. */
static float hist_bar_frac(unsigned int count, double log_peak) {
    if (count == 0 || log_peak <= 0.0) return 0.0f;
    return (float)(log10((double)count + 1.0) / log_peak);
}

static void hist_fmt_count(char *buf, int buf_sz, unsigned long v) {
    if (v >= 1000UL)
        snprintf(buf, (size_t)buf_sz, "%luk", v / 1000UL);
    else
        snprintf(buf, (size_t)buf_sz, "%lu", v);
}

int ui_histogram_panel_width(void) {
    return HIST_PANEL_W;
}

int ui_histogram_panel_height(void) {
    /* Legend rows come from the catalog, not from which series happen to
     * have samples this frame, so the panel doesn't resize under the stack
     * as sections start and stop running. */
    int legend_rows = (hist_series_count() + HIST_LEGEND_COLS - 1)
                    / HIST_LEGEND_COLS;
    return HIST_HEADER_H
         + HIST_PLOT_H
         + HIST_XAXIS_H
         + legend_rows * HIST_LEGEND_ROW_H
         + HIST_BOTTOM_PAD
         + HIST_MARGIN;
}

int ui_histogram_panel_hit_test(const UiHistogramPanelView *view,
                                int mx, int my) {
    int panel_h;
    int gl_y;
    int control_w;

    if (view == NULL || !view->visible ||
        view->window_w <= 0 || view->window_h <= 0)
        return 0;

    panel_h = ui_histogram_panel_height();
    gl_y = view->window_h - my;

    /* The right side of the title row owns the reset control — same band the
     * profile panel gives its collapse/expand-all control. */
    control_w = FONT_SMALL_W * (int)strlen(HIST_RESET_LABEL) + 2;
    if (mx >= view->panel_x + HIST_PANEL_W - control_w &&
        mx < view->panel_x + HIST_PANEL_W &&
        gl_y >= view->panel_y + panel_h - HIST_HEADER_H &&
        gl_y < view->panel_y + panel_h)
        return 1;
    return 0;
}

int ui_histogram_axis_range(int *lo_bin, int *hi_bin) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];
    int lo = PROF_HISTOGRAM_BIN_COUNT, hi = -1;

    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!hist_series_visible(s)) continue;
        prof_section_histogram(s, bins, PROF_HISTOGRAM_BIN_COUNT);
        for (int bin_idx = 0; bin_idx < PROF_HISTOGRAM_BIN_COUNT; bin_idx++) {
            if (!bins[bin_idx]) continue;
            if (bin_idx < lo) lo = bin_idx;
            if (bin_idx > hi) hi = bin_idx;
        }
    }
    if (hi < 0) return 0;

    /* Widen a too-narrow span symmetrically, then slide it back inside the
     * bin range if that pushed an edge out. */
    int span = hi - lo + 1;
    if (span < HIST_MIN_AXIS_BINS) {
        int grow = (HIST_MIN_AXIS_BINS - span + 1) / 2;
        lo -= grow;
        hi += grow;
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > PROF_HISTOGRAM_BIN_COUNT - 1) {
            lo -= hi - (PROF_HISTOGRAM_BIN_COUNT - 1);
            hi  = PROF_HISTOGRAM_BIN_COUNT - 1;
            if (lo < 0) lo = 0;
        }
    }
    *lo_bin = lo;
    *hi_bin = hi;
    return 1;
}

/* Collapse one series' bins onto the plot's pixel columns, taking the tallest
 * bin per column. The axis spans up to 1024 bins across ~338 px, so several
 * bins share a column; the max (rather than the sum) keeps a column's height
 * on the same "samples in a bin" scale as the y axis and the peak. */
static void hist_series_columns(ProfSection s, int lo_bin, int hi_bin,
                                int cols, unsigned int *out) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];
    int span = hi_bin - lo_bin + 1;

    prof_section_histogram(s, bins, PROF_HISTOGRAM_BIN_COUNT);
    for (int col = 0; col < cols; col++) out[col] = 0;
    for (int bin_idx = lo_bin; bin_idx <= hi_bin; bin_idx++) {
        if (!bins[bin_idx]) continue;
        int col = (bin_idx - lo_bin) * cols / span;
        if (col >= cols) col = cols - 1;
        if (bins[bin_idx] > out[col]) out[col] = bins[bin_idx];
    }
}

void ui_histogram_panel_render(const UiHistogramPanelView *view) {
    if (!view || !view->visible) return;

    int panel_h = ui_histogram_panel_height();
    int panel_x = view->panel_x;
    int panel_y = view->panel_y;

    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];
    int lo_bin = 0, hi_bin = 0;
    int have_data = ui_histogram_axis_range(&lo_bin, &hi_bin);

    /* Tallest single bin of any one series — the log-y ceiling. Per series,
     * not the summed height across series: each series is drawn against this
     * scale independently, so a pooled ceiling would shrink every bar by
     * however many sections happen to share its bin. The same sweep records
     * each series' sample count, which the legend needs to dim sections that
     * never ran. */
    unsigned long peak = 0;
    unsigned long series_samples[PROF_SECTION_COUNT];
    memset(series_samples, 0, sizeof(series_samples));
    if (have_data) {
        for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
            ProfSection s = (ProfSection)section_idx;
            if (!hist_series_visible(s)) continue;
            prof_section_histogram(s, bins, PROF_HISTOGRAM_BIN_COUNT);
            for (int bin_idx = 0; bin_idx < PROF_HISTOGRAM_BIN_COUNT; bin_idx++) {
                series_samples[section_idx] += bins[bin_idx];
                if (bins[bin_idx] > peak) peak = bins[bin_idx];
            }
        }
    }

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)HIST_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    int tx = panel_x + 8;
    int ty = panel_y + panel_h - HIST_HEADER_H + 2;

    /* Title + peak readout + reset control. */
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty, "Section Histograms", FONT_SMALL);
    {
        char meta[40];
        int reset_w = FONT_SMALL_W * (int)strlen(HIST_RESET_LABEL) + 2;
        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)(panel_x + HIST_PANEL_W - reset_w), (float)ty,
                         HIST_RESET_LABEL, FONT_SMALL);
        snprintf(meta, sizeof(meta), "peak %lu", peak);
        int mw = (int)strlen(meta) * FONT_SMALL_W;
        gl2d_draw_string(
            (float)(panel_x + HIST_PANEL_W - reset_w - 8 - mw), (float)ty,
            meta, FONT_SMALL);
    }

    /* Plot rect: legend rows sit under the x-axis labels. */
    int legend_rows = (hist_series_count() + HIST_LEGEND_COLS - 1)
                    / HIST_LEGEND_COLS;
    int plot_x = panel_x + HIST_PLOT_GUTTER + 4;
    int plot_w = HIST_PANEL_W - HIST_PLOT_GUTTER - 12;
    int plot_y = panel_y + HIST_BOTTOM_PAD
               + legend_rows * HIST_LEGEND_ROW_H + HIST_XAXIS_H;
    int plot_h = HIST_PLOT_H;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)plot_h,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);

    if (!have_data || peak == 0) {
        glDisable(GL_BLEND);
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        const char *empty = "(collecting)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        gl2d_draw_string((float)plot_x + ((float)plot_w - (float)ew) * 0.5f,
                         (float)plot_y + (float)plot_h * 0.5f,
                         empty, FONT_SMALL);
        gl2d_end();
        return;
    }

    /* The y ceiling the bars, gridlines and labels all share. "peak" stays the
     * true tallest bin (reported in the header); this is only the scale. */
    unsigned long scale_peak = (peak < HIST_MIN_PEAK) ? HIST_MIN_PEAK : peak;
    double log_peak = log10((double)scale_peak + 1.0);

    /* One column per pixel of plot width: the axis carries up to 1024 bins, so
     * drawing per bin would emit thousands of sub-pixel vertices per series. */
    int cols = plot_w;
    if (cols > HIST_MAX_COLS) cols = HIST_MAX_COLS;
    unsigned int col_h[HIST_MAX_COLS];
    float col_w = (float)plot_w / (float)cols;

    /* Count (y) gridlines behind the series. */
    ui_clr_a(UI_TOK_DIVIDER, 0.30f);
    glBegin(GL_LINES);
    for (unsigned long decade = 1; decade <= scale_peak; decade *= 10UL) {
        float gy = (float)plot_y
                 + hist_bar_frac((unsigned int)decade, log_peak) * (float)plot_h;
        glVertex2f((float)plot_x,          gy);
        glVertex2f((float)(plot_x + plot_w), gy);
    }
    /* Time (x) gridlines: one per decade boundary inside the drawn range. The
     * axis is linear in bin index, and bins are log-spaced, so decades land at
     * even pixel intervals. */
    for (double dec = PROF_HISTOGRAM_MIN_US; dec <= PROF_HISTOGRAM_MAX_US;
         dec *= 10.0) {
        int b = prof_histogram_bin_for_us(dec);
        if (b <= lo_bin || b >= hi_bin) continue;
        float gx = (float)plot_x
                 + (float)plot_w * (float)(b - lo_bin)
                                 / (float)(hi_bin - lo_bin + 1);
        glVertex2f(gx, (float)plot_y);
        glVertex2f(gx, (float)(plot_y + plot_h));
    }
    glEnd();

    /* Filled bars, additively blended: overlaps brighten instead of hiding
     * whichever series happens to draw last, so the plot is order-independent
     * and a section buried under a taller one still shows through. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    int ordinal = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!hist_series_visible(s)) continue;

        float col[3];
        hist_series_color(ordinal++, col);
        hist_series_columns(s, lo_bin, hi_bin, cols, col_h);

        glColor4f(col[0], col[1], col[2], 0.30f);
        glBegin(GL_QUADS);
        for (int c = 0; c < cols; c++) {
            if (col_h[c] == 0) continue;
            float x0 = (float)plot_x + (float)c * col_w;
            float x1 = x0 + col_w;
            if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;   /* sub-pixel columns stay visible */
            float bar_h = hist_bar_frac(col_h[c], log_peak) * (float)plot_h;
            glVertex2f(x0, (float)plot_y);
            glVertex2f(x1, (float)plot_y);
            glVertex2f(x1, (float)plot_y + bar_h);
            glVertex2f(x0, (float)plot_y + bar_h);
        }
        glEnd();
    }

    /* Silhouettes over the fills, back on straight alpha: at a pixel or two
     * per column the additive fills merge into a wash, and the step outline is
     * what keeps each series' shape readable. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ordinal = 0;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSection s = (ProfSection)section_idx;
        if (!hist_series_visible(s)) continue;

        float col[3];
        hist_series_color(ordinal++, col);
        hist_series_columns(s, lo_bin, hi_bin, cols, col_h);

        /* Staircase with equal-height runs collapsed. A per-column strip would
         * emit two vertices for every pixel of every series; almost all of
         * those sit on the empty baseline between the humps, and the profile
         * panel is expensive enough already without redrawing them. */
        glColor4f(col[0], col[1], col[2], 0.85f);
        glBegin(GL_LINE_STRIP);
        unsigned int run_h = col_h[0];
        glVertex2f((float)plot_x,
                   (float)plot_y + hist_bar_frac(run_h, log_peak) * (float)plot_h);
        for (int c = 1; c < cols; c++) {
            if (col_h[c] == run_h) continue;
            float x = (float)plot_x + (float)c * col_w;
            glVertex2f(x, (float)plot_y
                          + hist_bar_frac(run_h, log_peak) * (float)plot_h);
            run_h = col_h[c];
            glVertex2f(x, (float)plot_y
                          + hist_bar_frac(run_h, log_peak) * (float)plot_h);
        }
        glVertex2f((float)(plot_x + plot_w),
                   (float)plot_y + hist_bar_frac(run_h, log_peak) * (float)plot_h);
        glEnd();
    }
    glDisable(GL_BLEND);

    /* Y labels (sample counts, log scale) right-aligned in the gutter. */
    ui_clr(UI_TOK_TEXT_MUTED);
    for (unsigned long decade = 1; decade <= scale_peak; decade *= 10UL) {
        char lab[12];
        hist_fmt_count(lab, (int)sizeof(lab), decade);
        int lw = (int)strlen(lab) * FONT_SMALL_W;
        int gy = plot_y + (int)(hist_bar_frac((unsigned int)decade, log_peak)
                                * (float)plot_h);
        gl2d_draw_string((float)(panel_x + HIST_PLOT_GUTTER + 2 - lw),
                         (float)(gy - 4), lab, FONT_SMALL);
    }

    /* X labels: the axis bounds, plus a centered tick per decade boundary. The
     * bounds are the drawn range's real times, so they read as the actual
     * fastest/slowest sample rather than the histogram's fixed 1us..1s span. */
    {
        int  label_y = plot_y - HIST_XAXIS_H + 2;
        char lab[24];

        fmt_us(lab, (int)sizeof(lab), prof_histogram_bin_lo_us(lo_bin));
        gl2d_draw_string((float)plot_x, (float)label_y, lab, FONT_SMALL);

        fmt_us(lab, (int)sizeof(lab), prof_histogram_bin_hi_us(hi_bin));
        int max_x = plot_x + plot_w - (int)strlen(lab) * FONT_SMALL_W;
        gl2d_draw_string((float)max_x, (float)label_y, lab, FONT_SMALL);

        /* Decade ticks, skipped where they would collide with either bound. */
        for (double dec = PROF_HISTOGRAM_MIN_US; dec <= PROF_HISTOGRAM_MAX_US;
             dec *= 10.0) {
            int b = prof_histogram_bin_for_us(dec);
            if (b <= lo_bin || b >= hi_bin) continue;
            fmt_us(lab, (int)sizeof(lab), dec);
            int lw = (int)strlen(lab) * FONT_SMALL_W;
            int gx = plot_x + plot_w * (b - lo_bin) / (hi_bin - lo_bin + 1);
            int lx = gx - lw / 2;
            if (lx < plot_x + FONT_SMALL_W * 5) continue;   /* clear of the low bound */
            if (lx + lw > max_x - FONT_SMALL_W) continue;   /* clear of the high bound */
            gl2d_draw_string((float)lx, (float)label_y, lab, FONT_SMALL);
        }
    }

    /* Legend: catalog order, filling columns left to right. A series with no
     * samples keeps its slot (so the panel height is stable) but is dimmed. */
    {
        int legend_col_w = (HIST_PANEL_W - 16) / HIST_LEGEND_COLS;
        ordinal = 0;
        for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
            ProfSection s = (ProfSection)section_idx;
            if (!hist_series_visible(s)) continue;

            int row = ordinal / HIST_LEGEND_COLS;
            int col = ordinal % HIST_LEGEND_COLS;
            int lx  = tx + col * legend_col_w;
            int ly  = panel_y + HIST_BOTTOM_PAD
                    + (legend_rows - 1 - row) * HIST_LEGEND_ROW_H;

            float rgb[3];
            hist_series_color(ordinal++, rgb);

            unsigned long samples = series_samples[section_idx];
            if (samples > 0)
                glColor3fv(rgb);
            else
                glColor3fv(k_prof_dim);
            glRectf((float)lx, (float)ly + 2.0f,
                    (float)lx + 7.0f, (float)ly + 9.0f);

            if (samples > 0)
                ui_clr(UI_TOK_TEXT_PRIMARY);
            else
                glColor3fv(k_prof_stale);
            gl2d_draw_string((float)(lx + 12), (float)ly,
                             prof_section_info(s).label, FONT_SMALL);
        }
    }

    gl2d_end();
}
