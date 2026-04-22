# REPL Refactor Map

A working ownership map for the editor-adjacent cleanup slices. For the
one-page overview of all layers read [`MODULES.md`](MODULES.md); for per-module
detail read [`ARCHITECTURE.md`](ARCHITECTURE.md). This doc tracks the modules
split out of the old `repl_editor.c` and the nearby modules they coordinate
with.

Nodes are grouped by responsibility layer. The cluster boxes match the
six layers in `MODULES.md`.

```mermaid
flowchart LR
    sample["sample.c<br/>GLUT callback wiring"]

    subgraph pipeline["Command pipeline"]
        core["repl_core.c<br/>parser · display frame"]
        flatten["repl_flatten.c<br/>source to flat"]
        exec["repl_executor.c<br/>flat program execution"]
        commit["repl_commit.c<br/>decls · assigns · blocks"]
        store["repl_command_store.c<br/>source command mutation"]
    end

    subgraph input["Editor + input"]
        editor["repl_editor.c<br/>input router · feed_line"]
        actions["repl_actions.c<br/>config + menu side effects"]
        camera["repl_camera_controls.c<br/>orbit/pan/zoom + momentum"]
        undo["repl_undo.c<br/>undo/redo rings"]
        clipboard["repl_clipboard.c<br/>selection + copy/cut/paste"]
        vardrag["repl_var_drag.c<br/>variable drag transaction"]
        rename["repl_inline_rename.c<br/>scene-rename buffer"]
    end

    subgraph models["Domain models"]
        scenes["repl_scenes.c<br/>user scenes + workspace"]
        acmodel["repl_autocomplete.c<br/>completion model"]
        replay["repl_replay.c<br/>replay state + fade batches"]
        audio["repl_audio.c<br/>playlist engine"]
    end

    subgraph ui_layer["2D UI rendering"]
        uicp["ui_panels.c<br/>code panel rows · status banner"]
        layout["repl_code_panel_layout.c<br/>pure wrap iterator"]
        docrows["repl_code_panel_document.c<br/>document row model"]
        replay_ann["repl_replay_annotations.c<br/>code-panel replay text"]
        menu["ui_menu_bar.c<br/>menubar + dropdowns"]
        color["ui_color_picker.c<br/>floating color picker"]
        help["ui_help_overlay.c<br/>modal F1 help"]
        varpanel["ui_variable_panel.c<br/>slider panel (render only)"]
        acpanel["ui_autocomplete_panel.c<br/>completion popup"]
    end

    subgraph scene_layer["3D scene rendering"]
        sceneR["scene_render.c<br/>frame prep · theme specs · overlay walker"]
        backdrop["scene_backdrop.c<br/>backdrop pass"]
        lights["scene_lights.c<br/>light setup + indicators"]
        overlays["scene_overlays.c<br/>outline overlays"]
    end

    export["repl_export.c<br/>import/export + visual dump"]

    sample --> editor

    editor --> actions
    editor --> camera
    editor --> undo
    editor --> clipboard
    editor --> commit
    editor --> uicp
    editor --> replay
    editor --> varpanel
    editor --> rename
    editor --> vardrag

    actions --> audio
    actions --> replay
    actions --> scenes
    actions --> core
    actions --> rename
    actions --> uicp

    clipboard --> undo
    clipboard --> store
    commit --> undo
    commit --> store
    undo --> scenes
    store --> core

    core --> flatten
    core --> exec
    core --> sceneR
    core --> help
    core --> varpanel
    core --> acpanel

    acpanel --> acmodel
    rename --> scenes
    varpanel --> vardrag
    replay --> exec

    sceneR --> exec
    sceneR --> replay
    sceneR --> backdrop
    sceneR --> lights
    sceneR --> overlays

    uicp --> actions
    uicp --> scenes
    uicp --> docrows
    uicp --> menu
    uicp --> color
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
- `ui_menu_bar.c` owns top-level menu/dropdown state, menu hit-testing,
  right-click config cycling, and the inline search slot in the menu bar.
- `ui_color_picker.c` owns the floating HSV/alpha picker state and literal
  color swatch rendering/mutation for color commands.
- `ui_help_overlay.c` owns the modal F1 help overlay.
- `ui_variable_panel.c` owns the floating variable slider panel rendering,
  geometry, and hit-test; value mutation happens in `repl_var_drag.c`.
- `ui_autocomplete_panel.c` owns the floating completion popup renderer;
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
  Focus-grid vertex selection is prepared in `FrameRenderContext` before grid
  drawing, keeping the theme draw path read-only on focus state. Ocean-grid
  camera height/waterline classification is also derived once in frame prep
  instead of recomputed inside the grid theme. Backdrop rendering now delegates
  to `scene_backdrop.c`; per-pass light setup and light indicators delegate to
  `scene_lights.c`. Polygon outline/current-block highlighting delegates to
  `scene_overlays.c`, which also exposes the flat-block cursor matcher used by
  vertex-number and normal-vector overlays.

## Open Edges

- `ui_panels.c` still owns render-time iteration over code-panel rows, search
  match highlights inside source lines, and inline ghost/hint text rendering
  next to the input line.
- `repl_editor.c` still owns hidden-code-panel restore rules and the main
  keyboard/special/mouse route ordering.
- `sample.h` and `repl_state.h` still expose broad globals for compatibility.
  The current module splits are ownership boundaries, not yet context-object
  rewrites.
