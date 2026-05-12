/*
 * src/repl/core.h - Public REPL facade.
 *
 * Exposes the source/flat command model, persistence helpers, replay hooks,
 * example and user-scene management, and the input callback wrappers that the
 * controller forwards to. Runtime storage lives in src/repl/state.c and is accessed
 * through src/repl/state.h; scene/workspace persistence lives in src/repl/scenes.c.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include <stdio.h>

#include "repl/export.h"     /* ReplExportLayout (step 7c) */
#include "repl/flatten.h"
#include "source_document.h" /* SourceTextView (Phase 1 of feature/source-document-port.md) */

/* --- Save / load ------------------------------------------------------- */

/* Write the active user scene (or current example) to ./output.c.
 * `layout` is the controller-built ReplExportLayout (step 7c); the
 * pipeline reads its values as opaque integers. */
void repl_save_default_output(const ReplExportLayout *layout);

/* repl_export_load_from_file / repl_export_save_output live in
 * src/repl/export.h — include that header directly to use them. */

/* Workspace I/O: save every occupied user-scene slot to `<dir>/<slug>.c`.
 * Each slot is flushed with its own @scene-name header. Both functions
 * remember `dir` so single-file exports carry a `@workspace-dir` hint.
 * Sets status message on success or failure.
 * `repl_save_workspace()` returns the number of files written, or -1 on error.
 * `repl_load_workspace()` returns the number of files loaded, 0 for an empty
 * directory argument, or -1 on I/O error. */
int  repl_save_workspace(const char *dir, const ReplExportLayout *layout);
int  repl_load_workspace(const char *dir);

/* Query/set the bound workspace directory (persisted across save/load).
 * repl_workspace_dir() returns "" if not bound. repl_set_workspace_dir(NULL)
 * clears the binding. String is copied internally. */
const char *repl_workspace_dir(void);
void repl_set_workspace_dir(const char *dir);

/* --- Command pipeline -------------------------------------------------- */

/* repl_flatten_program lives in src/repl/flatten.h — include that
 * header directly to use it. */

/* Rebuild the live flat program from the current source commands (idempotent).
 * Expansion honors the laziness flag set by mark_normals_dirty(); call this
 * once per frame before execution if the source array changed. */
void repl_flatten_commands(void);

/* Recompute auto-normals for every glBegin/glEnd batch in the source
 * array. Called automatically when source commands are modified.
 *
 * `autonormal_enabled` gates the recompute. The toggle lives on
 * `GlrState.presentation` (step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md); callers pass the value
 * explicitly because `src/repl/autonormal.c` is a REPL pipeline TU and
 * cannot reach into glr_state. Pass 0 for an unconditional no-op. */
void repl_recompute_autonormals(int autonormal_enabled);

/* Shared status/document helpers surfaced outside src/repl/core.c. */
void        repl_set_status(const char *msg);
/* Install a status-message sink. Pipeline TUs call set_status() to
 * surface diagnostics; the controller installs ui_state_status_set as
 * the sink at startup. Pass NULL to clear (test scaffolding may want
 * to). The demo deliberately leaves the sink unset, so set_status is
 * a no-op there. See step 3 of feature/decouple-repl-from-gl-repl-alt.md. */
void        repl_set_status_sink(void (*sink)(const char *));

/* Install a presentation-reset sink for the example loader. The
 * `presentation` slice moved to glr_state.c (step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md), so the example loader
 * (REPL pipeline TU) can no longer reach into those fields. The
 * controller installs `glr_state_presentation_reset_example_defaults`
 * as the sink at startup; the demo leaves it unset so the per-load
 * reset is a no-op (the demo doesn't load examples). */
void        repl_install_example_presentation_reset_sink(void (*fn)(void));
/* Pipeline-side dispatch — invoked by src/repl/example_loader.c on every
 * example load. No-op when the sink is unset. */
void        repl_dispatch_example_presentation_reset(void);

/* Host-effect sinks. Loader / scene-switch / replay paths emit four
 * host-visible effects (input reset, insert-mode off, scroll-to-line,
 * follow-cursor toggle) the controller actualizes on the editor; the
 * demo and tests leave the matching sinks unset and the dispatches
 * become no-ops. The sinks are deliberately named after the EFFECT
 * (what the REPL is asking for) rather than the implementation, so
 * the public REPL surface doesn't carry editor concepts. Phases 3 +
 * 6 of feature/source-document-port.md.
 *
 * Insert-mode QUERY (for ReplCompileContext.insert_mode) is the
 * caller's responsibility — repl_compile_context_from_live() defaults
 * to 0 (overwrite, the safe load-path value); editor-side callers
 * overwrite the field with editor_insert_mode() before compiling. */
void        repl_install_input_reset_sink(void (*fn)(void));
void        repl_dispatch_input_reset(void);
void        repl_install_insert_mode_off_sink(void (*fn)(void));
void        repl_dispatch_insert_mode_off(void);
void        repl_install_scroll_to_line_sink(void (*fn)(int target));
void        repl_dispatch_scroll_to_line(int target);
void        repl_install_follow_cursor_sink(void (*fn)(int follow));
void        repl_dispatch_follow_cursor(int follow);

const char *repl_mode_name(GLenum mode);
GLenum      repl_current_begin_mode(void);
int         repl_count_vertices(void);
void        repl_mark_normals_dirty(void);

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
/* repl_replay_start / repl_replay_stop live in src/widgets/replay.h —
 * include that header directly to use them. */

/* --- Timekeeping ------------------------------------------------------- */

/* Advance the predefined `t` variable by `dt` seconds. The controller's
 * timer tick calls this every frame when the animation toggle (Ctrl+T)
 * is on. */
void repl_advance_time(float dt);

/* Reset `t` to 0. Called from controller/test paths that need a
 * deterministic time origin. */
void repl_reset_time_to_zero(void);

/* --- Editor / navigation helpers called from outside src/repl/core.c ------- */
void repl_navigate_to_line(int target);
void repl_load_initial_commands(const char *import_file);
/* Pure REPL pass: walks every command and rewrites the canonical
 * line text + GLCmd in place. Does not save/restore editor input;
 * the editor's Ctrl+\ wrapper (`editor_reformat_commands`) layers
 * that on top. */
void repl_reformat_program(void);

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

/* repl_reset_state was removed in step 2 of
 * feature/decouple-repl-from-gl-repl-alt.md. Tests and callers that
 * want full-world reset call `glr_app_reset_all()` from glr_ctrl.h.
 * REPL-only callers use `repl_state_init_defaults()` /
 * `repl_state_reset_program()` from src/repl/state_owners.h. */

/* Public wrapper over the internal feed_line() so test code outside
 * src/repl/core_internal.h users can drive command commitment end-to-end.
 * Used by integration tests that validate parsing → commit → execute flows. */
void repl_feed_line_public(const char *line);

#endif /* REPL_CORE_H */
