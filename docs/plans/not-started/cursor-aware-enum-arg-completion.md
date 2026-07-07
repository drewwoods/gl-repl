# Cursor-aware enum-arg autocomplete (complete a prior argument) — scope pending

Status: **in-review** — the enum-matching change is small and low-risk,
but it sits behind a system-wide "completion only appends at end of
input" assumption. How far to relax that assumption is a scope fork not
yet decided. Do not implement until a direction is chosen and the file
moves to `not-started/`.

## 2026-05-23 audit

- **Cross-plan dependency landed.** `glcolormask-gl-bool-tokens.md`
  shipped (it now sits in `plans/done/`). `src/app/glr_completion.c`
  uses the unified `AC_MODE_ENUM_SLOT` (line 25) and pulls candidates
  from `def->args[slot].enums` (line 356). The legacy
  `enums1`/`enums2` / `AC_MODE_ENUM_ARG1/2` shape this plan cites is
  gone — re-derive the file/line references before implementing.
- **The cursor-aware piece is NOT done.** The end-of-input guard
  (`if (editor_cursor_pos() != raw_input_len) return;`) still sits at
  `src/app/glr_completion.c:260` — mid-line completion remains
  impossible. No `cursor_arg_slot` or `tail_is_only_trailing_args`
  helper exists.

No decision recorded on which scope fork to pursue; stays in `in-review/`.

Dependency note: this is **separate** from
`plans/done/glcolormask-gl-bool-tokens.md`. The glColorMask Path C
work must make ordinary end-of-input enum completion N-slot aware as
part of the enum-spec refactor. This plan owns the larger cursor-aware
mid-line behavior: relaxing the end-of-input guard, computing the slot
at the cursor, and splicing accepted completion text into the input
buffer. **Cross-plan staleness (bidirectional):** both plans cite
`glr_completion.c` by line number against the current
`enums1`/`enums2` + `AC_MODE_ENUM_ARG1/2` shape. Whichever lands first
invalidates the other's citations — if Path C lands first, re-derive
this plan's `glr_completion.c` references and terminology
(`enums1`/`enums2` → `args[slot].enums`) from the then-current code;
do not trust the line numbers here.

## Context

Ask: complete an earlier argument while a later one is already typed —
e.g. cursor parked after `GL_FR` in

```
glColorMaterial(GL_FR, GL_DIFFUSE);
```

should offer `GL_FRONT` for arg1, even though arg2 (`GL_DIFFUSE`) exists.

Today this is impossible, blocked at three layers in
`src/app/glr_completion.c`:

1. **End-of-input guard (`:258`).**
   `if (editor_cursor_pos() != raw_input_len) return;` — completion
   bails before looking at the token whenever the cursor isn't at the
   very end of the line. Not enum-specific: *no* mid-line completion
   exists anywhere. Everything downstream assumes append-at-end.

2. **arg1/arg2 selector is comma-presence, not cursor-position
   (`:327`).** `char *comma = strchr(after, ',');` → any comma forces
   arg2 mode. With `GL_DIFFUSE` present a comma exists, so arg1 can
   never be completed even past the guard.

3. **Accept/ghost mechanics are pure append (`:209`, `:404`).**
   Preview ghost = `match + g_ac_token_len` drawn after the cursor;
   accept = `strcat(inp->input, ac.ghost)`. Finishing `GL_FR` →
   `GL_FRONT` mid-line needs an **insert/splice** at the cursor, not a
   tail strcat. `g_ac_token_len` / `g_ac_suffix` are end-anchored, not
   cursor-relative.

The arg-slot detection itself is cheap — the depth-tracking comma scan
already exists in `build_param_hint_text` (`:57-73`) and can be reused
to find which slot the cursor is in.

## Effort

| Piece | Effort |
|---|---|
| Cursor-aware arg-slot detection (count `depth==0` commas before cursor offset; extract token under cursor) | Easy — mirrors `build_param_hint_text` |
| Pick the positional enum table from the slot index | Trivial |
| Relax `:258` guard to "cursor at end of *current token*" not "end of input" | Moderate — must not regress POINT_PARAM / FUNC_PREFIX / ENUM modes |
| Cursor-relative accept + ghost rendered at cursor (mid-buffer splice) | Moderate — touches the editor input buffer and the active-input ghost renderer (assumes draw-after-cursor) |

Overall **moderate, ≈ half a day** for the scoped option; **≈ 1–1.5
days** for the fully general mid-line splice. The enum table matching is
a few lines; the cost is entirely the three end-anchored assumptions.

## Scope forks

### A. How far to generalize mid-line completion

- **Full mid-line splice.** Completion works at any cursor position;
  `g_ac_token_len`/`g_ac_suffix` become a cursor-relative span; accept
  splices the suffix at the cursor; ghost renders at an arbitrary
  interior position. Pro: solves the general case (any arg, any
  command, also helps FUNC_PREFIX edits). Con: largest blast radius —
  changes the accept path and the active-input ghost renderer for *all*
  modes; highest regression surface. — *not recommended as a first
  step.*
- **Token-tail-only subset.** Only fire when the cursor is at the end
  of the *token being completed* and everything after it is trailing
  `, <args>)[;]`. Still needs a cursor-relative accept (insert remaining
  suffix at cursor, keep the trailing text), but ghost rendering stays
  "draw at cursor, text already follows" — no general interior-render
  rework. Pro: delivers the exact ask with the smallest change; the
  guard relaxes to "cursor at end of current token". Con: doesn't help
  truly arbitrary mid-line edits. — *recommended.*

### B. Which commands

- **Enum commands only** (`repl_enum_command_specs()` +
  `glPointParameterfv`). The original ask. — *recommended.*
- Also FUNC_PREFIX / point-param coordinate completion mid-line. Larger;
  defer until the enum case proves the pattern.

### C. Token boundary when cursor is mid-token

- Require cursor at token end (subset A). Simple, predictable.
- Complete from cursor splitting the token (prefix before cursor,
  discard/keep suffix after). More editor-ish but ambiguous UX; defer.

## Recommendation

Build the **token-tail-only subset (fork A) for enum commands only
(fork B), cursor-at-token-end (fork C)**. This is the minimal change
that delivers the literal ask. Concretely:

1. Replace the hard `:258` guard with: cursor at end of input **OR**
   cursor at the end of the current token *and* the remainder of the
   line is only `[ws] , … )[;][ws]`. Other modes keep their existing
   end-of-input behavior (gate the new path explicitly).
2. In the enum-command block, compute `slot` = number of `depth==0`
   commas in `after` *before the cursor offset* (reuse the
   `build_param_hint_text` scan). Pick the enum table for that slot
   (`args[slot].enums` after the glColorMask enum-spec refactor;
   `enums1` / `enums2` only if this lands before that refactor).
   Extract the token under the cursor as the match prefix instead of
   "text after `(`" / "text after first comma".
3. Make accept cursor-relative: insert `match_suffix` at
   `editor_cursor_pos()` (buffer splice) instead of
   `strcat(inp->input, ac.ghost)`. Ghost preview already renders at the
   cursor; with the subset constraint the trailing text is untouched.

Keep the fully-general splice and FUNC_PREFIX/point-param mid-line as
explicitly out of scope, revisitable once this lands.

## If approved (sketch)

- `src/app/glr_completion.c`:
  - New helper `cursor_arg_slot(const char *after, int cursor_off)` —
    depth-aware comma count before the cursor (extracted/shared with
    `build_param_hint_text`'s scan so the two can't drift).
  - New predicate `tail_is_only_trailing_args(const char *p)` — accepts
    `[ws] (, … )* )` `[;]` `[ws]` to gate the relaxed guard.
  - Rework the enum block (`:318-376`) to branch on `slot` not
    `strchr(',')`; prefix = token under cursor; table =
    positional enum table for `slot`.
  - `accept_autocomplete()` (`:398`): splice at cursor when
    `g_ac_mode` is an enum mode and an interior-completion flag is set;
    keep strcat for the end-of-input path.
  - `g_ac_token_len` stays the already-typed length of the token under
    the cursor; document it's now cursor-relative for enum modes.
- Editor input buffer: confirm a splice/insert primitive exists
  (`editor_state_input_mut()` + memmove) or add one; the active-input
  ghost renderer already draws ghost at the cursor — verify it does not
  assume cursor==end (read-only check, likely fine for the subset).
- Tests: `tests/` pure unit for `cursor_arg_slot` (offsets → slot,
  nested-paren args like `f(cos(i+phase), …)` don't miscount) and
  `tail_is_only_trailing_args`; an autocomplete-state test driving
  `update_autocomplete` with a mid-line cursor asserting arg1 matches
  with arg2 present. Mirror existing `glr_completion` test style.
- Verify `make test`, `make test-stubs`, `make sample USE_GL_STUBS=1`,
  `make sample`; UI/editor boundary guards
  (`make check-state-ownership`).
- Docs: CLAUDE.md "Autocomplete" section — note enum modes now complete
  a prior arg when the cursor is at a token end with only trailing args
  after it.

## Folder note

`plans/in-review/` = contested-direction / decision pending. Lifecycle:
in-review → (decision) → not-started → active → done, or deleted if
rejected.
