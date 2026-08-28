# Perf hint: warn when an expensive setting is costing frames

## Context

gl-repl ships several settings that are cheap to switch on and expensive to
run: the accumulation effect (an N-pass whole-scene re-render), MSAA, line
smoothing, and frame-scope Post FX. Nothing today tells a user that the
setting they flipped is why the scene got sluggish. The profiler surfaces
exist (`prof_fps_current()`, the FPS plot, the section listing), but they are
opt-in, they read as diagnostics rather than advice, and none of them names a
culprit or offers a fix.

Goal: when the frame rate stays below a threshold for a sustained interval
**and** at least one of those settings is actually in play, show a
warning readout in the code-panel status strip that names the heaviest
culprit and offers a one-click disable. Native and web share one code path.

Deliberately out of scope: warning about a heavy scene with no expensive
setting on (nothing actionable to offer), and any automatic degrade — the app
never changes a setting the user did not ask it to.

## Design

Three pieces, following the bands the project already draws.

### 1. Watchdog — `src/app/glr_perf_hint.{c,h}` (new)

A small controller-band module with no GL and no UI dependency. Its tick takes
everything it needs by value, so it is fully deterministic and unit-testable
with no clock hook:

```c
typedef enum {
    GLR_PERF_CULPRIT_NONE = 0,
    GLR_PERF_CULPRIT_ACCUM,        /* N-pass re-render: heaviest */
    GLR_PERF_CULPRIT_POST_FX,      /* full-frame filter pass */
    GLR_PERF_CULPRIT_MSAA,
    GLR_PERF_CULPRIT_LINE_SMOOTH,
    GLR_PERF_CULPRIT_COUNT
} GlrPerfCulprit;

/* What the watchdog is allowed to blame this frame. Flat by value. */
typedef struct {
    int use_accum;          /* GlrRenderState.use_accum - see web note below */
    int accum_effect;       /* Render3dAccumEffect */
    int accum_passes;       /* resolved ladder step */
    int multisample_enabled;
    int line_smooth_enabled;
    int post_fx_scope;      /* GLR_POST_FX_SCOPE_* */
} GlrPerfHintInputs;

typedef struct {            /* snapshot-safe flat view */
    int active;
    int fps;                /* rounded, at trip time */
    int culprit;            /* GlrPerfCulprit - the heaviest one on */
    int culprit_count;      /* how many expensive settings are on */
} GlrPerfHintView;

void            glr_perf_hint_tick(double fps, double dt_us,
                                   const GlrPerfHintInputs *in);
GlrPerfHintView glr_perf_hint_view(void);
const char     *glr_perf_hint_culprit_label(int culprit,
                                            const GlrPerfHintInputs *in);
void            glr_perf_hint_dismiss(void);   /* session dismiss, this culprit set */
void            glr_perf_hint_reset(void);     /* re-arm + clear the debounce */
void            glr_perf_hint_set_suppressed(int suppressed);
```

Behaviour:

- **Culprit mask.** Each frame the tick builds a bitmask of expensive settings
  that are on. Accum counts only when `use_accum && accum_effect != OFF &&
  accum_passes > 1` — the same gate `prepare_aa`
  (`src/ui/app/repl_code_panel_statusbar.c:241`) and the blur hook
  (`src/app/glr_ctrl.c:3043-3055`) already use. This is what keeps the web
  build honest: `glr_ctrl_set_accum()` (`glr_ctrl.c:5088`) forces `use_accum =
  0` when the context has no accumulation buffer, so an unpatched gl4es build
  must never be blamed for a setting that is doing nothing. Post FX counts
  when `post_fx_scope != GLR_POST_FX_SCOPE_OFF`.
- **Trip.** Accumulate `dt_us` while `fps < GLR_PERF_HINT_FPS_TRIP` (50) and
  the mask is non-empty; trip once the accumulator passes
  `GLR_PERF_HINT_TRIP_US` (2 s). Accumulating real elapsed time rather than
  counting frames matters here — 120 frames at 40 fps is 3 s, not 2 s.
- **Release with hysteresis.** Clear after the same 2 s sustained above
  `GLR_PERF_HINT_FPS_CLEAR` (55), so the readout does not blink on and off for
  a scene sitting on 50. Also clears immediately when the mask empties.
- **Culprit ranking** is a static table in the `.c`, heaviest first: ACCUM >
  POST_FX > MSAA > LINE_SMOOTH. `view.culprit` is the highest-ranked bit set;
  `culprit_count` lets the readout say "+2 more".
- **Dismiss** records the mask that was dismissed. The hint re-arms when the
  mask changes (the user turns something else on) or on `glr_perf_hint_reset()`.
  Session-scoped, mouse-only — no `GlrConfigKey`, so no `@cfg` slug and no
  golden churn, the same precedent the assign-plot chips set.
- **Suppression** (`glr_perf_hint_set_suppressed`) forces the hint off. The
  controller drives it from `glr_pointer_script_active()`
  (`src/app/glr_pointer_script.h:216`) so a tour or a `scripts/record-gif.sh`
  run never bakes an amber warning into a recording, and
  `src/app/boot/glr_capture_env.c` calls it for headless capture runs
  (boot → controller is the allowed direction under `check-app-boot-band`).

Constants live beside the module in `glr_perf_hint.h`, not in `config.h` —
they are this module's policy and nothing else reads them.

### 2. Controller wiring — `src/app/glr_ctrl.c`

- **Tick** in `glr_frame_begin()` (`glr_ctrl.c:3091`), immediately after the
  existing `PROF_FRAME_WAIT` derivation, where `prof_frame_tick()` has already
  refreshed the interval. Feed it `prof_fps_current()`,
  `prof_frame_interval_last_us()`, and a `GlrPerfHintInputs` filled from
  `glr_state_render()` + `glr_state_presentation()`.
- **Snapshot** — three flat fields on `UiRenderSnapshot`
  (`src/ui/app/snapshot.h`), filled in `glr_ctrl_build_ui_snapshot()`
  (`glr_ctrl.c:2539`) right beside the existing `unbalanced_warning` block at
  `:2570`, which is the exact precedent for a controller-derived warning
  string:

  ```c
  int  perf_hint_active;
  int  perf_hint_fps;
  int  perf_hint_culprit;          /* GlrPerfCulprit, so prepare_aa can match */
  char perf_hint_label[24];        /* "Accum Blur 8x", "MSAA", "Post FX", ... */
  ```

  Keep it flat/by-value (`check-views-flat`).
- **Fix action** — a new `glr_ctrl_perf_hint_apply_fix()` mapping culprit to a
  single `glr_config_set()` call (`ACCUM_EFFECT` → OFF, `POST_FX_SCOPE` → OFF,
  `MSAA` → 0, `LINE_SMOOTH` → 0), then `repl_set_status()` confirming what it
  turned off (so the change lands in the message history ring) and
  `glr_perf_hint_reset()` so the watchdog re-measures instead of instantly
  re-tripping on the next-ranked culprit.
- **Reset** at the documented wholesale-replacement cluster — the
  `editor_undo_clear()` call sites: `glr_ctrl_reset_all`, the F12 cycle, and
  the load-example / load-scene / load-workspace actions in `glr_actions.c`.

### 3. Status strip — `src/ui/app/repl_code_panel_statusbar.c`

Three new rows in `k_statusbar_items[]` (`:858`), at the head of the CENTER
cluster so the warning sits immediately left of the AA readouts it accuses.
The file's own rule is "one row + prepare (and draw)"; `prepare_unbal` /
`draw_unbal` (`:208` / `:403`) are the existing `UI_TOK_STATUS_WARN` template.

| row | content | hit |
|---|---|---|
| readout | `prepare_perf` / `draw_perf` → `"! 38 fps  Accum Blur 8x"` in `UI_TOK_STATUS_WARN` | `UI_HIT_NONE`, tooltip explains |
| fix | `gl2d_chip_action("off")` | `UI_HIT_CODE_PERF_HINT_FIX` |
| dismiss | `gl2d_chip_action("x")` | `UI_HIT_CODE_PERF_HINT_DISMISS` |

Both are one-shots, so both are **action** chips per the documented chip
grammar (`src/ui/core/gl_2d.h`) — a verb-labelled chip must not carry a mode.
All three get `cull_rank = 0` (never culled, like the left cluster); the
existing center items already rank 1–5 and so drop first under width pressure,
which is the behaviour we want.

Culprit highlight: add `int warn` to `StatusbarPrepared` (`:60`). `prepare_aa`
and `prepare_aa_passes` set it when `snap->perf_hint_culprit ==
GLR_PERF_CULPRIT_ACCUM`, and `draw_state_text` picks `UI_TOK_STATUS_WARN` over
`statusbar_state_color(p->active)` when it is set. MSAA and line smooth have no
statusbar chip of their own — the readout naming them is their whole surface.

Two new `UiHitKind` values in `src/ui/app/hit.h` beside
`UI_HIT_CODE_AA_STATUS` (`:24`), and two router arms in
`src/app/glr_ctrl_router.c` beside the existing `UI_HIT_CODE_AA_STATUS` cases
(`:1686` and `:2210`) → `glr_ctrl_perf_hint_apply_fix()` and
`glr_perf_hint_dismiss()`.

## Files touched

| File | Change |
|---|---|
| `src/app/glr_perf_hint.{c,h}` | **new** — watchdog, ranking, dismiss, suppression |
| `src/app/glr_ctrl.c` | tick in `glr_frame_begin`, snapshot fill, `glr_ctrl_perf_hint_apply_fix()`, reset call sites |
| `src/app/glr_ctrl.h` | declare the fix entry point |
| `src/app/glr_actions.c` | `glr_perf_hint_reset()` at the load-example/scene/workspace actions |
| `src/app/glr_ctrl_router.c` | two hit arms |
| `src/app/boot/glr_capture_env.c` | suppress during headless capture runs |
| `src/ui/app/snapshot.h` | four flat fields |
| `src/ui/app/hit.h` | two `UiHitKind` values |
| `src/ui/app/repl_code_panel_statusbar.c` | three table rows + prepares/draws, `StatusbarPrepared.warn` |
| `tests/test_glr_perf_hint.c` | **new** |
| `tests/test_ui.c` | statusbar hit-kind + amber-trace cases |
| `Makefile` | `TEST_BINS += test_glr_perf_hint` (sources are wildcarded, no other edit) |
| `docs/USER_GUIDE.md` | what the readout and its two chips mean |

## Tests

`tests/test_glr_perf_hint.c` — pure, no GL, driven entirely through
`glr_perf_hint_tick(fps, dt_us, &in)`:

- does not trip below threshold when the culprit mask is empty
- does not trip before 2 s of accumulated sub-threshold time; trips just past it
- hysteresis: 52 fps after tripping keeps the hint up; 56 fps for 2 s clears it
- ranking: accum + MSAA both on ⇒ `culprit == GLR_PERF_CULPRIT_ACCUM`,
  `culprit_count == 2`
- accum with `use_accum == 0` (the web / no-accum-buffer case) is not blamed;
  accum with `accum_passes == 1` is not blamed
- dismiss hides it; the same mask stays hidden; changing the mask re-arms;
  `glr_perf_hint_reset()` re-arms
- `glr_perf_hint_set_suppressed(1)` forces inactive regardless of input

`tests/test_ui.c` — extend the two existing statusbar patterns:

- hit-kind sweep across the strip (`:964-1023` model) returns
  `UI_HIT_CODE_PERF_HINT_FIX` / `_DISMISS` at the chips' rects
- GL-stub trace (`statusbar_trace_count_pair`, `:104`) asserts the readout
  emits `UI_TOK_STATUS_WARN`'s `glColor4f`, and that the AA readout switches
  from `statusbar_state_color` to the warn hue when accum is the culprit
- `test_chip_grammar`'s well-counting model: the two new chips are action
  chips, so they draw **no** well

## Verification

```bash
make test-stubs >/tmp/.../t.log 2>&1; grep -nE 'FAIL|Error|error:' /tmp/.../t.log
make check-state-ownership          # includes check-c99, check-include-style, check-views-flat, check-app-boot-band
make test >/tmp/.../full.log 2>&1;  grep -nE 'FAIL|error:' /tmp/.../full.log
```

Web parity — the whole feature is `#ifdef`-free, so the value of the web lane
is the `__EMSCRIPTEN__` side of the accum gate and wasm's 32-bit pointers:

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
# Config > Rendering: Accum effect = Blur, Accum passes = 16
# wait ~2 s -> "! NN fps  Accum Blur 16x  [off] [x]" appears in the code-panel
#              status strip, with the AA/passes readouts amber
# click [off]  -> accum goes Off, status message confirms, readout clears
# click [x]    -> readout hides and stays hidden until the setting set changes
```

Web: `make web && make web-serve`, then the same steps at
`http://localhost:8000/`. Confirm that when the build has no accumulation
buffer (`accum_bits == 0`, so `use_accum == 0`) turning the Accum effect on
does **not** produce an accum culprit.

Recording check — start a tour (`./gl-repl --tour editing`) with Accum Blur 16x
already on and confirm the readout stays suppressed for the duration.
