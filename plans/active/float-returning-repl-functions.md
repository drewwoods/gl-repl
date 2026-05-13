# Float-Returning REPL Functions

## Summary
Implement `return expr;` inside REPL functions and make every user function conceptually return a `float`, defaulting to `0.0f` when no explicit return is reached. Existing statement calls like `func0(...);` keep working and ignore the return value. New expression calls like `glVertex3f(func0(4), 0, 0);` and `x = func0(n);` evaluate through a scalar function evaluator. Expression-called functions are scalar-only: if evaluation reaches a GL/render/state command before returning, flatten/evaluation fails with a status error instead of silently skipping side effects.

## Key Changes
- Add `CMD_RETURN` to the command model and specs.
  - Parse only `return expr;`; reject top-level returns and require the enclosing open block stack to contain a `CMD_FUNC_DEF`.
  - Normalize as `return <expr>;`, preserve raw expression text in the editor buffer, and mark it dynamic when it references predef vars, locals, scratch arrays, or user function calls.
  - Add `return` to reserved identifiers so it cannot be used as a variable or function alias.

- Add user-function support to expression evaluation without making `repl_eval.c` depend on document state.
  - Extend `ExprCtx` with an optional user-function callback plus user data; existing callers without the callback keep current behavior.
  - Widen expression-call argument storage from the current built-in-only `float args[3]` shape to `MAX_EXPR_VARS` so user functions can use the same parameter limit as function definitions.
  - Add a helper that detects `funcN(...)` or alias calls in expressions so parser/commit paths preserve those expressions for flatten-time evaluation instead of folding them to `0`.

- Add a REPL-owned scalar function evaluator.
  - New REPL module walks function bodies from source commands plus editor-buffer text, binds params as local `ExprVar`s, evaluates `if`/`for` control flow, handles nested scalar function calls, and returns the first reached `CMD_RETURN`.
  - If no return is reached, return `0.0f`.
  - Use the same recursion depth and visit-budget style as flattening; surface errors through caller-owned buffers, not `set_status`.
  - In scalar mode, reject reached non-scalar commands: GL draw/state/tessellation commands, global/scratch assignments, var declarations, and statement `CMD_CALL`. Use `return funcN(...);` for nested scalar calls.

- Integrate with flattening.
  - Statement-call flattening still expands function bodies for drawing as today.
  - When flattening a statement function call, a reached `CMD_RETURN` stops expansion of that call body and resumes after the call site; the returned value is ignored.
  - All flatten-time expression evaluation sites use the callback-aware evaluator: GL args, `if` conditions, `for` bounds, var assignment RHS, scratch index/RHS, and return expressions.
  - Top-level expressions that reference undefined functions fail like existing strict top-level statement calls.

## Export And Import
- Export every REPL function as `static float funcN(...)`, not `static void`.
  - Emit forward prototypes for all exported functions before definitions so recursion and mutual recursion compile in C.
  - Statement calls still emit as `funcN(...);`; C ignores the returned value.
  - `CMD_RETURN` emits `return <translated_expr>;`.
  - Always append `return 0.0f; // @repl-default-return` before the closing brace of each generated C function so C fallthrough is valid while preserving REPL default-return semantics.

- Make exported C use slot names, not aliases, in executable function calls.
  - Keep existing `// @func N = alias` directives for REPL round-trip.
  - When emitting `CMD_CALL` statements or expressions containing user function calls, rewrite aliases to `funcN(...)` in C output so saved code compiles.
  - On import, directives restore aliases before function/call lines are fed back into the REPL, so editor text can normalize back to alias form.

- Update import.
  - Accept both legacy `static void funcN(...)` and new `static float funcN(...)` headers.
  - Feed user-authored `return expr;` lines into the parser as `CMD_RETURN`.
  - Ignore only the generated fallback line carrying `@repl-default-return`; do not drop user-authored `return 0;`.

## Test Plan
- Parser/commit tests:
  - `return n + 1;` parses inside `funcN` and rejects outside a function.
  - `return` is reserved for variable declarations and aliases.
  - Return expressions preserve aliases, locals, predefs, scratch reads, and nested user-function calls in editor text.

- Flatten/evaluator tests:
  - `func0(n) { if(n <= 1) { return 1; } return n * func0(n - 1); }` used in `glVertex3f(func0(4), 0, 0);` flattens to `x == 24`.
  - A function with no return used in an expression evaluates to `0`.
  - A statement-called drawing function still renders/expands as before when it has no return.
  - A statement-called function stops expanding commands after a reached `return`.
  - An expression-called function whose reached path contains `glVertex3f`, `glColor3f`, assignment, scratch assignment, or statement `funcN();` fails with a clear status.
  - Recursion and mutual recursion hit the existing depth/visit-budget protections.

- Export/import tests:
  - Saved C contains `static float funcN` prototypes and definitions.
  - Explicit returns export as C `return` statements.
  - Missing returns export with `return 0.0f; // @repl-default-return`.
  - Reloading exported code restores user-authored returns but does not create an extra editor line for generated default returns.
  - Alias-backed functions used in expressions export executable C using `funcN(...)`, while import restores the alias directive and editor display.
  - Exported recursive and mutually recursive scalar functions compile under the normal test/export path.

- Run focused and full checks:
  - `make test_eval`
  - `make test_repl_core_parse`
  - `make test_repl_core_commit`
  - `make test_repl_core_io`
  - `make test`
  - `make test-stubs`

## Assumptions
- v1 supports `return expr;` only; use `return 0;` for an explicit zero return.
- No local variable declaration feature is added in this work.
- Expression-called user functions are scalar-only and side-effect-free; statement calls remain the path for drawing/procedural functions.
- All exported REPL functions become `float` functions, and statement callers intentionally ignore the result.

