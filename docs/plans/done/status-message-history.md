# Status-message history (recent messages viewer)

Status: **done** - implemented 2026-06-08. Built the persistent bottom
"messages" button (trigger, fork 1) + click-to-toggle inline stacked
history list (surface, fork 2) + 16-entry session ring, newest-at-bottom,
age-dimmed, consecutive-dup collapse (fork 3). The button appears once the
session has recorded at least one message (the ring is session-retained, so
it then stays). The transient-message **slide animation (fork 4)** was kept
on the plain-fade fallback the plan sanctioned - discoverability is already
solved by the persistent button, and the slide is the fiddly part; it can be
layered on later without touching the data/hit-test work.

Landed in existing modules (no new module):
- Data: `UiStatusEntry`/`UiStatusHistory` in `src/ui/app/state_types.h`;
  push + dedup + accessors in `src/ui/app/state.{c,h}` (`ui_state_status_history`,
  `_set_open`, `_toggle`), pushed from the single `ui_state_status_set_kind`
  chokepoint.
- Snapshot: by-value `status_history` slice on `UiRenderSnapshot`, filled in
  `glr_ctrl_build_ui_snapshot`.
- Render + hit-test: messages button + history list in `src/ui/app/panels.c`;
  `UI_HIT_STATUS_HISTORY` in `src/ui/app/hit.h`; pure geometry helper
  `ui_panels_status_history_button_rect`.
- Routing: `route_status_history_hit` in `src/app/glr_ctrl_router.c`.
- Tests: `tests/test_ui_status_history.c` (ring order/cap/dedup, kind
  preservation, reset/toggle, button geometry + hit + render). All 9802
  suite tests pass; `check-state-ownership`, `check-c99`,
  `check-duplicate-api-decls`, `check-trailing-whitespace` green.

---

Original plan (feasibility verified 2026-06-08; UX forks resolved):

## 2026-06-08 review

All architectural claims verified against the live codebase. The plan
is feasible, low-risk, and well-described. Key corrections applied
below (stale paths, missing error-kind detail). UX recommendation
(persistent button + slide animation + inline list) adopted as the
direction - forks 1–4 resolved.

## 2026-05-23 audit

Not implemented: no `UiStatusHistory`/`UiStatusEntry`/`status_history`
symbols in `src/ui/` or `src/app/`; no `tests/test_ui_status_history.c`.

## Context

Status messages (errors, "Saved …", "Runtime load unsupported …",
audio track, etc.) flash once in the amber scene-bottom banner and fade
after ~240 frames (`ui_panels_render_scene_status`). A message can also
be overwritten before it's read. Users want to pull up *recent* messages
on demand.

Verified chokepoint: `repl_set_status` / `repl_set_status_error` are
wired via the sink table to `ui_state_status_set()` /
`ui_state_status_set_error()` (`src/app/glr_ctrl.c:1720–1722`,
`.status = ui_state_status_set`, `.status_error =
ui_state_status_set_error`; impl `src/ui/app/state.c:58–75`). Both
delegate to the private `ui_state_status_set_kind(msg, kind)` helper -
**every** message funnels through that one function. A ring pushed
there captures all of them (both INFO and ERROR), no call-site sweep.

Note: two callers of `ui_state_status_mut()` exist (TTL decrement in
`glr_ctrl.c:2321` and save/restore in `editor/input.c:1025`) - neither
writes `.text` from scratch, so the ring push belongs only in
`ui_state_status_set_kind`.

A reusable scrollable text overlay already exists
(`ui_tabbed_overlay_render(UiOverlayState/UiOverlayContent)` in
`src/ui/core/tabbed_overlay.{c,h}`, the F1 help shell - explicitly
feature-agnostic per its header comment).

## Effort (≈ 1–2 days, low architectural risk)

- Ring buffer in `src/ui/app/state.c` (fixed N × `REPL_STATUS_TEXT_MAX`
  (256, from `config.h`) + kind + anim_time), pushed in
  `ui_state_status_set_kind`. Headless-unit-testable
  (push N → assert order/cap), mirrors the lights/derivation tests.
- By-value `UiStatusHistory` slice on `UiRenderSnapshot`, filled in
  `glr_ctrl_build_ui_snapshot` (same pattern as `scene_tabs`; ~4 KB/frame).
- Trigger = pure read of `snap->pointer` vs the status-bar rect (the
  scene-tab hover pattern) or a keybinding - no new input plumbing,
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
  click it to toggle the history. Pro: always-visible affordance -
  discoverability solved outright; clear, obvious purpose; steady (no
  hover flicker). Con: adds a permanent screen element + a real
  hit-region; small bespoke render. - *recommended (see fork 4).*

### 2. Surface
- **Inline stacked list** just above the bar (newest at bottom, fades
  by age, capped visible rows). Pro: matches the "recent peek" intent;
  non-modal. Con: more bespoke layout code. - *recommended.*
- **Reuse `tabbed_overlay`** (modal, scrollable). Pro: ~zero new render
  code, full scrollback. Con: heavier UX for a transient peek; blocks
  the scene.

### 3. Retention / display
- Ring size (propose **16**, session-only - no persistence).
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
  what the button is and where messages go - discoverability and
  purpose become self-evident; pairs naturally with the persistent
  button (fork 1). Con: replaces the fade path in
  `ui_panels_render_scene_status` with a slide transform anchored at
  the button rect (offset driven by remaining `ttl` / `anim_time`);
  modest extra render/animation code. - *recommended.*

  Mechanics sketch: button rect is the slide anchor. On set, message
  origin = button; animates to the banner slot over ~N frames; holds;
  on `ttl` low, animates back to the button and disappears "into" it.
  Reuses the existing `ttl` clock - no new timer. The button stays lit
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

## Implementation sketch

### Data layer

- `src/ui/app/state_types.h`: new `UiStatusEntry` (text, kind, frame
  counter or monotonic id) and `UiStatusHistory` (fixed-size ring of
  `UiStatusEntry[16]`, head index, count, consecutive-dup counter).
  Entry must preserve `UiStatusKind` (INFO vs ERROR) so the viewer
  can color-code (amber vs red, matching `ui_panels_render_scene_status`).
- `src/ui/app/state.{c,h}`: push in `ui_state_status_set_kind` (the
  single private chokepoint - not in the public `_set`/`_set_error`
  wrappers); reset in `ui_state_reset`. Accessor
  `ui_state_status_history()`. Consecutive-dup detection: if new text
  matches head entry, increment count instead of pushing.
- `src/ui/app/snapshot.h` + `glr_ctrl_build_ui_snapshot` (line ~1022):
  copy the ring by value onto `UiRenderSnapshot`.

### Render layer

- `src/ui/app/panels.c`:
  - persistent bottom **messages button** (small fixed rect; lit while
    history non-empty, brief pulse on new message) + a pure hit-test
    for it (window coords vs button rect; new `UI_HIT_STATUS_HISTORY`
    in `UiAppHitKind` enum in `src/ui/app/hit.h`).
  - replace the fade in `ui_panels_render_scene_status` (line ~121)
    with a **slide transform**: interpolate banner position between
    button rect and banner slot, parameterised by remaining `ttl`
    (rise on appear, hold, sink back into button on expiry). Plain-fade
    fallback kept behind the same path if needed.
  - the inline stacked history list, anchored at / growing up from the
    button, shown while the toggle is open.
  - **Caveat**: `rename_active` and `file_prompt_active` modal strips
    take priority over status rendering - the button and history list
    must respect this suppression (same guard at panels.c that already
    gates the status banner).

### Controller layer

- `status_history_open` toggle flipped by the button hit (lives with
  UI chrome state; snapshot-exposed for the renderer).

### Tests

- `tests/test_ui_status_history.c`: ring order/cap/dedup (push N,
  assert order, overflow eviction, consecutive-dup collapse with
  count); kind preservation (push INFO then ERROR, assert both kinds
  round-trip). Button hit-region geometry as a pure fn; slide math
  (position given ttl) as a pure fn - all headless-unit-testable.

### Docs & verification

- Update `AGENTS.md` File Layout / `MODULES.md` if a new module lands.
- Must pass all UI boundary guards - in particular:
  `check-ui-no-repl-state-mut`, `check-ui-renderer-takes-view`,
  `check-ui-no-repl-state-read`, `check-ui-panels-no-mutators`,
  `check-ui-returns-hits-only`.
- Verify `make test`, `make test-stubs`, `make check-state-ownership`;
  `make gl-tests` if any gl2d bracket is added.

## Lifecycle

`not-started` → `active` (once implementation begins) → `done`.
Previously held `in-review` status while UX forks were open; resolved
2026-06-08.
