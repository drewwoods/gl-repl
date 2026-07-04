# `src/ui` — the 2D view + hit-test layer

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
> module-local view: what a 2D UI layer *is*, how it is exercised, and what
> it does inside this app.

## What this is, in general

`src/ui` is a small **2D GUI / overlay layer**: it draws panels, menus, the
code panel, popups, sliders, and HUDs on top of the 3D stage, and it
**hit-tests** pointer positions back to those visual regions. It is built in
the **immediate-mode** spirit (à la Dear ImGui, but minimal and
fixed-function): widgets are redrawn from data every frame rather than
retained as a persistent object tree.

The architecturally important part is the **strict view/controller split**:

- **A UI renderer may draw.** It takes a read-only snapshot
  ([`UiRenderSnapshot`](src/ui/app/snapshot.h#L70)) and produces pixels. It does not read live program or
  editor state, and it does not mutate anything.
- **A UI input handler may hit-test and return.** It computes a *neutral*
  [`UiHit`](src/ui/core/hit.h#L51) (e.g. "the pointer is over code row 12, char 4" or "over the
  Replay button") and hands it back. It does **not** decide what that means
  or call into the editor/REPL.

The owning subsystem (the editor, or a `src/subsystems/` peer) then interprets
the [`UiHit`](src/ui/core/hit.h#L51). This is the classic lesson that a **view should not be a
controller**: keeping rendering and hit-testing free of policy is what lets
the same panel serve code editing, the help overlay, and the demos without
each one leaking into the view code.

This split is reflected on the filesystem with two subdirectories:

- **`core/`** — REPL-/editor-/peer-agnostic primitives. [`text_panel.c`](src/ui/core/text_panel.c)
  (reusable text panel), [`tabbed_overlay.c`](src/ui/core/tabbed_overlay.c) (modal paged reference
  card), [`text_layout.c`](src/ui/core/text_layout.c) (pure wrapping), [`text_search.c`](src/ui/core/text_search.c)
  (case-insensitive find), plus the header-only helpers ([`gl_2d.h`](src/ui/core/gl_2d.h),
  [`layout_utils.h`](src/ui/core/layout_utils.h), [`metrics.h`](src/ui/core/metrics.h), [`theme.h`](src/ui/core/theme.h), [`core/hit.h`](src/ui/core/hit.h)). These TUs are
  guarded against picking up REPL / editor knowledge and are linked
  into the standalone `editor_demo` to prove they work without the
  full app. `editor_demo` is the canary here: it should use `src/ui/core`
  only and must not grow a dependency on `src/ui/app`.
- **`app/`** — feature-UI that knows REPL / editor / peer concepts.
  The code-panel adapter ([`repl_code_panel.c`](src/ui/app/repl_code_panel.c)), app geometry
  ([`layout.c`](src/ui/app/layout.c), [`overlay_layout.c`](src/ui/app/overlay_layout.c)), the floating-panel view projection
  ([`variable_panel_view.c`](src/ui/app/variable_panel_view.c)), autocomplete, chrome ([`menu_bar.c`](src/ui/app/menu_bar.c),
  [`scene_tabs.c`](src/ui/app/scene_tabs.c), [`panels.c`](src/ui/app/panels.c)), and the UI runtime state itself
  (`state.{c,h}`, [`state_types.h`](src/ui/app/state_types.h), [`snapshot.h`](src/ui/app/snapshot.h), [`editor.h`](src/ui/app/editor.h)). Peer-specific
  renderers such as color picker, variable panel, and replay HUD live under
  `subsystems/`; support overlays such as the CPU profile panel live under
  `support/`. All of these read frame snapshots and may carry one-feature
  vocabulary.

Dependencies are strictly one-way: `app/` may include from `core/`;
`core/` never includes from `app/`.

## How it is exercised

`src/ui` has no standalone demo of its own, but its generic core is not
untested in isolation: the generic text panel ([`text_panel.c`](src/ui/core/text_panel.c) plus its
wrapping/search helpers) is linked and driven by
[`tools/editor_demo/`](../../tools/editor_demo/), the standalone plain-text
editor — so the reusable view half runs without the REPL, without `src/app`,
and without `src/ui/app`. The `render3d_demo` HUD shows the same fixed-function
2D-overlay drawing style this layer uses. There is no `ui_demo` because UI is a
*view for* other subsystems, not a subsystem with behavior of its own.

## In the REPL app

Inside the full app this is **layer 5** of the ownership map. Each frame the
controller ([`src/app/glr_ctrl.c`](src/app/glr_ctrl.c)) builds a [`UiRenderSnapshot`](src/ui/app/snapshot.h#L70) from
REPL runtime state + [`EditorState`](src/editor/state.h#L175) + [`UiState`](src/ui/app/state.h#L20) + peer state and fans it out to the
`ui_*_render` functions. On input, the controller asks UI to hit-test, gets
a [`UiHit`](src/ui/core/hit.h#L51) back, and dispatches it to the owning subsystem.

[`UiState`](src/ui/app/state.h#L20) ([`app/state.c`](src/ui/app/state.c)) owns only **transient chrome**: viewport, pointer,
status-text TTL, panel visibility, the panel-divider geometry. It explicitly
does *not* own cursor blink (the editor does), program state, or text. The
code panel is assembled by a two-piece split: the generic [`text_panel.c`](src/ui/core/text_panel.c)
renders rows, and the REPL-aware [`repl_code_panel.c`](src/ui/app/repl_code_panel.c) adapter builds those
rows from snapshots (editor buffer, command metadata, tutorial fade, replay
annotations, color transformers) and rewrites generic hits back to
source-line targets.

## File map

| File | Responsibility |
|---|---|
| [`core/text_panel.c`](src/ui/core/text_panel.c) / `.h`, [`core/text_search.c`](src/ui/core/text_search.c) | Generic text-panel renderer + hit-test + search visuals (REPL-free) |
| [`core/text_layout.c`](src/ui/core/text_layout.c) / `.h` | Pure wrapping, row counts, cursor-row mapping ([`CodeLayout`](src/ui/core/text_layout.h#L57)) |
| [`core/layout_utils.h`](src/ui/core/layout_utils.h) | Header-only rectangle helpers shared by layout code |
| [`core/tabbed_overlay.c`](src/ui/core/tabbed_overlay.c) / `.h` | Generic modal tabbed text overlay (the F1 help shell) |
| [`core/gl_2d.h`](src/ui/core/gl_2d.h) | Header-only 2D OpenGL helpers |
| [`core/hit.h`](src/ui/core/hit.h) | [`UiHit`](src/ui/core/hit.h#L51) / [`UiHitKind`](src/ui/core/hit.h#L17) — the passive UI → controller result |
| [`core/metrics.h`](src/ui/core/metrics.h), [`core/theme.h`](src/ui/core/theme.h) | Shared layout metrics + colors |
| [`app/layout.c`](src/ui/app/layout.c) / `.h` | App 3D viewport / code-panel rectangle geometry |
| [`app/overlay_layout.c`](src/ui/app/overlay_layout.c) / `.h` | Floating overlay panel placement |
| [`app/state.c`](src/ui/app/state.c) / `.h`, [`app/state_types.h`](src/ui/app/state_types.h) | Owns [`UiState`](src/ui/app/state.h#L20) (chrome/viewport/pointer/status TTL only) |
| [`app/snapshot.h`](src/ui/app/snapshot.h) | [`UiRenderSnapshot`](src/ui/app/snapshot.h#L70) — the read-only per-frame bundle every renderer takes |
| [`app/panels.c`](src/ui/app/panels.c) / `.h` | Top-level panel bridge: code panel + status banner, prioritizes overlay/menu hits |
| [`app/repl_code_panel.c`](src/ui/app/repl_code_panel.c) / `.h` | REPL-aware adapter: builds rows from snapshots, maps hits to source lines |
| [`app/menu_bar.c`](src/ui/app/menu_bar.c) / `.h` | Menu bar, dropdowns, flyout submenus, search slot |
| [`app/scene_tabs.c`](src/ui/app/scene_tabs.c) / `.h` | Scene tab strip (snapshot-pure render + whole-band hit-test) |
| [`app/variable_panel_view.c`](src/ui/app/variable_panel_view.c) / `.h` | Projects app snapshots into the variable-slider panel view |
| [`app/autocomplete_panel.c`](src/ui/app/autocomplete_panel.c) / `.h` | Completion popup renderer |
| [`app/editor.h`](src/ui/app/editor.h) | `Ui*` editor-overlay snapshot types (swatches, sliders, highlights) |
| [`subsystems/color_picker.c`](src/ui/subsystems/color_picker.c) / `.h` | Feature-UI: color-picker renderer + hit-test over [`ColorPickerView`](../subsystems/color_picker/color_picker_state.h#L47) |
| [`subsystems/variable_panel.c`](src/ui/subsystems/variable_panel.c) / `.h` | Feature-UI: variable-slider panel chrome (the peer owns drag/visibility) |
| [`subsystems/replay_hud.c`](src/ui/subsystems/replay_hud.c) / `.h` | Feature-UI: 2D replay HUD (reads the replay peer snapshot) |
| [`support/cpuprof.c`](src/ui/support/cpuprof.c) / `.h` | CPU profiling overlay renderer and geometry helpers |

**Boundary:** a UI renderer draws; a UI input handler hit-tests and returns a
[`UiHit`](src/ui/core/hit.h#L51). Neither directly mutates REPL / editor / peer state, and `ui_*` does
not include `render3d_*` headers.
