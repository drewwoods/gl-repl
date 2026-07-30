# `src/subsystems/` — the peer subsystems (Draft)

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
> module-local view: what a "peer subsystem" *is* and what each one does
> inside this app.

## What this is, in general

In a layered app, not every interactive feature belongs to the document
model or the view. A **peer subsystem** is a self-contained feature that
owns **its own state and its own controller**, plugs into the app through
the same routed-input and rendered-snapshot machinery as everything else,
and is otherwise independent of the editor and the language core. It is
the same idea as a **plugin** or a **feature module**: a vertical slice
you could add or remove without surgery on the core.

The defining rule for this directory: a subsystem **owns its state and
mutates it directly**; the editor does not know the subsystem exists, and
UI may *render* it but does not own it. Input reaches a subsystem by being
*routed* to it (by [`src/app/glr_ctrl.c`](../app/glr_ctrl.c)), not by the editor delegating.

Each subsystem lives in its own subdirectory under `src/subsystems/`,
keeping the per-feature files (state + controller) co-located. The
subsystems here:

- **`replay/`** — a step-by-step execution visualizer (a tiny transport:
  play / pause / step, a program counter, speed, and a fade-batch ring
  so old geometry fades as new geometry appears). Its fade-batch GL
  rendering lives in [`replay_render.c`](replay/replay_render.c), extracted out of `src/render3d/`.
- **`variable_panel/`** — floating sliders that scrub the REPL's scalar
  variables, with a log/linear drag transaction that writes the new value
  back into the source line.
- **`color_picker/`** — a floating HSV/alpha picker that rewrites the
  `glColor*` call under the cursor.
- **`tutorial/`** — a guided runner that feeds instruction comments, locks
  rows, gates commits, and tracks step progress.
- **`edit_overlays/`** — cursor edit-guide + vertex/normal overlay
  orchestration: owns the cursor-guide snapshot and the flat-program walk
  that drives the 3D overlay primitives (plus the GL_LINE / GL_POINT
  outline passes), extracted out of [`src/app/glr_ctrl.c`](../app/glr_ctrl.c).
- **`hidden_lines/`** — hidden-line wireframe execution: drives the REPL
  execution cursor through the render3d renderer's hidden/depth/visible wireframe
  passes while skipping pass-local state commands.
- **`buffer_viz/`** — framebuffer *inspection*: read a GL buffer back and
  composite a false-colour view of it (today `depth_viz.c`). The odd one
  out in one respect — its input is not user input but the pixels
  render3d just wrote — so it plugs in through render3d's neutral buffer
  hooks ([`Render3dRenderConfig`](../render3d/render_types.h)
  `buffer_read_fn` / `buffer_pass_overlay_fn` /
  `buffer_resolve_overlay_fn`) rather than through routed input. render3d
  fires those hooks knowing nothing about what subscribes; the controller
  supplies the per-frame modes as the hooks' `user_data`. Each viz splits
  into a **GL-free conversion core** (unit-tested against synthetic
  buffers — that is where the interesting math lives) and a thin GL
  shell around `glReadPixels` + a textured quad.

Subsystem file shapes vary: a single co-located file (like
[`color_picker_state.c`](color_picker/color_picker_state.c)), a `*_state.c` storage file plus a `*.c`
runner/controller, or a wider multi-file split where the behavior is large
(`replay/` spans playback / fade / input / render / walkers; `tutorial/`
spans runner / animation / match). The `*_state.c` always *owns the
storage* (a small struct with reset and narrow accessors; some peers also
carry capture/restore for snapshot round-trips).

For subsystems like `variable_panel`, `variable_panel_set_visible` is the canonical public visibility setter for external code, while [`variable_panel_state_mut()`](variable_panel/variable_panel_state.h#L80) provides direct mutable pointers for internal config-mapping. (Per-frame placement easing moved out of the peer: all floating panels glide via the overlay layout engine in [`src/ui/app/overlay_layout.c`](../ui/app/overlay_layout.c).)

## How it is exercised

These are app features rather than one reusable library, so there is no
single `subsystems_demo`. Most peers are exercised through the full app and
the focused unit tests under `tests/` (e.g. `test_repl_replay`,
`test_repl_var_drag`, `test_tutorial_runner`). Their *rendering* is done
by feature-UI in `src/ui/subsystems/` ([`replay_hud.c`](../ui/subsystems/replay_hud.c), [`color_picker.c`](../ui/subsystems/color_picker.c),
[`variable_panel.c`](../ui/subsystems/variable_panel.c)) reading the per-frame snapshot.

Two peers do have a standalone driver, and those are the ones whose host
seam is a **narrow installable interface** rather than the snapshot:

- **`make variable-panel-demo`** ([`tools/variable_panel_demo/`](../../tools/variable_panel_demo/)) —
  drives the peer over an in-memory `VariablePanelValueSource` and a
  hand-built `UiVariablePanelView`, so neither the REPL eval table nor
  [`UiRenderSnapshot`](../ui/app/snapshot.h#L82) is in the link set.
- **`make color-picker-demo`** ([`tools/color_picker_demo/`](../../tools/color_picker_demo/)) —
  drives the peer over a [`ColorPickerHostBridge`](color_picker/color_picker_state.h#L116)
  backed by a plain color array, standing in for the REPL document + commit
  pipeline the app wires up.

Both are guarded by `check-subsystem-demo-isolation.sh` (via
`make check-state-ownership`): the Makefile dep list, the demo's own
includes, and an `nm` sweep must all stay free of `src/app`, `src/ui/app`,
`src/repl`, and `src/editor`. If a peer ever reaches for the controller,
its demo stops linking. Index of every demo: [`tools/README.md`](../../tools/README.md).

## In the REPL app

Inside the full app these are **layer 2b** of the ownership map — peers
carved out of the editor/UI so neither becomes a grab bag. The flow for each
is identical:

1. State lives in the subsystem's own `*_state.c` (e.g.
   [`ReplayRuntimeState`](replay/replay_state.h#L96), the variable-panel drag transaction, the
   tutorial step/lock state).
2. Input is routed to the subsystem's controller by `glr_ctrl` based on
   the [`UiHit`](../ui/core/hit.h#L51) kind (e.g. `UI_HIT_VARIABLE_SLIDER` →
   `variable_panel_handle_*`).
3. The subsystem mutates its own state; if a change must reach the
   program (a slider value, a picked color), it goes through the editor
   *commit* transaction — the one sanctioned path into REPL runtime state.
4. UI renders the subsystem from the frame snapshot.

A peer may *produce* overlays the editor renders (replay annotations are
virtual lines), but it never *becomes* editor-owned.

## File map

| File | Responsibility |
|---|---|
| [`replay/replay.c`](replay/replay.c) / `.h` | Replay state machine: PC, mode (OFF/PLAYING/PAUSED/DONE), speed, fade-batch ring |
| [`replay/replay_annotations.c`](replay/replay_annotations.c) / `.h` | Replay-time source annotations / virtual lines |
| [`replay/replay_state.c`](replay/replay_state.c) / `.h` | Owns [`ReplayRuntimeState`](replay/replay_state.h#L96) storage + narrow accessors / snapshot view |
| [`variable_panel/variable_panel_drag.c`](variable_panel/variable_panel_drag.c) / `.h` | Slider drag transaction: begin/motion/reset, linear/log value writeback |
| [`variable_panel/variable_panel_state.c`](variable_panel/variable_panel_state.c) / `.h` | Owns the variable-panel visibility flag + drag-state storage |
| [`color_picker/color_picker_state.c`](color_picker/color_picker_state.c) / `.h` | Color-picker state, lifecycle, slider handlers, source-line writeback |
| [`hidden_lines/hidden_lines.c`](hidden_lines/hidden_lines.c) / `.h` | Hidden-line wireframe cursor filtering and colorless tessellation |
| [`tutorial/tutorial_runner.c`](tutorial/tutorial_runner.c), [`tutorial/tutorial.h`](tutorial/tutorial.h) | Tutorial runner public API and orchestration: start/stop/advance, locked-line guard, source-change guard |
| [`tutorial/tutorial_match.c`](tutorial/tutorial_match.c) | Tutorial command matching, normalization, expected-message formatting, and ghost-text suffix helpers |
| [`tutorial/tutorial_animation.c`](tutorial/tutorial_animation.c) / `.h` | Pure tutorial fade timing helpers |
| [`tutorial/tutorial_state.c`](tutorial/tutorial_state.c) / `.h` | Owns [`TutorialRuntimeState`](tutorial/tutorial_state.h#L44) (active flag, step, locked lines, fade timing) |

**Boundary:** a subsystem owns its own state and controller. It does
**not** own editor text behavior or REPL grammar; its renderer lives in
`src/ui/subsystems/`; its one user-driven write path into the program is the editor commit.
(Programmatic scene-setup or comment injection by subsystems like `tutorial` may
bypass the commit transaction and load directly via `repl_load_apply_line` without
generating undo history.)
(The tutorial *catalog* — the lesson content — lives in
[`src/repl/tutorials.c`](../repl/tutorials.c), separate from the runner here.)

## Lifecycle Vocabulary Conventions

To maintain a consistent and predictable API surface across all peer subsystems, we follow strict lifecycle naming standards:

1. **State Machine Session Systems (Interactive/Stateful Features):**
   Subsystems that run an active session or represent a state machine with active/inactive phases (such as `replay`, `tutorial`, and `color_picker`) must use a paired starting and stopping convention:
   - `_start()`: Allocates/initializes resources, transition states, and activates the subsystem.
   - `_stop()`: Deallocates resources, cleans up playback/history queues, and deactivates the subsystem.

2. **Togglable Display-Only Panels (Stateless/Presentation Features):**
   Subsystems or panels that act as simple toggles to show/hide UI overlays without running a separate session state machine (such as the `variable_panel`) must not use session start/stop verbs. Instead, they expose a canonical visibility setter:
   - `_set_visible(int visible)`: A Boolean-like toggle to explicitly configure the panel's presentation overlay visibility.
