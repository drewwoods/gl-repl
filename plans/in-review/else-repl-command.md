# First-class `else` / `else if` REPL commands — review pending

Status: **in-review** — adding both `else` and first-class `else if` is
feasible, but it is wider than the earlier `else`-only plan. The recommended
representation is now explicit: model both separators in the command stream
instead of lowering `else if` into nested `if` syntax. Do not start
implementation until this plan moves to `plans/not-started/`.

## What the code shape looks like today

- REPL conditionals are currently a strict two-marker shape:
  `CMD_IF_BEGIN ... CMD_IF_END` in `src/repl/command.h`.
- Shared block helpers treat block **heads** and **ends** as disjoint sets:
  `repl_cmd_is_block_head`, `repl_cmd_is_block_end`,
  `repl_source_scope_find_block_end`, `compile_find_block_head`,
  `repl_source_scope_block_extent`, and the block-batch branch of
  `repl_compile_toggle_comment`.
- The structured-commit chain is
  `close_brace -> for_loop -> func_def -> if_block`
  (`src/repl/compile.c`, `src/editor/commit.c`), so any input beginning
  with `}` is currently claimed by the close-brace compiler before an
  `if`-adjacent grammar can see it.
- Flatten / executor / replay logic all treat an `if` body as one contiguous
  region bounded only by `CMD_IF_BEGIN` and `CMD_IF_END`
  (`src/repl/flatten.c`, `src/repl/executor.c`,
  `src/subsystems/replay/replay_annotations.c`,
  `src/subsystems/replay/replay_playback.c`).
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
| Command model / syntax metadata | `src/repl/command.h`, `src/repl/command_spec.c`, `src/repl/help_text.c` | Add `CMD_ELSE` and `CMD_ELSE_IF`, syntax-highlight category metadata, help/completion text. |
| Structured compile / editor commit | `src/repl/compile.c`, `src/repl/compile.h`, `src/editor/commit.c`, `src/editor/commit.h` | `} else {` / `} else if(...) {` currently get stolen by close-brace handling; compile path must recognize and validate branch transitions before generic close-brace. |
| Source-scope / structural editing | `src/repl/source_scope.c`, `src/repl/source_scope.h`, `src/editor/input.c`, `src/editor/clipboard.c`, `src/repl/compile.c` | Block extent, nearest-open-block, sticky editing, block copy/cut, and block toggle-comment logic need branch-chain-aware matching. |
| Flatten | `src/repl/flatten.c` | `flatten_if_block()` currently chooses between "emit body" and "skip body"; it needs branch range splitting and first-true-branch selection across zero or more `CMD_ELSE_IF`s plus optional `CMD_ELSE`. |
| Execute / replay | `src/repl/executor.c`, `src/subsystems/replay/replay_annotations.c`, `src/subsystems/replay/replay_playback.c` | Runtime/replay walkers currently skip from false `IF_BEGIN` straight to `IF_END`; they need separator-aware skip targets and true-arm skip-over behavior. |
| Formatting / canonical text | `src/repl/reformat.c`, `src/repl/normalize.c`, `src/repl/text_helpers.c` as needed | Reformatter and normalization paths must preserve canonical `} else if(expr) {` and `} else {` output. |
| Import / load / export | `src/repl/load.c`, `src/repl/import.c`, `src/repl/export*.c` | Export can emit canonical separator lines, but import/load need explicit branch-separator support to round-trip the structure back into REPL commands. |
| Tests | `tests/test_repl_compile.c`, `tests/test_repl_editor.c`, `tests/test_repl_core_commit.c`, `tests/test_repl_executor.c`, `tests/test_repl_core_io.c`, replay/annotation tests as needed | Conditional parsing, editing, flattening, execution, replay annotation, and round-trip behavior all need coverage. |

## Specific implementation implications

1. **Close-brace handling must move first.**
   `} else {` and `} else if(...) {` cannot be added as new standalone
   compilers at the end of the chain; the existing close-brace compiler will
   consume the leading `}`.

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

4. **The runtime/replay walkers need separator-aware skip targets.**
   False `IF_BEGIN` should skip to the first true `CMD_ELSE_IF`, the `CMD_ELSE`,
   or `CMD_IF_END`. Once a true arm has been selected, reaching the next
   separator should skip the rest of the branch chain.

5. **Round-trip is part of the real feature.**
   A parse-and-execute-only implementation is cheap, but a repo-quality
   implementation should include export/import and editor structural
   behavior so `else` / `else if` are first-class command shapes, not special
   cases.

## Effort

| Scope | Estimate |
|---|---|
| Parse + flatten + execute prototype for `else` only | A few hours |
| Parse + flatten + execute prototype for first-class `else` + `else if` | **0.5–1 day** |
| Clean implementation with editor structural behavior and tests | **About 2 focused days** |
| Full polish including import/export, replay annotations, reformatting, and edge-case cleanup | **2–3 days** |

Overall rating: **moderate-to-large**, mostly because first-class `else if`
turns a two-arm conditional into a branch chain that every structural walker
must understand.

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
6. Update executor/replay walkers to honor separator-aware skip targets.
7. Add reformat / help / completion support.
8. Add export/import round-trip coverage.
9. Land the test matrix last so the behavior is locked.

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
