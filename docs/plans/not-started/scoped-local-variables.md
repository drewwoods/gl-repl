## Function-Scoped Local Variables

## Status — NOT STARTED (2026-07-26)

Rewritten 2026-07-26. This plan replaces an earlier note of the same name whose
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

- `float a, b = expr;` inside a function body declares function-scoped locals.
- Locals **hoist to the top of the function body**, mirroring the existing rule
  that top-level decls hoist to the top of non-decl code. Every reference
  therefore follows its declaration, and flatten's binding is a prefix scan.
- Initializers re-evaluate **per call**, against params plus already-bound
  locals.
- Assignment `a = expr;` targets the local when one is visible.
- Locals are **invocation-local**: `flatten_call` copies params + outer scope
  into a fresh `lvars[MAX_EXPR_VARS]` per call and never copies back, so
  recursion is correct for free.
- Locals never enter `g_predef_vars`. The variable panel, `@tune` knobs, replay
  baseline, export prologue and slot-shift cascade are all keyed on predef slots
  and need no changes.

### Non-goals

- No local declarations in `for` or `if` bodies (V2 candidate).
- No writable function parameters or loop variables.
- No shadowing.
- No local arrays; `A`/`B`/`C` stay global scratch.
- No `// @tune` / `// @config` on a local — those need a panel slot, so they are
  rejected.
- No implicit local creation from an unknown assignment target.

### Representation

- `payload.decl.is_local` — new int on the decl struct
  (`src/repl/command.h:139-142`). The union is keyed on `type` and has room.
- **Canonical text distinguishes them, matching C:** `  static float a, b = 2;`
  for a global (unchanged), `<indent>float a, b = 2;` for a local. `static float`
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
- `src/repl/compile.c:826` `repl_compile_float_decl` — after parse, resolve the
  enclosing function via a new `compile_enclosing_func_def_idx(ctx, insert_idx)`,
  extracted from the `ScopeFrame` walk already inside
  `compile_name_is_active_func_param` (`compile.c:265`). Returns the
  `CMD_FUNC_DEF` index or -1.
  - `-1` → existing global path, untouched.
  - `>= 0` → local path: `decl_pos` = first non-decl row after the func header;
    `build_decl_predef_ops` emits nothing; canonical text has no `static`.
- New `validate_local_decl_names()` beside `validate_decl_names`
  (`compile.c:693`). Rejects: duplicate-in-decl; any name visible at that point
  per `collect_visible_vars_in()` (param / loop var / outer local); any name in
  `ctx->predef` (global); reserved idents via `repl_eval_is_reserved_ident()`;
  bad ident shape or >15 chars; and
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

- New shared decl-line scanner in `src/repl/text_helpers.c`, built on the
  existing `repl_scan_decl_float_prefix()` (`text_helpers.c:247`), yielding
  `(name, init-expression span)` pairs. Four near-duplicate scanners exist today
  (`compile.c` `parse_float_name_list`, `reformat.c` `reformat_var_decl_text`,
  `import.c:512`); this one is additive, and the others can adopt it later.
- `src/repl/flatten.c:358` `flatten_call` — after binding params into `lvars`,
  call a new `flatten_bind_func_locals(ctx, body_start, body_end, lvars, ldeps,
  &lnv)` that walks the leading `CMD_VAR_DECLARE` rows of the body, evaluates
  each initializer against the scope built so far, and appends `{name, value}`
  plus its dep mask. Uses the existing `ReplFlattenExpr` warm cache with a new
  `REPL_EXPR_ROLE_DECL_INIT` role (ordinal = name index); the text path is the
  cold fallback, same shape as the for-header path.
- `src/repl/flatten.c:868` `flatten_var_assign` — when `var_idx` is
  `REPL_VAR_IDX_LOCAL`, resolve the LHS name against `vars[0..nv)` and write
  `vars[k].value = value` / `var_deps[k] = rhs_deps` instead of touching
  `g_predef_vars_mut`. Requires dropping `const` from `var_deps` along the
  `flatten_range` → `flatten_var_assign` chain.
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

- `compile_name_is_still_referenced` (`compile.c:352`) walks the whole document.
  Add a body-bounded variant for locals, and make sure
  `compile_line_uses_global_ident`'s shadow check (`compile.c:327`) does not
  treat the local's own decl as a shadowing binder.
- `compile_collect_undeclare_for_range` (`compile.c:1413`) must skip local decls
  when emitting `REPL_PREDEF_OP_UNDECLARE` (no slot to release) while still
  running the bounded reference check.
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
  `  float a, b = 2;` at its body position instead of the `/* @declare */`
  marker. No shadowing risk, because compile rejects shadowing. Globals keep the
  marker (the comment there explains why: a C local would shadow the file-scope
  static written by `write_predef_var_globals`).
- `src/repl/import.c:509` `import_parse_predef_decl_common` matches
  `[static] float ...` anywhere. Tighten it to the prologue (before the first
  generated function body) so an in-body `float x;` falls through to
  `editor_feed_line` and round-trips through the normal commit chain into a
  local. `parse_snippet_declare` (`import.c:607`) is untouched — locals never
  emit that marker.
- `tests/test_repl_core_examples.c` `strip_decl_trailing_comments()` exists
  because `@declare` cannot carry a trailing comment. Locals **can** keep theirs
  through export, so the stripping must not apply to them.

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
  `flukeAng`/`finAng`/`detail`.

Then `make rebuild-golden` (which re-enters the build with `USE_GL_STUBS=1`), or
per index: `build/release-gl-stubs/test_repl_core_examples --dump-index N >
tests/testdata/repl_examples_ui/NN.golden.txt`. Goldens are index-keyed but no
example is inserted, so numbering does not shift.

### Verification

**The decisive end-to-end check** — the three conversions are pure refactors of
the same program, so the flat command stream must be byte-identical:

```bash
./gl-repl --example "Orrery ..." --dump-flat > /tmp/before.txt   # pre-conversion
# ...convert the scene...
./gl-repl --example "Orrery ..." --dump-flat > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt        # must be empty
```

Run the same for the other two scenes, and at a non-zero `t` — per the
scene-authoring skill's warning, an expression that constant-folds at parse time
and one that flattens per-frame can disagree, so verify the animated path, not
just the `t = 0` frame.

New tests (extend `tests/test_repl_compile.c`, `test_repl_core_commit.c`,
`test_repl_flatten_differential.c`):

- declare + use a local in a function body; it does **not** appear in
  `g_predef_vars` afterwards
- shadow rejections: param, loop var, outer local, global — one case each
- `MAX_EXPR_VARS` overflow on params + locals
- recursion — two frames do not share a local (a `sierpinski`-shaped case)
- **accumulate across a `for` inside a func** (the `flatten_for_loop` copy-back)
- a local decl in a `for` / `if` body is rejected with the intended message
- `static float` inside a function body is rejected
- delete-guard bounded to the function body; block-batch comment-toggle over a
  body containing a local decl
- export → reimport round-trip preserving the local and its trailing comment
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
shows only the remaining globals (no Kepler scratch), edit a local's initializer
and confirm the scene responds, and confirm Ctrl+Z after a local decl restores
cleanly.

### Open questions for review

1. **`REPL_EXPR_ROLE_DECL_INIT` ordinal space.** Roles are per-line with an
   ordinal; a decl line can carry up to `MAX_NAMES_PER_DECL` = 8 initializers.
   Confirm 8 ordinals fit the existing role/ordinal encoding without colliding
   with `REPL_EXPR_ROLE_CMD_ARG`'s 0..15 matrix-slot usage.
2. **Dep-mask precision for locals.** Writing `var_deps[k] = rhs_deps` on assign
   is precise, but a local read *before* its assignment in the same body sees the
   initializer's mask. Confirm that under-approximates nothing — the concern is a
   value-only rebake that should have been a structural reflatten.
3. **`ReplVisibleVarKind` vs. `compile_name_is_active_func_param`.** The latter
   (`compile.c:265`) is a second, independent scope walker. Should Phase 1 fold
   it into `collect_visible_vars_in()` with the new kind tag, or leave both and
   accept the duplication?
4. **Reformat round-trip of a local decl's indentation** when the enclosing
   function is itself reformatted — verify `reformat_var_decl_text` and
   `repl_source_scope_block_depth_at` agree on the gutter.
