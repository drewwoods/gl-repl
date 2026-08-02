# Tutorial Scene Promotion: Treat Post-Tutorial Documents Like Examples

## Status - LANDED (2026-07-27)

Shipped as designed. `ReplSceneRuntimeState.tutorial_origin_idx` is the
post-tutorial marker, set only by `tutorial_end_keep_view()` and cleared by
`tutorial_teardown()` plus every scene-state transition in `src/repl/scenes.c`.
`repl_promote_example_if_needed()` was renamed to
`repl_promote_transient_if_needed()` outright (no compatibility wrapper - all
in-tree callers were updated) and now recognises example and tutorial origins,
with the capture-before-teardown ordering and the `apply_scene_cfg_from_slot()`
per-scene reapply the design calls for. Slot reservation was extracted to
`reserve_slot_for_promotion()` so a slots-full rejection is retryable.

Deviations from the plan, both in the verification section:

- **Prelude-failure test (item 4).** There is no runtime injection point for a
  scene-prelude load failure (the catalog is static and the transient scene is
  freshly reset before the prelude runs). Since `tutorial_start` writes the
  marker nowhere, every start-failure path shares the same outcome by
  construction; the test covers the reachable rejections (out-of-range and
  invalid index) and additionally pins that a rejected start does not discard
  an *existing* post-tutorial origin.
- **Global-slug test (item 6).** No shipped tutorial SETs a slug outside
  `cfg_key_in_scene_subset()` - Feature Tour's `grid` / `vertex_outlines`, and
  even `view_mode`, are all per-scene. The test synthesizes the case by adding
  `msaa` to the live tutorial's restore baseline before mutating it, which is
  the exact shape a future tutorial with a global SET step would produce.

The as-built prose lives in `docs/ARCHITECTURE.md`
("Post-tutorial scene promotion"); the design below is the historical record.

## Problem

Tutorials use a **transient scene** buffer with no persistent scene identity. After a tutorial completes or is stopped, the user remains in the generated document and can continue editing it. Because both `g_active_user_scene` and `g_example_idx` are `-1`, the undo hook's `repl_promote_example_if_needed()` does not promote that document. If the user later switches to another scene or example, those edits are silently discarded.

The fix mirrors the existing example-promotion model, but only after the tutorial has ended: give the post-tutorial document a **tutorial origin** identity so the first subsequent edit promotes it into a user-scene slot. Active tutorial steps must remain transient and must never trigger promotion.

## Design Invariants

1. An active tutorial never has a tutorial-origin marker and cannot auto-promote.
2. `tutorial_end_keep_view()` establishes the marker for the finished or stopped tutorial document.
3. Promotion captures the live tutorial document and its tutorial-mutated per-scene configuration **before** restoring the pre-tutorial baseline.
4. After capture, tutorial teardown restores global/tutorial-only configuration; the promoted slot's per-scene configuration is then reapplied to the live view.
5. A failed promotion leaves the tutorial origin and pending baseline intact so a later edit can retry.
6. Any wholesale document replacement clears the tutorial origin and flushes the pending tutorial baseline.

## Proposed Changes

### Scene State: Track Post-Tutorial Origin

#### [MODIFY] [state_views.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state_views.h)

Add `tutorial_origin_idx` to `ReplSceneRuntimeState`, parallel to `active_example_idx`:

- `-1`: the live document did not originate from a completed/stopped tutorial.
- `>= 0`: the transient live document is the retained result of that tutorial.

Use `tutorial_origin_idx`, not `active_tutorial_idx`: the marker deliberately describes an **inactive post-tutorial document**, not the currently running tutorial.

#### [MODIFY] [state.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state.c)

- Initialize `tutorial_origin_idx = -1` in `repl_state_apply_sentinels()` so reset/default state is unambiguous.
- Add read accessor `repl_state_tutorial_origin_idx()`.
- Add owner setter `repl_state_scenes_set_tutorial_origin_idx()`.

Because `ReplCheckpointState` already carries `scene_runtime`, ordinary REPL/tour checkpoint capture and restore include this field automatically.

#### [MODIFY] [state_owners.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/state_owners.h)

Declare `repl_state_scenes_set_tutorial_origin_idx()`.

---

### Tutorial Runner: Establish Origin Only When the Tutorial Ends

#### [MODIFY] [tutorial_runner.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/subsystems/tutorial/tutorial_runner.c)

#### `tutorial_start()`

Do **not** set the tutorial-origin marker here. Tutorial commands pass through `editor_undo_push_snapshot()`, so setting the marker at start would promote on the first tutorial command and could teardown the tutorial while it is still running.

The existing startup order remains correct:

1. `tutorial_teardown()` flushes any predecessor tutorial/post-tutorial state.
2. `repl_scenes_enter_transient_scene()` clears all scene origins.
3. The tutorial prelude loads and the tutorial becomes active.

A prelude-load failure therefore leaves no tutorial origin.

#### `tutorial_end_keep_view()`

Before `tutorial_state_reset_except_baseline()` clears the active tutorial index, copy that index into the scene-runtime marker:

```c
int origin_idx = tutorial_state_view().tutorial_idx;
tutorial_state_mut()->active = 0;
repl_state_scenes_set_tutorial_origin_idx(origin_idx);
tutorial_state_reset_except_baseline();
```

This path is shared by normal completion and `tutorial_stop()`, so both produce a promotable post-tutorial document while retaining the pending cfg baseline.

#### `tutorial_teardown()`

After restoring/clearing the baseline and resetting tutorial runtime state, clear `tutorial_origin_idx = -1`. This covers:

- switching to an example or user scene;
- reset-all;
- starting another tutorial;
- discarding an unedited post-tutorial document;
- successful tutorial promotion after the live document has already been captured.

---

### Promotion Hook: Extend Promotion to Tutorial Origins

#### [MODIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

Rename `repl_promote_example_if_needed()` to `repl_promote_transient_if_needed()` if practical, retaining a compatibility wrapper only if needed by existing callers/tests. The hook now recognizes two origin kinds:

- built-in example: `g_example_idx >= 0`;
- completed/stopped tutorial: `g_tutorial_origin_idx >= 0`.

Add a convenience alias alongside `g_example_idx`:

```c
#define g_tutorial_origin_idx (repl_state_tutorial_origin_idx())
```

An active tutorial never reaches the tutorial-origin branch because its marker remains `-1` until `tutorial_end_keep_view()`.

#### Promotion transaction

Use this ordering:

```c
int repl_promote_transient_if_needed(void) {
    if (g_active_user_scene >= 0)
        return -1;

    enum OriginKind origin_kind;
    const char *origin_name;

    if (g_example_idx >= 0) {
        origin_kind = ORIGIN_EXAMPLE;
        origin_name = repl_example_name(g_example_idx);
    } else if (g_tutorial_origin_idx >= 0) {
        origin_kind = ORIGIN_TUTORIAL;
        origin_name = repl_tutorial_name(g_tutorial_origin_idx);
    } else {
        return -1;
    }

    /* Reserve a free slot or complete LRU eviction first. */
    int slot = reserve_slot_for_promotion();
    if (slot < 0)
        return -1;  /* preserve origin + pending baseline for retry */

    char unique[USER_SCENE_NAME_MAX];
    derive_unique_scene_name(unique, sizeof(unique),
                             origin_name ? origin_name : "Scene", -1);

    /* Capture the live post-tutorial document and tutorial-mutated cfg. */
    save_scene_to_slot(slot, unique, repl_dispatch_edit_line_get());

    if (origin_kind == ORIGIN_TUTORIAL) {
        /* Restore and clear the complete pre-tutorial baseline only after
         * the promoted slot has captured the tutorial view. */
        repl_dispatch_tutorial_teardown();

        /* Teardown restores global and per-scene slugs to their pre-tutorial
         * values. Reapply only the promoted slot's per-scene cfg subset so
         * the live view continues to match the newly-created scene while
         * tutorial-only/global settings stay restored. */
        apply_scene_cfg_from_slot(slot);
    }

    g_active_user_scene = slot;
    repl_state_scenes_set_active_example_idx(-1);
    repl_state_scenes_set_tutorial_origin_idx(-1);

    /* Publish the existing promotion status. */
    return slot;
}
```

The exact slot-reservation code can remain the existing free-slot/LRU logic. The important rule is that no tutorial teardown or origin clearing occurs until a slot has been successfully obtained and `save_scene_to_slot()` has captured the live document.

#### Per-scene cfg reapply helper

Add a small file-local helper such as:

```c
static void apply_scene_cfg_from_slot(int slot) {
    const ReplConfigBridge *bridge = repl_config_bridge();
    if (slot < 0 || slot >= MAX_USER_SCENES ||
        !g_user_scenes[slot].used || !bridge || !bridge->apply)
        return;
    bridge->apply(&g_user_scenes[slot].snapshot.cfg);
}
```

Do not reload the full `SceneSnapshot` during promotion. A full apply would unnecessarily rewrite document, predefs, camera/editor cursor policy, and other live state while promotion is running inside an editor commit transaction. Only cfg needs reapplication after teardown.

#### Failed promotion semantics

When every user-scene slot is full and no workspace-backed eviction is possible:

- leave `tutorial_origin_idx` unchanged;
- leave the pending tutorial baseline valid;
- report the existing slots-full status;
- allow the edit to proceed in the transient document;
- retry promotion on the next edit.

If a slot later becomes available, that retry captures all edits accumulated in the transient document.

---

### Scene Transition Paths: Clear Tutorial Origin

#### [MODIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

Clear `tutorial_origin_idx = -1` whenever a scene transition supersedes the post-tutorial document:

- `repl_scenes_enter_transient_scene()`;
- `load_scene_from_slot()`;
- `repl_scenes_mark_example_active()`;
- explicit loaded-document/user-scene activation;
- `repl_scenes_reset()` / reset-all paths.

Most replacement paths already dispatch `tutorial_teardown()`, which clears the marker and restores the baseline. Clearing at the scene-state boundary as well keeps the origin invariant local and defensive.

`repl_scenes_enter_transient_scene()` must clear the marker unconditionally. Unlike the previous design, `tutorial_start()` does not immediately set it again; only `tutorial_end_keep_view()` establishes a new origin.

---

### Scene and Tour Snapshots

#### [VERIFY] [scenes.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/scenes.c)

`ReplScenesSnapshot` does not need a new field: it owns the user-scene catalog, active user-scene slot, LRU tick, and pre-example cfg. The tutorial-origin marker lives in `ReplSceneRuntimeState`.

#### [VERIFY] [glr_tour_snapshot.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/app/glr_tour_snapshot.c)

The tour snapshot already captures both:

- `ReplCheckpointState`, including `scene_runtime.tutorial_origin_idx`;
- `TutorialRuntimeState`, including the pending cfg baseline.

Add a round-trip test to ensure a retained post-tutorial document restores both pieces together.

---

### F12 Cycling and Wholesale Replacement

#### [VERIFY] [glr_ctrl_router.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/app/glr_ctrl_router.c)

No routing change should be required. A post-tutorial transient document has neither an active user scene nor an active example, so the existing “neither” branch wraps to examples or user scenes.

F12 performs a wholesale replacement rather than an edit, so it must not call the promotion hook. The destination load path already calls tutorial teardown:

- example load restores the pending tutorial baseline before applying example presentation state;
- user-scene load restores it before applying the destination scene snapshot.

Thus an unedited post-tutorial document is discarded, its origin is cleared, and the user's pre-tutorial configuration is restored.

---

## Decisions

> [!NOTE]
> **Naming the promoted scene:** use the tutorial's display name directly, matching example promotion. Existing unique-name suffixing handles collisions.

> [!NOTE]
> **Promotion timing:** promote on the first edit **after** tutorial completion or stop. Do not allocate a user-scene slot merely for viewing or abandoning a tutorial result.

> [!NOTE]
> **Configuration semantics:** the promoted scene owns the tutorial-mutated per-scene configuration. Tutorial-only/global settings are restored to their pre-tutorial values during promotion.

## Verification Plan

### Automated Tests

Add focused tests for the lifecycle rather than relying only on broad build checks:

1. **Active tutorial does not promote**
   - Start a tutorial and commit its first expected command.
   - Verify no user-scene slot was created and `tutorial_origin_idx == -1`.
   - Verify the tutorial remains active and advances normally.

2. **Completion establishes origin**
   - Complete a tutorial.
   - Verify the tutorial is inactive, the baseline remains valid, and `tutorial_origin_idx` names the completed tutorial.

3. **Stop establishes origin**
   - Stop mid-tutorial.
   - Verify the retained document has the stopped tutorial's origin.

4. **Prelude failure leaves no origin**
   - Force tutorial prelude loading to fail.
   - Verify `tutorial_origin_idx == -1` and no stale baseline remains.

5. **First post-tutorial edit promotes once**
   - Complete/stop a tutorial, then perform an edit.
   - Verify one user-scene slot is created with the tutorial display name.
   - Verify both example and tutorial origins are cleared.
   - Verify later edits do not allocate additional slots.

6. **Promotion preserves configuration correctly**
   - Begin with distinguishable pre-tutorial values for a per-scene slug and a global/tutorial-only slug such as `view_mode`.
   - Have the tutorial change both.
   - Promote after completion.
   - Verify the saved scene and live view retain the tutorial value for the per-scene slug.
   - Verify the global/tutorial-only slug returns to its pre-tutorial value.
   - Reload the promoted scene and verify its per-scene cfg round-trips.

7. **Slots-full failure is retryable**
   - Fill all user-scene slots without a workspace.
   - Complete a tutorial and edit.
   - Verify promotion fails but the tutorial origin and pending baseline remain.
   - Free a slot and edit again.
   - Verify promotion succeeds and captures the intervening transient edits.

8. **Wholesale replacement discards unedited result**
   - Complete a tutorial, then load an example or user scene without editing.
   - Verify no promotion occurs, origin/baseline are cleared, and pre-tutorial cfg is restored.

9. **Tour snapshot round-trip**
   - Capture while a post-tutorial transient document and pending baseline exist.
   - Mutate away from it, restore the tour snapshot, and verify origin plus baseline are restored consistently.

10. **Example promotion regression**
    - Load and edit an example.
    - Verify its existing promotion, naming, undo, LRU, and cfg behavior is unchanged.

Run the relevant suites and guards:

- `make test-stubs`
- `make check-c99`
- `make check-state-ownership`

### Manual Verification

1. Complete a tutorial and edit the result: promotion status appears and the scene is added to the tab strip.
2. Stop a tutorial mid-way and edit: the retained partial document promotes.
3. Complete a tutorial and press F12 without editing: the document is discarded and the original configuration returns.
4. Complete a tutorial, edit, then F12 away and back: the promoted scene persists with the tutorial view.
5. Fill all slots without a workspace, edit a completed tutorial, free a slot, then edit again: the second edit successfully promotes the accumulated document.
6. Load and edit a built-in example: existing example auto-promotion remains unchanged.
