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
| `src/widgets/tutorial_state.h` / `.c` | Peer subsystem owning `TutorialRuntimeState` (active flag, tutorial idx, step, fade line + start time, last match result). Standard `_view/_mut/_capture/_restore/_reset` API mirroring `replay_state` / `variable_panel_state`. |
| `src/widgets/tutorial.h` / `.c` | Runner: `tutorial_start(idx)`, `tutorial_exit()`, `tutorial_handle_commit_attempt(const char *input)`, `tutorial_advance_after_commit()`, `tutorial_current_expected_text()`, `tutorial_step_fade_alpha(int line_idx, int char_idx, float now)`, `tutorial_line_is_locked(int line_idx)` (used by editor to enforce read-only). |
| `src/repl/tutorials.h` / `.c` | Catalog parallel to `repl/examples`: per-tutorial null-terminated `comments[]` and `expected[]` arrays + name. Query API `repl_tutorial_count/name/step_count/step_comment/step_expected`. Ship 2–3 starter tutorials (e.g., "First Triangle", "Color & Transform"). |
| `tests/test_tutorial_match.c` | Pure-function tests for the match comparator (whitespace tolerance, shape mismatch, default field values). |
| `tests/test_tutorial_runner.c` | Runner tests: start feeds step 0 comment, correct line advances, wrong line preserves status + doesn't advance, loading an example resets `tutorial_active()`, `tutorial_current_expected_text` reflects the current step, fade alpha hits 0 at start and 1 after duration. |

## Files to modify

| Path / region | Change |
|---|---|
| `src/app/glr_actions.h` (`GLR_MENU_*` enum) | Add `GLR_MENU_TUTORIALS` before `GLR_MENU_COUNT`. |
| `src/ui/menu_bar.c:21-30` | Add `MENU_TUTORIALS = GLR_MENU_TUTORIALS`, bump `NUM_MENUS` via `GLR_MENU_COUNT`, append `"Tutorials"` to `g_menu_labels`. |
| `src/ui/menu_bar.c:88-132` | Add `case MENU_TUTORIALS:` branches in `menu_item_count`, `menu_item_label` (returns `repl_tutorial_name(i)`, plus `"---"` / `"Restart Tutorial"` / `"Exit Tutorial"` trailing items when `tutorial_active()`), and `menu_item_shortcut` (NULL). |
| `src/app/glr_actions.c` (menu-click dispatch around lines 423-494) | Route `menu_id == GLR_MENU_TUTORIALS` clicks: tutorial-name → `tutorial_start(item_idx)`; Restart → `tutorial_start(current_tutorial_idx)`; Exit → `tutorial_exit()`. |
| `src/editor/input.c:1118-1203` (`handle_semicolon_commit_key_route`) and `:1101-1116` (`handle_enter_key_route`) | Before `try_commit_any()`, if `tutorial_active()`: call `tutorial_handle_commit_attempt(editor_state_input().input)`. On mismatch, `repl_set_status(result.message)`, leave the input buffer, return 1 (consumed). On match, fall through to normal commit, then call `tutorial_advance_after_commit()` which feeds the next comment via `feed_line()` (line 1315) and seeds a new fade. |
| `src/editor/input.c` (Tab key handler) | Before delegating Tab to autocomplete, if `tutorial_active()` and input is empty (or user explicitly wants the fill), replace the input buffer with `tutorial_current_expected_text()`, move cursor to end, set status `"Filled expected tutorial command; press ; to commit"`, return consumed. Autocomplete is bypassed only while a tutorial is active. |
| `src/editor/input.c` (delete/backspace/cut handlers + line-range delete in `src/editor/clipboard.c`) | Reject mutation of any line where `tutorial_line_is_locked(idx)` returns 1 with status `"Tutorial comment is read-only"`. Single guard helper, called from each existing mutation site. |
| `src/repl/example_loader.c:393-487` (`load_example_lines`, `repl_load_example`) and `src/repl/scenes.c` user-scene activation | Call `tutorial_state_reset()` on entry so loading an example or activating a user scene exits tutorial mode cleanly. |
| `src/ui/panels.c:548` (code-panel row draw) | If `tutorial_step_fade_alpha(line_idx, 0, now) < 1.0`, draw the row char-by-char with `glColor4f(r, g, b, alpha_for_char)`; otherwise keep the existing single `gl2d_draw_string` call. The fast path is the default. |
| `src/repl/core.c:822` (`repl_advance_time`) + `src/repl/state.c` | Expose `float repl_anim_time_now(void)` returning the existing `g_anim_time` accumulator so the runner can timestamp fade starts in absolute time. |
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
in `locked_lines[0..locked_line_count]`. Each successful
`tutorial_advance_after_commit()` appends the just-revealed comment's
line index. Since the user is forbidden from inserting/deleting locked
lines (Phase 6), the recorded indices stay valid for the duration of
the tutorial; promoting to dynamic re-derivation is a follow-up if
document-mutating tools (multi-line paste from clipboard) need to
shift indices upward.

## End-to-end sequence

1. User clicks Tutorials → "First Triangle". `glr_actions.c` calls
   `tutorial_start(0)`.
2. `tutorial_start` clears the document (same path examples use to load into
   an empty scene), resets `TutorialRuntimeState`, calls
   `feed_line(comments[0])` so the first comment commits through the normal
   pipeline, then records `fade_line_idx = doc_count - 1`,
   `fade_start_t = repl_anim_time_now()`.
3. Each frame, `panels.c` queries `tutorial_step_fade_alpha` for the row; if
   `< 1.0`, draws per-char with `glColor4f`. After `fade_duration` elapses
   the runner clears `fade_line_idx` and subsequent frames take the fast
   path.
4. User types and presses `;`. `handle_semicolon_commit_key_route` sees
   `tutorial_active()`, calls `tutorial_handle_commit_attempt(input)`.
   - Mismatch → `repl_set_status(result.message)`, return consumed, input
     preserved.
   - Match → fall through to normal commit; afterwards
     `tutorial_advance_after_commit()` increments `step`, feeds the next
     comment via `feed_line`, seeds the new fade. If `comments[step]==NULL`
     it calls `repl_set_status("Tutorial complete")` and clears `active`.
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
- `src/repl/core.h` — declare `float repl_anim_time_now(void);` next to `repl_advance_time`.
- `src/repl/core.c:822` (`repl_advance_time`) — add `float repl_anim_time_now(void) { return g_anim_time; }` alongside it.
- `Makefile` — append `tutorials.o`, `tutorial_state.o` to the same `OBJS` list that currently builds `replay_state.o` (`grep -n replay_state Makefile` to locate).

Verify: `make sample` builds. `nm sample | grep tutorial_state_view` shows the symbol.

### Phase 2 — Runner + match function (headless)

Goal: full runtime logic exists and is unit-tested, still not wired to UI.

Create:
- `src/widgets/tutorial.h` — public runner API:
  - `void  tutorial_start(int idx);`
  - `void  tutorial_exit(void);`
  - `int   tutorial_handle_commit_attempt(const char *input, TutorialMatchResult *out);`
  - `void  tutorial_advance_after_commit(void);`
  - `const char *tutorial_current_expected_text(void);`
  - `float tutorial_step_fade_alpha(int line_idx, int char_idx, float now);`
  - `int   tutorial_line_is_locked(int line_idx);`
  - `TutorialMatchResult tutorial_match(const char *expected, const char *got);`
- `src/widgets/tutorial.c` — implementations. `tutorial_start` clears the document, resets state, then calls `feed_line(comments[0])` (defined at `src/editor/input.c:1315`; public alias `repl_feed_line_public` at `src/editor/input.c:1388`) and records `fade_line_idx = repl_state_document_count() - 1`, `fade_start_t = repl_anim_time_now()`. `tutorial_match` v1 = whitespace-normalize-and-strcmp.
- `tests/test_tutorial_match.c` — uses `tests/support/test_harness.h`. Cases: exact match, extra inner whitespace, leading/trailing whitespace, trailing `;` tolerance, empty input → `TUT_MISMATCH_EMPTY`, token-count mismatch → `TUT_MISMATCH_SHAPE`, default `arg_index == -1`. The `TUT_MISMATCH_COMMAND` and `TUT_MISMATCH_ARG` enum values are declared but not produced by v1; tests just assert they exist so v2 can wire them in without an API break.
- `tests/test_tutorial_runner.c` — uses `tests/support/repl_test_support.h`. Cases: `tutorial_start(0)` → first comment appears at line 0 and begins with `//`; correct line via `repl_feed_line_public` advances `step`; wrong line preserves `step`; `tutorial_current_expected_text()` returns the right string for the current step; fade-alpha math (now == start → 0 for char 0, now == start + duration → 1 for last char).
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

### Phase 4 — Commit interception (match check)

Goal: typing wrong rejects with status; typing right advances and reveals next.

Modify (with exact anchors):
- `src/editor/input.c:1118-1203` (`handle_semicolon_commit_key_route`) — insert a guard between line 1121 (`if (editor_state_input().input_len > 0)`) and line 1122 (`editor_undo_push_snapshot()`):
  ```c
  if (tutorial_active()) {
      TutorialMatchResult r;
      if (!tutorial_handle_commit_attempt(editor_state_input().input, &r)) {
          repl_set_status(r.message);
          editor_completion_clear();
          return 1;
      }
  }
  ```
  At the very end of the function, just before `return 1;` at line 1200, add:
  ```c
  if (tutorial_active()) tutorial_advance_after_commit();
  ```
- `src/editor/input.c:1101-1116` (`handle_enter_key_route`) — mirror the same pre-commit guard at line 1110 before `commit_current_input(1);`.
- `src/repl/example_loader.c:393` (`load_example_lines`) — call `tutorial_state_reset()` as the first statement.
- `src/repl/scenes.c` (user-scene activation; `grep -n repl_user_scene_activate src/repl/scenes.c`) — same first-statement reset.
- Include `widgets/tutorial.h` in `src/editor/input.c`, `src/repl/example_loader.c`, `src/repl/scenes.c`.

Tests:
- Extend `tests/test_tutorial_runner.c`: `tutorial_start` then `repl_load_example(0)` → `tutorial_active()` becomes 0.
- Add a wrong-input case asserting `repl_set_status` was called with a non-empty message (install a sink via `repl_set_status_sink` in the test).

Verify: With a starter tutorial loaded, typing `glEnd()` then `;` shows `"expected: glBegin(GL_TRIANGLES)"` in the status bar and does NOT commit; typing the right line commits, then a new instructional comment for step 2 appears.

### Phase 5 — Tab autofill

Goal: pressing Tab fills the input buffer with the current step's expected command (whole-line), without committing. The user inspects, optionally edits, then presses `;` to commit through the Phase 4 path.

Modify:
- `src/editor/input.c` — locate the existing Tab key handler (`grep -n "'\\\\t'" src/editor/input.c` — should sit alongside the autocomplete-accept path). Insert a tutorial branch ahead of the autocomplete dispatch:
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
          repl_set_status("Filled expected tutorial command; press ; to commit");
          return 1;
      }
  }
  ```
  Autocomplete is suppressed only while a tutorial is active; outside tutorial mode Tab keeps its current autocomplete behavior.

Tests:
- Extend `tests/test_tutorial_runner.c`: after `tutorial_start(0)`, simulate Tab (call the same helper the key router calls), then assert `editor_state_input().input` equals the step's expected text and `input_len` is non-zero. Then call `repl_feed_line_public(input + ";")` and assert `step` advanced.

Verify: with a tutorial active, Tab fills the input row with the expected command; pressing `;` commits and the next comment appears. Tab in a non-tutorial scene still triggers autocomplete.

### Phase 6 — Read-only locking of revealed comments

Goal: prevent the user from deleting or editing the tutorial's instructional comments mid-flow.

Maintain `locked_lines[64]` in `TutorialRuntimeState`. Each successful `tutorial_advance_after_commit` appends the just-revealed comment's source-line index. `tutorial_line_is_locked(idx)` is then an O(N) lookup over a tiny array. Since Phase 6 also blocks the operations that would re-number lines (line insert/delete above the locks), the recorded indices stay valid without a notification scheme.

Modify (single guard helper, called at each existing mutation site):
- `src/editor/input.c` — every line-deletion / row-replace site (backspace at column 0 merging up, Ctrl+K, line-up replace, etc.). Wrap with:
  ```c
  if (tutorial_line_is_locked(target_line)) {
      repl_set_status("Tutorial comment is read-only");
      return ...;
  }
  ```
- `src/editor/clipboard.c` — line-range cut/delete: same guard on the range (reject the whole operation if any line in the range is locked).
- `src/editor/undo.c` — undo that would roll the document back below the active step is hard to validate cheaply, so v1 = block undo while a tutorial is active: `if (tutorial_active()) { repl_set_status("Undo disabled during tutorial"); return; }`.

Verify: cursor on a tutorial comment line, backspace / Ctrl+K / line-cut all bounce off with status text. Cursor on the in-progress input line: editing works normally.

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
- `src/ui/panels.c:548` (the `gl2d_draw_string(text_x, line_y, text, FONT_MONO)` call for command rows) — wrap:
  ```c
  float now = repl_anim_time_now();
  if (tutorial_line_is_fading(ctx->row_idx, now)) {
      /* per-char loop: glColor4f(r, g, b, tutorial_step_fade_alpha(row, i, now))
         + glutBitmapCharacter at x = text_x + i * char_w */
  } else {
      gl2d_draw_string((float)ctx->text_x, (float)ctx->line_y, text, FONT_MONO);
  }
  ```
  Once the fade window closes, the runner clears `fade_line_idx`, the predicate returns 0, and every row takes the existing single-call path. No global slowdown.
- `src/widgets/tutorial.c` — implement `tutorial_step_fade_alpha`:
  ```c
  /* returns 1.0 if line_idx != fade_line_idx or now >= start + duration */
  float t = (now - fade_start_t) - char_idx * (fade_duration / line_len);
  return clamp01(t / per_char_window);
  ```

Verify: visually run a tutorial, watch the comment animate in over ~0.5 s left-to-right. Once finished it stays at full brightness and the renderer takes the fast path on subsequent frames.

### Phase 8 — Docs, ownership audit, smoke test

Modify:
- `MODULES.md` — add `src/widgets/tutorial_state.{c,h}`, `src/widgets/tutorial.{c,h}`, `src/repl/tutorials.{c,h}` to the layered overview and the state-ownership table (`TutorialRuntimeState` is owned by the peer subsystem, not `ReplState` / `EditorState`).
- `CLAUDE.md` — add one row per new file to the **File Layout** table.

Run:
- `make check-state-ownership` — must pass; tutorial code reads `repl_state_*` views but only mutates its own peer storage and goes through `feed_line` / `repl_set_status` for cross-module effects.
- `make test` — full suite. The Phase 3 menu enum bump should be caught by any test asserting `GLR_MENU_COUNT`; update expectations if so.
- Manual smoke test against the verification checklist below.

## Verification

- `make test_tutorial_match` — comparator unit tests.
- `make test_tutorial_runner` — start/advance/mismatch/reset; uses
  `tests/support/repl_test_support.h` and `repl_feed_line_public`.
- `make test` — full suite, ensures menu/example/loader tests still pass.
- `make check-state-ownership` — confirms the new peer doesn't reach across
  ownership boundaries.
- Manual: `./sample`, open Tutorials menu, pick "First Triangle", verify the
  first comment fades in, mistyping shows a status message, correct typing
  reveals the next comment, F12 cycling or loading an example exits the
  tutorial.

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
