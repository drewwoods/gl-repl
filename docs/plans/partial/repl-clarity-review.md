# `src/repl` Clarity, Coupling & Extensibility Review

## Status - FINDINGS 1-4 LANDED; 5-11 DEFERRED (2026-08-14)

Findings 1, 2, 3 and 4 - the four the sequencing section marks as worth doing
now - are implemented on branch `repl-cleanup`, one commit each. Everything
below is left verbatim so the deferred findings keep their context; each
landed finding carries a **LANDED** note under its heading.

Deferred, with the reason each was deferred rather than done:

| # | Why not now |
|---|---|
| 5 (two commit chains) | No cheap ratchet exists. Decide when the next structured form lands - see D3. |
| 6 (checkpoint invalidation tail) | Ready to do; not in the requested batch. |
| 7 (`ReplHostEffects` contract) | Comment-only fix, ready to do; the split itself waits for a 17th callback. |
| 8, 9 (import split, `g_`-prefixed macros) | Ride adjacent work; 8 must not pre-empt `one-scene-loader.md`. |
| 10 (builtin smoke loop) | Optional; dispatch is already shared by construction. |
| 11 (transform-scope walk) | Record only; no action recommended. |
| Architecture-document gaps | Four new `src/repl/ARCHITECTURE.md` sections plus the §12/README reconciliation. Not in the requested batch. |

One correction the work turned up, recorded here because the review's
finding 1 predicted a different set of source inventories: only two explicit
lists name `autonormal.c` (`REPL_DEMO_DEP_SRCS` and
`scripts/callgraph_file_groups.json`). `REPL_SRCS` is a `wildcard`, and the
state/app-boundary and editor-input guards derive from it, so they needed no
edit. Verifying `repl_demo` did surface an unrelated pre-existing break:
`doc_order.c` was missing from `REPL_DEMO_DEP_SRCS`, so the standalone
boundary proof had not linked since that module landed. Fixed in the same
commit.

A read-only review of `src/repl` (47 `.c` + 54 `.h`, ~45,300 lines) against
four questions: are responsibilities clear, is coupling necessary, will the
interfaces absorb the next feature, and is anything duplicated or overcomplicated.

No code was changed *by the review*. Every finding below carries a file:line
citation so it can be re-checked before anyone acts on it - note that the
citations are as of the review date and the four landed findings have since
moved some of the lines they name.

**Provenance.** First pass 2026-08-13. Second pass 2026-08-14 independently
re-read the pipeline, the host bridge, the new-command surface, and the
persistence band, then compared notes with the first draft. The verdict and
most of the original ten findings stand. Where this pass disagrees, the
disagreement is written first so it is not buried in a re-ranked list.
The plan itself was then reviewed against the live build/source inventories,
tests, and exact table-driven evaluator paths on 2026-08-14; that review
corrected several overstatements in the second-pass draft before remediation.

## Verdict first

**The module is in good shape and most of what looks like a problem turns out
to be a documented, defended decision.** This review deliberately did *not*
manufacture findings for the following, all of which were examined and found
sound:

- **The two-level command model** (`GLCmd[]` → flat program) and the decision to
  keep canonical text in the editor buffer rather than on `GLCmd`. The
  `repl_demo` link set is a real, load-bearing proof of the boundary.
- **The tagged-union `GLCmd.payload`** — keyed on `type`, documented member by
  member at `command.h:142-233`, with the zeroed-payload contract stated.
- **Two evaluators** (`eval.c` recursive-descent, `expr_program.c` compiled
  postfix). This looks like duplicated language semantics but carries an
  explicit parity contract and a fall-back-never-diverge policy
  (`expr_program.h:14-20`). That is the right shape for a perf-motivated
  second implementation.
- **Compile-is-pure / apply-mutates.** The contract at `compile.c:5-22` is real
  and honoured; `repl_load_apply_line` was already moved out to `load.c`
  precisely to protect it (see the note at `compile.c:3777`).
- **The `_kernel` / wrapper pairs** in `compile.c`. These look like duplication
  but are two genuinely different consumers: `src/editor/commit.c` needs the
  richer kernel struct, the loader path needs a `ReplCompiledChange`. The
  wrappers are thin and mechanical. `ARCHITECTURE.md` §4.3 already explains
  this well.
- **`command_spec.c`'s designated-initializer table** keyed by `CmdType`
  (`command_spec.c:790-795`) — row order is explicitly cosmetic, which is the
  right call.
- **`camera_header.c` and `doc_order.c`** are, on their own terms, two of the
  best-designed files in the module. `doc_order.h:1-42` in particular argues
  its own scope limits better than most projects' architecture docs.
- **`compile.c` size (~3,781 lines).** Large, but one coherent responsibility
  with a strong stated contract. A verb-boundary split was proposed in
  `plans/partial/src-repl-simplicity-review.md` and correctly deferred: it
  would force a private `compile_internal.h`, Makefile/guard churn, and a
  header web in exchange for nothing the kernels do not already give.

What follows is the residue: places where the design has drifted from its own
stated intent, or where the next feature will cost more than it should.

---

## Review history and disagreements

The first draft is a good review. D1-D4 are the places the second pass would
not sign its original wording; D5 records corrections found while reviewing
the revised plan itself.

### D1. The skill's "flatten_range is a required fifth edit" is false for ordinary GL commands

The first draft repeated the skill's claim that a new command must be added to
`flatten_range()` or it "renders once and vanishes on the next reflatten."
That is true of **structured / control-flow** commands. It is not true of a
bound GL/GLU/GLUT statement.

`flatten_range` (`flatten.c:1575-1657`) only special-cases control flow,
assignments, scratch writes, and source-only markers. Everything else falls
through to `flatten_reparse_line`, which re-evaluates from `command_spec`
tables. A new `k_std_command_specs` / `k_enum_command_specs` row is therefore
flattened automatically.

Measured, not argued: neither `a4056e54` (`glVertex4f`, 25 files) nor
`4c693a35` (`glPolygonMode` / `glPolygonOffset`, 17 files) touched
`flatten.c`. `docs/ARCHITECTURE.md` *Adding A New Command* already treats
flatten as a structured-syntax step, not a GL-command step. The skill is the
document that is wrong.

This matters because the first draft then measured "eight to seventeen files"
from those two commits and treated that number as the required surface. Those
commits also updated `USER_GUIDE.md`, `README.md`, `ARCHITECTURE.md`,
`CLAUDE.md`, tests, stubs, replay, overlays, and guides. The *required code*
surface for a table-driven GL command is smaller, and flatten is not on it.
The real omissions are `attrib_bits.c` and `gl_state_inspector.c` for state
commands, `replay_annotations.c` for the replay overlay, and
`command_descriptions.txt`. Finding 3 below is rewritten around that.

### D2. Finding 4 (host-effects split) is real but oversold as a top-four change

The first draft counted six callbacks with no `src/repl` caller. There are
**seven**: it missed `tutorial_presentation_reset`, whose only caller is
`tutorial_runner.c:121`. The table has 16 callbacks; 7 are unused inside
`src/repl`.

The diagnosis (the top-of-file contract is narrower than the table) is
correct. The recommended split into a second editor-owned bridge is the
largest change in the original set, undoes a previous consolidation that was
itself a review recommendation (`plans/partial/src-repl-simplicity-review.md`
Brittle #2, marked done), and the header already admits the second audience
at `host_effects.h:96` (*"Decoupled editor/completion mutations and reads for
subsystems"*). Prefer documenting the two groups in place. Split only if a
17th callback wants to land.

### D3. Finding 3 (two commit chains) has no cheap completeness ratchet

The kernels are already shared. Only the *order* the wrappers are offered in
can drift, and `ARCHITECTURE.md` §4.3 already writes that order down. A
shared enum sequence couples `src/editor/commit.c` to a compile-internal
table for a failure that has not happened yet. A classification corpus is
useful for the seven current forms, but it cannot detect a future form omitted
from the corpus itself, so it must not be sold as a structural fix. Defer the
choice: when a new structured form lands, either make a shared handler-kind
enum exhaustive in both dispatchers or add explicit acceptance coverage for
that form while retaining the duplicate order. The original "export the order
as an enum" remains reasonable if
`plans/not-started/float-returning-repl-functions.md` becomes the next consumer.

### D4. The dual scene loaders were omitted

`example_loader.c` and `import.c` still walk `.glr` with different error
policies, presentation-reset rules, and `@cfg` filters. That is a larger
structural split than several of the original ten findings, and it already
has a design of record: `plans/not-started/one-scene-loader.md`. This review
does not re-derive that plan. It records the gap so a reader of *this* review
is not left thinking persistence is "just import.c is large."

### D5. The second-pass remediation details needed five corrections

The findings still identify useful seams, but five proposed fixes were too
confident about their mechanics or protection:

- Moving the geometry-query implementation creates a new translation unit. It
  therefore also touches `REPL_DEMO_DEP_SRCS`, the callgraph grouping, and the
  explicit source lists in several boundary guards; it is not literally a
  one-include file move.
- The new-command skill is wrong about `parser.c` as well as `flatten.c` for an
  ordinary table-driven command. `command_spec.c` is what teaches the generic
  parser the spelling. The canonical checklist also omits the enforced
  `command_descriptions.txt` catalog.
- A corpus-based commit-path parity test characterizes the forms in its corpus,
  but cannot notice a future form omitted from both the editor chain and the
  corpus. It is useful regression coverage, not a completeness ratchet.
- `ImportState` does have implicit phases, but the cited nine fields are not
  nine booleans: `func_depth` is a counter and
  `active_staged_func_slot` is a tagged index. The "512 states" argument is
  invalid.
- Both expression evaluators already discover builtins from the same table and
  invoke the same function pointer. A table-driven smoke test would improve
  coverage, but the handwritten corpus is not the only thing preventing
  builtin-semantic divergence.

---

## Findings

Ranked by (cost of leaving it) × (cheapness of fixing it). The first four are
worth doing; 5-8 are worth doing when adjacent code is already open; 9-11 are
judgement calls flagged for the record.

---

### 1. `geometry_query.h` has no implementation file; it lives inside `autonormal.c`

> **LANDED** (`3c1dd665`). Moved verbatim to `src/repl/geometry_query.c`; joined
> `REPL_DEMO_DEP_SRCS` and `callgraph_file_groups.json`. The other inventories
> this section predicted turned out to derive from the `REPL_SRCS` wildcard.

**What.** Every function declared in `geometry_query.h` — the feeding-state
lookups, all six bracket matchers, and the three transform-scope walkers — is
implemented in `autonormal.c`, at lines 730-980:

| Declared in `geometry_query.h` | Defined in |
|---|---|
| `repl_find_feeding_normal_cmd` / `_color_cmd` | `autonormal.c:769,773` |
| `repl_find_matching_push_matrix` / `_pop_matrix` | `autonormal.c:808,813` |
| `repl_find_matching_push_attrib` / `_pop_attrib` | `autonormal.c:824,829` |
| `repl_find_matching_begin` / `_end` | `autonormal.c:838,842` |
| `repl_find_affecting_transforms` (+2 variants) | `autonormal.c:865,924,952` |

`autonormal.c`'s own banner (`autonormal.c:2`) already admits a second job
(*"Auto-generated normal commands and feeding-state lookup"*), so this is not
a file that pretends to be only normals. Lines 1-729 are still the autonormal
pass; lines 730-980 are an unrelated module: cursor-context source queries
that the render3d guides, the edit overlays and the code panel consume.

**Why it matters.** This is the single worst discoverability defect in the
module. A reader who wants to know how "the transform guides find which
transforms are in scope" has no path to the answer: the header names a file
that does not exist, and the file that does hold the code is named after a
different feature. (There are legitimate header-only files in this directory,
so the stronger "every header has a peer `.c`" claim is not true.) Two of the
three files that consume these queries live outside `src/repl` entirely, so the
mis-filing is visible from other bands.

**What to change.** Move `autonormal.c:730-980` into a new `geometry_query.c`
verbatim. The code seam is clean: `skip_function_body_backward`
(`autonormal.c:851`) and `append_unique_src_line` (`autonormal.c:916`) are both
private to the moved half, while `repl_source_scope_*` and
`TransformScopeScan` already come through public headers. This is a semantic
no-op, but not just an include edit: add `geometry_query.c` to
`REPL_DEMO_DEP_SRCS`, `scripts/callgraph_file_groups.json`, and every explicit
REPL source inventory that currently names `autonormal.c` (notably the
state/app-boundary and editor-input guards), so the standalone demo still links
and the new TU does not fall outside guard coverage. Then fix `src/repl/README.md`
and `src/repl/ARCHITECTURE.md` §12, and verify both `repl_demo` and the aggregate
guard suite.

---

### 2. The "immediate-mode vertex" subset has no predicate, so six call sites spell it by hand

> **LANDED** (`b057df1b`). `repl_cmd_emits_immediate_vertex()` added, six sites
> rewritten, `test_replay_walk.c`'s drift guard extended to pin the two
> predicates to each other. The `edit_overlays.c` re-emission ladder rode along
> as a file-private helper.

**What.** `command.h:270-282` defines `repl_cmd_emits_vertex()`, covering
`CMD_VERTEX2F/3F/4F` *and* `CMD_TESS_VERTEX`. But six call sites want the
immediate-mode subset only, and each re-derives it inline:

```
src/repl/autonormal.c:617     repl_cmd_emits_vertex(t) && t != CMD_TESS_VERTEX
src/repl/autonormal.c:742     repl_cmd_emits_vertex(t) && t != CMD_TESS_VERTEX
src/repl/flatten_query.c:382  !repl_cmd_emits_vertex(t) || t == CMD_TESS_VERTEX
src/repl/flatten_query.c:492  repl_cmd_emits_vertex(t) && t != CMD_TESS_VERTEX
src/repl/flatten_query.c:510  repl_cmd_emits_vertex(t) && t != CMD_TESS_VERTEX
src/subsystems/edit_overlays/edit_overlays.c:773
                              !repl_cmd_emits_vertex(t) || t == CMD_TESS_VERTEX
```

Three files, two bands, both polarities, and — because two sites use De Morgan's
form — the six are not even textually greppable as one pattern.

The comments at those sites are not empty: `autonormal.c:611-616` explains
that tess vertices never appear inside a `CMD_BEGIN` block;
`flatten_query.c:488-491` and `:506-509` explain that tess vertices have
their own color/normal feeders. The *set* is still the same set.

**Why it matters.** `command.h:272-276` explicitly *decides against* this
predicate: *"For tess-vs-immediate-mode distinctions … spell the subset out at
the call site - that's intent, not a uniform predicate."* That rationale was
defensible when there was one such site. At six identical spellings the
prediction has been falsified: this is not per-site intent, it is one concept
without a name. CLAUDE.md's own rule — *"`CmdType` set tests use the inline
predicates in `command.h` … never ad-hoc `||` chains"* — is being violated in
letter by code that is obeying it in spirit, because the predicate it needs
does not exist.

The concrete cost is visible in commit `a4056e54` (adding `glVertex4f`): it had
to touch all six sites plus `hidden_lines.c`'s own enumeration, and the risk on
the next vertex-family command is that one of six gets missed with no compiler
help.

**What to change.** Add one inline predicate to `command.h` next to its sibling —
`repl_cmd_emits_immediate_vertex(CmdType)`, returning true for
`CMD_VERTEX2F/3F/4F` only — and rewrite the six sites through it. Replace the
"spell it at the call site" paragraph with a pointer to the new predicate. This
is ~15 lines of diff and removes the module's most-repeated ad-hoc type test.

While there: `edit_overlays.c:770-790` and `:875-890` each carry a
type-dispatched `glVertex3f/2f/4f` re-emission ladder. A file-private helper in
`edit_overlays.c` could remove that local duplication. Do not share a GL-calling
inline with `executor.c`: the executor-only-GL guard and the REPL/subsystem
boundary make that the wrong abstraction.

---

### 3. The new-command skill is wrong and the canonical checklist is incomplete

> **LANDED** (`2b851964`). All four sub-items done: `docs/ARCHITECTURE.md` is
> now the single checklist (new steps 3b/3c), the skill points at it, and the
> `parser.c` / `flatten_range` / `IMPORT_EXPORT_STATE` / `editor_feed_line`
> claims are gone from the skill, CLAUDE.md and `docs/MODULES.md`. `compile.h`
> and `compile.c` corrected.

**What.** Adding a command is the module's main extension point, and the skill,
the canonical checklist, and the last representative state-command change do
not agree:

| Source | What it says a bound GL command needs |
|---|---|
| `.claude/skills/gl-repl-new-command/SKILL.md` | Five edits: `CmdType`, **`parser.c`**, executor, **`flatten_range`**, `command_spec`. The two bold steps are not required for an ordinary table-driven command. It also claims the `IMPORT_EXPORT_STATE` macro is duplicated across two TUs. |
| `docs/ARCHITECTURE.md` *Adding A New Command* | Eight steps: `command.h`, three `command_spec` tables, executor, **replay annotations**, F1 (via `k_func_completions`), stubs, export round-trip. Flatten is correctly listed only under the structured-syntax companion. |
| What `4c693a35` actually needed to make `glPolygonMode` usable | The architecture-doc list, **plus** `attrib_bits.c` and `gl_state_inspector.c`. The commit message is explicit that those two were *"what makes the commands actually usable."* Flatten was not touched. |

`gl_state_inspector.c:1082-1437` (`gl_state_apply_cmd`) is a second executor:
it re-implements the state semantics of every command `repl_apply_state_cmd`
(`executor.c:396-556`) emits, without issuing GL. `attrib_bits.c:137-169` is
the third per-command fold. Neither is named in the skill or in the
architecture checklist. The two ratchets that catch omissions (the
description-catalog generator and the tracked-cell sweep in
`tests/test_repl_state.c`) are not equivalent: the description generator fails
the build directly when a bound GL `CmdType` lacks a catalog entry, while the
tracked-cell sweep is gated by `attrib_bits` and therefore cannot report an
inspector omission until the author has classified the command there.

The generic parser is table-driven too: a normal `k_std_command_specs` or
`k_enum_command_specs` row is consumed without a command-specific edit in
`parser.c`. So the skill overstates two required code sites, not one. In the
other direction, `command_descriptions.txt` is required for every new bound
GL/GLU/GLUT `CmdType`; its generator is deliberately exhaustive over that set,
yet the canonical architecture checklist never names the authored catalog.

Smaller stale claims on the same on-ramp:

- CLAUDE.md, `docs/MODULES.md:712`, and the skill explicitly say import feeds
  geometry through `editor_feed_line()`; `docs/ARCHITECTURE.md:2651-2653`
  describes the same route more generally as the editor commit pipeline. It
  does neither. `import.c` goes through `repl_load_apply_line()` (`load.c`).
  `load.h:46-48` is explicit that this *replaces* `editor_feed_line` for
  non-editor callers.
- The same three documents still say *"The `IMPORT_EXPORT_STATE` macro block
  is deliberately duplicated verbatim across the two TUs."* **No such macro
  exists.** It was replaced by the single shared `export_format_shared.h`
  (`IMPORT_EXPORT_VIEW` / `IMPORT_EXPORT_WRITABLE`). The advice to "keep them
  in sync" is now actively misleading.
- `compile.h:275` says the dispatcher *"walks all six per-kind compile
  validators"* and then lists **seven** (`float_decl` → `var_assign` →
  `if_branch` → `close_brace` → `for_loop` → `func_def` → `if_block`).
- `compile.c:44-46` still describes that dispatcher as only the float-decl +
  var-assign chain, despite the same seven-entry table immediately below it.

**Why it matters.** This is an extensibility defect, not a documentation
footnote. The next planned state command is `glLightfv` / `glLightf`
(`plans/not-started/repl-capability-gaps.md` item 3). A reader who follows
the skill will edit `parser.c` and `flatten.c` unnecessarily, while still
skipping `attrib_bits` / the inspector and the enforced command-description
catalog. The OpenGL-state popup and the REPL's attribute-stack analysis can
then lie even though the real driver executes the command. Finding 4 is the
compiler-checked half of the same hole.

**What to change.** Make `docs/ARCHITECTURE.md` *Adding A New Command* the
single checklist, and shrink the skill to a pointer plus the specialized
guidance that the canonical document does not need to repeat:

1. Add `command_descriptions.txt` as an enforced step for every new bound
   GL/GLU/GLUT `CmdType`.
   Add `attrib_bits.c` and `gl_state_inspector.c` as conditional required steps
   for commands that write tracked GL state, and cite the `test_repl_state.c`
   coverage sweep.
2. Drop both `parser.c` and `flatten_range` from the skill's required baseline
   for table-driven bound commands. Point at the structured-syntax companion
   for the cases that really need custom parse/lowering work. Fix the matching
   "five required edits" summary in CLAUDE.md.
3. Delete the `IMPORT_EXPORT_STATE` sentence and the `editor_feed_line`
   import-path sentence from the skill, CLAUDE.md, `docs/MODULES.md`, and the
   canonical checklist's save/load step.
4. Fix `compile.h:275` ("six" → "seven") and the stale dispatcher summary at
   `compile.c:44-46`.

Do not invent a table of function pointers to "fold the switches." The
executor, the state fold, the attribute mapper and the C emitter each need
genuinely different per-command *behaviour*. Exhaustive switches plus an
accurate checklist are the extension point the module already has.

---

### 4. The attrib/inspector coverage ratchet is one-directional, and its gate is the switch most likely to be forgotten

> **LANDED** (`a200ce41`). `default:` dropped from `repl_attrib_bits_for_cmd`;
> the non-attribute-scoped types are enumerated. Verified the guard fires:
> removing one case now fails the build under `-Werror=switch`.

**What.** `tests/test_repl_state.c:2328-2341` is the sweep that commit
`4c693a35` (glPolygonMode/glPolygonOffset) credits with catching real gaps. Its
gate is:

```c
if (repl_attrib_bits_for_type(t, 0) == 0 || gl_state_cell_case_excused(t))
    continue;
```

It asserts: *if `attrib_bits` says this command carries attribute state, the
inspector must model it.* It does not assert the converse. And
`repl_attrib_bits_for_cmd` (`attrib_bits.c:137-169`) ends in `default: return 0;`,
which — unlike `executor.c:966` and `gl_state_inspector.c:1432`, both of which
enumerate `CMD_TYPE_COUNT` and are therefore `-Werror=switch`-enforced — means a
newly added `CmdType` silently answers "not attribute-scoped".

**Why it matters.** A new state command that its author forgets to classify in
`attrib_bits` returns 0, gets `continue`d by the ratchet, and the sweep passes.
The one guard designed to catch that omission is disabled by the omission. The
resulting defect is subtle and user-visible in the REPL's mirrors: the inspector
can restore/report the wrong value across a modeled `glPushAttrib` /
`glPopAttrib`, and the editor's per-bit push/pop highlights omit the setter.
The driver's real attribute stack still scopes a supported OpenGL command; the
failure here is analysis parity, not the GL call itself.

This matters more than it would in most codebases because
`docs/plans/not-started/repl-capability-gaps.md` has `glLightfv`/`glLightf`
queued as item 3 — new state commands are a planned, not hypothetical, event.

**What to change.** Drop the `default:` from `repl_attrib_bits_for_cmd` and
enumerate the non-attribute-scoped types explicitly (geometry, transforms,
control flow, the language primitives). `-Werror=switch` then makes it impossible
to add a `CmdType` without consciously answering "which attribute group, if
any?". The Makefile comment at line 178 already names this as the intended
idiom — *"dropping one is how a fold declares 'this list is exhaustive on
purpose'"* — and cites `gl_state_apply_cmd` as the precedent. `attrib_bits` has
a stronger claim to it than the inspector does, because its output gates the
inspector's own guard.

The `~90`-case switch this produces is verbose. That is the point: verbosity that
the compiler checks beats brevity that nothing does.

Do this *before* the `glLightfv` work.

---

### 5. Two commit chains encode the same load-bearing ordering, with nothing tying them together

**What.** The order in which input is offered to the compile validators is
spelled twice:

- `compile.c:87-99` — the `chain[]` function-pointer table used by
  `repl_compile_dispatch()` (the loader, import, and uncomment paths).
- `src/editor/commit.c:1085-1104` — `editor_try_commit_block_structs()` /
  `_var_statements()` / `_any()` (the interactive typing path).

`compile.c:76` says the table *"mirrors `editor_try_commit_any`"*, and
`compile.c:82-86` states the ordering is load-bearing in three specific ways
(`float_decl` before `var_assign`; `if_branch` before `close_brace`;
`close_brace` before the three block openers). Both lists are currently correct
and agree. The *handlers* are not twins — the editor wrappers add
header-replace / one-liner-body / paired-end branches on top of the shared
kernels — so a scene does not "parse differently when typed than when loaded"
today. What can drift is only classification: a new structured form added to
one chain and not the other.

**Why it matters.** There is no test, no guard, and no shared table asserting
they agree. A divergence would not fail the build. The next structured-syntax
consumer is `plans/not-started/float-returning-repl-functions.md`; that is
when this would actually bite.

**What to change.** Do not claim that a one-line corpus test closes this gap: it
only protects the forms someone remembered to put in the corpus, and a future
form omitted from one chain can also be omitted from the test. Such a test is
still useful characterization coverage for the seven current forms, but it is
not a completeness ratchet.

Defer the structural choice until a new form is actually added. In that change,
either export the *order* as a shared handler-kind enum and make both dispatchers
exhaustive over it, or add explicit parity coverage for the new form as an
acceptance test while retaining the documented duplicate lists. Do not unify
the handlers themselves; the kernel / wrapper split is load-bearing (see
"Explicitly not recommended").

---

### 6. `ReplCheckpointState` is `ReplRuntimeState` minus one field, hand-copied twice

**What.** `state.h:18-25` declares `ReplRuntimeState` (6 slices) and
`state.h:43-49` declares `ReplCheckpointState` — the same 5 of those 6, omitting
`flat_program`. `state.c:497-542` then hand-copies each slice in and out:

```c
out->document      = g_repl_state.document;
out->variables     = g_repl_state.variables;
out->render        = g_repl_state.render;
out->scene_runtime = g_repl_state.scene_runtime;
out->import_export = g_repl_state.import_export;
```

The post-restore invalidation block (rebind predef storage → `ensure_t_var_idx`
→ mark `source_uses_time_dirty` → invalidate source-scope cache → invalidate
expr cache → force flat dirty → clear `args_dirty_mask` / `rebake_ok`) is spelled
twice, at `state.c:480-494` and `state.c:525-541`, with near-identical comments.
The checkpoint restore also zeros `flat_program.cmd_count` /
`overflow_cmd_count` and calls `repl_state_flat_program_clear_current_block()`;
the full restore does not, because it copied the flat program in. That
difference is the only reason the two blocks are not identical.

**Why it matters.** Adding a seventh REPL-owned slice compiles cleanly while
being silently absent from every tour-baseline snapshot
(`src/app/glr_tour_snapshot.c`), because nothing connects the two structs. The
duplicated invalidation block is the more likely near-term bug: it is exactly the
kind of list that gets a new line added to one copy.

**What to change.** Extract the genuinely shared post-restore tail into one
helper and leave checkpoint-only flat-count/current-block clearing adjacent to
the checkpoint call. Prefer a named restore policy if the helper needs to
distinguish the two paths; a boolean `keep_flat_count` obscures the only semantic
difference.

Do not reshape all of `ReplRuntimeState` merely to make this copy shorter: that
would churn the state accessors and the explicit layout dump in
`src/app/glr_debug.c`. If another REPL-owned slice is added later, a shared
field-list macro (or a deliberately nested checkpoint payload) can make the
membership compile-checked in that same change. The current runtime-layout
static assertion catches a field omitted from its own dump, but does not connect
the runtime and checkpoint structs.

---

### 7. `ReplHostEffects` has drifted from "the pipeline's host bridge" into a general editor service locator

**What.** `host_effects.h:19-31` states the contract: *"Loader, scene-switch,
snippet-import, and replay code **in src/repl** call through this table."* Seven
of its sixteen callbacks have **no caller inside `src/repl` at all**:

| Callback | Only caller |
|---|---|
| `tutorial_presentation_reset` | `src/subsystems/tutorial/tutorial_runner.c` |
| `host_cursor_park` | `src/subsystems/tutorial/tutorial_runner.c` |
| `host_focus_line` | `src/subsystems/tutorial/tutorial_runner.c` |
| `completion_clear` | `src/subsystems/tutorial/tutorial_runner.c` |
| `completion_update` | `src/subsystems/tutorial/tutorial_runner.c` |
| `host_input_get` | `src/subsystems/tutorial/tutorial_runner.c` |
| `set_time_playing` | `src/app/glr_config.c`, `src/subsystems/replay/replay_playback.c` |

The header already admits the second audience at line 96. The top-of-file
contract was not updated when those hooks landed.

**Why it matters.** Six of the seven have `tutorial_runner.c` as their only
caller. Five let that peer subsystem drive the editor's cursor, input buffer
and autocomplete without linking to `src/editor`; the sixth requests the
tutorial-specific app presentation reset. The five editor operations are a
real dependency edge (tutorial runner → editor) routed through a table owned by
a third module that does not participate in it.

Two consequences, not three:

1. The header's stated contract is narrower than the table, so a reader
   cannot tell which callbacks are load-bearing for the pipeline and which
   are pass-through.
2. A table whose purpose is unclear grows without resistance.
   `host_focus_line`'s 10-line comment explaining how it differs from
   `host_cursor_park` is the visible symptom.

The first draft's third consequence — that `src/repl` has "practically
inverted" its "does not own editor state" claim — overstates this. The
controller still installs the table; `src/repl` only publishes dispatchers.
That is the same facade shape as `ReplExportConfigBridge`. The problem is
honesty of the contract, not a hidden editor dependency inside the pipeline.

**What to change.** First, rewrite the header contract to name both audiences
and group the callbacks in the struct (pipeline hooks, then tutorial/editor
hooks, then the clock hook). That is a comment change and stops the table
lying.

Split the table only if a 17th callback wants to land. If that happens: the
nine callbacks with real `src/repl` callers (`status`, `status_error`,
`example_presentation_reset`, `input_reset`, `insert_mode_off`,
`scroll_to_line`, `tutorial_teardown`, `edit_line_get`, `edit_line_set`) stay
as `ReplHostEffects`. The five tutorial-only editor hooks plus
`tutorial_presentation_reset` move to a tutorial-host table that the controller
installs for `tutorial_runner.c` directly. `set_time_playing` belongs with the
app/replay clock owner. Do not grow a third installer pattern — one extra
struct, still installed from `glr_ctrl.c`.

---

### 8. `import.c` is a 3,161-line monolith facing a six-file exporter, and `ImportState` is flag soup

**What.** The writer half is split by concern across six TUs (`export.c`,
`export_setup.c`, `export_prologue.c`, `export_display.c`, `export_cmd_writer.c`,
`export_glr.c`). The reader half is one file. Its own header comment
(`import.c:6-19`) already names five distinct responsibilities, which is a clean
seam list handed to whoever does the split:

1. the file-import state machine (line accumulation, dispatch, diagnostics),
2. the workspace header directive readers + `repl_state_parse_workspace_header_line`,
3. the camera-bridge import-line consumers,
4. the snippet directive table,
5. the C→REPL line translators (`import_make_repl_*_line` and their helpers,
   concentrated in `import.c:908-1904`).

Separately, `struct ImportState` (`import.c:92-118`) mixes phase, policy and
scanner state in adjacent scalar fields: `in_snippet` / `past_snippet` are a
phase pair; `func_depth` and `func_block_comment` are brace-scanner state;
`active_staged_func_slot` is a tagged index; and `check_order`, `order_failed`,
`allow_raw_scene`, and `has_func_body_markers` are format-policy/result flags.

A naming leftover sits next to the size: the reader's public API is still
`repl_export_load_from_file` / `_from_lines` / `_from_stream`, declared in
`export.h` (whose banner is *"Save/load of REPL sessions"*). There is no
`import.h`. Callers looking for the loader open the writer header.

**Why it matters.** The asymmetry alone is mild — a reader is naturally more
coupled than a writer, and `import_parse_payload_call` already factors the
translators' common shape well. The state layout is still harder to read than it
needs to be, but it is not a nine-boolean Cartesian product. The concrete phase
ambiguity is `in_snippet` plus `past_snippet`; the function-depth/comment and
staged-slot fields are legitimately orthogonal scanner state.
`import_process_line` → `import_try_*` (`import.c:2104-2420`) remains a
hand-ordered state machine whose phase order is implicit in handler order and
the snippet booleans rather than named.

This finding is **compatible with, and smaller than**,
`plans/not-started/one-scene-loader.md`. That plan retires
`example_loader.c`'s own `.glr` walk so format — not arrival route — picks the
reader. Extracting translators and naming the snippet phase does not collide
with it. Rewriting the import state machine in a way that assumes two `.glr`
walkers should wait for that plan, or be done as part of it.

**What to change.** Do not split the file for its own sake — split item 5 out
first (`import_line_translators.c` plus a narrow internal header). The cluster is
closer to 1,000 lines than 550 once its parsing helpers and the later local /
scratch translators are counted. It has no `ImportState` dependency, but it
does have shared evaluator/export-format helpers; expose only the body-line
translator and function-header translator needed by the state machine. That
pairs naturally with `export_cmd_writer.c` without manufacturing a public API.

Leave the state machine where it is until someone needs to touch it (likely
`one-scene-loader.md`). At that point, replace only `in_snippet` /
`past_snippet` with a named phase; keep function depth, block-comment state,
format policy, and the staged-function slot explicit. Renaming
`repl_export_load_*` → `repl_import_load_*` and cutting a thin `import.h` is
optional and should ride the same change, not lead it.

---

### 9. `export_format_shared.h` defines macros that impersonate globals

**What.** `export_format_shared.h:16-30` defines 14 macros of the form:

```c
#define g_workspace_header_lines      (IMPORT_EXPORT_VIEW.workspace_header_lines)
#define g_cam_lines_writable          (IMPORT_EXPORT_WRITABLE->cam_lines)
```

The shared-header macros are used by four TUs (`export_setup.c`, `import.c`,
`export.c`, `export_glr.c`); `scenes.c` independently redefines the same view /
writable pattern for three fields, so the naming idiom spans five TUs overall.

**Why it matters.** The `g_` prefix is the project's documented marker for a
**file-private static** (CLAUDE.md, Conventions). These are neither file-private
nor variables — each expands to a function call returning a by-value view struct
or a pointer. A reader who greps `g_cam_lines` finds no definition; a reader who
trusts the naming convention will assume a cheap load and may write it in a loop.
The read/write split being encoded as a `_writable` *name suffix* rather than as
C's own `const` is a second, smaller version of the same problem: the type system
already knows this and is being asked to stay quiet about it.

This was clearly a mechanical rename during the state-facade refactor that
preserved existing call sites — a reasonable transitional step that has now
outlived its transition.

**What to change.** Rename to something that reads as an accessor rather than
storage — `repl_ie_workspace_header_lines()` / `..._mut()` — or drop the macros
and let the ~31 call sites spell `IMPORT_EXPORT_VIEW.field` directly, which is
barely longer and honest about the indirection. Either way, stop spending the
`g_` prefix on things that are not file-private statics, because that prefix is
load-bearing everywhere else in the tree.

---

### 10. Builtin parity coverage is handwritten even though builtin dispatch is table-driven

**What.** `tests/test_expr_program.c:145` reads `/* builtins, all of them */`
above a hand-written list of expression strings. It is currently complete. Nothing
enforces that. `repl_eval_builtin_count()` / `repl_eval_builtin_at()`
(`eval.c:495-510`) already expose the builtin table for iteration.

**Why it matters.** This is a test-maintenance gap, not a duplicated-semantics
hazard. `expr_program.c:379-413` looks up every call through
`repl_eval_builtin_index_of()`, and its evaluator invokes the same function
pointer returned by `repl_eval_builtin_at()` (`expr_program.c:851-865`). Adding
a table row therefore teaches both paths the builtin automatically. What the
handwritten list can miss is compile/arity plumbing for the new row and the fact
that its "all" comment has become stale.

**What to change.** Keep the curated edge-case corpus, and add a generated smoke
loop over `repl_eval_builtin_at(0..count)` that constructs valid calls at
`arity_min` and (when different) `arity_max`, then runs each through
`check_parity`. Do not substring-search the handwritten corpus: `sin` inside
`asin` is an obvious false positive. Constants need no separate parity roster
today because both paths call the same `repl_eval_named_constant()` lookup.

Noted in passing: `round` appears in the test corpus, in `eval.h:7`, and in the
evaluator, but is absent from CLAUDE.md's and the scene-authoring skill's math
function lists.

---

### 11. The source-side transform-scope walk hand-rolls what `TransformScopeScan` already does

**What.** `repl_find_affecting_transforms_for_flat_vertex`
(`autonormal.c:924-949`) uses the shared `TransformScopeScan` from
`transform_utils.h` for its push/pop/load-identity accounting. Its source-side
twin, `repl_find_affecting_transforms` (`autonormal.c:865-910`), hand-rolls the
same accounting with a local `popped_depth` counter.

**Why it matters.** Low severity, flagged for completeness. The two walks are not
identical — the source walk additionally has to skip opaque function bodies and
stop at an enclosing `CMD_FUNC_DEF`, which the flat walk gets for free because
flattening already inlined everything (the comment at `autonormal.c:936-941`
explains this well). So the duplication is partial and the divergence is
justified. But the *scope-depth* half — three cases, ~10 lines — is genuinely the
same logic maintained twice, and it is the half where an off-by-one would be
hardest to spot.

**What to change.** Probably nothing right now. If `TransformScopeScan` is ever
touched, consider giving it an optional "stop at block head / skip nested func
body" policy so both callers share the depth accounting and differ only in
policy. Not worth a dedicated change. (Moves with finding 1 if that file split
lands first.)

---

## Already designed elsewhere — do not re-derive here

| Gap this review also saw | Design of record |
|---|---|
| Catalog `.glr` walk vs. `import.c` (different error policy, presentation reset, `@cfg` filter; F12 hard-blocks on a failed catalog load) | `plans/not-started/one-scene-loader.md` |
| Function-scoped locals force a full flatten when a global feeds them | `plans/not-started/local-aware-rebake.md` |
| `glLightfv` / float-returning functions / larger scratch | `plans/not-started/repl-capability-gaps.md` |

Finding 8 (import translators / named snippet phase) is sequenced so it can
land before or during `one-scene-loader.md`, not instead of it.

---

## Architecture-document gaps

`src/repl/ARCHITECTURE.md` §12 claims: *"The README carries the same map as a
flat table; this grouping is the mental model."* The two maps do not agree.

Five implementation modules and one contract header appear **nowhere in
`ARCHITECTURE.md`, not even in §12's file map**:

| Module | `.c` lines | In README map? | In ARCHITECTURE (any §)? |
|---|---|---|---|
| `gl_state_inspector.c` / `.h` | 2,221 | yes (one row) | **no** |
| `camera_header.c` / `.h` | 639 | **no** | **no** |
| `attrib_bits.c` / `.h` | 667 | **no** | **no** |
| `doc_order.c` / `.h` | 259 | **no** | **no** |
| `command_descriptions.c` / `.h` | 53 | yes | **no** |
| `stencil_limits.h` | n/a (45-line header) | **no** | **no** |

Two more are discussed in the body and then dropped from the §12 map, so the
"same map as the README" claim fails in the other direction too:

| Module | `.c` lines | Discussed in | Missing from §12 |
|---|---|---|---|
| `expr_program.c` / `.h` | 1,039 | §5.2 | yes |
| `flatten_expr.c` / `.h` | 305 | §5.2 | yes |
| `init_state.h` | n/a (45-line header) | README only | yes |

Four of the undocumented modules are not incidental helpers — they own
cross-module invariants that a reader must know before touching adjacent
code, and two of them are directly implicated in findings above.

### Sections that should exist

**§5.4 — The GL-state fold (`gl_state_inspector.c`).** The largest undocumented
surface in the module and, structurally, a *second executor*:
`gl_state_apply_cmd` (`gl_state_inspector.c:1082-1437`) re-implements the state
semantics of every command that `repl_apply_state_cmd`
(`executor.c:396-556`) emits, without issuing GL. A new reader has no way to
learn that this parallel implementation exists, let alone that it must be kept
in step. The section should state: what the fold is for (the OpenGL-state
popup), why it is a source-checkpoint fold rather than a `glGet*` read-back,
that it models OpenGL 2.1 initial defaults and generated `init()`/`display()`
writes as well as user commands, and — most importantly — that it is the
executor's shadow and both move together.

**§5.5 — Attribute-stack mapping (`attrib_bits.c`).** Owns a stated invariant
that currently lives only in a `.c` file comment (`attrib_bits.c:14-21`): *"A
cell's covering `GL_*_BIT` mask is a pure function of its identity (`cell_cover`)
and matches the attrib_bits `repl_attrib_cmd_writes` stamps on it - the two must
agree so a push saves exactly the cells a pop restores."* That is precisely the
kind of cross-function invariant `ARCHITECTURE.md` §10 exists to enumerate, and
it is absent from §10. The section should also document the coverage ratchet in
`test_repl_state.c` and its one-directional limitation (finding 4).

**§4.6 — The `.glr` document-order phase machine (`doc_order.c`).** A hard format
rule — four monotonic phases, one tolerated edge — that every loader must run or
"the format has two readings again" (`doc_order.h:36-42`). Someone adding a sixth
loader entry point needs this and will not find it. The header text is already
close to publication-ready; the section can largely quote it.

**§4.7 or an extension of §4.4 — The camera header (`camera_header.c`).** Carries
a contract that is easy to violate by optimising: *"Offering **every** line is a
contract, not an optimisation target - the reader counts braces from what it is
offered to know which region a tag sits in, and a caller that pre-filters
silently mis-scopes"* (`camera_header.h:22-26`). Also worth documenting is
`ReplCameraApplyMode`'s deliberate three-of-four enumeration
(`camera_header.h:45-60`), which is a good design decision currently invisible
outside the header.

### Smaller doc corrections found while reading

- **§12's map is stale in two directions.** It omits the modules above, lists
  `geometry_query.h` under "Frame flow" beside `program_query.c` as though it
  had its own TU (finding 1), and drops `expr_program.c` / `flatten_expr.c`
  even though §5.2 discusses them.
- **CLAUDE.md / MODULES.md / docs/ARCHITECTURE.md / the new-command skill**
  still describe the obsolete editor commit path for import (CLAUDE.md,
  MODULES.md, and the skill name `editor_feed_line`; the architecture checklist
  says "commit pipeline"). CLAUDE.md, MODULES.md, and the skill also describe a duplicated
  `IMPORT_EXPORT_STATE` macro, while the skill/CLAUDE summary sends ordinary
  commands through command-specific `parser.c` and `flatten.c` edits. All are
  stale (finding 3).
- **`round` is missing** from CLAUDE.md's and the scene-authoring skill's math
  function lists (finding 10).

---

## Recommended sequencing

Findings 1, 2 and the doc corrections are independently landable and carry low
behavioural risk — do them first and in any order.

1. **Finding 1** — move `autonormal.c:730-980` → `geometry_query.c`, including
   the standalone-demo, callgraph, and guard source inventories.
2. **Finding 2** — add `repl_cmd_emits_immediate_vertex()`, rewrite six sites.
3. **Finding 3** — one checklist. Cheapest, and currently sending readers in
   the wrong direction (`parser.c` / flatten falsely required; description
   catalog / inspector / attrib_bits invisible; `IMPORT_EXPORT_STATE` /
   `editor_feed_line` claims false).
4. **Finding 4** — drop `attrib_bits`' `default:`. Do this *before* the
   `glLightfv` work in `repl-capability-gaps.md` §3, which is the next new
   state command and the case the ratchet gap is waiting to miss.
5. **Architecture-document gaps** — add the GL-state fold, attribute mapping,
   document-order, and camera-header contracts; reconcile §12 with the README.
6. **Finding 6** — extract the shared post-restore invalidation.
7. **Finding 5** — no dedicated change now. Choose an exhaustive shared
   handler-kind enum or explicit new-form parity coverage when the next
   structured form lands.
8. **Finding 7** — rewrite the `ReplHostEffects` contract comment to name both
   audiences. Split only if a 17th callback wants to land.
9. **Findings 8 and 9** — when adjacent code is next open. Finding 8 should not
   pre-empt `one-scene-loader.md`.
10. **Finding 10** — optional table-driven builtin smoke coverage; the dispatch
    is already shared by construction.
11. **Finding 11** — record only; no action recommended.

## Explicitly not recommended

- **Do not fold the per-command switches into `command_spec.c`.** The executor,
  the state fold, the attribute mapper and the C emitter each need genuinely
  different per-command *behaviour*, not per-command *data*. A table of function
  pointers would relocate the switches without reducing them and would cost the
  `-Werror=switch` exhaustiveness that three of the four currently get for free.
  The right fix for the extensibility cost is finding 4 (make the fourth switch
  exhaustive too) plus finding 3 (an accurate checklist) — not a new
  abstraction.
- **Do not merge the two evaluators.** The parity contract plus fallback is the
  correct design for a perf-motivated second implementation. Builtins already
  share one table and function pointers; finding 10 adds generated compile/arity
  smoke coverage without changing that design.
- **Do not split `compile.c`** on size alone. At 3,781 lines it is large, but it
  is one coherent responsibility with a strong stated contract, and every entry
  point is a peer of the others. The previous simplicity review's
  `compile_var.c` / `compile_block.c` split is still the wrong trade: private
  header, Makefile/guard churn, nothing the kernels do not already give.
- **Do not unify the `_kernel` / wrapper pairs.** Two real consumers, thin glue.
- **Do not split `ReplHostEffects` as a dedicated change.** Document the two
  groups. Split only as a rider on the next hook that would otherwise be the
  17th callback.
- **Do not re-derive the dual-loader fix here.** That work is
  `one-scene-loader.md`.
