## Scoped Local Variable Reassignment Feature Note

## Status — NOT STARTED (2026-05-23 audit)

`repl_compile_var_assign` (src/repl/compile.c:825) still rejects any
target that isn't a predef-var or scratch-array slot — see line 908:
`int var_idx = repl_eval_find_predef_var_idx(name);` followed by
"undeclared variable '%s' - use 'float %s;' first". Loop vars /
function-param locals are visible to the validator (`vis_vars`,
line 859) for read-side identifier validation but never become valid
assignment targets. Stays in `not-started/`.

### Summary
Add a new proposal doc at `feature/writable-scoped-locals.md` using the same structure as `feature/bounded-global-arrays.md`.

Estimated implementation effort for this feature: **medium**, roughly one focused implementation pass plus regression tests. The reason it is not large is that the REPL already has scoped locals for loop variables and function parameters; the missing piece is that assignment, flattening, and replay still assume `foo = expr;` targets predefined globals only.

Target V1 behavior for the proposal:
- Accept `foo = expr;` when `foo` is a currently visible loop variable or function parameter.
- Keep current predefined-global assignment behavior unchanged.
- Do **not** add new declaration syntax and do **not** create locals implicitly on first assignment.

### Important Interface / Behavior Changes
- No new public API functions and no new REPL declaration form in V1.
- User-facing language change:
  - `foo = expr;` is valid when `foo` resolves to the nearest visible local in the current scope.
  - If no visible local matches, resolution falls back to predefined globals.
  - Unknown names are still rejected.
- Reuse `CMD_VAR_ASSIGN`; the proposal should explicitly avoid adding a new command type for local assignment.

### Key Changes To Capture In The Markdown
- Parser / commit path:
  - Make assignment target resolution scope-aware in `repl_editor.c`.
  - Search visible locals at the insertion point before checking `g_predef_vars`.
  - Preserve normalized source exactly as today so save/export still emits `foo = expr;`.
- Flattening / execution semantics:
  - Mutate the active local scope state in `flatten_range()` when a local assignment is encountered so later commands in the same loop iteration, taken `if` body, or function invocation see the updated value.
  - Keep loop-variable reassignment iteration-local: each new loop iteration starts from that iteration’s loop index value.
  - Keep function-call locals invocation-local: assignments inside a call do not leak back to the caller’s copied visible locals.
  - Predefined globals continue to mutate globally as they do today.
- Export / import:
  - Exported C should need no new syntax; local assignments should round-trip as ordinary C assignments inside generated loops/functions.
  - Import should rely on the same scope-aware assignment parsing, with no new file-format markers.
- Proposal markdown should explicitly call out non-goals:
  - no `local foo = ...;`
  - no implicit local creation from unknown names
  - no local arrays or separate local symbol tables beyond existing loop vars / function params

### Test Plan
- Add commit/flatten regressions in `test_repl_core_commit.c` for:
  - assigning to a loop variable inside a loop and verifying later commands in the same iteration use the new value
  - assigning to a function parameter and verifying later commands in that invocation use the new value
  - shadowing behavior: innermost visible local wins over outer locals and predefined globals
  - unknown local assignment still rejected
  - top-level unknown assignment still rejected
- Add save/load regression in `test_repl_core_io.c` proving a snippet with writable locals exports and reloads without losing structure or source text.
- Keep existing predefined-global assignment tests unchanged to prove no regression in current `x/y/z/...` behavior.

### Assumptions And Defaults
- V1 writable locals are limited to existing loop variables and function parameters.
- Assignment target precedence is: innermost visible local, then predefined global, then reject.
- Function-visible outer locals stay pass-by-value copies within the callee, matching the current flatten model.
- The feature note should present this as a contained language/runtime extension, not a broader variable-system redesign.
