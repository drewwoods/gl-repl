/*
 * ui_app_hit.h - App-specific hit-test kinds.
 */
#ifndef UI_APP_HIT_H
#define UI_APP_HIT_H

#include "ui/core/hit.h"

/* App-level hit kinds extend ui/core/hit.h's enum. Feature-subsystem kinds
 * that a ui/subsystems renderer emits (e.g. UI_HIT_VARIABLE_SLIDER) are
 * owned by that subsystem's own header instead, in a reserved high range
 * off UI_HIT_CORE_COUNT, so the renderer needn't depend on ui/app. */
typedef enum {
    UI_HIT_CODE_PANEL_CHROME = UI_HIT_CORE_COUNT,
    UI_HIT_CODE_CLEAR_ALL,
    UI_HIT_CODE_COPY,
    UI_HIT_CODE_CUT,
    UI_HIT_CODE_PASTE,
    UI_HIT_CODE_UNDO,
    UI_HIT_CODE_REDO,
    UI_HIT_CODE_FOCUS_TOGGLE,
    UI_HIT_CODE_AA_STATUS,   /* statusbar "AA off"/"AA 8x"/"Blur 8x" readout */
    UI_HIT_CODE_VERTEX_LABELS_STATUS, /* statusbar "VL off"/etc. readout */
    UI_HIT_CODE_OVERLAY_SCOPE_STATUS, /* statusbar "Sc last"/etc. readout */
    UI_HIT_CODE_POLY_HIGHLIGHT_STATUS, /* statusbar "PH on"/etc. readout */
    UI_HIT_HELP_TOGGLE,
    UI_HIT_CODE_PANEL_TAB,
    UI_HIT_CODE_PANEL_WORKSPACE_CHIP, /* tab strip's leading workspace chip */
    UI_HIT_INLINE_COLOR_SWATCH,
    UI_HIT_NUMERIC_SWATCH,
    UI_HIT_MENU_BUTTON,
    UI_HIT_MENU_ITEM,
    UI_HIT_SUBMENU_ITEM,
    UI_HIT_PIN_BUTTON,
    UI_HIT_REPLAY_BUTTON,
    UI_HIT_HELP_PANEL,
    UI_HIT_STATUS_HISTORY,   /* messages button: toggles recent-message list */
    UI_HIT_SEARCH_NAV,       /* find-bar match stepper: item_idx = +1 next / -1 prev */
    UI_HIT_SEARCH_FOCUS,     /* find-bar field click: item_idx = EditorSearchFocus */
    UI_HIT_SEARCH_WORD_TOGGLE, /* find-bar whole-word chip */
    UI_HIT_SEARCH_REPLACE,   /* replace buttons: item_idx = 0 current / 1 all */
    UI_HIT_PROFILE_SECTION_TOGGLE, /* item_idx = ProfSection or TOGGLE_ALL */
    UI_HIT_HISTOGRAM_RESET,  /* histogram panel header "[reset]" control */
    UI_HIT_HISTOGRAM_SERIES_TOGGLE, /* item_idx = ProfSection; legend swatch click */
    UI_HIT_ASSIGN_PLOT_CLOSE,  /* assignment plot header "[x]" */
    UI_HIT_ASSIGN_PLOT_RATE,   /* assignment plot capture-rate chip */
    UI_HIT_ASSIGN_PLOT_RESET,  /* assignment plot "[reset]" */
    UI_HIT_ASSIGN_PLOT_YSCALE, /* assignment plot "lin"/"log" state chip */
    UI_HIT_ASSIGN_PLOT_EXPAND, /* assignment plot "1x"/"2x" zoom state chip */
    UI_HIT_CONSOLE_CLOSE,      /* console panel header "[x]" */
    UI_HIT_OVERLAY_CHROME,   /* visible floating/status panel surface; inert */
    UI_HIT_SCENE
} UiAppHitKind;

#endif /* UI_APP_HIT_H */
