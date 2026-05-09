/*
 * tools/repl_demo/stubs.c -- No-op shims for editor/UI/controller entry
 * points that repl_state_reset_all() and a handful of REPL pipeline
 * helpers reach for.
 *
 * This file is the visible record of "what does the REPL pipeline pull
 * in beyond pure pipeline code?" Each stub is a 2-3 line no-op or
 * trivial getter. If this list grows past ~15 entries, the chokepoint
 * (repl_state_reset_all() + set_status thunk) deserves a structural
 * split rather than more stubs.
 *
 * The list is discovered empirically: start with what we know from
 * walking repl_state.c, then build, then add more stubs for whatever
 * the linker reports unresolved. The starting set covers:
 *
 *   - repl_state_reset_all() reset entry points (7)
 *   - status setter thunk
 *   - ui_state code-panel accessor used by repl_state.c
 */

#include "src/ui/state_types.h"   /* ReplCodePanelRuntimeState */

/* --- repl_state_reset_all() chokepoint --------------------------------
 * editor_state_reset, replay_state_reset come from the real TUs in the
 * link set (src/editor/state.c, replay_state.c). The remaining peers
 * are not linked, so we provide no-op stubs for them. */
void ui_state_reset(void) {}
void variable_panel_state_reset(void) {}
void editor_help_session_reset(void) {}

/* Lives in src/editor/input.c in the real build. Resets camera /
 * menu / picker / code-panel-drag transients alongside editor commit
 * transients. The demo doesn't have any of those transients, so a
 * no-op suffices. */
void repl_editor_reset_transients(void) {}

/* repl_state_sync_ui_chrome is defined in repl_state.c (real impl).
 * Don't stub. */

/* --- set_status thunk ------------------------------------------------- */

/* repl_core.c::set_status() forwards to ui_state_status_set. The demo
 * surfaces messages on stderr so a parse error or flatten failure is
 * visible without dragging in src/ui/state.c. */
#include <stdio.h>
void ui_state_status_set(const char *message) {
    if (message && message[0])
        fprintf(stderr, "[status] %s\n", message);
}

/* --- ui_state code-panel mut accessor --------------------------------- */

/* repl_state.c forward-declares this and may dereference it in the
 * presentation-cfg copy path. Provide a file-scope dummy so the call
 * succeeds without linking src/ui/state.c. */
static ReplCodePanelRuntimeState g_dummy_code_panel;
ReplCodePanelRuntimeState *ui_state_code_panel_mut(void) {
    return &g_dummy_code_panel;
}

/* --- editor_commit dispatcher (only as a hard reference) -------------- */

/* repl_compile_toggle_comment in repl_compile.c calls this as the
 * fall-through path. The demo doesn't trigger toggle-comment, but the
 * symbol reference is hard. Returning NO_CHANGE matches the
 * "nothing happened" contract; if the demo grows to call
 * toggle_comment we'd need the real impl. */
#include "repl_compile.h"
ReplCompileResult repl_compile_dispatch(const char *text,
                                        const ReplCompileContext *ctx,
                                        ReplCompiledChange *out,
                                        char *err, int err_size) {
    (void)text; (void)ctx; (void)err; (void)err_size;
    if (out) out->kind = REPL_COMPILED_NO_CHANGE;
    return REPL_COMPILE_OK;
}

/* --- src/editor/input.c entry points (only as hard references) -------- */

/* feed_line is the programmatic commit entry used by file loading and
 * the example loader. Reachable from repl_load_initial_commands and
 * repl_export_load_from_file -- neither of which the demo invokes. */
int feed_line(const char *line) {
    (void)line;
    return 0;
}

/* load_line_to_input is the editor's "navigate to line N" helper.
 * Reachable from repl_reformat_commands and repl_scenes::load_scene_from_slot.
 * The demo doesn't call either. */
void load_line_to_input(int idx) {
    (void)idx;
}

/* --- glr_actions.c data tables (empty descriptors) -------------------- */

/* glr_actions.c is the controller/menu surface; the demo doesn't link
 * it. glr_config.c iterates g_cfg_items[0..CFG_ITEM_COUNT) when
 * applying @cfg directives or resolving config keys. An empty table
 * makes those iterations no-ops and disables config-key application
 * for the demo, which is fine because the static-text samples don't
 * use @cfg. */
#include "glr_config.h"
const GlrConfigItem g_cfg_items[] = { {0} };
const int CFG_ITEM_COUNT = 0;

/* --- audio cfg-mode getters (referenced via glr_config.c) ------------- */

/* glr_config.c routes audio_mode through audio_get_cfg_mode /
 * audio_set_cfg_mode. The demo doesn't link audio.c. The setter is a
 * no-op; the getter returns 0 (off). */
int  audio_get_cfg_mode(void) { return 0; }
void audio_set_cfg_mode(int mode) { (void)mode; }

/* --- src/ui/state.c read accessors (used by ui/layout.c geometry) ----- */

/* src/ui/layout.c reads viewport + code-panel geometry through ui_state
 * accessors. The demo doesn't render anything, but the geometry helpers
 * are reachable from repl_export.c's code-panel dump path. Provide
 * file-scope dummies so the math returns zero-sized rectangles. */
#include "src/ui/state_types.h"

static ReplViewportState g_dummy_viewport;
ReplViewportState ui_state_viewport(void) { return g_dummy_viewport; }

ReplCodePanelRuntimeState ui_state_code_panel(void) {
    return g_dummy_code_panel;
}

static ReplProfilePanelState g_dummy_profile_panel;
ReplProfilePanelState *ui_state_profile_panel_mut(void) {
    return &g_dummy_profile_panel;
}

/* --- variable_panel peer (referenced via glr_config.c) ---------------- */

/* glr_config.c::config_value_ptr resolves the variable_panel visibility
 * flag through this accessor. The demo doesn't render the panel; an
 * always-zero view satisfies the link. */
#include "variable_panel_state.h"
static ReplVariablePanelState g_dummy_variable_panel_view;
ReplVariablePanelState *variable_panel_view_mut(void) {
    return &g_dummy_variable_panel_view;
}
