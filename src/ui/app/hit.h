/*
 * ui_app_hit.h - App-specific hit-test kinds.
 */
#ifndef UI_APP_HIT_H
#define UI_APP_HIT_H

#include "ui/core/hit.h"

typedef enum {
    UI_HIT_CODE_PANEL_CHROME = UI_HIT_CORE_COUNT,
    UI_HIT_CODE_FOCUS_TOGGLE,
    UI_HIT_HELP_TOGGLE,
    UI_HIT_CODE_PANEL_TAB,
    UI_HIT_INLINE_COLOR_SWATCH,
    UI_HIT_NUMERIC_SWATCH,
    UI_HIT_COLOR_SWATCH,
    UI_HIT_MENU_BUTTON,
    UI_HIT_MENU_ITEM,
    UI_HIT_SUBMENU_ITEM,
    UI_HIT_PIN_BUTTON,
    UI_HIT_VARIABLE_SLIDER,
    UI_HIT_REPLAY_BUTTON,
    UI_HIT_HELP_PANEL,
    UI_HIT_SCENE
} UiAppHitKind;

#endif /* UI_APP_HIT_H */
