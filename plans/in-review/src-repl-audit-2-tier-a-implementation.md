# `src/repl/` Audit-2 — Tier A Implementation Plan

Status: **in-review** — concrete commit sequence for the Tier A
findings from `plans/in-review/src-repl-code-smell-audit-2.md`
(2026-05-26). Each batch lists files, target diffs, and a verify
gate. No Tier B/C items here; those wait for their own plans.

## Scope

This plan closes the 22 Tier A findings from the audit. The list
splits cleanly into four batches, each independently committable
and build-safe:

| Batch | Findings | Net LOC | Risk |
|---|---|---|---|
| B1 — Real bugs | #3, #9, #11, #13 | +30 / -10 | low (each is a tight correctness fix) |
| B2 — Dead-code sweep | #53–#67 | +20 / -350 | near-zero (every deletion verified zero callers) |
| B3 — Naming + duplication | #34, #38, #39, #40, #52 | +40 / -80 | low (mechanical) |
| B4 — apply.c hardening | #46 | +20 / -10 | low (one helper signature change) |

**Out of scope** (deferred to a separate plan):
- #1 (help_text.c layering breach) — needs a design call between
  (a) move the file to `src/app/` or (b) inject F-key data via
  callback. Tier B sized.
- #2 (parse_cfg double-apply) — needs a decision on which path is
  the source of truth. Tier B sized.
- #5 (UserScene stack pressure) — real overflow risk but the fix
  (heap allocate vs. static scratch) is a design call worth
  isolating. Tier B sized.
- #14 (`_mut()` regression across 5 files + ratchet) — Tier C; the
  cross-file sweep + `check-state-ownership` script is the largest
  single piece of leftover work.
- All structural items #68–#82 — Tier B/C.

## Verify gates

After every batch:

```
make check-c99       # syntax-only ratchet (real gcc -std=c99)
make test-stubs      # 46 binaries, 7079 tests last green
```

The plan assumes you're on a fresh feature branch off `main`. If
any gate fails after a batch, revert that batch's commits and
re-plan; do not paper over with follow-up fixes inside the same
batch.

---

## Batch 1 — Real bugs (#3, #9, #11, #13)

Four tight correctness fixes. Each is its own commit so the bug
fix shows up in `git log` independently of cosmetic sweep.

### Commit B1.1 — #3 predef-values count bug

**File:** `src/repl/eval.c:233-253`, `src/repl/eval.h` (signatures),
all callers of `repl_copy_predef_values` / `repl_restore_predef_values`

**Current shape:**
```c
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals);
```
Both compute `n = min(g_num_predef_vars, max_vals)` independently —
restore uses the live count at restore time, not the count captured
at copy.

**Target shape:** Have copy *return* the count it captured; have
restore accept that count explicitly:

```c
int  repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals,
                                int saved_count);
```

**Callers to update** (from prior grep):
- `src/app/glr_ctrl.c:1785` + `:1894` — frame baseline save/restore pair
- `src/app/glr_ctrl.c:223` — fade-plan replay path
- `src/subsystems/replay/replay.c:762, 766, 812, 816, 826, 958, 1151`

Each save site already saves into a stack `float[MAX_PREDEF_VARS]`;
add `int saved_n = repl_copy_predef_values(buf, MAX_PREDEF_VARS);`
and pass `saved_n` to the matching restore.

**Verify:** add a unit test in `tests/test_repl_executor.c` that
declares 3 vars, copies, removes one (undeclare), restores, asserts
the third var's value survived. The current code would write only
2 floats back.

### Commit B1.2 — #9 load-side `fclose` / `ferror` check

**File:** `src/repl/export.c:3157-3158` (and the surrounding
`fgets` loop in `repl_export_load_from_file`)

**Current shape:**
```c
while (fgets(line, sizeof(line), f)) { ... }
if (fclose(f) != 0)
    return 0;
```
`fclose` failure is silent; `ferror` never checked.

**Target shape:** Mirror the save-side pattern (already in this
file post-prior-audit #6):
```c
while (fgets(line, sizeof(line), f)) { ... }
int had_read_err = ferror(f);
int close_failed = (fclose(f) != 0);
if (had_read_err || close_failed) {
    char msg[REPL_STATUS_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Error: cannot read %s", path);
    repl_set_status_error(msg);
    return 0;
}
```

**Verify:** no new test; manual sanity (open a workspace file,
verify no regression). The fix surfaces an error path that
previously was silent — there's nothing to assert "old behavior"
about.

### Commit B1.3 — #11 source_scope buf_sz≤0 guard

**File:** `src/repl/source_scope.c:92-104` (`cmd_indent`) and
`:145-156` (`cmd_tess_indent`)

**Current shape:** Both functions skip the guard that their two
siblings (`begin_indent` L111, `tess_close_indent` L122) have. With
`buf_sz == 0`, the `spaces` clamp lands at 0 and `buf[0] = '\0'`
writes one byte past the zero-length buffer.

**Target shape:** Add `if (buf_sz <= 0) return;` at the top of
both, matching the siblings.

**Verify:** unit test in `tests/test_repl_core_extra.c` (or a new
slot) calling each with `buf_sz = 0` — should be a no-op with no
write.

### Commit B1.4 — #13 `repl_eval_parse_exprs` doc-fix

**File:** `src/repl/eval.h:267-271`

**Current shape:** Header says `"Returns the number of expressions
parsed (up to max), -1 on error."` — implementation only returns
`n` (loop break on no progress). Callers that check for `-1` treat
malformed lists as "zero args, success."

**Target shape:** Drop the `-1` clause from the doc. This is the
safer of the two fixes (no behavior change). If a future caller
actually needs the error signal, they can switch to a different
parser entry that does provide it.

```c
/* Returns the number of expressions parsed (up to max). Stops on
 * malformed input; the caller distinguishes "stopped short" by
 * comparing against the expected count. */
int repl_eval_parse_exprs(const char *src, float *out_vals, int max,
                          const ExprVar *vars, int num_vars);
```

**Verify:** no test change needed; this is a pure doc fix.
Recompile sanity-check is enough.

---

## Batch 2 — Dead-code sweep (#53–#67)

Mechanical removals. Bundle these as ONE commit per cluster so the
sweep is auditable without paper-cut commits. The big one is #53
(~200 LOC out of `format.c` / `format.h`).

### Commit B2.1 — #53 delete dead `repl_format_*` functions

**File:** `src/repl/format.c:9-82`, `src/repl/format.h:1-121`,
`tests/test_format.c` (delete the corresponding test cases)

**Current shape:** Seven `repl_format_*` functions with zero
production callers. The only live one is
`repl_format_reindent_from_parsed` (called once by `core.c:198`).
Plus `ReplFmtCmd` / `ReplFmtType` types exist only for the dead
formatters.

**Delete:**
- `repl_format_tess_depth`
- `repl_format_begin_depth`
- `repl_format_indent`
- `repl_format_end_indent`
- `repl_format_tess_end_indent`
- `repl_format_tess_leaf_indent`
- `repl_format_reindent_expr`
- `ReplFmtCmd`, `ReplFmtType` (if unused after the above)
- All `tests/test_format.c` cases that exercise the deleted functions

**Keep:** `repl_format_reindent_from_parsed` (and any helpers it
genuinely needs).

**Verify:** `make test-stubs` — `test_format` will shrink but
should still pass for the one remaining function. Confirm no other
binary referenced the deleted symbols (linker would have caught it
otherwise, but a `grep -rn "repl_format_tess_depth\|repl_format_begin_depth\|..."`
sweep before deleting catches stragglers).

### Commit B2.2 — Dead parameters and one-line forwarders

Mechanical fixes, one file each (or grouped if they're in adjacent
sections):

- **#54** — `src/repl/compile.c:184-187` — delete
  `compile_scope_indent` wrapper; inline the one call site at L977.
- **#55** — `src/repl/autonormal.c:36-50` — delete `insert_pos`
  parameter from `make_auto_normal`; both callers (L261, L276) drop
  the discarded arg.
- **#56** — `src/repl/flatten.c:58-70` — delete `cmd` parameter
  from `flatten_get_for_var_name`; single caller at L211 simplifies.
- **#57** — `src/repl/executor.c:28-30, h:112` — delete the
  `repl_executor_camera_distance_source` getter (test-only seam).
  Update `tests/test_repl_executor.c` to use round-trip via the
  setter pointer.
- **#58** — `src/repl/eval.c:1002-1005` — delete dead `args[1] =
  0.0f` reset and the surrounding conditional.
- **#59** — `src/repl/flatten.c:237-245` — collapse the always-true
  `if (lnv < MAX_EXPR_VARS)` guard.
- **#61** — `src/repl/export.c:1878` — delete dead `(void)warnings;`
- **#62** — `src/repl/export.c:2884-2891` — drop `!section->enabled
  ||` half of condition; drop the `export_section_always` thunk.
- **#63** — `src/repl/export.c:1289-1298` — delete `int off =` and
  `+=` operators in `write_canonical_cmd_as_c`.
- **#64** — `src/repl/replay_annotations.c:57-60, 680, 723` — drop
  `void *ctx` from `ReplayBeforeStepFn` typedef and signature; the
  one caller passing NULL collapses.
- **#65** — `src/repl/replay_annotations.c:209` — delete redundant
  `len = (int)(p - start);`.

**Verify:** `make check-c99 && make test-stubs`. Each removal is
verified-no-callers by the build linker; tests should be unchanged.

### Commit B2.3 — #60 surface `vis_total` from `repl_load_apply_line`

**File:** `src/repl/load.c:177-179`

**Current shape:** `int vis_total = 0;` populated but never read;
truncation is silent in import. The interactive editor surfaces
the same condition via `warn_if_scope_truncated`.

**Target shape:** Emit the warning through the `err` buffer when
`vis_total > MAX_EXPR_VARS`. Match the editor's wording so the
diagnostic reads consistently.

```c
int vis_total = 0;
int num_vis_vars = collect_visible_vars(insert_idx, vis_vars,
                                        MAX_EXPR_VARS, &vis_total);
if (vis_total > MAX_EXPR_VARS && err && err_sz > 0)
    snprintf(err, err_sz, "scope truncated: %d vars visible, only "
                          "%d used", vis_total, num_vis_vars);
```

**Verify:** sanity-check that a workspace with > MAX_EXPR_VARS
visible variables produces a non-empty `load_err` (a unit test in
`test_repl_core_io.c` if there's a convenient way to set this up;
otherwise leave it to a manual test note).

### Commit B2.4 — #66 delete 13 unused `state.c` macros

**File:** `src/repl/state.c:150-168`

**Current shape:** 13 of 25 file-scope macros at lines 103-168
have zero readers — leftovers from the pre-`ReplRuntimeState`
era.

**Delete** (verify each with `grep -c '\bg_<name>\b' src/repl/state.c`
returning 1; if > 1, leave it):
- `g_workspace_dir`
- `g_workspace_header_lines`
- ... (sweep the L150-168 block)

**Keep** the 12 macros that actually carry the body of the file —
they aid readability of the implementation functions.

**Verify:** `make check-c99`. If a macro was inadvertently used
inside a function body, the compile fails; restore it.

### Commit B2.5 — #67 phase-N.M comment sweep

**Files:**
- `src/repl/parser.c:29-30, 889`
- `src/repl/parser.h:38, 62-65`
- `src/repl/compile.c:171-173`
- `src/repl/compile.h:79, 214-217`
- `src/repl/executor.c:177-178`
- `src/repl/pipeline.h:19`
- `src/repl/eval.h:42-88`

**Current shape:** "phase H.5 commit 40", "β forbids …", "phase
3.6.3" — construction-history references. Some reference deleted
modules (`eval.h` mentions `editor_undo.h`, `replay.c / glr_ctrl.c`,
`FlatScope`).

**Target shape:** Drop phase coordinates. Keep policy statements
("REPL pipeline does not reach into editor_state_*") where they
add value. In `eval.h:42-88`, also (a) swap the mislabeled
`MAX_EXPR_VARS`/`MAX_PREDEF_VARS` (currently the heading says the
former while the body describes the latter — separate finding #70
in the audit) and (b) update the three "Used in" paths.

**Verify:** `make check-c99`. Pure comment changes.

---

## Batch 3 — Naming + duplication (#34, #38, #39, #40, #52)

Mechanical convergence on canonical names + one-shot helper
extractions.

### Commit B3.1 — #34 collapse workspace_dir name pair

**Files:** `src/repl/state.c:478-484`, `src/repl/scenes.c:1016-1023`,
all callers

**Current shape:** `repl_workspace_dir` (scenes.c) and
`repl_state_workspace_dir` (state.c) are two names for the same
getter; same for the setter pair.

**Target shape:** Delete the state.c shims; rewrite the 4 callers
(`src/repl/export.c:273` + 3 tests) to call the scenes.c names
directly.

```bash
# verify no other consumers
grep -rn "repl_state_workspace_dir\|repl_state_workspace_set_dir" src/ tests/
```

**Verify:** `make test-stubs`. Caller-rename only.

### Commit B3.2 — #38 collapse examples API name pair

**Files:** `src/repl/examples.h:48-57`, `src/repl/example_loader.c:503-513`,
all callers

**Current shape:** Plural `repl_examples_*` in the header; singular
`repl_example_*` trampolines in example_loader.c (plus the tag/
subheading API which is already singular).

**Target shape:** Pick singular (matches tag/subheading already).
Rename the plural exports; delete the singular trampolines.

Callers split between bench/ (plural) and tests + glr_ctrl
(singular). Sweep both.

**Verify:** `make check-c99 && make test-stubs`. Sweep needs to
catch both `repl_examples_count` and `repl_example_count` etc.

### Commit B3.3 — #39 collapse the three `?:` cfg-get ternaries

**File:** `src/repl/export.c:873-878, 1454-1457, 2678-2683`

**Current shape:** Three open-coded `g_export_cfg_bridge &&
g_export_cfg_bridge->get_int ? ... : fallback` expressions.

**Target shape:** Each becomes `repl_cfg_get_int(slug, default)`
(already exists in `cfg_baseline.c`).

**Verify:** Confirm `repl_cfg_get_int` has the same semantics
(bridge → fallback) as the inline ternaries. Then sweep + test.

### Commit B3.4 — #40 funnel predef-var capture through `repl_eval_*`

**File:** `src/repl/scenes.c:208-213, 294-299, 393-398, 433-438,
458-463`

**Current shape:** Five sites manually loop:
```c
for (int i = 0; i < N; i++) {
    dst->predef_vals[i] = g_predef_vars[i].value;
    memcpy(dst->predef_names[i], g_predef_vars[i].name, sizeof(...));
}
```

**Target shape:** Call `repl_eval_copy_predef_vars()` /
`_restore_predef_vars()` (already exist in `eval.h`). The
function signatures should match; if not, add a small wrapper
`repl_eval_snapshot_for_userscene(UserScene *)` rather than
expanding the eval API.

**Verify:** Scene save/load round-trip tests
(`test_repl_core_io.c`) cover this.

### Commit B3.5 — #52 + #46 unify struct terminators + add `declare_with_value`

**Files:**
- `src/repl/command_spec.c:384, 407` (terminators)
- `src/repl/apply.c:144-149` (DECLARE+SET_VALUE redundancy)
- `src/repl/eval.h` + `src/repl/eval.c` (new helper)

**#52 — terminators:** Make both end-of-table terminators use the
same convention. `{ NULL }` (C99 zero-fill) is the cleanest:

```c
{ NULL }      /* end-of-table */
```

**#46 — `declare_with_value`:** Today apply.c does
`declare → O(n) name lookup → patch`. Add to eval.c:

```c
int repl_eval_declare_predef_var_with_value(const char *name,
                                             float value,
                                             char *err, int errsz);
```

Return the new slot index; apply.c uses it to write
`g_predef_vars[idx].value` directly. Existing
`repl_eval_declare_predef_var` stays (other callers don't need the
value). Document the new helper as "DECLARE + write initial value
atomically; intended for `repl_apply_predef_ops`."

**Verify:** `tests/test_repl_compile.c` already exercises the
DECLARE + SET_VALUE flow; should still pass without changes.

---

## Batch 4 — apply.c hardening (#46 — folded into B3.5 above)

(Covered by Commit B3.5; no separate batch needed.)

---

## Skipped Tier A items

These were originally listed as Tier A in the audit but on closer
look they need a Tier B-sized treatment:

- **#1 layering breach in help_text.c** — needs a design call.
  Defer to a dedicated plan.
- **#2 cfg double-apply** — semantics question (which path wins).
  Defer.
- **#5 UserScene stack pressure** — fix is small but the design
  call (heap vs. static scratch) deserves isolation.
- **#10 flatten_source_lighting_enabled** — needs to walk the flat
  program; touches flatten contract enough that it's Tier B.

---

## Post-batch hygiene

After all four batches land:

1. **Update the audit doc.** Mark the closed findings in
   `plans/in-review/src-repl-code-smell-audit-2.md`:
   - For each closed finding, suffix the heading with
     `(done — Tier A closed YYYY-MM-DD; commit SHA)`.
   - Update the "Sequencing → One-afternoon pass" section's
     checklist.
   - Update any "Bottom line" / "Headline take" counts.
2. **Decide Tier B kickoff.** Choose between:
   - The cross-cutting `_mut()` sweep + `check-state-ownership`
     ratchet (#14) — high leverage, one focused PR.
   - The `editor_compile_* / repl_compile_*` parse-core extraction
     (#15) — closes the largest remaining drift surface.
   - The `export.c` save/load TU split (#69) — biggest single LOC
     win.
   Open a follow-up plan for the chosen one.

## Expected outcome

After this plan completes (4 batches, ~9 commits, ~400 LOC net
reduction):

- 22 Tier A findings closed
- 4 real correctness bugs fixed (#3, #9, #11, #13)
- ~200 LOC of dead `format.c` removed
- 13 dead state.c macros removed
- 5 sites of duplicated predef-var loops funneled through one
  helper
- 2 name pairs collapsed to one canonical form each
- One audit-document update reflecting the Tier A closeout

The bigger Tier B work (cross-file `_mut()` sweep, helper
extractions, file splits) is left intact for its own plan.

## Time estimate

- Batch 1: ~1 hour (each bug is tight, but #3 has 7 callers to
  rewrite)
- Batch 2: ~1 hour (mostly delete + verify)
- Batch 3: ~1.5 hours (B3.2 and B3.4 each touch multiple files)
- Batch 4: covered in B3.5

Total: ~3-4 hours of focused work; ~9 commits.

## Verification matrix

| Gate | When | Expected |
|---|---|---|
| `make check-c99` | After every commit | OK |
| `make test-stubs` | After every commit | 46/46 binaries, ≥7079 tests (may grow if B1.1 adds a test) |
| `ssh gracemont ...` | Once at the end | check-c99 + test-stubs both green |
| Manual sanity | After batch 1 | start `./gl-repl`, type a few cmds, replay, verify nothing visually regressed |

## Rollback

Each batch is its own commit (or commit group) on a feature branch.
Roll back with `git revert <sha>` per batch. The bugs in batch 1 are
unrelated to each other; reverting any one doesn't compromise the
others.
