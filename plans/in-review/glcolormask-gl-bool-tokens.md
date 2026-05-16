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

The previous design fork — how a slot declares "expression fallback"
(`glLightModeli` arg2) — is resolved in "Slot-kind
representation": explicit per-slot kind metadata, not a `NULL`
sentinel. The plan is implementation-ready pending review.

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

Storage contract (atomic — all or nothing):

- Store every parsed enum argument in `cmd->args[arg_idx]` as a float
  value, even for one-arg commands.
- Set `cmd->num_args = def->num_args`.
- **No transitional dual-write.** `cmd->mode` is removed as the enum-arg
  carrier for table-driven `ReplEnumCommandSpec` commands in the same
  change: every `GLCmd.mode` reader for those commands moves to
  `args[]` in one commit. Do not ship a window where some readers use
  `mode` and others use `args[]` — a half-migrated reader diverges
  silently (pitfall 8). The golden snapshot below makes a big-bang
  refactor safe to verify, so the dual-write window buys nothing and
  only adds a drift surface.
- Custom enum+float commands are an explicit scope boundary:
  `CMD_MATERIALF` and `CMD_POINT_PARAMETER_FV` currently use
  `GLCmd.mode` through custom parser branches because they mix enum
  slots with float payloads. Either migrate those commands deliberately
  to `args[]` in the same refactor, or leave them as the only
  documented legitimate `GLCmd.mode` users. Do not delete `mode` unless
  those custom users are migrated too.
- Emit enum-value slots from the matched enum token names, not from
  numeric values. Enum-or-expression fallback slots preserve the typed
  expression text when no enum token matched.

Spec shape:

- Replace `enums1` / `enums2` with an array of positional arg specs,
  e.g. `ReplEnumArgSpec args[MAX_ENUM_ARGS]`.
- Add `MAX_ENUM_ARGS` at the command-spec layer. `4` is enough for
  `glColorMask`, but `8` is a better match for `GLCmd.args[8]` and
  avoids another shape change later.
- Each `ReplEnumArgSpec` carries the enum table, the slot diagnostic,
  and the slot kind:

  ```c
  typedef enum {
      REPL_ENUM_SLOT_ENUM_VALUE = 0,   /* token or constant numeric value */
      REPL_ENUM_SLOT_ENUM_OR_EXPR      /* token first, else full expression */
  } ReplEnumSlotKind;

  typedef struct {
      const ReplEnumEntry *enums;
      const char *usage;
      ReplEnumSlotKind kind;
  } ReplEnumArgSpec;
  ```

  `enums` remains the source of truth for real GL token values and
  autocomplete. Both slot kinds used by this plan have a real enum
  table; if a future command needs a true expression-only slot, add an
  explicit slot kind for it instead of overloading a null table pointer.
  The parser must branch on `kind`, not on whether `enums` is null.
- Keep `fmt` only if it still buys enough for display. A cleaner option
  is to canonicalize generically as:
  `indent + name + "(" + joined matched token names + ");"`.
  `glBegin` can still use the existing `indent_type` hook for its
  special begin-block indentation.
- Metadata-only/custom rows must be handled deliberately.
  `CMD_MATERIALF` currently has a negative `num_args` enum-spec row for
  metadata/completion while parsing is handled by a custom branch. Path
  C must either move that metadata out of `repl_enum_command_specs()`
  or mark custom rows so the generalized enum parser skips them. Do
  not run the N-arg parser over negative/custom rows.
- **Expression-fallback slot semantics**:
  - Try enum-token lookup first when `args[slot].enums` is present.
    This preserves `GL_TRUE` / `GL_FALSE` spelling and keeps
    autocomplete table-driven.
  - If no token matches and `kind == REPL_ENUM_SLOT_ENUM_OR_EXPR`,
    evaluate the raw arg once through the normal expression evaluator
    (vars in scope permitted), store the folded float in
    `cmd->args[slot]`, and **emit the original typed source token for
    that slot verbatim** — do not canonicalize it to a value or token
    name.
  - The current low-level parser branch for `CMD_LIGHT_MODEL_I` already
    preserves the typed fallback text (`trimmed2`) but does not set
    `cmd->has_vars` itself. Having the generalized parser set
    `has_vars` when an expression-fallback slot references visible vars
    is an intentional tightening of the parser contract so callers do
    not have to infer it later.
  `REPL_ENUM_SLOT_ENUM_VALUE` slots, by contrast, are constant-only and
  canonicalize to the table's token name.

Parser shape:

- Replace the `num_args == 1` and `num_args == 2` branches in
  `src/repl/parser.c` with one loop:
  1. Split `args` into exactly `def->num_args` top-level arguments.
  2. Trim each token.
  3. Resolve each token with one enum-arg resolver.
  4. Store each resolved value in `cmd->args[arg_idx]`.
  5. Emit text using the resolver's per-slot emitted arg text:
     canonical enum names for enum-value slots, and original expression
     text for enum-or-expression fallback misses.
- The per-slot resolver branches on `args[slot].kind`:
  - **Enum-value slot** (`REPL_ENUM_SLOT_ENUM_VALUE`) —
    `resolve_enum_arg(raw, args[slot].enums, vars, num_vars,
    out_value, out_name, err)`:
    1. Try exact enum-token lookup first.
    2. If that fails, evaluate the raw arg as a **constant-only**
       expression (reject visible vars — enum-value slots never
       animate).
    3. Reverse-map the folded value to a token by exact value match in
       `args[slot].enums`; fail with the slot's usage if no match.
    4. Return the numeric value **and** the canonical token name; the
       canonical name is what gets emitted.
  - **Enum-or-expression slot** (`REPL_ENUM_SLOT_ENUM_OR_EXPR`): try
    exact enum-token lookup first if an enum table is present; if that
    fails, evaluate as a full expression (vars permitted), store the
    folded float, set `has_vars` accordingly, and carry the original
    trimmed source token through to the emitter unchanged.
  This keeps numeric fallback generic and table-driven instead of
  special-casing bools or `glColorMask`, while preserving
  `glLightModeli`'s expression arg without a bespoke command branch.
- Do not use raw `strchr(args, ',')` for the generalized splitter.
  Use the same paren-aware delimiter helper pattern as expression-list
  parsing (`repl_scan_next_arg_delim()` / existing top-level arg split
  helpers) so the enum path does not preserve today’s comma-splitting
  limitation.
- `CMD_LIGHT_MODEL_I` is the concrete consumer of the slot-kind model:
  spec it as `num_args==2`, slot 0 =
  `{ k_light_model_params, ..., REPL_ENUM_SLOT_ENUM_VALUE }`, slot 1 =
  `{ k_bool_vals, ..., REPL_ENUM_SLOT_ENUM_OR_EXPR }`. This reproduces
  today's "pname is a token, param is a bool token *or* an
  int/expression" behavior with **no** bespoke command branch — it falls
  out of the generalized resolver. This compatibility is a hard
  requirement, not optional: it must have a dedicated test (`GL_TRUE`,
  `1`, `0`, and a non-table integer / a var expression) written
  *before* the refactor lands.

Completion shape:

- `src/app/glr_completion.c` is part of the enum-spec refactor, not a
  later polish item. It currently has `AC_MODE_ENUM_ARG1` /
  `AC_MODE_ENUM_ARG2` modes and reads `enums1` / `enums2` directly.
- Replace those with a slot-indexed enum completion path:
  - Track the active enum slot (`g_ac_enum_slot` or equivalent) instead
    of encoding slot 1 vs slot 2 in the mode name.
  - Select `def->args[slot].enums` for end-of-input enum completion.
  - Derive suffix from `slot + 1 == def->num_args`: `")"` for the last
    enum arg, otherwise `", "`.
  - Add `glColorMask` coverage so all four bool slots complete
    `GL_TRUE` / `GL_FALSE`.
- This is separate from cursor-aware mid-line completion. Path C only
  needs the existing end-of-input completion behavior to work with N
  enum slots; `plans/in-review/cursor-aware-enum-arg-completion.md`
  owns the larger cursor/ghost/accept splice behavior.
- **Cross-plan staleness (bidirectional).** Both plans cite
  `glr_completion.c` by line number against the *current*
  `enums1`/`enums2` + `AC_MODE_ENUM_ARG1/2` shape. Whichever lands
  first invalidates the other's citations. Whoever picks up the second
  plan must re-derive the `glr_completion.c` references from the
  then-current code and not trust the line numbers in either file.

Expression and numeric compatibility:

- Accept symbolic enum tokens first. The primary path is always token
  lookup: `GL_TRUE` maps to the real `GL_TRUE` value, `GL_DEPTH_TEST`
  maps to the real `GL_DEPTH_TEST` value, etc.
- For enum-value slots, also accept numeric constant values as a
  deliberate fallback. If token lookup fails, parse the slot as a
  constant expression with no visible runtime vars. Then reverse-map the
  folded numeric value through that slot's enum table. This makes
  `glColorMask(1, 0, 1, 0)` canonicalize to
  `glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);` because
  `GL_TRUE == 1` and `GL_FALSE == 0`.
- Enum-value numeric fallback should only succeed when the folded value
  exactly matches an enum value in that argument's table. That permits
  real GL enum values such as the numeric value for `GL_DEPTH_TEST`, but
  still canonicalizes them back to symbolic source and rejects unknown
  magic numbers.
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
  core block-mode helpers and controller outline/overlay passes read
  `CMD_BEGIN.mode`; flat lighting detection reads `CMD_ENABLE.mode` /
  `CMD_DISABLE.mode`; tests assert `CMD_DEPTH_MASK.mode`.
- Add a drift/contract test that every `k_enum_command_specs[]` row with
  `num_args > 0` produces `cmd.num_args == num_args` and fills
  `args[0..num_args-1]`.
- Add parser tests for existing enum commands before `glColorMask` so
  the refactor is proven independent of the new command.
- **Standing guard, not a one-time audit.** Pitfall 1 + the contract
  test catch *introduction-time* regressions; nothing stops a later
  patch from reintroducing `GLCmd.mode` for an enum-spec command. Add
  an enforced invariant in the existing `make check-state-ownership`
  infra (a new `check-*` target appended to the aggregator list,
  pattern-matching the other guards: a scoped `grep` that exits
  non-zero on violation). Preferred enforceable form, strongest first:
  1. **Delete `GLCmd.mode` entirely.** If the custom enum+float
     consumers (`CMD_MATERIALF`, `CMD_POINT_PARAMETER_FV`) are migrated
     to `args[]` in this refactor, the field has no users and the
     *compiler* enforces the invariant — no guard needed. This is the
     cleanest end state and the one to aim for.
  2. **If `mode` is retained** for those two documented custom
     commands only: the guard greps `$(REPL_SRCS)` for `\.mode`/
     `->mode` and fails on any hit outside an allowlist of the
     documented `MATERIALF`/`POINT_PARAMETER_FV` parser/executor
     sites — mirroring how `check-gl-boundaries` allowlists
     `executor.c`. Note `mode` is a generic field name; scope the
     grep to GLCmd-bearing files and pair it with the contract test
     so the structural guard and the behavioral test together pin it.

## Pitfalls (path C blast radius)

These are the reasons C was the *hesitated* choice. Each is a silent
regression — compiles and parses fine, fails only at render/round-trip.

1. **The `mode`-reader audit is grep-hostile.** `->mode` / `.mode`
   matches ~13 files, but most are unrelated `mode` fields (replay
   mode, UI/profile/config mode), not `GLCmd.mode`. A list-driven
   migration *will* miss a site. Do a **type-aware** audit: every read
   of `GLCmd.mode` specifically. Real candidates today:
   `src/repl/executor.c`, `autonormal.c`, `core.c`, `export.c`,
   `parser.c`, `flatten.c`, and controller/render helpers in
   `src/app/glr_ctrl.c` (+ `repl_begin_mode_name` callers in
   `core.c`). Treat this list as a starting grep, not the answer.
2. **`glBegin` primitive-mode is a state machine, not a value.** The
   executor keys vertex emission, begin/end bracketing, and overlay
   passes off the active begin mode. This is the hottest reader and the
   one least covered by parse tests — move it last and verify by
   rendering, not by asserting `args[0]`.
3. **`glFrontFace` winding feeds autonormal.** `autonormal.c` computes
   generated `glNormal3f` direction from `CMD_FRONT_FACE` winding. Miss
   this read and normals silently flip (lighting wrong, geometry
   "correct"). No parse/compile test catches it.
4. **`glLightModeli` arg2 is the semantic-regression hotspot.** Today
   slot 2 accepts a bool token **or an arbitrary int/expression**. A
   naive "token, else reverse-map into the enum table" resolver
   *narrows* this — `glLightModeli(pname, 2)` (or any value not in
   `k_bool_vals`) would start failing where it parsed before, and
   `glLightModeli(pname, 1)` would canonicalize to `GL_TRUE`
   unexpectedly. Slot 2 must be modeled explicitly as "enum token with
   expression fallback", and this exact compatibility must have a
   dedicated test (`GL_TRUE`, `1`, `0`, and a non-table integer if
   currently accepted) written *before* the refactor.
5. **Numeric reverse-map is header-dependent and intra-table
   ambiguous.** GL constant values differ between real GL headers and
   `tests/gl-stubs/`. Resolution is table-scoped (so cross-table value
   collisions like `GL_FALSE`/`GL_POINTS`/`GL_ZERO == 0` are harmless),
   but **aliased values within one table** make the canonical-name
   choice order-dependent. Mitigation: token lookup *always* wins
   (the token the user typed is preserved verbatim); numeric is
   fallback-only; and tests must feed **symbolic tokens**, never assert
   canonical text from numeric input under stubs unless stub enum
   values are pinned.
6. **Storing a `GLenum` in `float args[]` is correct but looks
   wrong.** All GL enums in use are < 2^24, so `float32` holds them
   exactly and `(GLenum)cmd->args[i]` round-trips losslessly. State
   this invariant in a comment at the storage site so a future
   maintainer doesn't "fix" it or assume it's lossy.
7. **`glBegin` indent must survive the generic emitter.** The
   `indent_type==1` begin-block indentation (`2 + 2*tess + 2*block`)
   and matching `glEnd` alignment are easy to drop when canonical text
   becomes a generic "join token names". Guard with a golden snapshot
   (below), not by eyeballing.
8. **Partial migration is its own hazard.** If `mode` and `args[]`
   were both live, a half-migrated reader would diverge silently.
   Decision: **no transition window** — the migration is atomic (see
   "Storage contract"). This pitfall is therefore designed out rather
   than mitigated; the only defense needed is the type-aware audit
   (pitfall 1) being exhaustive in the single migrating commit.
9. **Autocomplete is coupled to the old two-slot enum shape.**
   `glr_completion.c` reads `enums1` / `enums2` directly and uses
   `AC_MODE_ENUM_ARG1` / `AC_MODE_ENUM_ARG2`; a compile fix alone is not
   enough. The end-of-input completion path must become slot-indexed in
   the same commit as the spec shape change.

## Verification (beyond the functional list above)

- **Golden snapshot, behavior-neutral proof.** Before the refactor,
  capture every built-in example (`src/repl/examples.c`) rendered
  through `repl_dump_code_panel_text` *and* through `export.c`. After
  the refactor (still zero `glColorMask` code), assert **byte-identical**
  output. This is the cheapest high-coverage net for pitfalls 1, 2, 5,
  7.
- **Rendering-path coverage**, not just parse: scene/replay-walk tests
  exercising `glBegin` modes and a manual visual smoke (a lit model
  with `glFrontFace(GL_CW)` vs `GL_CCW`) for pitfalls 2 and 3.
- **Commit isolation.** Land the enum-path generalization as its own
  commit with the full pre-existing enum-command regression green and
  **no `CMD_COLOR_MASK` anywhere**. Add `glColorMask` only in a second
  commit. A bisect then cleanly separates "refactor broke an existing
  command" from "new command misbehaves".
- Run the standard gate (`make test`, `make test-stubs`,
  `make sample USE_GL_STUBS=1`, `make sample`,
  `make check-state-ownership`) on **both** commits independently.

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
   positional `ReplEnumArgSpec args[MAX_ENUM_ARGS]` entries carrying
   `enums`, `usage`, and explicit `ReplEnumSlotKind` metadata (see
   "Slot-kind representation"). Keep enough compatibility
   helpers/macros locally to make the spec table readable.
2. `command_spec.c`: migrate all existing enum rows to the new shape.
   Add the `glColorMask` func-completion row and enum-spec row using
   `k_bool_vals` for all four slots. Add
   `CMD_TYPE_SPEC_NOT_IN_BEGIN(CMD_COLOR_MASK, 1, 1, CMD_CAT_STATE)`.
3. `parser.c`: replace the one-arg/two-arg enum branches with one
   generalized N-arg parser, canonical text emitter, and diagnostics.
   Add `CMD_COLOR_MASK` to the begin-scope error display switch.
4. `src/app/glr_completion.c`: replace the `enums1`/`enums2` and
   `AC_MODE_ENUM_ARG1`/`AC_MODE_ENUM_ARG2` assumptions with a
   slot-indexed end-of-input enum completion path over
   `args[slot].enums`.
5. `executor.c`: update every enum command dispatch to read from
   `cmd->args[]`; add `CMD_COLOR_MASK` in `apply_state_cmd` and the
   main dispatch grouping.
6. Other source reads: update `CMD_BEGIN` / `CMD_FRONT_FACE` /
   `CMD_DEPTH_MASK` / other enum users outside executor to read the
   new `args[]` contract instead of `mode`, including controller
   outline/overlay paths.
7. Decide and implement the custom-command boundary:
   `CMD_MATERIALF` / `CMD_POINT_PARAMETER_FV` either remain documented
   `mode` users or move to `args[]` with executor/tests updated.
8. Tests:
   - Existing enum commands still parse, canonicalize, execute, export,
     and reject invalid tokens.
   - `glDepthMask(GL_TRUE)` and
     `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)` both round-trip
     symbolic bool tokens.
   - `glLightModeli` expression-fallback compatibility (write *before*
     the refactor): `GL_TRUE`, `1`, `0`, a non-table integer, and a
     var expression in slot 2 all behave as today, and slot 2 emits
     the typed source token verbatim (not canonicalized).
   - End-of-input autocomplete works for all enum slots, including
     `glColorMask` slot 4.
   - `expected_commands[]` and the all-commands export test include
     `CMD_COLOR_MASK`.
   - Contract test: enum-spec rows fill `args[]` uniformly and set
     `num_args`.
   - If numeric bool fallback is intentionally added, test
     `glColorMask(1, 0, 1, 0)` canonicalizes to symbolic tokens. If it
     is intentionally rejected, test the rejection message instead.
9. Standing guard: add a `check-*` target to the
   `make check-state-ownership` aggregator enforcing the
   no-`GLCmd.mode`-for-enum-spec invariant (delete the field outright
   if the custom consumers are migrated; else the allowlisted grep —
   see "Migration safety").
10. Docs: CLAUDE.md supported-commands list. Note that enum commands
   use one uniform `args[]` storage convention.
11. Gate: `make test`, `make test-stubs`,
   `make sample USE_GL_STUBS=1`, `make sample`,
   `make check-state-ownership`.

## Folder note

`plans/in-review/` = decision pending. The *direction* (fork C) is
chosen, but it stays here rather than `not-started/` because C is a
pre-req enum-path refactor with the blast radius documented above —
it should not be picked up casually. Lifecycle from here: in-review →
`not-started/` (when scheduled) → `active/` → `done/`. The prototype
was rolled back, so this is a **from-scratch** implementation per "If
approved", not a quick finish.
