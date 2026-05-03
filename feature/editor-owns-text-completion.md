# Plan: Three-Layer Ownership Split (Editor / REPL / UI)

> **Direction note (2026-05-02).** Commits 1–11 of this plan have
> landed and remain correct. Phase 4's broad `UiAction` enum direction
> has been **superseded** by
> [`feature/editor-text-model-controller.md`](editor-text-model-controller.md),
> which sharpens the contract:
>
> ```text
> Editor = text model + controller (owns text, cursor, scroll, selection,
>          search, autocomplete, clipboard, undo, key/mouse for text docs,
>          commit orchestration).
> UI     = view + hit-test/render services (renders glyphs, highlights,
>          cursors; reports neutral UiHit results; owns no editor state).
> REPL   = validator/compiler for committed source. Pure: no editor state,
>          no UI state, no globals. Returns ReplCompiledChange or diagnostic.
> imrepl_ctrl = router between subsystems. Receives raw GLUT events;
>          dispatches to the owning subsystem based on focus / region;
>          builds per-frame snapshots; routes diagnostics back.
> ```
>
> Keep that doc as the authoritative contract for remaining Phase 4 / 5
> work. The frame-loop pseudocode, `UiAction` enumeration, and Phase 4
> migration plan inside *this* doc reflect the older direction; sections
> marked **(superseded)** are kept for historical reference but should
> not be implemented as written.
>
> The migration history (commits 1–11) recorded in *Implementation
> Status* and *Phase 1.2 Slice migration order* below remains accurate.
> Where remaining destinations have shifted under the new contract, the
> *Implementation Status* table for commit 12+ has been updated.

## Target Contract

The load-bearing definition is now in
[`editor-text-model-controller.md`](editor-text-model-controller.md).
The original three-line summary below is preserved because it is still
correct as a high-level shape — it just under-describes how input
routes through the system.

```
Editor owns editable text, cursor, selection, navigation, and undo
transactions. (Plus search, autocomplete, clipboard, and read-only
text documents — see editor-text-model-controller.md.)

REPL owns validation / compilation of committed editor text into
command / state changes. (Pure compiler; no editor state, no UI state.)

UI owns rendering and hit-testing. (Reports neutral UiHit results;
the owning subsystem interprets them. UI does *not* emit editor
actions or mutate state.)

imrepl_ctrl routes raw input to the owning subsystem and builds
per-frame snapshots. (Not a "dispatch gate" against a giant action
enum.)
```

## Why The Spirit Wasn't Realized (Pre-Commit-1)

This section is preserved as the *original problem statement* before
commits 1–11 landed. Items 1 and most of 2 are now resolved; items 3
and 4 are still in flight, with their solution path shifted to the
corrected contract in `editor-text-model-controller.md`.

### 1. Editor state lives inside REPL state ✅ resolved (commits 4–11)

`ReplRuntimeState` *used to* hold editor-owned slices:

```
editor_input          editor_buffer          editor_transformers
editor_highlights     editor_virtual_lines   selection
clipboard             autocomplete           search
variable_drag         code_panel.scroll      code_panel.scroll_follow_cursor
```

These all moved to `EditorState` in commits 4, 5, 6, 7, 9, 11.

UI render-time slices (`status`, `help.visible`, `variable_panel.visible`,
`profile_panel.mode`, `viewport`, `pointer`) moved to `UiState` in
commit 8. The remaining `code_panel` chrome (`cursor_visible`,
`blink_tick`, `cursor_px/py`, `panel_frac`, `resizing_panel`) and
`camera` are in commit 12 — with cursor blink revised to land on
`EditorState` (editor controls the cursor) rather than `UiState`.

### 2. REPL modules read and write editor text ⚠️ partial — commit 13 finishes

- `repl_command_store_*_with_line[s]` *still* writes
  `editor_buffer.lines[]` in lockstep with the cmd array — drops in
  commit 13.
- `repl_replay_annotations.c::replay_visible_text(cmd_idx)` still
  reaches into the editor buffer — converts to `EditorBufferView`
  parameter in commit 13.
- `repl_export.c` and `repl_parser.c` reparse paths same — same
  commit.

In the corrected contract, REPL is *given* text by the editor on
commit. It does not own a buffer; it does not write to one.

### 3. UI input handlers mutate state directly ⚠️ in flight — commit 15

`ui_panels_handle_code_panel_press` still calls `ui_color_picker_open`,
`repl_action_*`, `repl_clipboard_*`, etc. inline. The render path is
snapshot-only (Phase B of `push-architecture-ui.md` finished that).

The corrected fix is **not** the original "convert input handlers to
return a `UiAction` list that the controller dispatches" — it's
narrower: UI handlers compute a neutral `UiHit`, `imrepl_ctrl`
dispatches on `UiHit.kind` to the owning subsystem (editor, variable
panel, replay, scene/viewport), and the subsystem mutates its own
state directly. See `editor-text-model-controller.md` and Phase 4
below.

### 4. `repl_editor.c` is too broad to keep its name ⚠️ in flight — commit 16

It still combines: GLUT input router + UI bridge for
menus/search/help/rename/replay/camera + text editor model + commit
orchestration + direct coordination of undo/store/status/dirty/AC.
Phase 5 splits it three ways: GLUT registration → `imrepl_ctrl`,
text-document behavior → `editor_input.c`, commit orchestration →
`editor_commit.c`. See §5.0 below.

## Target State Shape

```c
/* The user's program — what the REPL parses, flattens, executes,
 * replays, exports. */
typedef struct {
    ReplDocumentState           document;
    ReplFlatProgramState        flat_program;
    ReplVariableState           variables;
    ReplReplayRuntimeState      replay;        /* see note below */
    ReplSceneRuntimeState       scenes;
    ReplImportExportState       import_export;
    ReplRenderState             render;        /* GL pipeline knobs */
    ReplPresentationState       presentation;  /* render toggles */
} ReplState;

/* Editable text, cursor, selection, navigation, undo, AND cursor-blink
 * — the *editor controls when the cursor is visible*, so blink is
 * editor session state, not UI chrome. UI just renders whatever
 * `cursor_visible` says.
 *
 * variable_drag stays here only as a transitional home; under
 * editor-text-model-controller.md it belongs to the variable-panel
 * peer subsystem since it has nothing to do with text-document
 * behavior. Scheduled to move out before Phase 5. */
typedef struct {
    ReplEditorInputState        input;        /* typing buffer + cursor */
    ReplEditorBuffer            buffer;       /* committed lines */
    ReplSelectionState          selection;
    ReplClipboardState          clipboard;    /* + lines[][] sidecar */
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    EditorScrollState           scroll;       /* doc-line + follow flag */
    EditorCursorBlinkState      cursor_blink; /* cursor_visible + blink_tick — corrected from UiState */
    ReplVariableDragState       variable_drag;/* transitional; moves to variable-panel subsystem */
    EditorUndoRing              undo;         /* transaction snapshots */
} EditorState;

/* Window chrome and viewport: the things that aren't part of "the
 * program" *or* "a text-document session." Render reads these; UI
 * input handlers may report changes via raw events into the
 * controller, which mutates here.
 *
 * cursor_px / cursor_py are NOT here as live state. They are computed
 * each frame from EditorState's logical cursor + layout and surfaced
 * either via UiRenderSnapshot or a per-frame Ui*Output struct.
 * Whichever home is chosen, the *controller* is the editor (the
 * editor's logical cursor is the source of truth). */
typedef struct {
    ReplViewportState           viewport;
    ReplPointerState            pointer;
    ReplStatusState             status;        /* transient message; controller-written */
    ReplHelpState               help;          /* visibility — until help becomes a read-only editor session, see below */
    ReplVariablePanelState      variable_panel;/* visibility — variable_panel subsystem owns its own model */
    ReplProfilePanelState       profile_panel; /* visibility */
    PanelDividerState           divider;       /* panel_frac + resizing_panel: window-divider geometry, not text */
    ReplCameraState             camera;        /* viewport pose; mouse handlers route to scene/viewport controller */
} UiState;

/* Peer subsystems carved out of the editor (per
 * editor-text-model-controller.md). Each owns its own state +
 * controller; the editor is unaware of them. */
typedef struct { /* variable-panel subsystem */
    int           visible;          /* moves out of UiState when this lands */
    ReplVariableDragState drag;     /* moves out of EditorState when this lands */
} VariablePanelSystem;

typedef struct { /* replay subsystem (already exists as ReplReplayRuntimeState
                  * + repl_replay.c — promoted to first-class peer; the
                  * field on ReplState above stays during transition) */
    /* mode, pc, speed, fade batches, etc. — see repl_replay.h */
} ReplaySystem;
```

### Read-only document seam

`editor-text-model-controller.md` introduces the "editor session"
abstraction: an editor session owns text + cursor + scroll + search
state, with a content provider and a `read_only` flag. The
code-editing session is the modifiable instance backed by
`editor_buffer` + REPL commit path. The help overlay becomes a
*read-only* session whose content provider returns the help text and
whose commit path is empty. That collapses today's `ui_help_overlay.c`
+ `ReplHelpState.scroll` / `tab_idx` into per-session editor state.
The `ReplHelpState.visible` flag stays on `UiState` as a chrome bit —
it's "is the help overlay shown?", not "what page is it on?".

### Autocomplete completion-provider seam

The editor doesn't know what variables exist. It calls a registered
completion provider for candidates, supplying current document/cursor
context. REPL/eval is one such provider; the editor's autocomplete
state (popup visibility, selected match, ghost text) stays in
`EditorState`. See `editor-text-model-controller.md` §"Editor / REPL
Completion Boundary".

`imrepl_ctrl` owns all three plus the peer subsystems as static
singletons. The frame loop **(corrected per
`editor-text-model-controller.md`)**:

```
input event (GLUT keyboard / mouse / wheel / motion)
  -> imrepl_ctrl receives raw event
  -> imrepl_ctrl asks UI for hit-test (UiHit { kind, line_idx,
       visual_row, char_idx, cmd_idx, item_idx, local_x, local_y })
  -> imrepl_ctrl dispatches based on UiHit.kind to the OWNING subsystem:
       UI_HIT_CODE_TEXT     -> editor_handle_*(editor, hit_or_event)
       UI_HIT_CODE_GUTTER   -> editor_handle_gutter(editor, hit)
       UI_HIT_HELP_PANEL    -> editor_help_session_handle_*(...)
       UI_HIT_COLOR_SWATCH  -> editor commit path (rewrites source text)
       UI_HIT_VARIABLE_SLIDER -> variable_panel_handle_*(...)
       UI_HIT_REPLAY_BUTTON -> replay_handle_*(...)
       UI_HIT_MENU_ITEM     -> imrepl_menu_route(...)
  -> editor / peer subsystem mutates its own state
  -> only commit paths call repl_compile()

editor commit path (the only thing that crosses into REPL):
  ReplCompiledChange change;
  char err[REPL_STATUS_TEXT_MAX];
  if (repl_compile(text, ctx, &change, err, sizeof(err)) != REPL_COMPILE_OK) {
      editor_record_diagnostic(err);    /* editor stores rejected state */
      /* imrepl_ctrl forwards the diagnostic to ui_state_status_set
       * — REPL doesn't, editor doesn't. */
      /* nothing applied; no undo entry */
  } else {
      editor_undo_begin_transaction();      /* captures BEFORE snapshot */
        editor_buffer_apply(&change);       /* writes editor text only */
        repl_apply_compiled_change(&change);/* writes ReplState only */
      editor_undo_commit_transaction();     /* atomic; restores both halves on undo */
      if (change.commit_message[0])
          imrepl_ctrl_set_status(change.commit_message);
  }

frame render
  -> imrepl_ctrl_build_scene_config(SceneRenderConfig out)
  -> imrepl_ctrl_build_ui_snapshot(UiRenderSnapshot out)
  -> scene/ui renderers read snapshots only
```

Most input flows do **not** cross subsystem boundaries. Mouse-wheel
over code text → editor scrolls. Ctrl+G → editor opens search. Tab →
editor accepts autocomplete. None of these need a structured
"action" — they're function calls into editor APIs once `imrepl_ctrl`
has dispatched on `UiHit.kind`.

### Settled questions: presentation / camera placement

- **`presentation`** persists with the program via workspace `@cfg`
  lines and is read by scene rendering. Stays on `ReplState`. Revisit
  only if a non-GUI REPL build target ever lands.
- **`camera`** is a viewport / session concern, not a code-editor
  concern. Mouse handlers emit `UI_ACTION_CAMERA_*`; controller
  dispatches into `UiState.camera` (or a future `ViewportState` slice
  if more 3D-viewport state accumulates). Scene reads camera pose
  through `SceneRenderConfig`. *Do not* put the camera in
  `EditorState`: `EditorState` owns the code-editor session, not the
  3D viewport. This decision pre-empts the open question; Phase 5's
  `repl_camera_controls.c` rename moves to `scene_camera_controls.c`
  for the transform-application half, while the input half is just
  another UI handler emitting actions.

### Status messages: REPL never writes status

```
REPL returns diagnostics.       It does not call set_status.
Editor commit returns commit messages.   It does not call set_status.
Controller writes UiState.status from those return values.
```

This rule is load-bearing. Existing `set_status()` calls inside
`repl_*` and `editor_*` modules must be replaced with diagnostic
return values; only `imrepl_ctrl_dispatch_action()` calls
`ui_state_status_set()`. Otherwise the same coupling survives the
split under a different name.

## Who Drives The Editor UI?

The editor drives text-document UI behavior.

`imrepl_ctrl` does **not** implement cursor movement, scrolling,
search, selection, autocomplete, clipboard, or undo. It only routes
raw input to the editor once focus / hit-testing says the event
belongs to a text document.

UI does not implement text behavior either. UI renders an editor
snapshot and returns neutral `UiHit` results.

The editor receives editor events plus hit-test context and mutates
`EditorState` internally. It exposes a compact render snapshot for UI
and a small commit boundary for REPL validation.

```text
UI              sees pixels and rectangles
imrepl_ctrl     sees focus, routes, and subsystem boundaries
editor          sees text-document intent and owns editor state transitions
REPL            sees proposed committed source text
```

## imrepl_ctrl Non-Goals

`imrepl_ctrl` is the application router and frame coordinator. It
should not mirror the editor API.

Avoid this shape:

```c
imrepl_ctrl_editor_search_next();
imrepl_ctrl_editor_scroll(int delta);
imrepl_ctrl_editor_move_cursor(int line, int col);
imrepl_ctrl_editor_accept_autocomplete();
imrepl_ctrl_editor_cut();
imrepl_ctrl_editor_paste();
```

That shape makes editor state bubble upward into `imrepl_ctrl` and
recreates a god-controller.

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

This is enforced by the `check-imrepl-not-editor-mirror` boundary
guard described in `MODULES.md`.

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

Those are editor internals. They should be visible outside the
editor only through coarse interfaces:

```c
void editor_handle_key(...);
void editor_handle_mouse(...);
void editor_handle_scroll(...);
void editor_build_view_snapshot(const EditorState *editor,
                                EditorViewSnapshot *out);
```

If `imrepl_ctrl` starts accumulating per-field editor helpers, the
split is drifting.

## Editor Services Boundary

The editor owns commit orchestration, but REPL semantics are
*injected as services*. This prevents the editor from rummaging
through REPL globals and prevents `imrepl_ctrl` from becoming the
editor implementation.

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

`imrepl_ctrl` wires this service table at startup. The editor calls
the services when it needs source semantics. Most editor operations
do not touch the services at all — they're editor-local
(scroll, cursor, search, autocomplete navigation, clipboard buffer).

This pattern lets the editor be unit-tested with a stub
`EditorServices` and removes the temptation to reach across into
REPL globals.

## REPL Public Contract After The Split

```c
/* Compile a single committed line. Returns a change-set big enough
 * to express block-structured commits (for / func / if / multi-line
 * declarations / paste / load) atomically. Pure: no editor state, no
 * UI state, no global mutation. */
typedef enum {
    REPL_COMPILED_NO_CHANGE,
    REPL_COMPILED_REPLACE_ONE,
    REPL_COMPILED_INSERT_ONE,
    REPL_COMPILED_INSERT_MANY,    /* for-loop body, func body, paste */
    REPL_COMPILED_DELETE_RANGE,
    REPL_COMPILED_LOAD_ALL        /* full reformat / load workspace */
} ReplCompiledChangeKind;

#define MAX_COMMIT_CMDS  MAX_COMMANDS  /* upper bound for LOAD_ALL */

typedef struct {
    ReplCompiledChangeKind kind;
    int   pos;                            /* insert/replace/delete index */
    int   count;                          /* applies to MANY / DELETE / LOAD */
    GLCmd cmds[MAX_COMMIT_CMDS];          /* parsed commands */
    char  text[MAX_COMMIT_CMDS][MAX_LINE_LEN]; /* canonical text per cmd */
    /* Optional commit-side message that the controller may forward to
     * UiState.status (e.g. "for-loop: i from 0 to 10"). Empty = no
     * status update. */
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

/* Apply a compiled change to REPL state. Mutates document + dependent
 * caches; does not touch editor state. The shape of the change tells
 * the apply path which underlying repl_command_store_* primitive to
 * call. */
void repl_apply_compiled_change(const ReplCompiledChange *change);

/* Lower-level primitives — used by repl_apply_compiled_change and by
 * import / load paths that already hold parsed cmds. */
void repl_apply_insert(int pos, const GLCmd *cmd);
void repl_apply_replace(int pos, const GLCmd *cmd);
void repl_apply_delete(int start, int count);
void repl_apply_load(const GLCmd *cmds, int count);

/* Programmatic queries the editor uses to validate / preview. */
int  repl_indent_chars_for(int pos);
int  repl_in_begin_block_at(int pos);
ReplCompileContext repl_context_at(int pos);
```

The single-line `ReplParsedLine` from the prior draft is *not*
sufficient: structured commits (for-loop with body, func-def with
body, multi-line var declarations, paste of N lines, full reformat,
workspace load) all need to express N>1 lines / commands as one
atomic change so the editor commit transaction can apply both halves
together. `ReplCompiledChange` carries that change-set.

REPL no longer takes text from a buffer it doesn't own. The editor
calls `repl_compile(typed_text, ctx, &change)`, then on success
applies the change in a single editor-undo transaction:

```c
editor_undo_begin_transaction();
  editor_buffer_apply(&change);          /* writes lines[][] only */
  repl_apply_compiled_change(&change);   /* writes cmds[] only */
editor_undo_commit_transaction();        /* atomic boundary */
```

## UiAction Enumeration (Phase 4 starting cut) **(superseded)**

> **Superseded by `editor-text-model-controller.md`.** The corrected
> direction does **not** introduce a giant `UiAction` enum. Most of
> the entries below are editor-internal (search, autocomplete,
> clipboard, undo, cursor, selection) and don't cross any subsystem
> boundary — they should be plain function calls into editor APIs
> once `imrepl_ctrl` has dispatched on a neutral `UiHit`.
>
> The remaining entries fall into three small buckets that *do* cross
> a boundary:
>
> ```text
> Editor → REPL: repl_compile(text, ctx, &change, err)
>                — for commit / reformat / paste of source / load.
>
> imrepl → REPL-side: save, load_file, load_example
>                — direct function calls; no enum needed.
>
> imrepl → peer subsystem: replay_toggle, replay_step,
>                          variable_panel_drag_*,
>                          camera_orbit / pan / zoom
>                — direct function calls into the peer; no enum.
> ```
>
> `UiHit` (a passive struct) replaces the dispatch enum. UI reports
> *where* the event landed; the owning subsystem decides *what it
> means*. See `editor-text-model-controller.md` §"UI Hit-Test Result,
> Not UI Ownership".
>
> The old enum below is kept for historical reference only.

```c
typedef enum {
    /* Text + cursor */
    UI_ACTION_INSERT_TEXT, UI_ACTION_DELETE_TEXT,
    UI_ACTION_MOVE_CURSOR, UI_ACTION_NAVIGATE_LINE,
    UI_ACTION_COMMIT_LINE, UI_ACTION_TOGGLE_INSERT_MODE,

    /* REPL transactional change-set actions — same dispatch path as
     * COMMIT_LINE: controller calls repl_compile and applies the
     * resulting ReplCompiledChange in one editor_undo transaction.
     * Reformat is *not* a UI-only action; it rewrites editor text and
     * parsed commands together. */
    UI_ACTION_REFORMAT,           /* whole-document reformat → LOAD_ALL change */

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

/* Input-handler signatures uniformly take a snapshot. The handler
 * does not reach into live state for hit-test geometry, document
 * counts, scroll position, virtual-line layout, or anything else —
 * everything it needs is on the snapshot or comes in via the event.
 * This is what prevents Phase 4 from preserving the old live-state
 * coupling under a new action-list wrapper. */
typedef struct {
    UiInputEventKind kind;        /* press / release / motion / key / wheel */
    int mx, my;
    int mods;
    int button;
    unsigned char key;
    int special_key;
    int wheel_delta;
} UiInputEvent;

void ui_panels_handle_code_panel_press(const UiRenderSnapshot *snap,
                                       UiInputEvent ev,
                                       UiActionList *out);
void ui_menu_bar_handle_press(const UiRenderSnapshot *snap,
                              UiInputEvent ev,
                              UiActionList *out);
void ui_color_picker_handle_press(const UiRenderSnapshot *snap,
                                  UiInputEvent ev,
                                  UiActionList *out);
/* …same shape for every UI input handler… */
```

`imrepl_ctrl_dispatch_action(const UiAction *)` is the single mutation
gate. It is the only function allowed to mutate `ReplState` /
`EditorState` / `UiState` from input.

---

## Implementation Status

Branch: `feature/editor-ownership-gap-cleanup`. Tracked against a
sequence inspired by but not strictly bound to the original
17-commit list. Commit 12+ destinations have been **revised** to
match `editor-text-model-controller.md`, and the post-Phase-A work
is split into smaller medium-sized commits so each one stays
buildable and reviewable.

The phases below group related commits. Each phase ends with the
audit / ratchet / test signal that drives toward zero or hard-guards
the boundary.

### Pre-existing scaffolding (Phase 0–1) — landed

| # | Commit | Status |
|---|---|---|
| 1 | tools: add editor ownership audit report | ✅ landed (2026-05-02) |
| 2 | docs: record baseline editor ownership audit counts | ✅ landed (2026-05-02) |
| 3 | refactor: add EditorState + UiState storage + compatibility forwarders | ✅ landed (2026-05-02) |
| 4 | refactor: migrate editor_buffer slice to EditorState | ✅ landed (2026-05-02) |
| 5 | refactor: migrate editor_input slice to EditorState | ✅ landed (2026-05-02) |
| 6 | refactor: migrate selection + clipboard slices to EditorState | ✅ landed (2026-05-02) |
| 7 | refactor: migrate search + autocomplete slices to EditorState | ✅ landed (2026-05-02) |
| 8 | refactor: migrate UI slices (status / help / variable_panel / profile_panel / viewport / pointer) to UiState | ✅ landed (2026-05-02) |
| 9 | refactor: migrate editor overlay snapshot lists (transformers / highlights / virtual_lines) + variable_drag to EditorState | ✅ landed (2026-05-02) |
| 10 | refactor: rename editor-input convenience getters to editor_* namespace | ✅ landed (2026-05-02) |
| 11 | refactor: code_panel slice split (scroll → EditorState) + ownership ratchet for transitional couplings | ✅ landed (2026-05-02) |

### Phase A — Finish state placement (ratchet to zero forwarders)

| # | Commit | Status |
|---|---|---|
| 12 | refactor: migrate code_panel chrome slice to UiState | ✅ landed (70ce58a, 2026-05-03) |
| 13 | refactor: migrate camera viewport pose to UiState | ✅ landed (c24e08e, 2026-05-03) |
| 14 | refactor: drop transitional repl_state forwarders for UI/editor slices | ✅ landed (354e39f, 2026-05-03) |

Phase A signal: `ui_forwarder_count` ratchet drops to 3 (the three
remaining matches are non-forwarder `ui_state_*` calls — documented
in the baseline). Every UI/editor slice is on its true owner.

### Phase B — Editor-buffer single writer + view parameters

| # | Commit | Status |
|---|---|---|
| 15 | refactor: add `EditorBufferView` and convert read-only consumers (annotations, export, debug) | ✅ landed (5f9ee5b, 2026-05-03) |
| 16 | refactor: thread `EditorBufferView` through executor / flatten / search / scenes / commit / core reparse helpers | ✅ landed (ba17419, 2026-05-03) |
| 17 | refactor: drop `repl_command_store_*_with_line[s]` APIs; rewrite call sites to two-step writes | ✅ landed (a034af3, 2026-05-03) |
| 18 | checks: promote `check-no-store-text-api` and `check-repl-no-direct-buffer-read` from informational audits to hard guards | ✅ landed (86ccd29, 2026-05-03) |

Phase B signal: `repl_command_store_*_with_line[s]` calls = 0 outside
tests; `editor_buffer_line` reads in non-`editor_*` files = 0 except
explicit `EditorBufferView` consumers.

### Phase C — `repl_compile` / `repl_apply` split

The smell this phase attacks: today's commit code mixes parse +
validation + editor-text mutation + command-store mutation + status
mutation + undo into one tangle. Phase C separates those concerns.
The naming convention is load-bearing — get it wrong here and a
later commit will quietly merge text mutation back into the REPL
side:

```text
ReplCompileResult repl_compile(...);
    Pure. No editor mutation. No command-store mutation. No status
    mutation. No undo entry. Returns ReplCompiledChange or diagnostic.

void repl_apply_compiled_change(const ReplCompiledChange *change);
    Mutates ReplState command store ONLY. Does not touch editor
    text. Does not touch status.

void editor_buffer_apply_compiled_change(const ReplCompiledChange *change);
    Mutates EditorState text ONLY. Does not touch ReplState. Does
    not touch status.
```

Both `_apply_*` halves take the same `ReplCompiledChange` so the
editor commit orchestration can drive them in lockstep inside one
undo transaction.

| # | Commit | Status |
|---|---|---|
| 19 | refactor: introduce `ReplCompiledChange` and pure `repl_compile` validators (parse + source-structure + var-decl validation; produces source-command changes, **not** flat program); migrate `try_commit_float_decl` and `try_assign_variable` first | ✅ landed (57763d0, 2026-05-03) |
| 20 | refactor: add `repl_apply_compiled_change` (writes ReplState only) + `editor_buffer_apply_compiled_change` (writes EditorState only); route float-decl + var-assign through them. Block-structured handlers (for-loop / func-def / if-block / close-brace) deferred to Phase D commits 26b–26e — they need cursor/mode fields on ReplCompiledChange that don't exist yet | ✅ landed (f191886, 2026-05-03) |
| 21 | refactor: introduce `editor_commit_apply_compiled_change` orchestration helper wrapping the three apply halves in lockstep | ✅ landed (34b3d57, 2026-05-03) |
| 22 | test: commit-transaction invariants (failed validation leaves buffer + store + status + undo untouched; success updates both atomically; reformat keeps buffer + store aligned) | ✅ landed (213b286, 2026-05-03) |
| 23 | refactor: preflight cmd-store capacity/ranges before any mutation (`repl_apply_can_apply_compiled_change`); editor_commit_apply_compiled_change rejects atomically on overflow with all three halves untouched | ✅ landed (1234c07, 2026-05-03) |

Phase C signals:

- `repl_compile()` is pure — no editor mutation, no command-store
  mutation, no status mutation, no undo entry. Compile produces a
  source-command change description; flattening still happens later.
- `repl_apply_compiled_change()` writes only `ReplState` command
  arrays. The function name and docstring make that explicit so a
  future commit can't quietly fold text mutation back in.
- `editor_buffer_apply_compiled_change()` writes only `EditorState`
  text. Same naming discipline.
- Every editor-text-changing path goes through one undo transaction
  that wraps the two `_apply_*` halves.
- Status messages flow through diagnostic return values from
  `repl_compile()` and commit-message return values from the editor
  commit path; `repl_compile` itself never calls `set_status()`. The
  controller / status layer renders the diagnostic after the
  transaction returns.

### Phase D — Carve `editor_input.c` / `editor_commit.c`

The original 4-commit plan compressed too much into commits 24–25.
Revised here based on review: split the structured-block migration
across multiple commits, keep service-table ownership tight, and
defer `UiHit` dispatch wiring to Phase E.

The naming convention for the service table is load-bearing in the
same way the apply-name discipline is in Phase C — the services
provide REPL semantics, not generic app callbacks:

```text
typedef struct {
    /* Build a ReplCompileContext from the current REPL+editor state. */
    ReplCompileContext (*context)(void *user);

    /* Pure compile entry; no mutation. */
    ReplCompileResult (*compile)(const char *text,
                                 const ReplCompileContext *ctx,
                                 ReplCompiledChange *out,
                                 char *err, int err_size,
                                 void *user);

    /* Applies the ReplState half of a successful change. */
    int  (*apply_repl_change)(const ReplCompiledChange *change, void *user);

    /* Replays the change's predef-var ops against the eval table. */
    void (*apply_predef_ops)(const ReplCompiledChange *change, void *user);

    void *user;
} EditorServices;
```

The editor half of the apply (`editor_buffer_apply_compiled_change`)
stays inside the editor — services do not carry an `apply` that
mutates editor text. That keeps the service surface to "REPL
semantics in" and prevents the table from drifting into a generic
backdoor.

The result struct for the editor commit orchestration uses
explicit valid flags rather than empty-string sentinels:

```text
typedef struct {
    int  consumed;             /* dispatcher recognized the input */
    int  mutated;              /* commit landed; state changed */
    int  capacity_failed;      /* preflight rejected on capacity */
    int  diagnostic_valid;     /* compile failure diagnostic */
    int  commit_message_valid; /* success message */
    char diagnostic[REPL_STATUS_TEXT_MAX];
    char commit_message[REPL_STATUS_TEXT_MAX];
} EditorCommitResult;
```

Undo capture inside the orchestration sits AFTER successful compile
+ preflight but BEFORE the first mutation — it is a transaction
boundary tied to the act of mutating, not to "successful commit"
which would risk capturing post-state.

| # | Commit | Status |
|---|---|---|
| 24 | refactor: introduce `EditorServices` table (compile / apply_repl_change / apply_predef_ops / context — not a generic callback bag); `imrepl_ctrl` wires services. Behaviour-preserving indirection only | pending |
| 25 | refactor: carve `editor_commit.c` shell with `editor_commit_current_input` returning `EditorCommitResult` (explicit `*_valid` flags, not empty-string sentinels). Undo capture moves into the orchestration AFTER successful compile + preflight, BEFORE first mutation. Migrated handlers (float-decl, var-assign) stop calling `set_status` on compile failure — diagnostic flows through `EditorCommitResult.diagnostic` | pending |
| 26a | refactor: carve `editor_input.c` shell with `editor_handle_key/mouse/scroll`. Move keyboard / mouse / scroll handlers that don't need structured-commit migration. Route `;`-key / Enter / `feed_line` through `editor_commit_current_input` for the already-migrated simple commit paths. Structured `try_commit_*` stay in place. `repl_editor.c` becomes a transitional shim | pending |
| 26b | refactor: introduce `EditorCommitPostEffects` + `EditorCommitPlan` (editor-side, NOT on `ReplCompiledChange`); migrate `try_commit_close_brace` first — simplest cursor / mode model — to prove the shape | pending |
| 26c | refactor: migrate `try_commit_if_block` to compile/apply through `EditorCommitPlan` | pending |
| 26d | refactor: migrate `try_commit_func_def` to compile/apply (handles leading-comment relocation; potentially the largest single migration) | pending |
| 26e | refactor: migrate `try_commit_for_loop` to compile/apply (one-liner-body branch + inline body parse) | pending |
| 27 | checks: promote `check-imrepl-not-editor-mirror` to hard guard. Verify `imrepl_ctrl` calls only coarse `editor_handle_*` APIs (no per-field editor wrappers). Real `UiHit.kind` dispatch deferred to Phase E once the type exists | pending |

Phase D signals:

- `EditorServices` is the only seam through which the editor calls
  REPL semantics. Editor code stops including `repl_compile.h` /
  `repl_apply.h` directly — only `editor_commit.c` and the service
  registration site (in `imrepl_ctrl`) do.
- `editor_commit_current_input` is the single transaction boundary
  for editor-text-mutating commits. Undo snapshot, preflight,
  predef-op cascade, editor-buffer apply, and cmd-store apply all
  fire from here in lockstep. On a compile / preflight failure
  none of those run and no undo entry is pushed.
- `EditorCommitResult` carries diagnostic / commit-message text
  with explicit `*_valid` flags. Empty-string-as-state is not a
  signal anywhere in the contract.
- Each structured-commit migration (close-brace → if-block →
  func-def → for-loop) is its own reviewable commit. Cursor /
  mode side-effect fields land with the first migration that needs
  them; later migrations reuse the same fields.
- During the migration, unmigrated structured handlers may still
  call `set_status` directly. The rule is: migrated handlers
  return diagnostics; legacy handlers stay on `set_status` until
  their migration commit lands.
- `imrepl_ctrl` shrinks to coarse `editor_handle_*` calls. The
  `check-imrepl-not-editor-mirror` guard catches per-field editor
  wrappers. Real `UiHit.kind` dispatch is Phase E's job; the
  router does not stub a placeholder enum.

### Phase E — UI returns `UiHit` instead of mutating

| # | Commit | Status |
|---|---|---|
| 28 | refactor: define `UiHit` / `UiHitKind`; convert `ui_panels` mouse handlers to compute and return hit | done |
| 29 | refactor: convert `ui_menu_bar`, `ui_color_picker`, `ui_variable_panel` mouse handlers to return `UiHit` | done |
| 30 | checks: introduce `check-ui-returns-hits-only` hard guard with ratchet-down baseline (8 residual mutator calls scheduled for Phase F+ cleanup) | done |

Phase E signal: `check-ui-returns-hits-only` is enforced; UI input
handlers compute hits and return — they don't call mutators.

### Phase F — Peer subsystems carved out

| # | Commit | Status |
|---|---|---|
| 31 | refactor: extract `variable_panel` peer subsystem shell (visibility flag + drag-state bytes moved off EditorState/UiState into VariablePanelState; legacy accessors forward) | done |
| 32 | refactor: route variable-panel hits to `variable_panel_handle_*` (editor / ui_variable_panel / imrepl_ctrl / repl_config / repl_var_drag use peer accessors; legacy editor_state_variable_drag / ui_state_variable_panel kept as forwarders) | done |
| 32b | checks: ratchet legacy variable_panel forwarder API uses (`editor_state_variable_drag*`, `ui_state_variable_panel*`, `repl_var_drag_*`) — baseline 87 (test fixtures only) | done |
| 33 | refactor: promote `replay` to peer subsystem shell (move `ReplReplayRuntimeState` off `ReplState` into replay_state.c; legacy repl_state_replay* accessors forward) | done |
| 34 | refactor: route replay hits to `replay_handle_*`; production callers read via `replay_state_view` / `replay_active` (UI renders snapshot only, imrepl_ctrl routes only) | done |
| 34b | checks: ratchet legacy `repl_state_replay*` forwarder API uses — baseline 37 (bench + test fixtures only) | done |

Phase F signal: variable panel and replay each own their state;
neither lives on EditorState or UiState. Hits route through
`variable_panel_handle_*` / `replay_handle_*` instead of through
the editor or ui state slices.

### Phase G — Read-only document seam + completion provider

| # | Commit | Status |
|---|---|---|
| 34c | refactor: narrow `replay_state` public surface (replay_pc / replay_src_line / replay_machine_state / replay_mode / replay_speed / replay_total_flat / replay_expand_args); migrate single-field readers; `replay_state_view()` reserved for the snapshot-build path | done |
| 35 | refactor: convert help overlay to `editor_help_session` (tab_idx/scroll moved off `ReplHelpState` into `EditorHelpSession` peer; UiRenderSnapshot.help_session feeds the renderer; `UiState.help.visible` stays as chrome flag) | done |
| 36 | refactor: introduce `EditorCompletionProvider`; `repl_autocomplete` registers a provider; editor owns popup state (already on EditorState) | done |

Phase G signal: editor sessions support both editable source
documents and read-only documents through the same scroll/search/
cursor model. Completion semantics live behind a registered
provider, not a global call.

### Phase H — Renames

| # | Commit | Status |
|---|---|---|
| 37 | rename: editor-owned modules (`repl_undo` → `editor_undo`, `repl_clipboard` → `editor_clipboard`, `repl_search` → `editor_search`, `repl_autocomplete` → `editor_autocomplete`, `repl_inline_rename` → `editor_inline_rename`); transitional redirect headers omitted because every caller migrated in the same commit | done |
| 38 | rename: peer-subsystem and layout modules (`repl_var_drag` → `variable_panel_drag`, `repl_replay` → `replay`, `repl_layout` → `ui_layout`, `repl_code_panel_layout` → `ui_code_panel_layout`, `repl_code_panel_document` → `editor_code_panel_document`); allowlist `ui_layout` / `ui_code_panel_layout` for facade-include + ui-no-repl-state-read checks (Phase I follow-up: pass `code_panel_layout` as a parameter so geometry helpers become pure) | done |

Phase H signal: file names match ownership. No `repl_*` files own
editor-session state; no `editor_*` files own replay state.

**Deferred from Phase H** (each blocked on a prerequisite, not in
the rename list):

- `repl_camera_controls.c/h` — final namespace depends on the
  eventual `scene_*` vs `viewport_*` split (whichever ends up
  owning the orbit/pan/zoom state). Forcing a rename now risks
  bouncing it twice.
- `repl_actions.c/h` — controller / app-action glue (menu activation,
  key shortcuts, config table) rather than REPL grammar. Right
  destination (`controller_actions` / `app_actions` / etc.) depends
  on how `imrepl_ctrl` itself gets re-namespaced when the app shell
  carves out from REPL.
- `repl_commit.c/h` — its **existence** is transitional. The
  architectural target is: REPL parses and returns a structured
  result; editor commits. Renaming would calcify a structure
  Phase H.5 dissolves.

### Phase H.5 — Dissolve repl_commit into editor_commit

The corrected M/V/C+compiler+router contract has the editor attempt
a commit and the REPL act as a pure parser/validator. Today
`repl_commit.c` still hosts `try_commit_*` dispatchers as thin
wrappers around `editor_compile_*` — they live on the REPL side and
pretend the REPL owns commit dispatch. Phase H.5 inverts this so the
editor truly drives, and the REPL only parses + reports errors.

| # | Commit | Status |
|---|---|---|
| 39 | refactor: move `try_commit_*` dispatchers + `apply_*_change` helpers + `repl_commit_func_decl_resume_*` editor-orchestration scratch from `repl_commit.c` into `editor_commit.c`; reduce `repl_commit.c` to a placeholder pending deletion in commit 41 | done |
| 40 | refactor: extend `ReplParseContext` with `err_buf`/`err_sz`; `repl_parser.c` set_status sites become `parser_emit_error_*` helpers that write to ctx err_buf when provided (fall back to set_status only when caller did not opt in); demo: editor_compile_for_loop's body parse surfaces the specific parser diagnostic. `repl_compile.c` already returned errors via err buffer (Phase C). Future callers migrate to opt in incrementally. | done |
| 41 | refactor: declarations moved from `repl_core_internal.h` into `editor_commit.h`; `repl_commit.c` deleted; `check-no-repl-commit` hard guard wired into `make check-state-ownership` so the file (and any new `try_commit_*` wrappers under the `repl_*` namespace) cannot reappear | done |

Phase H.5 signal: there is no `repl_commit` translation unit. The
editor is the sole entry point for committing input; REPL parse
errors flow back through return values, not side effects on
`set_status`.

### Phase I — Close the parser fallback + hard guards + docs

Phase H.5 commit 40 introduced the `ReplParseContext.err_buf` seam
but kept a `set_status()` fallback inside `parser_emit_error_v` so
non-opt-in callers wouldn't break. Phase I closes that loophole, then
runs the boundary-audit sweep and the docs refresh.

| # | Commit | Status |
|---|---|---|
| 42a | refactor: migrate the 6 remaining `repl_parser_parse_command_ctx` callers (`repl_core.c`, `repl_flatten.c`, `repl_replay_annotations.c`, `ui_color_picker.c`, `repl_export.c`, `repl_editor.c`) to provide an `err_buf` and decide explicitly — surface, log, or document a deliberate drop with an empty buffer | pending |
| 42b | refactor: strip the `set_status()` fallback from `parser_emit_error_v`; the helper writes to `ctx->err_buf` (or no-ops if absent). `repl_parser.c` no longer touches `set_status` | pending |
| 42c | checks: add `check-no-set-status-in-repl-parser` hard guard so the fallback can't return | pending |
| 42 | checks: promote remaining audits to hard guards (`check-editor-services-only`, `check-no-set-status-in-repl-or-editor`); remove the budget ratchet now that all transitional forwarders are zero. (`check-imrepl-not-editor-mirror` already lands in Phase D commit 27; `check-no-repl-commit` lands in Phase H.5 commit 41.) | pending |
| 43 | docs: refresh MODULES, ARCHITECTURE, CLAUDE, callgraph groups; mark `editor-ownership-gap-cleanup`, `editor-text-model-controller`, and `editor-owns-text-completion(-revised)` plans as landed | pending |

Phase I signal: the parser never calls `set_status`; every boundary
the plan articulates is enforced by a hard guard; the North Star
MODULES.md and the build checks describe the same shape.

### Conventions across the sequence

- **Each commit is buildable.** `make test && make test-stubs` runs
  green at every commit.
- **Behavior preservation first.** Phases A–C are pure restructuring
  with no UX delta. UX-visible regressions are tracked manually using
  the smoke checklist in `editor-ownership-gap-cleanup.md` and at the
  bottom of this document.
- **Ratchet-driven.** Each commit either drives the audit counter
  monotonically downward or promotes an audit to a hard guard.
- **Renames last.** All file renames batch in Phase H so review noise
  (rename diffs) stays out of behavior commits.
- **Out of scope:** `sample.c → imrepl.c` rename, `repl_core.c`
  dissolution, syntax-theme work — separate tracks per the doc's
  *Non-goals* section.

### Phase notes (revised commit destinations)

- **Phase A (commits 12–14, landed).** The original plan put cursor
  blink fields on `UiState`. The corrected contract puts them on
  `EditorState` because the editor is the cursor's controller —
  Phase A only moves the render-chrome bits (panel_frac,
  resizing_panel, cursor_px / cursor_py, plus the transitional
  `cursor_visible` / `blink_tick` fields that the editor will reclaim
  in a later commit) so the slice fits cleanly inside `UiState` for
  now. Camera viewport pose moved to `UiState` alongside the rest of
  the chrome.
- **Phase C.23 (preflight) landed inline.** Phase C originally
  closed at commit 22; review surfaced an atomicity gap where a
  cmd-store capacity failure could land after the predef-var
  cascade and editor-buffer write. Commit 23 added
  `repl_apply_can_apply_compiled_change` so
  `editor_commit_apply_compiled_change` rejects the change before
  any mutation. Failure is now atomic: 0 ⇒ no mutation; 1 ⇒ all
  three halves landed.
- **Phase D split.** The original 4-commit Phase D compressed too
  much into commits 24–25. The revised plan above splits the
  `editor_input` carve and structured-block migration across
  commits 26a–26e (close_brace → if_block → func_def → for_loop)
  and tightens the service-table surface so it carries REPL
  semantics only (`compile`, `apply_repl_change`,
  `apply_predef_ops`, `context`) — the editor half of the apply
  stays inside the editor. The `EditorCommitResult` uses explicit
  `*_valid` flags rather than empty-string sentinels.
- **Editor side-effects do NOT live on `ReplCompiledChange`.**
  Structured-block migrations need cursor / insert-mode / input-
  clear / pending-newline / func-decl-resume side-effects
  applied as part of the commit transaction, but those are
  editor behaviour. Putting them on `ReplCompiledChange` would
  let the REPL pipeline tests learn cursor mechanics and re-mix
  the split Phase C just enforced. Phase D commit 26b introduces
  a sibling editor-side struct:

  ```text
  typedef struct {
      int cursor_target;             /* desired edit_line; -1 = no change */
      int insert_mode_target;        /* -1 / 0 / 1 */
      int clear_input;               /* drop g_input + reset cursor_pos */
      int clear_pending_newline;
      int load_line_after_apply;     /* call load_line_to_input(cursor_target) */
      int func_decl_resume_advance;  /* delta to add to edit_line via apply_func_decl_resume */
      int end_type;                  /* CmdType label for status / resume */
  } EditorCommitPostEffects;

  typedef struct {
      ReplCompiledChange      change;
      EditorCommitPostEffects effects;
      char                    commit_message[REPL_STATUS_TEXT_MAX];
      int                     commit_message_valid;
  } EditorCommitPlan;
  ```

  The apply path becomes:
  - `editor_commit_apply_plan(plan)` — preflights `plan->change`,
    captures undo, applies the REPL change halves, then applies
    `plan->effects` (cursor / mode / input / load_line / func-decl-
    resume).
  - Pure-REPL handlers (float-decl, var-assign) keep returning
    `ReplCompiledChange` — they have no editor post-effects beyond
    "clear input + reset cursor" which the existing
    `editor_commit_current_input` covers.
  - Structured-block handlers (close_brace, if_block, func_def,
    for_loop) return `EditorCommitPlan`. Their compile functions
    live in editor_commit.c (or a sibling `editor_compile_*` file)
    because the post-effects are editor-owned.

  The invariant the rule preserves:

  ```text
  repl_compile_*  → ReplCompiledChange     (source-command level only)
  editor_compile_* → EditorCommitPlan      (REPL change + editor effects)
  apply path applies both sides in one transaction after preflight.
  ```

- **`g_func_decl_resume_delta` becomes editor post-effect data.**
  Today the global is a hidden coupling between compile-time
  reading and apply-time consuming. Phase D commit 26d folds it
  into `EditorCommitPostEffects.func_decl_resume_advance` so the
  apply path owns clearing it.
- **Undo capture ordering.** Inside the editor commit
  orchestration the undo snapshot fires AFTER successful compile
  + preflight but BEFORE the first mutation. Tying it to
  "successful commit" risks capturing post-state; tying it to the
  pre-mutation moment is the transaction boundary the rest of the
  shape needs.
- **Phases D–E reshape (UiAction → UiHit).** The original Phase 4
  was three commits of `UiAction` dispatch wiring. The corrected
  plan replaces the enum with a passive `UiHit` struct that UI
  returns; `imrepl_ctrl` routes on `UiHit.kind`. Phase D commit 27
  promotes `check-imrepl-not-editor-mirror` as a hard guard but
  does NOT stub a `UiHit` enum — Phase E introduces the real type
  and wires the dispatch. Variable panel and replay become peer
  subsystems (Phase F), not editor slices.
- **Phase G.** Adds the read-only document seam: help becomes an
  editor session pointed at a help-text content provider.
  `ReplHelpState.scroll` / `tab_idx` move into per-session editor
  state. Also lands the autocomplete completion-provider registration
  seam so the editor stops reaching into `repl_eval` for variable
  names.

## Phase 0 — Audits Before Moving Code

This phase lands before structural changes so every later phase has a
measurable exit criterion. The first goal is *not* zero — it is making
drift visible.

### 0.1 Add `scripts/audit_editor_ownership.sh`

✅ **Landed (commit 1).** Wired as `make audit-editor-ownership`.
Informational target only; promotion to a hard guard is deferred to
commit 17 (Phase 5). The script drops the doc-draft sentinel comment
and uses `bash` (matching the surrounding `scripts/check-*.sh` style).
Tolerated exceptions are skipped via `EDITOR_OWNERSHIP_TODO` markers
so known-knowns don't drown unknown drift.

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

✅ **Landed (commit 2).** Recorded against `make audit-editor-ownership`
output. Count = output line count per section; one line is one hit.

```text
Date:                                  2026-05-02
Branch:                                feature/editor-ownership-gap-cleanup
SHA at measurement (post-commit-1):    0595c42
repl_state editor/ui-like accessor hits:    1509
command store _with_line API hits:            46
REPL direct editor-buffer read hits:          36
UI live mutation hits:                        16
repl_editor.c include count:                  20
Known intentional exceptions:                  0
```

**Per-phase target.** Phase 1 (commits 3–7) drives the editor/ui-like
`repl_state_*` count toward zero outside `repl_state.c`, `editor_state.c`,
`ui_state.c`, and forwarder shims. Phase 2 (commit 8) drives the
`_with_line[s]` count to 0. Phase 2 (commit 9) drives REPL direct
editor-buffer reads to 0. Phase 4 (commits 13–14) drives UI live
mutation hits to 0. Phase 5 (commit 15) splits `repl_editor.c` so the
include-surface signal becomes structural rather than sized.

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

✅ **Landed (commit 3).** Initial scaffold uses just `editor_state.{c,h}`
and `ui_state.{c,h}`; the optional `_views.h` / `_owners.h` splits are
deferred until a slice has enough surface to warrant them. Each struct
holds a `_phase1_scaffold_placeholder` field that is removed when the
first real slice migrates into it (commit 4 for `EditorState`, commit 7
for `UiState`).

`repl_state.c` calls `editor_state_reset()` and `ui_state_reset()` from
`repl_state_reset_all()` so every test reset clears all three structs.
`ui_state_reset` is forward-declared rather than included, because
`check-controller-boundaries` forbids `repl_*.c` from depending on the
`ui_*` layer. The hookup migrates to the controller along with the
slices once Phase 1 commit 7 lands and `g_repl_state` no longer hosts
any UI-shaped state.

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
   ✅ **Landed (commit 4).** Storage moved from `g_repl_state.editor_buffer`
   to `g_editor_state.buffer`. New API: `editor_state_buffer / _mut` for
   the whole struct, `editor_buffer_line / set_line / count / set_count`
   for slice-level access. The legacy `repl_state_editor_buffer*`
   accessors are deleted entirely (Path A — no transitional forwarder
   cruft); 29 production files and 11 test files were migrated as one
   atomic commit. The `ReplEditorBuffer` typedef moved from
   `repl_state_views.h` to `editor_state.h` alongside its owning struct.
   `test_capture_restore_round_trip` in `tests/test_repl_state.c` gained
   a paired `editor_state_capture/restore` since the editor session is
   now its own snapshot domain. Audit deltas vs. commit 3 baseline:
   section-1 1509 → 1460 (−49); section-3 36 → 29 (−7, now reports
   only true text-line reads since `_count / _set_*` accessors are no
   longer in the regex's union).
2. `editor_input` (typing buffer / cursor / edit-line / insert mode /
   pending newline).
   ✅ **Landed (commit 5; convenience getter rename in commit 10).** `ReplEditorInputState` and
   `ReplEditorInputView` typedefs moved to `editor_state.h`; the field
   `editor_input` was removed from `ReplRuntimeState`; storage now
   lives at `g_editor_state.input`. New API: `editor_state_input` /
   `_mut` / `_reset`. The 17 callers of `repl_state_editor_input*` were
   migrated mechanically. The convenience getters' impls moved from
   `repl_state.c` to `editor_state.c`, reading `g_editor_state.input`
   directly; commit 10 then renamed the public surface from
   `repl_state_input_*` / `_cursor_pos*` / `_insert_mode*` /
   `_pending_newline_*` to `editor_input_*` / `editor_cursor_pos*` /
   `editor_insert_mode*` / `editor_pending_newline_*` so the names
   match their owning namespace (24 caller files migrated). The
   decls moved from `repl_state_owners.h` / `repl_state_views.h` to
   `editor_state.h`. `edit_line_idx` stays on `ReplDocumentState`
   (the canonical home); the input view builder forward-declares
   `repl_state_edit_line()` to populate the view's
   `edit_line_idx` field. Audit deltas: section-1 1460 → 1363 (−97);
   `repl_state.c` shrank from 970 → 872 lines.
3. `selection` + `clipboard`.
   ✅ **Landed (commit 6).** `ReplSelectionState` and `ReplClipboardState`
   typedefs moved to `editor_state.h`; the `selection` and `clipboard`
   fields were removed from `ReplRuntimeState`; storage now lives at
   `g_editor_state.{selection,clipboard}`. New API:
   `editor_state_selection / _mut / _clear / _anchor / _end_idx / _set`
   and `editor_state_clipboard / _mut / _clear / _cmds_mut / _count /
   _count_set`. The clipboard's parallel `lines[][]` text sidecar
   (1.88 MB) moves off `ReplRuntimeState` entirely. 7 caller files
   migrated mechanically. `repl_state.c` g_clipboard / g_sel_*
   macros removed. `repl_debug.c` runtime-state-layout dump dropped
   the `selection` and `clipboard` rows. Audit delta: section-1
   1363 → 1307 (−56).
4. `search` + `autocomplete`.
   ✅ **Landed (commit 7).** `ReplSearchState` and
   `ReplAutocompleteState` typedefs moved to `editor_state.h`; the
   `search` and `autocomplete` fields were removed from
   `ReplRuntimeState`; storage now lives at
   `g_editor_state.{search,autocomplete}`. New API:
   `editor_state_search / _mut / _clear` and
   `editor_state_autocomplete / _mut / _clear`. 13 caller files
   migrated mechanically. `repl_state_defaults.inc` no longer
   declares `.search` or `.autocomplete`; defaults moved into a
   shared `EDITOR_STATE_INITIAL` macro in `editor_state.c` so the
   live singleton and the defaults snapshot stay in sync.
   `repl_debug.c` runtime-state-layout dump dropped both rows.
   Audit delta: section-1 1307 → 1263 (−44); `repl_state.c` shrank
   from 872 → 799 lines.
5. transformer / highlight / virtual-line snapshot lists.
   ✅ **Landed (commit 9 — bundled with variable_drag).** Storage moves
   for four slices: `EditorTransformerList`, `EditorHighlightList`,
   `EditorVirtualLineList`, and `ReplVariableDragState` all migrated
   from `ReplRuntimeState` to `EditorState`. New canonical names:
   `editor_state_transformers / _clear / _append`,
   `editor_state_highlights / _clear / _append`,
   `editor_state_virtual_lines / _clear / _append`,
   `editor_state_variable_drag / _mut / _reset`. The
   `ReplVariableDragState` typedef moved from `repl_state_views.h`
   to `editor_state.h`. 10 caller files migrated mechanically.
   `repl_state.c`'s `g_drag_*` macros removed (no remaining users).
   `repl_state_defaults.inc` lost the `.variable_drag` block;
   defaults captured in `editor_state.c`'s `EDITOR_STATE_INITIAL`.
   `repl_debug.c` runtime-state-layout dump dropped the
   variable_drag row. Audit delta: section-1 1260 → 1209 (−51).
   `repl_state.c` 826 → 748 lines.
6. `code_panel` scroll + scroll-follow → `EditorState.scroll`.
   ✅ **Landed (commit 11).** `ReplCodePanelRuntimeState` shrunk by
   removing `scroll` and `scroll_follow_cursor`; a new
   `EditorScrollState` slice on `EditorState` owns them. New API:
   `editor_state_scroll / _mut`, `editor_scroll / _set`,
   `editor_scroll_follow_cursor / _set`. `UiRenderSnapshot` gained an
   `EditorScrollState scroll` field; `imrepl_ctrl` populates it from
   `editor_state_scroll()`. UI render code reads `snap->scroll.scroll`
   instead of `snap->code_panel.scroll`. 9 caller files migrated
   (mostly mechanically; the `+= / -= / ++ / --` patterns and
   local-var-pointer writes were rewritten by hand to use the
   getter/setter pair). `repl_state_defaults.inc` lost the scroll
   fields from its `.code_panel` block. `repl_state.c`'s `g_scroll` /
   `g_scroll_follow_cursor` macros removed. Audit delta: section-1
   1209 → 1194 (−15).

   Commit 11 also lands ownership-budget guardrails per the user's
   directive that transitional couplings must not become permanent.
   `scripts/check-editor-ownership-budget.sh` ratchets two counts
   strictly downward: (a) the body line count of UI-slice forwarders
   in `repl_state.c` (currently 21), and (b) whether `ui_state.h`
   still includes `repl_state_views.h` (currently 1). Both sites
   carry an `EDITOR_OWNERSHIP_TODO(phase-N)` marker pointing at the
   commit that finally retires the coupling. The check is wired into
   the `check-state-ownership` aggregate and `make check`.
7. `status` / `help` / `profile_panel` / `variable_panel` →
   `UiState`.
8. `viewport` / `pointer` → `UiState`.
   ✅ **Landed (commit 8 — slices 7+8 batched).** `ReplStatusState`,
   `ReplHelpState`, `ReplVariablePanelState`, `ReplProfilePanelState`,
   `ReplViewportState`, `ReplPointerState` storage moved from
   `ReplRuntimeState` to `UiState`. New canonical API:
   `ui_state_status / _mut / _set / _clear / _tick`,
   `ui_state_help / _mut / _reset`, `ui_state_variable_panel / _mut`,
   `ui_state_profile_panel / _mut`,
   `ui_state_viewport / _mut / _set_size`,
   `ui_state_pointer / _mut / _set / _set_pos / _set_button`.

   Naming asymmetry vs. EditorState: the legacy `repl_state_*`
   accessors stay alive as one-line forwarders defined in
   `repl_state.c` (forward-declaring the `ui_state_*` symbols)
   because `check-controller-boundaries` forbids most `repl_*.c`
   callers from including `ui_state.h`. Defining the forwarders on
   the repl_state side rather than ui_state.c also keeps
   `check-state-boundaries` happy (the rule "no
   `repl_state_*_mut` from `ui_*.c`" stays enforced). The
   forwarders go away in Phase 4 once `UiAction` eliminates the
   direct-mutation call sites.

   `repl_state_views.h` keeps the `Repl*State` typedefs for now
   (they're shared with `ui_snapshot.h`); `ui_state.h` includes
   `repl_state_views.h` to embed them as fields. That requires
   adding `ui_state.h` to the
   `scripts/allowlists/facade-includes-in-views.txt` allowlist —
   cleared away in Phase 5 when typedefs migrate to their owning
   state header.

   `test_capture_restore_round_trip` in `tests/test_repl_state.c`
   now also captures and restores `UiState` so the round-trip
   asserts against state that no longer lives on
   `ReplRuntimeState`. `repl_state_defaults.inc` lost its `.help`,
   `.variable_panel`, `.profile_panel`, `.status`, `.pointer`,
   `.viewport` blocks; defaults moved to
   `UI_STATE_INITIAL` in `ui_state.c`. `repl_debug.c`'s
   runtime-state-layout dump dropped six rows. Audit delta:
   section-1 1263 → 1260 (−3 only — the forwarders preserve the
   matching name shape; the architectural prize is the storage
   move). `repl_state.c` 799 → 826 (+27 lines net: impls moved out,
   24 forwarders moved in). `ui_state.c` 24 → 133.
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
/* Compiler side (REPL): returns a ReplCompiledChange so block
 * commits, paste, and reformat can flow through the same gate. */
ReplCompileResult repl_compile(const char *line,
                               const ReplCompileContext *ctx,
                               ReplCompiledChange *out,
                               char *err_msg, int err_size);

/* Apply side (REPL) — applies a compiled change atomically; never
 * mutates editor or UI state. */
void repl_apply_compiled_change(const ReplCompiledChange *change);

/* Lower-level primitives used by repl_apply_compiled_change and by
 * import / load paths that already hold parsed cmds. */
void repl_apply_insert(int pos, const GLCmd *cmd);
void repl_apply_replace(int pos, const GLCmd *cmd);
void repl_apply_delete(int start, int count);
void repl_apply_load(const GLCmd *cmds, int count);

/* Editor side: orchestrates a single transaction. Captures a BEFORE
 * snapshot, applies both the editor-buffer change and the REPL apply
 * atomically, and only then commits the undo entry. On compile error
 * neither half is touched and the typed input is preserved with a
 * rejected flag. */
typedef struct {
    int  applied;             /* 1 if change was applied, 0 if rejected */
    char status_message[REPL_STATUS_TEXT_MAX]; /* for controller to forward */
} EditorCommitResult;

EditorCommitResult editor_commit_current_input(EditorState *editor,
                                               ReplState *repl,
                                               int enter_mode);
```

### 3.4 Split `repl_commit.c`

1. Pure validators move into `repl_compile.c`:
   `try_compile_float_decl`, `try_compile_for_loop`,
   `try_compile_func_def`, `try_compile_if_block`,
   `try_compile_close_brace`, `try_compile_var_assign`. Each fills a
   `ReplCompiledChange` and returns an error code; no mutations.
2. Orchestration moves to `editor_commit.c`:
   ```c
   editor_undo_begin_transaction();
     editor_buffer_apply(&change);          /* lines[][] only */
     repl_apply_compiled_change(&change);   /* cmds[] only */
   editor_undo_commit_transaction();        /* captures BEFORE snapshot */
   ```
3. `repl_editor.c` callers switch to `editor_commit_*` entry points.

### 3.5 Status messages flow through return values, not `set_status`

REPL validators do **not** call `set_status`. They write into the
`err_msg` out-parameter (compile errors) or the
`ReplCompiledChange.commit_message` field (success messages like
"for-loop: i from 0 to 10"). `editor_commit_*` returns
`EditorCommitResult.status_message`. Only
`imrepl_ctrl_dispatch_action()` calls `ui_state_status_set()`. Audit:

```sh
grep -RIn 'set_status\|ui_state_status_set' repl_*.c editor_*.c \
  | grep -v ' imrepl_ctrl\.c:'
# expected: 0 (status writes are controller-only)
```

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

## Phase 4 — Route input to the owning subsystem (corrected)

> Replaces the original Phase 4 `UiAction` migration. The corrected
> goal: UI is a view + hit-test layer; `imrepl_ctrl` routes raw input
> to the editor or to a peer subsystem; only commit paths call into
> REPL. Per `editor-text-model-controller.md`.

The work splits into four kinds of cleanup, each smaller than a
"wave" of the old plan:

### 4.1 Replace inline mutation with neutral `UiHit` reporting

Today `ui_panels_handle_code_panel_press` calls `ui_color_picker_open`,
`repl_action_*`, etc. directly. Those handlers should:

1. Compute a `UiHit` (kind + line/char/cmd_idx + local_x/local_y).
2. Hand the hit to `imrepl_ctrl`.
3. Stop mutating editor / REPL / peer state directly.

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
    int       line_idx;
    int       visual_row;
    int       char_idx;
    int       cmd_idx;
    int       item_idx;
    float     local_x;
    float     local_y;
} UiHit;

UiHit ui_panels_hit_test(const UiRenderSnapshot *snap, int mx, int my);
```

UI files keep all their layout/measure/render code. They lose
`ui_color_picker_open` / `repl_action_*` / `repl_clipboard_*` calls.

### 4.2 Move text-document input handling into the editor

GLUT keyboard / mouse / wheel callbacks already exist in
`repl_editor.c` (renamed `editor_input.c` in Phase 5.0). Their job
under the corrected contract:

- Receive raw events from `imrepl_ctrl`.
- Mutate `EditorState` directly: cursor, selection, scroll, search
  query, autocomplete popup, clipboard buffer, undo ring.
- Reach the commit path (`editor_commit_current_input`) when the user
  presses `;` / Enter; *that* is where `repl_compile` is called.

No `UiAction` enum, no dispatch table. The editor *is* the controller
for text-document behavior.

### 4.3 Carve out peer subsystems

`variable_panel`, `replay`, `camera/viewport` are not part of the
editor. Each gets its own input entry point:

```c
void variable_panel_handle_drag_begin(VariablePanelSystem *vp, int var_idx, int mx);
void variable_panel_handle_drag_motion(VariablePanelSystem *vp, int mx);
void variable_panel_handle_drag_end(VariablePanelSystem *vp);

void replay_handle_toggle_play_pause(ReplaySystem *rp);
void replay_handle_step(ReplaySystem *rp, int direction);

void scene_camera_handle_orbit(float dx, float dy);
void scene_camera_handle_pan(float dx, float dy);
void scene_camera_handle_zoom(float dz);
```

`imrepl_ctrl` dispatches to these based on `UiHit.kind` for mouse
events and on focus / mode for keyboard events. Color picker (when it
rewrites source text) is the one peer that crosses back into the
editor commit path — it's the only true "action" that needs the
compile-and-apply transaction.

### 4.4 Tighten UI guards

After 4.1–4.3 land, two guards strengthen the contract:

- `check-ui-returns-hits-only` (replaces the original
  `check-ui-emits-actions-only`) — `ui_*.c` input helpers do not call
  `repl_action_*`, `repl_command_store_*`, `editor_*_mut*`,
  `repl_state_*_mut*`, or peer-subsystem mutators directly. They
  compute a `UiHit` and return it.
- `check-imrepl-not-editor-mirror` (new) — `imrepl_ctrl` must not
  accumulate one wrapper per editor operation
  (`imrepl_ctrl_editor_search_next`,
  `imrepl_ctrl_editor_scroll`, `imrepl_ctrl_editor_move_cursor`, …).
  New editor behavior belongs behind `editor_handle_*` /
  `editor_*` APIs.

### Phase 4 checklist (corrected)

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

### Verification

```sh
make test && make test-stubs
make check-ui-no-repl-state-read
make check-ui-renderer-takes-view
make check-ui-returns-hits-only
make check-imrepl-not-editor-mirror

grep -rE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(|editor_[a-z_]+_(set|mut|begin|append|clear)' \
    ui_*.c
# expected: 0 by end of phase (UI emits hits, doesn't mutate)

grep -RInE 'imrepl_ctrl_editor_(search|scroll|move|autocomplete|cut|paste|undo|redo)' . \
    --include='*.c' --include='*.h'
# expected: 0 (no editor-API mirrors on imrepl_ctrl)
```

Manual smoke required (see Verification Matrix).

---

## Phase 5 — Rename to match ownership

Lands only after behavior + ownership have already moved. Renames are
mechanical with redirect headers during transition.

### 5.1 Rename checklist

Updated for the corrected contract. `ui_action_dispatch.c` is gone;
the controller routes via `UiHit` directly. `ui_help_overlay.c`
collapses into a read-only editor session.

| Old | New | Owner |
|---|---|---|
| `repl_editor.c` | three-way split (see below): `editor_input.c` + `editor_commit.c` + lifted-into-`imrepl_ctrl.c` | editor / controller |
| `repl_undo.c` | `editor_undo.c` | editor |
| `repl_clipboard.c` | `editor_clipboard.c` | editor |
| `repl_search.c` | `editor_search.c` | editor |
| `repl_inline_rename.c` | `editor_inline_rename.c` | editor |
| `repl_var_drag.c` | `variable_panel_drag.c` (peer subsystem, NOT editor — was the wrong destination in the prior plan) | variable-panel subsystem |
| `repl_autocomplete.c` | `editor_autocomplete.c`; gains a `EditorCompletionProvider` registration seam so the editor stops reaching into `repl_eval` directly | editor |
| `repl_layout.c` | `ui_layout.c` | UI |
| `repl_code_panel_layout.c` | `ui_code_panel_layout.c` (pure wrap iterator) | UI |
| `repl_code_panel_document.c` | `editor_code_panel_document.c` (owns scroll / hit-test, editor-state-shaped) | editor |
| `repl_actions.c` | split: menu activation routing → `imrepl_actions.c`; editor actions → `editor_actions.c`; REPL-side stays in repl_* | mixed |
| `repl_camera_controls.c` | `scene_camera_controls.c` (transform application reads `UiState.camera`); the mouse-input half is a `imrepl_ctrl` route into `scene_camera_handle_*` | scene/UI |
| `ui_help_overlay.c` | `editor_help_session.c` — a read-only editor session whose content provider returns help text. `ReplHelpState.scroll` and `tab_idx` move out of `UiState` into per-session editor state. `ReplHelpState.visible` stays on `UiState` as a chrome bit. | editor |

### 5.0 The `repl_editor.c` three-way split

Today `repl_editor.c` mixes three responsibilities. Each goes to a
different home — corrected for the M/V/C+router contract:

```text
repl_editor.c (today)
  GLUT key/mouse callback registration             -> imrepl_ctrl.c
   + cross-subsystem routing                          (GLUT events become raw input;
                                                      imrepl_ctrl asks UI for UiHit;
                                                      dispatches to the owning subsystem
                                                      based on UiHit.kind / focus)

  editor-side text-document behavior                -> editor_input.c
   (cursor moves, insert text, delete text,           (the text-document model/controller —
    navigate line, toggle insert mode, scroll,        receives raw events from imrepl_ctrl
    search-key handling, autocomplete navigation,     once dispatched here, mutates
    clipboard cut/copy/paste keys, undo/redo keys)    EditorState directly. It IS the editor.
                                                      No GLUT registration, no UiAction enum.)

  commit/navigation orchestration                   -> editor_commit.c
   (compile + undo + buffer + apply transaction;       (Phase 3 work)
    rejected-parse rollback;
    commit-before-navigation)
```

`editor_input.c` after the split is the editor's text-document
controller. It receives raw key/mouse events that `imrepl_ctrl` has
routed to it (via `UiHit.kind == UI_HIT_CODE_TEXT` for mouse, or via
focus state for keyboard). It mutates `EditorState`. It does **not**
register GLUT callbacks; that's `imrepl_ctrl`'s job. It does **not**
go through a `UiAction` dispatch table — the dispatch happens at the
`imrepl_ctrl` boundary, not inside the editor.

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

## Suggested Branch / Commit Breakdown **(historical — see Implementation Status above for current sequence)**

Branch name:

```text
feature/editor-ownership-gap-cleanup
```

The original 17-commit list below was the *initial* plan. Commits 1–11
matched it; commit 12+ has been retargeted under the corrected
contract (see *Implementation Status*). The list is preserved for
historical reference.

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
12. refactor: introduce UiAction dispatch gate            (REPLACED — UiHit routing instead)
13. refactor: migrate keyboard input handlers to UiAction (REPLACED — direct editor calls)
14. refactor: migrate mouse/menu/color/camera input handlers to UiAction (REPLACED — direct peer calls)
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
make check-ui-returns-hits-only
make check-imrepl-not-editor-mirror
```

### Boundary verification greps

These spot-check the boundaries the corrected contract enforces:

```sh
# REPL must not own editor text mutation.
grep -RIn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# expected after Phase 2: 0

# REPL readers that need source text take explicit EditorBufferView.
grep -RIn 'editor_buffer_line' repl_*.c repl_*.h
# expected after Phase 2: only explicit view consumers / no global reach-through

# UI hit-test/render files should not mutate editor/REPL/peer state.
grep -RInE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(|editor_.*_mut\(' \
    ui_*.c ui_*.h
# expected after Phase 4: 0 or intentional allowlisted exceptions

# imrepl_ctrl should not mirror editor internals.
grep -RInE 'imrepl_ctrl_editor_(search|scroll|move|autocomplete|cut|paste|undo|redo)' . \
    --include='*.c' --include='*.h'
# expected: 0
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

The split is complete when:

```text
 1. ReplState contains program state only.
 2. EditorState contains text-document model/controller state
    (text + cursor + scroll + selection + search + autocomplete +
    clipboard + undo + cursor_blink).
 3. UiState contains transient UI / session chrome and viewport
    state only (viewport + pointer + status TTL + visibility +
    panel divider + camera pose).
 4. UI renderers consume snapshots only.
 5. UI hit-test code reports hits; it does not own editor behavior.
 6. Search, scroll, selection, autocomplete, and undo are
    editor-owned.
 7. Only source/program mutations call repl_compile().
 8. repl_command_store mutates commands only.
 9. editor_buffer mutates text only.
10. Undo restores editor text and REPL command state together.
11. Variable panel and replay remain separate subsystems with their
    own state + controllers.
12. imrepl_ctrl routes to coarse owner APIs (editor_handle_*,
    variable_panel_handle_*, replay_handle_*, scene_camera_handle_*)
    instead of mirroring editor internals.
13. ui_help_overlay is a read-only editor session backed by a content
    provider; help scroll / search live in editor session state, help
    visibility stays on UiState.
14. Autocomplete reaches the variable / GL-name table via a
    registered EditorCompletionProvider, not by reaching into
    repl_eval directly.
15. MODULES.md, editor-text-model-controller.md, and ARCHITECTURE.md
    describe the same ownership boundaries enforced by Makefile
    checks (`check-ui-returns-hits-only`,
    `check-imrepl-not-editor-mirror`,
    `check-editor-ownership-budget`, etc.).
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

- **Authoritative current contract:**
  [`feature/editor-text-model-controller.md`](editor-text-model-controller.md)
  — defines the M/V/C+compiler+router framing. Read first.
- **Sibling revision:**
  [`feature/editor-owns-text-completion-revised.md`](editor-owns-text-completion-revised.md)
  — a cleaner, history-light version of *this* doc. The
  architectural sections of this file (Target Contract, Who Drives,
  imrepl_ctrl Non-Goals, Avoid Bubbling, Editor Services Boundary,
  Phase 4 corrected) are kept synchronized with that sibling. The
  detailed migration history (Implementation Status table, per-slice
  Phase 0/1 notes with audit deltas) lives only here.
- **Supersedes** the deferred Phase C of
  `feature/push-architecture-ui.md` — the input-handler work is folded
  in here as Phase 4 (now framed as `UiHit` routing rather than a
  `UiAction` enum).
- **Builds on** `feature/editor-owns-text.md` Steps 2–6 (data shape) and
  `feature/gold-standard-state-ownership.md` Stage 7 (UI snapshot
  purity).
- **Companion** to `feature/editor-ownership-gap-cleanup.md` — that doc
  is the audit/landing-sequence checklist; the contract lives in
  `editor-text-model-controller.md` plus the architectural sections
  of this doc. Read all three together.
- **Reflected in:**
  [`feature/modules-editor-view-update.md`](modules-editor-view-update.md)
  — the focused MODULES.md update plan applied via the prior commit.
- **Independent** of `push-architecture-refinement.md` R10 (dissolve
  `repl_core.c`) and R8 (`sample → imrepl`).
