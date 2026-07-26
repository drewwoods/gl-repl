## Function-Scoped Local Variables

## Status — NOT STARTED (2026-07-26, rev 3 after second review)

Rev 3 incorporates a second review (four blocking findings plus four
corrections, all confirmed). One changed the representation, the rest tightened
under-specified guards; items are marked **[rev 3]** in place. Headlines:

- **`is_local` moves onto `var_idx`, not into the payload union.** Measured,
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
- **Exported locals are uninitialized C automatics** while the REPL zero-fills.
  Accepted as a documented divergence (the language is C-like, and C's rule is
  the right one for exported code); rejected alternatives recorded in Phase 4.

Rev 2 incorporated a first review that found seven material issues, all
confirmed against the tree. Two changed the design rather than the wording:

- **Locals take no initializer in V1.** `format_decl_text` (`compile.c:753`)
  emits `" = %g"`, destroying the initializer *expression* at commit time, and
  `parse_float_name_list` (`compile.c:654`) validates initializer identifiers
  against predefs only and evaluates immediately. So `float tmp = param;` cannot
  work through the existing path no matter how locality is detected, and
  `float a = PI*2;` would reach flatten as the constant `6.28319`. All 34
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
| Top-level scratch / tunable (genuine globals) | 105 |
| Const tunables (`@tune`, never reassigned) | 63 |
| **Written and read inside exactly one func — wants a local** | **34** |
| Written in a func, read by the caller — wants a return value | 9 |
| Mixed | 9 |

34 of 220 declared names exist only because the language has no local storage.
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
| Where may a local be declared | **Function bodies only.** All 34 observed cases are function-body temporaries. `for` / `if` bodies reject with a clear message. |
| Shadowing | **Rejected.** A local may not reuse a visible param, loop var, outer local, or global name. Authors already hand-avoid this. |
| Writable params / loop vars | **Out of scope.** Keep the `compile.c:1146` guard ("function parameters are constant"). `float tmp; tmp = param;` covers the need. |
| Example conversion | **3 scenes as proof** in this change: orrery, swaying-grass, whale. |

### V1 behavior

- `float a, b;` inside a function body declares function-scoped locals.
  **No initializer** — see Non-goals and the rev-2 status note.
- Declaring is only legal when the **nearest open block is the `CMD_FUNC_DEF`
  itself**, not merely when some enclosing function exists. [rev 2]
- Locals **hoist to the top of the function body**, mirroring the existing rule
  that top-level decls hoist to the top of non-decl code. Every reference
  therefore follows its declaration, and flatten's binding is a prefix scan.
- Each local binds to `0.0f` on entry to the call, with dep mask 0.
- Assignment `a = expr;` targets the local when one is visible. Every dep of
  that RHS is reported **structural**. [rev 2]
- Locals are **invocation-local**: `flatten_call` copies params + outer scope
  into a fresh `lvars[MAX_EXPR_VARS]` per call and never copies back, so
  recursion is correct for free.
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
- No local declarations in `for` or `if` bodies (V2 candidate).
- No writable function parameters or loop variables.
- No shadowing.
- No local arrays; `A`/`B`/`C` stay global scratch.
- No `// @tune` / `// @config` on a local — those need a panel slot, so they are
  rejected.
- No implicit local creation from an unknown assignment target.

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
  for a global (unchanged), `<indent>float a, b;` for a local (no `static`, and
  no initializer — V1 locals have none). `static float`
  typed inside a function body is rejected — "static declarations must be at the
  top level" — rather than silently meaning a third thing. This also makes the
  export/import round-trip fall out for free.
- `CMD_VAR_ASSIGN` targeting a local carries `var_idx = REPL_VAR_IDX_LOCAL (-1)`
  and emits no `REPL_PREDEF_OP_SET_VALUE`. The target name comes from
  `repl_extract_assignment_parts()` (`src/repl/text_helpers.h:96`), which flatten
  already calls with a NULL name buffer today.

### Why this shape

It reuses three mechanisms wholesale rather than adding a fourth: the per-block
`ExprVar` scope arrays flatten already builds, the `ReplExprDepMask` array that
already rides alongside them, and the "names are re-derived from source text"
convention that loop variables and parameters already follow. Name resolution
needs no evaluator change — `eval_primary` (`src/repl/eval.c:1227-1243`) already
checks `ctx->vars` before the predef table.

`flatten_range` (`src/repl/flatten.c:1076`) grows by zero lines, which matters:
`scripts/baselines/tier-c-function-size.txt` ratchets it at 91 lines and
`parse_command` at 335.

### Implementation

#### Phase 1 — Representation and declaration compile

- `src/repl/command.h` — add `payload.decl.is_local`; define
  `REPL_VAR_IDX_LOCAL`.
- `src/repl/compile.c:826` `repl_compile_float_decl` — **decide locality before
  parsing**, because the parser's initializer handling differs (locals reject
  them outright). [rev 2] Use the existing
  `compile_nearest_open_block_head_at(ctx, insert_pos, &type)`
  (`compile.c:215`) — it returns both the block index and its `CmdType` in one
  call, which is exactly the "direct function body only" test *and* supplies the
  body bound for the reference scan in Phase 3. Do **not** use an
  enclosing-function search: that still resolves to the function from inside a
  nested `for`/`if`, silently taking the local path where the plan says reject.
  - no open block → existing global path, untouched.
  - open block is `CMD_FUNC_DEF` → local path: `decl_pos` = first non-decl row
    after the func header; `build_decl_predef_ops` emits nothing; canonical text
    has no `static`, and `format_decl_text` (`compile.c:739`) grows a
    local/global prefix switch (it needs no initializer handling, since locals
    have none).
  - open block is `CMD_FOR_BEGIN` / `CMD_IF_BEGIN` → reject.
- **The parser needs a locality-aware preflight, because it currently rejects
  the local cases before any local diagnostic can run.** [rev 3]
  `parse_float_name_list` (`compile.c:573`) strips the optional `static` prefix
  without reporting whether the user typed it, and validates initializer
  identifiers against predefs only (`compile.c:654`) — so `float x = param;`
  dies with "unknown identifier 'param'" before `validate_local_decl_names()`
  can emit the promised initializer message. Add a lexical preflight (or a
  `FloatDeclParse` mode flag) that, on the local path, rejects **before**
  initializer validation and evaluation:
  - a `static` prefix → "local declarations cannot be static"
  - any top-level `=` → "local declarations cannot have an initializer — assign
    on the next line"
  - a `// @tune` / `// @config` tag → "@tune/@config require a global
    declaration" (these need a variable-panel slot, which locals do not have —
    this is the enforcement point the earlier revision promised but never
    located)

  Test with `float x = param;`, not only `float x = 1;` — they fail in different
  places today.
- New `validate_local_decl_names()` beside `validate_decl_names`
  (`compile.c:693`). Rejects: duplicate-in-decl; any name visible at that point
  per `collect_visible_vars_in()` (param / loop var / outer local); any name in
  `ctx->predef` (global); reserved idents via `repl_eval_is_reserved_ident()`;
  bad ident shape or >15 chars; any initializer; and
  `visible_count + parsed.count > MAX_EXPR_VARS`.
- `src/repl/visible_vars.{c,h}` — teach `collect_visible_vars_in()` to add local
  decl names to the current `CMD_FUNC_DEF` `ScopeFrame` (value `0.0f`, same as
  params — it is a name scope, not a value scope), and add an optional
  `ReplVisibleVarKind *kinds_out` parameter (LOOP / PARAM / LOCAL) so callers can
  tell them apart. One place; all 14 call sites benefit.
- `src/repl/compile.c:1038` `repl_compile_var_assign` — before the "undeclared
  variable" error, check for a visible **local decl** (kind LOCAL, not
  PARAM/LOOP) and emit `var_idx = REPL_VAR_IDX_LOCAL` with no predef op. The
  param guard at `compile.c:1146` stays exactly as it is.

#### Phase 2 — Flatten

- `src/repl/flatten.c:358` `flatten_call` — after binding params into `lvars`,
  call a new `flatten_bind_func_locals(ctx, body_start, body_end, lvars, ldeps,
  &lnv)` that walks the body's **declaration prologue** and appends
  `{name, 0.0f}` with dep mask 0 for each name. No expression evaluation is
  involved, so no new `ReplFlattenExpr` role and no shared decl-line scanner is
  needed — names come straight off `payload.decl.names[]`.
- **Define the prologue to tolerate `CMD_COMMENT` and `CMD_EMPTY`**, stopping at
  the first other command type. [rev 2] A strictly contiguous run breaks the
  moment an author comments out the first local: the body becomes
  `CMD_COMMENT, CMD_VAR_DECLARE, …` and every later local silently stops
  binding. The same tolerance is what lets a `// grouping comment` sit between
  declaration rows.
- `src/repl/flatten.c:868` `flatten_var_assign` — when `var_idx` is
  `REPL_VAR_IDX_LOCAL`, resolve the LHS name against `vars[0..nv)` and write
  `vars[k].value = value` / `var_deps[k] = rhs_deps` instead of touching
  `g_predef_vars_mut`. Requires dropping `const` from `var_deps` along the
  `flatten_range` → `flatten_var_assign` chain.
- **In that same branch, report the RHS deps via
  `repl_flatten_expr_note_structural()`, not `note_value()`.** [rev 2] This is
  the fix for the rebake hazard in the status note: `rebake_one_cmd` cannot
  thread a local's new value into later commands' frozen snapshots, so any
  predef change that can reach a local must force a full reflatten instead of a
  value-only rebake. Because assignment is now the *only* way a value enters a
  local (no initializers), marking the assignment RHS structural covers the
  whole dataflow. Cost: editing a global that feeds a local reflattens rather
  than rebakes — correct, and bounded.
- **`src/repl/flatten.c:248` `flatten_for_loop` — copy back.** This is the one
  non-obvious correctness point. `lvars` is rebuilt per iteration, so without a
  copy-back of the outer entries after each `flatten_range` returns,
  `float acc; acc = 0; for(i,0,n) { acc = acc + i; }` silently resets every
  iteration. Copy `lvars[1..]` back into `vars[]` (skipping the iterator at
  index 0). `flatten_if_block` needs nothing — it shares the caller's array.
  `flatten_call` deliberately does not copy back.
- `flatten_range` (`flatten.c:1076`): unchanged apart from the `const` removal.
  Its existing `if (src_cmd->type == CMD_VAR_DECLARE) { i++; continue; }` at
  line 1138 is already correct.
- `src/repl/executor.c:887` needs **no change** — its `var_idx >= 0` guard makes
  a local-target assign a natural no-op.

#### Phase 3 — Edit guards and formatting

**The no-shadowing and capacity invariants must be enforced in both
directions.** [rev 2] Validating only at the point the local is declared leaves
every later edit free to create the collision the invariant forbids, and flatten
resolves locals by *name* — so a stale classification means an assignment
compiled as "local" mutates a same-name iterator or parameter instead. The
binders have separate compile paths, and each must now also validate against
existing locals in scope:

| Edit | Path | Must reject |
|---|---|---|
| Add/rename a function parameter | **`repl_compile_func_def_kernel`** | a name matching a local in that body; `params + locals > MAX_EXPR_VARS` |
| Add a loop iterator inside a func body | `repl_compile_for_loop_kernel` (`compile.c:2453`) | a name matching a visible local; **`visible_count + 1 > MAX_EXPR_VARS`** |
| Add a global | `repl_compile_float_decl` global path | a name matching any existing local |
| Add/rename a local | local path | a name matching a *later* local in the same body, **or an existing loop iterator later in the function** |

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

- `repl_compile_split_decl` (`compile.c:921`) must preserve `is_local` and emit
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
  reference lines, not the declaration. Because shadowing is forbidden, the
  local variant can be simpler and stricter: a **raw identifier scan over the
  function body** (via `repl_eval_source_uses_ident`, comment-aware), excluding
  the range being changed.
- **Two overwrite paths, not one.** [rev 3] `compile_collect_undeclare_for_range`
  (`compile.c:1413`) must skip local decls when emitting
  `REPL_PREDEF_OP_UNDECLARE` (no slot to release) while still running the
  bounded reference check — **and `repl_compile_var_assign` has its own
  independent decl-overwrite cascade** (`compile.c:1244`) that calls
  `compile_name_is_still_referenced` directly. Editing a local declaration row
  into an assignment goes through that second path and is missed if only the
  first is updated. Needs an explicit "overwrite a used local" test.
- **`tests/test_repl_editor.c:3410-3423` documents that a `CMD_VAR_DECLARE`
  inside a block "cannot arise through normal user input", and the block-batch
  comment-toggle path (`compile.c:1614`) is untested for that reason. This
  feature makes it arise** — that path needs a real test now.
- Cut/copy of decl rows stays blocked (`repl_range_contains_var_decl`,
  `src/repl/source_scope.c:459`); moving a local out of its function would
  silently change its meaning.
- `src/repl/reformat.c:348` `case CMD_VAR_DECLARE:` hardcodes depth-0 indent and
  always emits `static float`. Must honor `is_local` (plain `float`, indent from
  `repl_source_scope_block_depth_at`).

#### Phase 4 — Export / import

- `src/repl/export_cmd_writer.c:290` — branch on `is_local`: emit the real C line
  `  float a, b;` at its body position instead of the `/* @declare */` marker.
  No shadowing risk, because compile rejects shadowing; and no expression
  translation is needed, because locals carry no initializer. (Had they kept
  one, it would have to route through `repl_eval_expr_to_c`, as the
  `CMD_VAR_ASSIGN` arm at `export_cmd_writer.c:316` does — `ln` → `logf` and so
  on. That is the V2 tax noted under Non-goals.) [rev 2] Globals keep the
  marker; the comment there explains why a C local would shadow the file-scope
  static written by `write_predef_var_globals`.
- **Accepted divergence: read-before-assign.** [rev 3] The REPL binds every
  local to `0.0f` on call entry; `float a, b;` in exported C is an
  uninitialized automatic, so the same read is undefined there. This is
  deliberate — the language is C-like, and C's rule is the one exported code
  should follow. Document it in `docs/USER_GUIDE.md` next to the local-decl
  syntax: *assign before you read; the REPL happens to zero-fill, exported C
  does not.*

  The two ways to close the gap were both rejected for V1. Emitting synthesized
  `a = 0.0f;` lines after the declaration round-trips as ordinary assignments,
  but reimport turns them into real `CMD_VAR_ASSIGN` rows, so the next export
  emits them *again* — the body grows by one row per round-trip. Emitting
  `float a = 0.0f;` would require the local path to accept a literal-zero
  initializer as a no-op and strip it from canonical text, which is a small but
  real special case in the parser preflight above. If a future reviewer wants
  the divergence closed, the literal-zero variant is the cheaper of the two.
- **Import probably needs no change — verify before touching it.** [rev 2]
  Exported function-body lines already reach `import_try_function_body`
  (`import.c:1779`), which routes through `repl_load_apply_line`
  (`import.c:1819`), ahead of the predef-stash handlers at `import.c:585`/`589`.
  Likewise `strip_decl_trailing_comments()`
  (`tests/test_repl_core_examples.c:292`) keys on `static float`, so plain local
  declarations are already excluded. Write the round-trip test first and change
  `import_parse_predef_decl_common` (`import.c:509`) only if it actually
  mis-claims an in-body `float x;`. `parse_snippet_declare` (`import.c:607`) is
  untouched either way — locals never emit that marker.

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
- shadow rejections: param, loop var, outer local, global — one case each
- **reverse-direction collisions** [rev 2]: add a global named after an existing
  local; rename a param onto a local; add a loop iterator named after a visible
  local; overwrite a local with a later local's name — one case each.
  Plus [rev 3]: **add/rename a local onto an iterator that already exists later
  in the function** (the mirror case rev 2 missed), and drive the param case
  through `repl_compile_func_def_kernel` as the editor does, not just the
  wrapper
- `MAX_EXPR_VARS` overflow at declaration, when parameters are added afterwards,
  and **when a loop iterator is added at the cap** [rev 3] — the last must be
  rejected at compile time, since `flatten_for_loop` would otherwise silently
  drop an outer local (`flatten.c:347-351`)
- **`float x = param;` inside a function body is rejected with the initializer
  message, not an unknown-identifier error** [rev 3]
- **`// @tune` on a local declaration is rejected** [rev 3]
- **overwrite a *used* local decl row into an assignment** [rev 3] — exercises
  `repl_compile_var_assign`'s independent cascade (`compile.c:1244`)
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
- a local decl in a `for` / `if` body is rejected — **including when the cursor
  sits in a `for` nested inside a function**, which is the case an
  enclosing-function search gets wrong [rev 2]
- `static float` inside a function body is rejected; `float x = 1;` inside a
  function body is rejected [rev 2]
- **declaration prologue tolerance** [rev 2] — comment out the first of three
  locals; the remaining two must still bind
- **`repl_compile_split_decl` on a local** keeps `is_local` and the local form
  [rev 2]
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

### Open questions

None blocking. The one judgement call left for implementation is whether the
structural-dep rule in Phase 2 proves too coarse in practice — if editing a
global that feeds a local causes visibly sluggish scrubbing in the converted
scenes, the alternative is teaching `rebake_one_cmd` to simulate local frames,
which is a much larger change and should be its own plan.
