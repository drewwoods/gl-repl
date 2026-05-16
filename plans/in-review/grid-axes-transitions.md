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
3. **Controller owns OUT (strict).** During OUT, `opacity` is
   authoritative — themes cannot override it. Interrupting an
   incomplete IN forces OUT as a plain fade with a **global shortened
   out duration** (handles toggle-spamming).
4. **Theme may own IN.** During IN the controller supplies a default
   rising-fade `opacity`, and also passes `runtime` so a theme can
   substitute its own in-animation (fog, etc.), clamped to [0,1] and
   instantly forced toward 0 if an OUT triggers.
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

Pure: `(state, set_request, dt) -> state`. Renderer reads
`{draw_theme, opacity, runtime, anim_type}`. Fully headless-unit-
testable.

## Open forks

- **F1 — return-to-current during OUT.** User toggles away then back
  to `current` mid-OUT. Options: (a) **reverse** (fade back in —
  smoother), (b) **complete-then-reenter** (run to 0, then IN same
  theme — simplest, matches rule 1). Lean (a) as polish, (b) as the
  trivial default. Pick one.
- **F2 — durations (units settled: seconds).** The machine ticks on
  `dt` and `runtime` in **seconds** (`anim_time` is seconds-based);
  durations MUST be float-seconds constants, NOT frame counts — mixing
  units makes interruption timing unreasonable. `config.h` float
  constants, e.g. `GRID_AXES_FADE_IN_SECS`, `_FADE_OUT_SECS`,
  `_FADE_OUT_INTERRUPT_SECS` (the global shortened-OUT). Only the
  concrete values are open.
- **F3 — animation-type registry.** Enum `{ FADE (default), ... }`;
  ship FADE only, leave the switch + `runtime` hook for fog-near→far
  later. Confirm scope (no fog now).

## Recommendation

Build via the observer/diff approach with the settled state model.
Default everything to FADE. Resolve F1 (suggest (b) for v1, (a) as a
follow-up), F2 (config.h constants), F3 (FADE-only v1).

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

## Folder note

`plans/in-review/` = contested-direction. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
