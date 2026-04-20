# Auto-Commit Edited REPL Lines On Navigation

## Summary
- Refactor the current Enter commit branch in [repl_editor.c](/Users/drew/src/code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_editor.c) into a shared helper that can be used by both Enter and line navigation.
- When Up/Down or a code-panel mouse click moves from one source line to another, first try to commit the current edited line if it differs from the committed source.
- If that auto-commit fails, restore the previous committed state for that line, keep the parser error/status visible, and still complete the navigation to the requested line.

## Key Changes
- Add a small commit-result enum in `repl_editor.c`, for example `COMMIT_UNCHANGED`, `COMMIT_OK`, `COMMIT_REJECTED`.
- Extract the Enter commit logic into a helper that preserves the existing behavior for:
  - insert mode commits
  - overwrite-line commits
  - append-at-end commits
  - block headers and close braces
  - float declarations and variable assignments
  - ordinary GL/GLU/GLUT command parsing
- Add a navigation-specific wrapper around that helper:
  - only runs when the target line differs from the current line
  - treats empty or unchanged input as no-op
  - saves a local snapshot before attempting commit
  - on success, records undo normally and continues navigation
  - on rejection, restores the snapshot, clears insertion/autocomplete state, then navigates to the requested line
- Change `navigate_to_line()` so all keyboard and mouse navigation paths inherit the behavior automatically.
- Keep autocomplete Up/Down behavior unchanged: when autocomplete is open, Up/Down only changes the selected completion and does not auto-commit.
- Keep Page Up/Page Down unchanged: they scroll the panel and do not commit.
- Code-panel clicks and drags will use the same navigation path, so a click away from a modified valid line commits it before loading the clicked line.

## Tests
- Add focused coverage in [test_repl_editor.c](/Users/drew/src/code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/test_repl_editor.c):
  - editing an existing command, pressing Down commits the valid edit and moves to the next line
  - editing an existing command to invalid text, pressing Down restores the old command, leaves command count unchanged, moves to the next line, and preserves an error status
  - typing a valid new append-at-end line, pressing Up commits it and moves upward
  - typing an invalid append-at-end line, pressing Up discards it and moves upward without adding a command
  - mouse click from a modified valid line commits before selecting the clicked line
  - autocomplete Up/Down still only changes completion selection
- Run focused validation:
  - `make test_repl_editor`
  - `make test_repl_core_commit`
- If those pass, run broader local regression:
  - `make test`

## Assumptions
- “Revert if invalid” means the invalid text is discarded and the cursor still moves to the requested destination line.
- The existing parser/status message should remain visible after an invalid auto-commit so the user can tell why the edit was rejected.
- Only actual line changes trigger auto-commit; clicking within the same line to move the text cursor should not commit.

