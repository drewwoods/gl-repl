/*
 * ui_scene_tabs.c -- Scene tab strip renderer + hit-test (see scene_tabs.h).
 *
 * Snapshot-pure: tab/scene content and the hit-test y-flip come from the
 * frozen UiRenderSnapshot; panel geometry comes through the shared
 * ui_layout_code_panel_rect() helper (single source of truth for panel
 * chrome geometry, exactly as menu_bar.c uses it). No state.h, no
 * repl-layer or editor-layer headers - reads/mutates no REPL/editor state.
 */
#include "ui/app/scene_tabs.h"

#include "config.h"        /* FONT_SMALL, FONT_SMALL_W */
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include <math.h>
#include <string.h>

/* File-private tab metrics. */
enum {
    TAB_PAD_X    = 10,  /* horizontal label padding each side */
    TAB_MIN_W    = 44,
    TAB_MAX_W    = 180,
    TAB_CORNER_R = 9,   /* top-corner radius (clamped to the tab) */
    TAB_TOP_GAP  = 3,   /* band px left above the tab so its rounded
                         * top isn't clipped by the menu bar */
};

/* Workspace chip metrics. The chip leads the strip as a breadcrumb root -
 * "<workspace> > [tab][tab]" - so the container is named before the things it
 * contains. It is deliberately NOT tab-shaped (no rounded top, no outline):
 * it is not selectable and must not read as a ninth scene. */
enum {
    CHIP_PAD_X   = 8,
    CHIP_MIN_W   = 52,
    CHIP_MAX_W   = 150,
    CHIP_GAP     = 6,   /* px between the chip's separator and tab 0 */
    CHIP_SEP_W   = 7,   /* width reserved for the ">" breadcrumb glyph */
};

/* Chip label: the workspace name, or the unbound state spelled out. Loose
 * scenes are the state users cannot currently see, so the chip names it
 * instead of going blank. */
static const char *chip_label(const UiRenderSnapshot *snap) {
    return snap->workspace.name[0] ? snap->workspace.name : "no workspace";
}

/* Chip width, including its trailing separator glyph and gap. The chip always
 * shows when the strip shows - its absence would be indistinguishable from an
 * unbound workspace, which is the very distinction it exists to draw. */
static int chip_total_w(const UiRenderSnapshot *snap) {
    int lw = (int)strlen(chip_label(snap)) * FONT_SMALL_W + 2 * CHIP_PAD_X;
    if (lw < CHIP_MIN_W) lw = CHIP_MIN_W;
    if (lw > CHIP_MAX_W) lw = CHIP_MAX_W;
    return lw + CHIP_SEP_W + CHIP_GAP;
}

/* Compute the band rect and per-tab x[]/w[]. Returns the tab count (0 when
 * the panel is hidden or the derived set is empty). cp_x / cp_w / tab_by /
 * tab_bh are the band's outer rectangle (OpenGL bottom-left y). */
static int scene_tabs_rects(const UiRenderSnapshot *snap,
                            int x[UI_SCENE_TAB_CAP],
                            int w[UI_SCENE_TAB_CAP],
                            int *cp_x_out, int *cp_w_out,
                            int *tab_by, int *tab_bh,
                            int *chip_x_out, int *chip_w_out) {
    int cp_x, cp_y, cp_w, cp_h;
    int menu_by, by, bh;
    int count, avail, natural_sum, i, cx;
    int chip_w;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0)
        return 0;

    count = snap->scene_tabs.count;
    if (count <= 0)
        return 0;
    if (count > UI_SCENE_TAB_CAP)
        count = UI_SCENE_TAB_CAP;

    ui_layout_menu_bar_rect(NULL, &menu_by, NULL, NULL);
    bh = TAB_STRIP_H;
    by = menu_by - bh;

    /* Chip width is reserved before the tabs are laid out, so the overflow
     * shrink below divides what is actually left over rather than letting the
     * last tab slide under the chip. */
    chip_w = chip_total_w(snap);
    avail = cp_w - 2 * CODE_MARGIN_X - chip_w;
    if (avail < count) {
        /* Panel too narrow for chip + one px per tab: the tabs are the
         * primary control, so they keep the space and the chip yields. */
        chip_w = 0;
        avail = cp_w - 2 * CODE_MARGIN_X;
        if (avail < count)
            avail = count;
    }

    if (cp_x_out)   *cp_x_out = cp_x;
    if (cp_w_out)   *cp_w_out = cp_w;
    if (tab_by)     *tab_by = by;
    if (tab_bh)     *tab_bh = bh;
    if (chip_x_out) *chip_x_out = cp_x + CODE_MARGIN_X;
    if (chip_w_out) *chip_w_out = chip_w ? chip_w - CHIP_SEP_W - CHIP_GAP : 0;

    natural_sum = 0;
    for (i = 0; i < count; i++) {
        int lw = (int)strlen(snap->scene_tabs.tabs[i].name) * FONT_SMALL_W
                 + 2 * TAB_PAD_X;
        if (lw < TAB_MIN_W) lw = TAB_MIN_W;
        if (lw > TAB_MAX_W) lw = TAB_MAX_W;
        w[i] = lw;
        natural_sum += lw;
    }

    /* Overflow v1: equal-share shrink, hard label truncation at render,
     * no horizontal scroll (max 9 tabs fit any normal panel). */
    if (natural_sum > avail) {
        int share = avail / count;
        for (i = 0; i < count; i++)
            w[i] = share;
    }

    cx = cp_x + CODE_MARGIN_X + chip_w;
    for (i = 0; i < count; i++) {
        x[i] = cx;
        cx += w[i];
    }
    return count;
}

int ui_scene_tabs_chip_rect(const UiRenderSnapshot *snap,
                            int *x, int *y, int *w, int *h) {
    int tx[UI_SCENE_TAB_CAP], tw[UI_SCENE_TAB_CAP];
    int by, bh, chip_x, chip_w;

    if (!snap)
        return 0;
    if (scene_tabs_rects(snap, tx, tw, NULL, NULL, &by, &bh,
                         &chip_x, &chip_w) <= 0)
        return 0;
    if (chip_w <= 0)
        return 0;
    if (x) *x = chip_x;
    if (y) *y = by;
    if (w) *w = chip_w;
    if (h) *h = bh;
    return 1;
}

int ui_scene_tabs_band_h(const UiRenderSnapshot *snap) {
    int x[UI_SCENE_TAB_CAP], w[UI_SCENE_TAB_CAP];
    int by, bh;

    if (!snap)
        return 0;
    if (scene_tabs_rects(snap, x, w, NULL, NULL, &by, &bh, NULL, NULL) <= 0)
        return 0;
    return bh;
}

/* Fill a rect with rounded TOP corners (square bottom - the tab sits on
 * the band's lower edge). The current GL color is used. Convex outline,
 * so GL_POLYGON is fine. r is clamped to the rect. */
static void scene_tabs_fill_round_top(float x, float y, float w, float h,
                                      float r) {
    enum { SEG = 8 };
    const float HALF_PI = 1.57079633f;
    int k;

    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h)        r = h;

    glBegin(GL_POLYGON);
    glVertex2f(x, y);                       /* bottom-left  */
    glVertex2f(x + w, y);                   /* bottom-right */
    glVertex2f(x + w, y + h - r);           /* up the right edge */
    for (k = 1; k <= SEG; k++) {            /* top-right corner: 0->90 */
        float a = (float)k / SEG * HALF_PI;
        glVertex2f(x + w - r + r * cosf(a), y + h - r + r * sinf(a));
    }
    for (k = 0; k <= SEG; k++) {            /* top-left corner: 90->180 */
        float a = HALF_PI + (float)k / SEG * HALF_PI;
        glVertex2f(x + r + r * cosf(a), y + h - r + r * sinf(a));
    }
    glEnd();                                /* closes back to bottom-left */
}

/* Stroke the rounded-top outline of a tab WITHOUT the bottom edge, so
 * tabs visually connect to the content area below. Same corner geometry
 * as scene_tabs_fill_round_top. Uses the current GL color. */
static void scene_tabs_stroke_round_top(float x, float y, float w, float h,
                                        float r) {
    enum { SEG = 8 };
    const float HALF_PI = 1.57079633f;
    int k;

    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h)        r = h;

    glBegin(GL_LINE_STRIP);
    glVertex2f(x + w, y);                   /* bottom-right (open bottom) */
    glVertex2f(x + w, y + h - r);           /* up the right edge */
    for (k = 1; k <= SEG; k++) {            /* top-right corner: 0->90 */
        float a = (float)k / SEG * HALF_PI;
        glVertex2f(x + w - r + r * cosf(a), y + h - r + r * sinf(a));
    }
    for (k = 0; k <= SEG; k++) {            /* top-left corner: 90->180 */
        float a = HALF_PI + (float)k / SEG * HALF_PI;
        glVertex2f(x + r + r * cosf(a), y + h - r + r * sinf(a));
    }
    glVertex2f(x, y);                       /* down to bottom-left */
    glEnd();
}

/* Draw label clipped to max_w pixels (hard char truncation, no ellipsis -
 * idiomatic with menu_bar's max_chars hard limit). */
static void scene_tabs_draw_label(int tx, int ty, const char *name,
                                  int max_w) {
    char buf[UI_SCENE_TAB_NAME_MAX];
    int max_chars = max_w / FONT_SMALL_W;
    int n;

    if (max_chars <= 0)
        return;
    if (max_chars >= (int)sizeof(buf))
        max_chars = (int)sizeof(buf) - 1;
    n = (int)strlen(name);
    if (n > max_chars)
        n = max_chars;
    memcpy(buf, name, (size_t)n);
    buf[n] = '\0';
    gl2d_draw_string((float)tx, (float)ty, buf, FONT_SMALL);
}

/* Draw the leading workspace chip: a flat recessed plate, its label, and the
 * ">" breadcrumb glyph that ties it to the tabs. No-op when the chip yielded
 * its width to the tabs (chip_w == 0).
 *
 * A bound workspace reads in the primary text colour; the unbound state is
 * muted and lower-contrast, so "loose scenes" looks like a gap in the
 * breadcrumb rather than a named container. */
static void scene_tabs_draw_chip(const UiRenderSnapshot *snap,
                                 int chip_x, int chip_w, int by, int bh,
                                 int hover) {
    const char *label;
    int bound;
    float ch;

    if (chip_w <= 0)
        return;
    label = chip_label(snap);
    bound = (snap->workspace.name[0] != '\0');
    ch = (float)(bh - TAB_TOP_GAP);

    ui_clr_a(hover ? UI_TOK_MENU_LABEL_HOVER_BG : UI_TOK_RAISED, 0.9f);
    glRectf((float)chip_x, (float)by, (float)(chip_x + chip_w), (float)by + ch);

    if (hover)
        ui_clr(UI_TOK_TEXT_ON_HILITE);
    else if (bound)
        ui_clr(UI_TOK_TEXT_PRIMARY);
    else
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
    scene_tabs_draw_label(chip_x + CHIP_PAD_X, by + 3, label,
                          chip_w - 2 * CHIP_PAD_X);

    /* Breadcrumb separator, always muted - it is punctuation, not content. */
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(chip_x + chip_w + 1), (float)(by + 3), ">",
                     FONT_SMALL);
}

void ui_scene_tabs_render(const UiRenderSnapshot *snap) {
    int x[UI_SCENE_TAB_CAP], w[UI_SCENE_TAB_CAP];
    int cp_x, cp_w, by, bh, count;
    int chip_x, chip_w;
    int ry, hover, in_band, chip_hover, i;

    if (!snap)
        return;
    count = scene_tabs_rects(snap, x, w, &cp_x, &cp_w, &by, &bh,
                             &chip_x, &chip_w);
    if (count <= 0)
        return;

    /* Hover from the snapshot pointer (window coords -> bottom-left y),
     * same flip the hit-test uses. */
    ry = snap->viewport.window_h - snap->pointer.mouse_y;
    hover = -1;
    chip_hover = 0;
    in_band = (snap->pointer.mouse_x >= cp_x &&
               snap->pointer.mouse_x < cp_x + cp_w &&
               ry >= by && ry < by + bh);
    if (in_band) {
        for (i = 0; i < count; i++) {
            if (snap->pointer.mouse_x >= x[i] &&
                snap->pointer.mouse_x < x[i] + w[i]) {
                hover = i;
                break;
            }
        }
        chip_hover = (chip_w > 0 &&
                      snap->pointer.mouse_x >= chip_x &&
                      snap->pointer.mouse_x < chip_x + chip_w);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Full-width band background: matches the menu strip surface. */
    ui_clr_a(UI_TOK_SURFACE, 0.98f);
    glRectf((float)cp_x, (float)by,
            (float)(cp_x + cp_w), (float)(by + bh));

    scene_tabs_draw_chip(snap, chip_x, chip_w, by, bh, chip_hover);

    for (i = 0; i < count; i++) {
        const UiSceneTab *tab = &snap->scene_tabs.tabs[i];
        int active = tab->active;
        int is_example = (tab->kind == UI_SCENE_TAB_EXAMPLE);
        int tx = x[i] + TAB_PAD_X;
        /* Draw the tab shorter than the reserved band so its rounded
         * top clears the menu bar (band height stays TAB_STRIP_H for
         * the visible-row lockstep; only the drawn tab is inset). */
        float th = (float)(bh - TAB_TOP_GAP);

        /* Active tab: shade the WHOLE tab with a tinted hue (primary accent
         * for user scenes, alternate accent for the built-in example tab) so
         * the highlight is unmistakably the tab - not a thin rule that reads
         * as an underline of the menu row directly above. */
        if (active) {
            if (is_example)
                ui_clr_a(UI_TOK_ACCENT_ALT_DIM, 0.55f);
            else
                ui_clr(UI_TOK_DROPDOWN_ITEM_HOVER_BG);  /* accent selection */
            scene_tabs_fill_round_top((float)x[i], (float)by,
                                      (float)w[i], th,
                                      (float)TAB_CORNER_R);
        } else if (hover == i) {
            ui_clr(UI_TOK_MENU_LABEL_HOVER_BG);
            scene_tabs_fill_round_top((float)x[i], (float)by,
                                      (float)w[i], th,
                                      (float)TAB_CORNER_R);
        }

        /* Rounded outline on EVERY tab (no bottom edge). The active
         * tab's outline keys off its shaded hue (primary / alternate
         * accent); inactive tabs get a neutral edge so they still read
         * as discrete tabs. */
        if (active) {
            if (is_example)
                ui_clr(UI_TOK_ACCENT_ALT);
            else
                ui_clr(UI_TOK_ACCENT);              /* accent outline */
        } else {
            ui_clr(UI_TOK_BORDER);                  /* neutral edge */
        }
        scene_tabs_stroke_round_top((float)x[i], (float)by,
                                    (float)w[i], th,
                                    (float)TAB_CORNER_R);

        /* Label color. Example tabs use the alternate theme family vs the
         * user tabs' neutral, keeping the kinds distinguishable even when
         * no tab is active. */
        if (active || hover == i) {
            ui_clr(UI_TOK_TEXT_ON_HILITE);
        } else if (is_example) {
            ui_clr(UI_TOK_ACCENT_ALT_DIM);
        } else {
            ui_clr(UI_TOK_TEXT_PRIMARY);
        }

        scene_tabs_draw_label(tx, by + 3, tab->name,
                              w[i] - 2 * TAB_PAD_X);
    }
}

UiHit ui_scene_tabs_hit_test(const UiRenderSnapshot *snap, int mx, int my) {
    UiHit h = ui_hit_none();
    int x[UI_SCENE_TAB_CAP], w[UI_SCENE_TAB_CAP];
    int cp_x, cp_w, by, bh, count, ry, i;
    int chip_x, chip_w;

    if (!snap)
        return h;
    count = scene_tabs_rects(snap, x, w, &cp_x, &cp_w, &by, &bh,
                             &chip_x, &chip_w);
    if (count <= 0)
        return h;

    ry = snap->viewport.window_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w || ry < by || ry >= by + bh)
        return h;  /* outside the band -> let other handlers run */

    if (chip_w > 0 && mx >= chip_x && mx < chip_x + chip_w) {
        h.kind = UI_HIT_CODE_PANEL_WORKSPACE_CHIP;
        h.local_x = (float)(mx - chip_x);
        h.local_y = (float)(ry - by);
        return h;
    }

    for (i = 0; i < count; i++) {
        if (mx >= x[i] && mx < x[i] + w[i]) {
            h.kind = UI_HIT_CODE_PANEL_TAB;
            h.item_idx = i;
            h.local_x = (float)(mx - x[i]);
            h.local_y = (float)(ry - by);
            return h;
        }
    }

    /* In-band but off-tab (gaps / right of last tab): consume it inertly
     * so it cannot fall through to a code-text/gutter hit. */
    h.kind = UI_HIT_CODE_PANEL_CHROME;
    h.local_x = (float)(mx - cp_x);
    h.local_y = (float)(ry - by);
    return h;
}
