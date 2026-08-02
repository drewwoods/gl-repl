# Revised Tour Transport and Backstep Plan

## Summary and Current State

Implement replay-style controls for Tours-menu tours while preserving the
environment-driven pointer-script capture mode.

Already landed:

- Step 1, `8721a4b9`: focused owner snapshot APIs.
- Step 2, `e30df777`: composite `GlrTourSnapshot`, restore synchronization,
  `glr_ctrl_after_tour_restore()`, and the passing round-trip test.
- Step 3, `37aa1d30`: `PsEvent.source_line`, catalog filename metadata, and
  catalog-time rejection of timestamped tours.
- Step 4, `fdf8e8b9`: `ps_advance_one_virtual_frame()` refactor.
- Steps 5–8, `17206ba4`: controlled-tour state machine, completion fork,
  baseline/restart lifecycle, immediate execution, and resumable backstep
  seeking. This commit also removes `glr_pointer_script_start_lines()` and
  migrates its still-relevant tests.

- Step 9, `70a3e951`: host transport-key routing before the cancel intercept.
- Step 10, `e7516251`: `tour_hud.{c,h}`, the `UiRenderSnapshot.tour` field, and
  the top-of-scene HUD render call.
- Step 11, `16201e2a`: `test_glr_tour_transport.c`, documentation
  (tours/README, MODULES, ARCHITECTURE, F1 help), and full verification.

All steps are landed; the feature is complete.

Post-plan camera rewind polish also scopes seeking as deterministic
reconstruction: simulation/view/camera ticks are suppressed for the full seek
frame, and camera eases requested by prefix events resolve immediately. This
prevents a scene load from flashing the restored baseline camera and replaying
its ease on every Back.

Post-plan HUD polish makes the overlay compact by default:
`Tour | <name> | [+]`. A click anywhere on the strip expands the original
transport details, and a click anywhere on the expanded HUD collapses it.
Canonical UI hit routing gives later overlays priority and exempts only a real
HUD press from the host's click-to-cancel rule. `hud_expanded` belongs to
controlled-tour transport metadata rather than the rewind snapshot, so the
choice survives Back while each new tour begins compact.

At the Tours-menu entrypoint, a successfully loaded tour also stops any active
REPL replay before the deferred baseline capture. This keeps replay's narrowed
execution state out of tours and their Back reconstruction; a load failure
leaves the prior replay untouched.

This is unreleased software, so there is no backward-compatibility path for
`glr_pointer_script_start_lines()`. There are exactly two run kinds:
environment capture and controlled tours.

## Explicit Playback Semantics

### Two pointer-script run kinds

Replace ambiguous combinations of `g_tour`/`g_active` with an internal run-kind enum:

```c
typedef enum {
    PS_RUN_NONE = 0,
    PS_RUN_ENV_CAPTURE,
    PS_RUN_CONTROLLED_TOUR
} PsRunKind;
```

Behavior:

| Run kind | Entrypoint | Transport/HUD | Completion |
|---|---|---|---|
| Environment capture | `glr_pointer_script_load_env()` | No | Never auto-stops; recorder owns exit |
| Controlled tour | new `glr_pointer_script_start_tour()` | Yes | Enters persistent Done |

`glr_pointer_script_tour_active()` (the user-cancel gate) is true only for
`PS_RUN_CONTROLLED_TOUR`; the env-capture kind is never canceled by user input.
The playback view reports active only for `PS_RUN_CONTROLLED_TOUR`.

### Controlled tours are untimed only

`glr_pointer_script_start_tour()` must reject any timestamped event or mixed script before capturing a baseline. Report a clear authoring error such as:

```text
Tour scripts must use untimed, completion-driven events
```

Timestamped scripts remain supported only in the environment-capture mode
(`glr_pointer_script_load_env()`).

Also update `scripts/gen_tours.py` to reject catalog scripts whose executable lines begin with timestamps. Runtime rejection remains the authoritative backstop.

### Normal event completion

During ordinary controlled playback, retain the current `PsWait` semantics exactly:

| Event | Normal completion |
|---|---|
| `glide` | Glide reaches its endpoint |
| `click`, `rightclick` | Synthesized release occurs |
| paced `key@N` | All text is delivered |
| `pause` | Pause duration expires |
| `ring` | Ring duration expires |
| `echo` | Immediate/non-blocking |
| `move`, `wheel`, plain `key`, `skey`, `chord`, `down`, `up` | Immediate |

A normal ring is therefore an authored beat. If the final event is a ring, the tour enters Done only after `PS_WAIT_RING` completes.

Echo and ripple remain decorative and never delay Done.

Immediate stepping and seeking deliberately bypass waits. A ring executed by Right Arrow or fast seek counts as complete immediately; this does not change normal playback semantics.

### Event accounting

Maintain:

```c
int g_next_event;       /* next event not yet fired */
int g_current_event;    /* active/in-flight event, or -1 */
int g_completed_events; /* count of completed events */
```

Rules:

- Firing an event does not automatically increment `g_completed_events`.
- Normal playback increments it only when the event’s `PsWait` completes.
- Immediate step/seek increments it after forcing the event to completion.
- `current_event` remains valid while a normal wait is active.
- Done requires `completed_events == event_count` and no incomplete logical event.
- Done does not wait for echo or ripple decoration.
- Entering Done releases any held scripted mouse button.

This prevents a normal still-animating ring from being counted as complete while retaining immediate-step behavior.

## Transport State and Controls

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

Add:

```c
int glr_pointer_script_start_tour(
    const char *name,
    const char *file,
    const char *const *lines,
    int line_count);

GlrTourPlaybackView glr_pointer_script_tour_view(void);

int glr_pointer_script_handle_tour_key(unsigned char key);
int glr_pointer_script_handle_tour_special(int key);
```

### Complete control-state table

| Input | Baseline pending | Playing | Paused | Stepping/Seeking | Done |
|---|---|---|---|---|---|
| Space | Consume, no-op | Pause without completing current event | Resume | Consume, no-op | Restore baseline and replay from start |
| Right | Consume, no-op | Force current event complete, or execute next if between events; then pause | Execute next immediately; remain paused | Consume, no-op | Consume and report “at end” |
| Left | Consume, no-op | Seek to boundary before the current event | Seek back one completed event | Consume, no-op | Seek to `total - 1` |
| `+`, `=` | Adjust speed | Adjust speed | Adjust speed | Adjust stored speed | Adjust restart speed |
| `-` | Adjust speed | Adjust speed | Adjust speed | Adjust stored speed | Adjust restart speed |
| Escape | Stop | Stop | Stop | Stop | Stop |
| Other real key | Cancel | Cancel | Cancel | Cancel | Cancel |

Escape and ordinary cancellation preserve the current application result and destroy the baseline.

### Speed model

Use discrete speeds:

```text
0.25×, 0.5×, 1×, 2×, 4×, 8×, 16×
```

Default to `1×`.

Maintain:

```c
float g_speed;
float g_frame_credit;
int   g_frame; /* virtual tour frame */
```

While Playing:

```text
frame_credit += speed
while frame_credit >= 1:
    advance one virtual tour frame
    frame_credit -= 1
```

Do not advance virtual frames while Paused, Done, Baseline Pending, or Seeking. Speed affects only pointer-script timing, not animation `t`, camera easing, REPL replay, status TTL, or audio.

## Immediate Event Execution

Create one helper used by both Right Arrow and fast seeking:

```c
static int ps_finish_event_immediate(const PsEvent *event);
```

Behavior:

| Event | Immediate execution |
|---|---|
| `move` | Dispatch target position |
| `glide` | Resolve once and dispatch smoothstep samples across the original duration, including the exact endpoint |
| `click`, `rightclick` | Move if needed, press, then release synchronously |
| `down` | Press and leave held |
| `up` | Move if needed, then release |
| `wheel` | Dispatch once |
| `key`, `key@N` | Deliver the complete payload synchronously |
| `skey`, `chord` | Dispatch once |
| `pause` | Skip duration |
| `ring`, `echo` | Create the overlay but mark the event complete immediately |

A paused boundary after `down` may intentionally retain the held button so subsequent stepped glide/up events reproduce a drag. Escape, Back, cancellation, and Done must release it.

If the final event is `down`, it is marked complete and Done immediately releases the held button.

## Baseline Lifecycle and Backstep

### Baseline capture

`glr_pointer_script_start_tour()`:

1. Stops and frees any previous run.
2. Parses the script with “untimed required.”
3. Stores tour name/file metadata.
4. Enters `GLR_TOUR_BASELINE_PENDING`.
5. Does not capture or execute an event yet.

On the next `glr_pointer_script_frame()`:

1. The Tours menu has completed its normal close path.
2. Capture `GlrTourSnapshot`.
3. Seed scripted pointer coordinates from `ui_state_pointer()`.
4. Enter Playing.
5. Wait until the next virtual frame before firing event zero.

On allocation failure, stop the tour and report `Tour could not capture rewind state`.

Retain the baseline through Playing, Paused, Seeking, and Done. Destroy it on stop, cancellation, failed start, replacement tour, or shutdown.

### Backstep target

```c
if (state == GLR_TOUR_DONE)
    target = max(0, event_count - 1);
else if (g_current_event >= 0)
    target = g_completed_events;       /* before in-flight event */
else
    target = max(0, g_completed_events - 1);
```

### Seek procedure

1. Enter Seeking.
2. Release held/pending mouse state and clear in-flight typing/glide/waits.
3. Restore `GlrTourSnapshot`.
4. Call `glr_ctrl_after_tour_restore()`.
5. Reset script cursor/event counters to zero.
6. Fast-execute events `[0, target)`.
7. Enter Paused at `completed_events == target`.

Process at most 32 events per rendered frame. With `PS_MAX_EVENTS == 256`, a full seek requires at most eight rendered frames.

Suppress all decorative overlays during the prefix sweep (no strobing as the seek blasts through 32 events/frame), then **reconstruct** the landing overlays once settled: `ps_tour_restore_landing_overlays()` re-walks `[0, target)` summing each event's intrinsic playback cost into a virtual landing frame, anchors `g_frame` there, and re-creates whatever live playback would still be showing - the most-recent still-live caption (`echo`) and click ripple, plus a ring shown fresh when the boundary lands directly on one. This is what makes rewinding into a caption's on-screen window bring the caption back, rather than only showing the final replayed event's overlay.

`glr_ctrl_tick()` may still be scheduled by the host during a rendered seek
frame. It must consult
`glr_pointer_script_tour_suppresses_app_tick()` and suppress animation `t`, REPL
replay, view-transition, and camera advancement, including on the frame whose
seek changes state to Paused. UI/status/audio housekeeping may continue.

Wrap prefix execution in the camera owner's reconstruction scope. Within that
scope, newly requested camera eases snap to their destination while a
pre-existing target restored from the baseline remains frozen unless a prefix
event replaces it. This avoids rendering the baseline angle between restore and
the reconstructed scene-camera target without adding per-event camera
snapshots.

### Emscripten shell events

A `shell:` click schedules a DOM callback asynchronously. After executing one during Stepping or Seeking:

- end the current rendered-frame chunk immediately;
- retain the target and event cursor;
- resume on the next rendered frame.

The same resumable 32-event seek loop therefore provides the required browser event-loop yield; do not build a separate web-only seek mechanism.

## Done and Completion

Completion is a two-way fork on run kind:

```text
PS_RUN_ENV_CAPTURE:
    never auto-stop (the shared env/capture frame path has no stop check;
    the recorder owns exit)

PS_RUN_CONTROLLED_TOUR:
    when the current logical event has completed and completed == count:
        release any held button
        clear in-flight wait/release/typing state
        enter GLR_TOUR_DONE
        retain baseline, HUD, cursor, and allowed decorative overlay
```

Do not call `glr_pointer_script_stop()` when a controlled tour reaches Done.
With `start_lines()` gone, the env-capture path keeps no auto-stop at all.

## Catalog and Source Metadata

The landed `PsEvent.source_line` metadata uses both assignments:

- environment loader: physical `lineno`;
- in-memory loader: `i + 1`.

Update generated `TourEntry`:

```c
typedef struct {
    const char        *name;
    const char        *file;
    const char *const *lines;
    int                line_count;
} TourEntry;
```

`gen_tours.py` emits the catalog-relative `.pointer` filename. `glr_tours_start()` calls `glr_pointer_script_start_tour()` with name, file, lines, and count.

The HUD source line is:

- active event’s line during Playing/Stepping;
- next event’s line while paused before it;
- final event’s line in Done;
- `-1` only when no controlled tour exists.

The HUD step number must identify the same logical event as `source_line`:
`current_event + 1` while an event is active, `completed_events + 1` at a
paused between-event boundary, and `total_events` in Done. Clamp the displayed
value to `[1, total_events]`.

## Input Routing

In `gl_repl.c`, change keyboard and special callbacks to:

```text
if controlled-tour transport handler consumes:
    return

if existing tour cancel intercept fires:
    return

route to normal controller
```

Mouse-down and wheel remain cancel actions. Passive pointer motion remains non-canceling.

Synthetic script keys still call `glr_ctrl_*` directly and never pass through these host callbacks, so tour-authored Space/arrows/`+`/`-` do not collide with transport controls.

## HUD

Add:

- `src/ui/subsystems/tour_hud.c`
- `src/ui/subsystems/tour_hud.h`

Add `GlrTourPlaybackView tour` to `UiRenderSnapshot` and populate it in `glr_ctrl_build_ui_snapshot()`.

Render a compact-by-default HUD at the top of the scene viewport, separate from
the bottom REPL replay HUD:

```text
Tour | Editing Basics | [+]
```

Clicking it expands the detailed transport surface:

```text
Tour  Editing Basics | Paused | 4× | Step 17 / 43 | editing-basics.pointer:26  [-]
[progress]
Space play | ← back | → step | +/- speed | Esc exit
```

Requirements:

- read only from `UiRenderSnapshot`;
- use existing theme tokens/fonts;
- return a passive `UI_HIT_TOUR_HUD` for the exact rendered bounds;
- let controller/host routing toggle compact/expanded without canceling;
- keep the presentation bit outside the rewind baseline;
- no new profiling section;
- render before compositor post-processing;
- leave the existing pointer/ring/echo overlay after compositing so it remains visually topmost.

## Remaining File Work

### New files

- `src/ui/subsystems/tour_hud.{c,h}`
- `tests/test_glr_tour_transport.c`

### Primary modifications

- `gl_repl.c`: transport-before-cancel routing.
- `src/ui/app/snapshot.h`: playback view.
- `src/app/glr_ctrl.c`: snapshot population and HUD render call.
- `Makefile`: add `test_glr_tour_transport` under GL-stub tests.
- `tours/README.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`, and help text.

### Already landed; do not reimplement

- Steps 1–2 (`8721a4b9`, `e30df777`): owner snapshot APIs,
  `GlrTourSnapshot`, `test_glr_tour_snapshot`, and
  `glr_ctrl_after_tour_restore()`.
- Step 3 (`37aa1d30`): source-line/catalog metadata and generator validation.
- Step 4 (`fdf8e8b9`): virtual-frame extraction.
- Steps 5–8 (`17206ba4`): run kinds, state machine, virtual clock, controls,
  immediate execution, seeking, Done, controlled-tour entrypoint, and removal
  of `glr_pointer_script_start_lines()`.

## Implementation Sequence

1. **Landed - `8721a4b9`:** add focused owner snapshot APIs.
2. **Landed - `e30df777`:** compose `GlrTourSnapshot`, add restore
   synchronization, and pass the composite round-trip gate.
3. **Landed - `37aa1d30`:** add `source_line`, catalog filename metadata, and
   catalog timed-tour rejection.
4. **Landed - `fdf8e8b9`:** extract `ps_advance_one_virtual_frame()` while
   keeping environment-script behavior unchanged.
5. **Landed - `17206ba4`:** introduce the controlled-tour run kind, playback
   state, event accounting, speed, pause, immediate Right, and Done; remove
   `start_lines()` and migrate its relevant tests.
6. **Landed - `17206ba4`:** add the two-way completion fork: environment
   capture never auto-stops; controlled tours enter Done.
7. **Landed - `17206ba4`:** integrate baseline-pending capture and Done
   restart.
8. **Landed - `17206ba4`:** add resumable Back/Seeking using the snapshot
   layer.
9. **Landed - `70a3e951`:** route host transport keys before cancellation.
10. **Landed - `e7516251`:** add the HUD and UI snapshot field.
11. **Landed - `16201e2a`:** add the focused transport test, finish
    documentation/help text, and run all verification commands.

Each stage should leave the environment-script behavior intact.

## Required Tests

### Completion semantics

- Normal final ring remains Playing until ring expiration, then enters Done.
- Right-stepped final ring enters Done immediately.
- Backstep counting agrees with both ring cases.
- Echo and ripple never delay Done.
- Normal click waits through its release frames.
- Immediate click presses/releases synchronously.
- Final `down` enters Done with no held button remaining.
- Escape from a paused post-`down` boundary releases the button.
- Back from a paused post-`down` boundary releases before restore.

### Mode compatibility

- Timestamped catalog tour is rejected (`gen_tours.py` + runtime).
- Timestamped controlled tour (`start_tour`) is rejected at runtime.
- Environment capture timing (timed + untimed) remains unchanged.
- Controlled tour enters persistent Done rather than calling stop.
- `glr_pointer_script_start_lines()` no longer exists (removed); its
  parser/paced-typing/pause/echo coverage moves to `start_tour` or the
  new transport test where still relevant.

### Control-state matrix

Test every meaningful table entry, especially:

- Right while Playing forces the current event complete and pauses.
- Right while Done is a no-op.
- Left while Playing returns before the in-flight event.
- Left from Done lands on `total - 1`.
- Space from Done restores the baseline and restarts.
- Inputs during Baseline Pending and Seeking cannot corrupt state.
- Speed changes persist through pause, seek, and Done restart.

### Seeking

- Back reconstructs editor commits, config toggles, scene changes, menus, and camera drags.
- Seek processes no more than 32 events per rendered frame.
- A `shell:` click yields and resumes on the next frame.
- Overlays are suppressed during the sweep, then the landing overlays are reconstructed: a still-live caption is restored, an expired one is not (`test_backstep_restores_live_caption` / `test_backstep_expired_caption_not_shown` in `test_tour_overlay_feedback`).
- No scene/application time advances during seek.

### Metadata and HUD

- Comments and blanks are excluded from event count.
- Physical source lines remain correct.
- Tour filename reaches the HUD.
- New tours default to compact; clicks expand and collapse the full surface.
- Expanded/collapsed choice survives Back reconstruction.
- A click outside the HUD retains the tour-cancel behavior.
- Tour and REPL replay HUDs render without overlap.
- No HUD appears for environment-capture scripts.

### Verification

```sh
make check-tours-catalog
make WEB=1 check-tours-catalog
make test_glr_tour_snapshot USE_GL_STUBS=1
make test_glr_tour_transport USE_GL_STUBS=1
make test-stubs
make gl-repl USE_GL_STUBS=1
make check-state-ownership
make check-c99
```

## Assumptions

- One executable untimed event is one tour step.
- Normal playback respects every existing `PsWait`, including ring duration.
- Immediate step and seek intentionally bypass waits.
- Back uses one baseline plus prefix replay, not per-event snapshots.
- Filesystem writes, process exit, audio position, and other external effects remain non-reversible.
- Time-driven settling is not simulated during seek.
- Camera eases authored by the reconstructed prefix resolve immediately.
- Controlled tours remain in Done until restarted, canceled, replaced, or exited.
