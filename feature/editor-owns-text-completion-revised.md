# Plan: Editor-Owned Text Completion — Revised

> **Status: landed.** This revised sibling's direction shipped via
> Phases A–I of
> [`editor-owns-text-completion.md`](editor-owns-text-completion.md).
> The corrected controller boundary (no `UiAction` dispatch enum;
> passive `UiHit` results routed by `imrepl_ctrl`) is in place;
> Phase G's `EditorCompletionProvider` registry decouples editor
> dispatch from REPL grammar.

This is a revised sibling of `feature/editor-owns-text-completion.md`.

The original file remains useful for migration history, especially the
Phase 0/1 work already landed on `feature/editor-ownership-gap-cleanup`.
This revision folds in the corrected controller boundary from
`feature/editor-text-model-controller.md` and removes the older implication
that Phase 4 should introduce a broad `UiAction` dispatch layer.

## Load-Bearing Contract

```text
Editor = text model + controller.
    Owns text-document behavior: editable source text, read-only text
    documents, cursor, selection, scroll, search, autocomplete state,
    clipboard, undo/redo, cursor blink, keyboard/mouse handling for
    text documents, and source commit orchestration.

UI = view + hit-test/render services.
    Draws glyphs, highlights, gutters, panels, widgets. Measures text.
    Hit-tests visual rows/chars/buttons/swatches/sliders. Returns neutral
    UiHit results. Does not own text behavior or editor state.

REPL = validator/compiler for committed source.
    Given proposed source text + context, returns ReplCompiledChange or
    a diagnostic. Does not own editor state. Does not own UI state. Does
    not write editor text. Does not call set_status.

imrepl_ctrl = thin router + frame/snapshot coordinator.
    Receives raw GLUT events, determines focus/region, asks UI for hit-test
    results, routes events to the owning subsystem, builds snapshots, and
    relays diagnostics/status messages. It does not implement editor behavior.
```

The key distinction:

```text
imrepl_ctrl routes the event.
The editor drives the editor UI behavior.
UI draws and hit-tests.
```

Only committed source/program changes go through REPL validation. Editor-local
operations do not.

---

## Why The Original Direction Needed Correction

The data-shape part of editor-owned text was correct:

```text
GLCmd.source[] removed
parser returns canonical text alongside parsed command
editor buffer holds committed line text
snapshots feed renderers
```

The missing part was controller ownership. The older Phase 4 direction drifted
toward:

```text
UI input handler -> large UiAction enum -> imrepl_ctrl mutates everything
```

That would make UI look like the editor controller and would encourage
`imrepl_ctrl` to grow one wrapper per editor operation. The corrected model is:

```text
UI hit-tests and renders.
Editor handles text-document behavior.
imrepl_ctrl routes app-level input to the owning subsystem.
REPL validates only committed source mutations.
```

A click or key event over the code panel is not primarily a UI action. It is
text-document input. UI can report where the event landed; the editor decides
what that means.

---

## Who Drives The Editor UI?

The editor drives text-document UI behavior.

`imrepl_ctrl` does **not** implement cursor movement, scrolling, search,
selection, autocomplete, clipboard, or undo. It only routes raw input to the
editor once focus/hit-testing says the event belongs to a text document.

UI does not implement text behavior either. UI renders an editor snapshot and
returns neutral `UiHit` results.

The editor receives editor events plus hit-test context and mutates
`EditorState` internally. It exposes a compact render snapshot for UI and a
small commit boundary for REPL validation.

```text
UI              sees pixels and rectangles
imrepl_ctrl     sees focus, routes, and subsystem boundaries
editor          sees text-document intent and owns editor state transitions
REPL            sees proposed committed source text
```

---

## imrepl_ctrl Non-Goals

`imrepl_ctrl` is the application router and frame coordinator. It should not
mirror the editor API.

Avoid this shape:

```c
imrepl_ctrl_editor_search_next();
imrepl_ctrl_editor_scroll(int delta);
imrepl_ctrl_editor_move_cursor(int line, int col);
imrepl_ctrl_editor_accept_autocomplete();
imrepl_ctrl_editor_cut();
imrepl_ctrl_editor_paste();
```

That shape makes editor state bubble upward into `imrepl_ctrl` and recreates a
god-controller.

Prefer this shape:

```c
editor_handle_key(EditorState *editor,
                  const EditorKeyEvent *ev,
                  const EditorServices *services);

editor_handle_mouse(EditorState *editor,
                    const EditorMouseEvent *ev,
                    const UiHit *hit,
                    const EditorServices *services);

editor_handle_scroll(EditorState *editor,
                     int delta,
                     const UiHit *hit);
```

Then `imrepl_ctrl` only routes:

```c
switch (hit.kind) {
case UI_HIT_CODE_TEXT:
case UI_HIT_CODE_GUTTER:
case UI_HIT_HELP_PANEL:
    editor_handle_mouse(editor_state_mut(), &ev, &hit, &editor_services);
    break;

case UI_HIT_VARIABLE_SLIDER:
    variable_panel_handle_mouse(variable_panel_state_mut(), &ev, &hit);
    break;

case UI_HIT_REPLAY_BUTTON:
    replay_handle_mouse(replay_state_mut(), &ev, &hit);
    break;
}
```

The controller routes to owners. Owners implement behavior.

A useful rule:

```text
New editor behavior should add an editor API, not an imrepl_ctrl wrapper.
```

---

## Avoid Bubbling Editor State Upward

`imrepl_ctrl` should not need individual editor fields:

```text
cursor_pos
selection_anchor
scroll
search query
current search match
autocomplete selected item
clipboard count
pending newline
edit line
insert mode
```

Those are editor internals. They should be visible outside the editor only
through coarse interfaces:

```c
void editor_handle_key(...);
void editor_handle_mouse(...);
void editor_handle_scroll(...);
void editor_build_view_snapshot(const EditorState *editor,
                                EditorViewSnapshot *out);
```

If `imrepl_ctrl` starts accumulating per-field editor helpers, the split is
drifting.

---

## Target State Shape

```c
/* The user's program: what the REPL parses, flattens, executes,
 * replays, exports, and persists. */
typedef struct {
    ReplDocumentState           document;
    ReplFlatProgramState        flat_program;
    ReplVariableState           variables;
    ReplReplayRuntimeState      replay;        /* transitional until replay peer split */
    ReplSceneRuntimeState       scenes;
    ReplImportExportState       import_export;
    ReplRenderState             render;
    ReplPresentationState       presentation;
} ReplState;

/* Text-document model/controller state. */
typedef struct {
    ReplEditorInputState        input;
    ReplEditorBuffer            buffer;
    ReplSelectionState          selection;
    ReplClipboardState          clipboard;
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    EditorScrollState           scroll;
    EditorCursorBlinkState      cursor_blink;
    EditorUndoRing              undo;

    /* Transitional: moves to variable-panel peer subsystem. */
    ReplVariableDragState       variable_drag;
} EditorState;

/* Transient UI/session chrome. Not program state, not editor behavior. */
typedef struct {
    ReplViewportState           viewport;
    ReplPointerState            pointer;
    ReplStatusState             status;
    ReplHelpState               help;          /* visibility only once help is read-only editor session */
    ReplVariablePanelState      variable_panel;/* visibility only until variable-panel peer split */
    ReplProfilePanelState       profile_panel;
    PanelDividerState           divider;
    ReplCameraState             camera;        /* viewport pose; not editor state */
} UiState;
```

Storage lives in owner modules:

```text
repl_state.c     owns ReplState storage
editor_state.c   owns EditorState storage
ui_state.c       owns UiState storage
```

`imrepl_ctrl` obtains views/mutable handles and wires subsystems together. It
does not own all state bytes.

---

## Read-Only Document Seam

The editor should support text-document sessions:

```text
source document      editable; has REPL-backed commit path
help document        read-only; searchable and scrollable
output/log document  read-only; searchable and scrollable
annotation document  read-only or overlay-backed
```

The help overlay becomes a read-only editor session backed by a content
provider. Help visibility can remain `UiState` chrome, but help scroll/search
belongs to the editor session.

Only source documents have a REPL compile/apply path.

---

## UI Hit-Test Result, Not UI Ownership

UI may expose neutral hit results:

```c
typedef enum {
    UI_HIT_NONE,
    UI_HIT_CODE_TEXT,
    UI_HIT_CODE_GUTTER,
    UI_HIT_COLOR_SWATCH,
    UI_HIT_MENU_ITEM,
    UI_HIT_VARIABLE_SLIDER,
    UI_HIT_REPLAY_BUTTON,
    UI_HIT_HELP_PANEL,
} UiHitKind;

typedef struct {
    UiHitKind kind;
    int line_idx;
    int visual_row;
    int char_idx;
    int cmd_idx;
    int item_idx;
    float local_x;
    float local_y;
} UiHit;
```

But the owner interprets the hit:

```text
UI_HIT_CODE_TEXT         -> editor handles it
UI_HIT_CODE_GUTTER       -> editor handles it
UI_HIT_COLOR_SWATCH      -> editor commit path if it rewrites source text
UI_HIT_VARIABLE_SLIDER   -> variable-panel subsystem handles it
UI_HIT_REPLAY_BUTTON     -> replay subsystem handles it
UI_HIT_MENU_ITEM         -> app/menu routing handles it
UI_HIT_HELP_PANEL        -> read-only editor document/session handles it
```

UI does not own search, scroll, cursor movement, selection, autocomplete,
undo, or source commit.

---

## Corrected Input Flow

```text
GLUT event
  -> imrepl_ctrl receives raw input
  -> imrepl_ctrl determines focus / asks UI for UiHit
  -> imrepl_ctrl routes to the owning subsystem

If the hit/focus is a text document:
  -> editor_handle_key / editor_handle_mouse / editor_handle_scroll
  -> editor mutates EditorState internally
  -> REPL is not involved unless source text is committed

If the hit is a variable slider:
  -> variable_panel subsystem handles it

If the hit is a replay button:
  -> replay subsystem handles it

If the hit is scene/camera:
  -> scene/viewport controller handles it
```

Most input flows do not cross into REPL:

```text
mouse wheel over code text -> editor scrolls
Ctrl+G                     -> editor opens search
Tab                        -> editor accepts autocomplete into active input
click text                 -> editor moves cursor / selection
```

---

## Editor Services Boundary

The editor owns commit orchestration, but REPL semantics are injected as
services. This prevents the editor from rummaging through REPL globals and
prevents `imrepl_ctrl` from becoming the editor implementation.

```c
typedef struct {
    ReplCompileContext (*context_at)(int line_idx, void *user);

    ReplCompileResult (*compile)(const char *text,
                                 const ReplCompileContext *ctx,
                                 ReplCompiledChange *out,
                                 char *err,
                                 int err_size,
                                 void *user);

    void (*apply_repl_change)(const ReplCompiledChange *change,
                              void *user);

    EditorCompletionProvider completion_provider;
    void *user;
} EditorServices;
```

`imrepl_ctrl` wires this service table. The editor calls the services when it
needs source semantics. Most editor operations do not touch the services at all.

---

## Editor / REPL Commit Boundary

REPL compiles proposed source changes. It does not read editor globals and does
not write editor text.

```c
typedef enum {
    REPL_COMPILED_NO_CHANGE,
    REPL_COMPILED_REPLACE_ONE,
    REPL_COMPILED_INSERT_ONE,
    REPL_COMPILED_INSERT_MANY,
    REPL_COMPILED_DELETE_RANGE,
    REPL_COMPILED_LOAD_ALL
} ReplCompiledChangeKind;

#define MAX_COMMIT_CMDS MAX_COMMANDS

typedef struct {
    ReplCompiledChangeKind kind;
    int   pos;
    int   count;
    GLCmd cmds[MAX_COMMIT_CMDS];
    char  text[MAX_COMMIT_CMDS][MAX_LINE_LEN];
    char  commit_message[REPL_STATUS_TEXT_MAX];
} ReplCompiledChange;

typedef enum {
    REPL_COMPILE_OK = 0,
    REPL_COMPILE_ERROR
} ReplCompileResult;

ReplCompileResult repl_compile(const char *line,
                               const ReplCompileContext *ctx,
                               ReplCompiledChange *out,
                               char *err_msg, int err_size);

void repl_apply_compiled_change(const ReplCompiledChange *change);
```

Commit path:

```text
editor_commit_current_line(editor, services)
  -> editor gathers proposed source text + context
  -> services.compile(text, ctx) returns ReplCompiledChange or diagnostic

on success:
  -> editor starts undo transaction
  -> editor_buffer_apply(change.text)
  -> services.apply_repl_change(change.cmds)
  -> editor commits undo transaction

on error:
  -> editor records rejected/diagnostic state
  -> imrepl_ctrl routes diagnostic to status/display
  -> no editor-buffer mutation
  -> no REPL command-state mutation
  -> no undo entry
```

---

## What Needs REPL Compile

Only text/program-changing operations go through `repl_compile()`:

```text
commit current source line
replace committed source line
insert committed source line
delete committed source line/range
cut committed source line/range
paste committed source lines
reformat source document
load source document
color edit that rewrites command source text
```

These operations must update editor text and REPL command state as one
transaction.

---

## What Does Not Need REPL Compile

Editor-local operations do not call `repl_compile()`:

```text
cursor movement
selection movement
scrolling
search open / set query / next / previous / close
autocomplete popup navigation
accept autocomplete into active input buffer
read-only help document scrolling
read-only help document search
render highlight updates
cursor blink
status banner display
```

Some of these may update `EditorState`. Some may update `UiState`. None of them
change the parsed program.

---

## Editor / REPL Completion Boundary

Autocomplete state belongs to the editor. Semantic completion rules belong to
providers.

The editor does not know what variables are. It asks a provider for candidates
using the current document/cursor context:

```c
typedef struct {
    const char *display;
    const char *insert_text;
    const char *hint;
} EditorCompletion;

typedef int (*EditorCompletionProvider)(
    const EditorDocumentView *doc,
    int line_idx,
    int char_idx,
    EditorCompletion *out,
    int max_out,
    void *user);
```

The REPL may implement a provider that offers command names, syntax forms,
variables, and context-sensitive hints. The editor owns popup state, selected
candidate, ghost text, and insertion behavior.

---

## Status And Diagnostics

```text
REPL returns diagnostics. It does not call set_status.
Editor commit returns commit messages. It does not call set_status.
imrepl_ctrl routes diagnostics/status messages to UiState/editor display.
```

Existing `set_status()` calls inside `repl_*` and `editor_*` modules are
transitional. The final boundary is diagnostic return values, not deep UI-state
mutation.

---

## Variable Panel And Replay Are Separate Subsystems

The variable panel is not part of the editor. It owns its slider/drag behavior
and writeback policy. It may ask the REPL/app layer to update a variable value,
and the resulting program/render state may change. It becomes editor-related
only if it rewrites committed source text.

Replay is not part of the editor. Replay owns play/pause/step/speed and replay
program-counter state. Replay may produce editor overlays or annotations, but
those overlays are supplied to the editor/view; replay does not become
editor-owned.

Rule:

```text
Other systems may provide overlays to the editor.
They do not become editor-owned unless they are text-document behavior.
```

---

## Corrected Phase 4

Phase 4 should not introduce a giant `UiAction` enum.

Correct Phase 4 target:

```text
UI hit-tests and renders.
Editor handles text-document input.
imrepl_ctrl routes app-level input to the right subsystem.
```

Checklist:

```text
1. Identify UI input handlers that directly mutate editor state.
2. Replace mutation with neutral UiHit output where possible.
3. Route text-document events into coarse editor controller functions.
4. Route variable-panel events into variable-panel subsystem functions.
5. Route replay events into replay subsystem functions.
6. Route camera/viewport events into scene/viewport controller functions.
7. Keep UI renderers snapshot-only.
8. Keep REPL compile limited to committed source mutations.
9. Keep imrepl_ctrl from gaining one wrapper per editor operation.
```

---

## Phase Status

The original plan's migration history remains useful. As of the current branch,
these have landed or are in-flight:

```text
1. tools: add editor ownership audit report                         landed
2. docs: record baseline editor ownership audit counts              landed
3. refactor: add EditorState + UiState storage + forwarders         landed
4. refactor: migrate editor_buffer slice to EditorState             landed
5. refactor: migrate editor_input slice to EditorState              landed
6. refactor: migrate selection + clipboard slices to EditorState    landed
7. refactor: migrate search + autocomplete slices to EditorState    landed
8. refactor: migrate UI slices to UiState                           landed
9. refactor: migrate editor overlays + variable_drag to EditorState landed
10. refactor: rename editor-input convenience getters               landed
11. refactor: code_panel scroll split + guardrails                  landed
```

Remaining direction:

```text
12. Finish code_panel chrome/camera placement without moving editor behavior into imrepl_ctrl.
13. Drop repl_command_store_*_with_line[s]; pass EditorBufferView to REPL readers.
14. Split pure repl_compile validators from editor_commit transaction orchestration.
15. Replace UI direct mutation with UiHit + owner routing; do not add giant UiAction.
16. Rename editor-owned modules and carve variable panel/replay as peer subsystems.
17. Refresh MODULES/ARCHITECTURE and promote audits/guards.
```

---

## Verification Targets

```sh
# REPL must not own editor text mutation.
grep -RIn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# expected after Phase 2: 0

# REPL readers that need source text take explicit EditorBufferView.
grep -RIn 'editor_buffer_line' repl_*.c repl_*.h
# expected after Phase 2: only explicit view consumers / no global reach-through

# UI hit-test/render files should not mutate editor/REPL/peer state.
grep -RInE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(|editor_.*_mut\(' ui_*.c ui_*.h
# expected after Phase 4: 0 or intentional allowlisted exceptions

# imrepl_ctrl should not mirror editor internals.
grep -RInE 'imrepl_ctrl_editor_(search|scroll|move|autocomplete|cut|paste|undo|redo)' . --include='*.c' --include='*.h'
# expected: 0
```

---

## Exit Criteria

The split is complete when:

```text
1. ReplState contains program state only.
2. EditorState contains text-document model/controller state.
3. UiState contains transient UI/session chrome and viewport state only.
4. UI renderers consume snapshots only.
5. UI hit-test code reports hits; it does not own editor behavior.
6. Search, scroll, selection, autocomplete, and undo are editor-owned.
7. Only source/program mutations call repl_compile().
8. repl_command_store mutates commands only.
9. editor_buffer mutates text only.
10. Undo restores editor text and REPL command state together.
11. Variable panel and replay remain separate subsystems.
12. imrepl_ctrl routes to coarse owner APIs instead of mirroring editor internals.
13. MODULES.md and checks enforce the same boundaries.
```

---

## Relationship To Other Plans

- `MODULES.md` remains the North Star for module ownership.
- `feature/editor-text-model-controller.md` is the focused companion for the
  corrected editor-as-model/controller boundary.
- `feature/editor-ownership-gap-cleanup.md` remains the audit and sequencing
  companion.
- The original `feature/editor-owns-text-completion.md` remains useful for
  detailed migration history, but its broad `UiAction` Phase 4 should be read
  as superseded.
