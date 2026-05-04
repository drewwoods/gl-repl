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

#include "editor_state.h"   /* EditorBufferView */
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
 * Sets status message on success or failure. `text` is the editor
 * buffer view the caller built; export reads source text exclusively
 * through that view. */
void repl_export_save_output(const char *filename, EditorBufferView text);

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

/* Shared status/document helpers surfaced outside repl_core.c. */
void        set_status(const char *msg);
const char *mode_name(GLenum mode);
GLenum      current_begin_mode(void);
int         count_vertices(void);
void        mark_normals_dirty(void);

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

/* --- Input callback entry points -------------------------------------- */
/* The ReplInputDispatchEffects typedef and the editor_handle_* /
 * editor_input_router_* dispatch APIs live in editor_input.h. The
 * legacy repl_*_func dispatch entry points were deleted in Phase J1
 * commit 49a. */

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
