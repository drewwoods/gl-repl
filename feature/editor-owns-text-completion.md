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

/* Editable text, cursor, selection, navigation, undo. The
 * code-editor session — not the 3D viewport. */
typedef struct {
    ReplEditorInputState        input;        /* typing buffer + cursor */
    ReplEditorBuffer            buffer;       /* committed lines */
    ReplSelectionState          selection;
    ReplClipboardState          clipboard;    /* + lines[][] sidecar */
    ReplSearchState             search;
    ReplAutocompleteState       autocomplete;
    ReplVariableDragState       variable_drag;
    EditorScrollState           scroll;       /* extracted from code_panel */
    EditorUndoRing              undo;         /* transaction snapshots */
} EditorState;

/* Render-time scratch + chrome visibility + 3D-viewport session state.
 * Nothing here is part of "the program" or "the code-editor session."
 * Camera pose lives here (or in a future ViewportState slice) — not
 * in EditorState — because the camera is a viewport concern. */
typedef struct {
    ReplViewportState           viewport;
    ReplPointerState            pointer;
    ReplStatusState             status;        /* transient message; controller-written */
    ReplHelpState               help;          /* visibility */
    ReplVariablePanelState      variable_panel;/* visibility */
    ReplProfilePanelState       profile_panel; /* visibility */
    EditorCursorBlinkState      cursor_blink;  /* render-only */
    ReplCameraState             camera;        /* pose; mouse-driven via UI_ACTION_CAMERA_* */
    /* Per-frame snapshots populated by the controller, consumed by
     * renderers (already pointer-shaped on UiRenderSnapshot). */
} UiState;
```

`imrepl_ctrl` owns all three as static singletons. The frame loop:

```
input event
  -> ui_*_input_handler(const UiRenderSnapshot *snap,
                        UiInputEvent ev,
                        UiActionList *out)
  -> imrepl_ctrl_dispatch(action_list)
       -> on commit / reformat / paste / cut / load (text-changing action):
            ReplCompiledChange change;
            char err[REPL_STATUS_TEXT_MAX];
            if (repl_compile(text, ctx, &change, err, sizeof(err)) != REPL_COMPILE_OK) {
                ui_state_status_set(err);             /* controller writes status */
                editor_input_mark_rejected();         /* keep typed text + flag */
                /* nothing applied; no undo entry */
            } else {
                editor_undo_begin_transaction();      /* captures BEFORE snapshot */
                  editor_buffer_apply(&change);       /* writes editor text only */
                  repl_apply_compiled_change(&change);/* writes ReplState only */
                editor_undo_commit_transaction();     /* atomic; restores both halves on undo */
                if (commit_msg) ui_state_status_set(commit_msg);
            }
       -> on REPL-side actions (save / replay / load):
            mutates ReplState; controller writes any UI status
       -> on editor-side actions (cursor / selection / clipboard /
          search / autocomplete / scroll):
            mutates EditorState only
       -> on UI-side actions (visibility / viewport / camera /
          status / panel modes):
            mutates UiState only

frame render
  -> imrepl_ctrl_build_scene_config(SceneRenderConfig out)
  -> imrepl_ctrl_build_ui_snapshot(UiRenderSnapshot out)
  -> scene/ui renderers read snapshots only
```

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

## UiAction Enumeration (Phase 4 starting cut)

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

Branch: `feature/editor-ownership-gap-cleanup`. Tracked against the
17-commit sequence in *Suggested Branch / Commit Breakdown* below.

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
| 10 | refactor: code_panel slice split (scroll → EditorState; chrome → UiState) + camera placement | next |
| 11–17 | (store text drop → compile gate → UiAction → renames → hard guards) | pending |

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
   ✅ **Landed (commit 5).** `ReplEditorInputState` and
   `ReplEditorInputView` typedefs moved to `editor_state.h`; the field
   `editor_input` was removed from `ReplRuntimeState`; storage now
   lives at `g_editor_state.input`. New API: `editor_state_input` /
   `_mut` / `_reset`. The 17 callers of `repl_state_editor_input*` were
   migrated mechanically. The convenience getters
   (`repl_state_input_text` / `_cursor_pos` / `_insert_mode` /
   `_pending_newline_*` etc.) keep their public names but their impls
   moved from `repl_state.c` to `editor_state.c`, where they read
   `g_editor_state.input` directly. `edit_line_idx` stays on
   `ReplDocumentState` (the canonical home); the input view builder
   forward-declares `repl_state_edit_line()` to populate the view's
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
| `repl_editor.c` | three-way split (see below): `editor_input.c` + `editor_commit.c` + lifted-into-`imrepl_ctrl.c` | editor / controller |
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
| `repl_camera_controls.c` | `scene_camera_controls.c` (transform application reads `UiState.camera`); the mouse-input half is just a regular UI handler emitting `UI_ACTION_CAMERA_*` | scene/UI |

### 5.0 The `repl_editor.c` three-way split

Today `repl_editor.c` mixes three responsibilities. Each goes to a
different home — the new `editor_input.c` is **not** a GLUT router:

```text
repl_editor.c (today)
  GLUT key/mouse callbacks + cross-layer routing  -> imrepl_ctrl.c
                                                     (GLUT events become UiInputEvent;
                                                      UI handlers emit UiAction;
                                                      imrepl_ctrl_dispatch_action mutates)

  editor-action application (cursor moves,         -> editor_input.c
   insert text, delete text, navigate line,           (an editor-action *applier* / reducer
   toggle insert mode)                                that mutates EditorState given a
                                                      cursor / text / navigation action;
                                                      no GLUT, no UI snapshot reads)

  commit/navigation orchestration                  -> editor_commit.c
   (compile + undo + buffer + apply transaction;      (already extracted in Phase 3)
    rejected-parse rollback;
    commit-before-navigation)
```

`editor_input.c` after the split is the editor's text-state reducer.
It receives `UiAction`s from the dispatch gate and updates
`EditorState` (cursor, selection, scroll, etc.). It does **not**
register GLUT callbacks; that's the controller's job.

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
