# Tutorial System Extended Placement

## Context

The current tutorial runner is append-only. Each tutorial step emits a locked
`//` instruction comment at the end of the source document, waits for the user
to type the expected command at the trailing edit row, then appends the next
instruction comment.

That is enough for linear examples, but it cannot teach workflows where the
user first builds something, then goes back and inserts a setup command before
existing geometry. Example:

1. Draw a triangle.
2. Observe that it renders without depth testing.
3. Insert `glEnable(GL_DEPTH_TEST)` before the triangle batch.

This plan extends tutorial steps so a built-in tutorial can specify an exact
source line where the next instruction should be inserted. This is not an
anchor-to-prior-step system. The catalog supplies a concrete line number, and
the runner interprets that number against the current source document at the
moment the step begins.

## Goals

- Allow a tutorial step to insert its instruction comment above an arbitrary
  specified source line, not only at the end of the document.
- Keep user commits constrained to the step's intended insertion line.
- Preserve read-only behavior for revealed tutorial instruction comments.
- Keep the matcher API and current Tab autofill behavior intact.
- Keep existing append-only tutorials working with minimal catalog churn.
- Add tests that prove line targets, lock shifting, navigation commits, and
  completion still behave correctly.

## Non-Goals

- No anchor-step placement in this version. A step does not say "before step 2"
  or "after the last `glBegin`"; it says "line N".
- No free-form tutorial scripting language.
- No UI picker for line targets. Placement is authored in the built-in tutorial
  catalog.
- No change to v1 matching. Matching remains whitespace-tolerant exact text.
- No support for user edits that move tutorial comments around while a tutorial
  is active.

## Line Target Semantics

Line targets are zero-based source-document line indices.

A targeted step uses this sequence:

1. Read `target_line` from the catalog.
2. Interpret it against the current source document before this step emits
   anything.
3. Insert the step's locked instruction comment at `target_line`.
4. Set the expected user commit line to `target_line + 1`.
5. Put the editor cursor on that expected commit line.
6. If `target_line + 1` is before the end of the document, put the editor in
   insert mode so the expected command inserts before the existing line instead
   of replacing it.

For example, after a triangle tutorial has produced:

```c
// Build the smallest filled shape: open a GL_TRIANGLES batch.
glBegin(GL_TRIANGLES);
// Place the first vertex near the top; this becomes the triangle tip.
glVertex3f(0, 0.8, 0);
...
```

A later step can target line `1`. The runner inserts the new instruction at
line `1`; the user's expected command lands at line `2`; the original
`glBegin(GL_TRIANGLES);` shifts down.

`target_line == document_count` is allowed and is equivalent to appending.
Append-only steps should still use the explicit append placement for clarity.
Targets outside `[0, document_count]` are catalog errors. The runner should not
partially mutate the document when a target is invalid. Concretely:
`tutorial_step_instruction_line` returns false → `tutorial_start` /
`tutorial_advance_after_successful_commit` set a status message, call
`tutorial_state_reset()`, and return. No instruction comment is emitted, no
locked line is recorded, and the editor is left in its prior state.

Important: line targets are source-document lines, not wrapped code-panel rows.
Later line targets are authored against the document shape that exists when
that later step begins, after all prior instruction comments and user commands
have been inserted.

### `repl_load_apply_line` Contract Drift

The current loader (`src/repl/load.c`) is documented in `src/repl/load.h:42-46`
as append-only: callers are required to set `repl_state_edit_line` to
`repl_state_document_count()` before calling. In practice the plain-command
path computes `insert_idx = min(edit_line, document_count)`, so setting
`edit_line` to a mid-document position already produces a mid-document insert
for plain GL commands and `// comment` lines (which both flow through that
path). The structured-block validators in the same function honor
`change.pos` from the compile result, which today is set from `edit_line`
too.

Two acceptable resolutions:

1. **Widen the contract.** Update the comment in `src/repl/load.h:42-46` to
   say `repl_state_edit_line` must be in `[0, document_count]` and the line
   will be inserted at that index. This matches actual behavior and unlocks
   the tutorial use case without new API surface.
2. **Add a focused helper.** Introduce
   `repl_load_apply_line_at(const char *line, int target_idx, char *err,
   int err_size)` that explicitly sets edit_line internally, then delegates.
   Existing append callers stay on `repl_load_apply_line`; tutorial emission
   uses the new helper.

Option (1) is cheaper and the actual behavior is already what the tutorial
runner needs. Option (2) is sturdier against future contract tightening.
Pick (1) for v1; revisit if `repl_load_apply_line` ever grows an internal
assertion that `edit_line == document_count`.

## Data Model

Replace the parallel `comments[]` / `expected[]` arrays with explicit step
records.

```c
typedef enum {
    TUTORIAL_STEP_APPEND = 0,
    TUTORIAL_STEP_LINE,
} TutorialStepPlacementKind;

typedef struct {
    const char *comment;
    const char *expected;
    TutorialStepPlacementKind placement;
    int target_line;
} TutorialStep;

typedef struct {
    const char *name;
    const TutorialStep *steps;
} TutorialEntry;
```

Conventions:

- `comment` begins with `//`.
- `expected` has no trailing `;`, matching the current catalog.
- `placement == TUTORIAL_STEP_APPEND` ignores `target_line`.
- `placement == TUTORIAL_STEP_LINE` uses `target_line` as described above.
- The steps array terminates with `{ NULL, NULL, TUTORIAL_STEP_APPEND, 0 }`.

Keep the current query API working:

- `repl_tutorial_step_count`
- `repl_tutorial_step_comment`
- `repl_tutorial_step_expected`

Add placement queries:

```c
TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx);
int repl_tutorial_step_target_line(int idx, int step_idx);
```

Existing tutorials can migrate mechanically. To keep the migration readable,
introduce file-local convenience macros in `src/repl/tutorials.c`:

```c
#define STEP_APPEND(c, e)   { (c), (e), TUTORIAL_STEP_APPEND, 0 }
#define STEP_AT(c, e, line) { (c), (e), TUTORIAL_STEP_LINE,   (line) }
```

The 11-step "Color & Transform" tutorial then stays compact:

```c
STEP_APPEND("// Save the current matrix ...", "glPushMatrix()"),
STEP_APPEND("// Set the drawing color ...",   "glColor3f(0.2, 0.8, 1)"),
...
```

Without macros each row needs a literal `TUTORIAL_STEP_APPEND, 0` trailer,
which makes the catalog harder to scan and easier to mis-edit. Macros are
file-local — no public API exposure.

## Runtime State

Extend `TutorialRuntimeState` with the current expected insertion site and the
document count captured before the expected user commit.

```c
int expected_commit_line;
int pending_doc_count_before_commit;
int pending_commit_line;
```

Recommended sentinels:

- `expected_commit_line = -1` when no step is waiting for a user command.
- `pending_doc_count_before_commit = -1` when no commit is in flight.
- `pending_commit_line = -1` when no commit is in flight.

The existing `locked_lines[]` remains a list of source line indices for
revealed tutorial instruction comments. The extension needs one helper that
updates all tracked line indices when the tutorial runner or the expected user
commit inserts lines above them:

```c
static void tutorial_shift_tracked_lines_from(int pos, int delta);
```

This helper should shift:

- `locked_lines[i] >= pos`
- `fade_line_idx >= pos`
- `expected_commit_line >= pos`, if it is already set and the shift is not for
  the insertion currently defining it

For this feature, tutorial-approved changes are insert-only. The existing
guards should continue to block deletes, replacements, paste operations, undo,
redo, clear-all, and reformat while a tutorial is active.

## Runner Changes

### Instruction Emission

Change `tutorial_emit_instruction_comment` so it accepts a source insertion
line:

```c
static int tutorial_emit_instruction_comment(const char *comment,
                                             int instruction_line);
```

Flow:

1. Validate `instruction_line >= 0 && instruction_line <= document_count`.
2. Shift tracked tutorial lines at or after `instruction_line` by `+1`.
3. Set `repl_state_edit_line` to `instruction_line`.
4. Clear input transients.
5. Clear insert mode before calling `repl_load_apply_line`.
6. Call `repl_load_apply_line(comment, ...)`.
7. Mark flat and normals dirty.
8. Record `fade_line_idx = instruction_line`.
9. Append `instruction_line` to `locked_lines[]`.
10. Set `expected_commit_line = instruction_line + 1`.
11. Move the editor to `expected_commit_line`.
12. Set insert mode to `expected_commit_line < document_count`.

The current append path falls out naturally:

- Before emit: `instruction_line == old_document_count`.
- After emit: `expected_commit_line == new_document_count`.
- Insert mode remains off because the user command lands at the trailing row.

### Step Placement

Add a helper:

```c
static int tutorial_step_instruction_line(int tutorial_idx, int step,
                                          int *out_line);
```

Rules:

- Append placement returns `document_count`.
- Line placement returns `target_line` if `0 <= target_line <= document_count`.
- Invalid placement sets a status message such as
  `"Tutorial step target is out of range"` and returns false.

`tutorial_start` and `tutorial_advance_after_successful_commit` should use this
helper before emitting each instruction comment.

### Commit Precheck

Replace the current append-only check in `tutorial_precheck_current_input`.

Current behavior rejects every tutorial commit unless:

```c
repl_state_edit_line() >= repl_state_document_count()
```

New behavior should require the cursor to be on the current step's expected
commit line:

```c
if (repl_state_edit_line() != tutorial_expected_commit_line()) {
    repl_set_status("Move cursor to the tutorial insertion line");
    editor_completion_clear();
    return 0;
}
```

If `expected_commit_line < document_count`, also require insert mode:

```c
if (expected_commit_line < repl_state_document_count() &&
    !editor_insert_mode()) {
    repl_set_status("Tutorial step must insert at the highlighted line");
    editor_completion_clear();
    return 0;
}
```

Then run the existing matcher. Mismatch behavior stays unchanged: status shows
`expected: ...`, the typed input is preserved, and the command is not applied.

Empty-input special case: when the input buffer is empty and the commit
route is a navigation auto-commit (not a `;`/Enter direct intent), the
precheck must return 0 **without updating the status**. Otherwise the
navigation that initially places the cursor on `expected_commit_line` —
with empty input and insert mode on — would immediately spam `expected:
…` even though the user hasn't typed yet. The simplest implementation
splits the precheck: keep the existing `expected: …` for non-empty input
and surface a silent rejection for empty input. Direct `;`/Enter on
empty input can keep the existing "expected: …" hint by checking the
key path explicitly, or by always-rejecting silently and accepting that
Enter on empty input becomes a no-op (the user has Tab and the visible
shadow text as discovery affordances).

### Commit Success Bookkeeping

The "begin" call is paired with the precheck, not a separate editor route
hook: fold it into `tutorial_precheck_current_input()` so it fires exactly
when the matcher passes and never on rejection:

```c
static int tutorial_precheck_current_input(void) {
    ...
    if (!tutorial_handle_commit_attempt(input, &result)) {
        repl_set_status(result.message);
        editor_completion_clear();
        return 0;
    }
    tutorial_begin_expected_commit_attempt();
    return 1;
}
```

That helper stores:

- `pending_doc_count_before_commit = repl_state_document_count()`
- `pending_commit_line = expected_commit_line`
- `allow_expected_insert = 1`

After a real successful commit, before advancing to the next step, call:

```c
tutorial_note_expected_commit_applied();
```

It computes:

```c
int delta = repl_state_document_count() - pending_doc_count_before_commit;
```

If `delta > 0`, shift tracked tutorial lines at or after
`pending_commit_line` by `delta`. Then clear the pending fields and the
`allow_expected_insert` flag.

This keeps locked instruction comments correct when the expected command is
inserted in the middle of the document. It also handles future expected lines
that compile to more than one source row (e.g. `func0() { ... }` block
commits that grow the document by N rows), although starter tutorials should
continue to use one-line expected commands.

The existing `tutorial_advance_if_commit_ok(result)` wrapper is the right place
to sequence this, with explicit symmetry for rejected commits:

```c
static void tutorial_advance_if_commit_ok(CommitResult result) {
    if (!tutorial_active()) return;
    if (result == COMMIT_OK) {
        tutorial_note_expected_commit_applied();
        tutorial_advance_after_successful_commit();
    } else {
        /* Precheck called _begin; if the editor commit failed (parse
         * error, capacity, etc.) we must clear the pending fields and
         * the allow flag so the next attempt starts fresh. */
        tutorial_cancel_expected_commit_attempt();
    }
}
```

`tutorial_cancel_expected_commit_attempt()` clears the pending fields and the
`allow_expected_insert` flag without shifting any tracked lines.

## Guard Changes

The current `tutorial_guard_source_change(pos, delete_count, insert_count)` is
conservative: any insertion at or before a locked line is rejected. That must
remain true for ordinary user operations, but the current tutorial step needs
one narrow exception.

Allow exactly this case:

```c
delete_count == 0 &&
insert_count > 0 &&
pos == expected_commit_line &&
tutorial input has already matched the current expected command
```

Implementation options:

1. Add a runtime flag set by `tutorial_begin_expected_commit_attempt()`:
   `allow_expected_insert = 1`.
2. Make `tutorial_guard_source_change` allow the current expected insert only
   while that flag is set.
3. Clear the flag after commit success or rejection.

This keeps paste, Ctrl+/ comment toggle, Ctrl+D, reformat, clear-all, undo, and
redo blocked by the existing call sites. It also avoids opening a generic
"insert anywhere above tutorial comments" hole.

The guard should still reject:

- Any delete or replace touching a locked instruction comment.
- Any insertion not associated with the current matched tutorial step.
- Any insertion at the expected line before the input has matched.
- Whole-document transformations while active.

## Editor Behavior

When a targeted step starts:

- The cursor moves to the expected command line.
- The input buffer is empty.
- The status still reads `Tutorial: step X/Y`.
- Tab fills the current expected command exactly as it does today.
- Pressing `;` or Enter commits only if the cursor remains on the expected
  insertion line.
- If the user navigates away with non-empty matching input, the existing
  navigation-commit path may commit it, but only if the cursor was still on the
  expected insertion line when navigation began.
- If the user navigates away with empty input, no source change should occur.
  If insert mode currently makes navigation treat empty input as a pending
  commit, adjust `current_input_needs_navigation_commit()` or the tutorial
  precheck so empty targeted insert rows do not produce noisy status messages.

Locked instruction comments should behave as they do today:

- Navigation may land on them.
- `load_line_to_input` clears input and says the instruction is read-only.
- Editing, deleting, toggling, cutting, pasting over, undo, redo, clear-all,
  and reformat are blocked while active.

## Example Catalog Shape

A tutorial can first draw the triangle, then insert depth testing before the
triangle batch:

```c
static const TutorialStep g_tutorial_depth_triangle_steps[] = {
    { "// Start the triangle batch.", "glBegin(GL_TRIANGLES)",
      TUTORIAL_STEP_APPEND, 0 },
    { "// Add the top vertex.", "glVertex3f(0, 0.8, 0)",
      TUTORIAL_STEP_APPEND, 0 },
    { "// Add the lower-left vertex.", "glVertex3f(-0.8, -0.6, 0)",
      TUTORIAL_STEP_APPEND, 0 },
    { "// Add the lower-right vertex.", "glVertex3f(0.8, -0.6, 0)",
      TUTORIAL_STEP_APPEND, 0 },
    { "// Close the triangle batch.", "glEnd()",
      TUTORIAL_STEP_APPEND, 0 },

    /*
     * At this point line 1 is the glBegin command. Insert a new
     * instruction above it, then place glEnable directly under the new
     * instruction and before the original glBegin.
     */
    { "// Enable depth testing before the triangle is submitted.",
      "glEnable(GL_DEPTH_TEST)", TUTORIAL_STEP_LINE, 1 },

    { NULL, NULL, TUTORIAL_STEP_APPEND, 0 },
};
```

The `target_line = 1` value is authored against the current document at the
moment that step begins.

## Implementation Phases

### Phase 1 - Catalog Step Records

Goal: migrate the catalog shape without changing behavior.

Modify:

- `src/repl/tutorials.h`
  - Add `TutorialStepPlacementKind`.
  - Add `TutorialStep`.
  - Change `TutorialEntry` to hold `const TutorialStep *steps`.
  - Add `repl_tutorial_step_placement` and
    `repl_tutorial_step_target_line`.
- `src/repl/tutorials.c`
  - Convert existing tutorials to `TutorialStep` arrays.
  - Mark every existing step as `TUTORIAL_STEP_APPEND`.
  - Preserve all existing tutorial names and expected command strings.
- `tests/test_tutorial_runner.c`
  - Keep existing catalog assertions.
  - Add assertions that current starter steps report append placement.

Verify:

- `make test_tutorial_runner`
- `build/release/test_tutorial_runner`

### Phase 2 - Targeted Instruction Emission

Goal: the runner can reveal an instruction comment at an arbitrary source line,
but all catalog steps still append.

Modify:

- `src/widgets/tutorial_state.h`
  - Add `expected_commit_line`.
  - Add pending commit bookkeeping fields.
  - Add the expected-insert allow flag if using the flag-based guard.
- `src/widgets/tutorial_state.c`
  - Initialize all new line fields to `-1` in reset.
- `src/widgets/tutorial.c`
  - Add `tutorial_shift_tracked_lines_from`.
  - Change `tutorial_emit_instruction_comment` to take an
    `instruction_line`.
  - Add `tutorial_step_instruction_line`.
  - Have `tutorial_start` and `tutorial_advance_after_successful_commit`
    compute the instruction line before emitting.
  - After emitting, set `expected_commit_line` and place the editor on it.

Tests:

- Start an append tutorial and assert the first expected commit line is the
  trailing row.
- Add a real third tutorial with a targeted step rather than a test-only
  entry. The user-visible tutorial in Phase 4 already needs to ship; landing
  it in Phase 2 means both Phase 2's code path and Phase 4's dogfood get
  covered by the same fixture instead of carrying a synthetic entry that
  later has to be deleted. The Phase 4 section below stays as the "ship a
  worked targeted tutorial" milestone — Phase 2 just brings forward enough
  of it to exercise the runner.
- Advance to the targeted step and assert the new instruction appears at the
  specified line, not the end.
- Assert previously locked instruction comments after the insertion were
  shifted and remain locked.

Verify:

- `make test_tutorial_runner`

### Phase 3 - Commit Precheck and Guard Exception

Goal: a matching expected command can be inserted at the targeted line, while
ordinary user edits above locked comments remain blocked.

Modify:

- `src/editor/input.c`
  - Replace the append-only tutorial precheck with an expected-line precheck.
  - Begin tutorial expected-commit bookkeeping after a match and before the
    normal commit path runs.
  - On `COMMIT_OK`, note the applied tutorial insert before advancing.
  - On rejection, clear pending tutorial commit bookkeeping.
- `src/widgets/tutorial.h` / `.c`
  - Add narrow helpers for expected commit bookkeeping:
    - `tutorial_expected_commit_line`
    - `tutorial_begin_expected_commit_attempt`
    - `tutorial_note_expected_commit_applied`
    - `tutorial_cancel_expected_commit_attempt`
  - Update `tutorial_guard_source_change` to allow only the current matched
    expected insert.

Tests:

- Targeted step with correct input inserts the command at the expected middle
  line and advances.
- Targeted step with wrong input preserves input, does not insert, and does
  not advance.
- Pasting or manually inserting above a locked tutorial comment is still
  rejected when not part of the matched expected commit.
- Ctrl+/ and Ctrl+D on locked comments still reject.
- Navigation commit with matching input at the expected line advances.
- Navigation commit with matching input from any other line rejects.

Verify:

- `make test_tutorial_runner`
- `make test_ui USE_GL_STUBS=1` if fade-line tracking is touched by the test
  path.

### Phase 4 - Add a Worked Targeted Tutorial

Goal: ship a user-visible tutorial that uses specified-line insertion.

Modify:

- `src/repl/tutorials.c`
  - Add or revise a tutorial to demonstrate "draw first, then insert setup
    before the batch".
  - Keep comments written as teaching prompts, not test assertions.
  - Use a targeted step such as `TUTORIAL_STEP_LINE, 1` once the triangle batch
    exists.
- `tests/test_tutorial_runner.c`
  - Pin the new tutorial name, step count, first expected command, and targeted
    step placement.
  - Walk the full tutorial and assert the final source order has the inserted
    setup command before the original batch command.

Verify:

- `make test_tutorial_runner`
- Manual `./sample`: run the tutorial, confirm the cursor jumps back to the
  targeted insertion line and the command lands before the existing line.

### Phase 5 - Ownership and Regression Sweep

Goal: ensure the new mid-document tutorial path does not bypass mutation
guards or state ownership rules.

Run the mutation-site checklist from the original tutorial plan:

```bash
rg -n 'repl_command_store_(insert|replace|delete|clear)|editor_buffer_(insert_line|insert_lines|replace_line|delete_range|set_line|set_count|load_lines|clear|apply_compiled_change)|delete_cmd_range|repl_clear_all_cmds\(' src/editor src/app src/widgets src/repl
```

Every user-reachable source mutation must be one of:

- Guarded by `tutorial_guard_source_change`.
- Part of the tutorial runner's own instruction-comment emission.
- Part of the current matched expected-command insertion.
- Documented as unreachable while a tutorial is active.

Run:

- `make test_tutorial_runner`
- `make test`
- `make check-state-ownership`

## Edge Cases

- **Target is a locked instruction line:** allowed for the runner. The new
  instruction inserts above that locked line. Ordinary user insertions there
  remain blocked.
- **Target is the end of the document:** allowed. Equivalent to append.
- **Target is out of range:** reject without partial mutation and set a status
  message. Since tutorials are built in, this should be covered by tests.
- **Commit produces multiple source rows:** shift tracked lines by the actual
  document-count delta. Starter tutorials should still use one-line commands.
- **User navigates away before typing:** should not mutate source. Avoid noisy
  "expected: ..." status if possible when the input buffer is empty.
- **User navigates to another editable line and types the expected text:** reject
  because the edit line is not `expected_commit_line`.
- **Tutorial completion:** clear active state and locks. The source document
  remains, and all tutorial comments become editable, same as the current
  completion behavior.

## Verification Checklist

- Existing append-only tutorials still pass unchanged.
- A targeted step inserts its instruction comment at the requested current
  source line.
- The expected command lands immediately below that instruction comment.
- Existing lines at and below the target shift down.
- Previously locked instruction comments remain locked after shifting.
- Manual edits above locked comments are still blocked.
- Wrong input at a targeted step does not insert anything.
- Correct input at the wrong line does not insert anything.
- Tab autofill still fills the expected command for targeted steps.
- Fade applies to the newly inserted instruction line, even when it appears in
  the middle of the document.
- Completing the tutorial clears locks and leaves the final document editable.
