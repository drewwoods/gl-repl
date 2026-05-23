# Status-message history (recent messages viewer) — decision pending

Status: **in-review** — feasible and low-risk to build, but the trigger
and surface are UX forks not yet decided. Do not implement until a
direction is chosen and the file moves to `not-started/`.

## 2026-05-23 audit

Not implemented: no `UiStatusHistory`/`UiStatusEntry`/`status_history`
symbols in `src/ui/` or `src/app/`; no `tests/test_ui_status_history.c`.

**Folder mismatch noted:** the header above declares
`Status: **in-review**` but the file lives in `plans/not-started/`.
The plan content is decision-pending (UX forks 1–4 unresolved), which
matches `in-review/` semantics; consider moving it back. Out of scope
for this audit pass.

## Context

Status messages (errors, "Saved …", "Runtime load unsupported …",
audio track, etc.) flash once in the amber scene-bottom banner and fade
after ~240 frames (`ui_panels_render_scene_status`). A message can also
be overwritten before it's read. Users want to pull up *recent* messages
on demand.

Verified chokepoint: `repl_set_status` is wired via the sink table to
`ui_state_status_set()` (`glr_ctrl.c` `.status = ui_state_status_set`;
impl `src/ui/state.c:59`). **Every** message funnels through that one
function — a ring pushed there captures all of them, no call-site sweep.
A reusable scrollable text overlay already exists
(`ui_tabbed_overlay_render(UiOverlayState/UiOverlayContent)`, the F1
help shell).

## Effort (≈ 1–2 days, low architectural risk)

- Ring buffer in `ui/state.c` (fixed N × `REPL_STATUS_TEXT_MAX` +
  anim_time), pushed in `ui_state_status_set`. Headless-unit-testable
  (push N → assert order/cap), mirrors the lights/derivation tests.
- By-value `UiStatusHistory` slice on `UiRenderSnapshot`, filled in
  `glr_ctrl_build_ui_snapshot` (same pattern as `scene_tabs`; ~4 KB/frame).
- Trigger = pure read of `snap->pointer` vs the status-bar rect (the
  scene-tab hover pattern) or a keybinding — no new input plumbing,
  respects `check-ui-*` purity guards.
- Viewer: reuse `ui_tabbed_overlay_render` (single tab, lines = history)
  → mostly plumbing; or a bespoke inline list above the bar (nicer for a
  transient peek, ~80–120 render lines).

No new layering or boundary-guard exposure (reuses status chokepoint,
snapshot-view pattern, pointer-hover pattern, existing overlay renderer).

## Decision forks

### 1. Trigger
- **Hover-to-peek** near the status bar. Pro: discoverable. Con:
  flickers; needs a dwell timer or it's twitchy.
- **Click-to-toggle** the status area (hover only highlights it as
  clickable). Pro: steady, no flicker, simple. Con: slightly less
  discoverable.
- **Persistent "messages" button** fixed at the bottom of the screen;
  click it to toggle the history. Pro: always-visible affordance —
  discoverability solved outright; clear, obvious purpose; steady (no
  hover flicker). Con: adds a permanent screen element + a real
  hit-region; small bespoke render. — *recommended (see fork 4).*

### 2. Surface
- **Inline stacked list** just above the bar (newest at bottom, fades
  by age, capped visible rows). Pro: matches the "recent peek" intent;
  non-modal. Con: more bespoke layout code. — *recommended.*
- **Reuse `tabbed_overlay`** (modal, scrollable). Pro: ~zero new render
  code, full scrollback. Con: heavier UX for a transient peek; blocks
  the scene.

### 3. Retention / display
- Ring size (propose **16**, session-only — no persistence).
- Show age / relative time per entry? (propose: yes, dim older).
- De-dup consecutive identical messages? (propose: collapse with a
  count, e.g. "(x3)").

### 4. Transient-message animation (ties trigger + button together)
- **Fade in/out** (current behavior). Pro: zero new code. Con: doesn't
  connect the message to any affordance; nothing teaches the user
  history exists.
- **Slide from / into the messages button.** A new transient message
  slides out of the bottom "messages" button into the banner position;
  on expiry it slides back into the button. Pro: visually *teaches*
  what the button is and where messages go — discoverability and
  purpose become self-evident; pairs naturally with the persistent
  button (fork 1). Con: replaces the fade path in
  `ui_panels_render_scene_status` with a slide transform anchored at
  the button rect (offset driven by remaining `ttl` / `anim_time`);
  modest extra render/animation code. — *recommended.*

  Mechanics sketch: button rect is the slide anchor. On set, message
  origin = button; animates to the banner slot over ~N frames; holds;
  on `ttl` low, animates back to the button and disappears "into" it.
  Reuses the existing `ttl` clock — no new timer. The button stays lit
  while messages exist / briefly pulses on new message.

## Recommendation

Build it (cheap, useful) as a cohesive unit: **persistent bottom
"messages" button (trigger, fork 1) + transient messages slide out
of / into that button instead of fading (fork 4) + click the button to
toggle an inline stacked history list (surface, fork 2) + 16-entry
session ring, newest-at-bottom, age-dimmed, consecutive-dup collapse
(fork 3)**. This makes the affordance and its purpose self-evident
(the original ask). Reserve the modal `tabbed_overlay` route only if
full scrollback is later wanted; keep plain fade as the trivial
fallback if the slide proves fiddly.

## If approved (sketch)

- `src/ui/state.{c,h}` (+ `state_types.h`): `UiStatusEntry` /
  `UiStatusHistory` ring; push in `ui_state_status_set`; reset in
  `ui_state_reset`. Accessor `ui_state_status_history()`.
- `src/ui/snapshot.h` + `glr_ctrl_build_ui_snapshot`: copy the ring
  by value.
- `src/ui/panels.c`:
  - persistent bottom **messages button** (small fixed rect; lit while
    history non-empty, brief pulse on new message) + a pure hit-test
    for it (window coords vs the button rect; returns a new `UiHit`
    kind or reuses CHROME + a controller toggle flag).
  - replace the fade in `ui_panels_render_scene_status` with a **slide
    transform**: interpolate the banner position between the button
    rect and the banner slot, parameterised by remaining `ttl` (rise
    on appear, hold, sink back into the button on expiry). Plain-fade
    fallback kept behind the same path if needed.
  - the inline stacked history list, anchored at / growing up from the
    button, shown while the toggle is open.
- Controller: a `status_history_open` toggle flipped by the button hit
  (lives with UI chrome state; snapshot-exposed for the renderer).
- Tests: `tests/test_ui_status_history.c` (ring order/cap/dedup, pure)
  + button hit-region geometry as a pure fn; slide math (position given
  ttl) as a pure fn so it's headless-unit-testable.
- Docs: CLAUDE.md File Layout / MODULES.md if a new module lands.
- Verify `make test`, `make test-stubs`, UI boundary guards; `make
  gl-tests` if any gl2d bracket is added.

## Folder note

`plans/in-review/` = contested-direction. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
