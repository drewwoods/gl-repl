# Implementation Plan - Explicit File-Scope Insertion Slot (Revision 3)

Implement a visible, discoverable insertion point above the generated `void display(void) {` line in the code panel for declaring top-level variables and functions, incorporating all review feedback on commit transaction unification, non-redundant exit navigation, complete undo lifecycle, structural boundary re-indentation, selection clearing, and early key dispatch gating.

## User Review Required

> [!IMPORTANT]
> **Key Architectural Guarantees in Revision 3**:
> 1. **Unified Restricted Commit Path**: A single `CommitResult commit_file_scope_input(void)` helper serves Enter, Semicolon, and `commit_before_navigation()` (called on navigation and mouse-click transitions). It rejects comments, GL calls, assignments, loops, and blank lines with `"File scope accepts only float declarations or function definitions"`, holding slot, input, and cursor on rejection.
> 2. **Single-Commit Exit Primitive & Semantic Direction**: Slot exit executes `commit_before_navigation()` exactly once. If accepted/unchanged, it clears insert mode/scope, recomputes `display_body_start`, resolves semantic target lines (`display_body_start - 1` for Up, `display_body_start` for Down), and navigates directly using the internal raw resolver `navigate_to_line_raw_resolved()` without redundant secondary commits or stale targets.
> 3. **Complete Undo Lifecycle**: File-scope commits (`editor_try_commit_file_scope_decl()` and `editor_compile_func_def()`) compile first, preflight, push exactly one undo snapshot via `editor_undo_push_snapshot()`, apply the change, and run post-effects (`clear_input = 1`, `load_line_after_apply = 0`, retargeting boundary).
> 4. **Structural-Edit-Aware Re-indentation**: `repl_reindent_after_change(at_before, change)` maps post-change indices to pre-change coordinates, correctly handling insertions, deletions, replacements, and skipping new rows. `repl_reindent_after_boundary_move(at_before)` is preserved as a fallback for equal-length replacement callers like `comment_toggle.c`.
> 5. **Early Dispatch Gate & Selection Safety**: Slot entry clears line-range and character selections. `keyboard_func()` intercepts `EDITOR_INSERT_FILE_SCOPE` at the top of dispatch to block Ctrl+X (cut), Ctrl+V (paste), Ctrl+/ (comment toggle), and Ctrl+\ (whole-doc format) with the file-scope diagnostic.
> 6. **Scope Lifetime & Re-assertion**: Scope resets to `EDITOR_INSERT_DOCUMENT` whenever `edit_line` changes or `insert_mode == 0`. Float declaration commit post-effects explicitly re-assert `edit_line = new_display_body_start`, `insert_mode = 1`, and `insert_scope = EDITOR_INSERT_FILE_SCOPE`.

---

## Proposed Changes

### Component 1: Editor State Model & Invariant Enforcement

#### [MODIFY] [state.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/state.h) & [state.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/state.c)
- Define `EditorInsertScope` enum:
  ```c
  typedef enum {
      EDITOR_INSERT_DOCUMENT = 0,
      EDITOR_INSERT_FILE_SCOPE
  } EditorInsertScope;
  ```
- Add `EditorInsertScope insert_scope` to `EditorInputState` and `EditorInputView`.
- Update `EditorInputView` contract comments to document `insert_scope`.
- Add public accessors:
  - `EditorInsertScope editor_insert_scope(void);`
  - `void editor_insert_scope_set(EditorInsertScope scope);`
- In `editor_state_input()`: copy `in->insert_scope` into `EditorInputView`.
- In `editor_insert_mode_set(int insert_mode)`: when `insert_mode == 0`, reset `insert_scope = EDITOR_INSERT_DOCUMENT`.
- In `editor_state_edit_line_set(int line)`: if `line != g_editor_state.document.edit_line_idx`, reset `insert_scope = EDITOR_INSERT_DOCUMENT`.
- In `editor_state_input_reset()` and `editor_state_reset()`: ensure `insert_scope = EDITOR_INSERT_DOCUMENT`.

---

### Component 2: Unified Commit Transaction, Navigation, & Router

#### [MODIFY] [commit.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/commit.h) & [commit.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/commit.c)
- Add checked commit helper for file-scope declarations:
  - `int editor_try_commit_file_scope_decl(void)`:
    1. Check `repl_compile_float_decl(&change, input, edit_line, ctx, err, sizeof(err))`.
    2. If compile error or `change.kind == REPL_COMPILED_NO_CHANGE`, call `repl_set_status_error(err)` and return 0.
    3. Preflight check `repl_apply_can_apply_compiled_change(&change)`. If false, return 0.
    4. Push undo snapshot `editor_undo_push_snapshot()`.
    5. Apply change `apply_compiled_change_full(&change)`.
    6. Post-effects: `editor_input_clear()`, `editor_cursor_pos_set(0)`, `editor_completion_clear()`.
    7. Retarget & re-assert:
       ```c
       int new_body_start = repl_source_scope_display_body_start();
       editor_state_edit_line_set(new_body_start);
       editor_insert_mode_set(1);
       editor_insert_scope_set(EDITOR_INSERT_FILE_SCOPE);
       ```
    8. Publish status message `repl_set_status(change.commit_message)`.
    9. Return 1 on success.
- Add `int editor_try_commit_file_scope_func_def(void)`:
  1. Compile via `editor_compile_func_def(&plan, input, edit_line, ctx, err, sizeof(err))`.
  2. If compile error or no match, call `repl_set_status_error(err)` and return 0.
  3. Preflight check `repl_apply_can_apply_compiled_change(&plan.change)`. If false, return 0.
  4. Push undo snapshot `editor_undo_push_snapshot()`.
  5. Apply plan `editor_commit_apply_plan(&plan)` (cursor lands inside function body).
  6. Post-effects: reset `editor_insert_scope_set(EDITOR_INSERT_DOCUMENT)`.
  7. Return 1 on success.

#### [MODIFY] [input.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/input.h) & [input.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/input.c)
- Implement unified `commit_file_scope_input(void)` returning `CommitResult`:
  - If input is empty:
    - If called from Enter/semicolon: publish error `"File scope accepts only float declarations or function definitions"`, return `COMMIT_REJECTED`.
    - If called from navigation: return `COMMIT_OK` (no changes to commit, clean transition).
  - If input starts with `//` (comment), assignment (`=`), GL call, `if`/`for`/`}`:
    - Publish error `"File scope accepts only float declarations or function definitions"`, return `COMMIT_REJECTED`.
  - Try `editor_try_commit_file_scope_decl()`: if matched & applied, return `COMMIT_OK`; if matched & rejected, return `COMMIT_REJECTED`.
  - Try `editor_try_commit_file_scope_func_def()`: if matched & applied, return `COMMIT_OK`; if matched & rejected, return `COMMIT_REJECTED`.
  - Otherwise: publish error `"File scope accepts only float declarations or function definitions"`, return `COMMIT_REJECTED`.
- In `commit_current_input()`:
  - At the very top, if `editor_insert_scope() == EDITOR_INSERT_FILE_SCOPE`, return `commit_file_scope_input()`.
- **Slot Entry**:
  - `int editor_input_enter_file_scope_slot(void)`:
    - Call `if (commit_before_navigation() == COMMIT_REJECTED) return 0;`
    - Clear selections: `editor_selection_clear_line_range(); editor_clipboard_sel_clear();`
    - Set `editor_state_edit_line_set(repl_source_scope_display_body_start())`.
    - Set `editor_insert_mode_set(1); editor_insert_scope_set(EDITOR_INSERT_FILE_SCOPE);`.
    - Clear input, autocomplete, set cursor_pos = 0.
    - Return 1.
- **Slot Exit**:
  - `int editor_input_exit_file_scope_slot(int semantic_dir_up)`:
    - Call `if (commit_before_navigation() == COMMIT_REJECTED) return 0;`
    - Clear `editor_insert_mode_set(0); editor_insert_scope_set(EDITOR_INSERT_DOCUMENT);`
    - Recompute `int body_start = repl_source_scope_display_body_start();`
    - Resolve target: `int target = semantic_dir_up ? (body_start > 0 ? body_start - 1 : 0) : body_start;`
    - Navigate directly via `navigate_to_line_raw_resolved(target);` (no second commit!).
    - Return 1.
- **Vertical Navigation (`handle_vertical_special_key_route`)**:
  - `GLUT_KEY_UP`:
    - When `!tutorial_active()` and cursor is at `display_body_start` (in non-insert mode or body insert mode) or trailing line in functions-only doc (`display_body_start == document_count`): call `editor_input_enter_file_scope_slot()`.
    - When in `EDITOR_INSERT_FILE_SCOPE`: call `editor_input_exit_file_scope_slot(1 /* up */)`.
  - `GLUT_KEY_DOWN`:
    - When in `EDITOR_INSERT_FILE_SCOPE`: call `editor_input_exit_file_scope_slot(0 /* down */)`.
- **Early Dispatch Gate in `keyboard_func()`**:
  - If `editor_insert_scope() == EDITOR_INSERT_FILE_SCOPE`:
    - Intercept Ctrl+X (`handle_cut_key_route`), Ctrl+V (`handle_paste_key_route`), Ctrl+/ (`handle_comment_toggle_key_route`), and Ctrl+\ (`handle_buffer_command_key_route`).
    - Block mutation, call `repl_set_status_error("File scope accepts only float declarations or function definitions")`, and return.

#### [MODIFY] [hit.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/ui/app/hit.h) & [glr_ctrl_router.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/app/glr_ctrl_router.c)
- In `src/ui/app/hit.h`: add `UI_HIT_CODE_FILE_SCOPE_INSERT` to `UiAppHitKind`.
- In `src/app/glr_ctrl_router.c`: dispatch `UI_HIT_CODE_FILE_SCOPE_INSERT` by calling `editor_input_enter_file_scope_slot()`, routing epilog, and requesting redraw.

---

### Component 3: Structural-Edit-Aware Boundary Re-indentation

#### [MODIFY] [reformat.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/reformat.h) & [reformat.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/repl/reformat.c)
- Introduce `void repl_reindent_after_change(int at_before, const ReplCompiledChange *change)`:
  - If `change == NULL`: fallback to direct index comparison `pre_idx = cmd_idx` for equal-length replacement callers.
  - If `change != NULL`: map each post-change row index `cmd_idx` back to its pre-change index `pre_idx`:
    - `REPL_COMPILED_INSERT_ONE`:
      - `cmd_idx < change->pos`: `pre_idx = cmd_idx;`
      - `cmd_idx == change->pos`: newly inserted row; skip!
      - `cmd_idx > change->pos`: `pre_idx = cmd_idx - 1;`
    - `REPL_COMPILED_INSERT_MANY`:
      - `cmd_idx < change->pos`: `pre_idx = cmd_idx;`
      - `cmd_idx >= change->pos && cmd_idx < change->pos + change->count`: newly inserted rows; skip!
      - `cmd_idx >= change->pos + change->count`: `pre_idx = cmd_idx - change->count + change->delete_count;`
    - `REPL_COMPILED_DELETE_RANGE`:
      - `cmd_idx < change->pos`: `pre_idx = cmd_idx;`
      - `cmd_idx >= change->pos`: `pre_idx = cmd_idx + change->count;`
    - `REPL_COMPILED_REPLACE_ONE`:
      - `pre_idx = cmd_idx;`
    - For mapped existing rows (`pre_idx >= 0`):
      - `was_body = pre_idx >= at_before;`
      - `is_body  = cmd_idx >= at_after;`
      - If `was_body != is_body`, shift row indentation by `delta = (is_body ? 2 : 0) - (was_body ? 2 : 0)`.
- Keep `void repl_reindent_after_boundary_move(int at_before)` in `reformat.h` calling `repl_reindent_after_change(at_before, NULL)` so existing caller `comment_toggle.c:429` remains clean and backwards-compatible.

#### [MODIFY] [commit.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/editor/commit.c)
- In `apply_compiled_change_full()`:
  - Capture `int at_before = repl_source_scope_display_body_start();` before mutation.
  - After `repl_apply_compiled_change()` lands, call `repl_reindent_after_change(at_before, change);`.

---

### Component 4: UI Code Panel Layout, Rendering, & Autocomplete

#### [MODIFY] [repl_code_panel.h](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/ui/app/repl_code_panel.h) & [repl_code_panel.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/ui/app/repl_code_panel.c)
- Read `snap->tutorial.active` on `UiRenderSnapshot` (preserving UI snapshot purity).
- Layout accounting:
  - `file_scope_slot_rows`: 0 when `snap->tutorial.active`, `repl_code_panel_current_input_rows()` when active in `EDITOR_INSERT_FILE_SCOPE`, 1 (`+ float or function` placeholder) when inactive.
  - Include `file_scope_slot_rows` in `total_lines` before `display_open_rows`.
  - Adjust `ui_repl_code_panel_display_open_row()` and `cursor_doc_line` to account for the slot.
- Rendering:
  - In `repl_code_panel_build_rows()`: emit the file-scope slot immediately before `display_open_rows` (active input row at indent 0 or placeholder `+ float or function` with hit `UI_HIT_CODE_FILE_SCOPE_INSERT`).
  - In `repl_code_panel_add_rows_for_line()`: emit the body insert row only when `snap->editor_input.insert_scope == EDITOR_INSERT_DOCUMENT`.
- Reverse mapping:
  - Update `ui_repl_code_panel_target_for_doc_line()` to take optional `EditorInsertScope *out_insert_scope` to distinguish the file-scope slot from the body insert row at `display_body_start`.

#### [MODIFY] [glr_completion.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/src/app/glr_completion.c)
- When `editor_state_input().insert_scope == EDITOR_INSERT_FILE_SCOPE`:
  - Complete `float `, `static float `, and function definition headers (`func0() {`, user signatures).
  - Suppress OpenGL enum and command completions.

---

### Component 5: Tests & Golden Verification

#### [MODIFY] [test_repl_editor.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/tests/test_repl_editor.c), [test_repl_code_panel_document.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/tests/test_repl_code_panel_document.c), [test_repl_core_commit.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/tests/test_repl_core_commit.c), [test_glr_extedit.c](file:///Users/drew/src/code/openGL/samples/gen-ai/gl-repl/tests/test_glr_extedit.c)
- Test coverage for:
  1. Slot rendering & placeholder `+ float or function` (full mode and code-focus mode, with/without prologue).
  2. Keyboard Up-arrow entry and Down-arrow exit across `display_body_start` (including functions-only scenes where `display_body_start == document_count`).
  3. Single-commit exit transaction & semantic direction resolution without stale targets.
  4. Undo lifecycle: undo snapshot pushed on successful float/func commit, undo restores previous state cleanly.
  5. Early dispatch gate (Ctrl+X cut, Ctrl+V paste, Ctrl+/ comment toggle, and Ctrl+\ whole-doc format blocked at file scope).
  6. Selection clearing on slot entry.
  7. Restricted commit chain:
     - `float x = 1;` / `static float y;` success + consecutive declaration slot retargeting and scope re-assertion.
     - `func0() {` success + cursor entering function body + scope reset to `DOCUMENT`.
     - Rejected input (comments, GL calls, assignments, loops, duplicate func/var names, overflow, blank lines on Enter) -> status error `"File scope accepts only float declarations or function definitions"` + slot held + input held.
  8. Structural-edit-aware boundary re-indentation:
     - Hoisting declaration past comment-led scene re-indents crossing comments to indent 0.
     - Deleting multi-row function does not double-indent shifted body rows.
  9. `target_for_doc_line` disambiguation between the file-scope slot and body insert row.
  10. Tutorial suppression (`snap->tutorial.active` disables slot).
  11. `--watch` / extedit input-row gate in `test_glr_extedit.c`.

---

## Verification Plan

### Automated Tests
- Run `make test-stubs` (all 86 test binaries).
- Run `make check-state-ownership`.
- Run `make check-c99`.
- Run `make test-web`.
- Run `make test-scenes`.
- Run `make rebuild-golden` if panel text fixtures change, and review diffs.

### Manual / Interactive Verification
- Verify interactive typing at file scope:
  - Up-arrow into file scope slot.
  - Type `float my_var = 1.5;` and press Enter -> verifies variable declared, slot retargeted and re-asserted.
  - Type `func0() {` and press Enter -> verifies function created, matching `}` inserted, cursor placed inside func body.
  - Type `glColor3f(1,0,0);` in file scope slot -> verifies rejection with clear diagnostic, slot held.
  - Press Up/Down to navigate out -> verifies single commit and clean navigation.
