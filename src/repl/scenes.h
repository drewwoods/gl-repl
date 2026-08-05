/*
 * src/repl/scenes.h - User-scene promotion, capture, and reset hooks.
 *
 * The slot model itself lives in src/repl/scenes.c (fixed capacity, workspace
 * persistence). This header exposes the lifecycle entry points that
 * other REPL modules, the editor, and the controller call when an edit should
 * promote an example, an example load should capture/restore scene context, or
 * a full reset should discard scene state.
 *
 * These hooks were split out of the former src/repl/core_internal.h umbrella,
 * which has since been removed entirely so callers include the focused owner
 * headers they need directly.
 */
#ifndef REPL_SCENES_H
#define REPL_SCENES_H

#ifndef REPL_EXPORT_LAYOUT_DECLARED
#define REPL_EXPORT_LAYOUT_DECLARED
typedef struct ReplExportLayout ReplExportLayout;
#endif

#ifndef MAX_USER_SCENES
#define MAX_USER_SCENES      8
#endif
#ifndef USER_SCENE_NAME_MAX
#define USER_SCENE_NAME_MAX 64
#endif

/* Reasons repl_load_scene_as_new_slot can fail. Callers (status-bar
 * prompts, menu wiring) translate these into user-visible messages
 * without needing to do filesystem probing themselves. */
typedef enum {
    REPL_SCENE_LOAD_OK = 0,         /* succeeded; slot index returned */
    REPL_SCENE_LOAD_ERR_EMPTY_PATH, /* empty / NULL path */
    REPL_SCENE_LOAD_ERR_NOT_FOUND,  /* stat() failed (no such file, EACCES, ...) */
    REPL_SCENE_LOAD_ERR_IS_DIR,     /* path resolved to a directory */
    REPL_SCENE_LOAD_ERR_PARSE,      /* repl_export_load_from_file rejected it */
    REPL_SCENE_LOAD_ERR_NO_SLOT     /* all eight scene slots are occupied */
} ReplSceneLoadStatus;

typedef struct {
    int ok;
    int managed;
    int files_seen;
    int scenes_loaded;
    int files_failed;
    int capacity_exceeded;
} ReplWorkspaceLoadResult;

/* Called before any mutation: if the live document is a promotable
 * TRANSIENT one (no active user scene), allocate a scene slot, copy the
 * current editor state into it, and inherit the origin's name
 * (de-duplicated). Two origins qualify:
 *   - a built-in example being viewed (active_example_idx >= 0);
 *   - the retained result of a completed or stopped tutorial
 *     (tutorial_origin_idx >= 0). An ACTIVE tutorial never qualifies.
 * For a tutorial origin the slot captures the lesson's view first, then
 * tutorial teardown restores the user's pre-tutorial global settings and the
 * slot's per-scene cfg subset is re-applied to the live view.
 * Returns the promoted slot index, or -1 if promotion was a no-op or
 * rejected. A rejection (all slots full) leaves
 * the origin - and any pending tutorial baseline - intact, so the next edit
 * retries and still captures everything typed in between. */
int  repl_promote_transient_if_needed(void);

/* Persist the currently-active user scene back to its slot before
 * loading a different scene/example. No-op when no slot is active. */
void repl_scenes_save_active_scene_if_any(void);

/* Detach the live document from examples and user-scene slots so a
 * transient buffer can take over without being written back into a slot. */
void repl_scenes_enter_transient_scene(void);

/* Reset the REPL-side document / flat program / predef vars / func
 * aliases / editor-input dispatch so a transient buffer can be
 * populated from scratch. The choreography mirrors load_example_lines
 * but skips example-specific cfg handling. Callers must pair this with
 * `repl_scenes_enter_transient_scene` to detach the slot markers. */
void repl_scenes_reset_for_transient(void);

/* Create a fresh, named user-scene slot, seed its editable display defaults,
 * and make it active. This is the File -> New Scene path: unlike
 * tutorial/transient buffers, it must appear in the scene tab strip
 * immediately. Returns the active slot index, or -1 if every slot is full. */
int  repl_scenes_create_empty_user_scene(void);

/* User scenes: persistent named snapshots (up to MAX_USER_SCENES slots).
 * Any slot can hold a user-created, imported, or promoted scene. Capacity is a
 * hard eight scenes until an explicit workspace scene browser exists. The active slot is the one
 * currently loaded into g_cmds[]; -1 means an example or transient buffer is
 * active instead. */
int  repl_user_scene_valid(void);         /* 1 if any slot occupied */
void repl_load_user_scene(void);          /* load slot 0 (back-compat) */
int  repl_user_scene_count(void);         /* number of occupied slots */
int  repl_user_scene_slot_used(int slot); /* 1 if slot is occupied */
const char *repl_user_scene_name(int slot);
int  repl_user_scene_rename(int slot, const char *new_name);
int  repl_load_user_scene_idx(int slot);  /* load slot, returns 1 on success */
int  repl_active_user_scene(void);        /* current slot index, -1 if none */

/* Snapshot the scene-presentation cfg subset when entering an example from
 * non-example state. The saved values are restored on the next user-scene or
 * transient transition. Idempotent across consecutive example loads. */
void repl_scenes_capture_pre_example_cfg_if_entering(void);

/* Leave any active user scene when a replacement document takes ownership.
 * The caller sets active_example_idx separately when the replacement is a
 * successfully loaded example. */
void repl_scenes_detach_active_user_scene(void);

/* Activate slot 0 from the current live document. `scene_name_hint` is the
 * `@scene-name` directive value parsed from a freshly-loaded file; when empty,
 * the slot uses a generic imported-scene name. */
void repl_scenes_activate_loaded_document_slot(const char *scene_name_hint);

/* Workspace I/O: load the ordered scene files named by `dir/.glr-workspace`.
 * Manifest-less directories are rejected. The compact wrapper returns the
 * number of loaded scenes (including zero for a valid empty workspace), or
 * -1 on any validation, capacity, I/O, or parse error. */
int  repl_load_workspace(const char *dir);
ReplWorkspaceLoadResult repl_load_workspace_ex(const char *dir);

/* Save every occupied user-scene slot and publish `dir/.glr-workspace` last.
 * Scene filenames are assigned once and remain stable across display-name
 * changes. Only files owned by the previous manifest may be pruned. The
 * workspace binding changes only after a successful commit. Returns the
 * number of files written, or -1 on error. */
int  repl_save_workspace(const char *dir, const ReplExportLayout *layout);

/* Save the active user scene to a file named after the scene:
 * `<workspace_dir>/<slug>.c` when a workspace dir is bound, else
 * `<slug>.c` in the cwd. App/controller callers that need a per-user
 * fallback directory should bind it before calling. When there is no
 * active named user scene (an example / transient document) this falls
 * back to repl_save_default_output() (./output.c). Creates the workspace
 * dir if needed and sets its own status naming the real file. */
int  repl_save_active_scene(const ReplExportLayout *layout);

/* Return the path to use when exporting the active scene to a file with
 * extension `ext` (no leading dot), mirroring repl_save_active_scene's
 * naming: `<workspace_dir>/<slug>.<ext>` (creating the dir) or
 * `<slug>.<ext>` in the cwd for an active named user scene, else
 * `output.<ext>` for an example / transient document. Used by the .ply
 * export so it tracks the scene name the way Save Scene does. Returns a
 * pointer to a static buffer valid until the next call. */
const char *repl_active_scene_export_path(const char *ext);
const char *repl_user_scene_file_name(int slot);
int  repl_user_scene_delete(int slot);
int  repl_workspace_is_managed(void);

/* Runtime "load file as new scene": loads `path` into a freshly-
 * allocated user-scene slot (slot allocation is delegated to the
 * shared load_scene_file_into_slot helper, which also covers the
 * workspace-load path), makes that slot the active scene, and leaves
 * any previously-active scene in its own slot for later retrieval
 * via the scene tabs / F12. Returns the new slot index on success,
 * -1 on failure. *out_reason (when non-NULL) receives a distinguished
 * failure reason so callers can render the right error without
 * re-probing the path. The live document AND the undo ring are
 * preserved on failure regardless of reason -- only the caller
 * controls when to clear the undo ring (typically only on success). */
int  repl_load_scene_as_new_slot(const char *path,
                                 ReplSceneLoadStatus *out_reason);

/* Runtime "load text as new scene": same transactional slot semantics as
 * repl_load_scene_as_new_slot(), but imports already-read scene text instead
 * of probing a filesystem path. Used by clipboard/stdin-style callers.
 * `fallback_name` names the slot when the text has no @scene-name metadata. */
int  repl_load_scene_text_as_new_slot(const char *text,
                                      const char *fallback_name,
                                      ReplSceneLoadStatus *out_reason);

/* Query/set the bound workspace directory (persisted across save/load).
 * repl_workspace_dir() returns "" if not bound. repl_set_workspace_dir(NULL)
 * clears the binding. String is copied internally. */
const char *repl_workspace_dir(void);
void repl_set_workspace_dir(const char *dir);

/* Activate the first occupied slot after repl_load_workspace (which leaves
 * active == -1 and the pre-load document live). Returns the slot, or -1. */
int  repl_scenes_activate_first_loaded_slot(void);

/* Drop all user-scene state. Called from glr_ctrl_reset_all. */
void repl_scenes_reset(void);

/* Opaque, heap-backed snapshot of the whole user-scene catalog: every slot's
 * occupancy/name/stable filename/SceneSnapshot, the active user-scene index, the
 * monotonic scene tick, and the pre-example config bag. Used by the tour
 * baseline (src/app/glr_tour_snapshot.c) so Back / Done-restart reinstate the
 * exact scene catalog the tour started from.
 *
 * Capture deliberately does NOT call repl_scenes_save_active_scene_if_any() -
 * that would mutate the catalog (flush the live document into its slot) while
 * taking the baseline. It records the catalog exactly as it stands. Returns
 * NULL on allocation failure; destroy frees it. */
typedef struct ReplScenesSnapshot ReplScenesSnapshot;

ReplScenesSnapshot *repl_scenes_snapshot_capture(void);
int  repl_scenes_snapshot_restore(const ReplScenesSnapshot *snapshot);
void repl_scenes_snapshot_destroy(ReplScenesSnapshot *snapshot);

#endif /* REPL_SCENES_H */
