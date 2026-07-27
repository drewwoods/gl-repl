## Function-Scoped Local Variables

## Status — IN PROGRESS (2026-07-27): Phase 1 landed

Phase 1 (representation + declaration compile) is implemented and tested;
Phases 2–5 are untouched. **Until Phase 2 lands, a local is a compile-time
construct only** — `flatten_var_assign`'s existing `var_idx >= 0` guards make a
local-target assignment a no-op, and a local read resolves to nothing, so a
converted scene would render wrong. Do not convert any example scene before
Phase 2.

What Phase 1 delivered, against the plan below:

- `REPL_VAR_IDX_LOCAL` on `src/repl/command.h`; `repl_scan_decl_float_prefix`
  now reports whether `static` was typed.
- `collect_visible_vars_in()` binds local decl names into the innermost
  enclosing `CMD_FUNC_DEF` frame and takes the optional `ReplVisibleVarKind
  *kinds_out`. `compile_name_is_active_func_param` is gone, folded into that
  kind-tagged collection as planned.
- `repl_compile_float_decl` decides storage before parsing (`static` →
  global; enclosing function → local), with `compile_float_decl_local` as the
  local arm, `validate_local_decl_names`, the locality-aware parser preflight
  (initializer + `@tune`/`@config`), the `"declared local u in blade"` banner,
  and the local→global conversion (rejected while referenced; a
  delete-and-reinsert when not).
- `repl_compile_var_assign` resolves the LHS lexically through the ordered
  binding list, emits `REPL_VAR_IDX_LOCAL` with no predef op for a LOCAL
  target, adds the "loop variables are constant" diagnostic, and gates the
  slot rebase on both sides being global.
- `compile_func_scope_peak()` implements the whole-function capacity rule
  (`params + locals + max nested-loop depth <= MAX_EXPR_VARS`). It is written
  to be reused by the Phase 3 parameter and loop-header edits.

**Two Phase 3 items were pulled forward**, because Phase 1 opens the hazard
they close and leaving them for later would have been a live bug in between:

- `compile_collect_undeclare_for_range` skips `REPL_PREDEF_OP_UNDECLARE` for
  local decl rows and uses the local reference scan. Ungated, deleting a local
  would have undeclared a *same-named global* by name.
- `reformat.c`'s `CMD_VAR_DECLARE` case honors the storage marker instead of
  always re-emitting `static float` at depth 0.

`repl_compile_split_decl` (also listed under Phase 3) preserves the marker
too — it re-parses the row, so it could not be left for later either.

The scope-aware local reference scan Phase 3 specifies
(`compile_line_uses_local_ident`, the mirror of
`compile_line_uses_global_ident`) exists and is already used by the delete and
overwrite guards. What Phase 3 still owns: the reverse binder guards on
`repl_compile_func_def_kernel` / `repl_compile_for_loop_kernel`, and routing
the two raw-replace overwrite routes (`input.c:660`, `input.c:1017`) through a
shared `compile_decl_replacement_is_allowed`.

## Status — rev 9 (plan as reviewed, 2026-07-27)

Rev 9 repairs four consequences exposed by rev 8's shadowing rule. Items marked
**[rev 9]**:

- **Calls use lexical, not dynamic, scope.** Copying the caller's bindings into
  a callee makes a caller local hide a global that the callee references. A
  call frame therefore contains only the callee's parameters and locals; call
  arguments are still evaluated in the caller before that fresh frame is made.
- **Assignment targets are resolved lexically during flatten.** `var_idx` is
  persistent compile metadata, so inserting a legal local over an existing
  global otherwise leaves older assignment rows writing the global. Flatten
  re-derives every scalar assignment's LHS from source and resolves the
  innermost binding with a parallel binding-kind array.
- **Unwritable binders have reverse edit guards.** The first matching binding
  decides assignment legality: LOCAL is writable, PARAM and LOOP are not. A
  parameter or loop-header edit is rejected if it would capture an existing
  assignment in its scope and turn it into a write to an unwritable binding.
- **Local→global conversion follows the same shadowing rule.** A same-name local
  in another function is legal and does not block conversion; an already
  existing same-name global is the actual duplicate and still rejects it.

## Status — rev 8 — shadowing rule change

Rev 8 replaces the blanket no-shadowing ban with C's actual rule, on the
maintainer's observation that the ban was adopted to simplify implementation and
had not done so. Items marked **[rev 8]**. (Rev 7, below, landed concurrently
from a fifth review; the two do not overlap except where noted.)

The deciding fact is that our locals **hoist to the function-body top, which is
the same scope as the parameter list**:

```
$ gcc -std=c99 -c -xc - <<< 'float f(float x){ float x; return x; }'
error: redefinition of 'x'
```

So local-vs-parameter is not shadowing at all — C calls it a redefinition. Every
*other* direction the plan was rejecting is ordinary, legal C shadowing.

| Direction | Rev 7 | Rev 8 | Why |
|---|---|---|---|
| local ↔ parameter | reject | **reject** | same scope — redefinition |
| local ↔ local, same body | reject | **reject** | same scope — duplicate |
| local ↔ loop iterator | reject | **allow** | iterator is a nested scope |
| local ↔ global | reject | **allow** | global is outer; innermost wins |
| parameter/iterator ↔ global | allow | allow | pre-existing, untouched |

Consequences:

- **Phase 3's *redefinition* matrix collapses from four rows to two.** The "add
  a global" shadowing row disappears. The loop row remains only for capacity
  and for rev 9's assignment-capture guard; it no longer rejects a loop/local
  name collision merely because the names match. This also supersedes rev 7's
  "the no-shadowing table has four directions" bookkeeping note.
- **The evaluator needs no change, but flatten does.** `eval_primary`
  (`eval.c:1227-1243`) already scans `ctx->vars` innermost-first before falling
  through to predefs. Rev 9 corrects the scope array that it receives: a callee
  must not inherit caller bindings, and assignment LHS resolution must carry
  binding kinds rather than trusting stale `var_idx` metadata.
- **One thing gets harder.** The local delete guard becomes scope-aware instead
  of a raw identifier scan — it must not count a reference that a nested
  `for(x, …)` shadows. That is the mirror of `compile_line_uses_global_ident`
  (`compile.c:327`), so the walk already exists.
- **Call frames become lexical.** `flatten_bind_func_locals` appends locals
  after the callee's parameters, and `flatten_call` does not copy the caller's
  bindings. Otherwise a caller local would dynamically shadow a global read by
  the callee, unlike exported C. [rev 9]
- **Export parity depends on that lexical frame rule** — a local shadowing a
  file-scope static then behaves identically in C and in the REPL, inner wins in
  both, while a callee that has no such local still reads the global.
- The `static`-inside-a-function justification is updated once more: a local
  *can* now shadow such a global, which is precisely C's behavior and therefore
  still not a divergence.

This retires the plan's stated top maintenance liability; see "Known liabilities
and revisit triggers".

## Status — rev 7 after fifth review

Three corrections, all incorporated below and marked **[rev 7]** where the
implementation contract changed:

- **Local→global conversion has explicit cross-storage accounting.** The old
  local row is excluded from its own collision scan but contributes no old-global
  slot credit and emits no `UNDECLARE`; every converted name needs a fresh
  predef slot. A full predef table therefore rejects the conversion before any
  source mutation.
- **Global→local conversion is removed from V1.** A global declaration lives at
  document top, so removing `static` while editing that row still selects the
  global path. Supporting the reverse would require a new move-into-function
  operation plus predef teardown and slot rebasing.
- **The guard inventories and shadowing prose agree again.** The replacement
  inventory has five routes, and the Non-goals wording is explicitly limited to
  local declarations. (The shadowing table's four directions were then reduced
  to two by rev 8 — see below.)

## Status — rev 6 after fourth review

Three findings; one narrows a rule rather than adding code. Items marked
**[rev 6]**.

- **Local→global storage conversion is rejected while the name is referenced.**
  Converting `float x;` to `static float x;` invalidates every compiled
  assignment to that name — existing rows carry `var_idx ==
  REPL_VAR_IDX_LOCAL`, which after conversion must be a real predef slot.
  Rev 4 treated this as a relocation problem; it is a correctness problem.
  Refusing the edit matches the rule already next door (`"variable '%s' is in
  use, cannot overwrite"`) and needs no transaction machinery. This also adds
  declaration→declaration replacement to the overwrite-route list: that path
  exempts an unchanged name as "kept" (`compile.c:865-871`), which is wrong when
  the storage changed under the same name.
- **The no-shadowing rule is narrower than rev 5 stated.** *(Narrowed further
  by rev 8, which keeps only the same-scope redefinition cases.)* It constrains
  *local declarations only*. A parameter or loop iterator
  shadowing a **global** is pre-existing, deliberate behavior —
  `compile.c:323-326` documents it, and `compile_line_uses_global_ident` returns
  0 precisely so a shadowed global reads as unreferenced. Adding the "missing"
  blanket name-collision guards would be a behavior change outside this
  feature's scope and would likely break existing scenes. Added a regression
  guard instead. Rev 9 adds only the narrower reverse guard required when a
  binder edit would capture a write to an unwritable parameter or iterator.
  Knock-on: rev 4's justification for `static`-inside-a-function claimed the
  scope difference was "unobservable because shadowing is forbidden" — that was
  overstated and is corrected; the decision itself stands.
- **Phase 4 carried contradictory superseded instructions** (bare
  `float a, b;` next to the zero-initializer requirement; "import needs no
  change" next to the import-lowering requirement). Rewritten as one
  authoritative sequence.

## Status — rev 5 after third review

Three blocking findings, all confirmed. Items marked **[rev 5]**.

- **Export emits explicit zero-initializers; the accepted divergence is
  withdrawn.** Rev 3 documented "REPL reads 0, exported C is undefined" as a
  deliberate C-like choice. That violates a stated contract —
  `docs/ARCHITECTURE.md:2156`: *"Behavior parity is required, not just syntactic
  round-trip."* Undefined behavior is also the one case the REPL cannot
  reproduce, so "match C" was never actually available. Export writes
  `float a = 0.0f;`; import lowers that literal-zero form back to `float a;`,
  keeping the round-trip idempotent.
- **Capacity is a whole-function property.** `params + locals <= MAX_EXPR_VARS`
  is wrong in both directions: `flatten_for_loop` prepends an iterator per
  nesting level and drops the last binding at the cap, so what must fit is
  `params + locals + max nested-loop depth`. Rev 4 guarded only the "add a loop"
  edit; adding a param or a local when a loop already exists overflows the same
  way.
- **There are four declaration-overwrite routes, not two.** Beyond the range
  mutation and `repl_compile_var_assign` cascades, `editor_place_parsed_command`
  (`input.c:660`) and the Enter path (`input.c:1017`) both call
  `repl_command_store_replace_one` with no reference check at all. Survivable
  for a global (the predef slot outlives the row); fatal for a local, whose
  binding *is* the prologue row. One shared guard, four callers. Related: the
  var-assign slot rebase must run only when both sides are global predef slots,
  or it rejects the legitimate local-overwrite case.

Also corrected: the local marker is `var_idx == REPL_VAR_IDX_LOCAL` throughout
(Phase 1 and four later bullets still said `payload.decl.is_local`), and the
motivation table now reads 33 locals / 106 globals to match the conversion
audit's reclassification of `detail`.

## Status — rev 4

Rev 4 settles two UX questions raised by the maintainer, and both simplify the
design. Items marked **[rev 4]**.

- **Declarations hoist from any depth inside a function**, rather than being
  rejected in `for` / `if` bodies. The REPL's existing contract is "declare
  wherever, the editor moves it to the top" — applying that at the top level but
  not one level down was an arbitrary hole. (This block claimed the
  no-shadowing rule would reject `for(i,…) { float i; }`; **rev 8 makes that
  case legal** — the local hoists to the body top and the iterator shadows it,
  as in C. The hoisting decision itself is unaffected.)
- **`static` selects storage and beats cursor position.** `static float x;` is
  always a global, from anywhere — which is also the escape hatch for declaring
  a global without first moving the cursor out of a function. Plain `float x;`
  is a local inside a function, a global at top level. This *shrinks* the parser
  preflight rev 3 specified, since `static` stops being a rejection.

Consequence worth flagging to reviewers: rev 3 accepted a finding that the
locality test must use `compile_nearest_open_block_head_at` and explicitly *not*
an enclosing-function search. That was right for the old reject-in-`for`/`if`
policy and is now inverted — resolving through a nested block to the owning
function is the required behavior. See Phase 1.

## Status — rev 3 after second review

Rev 3 incorporates a second review (four blocking findings plus four
corrections, all confirmed). One changed the representation, the rest tightened
under-specified guards; items are marked **[rev 3]** in place. Headlines:

- **The local marker lives on `var_idx`, not in the payload union.** Measured,
  the decl arm dominates the union exactly, so a new `int` would have grown
  every command in the source array, flat array, undo rings and scene snapshots.
- **The declaration parser needs a locality-aware preflight.** `float x = param;`
  currently dies on unknown-identifier validation before any local diagnostic can
  run, and the parser discards whether the user typed `static`.
- **The local reference scan cannot reuse `compile_line_uses_global_ident`**,
  which by design suppresses exactly the references the guard needs to see — and
  there is a *second* decl-overwrite cascade in `repl_compile_var_assign`.
- **Reverse-shadowing must also cover the loop-iterator capacity cap**, which
  silently drops an outer binding rather than erroring.
- ~~**Exported locals are uninitialized C automatics** while the REPL
  zero-fills. Accepted as a documented divergence.~~ **Superseded by rev 5** —
  this violated the export behavior-parity contract; export now emits explicit
  zero-initializers.

Rev 2 incorporated a first review that found seven material issues, all
confirmed against the tree. Two changed the design rather than the wording:

- **Locals take no initializer in V1.** `format_decl_text` (`compile.c:753`)
  emits `" = %g"`, destroying the initializer *expression* at commit time, and
  `parse_float_name_list` (`compile.c:654`) validates initializer identifiers
  against predefs only and evaluates immediately. So `float tmp = param;` cannot
  work through the existing path no matter how locality is detected, and
  `float a = PI*2;` would reach flatten as the constant `6.28319`. All
  motivating cases are already declare-then-assign, because a global decl's
  initializer cannot reference parameters today either — so dropping
  initializers costs the corpus nothing and removes an entire phase of parse,
  format, and export work. See "Non-goals".
- **Local dataflow is structural, not value-only.** `rebake_one_cmd`
  (`flatten.c:1338-1353`) evaluates a `CMD_VAR_ASSIGN` RHS against the *frozen*
  `FlatCmdLocalVars` snapshot and writes back only through
  `cmd->var_idx >= 0 → g_predef_vars_mut`. Nothing propagates a local's new
  value into later commands' snapshots, so a value-only rebake would read stale
  locals. Every dep feeding a local assignment must be reported structural.

The other five findings are addressed in place and marked **[rev 2]**.

This plan replaces an earlier note of the same name whose
V1 was *making existing loop variables and function parameters assignable*. That
proposal does not address the problem measured below — you cannot turn
`blade()`'s eight temporaries into parameters — and its coordinates had rotted
(`repl_editor.c` no longer exists; `repl_eval_find_predef_var_idx()` gained a
context parameter and became `_in()`; line numbers were off by ~200). Writable
params/loop vars are explicitly **out of scope** here; see "Decisions taken".

Nothing has landed. `repl_compile_float_decl` (`src/repl/compile.c:826`) still
hoists every declaration to the top of the document, and
`repl_compile_var_assign` (`src/repl/compile.c:1038`) still rejects any target
that is not a predef slot or a scratch cell.

### Summary

`float x;` inside a function body declares a variable scoped to that function:
invocation-local (so recursion is safe), consuming no predef slot and no
variable-panel row. Locals live where loop variables and function parameters
already live — as `ExprVar` entries in the stack-allocated scope array that
`flatten_call` builds per invocation. No new storage, no new table, no new
`CmdType`.

Estimated effort: **medium-large**. The evaluator needs no change at all
(`eval_primary` already checks caller-supplied locals before the predef table),
and `flatten_range` grows by zero lines. The work is spread across the compile
path, the edit guards, and export/import rather than concentrated anywhere.

### Motivation

A survey of all 35 example scenes classified every declared float by where it is
written and read:

| Category | Count |
|---|---|
| Top-level scratch / tunable (genuine globals) | 106 |
| Const tunables (`@tune`, never reassigned) | 63 |
| **Written and read inside exactly one func — wants a local** | **33** |
| Written in a func, read by the caller — wants a return value | 9 |
| Mixed | 9 |

33 of 220 declared names exist only because the language has no local storage.
(The raw classifier said 34; `detail` was reclassified to a global by the
conversion audit below — it is a scrubbable knob, not a temporary. [rev 5])
The cost is concrete, not cosmetic:

- **Budget pressure.** `examples/scenes/orrery-labels-track-3d-orbits.glr`
  declares 29 floats against a 31-slot user budget (`MAX_PREDEF_VARS` = 32, one
  reserved for `t`). Sixteen are `planetKepler()` intermediates. The scene is two
  variables from being unauthorable.
- **Prose standing in for a calling convention.** That scene carries the comment
  *"px/py/pz stay set after the call so the caller can hang a label off the body
  (th/u are internal scratch and get clobbered mid-function, so don't rely on
  them)"*. `whale-particle-system-lit-model.glr` needs a comment explaining
  `drawWhale`'s params are "named apart so they don't shadow".
- **Unusable under recursion.** `sierpinski-sponge-3d-recursion.glr` and
  `recursive-triangle-tree-func-recursion.glr` declare zero floats — a global
  scratch var is clobbered by the recursive call before the parent is done with
  it, so every intermediate must be inlined or promoted to a parameter.

The 9 "wants a return value" cases are **not** addressed here. Three of the four
functions involved return coordinate triples (`px/py/pz`, `x/y`), so a scalar
`return` would not fix them; see `float-returning-repl-functions.md`.

### Decisions taken

| Question | Decision |
|---|---|
| Where may a local be declared | **Anywhere lexically inside a function body**, at any nesting depth; the editor hoists it to the top of that body, exactly as top-level decls hoist to the top of the document today. [rev 4] |
| Global vs. local | **Keyword-driven.** `static float x;` is always a global, from any cursor position. Plain `float x;` is a local when inside a function, a global at top level (unchanged). [rev 4] |
| Shadowing | **C's rule, not a blanket ban.** [rev 8] A local may not collide with a parameter or another local of the same body — in C those share one scope and it is a *redefinition*, not shadowing (`gcc`: "error: redefinition of 'x'"). Shadowing a global or a loop iterator is legal and behaves as C does, innermost wins. |
| Writable params / loop vars | **Out of scope.** Keep the `compile.c:1146` guard ("function parameters are constant"). `float tmp; tmp = param;` covers the need. |
| Example conversion | **3 scenes as proof** in this change: orrery, swaying-grass, whale. |

### V1 behavior

- `float a, b;` inside a function body declares function-scoped locals.
  **No initializer** — see Non-goals and the rev-2 status note.
- **The `static` keyword selects storage, and it wins over cursor position.**
  [rev 4] `static float x;` is a global wherever it is typed, including from
  inside a function body — which is also the escape hatch for declaring a global
  without moving the cursor out of the function first. Plain `float x;` is a
  local when there is an enclosing function and a global otherwise, so existing
  top-level behavior is unchanged.

  This is C's storage-duration distinction, and it is honest here: a `static`
  in C persists across calls exactly as a predef slot does. The one place C and
  the REPL differ — a C function-static is scoped to the function, ours is
  document-wide — is not observable, because every construct that can shadow it
  does so with C's own semantics: a local, a parameter or a loop iterator hides
  it for exactly the region C would, innermost wins, and it stays reachable
  everywhere else. [rev 8]
- **Declarations may appear at any depth inside the body** and are hoisted to
  the top of that body. [rev 4] `float u;` typed inside a `for` nested in a
  function relocates to the function's declaration prologue, the same way a
  top-level `float x;` relocates to the top of the document today. There is no
  depth-based rejection: it would break the very C89-flavor analogy that
  motivates hoisting. Every reference therefore follows its declaration, and
  flatten's binding stays a prefix scan.
- **Name collisions follow C's scope rules.** [rev 8] Because locals hoist to
  the function-body top, they occupy the same scope as parameters — so a local
  colliding with a parameter, or with another local of the same body, is a
  *redefinition* and is rejected. A loop iterator is a nested scope, and globals
  are outer, so `for(i, 0, n) { float i; }` is legal: the local `i` is visible
  through the function, the iterator shadows it inside the loop. That is exactly
  what C does. `eval_primary` already supplies innermost-first lookup; rev 9's
  flatten changes ensure the binding array contains lexical scopes only and
  carries enough kind information to keep the iterator unwritable.
- Each local binds to `0.0f` on entry to the call, with dep mask 0.
- Assignment `a = expr;` targets the innermost lexical binding named `a`.
  LOCAL is writable; PARAM and LOOP produce the existing constant-binding
  diagnostic; with no scoped binding, the assignment targets the global predef
  slot. Every dep of a local RHS is reported **structural**. [rev 2, rev 9]
- Locals are **invocation-local**: `flatten_call` creates a fresh
  `lvars[MAX_EXPR_VARS]` containing the callee's parameters and own locals only
  and never copies it back. Caller bindings are not in the callee's lexical
  scope; call arguments have already captured every caller value the callee is
  allowed to receive. Recursion is therefore isolated naturally. [rev 9]
- Locals never enter `g_predef_vars`. The variable panel, `@tune` knobs, replay
  baseline, export prologue and slot-shift cascade are all keyed on predef slots
  and need no changes.

### Non-goals

- **No initializers on locals.** `float x = 1;` inside a function body is
  rejected with "local declarations cannot have an initializer — assign on the
  next line". Rationale in the rev-2 status note; supporting them means
  preserving initializer spans through `format_decl_text`, validating them
  sequentially against params plus earlier names, re-evaluating per call in
  flatten, and routing them through `repl_eval_expr_to_c` on export — a V2 in
  its own right. [rev 2]
- No **block**-scoped locals. A declaration inside a `for` or `if` is hoisted to
  the enclosing *function's* prologue and lives for the whole call — it is not
  scoped to the block it was typed in, and it is not re-initialized per
  iteration. [rev 4] This matches C89 and matches how top-level decls already
  behave; genuine block scope would be a V2.
- No locals outside a function. `float x;` at top level is a global, as today.
- No writable function parameters or loop variables.
- No *redefinition*: a local may not collide with a parameter or another local
  of the same function body (same scope in C). Shadowing an outer binding — a
  global or an enclosing loop iterator — is allowed. [rev 8]
- No local arrays; `A`/`B`/`C` stay global scratch.
- No `// @tune` / `// @config` on a local — those need a panel slot, so they are
  rejected.
- No implicit local creation from an unknown assignment target. `tmp = 5;`
  without a declaration still errors "undeclared variable". [rev 4] With
  redefinition rejected and shadowing allowed there is no way to distinguish "I
  meant a new local" from "I mistyped the global's name", so implicit creation
  would turn every typo into a silent new variable. Only the *declaration
  statement* may appear anywhere; the declaration itself stays explicit.

### Representation

- **Mark a local decl with `var_idx = REPL_VAR_IDX_LOCAL` on the decl row.**
  [rev 3] Not a new union field: measured, `sizeof(GLCmd) = 208`,
  `sizeof(payload) = 132`, `sizeof(payload.decl) = 132` — the decl arm dominates
  the union exactly (`8 × 16` names + `int count`, no slack), so an added `int`
  grows *every* command by 4 bytes across the source array, the flat array, the
  32-slot undo rings and every scene snapshot. `var_idx` is already documented as
  "Zero / unused for every other CmdType" (`command.h:120-123`), so a decl row
  can carry the flag at zero cost — and it reads consistently with
  `CMD_VAR_ASSIGN` using the same sentinel for a local target.
- **Canonical text distinguishes them, matching C:** `  static float a, b = 2;`
  for a global (unchanged), `<indent>float a, b;` for a local (no `static`, no
  initializer, indent from the body's block depth). Because the keyword is also
  what the *author* types to choose storage [rev 4], canonical text and intent
  agree by construction — and the export/import round-trip falls out for free,
  since the same keyword carries the meaning in generated C.
- A newly compiled `CMD_VAR_ASSIGN` targeting a local carries `var_idx =
  REPL_VAR_IDX_LOCAL (-1)` and emits no `REPL_PREDEF_OP_SET_VALUE`. The source
  command's `var_idx` is a commit-time storage hint, not the final lexical
  authority: a later legal binder edit can change what the row names. Flatten
  therefore always extracts the target with `repl_extract_assignment_parts()`
  (`src/repl/text_helpers.h:96`), resolves it against the current binding-kind
  array, and normalizes the emitted flat command's `var_idx` to the resolved
  destination. [rev 9]

### Why this shape

It reuses the per-block `ExprVar` scope arrays flatten already builds, the
`ReplExprDepMask` array that already rides alongside them, and the "names are
re-derived from source text" convention that loop variables and parameters
already follow. A parallel `ReplVisibleVarKind` array makes the same ordered
scope usable for assignment LHS resolution without making parameters or loop
iterators writable. Name lookup needs no evaluator change — `eval_primary`
(`src/repl/eval.c:1227-1243`) already checks `ctx->vars` before the predef table.

`flatten_range` (`src/repl/flatten.c:1076`) gains only the parallel-kind
parameter and forwards it to helpers; it gains no new control-flow arm. Keep it
within the 91-line `scripts/baselines/tier-c-function-size.txt` ratchet (and
leave `parse_command` at its separate 335-line ratchet). [rev 9]

### Implementation

#### Phase 1 — Representation and declaration compile

- `src/repl/command.h` — define `REPL_VAR_IDX_LOCAL` (-1). A decl row is local
  iff `var_idx == REPL_VAR_IDX_LOCAL`; **no new payload field** (see
  Representation for the measurement). [rev 5]
- `src/repl/compile.c:826` `repl_compile_float_decl` — **decide storage before
  parsing**, because the two paths validate initializers differently. [rev 2]
  The decision is a two-step, in this order [rev 4]:

  1. **Lexical: did the user type `static`?** `repl_scan_decl_float_prefix`
     (`text_helpers.c:247`) already recognizes the optional prefix but discards
     the fact; have it report it. `static` present → global path, existing
     behavior, works from any cursor position.
  2. **Otherwise, is there an enclosing function?** Walk the `ScopeFrame` stack
     to the innermost open `CMD_FUNC_DEF` — the walk already in
     `compile_name_is_active_func_param` (`compile.c:265`), which Phase 1 is
     folding into kind-tagged `collect_visible_vars_in()` anyway, so this
     converges rather than adding a fourth scope walker. Found → local path;
     not found → global path.

  Rev 2 said to use `compile_nearest_open_block_head_at` (`compile.c:215`) and
  explicitly *not* an enclosing-function search. That was correct for the old
  "reject in `for`/`if` bodies" policy and is now wrong: with hoist-from-any-depth,
  resolving through a nested `for`/`if` to the owning function is exactly the
  required behavior. [rev 4] The nearest-open-block helper is still useful — it
  keeps a stack of all open block indices, so the innermost `CMD_FUNC_DEF` can
  be read off it directly, and the same index feeds
  `compile_scope_find_block_end` for the body bound the Phase 3 reference scan
  needs.

  On the local path: `decl_pos` = first non-decl row after the func header
  (mirroring the global rule, which skips only `CMD_VAR_DECLARE` rows);
  `build_decl_predef_ops` emits nothing; canonical text drops `static`; and
  `format_decl_text` (`compile.c:739`) grows a local/global prefix switch plus a
  depth-aware indent. It needs no initializer handling — locals have none.
- **The parser needs a locality-aware preflight, because it currently rejects
  the local cases before any local diagnostic can run.** [rev 3]
  `parse_float_name_list` (`compile.c:573`) validates initializer identifiers
  against predefs only (`compile.c:654`), so `float x = param;` dies with
  "unknown identifier 'param'" before `validate_local_decl_names()` can emit the
  promised initializer message. Add a lexical preflight (or a `FloatDeclParse`
  mode flag) that, on the local path, rejects **before** initializer validation
  and evaluation:
  - any top-level `=` → "local declarations cannot have an initializer — assign
    on the next line"
  - a `// @tune` / `// @config` tag → "@tune/@config require a global
    declaration — use `static float`" (these need a variable-panel slot, which
    locals do not have; this is the enforcement point rev 2 promised but never
    located)

  `static` is no longer a rejection here — it selects the global path in step 1
  above, so the preflight got *smaller* under the rev-4 rule. [rev 4]

  Test with `float x = param;`, not only `float x = 1;` — they fail in different
  places today.
- **Make the storage choice visible in the status line.** [rev 4] Storage now
  depends on cursor position for the plain-`float` case, which is invisible
  state. `build_decl_commit_message` (`compile.c:815`) already produces
  `"declared a, b, c"`; have the local path say `"declared local u in blade"`.
  Combined with the canonical text (`float` vs `static float`) that gives two
  independent confirmations of what the author just got.
- **Local→global storage conversion: reject when the name is referenced.**
  [rev 6, rev 7] Retyping a local as `static float x;` is not just a relocation
  — it invalidates every *compiled* assignment to that name. Existing `x = …`
  rows carry `var_idx == REPL_VAR_IDX_LOCAL`, and after conversion to a global
  they must carry the new predef slot instead; leave them stale and the
  assignments write to a local that no longer exists.

  Two resolutions were possible — atomically reclassify and rebuild every
  assignment, or refuse the edit. **Refuse it**: it matches the shape of the
  rule already next door (`"variable '%s' is in use, cannot overwrite"`,
  `compile.c:875`), it needs no new transaction machinery, and the author's
  workaround is one extra step (delete the references, convert, retype them).
  Converting an *unreferenced* declaration stays legal.

  This also means **declaration→declaration replacement joins the overwrite-route
  list** in Phase 3. That path currently exempts an unchanged name from the
  feasibility check as "kept" (`compile.c:865-871`), which is right when only
  the name set changes and wrong when the *storage* changes under the same name.
  A kept-but-converted name must run the reference check like a dropped one.

  When conversion is allowed (unreferenced), the row still has to *move* —
  document top vs. function-body top — so it is a delete-here-and-reinsert, not
  a replace-in-place. [rev 4]

  This is the only storage conversion reachable through normal editing. [rev 7]
  Storage is chosen from the declaration row's cursor scope: a global row
  already lives at document top, so removing `static` there still selects the
  global path. Global→local conversion would require an explicit
  move-into-function operation plus a predef `UNDECLARE`/slot-rebase
  transaction; neither is part of V1.

  The allowed local→global case needs **cross-storage validation**, not the
  ordinary same-kind overwrite accounting. [rev 7]

  - Do not run a document-wide "name matches an existing local" rejection.
    The declaration row being converted stops being local, and same-name locals
    in other functions legally shadow the new global. Do reject when the name
    already has a global/predef declaration: that is a duplicate in the same
    storage namespace, not shadowing. [rev 9]
  - Treat the old row as a local, never as an old global. It contributes zero to
    the predef-slot credit in `validate_decl_names`, emits no `UNDECLARE`, and
    every new global name emits a `DECLARE`. In particular, the capacity check is
    `predef.count + parsed.count <= MAX_PREDEF_VARS`, with no subtraction for
    the removed local row.
  - Keep the reference check storage-aware: an unchanged textual name is still
    "removed" from local storage and therefore must be unreferenced before the
    conversion proceeds.
- New `validate_local_decl_names()` beside `validate_decl_names`
  (`compile.c:693`). Rejects **same-scope redefinitions only** [rev 8]:
  duplicate-in-decl; a name matching a **parameter of the enclosing function**;
  a name matching **another local of the same body**; reserved idents via
  `repl_eval_is_reserved_ident()`; bad ident shape or >15 chars; any
  initializer; a `@tune`/`@config` tag; and a capacity overflow.

  It deliberately does **not** reject a name that matches an enclosing loop
  iterator or a global — those are outer scopes, and shadowing them is legal C
  that `eval_primary` already resolves correctly. Note this means
  `collect_visible_vars_in()`'s flat result is not the right input on its own:
  the validator needs the *kind* tag (PARAM / LOCAL vs LOOP) that Phase 1 is
  adding anyway, plus the enclosing-function bound, so it can distinguish
  same-scope from outer-scope matches.
- `src/repl/visible_vars.{c,h}` — teach `collect_visible_vars_in()` to add local
  decl names to the current `CMD_FUNC_DEF` `ScopeFrame` (value `0.0f`, same as
  params — it is a name scope, not a value scope), and add an optional
  `ReplVisibleVarKind *kinds_out` parameter (LOOP / PARAM / LOCAL) so callers can
  tell them apart. Make that kind enum the shared compile/flatten binding tag,
  not a compile-only diagnostic detail. One place; all 14 collection call sites
  benefit, and flatten uses the same ordering contract. [rev 9]
- `src/repl/compile.c:1038` `repl_compile_var_assign` — before the "undeclared
  variable" error, resolve the first matching entry in the ordered visible-var
  list and inspect its kind; do **not** skip a matching PARAM/LOOP in search of
  an outer LOCAL. LOCAL emits `var_idx = REPL_VAR_IDX_LOCAL` with no predef op;
  PARAM keeps the existing constant-parameter diagnostic; LOOP gets the
  parallel "loop variables are constant" diagnostic. Only when no scoped
  binding matches may the resolver fall through to a predef slot. Factor this
  as the shared lexical assignment-target resolver used by the reverse binder
  guards in Phase 3. [rev 9]

#### Phase 2 — Flatten

- `src/repl/flatten.c:358` `flatten_call` — after binding params into `lvars`,
  call a new `flatten_bind_func_locals(ctx, body_start, body_end, lvars, ldeps,
  `lkinds, &lnv)` that walks the body's **declaration prologue** and appends
  `{name, 0.0f}` with dep mask 0 and kind LOCAL for each name. Parameters are
  tagged PARAM. No expression evaluation is involved, so no new
  `ReplFlattenExpr` role and no shared decl-line scanner is needed — names come
  straight off `payload.decl.names[]`.

  **Delete the caller outer-scope copy.** [rev 9] The current
  `flatten.c:480-483` copy implements dynamic scope: if caller `A` has local
  `x`, callee `B` has no `x`, and `B` reads global `x`, the copied caller local
  wins because `eval_primary` searches `lvars` before predefs. Exported C reads
  the global. The callee frame is parameters followed by its own locals only;
  call arguments were already evaluated against the caller before the frame was
  built. Params and locals cannot collide because Phase 3 rejects that
  same-scope redefinition.
- **Define the prologue to tolerate `CMD_COMMENT` and `CMD_EMPTY`**, stopping at
  the first other command type. [rev 2] A strictly contiguous run breaks the
  moment an author comments out the first local: the body becomes
  `CMD_COMMENT, CMD_VAR_DECLARE, …` and every later local silently stops
  binding. The same tolerance is what lets a `// grouping comment` sit between
  declaration rows.
- `src/repl/flatten.c:868` `flatten_var_assign` — **always** extract and resolve
  the LHS against ordered `(vars, var_kinds)[0..nv)`, even when the persisted
  source command has a nonnegative `var_idx`. [rev 9] This is what makes adding
  a local over an existing global correct without rewriting every older
  assignment command. The first name match decides:

  - LOCAL: write `vars[k].value = value` / `var_deps[k] = rhs_deps`, and set the
    emitted flat command's `var_idx = REPL_VAR_IDX_LOCAL`;
  - PARAM or LOOP: fail flatten defensively — commit and reverse edit guards
    must prevent this source state, but flatten must never mutate the binding;
  - no scoped match: write the global slot from `src_cmd->var_idx` as today.

  This requires dropping `const` from `var_deps` and forwarding the parallel
  `var_kinds` array along the `flatten_range` → `flatten_var_assign` chain.
- **In that same branch, report the RHS deps via
  `repl_flatten_expr_note_structural()`, not `note_value()`.** [rev 2] This is
  the fix for the rebake hazard in the status note: `rebake_one_cmd` cannot
  thread a local's new value into later commands' frozen snapshots, so any
  predef change that can reach a local must force a full reflatten instead of a
  value-only rebake. Because assignment is now the *only* way a value enters a
  local (no initializers), marking the assignment RHS structural covers the
  whole dataflow. Test the **resolved target kind**, not the persisted
  `src_cmd->var_idx`, so a pre-existing global assignment retargeted by a newly
  inserted local also takes the structural path. [rev 9] Cost: editing a global
  that feeds a local reflattens rather than rebakes — correct, and bounded.
- **`src/repl/flatten.c:248` `flatten_for_loop` — copy back.** This is the one
  non-obvious correctness point. `lvars` is rebuilt per iteration, so without a
  copy-back of the outer entries after each `flatten_range` returns,
  `float acc; acc = 0; for(i,0,n) { acc = acc + i; }` silently resets every
  iteration. Tag the prepended binding LOOP, copy the outer kind entries beside
  their values/deps, and copy `lvars[1..]` back into `vars[]` (skipping the
  iterator at index 0). `flatten_if_block` needs nothing — it shares the
  caller's arrays. `flatten_call` deliberately does not copy back.
- `flatten_range` (`flatten.c:1076`): unchanged apart from the `const` removal
  and the parallel `var_kinds` parameter/forwarding required above.
  Its existing `if (src_cmd->type == CMD_VAR_DECLARE) { i++; continue; }` at
  line 1138 is already correct.
- `src/repl/executor.c:887` needs **no change** — its `var_idx >= 0` guard makes
  a local-target assign a natural no-op.

#### Phase 3 — Edit guards and formatting

**Redefinition, capacity and unwritable-target capture must be enforced in both
directions.** [rev 2, narrowed rev 8, corrected rev 9] Validating only at the
point the local is declared leaves later edits free to create either a
same-scope collision or a PARAM/LOOP binding over an existing assignment. Four
binders have their own compile paths; only the first two participate in the
same-scope redefinition rule, while parameter and loop edits also need the
assignment-capture check:

| Edit | Path | Must reject |
|---|---|---|
| Add/rename a function parameter | **`repl_compile_func_def_kernel`** | a name matching a local of that body (same scope — redefinition); a capacity overflow; an assignment in that function whose target the edited parameter would capture |
| Add/rename a local | local path | a name matching a parameter, or another local of the same body; a capacity overflow (below) |
| Add/rename a loop iterator inside a func body | `repl_compile_for_loop_kernel` (`compile.c:2453`) | a capacity overflow; an assignment in that loop body whose target the edited iterator would capture |
| Add a global | `repl_compile_float_decl` global path | *nothing new* |

**Why only two redefinition rows.** [rev 8] Locals hoist to the
function-body top, which is the *same scope* as the parameter list — C treats a
collision there as a redefinition, not shadowing:

```
$ gcc -std=c99 -c -xc - <<< 'float f(float x){ float x; return x; }'
error: redefinition of 'x'
```

Every other direction is legal C shadowing, so the REPL allows the name
collision and resolves innermost-first, which is what `eval_primary`
(`eval.c:1227-1243`) already does. Assignment writability remains a separate
language rule, handled below.
Rev 6 had four bidirectional guards here; three of them banned ordinary
shadowing for no reason C would recognise, and they were the plan's stated top
maintenance liability. Dropping them also preserves the pre-existing behavior
that a parameter or loop iterator may shadow a global (`compile.c:323-326`),
rather than making this feature quietly change it.

**Shadowing does not make parameters or iterators writable.** [rev 9] Reuse the
Phase 1 lexical assignment-target resolver to validate the *post-edit* scope
before accepting a function-header or loop-header change. Walk only the edited
binder's lexical body (excluding nested function bodies), respecting nested
same-name binders exactly as normal resolution does. Reject if any existing
`CMD_VAR_ASSIGN` would resolve first to the proposed PARAM or LOOP binding.
This covers both adding and renaming the binder and prevents two stale-metadata
failures:

- renaming `for(i, ...)` to `for(x, ...)` over `x = x + 1;` must not turn the
  row into an assignment to the iterator; and
- adding/renaming a parameter `x` over a body assignment that previously wrote
  global `x` must not turn that row into an assignment to the parameter.

Adding a LOCAL over an existing global assignment is different: the new target
is writable and legal. Accept the edit; Phase 2's lexical LHS resolution makes
the old row write the new local on subsequent flatten passes, without mutating
the global. A loop or global may still shadow a local when no prohibited
assignment capture occurs. This is a target-legality guard, not a return to the
blanket name-collision ban.

**Capacity is a whole-function property, not a per-edit one.** [rev 5] A check
of the form `params + locals <= MAX_EXPR_VARS` is insufficient in both
directions, because `flatten_for_loop` prepends its iterator to a *fresh* scope
array and copies outer bindings under `lnv < MAX_EXPR_VARS`
(`flatten.c:347-351`), silently dropping the last one at the cap. The quantity
that must fit is the **peak** scope size anywhere in the body:

```
params + locals + max nested-loop depth  <=  MAX_EXPR_VARS
```

`if` blocks contribute nothing (they share the caller's array) and calls open a
fresh frame, so loop nesting is the only multiplier. Every one of the three
edits above can push this over: rev 4 caught only "add a loop when locals
already fill the scope", but adding a parameter or a local when a loop already
exists later in the body overflows identically. Compute the max nested-loop
depth over the function body once and validate all three edits against the same
expression.

Two corrections to that table from rev 2: [rev 3]

- **Validation must live in `repl_compile_func_def_kernel`, not the
  `repl_compile_func_def` wrapper** — the editor calls the kernel directly
  (`src/editor/commit.c:568`), so checks placed only on the wrapper are bypassed
  on the interactive path.
- **The loop-iterator capacity guard is not optional.** `flatten_for_loop`
  prepends the iterator and then copies outer bindings under
  `for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)` (`flatten.c:347-351`) —
  at the cap it **silently drops** an outer variable, which with locals in the
  array means a live local vanishes mid-body and reads as 0. Reject at compile
  time instead.

- `repl_compile_split_decl` (`compile.c:921`) must preserve `var_idx ==
  REPL_VAR_IDX_LOCAL` on every emitted row and emit
  the local form. [rev 2] It currently re-parses through
  `parse_float_name_list` and rebuilds every row through the global
  `static float` formatter, so Ctrl+Shift+S on a local would silently convert it
  to a global. Needs an explicit test.
- **The local reference scan must not reuse `compile_line_uses_global_ident`.**
  [rev 3] That helper deliberately returns 0 whenever the name is visible as a
  local (`compile.c:342`) — its whole job is "does this line reference the
  *global*". Once `collect_visible_vars_in()` reports locals, it would suppress
  *every* reference to a local inside its own body, so the delete guard would
  conclude an in-use local is unreferenced and let it be removed. An "own
  declaration" exception does not fix this; the suppression applies to the
  reference lines, not the declaration. The local variant is
  `compile_line_uses_global_ident`'s **mirror image**: walk the same scope
  frames and count the line only when the innermost binding of that name *is
  this local* — a nested `for(x, …)` shadowing it means the line does not
  reference it. [rev 8] Rev 6 specified a plain raw identifier scan, which was
  only sound while shadowing was banned; with shadowing allowed it would
  over-match a shadowed reference and block a legal delete. The walk itself is
  not new machinery — `compile_line_uses_global_ident` (`compile.c:327`) already
  does exactly this in the opposite direction. Bound it to the function body,
  excluding the range being
  changed.
- **Five overwrite paths, and only one shared guard should exist.** [rev 5, rev 6]
  Declaration replacement is reachable five ways, and each currently decides for
  itself whether to check references:

  | Route | Site | Today |
  |---|---|---|
  | Range delete / comment-toggle | `compile_collect_undeclare_for_range` (`compile.c:1413`) | checks |
  | Retype a decl row as an assignment | `repl_compile_var_assign` (`compile.c:1244`) | checks, independently |
  | Retype a decl row as a GL command | `editor_place_parsed_command` (`src/editor/input.c:660`) | **`repl_command_store_replace_one`, no check** |
  | Enter over a decl row | `src/editor/input.c:1017` | **`repl_command_store_replace_one`, no check** |
  | Retype a decl row as another decl | `repl_compile_float_decl` overwrite branch (`compile.c:865`) | checks *dropped* names only — must also check kept-but-converted ones [rev 6] |

  The last two are raw replaces. For a global this is survivable — the predef
  slot outlives the row — but a **local's binding exists only as that prologue
  row**, so removing it leaves every assignment to it resolving against nothing.
  Factor one `compile_decl_replacement_is_allowed(ctx, pos, …)` helper and route
  all five through it, rather than adding more independent copies of the check.
  `compile_collect_undeclare_for_range` additionally skips
  `REPL_PREDEF_OP_UNDECLARE` for local decls (no slot to release).
- **Gate the var-assign rebase on both sides being global slots.** [rev 5]
  `repl_compile_var_assign` runs
  `compile_rebase_var_assign_slot_after_undeclares` whenever it overwrites a
  decl row (`compile.c:1260`). A local-target assignment carries
  `var_idx == REPL_VAR_IDX_LOCAL` (-1), which that helper reads as failure, so
  the *legitimate* case — overwriting an unused local decl with an assignment to
  another local — would be rejected with "cannot overwrite declaration of 'x'
  with assignment". Rebase exists to fix up predef slot indices after
  undeclares; run it only when the removed declaration and the assignment target
  are both global. Test the success path, not just the rejection.
- **`tests/test_repl_editor.c:3410-3423` documents that a `CMD_VAR_DECLARE`
  inside a block "cannot arise through normal user input", and the block-batch
  comment-toggle path (`compile.c:1614`) is untested for that reason. This
  feature makes it arise** — that path needs a real test now.
- Cut/copy of decl rows stays blocked (`repl_range_contains_var_decl`,
  `src/repl/source_scope.c:459`); moving a local out of its function would
  silently change its meaning.
- `src/repl/reformat.c:348` `case CMD_VAR_DECLARE:` hardcodes depth-0 indent and
  always emits `static float`. Must honor `var_idx == REPL_VAR_IDX_LOCAL` (plain
  `float`, indent from
  `repl_source_scope_block_depth_at`).

#### Phase 4 — Export / import

- **Export** — `src/repl/export_cmd_writer.c:290`, branch on
  `var_idx == REPL_VAR_IDX_LOCAL`. A local emits a real C declaration at its
  body position, **with explicit zero-initializers**:
  `  float a = 0.0f, b = 0.0f;`. Globals keep the `/* @declare */` marker; the
  comment there explains why a C local would shadow the file-scope static
  written by `write_predef_var_globals`.

  The initializers are not cosmetic. The REPL binds every local to `0.0f` on
  call entry, so a bare `float a, b;` would make a read-before-write `0` in the
  REPL and undefined in the generated file. `docs/ARCHITECTURE.md:2156` states
  the contract — **"Behavior parity is required, not just syntactic
  round-trip"** — and undefined behavior is precisely what the REPL cannot
  reproduce, so "match C" is not available as a resolution. [rev 5]

  No expression translation is needed, because locals carry no initializer of
  their own. (Had they kept one it would route through `repl_eval_expr_to_c`, as
  the `CMD_VAR_ASSIGN` arm at `export_cmd_writer.c:316` does — `ln` → `logf` and
  so on. That is the V2 tax noted under Non-goals.)
- **Import** — lower the generated zero-initializer back to canonical form.
  [rev 5] On an in-body float declaration, strip a *literal-zero* initializer
  and reconstruct `float a;`. This keeps the round-trip idempotent — REPL text
  `float a;` → C `float a = 0.0f;` → REPL text `float a;`, with the second
  round-trip a fixed point — and avoids the failure mode that sank the
  alternative: synthesized `a = 0.0f;` *statement* lines reimport as real
  `CMD_VAR_ASSIGN` rows, so each round-trip would grow the body by one row per
  local.

  Import is the right place for this because it is already a trusted path that
  bypasses commit handlers for exactly this class of reason
  (`parse_snippet_declare`, `import.c:733-736`). Strip **only** the exporter's
  own generated form; a hand-written `float a = 5;` inside a function body must
  still hit the Phase 1 preflight rejection, so REPL and file semantics stay
  identical rather than merely compatible.
- **What import does *not* need.** Exported function-body lines already reach
  `import_try_function_body` (`import.c:1779`) → `repl_load_apply_line`
  (`import.c:1819`), ahead of the predef-stash handlers at
  `import.c:585`/`589`, so `import_parse_predef_decl_common` (`import.c:509`)
  should not need tightening — verify with the round-trip test before touching
  it. Likewise `strip_decl_trailing_comments()`
  (`tests/test_repl_core_examples.c:292`) keys on `static float`, so plain local
  declarations are already excluded. `parse_snippet_declare` (`import.c:607`) is
  untouched — locals never emit that marker.

#### Phase 5 — Docs and example conversion

Docs (all carry `#L` anchors validated by `make check-doc-links`; use
`make fix-doc-links` after edits):

- `docs/USER_GUIDE.md` — `### Variables` (line 754), `### Functions` (862).
- `src/repl/ARCHITECTURE.md` — §8 "Variables, scratch arrays, and time" (717),
  §3.4 "Per-flat-command local variable snapshots" (282), §5.2 "Flatten" (495).
- `src/repl/README.md` — File map (149).
- `.claude/skills/gl-repl-scene-authoring/SKILL.md` — `## The language` (8),
  `## Math` (52, the reserved-names and slot-budget block), `## Budgets` (160).
- `CLAUDE.md` — the "Float declarations (`CMD_VAR_DECLARE`)" section.

Scene conversion (three, chosen to cover func locals, a loop inside a func, and
the export round-trip):

- `examples/scenes/orrery-labels-track-3d-orbits.glr` — 13 Kepler intermediates
  (`aK eK iK lK pK nK mK eAn xh yh x1 y1 z1`) plus `th`/`u` become locals.
  29 decls → ~14. `px/py/pz` **stay global** — the caller reads them, which is
  the return-value case this feature does not address.
- `examples/scenes/swaying-grass-field-rand-t.glr` — `blade()`'s eight
  temporaries; exercises a local written from inside a `for` body (the
  copy-back path).
- `examples/scenes/whale-particle-system-lit-model.glr` — `drawWhale()`'s
  `flukeAng` and `finAng` only. **`detail` stays global** — see below.

**Conversion-safety criterion.** [rev 3] A candidate converts mechanically iff,
within the function, *every read is preceded by a write in the same call*. V1
locals bind to `0.0f` on entry and take no initializer, so any value that
reaches the body from outside it is lost. Auditing the 34 candidates for
read-modify-write turned up exactly two, and they differ:

- **`eAn` (orrery, `planetKepler`) — safe.** Four unrolled Newton-Raphson
  iterations each read `eAn`, but the body seeds it first with
  `eAn = mK + eK*sin(mK);`. Every read is preceded by a write. Convert.
- **`detail` (whale, `drawWhale`) — do not convert.** Its initial value lives in
  the *declaration* (`static float detail = 30;`, scene line 33), and its first
  in-body statement reads it: `detail = max(12, floor(detail));` (line 73).
  Deleting the declaration leaves the read seeing the zero-fill, so the clamp
  yields 12 instead of 30 and the whale's spheres visibly coarsen.

  Seeding it with `detail = 30;` would fix the number but miss the point:
  `detail` is a *knob*, not a temporary. It is a predef, so it appears in the
  variable panel, and the clamp is load-bearing for scrubbing — drag to 8 and it
  holds at 12; drag to 60 and it stays 60. As a local it leaves the panel,
  becomes unscrubbable, and the clamp turns into dead code. This is a false
  positive of the "written and read in exactly one function" classifier, which
  cannot distinguish a temporary from a persistent knob that happens to be used
  in one place. The headline count is therefore 33 genuine locals, not 34.

Related evidence for the budget argument, from the same function: the author is
hand-recycling slots — `// Reuse eAn (done with the eccentric anomaly now) to
hold the radius r, staying under MAX_PREDEF_VARS.`

Then `make rebuild-golden` (which re-enters the build with `USE_GL_STUBS=1`), or
per index: `build/release-gl-stubs/test_repl_core_examples --dump-index N >
tests/testdata/repl_examples_ui/NN.golden.txt`. Goldens are index-keyed but no
example is inserted, so numbering does not shift.

### Verification

**The decisive end-to-end check** — the three conversions are pure refactors of
the same program, so the *executable* flat stream must be unchanged.

A raw `--dump-flat` diff is the wrong instrument, in both directions. [rev 2]
`glr_debug_dump_flat_commands_sync` (`src/app/glr_debug.c:63`) prints type,
`valid`, `has_vars`, `src_cmd_idx`, `call_src_cmd_idx`,
`root_call_src_cmd_idx`, `func_scope_mask` and the source line — and **no
`args[]` at all**. So an empty diff is *insufficient* (every vertex coordinate
could change undetected) and *unnecessary* (moving declarations shifts
`src_cmd_idx` on every following row, and `var_idx` changes by design).

Use a semantic comparator instead:

- Extend `glr_debug_dump_flat_commands_sync` to print `num_args` and
  `args[0..num_args)` plus the owned payload (`payload.matrix.m[]` for
  `CMD_MULT_MATRIXF`, `payload.label.fmt` for `CMD_LABEL`). **Print floats with
  `%a`** (or raw bits) [rev 3] — a decimal rendering can hide a difference below
  its precision, which defeats the point of a byte-exact comparison. This is a
  strict improvement to a debug dump and is useful well beyond this work.
- Compare with provenance and storage metadata filtered out: drop
  `src_cmd_idx` / `call_src_cmd_idx` / `root_call_src_cmd_idx` / `var_idx`, and
  skip `CMD_VAR_ASSIGN` and `CMD_VAR_DECLARE` rows entirely (their storage
  target is exactly what the change is meant to alter). What must match
  byte-for-byte is the sequence of executable command types and their argument
  values.

Run it for all three scenes, and at a non-zero `t` — per the scene-authoring
skill's warning, an expression that constant-folds at parse time and one that
flattens per-frame can disagree, so verify the animated path, not just the
`t = 0` frame.

New tests (extend `tests/test_repl_compile.c`, `test_repl_core_commit.c`,
`test_repl_flatten_differential.c`):

- declare + use a local in a function body; it does **not** appear in
  `g_predef_vars` afterwards
- **redefinition rejections** [rev 8]: a local colliding with a parameter of the
  same function, and with another local of the same body — one case each, and
  the mirror (adding a parameter named after an existing local)
- **legal shadowing is accepted and resolves innermost-first** [rev 8]: a local
  shadowing a global, and a loop iterator shadowing a function local; assert the
  *values*, not just that the commit succeeded — inside the loop the iterator
  wins, after it the local does
- **callee frames are lexical, never dynamic** [rev 9] — with global `x = 10`,
  caller-local `x = 2`, and a callee that has no `x` but reads it, the callee
  must read global `10`, matching exported C. Keep the complementary rev-8 case
  where a callee's own local beats the same-named caller local; the frame is
  `lvars`: callee params, then callee locals, with no caller copy
- **adding a local retargets an older global assignment lexically** [rev 9] —
  first compile a function-body `x = 5;` while `x` is global, then insert local
  `float x;`. On the next call the assignment must update the local and leave
  the global unchanged; assert the emitted flat assignment carries the local
  sentinel even though the persisted source command was compiled with a global
  slot
- **reverse-direction redefinition** [rev 2, narrowed rev 8]: rename a parameter
  onto an existing local, and overwrite a local with a later local's name — and
  drive the parameter case through `repl_compile_func_def_kernel` as the editor
  does (`commit.c:568`), not just the wrapper. The rev-2/rev-3 cases that added
  a *global* or a *loop iterator* over a local are now regression guards on the
  opposite outcome: they must be **accepted when they do not capture a write**
- **unwritable shadow capture is rejected in both edit directions** [rev 9] —
  committing `x = ...` inside `for(x, ...)` must diagnose a constant loop
  variable even when an outer local `x` exists; separately, rename an existing
  `for(i, ...)` to `for(x, ...)` over a body assignment to outer local `x` and
  assert the header edit is rejected atomically. Do the equivalent reverse edit
  for a function parameter over an assignment that previously targeted global
  `x`
- `MAX_EXPR_VARS` overflow at declaration, when parameters are added afterwards,
  and **when a loop iterator is added at the cap** [rev 3] — the last must be
  rejected at compile time, since `flatten_for_loop` would otherwise silently
  drop an outer local (`flatten.c:347-351`)
- **`float x = param;` inside a function body is rejected with the initializer
  message, not an unknown-identifier error** [rev 3]
- **`// @tune` on a local declaration is rejected** [rev 3]
- **overwrite a *used* local decl row into an assignment** [rev 3] — exercises
  `repl_compile_var_assign`'s independent cascade (`compile.c:1244`)
- **overwrite an *unused* local decl row with an assignment to another local**
  [rev 5] — the success counterpart; guards against the slot rebase
  (`compile.c:1260`) reading `REPL_VAR_IDX_LOCAL` as failure and rejecting a
  legal edit
- **overwrite a used local decl row with a GL command, and with Enter** [rev 5]
  — the two raw-replace routes (`input.c:660`, `input.c:1017`); both must be
  rejected by the shared guard
- **whole-function capacity** [rev 5] — with a loop already present later in the
  body, adding one more parameter, and separately one more local, must each be
  rejected rather than silently dropping a binding at flatten time
- **export parity: read before write** [rev 5] — a function that reads a local
  before assigning it must produce identical output from the REPL and from the
  exported C (this is the test the withdrawn divergence would have made
  impossible)
- **export/import idempotence for locals** [rev 5] — `float a;` exports as
  `float a = 0.0f;` and reimports as `float a;`; a second round-trip is a
  fixed point, and the function body does not grow
- **a hand-written `float a = 5;` inside an imported function body is still
  rejected** [rev 5] — import lowers only the exporter's literal-zero form
- **delete a local that is referenced later in the same body must be rejected**
  [rev 3] — the case a reused `compile_line_uses_global_ident` would wrongly
  allow
- recursion — two frames do not share a local (a `sierpinski`-shaped case)
- **accumulate across a `for` inside a func** (the `flatten_for_loop` copy-back)
- **read-before-write in a converted function** [rev 3] — assert a local reads
  `0` on entry, so the conversion-safety criterion has a regression behind it
- **rebake routing** [rev 2] — this must change a *live predef value*, not just
  source text, and assert both the returned `ReplFlatRefreshKind` (a full
  reflatten, never a value-only rebake) and the resulting `args[]`. The
  minimal shape:
  ```
  static float radius;
  drawIt() { float x; x = radius; glVertex3f(x, 0, 0); }
  ```
  Move the `radius` slider; the emitted vertex must track it.
- **hoisting from depth** [rev 4] — `float u;` typed inside a `for`, and inside
  an `if`, both nested in a function, relocate to that function's prologue and
  bind correctly at call time. (Rev 2 had the inverse test here, asserting
  rejection; the rev-4 policy replaces it.)
- **cursor adjustment on hoist** [rev 4] — after the relocation the edit line
  still points at the row the author was typing on, for both the local case
  (row moves up within the body) and `static` from inside a function (row moves
  to the document top)
- **keyword selects storage** [rev 4] — `static float x;` with the cursor inside
  a function body produces a *global* at the document top, not a local; plain
  `float x;` at top level still produces a global (regression guard on existing
  behavior)
- **local→global storage conversion** [rev 6, rev 7, rev 9] — retyping a local
  decl row as `static float x;` is *rejected* while the name is referenced, and when
  unreferenced it relocates to the document top rather than replacing in place.
  Test the used and unused cases and assert no `CMD_VAR_ASSIGN` is left carrying
  a stale `var_idx`. At `MAX_PREDEF_VARS`, the same conversion must be rejected
  before source mutation; the removed local contributes no predef-slot credit.
  A same-name local in a different function must **not** block conversion — it
  legally shadows the new global. An already existing same-name global must
  reject the conversion as a duplicate.
- `float x = 1;` inside a function body is rejected [rev 2]
- `for(i, 0, n) { float i; }` is **accepted** [rev 8] — the local hoists to the
  body top and the iterator shadows it inside the loop, as in C
- **declaration prologue tolerance** [rev 2] — comment out the first of three
  locals; the remaining two must still bind
- **`repl_compile_split_decl` on a local** keeps `var_idx == REPL_VAR_IDX_LOCAL`
  and the local form [rev 2]
- **a parameter or loop iterator may still shadow a global** [rev 6] — a
  regression guard on existing behavior: `func0(i)` over an existing global `i`,
  and `static float i;` added under an existing `for(i, ...)`, must both still
  be accepted
- **delete a local that is referenced only under a shadowing iterator** [rev 8]
  — `float x;` in a body whose sole textual `x` sits inside `for(x, …)` is
  *not* referenced and must delete cleanly; the raw-scan version rev 6
  specified would have blocked it
- delete-guard bounded to the function body; block-batch comment-toggle over a
  body containing a local decl
- export → reimport round-trip preserving the local and its trailing comment
  (write this **before** touching `import.c` — it may already pass [rev 2])
- flatten differential parity (locals must not perturb the fast paths)

Gates:

```bash
make test && make test-stubs
make check-state-ownership     # 67 guards; check-tier-c-function-size caps
                               # flatten_range at 91 lines, parse_command at 335
make check-c99 check-include-style check-duplicate-api-decls
make check-trailing-whitespace check-doc-links
make rebuild-golden            # after the scene conversions

ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
  git pull --ff-only origin main && make check-c99 && make test-stubs'
```

Manual: launch `./gl-repl`, open a converted scene, confirm the variable panel
shows only the remaining globals (no Kepler scratch), scrub a *global* that
feeds a local and confirm the scene tracks it (this is the structural-dep path
from Phase 2, and the place a rebake regression would show as a frozen scene),
and confirm Ctrl+Z after a local decl restores cleanly.

### Resolved by review (rev 2)

1. **Role ordinal space** — moot. Locals have no initializers, so no
   `REPL_EXPR_ROLE_DECL_INIT` is introduced. (For the record, review confirmed 8
   decl ordinals would have been safe: cache identity is `(role, ordinal)`, the
   ordinal is a `short`, and it does not collide with `CMD_ARG` ordinals.)
2. **Dep-mask precision** — the concern was real and is now the load-bearing
   correctness rule: local dataflow must be **structural** under the current
   rebake architecture. Folded into Phase 2 and the test list.
3. **`compile_name_is_active_func_param`** — fold it into kind-tagged
   `collect_visible_vars_in()` rather than leave a third scope walker to drift.
   Phase 1 owns this.
4. **Reformat indentation** — `reformat.c` already computes the correct row
   gutter in `ind_s` (`reformat.c:213`). The decl formatter needs only the
   local/global prefix choice, not new indent logic.

### Known liabilities and revisit triggers

None blocking; both are accepted costs with a defined trip-wire, recorded so a
future maintainer does not have to rediscover the trade-off. [rev 7]

**1. ~~The no-shadowing guard matrix is the maintenance tax.~~ Resolved in
rev 8.** Earlier revisions banned shadowing outright, which turned "declare a
local" into a bidirectional invariant across four compile paths and was named by
review as the design's top maintenance liability and the part most likely to
rot. Rev 8 replaced the ban with C's own rule: same-scope collisions (local vs.
parameter, local vs. local) are *redefinitions* and are rejected; everything
else is ordinary shadowing and is allowed, resolved innermost-first.

That collapsed the four-way *name-collision* guard to the two true
same-scope-redefinition directions. Rev 9 records the runtime work rev 8 had
missed: `eval_primary` already resolves innermost-first, but flatten must give it
a lexical call frame, and scalar assignment LHS resolution needs binding kinds
so persisted `var_idx` metadata cannot defeat a later legal shadow. Parameter
and loop paths retain a narrowly targeted reverse guard only when an edit would
capture an existing assignment and make it write an unwritable binding. The
other cost is that the local delete guard is scope-aware rather than a raw
identifier scan; it is the mirror of `compile_line_uses_global_ident`
(`compile.c:327`), which already does that walk in the opposite direction.

**2. Structural deps trade scrub latency for correctness.** Any global feeding a
local forces a full reflatten instead of a value-only rebake. The converted
orrery — 16 locals fed by globals inside `planetKepler` — is precisely the scene
where this would surface.

*Trigger to revisit:* the manual scrub step in Verification is load-bearing, not
optional. If scrubbing a global that feeds a local is visibly sluggish there,
the fix is teaching `rebake_one_cmd` to simulate local frames — a much larger
change that should be its own plan, not a patch to this one.

**Resolved, for the record:** the export zero-fill divergence (REPL reads 0,
exported C undefined) was flagged as a wart in earlier reviews with the
literal-zero-initializer fix kept "in the back pocket". Rev 5 took that fix off
the shelf — it is now the specified behavior (export emits `float a = 0.0f;`,
import lowers it back) — because `docs/ARCHITECTURE.md:2156` makes behavior
parity a contract rather than a preference.
