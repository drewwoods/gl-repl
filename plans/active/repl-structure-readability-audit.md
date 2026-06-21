# `src/repl` structure and readability audit - active

Status: **active** - implementation has started. Findings 1 (core split),
2 (compile context purity — visible-var, predef validator, and the eval path),
3 (parser strict-ref context), and 4 (source-scope view split + the 4b
performance-regression fix) are landed. The remaining findings in this document
are still a cleanup map, not completed work.

Recent implementation commits:

- `515a264f` — add the source-scope prerequisite benchmarks.
- `bd138a4b` — record the prerequisite benchmark context in this audit.
- `18c9b742` — add `ReplSourceScopeView` and move the prefix-depth cache onto
  explicit views while keeping live wrappers for app/UI callers.
- `dcf56b6a` — thread source-scope views through parser/compile paths and bind
  explicit views at parse call sites that operate on stable document snapshots.
- `38b5f5e3` — move this audit to `plans/active/` and record the Finding 4
  implementation.

## Current Implementation Status

| Slice | Status | Notes |
|---|---|---|
| Finding 1 implementation split | Done | Core split complete; `f9f8b4be` migrated every `core.h` includer and deleted the compat header — no residual. |
| Finding 4 prerequisite benchmarks | Landed | `source_scope_query` and `source_scope_churn` are in `bench/bench_repl.c`; original baseline is machine-specific. |
| Finding 4 phase 1: source-scope view/cache ownership | Landed | `18c9b742` adds view-owned prefix caches plus live compatibility wrappers. |
| Finding 4 phase 2: parser/compile source-scope plumbing | Landed | `dcf56b6a` threads `ReplSourceScopeView` through parser, compile, normalize, flatten, and scope-sensitive parse call sites. |
| Finding 4 docs/status | Landed | `38b5f5e3` moved the audit to active and marked Finding 4 implemented. |
| Finding 4 integration guard | Landed | `0906eb56` adds `normalize_large_doc`; revealed a ~14× per-call regression in the normalize path on large documents. |
| Finding 4 phase 4b: warm-view fix | Landed | Live-document normalize now reuses the warm live source-scope cache through parser live-wrapper fallback, and `reformat_large_doc` guards the user-visible reformat path. |
| Finding 2 compile purity (visible-var + predef) | Landed | `b828b64f` threads the document view; `e1eb9ae4` the predef ident-validator; the eval path now carries an `ExprCtx.predef_vars` fallback so value evaluation (scratch/var-assign/if-condition/for-bounds) resolves against `ctx->predef`, not live `g_predef_vars` (closed by the P2 follow-up plus an if-condition fix; covered by `test_eval` and `test_repl_compile`). |
| Finding 3 strict-ref context cleanup | Landed | Parser strict function-call validation now uses context-supplied source-scope and alias views; `source_scope == NULL` no longer reads live state. |

Latest verification for Finding 4:

- `make check-c99`
- `make test_repl_core_parse USE_GL_STUBS=1`
- `make test-stubs`
- `make bench-csv USE_GL_STUBS=1`
- `make bench-csv USE_GL_STUBS=1 BENCH_ARGS="--only normalize_large_doc,reformat_large_doc --iters 10"`

Local benchmark status on `drew-macbook-air` (`min_iter_ms`):

| Phase | `source_scope_query` | `source_scope_churn` |
|---|---:|---:|
| Phase 1 local reference | 0.159 | 1.227 |
| Phase 2 after parser/compile plumbing | 0.159 | 1.216 |

The local benchmark did not show the failure mode this finding guards against:
`source_scope_query` did not drift toward `source_scope_churn`.

Local Finding 4b benchmark rerun on `drew-macbook-air` after adding
`reformat_large_doc` (`--iters 10`, CSV `per_op_us` / per-reformat average):

| Phase | `normalize_large_doc` | `reformat_large_doc` |
|---|---:|---:|
| Temporary per-call-bind regression mode | 6.3143 µs | 44.6315 ms |
| Phase 4b fix | 0.4293 µs | 31.5702 ms |

The normalize entry is back in the pre-regression range. The direct reformat
bench still includes all normal whole-document formatting work, but it now
avoids the extra O(N) source-scope bind on every row.

Re-reviewed after the recent `main` rebase. The rebase partially addressed a
few items from the original audit: function-alias compile no longer mutates the
alias table, `core_internal.h` no longer re-exports scene/util headers, and
shared export-state dimensions now live in `src/repl/export_state.h`. The
findings below are updated to separate those completed pieces from the
remaining structure issues.

## Scope

This audit is a static read of `src/repl` after several architecture refactors.
The goal is not to identify behavior bugs first; it is to call out places where
module boundaries, naming, data flow, or stale transitional code now make the
system harder to reason about than it needs to be.

The strongest pattern is not "bad code" in isolation. It is **refactor
sediment**:

- APIs whose comments describe the intended architecture, while the
  implementation still reaches through older live-state paths.
- Facade headers that survived earlier migrations and now pull unrelated
  subsystems together.
- Large translation units that contain several coherent modules but have not
  been physically split yet.
- Inline migration notes and legacy aliases that were useful during the
  refactor but now compete with the current model for attention.

## Relationship to the 2026-05-14 simplicity review

This audit is the **single active cleanup map** for `src/repl`. It supersedes
`plans/partial/src-repl-simplicity-review.md`: that earlier review's
brittle-spots and smaller-risks lists are either already shipped (see its
2026-06-20 currency pass) or folded into the findings below. Two of its
still-live items are not otherwise covered here and are tracked as:

- **`compile.c` verb-boundary split** — size, *not* purity, so distinct from
  Finding 2. Deferred there; still deferred. `compile.c` is 2137 lines; the
  natural cut is `compile_var.c` (float-decl / var-assign / set-predef) +
  `compile_block.c` (close-brace / if / func / for) + a small
  `compile_internal.h` for the ~20 shared file-statics. Budget the Makefile +
  guard-script (`check-no-set-status-in-compile-apply.sh`) plumbing, not just
  the function moves.
- **`apply.c` `num_args` cascade** (~`apply.c:149`) reaches into
  `repl_state_document_cmds_mut()` to decrement `num_args` above a deleted
  predef slot. A `command_store` "decrement num_args above slot X" helper would
  localize it. Pairs with Finding 12.

The earlier review's one irreplaceable asset — the explicit "what is
load-bearing, do not refactor" list — is carried forward immediately below so
this audit is not a pure change-list with no guardrail.

## Durable spine — do not touch

Structural commitments flagged as load-bearing and well-paid-for in the
2026-05-14 review and re-confirmed here. The findings below should preserve
these, not unwind them:

- **Two-level command model (source → flat → GL).** `GLCmd` is a pure
  parse-result record with **no source-text field**; per-line text lives on
  `EditorState`. This is what lets `flatten`/`executor`/`replay_annotations`
  take a `SourceTextView`, and what lets `tools/repl_demo` link without the
  editor. Findings 9-10 must not reintroduce a text field on `GLCmd`.
- **Descriptor-table pattern.** `command_spec.c`'s three arrays
  (`k_enum_command_specs[]`, `k_std_command_specs[]`,
  `g_command_type_specs[CMD_TYPE_COUNT]`) are the single extension point for new
  commands — parser, formatter, autocomplete, code panel, F1 help, and the
  begin-block guard all read from it. Don't scatter switches back out.
- **compile/apply purity split** — the direction every finding here pushes
  *toward*, never away from. `compile.c` produces `ReplCompiledChange` values
  with side effects represented as data (`ReplPredefOp[]`, `ReplScratchOp[]`,
  and now `alias_op`); `apply.c` is the mutating dual behind a
  `repl_apply_can_apply_compiled_change` preflight. Findings 2-4 narrow the
  remaining live-state reads; they must not weaken this split.
- **Owner-vs-view header split** (`state.h` / `state_views.h` /
  `state_owners.h`, enforced by `check-views-no-owners`). The cheapest possible
  read-by-value vs. mutate barrier; the `_mut()` suffix reads cleanly. Finding
  11's `state.c` comment cleanup must keep this boundary intact.
- **`command_store.c` stays small (~150 lines) and parse-free**, and
  **`source_scope.c`'s editor-facing predicates** (`repl_line_is_block_head`,
  `repl_line_is_label`, `repl_range_contains_var_decl`) keep the editor off
  `CmdType` pattern-matching. Finding 4's view split must preserve these
  predicates as live wrappers.

## Summary table

| Area | Main files | Smell | Recommended cleanup |
|---|---|---|---|
| Core facade | `src/repl/core.h` | Addressed for implementation: `src/repl/core.c` was dissolved, and canonical APIs moved to focused owners. `core.h` remains as a compatibility reexport header, so include fan-out cleanup is the residual work. | Migrate callers from `core.h` to focused owner headers, then delete the compatibility facade when no longer needed. |
| Compile contract | `src/repl/compile.c`, `src/repl/compile.h` | Alias mutation and source-scope ownership are now fixed, but visible-var and predef reads still make compile less pure than the header claims. | Thread visible vars / predef symbols through explicit context, or document the remaining transitional reads. |
| Parser contract | `src/repl/parser.c`, `src/repl/parser.h` | Parser is documented as stateless, but strict function-call parsing reads live document and aliases. | Pass symbols through `ReplParseContext`; keep live lookup outside parser. |
| Source scope | `src/repl/source_scope.c`, `src/repl/source_scope.h` | Addressed for implementation: query APIs now have explicit view variants and the prefix-depth cache lives on the view. | Keep live wrappers for UI/app callers; continue migrating future parser/compile callers through explicit views. |
| Internal header | `src/repl/core_internal.h` | Narrower after the rebase, but still bundles normalize entry points, text helpers, and visible-var collection. | Split into `normalize.h`, `visible_vars.h`, and direct includes where useful. |
| Scene slots/workspace | `src/repl/scenes.c` | One file owns scene slots, snapshots, promotion, workspace IO, cfg/camera/predef capture, and restore mechanics. | Extract `SceneSnapshot` and workspace IO helpers. |
| Import/export | `src/repl/import.c`, `src/repl/export.c`, `src/repl/export_state.h` | Shared dimensions moved to `export_state.h`, but state-access macros, snippet/C89 constants, global import accumulators, and very large emit/load flows remain. | Centralize the remaining shared constants/accessors; then split export/import by concern. |
| Flatten | `src/repl/flatten.c` | Lowering is mixed with current-block highlight, cost queries, and cursor matching. | Move read/query helpers to `flatten_query.c` or similar. |
| State layer | `src/repl/state.c`, `src/repl/state.h` | Migration history and macro aliases obscure current ownership boundaries. | Keep current invariants inline; move historical notes to plans/docs. |
| Load transaction | `src/repl/load.c`, `src/repl/apply.c` | Apply preflight is now stronger, but load still has two visible transaction flows and an implicit "cannot fail after preflight" structured path. | Factor or document the compiled-change transaction boundary explicitly. |

## Finding 1: `core.h` and `core.c` are still a residual facade

**Status:** Done. `src/repl/core.c` was removed, and the residual
`src/repl/core.h` compatibility facade was migrated off and **deleted**
(`f9f8b4be`) — declarations now live in focused owner headers.

### What changed

The split landed as a sequence of small commits:

- `7d965430` — split host effects into `src/repl/host_effects.{c,h}`.
- `f0b542e2` — split source reformatting into `src/repl/reformat.{c,h}`.
- `f656f5f8` — split mode/vertex/tunable-var queries into
  `src/repl/program_query.{c,h}`.
- `dd35e040` — moved cursor/feed/transform query declarations into
  `src/repl/geometry_query.h`.
- `a0aca043` — split timekeeping into `src/repl/time.{c,h}`.
- `126c0a7c` — split visible-variable collection into
  `src/repl/visible_vars.{c,h}`.
- `5f640112` — split normalization into `src/repl/normalize.{c,h}`.
- `64a567d4` — moved `cmd_type_name` into `command_spec`.
- `bb26b10f` — split startup loading into `src/repl/bootstrap.{c,h}`.
- `4af9bf60` — moved scene/workspace declarations to `src/repl/scenes.h`.
- `f3efb298` — moved dirty notifications to `src/repl/state_notify.h` and
  stopped redeclaring pipeline entry points in `core.h`.
- `8e7e188e` — moved the default save wrapper into `src/repl/export.c` and
  deleted `src/repl/core.c`.
- `f9f8b4be` — migrated every remaining `#include "repl/core.h"` call site to
  the focused owner headers and deleted the compatibility header.

### Current state

The original readability problem is largely gone: new code can include
`host_effects.h`, `normalize.h`, `reformat.h`, `bootstrap.h`,
`program_query.h`, `geometry_query.h`, `time.h`, `visible_vars.h`,
`scenes.h`, `pipeline.h`, `state_notify.h`, or `export.h` directly depending
on the role it needs.

The compatibility fan-out is gone too: `f9f8b4be` migrated every
`#include "repl/core.h"` call site onto the focused owner headers and deleted
the compat header, so no TU depends on the old facade.

### Residual cleanup

None — Finding 1 is complete.

## Finding 2: compile purity is improved, but still not fully true

**Status:** Done (2026-06-21). All three pieces are context-driven:

- Visible-var collection threads the document view via `collect_visible_vars_in`
  (`b828b64f`).
- The predef ident validator + existence/capacity checks + float-decl initializer
  eval use `ctx->predef` and the eval `repl_eval_*_in` variants (`e1eb9ae4`).
- Value **evaluation** is now predef-context-driven too (closes P2). `ExprCtx`
  carries an optional `predef_vars`/`predef_count`; `eval_primary` resolves
  predef idents against it when set, else the live table (zero-initialized
  default → existing runtime callers unchanged). compile sets it on its
  scratch/var-assign value evals, the `if` condition eval, and threads it
  through the `for`-header bounds parser; the runtime (flatten) path passes NULL
  to keep live values. A `test_eval` case proves a synthetic context view wins
  over the live table for both a direct eval and a `for` bound, and
  `test_repl_compile` covers the compile-time `if` condition path.

A non-live `ReplCompileContext` now validates *and* evaluates against the same
predef table. Production behavior is unchanged (`ctx->predef` == live).

**Residual (separate):** `collect_visible_vars`' CMD_FUNC_DEF param extraction
still resolves a custom alias via the live func-alias table (symbol state, not
document/predef — a Finding-3-family follow-up, noted in `visible_vars.c`).

### Evidence

`src/repl/compile.h` describes compile as a pure source-text validation layer
that produces `ReplCompiledChange` descriptors and does not mutate state.

The recent rebase fixed one major exception: function-alias compilation no
longer speculatively mutates the global alias table. Alias changes now ride in
`ReplCompiledChange.alias_op` and are published later by
`repl_apply_alias_ops()` after the command-store mutation succeeds.

Two remaining dependencies still make the contract overstate the current
implementation:

1. Visible-variable collection still reaches live document state. The comment
   around `repl_compile_var_assign` calls this out as context-pure for document
   data, but live-state-coupled for visible-var collection.
2. Predef variables still go through the shared eval table. The header already
   labels this as transitional, but it means compile is not yet fully driven by
   `ReplCompileContext`.

### Why it hurts readability

The compile layer is one of the most important boundaries in the repo. A reader
expects it to validate input and return a descriptor, while `apply.c` and the
command store perform mutation. The alias cleanup moves the code in the right
direction; the remaining visible-var/source-scope and predef dependencies now
stand out more clearly as the next boundary issues.

### Recommended cleanup

Introduce an explicit compile-time symbol context that contains:

- visible vars for the current source position,
- source-scope information for the current document view,
- predef vars.

Then make parser/compiler helpers consult that context instead of live globals.
Alias changes are already on the desired path: they stay in
`ReplCompiledChange` and are applied in `apply.c`.

This should be sliced carefully because it touches parser, compile, and
source-scope behavior.

## Finding 3: parser is documented as stateless, but still performs live semantic lookup

**Status:** Addressed for implementation on 2026-06-20. Parser function-call
recognition, strict function-definition lookup, and alias-based canonical text
now read only from `ReplParseContext`: `source_scope` supplies the document view
for strict `CMD_FUNC_DEF` checks, and `func_aliases` supplies alias names.
`repl_parse_and_normalize{,_strict}` remain the live convenience entries, but
they now explicitly pass the warm live source-scope handle and live alias view
instead of relying on parser fallbacks.

### Evidence

`src/repl/parser.h` describes the parser as stateless: parsing the same line
with the same context should produce the same result without touching the live
editor or command list.

At audit time, `src/repl/parser.c` still performed live semantic lookup in strict function-call
parsing:

- It reads `repl_state_document_cmds()` and `repl_state_document_count()` to
  find a matching function definition.
- It reads the function alias table to resolve alias calls.

The parser also owns canonical text emission for many command classes:

- Standard command text formatting.
- Enum command text formatting.
- Special formatting for function calls, labels, material, point parameters,
  matrix ops, tessellation commands, and control-flow markers.

The main parse dispatcher is a long ordered recognition ladder with several
special cases before falling back to unknown-command preservation.

### Why it hurts readability

The parser is doing three jobs:

1. Recognize source syntax.
2. Perform semantic lookup against the current program.
3. Emit canonical source text.

Those jobs are related, but not identical. Combining them makes strict parsing
and formatting behavior harder to test in isolation, and it makes the parser's
"same input, same output" claim dependent on hidden live state.

### Implemented cleanup

- Extended `ReplParseContext` with `func_aliases` alongside the existing
  `source_scope`.
- Moved parser function-name alias resolution, strict function-definition
  lookup, undefined-function diagnostics, and canonical alias text emission
  behind that context.
- Kept live-state convenience wrappers outside the parser: live normalize paths
  call `repl_source_scope_live_view()` and `repl_func_alias_view()` before
  invoking the parser.
- Consider moving canonical text emission into a formatter layer once the live
  dependency is removed. The parser can still return enough structured data for
  canonical formatting; it should not need to know the live document.
- Replace the largest recognition ladder with a table of handler functions over
  time. This is a readability improvement, not an urgent correctness fix.
- Removed the `ReplParseContext.source_scope == NULL` live fallback added by
  the Finding 4b fix (`c3d0d88d`). To keep the 4b performance fix without parser
  live-state reach, `source_scope.c` now exposes a guard-compatible
  value-returning `repl_source_scope_live_view()` handle; normalize passes its
  contained view explicitly. `NULL` once again means "no scope." The
  `normalize_large_doc` bench guards the performance side of that swap.

## Finding 4: `source_scope` query APIs hide live-state cache behavior

**Status:** Addressed for implementation on 2026-06-20. The view/cache split
landed in `18c9b742`, and parser/compile source-scope callers moved onto
explicit views in `dcf56b6a`. Existing live wrappers remain for app/UI/editor
compatibility call sites.

### Evidence

Prior to the implementation commits, `src/repl/source_scope.c` owned static
prefix-depth cache arrays and rebuilt them by reading
`repl_state_document_count()` and `repl_state_document_cmds()`. Several
source-scope functions looked like pure queries from their names, but operated
on the current live document implicitly.

The remaining live wrappers are now explicit compatibility helpers. Parser and
core compile code carry `ReplSourceScopeView` through their contexts.

### Why it hurts readability

This is the deepest form of hidden coupling in the current `src/repl` shape.
Callers that appear to be context-based are still bound to the live document
because a helper underneath them reads `g_repl_state`.

It also makes testing harder. Tests need the live document arranged correctly,
even when the function under test appears to accept all of the needed context.

### Implemented cleanup

The implementation added explicit source-scope view APIs:

```c
typedef struct ReplSourceScopeView {
    const GLCmd *cmds;
    int count;
    /* The prefix-depth cache lives here, built once when the view is bound
     * to a document — so queries stay O(1) with no global. See below. */
} ReplSourceScopeView;
```

There are now two layers:

- `*_view` functions that operate only on the passed document view.
- Existing live wrappers that build/use the current global document view for
  app/UI callers.

After that split, parser and core compile code use the view APIs for
source-scope queries.

#### Cache ownership — the crux, not an afterthought

The prefix-depth cache was the thing to get right — but **not because lookups
are expensive. They aren't.** At audit time, `source_scope.c` kept four
file-static prefix arrays (`g_block_depth_prefix`, `g_begin_depth_prefix`,
`g_tess_depth_prefix`, `g_matrix_depth_prefix`) behind a dirty flag
(`g_depth_cache_dirty`). The implemented shape keeps the same
**build-once, query-many** cost model, but owns the arrays on
`ReplSourceScopeView` instead of hidden file-statics. The two costs remain very
different:

- A **query** is an O(1) array index at `pos`. This is the hot path —
  `flatten.c` and `parser.c` hit it repeatedly per frame — and it stays O(1)
  no matter how the cache is owned.
- The **O(N) cost is the rebuild** (`source_scope_view_build()`, one pass over
  the document), paid when a caller binds a `ReplSourceScopeView` or when the
  live wrapper lazily rebuilds after
  `repl_source_scope_depth_cache_invalidate()` marks it dirty.

So the earlier "live callers keep O(1), view callers pay an honest O(N)"
framing was wrong, and worth correcting because the whole REPL hinges on these
lookups staying fast: it implied decoupling makes lookups slow. It doesn't. The
build-once/query-many shape is **orthogonal to where the prefix arrays live** —
a file-static global and a field on a passed-in view both build in O(N) and
serve queries in O(1). A view is only "O(N) per query" if it *rebuilds on every
call*, which is a design mistake, not a property of views.

The real decision is therefore not speed, it is **cache lifetime / ownership
and invalidation**:

- Put the four prefix arrays (plus a `built` flag) on the source-scope
  view/context instead of file-statics, and build them when the view is **bound
  to a document** — once per frame / per compile pass, where the document is
  stable. Queries then index the view in O(1). Same cost profile as the old
  global, with explicit ownership instead of hidden state.
- Keep a thin **live wrapper** that owns a process-wide view for the editor/UI
  hot path and rebuilds it at exactly today's five invalidation points (or
  lazily on first query after a mutation, as now). Production keeps its
  amortized O(1); nothing in the steady state gets slower.
- Tests / one-shot callers build a view over their own `(cmds, count)` and
  query it — also O(1) per query after a single O(N) build. The only thing that
  is genuinely O(N)-heavy is "construct a fresh view, do one query, discard,
  repeat," which is a usage anti-pattern, not a cost the design imposes.

The two failure modes to **avoid** are both design errors, not inherent costs:
a view that rebuilds the prefix arrays on every query (needless O(N²) over a
query batch), and a view where nobody owns invalidation (stale depths served
silently). **Decide this ownership/lifetime question before steps 3-5 of the
cleanup order** — context-based source-scope, visible-vars, and parser
strict-ref all sit on top of it.

#### Prerequisite benchmark (landed)

A regression guard for the lookups this refactor must keep fast is now in place
(`bench/bench_repl.c`):

- **`source_scope_query`** sweeps all depth/scope queries over a 2880-row
  deeply-nested document with a warm cache — it isolates the amortized O(1)
  lookup, so a view that recomputes prefixes per call shows up as a large
  per-sweep blow-up (toward churn cost).
- **`source_scope_churn`** invalidates + queries per op — it isolates the O(N)
  rebuild, guarding the "build on (re)bind" cost.

The post-phase-1 baseline (stub build = reproducible pure-C cost, `--iters 10`)
is committed at `bench/baselines/finding4-prereq.mac-mini.csv`. Re-run after
each of steps 3-5 with `make bench-csv USE_GL_STUBS=1` and compare the
**`min_iter_ms`** column — the mean is noisy on a shared host, and the CSV's
`# machine:` preamble records which host a baseline came from. Treat a
`source_scope_query` time drifting toward `source_scope_churn` as the cache
having been dropped — exactly the failure mode this finding warns about.

#### Phase 4b (recommended): fix the per-call view-bind regression

**Status (2026-06-20): landed.** The implementation keeps the warm live view
private because the public pointer-returning accessor proposed below failed the
`check-views-by-value-snapshot` ownership guard. Instead,
`repl_parse_and_normalize{,_strict}` passes no explicit source-scope view, and
the parser's source-scope helpers fall back to the existing live wrapper
functions when `ctx->source_scope` is `NULL`. Snapshot/non-live callers still use
`repl_parse_and_normalize_strict_with_scope` with an explicit
`ReplSourceScopeView`.

The `normalize_large_doc` integration guard (commit `0906eb56`) caught a real
regression that `source_scope_query` could not: the view split routed
`repl_parse_and_normalize{,_strict}` (`src/repl/normalize.c:175-201`) off the
warm live cache and onto a **fresh `ReplSourceScopeView` bound against the whole
live document per call** (O(N) build). Measured on a 2880-row document (stub
build, `--iters 10`, mac-mini, `min_iter_ms`):

| | per normalize |
|---|---|
| pre-Finding-4 (`bd138a4b`, warm `g_live_scope`) | 0.35 µs |
| post-Finding-4 (HEAD, per-call bind) | 5.0 µs (~14×) |

**Where it bites.** One commit normalizes once (5 µs — imperceptible). The
victim is the per-line loop in **`reformat.c:380`** (`repl_parse_and_normalize`
called for every command in `Ctrl+\` reformat-all): O(N) per line × N lines =
**O(N²)** — roughly 14 ms at 2880 rows, ~26 ms approaching `MAX_COMMANDS`
(4096), a visible hitch. `load.c`'s per-line `_strict` call mutates the document
each line, so it rebuilt per line pre-Finding-4 too (no regression there);
commit (`input.c`) and `compile.c`'s `repl_compile_context_from_live` bind once,
not in a loop (fine).

**Fix — route live-document normalize through the warm view (this is what this
finding's own cache-ownership recommendation already called for):**

1. Keep the warm live view private behind `live_source_scope_view()` and the
   existing live wrapper API; do not add a public pointer-returning live-view
   accessor.
2. Make `repl_parse_and_normalize{,_strict}` pass `NULL` into
   `parse_and_normalize_impl` instead of binding a fresh per-call view. The
   parser helpers interpret `NULL` as "use the live document wrappers", so a
   stable document (the reformat read pass) returns to amortized O(1) live-cache
   behavior.
3. Keep `repl_parse_and_normalize_strict_with_scope` (explicit view) for callers
   that legitimately parse against a *non-live* document (tests,
   replay-against-snapshot). The view API stays; only the live convenience
   entries change.
4. Leave `repl_compile_context_from_live` on its explicit owned view for now; its
   one bind per commit is not the regression, and changing it would reopen the
   public live-view ownership question.

**Correctness.** The warm view reflects the committed document (the candidate
line being normalized is not yet in it) — identical to the pre-Finding-4 warm
global, so no semantic change. Reusing `g_live_scope` is strictly ≥
pre-Finding-4 behavior, since pre-Finding-4 used that same warm global.

**Verify.** Re-run `make bench-csv USE_GL_STUBS=1 BENCH_ARGS="--only
normalize_large_doc"`; expect the per-normalize result to return to the
sub-microsecond pre-regression range. `reformat_large_doc` is now in
`bench/bench_repl.c` as the direct guard: it reformats the same large live
document end-to-end, so the user-visible O(N²) failure mode has a named bench.
On `drew-macbook-air` (`--iters 10`), the temporary per-call-bind mode measured
`normalize_large_doc=6.3143 µs` and `reformat_large_doc=44.6315 ms`; the fixed
code measured `normalize_large_doc=0.4293 µs` and
`reformat_large_doc=31.5702 ms`.

## Finding 5: `core_internal.h` is narrower, but still a mixed internal bucket

### Evidence

`src/repl/core_internal.h` contains:

- Normalize/commit pipeline declarations.
- Text-helper declarations by including `repl/text_helpers.h`.
- Visible-variable collection.

The recent rebase removed the legacy scene/util re-exports, which resolves the
stale part of the original finding. The header still leaks beyond a narrow
REPL-internal seam: it is included by core REPL modules, editor code,
app-side actions/completion, replay/tutorial internals, the demo, and tests.

### Why it hurts readability

An "internal" header can be useful when it expresses a coherent internal module
boundary. This one is closer than before, but callers still include it to get
one helper and receive parse/normalize, text-helper, and visible-var concepts
together. The app-side includes are a sign that useful public seams are still
missing or that those call sites should move behind a narrower facade.

It also weakens the public/private distinction. Helpers that should have
specific homes remain reachable from a broad internal header.

### Recommended cleanup

- Move normalization declarations to `normalize.h`.
- Move visible-variable collection to `visible_vars.h` or source-scope.
- Move `#include "repl/text_helpers.h"` to the top of files that need it
  directly.

This is mostly mechanical once source-scope and visible-vars ownership is
decided.

## Finding 6: `scenes.c` combines scene state, snapshots, and workspace IO

### Evidence

`src/repl/scenes.c` owns all of the following:

- User-scene slot data.
- Active slot and pre-example state.
- Save/load of a live scene into a slot.
- Promotion from example to user scene.
- Workspace path/slug handling.
- Workspace save/load.
- Stash/restore helpers used while iterating scene slots.
- Capture/restore of commands, source lines, cursor, predef values, scratch
  arrays, function aliases, cfg, camera, and status text.

Several functions perform similar "copy live state into a container" or
"install container into live state" work.

### Why it hurts readability

The real concepts are separable:

1. A serializable/copyable scene snapshot.
2. A scene-slot state machine.
3. Workspace filesystem IO.
4. Example promotion policy.

Keeping all four in one file makes it hard to inspect invariants like "what
exactly is captured by a scene?" or "what does workspace save temporarily
install into live state?"

### Recommended cleanup

Extract a `SceneSnapshot` helper:

```c
typedef struct SceneSnapshot SceneSnapshot;

void scene_snapshot_capture_live(SceneSnapshot *dst);
void scene_snapshot_apply_live(const SceneSnapshot *src);
void scene_snapshot_clear(SceneSnapshot *snapshot);
int  scene_snapshot_copy(SceneSnapshot *dst, const SceneSnapshot *src);
```

Then split workspace mechanics into `workspace_io.c` or a similarly named file.
After that, `scenes.c` can focus on slot selection, active-scene state, and
promotion policy.

This also helps clarify the caveat in `state.h` that scene catalog statics are
not covered by `repl_state_capture` / `repl_state_restore`.

## Finding 7: import/export share duplicated constants and access macros

### Evidence

`src/repl/export_state.h` now centralizes shared dimensions such as workspace
header and camera-line sizes. That resolves part of the original duplication.

`src/repl/import.c` still explicitly says some state-access macros are
duplicated verbatim from `export.c` because the two translation units do not
share helpers. Both files also duplicate directive/helper constants for snippet
declarations and C89 export markers.

`import.c` also has file-level accumulator state for:

- pending cfg values,
- deferred predef values,
- header warning counts.

### Why it hurts readability

The import/export pair is a round-trip contract. `export_state.h` now lets the
compiler guard shared buffer dimensions, but the remaining duplicated constants
and access macros are still maintained by convention. A future export change
can still silently drift from import.

The file-level import accumulators also make the import pipeline harder to
reason about because state lifetime is not carried by the main `ImportState`
object.

### Recommended cleanup

- Add a tiny shared header for the remaining import/export constants and
  state-access helper macros, for example `src/repl/export_format_shared.h`.
- Move import accumulator fields into `ImportState` where practical.
- Keep the shared header small. Do not create a broad "import_export_util" file
  that becomes another dumping ground.

This remains a good cleanup before physically splitting `export.c`, because it
reduces duplicated scaffolding first. Keep `export_state.h` focused on shared
state dimensions; do not overload it with format directive policy.

## Finding 8: import flow is an implicit state machine

### Evidence

`src/repl/import.c` has a real import state machine:

- It accumulates physical C lines into logical lines.
- It tracks function-body state.
- It detects workspace headers, snippet markers, cfg directives, comments,
  camera blocks, predef directives, and command body lines.
- It converts C-ish lines back into REPL source through several helper paths.

The main line processor is an ordered chain of special cases. Handler order is
critical, but the order is encoded as control flow rather than data or an
explicit line kind.

Some import errors/statuses are reported directly to `stderr` inside helper
functions instead of being accumulated through `ImportState`.

### Why it hurts readability

The current code requires a reader to simulate the entire ordered chain to
answer "which handler owns this line?" or "what states can this line transition
through?" That makes import changes risky, especially because import/export
round-trip behavior is user-visible.

### Recommended cleanup

- Introduce an `ImportLineKind` enum or a small handler-result enum.
- Make the ordered handler chain explicit, either as a table or as named phases
  with a single return convention.
- Thread diagnostics through `ImportState` so the top-level import routine owns
  reporting policy.

This does not need to change the file format. It is primarily a flow cleanup.

## Finding 9: `export.c` is several modules in one translation unit

### Evidence

`src/repl/export.c` is over 3k lines and contains at least these concerns:

- Local state bridge macros.
- Exported header strings and helper text.
- Workspace header emission.
- Init/bootstrap emission.
- Light/state export.
- Command-to-C writers.
- Generated-name collision handling.
- Export-needs scanning.
- Display-pass generation.
- Tune helper emission.
- Keyboard/tick/init/footer scaffold generation.
- Public save entry point.

There is also a local TODO around display pass generation where cfg toggles are
read and then deliberately ignored because overlays are disabled in export.

### Why it hurts readability

The file is long because export itself is broad, but its internal sections are
already natural module boundaries. Keeping everything in one translation unit
forces unrelated details into the same search space.

The display-pass TODO is a small example of policy being implicit: export has a
policy about what scene overlays are preserved, but the code expresses that as
local ignored variables and comments.

### Recommended cleanup

Divide by **output region** — the axis the existing scaffold section table
already orders the file by — into **4 focused TUs + `export.c` as the
orchestrator**, behind a shared private `export_internal.h`. This is a
deliberate reduction from an earlier 7-file sketch, which over-fragmented:
splitting `export_names.c` (~140 lines) and `export_tune.c` (~150 lines) into
standalone files scatters `display()` generation across three TUs when names +
needs-scan + passes + tune are one concern. Grouping by output region means
each TU maps 1:1 to a contiguous region of the generated `output.c`, so a
reader navigating the output lands in one file.

| New TU | ~lines | Holds (output region) |
|---|---:|---|
| `export_cmd_writer.c` | ~500 | GLCmd→C **body** emission: `write_canonical_cmd_as_c`, for/tess writers, `write_render_body_range`, `format_cmd_source_as_c`, `find_export_block_end`, the materialfv / point-parameter C89 translators. The largest, most self-contained chunk. |
| `export_prologue.c` | ~500 | The generated file's **helpers + globals**: predef/scratch globals, save/restore helpers, `rand`, glfloat-vector helpers, `label`, render helper, func defs, tess preamble. |
| `export_display.c` | ~550 | **`display()` generation + runtime UI**: generated-name collision (`ExportNameSet`, `export_choose_name`), needs scan (`ExportNeeds`, `export_collect_needs`), display passes + render policy, the tune knobs / keyboard handlers. Keeps names + needs + passes + tune together. |
| `export_setup.c` | ~700 | **Header + GL-state setup**: workspace `@var`/`@cfg`/`@scene` metadata + C89 comment formatting, init bootstrap, lights init/display, camera/render-state line refresh. (Split into `export_header.c` + `export_init.c` if it feels too big.) |
| `export.c` | ~400 | **Orchestrator**: the light/camera/projection bridges (install + accessors), the scaffold section table + `emit_export_scaffold`, and the public API (`repl_export_save_output`, `repl_save_default_output`, `repl_dump_code_panel_text`). |

`export_internal.h` carries the shared substrate: `ExportNeeds`,
`ExportGeneratedNames`/`ExportNameSet`, the `IMPORT_EXPORT_STATE` macro block,
the float/format helpers, the bridge accessors, and `ExportScaffoldContext`.

**Sequencing / cost (this is where the work is, not the function moves):**

1. **Do Finding 7 first.** The `IMPORT_EXPORT_STATE` macros + shared constants
   need `export_internal.h` as their home — which is exactly the substrate this
   split requires anyway. Centralize them, then split.
2. **Makefile + guards:** add the new TUs to every source / stub / test list,
   and repoint any `export.c`-named guard (e.g. `check-repl-export-via-bridge`)
   at the new filenames — the same plumbing tax flagged for the `compile.c`
   split.
3. **Behavior-preserving + guarded:** the cut is a mechanical move;
   `test_export_trace_parity` + `test_repl_core_io` (round-trip) are the safety
   net.

The one *non*-move worth doing alongside: make the export render policy
explicit instead of the ignored-cfg TODO in the display path —

```c
typedef struct ReplExportRenderPolicy {
    int include_axes;
    int include_grid;
    int include_overlays;
    int include_point_guides;
} ReplExportRenderPolicy;
```

— so a disabled overlay reads as policy, not as a silently-ignored cfg read.
That is a behavior clarification, not part of the file cut.

## Finding 10: `flatten.c` mixes lowering with query/UI-adjacent helpers

### Evidence

`src/repl/flatten.c` owns the flat-program construction path, but it also owns:

- current-block highlight refresh,
- flat-command cost helpers,
- cost lookup by source line,
- cursor matching against flat commands.

### Why it hurts readability

Flattening is the compiler/runtime bridge: source commands become executable
flat commands. Cost queries and cursor/highlight matching are consumers of the
flat result, not part of lowering itself.

Keeping these together makes the file look more stateful and UI-aware than the
core lowering logic needs to be.

### Recommended cleanup

Move read/query helpers into a companion module:

- `src/repl/flatten_query.c`
- `src/repl/flatten_query.h`

Leave `flatten.c` responsible for:

- context setup,
- recursive/range flattening,
- command emission,
- flat dirty/rebuild orchestration,
- final flat-program result state.

This should be a low-risk split if done mechanically.

## Finding 11: `state.c` still carries migration scaffolding

### Evidence

`src/repl/state.c` contains:

- Macro aliases over `g_repl_state` fields.
- Comments about old macros and removed accessors.
- Large notes about moved editor/UI/presentation ownership.
- Notes about replay forwarders that are gone.

`src/repl/state.h` also contains an important caveat that scene catalog statics
in `scenes.c` are not covered by `repl_state_capture` / `repl_state_restore`.

### Why it hurts readability

The state layer is where ownership should be clearest. Inline migration history
was useful during the refactor, but now it makes the file harder to scan for
the current contract.

The scene-catalog caveat is important enough that it should influence naming
or API shape, not just a comment.

### Recommended cleanup

- Remove or condense migration comments that describe old shapes rather than
  current invariants.
- Replace local macro aliases with direct `g_repl_state.field` access where it
  improves clarity inside `state.c`.
- Consider renaming `repl_state_capture` if it intentionally excludes scene
  catalog state, or make scene snapshot/catalog state explicit in a separate
  API.

This cleanup should be kept separate from behavior changes.

## Finding 12: load transaction flow is clearer, but still implicit

### Evidence

`src/repl/load.c` has two main application paths:

- structured compiled changes,
- plain command insertion.

Both paths manage source-document writes, command-store writes, cursor
advancement, predef/scratch side effects, and rollback/error handling. The
recent rebase tightened `repl_apply_compiled_change()` with an internal
preflight, so the structured path's "cannot fail after preflight" assumption is
much stronger than before.

The flow is still duplicated and asymmetric to read: the plain command path has
explicit rollback if command-store insertion fails after source-document insert,
while the structured path applies text, predef ops, scratch ops, command-store
mutation, then alias ops under the implicit invariant that apply cannot fail
after the earlier gate.

### Why it hurts readability

The behavior is more defensible now, but the transaction model is still hard to
see. Readers have to compare both paths and `apply.c` to understand which
mutations happen before which validation and what gets rolled back on failure.

### Recommended cleanup

Factor a helper with an explicit contract:

```c
typedef struct ReplLoadTransactionResult {
    int applied;
    int wrote_local;
    int next_cursor;
} ReplLoadTransactionResult;

int repl_load_apply_compiled_change_transaction(
    const ReplCompiledChange *change,
    int cursor,
    ReplLoadTransactionResult *out);
```

The helper should either:

- perform symmetric rollback for source/predef/scratch/command-store changes,
  or
- document and assert the invariant that nothing after preflight can fail.

Either way, the transaction boundary becomes visible.

## Recommended cleanup order

Do not attempt this as one broad refactor. The safest sequence is:

1. **Migrate compatibility-facade includes.**
   Finding 1's implementation work is done, but many TUs still include
   `src/repl/core.h`. Replace those with focused owner headers and delete
   `core.h` once it has no callers.

2. **Add context-based source-scope APIs.** **Done on 2026-06-20.**
   Live wrappers remain, while parser/core compile paths now use explicit
   document views.

3. **Move visible-variable lookup onto the same explicit context path.**
   This removes one major exception from compile purity.

4. **Move parser strict-ref lookup onto the same explicit context path.**
   Function aliases are no longer mutated during compile, but strict call
   validation still reads the live document and alias table.

5. **Extract `SceneSnapshot` from `scenes.c`.**
   This gives scene slots, workspace IO, and state capture a clearer shared
   primitive.

6. **Centralize the remaining import/export shared constants and accessors.**
   `export_state.h` already handles dimensions; do this for directive names,
   C89 markers, and duplicated state-access macros before splitting either
   large file.

7. **Mechanically split `export.c` and flatten query helpers.**
   These are mostly physical organization wins once shared scaffolding is in
   place.

8. **Clean migration comments and local aliases in `state.c`.**
   Do this last, after ownership APIs settle, so comments do not churn twice.

## Suggested first slice

The best next implementation slice is a mechanical include migration:

1. Pick one cluster of `#include "repl/core.h"` callers, such as app
   controller files, editor files, or REPL pipeline files.
2. Replace the facade include with the specific owner headers used in that
   file (`host_effects.h`, `pipeline.h`, `scenes.h`, `export.h`,
   `state_notify.h`, and so on).
3. Build after each cluster so any accidental transitive-include dependency is
   caught locally.
4. When `rg '#include "repl/core.h"'` is empty, delete `core.h`.

## Verification strategy

For mechanical splits:

```bash
make check-c99
make test-stubs
```

For changes that touch parser, compile, source-scope, load, import, or export:

```bash
make test
make check-state-ownership
```

For portability-sensitive cleanup or any build-system-adjacent change, also run
the Linux real-GCC check described in `AGENTS.md`:

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
  git pull --ff-only origin main && \
  make check-c99 && make test-stubs'
```

## Open review questions

1. Should the remaining compile purity gap be fixed now, or should the comments
   first be updated to honestly document the visible-var/source-scope and predef
   dependencies?
2. Should `repl_state_capture` grow to include scene catalog state, or should
   scene snapshots become a separate named concept?
3. Should `core.h` remain a compatibility facade after `pipeline.h`, or should
   each caller be pushed to specific owner headers as the cleanup proceeds?
4. Should export/import be split before or after the parser/compile context
   cleanup? Splitting first improves readability sooner; context cleanup first
   reduces hidden dependencies before moving code around.
