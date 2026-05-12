# Plan: REPL-Agnostic Clipboard

## Status

Phase A landed 2026-05-08 on `feature/source-tree-reorg`. Block-aware
copy and the decl-guard predicate now route through REPL-side queries
(`repl_source_scope_block_extent`, `repl_range_contains_var_decl`,
`repl_array_contains_var_decl`); `editor_clipboard.{c,h}` carry zero
`CMD_*` source references. The block-aware copy/cut behavior extends
from FOR-only to all structured-block heads (FOR / FUNC / IF) — same
UX upgrade as the comment-toggle migration; status text now reads
"Copied block (N lines)" instead of "Copied for-loop (N lines)".

Phase B (drop `GLCmd[]` storage from `ReplClipboardState`) **not
started**; remains optional per the plan.

## Load-Bearing Contract

`editor_clipboard.c` does not interpret REPL command grammar. Block-aware copy
("copy whole for-loop when cursor sits on the header") and decl-guard checks
("can't cut a line whose variable is still referenced") become REPL-side
queries the clipboard module calls — not direct `CMD_*` reads.

Whether the clipboard *stores* `GLCmd[]` is a separate question (Phase B
below) — storage shape doesn't affect the architectural goal. Phase A is
required; Phase B is optional.

## What's wrong today

`editor_clipboard.c` is the editor module that owns copy/cut/paste behavior,
yet it reaches into REPL grammar in three places:

- **Line 56** (`editor_selection_cmds_contain_var_decl`): scans a `GLCmd[]`
  for `cmds[idx].type == CMD_VAR_DECLARE`.
- **Line 114** (`current_copy_range`): tests `repl_state_document_cmds()[start].type
  == CMD_FOR_BEGIN` to decide whether to extend the copy across the block.
- **Line 140** (`current_cut_range`): same FOR_BEGIN test for cut.

`editor_state.h:94-98` also stores a `GLCmd cmds[MAX_COMMANDS]` field on
`ReplClipboardState`, parallel with `lines[][]`. Copy snapshots both arrays;
paste passes both to `repl_command_store_insert_many` + `editor_buffer_insert_lines`.
This avoids re-parsing on paste, but makes the editor a co-owner of REPL
storage shape.

## Target shape

```text
editor_clipboard.c
  - knows: row ranges, text, selection anchors
  - asks REPL: "is this line the head of a block? what range does it cover?"
  - asks REPL: "does this row range contain a variable declaration?"

REPL provides:
  - repl_block_extent(line_idx, *out_start, *out_count) -> 1 if block head
  - repl_range_contains_decl(start, count) -> 1 if any CMD_VAR_DECLARE
```

Phase A migrates the queries. Phase B (optional) drops `GLCmd[]` from the
clipboard storage entirely, parsing on paste.

## Phase A — Move grammar judgments behind REPL queries (~1 day)

### A.1 — Add REPL-side block-extent query (~2 hr)

**Where**: `repl_source_scope.{c,h}` already owns block lookup
(`repl_source_scope_find_block_end`). Add a sibling:

```c
/* If line_idx is the head of a structured block (CMD_FOR_BEGIN,
 * CMD_FUNC_DEF, CMD_IF_BEGIN), fill *out_start and *out_count with the
 * inclusive extent (head..end) and return 1. Returns 0 otherwise.
 *
 * Out values are unmodified when the function returns 0. */
int repl_source_scope_block_extent(int line_idx,
                                   int *out_start, int *out_count);
```

Today's clipboard only checks `CMD_FOR_BEGIN`. The new query covers FOR /
FUNC / IF — same shape. Add cases as the kinds expand.

Body: read `repl_state_document_cmds()`, check head type, call
`repl_source_scope_find_block_end(line_idx)`, derive count.

### A.2 — Add REPL-side decl-presence query (~1 hr)

**Where**: `repl_command_spec.{c,h}` is the natural home for grammar
predicates (it already owns `g_command_type_specs[]`). Add:

```c
/* Returns 1 if any CMD_VAR_DECLARE sits in the (start, count) range of
 * the live document, 0 otherwise. */
int repl_range_contains_var_decl(int start, int count);
```

Body: bounds-check, walk `repl_state_document_cmds()`, return on first
match.

### A.3 — Migrate clipboard call sites (~3 hr)

Replace direct `CMD_*` reads in `editor_clipboard.c`:

- `editor_selection_cmds_contain_var_decl` (line 54-60): becomes a thin
  wrapper around `repl_range_contains_var_decl`. Or delete it and inline
  the new query at every caller (5 sites).
- `editor_selection_cmd_range_contains_var_decl` (line 62-69): switch to
  `repl_range_contains_var_decl`. Drop the `editor_selection_cmds_contain_var_decl`
  call.
- `current_copy_range` (line 103-126): replace the
  `repl_state_document_cmds()[start].type == CMD_FOR_BEGIN` test +
  `repl_source_scope_find_block_end` call with a single
  `repl_source_scope_block_extent(start, &block_start, &block_count)` call.
- `current_cut_range` (line 128-148): same migration.
- `editor_clipboard_paste_current` (line 229-230): switch to
  `repl_range_contains_var_decl`. Note: this currently reads from
  `editor_state_clipboard_cmds_mut()` — a `GLCmd[]` array on the
  clipboard, not the live document. Phase A keeps that storage; the
  query in Phase A still walks live document for cut/copy guards but the
  paste-time guard scans clipboard cmds. **Either** keep the existing
  inline `cmd[idx].type == CMD_VAR_DECLARE` walk on clipboard cmds (small
  scope-creep allowance), **or** add a sibling query
  `repl_array_contains_var_decl(const GLCmd *cmds, int count)` that
  closes the seam. Recommended: add the sibling — keeps clipboard
  agnostic of `CMD_*` even at paste time.

After A.3, `editor_clipboard.c` and `editor_clipboard.h` carry no `CMD_*`
references. `#include "repl_command.h"` may stay (for the `GLCmd *` arg
type in the existing `editor_selection_cmds_contain_var_decl` signature)
or be removed entirely if that function is deleted.

### A.4 — Verification

```bash
make test                       # 28 binaries / 3382 tests stay green
make check-state-ownership      # all guards green
grep -nE "CMD_FOR_BEGIN|CMD_VAR_DECLARE" editor_clipboard.c editor_clipboard.h
# Expected: 0 hits  (down from 4 today)
```

Manual smoke test:

- Place cursor on `for (i, 0, 10) {` header, press copy. Status should
  say "Copied for-loop (N lines)".
- Cut a single line that has `float x;` while `x` is still referenced.
  Should reject with "Cannot remove float declarations".
- Paste a clipboard that contains `float x;` into a document that already
  declares `x`. Should reject. (Today's behavior — Phase A preserves it.)

## Phase B — Text-only clipboard storage (optional, ~3 days)

Drop `GLCmd cmds[MAX_COMMANDS]` from `ReplClipboardState`, leaving only
`lines[][]` and a count.

### Why optional

The architectural goal of Phase A is met without Phase B: the editor no
longer interprets REPL grammar. Phase B is purely about storage shape.

**Cost**: every paste re-parses; multi-line structured blocks need a
multi-line compile entry that incrementally advances the document so
close-brace handlers see the correct scope context.

**Benefit**: ~344 KB reduction in the clipboard struct on heap; the
clipboard storage shape stops mirroring REPL command shape.

**Recommendation**: skip unless the heap savings or storage shape are
independently load-bearing. Phase A alone closes the architectural seam
this plan was written to close.

### B.1 — Add `repl_compile_paste_lines` (~1.5 days)

```c
ReplCompileResult repl_compile_paste_lines(const char *const *lines,
                                           int n_lines, int at_line,
                                           const ReplCompileContext *ctx,
                                           ReplCompiledChange *out,
                                           char *err, int err_size);
```

Body: walk each line, parse with `repl_parser_parse_command_ctx`, build
`out->cmds[]` and `out->text[]`. Aggregate predef ops for any DECLARE.
Set `kind = REPL_COMPILED_INSERT_MANY`, `pos = at_line`, `count = n_lines`.

**Hard part**: structured blocks. After parsing line K of the paste,
the document at line K+1 needs to see line K's parse result so a `}` can
match its `for(...)`. The compile entry is pure (no live mutation), so
either:

- **Option 1** — produce a synthetic working buffer that overlays the
  live document with the partial paste, and feed it to the parser via a
  custom `EditorBufferView`.
- **Option 2** — parse top-down line by line, incrementally extending
  `out->cmds[]` as each line is parsed. The parse context for line K
  reads from `out->cmds[0..K)` for any block-depth queries it makes.
  This means `ReplCompileContext` needs a "pending insertion" view, not
  just the live document.

Option 2 is structurally cleaner but requires extending the context
type. Option 1 is more contained but allocates a temporary buffer view.
Either is workable; pick after spiking.

### B.2 — Migrate clipboard storage (~1 day)

- `editor_state.h:94-98`: drop `GLCmd cmds[MAX_COMMANDS]` field; rename
  `cmd_count` to `line_count`.
- Drop `editor_state_clipboard_cmds_mut()` accessor.
- `editor_clipboard.c::clipboard_copy_range`: only copies text now.
- `editor_clipboard.c::editor_clipboard_paste_current`: uses
  `repl_compile_paste_lines` + `editor_commit_apply_external_change`.
  Replaces the current `repl_command_store_insert_many` +
  `editor_buffer_insert_lines` direct calls.
- Decl-presence guard at paste time scans clipboard `lines[][]`
  textually using `repl_eval_source_uses_ident()` — or deferred to the
  compile entry which sees the parsed cmds and can produce the
  diagnostic itself.

### B.3 — Verification

```bash
make test
make check-state-ownership
# editor_state.h GLCmd refs:
grep -n "GLCmd" editor_state.h
# Expected: only buffer-related references (e.g., MAX_COMMANDS)
# Down from: GLCmd cmds[MAX_COMMANDS] in ReplClipboardState
```

Manual smoke test:

- Copy a 5-line for-loop body. Paste at end of document. The pasted
  block must execute correctly (FOR_BEGIN/.../FOR_END preserved).
- Copy `float x = 5;`. Paste into a document that already declares `x`.
  Reject as today.
- Undo a paste. The pasted lines must vanish atomically (single undo
  step, not 5 steps).

## Critical Files

| File | Phase | Change |
|---|---|---|
| `repl_source_scope.h` | A.1 | Declare `repl_source_scope_block_extent`. |
| `repl_source_scope.c` | A.1 | Body. |
| `repl_command_spec.h` | A.2 | Declare `repl_range_contains_var_decl` (and optional `repl_array_contains_var_decl`). |
| `repl_command_spec.c` | A.2 | Bodies. |
| `editor_clipboard.c` | A.3 | Replace `CMD_*` reads with REPL queries. |
| `editor_clipboard.h` | A.3 | Possibly drop `editor_selection_cmds_contain_var_decl` (delete or inline). |
| `repl_compile.h` | B.1 | Declare `repl_compile_paste_lines`; possibly extend `ReplCompileContext` with a pending-insertion view. |
| `repl_compile.c` | B.1 | Body, including the multi-line structured-block parse strategy. |
| `editor_state.h` | B.2 | Drop `GLCmd cmds[MAX_COMMANDS]` from `ReplClipboardState`; rename `cmd_count`. |
| `editor_state.c` | B.2 | Drop `_clipboard_cmds_mut` accessor; update capture/restore. |
| `editor_clipboard.c` | B.2 | Switch paste path to `repl_compile_paste_lines` + `editor_commit_apply_external_change`. |

## Dependency Order

```
Phase A.1 (block-extent query) ── independent
Phase A.2 (decl-presence query) ── independent
   ↓
Phase A.3 (clipboard migration)
   ↓
Phase A.4 (verification)
   ↓
[STOP here unless Phase B is independently justified]
   ↓
Phase B.1 (multi-line compile entry)
   ↓
Phase B.2 (drop GLCmd[] storage)
   ↓
Phase B.3 (verification)
```

A.1 and A.2 can land as separate commits. A.3 lands as one commit consuming
both. Phase B is one chunk per sub-step.

## Open Questions

1. Should `repl_array_contains_var_decl(const GLCmd *cmds, int count)` exist,
   or should the clipboard's paste-time guard wait for Phase B (where the
   clipboard has no `GLCmd[]` to scan)? **Lean**: add the array helper for
   Phase A so the seam is fully closed in one phase, even if Phase B never
   lands.

2. Where should `repl_range_contains_var_decl` live — `repl_command_spec.c`
   (grammar predicate) or `repl_source_scope.c` (range query)? Both are
   defensible. **Lean**: `repl_source_scope.c` since it already takes range
   arguments and reads `repl_state_document_cmds`.

3. Phase B's structured-block re-parsing — Option 1 (overlay buffer) vs
   Option 2 (extend context). Decide by spike.
