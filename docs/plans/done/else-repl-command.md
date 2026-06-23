# First-class `else` / `else if` REPL commands

Status: **done** — shipped on `feature/first-class-else-if` (commit
`a3a1194d`) and reviewed. The command model, structured compile/editor path,
source-scope indent/extent behavior, flatten arm selection, reformat/import
coverage, defensive replay skip-list handling, and architecture notes all
landed; the post-implementation review (see [Review](#review)) confirmed the
design and closed the one coverage gap (export/import round-trip). `else` and
first-class `else if` are modeled with the explicit separator representation:
both separators live in the command stream rather than lowering `else if` into
nested `if` syntax.

> **Review correction (load-bearing).** Branch *selection* is a
> **flatten-time** concern only. `flatten_if_block()` (`src/repl/flatten.c`)
> already evaluates the condition and emits *only* the taken arm, **stripping
> the `CMD_IF_BEGIN`/`CMD_IF_END` markers** from the flat program (verified:
> `./gl-repl --example cond --dump-flat` reports `num_flat_cmds=18` with zero IF
> markers). Animated conditions work because the flat program is rebuilt every
> frame, not because the executor re-evaluates. Consequently the
> `case CMD_IF_BEGIN` logic in `executor.c` and `replay_annotations.c` is
> **dead on the live (flat) path**; this feature must put first-true-arm
> selection in flatten and must *not* extend those dead runtime/replay cases.
> See the corrected Execute/replay scope below.

## What the code shape looks like today

- REPL conditionals are currently a strict two-marker shape:
  `CMD_IF_BEGIN ... CMD_IF_END` in `src/repl/command.h`.
- Shared block helpers treat block **heads** and **ends** as disjoint sets:
  `repl_cmd_is_block_head`, `repl_cmd_is_block_end`,
  `repl_source_scope_find_block_end`, `compile_find_block_head`,
  `repl_source_scope_block_extent`, and the block-batch branch of
  `repl_compile_toggle_comment`.
- The structured-commit chain is now branch-aware:
  `if_branch -> close_brace -> for_loop -> func_def -> if_block`
  (`src/repl/compile.c`, `src/editor/commit.c`), so `} else ...`
  input is recognized before the generic close-brace compiler can claim it.
- Flatten resolves the conditional **at flatten time**: `flatten_if_block()`
  (`src/repl/flatten.c`) evaluates `CMD_IF_BEGIN`'s condition, emits the body
  only when true, and skips the `CMD_IF_BEGIN`/`CMD_IF_END` markers — they never
  reach the flat program. The flat array is rebuilt every frame, so an animated
  condition re-selects each frame.
- The `case CMD_IF_BEGIN` / `CMD_IF_END` handlers in `src/repl/executor.c` and
  the `CMD_IF_BEGIN` case in `src/subsystems/replay/replay_annotations.c` walk
  the flat stream, which carries no IF markers, so they are effectively dead
  today. `replay_playback.c`'s `replay_cmd_is_focus_candidate` lists the IF
  markers only as a defensive skip-list entry over that same flat stream.
- Block-aware editor behavior (sticky Enter behavior on block headers,
  copy/cut-whole-block, toggle-comment on a whole block) keys off those same
  helpers, so branch separators are not just parser/executor changes.

## Recommended representation

Add two mid-block separator markers:

```c
CMD_IF_BEGIN
  ... then body ...
CMD_ELSE_IF   // canonical source text: "} else if(expr) {"
  ... else-if body ...
CMD_ELSE      // canonical source text: "} else {"
  ... else body ...
CMD_IF_END
```

`CMD_ELSE_IF` may repeat. `CMD_ELSE` is optional, appears at most once, and
must be the final separator before `CMD_IF_END`.

### Why this is the least disruptive shape

- `CMD_IF_END` stays the one true block closer.
- `CMD_ELSE` and `CMD_ELSE_IF` are separators, not second nested blocks.
- `else if` is first-class in the source command stream, so structural tools
  can reason about the whole branch chain without expanding it into nested
  `if`/`else` pairs.
- C export text can stay natural (`} else if(...) {`, `} else {`).
- Shared walkers do **not** need to learn that one line is both a block end
  and a new block head.
- `CMD_ELSE_IF` can reuse the same condition storage pattern as
  `CMD_IF_BEGIN`: `args[0]` is the fallback evaluated value, `has_vars` means
  re-evaluate from source text, and the source line preserves the condition
  expression.

### Not recommended

Do **not** model branch separators as generic block end + block head pairs
(`CMD_IF_END` + `CMD_ELSE_BEGIN`, `CMD_ELSE_IF_BEGIN`, or a dual-role line
folded into `repl_cmd_is_block_head/end`). Too many shared helpers currently
rely on the head/end sets being simple and non-overlapping, so that route
widens the blast radius for little benefit.

Do **not** lower `else if` into text-equivalent nested `else { if (...) { ... }
}` as the internal representation. That would be quicker at parse time but
would make the editor, formatter, export/import, branch extent, and replay
views explain one user branch as two nested blocks. The user asked for
first-class `else if`; the model should preserve that shape.

## What must change

| Area | Files | Why it is touched |
|---|---|---|
| Command model / syntax metadata | `src/repl/command.h`, `src/repl/command_spec.c` | Add `CMD_ELSE` and `CMD_ELSE_IF`, syntax-highlight category metadata, and language-construct completion text. |
| Structured compile / editor commit | `src/repl/compile.c`, `src/repl/compile.h`, `src/editor/commit.c`, `src/editor/commit.h` | `} else {` / `} else if(...) {` currently get stolen by close-brace handling; compile path must recognize and validate branch transitions before generic close-brace. |
| Source-scope / structural editing | `src/repl/source_scope.c`, `src/repl/source_scope.h`, `src/editor/input.c`, `src/editor/clipboard.c`, `src/repl/compile.c` | Block extent, nearest-open-block, sticky editing, block copy/cut, and block toggle-comment logic need branch-chain-aware matching. |
| Flatten (the core of the feature) | `src/repl/flatten.c` | `flatten_if_block()` currently chooses "emit body" vs "skip body"; it becomes the **sole** arm selector: evaluate `IF_BEGIN`, then each same-depth `CMD_ELSE_IF` in source order, and emit the first true arm's range (or the optional `CMD_ELSE` arm), continuing to strip all separators as it strips IF markers today. |
| Execute / replay (re-scoped — mostly do *not* extend) | `src/repl/executor.c`, `src/subsystems/replay/replay_annotations.c`, `src/subsystems/replay/replay_playback.c` | Because flatten strips IF markers, the runtime/replay IF handlers are dead on the flat path and need **no** separator-aware skip logic. Treat as cleanup: optionally delete the dead `case CMD_IF_BEGIN/END` blocks, and add `CMD_ELSE`/`CMD_ELSE_IF` to `replay_cmd_is_focus_candidate`'s skip-list for parity. |
| Formatting / canonical text | `src/repl/reformat.c`, `src/repl/normalize.c`, `src/repl/text_helpers.c` as needed | Reformatter and normalization paths must preserve canonical `} else if(expr) {` and `} else {` output. |
| Import / load / export | `src/repl/load.c`, `src/repl/import.c`, `src/repl/export*.c` | Export can emit canonical separator lines, but import/load need explicit branch-separator support to round-trip the structure back into REPL commands. |
| Tests | `tests/test_repl_compile.c`, `tests/test_repl_editor.c`, `tests/test_repl_core_commit.c`, `tests/test_repl_executor.c`, `tests/test_repl_core_io.c`, replay/annotation tests as needed | Conditional parsing, editing, flattening, execution, replay annotation, and round-trip behavior all need coverage. |

## Specific implementation implications

1. **Branch handling must precede close-brace handling.**
   `} else {` and `} else if(...) {` are now recognized before the generic
   close-brace compiler; adding them at the end of the chain would still let
   the leading `}` be consumed as a plain block close.

2. **Simple depth counting is no longer enough for `if`.**
   Helpers that currently just walk "block heads" and "block ends" need an
   `if`-specific branch-chain notion of:
   - matching `IF_BEGIN -> IF_END`
   - zero or more same-depth `CMD_ELSE_IF` separators
   - optional final same-depth `CMD_ELSE` separator
   - arm ranges (`then`, each `else if`, optional `else`)
   - "whole conditional extent" for copy/cut/comment operations

3. **The branch evaluator needs first-true-arm selection.**
   Flatten should evaluate the `if` condition first. If false, it scans
   same-depth `CMD_ELSE_IF` separators in source order, evaluates each
   condition, emits the first true arm, otherwise emits the `CMD_ELSE` arm if
   present. No branch emits when no condition is true and no `else` exists.

4. **The runtime/replay walkers do *not* need separator-aware skip targets.**
   Flatten emits exactly one arm and strips every IF/separator marker, so the
   flat program the executor and `replay_annotations` walk never contains
   `CMD_IF_BEGIN`, `CMD_ELSE_IF`, `CMD_ELSE`, or `CMD_IF_END`. The existing
   `case CMD_IF_BEGIN` handling there is already dead on the flat path
   (confirmed via `--dump-flat`); do not extend it. The only optional touch is
   deleting that dead code and adding the new separators to
   `replay_cmd_is_focus_candidate`'s defensive skip-list.

5. **Round-trip is part of the real feature.**
   A parse-and-execute-only implementation is cheap, but a repo-quality
   implementation should include export/import and editor structural
   behavior so `else` / `else if` are first-class command shapes, not special
   cases.

6. **The separator line itself needs outer-indent rendering.**
   Block-boundary lines render at the *enclosing* indent while bodies sit at
   +1. Keeping the separators out of the head/end predicates makes the
   then/else-if/else *bodies* indent correctly for free (a separator doesn't
   change block depth), but the `} else {` / `} else if(...) {` *line* will
   otherwise inherit body indent. `cmd_indent_chars` keys purely on
   `block_depth_prefix` (`src/repl/source_scope.c`), so add the same dedent
   special-casing that block-end lines already get: the separator line at the
   enclosing-`if` indent, its following body at +1.

7. **Syntax metadata + the predicate/category drift test.**
   `CMD_ELSE` / `CMD_ELSE_IF` need a `g_command_type_specs[]` entry with
   `CMD_CAT_CONDITIONAL` so they pick up the `if` highlight color. They will
   have a syntax *category* but intentionally no block-head/end *predicate*
   twin — confirm the predicate↔category drift test in
   `tests/test_replay_walk.c` allows that asymmetry (per `command.h`'s note on
   intentionally-narrower subsets) or add an explicit carve-out.

8. **`goto`/label semantics are unchanged, not improved.**
   A label inside a *not-taken* arm is absent from the flat stream (its arm was
   not emitted) — identical to today's single `if`, so not a regression, but
   state it. `flatten.c` already rejects goto/labels inside `funcN`; nothing
   new is needed there.

## Alternative considered: a `switch` / `case` construct

Not easier — and less general. A `switch(expr) { case k: … default: … }` is the
*same separator shape* as this plan (`SWITCH_BEGIN` + repeatable `CASE` +
optional `DEFAULT` + `SWITCH_END`), so it inherits the entire
source-scope / indent / reformat / round-trip / block-editing cost above with
nothing saved, then adds problems `else if` doesn't have:

- **Fall-through is the fork in the road.** C `switch` falls through until
  `break`. Real fall-through means a first-class `CMD_BREAK` and a flatten model
  that emits from the matched case *through* later cases until a break —
  strictly harder than one-arm selection. Dropping fall-through (each `case`
  auto-breaks) makes `switch` merely an equality-dispatch spelling of an
  `else if` chain.
- **Less expressive.** `else if` takes arbitrary conditions
  (`else if(t > 5 && x < 2)`), which suits an animated GL REPL; `switch` only
  dispatches on equality against one expression (and float equality is itself
  awkward in an all-float evaluator).
- **Syntax collisions.** `case k:` / `default:` are colon-terminated and visually
  overlap the existing `name:` goto-label syntax (`CMD_GOTO_LABEL`); the double
  indent (`case` under `switch`, body under `case`) is an unresolved style
  question the 2-space auto-indent has no clean answer for.

Recommendation: ship `else` / `else if` first. If integer-mode dispatch
ergonomics are wanted later, a *no-fall-through* `switch` can be layered cheaply
on the separator infrastructure — and because no-fall-through `switch` ≡ an
`else if` chain, desugaring `switch` → `else if` at parse time is defensible
(it flattens rather than nests, unlike desugaring `else if` → nested `if`,
which this plan rejects). The fall-through decision is the gate: if you want C
fall-through, scope `CMD_BREAK` explicitly.

## Effort

| Scope | Estimate |
|---|---|
| Parse + flatten prototype for `else` only (no execute changes — flatten selects the arm) | A few hours |
| Parse + flatten prototype for first-class `else` + `else if` | **0.5–1 day** |
| Clean implementation with editor structural behavior (compile ordering, separator indent, block extent / copy / comment) and tests | **About 2 focused days** |
| Full polish including import/export round-trip, reformatting, dead-code cleanup in executor/replay, and edge cases | **2–3 days** |

Overall rating: **moderate** — lower than a first read suggests. First-class
`else if` turns a two-arm conditional into a branch chain that the
**source/structural** tools (compile, source-scope, reformat, import/export,
editor block ops) must understand, but the **runtime** path needs essentially
no new logic — flatten already owns arm selection. The risk concentrates in
flatten + structural editing, not in the executor/replay walkers.

## Recommended implementation order

1. Add `CMD_ELSE` and `CMD_ELSE_IF`. Keep both out of
   `repl_cmd_is_block_head` / `repl_cmd_is_block_end`; they are branch
   separators.
2. Add canonical source text shapes:

```c
} else if(expr) {
} else {
```

3. Teach the structured compile path to recognize those separator forms before
   the generic close-brace case can consume them.
4. Add `if`-specific source-scope helpers for:
   - matching `IF_BEGIN` / `IF_END`
   - enumerating same-depth `CMD_ELSE_IF` and `CMD_ELSE` separators
   - computing arm ranges and the full conditional extent
5. Update flatten to emit exactly the first true arm.
6. Confirm flatten owns selection end-to-end (it does); optionally delete the
   now-dead `case CMD_IF_BEGIN/END` handlers in `executor.c` /
   `replay_annotations.c` and add the separators to
   `replay_cmd_is_focus_candidate`. No new runtime branch logic.
7. Add reformat / completion support (incl. separator-line indent and the
   `command_spec` / drift-test wiring from implications 6–7).
8. Add export/import round-trip coverage.
9. Lock tests *as each stage lands* — at minimum a flatten arm-selection test
   and a compile-recognition test before the structural-editor work — rather
   than deferring the whole matrix to the end, given the breadth of structural
   code touched and the index-keyed golden fixtures.

## Syntax recommendation

Accept C-shaped separator lines and canonicalize to:

```c
} else if(expr) {
} else {
```

The compiler may accept optional whitespace (`} else if (expr) {`) on input,
but export/reformat should emit the existing REPL style with no space between
`if` and `(`. A custom non-C-shaped `else if(expr) {` / `else {` line without
the leading `}` would force extra translation work in export/import and would
not reduce the implementation cost elsewhere.

## Review

Post-implementation review of commit `a3a1194d` against this plan. **Verdict:
high quality; faithfully follows the revised plan, including the load-bearing
flatten-time-selection correction.** Ground-truthed against the code (model,
flatten, executor, both replay walkers, source-scope, compile chain) plus an
empirical `--dump-flat` check.

### What matched the plan

- **Model** (`command.h`) — `CMD_ELSE_IF` / `CMD_ELSE` appended (stable enum
  order), new `repl_cmd_is_if_branch_separator` predicate, deliberately kept
  out of the block head/end sets.
- **Flatten** (`flatten.c`) — `flatten_if_block` became a first-true-arm chain
  selector (`flatten_if_arm_boundary` walks same-depth separators, skipping
  nested blocks via block-head/end depth tracking); markers stay stripped.
- **Executor** (`executor.c`) — the dead `CMD_IF_BEGIN`/`IF_END` walker was
  **deleted** (not extended) and folded into the "resolved during flatten" case
  group, matching the re-scoped Execute/replay guidance. `replay_playback.c`
  got the defensive focus-list parity entries.
- **Compile** (`compile.c`) — `if_branch` dispatched before close-brace;
  validation is *stronger* than the plan sketched: it rejects duplicate `else`,
  `else` not last, and `else if` after `else`.
- **Source-scope** — separator-line outer-indent fix (implication 6,
  `cmd_indent` / `cmd_indent_chars`) and chain-head `block_extent` for
  copy/cut/comment both present.
- **Editor / reformat / load** — `editor_compile_if_branch` wired first in the
  block-struct chain (handles insert + overwrite), reformat emits canonical
  separator text, the loader dispatches `if_branch`.

### Empirical confirmation

`./gl-repl --example cond --dump-flat` reports `num_flat_cmds=18` with **zero**
`CMD_IF_BEGIN`/`IF_END` markers — confirming branch selection is entirely a
flatten-time concern and the deleted executor/replay IF handling was dead on
the live path, exactly as the plan's review correction stated.

### Gap found and closed

The implementation added compile / format / parse / commit tests, but **no test
exercised the export side or a full export → import cycle**:
`test_repl_core_format.c` was import-only (loading a hand-written `else` file)
and `test_repl_core_commit.c` covered commit + flatten arm selection. The
export emission of `} else if(...) {` / `} else {` and the lossless round-trip
were unverified.

Added an if-chain round-trip case to `tests/test_repl_core_io.c` that builds a
first-class `if` / `else if` / `else` chain in the REPL, then asserts:

- export emits canonical `} else if(n < 3) {` / `} else {`;
- reload preserves command count and types with `CMD_ELSE_IF` / `CMD_ELSE`
  intact (not lowered to nested `if`s);
- flatten still selects the first true arm post-round-trip (`n = 2` → the
  else-if arm, `x == 2`);
- re-export is byte-identical (lossless canonical round-trip).

Verified green (debug + GL stubs): `test_repl_core_io` 469/469,
`test_repl_core_commit` 882/882, `test_repl_core_format` 81/81.

### Residual notes (non-blocking)

- The comprehensive `tests/test_repl_export_all_commands.c` still excludes
  control flow by design; the new `test_repl_core_io.c` case is the round-trip
  home for the if-chain.
- No follow-up scope outstanding; closing the plan.
