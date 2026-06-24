/*
 * src/ui/app/numeric_swatch.h - Inline numeric stepper renderer and hit-test.
 *
 * Pure UI layer over the numeric_swatch snapshot field in UiRenderSnapshot.
 * Stateless: the controller re-derives visibility every frame.
 */
#ifndef UI_NUMERIC_SWATCH_H
#define UI_NUMERIC_SWATCH_H

#include "ui/app/hit.h"
#include "ui/app/snapshot.h"

#define UI_NUMERIC_SWATCH_BTN_W  16
#define UI_NUMERIC_SWATCH_BTN_H  12

/* Generic vertical two-arrow stepper (up over down), the pair centered on
 * center_y (GL bottom-up) with its left edge at x and each button btn_w x
 * btn_h. Pure: the caller maps the up/down result to its own UiHit kind.
 * Reused by the inline numeric swatch and the find-bar match navigator.
 * ui_stepper_hit returns +1 (up button), -1 (down button), or 0 (miss). */
void ui_stepper_render(float x, float center_y, float btn_w, float btn_h);
int  ui_stepper_hit(float x, float center_y, float btn_w, float btn_h,
                    int mx, float gl_y);

void  ui_numeric_swatch_render(const UiRenderSnapshot *snap);
UiHit ui_numeric_swatch_hit_test(const UiRenderSnapshot *snap,
                                 int mx, int my);

#endif /* UI_NUMERIC_SWATCH_H */
