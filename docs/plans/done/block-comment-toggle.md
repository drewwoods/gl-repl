# Make Block Comment and Uncomment Symmetric

## Status - LANDED (2026-08-04)

Every acceptance criterion holds. Verified with `make test-stubs` (76/76
binaries, 26019 tests), `make test-web` (74/74), `make check-c99`,
`make check-state-ownership`, `make fix-doc-links`, and native + stub builds.

Formerly listed as a **prerequisite for** `goto-removal.md`. That plan landed
first (`docs/plans/done/goto-removal.md`, 2026-08-04) without waiting: `goto`
is gone, and `if(0) { … }` now carries the disable-a-region workflow. This plan
stood on its own merits - the asymmetry below was a real bug regardless.

### What the measurement changed

**The parameter list was never lost.** §"Suspected cause of the alias loss"
below is half right. Reproduced against the real code, `// triangle(sz) {`
came back as `func0(sz) {`: the alias name went, `(sz)` survived. The hypothesis
about *where* is also off - `repl_parse_and_normalize_strict_with_scope` is
never reached for a func header, and the pending-alias parse resolves the name
fine (`repl_parse_func_name_token_with_pending_alias` looks the ident up in the
live table before consulting the pending op, so `fn` and the parameters are
correct).

The actual cause is one expression at the end of `repl_compile_func_def_kernel`:

```c
format_func_header_with_alias(..., out->alias_op.slot >= 0
                                       ? out->alias_op.name : NULL);
```

`resolve_alias` deliberately emits no op for an already-registered name, so
`slot` is -1 and the header formats bare. Nothing to do with commenting: the
same rewrite happens whenever a known alias is re-committed, reachable on its
own by deleting a func def and retyping its name. Fixed by resolving the header
name from the pending op *or* the slot's registered alias, published as
`ReplFuncDefKernel.header_name` so the editor's relocation branch reuses it
rather than re-deriving it. Pinned by
`test_func_def_header_keeps_a_registered_alias`.

**Expression text was already safe.** The §4 hazard about `glVertex3f(t, 0, 0)`
returning as `glVertex3f(0.0000, 0, 0)` was fixed by the existing single-row
carve-out; it holds per row in the range path because the visible-variable set
is collected at each row's own position, as §4 required.

### What was built instead of §2's re-parse-as-a-unit

§2 proposed stripping the range into a scratch buffer and re-parsing it as a
unit. That is not expressible as a pure compile: row N's parse needs the
document with rows first..N-1 *already restored* - a body line needs its
function's parameters, a `}` needs the head it closes, an indent needs the
depth those two establish - and `ReplCompileContext` holds raw array pointers
with no overlay seam. Faithfully reproducing that purely would mean copying the
whole document (~500 KB of scratch) and re-deriving the source-scope views,
duplicating what the loader already does.

So the split follows §3's instruction to reuse the find/replace model, but
scoped to the range rather than the document:

- **Comment** stays pure - `repl_compile_comment_range()` in `compile.c`, one
  `ReplCompiledChange` for the whole range. It also *preserves each row's
  original indentation*, which a re-parse would not: once the block head is a
  comment, the rows under it are at depth 0 and would be re-indented flat.
- **Uncomment** is a range transaction in the new `src/repl/comment_toggle.c`:
  `repl_compile_uncomment_line()` (the existing single-row compiler) applied in
  place, row by row, against a document mutated as it goes, under a
  `SceneSnapshot` restored wholesale if any row is rejected.

`repl_document_rebuild()` was considered and rejected for this: a whole-document
replay makes a local toggle fail on unrelated document invalidity (comment out
one function whose calls remain, and every later toggle anywhere would refuse),
and it re-slots aliases as a side effect.

`repl_compile_toggle_comment()` is gone, split into the two direction-specific
entries above; range resolution moved to `repl_comment_toggle_plan()`.

### The three open questions, answered

1. **The selection survives.** Ctrl+/ joins copy/cut/backspace/delete in
   `keyboard_begin_key`'s exemption list. It rewrites the selected rows in
   place and preserves the row count, so the selection still names the same
   code - and keeping it is what makes the second press an exact undo.
2. **Comment refuses an unbalanced selection**, on the plan's own reasoning.
   `compile_check_range_balanced()` walks the range's command kinds; the
   uncomment side asks the same question of the stripped text, since by then
   every row is `CMD_COMMENT`.
3. **Cursor-only uncomment is brace-matched, not maximal-run.** The run of
   commented rows around the cursor is only the *search window*; the range is
   matched outward from the cursor row by reading `{` / `}` back out of the
   stripped text. So an adjacent hand-written comment is never swept in (the
   worry that motivated the question), and a nested commented block restores
   on its own. Both pinned.

### Three behaviors the plan did not name

**Blank lines.** `compile_prepend_prefix` turns an empty row into a bare `// `,
and nothing in the dispatch chain claims empty input - so before this, a single
blank line inside a block made the whole block's uncomment fail.
`repl_compile_uncomment_line` now names that case and restores `CMD_EMPTY`.

**Braces in prose.** §2's "re-derive the structure from the text" has to mean
the *code* half of the text. A `{` past a `//` - a standalone `// note {`, or a
trailing `glVertex2f(i, 0);   // and here {` - is not structure, and a row that
is still a comment after one strip can never uncomment into a block head or
end. Counting them made an ordinary comment refuse its own reverse toggle with
"unmatched {". `row_brace_shape` measures only up to
`repl_line_trailing_comment()`, which also skips string literals so
`label("a { b")` stays code all the way.

**Promotion is not undo-able.** `editor_undo_push_snapshot()` is also the
transient-scene auto-promotion hook, and promotion is one-way: it consumes a
scene slot and, for a tutorial origin, runs teardown. Pushing before a fallible
operation therefore leaks a promoted scene when that operation refuses -
rewinding the undo ring does not take it back. (The old toggle never hit this
because it pushed only after a pure compile succeeded.) So the route rehearses
before it pushes: `repl_comment_toggle_run(..., REHEARSE)` answers "would this land?"
without a trace - free on the comment side, and on the uncomment side the
transaction itself, rolled back on the way out.

## The problem

Ctrl+/ already batch-comments a block. `repl_compile_toggle_comment()`
(`src/repl/compile.c:2204`) checks whether the cursor row is
a block head, block end, or if-branch separator, resolves the enclosing
`[head..end]` extent, and rewrites the whole range as `CMD_COMMENT` rows in one
`INSERT_MANY` transaction. Put the cursor on `triangle() {`, press Ctrl+/, and
the entire function is commented out. That half works.

**Uncomment has no block path at all.** Once commented, every row in the range
is a `CMD_COMMENT`, so the block structure the comment path depended on is gone
from the command model: `repl_cmd_is_block_head(CMD_COMMENT)` is false, and the
batch branch can never fire. The uncomment arm is strictly single-row - strip
the prefix from *this* line, re-parse it, coerce to `REPLACE_ONE`.

The result is the reported failure. Pressing Ctrl+/ on a commented-out
`// triangle() {`:

1. restores only that one row;
2. leaving a function header with no body and no closing brace, so the document
   is now structurally invalid;
3. and the restored row comes back as `func0 {` rather than `triangle() {` -
   the alias and the parameter list are lost.

The operation is not reversible, and step 3 means it is not even
information-preserving on the one row it does touch.

### Suspected cause of the alias loss

Aliases are not pruned when a `CMD_FUNC_DEF` row disappears - no call site
clears them, and `repl_compile_func_def_resolve_alias()` (`compile.c:2897`)
takes the "already registered" path for a known name, deliberately emitting
*no* `alias_op`. The pending-alias parse that follows
(`compile.c:3006`) is then handed `out->alias_op.name` = `""` and
`out->alias_op.slot` = `-1`, which is the likely reason the row canonicalizes
to a bare `func0`. **Confirm this before fixing it** - it is a hypothesis from
reading, not a measurement, and the single-row restore may be corrupting the
signature before the alias lookup is even reached.

## Goals

- Ctrl+/ toggles the **selected line range** when a selection is active,
  comment and uncomment alike.
- Uncomment is the exact inverse of comment: whatever a Ctrl+/ commented, a
  second Ctrl+/ restores, including block heads, bodies, and closing braces.
- A toggle that cannot be performed cleanly changes nothing and says why.
- Aliases, parameter lists, and expression text survive a comment/uncomment
  round trip unchanged.

## Non-goals

- A general "disabled row" concept with its own storage, export form, or
  golden churn. Commented rows stay `CMD_COMMENT` and export as comments.
- Commenting a range that is not brace-balanced. Restrictive is fine and
  explicitly acceptable: **commented code still has to be legal code.**
- Preserving a partially-typed or unparseable line through the round trip.
  Comment is only offered for rows that are already valid commands.
- Multi-cursor or non-contiguous selection. The existing selection is a single
  contiguous line range and stays that way.

## Design

### 1. Selection drives the range

A contiguous line-range selection already exists and is already highlighted in
the code panel - `editor_clipboard_sel_active()` / `_sel_lo()` / `_sel_hi()`
(`src/editor/clipboard.h`), used today by copy/cut/paste.

`handle_comment_toggle_key_route()` (`src/editor/input.c:1354`)
currently reads `editor_state_edit_line()` and toggles that one row. It gains a
range: when a selection is active, the toggle target is
`[sel_lo .. sel_hi]`; otherwise it is the single row, exactly as today.

Direction is decided by the whole range, not per row: if **every** row in the
range is a `CMD_COMMENT` carrying the configured prefix, the operation is
uncomment; otherwise it is comment. A mixed range comments the rest rather than
toggling each row independently - that keeps a second press an exact undo of
the first.

### 2. Uncomment learns the block path comment already has

The asymmetry is that comment can see structure and uncomment cannot. Give
uncomment the structure back by **re-deriving** it rather than recording it:

- strip the prefix from every row in the target range into a scratch buffer;
- re-parse the stripped rows as a unit through the same path a load uses;
- accept only if the result is balanced and every row parses.

With no selection and the cursor on a commented row, the range is the maximal
run of consecutive `CMD_COMMENT` rows around the cursor that satisfies that
test. That restores the "cursor on the block head" ergonomics symmetrically:
Ctrl+/ on `// triangle() {` uncomments the whole function, because the run
starting there re-parses as a balanced block.

Rows that were *already* comments before the block was commented out come back
as comments, because stripping one prefix from `// // note` leaves `// note`,
which re-parses as a comment. The round trip is therefore stable without
tracking which rows were "originally" code.

### 3. It is a transaction

This is the same shape as find/replace (`src/repl/replace.c`):
a whole-range rewrite that is invalid at every intermediate step. Reuse that
model rather than inventing one.

- One `ReplCompiledChange` for the whole range (`INSERT_MANY` with a matching
  `delete_pos`/`delete_count`), as the comment path already produces.
- One undo snapshot per toggle, not one per row.
- If any row fails to parse, or the result is unbalanced, or the range exceeds
  `MAX_COMMIT_CMDS`, **nothing is applied** and the status line names the first
  offending row.

The existing decl handling carries over unchanged: commenting a range
containing `CMD_VAR_DECLARE` rows already routes through
`compile_collect_undeclare_for_range()`, which refuses when a declared variable
is still referenced outside the range and emits the UNDECLARE ops otherwise.
Uncomment needs the mirror - re-declaring those variables - and must refuse for
the same reasons a load would.

### 4. Fix the identity loss

Whatever the round trip restores must be byte-identical to what was commented,
for every row. Two known hazards:

- **funcN aliases and parameter lists** - the `func0 {` symptom above. Root
  cause it against the confirmed behavior, then pin it.
- **Expression text.** The single-row uncomment path already has a carve-out
  for this (`compile.c:2306`): a bare re-parse emits args from `cmd->args[]`,
  so `// glVertex3f(t, 0, 0)` would come back as
  `glVertex3f(0.0000, 0, 0)`. It calls `collect_visible_vars_in` +
  `repl_parse_and_normalize_strict_with_scope` with `preserve_expr` to avoid
  that. The range path must carry the same carve-out, and the visible-variable
  set has to be collected **per row at its own position**, not once for the
  range.

The acceptance test is a property, not a special case: comment then uncomment
any legal range and the document text is unchanged.

## Implementation sequence

1. **Round-trip identity, single row.** Fix the alias / parameter / expression
   loss on the existing single-row path first, with a test that comments and
   uncomments one row of each kind and asserts the text is unchanged. This is
   independently valuable and is the smallest reproduction of the reported bug.
2. **Range plumbing.** Teach `repl_compile_toggle_comment()` a
   `[first..last]` range and a direction, keeping the single-row and
   block-head entry points as callers of it. No behavior change yet.
3. **Uncomment block path.** Re-derive the range as in §2, with the
   balanced/parses-clean gate and whole-transaction refusal.
4. **Selection binding.** Route Ctrl+/ through the selection when one is
   active. Decide and document what happens to the selection afterwards -
   keeping it is what makes a second Ctrl+/ an undo.

## Test plan

`test_repl_core_commit` (comment toggle already lives there) and
`test_editor_input_selection` (selection semantics):

- **Round-trip identity.** For each of: a plain GL command, a command with an
  expression referencing a predef var, a `funcN` def with an alias and
  parameters, a `for` block, an `if`/`else` chain, and a row that was already a
  comment - comment then uncomment and assert the document text is byte-identical.
- **Block symmetry.** Cursor on a function header, Ctrl+/ twice, document
  unchanged. This is the reported bug, stated as its own case.
- **Selection.** A range spanning several rows comments in one press and
  uncomments in the next; one undo reverses each.
- **Mixed range.** A range containing both code and comment rows comments
  everything, and the next press restores exactly the prior mix.
- **Refusal is total.** A range whose uncomment would not be balanced, or whose
  stripped text does not parse, leaves the document *and* the undo ring
  untouched, with a status message naming the row.
- **Declarations.** Commenting a range holding a `CMD_VAR_DECLARE` still
  refuses when the variable is referenced outside it; uncommenting re-declares.
- **Capacity.** A range over `MAX_COMMIT_CMDS` refuses without mutation.

Full verification: `make test-stubs`, `make gl-repl USE_GL_STUBS=1`,
`make gl-repl`, `make check-c99`, `make check-state-ownership`, `make test-web`.

## Documentation updates

- `docs/USER_GUIDE.md` - document Ctrl+/ over a selection, and the block
  toggle, in the Comments section. Note the restriction plainly: a range is
  only commentable if it is legal code, so the toggle is reversible.
- `docs/MODULES.md` / `src/repl/ARCHITECTURE.md` - the toggle is a
  whole-range transaction owned by `compile.c`, in the same family as
  find/replace, not a per-row edit.
- `make keymap-list` output needs no change - the binding is unchanged, only
  its scope.

## Acceptance criteria

- Ctrl+/ over a selection comments the range in one press and uncomments it in
  the next.
- Ctrl+/ on a commented block head restores the entire block, not its first
  row.
- Comment followed by uncomment is byte-identity on the document, for every
  construct listed in the test plan - in particular a `funcN` def keeps its
  alias and parameters, and expression args keep their source text.
- A toggle that cannot complete cleanly mutates nothing and leaves no undo
  entry.
- One undo step reverses one toggle, whatever the range size.

## Open questions for the design read

1. **Does the selection survive the toggle?** Keeping it makes a second Ctrl+/
   an exact undo, which is the whole point; but every other selection consumer
   (copy/cut/paste) clears on use. Diverging here needs to be deliberate.
2. **Should comment refuse an unbalanced *selection*?** Commenting an
   unbalanced range is harmless going out - every row becomes a comment - but
   it cannot round-trip back, because the re-derived run would not parse as a
   balanced block. Refusing at comment time is the stricter, more honest rule
   and matches "commented code still has to be legal"; allowing it means
   accepting a one-way operation.
3. **Cursor-only uncomment scope.** With no selection, the maximal run of
   consecutive comment rows may be much larger than what the user commented
   (adjacent hand-written comments merge into it). Ending the run at the first
   row that does not re-parse as part of a balanced block is proposed; an
   explicit marker would be exact but reintroduces the "disabled row" concept
   this plan rules out.
