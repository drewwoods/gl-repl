/*
 * src/repl/host_effects.c - Host-installed side-effect bridge.
 */
#include "repl/host_effects.h"

/* Host-effect bridge: a single file-static pointer the controller
 * installs at startup, mirroring export.c's g_export_cfg_bridge /
 * g_export_camera_bridge. NULL bridge = every dispatch no-ops, which
 * is what tests want when they leave it unset. The standalone demo installs
 * only edit-line hooks, clearing the ui_state_status_set / editor / tutorial
 * stubs from tools/repl_demo/stubs.c.
 *
 * These started as individual callbacks; the six installers were consolidated into
 * one struct per docs/plans/partial/src-repl-simplicity-review.md item 2.
 *
 * The status callback's long-term preferred shape is still per-function
 * out-params (err_buf / ReplDiagnostic) on each pipeline entry point.
 * The callback is the "or a callback" branch the decouple plan allows,
 * chosen because the alternative is 48+ test call-site updates for
 * high-traffic public APIs (export_save_output, save_workspace, etc.).
 * Bundling it in the bridge doesn't block that migration (introduced in
 * step 3 of feature/decouple-repl-from-gl-repl-alt.md). */
static const ReplHostEffects *g_host_effects = 0;

void repl_install_host_effects(const ReplHostEffects *effects) {
    g_host_effects = effects;
}

void repl_set_status(const char *msg) {
    if (g_host_effects && g_host_effects->status && msg && msg[0])
        g_host_effects->status(msg);
}

void repl_set_status_error(const char *msg) {
    if (!g_host_effects || !msg || !msg[0])
        return;
    if (g_host_effects->status_error)
        g_host_effects->status_error(msg);
    else if (g_host_effects->status)
        g_host_effects->status(msg);
}

void repl_dispatch_example_presentation_reset(unsigned int tag_mask) {
    if (g_host_effects && g_host_effects->example_presentation_reset)
        g_host_effects->example_presentation_reset(tag_mask);
}

void repl_dispatch_tutorial_presentation_reset(void) {
    if (g_host_effects && g_host_effects->tutorial_presentation_reset)
        g_host_effects->tutorial_presentation_reset();
}

void repl_dispatch_input_reset(void) {
    if (g_host_effects && g_host_effects->input_reset)
        g_host_effects->input_reset();
}

void repl_dispatch_insert_mode_off(void) {
    if (g_host_effects && g_host_effects->insert_mode_off)
        g_host_effects->insert_mode_off();
}

void repl_dispatch_scroll_to_line(int target) {
    if (g_host_effects && g_host_effects->scroll_to_line)
        g_host_effects->scroll_to_line(target);
}

void repl_dispatch_tutorial_teardown(void) {
    if (g_host_effects && g_host_effects->tutorial_teardown)
        g_host_effects->tutorial_teardown();
}

int repl_dispatch_edit_line_get(void) {
    if (g_host_effects && g_host_effects->edit_line_get)
        return g_host_effects->edit_line_get();
    return 0;
}

void repl_dispatch_edit_line_set(int line) {
    if (g_host_effects && g_host_effects->edit_line_set)
        g_host_effects->edit_line_set(line);
}

void repl_dispatch_host_cursor_park(int line, int insert_mode) {
    if (g_host_effects && g_host_effects->host_cursor_park)
        g_host_effects->host_cursor_park(line, insert_mode);
}

void repl_dispatch_completion_clear(void) {
    if (g_host_effects && g_host_effects->completion_clear)
        g_host_effects->completion_clear();
}

void repl_dispatch_completion_update(void) {
    if (g_host_effects && g_host_effects->completion_update)
        g_host_effects->completion_update();
}

const char *repl_dispatch_host_input_get(void) {
    if (g_host_effects && g_host_effects->host_input_get)
        return g_host_effects->host_input_get();
    return "";
}

void repl_dispatch_set_time_playing(int playing) {
    if (g_host_effects && g_host_effects->set_time_playing)
        g_host_effects->set_time_playing(playing);
}
