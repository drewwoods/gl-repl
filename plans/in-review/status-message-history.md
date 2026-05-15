# Status-message history (recent messages viewer) — decision pending

Status: **in-review** — feasible and low-risk to build, but the trigger
and surface are UX forks not yet decided. Do not implement until a
direction is chosen and the file moves to `not-started/`.

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
  discoverable. — *recommended.*
- **Keybinding / menu / pin.** Pro: steadiest, no geometry coupling.
  Con: least discoverable.

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

## Recommendation

Build it (cheap, useful), with **click-to-toggle + inline stacked list +
16-entry session ring, newest-at-bottom, age-dimmed, consecutive-dup
collapse**. Reserve the `tabbed_overlay` route only if full scrollback
is later wanted.

## If approved (sketch)

- `src/ui/state.{c,h}` (+ `state_types.h`): `UiStatusEntry` /
  `UiStatusHistory` ring; push in `ui_state_status_set`; reset in
  `ui_state_reset`. Accessor `ui_state_status_history()`.
- `src/ui/snapshot.h` + `glr_ctrl_build_ui_snapshot`: copy the ring
  by value.
- `src/ui/panels.c`: a pure hover/hit helper for the status hot-zone
  (returns whether the peek is open, given `snap->pointer` or a toggle
  flag) + the inline-list renderer; wire the toggle through the
  existing code-panel router (a new `UiHit` kind or reuse CHROME +
  controller toggle state).
- Tests: `tests/test_ui_status_history.c` (ring order/cap/dedup, pure)
  + hover-zone geometry if exposed as a pure fn.
- Docs: CLAUDE.md File Layout / MODULES.md if a new module lands.
- Verify `make test`, `make test-stubs`, UI boundary guards; `make
  gl-tests` if any gl2d bracket is added.

## Folder note

`plans/in-review/` = contested-direction. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
