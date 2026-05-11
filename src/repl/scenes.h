/*
 * src/repl/scenes.h - User-scene promotion, capture, and reset API.
 *
 * The user-scene model lives in src/repl/scenes.c (slots + LRU eviction +
 * workspace persistence). This header is the public surface other
 * REPL TUs and the editor/controller call into. Scene queries (slot
 * count / names / load / active) live on src/repl/core.h; THIS file
 * exposes the lifecycle hooks that fire on edit, example load, and
 * world reset.
 *
 * Phase 5 of feature/source-document-port.md split these out of
 * src/repl/core_internal.h so the "core internal" header can shrink
 * toward parse-only internals.
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

/* On first example load only, capture the pre-example editor state
 * into the pinned "home" slot (slot 0) so the user can always return
 * to their starting work. */
void repl_scenes_capture_home_if_needed(void);

/* Snapshot the 14 presentation-cfg keys when entering an example from
 * non-example state. Restored on the next user-scene / home transition.
 * Idempotent across consecutive example loads. */
void repl_scenes_capture_pre_example_cfg_if_entering(void);

/* Record that an example is currently the active scene (active_example_idx
 * already set by the loader; this updates derived state). */
void repl_scenes_mark_example_active(void);

/* Activate the pinned home slot (slot 0). Used when the user cycles
 * back through F12. */
void repl_scenes_activate_home_slot(void);

/* Drop all user-scene state. Called from glr_app_reset_all. */
void repl_scenes_reset(void);

#endif /* REPL_SCENES_H */
