/*
 * tools/repl_demo/stubs.c -- No-op shims for editor/UI/controller
 * entry points that the REPL pipeline used to reach for. The file is
 * empty: every stub the decoupling plan listed has been cleared. It
 * stays in the build (linked into repl_demo) as the visible "no stubs
 * needed" sentinel — adding a new stub here is the canary that a
 * pipeline TU has acquired a fresh app/editor/UI/peer dependency. See
 * feature/decouple-repl-from-gl-repl-alt.md.
 *
 * History (each step cleared the listed stubs):
 *   Step 1: repl_compile_dispatch.
 *   Step 2: ui_state_reset, variable_panel_state_reset,
 *           editor_help_session_reset, editor_reset_transients,
 *           ui_state_code_panel_mut.
 *   Step 3: ui_state_status_set (via the ReplHostEffects bridge).
 *   Step 4: g_cfg_items, CFG_ITEM_COUNT, audio_get_cfg_mode,
 *           audio_set_cfg_mode, variable_panel_view_mut,
 *           ui_state_profile_panel_mut (via ReplExportConfigBridge).
 *   Step 6: editor_load_line_to_input (locked by
 *           check-no-load-line-to-input-in-pipeline).
 *   Step 7c: ui_state_viewport, ui_state_code_panel (via
 *           ReplExportLayout); src/ui/core/layout.c left
 *           REPL_DEMO_DEP_SRCS.
 *   Step 7e: editor_feed_line (example loader migrated to
 *           repl_load_apply_line; locked by
 *           check-no-feed-line-in-pipeline);
 *           glr_camera_set_orbit/pan/distance (example loader
 *           migrated to ReplExportCameraBridge); glr_camera.c
 *           left REPL_DEMO_DEP_SRCS.
 *
 *   Step 8: tutorial_teardown moved behind ReplHostEffects as
 *           repl_dispatch_tutorial_teardown; locked by
 *           check-repl-no-direct-tutorial-runner.
 *
 * Demo-side architectural choices that keep this file lean:
 *   - Pipeline diagnostics flow through the ReplHostEffects bridge
 *     (status callback). The demo installs only edit-line hooks, so
 *     status remains unset → set_status() is a silent no-op.
 *   - Cfg state flows through repl_export_install_config_bridge.
 *     The demo doesn't install a bridge → @cfg emission/parsing is a
 *     no-op → no glr_config / audio / peer / profile reach-in.
 *   - Camera state flows through repl_export_install_camera_bridge
 *     (used by both the importer and the example loader). Demo
 *     leaves the bridge unset → camera blocks parse but apply nothing.
 *   - Tutorial teardown requests flow through ReplHostEffects. The
 *     demo leaves that hook unset → no tutorial subsystem is linked.
 */
