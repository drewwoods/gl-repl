## Bounded Global Arrays and Deterministic `rand()` for the Immediate-Mode REPL

## Status - NOT STARTED (2026-05-23 audit)

No `CMD_ARRAY_DEF` / `CMD_ARRAY_SET` in the command model; no
`MAX_REPL_ARRAYS` / `MAX_REPL_ARRAY_LEN` constants; no
`array name(size);` syntax. A different, smaller feature shipped
instead - the fixed scratch arrays `A[8]`/`B[8]`/`C[8]` (see CLAUDE.md
"Math"), backed by `CMD_SCRATCH_ASSIGN`. That covers part of the
user-particle-loop motivation with much less surface area.

`rand()` also exists today but in a different shape than this plan
proposed: it is `rand(seed[, iter])` / `rand2(...)` (deterministic per
seed/iter), not the zero-arg + session-seeded form here. Any future
implementation of this plan would need to reconcile the two designs.

### Summary
Add first-class bounded global float arrays plus a deterministic `rand()` function to `src/immediate-mode-repl/claude4.6-opus-thinking` so particle-style state can be authored directly in REPL code without unbounded memory or hidden allocation.

V1 behavior:
- Up to **16** named arrays.
- Each array holds up to **4096** `float` elements.
- Arrays are **global** and **persist across frames**.
- User syntax:
  - Declare: `array posx(4096);`
  - Read in expressions: `posx[i]`
  - Write statement: `posx[i] = expr;`
- Add `rand()` as a **zero-argument** expression function returning a float in **[0.0, 1.0)**.
- `rand()` is **deterministic per session** with fixed default seed **`1u`** on reset/load/export.

### Key Changes
#### Language and command model
- Add new command types:
  - `CMD_ARRAY_DEF` for `array name(size);`
  - `CMD_ARRAY_SET` for `name[idx] = expr;`
- Keep arrays bounded and explicit:
  - names must be valid C identifiers,
  - name length capped at **15 chars + NUL**,
  - names may not collide with predefined scalars (`x`, `y`, `z`, `i`, `j`, `k`, `n`, `t`) or `func0..func9`,
  - declarations are **top-level only** and must appear before first use,
  - declaration size must resolve to an integer in `[1, 4096]`,
  - redeclaring an existing array recreates it and zeroes contents.
- Add expression support in `repl_eval.*` for:
  - `ident[expr]` array reads,
  - `rand()` zero-arg calls.
- Add one explicit “runtime dynamic” flag on `GLCmd` for commands whose numeric args cannot safely stay baked after flattening. Mark it for:
  - `CMD_ARRAY_SET`,
  - any command whose preserved source directly contains array reads,
  - any command whose preserved source directly contains `rand()`.

#### Runtime, flattening, and execution
- Add a bounded array registry plus RNG state owned by the REPL runtime:
  - 16 array slots with names, sizes, and `float data[4096]`,
  - RNG state stored as `uint32_t g_rand_state`,
  - RNG algorithm fixed to **xorshift32** so REPL and exported C match exactly,
  - `rand()` maps to the next xorshift32 output using the high 24 bits divided by `16777216.0f`.
- Array index semantics:
  - index expression evaluated as float,
  - converted with C-style `(int)` truncation,
  - out-of-range reads return `0.0f`,
  - out-of-range writes do nothing.
- Execution model:
  - `CMD_ARRAY_SET` executes at render/replay time and mutates persistent array storage.
  - Commands marked runtime-dynamic are re-parsed from preserved source at execute time using:
    - current predefined scalar values,
    - the flat command’s local-var snapshot,
    - current array contents,
    - current RNG state.
  - This dynamic path is what makes particle loops work:
    - `posx[i] = posx[i] + velx[i];`
    - `glVertex3f(posx[i], posy[i], 0);`
- Keep structural flattening unchanged for `for`, `if`, and `func`.
- Do **not** make scalar intermediary propagation more general in v1:
  - direct array reads and direct `rand()` calls in command expressions are supported,
  - patterns like `x = posx[i]; glVertex3f(x, ...)` keep current scalar semantics and are not guaranteed to stay reactive frame-to-frame.

#### Export, import, replay, and state snapshots
- Export C with the same user array names:
  - emit file-scope declarations like `static float posx[4096];`,
  - skip `CMD_ARRAY_DEF` from the snippet body because declarations are emitted once above `display()`,
  - emit `CMD_ARRAY_SET` and array reads using normal `name[idx]` syntax.
- Export deterministic RNG helpers above `display()`:
  - `static uint32_t g_rand_state = 1u;`
  - `static float repl_rand01(void) { ... }`
  - `repl_expr_to_c` translates `rand()` to `repl_rand01()`,
  - `c_expr_to_repl` translates `repl_rand01()` back to `rand()`.
- Import support:
  - recognize exported `static float name[SIZE];` declarations and reconstruct `array name(SIZE);`,
  - continue importing array writes through normal snippet parsing,
  - do not import RNG helper definitions; they are exporter-owned boilerplate.
- Snapshot arrays and RNG anywhere the renderer already snapshots predefined vars:
  - undo/redo,
  - replay baseline,
  - accumulation-buffer multi-pass rendering,
  - replay fade re-render passes.
- Replay behavior:
  - replay start captures baseline predefined scalars, array contents, and RNG state,
  - each replay render pass restores all three before executing the current flat prefix,
  - this keeps `rand()` and array mutation deterministic instead of double-advancing across preview frames.

### Important API / Type Additions
- In `repl_eval.h`:
  - add bounded array constants (`MAX_REPL_ARRAYS = 16`, `MAX_REPL_ARRAY_LEN = 4096`, name length constant),
  - add shared array-registry and RNG helper declarations used by both evaluator and REPL core,
  - add expression-inspection helpers to detect direct array access / `rand()` usage for command flags.
- In `sample.h` / `GLCmd`:
  - add `CMD_ARRAY_DEF` and `CMD_ARRAY_SET`,
  - add one `needs_runtime_eval`-style flag distinct from current `has_vars`.
- In runtime snapshot structs:
  - extend undo/replay snapshot state to include full array contents and RNG state.

### Test Plan
- Evaluator tests:
  - `rand()` returns values in `[0.0, 1.0)`,
  - deterministic `rand()` sequence after `repl_reset_state`,
  - `posx[i]` reads with constant, loop-var, and function-param indices,
  - out-of-range array reads return `0.0f`.
- Parser / commit tests:
  - `array posx(128);` parses and allocates,
  - `posx[i] = expr;` parses as array write,
  - direct array-read draw commands are marked runtime-dynamic,
  - direct `rand()` draw commands are marked runtime-dynamic,
  - reject undeclared arrays, invalid names, top-level collisions, declaration inside block, size `<= 0`, size `> 4096`, and 17th array.
- Execution / behavior tests:
  - particle-style loop updates arrays and later draw commands in the same frame see updated values,
  - `rand()` in array writes produces deterministic per-session sequences,
  - replay prefix restores array and RNG baseline correctly,
  - accumulation-buffer multi-pass rendering does not consume extra random numbers or double-advance arrays.
- Export / import tests:
  - export emits `static float name[size];` with original names,
  - export emits deterministic RNG helper and translates `rand()` to `repl_rand01()`,
  - import reconstructs array declarations from exported C,
  - exported array/rand snippets round-trip back into REPL form.

### Assumptions and Defaults
- V1 is **1D float arrays only**.
- Arrays are **global only**; no local arrays and no array parameters.
- No v1 array inspector, slider UI, resize API, free API, or seed command.
- `rand()` has no arguments and uses fixed seed **`1u`** on reset/load/export.
- `goto` remains out of scope for array-side-effect correctness; array support is aimed at `for`/`if`/`func` particle-style code.
- Users should read arrays directly in render expressions; scalar intermediary patterns keep current REPL semantics in v1.

