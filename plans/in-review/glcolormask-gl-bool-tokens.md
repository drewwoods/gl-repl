# glColorMask: preserve GL_TRUE/GL_FALSE via generalized enum args

Status: **design chosen; not implemented**. A full prototype (incl. a
std-path special-case) was built and then **rolled back** at the user's
request; only the unrelated alphabetical sort of `command_spec.c` was
kept. This file captures the design so the work can be picked up later.

Chosen path: **C — generalize enum parsing to N args**. This is larger
than the std-path workaround, but it is the right hygiene move: boolean
GL tokens should not be parsed through one command path for
`glDepthMask` and a different expression/text-repair path for
`glColorMask`.

Do not implement until this file moves to `not-started/`.

## Context

Ask: `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)` must round-trip
and display with the symbolic tokens. Numeric `1, 0` "breaks the
illusion" of writing real GL.

`glColorMask` takes **4 GLboolean args**. The codebase has two arg
models:

- **Enum path** (`k_enum_command_specs[]`, `src/repl/parser.c:215`):
  literal token → table lookup, echoes the token name back into the
  canonical source text. *Not generalized* — hand-written
  `num_args==1` (→ `cmd->mode`) and `num_args==2` (→ `mode` +
  `args[0]`) branches with **divergent storage conventions** the
  executor reads per-command. Struct carries only `enums1`/`enums2`.
- **Std path** (`k_std_command_specs[]`, `src/repl/parser.c:326`):
  expression eval into `args[]`, text formatted with `%g` → numeric.

Neither fits cleanly: the std path flattens to `1/0`; the enum path
has no N-arg shape and would need a third bespoke branch + a third
storage convention for one command.

## Current state — rolled back; nothing implemented

The prototype touched `command.h` (`CMD_COLOR_MASK`),
`command_spec.c` (spec row, func-completion, type-spec),
`executor.c` (dispatch + `apply_state_cmd`), `parser.c`
(begin-error name + a post-parse text special-case),
`eval.c` (`GL_TRUE`/`GL_FALSE` as evaluator constants),
`tests/test_repl_export_all_commands.c`, and CLAUDE.md. **All of it
was reverted.**

**Kept (separate standing request, not part of this fork):**
`k_enum_command_specs[]` and `k_std_command_specs[]` in
`command_spec.c` are now alphabetized by GL name. That ordering stays
regardless of whether glColorMask is ever built.

A useful sub-finding from the prototype, reusable if this resumes:
`GL_TRUE`/`GL_FALSE` can be added as evaluator constants in `eval.c`
(4 sites, mirroring `PI`/`TAU`; **no** C round-trip map entry — the
tokens are byte-identical valid literals in both the REPL and exported
C). That alone makes the std path accept the symbolic tokens; only the
*display-as-`1/0`* problem then remains, which is what the fork below
is about.

## Scope fork — where glColorMask lives

### A. Std path + post-parse text special-case

Keep the implemented approach. Accept value-changing parse, override
only text emission.

- Pro: smallest diff; reuses the entire std pipeline (expression
  parsing, the new `GL_TRUE`/`GL_FALSE` constants, validation, arg
  storage); accepts `GL_TRUE`/`GL_FALSE` **and** `1/0`/expressions and
  normalizes display to symbolic. The one wart
  (`if (def->type == …)` in a generic loop) is **not novel** — it is
  the established `glClearColor` idiom.
- Con: a second type-special-case accreting on the std loop; carries a
  numeric `fmt` that is written then overwritten (same as
  `glClearColor`).

### B. Add a `num_args==4` enum branch

Bespoke 4-token branch in the enum path.

- Pro: glColorMask sits in the semantically "enum" home; text
  preservation is intrinsic, not patched.
- Con: net-new parsing loop **plus** a third storage convention
  (`args[0..3]`) that reuses neither existing branch; new struct
  fields (`enums3/4`) used by one command; enum path rejects `1/0`
  and expressions. More code than A, in the wrong-to-grow place.

### C. Generalize the enum path to N args (uniform `args[]`) — *chosen*

The clean home: one N-arg enum branch, uniform storage, and no
`glColorMask`-specific source-text repair. `glDepthMask(flag)` and
`glColorMask(red, green, blue, alpha)` both stay in the same semantic
model: symbolic GL tokens are parsed as tokens and emitted back as
tokens.

- Pro: removes the mode/args[0] divergence wart; correct long-term
  model. Keeps `GL_TRUE`/`GL_FALSE` handling coherent across depth and
  color masks. Future multi-enum or multi-bool commands become table
  rows, not parser forks.
- Con: largest blast radius — changes the storage contract and
  executor reads for **every** existing enum command (glEnable,
  glDisable, glShadeModel, glFrontFace, glDepthMask, glColorMaterial,
  glBlendFunc, glLightModeli, glBegin) for the benefit of one new
  command. Higher regression surface; must be implemented as a focused
  enum-path refactor before adding `glColorMask`.

#### C design details

Goal: `ReplEnumCommandSpec` describes N positional enum arguments, and
the parser handles all N through one loop. Parsed enum values live in a
single storage convention so the executor no longer has to remember
that one-arg enum commands use `cmd->mode` while two-arg enum commands
use `cmd->mode + cmd->args[0]`.

Hard constraint: enum tables store **real GL constant values**, not
project-local aliases or dense ordinal IDs. The REPL source should stay
copy/paste-shaped like GL code: `GL_TRUE` means the platform's
`GL_TRUE`, `GL_DEPTH_TEST` means the platform's `GL_DEPTH_TEST`, and a
numeric fallback succeeds only by matching one of those real values in
the expected enum table.

Recommended storage contract:

- Store every parsed enum argument in `cmd->args[arg_idx]` as a float
  value, even for one-arg commands.
- Set `cmd->num_args = def->num_args`.
- Stop using `cmd->mode` for enum-spec command arguments after the
  refactor. `mode` can remain in `GLCmd` for compatibility with
  non-refactored/custom paths during the patch, but enum-spec executor
  reads should move to `args[]`.
- Emit canonical source text from the matched enum token names, not
  from numeric values.

Spec shape:

- Replace `enums1` / `enums2` with an array of enum-table pointers,
  e.g. `const ReplEnumEntry *enums[MAX_ENUM_ARGS]`.
- Add `MAX_ENUM_ARGS` at the command-spec layer. `4` is enough for
  `glColorMask`, but `8` is a better match for `GLCmd.args[8]` and
  avoids another shape change later.
- Keep one `usage[MAX_ENUM_ARGS]` or equivalent per-slot diagnostic
  string.
- Keep `fmt` only if it still buys enough for display. A cleaner option
  is to canonicalize generically as:
  `indent + name + "(" + joined matched token names + ");"`.
  `glBegin` can still use the existing `indent_type` hook for its
  special begin-block indentation.

Parser shape:

- Replace the `num_args == 1` and `num_args == 2` branches in
  `src/repl/parser.c` with one loop:
  1. Split `args` into exactly `def->num_args` top-level arguments.
  2. Trim each token.
  3. Resolve each token with one enum-arg resolver.
  4. Store each resolved value in `cmd->args[arg_idx]`.
  5. Emit canonical text using the resolver's canonical enum names.
- Add a small helper shaped roughly like:
  `resolve_enum_arg(raw_arg, enum_table, vars, num_vars, out_value,
  out_name, err, err_sz)`. It should:
  1. Try exact enum-token lookup first.
  2. If that fails, pre-parse/evaluate the raw arg as a constant-only
     expression.
  3. Convert the folded value back to an enum by finding an exact value
     match in the expected enum table.
  4. Return both the numeric enum value and the canonical enum token
     name.
  This keeps numeric fallback generic and table-driven instead of
  special-casing bools or `glColorMask`.
- Do not use raw `strchr(args, ',')` for the generalized splitter.
  Use the same paren-aware delimiter helper pattern as expression-list
  parsing (`repl_scan_next_arg_delim()` / existing top-level arg split
  helpers) so the enum path does not preserve today’s comma-splitting
  limitation.
- Preserve the `CMD_LIGHT_MODEL_I` exception only if needed:
  `glLightModeli(pname, param)` currently allows the second argument to
  be either a bool token or an integer/expression. If that compatibility
  is still desired, model the second slot as "enum token with expression
  fallback" rather than keeping a bespoke two-arg branch.

Expression and numeric compatibility:

- Accept symbolic enum tokens first. The primary path is always token
  lookup: `GL_TRUE` maps to the real `GL_TRUE` value, `GL_DEPTH_TEST`
  maps to the real `GL_DEPTH_TEST` value, etc.
- Also accept numeric constant values as a deliberate fallback. If token
  lookup fails, parse the slot as a constant expression with no visible
  runtime vars. Then reverse-map the folded numeric value through that
  slot's enum table. This makes `glColorMask(1, 0, 1, 0)` canonicalize
  to `glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);` because
  `GL_TRUE == 1` and `GL_FALSE == 0`.
- Numeric fallback should only succeed when the folded value exactly
  matches an enum value in that argument's table. That permits real GL
  enum values such as the numeric value for `GL_DEPTH_TEST`, but still
  canonicalizes them back to symbolic source and rejects unknown magic
  numbers.
- Do **not** accept runtime-variable expressions for `glColorMask` in
  the enum path unless the storage/text model is extended to preserve
  expression source per arg. Otherwise flatten/replay/export semantics
  will silently collapse the value at commit time.

Executor update:

- `CMD_BEGIN`: `glBegin((GLenum)cmd->args[0])`
- `CMD_ENABLE` / `CMD_DISABLE`: `glEnable((GLenum)cmd->args[0])`,
  `glDisable((GLenum)cmd->args[0])`; light tracking reads the same slot.
- `CMD_SHADE_MODEL`: `glShadeModel((GLenum)cmd->args[0])`
- `CMD_FRONT_FACE`: `glFrontFace((GLenum)cmd->args[0])`
- `CMD_DEPTH_MASK`: `glDepthMask((GLboolean)cmd->args[0])`
- `CMD_COLOR_MATERIAL`: `glColorMaterial((GLenum)cmd->args[0],
  (GLenum)cmd->args[1])`
- `CMD_BLEND_FUNC`: `glBlendFunc((GLenum)cmd->args[0],
  (GLenum)cmd->args[1])`
- `CMD_LIGHT_MODEL_I`: `glLightModeli((GLenum)cmd->args[0],
  (GLint)cmd->args[1])`
- `CMD_COLOR_MASK`: `glColorMask((GLboolean)cmd->args[0],
  (GLboolean)cmd->args[1], (GLboolean)cmd->args[2],
  (GLboolean)cmd->args[3])`

Migration safety:

- Update any non-executor reads of enum command values. Known examples:
  autonormal front-face tracking currently reads `CMD_FRONT_FACE.mode`;
  block mode handling reads `CMD_BEGIN.mode`; tests assert
  `CMD_DEPTH_MASK.mode`.
- Add a drift/contract test that every `k_enum_command_specs[]` row with
  `num_args > 0` produces `cmd.num_args == num_args` and fills
  `args[0..num_args-1]`.
- Add parser tests for existing enum commands before `glColorMask` so
  the refactor is proven independent of the new command.

## Recommendation

**Fork C is chosen.** The std-path workaround is smaller, but it splits
boolean GL token handling across two command models: `glDepthMask`
would remain enum-token parsed while `glColorMask` would be
expression-parsed and repaired after the fact. That is not worth the
long-term inconsistency.

Do the enum-path cleanup first, then add `glColorMask` as a normal
4-argument bool-token enum command.

## If approved (implement C from scratch)

1. `command_spec.h`: replace `ReplEnumCommandSpec.enums1/enums2` with
   positional enum tables (`enums[MAX_ENUM_ARGS]`) and positional usage
   strings. Keep enough compatibility helpers/macros locally to make
   the spec table readable.
2. `command_spec.c`: migrate all existing enum rows to the new shape.
   Add the `glColorMask` func-completion row and enum-spec row using
   `k_bool_vals` for all four slots. Add
   `CMD_TYPE_SPEC_NOT_IN_BEGIN(CMD_COLOR_MASK, 1, 1, CMD_CAT_STATE)`.
3. `parser.c`: replace the one-arg/two-arg enum branches with one
   generalized N-arg parser, canonical text emitter, and diagnostics.
   Add `CMD_COLOR_MASK` to the begin-scope error display switch.
4. `executor.c`: update every enum command dispatch to read from
   `cmd->args[]`; add `CMD_COLOR_MASK` in `apply_state_cmd` and the
   main dispatch grouping.
5. Other source reads: update `CMD_BEGIN` / `CMD_FRONT_FACE` /
   `CMD_DEPTH_MASK` / other enum users outside executor to read the
   new `args[]` contract instead of `mode`.
6. Tests:
   - Existing enum commands still parse, canonicalize, execute, export,
     and reject invalid tokens.
   - `glDepthMask(GL_TRUE)` and
     `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)` both round-trip
     symbolic bool tokens.
   - `expected_commands[]` and the all-commands export test include
     `CMD_COLOR_MASK`.
   - Contract test: enum-spec rows fill `args[]` uniformly and set
     `num_args`.
   - If numeric bool fallback is intentionally added, test
     `glColorMask(1, 0, 1, 0)` canonicalizes to symbolic tokens. If it
     is intentionally rejected, test the rejection message instead.
7. Docs: CLAUDE.md supported-commands list. Note that enum commands use
   one uniform `args[]` storage convention.
8. Gate: `make test`, `make test-stubs`,
   `make sample USE_GL_STUBS=1`, `make sample`,
   `make check-state-ownership`.

## Folder note

`plans/in-review/` = decision pending. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
The prototype was rolled back, so an approve-A decision means
**implement from scratch** per "If approved" (move to `not-started/`
→ `active/`), not a quick finish.
