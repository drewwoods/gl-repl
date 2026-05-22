# `src/widgets` — the peer subsystems

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../MODULES.md`](../../MODULES.md); the per-frame pipeline narrative
> is in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). This README is the
> module-local view: what a "peer subsystem" *is* and what each one does
> inside this app.

## What this is, in general

In a layered app, not every interactive feature belongs to the document
model or the view. A **peer subsystem** (here, a "widget") is a
self-contained feature that owns **its own state and its own controller**,
plugs into the app through the same routed-input and rendered-snapshot
machinery as everything else, and is otherwise independent of the editor and
the language core. It is the same idea as a **plugin** or a **feature
module**: a vertical slice you could add or remove without surgery on the
core.

The defining rule for this directory: a widget **owns its state and mutates
it directly**; the editor does not know the widget exists, and UI may
*render* it but does not own it. Input reaches a widget by being *routed* to
it (by `src/app/glr_ctrl.c`), not by the editor delegating.

The widgets here:

- **Replay** — a step-by-step execution visualizer (a tiny transport: play /
  pause / step, a program counter, speed, and a fade-batch ring so old
  geometry fades as new geometry appears).
- **Variable panel** — floating sliders that scrub the REPL's scalar
  variables, with a log/linear drag transaction that writes the new value
  back into the source line.
- **Color picker** — a floating HSV/alpha picker that rewrites the
  `glColor*` call under the cursor.
- **Tutorial** — a guided runner that feeds instruction comments, locks
  rows, gates commits, and tracks step progress.

Each widget follows the same two-file shape: a `*_state.c` that *owns the
storage* (a small struct, with capture/restore/reset and narrow accessors)
and a `*.c` runner/controller that implements the behavior.

## How it is exercised

These are app features rather than reusable libraries, so there is no
standalone `widgets_demo`. They are exercised through the full app and by the
focused unit tests under `tests/` (e.g. `test_repl_replay`,
`test_repl_var_drag`, `test_tutorial_runner`). Their *rendering* is done by
feature-UI in `src/ui` (`replay_hud.c`, `color_picker.c`,
`variable_panel.c`) reading the per-frame snapshot.

## In the REPL app

Inside the full app these are **layer 2b** of the ownership map — peers
carved out of the editor/UI so neither becomes a grab bag. The flow for each
is identical:

1. State lives in the widget's own `*_state.c` (e.g. `ReplReplayRuntimeState`,
   the variable-panel drag transaction, the tutorial step/lock state).
2. Input is routed to the widget's controller by `glr_ctrl` based on the
   `UiHit` kind (e.g. `UI_HIT_VARIABLE_SLIDER` → `variable_panel_handle_*`).
3. The widget mutates its own state; if a change must reach the program (a
   slider value, a picked color), it goes through the editor *commit*
   transaction — the one sanctioned path into `ReplState`.
4. UI renders the widget from the frame snapshot.

A peer may *produce* overlays the editor renders (replay annotations are
virtual lines), but it never *becomes* editor-owned.

## File map

| File | Responsibility |
|---|---|
| `replay.c` / `.h` | Replay state machine: PC, mode (OFF/PLAYING/PAUSED/DONE), speed, fade-batch ring |
| `replay_state.c` / `.h` | Owns `ReplReplayRuntimeState` storage + narrow accessors / snapshot view |
| `variable_panel_drag.c` / `.h` | Slider drag transaction: begin/motion/reset, linear/log value writeback |
| `variable_panel_state.c` / `.h` | Owns the variable-panel visibility flag + drag-state storage |
| `color_picker_state.c` / `.h` | Color-picker state, lifecycle, slider handlers, source-line writeback |
| `tutorial.c` / `.h` | Tutorial runner: start/exit/advance, match, locked-line guard, fade math |
| `tutorial_state.c` / `.h` | Owns `TutorialRuntimeState` (active flag, step, locked lines, fade timing) |

**Boundary:** a widget owns its own state and controller. It does **not** own
editor text behavior or REPL grammar; its renderer lives in `src/ui`; its one
write path into the program is the editor commit. (The tutorial *catalog* —
the lesson content — lives in `src/repl/tutorials.c`, separate from the
runner here.)
