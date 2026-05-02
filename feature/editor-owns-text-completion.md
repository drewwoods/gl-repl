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
    EditorCameraNavState        camera_nav;   /* mouse-driven view nav */
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
  -> ui_*_input_handler(event)              [returns UiAction]
  -> imrepl_ctrl_dispatch(action)
       -> mutates EditorState
       -> on commit: repl_compile(text, ctx) -> ParsedLine | error
                     -> on success: mutates ReplState (document/flat)
                                    pushes onto EditorState.undo
                                    writes editor.buffer
       -> on REPL-side actions (save / replay / load): mutates ReplState

frame render
  -> imrepl_ctrl_build_scene_config(SceneRenderConfig out)
  -> imrepl_ctrl_build_ui_snapshot(UiRenderSnapshot out)
  -> scene/ui renderers read snapshots only
```

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

## UiAction Enumeration (Phase 4)

Initial cut, refined as call sites are migrated:

```c
typedef enum {
    /* Text + cursor */
    UI_ACTION_INSERT_TEXT,
    UI_ACTION_DELETE_TEXT,
    UI_ACTION_MOVE_CURSOR,
    UI_ACTION_NAVIGATE_LINE,        /* up/down/home/end */
    UI_ACTION_COMMIT_LINE,          /* "execute" — invokes repl_compile */
    UI_ACTION_TOGGLE_INSERT_MODE,
    UI_ACTION_REFORMAT,

    /* Selection + clipboard */
    UI_ACTION_SELECT_BEGIN,
    UI_ACTION_SELECT_EXTEND,
    UI_ACTION_SELECT_CLEAR,
    UI_ACTION_CLIPBOARD_CUT,
    UI_ACTION_CLIPBOARD_COPY,
    UI_ACTION_CLIPBOARD_PASTE,

    /* Undo */
    UI_ACTION_UNDO,
    UI_ACTION_REDO,

    /* Search */
    UI_ACTION_SEARCH_OPEN,
    UI_ACTION_SEARCH_NEXT,
    UI_ACTION_SEARCH_PREV,
    UI_ACTION_SEARCH_CLOSE,

    /* Autocomplete */
    UI_ACTION_AUTOCOMPLETE_ACCEPT,
    UI_ACTION_AUTOCOMPLETE_DISMISS,

    /* REPL-side actions */
    UI_ACTION_SAVE,
    UI_ACTION_LOAD_FILE,
    UI_ACTION_LOAD_EXAMPLE,
    UI_ACTION_REPLAY_TOGGLE,
    UI_ACTION_REPLAY_STEP,

    /* Color picker / variable slider transaction */
    UI_ACTION_COLOR_PICKER_OPEN,
    UI_ACTION_COLOR_PICKER_DRAG,
    UI_ACTION_COLOR_PICKER_CLOSE,
    UI_ACTION_VAR_DRAG_BEGIN,
    UI_ACTION_VAR_DRAG_MOTION,
    UI_ACTION_VAR_DRAG_END,

    /* Camera / scene viewport */
    UI_ACTION_CAMERA_ORBIT,
    UI_ACTION_CAMERA_PAN,
    UI_ACTION_CAMERA_ZOOM,

    /* Config */
    UI_ACTION_CONFIG_TOGGLE,
    UI_ACTION_CONFIG_CYCLE,

    /* Visibility */
    UI_ACTION_HELP_TOGGLE,
    UI_ACTION_VARIABLE_PANEL_TOGGLE,
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
```

UI input handlers fill an output buffer:

```c
typedef struct {
    UiAction items[MAX_UI_ACTIONS];
    int count;
} UiActionList;

void ui_panels_handle_code_panel_press(int mx, int my, UiActionList *out);
```

`imrepl_ctrl_dispatch_action(const UiAction *)` is the single mutation
gate. It is the only function allowed to mutate `ReplState` or
`EditorState` from input.

## Phases

Each phase is a contained commit; each builds clean and keeps `make
test` / `make test-stubs` green. The order is load-bearing.

### Phase 1 — Carve `EditorState` out of `ReplRuntimeState` (~1 day)

Mechanical struct surgery; no behavior change.

1. Define `EditorState` and `UiState` in new headers
   (`editor_state.h`, `ui_state.h`). Move the slices listed in *Target
   State Shape* off `ReplRuntimeState`.
2. Add `static EditorState g_editor_state` and `static UiState
   g_ui_state` in new `editor_state.c` / `ui_state.c`. Mirror
   `repl_state_capture/restore` with `editor_state_*` and `ui_state_*`.
3. Rename every accessor:
   - `repl_state_editor_buffer*` → `editor_state_buffer*`
   - `repl_state_clipboard*` → `editor_state_clipboard*`
   - `repl_state_search*` → `editor_state_search*`
   - `repl_state_status*` → `ui_state_status*`
   - …etc.
4. `UiRenderSnapshot` builder reads from all three structs; render
   signatures unchanged.
5. Add Makefile guards mirroring `check-views-no-owners` for
   `editor_state_owners.h` and `ui_state_owners.h`.

### Phase 2 — REPL stops touching editor text (~1 day)

1. Drop `repl_command_store_*_with_line[s]`. The store mutates the cmd
   array only.
2. Add an `editor_buffer_*` mutation API on `EditorState` for the text
   half. Each former `_with_line` call site becomes two adjacent calls
   (cmd write + text write) issued by the editor side.
3. Convert the three REPL readers to take an explicit
   `EditorBufferView`:
   ```c
   typedef struct {
       const char (*lines)[MAX_LINE_LEN];
       int        line_count;
   } EditorBufferView;
   ```
   - `repl_replay_annotations_prepare(EditorBufferView)`
   - `repl_export_save(path, EditorBufferView, ...)`
   - parser reparse paths take a `const char *` argument explicitly
4. The controller passes the view from `editor_state_buffer_view()`.
5. Verification: `grep '#include "editor_state.h"' repl_*.c` is empty.

### Phase 3 — `repl_compile` is the validation gate (~1 day)

1. Split `repl_commit.c`. Move pure validation into
   `repl_compile.c` — float-decl, for-loop, func-def, if-block,
   close-brace, var-assign validators each return `ReplParsedLine` plus
   an error. They do not call store mutators.
2. The orchestration — "try each validator in order, on success call
   `repl_apply_insert/replace`, on success append to undo, on success
   write editor buffer" — moves to `editor_commit.c`. This is the
   editor-side counterpart.
3. Public REPL surface becomes `repl_compile()` + `repl_apply_*`.
4. Verification: `repl_command_store.c` has no callers in `editor_*`
   that pass text; only `repl_apply_*` paths remain.

### Phase 4 — UI input handlers emit `UiAction` (~3-5 days)

The biggest phase. Walk every UI input entry point, convert from
direct mutation to action emission.

1. Define `UiAction`, `UiActionList`, `UiActionKind` in `ui_action.h`.
2. Add `imrepl_ctrl_dispatch_action(const UiAction *)`. This is the
   only function in the codebase allowed to mutate `EditorState` /
   `ReplState` from an input event.
3. Convert handlers in waves:
   - Wave A: keyboard (`ui_panels_handle_*`, character input) →
     `UI_ACTION_INSERT_TEXT` / `_DELETE_TEXT` / `_MOVE_CURSOR` /
     `_COMMIT_LINE`.
   - Wave B: mouse press / drag / release on the code panel →
     `UI_ACTION_SELECT_*`, `UI_ACTION_COLOR_PICKER_OPEN`, etc.
   - Wave C: menu bar dispatch → `UI_ACTION_CONFIG_TOGGLE`,
     `UI_ACTION_LOAD_EXAMPLE`, `UI_ACTION_SAVE`, etc.
   - Wave D: scene mouse (camera) → `UI_ACTION_CAMERA_*`.
4. Tighten `check-ui-no-repl-state-read` to forbid mutation calls
   (`repl_action_*`, `repl_command_store_*`, `editor_state_*_mut`)
   from `ui_*.c` files.

### Phase 5 — Rename to match ownership (~mostly mechanical)

Files whose contents are now editor- or UI-owned change prefix:

| Old | New | Owner |
|---|---|---|
| `repl_editor.c` | split: `editor_input.c` (router, see below), `editor_commit.c` (Phase 3) | editor |
| `repl_undo.c` | `editor_undo.c` | editor |
| `repl_clipboard.c` | `editor_clipboard.c` | editor |
| `repl_search.c` | `editor_search.c` | editor |
| `repl_inline_rename.c` | `editor_inline_rename.c` | editor |
| `repl_var_drag.c` | `editor_var_drag.c` | editor |
| `repl_autocomplete.c` | `editor_autocomplete.c` | editor |
| `repl_layout.c` | `editor_layout.c` | UI |
| `repl_code_panel_layout.c` | `editor_code_panel_layout.c` | UI |
| `repl_code_panel_document.c` | `editor_code_panel_document.c` | UI |
| `repl_actions.c` | split: REPL-side action dispatchers stay; UI-flavored bits move into `ui_action_dispatch.c` | both |
| `repl_camera_controls.c` | `scene_camera_controls.c` (or `viewport_camera_controls.c`) | scene/UI |

Each rename is a one-commit move with header redirects during the
transition. ARCHITECTURE.md, MODULES.md, and
`scripts/callgraph_file_groups.json` update in the same commits.

## Verification Per Phase

```bash
# Phase 1
make test && make test-stubs
# editor / ui state struct callers must use the new accessors:
grep -E 'repl_state_(editor_|clipboard|selection|autocomplete|search|status|variable_panel|variable_drag|help|profile_panel|viewport|pointer)' \
    *.c | grep -v 'imrepl_ctrl.c\|editor_state.c\|ui_state.c'
# should be 0

# Phase 2
grep -rn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# should be 0
grep -l '#include "editor_state.h"' repl_*.c | grep -v repl_state.c
# should be empty

# Phase 3
grep -E 'try_commit_(float_decl|for_loop|func_def|if_block|close_brace|var)' repl_commit*.c
# should only appear inside repl_compile.c (validators) or editor_commit.c
# (orchestration); never both in the same TU

# Phase 4
grep -rE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(' \
    ui_*.c
# should be 0

# Phase 5
ls editor_*.c
# matches the table above
ls repl_*.c | wc -l
# strictly fewer than before — only true REPL files remain
```

## Trade-offs

**Pros**

- Three-layer contract is enforceable mechanically (Makefile guards).
- A "headless REPL" / "embedded REPL in another editor" / "scripted
  REPL test harness" becomes feasible — drop `EditorState` and `UiState`
  entirely.
- `ReplState` shrinks dramatically; `repl_state_capture/restore` get
  cheaper; clipboard's 1.88 MB lines sidecar moves off the REPL state
  struct.
- UI input becomes a pure function `(snapshot, event) -> UiActionList`,
  testable without a live REPL.

**Cons**

- Phase 4 is the largest commit set and touches every input handler.
  Risk of behavior drift; mitigated by per-handler test coverage.
- Phase 5 invalidates external references to the old file names.
- `presentation` / `camera` placement is genuinely ambiguous (program
  state vs. session state). This plan keeps `presentation` on
  `ReplState` (it persists with the program via workspace `@cfg` lines)
  and moves camera-nav scratch to `EditorState` (mouse-driven). Revisit
  once a non-GUI REPL frontend exists, if ever.

## Out of Scope

- The further `sample.c → imrepl.c` rename
  (`push-architecture-refinement.md` R8). Mechanical and last.
- Color scheme / syntax keyword extraction (deferred sub-task of
  `editor-owns-text.md` Step 6).
- Dissolving `repl_core.c` (R10 in the refinement plan). Unrelated.

## Relationship To Other Plans

- This plan supersedes the deferred Phase C of
  `push-architecture-ui.md` — the `UiAction` work is folded in here as
  Phase 4.
- It builds on `feature/editor-owns-text.md` Steps 2–6 (the data-shape
  half) and `feature/gold-standard-state-ownership.md` (Stage 7's UI
  snapshot purity).
- `feature/push-architecture-refinement.md` R10 (dissolve
  `repl_core.c`) and R8 (`sample → imrepl` rename) remain independent.
