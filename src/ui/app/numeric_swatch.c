/*
 * src/ui/app/numeric_swatch.c -- Inline numeric stepper renderer + hit-test.
 *
 * Stateless: the controller fills a snapshot field each frame; this file
 * renders two small arrow buttons and classifies pointer hits. No REPL,
 * editor, or subsystem reads.
 */
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"

#include "gl_includes.h"

#define BTN_W   UI_NUMERIC_SWATCH_BTN_W
#define BTN_H   UI_NUMERIC_SWATCH_BTN_H
#define GAP     1
#define MARGIN  4

static const float k_bg[4]        = { 0.18f, 0.20f, 0.25f, 0.92f };
static const float k_border[4]    = { 0.35f, 0.38f, 0.45f, 0.85f };
static const float k_arrow[4]     = { 0.80f, 0.82f, 0.85f, 0.90f };

static void draw_btn(float bx, float by, int is_up) {
    float cx, cy, half_w, half_h;

    glColor4fv(k_bg);
    glRectf(bx, by, bx + BTN_W, by + BTN_H);

    glColor4fv(k_border);
    glBegin(GL_LINE_LOOP);
    glVertex2f(bx,         by);
    glVertex2f(bx + BTN_W, by);
    glVertex2f(bx + BTN_W, by + BTN_H);
    glVertex2f(bx,         by + BTN_H);
    glEnd();

    cx = bx + BTN_W * 0.5f;
    cy = by + BTN_H * 0.5f;
    half_w = 3.0f;
    half_h = 2.5f;

    glColor4fv(k_arrow);
    glBegin(GL_TRIANGLES);
    if (is_up) {
        glVertex2f(cx,            cy + half_h);
        glVertex2f(cx - half_w,   cy - half_h);
        glVertex2f(cx + half_w,   cy - half_h);
    } else {
        glVertex2f(cx,            cy - half_h);
        glVertex2f(cx - half_w,   cy + half_h);
        glVertex2f(cx + half_w,   cy + half_h);
    }
    glEnd();
}

void ui_numeric_swatch_render(const UiRenderSnapshot *snap) {
    float bx, by_up, by_dn;
    int total_h;

    if (!snap || !snap->numeric_swatch.visible)
        return;

    total_h = BTN_H * 2 + GAP;
    bx    = snap->numeric_swatch.anchor_x;
    by_up = snap->numeric_swatch.anchor_y - 3 + (18 - total_h) / 2.0f + BTN_H + GAP;
    by_dn = by_up - BTN_H - GAP;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_btn(bx, by_up, 1);
    draw_btn(bx, by_dn, 0);

    glDisable(GL_BLEND);
}

UiHit ui_numeric_swatch_hit_test(const UiRenderSnapshot *snap,
                                 int mx, int my) {
    UiHit h = ui_hit_none();
    float bx, by_up, by_dn;
    int total_h, gl_y;

    if (!snap || !snap->numeric_swatch.visible)
        return h;

    gl_y = snap->viewport.window_h - my;
    total_h = BTN_H * 2 + GAP;
    bx    = snap->numeric_swatch.anchor_x;
    by_up = snap->numeric_swatch.anchor_y - 3 + (18 - total_h) / 2.0f + BTN_H + GAP;
    by_dn = by_up - BTN_H - GAP;

    if ((float)mx < bx || (float)mx >= bx + BTN_W)
        return h;

    if ((float)gl_y >= by_up && (float)gl_y < by_up + BTN_H) {
        h.kind = UI_HIT_NUMERIC_SWATCH;
        h.item_idx = 1;
        return h;
    }
    if ((float)gl_y >= by_dn && (float)gl_y < by_dn + BTN_H) {
        h.kind = UI_HIT_NUMERIC_SWATCH;
        h.item_idx = -1;
        return h;
    }
    return h;
}

typedef int ui_numeric_swatch_nonempty_tu;
