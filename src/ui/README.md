# `src/ui` - the 2D view + hit-test layer (Draft)

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
  ([`UiRenderSnapshot`](app/snapshot.h#L85)) and produces pixels. It does not read live program or
  editor state, and it does not mutate anything.
- **A UI input handler may hit-test and return.** It computes a *neutral*
  [`UiHit`](core/hit.h#L60) (e.g. "the pointer is over code row 12, char 4" or "over the
  Replay button") and hands it back. It does **not** decide what that means
  or call into the editor/REPL.

The owning subsystem (the editor, or a `src/subsystems/` peer) then interprets
the [`UiHit`](core/hit.h#L60). This is the classic lesson that a **view should not be a
controller**: keeping rendering and hit-testing free of policy is what lets
the same panel serve code editing, the help overlay, and the demos without
each one leaking into the view code.

This split is reflected on the filesystem with two subdirectories:

- **`core/`** - REPL-/editor-/peer-agnostic primitives. [`text_panel.c`](core/text_panel.c)
  (reusable text panel), [`tabbed_overlay.c`](core/tabbed_overlay.c) (modal paged reference
  card), [`text_layout.c`](core/text_layout.c) (pure wrapping), [`text_search.c`](core/text_search.c)
  (case-insensitive find), plus the header-only helpers ([`gl_2d.h`](core/gl_2d.h),
  [`layout_utils.h`](core/layout_utils.h), [`metrics.h`](core/metrics.h), [`theme.h`](core/theme.h), [`core/hit.h`](core/hit.h)). These TUs are
  guarded against picking up REPL / editor knowledge and are linked
  into the standalone `editor_demo` to prove they work without the
  full app. `editor_demo` is the canary here: it should use `src/ui/core`
  only and must not grow a dependency on `src/ui/app`.
- **`app/`** - feature-UI that knows REPL / editor / peer concepts.
  The code-panel adapter ([`repl_code_panel.c`](app/repl_code_panel.c)), app geometry
  ([`layout.c`](app/layout.c), [`overlay_layout.c`](app/overlay_layout.c)), the floating-panel view projection
  ([`variable_panel_view.c`](app/variable_panel_view.c)), autocomplete, chrome ([`menu_bar.c`](app/menu_bar.c),
  [`scene_tabs.c`](app/scene_tabs.c), [`panels.c`](app/panels.c)), and the UI runtime state itself
  (`state.{c,h}`, [`state_types.h`](app/state_types.h), [`snapshot.h`](app/snapshot.h), [`editor.h`](app/editor.h)). Peer-specific
  renderers such as color picker, variable panel, and replay HUD live under
  `subsystems/`; support overlays such as the CPU profile panel live under
  `support/`. All of these read frame snapshots and may carry one-feature
  vocabulary.

Dependencies are strictly one-way: `app/` may include from `core/`;
`core/` never includes from `app/`.

## How it is exercised

`src/ui` has no standalone demo of its own, but its generic core is not
untested in isolation: the generic text panel ([`text_panel.c`](core/text_panel.c) plus its
wrapping/search helpers) is linked and driven by
[`tools/editor_demo/`](../../tools/editor_demo/), the standalone plain-text
editor - so the reusable view half runs without the REPL, without `src/app`,
and without `src/ui/app`. The `render3d_demo` HUD shows the same fixed-function
2D-overlay drawing style this layer uses. There is no `ui_demo` because UI is a
*view for* other subsystems, not a subsystem with behavior of its own.

## In the REPL app

Inside the full app this is **layer 5** of the ownership map. Each frame the
controller ([`src/app/glr_ctrl.c`](../app/glr_ctrl.c)) builds a [`UiRenderSnapshot`](app/snapshot.h#L85) from
REPL runtime state + [`EditorState`](../editor/state.h#L199) + [`UiState`](app/state.h#L20) + peer state and fans it out to the
`ui_*_render` functions. On input, the controller asks UI to hit-test, gets
a [`UiHit`](core/hit.h#L60) back, and dispatches it to the owning subsystem.

[`UiState`](app/state.h#L20) ([`app/state.c`](app/state.c)) owns only **transient chrome**: viewport, pointer,
status-text TTL, panel visibility, the panel-divider geometry. It explicitly
does *not* own cursor blink (the editor does), program state, or text. The
code panel is assembled by a two-piece split: the generic [`text_panel.c`](core/text_panel.c)
renders rows, and the REPL-aware [`repl_code_panel.c`](app/repl_code_panel.c) adapter builds those
rows from snapshots (editor buffer, command metadata, tutorial fade, replay
annotations, color transformers) and rewrites generic hits back to
source-line targets.

## File map

| File | Responsibility |
|---|---|
| [`core/text_panel.c`](core/text_panel.c) / `.h`, [`core/text_search.c`](core/text_search.c) | Generic text-panel renderer + hit-test + search visuals (REPL-free) |
| [`core/text_layout.c`](core/text_layout.c) / `.h` | Pure wrapping, row counts, cursor-row mapping ([`CodeLayout`](core/text_layout.h#L57)) |
| [`core/layout_utils.h`](core/layout_utils.h) | Header-only rectangle helpers shared by layout code |
| [`core/tabbed_overlay.c`](core/tabbed_overlay.c) / `.h` | Generic modal tabbed text overlay (the F1 help shell) |
| [`core/gl_2d.h`](core/gl_2d.h) | Header-only 2D OpenGL helpers |
| [`core/hit.h`](core/hit.h) | [`UiHit`](core/hit.h#L60) / [`UiHitKind`](core/hit.h#L17) - the passive UI → controller result |
| [`core/metrics.h`](core/metrics.h), [`core/theme.h`](core/theme.h) | Shared layout metrics + colors |
| [`app/layout.c`](app/layout.c) / `.h` | App 3D viewport / code-panel rectangle geometry |
| [`app/overlay_layout.c`](app/overlay_layout.c) / `.h` | Floating overlay panel placement |
| [`app/state.c`](app/state.c) / `.h`, [`app/state_types.h`](app/state_types.h) | Owns [`UiState`](app/state.h#L20) (chrome/viewport/pointer/status TTL only) |
| [`app/snapshot.h`](app/snapshot.h) | [`UiRenderSnapshot`](app/snapshot.h#L85) - the read-only per-frame bundle every renderer takes |
| [`app/panels.c`](app/panels.c) / `.h` | Top-level panel bridge: code panel + status banner, prioritizes overlay/menu hits |
| [`app/repl_code_panel.c`](app/repl_code_panel.c) / `.h` | REPL-aware adapter: builds rows from snapshots, maps hits to source lines |
| [`app/menu_bar.c`](app/menu_bar.c) / `.h` | Menu bar, dropdowns, flyout submenus, search slot |
| [`app/scene_tabs.c`](app/scene_tabs.c) / `.h` | Scene tab strip (snapshot-pure render + whole-band hit-test) |
| [`app/variable_panel_view.c`](app/variable_panel_view.c) / `.h` | Projects app snapshots into the variable-slider panel view |
| [`app/autocomplete_panel.c`](app/autocomplete_panel.c) / `.h` | Completion popup renderer |
| [`app/command_description_panel.c`](app/command_description_panel.c) / `.h` | Word-wrapped GL command description popup renderer over a controller-built view |
| [`app/editor.h`](app/editor.h) | `Ui*` editor-overlay snapshot types (swatches, sliders, highlights) |
| [`subsystems/color_picker.c`](subsystems/color_picker.c) / `.h` | Feature-UI: color-picker renderer + hit-test over [`ColorPickerView`](../subsystems/color_picker/color_picker_state.h#L47) |
| [`subsystems/variable_panel.c`](subsystems/variable_panel.c) / `.h` | Feature-UI: variable-slider panel chrome (the peer owns drag/visibility) |
| [`subsystems/replay_hud.c`](subsystems/replay_hud.c) / `.h` | Feature-UI: 2D replay HUD (reads the replay peer snapshot) |
| [`support/cpuprof.c`](support/cpuprof.c) / `.h` | CPU profiling overlay renderer and geometry helpers |

**Boundary:** a UI renderer draws; a UI input handler hit-tests and returns a
[`UiHit`](core/hit.h#L60). Neither directly mutates REPL / editor / peer state, and `ui_*` does
not include `render3d_*` headers.
