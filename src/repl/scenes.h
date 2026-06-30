/*
 * src/repl/scenes.h - User-scene promotion, capture, and reset hooks.
 *
 * The slot model itself lives in src/repl/scenes.c (home slot, LRU eviction,
 * workspace persistence). This header exposes the lifecycle entry points that
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
    REPL_SCENE_LOAD_ERR_NO_SLOT     /* slots full and no workspace bound to evict to */
} ReplSceneLoadStatus;

/* Called before any mutation: if an example is currently viewed (no
 * active user scene), allocate a scene slot, copy the current editor
 * state into it, and inherit the example's name (de-duplicated).
 * Returns the promoted slot index, or -1 if promotion was a no-op or
 * rejected. */
int  repl_promote_example_if_needed(void);

/* Persist the currently-active user scene back to its slot before
 * loading a different scene/example. No-op when no slot is active. */
void repl_scenes_save_active_scene_if_any(void);

/* Detach the live document from examples and user-scene slots so a
 * transient buffer can take over without being written back into a slot.
 * Also captures the pre-tutorial buffer into slot 0 if home is not yet
 * populated, mirroring the example-load capture so fresh-buffer work
 * isn't silently discarded. */
void repl_scenes_enter_transient_scene(void);

/* Reset the REPL-side document / flat program / predef vars / func
 * aliases / editor-input dispatch so a transient buffer can be
 * populated from scratch. The choreography mirrors load_example_lines
 * but skips example-specific cfg handling. Callers must pair this with
 * `repl_scenes_enter_transient_scene` to detach the slot markers. */
void repl_scenes_reset_for_transient(void);

/* Create an empty, named user-scene slot and make it active. This is the
 * File -> New Scene path: unlike tutorial/transient buffers, it must appear
 * in the scene tab strip immediately. Returns the active slot index, or -1
 * if every slot is full and no workspace-backed LRU eviction is available. */
int  repl_scenes_create_empty_user_scene(void);

/* User scenes: persistent named snapshots (up to MAX_USER_SCENES slots).
 * Slot 0 is the "home" scene (captured on first example load) and is never
 * evicted by LRU. When every non-home, non-active slot is full and a new
 * promotion happens, the LRU slot is flushed to the workspace directory
 * (if bound) and reused. The active slot is the one currently loaded into
 * g_cmds[]; -1 means an example or fresh workspace is active instead.
 * Editing resets the "last touched" timestamp for LRU eviction. */
int  repl_user_scene_valid(void);         /* 1 if any slot occupied */
void repl_load_user_scene(void);          /* load slot 0 (back-compat) */
int  repl_user_scene_count(void);         /* number of occupied slots */
int  repl_user_scene_slot_used(int slot); /* 1 if slot is occupied */
const char *repl_user_scene_name(int slot);
int  repl_user_scene_rename(int slot, const char *new_name);
int  repl_load_user_scene_idx(int slot);  /* load slot, returns 1 on success */
int  repl_active_user_scene(void);        /* current slot index, -1 if none */

/* On first example load only, capture the pre-example editor state
 * into the pinned "home" slot (slot 0) so the user can always return
 * to their starting work. */
void repl_scenes_capture_home_if_needed(void);

/* Snapshot the scene-presentation cfg subset when entering an example from
 * non-example state. The saved values are restored on the next user-scene/home
 * transition. Idempotent across consecutive example loads. */
void repl_scenes_capture_pre_example_cfg_if_entering(void);

/* Record that an example is currently the active scene (active_example_idx
 * already set by the loader; this updates derived state). */
void repl_scenes_mark_example_active(void);

/* Activate the pinned home slot (slot 0). `scene_name_hint` is the
 * `@scene-name` directive value parsed from a freshly-loaded file
 * (or NULL/"" for the no-import path); when non-empty it becomes
 * the slot 0 name, otherwise the slot picks the built-in default. */
void repl_scenes_activate_home_slot(const char *scene_name_hint);

/* Workspace I/O: load every *.c scene under `dir`. Returns the number of
 * files loaded, 0 for an empty directory argument, or -1 on I/O error. */
int  repl_load_workspace(const char *dir);

/* Save every occupied user-scene slot to `<dir>/<slug>.c`.
 * Each slot is flushed with its own @scene-name header. Both functions
 * remember `dir` so single-file exports carry a `@workspace-dir` hint.
 * Sets status message on success or failure.
 * Returns the number of files written, or -1 on error. */
int  repl_save_workspace(const char *dir, const ReplExportLayout *layout);

/* Save the active user scene to a file named after the scene:
 * `<workspace_dir>/<slug>.c` when a workspace dir is bound, else
 * `<slug>.c` in the cwd. App/controller callers that need a per-user
 * fallback directory should bind it before calling. When there is no
 * active named user scene (an example / transient document) this falls
 * back to repl_save_default_output() (./output.c). Creates the workspace
 * dir if needed and sets its own status naming the real file. */
void repl_save_active_scene(const ReplExportLayout *layout);

/* Return the path to use when exporting the active scene to a file with
 * extension `ext` (no leading dot), mirroring repl_save_active_scene's
 * naming: `<workspace_dir>/<slug>.<ext>` (creating the dir) or
 * `<slug>.<ext>` in the cwd for an active named user scene, else
 * `output.<ext>` for an example / transient document. Used by the .ply
 * export so it tracks the scene name the way Save Scene does. Returns a
 * pointer to a static buffer valid until the next call. */
const char *repl_active_scene_export_path(const char *ext);

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

#endif /* REPL_SCENES_H */
