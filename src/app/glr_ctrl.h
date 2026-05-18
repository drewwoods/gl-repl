#ifndef GLR_CTRL_H
#define GLR_CTRL_H

#include "ui/repl_code_panel.h"

/* App-frame controller entrypoints. sample.c forwards raw GLUT
 * callbacks here; this module owns frame orchestration, snapshot
 * assembly, and cross-subsystem input routing between editor, UI,
 * camera, replay, config, save/load, and help flows. */

void glr_ctrl_init_gl(void);
void glr_ctrl_bootstrap_repl(const char *input_file);

/* Full-world reset: REPL state + editor + UI + peer subsystems +
 * autocomplete provider registration + UI chrome mirror. Production
 * startup and tests that exercise cross-module behavior call this
 * instead of the REPL-only repl_state_init_defaults / repl_state_reset_program. */
void glr_app_reset_all(void);

/* Mirror chrome-relevant presentation fields from REPL state into
 * UiState.code_panel (layout_mode, show_vertex_indices). The
 * controller calls this once per frame in glr_ctrl_build_ui_snapshot;
 * tests call it after tweaking glr_state_presentation_mut() so
 * subsequent ui_layout_* / ui_panels_hit_test calls see the new
 * chrome state. */
void glr_ctrl_sync_ui_chrome(void);

/* Toggle the code-panel "focus" view (hide derived C boilerplate
 * chrome). Shared by the Ctrl+Shift+F shortcut and the status-bar
 * keycap click so both paths sync chrome, request follow-scroll, and
 * post the same status line. Session-only state, like the F1 help
 * overlay and the Ctrl+N post-filter — no Config row, no @cfg. */
void glr_ctrl_toggle_code_focus(void);

void glr_ctrl_build_ui_snapshot(UiRenderSnapshot *snap);
void glr_ctrl_apply_code_panel_follow_scroll(
    const UiReplCodePanelLayout *layout);
int glr_ctrl_code_panel_apply_scroll_follow_for_test(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines);
void glr_ctrl_set_accum(int enabled);
void glr_ctrl_display_frame(void);
void glr_ctrl_reshape(int w, int h);
void glr_ctrl_keyboard(unsigned char key, int x, int y);
void glr_ctrl_special(int key, int x, int y);
void glr_ctrl_mouse(int button, int state, int x, int y);
void glr_ctrl_motion(int x, int y);
void glr_ctrl_passive_motion(int x, int y);
void glr_ctrl_mousewheel(int wheel, int direction, int x, int y);
void glr_ctrl_timer(int value);

/* Per-frame tick (16 ms). The timer entry above wraps this with
 * glutPostRedisplay + glutTimerFunc reschedule; tests can drive a
 * single tick by calling glr_ctrl_tick directly when GLUT isn't
 * initialised. */
void glr_ctrl_tick(void);

/* ---- Router helpers ----
 *
 * Non-editor input concerns (replay forwarding, audio, config menu,
 * cfg shortcuts, save / debug / quit, scene cycle, help, variable
 * panel drag, scene press, camera, scroll-wheel zoom) are routed by
 * glr_ctrl from raw GLUT events to their owning subsystem. The
 * helpers below are exposed so test fixtures can drive a single
 * routing concern in isolation without applying GLUT effects (they
 * fill the editor_input ReplInputDispatchEffects struct via
 * editor_request_redraw etc., but glutPostRedisplay /
 * glutSetCursor / glutTimerFunc fire only inside
 * glr_ctrl_apply_input_effects, which the router-helper paths
 * deliberately bypass).
 *
 * Each returns 1 if the event was consumed.
 */
int glr_ctrl_router_handle_save_key(unsigned char key);             /* Ctrl+S */
int glr_ctrl_router_handle_debug_dump_key(unsigned char key);       /* Ctrl+P */

/* Fill a ReplExportLayout from current ui_layout_* / glr_state_*
 * values. The export pipeline reads layout as opaque integers, so
 * controllers and full-app tests build the struct through this helper
 * instead of duplicating the current layout/state reads. The
 * decouple-plan note is secondary: this is the app-side adapter that
 * keeps export.c from reaching into ui/layout or glr_state directly. */
#include "repl/export.h"   /* ReplExportLayout */
void glr_ctrl_fill_export_layout(ReplExportLayout *out);
int glr_ctrl_router_handle_quit_key(unsigned char key);             /* Ctrl+Q */
int glr_ctrl_router_handle_config_menu_key(unsigned char key);      /* backtick → config menu */
int glr_ctrl_router_handle_active_replay_key(unsigned char key);    /* replay forwarding when active */
int glr_ctrl_router_handle_replay_toggle_key(unsigned char key);    /* replay key surface, including Ctrl+R */
int glr_ctrl_router_handle_cfg_shortcut_key(unsigned char key);     /* glr_cfg_handle_ascii_shortcut */
int glr_ctrl_router_handle_accum_samples_key(unsigned char key);    /* Ctrl+= / Ctrl+- */
int glr_ctrl_router_handle_post_filter_key(unsigned char key);      /* Ctrl+N (experimental post-process) */
int glr_ctrl_router_handle_code_focus_key(unsigned char key);       /* Ctrl+Shift+F (code-panel focus) */
int glr_ctrl_router_handle_replay_special(int key);                 /* replay-active forwarding */
int glr_ctrl_router_handle_cfg_special_shortcut(int key);           /* cfg shortcut on F-keys */
int glr_ctrl_router_handle_horizontal_audio_special(int key);       /* Ctrl+Left/Right audio */
int glr_ctrl_router_handle_help_tab_special(int key);               /* Left/Right help-tab */
int glr_ctrl_router_handle_help_scroll_special(int key);            /* Up/Down/PageUp/Down when help visible */
int glr_ctrl_router_handle_help_toggle_special(int key);            /* F1 */
int glr_ctrl_router_handle_scene_cycle_special(int key);            /* F12 */

int glr_ctrl_router_handle_variable_panel_drag_begin(int button, int state, int x, int y);
int glr_ctrl_router_handle_variable_panel_drag_release(int state);
int glr_ctrl_router_handle_right_config_press(int button, int state, int x, int y);
int glr_ctrl_router_handle_scene_press(int button, int state, int x, int y);
int glr_ctrl_router_handle_camera_mouse(int button, int state, int x, int y);
int glr_ctrl_router_handle_variable_panel_motion(int x, int y);
int glr_ctrl_router_handle_camera_motion(int x, int y);
int glr_ctrl_router_handle_camera_pointer_set(int x, int y);
int glr_ctrl_router_handle_glut_scroll_wheel_button(int button, int state, int x, int y);

/* Dispatch a code-panel UiHit to the owning subsystem. Switches on
 * hit.kind: code text / insert line / gutter / inline color swatch /
 * panel divider / pin button / menu button / menu item / variable
 * slider / floating color picker control. Returns 1 if the hit was
 * consumed (i.e. dispatched to an owner). The (x, y) screen coords
 * are passed through for helpers that need raw mouse coordinates
 * (e.g. color_picker_open expects screen-space my). */
int glr_ctrl_router_handle_code_panel_hit(UiHit hit, int x, int y);

/* Motion handler for an in-progress code-panel selection drag.
 * The controller owns the drag state (active / anchor / moved); this
 * helper re-runs ui_panels_hit_test on motion to derive the drag
 * target and updates the editor selection. Returns 1 if drag was
 * active (consumed), 0 otherwise. */
int glr_ctrl_router_handle_code_panel_drag(int x, int y);

/* Clear the controller's code-panel drag tracking state. Called
 * by the editor's reset_transients hook so a state reset (Ctrl+L,
 * example load) doesn't leave an orphaned mid-drag. */
void glr_ctrl_router_reset_code_panel_drag(void);

/* Apply an input-row drag motion. With the drag-anchor state armed by
 * a prior code-panel press, if (target_line, target_char) still hits
 * the active edit row, grow the input-buffer selection toward
 * target_char and return 1. Returns 0 when the motion should fall
 * through to the line-range path — drag wandered off the press row,
 * the press wasn't on a code-text row, or no drag is armed. The
 * production drag handler calls this after ui_panels_hit_test; tests
 * call it directly so they don't need to compute pixel coordinates
 * matching the live panel layout. */
int glr_ctrl_router_apply_input_row_drag(int target_line, int target_char);

/* Select the word at (line_idx, char_idx) as an input-buffer
 * selection. Navigates to line_idx first so the word walk runs
 * against that row's canonical text. A char_idx outside a word
 * leaves the cursor placed with no selection. The double-click
 * dispatch routes here; tests call it directly to verify word
 * boundary behavior without faking the click-time clock. */
void glr_ctrl_router_select_word_at(int line_idx, int char_idx);

/* Replace the clock used by route_code_text_hit to detect
 * double-clicks (the duplicate press within DOUBLE_CLICK_MS).
 * Production falls back to glutGet(GLUT_ELAPSED_TIME); tests install
 * a deterministic source. Pass NULL to restore the GLUT-backed clock.
 * Installing a new clock also resets the last-press history. */
void glr_ctrl_router_set_double_click_clock_for_test(
    unsigned int (*clock_ms)(void));

/* Build the app/UI-shaped F1 help content from the neutral REPL help
 * text model. The controller owns this adapter because it composes
 * `repl_help_text` with `ui_tabbed_overlay`. */
struct UiOverlayContent;
const struct UiOverlayContent *glr_ctrl_help_overlay_content(void);

/* Publish a ReplReplayAnnotationOutput to editor_state_virtual_lines.
 * replay_annotations_prepare() fills the output struct and the caller
 * (controller, panels.c, full-app-linked tests) uses this helper to
 * forward those rows to the editor's virtual-line list. The standalone
 * demo doesn't link this path because it has no UI to render
 * annotations to. The source-document migration note is secondary to
 * that current contract. */
#include "repl/replay_annotations.h" /* ReplReplayAnnotationOutput */
void glr_publish_replay_annotations(const ReplReplayAnnotationOutput *out);

/* Program name for user-facing messages. main() forwards argv[0]
 * (basename kept); defaults to "gl-repl" when unset (tests/demos). */
void        glr_ctrl_set_program_name(const char *argv0);
const char *glr_ctrl_program_name(void);

#endif /* GLR_CTRL_H */
