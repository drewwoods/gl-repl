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

#include "repl/export.h"     /* ReplExportLayout and save/load helpers */
#include "repl/flatten.h"
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

/* Shared status/document helpers surfaced outside src/repl/core.c. */
void        repl_set_status(const char *msg);
/* Error variant: dispatches through `status_error` so the UI renders
 * the status bar in red. Falls back to `status` when the host bridge
 * does not provide an error hook (so demo / test harnesses still see
 * the text). */
void        repl_set_status_error(const char *msg);

/* Controller-installed side-effect hooks for pipeline-only code paths.
 *
 * Loader, scene-switch, snippet-import, and replay code in src/repl call
 * through this table when they need a host action such as clearing editor
 * input, scrolling the code panel, tearing down tutorial state before source
 * replacement, or resetting example presentation state.
 * Most pure REPL tests leave the table unset; the standalone demo installs
 * only its edit-line hooks. Any unset callback makes the matching dispatcher
 * a no-op.
 *
 * This mirrors the export cfg/camera bridges: the REPL asks for an effect by
 * purpose, without naming editor/UI implementation details. Phases 3 + 6 of
 * feature/source-document-port.md consolidated the older per-effect installers
 * into this single struct.
 *
 * Insert-mode QUERY (for ReplCompileContext.insert_mode) is the
 * caller's responsibility — repl_compile_context_from_live() defaults
 * to 0 (overwrite, the safe load-path value); editor-side callers
 * overwrite the field with editor_insert_mode() before compiling. */
typedef struct {
    /* Surface a diagnostic/status message. The controller routes this
     * to UiState; pipeline TUs call repl_set_status() unchanged. */
    void (*status)(const char *msg);
    /* Same as `status` but tags the message as an error so the
     * controller can render it in the red-palette status bar. NULL is
     * permitted — `repl_set_status_error()` then falls back to the
     * info path so test harnesses without a host bridge still see the
     * message text. */
    void (*status_error)(const char *msg);
    /* Refresh scene-bound presentation defaults (wireframe, grid,
     * axes, vertex overlays, backdrop) before each example load. The
     * `presentation` slice lives on glr_state, out of the REPL
     * pipeline's reach.
     *
     * `tag_mask` is the bitmask of REPL_EXAMPLE_TAG_* bits for the
     * example being loaded (0 for non-example loads such as the test
     * line-feed entrypoint). The controller applies the global
     * defaults first, then layers tag-specific `@cfg` overrides on
     * top before the example's own leading `@cfg` metadata runs.
     * Order of precedence: example `@cfg` > tag default > global
     * default. */
    void (*example_presentation_reset)(unsigned int tag_mask);
    /* Clear the editor input buffer. */
    void (*input_reset)(void);
    /* Force the editor out of insert mode. */
    void (*insert_mode_off)(void);
    /* Scroll the code panel so source line `target` is visible. */
    void (*scroll_to_line)(int target);
    /* Tear down any active tutorial before wholesale source replacement.
     * The tutorial runner is a peer subsystem; routing this through the
     * host bridge keeps src/repl from linking tutorial.c just to request
     * its idempotent cleanup. NULL = no tutorial host, so no-op. */
    void (*tutorial_teardown)(void);
    /* Read/write the edit-line cursor. Cursor storage moved
     * to EditorState; REPL code that previously called
     * repl_state_edit_line() / _set() goes through these hooks so
     * the β invariant (REPL → editor symbol references forbidden)
     * stays intact. Higher-level pipeline entry points (scenes.c)
     * use this; the parse / compile / flatten / load layers take
     * cursor as an explicit parameter instead (see phase 3.6.x;
     * implemented in phase 4 of plans/in-review/edit-line-ownership.md).
     *
     * Default behavior when the hook is NULL: edit_line_get
     * returns 0; edit_line_set is a no-op. The demo / pure REPL
     * tests run unchanged. */
    int  (*edit_line_get)(void);
    void (*edit_line_set)(int line);
    /* Decoupled editor/completion mutations and reads for subsystems. */
    void (*host_cursor_park)(int line, int insert_mode);
    void (*completion_clear)(void);
    void (*completion_update)(void);
    const char *(*host_input_get)(void);
    void (*set_time_playing)(int playing);
} ReplHostEffects;

void                   repl_install_host_effects(const ReplHostEffects *effects);

/* Pipeline-side dispatchers — invoked by the loader / scene-switch /
 * snippet-import / replay paths. Each is a no-op when the bridge is
 * unset or the matching callback is NULL. */
void        repl_dispatch_example_presentation_reset(unsigned int tag_mask);
void        repl_dispatch_input_reset(void);
void        repl_dispatch_insert_mode_off(void);
void        repl_dispatch_scroll_to_line(int target);
void        repl_dispatch_tutorial_teardown(void);
int         repl_dispatch_edit_line_get(void);
void        repl_dispatch_edit_line_set(int line);
void        repl_dispatch_host_cursor_park(int line, int insert_mode);
void        repl_dispatch_completion_clear(void);
void        repl_dispatch_completion_update(void);
const char *repl_dispatch_host_input_get(void);
void        repl_dispatch_set_time_playing(int playing);

const char *repl_mode_name(GLenum mode);
GLenum      repl_current_begin_mode(void);
int         repl_count_vertices(void);
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
 * in phase 3.6.4 of plans/in-review/edit-line-ownership.md). */
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

/* --- Timekeeping ------------------------------------------------------- */

/* Advance the predefined `t` variable by `dt` seconds. The controller's
 * timer tick calls this every frame when the animation toggle (Ctrl+T)
 * is on. */
void repl_advance_time(float dt);

/* Reset `t` to 0. Called from controller/test paths that need a
 * deterministic time origin. */
void repl_reset_time_to_zero(void);

/* Set the predefined `t` variable to an explicit value (seconds). Used by
 * the startup `--time` flag / `GLR_TIME` env override so animations can be
 * captured starting from a later point in their timeline. */
void repl_set_time(float value);

/* Returns the post-load cursor target. Caller applies the value
 * via editor_state_edit_line_set() above the β boundary (implemented
 * in phase 3.6.4 of plans/in-review/edit-line-ownership.md). */
int repl_load_initial_commands(const char *import_file);
/* Pure REPL pass: walks every command and rewrites the canonical
 * line text + GLCmd in place. Does not save/restore editor input;
 * the editor's Ctrl+\ wrapper (`editor_reformat_commands`) layers
 * that on top. */
void repl_reformat_program(void);

/* --- Cursor / feed queries --------------------------------------------- */
int  repl_flat_cmd_matches_cursor(int flat_idx, int edit_line_idx);
int  repl_find_feeding_normal_cmd(int line_idx);
int  repl_find_feeding_color_cmd(int line_idx);
/* When the cursor sits on a CMD_POP_MATRIX line, returns the source-line
 * index of the matching CMD_PUSH_MATRIX (the nearest earlier push at the
 * same nesting level). Returns -1 if the cursor isn't on a pop or no
 * matching push exists. */
int  repl_find_matching_push_matrix(int line_idx);
/* Mirror of repl_find_matching_push_matrix: when the cursor sits on a
 * CMD_PUSH_MATRIX line, returns the source-line index of the matching
 * CMD_POP_MATRIX (the nearest later pop at the same nesting level).
 * Returns -1 if the cursor isn't on a push or no matching pop exists. */
int  repl_find_matching_pop_matrix(int line_idx);
/* When the cursor sits on a color-consuming line (immediate vertex,
 * gluVertex, or glutSolid*), fills out_line_idx[] with up to out_cap
 * source-line indices of the modelview-affecting transforms in scope
 * at that line: CMD_TRANSLATE3F / CMD_SCALEF / CMD_ROTATEF. Walks the
 * source array backwards, honors glPushMatrix/glPopMatrix scopes (a
 * transform inside a popped range is excluded), stops at the nearest
 * CMD_LOAD_IDENTITY in scope, skips function bodies, and stops at the
 * enclosing CMD_FUNC_DEF when the cursor lives inside a function.
 * Returns the number of entries written; 0 if the cursor isn't on a
 * color-consuming line. */
#define MAX_AFFECTING_TRANSFORMS 32
int  repl_find_affecting_transforms(int line_idx, int *out_line_idx, int out_cap);

#endif /* REPL_CORE_H */
