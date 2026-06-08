# `else` REPL command (`if` branch split) — review pending

Status: **in-review** — adding `else` is feasible and not a large feature,
but the internal representation needs an explicit decision first. Do not
start implementation until that representation is chosen and this plan moves
to `plans/not-started/`.

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
  helpers, so `else` is not just a parser/executor change.

## Recommended representation

Add a single mid-block marker:

```c
CMD_IF_BEGIN
  ... then body ...
CMD_ELSE      // canonical source text: "} else {"
  ... else body ...
CMD_IF_END
```

### Why this is the least disruptive shape

- `CMD_IF_END` stays the one true block closer.
- `CMD_ELSE` is a separator, not a second nested block.
- C export text can stay natural (`} else {`).
- Shared walkers do **not** need to learn that one line is both a block end
  and a new block head.

### Not recommended

Do **not** model `else` as a generic block end + block head pair
(`CMD_IF_END` + `CMD_ELSE_BEGIN`, or a dual-role line folded into
`repl_cmd_is_block_head/end`). Too many shared helpers currently rely on the
head/end sets being simple and non-overlapping, so that route widens the
blast radius for little benefit.

## What must change

| Area | Files | Why it is touched |
|---|---|---|
| Command model / syntax metadata | `src/repl/command.h`, `src/repl/command_spec.c`, `src/repl/help_text.c` | Add `CMD_ELSE`, syntax-highlight category, help/completion text. |
| Structured compile / editor commit | `src/repl/compile.c`, `src/repl/compile.h`, `src/editor/commit.c`, `src/editor/commit.h` | `} else {` currently gets stolen by close-brace handling; compile path must recognize and validate an else transition. |
| Source-scope / structural editing | `src/repl/source_scope.c`, `src/repl/source_scope.h`, `src/editor/input.c`, `src/editor/clipboard.c`, `src/repl/compile.c` | Block extent, nearest-open-block, sticky editing, block copy/cut, and block toggle-comment logic need `else`-aware matching. |
| Flatten | `src/repl/flatten.c` | `flatten_if_block()` currently chooses between "emit body" and "skip body"; it needs then/else range splitting. |
| Execute / replay | `src/repl/executor.c`, `src/subsystems/replay/replay_annotations.c`, `src/subsystems/replay/replay_playback.c` | The runtime and replay walkers currently skip from false `IF_BEGIN` straight to `IF_END`. |
| Formatting / canonical text | `src/repl/core.c` | Reformatter must preserve canonical `} else {` output. |
| Import / load / export | `src/repl/load.c`, `src/repl/import.c`, `src/repl/export.c` | Export can emit canonical `} else {`, but import/load need explicit else support to round-trip it back into REPL structure. |
| Tests | `tests/test_repl_compile.c`, `tests/test_repl_editor.c`, `tests/test_repl_core_commit.c`, `tests/test_repl_executor.c`, `tests/test_repl_core_io.c` | Conditional parsing, editing, flattening, execution, and round-trip behavior all need coverage. |

## Specific implementation implications

1. **Close-brace handling must move first.**
   `} else {` cannot be added as a new standalone compiler at the end of the
   chain; the existing close-brace compiler will consume the leading `}`.

2. **Simple depth counting is no longer enough for `if`.**
   Helpers that currently just walk "block heads" and "block ends" need an
   `if`-specific notion of:
   - matching `IF_BEGIN -> IF_END`
   - optional `IF_BEGIN -> ELSE` separator at the same nesting depth
   - "whole conditional extent" for copy/cut/comment operations

3. **The runtime needs two skip targets, not one.**
   False branch on `IF_BEGIN` should jump to `CMD_ELSE` when present,
   otherwise to `CMD_IF_END`. True branch should skip over the else arm
   once it reaches `CMD_ELSE`.

4. **Round-trip is part of the real feature.**
   A parse-and-execute-only implementation is cheap, but a repo-quality
   implementation should include export/import and editor structural
   behavior so `else` is a first-class command shape, not a special case.

## Effort

| Scope | Estimate |
|---|---|
| Parse + flatten + execute prototype only | A few hours |
| Clean implementation with editor structural behavior and tests | **About 1 focused day** |
| Full polish including import/export round-trip and edge-case cleanup | **1.5–2 days** |

Overall rating: **moderate**.

## Recommended implementation order

1. Add `CMD_ELSE` and the canonical source text shape `} else {`.
2. Teach the structured compile path to recognize `} else {` before the
   generic close-brace case can consume it.
3. Add `if`-specific source-scope helpers for:
   - matching `IF_BEGIN` / `IF_END`
   - locating the same-depth `CMD_ELSE` when present
   - computing the full conditional extent for structural editor ops
4. Update flatten, executor, and replay walkers to honor the separator.
5. Add reformat / help / completion support.
6. Add export/import round-trip coverage.
7. Land the test matrix last so the behavior is locked.

## Open review question

Keep the user-facing syntax strictly canonical as:

```c
} else {
```

That is the best fit for the current source/export model. A custom
non-C-shaped `else {` line would force extra translation work in export/import
without reducing the implementation cost elsewhere.
