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
- Each step is a `(comment, expected)` pair. Match is whitespace-tolerant
  exact-string in v1; the match result struct already carries fields for
  future "command-right, arg-wrong" granularity and Tab-autofill seams.
- Mismatch surfaces via `repl_set_status` only; typed input is preserved
  so the user can edit and retry.
- Revealed comments are read-only while the tutorial is active.
- Completing the last step shows a status message and leaves the buffer in
  place; no auto-promotion to a user scene.
- Newly revealed comments fade in left-to-right (~0.5 s total) for a subtle
  cue. After the fade window the row reverts to the cheap whole-line draw.

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
| `src/ui/menu_bar.c:88-132` | Add `case MENU_TUTORIALS:` branches in `menu_item_count`, `menu_item_label` (returns `repl_tutorial_name(i)`), and `menu_item_shortcut` (NULL). |
| `src/app/glr_actions.c` (menu-click dispatch around lines 423-494) | Route `menu_id == GLR_MENU_TUTORIALS` clicks to `tutorial_start(item_idx)`. |
| `src/editor/input.c:1118-1203` (`handle_semicolon_commit_key_route`) and `:1101-1116` (`handle_enter_key_route`) | Before `try_commit_any()`, if `tutorial_active()`: call `tutorial_handle_commit_attempt(editor_state_input().input)`. On mismatch, `repl_set_status(result.message)`, leave the input buffer, return 1 (consumed). On match, fall through to normal commit, then call `tutorial_advance_after_commit()` which feeds the next comment via `feed_line()` (line 1315) and seeds a new fade. |
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
- `TUT_MISMATCH_SHAPE` — token count differs (v1 catch-all)
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
- `int fade_line_idx` (-1 when no row is fading)
- `float fade_start_t`, `fade_duration` (0.5 s)
- `TutorialMatchResult last_result`

V1 `tutorial_match`: whitespace-normalize both strings, strcmp → `OK` or
`SHAPE`. The struct shape lets v2 add token-walk classification without
touching call sites.

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
  argument index is wrong, populating `kind` / `arg_index` so the status
  message can read "command correct, argument 2 should be `0.5`".
- **Tab/Enter autofill.** With `tutorial_current_expected_text()` already in
  the runner API, an input.c Tab handler can fill the input buffer with the
  expected line for browsing tutorials quickly.
- **Feature highlights.** After tutorial completion (or as standalone
  "tours"), highlight a UI element (Replay button, a menu item, etc.) with
  a pulsing outline. Same fade infrastructure plus a hit-rect registry in
  the menu bar / pin buttons.
- **Popup tooltips.** Tutorial steps could optionally trigger a small
  floating panel anchored to a UI element with longer-form explanation —
  reuse `src/ui/tabbed_overlay.c` for the renderer, drive visibility from
  the tutorial step.
