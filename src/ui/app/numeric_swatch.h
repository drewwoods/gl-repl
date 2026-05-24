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

void  ui_numeric_swatch_render(const UiRenderSnapshot *snap);
UiHit ui_numeric_swatch_hit_test(const UiRenderSnapshot *snap,
                                 int mx, int my);

#endif /* UI_NUMERIC_SWATCH_H */
