/*
 * src/repl/core.h - REPL pipeline facade.
 *
 * Exposes the command-pipeline entry points (parse-and-normalize, flatten,
 * reformat, autonormal recompute), the host-effects bridge through which
 * pipeline TUs surface status / cursor / input effects to the controller,
 * scene/workspace persistence wrappers, cursor/feed queries, and timekeeping
 * for the predefined `t` variable.
 *
 * Sibling headers callers may need to include directly:
 *   src/repl/state.h           — read-only state views
 *   src/repl/state_owners.h    — mutable accessors + reset helpers
 *   src/repl/export.h          — save/load file I/O
 *   src/repl/flatten.h         — flatten primitives
 *   src/subsystems/replay/replay.h — replay state machine
 *   src/editor/input.h         — editor commit / navigation entry points
 *
 * Many functions formerly declared here moved out as the pipeline grew its
 * own narrower owners; this file no longer re-declares or re-documents them.
 */
#ifndef REPL_CORE_H
#define REPL_CORE_H

#include <stdio.h>

#include "repl/bootstrap.h"   /* Compatibility: startup bootstrap moved out of core. */
#include "repl/export.h"     /* ReplExportLayout and save/load helpers */
#include "repl/flatten.h"
#include "repl/geometry_query.h" /* Compatibility: cursor/feed queries moved out of core. */
#include "repl/host_effects.h" /* Compatibility: host bridge moved out of core. */
#include "repl/program_query.h" /* Compatibility: program queries moved out of core. */
#include "repl/reformat.h"   /* Compatibility: reformatter moved out of core. */
#include "repl/time.h"       /* Compatibility: timekeeping moved out of core. */
#include "source_document.h" /* SourceTextView document view */

/* --- Save / load ------------------------------------------------------- */

/* Write the active user scene (or current example/transient buffer) to
 * ./output.c. This is the default single-file export path behind Ctrl+S when no
 * named scene-specific save target takes over. `layout` is the controller-built
 * ReplExportLayout passed through to the exporter as opaque integers. */
void repl_save_default_output(const ReplExportLayout *layout);

/* Save the active user scene to a file named after the scene:
 * `<workspace_dir>/<slug>.c` when a workspace dir is bound, else
 * `<slug>.c` in the cwd. App/controller callers that need a per-user
 * fallback directory should bind it before calling. When there is no
 * active named user scene (an example / transient document) this falls
 * back to repl_save_default_output() (./output.c). Creates the workspace
 * dir if needed and sets its own status naming the real file (the
 * underlying writer hardcodes an output.c message). */
void repl_save_active_scene(const ReplExportLayout *layout);

/* Return the path to use when exporting the active scene to a file with
 * extension `ext` (no leading dot), mirroring repl_save_active_scene's
 * naming: `<workspace_dir>/<slug>.<ext>` (creating the dir) or
 * `<slug>.<ext>` in the cwd for an active named user scene, else
 * `output.<ext>` for an example / transient document. Used by the .ply
 * export so it tracks the scene name the way Save Scene does. Returns a
 * pointer to a static buffer valid until the next call. */
const char *repl_active_scene_export_path(const char *ext);

/* Workspace I/O: save every occupied user-scene slot to `<dir>/<slug>.c`.
 * Each slot is flushed with its own @scene-name header. Both functions
 * remember `dir` so single-file exports carry a `@workspace-dir` hint.
 * Sets status message on success or failure.
 * `repl_save_workspace()` returns the number of files written, or -1 on error.
 * `repl_load_workspace()` returns the number of files loaded, 0 for an empty
 * directory argument, or -1 on I/O error. */
int  repl_save_workspace(const char *dir, const ReplExportLayout *layout);
int  repl_load_workspace(const char *dir);

/* Reasons repl_load_scene_as_new_slot can fail. Callers (status-bar
 * prompts, menu wiring) translate these into user-visible messages
 * without needing to do filesystem probing themselves. */
typedef enum {
    REPL_SCENE_LOAD_OK = 0,         /* succeeded; slot index returned */
    REPL_SCENE_LOAD_ERR_EMPTY_PATH, /* empty / NULL path */
    REPL_SCENE_LOAD_ERR_NOT_FOUND,  /* stat() failed (no such file, EACCES, ...) */
    REPL_SCENE_LOAD_ERR_IS_DIR,     /* path resolved to a directory */
    REPL_SCENE_LOAD_ERR_PARSE,      /* repl_export_load_from_file rejected it */
    REPL_SCENE_LOAD_ERR_NO_SLOT     /* slots full and no workspace bound to evict to */
} ReplSceneLoadStatus;

/* Runtime "load file as new scene": loads `path` into a freshly-
 * allocated user-scene slot (slot allocation is delegated to the
 * shared load_scene_file_into_slot helper, which also covers the
 * workspace-load path), makes that slot the active scene, and leaves
 * any previously-active scene in its own slot for later retrieval
 * via the scene tabs / F12. Returns the new slot index on success,
 * -1 on failure. *out_reason (when non-NULL) receives a distinguished
 * failure reason so callers can render the right error without
 * re-probing the path. The live document AND the undo ring are
 * preserved on failure regardless of reason — only the caller
 * controls when to clear the undo ring (typically only on success). */
int  repl_load_scene_as_new_slot(const char *path,
                                 ReplSceneLoadStatus *out_reason);

/* Query/set the bound workspace directory (persisted across save/load).
 * repl_workspace_dir() returns "" if not bound. repl_set_workspace_dir(NULL)
 * clears the binding. String is copied internally. */
const char *repl_workspace_dir(void);
void repl_set_workspace_dir(const char *dir);

/* --- Command pipeline -------------------------------------------------- */

/* Rebuild the live flat program from the current source commands (idempotent).
 * Expansion honors the laziness flag set by mark_normals_dirty(); call this
 * once per frame before execution if the source array changed. */
void repl_flatten_commands(int edit_line_idx);

/* Recompute auto-normals for every glBegin/glEnd batch in the source
 * array. Called automatically when source commands are modified.
 *
 * `autonormal_enabled` gates the recompute. The toggle lives on
 * `GlrState.presentation` (step 7a of
 * feature/decouple-repl-from-gl-repl-alt.md); callers pass the value
 * explicitly because `src/repl/autonormal.c` is a REPL pipeline TU and
 * cannot reach into glr_state. Pass 0 for an unconditional no-op. */
void repl_recompute_autonormals(int autonormal_enabled,
                                int *edit_line_inout);

/* Mark the source program dirty: invalidates the auto-normal pass, the
 * flat program, and the source-scope depth cache. Every source mutation
 * goes through here. Was named repl_mark_normals_dirty until the audit
 * pointed out it does much more than that. */
void        repl_mark_source_dirty(void);

/* --- Example library & user scene -------------------------------------- */
#ifndef MAX_USER_SCENES
#define MAX_USER_SCENES      8
#endif
#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

/* Built-in example library. Examples are immutable snapshots of
 * documented GL patterns; editing an example auto-promotes it to a
 * user scene on first edit (via repl_promote_example_if_needed()).
 * Query API (repl_example_count / repl_example_name / _lines /
 * _subheading / tag accessors) lives in src/repl/examples.h —
 * include that header directly rather than re-declaring here.
 *
 * repl_load_example returns the post-load cursor target (= document
 * line count after the example body emits). Caller applies the value
 * via editor_state_edit_line_set() above the β boundary (implemented
 * in phase 3.6.4 of plans/done/edit-line-ownership.md). */
int  repl_load_example(int idx);

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

/* Activate the first occupied slot after repl_load_workspace (which leaves
 * active == -1 and the pre-load document live). Returns the slot, or -1. */
int  repl_scenes_activate_first_loaded_slot(void);

#endif /* REPL_CORE_H */
