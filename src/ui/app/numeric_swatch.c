/*
 * src/ui/app/numeric_swatch.c -- Inline numeric stepper renderer + hit-test.
 *
 * Stateless: the controller fills a snapshot field each frame; this file
 * places the shared gl2d stepper over the code panel and classifies pointer
 * hits on it. No REPL, editor, or subsystem reads. The widget itself lives in
 * ui/core/gl_2d.h - the find bar and the variable panel draw the same one.
 */
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"

#include "gl_includes.h"

/* The inline swatch sits on an 18px code-panel row; its arrows centre 6px
 * above the anchor baseline (preserving the historic placement). At 16x12 the
 * pair is taller than the row and deliberately overhangs it - it floats over
 * the code, unlike the in-row steppers elsewhere. */
#define SWATCH_CENTER_Y(anchor_y)  ((anchor_y) + 6.0f)

void ui_numeric_swatch_render(const UiRenderSnapshot *snap) {
    if (!snap || !snap->numeric_swatch.visible)
        return;
    gl2d_stepper_render(snap->numeric_swatch.anchor_x,
                        SWATCH_CENTER_Y(snap->numeric_swatch.anchor_y),
                        UI_NUMERIC_SWATCH_BTN_W, UI_NUMERIC_SWATCH_BTN_H);
}

UiHit ui_numeric_swatch_hit_test(const UiRenderSnapshot *snap,
                                 int mx, int my) {
    UiHit h = ui_hit_none();
    int dir;

    if (!snap || !snap->numeric_swatch.visible)
        return h;

    dir = gl2d_stepper_hit(snap->numeric_swatch.anchor_x,
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
