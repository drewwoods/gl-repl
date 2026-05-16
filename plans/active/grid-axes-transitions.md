# Grid / axes in-out transitions — decision pending

Status: **in-review** — recommended approach + model identified; a few
forks open. Do not implement until a direction is chosen and the file
moves to `not-started/`.

## Context

Grid and axes snap instantly when their theme is toggled
(`scene_grid_render` / `scene_axes_render` read `config.grid_theme` /
`config.axes_theme`; the config system just increments an `int` at a
pointer). Want animated out→in transitions (default fade; later
richer, e.g. fog near→far), interruptible, with controlled behavior
under rapid toggling.

## Hooking approach (recommended: observer/diff — low intrusiveness)

Do **not** make grid/axes "special" config keys (that carves an
exception into the uniform `ReplConfigItem`/`config_value_ptr`
pattern). Instead: the config system is **untouched** — toggling still
just flips `presentation.grid_theme`/`axes_theme`. The controller
diffs that against a per-overlay transition state each frame and feeds
the renderer an *effective* `{theme, opacity, runtime}`.

Footprint (additive, contained):
- New pure **transition state machine** module (one instance each for
  grid and axes — independent).
- `SceneRenderConfig`: per overlay, replace the single theme int with
  `{draw_theme, opacity, runtime}` derived from the machine (OUT draws
  the *old* theme fading; IN draws the *new* one).
- `scene/grid.c`, `scene/axes.c`: opacity is **not** a one-liner —
  these files mix a `gl_color_rgba` helper with ~12+ *direct*
  `glColor4f`/`glColor3f` calls (axis labels via `glColor3f`; grid
  focus/ocean/ruler/planes via `glColor4f`). Every color path —
  helper, direct calls, and text labels — must route through a single
  **opacity-aware color helper** (a per-file `gl_color`/`gl_color_rgba`
  that multiplies in the transition opacity). The transition opacity is
  applied **after** any existing `alpha_scale` (replay-fade) clamp so
  controller-owned OUT stays authoritative (rule 3). FADE only; fog is
  a later enum case. Scene render already brackets GL state. (This is
  the bulk of the implementation effort — budget accordingly, not "one
  param".)
- Per-frame `tick(dt)` in the controller pre-frame prep (with flatten/
  replay), driven by existing `anim_time`.

Effort ≈ 1–1.5 days incl. the state-machine tests. Risk low (config
path untouched; rendering opacity is the only GL change).

## State model (settled via Q&A)

Per overlay: `current` (theme being drawn / fading), `next` (latest
selection), `phase ∈ {STEADY, OUT, IN}`, `opacity` (controller-owned,
0..1), `runtime` (seconds since `current` became current).

Invariants / rules:
1. **`current` mutates only when `opacity` reaches 0** (the OUT→IN
   boundary). `next` is just the latest selection and may change freely.
2. **Rapid cycling skips ephemeral themes.** A theme that is only ever
   `next` and never becomes `current` gets no `runtime` and is never
   drawn. When OUT finishes, `current = next` *as of that instant*.
3. **Controller is history-agnostic.** It owns `opacity` outright.
   `FADE_IN` ramps it toward 1 at `fade_in_secs`; `FADE_OUT` ramps it
   toward 0 at `fade_out_secs`; both operate on *whatever opacity
   currently is* — the controller never inspects how it got there. No
   shortened-interrupt duration (dropped — rapid toggling is already
   handled by reverse + skip-ephemeral; one out-rate suffices).
4. **Renderer interprets, controller envelopes.** Grid/axes scale
   color alpha by `opacity` (hard ceiling — OUT always wins). *How*
   that becomes a visual (plain fade now; later fog approach/recede
   keyed off `phase`) lives in `scene/grid.c`/`axes.c`, NOT the
   controller — the controller does not own an anim-type. (`runtime`
   and theme-dictated in-time are **deferred** — not in the contract
   yet.)
5. **Same-value set is a no-op.** Setting `next == current` while
   STEADY or IN does nothing (runtime keeps running) — so example
   loads that reuse the current theme don't re-fade. Examples
   therefore *can* animate; the shared-theme common case simply
   doesn't re-trigger. (No special "snap on scene load" rule needed —
   load is an ordinary `next` set.)
6. **"off" needs no special case** — it's a theme index with nothing
   drawn (opacity 0); off↔theme falls out of the same machine
   (OUT-of-off is instantaneous).
7. Grid and axes are **independent** machines.
8. **Init / world-reset = snap, never animate.** A zero-initialized
   machine would diff OFF → default theme on the first frame and
   animate the default grid in at startup (`CFG_DEFAULT_GRID_THEME` is
   non-off (8); `CFG_DEFAULT_AXES_THEME` is 0/off), and animate stale
   prior-world themes after a reset. So both machines MUST be **seeded
   to the current presentation theme at full opacity, STEADY** at
   program init *and* in `glr_app_reset_all()` (`glr_ctrl.c:1675`,
   which today only resets presentation). Seeding makes rule 5 cover
   the rest (a post-reset same-theme is then a no-op). This snap is an
   explicit machine operation, not an observed diff.

Pure: `(state, set_request, dt) -> state`. Fully headless-unit-testable.

## Settled spec (decided — forks closed)

**Phase enum & scene-facing struct** (controller-owned; per overlay):

```c
typedef enum {
    SCENE_XN_STEADY = 0,   /* no ramp; opacity stable (1 shown / 0 hidden) */
    SCENE_XN_FADE_IN,      /* opacity -> 1 at fade_in_secs  */
    SCENE_XN_FADE_OUT      /* opacity -> 0 at fade_out_secs */
} SceneXnPhase;

typedef struct {
    int          theme;    /* authoritative = machine `current`; off => skip */
    float        opacity;  /* 0..1; applied AFTER alpha_scale clamp (OUT wins) */
    SceneXnPhase phase;    /* advisory direction hint (unused by FADE v1)      */
} SceneOverlayXn;          /* one grid, one axes                              */
```

- No `anim_type` field, no `runtime` field (renderer owns the visual
  taxonomy; runtime deferred).
- **F1 = reverse, free.** `set(next)`: `next==current` while
  STEADY/FADE_IN → no-op; `next!=current` → `FADE_OUT`;
  `next==current` while `FADE_OUT` → `FADE_IN` (reverse from current
  opacity). `FADE_OUT` reaching 0 with `next!=current` → `current=next;
  FADE_IN`. History-agnostic ⇒ no resume-rate decision.
- **F2 = two `config.h` float-seconds constants.**
  `GRID_AXES_FADE_IN_SECS = 0.30f`, `GRID_AXES_FADE_OUT_SECS = 0.20f`.
  (No interrupt constant — dropped.)
- **F3 = FADE only for v1.** No fog; `phase` is plumbed but unused so
  fog needs no later contract change.

## If approved (sketch)

- `src/scene/scene_transition.{c,h}` (or `src/app/`): pure machine —
  `set(next)`, `tick(dt)`, queries `draw_theme()/opacity()/runtime()/
  anim_type()`. No GL.
- Controller: one instance per overlay; `set()` from the
  presentation-theme diff each frame; `tick()` in pre-frame prep;
  write effective `{theme,opacity,runtime}` into `SceneRenderConfig`.
- `render_types.h`: per-overlay effective fields (replace bare
  `grid_theme`/`axes_theme` draw inputs).
- `scene/grid.c`, `scene/axes.c`: opacity-scaled color + `GL_BLEND`
  bracket; `runtime`/`anim_type` consumed (FADE only for now).
- Tests:
  - `tests/test_scene_transition.c` — pure machine: skip-ephemeral
    (rule 2), same-value-no-reset (rule 5), interrupt-IN→forced-short-
    OUT (rule 3), OUT→IN sequencing (rule 1), off↔theme (rule 6),
    init/reset snap (rule 8), F1 chosen behavior.
  - **Controller diff/wiring** in `tests/test_glr_ctrl.c` (it already
    captures `SceneRenderConfig` via `g_last_scene_config` /
    `test_scene_render_3d_scene`): assert effective
    `{draw_theme,opacity}` for first-frame **snap** (no animation at
    startup), a theme change driving OUT then IN, rapid toggle skipping
    ephemeral themes, and `glr_app_reset_all()` snapping both machines.
- Visual-only behaviors verified via `make gl-tests` if a gl2d/scene
  bracket is touched; otherwise manual.
- Docs: CLAUDE.md File Layout / MODULES.md for the new module.
- Verify `make test`, `make test-stubs`, `check-gl-boundaries`, UI
  guards.

## Review corrections (incorporated)

- **[P1] Init/reset snap.** Added rule 8 + the controller-wiring test:
  zero-init would animate the non-off default grid on frame 1 / stale
  themes after reset; both machines must be seeded to current
  presentation at full opacity in init and `glr_app_reset_all()`.
- **[P1] Opacity is not one-liner.** §sketch now spells out that
  `grid.c`/`axes.c` mix `gl_color_rgba` with ~12+ direct
  `glColor4f`/`glColor3f` (incl. labels); all must route through one
  opacity-aware helper, applied *after* the `alpha_scale` clamp so OUT
  stays authoritative — flagged as the bulk of the effort.
- **[P2] Test scope.** Added controller diff/`SceneRenderConfig`
  cases via `test_glr_ctrl.c`'s existing capture stub (first-frame
  snap, OUT/IN, rapid-toggle skip, reset).
- **[P3] Units.** F2 settled on float-**seconds** `config.h` constants
  (model already ticks `dt`/`runtime` seconds); frame-counts rejected.

## Progress / resumption recipe

**Step A — DONE & committed.** `config.h`
(`GRID_AXES_FADE_IN_SECS 0.30f`, `GRID_AXES_FADE_OUT_SECS 0.20f`);
`src/scene/scene_transition.{c,h}` (pure machine: `SceneXnPhase`,
`SceneXnState`, `scene_xn_init/set/tick`); `test_scene_transition`
24/24; wired in Makefile SRCS/CORE_TEST_SRCS/header list + TEST_BINS.
Build green, feature inert.

**Step B — controller wiring (next). Build-green, still inert.**
- `src/scene/render_types.h`: `#include "scene/scene_transition.h"`;
  add to `SceneRenderConfig` near the grid/axes block:
  `float grid_opacity; SceneXnPhase grid_xn_phase; float axes_opacity;
  SceneXnPhase axes_xn_phase;` (existing `grid_theme`/`axes_theme`
  ints become the *effective/current* theme).
- `src/app/glr_ctrl.c`: file-static `SceneXnState g_grid_xn,
  g_axes_xn;`. `glr_ctrl_seed_overlay_xn()` =
  `scene_xn_init(&g_grid_xn, presentation.grid_theme)` /
  `(&g_axes_xn, presentation.axes_theme)`; call from program init
  *and* `glr_app_reset_all()` (rule 8). Pre-frame prep:
  `scene_xn_set(&g_grid_xn, presentation.grid_theme)` +
  `scene_xn_tick(&g_grid_xn, dt, GRID_AXES_FADE_IN_SECS,
  GRID_AXES_FADE_OUT_SECS)` (axes likewise; `dt` = `anim_time`
  delta). In `glr_ctrl_build_scene_config` (~:1135/:1138): set
  `config->grid_theme=g_grid_xn.current; grid_opacity=g_grid_xn.opacity;
  grid_xn_phase=g_grid_xn.phase;` (axes likewise) replacing the direct
  `presentation.grid_theme` copy. Commit B.

**Step C — renderer opacity (the bulk).** `scene/grid.c`,
`scene/axes.c`: per-file static `s_xn_opacity` set at render entry
from `config.grid_opacity`/`axes_opacity`; ONE opacity-aware color
helper per file — route `gl_color_rgba` AND every direct
`glColor4f`/`glColor3f` (axis labels, indicators, grid focus/ocean/
ruler/planes) through it, multiplying final alpha by `s_xn_opacity`
**after** any `alpha_scale` clamp (OUT authoritative); `GL_BLEND`
bracket; skip drawing when effective theme == off index. `phase`
unused (fog later). Commit C — feature visible.

**Step D — tests/docs/verify.** `tests/test_glr_ctrl.c` via the
existing `g_last_scene_config`/`test_scene_render_3d_scene` capture:
first-frame **snap** (opacity 1, no startup fade), theme change →
`grid_xn_phase` FADE_OUT then FADE_IN, rapid toggle adopts latest,
`glr_app_reset_all()` snaps. CLAUDE.md/MODULES.md add
`scene_transition`. Run `make test`, `make test-stubs`,
`check-gl-boundaries`, UI guards, `make gl-tests`. Commit D; move
plan to `plans/done/`.

## Folder note

`plans/in-review/` = contested-direction. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
