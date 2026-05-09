/*
 * tools/repl_demo/stubs.c -- No-op shims for editor/UI/controller entry
 * points that the REPL pipeline still reaches for.
 *
 * This file is the visible record of "what does the REPL pipeline pull
 * in beyond pure pipeline code?" Each stub is a 2-3 line no-op or
 * trivial getter. See feature/decouple-repl-from-gl-repl-alt.md for
 * the dependency table and the step-by-step removal path. Step 1
 * cleared `repl_compile_dispatch`. Step 2 cleared the
 * `repl_state_reset_all()` chokepoint stubs (`ui_state_reset`,
 * `variable_panel_state_reset`, `editor_help_session_reset`,
 * `repl_editor_reset_transients`, `ui_state_code_panel_mut`).
 *
 * The remaining stubs cluster as:
 *   - status setter thunk (cleared in step 3)
 *   - feed_line / load_line_to_input editor input dispatch (steps 5b/6)
 *   - glr_config table + audio + peer accessors (step 4)
 *   - ui_state_viewport / ui_state_code_panel layout reads (step 7)
 */

/* --- set_status thunk ------------------------------------------------- */

/* repl_core.c::set_status() forwards to ui_state_status_set. The demo
 * surfaces messages on stderr so a parse error or flatten failure is
 * visible without dragging in src/ui/state.c. */
#include <stdio.h>
void ui_state_status_set(const char *message) {
    if (message && message[0])
        fprintf(stderr, "[status] %s\n", message);
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

static ReplCodePanelRuntimeState g_dummy_code_panel;
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
