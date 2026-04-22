# REPL Module Guide

One-page lay of the land for the REPL source tree. For per-module
detail read `ARCHITECTURE.md`; for the editor-adjacent ownership
diagram read `REPL_REFACTOR_MAP.md`. This doc is deliberately short —
it should read in under two minutes.

## Responsibility Layers

Every source file belongs to one of six layers. The file-name prefix
is a hint but not a contract — see *Naming Notes* below for the
current gap.

### 1. Command pipeline — source → flatten → execute

The central data flow. Edits mutate `g_cmds[]`; everything downstream
(execution, replay, overlays) reads the flattened array `g_flat_cmds[]`.

| Module | Role |
|--------|------|
| `repl_core` | Parser, normalization, display frame, GL init |
| `repl_command_spec` | Declarative descriptors for fixed-arity GL commands |
| `repl_command_store` | Insert/delete/replace API over `g_cmds[]` |
| `repl_commit` | Float decls, variable assignments, structured block commits |
| `repl_flatten` | Source-to-flat expansion (loops, functions, `if`) |
| `repl_executor` | Flat-program GL dispatch |
| `repl_eval` | Expression evaluator (recursive descent) |
| `cmd_format` | Pure indentation/depth computation (no GL) |

### 2. Editor + input

Accepts keystrokes/clicks, routes each event to the right subsystem,
and performs mutations through the command store.

| Module | Role |
|--------|------|
| `repl_editor` | Keyboard/mouse dispatcher, commit orchestration, `feed_line` |
| `repl_actions` | Config/menu side effects (single mutation lane for UI) |
| `repl_commit` | *(see pipeline)* — invoked from the editor's commit path |
| `repl_keys` | Keybinding constants |
| `repl_camera_controls` | Scene camera drag + momentum |
| `repl_clipboard` | Line selection and copy/cut/paste |
| `repl_undo` | Snapshot rings; example-promotion hook |
| `repl_search` | Search state and navigation |
| `repl_var_drag` | Variable slider drag transaction + writeback |
| `repl_inline_rename` | Scene-rename input buffer (surfaces via status strip) |

### 3. Domain models

Non-pipeline state that UI and input both read. None of these call
OpenGL.

| Module | Role |
|--------|------|
| `repl_scenes` | User-scene slots, workspace directory, LRU eviction |
| `repl_example_loader` | Built-in example loading + active tracking |
| `repl_examples` | Built-in example data |
| `repl_autocomplete` | Completion model: matches, selection, ghost, hints |
| `repl_autonormal` | Auto-generated `glNormal3f` maintenance |
| `repl_replay` | Replay state machine + fade batches |
| `repl_state` | Cross-cutting state catalog (`ReplUiState`, etc.) |

### 4. 2D UI rendering (code panel + overlays)

Pure renderers that read from the models above. Each module ends up
as one visible region on screen.

| Module | Role |
|--------|------|
| `ui_panels` | Code-panel rows, inline ghost/hint, scene status banner |
| `repl_code_panel_layout` | Pure text wrapping (no GL) — shared with export dumps + tests |
| `repl_code_panel_document` | Document row model (no GL) |
| `repl_replay_annotations` | Source-line replay text expansion |
| `repl_menu_bar` | Top-level menus, dropdowns, pinned buttons, search slot |
| `repl_color_picker` | Floating HSV/alpha picker + literal colour swatches |
| `repl_help_overlay` | Modal F1 help (Commands / Keys tabs) |
| `repl_variable_panel` | Floating variable slider panel (render-only) |
| `repl_autocomplete_panel` | Completion popup renderer |
| `profile_panel` | CPU timing HUD |

### 5. 3D scene rendering

The viewport. Shared `FrameRenderContext`, guarded helper passes,
table-driven themes.

| Module | Role |
|--------|------|
| `scene_render` | Camera, frame prep, grid/axes theme specs, overlay walker |
| `scene_backdrop` | Backdrop pass (e.g. cityscape) |
| `scene_lights` | Light setup + indicator drawing |
| `scene_overlays` | Outline overlays and cursor-block detection |

### 6. Persistence, audio, lifecycle

| Module | Role |
|--------|------|
| `repl_export` | Save/load, workspace headers, code-panel dumps |
| `repl_audio` | Playlist engine + persisted audio config |
| `sample` | `main()` + GLUT callback wiring |
| `gl_stub_counts` | `USE_GL_STUBS` symbol tracking |

## Render / Model Split

For UI and scene layers the convention is paired modules:

- **Model** owns state + computation, makes no GL calls.
- **Panel / render** owns drawing, reads model state, performs no
  mutations.

Concrete pairs today:

| Model | Renderer |
|-------|----------|
| `repl_autocomplete` | `repl_autocomplete_panel` |
| `repl_code_panel_layout` + `repl_code_panel_document` | `ui_panels` |
| `repl_var_drag` (writeback) | `repl_variable_panel` |

The split is the boundary most likely to be violated by new code —
keep GL calls out of the model files and keep mutations out of the
render files.

## Naming Notes

File prefixes today:

- `repl_*` — most modules. Originally meant *"part of the REPL
  sample"*, now covers models, input, and some 2D UI.
- `ui_*` — only `ui_panels` uses this prefix today.
- `scene_*` — 3D viewport rendering (`scene_render`, `scene_backdrop`,
  `scene_lights`, `scene_overlays`).

Result: 2D UI renderers extracted from `ui_panels.c` ended up under
`repl_*` (e.g. `repl_color_picker`, `repl_help_overlay`,
`repl_menu_bar`, `repl_variable_panel`, `repl_autocomplete_panel`).
Prefix alone doesn't tell you *renderer* vs *model* vs *input*.

### Known gap and the future rename

A consistent scheme would be:

- `ui_*` for 2D renderers (panels, overlays, popups)
- `scene_*` for 3D rendering (already consistent)
- `repl_*` for models, input, and pipeline

Under that scheme the renames would be:

| Current | Proposed |
|---------|----------|
| `repl_color_picker` | `ui_color_picker` |
| `repl_help_overlay` | `ui_help_overlay` |
| `repl_menu_bar` | `ui_menu_bar` |
| `repl_variable_panel` | `ui_variable_panel` |
| `repl_autocomplete_panel` | `ui_autocomplete_panel` |

Edge cases to decide before executing:

- `repl_code_panel_layout` / `repl_code_panel_document` — pure
  models *for* UI but have no GL calls. Arguably `ui_*` (they're
  UI-scoped) or stay `repl_*` (they're pure models reusable from
  tests/export). Pick one.
- `repl_replay_annotations` — model for UI replay text. Same
  question.
- `repl_inline_rename` / `repl_var_drag` — input handlers, not
  renderers. Stay `repl_*`.
- `profile_panel` — already has no prefix; optional rename to
  `ui_profile_panel` for uniformity.

The rename is cheap (grep + sed + Makefile SRCS update) but noisy in
git history. Worth doing as a single commit after agreeing on the
edge cases above, not as a side effect of another refactor.

## Where to put new code

- Adding a GL command? Pipeline layer — start in `repl_core.c`
  (`parse_command`), then `repl_executor.c` (`execute_commands`),
  and `repl_flatten.c` if it needs expansion.
- Adding a keyboard shortcut? Editor layer — `repl_editor.c` if
  it's a new route, `repl_actions.c` if it piggybacks an existing
  config toggle.
- Adding a visual overlay? 2D UI layer (new `repl_*_panel.c` /
  future `ui_*.c`) or scene layer (extend `scene_render.c`
  overlays) depending on whether it lives in the code panel or the
  viewport.
- Adding example content? `repl_examples.c`.
- Adding persisted state? Add the extern in `repl_state.h` and
  register the pointer in `repl_state.c`'s catalog.
