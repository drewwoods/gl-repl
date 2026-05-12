# MODULES.md Update: Editor Uses UI As Its View

This file contains the intended `MODULES.md` update for the corrected editor/UI/controller boundary.

Directly overwriting `MODULES.md` through the GitHub contents API is currently blocked because the connector does not expose the current blob SHA for the large file. This document preserves the replacement language and Mermaid diagram so it can be applied to `MODULES.md` without ambiguity.

## Replacement Four-Role Ownership Contract

```text
Editor = text model + controller.
    Owns text-document behavior: editable source text, read-only text documents,
    cursor, selection, scroll, search, autocomplete state, clipboard, undo/redo,
    cursor blink, keyboard/mouse handling for text documents, and source commit
    orchestration.

    The editor uses UI as its view. It exposes editor snapshots for UI to render,
    and it consumes neutral UI hit-test results to interpret mouse/pointer input.
    UI does not decide text behavior.

UI = view + hit-test/render services.
    Provides the view for the editor and other subsystems. Draws glyphs,
    highlights, gutters, panels, widgets. Measures text. Hit-tests visual
    rows/chars/buttons/swatches/sliders. Returns neutral UiHit results.
    Does not own text behavior or editor state.

REPL = validator/compiler for committed source.
    Given proposed source text + context, returns ReplCompiledChange or diagnostic.
    Does not own editor state. Does not own UI state. Does not write editor text.
    Does not call set_status.

imrepl_ctrl = thin router + frame/snapshot coordinator.
    Receives raw GLUT events, determines focus/region, asks UI for hit-test
    results, routes events to the owning subsystem, builds snapshots, and relays
    diagnostics/status messages. It does not implement editor behavior and does
    not drive the editor UI.
```

The key distinction:

```text
The editor drives text-document UI behavior.
The editor uses UI as its view.
imrepl_ctrl routes the event.
UI draws, measures, and hit-tests.
```

## New Section: Editor Uses UI As Its View

The editor is the model/controller for text documents. UI is the view layer the editor uses to present that model and to map pixels back to document positions.

```text
EditorState
  -> editor_build_view_snapshot(...)
  -> UI renders text, highlights, cursor, gutters, overlays, popup geometry

Mouse/pointer input
  -> UI hit-tests visual geometry and returns UiHit
  -> imrepl_ctrl routes the event + UiHit to the editor
  -> editor interprets the hit in terms of text-document behavior
```

UI can know about rectangles, rows, columns, glyph bounds, swatches, and panel regions. UI should not know that Ctrl+G means search, that Tab accepts an autocomplete candidate, that scrolling should follow the cursor, or that a source-line edit requires a REPL commit. Those are editor decisions.

The editor should not render glyphs or duplicate hit-test math. It asks UI for view services. But UI should not own editor state or editor policy.

```text
Editor model/controller:  what the text document is and how editing behaves
UI view:                  how that document appears and where the user clicked
imrepl_ctrl router:        which subsystem receives this event
REPL compiler:             whether committed source text is valid
```

## Updated MODULES Mermaid Diagram

```mermaid
flowchart LR
    subgraph legend["Edge meaning"]
        lmut_a["controller mutates"] e1@==> lmut_b["owned state / writer"]
        lread_a["reads / renders"] -.-> lread_b["query / model / helper"]
        lflow_a["routes / invokes"] i1@--> lflow_b["callback / dispatch / pass"]
    end

    sample["sample.c<br/>GLUT callback wiring · buffer swap"]

    subgraph app["0. App router"]
        ctrl["imrepl_ctrl.c<br/>raw input router · frame/snapshot coordinator<br/>does NOT drive editor behavior"]
    end

    subgraph repl_pipeline["1. REPL compiler/program pipeline"]
        compile["repl_compile.c<br/>pure validation → ReplCompiledChange"]
        parser["repl_parser.c<br/>line parser"]
        scope["repl_source_scope.c<br/>depth · indent · context"]
        exec["repl_executor.c<br/>flat command execution"]
        store["repl_command_store.c<br/>GLCmd array only"]
        state["repl_state.c<br/>ReplState"]
    end

    subgraph editor["2. Editor (text model + controller)"]
        einput["editor_input.c<br/>text-doc controller<br/>cursor · scroll · select · search"]
        ecommit["editor_commit.c<br/>commit transaction<br/>compile + undo + buffer + apply"]
        eview["editor_view_snapshot.c<br/>builds editor view snapshot"]
        ebuf["editor_buffer.c<br/>line text · only writer"]
        edoc["editor_document.c<br/>input · cursor · edit line"]
        eundo["editor_undo.c<br/>transaction snapshots"]
        eclip["editor_clipboard.c<br/>selection · cut/copy/paste"]
        esearch["editor_search.c<br/>query · hit tracking"]
        eac["editor_autocomplete.c<br/>matches · ghost · hint<br/>asks CompletionProvider"]
        ehelpsess["editor_help_session.c<br/>read-only editor session"]
    end

    subgraph peers["2b. Peer subsystems"]
        vpanel["variable_panel_drag.c<br/>visibility + drag transaction"]
        replay_sys["replay.c<br/>state machine · fades"]
    end

    subgraph ui_layer["5. UI view + hit-test services"]
        uistate["ui_state.c<br/>UiState (chrome only)"]
        uihit["ui_hit.h<br/>UiHit · UiHitKind"]
        uipanels["ui_panels.c<br/>code panel · statusbar<br/>renders snapshots · returns UiHit"]
        uimenu["ui_menu_bar.c<br/>menubar + dropdowns<br/>returns UiHit"]
        uicolor["ui_color_picker.c<br/>color picker"]
        uiac["ui_autocomplete_panel.c<br/>completion popup view"]
    end

    subgraph scene_layer["4. 3D scene rendering"]
        sceneR["scene_render.c<br/>3D frame"]
        scam["scene_camera_controls.c<br/>camera transform"]
    end

    sample i2@--> ctrl

    %% UI is the editor's view: it renders snapshots and provides hit-test results.
    eview -.-> uipanels
    eview -.-> uiac
    eview -.-> uicolor
    uipanels i3@--> ctrl
    uimenu i4@--> ctrl
    uicolor i5@--> ctrl

    %% imrepl_ctrl routes to owners; it does not implement owner behavior.
    ctrl i6@--> einput
    ctrl i7@--> ehelpsess
    ctrl i8@--> vpanel
    ctrl i9@--> replay_sys
    ctrl i10@--> scam

    %% Editor controllers mutate editor-owned state directly.
    einput e1@==> edoc
    einput e2@==> ebuf
    einput e3@==> esearch
    einput e4@==> eac
    einput e5@==> eclip
    einput e6@==> eview

    %% Editor commit transaction is the only path crossing into REPL.
    einput i11@--> ecommit
    ecommit i12@--> compile
    ecommit e7@==> eundo
    ecommit e8@==> ebuf
    ecommit e9@==> store

    %% Clipboard cut/paste and color edits that rewrite source go through commit.
    eclip i13@--> ecommit
    uicolor i14@--> ecommit

    %% REPL compile is pure.
    compile -.-> parser
    compile -.-> scope
    store e10@==> state

    %% Frame render / snapshot fan-out.
    ctrl i15@--> eview
    ctrl i16@--> sceneR
    ctrl i17@--> uipanels
    ctrl i18@--> uimenu
    ctrl i19@--> uiac

    %% Snapshot reads.
    ctrl -.-> state
    ctrl -.-> uistate
    ctrl -.-> vpanel
    ctrl -.-> replay_sys

    %% Scene routes.
    sceneR i20@--> exec
    sceneR -.-> scam
    sceneR -.-> replay_sys

    %% UI render is read-only.
    uipanels -.-> uistate
    uiac -.-> eac

    classDef animateE stroke:#f50,stroke-dasharray: 9\,5,stroke-dashoffset: 900,animation: dash 90s linear infinite;
    classDef animateF stroke:#5f0,stroke-dasharray: 9\,5,stroke-dashoffset: 900,animation: dash 90s linear infinite;

    class e1,e2,e3,e4,e5,e6,e7,e8,e9,e10 animateE
    class i2,i3,i4,i5,i6,i7,i8,i9,i10,i11,i12,i13,i14,i15,i16,i17,i18,i19,i20 animateF
```

Reading the diagram:

- UI is the editor's view. It renders editor snapshots and returns passive `UiHit` results. It does not decide editor behavior.
- `imrepl_ctrl` routes raw input to owners and builds frame snapshots. It does not mirror the editor API.
- The editor is the text model/controller. It mutates `EditorState` directly for cursor, scroll, selection, search, autocomplete, clipboard, and undo.
- `editor_commit` is the only editor path that crosses into REPL via `repl_compile`. On success it updates undo + buffer + command store as one transaction. On failure nothing mutates.
- `repl_compile` is pure. `repl_command_store` mutates command arrays only; `editor_buffer` mutates line text only.
- Variable panel and replay are peer subsystems. They may provide overlays or UI panels; they are not editor-owned.

## Boundary Rule Updates

```text
check-ui-returns-hits-only
    ui_*.c input helpers do not call repl_action_*, repl_command_store_*,
    editor_*_mut*, repl_state_*_mut*, or peer-subsystem mutators directly.
    They compute a UiHit and return it.

check-imrepl-not-editor-mirror
    imrepl_ctrl must not accumulate one wrapper per editor operation.
    New editor behavior belongs behind editor_handle_* or editor_* APIs.
```
