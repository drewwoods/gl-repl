# Tour Transport and Backstep Authoring Controls

## Summary

Add replay-style controls to Tours-menu tours:

- Space: pause/resume; restart from Done.
- Right Arrow: execute one event immediately, then remain paused.
- Left Arrow: restore the tour-start state, fast-run to the preceding event boundary, then remain paused.
- `+` / `-`: change tour timing from `0.25×` through `16×`.
- Escape: exit the tour while preserving its current result.
- HUD: show tour name, state, speed, event number, total events, and `.pointer` source line.

One executable `.pointer` line is one step. Environment-driven recording scripts retain their current behavior and receive no transport HUD or snapshot overhead.

Backstep will use one optimized baseline plus prefix replay. Do not implement per-event whole-app snapshots.

## New Files

### `src/app/glr_tour_snapshot.{c,h}`

Own the tour-start baseline and coordinate capture/restore across state owners.

Expose an opaque API:

```c
typedef struct GlrTourSnapshot GlrTourSnapshot;

GlrTourSnapshot *glr_tour_snapshot_capture(void);
int              glr_tour_snapshot_restore(const GlrTourSnapshot *snapshot);
void             glr_tour_snapshot_destroy(GlrTourSnapshot *snapshot);
```

The implementation must heap-allocate large components and clean up partially constructed snapshots on failure. No large snapshot structure should be placed on the stack.

### `src/ui/subsystems/tour_hud.{c,h}`

Render a compact, non-interactive transport HUD from the per-frame UI snapshot. Keep the renderer state-read-only and consistent with the replay HUD’s snapshot boundary.

### Tests

Add:

- `tests/test_glr_tour_snapshot.c` for baseline round trips.
- `tests/test_glr_tour_transport.c` for playback, controls, seeking, and completion.

Both tests should run with GL stubs and link through `CORE_TEST_OBJS`.

## State and Interface Additions

### Tour playback state

Add to `glr_pointer_script.h`:

```c
typedef enum {
    GLR_TOUR_OFF = 0,
    GLR_TOUR_BASELINE_PENDING,
    GLR_TOUR_PLAYING,
    GLR_TOUR_PAUSED,
    GLR_TOUR_STEPPING,
    GLR_TOUR_SEEKING,
    GLR_TOUR_DONE
} GlrTourPlaybackState;

typedef struct {
    int                  active;
    GlrTourPlaybackState state;
    const char          *name;
    const char          *file;
    float                speed;
    int                  completed_events;
    int                  current_event;
    int                  total_events;
    int                  source_line;
} GlrTourPlaybackView;
```

Add APIs:

```c
int  glr_pointer_script_start_tour(
         const char *name,
         const char *file,
         const char *const *lines,
         int line_count);

GlrTourPlaybackView glr_pointer_script_tour_view(void);

int glr_pointer_script_handle_tour_key(unsigned char key);
int glr_pointer_script_handle_tour_special(int key);
```

Keep `glr_pointer_script_start_lines()` for existing tests and non-catalog callers. It must preserve its current behavior unless explicitly started as a tour.

Extend `PsEvent` with the original one-based source line. Because the generator already embeds blank and comment lines, `i + 1` in the parser is the correct `.pointer` file line.

### Catalog metadata

Update `scripts/gen_tours.py` and `TourEntry` to emit:

```c
typedef struct {
    const char        *name;
    const char        *file;
    const char *const *lines;
    int                line_count;
} TourEntry;
```

`glr_tours_start()` passes the name and file to `glr_pointer_script_start_tour()`.

### REPL checkpoint state

Do not store the 6.8 MB `ReplFlatProgramState`; it is derived and must be rebuilt.

Add to `src/repl/state.{c,h}`:

```c
typedef struct {
    ReplDocumentState     document;
    ReplVariableState     variables;
    ReplRenderState       render;
    ReplSceneRuntimeState scene_runtime;
    ReplImportExportState import_export;
} ReplCheckpointState;

void repl_state_checkpoint_capture(ReplCheckpointState *out);
void repl_state_checkpoint_restore(const ReplCheckpointState *snapshot);
```

Restore must:

- rebind evaluator predef storage;
- restore the special `t` binding;
- invalidate expression and source-scope caches;
- clear flat-program count;
- mark the flat program dirty;
- clear stale rebake/argument-dirty state.

### Editor session state

Do not copy per-frame transformers, highlights, virtual lines, or line overrides.

Add an `EditorSessionSnapshot` containing:

- `EditorBuffer`;
- input and insert state;
- selection and clipboard;
- search and autocomplete;
- scroll state and edit-line cursor;
- cursor blink state.

Add capture/restore APIs to `src/editor/state.{c,h}`. The REPL command array and editor text buffer must still restore in lockstep.

### Undo/redo history

The existing `EditorUndoRingState` stores only counters, not the actual history. Add an opaque, heap-backed history snapshot API to `src/editor/undo.{c,h}`:

```c
typedef struct EditorUndoHistorySnapshot EditorUndoHistorySnapshot;

EditorUndoHistorySnapshot *editor_undo_history_capture(void);
int editor_undo_history_restore(
        const EditorUndoHistorySnapshot *snapshot);
void editor_undo_history_destroy(
        EditorUndoHistorySnapshot *snapshot);
```

Capture only live undo and redo entries, in logical oldest-to-newest order. Restore them into a canonical ring layout and restore the generation counter. Do not copy all 64 fixed slots when most are unused.

### Scene catalog

The active document is separate from `g_user_scenes[]`, so capture both.

Add an opaque `ReplScenesSnapshot` API to `src/repl/scenes.{c,h}` that preserves:

- every occupied slot’s index, name, LRU timestamp, and `SceneSnapshot`;
- active user-scene index;
- monotonic scene tick;
- pre-example configuration bag.

Capture must not call `repl_scenes_save_active_scene_if_any()`, because that would mutate the catalog while taking the baseline.

### Missing peer snapshots

Add complete snapshot APIs for state that current pose-only or reset-only functions cannot restore:

- `GlrCameraRuntimeSnapshot`: live pose, ease target, target-active flag, target decay, control mode, scene default, pointer/button cache, modifier state, and all momentum velocities.
- `GlrViewTransitionSnapshot`: projection mixes, target mode, phase, saved 3D camera, and validity.
- `UiMenuBarRuntimeSnapshot`: open menu, hover row, flyout parent/state/scroll, and animation clocks; dropdown geometry cache remains derived.
- `ColorPickerRuntimeSnapshot`: open line, HSV/alpha, palette/key state, drag state, anchor, value limits, and undo-captured flag; omit the installed host pointer and derived palette cache.
- `UiOverlayLayoutSnapshot`: eased panel positions and reserved band.
- Existing by-value snapshots: `GlrState`, `UiState`, `ReplayRuntimeState`, `TutorialRuntimeState`, `VariablePanelState`, and `EditorHelpSession`.

Renderer resources, profiling histories, GL objects, audio engine internals, and controller frame caches are not baseline state.

## Baseline Capture and Restore

### Capture timing

`glr_pointer_script_start_tour()` should parse the script and enter `GLR_TOUR_BASELINE_PENDING`, but not capture immediately.

On the next `glr_pointer_script_frame()`:

1. The Tours dropdown has completed its normal close path.
2. Capture `GlrTourSnapshot`.
3. Seed the scripted pointer from the restored UI pointer coordinates.
4. Enter `GLR_TOUR_PLAYING`.
5. Do not fire the first event until the following virtual tour frame.

If capture fails, stop the tour, free partial allocations, and report `Tour could not capture rewind state`.

### Snapshot contents

`GlrTourSnapshot` should contain:

```text
ReplCheckpointState
EditorSessionSnapshot
EditorUndoHistorySnapshot *
ReplScenesSnapshot *
GlrState
UiState
ReplayRuntimeState
TutorialRuntimeState
VariablePanelState
EditorHelpSession
GlrCameraRuntimeSnapshot
GlrViewTransitionSnapshot
UiMenuBarRuntimeSnapshot
ColorPickerRuntimeSnapshot
UiOverlayLayoutSnapshot
```

### Restore order

Restore in this order:

1. Release any synthetic click or explicitly held pointer button.
2. Clear the pointer engine’s in-flight glide, click-release, paced typing, wait, ring, echo, and ripple state.
3. Restore the scene catalog.
4. Restore `GlrState`.
5. Restore the REPL checkpoint.
6. Restore the editor session.
7. Restore undo/redo history.
8. Restore camera and view-transition state.
9. Restore replay, tutorial, variable-panel, color-picker, and help-session state.
10. Restore `UiState`, menu state, and overlay-layout state.
11. Re-sync controller-derived chrome and invalidate controller frame caches.
12. Refresh workspace-header, render-state, and camera export strings.

Add an app-internal helper such as `glr_ctrl_after_tour_restore()` for step 11–12. It must not call `glr_ctrl_reset_transients()`, because that would erase restored camera defaults, menus, and peer state.

## Pointer-Script Transport

### Virtual clock

Separate rendered frames from tour virtual frames.

Maintain:

```c
float g_speed;        /* default 1.0 */
float g_frame_credit; /* accumulated virtual-frame credit */
int   g_frame;        /* tour virtual frame */
```

For a playing live tour:

```text
frame_credit += speed
while frame_credit >= 1:
    advance one virtual tour frame
    frame_credit -= 1
```

Use speed values `0.25`, `0.5`, `1`, `2`, `4`, `8`, and `16`. Clamp at the endpoints.

Paused, Done, and baseline-pending tours accumulate no credit. Environment-driven capture scripts continue advancing exactly once per rendered frame.

### Event accounting

Track these separately:

- `g_next_event`: next event that has not fired;
- `g_current_event`: in-flight event, or `-1`;
- `g_completed_events`: number whose completion condition has passed.

Do not treat an event as completed merely because it fired. Update `g_completed_events` only after its `PsWait` is satisfied. The HUD shows:

- Playing/Stepping: `current_event + 1` as the active step.
- Paused between events: `completed_events`.
- Done: `event_count / event_count`.

The HUD source line is the active event’s line, or the next event’s line when paused before it.

### Pause and completion

Space:

- Playing → Paused.
- Paused → Playing.
- Done → restore baseline, reset event position, enter Playing.
- Seeking/Stepping → consume without changing state.
- Baseline Pending → consume.

A tour enters Done when every event has logically completed. Do not wait for decorative echo, ring, or ripple expiration. Retain the baseline and HUD until Escape, cancellation, or another tour starts.

### Immediate Right-Arrow step

Right Arrow from Paused executes exactly one event and returns to Paused.

Immediate completion rules:

| Event | Immediate behavior |
|---|---|
| `move` | Dispatch target position once. |
| `glide` | Resolve once; dispatch smoothstep samples across its original virtual-frame count, ending exactly at the target. |
| `click` / `rightclick` | Move if needed, press, and release synchronously. |
| `down` | Press and remain held across the paused boundary. |
| `up` | Move if needed and release. |
| `wheel` | Dispatch once. |
| `key` / `key@N` | Flush the complete payload synchronously. |
| `skey` / `chord` | Dispatch once. |
| `pause` | Complete without waiting. |
| `echo` / `ring` | Create the overlay and complete immediately; freeze its tour-clock age while paused. |

Escape or cancellation must release a button left held by a stepped `down`.

### Left-Arrow backstep

Define the target boundary as:

```text
if an event is in flight:
    target = completed_events
else:
    target = max(0, completed_events - 1)
```

Then:

1. Enter `GLR_TOUR_SEEKING`.
2. Restore the baseline.
3. Reset the pointer runtime to event zero.
4. Fast-dispatch the first `target` events using the immediate rules above.
5. Enter Paused with `completed_events == target`.

Process at most 32 events per rendered frame while Seeking so a long tour cannot monopolize the UI thread. Suppress historical ripple/ring/echo rendering during the prefix; materialize only the overlay state belonging to the target boundary.

For Emscripten `shell:` clicks, yield one browser event-loop turn after scheduling the DOM click before continuing the prefix. This preserves the asynchronous New-button behavior.

Fast seeking must not call `glr_ctrl_tick()` or accelerate scene time, camera easing, REPL replay, status TTL, or audio. Those systems resume normally once the target boundary is reached. Discrete edits, toggles, scene actions, and sampled camera drags are reconstructed; time-driven settling may differ from the original traversal.

## Input and HUD Integration

### Host input routing

In `gl_repl.c`, route tour transport keys before the existing cancel intercept:

```text
real key
  -> tour transport handler
  -> if consumed, return
  -> otherwise cancel active tour and swallow event
  -> normal controller routing when no tour is active
```

Special keys follow the same order.

Mouse presses and wheel input continue to cancel. Passive pointer motion continues not to cancel. Synthetic events still call `glr_ctrl_*` directly and therefore bypass host interception.

Recognized controls:

- Space
- Escape
- `+`, `=`, `-`
- Left Arrow
- Right Arrow

### UI snapshot

Add `GlrTourPlaybackView tour` to `UiRenderSnapshot` and populate it in `glr_ctrl_build_ui_snapshot()`.

Render `tour_ui_hud_render(&ui_snap)` near the top of the scene viewport. Keep it separate from the bottom-mounted REPL replay HUD so a tour demonstrating replay can show both simultaneously.

HUD contents:

```text
Tour  Editing Basics  | Paused | 4× | Step 17 / 43 | editing-basics.pointer:26
[progress groove]
Space play  |  ← back  |  → step  |  +/- speed  |  Esc exit
```

Use existing UI theme tokens and bitmap fonts. The HUD is display-only; do not add mouse hit-testing in this change.

The existing pointer/caption overlay remains rendered after the composited app frame, so it stays visually above the HUD.

## File-Level Work Map

- `src/app/glr_pointer_script.{c,h}`: transport state machine, virtual clock, immediate execution, seeking, playback view.
- `src/app/glr_tours.{c,h}` and `scripts/gen_tours.py`: pass tour name/file metadata.
- `src/app/glr_tour_snapshot.{c,h}`: baseline orchestration and cleanup.
- `gl_repl.c`: route transport keys before cancellation.
- `src/app/glr_ctrl.c` and internal header: snapshot population, HUD call, post-restore cache/chrome synchronization.
- `src/ui/app/snapshot.h`: add tour view.
- `src/ui/subsystems/tour_hud.{c,h}`: HUD renderer.
- REPL/editor/scene/camera/menu/peer owner modules: add the focused capture/restore APIs described above.
- `Makefile`: add both test binaries under the GL-stub test set; application sources are already wildcarded.
- `tours/README.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`, and user-facing help text: document controls, state ownership, and rewind limitations.

## Implementation Order

1. Add the focused owner snapshot APIs and their isolated round-trip tests.
2. Implement `GlrTourSnapshot` capture/restore and verify a composite round trip.
3. Refactor pointer-script frame advancement into a reusable one-virtual-frame function without changing existing behavior.
4. Add live-tour state, pause, speed, immediate forward step, and Done.
5. Add baseline-pending capture and Back/Seeking.
6. Add host key interception and the HUD.
7. Update generator metadata, documentation, Makefile tests, and guards.
8. Run the full verification suite.

Do not begin Back/Seeking until the composite snapshot round-trip test passes.

## Test and Acceptance Plan

### Snapshot tests

- Capture two populated user scenes, variables, editor input, selection, config, camera target/momentum, replay, tutorial, menus, and undo/redo.
- Mutate every captured owner.
- Restore and compare public views to the baseline.
- Assert the flat program is dirty and rebuilds to equivalent output.
- Verify allocation failure leaves the live app unchanged and leaks nothing.

### Transport tests

- Pause freezes event dispatch and virtual time.
- Each speed advances the expected virtual-frame count.
- Right Arrow completes every verb correctly and pauses.
- A stepped drag preserves held-button behavior across `down`/`glide`/`up`.
- Escape and cancellation always release held buttons.
- Left Arrow during an in-flight event returns to its starting boundary.
- Left Arrow between events returns one completed event.
- Back reconstructs code commits, config toggles, menu navigation, scene creation/load, and camera drag state.
- Done remains active; Space restores the baseline and restarts.
- Event numbering ignores comments/blanks while source lines remain exact.
- Timed environment scripts retain their original scheduling, auto-stop policy, and lack of HUD.
- Native and web catalogs parse through the new tour-start path.
- Tour and REPL replay HUDs render together without overlap.

### Required commands

```sh
make check-tours-catalog
make WEB=1 check-tours-catalog
make test-stubs
make gl-repl USE_GL_STUBS=1
make check-state-ownership
make check-c99
```

Also run the focused tour snapshot/transport tests directly during development.

## Assumptions and Boundaries

- Every executable event line is one step; no grouping grammar is added.
- All live Tours-menu tours expose the transport HUD and controls.
- Speed affects tour timing only.
- Back uses one baseline plus fast prefix replay.
- Filesystem writes, process exit, audio playback position, and other external effects cannot be reversed. The current built-in tours do not use those actions; future authors must avoid them in rewindable tours.
- Time-driven systems are not simulated during fast seek. They resume from restored baseline state after arrival.
- Escape and ordinary cancellation preserve the current tour result; only Back and Done-restart restore the baseline.
- The baseline remains allocated through Paused and Done and is destroyed on Escape, cancellation, failed start, replacement by another tour, or application shutdown.
