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
- `scene/grid.c`, `scene/axes.c`: take an `opacity` multiplier (+ a
  `runtime` clock and animation-type enum) on their `glColor`/blend;
  only FADE implemented, fog etc. are later enum cases — no
  architecture change. Scene render already brackets GL state.
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

Pure: `(state, set_request, dt) -> state`. Renderer reads
`{draw_theme, opacity, runtime, anim_type}`. Fully headless-unit-
testable.

## Open forks

- **F1 — return-to-current during OUT.** User toggles away then back
  to `current` mid-OUT. Options: (a) **reverse** (fade back in —
  smoother), (b) **complete-then-reenter** (run to 0, then IN same
  theme — simplest, matches rule 1). Lean (a) as polish, (b) as the
  trivial default. Pick one.
- **F2 — durations.** Per-animation-type default in/out durations + the
  single global shortened-OUT used when interrupting an incomplete IN.
  Propose concrete frame counts as `config.h` constants (precedent:
  `REPL_STATUS_MESSAGE_TTL`).
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
- Tests: `tests/test_scene_transition.c` — pure: skip-ephemeral
  (rule 2), same-value-no-reset (rule 5), interrupt-IN→forced-short-OUT
  (rule 3), OUT→IN sequencing (rule 1), off↔theme (rule 6), F1 chosen
  behavior. Headless/core.
- Visual-only behaviors verified via `make gl-tests` if a gl2d/scene
  bracket is touched; otherwise manual.
- Docs: CLAUDE.md File Layout / MODULES.md for the new module.
- Verify `make test`, `make test-stubs`, `check-gl-boundaries`, UI
  guards.

## Folder note

`plans/in-review/` = contested-direction. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
