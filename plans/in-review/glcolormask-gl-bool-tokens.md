# glColorMask: preserve GL_TRUE/GL_FALSE in source — scope pending

Status: **in-review** — `glColorMask` is **not implemented**. A full
prototype (incl. a std-path special-case) was built and then **rolled
back** at the user's request; only the unrelated alphabetical sort of
`command_spec.c` was kept. This file captures the design so the work
can be picked up later. The open question is *where glColorMask should
live* so its 4 boolean args render back as `GL_TRUE`/`GL_FALSE` instead
of `1`/`0`. Do not implement until a fork is chosen and the file moves
to `not-started/`.

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

### A. Std path + post-parse text special-case — *recommended*

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

### C. Generalize the enum path to N args (uniform `args[]`)

The genuinely clean home: one N-arg branch, uniform storage.

- Pro: removes the mode/args[0] divergence wart; correct long-term
  model.
- Con: largest blast radius — changes the storage contract and
  executor reads for **every** existing enum command (glEnable,
  glDisable, glShadeModel, glFrontFace, glDepthMask, glColorMaterial,
  glBlendFunc, glLightModeli, glBegin) for the benefit of one new
  command. High regression surface; not justified now.

## Recommendation

**Fork A.** Given the enum path's non-generalized, divergent-storage
reality, the std-path special-case is both the smallest and the
cleaner-by-consistency choice (reuses an existing idiom vs. inventing a
third enum-storage variant). Revisit C only if a second multi-bool GL
command (or a broader enum-arg cleanup) lands and makes the
generalization pay for itself.

`glDepthMask` stays on the enum path either way — its `num_args==1`
branch already echoes the token; moving it would regress its saved
text to `1/0`.

## If approved (implement A from scratch)

1. `command.h`: add `CMD_COLOR_MASK`.
2. `command_spec.c`: std-spec row (numeric `fmt`, used as the
   pre-override generic write — same as `glClearColor`), func-
   completion entry, `g_command_type_specs`
   `CMD_TYPE_SPEC_NOT_IN_BEGIN(..., CMD_CAT_STATE)`. Insert in the
   already-alphabetized slots.
3. `eval.c`: `GL_TRUE`/`GL_FALSE` evaluator constants — 4 sites
   mirroring `PI`/`TAU` (value resolution; the two ident-validation
   scans; reserved-ident list). No C round-trip map entry.
4. `executor.c`: `apply_state_cmd` case
   `glColorMask((GLboolean)args[0..3])` + group it with the other
   `apply_state_cmd`-routed state commands in the main dispatch.
5. `parser.c`: begin-error name; the post-parse
   `if (def->type == CMD_COLOR_MASK)` block that rebuilds `text_out`
   as `glColorMask(GL_TRUE|GL_FALSE × 4)` from `args[i] != 0` —
   placed right after the `glClearColor` special-case, with a *why*
   comment (readability/illusion).
6. Gate: `make test`, `make test-stubs`,
   `make sample USE_GL_STUBS=1`, `make sample`,
   `make check-state-ownership`.
7. Tests: feed `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);` in
   `test_repl_export_all_commands` (+ `CMD_COLOR_MASK` in
   `expected_commands[]`); assert the canonical line round-trips with
   `GL_TRUE`/`GL_FALSE`, not `1`/`0` (guards the numeric-`fmt`
   regression). Add a focused unit: `glColorMask(1, 0, 1, 0)` and the
   symbolic form both normalize to the symbolic text; `x = GL_TRUE;`
   evaluates to `1`.
8. Docs: CLAUDE.md supported-commands list.

## Folder note

`plans/in-review/` = decision pending. Lifecycle: in-review →
(decision) → not-started → active → done, or deleted if rejected.
The prototype was rolled back, so an approve-A decision means
**implement from scratch** per "If approved" (move to `not-started/`
→ `active/`), not a quick finish.
