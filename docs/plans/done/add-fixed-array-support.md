# Fixed Scratch Arrays `A/B/C[8]` for Recursive REPL Programs

## Summary
Add a deliberately small array feature for recursive/loop algorithms: three predefined global scratch arrays, `A[8]`, `B[8]`, and `C[8]`. They are mutable during flatten/execution, visible from recursive function calls, and separate from scalar predefined variables.

Supported v1 syntax:

```c
A[i] = lerp(A[i], A[i + 1], u);
glVertex3f(A[0], B[0], 0);
```

Non-goals: user-declared arrays, dynamic sizes, array parameters, array literals, multidimensional arrays, variable-panel array editing.

## V1 Semantics
- `A`, `B`, and `C` are reserved identifiers.
- Index expressions are normal REPL float expressions, converted with `(int)value`.
- Valid index range is `0..7`.
- Scratch arrays are global mutable runtime state. Recursive function calls see writes made by earlier recursive frames.
- `lerp(a, b, t)` is a new expression builtin equal to `a + (b - a) * t`.
- `float A;`, `func0(A) {`, and `for(A, 0, 8, 1)` must be rejected.
- Array reads in evaluator-only fallback paths may return `0` on error, but compile/parse/flatten paths with error buffers must surface a clear status message.

## Implementation Steps

### Step 1 - Add Scratch State And Helpers
Files: `repl_eval.h`, `repl_eval.c`, `repl_state_views.h`, `repl_state.c`, `repl_state_defaults.inc`.

Add:

```c
#define REPL_SCRATCH_ARRAY_COUNT 3
#define REPL_SCRATCH_ARRAY_LEN 8
```

Extend `ReplVariableState`:

```c
float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
```

Add evaluator accessors:

```c
void repl_eval_bind_scratch_storage(float arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);
void repl_eval_reset_scratch_arrays(void);
int  repl_eval_scratch_array_index(const char *name); /* A=0, B=1, C=2, else -1 */
int  repl_eval_scratch_get(int array_idx, int elem_idx, float *out);
int  repl_eval_scratch_set(int array_idx, int elem_idx, float value);
void repl_eval_copy_scratch_arrays(float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);
void repl_eval_restore_scratch_arrays(const float src[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);
```

Use fallback storage in `repl_eval.c` for standalone evaluator tests, mirroring current `g_fallback_predef_vars`.

Verification:
- Add `test_eval` coverage for reset/get/set/copy/restore.
- Run `make test_eval`.

### Step 2 - Reserve Names And Add `lerp`
Files: `repl_eval.c`, `repl_export.c`, commit/for/function tests.

- Add `A`, `B`, `C`, and `lerp` to `s_reserved_idents`.
- Update function parameter parsing so `func0(A) {` fails.
- Update for-loop commit validation so `for(A, 0, 8, 1)` fails.
- Update float declaration validation so `float A;`, `float B;`, `float C;` fail through the existing reserved-name path.
- Replace the evaluator’s current one/two-arg function parsing with a small fixed arg parser that supports existing builtins plus `lerp(a,b,t)`.

Verification:
- `lerp(0, 10, 0.25)` returns `2.5`.
- Existing `pow`, `min`, `max`, `rand`, `sin` behavior remains unchanged.
- Reserved-name tests for scalar declarations, function params, and loop vars pass.
- Run `make test_eval && make test_repl_core_commit`.

### Step 3 - Support Scratch Reads In Expressions
Files: `repl_eval.h`, `repl_eval.c`.

Extend `ExprCtx` with optional error fields while keeping existing initializers valid:

```c
char *err;
int   err_sz;
```

In `eval_primary()`:
- After reading an identifier, check for `[` before scalar/function lookup.
- If the name is `A/B/C`, parse the bracket expression with `repl_eval_expr`, require closing `]`, bounds-check, then read the scratch element.
- If a non-scratch identifier is followed by `[`, report `unknown array '<name>'`.
- If `A/B/C` appears without `[`, report `scratch array 'A' requires an index`.

Update `repl_eval_validate_expression_idents()`:
- Treat `A[expr]`, `B[expr]`, `C[expr]` as valid after recursively validating the index expression.
- Reject bare `A/B/C`.
- Reject unknown array syntax like `x[0]`.

Update `repl_eval_input_has_predef_vars()` or replace it with a better-named helper used by callers, so scratch reads force runtime re-evaluation like scalar predefined vars do today.

Verification:
- `A[0]`, `A[1 + 1]`, `A[i]` evaluate correctly.
- `A[8]`, `A[-1]`, `A`, and `x[0]` fail in validation/checked contexts.
- GL commands using `A[0]` are marked `has_vars`.
- Run `make test_eval && make test_repl_core_parse`.

### Step 4 - Add `CMD_SCRATCH_ASSIGN` And Compile/Apply Ops
Files: `repl_command.h`, `repl_compile.h`, `repl_compile.c`, `repl_apply.c`, `editor_services.*`, `editor_commit.c`.

Add command type:

```c
CMD_SCRATCH_ASSIGN
```

Encoding:
- `args[0]`: array index `0..2`
- `args[1]`: last resolved element index
- `args[2]`: last resolved value
- `num_args = 3`
- `has_vars = 1` when either index or RHS references runtime values

Add scratch side effects to `ReplCompiledChange`:

```c
typedef struct {
    int array_idx;
    int elem_idx;
    float value;
} ReplScratchOp;
```

Add `scratch_ops[]` and `scratch_op_count`, plus `repl_apply_scratch_ops()`.

Update commit orchestration:
- Add `apply_scratch_ops` to `EditorServices`.
- Call it after `apply_predef_ops()` and before editor/repl command-store mutation in both normal and external commit paths.
- Keep preflight first, so failed command-store capacity does not mutate scratch state.

Update assignment parsing:
- Add a new target parser such as `repl_extract_assignment_target_parts()`.
- Keep `repl_extract_assignment_parts()` as the scalar-only compatibility wrapper.
- `A[index] = rhs;` compiles to `CMD_SCRATCH_ASSIGN`.
- Scalar assignment behavior remains unchanged.

Verification:
- `A[0] = 1;` commits, updates live scratch state, and stores normalized text.
- `A[i] = A[i] + 1;` commits inside loop/function scope.
- Scalar `x = 1;` tests remain unchanged.
- Run `make test_repl_core_commit && make test`.

### Step 5 - Wire Flatten And Execution
Files: `repl_flatten.c`, `repl_executor.c`, replay annotation evaluator call sites.

In `flatten_range()`:
- Handle `CMD_SCRATCH_ASSIGN` similarly to `CMD_VAR_ASSIGN`.
- Re-evaluate index and RHS from source text using current local vars.
- Set the scratch array element immediately.
- Append a flat `CMD_SCRATCH_ASSIGN` with resolved index/value and captured local vars.

In `repl_execute_program()`:
- Handle `CMD_SCRATCH_ASSIGN` so goto/replay execution re-applies scratch writes.
- Re-evaluate index/RHS when `has_vars` is set, using flat local vars exactly like scalar assignment.

Verification:
- Add recursive Bezier test using `A/B`, `lerp`, `for(u...)`, and recursive `func0(count)`.
- Confirm recursive calls see scratch writes from earlier frames.
- Confirm multiple `u` iterations reload control points and produce stable output.
- Run `make test_repl_core_commit && make test_repl_replay && make test`.

### Step 6 - Preserve Scratch State Across Snapshots
Files: `editor_undo.*`, `repl_scenes.c`, `replay.c`, `repl_replay_annotations.c`, `imrepl_ctrl.c`.

Wherever predefined values are copied/restored today, copy/restore scratch arrays too:
- Undo/redo snapshots.
- User scene capture/restore/stash.
- Replay baseline and live-value stashes.
- Replay annotation per-source snapshots.
- Controller temporary state around frame/replay operations.

Do not copy scratch arrays through `MAX_PREDEF_VARS`; use the dedicated scratch copy/restore helpers.

Verification:
- Undo after `A[0] = 3;` restores the previous `A[0]`.
- Scene switching preserves independent scratch state.
- Replay annotations for commands using `A[i]` use the scratch state before that command.
- Run `make test_repl_replay && make test_repl_editor && make test`.

### Step 7 - Export/Import Round Trip
Files: `repl_export.c`, IO tests.

Export:
- Emit global scratch arrays in generated C:

```c
static float A[8] = {0};
static float B[8] = {0};
static float C[8] = {0};
```

- Translate `lerp(a,b,t)` to either a generated helper or inline expression.
- Translate scratch assignment source as normal C assignment.
- Translate scratch reads in expressions to C array reads.

Import:
- Recognize `A[...] = ...;`, `B[...] = ...;`, `C[...] = ...;` as scratch assignments.
- Ignore generated scratch global declarations on import; they are runtime scaffolding, not REPL source commands.

Verification:
- Export/import a Bezier scratch-array program and compare command text.
- Generated C compiles in normal and stub builds.
- Run `make test_repl_core_io && make sample USE_GL_STUBS=1`.

### Step 8 - UI, Completion, Docs, Final Gate
Files: `editor_autocomplete.c`, `MODULES.md`, `AGENTS.md` or feature docs.

- Add optional completions for `A[`, `B[`, `C[`, and `lerp(`.
- Do not add variable-panel editing in v1.
- Document scratch arrays as REPL language runtime state, not UI state and not predefined scalar variables.
- Add a small example snippet to docs or examples if the repo has an appropriate example fixture.

Final verification:
- `make test_eval`
- `make test_repl_core_parse`
- `make test_repl_core_commit`
- `make test_repl_core_io`
- `make test_repl_replay`
- `make test`
- `make test-stubs`
- `make check-state-ownership`

## Acceptance Criteria
- Recursive Bezier example works with no scalar explosion.
- `A/B/C[8]` reads and writes work inside loops, functions, and recursive calls.
- Existing scalar variables, variable panel updates, replay, export/import, undo, and scene switching continue to pass existing tests.
- No generic array syntax is accepted beyond predefined `A/B/C[index]`.
- The implementation does not consume `MAX_PREDEF_VARS` slots.

## Defaults Chosen
- Arrays are fixed at three arrays of eight floats.
- Arrays are global scratch memory, not lexical locals.
- Index conversion is `(int)` truncation.
- Variable-panel array editing is deferred.
- Variable-length function calls are deferred because they do not provide mutable indexed storage across recursion.

