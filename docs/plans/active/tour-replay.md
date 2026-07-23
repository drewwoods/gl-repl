# Revised Tour Transport and Backstep Plan

## Summary and Current State

Implement replay-style controls for Tours-menu tours while preserving both existing pointer-script modes.

Already landed:

- `6bea8dfa`: focused owner snapshot APIs.
- `d5dd9c3a`: composite `GlrTourSnapshot`, restore synchronization, and passing round-trip test.
- `glr_ctrl_after_tour_restore()` is available.
- `PsEvent.source_line` is correctly started but remains uncommitted in `src/app/glr_pointer_script.c`; include it with the next transport commit.

Remaining work is the pointer-script transport, backstep seeking, input routing, HUD, catalog metadata, compatibility tests, and documentation.

## Explicit Playback Semantics

### Three pointer-script run kinds

Replace ambiguous combinations of `g_tour`/`g_active` with an internal run-kind enum:

```c
typedef enum {
    PS_RUN_NONE = 0,
    PS_RUN_ENV_CAPTURE,
    PS_RUN_LEGACY_RUNTIME,
    PS_RUN_CONTROLLED_TOUR
} PsRunKind;
```

Behavior:

| Run kind | Entrypoint | Transport/HUD | Completion |
|---|---|---|---|
| Environment capture | `glr_pointer_script_load_env()` | No | Never auto-stops; recorder owns exit |
| Legacy runtime | `glr_pointer_script_start_lines()` | No | Existing overlay-aware auto-stop |
| Controlled tour | new `glr_pointer_script_start_tour()` | Yes | Enters persistent Done |

`glr_pointer_script_tour_active()` remains true for both runtime kinds so existing cancellation behavior continues. The new playback view reports active only for `PS_RUN_CONTROLLED_TOUR`.

### Controlled tours are untimed only

`glr_pointer_script_start_tour()` must reject any timestamped event or mixed script before capturing a baseline. Report a clear authoring error such as:

```text
Tour scripts must use untimed, completion-driven events
```

Keep timestamped support unchanged in environment and legacy `start_lines()` modes.

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

Suppress historical echo/ring/ripple overlays during the prefix. Permit overlay creation only for the final replayed event so the target boundary has useful visual context without replaying every decorative artifact.

Do not call `glr_ctrl_tick()` during seek.

### Emscripten shell events

A `shell:` click schedules a DOM callback asynchronously. After executing one during Stepping or Seeking:

- end the current rendered-frame chunk immediately;
- retain the target and event cursor;
- resume on the next rendered frame.

The same resumable 32-event seek loop therefore provides the required browser event-loop yield; do not build a separate web-only seek mechanism.

## Done and Legacy Auto-Stop

Replace the current single auto-stop block with explicit run-kind handling:

```text
PS_RUN_ENV_CAPTURE:
    never auto-stop

PS_RUN_LEGACY_RUNTIME:
    retain the current condition:
    all events fired and all glide/release/type/ring/echo/ripple effects expired
    -> glr_pointer_script_stop()

PS_RUN_CONTROLLED_TOUR:
    when the current logical event has completed and completed == count:
        release any held button
        clear in-flight wait/release/typing state
        enter GLR_TOUR_DONE
        retain baseline, HUD, cursor, and allowed decorative overlay
```

Do not call `glr_pointer_script_stop()` when a controlled tour reaches Done.

This fork is required to keep `glr_pointer_script_start_lines()` compatibility tests passing.

## Catalog and Source Metadata

The current uncommitted `PsEvent.source_line` change is correct. Keep both assignments:

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

Render a compact HUD at the top of the scene viewport, separate from the bottom REPL replay HUD:

```text
Tour  Editing Basics | Paused | 4× | Step 17 / 43 | editing-basics.pointer:26
[progress]
Space play | ← back | → step | +/- speed | Esc exit
```

Requirements:

- read only from `UiRenderSnapshot`;
- use existing theme tokens/fonts;
- no mouse hit-testing;
- no new profiling section;
- render before compositor post-processing;
- leave the existing pointer/ring/echo overlay after compositing so it remains visually topmost.

## Remaining File Work

### New files

- `src/ui/subsystems/tour_hud.{c,h}`
- `tests/test_glr_tour_transport.c`

### Primary modifications

- `src/app/glr_pointer_script.{c,h}`: run kinds, state machine, virtual clock, controls, immediate execution, seeking, Done.
- `src/app/glr_tours.c`: controlled-tour entrypoint and metadata.
- `scripts/gen_tours.py`: filename emission and catalog timed-script rejection.
- `gl_repl.c`: transport-before-cancel routing.
- `src/ui/app/snapshot.h`: playback view.
- `src/app/glr_ctrl.c`: snapshot population and HUD render call.
- `Makefile`: add `test_glr_tour_transport` under GL-stub tests.
- `tours/README.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`, and help text.

### Already landed; do not reimplement

- owner snapshot APIs;
- `GlrTourSnapshot`;
- `test_glr_tour_snapshot`;
- `glr_ctrl_after_tour_restore()`.

## Implementation Sequence

1. Commit the `source_line` extension with catalog filename metadata and timed-tour rejection.
2. Refactor the existing frame body into `ps_advance_one_virtual_frame()` while keeping environment and legacy tests unchanged.
3. Introduce run kind, controlled-tour state, event accounting, speed, pause, immediate Right, and Done.
4. Add the explicit three-way completion/auto-stop fork.
5. Integrate baseline-pending capture and Done restart.
6. Add resumable Back/Seeking using the landed snapshot layer.
7. Route host transport keys before cancellation.
8. Add the HUD and UI snapshot field.
9. Add documentation and run all guards.

Each stage should leave legacy `start_lines()` and environment-script tests passing.

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

- Timestamped catalog tour is rejected.
- Timestamped `start_lines()` remains accepted.
- Environment capture timing remains unchanged.
- Legacy `start_lines()` retains overlay-aware auto-stop.
- Controlled tour enters persistent Done rather than calling stop.

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
- Historical overlays are suppressed; the target event overlay is retained.
- No scene/application time advances during seek.

### Metadata and HUD

- Comments and blanks are excluded from event count.
- Physical source lines remain correct.
- Tour filename reaches the HUD.
- Tour and REPL replay HUDs render without overlap.
- No HUD appears for environment or legacy runtime scripts.

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
- Controlled tours remain in Done until restarted, canceled, replaced, or exited.
