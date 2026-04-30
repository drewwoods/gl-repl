/*
 * repl_core.h - Public REPL facade.
 *
 * Exposes the source/flat command model, persistence helpers, replay hooks,
 * example and user-scene management, and the input callback wrappers that the
 * controller forwards to. Runtime storage lives in repl_state.c and is accessed
 * through repl_state.h; scene/workspace persistence lives in repl_scenes.c.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include <stdio.h>

#include "repl_flatten.h"

/* --- Save / load ------------------------------------------------------- */

/* Write the active user scene (or current example) to ./output.c. */
void repl_save_default_output(void);

/* Load a single .c file containing source commands and camera state.
 * Parses leading workspace metadata, feeds lines through the commit pipeline,
 * and leaves any pending scene/workspace directives in import/export state for
 * the caller to apply. Returns 1 on success, 0 on error. */
int  repl_export_load_from_file(const char *filename);

/* Save active scene or example to a standalone .c file with metadata headers
 * (@var name=value, @cfg key=value, @camera, @scene-name, @workspace-dir).
 * Sets status message on success or failure. */
void repl_export_save_output(const char *filename);

/* Workspace I/O: save every occupied user-scene slot to `<dir>/<slug>.c`.
 * Each slot is flushed with its own @scene-name header. Both functions
 * remember `dir` so single-file exports carry a `@workspace-dir` hint.
 * Sets status message on success or failure.
 * `repl_save_workspace()` returns the number of files written, or -1 on error.
 * `repl_load_workspace()` returns the number of files loaded, 0 for an empty
 * directory argument, or -1 on I/O error. */
int  repl_save_workspace(const char *dir);
int  repl_load_workspace(const char *dir);

/* Query/set the bound workspace directory (persisted across save/load).
 * repl_workspace_dir() returns "" if not bound. repl_set_workspace_dir(NULL)
 * clears the binding. String is copied internally. */
const char *repl_workspace_dir(void);
void repl_set_workspace_dir(const char *dir);

/* --- Command pipeline -------------------------------------------------- */

/* Expand a source program into caller-provided flat buffers. This is the
 * core two-level model: for-loops are unrolled, function calls are inlined,
 * and if-blocks are evaluated against their conditions, subject to the
 * MAX_FLATTEN_CALL_DEPTH and MAX_FLATTEN_VISIT_BUDGET limits. Tests use this
 * to flatten into temporary storage without mutating the live flat program;
 * the display loop uses the wrapped repl_flatten_commands() below. Result->
 * flat_cmd_count carries the generated count. Returns 1 on success, 0 on
 * error. */
int  repl_flatten_program(const ReplFlattenOptions *options,
                          ReplFlattenResult *result);

/* Rebuild the live flat program from the current source commands (idempotent).
 * Expansion honors the laziness flag set by mark_normals_dirty(); call this
 * once per frame before execution if the source array changed. */
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

/* --- Cursor / feed queries --------------------------------------------- */
int  repl_flat_cmd_matches_cursor(int flat_idx);
int  repl_find_feeding_normal_cmd(int line_idx);
int  repl_find_feeding_color_cmd(int line_idx);

/* --- Controller-owned input callback effects --------------------------- */

typedef struct ReplInputDispatchEffects {
    int request_redraw;
    int set_cursor;
    int cursor;
    int schedule_timer;
    unsigned int timer_millis;
    int timer_value;
} ReplInputDispatchEffects;

/* --- Input callback entry points (wired through the controller) -------- */

/* ASCII key and Ctrl-key input. Routes to repl_editor.c for editing, or to
 * repl_actions.c for config shortcuts (Ctrl+S, Ctrl+Z, Ctrl+R, etc.). */
ReplInputDispatchEffects repl_keyboard_func(unsigned char key, int x, int y);

/* Function key (F1-F12) and arrow input. Routes to repl_editor.c and
 * repl_actions.c for visual toggles, help overlay, theme cycling. */
ReplInputDispatchEffects repl_special_func(int key, int x, int y);

/* Mouse button press/release. Routes to ui_panels.c for code-panel hits,
 * color-picker drag, variable-slider transactions. */
ReplInputDispatchEffects repl_mouse_func(int button, int state, int x, int y);

/* Mouse motion during drag. Routes to ui_panels.c for in-progress drags. */
ReplInputDispatchEffects repl_motion_func(int x, int y);

/* Mouse motion without button held. Updates hover state for tooltips. */
ReplInputDispatchEffects repl_passive_motion_func(int x, int y);

#ifndef USE_GLUT
/* Mousewheel (freeglut only). Routes to repl_camera_controls.c for zoom velocity. */
ReplInputDispatchEffects repl_mousewheel_func(int wheel, int direction, int x, int y);
#endif

/* Polling timer callback (every ~16ms @ 60 FPS). Ticks animation frame counter,
 * advances replays, updates camera momentum, and posts next timer event. */
ReplInputDispatchEffects repl_timer_func(int value);

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
