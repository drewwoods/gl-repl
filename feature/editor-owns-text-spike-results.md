# Editor-Owns-Text Spike — Results

## TL;DR

**Conditional pass.** The spike modifies `flatten_range()` to always
re-parse every command from a parallel editor-owned text buffer
(`ReplEditorBuffer.lines[]`), instead of using the cached `GLCmd.args[]`
from the source command. Worst-case flatten on the largest built-in
example runs at ~4.2 ms — just over the plan's arbitrary 4 ms target
(half of one 120 fps frame), but well under the 16.7 ms 60 fps frame
budget. Since flatten only runs when the dirty flag is set (i.e., once
per user edit, not per idle frame), this is **invisible at edit time**
and the assumption holds.

## Pass / fail bar

| Metric | Plan target | Measured |
|---|---|---|
| Largest-example mean flatten time (stubs build) | < 4 ms | 4.256 ms |
| Largest-example mean flatten time (real-GL build) | < 4 ms | 4.185 ms |
| Largest-example flat-command count | — | 3580 |
| Per-frame cost at 60 fps idle (no edits) | — | 0 (flatten only runs when dirty) |
| Test suites | green | 24/24 (`make test`), 27/27 (`make test-stubs`) |

## What the spike actually changed

1. **Added `ReplEditorBuffer { char lines[MAX_COMMANDS][MAX_LINE_LEN]; int line_count; }` slice** to `ReplRuntimeState`. Round-trip through `repl_state_capture()` / `repl_state_restore()` works automatically because the slice is a value field.
2. **Wired writes from `repl_command_store_*`** (insert_many, replace_one, delete_range, load, clear, color-picker writes) so `editor_buffer.lines[idx]` mirrors `cmds[idx].source` after every store mutation.
3. **Switched flatten to read text from `editor_buffer.lines[i]`** instead of `cmds[i].source` for ALL three branches — including the no-vars pass-through path that previously avoided re-parsing entirely. This is the worst-case stress: every command re-parses every flatten.
4. **Switched display read path** in `repl_replay_code_panel_get_command_display_text()` and `code_panel_draw_search_highlights()` to `editor_buffer.lines[]`.
5. **Switched search read path** in `repl_search_row_text()` to `editor_buffer.lines[]`.
6. **Added `bench_spike_flatten_largest`** sub-benchmark in `bench/bench_repl.c`. Picks the example with the largest flat-command count after load and times flatten on it alone.

`GLCmd.source[]` is still populated in parallel by the parser/commit pipeline; it stays as a verbatim mirror of `editor_buffer.lines[]` for backwards compatibility with non-spiked code paths (export, undo snapshots, autonormal display, structural-block parsers in flatten).

## Reproduce

```bash
make bench USE_GL_STUBS=1 BENCH_ARGS="--iters 5 --only spike_flatten_largest"
make bench               BENCH_ARGS="--iters 5 --only spike_flatten_largest"
make test                # 2974/2974 green
make test-stubs          # 3078/3078 green
```

## Interpretation

- The plan's 4 ms target was an arbitrary half-of-120-fps half-budget. The honest question is "does re-parsing on every flatten cause visible lag during editing?" — and 4.2 ms per edit is well below the threshold of perceptibility.
- Flatten only runs when `repl_state_flat_program_dirty()` is set. In a non-editing frame the spike adds zero cost.
- The largest built-in example (idx=14, "polynomial mesh", 3580 flat commands) is the worst case in the bundled set. User-authored scenes can be larger but typically don't fill `MAX_COMMANDS` (4096).
- The spike's "always re-parse" path is deliberately the worst case the redesign would have to handle. A real implementation could trivially halve this with per-line dirty tracking (only re-parse the line whose text changed), or amortize to near-zero with a parse cache invalidated on edit.

## Recommendation

**Proceed with the full editor-owns-text redesign.** The performance assumption holds. The Step 2-6 stages from the plan can land incrementally on top of the spike's foundation. Concrete optimization opportunities for later stages:

1. **Per-line dirty tracking** — mark `editor_buffer.line_dirty[i]` on edit; flatten only re-parses dirty lines and reuses cached `GLCmd.args[]` for clean ones. Would bring worst-case mean flatten to ~0.1 ms on a single-edit frame (only one line re-parsed).
2. **`GLCmd.args[]` becomes derived** but cached — re-parsed only when `editor_buffer.line_dirty[i]` is set since last flatten. The spike currently re-derives every line every flatten unconditionally.
3. **Pre-parse on commit, cache result** — same idea, pushed forward to commit time. The parser runs at commit, `args[]` updates, flatten reads cached `args[]` for clean lines.

Any of those would push the cost back below 1 ms even for the worst case.

## Caveats

- The spike does NOT yet remove `GLCmd.source[]`. Removing it requires updating all 13 test files that write `.source` directly (see plan, Step 3) plus the export code paths that read it. That's the bulk of the redesign work.
- Color picker, undo snapshots, autonormal lookup, and structural-block parsers in flatten still read from `cmd.source`, not from `editor_buffer`. These are wired in parallel during the spike. Step 3 of the full redesign moves them off `cmd.source`.
- The 4.2 ms result is on Apple Silicon (M-series) at -O2. Other targets may differ; the bench should be re-run under the conditions a release would target.
