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
 *   Step 6 cleared `load_line_to_input` by splitting the reformat pass
 *           into a pure REPL helper (`repl_reformat_program`) and an
 *           editor wrapper (`editor_reformat_commands`) and lifting
 *           the post-scene-load editor-input refresh to the
 *           controller. check-no-load-line-to-input-in-pipeline
 *           locks the boundary in.
 *   Step 7c cleared `ui_state_viewport` / `ui_state_code_panel` by
 *           routing layout values through `ReplExportLayout`; the
 *           follow-up dropped `src/ui/layout.c` from REPL_DEMO_DEP_SRCS
 *           since no demo TU calls `ui_layout_*` anymore.
 *
 * Pipeline diagnostics flow through repl_set_status_sink. The demo
 * deliberately leaves the sink unset → set_status is a silent no-op.
 *
 * Cfg state likewise flows through repl_export_install_config_bridge.
 * The demo doesn't install a bridge → @cfg emission/parsing is a no-op
 * → no glr_config / audio / peer / profile reach-in. This is the
 * architectural goal of step 4.
 *
 * Only `feed_line` remains: the example loader still uses it because
 * the editor's `try_commit_func_def` reorder + comment-relocation
 * behavior the lean loader (`repl_load_apply_line`) doesn't replicate.
 * Convergence is queued for a future step.
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

/* load_line_to_input was previously stubbed because repl_reformat_commands
 * (in repl_core.c) and load_scene_from_slot (in repl_scenes.c) called it
 * directly from REPL pipeline TUs. Step 6 of the decoupling plan removed
 * those calls: the editor wrapper editor_reformat_commands lives in
 * src/editor/reformat.c (outside the demo link set) while pipeline-side
 * normalization uses the pure repl_reformat_program; scene loading
 * leaves the input-buffer refresh to the controller after the API
 * returns. The check-no-load-line-to-input-in-pipeline guard locks the
 * stub-free state in. */

/* Step 7c routed viewport / code-panel geometry through the opaque
 * `ReplExportLayout` struct (controller fills before calling export);
 * `repl_export.c` no longer calls `ui_layout_*`, and `src/ui/layout.c`
 * left REPL_DEMO_DEP_SRCS along with its `ui_state_viewport` /
 * `ui_state_code_panel` reads. Both stubs cleared. */
