# `src/repl` structure and readability audit - review pending

Status: **in-review** - this is a findings document and cleanup map. No
implementation has started. Do not move this to `plans/active/` until a
specific first slice is chosen.

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

## Summary table

| Area | Main files | Smell | Recommended cleanup |
|---|---|---|---|
| Core facade | `src/repl/core.c`, `src/repl/core.h` | Residual catch-all API for host effects, save/load wrappers, scenes, time, cursor/feed queries, and metrics; `pipeline.h` is a useful start but does not drain the facade. | Split host effects and remaining reformat/bootstrap/query APIs into focused headers. |
| Compile contract | `src/repl/compile.c`, `src/repl/compile.h` | Alias mutation is now fixed, but visible-var/source-scope and predef reads still make compile less pure than the header claims. | Thread visible vars / source scope / predef symbols through explicit context, or document the remaining transitional reads. |
| Parser contract | `src/repl/parser.c`, `src/repl/parser.h` | Parser is documented as stateless, but strict function-call parsing reads live document and aliases. | Pass symbols through `ReplParseContext`; keep live lookup outside parser. |
| Source scope | `src/repl/source_scope.c`, `src/repl/source_scope.h` | Query APIs hide a live-state prefix-depth cache. | Add explicit document-view APIs and keep live wrappers only for UI/app callers. |
| Internal header | `src/repl/core_internal.h` | Narrower after the rebase, but still bundles normalize entry points, text helpers, and visible-var collection. | Split into `normalize.h`, `visible_vars.h`, and direct includes where useful. |
| Scene slots/workspace | `src/repl/scenes.c` | One file owns scene slots, snapshots, promotion, workspace IO, cfg/camera/predef capture, and restore mechanics. | Extract `SceneSnapshot` and workspace IO helpers. |
| Import/export | `src/repl/import.c`, `src/repl/export.c`, `src/repl/export_state.h` | Shared dimensions moved to `export_state.h`, but state-access macros, snippet/C89 constants, global import accumulators, and very large emit/load flows remain. | Centralize the remaining shared constants/accessors; then split export/import by concern. |
| Flatten | `src/repl/flatten.c` | Lowering is mixed with current-block highlight, cost queries, and cursor matching. | Move read/query helpers to `flatten_query.c` or similar. |
| State layer | `src/repl/state.c`, `src/repl/state.h` | Migration history and macro aliases obscure current ownership boundaries. | Keep current invariants inline; move historical notes to plans/docs. |
| Load transaction | `src/repl/load.c`, `src/repl/apply.c` | Apply preflight is now stronger, but load still has two visible transaction flows and an implicit "cannot fail after preflight" structured path. | Factor or document the compiled-change transaction boundary explicitly. |

## Finding 1: `core.h` and `core.c` are still a residual facade

### Evidence

`src/repl/core.c` starts with a file comment that explicitly calls the module a
residual staging area:

- `repl_parse_and_normalize`
- `repl_reformat_program`
- `load_initial_commands`
- `current_begin_mode`
- `count_vertices`

The public header `src/repl/core.h` now exposes several unrelated surfaces:

- Save/load and workspace APIs.
- The `ReplHostEffects` bridge and all dispatch wrappers.
- Example and user-scene helpers.
- Time/load/reformat helpers.
- Cursor/feed/tune/metric queries.

Several modules include `core.h` for a narrow reason, usually one dispatch hook
or one legacy helper, and inherit the whole facade dependency.

### Why it hurts readability

When a file includes `core.h`, it is not clear whether it wants compiler
helpers, host effects, scene management, runtime time state, or user-scene
queries. This makes dependency direction harder to inspect and makes new
callers more likely to use the facade instead of the owning module.

The file comment is correct, but that also means the codebase is carrying an
explicit "to be dissolved" module as a central public API.

### Recommended cleanup

Split by role:

| New area | Possible files | Moves from `core.*` |
|---|---|---|
| Host bridge | `src/repl/host_effects.h`, `src/repl/host_effects.c` | `ReplHostEffects`, registration, status/cursor/input/time dispatchers. |
| Normalize | `src/repl/normalize.h`, `src/repl/normalize.c` | `repl_parse_and_normalize*`, normalization helpers. |
| Reformat | `src/repl/reformat.h`, `src/repl/reformat.c` | `repl_reformat_program`. |
| Bootstrap/load helpers | `src/repl/bootstrap.h`, `src/repl/bootstrap.c` | `repl_load_initial_commands` if it does not belong in load/scenes. |
| Queries/metrics | `src/repl/program_query.h`, `src/repl/program_query.c` | `repl_current_begin_mode`, `repl_count_vertices`, feed/cursor/tune queries where appropriate. |

`src/repl/pipeline.h` and `src/repl/format.h` are already steps in this
direction. They make some controller-facing and formatting dependencies
narrower, but `core.h` still owns the host bridge and several unrelated query /
scene / time surfaces.

This can be done incrementally. The lowest-risk first slice remains extracting
`ReplHostEffects`, because many modules include `core.h` only for dispatch
wrappers.

## Finding 2: compile purity is improved, but still not fully true

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

### Evidence

`src/repl/parser.h` describes the parser as stateless: parsing the same line
with the same context should produce the same result without touching the live
editor or command list.

`src/repl/parser.c` still performs live semantic lookup in strict function-call
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

### Recommended cleanup

- Extend `ReplParseContext` with a symbol/source view for strict parsing.
- Move function-definition and alias lookup behind that context.
- Keep live-state convenience wrappers outside the parser for callers that
  intentionally parse against the current document.
- Consider moving canonical text emission into a formatter layer once the live
  dependency is removed. The parser can still return enough structured data for
  canonical formatting; it should not need to know the live document.
- Replace the largest recognition ladder with a table of handler functions over
  time. This is a readability improvement, not an urgent correctness fix.

## Finding 4: `source_scope` query APIs hide live-state cache behavior

### Evidence

`src/repl/source_scope.c` owns static prefix-depth cache arrays and rebuilds
them by reading `repl_state_document_count()` and `repl_state_document_cmds()`.
Several source-scope functions look like pure queries from their names, but
they operate on the current live document implicitly.

Compile and parser code call into source-scope helpers while otherwise passing
explicit parse/compile contexts.

### Why it hurts readability

This is the deepest form of hidden coupling in the current `src/repl` shape.
Callers that appear to be context-based are still bound to the live document
because a helper underneath them reads `g_repl_state`.

It also makes testing harder. Tests need the live document arranged correctly,
even when the function under test appears to accept all of the needed context.

### Recommended cleanup

Add explicit source-scope view APIs:

```c
typedef struct ReplSourceScopeView {
    const GLCmd *cmds;
    int count;
    /* optional cache storage or cache handle */
} ReplSourceScopeView;
```

Then provide two layers:

- `*_view` functions that operate only on the passed document view.
- Existing live wrappers that build/use the current global document view for
  app/UI callers.

After that split, compile and parser should use the view APIs exclusively.

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

Split mechanically after shared import/export constants are centralized:

| New file | Responsibility |
|---|---|
| `export_workspace_header.c` | Workspace/snippet header directives. |
| `export_init.c` | Bootstrap, init, light/state setup. |
| `export_cmd_writer.c` | Command-to-C emission. |
| `export_names.c` | Generated names and collision avoidance. |
| `export_display.c` | Display passes and render policy. |
| `export_tune.c` | Tune helpers. |
| `export_scaffold.c` | Main/footer/keyboard/tick scaffolding. |

Keep `export.c` as the orchestration layer around the public save function.

Also make export render policy explicit, for example:

```c
typedef struct ReplExportRenderPolicy {
    int include_axes;
    int include_grid;
    int include_overlays;
    int include_point_guides;
} ReplExportRenderPolicy;
```

If export intentionally disables some overlays, that should be visible as
policy, not as ignored cfg reads.

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

1. **Extract host effects from `core.h/core.c`.**
   This reduces include fan-out without changing compiler behavior.

2. **Finish narrowing `core_internal.h`.**
   The rebase removed legacy re-exports. Move the remaining visible-var and
   normalize declarations to specific homes when those owners are chosen.

3. **Add context-based source-scope APIs.**
   Keep live wrappers, but make compile/parser use explicit document views.

4. **Move visible-variable lookup onto the same explicit context path.**
   This removes one major exception from compile purity.

5. **Move parser strict-ref lookup onto the same explicit context path.**
   Function aliases are no longer mutated during compile, but strict call
   validation still reads the live document and alias table.

6. **Extract `SceneSnapshot` from `scenes.c`.**
   This gives scene slots, workspace IO, and state capture a clearer shared
   primitive.

7. **Centralize the remaining import/export shared constants and accessors.**
   `export_state.h` already handles dimensions; do this for directive names,
   C89 markers, and duplicated state-access macros before splitting either
   large file.

8. **Mechanically split `export.c` and flatten query helpers.**
   These are mostly physical organization wins once shared scaffolding is in
   place.

9. **Clean migration comments and local aliases in `state.c`.**
   Do this last, after ownership APIs settle, so comments do not churn twice.

## Suggested first slice

The best first implementation slice is:

1. Add `src/repl/host_effects.h`.
2. Move `ReplHostEffects` and `repl_dispatch_*` declarations out of
   `core.h`.
3. Move the corresponding implementation out of `core.c`.
4. Update includes that only need host dispatchers.
5. Keep `core.h` including `host_effects.h` temporarily if that makes the first
   patch smaller, then remove the compatibility include in a follow-up.

Why this slice first:

- It is low semantic risk.
- It addresses the most visible dependency smell.
- It makes later `core.h` shrinkage easier to review.
- It does not require solving parser/compile context purity in the same patch.

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
