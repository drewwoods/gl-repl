# Plan: Editor Ownership Gap Audit and Cleanup Companion

## Purpose

This document is an implementation companion to
`feature/editor-owns-text-completion.md`, not a competing plan.

`feature/editor-owns-text-completion.md` is the architectural plan. It defines
the target contract:

```text
Editor owns editable text, cursor, selection, navigation, and undo transactions.
REPL owns validation / compilation of committed editor text into command / state changes.
UI owns rendering and hit-testing, and emits editor actions rather than mutating state directly.
```

This document adds the missing operational layer:

```text
What are the remaining gaps?
How do we measure them?
What order should the cleanup land in?
What checks prevent regression?
What tests prove the split is real?
```

In short:

```text
editor-owns-text-completion.md = desired architecture and phase structure
editor-ownership-gap-cleanup.md = audit checklist, landing sequence, and verification gates
```

---

## How This Resonates With `editor-owns-text-completion.md`

The existing completion plan is stronger than the first version of this doc in
two important ways:

1. It explicitly splits state into **three** owners: `ReplState`,
   `EditorState`, and `UiState`.
2. It treats `UiAction` output from UI input handlers as part of the ownership
   split, not as a merely optional afterthought.

This companion should therefore follow that structure exactly.

### Phase alignment

| Completion-plan phase | Companion-plan role |
|---|---|
| Phase 1 — carve `EditorState` / `UiState` out of `ReplRuntimeState` | Add audit script, slice inventory, compatibility-wrapper strategy, per-slice migration order, guards |
| Phase 2 — REPL stops touching editor text | Add concrete grep targets, call-site transaction pattern, `EditorBufferView` conversion checklist |
| Phase 3 — `repl_compile` is the validation gate | Add behavior-preservation list and focused commit/reject/undo tests |
| Phase 4 — UI input handlers emit `UiAction` | Add migration waves, action-count tracking, mutation-call guards |
| Phase 5 — rename to match ownership | Add file-by-file rename checklist, redirect-header strategy, doc/callgraph update gates |

### State-shape alignment

Use the completion plan's three-state model:

```text
ReplState:
    document
    flat_program
    variables
    replay
    scenes
    import_export
    render
    presentation

EditorState:
    input
    buffer
    selection
    clipboard
    search
    autocomplete
    variable_drag
    scroll / editor navigation state
    undo
    camera_nav, if kept mouse/editor-session-owned

UiState:
    viewport
    pointer
    status
    help visibility
    variable/profile panel visibility
    cursor blink / render-only cursor pixels
    transient UI chrome state
```

Do not move UI visibility/status/pointer/viewport state into `EditorState` just
because it currently sits near editor code. That would merely replace one mixed
owner with another.

---

## Current Gap Inventory

### Gap 1: `repl_editor.c` is too broad

Today `repl_editor.c` combines several roles:

1. GLUT-style input router.
2. UI bridge for menus, search, help, rename, replay, and camera routes.
3. Text editor model for input buffer, cursor, pending newline, edit line, and
   insert mode.
4. Commit orchestration, including commit-before-navigation and rollback on
   rejected parse.
5. Direct coordination of undo snapshots, command store writes, status text,
   dirty flags, and autocomplete refresh.

Target split:

| Current responsibility | Target home |
|---|---|
| input event routing | `imrepl_ctrl.c` dispatch + `UiAction` handling |
| active input buffer / cursor / pending newline | `EditorState` / `editor_document.c` |
| line navigation | `editor_navigation.c` |
| commit-before-navigation | `editor_commit.c` |
| validation of proposed text | `repl_compile.c` |
| structural block expansion semantics | REPL compiler / validator |
| command-array mutation | `repl_apply_*` / `repl_command_store.c` |
| editor-buffer mutation | `editor_buffer.c` |
| undo transaction creation | `editor_undo.c` |
| status/help/profile/panel visibility | `UiState` / `ui_action_dispatch.c` |
| GLUT cursor/redraw/timer effects | `imrepl_ctrl.c` |

### Gap 2: `ReplRuntimeState` still contains editor/UI slices

The completion plan lists the intended owners. The audit should find current
uses of editor/UI-like slices still exposed through `repl_state_*` accessors:

```text
editor_input
editor_buffer
editor_transformers
editor_highlights
editor_virtual_lines
selection
clipboard
autocomplete
search
variable_drag
code_panel
help
profile_panel
variable_panel
status
viewport
pointer
camera / camera controls
```

The cleanup must classify each as one of:

```text
ReplState       program / replay / export / persistent render config
EditorState    editable document and editor-session state
UiState        transient chrome, viewport, pointer, status, visibility
SceneState     only if a separate scene/app state is introduced later
```

### Gap 3: `repl_command_store.c` writes editor text

The `_with_line[s]` APIs were a good migration bridge while deleting
`GLCmd.source[]`, but they are now an ownership inversion.

Target:

```text
repl_command_store_*      -> command array only
editor_buffer_*           -> text array only
editor_commit_*           -> coordinates both through one transaction
```

### Gap 4: REPL modules read editor text through global state

REPL-side code may need source text for replay annotations, export, debug dumps,
or reparse flows. That text must be passed in as a view, not discovered through
global editor state.

Target shape:

```c
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int line_count;
} EditorBufferView;

const char *editor_buffer_view_line(EditorBufferView view, int idx);
```

Likely consumers:

```text
repl_replay_annotations.c
repl_export.c
repl_flatten.c / reparse helpers
repl_executor.c / flat command text helpers
repl_debug.c
repl_code_panel_document.c or replay display-text helpers, depending on final owner
```

### Gap 5: UI input bridges are not output-driven

The render path is mostly snapshot-shaped, but input handlers still call state
mutation functions directly. This is Phase 4 in the completion plan.

Target:

```text
ui_* input handler + snapshot + event -> UiActionList
imrepl_ctrl_dispatch_action(action) -> mutates UiState / EditorState / ReplState
```

Do not defer this indefinitely. It is the UI half of the same ownership split.

---

## Phase 0: Add Audits Before Moving Code

This phase should land before large structural changes. It makes the gap visible
and gives each later phase a measurable exit criterion.

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

Wire it as a report target first:

```make
.PHONY: audit-editor-ownership
audit-editor-ownership:
	@scripts/audit_editor_ownership.sh
```

### 0.2 Add a baseline table

After adding the script, update this section with real counts:

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

The first goal is not zero. The first goal is to make drift visible.

### 0.3 Add explicit TODO markers for tolerated exceptions

Use a searchable marker:

```c
/* EDITOR_OWNERSHIP_TODO(phase-2): takes EditorBufferView instead. */
```

The audit can count known exceptions separately from unknown regressions.

---

## Phase 1 Companion: Carve `EditorState` / `UiState` Out Safely

Completion-plan Phase 1 is mechanical storage movement. Recommended landing
sequence:

### 1.1 Add new state structs and compatibility wrappers

Add:

```text
editor_state.h/.c
editor_state_views.h        optional split, mirrors repl_state_views.h
editor_state_owners.h       optional split, mirrors repl_state_owners.h
ui_state.h/.c
ui_state_views.h            optional
ui_state_owners.h           optional
```

Move storage first, callers second. Keep temporary wrappers:

```c
ReplEditorInputView repl_state_editor_input(void) {
    return editor_state_input();
}

ReplStatusState repl_state_status(void) {
    return ui_state_status();
}
```

This makes ownership real without creating a giant rename commit.

### 1.2 Migrate one slice group at a time

Suggested order:

1. `editor_buffer` — most central to this cleanup.
2. `editor_input` / cursor / edit line / insert mode / pending newline.
3. selection + clipboard.
4. search + autocomplete.
5. transformer/highlight/virtual-line snapshot lists.
6. code-panel scroll/editor viewport state.
7. status/help/profile/variable panel into `UiState`.
8. pointer/viewport into `UiState`.
9. camera-nav decision: `EditorState.camera_nav` or later scene/app state.

### 1.3 Promote guards slice-by-slice

Do not add one giant failing check immediately. Add narrow checks as each slice
migrates:

```make
check-no-repl-editor-buffer-access:
	@if grep -RIn 'repl_state_editor_buffer' -- '*.c' '*.h' | grep -v '^tests/'; then \
		echo 'ERROR: editor buffer must use editor_buffer_* accessors'; exit 1; \
	fi
```

Verification:

```sh
make test && make test-stubs
make audit-editor-ownership
```

Exit criterion:

```text
Editor-owned slices live in EditorState.
UI-owned slices live in UiState.
Compatibility wrappers either removed or explicitly marked transitional.
```

---

## Phase 2 Companion: Stop REPL Touching Editor Text

Completion-plan Phase 2 combines command-store cleanup and explicit text views.
This is the most important boundary fix after state extraction.

### 2.1 Add editor-buffer mutation API

```c
const char *editor_buffer_line(int idx);
int editor_buffer_count(void);
void editor_buffer_set_count(int count);

int editor_buffer_insert_line(int pos, const char *line);
int editor_buffer_insert_lines(int pos, const char *const *lines, int count);
int editor_buffer_replace_line(int pos, const char *line);
int editor_buffer_delete_range(int start, int count);
int editor_buffer_load(const char *const *lines, int count);
void editor_buffer_clear(void);

EditorBufferView editor_buffer_view(void);
```

### 2.2 Remove command-store text APIs

Delete/deprecate:

```text
repl_command_store_insert_many_with_lines
repl_command_store_insert_one_with_line
repl_command_store_replace_one_with_line
repl_command_store_load_with_lines
```

The remaining store API should mutate only `GLCmd` arrays and command-store
cursor/count mechanics.

### 2.3 Convert combined writes into explicit transactions

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

Where rollback matters, wrap both sides in an editor commit transaction. Initial
implementation can use existing snapshots; later phases can refine the undo
shape.

### 2.4 Pass `EditorBufferView` to REPL consumers

Convert global text reads to parameters:

```c
void repl_replay_annotations_prepare(EditorBufferView text);
int  repl_export_save_workspace(..., EditorBufferView text);
const char *repl_replay_code_panel_get_command_display_text(EditorBufferView text,
                                                            int cmd_idx);
```

Verification:

```sh
make test && make test-stubs

grep -RIn 'repl_command_store_.*_with_line' . --include='*.c' --include='*.h'
# expected: 0

grep -l '#include "editor_state.h"' repl_*.c | grep -v repl_state.c
# expected: empty, or only marked transition exceptions
```

---

## Phase 3 Companion: Make `repl_compile` the Validation Gate

Completion-plan Phase 3 is where the spirit becomes true. The editor proposes a
text change; the REPL compiles it without mutation; the editor applies the
result as a transaction.

### 3.1 Preserve behavior first

Do not change UX while splitting ownership. Preserve:

```text
Enter on unchanged line enters insert mode / advances as before.
Navigation commits modified input before moving.
Rejected navigation restores committed model and preserves rejected status.
Empty insert exits insert mode as before.
Var declaration restrictions remain unchanged.
For/function/if structured commits remain unchanged.
Autocomplete refresh remains unchanged.
Normals/flat dirty flags remain unchanged.
Color picker edits still reparse and update text + command together.
```

### 3.2 Add focused tests before or during the split

Required tests:

```text
modified line + failed validation does not alter editor buffer
modified line + failed validation does not alter command array
modified line + successful validation updates both
commit-before-navigation rejected parse restores previous input/model
undo after successful commit restores both text and command
block-structured commit returns all lines/cmds before applying any mutation
color picker failed reparse does not corrupt text or cmd args
```

### 3.3 Suggested API checkpoints

Compiler side:

```c
ReplCompileResult repl_compile(const char *line,
                               const ReplCompileContext *ctx,
                               ReplParsedLine *out,
                               char *err_msg, int err_size);
```

Apply side:

```c
void repl_apply_insert(int pos, const GLCmd *cmd);
void repl_apply_replace(int pos, const GLCmd *cmd);
void repl_apply_delete(int start, int count);
void repl_apply_load(const GLCmd *cmds, int count);
```

Editor side:

```c
EditorCommitResult editor_commit_current_input(EditorState *editor,
                                               ReplState *repl,
                                               int enter_mode);
```

Verification:

```sh
make test_repl_editor
make test_repl_core_commit
make test && make test-stubs
```

---

## Phase 4 Companion: UI Input Emits `UiAction`

This should align with completion-plan Phase 4, not be treated as optional.

### 4.1 Migration waves

1. Keyboard text editing:
   ```text
   UI_ACTION_INSERT_TEXT
   UI_ACTION_DELETE_TEXT
   UI_ACTION_MOVE_CURSOR
   UI_ACTION_COMMIT_LINE
   UI_ACTION_TOGGLE_INSERT_MODE
   ```
2. Code-panel mouse selection:
   ```text
   UI_ACTION_SELECT_BEGIN
   UI_ACTION_SELECT_EXTEND
   UI_ACTION_SELECT_CLEAR
   ```
3. Clipboard/search/autocomplete:
   ```text
   UI_ACTION_CLIPBOARD_CUT/COPY/PASTE
   UI_ACTION_SEARCH_*
   UI_ACTION_AUTOCOMPLETE_*
   ```
4. Color picker / variable slider:
   ```text
   UI_ACTION_COLOR_PICKER_*
   UI_ACTION_VAR_DRAG_*
   ```
5. Menu/config/replay/load/save:
   ```text
   UI_ACTION_CONFIG_*
   UI_ACTION_REPLAY_*
   UI_ACTION_LOAD_*
   UI_ACTION_SAVE
   ```
6. Camera/scene gestures:
   ```text
   UI_ACTION_CAMERA_ORBIT/PAN/ZOOM
   ```

### 4.2 Add dispatch gate

```c
void imrepl_ctrl_dispatch_action(const UiAction *action);
void imrepl_ctrl_dispatch_actions(const UiActionList *actions);
```

This becomes the only input-event mutation gate.

### 4.3 Tighten UI guards

After each wave, forbid direct mutation calls from the migrated UI files:

```sh
grep -rE 'repl_(action|command_store|clipboard|undo|search|var_drag|autocomplete)_[a-z_]+\(' ui_*.c
```

Verification:

```sh
make test && make test-stubs
make check-ui-no-repl-state-read
make check-ui-renderer-takes-view
```

Manual smoke is essential here because behavior drift risk is highest.

---

## Phase 5 Companion: Rename Files to Match Ownership

Completion-plan Phase 5 should land only after behavior and ownership have
already moved. Renames should be mechanical.

### 5.1 Rename checklist

| Old | New | Owner |
|---|---|---|
| `repl_editor.c` | split into `editor_input.c` / `editor_commit.c` | editor/controller |
| `repl_undo.c` | `editor_undo.c` | editor |
| `repl_clipboard.c` | `editor_clipboard.c` | editor |
| `repl_search.c` | `editor_search.c` | editor |
| `repl_inline_rename.c` | `editor_inline_rename.c` | editor/UI |
| `repl_var_drag.c` | `editor_var_drag.c` | editor |
| `repl_autocomplete.c` | `editor_autocomplete.c` | editor |
| `repl_layout.c` | `editor_layout.c` or `ui_layout.c` | UI/layout |
| `repl_code_panel_layout.c` | `editor_code_panel_layout.c` or `ui_code_panel_layout.c` | UI/layout |
| `repl_code_panel_document.c` | `editor_code_panel_document.c` or `ui_code_panel_document.c` | editor/UI boundary |
| `repl_actions.c` | split into `ui_action_dispatch.c`, `editor_actions.c`, REPL actions | mixed |
| `repl_camera_controls.c` | `scene_camera_controls.c` or `viewport_camera_controls.c` | scene/UI |

### 5.2 Use redirect headers during transition

For noisy public headers, use temporary redirects:

```c
/* repl_undo.h -- transitional compatibility header. */
#ifndef REPL_UNDO_H
#define REPL_UNDO_H
#include "editor_undo.h"
#endif
```

Remove redirect headers after downstream includes are migrated.

### 5.3 Update documentation and callgraph grouping with each rename

Every rename commit should update:

```text
MODULES.md
ARCHITECTURE.md
scripts/callgraph_file_groups.json
Makefile source lists
relevant tests
```

Verification:

```sh
make test && make test-stubs
make callgraph-files
```

---

## Suggested Branch/Commit Breakdown

Branch name for implementation work:

```text
feature/editor-ownership-gap-cleanup
```

Suggested commits:

```text
1. tools: add editor ownership audit report
2. docs: record baseline editor ownership audit counts
3. refactor: add EditorState and UiState storage with compatibility accessors
4. refactor: migrate editor buffer accessors to editor namespace
5. refactor: migrate editor input/cursor accessors to editor namespace
6. refactor: migrate selection/clipboard/search/autocomplete accessors
7. refactor: migrate status/help/panel/viewport/pointer slices to UiState
8. refactor: remove text writes from repl_command_store
9. refactor: pass EditorBufferView to REPL text consumers
10. test: cover failed validation and commit transaction invariants
11. refactor: split repl_compile validation from editor_commit application
12. refactor: introduce UiAction dispatch gate
13. refactor: migrate keyboard input handlers to UiAction
14. refactor: migrate mouse/menu/color/camera input handlers to UiAction
15. refactor: rename editor-owned modules
16. docs: refresh MODULES and ARCHITECTURE after ownership split
17. checks: promote editor ownership audits to hard guards
```

Keep each commit buildable. Avoid landing a commit that requires the next phase
to restore green tests.

---

## Verification Matrix

Run after every phase:

```sh
make test
make test-stubs
make audit-editor-ownership
```

Run after commit/undo phases:

```sh
make test_repl_editor
make test_repl_core_commit
make test_repl_command_store
```

Run after UI/render/input boundary changes:

```sh
make check-ui-no-repl-state-read
make check-ui-renderer-takes-view
make check-state-boundaries
```

Manual smoke checklist:

```text
Type valid GL command; line commits and renders.
Type invalid GL command; status error appears and previous command remains.
Modify existing line; successful commit updates code panel and scene.
Modify existing line with invalid text; navigation does not corrupt document.
Undo after insert restores text and scene.
Undo after color picker edit restores text and parsed color.
Copy/cut/paste preserves line text.
Search still finds committed text.
Autocomplete still works from active input.
Replay annotations still show source/eval text.
Export/save still writes expected source text.
Load workspace restores text and parsed command model.
Menu/config/replay shortcuts still route through actions.
Camera orbit/pan/zoom still work after UiAction migration.
```

---

## Exit Criteria

The cleanup is complete when these statements are true:

```text
1. Program-owned slices live in ReplState.
2. Editor-owned slices live in EditorState.
3. UI/session chrome slices live in UiState.
4. Editor text is mutated only through editor_buffer/editor_document APIs.
5. repl_command_store mutates only GLCmd arrays and related command-store state.
6. REPL modules that need source text receive EditorBufferView explicitly.
7. Commit validation can be called without mutating editor text or command state.
8. Successful commit application updates editor text and REPL command model as one transaction.
9. Undo restores both editor text and REPL command state consistently.
10. ui_* renderers remain snapshot-only.
11. ui_* input handlers emit UiAction rather than mutating state directly.
12. imrepl_ctrl is the input-event mutation gate.
13. MODULES.md and ARCHITECTURE.md describe the same ownership boundaries enforced by checks.
```

---

## Non-goals

Do not combine this cleanup with:

```text
sample.c -> imrepl.c rename
full repl_core.c dissolution
color scheme / syntax theme extraction
new editor UX behavior
new parser features
new command syntax
```

Those are separate tracks. This branch is about ownership and boundaries, not
feature behavior.
