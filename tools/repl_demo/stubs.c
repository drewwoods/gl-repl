/*
 * tools/repl_demo/stubs.c -- No-op shims for editor/UI/controller entry
 * points that the REPL pipeline still reaches for.
 *
 * This file is the visible record of "what does the REPL pipeline pull
 * in beyond pure pipeline code?" Each stub is a 2-3 line no-op or
 * trivial getter. See feature/decouple-repl-from-gl-repl-alt.md for
 * the dependency table and the step-by-step removal path.
 *
 *   Step 1 cleared `repl_compile_dispatch`.
 *   Step 2 cleared the reset chokepoint (`ui_state_reset`,
 *           `variable_panel_state_reset`, `editor_help_session_reset`,
 *           `repl_editor_reset_transients`, `ui_state_code_panel_mut`).
 *   Step 3 cleared `ui_state_status_set` via a callback sink.
 *   Step 4 cleared `g_cfg_items`, `CFG_ITEM_COUNT`, `audio_get_cfg_mode`,
 *           `audio_set_cfg_mode`, `variable_panel_view_mut`,
 *           `ui_state_profile_panel_mut` by introducing the neutral
 *           `ReplExportConfigBridge` so repl_export.c / repl_scenes.c
 *           no longer call glr_config_*. glr_config.c is no longer in
 *           the demo link set.
 *
 * Pipeline diagnostics flow through repl_set_status_sink. The demo
 * deliberately leaves the sink unset → set_status is a silent no-op.
 *
 * Cfg state likewise flows through repl_export_install_config_bridge.
 * The demo doesn't install a bridge → @cfg emission/parsing is a no-op
 * → no glr_config / audio / peer / profile reach-in. This is the
 * architectural goal of step 4.
 *
 * The remaining stubs cluster as:
 *   - feed_line / load_line_to_input editor input dispatch (steps 5b/6)
 *   - ui_state_viewport / ui_state_code_panel layout reads (step 7)
 */

/* --- src/editor/input.c entry points (only as hard references) -------- *
 *
 * Step 5b cleared feed_line for the import path (repl_export.c) by
 * routing it through repl_load_apply_line. The example loader still
 * uses feed_line because the editor's try_commit_func_def has
 * reorder + comment-relocation behavior the lean loader doesn't
 * replicate; mid-snippet `func0() {` lines need to land at the
 * canonical document-top position, which is editor-specific
 * placement logic. Future cleanup can converge example loading
 * onto the lean loader once that placement gets consolidated. */
int feed_line(const char *line) {
    (void)line;
    return 0;
}

/* load_line_to_input is the editor's "navigate to line N" helper.
 * Reachable from repl_reformat_commands and repl_scenes::load_scene_from_slot.
 * Step 6 will clear this. */
void load_line_to_input(int idx) {
    (void)idx;
}

/* --- src/ui/state.c read accessors (used by ui/layout.c geometry) ----- */

/* src/ui/layout.c reads viewport + code-panel geometry through ui_state
 * accessors. The demo doesn't render anything, but the geometry helpers
 * are reachable from repl_export.c's code-panel dump path. Provide
 * file-scope dummies so the math returns zero-sized rectangles. Step 7
 * of the decouple plan removes these by passing layout values to
 * repl_export as explicit options. */
#include "src/ui/state_types.h"

static ReplViewportState g_dummy_viewport;
ReplViewportState ui_state_viewport(void) { return g_dummy_viewport; }

static ReplCodePanelRuntimeState g_dummy_code_panel;
ReplCodePanelRuntimeState ui_state_code_panel(void) {
    return g_dummy_code_panel;
}
