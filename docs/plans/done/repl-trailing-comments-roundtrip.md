# Trailing comments on REPL commands — full round-trip

Status: **DONE — implemented as designed (inline-on-the-line, shared
re-derive helper).** The architecture fork held; no `CMD_TRAILING_COMMENT`
and no `GLCmd` field. The design rationale below is retained as the
historical record; the implementation outcome is summarized next.

## Implementation outcome

Full round-trip shipped: a trailing `// ...` on a command survives a typed
commit (`;` and Enter), Ctrl+R reformat, the inline numeric swatch, and
export→import.

What landed (and where it differed from the plan):

- **Shared helpers** `repl_line_trailing_comment()` (string-aware top-level
  `//` finder, skips `label("...")`) and `repl_append_trailing_comment()`
  (idempotent: a no-op if `dst` already ends in a comment; emits the
  separator space ONLY when `dst` is non-empty) in `src/repl/eval.c`.
- **Centralized in the parser, not per-site.** The plan listed commit /
  reformat / swatch as separate call sites; in practice they all funnel
  through `repl_parser_parse_command_ctx` and copy its canonical `pl.text`
  (which already carries the trailing `;`). Appending the comment there
  covers the typed `;` commit, reformat's general-command case (it
  re-parses via `repl_parse_and_normalize`), the swatch re-parse, and the
  Enter/insert no-vars paths — one edit instead of four. It also keeps the
  editor-layer files' `repl_*` surface unchanged (the
  `check-editor-repl-surface` ratchet).
- **has-vars paths** (which rebuild text from the input, bypassing
  `pl.text`): `normalize_with_indent()` (the `;`-route, `src/repl/core.c`)
  and `rewrite_source_text_with_indent()` (Enter/insert, `src/editor/input.c`)
  now split off the comment before the `;`-normalization and re-append it
  — fixing the pre-existing `code // c;` artifact. The input.c site uses a
  plain libc `strstr` (label format strings forbid `//`) to avoid adding a
  `repl_*` symbol to input.c's ratcheted surface.
- **Export** is free for no-vars commands (`format_cmd_source_as_c` copies
  verbatim) and made correct for the translate path by teaching
  `repl_eval_expr_to_c()` to translate only the code and re-attach the raw
  comment — which ALSO fixes latent word-mangling (a `max`/`PI`/scratch
  subscript inside a comment was being rewritten to `fmaxf`/`M_PI`/...).
- **Bug found via the example golden tests:** `repl_eval_expr_to_c` is
  called on comment-ONLY lines during the code-panel dump; the first cut
  prepended a separator space (` // c`), shifting nested-comment indent by
  one. Fixed by the "separator only when `dst` non-empty" rule above.
- **Tests:** a round-trip block in `tests/test_repl_core_io.c` (commit →
  reformat → export → import; the has-vars `;`-placement; comment-word
  non-mangling). Full suite 6673/6673 green; `make check-state-ownership`
  (incl. check-c99 and editor-repl-surface input.c=25/25 commit.c=27/27)
  clean.

Deliberately **out of scope** (in-session behavior is fine; export does not
round-trip these, noted for a follow-up): trailing comments on block-head
lines (`for(...) { // c`, `funcN() { // c`) and on `CMD_VAR_DECLARE`
(export uses the `// @declare` marker, which carries names+inits but not the
decl's trailing comment).

## TL;DR (the decision)

> **Goal:** let any committed command carry a trailing `// comment` that
> survives commit, reformat (Ctrl+R), the inline numeric swatch, and
> export→import — e.g. `glBegin(GL_LINES); // open the line batch` stays
> intact instead of collapsing to `glBegin(GL_LINES);`.
>
> **Chosen design:** keep the comment as part of the command's **source
> line text**, re-derived from the buffer at each text-emission site via
> one shared helper. Do **not** model it as a separate command, and do
> **not** add a comment field to `GLCmd`.
>
> **Rejected design:** a separate `CMD_TRAILING_COMMENT` command joined to
> the previous row at render/export time. It is conceptually appealing
> (comments as first-class nodes) but breaks the load-bearing
> `command ↔ buffer-line ↔ panel-row` 1:1 invariant this codebase is built
> on, so it costs far more than it saves here.

## Background — the request

Today a trailing comment typed after a command is discarded:

```
glBegin(GL_LINES); // Some comment     ->  glBegin(GL_LINES);
```

The user asked for full round-trip preservation, and specifically asked
whether it would be cleaner to turn the trailing comment into its own
`GLCmd` (parser breaks on `;`, emits a `CMD_TRAILING_COMMENT`, and the
formatter/exporter "remove the newline" so it renders on the prior line).

## Current behavior (what the investigation found)

The REPL does **not** store your literal input. Each source command keeps
a parsed `GLCmd` (semantic) plus a **regenerated canonical line text** in
the editor buffer (`editor_buffer_view_line()` / `source_text_line()`). A
trailing comment therefore only survives where a code path explicitly
re-appends it — and most paths regenerate text from the parsed command and
drop it.

Concretely:

- **Commit, no-vars command** (e.g. `glBegin(GL_LINES)`):
  `parse_and_normalize_impl()` in `src/repl/core.c` writes the parser's
  canonical `pl.text` into the buffer. The parser rebuilds the call from
  parsed args, so the comment is gone at commit time. **This is the
  reported case.**
- **Commit, has-vars command** (e.g. `glVertex3f(0, n, 0)`): takes the
  `normalize_with_indent()` path, which keeps the body verbatim — so it
  *half-preserves* the comment, but then trims/re-adds the trailing `;`
  and emits `glVertex3f(0, n, 0); // c;` (a spurious `;` after the
  comment). So preservation is currently **inconsistent** between
  has-vars and no-vars commands, and buggy where it does happen.
- **Reformat (Ctrl+R)** — `repl_reformat_program()` in `src/repl/core.c`
  regenerates each line from the `GLCmd` in a per-type `switch`. Only the
  `CMD_VAR_ASSIGN` case preserves the comment (it does
  `strstr(orig_text, "//")` and re-appends). Every other case drops it.
- **Float decls / `var = expr`** already round-trip a trailing comment
  (decls via `FloatDeclParse.decl_comment` in `src/repl/compile.c`;
  assigns via the reformatter case above). **This is the precedent to
  generalize.**
- **Export** (`src/repl/export.c`) and the inline numeric swatch
  (`editor_commit_apply_swatch_change` in `src/editor/commit.c`) also
  regenerate text and would need the same treatment.

So the parser is *not* the bottleneck — it already has paren/string-aware
`//` scanning (`src/repl/eval.c`). The hard part is **preserving the
comment across every regeneration site**.

## The architecture fork

### Option A — comment lives in the command's source line (CHOSEN)

The comment is part of the one line that already belongs to the command.
A shared helper re-derives it from the current buffer/input text wherever
canonical text is emitted, and appends it. No new command type, no
`GLCmd` change.

### Option B — `CMD_TRAILING_COMMENT` as a separate command (REJECTED)

Parse `cmd; // c` into two commands (`cmd`, then `CMD_TRAILING_COMMENT`),
and have the formatter/exporter "remove the newline" so the comment
renders on the previous row.

## Why Option B is *not* cleaner here

The whole document model rests on a strict **1:1:1 correspondence**:

```
source GLCmd[i]   ↔   editor buffer line[i]   ↔   code-panel row
```

That lockstep is load-bearing across the codebase:

- `edit_line` is a command index used directly to index the buffer.
- Undo snapshots capture *both* the command array and the buffer lines and
  assume they match; there is an explicit guard against the two "drifting
  out of lockstep."
- `flatten` maps `src_cmd_idx` (flat → source command index).
- The tutorial runner's `locked_lines` are line/command indices.
- Clipboard operates on line ranges; click hit-testing, scroll, and
  `src/ui/core/text_layout.c` wrapping all key off buffer-line == row.

A `CMD_TRAILING_COMMENT` that shares the previous *display row* breaks the
invariant in one of two ways, both pervasive:

1. **Two commands, one buffer line** → `command_count != buffer_line_count`.
   That is exactly the drift the lockstep guard exists to prevent; every
   index that walks "the line at command i" desyncs.
2. **Comment gets its own buffer line but the renderer joins it** →
   `buffer_line_count != display_row_count`. Now cursor math, scroll
   offsets, click hit-testing, and text-layout wrapping must each
   special-case "this buffer line renders onto the previous row."

The "remove the newline at render/export" step is the tell: the instant a
model row and a display row stop being the same thing, you inherit the
whole class of bugs that 1:1 currently makes impossible. The comment is
also purely **source-level** — the flat/execution array never needs it —
so promoting it to a command drags it into machinery (flatten, the up-to
~200k-entry flat array) that has no use for it.

Option B would be the right shape in an AST-based editor where comments
are *trivia* attached to tokens and a separate view layer renders them
inline. This REPL is line-oriented (buffer line = command = row);
retrofitting trivia-style comments means either a real AST/trivia layer
(huge) or the 2-rows-as-1 hack (breaks the invariants above). Neither is
cleaner than leaving the comment in the line text.

## Why not a `GLCmd` field either

Within Option A there is a sub-choice: store the comment in a new
top-level `GLCmd` field vs. re-derive it from the buffer text.

Re-derive wins:

- Comments are source-only; the **flat array** (execution) never needs
  them. It can hold up to `MAX_FLATTEN_VISIT_BUDGET = 200000` entries after
  loop unrolling, so a fixed per-command comment buffer would bloat that
  array by megabytes for a feature most commands never use.
- A trailing comment can attach to *any* command type, so it cannot live
  in the type-keyed `payload` union; it would have to be a top-level field
  paid by every `GLCmd`, undoing the packing audit #37 deliberately did.
- Re-deriving matches the existing decl/assign precedent (which reads the
  `//` out of the source text), so it adds no new storage contract.

## Implementation plan (full round-trip)

Add one shared, string-aware helper and call it at every canonical-text
emit site:

```c
/* Append the top-level trailing `// ...` of `source_line` (if any) to
 * `dst`, separated by a single space. Skips `//` inside a label("...")
 * string literal (reuse the paren/string-aware scan in src/repl/eval.c).
 * No-op when source_line has no trailing comment. */
void repl_append_trailing_comment(char *dst, size_t dst_sz,
                                  const char *source_line);
```

Call sites (each re-derives the comment from the relevant source text):

1. **Commit** — `parse_and_normalize_impl()` (`src/repl/core.c`):
   - no-vars branch: append the input's comment to `text_out` after
     copying `pl.text`.
   - has-vars branch: fix `normalize_with_indent()` so it splits the body
     at the top-level `//` *before* the `;`-normalization, then re-appends
     the comment (kills the `// c;` artifact).
2. **Reformat** — `repl_reformat_program()` (`src/repl/core.c`): in the
   general-command/default case (and the block-head cases if comments on
   `for`/`func`/`if {` headers should survive), re-append from `orig_text`,
   exactly as the `CMD_VAR_ASSIGN` case already does.
3. **Inline swatch** — `editor_commit_apply_swatch_change()`
   (`src/editor/commit.c`): preserve the comment through the
   number-rewrite + re-parse (the comment sits after the edited arg, so
   carry it on the rebuilt line).
4. **Export** — `src/repl/export.c`: emit the trailing comment on the C
   line so saved files round-trip. Confirm the import path
   (`src/repl/import.c`, `editor_feed_line`) re-attaches it on load (it
   should, since load goes back through the commit path in step 1).
5. *(Optional)* replay annotation display
   (`src/repl/replay_annotations.c`) if the comment should show during
   replay.

### Edge cases / risks

- **`label("...")` strings** are the only commands with a string arg; a
  `//` inside the format string is *not* a comment. The label parser
  already forbids `//` in the format string, and the shared helper must be
  string-aware regardless (reuse `eval.c`'s scan). Low risk, but pin it
  with a test.
- **Block heads** (`for(...) { // c`, `func0() { // c`): decide whether a
  trailing comment after `{` is in scope; if yes, the close-brace/indent
  reformat cases need it too.
- **`;`-vs-comment ordering** in the has-vars path is the one genuine bug
  to fix, not just a feature gap (see step 1).
- **Round-trip test**: type `glBegin(GL_LINES); // c`, commit, Ctrl+R,
  nudge an unrelated swatch, export to `output.c`, reload — the comment
  must survive every hop. Add to `test_repl_core_io` / the comprehensive
  command round-trip suite.

### Effort

~1 helper + ~4–6 call sites + tests. Low-to-moderate, well-precedented
(decl/assign already do it), but genuinely spans the
parse/reformat/swatch/export paths — so "straightforward" only in the
sense that the pattern exists, not that it is a single edit.

## References

- 1:1 lockstep + canonical-text-in-buffer model: `CLAUDE.md`
  ("Two-Level Command Model", "Editing Existing Lines") and
  `src/editor/input.c::editor_reset_document_to_empty` (the drift warning).
- Existing trailing-comment precedent: `FloatDeclParse.decl_comment`
  (`src/repl/compile.c`) and the `CMD_VAR_ASSIGN` reformat case
  (`src/repl/core.c::repl_reformat_program`).
- `GLCmd` packing rationale: audit #37 (the `payload` tagged union).
- Flat-array size bound: `MAX_FLATTEN_VISIT_BUDGET` in `src/repl/flatten.c`.
