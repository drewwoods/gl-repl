# Close-scene capability (deferred - design brief)

## 2026-05-23 audit

No "Close active scene" item in the File menu (`GLR_FILE_ITEM_*` enum
in `src/app/glr_actions.h:33-42` is still NEW / SAVE / LOAD / RENAME /
SAVE_WORKSPACE / LOAD_WORKSPACE). No close/evict API in
`src/repl/scenes.c`. Open questions 1-4 below are unresolved. Stays
deferred in `not-started/`.

## Context

There is currently no way to remove a user scene from the in-memory
workspace / tab strip. Scenes accumulate (up to `MAX_USER_SCENES = 8`)
and the only "removal" is the implicit LRU eviction inside
`repl_promote_example_if_needed()` when all slots are full and a
workspace dir is bound. Users want an explicit "close" (close a tab /
remove a scene). This is non-trivial - it has real semantic forks and a
destructive edge - so it is split out of the Scene/File menu rework and
parked here until the open questions are decided.

## What "close" needs (mechanics)

- A real eviction/remove API in `src/repl/scenes.c` (today eviction is
  private LRU logic inside promote). Frees the slot, picks a sensible
  next active scene (another occupied slot → else home → else empty /
  transient), refreshes `repl_active_user_scene()`.
- Must call `editor_undo_clear()` on close - the undo ring is global;
  any wholesale document swap clears it (same invariant as F12 /
  load-scene; see CLAUDE.md "Undo/Redo"). Owned by the editor layer,
  called from the controller, to preserve editor→repl layering.
- Snapshot tab list (`glr_ctrl_build_scene_tabs`) and the active-tab
  highlight follow automatically next frame (derived state) - no
  tab-model change needed.
- Builds on the Scene/File menu restructure (separate, in-scope work):
  "Close active scene" lands in the File menu beside New/Save/Rename.

## Open design questions (must resolve before implementing)

1. **Pinned home slot 0.** Slot 0 is the pinned "home" scene
   (`repl_scenes_activate_home_slot`, never auto-evicted). Is it
   closable at all? Options: (a) not closable (status message),
   (b) closable → drops to an empty/transient document.
2. **On-disk file.** When a workspace dir is bound a slot maps to a
   `<slug>.c` file. Does close (a) only free the in-memory slot (file
   untouched; a workspace reload brings it back - "close the tab"), or
   (b) also delete the file (permanent; destructive, would want a
   confirm which the app has nowhere else)? (a) is the safe default.
3. **Example tabs.** An example tab is not "in the workspace." What
   does close mean there - just deactivate the example (return to a
   user scene / empty), or is close disabled for example tabs? Likely:
   close is a user-scene-only action; example tabs have no close.
4. **Unsaved edits.** There is no per-scene dirty flag (editor dirty
   state is global). Close silently drops in-memory edits - consistent
   with the app (no save prompts anywhere), but confirm that is
   acceptable, or whether close should auto-save when a workspace is
   bound.

## UI scope options

- **v1: File-menu item only** - "Close active scene" reuses menu
  routing; lowest risk.
- **Follow-up: per-tab × button** - hover/active close affordance on
  the tab strip (new render sub-region + hit-test sub-rect + routing;
  the existing whole-band consume logic must still hold). Larger.

## Dependencies / ripple

- Depends on the Scene/File menu restructure (subheading rename +
  moving New/Save/Rename to File) landing first - close slots into
  that same File-menu layout and dispatch.
- Eviction must not desync `glr_scene_menu_slot_for_dense_index()`,
  the tab router (`route_scene_tab_hit`), or F12 cycle - all derive
  from occupied-slot order, which close mutates.
- Tests: `test_ui_scene_tabs` derivation cases (a closed slot →
  recomputed tab list / active idx), plus a scenes.c eviction unit
  test.

## Effort

Moderate, **gated on the decisions above** (1-4). The mechanics are a
few focused edits; the cost is getting the semantics right and the
destructive-edge (Q2) decision. Estimate once decided: ~half a day for
the File-menu v1; the per-tab × follow-up is a separate, similar-sized
piece.

## Status

Deferred. Not started. Resolve open questions 1-4, then promote to an
active plan (sequenced after the Scene/File menu rework).
