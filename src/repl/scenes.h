/*
 * src/repl/scenes.h - User-scene promotion, capture, and reset hooks.
 *
 * The slot model itself lives in src/repl/scenes.c (home slot, LRU eviction,
 * workspace persistence). This header exposes the lifecycle entry points that
 * other REPL modules, the editor, and the controller call when an edit should
 * promote an example, an example load should capture/restore scene context, or
 * a full reset should discard scene state. Slot queries such as count/name/load
 * stay on src/repl/core.h.
 *
 * These hooks were split out of src/repl/core_internal.h so the remaining
 * internal header could focus on parse/normalize helpers (implemented as
 * phase 5 of feature/source-document-port.md).
 */
#ifndef REPL_SCENES_H
#define REPL_SCENES_H

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

/* Activate the pinned home slot (slot 0). Used when the user cycles
 * back through F12. */
void repl_scenes_activate_home_slot(void);

/* Drop all user-scene state. Called from glr_ctrl_reset_all. */
void repl_scenes_reset(void);

#endif /* REPL_SCENES_H */
