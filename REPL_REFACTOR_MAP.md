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

    ui["ui_panels.c<br/>code/menu/help rendering<br/>hit testing<br/>rename UI"]
    layout["repl_code_panel_layout.c<br/>pure wrap iterator<br/>row/segment lookup<br/>cursor row mapping"]
    export["repl_export.c<br/>import/export<br/>visual code dump"]
    audio["repl_audio.c<br/>playlist engine<br/>persisted audio cfg"]
    replay["repl_replay.c<br/>replay state machine<br/>fade batches"]
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
    replay --> exec
    ui --> actions
    ui --> scenes
    ui --> layout
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

## Open Edges

- `ui_panels.c` still owns menu layout, render-time code panel row iteration,
  hit testing, and inline rename UI. Code-panel wrapping is shared, but the
  broader document row model is still embedded in UI code.
- `repl_editor.c` still owns variable slider dragging, hidden-code-panel restore
  rules, and the main keyboard/special/mouse route ordering.
- `sample.h` and `repl_state.h` still expose broad globals for compatibility.
  The current module splits are ownership boundaries, not yet context-object
  rewrites.
