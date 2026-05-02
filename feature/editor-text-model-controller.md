# Plan: Editor as Text Model/Controller

## Purpose

This document supersedes the broad Phase 4 `UiAction` direction in
`feature/editor-owns-text-completion.md`.

The correction is simple:

```text
The editor is the model/controller for textual documents.
UI is a view and hit-test layer.
REPL validates and compiles committed source text.
imrepl_ctrl routes between subsystems.
```

The editor should not be reduced to a passive text buffer behind UI
actions. It should own text behavior: key handling, mouse handling,
scrolling, search, autocomplete state, undo, and commit orchestration.

---

## Target Contract

```text
Editor owns textual documents:
    editable source text
    read-only text documents
    cursor
    selection
    scroll
    search
    autocomplete state
    clipboard
    undo / redo
    key and mouse behavior for text documents
    source commit orchestration

UI owns view services:
    rendering snapshots
    drawing text, highlights, gutters, panels, widgets
    measuring text
    hit-testing visual rows, chars, buttons, swatches, sliders

REPL owns source semantics:
    parse / validate / compile committed source text
    indentation and context information
    diagnostics
    completion candidates / rules
    parsed program state

imrepl_ctrl owns routing:
    raw GLUT callback entry
    focus / region dispatch
    cross-subsystem coordination
    snapshot construction
```

Only committed source/program changes go through REPL validation. Editor-local
operations do not.

---

## Corrected Mental Model

The prior Phase 4 direction drifted toward:

```text
UI input handler -> large UiAction enum -> controller mutates everything
```

That is not the right model. It makes UI look like the editor controller.

The corrected model is:

```text
UI hit-tests and renders.
Editor handles text-document behavior.
imrepl_ctrl routes app-level input to the owning subsystem.
REPL validates only committed source mutations.
```

A click or key event over a code panel is not primarily a UI action. It is
text-document input. UI can report where the event landed; the editor decides
what that means.

---

## Example Input Flows

### Mouse wheel over code text

```text
GLUT wheel event
  -> imrepl_ctrl receives raw input
  -> UI hit-test says: code document region
  -> imrepl_ctrl routes to editor_scroll(editor, delta)
  -> editor updates scroll state
  -> next frame snapshot renders new scroll position
```

No REPL compile. No program mutation.

### Click in code text

```text
GLUT mouse press
  -> imrepl_ctrl receives raw input
  -> UI hit-test says: source line N, visual row R, char C
  -> imrepl_ctrl routes to editor_handle_text_click(editor, hit)
  -> editor updates cursor / selection / focused document
```

No REPL compile unless the interaction causes a committed source edit.

### Key press while editor is focused

```text
GLUT key event
  -> imrepl_ctrl routes to editor_handle_key(editor, key, mods)
  -> editor updates active input, cursor, selection, autocomplete, etc.
```

No REPL compile until the editor reaches a commit path.

### Commit current source line

```text
editor_commit_current_line(editor, repl)
  -> editor gathers proposed source text + commit context
  -> repl_compile(text, ctx) returns ReplCompiledChange or diagnostic

on success:
  -> editor starts undo transaction
  -> editor_buffer_apply(change.text)
  -> repl_apply_compiled_change(change.cmds)
  -> editor commits undo transaction

on error:
  -> editor records rejected/diagnostic state
  -> imrepl_ctrl routes diagnostic to status/display
  -> no editor-buffer mutation
  -> no REPL command-state mutation
  -> no undo entry
```

This is the only kind of flow that needs REPL validation.

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
UI_HIT_CODE_TEXT        -> editor handles it
UI_HIT_COLOR_SWATCH     -> editor commit path if it rewrites source text
UI_HIT_VARIABLE_SLIDER  -> variable-panel subsystem handles it
UI_HIT_REPLAY_BUTTON    -> replay subsystem handles it
UI_HIT_MENU_ITEM        -> app/menu routing handles it
UI_HIT_HELP_PANEL       -> read-only editor document or help subsystem handles it
```

UI does not own search, scroll, cursor movement, selection, autocomplete,
undo, or source commit.

---

## Editor-Owned Behavior

The editor owns model/controller behavior for text documents:

```text
text buffers
editable vs read-only document mode
cursor
selection
scroll
search
autocomplete popup state
clipboard for text/document ranges
undo / redo
keyboard behavior for text documents
mouse behavior for text documents
commit lifecycle for editable source documents
```

The editor can display multiple document kinds:

```text
source document      editable; has REPL-backed commit path
help document        read-only; searchable and scrollable
output/log document  read-only; searchable and scrollable
annotation document  read-only or overlay-backed
```

Only source documents have a REPL compile/apply path.

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

On success:

```c
editor_undo_begin_transaction();
  editor_buffer_apply(&change);          /* writes editor text only */
  repl_apply_compiled_change(&change);   /* writes ReplState only */
editor_undo_commit_transaction();        /* undo restores both halves */
```

On failure:

```text
no editor text mutation
no REPL command mutation
no undo entry
diagnostic routed back through imrepl_ctrl
```

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

The variable panel is not part of the editor. It displays variables and handles
slider/drag behavior. It may ask the REPL/app layer to update a variable value,
and the resulting program/render state may change. It becomes editor-related
only if it rewrites committed source text.

Replay is also not part of the editor. Replay owns play/pause/step/speed and
replay program-counter state. Replay may produce editor overlays or annotations,
but those overlays are supplied to the editor/view; replay does not become
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

Input routing may still use small typed event/result structs, but search,
scroll, autocomplete, cursor, selection, and undo remain editor behavior. Only
source-commit operations go through REPL validation.

A useful Phase 4 checklist:

```text
1. Identify current UI input handlers that directly mutate editor state.
2. Replace mutation with neutral hit-test output where possible.
3. Route text-document events into editor controller functions.
4. Route variable-panel events into variable-panel subsystem functions.
5. Route replay events into replay subsystem functions.
6. Route camera/viewport events into scene/viewport controller functions.
7. Keep UI renderers snapshot-only.
8. Keep REPL compile limited to committed source mutations.
```

---

## Relationship To Existing Plans

`MODULES.md` remains the North Star for module ownership.

`feature/editor-owns-text-completion.md` remains useful for the Phase 1/2/3
migration history, but its broad Phase 4 `UiAction` section should be read as
superseded by this document.

`feature/editor-ownership-gap-cleanup.md` remains the audit and sequencing
companion.

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
12. MODULES.md and checks enforce the same boundaries.
```
