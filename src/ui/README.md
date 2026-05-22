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

The owning subsystem (the editor, or a `src/widgets` peer) then interprets
the `UiHit`. This is the classic lesson that a **view should not be a
controller**: keeping rendering and hit-testing free of policy is what lets
the same panel serve code editing, the help overlay, and the demos without
each one leaking into the view code.

A few modules are explicitly *generic* (REPL-free, guarded as such):
`text_panel.c` (a reusable text panel), `tabbed_overlay.c` (a modal paged
reference card), `layout.c` and `text_layout.c` (pure geometry / wrapping).
A few are *feature-UI* (allowed to know one feature's vocabulary):
`replay_hud.c` and `color_picker.c` carry the `replay_ui_*` /
color-picker discipline and route hits to their owning peer.

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
| `state.c` / `.h`, `state_types.h` | Owns `UiState` (chrome/viewport/pointer/status TTL only) |
| `snapshot.h` | `UiRenderSnapshot` — the read-only per-frame bundle every renderer takes |
| `hit.h` | `UiHit` / `UiHitKind` — the passive UI → controller result |
| `panels.c` / `.h` | Top-level panel bridge: code panel + status banner, prioritizes overlay/menu hits |
| `text_panel.c` / `.h`, `text_search.c` | Generic text-panel renderer + hit-test + search visuals (REPL-free) |
| `repl_code_panel.c` / `.h` | REPL-aware adapter: builds rows from snapshots, maps hits to source lines |
| `text_layout.c` / `.h` | Pure wrapping, row counts, cursor-row mapping (`CodeLayout`) |
| `layout.c` / `.h` | Pure scene / code-panel rectangle geometry |
| `menu_bar.c` / `.h` | Menu bar, dropdowns, flyout submenus, search slot |
| `scene_tabs.c` / `.h` | Scene tab strip (snapshot-pure render + whole-band hit-test) |
| `variable_panel.c` / `.h` | Variable-slider panel chrome (the peer owns drag/visibility) |
| `autocomplete_panel.c` / `.h` | Completion popup renderer |
| `tabbed_overlay.c` / `.h` | Generic modal tabbed text overlay (the F1 help shell) |
| `color_picker.c` / `.h` | Feature-UI: color-picker renderer + hit-test over `ColorPickerView` |
| `replay_hud.c` / `.h` | Feature-UI: 2D replay HUD (reads the replay peer snapshot) |
| `profile_panel.c` / `.h` | CPU profiling overlay |
| `editor.h` | `Ui*` editor-overlay snapshot types (swatches, sliders, highlights) |
| `metrics.h`, `theme.h`, `gl_2d.h` | Shared layout metrics, colors, header-only 2D GL helpers |

**Boundary:** a UI renderer draws; a UI input handler hit-tests and returns a
`UiHit`. Neither directly mutates REPL / editor / peer state, and `ui_*` does
not include `scene_*` headers.
