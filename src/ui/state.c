#include "state.h"
#include "profile_panel.h"
#include "repl_presentation.h"    /* CFG_DEFAULT_CAMERA_ROTATE */

#include <stddef.h>
#include <string.h>

/* Defaults preserve the legacy behavior the pre-migration
 * repl_state_defaults.inc enshrined: pointer button starts at -1
 * (no button held); cursor starts visible so the renderer's blink
 * phase begins ON; camera faces the same orbit/distance the example
 * loader expects on a fresh session; other slices zeroed.
 *
 * variable_panel visibility lives on the variable_panel peer
 * (Phase F commit 31); callers use variable_panel_view /
 * variable_panel_view_mut directly. */
#define UI_STATE_INITIAL                                              \
    {                                                                 \
        .status = { .text = "", .ttl = 0 },                           \
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
        .camera = {                                                   \
            .rx          = 20.0f,                                     \
            .ry          = 30.0f,                                     \
            .dist        = 5.0f,                                      \
            .tx          = 0.0f,                                      \
            .ty          = 0.0f,                                      \
            .tz          = 0.0f,                                      \
            .motion_glow = 0.0f,                                      \
            .auto_rotate = CFG_DEFAULT_CAMERA_ROTATE,                 \
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

/* Phase J7: the legacy `ui_state_variable_panel` / `_mut`
 * forwarders are gone. Callers use `variable_panel_view` /
 * `variable_panel_view_mut` directly. */

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

ReplCameraState ui_state_camera(void) {
    return g_ui_state.camera;
}

ReplCameraState *ui_state_camera_mut(void) {
    return &g_ui_state.camera;
}

ReplCameraState ui_state_camera_snapshot(void) {
    return g_ui_state.camera;
}

void ui_state_camera_set(float rx, float ry, float dist,
                         float tx, float ty, float tz,
                         float motion_glow) {
    g_ui_state.camera.rx = rx;
    g_ui_state.camera.ry = ry;
    g_ui_state.camera.dist = dist;
    g_ui_state.camera.tx = tx;
    g_ui_state.camera.ty = ty;
    g_ui_state.camera.tz = tz;
    g_ui_state.camera.motion_glow = motion_glow;
}

void ui_state_camera_set_orbit(float rx, float ry) {
    g_ui_state.camera.rx = rx;
    g_ui_state.camera.ry = ry;
}

void ui_state_camera_set_pan(float tx, float ty, float tz) {
    g_ui_state.camera.tx = tx;
    g_ui_state.camera.ty = ty;
    g_ui_state.camera.tz = tz;
}

void ui_state_camera_set_distance(float dist) {
    g_ui_state.camera.dist = dist;
}

void ui_state_camera_set_motion_glow(float motion_glow) {
    g_ui_state.camera.motion_glow = motion_glow;
}

void ui_state_camera_reset_default(void) {
    g_ui_state.camera = g_ui_state_defaults.camera;
}

/* Legacy `repl_state_*` forwarders for the slices migrated in Phase 1
 * commit 8 (and the code_panel slice migrated in Phase A commit 12)
 * are defined in repl_state.c rather than here so the
 * check-state-boundaries guard's "no repl_state_*_mut from ui_*.c"
 * rule keeps working. */
