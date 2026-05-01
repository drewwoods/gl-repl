# Plan: Editor-Owns-Text Spike

## Context

The current architecture stores the canonical text of every committed line as `GLCmd.source[256]` — a 256-byte char array baked directly into the command struct. Every display, export, search, undo snapshot, replay annotation, and re-indent pass reads this field. The color picker is the only code that modifies `source[]` of an already-committed command (in `repl_command_store_write_color_source`), doing so by reading the indent out of the existing source before overwriting.

The redesign direction: make the **editor own the text**. Committed lines live in a text buffer inside `ReplRuntimeState`; `GLCmd` drops `source[]` and becomes a pure parse-result struct. `GLCmd.args[]` becomes derived (re-parsed from text at flatten time, already the path for `has_vars` commands). Cross-line highlights (feeding normal/color lines), color picker mutations, and replay annotations become controller-pushed decorators/transformers/virtual-lines that the editor renders from configuration.

This plan covers **Step 1 only: the spike**. The spike proves out the load-bearing performance assumption ("re-parsing on every keystroke is fast enough") before committing to the full 6-step redesign. If the spike validates the assumption, the full staged plan follows naturally from its foundation.

## Scope of the Spike

**The one question to answer:** Can we stop caching `cmd.source` as the canonical text and instead re-derive the text from the editor buffer on every re-parse, with acceptable latency under worst-case conditions (large examples, many `has_vars` lines)?

**What the spike does:**

1. Adds `char lines[MAX_COMMANDS][MAX_LINE_LEN]` to `ReplEditorBuffer` inside `ReplRuntimeState`, alongside the existing `GLCmd[]` array.
2. On every commit (`;` key, `feed_line()`, paste, undo restore), writes the raw pre-normalization text into `editor_buffer.lines[idx]`.
3. Changes `GLCmd.args[]` to be re-derived at flatten time for ALL commands (not just `has_vars`), using `repl_parser_parse_command_ctx()` reading from `editor_buffer.lines[idx]`.
4. Keeps `GLCmd.source[]` alive (no struct change yet) but stops using it as the read path for display — code panel reads from `editor_buffer.lines[idx]` instead.
5. Instruments: measure time spent in `flatten_commands()` per frame on the largest built-in example under worst-case replay (every command is `has_vars`-style re-parse).
6. Sets a pass/fail bar: frame budget at 120fps is ~8ms. Flatten must stay under 4ms (half-budget) on the worst-case example.

## What Is NOT in the Spike

- No structural change to `GLCmd` (source[] stays, just stops being authoritative).
- No transformer/decorator/virtual-line API.
- No color picker changes.
- No undo model change (undo still snapshots `GLCmd[]` including the now-redundant `source[]`).
- No test changes beyond adding the spike's instrumentation test.
- No export/import changes.

The spike is independently revertable in one commit.

## Critical Files

| File | Change |
|---|---|
| `repl_state.h` | Add `ReplEditorBuffer { char lines[MAX_COMMANDS][MAX_LINE_LEN]; int line_count; }` field to `ReplRuntimeState` |
| `repl_state_views.h` | Add `ReplEditorBuffer` struct; add `repl_state_editor_buffer()` getter and `repl_state_editor_buffer_mut()` mutator |
| `repl_state.c` | Implement getter/mutator; add `editor_buffer` to `repl_state_capture()` / `repl_state_restore()` round-trip |
| `repl_editor.c` | On every commit path (`;` handler, `feed_line`, undo restore, `load_line_to_input`) write raw input text into `editor_buffer.lines[edit_line]` |
| `repl_command_store.c` | In `repl_command_store_insert_one`, `replace_one`, `load`, also write to `editor_buffer.lines[]` |
| `repl_flatten.c` | In `flatten_range()`, for ALL commands (not just `has_vars`), re-parse from `editor_buffer.lines[src_cmd_idx]` instead of using `src_cmd->args` directly. Time this path. |
| `ui_panels.c` | In `ui_panels_render_code_panel()`, for committed non-edit lines, read from `editor_buffer.lines[i]` instead of `cmds[i].source`. Compare visual output. |
| `repl_search.c` | In `repl_search_row_text()`, read committed lines from `editor_buffer.lines[row_idx]` instead of `cmds[row_idx].source`. |
| `tests/test_spike_perf.c` (new) | Loads the largest built-in example, runs flatten 1000 times, prints mean and max flatten time per frame. Pass/fail vs 4ms target. |

## Key Existing Functions to Reuse

- `repl_parser_parse_command_ctx()` — `repl_parser.c:700` — re-parse a line of text into a `GLCmd`. Already called in `flatten_range()` for `has_vars` commands; the spike extends this to all commands.
- `flatten_range()` — `repl_flatten.c` — the main flatten pass; the spike instruments it.
- `repl_command_store_load()` — `repl_command_store.c` — bulk restore used by undo; must also restore `editor_buffer.lines[]`.
- `load_line_to_input()` — `repl_editor.c` — already the canonical "sync g_input from source" function; read direction stays, but write direction now also updates `editor_buffer.lines[]`.
- `repl_state_capture()` / `repl_state_restore()` — `repl_state.c` — must include `editor_buffer` to keep round-trip tests passing.

## Invariants to Maintain

1. **`editor_buffer.lines[idx]` is the raw user-typed text**, stripped of trailing `;` and leading whitespace (matching what `load_line_to_input` currently produces). It is NOT the normalized form.
2. **`GLCmd.source[]` keeps the normalized form** during the spike (unchanged), so all display/export code that reads `cmd.source` keeps working. The spike runs both paths in parallel; correctness is confirmed by comparing output.
3. **`GLCmd.args[]` is no longer the primary args source** for the flatten path; it's still written at commit time (so undo snapshots stay consistent) but the spike proves we can re-derive it cheaply at flatten time.
4. **`repl_state_capture()` must include `editor_buffer`** so undo round-trips don't break `test_repl_state.c`.

## Implementation Steps

1. **Add `ReplEditorBuffer` to `ReplRuntimeState`** (repl_state.h, repl_state_views.h, repl_state.c). Wire into capture/restore. Run `make test` — should be green (no behavior change yet).

2. **Write into `editor_buffer.lines[]` on every commit** (repl_editor.c: `;` handler, `feed_line()`, `load_line_to_input`, undo restore; repl_command_store.c: insert_one, replace_one, load). Write the text BEFORE normalization — what the user typed, same as what `load_line_to_input` currently loads back from `cmd.source` after stripping the semicolon. Run `make test` green.

3. **Switch flatten to re-parse from `editor_buffer.lines`** for ALL commands (repl_flatten.c). Add a `prof_begin(PROF_FLATTEN_REPARSE)` / `prof_end` bracket around this path. Build and run the largest example; compare visual output to confirm correctness. Run `make test`.

4. **Switch display read path in ui_panels.c** from `cmds[i].source` to `editor_buffer.lines[i]` for committed non-edit lines. Confirm code panel renders identically.

5. **Switch search** (repl_search.c) from `cmds[row_idx].source` to `editor_buffer.lines[row_idx]`.

6. **Write the performance test** (`tests/test_spike_perf.c`). Load `repl_examples_lines(example_idx_of_largest_example)`, feed all lines via `feed_line()`, then loop 1000x calling `flatten_commands()` with a dirty flag set, recording wall-clock time. Print mean/max, pass at <4ms.

7. **Run and record results.** If pass: proceed to full redesign plan. If fail: document where the time is spent, and design an incremental-parse story before continuing.

## Verification

```bash
make sample USE_GL_STUBS=1        # must be clean
make test                          # 24/24 binaries must pass
make test-stubs                    # 27/27 binaries must pass
./test_spike_perf                  # must report mean flatten < 4ms on largest example
# Manual: run ./sample with the largest example, confirm visual correctness
# Manual: open color picker, confirm it still writes correctly
# Manual: test undo/redo across 10 commits, confirm no corruption
```

## Exit Criteria

- All automated tests pass.
- `test_spike_perf` reports mean flatten time < 4ms on the largest built-in example.
- Code panel renders identically to before the spike (confirmed visually).
- Search, color picker, undo all work correctly.
- The spike commit is clearly labeled and independently revertable.

## If the Spike Fails the Performance Bar

Record the profiling breakdown and document it in `feature/editor-owns-text-spike-results.md`. Likely cause: `repl_parser_parse_command_ctx()` being called O(n) times per frame where n is command count. Solutions to evaluate: batch dirty-tracking (only re-parse commands where text changed since last flatten), partial parse cache, or incremental parse.

## Full Staged Plan (Post-Spike, For Reference)

If the spike validates the performance assumption, the full redesign proceeds in these stages:

- **Step 2** — Thin editor wrapper façade: `editor_buffer_line_text(idx)` / `editor_buffer_set_line(idx, str)` API hiding the storage. Remove `cmd.source[]` reads from all callers in favor of the API. (~3 days)
- **Step 3** — Drop `cmd.source[]` from `GLCmd`. Struct shrinks from ~340 bytes to ~84 bytes. Update all 13 test files that write `.source` directly. Update undo snapshot (which currently copies the whole `GLCmd[]` array). (~2 weeks)
- **Step 4** — Transformer API: `Transformer { line_idx, char_start, char_end, kind, state }`. Convert color picker to a transformer pushed by the controller. (~1 week)
- **Step 5** — Cross-line highlight API: controller pushes `Highlight[]` to editor after cursor moves. Feeding-line accents, replay PC highlight, selection, search matches all become controller-pushed highlights. (~1 week)
- **Step 6** — Configuration extraction: color scheme, syntax rules as data structs. Virtual lines for replay annotations. (~3 days)
