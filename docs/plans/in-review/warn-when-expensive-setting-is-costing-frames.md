# Perf hint: warn when an expensive setting is costing frames

## Context

gl-repl ships settings that are cheap to switch on and expensive to run: the
accumulation effect (an N-pass whole-scene re-render), Post FX (a full-screen
filter pass), and line smoothing. Nothing today tells a user that the setting
they flipped is why the scene got sluggish. The profiler surfaces exist
(`prof_fps_current()`, the FPS plot, the section listing), but they are opt-in,
they read as diagnostics rather than advice, and none of them names a culprit
or offers a fix.

Goal: when the frame rate stays below a display-relative threshold for a
sustained interval **and** at least one setting the user raised above its
shipped default is in play, show a warning readout in the code-panel status
strip that names the heaviest culprit and offers a one-click step back. Native
and web share one code path.

Deliberately out of scope: warning about a heavy scene with no expensive
setting on (nothing actionable to offer), and any automatic degrade — the app
never changes a setting the user did not ask it to.

## Design

Three pieces, following the bands the project already draws.

### 1. Watchdog — `src/app/glr_perf_hint.{c,h}` (new)

A controller-band module with no GL and no UI dependency. Its tick takes
everything it needs by value, so it is fully deterministic and unit-testable
with no clock hook:

```c
typedef enum {
    GLR_PERF_CULPRIT_NONE = 0,
    GLR_PERF_CULPRIT_ACCUM,          /* N-pass whole-scene re-render: heaviest */
    GLR_PERF_CULPRIT_POST_FX_FRAME,  /* filter over the whole composited window */
    GLR_PERF_CULPRIT_POST_FX_VIEW,   /* filter over the 3D view only */
    GLR_PERF_CULPRIT_LINE_SMOOTH,
    GLR_PERF_CULPRIT_COUNT
} GlrPerfCulprit;

/* What the watchdog may blame this frame, plus the per-frame suppression
 * source. Flat by value; the module reads no live state of its own. */
typedef struct {
    int use_accum;              /* GlrRenderState.use_accum - see the web note */
    int accum_effect;           /* Render3dAccumEffect */
    int accum_passes;           /* resolved ladder step */
    int line_smooth_enabled;
    int post_fx_scope;          /* GLR_POST_FX_SCOPE_* */
    int pointer_script_active;  /* tour or scripted capture is driving input */
} GlrPerfHintInputs;

typedef struct {                /* snapshot-safe flat view */
    int active;
    int fps;                    /* rounded smoothed fps, refreshed while up */
    int culprit;                /* GlrPerfCulprit - the heaviest one on */
    int culprit_count;          /* how many expensive settings are on */
} GlrPerfHintView;

void            glr_perf_hint_tick(double fps, double dt_us,
                                   const GlrPerfHintInputs *in);
GlrPerfHintView glr_perf_hint_view(void);
void            glr_perf_hint_dismiss(void);  /* session dismiss, this culprit set */
void            glr_perf_hint_reset(void);    /* re-arm + clear debounce; keep ceiling/latches */
void            glr_perf_hint_set_capture_session(int capturing);
```

#### Culprit mask — non-default is the admission ticket

A setting may be blamed only when it is set **above its shipped default**.
That rule is what keeps the "no actionable culprit ⇒ stay silent" promise
honest, and it is why MSAA is not in the set (see below).

| Setting | Default (`src/app/glr_defaults.h`) | Counts when |
|---|---|---|
| Accum | `AA`, `CFG_DEFAULT_ACCUM_PASSES` = 1 | `use_accum && accum_effect != OFF && accum_passes > 1` |
| Post FX | `GLR_POST_FX_SCOPE_OFF` | `scope == FRAME` or `scope == VIEW_3D` (two distinct culprits) |
| Line smooth | `CFG_DEFAULT_LINE_SMOOTH` = 0 | `line_smooth_enabled` |

**MSAA is deliberately excluded from this first cut.** `CFG_DEFAULT_MULTISAMPLE`
is **1** and MSAA is not in `k_cfg_scene_defaults[]`, so it survives every
example load: a mask that counted it would give every fresh session the
non-empty mask `{MSAA}` and would blame MSAA for any scene that cannot hold the
threshold — including scenes whose cost is pure geometry. That is exactly the
case this design promised to leave out of scope. Worse, we could not even prove
MSAA was doing anything: `GL_SAMPLES` is queried once at init purely to build
the menu label (`glr_ctrl.c:4891-4894`) and is never retained in state, so on a
visual that granted no multisampling `glEnable(GL_MULTISAMPLE)` costs nothing
and the watchdog would still accuse it. "You are below 50 fps, turn off MSAA"
is a different feature with a different justification; if it is wanted, it
needs its own story, and it starts by storing the queried sample count in
`GlrRenderState` and requiring `samples > 1`.

The accum gate matches `prepare_aa`'s `active` test
(`src/ui/app/repl_code_panel_statusbar.c:246-247`) and the blur-hook early-out
(`glr_ctrl.c:3043-3055`), and it respects `accum_bits == 0`: `glr_ctrl_set_accum()`
(`glr_ctrl.c:5068`) forces `use_accum = 0` on a context with no accumulation
buffer, so an unpatched gl4es web build is never blamed for a setting that is
doing nothing. Where the landed gl4es FBO patch supplies real bits, blaming
accum on web is correct.

Post FX splits into two culprits because the two scopes are two different
costs: `VIEW_3D` writes `post_filter_mode` (the 3D view only) while `FRAME`
writes `compositor_filter_mode` over the whole composited UI
(`glr_config_apply_post_fx_modes`, `glr_config.c:49`). Ranking them separately
stops a 3D-view vignette from outranking the frame-wide pass.

**Ranking**, heaviest first, as a static table in the `.c`:
`ACCUM > POST_FX_FRAME > POST_FX_VIEW > LINE_SMOOTH`. `view.culprit` is the
highest-ranked bit set; `culprit_count` lets the readout say "+1 more".

#### Threshold — relative to the cadence this display can actually reach

An absolute 50 fps floor is wrong on a 30 Hz panel or any host that never offers
60. The watchdog therefore tracks a **clean ceiling**: the highest smoothed FPS
seen on a valid frame while the culprit mask is empty. Frames with an expensive
setting active can consume that baseline, but can never lower or establish it;
otherwise a session loaded with Blur 16x at 35 FPS would learn 35 as "healthy"
and set a trip point of 28 that it can never cross.

Once a clean ceiling exists, the effective thresholds are

```
trip    = min(GLR_PERF_HINT_FPS_TRIP  (50), ceiling * GLR_PERF_HINT_TRIP_FRAC  (0.80))
release = min(GLR_PERF_HINT_FPS_CLEAR (55), ceiling * GLR_PERF_HINT_CLEAR_FRAC (0.90))
```

On a 60 Hz host the ceiling is 60 and the floor is the familiar 48–50. On a
30 Hz panel the ceiling is 30 and the trip floor is 24, so merely being on a
slow display never warns. The hysteresis gap (trip < release) is what stops the
readout blinking for a scene parked exactly on the threshold.

If the process starts with a non-empty mask (a saved `@cfg`, for example) and
has not observed even one clean frame, it falls back to the absolute 50/55
thresholds. There is no display-cadence evidence from which to derive a relative
floor in that state; treating the already-slow loaded setting as its own healthy
baseline would guarantee silence. The first later empty-mask frame begins
learning the real display ceiling.

Nothing trips during a `GLR_PERF_HINT_WARMUP_US` (3 s) warm-up, which both lets
the ceiling establish itself when clean frames are available and swallows the
startup hitch. `glr_perf_hint_reset()` restarts warm-up and clears the trip /
release accumulators and dismissed mask, but deliberately preserves the clean
ceiling and the capture-session latch. In particular, `[off]` must not erase the
baseline needed to assess the next-ranked culprit.

#### Trip, and what does not count as slowness

A tick with `dt_us <= 0` or `fps <= 0` is initialization/no sample: it may
refresh the current culprit mask, but it neither divides by the interval nor
advances warm-up, ceiling learning, or a debounce accumulator. This is the
ordinary first `glr_frame_begin()`, before two profiler ticks exist.

Accumulate `dt_us` while the mask is non-empty and **both** the smoothed `fps`
and this tick's instantaneous cadence (`1e6 / dt_us`) are below `trip`. Trip
once the accumulator passes `GLR_PERF_HINT_TRIP_US` (2 s). Requiring the last
interval too means a recovered 16.7 ms frame zeros the slow accumulator even
while `prof_fps_current()`'s EMA still reads low. Accumulating elapsed time
rather than counting frames matters: 120 frames at 40 fps is 3 s, not 2 s.
If either cadence is at or above `trip`, the trip accumulator returns to zero.

After a trip, clear after 2 s sustained with the instantaneous cadence and EMA
both above `release`; any tick that fails that pair resets the release
accumulator. **An empty mask clears immediately** and resets both
debounce accumulators: the user may turn a setting off through its ordinary
Config/statusbar control rather than the hint's `[off]`, and an active hint must
never render a stale `culprit == NONE` or an "Accum noAA 1x" label.

`dt_us` is the raw start-to-start callback interval, so a restored minimized
window, resumed debugger, or backgrounded browser tab can hand over one interval
of many seconds. A raw interval above
`GLR_PERF_HINT_DISCONTINUITY_US` (500 ms) is treated as a suspension: ignore it
for ceiling learning and zero both debounce accumulators. Valid slow frames are
otherwise accumulated at their real duration — a sustained 5 FPS render has
200 ms intervals and must trip after about 2 s, not be mistaken for ten separate
pauses. Because every accepted interval is at most 500 ms and the trip requires
2 s, no single callback can trip the hint.

The `fps` input is `prof_fps_current()`, whose EMA (`PROF_FPS_EMA_ALPHA` 0.08)
lags a recovery. The instantaneous-cadence gate plus release hysteresis bound
how long that lag can hold the readout up; the unit test pins the case (1.7 s
of low EMA/slow intervals followed by recovered 16.7 ms intervals must never
trip, even while the supplied EMA remains temporarily below the floor).

#### Dismiss and suppression

**Dismiss** records the mask that was dismissed. The hint re-arms when the mask
changes (the user turns something else on) or on `glr_perf_hint_reset()`.
Session-scoped and mouse-only — no `GlrConfigKey`, so no `@cfg` slug and no
golden churn, the precedent the assign-plot chips set.

**Suppression has two independent sources, OR'd inside the module**, and no
single overwriteable flag — a per-frame poll writing 0 over a boot-time 1 would
bake the amber readout into screenshots:

- `GlrPerfHintInputs.pointer_script_active` — polled per tick by the controller
  from `glr_pointer_script_active()` (`src/app/glr_pointer_script.h:216`), true
  for tours and `GLR_POINTER_SCRIPT` runs. It rides the tick inputs precisely
  so it cannot be confused with the other source.
- `glr_perf_hint_set_capture_session(1)` — a latched session bit set once from
  `src/app/boot/glr_capture_env.c` and never cleared by the poll. This is the
  source that covers a still `scripts/docs-assets.sh` capture or a
  `GLR_ACCUM_PASSES=16` shot with no pointer script at all — the very runs that
  raise accum passes. (boot → controller is the allowed direction under
  `check-app-boot-band`.)

  `glr_capture_env_apply()` does **not** imply capture by itself; it runs on
  every boot. It latches the bit only when at least one backend capture variable
  is non-empty: `FREEGLUT_CAPTURE_FRAMES`, `FREEGLUT_CAPTURE_STREAM`, or
  `FREEGLUT_CAPTURE_FILE`. The last one is required for the one-shot
  `scripts/docs-assets.sh` path, which sets a filename prefix and later sends
  `SIGUSR1` without record mode. `GLR_ACCUM_PASSES`, `GLR_NO_INPUT`, and the
  other posing variables are not capture proof on their own. A bare later
  `SIGUSR1` with no capture environment cannot be predicted at bootstrap and is
  deliberately outside this latch.

While either source is active, a tick forces `active = 0`, clears both debounce
accumulators, restarts warm-up, and does not learn a ceiling. Thus a tour cannot
quietly accrue two seconds of slow frames and reveal the warning on the first
unsuppressed frame after it ends. The capture-session bit remains latched for
the process; `glr_perf_hint_reset()` does not clear it.

Constants live in `glr_perf_hint.h`, not `config.h` — they are this module's
policy and nothing else reads them.

### 2. Controller wiring — `src/app/glr_ctrl.c`

- **Tick** in `glr_frame_begin()` (`glr_ctrl.c:3100`), immediately after the
  existing `PROF_FRAME_WAIT` derivation, where `prof_frame_tick()` has already
  refreshed the interval. Feed it `prof_fps_current()`,
  `prof_frame_interval_last_us()`, and a `GlrPerfHintInputs` filled from
  `glr_state_render()`, `glr_state_presentation()`, and
  `glr_pointer_script_active()`.
- **Snapshot** — four flat `int` fields on `UiRenderSnapshot`
  (`src/ui/app/snapshot.h`), filled in `glr_ctrl_build_ui_snapshot()`
  (`glr_ctrl.c:2539`) beside the existing `unbalanced_*` block (`:183-188`
  in the struct, filled at `:2570`), which is the precedent for a
  controller-derived warning:

  ```c
  int perf_hint_active;
  int perf_hint_fps;
  int perf_hint_culprit;        /* GlrPerfCulprit, so prepare_aa can match */
  int perf_hint_culprit_count;
  ```

  **No label string.** `prepare_perf` composes the text from
  `perf_hint_culprit` plus the live `snap->render`, so a user lowering the pass
  count without changing the mask cannot leave a stale "8x" on screen. The
  three non-accum culprits are fixed strings; accum reads
  `snap->render.accum_effect` / `.accum_passes` and reuses `prepare_aa`'s
  spelling ("AA" / "Blur" / "Cam"). Keeping the struct flat is a convention
  here, not a guarded one: `check-views-flat-types` scans
  `src/repl/state_views.h`, root `ui_*.h`, and two render3d headers — not
  `src/ui/app/snapshot.h`.
- **Fix action** — `glr_ctrl_perf_hint_apply_fix()` maps the culprit to a single
  `glr_config_set()`, then posts a `repl_set_status()` naming what it changed
  (so it lands in the message-history ring) and calls `glr_perf_hint_reset()` so
  the watchdog restarts its debounce rather than instantly re-tripping on the
  next-ranked culprit. Reset preserves the clean ceiling, so the remaining
  setting is measured against the same known-good cadence rather than learning
  its own degraded rate as healthy.

  | culprit | fix |
  |---|---|
  | ACCUM | `GLR_CONFIG_ACCUM_PASSES` → 1 |
  | POST_FX_FRAME / POST_FX_VIEW | `GLR_CONFIG_POST_FX_SCOPE` → `GLR_POST_FX_SCOPE_OFF` |
  | LINE_SMOOTH | `GLR_CONFIG_LINE_SMOOTH` → 0 |

  The accum fix drops **passes**, not the effect. `CFG_DEFAULT_ACCUM_EFFECT` is
  already `AA` at 1 pass, so passes → 1 lands exactly on the shipped default and
  is the same gate the watchdog uses; setting the effect to `Off` would be a
  panic-off past the default and would leave the readout claiming credit for a
  state the user never had.
- **Reset** on `glr_ctrl_reset_all()` (`glr_ctrl.c:4563`) only — that is where
  these settings actually return to their defaults — plus the mask-change re-arm
  already in the watchdog. **No reset on document load.** Production wholesale
  replacement is `editor_undo_note_wholesale_replacement()`
  (`src/editor/undo.h:138`; `editor_undo_clear()` is test-only), and it has a
  dozen live callers — both F12 paths in `glr_ctrl_cycle.c`, tutorial start,
  New Scene, workspace open, scene delete, `glr_web_io.c`'s browser New/import,
  the inline file prompt. None of them changes any of these settings: they are
  not scene-local and survive every load. Re-arming a session dismiss there
  would resurrect the readout every 2 s while a user cycles examples, for
  settings that did not change.

### 3. Status strip — `src/ui/app/repl_code_panel_statusbar.c`

Three new rows inserted at the **head of the LEFT** cluster of
`k_statusbar_items[]` (`:858`), before document stats. The strip solver never
culls LEFT rows, but it does scissor the cluster at the panel edge; putting the
warning first makes lower-priority stats disappear before the readout or its
actions in a narrow layout.
`ALIGN_LEFT` is what actually means never-culled: `statusbar_highest_cull()`
(`:1075`) skips non-CENTER items entirely, whereas its `best_rank = -1` seed
means a *center* `cull_rank = 0` row is still selected (`0 > -1`) and the three
rows would drop one at a time in table order — the readout first, orphaning
`[off] [x]` with nothing explaining them. At the supported narrow-layout test
width (360 px), all three warning rows must remain visible and hittable; below
the width of the warning itself, ordinary scissor clipping is unavoidable.
The file's own rule is
"one row + prepare (and draw)"; `prepare_unbal` / `draw_unbal` (`:208` / `:403`)
are the existing `UI_TOK_STATUS_WARN` template.

| row | content | hit |
|---|---|---|
| readout | `prepare_perf` / `draw_perf` → `"! 38 fps  Accum Blur 8x"` in `UI_TOK_STATUS_WARN` | `UI_HIT_CODE_PERF_HINT` (inert; consumed by the router) |
| fix | `gl2d_chip_action("[off]")` | `UI_HIT_CODE_PERF_HINT_FIX` |
| dismiss | `gl2d_chip_action("[x]")` | `UI_HIT_CODE_PERF_HINT_DISMISS` |

Chip labels carry their own brackets — `gl2d_chip_action`'s contract
(`src/ui/core/gl_2d.h:272`), matching assign-plot's `"[x]"` / `"[reset]"`. Hit
width is measured on the bracketed form. Both chips are one-shots, so both are
**action** chips per the documented grammar; a verb-labelled chip must not
carry a mode.

The readout needs its own hit kind rather than `UI_HIT_NONE`:
`statusbar_hover_idx()` (`:1252`) skips `UI_HIT_NONE` rows outright, and
tooltip rendering keys off that hover result, so a `UI_HIT_NONE` readout could
never show the tooltip that explains it. `UI_HIT_CODE_PERF_HINT` is inert — the
router consumes it and does nothing, the way `UI_HIT_CODE_PANEL_CHROME` is
handled. Its tooltip is where the fix is spelled out ("Accum passes → 1").

All three rows are eligible only while `snap->perf_hint_active`.

Culprit highlight: add `int warn` to `StatusbarPrepared` (`:60`). `prepare_aa`
and `prepare_aa_passes` set it when `snap->perf_hint_culprit ==
GLR_PERF_CULPRIT_ACCUM`, and `draw_state_text` picks `UI_TOK_STATUS_WARN` over
`statusbar_state_color(p->active)` when it is set. Post FX and line smooth have
no statusbar chip of their own — the readout naming them is their whole surface.

Three new `UiHitKind` values in `src/ui/app/hit.h` beside `UI_HIT_CODE_AA_STATUS`
(`:24`), and three router arms in `src/app/glr_ctrl_router.c` beside the existing
`UI_HIT_CODE_AA_STATUS` cases (`:1686` and `:2210`).

## Files touched

| File | Change |
|---|---|
| `src/app/glr_perf_hint.{c,h}` | **new** — watchdog, ranking, dismiss, two suppression sources |
| `src/app/glr_ctrl.c` | tick in `glr_frame_begin` (`:3100`), snapshot fill (`:2570`), `glr_ctrl_perf_hint_apply_fix()`, reset in `glr_ctrl_reset_all` (`:4563`) |
| `src/app/glr_ctrl.h` | declare the fix entry point |
| `src/app/glr_ctrl_router.c` | three hit arms (one inert) |
| `src/app/boot/glr_capture_env.c` | latch capture suppression only when a `FREEGLUT_CAPTURE_*` session variable is present |
| `src/ui/app/snapshot.h` | four flat `int` fields |
| `src/ui/app/hit.h` | three `UiHitKind` values |
| `src/ui/app/repl_code_panel_statusbar.c` | three LEFT-cluster rows + prepares/draws, `StatusbarPrepared.warn` |
| `tests/test_glr_perf_hint.c` | **new** |
| `tests/test_glr_capture_env.c` | capture-session predicate: record, stream, and one-shot prefix |
| `tests/test_ui.c` | dedicated perf-hint statusbar case (hit kinds + amber trace) |
| `Makefile` | `TEST_BINS += test_glr_perf_hint` (sources are wildcarded, no other edit) |
| `docs/MODULES.md` | one row for the new controller-band module |
| `docs/USER_GUIDE.md` | what the readout and its two chips mean |
| `docs/plans/in-review/README.md` | list this plan |

`glr_actions.c` is **not** touched — the load-time reset was dropped.

## Tests

`tests/test_glr_perf_hint.c` — pure, no GL, driven entirely through
`glr_perf_hint_tick(fps, dt_us, &in)` with helpers that feed consistent FPS /
interval pairs (16.7 ms at 60 FPS, 200 ms at 5 FPS, and so on):

- empty mask never trips, however slow
- MSAA-only state is not representable — the input struct has no MSAA field
  (the exclusion is structural, not a runtime branch)
- warm-up: nothing trips inside the first 3 s; clean 60 FPS frames establish a
  60 FPS ceiling during it
- first profiler tick (`fps == 0`, `dt_us == 0`) is ignored safely
- trips just past 2 s of sub-threshold time, not before
- hysteresis: **after clean warm-up at 60 FPS**, 52 FPS keeps a tripped hint up;
  56 FPS for 2 s clears it
- **display-relative threshold**: a run whose ceiling never exceeds 30 fps does
  not trip at 28 fps, but does at 20 fps
- **ceiling admission**: non-empty-mask frames cannot establish or raise the
  ceiling; when no clean sample exists, an already-loaded 35 FPS Accum setting
  is judged against the absolute fallback and trips
- **ceiling retention**: `glr_perf_hint_reset()` and an `[off]`/next-culprit
  sequence preserve the clean ceiling
- **discontinuity**: one 5 s `dt_us` tick does not trip, update the ceiling, or
  contribute to either debounce accumulator
- **very slow but valid**: consistent 5 FPS / 200 ms ticks are not discarded and
  trip after about 2 s
- **hitch recovery**: after 1.7 s of slow intervals, recovered 16.7 ms intervals
  reset the trip accumulator even while the supplied smoothed FPS remains low
- accum: `use_accum == 0` (web / no accum buffer) is not blamed; `accum_passes == 1`
  is not blamed; `accum_effect == OFF` is not blamed
- post FX: `FRAME` and `VIEW_3D` are distinct culprits and `FRAME` outranks
  `VIEW_3D`; `OFF` is not blamed
- ranking: accum + post-FX-frame both on ⇒ `culprit == GLR_PERF_CULPRIT_ACCUM`,
  `culprit_count == 2`
- dismiss hides it; the same mask stays hidden; changing the mask re-arms;
  `glr_perf_hint_reset()` re-arms
- changing the mask to empty clears `active` and both debounce accumulators in
  that tick, without waiting for release hysteresis
- **suppression, both sources independently**: `pointer_script_active = 1`
  forces inactive; and — the regression this guards —
  `glr_perf_hint_set_capture_session(1)` **followed by many ticks with
  `pointer_script_active = 0`** stays inactive throughout
- ending pointer-script suppression starts a fresh warm-up; slow frames accrued
  during the script cannot make the hint appear immediately afterward

`tests/test_glr_capture_env.c` — clear and set the three backend capture
variables independently. Each of `FREEGLUT_CAPTURE_FRAMES`,
`FREEGLUT_CAPTURE_STREAM`, and `FREEGLUT_CAPTURE_FILE` latches capture-session
suppression; a bare `glr_capture_env_apply()` and posing-only variables do not.

`tests/test_ui.c` — the existing 700 px hit-kind sweep (`:969-1026`) only sees
rows that are eligible by default, and the perf rows are eligible only while
the hint is active. Leave that sweep pinning today's strip and add a dedicated
case that sets the `perf_hint_*` snapshot fields by hand first:

- hit-test at the readout / `[off]` / `[x]` rects returns
  `UI_HIT_CODE_PERF_HINT`, `_FIX`, `_DISMISS`
- repeat the hit test at the existing 360 px narrow-layout width and assert all
  three warning rows remain visible/hittable at the head of the LEFT cluster
- GL-stub trace (`statusbar_trace_count_pair`, `:104`) asserts the readout emits
  `UI_TOK_STATUS_WARN`'s `glColor4f`, and that the AA readout switches from
  `statusbar_state_color` to the warn hue when accum is the culprit
- the rows draw nothing when `perf_hint_active == 0`
- following `test_chip_grammar`'s well-counting model
  (`tests/test_ui_assign_plot.c:448`): the two chips are action chips, so they
  draw **no** well

## Verification

```bash
make test-stubs >"$SCRATCH/t.log" 2>&1; grep -nE 'FAIL|Error|error:' "$SCRATCH/t.log"
make check-state-ownership   # includes check-c99, check-include-style, check-app-boot-band
make test >"$SCRATCH/full.log" 2>&1; grep -nE 'FAIL|error:' "$SCRATCH/full.log"
```

Web parity — the feature is `#ifdef`-free, so the web lane's value is the
`__EMSCRIPTEN__` side of the accum gate and wasm's 32-bit pointers:

```bash
make test-web
```

Real-GCC cross-check (new TU, new header):

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && make check-c99 && make test-stubs'
```

Manual, native — the honest end-to-end check, since the trip needs a genuinely
slow frame:

```bash
./gl-repl --example torus
# baseline: fresh session, defaults -> no readout, ever (this is the MSAA case)
# Config > Rendering: Accum effect = Blur, Accum passes = 16
# wait ~2 s -> "! NN fps  Accum Blur 16x  [off] [x]" at the head of the LEFT
#              cluster; AA/passes readouts amber
# hover the readout -> tooltip names the fix
# click [off]  -> passes drop to 1, status message confirms, readout clears
# click [x]    -> readout hides, stays hidden until the setting set changes
# F12 through several examples -> a dismissed hint stays dismissed
```

Web: `make web && make web-serve`, then the same steps at
`http://localhost:8000/`. Confirm that where the build has no accumulation
buffer (`accum_bits == 0`, so `use_accum == 0`) turning the Accum effect on
produces **no** accum culprit.

Recording checks — both suppression sources:

```bash
./gl-repl --tour editing            # with Accum Blur 16x on: readout stays hidden
GLR_ACCUM_PASSES=16 FREEGLUT_CAPTURE_FRAMES=120 ./gl-repl --example torus
# still capture, no pointer script -> the captured PPMs must carry no readout
```
