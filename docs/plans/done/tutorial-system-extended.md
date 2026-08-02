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

This plan extends tutorial steps so a built-in tutorial can place a later step
relative to a named earlier step. The catalog assigns an optional label to a
step's committed command, and another step can target that label. The runner
resolves the label to the command's current source line at the moment the later
step begins.

## Goals

- Allow a tutorial step to insert its instruction comment above the source line
  committed by a labeled earlier tutorial step.
- Keep user commits constrained to the step's intended insertion line.
- Preserve read-only behavior for revealed tutorial instruction comments.
- Keep the matcher API and current Tab autofill behavior intact.
- Keep existing append-only tutorials working with minimal catalog churn.
- Validate tutorial catalogs so labels are unique and label-targeted steps
  cannot reference missing labels.
- Add tests that prove label targeting, lock shifting, navigation commits, and
  completion still behave correctly.

## Non-Goals

- No raw source-line-number placement in the catalog.
- No free-form tutorial scripting language.
- No UI picker for labels or insertion targets. Placement is authored in the
  built-in tutorial catalog.
- No change to v1 matching. Matching remains whitespace-tolerant exact text.
- No support for user edits that move tutorial comments around while a tutorial
  is active.

## Label Target Semantics

Each tutorial step may define a non-empty `label`. That label names the source
line created when the user successfully commits that step's `expected` command,
not the locked instruction comment line.

Labels are optional:

- `label == NULL` or `label[0] == '\0'` means unlabeled.
- Non-empty labels must be unique within a tutorial.
- A label becomes resolvable only after its step has committed successfully.

A label-targeted step uses this sequence:

1. Read `target_label` from the catalog.
2. Resolve `target_label` to the current source line of the earlier committed
   step with that label.
3. Insert the new locked instruction comment at that source line.
4. Set the expected user commit line to the line below the new instruction
   comment.
5. Put the editor cursor on that expected commit line.
6. Put the editor in insert mode so the expected command inserts before the
   original target command instead of replacing it.

For example, after a triangle tutorial has produced:

```c
// Build the smallest filled shape: open a GL_TRIANGLES batch.
glBegin(GL_TRIANGLES);
// Place the first vertex near the top; this becomes the triangle tip.
glVertex3f(0, 0.8, 0);
...
```

If the `glBegin(GL_TRIANGLES)` step was labeled `"triangle_begin"`, a later
step can target `"triangle_begin"`. The runner inserts the new instruction
above the current `glBegin` line; the user's expected command lands directly
under that instruction; the original `glBegin(GL_TRIANGLES);` shifts down.

Append-only steps still use explicit append placement. Label-targeted steps
must use a non-null, non-empty `target_label` that refers to an earlier labeled
step. Missing labels, duplicate labels, and forward references are catalog
errors and should be rejected before the tutorial mutates the document.

Important: labels resolve to source-document lines, not wrapped code-panel
rows. The line for a label is tracked by the tutorial runtime and shifts as
tutorial-approved insertions move source rows.

### `repl_load_apply_line` Contract Widening (prerequisite)

Every later phase depends on the loader accepting a mid-document edit line.
The current loader (`src/repl/load.c`) is documented in `src/repl/load.h:42-46`
as append-only: callers are required to set `repl_state_edit_line` to
`repl_state_document_count()` before calling. In practice the plain-command
path computes `insert_idx = min(edit_line, document_count)`, so setting
`edit_line` to a mid-document position already produces a mid-document insert
for plain GL commands and `// comment` lines (which both flow through that
path). The structured-block validators in the same function honor
`change.pos` from the compile result, which today is set from `edit_line`
too.

Resolution: widen the documented contract. Update the comment in
`src/repl/load.h:42-46` to say `repl_state_edit_line` must be in
`[0, document_count]` and the line will be inserted at that index. This
matches actual behavior and unlocks the tutorial use case without new API
surface. Land this as **Phase 0** below before any tutorial code change, with
an assertion-test that pins the new contract.

(A focused `repl_load_apply_line_at(line, target_idx, err, err_size)` helper
was considered as a sturdier alternative; rejected for v1 as needless API
surface given actual behavior already matches the widened contract. Revisit
only if `repl_load_apply_line` ever grows an internal assertion that
`edit_line == document_count`.)

## Data Model

Replace the parallel `comments[]` / `expected[]` arrays with explicit step
records.

```c
typedef enum {
    TUTORIAL_STEP_APPEND = 0,
    TUTORIAL_STEP_LABEL,
} TutorialStepPlacementKind;

typedef struct {
    const char *label;
    const char *comment;
    const char *expected;
    TutorialStepPlacementKind placement;
    const char *target_label;
} TutorialStep;

typedef struct {
    const char *name;
    const TutorialStep *steps;
} TutorialEntry;
```

Conventions:

- `label` is optional. `NULL` and `""` mean unlabeled.
- `comment` begins with `//`.
- `expected` has no trailing `;`, matching the current catalog.
- `placement == TUTORIAL_STEP_APPEND` ignores `target_label`, which should be
  `NULL` or empty.
- `placement == TUTORIAL_STEP_LABEL` requires a non-null, non-empty
  `target_label` that names an earlier step in the same tutorial.
- The steps array terminates with
  `{ NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL }`.

Keep the current query API working:

- `repl_tutorial_step_count`
- `repl_tutorial_step_comment`
- `repl_tutorial_step_expected`

Add placement and label queries:

```c
TutorialStepPlacementKind repl_tutorial_step_placement(int idx, int step_idx);
const char *repl_tutorial_step_label(int idx, int step_idx);
const char *repl_tutorial_step_target_label(int idx, int step_idx);
int repl_tutorial_validate(int idx, char *err, int err_size);
```

Existing tutorials can migrate mechanically. To keep the migration readable,
introduce file-local convenience macros in `src/repl/tutorials.c`:

```c
#define STEP_APPEND(label, c, e) \
    { (label), (c), (e), TUTORIAL_STEP_APPEND, NULL }

#define STEP_AT(label, c, e, target) \
    { (label), (c), (e), TUTORIAL_STEP_LABEL, (target) }
```

For unlabeled append steps, use `NULL` for the label:

```c
STEP_APPEND(NULL, "// Save the current matrix ...", "glPushMatrix()"),
STEP_APPEND(NULL, "// Set the drawing color ...",   "glColor3f(0.2, 0.8, 1)"),
```

For a targetable step, provide a label:

```c
STEP_APPEND("triangle_begin",
            "// Start the triangle batch.",
            "glBegin(GL_TRIANGLES)"),
```

For a later insertion, target that label:

```c
STEP_AT(NULL,
        "// Enable depth testing before the triangle is submitted.",
        "glEnable(GL_DEPTH_TEST)",
        "triangle_begin"),
```

Macros are file-local; no public API exposure.

## Catalog Validation

Add a simple validation pass over each tutorial. `tutorial_start(idx)` should
validate before any transient-scene reset or source mutation. Tests should also
iterate the full catalog and validate every tutorial.

Validation rules:

- Each step before the sentinel has non-null `comment` and `expected`.
- Every non-empty `label` is unique within the tutorial.
- `TUTORIAL_STEP_APPEND` has no non-empty `target_label`.
- `TUTORIAL_STEP_LABEL` has a non-null, non-empty `target_label`.
- `TUTORIAL_STEP_LABEL.target_label` refers to an earlier non-empty label in
  the same tutorial. Forward references are rejected so every label target is
  already committed when the step begins.
- Step count fits the runtime tracking arrays.

Failure behavior:

- `repl_tutorial_validate` returns 0 and writes a concise diagnostic to `err`.
- `tutorial_start` surfaces the diagnostic via `repl_set_status`, resets
  tutorial state, and returns without mutating the source document.
- `tutorial_advance_after_successful_commit` should not encounter validation
  failures if `tutorial_start` validated the full tutorial. If it cannot resolve
  a target label at runtime anyway, it sets a status message, calls
  `tutorial_state_reset()`, and returns without emitting an instruction comment
  or recording a locked line.

## Runtime State

Extend `TutorialRuntimeState` with the current expected insertion site, a
single in-flight commit-attempt record, and a per-step source-line map.

```c
typedef struct {
    int step_idx;        /* -1 ⇒ no commit attempt in flight */
    int commit_line;
    int doc_count_before;
} TutorialPendingCommit;

int expected_commit_line;
TutorialPendingCommit pending;
int committed_line_for_step[TUTORIAL_LOCKED_LINE_MAX];
```

The four `pending_*` scalars and `allow_expected_insert` flag from earlier
drafts collapsed into one struct with `step_idx == -1` as the "inactive"
sentinel. The guard-exception predicate becomes `pending.step_idx >= 0` -
it is derived, not stored independently, so the flag cannot drift out of
sync with the rest of the bookkeeping. The committed-line map reuses
`TUTORIAL_LOCKED_LINE_MAX` (the existing 64-element cap for tracked
tutorial-owned lines) rather than introducing a second cap with a different
name.

Recommended sentinels:

- `expected_commit_line = -1` when no step is waiting for a user command.
- `pending.step_idx = -1` when no commit attempt is in flight; the other
  `pending` fields are then ignored.
- `committed_line_for_step[i] = -1` until step `i` has committed.

`tutorial_state_reset` clears all new fields, including `pending.step_idx`
to `-1`.

**Immutability invariant.** `pending.commit_line` is captured at
`tutorial_begin_expected_commit_attempt` time and is logically immutable
until the paired `tutorial_note_expected_commit_applied` or
`tutorial_cancel_pending` clears the record. In particular, the shift
helper deliberately does not touch it - see below.

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
  the insertion described by `pending`
- `committed_line_for_step[i] >= pos`

It deliberately **does not** touch `pending.commit_line` - that field is the
immutable snapshot of where the in-flight commit attempt is targeting, and
the success bookkeeping below relies on reading it back unchanged after the
shift pass. If the shift helper bumped it, a label recorded for the
just-committed step would be one row too low.

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

The append path falls out naturally:

- Before emit: `instruction_line == old_document_count`.
- After emit: `expected_commit_line == new_document_count`.
- Insert mode remains off because the user command lands at the trailing row.

The label-target path also falls out naturally:

- Before emit: `instruction_line` is the current line for `target_label`.
- After the instruction comment inserts, the target label's committed line has
  shifted down by one.
- `expected_commit_line == instruction_line + 1`, so the expected command
  inserts below the new instruction and above the original target command.

### Step Placement

Add a helper:

```c
static int tutorial_step_instruction_line(int tutorial_idx, int step,
                                          int *out_line);
```

Rules:

- Append placement returns `document_count`.
- Label placement resolves `target_label` to the current
  `committed_line_for_step[target_step]`.
- If the target label is missing, unresolved, or out of range, set a status
  message such as `"Tutorial step target label is unresolved"` and return
  false.

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
    repl_set_status("Tutorial step must insert at the fading line");
    editor_completion_clear();
    return 0;
}
```

("fading line" rather than "highlighted line" because the new instruction
comment is the row currently animating via `tutorial_line_is_fading`; that
gives the user a concrete visual anchor for the message.)

Then run the existing matcher. Mismatch behavior stays unchanged: status shows
`expected: ...`, the typed input is preserved, and the command is not applied.

Empty-input rule (decided): when `input[0] == '\0'`, the precheck returns 0
silently - no status update - on **both** the `;`/Enter path and the
navigation auto-commit path. Without this, the navigation that initially
places the cursor on `expected_commit_line` (empty input, insert mode on)
would immediately spam `expected: ...` before the user has typed. The user's
discovery affordances on an empty expected line are the visible shadow text
and Tab; the `expected: ...` hint is reserved for the case where the user
has typed *something* that didn't match. Enter on an empty expected line is
accepted as a no-op.

### Commit Success Bookkeeping

The "begin" call is paired with the precheck, not a separate editor route hook:
fold it into `tutorial_precheck_current_input()` so it fires exactly when the
matcher passes and never on rejection:

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

```c
pending.step_idx        = tutorial_state.step;
pending.commit_line     = expected_commit_line;
pending.doc_count_before = repl_state_document_count();
```

After a real successful commit, before advancing to the next step, call:

```c
tutorial_note_expected_commit_applied();
```

It computes (catalog rule below guarantees `delta == 1`, but the math stays
general):

```c
int delta = repl_state_document_count() - pending.doc_count_before;
```

If `delta > 0`, first shift existing tracked tutorial lines at or after
`pending.commit_line` by `delta`. Then, if the just-committed step has a
non-empty label, record:

```c
committed_line_for_step[pending.step_idx] = pending.commit_line;
```

Record the current step's label line after shifting existing lines so the new
step's own label is not shifted as though it pre-existed the insertion. Then
clear the pending record (`pending.step_idx = -1`).

This keeps locked instruction comments and prior label positions correct when
the expected command is inserted in the middle of the document.

**v1 catalog rule: one expected line == one source row.** Catalog validation
(`repl_tutorial_validate`) rejects any `expected` that does not parse to a
single source command. The `delta` arithmetic above stays general so a future
extension to multi-row commits is mechanical, but designing for it now buys
nothing for the starter catalog.

**Single invariant for all rejection paths.** Every editor commit attempt
that called `tutorial_begin_expected_commit_attempt` MUST be followed by
exactly one of `tutorial_note_expected_commit_applied` (on `COMMIT_OK`) or
`tutorial_cancel_pending` (on every other outcome).
`tutorial_cancel_pending` is idempotent - calling it when
`pending.step_idx == -1` is a no-op - so call sites can dispatch it
unconditionally on the rejection branch without worrying about whether the
precheck actually reached the `_begin` call.

The existing `tutorial_advance_if_commit_ok(result)` wrapper is the right
place to sequence this:

```c
static void tutorial_advance_if_commit_ok(CommitResult result) {
    if (!tutorial_active()) return;
    if (result == COMMIT_OK) {
        tutorial_note_expected_commit_applied();
        tutorial_advance_after_successful_commit();
    } else {
        tutorial_cancel_pending();
    }
}
```

`tutorial_cancel_pending()` clears the pending record without shifting any
tracked lines.

Because `tutorial_cancel_pending` is idempotent, any commit path that bypasses
`tutorial_advance_if_commit_ok` (notably `commit_before_navigation()`'s
`COMMIT_REJECTED` branch, which restores the committed state and returns
early) calls `tutorial_cancel_pending()` unconditionally before returning.
That is harder to forget than wiring a flag-clearing call into one specific
branch, and it's safe because the no-op fast path costs nothing.

## Guard Changes

The current `tutorial_guard_source_change(pos, delete_count, insert_count)` is
conservative: any insertion at or before a locked line is rejected. That must
remain true for ordinary user operations, but the current tutorial step needs
one narrow exception.

Allow exactly this case:

```c
delete_count == 0 &&
insert_count > 0 &&
pos == pending.commit_line &&
pending.step_idx >= 0
```

The guard predicate keys off `pending.commit_line` rather than
`expected_commit_line` so the exception window is a property of the in-flight
attempt rather than of ambient editor state. Both fields hold the same value
at `_begin` time, but only `pending.commit_line` carries the immutability
invariant above; any future change to navigation or fade-line logic that
touches `expected_commit_line` cannot silently move the guard's exception.

Implementation:

- `tutorial_begin_expected_commit_attempt()` populates the `pending` record
  (`step_idx`, `commit_line`, `doc_count_before`); `pending.step_idx >= 0` is
  the derived guard-exception predicate.
- `tutorial_guard_source_change` allows the current expected insert iff
  `pending.step_idx >= 0 && pos == pending.commit_line && delete_count == 0
  && insert_count > 0`. All other guard rejection paths are unchanged.
- `tutorial_note_expected_commit_applied()` and `tutorial_cancel_pending()`
  both clear `pending.step_idx` back to `-1`.

This keeps paste, Ctrl+/ comment toggle, Ctrl+D, reformat, clear-all, undo, and
redo blocked by the existing call sites. It also avoids opening a generic
"insert anywhere above tutorial comments" hole.

The guard should still reject:

- Any delete or replace touching a locked instruction comment.
- Any insertion not associated with the current matched tutorial step.
- Any insertion at the expected line before the input has matched.
- Whole-document transformations while active.

## Editor Behavior

When a label-targeted step starts:

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
    STEP_APPEND("triangle_begin",
        "// Start the triangle batch.",
        "glBegin(GL_TRIANGLES)"),
    STEP_APPEND(NULL,
        "// Add the top vertex.",
        "glVertex3f(0, 0.8, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-left vertex.",
        "glVertex3f(-0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Add the lower-right vertex.",
        "glVertex3f(0.8, -0.6, 0)"),
    STEP_APPEND(NULL,
        "// Close the triangle batch.",
        "glEnd()"),

    /*
     * Insert a new instruction above the source line committed by the
     * step labeled "triangle_begin", then place glEnable directly under
     * the new instruction and before the original glBegin.
     */
    STEP_AT(NULL,
        "// Enable depth testing before the triangle is submitted.",
        "glEnable(GL_DEPTH_TEST)",
        "triangle_begin"),

    { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
};
```

The label remains stable if earlier tutorial copy changes or if previous steps
grow by more than one source row.

## Implementation Phases

### Phase 0 - Widen `repl_load_apply_line` Contract

Goal: document and pin the loader's actual `[0, document_count]` behavior
before any tutorial code depends on mid-document insertion. Every later
phase depends on this; landing it first keeps the contract change out of the
tutorial diff.

Modify:

- `src/repl/load.h`
  - Update the comment at lines 42-46 to state that
    `repl_state_edit_line` must be in `[0, document_count]` and the line
    will be inserted at that index.
- `tests/test_repl_load.c` (or the closest existing loader test)
  - Add an assertion test that pins the widened contract: calling
    `repl_load_apply_line` with `edit_line` set to a mid-document index
    inserts at that index for plain GL commands and `// comment` lines.

Verify:

- `make test`

### Phase 1 - Catalog Step Records + Validation

Goal: migrate the catalog shape without changing behavior.

Modify:

- `src/repl/tutorials.h`
  - Add `TutorialStepPlacementKind`.
  - Add `TutorialStep`.
  - Change `TutorialEntry` to hold `const TutorialStep *steps`.
  - Add `repl_tutorial_step_placement`,
    `repl_tutorial_step_label`, `repl_tutorial_step_target_label`, and
    `repl_tutorial_validate`.
- `src/repl/tutorials.c`
  - Convert existing tutorials to `TutorialStep` arrays.
  - Mark every existing step as `TUTORIAL_STEP_APPEND`.
  - Preserve all existing tutorial names and expected command strings.
  - Add file-local `STEP_APPEND` / `STEP_AT` macros.
  - Implement validation for unique labels, valid label targets, and the
    v1 one-source-row rule (each `expected` must parse to a single
    source command).
- `tests/test_tutorial_runner.c`
  - Keep existing catalog assertions.
  - Add assertions that current starter steps report append placement.
  - Add catalog-validation tests: valid tutorials pass; duplicate labels,
    missing target labels, forward references, and multi-row `expected`
    strings fail.

Verify:

- `make test_tutorial_runner` (builds and runs both the runner and match tests
  under the GL stubs path by default).

### Phase 2 - Label-Targeted Instruction Emission

Goal: the runner can reveal an instruction comment above a labeled earlier
command line, with a small real tutorial fixture exercising the label-targeted
path. Phase 4 turns that fixture into the polished worked tutorial.

Modify:

- `src/widgets/tutorial_state.h`
  - Add `expected_commit_line`.
  - Add `TutorialPendingCommit pending` (single struct with `step_idx == -1`
    sentinel; carries `commit_line` and `doc_count_before`).
  - Add `committed_line_for_step[TUTORIAL_LOCKED_LINE_MAX]` (reuses the
    existing tracked-line cap).
- `src/widgets/tutorial_state.c`
  - Initialize `expected_commit_line`, `pending.step_idx`, and every
    `committed_line_for_step` slot to `-1`.
- `src/widgets/tutorial.c`
  - Add `tutorial_shift_tracked_lines_from`.
  - Change `tutorial_emit_instruction_comment` to take an
    `instruction_line`.
  - Add label lookup over the catalog steps.
  - Add `tutorial_step_instruction_line`.
  - Have `tutorial_start` validate the full tutorial before mutating state.
  - Have `tutorial_start` and `tutorial_advance_after_successful_commit`
    compute the instruction line before emitting.
  - After emitting, set `expected_commit_line` and place the editor on it.

Tests:

- Start an append tutorial and assert the first expected commit line is the
  trailing row.
- Add a real third tutorial with a label-targeted step rather than a test-only
  entry. The user-visible tutorial in Phase 4 already needs to ship; landing
  it in Phase 2 means both Phase 2's code path and Phase 4's dogfood get
  covered by the same fixture instead of carrying a synthetic entry that later
  has to be deleted. The Phase 4 section below stays as the "ship a worked
  targeted tutorial" milestone - Phase 2 just brings forward enough of it to
  exercise the runner.
- Advance to the label-targeted step and assert the new instruction appears
  above the command line for the target label, not at the end.
- Assert previously locked instruction comments and previously recorded label
  lines after the insertion were shifted and remain correct.

Verify:

- `make test_tutorial_runner`

### Phase 3 - Commit Precheck and Guard Exception

Goal: a matching expected command can be inserted at the label-targeted line,
while ordinary user edits above locked comments remain blocked.

Modify:

- `src/editor/input.c`
  - Replace the append-only tutorial precheck with an expected-line precheck.
  - Apply the empty-input silent-reject rule (`input[0] == '\0'` returns 0
    without a status update) on both the `;`/Enter and navigation
    auto-commit paths.
  - Keep `tutorial_begin_expected_commit_attempt()` inside the successful
    precheck path.
  - On `COMMIT_OK`, note the applied tutorial insert before advancing.
  - On every other commit outcome (including the
    `commit_before_navigation()` `COMMIT_REJECTED` branch that returns
    early), dispatch `tutorial_cancel_pending()` unconditionally - it is
    idempotent, so the call site does not need to know whether the precheck
    actually reached `_begin`.
- `src/widgets/tutorial.h` / `.c`
  - Add narrow helpers for expected commit bookkeeping:
    - `tutorial_expected_commit_line`
    - `tutorial_begin_expected_commit_attempt`
    - `tutorial_note_expected_commit_applied`
    - `tutorial_cancel_pending` (idempotent - no-op when
      `pending.step_idx == -1`)
  - Update `tutorial_guard_source_change` to allow only the current matched
    expected insert (predicate: `pending.step_idx >= 0 && pos ==
    pending.commit_line && delete_count == 0 && insert_count > 0`).

Tests:

- Label-targeted step with correct input inserts the command at the expected
  middle line and advances.
- Label-targeted step with wrong input preserves input, does not insert, and
  does not advance.
- Pasting or manually inserting above a locked tutorial comment is still
  rejected when not part of the matched expected commit.
- Ctrl+/ and Ctrl+D on locked comments still reject.
- Navigation commit with matching input at the expected line advances.
- Navigation commit with matching input from any other line rejects.
- Editor commit failure after a matched precheck clears the `pending`
  record back to `step_idx == -1`.
- Empty-input commit attempt on the expected line - via `;`/Enter or
  navigation - leaves the status untouched.

Verify:

- `make test_tutorial_runner`
- `make test_ui USE_GL_STUBS=1` if fade-line tracking is touched by the test
  path.

### Phase 4 - Add a Worked Label-Targeted Tutorial

Goal: ship a user-visible tutorial that uses label-targeted insertion.

Modify:

- `src/repl/tutorials.c`
  - Add or revise a tutorial to demonstrate "draw first, then insert setup
    before the batch".
  - Keep comments written as teaching prompts, not test assertions.
  - Label the target batch-opening step, then use
    `TUTORIAL_STEP_LABEL` / `STEP_AT` to insert before it.
- `tests/test_tutorial_runner.c`
  - Pin the new tutorial name, step count, first expected command, target
    label, and label-targeted step placement.
  - Walk the full tutorial and assert the final source order has the inserted
    setup command before the original batch command.

Verify:

- `make test_tutorial_runner`
- Manual `./sample`: run the tutorial, confirm the cursor jumps back to the
  label-targeted insertion line and the command lands before the existing
  labeled command.

## Edge Cases

- **Target label belongs to a locked instruction's neighboring command:**
  allowed for the runner. The new instruction inserts above the labeled command.
  Ordinary user insertions there remain blocked.
- **Target label is missing or forward-referenced:** catalog validation fails
  before tutorial startup mutates state.
- **Target label has not been committed at runtime:** treat as an internal
  runner failure, set status, reset tutorial state, and do not emit a new
  instruction.
- **Commit produces multiple source rows:** rejected at catalog validation
  for v1. The runner's shift arithmetic stays general so a future extension
  is mechanical, but starter tutorials must use single-row `expected`
  strings.
- **User navigates away before typing:** the empty-input silent-reject rule
  applies on both the `;`/Enter and navigation auto-commit paths, so an
  empty input buffer never mutates source and never updates the status.
- **User navigates to another editable line and types the expected text:** reject
  because the edit line is not `expected_commit_line`.
- **Tutorial completion:** clear active state and locks. The source document
  remains, and all tutorial comments become editable, same as the current
  completion behavior.

## Verification Checklist

- Existing append-only tutorials still pass unchanged.
- Catalog validation rejects duplicate labels, missing targets, forward
  references, and multi-row `expected` strings.
- A label-targeted step inserts its instruction comment above the command line
  for the target label.
- The expected command lands immediately below that instruction comment.
- Existing lines at and below the target shift down.
- Previously locked instruction comments remain locked after shifting.
- Previously recorded label lines remain correct after shifting.
- Manual edits above locked comments are still blocked.
- Wrong input at a label-targeted step does not insert anything.
- Correct input at the wrong line does not insert anything.
- Empty input on the expected line - via `;`/Enter or navigation - never
  updates the status.
- Tab autofill still fills the expected command for label-targeted steps.
- Fade applies to the newly inserted instruction line, even when it appears in
  the middle of the document.
- Completing the tutorial clears locks and leaves the final document editable.

### Ownership and regression sweep (run before merge)

Every user-reachable source mutation must be one of: guarded by
`tutorial_guard_source_change`; part of the tutorial runner's own
instruction-comment emission; part of the current matched expected-command
insertion; or documented as unreachable while a tutorial is active. Audit
the mutation sites with:

```bash
rg -n 'repl_command_store_(insert|replace|delete|clear)|editor_buffer_(insert_line|insert_lines|replace_line|delete_range|set_line|set_count|load_lines|clear|apply_compiled_change)|delete_cmd_range|repl_clear_all_cmds\(' src/editor src/app src/widgets src/repl
```

Then run:

- `make test_tutorial_runner`
- `make test`
- `make check-state-ownership`
