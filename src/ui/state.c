#include "state.h"
#include "profile_panel.h"
#include "layout.h"  /* CFG_DEFAULT_PANEL_FRAC */

#include <stddef.h>
#include <string.h>

/* Defaults preserve the behavior the pre-migration
 * src/repl/state_defaults.inc enshrined: pointer button starts at -1
 * (no button held); cursor starts visible so the renderer's blink
 * phase begins ON; camera faces the same orbit/distance the example
 * loader expects on a fresh session; other slices zeroed.
 *
 * variable_panel visibility lives on the variable_panel peer; callers
 * use variable_panel_view / variable_panel_view_mut directly. */
#define UI_STATE_INITIAL                                              \
    {                                                                 \
        .status = { .text = "", .ttl = 0, .kind = UI_STATUS_INFO },   \
        .help = { .visible = 0 },                                     \
        .profile_panel = { .mode = PROFILE_PANEL_OFF },               \
        .viewport = { .window_w = 0, .window_h = 0 },                 \
        .pointer = { .mouse_x = 0, .mouse_y = 0, .mouse_button = -1 },\
        .code_panel = {                                               \
            .panel_frac     = CFG_DEFAULT_PANEL_FRAC,                 \
            .resizing_panel = 0,                                      \
            .cursor_visible = 1,                                      \
            .blink_tick     = 0,                                      \
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

UiStatusState ui_state_status(void) {
    return g_ui_state.status;
}

UiStatusState *ui_state_status_mut(void) {
    return &g_ui_state.status;
}

static void ui_state_status_set_kind(const char *message, int kind) {
    if (!message)
        message = "";
    strncpy(g_ui_state.status.text, message,
            sizeof(g_ui_state.status.text) - 1);
    g_ui_state.status.text[sizeof(g_ui_state.status.text) - 1] = '\0';
    g_ui_state.status.ttl = REPL_STATUS_MESSAGE_TTL;
    g_ui_state.status.kind = kind;
}

void ui_state_status_set(const char *message) {
    ui_state_status_set_kind(message, UI_STATUS_INFO);
}

void ui_state_status_set_error(const char *message) {
    ui_state_status_set_kind(message, UI_STATUS_ERROR);
}

void ui_state_status_clear(void) {
    g_ui_state.status.text[0] = '\0';
    g_ui_state.status.ttl = 0;
    g_ui_state.status.kind = UI_STATUS_INFO;
}

void ui_state_status_tick(void) {
    if (g_ui_state.status.ttl > 0)
        g_ui_state.status.ttl--;
}

UiHelpState ui_state_help(void) {
    return g_ui_state.help;
}

UiHelpState *ui_state_help_mut(void) {
    return &g_ui_state.help;
}

void ui_state_help_reset(void) {
    g_ui_state.help = g_ui_state_defaults.help;
}

/* Variable-panel accessors live on the variable_panel peer:
 * use `variable_panel_view` / `variable_panel_view_mut` directly. */

UiProfilePanelState ui_state_profile_panel(void) {
    return g_ui_state.profile_panel;
}

UiProfilePanelState *ui_state_profile_panel_mut(void) {
    return &g_ui_state.profile_panel;
}

UiViewportState ui_state_viewport(void) {
    return g_ui_state.viewport;
}

UiViewportState *ui_state_viewport_mut(void) {
    return &g_ui_state.viewport;
}

void ui_state_viewport_set_size(int window_w, int window_h) {
    g_ui_state.viewport.window_w = window_w;
    g_ui_state.viewport.window_h = window_h;
}

UiPointerState ui_state_pointer(void) {
    return g_ui_state.pointer;
}

UiPointerState *ui_state_pointer_mut(void) {
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

UiCodePanelRuntimeState ui_state_code_panel(void) {
    return g_ui_state.code_panel;
}

UiCodePanelRuntimeState *ui_state_code_panel_mut(void) {
    return &g_ui_state.code_panel;
}

void ui_state_code_panel_reset(void) {
    g_ui_state.code_panel = g_ui_state_defaults.code_panel;
}

/* Camera accessors moved to glr_camera.c. Storage lives there too;
 * the UiState.camera field is gone (see src/ui/state.h). */

/* The `repl_state_*` forwarders for these UI slices are defined in
 * src/repl/state.c rather than here so the check-state-boundaries
 * guard's "no repl_state_*_mut from ui_*.c" rule keeps working. */
