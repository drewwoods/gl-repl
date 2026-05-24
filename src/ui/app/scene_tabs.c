/*
 * ui_scene_tabs.c -- Scene tab strip renderer + hit-test (see scene_tabs.h).
 *
 * Snapshot-pure: tab/scene content and the hit-test y-flip come from the
 * frozen UiRenderSnapshot; panel geometry comes through the shared
 * ui_layout_code_panel_rect() helper (single source of truth for panel
 * chrome geometry, exactly as menu_bar.c uses it). No state.h, no
 * repl-layer or editor-layer headers — reads/mutates no REPL/editor state.
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

/* Ephemeral-example-tab identity. Deliberately NOT theme tokens: example
 * tabs must stay an amber/warm family across every scheme so they read
 * as distinct from the accent-colored user-scene tabs. (See theme.h:
 * the "named constant" bucket - fixed, non-theme one-offs.) */
static const float k_tab_example_fill[4]      = { 0.260f, 0.190f, 0.080f, 1.0f };
static const float k_tab_example_edge[3]      = { 0.900f, 0.660f, 0.320f };
static const float k_tab_example_label[3]     = { 0.800f, 0.640f, 0.400f };
static const float k_tab_example_label_hot[3] = { 1.000f, 0.930f, 0.800f };

/* Compute the band rect and per-tab x[]/w[]. Returns the tab count (0 when
 * the panel is hidden or the derived set is empty). cp_x / cp_w / tab_by /
 * tab_bh are the band's outer rectangle (OpenGL bottom-left y). */
static int scene_tabs_rects(const UiRenderSnapshot *snap,
                            int x[UI_SCENE_TAB_CAP],
                            int w[UI_SCENE_TAB_CAP],
                            int *cp_x_out, int *cp_w_out,
                            int *tab_by, int *tab_bh) {
    int cp_x, cp_y, cp_w, cp_h;
    int panel_top, menu_by, by, bh;
    int count, avail, natural_sum, i, cx;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0)
        return 0;

    count = snap->scene_tabs.count;
    if (count <= 0)
        return 0;
    if (count > UI_SCENE_TAB_CAP)
        count = UI_SCENE_TAB_CAP;

    panel_top = cp_y + cp_h;
    menu_by = panel_top - CODE_MARGIN_Y - LINE_H;  /* unchanged menu bar */
    bh = TAB_STRIP_H;
    by = menu_by - bh;

    if (cp_x_out) *cp_x_out = cp_x;
    if (cp_w_out) *cp_w_out = cp_w;
    if (tab_by)   *tab_by = by;
    if (tab_bh)   *tab_bh = bh;

    avail = cp_w - 2 * CODE_MARGIN_X;
    if (avail < count)
        avail = count;

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

    cx = cp_x + CODE_MARGIN_X;
    for (i = 0; i < count; i++) {
        x[i] = cx;
        cx += w[i];
    }
    return count;
}

int ui_scene_tabs_band_h(const UiRenderSnapshot *snap) {
    int x[UI_SCENE_TAB_CAP], w[UI_SCENE_TAB_CAP];
    int by, bh;

    if (!snap)
        return 0;
    if (scene_tabs_rects(snap, x, w, NULL, NULL, &by, &bh) <= 0)
        return 0;
    return bh;
}

/* Fill a rect with rounded TOP corners (square bottom — the tab sits on
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

/* Draw label clipped to max_w pixels (hard char truncation, no ellipsis —
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

void ui_scene_tabs_render(const UiRenderSnapshot *snap) {
    int x[UI_SCENE_TAB_CAP], w[UI_SCENE_TAB_CAP];
    int cp_x, cp_w, by, bh, count;
    int ry, hover, i;

    if (!snap)
        return;
    count = scene_tabs_rects(snap, x, w, &cp_x, &cp_w, &by, &bh);
    if (count <= 0)
        return;

    /* Hover from the snapshot pointer (window coords -> bottom-left y),
     * same flip the hit-test uses. */
    ry = snap->viewport.window_h - snap->pointer.mouse_y;
    hover = -1;
    if (snap->pointer.mouse_x >= cp_x &&
        snap->pointer.mouse_x < cp_x + cp_w &&
        ry >= by && ry < by + bh) {
        for (i = 0; i < count; i++) {
            if (snap->pointer.mouse_x >= x[i] &&
                snap->pointer.mouse_x < x[i] + w[i]) {
                hover = i;
                break;
            }
        }
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Full-width band background: matches the menu strip surface. */
    ui_clr_a(UI_TOK_SURFACE, 0.98f);
    glRectf((float)cp_x, (float)by,
            (float)(cp_x + cp_w), (float)(by + bh));

    for (i = 0; i < count; i++) {
        const UiSceneTab *tab = &snap->scene_tabs.tabs[i];
        int active = tab->active;
        int is_example = (tab->kind == UI_SCENE_TAB_EXAMPLE);
        int tx = x[i] + TAB_PAD_X;
        /* Draw the tab shorter than the reserved band so its rounded
         * top clears the menu bar (band height stays TAB_STRIP_H for
         * the visible-row lockstep; only the drawn tab is inset). */
        float th = (float)(bh - TAB_TOP_GAP);

        /* Active tab: shade the WHOLE tab with a tinted hue (green for
         * user scenes, amber for the ephemeral example tab) so the
         * highlight is unmistakably the tab — not a thin rule that
         * reads as an underline of the menu row directly above. */
        if (active) {
            if (is_example)
                glColor4fv(k_tab_example_fill);
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
         * tab's outline keys off its shaded hue (a brighter green /
         * amber); inactive tabs get a neutral edge so they still read
         * as discrete tabs. */
        if (active) {
            if (is_example)
                glColor3fv(k_tab_example_edge);
            else
                ui_clr(UI_TOK_ACCENT);              /* accent outline */
        } else {
            ui_clr(UI_TOK_BORDER);                  /* neutral edge */
        }
        scene_tabs_stroke_round_top((float)x[i], (float)by,
                                    (float)w[i], th,
                                    (float)TAB_CORNER_R);

        /* Label color. Example tabs keep an amber/warm family vs the
         * user tabs' neutral so the kinds stay distinguishable even
         * when no tab is active. */
        if (active || hover == i) {
            if (is_example)
                glColor3fv(k_tab_example_label_hot);
            else
                ui_clr(UI_TOK_TEXT_ON_HILITE);
        } else if (is_example) {
            glColor3fv(k_tab_example_label);
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

    if (!snap)
        return h;
    count = scene_tabs_rects(snap, x, w, &cp_x, &cp_w, &by, &bh);
    if (count <= 0)
        return h;

    ry = snap->viewport.window_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w || ry < by || ry >= by + bh)
        return h;  /* outside the band -> let other handlers run */

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
