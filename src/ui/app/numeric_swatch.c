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

#define GAP     1

static const float k_bg[4]        = { 0.18f, 0.20f, 0.25f, 0.92f };
static const float k_border[4]    = { 0.35f, 0.38f, 0.45f, 0.85f };
static const float k_arrow[4]     = { 0.80f, 0.82f, 0.85f, 0.90f };

static void draw_btn(float bx, float by, float bw, float bh, int is_up) {
    float cx, cy, half_w, half_h;

    glColor4fv(k_bg);
    glRectf(bx, by, bx + bw, by + bh);

    glColor4fv(k_border);
    glBegin(GL_LINE_LOOP);
    glVertex2f(bx,      by);
    glVertex2f(bx + bw, by);
    glVertex2f(bx + bw, by + bh);
    glVertex2f(bx,      by + bh);
    glEnd();

    cx = bx + bw * 0.5f;
    cy = by + bh * 0.5f;
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

/* Two-button vertical span: down button bottom + up button top, the pair
 * centered on center_y. Shared by render and hit-test so they agree. */
static void stepper_span(float center_y, float btn_h,
                         float *out_dn, float *out_up) {
    float total_h = btn_h * 2.0f + (float)GAP;
    *out_dn = center_y - total_h * 0.5f;
    *out_up = *out_dn + btn_h + (float)GAP;
}

void ui_stepper_render(float x, float center_y, float btn_w, float btn_h) {
    float by_dn, by_up;
    stepper_span(center_y, btn_h, &by_dn, &by_up);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_btn(x, by_up, btn_w, btn_h, 1);
    draw_btn(x, by_dn, btn_w, btn_h, 0);

    glDisable(GL_BLEND);
}

int ui_stepper_hit(float x, float center_y, float btn_w, float btn_h,
                   int mx, float gl_y) {
    float by_dn, by_up;
    stepper_span(center_y, btn_h, &by_dn, &by_up);

    if ((float)mx < x || (float)mx >= x + btn_w)
        return 0;
    if (gl_y >= by_up && gl_y < by_up + btn_h)
        return 1;
    if (gl_y >= by_dn && gl_y < by_dn + btn_h)
        return -1;
    return 0;
}

/* The inline swatch sits on an 18px code-panel row; its arrows centre 6px
 * above the anchor baseline (preserving the historic placement). */
#define SWATCH_CENTER_Y(anchor_y)  ((anchor_y) + 6.0f)

void ui_numeric_swatch_render(const UiRenderSnapshot *snap) {
    if (!snap || !snap->numeric_swatch.visible)
        return;
    ui_stepper_render(snap->numeric_swatch.anchor_x,
                      SWATCH_CENTER_Y(snap->numeric_swatch.anchor_y),
                      UI_NUMERIC_SWATCH_BTN_W, UI_NUMERIC_SWATCH_BTN_H);
}

UiHit ui_numeric_swatch_hit_test(const UiRenderSnapshot *snap,
                                 int mx, int my) {
    UiHit h = ui_hit_none();
    int dir;

    if (!snap || !snap->numeric_swatch.visible)
        return h;

    dir = ui_stepper_hit(snap->numeric_swatch.anchor_x,
                         SWATCH_CENTER_Y(snap->numeric_swatch.anchor_y),
                         UI_NUMERIC_SWATCH_BTN_W, UI_NUMERIC_SWATCH_BTN_H,
                         mx, (float)(snap->viewport.window_h - my));
    if (dir == 0)
        return h;
    h.kind = UI_HIT_NUMERIC_SWATCH;
    h.item_idx = dir;
    return h;
}

typedef int ui_numeric_swatch_nonempty_tu;
