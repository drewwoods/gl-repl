#include "ui_state.h"

#include <stddef.h>
#include <string.h>

/* Defaults preserve the legacy behavior the pre-migration
 * repl_state_defaults.inc enshrined: variable panel visible by
 * default; pointer button starts at -1 (no button held); cursor
 * starts visible so the renderer's blink phase begins ON; other
 * slices zeroed. */
#define UI_STATE_INITIAL                                              \
    {                                                                 \
        .status = { .text = "", .ttl = 0 },                           \
        .help = { .visible = 0, .tab_idx = 0, .scroll = 0 },          \
        .variable_panel = { .visible = 1 },                           \
        .profile_panel = { .mode = PROFILE_PANEL_OFF },               \
        .viewport = { .window_w = 0, .window_h = 0 },                 \
        .pointer = { .mouse_x = 0, .mouse_y = 0, .mouse_button = -1 },\
        .code_panel = {                                               \
            .panel_frac     = CFG_DEFAULT_PANEL_FRAC,                 \
            .resizing_panel = 0,                                      \
            .cursor_visible = 1,                                      \
            .blink_tick     = 0,                                      \
            .cursor_px      = 0,                                      \
            .cursor_py      = 0,                                      \
        },                                                            \
    }

static UiState g_ui_state = UI_STATE_INITIAL;
static const UiState g_ui_state_defaults = UI_STATE_INITIAL;

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

ReplStatusState ui_state_status(void) {
    return g_ui_state.status;
}

ReplStatusState *ui_state_status_mut(void) {
    return &g_ui_state.status;
}

void ui_state_status_set(const char *message) {
    if (!message)
        message = "";
    strncpy(g_ui_state.status.text, message,
            sizeof(g_ui_state.status.text) - 1);
    g_ui_state.status.text[sizeof(g_ui_state.status.text) - 1] = '\0';
    g_ui_state.status.ttl = 240;
}

void ui_state_status_clear(void) {
    g_ui_state.status.text[0] = '\0';
    g_ui_state.status.ttl = 0;
}

void ui_state_status_tick(void) {
    if (g_ui_state.status.ttl > 0)
        g_ui_state.status.ttl--;
}

ReplHelpState ui_state_help(void) {
    return g_ui_state.help;
}

ReplHelpState *ui_state_help_mut(void) {
    return &g_ui_state.help;
}

void ui_state_help_reset(void) {
    g_ui_state.help = g_ui_state_defaults.help;
}

ReplVariablePanelState ui_state_variable_panel(void) {
    return g_ui_state.variable_panel;
}

ReplVariablePanelState *ui_state_variable_panel_mut(void) {
    return &g_ui_state.variable_panel;
}

ReplProfilePanelState ui_state_profile_panel(void) {
    return g_ui_state.profile_panel;
}

ReplProfilePanelState *ui_state_profile_panel_mut(void) {
    return &g_ui_state.profile_panel;
}

ReplViewportState ui_state_viewport(void) {
    return g_ui_state.viewport;
}

ReplViewportState *ui_state_viewport_mut(void) {
    return &g_ui_state.viewport;
}

void ui_state_viewport_set_size(int window_w, int window_h) {
    g_ui_state.viewport.window_w = window_w;
    g_ui_state.viewport.window_h = window_h;
}

ReplPointerState ui_state_pointer(void) {
    return g_ui_state.pointer;
}

ReplPointerState *ui_state_pointer_mut(void) {
    return &g_ui_state.pointer;
}

void ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button) {
    g_ui_state.pointer.mouse_x = mouse_x;
    g_ui_state.pointer.mouse_y = mouse_y;
    g_ui_state.pointer.mouse_button = mouse_button;
}

void ui_state_pointer_set_pos(int mouse_x, int mouse_y) {
    g_ui_state.pointer.mouse_x = mouse_x;
    g_ui_state.pointer.mouse_y = mouse_y;
}

void ui_state_pointer_set_button(int mouse_button) {
    g_ui_state.pointer.mouse_button = mouse_button;
}

ReplCodePanelRuntimeState ui_state_code_panel(void) {
    return g_ui_state.code_panel;
}

ReplCodePanelRuntimeState *ui_state_code_panel_mut(void) {
    return &g_ui_state.code_panel;
}

void ui_state_code_panel_reset(void) {
    g_ui_state.code_panel = g_ui_state_defaults.code_panel;
}

/* Legacy `repl_state_*` forwarders for the slices migrated in Phase 1
 * commit 8 (and the code_panel slice migrated in Phase A commit 12)
 * are defined in repl_state.c rather than here so the
 * check-state-boundaries guard's "no repl_state_*_mut from ui_*.c"
 * rule keeps working. */
