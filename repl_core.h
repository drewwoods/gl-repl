/*
 * repl_core.h - Public API for the REPL core module.
 *
 * The REPL lives on two-level command model: **source commands** (parsed user
 * input, displayed in the code panel) and **flat commands** (expanded copy with
 * for-loops unrolled, functions inlined, if-blocks resolved). The source array
 * is edited directly; the flat array is rebuilt on demand before rendering.
 *
 * Lifecycle:
 *   imrepl_ctrl_init_gl()           once, at app startup, after GL context ready (from imrepl_ctrl.h)
 *   repl_load_example(idx)          or repl_load_user_scene_idx() / repl_load_initial_commands()
 *   repl_keyboard_func()            per keystroke (from GLUT)
 *   repl_special_func()             per F-key/arrow (from GLUT)
 *   imrepl_ctrl_display_frame()     per frame (from GLUT, owned by imrepl_ctrl.h)
 *   imrepl_ctrl_reshape()           on window resize (from GLUT, owned by imrepl_ctrl.h)
 *   repl_timer_func()               polling timer (from GLUT)
 *   repl_save_default_output()      or repl_export_save_output() to persist
 *
 * repl_core.c owns:
 *   - Live source-command array (g_cmds[], up to MAX_COMMANDS)
 *   - Live flat-command array (g_flat_cmds[], rebuilt lazily)
 *   - App-level lifecycle and input routing wrappers
 *
 * Specialized responsibilities delegated to focused modules:
 *   repl_parser.c      - Parse source lines to GLCmd (expression validation, normalization)
 *   repl_flatten.c     - Expand source → flat (for-loops, functions, if-blocks)
 *   repl_executor.c    - Emit GL calls from flat commands
 *   repl_replay.c      - Step-by-step playback state and visualization
 *   repl_editor.c      - Keyboard routing, line editing, undo/redo
 *   repl_export.c      - Save/load source files with metadata
 *
 * Implementation helpers (repl_state.c, repl_search.c, etc.) share state via
 * repl_core_internal.h; keep them out of this header to preserve the module boundary.
 *
 * Everything listed here is safe to call from sample.c, scene_render.c,
 * ui_panels.c, and test binaries.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include <stdio.h>

#include "repl_flatten.h"

/* --- Save / load ------------------------------------------------------- */

/* Write the active user scene (or current example) to ./output.c. */
void repl_save_default_output(void);

/* Load a single .c file containing source commands and camera state.
 * Parses @var/@cfg/@camera metadata headers, feeds lines through the commit
 * pipeline. Sets status message on success or failure.
 * Returns 1 on success, -1 on error. */
int  repl_export_load_from_file(const char *filename);

/* Save active scene or example to a standalone .c file with metadata headers
 * (@var name=value, @cfg key=value, @camera, @scene-name, @workspace-dir).
 * Sets status message on success or failure. */
void repl_export_save_output(const char *filename);

/* Workspace I/O: save every occupied user-scene slot to `<dir>/<slug>.c`.
 * Each slot is flushed with its own @scene-name header. Both functions
 * remember `dir` so single-file exports carry a `@workspace-dir` hint.
 * Sets status message on success or failure.
 * Returns -1 on error, or the number of files saved/loaded. */
int  repl_save_workspace(const char *dir);
int  repl_load_workspace(const char *dir);

/* Query/set the bound workspace directory (persisted across save/load).
 * repl_workspace_dir() returns "" if not bound. repl_set_workspace_dir(NULL)
 * clears the binding. String is copied internally. */
const char *repl_workspace_dir(void);
void repl_set_workspace_dir(const char *dir);

/* --- Command pipeline -------------------------------------------------- */

/* Expand a source program into caller-provided flat buffers. This is the
 * core two-level model: for-loops are unrolled (capped at 100k visits per
 * flatten), function calls are inlined with actual arguments substituted,
 * if-blocks are evaluated with condition predicates. Tests use this to
 * flatten into temporary storage without mutating the live g_flat_cmds[];
 * the display loop uses the wrapped repl_flatten_commands() below.
 * Returns the number of flat commands generated, -1 on error. */
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

/* Rebuild g_flat_cmds from g_cmds (idempotent). Expansion honors the
 * laziness flag set by mark_normals_dirty(); call this once per frame
 * before execution if the source array changed. */
void repl_flatten_commands(void);

/* Recompute auto-normals for every glBegin/glEnd batch in the source array.
 * Called automatically when source commands are modified. */
void repl_recompute_autonormals(void);

/* --- Example library & user scene -------------------------------------- */
#ifndef MAX_USER_SCENES
#define MAX_USER_SCENES      8
#endif
#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

/* Query the built-in example library. Examples are immutable snapshots
 * of documented GL patterns; editing an example auto-promotes it to a
 * user scene on first edit (via repl_promote_example_if_needed()). */
int  repl_example_count(void);
const char *repl_example_name(int idx);
void repl_load_example(int idx);

/* User scenes: persistent named snapshots (up to MAX_USER_SCENES slots).
 * Slot 0 is the "home" scene (captured on first example load) and is never
 * evicted by LRU. When every non-home, non-active slot is full and a new
 * promotion happens, the LRU slot is flushed to the workspace directory
 * (if bound) and reused. The active slot is the one currently loaded into
 * g_cmds[]; -1 means an example or fresh workspace is active instead.
 * Editing resets the "last touched" timestamp for LRU eviction.
 */

int  repl_user_scene_valid(void);         /* 1 if any slot occupied */
void repl_load_user_scene(void);          /* load slot 0 (back-compat) */
int  repl_user_scene_count(void);         /* number of occupied slots */
int  repl_user_scene_slot_used(int slot); /* 1 if slot is occupied */
const char *repl_user_scene_name(int slot);
int  repl_user_scene_rename(int slot, const char *new_name);
int  repl_load_user_scene_idx(int slot);  /* load slot, returns 1 on success */
int  repl_active_user_scene(void);        /* current slot index, -1 if none */

/* --- Replay ------------------------------------------------------------ */
void repl_replay_start(void);
void repl_replay_stop(void);

/* --- Editor / navigation helpers called from outside repl_core.c ------- */
void repl_navigate_to_line(int target);
void repl_load_initial_commands(const char *import_file);
void repl_reformat_commands(void);

/* --- Debug dumps (used by tests / CLI flags) --------------------------- */
void repl_debug_dump_editor(FILE *out);
void repl_debug_dump_flat_commands(FILE *out);

/* --- Cursor / feed queries --------------------------------------------- */
int  repl_flat_cmd_matches_cursor(int flat_idx);
int  repl_find_feeding_normal_cmd(int line_idx);
int  repl_find_feeding_color_cmd(int line_idx);

/* --- GLUT callback entry points (wired in sample.c) -------------------- */

/* ASCII key and Ctrl-key input. Routes to repl_editor.c for editing, or to
 * repl_actions.c for config shortcuts (Ctrl+S, Ctrl+Z, Ctrl+R, etc.). */
void repl_keyboard_func(unsigned char key, int x, int y);

/* Function key (F1-F12) and arrow input. Routes to repl_editor.c and
 * repl_actions.c for visual toggles, help overlay, theme cycling. */
void repl_special_func(int key, int x, int y);

/* Mouse button press/release. Routes to ui_panels.c for code-panel hits,
 * color-picker drag, variable-slider transactions. */
void repl_mouse_func(int button, int state, int x, int y);

/* Mouse motion during drag. Routes to ui_panels.c for in-progress drags. */
void repl_motion_func(int x, int y);

/* Mouse motion without button held. Updates hover state for tooltips. */
void repl_passive_motion_func(int x, int y);

#ifndef USE_GLUT
/* Mousewheel (freeglut only). Routes to repl_camera_controls.c for zoom velocity. */
void repl_mousewheel_func(int wheel, int direction, int x, int y);
#endif

/* Polling timer callback (every ~16ms @ 60 FPS). Ticks animation frame counter,
 * advances replays, updates camera momentum, and posts next timer event. */
void repl_timer_func(int value);

/* --- Test helpers ------------------------------------------------------ */

/* Reset global REPL state (cmds, input, cursor, camera, predef vars, undo/redo,
 * examples, user scenes, etc.) to the same configuration the real binary starts
 * in. Used by every test fixture before each test case. */
void repl_reset_state(void);

/* Public wrapper over the internal feed_line() so test code outside
 * repl_core_internal.h users can drive command commitment end-to-end.
 * Used by integration tests that validate parsing → commit → execute flows. */
void repl_feed_line_public(const char *line);

#endif /* REPL_CORE_H */
