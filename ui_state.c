#include "ui_state.h"

#include <stddef.h>

static UiState g_ui_state;
static const UiState g_ui_state_defaults = {0};

void ui_state_capture(UiState *snapshot) {
    if (!snapshot)
        return;
    *snapshot = g_ui_state;
}

void ui_state_restore(const UiState *snapshot) {
    if (!snapshot)
        return;
    g_ui_state = *snapshot;
}

void ui_state_reset(void) {
    g_ui_state = g_ui_state_defaults;
}
