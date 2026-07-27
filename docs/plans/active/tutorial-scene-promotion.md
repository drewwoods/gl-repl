# Tutorial Scene Promotion: Treat Tutorials Like Examples

Tutorials currently use a **transient scene** buffer that has no identity. Both `g_active_user_scene` and `g_example_idx` are `-1`, so the undo hook's `repl_promote_example_if_needed()` never fires. When the user finishes a tutorial (or stops mid-tutorial) and continues editing, then later switches to another scene/example, their modifications are silently discarded.

The fix mirrors the existing example-promotion model: give the post-tutorial document a "tutorial origin" identity so the promotion machinery detects it and auto-promotes to a user scene slot on first edit.

## Proposed Changes

### Scene State: Track Tutorial Origin

#### [MODIFY] [state_views.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state_views.h)

Add an `active_tutorial_idx` field to `ReplSceneRuntimeState`, parallel to `active_example_idx`. Value is `-1` when no tutorial is active/completed; `>= 0` records which tutorial the current transient document originated from.

#### [MODIFY] [state.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state.c)

- Initialize `active_tutorial_idx = -1` in `repl_state_init()`.
- Add accessor `repl_state_active_tutorial_idx()` and setter `repl_state_scenes_set_active_tutorial_idx()`.

#### [MODIFY] [state_owners.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state_owners.h)

Declare the new setter `repl_state_scenes_set_active_tutorial_idx()`.

---

### Tutorial Runner: Set the Tutorial Origin Marker

#### [MODIFY] [tutorial_runner.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/subsystems/tutorial/tutorial_runner.c)

- **`tutorial_start()`**: After `repl_scenes_enter_transient_scene()`, set `repl_state_scenes_set_active_tutorial_idx(idx)` so the transient buffer carries the tutorial's identity.
- **`tutorial_end_keep_view()`**: The tutorial-origin marker stays set — this is the key difference from examples. The document is now a post-tutorial transient scene with a tutorial-origin identity that the promotion hook can detect.
- **`tutorial_teardown()`**: Clear `repl_state_scenes_set_active_tutorial_idx(-1)` so a full teardown (example load, reset-all, new tutorial) correctly removes the origin marker.

---

### Promotion Hook: Extend to Tutorials

#### [MODIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

Extend `repl_promote_example_if_needed()` (or rename to something like `repl_promote_transient_if_needed` and add a compatibility `#define`/wrapper) to also promote when a tutorial-origin is set. The logic:

```c
int repl_promote_example_if_needed(void) {
    if (g_active_user_scene >= 0) return -1;

    const char *origin_name = NULL;
    if (g_example_idx >= 0)
        origin_name = repl_example_name(g_example_idx);
    else if (g_tutorial_idx >= 0)
        origin_name = repl_tutorial_name(g_tutorial_idx);
    else
        return -1;

    // ... existing slot-finding + LRU eviction ...

    // derive unique name from origin_name
    // save_scene_to_slot(...)
    // set g_active_user_scene, clear both g_example_idx and g_tutorial_idx
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_active_tutorial_idx(-1);
    ...
}
```

Add a convenience `#define g_tutorial_idx (repl_state_active_tutorial_idx())` alongside the existing `g_example_idx` alias.

Also clear `active_tutorial_idx` in `repl_scenes_enter_transient_scene()` — the marker must only be set when entering from a tutorial, not from any arbitrary transient path (the tutorial runner sets it explicitly after the enter-transient call).

---

### Teardown Paths: Clear Tutorial Origin

#### [MODIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

In `repl_scenes_enter_transient_scene()`, also clear `active_tutorial_idx = -1`. (The tutorial runner in `tutorial_start()` then immediately re-sets it for the new tutorial — the clear-then-set order is correct.)

In `load_scene_from_slot()` and `repl_scenes_mark_example_active()`, clear `active_tutorial_idx = -1` since loading a scene or example supersedes any tutorial origin.

---

### Scene Snapshot: Include Tutorial Origin

#### [MODIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

The `ReplScenesSnapshot` struct and its `capture()`/`restore()` functions (used by tour baselines) don't need to track `active_tutorial_idx` — it lives in `ReplSceneRuntimeState` which is already part of `ReplState`. If the tour snapshot explicitly needs it, we can add it, but `ReplState`'s init/reset already covers the field.

---

### Baseline Restore on Promotion

#### [MODIFY] [tutorial_runner.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/subsystems/tutorial/tutorial_runner.c)

When the tutorial completes (`tutorial_end_keep_view()`), the cfg baseline bag stays pending. When the user later edits and auto-promotes, the promotion path must flush the pending baseline so the promoted scene carries the tutorial-mutated cfg. The existing `tutorial_teardown()` call in `repl_load_user_scene_idx()` handles this — it already runs `tutorial_baseline_restore()`. Since promotion goes through `editor_undo_push_snapshot() → repl_promote_example_if_needed()`, the teardown doesn't run at that point. We need to call `repl_dispatch_tutorial_teardown()` during tutorial promotion so the baseline is flushed before the slot snapshot captures live cfg. This is added inside `repl_promote_example_if_needed()` for the tutorial path.

---

### F12 Cycling: Include Post-Tutorial Transient in Cycle

#### [MODIFY] [glr_ctrl_router.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/app/glr_ctrl_router.c)

The existing `cycle_example_or_user_scene_dir()` handles three states: active user scene, active example, and "neither" (wraps to first example or user scene). The "neither" state now includes post-tutorial transient scenes. Currently the F12 path already handles this gracefully — when both `active_scene` and `g_example_idx` are `-1`, it wraps around to examples or user scenes. No change needed here since the transient-to-promoted promotion happens on edit, and F12 always calls `editor_undo_note_wholesale_replacement()` first.

However, we should ensure that switching away via F12 from a never-edited post-tutorial transient document triggers `tutorial_teardown()` so the cfg baseline is restored. The `repl_load_example()` path already calls `repl_dispatch_tutorial_teardown()`. The `repl_load_user_scene_idx()` path also already calls it. So this is covered.

## Open Questions

> [!NOTE]
> **Naming the promoted scene**: When an example promotes, it takes the example's display name (e.g., "Torus"). For tutorials, should the promoted scene be named after the tutorial (e.g., "Color & Transform") or should it include a "Tutorial:" prefix? I'll use the tutorial's display name directly (matching how examples work) unless you prefer otherwise.

> [!NOTE]
> **Promotion timing**: The plan promotes on **first post-tutorial edit** (same as examples). An alternative would be to promote immediately when the tutorial completes or is stopped. The lazy approach (promote-on-edit) is simpler, avoids allocating slots for tutorials the user abandons without editing, and is consistent with examples. I'll go with promote-on-edit.

## Verification Plan

### Automated Tests
- `make test-stubs` — ensures no compilation errors with GL stubs.
- `make check-c99` — C99 compliance.
- `make check-state-ownership` — guard suite passes.

### Manual Verification
1. Start a tutorial, complete it, edit the result → verify promotion status message appears and the scene shows up in the tab strip.
2. Start a tutorial, stop mid-way, edit → verify promotion.
3. Start a tutorial, complete it, F12 to another example → verify tutorial cfg baseline is restored and the post-tutorial transient document is discarded (not promoted because there was no edit).
4. Start a tutorial, complete it, edit, F12 away → verify promoted scene persists and can be F12'd back to.
5. Regression: loading examples still auto-promotes correctly.
