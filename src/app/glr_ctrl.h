#ifndef GLR_CTRL_H
#define GLR_CTRL_H

#include "app/glr_defaults.h"  /* GlrExampleTagDefault */
#include "ui/app/repl_code_panel.h"

/* App-frame controller entrypoints. gl_repl.c forwards raw GLUT
 * callbacks here; this module owns frame orchestration, snapshot
 * assembly, and cross-subsystem input routing between editor, UI,
 * camera, replay, config, save/load, and help flows. */

void glr_ctrl_init_gl(void);
void glr_ctrl_bootstrap_repl(const char *input_file);
void glr_shutdown(void);
void glr_ctrl_set_time(float value);

/* Park the cursor on source line `line` (0-based, clamped to the
 * document) and load that line into the input buffer — the same state
 * arrowing to the line produces. Startup hook for GLR_EDIT_LINE:
 * cursor-bound overlays (transform guides, vertex labels) can't
 * otherwise be posed on a headless render with no keyboard input. */
void glr_ctrl_set_edit_line(int line);

/* Set the accumulation sample count (one of 1/2/4/8/12/16; anything
 * else warns and is ignored). Startup hook for GLR_ACCUM_PASSES:
 * headless captures raise the AA passes for smoother 3D edges without
 * supersampling — the 2D UI renders outside the accumulation loop, so
 * text keeps its full size. */
void glr_ctrl_set_accum_passes(int count);

/* Apply tag-keyed presentation defaults from a (table, n) policy.
 * For each entry whose tag bit is set in `tag_mask`, call glr_config_set
 * in declaration order; if two entries target the same GlrConfigKey for
 * this mask the later one wins and a warning is logged to stderr.
 * Returns the collision count (0 when the policy is conflict-free, >0
 * when at least one key was overwritten — the policy is misconfigured
 * and the warning surfaces it). The controller's example-reset hook
 * passes the shipped k_example_tag_defaults policy; tests pass synthetic
 * tables to exercise the collision path without modifying shipped data. */
int glr_ctrl_apply_tag_defaults(unsigned int tag_mask,
                                 const GlrExampleTagDefault *table,
                                 int n);

/* Full-world reset: REPL state + editor + UI + peer subsystems +
 * autocomplete provider registration + UI chrome mirror. Production
 * startup and tests that exercise cross-module behavior call this
 * instead of the REPL-only repl_state_reset_program. */
void glr_ctrl_reset_all(void);

/* Drop camera / menu / picker / code-panel-drag transient state in
 * addition to the editor commit transients. Called from
 * glr_ctrl_reset_all() and from controller paths that switch examples /
 * scenes so the editor returns to a clean idle posture. Hoisted out
 * of src/editor/input.c per audit #8 — the body reaches into
 * camera / UI / picker / controller state, not editor-text state. */
void glr_ctrl_reset_transients(void);

/* Layout provider installed on the editor at glr_ctrl_init_gl so
 * src/editor/ can query the current code-panel layout without
 * depending on src/app/glr_state.h. */
int glr_ctrl_code_panel_layout_provider(void);

/* Switch the code-panel layout from HIDDEN back to LEFT (and sync UI
 * chrome / close menu / close picker). The editor signals the
 * request via the restore_hidden_code_panel effect flag the
 * controller actualizes after editor_handle_*. Returns 1 if the
 * layout was hidden (and is now restored), 0 if it was already
 * visible. Hoisted out of src/editor/input.c per audit #8 — the
 * body writes glr_state and runs glr_ctrl_sync_ui_chrome. */
int glr_ctrl_restore_hidden_code_panel(void);

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

/* Toggle the F1 help overlay. Shared by the F1 key and the status-bar
 * "F1 help" keycap click so both reset the overlay tab/scroll
 * identically. Session-only, like the code-focus toggle. */
void glr_ctrl_toggle_help(void);
void glr_ctrl_close_help(void);

void glr_ctrl_build_ui_snapshot(UiRenderSnapshot *snap);
void glr_ctrl_apply_code_panel_follow_scroll(
    const UiReplCodePanelLayout *layout);
int glr_ctrl_code_panel_apply_scroll_follow_for_test(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines);
void glr_ctrl_set_accum(int enabled);

/* Request a deferred save-and-quit. Async-signal-safe (sets a sig_atomic_t
 * flag only), so it is used by SIGINT and by UI actions that should share
 * the same recovery-save exit path. The recovery save + exit happen on the
 * normal path in glr_ctrl_tick(). */
void glr_ctrl_request_quit(void);

/* Write the live scene to the recovery file (config.h QUIT_RECOVERY_FILE).
 * Used by the quit safeguard and by Load Workspace before it discards the
 * current in-memory scene. Returns 1 on success, 0 on failure. */
int glr_ctrl_save_recovery_file(void);

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
 * fill the editor_input EditorInputDispatchEffects struct via
 * editor_request_redraw etc., but glutPostRedisplay /
 * glutSetCursor / glutTimerFunc fire only inside
 * glr_ctrl_apply_input_effects, which the router-helper paths
 * deliberately bypass).
 *
 * Each returns 1 if the event was consumed.
 */
int glr_ctrl_router_handle_escape_key(unsigned char key);           /* Escape to close modals */
int glr_ctrl_router_handle_save_key(unsigned char key);             /* Ctrl+S */
int glr_ctrl_router_handle_debug_dump_key(unsigned char key);       /* Ctrl+P */

int glr_ctrl_router_handle_quit_key(unsigned char key);             /* Ctrl+Q */
int glr_ctrl_router_handle_config_menu_key(unsigned char key);      /* backtick → config menu */
int glr_ctrl_router_handle_replay_key(unsigned char key);    /* replay key surface excluding config-owned Ctrl+R */
int glr_ctrl_router_handle_cfg_shortcut_key(unsigned char key);     /* glr_cfg_handle_ascii_shortcut */
int glr_ctrl_router_handle_accum_samples_key(unsigned char key);    /* Ctrl+= / Ctrl+- */
int glr_ctrl_router_handle_post_filter_key(unsigned char key);      /* Ctrl+N (experimental post-process) */
int glr_ctrl_router_handle_code_focus_key(unsigned char key);       /* Ctrl+Shift+F (code-panel focus) */
int glr_ctrl_router_handle_tutorial_ack_key(unsigned char key);     /* Enter/Tab/Space ack during tutorial SET steps */
int glr_ctrl_router_handle_replay_special(int key);                 /* replay-active forwarding */
int glr_ctrl_router_handle_cfg_special_shortcut(int key);           /* cfg shortcut on F-keys */
int glr_ctrl_router_handle_horizontal_audio_special(int key);       /* Ctrl+Left/Right audio */
int glr_ctrl_router_handle_help_tab_special(int key);               /* Left/Right help-tab */
int glr_ctrl_router_handle_help_scroll_special(int key);            /* Up/Down/PageUp/Down when help visible */
int glr_ctrl_router_handle_help_toggle_special(int key);            /* F1 */
int glr_ctrl_router_handle_help_click(int button, int state, int x, int y); /* tab / click-away */
void glr_ctrl_help_scroll_by(int delta);                            /* clamped help scroll */
int glr_ctrl_router_handle_scene_cycle_special(int key);            /* F12 */
int glr_ctrl_router_handle_export_special(int key);                 /* F11 -> .ply export */

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
 * (e.g. color_picker_start expects screen-space my). */
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

/* Program name for user-facing messages. main() forwards argv[0]
 * (basename kept); defaults to "gl-repl" when unset (tests/demos). */
void        glr_ctrl_set_program_name(const char *argv0);
const char *glr_ctrl_program_name(void);

/* Report that an external caller (e.g. an example's `// camera` block
 * applied via the camera bridge) has set the camera to a new 3D pose.
 * While the user is dwelling in 2D, the saved-3D snapshot used to
 * restore the camera on 2D->3D otherwise stays at the pose captured at
 * 2D entry, so a later return to 3D would override the example's
 * intended angle. In 3D mode this is a no-op (the live camera is
 * authoritative). */
void glr_ctrl_view_record_external_3d_pose(float rx, float ry, float tz);

#endif /* GLR_CTRL_H */
