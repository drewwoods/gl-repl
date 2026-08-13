# `src/repl` Clarity, Coupling & Extensibility Review

## Status - NOT STARTED (2026-08-13)

A read-only review of `src/repl` (47 `.c` + 54 `.h`, ~45,300 lines) against
four questions: are responsibilities clear, is coupling necessary, will the
interfaces absorb the next feature, and is anything duplicated or overcomplicated.

No code was changed. Every finding below carries a file:line citation so it can
be re-checked before anyone acts on it.

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
  wrappers are thin and mechanical.
- **`command_spec.c`'s designated-initializer table** keyed by `CmdType`
  (`command_spec.c:790-795`) — row order is explicitly cosmetic, which is the
  right call.
- **`camera_header.c` and `doc_order.c`** are, on their own terms, two of the
  best-designed files in the module. `doc_order.h:1-42` in particular argues
  its own scope limits better than most projects' architecture docs.

What follows is the residue: ten places where the design has drifted from its
own stated intent, or where the next feature will cost more than it should.

---

## Findings

Ranked by (cost of leaving it) × (cheapness of fixing it). The first four are
worth doing; 5-8 are worth doing when adjacent code is already open; 9-10 are
judgement calls flagged for the record.

---

### 1. `geometry_query.h` has no implementation file; it lives inside `autonormal.c`

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

`autonormal.c` is described everywhere — its own header comment, the README file
map, `ARCHITECTURE.md` §12 — as "auto-generated `glNormal3f` maintenance". Lines
1-729 are exactly that. Lines 730-980 are an unrelated module: cursor-context
source queries that the render3d guides, the edit overlays and the code panel
consume, and that have nothing to do with normals.

**Why it matters.** This is the single worst discoverability defect in the
module. A reader who wants to know how "the transform guides find which
transforms are in scope" has no path to the answer: the header names a file that
does not exist, and the file that does hold the code is named after a different
feature. It also inverts the module's own convention — every other `foo.h` in
`src/repl` has a `foo.c`. Two of the three files that consume these queries live
outside `src/repl` entirely, so the mis-filing is visible from other bands.

**What to change.** Move `autonormal.c:730-980` into a new `geometry_query.c`
verbatim. The seam is clean: the only shared helper across it is
`repl_source_scope_*`, already an external include. `skip_function_body_backward`
(`autonormal.c:851`) and `append_unique_src_line` (`autonormal.c:916`) are both
private to the moved half. This is a pure file move — no signature changes, no
behaviour change, one `#include` added and one removed. Then fix the `README.md`
and `ARCHITECTURE.md` §12 entries, which currently group `geometry_query.h` with
`program_query.c` as if they were peers.

---

### 2. The "immediate-mode vertex" subset has no predicate, so six call sites spell it by hand

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

**Why it matters.** `command.h:272-276` explicitly *decides against* this
predicate: *"For tess-vs-immediate-mode distinctions … spell the subset out at
the call site - that's intent, not a uniform predicate."* That rationale was
defensible when there was one such site. At six identical spellings the
prediction has been falsified: this is not per-site intent, it is one concept
without a name. CLAUDE.md's own rule — *"`CmdType` set tests use the inline
predicates in `command.h` … never ad-hoc `||` chains"* — is being violated in
letter by code that is obeying it in spirit, because the predicate it needs does
not exist.

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
type-dispatched `glVertex3f/2f/4f` re-emission ladder that duplicates the
executor's. Worth a shared inline emitter, but that is a separate, smaller call.

---

### 3. Two commit chains encode the same load-bearing ordering, with nothing tying them together

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
and agree.

**Why it matters.** There is no test, no guard, and no shared table asserting
they agree — I checked `tests/test_repl_compile.c` and the
`check-state-ownership` guard set. A divergence would not fail the build; it
would produce a scene that parses differently when typed than when loaded from
the file it was just exported to. That failure mode is invisible to every
single-path test, because each path is individually self-consistent.

This is the same class of defect the project already spends effort on elsewhere:
`test_replay_walk.c` carries a drift test for the control-flow-vs-visual
taxonomy, and `test_repl_state.c` carries the attrib/inspector coverage sweep.
This pair has no such tie.

**What to change.** Cheapest credible fix: have `src/editor/commit.c` derive its
order from one exported table rather than restating it. If the two paths need
genuinely different handlers (they do — kernels vs. `ReplCompiledChange`), export
the *order* as a shared enum sequence and index both dispatchers off it. Failing
that, a drift test that walks a corpus of one-line inputs through both paths and
asserts identical classification is maybe 60 lines and catches the whole class.

---

### 4. `ReplHostEffects` has drifted from "the pipeline's host bridge" into a general editor service locator

**What.** `host_effects.h:19-31` states the contract: *"Loader, scene-switch,
snippet-import, and replay code **in src/repl** call through this table."* Six of
its sixteen callbacks have **no caller inside `src/repl` at all**:

| Callback | Only caller |
|---|---|
| `host_cursor_park` | `src/subsystems/tutorial/tutorial_runner.c` |
| `host_focus_line` | `src/subsystems/tutorial/tutorial_runner.c` |
| `completion_clear` | `src/subsystems/tutorial/tutorial_runner.c` |
| `completion_update` | `src/subsystems/tutorial/tutorial_runner.c` |
| `host_input_get` | `src/subsystems/tutorial/tutorial_runner.c` |
| `set_time_playing` | `src/app/glr_config.c`, `src/subsystems/replay/replay_playback.c` |

**Why it matters.** Five of the six exist so that `tutorial_runner.c` — a peer
subsystem, not pipeline code — can drive the editor's cursor, input buffer and
autocomplete without linking to `src/editor`. That is a real dependency edge
(tutorial runner → editor) routed invisibly through a table owned by a third
module that does not participate in it.

Three consequences, in increasing order of seriousness:

1. The header's stated contract is false, so it stops being a reliable guide to
   what the table is for — and a table whose purpose is unclear grows without
   resistance. `host_focus_line`'s 10-line comment explaining how it differs from
   `host_cursor_park` is the visible symptom.
2. `src/repl`'s boundary claim ("does not own editor state") is technically
   preserved while being practically inverted: the REPL layer now *publishes the
   API* by which everyone else mutates editor state.
3. A `src/repl` reader cannot tell which callbacks are load-bearing for the
   pipeline and which are pass-through, so none can safely be removed.

**What to change.** Split the table by owner, not by convenience. The nine
callbacks with real `src/repl` callers (`status`, `status_error`,
`example_presentation_reset`, `input_reset`, `insert_mode_off`, `scroll_to_line`,
`tutorial_teardown`, `edit_line_get`, `edit_line_set`) stay as
`ReplHostEffects`. The five tutorial-only editor hooks move to an
editor-owned bridge that `tutorial_runner.c` installs against directly — the
dependency then appears in the link set where it actually is.
`set_time_playing` belongs with the app/replay clock owner. Net effect: the
`src/repl` bridge shrinks by a third and starts describing its own contract
accurately again.

---

### 5. `ReplCheckpointState` is `ReplRuntimeState` minus one field, hand-copied twice

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

**Why it matters.** Adding a seventh REPL-owned slice compiles cleanly while
being silently absent from every tour-baseline snapshot
(`src/app/glr_tour_snapshot.c`), because nothing connects the two structs. The
duplicated invalidation block is the more likely near-term bug: it is exactly the
kind of list that gets a new line added to one copy.

**What to change.** Two options; the first is cleaner.

- Give `ReplRuntimeState` a nested-struct shape where the checkpointed slices are
  one member and `flat_program` is the other, so the checkpoint is one assignment
  and a new slice lands in both by construction.
- Or, minimally: extract the shared tail into
  `repl_state_after_restore_invalidate(int keep_flat_count)` and call it from
  both restores. That kills the more dangerous half of the duplication for ~20
  lines of diff.

---

### 6. The attrib/inspector coverage ratchet is one-directional, and its gate is the switch most likely to be forgotten

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
resulting defect is subtle and user-visible: `glPushAttrib`/`glPopAttrib` silently
fails to scope the new state, and the editor's push/pop gutter markers point at
the wrong lines.

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

---

### 7. `import.c` is a 3,161-line monolith facing a six-file exporter, and `ImportState` is flag soup

**What.** The writer half is split by concern across six TUs (`export.c`,
`export_setup.c`, `export_prologue.c`, `export_display.c`, `export_cmd_writer.c`,
`export_glr.c`). The reader half is one file. Its own header comment
(`import.c:6-19`) already names five distinct responsibilities, which is a clean
seam list handed to whoever does the split:

1. the file-import state machine (line accumulation, dispatch, diagnostics),
2. the workspace header directive readers + `repl_state_parse_workspace_header_line`,
3. the camera-bridge import-line consumers,
4. the snippet directive table,
5. the C→REPL line translators (`import_make_repl_*_line`, ~10 functions,
   `import.c:1068-1600`).

Separately, `struct ImportState` (`import.c:92-118`) carries **nine** interacting
ints that encode parse position: `check_order`, `order_failed`, `in_snippet`,
`past_snippet`, `func_depth`, `func_block_comment`, `allow_raw_scene`,
`has_func_body_markers`, `active_staged_func_slot`.

**Why it matters.** The asymmetry alone is mild — a reader is naturally more
coupled than a writer, and `import_parse_payload_call` already factors the
translators' common shape well. The flag set is the real cost: nine booleans
nominally describe 512 states, of which a handful are reachable, and nothing
writes down which. Every new import feature has to reason about the whole
product. `import_process_line` → `import_try_*` (`import.c:2104-2420`) is a
hand-rolled state machine whose states are implicit in flag combinations rather
than named.

**What to change.** Do not split the file for its own sake — split item 5 out
first (`import_line_translators.c`, ~550 self-contained lines with one entry
point each and no `ImportState` dependency), which is the piece that pays off
immediately by pairing 1:1 with `export_cmd_writer.c`. Leave the state machine
where it is until someone needs to touch it, then replace the position flags with
one named `ImportPhase` enum plus the two or three genuinely orthogonal flags
that survive. Both steps are independently landable.

---

### 8. `export_format_shared.h` defines macros that impersonate globals

**What.** `export_format_shared.h:16-30` defines 14 macros of the form:

```c
#define g_workspace_header_lines      (IMPORT_EXPORT_VIEW.workspace_header_lines)
#define g_cam_lines_writable          (IMPORT_EXPORT_WRITABLE->cam_lines)
```

Used across five TUs (`export_setup.c` ×13, `import.c` ×7, `scenes.c` ×4,
`export.c` ×4, `export_glr.c` ×3).

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

### 9. The `expr_program` ↔ `eval` parity corpus is hand-maintained behind a comment that claims completeness

**What.** `tests/test_expr_program.c:145` reads `/* builtins, all of them */`
above a hand-written list of expression strings. It is currently complete. Nothing
enforces that. `repl_eval_builtin_count()` / `repl_eval_builtin_at()`
(`eval.c:495-510`) already expose the builtin table for iteration — autocomplete
uses them.

**Why it matters.** The two evaluators' parity contract (`expr_program.h:14-20`)
is the thing that makes having two evaluators safe. A new builtin added to
`eval.c` and to `expr_program.c`'s `compile_primary` but not to this corpus is
untested for parity, and the comment will still say "all of them". The failure is
silent and the corpus is the only thing standing between a divergence and a
scene that renders differently on the warm path than the cold one.

**What to change.** Add a coverage assertion that walks
`repl_eval_builtin_at(0..count)` and fails if a builtin's name appears in no
corpus entry. ~15 lines, converts a comment into a ratchet. The same treatment
applies to the constants (`PI`, `TAU`, `e`) if `repl_eval_named_constant` grows a
table.

Noted in passing: `round` appears in the test corpus and in the evaluator but is
absent from CLAUDE.md's and the scene-authoring skill's math-function lists.

---

### 10. The source-side transform-scope walk hand-rolls what `TransformScopeScan` already does

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
policy. Not worth a dedicated change.

---

## Architecture-document gaps

`src/repl/ARCHITECTURE.md` §12 claims: *"The README carries the same map as a
flat table; this grouping is the mental model."* The two maps do not agree.
Four substantive modules — ~2,900 lines, including the module's second-largest
TU — appear **nowhere in `ARCHITECTURE.md`, not even in §12's file map**:

| Module | Lines | In README map? | In ARCHITECTURE (any §)? |
|---|---|---|---|
| `gl_state_inspector.c` / `.h` | 2,320 | yes (one row) | **no** |
| `camera_header.c` / `.h` | 870 | **no** | **no** |
| `attrib_bits.c` / `.h` | 803 | **no** | **no** |
| `doc_order.c` / `.h` | 355 | **no** | **no** |
| `command_descriptions.c` / `.h` | 78 | yes | **no** |
| `stencil_limits.h` | 45 | **no** | **no** |

Three of these are not incidental helpers — they own cross-module invariants
that a reader must know before touching adjacent code, and two of them are
directly implicated in findings above.

### Sections that should exist

**§5.4 — The GL-state fold (`gl_state_inspector.c`).** The largest undocumented
surface in the module and, structurally, a *second executor*:
`gl_state_apply_cmd` (`gl_state_inspector.c:1082-1437`) re-implements the state
semantics of every command that `repl_apply_state_cmd`
(`executor.c:396-556`) emits, without issuing GL. A new reader has no way to
learn that this parallel implementation exists, let alone that it must be kept in
step. The section should state: what the fold is for (the OpenGL-state popup),
why it is a source-checkpoint fold rather than a `glGet*` read-back, that it
models OpenGL 2.1 initial defaults and generated `init()`/`display()` writes as
well as user commands, and — most importantly — that it is the executor's shadow
and both move together.

**§5.5 — Attribute-stack mapping (`attrib_bits.c`).** Owns a stated invariant
that currently lives only in a `.c` file comment (`attrib_bits.c:14-21`): *"A
cell's covering `GL_*_BIT` mask is a pure function of its identity (`cell_cover`)
and matches the attrib_bits `repl_attrib_cmd_writes` stamps on it - the two must
agree so a push saves exactly the cells a pop restores."* That is precisely the
kind of cross-function invariant `ARCHITECTURE.md` §10 exists to enumerate, and
it is absent from §10. The section should also document the coverage ratchet in
`test_repl_state.c` and its one-directional limitation (finding 6).

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

- **§12's map is stale in two directions.** It omits the four modules above, and
  it lists `geometry_query.h` under "Frame flow" beside `program_query.c` as
  though it had its own TU (finding 1).
- **CLAUDE.md and `.claude/skills/gl-repl-new-command/SKILL.md` both assert:**
  *"The `IMPORT_EXPORT_STATE` macro block is deliberately duplicated verbatim
  across the two TUs."* **No such macro exists.** It was replaced by the single
  shared `export_format_shared.h` (`IMPORT_EXPORT_VIEW` / `IMPORT_EXPORT_WRITABLE`),
  which both TUs include. The advice to "keep them in sync" is now actively
  misleading — there is nothing to sync.
- **The new-command skill says "Five edits, all required."** The real footprint,
  measured from commits `a4056e54` (glVertex4f) and `4c693a35`
  (glPolygonMode/glPolygonOffset), is eight to seventeen files. The skill never
  mentions `gl_state_inspector.c` or `attrib_bits.c` at all, and the
  `4c693a35` commit message is explicit that those two were *"what makes the
  commands actually usable"*. The skill should be corrected to list all required
  edits — the two ratchets it relies on (the description-catalog generator and
  the tracked-cell sweep) only fire *after* someone has already guessed which
  files to open.
- **`round` is missing** from CLAUDE.md's and the scene-authoring skill's math
  function lists (finding 9).

---

## Recommended sequencing

Findings 1, 2 and the doc corrections are mechanical, independently landable, and
carry no behavioural risk — do them first and in any order.

1. **Finding 1** — move `autonormal.c:730-980` → `geometry_query.c`. Pure move.
2. **Finding 2** — add `repl_cmd_emits_immediate_vertex()`, rewrite six sites.
3. **Doc corrections** — the four stale claims above. Cheapest, and two of them
   are currently sending readers in the wrong direction.
4. **Finding 6** — drop `attrib_bits`' `default:`. Do this *before* the
   `glLightfv` work in `repl-capability-gaps.md` §3, which is the next new state
   command and the case the ratchet gap is waiting to miss.
5. **Finding 5** — extract the shared post-restore invalidation.
6. **Finding 3** — the commit-chain drift test.
7. **Finding 4** — the host-effects split. Largest of the set and touches three
   bands; worth doing, but schedule it as its own change rather than folding it
   into something else.
8. **Findings 7, 8, 9** — when adjacent code is next open.
9. **Finding 10** — record only; no action recommended.

## Explicitly not recommended

- **Do not fold the per-command switches into `command_spec.c`.** The executor,
  the state fold, the attribute mapper and the C emitter each need genuinely
  different per-command *behaviour*, not per-command *data*. A table of function
  pointers would relocate the switches without reducing them and would cost the
  `-Werror=switch` exhaustiveness that three of the four currently get for free.
  The right fix for the extensibility cost is finding 6 (make the fourth switch
  exhaustive too) plus an accurate skill checklist — not a new abstraction.
- **Do not merge the two evaluators.** The parity contract plus fallback is the
  correct design for a perf-motivated second implementation.
- **Do not split `compile.c`** on size alone. At 3,781 lines it is large, but it
  is one coherent responsibility with a strong stated contract, and every entry
  point is a peer of the others. Splitting it would create a header dependency
  web in exchange for nothing.
- **Do not unify the `_kernel` / wrapper pairs.** Two real consumers, thin glue.
