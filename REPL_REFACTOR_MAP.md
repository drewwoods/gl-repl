# REPL Refactor Map

This is a working ownership map for the editor-adjacent cleanup slices. It is
not the full architecture; it tracks the modules split out of the old
`repl_editor.c` and the nearby modules they coordinate with.

```mermaid
flowchart LR
    sample["sample.c<br/>GLUT callback wiring"]
    editor["repl_editor.c<br/>input router<br/>line navigation<br/>feed_line"]

    actions["repl_actions.c<br/>config table<br/>shortcut dispatch<br/>menu actions"]
    camera["repl_camera_controls.c<br/>viewport drag state<br/>orbit/pan/zoom<br/>momentum tick"]
    undo["repl_undo.c<br/>undo/redo rings<br/>mutation snapshots"]
    clipboard["repl_clipboard.c<br/>selection anchors<br/>copy/cut/paste"]
    commit["repl_commit.c<br/>declarations<br/>assignments<br/>block commits"]
    store["repl_command_store.c<br/>source command mutation"]

    ui["ui_panels.c<br/>code panel renderer<br/>hit routing + status banner"]
    layout["repl_code_panel_layout.c<br/>pure wrap iterator<br/>row/segment lookup<br/>cursor row mapping"]
    docrows["repl_code_panel_document.c<br/>document row model<br/>scroll follow<br/>hit-test targets"]
    menu["repl_menu_bar.c<br/>menubar + dropdowns<br/>search slot rendering"]
    color["repl_color_picker.c<br/>floating color picker<br/>literal color swatches"]
    help["repl_help_overlay.c<br/>modal F1 help<br/>Commands/Keys tabs"]
    varpanel["repl_variable_panel.c<br/>floating slider panel<br/>rect + hit + render"]
    acmodel["repl_autocomplete.c<br/>completion model<br/>matches + hint state"]
    acpanel["repl_autocomplete_panel.c<br/>completion popup renderer"]
    rename["repl_inline_rename.c<br/>scene-rename input buffer"]
    vardrag["repl_var_drag.c<br/>variable drag transaction<br/>linear/log writeback"]
    replay_ann["repl_replay_annotations.c<br/>code-panel replay notes<br/>expanded/evaluated args"]
    export["repl_export.c<br/>import/export<br/>visual code dump"]
    audio["repl_audio.c<br/>playlist engine<br/>persisted audio cfg"]
    replay["repl_replay.c<br/>replay state machine<br/>fade batches"]
    scene["scene_render.c<br/>3D scene frame<br/>theme specs<br/>guarded helper passes"]
    scene_theme["GridThemeSpec / AxesThemeSpec<br/>standard theme tables"]
    scene_overlay["vertex overlay walker<br/>flat traversal<br/>numbers + normals"]
    scenes["repl_scenes.c<br/>user scenes<br/>workspace slots"]
    core["repl_core.c<br/>parser<br/>display frame<br/>shared helpers"]
    flatten["repl_flatten.c<br/>source to flat program"]
    exec["repl_executor.c<br/>flat program execution"]

    sample --> editor
    editor --> actions
    editor --> camera
    editor --> undo
    editor --> clipboard
    editor --> commit
    editor --> ui
    editor --> replay

    actions --> ui
    actions --> audio
    actions --> replay
    actions --> scenes
    actions --> core

    clipboard --> undo
    clipboard --> store
    commit --> undo
    commit --> store
    undo --> scenes
    store --> core

    core --> flatten
    core --> exec
    core --> help
    core --> varpanel
    core --> acpanel
    editor --> varpanel
    editor --> rename
    editor --> vardrag
    actions --> rename
    acpanel --> acmodel
    rename --> scenes
    varpanel --> vardrag
    replay --> exec
    core --> scene
    scene --> exec
    scene --> replay
    scene --> scene_theme
    scene --> scene_overlay
    ui --> actions
    ui --> scenes
    ui --> docrows
    ui --> menu
    ui --> color
    docrows --> layout
    docrows --> replay_ann
    replay_ann --> replay
    menu --> actions
    menu --> scenes
    color --> core
    export --> layout
```

## Current Boundaries

- `repl_editor.c` should keep shrinking toward ordered input routing and small
  editor-local state. It should delegate mutations once it knows which route
  owns an input event.
- `repl_actions.c` owns side effects that begin from config rows, F-key/Ctrl-key
  config shortcuts, and top-level menu items. UI code should ask it to execute
  actions instead of mutating scenes/files/config directly.
- `repl_camera_controls.c` owns only viewport camera state after UI routing has
  decided a mouse event belongs to the scene.
- `repl_undo.c` owns snapshots and the example-promotion hook that must run
  before mutating editor state.
- `repl_clipboard.c` owns selection and clipboard buffers. It mutates commands
  through `ReplCommandStore` and takes snapshots through `repl_undo.c`.
- `repl_code_panel_layout.c` owns pure text wrapping for code-panel rows:
  continuation indent, wrap break choice, row counts, segment lookup, and cursor
  row mapping. `ui_panels.c`, export visual dumps, and tests should consume this
  instead of carrying local copies.
- `repl_code_panel_document.c` owns the higher-level code-panel row model:
  header/body/footer row counts, replay-extra rows, cursor-follow scrolling,
  and document-line to command-line hit targets.
- `repl_replay_annotations.c` owns code-panel replay text expansion: source to
  flat-command mapping, variable substitution comments, and evaluated command
  display text.
- `repl_menu_bar.c` owns top-level menu/dropdown state, menu hit-testing,
  right-click config cycling, and the inline search slot in the menu bar.
- `repl_color_picker.c` owns the floating HSV/alpha picker state and literal
  color swatch rendering/mutation for color commands.
- `repl_help_overlay.c` owns the modal F1 help overlay.
- `repl_variable_panel.c` owns the floating variable slider panel rendering,
  geometry, and hit-test; value mutation happens in `repl_var_drag.c`.
- `repl_autocomplete_panel.c` owns the floating completion popup renderer;
  match/selection/hint state lives in `repl_autocomplete.c`.
- `repl_inline_rename.c` owns the inline scene-rename input buffer and key
  handling (surfaced through `set_status()`, no dedicated render pass).
- `repl_var_drag.c` owns the variable slider drag transaction: start/motion/
  reset, the linear-vs-log value mapping, and the writeback into
  `g_predef_vars` plus matching `CMD_VAR_ASSIGN` sources.
- `scene_render.c` now keeps helper-pass GL state local with small
  `glPushAttrib`/`glPopAttrib` wrappers. Standard grid/axes themes are table
  entries, while focus/ocean/adaptive-plane grid themes remain custom render
  paths. Vertex-number and normal-vector overlays share one flat-command
  visitor so transform replay and tessellation block tracking stay consistent.

## Open Edges

- `ui_panels.c` still owns render-time iteration over code-panel rows, search
  match highlights inside source lines, and inline ghost/hint text rendering
  next to the input line.
- `repl_editor.c` still owns hidden-code-panel restore rules and the main
  keyboard/special/mouse route ordering.
- `sample.h` and `repl_state.h` still expose broad globals for compatibility.
  The current module splits are ownership boundaries, not yet context-object
  rewrites.
