# Tutorial System

## Context

The REPL today has built-in examples (`src/repl/examples.c`) and user scenes
(`src/repl/scenes.c`) reachable from the Scene menu. New users see finished
scenes but get no guided path for learning the command set. We want a
tutorial mode that reveals one instructional `// comment` at a time, waits
for the user to type a matching command, and reveals the next comment on
success — using the existing editor, parser, and code-panel rendering.

User-facing behavior agreed:
- Tutorials live in their own top-level menu ("Tutorials"), parallel to Scene.
  The dropdown lists tutorial names and, while a tutorial is active, also
  exposes "Restart Tutorial" and "Exit Tutorial" items.
- Each step is a `(comment, expected)` pair. Match is whitespace-tolerant
  exact-string in v1; the match result struct carries enum + arg_index
  fields so v2 can distinguish wrong command from wrong argument without
  touching call sites.
- Mismatch surfaces via `repl_set_status` only; typed input is preserved
  so the user can edit and retry.
- Pressing **Tab** fills the input buffer with the current step's expected
  command (whole-line) but does **not** commit. The user sees the answer,
  can inspect/edit, and commits with `;`.
- Revealed comments are read-only while the tutorial is active.
- Completing the last step shows a status message and leaves the buffer in
  place; no auto-promotion to a user scene.
- Newly revealed comments fade in left-to-right (~0.5 s total) for a subtle
  cue. After the fade window the row reverts to the cheap whole-line draw.

## MVP scope

1. Tutorial catalog in `src/repl/tutorials.c`.
2. Runtime state in `src/widgets/tutorial_state.c`.
3. Runner in `src/widgets/tutorial.c`.
4. Top-level **Tutorials** menu with selection + **Restart** / **Exit**.
5. Whitespace-tolerant exact matching with a future-friendly mismatch enum.
6. Status message on mismatch, preserving the typed input.
7. Advance to next `//` comment after a correct commit.
8. Read-only tutorial comments.
9. Left-to-right fade for newly revealed comments.
10. **Tab autofill** of the current expected command (whole-line; no commit).
11. Auto-exit when an example or user scene is loaded.

## Files to create

| Path | Purpose |
|---|---|
| `src/widgets/tutorial_state.h` / `.c` | Peer subsystem owning `TutorialRuntimeState` (active flag, tutorial idx, step, fade line + start time, last match result). Start with `_view/_mut/_reset/_active`; defer `_capture/_restore` unless tutorials later need to participate in undo/snapshot flows. |
| `src/widgets/tutorial.h` / `.c` | Runner: `tutorial_start(idx)`, `tutorial_exit()`, `tutorial_handle_commit_attempt(const char *input)`, `tutorial_advance_after_successful_commit()`, `tutorial_current_expected_text()`, `tutorial_step_fade_alpha(int line_idx, int char_idx, int line_len, float now)`, `tutorial_line_is_fading(int line_idx, float now)`, `tutorial_line_is_locked(int line_idx)`, `tutorial_guard_source_change(pos, delete_count, insert_count)` (used by editor to enforce read-only / no-renumber rules). |
| `src/repl/tutorials.h` / `.c` | Catalog parallel to `repl/examples`: per-tutorial null-terminated `comments[]` and `expected[]` arrays + name. Query API `repl_tutorial_count/name/step_count/step_comment/step_expected`. Ship 2–3 starter tutorials (e.g., "First Triangle", "Color & Transform"). |
| `tests/test_tutorial_match.c` | Pure-function tests for the match comparator (whitespace tolerance, shape mismatch, default field values). |
| `tests/test_tutorial_runner.c` | Runner tests: start enters a transient tutorial scene and emits step 0 comment through `repl_load_apply_line`, correct line committed through `editor_handle_key(';')` advances, wrong line preserves status + doesn't advance, direct `editor_feed_line` is not used for tutorial user commits, loading an example resets `tutorial_active()`, `tutorial_current_expected_text` reflects the current step, fade alpha hits 0 at start and 1 after duration. |

## Files to modify

| Path / region | Change |
|---|---|
| `src/app/glr_actions.h` (`GLR_MENU_*` enum) | Add `GLR_MENU_TUTORIALS` before `GLR_MENU_COUNT`. |
| `src/ui/menu_bar.c:21-30` | Add `MENU_TUTORIALS = GLR_MENU_TUTORIALS`, bump `NUM_MENUS` via `GLR_MENU_COUNT`, append `"Tutorials"` to `g_menu_labels`. |
| `src/ui/menu_bar.c:88-132` | Add `case MENU_TUTORIALS:` branches in `menu_item_count`, `menu_item_label` (returns `repl_tutorial_name(i)`, plus `"---"` / `"Restart Tutorial"` / `"Exit Tutorial"` trailing items when `tutorial_active()`), and `menu_item_shortcut` (NULL). |
| `src/app/glr_actions.c` (menu-click dispatch around lines 423-494) | Route `menu_id == GLR_MENU_TUTORIALS` clicks: tutorial-name → `tutorial_start(item_idx)`; Restart → `tutorial_start(current_tutorial_idx)`; Exit → `tutorial_exit()`. |
| `src/editor/input.c:1118-1203` (`handle_semicolon_commit_key_route`) and `:1101-1116` (`handle_enter_key_route`) | Add a shared tutorial precheck helper before normal commit. On mismatch, `repl_set_status(result.message)`, leave the input buffer, return consumed. On match, fall through to normal commit and call `tutorial_advance_after_successful_commit()` only after a real successful source mutation/accepted commit, never from the generic function tail. |
| `src/editor/input.c` (Tab key handler) | Keep the existing `editor_input_anchor_clear()` pre-step, then before delegating Tab to autocomplete, if `tutorial_active()`, replace the whole input buffer with `tutorial_current_expected_text()`, move cursor to end, set status `"Replaced input with expected tutorial command; press ; to commit"`, return consumed. This is intentionally always-overwrite while tutorial mode is active. |
| `src/editor/input.c` / `src/editor/clipboard.c` / `src/editor/undo.c` | Reject source mutations that would edit locked tutorial comments or renumber locked-line indices: replace/toggle/delete/cut of a locked row, insert/paste above or inside the locked prefix, Ctrl+L clear, Ctrl+\ reformat, and undo/redo while active. Input-row editing remains allowed. |
| `src/editor/input.c:232` (`load_line_to_input`) | If `tutorial_line_is_locked(idx)`, clear the input buffer, move cursor to 0, set status `"Tutorial instruction is read-only"`, and return. Navigation may land on instruction rows, but locked comment text should not be copied into the editable input row. |
| `src/repl/scenes.h` / `.c` | Add `repl_scenes_enter_transient_scene()` (name bikesheddable): save the active user scene if any, restore any pre-example cfg snapshot, clear pending scene-name metadata, then detach live state from both `active_example_idx` and `g_active_user_scene`. `tutorial_start()` calls this before clearing/loading tutorial content so tutorial buffers are not saved back into a user-scene slot. |
| `src/repl/example_loader.c:393-487` (`load_example_lines`, `repl_load_example`) and `src/repl/scenes.c` (`repl_load_workspace`, `repl_load_user_scene_idx`) | Call `tutorial_state_reset()` on entry so loading an example, loading a workspace, or activating a user scene exits tutorial mode cleanly. `repl_load_workspace()` loads slots directly and restores a live stash; do not rely on `repl_load_user_scene_idx()` for this reset. After workspace load, the live document is intentionally restored as it was; any visible tutorial comments remain but are editable because tutorial lock state is cleared. |
| `src/app/glr_ctrl.c` (`glr_app_reset_all`) | Reset tutorial state alongside replay/help/color-picker peers so full app reset and CLI startup cannot inherit an active tutorial. |
| `src/ui/panels.c` (`code_panel_draw_command_row`, `code_panel_draw_segment(...)` call) | If `tutorial_line_is_fading(i, ctx->snap->anim_time)`, draw the visible segment char-by-char with `glColor4f(r, g, b, alpha_for_char)`; otherwise keep the existing `code_panel_draw_segment` fast path. Use source line `i`, not a non-existent row field. |
| `src/widgets/tutorial.c` | Emit tutorial instruction comments via `repl_load_apply_line` from `src/repl/load.h`, not `feed_line`; timestamp newly revealed comments from the existing `repl_state_variables().anim_time` view. Render code receives `now` from `UiRenderSnapshot.anim_time`; both clocks come from the same source. Do not add a new `repl_anim_time_now()` public API. |
| `Makefile` | Add `tutorial.o`, `tutorial_state.o`, `tutorials.o` to the link list; add rules for `test_tutorial_match` and `test_tutorial_runner`; include both in the aggregate `test` target. |
| `MODULES.md` | List the three new modules in the layered overview; add `TutorialRuntimeState` to the state-ownership table. |
| `CLAUDE.md` (File Layout table) | One row per new file. |

## Data structures

`TutorialEntry` (in `tutorials.c`):
- `const char *name`
- `const char *const *comments` — null-terminated; each begins with `//`.
- `const char *const *expected` — same length as `comments`; literal command
  text, no trailing `;`.

`TutorialMatchKind`:
- `TUT_MATCH_OK`
- `TUT_MISMATCH_EMPTY` — input is empty / whitespace only
- `TUT_MISMATCH_SHAPE` — token count differs (v1 catch-all for "something is off")
- `TUT_MISMATCH_COMMAND` — reserved for v2 (command keyword wrong)
- `TUT_MISMATCH_ARG` — reserved for v2 (specific arg wrong)

`TutorialMatchResult`:
- `TutorialMatchKind kind`
- `int arg_index` — `-1` unless `TUT_MISMATCH_ARG`
- `char message[128]` — preformatted for `repl_set_status`

`TutorialRuntimeState`:
- `int active`
- `int tutorial_idx`
- `int step` (next comment to reveal)
- `int locked_line_count`
- `int locked_lines[64]` (source-line indices of revealed instruction comments)
- `int fade_line_idx` (-1 when no row is fading)
- `float fade_start_t`, `fade_duration` (0.5 s)
- `TutorialMatchResult last_result`

V1 `tutorial_match`: whitespace-normalize both strings, strcmp → `OK`,
`EMPTY`, or `SHAPE`. The enum and `arg_index` field are wider than v1
needs so v2 can drop in a token-walk classifier without touching call
sites.

V1 line-locking: `tutorial_line_is_locked(idx)` returns 1 iff `idx` is
in `locked_lines[0..locked_line_count)`. `tutorial_start()` records the
first revealed comment line; each successful
`tutorial_advance_after_successful_commit()` appends the newly revealed
comment's line index. Recorded indices stay valid because Phase 6 blocks
all non-tutorial source mutations that could edit locked comments or
renumber them (replace/toggle/delete/cut locked rows, insert/paste above
the locked prefix, clear-all, reformat, undo/redo). Input-buffer edits
are still allowed.

## End-to-end sequence

1. User clicks Tutorials → "First Triangle". `glr_actions.c` calls
   `tutorial_start(0)`.
2. `tutorial_start` saves/detaches the current scene via
   `repl_scenes_enter_transient_scene()`, clears the live document with
   `repl_state_document_reset()` (not `editor_clear_all_cmds()`), resets
   tutorial state / predef vars / function aliases / editor input
   transients, then emits `comments[0]` through a small runner helper
   around `repl_load_apply_line()`. That helper follows `src/repl/load.h`'s
   caller contract: set `edit_line` to `document_count`, clear the editor
   input buffer, clear insert mode, call the lean loader, then mark both
   flat state and normals dirty. It records `fade_line_idx = doc_count - 1`,
   appends that line to `locked_lines`, and sets `fade_start_t` from
   `repl_state_variables().anim_time`. Camera/orbit state is intentionally
   preserved when a tutorial starts; only the source document and language
   runtime state are reset.
3. Each frame, `panels.c` passes `ctx->snap->anim_time` to
   `tutorial_line_is_fading` / `tutorial_step_fade_alpha` for the source
   line. Fading rows draw per-char with `glColor4f`; all other rows keep
   the existing segment fast path.
4. User types and presses `;`. `handle_semicolon_commit_key_route` sees
   `tutorial_active()`, calls `tutorial_handle_commit_attempt(input)`.
   - Mismatch → `repl_set_status(result.message)`, return consumed, input
     preserved.
   - Match → fall through to normal commit. Only if the commit actually
     succeeds, `tutorial_advance_after_successful_commit()` increments
     `step`, appends the next comment through `repl_load_apply_line`,
     records the locked line, and seeds the new fade. If
     `comments[step]==NULL` it calls `repl_set_status("Tutorial complete")`
     and clears active state / lock state; the tutorial rows remain in the
     document and become editable.
5. Loading an example or user scene anywhere calls `tutorial_state_reset()`,
   exiting tutorial mode cleanly.

## Implementation Phases

Each phase is independently buildable and testable. Land them as separate
commits; the feature is non-visible until Phase 3 ships, and the MVP is
complete once Phase 7 lands (Phase 8 is docs only).

### Phase 1 — Catalog + peer state scaffolding

Goal: data and storage exist; nothing wired into UI or commit flow yet.

Create:
- `src/repl/tutorials.h` — query API (`repl_tutorial_count/name/step_count/step_comment/step_expected`) + struct typedefs.
- `src/repl/tutorials.c` — one starter tutorial: `g_tutorial_first_triangle_comments[]` and `_expected[]` (null-terminated parallel arrays), `g_tutorials[]` table, query implementations.
- `src/widgets/tutorial_state.h` / `.c` — `TutorialRuntimeState` struct, static `g_tutorial_state`, `tutorial_state_view/_mut/_reset/_active`. No capture/restore yet (tutorials don't participate in undo snapshots).

Modify:
- `src/repl/scenes.h` / `.c` — declare and implement
  `repl_scenes_enter_transient_scene()`: save the active user scene if
  one is loaded, restore any pre-example cfg snapshot, clear pending
  scene-name metadata, then set both the active user-scene marker and
  active example marker to `-1`. This gives tutorials an unsaved
  transient buffer and prevents tutorial contents from being flushed into
  a user-scene slot later.
- `Makefile` — append `tutorials.o`, `tutorial_state.o` to the same `OBJS` list that currently builds `replay_state.o` (`grep -n replay_state Makefile` to locate).

Verify: `make sample` builds. `nm sample | grep tutorial_state_view` shows the symbol.

### Phase 2 — Runner + match function (headless)

Goal: full runtime logic exists and is unit-tested, still not wired to UI.

Create:
- `src/widgets/tutorial.h` — public runner API:
  - `void  tutorial_start(int idx);`
  - `void  tutorial_exit(void);`
  - `int   tutorial_handle_commit_attempt(const char *input, TutorialMatchResult *out);`
  - `void  tutorial_advance_after_successful_commit(void);`
  - `const char *tutorial_current_expected_text(void);`
  - `float tutorial_step_fade_alpha(int line_idx, int char_idx, int line_len, float now);`
  - `int   tutorial_line_is_fading(int line_idx, float now);`
  - `int   tutorial_line_is_locked(int line_idx);`
  - `int   tutorial_guard_source_change(int pos, int delete_count, int insert_count);`
  - `TutorialMatchResult tutorial_match(const char *expected, const char *got);`
- `src/widgets/tutorial.c` — implementations. `tutorial_start` first calls
  `repl_scenes_enter_transient_scene()`, then clears the live document via
  `repl_state_document_reset()` (not `editor_clear_all_cmds()`), resets
  predef vars / function aliases / input transients, resets tutorial
  state, emits `comments[0]` through a file-private
  `tutorial_emit_instruction_comment()` helper backed by
  `repl_load_apply_line()`, appends that line to `locked_lines`, and
  records `fade_line_idx = repl_state_document_count() - 1` plus
  `fade_start_t = repl_state_variables().anim_time`. The emit helper must
  set `repl_state_edit_line` to `repl_state_document_count()`, clear the
  editor input buffer, clear insert mode before loading, and mark both
  `repl_state_mark_flat_dirty()` and `repl_state_mark_normals_dirty()`
  after loading, matching `src/repl/load.h`'s caller responsibilities.
  If the reset choreography starts spreading beyond `tutorial_start`,
  extract a REPL-side `repl_scenes_reset_for_transient()` helper rather
  than duplicating direct predef-var / alias / dispatch reset calls across
  widgets. `tutorial_match` v1 = whitespace-normalize-and-strcmp.
- `tests/test_tutorial_match.c` — uses `tests/support/test_harness.h`. Cases: exact match, extra inner whitespace, leading/trailing whitespace, trailing `;` tolerance, empty input → `TUT_MISMATCH_EMPTY`, token-count mismatch → `TUT_MISMATCH_SHAPE`, default `arg_index == -1`. The `TUT_MISMATCH_COMMAND` and `TUT_MISMATCH_ARG` enum values are declared but not produced by v1; tests just assert they exist so v2 can wire them in without an API break.
- `tests/test_tutorial_runner.c` — uses `tests/support/repl_test_support.h`. Cases: `tutorial_start(0)` → first comment appears at line 0 and begins with `//`, active user scene/example markers are detached, and line 0 is locked; correct line committed by loading `editor_state_input_mut()->input` and calling `editor_handle_key(';', 0, 0)` advances `step`; wrong line preserves `step` and input; direct `editor_feed_line(expected)` is not a tutorial-success path; `tutorial_current_expected_text()` returns the right string for the current step; fade-alpha math (now == start → 0 for char 0, now == start + duration → 1 for last char).
- `Makefile` — add rules for `test_tutorial_match` and `test_tutorial_runner` mirroring `test_eval` / `test_format`; add both to the aggregate `test` target.

Verify: `make test_tutorial_match` and `make test_tutorial_runner` pass.

### Phase 3 — Tutorials menu

Goal: user can pick a tutorial from the menu and see the first comment.

Modify (with exact anchors):
- `src/app/glr_actions.h:30-33` — change the enum to:
  ```c
  GLR_MENU_FILE = 0,
  GLR_MENU_SCENE,
  GLR_MENU_TUTORIALS,
  GLR_MENU_CONFIG,
  GLR_MENU_COUNT
  ```
- `src/ui/menu_bar.c:21-30` — add `MENU_TUTORIALS = GLR_MENU_TUTORIALS` to the local enum and append `"Tutorials"` to `g_menu_labels[]` (size auto-tracks via `GLR_MENU_COUNT`).
- `src/ui/menu_bar.c:88-100` (`menu_item_count`) — `case MENU_TUTORIALS: return repl_tutorial_count() + (tutorial_active() ? 3 : 0);` (the 3 trailing slots are the divider, Restart, and Exit items, shown only while a tutorial is active).
- `src/ui/menu_bar.c:102-132` (`menu_item_label`) — `MENU_TUTORIALS` branch: items `[0..N-1]` return `repl_tutorial_name(i)`; if `tutorial_active()`, item `N` is `"---"`, `N+1` is `"Restart Tutorial"`, `N+2` is `"Exit Tutorial"`.
- `src/ui/menu_bar.c:134+` (`menu_item_shortcut`) — `case MENU_TUTORIALS: return NULL;`.
- `src/app/glr_actions.c:422-498` (`glr_action_menu_item_activate`) — add a new branch after the `GLR_MENU_SCENE` block (line 450) and before `GLR_MENU_CONFIG` (line 494):
  ```c
  } else if (menu_id == GLR_MENU_TUTORIALS) {
      int n = repl_tutorial_count();
      if (item_idx < n) {
          tutorial_start(item_idx);
      } else if (tutorial_active() && item_idx == n + 1) {
          tutorial_start(tutorial_state_view().tutorial_idx);  /* Restart */
      } else if (tutorial_active() && item_idx == n + 2) {
          tutorial_exit();
      }
  ```
- Include `widgets/tutorial.h` and `widgets/tutorial_state.h` in `src/app/glr_actions.c` and `src/ui/menu_bar.c`.

Verify: `./sample` opens, the Tutorials menu lists the starter tutorial, clicking it clears the buffer and shows the first `// ...` comment on line 0; reopening the menu now shows the Restart/Exit items. No fade yet — that lands in Phase 7.
Also smoke-test a narrow window/code panel: adding the top-level
Tutorials menu shifts `menubar_rects()`'s left-menu edge, so verify the
Search and Replay pins remain usable or degrade acceptably when the code
panel is narrow.

### Phase 4 — Commit interception (match check)

Goal: typing wrong rejects with status; typing right advances and reveals next.

Modify (with exact anchors):
- `src/editor/input.c` — add a tiny shared helper near the commit routes:
  ```c
  static int tutorial_precheck_current_input(void) {
      if (!tutorial_active()) return 1;
      TutorialMatchResult r;
      if (!tutorial_handle_commit_attempt(editor_state_input().input, &r)) {
          repl_set_status(r.message);
          editor_completion_clear();
          return 0;
      }
      return 1;
  }

  static void tutorial_advance_if_commit_ok(CommitResult result) {
      if (tutorial_active() && result == COMMIT_OK)
          tutorial_advance_after_successful_commit();
  }
  ```
- `src/editor/input.c:1118-1203` (`handle_semicolon_commit_key_route`) — run
  `tutorial_precheck_current_input()` inside the `input_len > 0` branch
  before any undo snapshot or commit attempt. On false, return consumed.
  Because this route open-codes its parser insert/replace path instead of
  calling `commit_current_input()`, add a local
  `CommitAttemptState before; capture_commit_attempt_state(&before);`
  after the tutorial precheck and before the normal commit attempts, then
  call `commit_progressed_since(&before)` after the normal route finishes
  to decide whether a real commit occurred. Do not treat
  `try_commit_any()` returning 1 as success by itself; some handlers
  consume invalid input to surface an error. Call
  `tutorial_advance_after_successful_commit()` only on a proven success
  path. Do not call it from the generic tail before `return 1`, because
  parse/capacity failures also reach that tail.
- `src/editor/input.c:1101-1116` (`handle_enter_key_route`) — run
  `tutorial_precheck_current_input()` before `commit_current_input(1)`;
  capture the returned `CommitResult`, then call
  `tutorial_advance_if_commit_ok(result)`. This keeps Enter behavior
  aligned with semicolon and avoids advancing on `COMMIT_REJECTED`.
- `src/repl/example_loader.c:393` (`load_example_lines`) — call `tutorial_state_reset()` as the first statement.
- `src/repl/scenes.c:520` (`repl_load_workspace`) — call
  `tutorial_state_reset()` on entry. Workspace load fills user-scene slots
  with `load_scene_file_into_slot()` and restores the live stash; it does
  not transitively pass through `repl_load_user_scene_idx()`.
- `src/repl/scenes.c:670` (`repl_load_user_scene_idx`) — same first-statement reset before `load_scene_from_slot(slot)`.
- Include `widgets/tutorial.h` in `src/editor/input.c`; include the smallest reset header needed by `src/repl/example_loader.c` and `src/repl/scenes.c` (prefer `widgets/tutorial_state.h` if reset is declared there).

Tests:
- Extend `tests/test_tutorial_runner.c`: `tutorial_start` then
  `repl_load_example(0)` → `tutorial_active()` becomes 0; `tutorial_start`
  then `repl_load_workspace(tmp_workspace_dir)` → `tutorial_active()`
  becomes 0.
- Add a wrong-input case asserting `repl_set_status` was called with a non-empty message (install a sink via `repl_set_status_sink` in the test).
- Add a rejected-parse case: precheck matches but the normal parser path
  rejects/capacity-fails; assert `step` does not advance.

Verify: With a starter tutorial loaded, typing `glEnd()` then `;` shows `"expected: glBegin(GL_TRIANGLES)"` in the status bar and does NOT commit; typing the right line commits, then a new instructional comment for step 2 appears.

### Phase 5 — Tab autofill

Goal: pressing Tab replaces the input buffer with the current step's
expected command (whole-line), without committing. The user inspects,
optionally edits, then presses `;` to commit through the Phase 4 path.
This is intentionally always-overwrite while a tutorial is active.

Modify:
- `src/editor/input.c` — locate the existing Tab key handler (`grep -n "'\\\\t'" src/editor/input.c` — should sit alongside the autocomplete-accept path). Keep `editor_input_anchor_clear()` before the tutorial branch, then insert the tutorial branch ahead of the autocomplete dispatch:
  ```c
  if (tutorial_active()) {
      const char *expected = tutorial_current_expected_text();
      if (expected) {
          ReplEditorInputState *inp = editor_state_input_mut();
          strncpy(inp->input, expected, MAX_INPUT_LEN - 1);
          inp->input[MAX_INPUT_LEN - 1] = '\0';
          inp->input_len = (int)strlen(inp->input);
          editor_cursor_pos_set(inp->input_len);
          editor_completion_clear();
          repl_set_status("Replaced input with expected tutorial command; press ; to commit");
          return 1;
      }
  }
  ```
  Autocomplete is suppressed only while a tutorial is active; outside
  tutorial mode Tab keeps its current autocomplete behavior. Clearing the
  input-selection anchor first is load-bearing: otherwise overwriting
  `inp->input` can leave a stale selection range behind.

Tests:
- Extend `tests/test_tutorial_runner.c`: after `tutorial_start(0)`,
  simulate Tab via `editor_handle_key('\t', 0, 0)`, then assert
  `editor_state_input().input` equals the step's expected text and
  `input_len` is non-zero. Then commit with `editor_handle_key(';', 0, 0)`
  and assert `step` advanced.

Verify: with a tutorial active, Tab replaces the input row with the expected command; pressing `;` commits and the next comment appears. Tab in a non-tutorial scene still triggers autocomplete.

### Phase 6 — Read-only locking of revealed comments

Goal: prevent the user from deleting or editing the tutorial's instructional comments mid-flow.

Maintain `locked_lines[64]` in `TutorialRuntimeState`. `tutorial_start`
records the first comment line; each successful
`tutorial_advance_after_successful_commit` appends the newly revealed
comment's source-line index. `tutorial_line_is_locked(idx)` is then an
O(N) lookup over a tiny array. `tutorial_guard_source_change(pos,
delete_count, insert_count)` rejects any operation that:
- deletes/replaces/toggles a locked line,
- inserts or pastes at or before any locked line, or
- runs a whole-document transformation while a tutorial is active.

This v1 guard is intentionally conservative. Users can edit the current
input row freely, but source-document mutations outside the expected
tutorial commit path are blocked so recorded line indices cannot drift.

Modify (single guard helper, called at each existing mutation site):
- `src/editor/input.c:232` (`load_line_to_input`) — add a locked-line
  early return:
  ```c
  if (tutorial_line_is_locked(idx)) {
      editor_input_clear();
      editor_cursor_pos_set(0);
      repl_set_status("Tutorial instruction is read-only");
      return;
  }
  ```
  Arrow/mouse navigation may land on tutorial comments, but their text
  should not be copied into the editable input row.
- `src/editor/input.c` — every line-deletion / row-replace / line-insert
  site (Ctrl+D, backspace/delete with a line selection, semicolon parser
  replace/insert, Enter parser replace/insert, Ctrl+/ comment toggle,
  Ctrl+L clear-all, Ctrl+\ reformat). Wrap with:
  ```c
  if (!tutorial_guard_source_change(pos, delete_count, insert_count)) {
      repl_set_status("Tutorial comment is read-only");
      return ...;
  }
  ```
- `src/editor/clipboard.c` — line-range cut/delete and line paste: same
  guard on the delete range or paste insertion point. Reject the whole
  operation if any affected locked line would be edited or any insertion
  would renumber a locked line.
- `src/editor/undo.c` — undo/redo that would roll the document back below
  the active step is hard to validate cheaply, so v1 = block undo and
  redo while a tutorial is active:
  `if (tutorial_active()) { repl_set_status("Undo disabled during tutorial"); return; }`.

Mutation-site coverage checklist:
```bash
rg -n 'repl_command_store_(insert|replace|delete|clear)|editor_buffer_(insert_line|insert_lines|replace_line|delete_range|set_line|set_count|load_lines|clear|apply_compiled_change)|delete_cmd_range|editor_clear_all_cmds\(' src/editor src/app src/widgets src/repl
```
Every user-reachable source mutation from that list must either be guarded
by `tutorial_guard_source_change`, be part of the tutorial runner's own
instruction-comment append path, or be documented as unreachable while a
tutorial is active. This is the backstop against silently corrupting
`locked_lines[]` when a mutation site is missed.

Verify: cursor on a tutorial comment line, overwrite / Ctrl+/ / backspace /
line-cut all bounce off with status text. Pasting or inserting above the
first locked comment is rejected. Cursor on the in-progress input line:
editing works normally.

### Phase 7 — Fade-in render

Goal: newly revealed comments fade in left-to-right over ~0.5 s.

Important: don't gate the per-char path on `alpha0 < 1.0`. In a left-to-right reveal, char 0 reaches alpha 1 well before later chars do, so an alpha-0 check flips back to the cheap whole-line draw mid-animation. Use a dedicated `tutorial_line_is_fading()` predicate instead.

Modify:
- `src/widgets/tutorial.h` / `.c` — add:
  ```c
  /* true iff line_idx is the current reveal target and the fade window is still open */
  int tutorial_line_is_fading(int line_idx, float now);
  ```
  Implementation: `return active && line_idx == fade_line_idx && now < fade_start_t + fade_duration;`
- `src/ui/panels.c` (`code_panel_draw_command_row`, the
  `code_panel_draw_segment(...)` call) — wrap:
  ```c
  float now = ctx->snap->anim_time;
  if (tutorial_line_is_fading(i, now)) {
      int line_len = (int)strlen(display_text);
      /* per-char loop: glColor4f(r, g, b,
         tutorial_step_fade_alpha(i, wrap_start + local_i, line_len, now))
         + glutBitmapCharacter at x = wrap_x + local_i * char_w */
  } else {
      code_panel_draw_segment(wrap_x, ctx->line_y, display_text,
                              wrap_start, wrap_len, FONT_MONO);
  }
  ```
  The fading branch must respect `wrap_start` / `wrap_len` so wrapped rows
  animate only the visible segment. Use the complete logical source-line
  display text length (`strlen(display_text)` before wrapping) as
  `line_len`, and pass absolute character indices
  `wrap_start + local_i` into `tutorial_step_fade_alpha`; this makes the
  reveal cascade span the whole source line consistently across wrapped
  continuation rows. Once the fade window closes, the predicate returns 0
  and every row takes the existing fast path. No global slowdown.
- `src/widgets/tutorial.c` — implement `tutorial_step_fade_alpha`:
  ```c
  /* returns 1.0 if line_idx != fade_line_idx or now >= start + duration */
  int safe_len = line_len > 0 ? line_len : 1;
  float per_char_window = fade_duration / safe_len;
  float t = (now - fade_start_t) - char_idx * per_char_window;
  return clamp01(t / per_char_window);
  ```
  `fade_start_t` is captured from `repl_state_variables().anim_time`, and
  render-time `now` comes from `UiRenderSnapshot.anim_time`; those are the
  same clock, so runner timestamps and renderer comparisons stay aligned.

Verify: visually run a tutorial, watch the comment animate in over ~0.5 s left-to-right. Once finished it stays at full brightness and the renderer takes the fast path on subsequent frames.

### Phase 8 — Docs, ownership audit, smoke test

Modify:
- `MODULES.md` — add `src/widgets/tutorial_state.{c,h}`, `src/widgets/tutorial.{c,h}`, `src/repl/tutorials.{c,h}` to the layered overview and the state-ownership table (`TutorialRuntimeState` is owned by the peer subsystem, not `ReplState` / `EditorState`).
- `CLAUDE.md` — add one row per new file to the **File Layout** table.
- `Makefile` — add new tutorial sources/headers to `SRCS`, `HDRS`,
  `CORE_TEST_SRCS`, and test binary lists, not just the sample link path.
- `scripts/callgraph_file_groups.json` — classify the tutorial runner and
  tutorial state as peer-widget modules, not REPL or editor owners.
- Any individual `scripts/check-*` ownership guard that hard-codes source
  file lists — update that script's list or expectation if
  `make check-state-ownership` reports the new tutorial files. There is no
  single monolithic ownership allowlist.

Run:
- `make check-state-ownership` — must pass; tutorial code owns only its
  peer storage, uses the new transient-scene/document reset boundary for
  tutorial startup, uses `repl_load_apply_line` for runner-owned
  instructional comments, and uses the editor key route for user commits.
- `make test` — full suite. The Phase 3 menu enum bump should be caught by any test asserting `GLR_MENU_COUNT`; update expectations if so.
- Manual smoke test against the verification checklist below.

## Verification

- `make test_tutorial_match` — comparator unit tests.
- `make test_tutorial_runner` — start/advance/mismatch/reset; uses
  `tests/support/repl_test_support.h` and `editor_handle_key` for user
  tutorial commits.
- `make test` — full suite, ensures menu/example/loader tests still pass.
- `make check-state-ownership` — confirms the new peer doesn't reach across
  ownership boundaries.
- Manual: `./sample`, open Tutorials menu, pick "First Triangle", verify the
  first comment fades in, mistyping shows a status message, correct typing
  reveals the next comment, F12 cycling or loading an example exits the
  tutorial.
- Manual: finish the last tutorial step; verify the status says tutorial
  complete, tutorial mode is inactive, and the previously locked tutorial
  rows can now be edited like normal source lines.
- Manual: start `./sample output.c`; tutorial state should be inactive on
  startup. This should already follow from zero-initialized state plus
  `glr_app_reset_all`, but keep it in the smoke test so future reset wiring
  changes do not regress it.

## Follow-up ideas (out of scope)

- **Token-aware match classifier.** v2 of `tutorial_match` tokenizes both
  sides; on mismatch reports whether the command keyword or a specific
  argument index is wrong, populating `kind` (`TUT_MISMATCH_COMMAND` /
  `TUT_MISMATCH_ARG`) and `arg_index` so the status message can read
  "command correct, argument 2 should be `0.5`".
- **Progressive autofill.** Successive Tab presses walk the expected line
  step by step — `glBegin` → `glBegin(` → `glBegin(GL_TRIANGLES)` → full
  line — so the user can learn syntax piece by piece instead of seeing the
  whole answer at once.
- **UI feature highlights.** Pulse / highlight named UI rects (menu items,
  the Replay button, panel regions) during a tutorial step. Needs a
  hit-rect registry and a pulsing overlay renderer.
- **Popup description panels.** Small anchored explanations for UI elements,
  shown while a tutorial step references them — reuse
  `src/ui/tabbed_overlay.c` or a single-panel variant.
