# Plan: Three-Layer Ownership Split (Editor / REPL / UI)

## Target Contract

```
Editor owns editable text, cursor, selection, navigation, and undo
transactions.

REPL owns validation / compilation of committed editor text into
command / state changes.

UI owns rendering and hit-testing, and emits editor actions rather
than mutating REPL / editor state directly.
```

That is the load-bearing definition for this plan. Everything below
exists to make the codebase obey it.

## Why The Spirit Isn't Realized Today

`feature/editor-owns-text.md` (Steps 2–6) fixed the *data shape* —
`GLCmd.source[]` is gone, the parser returns `ReplParsedLine`, the
controller pushes editor-overlay snapshots. It did not move
*ownership*. Three concrete violations of the target contract:

### 1. Editor state lives inside REPL state

`ReplRuntimeState` holds editor-owned slices today:

```
editor_input          editor_buffer          editor_transformers
editor_highlights     editor_virtual_lines   selection
clipboard             autocomplete           search
variable_drag         code_panel.scroll      code_panel.scroll_follow_cursor
```

Plus UI render-time slices:

```
code_panel.cursor_visible   code_panel.blink_tick   code_panel.cursor_px/py
help.visible                profile_panel.mode      variable_panel.visible
status                      viewport                pointer
```

A REPL state struct should hold the *program* — document, flat program,
variables, replay, scenes, render, presentation — not the typing buffer
or the search query.

### 2. REPL modules read and write editor text

- `repl_command_store_*_with_line[s]` writes `editor_buffer.lines[]` in
  lockstep with the cmd array.
- `repl_replay_annotations.c::replay_visible_text(cmd_idx)` reaches
  into the editor buffer to compose annotations.
- `repl_export.c` reads the editor buffer when serializing.
- `repl_parser.c` reparse paths read the editor buffer for context.

In the target contract, REPL is *given* text by the editor on commit.
It does not own a buffer; it does not write to one.

### 3. UI input handlers mutate state directly

`ui_panels_handle_code_panel_press` calls `ui_color_picker_open`,
`repl_action_*`, `repl_clipboard_*`, etc. inline. The render path is
already snapshot-only (Phase B of `push-architecture-ui.md` finished
that). Phase C — convert input handlers to return a `UiAction` list
that the controller dispatches — was deferred. That deferral is the
gap.

### 4. `repl_editor.c` is too broad to keep its name

It currently combines: GLUT input router + UI bridge for
menus/search/help/rename/replay/camera + text editor model + commit
orchestration + direct coordination of undo/store/status/dirty/AC. Each
of those belongs in a different file under the target contract.

## Target State Shape

```c
/* The user's program — what the REPL parses, flattens, executes,
 * replays, exports. */
typedef struct {
    ReplDocumentState           document;
    ReplFlatProgramState        flat_program;
    ReplVariableState           variables;
    ReplReplayRuntimeState      replay;
    ReplSceneRuntimeState       scenes;
    ReplImportExportState       import_export;
    ReplRenderState             render;        /* GL pipeline knobs */
    ReplPresentationState       presentation;  /* render toggles */
} ReplState;

/* Editable text, cursor, selection, navigation, undo. */
typedef struct {
    ReplEditorInputState        input;        /* typing buffer + cursor */
    ReplEditorBuffer            buffer;       /* committed lines */
    ReplSelectionState          selection;
    ReplClipboardState          clipboard;    /* + lines[][] sidecar */
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    ReplVariableDragState       variable_drag;
    EditorScrollState           scroll;       /* extracted from code_panel */
    EditorUndoRing              undo;         /* moved from sidecar */
    EditorCameraNavState        camera_nav;   /* mouse-driven view nav (see open question) */
} EditorState;

/* Render-time scratch + chrome visibility. UI owns geometry it
 * computes per frame; nothing here is part of "the program" or "the
 * editor session." */
typedef struct {
    ReplViewportState           viewport;
    ReplPointerState            pointer;
    ReplStatusState             status;        /* transient message */
    ReplHelpState               help;          /* visibility */
    ReplVariablePanelState      variable_panel;/* visibility */
    ReplProfilePanelState       profile_panel; /* visibility */
    EditorCursorBlinkState      cursor_blink;  /* render-only */
    /* Per-frame snapshots populated by the controller, consumed by
     * renderers (already pointer-shaped on UiRenderSnapshot). */
} UiState;
```

`imrepl_ctrl` owns all three as static singletons. The frame loop:

```
input event
  -> ui_*_input_handler(snapshot, event) -> UiActionList
  -> imrepl_ctrl_dispatch(action_list)
       -> mutates UiState / EditorState
       -> on commit: repl_compile(text, ctx) -> ParsedLine | error
                     -> on success: editor_undo_push (snapshot prior text + cmd)
                                    editor_buffer_replace(idx, parsed.text)
                                    repl_apply_replace(idx, parsed.cmd)
       -> on REPL-side actions (save / replay / load): mutates ReplState

frame render
  -> imrepl_ctrl_build_scene_config(SceneRenderConfig out)
  -> imrepl_ctrl_build_ui_snapshot(UiRenderSnapshot out)
  -> scene/ui renderers read snapshots only
```

### Open question: presentation / camera placement

- **`presentation`** persists with the program via workspace `@cfg`
  lines and is read by scene rendering. Recommendation: stay on
  `ReplState`. Revisit only if a non-GUI REPL build target ever lands.
- **`camera`** is mouse-driven by editor input but feeds scene
  rendering. Recommendation: editor mouse handlers emit
  `UI_ACTION_CAMERA_*`; the resulting *transient* camera state lives in
  `EditorState.camera_nav` (or a new `viewport_camera`); the scene reads
  via snapshot. Resolve before Phase 5 when `repl_camera_controls.c`
  renames.

## REPL Public Contract After The Split

```c
/* Validate + compile a single committed line. Pure: no editor state,
 * no UI state, no globals beyond the REPL's own. */
typedef struct {
    GLCmd cmd;                     /* parsed command (type, args, flags) */
    char  text[MAX_LINE_LEN];      /* canonical normalized text */
} ReplParsedLine;

typedef enum {
    REPL_COMPILE_OK = 0,
    REPL_COMPILE_ERROR
} ReplCompileResult;

ReplCompileResult repl_compile(const char *line,
                               const ReplCompileContext *ctx,
                               ReplParsedLine *out,
                               char *err_msg, int err_size);

/* Apply a compiled command to REPL state at a specific document index.
 * Mutates document + dependent caches; does not touch editor state. */
void repl_apply_insert(int pos, const GLCmd *cmd);
void repl_apply_replace(int pos, const GLCmd *cmd);
void repl_apply_delete(int start, int count);
void repl_apply_load(const GLCmd *cmds, int count);

/* Programmatic queries the editor uses to validate / preview. */
int  repl_indent_chars_for(int pos);
int  repl_in_begin_block_at(int pos);
ReplCompileContext repl_context_at(int pos);
```

REPL no longer takes text from a buffer it doesn't own. The editor
calls `repl_compile(typed_text, ctx)`, then on success calls
`repl_apply_*` with the result, then writes the canonical text into its
own buffer.

## UiAction Enumeration (Phase 4 starting cut)

```c
typedef enum {
    /* Text + cursor */
    UI_ACTION_INSERT_TEXT, UI_ACTION_DELETE_TEXT,
    UI_ACTION_MOVE_CURSOR, UI_ACTION_NAVIGATE_LINE,
    UI_ACTION_COMMIT_LINE, UI_ACTION_TOGGLE_INSERT_MODE,
    UI_ACTION_REFORMAT,

    /* Selection + clipboard */
    UI_ACTION_SELECT_BEGIN, UI_ACTION_SELECT_EXTEND, UI_ACTION_SELECT_CLEAR,
    UI_ACTION_CLIPBOARD_CUT, UI_ACTION_CLIPBOARD_COPY, UI_ACTION_CLIPBOARD_PASTE,

    /* Undo */
    UI_ACTION_UNDO, UI_ACTION_REDO,

    /* Search */
    UI_ACTION_SEARCH_OPEN, UI_ACTION_SEARCH_NEXT,
    UI_ACTION_SEARCH_PREV, UI_ACTION_SEARCH_CLOSE,

    /* Autocomplete */
    UI_ACTION_AUTOCOMPLETE_ACCEPT, UI_ACTION_AUTOCOMPLETE_DISMISS,

    /* REPL-side actions */
    UI_ACTION_SAVE, UI_ACTION_LOAD_FILE, UI_ACTION_LOAD_EXAMPLE,
    UI_ACTION_REPLAY_TOGGLE, UI_ACTION_REPLAY_STEP,

    /* Color picker / variable slider transactions */
    UI_ACTION_COLOR_PICKER_OPEN, UI_ACTION_COLOR_PICKER_DRAG,
    UI_ACTION_COLOR_PICKER_CLOSE,
    UI_ACTION_VAR_DRAG_BEGIN, UI_ACTION_VAR_DRAG_MOTION,
    UI_ACTION_VAR_DRAG_END,

    /* Camera / scene viewport */
    UI_ACTION_CAMERA_ORBIT, UI_ACTION_CAMERA_PAN, UI_ACTION_CAMERA_ZOOM,

    /* Config */
    UI_ACTION_CONFIG_TOGGLE, UI_ACTION_CONFIG_CYCLE,

    /* Visibility */
    UI_ACTION_HELP_TOGGLE, UI_ACTION_VARIABLE_PANEL_TOGGLE,
    UI_ACTION_PROFILE_PANEL_CYCLE,
} UiActionKind;

typedef struct {
    UiActionKind kind;
    union {
        struct { int line, col; } cursor;
        struct { const char *text; int len; } text;
        struct { int line_idx; } commit;
        struct { float dx, dy; } camera;
        struct { int cmd_idx; float r, g, b, a; } color;
        struct { ReplConfigKey key; } cfg;
        ...
    } data;
} UiAction;

#define MAX_UI_ACTIONS 16
typedef struct { UiAction items[MAX_UI_ACTIONS]; int count; } UiActionList;

void ui_panels_handle_code_panel_press(int mx, int my, UiActionList *out);
```

`imrepl_ctrl_dispatch_action(const UiAction *)` is the single mutation
gate. It is the only function allowed to mutate `ReplState` /
`EditorState` from input.

---

## Phase 0 — Audits Before Moving Code

This phase lands before structural changes so every later phase has a
measurable exit criterion. The first goal is *not* zero — it is making
drift visible.

### 0.1 Add `scripts/audit_editor_ownership.sh`

```sh
#!/bin/sh
set -eu

echo "== editor/ui-like state still exposed through repl_state =="
grep -RInE \
  'repl_state_(editor_|clipboard|selection|autocomplete|search|status|variable_panel|variable_drag|help|code_panel|profile_panel|viewport|pointer|camera)' \
  -- '*.c' '*.h' \
  | grep -v '^tests/' || true

echo "== command store text-aware API usage =="
grep -RInE \
  'repl_command_store_.*_with_line|repl_command_store_.*_with_lines' \
  -- '*.c' '*.h' || true

echo "== REPL modules reading editor-buffer text directly =="
grep -RInE \
  'repl_state_editor_buffer|editor_buffer_line' \
  repl_*.c repl_*.h \
  | grep -v '^repl_state' || true

echo "== UI files with live mutation calls =="
grep -RInE \
  'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(|repl_state_.*_mut\(' \
  ui_*.c ui_*.h || true

echo "== repl_editor broad dependency inventory =="
grep -nE '^#include ' repl_editor.c || true
```

Wired as a report target:

```make
.PHONY: audit-editor-ownership
audit-editor-ownership:
	@scripts/audit_editor_ownership.sh
```

### 0.2 Record baseline counts

After the script lands, fill in a baseline section in this doc:

```text
Date:
Branch:
repl_state editor/ui-like accessor hits:
command store _with_line API hits:
REPL direct editor-buffer read hits:
UI live mutation hits:
repl_editor.c include count:
Known intentional exceptions:
```

### 0.3 Tolerated-exception markers

Use a searchable annotation so the audit can count knowns vs. unknowns:

```c
/* EDITOR_OWNERSHIP_TODO(phase-2): takes EditorBufferView instead. */
```

---

## Phase 1 — Carve `EditorState` and `UiState` out of `ReplRuntimeState`

Mechanical storage movement. No behavior change. Lands as a series of
slice-by-slice migrations rather than one giant rename, with
compatibility wrappers that delete in 1.x.

### 1.1 Add new state structs and compatibility wrappers

Add files:

```text
editor_state.h         editor_state_views.h   editor_state_owners.h
editor_state.c
ui_state.h             ui_state_views.h       ui_state_owners.h
ui_state.c
```

Move storage first, callers second. Keep temporary forwarders:

```c
ReplEditorInputView repl_state_editor_input(void) {
    return editor_state_input();
}

ReplStatusState repl_state_status(void) {
    return ui_state_status();
}
```

This lets ownership move slice by slice without one mass-rename commit.
Each forwarder gets removed in the slice's own commit once callers
have been migrated.

### 1.2 Slice migration order

Each migration is a separate commit with passing tests at the end:

1. `editor_buffer` — most central; everything else builds on it.
2. `editor_input` (typing buffer / cursor / edit-line / insert mode /
   pending newline).
3. `selection` + `clipboard`.
4. `search` + `autocomplete`.
5. transformer / highlight / virtual-line snapshot lists.
6. `code_panel` scroll + scroll-follow → `EditorState.scroll`.
7. `status` / `help` / `profile_panel` / `variable_panel` →
   `UiState`.
8. `viewport` / `pointer` → `UiState`.
9. `camera_nav` decision: `EditorState.camera_nav` (if mouse-input
   transient) or scene/app state (if output of an action).

### 1.3 Promote guards slice-by-slice

Do not add one big failing check. Add narrow checks per slice once it
migrates:

```make
check-no-repl-editor-buffer-access:
	@if grep -RIn 'repl_state_editor_buffer' -- '*.c' '*.h' \
	     | grep -v '^tests/' \
	     | grep -v 'EDITOR_OWNERSHIP_TODO'; then \
	    echo 'ERROR: editor buffer must use editor_buffer_* accessors'; exit 1; \
	fi
```

### 1.4 Capture / restore symmetry

Add `editor_state_capture/restore` and `ui_state_capture/restore`
mirroring `repl_state_*`. Wire them into `repl_state_reset_all()` so
every test reset clears all three.

### Verification

```sh
make test && make test-stubs
make audit-editor-ownership

grep -E 'repl_state_(editor_|clipboard|selection|autocomplete|search|status|variable_panel|variable_drag|help|code_panel|profile_panel|viewport|pointer)' \
    *.c | grep -v 'imrepl_ctrl.c\|editor_state.c\|ui_state.c' \
        | grep -v 'EDITOR_OWNERSHIP_TODO'
# should be 0 by end of phase
```

---

## Phase 2 — REPL stops touching editor text

The most important boundary fix after state extraction.

### 2.1 Editor-buffer mutation API

```c
const char *editor_buffer_line(int idx);
int  editor_buffer_count(void);
void editor_buffer_set_count(int count);

int  editor_buffer_insert_line(int pos, const char *line);
int  editor_buffer_insert_lines(int pos, const char *const *lines, int count);
int  editor_buffer_replace_line(int pos, const char *line);
int  editor_buffer_delete_range(int start, int count);
int  editor_buffer_load(const char *const *lines, int count);
void editor_buffer_clear(void);

EditorBufferView editor_buffer_view(void);
```

### 2.2 Drop command-store text APIs

Delete:

```text
repl_command_store_insert_many_with_lines
repl_command_store_insert_one_with_line
repl_command_store_replace_one_with_line
repl_command_store_load_with_lines
```

The store mutates only `GLCmd` arrays + cursor / count / capacity.

### 2.3 Convert combined writes to explicit transactions

Before:

```c
repl_command_store_replace_one(&store, idx, &cmd, line);
```

After:

```c
if (!repl_command_store_replace_one(&store, idx, &cmd))
    return 0;
if (!editor_buffer_replace_line(idx, line))
    return 0;
```

Where rollback matters, wrap both in an editor commit transaction.
Initial implementation can use existing snapshots; later phases refine
the undo shape.

### 2.4 `EditorBufferView` for REPL consumers

```c
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int        line_count;
} EditorBufferView;
```

Convert global text reads to parameters:

```c
void repl_replay_annotations_prepare(EditorBufferView text);
int  repl_export_save_workspace(..., EditorBufferView text);
const char *repl_replay_code_panel_get_command_display_text(EditorBufferView text,
                                                            int cmd_idx);
```

Likely consumers: `repl_replay_annotations.c`, `repl_export.c`,
`repl_flatten.c` reparse helpers, `repl_executor.c` flat-text helpers,
`repl_debug.c`, `repl_code_panel_document.c` (until it moves to UI in
Phase 5).

### Verification

```sh
make test && make test-stubs

grep -RIn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# expected: 0

grep -l '#include "editor_state.h"' repl_*.c | grep -v repl_state.c
# expected: empty (or only marked transition exceptions)
```

---

## Phase 3 — `repl_compile` is the validation gate

The editor proposes a text change; the REPL compiles it without
mutation; the editor applies the result as a transaction.

### 3.1 Behavior preservation list

Do not change UX while splitting ownership. Tests must show:

```text
- Enter on unchanged line enters insert mode / advances as before.
- Navigation commits modified input before moving.
- Rejected navigation restores committed model and preserves
  rejected-status / status-text.
- Empty insert exits insert mode as before.
- Var declaration restrictions remain unchanged.
- For-loop / function / if structured commits remain unchanged.
- Autocomplete refresh remains unchanged.
- Normals / flat dirty flags remain unchanged.
- Color picker edits still reparse and update text + command together.
```

### 3.2 Required focused tests

```text
- Modified line + failed validation does not alter editor buffer.
- Modified line + failed validation does not alter command array.
- Modified line + successful validation updates both.
- Commit-before-navigation rejected parse restores previous input/model.
- Undo after successful commit restores both text and command.
- Block-structured commit returns all lines/cmds before applying any
  mutation (atomic insert).
- Color picker failed reparse does not corrupt text or cmd args.
```

### 3.3 API checkpoints

```c
/* Compiler side (REPL) */
ReplCompileResult repl_compile(const char *line,
                               const ReplCompileContext *ctx,
                               ReplParsedLine *out,
                               char *err_msg, int err_size);

/* Apply side (REPL) — pure cmd-array mutators */
void repl_apply_insert(int pos, const GLCmd *cmd);
void repl_apply_replace(int pos, const GLCmd *cmd);
void repl_apply_delete(int start, int count);
void repl_apply_load(const GLCmd *cmds, int count);

/* Editor side */
EditorCommitResult editor_commit_current_input(EditorState *editor,
                                               ReplState *repl,
                                               int enter_mode);
```

### 3.4 Split `repl_commit.c`

1. Pure validators move into `repl_compile.c`:
   `try_commit_float_decl_validate`, `try_commit_for_loop_validate`,
   `try_commit_func_def_validate`, `try_commit_if_block_validate`,
   `try_commit_close_brace_validate`, `try_assign_variable_validate`.
   Each returns `ReplParsedLine` plus an error code; no mutations.
2. Orchestration moves to `editor_commit.c` — validates, on success
   pushes undo, writes editor buffer, calls `repl_apply_*`.
3. `repl_editor.c` callers switch to `editor_commit_*` entry points.

### Verification

```sh
make test_repl_editor
make test_repl_core_commit
make test && make test-stubs

# No validator should mutate state:
grep -E 'try_commit_(float_decl|for_loop|func_def|if_block|close_brace|var)' \
    repl_compile.c | grep -E 'editor_buffer_|repl_apply_|repl_command_store_|repl_state_.*_mut'
# expected: 0
```

---

## Phase 4 — UI input handlers emit `UiAction`

The largest phase; behavior-drift risk is highest. Land in waves.

### 4.1 Migration waves

1. **Keyboard text editing** —
   `UI_ACTION_INSERT_TEXT`, `_DELETE_TEXT`, `_MOVE_CURSOR`,
   `_COMMIT_LINE`, `_TOGGLE_INSERT_MODE`, `_REFORMAT`.
2. **Code-panel mouse selection** —
   `UI_ACTION_SELECT_BEGIN`, `_EXTEND`, `_CLEAR`.
3. **Clipboard / search / autocomplete** —
   `UI_ACTION_CLIPBOARD_*`, `UI_ACTION_SEARCH_*`,
   `UI_ACTION_AUTOCOMPLETE_*`.
4. **Color picker / variable slider** —
   `UI_ACTION_COLOR_PICKER_*`, `UI_ACTION_VAR_DRAG_*`.
5. **Menu / config / replay / load / save** —
   `UI_ACTION_CONFIG_*`, `UI_ACTION_REPLAY_*`, `UI_ACTION_LOAD_*`,
   `UI_ACTION_SAVE`.
6. **Camera / scene gestures** —
   `UI_ACTION_CAMERA_ORBIT`, `_PAN`, `_ZOOM`.

### 4.2 Dispatch gate

```c
void imrepl_ctrl_dispatch_action(const UiAction *action);
void imrepl_ctrl_dispatch_actions(const UiActionList *actions);
```

The only function in the codebase allowed to mutate `EditorState` /
`ReplState` from an input event.

### 4.3 Tighten UI guards per wave

After each wave, forbid direct mutation calls from migrated UI files.
The existing `check-ui-no-repl-state-read` extends to forbid mutation
calls (`repl_action_*`, `repl_command_store_*`, `editor_state_*_mut`,
etc.) from `ui_*.c`.

### Verification

```sh
make test && make test-stubs
make check-ui-no-repl-state-read
make check-ui-renderer-takes-view

grep -rE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(' \
    ui_*.c
# expected: 0 by end of phase
```

Manual smoke required (see Verification Matrix).

---

## Phase 5 — Rename to match ownership

Lands only after behavior + ownership have already moved. Renames are
mechanical with redirect headers during transition.

### 5.1 Rename checklist

| Old | New | Owner |
|---|---|---|
| `repl_editor.c` | split into `editor_input.c` (router) + `editor_commit.c` (already from Phase 3) | editor |
| `repl_undo.c` | `editor_undo.c` | editor |
| `repl_clipboard.c` | `editor_clipboard.c` | editor |
| `repl_search.c` | `editor_search.c` | editor |
| `repl_inline_rename.c` | `editor_inline_rename.c` | editor |
| `repl_var_drag.c` | `editor_var_drag.c` | editor |
| `repl_autocomplete.c` | `editor_autocomplete.c` | editor |
| `repl_layout.c` | `ui_layout.c` | UI |
| `repl_code_panel_layout.c` | `ui_code_panel_layout.c` (pure wrap iterator) | UI |
| `repl_code_panel_document.c` | `editor_code_panel_document.c` (owns scroll / hit-test, editor-state-shaped) | editor |
| `repl_actions.c` | split into `ui_action_dispatch.c` + `editor_actions.c` + REPL-side actions stay | mixed |
| `repl_camera_controls.c` | `scene_camera_controls.c` (or keep mouse-handler in editor + scene-side reader) | scene/UI |

### 5.2 Redirect headers during transition

```c
/* repl_undo.h -- transitional compatibility header. */
#ifndef REPL_UNDO_H
#define REPL_UNDO_H
#include "editor_undo.h"
#endif
```

Remove redirect headers after downstream includes migrate.

### 5.3 Update docs and callgraph grouping per rename

Every rename commit updates:

```text
MODULES.md
ARCHITECTURE.md
scripts/callgraph_file_groups.json
Makefile source lists
relevant tests
```

### Verification

```sh
make test && make test-stubs
make callgraph-files

ls editor_*.c
# matches the table above
ls repl_*.c | wc -l
# strictly fewer than before — only true REPL files remain
```

---

## Suggested Branch / Commit Breakdown

Branch name:

```text
feature/editor-ownership-gap-cleanup
```

Suggested commits (each buildable, each with green tests):

```text
1.  tools: add editor ownership audit report
2.  docs: record baseline editor ownership audit counts
3.  refactor: add EditorState and UiState storage with compatibility accessors
4.  refactor: migrate editor buffer accessors to editor namespace
5.  refactor: migrate editor input/cursor accessors to editor namespace
6.  refactor: migrate selection/clipboard/search/autocomplete accessors
7.  refactor: migrate status/help/panel/viewport/pointer slices to UiState
8.  refactor: remove text writes from repl_command_store
9.  refactor: pass EditorBufferView to REPL text consumers
10. test: cover failed validation and commit transaction invariants
11. refactor: split repl_compile validation from editor_commit application
12. refactor: introduce UiAction dispatch gate
13. refactor: migrate keyboard input handlers to UiAction
14. refactor: migrate mouse/menu/color/camera input handlers to UiAction
15. refactor: rename editor-owned modules
16. docs: refresh MODULES and ARCHITECTURE after ownership split
17. checks: promote editor ownership audits to hard guards
```

Avoid landing a commit that requires the next phase to restore green
tests.

---

## Verification Matrix

After every phase:

```sh
make test
make test-stubs
make audit-editor-ownership
```

After commit / undo phases:

```sh
make test_repl_editor
make test_repl_core_commit
make test_repl_command_store
```

After UI / render / input boundary changes:

```sh
make check-ui-no-repl-state-read
make check-ui-renderer-takes-view
make check-state-boundaries
```

### Manual smoke checklist

```text
- Type valid GL command; line commits and renders.
- Type invalid GL command; status error appears and previous command remains.
- Modify existing line; successful commit updates code panel and scene.
- Modify existing line with invalid text; navigation does not corrupt document.
- Undo after insert restores text and scene.
- Undo after color picker edit restores text and parsed color.
- Copy / cut / paste preserves line text.
- Search still finds committed text.
- Autocomplete still works from active input.
- Replay annotations still show source / eval text.
- Export / save still writes expected source text.
- Load workspace restores text and parsed command model.
- Menu / config / replay shortcuts still route through actions.
- Camera orbit / pan / zoom still work after UiAction migration.
```

---

## Exit Criteria

Cleanup is complete when all thirteen statements are true:

```text
1.  Program-owned slices live in ReplState.
2.  Editor-owned slices live in EditorState.
3.  UI / session chrome slices live in UiState.
4.  Editor text is mutated only through editor_buffer / editor_document APIs.
5.  repl_command_store mutates only GLCmd arrays and related command-store state.
6.  REPL modules that need source text receive EditorBufferView explicitly.
7.  Commit validation can be called without mutating editor text or command state.
8.  Successful commit application updates editor text and REPL command model
    as one transaction.
9.  Undo restores both editor text and REPL command state consistently.
10. ui_* renderers remain snapshot-only.
11. ui_* input handlers emit UiAction rather than mutating state directly.
12. imrepl_ctrl is the input-event mutation gate.
13. MODULES.md and ARCHITECTURE.md describe the same ownership boundaries
    enforced by Makefile checks.
```

---

## Trade-offs

**Pros**

- Three-layer contract is enforceable mechanically (Makefile guards).
- A "headless REPL" / "embedded REPL in another editor" / "scripted
  REPL test harness" becomes feasible — drop `EditorState` / `UiState`
  entirely.
- `ReplState` shrinks dramatically; `repl_state_capture/restore` get
  cheaper; clipboard's 1.88 MB lines sidecar moves off `ReplState`.
- UI input becomes a pure function `(snapshot, event) -> UiActionList`,
  testable without a live REPL.

**Cons**

- Phase 4 is the largest commit set and touches every input handler.
  Per-handler test coverage mitigates drift risk.
- Phase 5 invalidates external references to old file names; redirect
  headers cushion the transition.
- `presentation` / `camera` placement is genuinely ambiguous; this plan
  keeps `presentation` on `ReplState` and routes camera through the
  editor-input → controller-dispatch → scene-render path.

---

## Non-goals

```text
- sample.c -> imrepl.c rename (push-architecture-refinement.md R8)
- full repl_core.c dissolution (R10)
- color scheme / syntax theme extraction
- new editor UX behavior
- new parser features
- new command syntax
```

Those are separate tracks.

---

## Relationship To Other Plans

- **Supersedes** the deferred Phase C of
  `feature/push-architecture-ui.md` — the `UiAction` work is folded in
  here as Phase 4.
- **Builds on** `feature/editor-owns-text.md` Steps 2–6 (data shape) and
  `feature/gold-standard-state-ownership.md` Stage 7 (UI snapshot
  purity).
- **Companion** to `feature/editor-ownership-gap-cleanup.md` — that doc
  is the audit/landing-sequence checklist; this doc is the architectural
  contract. Read both together.
- **Independent** of `push-architecture-refinement.md` R10 (dissolve
  `repl_core.c`) and R8 (`sample → imrepl`).
