# `src/ui` — the 2D view + hit-test layer

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../MODULES.md`](../../MODULES.md); the per-frame pipeline narrative
> is in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). This README is the
> module-local view: what a 2D UI layer *is*, how it is exercised, and what
> it does inside this app.

## What this is, in general

`src/ui` is a small **2D GUI / overlay layer**: it draws panels, menus, the
code panel, popups, sliders, and HUDs on top of the 3D scene, and it
**hit-tests** pointer positions back to those visual regions. It is built in
the **immediate-mode** spirit (à la Dear ImGui, but minimal and
fixed-function): widgets are redrawn from data every frame rather than
retained as a persistent object tree.

The architecturally important part is the **strict view/controller split**:

- **A UI renderer may draw.** It takes a read-only snapshot
  (`UiRenderSnapshot`) and produces pixels. It does not read live program or
  editor state, and it does not mutate anything.
- **A UI input handler may hit-test and return.** It computes a *neutral*
  `UiHit` (e.g. "the pointer is over code row 12, char 4" or "over the
  Replay button") and hands it back. It does **not** decide what that means
  or call into the editor/REPL.

The owning subsystem (the editor, or a `src/subsystems/` peer) then interprets
the `UiHit`. This is the classic lesson that a **view should not be a
controller**: keeping rendering and hit-testing free of policy is what lets
the same panel serve code editing, the help overlay, and the demos without
each one leaking into the view code.

This split is reflected on the filesystem with two subdirectories:

- **`core/`** — REPL-/editor-/peer-agnostic primitives. `text_panel.c`
  (reusable text panel), `tabbed_overlay.c` (modal paged reference
  card), `layout.c` and `text_layout.c` (pure geometry / wrapping),
  `text_search.c` (case-insensitive find), plus the header-only
  helpers (`gl_2d.h`, `metrics.h`, `theme.h`, `hit.h`). These TUs are
  guarded against picking up REPL / editor knowledge and are linked
  into the standalone `editor_demo` to prove they work without the
  full app.
- **`app/`** — feature-UI that knows REPL / editor / peer concepts.
  The code-panel adapter (`repl_code_panel.c`), the floating panels
  (`color_picker.c`, `variable_panel.c`, `autocomplete_panel.c`,
  `profile_panel.c`), the chrome (`menu_bar.c`, `scene_tabs.c`,
  `panels.c`), the feature HUDs (`replay_hud.c`), and the UI runtime
  state itself (`state.{c,h}`, `state_types.h`, `snapshot.h`,
  `editor.h`). All of these read frame snapshots and may carry
  one-feature vocabulary (e.g. `replay_ui_*` in `replay_hud.c`).

Dependencies are strictly one-way: `app/` may include from `core/`;
`core/` never includes from `app/`.

## How it is exercised

`src/ui` has no standalone demo of its own, but its generic core is not
untested in isolation: the generic text panel (`text_panel.c` plus its
layout/search helpers) is linked and driven by
[`tools/editor_demo/`](../../tools/editor_demo/), the standalone plain-text
editor — so the reusable view half runs without the REPL. The `scene_demo`
HUD shows the same fixed-function 2D-overlay drawing style this layer uses.
There is no `ui_demo` because UI is a *view for* other subsystems, not a
subsystem with behavior of its own.

## In the REPL app

Inside the full app this is **layer 5** of the ownership map. Each frame the
controller (`src/app/glr_ctrl.c`) builds a `UiRenderSnapshot` from
`ReplState` + `EditorState` + `UiState` + peer state and fans it out to the
`ui_*_render` functions. On input, the controller asks UI to hit-test, gets
a `UiHit` back, and dispatches it to the owning subsystem.

`UiState` (`state.c`) owns only **transient chrome**: viewport, pointer,
status-text TTL, panel visibility, the panel-divider geometry. It explicitly
does *not* own cursor blink (the editor does), program state, or text. The
code panel is assembled by a two-piece split: the generic `text_panel.c`
renders rows, and the REPL-aware `repl_code_panel.c` adapter builds those
rows from snapshots (editor buffer, command metadata, tutorial fade, replay
annotations, color transformers) and rewrites generic hits back to
source-line targets.

## File map

| File | Responsibility |
|---|---|
| `core/text_panel.c` / `.h`, `core/text_search.c` | Generic text-panel renderer + hit-test + search visuals (REPL-free) |
| `core/text_layout.c` / `.h` | Pure wrapping, row counts, cursor-row mapping (`CodeLayout`) |
| `core/layout.c` / `.h` | Pure scene / code-panel rectangle geometry |
| `core/tabbed_overlay.c` / `.h` | Generic modal tabbed text overlay (the F1 help shell) |
| `core/gl_2d.h` | Header-only 2D OpenGL helpers |
| `core/hit.h` | `UiHit` / `UiHitKind` — the passive UI → controller result |
| `core/metrics.h`, `core/theme.h` | Shared layout metrics + colors |
| `app/state.c` / `.h`, `app/state_types.h` | Owns `UiState` (chrome/viewport/pointer/status TTL only) |
| `app/snapshot.h` | `UiRenderSnapshot` — the read-only per-frame bundle every renderer takes |
| `app/panels.c` / `.h` | Top-level panel bridge: code panel + status banner, prioritizes overlay/menu hits |
| `app/repl_code_panel.c` / `.h` | REPL-aware adapter: builds rows from snapshots, maps hits to source lines |
| `app/menu_bar.c` / `.h` | Menu bar, dropdowns, flyout submenus, search slot |
| `app/scene_tabs.c` / `.h` | Scene tab strip (snapshot-pure render + whole-band hit-test) |
| `app/variable_panel.c` / `.h` | Variable-slider panel chrome (the peer owns drag/visibility) |
| `app/autocomplete_panel.c` / `.h` | Completion popup renderer |
| `app/color_picker.c` / `.h` | Feature-UI: color-picker renderer + hit-test over `ColorPickerView` |
| `app/replay_hud.c` / `.h` | Feature-UI: 2D replay HUD (reads the replay peer snapshot) |
| `app/profile_panel.c` / `.h` | CPU profiling overlay |
| `app/editor.h` | `Ui*` editor-overlay snapshot types (swatches, sliders, highlights) |

**Boundary:** a UI renderer draws; a UI input handler hit-tests and returns a
`UiHit`. Neither directly mutates REPL / editor / peer state, and `ui_*` does
not include `scene_*` headers.
